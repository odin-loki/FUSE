#include <fuse/core/init.hpp>
#include <fuse/renderer/atmosphere/sky_pass.hpp>
#include <fuse/renderer/command_buffer.hpp>
#include <fuse/renderer/deferred/deferred_renderer.hpp>
#include <fuse/renderer/deferred/frame_pipeline.hpp>
#include <fuse/renderer/gi/ddgi.hpp>
#include <fuse/renderer/material/material.hpp>
#include <fuse/renderer/render_graph.hpp>
#include <fuse/renderer/resource_manager.hpp>
#include <fuse/renderer/shadow/directional_shadow.hpp>
#include <fuse/renderer/shadow/shadow_pass.hpp>
#include <fuse/renderer/taa/taa_pass.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/renderer/vk/bootstrap.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_set>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

bool recorderContainsPass(const fuse::renderer::CommandBufferRecorder& recorder, const char* passName) {
    for (const fuse::renderer::CommandRecord& record : recorder.records()) {
        if (record.kind == fuse::renderer::CommandRecordKind::BeginPass && record.passName != nullptr &&
            std::string(record.passName) == passName) {
            return true;
        }
    }
    return false;
}

void expectKeyDeferredPassesRecorded(const fuse::renderer::CommandBufferRecorder& recorder) {
    // Passes with no texture consumers (shadow_maps, vk/cuda signals) are culled by the render
    // graph — integration asserts the connected spine, not every registered slot.
    const char* keyPasses[] = {
        "depth_prepass",
        "gbuffer",
        "sdf_ray_march",
        "ddgi_probe_update",
        "clustered_light_cull",
        "deferred_shading",
        "sdf_shadows",
        "screen_space_ao",
        "transparent_pass",
        "atmosphere_sky",
        "taa_resolve",
        "post_process_stack",
        "volumetric_fog",
        "light_shafts",
        "lens_flare",
        "ui_composite_present",
    };
    for (const char* name : keyPasses) {
        expectTrue(recorderContainsPass(recorder, name), name);
    }
}

