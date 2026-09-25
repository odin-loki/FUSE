// WP-6.3 screen-space fallback on Vulkan: records, push constants and the view / vector math every ssfx
// kernel shares. GLSL twin of sx_common.slang. The C++ mirror of the records is
// include/fuse/renderer/ssfx_gpu/ssfx_gpu_types.hpp; the math is a line-for-line port of
// fuse::ssfx::SsfxCamera / SsfxGBufferView (ScreenSpace/include/fuse/ssfx/ssfx_view.hpp) and
// fuse::math::Vec3 (dot = left-to-right sum, normalized = v * (1 / sqrt(dot)) with the 1e-8 guard).
// Every arithmetic result is held in a `precise` variable so no multiply-add is contracted: the kernels
// keep the oracle's IEEE operations and order.
#ifndef FUSE_SX_COMMON_GLSL
#define FUSE_SX_COMMON_GLSL
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_samplerless_texture_functions : require
#include "bindless.glsl"

#define SX_FLAG_AO (1u << 0)
#define SX_FLAG_SSR (1u << 1)
#define SX_FLAG_SSGI (1u << 2)
#define SX_FLAG_REVERSED_Z (1u << 3)
#define SX_FLAG_MULTI_BOUNCE (1u << 4)
#define SX_FLAG_SSR_ROUGHNESS (1u << 5)
#define SX_FLAG_SSR_CONTACT (1u << 6)
#define SX_FLAG_AO_JITTER (1u << 7)
#define SX_FLAG_SKY (1u << 8)
#define SX_FLAG_SKY_GI (1u << 9)

#define SX_MAX_SLICES 32

