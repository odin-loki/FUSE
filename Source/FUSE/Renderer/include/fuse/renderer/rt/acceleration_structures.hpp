#pragma once

// WP-6.0 acceleration structures (renderer plan Phase 6, tier T2; docs/unification/RENDERER-EXECUTION.md).
//
// One BLAS per GPU-scene mesh and one TLAS over the GPU scene's instance slots, built on the GPU
// inside the render graph (RG v2 declares every acceleration-structure access; no pass records a
// barrier by hand):
//
//   rt.blas.compact        vkCmdCopyAccelerationStructureKHR(COMPACT) of BLASes whose compacted size
//                          was read back (the old BLAS retires at the frame serial)
//   rt.blas.decode         compute: WP-1.2 VPOS (u16x4 quantised) -> f32x3 vertex input (decode arena)
//   rt.blas.build          vkCmdBuildAccelerationStructuresKHR: new BLASes (BUILD) and refits of
//                          deformed meshes (UPDATE, in place); index input = the scene index buffer
//                          at GpuMesh::firstIndex (primitiveOffset), so BLAS primitive t == mesh triangle t
//   rt.blas.compact_query  vkCmdWriteAccelerationStructuresPropertiesKHR(COMPACTED_SIZE) of new static BLASes
//   rt.tlas.instances      compute: GpuInstance + GpuTransform + BLAS address table -> AsInstance[]
//                          (rt_types.hpp packRtInstance). CPU fallback: the same packing on the host,
//                          uploaded through the UploadQueue (RtInstancePacking::Cpu, or no kernel built)
//   rt.tlas.build          TLAS BUILD (structure changed: slots, meshes, flags, BLAS addresses) or
//                          UPDATE (transforms / refit BLASes only; forced BUILD every maxTlasUpdates)
//
// Frame protocol (one owner thread; mirrors GpuScene):
//
//   scene.beginFrame(serial); rt.beginFrame(serial);
//   ... scene mutations, rt.deformMesh(mesh, positions) ...
//   scene.commit(); rt.commit();          // creates BLAS / TLAS objects, stages address table and
//                                         // (CPU packing) instances; never flushes
//   upload.flush();
//   GpuSceneGraphRefs sr = scene.importInto(graph);
//   RtGraphRefs rr = rt.importInto(graph, sr);    // declare before the passes that trace
//   graph.addPass("gi.trace", ...).use(rr.tlas, rg::Access::AccelerationStructureRead, {}, rg::kStageCompute);
//   executor.execute(graph);
//   ... scene.collectRetired(done); rt.collectRetired(done);   // also makes compaction sizes readable
//
// Compaction: static meshes build with ALLOW_COMPACTION | PREFER_FAST_TRACE; their compacted size is
// queried in the build frame and read back (no wait) once collectRetired() has seen that frame
// complete; the next commit() creates the compacted BLAS and records the copy, repoints the address
// table and rebuilds the TLAS. Deformable meshes (setMeshOptions) build with ALLOW_UPDATE |
// PREFER_FAST_BUILD and are refit in place by deformMesh().
//
// Steady-state frames (no new meshes, no growth, no compaction) make no heap allocations: every
// per-mesh / per-instance array is sized on growth only.
//
// Requires queryRtCapabilities(device).usable (T2 gate, rt_caps.hpp): init() fails with reason()
// otherwise and callers keep their T0 path. The stub backend builds this header and fails init().

#include <fuse/renderer/gpu_scene/gpu_scene.hpp>
#include <fuse/renderer/resources.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/rt/rt_caps.hpp>
#include <fuse/renderer/rt/rt_types.hpp>
#include <fuse/types.hpp>

#include <memory>
#include <vector>

namespace fuse::renderer {
class GpuAllocator;
class UploadQueue;
class VulkanDevice;
} // namespace fuse::renderer

