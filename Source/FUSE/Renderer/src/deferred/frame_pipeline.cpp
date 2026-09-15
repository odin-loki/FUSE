#include <fuse/renderer/deferred/frame_pipeline.hpp>

#include <fuse/renderer/command_buffer.hpp>
#include <fuse/renderer/lighting/clustered.hpp>
#include <fuse/renderer/postprocess/lens_flare.hpp>
#include <fuse/renderer/volumetric/light_shafts.hpp>
#include <fuse/renderer/volumetric/volumetric_fog.hpp>

namespace fuse::renderer {
namespace {

struct DeferredPassUserData {
    DeferredPassId id = DeferredPassId::DepthPrepass;
    ClusteredLightCuller* culler = nullptr;
    ClusterCameraDesc camera{};
};

DeferredPassUserData g_passUserData[RenderGraph::kMaxPassesPerFrame]{};
RGTextureAccess g_passTextureAccesses[RenderGraph::kMaxPassesPerFrame * 8]{};
u32 g_passTextureAccessCounts[RenderGraph::kMaxPassesPerFrame]{};
u32 g_deferredPassCount = 0;

void executeDeferredPass(void* commandBuffer, void* userData) {
    auto* recorder = static_cast<CommandBufferRecorder*>(commandBuffer);
    const auto* pass = static_cast<const DeferredPassUserData*>(userData);
    if (recorder == nullptr || pass == nullptr) {
        return;
    }

    if (pass->id == DeferredPassId::ClusteredLightCull && pass->culler != nullptr &&
        pass->culler->isReady()) {
        pass->culler->recordCullPass(*recorder, pass->camera, {}, {});
        return;
    }

    recorder->beginPass(DeferredFramePipeline::passName(pass->id));
    recorder->endPass();
}

RGTextureAccess makeGBufferAccess(RGTextureRef texture, RGResourceAccess access) {
    RGTextureAccess result{};
    result.texture = texture;
    result.access = access;
    return result;
}

void addDeferredPass(RenderGraph& graph,
                     DeferredPassId id,
                     bool isCuda,
                     const RGTextureAccess* accesses,
                     u32 accessCount,
                     ClusteredLightCuller* culler = nullptr,
                     const ClusterCameraDesc* camera = nullptr) {
    if (g_deferredPassCount >= RenderGraph::kMaxPassesPerFrame) {
        return;
    }

    const u32 passIndex = g_deferredPassCount++;
    DeferredPassUserData& userData = g_passUserData[passIndex];
    userData.id = id;
    userData.culler = culler;
    userData.camera = camera != nullptr ? *camera : ClusterCameraDesc{};

    if (accesses != nullptr && accessCount > 0u) {
        const u32 base = passIndex * 8u;
        for (u32 i = 0; i < accessCount; ++i) {
            g_passTextureAccesses[base + i] = accesses[i];
        }
        g_passTextureAccessCounts[passIndex] = accessCount;
    } else {
        g_passTextureAccessCounts[passIndex] = 0u;
    }

    RGPassDesc pass{};
    pass.name = DeferredFramePipeline::passName(id);
    pass.execute = executeDeferredPass;
    pass.userData = &userData;
    pass.textureAccesses = g_passTextureAccessCounts[passIndex] > 0u
                               ? &g_passTextureAccesses[passIndex * 8u]
                               : nullptr;
    pass.textureAccessCount = g_passTextureAccessCounts[passIndex];
    pass.isCuda = isCuda;
    graph.addPass(pass);
}

} // namespace

DeferredFramePipeline::DeferredFramePipeline(const DeferredFramePipelineDesc& desc) : m_desc(desc) {
    m_stats.ready = true;
    m_stats.passCount = passCount();
}

void DeferredFramePipeline::resetGraphStorage() {
    g_deferredPassCount = 0;
    resetVolumetricFogPassGraphStorage();
    resetLightShaftsPassGraphStorage();
    resetLensFlarePassGraphStorage();
}

const char* DeferredFramePipeline::passName(DeferredPassId id) {
    switch (id) {
    case DeferredPassId::DepthPrepass:
        return "depth_prepass";
    case DeferredPassId::GBuffer:
        return "gbuffer";
    case DeferredPassId::ShadowMaps:
        return "shadow_maps";
    case DeferredPassId::VkToCudaSignal:
        return "vk_to_cuda_signal";
    case DeferredPassId::SdfRayMarch:
        return "sdf_ray_march";
    case DeferredPassId::DdgiProbeUpdate:
        return "ddgi_probe_update";
    case DeferredPassId::ClusteredLightCull:
        return "clustered_light_cull";
    case DeferredPassId::DeferredShading:
        return "deferred_shading";
    case DeferredPassId::SdfShadows:
        return "sdf_shadows";
    case DeferredPassId::ScreenSpaceAo:
        return "screen_space_ao";
    case DeferredPassId::CudaToVkSignal:
        return "cuda_to_vk_signal";
    case DeferredPassId::TransparentPass:
        return "transparent_pass";
    case DeferredPassId::AtmosphereSky:
        return "atmosphere_sky";
    case DeferredPassId::TaaResolve:
        return "taa_resolve";
    case DeferredPassId::PostProcessStack:
        return "post_process_stack";
    case DeferredPassId::VolumetricFog:
        return "volumetric_fog";
    case DeferredPassId::LightShafts:
        return "light_shafts";
    case DeferredPassId::LensFlare:
        return "lens_flare";
    case DeferredPassId::UiCompositePresent:
        return "ui_composite_present";
    default:
        return "deferred_unknown";
    }
}

void DeferredFramePipeline::buildGraph(RenderGraph& graph,
                                       const GBuffer& gbuffer,
                                       ClusteredLightCuller* culler) {
    resetGraphStorage();

    RGTextureRef depthTexture{};
    RGTextureRef normalTexture{};
    RGTextureRef albedoTexture{};
    RGTextureRef materialTexture{};
    RGTextureRef velocityTexture{};
    RGTextureRef emissiveTexture{};

    if (gbuffer.isReady()) {
        depthTexture = graph.importTexture(gbuffer.targets().attachments[static_cast<usize>(
                                               GBufferAttachment::Depth)],
                                           RGImageLayout::Undefined);
        normalTexture = graph.importTexture(gbuffer.targets().attachments[static_cast<usize>(
                                                GBufferAttachment::NormalAo)],
                                            RGImageLayout::Undefined);
        albedoTexture = graph.importTexture(gbuffer.targets().attachments[static_cast<usize>(
                                                GBufferAttachment::AlbedoAlpha)],
                                            RGImageLayout::Undefined);
        materialTexture = graph.importTexture(
            gbuffer.targets().attachments[static_cast<usize>(GBufferAttachment::RoughMetalEmissiveShading)],
            RGImageLayout::Undefined);
        velocityTexture = graph.importTexture(gbuffer.targets().attachments[static_cast<usize>(
                                                  GBufferAttachment::Velocity)],
                                              RGImageLayout::Undefined);
        emissiveTexture = graph.importTexture(gbuffer.targets().attachments[static_cast<usize>(
                                                  GBufferAttachment::Emissive)],
                                              RGImageLayout::Undefined);
    }

    RGTextureAccess depthWrite = makeGBufferAccess(depthTexture, RGResourceAccess::DepthAttachmentWrite);
    addDeferredPass(graph, DeferredPassId::DepthPrepass, false, &depthWrite, 1u);

    RGTextureAccess gbufferWrites[5] = {
        makeGBufferAccess(normalTexture, RGResourceAccess::ColorAttachmentWrite),
        makeGBufferAccess(albedoTexture, RGResourceAccess::ColorAttachmentWrite),
        makeGBufferAccess(materialTexture, RGResourceAccess::ColorAttachmentWrite),
        makeGBufferAccess(velocityTexture, RGResourceAccess::ColorAttachmentWrite),
        makeGBufferAccess(depthTexture, RGResourceAccess::DepthAttachmentWrite),
    };
    addDeferredPass(graph, DeferredPassId::GBuffer, false, gbufferWrites, 5u);

    const RGTextureRef shadowAtlas = graph.createTransient({});
    RGTextureAccess shadowWrite = makeGBufferAccess(shadowAtlas, RGResourceAccess::DepthAttachmentWrite);
    addDeferredPass(graph, DeferredPassId::ShadowMaps, false, &shadowWrite, 1u);

    addDeferredPass(graph, DeferredPassId::VkToCudaSignal, false, nullptr, 0u);

    RGTextureAccess cudaGbufferReads[4] = {
        makeGBufferAccess(normalTexture, RGResourceAccess::CUDARead),
        makeGBufferAccess(albedoTexture, RGResourceAccess::CUDARead),
        makeGBufferAccess(materialTexture, RGResourceAccess::CUDARead),
        makeGBufferAccess(depthTexture, RGResourceAccess::CUDARead),
    };
    addDeferredPass(graph, DeferredPassId::SdfRayMarch, true, cudaGbufferReads, 4u);
    addDeferredPass(graph, DeferredPassId::DdgiProbeUpdate, true, nullptr, 0u);

    ClusterCameraDesc clusterCamera{};
    clusterCamera.screenWidth = m_desc.width;
    clusterCamera.screenHeight = m_desc.height;
    addDeferredPass(graph,
                    DeferredPassId::ClusteredLightCull,
                    true,
                    nullptr,
                    0u,
                    culler != nullptr && culler->isReady() ? culler : nullptr,
                    &clusterCamera);

    RGTextureAccess deferredWrite = makeGBufferAccess({RenderGraph::kBackbufferTextureId},
                                                      RGResourceAccess::CUDAWrite);
    addDeferredPass(graph, DeferredPassId::DeferredShading, true, &deferredWrite, 1u);
    addDeferredPass(graph, DeferredPassId::SdfShadows, true, nullptr, 0u);
    addDeferredPass(graph, DeferredPassId::ScreenSpaceAo, true, nullptr, 0u);

    RGTextureAccess volumetricDepthRead = makeGBufferAccess(depthTexture, RGResourceAccess::CUDARead);
    addVolumetricFogPassToGraph(graph, &volumetricDepthRead, 1u);

    addDeferredPass(graph, DeferredPassId::CudaToVkSignal, false, nullptr, 0u);

    RGTextureAccess transparentWrite = makeGBufferAccess({RenderGraph::kBackbufferTextureId},
                                                         RGResourceAccess::ColorAttachmentWrite);
    addDeferredPass(graph, DeferredPassId::TransparentPass, false, &transparentWrite, 1u);
    addDeferredPass(graph, DeferredPassId::AtmosphereSky, false, &transparentWrite, 1u);
    addLightShaftsPassToGraph(graph, transparentWrite.texture);
    addDeferredPass(graph, DeferredPassId::TaaResolve, false, &transparentWrite, 1u);
    addDeferredPass(graph, DeferredPassId::PostProcessStack, false, &transparentWrite, 1u);
    addLensFlarePassToGraph(graph, transparentWrite.texture);

    RGTextureAccess presentAccess = makeGBufferAccess({RenderGraph::kBackbufferTextureId},
                                                        RGResourceAccess::Present);
    addDeferredPass(graph, DeferredPassId::UiCompositePresent, false, &presentAccess, 1u);

    if (emissiveTexture.id != 0u) {
        RGTextureAccess emissiveWrite = makeGBufferAccess(emissiveTexture, RGResourceAccess::ColorAttachmentWrite);
        (void)emissiveWrite;
    }

    m_stats.passCount = g_deferredPassCount + 3u;
    m_stats.vulkanPassCount = 0u;
    m_stats.cudaPassCount = 0u;
    for (u32 i = 0; i < g_deferredPassCount; ++i) {
        if (g_passUserData[i].id >= DeferredPassId::SdfRayMarch &&
            g_passUserData[i].id <= DeferredPassId::ScreenSpaceAo) {
            ++m_stats.cudaPassCount;
        } else {
            ++m_stats.vulkanPassCount;
        }
    }
    ++m_stats.cudaPassCount;
    m_stats.vulkanPassCount += 2u;
}

} // namespace fuse::renderer
