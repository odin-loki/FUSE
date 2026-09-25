#pragma once

// WP-1.1 GPU scene (renderer plan §5.2, Phase 1 "GPU scene buffers with delta uploads").
//
// Persistent device buffers, one per table, addressed by buffer device address and registered in
// the bindless heap (WP-0.4):
//
//   header          GpuSceneHeader    table addresses, counts, bindless handles (the pass root)
//   instances       GpuInstance       stable slots with generations (free slot = flags 0)
//   transforms      GpuTransform      object->world, current frame, indexed by instance slot
//   prevTransforms  GpuTransform      previous frame (per-object motion vectors), same indexing
//   meshes          GpuMesh           WP-1.2 meshlet stream addresses + quantisation + bounds
//   materials       GPUMaterial       Material::GPUMaterial rows (shaders/common/material.glsl)
//   lights          GpuLight          stable slots like instances (free slot = type None)
//
// Every table has a CPU mirror (scene_table.hpp). Mutations write the mirror and mark changed rows;
// commit() turns them into sorted, coalesced ranges and uploads only those rows, one of two ways:
//   * direct: one vkCmdCopyBuffer region per range through the UploadQueue (few ranges, and every
//     full upload after a reallocation);
//   * scatter: when a table has many scattered ranges, its dirty rows plus one u32 destination index
//     per row are staged as ONE packed block (one copy into the scatter buffer), and importInto()
//     records the render-graph compute pass "gpu_scene.scatter" (gpu_scene_scatter.slang / .comp,
//     pure BDA) that writes them into the table. The graph derives the barriers to later readers.
// Either way upload bytes scale with the rows that changed (scatter adds 4 bytes per row).
//
// Frame protocol (one owner thread):
//
//   scene.beginFrame(serial);        // prev <- cur for rows moved last frame; frees parked slots
//   extractor.extract(registry, scene) and/or addInstance / setTransform / removeInstance / ...
//   scene.commit();                  // stage + record copies (never flushes); may run more than once
//   upload.flush();                  // owner submits the upload batch BEFORE the graph executes
//   GpuSceneGraphRefs refs = scene.importInto(graph);   // + "gpu_scene.scatter" pass when pending;
//                                                       // declare it before the scene's readers
//   push.scene = scene.headerHandle();                  // fuse_buffer_address(push.scene)
//   ...                              // later, with the serial of a completed frame:
//   scene.collectRetired(completedSerial);
//
// Previous transforms: prev[i] for frame N equals cur[i] of frame N-1. beginFrame() copies cur -> prev
// only for rows whose current transform changed in the previous frame, so prev deltas are bounded by
// last frame's moves. setTransform(..., teleport = true) and addInstance() write both (no motion).
//
// Growth (a table exceeds its capacity) reallocates that GPU buffer at the next commit(): a new
// buffer is registered in the bindless heap, the whole mirror [0, count) is uploaded once, and the
// old buffer and slot are retired at the current frame serial (collectRetired destroys them).
//
// CPU-only mode (no device / allocator / upload queue, or the stub backend): mirrors, dirty
// tracking and commit statistics work unchanged; nothing is uploaded. The CPU gates use it.
//
// Steady-state frames (no growth) make no heap allocations: every per-row structure is sized with
// the table (scene_table.hpp) and staging uses a fixed scratch block.

#include <fuse/renderer/gpu_scene/gpu_scene_types.hpp>
#include <fuse/renderer/gpu_scene/scene_table.hpp>
#include <fuse/renderer/material/material.hpp>
#include <fuse/renderer/resources.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::renderer {
class GpuAllocator;
class UploadQueue;
class VulkanDevice;
} // namespace fuse::renderer

namespace fuse::renderer::geometry {
struct MeshletMesh;
}

