// FUSE visibility buffer (WP-1.4): the pixel formats, Slang twin of vis_format.glsl and of
// include/fuse/renderer/visbuffer/vis_format.hpp (see there).
#pragma once

static const uint kFuseVisInvalid = 0xFFFFFFFFu;
static const uint kFuseVis64InstanceShift = 20u;
static const uint kFuseVis64DepthShift = 40u;
static const uint kFuseVis64IdMask = 0xFFFFFu;
static const uint kFuseVis64DepthMax = 0xFFFFFFu;
static const uint64_t kFuseVis64Clear = 0xFFFFFFFFFFFFFFFFull;

uint fuseVis64QuantizeDepth(float depth) {
    if (!(depth > 0.0)) {
        return 0u;
    }
    if (depth >= 1.0) {
        return kFuseVis64DepthMax;
    }
    return min(uint(depth * 16777216.0), kFuseVis64DepthMax);
}

uint64_t fuseVis64Pack(float depth, uint instance, uint triangle) {
    return (uint64_t(fuseVis64QuantizeDepth(depth)) << kFuseVis64DepthShift) |
           (uint64_t(instance & kFuseVis64IdMask) << kFuseVis64InstanceShift) | uint64_t(triangle & kFuseVis64IdMask);
}

uint fuseVis64Instance(uint64_t v) { return uint(v >> kFuseVis64InstanceShift) & kFuseVis64IdMask; }
uint fuseVis64Triangle(uint64_t v) { return uint(v) & kFuseVis64IdMask; }
uint fuseVis64DepthBits(uint64_t v) { return uint(v >> kFuseVis64DepthShift); }
bool fuseVis64Valid(uint64_t v) { return fuseVis64Instance(v) != kFuseVis64IdMask; }

uint2 fuseVis64ToRaster(uint64_t v) {
    return fuseVis64Valid(v) ? uint2(fuseVis64Instance(v), fuseVis64Triangle(v)) : uint2(kFuseVisInvalid, kFuseVisInvalid);
}

float fuseVis64ExportDepth(uint64_t v) {
    if (!fuseVis64Valid(v)) {
        return 1.0;
    }
    return min(float(fuseVis64DepthBits(v) + 1u) * (1.0 / 16777216.0), 1.0);
}
