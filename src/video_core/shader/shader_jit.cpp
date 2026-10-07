// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/arch.h"
#if CITRA_ARCH(x86_64) || CITRA_ARCH(arm64)

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include "common/assert.h"
#include "common/file_util.h"
#include "common/hash.h"
#include "common/logging/log.h"
#include "common/microprofile.h"
#include "video_core/shader/shader.h"
#include "video_core/shader/shader_jit.h"
#if CITRA_ARCH(arm64)
#include "video_core/shader/shader_jit_a64_compiler.h"
#endif
#if CITRA_ARCH(x86_64)
#include "video_core/shader/shader_jit_x64_compiler.h"
#endif

namespace Pica::Shader {

namespace {
constexpr std::array<char, 8> PersistentCacheMagic{'A', 'Z', 'P', 'I', 'C', 'A', 'J', 'T'};
constexpr u32 PersistentCacheVersion = 1;
constexpr std::size_t PersistentCacheMaximumShaders = 128;

struct PersistentCacheHeader {
    std::array<char, 8> magic;
    u32 version;
    u32 record_count;
    u64 program_id;
    u64 checksum;
};

struct PersistentRecordHeader {
    u64 cache_key;
    u32 program_size;
    u32 swizzle_size;
};

void HashBytes(u64& hash, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const u8*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ULL;
    }
}
} // namespace

struct JitEngine::PersistentShader {
    u64 cache_key{};
    u32 program_size{};
    u32 swizzle_size{};
    ProgramCode program_code{};
    SwizzleData swizzle_data{};
};

JitEngine::JitEngine() = default;
JitEngine::~JitEngine() {
    FlushPersistentCache();
}

void JitEngine::ConfigurePersistentCache(u64 program_id) {
    persistent_program_id = program_id;
    const std::string directory = FileUtil::GetUserPath(FileUtil::UserPath::CacheDir) + "pica_jit/";
    FileUtil::CreateFullPath(directory);
    persistent_cache_path =
        fmt::format("{}{:016X}-pica-source-v{}.bin", directory, program_id, PersistentCacheVersion);
    LoadPersistentCache();
}

void JitEngine::LoadPersistentCache() {
    std::FILE* file = std::fopen(persistent_cache_path.c_str(), "rb");
    if (!file) {
        return;
    }

    PersistentCacheHeader header{};
    if (std::fread(&header, sizeof(header), 1, file) != 1 || header.magic != PersistentCacheMagic ||
        header.version != PersistentCacheVersion || header.program_id != persistent_program_id ||
        header.record_count > PersistentCacheMaximumShaders) {
        std::fclose(file);
        return;
    }

    std::vector<PersistentShader> loaded;
    loaded.reserve(header.record_count);
    u64 checksum = 1469598103934665603ULL;
    HashBytes(checksum, &header.program_id, sizeof(header.program_id));
    bool valid = true;
    for (u32 i = 0; i < header.record_count; ++i) {
        PersistentRecordHeader record_header{};
        if (std::fread(&record_header, sizeof(record_header), 1, file) != 1 ||
            record_header.program_size > MAX_PROGRAM_CODE_LENGTH ||
            record_header.swizzle_size > MAX_SWIZZLE_DATA_LENGTH) {
            valid = false;
            break;
        }

        PersistentShader record{};
        record.cache_key = record_header.cache_key;
        record.program_size = record_header.program_size;
        record.swizzle_size = record_header.swizzle_size;
        const bool read_program = record.program_size == 0 ||
                                  std::fread(record.program_code.data(), sizeof(u32),
                                             record.program_size, file) == record.program_size;
        const bool read_swizzle = record.swizzle_size == 0 ||
                                  std::fread(record.swizzle_data.data(), sizeof(u32),
                                             record.swizzle_size, file) == record.swizzle_size;
        if (!read_program || !read_swizzle) {
            valid = false;
            break;
        }

        const u64 code_hash =
            Common::ComputeHash64(record.program_code.data(), record.program_size * sizeof(u32));
        const u64 swizzle_hash =
            Common::ComputeHash64(record.swizzle_data.data(), record.swizzle_size * sizeof(u32));
        if (record.cache_key != Common::HashCombine(code_hash, swizzle_hash)) {
            valid = false;
            break;
        }
        HashBytes(checksum, &record_header, sizeof(record_header));
        HashBytes(checksum, record.program_code.data(), record.program_size * sizeof(u32));
        HashBytes(checksum, record.swizzle_data.data(), record.swizzle_size * sizeof(u32));
        loaded.emplace_back(std::move(record));
    }
    std::fclose(file);
    if (!valid || checksum != header.checksum) {
        LOG_WARNING(HW_GPU, "Ignoring invalid persistent PICA JIT cache {}", persistent_cache_path);
        return;
    }

    const auto started = std::chrono::steady_clock::now();
    for (const auto& record : loaded) {
        if (cache.contains(record.cache_key)) {
            continue;
        }
        auto shader = std::make_unique<JitShader>();
        shader->Compile(&record.program_code, &record.swizzle_data);
        cache.emplace(record.cache_key, std::move(shader));
    }
    persistent_shaders = std::move(loaded);
    const double elapsed_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
            .count();
    LOG_INFO(HW_GPU, "Precompiled {} persistent PICA shaders for {:016X} in {:.1f} ms",
             persistent_shaders.size(), persistent_program_id, elapsed_ms);
}

