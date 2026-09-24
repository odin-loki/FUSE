// FUSE Relight RL-5.1: the path-tracing core in GLSL, the fallback for hosts with glslangValidator only
// (docs/plans/FUSE_REMIX_PORT_PLAN.md §5.1). Same single-source core as rl_pt.slang and the CPU reference
// (Relight/kernels/pt_reference_core.h); this file sets the GLSL dialect and the scene accessors:
//   traversal  one ray query per trace on the WP-6.0 TLAS (device address), gl_RayFlagsOpaqueEXT (the instances are
//              FORCE_OPAQUE; legacy alpha test is the core's re-trace loop);
//   tables     buffer device addresses (push constants): instance / triangle / material / portal words, light map;
//   lights     RL-4.4 rl_lights.glsl (the table + the WP-7.1 tree: rlSetSample / rlSetPdf / rlLoadLight);
//   BSDF       RL-4.3 bsdf.glsl, albedo table by device address;
//   textures   the WP-0.4 bindless heap (set 0: sampled images, samplers), LOD 0.
// The includer declares the push-constant block `pc` (PtTracePush, pt_gpu.hpp) before including this file. Needs
// -I <Relight>/kernels, <Relight>/shaders/material, <Relight>/render/lights/shaders, <Renderer>/shaders/light_tree,
// <Renderer>/shaders/common.
#ifndef FUSE_RELIGHT_PT_GLSL
#define FUSE_RELIGHT_PT_GLSL

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer PtWordsRef { vec4 v[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer PtFloatsRef { float v[]; };

#define FUSE_BSDF_LUT_READ(i) PtFloatsRef(pc.lut).v[(i)]
#include "bsdf.glsl"
#include "rl_lights.glsl"
#include "bindless.glsl"

#define PT_FN
#define PT_CONST const
#define PT_OUT(T) out T
#define PT_INOUT(T) inout T
#define PT_CTX_PARAM
#define PT_CTX_ARG
#define PT_LUT_ARG
#define PT_L3(v) (v)
#define PT_B3(v) (v)
#define PT_PARAM_WORDS(name) vec4 name[11]

#include "pt_reference_types.h"

PtRawHit ptRawTrace(vec3 o, vec3 d, float tmin, float tmax, uint cullMask) {
    PtRawHit h;
    h.hit = false;
    h.t = 0.0;
    h.instance = kPtInvalid;
    h.primitive = kPtInvalid;
    h.u = 0.0;
    h.v = 0.0;
    rayQueryEXT q;
    rayQueryInitializeEXT(q, accelerationStructureEXT(pc.tlas), gl_RayFlagsOpaqueEXT, cullMask & 0x7Fu, o, tmin, d,
                          tmax);
    while (rayQueryProceedEXT(q)) {
    }
    if (rayQueryGetIntersectionTypeEXT(q, true) == gl_RayQueryCommittedIntersectionTriangleEXT) {
        vec2 bary = rayQueryGetIntersectionBarycentricsEXT(q, true);
        h.hit = true;
        h.t = rayQueryGetIntersectionTEXT(q, true);
        h.instance = uint(rayQueryGetIntersectionInstanceCustomIndexEXT(q, true));
        h.primitive = uint(rayQueryGetIntersectionPrimitiveIndexEXT(q, true));
        h.u = bary.x;
        h.v = bary.y;
    }
    return h;
}

vec4 ptInstanceWord(uint slot, uint k) { return PtWordsRef(pc.instances).v[slot * kPtInstanceWords + k]; }
vec4 ptTriangleWord(uint tri, uint k) { return PtWordsRef(pc.triangles).v[tri * kPtTriangleWords + k]; }
vec4 ptMaterialWord(uint material, uint k) { return PtWordsRef(pc.materials).v[material * kPtMaterialWords + k]; }
vec4 ptPortalWord(uint portal, uint k) { return PtWordsRef(pc.portals).v[portal * kPtPortalWords + k]; }
float ptLightMapValue(uint index) { return PtFloatsRef(pc.lightMap).v[index]; }

vec4 ptTextureSample(uint texture, uint smp, float u, float v) {
    if (!fuse_handle_valid(texture) || !fuse_handle_valid(smp)) {
        return vec4(1.0);
    }
    return textureLod(FUSE_TEXTURE_2D(texture, smp), vec2(u, v), 0.0);
}

RlLight ptLoadLight(uint light) { return rlLoadLight(pc.lightTable, light); }

PtLightPick ptSampleLightSet(vec3 p, vec3 n, float u0, float u1, float u2) {
    PtLightPick pick;
    pick.light = kPtInvalid;
    pick.pdf = 0.0;
    pick.wi = vec3(0.0, 0.0, 1.0);
    pick.dist = 0.0;
    pick.radiance = vec3(0.0);
    pick.delta = false;
    RlSetSample s = rlSetSample(pc.lightTable, pc.lightCount, pc.lightTree, p, n, u0, u1, u2);
    if (s.light == LT_INVALID || s.light >= pc.lightCount || (s.s.flags & kRlSampleValid) == 0u) {
        return pick;
    }
    pick.light = s.light;
    pick.pdf = s.pdf;
    pick.wi = s.s.wi;
    pick.dist = s.s.dist;
    pick.radiance = s.s.radiance;
    pick.delta = (s.s.flags & kRlSampleDelta) != 0u;
    return pick;
}

float ptLightSetPdf(vec3 p, vec3 n, uint light, vec3 wi) {
    return rlSetPdf(pc.lightTable, pc.lightCount, pc.lightTree, p, n, light, wi);
}

#include "pt_reference_core.h"

#endif // FUSE_RELIGHT_PT_GLSL