namespace fuse::renderer::gpu_scene {

using GpuMaterial = Material::GPUMaterial;
static_assert(sizeof(GpuMaterial) == 128u, "material rows are Material::GPUMaterial (128-byte std430 stride)");
static_assert((kGpuMaterialLayered & (MaterialFlagBits::kProcedural |
                                      (MaterialFlagBits::kProceduralIdMask << MaterialFlagBits::kProceduralIdShift) |
                                      MaterialFlagBits::kHasNormalMap | MaterialFlagBits::kHasAoMap |
                                      MaterialFlagBits::kHasMetallicMap)) == 0u,
              "kGpuMaterialLayered is a free GPUMaterial::flags bit");

/// Marks a material row as layered (kGpuMaterialLayered): the resolve's layered bin evaluates entry `layeredIndex` of
/// the layered-material table instead of the row's base colour / textures.
inline void set_gpu_material_layered(GpuMaterial& m, u32 layeredIndex) {
    m.flags |= kGpuMaterialLayered;
    m.padding = layeredIndex;
}
inline bool gpu_material_layered(const GpuMaterial& m) { return (m.flags & kGpuMaterialLayered) != 0u; }
/// Layered-table index of a layered row.
inline u32 gpu_material_layered_index(const GpuMaterial& m) { return m.padding; }

using InstanceHandle = SlotAllocator::Handle;
using LightHandle = SlotAllocator::Handle;

/// Scatter-upload kernel choice (both are built from twin sources when the toolchains exist).
enum class GpuSceneScatter : u8 {
    Auto = 0, ///< Slang build if present, else GLSL, else Off
    Slang,
    Glsl,
    Off,      ///< direct copies only
};

struct GpuSceneDesc {
    /// All three null (or the stub backend): CPU-only mode.
    VulkanDevice* device = nullptr;
    GpuAllocator* allocator = nullptr;
    UploadQueue* upload = nullptr;
    /// Optional: tables and the header get bindless storage-buffer slots (shader handles). Without
    /// it the header still carries every table's device address.
    BindlessDescriptors* bindless = nullptr;

    /// Initial row capacities (tables grow by doubling).
    u32 instanceCapacity = 1024;
    u32 meshCapacity = 256;
    u32 materialCapacity = 256;
    u32 lightCapacity = 256;

    /// Dirty runs separated by at most this many clean bytes are uploaded as one copy region
    /// (converted to whole rows per table). 0 = exact: only changed rows are uploaded.
    u32 mergeGapBytes = 0;
    /// Staging granularity: small ranges are packed into a scratch block of this size and staged
    /// once; ranges at least `directStageBytes` long are staged straight from the mirror.
    u32 stagingChunkBytes = 64u * 1024u;
    u32 directStageBytes = 4096u;
    GpuSceneScatter scatter = GpuSceneScatter::Auto;
    /// A table whose delta has more than this many ranges is scattered instead of copied.
    u32 scatterMinRanges = 8u;
    /// CPU-only mode: receives every byte range commit() would upload (table = GpuSceneTable value,
    /// kGpuSceneTableCount = header), e.g. to replay deltas into a shadow copy in tests or to feed
    /// another transport. Ignored when the GPU path is active.
    void (*uploadSink)(void* user, u32 table, u64 dstOffset, const void* data, u64 bytes) = nullptr;
    void* uploadSinkUser = nullptr;
    const char* name = "gpu_scene";
};

struct InstanceDesc {
    u32 mesh = kInvalidIndex;
    u32 material = kInvalidIndex;
    /// kInstanceValid is added automatically.
    u32 flags = kInstanceVisible | kInstanceCastShadow | kInstanceReceiveShadow;
    GpuTransform transform{};
    u32 entityIndex = kInvalidIndex;
    u32 entityGeneration = 0;
    u32 userData = 0;
};

/// Per-table upload accounting of one commit().
struct TableCommitStats {
    u32 dirtyRows = 0;    ///< rows marked since the last commit
    u32 uploadedRows = 0; ///< rows covered by copy regions (>= dirtyRows when gaps are merged)
    u32 ranges = 0;       ///< copy regions recorded
    u64 bytes = 0;        ///< bytes staged for this table (rows + scatter indices)
    u64 indexBytes = 0;   ///< scatter only: 4 bytes per row
    bool scattered = false;
    bool fullUpload = false;
    bool reallocated = false;
};

struct GpuSceneCommitStats {
    TableCommitStats tables[kGpuSceneTableCount] = {};
    bool headerUploaded = false;
    u64 bytes = 0;         ///< total payload bytes this commit (tables + header)
    u32 ranges = 0;        ///< total copy regions (tables + header)
    u32 stageCalls = 0;    ///< UploadQueue::stage calls
    u32 copies = 0;        ///< UploadQueue::recordBufferCopy calls
    u32 scatterJobs = 0;   ///< tables scattered (dispatches added by the next importInto())
    bool gpu = false;      ///< copies were recorded (false in CPU-only mode)
    bool ok = true;        ///< false when staging or recording failed (mirror stays dirty)
};

/// Render-graph handles of the scene buffers for one frame (importInto()).
struct GpuSceneGraphRefs {
    rg::BufferRef header;
    rg::BufferRef tables[kGpuSceneTableCount];
    /// Scene index buffer (GpuMesh "Index layout"), when any meshlet mesh was added. Not part of
    /// useAll(): raster passes declare IndexRead, BDA readers StorageRead.
    rg::BufferRef indices;
};

/// Kernel-facing views for bulk writers (the ECS extractor): mirror rows plus dirty storage.
template <typename T>
struct TableView {
    kernel::Span<T> rows;
    DirtyView dirty;
    u32 count = 0; ///< rows in use; writers must stay below it

