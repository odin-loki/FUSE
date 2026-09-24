// FUSE Relight RL-4.2: raster remaster shader interface (docs/plans/FUSE_REMIX_PORT_PLAN.md §5.9).
// Mirrors Relight/render/raster/include/fuse/relight/render/raster/raster_scene.hpp (RasterVertex, RasterMaterial,
// RasterDrawGpu, RasterFrameGpu; the C++ side pins every offset). Buffers are reached by device address (push
// constants and the frame record); textures, samplers and the WP-1.1 GPU scene through the WP-0.4 bindless heap
// (set 0). Needs GL_GOOGLE_include_directive and -I <Renderer>/shaders/common -I <Renderer>/include/fuse/renderer/gpu_scene.
#ifndef FUSE_RELIGHT_RASTER_COMMON_GLSL
#define FUSE_RELIGHT_RASTER_COMMON_GLSL

#extension GL_EXT_buffer_reference : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

#include "bindless.glsl"
#include "gpu_scene.glsl"

struct RasterVertex {
    vec3 pos;
    uint color; // RGBA8, r in the low byte
    vec3 normal;
    uint pad0;
    vec2 uv;
    uint pad1;
    uint pad2;
};

struct RasterMaterial {
    vec4 diffuse;
    vec4 emissive;
    vec4 specular;
    vec4 tfactor;
    uint texture;
    uint samplerHandle;
    uint ops;
    uint flags;
    uint alphaTest;
    float metallic;
    float roughness;
    uint pad;
};

struct RasterDraw {
    mat4 objectToWorld; // D3D row-major memory read column-major: M * v == v x M(D3D)
    mat4 worldToClip;
    mat4 normalToWorld;
    RasterMaterial mat;
};

struct RasterFrame {
    vec4 eye;
    vec4 forward;      // xyz, w near
    vec4 cluster;      // tiles x, tiles y, slices, far
    vec4 screen;       // width, height, 1 / width, 1 / height
    vec4 fogColor;     // rgb display, w D3DFOGMODE
    vec4 fogParams;    // 1 / (end - start), end, density, enabled
    vec4 ambient;      // rgb linear, w exposure
    vec4 clearColor;
    vec4 fallbackDir;  // w enabled
    vec4 fallbackColor;
    mat4 shadowMatrix;
    vec4 shadowParams; // texel size, depth bias, texel size in world units, enabled
    uvec4 counts;      // directional, clusters, shadow light slot, features
    uvec4 gbuffer;     // albedo, normal, emissive, position
    uvec4 handles;     // -, shadow map, nearest sampler, GPU-scene header
    uint64_t grid;
    uint64_t lightList;
    uint64_t directional;
    uint64_t lut;
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer RasterVerticesRef { RasterVertex v[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer RasterDrawsRef { RasterDraw v[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer RasterFrameRef { RasterFrame f; };
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer RasterUintsRef { uint v[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer RasterFloatsRef { float v[]; };

// Passes (RasterPush::pass).
#define RASTER_PASS_GBUFFER 0u
#define RASTER_PASS_DECAL 1u
#define RASTER_PASS_FORWARD 2u
#define RASTER_PASS_SHADOW 3u
#define RASTER_PASS_LIGHT 4u

layout(push_constant) uniform RasterPush {
    uint64_t vertices;
    uint64_t draws;
    uint64_t frame;
    uint drawIndex;
    uint pass;
} pc;

// RasterMaterial::flags
#define RASTER_MAT_UNLIT 1u
#define RASTER_MAT_HAS_NORMALS 2u
#define RASTER_MAT_VERTEX_COLOR 4u
#define RASTER_MAT_ALPHA_TEST 8u
#define RASTER_MAT_REPLACEMENT 16u
#define RASTER_MAT_TEXTURED 32u
#define RASTER_MAT_FOG 64u
#define RASTER_MAT_CAST_SHADOW 128u

// RasterFrame::counts.w (RasterFeature)
#define RASTER_FEATURE_FOG 16u
#define RASTER_FEATURE_SHADOWS 32u

vec3 rasterToLinear(vec3 c) { return pow(max(c, vec3(0.0)), vec3(2.2)); }
vec3 rasterToDisplay(vec3 c) { return pow(clamp(c, vec3(0.0), vec3(1.0)), vec3(1.0 / 2.2)); }

#endif // FUSE_RELIGHT_RASTER_COMMON_GLSL
