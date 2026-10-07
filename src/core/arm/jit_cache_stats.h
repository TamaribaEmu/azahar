// Copyright Azahar Emulator Project / GBAShield
// Licensed under GPLv2 or any later version

#pragma once

#include "common/common_types.h"

namespace Core {

struct JitCacheStats {
    u64 emitted_blocks{};
    u64 emitted_bytes{};
    u64 invalidated_blocks{};
    u64 cache_clears{};
    u64 live_blocks{};
    u64 used_bytes{};
    u64 capacity_bytes{};
    u64 address_spaces{};
    u64 precompile_translate_nanoseconds{};
    u64 precompile_emit_nanoseconds{};
    u64 precompile_body_nanoseconds{};
    u64 precompile_terminal_nanoseconds{};
    u64 precompile_deferred_nanoseconds{};
    u64 precompile_metadata_nanoseconds{};
    u64 precompile_link_nanoseconds{};
};

} // namespace Core
