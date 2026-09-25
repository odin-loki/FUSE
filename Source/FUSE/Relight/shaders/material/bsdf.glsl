// FUSE Relight RL-4.3: the Relight BSDF in GLSL, the fallback for hosts with glslangValidator only
// (docs/plans/FUSE_REMIX_PORT_PLAN.md §5.2). Same single-source core as bsdf.slang and the CPU reference
// (Relight/kernels/bsdf_core.h); this file sets the GLSL dialect. Needs GL_GOOGLE_include_directive and
// -I <Relight>/kernels.
//
// The includer declares the albedo table and defines FUSE_BSDF_LUT_READ(i) before including this file, e.g.
//   layout(std430, set = 0, binding = 1) readonly buffer FuseBsdfLut { float v[]; } g_bsdfLut;
//   #define FUSE_BSDF_LUT_READ(i) g_bsdfLut.v[(i)]
//   #include "bsdf.glsl"
#ifndef FUSE_RELIGHT_BSDF_GLSL
#define FUSE_RELIGHT_BSDF_GLSL

#ifndef FUSE_BSDF_LUT_READ
#error "define FUSE_BSDF_LUT_READ(i) (the albedo table) before including bsdf.glsl"
#endif

#define float3 vec3
#define float4 vec4
#define FUSE_BSDF_FN
#define FUSE_BSDF_CONST const
#define FUSE_BSDF_OUT(T) out T
#define FUSE_BSDF_LUT_PARAM
#define FUSE_BSDF_LUT_ARG
#define FUSE_BSDF_WORDS_PARAM(name) vec4 name[11]

#include "bsdf_core.h"

#endif // FUSE_RELIGHT_BSDF_GLSL
