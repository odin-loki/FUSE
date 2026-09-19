#include <fuse/legacy/t2d/api.hpp>
#include <fuse/log/logger.hpp>

#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace {

std::unordered_map<std::string, std::string> g_variables;
std::unordered_map<std::string, std::string> g_pathExpandos;

struct BoundVariable {
    int type = 0;
    void* ptr = nullptr;
};

std::unordered_map<std::string, BoundVariable> g_bindings;
std::unordered_set<std::string> g_functionNames;

thread_local char g_dataBuffer[512];

bool parseBool(const char* value, bool def);

int typeElementSize(int type) {
    switch (type) {
    case fuse::legacy::t2d::DynamicType::Bool:
        return static_cast<int>(sizeof(bool));
    case fuse::legacy::t2d::DynamicType::S32:
        return static_cast<int>(sizeof(int));
    case fuse::legacy::t2d::DynamicType::F32:
        return static_cast<int>(sizeof(float));
    default:
        return 0;
    }
}

const char* formatDataValue(int type, void* dptr, int index) {
    if (!dptr || typeElementSize(type) == 0) {
        g_dataBuffer[0] = '\0';
        return g_dataBuffer;
    }

    const char* base = static_cast<const char*>(dptr) + static_cast<std::size_t>(index) *
                                                              static_cast<std::size_t>(typeElementSize(type));

    switch (type) {
    case fuse::legacy::t2d::DynamicType::Bool: {
        const bool value = *reinterpret_cast<const bool*>(base);
        std::snprintf(g_dataBuffer, sizeof(g_dataBuffer), "%s", value ? "1" : "0");
        break;
    }
    case fuse::legacy::t2d::DynamicType::S32: {
        const int value = *reinterpret_cast<const int*>(base);
        std::snprintf(g_dataBuffer, sizeof(g_dataBuffer), "%d", value);
        break;
    }
    case fuse::legacy::t2d::DynamicType::F32: {
        const float value = *reinterpret_cast<const float*>(base);
        std::snprintf(g_dataBuffer, sizeof(g_dataBuffer), "%g", static_cast<double>(value));
        break;
    }
    default:
        g_dataBuffer[0] = '\0';
        break;
    }
    return g_dataBuffer;
}

void applyDataValue(int type, void* dptr, int index, const char* value) {
    if (!dptr || typeElementSize(type) == 0) {
        return;
    }

    char* base = static_cast<char*>(dptr) + static_cast<std::size_t>(index) *
                                                static_cast<std::size_t>(typeElementSize(type));

    switch (type) {
    case fuse::legacy::t2d::DynamicType::Bool:
        *reinterpret_cast<bool*>(base) = parseBool(value, false);
        break;
    case fuse::legacy::t2d::DynamicType::S32: {
        char* end = nullptr;
        const long parsed = std::strtol(value ? value : "0", &end, 10);
        *reinterpret_cast<int*>(base) = static_cast<int>(parsed);
        break;
    }
    case fuse::legacy::t2d::DynamicType::F32: {
        char* end = nullptr;
        const float parsed = static_cast<float>(std::strtod(value ? value : "0", &end));
        *reinterpret_cast<float*>(base) = parsed;
        break;
    }
    default:
        break;
    }
}

void syncBindingToVariable(const std::string& key) {
    const auto bindingIt = g_bindings.find(key);
    if (bindingIt == g_bindings.end()) {
        return;
    }
    g_variables[key] = formatDataValue(bindingIt->second.type, bindingIt->second.ptr, 0);
}

void registerFunctionName(const char* name) {
    if (!name || name[0] == '\0') {
        return;
    }
    g_functionNames.emplace(name);
}

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

extern "C" void fuse_t2d_Con_init() {
    g_variables.clear();
    g_pathExpandos.clear();
    g_bindings.clear();
    g_functionNames.clear();
}

extern "C" void fuse_t2d_Con_execute(const char* script) {
    fuse::log::info("[t2d] Con::execute: %s", script ? script : "(null)");
}

extern "C" void fuse_t2d_Con_executef(const char* fmt, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    fuse_t2d_Con_execute(buffer);
}

extern "C" void fuse_t2d_Con_printf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    logFormatted(fuse::log::Level::Info, fmt, args);
    va_end(args);
}

extern "C" void fuse_t2d_Con_errorf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    logFormatted(fuse::log::Level::Error, fmt, args);
    va_end(args);
}

extern "C" void fuse_t2d_Con_warnf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    logFormatted(fuse::log::Level::Warn, fmt, args);
    va_end(args);
}

extern "C" const char* fuse_t2d_Con_getVariable(const char* name) {
    if (!name) {
        return "";
    }
    const std::string key = normalizeVariableName(name);
    const auto bindingIt = g_bindings.find(key);
    if (bindingIt != g_bindings.end()) {
        return formatDataValue(bindingIt->second.type, bindingIt->second.ptr, 0);
    }
    const auto it = g_variables.find(key);
    if (it == g_variables.end()) {
        return "";
    }
    return it->second.c_str();
}

