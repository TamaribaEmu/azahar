// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/optimization_flags.h>
#include "common/assert.h"
#include "common/file_util.h"
#include "common/logging/log.h"
#include "common/microprofile.h"
#include "core/arm/dynarmic/arm_dynarmic.h"
#include "core/arm/dynarmic/arm_dynarmic_cp15.h"
#include "core/arm/dynarmic/arm_exclusive_monitor.h"
#include "core/arm/dynarmic/arm_tick_counts.h"
#include "core/core.h"
#include "core/core_timing.h"
#ifdef ENABLE_GDBSTUB
#include "core/gdbstub/gdbstub.h"
#endif
#include "core/hle/kernel/svc.h"
#include "core/memory.h"

#ifndef SIGILL
constexpr u32 SIGILL = 4;
#endif

#ifndef SIGTRAP
constexpr u32 SIGTRAP = 5;
#endif

namespace Core {

namespace {
constexpr std::array<char, 8> PersistentCacheMagic{'A', 'Z', 'D', 'Y', 'N', 'A', '3', '2'};
constexpr u32 PersistentCacheVersion = 2;
// Keep each emulated CPU's observed set separate. Loading a merged manifest into all four ARM11
// JITs multiplied native code memory and created reclaim pressure on the 3 GiB Shield.
constexpr std::size_t PersistentCachePrimaryMaximumBlocks = 100'000;
constexpr std::size_t PersistentCacheSecondaryMaximumBlocks = 20'000;
constexpr u32 PersistentModuleCacheVersion = 1;
constexpr std::size_t PersistentModuleMaximumCount = 16;
constexpr std::size_t PersistentModulePrimaryMaximumBlocks = 40'000;
constexpr std::size_t PersistentModuleSecondaryMaximumBlocks = 8'000;

constexpr std::size_t PersistentCacheMaximumBlocks(u32 core_id) {
    return core_id == 0 ? PersistentCachePrimaryMaximumBlocks
                        : PersistentCacheSecondaryMaximumBlocks;
}

struct PersistentCacheHeader {
    std::array<char, 8> magic;
    u32 version;
    u32 descriptor_size;
    u32 core_id;
    u64 program_id;
    u64 code_hash;
    u32 code_start;
    u32 code_size;
    u64 descriptor_count;
    u64 checksum;
};

struct PersistentModuleCacheHeader {
    std::array<char, 8> magic;
    u32 version;
    u32 descriptor_size;
    u32 core_id;
    u64 program_id;
    u64 module_hash;
    u32 code_size;
    u64 descriptor_count;
    u64 checksum;
};

constexpr std::array<char, 8> PersistentModuleCacheMagic{'A', 'Z', 'C', 'R', 'O', 'A', '3', '2'};

constexpr std::size_t PersistentModuleMaximumBlocks(u32 core_id) {
    return core_id == 0 ? PersistentModulePrimaryMaximumBlocks
                        : PersistentModuleSecondaryMaximumBlocks;
}

u64 PersistentCacheChecksum(u32 core_id, u64 program_id, u64 code_hash, u32 code_start,
                            u32 code_size, const std::vector<u64>& descriptors) {
    u64 hash = 1469598103934665603ULL;
    const auto mix = [&hash](const void* data, std::size_t size) {
        const auto* bytes = static_cast<const u8*>(data);
        for (std::size_t i = 0; i < size; ++i) {
            hash ^= bytes[i];
            hash *= 1099511628211ULL;
        }
    };
    mix(&PersistentCacheVersion, sizeof(PersistentCacheVersion));
    mix(&core_id, sizeof(core_id));
    mix(&program_id, sizeof(program_id));
    mix(&code_hash, sizeof(code_hash));
    mix(&code_start, sizeof(code_start));
    mix(&code_size, sizeof(code_size));
    if (!descriptors.empty())
        mix(descriptors.data(), descriptors.size() * sizeof(u64));
    return hash;
}

std::string PersistentCachePath(u32 core_id, u64 program_id, u64 code_hash) {
    const std::string directory = FileUtil::GetUserPath(FileUtil::UserPath::CacheDir) + "dynarmic/";
    FileUtil::CreateFullPath(directory);
    return fmt::format("{}{:016X}-{:016X}-core{}-a32-arm64-v{}.bin", directory, program_id,
                       code_hash, core_id, PersistentCacheVersion);
}

std::vector<u64> ReadPersistentCache(u32 core_id, u64 program_id, u64 code_hash, u32 code_start,
                                     u32 code_size) {
    const std::string path = PersistentCachePath(core_id, program_id, code_hash);
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (!file)
        return {};

    PersistentCacheHeader header{};
    if (std::fread(&header, sizeof(header), 1, file) != 1 || header.magic != PersistentCacheMagic ||
        header.version != PersistentCacheVersion || header.descriptor_size != sizeof(u64) ||
        header.core_id != core_id || header.program_id != program_id ||
        header.code_hash != code_hash || header.code_start != code_start ||
        header.code_size != code_size ||
        header.descriptor_count > PersistentCacheMaximumBlocks(core_id)) {
        std::fclose(file);
        return {};
    }
    std::vector<u64> descriptors(static_cast<std::size_t>(header.descriptor_count));
    const bool read_ok =
        descriptors.empty() ||
        std::fread(descriptors.data(), sizeof(u64), descriptors.size(), file) == descriptors.size();
    std::fclose(file);
    if (!read_ok || header.checksum != PersistentCacheChecksum(core_id, program_id, code_hash,
                                                               code_start, code_size, descriptors))
        return {};
    return descriptors;
}

bool IsSanePersistentDescriptor(u64 value, u32 code_start, u32 code_size) {
    const u32 pc = static_cast<u32>(value);
    const bool thumb = ((value >> 32) & 1) != 0;
    const bool single_stepping = ((value >> 34) & 1) != 0;
    const u32 alignment = thumb ? 2 : 4;
    const u64 code_end = static_cast<u64>(code_start) + code_size;
    return !single_stepping && pc >= code_start && pc < code_end && (pc & (alignment - 1)) == 0;
}

std::size_t WritePersistentCache(u32 core_id, u64 program_id, u64 code_hash, u32 code_start,
                                 u32 code_size, std::vector<u64> descriptors) {
    std::erase_if(descriptors, [code_start, code_size](u64 descriptor) {
        return !IsSanePersistentDescriptor(descriptor, code_start, code_size);
    });
    std::sort(descriptors.begin(), descriptors.end());
    descriptors.erase(std::unique(descriptors.begin(), descriptors.end()), descriptors.end());
    if (descriptors.size() > PersistentCacheMaximumBlocks(core_id)) {
        descriptors.resize(PersistentCacheMaximumBlocks(core_id));
    }

    if (descriptors.empty()) {
        return 0;
    }

    const std::string path = PersistentCachePath(core_id, program_id, code_hash);
    const std::string temporary = path + ".tmp";
    std::FILE* file = std::fopen(temporary.c_str(), "wb");
    if (!file) {
        LOG_ERROR(Core_ARM11, "Failed to open persistent Dynarmic cache {}", temporary);
        return 0;
    }
    const PersistentCacheHeader header{PersistentCacheMagic,
                                       PersistentCacheVersion,
                                       sizeof(u64),
                                       core_id,
                                       program_id,
                                       code_hash,
                                       code_start,
                                       code_size,
                                       descriptors.size(),
                                       PersistentCacheChecksum(core_id, program_id, code_hash,
                                                               code_start, code_size, descriptors)};
    const bool wrote =
        std::fwrite(&header, sizeof(header), 1, file) == 1 &&
        (descriptors.empty() || std::fwrite(descriptors.data(), sizeof(u64), descriptors.size(),
                                            file) == descriptors.size());
    const bool closed = std::fclose(file) == 0;
    if (wrote && closed) {
        if (FileUtil::Rename(temporary, path)) {
            return descriptors.size();
        }
        LOG_ERROR(Core_ARM11, "Failed to publish persistent Dynarmic cache {}", path);
    } else {
        FileUtil::Delete(temporary);
        LOG_ERROR(Core_ARM11, "Failed to write persistent Dynarmic cache {}", path);
    }
    return 0;
}

u64 PersistentModuleChecksum(u32 core_id, u64 program_id, u64 module_hash, u32 code_size,
                             const std::vector<u64>& descriptors) {
    u64 hash = 1469598103934665603ULL;
    const auto mix = [&hash](const void* data, std::size_t size) {
        const auto* bytes = static_cast<const u8*>(data);
        for (std::size_t i = 0; i < size; ++i) {
            hash ^= bytes[i];
            hash *= 1099511628211ULL;
        }
    };
    mix(&PersistentModuleCacheVersion, sizeof(PersistentModuleCacheVersion));
    mix(&core_id, sizeof(core_id));
    mix(&program_id, sizeof(program_id));
    mix(&module_hash, sizeof(module_hash));
    mix(&code_size, sizeof(code_size));
    if (!descriptors.empty()) {
        mix(descriptors.data(), descriptors.size() * sizeof(u64));
    }
    return hash;
}

std::string PersistentModuleCachePath(u32 core_id, u64 program_id, u64 module_hash) {
    const std::string directory =
        FileUtil::GetUserPath(FileUtil::UserPath::CacheDir) + "dynarmic/cro/";
    FileUtil::CreateFullPath(directory);
    return fmt::format("{}{:016X}-{:016X}-core{}-a32-arm64-v{}.bin", directory, program_id,
                       module_hash, core_id, PersistentModuleCacheVersion);
}

bool IsSanePersistentModuleDescriptor(u64 descriptor, u32 code_size) {
    const u32 offset = static_cast<u32>(descriptor);
    const bool thumb = ((descriptor >> 32) & 1) != 0;
    const bool single_stepping = ((descriptor >> 34) & 1) != 0;
    const u32 alignment = thumb ? 2 : 4;
    return !single_stepping && offset < code_size && (offset & (alignment - 1)) == 0;
}

std::vector<u64> ReadPersistentModuleCache(u32 core_id, u64 program_id, u64 module_hash,
                                           u32 code_size) {
    const std::string path = PersistentModuleCachePath(core_id, program_id, module_hash);
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) {
        return {};
    }
    PersistentModuleCacheHeader header{};
    if (std::fread(&header, sizeof(header), 1, file) != 1 ||
        header.magic != PersistentModuleCacheMagic ||
        header.version != PersistentModuleCacheVersion || header.descriptor_size != sizeof(u64) ||
        header.core_id != core_id || header.program_id != program_id ||
        header.module_hash != module_hash || header.code_size != code_size ||
        header.descriptor_count > PersistentModuleMaximumBlocks(core_id)) {
        std::fclose(file);
        return {};
    }
    std::vector<u64> descriptors(static_cast<std::size_t>(header.descriptor_count));
    const bool read_ok =
        descriptors.empty() ||
        std::fread(descriptors.data(), sizeof(u64), descriptors.size(), file) == descriptors.size();
    std::fclose(file);
    if (!read_ok || header.checksum != PersistentModuleChecksum(core_id, program_id, module_hash,
                                                                code_size, descriptors)) {
        return {};
    }
    std::erase_if(descriptors, [code_size](u64 descriptor) {
        return !IsSanePersistentModuleDescriptor(descriptor, code_size);
    });
    return descriptors;
}

std::size_t WritePersistentModuleCache(u32 core_id, u64 program_id, u64 module_hash, u32 code_size,
                                       std::vector<u64> descriptors) {
    std::erase_if(descriptors, [code_size](u64 descriptor) {
        return !IsSanePersistentModuleDescriptor(descriptor, code_size);
    });
    std::sort(descriptors.begin(), descriptors.end());
    descriptors.erase(std::unique(descriptors.begin(), descriptors.end()), descriptors.end());
    if (descriptors.size() > PersistentModuleMaximumBlocks(core_id)) {
        descriptors.resize(PersistentModuleMaximumBlocks(core_id));
    }
    if (descriptors.empty()) {
        return 0;
    }

    const std::string path = PersistentModuleCachePath(core_id, program_id, module_hash);
    const std::string temporary = path + ".tmp";
    std::FILE* file = std::fopen(temporary.c_str(), "wb");
    if (!file) {
        LOG_ERROR(Core_ARM11, "Failed to open persistent CRO cache {}", temporary);
        return 0;
    }
    const PersistentModuleCacheHeader header{
        PersistentModuleCacheMagic,
        PersistentModuleCacheVersion,
        sizeof(u64),
        core_id,
        program_id,
        module_hash,
        code_size,
        descriptors.size(),
        PersistentModuleChecksum(core_id, program_id, module_hash, code_size, descriptors)};
    const bool wrote = std::fwrite(&header, sizeof(header), 1, file) == 1 &&
                       std::fwrite(descriptors.data(), sizeof(u64), descriptors.size(), file) ==
                           descriptors.size();
    const bool closed = std::fclose(file) == 0;
    if (wrote && closed && FileUtil::Rename(temporary, path)) {
        return descriptors.size();
    }
    FileUtil::Delete(temporary);
    LOG_ERROR(Core_ARM11, "Failed to save persistent CRO cache {}", path);
    return 0;
}
} // namespace

