// FUSE Relight RL-5.6: the terrain bake on the GPU - TerrainBaker's rules (terrain_baker.hpp) as render-graph (WP-0.3
// v2) compute passes over a host-packed copy of the layers:
//
//   relight.terrain.clear   output <- the clear colour
//   relight.terrain.layer   (one per layer, in order) output <- blend(output, the layer's covering triangle, shaded)
//
// Each texel finds its covering triangle of the layer by testing the layer's triangles (the CPU reference rasterises
// each triangle's texel box: the same texels, the same tie rule), then shades and blends exactly like the CPU. Inputs
// (layer records, vertices, indices, texels) live in one host-visible buffer re-packed only when the content hash
// changes (the bake cache); the output is a host-visible f32x4 buffer (the path tracer's terrain texture source, or
// read back). Every pass declares its accesses; no manual barriers. Steady-state bakes make no heap allocation.
//
// Note: the plan's "RG raster passes" are compute passes here (a triangle loop per texel): exact parity with the CPU
// reference and no render-target plumbing; a raster-pipeline variant for large terrains is open.
#pragma once

#include <fuse/relight/terrain/terrain_baker.hpp>

#include <fuse/renderer/resources.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/types.hpp>

#include <span>
#include <vector>

namespace fuse::renderer {
class GpuAllocator;
class VulkanDevice;
} // namespace fuse::renderer

namespace fuse::relight::terrain {

enum class TerrainKernelLanguage : u8 { Auto = 0, Slang, Glsl };

struct TerrainBakerGpuDesc {
    renderer::VulkanDevice* device = nullptr;
    renderer::GpuAllocator* allocator = nullptr;
    renderer::BindlessDescriptors* bindless = nullptr; ///< required (pipeline layout / create flags)
    TerrainKernelLanguage language = TerrainKernelLanguage::Auto;
    u32 maxLayers = 32;
};

struct TerrainBakerGpuStats {
    u32 uploads = 0;      ///< content changes packed
    u32 bakes = 0;        ///< graphs recorded
    u32 reallocations = 0;
};

class TerrainBakerGpu {
public:
    TerrainBakerGpu();
    ~TerrainBakerGpu();
    TerrainBakerGpu(const TerrainBakerGpu&) = delete;
    TerrainBakerGpu& operator=(const TerrainBakerGpu&) = delete;

    bool init(const TerrainBakerGpuDesc& desc);
    void destroy();
    bool valid() const { return m_initialized; }
    const char* reason() const { return m_reason; }
    const char* kernelLanguage() const { return m_language; }

    /// Packs the inputs (skipped when the content hash is unchanged; `rebake` reports whether the output is stale).
    /// The caller has retired every frame that read the previous inputs when this reallocates.
    bool prepare(const TerrainBakeDesc& desc, std::span<const TerrainLayer> layers, bool* rebake = nullptr);
    /// The clear + per-layer passes; `output` is the buffer ref for later readers (declare HostRead to read back).
    bool addPasses(renderer::rg::Graph& graph, renderer::rg::BufferRef* output = nullptr);
    u32 collectRetired();

    const float* mappedOutput() const;
    u64 outputAddress() const;
    const TerrainBakerGpuStats& stats() const { return m_stats; }

private:
    struct Push {
        float clear[4] = {0.f, 0.f, 0.f, 0.f};
        u64 layers = 0;
        u64 vertices = 0;
        u64 indices = 0;
        u64 texels = 0;
        u64 outputs = 0;
        u32 layer = 0;
        u32 width = 0;
        u32 height = 0;
        u32 stage = 0;
        float minX = 0.f;
        float minZ = 0.f;
        float sx = 0.f;
        float sz = 0.f;
    };
    static_assert(sizeof(Push) == 88u, "TerrainPush (rl_terrain_bake.comp / .slang)");
    struct PassRecord {
        TerrainBakerGpu* self = nullptr;
        Push push{};
    };

    bool createPipeline();
    bool ensure(renderer::Buffer& buffer, u64 bytes, u32 memory, const char* name, u8* queue);
    static void record(const renderer::rg::PassContext& context, void* user);

    TerrainBakerGpuDesc m_desc{};
    bool m_initialized = false;
    const char* m_reason = "not initialised";
    const char* m_language = "none";
    renderer::Buffer m_input{};
    u8 m_inputQueue = 0xFFu;
    renderer::Buffer m_output{};
    u8 m_outputQueue = 0xFFu;
    std::vector<renderer::Buffer> m_retired;
    std::vector<PassRecord> m_records;
    TerrainBakeDesc m_bake{};
    u32 m_layerCount = 0;
    u64 m_offsets[4] = {}; ///< layers, vertices, indices, texels within m_input
    u64 m_inputBytes = 0;
    u64 m_hash = 0;
    bool m_prepared = false;
    void* m_layoutHandle = nullptr;
    void* m_pipeline = nullptr;
    TerrainBakerGpuStats m_stats{};
};

} // namespace fuse::relight::terrain