namespace fuse::renderer::rt {

enum class RtInstancePacking : u8 {
    Auto = 0, ///< GPU kernel when built, else CPU
    Gpu,
    Cpu,
};

/// How dead TLAS slots (free, no BLAS, masked out) are encoded (rt_types.hpp packRtInstance).
enum class RtInactivePolicy : u8 {
    Auto = 0,    ///< Placeholder on drivers with RtCapabilities::inactiveInstanceQuirk, else Reference0
    Reference0,  ///< spec: reference 0, mask 0
    Placeholder, ///< any live BLAS, mask kRtMaskDead (never in a cull mask)
};

enum class RtKernelLanguage : u8 {
    Auto = 0, ///< Slang build if present, else GLSL
    Slang,
    Glsl,
};

struct AccelerationStructuresDesc {
    VulkanDevice* device = nullptr;
    GpuAllocator* allocator = nullptr;
    UploadQueue* upload = nullptr;
    gpu_scene::GpuScene* scene = nullptr;
    RtInstancePacking packing = RtInstancePacking::Auto;
    RtKernelLanguage language = RtKernelLanguage::Auto;
    RtInactivePolicy inactive = RtInactivePolicy::Auto;
    /// Compact static BLASes (query + copy). Off: every BLAS keeps its build size.
    bool compaction = true;
    /// Diagnostics: record the compaction copy even when the queried size is not smaller (drivers
    /// whose builder already allocates the exact size, e.g. Mesa lavapipe), so the copy / repoint /
    /// TLAS-rebuild path runs everywhere. Off in production (a copy that saves nothing).
    bool forceCompactionCopy = false;
    /// TLAS refits in a row before a forced rebuild (quality). 0 = always rebuild.
    u32 maxTlasUpdates = 64;
    /// Initial capacities (grow by doubling; the TLAS is sized for instanceCapacity instances).
    u32 instanceCapacity = 1024;
    u32 meshCapacity = 256;
    /// The frame's bindless heap: the rt.* compute pipelines bind no descriptors but follow its backend
    /// (VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT on the descriptor-buffer backend), so they may be
    /// dispatched after a bindless pass bound its descriptor buffers (VUID-vkCmdDispatch-None-08117).
    /// Null = the scene's heap (GpuSceneDesc::bindless); neither = plain pipelines.
    BindlessDescriptors* bindless = nullptr;
    const char* name = "rt";
};

struct RtMeshOptions {
    /// Refit with deformMesh() (ALLOW_UPDATE | PREFER_FAST_BUILD, never compacted).
    bool deformable = false;
};

enum class BlasState : u8 {
    None = 0,     ///< mesh not seen yet
    Unsupported,  ///< no scene index range or no positions (instances of it are inactive)
    Pending,      ///< object created, build recorded by this frame's importInto()
    Built,        ///< built; compaction query outstanding when compactQueued
    Compacted,    ///< replaced by its compacted copy
    Failed,
};

enum class TlasBuildMode : u8 {
    None = 0,
    Build,
    Update,
};

/// Per-mesh BLAS record (read-only view).
struct BlasInfo {
    BlasState state = BlasState::None;
    bool deformable = false;
    bool compactQueued = false; ///< compacted-size query written, result not read yet
    u64 address = 0;            ///< device address (0 = none)
    u64 size = 0;               ///< current accelerationStructureSize
    u64 buildSize = 0;          ///< size at build time (before compaction)
    u64 compactedSize = 0;      ///< queried compacted size (0 = not known)
    u32 triangles = 0;
    u32 vertices = 0;
    u32 updates = 0;            ///< refits since the build
    u64 buildSerial = 0;
};

struct RtCommitStats {
    u32 blasBuilds = 0;
    u32 blasUpdates = 0;
    u32 blasCompactions = 0;    ///< copies recorded this frame
    u32 compactionQueries = 0;  ///< compacted-size queries recorded this frame
    u32 decodedVertices = 0;
    TlasBuildMode tlas = TlasBuildMode::None;
    u32 instances = 0;          ///< TLAS primitive count (instance slots)
    bool cpuPacked = false;     ///< instances packed on the host this frame
    bool ok = true;
};

struct RtMemoryStats {
    u64 blasBytes = 0;     ///< sum of live BLAS accelerationStructureSize
    u64 blasBuildBytes = 0; ///< the same BLASes at their build size
    u64 tlasBytes = 0;
    u64 scratchBytes = 0;
    u64 instanceBytes = 0;
    u32 blasCount = 0;
    u32 compactedCount = 0;
};

/// Render-graph handles of this frame (valid after importInto()).
struct RtGraphRefs {
    rg::BufferRef tlas;      ///< declare AccelerationStructureRead to trace
    rg::BufferRef instances; ///< AsInstance[] (TLAS build input)
};

class AccelerationStructures {
public:
    AccelerationStructures();
    ~AccelerationStructures();
    AccelerationStructures(const AccelerationStructures&) = delete;
    AccelerationStructures& operator=(const AccelerationStructures&) = delete;