class DynarmicUserCallbacks final : public Dynarmic::A32::UserCallbacks {
public:
    explicit DynarmicUserCallbacks(ARM_Dynarmic& parent)
        : parent(parent), svc_context(parent.system), memory(parent.memory) {}
    ~DynarmicUserCallbacks() = default;

    std::optional<std::uint32_t> MemoryReadCode(VAddr vaddr) override {
        return memory.Read32OrNullopt(vaddr);
    }

    std::uint8_t MemoryRead8(VAddr vaddr) override {
        return memory.Read8(vaddr);
    }
    std::uint16_t MemoryRead16(VAddr vaddr) override {
        return memory.Read16(vaddr);
    }
    std::uint32_t MemoryRead32(VAddr vaddr) override {
        return memory.Read32(vaddr);
    }
    std::uint64_t MemoryRead64(VAddr vaddr) override {
        return memory.Read64(vaddr);
    }

    void MemoryWrite8(VAddr vaddr, std::uint8_t value) override {
        memory.Write8(vaddr, value);
    }
    void MemoryWrite16(VAddr vaddr, std::uint16_t value) override {
        memory.Write16(vaddr, value);
    }
    void MemoryWrite32(VAddr vaddr, std::uint32_t value) override {
        memory.Write32(vaddr, value);
    }
    void MemoryWrite64(VAddr vaddr, std::uint64_t value) override {
        memory.Write64(vaddr, value);
    }

