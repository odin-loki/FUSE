// Minimal Torque platform symbols for FUSE_T3D_LEGACY_ENGINE_PROBE (bitmapUtils.cpp).
// Satisfies dMem* and Float_Inf without pulling full platform/*.cpp closure.

#include "platform/types.h"

#include <cmath>
#include <cstring>
#include <limits>

const F32 Float_Inf = std::numeric_limits<F32>::infinity();

void* dMemcpy(void* dst, const void* src, dsize_t size) {
    return std::memcpy(dst, src, size);
}

void* dMemmove(void* dst, const void* src, dsize_t size) {
    return std::memmove(dst, src, size);
}

void* dMemset(void* dst, S32 c, dsize_t size) {
    return std::memset(dst, c, size);
}

S32 dMemcmp(const void* ptr1, const void* ptr2, dsize_t size) {
    return static_cast<S32>(std::memcmp(ptr1, ptr2, size));
}
