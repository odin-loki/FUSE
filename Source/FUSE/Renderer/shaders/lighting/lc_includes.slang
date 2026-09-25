// WP-2.1: the include chain every clustered-lighting Slang kernel uses (twin of lc_includes.glsl).
#pragma once
#include "gpu_scene.slang"
#include "lc_common.slang"
#include "lc_ltc.slang" // WP-2.2 compensated BRDF + area lights

struct LcPush {
    uint64_t frame; // BDA of LightingFrameConstants
    uint64_t out_;  // light.shade: f32x4 radiance dump, else 0
};
[[vk::push_constant]] ConstantBuffer<LcPush> pc;

LcFrame fuseLcFrame() { return ((LcFrame*)pc.frame)[0]; }
uint* fuseLcWords(uint64_t address) { return (uint*)address; }
