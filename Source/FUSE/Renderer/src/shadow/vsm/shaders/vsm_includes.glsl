// WP-3.1: the include chain of every vsm.* GLSL kernel (bindless heap, GPU scene, VSM records and math)
// + the push constants (VsmPush) and the bindless word-buffer array (atomics). Twin: vsm_includes.slang.
#ifndef FUSE_VSM_INCLUDES_GLSL
#define FUSE_VSM_INCLUDES_GLSL
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : require
#include "bindless.glsl"
#include "gpu_scene.glsl"
#include "vsm_common.glsl"

layout(push_constant) uniform FuseVsmPush {
    uint64_t constants; // BDA of VsmFrameConstants
    uint kernelArg;
    uint pad;
} pc;

#ifdef FUSE_VSM_COHERENT_WORDS
FUSE_BINDLESS_SSBO_LAYOUT coherent buffer FuseVsmWords { uint w[]; } fuse_vsm_words[];
#else
FUSE_BINDLESS_SSBO_LAYOUT buffer FuseVsmWords { uint w[]; } fuse_vsm_words[];
#endif

FuseVsmConstantsRef fuse_vsm_constants() { return FuseVsmConstantsRef(pc.constants); }
#define FUSE_VSM_WORDS(handle) fuse_vsm_words[fuse_handle_index(handle)].w
#endif