    bool MemoryWriteExclusive8(u32 vaddr, u8 value, u8 expected) override {
        return memory.WriteExclusive8(vaddr, value, expected);
    }
    bool MemoryWriteExclusive16(u32 vaddr, u16 value, u16 expected) override {
        return memory.WriteExclusive16(vaddr, value, expected);
    }
    bool MemoryWriteExclusive32(u32 vaddr, u32 value, u32 expected) override {
        return memory.WriteExclusive32(vaddr, value, expected);
    }
    bool MemoryWriteExclusive64(u32 vaddr, u64 value, u64 expected) override {
        return memory.WriteExclusive64(vaddr, value, expected);
    }

    void InterpreterFallback(VAddr pc, std::size_t num_instructions) override {
        // Should never happen.
        UNREACHABLE_MSG("InterpeterFallback reached with pc = 0x{:08x}, code = 0x{:08x}, num = {}",
                        pc, MemoryReadCode(pc).value(), num_instructions);
    }

    void CallSVC(std::uint32_t swi) override {
        svc_context.CallSVC(swi);
    }

    void ExceptionRaised(VAddr pc, Dynarmic::A32::Exception exception) override {
        switch (exception) {
        case Dynarmic::A32::Exception::UndefinedInstruction:
        case Dynarmic::A32::Exception::UnpredictableInstruction:
        case Dynarmic::A32::Exception::DecodeError:
        case Dynarmic::A32::Exception::NoExecuteFault:
            break;
        case Dynarmic::A32::Exception::Breakpoint:
#ifdef ENABLE_GDBSTUB
            if (GDBStub::IsConnected()) {
                parent.SetPC(pc);
                parent.ServeBreak(SIGTRAP);
                return;
            }
#endif
            break;
        case Dynarmic::A32::Exception::SendEvent:
        case Dynarmic::A32::Exception::SendEventLocal:
        case Dynarmic::A32::Exception::WaitForInterrupt:
        case Dynarmic::A32::Exception::WaitForEvent:
        case Dynarmic::A32::Exception::Yield:
        case Dynarmic::A32::Exception::PreloadData:
        case Dynarmic::A32::Exception::PreloadDataWithIntentToWrite:
        case Dynarmic::A32::Exception::PreloadInstruction:
            return;
        }

        static constexpr auto ExceptionToString = [](Dynarmic::A32::Exception e) -> std::string {
            switch (e) {
            case Dynarmic::A32::Exception::UndefinedInstruction:
                return "UndefinedInstruction";
            case Dynarmic::A32::Exception::UnpredictableInstruction:
                return "UnpredictableInstruction";
            case Dynarmic::A32::Exception::DecodeError:
                return "DecodeError";
            case Dynarmic::A32::Exception::NoExecuteFault:
                return "NoExecuteFault";
            case Dynarmic::A32::Exception::Breakpoint:
                return "Breakpoint";
            default:
                return fmt::format("Unknown({})", e);
            }
        };

        parent.SetPC(pc);
#ifdef ENABLE_GDBSTUB
        if (GDBStub::IsConnected()) {
            parent.ServeBreak(SIGILL);
        } else
#endif
        {
            std::string error;
            for (int i = 0; i < 16; i++) {
                error += fmt::format("r{:02d} = {:08X}\n", i, parent.GetReg(i));
            }
            error += fmt::format("ExceptionRaised(exception = {}, pc = {:08X})",
                                 ExceptionToString(exception), pc);
            parent.system.SetStatus(Core::System::ResultStatus::ErrorCoreExceptionRaised,
                                    error.c_str());
        }
    }

