#include <fuse/renderer/render_graph.hpp>

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

} // namespace

RenderGraph::TextureState& RenderGraph::textureStateAt(u32 textureId) {
    if (textureId >= m_textureStates.size()) {
        m_textureStates.resize(textureId + 1u);
    }
    return m_textureStates[textureId];
}

void RenderGraph::reset() {
    m_backbufferIndex = 0;
    m_nextTextureId = kBackbufferTextureId + 1u;
    m_nextBufferId = 1u;
    m_compileInfo = {};
    m_passes.clear();
    m_barriers.clear();
    m_textureStates.clear();

    TextureState& backbuffer = textureStateAt(kBackbufferTextureId);
    backbuffer.imported = true;
    backbuffer.layout = RGImageLayout::Undefined;
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

RGBufferRef RenderGraph::importBuffer(BufferHandle /*handle*/) {
    RGBufferRef ref{m_nextBufferId++};
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
    if (m_passes.size() >= kMaxPassesPerFrame) {
        return;
    }

    PassNode node;
    node.desc = desc;
    if (desc.textureAccesses != nullptr && desc.textureAccessCount > 0u) {
        node.textureAccesses.assign(desc.textureAccesses,
                                    desc.textureAccesses + desc.textureAccessCount);
    }
    if (desc.bufferAccesses != nullptr && desc.bufferAccessCount > 0u) {
        node.bufferAccesses.assign(desc.bufferAccesses,
                                   desc.bufferAccesses + desc.bufferAccessCount);
    }
    m_passes.push_back(std::move(node));
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

void RenderGraph::planBarriersForPass(const PassNode& pass) {
    for (const RGTextureAccess& access : pass.textureAccesses) {
        TextureState& state = textureStateAt(access.texture.id);
        const RGImageLayout requiredLayout = layoutForAccess(access.access);

        if (state.layout != RGImageLayout::Undefined && state.layout != requiredLayout) {
            RGBarrier barrier;
            barrier.texture = access.texture;
            barrier.fromLayout = state.layout;
            barrier.toLayout = requiredLayout;
            barrier.fromAccess = state.lastAccess;
            barrier.toAccess = access.access;
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
}

void RenderGraph::cullUnusedPasses() {
    if (m_passes.empty()) {
        return;
    }

    std::vector<bool> required(m_passes.size(), false);
    for (u32 i = 0; i < m_passes.size(); ++i) {
        for (const RGTextureAccess& access : m_passes[i].textureAccesses) {
            if (access.access == RGResourceAccess::Present) {
                required[i] = true;
            }
        }
        if (m_passes[i].desc.isCuda) {
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
                for (const RGTextureAccess& producerAccess : m_passes[producer].textureAccesses) {
                    for (const RGTextureAccess& consumerAccess : m_passes[i].textureAccesses) {
                        if (producerAccess.texture.id == consumerAccess.texture.id &&
                            producerAccess.access != RGResourceAccess::Present) {
                            required[producer] = true;
                            changed = true;
                        }
                    }
                }
            }
        }
    }

    for (u32 i = 0; i < m_passes.size(); ++i) {
        m_passes[i].culled = !required[i];
    }
}

void RenderGraph::assignExecutionOrder() {
    u32 order = 0;
    for (PassNode& pass : m_passes) {
        if (!pass.culled) {
            pass.order = order++;
        }
    }
    m_compileInfo.executablePassCount = order;
    m_compileInfo.culledPassCount = m_compileInfo.passCount - order;
}

void RenderGraph::compile() {
    m_barriers.clear();
    m_compileInfo = {};
    m_compileInfo.passCount = static_cast<u32>(m_passes.size());

    cullUnusedPasses();

    m_textureStates.clear();
    TextureState& backbuffer = textureStateAt(kBackbufferTextureId);
    backbuffer.imported = true;
    backbuffer.layout = RGImageLayout::Undefined;

    for (PassNode& pass : m_passes) {
        if (pass.culled) {
            continue;
        }
        planBarriersForPass(pass);
    }

    assignExecutionOrder();
    m_compileInfo.barrierCount = static_cast<u32>(m_barriers.size());
    m_compileInfo.compiled = true;
}

RenderGraphExecuteInfo RenderGraph::execute(VulkanDevice& device,
                                            FrameManager& frames,
                                            CommandBufferRecorder& recorder) {
    RenderGraphExecuteInfo result;
    if (!m_compileInfo.compiled) {
        return result;
    }

    void* nativeCommandBuffer = frames.currentCommandBuffer();
    if (nativeCommandBuffer == nullptr) {
        nativeCommandBuffer = reinterpret_cast<void*>(0x1);
    }

    recorder.beginRecording(nativeCommandBuffer);

    for (const RGBarrier& barrier : m_barriers) {
        recorder.pipelineBarrier(barrier.texture.id,
                                 static_cast<u32>(barrier.fromLayout),
                                 static_cast<u32>(barrier.toLayout));
    }

    for (const PassNode& pass : m_passes) {
        if (pass.culled) {
            continue;
        }

        recorder.beginPass(pass.desc.name != nullptr ? pass.desc.name : "pass");
        if (pass.desc.isCuda) {
            ++result.executedPassCount;
            recorder.endPass();
            continue;
        }

        if (pass.desc.execute != nullptr) {
            pass.desc.execute(&recorder, pass.desc.userData);
        }
        recorder.endPass();
        ++result.executedPassCount;
    }

    recorder.endRecording();
    result.recordedCommands = recorder.recordCount();
    (void)device;
    return result;
}

namespace {

ClearPassUserData g_clearPasses[RenderGraph::kMaxPassesPerFrame]{};
DrawPassUserData g_drawPasses[RenderGraph::kMaxPassesPerFrame]{};
u32 g_clearPassCount = 0;
u32 g_drawPassCount = 0;

RGTextureAccess g_backbufferColorWrite{};
RGTextureAccess g_backbufferPresent{};

} // namespace

void populateRenderGraphFromCommandList(RenderGraph& graph, const RenderCommandList& commands) {
    g_clearPassCount = 0;
    g_drawPassCount = 0;

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
        RGPassDesc present{};
        present.name = "present";
        present.execute = executePresentPass;
        present.textureAccesses = &g_backbufferPresent;
        present.textureAccessCount = 1;
        graph.addPass(present);
    }
}

} // namespace fuse::renderer
