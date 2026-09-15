#include <fuse/legacy/t3d/api.hpp>

#include <cstdarg>
#include <cstdio>

// C-linkage exports use the fuse_t3d_ prefix so both legacy libs can link in one process.
extern "C" void fuse_t3d_Con_execute(const char* script) {
    std::fprintf(stderr, "[fuse_t3d_legacy] Con::execute: %s\n", script ? script : "(null)");
}

extern "C" void fuse_t3d_Con_printf(const char* fmt, ...) {
    std::fprintf(stderr, "[fuse_t3d_legacy] Con::printf: ");
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
    std::fputc('\n', stderr);
}

namespace fuse::legacy::t3d::Con {

void execute(const char* script) {
    fuse_t3d_Con_execute(script);
}

void printf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::fprintf(stderr, "[fuse_t3d_legacy] Con::printf: ");
    std::vfprintf(stderr, fmt, args);
    va_end(args);
    std::fputc('\n', stderr);
}

} // namespace fuse::legacy::t3d::Con
