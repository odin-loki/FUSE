#pragma once

#include <fuse/renderer/command_buffer.hpp>
#include <fuse/renderer/draw_list.hpp>
#include <fuse/renderer/render_command_list.hpp>
#include <fuse/renderer/resources.hpp>
#include <fuse/renderer/vk/device.hpp>
#include <fuse/renderer/vk/frame.hpp>
#include <fuse/types.hpp>

#include <span>
#include <vector>

namespace fuse::renderer {

enum class RGResourceAccess : u32 {
    ColorAttachmentWrite = 1,
    DepthAttachmentWrite = 2,
    ShaderRead = 3,
    ShaderWrite = 4,
    TransferSrc = 5,
    TransferDst = 6,
    Present = 7,
    CUDAWrite = 8,
    CUDARead = 9,
};

enum class RGImageLayout : u32 {
    Undefined = 0,
    ColorAttachment = 1,
    DepthAttachment = 2,
    ShaderReadOnly = 3,
    TransferSrc = 4,
    TransferDst = 5,
    PresentSrc = 6,
    General = 7,
};

struct RGTextureRef {
    u32 id = 0;
};

struct RGBufferRef {
    u32 id = 0;
};

struct RGTextureAccess {
    RGTextureRef texture;
    RGResourceAccess access = RGResourceAccess::ShaderRead;
};

struct RGBufferAccess {
    RGBufferRef buffer;
    RGResourceAccess access = RGResourceAccess::ShaderRead;
};

struct RGBarrier {
    RGTextureRef texture;
    RGImageLayout fromLayout = RGImageLayout::Undefined;
    RGImageLayout toLayout = RGImageLayout::Undefined;
    RGResourceAccess fromAccess = RGResourceAccess::ShaderRead;
    RGResourceAccess toAccess = RGResourceAccess::ShaderRead;
    /// Pass the barrier is recorded in front of (execute() emits barriers per pass).
    u32 passIndex = 0;
};

struct RGBufferBarrier {
    RGBufferRef buffer;
    RGResourceAccess fromAccess = RGResourceAccess::ShaderRead;
    RGResourceAccess toAccess = RGResourceAccess::ShaderRead;
    u32 passIndex = 0;
};

using RGPassExecuteFn = void (*)(void* commandBuffer, void* userData);

struct RGPassDesc {
    const char* name = nullptr;
    RGPassExecuteFn execute = nullptr;
    void* userData = nullptr;
    const RGTextureAccess* textureAccesses = nullptr;
    u32 textureAccessCount = 0;
    const RGBufferAccess* bufferAccesses = nullptr;
    u32 bufferAccessCount = 0;
    bool isCompute = false;
    bool isCuda = false;
};

enum class RGPassDependencyKind : u32 {
    Explicit = 0,
    ResourceAccess = 1,
};

/// Directed edge: `fromPassIndex` must complete before `toPassIndex` runs.
struct RGPassDependencyEdge {
    u32 fromPassIndex = 0;
    u32 toPassIndex = 0;
    RGPassDependencyKind kind = RGPassDependencyKind::Explicit;
};

enum class RGResourceLifetimePhase : u32 {
    Unknown = 0,
    Imported = 1,
    TransientCreated = 2,
    Released = 4,
};

/// CPU-side first/last pass indices for a graph resource. Transients are marked
/// Released after their last pass during execute() (alias groups assigned at compile).
struct RGResourceLifetime {
    u32 resourceId = 0;
    bool isTexture = true;
    u32 firstPassIndex = static_cast<u32>(-1);
    u32 lastPassIndex = static_cast<u32>(-1);
    RGResourceLifetimePhase phase = RGResourceLifetimePhase::Unknown;
    /// Non-zero alias group id for transients; 0 = none (imported / unassigned).
    u32 aliasGroup = 0;
};

struct RenderGraphCompileInfo {
    u32 passCount = 0;
    u32 executablePassCount = 0;
    u32 culledPassCount = 0;
    u32 barrierCount = 0;
    u32 bufferBarrierCount = 0;
    u32 dependencyEdgeCount = 0;
    u32 resourceLifetimeCount = 0;
    u32 aliasGroups = 0;
    u32 compileDurationUs = 0;
    bool compiled = false;
    bool usedDeclarationOrderFallback = false;
};

struct RenderGraphExecuteInfo {
    u32 executedPassCount = 0;
    u32 recordedCommands = 0;
    u32 cudaPassCount = 0;
    u32 computePassCount = 0;
    u32 bufferBarrierCount = 0;
    u32 scratchBytesUsed = 0;
    u32 transientsReleased = 0;
    u32 aliasGroups = 0;
    u32 executeDurationUs = 0;
};

/// Render graph v1 — kept as the facade the B5 schedules, the deferred pipeline and the CUDA passes
/// compile against (WP-0.3). Pass ordering, culling, lifetimes and alias groups are unchanged;
/// execute() now records each pass's barriers directly in front of that pass (per-pass batches,
/// encoded by the render graph module, src/rg/). There is no pass cap any more.
///
/// New code should use render graph v2 (`fuse/renderer/rg/graph.hpp` + `rg/executor.hpp`):
/// synchronization2 barriers derived from declared accesses on real VkImage/VkBuffer resources,
/// subresource and byte ranges, aliased transient memory, async compute / transfer queues with
/// timeline semaphores and queue-family ownership transfers, and per-pass debug labels.
class RenderGraph {
public:
    static constexpr u32 kBackbufferTextureId = 1u;
    static constexpr u32 kDepthTextureId = 2u;
    /// Size of the per-feature static pass storage the v1 populate helpers keep (sky, shadow,
    /// composite, ...). No longer a graph limit: addPass() accepts any number of passes (pools grow
    /// once and keep their capacity, so warm frames still do not allocate).
    static constexpr u32 kMaxPassesPerFrame = 32u;