    void AddTicks(std::uint64_t ticks) override {
        parent.GetTimer().AddTicks(ticks);
    }
    std::uint64_t GetTicksRemaining() override {
        s64 ticks = parent.GetTimer().GetDowncount();
        return static_cast<u64>(ticks <= 0 ? 0 : ticks);
    }
    std::uint64_t GetTicksForCode(bool is_thumb, VAddr, std::uint32_t instruction) override {
        return Core::TicksForInstruction(is_thumb, instruction);
    }

    ARM_Dynarmic& parent;
    Kernel::SVCContext svc_context;
    Memory::MemorySystem& memory;
};

ARM_Dynarmic::ARM_Dynarmic(Core::System& system_, Memory::MemorySystem& memory_, u32 core_id_,
                           std::shared_ptr<Core::Timing::Timer> timer_,
                           Core::ExclusiveMonitor& exclusive_monitor_)
    : ARM_Interface(core_id_, timer_), system(system_), memory(memory_),
      cb(std::make_unique<DynarmicUserCallbacks>(*this)),
      exclusive_monitor{dynamic_cast<Core::DynarmicExclusiveMonitor&>(exclusive_monitor_)} {
    SetPageTable(memory.GetCurrentPageTable());
}

ARM_Dynarmic::~ARM_Dynarmic() {
    FlushPersistentCache();
}

