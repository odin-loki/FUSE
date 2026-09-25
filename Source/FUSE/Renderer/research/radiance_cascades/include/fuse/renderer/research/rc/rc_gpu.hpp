#pragma once

// WP-6.5 radiance cascades (research): the flatland prototype on Vulkan compute, render graph v2.
//
//   rc.beginFrame(serial, settings, width, height, sceneBuffer.deviceAddress);
//   RcGraphRefs refs = rc.importInto(graph);
//   rc.addPasses(graph, refs, sceneRef);     // rc.cascade (top cascade first) x n, then rc.gather
//   rc.addCopyOutput(graph, refs, readbackRef, 0);
//
// Passes (compute, 64 threads, 1D; all declared on the graph, no manual barriers):
//   rc.cascade   one per cascade, coarse to fine: trace the interval of every (probe, direction) record and
//                merge with the cascade above (vanilla or bilinear fix; rc_cascade_record is the CPU twin)
//   rc.gather    mean radiance per pixel from cascade 0 (rc_gather_pixel)
// Storage: the caller's scene (f32x4 per texel, BDA, declared StorageRead), one persistent work buffer (two
// cascade ping-pong sections + the f32x4 output) and a host-written direction table; both follow the layout
// (extent + settings) and are retired, never touched in steady state, so steady-state frames make no heap
// allocation. Kernels: Slang primary (-fp-mode precise) with GLSL twins (`precise`), embedded by
// cmake/rp_wp65.cmake.

#include <fuse/renderer/research/rc/rc_reference.hpp>
#include <fuse/renderer/research/rc/rc_types.hpp>
#include <fuse/renderer/resources.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::renderer {
class GpuAllocator;
class VulkanDevice;
class BindlessDescriptors;
} // namespace fuse::renderer

namespace fuse::renderer::research::rc {

enum class RcKernelLanguage : u8 {
    Auto = 0, ///< Slang if built, else GLSL
    Slang,
    Glsl,
};

struct RcCapabilities {
    bool rc = false;
    const char* reason = "no device"; ///< "ok" when usable
};

RcCapabilities queryRcCapabilities(const VulkanDevice* device);

struct RcGpuDesc {
    VulkanDevice* device = nullptr;
    GpuAllocator* allocator = nullptr;
    /// Optional: when set, the pipelines use its set layout / create flags and bind it (so the passes can
    /// follow bindless passes on the descriptor-buffer backend); the kernels themselves use no descriptor.
    BindlessDescriptors* bindless = nullptr;
    RcKernelLanguage language = RcKernelLanguage::Auto;
};

/// Byte layout of the work buffer (every section 256-aligned).
struct RcBufferLayout {
    u64 cascade[2] = {0, 0}; ///< f32x4 per record (maxRecords)
    u64 output = 0;          ///< f32x4 per pixel
    u64 workBytes = 0;
    static RcBufferLayout compute(const RcLayout& layout, u32 width, u32 height);
};

struct RcGraphRefs {
    rg::BufferRef work;
};

struct RcGpuStats {
    u32 layoutRebuilds = 0; ///< work buffer + direction table rebuilds
    u32 retired = 0;
    u32 passes = 0; ///< passes added this frame
};

class RadianceCascadesGpu {
public:
    RadianceCascadesGpu() = default;
    ~RadianceCascadesGpu();
    RadianceCascadesGpu(const RadianceCascadesGpu&) = delete;
    RadianceCascadesGpu& operator=(const RadianceCascadesGpu&) = delete;

    /// False (nothing created) without a capable device, a built kernel of the requested language, or in the
    /// stub backend.
    bool init(const RcGpuDesc& desc);
    /// The caller must have retired every frame that used the buffers.
    void destroy();
    bool valid() const { return m_initialized; }

    /// Resolves this frame's cascade layout; rebuilds the work buffer / direction table when it changed.
    /// False for an invalid layout, a zero scene address or a failed allocation.
    bool beginFrame(u64 frameSerial, const RcSettings& settings, u32 width, u32 height, u64 sceneAddress);
    RcGraphRefs importInto(rg::Graph& graph);
    /// rc.cascade x cascades (top first), rc.gather. `scene` is the caller's import of the scene buffer.
    void addPasses(rg::Graph& graph, const RcGraphRefs& refs, rg::BufferRef scene);
    /// Copies the f32x4 output (outputBytes()) into dst at dstOffset (after addPasses).
    void addCopyOutput(rg::Graph& graph, const RcGraphRefs& refs, rg::BufferRef dst, u64 dstOffset);
    u64 outputBytes() const { return static_cast<u64>(m_width) * m_height * 16u; }
    /// BDA of the output section (f32x4 per pixel, valid after rc.gather).
    u64 outputAddress() const { return m_work.deviceAddress != 0u ? m_work.deviceAddress + m_buffers.output : 0u; }

    /// Destroys buffers retired at serials <= completedSerial.
    u32 collectRetired(u64 completedSerial);

    const char* kernelLanguage() const { return m_language; }
    const RcLayout& layout() const { return m_layout; }
    const RcBufferLayout& bufferLayout() const { return m_buffers; }
    const RcGpuStats& stats() const { return m_stats; }

private:
    struct Retired {
        Buffer work{};
        Buffer dirs{};
        u64 serial = 0;
    };
    struct PassRecord {
        RadianceCascadesGpu* self = nullptr;
        RcPush push{};
        u32 kernel = 0;
        u32 groups = 1;
        u64 copySrc = 0;
        u64 copyDst = 0;
        u64 copyBytes = 0;
        rg::BufferRef copyFrom{};
        rg::BufferRef copyTo{};
    };
    enum Kernel : u32 { kCascade = 0, kGather, kKernelCount };
    static constexpr u32 kMaxPasses = kRcMaxCascades + 4u;

    bool createPipelines();
    bool rebuild(const RcLayout& layout, u32 width, u32 height);
    void destroyRetired(Retired& r);
    PassRecord* nextRecord(u32 kernel);
    static void recordDispatch(const rg::PassContext& context, void* user);
    static void recordCopy(const rg::PassContext& context, void* user);

    RcGpuDesc m_desc{};
    bool m_initialized = false;
    const char* m_language = "none";
    u64 m_frameSerial = 0;
    u32 m_width = 0;
    u32 m_height = 0;
    u64 m_scene = 0;
    RcSettings m_settings{};
    RcLayout m_layout{};
    RcBufferLayout m_buffers{};
    Buffer m_work{};
    u8 m_workQueue = rg::kNoQueue;
    Buffer m_dirs{};
    std::vector<Retired> m_retired;
    std::vector<f32> m_dirScratch;

    PassRecord m_records[kMaxPasses] = {};
    u32 m_recordCount = 0;

    void* m_layoutHandle = nullptr; ///< VkPipelineLayout (optional bindless set + 112-byte push, compute)
    void* m_pipelines[kKernelCount] = {};
    RcGpuStats m_stats{};
};

} // namespace fuse::renderer::research::rc