void JitEngine::FlushPersistentCache() {
    if (!persistent_cache_dirty || persistent_cache_path.empty() || persistent_shaders.empty()) {
        return;
    }

    u64 checksum = 1469598103934665603ULL;
    HashBytes(checksum, &persistent_program_id, sizeof(persistent_program_id));
    for (const auto& record : persistent_shaders) {
        const PersistentRecordHeader record_header{record.cache_key, record.program_size,
                                                   record.swizzle_size};
        HashBytes(checksum, &record_header, sizeof(record_header));
        HashBytes(checksum, record.program_code.data(), record.program_size * sizeof(u32));
        HashBytes(checksum, record.swizzle_data.data(), record.swizzle_size * sizeof(u32));
    }

    const std::string temporary = persistent_cache_path + ".tmp";
    std::FILE* file = std::fopen(temporary.c_str(), "wb");
    if (!file) {
        LOG_ERROR(HW_GPU, "Failed to open persistent PICA JIT cache {}", temporary);
        return;
    }
    const PersistentCacheHeader header{PersistentCacheMagic, PersistentCacheVersion,
                                       static_cast<u32>(persistent_shaders.size()),
                                       persistent_program_id, checksum};
    bool wrote = std::fwrite(&header, sizeof(header), 1, file) == 1;
    for (const auto& record : persistent_shaders) {
        const PersistentRecordHeader record_header{record.cache_key, record.program_size,
                                                   record.swizzle_size};
        wrote = wrote && std::fwrite(&record_header, sizeof(record_header), 1, file) == 1;
        wrote = wrote && (record.program_size == 0 ||
                          std::fwrite(record.program_code.data(), sizeof(u32), record.program_size,
                                      file) == record.program_size);
        wrote = wrote && (record.swizzle_size == 0 ||
                          std::fwrite(record.swizzle_data.data(), sizeof(u32), record.swizzle_size,
                                      file) == record.swizzle_size);
    }
    const bool closed = std::fclose(file) == 0;
    if (wrote && closed && FileUtil::Rename(temporary, persistent_cache_path)) {
        persistent_cache_dirty = false;
        LOG_INFO(HW_GPU, "Saved {} persistent PICA shaders for {:016X}", persistent_shaders.size(),
                 persistent_program_id);
        return;
    }
    FileUtil::Delete(temporary);
    LOG_ERROR(HW_GPU, "Failed to save persistent PICA JIT cache {}", persistent_cache_path);
}

void JitEngine::SetupBatch(ShaderSetup& setup, u32 entry_point) {
    ASSERT(entry_point < MAX_PROGRAM_CODE_LENGTH);
    setup.entry_point = entry_point;

    setup.DoProgramCodeFixup();
    const u64 code_hash = setup.GetProgramCodeHash();
    const u64 swizzle_hash = setup.GetSwizzleDataHash();

    const u64 cache_key = Common::HashCombine(code_hash, swizzle_hash);
    auto iter = cache.find(cache_key);
    if (iter != cache.end()) {
        setup.cached_shader = iter->second.get();
    } else {
        auto shader = std::make_unique<JitShader>();
        shader->Compile(&setup.GetProgramCode(), &setup.GetSwizzleData());
        setup.cached_shader = shader.get();
        cache.emplace_hint(iter, cache_key, std::move(shader));

        if (persistent_program_id != 0 &&
            persistent_shaders.size() < PersistentCacheMaximumShaders) {
            const auto existing = std::find_if(persistent_shaders.begin(), persistent_shaders.end(),
                                               [cache_key](const PersistentShader& record) {
                                                   return record.cache_key == cache_key;
                                               });
            if (existing == persistent_shaders.end()) {
                PersistentShader record{};
                record.cache_key = cache_key;
                record.program_size = setup.GetBiggestProgramSize();
                record.swizzle_size = setup.GetBiggestSwizzleSize();
                std::copy_n(setup.GetProgramCode().begin(), record.program_size,
                            record.program_code.begin());
                std::copy_n(setup.GetSwizzleData().begin(), record.swizzle_size,
                            record.swizzle_data.begin());
                persistent_shaders.emplace_back(std::move(record));
                persistent_cache_dirty = true;
            }
        }
    }
}

MICROPROFILE_DECLARE(GPU_Shader);

void JitEngine::Run(const ShaderSetup& setup, ShaderUnit& state) const {
    ASSERT(setup.cached_shader != nullptr);

    MICROPROFILE_SCOPE(GPU_Shader);

    const JitShader* shader = static_cast<const JitShader*>(setup.cached_shader);
    shader->Run(setup, state, setup.entry_point);
}

} // namespace Pica::Shader

#endif // CITRA_ARCH(x86_64) || CITRA_ARCH(arm64)