MICROPROFILE_DEFINE(ARM_Jit, "ARM JIT", "ARM JIT", MP_RGB(255, 64, 64));

void ARM_Dynarmic::Run() {
    ASSERT(memory.GetCurrentPageTable() == current_page_table);
    LoadPersistentModulesIfReady();
    MICROPROFILE_SCOPE(ARM_Jit);
    if (break_flag) [[unlikely]] {
        return;
    }

    jit->Run();
}

void ARM_Dynarmic::Step() {
    if (break_flag) [[unlikely]] {
        return;
    }

    jit->Step();
}

void ARM_Dynarmic::SetPC(u32 pc) {
    jit->Regs()[15] = pc;
}

u32 ARM_Dynarmic::GetPC() const {
    return jit->Regs()[15];
}

u32 ARM_Dynarmic::GetReg(int index) const {
    return jit->Regs()[index];
}

void ARM_Dynarmic::SetReg(int index, u32 value) {
    jit->Regs()[index] = value;
}

u32* ARM_Dynarmic::RegFile() {
    return jit->Regs().data();
}

u32 ARM_Dynarmic::GetVFPReg(int index) const {
    return jit->ExtRegs()[index];
}

void ARM_Dynarmic::SetVFPReg(int index, u32 value) {
    jit->ExtRegs()[index] = value;
}

u32 ARM_Dynarmic::GetVFPSystemReg(VFPSystemRegister reg) const {
    switch (reg) {
    case VFP_FPSCR:
        return jit->Fpscr();
    case VFP_FPEXC:
        return fpexc;
    default:
        UNREACHABLE_MSG("Unknown VFP system register: {}", reg);
    }

    return UINT_MAX;
}

void ARM_Dynarmic::SetVFPSystemReg(VFPSystemRegister reg, u32 value) {
    switch (reg) {
    case VFP_FPSCR:
        jit->SetFpscr(value);
        return;
    case VFP_FPEXC:
        fpexc = value;
        return;
    default:
        UNREACHABLE_MSG("Unknown VFP system register: {}", reg);
    }
}

u32 ARM_Dynarmic::GetCPSR() const {
    return jit->Cpsr();
}

void ARM_Dynarmic::SetCPSR(u32 cpsr) {
    jit->SetCpsr(cpsr);
}

u32 ARM_Dynarmic::GetCP15Register(CP15Register reg) const {
    switch (reg) {
    case CP15_THREAD_UPRW:
        return cp15_state.cp15_thread_uprw;
    case CP15_THREAD_URO:
        return cp15_state.cp15_thread_uro;
    default:
        UNREACHABLE_MSG("Unknown CP15 register: {}", reg);
    }

    return 0;
}

void ARM_Dynarmic::SetCP15Register(CP15Register reg, u32 value) {
    switch (reg) {
    case CP15_THREAD_UPRW:
        cp15_state.cp15_thread_uprw = value;
        return;
    case CP15_THREAD_URO:
        cp15_state.cp15_thread_uro = value;
        return;
    default:
        UNREACHABLE_MSG("Unknown CP15 register: {}", reg);
    }
}

