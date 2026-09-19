#include <fuse/legacy/t3d/api.hpp>
#include <fuse/log/logger.hpp>

#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

namespace {

std::unordered_map<std::string, std::string> g_variables;
std::unordered_map<std::string, std::string> g_pathExpandos;

void logFormatted(fuse::log::Level level, const char* fmt, va_list args) {
    fuse::log::Logger::instance().logV(level, fmt, args);
}

std::string normalizeVariableName(const char* name) {
    if (!name) {
        return {};
    }
    if (name[0] == '$') {
        return name;
    }
    return std::string("$") + name;
}

bool copyToBuffer(char* dst, std::uint32_t size, const char* src) {
    if (!dst || size == 0) {
        return false;
    }
    std::snprintf(dst, size, "%s", src ? src : "");
    return true;
}

void appendTrailingSlash(char* dst, std::uint32_t size, const char* src) {
    if (!dst || size == 0 || !src) {
        return;
    }
    const std::size_t len = std::strlen(src);
    if (len > 0 && src[len - 1] == '/') {
        copyToBuffer(dst, size, src);
        return;
    }
    std::snprintf(dst, size, "%s/", src);
}

bool parseBool(const char* value, bool def) {
    if (!value || value[0] == '\0') {
        return def;
    }
    if (value[0] == '0' && value[1] == '\0') {
        return false;
    }
    if (value[0] == '1' && value[1] == '\0') {
        return true;
    }
    if (std::strcmp(value, "false") == 0 || std::strcmp(value, "False") == 0) {
        return false;
    }
    if (std::strcmp(value, "true") == 0 || std::strcmp(value, "True") == 0) {
        return true;
    }
    return def;
}

} // namespace

extern "C" void fuse_t3d_Con_init() {
    g_variables.clear();
    g_pathExpandos.clear();
}

extern "C" void fuse_t3d_Con_execute(const char* script) {
    fuse::log::info("[t3d] Con::execute: %s", script ? script : "(null)");
}

extern "C" void fuse_t3d_Con_executef(const char* fmt, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    fuse_t3d_Con_execute(buffer);
}

extern "C" void fuse_t3d_Con_printf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    logFormatted(fuse::log::Level::Info, fmt, args);
    va_end(args);
}

extern "C" void fuse_t3d_Con_errorf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    logFormatted(fuse::log::Level::Error, fmt, args);
    va_end(args);
}

extern "C" void fuse_t3d_Con_warnf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    logFormatted(fuse::log::Level::Warn, fmt, args);
    va_end(args);
}

extern "C" const char* fuse_t3d_Con_getVariable(const char* name) {
    if (!name) {
        return "";
    }
    const auto it = g_variables.find(normalizeVariableName(name));
    if (it == g_variables.end()) {
        return "";
    }
    return it->second.c_str();
}

extern "C" void fuse_t3d_Con_setVariable(const char* name, const char* value) {
    if (!name) {
        return;
    }
    g_variables[normalizeVariableName(name)] = value ? value : "";
}

extern "C" int fuse_t3d_Con_getIntVariable(const char* name, int def) {
    const char* value = fuse_t3d_Con_getVariable(name);
    if (!value || value[0] == '\0') {
        return def;
    }
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (end == value) {
        return def;
    }
    return static_cast<int>(parsed);
}

extern "C" void fuse_t3d_Con_setIntVariable(const char* name, int value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%d", value);
    fuse_t3d_Con_setVariable(name, buffer);
}

extern "C" int fuse_t3d_Con_getBoolVariable(const char* name, int def) {
    const char* value = fuse_t3d_Con_getVariable(name);
    return parseBool(value, def != 0) ? 1 : 0;
}

extern "C" void fuse_t3d_Con_setBoolVariable(const char* name, int value) {
    fuse_t3d_Con_setVariable(name, value ? "1" : "0");
}

extern "C" void fuse_t3d_Con_addPathExpando(const char* expandoName, const char* path) {
    if (!expandoName || !path) {
        return;
    }
    g_pathExpandos[expandoName] = path;
}

