#include <fuse/renderer/composite_pass.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <vector>

namespace fuse::renderer {
namespace {

struct ClearPassUserData {
    float r = 0.f;
    float g = 0.f;
    float b = 0.f;
};

struct DrawPassUserData {
    u32 drawCount = 0;
};

void executeClearPass(void* commandBuffer, void* userData) {
    auto* recorder = static_cast<CommandBufferRecorder*>(commandBuffer);
    const auto* clear = static_cast<const ClearPassUserData*>(userData);
    if (recorder == nullptr || clear == nullptr) {
        return;
    }
    recorder->clearColor(clear->r, clear->g, clear->b);
}

void executeDrawPass(void* commandBuffer, void* userData) {
    auto* recorder = static_cast<CommandBufferRecorder*>(commandBuffer);
    const auto* draw = static_cast<const DrawPassUserData*>(userData);
    if (recorder == nullptr || draw == nullptr) {
        return;
    }
    recorder->draw(draw->drawCount);
}

void executePresentPass(void* commandBuffer, void* /*userData*/) {
    auto* recorder = static_cast<CommandBufferRecorder*>(commandBuffer);
    if (recorder == nullptr) {
        return;
    }
    recorder->present();
}

void executeDrawListPass(void* commandBuffer, void* userData) {
    auto* recorder = static_cast<CommandBufferRecorder*>(commandBuffer);
    auto* list = static_cast<const DrawList*>(userData);
    if (recorder && list) {
        list->record(*recorder);
    }
}

} // namespace

RenderGraph::TextureState& RenderGraph::textureStateAt(u32 textureId) {
    if (textureId >= m_textureStates.size()) {
        m_textureStates.resize(textureId + 1u);
    }
    return m_textureStates[textureId];
}

RenderGraph::BufferState& RenderGraph::bufferStateAt(u32 id) {
    if (id >= m_bufferStates.size()) {
        m_bufferStates.resize(id + 1u);
    }
    return m_bufferStates[id];
}

std::span<const RGTextureAccess> RenderGraph::textureAccessesOf(const PassNode& pass) const {
    return {m_textureAccessPool.data() + pass.textureAccessBegin, pass.textureAccessCount};
}

std::span<const RGBufferAccess> RenderGraph::bufferAccessesOf(const PassNode& pass) const {
    return {m_bufferAccessPool.data() + pass.bufferAccessBegin, pass.bufferAccessCount};
}

BufferHandle RenderGraph::importedBufferHandle(u32 bufferId) const {
    if (bufferId >= m_bufferStates.size()) {
        return {};
    }
    return m_bufferStates[bufferId].sourceHandle;
}

void RenderGraph::reset() {
    m_backbufferIndex = 0;
    m_nextTextureId = kDepthTextureId + 1u;
    m_nextBufferId = 1u;
    m_compileInfo = {};
    m_passes.clear();
    m_barriers.clear();
    m_bufferBarriers.clear();
    m_textureStates.clear();
    m_bufferStates.clear();
    m_explicitEdges.clear();
    m_dependencyEdges.clear();
    m_resourceLifetimes.clear();
    m_compileOrder.clear();
    m_textureAccessPool.clear();
    m_bufferAccessPool.clear();

    TextureState& backbuffer = textureStateAt(kBackbufferTextureId);
    backbuffer.imported = true;
    backbuffer.layout = RGImageLayout::Undefined;

    TextureState& depth = textureStateAt(kDepthTextureId);
    depth.imported = true;
    depth.layout = RGImageLayout::Undefined;
}

void RenderGraph::beginFrame(u32 backbufferIndex) {
    reset();
    m_backbufferIndex = backbufferIndex;
}

RGTextureRef RenderGraph::importTexture(TextureHandle handle, RGImageLayout currentLayout) {
    RGTextureRef ref{m_nextTextureId++};
    TextureState& state = textureStateAt(ref.id);
    state.imported = true;
    state.sourceHandle = handle;
    state.layout = currentLayout;
    return ref;
}

RGBufferRef RenderGraph::importBuffer(BufferHandle handle) {
    RGBufferRef ref{m_nextBufferId++};
    BufferState& state = bufferStateAt(ref.id);
    state.imported = true;
    state.sourceHandle = handle;
    return ref;
}

RGTextureRef RenderGraph::createTransient(const TextureDesc& /*desc*/) {
    RGTextureRef ref{m_nextTextureId++};
    TextureState& state = textureStateAt(ref.id);
    state.transient = true;
    state.layout = RGImageLayout::Undefined;
    return ref;
}

void RenderGraph::addPass(const RGPassDesc& desc) {
    PassNode node;
    node.desc = desc;
    node.textureAccessBegin = static_cast<u32>(m_textureAccessPool.size());
    node.bufferAccessBegin = static_cast<u32>(m_bufferAccessPool.size());
    if (desc.textureAccesses != nullptr && desc.textureAccessCount > 0u) {
        m_textureAccessPool.insert(m_textureAccessPool.end(), desc.textureAccesses,
                                   desc.textureAccesses + desc.textureAccessCount);
        node.textureAccessCount = desc.textureAccessCount;
    }
    if (desc.bufferAccesses != nullptr && desc.bufferAccessCount > 0u) {
        m_bufferAccessPool.insert(m_bufferAccessPool.end(), desc.bufferAccesses,
                                  desc.bufferAccesses + desc.bufferAccessCount);
        node.bufferAccessCount = desc.bufferAccessCount;
    }
    m_passes.push_back(node);
}

void RenderGraph::addPassDependency(u32 fromPassIndex, u32 toPassIndex) {
    if (fromPassIndex >= m_passes.size() || toPassIndex >= m_passes.size() ||
        fromPassIndex == toPassIndex) {
        return;
    }

    const auto duplicate = std::find_if(m_explicitEdges.begin(), m_explicitEdges.end(),
                                        [&](const RGPassDependencyEdge& edge) {
                                            return edge.fromPassIndex == fromPassIndex &&
                                                   edge.toPassIndex == toPassIndex;
                                        });
    if (duplicate != m_explicitEdges.end()) {
        return;
    }

    m_explicitEdges.push_back({fromPassIndex, toPassIndex, RGPassDependencyKind::Explicit});
}

RGImageLayout RenderGraph::layoutForAccess(RGResourceAccess access) const {
    switch (access) {
    case RGResourceAccess::ColorAttachmentWrite:
        return RGImageLayout::ColorAttachment;
    case RGResourceAccess::DepthAttachmentWrite:
        return RGImageLayout::DepthAttachment;
    case RGResourceAccess::ShaderRead:
        return RGImageLayout::ShaderReadOnly;
    case RGResourceAccess::ShaderWrite:
        return RGImageLayout::General;
    case RGResourceAccess::TransferSrc:
        return RGImageLayout::TransferSrc;
    case RGResourceAccess::TransferDst:
        return RGImageLayout::TransferDst;
    case RGResourceAccess::Present:
        return RGImageLayout::PresentSrc;
    case RGResourceAccess::CUDAWrite:
    case RGResourceAccess::CUDARead:
        return RGImageLayout::General;
    default:
        return RGImageLayout::Undefined;
    }
}

RGResourceAccess RenderGraph::accessForLayout(RGImageLayout layout) const {
    switch (layout) {
    case RGImageLayout::ColorAttachment:
        return RGResourceAccess::ColorAttachmentWrite;
    case RGImageLayout::DepthAttachment:
        return RGResourceAccess::DepthAttachmentWrite;
    case RGImageLayout::ShaderReadOnly:
        return RGResourceAccess::ShaderRead;
    case RGImageLayout::TransferSrc:
        return RGResourceAccess::TransferSrc;
    case RGImageLayout::TransferDst:
        return RGResourceAccess::TransferDst;
    case RGImageLayout::PresentSrc:
        return RGResourceAccess::Present;
    case RGImageLayout::General:
        return RGResourceAccess::ShaderWrite;
    default:
        return RGResourceAccess::ShaderRead;
    }
}

void RenderGraph::planBarriersForPass(u32 passIndex, const PassNode& pass) {
    for (const RGTextureAccess& access : textureAccessesOf(pass)) {
        TextureState& state = textureStateAt(access.texture.id);
        const RGImageLayout requiredLayout = layoutForAccess(access.access);

        const bool importedOrCreated = state.imported || state.transient;
        const bool planInitialDepth =
            importedOrCreated && requiredLayout == RGImageLayout::DepthAttachment;
        if (state.layout != requiredLayout &&
            (state.layout != RGImageLayout::Undefined || planInitialDepth)) {
            RGBarrier barrier;
            barrier.texture = access.texture;
            barrier.fromLayout = state.layout;
            barrier.toLayout = requiredLayout;
            barrier.fromAccess = state.lastAccess;
            barrier.toAccess = access.access;
            barrier.passIndex = passIndex;
            m_barriers.push_back(barrier);
        }

        state.layout = requiredLayout;
        state.lastAccess = access.access;
        state.written = state.written ||
                        access.access == RGResourceAccess::ColorAttachmentWrite ||
                        access.access == RGResourceAccess::DepthAttachmentWrite ||
                        access.access == RGResourceAccess::ShaderWrite ||
                        access.access == RGResourceAccess::TransferDst ||
                        access.access == RGResourceAccess::CUDAWrite;
    }

    for (const RGBufferAccess& access : bufferAccessesOf(pass)) {
        BufferState& state = bufferStateAt(access.buffer.id);
        if (state.touched && state.lastAccess != access.access) {
            RGBufferBarrier barrier;
            barrier.buffer = access.buffer;
            barrier.fromAccess = state.lastAccess;
            barrier.toAccess = access.access;
            barrier.passIndex = passIndex;
            m_bufferBarriers.push_back(barrier);
        }

        state.touched = true;
        state.lastAccess = access.access;
        state.written = state.written ||
                        access.access == RGResourceAccess::ShaderWrite ||
                        access.access == RGResourceAccess::TransferDst ||
                        access.access == RGResourceAccess::CUDAWrite;
    }
}

void RenderGraph::cullUnusedPasses() {
    if (m_passes.empty()) {
        return;
    }

    std::vector<u8>& required = m_scratchRequired;
    required.assign(m_passes.size(), 0u);
    for (u32 i = 0; i < m_passes.size(); ++i) {
        for (const RGTextureAccess& access : textureAccessesOf(m_passes[i])) {
            if (access.access == RGResourceAccess::Present) {
                required[i] = true;
            }
        }
        if (m_passes[i].desc.isCuda || m_passes[i].desc.isCompute) {
            required[i] = true;
        }
    }

    bool changed = true;
    while (changed) {
        changed = false;
        for (u32 i = 0; i < m_passes.size(); ++i) {
            if (!required[i]) {
                continue;
            }
            for (u32 producer = 0; producer < i; ++producer) {
                if (required[producer]) {
                    continue;
                }
                for (const RGTextureAccess& producerAccess : textureAccessesOf(m_passes[producer])) {
                    for (const RGTextureAccess& consumerAccess : textureAccessesOf(m_passes[i])) {
                        if (producerAccess.texture.id == consumerAccess.texture.id &&
                            producerAccess.access != RGResourceAccess::Present) {
                            required[producer] = true;
                            changed = true;
                        }
                    }
                }
            }
            for (const RGPassDependencyEdge& edge : m_explicitEdges) {
                if (edge.toPassIndex == i && edge.fromPassIndex < m_passes.size() &&
                    !required[edge.fromPassIndex]) {
                    required[edge.fromPassIndex] = true;
                    changed = true;
                }
            }
        }
    }

    for (u32 i = 0; i < m_passes.size(); ++i) {
        m_passes[i].culled = required[i] == 0u;
    }
}

void RenderGraph::buildDependencyEdges() {
    m_dependencyEdges = m_explicitEdges;

    auto addResourceEdge = [&](u32 fromPassIndex, u32 toPassIndex) {
        if (fromPassIndex == toPassIndex || fromPassIndex >= m_passes.size() ||
            toPassIndex >= m_passes.size()) {
            return;
        }
        if (m_passes[fromPassIndex].culled || m_passes[toPassIndex].culled) {
            return;
        }

        const auto duplicate = std::find_if(m_dependencyEdges.begin(), m_dependencyEdges.end(),
                                            [&](const RGPassDependencyEdge& edge) {
                                                return edge.fromPassIndex == fromPassIndex &&
                                                       edge.toPassIndex == toPassIndex;
                                            });
        if (duplicate != m_dependencyEdges.end()) {
            return;
        }

        m_dependencyEdges.push_back(
            {fromPassIndex, toPassIndex, RGPassDependencyKind::ResourceAccess});
    };

    // Resource ids are small and dense (per-frame counters), so "last pass touching id" is a flat
    // table instead of a hash map.
    constexpr u32 kNoPass = static_cast<u32>(-1);
    u32 maxTextureId = 0;
    u32 maxBufferId = 0;
    for (const RGTextureAccess& access : m_textureAccessPool) {
        maxTextureId = std::max(maxTextureId, access.texture.id);
    }
    for (const RGBufferAccess& access : m_bufferAccessPool) {
        maxBufferId = std::max(maxBufferId, access.buffer.id);
    }
    std::vector<u32>& lastTexturePass = m_scratchLastTexturePass;
    std::vector<u32>& lastBufferPass = m_scratchLastBufferPass;
    lastTexturePass.assign(static_cast<usize>(maxTextureId) + 1u, kNoPass);
    lastBufferPass.assign(static_cast<usize>(maxBufferId) + 1u, kNoPass);

    for (u32 passIndex = 0; passIndex < m_passes.size(); ++passIndex) {
        if (m_passes[passIndex].culled) {
            continue;
        }

        for (const RGTextureAccess& access : textureAccessesOf(m_passes[passIndex])) {
            u32& previous = lastTexturePass[access.texture.id];
            if (previous != kNoPass) {
                addResourceEdge(previous, passIndex);
            }
            previous = passIndex;
        }

        for (const RGBufferAccess& access : bufferAccessesOf(m_passes[passIndex])) {
            u32& previous = lastBufferPass[access.buffer.id];
            if (previous != kNoPass) {
                addResourceEdge(previous, passIndex);
            }
            previous = passIndex;
        }
    }
}

void RenderGraph::resolveCompileOrder() {
    m_compileOrder.clear();
    m_compileInfo.usedDeclarationOrderFallback = false;

    std::vector<u32>& activePasses = m_scratchActivePasses;
    activePasses.clear();
    for (u32 passIndex = 0; passIndex < m_passes.size(); ++passIndex) {
        if (!m_passes[passIndex].culled) {
            activePasses.push_back(passIndex);
        }
    }

    if (activePasses.empty()) {
        return;
    }

    // Kahn's algorithm over the pass list, always taking the lowest ready pass
    // index (deterministic, declaration-order tie break). Edges touching culled passes are ignored.
    const usize passCount = m_passes.size();
    std::vector<u32>& indegree = m_scratchIndegree;
    std::vector<u8>& emitted = m_scratchEmitted;
    indegree.assign(passCount, 0u);
    emitted.assign(passCount, 0u);
    auto edgeIsActive = [&](const RGPassDependencyEdge& edge) {
        return edge.fromPassIndex < passCount && edge.toPassIndex < passCount &&
               !m_passes[edge.fromPassIndex].culled && !m_passes[edge.toPassIndex].culled;
    };
    for (const RGPassDependencyEdge& edge : m_dependencyEdges) {
        if (edgeIsActive(edge)) {
            ++indegree[edge.toPassIndex];
        }
    }

    for (usize step = 0; step < activePasses.size(); ++step) {
        u32 current = static_cast<u32>(-1);
        for (const u32 passIndex : activePasses) {
            if (emitted[passIndex] == 0u && indegree[passIndex] == 0u) {
                current = passIndex;
                break;
            }
        }
        if (current == static_cast<u32>(-1)) {
            break; // cycle
        }
        emitted[current] = 1u;
        m_compileOrder.push_back(current);
        for (const RGPassDependencyEdge& edge : m_dependencyEdges) {
            if (edge.fromPassIndex == current && edgeIsActive(edge)) {
                --indegree[edge.toPassIndex];
            }
        }
    }

    if (m_compileOrder.size() != activePasses.size()) {
        m_compileInfo.usedDeclarationOrderFallback = true;
        m_compileOrder = activePasses;
    }
}

void RenderGraph::assignResourceLifetimes() {
    m_resourceLifetimes.clear();

    auto recordAccess = [&](u32 resourceId, bool isTexture, u32 passIndex) {
        const auto existing = std::find_if(m_resourceLifetimes.begin(), m_resourceLifetimes.end(),
                                           [&](const RGResourceLifetime& lifetime) {
                                               return lifetime.resourceId == resourceId &&
                                                      lifetime.isTexture == isTexture;
                                           });
        if (existing == m_resourceLifetimes.end()) {
            RGResourceLifetime lifetime;
            lifetime.resourceId = resourceId;
            lifetime.isTexture = isTexture;
            lifetime.firstPassIndex = passIndex;
            lifetime.lastPassIndex = passIndex;
            if (isTexture) {
                const TextureState& state = textureStateAt(resourceId);
                lifetime.phase = state.transient ? RGResourceLifetimePhase::TransientCreated
                                                 : RGResourceLifetimePhase::Imported;
            } else {
                lifetime.phase = RGResourceLifetimePhase::Imported;
            }
            m_resourceLifetimes.push_back(lifetime);
            return;
        }

        existing->firstPassIndex = std::min(existing->firstPassIndex, passIndex);
        existing->lastPassIndex = std::max(existing->lastPassIndex, passIndex);
    };

    for (const u32 passIndex : m_compileOrder) {
        const PassNode& pass = m_passes[passIndex];
        for (const RGTextureAccess& access : textureAccessesOf(pass)) {
            recordAccess(access.texture.id, true, passIndex);
        }
        for (const RGBufferAccess& access : bufferAccessesOf(pass)) {
            recordAccess(access.buffer.id, false, passIndex);
        }
    }
}

void RenderGraph::assignTransientAliasGroups() {
    auto rangesOverlap = [](const RGResourceLifetime& a, const RGResourceLifetime& b) {
        return a.firstPassIndex <= b.lastPassIndex && b.firstPassIndex <= a.lastPassIndex;
    };

    u32 nextGroupId = 1u;
    for (RGResourceLifetime& lifetime : m_resourceLifetimes) {
        lifetime.aliasGroup = 0;
        if (!lifetime.isTexture || lifetime.phase != RGResourceLifetimePhase::TransientCreated) {
            continue;
        }

        u32 reusedGroup = 0;
        for (u32 groupId = 1u; groupId < nextGroupId; ++groupId) {
            bool groupOverlaps = false;
            for (const RGResourceLifetime& member : m_resourceLifetimes) {
                if (member.aliasGroup != groupId) {
                    continue;
                }
                if (rangesOverlap(lifetime, member)) {
                    groupOverlaps = true;
                    break;
                }
            }
            if (!groupOverlaps) {
                reusedGroup = groupId;
                break;
            }
        }

        if (reusedGroup != 0u) {
            lifetime.aliasGroup = reusedGroup;
        } else {
            lifetime.aliasGroup = nextGroupId++;
        }
    }

    m_compileInfo.aliasGroups = nextGroupId > 1u ? nextGroupId - 1u : 0u;
}

void RenderGraph::assignExecutionOrder() {
    u32 order = 0;
    for (PassNode& pass : m_passes) {
        pass.order = static_cast<u32>(-1);
    }
    for (const u32 passIndex : m_compileOrder) {
        m_passes[passIndex].order = order++;
    }
    m_compileInfo.executablePassCount = order;
    m_compileInfo.culledPassCount = m_compileInfo.passCount - order;
}

void RenderGraph::compile() {
    const auto compileStart = std::chrono::steady_clock::now();

    m_barriers.clear();
    m_bufferBarriers.clear();
    m_compileInfo = {};
    m_compileInfo.passCount = static_cast<u32>(m_passes.size());

    cullUnusedPasses();
    buildDependencyEdges();
    resolveCompileOrder();
    assignResourceLifetimes();
    assignTransientAliasGroups();

    m_textureStates.clear();
    TextureState& backbuffer = textureStateAt(kBackbufferTextureId);
    backbuffer.imported = true;
    backbuffer.layout = RGImageLayout::Undefined;

    TextureState& depth = textureStateAt(kDepthTextureId);
    depth.imported = true;
    depth.layout = RGImageLayout::Undefined;

    for (BufferState& state : m_bufferStates) {
        state.lastAccess = RGResourceAccess::ShaderRead;
        state.written = false;
        state.touched = false;
    }

    for (const u32 passIndex : m_compileOrder) {
        planBarriersForPass(passIndex, m_passes[passIndex]);
    }

    assignExecutionOrder();
    m_compileInfo.barrierCount = static_cast<u32>(m_barriers.size());
    m_compileInfo.bufferBarrierCount = static_cast<u32>(m_bufferBarriers.size());
    m_compileInfo.dependencyEdgeCount = static_cast<u32>(m_dependencyEdges.size());
    m_compileInfo.resourceLifetimeCount = static_cast<u32>(m_resourceLifetimes.size());
    m_compileInfo.compiled = true;

    if (m_compileInfo.passCount == 0u) {
        m_compileInfo.compileDurationUs = 0;
        return;
    }

    const auto compileEnd = std::chrono::steady_clock::now();
    const auto elapsedUs =
        std::chrono::duration_cast<std::chrono::microseconds>(compileEnd - compileStart).count();
    m_compileInfo.compileDurationUs = elapsedUs > 0 ? static_cast<u32>(elapsedUs) : 0u;
}

RenderGraphExecuteInfo RenderGraph::execute(VulkanDevice& device,
                                            FrameManager& frames,
                                            CommandBufferRecorder& recorder,
                                            const VkFrameEncodeContext* encodeContext) {
    const auto executeStart = std::chrono::steady_clock::now();
    RenderGraphExecuteInfo result;
    if (!m_compileInfo.compiled) {
        result.executeDurationUs = 0;
        return result;
    }
    result.aliasGroups = m_compileInfo.aliasGroups;

    if (frames.isReady()) {
        const u32 scratchBytes = static_cast<u32>(m_compileOrder.size() * sizeof(u32));
        if (scratchBytes > 0u) {
            void* scratch = frames.allocateScratch(scratchBytes);
            if (scratch != nullptr) {
                std::memcpy(scratch, m_compileOrder.data(), scratchBytes);
            }
        }
        result.scratchBytesUsed = frames.scratchUsedBytes();
    }

    void* nativeCommandBuffer = frames.currentCommandBuffer();
    if (nativeCommandBuffer == nullptr) {
        nativeCommandBuffer = reinterpret_cast<void*>(0x1);
    }

    void* computeCommandBuffer = frames.isReady() ? frames.currentComputeCommandBuffer() : nullptr;
    const bool routeCompute =
        computeCommandBuffer != nullptr && computeCommandBuffer != reinterpret_cast<void*>(0x1);
    std::vector<u32> deferredComputePasses;

    recorder.setVulkanEncodeContext(encodeContext);
    recorder.beginRecording(nativeCommandBuffer);
    if (recorder.vulkanEncodeActive()) {
        frames.writeTimestampBegin(nativeCommandBuffer);
    }

    result.bufferBarrierCount = static_cast<u32>(m_bufferBarriers.size());

    // Barriers were planned in compile order, so each pass's batch is the next contiguous run.
    usize nextBarrier = 0;
    usize nextBufferBarrier = 0;
    auto recordBarriersForPass = [&](u32 passIndex) {
        while (nextBarrier < m_barriers.size() && m_barriers[nextBarrier].passIndex == passIndex) {
            const RGBarrier& barrier = m_barriers[nextBarrier++];
            recorder.pipelineBarrier(barrier.texture.id, static_cast<u32>(barrier.fromLayout),
                                     static_cast<u32>(barrier.toLayout), barrier.baseMip, barrier.levelCount,
                                     barrier.baseLayer, barrier.layerCount, barrier.srcQueueFamily,
                                     barrier.dstQueueFamily);
        }
        while (nextBufferBarrier < m_bufferBarriers.size() &&
               m_bufferBarriers[nextBufferBarrier].passIndex == passIndex) {
            const RGBufferBarrier& barrier = m_bufferBarriers[nextBufferBarrier++];
            recorder.bufferBarrier(barrier.buffer.id, static_cast<u32>(barrier.fromAccess),
                                   static_cast<u32>(barrier.toAccess), barrier.srcQueueFamily,
                                   barrier.dstQueueFamily);
        }
    };

    auto releaseTransientsAfterPass = [&](u32 passIndex) {
        for (RGResourceLifetime& lifetime : m_resourceLifetimes) {
            if (lifetime.isTexture &&
                lifetime.phase == RGResourceLifetimePhase::TransientCreated &&
                lifetime.lastPassIndex == passIndex) {
                lifetime.phase = RGResourceLifetimePhase::Released;
                ++result.transientsReleased;
            }
        }
    };

    for (const u32 passIndex : m_compileOrder) {
        const PassNode& pass = m_passes[passIndex];
        recordBarriersForPass(passIndex);

        if (pass.desc.isCuda) {
            if (pass.desc.execute != nullptr) {
                pass.desc.execute(&recorder, pass.desc.userData);
            }
            ++result.cudaPassCount;
            ++result.executedPassCount;
            releaseTransientsAfterPass(passIndex);
            continue;
        }

        if (pass.desc.isCompute) {
            if (routeCompute) {
                deferredComputePasses.push_back(passIndex);
            } else if (pass.desc.execute != nullptr) {
                pass.desc.execute(&recorder, pass.desc.userData);
            }
            ++result.computePassCount;
            ++result.executedPassCount;
            releaseTransientsAfterPass(passIndex);
            continue;
        }

        recorder.beginPass(pass.desc.name != nullptr ? pass.desc.name : "pass");
        if (pass.desc.execute != nullptr) {
            pass.desc.execute(&recorder, pass.desc.userData);
        }
        recorder.endPass();
        ++result.executedPassCount;
        releaseTransientsAfterPass(passIndex);
    }

    if (recorder.vulkanEncodeActive()) {
        frames.writeTimestampEnd(nativeCommandBuffer);
    }
    recorder.endRecording();
    result.recordedCommands = recorder.recordCount();

    if (routeCompute && !deferredComputePasses.empty()) {
        CommandBufferRecorder computeRecorder;
        computeRecorder.setVulkanEncodeContext(encodeContext);
        const bool began = computeRecorder.beginRecording(computeCommandBuffer);
        for (const u32 passIndex : deferredComputePasses) {
            const PassNode& pass = m_passes[passIndex];
            if (pass.desc.execute != nullptr) {
                pass.desc.execute(&computeRecorder, pass.desc.userData);
            }
        }
        if (began) {
            computeRecorder.endRecording();
        }
        result.computeQueueRecorded = computeRecorder.vulkanRecordingComplete();
        result.recordedCommands += computeRecorder.recordCount();
    }
    (void)device;

    const auto executeEnd = std::chrono::steady_clock::now();
    const auto elapsedUs =
        std::chrono::duration_cast<std::chrono::microseconds>(executeEnd - executeStart).count();
    result.executeDurationUs = elapsedUs > 0 ? static_cast<u32>(elapsedUs) : 0u;
    return result;
}

namespace {

ClearPassUserData g_clearPasses[RenderGraph::kMaxPassesPerFrame]{};
DrawPassUserData g_drawPasses[RenderGraph::kMaxPassesPerFrame]{};
u32 g_clearPassCount = 0;
u32 g_drawPassCount = 0;

/// Thread-unsafe; DrawList must outlive compile() + execute() (same pattern as g_clearPasses).
const DrawList* g_drawListForGraph = nullptr;

RGTextureAccess g_backbufferColorWrite{};
RGTextureAccess g_backbufferPresent{};
RGTextureAccess g_meshPassAccesses[2]{};

} // namespace

void populateRenderGraphFromCommandList(RenderGraph& graph,
                                        const RenderCommandList& commands,
                                        float compositeBlend) {
    g_clearPassCount = 0;
    g_drawPassCount = 0;
    resetCompositePassGraphStorage();

    g_backbufferColorWrite.texture = {RenderGraph::kBackbufferTextureId};
    g_backbufferColorWrite.access = RGResourceAccess::ColorAttachmentWrite;
    g_backbufferPresent.texture = {RenderGraph::kBackbufferTextureId};
    g_backbufferPresent.access = RGResourceAccess::Present;

    bool wroteColor = false;
    u32 spriteDrawCount = 0;

    for (const RenderCommand& command : commands.commands()) {
        switch (command.kind) {
        case RenderCommandKind::Clear3D:
            if (g_clearPassCount < RenderGraph::kMaxPassesPerFrame) {
                ClearPassUserData& clear = g_clearPasses[g_clearPassCount++];
                clear.r = command.clear3D.r;
                clear.g = command.clear3D.g;
                clear.b = command.clear3D.b;

                RGPassDesc pass{};
                pass.name = "clear3d";
                pass.execute = executeClearPass;
                pass.userData = &clear;
                pass.textureAccesses = &g_backbufferColorWrite;
                pass.textureAccessCount = 1;
                graph.addPass(pass);
                wroteColor = true;
            }
            break;
        case RenderCommandKind::DrawSprite2D:
            ++spriteDrawCount;
            break;
        default:
            break;
        }
    }

    if (spriteDrawCount > 0u && g_drawPassCount < RenderGraph::kMaxPassesPerFrame) {
        DrawPassUserData& draw = g_drawPasses[g_drawPassCount++];
        draw.drawCount = spriteDrawCount;

        RGTextureAccess spriteRead{};
        spriteRead.texture = {RenderGraph::kBackbufferTextureId};
        spriteRead.access = RGResourceAccess::ShaderRead;

        RGPassDesc pass{};
        pass.name = "sprites2d";
        pass.execute = executeDrawPass;
        pass.userData = &draw;
        pass.textureAccesses = &spriteRead;
        pass.textureAccessCount = 1;
        graph.addPass(pass);
    }

    if (wroteColor || spriteDrawCount > 0u) {
        addCompositePassToGraph(graph, compositeBlend);

        RGPassDesc present{};
        present.name = "present";
        present.execute = executePresentPass;
        present.textureAccesses = &g_backbufferPresent;
        present.textureAccessCount = 1;
        graph.addPass(present);
    }
}

void populateRenderGraphFromDrawList(RenderGraph& graph, const DrawList& draws) {
    g_drawListForGraph = nullptr;

    if (draws.count() == 0u) {
        return;
    }

    g_drawListForGraph = &draws;

    g_meshPassAccesses[0].texture = {RenderGraph::kBackbufferTextureId};
    g_meshPassAccesses[0].access = RGResourceAccess::ColorAttachmentWrite;
    g_meshPassAccesses[1].texture = {RenderGraph::kDepthTextureId};
    g_meshPassAccesses[1].access = RGResourceAccess::DepthAttachmentWrite;
    g_backbufferPresent.texture = {RenderGraph::kBackbufferTextureId};
    g_backbufferPresent.access = RGResourceAccess::Present;

    RGPassDesc meshes{};
    meshes.name = "meshes";
    meshes.execute = executeDrawListPass;
    meshes.userData = const_cast<DrawList*>(g_drawListForGraph);
    meshes.textureAccesses = g_meshPassAccesses;
    meshes.textureAccessCount = 2;
    graph.addPass(meshes);

    RGPassDesc present{};
    present.name = "present";
    present.execute = executePresentPass;
    present.textureAccesses = &g_backbufferPresent;
    present.textureAccessCount = 1;
    graph.addPass(present);
}

} // namespace fuse::renderer
