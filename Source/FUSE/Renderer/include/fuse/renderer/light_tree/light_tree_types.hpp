#pragma once

// WP-7.1 light tree: the records shared by the C++ host code, the single-source CPU sampler
// (light_tree_kernel.hpp) and the GPU kernels (shaders/light_tree/lt_common.{glsl,slang} declare the same
// fields in the same order; fuse_rp_light_tree_layout checks names, order, offsets and sizes).
//
// Layout rules (as gpu_scene_types.hpp): 4-byte scalars, fixed float arrays and 8-byte addresses only, no
// vec3 members, no implicit padding, every record a multiple of 16 bytes, so the tables compare with memcmp
// and the GPU reads them through buffer device addresses with std430 == scalar layout.
//
// Device-safe (only <fuse/types.hpp>).

#include <fuse/types.hpp>

#include <cstddef>

namespace fuse::renderer::light_tree {

inline constexpr u32 kLtInvalid = 0xFFFFFFFFu;
/// Deepest leaf (root = depth 0). The bit trail of a light is a u32 whose bit d is the branch taken at
/// depth d (0 = first child, 1 = second child), so depth <= 31. The builder switches from the SAH split to
/// a count-median split wherever the SAH could exceed it.
inline constexpr u32 kLtMaxDepth = 31u;
/// Largest float below 1 (0x1.fffffep-1): the remapped random number stays in [0, 1).
inline constexpr f32 kLtOneMinusEpsilon = 0.99999994f;
/// Squared-distance floor of the importance (a point light at the shading point).
inline constexpr f32 kLtMinDistance2 = 1e-12f;

/// Emitter kinds (LightTreeEmitter::kind). Directional lights are not in the tree: they are chosen
/// uniformly with probability dirCount / (dirCount + 1) (1 when the tree is empty), PBRT-v4's split between
/// infinite lights and the light BVH.
enum LtKind : u32 {
    kLtKindNone = 0,
    kLtKindPoint = 1,       ///< p0 = position
    kLtKindSpot = 2,        ///< p0 = position, normal = axis
    kLtKindRect = 3,        ///< p0 = centre, e1 / e2 = half axes, normal = e1 x e2 (lit side)
    kLtKindDisk = 4,        ///< p0 = centre, e1 / e2 = radii (ellipse half axes), normal = e1 x e2
    kLtKindTriangle = 5,    ///< p0 = v0, e1 = v1 - v0, e2 = v2 - v0, normal = e1 x e2 (lit side)
    kLtKindDirectional = 6, ///< normal = direction the light travels
};

/// LightTreeEmitter::flags / LightTreeNode::flags
enum LtFlag : u32 {
    kLtFlagLeaf = 1u << 0,     ///< node: childOrEmitter is an emitter index
    kLtFlagTwoSided = 1u << 1, ///< emits on both sides of its normal (area emitters)
};

/// One node of the flattened tree, depth-first: the first child of interior node i is i + 1, the second is
/// childOrEmitter. The bounds are conservative for every emitter below (Conty Estevez and Kulla,
/// "Importance Sampling of Many Lights on the GPU" / PBRT-v4 LightBounds): an AABB, the total power, and an
/// orientation cone (axis, cos theta_o, the normals' spread) with the emission falloff (cos theta_e).
struct LightTreeNode {
    f32 boundsMin[3] = {0.f, 0.f, 0.f};
    f32 phi = 0.f; ///< sum of the emitters' LightTreeEmitter::power
    f32 boundsMax[3] = {0.f, 0.f, 0.f};
    u32 childOrEmitter = kLtInvalid;
    f32 axis[3] = {0.f, 0.f, 1.f};
    f32 cosThetaO = -1.f; ///< normals within theta_o of axis (-1: any direction)
    f32 cosThetaE = 0.f;  ///< emission within theta_e beyond the normal cone (0: hemisphere)
    f32 sinThetaO = 0.f;  ///< sqrt(max(0, 1 - cosThetaO^2)), stored so the sampler never recomputes it
    u32 flags = 0;        ///< LtFlag
    u32 depth = 0;
};
static_assert(sizeof(LightTreeNode) == 64u, "LightTreeNode layout (lt_common.glsl / .slang)");
static_assert(offsetof(LightTreeNode, boundsMax) == 16u && offsetof(LightTreeNode, axis) == 32u &&
                  offsetof(LightTreeNode, cosThetaE) == 48u && offsetof(LightTreeNode, flags) == 56u,
              "LightTreeNode offsets");

/// One light (every light of the input list, in list order: the light index the sampler returns is the
/// index into this table). Point sampling uses p0 / e1 / e2 as documented for each kind.
struct LightTreeEmitter {
    f32 p0[3] = {0.f, 0.f, 0.f};
    u32 kind = kLtKindNone; ///< LtKind
    f32 e1[3] = {0.f, 0.f, 0.f};
    f32 area = 0.f;         ///< surface area (area kinds), 0 otherwise
    f32 e2[3] = {0.f, 0.f, 0.f};
    u32 source = kLtInvalid;///< caller id (GpuScene light slot, emissive triangle id, ...)
    f32 normal[3] = {0.f, 0.f, 1.f}; ///< unit: area normal / spot axis / directional travel direction
    u32 flags = 0;          ///< LtFlag (kLtFlagTwoSided)
    u32 leafNode = kLtInvalid; ///< node holding the emitter (kLtInvalid: directional or unused)
    u32 bitTrail = 0;       ///< branch bits root -> leaf (bit d = branch at depth d)
    u32 depth = 0;          ///< leaf depth; directional: index in the directional table
    f32 power = 0.f;        ///< importance scale: max radiant intensity (W/sr): I, or L x area (area kinds)
};
static_assert(sizeof(LightTreeEmitter) == 80u, "LightTreeEmitter layout (lt_common.glsl / .slang)");
static_assert(offsetof(LightTreeEmitter, e1) == 16u && offsetof(LightTreeEmitter, e2) == 32u &&
                  offsetof(LightTreeEmitter, normal) == 48u && offsetof(LightTreeEmitter, leafNode) == 64u,
              "LightTreeEmitter offsets");

/// Root record of one uploaded tree (the pass root: its BDA goes in push constants).
struct LightTreeHeader {
    u64 nodes = 0;       ///< LightTreeNode[nodeCount]
    u64 emitters = 0;    ///< LightTreeEmitter[emitterCount]
    u64 directional = 0; ///< u32[directionalCount]: emitter indices of the directional lights
    u32 nodeCount = 0;
    u32 emitterCount = 0;
    u32 directionalCount = 0;
    u32 version = 0;     ///< LightTree::version() of the uploaded content
    u32 reserved[2] = {0u, 0u};
};
static_assert(sizeof(LightTreeHeader) == 48u, "LightTreeHeader layout (lt_common.glsl / .slang)");

/// One sampling request of the lt_sample kernel (and of LightTreeSampler::sample on the CPU).
struct LightTreeQuery {
    f32 position[3] = {0.f, 0.f, 0.f};
    f32 u0 = 0.f;        ///< light selection
    f32 normal[3] = {0.f, 0.f, 0.f}; ///< shading normal; all zero = none (volumes)
    f32 u1 = 0.f;        ///< point on the light
    f32 u2 = 0.f;
    u32 evalLight = kLtInvalid; ///< light whose pmf the kernel also evaluates (kLtInvalid: skip)
    u32 reserved[2] = {0u, 0u};
};
static_assert(sizeof(LightTreeQuery) == 48u, "LightTreeQuery layout");

/// Result of one query.
struct LightTreeSample {
    u32 light = kLtInvalid; ///< emitter index (kLtInvalid: no light)
    f32 pmf = 0.f;          ///< probability of choosing `light` at this shading point
    f32 pdfArea = 0.f;      ///< density of `position` on the light's surface (1 / area; 0 for delta lights)
    u32 kind = kLtKindNone;
    f32 position[3] = {0.f, 0.f, 0.f}; ///< sampled point (point / spot: the light position;
                                        ///< directional: the travel direction)
    f32 evalPmf = 0.f;      ///< pmf of LightTreeQuery::evalLight
};
static_assert(sizeof(LightTreeSample) == 32u, "LightTreeSample layout");

/// Push constants of lt_sample (32 bytes).
struct LightTreePush {
    u64 header = 0;  ///< BDA of the LightTreeHeader
    u64 queries = 0; ///< BDA of LightTreeQuery[count]
    u64 results = 0; ///< BDA of LightTreeSample[count]
    u32 count = 0;
    u32 reserved = 0;
};
static_assert(sizeof(LightTreePush) == 32u, "LightTreePush layout");

inline constexpr u32 kLtWorkgroup = 64u;

} // namespace fuse::renderer::light_tree
