#include <fuse/script/legacy_script_route.hpp>

#include <cstring>

namespace fuse::script {

namespace {

bool startsWith(const char* text, const char* prefix) {
    if (text == nullptr || prefix == nullptr) {
        return false;
    }
    return std::strncmp(text, prefix, std::strlen(prefix)) == 0;
}

LegacyChunkRoute routeWithPrefix(const char* chunk_name, const char* prefix, LegacyScriptDialect dialect) {
    LegacyChunkRoute route;
    route.dialect = dialect;
    route.module = std::string_view(chunk_name + std::strlen(prefix));
    return route;
}

} // namespace

LegacyChunkRoute parse_legacy_chunk_route(const char* chunk_name) {
    LegacyChunkRoute route;
    if (chunk_name == nullptr || chunk_name[0] == '\0') {
        return route;
    }

    if (startsWith(chunk_name, "uaisk:")) {
        return routeWithPrefix(chunk_name, "uaisk:", LegacyScriptDialect::UaiskCompat);
    }
    if (startsWith(chunk_name, "t3d:")) {
        return routeWithPrefix(chunk_name, "t3d:", LegacyScriptDialect::T3dTorqueScript);
    }
    if (startsWith(chunk_name, "t2d:")) {
        return routeWithPrefix(chunk_name, "t2d:", LegacyScriptDialect::T2dTorqueScript);
    }
    if (startsWith(chunk_name, "fuse:")) {
        return routeWithPrefix(chunk_name, "fuse:", LegacyScriptDialect::Fuse);
    }

    route.module = std::string_view(chunk_name);
    return route;
}

const char* legacy_dialect_name(LegacyScriptDialect dialect) {
    switch (dialect) {
        case LegacyScriptDialect::Fuse:
            return "fuse";
        case LegacyScriptDialect::UaiskCompat:
            return "uaisk_compat";
        case LegacyScriptDialect::T3dTorqueScript:
            return "t3d_torquescript";
        case LegacyScriptDialect::T2dTorqueScript:
            return "t2d_torquescript";
    }
    return "unknown";
}

bool legacy_dialect_is_compat_stub(LegacyScriptDialect dialect) {
    return dialect == LegacyScriptDialect::T3dTorqueScript ||
           dialect == LegacyScriptDialect::T2dTorqueScript;
}

} // namespace fuse::script
