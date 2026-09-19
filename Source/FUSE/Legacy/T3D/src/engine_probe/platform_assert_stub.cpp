// Minimal PlatformAssert / avar / debugBreak for FUSE_T3D_LEGACY_ENGINE_PROBE (bitmapPng.cpp).

#include "core/strings/stringFunctions.h"
#include "platform/platform.h"
#include "platform/platformAssert.h"

#include <cstdarg>

namespace Platform {

void debugBreak() {}

} // namespace Platform

bool PlatformAssert::processAssert(Type, const char*, U32, const char*) {
    return false;
}

const char* avar(const char* message, ...) {
    static char buffer[4096];
    va_list args;
    va_start(args, message);
    dVsprintf(buffer, sizeof(buffer), message, args);
    va_end(args);
    return buffer;
}