    /// Writes `value` to `row` and marks it when the bytes changed (row owned by the caller).
    FUSE_HOST_DEVICE bool write(u32 row, const T& value) const {
        T& dst = rows[row];
        const u8* a = reinterpret_cast<const u8*>(&dst);
        const u8* b = reinterpret_cast<const u8*>(&value);
        bool same = true;
        for (u32 i = 0; i < sizeof(T); ++i) {
            if (a[i] != b[i]) {
                same = false;
                break;
            }
        }
        if (same) {
            return false;
        }
        dst = value;
        dirty.mark(row);
        return true;
    }
};

class GpuScene {
public:
    GpuScene() = default;
    ~GpuScene();
    GpuScene(const GpuScene&) = delete;
    GpuScene& operator=(const GpuScene&) = delete;

    /// Returns false only when a GPU resource could not be created (CPU-only mode always succeeds).
    bool init(const GpuSceneDesc& desc);
    /// Waits for nothing: the caller must have retired every frame that used the scene.
    void destroy();
    bool gpuEnabled() const { return m_gpu; }
    const GpuSceneDesc& desc() const { return m_desc; }

    // --- frame ----------------------------------------------------------------------------------
    void beginFrame(u64 frameSerial);
    GpuSceneCommitStats commit();
    /// Destroys buffers (and releases their bindless slots) retired at serials <= completedSerial.
    u32 collectRetired(u64 completedSerial);
    u64 frameSerial() const { return m_frameSerial; }

    // --- instances ------------------------------------------------------------------------------
    InstanceHandle addInstance(const InstanceDesc& desc);
    bool removeInstance(InstanceHandle handle);
    bool alive(InstanceHandle handle) const { return m_instanceSlots.alive(handle); }
    bool setTransform(InstanceHandle handle, const GpuTransform& transform, bool teleport = false);
    bool setInstanceMesh(InstanceHandle handle, u32 mesh);
    bool setInstanceMaterial(InstanceHandle handle, u32 material);
    bool setInstanceFlags(InstanceHandle handle, u32 flags);
    const GpuInstance& instance(u32 slot) const { return m_instances[slot]; }
    const GpuTransform& transform(u32 slot) const { return m_transforms[slot]; }
    const GpuTransform& prevTransform(u32 slot) const { return m_prevTransforms[slot]; }
    u32 instanceHighWater() const { return m_instances.count(); }
    u32 liveInstances() const { return m_instanceSlots.live(); }
    /// Pre-sizes the instance tables (GPU buffers follow at the next commit).
    void reserveInstances(u32 capacity);