extern "C" void fuse_t2d_Con_setVariable(const char* name, const char* value) {
    if (!name) {
        return;
    }
    const std::string key = normalizeVariableName(name);
    g_variables[key] = value ? value : "";
    const auto bindingIt = g_bindings.find(key);
    if (bindingIt != g_bindings.end()) {
        applyDataValue(bindingIt->second.type, bindingIt->second.ptr, 0, value);
    }
}

extern "C" int fuse_t2d_Con_getIntVariable(const char* name, int def) {
    const char* value = fuse_t2d_Con_getVariable(name);
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

extern "C" void fuse_t2d_Con_setIntVariable(const char* name, int value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%d", value);
    fuse_t2d_Con_setVariable(name, buffer);
}

extern "C" int fuse_t2d_Con_getBoolVariable(const char* name, int def) {
    const char* value = fuse_t2d_Con_getVariable(name);
    return parseBool(value, def != 0) ? 1 : 0;
}

extern "C" void fuse_t2d_Con_setBoolVariable(const char* name, int value) {
    fuse_t2d_Con_setVariable(name, value ? "1" : "0");
}

extern "C" int fuse_t2d_Con_addVariable(const char* name, int type, void* pointer) {
    if (!name || !pointer) {
        return 0;
    }
    const std::string key = normalizeVariableName(name);
    g_bindings[key] = BoundVariable{type, pointer};
    syncBindingToVariable(key);
    return 1;
}

extern "C" void fuse_t2d_Con_setData(int type, void* dptr, int index, int argc, const char** argv,
                                     const void* /*tbl*/, unsigned /*flag*/) {
    if (!dptr || argc <= 0 || !argv || !argv[0]) {
        return;
    }
    applyDataValue(type, dptr, index, argv[0]);
}

extern "C" const char* fuse_t2d_Con_getData(int type, void* dptr, int index, const void* /*tbl*/,
                                            unsigned /*flag*/) {
    return formatDataValue(type, dptr, index);
}

extern "C" int fuse_t2d_Con_isFunction(const char* fn) {
    if (!fn || fn[0] == '\0') {
        return 0;
    }
    return g_functionNames.find(fn) != g_functionNames.end() ? 1 : 0;
}

extern "C" void fuse_t2d_Con_registerFunction(const char* fn) {
    registerFunctionName(fn);
}

extern "C" void fuse_t2d_Con_threadSafeExecute(const char* script) {
    fuse_t2d_Con_execute(script);
}

extern "C" void fuse_t2d_Con_addPathExpando(const char* expandoName, const char* path) {
    if (!expandoName || !path) {
        return;
    }
    g_pathExpandos[expandoName] = path;
}

extern "C" int fuse_t2d_Con_expandPath(char* dst, unsigned size, const char* src,
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
            fuse::log::info("[t2d] Con::expandPath: missing expando '%s' for '%s'", pathBuffer, src);
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

extern "C" void fuse_t2d_Con_collapsePath(char* dst, unsigned size, const char* src,
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

namespace fuse::legacy::t2d::Con {

void init() {
    fuse_t2d_Con_init();
}

void execute(const char* script) {
    fuse_t2d_Con_execute(script);
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
    return fuse_t2d_Con_getVariable(name);
}

void setVariable(const char* name, const char* value) {
    fuse_t2d_Con_setVariable(name, value);
}

int getIntVariable(const char* name, int def) {
    return fuse_t2d_Con_getIntVariable(name, def);
}

void setIntVariable(const char* name, int value) {
    fuse_t2d_Con_setIntVariable(name, value);
}

bool getBoolVariable(const char* name, bool def) {
    return fuse_t2d_Con_getBoolVariable(name, def ? 1 : 0) != 0;
}

void setBoolVariable(const char* name, bool value) {
    fuse_t2d_Con_setBoolVariable(name, value ? 1 : 0);
}

bool addVariable(const char* name, int type, void* pointer) {
    return fuse_t2d_Con_addVariable(name, type, pointer) != 0;
}

void setData(int type, void* dptr, int index, int argc, const char** argv) {
    fuse_t2d_Con_setData(type, dptr, index, argc, argv, nullptr, 0);
}

const char* getData(int type, void* dptr, int index) {
    return fuse_t2d_Con_getData(type, dptr, index, nullptr, 0);
}

bool isFunction(const char* fn) {
    return fuse_t2d_Con_isFunction(fn) != 0;
}

void threadSafeExecute(const char* script) {
    fuse_t2d_Con_threadSafeExecute(script);
}

void addPathExpando(const char* expandoName, const char* path) {
    fuse_t2d_Con_addPathExpando(expandoName, path);
}

bool expandPath(char* dst, fuse::u32 size, const char* src, const char* workingDirHint,
                bool ensureTrailingSlash) {
    return fuse_t2d_Con_expandPath(dst, size, src, workingDirHint, ensureTrailingSlash ? 1 : 0) != 0;
}

void collapsePath(char* dst, fuse::u32 size, const char* src, const char* workingDirHint) {
    fuse_t2d_Con_collapsePath(dst, size, src, workingDirHint);
}

} // namespace fuse::legacy::t2d::Con