void ARM_Dynarmic::SaveContext(ThreadContext& ctx) {
    ctx.cpu_registers = jit->Regs();
    ctx.cpsr = jit->Cpsr();
    ctx.fpu_registers = jit->ExtRegs();
    ctx.fpscr = jit->Fpscr();
    ctx.fpexc = fpexc;
}

void ARM_Dynarmic::LoadContext(const ThreadContext& ctx) {
    jit->Regs() = ctx.cpu_registers;
    jit->SetCpsr(ctx.cpsr);
    jit->ExtRegs() = ctx.fpu_registers;
    jit->SetFpscr(ctx.fpscr);
    fpexc = ctx.fpexc;
}

void ARM_Dynarmic::PrepareReschedule() {
    if (jit->IsExecuting()) {
        jit->HaltExecution();
    }
}

void ARM_Dynarmic::ClearInstructionCache() {
    for (const auto& j : jits) {
        j.second->ClearCache();
    }
}

void ARM_Dynarmic::InvalidateCacheRange(u32 start_address, std::size_t length) {
    jit->InvalidateCacheRange(start_address, length);
}

void ARM_Dynarmic::ClearExclusiveState() {
    jit->ClearExclusiveState();
}

std::shared_ptr<Memory::PageTable> ARM_Dynarmic::GetPageTable() const {
    return current_page_table;
}

void ARM_Dynarmic::SetPageTable(const std::shared_ptr<Memory::PageTable>& page_table) {
    current_page_table = page_table;
    ThreadContext ctx{};
    if (jit) {
        SaveContext(ctx);
    }

    auto iter = jits.find(current_page_table);
    if (iter != jits.end()) {
        jit = iter->second.get();
        LoadContext(ctx);
        LoadPersistentCacheIfReady();
        return;
    }

    auto new_jit = MakeJit();
    jit = new_jit.get();
    LoadContext(ctx);
    jits.emplace(current_page_table, std::move(new_jit));
    LoadPersistentCacheIfReady();
}

void ARM_Dynarmic::ConfigurePersistentCache(u64 program_id, u64 code_hash, u32 code_start,
                                            u32 code_size,
                                            const std::shared_ptr<Memory::PageTable>& page_table) {
    persistent_cache_program_id = program_id;
    persistent_cache_code_hash = code_hash;
    persistent_cache_code_start = code_start;
    persistent_cache_code_size = code_size;
    persistent_cache_page_table = page_table;
    persistent_cache_loaded = false;
    LOG_INFO(Core_ARM11,
             "Configured persistent Dynarmic cache for {:016X}, code {:08X}-{:08X}, hash {:016X}",
             persistent_cache_program_id, persistent_cache_code_start,
             persistent_cache_code_start + persistent_cache_code_size, persistent_cache_code_hash);
    LoadPersistentCacheIfReady();
}

void ARM_Dynarmic::LoadPersistentCacheIfReady() {
    if (persistent_cache_loaded || persistent_cache_program_id == 0 ||
        !persistent_cache_page_table || current_page_table != persistent_cache_page_table || !jit) {
        return;
    }
    persistent_cache_loaded = true;
    auto descriptors =
        ReadPersistentCache(GetID(), persistent_cache_program_id, persistent_cache_code_hash,
                            persistent_cache_code_start, persistent_cache_code_size);
    std::erase_if(descriptors, [this](u64 descriptor) {
        return !IsSanePersistentDescriptor(descriptor, persistent_cache_code_start,
                                           persistent_cache_code_size);
    });
    if (descriptors.empty()) {
        LOG_INFO(Core_ARM11, "No persistent Dynarmic cache found for {:016X}",
                 persistent_cache_program_id);
        return;
    }

    const auto started = std::chrono::steady_clock::now();
    const std::size_t compiled = jit->Precompile(descriptors);
    const double elapsed_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
            .count();
    LOG_INFO(Core_ARM11,
             "Precompiled {} persistent Dynarmic blocks for {:016X} core {} in {:.1f} ms", compiled,
             persistent_cache_program_id, GetID(), elapsed_ms);
}

void ARM_Dynarmic::FlushPersistentCache() {
    if (persistent_cache_program_id == 0 || !persistent_cache_page_table) {
        return;
    }
    const auto iter = jits.find(persistent_cache_page_table);
    if (iter == jits.end()) {
        LOG_WARNING(Core_ARM11,
                    "Persistent Dynarmic cache has no matching address space for {:016X}",
                    persistent_cache_program_id);
        return;
    }
    auto descriptors = iter->second->GetCompiledBlockDescriptors();
    const std::size_t compiled_count = descriptors.size();
    const std::size_t saved_count = WritePersistentCache(
        GetID(), persistent_cache_program_id, persistent_cache_code_hash,
        persistent_cache_code_start, persistent_cache_code_size, std::move(descriptors));
    LOG_INFO(
        Core_ARM11,
        "Flushed {} of {} compiled blocks to the persistent Dynarmic cache for {:016X} core {}",
        saved_count, compiled_count, persistent_cache_program_id, GetID());

    for (const auto& module : persistent_modules) {
        FlushPersistentModule(module);
    }
}