void testPhase5DeferredPipelineIntegration() {
    fuse::core::initialize();

    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "Vulkan bootstrap allocated for Phase 5 integration");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    expectTrue(resources.init(*bootstrap->device(), bindless), "resource manager ready");

    fuse::renderer::DeferredRendererDesc rendererDesc{};
    rendererDesc.pipeline.width = 128;
    rendererDesc.pipeline.height = 128;

    auto renderer = fuse::renderer::DeferredRenderer::create(rendererDesc);
    expectTrue(renderer->init(resources), "DeferredRenderer initialized with G-buffer and cluster culler");
    expectTrue(renderer->isReady(), "DeferredRenderer ready");
    expectTrue(renderer->gbuffer().isReady(), "G-buffer allocated");
    expectTrue(renderer->materials().isReady(), "material system ready");
    expectTrue(renderer->lightCuller().isReady(), "clustered light culler ready");

    fuse::renderer::Material dielectric{};
    dielectric.baseColor = {0.8f, 0.8f, 0.8f};
    dielectric.metallic = 0.f;
    dielectric.roughness = 0.5f;

    fuse::renderer::Material metallic{};
    metallic.baseColor = {0.9f, 0.9f, 0.9f};
    metallic.metallic = 1.f;
    metallic.roughness = 0.2f;

    const fuse::u32 dielectricId = renderer->materials().registerMaterial(dielectric);
    const fuse::u32 metallicId = renderer->materials().registerMaterial(metallic);
    renderer->materials().flushGpuBuffer();
    expectTrue(dielectricId == 0u && metallicId == 1u, "materials registered");
    expectTrue(renderer->materials().materialCount() == 2u, "material table populated");
    expectTrue(renderer->materials().materialSsbo().isValid(), "material SSBO allocated");

    fuse::renderer::DirectionalShadow shadows;
    expectTrue(shadows.init(resources, {}), "directional shadow system initialized");
    expectTrue(shadows.isReady(), "directional shadow ready");

    auto shadowPass = fuse::renderer::ShadowPass::create({});
    expectTrue(shadowPass->isReady(), "shadow pass scaffold ready");

    fuse::renderer::DDGI ddgi;
    expectTrue(ddgi.init({}, resources), "DDGI probe volume initialized");
    expectTrue(ddgi.isReady(), "DDGI ready");

    fuse::renderer::TaaPassDesc taaDesc{};
    taaDesc.width = 128;
    taaDesc.height = 128;
    auto taaPass = fuse::renderer::TaaPass::create(taaDesc);
    expectTrue(taaPass->init(resources), "TAA pass initialized");
    expectTrue(taaPass->isReady(), "TAA pass ready");

    fuse::renderer::SkyPassDesc skyDesc{};
    skyDesc.sun_direction = {0.f, 0.4f, 1.f};
    auto skyPass = fuse::renderer::SkyPass::create(skyDesc);
    expectTrue(skyPass != nullptr && skyPass->isReady(), "sky pass ready");

    auto frames = fuse::renderer::FrameManager::create(*bootstrap->device());
    fuse::renderer::CommandBufferRecorder recorder;

    fuse::renderer::ShadowCameraParams shadowCamera{};
    shadowCamera.position = {0.f, 10.f, 20.f};
    shadowCamera.forward = {0.f, -0.2f, -1.f};
    shadowCamera.farPlane = 500.f;

    fuse::renderer::ClusterCameraDesc clusterCamera{};
    clusterCamera.screenWidth = rendererDesc.pipeline.width;
    clusterCamera.screenHeight = rendererDesc.pipeline.height;
    clusterCamera.nearPlane = 0.5f;
    clusterCamera.farPlane = 100.f;

    fuse::renderer::PointLightInput keyLight{};
    keyLight.position = {0.f, 2.f, -5.f};
    keyLight.radius = 8.f;

    fuse::renderer::RenderGraph scheduleProbe;
    expectTrue(renderer->buildFrameGraph(scheduleProbe, 0u), "deferred frame graph compiles for schedule probe");
    expectTrue(scheduleProbe.compileInfo().passCount == fuse::renderer::DeferredFramePipeline::passCount(),
               "all deferred passes registered");
    expectTrue(scheduleProbe.compileInfo().executablePassCount > 0u, "executable passes present after cull");
    const fuse::u32 expectedExecutablePasses = scheduleProbe.compileInfo().executablePassCount;

    constexpr fuse::u32 kFramesToExercise = 3u;
    for (fuse::u32 frameIndex = 0; frameIndex < kFramesToExercise; ++frameIndex) {
        shadows.update(shadowCamera, {-0.3f, -1.f, -0.2f});

        fuse::renderer::RenderGraph shadowGraph;
        shadowGraph.beginFrame(frameIndex);
        expectTrue(shadowPass->recordFrame(shadows, shadowGraph), "shadow pass records into graph");
        shadowGraph.compile();
        expectTrue(shadowGraph.compileInfo().compiled, "shadow graph compiles");

        expectTrue(ddgi.update(frameIndex), "DDGI probe update completes");
        expectTrue(ddgi.lastUpdateStats().probes_scheduled == 64u, "DDGI schedules 64 probes per frame");

        renderer->lightCuller().updateClusters(clusterCamera);
        renderer->lightCuller().cullLights({keyLight}, {}, clusterCamera);
        expectTrue(renderer->lightCuller().stats().clustersBuilt > 0u, "cluster grid built");

        taaPass->advanceJitter();
        expectTrue(skyPass->recordFrame(), "sky pass records frame");

        const fuse::renderer::RenderGraphExecuteInfo info =
            renderer->executeFrame(*bootstrap->device(), *frames, recorder, frameIndex);

        expectTrue(info.executedPassCount == expectedExecutablePasses,
                   "all non-culled deferred passes executed");
        expectTrue(recorder.recordCount() > 0u, "command recorder captured deferred work");
        expectTrue(renderer->stats().framesBuilt == frameIndex + 2u,
                   "frame graph build count advances (includes schedule probe)");
        expectTrue(renderer->stats().lastExecutedPassCount == expectedExecutablePasses,
                   "deferred renderer stats match executable pass count");
        expectTrue(renderer->pipeline().lastStats().passCount == 19u, "pipeline reports 19 passes");
        expectTrue(renderer->pipeline().lastStats().vulkanPassCount == 12u, "twelve Vulkan passes scheduled");
        expectTrue(renderer->pipeline().lastStats().cudaPassCount == 7u, "seven CUDA passes scheduled");
        expectTrue(renderer->stats().lastBarrierCount > 0u, "render graph planned barriers");

        expectKeyDeferredPassesRecorded(recorder);
    }

    expectTrue(shadows.stats().framesUpdated == kFramesToExercise, "shadow updates per frame");
    expectTrue(shadowPass->lastStats().framesRecorded == kFramesToExercise, "shadow pass stats advance");
    expectTrue(skyPass->lastStats().framesRecorded == kFramesToExercise, "sky pass stats advance");
    expectTrue(taaPass->lastStats().framesResolved == 0u, "TAA resolve deferred until surfaces wired");

    taaPass->destroy();
    ddgi.destroy();
    shadows.destroy();
    renderer->destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());

    fuse::core::shutdown();
}

} // namespace

int main() {
    testPhase5DeferredPipelineIntegration();

    if (g_failures == 0) {
        std::printf("fuse_phase5_deferred_integration: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_phase5_deferred_integration: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