extern "C" int fuse_t3d_Con_expandPath(char* dst, unsigned size, const char* src,
                                       const char* workingDirHint, int ensureTrailingSlash) {
    if (!dst || size == 0 || !src) {
        return 0;
    }

    char pathBuffer[2048] = {};

    if (src[0] == '^') {
        const char* prefixSrc = src + 1;
        char* prefixDst = pathBuffer;
        while (*prefixSrc != '/' && *prefixSrc != '\0') {
            *prefixDst++ = *prefixSrc++;
        }
        *prefixDst = '\0';

        const auto expandoIt = g_pathExpandos.find(pathBuffer);
        if (expandoIt == g_pathExpandos.end()) {
            fuse::log::info("[t3d] Con::expandPath: missing expando '%s' for '%s'", pathBuffer, src);
            if (ensureTrailingSlash) {
                appendTrailingSlash(dst, size, src);
            } else {
                copyToBuffer(dst, size, src);
            }
            return 0;
        }

        if (*prefixSrc == '/') {
            ++prefixSrc;
        }
        std::snprintf(pathBuffer, sizeof(pathBuffer), "%s/%s", expandoIt->second.c_str(), prefixSrc);
        if (ensureTrailingSlash) {
            appendTrailingSlash(dst, size, pathBuffer);
        } else {
            copyToBuffer(dst, size, pathBuffer);
        }
        return 1;
    }

    if (src[0] == '.' && workingDirHint && workingDirHint[0] != '\0') {
        std::snprintf(pathBuffer, sizeof(pathBuffer), "%s/%s", workingDirHint, src);
        if (ensureTrailingSlash) {
            appendTrailingSlash(dst, size, pathBuffer);
        } else {
            copyToBuffer(dst, size, pathBuffer);
        }
        return 1;
    }

    if (ensureTrailingSlash) {
        appendTrailingSlash(dst, size, src);
    } else {
        copyToBuffer(dst, size, src);
    }
    return 1;
}

extern "C" void fuse_t3d_Con_collapsePath(char* dst, unsigned size, const char* src,
                                          const char* /*workingDirHint*/) {
    if (!dst || size == 0 || !src) {
        return;
    }

    for (const auto& entry : g_pathExpandos) {
        const std::string& expandoPath = entry.second;
        if (expandoPath.empty()) {
            continue;
        }
        if (std::strncmp(src, expandoPath.c_str(), expandoPath.size()) == 0) {
            const char* remainder = src + expandoPath.size();
            if (*remainder == '/') {
                ++remainder;
            }
            std::snprintf(dst, size, "^%s/%s", entry.first.c_str(), remainder);
            return;
        }
    }

    copyToBuffer(dst, size, src);
}

namespace fuse::legacy::t3d::Con {

void init() {
    fuse_t3d_Con_init();
}

void execute(const char* script) {
    fuse_t3d_Con_execute(script);
}

void executef(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char buffer[1024];
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    execute(buffer);
}

void printf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    fuse::log::Logger::instance().logV(fuse::log::Level::Info, fmt, args);
    va_end(args);
}

void errorf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    fuse::log::Logger::instance().logV(fuse::log::Level::Error, fmt, args);
    va_end(args);
}

void warnf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    fuse::log::Logger::instance().logV(fuse::log::Level::Warn, fmt, args);
    va_end(args);
}

const char* getVariable(const char* name) {
    return fuse_t3d_Con_getVariable(name);
}

void setVariable(const char* name, const char* value) {
    fuse_t3d_Con_setVariable(name, value);
}

int getIntVariable(const char* name, int def) {
    return fuse_t3d_Con_getIntVariable(name, def);
}

void setIntVariable(const char* name, int value) {
    fuse_t3d_Con_setIntVariable(name, value);
}

bool getBoolVariable(const char* name, bool def) {
    return fuse_t3d_Con_getBoolVariable(name, def ? 1 : 0) != 0;
}

void setBoolVariable(const char* name, bool value) {
    fuse_t3d_Con_setBoolVariable(name, value ? 1 : 0);
}

void addPathExpando(const char* expandoName, const char* path) {
    fuse_t3d_Con_addPathExpando(expandoName, path);
}

bool expandPath(char* dst, fuse::u32 size, const char* src, const char* workingDirHint,
                bool ensureTrailingSlash) {
    return fuse_t3d_Con_expandPath(dst, size, src, workingDirHint, ensureTrailingSlash ? 1 : 0) != 0;
}

void collapsePath(char* dst, fuse::u32 size, const char* src, const char* workingDirHint) {
    fuse_t3d_Con_collapsePath(dst, size, src, workingDirHint);
}

} // namespace fuse::legacy::t3d::Con
