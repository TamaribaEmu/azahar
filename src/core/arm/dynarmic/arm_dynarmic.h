// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <map>
#include <memory>
#include <set>
#include <utility>
#include <vector>
#include <dynarmic/interface/A32/a32.h>
#include "common/common_types.h"
#include "core/arm/arm_interface.h"
#include "core/arm/dynarmic/arm_dynarmic_cp15.h"
#include "core/arm/jit_cache_stats.h"

namespace Memory {
struct PageTable;
class MemorySystem;
} // namespace Memory

namespace Core {

class DynarmicUserCallbacks;
class DynarmicExclusiveMonitor;
class ExclusiveMonitor;
class System;

class ARM_Dynarmic final : public ARM_Interface {
public:
    explicit ARM_Dynarmic(Core::System& system_, Memory::MemorySystem& memory_, u32 core_id_,
                          std::shared_ptr<Core::Timing::Timer> timer,
                          Core::ExclusiveMonitor& exclusive_monitor_);
    ~ARM_Dynarmic() override;

    void Run() override;
    void Step() override;

    void SetPC(u32 pc) override;
    u32 GetPC() const override;
    u32 GetReg(int index) const override;
    void SetReg(int index, u32 value) override;
    u32* RegFile() override;
    u32 GetVFPReg(int index) const override;
    void SetVFPReg(int index, u32 value) override;
    u32 GetVFPSystemReg(VFPSystemRegister reg) const override;
    void SetVFPSystemReg(VFPSystemRegister reg, u32 value) override;
    u32 GetCPSR() const override;
    void SetCPSR(u32 cpsr) override;
    u32 GetCP15Register(CP15Register reg) const override;
    void SetCP15Register(CP15Register reg, u32 value) override;

    void SaveContext(ThreadContext& ctx) override;
    void LoadContext(const ThreadContext& ctx) override;

    void PrepareReschedule() override;

    void ClearInstructionCache() override;
    void InvalidateCacheRange(u32 start_address, std::size_t length) override;
    void ClearExclusiveState() override;
    void SetPageTable(const std::shared_ptr<Memory::PageTable>& page_table) override;

    /// Enables the validated per-title guest-block manifest for this CPU core.
    void ConfigurePersistentCache(u64 program_id, u64 code_hash, u32 code_start, u32 code_size,
                                  const std::shared_ptr<Memory::PageTable>& page_table);

    /// Writes translated guest-block descriptors while the emulation state is still intact.
    void FlushPersistentCache();

    void RegisterPersistentModule(u64 module_hash, u64 executable_hash, u32 code_start,
                                  u32 code_size);
    void CheckpointPersistentModule(u32 code_start, u64 executable_hash);
    void UnregisterPersistentModule(u32 code_start);

    /// Returns interval JIT events and current aggregate code-cache occupancy.
    JitCacheStats GetAndResetJitCacheStats();

    bool HasSingleInstructionBreakAccuracy() override {
        return false;
    }

protected:
    std::shared_ptr<Memory::PageTable> GetPageTable() const override;

private:
    void ServeBreak(int signal);
    void LoadPersistentCacheIfReady();
    struct PersistentModule {
        u64 hash;
        u64 executable_hash;
        u32 code_start;
        u32 code_size;
        std::vector<u64> pending_descriptors;
        std::vector<Dynarmic::A32::JitBlockCacheEntry> dormant_blocks;
    };
    struct DormantModule {
        u64 hash;
        u64 executable_hash;
        u32 code_start;
        u32 code_size;
        std::vector<Dynarmic::A32::JitBlockCacheEntry> blocks;
    };
    void LoadPersistentModulesIfReady();
    void FlushPersistentModule(const PersistentModule& module);
    // Boot-time warm-up of loadable modules (CROs): their code is saved beside their block
    // lists when they load, and at boot their blocks are compiled from that copy into the
    // dormant set, so the first load of a module in a session reactivates instead of compiling.
    void SavePersistentModuleCode(const PersistentModule& module);
    void PrewarmPersistentModules();

    friend class DynarmicUserCallbacks;
    Core::System& system;
    Memory::MemorySystem& memory;
    std::unique_ptr<DynarmicUserCallbacks> cb;
    std::unique_ptr<Dynarmic::A32::Jit> MakeJit();

    u32 fpexc = 0;
    CP15State cp15_state;
    Core::DynarmicExclusiveMonitor& exclusive_monitor;

    Dynarmic::A32::Jit* jit = nullptr;
    std::shared_ptr<Memory::PageTable> current_page_table = nullptr;
    std::map<std::shared_ptr<Memory::PageTable>, std::unique_ptr<Dynarmic::A32::Jit>> jits;
    u64 persistent_cache_program_id = 0;
    u64 persistent_cache_code_hash = 0;
    u32 persistent_cache_code_start = 0;
    u32 persistent_cache_code_size = 0;
    std::shared_ptr<Memory::PageTable> persistent_cache_page_table;
    bool persistent_cache_loaded = false;
    bool persistent_cache_flushed = false;  // written already (System::Shutdown)
    std::vector<PersistentModule> persistent_modules;
    std::vector<DormantModule> dormant_modules;
    /// While prewarming: Dynarmic reads these bytes for [code_override_start, +size) instead of
    /// guest memory (MemoryReadCode), so a module can be compiled before the game loads it.
    u32 code_override_start = 0;
    std::vector<u8> code_override;
    /// (module hash, executable hash) pairs whose code is known to be on disk.
    std::set<std::pair<u64, u64>> saved_module_code;
    /// Per module hash: digest of the block list last read from or written to disk.
    std::map<u64, u64> saved_module_digests;
    /// Each module's descriptor list as read from its cache file (relative to the module), so a
    /// module loaded again (Pokémon's field after every battle) skips reading and decoding it.
    std::map<u64, std::vector<u64>> module_descriptor_cache;
};

} // namespace Core
