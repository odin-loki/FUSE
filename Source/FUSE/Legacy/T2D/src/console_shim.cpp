#include <fuse/legacy/t2d/api.hpp>
#include <fuse/log/logger.hpp>

#include <cstdarg>
#include <cstdio>

extern "C" void fuse_t2d_Con_execute(const char* script) {
    fuse::log::info("[t2d] Con::execute: %s", script ? script : "(null)");
}

extern "C" void fuse_t2d_Con_printf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    fuse::log::Logger::instance().logV(fuse::log::Level::Info, fmt, args);
    va_end(args);
}

namespace fuse::legacy::t2d::Con {

void execute(const char* script) {
    fuse_t2d_Con_execute(script);
}

void printf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    fuse::log::Logger::instance().logV(fuse::log::Level::Info, fmt, args);
    va_end(args);
}

} // namespace fuse::legacy::t2d::Con
