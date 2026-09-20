#pragma once

#include <fuse/types.hpp>

#include <string_view>

namespace fuse::script {

/// Legacy dialect routed by chunk-name prefix — dual TorqueScript VMs stay quarantined (U0–U2).
enum class LegacyScriptDialect : u8 {
    Fuse,
    UaiskCompat,
    T3dTorqueScript,
    T2dTorqueScript,
};

struct LegacyChunkRoute {
    LegacyScriptDialect dialect = LegacyScriptDialect::Fuse;
    std::string_view module;
};

/// Parse `uaisk:`, `t3d:`, `t2d:`, or `fuse:` chunk prefixes; unqualified names are FUSE scripts.
[[nodiscard]] LegacyChunkRoute parse_legacy_chunk_route(const char* chunk_name);

/// Stable diagnostic label for logging/tests.
[[nodiscard]] const char* legacy_dialect_name(LegacyScriptDialect dialect);

/// True when the dialect is recorded for compat routing without invoking a legacy VM.
[[nodiscard]] bool legacy_dialect_is_compat_stub(LegacyScriptDialect dialect);

} // namespace fuse::script
