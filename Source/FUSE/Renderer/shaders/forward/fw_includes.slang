// WP-2.3: the include chain of the forward-transparency Slang shaders (twin of fw_includes.glsl).
#pragma once
#include "gpu_scene.slang"
#include "vis_format.slang"
#include "vis_common.slang"
#include "mr_gbuffer.slang"
#include "mr_common.slang"
#include "lc_common.slang"
#include "lc_ltc.slang" // WP-2.2 compensated BRDF + area lights (light.shade uses them too)
#include "fw_common.slang"

struct FwPush {
    uint64_t frame; // BDA of ForwardFrameConstants
    uint64_t reserved;
};
[[vk::push_constant]] ConstantBuffer<FwPush> pc;

FwFrame fuseFwFrame() { return ((FwFrame*)pc.frame)[0]; }