    void reset();
    void beginFrame(u32 backbufferIndex);

    RGTextureRef importTexture(TextureHandle handle, RGImageLayout currentLayout);
    RGBufferRef importBuffer(BufferHandle handle);
    RGTextureRef createTransient(const TextureDesc& desc);

    void addPass(const RGPassDesc& desc);
    void addPassDependency(u32 fromPassIndex, u32 toPassIndex);
    void compile();

    RenderGraphExecuteInfo execute(VulkanDevice& device,
                                   FrameManager& frames,
                                   CommandBufferRecorder& recorder,
                                   const VkFrameEncodeContext* encodeContext = nullptr);

    const RenderGraphCompileInfo& compileInfo() const { return m_compileInfo; }
    const std::vector<RGBarrier>& plannedBarriers() const { return m_barriers; }
    const std::vector<RGBufferBarrier>& plannedBufferBarriers() const { return m_bufferBarriers; }
    const std::vector<RGPassDependencyEdge>& dependencyEdges() const { return m_dependencyEdges; }
    const std::vector<RGResourceLifetime>& resourceLifetimes() const { return m_resourceLifetimes; }
    /// Pass indices in execution order after `compile()` (non-culled passes only).
    const std::vector<u32>& compileOrder() const { return m_compileOrder; }
    BufferHandle importedBufferHandle(u32 bufferId) const;
    u32 backbufferIndex() const { return m_backbufferIndex; }

private:
    /// Accesses live in the graph-wide pools below (ranges per pass) so steady-state frames reuse
    /// capacity instead of allocating per pass (B2.11 zero per-frame heap allocations).
    struct PassNode {
        RGPassDesc desc{};
        u32 order = 0;
        bool culled = false;
        u32 textureAccessBegin = 0;
        u32 textureAccessCount = 0;
        u32 bufferAccessBegin = 0;
        u32 bufferAccessCount = 0;
    };

    struct TextureState {
        RGImageLayout layout = RGImageLayout::Undefined;
        RGResourceAccess lastAccess = RGResourceAccess::ShaderRead;
        bool written = false;
        bool imported = false;
        bool transient = false;
        TextureHandle sourceHandle{};
    };

    struct BufferState {
        RGResourceAccess lastAccess = RGResourceAccess::ShaderRead;
        bool written = false;
        bool imported = false;
        bool touched = false;
        BufferHandle sourceHandle{};
    };

    RGImageLayout layoutForAccess(RGResourceAccess access) const;
    RGResourceAccess accessForLayout(RGImageLayout layout) const;
    std::span<const RGTextureAccess> textureAccessesOf(const PassNode& pass) const;
    std::span<const RGBufferAccess> bufferAccessesOf(const PassNode& pass) const;
    TextureState& textureStateAt(u32 textureId);
    BufferState& bufferStateAt(u32 id);
    void planBarriersForPass(u32 passIndex, const PassNode& pass);
    void cullUnusedPasses();
    void buildDependencyEdges();
    void resolveCompileOrder();
    void assignResourceLifetimes();
    void assignTransientAliasGroups();
    void assignExecutionOrder();

    u32 m_backbufferIndex = 0;
    u32 m_nextTextureId = kDepthTextureId + 1u;
    u32 m_nextBufferId = 1u;
    RenderGraphCompileInfo m_compileInfo{};
    std::vector<PassNode> m_passes;
    std::vector<RGBarrier> m_barriers;
    std::vector<RGBufferBarrier> m_bufferBarriers;
    std::vector<TextureState> m_textureStates;
    std::vector<BufferState> m_bufferStates;
    std::vector<RGPassDependencyEdge> m_explicitEdges;
    std::vector<RGPassDependencyEdge> m_dependencyEdges;
    std::vector<RGResourceLifetime> m_resourceLifetimes;
    std::vector<u32> m_compileOrder;
    std::vector<RGTextureAccess> m_textureAccessPool;
    std::vector<RGBufferAccess> m_bufferAccessPool;
    // compile() scratch — cleared, never shrunk, so warm frames do not touch the heap.
    std::vector<u8> m_scratchRequired;
    std::vector<u32> m_scratchLastTexturePass;
    std::vector<u32> m_scratchLastBufferPass;
    std::vector<u32> m_scratchActivePasses;
    std::vector<u32> m_scratchIndegree;
    std::vector<u8> m_scratchEmitted;
};

void populateRenderGraphFromCommandList(RenderGraph& graph,
                                        const RenderCommandList& commands,
                                        float compositeBlend = 0.5f);

/// Raster mesh path — one "meshes" pass plus present. `draws` must outlive compile() and execute().
/// Empty lists add no passes.
void populateRenderGraphFromDrawList(RenderGraph& graph, const DrawList& draws);

} // namespace fuse::renderer
