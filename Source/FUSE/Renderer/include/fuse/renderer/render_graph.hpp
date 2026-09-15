#pragma once

#include <fuse/renderer/command_buffer.hpp>
#include <fuse/renderer/render_command_list.hpp>
#include <fuse/renderer/resources.hpp>
#include <fuse/renderer/vk/device.hpp>
#include <fuse/renderer/vk/frame.hpp>
#include <fuse/types.hpp>

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
};

/// CPU-side first/last pass indices for a graph resource (release stub deferred).
struct RGResourceLifetime {
    u32 resourceId = 0;
    bool isTexture = true;
    u32 firstPassIndex = static_cast<u32>(-1);
    u32 lastPassIndex = static_cast<u32>(-1);
    RGResourceLifetimePhase phase = RGResourceLifetimePhase::Unknown;
};

struct RenderGraphCompileInfo {
    u32 passCount = 0;
    u32 executablePassCount = 0;
    u32 culledPassCount = 0;
    u32 barrierCount = 0;
    u32 dependencyEdgeCount = 0;
    u32 resourceLifetimeCount = 0;
    bool compiled = false;
    bool usedDeclarationOrderFallback = false;
};

struct RenderGraphExecuteInfo {
    u32 executedPassCount = 0;
    u32 recordedCommands = 0;
};

/// Lightweight render graph — pass ordering, barrier planning, and stub command recording.
class RenderGraph {
public:
    static constexpr u32 kBackbufferTextureId = 1u;
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
                                   CommandBufferRecorder& recorder);

    const RenderGraphCompileInfo& compileInfo() const { return m_compileInfo; }
    const std::vector<RGBarrier>& plannedBarriers() const { return m_barriers; }
    const std::vector<RGPassDependencyEdge>& dependencyEdges() const { return m_dependencyEdges; }
    const std::vector<RGResourceLifetime>& resourceLifetimes() const { return m_resourceLifetimes; }
    /// Pass indices in execution order after `compile()` (non-culled passes only).
    const std::vector<u32>& compileOrder() const { return m_compileOrder; }
    u32 backbufferIndex() const { return m_backbufferIndex; }

private:
    struct PassNode {
        RGPassDesc desc{};
        u32 order = 0;
        bool culled = false;
        std::vector<RGTextureAccess> textureAccesses;
        std::vector<RGBufferAccess> bufferAccesses;
    };

    struct TextureState {
        RGImageLayout layout = RGImageLayout::Undefined;
        RGResourceAccess lastAccess = RGResourceAccess::ShaderRead;
        bool written = false;
        bool imported = false;
        bool transient = false;
        TextureHandle sourceHandle{};
    };

    RGImageLayout layoutForAccess(RGResourceAccess access) const;
    RGResourceAccess accessForLayout(RGImageLayout layout) const;
    TextureState& textureStateAt(u32 textureId);
    void planBarriersForPass(const PassNode& pass);
    void cullUnusedPasses();
    void buildDependencyEdges();
    void resolveCompileOrder();
    void assignResourceLifetimes();
    void assignExecutionOrder();

    u32 m_backbufferIndex = 0;
    u32 m_nextTextureId = kBackbufferTextureId + 1u;
    u32 m_nextBufferId = 1u;
    RenderGraphCompileInfo m_compileInfo{};
    std::vector<PassNode> m_passes;
    std::vector<RGBarrier> m_barriers;
    std::vector<TextureState> m_textureStates;
    std::vector<RGPassDependencyEdge> m_explicitEdges;
    std::vector<RGPassDependencyEdge> m_dependencyEdges;
    std::vector<RGResourceLifetime> m_resourceLifetimes;
    std::vector<u32> m_compileOrder;
};

void populateRenderGraphFromCommandList(RenderGraph& graph,
                                        const RenderCommandList& commands,
                                        float compositeBlend = 0.5f);

} // namespace fuse::renderer