    /// False when the T2 gate fails (reason() says why) or a GPU object could not be created.
    bool init(const AccelerationStructuresDesc& desc);
    /// The caller must have retired every frame that used the structures.
    void destroy();
    bool ready() const { return m_ready; }
    const char* reason() const { return m_reason; }
    const RtCapabilities& capabilities() const { return m_caps; }

    // --- frame ----------------------------------------------------------------------------------
    void beginFrame(u64 frameSerial);
    /// After scene.commit(), before upload.flush().
    RtCommitStats commit();
    /// Adds this frame's rt.* passes (declare after scene.importInto(), before the tracers).
    RtGraphRefs importInto(rg::Graph& graph, const gpu_scene::GpuSceneGraphRefs& sceneRefs);
    /// Destroys objects retired at serials <= completedSerial; compaction sizes of frames up to it
    /// become readable.
    u32 collectRetired(u64 completedSerial);

    // --- meshes ---------------------------------------------------------------------------------
    /// Before the mesh's first commit (later calls are ignored for built meshes).
    bool setMeshOptions(u32 mesh, const RtMeshOptions& options);
    /// Refits the BLAS of a deformable mesh from `positions` (f32 x 3 per vertex, tightly packed,
    /// GpuMesh::vertexCount entries at `offset`; the buffer needs BufferUsage::AccelerationStructureBuildInput
    /// and must hold the new positions before the graph executes). Applied by the next commit();
    /// the TLAS is refit in the same frame.
    bool deformMesh(u32 mesh, const Buffer& positions, u64 offset = 0);
    const BlasInfo& blas(u32 mesh) const;
    u32 blasCount() const { return static_cast<u32>(m_blas.size()); }

    // --- results --------------------------------------------------------------------------------
    u64 tlasAddress() const { return m_tlasAddress; }
    const Buffer& tlasBuffer() const { return m_tlasBuffer; }
    void* tlasHandle() const { return m_tlas; } ///< VkAccelerationStructureKHR
    const Buffer& instanceBuffer() const { return m_instanceBuffer; }
    u32 tlasInstanceCount() const { return m_tlasCount; }
    bool tlasBuilt() const { return m_tlasBuilt; }
    RtMemoryStats memory() const;
    const RtCommitStats& lastCommit() const { return m_stats; }
    /// "slang", "glsl" or "none" (kernels in use), "gpu" / "cpu" (instance packing).
    const char* kernelLanguage() const { return m_language; }
    const char* packingName() const { return m_gpuPacking ? "gpu" : "cpu"; }
    /// Host copy of the BLAS address table (u64 per mesh; the CPU packing input).
    const u64* blasAddresses() const { return m_addressMirror.data(); }
    /// packRtInstance `inactiveBlas` / `inactiveMask` of this frame (0 / 0 with RtInactivePolicy::Reference0).
    u64 inactiveBlasAddress() const { return m_inactiveBlas; }
    u32 inactiveMask() const { return m_inactiveMask; }

private:
    struct Impl;
    struct Blas {
        BlasInfo info{};
        void* handle = nullptr; ///< VkAccelerationStructureKHR
        Buffer buffer{};
        u64 buildScratch = 0;
        u64 updateScratch = 0;
        u32 flags = 0; ///< VkBuildAccelerationStructureFlagsKHR
        // Deformation source of the next refit.
        void* deformBuffer = nullptr;
        u64 deformAddress = 0;
        u64 deformSize = 0;
        bool deformPending = false;
    };
    enum class OpKind : u8 { Build, Update, Compact };
    /// One BLAS operation recorded by this frame's passes.
    struct BlasOp {
        u32 mesh = 0;
        OpKind kind = OpKind::Build;
        bool query = false;    ///< Build: write the compacted-size query afterwards
        u64 vertexAddress = 0; ///< f32x3 input (decode arena or deformation buffer)
        u64 scratchOffset = 0; ///< from the aligned scratch base
        u64 scratchSize = 0;
        /// Build / Update: deformation buffer (VkBuffer, null = decode arena); Compact: old BLAS buffer.
        void* srcBuffer = nullptr;
        u64 srcSize = 0;
        void* srcHandle = nullptr; ///< Compact: old VkAccelerationStructureKHR
        void* dstHandle = nullptr; ///< Compact: new VkAccelerationStructureKHR
    };
    struct Retired {
        void* handle = nullptr; ///< VkAccelerationStructureKHR (may be null)
        Buffer buffer{};
        u64 serial = 0;
    };