    // --- meshes ---------------------------------------------------------------------------------
    u32 addMesh(const GpuMesh& mesh);
    bool setMesh(u32 index, const GpuMesh& mesh);
    /// Uploads a WP-1.2 meshlet mesh into its own geometry buffer (BDA + bindless handle) and adds
    /// its GpuMesh. CPU-only mode records the counts, quantisation and bounds with zero addresses.
    u32 addMeshletMesh(const geometry::MeshletMesh& mesh);
    /// Scene index buffer (u32 triangle lists of every meshlet mesh, GpuMesh::firstIndex /
    /// indexCount; see gpu_scene_types.hpp "Index layout"). Uploaded by addMeshletMesh() through the
    /// UploadQueue; grows by doubling (the old buffer retires like a table). Its BDA is
    /// GpuSceneHeader::indexAddress. Bind it for every indexed draw of the scene.
    const Buffer& indexBuffer() const { return m_indexBuffer; }
    /// CPU copy of the scene index buffer (entries [0, indexCount())), also kept in CPU-only mode.
    const u32* indexData() const { return m_indexMirror.data(); }
    u32 indexCount() const { return static_cast<u32>(m_indexMirror.size()); }
    const GpuMesh& mesh(u32 index) const { return m_meshes[index]; }
    u32 meshCount() const { return m_meshes.count(); }

    // --- materials ------------------------------------------------------------------------------
    bool setMaterial(u32 index, const GpuMaterial& material);
    bool setMaterial(u32 index, const Material& material) { return setMaterial(index, material.pack()); }
    u32 materialCount() const { return m_materials.count(); }

    // --- lights ---------------------------------------------------------------------------------
    LightHandle addLight(const GpuLight& light);
    bool setLight(LightHandle handle, const GpuLight& light);
    bool removeLight(LightHandle handle);
    bool lightAlive(LightHandle handle) const { return m_lightSlots.alive(handle); }
    const GpuLight& light(u32 slot) const { return m_lights[slot]; }
    u32 lightHighWater() const { return m_lights.count(); }

    // --- GPU access -----------------------------------------------------------------------------
    /// Bindless storage-buffer handle of the header (push-constant root), 0 without bindless.
    u32 headerHandle() const;
    u64 headerAddress() const { return m_headerBuffer.deviceAddress; }
    const GpuSceneHeader& header() const { return m_header; }
    const Buffer& headerBuffer() const { return m_headerBuffer; }
    const Buffer& tableBuffer(GpuSceneTable table) const { return m_gpuTables[static_cast<u32>(table)].buffer; }
    /// CPU mirror bytes of a table (rows [0, count)).
    TableBytes tableBytes(GpuSceneTable table) const;
    /// Imports the header and every table buffer; pass the refs to PassBuilder::use. When scatter
    /// uploads are pending it also adds the "gpu_scene.scatter" compute pass (neverCull) writing the
    /// affected tables, so passes declared afterwards see the new rows.
    GpuSceneGraphRefs importInto(rg::Graph& graph);
    /// Scatter kernel in use ("slang", "glsl" or "off").
    const char* scatterKernel() const { return m_scatterKernel; }
    u32 pendingScatterJobs() const { return m_jobCount; }
    /// Declares `access` on the header and every table for the pass being built.
    static void useAll(rg::PassBuilder& pass, const GpuSceneGraphRefs& refs, rg::Access access, u8 shaderStages = 0);

    // --- bulk writers (ECS extractor kernels) ---------------------------------------------------
    TableView<GpuInstance> instanceView();
    TableView<GpuTransform> transformView();
    /// Current slot generation (preserved by bulk instance writes).
    u32 slotGeneration(u32 slot) const { return m_instanceSlots.generation(slot); }

private:
    struct GpuTable {
        Buffer buffer{};
        BindlessSlotHandle slot{};
        u32 handle = 0;
        u32 capacityRows = 0;
    };
    struct Retired {
        Buffer buffer{};
        BindlessSlotHandle slot{};
        u64 serial = 0;
    };
    struct Geometry {
        Buffer buffer{};
        BindlessSlotHandle slot{};
    };
    /// One scatter dispatch (push constants of gpu_scene_scatter).
    struct ScatterJob {
        u64 src = 0;
        u64 dst = 0;
        u32 rowWords = 0;
        u32 rowCount = 0;
        u32 indexOffsetWords = 0;
        u32 threadCount = 0;
        u32 table = 0;
    };
    static constexpr u32 kMaxScatterJobs = 32u;
    /// A small range packed into m_scratch, copied once the scratch block is staged.
    struct PendingCopy {
        usize scratchOffset = 0;
        usize dstOffset = 0;
        usize size = 0;
    };

