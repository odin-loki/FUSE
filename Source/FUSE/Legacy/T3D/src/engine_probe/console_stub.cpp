// Minimal Con:: logging for FUSE_T3D_LEGACY_ENGINE_PROBE (bitmapSTB.cpp).
#include "console/console.h"

#include <cstdio>
#include <cstdarg>

namespace Con {

void printf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stdout, fmt, args);
    std::fprintf(stdout, "\n");
    va_end(args);
}

void errorf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    std::fprintf(stderr, "\n");
    va_end(args);
}

} // namespace Con