    bool createKernels();
    void destroyKernels();
    bool createAsBuffer(Buffer& out, u64 bytes, const char* name);
    bool ensureBuffer(Buffer& buffer, u64 bytes, u32 usage, const char* name, bool mapped = false);
    bool ensureBlas(u32 mesh, RtCommitStats& stats);
    bool createBlasObject(u32 mesh, Blas& blas);
    bool ensureTlas(u32 instanceCapacity);
    bool readCompactions(RtCommitStats& stats);
    TlasBuildMode decideTlas(bool blasChanged);
    bool packInstancesCpu();
    bool uploadAddressTable();
    void retire(void* handle, Buffer& buffer);
    void destroyAs(void* handle, Buffer& buffer);
    static void recordCompact(const rg::PassContext& context, void* user);
    static void recordDecode(const rg::PassContext& context, void* user);
    static void recordBlasBuild(const rg::PassContext& context, void* user);
    static void recordQuery(const rg::PassContext& context, void* user);
    static void recordInstances(const rg::PassContext& context, void* user);
    static void recordTlas(const rg::PassContext& context, void* user);

    AccelerationStructuresDesc m_desc{};
    RtCapabilities m_caps{};
    std::unique_ptr<Impl> m_impl;
    bool m_ready = false;
    const char* m_reason = "not initialised";
    const char* m_language = "none";
    bool m_gpuPacking = false;
    u64 m_frameSerial = 0;
    u64 m_completedSerial = 0;
    u32 m_scratchAlignment = 256;

    std::vector<Blas> m_blas;             ///< indexed by mesh
    std::vector<RtMeshOptions> m_options; ///< indexed by mesh
    std::vector<u64> m_addressMirror;     ///< BLAS address per mesh (0 = none)
    bool m_addressDirty = false;
    Buffer m_addressBuffer{};
    u32 m_addressCapacity = 0;

    std::vector<BlasOp> m_ops;   ///< this frame's compact ops, then build / update ops
    u32 m_compactOps = 0;
    u32 m_decodeOps = 0;         ///< build ops needing the decode kernel (first m_decodeOps build ops)
    u32 m_queryOps = 0;          ///< build ops with a compaction query
    std::vector<rg::BufferRef> m_blasRefs; ///< per mesh, this frame
    std::vector<Retired> m_retired;

    Buffer m_decodeArena{};
    Buffer m_scratch{};
    u64 m_scratchUsed = 0;
    u64 m_tlasScratchOffset = 0;

    // TLAS.
    void* m_tlas = nullptr; ///< VkAccelerationStructureKHR
    Buffer m_tlasBuffer{};
    u64 m_tlasAddress = 0;
    u64 m_tlasBuildScratch = 0;
    u64 m_tlasUpdateScratch = 0;
    u32 m_tlasCapacity = 0; ///< instances the TLAS / instance buffer are sized for
    Buffer m_instanceBuffer{};
    std::vector<AsInstance> m_cpuInstances;
    std::vector<GpuInstance> m_lastInstances; ///< instance records at the last TLAS build / update
    std::vector<GpuTransform> m_lastTransforms;
    u32 m_tlasCount = 0;
    bool m_tlasBuilt = false;
    u32 m_tlasUpdates = 0;
    TlasBuildMode m_tlasMode = TlasBuildMode::None;
    bool m_blasRefitThisFrame = false;
    RtInstancesPush m_instancesPush{};
    u64 m_inactiveBlas = 0;
    u32 m_inactiveMask = 0;
    u64 m_lastInactiveBlas = 0;

    RtCommitStats m_stats{};
    BlasInfo m_noBlas{};
};

} // namespace fuse::renderer::rt
