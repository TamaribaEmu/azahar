// Copyright 2016 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include "common/arch.h"
#if CITRA_ARCH(x86_64) || CITRA_ARCH(arm64)

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "common/common_types.h"
#include "video_core/shader/shader.h"

namespace Pica::Shader {

class JitShader;

class JitEngine final : public ShaderEngine {
public:
    JitEngine();
    ~JitEngine() override;

    void ConfigurePersistentCache(u64 program_id) override;
    void FlushPersistentCache() override;
    void SetupBatch(ShaderSetup& setup, u32 entry_point) override;
    void Run(const ShaderSetup& setup, ShaderUnit& state) const override;

private:
    struct PersistentShader;

    void LoadPersistentCache();

    std::unordered_map<u64, std::unique_ptr<JitShader>> cache;
    std::vector<PersistentShader> persistent_shaders;
    std::string persistent_cache_path;
    u64 persistent_program_id{};
    bool persistent_cache_dirty{};
};

} // namespace Pica::Shader

#endif // CITRA_ARCH(x86_64) || CITRA_ARCH(arm64)
