#include <fuse/legacy/t3d/api.hpp>
#include <fuse/log/logger.hpp>

#include <cstdarg>
#include <cstdio>

// C-linkage exports use the fuse_t3d_ prefix so both legacy libs can link in one process.
extern "C" void fuse_t3d_Con_execute(const char* script) {
    fuse::log::info("[t3d] Con::execute: %s", script ? script : "(null)");
}

extern "C" void fuse_t3d_Con_printf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    fuse::log::Logger::instance().logV(fuse::log::Level::Info, fmt, args);
    va_end(args);
}

namespace fuse::legacy::t3d::Con {

void execute(const char* script) {
    fuse_t3d_Con_execute(script);
}

void printf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    fuse::log::Logger::instance().logV(fuse::log::Level::Info, fmt, args);
    va_end(args);
}

} // namespace fuse::legacy::t3d::Con