JitCacheStats ARM_Dynarmic::GetAndResetJitCacheStats() {
    JitCacheStats aggregate{};
    aggregate.address_spaces = jits.size();
    for (const auto& [page_table, jit_instance] : jits) {
        const auto stats = jit_instance->GetAndResetCacheStats();
        aggregate.emitted_blocks += stats.emitted_blocks;
        aggregate.emitted_bytes += stats.emitted_bytes;
        aggregate.invalidated_blocks += stats.invalidated_blocks;
        aggregate.cache_clears += stats.cache_clears;
        aggregate.live_blocks += stats.live_blocks;
        aggregate.used_bytes += stats.used_bytes;
        aggregate.capacity_bytes += stats.capacity_bytes;
        aggregate.precompile_translate_nanoseconds += stats.precompile_translate_nanoseconds;
        aggregate.precompile_emit_nanoseconds += stats.precompile_emit_nanoseconds;
        aggregate.precompile_body_nanoseconds += stats.precompile_body_nanoseconds;
        aggregate.precompile_terminal_nanoseconds += stats.precompile_terminal_nanoseconds;
        aggregate.precompile_deferred_nanoseconds += stats.precompile_deferred_nanoseconds;
        aggregate.precompile_metadata_nanoseconds += stats.precompile_metadata_nanoseconds;
        aggregate.precompile_link_nanoseconds += stats.precompile_link_nanoseconds;
    }
    return aggregate;
}

void ARM_Dynarmic::RegisterPersistentModule(u64 module_hash, u64 executable_hash, u32 code_start,
                                            u32 code_size) {
    if (persistent_cache_program_id == 0 || code_size == 0 || !persistent_cache_page_table) {
        return;
    }
    const auto existing = std::find_if(
        persistent_modules.begin(), persistent_modules.end(),
        [code_start](const PersistentModule& module) { return module.code_start == code_start; });
    if (existing != persistent_modules.end() ||
        persistent_modules.size() >= PersistentModuleMaximumCount) {
        return;
    }

    auto descriptors =
        ReadPersistentModuleCache(GetID(), persistent_cache_program_id, module_hash, code_size);
    for (u64& descriptor : descriptors) {
        descriptor = (descriptor & 0xFFFFFFFF00000000ULL) |
                     static_cast<u32>(code_start + static_cast<u32>(descriptor));
    }
    std::vector<Dynarmic::A32::JitBlockCacheEntry> dormant_blocks;
    const auto exact_dormant = std::find_if(
        dormant_modules.rbegin(), dormant_modules.rend(),
        [module_hash, executable_hash, code_start, code_size](const DormantModule& module) {
            return module.hash == module_hash && module.executable_hash == executable_hash &&
                   module.code_start == code_start && module.code_size == code_size;
        });
    const auto compatible_dormant =
        exact_dormant != dormant_modules.rend()
            ? exact_dormant
            : std::find_if(dormant_modules.rbegin(), dormant_modules.rend(),
                           [module_hash, code_start, code_size](const DormantModule& module) {
                               return module.hash == module_hash &&
                                      module.code_start == code_start &&
                                      module.code_size == code_size;
                           });
    if (compatible_dormant != dormant_modules.rend()) {
        dormant_blocks = compatible_dormant->blocks;
    }
    persistent_modules.push_back({module_hash, executable_hash, code_start, code_size,
                                  std::move(descriptors), std::move(dormant_blocks)});
}

