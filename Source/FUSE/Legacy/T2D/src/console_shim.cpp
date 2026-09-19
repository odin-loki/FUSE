#include <fuse/legacy/t2d/api.hpp>
#include <fuse/log/logger.hpp>

#include <cstdarg>
#include <cstdio>
#include <string>
#include <unordered_map>

namespace {

std::unordered_map<std::string, std::string> g_variables;

void logFormatted(fuse::log::Level level, const char* fmt, va_list args) {
    fuse::log::Logger::instance().logV(level, fmt, args);
}

} // namespace

extern "C" void fuse_t2d_Con_init() {
    g_variables.clear();
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
    const auto it = g_variables.find(name);
    if (it == g_variables.end()) {
        return "";
    }
    return it->second.c_str();
}

extern "C" void fuse_t2d_Con_setVariable(const char* name, const char* value) {
    if (!name) {
        return;
    }
    g_variables[name] = value ? value : "";
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

} // namespace fuse::legacy::t2d::Con