// SsfxFrameConstants, 592 bytes.
struct SxFrame {
    uint64_t prepared;
    uint64_t normals;
    uint64_t radiance;
    uint64_t albedo;
    uint64_t diffuse;
    uint64_t ao;
    uint64_t ssr;
    uint64_t gi;
    uint64_t bounce0;
    uint64_t bounce1;
    uint64_t dump;
    uint64_t sky;
    uint width;
    uint height;
    uint inputDepth;
    uint inputNormal;
    uint inputAlbedo;
    uint inputRoughMetal;
    uint inputLit;
    uint output_;
    uint flags;
    uint reserved1;
    float fx;
    float fy;
    float cx;
    float cy;
    float nearZ;
    float nearPlane;
    float farPlane;
    float reserved2;
    float viewRot[12];
    float ambient[4];
    float aoRadius;
    float aoFalloff;
    float aoBias;
    float aoStrength;
    float aoMaxRadiusPx;
    uint aoSlices;
    uint aoSteps;
    uint aoFrame;
    float aoSliceCos[32];
    float aoSliceSin[32];
    uint ssrMaxSteps;
    uint ssrRefineSteps;
    float ssrStride;
    float ssrThickness;
    float ssrMaxDistance;
    float ssrFadeEdge;
    float ssrContactDistance;
    float ssrContactFloor;
    float ssrContactExponent;
    uint reserved3;
    uint giSampleSqrt;
    uint giBounces;
    uint giMaxSteps;
    uint giRefineSteps;
    float giStride;
    float giThickness;
    float giMaxDistance;
    float giIntensity;
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer SxFrameRef { SxFrame f; };
layout(buffer_reference, std430, buffer_reference_align = 16) buffer SxTexelsRef { vec4 v[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) buffer SxFloatsRef { float v[]; };

layout(push_constant) uniform SxPush {
    uint64_t frame;
    uint64_t src;
    uint64_t dst;
    uint mode;
    uint reserved;
} pc;

// The frame constants: every F.field is a load through the frame's BDA (a global copy trips glslang's
// `precise` propagation; the loads are uniform and cached).
#define F (SxFrameRef(pc.frame).f)

void sx_begin() {}

vec4 sx_fetch(uint handle, ivec2 p) { return texelFetch(fuse_textures_2d[fuse_handle_index(handle)], p, 0); }

// --- scalar / vector helpers (std::min / std::max / std::clamp on finite values) -----------------
float sx_saturate(float v) { return max(0.0, min(1.0, v)); }
float sx_clamp(float v, float lo, float hi) { return v < lo ? lo : (hi < v ? hi : v); }

float sx_dot(vec3 a, vec3 b) {
    precise float d = a.x * b.x + a.y * b.y + a.z * b.z;
    return d;
}
float sx_length(vec3 v) {
    precise float l = sqrt(sx_dot(v, v));
    return l;
}
vec3 sx_scale(vec3 v, float s) {
    precise vec3 r = vec3(v.x * s, v.y * s, v.z * s);
    return r;
}
vec3 sx_add(vec3 a, vec3 b) {
    precise vec3 r = vec3(a.x + b.x, a.y + b.y, a.z + b.z);
    return r;
}
vec3 sx_sub(vec3 a, vec3 b) {
    precise vec3 r = vec3(a.x - b.x, a.y - b.y, a.z - b.z);
    return r;
}
vec3 sx_normalize(vec3 v) {
    const float len = sx_length(v);
    if (len < 1e-8) {
        return vec3(0.0);
    }
    precise float inv = 1.0 / len;
    return sx_scale(v, inv);
}
vec3 sx_cross(vec3 a, vec3 b) {
    precise vec3 r = vec3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
    return r;
}
vec3 sx_neg(vec3 v) { return sx_scale(v, -1.0); }

// --- SsfxCamera / SsfxGBufferView over the prepared sections ------------------------------------
uint sx_index(uint x, uint y) { return y * F.width + x; }
float sx_depth_at(uint x, uint y) { return SxTexelsRef(F.prepared).v[sx_index(x, y)].x; }
vec3 sx_normal_at(uint x, uint y) { return SxTexelsRef(F.normals).v[sx_index(x, y)].xyz; }
vec3 sx_color_at(uint64_t address, uint i) { return SxTexelsRef(address).v[i].xyz; }

// The view / hash / basis helpers below are compiled only by the kernels that use them (SX_VIEW_HELPERS):
// glslang's `precise` propagation fails (unbounded allocation) on unused functions with precise locals.
#ifdef SX_VIEW_HELPERS
vec3 sx_ray_direction(float px, float py) {
    precise float a = (px - F.cx) / F.fx;
    precise float b = (py - F.cy) / F.fy;
    return vec3(a, b, 1.0);
}
vec3 sx_unproject(float px, float py, float viewZ) { return sx_scale(sx_ray_direction(px, py), viewZ); }
vec3 sx_position_at(uint x, uint y) {
    precise float px = float(x) + 0.5;
    precise float py = float(y) + 0.5;
    return sx_unproject(px, py, sx_depth_at(x, y));
}
bool sx_project(vec3 p, out float px, out float py) {
    px = 0.0;
    py = 0.0;
    if (p.z < F.nearZ) {
        return false;
    }
    precise float x = F.fx * p.x / p.z + F.cx;
    precise float y = F.fy * p.y / p.z + F.cy;
    px = x;
    py = y;
    return true;
}
bool sx_inside(float px, float py) { return px >= 0.0 && py >= 0.0 && px < float(F.width) && py < float(F.height); }

// Unit normal oriented towards the camera (hbao_kernel::facing_normal / the kernels' inline form).
vec3 sx_facing_normal(uint x, uint y, vec3 p) {
    vec3 n = sx_normalize(sx_normal_at(x, y));
    if (sx_dot(n, p) > 0.0) {
        n = sx_neg(n);
    }
    return n;
}

// Integer hash (lowbias32) -> uniform [0, 1) (ssgi_kernel::hash_unit / gtao_kernel::hash_unit).
float sx_hash_unit(uint v) {
    v ^= v >> 16;
    v *= 0x7feb352du;
    v ^= v >> 15;
    v *= 0x846ca68bu;
    v ^= v >> 16;
    precise float r = float(v >> 8) * (1.0 / 16777216.0);
    return r;
}

// hbao_kernel::tangent_basis / gtao_kernel::tangent_basis.
void sx_tangent_basis(vec3 n, out vec3 t, out vec3 b) {
    const vec3 helper = abs(n.x) < 0.9 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0);
    t = sx_normalize(sx_cross(helper, n));
    b = sx_cross(n, t);
}
#endif // SX_VIEW_HELPERS
#endif