    template <typename T>
    bool commitTable(GpuSceneTable table, MirrorTable<T>& mirror, GpuSceneCommitStats& stats);
    bool uploadRanges(u32 table, void* dstBuffer, const TableBytes& bytes, const std::vector<RowRange>& ranges,
                      TableCommitStats& tableStats, GpuSceneCommitStats& stats);
    bool flushPending(void* dstBuffer, GpuSceneCommitStats& stats);
    bool stageBytes(void* dstBuffer, const u8* data, usize size, usize dstOffset, GpuSceneCommitStats& stats);
    bool scatterRanges(u32 table, const TableBytes& bytes, const std::vector<RowRange>& ranges,
                       TableCommitStats& tableStats, GpuSceneCommitStats& stats);
    bool createScatterPipeline();
    void destroyScatterPipeline();
    static void recordScatter(const rg::PassContext& context, void* user);
    bool ensureGpuTable(GpuSceneTable table, u32 capacityRows, u32 stride, bool& reallocated);
    bool createBuffer(Buffer& out, u64 bytes, const char* name);
    void retire(Buffer& buffer, BindlessSlotHandle& slot);
    void refreshHeader();
    bool uploadIndices(u32 firstIndex, u32 count);
    void growInstanceTables(u32 count);

    GpuSceneDesc m_desc{};
    bool m_gpu = false;
    bool m_initialized = false;
    u64 m_frameSerial = 0;

    MirrorTable<GpuInstance> m_instances;
    MirrorTable<GpuTransform> m_transforms;
    MirrorTable<GpuTransform> m_prevTransforms;
    MirrorTable<GpuMesh> m_meshes;
    MirrorTable<GpuMaterial> m_materials;
    MirrorTable<GpuLight> m_lights;
    SlotAllocator m_instanceSlots;
    SlotAllocator m_lightSlots;
    /// Rows whose current transform was committed this frame (prev catches up at beginFrame()).
    DirtySet m_moved;

    GpuSceneHeader m_header{};
    GpuSceneHeader m_uploadedHeader{};
    bool m_headerValid = false;
    Buffer m_headerBuffer{};
    BindlessSlotHandle m_headerSlot{};
    GpuTable m_gpuTables[kGpuSceneTableCount] = {};
    std::vector<Retired> m_retired;
    std::vector<Geometry> m_geometry;
    std::vector<u32> m_indexMirror;
    Buffer m_indexBuffer{};
    u32 m_indexCapacity = 0; ///< u32 entries of m_indexBuffer
    std::vector<u8> m_scratch;
    std::vector<PendingCopy> m_pending;
    usize m_pendingBytes = 0;

    // Scatter upload.
    void* m_scatterPipeline = nullptr; ///< VkPipeline
    void* m_scatterLayout = nullptr;   ///< VkPipelineLayout
    const char* m_scatterKernel = "off";
    Buffer m_scatterBuffer{};
    u64 m_scatterCursor = 0; ///< bytes of m_scatterBuffer used by this frame's pending jobs
    std::vector<u8> m_scatterBlob;
    ScatterJob m_jobs[kMaxScatterJobs] = {};
    u32 m_jobCount = 0;
    u32 m_scatterTables = 0; ///< bit per table with pending jobs
    ScatterJob m_recordJobs[kMaxScatterJobs] = {}; ///< jobs of the pass added by importInto()
    u32 m_recordCount = 0;
};

} // namespace fuse::renderer::gpu_scene
