#pragma once

// WP-1.1 GPU scene: record layouts shared by the C++ mirror and the shaders
// (docs/unification/RENDERER-EXECUTION.md, renderer plan §5.2 "GPU scene", Phase 1).
//
// Every table is one persistent device buffer addressed by buffer device address (BDA) and
// registered in the bindless heap (WP-0.4), so a shader reaches it either as
//   header = GpuSceneHeader(fuse_buffer_address(sceneHandle)); header.instances[i] ...
// or through the table's own 32-bit storage-buffer handle stored in the header.
//
// Layout rules (so C++, GLSL std430 / buffer_reference and Slang pointers agree byte for byte):
//   * only 4-byte scalars, fixed arrays of them and 8-byte addresses; no vec3 members;
//   * u64 members are 8-byte aligned, every record size is a multiple of 16;
//   * no implicit padding (the static_asserts below pin sizes and offsets), so records compare
//     with memcmp and the GPU readback gate can be bit-exact.
// Shader mirrors: gpu_scene.glsl and gpu_scene.slang in this directory. Keep all three in sync.
//
// Device-safe (only <fuse/types.hpp>): kernel headers include it.

#include <fuse/types.hpp>

#include <cstddef>

namespace fuse::renderer::gpu_scene {

inline constexpr u32 kInvalidIndex = 0xFFFFFFFFu;

/// GpuInstance::flags. 0 means "free slot": consumers skip instances without kInstanceValid.
enum GpuInstanceFlag : u32 {
    kInstanceValid = 1u << 0,
    kInstanceVisible = 1u << 1,
    kInstanceCastShadow = 1u << 2,
    kInstanceReceiveShadow = 1u << 3,
    kInstanceStatic = 1u << 4,
    /// Drawn by the forward transparency pass (WP-2.3), never by the opaque visibility-buffer path
    /// (the WP-1.3 culler skips it).
    kInstanceTransparent = 1u << 5,
};

/// One renderable instance. Instance slots are stable for the instance's lifetime (temporal GPU
/// data such as last frame's visibility bit can be indexed by slot); a slot freed by
/// removeInstance() is not reused before the next frame, and `generation` changes on every reuse.
struct GpuInstance {
    u32 mesh = kInvalidIndex;     ///< GpuMesh index
    u32 material = kInvalidIndex; ///< base material row; submesh s uses material + SubmeshRange::material_index
    u32 flags = 0;                ///< GpuInstanceFlag bits (0 = free slot)
    u32 generation = 0;           ///< slot generation (low 32 bits), bumped by every add
    u32 entityIndex = kInvalidIndex; ///< ECS EntityID (picking / debug), kInvalidIndex when none
    u32 entityGeneration = 0;
    u32 userData = 0;
    u32 reserved = 0;
};
static_assert(sizeof(GpuInstance) == 32u, "GpuInstance layout (gpu_scene.glsl / .slang)");

/// Object-to-world affine transform, row-major 3x4: world = rows * (x, y, z, 1).
/// Indexed by instance slot in both the current and the previous-frame table.
struct GpuTransform {
    f32 rows[3][4] = {{1.f, 0.f, 0.f, 0.f}, {0.f, 1.f, 0.f, 0.f}, {0.f, 0.f, 1.f, 0.f}};
};
static_assert(sizeof(GpuTransform) == 48u, "GpuTransform layout");

/// Converts a column-major 4x4 (OpenGL / Vulkan / fuse::ecs::mat4 convention: element (row r,
/// column c) at m[c * 4 + r]) into the 3x4 row-major record. Bit-exact (a pure shuffle).
FUSE_HOST_DEVICE inline void transformFromColumnMajor(const f32* m, GpuTransform& out) {
    for (u32 r = 0; r < 3u; ++r) {
        for (u32 c = 0; c < 4u; ++c) {
            out.rows[r][c] = m[c * 4u + r];
        }
    }
}

/// GPU form of one WP-1.2 meshlet: byte-identical to the `.fusemeshlet` MSHL chunk element
/// (geometry/meshlet_format.hpp), so cooked chunks can be copied to the GPU without repacking.
/// `counts` = vertex_count | triangle_count << 8 | submesh << 16 (the u8, u8, u16 on disk).
struct GpuMeshlet {
    u32 vertexOffset = 0;   ///< first MVRT entry
    u32 triangleOffset = 0; ///< first MTRI entry
    u32 counts = 0;
    f32 center[3] = {0.f, 0.f, 0.f};
    f32 radius = 0.f;
    f32 coneApex[3] = {0.f, 0.f, 0.f};
    f32 coneAxis[3] = {0.f, 0.f, 0.f};
    f32 coneCutoff = 1.f;
    u32 coneS8 = 0; ///< s8 axis x, y, z, cutoff (bytes 0..3)
    f32 aabbMin[3] = {0.f, 0.f, 0.f};
    f32 aabbMax[3] = {0.f, 0.f, 0.f};
    u32 reserved[3] = {0u, 0u, 0u};
};
static_assert(sizeof(GpuMeshlet) == 96u, "GpuMeshlet must match the MSHL chunk element (96 bytes)");

/// Mirrors geometry::SubmeshRange (SUBM chunk element).
struct GpuSubmesh {
    u32 meshletOffset = 0;
    u32 meshletCount = 0;
    u32 materialIndex = 0;
    u32 triangleCount = 0;
};
static_assert(sizeof(GpuSubmesh) == 16u, "GpuSubmesh layout");

/// One mesh. Stream addresses point into the mesh's geometry buffer (WP-1.2 cooked layout:
/// MVRT u32, MTRI packed u32, VPOS u16x4 quantised, VNRM / VTAN oct snorm16x2, VUV0 half2).
/// Decoded position = quantOffset + float(q) * quantStep (exact, see vertex_codec_kernel.hpp).
/// An address of 0 means the stream is absent (e.g. a mesh registered from bounds only).
///
/// Index layout (WP-1.4, the draw range of the mesh): the scene owns ONE u32 index buffer
/// (GpuScene::indexBuffer(), BDA in GpuSceneHeader::indexAddress). A meshlet mesh's triangles are
/// stored there as a plain triangle list in meshlet order (MTRI order), each index the mesh-local
/// vertex MVRT[meshlet.vertexOffset + micro-index], so
///   triangle t of the mesh == MTRI entry t == indices[firstIndex + 3t .. 3t + 2],
/// gl_PrimitiveID of an indexed draw of the range is t, and every indexed draw of every mesh uses the
/// same bound index buffer (one vkCmdDrawIndexedIndirectCount for the whole scene). Vertex pulling
/// reads VPOS[index] through BDA; vertexOffset stays 0 for meshlet meshes (the raw index is the
/// mesh-local vertex). indexCount == 0 means "no range in the scene index buffer" (a mesh added from
/// bounds only, drawn with a caller-bound index buffer): draws then use the legacy implicit range
/// {3 x triangleCount, firstIndex 0, vertexOffset 0} (meshDrawIndexCount()).
struct GpuMesh {
    u64 meshlets = 0;         ///< GpuMeshlet[meshletCount]
    u64 meshletVertices = 0;  ///< u32 per meshlet vertex (MVRT)
    u64 meshletTriangles = 0; ///< u32 per triangle (MTRI)
    u64 positions = 0;        ///< u16 x 4 per vertex (VPOS)
    u64 normals = 0;          ///< u32 per vertex (VNRM)
    u64 tangents = 0;         ///< u32 per vertex (VTAN)
    u64 uvs = 0;              ///< u32 per vertex (VUV0)
    u64 submeshes = 0;        ///< GpuSubmesh[submeshCount]
    u32 meshletCount = 0;
    u32 submeshCount = 0;
    u32 vertexCount = 0;
    u32 triangleCount = 0;
    f32 quantOffset[3] = {0.f, 0.f, 0.f};
    f32 quantStep[3] = {1.f, 1.f, 1.f};
    f32 boundsCenter[3] = {0.f, 0.f, 0.f}; ///< object-space bounding sphere of the whole mesh
    f32 boundsRadius = 0.f;
    u32 geometryHandle = 0; ///< bindless storage-buffer handle of the geometry buffer (0 = none)
    u32 flags = 0;
    u32 firstIndex = 0;   ///< first u32 of the mesh's triangle list in the scene index buffer
    u32 indexCount = 0;   ///< 3 x triangleCount; 0 = no range in the scene index buffer (see above)
    s32 vertexOffset = 0; ///< VkDrawIndexedIndirectCommand::vertexOffset (0 for meshlet meshes)
    u32 reserved = 0;
};
static_assert(sizeof(GpuMesh) == 144u, "GpuMesh layout");
static_assert(offsetof(GpuMesh, meshletCount) == 64u && offsetof(GpuMesh, quantOffset) == 80u &&
                  offsetof(GpuMesh, boundsCenter) == 104u && offsetof(GpuMesh, geometryHandle) == 120u &&
                  offsetof(GpuMesh, firstIndex) == 128u,
              "GpuMesh offsets (gpu_scene.glsl / .slang)");

/// Index count an indexed draw of `mesh` uses (the legacy implicit range when it has none).
FUSE_HOST_DEVICE inline u32 meshDrawIndexCount(const GpuMesh& mesh) {
    return mesh.indexCount != 0u ? mesh.indexCount : mesh.triangleCount * 3u;
}

enum class GpuLightType : u32 {
    None = 0, ///< free slot
    Directional = 1,
    Point = 2,
    Spot = 3,
};

/// One analytic light (world space). Directional: `direction` is the direction the light travels.
struct GpuLight {
    f32 position[3] = {0.f, 0.f, 0.f};
    u32 type = 0; ///< GpuLightType
    f32 direction[3] = {0.f, 0.f, -1.f};
    f32 range = 0.f;
    f32 color[3] = {1.f, 1.f, 1.f};
    f32 intensity = 0.f;
    f32 cosInner = 1.f; ///< spot: cos(inner half angle)
    f32 cosOuter = 1.f; ///< spot: cos(outer half angle)
    u32 flags = 0;
    u32 entityIndex = kInvalidIndex;
};
static_assert(sizeof(GpuLight) == 64u, "GpuLight layout");

/// Table order in GpuSceneHeader::addresses / handles / counts.
enum class GpuSceneTable : u32 {
    Instances = 0,
    Transforms = 1,
    PrevTransforms = 2,
    Meshes = 3,
    Materials = 4, ///< rows are Material::GPUMaterial (128 bytes, shaders/common/material.glsl)
    Lights = 5,
};
inline constexpr u32 kGpuSceneTableCount = 6u;
inline constexpr u32 kGpuSceneMagic = 0x43534746u; ///< "FGSC"
/// 2: GpuMesh 144 bytes with the draw range, GpuSceneHeader::indexAddress (WP-1.4).
inline constexpr u32 kGpuSceneLayoutVersion = 2u;

/// Root record: one small buffer whose bindless handle is the only thing a pass needs.
struct GpuSceneHeader {
    u64 addresses[kGpuSceneTableCount] = {}; ///< BDA of each table (GpuSceneTable order)
    u32 counts[kGpuSceneTableCount] = {};    ///< rows in use; instances / lights: slot high-water mark
    u32 handles[kGpuSceneTableCount] = {};   ///< bindless storage-buffer handle of each table
    u32 capacities[kGpuSceneTableCount] = {};///< rows allocated
    u32 liveInstances = 0;
    u32 liveLights = 0;
    u32 magic = kGpuSceneMagic;
    u32 version = kGpuSceneLayoutVersion;
    u64 indexAddress = 0; ///< BDA of the scene index buffer (u32 triangle lists, GpuMesh::firstIndex); 0 = none
};
static_assert(sizeof(GpuSceneHeader) == 144u, "GpuSceneHeader layout");
static_assert(offsetof(GpuSceneHeader, counts) == 48u && offsetof(GpuSceneHeader, handles) == 72u &&
                  offsetof(GpuSceneHeader, capacities) == 96u && offsetof(GpuSceneHeader, liveInstances) == 120u &&
                  offsetof(GpuSceneHeader, indexAddress) == 136u,
              "GpuSceneHeader offsets (gpu_scene.glsl / .slang)");

} // namespace fuse::renderer::gpu_scene
