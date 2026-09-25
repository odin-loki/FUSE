// WP-1.5: the include chain every material-resolve Slang kernel uses (twin of mr_includes.glsl).
#pragma once
#include "gpu_scene.slang"
#include "vis_format.slang"
#include "vis_common.slang"
#include "mr_gbuffer.slang"
#include "mr_common.slang"
#include "mr_layered.slang"

struct MrPush {
    uint64_t frame; // BDA of ResolveFrameConstants
    uint64_t out_;  // attribute dump output (ResolveAttributeTexel[]), else 0
};
[[vk::push_constant]] ConstantBuffer<MrPush> pc;

MrFrame fuseMrFrame() { return ((MrFrame*)pc.frame)[0]; }
