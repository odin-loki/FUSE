#pragma once

// GPU skinning in the GPU scene: one compute pass "skin.lbs" (linear blend skinning, the twin of the single-source
// CPU kernel fuse::animation::skinning_kernel "skinning_lbs") writes, per skinned instance, the CURRENT skinned
// positions + normals and the PREVIOUS skinned positions into GPU-scene streams that every visibility-buffer consumer
// reads through the instance's own GpuMesh row (gpu_scene::kMeshFloatPositions | kMeshFloatNormals | kMeshSkinned):
// the WP-1.4 raster / decode, the WP-1.5 material resolve, the WP-3.2 VSM raster and the WP-4.1 motion pass, which
// takes the previous surface point from the previous skinned positions (skinned motion vectors).
//
//   SkinningGpu skin;  skin.init({device, allocator, upload, ...});  skin.attach(scene);
//   const u32 mesh = skin.addMesh({restPositions, restNormals, weights, vertexCount});   // once (UploadQueue)
//   const u32 si = skin.addInstance(scene, {instanceHandle, mesh, baseMeshRow, boneCount}); // own GpuMesh row
//   per frame:  scene.beginFrame(serial);  skin.setPose(si, palette, boneCount);  scene.commit();
//               skin.beginFrame(serial);                // palette ring + previous-palette history
//               refs = scene.importInto(graph);  skin.addSkinning(graph, refs);   // before any scene reader
//
// Previous-palette history: beginFrame writes this frame's palettes into a host-visible ring slot; the previous
// palette of an instance is the one the previous beginFrame wrote (read in place from the previous slot, or uploaded
// again when the previous frame used the same slot), or this frame's on the instance's first frame or after
// resetHistory (e.g. a teleport: zero skinned motion). The CPU keeps the last palettes for that fallback and for
// inspection. The kernel skins the rest pose with both palettes. Re-skinning the
// previous pose (instead of keeping last frame's output) costs one more 4-influence blend per vertex but needs no
// ping-pong of the mesh rows and makes the previous positions bit-identical to what the previous frame drew.
//
// Synchronisation: the output arena is registered with the GpuScene (GpuScene::setDynamicGeometry), so
// GpuSceneGraphRefs::dynamicGeometry carries it and GpuScene::useAll declares it on every pass that reads the
// scene; "skin.lbs" writes it, the render graph orders the rest. No per-frame allocation (fixed arenas sized at
// init; rows and jobs are created by addMesh / addInstance only).
//
// Parity: the kernel performs the CPU kernel's operations in the same order without contraction (GLSL `precise`,
// Slang -fp-mode precise) with correctly rounded sqrt / division (common/exact_math), so positions and normals are
// bit-identical to animation::skinning_lbs on every device (fuse_rp_skinning_vk_parity_*).

#include <fuse/renderer/gpu_scene/gpu_scene.hpp>
#include <fuse/renderer/resources.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/skinning/skinning_gpu_types.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::renderer {
class GpuAllocator;
class UploadQueue;
class VulkanDevice;
} // namespace fuse::renderer

namespace fuse::renderer::skinning {

enum class SkinningKernelLanguage : u8 {
    Auto = 0, ///< Slang if built, else GLSL
    Slang,
    Glsl,
};

inline constexpr u32 kInvalidSkin = 0xFFFFFFFFu;

struct SkinningGpuDesc {
    VulkanDevice* device = nullptr;
    GpuAllocator* allocator = nullptr;
    UploadQueue* upload = nullptr; ///< rest streams (addMesh); the caller flushes / waits like GpuScene uploads
    u32 maxRestVertices = 1u << 16;   ///< sum of addMesh vertex counts
    u32 maxOutputVertices = 1u << 18; ///< sum of addInstance vertex counts
    u32 maxInstances = 1024u;
    u32 maxPaletteBones = 1u << 14; ///< sum of addInstance bone counts
    u32 framesInFlight = 3;         ///< palette / job ring slots
    SkinningKernelLanguage language = SkinningKernelLanguage::Auto;
};

/// Rest pose of one skinnable mesh (vertex order == the base GpuMesh's mesh-local vertices).
struct SkinMeshDesc {
    const f32* restPositions = nullptr; ///< xyz per vertex
    const f32* restNormals = nullptr;   ///< xyz per vertex, optional (then the base row's normals stay in use)
    const f32* handedness = nullptr;    ///< tangent handedness +-1 per vertex, optional (+1)
    const SkinVertexWeights* weights = nullptr;
    u32 vertexCount = 0;
};

struct SkinInstanceDesc {
    gpu_scene::InstanceHandle instance{};
    u32 mesh = kInvalidSkin;        ///< addMesh result
    u32 baseMeshRow = kInvalidSkin; ///< GpuScene mesh row whose index range / uvs / tangents / submeshes the instance keeps
    u32 boneCount = 0;
    /// Object-space bounding sphere of every pose (instance culling). Radius <= 0: the base row's sphere with its
    /// radius scaled by boundsScale.
    f32 boundsCenter[3] = {0.f, 0.f, 0.f};
    f32 boundsRadius = 0.f;
    f32 boundsScale = 1.5f;
};

struct SkinningStats {
    u32 instances = 0;     ///< live skinned instances
    u32 jobs = 0;          ///< jobs written by the last beginFrame
    u32 vertices = 0;      ///< vertices skinned per frame (x 2 poses)
    u32 dispatches = 0;    ///< "skin.lbs" passes added this frame
    u32 meshRows = 0;      ///< GpuMesh rows created by addInstance
};

class SkinningGpu {
public:
    SkinningGpu() = default;
    ~SkinningGpu();
    SkinningGpu(const SkinningGpu&) = delete;
    SkinningGpu& operator=(const SkinningGpu&) = delete;