void ARM_Dynarmic::LoadPersistentModulesIfReady() {
    if (!jit || current_page_table != persistent_cache_page_table) {
        return;
    }

    for (auto& module : persistent_modules) {
        if (!module.dormant_blocks.empty()) {
            const std::size_t expected = module.dormant_blocks.size();
            const std::size_t reactivated = jit->ReactivateBlocks(module.dormant_blocks);
            module.dormant_blocks.clear();
            module.dormant_blocks.shrink_to_fit();
            if (reactivated == expected) {
                LOG_INFO(Core_ARM11,
                         "Reactivated {} dormant CRO blocks for {:016X} module {:016X} core {}",
                         reactivated, persistent_cache_program_id, module.hash, GetID());
                module.pending_descriptors.clear();
                module.pending_descriptors.shrink_to_fit();
                continue;
            }
            LOG_INFO(Core_ARM11,
                     "Reactivated {} of {} unchanged dormant CRO blocks for module {:016X}; "
                     "precompiling changed blocks",
                     reactivated, expected, module.hash);
        }
        if (module.pending_descriptors.empty()) {
            continue;
        }

        const auto started = std::chrono::steady_clock::now();
        const std::size_t compiled = jit->Precompile(module.pending_descriptors);
        const double elapsed_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
                .count();
        LOG_INFO(Core_ARM11,
                 "Eagerly precompiled {} persistent CRO blocks for {:016X} module {:016X} core {} "
                 "in {:.1f} ms",
                 compiled, persistent_cache_program_id, module.hash, GetID(), elapsed_ms);
        module.pending_descriptors.clear();
        module.pending_descriptors.shrink_to_fit();
    }
}

void ARM_Dynarmic::FlushPersistentModule(const PersistentModule& module) {
    const auto jit_iter = jits.find(persistent_cache_page_table);
    if (jit_iter == jits.end()) {
        return;
    }
    auto descriptors = jit_iter->second->GetCompiledBlockDescriptors();
    std::erase_if(descriptors, [&module](u64 descriptor) {
        return !IsSanePersistentDescriptor(descriptor, module.code_start, module.code_size);
    });
    for (u64& descriptor : descriptors) {
        descriptor = (descriptor & 0xFFFFFFFF00000000ULL) |
                     static_cast<u32>(static_cast<u32>(descriptor) - module.code_start);
    }
    const std::size_t saved =
        WritePersistentModuleCache(GetID(), persistent_cache_program_id, module.hash,
                                   module.code_size, std::move(descriptors));
    LOG_INFO(Core_ARM11, "Saved {} persistent CRO blocks for module {:016X} core {}", saved,
             module.hash, GetID());
}

void ARM_Dynarmic::UnregisterPersistentModule(u32 code_start) {
    const auto iter = std::find_if(
        persistent_modules.begin(), persistent_modules.end(),
        [code_start](const PersistentModule& module) { return module.code_start == code_start; });
    if (iter == persistent_modules.end()) {
        return;
    }
    persistent_modules.erase(iter);
}

void ARM_Dynarmic::CheckpointPersistentModule(u32 code_start, u64 executable_hash) {
    const auto iter = std::find_if(
        persistent_modules.begin(), persistent_modules.end(),
        [code_start](const PersistentModule& module) { return module.code_start == code_start; });
    if (iter != persistent_modules.end()) {
        FlushPersistentModule(*iter);
        auto entries = jit->GetCompiledBlockEntries();
        std::erase_if(entries, [&module = *iter](const auto& entry) {
            return !IsSanePersistentDescriptor(entry.descriptor, module.code_start,
                                               module.code_size);
        });
        if (executable_hash == iter->executable_hash && !entries.empty()) {
            const auto existing = std::find_if(
                dormant_modules.begin(), dormant_modules.end(),
                [&module = *iter](const DormantModule& dormant) {
                    return dormant.hash == module.hash &&
                           dormant.executable_hash == module.executable_hash &&
                           dormant.code_start == module.code_start &&
                           dormant.code_size == module.code_size;
                });
            DormantModule snapshot{iter->hash, iter->executable_hash, iter->code_start,
                                   iter->code_size, std::move(entries)};
            if (existing != dormant_modules.end()) {
                *existing = std::move(snapshot);
            } else {
                if (dormant_modules.size() >= PersistentModuleMaximumCount) {
                    dormant_modules.erase(dormant_modules.begin());
                }
                dormant_modules.emplace_back(std::move(snapshot));
            }
        }
    }
}

void ARM_Dynarmic::ServeBreak([[maybe_unused]] int signal) {
#ifdef ENABLE_GDBSTUB
    GDBStub::Break(signal);
#endif
}

std::unique_ptr<Dynarmic::A32::Jit> ARM_Dynarmic::MakeJit() {
    Dynarmic::A32::UserConfig config;
    config.callbacks = cb.get();
    if (current_page_table) {
        config.page_table = &current_page_table->GetPointerArray();
    }
    config.coprocessors[15] = std::make_shared<DynarmicCP15>(cp15_state);
    config.define_unpredictable_behaviour = true;

    // Multi-process state
    config.processor_id = GetID();
    config.global_monitor = &exclusive_monitor.monitor;

    return std::make_unique<Dynarmic::A32::Jit>(config);
}

} // namespace Core
