// WP-3.1: the include chain of every vsm.* Slang kernel (twin of vsm_includes.glsl).
#pragma once
#include "gpu_scene.slang"
#include "vsm_common.slang"

struct VsmPush {
    uint64_t constants; // BDA of VsmFrameConstants
    uint kernelArg;
    uint pad;
};
[[vk::push_constant]] ConstantBuffer<VsmPush> pc;

#ifdef FUSE_VSM_COHERENT_WORDS
[[vk::binding(3, 0)]] globallycoherent RWStructuredBuffer<uint> fuse_vsm_words[];
#else
[[vk::binding(3, 0)]] RWStructuredBuffer<uint> fuse_vsm_words[];
#endif

VsmConstants* fuseVsmConstants() { return (VsmConstants*)pc.constants; }