    bool init(const SkinningGpuDesc& desc);
    /// The caller must have retired every frame that used the arenas.
    void destroy();
    bool valid() const { return m_initialized; }
    const char* kernelLanguage() const { return m_language; }

    /// Registers the output arena with `scene` (GpuScene::setDynamicGeometry) so its readers order after skin.lbs.
    void attach(gpu_scene::GpuScene& scene);

    /// Uploads a rest pose (UploadQueue). kInvalidSkin when the arena is full or the input invalid.
    u32 addMesh(const SkinMeshDesc& desc);
    /// Gives `desc.instance` its own GpuMesh row (a copy of the base row with the float streams of this instance)
    /// and allocates its output streams and palette. kInvalidSkin on failure.
    u32 addInstance(gpu_scene::GpuScene& scene, const SkinInstanceDesc& desc);
    /// This frame's palette of skinned instance `skin` (column-major 4x4 f32 per bone, `world * inverse bind` as
    /// animation::compute_skinning_palette writes it). Not calling it keeps the last pose (zero skinned motion).
    bool setPose(u32 skin, const f32* palette, u32 boneCount);
    /// Next beginFrame skins the previous pose with the current palette (teleport / first frame: no skinned motion).
    void resetHistory(u32 skin);
    /// Writes this frame's jobs and palettes (ring slot of `frameSerial`) and rolls the palette history.
    bool beginFrame(u64 frameSerial);
    /// "skin.lbs": writes `scene.dynamicGeometry` (the output arena). Add it after GpuScene::importInto and before
    /// every pass that reads the scene.
    void addSkinning(rg::Graph& graph, const gpu_scene::GpuSceneGraphRefs& scene);

    // --- inspection ------------------------------------------------------------------------------------
    const Buffer& outputBuffer() const { return m_out; }
    const Buffer& restBuffer() const { return m_rest; }
    u32 instanceCount() const { return static_cast<u32>(m_instances.size()); }
    u32 meshRow(u32 skin) const { return m_instances[skin].meshRow; }
    u32 vertexCount(u32 skin) const { return m_instances[skin].vertexCount; }
    /// Byte offset of instance `skin`'s output streams in outputBuffer() (current, previous, normals; 16 B / vertex).
    u64 outputOffset(u32 skin) const { return static_cast<u64>(m_instances[skin].outVertex) * 48u; }
    /// The palettes the last beginFrame wrote for `skin` (current / previous), boneCount(skin) x 16 floats.
    const f32* currentPalette(u32 skin) const { return &m_pose[static_cast<usize>(m_instances[skin].paletteBone) * 16u]; }
    const f32* previousPalette(u32 skin) const { return &m_prevWritten[static_cast<usize>(m_instances[skin].paletteBone) * 16u]; }
    u32 boneCount(u32 skin) const { return m_instances[skin].boneCount; }
    const SkinningStats& stats() const { return m_stats; }

private:
    struct MeshRecord {
        u32 restVertex = 0; ///< first vertex in the rest arena (64 B / vertex)
        u32 vertexCount = 0;
        bool normals = false;
    };
    struct InstanceRecord {
        u32 mesh = 0;
        u32 meshRow = 0;
        u32 outVertex = 0;   ///< first vertex in the output arena (48 B / vertex)
        u32 vertexCount = 0;
        u32 paletteBone = 0; ///< first bone in the palette arena
        u32 boneCount = 0;
        bool history = false;
    };
    struct PassRecord {
        SkinningGpu* self = nullptr;
        SkinPush push{};
        u32 groupsX = 0;
    };
    static void recordSkinning(const rg::PassContext& context, void* user);

    SkinningGpuDesc m_desc{};
    bool m_initialized = false;
    const char* m_language = "none";
    Buffer m_rest{};
    Buffer m_out{};
    Buffer m_ring{};
    u64 m_slotBytes = 0;
    u64 m_paletteOffset = 0; ///< byte offset of the palettes inside a ring slot
    u32 m_restUsed = 0;
    u32 m_outUsed = 0;
    u32 m_bonesUsed = 0;
    u32 m_maxInstanceVertices = 0;
    std::vector<MeshRecord> m_meshes;
    std::vector<InstanceRecord> m_instances;
    std::vector<f32> m_pose;        ///< current palette per instance (maxPaletteBones x 16)
    std::vector<f32> m_last;        ///< palette written last frame (history)
    std::vector<f32> m_prevWritten; ///< previous palette written by the last beginFrame (inspection)
    u64 m_jobsAddress = 0;
    u64 m_lastPaletteAddress = 0; ///< BDA of the palettes the previous beginFrame wrote (its ring slot)
    u32 m_jobCount = 0;
    PassRecord m_record{};
    void* m_layout = nullptr;
    void* m_pipeline = nullptr;
    SkinningStats m_stats{};
};

} // namespace fuse::renderer::skinning
