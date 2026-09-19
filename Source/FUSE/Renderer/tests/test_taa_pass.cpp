#include <fuse/core/init.hpp>
#include <fuse/renderer/render_graph.hpp>
#include <fuse/renderer/resource_manager.hpp>
#include <fuse/renderer/taa/taa_history.hpp>
#include <fuse/renderer/taa/taa_jitter.hpp>
#include <fuse/renderer/taa/taa_pass.hpp>
#include <fuse/renderer/taa/taa_resolve.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/renderer/vk/bootstrap.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace {

int g_failures = 0;

std::unique_ptr<fuse::renderer::VulkanBootstrap> createTaaTestBootstrap() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    if (bootstrap == nullptr || bootstrap->device() == nullptr || !bootstrap->device()->isValid()) {
        return nullptr;
    }
    return bootstrap;
}

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void expectNear(float value, float expected, float epsilon, const char* message) {
    if (std::fabs(value - expected) > epsilon) {
        std::fprintf(stderr, "FAIL: %s (got %f, expected %f)\n", message, value, expected);
        ++g_failures;
    }
}

bool taaVulkanBootstrapReady(const std::unique_ptr<fuse::renderer::VulkanBootstrap>& bootstrap) {
    return bootstrap != nullptr && bootstrap->device() != nullptr && bootstrap->device()->isValid();
}

#define TAA_SKIP_UNLESS_VULKAN_READY(bootstrap) \
    do { \
        if (!taaVulkanBootstrapReady(bootstrap)) { \
            std::fprintf(stderr, "SKIP: %s — Vulkan device unavailable\n", __func__); \
            return; \
        } \
    } while (0)

void testHaltonJitterSequence() {
    fuse::renderer::TaaJitter jitter;
    expectTrue(jitter.sequenceLength() == 8u, "default jitter sequence length is 8");

    const fuse::math::Vec2 first = jitter.currentPixelOffset();
    expectNear(first.x, 0.5f, 1e-5f, "first Halton X sample");
    expectNear(first.y, 0.333f, 1e-3f, "first Halton Y sample");

    jitter.advance();
    const fuse::math::Vec2 second = jitter.currentPixelOffset();
    expectNear(second.x, 0.25f, 1e-5f, "second Halton X sample");

    jitter.reset();
    const fuse::math::Vec2 reset = jitter.currentPixelOffset();
    expectNear(reset.x, first.x, 1e-5f, "jitter reset returns to first sample");
}

void testJitterSequenceLayout() {
    using fuse::renderer::TaaJitterLayout;

    expectTrue(TaaJitterLayout::validateSequenceLength(8u), "default sequence length valid");
    expectTrue(!TaaJitterLayout::validateSequenceLength(0u), "zero sequence length rejected");
    expectTrue(!TaaJitterLayout::validateSequenceLength(fuse::renderer::kTaaMaxJitterSequenceLength + 1u),
               "oversized sequence length rejected");

    const fuse::math::Vec2 computed = TaaJitterLayout::haltonPixelOffset(0u, 8u);
    expectNear(computed.x, 0.5f, 1e-5f, "layout Halton X matches default table");
    expectNear(computed.y, 0.333f, 1e-3f, "layout Halton Y matches default table");

    fuse::math::Vec2 sequence[8]{};
    expectTrue(TaaJitterLayout::fillHaltonSequence(8u, sequence), "fillHaltonSequence succeeds for valid length");
    expectNear(sequence[0].x, 0.5f, 1e-5f, "filled sequence slot 0 X");
    expectNear(sequence[7].x, 0.0625f, 1e-5f, "filled sequence slot 7 X");
    expectNear(sequence[7].y, 0.889f, 1e-3f, "filled sequence slot 7 Y");

    expectTrue(!TaaJitterLayout::fillHaltonSequence(0u, sequence), "fillHaltonSequence rejects zero length");
    expectTrue(!TaaJitterLayout::fillHaltonSequence(8u, nullptr), "fillHaltonSequence rejects null output");
}

void testJitterSequencePeriod() {
    using fuse::renderer::TaaJitterLayout;

    expectTrue(TaaJitterLayout::sequencePeriod(8u) == 8u, "default sequence period is 8");
    expectTrue(TaaJitterLayout::sequencePeriod(0u) == 0u, "invalid sequence period is zero");
    expectTrue(TaaJitterLayout::sequencePeriod(fuse::renderer::kTaaMaxJitterSequenceLength + 1u) == 0u,
               "oversized sequence period is zero");

    fuse::renderer::TaaJitter jitter;
    const fuse::math::Vec2 periodStart = jitter.currentPixelOffset();
    for (fuse::u32 frame = 0u; frame < 8u; ++frame) {
        expectTrue(TaaJitterLayout::frameIndexInSequence(frame, 8u) == frame,
                   "frame index maps into sequence slot");
        const fuse::math::Vec2 frameOffset = TaaJitterLayout::offsetForFrameIndex(frame, 8u);
        const fuse::math::Vec2 slotOffset = TaaJitterLayout::haltonPixelOffset(frame, 8u);
        expectNear(frameOffset.x, slotOffset.x, 1e-5f, "offsetForFrameIndex matches slot offset X");
        expectNear(frameOffset.y, slotOffset.y, 1e-5f, "offsetForFrameIndex matches slot offset Y");
        jitter.advance();
    }
    expectTrue(jitter.index() == 0u, "default sequence wraps after one period");
    const fuse::math::Vec2 periodEnd = jitter.currentPixelOffset();
    expectNear(periodEnd.x, periodStart.x, 1e-5f, "sequence period returns to first sample");

    const fuse::math::Vec2 wrappedOffset = TaaJitterLayout::offsetForFrameIndex(8u, 8u);
    expectNear(wrappedOffset.x, periodStart.x, 1e-5f, "offsetForFrameIndex wraps with sequence period");
}

void testJitterLargeFrameWrap() {
    using fuse::renderer::TaaJitterLayout;

    expectTrue(TaaJitterLayout::frameIndexInSequence(1008u, 8u) == 0u,
               "large frame index wraps to sequence start");
    expectTrue(TaaJitterLayout::frameIndexInSequence(0xFFFFFFFFu, 8u) == 7u,
               "UINT32_MAX frame index maps into final slot");

    const fuse::math::Vec2 start = TaaJitterLayout::offsetForFrameIndex(0u, 8u);
    const fuse::math::Vec2 wrapped = TaaJitterLayout::offsetForFrameIndex(1008u, 8u);
    expectNear(wrapped.x, start.x, 1e-5f, "offsetForFrameIndex wraps across many periods");
    expectNear(wrapped.y, start.y, 1e-5f, "offsetForFrameIndex Y wraps across many periods");
}

void testNdcOffsetForFrameIndex() {
    using fuse::renderer::TaaJitterLayout;

    const fuse::math::Vec2 ndc0 = TaaJitterLayout::ndcOffsetForFrameIndex(0u, 1920u, 1080u, 8u);
    const fuse::math::Vec2 ndcDirect =
        TaaJitterLayout::haltonNdcOffset(0u, 1920u, 1080u, 8u);
    expectNear(ndc0.x, ndcDirect.x, 1e-6f, "ndcOffsetForFrameIndex matches haltonNdcOffset at slot 0");
    expectNear(ndc0.y, ndcDirect.y, 1e-6f, "ndcOffsetForFrameIndex Y matches haltonNdcOffset at slot 0");

    const fuse::math::Vec2 ndcWrapped = TaaJitterLayout::ndcOffsetForFrameIndex(8u, 1920u, 1080u, 8u);
    expectNear(ndcWrapped.x, ndc0.x, 1e-6f, "ndcOffsetForFrameIndex wraps with sequence period");
}

void testJitterSyncToFrameIndex() {
    fuse::renderer::TaaJitter jitter;
    jitter.syncToFrameIndex(5u);
    expectTrue(jitter.index() == 5u, "syncToFrameIndex sets slot directly");
    expectTrue(jitter.monotonicFrameIndex() == 5u, "syncToFrameIndex sets monotonic frame counter");

    const fuse::math::Vec2 synced = jitter.currentPixelOffset();
    const fuse::math::Vec2 expected = fuse::renderer::TaaJitterLayout::offsetForFrameIndex(5u, 8u);
    expectNear(synced.x, expected.x, 1e-5f, "syncToFrameIndex offset matches offsetForFrameIndex");
    expectNear(synced.y, expected.y, 1e-5f, "syncToFrameIndex Y matches offsetForFrameIndex");

    jitter.syncToFrameIndex(13u);
    expectTrue(jitter.index() == 5u, "syncToFrameIndex wraps monotonic frame counter");
    expectTrue(jitter.monotonicFrameIndex() == 13u, "syncToFrameIndex preserves monotonic frame counter");

    jitter.advance();
    expectTrue(jitter.monotonicFrameIndex() == 14u, "advance increments monotonic frame counter");
    expectTrue(jitter.index() == 6u, "advance wraps slot from monotonic counter");

    jitter.reset();
    expectTrue(jitter.monotonicFrameIndex() == 0u, "reset clears monotonic frame counter");
}

void testCustomJitterSequenceLength() {
    fuse::renderer::TaaJitterDesc desc{};
    desc.sequence_length = 4u;
    fuse::renderer::TaaJitter jitter(desc);
    expectTrue(jitter.sequenceLength() == 4u, "custom jitter sequence length applied");

    const fuse::math::Vec2 first = jitter.currentPixelOffset();
    expectNear(first.x, 0.5f, 1e-5f, "custom sequence first sample X");

    for (fuse::u32 i = 0u; i < 4u; ++i) {
        jitter.advance();
    }
    expectTrue(jitter.index() == 0u, "custom sequence wraps after four advances");
    const fuse::math::Vec2 wrapped = jitter.currentPixelOffset();
    expectNear(wrapped.x, first.x, 1e-5f, "custom sequence wraps to first sample");
}

void testHaltonComputeMatchesTable() {
    using fuse::renderer::TaaJitterLayout;

    expectNear(TaaJitterLayout::halton(1u, 2u), 0.5f, 1e-5f, "halton(1,2)");
    expectNear(TaaJitterLayout::halton(2u, 2u), 0.25f, 1e-5f, "halton(2,2)");
    expectNear(TaaJitterLayout::halton(1u, 3u), 0.333f, 1e-3f, "halton(1,3)");
    expectNear(TaaJitterLayout::halton(8u, 3u), 0.889f, 1e-3f, "halton(8,3)");
}

void testJitterNdcOffset() {
    const fuse::math::Vec2 ndc =
        fuse::renderer::TaaJitter::haltonNdcOffset(0u, 1920u, 1080u);
    expectNear(ndc.x, 0.f, 1e-6f, "centred Halton X maps to zero NDC at 1920");
    expectTrue(ndc.y < 0.f, "Halton Y below centre yields negative NDC Y");
}

void testHistoryBufferPingPong() {
    auto bootstrap = createTaaTestBootstrap();
    if (bootstrap == nullptr) {
        std::fprintf(stderr, "SKIP: bootstrap allocated for TAA history test (Vulkan device unavailable)\n");
        return;
    }
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    TAA_SKIP_UNLESS_VULKAN_READY(bootstrap);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for TAA history test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    expectTrue(resources.init(*bootstrap->device(), bindless), "resource manager ready for TAA history");

    fuse::renderer::TaaHistoryBuffer history;
    fuse::renderer::TaaHistoryBufferDesc desc{};
    desc.width = 64;
    desc.height = 64;
    expectTrue(history.init(resources, desc), "history buffer allocated");
    expectTrue(history.activeIndex() == 0u, "history starts on buffer A");

    const fuse::u32 readBefore = history.read().index();
    const fuse::u32 writeBefore = history.write().index();
    expectTrue(readBefore != writeBefore, "read/write handles differ before swap");

    history.swap();
    expectTrue(history.activeIndex() == 1u, "history swapped to buffer B");
    expectTrue(history.read().index() == writeBefore, "read handle follows swap");
    expectTrue(history.write().index() == readBefore, "write handle follows swap");

    history.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testHistoryValidityFlags() {
    auto bootstrap = createTaaTestBootstrap();
    if (bootstrap == nullptr) {
        std::fprintf(stderr, "SKIP: bootstrap allocated for TAA history validity test (Vulkan device unavailable)\n");
        return;
    }
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    TAA_SKIP_UNLESS_VULKAN_READY(bootstrap);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for TAA history validity test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaHistoryBuffer history;
    fuse::renderer::TaaHistoryBufferDesc historyDesc{64, 64};
    expectTrue(history.init(resources, historyDesc), "history ready for validity checks");
    expectTrue(!history.hasValidHistory(), "history invalid before first resolve");
    expectTrue(history.needsWarmup(), "history needs warmup before first resolve");
    expectTrue(history.accumulatedFrames() == 0u, "no accumulated frames before resolve");

    history.markResolved();
    expectTrue(history.hasValidHistory(), "history valid after first resolve");
    expectTrue(!history.needsWarmup(), "history no longer needs warmup after resolve");
    expectTrue(history.accumulatedFrames() == 1u, "one accumulated frame after first resolve");

    history.invalidateHistory();
    expectTrue(!history.hasValidHistory(), "invalidate clears validity flag");
    expectTrue(history.accumulatedFrames() == 0u, "invalidate clears accumulated frame count");

    history.resize(128, 128);
    expectTrue(!history.hasValidHistory(), "resize invalidates history");
    expectTrue(history.desc().width == 128u, "history resized width");
    expectTrue(history.desc().height == 128u, "history resized height");

    history.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testBlendWeightGuards() {
    fuse::renderer::TAAParams params{};
    params.blend_factor = 0.15f;

    expectTrue(fuse::renderer::isTaaBlendFactorInRange(0.f), "zero blend factor in range");
    expectTrue(fuse::renderer::isTaaBlendFactorInRange(1.f), "unit blend factor in range");
    expectTrue(fuse::renderer::isTaaBlendFactorInRange(0.15f), "typical blend factor in range");
    expectTrue(!fuse::renderer::isTaaBlendFactorInRange(-0.1f), "negative blend factor out of range");
    expectTrue(!fuse::renderer::isTaaBlendFactorInRange(1.1f), "blend factor above one out of range");

    expectTrue(fuse::renderer::taaUsesWarmupBlend(true), "first frame uses warmup blend");
    expectTrue(!fuse::renderer::taaUsesWarmupBlend(false), "subsequent frames do not use warmup blend");

    const fuse::f32 warmupBlend = fuse::renderer::computeEffectiveBlend(true, params);
    expectNear(warmupBlend, 1.f, 1e-5f, "warmup effective blend is full current weight");
    expectTrue(!fuse::renderer::taaBlendWeightReusesHistory(warmupBlend),
               "warmup blend does not reuse history");
    expectNear(fuse::renderer::computeHistoryContributionWeight(warmupBlend), 0.f, 1e-5f,
               "warmup history contribution is zero");
    expectNear(fuse::renderer::computeHistoryBlend(warmupBlend), 0.f, 1e-5f,
               "warmup history blend matches contribution weight");

    const fuse::f32 steadyBlend = fuse::renderer::computeEffectiveBlend(false, params);
    expectNear(steadyBlend, 0.15f, 1e-5f, "steady effective blend uses configured factor");
    expectTrue(fuse::renderer::taaBlendWeightReusesHistory(steadyBlend),
               "steady blend reuses history");
    expectNear(fuse::renderer::computeHistoryContributionWeight(steadyBlend), 0.85f, 1e-5f,
               "steady history contribution complements effective blend");
}

void testHistoryReuseGuards() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for history reuse guard test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaHistoryBuffer history;
    fuse::renderer::TaaHistoryBufferDesc historyDesc{64, 64};
    expectTrue(history.init(resources, historyDesc), "history ready for reuse guard test");
    expectTrue(!history.canReuseHistory(), "history cannot reuse before first resolve");
    expectTrue(!fuse::renderer::taaHistoryCanReuse(history), "free helper rejects unwarmed history");
    expectTrue(!fuse::renderer::taaHistoryReuseReady(history, 0u),
               "reuse-ready requires warmed history");

    history.markResolved();
    expectTrue(history.canReuseHistory(), "history can reuse after first resolve");
    expectTrue(fuse::renderer::taaHistoryCanReuse(history), "free helper accepts warmed history");
    expectTrue(fuse::renderer::taaHistoryReuseReady(history, 0u),
               "reuse-ready passes with matching generation");
    expectTrue(fuse::renderer::taaHistoryIsGenerationCurrent(history, 0u),
               "generation current helper matches warmed epoch");

    history.invalidateHistory();
    expectTrue(!history.canReuseHistory(), "invalidate clears reuse readiness");
               "stale generation fails reuse-ready guard");
    expectTrue(!fuse::renderer::taaHistoryReuseReady(history, history.invalidateGeneration()),
               "reuse-ready requires warmed history even with current generation");
    expectTrue(fuse::renderer::taaHistoryIsGenerationCurrent(history, history.invalidateGeneration()),
               "current generation matches after invalidate");

    history.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testJitterNdcGuards() {
    using fuse::renderer::TaaJitterLayout;

    expectTrue(TaaJitterLayout::canComputeNdcOffset(1920u, 1080u), "non-zero viewport can compute NDC");
    expectTrue(!TaaJitterLayout::canComputeNdcOffset(0u, 1080u), "zero width cannot compute NDC");
    expectTrue(TaaJitterLayout::validateViewportDimensions(1920u, 1080u),
               "validateViewportDimensions agrees with canComputeNdcOffset");

    const fuse::math::Vec2 safeZero = TaaJitterLayout::safeNdcOffsetForFrameIndex(0u, 0u, 1080u, 8u);
    expectNear(safeZero.x, 0.f, 1e-6f, "safe NDC offset is zero for invalid viewport X");
    expectNear(safeZero.y, 0.f, 1e-6f, "safe NDC offset is zero for invalid viewport Y");

    const fuse::math::Vec2 safeValid = TaaJitterLayout::safeNdcOffsetForFrameIndex(0u, 1920u, 1080u, 8u);
    const fuse::math::Vec2 direct =
        TaaJitterLayout::ndcOffsetForFrameIndex(0u, 1920u, 1080u, 8u);
    expectNear(safeValid.x, direct.x, 1e-6f, "safe NDC offset matches direct offset for valid viewport");
    expectNear(safeValid.y, direct.y, 1e-6f, "safe NDC offset Y matches direct offset");

    fuse::renderer::TaaJitter jitter;
    expectTrue(jitter.canProvideNdcOffset(128u, 128u), "jitter can provide NDC for valid viewport");
    expectTrue(!jitter.canProvideNdcOffset(0u, 128u), "jitter rejects zero-width viewport");

void testResolvePreflightGuards() {
    expectTrue(bootstrap != nullptr, "bootstrap allocated for resolve preflight guard test");



    expectTrue(history.init(resources, historyDesc), "history ready for resolve preflight guard test");
    expectTrue(fuse::renderer::taaHistoryCanAccumulate(history), "ready history can accumulate");

    fuse::renderer::TaaResolveDesc desc{};
    desc.width = 64;
    desc.height = 64;
    expectTrue(!fuse::renderer::taaResolveSurfacesSatisfied(desc),
               "surfaces guard rejects missing bindings");
    desc.surfaces.current_frame = reinterpret_cast<void*>(0x1);
    desc.surfaces.output = reinterpret_cast<void*>(0x2);
    expectTrue(fuse::renderer::taaResolveSurfacesSatisfied(desc),
               "surfaces guard accepts current/output bindings");
    expectTrue(fuse::renderer::canAttemptTaaResolve(desc, history),
               "canAttempt passes valid resolve request");
    expectTrue(fuse::renderer::prepareTaaResolveDesc(desc, history),
               "prepare stamps generation and passes valid request");
    expectTrue(desc.observed_history_generation == 0u, "prepare stamps current generation");

    desc.observed_history_generation = fuse::renderer::kTaaResolveNoHistoryGeneration;
               "prepare restamps sentinel generation");
    expectTrue(desc.observed_history_generation == 0u, "prepare fills sentinel with current generation");

    desc.observed_history_generation = 0u;
    expectTrue(!fuse::renderer::canAttemptTaaResolve(desc, history),
               "canAttempt rejects stale generation");
               "prepare restamps after invalidate");


void testTaaPassCanResolveFrame() {
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass canResolve test");



    fuse::renderer::TaaPassDesc passDesc{};
    passDesc.width = 64;
    passDesc.height = 64;

    auto pass = fuse::renderer::TaaPass::create(passDesc);
    expectTrue(pass->init(resources), "TaaPass initialized for canResolve test");

    expectTrue(pass->canResolveFrame(desc), "pass canResolveFrame passes valid desc");
    expectTrue(pass->prepareAndCanResolve(desc), "pass prepareAndCanResolve stamps and passes");
    expectTrue(desc.observed_history_generation == 0u, "pass prepare stamps generation");

    desc.width = 32;
    expectTrue(!pass->canResolveFrame(desc), "pass canResolveFrame rejects dimension mismatch");

    pass->destroy();

void testClampTaaParams() {
    fuse::renderer::TAAParams raw{};
    raw.blend_factor = 2.f;
    raw.velocity_rejection = -0.5f;
    raw.depth_rejection = -1.f;
    raw.clamp_gamma = 0.5f;

    expectTrue(!fuse::renderer::taaParamsInRange(raw), "out-of-range params rejected by taaParamsInRange");

    const fuse::renderer::TAAParams clamped = fuse::renderer::clampTaaParams(raw);
    expectNear(clamped.blend_factor, 1.f, 1e-5f, "blend_factor clamped to 1");
    expectNear(clamped.velocity_rejection, 0.f, 1e-5f, "velocity_rejection clamped to zero");
    expectNear(clamped.depth_rejection, 0.f, 1e-5f, "depth_rejection clamped to zero");
    expectNear(clamped.clamp_gamma, 1.f, 1e-5f, "clamp_gamma clamped to 1");
    expectTrue(fuse::renderer::taaParamsInRange(clamped), "clamped params are in range");

    fuse::renderer::TAAParams inPlace = raw;
    fuse::renderer::normalizeTaaParams(inPlace);
    expectNear(inPlace.blend_factor, 1.f, 1e-5f, "normalizeTaaParams clamps blend_factor in place");
    expectTrue(fuse::renderer::taaParamsInRange(inPlace), "normalizeTaaParams leaves params in range");
    expectTrue(fuse::renderer::taaParamsRequireClamping(raw), "out-of-range params require clamping");

    fuse::renderer::TAAParams safe{};
    safe.blend_factor = 0.1f;
    expectTrue(!fuse::renderer::taaParamsRequireClamping(safe), "in-range params do not require clamping");

    expectNear(fuse::renderer::computeEffectiveBlend(true, raw), 1.f, 1e-5f,
               "first frame effective blend is full current weight");
    expectNear(fuse::renderer::computeEffectiveBlend(false, raw), 1.f, 1e-5f,
               "subsequent effective blend uses clamped blend_factor");
    expectNear(fuse::renderer::computeHistoryBlend(0.15f), 0.85f, 1e-5f,
               "history blend is complement of effective blend");
    expectNear(fuse::renderer::computeHistoryBlend(1.f), 0.f, 1e-5f,
               "full current weight yields zero history blend");
}

void testRejectionSurfaceHelpers() {
    fuse::renderer::TAAParams params{};
    params.velocity_rejection = 0.f;
    params.depth_rejection = 0.f;
    expectTrue(!fuse::renderer::taaResolveRequiresVelocity(params), "zero velocity rejection");
    expectTrue(!fuse::renderer::taaResolveRequiresDepth(params), "zero depth rejection");
    expectTrue(!fuse::renderer::taaResolveRequiresRejectionSurfaces(params),
               "no rejection surfaces required when rejection disabled");

    params.velocity_rejection = 0.25f;
    expectTrue(fuse::renderer::taaResolveRequiresVelocity(params), "velocity rejection active");
    expectTrue(fuse::renderer::taaResolveRequiresRejectionSurfaces(params),
               "velocity rejection requires surfaces");

    params.depth_rejection = 0.1f;
    expectTrue(fuse::renderer::taaResolveRequiresDepth(params), "depth rejection active");
               "depth rejection requires surfaces");
}

void testComputeEffectiveBlendForHistory() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for effective blend history test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaHistoryBuffer history;
    fuse::renderer::TaaHistoryBufferDesc historyDesc{64, 64};
    expectTrue(history.init(resources, historyDesc), "history ready for effective blend test");

    params.blend_factor = 0.2f;
    expectNear(fuse::renderer::computeEffectiveBlendForHistory(history, params), 1.f, 1e-5f,
               "warm-up history uses full current weight");

    history.markResolved();
    expectNear(fuse::renderer::computeEffectiveBlendForHistory(history, params), 0.2f, 1e-5f,
               "valid history uses configured blend");

    history.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());

void testHistoryGenerationAndAcceptanceHelpers() {
    expectTrue(bootstrap != nullptr, "bootstrap allocated for generation helper test");



    expectTrue(history.init(resources, historyDesc), "history ready for generation helper test");
    expectTrue(history.generationMatches(0u), "initial generation matches");
    expectTrue(!history.isHistoryStale(0u), "generationMatches mirrors isHistoryStale");
    expectTrue(history.canAcceptResolveAt(64u, 64u), "ready history accepts matching resolve");
    expectTrue(!history.canAcceptResolveAt(128u, 64u), "ready history rejects mismatched resolve");

    history.invalidateHistory();
    expectTrue(!history.generationMatches(0u), "invalidate breaks generation match");
    expectTrue(history.generationMatches(history.invalidateGeneration()), "current generation always matches");

    expectTrue(!history.canAcceptResolveAt(64u, 64u), "destroyed history cannot accept resolve");


void testPreflightAndCanProceedHelpers() {
    expectTrue(bootstrap != nullptr, "bootstrap allocated for preflight helper test");



    expectTrue(history.init(resources, historyDesc), "history ready for preflight helper test");

    fuse::renderer::TaaResolveDesc desc{};
    desc.width = 64;
    desc.height = 64;
    desc.surfaces.current_frame = reinterpret_cast<void*>(0x1);
    desc.surfaces.output = reinterpret_cast<void*>(0x2);

    fuse::renderer::TaaResolveSkipReason skipReason = fuse::renderer::TaaResolveSkipReason::HistoryNotReady;
    expectTrue(fuse::renderer::preflightTaaResolve(desc, history) == fuse::renderer::TaaResolveSkipReason::None,
               "preflight stamps generation and passes valid desc");
    expectTrue(desc.observed_history_generation == 0u, "preflight stamps current generation");
    expectTrue(fuse::renderer::isObservedHistoryGenerationCurrent(desc, history),
               "stamped generation is current");
    expectTrue(fuse::renderer::taaResolveCanProceed(desc, history, &skipReason),
               "canProceed passes valid desc");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::None, "canProceed reports None");

    expectTrue(!fuse::renderer::isObservedHistoryGenerationCurrent(desc, history),
               "stale stamped generation is not current");
    expectTrue(!fuse::renderer::taaResolveCanProceed(desc, history, &skipReason),
               "canProceed rejects stale generation");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::StaleHistoryGeneration,
               "canProceed reports stale generation");

    desc.width = 0;
    expectTrue(fuse::renderer::preflightTaaResolve(desc, history) ==
                   fuse::renderer::TaaResolveSkipReason::InvalidDimensions,
               "preflight classifies invalid dimensions");


void testViewportDimensionHelpers() {
    desc.width = 1920;
    desc.height = 1080;
    expectTrue(fuse::renderer::taaViewportDimensionsMatchPass(1920u, 1080u, desc),
               "matching pass viewport");
    expectTrue(!fuse::renderer::taaViewportDimensionsMatchPass(1280u, 1080u, desc),
               "pass viewport width mismatch");
    expectTrue(!fuse::renderer::taaViewportDimensionsMatchPass(0u, 1080u, desc),
               "invalid pass viewport rejected");

    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass viewport test");



    fuse::renderer::TaaPassDesc passDesc{};
    passDesc.width = 64;
    passDesc.height = 64;
    auto pass = fuse::renderer::TaaPass::create(passDesc);
    expectTrue(pass->init(resources), "TaaPass initialized for viewport helper test");
    expectTrue(pass->viewportMatchesResolve(desc) == false, "pass viewport rejects mismatched resolve");

    fuse::renderer::TaaResolveDesc matching{};
    matching.width = 64;
    matching.height = 64;
    expectTrue(pass->viewportMatchesResolve(matching), "pass viewport accepts matching resolve");

    pass->destroy();

void testTaaPassResolveFrameNoOpOnSkip() {
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass no-op skip test");



    expectTrue(pass->init(resources), "TaaPass initialized for no-op skip test");

    fuse::renderer::TaaResolveDesc resolveDesc{};
    resolveDesc.width = 32;
    resolveDesc.height = 64;
    resolveDesc.surfaces.current_frame = reinterpret_cast<void*>(0x10);
    resolveDesc.surfaces.output = reinterpret_cast<void*>(0x20);

    expectTrue(!pass->resolveFrame(resolveDesc), "resolveFrame no-ops on dimension mismatch");
    expectTrue(pass->lastStats().framesResolved == 0u, "skipped resolve does not advance frame count");
    expectTrue(pass->history().accumulatedFrames() == 0u, "skipped resolve does not mutate history");

    resolveDesc.width = 64;
    expectTrue(pass->resolveFrame(resolveDesc), "resolveFrame succeeds after auto-stamp");
    expectTrue(pass->lastStats().framesResolved == 1u, "successful resolve advances frame count");
    expectTrue(pass->history().hasValidHistory(), "successful resolve warms history");


void testHistoryBlendHelpers() {

    expectNear(fuse::renderer::clampEffectiveBlend(1.5f), 1.f, 1e-5f, "clampEffectiveBlend caps high values");
    expectNear(fuse::renderer::clampEffectiveBlend(-0.25f), 0.f, 1e-5f, "clampEffectiveBlend floors low values");

    expectNear(fuse::renderer::computeHistoryBlendWeight(0.2f), 0.8f, 1e-5f,
               "history blend weight complements effective blend");
    expectNear(fuse::renderer::computeHistoryBlendWeight(true, params), 0.f, 1e-5f,
               "first frame history blend weight is zero");
    expectNear(fuse::renderer::computeHistoryBlendWeight(false, params), 0.8f, 1e-5f,
               "subsequent history blend weight uses clamped blend_factor");

    const fuse::f32 effective = fuse::renderer::computeEffectiveBlend(false, params);
    expectNear(fuse::renderer::computeHistoryBlendWeight(effective) + effective, 1.f, 1e-5f,
               "history and current blend weights sum to one");

void testHistoryReadGuards() {
    fuse::renderer::TaaHistoryBuffer emptyHistory;
    expectTrue(!fuse::renderer::canReadHistoryForResolve(emptyHistory),
               "empty history cannot be read for resolve");
    expectTrue(!fuse::renderer::historyAwaitingWarmup(emptyHistory),
               "unallocated history is not awaiting warmup");
    expectTrue(!emptyHistory.canReadForResolve(), "canReadForResolve false before init");

    expectTrue(bootstrap != nullptr, "bootstrap allocated for history read guard test");



    expectTrue(history.init(resources, historyDesc), "history ready for read guard test");
    expectTrue(fuse::renderer::historyAwaitingWarmup(history), "allocated history awaits warmup");
    expectTrue(!fuse::renderer::canReadHistoryForResolve(history), "cold history cannot be read");

    expectTrue(fuse::renderer::canReadHistoryForResolve(history), "warm history can be read");
    expectTrue(!fuse::renderer::historyAwaitingWarmup(history), "warm history no longer awaits warmup");
    expectTrue(history.canReadForResolve(), "canReadForResolve true after first resolve");

    expectTrue(!fuse::renderer::canReadHistoryForResolve(history), "invalidated history cannot be read");
    expectTrue(fuse::renderer::historyAwaitingWarmup(history), "invalidated history awaits warmup again");


void testResolveSurfaceGuards() {
    expectTrue(fuse::renderer::taaResolveSurfacesComplete(desc), "complete surfaces pass guard");
    expectTrue(fuse::renderer::taaResolveRejectionSurfacesComplete(desc),
               "rejection guard passes when enforcement disabled");

    desc.surfaces.output = nullptr;
    expectTrue(!fuse::renderer::taaResolveSurfacesComplete(desc), "missing output fails surface guard");

    desc.enforce_rejection_surfaces = true;
    desc.params.velocity_rejection = 0.5f;
    expectTrue(!fuse::renderer::taaResolveRejectionSurfacesComplete(desc),
               "missing velocity fails rejection guard when enforced");

    desc.surfaces.velocity_buffer = reinterpret_cast<void*>(0x3);
    desc.params.velocity_rejection = 0.f;
    desc.params.depth_rejection = 0.25f;
               "missing depth fails rejection guard when enforced");

    desc.surfaces.depth_buffer = reinterpret_cast<void*>(0x4);
               "complete rejection surfaces pass guard");

void testShouldSkipTaaResolve() {

    fuse::renderer::TaaResolveSkipReason skipReason = fuse::renderer::TaaResolveSkipReason::None;
    expectTrue(fuse::renderer::shouldSkipTaaResolve(desc, emptyHistory, &skipReason),
               "shouldSkipTaaResolve detects empty history");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::HistoryNotReady,
               "shouldSkipTaaResolve reports HistoryNotReady");

    expectTrue(bootstrap != nullptr, "bootstrap allocated for shouldSkipTaaResolve test");



    expectTrue(history.init(resources, historyDesc), "history ready for shouldSkipTaaResolve test");

    desc.width = 128;
    skipReason = fuse::renderer::TaaResolveSkipReason::None;
    expectTrue(fuse::renderer::shouldSkipTaaResolve(desc, history, &skipReason),
               "shouldSkipTaaResolve detects dimension mismatch");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::DimensionMismatch,
               "shouldSkipTaaResolve reports DimensionMismatch");

    desc.surfaces.current_frame = nullptr;
               "shouldSkipTaaResolve detects missing surfaces");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::MissingSurfaces,
               "shouldSkipTaaResolve reports MissingSurfaces");

    expectTrue(!fuse::renderer::shouldSkipTaaResolve(desc, history), "shouldSkipTaaResolve passes valid desc");

    fuse::renderer::TaaResolve resolve;
    expectTrue(resolve.wouldSkip(desc, history, &skipReason) ==
                   fuse::renderer::shouldSkipTaaResolve(desc, history, &skipReason),
               "shouldSkipTaaResolve matches TaaResolve::wouldSkip");


void testTaaResolveSkipCounts() {
    fuse::renderer::TaaResolveSkipCounts noneCounts =
        fuse::renderer::taaResolveSkipCountsFromReason(fuse::renderer::TaaResolveSkipReason::None);
    expectTrue(noneCounts.total == 0u, "None reason produces zero skip counts");

    const fuse::renderer::TaaResolveSkipCounts staleCounts =
        fuse::renderer::taaResolveSkipCountsFromReason(
            fuse::renderer::TaaResolveSkipReason::StaleHistoryGeneration);
    expectTrue(staleCounts.total == 1u, "stale generation counted once");
    expectTrue(staleCounts.staleHistoryGeneration == 1u, "stale generation bucket incremented");

    fuse::renderer::TaaResolveSkipCounts accumulated{};
    fuse::renderer::accumulateTaaResolveSkipReason(accumulated,
                                                     fuse::renderer::TaaResolveSkipReason::MissingSurfaces);
                                                     fuse::renderer::TaaResolveSkipReason::DimensionMismatch);
    fuse::renderer::accumulateTaaResolveSkipReason(accumulated, fuse::renderer::TaaResolveSkipReason::None);
    expectTrue(accumulated.total == 2u, "accumulate counts blocking reasons only");
    expectTrue(accumulated.missingSurfaces == 1u, "missing surfaces bucket incremented");
    expectTrue(accumulated.dimensionMismatch == 1u, "dimension mismatch bucket incremented");

void testTaaPassEffectiveBlend() {
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass effective blend test");



    passDesc.params.blend_factor = 0.25f;

    expectTrue(pass->init(resources), "TaaPass initialized for effective blend test");
    expectTrue(!pass->canReadHistory(), "pass cannot read history before first resolve");
    expectNear(pass->effectiveBlendForNextResolve(), 1.f, 1e-5f,
               "pass uses full current weight before warmup");

    resolveDesc.surfaces.current_frame = reinterpret_cast<void*>(0x1);
    resolveDesc.surfaces.output = reinterpret_cast<void*>(0x2);
    expectTrue(pass->resolveFrame(resolveDesc), "initial resolve succeeds");
    expectTrue(pass->canReadHistory(), "pass can read history after first resolve");
    expectNear(pass->effectiveBlendForNextResolve(), 0.25f, 1e-5f,
               "pass uses configured blend after warmup");

    pass->invalidateHistory();
    expectTrue(!pass->canReadHistory(), "pass cannot read history after invalidate");
               "pass returns to full current weight after invalidate");


void testBlendWeightGuards() {
    fuse::renderer::TAAParams params{};
    params.blend_factor = 0.15f;

    expectNear(fuse::renderer::computeEffectiveBlend(false, true, params), 0.15f, 1e-5f,
               "reusable history uses configured blend");
    expectNear(fuse::renderer::computeEffectiveBlend(false, false, params), 1.f, 1e-5f,
               "non-reusable history forces full current weight");
    expectNear(fuse::renderer::computeEffectiveBlend(true, true, params), 1.f, 1e-5f,
               "first frame forces full current weight even when reusable");

    expectTrue(fuse::renderer::taaBlendUsesHistory(0.15f), "partial blend uses history");
    expectTrue(!fuse::renderer::taaBlendUsesHistory(1.f), "full current blend does not use history");
    expectTrue(fuse::renderer::taaBlendSkipsHistoryReuse(1.f), "full current blend skips history reuse");
    expectTrue(!fuse::renderer::taaBlendSkipsHistoryReuse(0.15f),
               "partial blend does not skip history reuse");

    params.blend_factor = 1.f;
    expectNear(fuse::renderer::computeEffectiveBlend(false, true, params), 1.f, 1e-5f,
               "clamped full blend skips history reuse");
    expectTrue(!fuse::renderer::taaBlendUsesHistory(1.f), "clamped full blend does not use history");
}

void testJitterSafeNdcOffset() {
    using fuse::renderer::TaaJitterLayout;

    const fuse::math::Vec2 safeZero = TaaJitterLayout::safeHaltonNdcOffset(0u, 0u, 1080u, 8u);
    expectNear(safeZero.x, 0.f, 1e-6f, "safeHaltonNdcOffset returns zero for invalid width");
    expectNear(safeZero.y, 0.f, 1e-6f, "safeHaltonNdcOffset Y returns zero for invalid width");

    const fuse::math::Vec2 safeFrameZero = TaaJitterLayout::safeNdcOffsetForFrameIndex(3u, 1920u, 0u, 8u);
    expectNear(safeFrameZero.x, 0.f, 1e-6f, "safeNdcOffsetForFrameIndex returns zero for invalid height");
    expectNear(safeFrameZero.y, 0.f, 1e-6f, "safeNdcOffsetForFrameIndex Y returns zero for invalid height");

    const fuse::math::Vec2 safe = TaaJitterLayout::safeNdcOffsetForFrameIndex(3u, 1920u, 1080u, 8u);
    const fuse::math::Vec2 direct = TaaJitterLayout::ndcOffsetForFrameIndex(3u, 1920u, 1080u, 8u);
    expectNear(safe.x, direct.x, 1e-6f, "safeNdcOffsetForFrameIndex matches direct offset for valid viewport");
    expectNear(safe.y, direct.y, 1e-6f, "safeNdcOffsetForFrameIndex Y matches direct offset for valid viewport");
}

void testHistoryReusableGuard() {
    fuse::renderer::TaaHistoryBuffer emptyHistory;
    fuse::renderer::TaaResolveDesc desc{};
    expectTrue(!fuse::renderer::taaHistoryIsReusable(emptyHistory, desc),
               "empty history is not reusable");

    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for history reusable guard test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaHistoryBuffer history;
    fuse::renderer::TaaHistoryBufferDesc historyDesc{64, 64};
    expectTrue(history.init(resources, historyDesc), "history ready for reusable guard test");
    expectTrue(!fuse::renderer::taaHistoryIsReusable(history, desc),
               "unwarmed history is not reusable");

    desc.width = 64;
    desc.height = 64;
    desc.surfaces.current_frame = reinterpret_cast<void*>(0x1);
    desc.surfaces.output = reinterpret_cast<void*>(0x2);

    fuse::renderer::TaaResolve resolve;
    expectTrue(resolve.resolve(desc, history), "initial resolve warms history");
    expectTrue(fuse::renderer::taaHistoryIsReusable(history, desc),
               "warmed history is reusable with current generation");

    history.invalidateHistory();
    desc.observed_history_generation = 0u;
    expectTrue(!fuse::renderer::taaHistoryIsReusable(history, desc),
               "invalidated history is not reusable");
    expectTrue(fuse::renderer::classifyTaaResolveSkip(desc, history) ==
                   fuse::renderer::TaaResolveSkipReason::StaleHistoryGeneration,
               "stale generation still blocks resolve before reuse");

    desc.observed_history_generation = history.invalidateGeneration();
    expectTrue(!fuse::renderer::taaHistoryIsReusable(history, desc),
               "invalidated history remains non-reusable until next resolve");

    history.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testResolveHistoryReusedStat() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for history reused stat test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaHistoryBuffer history;
    fuse::renderer::TaaHistoryBufferDesc historyDesc{64, 64};
    expectTrue(history.init(resources, historyDesc), "history ready for history reused stat test");

    fuse::renderer::TaaResolve resolve;
    fuse::renderer::TaaResolveDesc desc{};
    desc.width = 64;
    desc.height = 64;
    desc.surfaces.current_frame = reinterpret_cast<void*>(0x1);
    desc.surfaces.output = reinterpret_cast<void*>(0x2);
    desc.params.blend_factor = 0.2f;
    desc.observed_history_generation = 0u;

    expectTrue(resolve.resolve(desc, history), "first resolve succeeds");
    expectTrue(!resolve.lastStats().history_reused, "first resolve does not reuse history");
    expectTrue(resolve.lastStats().first_frame, "first resolve marks first frame");

    expectTrue(resolve.resolve(desc, history), "second resolve succeeds");
    expectTrue(resolve.lastStats().history_reused, "second resolve reuses history with partial blend");
    expectTrue(!resolve.lastStats().first_frame, "second resolve is not first frame");

    desc.params.blend_factor = 1.f;
    expectTrue(resolve.resolve(desc, history), "full blend resolve succeeds");
    expectTrue(!resolve.lastStats().history_reused, "full blend resolve does not reuse history");

    history.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testHistoryBlendHelpers() {
    fuse::renderer::TAAParams params{};
    params.blend_factor = 0.2f;

    expectTrue(fuse::renderer::isTaaBlendFactorInRange(0.2f), "in-range blend factor accepted");
    expectTrue(!fuse::renderer::isTaaBlendFactorInRange(1.5f), "out-of-range blend factor rejected");

    expectNear(fuse::renderer::clampEffectiveBlend(1.5f), 1.f, 1e-5f, "clampEffectiveBlend caps high values");
    expectNear(fuse::renderer::clampEffectiveBlend(-0.25f), 0.f, 1e-5f, "clampEffectiveBlend floors low values");

    expectTrue(!fuse::renderer::taaBlendWeightReusesHistory(1.f), "full current weight does not reuse history");
    expectTrue(fuse::renderer::taaBlendWeightReusesHistory(0.2f), "partial blend reuses history");

    expectTrue(fuse::renderer::taaUsesWarmupBlend(true), "warmup blend on first frame");
    expectTrue(!fuse::renderer::taaUsesWarmupBlend(false), "no warmup blend after first frame");

    expectNear(fuse::renderer::computeHistoryBlendWeight(0.2f), 0.8f, 1e-5f,
               "history blend weight complements effective blend");
    expectNear(fuse::renderer::computeHistoryBlendWeight(true, params), 0.f, 1e-5f,
               "first frame history blend weight is zero");
    expectNear(fuse::renderer::computeHistoryBlendWeight(false, params), 0.8f, 1e-5f,
               "subsequent history blend weight uses clamped blend_factor");

    const fuse::f32 effective = fuse::renderer::computeEffectiveBlend(false, params);
    expectNear(fuse::renderer::computeHistoryBlendWeight(effective) + effective, 1.f, 1e-5f,
               "history and current blend weights sum to one");
}

void testHistoryReadGuards() {
    fuse::renderer::TaaHistoryBuffer emptyHistory;
    expectTrue(!fuse::renderer::canReadHistoryForResolve(emptyHistory),
               "empty history cannot be read for resolve");
    expectTrue(!fuse::renderer::historyAwaitingWarmup(emptyHistory),
               "unallocated history is not awaiting warmup");
    expectTrue(!emptyHistory.canReadForResolve(), "canReadForResolve false before init");

void testTaaPassBlendAndReuseGuards() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for history read guard test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaHistoryBuffer history;
    fuse::renderer::TaaHistoryBufferDesc historyDesc{64, 64};
    expectTrue(history.init(resources, historyDesc), "history ready for read guard test");
    expectTrue(fuse::renderer::historyAwaitingWarmup(history), "allocated history awaits warmup");
    expectTrue(!fuse::renderer::canReadHistoryForResolve(history), "cold history cannot be read");

    history.markResolved();
    expectTrue(fuse::renderer::canReadHistoryForResolve(history), "warm history can be read");
    expectTrue(!fuse::renderer::historyAwaitingWarmup(history), "warm history no longer awaits warmup");
    expectTrue(history.canReadForResolve(), "canReadForResolve true after first resolve");

    history.invalidateHistory();
    expectTrue(!fuse::renderer::canReadHistoryForResolve(history), "invalidated history cannot be read");
    expectTrue(fuse::renderer::historyAwaitingWarmup(history), "invalidated history awaits warmup again");

    history.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testResolveSurfaceGuards() {
    fuse::renderer::TaaResolveDesc desc{};
    desc.width = 64;
    desc.height = 64;
    desc.surfaces.current_frame = reinterpret_cast<void*>(0x1);
    desc.surfaces.output = reinterpret_cast<void*>(0x2);
    expectTrue(fuse::renderer::taaResolveSurfacesComplete(desc), "complete surfaces pass guard");
    expectTrue(fuse::renderer::taaResolveRejectionSurfacesComplete(desc),
               "rejection guard passes when enforcement disabled");

    desc.surfaces.output = nullptr;
    expectTrue(!fuse::renderer::taaResolveSurfacesComplete(desc), "missing output fails surface guard");

    desc.enforce_rejection_surfaces = true;
    desc.params.velocity_rejection = 0.5f;
    expectTrue(!fuse::renderer::taaResolveRejectionSurfacesComplete(desc),
               "missing velocity fails rejection guard when enforced");

    desc.surfaces.velocity_buffer = reinterpret_cast<void*>(0x3);
    desc.params.velocity_rejection = 0.f;
    desc.params.depth_rejection = 0.25f;
               "missing depth fails rejection guard when enforced");

    desc.surfaces.depth_buffer = reinterpret_cast<void*>(0x4);
               "complete rejection surfaces pass guard");

void testShouldSkipTaaResolve() {
    fuse::renderer::TaaHistoryBuffer emptyHistory;

    fuse::renderer::TaaResolveSkipReason skipReason = fuse::renderer::TaaResolveSkipReason::None;
    expectTrue(fuse::renderer::shouldSkipTaaResolve(desc, emptyHistory, &skipReason),
               "shouldSkipTaaResolve detects empty history");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::HistoryNotReady,
               "shouldSkipTaaResolve reports HistoryNotReady");

    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for shouldSkipTaaResolve test");



    expectTrue(history.init(resources, historyDesc), "history ready for shouldSkipTaaResolve test");

    desc.width = 128;
    skipReason = fuse::renderer::TaaResolveSkipReason::None;
    expectTrue(fuse::renderer::shouldSkipTaaResolve(desc, history, &skipReason),
               "shouldSkipTaaResolve detects dimension mismatch");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::DimensionMismatch,
               "shouldSkipTaaResolve reports DimensionMismatch");

    desc.surfaces.current_frame = nullptr;
               "shouldSkipTaaResolve detects missing surfaces");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::MissingSurfaces,
               "shouldSkipTaaResolve reports MissingSurfaces");

    expectTrue(!fuse::renderer::shouldSkipTaaResolve(desc, history), "shouldSkipTaaResolve passes valid desc");

    fuse::renderer::TaaResolve resolve;
    expectTrue(resolve.wouldSkip(desc, history, &skipReason) ==
                   fuse::renderer::shouldSkipTaaResolve(desc, history, &skipReason),
               "shouldSkipTaaResolve matches TaaResolve::wouldSkip");


void testTaaResolveSkipCounts() {
    fuse::renderer::TaaResolveSkipCounts noneCounts =
        fuse::renderer::taaResolveSkipCountsFromReason(fuse::renderer::TaaResolveSkipReason::None);
    expectTrue(noneCounts.total == 0u, "None reason produces zero skip counts");

    const fuse::renderer::TaaResolveSkipCounts staleCounts =
        fuse::renderer::taaResolveSkipCountsFromReason(
            fuse::renderer::TaaResolveSkipReason::StaleHistoryGeneration);
    expectTrue(staleCounts.total == 1u, "stale generation counted once");
    expectTrue(staleCounts.staleHistoryGeneration == 1u, "stale generation bucket incremented");

    fuse::renderer::TaaResolveSkipCounts accumulated{};
    fuse::renderer::accumulateTaaResolveSkipReason(accumulated,
                                                     fuse::renderer::TaaResolveSkipReason::MissingSurfaces);
                                                     fuse::renderer::TaaResolveSkipReason::DimensionMismatch);
    fuse::renderer::accumulateTaaResolveSkipReason(accumulated, fuse::renderer::TaaResolveSkipReason::None);
    expectTrue(accumulated.total == 2u, "accumulate counts blocking reasons only");
    expectTrue(accumulated.missingSurfaces == 1u, "missing surfaces bucket incremented");
    expectTrue(accumulated.dimensionMismatch == 1u, "dimension mismatch bucket incremented");

void testTaaPassEffectiveBlend() {
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass effective blend test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass blend/reuse guard test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaPassDesc passDesc{};
    passDesc.width = 64;
    passDesc.height = 64;
    passDesc.params.blend_factor = 0.25f;

    auto pass = fuse::renderer::TaaPass::create(passDesc);
    expectTrue(pass->init(resources), "TaaPass initialized for effective blend test");
    expectTrue(!pass->canReadHistory(), "pass cannot read history before first resolve");
    expectNear(pass->effectiveBlendForNextResolve(), 1.f, 1e-5f,
               "pass uses full current weight before warmup");

    fuse::renderer::TaaResolveDesc resolveDesc{};
    resolveDesc.width = 64;
    resolveDesc.height = 64;
    resolveDesc.surfaces.current_frame = reinterpret_cast<void*>(0x1);
    resolveDesc.surfaces.output = reinterpret_cast<void*>(0x2);
    expectTrue(pass->resolveFrame(resolveDesc), "initial resolve succeeds");
    expectTrue(pass->canReadHistory(), "pass can read history after first resolve");
    expectNear(pass->effectiveBlendForNextResolve(), 0.25f, 1e-5f,
               "pass uses configured blend after warmup");

    pass->invalidateHistory();
    expectTrue(!pass->canReadHistory(), "pass cannot read history after invalidate");
               "pass returns to full current weight after invalidate");
    expectTrue(pass->init(resources), "TaaPass initialized for blend/reuse guard test");

    fuse::renderer::TaaResolveDesc desc{};
    desc.width = 64;
    desc.height = 64;
    desc.surfaces.current_frame = reinterpret_cast<void*>(0x1);
    desc.surfaces.output = reinterpret_cast<void*>(0x2);
    desc.params = passDesc.params;

    expectNear(pass->effectiveBlendForNextResolve(desc), 1.f, 1e-5f,
               "cold history forces full current blend");
    expectTrue(!pass->canReuseHistory(desc), "cold history is not reusable");

    expectTrue(pass->resolveFrame(desc), "initial resolve warms pass history");
    expectTrue(pass->canReuseHistory(desc), "warmed pass history is reusable");
    expectNear(pass->effectiveBlendForNextResolve(desc), 0.25f, 1e-5f,
               "warmed reusable history uses configured blend");

    pass->destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testResolveSkipReasonLabels() {
    expectTrue(!fuse::renderer::taaResolveSkipReasonIsBlocking(fuse::renderer::TaaResolveSkipReason::None),
               "None skip reason is not blocking");
    expectTrue(fuse::renderer::taaResolveSkipReasonIsBlocking(
                   fuse::renderer::TaaResolveSkipReason::HistoryNotReady),
               "HistoryNotReady skip reason is blocking");

    expectTrue(std::strcmp(fuse::renderer::taaResolveSkipReasonLabel(fuse::renderer::TaaResolveSkipReason::None),
                           "none") == 0,
               "None skip reason label");
    expectTrue(std::strcmp(fuse::renderer::taaResolveSkipReasonLabel(
                               fuse::renderer::TaaResolveSkipReason::HistoryNotReady),
                           "history_not_ready") == 0,
               "HistoryNotReady skip reason label");
    expectTrue(std::strcmp(fuse::renderer::taaResolveSkipReasonLabel(
                               fuse::renderer::TaaResolveSkipReason::InvalidDimensions),
                           "invalid_dimensions") == 0,
               "InvalidDimensions skip reason label");
    expectTrue(std::strcmp(fuse::renderer::taaResolveSkipReasonLabel(
                               fuse::renderer::TaaResolveSkipReason::DimensionMismatch),
                           "dimension_mismatch") == 0,
               "DimensionMismatch skip reason label");
    expectTrue(std::strcmp(fuse::renderer::taaResolveSkipReasonLabel(
                               fuse::renderer::TaaResolveSkipReason::MissingSurfaces),
                           "missing_surfaces") == 0,
               "MissingSurfaces skip reason label");
    expectTrue(std::strcmp(fuse::renderer::taaResolveSkipReasonLabel(
                               fuse::renderer::TaaResolveSkipReason::MissingVelocityBuffer),
                           "missing_velocity_buffer") == 0,
               "MissingVelocityBuffer skip reason label");
    expectTrue(std::strcmp(fuse::renderer::taaResolveSkipReasonLabel(
                               fuse::renderer::TaaResolveSkipReason::MissingDepthBuffer),
                           "missing_depth_buffer") == 0,
               "MissingDepthBuffer skip reason label");
    expectTrue(std::strcmp(fuse::renderer::taaResolveSkipReasonLabel(
                               fuse::renderer::TaaResolveSkipReason::StaleHistoryGeneration),
                           "stale_history_generation") == 0,
               "StaleHistoryGeneration skip reason label");
}

void testHistoryBufferDescValid() {
    expectTrue(fuse::renderer::taaHistoryBufferDescValid({64u, 64u}), "non-zero history desc is valid");
    expectTrue(!fuse::renderer::taaHistoryBufferDescValid({0u, 64u}), "zero width history desc is invalid");
    expectTrue(!fuse::renderer::taaHistoryBufferDescValid({64u, 0u}), "zero height history desc is invalid");
    expectTrue(!fuse::renderer::taaHistoryBufferDescValid({0u, 0u}), "zero width and height history desc is invalid");

    expectTrue(!fuse::renderer::taaHistoryResizeNeeded(64u, 64u, 64u, 64u), "same dimensions do not need resize");
    expectTrue(fuse::renderer::taaHistoryResizeNeeded(64u, 64u, 128u, 64u), "width change needs resize");
    expectTrue(fuse::renderer::taaHistoryResizeNeeded(64u, 64u, 64u, 32u), "height change needs resize");
}

void testResolveDimensionMismatchHelper() {
    auto bootstrap = createTaaTestBootstrap();
    if (bootstrap == nullptr) {
        std::fprintf(stderr, "SKIP: bootstrap allocated for dimension mismatch helper test (Vulkan device unavailable)\n");
        return;
    }
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    TAA_SKIP_UNLESS_VULKAN_READY(bootstrap);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for dimension mismatch helper test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaHistoryBuffer history;
    fuse::renderer::TaaHistoryBufferDesc historyDesc{64, 64};
    expectTrue(history.init(resources, historyDesc), "history ready for dimension mismatch helper test");

    fuse::renderer::TaaResolveDesc desc{};
    desc.width = 128;
    desc.height = 64;
    expectTrue(fuse::renderer::taaResolveHasDimensionMismatch(desc, history),
               "dimension mismatch helper detects width mismatch");
    expectTrue(!fuse::renderer::taaResolveHasDimensionMismatch(
                   fuse::renderer::TaaResolveDesc{.width = 64, .height = 64}, history),
               "dimension mismatch helper passes matching dimensions");

    history.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testHistoryGenerationMatchHelpers() {
    auto bootstrap = createTaaTestBootstrap();
    if (bootstrap == nullptr) {
        std::fprintf(stderr, "SKIP: bootstrap allocated for generation match helper test (Vulkan device unavailable)\n");
        return;
    }
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    TAA_SKIP_UNLESS_VULKAN_READY(bootstrap);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for generation match helper test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaHistoryBuffer history;
    fuse::renderer::TaaHistoryBufferDesc historyDesc{64, 64};
    expectTrue(history.init(resources, historyDesc), "history ready for generation match helper test");
    expectTrue(history.generationMatches(0u), "generation zero matches after init");
    expectTrue(!history.isHistoryStale(0u), "generation zero is not stale after init");

    history.invalidateHistory();
    expectTrue(!history.generationMatches(0u), "generation zero no longer matches after invalidate");
    expectTrue(history.isHistoryStale(0u), "generation zero is stale after invalidate");
    expectTrue(history.generationMatches(history.invalidateGeneration()),
               "current generation always matches itself");

    fuse::renderer::TaaResolveDesc desc{};
    desc.observed_history_generation = 0u;
    expectTrue(fuse::renderer::taaResolveHistoryGenerationIsStale(desc, history),
               "stale generation helper detects invalidated epoch");
    desc.observed_history_generation = history.invalidateGeneration();
    expectTrue(!fuse::renderer::taaResolveHistoryGenerationIsStale(desc, history),
               "stale generation helper passes current epoch");
    desc.observed_history_generation = fuse::renderer::kTaaResolveNoHistoryGeneration;
    expectTrue(!fuse::renderer::taaResolveHistoryGenerationIsStale(desc, history),
               "stale generation helper bypasses no-guard sentinel");

    history.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testJitterViewportDimensions() {
    using fuse::renderer::TaaJitterLayout;

    expectTrue(TaaJitterLayout::validateViewportDimensions(1920u, 1080u), "non-zero viewport is valid");
    expectTrue(!TaaJitterLayout::validateViewportDimensions(0u, 1080u), "zero width viewport is invalid");
    expectTrue(!TaaJitterLayout::validateViewportDimensions(1920u, 0u), "zero height viewport is invalid");
}

void testTaaPassAutoStampResolveFrame() {
    auto bootstrap = createTaaTestBootstrap();
    if (bootstrap == nullptr) {
        std::fprintf(stderr, "SKIP: bootstrap allocated for auto-stamp resolve test (Vulkan device unavailable)\n");
        return;
    }
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    TAA_SKIP_UNLESS_VULKAN_READY(bootstrap);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for auto-stamp resolve test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaPassDesc passDesc{};
    passDesc.width = 64;
    passDesc.height = 64;

    auto pass = fuse::renderer::TaaPass::create(passDesc);
    expectTrue(pass->init(resources), "TaaPass initialized for auto-stamp resolve test");

    fuse::renderer::TaaResolveDesc desc{};
    desc.width = 64;
    desc.height = 64;
    desc.surfaces.current_frame = reinterpret_cast<void*>(0x1);
    desc.surfaces.output = reinterpret_cast<void*>(0x2);
    expectTrue(desc.observed_history_generation == fuse::renderer::kTaaResolveNoHistoryGeneration,
               "desc uses no-guard sentinel before resolveFrame");

    expectTrue(pass->resolveFrame(desc), "resolveFrame auto-stamps and succeeds");
    pass->invalidateHistory();
    expectTrue(pass->isHistoryStale(0u), "pass reports stale generation after invalidate");
    expectTrue(pass->historyInvalidateGeneration() == 1u, "invalidate bumps pass generation");

    desc.observed_history_generation = fuse::renderer::kTaaResolveNoHistoryGeneration;
    expectTrue(pass->resolveFrame(desc), "resolveFrame auto-stamps bumped generation after invalidate");

    pass->destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testResolveDimensionHelpers() {
    expectTrue(fuse::renderer::taaResolveDimensionsValid(64u, 64u), "non-zero dimensions are valid");
    expectTrue(!fuse::renderer::taaResolveDimensionsValid(0u, 64u), "zero width is invalid");
    expectTrue(!fuse::renderer::taaResolveDimensionsValid(64u, 0u), "zero height is invalid");
    expectTrue(!fuse::renderer::taaResolveDimensionsValid(0u, 0u), "zero width and height are invalid");

    auto bootstrap = createTaaTestBootstrap();
    if (bootstrap == nullptr) {
        std::fprintf(stderr, "SKIP: bootstrap allocated for dimension helper test (Vulkan device unavailable)\n");
        return;
    }
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    TAA_SKIP_UNLESS_VULKAN_READY(bootstrap);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for dimension helper test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaHistoryBuffer history;
    fuse::renderer::TaaHistoryBufferDesc historyDesc{64, 64};
    expectTrue(history.init(resources, historyDesc), "history ready for dimension helper test");

    fuse::renderer::TaaResolveDesc desc{};
    desc.width = 64;
    desc.height = 64;
    expectTrue(fuse::renderer::taaResolveDimensionsMatch(desc, history), "matching dimensions");
    expectTrue(!fuse::renderer::taaResolveDimensionsMatch(
                   fuse::renderer::TaaResolveDesc{.width = 128, .height = 64}, history),
               "width mismatch");
    expectTrue(!fuse::renderer::taaResolveDimensionsMatch(
                   fuse::renderer::TaaResolveDesc{.width = 64, .height = 32}, history),
               "height mismatch");

    history.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testClassifyTaaResolveSkipPriority() {
    fuse::renderer::TaaHistoryBuffer emptyHistory;
    fuse::renderer::TaaResolveDesc desc{};
    desc.width = 64;
    desc.height = 64;
    desc.surfaces.current_frame = reinterpret_cast<void*>(0x1);
    desc.surfaces.output = reinterpret_cast<void*>(0x2);

    expectTrue(fuse::renderer::classifyTaaResolveSkip(desc, emptyHistory) ==
                   fuse::renderer::TaaResolveSkipReason::HistoryNotReady,
               "HistoryNotReady wins over later checks");

    auto bootstrap = createTaaTestBootstrap();
    if (bootstrap == nullptr) {
        std::fprintf(stderr, "SKIP: bootstrap allocated for classify priority test (Vulkan device unavailable)\n");
        return;
    }
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    TAA_SKIP_UNLESS_VULKAN_READY(bootstrap);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for classify priority test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaHistoryBuffer history;
    fuse::renderer::TaaHistoryBufferDesc historyDesc{64, 64};
    expectTrue(history.init(resources, historyDesc), "history ready for classify priority test");

    desc.width = 0;
    desc.height = 64;
    expectTrue(fuse::renderer::classifyTaaResolveSkip(desc, history) ==
                   fuse::renderer::TaaResolveSkipReason::InvalidDimensions,
               "InvalidDimensions wins when history is ready");

    desc.width = 64;
    desc.height = 0;
    expectTrue(fuse::renderer::classifyTaaResolveSkip(desc, history) ==
                   fuse::renderer::TaaResolveSkipReason::InvalidDimensions,
               "zero height classified as InvalidDimensions");

    desc.width = 128;
    desc.height = 64;
    expectTrue(fuse::renderer::classifyTaaResolveSkip(desc, history) ==
                   fuse::renderer::TaaResolveSkipReason::DimensionMismatch,
               "DimensionMismatch wins over surface checks");

    desc.width = 64;
    desc.surfaces.current_frame = nullptr;
    expectTrue(fuse::renderer::classifyTaaResolveSkip(desc, history) ==
                   fuse::renderer::TaaResolveSkipReason::MissingSurfaces,
               "MissingSurfaces wins when dimensions match");

    history.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testRejectionSurfaceGuards() {
    fuse::renderer::TaaResolveDesc desc{};
    desc.params.velocity_rejection = 0.5f;
    desc.params.depth_rejection = 0.25f;
    desc.enforce_rejection_surfaces = false;
    expectTrue(!fuse::renderer::taaResolveRejectionSurfacesRequired(desc),
               "rejection surfaces not required when enforcement disabled");

    desc.enforce_rejection_surfaces = true;
    expectTrue(fuse::renderer::taaResolveRejectionSurfacesRequired(desc),
               "rejection surfaces required when enforcement enabled");
    expectTrue(!fuse::renderer::taaResolveRejectionSurfacesSatisfied(desc),
               "rejection surfaces unsatisfied without velocity/depth buffers");

    desc.surfaces.velocity_buffer = reinterpret_cast<void*>(0x1);
               "velocity alone does not satisfy depth rejection");
    desc.surfaces.depth_buffer = reinterpret_cast<void*>(0x2);
    expectTrue(fuse::renderer::taaResolveRejectionSurfacesSatisfied(desc),
               "velocity and depth satisfy rejection guards");

    desc.params.velocity_rejection = 0.f;
    desc.surfaces.velocity_buffer = nullptr;
               "depth-only rejection satisfied with depth buffer");
}

void testSanitizeAndPreflightResolve() {
void testHistoryGenerationCurrentHelpers() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    TAA_SKIP_UNLESS_VULKAN_READY(bootstrap);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for sanitize/preflight test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for generation current test");
    auto bootstrap = createTaaTestBootstrap();
    if (bootstrap == nullptr) {
        std::fprintf(stderr, "SKIP: bootstrap allocated for sanitize/preflight test (Vulkan device unavailable)\n");
        return;
    }

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaHistoryBuffer history;
    fuse::renderer::TaaHistoryBufferDesc historyDesc{64, 64};
    expectTrue(history.init(resources, historyDesc), "history ready for sanitize/preflight test");
    expectTrue(history.init(resources, historyDesc), "history ready for generation current test");
    expectTrue(fuse::renderer::taaHistoryCanAccumulate(history), "ready history can accumulate");
    expectTrue(history.isGenerationCurrent(0u), "generation zero is current after init");
    expectTrue(fuse::renderer::taaHistoryIsGenerationCurrent(history, 0u),
               "free helper reports current generation");
    expectTrue(!history.isHistoryStale(0u), "current generation is not stale");
    expectTrue(!history.isHistoryStale(0u) == history.isGenerationCurrent(0u),
               "stale and current are complementary");

    history.invalidateHistory();
    expectTrue(history.isHistoryStale(0u), "prior generation stale after invalidate");
    expectTrue(!history.isGenerationCurrent(0u), "prior generation not current after invalidate");
    expectTrue(history.isGenerationCurrent(history.invalidateGeneration()),
               "current generation matches after invalidate");
    expectTrue(fuse::renderer::taaHistoryIsGenerationCurrent(history, history.invalidateGeneration()),
               "free helper matches bumped generation");

    fuse::renderer::TaaHistoryBuffer emptyHistory;
    expectTrue(!fuse::renderer::taaHistoryCanAccumulate(emptyHistory), "empty history cannot accumulate");

    history.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testResolveSurfaceGuards() {
    fuse::renderer::TaaResolveDesc desc{};
    desc.width = 64;
    desc.height = 64;
    expectTrue(!fuse::renderer::taaResolveSurfacesSatisfied(desc), "null surfaces not satisfied");

    desc.surfaces.current_frame = reinterpret_cast<void*>(0x1);
    expectTrue(!fuse::renderer::taaResolveSurfacesSatisfied(desc), "missing output not satisfied");

    desc.surfaces.output = reinterpret_cast<void*>(0x2);
    expectTrue(fuse::renderer::taaResolveSurfacesSatisfied(desc), "both colour surfaces satisfied");

    desc.params.velocity_rejection = 0.5f;
    desc.params.depth_rejection = 0.f;
    desc.enforce_rejection_surfaces = true;
    expectTrue(!fuse::renderer::taaResolveRejectionSurfacesSatisfied(desc),
               "velocity rejection requires velocity buffer");

    desc.surfaces.velocity_buffer = reinterpret_cast<void*>(0x3);
    expectTrue(fuse::renderer::taaResolveRejectionSurfacesSatisfied(desc),
               "velocity buffer satisfies rejection guard");

    desc.params.velocity_rejection = 0.f;
    desc.params.depth_rejection = 0.25f;
    desc.surfaces.velocity_buffer = nullptr;
               "depth rejection requires depth buffer");

    desc.surfaces.depth_buffer = reinterpret_cast<void*>(0x4);
               "depth buffer satisfies rejection guard");

    desc.enforce_rejection_surfaces = false;
               "rejection guard bypassed when enforcement disabled");

void testCanAttemptAndPrepareResolve() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for canAttempt test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaHistoryBuffer history;
    fuse::renderer::TaaHistoryBufferDesc historyDesc{64, 64};
    expectTrue(history.init(resources, historyDesc), "history ready for canAttempt test");

    fuse::renderer::TaaResolveDesc desc{};
    desc.width = 64;
    desc.height = 64;
    desc.surfaces.current_frame = reinterpret_cast<void*>(0x1);
    desc.surfaces.output = reinterpret_cast<void*>(0x2);
    desc.params.blend_factor = 2.f;
    desc.observed_history_generation = fuse::renderer::kTaaResolveNoHistoryGeneration;

    fuse::renderer::sanitizeTaaResolveDesc(desc, history);
    expectNear(desc.params.blend_factor, 1.f, 1e-5f, "sanitize clamps resolve params");
    expectTrue(desc.observed_history_generation == 0u, "sanitize stamps observed generation");

    fuse::renderer::TaaResolveSkipReason skipReason = fuse::renderer::TaaResolveSkipReason::None;
    expectTrue(fuse::renderer::preflightTaaResolve(desc, history, &skipReason),
               "preflight passes sanitized resolve desc");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::None, "preflight skip reason is None");

    desc.width = 0;
    expectTrue(!fuse::renderer::preflightTaaResolve(desc, history, &skipReason),
               "preflight rejects invalid dimensions");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::InvalidDimensions,
               "preflight reports InvalidDimensions");

    history.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testHistoryGenerationGuardPasses() {
    auto bootstrap = createTaaTestBootstrap();
    if (bootstrap == nullptr) {
        std::fprintf(stderr, "SKIP: bootstrap allocated for generation guard passes test (Vulkan device unavailable)\n");
        return;
    }
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    TAA_SKIP_UNLESS_VULKAN_READY(bootstrap);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for generation guard passes test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaHistoryBuffer history;
    fuse::renderer::TaaHistoryBufferDesc historyDesc{64, 64};
    expectTrue(history.init(resources, historyDesc), "history ready for generation guard passes test");

    fuse::renderer::TaaResolveDesc desc{};
    expectTrue(fuse::renderer::taaResolveHistoryGenerationGuardPasses(desc, history),
               "sentinel bypasses generation guard");

    desc.observed_history_generation = 0u;
               "current generation passes guard");

    history.invalidateHistory();
    expectTrue(!fuse::renderer::taaResolveHistoryGenerationGuardPasses(desc, history),
               "stale generation fails guard");
    expectTrue(fuse::renderer::canAttemptTaaResolve(desc, history),
               "valid desc can attempt resolve with generation guard bypassed");

    expectTrue(!fuse::renderer::canAttemptTaaResolve(desc, history),
               "invalid dimensions block canAttempt");
    desc.width = 64;

               "current generation passes canAttempt");

               "stale generation blocks canAttempt");

    expectTrue(fuse::renderer::prepareTaaResolveDesc(desc, history),
               "prepare stamps generation and passes preflight");
    expectTrue(desc.observed_history_generation == history.invalidateGeneration(),
               "prepare fills observed generation from history");
               "prepared desc passes canAttempt");

    history.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testInvalidateHistoryIfStale() {
void testTaaPassPrepareAndCanResolve() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    TAA_SKIP_UNLESS_VULKAN_READY(bootstrap);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for invalidate-if-stale test");
    auto bootstrap = createTaaTestBootstrap();
    if (bootstrap == nullptr) {
        std::fprintf(stderr, "SKIP: bootstrap allocated for invalidate-if-stale test (Vulkan device unavailable)\n");
        return;
    }

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaHistoryBuffer history;
    fuse::renderer::TaaHistoryBufferDesc historyDesc{64, 64};
    expectTrue(history.init(resources, historyDesc), "history ready for invalidate-if-stale test");

    history.markResolved();
    expectTrue(history.hasValidHistory(), "history valid before invalidate-if-stale");

    expectTrue(!history.invalidateHistoryIfStale(0u), "current generation does not invalidate");
    expectTrue(history.hasValidHistory(), "validity preserved when generation matches");

    expectTrue(history.invalidateHistoryIfStale(99u), "stale observed generation triggers invalidate");
    expectTrue(!history.hasValidHistory(), "validity cleared by invalidate-if-stale");
    expectTrue(history.invalidateGeneration() == 1u, "invalidate-if-stale bumps generation");

    history.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testTaaPassSanitizeAndGenerationCurrent() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    TAA_SKIP_UNLESS_VULKAN_READY(bootstrap);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass sanitize test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass prepare test");
    auto bootstrap = createTaaTestBootstrap();
    if (bootstrap == nullptr) {
        std::fprintf(stderr, "SKIP: bootstrap allocated for pass sanitize test (Vulkan device unavailable)\n");
        return;
    }

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaPassDesc passDesc{};
    passDesc.width = 64;
    passDesc.height = 64;

    auto pass = fuse::renderer::TaaPass::create(passDesc);
    expectTrue(pass->init(resources), "TaaPass initialized for sanitize test");
    expectTrue(pass->isObservedHistoryGenerationCurrent(0u), "initial generation is current");
    expectTrue(pass->init(resources), "TaaPass initialized for prepare test");

    fuse::renderer::TaaResolveDesc desc{};
    desc.width = 64;
    desc.height = 64;
    desc.surfaces.current_frame = reinterpret_cast<void*>(0x1);
    desc.surfaces.output = reinterpret_cast<void*>(0x2);
    desc.params.blend_factor = -0.5f;

    pass->sanitizeResolveDesc(desc);
    expectNear(desc.params.blend_factor, 0.f, 1e-5f, "pass sanitize clamps params");
    expectTrue(desc.observed_history_generation == 0u, "pass sanitize stamps generation");
    expectTrue(pass->resolveFrame(desc), "resolve succeeds with sanitized desc");

    pass->invalidateHistory();
    expectTrue(!pass->isObservedHistoryGenerationCurrent(0u), "prior generation stale after invalidate");
    expectTrue(pass->isObservedHistoryGenerationCurrent(pass->historyInvalidateGeneration()),
               "current generation matches after invalidate");
    expectTrue(pass->canResolveFrame(desc), "pass canResolveFrame with bypassed generation guard");

    desc.observed_history_generation = 0u;
    expectTrue(pass->prepareAndCanResolve(desc), "pass prepareAndCanResolve stamps and passes");
    expectTrue(desc.observed_history_generation == pass->historyInvalidateGeneration(),
               "pass prepare stamps current generation");

    expectTrue(!pass->canResolveFrame(desc), "pass canResolveFrame blocks stale generation");
    desc.observed_history_generation = fuse::renderer::kTaaResolveNoHistoryGeneration;
    expectTrue(pass->prepareAndCanResolve(desc), "pass prepareAndCanResolve recovers after invalidate");

    pass->destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testHistoryGenerationGuardBypass() {
    auto bootstrap = createTaaTestBootstrap();
    if (bootstrap == nullptr) {
        std::fprintf(stderr, "SKIP: bootstrap allocated for generation guard bypass test (Vulkan device unavailable)\n");
        return;
    }
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    TAA_SKIP_UNLESS_VULKAN_READY(bootstrap);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for generation guard bypass test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaHistoryBuffer history;
    fuse::renderer::TaaHistoryBufferDesc historyDesc{64, 64};
    expectTrue(history.init(resources, historyDesc), "history ready for generation guard bypass test");

    fuse::renderer::TaaResolveDesc desc{};
    desc.width = 64;
    desc.height = 64;
    desc.surfaces.current_frame = reinterpret_cast<void*>(0x1);
    desc.surfaces.output = reinterpret_cast<void*>(0x2);
    desc.observed_history_generation = fuse::renderer::kTaaResolveNoHistoryGeneration;

    expectTrue(fuse::renderer::taaResolveBypassesHistoryGenerationGuard(desc),
               "sentinel bypasses history generation guard");

    fuse::renderer::TaaResolve resolve;
    expectTrue(resolve.resolve(desc, history), "resolve succeeds with generation guard bypassed");
    history.invalidateHistory();

    desc.observed_history_generation = 0u;
    expectTrue(!fuse::renderer::taaResolveBypassesHistoryGenerationGuard(desc),
               "explicit generation enables guard");
    expectTrue(fuse::renderer::classifyTaaResolveSkip(desc, history) ==
                   fuse::renderer::TaaResolveSkipReason::StaleHistoryGeneration,
               "stale generation classified when guard enabled");
    expectTrue(!resolve.resolve(desc, history), "resolve rejects stale generation when guard enabled");

    history.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testStampObservedHistoryGeneration() {
    auto bootstrap = createTaaTestBootstrap();
    if (bootstrap == nullptr) {
        std::fprintf(stderr, "SKIP: bootstrap allocated for stamp generation test (Vulkan device unavailable)\n");
        return;
    }
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    TAA_SKIP_UNLESS_VULKAN_READY(bootstrap);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for stamp generation test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaPassDesc passDesc{};
    passDesc.width = 64;
    passDesc.height = 64;

    auto pass = fuse::renderer::TaaPass::create(passDesc);
    expectTrue(pass->init(resources), "TaaPass initialized for stamp generation test");
    expectTrue(pass->historyInvalidateGeneration() == 0u, "initial invalidate generation is zero");

    fuse::renderer::TaaResolveDesc desc{};
    desc.width = 64;
    desc.height = 64;
    desc.surfaces.current_frame = reinterpret_cast<void*>(0x1);
    desc.surfaces.output = reinterpret_cast<void*>(0x2);
    expectTrue(desc.observed_history_generation == fuse::renderer::kTaaResolveNoHistoryGeneration,
               "desc starts with no-guard sentinel");

    pass->stampObservedHistoryGeneration(desc);
    expectTrue(desc.observed_history_generation == 0u, "stamp fills current invalidate generation");
    expectTrue(!fuse::renderer::taaResolveBypassesHistoryGenerationGuard(desc),
               "stamp enables generation guard");

    expectTrue(pass->resolveFrame(desc), "resolve succeeds with stamped generation");
    pass->invalidateHistory();
    expectTrue(pass->historyInvalidateGeneration() == 1u, "invalidate bumps pass generation");

    desc.observed_history_generation = 0u;
    fuse::renderer::TaaResolveSkipReason skipReason = fuse::renderer::TaaResolveSkipReason::None;
    expectTrue(pass->wouldSkipResolve(desc, &skipReason), "stamped generation becomes stale after invalidate");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::StaleHistoryGeneration,
               "stale generation after invalidate");

    desc.observed_history_generation = fuse::renderer::kTaaResolveNoHistoryGeneration;
    pass->stampObservedHistoryGeneration(desc);
    expectTrue(desc.observed_history_generation == 1u, "restamp picks up bumped generation");
    expectTrue(!pass->wouldSkipResolve(desc), "restamped generation passes preflight");

    pass->destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testInvalidDimensionsSkipReason() {
    auto bootstrap = createTaaTestBootstrap();
    if (bootstrap == nullptr) {
        std::fprintf(stderr, "SKIP: bootstrap allocated for invalid dimensions test (Vulkan device unavailable)\n");
        return;
    }
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    TAA_SKIP_UNLESS_VULKAN_READY(bootstrap);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for invalid dimensions test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaHistoryBuffer history;
    fuse::renderer::TaaHistoryBufferDesc historyDesc{64, 64};
    expectTrue(history.init(resources, historyDesc), "history ready for invalid dimensions test");

    fuse::renderer::TaaResolve resolve;
    fuse::renderer::TaaResolveDesc desc{};
    desc.width = 0;
    desc.height = 64;
    desc.surfaces.current_frame = reinterpret_cast<void*>(0x1);
    desc.surfaces.output = reinterpret_cast<void*>(0x2);

    fuse::renderer::TaaResolveSkipReason skipReason = fuse::renderer::TaaResolveSkipReason::None;
    expectTrue(resolve.wouldSkip(desc, history, &skipReason), "wouldSkip detects zero width");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::InvalidDimensions,
               "zero width skip reason is InvalidDimensions");
    expectTrue(!resolve.resolve(desc, history), "resolve rejects zero width");
    expectTrue(resolve.lastStats().skip_reason == fuse::renderer::TaaResolveSkipReason::InvalidDimensions,
               "resolve records InvalidDimensions skip reason");

    history.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testDimensionMismatchResolve() {
    auto bootstrap = createTaaTestBootstrap();
    if (bootstrap == nullptr) {
        std::fprintf(stderr, "SKIP: bootstrap allocated for dimension mismatch test (Vulkan device unavailable)\n");
        return;
    }
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    TAA_SKIP_UNLESS_VULKAN_READY(bootstrap);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for dimension mismatch test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaHistoryBuffer history;
    fuse::renderer::TaaHistoryBufferDesc historyDesc{64, 64};
    expectTrue(history.init(resources, historyDesc), "history ready for dimension mismatch test");
    expectTrue(history.matchesDimensions(64u, 64u), "history matches its own dimensions");
    expectTrue(!history.matchesDimensions(128u, 64u), "history rejects mismatched width");

    fuse::renderer::TaaResolve resolve;
    fuse::renderer::TaaResolveDesc desc{};
    desc.width = 128;
    desc.height = 64;
    desc.surfaces.current_frame = reinterpret_cast<void*>(0x1);
    desc.surfaces.output = reinterpret_cast<void*>(0x2);

    fuse::renderer::TaaResolveSkipReason skipReason = fuse::renderer::TaaResolveSkipReason::None;
    expectTrue(resolve.wouldSkip(desc, history, &skipReason), "wouldSkip detects dimension mismatch");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::DimensionMismatch,
               "dimension mismatch skip reason");
    expectTrue(!resolve.resolve(desc, history), "resolve rejects dimension mismatch");
    expectTrue(resolve.lastStats().skip_reason == fuse::renderer::TaaResolveSkipReason::DimensionMismatch,
               "resolve records DimensionMismatch skip reason");

    history.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testMissingSurfacesSkipReason() {
    auto bootstrap = createTaaTestBootstrap();
    if (bootstrap == nullptr) {
        std::fprintf(stderr, "SKIP: bootstrap allocated for missing surfaces test (Vulkan device unavailable)\n");
        return;
    }
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    TAA_SKIP_UNLESS_VULKAN_READY(bootstrap);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for missing surfaces test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaHistoryBuffer history;
    fuse::renderer::TaaHistoryBufferDesc historyDesc{64, 64};
    expectTrue(history.init(resources, historyDesc), "history ready for missing surfaces test");

    fuse::renderer::TaaResolve resolve;
    fuse::renderer::TaaResolveDesc desc{};
    desc.width = 64;
    desc.height = 64;

    fuse::renderer::TaaResolveSkipReason skipReason = fuse::renderer::TaaResolveSkipReason::None;
    expectTrue(resolve.wouldSkip(desc, history, &skipReason), "wouldSkip detects missing surfaces");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::MissingSurfaces,
               "missing surfaces skip reason");
    expectTrue(!resolve.resolve(desc, history), "resolve rejects missing surfaces");
    expectTrue(resolve.lastStats().skip_reason == fuse::renderer::TaaResolveSkipReason::MissingSurfaces,
               "resolve records MissingSurfaces skip reason");

    history.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testMissingVelocityDepthSkipReason() {
    auto bootstrap = createTaaTestBootstrap();
    if (bootstrap == nullptr) {
        std::fprintf(stderr, "SKIP: bootstrap allocated for velocity/depth skip test (Vulkan device unavailable)\n");
        return;
    }
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    TAA_SKIP_UNLESS_VULKAN_READY(bootstrap);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for velocity/depth skip test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaHistoryBuffer history;
    fuse::renderer::TaaHistoryBufferDesc historyDesc{64, 64};
    expectTrue(history.init(resources, historyDesc), "history ready for velocity/depth skip test");

    fuse::renderer::TaaResolve resolve;
    fuse::renderer::TaaResolveDesc desc{};
    desc.width = 64;
    desc.height = 64;
    desc.surfaces.current_frame = reinterpret_cast<void*>(0x1);
    desc.surfaces.output = reinterpret_cast<void*>(0x2);
    desc.params.velocity_rejection = 0.5f;
    desc.enforce_rejection_surfaces = true;

    fuse::renderer::TaaResolveSkipReason skipReason = fuse::renderer::TaaResolveSkipReason::None;
    expectTrue(resolve.wouldSkip(desc, history, &skipReason), "wouldSkip detects missing velocity buffer");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::MissingVelocityBuffer,
               "missing velocity buffer skip reason");
    expectTrue(!resolve.resolve(desc, history), "resolve rejects missing velocity buffer");
    expectTrue(resolve.lastStats().skip_reason == fuse::renderer::TaaResolveSkipReason::MissingVelocityBuffer,
               "resolve records MissingVelocityBuffer skip reason");

    desc.params.velocity_rejection = 0.f;
    desc.params.depth_rejection = 0.25f;
    skipReason = fuse::renderer::TaaResolveSkipReason::None;
    expectTrue(resolve.wouldSkip(desc, history, &skipReason), "wouldSkip detects missing depth buffer");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::MissingDepthBuffer,
               "missing depth buffer skip reason");

    desc.params.depth_rejection = 0.f;
    expectTrue(!resolve.wouldSkip(desc, history, &skipReason), "wouldSkip passes when rejection disabled");

    history.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testStaleHistoryGenerationSkipReason() {
    auto bootstrap = createTaaTestBootstrap();
    if (bootstrap == nullptr) {
        std::fprintf(stderr, "SKIP: bootstrap allocated for stale generation test (Vulkan device unavailable)\n");
        return;
    }
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    TAA_SKIP_UNLESS_VULKAN_READY(bootstrap);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for stale generation test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaHistoryBuffer history;
    fuse::renderer::TaaHistoryBufferDesc historyDesc{64, 64};
    expectTrue(history.init(resources, historyDesc), "history ready for stale generation test");
    expectTrue(!history.isHistoryStale(0u), "generation zero is current after init");

    fuse::renderer::TaaResolve resolve;
    fuse::renderer::TaaResolveDesc desc{};
    desc.width = 64;
    desc.height = 64;
    desc.surfaces.current_frame = reinterpret_cast<void*>(0x1);
    desc.surfaces.output = reinterpret_cast<void*>(0x2);
    desc.observed_history_generation = 0u;

    expectTrue(resolve.resolve(desc, history), "resolve succeeds with current generation");
    history.invalidateHistory();
    expectTrue(history.isHistoryStale(0u), "invalidate marks prior generation stale");

    desc.observed_history_generation = 0u;
    fuse::renderer::TaaResolveSkipReason skipReason = fuse::renderer::TaaResolveSkipReason::None;
    expectTrue(resolve.wouldSkip(desc, history, &skipReason), "wouldSkip detects stale generation");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::StaleHistoryGeneration,
               "stale generation skip reason");
    expectTrue(!resolve.resolve(desc, history), "resolve rejects stale generation");
    expectTrue(resolve.lastStats().skip_reason == fuse::renderer::TaaResolveSkipReason::StaleHistoryGeneration,
               "resolve records StaleHistoryGeneration skip reason");

    desc.observed_history_generation = history.invalidateGeneration();
    expectTrue(!resolve.wouldSkip(desc, history, &skipReason), "wouldSkip passes with updated generation");
    expectTrue(resolve.resolve(desc, history), "resolve succeeds with updated generation");

    history.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testHistoryInvalidateGeneration() {
    auto bootstrap = createTaaTestBootstrap();
    if (bootstrap == nullptr) {
        std::fprintf(stderr, "SKIP: bootstrap allocated for invalidate generation test (Vulkan device unavailable)\n");
        return;
    }
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    TAA_SKIP_UNLESS_VULKAN_READY(bootstrap);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for invalidate generation test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaHistoryBuffer history;
    fuse::renderer::TaaHistoryBufferDesc historyDesc{64, 64};
    expectTrue(history.init(resources, historyDesc), "history ready for invalidate generation test");
    expectTrue(history.invalidateGeneration() == 0u, "invalidate generation starts at zero");

    fuse::renderer::TaaResolve resolve;
    fuse::renderer::TaaResolveDesc desc{};
    desc.width = 64;
    desc.height = 64;
    desc.surfaces.current_frame = reinterpret_cast<void*>(0x1);
    desc.surfaces.output = reinterpret_cast<void*>(0x2);
    expectTrue(resolve.resolve(desc, history), "initial resolve succeeds");
    expectTrue(resolve.lastStats().history_invalidate_generation == 0u,
               "resolve records invalidate generation at resolve time");

    history.invalidateHistory();
    expectTrue(history.invalidateGeneration() == 1u, "invalidate bumps generation");
    expectTrue(resolve.resolve(desc, history), "resolve succeeds after invalidate");
    expectTrue(resolve.lastStats().history_invalidate_generation == 1u,
               "resolve records bumped invalidate generation");

    history.resize(128, 128);
    expectTrue(history.invalidateGeneration() == 2u, "resize bumps invalidate generation");

    history.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testEmptyHistoryResolve() {
    fuse::renderer::TaaHistoryBuffer history;
    expectTrue(!history.isReady(), "default history buffer is not ready");
    expectTrue(!history.hasValidHistory(), "default history buffer has no valid history");
    expectTrue(history.accumulatedFrames() == 0u, "default history buffer has zero accumulated frames");

    fuse::renderer::TaaResolve resolve;
    fuse::renderer::TaaResolveDesc desc{};
    desc.width = 64;
    desc.height = 64;
    desc.surfaces.current_frame = reinterpret_cast<void*>(0x1);
    desc.surfaces.output = reinterpret_cast<void*>(0x2);
    fuse::renderer::TaaResolveSkipReason skipReason = fuse::renderer::TaaResolveSkipReason::None;
    expectTrue(resolve.wouldSkip(desc, history, &skipReason), "wouldSkip detects empty history buffer");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::HistoryNotReady,
               "empty history skip reason is HistoryNotReady");

    expectTrue(!resolve.resolve(desc, history), "resolve rejects empty history buffer");
    expectTrue(!resolve.lastStats().resolved, "empty history resolve stats not marked resolved");
    expectTrue(resolve.lastStats().skipped, "empty history resolve stats marked skipped");
    expectTrue(resolve.lastStats().skip_reason == fuse::renderer::TaaResolveSkipReason::HistoryNotReady,
               "empty history resolve records skip reason");
}

void testValidityResetAfterInvalidate() {
    auto bootstrap = createTaaTestBootstrap();
    if (bootstrap == nullptr) {
        std::fprintf(stderr, "SKIP: bootstrap allocated for validity reset test (Vulkan device unavailable)\n");
        return;
    }
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    TAA_SKIP_UNLESS_VULKAN_READY(bootstrap);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for validity reset test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaHistoryBuffer history;
    fuse::renderer::TaaHistoryBufferDesc historyDesc{64, 64};
    expectTrue(history.init(resources, historyDesc), "history ready for validity reset");

    fuse::renderer::TaaResolve resolve;
    fuse::renderer::TaaResolveDesc desc{};
    desc.width = 64;
    desc.height = 64;
    desc.surfaces.current_frame = reinterpret_cast<void*>(0x1);
    desc.surfaces.output = reinterpret_cast<void*>(0x2);

    expectTrue(resolve.resolve(desc, history), "initial resolve succeeds");
    expectTrue(resolve.lastStats().first_frame, "initial resolve marks first frame");
    expectTrue(history.hasValidHistory(), "history valid after initial resolve");

    history.invalidateHistory();
    expectTrue(!history.hasValidHistory(), "invalidate clears history validity");
    expectTrue(history.accumulatedFrames() == 0u, "invalidate clears accumulated frames");

    resolve.resetBookkeeping();
    expectTrue(resolve.resolve(desc, history), "resolve succeeds after validity reset");
    expectTrue(resolve.lastStats().first_frame, "first frame flagged after validity reset");
    expectTrue(resolve.lastStats().accumulated_frames == 1u, "accumulated frames restart after reset");

    history.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testResolveStub() {
    auto bootstrap = createTaaTestBootstrap();
    if (bootstrap == nullptr) {
        std::fprintf(stderr, "SKIP: bootstrap allocated for TAA resolve test (Vulkan device unavailable)\n");
        return;
    }
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    TAA_SKIP_UNLESS_VULKAN_READY(bootstrap);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for TAA resolve test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaHistoryBuffer history;
    fuse::renderer::TaaHistoryBufferDesc historyDesc{64, 64};
    expectTrue(history.init(resources, historyDesc), "history ready for resolve");

    fuse::renderer::TaaResolve resolve;
    fuse::renderer::TaaResolveDesc desc{};
    desc.width = 64;
    desc.height = 64;
    desc.surfaces.current_frame = reinterpret_cast<void*>(0x1);
    desc.surfaces.output = reinterpret_cast<void*>(0x2);
    desc.params.blend_factor = 0.15f;

    expectTrue(resolve.resolve(desc, history), "resolve stub succeeds with valid surfaces");
    expectTrue(resolve.lastStats().resolved, "resolve stats marked resolved");
    expectTrue(resolve.lastStats().history_swapped, "resolve swaps history");
    expectTrue(resolve.lastStats().first_frame, "first resolve marks first frame");
    expectTrue(resolve.lastStats().has_valid_history, "resolve marks history valid");
    expectTrue(resolve.lastStats().accumulated_frames == 1u, "resolve increments accumulated frames");
    expectNear(resolve.lastStats().last_blend, 0.15f, 1e-5f, "resolve records blend factor");
    expectNear(resolve.lastStats().effective_blend, 1.f, 1e-5f, "first resolve uses full current weight");

    desc.surfaces.current_frame = reinterpret_cast<void*>(0x3);
    desc.surfaces.output = reinterpret_cast<void*>(0x4);
    expectTrue(resolve.resolve(desc, history), "second resolve succeeds");
    expectTrue(!resolve.lastStats().first_frame, "second resolve is not first frame");
    expectTrue(resolve.lastStats().accumulated_frames == 2u, "second resolve increments accumulated frames");
    expectNear(resolve.lastStats().effective_blend, 0.15f, 1e-5f, "subsequent resolve uses configured blend");

    fuse::renderer::TaaResolveDesc invalid{};
    invalid.width = 64;
    invalid.height = 64;
    expectTrue(!resolve.resolve(invalid, history), "resolve rejects missing surfaces");

    history.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testTaaPassLifecycle() {
    auto bootstrap = createTaaTestBootstrap();
    if (bootstrap == nullptr) {
        std::fprintf(stderr, "SKIP: bootstrap allocated for TaaPass test (Vulkan device unavailable)\n");
        return;
    }
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    TAA_SKIP_UNLESS_VULKAN_READY(bootstrap);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for TaaPass test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaPassDesc passDesc{};
    passDesc.width = 128;
    passDesc.height = 128;

    auto pass = fuse::renderer::TaaPass::create(passDesc);
    expectTrue(pass->init(resources), "TaaPass initialized");
    expectTrue(pass->isReady(), "TaaPass ready after init");
    expectTrue(pass->history().isReady(), "TaaPass owns history buffers");

    const fuse::math::Vec2 jitterBefore = pass->currentJitterNdc();
    pass->advanceJitter();
    const fuse::math::Vec2 jitterAfter = pass->currentJitterNdc();
    expectTrue(jitterBefore.x != jitterAfter.x || jitterBefore.y != jitterAfter.y,
               "advanceJitter changes NDC offset");

    fuse::renderer::TaaResolveDesc resolveDesc{};
    resolveDesc.width = 128;
    resolveDesc.height = 128;
    resolveDesc.surfaces.current_frame = reinterpret_cast<void*>(0x10);
    resolveDesc.surfaces.output = reinterpret_cast<void*>(0x20);
    expectTrue(pass->resolveFrame(resolveDesc), "TaaPass resolveFrame succeeds");
    expectTrue(pass->lastStats().framesResolved == 1u, "TaaPass counts resolved frames");

    pass->destroy();
    expectTrue(!pass->isReady(), "TaaPass not ready after destroy");

    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testTaaPassInvalidateHistory() {
    auto bootstrap = createTaaTestBootstrap();
    if (bootstrap == nullptr) {
        std::fprintf(stderr, "SKIP: bootstrap allocated for TaaPass invalidate test (Vulkan device unavailable)\n");
        return;
    }
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    TAA_SKIP_UNLESS_VULKAN_READY(bootstrap);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for TaaPass invalidate test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaPassDesc passDesc{};
    passDesc.width = 64;
    passDesc.height = 64;

    auto pass = fuse::renderer::TaaPass::create(passDesc);
    expectTrue(pass->init(resources), "TaaPass initialized for invalidate test");

    fuse::renderer::TaaResolveDesc resolveDesc{};
    resolveDesc.width = 64;
    resolveDesc.height = 64;
    resolveDesc.surfaces.current_frame = reinterpret_cast<void*>(0x10);
    resolveDesc.surfaces.output = reinterpret_cast<void*>(0x20);
    expectTrue(pass->resolveFrame(resolveDesc), "initial resolve succeeds");
    expectTrue(pass->history().hasValidHistory(), "history valid before invalidate");

    pass->invalidateHistory();
    expectTrue(!pass->history().hasValidHistory(), "TaaPass invalidate clears history validity");
    expectTrue(pass->history().accumulatedFrames() == 0u, "TaaPass invalidate clears accumulated frames");
    expectTrue(!pass->resolve().lastStats().resolved, "TaaPass invalidate resets resolve bookkeeping");

    expectTrue(pass->resolveFrame(resolveDesc), "resolve succeeds after TaaPass invalidate");
    expectTrue(pass->resolve().lastStats().first_frame, "first frame flagged after TaaPass invalidate");
    expectTrue(pass->resolve().lastStats().accumulated_frames == 1u,
               "accumulated frames restart after TaaPass invalidate");

    pass->destroy();
    expectTrue(!pass->history().isReady(), "TaaPass destroy releases history buffers");

    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testTaaPassResize() {
    auto bootstrap = createTaaTestBootstrap();
    if (bootstrap == nullptr) {
        std::fprintf(stderr, "SKIP: bootstrap allocated for pass resize test (Vulkan device unavailable)\n");
        return;
    }
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    TAA_SKIP_UNLESS_VULKAN_READY(bootstrap);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass resize test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaPassDesc passDesc{};
    passDesc.width = 64;
    passDesc.height = 64;

    auto pass = fuse::renderer::TaaPass::create(passDesc);
    expectTrue(pass->init(resources), "TaaPass initialized for resize test");
    expectTrue(pass->matchesDimensions(64u, 64u), "pass matches initial dimensions");

    fuse::renderer::TaaResolveDesc resolveDesc{};
    resolveDesc.width = 64;
    resolveDesc.height = 64;
    resolveDesc.surfaces.current_frame = reinterpret_cast<void*>(0x10);
    resolveDesc.surfaces.output = reinterpret_cast<void*>(0x20);
    expectTrue(pass->resolveFrame(resolveDesc), "initial resolve succeeds");
    expectTrue(pass->history().hasValidHistory(), "history valid before resize");

    pass->resize(128, 128);
    expectTrue(pass->matchesDimensions(128u, 128u), "pass matches resized dimensions");
    expectTrue(!pass->history().hasValidHistory(), "resize invalidates history validity");
    expectTrue(pass->needsHistoryWarmup(), "pass needs warmup after resize");

    resolveDesc.width = 128;
    resolveDesc.height = 128;
    expectTrue(!pass->wouldSkipResolve(resolveDesc), "wouldSkipResolve passes at new dimensions");
    expectTrue(pass->resolveFrame(resolveDesc), "resolve succeeds after resize");

    pass->destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testTaaPassWouldSkipResolve() {
    auto bootstrap = createTaaTestBootstrap();
    if (bootstrap == nullptr) {
        std::fprintf(stderr, "SKIP: bootstrap allocated for pass wouldSkip test (Vulkan device unavailable)\n");
        return;
    }
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    TAA_SKIP_UNLESS_VULKAN_READY(bootstrap);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass wouldSkip test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaPassDesc passDesc{};
    passDesc.width = 64;
    passDesc.height = 64;

    auto pass = fuse::renderer::TaaPass::create(passDesc);
    expectTrue(pass->init(resources), "TaaPass initialized for wouldSkip test");
    expectTrue(pass->needsHistoryWarmup(), "TaaPass needs warmup before first resolve");

    fuse::renderer::TaaResolveDesc resolveDesc{};
    resolveDesc.width = 32;
    resolveDesc.height = 64;
    resolveDesc.surfaces.current_frame = reinterpret_cast<void*>(0x10);
    resolveDesc.surfaces.output = reinterpret_cast<void*>(0x20);

    fuse::renderer::TaaResolveSkipReason skipReason = fuse::renderer::TaaResolveSkipReason::None;
    expectTrue(pass->wouldSkipResolve(resolveDesc, &skipReason), "TaaPass wouldSkipResolve detects mismatch");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::DimensionMismatch,
               "TaaPass wouldSkipResolve reports DimensionMismatch");

    resolveDesc.width = 64;
    expectTrue(!pass->wouldSkipResolve(resolveDesc, &skipReason), "TaaPass wouldSkipResolve passes valid desc");
    expectTrue(pass->resolveFrame(resolveDesc), "TaaPass resolveFrame succeeds");
    expectTrue(!pass->needsHistoryWarmup(), "TaaPass no longer needs warmup after resolve");

    pass->destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testTaaPassSyncJitterToFrameIndex() {
    fuse::renderer::TaaPassDesc passDesc{};
    passDesc.width = 128;
    passDesc.height = 128;

    auto pass = fuse::renderer::TaaPass::create(passDesc);
    pass->syncJitterToFrameIndex(3u);
    const fuse::math::Vec2 synced = pass->currentJitterNdc();
    const fuse::math::Vec2 expected =
        fuse::renderer::TaaJitterLayout::ndcOffsetForFrameIndex(3u, 128u, 128u, 8u);
    expectNear(synced.x, expected.x, 1e-6f, "TaaPass syncJitterToFrameIndex matches layout NDC offset");
    expectNear(synced.y, expected.y, 1e-6f, "TaaPass syncJitterToFrameIndex Y matches layout NDC offset");
    expectTrue(pass->jitter().monotonicFrameIndex() == 3u, "TaaPass syncJitterToFrameIndex sets monotonic counter");
}

void testTaaPassGraphHook() {
    fuse::renderer::RenderGraph graph;
    fuse::renderer::resetTaaPassGraphStorage();
    fuse::renderer::addTaaPassToGraph(graph);
    graph.compile();
    expectTrue(graph.compileInfo().passCount == 1u, "TAA pass registered on render graph");
    expectTrue(graph.compileInfo().compiled, "TAA render graph compiles");
}

void testBlendWeightGuards() {
    fuse::renderer::TAAParams params{};
    params.blend_factor = 0.2f;

    const fuse::renderer::TaaBlendWeights warmup =
        fuse::renderer::computeTaaBlendWeights(true, params);
    expectNear(warmup.current, 1.f, 1e-5f, "warmup blend uses full current weight");
    expectNear(warmup.history, 0.f, 1e-5f, "warmup blend uses zero history weight");
    expectTrue(fuse::renderer::taaBlendWeightsValid(warmup), "warmup blend weights are valid");

    const fuse::renderer::TaaBlendWeights steady =
        fuse::renderer::computeTaaBlendWeights(false, params);
    expectNear(steady.current, 0.2f, 1e-5f, "steady blend uses configured current weight");
    expectNear(steady.history, 0.8f, 1e-5f, "steady blend uses history complement");
    expectTrue(fuse::renderer::taaBlendWeightsValid(steady), "steady blend weights are valid");

    fuse::renderer::TaaBlendWeights invalid{0.6f, 0.6f};
    expectTrue(!fuse::renderer::taaBlendWeightsValid(invalid), "weights that do not sum to one are invalid");

    expectTrue(fuse::renderer::isTaaBlendFactorInRange(0.f), "zero blend factor in range");
    expectTrue(fuse::renderer::isTaaBlendFactorInRange(1.f), "unit blend factor in range");
    expectTrue(!fuse::renderer::isTaaBlendFactorInRange(-0.1f), "negative blend factor out of range");
    expectTrue(!fuse::renderer::isTaaBlendFactorInRange(1.1f), "blend factor above one out of range");

    expectTrue(fuse::renderer::taaUsesWarmupBlend(true), "first frame uses warmup blend");
    expectTrue(!fuse::renderer::taaUsesWarmupBlend(false), "subsequent frames do not use warmup blend");

    expectNear(fuse::renderer::computeEffectiveBlend(false, true, params), 0.2f, 1e-5f,
               "reusable history uses configured blend");
    expectNear(fuse::renderer::computeEffectiveBlend(false, false, params), 1.f, 1e-5f,
               "non-reusable history forces full current weight");
    expectTrue(fuse::renderer::taaBlendUsesHistory(0.2f), "partial blend uses history");
    expectTrue(!fuse::renderer::taaBlendUsesHistory(1.f), "full current blend does not use history");
    expectTrue(fuse::renderer::taaBlendSkipsHistoryReuse(1.f), "full current blend skips history reuse");

    const fuse::renderer::TaaBlendWeights guarded =
        fuse::renderer::computeTaaBlendWeightsWithReuseGuard(false, false, params);
    expectNear(guarded.current, 1.f, 1e-5f, "reuse guard forces full current when history not reusable");
    expectTrue(fuse::renderer::taaBlendWeightsValid(guarded), "guarded blend weights are valid");
}

void testHistoryReuseGuards() {
    auto bootstrap = createTaaTestBootstrap();
    if (bootstrap == nullptr) {
        std::fprintf(stderr, "SKIP: bootstrap allocated for history reuse guard test (Vulkan device unavailable)\n");
        return;
    }
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    TAA_SKIP_UNLESS_VULKAN_READY(bootstrap);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for history reuse guard test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaHistoryBuffer history;
    fuse::renderer::TaaHistoryBufferDesc historyDesc{64, 64};
    expectTrue(history.init(resources, historyDesc), "history ready for reuse guard test");
    expectTrue(!fuse::renderer::taaHistoryCanReuse(history), "unwarmed history cannot be reused");
    expectTrue(!history.canReuseHistory(), "canReuseHistory false before first resolve");

    history.markResolved();
    expectTrue(fuse::renderer::taaHistoryCanReuse(history), "warmed history can be reused");
    expectTrue(history.canReuseHistory(), "canReuseHistory true after first resolve");
    expectTrue(fuse::renderer::taaHistoryReuseAllowed(history, 0u),
               "reuse allowed when observed generation matches");
    expectTrue(!fuse::renderer::taaHistoryBlendAllowed(true, history),
               "history blend blocked on first frame even when warmed");

    history.invalidateHistory();
    expectTrue(!fuse::renderer::taaHistoryCanReuse(history), "invalidated history cannot be reused");
    expectTrue(!fuse::renderer::taaHistoryReuseAllowed(history, 0u),
               "stale observed generation blocks reuse");

    fuse::renderer::TaaResolveDesc desc{};
    desc.observed_history_generation = 0u;
    expectTrue(!fuse::renderer::taaResolveCanReuseHistory(desc, history),
               "resolve cannot reuse history after invalidate");

    desc.observed_history_generation = history.invalidateGeneration();
    history.markResolved();
    expectTrue(fuse::renderer::taaResolveCanReuseHistory(desc, history),
               "resolve can reuse history with current generation");
    expectTrue(fuse::renderer::taaHistoryBlendAllowed(false, history),
               "history blend allowed after warmup frame");

    desc.observed_history_generation = fuse::renderer::kTaaResolveNoHistoryGeneration;
    expectTrue(fuse::renderer::taaResolveCanReuseHistory(desc, history),
               "resolve reuse bypasses generation guard with sentinel");

    history.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testHistoryWarmupGuards() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for history warmup guard test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaHistoryBuffer history;
    fuse::renderer::TaaHistoryBufferDesc historyDesc{64, 64};
    expectTrue(history.init(resources, historyDesc), "history ready for warmup guard test");
    expectTrue(fuse::renderer::taaHistoryNeedsWarmup(history), "fresh history needs warmup");
    expectTrue(!fuse::renderer::taaHistoryWarmupComplete(history), "fresh history warmup incomplete");
    expectTrue(!fuse::renderer::taaHistoryCanReuse(history), "fresh history cannot be reused");

    history.markResolved();
    expectTrue(!fuse::renderer::taaHistoryNeedsWarmup(history), "warmed history no longer needs warmup");
    expectTrue(fuse::renderer::taaHistoryWarmupComplete(history), "warmed history warmup complete");
    expectTrue(fuse::renderer::taaHistoryCanReuse(history), "warmed history can be reused");

    history.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testJitterSafeNdcOffset() {
    using fuse::renderer::TaaJitterLayout;

    const fuse::math::Vec2 safeZero = TaaJitterLayout::safeHaltonNdcOffset(0u, 0u, 1080u, 8u);
    expectNear(safeZero.x, 0.f, 1e-6f, "safeHaltonNdcOffset returns zero for invalid width");
    expectNear(safeZero.y, 0.f, 1e-6f, "safeHaltonNdcOffset Y returns zero for invalid width");

    const fuse::math::Vec2 safeFrameZero = TaaJitterLayout::safeNdcOffsetForFrameIndex(3u, 1920u, 0u, 8u);
    expectNear(safeFrameZero.x, 0.f, 1e-6f, "safeNdcOffsetForFrameIndex returns zero for invalid height");
    expectNear(safeFrameZero.y, 0.f, 1e-6f, "safeNdcOffsetForFrameIndex Y returns zero for invalid height");

    const fuse::math::Vec2 safe = TaaJitterLayout::safeNdcOffsetForFrameIndex(3u, 1920u, 1080u, 8u);
    const fuse::math::Vec2 direct = TaaJitterLayout::ndcOffsetForFrameIndex(3u, 1920u, 1080u, 8u);
    expectNear(safe.x, direct.x, 1e-6f, "safeNdcOffsetForFrameIndex matches direct offset for valid viewport");
    expectNear(safe.y, direct.y, 1e-6f, "safeNdcOffsetForFrameIndex Y matches direct offset for valid viewport");
}

void testJitterAdvanceIfPossible() {
    fuse::renderer::TaaJitter jitter;
    expectTrue(jitter.canAdvance(), "default jitter can advance");
    const fuse::u32 indexBefore = jitter.index();
    expectTrue(jitter.advanceIfPossible(), "advanceIfPossible succeeds for valid sequence");
    expectTrue(jitter.index() == indexBefore + 1u, "advanceIfPossible advances slot");

    jitter.reset();
    jitter.advance();
    const fuse::u32 afterAdvance = jitter.index();
    jitter.reset();
    expectTrue(jitter.advanceIfPossible(), "advanceIfPossible succeeds after reset");
    expectTrue(jitter.index() == afterAdvance, "advanceIfPossible matches advance slot");

    fuse::renderer::TaaJitterDesc invalidDesc{};
    invalidDesc.sequence_length = 0u;
    fuse::renderer::TaaJitter fallback(invalidDesc);
    expectTrue(fallback.canAdvance(), "invalid desc falls back to default sequence");
    expectTrue(fallback.sequenceLength() == 8u, "zero sequence length falls back to default");
    expectTrue(fallback.advanceIfPossible(), "advanceIfPossible succeeds after fallback");
}

void testJitterSyncGuards() {
    using fuse::renderer::TaaJitterLayout;

    fuse::renderer::TaaJitter jitter;
    expectTrue(jitter.isSyncedToFrameIndex(0u), "fresh jitter synced to frame zero");
    expectTrue(jitter.slotMatchesMonotonicFrame(), "fresh jitter slot matches monotonic frame");

    jitter.advance();
    expectTrue(jitter.isSyncedToFrameIndex(1u), "advance keeps monotonic sync");
    expectTrue(jitter.slotMatchesMonotonicFrame(), "advance keeps slot sync");

    jitter.syncToFrameIndex(13u);
    expectTrue(jitter.isSyncedToFrameIndex(13u), "syncToFrameIndex marks monotonic sync");
    expectTrue(jitter.slotMatchesMonotonicFrame(), "syncToFrameIndex keeps slot sync");
    expectTrue(TaaJitterLayout::monotonicFrameMatchesSlot(13u, jitter.index(), 8u),
               "layout slot guard matches synced jitter");

    expectTrue(!jitter.isSyncedToFrameIndex(13u), "advance clears prior frame sync");
    expectTrue(jitter.isSyncedToFrameIndex(14u), "advance updates monotonic sync");
    expectTrue(jitter.slotMatchesMonotonicFrame(), "advance keeps slot aligned after sync drift");
    expectTrue(jitter.isSyncedToFrameIndex(0u), "initial jitter synced to frame zero");
    expectTrue(!jitter.isSyncedToFrameIndex(5u), "initial jitter not synced to frame five");

    jitter.syncToFrameIndex(5u);
    expectTrue(jitter.isSyncedToFrameIndex(5u), "syncToFrameIndex marks frame as synced");
    expectTrue(TaaJitterLayout::slotMatchesMonotonicFrame(jitter.index(), 5u, 8u),
               "slot matches monotonic frame after sync");

    expectTrue(jitter.isSyncedToFrameIndex(6u), "advance keeps monotonic sync");
    expectTrue(!jitter.isSyncedToFrameIndex(5u), "advance invalidates prior frame sync");

    fuse::renderer::TaaPassDesc passDesc{};
    passDesc.width = 128;
    passDesc.height = 128;
    auto pass = fuse::renderer::TaaPass::create(passDesc);
    pass->syncJitterToFrameIndex(11u);
    expectTrue(pass->isJitterSyncedToFrameIndex(11u), "pass jitter synced after syncJitterToFrameIndex");
    expectTrue(!pass->isJitterSyncedToFrameIndex(10u), "pass jitter not synced to prior frame");
}

void testJitterProduceNdcGuards() {
    using fuse::renderer::TaaJitterLayout;

    expectTrue(TaaJitterLayout::jitterIndexInRange(0u, 8u), "slot zero is in range");
    expectTrue(TaaJitterLayout::jitterIndexInRange(7u, 8u), "final slot is in range");
    expectTrue(!TaaJitterLayout::jitterIndexInRange(8u, 8u), "slot equal to period is out of range");
    expectTrue(!TaaJitterLayout::jitterIndexInRange(0u, 0u), "zero-length sequence rejects all slots");

    expectTrue(TaaJitterLayout::canProduceNdcOffset(1920u, 1080u, 8u),
               "valid viewport and sequence can produce NDC offset");
    expectTrue(!TaaJitterLayout::canProduceNdcOffset(0u, 1080u, 8u),
               "zero width blocks NDC offset production");
    expectTrue(!TaaJitterLayout::canProduceNdcOffset(1920u, 1080u, 0u),
               "invalid sequence blocks NDC offset production");

    fuse::renderer::TaaJitter jitter;
    expectTrue(jitter.canAdvance(), "default jitter can advance");
    expectTrue(jitter.canProduceNdcOffset(128u, 128u), "default jitter can produce NDC offset");
    expectTrue(!jitter.canProduceNdcOffset(0u, 128u), "jitter blocks zero-width NDC offset");

    const fuse::math::Vec2 validNdc = jitter.currentNdcOffset(128u, 128u);
    expectTrue(validNdc.x != 0.f || validNdc.y != 0.f, "valid viewport yields non-zero NDC offset");

    const fuse::math::Vec2 invalidNdc = jitter.currentNdcOffset(0u, 128u);
    expectNear(invalidNdc.x, 0.f, 1e-6f, "invalid viewport yields zero NDC X");
    expectNear(invalidNdc.y, 0.f, 1e-6f, "invalid viewport yields zero NDC Y");
}

void testResolveSurfaceGuards() {
    fuse::renderer::TaaResolveDesc desc{};
    desc.width = 64;
    desc.height = 64;
    expectTrue(!fuse::renderer::taaResolveSurfacesSatisfied(desc), "null surfaces not satisfied");

    desc.surfaces.current_frame = reinterpret_cast<void*>(0x1);
    expectTrue(!fuse::renderer::taaResolveSurfacesSatisfied(desc), "missing output not satisfied");

    desc.surfaces.output = reinterpret_cast<void*>(0x2);
    expectTrue(fuse::renderer::taaResolveSurfacesSatisfied(desc), "both colour surfaces satisfied");
}

void testResolvePreflightHelpers() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for resolve preflight helper test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaHistoryBuffer history;
    fuse::renderer::TaaHistoryBufferDesc historyDesc{64, 64};
    expectTrue(history.init(resources, historyDesc), "history ready for resolve preflight helper test");

    fuse::renderer::TaaResolveDesc desc{};
    desc.width = 64;
    desc.height = 64;
    desc.surfaces.current_frame = reinterpret_cast<void*>(0x1);
    desc.surfaces.output = reinterpret_cast<void*>(0x2);
    desc.params.blend_factor = 2.f;
    desc.observed_history_generation = fuse::renderer::kTaaResolveNoHistoryGeneration;

    expectTrue(fuse::renderer::canAttemptTaaResolve(desc, history), "canAttempt passes valid desc");
    expectTrue(fuse::renderer::prepareTaaResolveDesc(desc, history),
               "prepareTaaResolveDesc passes valid desc");
    expectNear(desc.params.blend_factor, 1.f, 1e-5f, "prepare clamps params");
    expectTrue(desc.observed_history_generation == 0u, "prepare stamps observed generation");

    const fuse::renderer::TaaBlendWeights preflight =
        fuse::renderer::preflightTaaBlendWeights(desc, history);
    expectNear(preflight.current, 1.f, 1e-5f, "preflight blend uses warmup weight before first resolve");
    expectTrue(fuse::renderer::taaBlendWeightsValid(preflight), "preflight blend weights are valid");

    desc.width = 0;
    expectTrue(!fuse::renderer::canAttemptTaaResolve(desc, history), "canAttempt rejects invalid dimensions");
    expectTrue(!fuse::renderer::prepareTaaResolveDesc(desc, history),
               "prepare rejects invalid dimensions");

    history.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testResolveWillReuseHistory() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for resolve will-reuse test");



    expectTrue(history.init(resources, historyDesc), "history ready for resolve will-reuse test");

    desc.params.blend_factor = 0.25f;
    desc.observed_history_generation = 0u;

    expectTrue(!fuse::renderer::taaResolveWillReuseHistory(desc, history),
               "unwarmed history will not be reused");

    fuse::renderer::TaaResolve resolve;
    expectTrue(resolve.resolve(desc, history), "initial resolve warms history");
    expectTrue(!resolve.lastStats().history_reused, "first resolve does not reuse history");

    expectTrue(fuse::renderer::taaResolveWillReuseHistory(desc, history),
               "warmed history will be reused with partial blend");
    expectTrue(resolve.resolve(desc, history), "second resolve succeeds");
    expectTrue(resolve.lastStats().history_reused, "second resolve records history reuse");

    desc.params.blend_factor = 1.f;
               "full blend will not reuse history");
    expectTrue(resolve.resolve(desc, history), "full blend resolve succeeds");
    expectTrue(!resolve.lastStats().history_reused, "full blend resolve does not reuse history");


void testPreflightTaaResolveBlend() {
    expectTrue(bootstrap != nullptr, "bootstrap allocated for resolve blend preflight test");



    expectTrue(history.init(resources, historyDesc), "history ready for resolve blend preflight test");

    desc.params.blend_factor = 0.3f;

    fuse::renderer::TaaBlendWeights weights{};
    expectTrue(fuse::renderer::preflightTaaResolveBlend(desc, history, &weights),
               "blend preflight passes unwarmed history");
    expectNear(weights.current, 1.f, 1e-5f, "blend preflight warmup uses full current weight");
    expectNear(weights.history, 0.f, 1e-5f, "blend preflight warmup uses zero history weight");
    expectTrue(fuse::renderer::taaBlendWeightsValid(weights), "blend preflight weights are valid");

    expectTrue(resolve.resolve(desc, history), "initial resolve warms history for blend preflight");

               "blend preflight passes warmed history");
    expectNear(weights.current, 0.3f, 1e-5f, "blend preflight uses configured current weight");
    expectNear(weights.history, 0.7f, 1e-5f, "blend preflight uses history complement");

    expectTrue(!fuse::renderer::preflightTaaResolveBlend(desc, history, &weights),
               "blend preflight rejects invalid dimensions");


void testTaaPassResolvePreflight() {
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass resolve preflight test");



    fuse::renderer::TaaPassDesc passDesc{};
    passDesc.width = 64;
    passDesc.height = 64;

    auto pass = fuse::renderer::TaaPass::create(passDesc);
    expectTrue(pass->init(resources), "TaaPass initialized for resolve preflight test");

    desc.params.blend_factor = 0.2f;

    expectTrue(!pass->resolveWillReuseHistory(desc), "pass will not reuse history before warmup");
    expectTrue(pass->canResolveFrame(desc), "pass canResolveFrame passes valid desc");
    expectTrue(pass->prepareAndCanResolve(desc), "pass prepareAndCanResolve passes valid desc");
    expectTrue(pass->resolveFrame(desc), "initial resolve warms pass history");
    expectTrue(pass->resolveWillReuseHistory(desc), "pass will reuse history after warmup");

    desc.width = 32;
    expectTrue(!pass->canResolveFrame(desc), "pass canResolveFrame rejects dimension mismatch");
    expectTrue(!pass->prepareAndCanResolve(desc), "pass prepareAndCanResolve rejects mismatch");

    pass->destroy();

void testTaaPassJitterSyncGuards() {
    passDesc.width = 128;
    passDesc.height = 128;

    expectTrue(pass->isJitterSyncedToFrameIndex(0u), "pass jitter synced to frame zero before init");
    expectTrue(pass->jitterMonotonicFrameIndex() == 0u, "pass jitter monotonic counter starts at zero");

    pass->syncJitterToFrameIndex(11u);
    expectTrue(pass->isJitterSyncedToFrameIndex(11u), "pass syncJitterToFrameIndex marks sync");
    expectTrue(pass->jitterMonotonicFrameIndex() == 11u, "pass sync sets monotonic counter");

    pass->advanceJitter();
    expectTrue(!pass->isJitterSyncedToFrameIndex(11u), "pass advance clears prior sync");
    expectTrue(pass->isJitterSyncedToFrameIndex(12u), "pass advance updates sync");
    expectTrue(pass->jitter().slotMatchesMonotonicFrame(), "pass jitter slot stays aligned after advance");

void testResolveHistoryBlendStats() {
    auto bootstrap = createTaaTestBootstrap();
    if (bootstrap == nullptr) {
        std::fprintf(stderr, "SKIP: bootstrap allocated for history blend stats test (Vulkan device unavailable)\n");
        return;
    TAA_SKIP_UNLESS_VULKAN_READY(bootstrap);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for history blend stats test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaHistoryBuffer history;
    fuse::renderer::TaaHistoryBufferDesc historyDesc{64, 64};
    expectTrue(history.init(resources, historyDesc), "history ready for history blend stats test");

    fuse::renderer::TaaResolve resolve;
    fuse::renderer::TaaResolveDesc desc{};
    desc.width = 64;
    desc.height = 64;
    desc.surfaces.current_frame = reinterpret_cast<void*>(0x1);
    desc.surfaces.output = reinterpret_cast<void*>(0x2);
    desc.params.blend_factor = 0.25f;

    expectTrue(resolve.resolve(desc, history), "first resolve succeeds");
    expectNear(resolve.lastStats().effective_blend, 1.f, 1e-5f, "first resolve uses full current blend");
    expectNear(resolve.lastStats().history_blend, 0.f, 1e-5f, "first resolve records zero history blend");

    expectTrue(resolve.resolve(desc, history), "second resolve succeeds");
    expectNear(resolve.lastStats().effective_blend, 0.25f, 1e-5f, "second resolve uses configured current blend");
    expectNear(resolve.lastStats().history_blend, 0.75f, 1e-5f, "second resolve records history blend complement");
    expectTrue(fuse::renderer::taaBlendWeightsValid(
                   {resolve.lastStats().effective_blend, resolve.lastStats().history_blend}),
               "resolve stats blend weights are valid");

    history.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testHistoryNeedsWarmupGuard() {
    auto bootstrap = createTaaTestBootstrap();
    if (bootstrap == nullptr) {
        std::fprintf(stderr, "SKIP: bootstrap allocated for needs-warmup guard test (Vulkan device unavailable)\n");
        return;
    }
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    TAA_SKIP_UNLESS_VULKAN_READY(bootstrap);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for needs-warmup guard test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaHistoryBuffer history;
    fuse::renderer::TaaHistoryBufferDesc historyDesc{64, 64};
    expectTrue(history.init(resources, historyDesc), "history ready for needs-warmup guard test");
    expectTrue(fuse::renderer::taaHistoryNeedsWarmup(history), "fresh history needs warmup");
    expectTrue(history.needsWarmup(), "needsWarmup mirrors taaHistoryNeedsWarmup");

    history.markResolved();
    expectTrue(!fuse::renderer::taaHistoryNeedsWarmup(history), "warmed history does not need warmup");

    history.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testComputeTaaResolveBlendWeights() {
    expectTrue(bootstrap != nullptr, "bootstrap allocated for resolve blend weights test");



    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    auto bootstrap = createTaaTestBootstrap();
    if (bootstrap == nullptr) {
        std::fprintf(stderr, "SKIP: bootstrap allocated for resolve blend weights test (Vulkan device unavailable)\n");
        return;
    }
    TAA_SKIP_UNLESS_VULKAN_READY(bootstrap);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for resolve blend weights test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaHistoryBuffer history;
    fuse::renderer::TaaHistoryBufferDesc historyDesc{64, 64};
    expectTrue(history.init(resources, historyDesc), "history ready for resolve blend weights test");

    fuse::renderer::TaaResolveDesc desc{};
    desc.params.blend_factor = 0.3f;

    const fuse::renderer::TaaBlendWeights warmup =
        fuse::renderer::computeTaaResolveBlendWeights(desc, history);
    expectNear(warmup.current, 1.f, 1e-5f, "resolve blend weights use full current on warmup");
    expectNear(warmup.history, 0.f, 1e-5f, "resolve blend weights use zero history on warmup");
    expectTrue(!fuse::renderer::taaResolveAppliesHistoryBlend(desc, history),
               "resolve does not apply history blend on warmup");

    const fuse::renderer::TaaBlendWeights steady =
    history.markResolved();
        fuse::renderer::computeTaaResolveBlendWeights(desc, history);
    expectNear(steady.current, 0.3f, 1e-5f, "resolve blend weights use configured current after warmup");
    expectNear(steady.history, 0.7f, 1e-5f, "resolve blend weights use history complement after warmup");
    expectTrue(fuse::renderer::taaResolveAppliesHistoryBlend(desc, history),
               "resolve applies history blend after warmup");

    history.invalidateHistory();
    desc.observed_history_generation = 0u;
    const fuse::renderer::TaaBlendWeights stale =
    expectNear(stale.current, 1.f, 1e-5f, "stale generation forces full current blend");
    expectNear(stale.history, 0.f, 1e-5f, "stale generation zeroes history blend");
               "resolve does not apply history blend when generation is stale");

        fuse::renderer::computeTaaResolveBlendWeights(desc, history);
    expectTrue(!fuse::renderer::taaResolveAppliesHistoryBlend(desc, history),

    history.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testBlendWeightsConsistentWithReuse() {
    const fuse::renderer::TaaBlendWeights warmup{1.f, 0.f};
    expectTrue(fuse::renderer::taaBlendWeightsConsistentWithReuse(warmup, false),
               "warmup weights consistent when history blend blocked");
    expectTrue(fuse::renderer::taaBlendWeightsConsistentWithReuse(warmup, true),
               "warmup weights consistent when history blend allowed");

    const fuse::renderer::TaaBlendWeights steady{0.2f, 0.8f};
    expectTrue(fuse::renderer::taaBlendWeightsConsistentWithReuse(steady, true),
               "steady weights consistent when history blend allowed");
    expectTrue(!fuse::renderer::taaBlendWeightsConsistentWithReuse(steady, false),
               "steady weights inconsistent when history blend blocked");

    const fuse::renderer::TaaBlendWeights invalid{0.6f, 0.6f};
    expectTrue(!fuse::renderer::taaBlendWeightsConsistentWithReuse(invalid, true),
               "invalid weights fail consistency check");
}

void testClassifyTaaHistoryReuseBlock() {
    fuse::renderer::TaaHistoryBuffer emptyHistory;
    expectTrue(fuse::renderer::classifyTaaHistoryReuseBlock(emptyHistory, 0u) ==
                   fuse::renderer::TaaHistoryReuseBlockReason::NotReady,
               "empty history classified as NotReady");
    expectTrue(std::strcmp(fuse::renderer::taaHistoryReuseBlockReasonLabel(
                               fuse::renderer::TaaHistoryReuseBlockReason::NotReady),
                           "not_ready") == 0,
               "NotReady reuse block label");

    expectTrue(bootstrap != nullptr, "bootstrap allocated for reuse block classify test");



    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    auto bootstrap = createTaaTestBootstrap();
    if (bootstrap == nullptr) {
        std::fprintf(stderr, "SKIP: bootstrap allocated for reuse block classify test (Vulkan device unavailable)\n");
        return;
    }
    TAA_SKIP_UNLESS_VULKAN_READY(bootstrap);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for reuse block classify test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaHistoryBuffer history;
    fuse::renderer::TaaHistoryBufferDesc historyDesc{64, 64};
    expectTrue(history.init(resources, historyDesc), "history ready for reuse block classify test");
    expectTrue(fuse::renderer::classifyTaaHistoryReuseBlock(history, 0u) ==
                   fuse::renderer::TaaHistoryReuseBlockReason::NotWarm,
               "unwarmed history classified as NotWarm");
    expectTrue(std::strcmp(fuse::renderer::taaHistoryReuseBlockReasonLabel(
                               fuse::renderer::TaaHistoryReuseBlockReason::NotWarm),
                           "not_warm") == 0,
               "NotWarm reuse block label");

                   fuse::renderer::TaaHistoryReuseBlockReason::None,
               "warmed history with matching generation is not blocked");

                   fuse::renderer::TaaHistoryReuseBlockReason::StaleGeneration,
               "stale generation classified as StaleGeneration");
    history.markResolved();
    expectTrue(fuse::renderer::classifyTaaHistoryReuseBlock(history, 0u) ==

    history.invalidateHistory();
    expectTrue(std::strcmp(fuse::renderer::taaHistoryReuseBlockReasonLabel(
                               fuse::renderer::TaaHistoryReuseBlockReason::StaleGeneration),
                           "stale_generation") == 0,
               "StaleGeneration reuse block label");

    history.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testJitterSyncAndAdvanceGuards() {
    using fuse::renderer::TaaJitterLayout;

    expectTrue(TaaJitterLayout::canSyncToFrameIndex(0u, 8u), "valid sequence allows sync");
    expectTrue(TaaJitterLayout::canSyncToFrameIndex(1008u, 8u), "large frame index allows sync");
    expectTrue(!TaaJitterLayout::canSyncToFrameIndex(0u, 0u), "invalid sequence blocks sync");

    fuse::renderer::TaaJitter jitter;
    expectTrue(jitter.canSyncToFrameIndex(5u), "default jitter can sync");
    jitter.syncToFrameIndex(5u);
    expectTrue(jitter.index() == 5u, "syncToFrameIndex still works with guard");

    const fuse::u32 indexBefore = jitter.index();
    expectTrue(jitter.advanceIfReady(), "advanceIfReady succeeds for valid sequence");
    expectTrue(jitter.index() != indexBefore, "advanceIfReady advances jitter");

    fuse::renderer::TaaJitterDesc invalidDesc{};
    invalidDesc.sequence_length = 0u;
    fuse::renderer::TaaJitter fallbackJitter(invalidDesc);
    expectTrue(fallbackJitter.sequenceLength() == 8u, "invalid desc falls back to default sequence length");
    expectTrue(fallbackJitter.canAdvance(), "fallback jitter can advance");
    expectTrue(fallbackJitter.advanceIfReady(), "advanceIfReady succeeds after fallback");
    expectTrue(fallbackJitter.canSyncToFrameIndex(0u), "sync allowed after fallback to default length");

void testTaaPassExpectedBlendAndReuseGuards() {
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass expected blend test");


}

    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    auto bootstrap = createTaaTestBootstrap();
    if (bootstrap == nullptr) {
        std::fprintf(stderr, "SKIP: bootstrap allocated for pass expected blend test (Vulkan device unavailable)\n");
        return;
    TAA_SKIP_UNLESS_VULKAN_READY(bootstrap);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass expected blend test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaPassDesc passDesc{};
    passDesc.width = 64;
    passDesc.height = 64;
    passDesc.params.blend_factor = 0.25f;

    auto pass = fuse::renderer::TaaPass::create(passDesc);
    expectTrue(pass->init(resources), "TaaPass initialized for expected blend test");

    fuse::renderer::TaaResolveDesc resolveDesc{};
    resolveDesc.params = passDesc.params;

    const fuse::renderer::TaaBlendWeights preWarmup = pass->expectedResolveBlendWeights(resolveDesc);
    expectNear(preWarmup.current, 1.f, 1e-5f, "pass expected blend is full current before warmup");
    expectTrue(!pass->resolveWouldReuseHistory(resolveDesc), "pass would not reuse history before warmup");
    expectTrue(pass->classifyHistoryReuseBlock(0u) == fuse::renderer::TaaHistoryReuseBlockReason::NotWarm,
               "pass classifies unwarmed history as NotWarm");

    resolveDesc.width = 64;
    resolveDesc.height = 64;
    resolveDesc.surfaces.current_frame = reinterpret_cast<void*>(0x10);
    resolveDesc.surfaces.output = reinterpret_cast<void*>(0x20);
    expectTrue(pass->resolveFrame(resolveDesc), "initial resolve warms pass history");

    const fuse::renderer::TaaBlendWeights postWarmup = pass->expectedResolveBlendWeights(resolveDesc);
    expectNear(postWarmup.current, 0.25f, 1e-5f, "pass expected blend uses configured current after warmup");
    expectNear(postWarmup.history, 0.75f, 1e-5f, "pass expected blend uses history complement after warmup");
    expectTrue(pass->resolveWouldReuseHistory(resolveDesc), "pass would reuse history after warmup");
    expectTrue(pass->classifyHistoryReuseBlock(0u) == fuse::renderer::TaaHistoryReuseBlockReason::None,
               "pass classifies warmed history as not blocked");

    const fuse::u32 jitterIndexBefore = pass->jitter().index();
    expectTrue(pass->advanceJitterIfReady(), "pass advanceJitterIfReady succeeds");
    expectTrue(pass->jitter().index() != jitterIndexBefore, "pass advanceJitterIfReady advances jitter");

    pass->invalidateHistory();
    expectTrue(pass->classifyHistoryReuseBlock(0u) ==
               "pass classifies stale generation after invalidate");

    pass->destroy();

void testPreflightTaaHistoryReuse() {
    fuse::renderer::TaaHistoryReuseBlockReason reason = fuse::renderer::TaaHistoryReuseBlockReason::None;
    expectTrue(!fuse::renderer::preflightTaaHistoryReuse(emptyHistory, 0u, &reason),
               "empty history fails reuse preflight");
    expectTrue(reason == fuse::renderer::TaaHistoryReuseBlockReason::NotReady,
               "empty history reuse preflight reason is NotReady");

    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    TAA_SKIP_UNLESS_VULKAN_READY(bootstrap);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for history reuse preflight test");
    auto bootstrap = createTaaTestBootstrap();
    if (bootstrap == nullptr) {
        std::fprintf(stderr, "SKIP: bootstrap allocated for history reuse preflight test (Vulkan device unavailable)\n");
        return;
    }



    expectTrue(history.init(resources, historyDesc), "history ready for reuse preflight test");
    expectTrue(fuse::renderer::taaHistoryReadyForResolve(history), "allocated history ready for resolve");
    expectTrue(fuse::renderer::taaHistoryWarmupFramesRemaining(history) == 1u,
               "unwarmed history has one warmup frame remaining");
    expectTrue(!fuse::renderer::preflightTaaHistoryReuse(history, 0u, &reason),
               "unwarmed history fails reuse preflight");
    expectTrue(reason == fuse::renderer::TaaHistoryReuseBlockReason::NotWarm,
               "unwarmed history reuse preflight reason is NotWarm");

    expectTrue(fuse::renderer::taaHistoryWarmupFramesRemaining(history) == 0u,
               "warmed history has zero warmup frames remaining");
    expectTrue(fuse::renderer::preflightTaaHistoryReuse(history, 0u, &reason),
               "warmed history passes reuse preflight");
    expectTrue(reason == fuse::renderer::TaaHistoryReuseBlockReason::None,
               "warmed history reuse preflight reason is None");

               "stale generation fails reuse preflight");
    expectTrue(reason == fuse::renderer::TaaHistoryReuseBlockReason::StaleGeneration,
               "stale generation reuse preflight reason is StaleGeneration");


void testPreflightTaaResolveBlendWeights() {
    expectTrue(std::strcmp(fuse::renderer::taaResolveBlendRejectReasonLabel(
                               fuse::renderer::TaaResolveBlendRejectReason::None),
                           "none") == 0,
               "None blend reject label");
                               fuse::renderer::TaaResolveBlendRejectReason::InvalidWeights),
                           "invalid_weights") == 0,
               "InvalidWeights blend reject label");
                               fuse::renderer::TaaResolveBlendRejectReason::InconsistentWithReuse),
                           "inconsistent_with_reuse") == 0,
               "InconsistentWithReuse blend reject label");

    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    TAA_SKIP_UNLESS_VULKAN_READY(bootstrap);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for blend preflight test");
    auto bootstrap = createTaaTestBootstrap();
    if (bootstrap == nullptr) {
        std::fprintf(stderr, "SKIP: bootstrap allocated for blend preflight test (Vulkan device unavailable)\n");
        return;
    }



    expectTrue(history.init(resources, historyDesc), "history ready for blend preflight test");

    desc.params.blend_factor = 0.2f;

    fuse::renderer::TaaResolveBlendRejectReason rejectReason =
        fuse::renderer::TaaResolveBlendRejectReason::None;
    expectTrue(fuse::renderer::preflightTaaResolveBlendWeights(desc, history, &rejectReason),
               "warmup blend weights pass preflight");
    expectTrue(rejectReason == fuse::renderer::TaaResolveBlendRejectReason::None,
               "warmup blend preflight reject reason is None");
    expectTrue(fuse::renderer::classifyTaaResolveBlendReject(desc, history) ==
                   fuse::renderer::TaaResolveBlendRejectReason::None,
               "warmup blend classify returns None");

               "steady blend weights pass preflight after warmup");

               "stale generation forces full-current blend that still passes preflight");
               "stale generation blend preflight reject reason is None");

               "invalid weights fail reuse consistency");
    expectTrue(!fuse::renderer::taaBlendWeightsValid(invalid), "invalid weights fail validity check");


void testJitterSyncIfReadyGuards() {

    expectTrue(TaaJitterLayout::jitterSlotMatchesFrameIndex(5u, 5u, 8u),
               "slot five matches frame index five");
    expectTrue(TaaJitterLayout::jitterSlotMatchesFrameIndex(13u, 5u, 8u),
               "wrapped frame index maps to matching slot");
    expectTrue(!TaaJitterLayout::jitterSlotMatchesFrameIndex(5u, 6u, 8u),
               "mismatched slot fails frame-index alignment check");
    expectTrue(!TaaJitterLayout::jitterSlotMatchesFrameIndex(0u, 0u, 0u),
               "invalid sequence fails slot alignment check");

    expectTrue(!jitter.isAlignedToFrameIndex(5u), "default jitter is not aligned to frame five");
    expectTrue(jitter.syncToFrameIndexIfReady(5u), "syncToFrameIndexIfReady succeeds for valid sequence");
    expectTrue(jitter.isAlignedToFrameIndex(5u), "jitter aligned after syncToFrameIndexIfReady");
    expectTrue(jitter.monotonicFrameIndex() == 5u, "syncToFrameIndexIfReady sets monotonic counter");

    expectTrue(fallbackJitter.syncToFrameIndexIfReady(3u), "syncToFrameIndexIfReady succeeds after fallback");
    expectTrue(fallbackJitter.isAlignedToFrameIndex(3u), "fallback jitter aligned after sync");
void testJitterSyncGuards() {

    expectTrue(jitter.isSyncedToFrameIndex(5u), "jitter synced after syncToFrameIndex");
    expectTrue(!jitter.isSyncedToFrameIndex(4u), "jitter not synced to prior frame index");

    jitter.syncToFrameIndex(13u);
    expectTrue(jitter.isSyncedToFrameIndex(13u), "jitter synced after wrapped syncToFrameIndex");
    expectTrue(TaaJitterLayout::jitterSyncMatches(13u, 5u, 8u),
               "wrapped frame indices map to same jitter slot");
    expectTrue(!TaaJitterLayout::jitterSyncMatches(13u, 6u, 8u),
               "different slots fail jitter sync match");

    passDesc.width = 128;
    passDesc.height = 128;
    expectTrue(!pass->jitterAlignedToFrameIndex(7u), "pass jitter not aligned before sync");
    expectTrue(pass->syncJitterToFrameIndexIfReady(7u), "pass syncJitterToFrameIndexIfReady succeeds");
    expectTrue(pass->jitterAlignedToFrameIndex(7u), "pass jitter aligned after syncIfReady");
    expectTrue(pass->jitter().monotonicFrameIndex() == 7u, "pass syncIfReady sets monotonic counter");

void testTaaPassPreflightHelpers() {
    pass->syncJitterToFrameIndex(2u);
    expectTrue(pass->isJitterSyncedToFrameIndex(2u), "pass jitter synced after syncJitterToFrameIndex");
    expectTrue(!pass->isJitterSyncedToFrameIndex(3u), "pass jitter not synced to different frame");

void testJitterSafeNdcOffset() {

    const fuse::math::Vec2 safeZero = TaaJitterLayout::safeHaltonNdcOffset(0u, 0u, 1080u, 8u);
    expectNear(safeZero.x, 0.f, 1e-6f, "safeHaltonNdcOffset returns zero for invalid width");
    expectNear(safeZero.y, 0.f, 1e-6f, "safeHaltonNdcOffset Y returns zero for invalid width");

    const fuse::math::Vec2 safeFrameZero = TaaJitterLayout::safeNdcOffsetForFrameIndex(3u, 1920u, 0u, 8u);
    expectNear(safeFrameZero.x, 0.f, 1e-6f, "safeNdcOffsetForFrameIndex returns zero for invalid height");
    expectNear(safeFrameZero.y, 0.f, 1e-6f, "safeNdcOffsetForFrameIndex Y returns zero for invalid height");

    const fuse::math::Vec2 safe = TaaJitterLayout::safeNdcOffsetForFrameIndex(3u, 1920u, 1080u, 8u);
    const fuse::math::Vec2 direct = TaaJitterLayout::ndcOffsetForFrameIndex(3u, 1920u, 1080u, 8u);
    expectNear(safe.x, direct.x, 1e-6f, "safeNdcOffsetForFrameIndex matches direct offset for valid viewport");
    expectNear(safe.y, direct.y, 1e-6f, "safeNdcOffsetForFrameIndex Y matches direct offset for valid viewport");

void testJitterAdvanceIfPossible() {
    expectTrue(jitter.canAdvance(), "default jitter can advance");
    expectTrue(jitter.advanceIfPossible(), "advanceIfPossible succeeds for valid sequence");
    expectTrue(jitter.index() == indexBefore + 1u, "advanceIfPossible advances slot");

    jitter.reset();
    jitter.advance();
    const fuse::u32 afterAdvance = jitter.index();
    expectTrue(jitter.advanceIfPossible(), "advanceIfPossible succeeds after reset");
    expectTrue(jitter.index() == afterAdvance, "advanceIfPossible matches advance slot");

void testHistoryWarmupPreflightGuards() {
void testHistoryReuseRejectReasons() {
    expectTrue(std::strcmp(fuse::renderer::taaHistoryReuseRejectReasonLabel(
                               fuse::renderer::TaaHistoryReuseRejectReason::None),
               "None history reuse reject label");
                               fuse::renderer::TaaHistoryReuseRejectReason::NotReady),
               "NotReady history reuse reject label");
                               fuse::renderer::TaaHistoryReuseRejectReason::NeedsWarmup),
                           "needs_warmup") == 0,
               "NeedsWarmup history reuse reject label");
                               fuse::renderer::TaaHistoryReuseRejectReason::StaleGeneration),
               "StaleGeneration history reuse reject label");

    expectTrue(fuse::renderer::classifyTaaHistoryReuseReject(emptyHistory, 0u) ==
                   fuse::renderer::TaaHistoryReuseRejectReason::NotReady,
    expectTrue(!fuse::renderer::tryTaaHistoryReuse(emptyHistory, 0u),
               "empty history fails tryTaaHistoryReuse");
void testHistoryWarmupGuards() {
    expectTrue(fuse::renderer::taaHistoryNeedsWarmup(emptyHistory), "empty history needs warmup");
    expectTrue(!fuse::renderer::taaHistoryWarmupComplete(emptyHistory), "empty history warmup incomplete");
    expectTrue(!emptyHistory.warmupComplete(), "empty history warmupComplete is false");
    expectTrue(fuse::renderer::taaResolveWouldBeFirstFrame(emptyHistory),
               "empty history next resolve would be first frame");

    expectTrue(jitter.isSyncedToFrameIndex(0u), "default jitter starts synced to frame zero");
    expectTrue(!jitter.needsSyncToFrameIndex(0u), "default jitter does not need resync to frame zero");
    expectTrue(jitter.needsSyncToFrameIndex(1u), "default jitter needs sync to frame one");
    expectTrue(!jitter.isSyncedToFrameIndex(1u), "default jitter is not synced to frame one");

    jitter.syncToFrameIndex(11u);
    expectTrue(jitter.isSyncedToFrameIndex(11u), "syncToFrameIndex marks frame as synced");
    expectTrue(!jitter.needsSyncToFrameIndex(11u), "synced jitter no longer needs resync");
    expectTrue(jitter.slotMatchesFrame(11u), "synced slot matches wrapped frame index");
    expectTrue(TaaJitterLayout::jitterIndexMatchesFrame(jitter.index(), 11u, 8u),
               "layout slot match agrees with jitter state");

    expectTrue(jitter.isSyncedToFrameIndex(12u), "advance keeps monotonic sync");
    expectTrue(jitter.slotMatchesFrame(12u), "advanced slot matches frame index");

    expectTrue(jitter.isSyncedToFrameIndex(0u), "reset syncs monotonic counter to zero");
    expectTrue(jitter.slotMatchesFrame(0u), "reset slot matches frame zero");

    expectTrue(!fuse::renderer::taaHistoryWarmupRequired(emptyHistory),
               "empty history does not require warmup preflight");
    expectTrue(!fuse::renderer::taaHistoryWarmupComplete(emptyHistory),
               "empty history warmup is incomplete");
    expectTrue(!fuse::renderer::preflightTaaHistoryReuse(emptyHistory, 0u),


    expectTrue(TaaJitterLayout::slotMatchesFrameIndex(5u, 13u, 8u),
               "slot matches wrapped monotonic frame index");
    expectTrue(!TaaJitterLayout::slotMatchesFrameIndex(6u, 13u, 8u),
               "mismatched slot fails frame-index sync check");
    expectTrue(!TaaJitterLayout::slotMatchesFrameIndex(0u, 0u, 0u),
               "invalid sequence rejects slot/frame sync check");

    expectTrue(jitter.canSyncToFrameIndex(), "default jitter can sync to frame index");
    expectTrue(jitter.isSyncedToFrameIndex(0u), "fresh jitter is synced to frame zero");

    expectTrue(!jitter.isSyncedToFrameIndex(0u), "advanced jitter is not synced to frame zero");
    expectTrue(jitter.isSyncedToFrameIndex(1u), "advanced jitter is synced to its monotonic frame");

    jitter.syncToFrameIndex(21u);
    expectTrue(jitter.isSyncedToFrameIndex(21u), "syncToFrameIndex marks jitter as synced");
    expectTrue(jitter.index() == 5u, "synced jitter slot wraps monotonic frame");
    expectTrue(TaaJitterLayout::slotMatchesFrameIndex(jitter.index(), 21u, jitter.sequenceLength()),
               "synced slot matches layout frame mapping");
    expectTrue(fuse::renderer::taaJitterFrameSynced(jitter, 21u),
               "free-function jitter sync guard matches instance check");
    expectTrue(!fuse::renderer::taaJitterFrameSynced(jitter, 20u),
               "free-function jitter sync guard rejects prior frame");

    expectTrue(jitter.isSyncedToFrameIndex(0u), "reset jitter is synced to frame zero");

void testHistoryWarmupReuseGuards() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    TAA_SKIP_UNLESS_VULKAN_READY(bootstrap);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass preflight helper test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for history warmup preflight test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for history reuse reject test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for history warmup guard test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for warmup preflight test");
    auto bootstrap = createTaaTestBootstrap();
    if (bootstrap == nullptr) {
        std::fprintf(stderr, "SKIP: bootstrap allocated for pass preflight helper test (Vulkan device unavailable)\n");
        return;
    }

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaPassDesc passDesc{};
    passDesc.width = 64;
    passDesc.height = 64;
    passDesc.params.blend_factor = 0.3f;

    auto pass = fuse::renderer::TaaPass::create(passDesc);
    expectTrue(pass->init(resources), "TaaPass initialized for preflight helper test");

    fuse::renderer::TaaHistoryReuseBlockReason reuseReason = fuse::renderer::TaaHistoryReuseBlockReason::None;
    expectTrue(!pass->preflightHistoryReuse(0u, &reuseReason),
               "pass reuse preflight fails before warmup");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::NotWarm,
               "pass reuse preflight reason is NotWarm before warmup");

    fuse::renderer::TaaResolveDesc resolveDesc{};
    resolveDesc.width = 64;
    resolveDesc.height = 64;
    resolveDesc.surfaces.current_frame = reinterpret_cast<void*>(0x10);
    resolveDesc.surfaces.output = reinterpret_cast<void*>(0x20);
    resolveDesc.params = passDesc.params;

    fuse::renderer::TaaResolveBlendRejectReason blendReason =
    expectTrue(pass->preflightResolveBlendWeights(resolveDesc, &blendReason),
               "pass blend preflight passes before first resolve");
    expectTrue(pass->resolveFrame(resolveDesc), "initial resolve warms pass history");

    expectTrue(pass->preflightHistoryReuse(0u, &reuseReason),
               "pass reuse preflight passes after warmup");
               "pass blend preflight passes after warmup");

    pass->invalidateHistory();
               "pass reuse preflight fails after invalidate");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::StaleGeneration,
               "pass reuse preflight reason is StaleGeneration after invalidate");

    fuse::renderer::TaaHistoryBuffer history;
    fuse::renderer::TaaHistoryBufferDesc historyDesc{64, 64};
    expectTrue(history.init(resources, historyDesc), "history ready for warmup preflight test");
    expectTrue(fuse::renderer::taaHistoryNeedsWarmup(history), "unwarmed history needs warmup");
    expectTrue(!fuse::renderer::taaHistoryWarmupComplete(history), "unwarmed history warmup incomplete");
    expectTrue(!fuse::renderer::taaHistoryReusePreflightPasses(history, 0u),
               "reuse preflight fails before warmup");

    history.markResolved();
    expectTrue(!fuse::renderer::taaHistoryNeedsWarmup(history), "warmed history no longer needs warmup");
    expectTrue(fuse::renderer::taaHistoryWarmupComplete(history), "warmed history warmup complete");
    expectTrue(fuse::renderer::taaHistoryReusePreflightPasses(history, 0u),
               "reuse preflight passes after warmup");

    history.invalidateHistory();
    expectTrue(fuse::renderer::taaHistoryNeedsWarmup(history), "invalidated history needs warmup again");
    expectTrue(history.init(resources, historyDesc), "history ready for reuse reject test");
    expectTrue(!history.hasReadableHistory(), "history not readable before warmup");
    expectTrue(fuse::renderer::classifyTaaHistoryReuseReject(history, 0u) ==
                   fuse::renderer::TaaHistoryReuseRejectReason::NeedsWarmup,
               "unwarmed history classified as NeedsWarmup");

    expectTrue(history.hasReadableHistory(), "history readable after warmup");
    expectTrue(fuse::renderer::tryTaaHistoryReuse(history, 0u),
               "warmed history passes tryTaaHistoryReuse");

    expectTrue(!history.hasReadableHistory(), "invalidated history is not readable");
               "invalidated history needs warmup again");
    expectTrue(fuse::renderer::classifyTaaHistoryReuseReject(history, 99u) ==
               "stale generation still needs warmup before reuse");

                   fuse::renderer::TaaHistoryReuseRejectReason::StaleGeneration,
               "stale generation classified after warmup");
    expectTrue(fuse::renderer::classifyTaaHistoryReuseReject(history, history.invalidateGeneration()) ==
                   fuse::renderer::TaaHistoryReuseRejectReason::None,
               "current generation passes classify after warmup");
    expectTrue(fuse::renderer::classifyTaaHistoryReuseReject(
                   history, fuse::renderer::kTaaResolveNoHistoryGeneration) ==
               "sentinel bypasses stale generation classify");
    expectTrue(history.init(resources, historyDesc), "history ready for warmup guard test");
    expectTrue(fuse::renderer::taaHistoryNeedsWarmup(history), "allocated history still needs warmup");
    expectTrue(!fuse::renderer::taaHistoryWarmupComplete(history), "allocated history warmup incomplete");
    expectTrue(fuse::renderer::taaResolveWouldBeFirstFrame(history),
               "allocated history next resolve would be first frame");

    expectTrue(!fuse::renderer::taaHistoryNeedsWarmup(history), "resolved history no longer needs warmup");
    expectTrue(fuse::renderer::taaHistoryWarmupComplete(history), "resolved history warmup complete");
    expectTrue(history.warmupComplete(), "resolved history warmupComplete is true");
    expectTrue(!fuse::renderer::taaResolveWouldBeFirstFrame(history),
               "resolved history next resolve is not first frame");

               "invalidated history next resolve is first frame again");
    expectTrue(fuse::renderer::taaHistoryWarmupRequired(history), "allocated history requires warmup");
    expectTrue(!history.warmupComplete(), "warmupComplete false before first resolve");
    expectTrue(!history.temporalReuseAllowed(0u), "temporal reuse blocked before warmup");

    expectTrue(!fuse::renderer::taaHistoryWarmupRequired(history), "warmed history no longer requires warmup");
    expectTrue(history.warmupComplete(), "warmupComplete true after first resolve");
    expectTrue(fuse::renderer::preflightTaaHistoryReuse(history, 0u),
               "reuse preflight passes with current generation");

    expectTrue(!fuse::renderer::preflightTaaHistoryReuse(history, 0u),
               "reuse preflight fails after invalidate");
    expectTrue(fuse::renderer::taaHistoryNeedsWarmup(history), "initialized history needs warmup");
    expectTrue(!fuse::renderer::taaHistoryWarmupComplete(history), "warmup not complete before resolve");

    fuse::renderer::TaaHistoryReuseRejectReason reuseReason = fuse::renderer::TaaHistoryReuseRejectReason::None;
    expectTrue(!fuse::renderer::taaHistoryReusePreflight(history, 0u, &reuseReason),
               "reuse preflight rejects unwarmed history");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseRejectReason::HistoryNotWarmed,
               "unwarmed history reports HistoryNotWarmed");

    expectTrue(fuse::renderer::taaHistoryWarmupComplete(history), "warmup complete after first resolve");
    expectTrue(fuse::renderer::taaHistoryReusePreflight(history, 0u, &reuseReason),
               "reuse preflight passes warmed history");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseRejectReason::None,
               "warmed history reuse reason is None");

    reuseReason = fuse::renderer::TaaHistoryReuseRejectReason::None;
    expectTrue(!fuse::renderer::taaHistoryReusePreflight(history, 99u, &reuseReason),
               "reuse preflight rejects stale observed generation on warmed history");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseRejectReason::StaleGeneration,
               "stale observed generation reports StaleGeneration");

               "reuse preflight rejects invalidated history");
               "invalidated history reports HistoryNotWarmed");

                               fuse::renderer::TaaHistoryReuseRejectReason::HistoryNotWarmed),
                           "history_not_warmed") == 0,
               "HistoryNotWarmed reuse label");

    history.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testResolveSurfaceAndPreflightHelpers() {
    fuse::renderer::TaaResolveDesc desc{};
    desc.width = 64;
    desc.height = 64;
    expectTrue(!fuse::renderer::taaResolveSurfacesSatisfied(desc), "null surfaces not satisfied");

    desc.surfaces.current_frame = reinterpret_cast<void*>(0x1);
    expectTrue(!fuse::renderer::taaResolveSurfacesSatisfied(desc), "missing output not satisfied");

    desc.surfaces.output = reinterpret_cast<void*>(0x2);
    expectTrue(fuse::renderer::taaResolveSurfacesSatisfied(desc), "both colour surfaces satisfied");

    expectTrue(bootstrap != nullptr, "bootstrap allocated for resolve preflight helper test");



    expectTrue(history.init(resources, historyDesc), "history ready for resolve preflight helper test");

    expectTrue(fuse::renderer::canAttemptTaaResolve(desc, history), "valid desc can attempt resolve");
    expectTrue(!fuse::renderer::canAttemptTaaResolve(
                   fuse::renderer::TaaResolveDesc{.width = 0, .height = 64}, history),
               "invalid dimensions cannot attempt resolve");

    fuse::renderer::TaaResolveDesc prepared = desc;
    expectTrue(fuse::renderer::prepareTaaResolveDesc(prepared, history),
               "prepareTaaResolveDesc succeeds for valid desc");
    expectTrue(prepared.observed_history_generation == 0u, "prepare stamps observed generation");

    expectTrue(pass->canResolveFrame(desc), "pass canResolveFrame accepts valid desc");

    fuse::renderer::TaaResolveDesc preparedPass = desc;
    expectTrue(pass->prepareAndCanResolve(preparedPass), "pass prepareAndCanResolve succeeds");
    expectTrue(preparedPass.observed_history_generation == 0u, "pass prepare stamps generation");

    expectTrue(!fuse::renderer::taaHistoryWarmupComplete(history), "warmup incomplete before first resolve");
    expectTrue(fuse::renderer::taaHistoryReuseBlocked(history, 0u),
               "reuse blocked before warmup completes");

    expectTrue(fuse::renderer::taaResolveRequiresWarmup(desc, history),
               "resolve requires warmup before first successful resolve");

    expectTrue(!fuse::renderer::taaHistoryReuseBlocked(history, 0u),
               "reuse allowed when warmup complete and generation matches");
    expectTrue(!fuse::renderer::taaResolveRequiresWarmup(desc, history),
               "resolve no longer requires warmup after first resolve");

    expectTrue(!fuse::renderer::taaHistoryWarmupComplete(history), "invalidate clears warmup-complete state");
               "stale generation blocks reuse after invalidate");
    expectTrue(fuse::renderer::taaHistoryReuseBlocked(history, history.invalidateGeneration()),
               "warmup still required after invalidate even with current generation");

    expectTrue(!fuse::renderer::taaHistoryReuseBlocked(history, history.invalidateGeneration()),
               "current generation allows reuse after re-warm");


void testHistoryReuseShouldSkipAndReady() {
    fuse::renderer::TaaHistoryBuffer emptyHistory;
    expectTrue(fuse::renderer::shouldSkipTaaHistoryReuse(emptyHistory, 0u),
               "empty history should skip reuse");
    expectTrue(!fuse::renderer::taaHistoryReuseReady(emptyHistory, 0u),
               "empty history is not reuse-ready");
void testResolveBlendPreflightGuards() {
    fuse::renderer::TAAParams params{};
    params.blend_factor = 0.2f;

    fuse::renderer::TaaBlendWeights warmup{};
    expectTrue(fuse::renderer::preflightTaaBlendWeights(true, params, &warmup),
               "warmup blend preflight succeeds");
    expectNear(warmup.current, 1.f, 1e-5f, "warmup preflight uses full current weight");
    expectNear(warmup.history, 0.f, 1e-5f, "warmup preflight uses zero history weight");

    fuse::renderer::TaaBlendWeights steady{};
    expectTrue(fuse::renderer::preflightTaaBlendWeights(false, params, &steady),
               "steady blend preflight succeeds");
    expectNear(steady.current, 0.2f, 1e-5f, "steady preflight uses configured current weight");
    expectNear(steady.history, 0.8f, 1e-5f, "steady preflight uses history complement");

void testJitterSyncPreflightGuards() {
    using fuse::renderer::TaaJitterLayout;

    expectTrue(TaaJitterLayout::canSyncToFrameIndex(8u), "default sequence can sync");
    expectTrue(!TaaJitterLayout::canSyncToFrameIndex(0u), "zero-length sequence cannot sync");

    expectTrue(fuse::renderer::taaJitterSlotMatchesFrame(13u, 5u, 8u),
               "slot matches wrapped frame index");
    expectTrue(!fuse::renderer::taaJitterSlotMatchesFrame(13u, 6u, 8u),
               "slot mismatch detected for wrapped frame index");

    expectTrue(std::strcmp(fuse::renderer::taaJitterSyncRejectReasonLabel(
                               fuse::renderer::TaaJitterSyncRejectReason::None),
               "None jitter sync reject label");
                               fuse::renderer::TaaJitterSyncRejectReason::InvalidSequence),
                           "invalid_sequence") == 0,
               "InvalidSequence jitter sync reject label");
                               fuse::renderer::TaaJitterSyncRejectReason::InvalidViewport),
                           "invalid_viewport") == 0,
               "InvalidViewport jitter sync reject label");
                               fuse::renderer::TaaJitterSyncRejectReason::FrameIndexMismatch),
                           "frame_index_mismatch") == 0,
               "FrameIndexMismatch jitter sync reject label");

    fuse::renderer::TaaJitter jitter;
    expectTrue(jitter.isSyncedToFrameIndex(0u), "default jitter synced to frame zero");
    expectTrue(!jitter.isSyncedToFrameIndex(1u), "default jitter not synced to frame one");
    jitter.syncToFrameIndex(5u);
    expectTrue(!jitter.isSyncedToFrameIndex(6u), "jitter not synced to different frame");

    fuse::renderer::TaaJitterSyncRejectReason syncReason = fuse::renderer::TaaJitterSyncRejectReason::None;
    expectTrue(fuse::renderer::preflightTaaJitterSync(jitter, 5u, 128u, 128u, &syncReason),
               "preflight passes for synced jitter and valid viewport");
    expectTrue(syncReason == fuse::renderer::TaaJitterSyncRejectReason::None,
               "preflight sync reason is None");

    expectTrue(!fuse::renderer::preflightTaaJitterSync(jitter, 6u, 128u, 128u, &syncReason),
               "preflight rejects frame index mismatch");
    expectTrue(syncReason == fuse::renderer::TaaJitterSyncRejectReason::FrameIndexMismatch,
               "preflight reports FrameIndexMismatch");

    expectTrue(!fuse::renderer::preflightTaaJitterSync(jitter, 5u, 0u, 128u, &syncReason),
               "preflight rejects zero-width viewport");
    expectTrue(syncReason == fuse::renderer::TaaJitterSyncRejectReason::InvalidViewport,
               "preflight reports InvalidViewport");


    expectTrue(TaaJitterLayout::canSyncToFrameIndex(8u), "default sequence allows sync");
    expectTrue(!TaaJitterLayout::canSyncToFrameIndex(0u), "zero-length sequence blocks sync");

    expectTrue(jitter.canAdvance(), "default jitter can sync via advance path");
    expectTrue(!jitter.isSyncedToFrameIndex(3u), "jitter starts unsynced to frame 3");
    expectTrue(jitter.expectedSlotForFrameIndex(3u) == 3u, "expected slot for frame 3 is slot 3");

    jitter.syncToFrameIndex(3u);
    expectTrue(jitter.isSyncedToFrameIndex(3u), "jitter synced to frame 3");
    expectTrue(jitter.slotMatchesFrameIndex(3u), "jitter slot matches frame 3");
    expectTrue(!jitter.slotMatchesFrameIndex(4u), "jitter slot does not match frame 4");

    expectTrue(jitter.isSyncedToFrameIndex(11u), "jitter synced to wrapped frame 11");
    expectTrue(jitter.slotMatchesFrameIndex(11u), "jitter slot matches wrapped frame 11");
    expectTrue(jitter.index() == 3u, "wrapped frame 11 maps to slot 3");

    expectTrue(jitter.slotMatchesFrameIndex(0u), "reset jitter matches frame 0");
    expectTrue(!jitter.slotMatchesFrameIndex(1u), "reset jitter does not match frame 1");

void testResolveBlendPreflight() {

    jitter.syncToFrameIndex(4u);
    expectTrue(jitter.isSyncedToFrameIndex(4u), "jitter synced after syncToFrameIndex");
    expectTrue(!jitter.isSyncedToFrameIndex(5u), "jitter not synced to different frame");

    expectTrue(fuse::renderer::taaJitterSyncPreflight(jitter, 4u, 128u, 128u, &syncReason),
               "jitter sync preflight passes synced state");
    expectTrue(syncReason == fuse::renderer::TaaJitterSyncRejectReason::None, "synced jitter reason is None");

    expectTrue(!fuse::renderer::taaJitterSyncPreflight(jitter, 3u, 128u, 128u, &syncReason),
               "jitter sync preflight rejects frame mismatch");
               "frame mismatch sync reason");

    expectTrue(!fuse::renderer::taaJitterSyncPreflight(jitter, 4u, 0u, 128u, &syncReason),
               "jitter sync preflight rejects zero width");
               "invalid viewport sync reason");

    const fuse::math::Vec2 currentNdc = jitter.currentNdcOffset(128u, 128u);
    const fuse::math::Vec2 expectedNdc = TaaJitterLayout::ndcOffsetForFrameIndex(4u, 128u, 128u, 8u);
    expectTrue(TaaJitterLayout::ndcOffsetsMatch(currentNdc, expectedNdc), "synced jitter NDC matches layout");

               "FrameIndexMismatch sync label");

    expectTrue(pass->isJitterSyncedToFrameIndex(2u), "pass jitter synced to frame index");

    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    TAA_SKIP_UNLESS_VULKAN_READY(bootstrap);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for should-skip reuse test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for resolve blend preflight test");
    auto bootstrap = createTaaTestBootstrap();
    if (bootstrap == nullptr) {
        std::fprintf(stderr, "SKIP: bootstrap allocated for should-skip reuse test (Vulkan device unavailable)\n");
        return;
    }



    expectTrue(history.init(resources, historyDesc), "history ready for should-skip reuse test");
    expectTrue(history.warmupFramesRemaining() == 1u, "history buffer warmup frames remaining is one");
    expectTrue(fuse::renderer::shouldSkipTaaHistoryReuse(history, 0u),
               "unwarmed history should skip reuse");
    expectTrue(!history.reuseReady(0u), "unwarmed history buffer is not reuse-ready");

    expectTrue(!fuse::renderer::tryPreflightTaaHistoryReuse(history, 0u, reason),
               "tryPreflight fails for unwarmed history");
               "tryPreflight reason is NotWarm for unwarmed history");

    expectTrue(history.warmupFramesRemaining() == 0u, "warmed history buffer has zero warmup frames");
    expectTrue(!fuse::renderer::shouldSkipTaaHistoryReuse(history, 0u),
               "warmed history should not skip reuse");
    expectTrue(fuse::renderer::taaHistoryReuseReady(history, 0u), "warmed history is reuse-ready");
    expectTrue(history.reuseReady(0u), "history buffer reuseReady after warmup");
    expectTrue(fuse::renderer::tryPreflightTaaHistoryReuse(history, 0u, reason),
               "tryPreflight passes for warmed history");
               "tryPreflight reason is None for warmed history");

               "stale generation should skip reuse");
    expectTrue(!fuse::renderer::taaHistoryReuseReady(history, 0u),
               "stale generation is not reuse-ready");
    expectTrue(history.init(resources, historyDesc), "history ready for resolve blend preflight test");


    fuse::renderer::TaaBlendWeights weights{};
    expectTrue(fuse::renderer::preflightTaaResolveBlendWeights(desc, history, &weights),
               "blend-weight preflight passes before first resolve");
    expectNear(weights.current, 1.f, 1e-5f, "preflight warmup blend uses full current weight");
    expectNear(weights.history, 0.f, 1e-5f, "preflight warmup blend uses zero history weight");

    fuse::renderer::TaaResolveBlendPreflight snapshot{};
    expectTrue(fuse::renderer::preflightTaaResolveBlend(desc, history, &snapshot),
               "resolve blend preflight passes before first resolve");
    expectTrue(snapshot.resolve_would_pass, "snapshot resolve preflight passes");
    expectTrue(snapshot.blend_weights_valid, "snapshot blend weights valid");
    expectTrue(!snapshot.history_blend_allowed, "snapshot history blend blocked on first frame");
    expectTrue(snapshot.passes(), "snapshot passes before first resolve");

    fuse::renderer::TaaResolve resolve;
    expectTrue(resolve.resolve(desc, history), "first resolve warms history");

               "blend-weight preflight passes after warmup");
    expectNear(weights.current, 0.2f, 1e-5f, "preflight steady blend uses configured current weight");
    expectNear(weights.history, 0.8f, 1e-5f, "preflight steady blend uses history complement");

               "resolve blend preflight passes after warmup");
    expectTrue(snapshot.history_blend_allowed, "snapshot history blend allowed after warmup");

    desc.width = 0;
    expectTrue(!fuse::renderer::preflightTaaResolveBlend(desc, history, &snapshot),
               "resolve blend preflight rejects invalid dimensions");
    expectTrue(!snapshot.resolve_would_pass, "snapshot resolve preflight fails");
    expectTrue(snapshot.resolve_skip_reason == fuse::renderer::TaaResolveSkipReason::InvalidDimensions,
               "snapshot records InvalidDimensions");
    desc.params.blend_factor = 0.3f;

    fuse::renderer::TaaBlendWeights warmupWeights =
        fuse::renderer::computeTaaResolveBlendWeights(desc, history);
    expectNear(warmupWeights.current, 1.f, 1e-5f, "blend preflight warmup uses full current weight");
    expectNear(warmupWeights.history, 0.f, 1e-5f, "blend preflight warmup uses zero history weight");
    expectTrue(fuse::renderer::taaBlendWeightsValid(warmupWeights), "warmup blend weights are valid");

    fuse::renderer::TaaBlendWeights computed{};
    expectTrue(fuse::renderer::preflightTaaResolveBlend(desc, history, &computed),
               "blend preflight passes for valid resolve desc");
    expectNear(computed.current, 1.f, 1e-5f, "preflight fills warmup current weight");
    expectTrue(fuse::renderer::taaResolveBlendPreflightPasses(desc, history),
               "blend preflight passes alias succeeds");

    const fuse::renderer::TaaBlendWeights steadyWeights =
    expectNear(steadyWeights.current, 0.3f, 1e-5f, "steady blend preflight uses configured current weight");
    expectNear(steadyWeights.history, 0.7f, 1e-5f, "steady blend preflight uses history complement");

    expectTrue(!fuse::renderer::preflightTaaResolveBlend(desc, history, &computed),
               "blend preflight rejects invalid dimensions");
    expectTrue(!fuse::renderer::taaResolveBlendPreflightPasses(desc, history),
               "blend preflight alias rejects invalid dimensions");

        fuse::renderer::computeTaaResolveBlendPreflight(desc, history);
    expectNear(warmupWeights.current, 1.f, 1e-5f, "blend preflight uses full current on warmup frame");
    expectNear(warmupWeights.history, 0.f, 1e-5f, "blend preflight uses zero history on warmup frame");

    fuse::renderer::TaaBlendWeights projected{};
    expectTrue(fuse::renderer::preflightTaaResolveBlend(desc, history, &projected),
               "preflightTaaResolveBlend succeeds before first resolve");
    expectNear(projected.current, warmupWeights.current, 1e-5f, "preflight fills projected current weight");
    expectNear(projected.history, warmupWeights.history, 1e-5f, "preflight fills projected history weight");

    expectNear(steadyWeights.current, 0.3f, 1e-5f, "blend preflight uses configured current after warmup");
    expectNear(steadyWeights.history, 0.7f, 1e-5f, "blend preflight uses history complement after warmup");
    expectTrue(fuse::renderer::preflightTaaResolveBlend(desc, history),
               "preflightTaaResolveBlend succeeds after warmup");

    expectTrue(!fuse::renderer::preflightTaaResolveBlend(desc, history, &projected),
               "preflightTaaResolveBlend rejects invalid dimensions");
    desc.observed_history_generation = 0u;

    expectTrue(fuse::renderer::tryComputeTaaResolveBlendWeights(desc, history, weights),
               "blend-weight preflight succeeds for valid resolve desc");
    expectNear(weights.current, 1.f, 1e-5f, "warmup blend preflight uses full current weight");
    expectNear(weights.history, 0.f, 1e-5f, "warmup blend preflight uses zero history weight");
    expectTrue(fuse::renderer::preflightTaaResolveBlend(desc, history, &weights),
               "resolve-blend preflight passes before first resolve");
    expectTrue(fuse::renderer::taaResolveHistoryBlendPreflightPasses(desc, history),
               "history-blend preflight passes before first resolve");

               "blend-weight preflight succeeds after warmup");
    expectNear(weights.current, 0.3f, 1e-5f, "steady blend preflight uses configured current weight");
    expectNear(weights.history, 0.7f, 1e-5f, "steady blend preflight uses history complement");
               "history-blend preflight passes when reuse is allowed");

    expectTrue(!fuse::renderer::taaResolveHistoryBlendPreflightPasses(desc, history),
               "history-blend preflight fails when observed generation is stale");

    desc.observed_history_generation = history.invalidateGeneration();
               "history-blend preflight passes with current generation after invalidate");

    expectTrue(!fuse::renderer::tryComputeTaaResolveBlendWeights(desc, history, weights),
               "blend-weight preflight rejects invalid dimensions");
    expectTrue(!fuse::renderer::preflightTaaResolveBlend(desc, history),
               "resolve-blend preflight rejects invalid dimensions");


    expectTrue(fuse::renderer::computeTaaResolveFirstFrame(history),
               "first resolve frame before warmup");
    expectTrue(!fuse::renderer::taaResolveWouldUseHistoryBlend(desc, history),
               "history blend not used on warmup frame");

    fuse::renderer::TaaBlendPreflightRejectReason blendReason =
        fuse::renderer::TaaBlendPreflightRejectReason::None;
    expectTrue(!fuse::renderer::taaResolveBlendPreflight(desc, history, &blendReason),
               "blend preflight rejects warmup frame");
    expectTrue(blendReason == fuse::renderer::TaaBlendPreflightRejectReason::WarmupRequired,
               "warmup frame reports WarmupRequired");

    expectTrue(!fuse::renderer::computeTaaResolveFirstFrame(history),
               "not first frame after warmup");
    expectTrue(fuse::renderer::taaResolveBlendPreflight(desc, history, &blendReason),
               "blend preflight passes warmed history");
    expectTrue(fuse::renderer::taaResolveWouldUseHistoryBlend(desc, history),
               "history blend used after warmup");

    blendReason = fuse::renderer::TaaBlendPreflightRejectReason::None;
               "blend preflight rejects after invalidate warmup");
               "post-invalidate blend reports WarmupRequired");

               "blend preflight rejects stale generation");
    expectTrue(blendReason == fuse::renderer::TaaBlendPreflightRejectReason::StaleGeneration,
               "stale generation blend reason");

    desc.observed_history_generation = fuse::renderer::kTaaResolveNoHistoryGeneration;
               "blend preflight bypasses generation guard with sentinel");

    expectTrue(std::strcmp(fuse::renderer::taaBlendPreflightRejectReasonLabel(
                               fuse::renderer::TaaBlendPreflightRejectReason::WarmupRequired),
                           "warmup_required") == 0,
               "WarmupRequired blend label");

    expectTrue(pass->init(resources), "TaaPass initialized for blend preflight test");
    expectTrue(!pass->preflightHistoryReuse(0u), "pass reuse preflight rejects unwarmed history");
    expectTrue(!pass->preflightResolveBlend(desc), "pass blend preflight rejects unwarmed history");

    resolveDesc.surfaces.current_frame = reinterpret_cast<void*>(0x1);
    resolveDesc.surfaces.output = reinterpret_cast<void*>(0x2);
    expectTrue(pass->preflightHistoryReuse(0u), "pass reuse preflight passes warmed history");
    expectTrue(pass->preflightResolveBlend(resolveDesc), "pass blend preflight passes warmed history");


void testJitterGuardRejectReasons() {

    expectTrue(std::strcmp(fuse::renderer::taaJitterGuardRejectReasonLabel(
                               fuse::renderer::TaaJitterGuardRejectReason::None),
               "None jitter guard reject label");
                               fuse::renderer::TaaJitterGuardRejectReason::InvalidSequence),
               "InvalidSequence jitter guard reject label");
                               fuse::renderer::TaaJitterGuardRejectReason::InvalidViewport),
               "InvalidViewport jitter guard reject label");

    expectTrue(fuse::renderer::classifyTaaJitterSyncReject(8u) ==
                   fuse::renderer::TaaJitterGuardRejectReason::None,
               "valid sequence passes sync classify");
    expectTrue(fuse::renderer::classifyTaaJitterSyncReject(0u) ==
                   fuse::renderer::TaaJitterGuardRejectReason::InvalidSequence,
               "zero sequence fails sync classify");

    expectTrue(fuse::renderer::classifyTaaJitterNdcReject(1920u, 1080u, 8u) ==
               "valid viewport passes NDC classify");
    expectTrue(fuse::renderer::classifyTaaJitterNdcReject(0u, 1080u, 8u) ==
                   fuse::renderer::TaaJitterGuardRejectReason::InvalidViewport,
               "zero width fails NDC classify");
    expectTrue(fuse::renderer::classifyTaaJitterNdcReject(1920u, 1080u, 0u) ==
               "invalid sequence fails NDC classify before viewport");

    fuse::renderer::TaaJitterGuardRejectReason rejectReason =
        fuse::renderer::TaaJitterGuardRejectReason::None;
    expectTrue(fuse::renderer::preflightTaaJitterSync(5u, 8u, &rejectReason),
               "preflightTaaJitterSync passes for valid sequence");
    expectTrue(rejectReason == fuse::renderer::TaaJitterGuardRejectReason::None,
               "preflightTaaJitterSync reject reason is None");
    expectTrue(fuse::renderer::tryPreflightTaaJitterSync(5u, 8u, rejectReason),
               "tryPreflightTaaJitterSync passes for valid sequence");
    expectTrue(!fuse::renderer::preflightTaaJitterSync(5u, 0u, &rejectReason),
               "preflightTaaJitterSync rejects invalid sequence");
    expectTrue(rejectReason == fuse::renderer::TaaJitterGuardRejectReason::InvalidSequence,
               "preflightTaaJitterSync reject reason is InvalidSequence");

    expectTrue(fuse::renderer::preflightTaaJitterNdc(128u, 128u, 8u, &rejectReason),
               "preflightTaaJitterNdc passes for valid viewport");
    expectTrue(!fuse::renderer::preflightTaaJitterNdc(0u, 128u, 8u, &rejectReason),
               "preflightTaaJitterNdc rejects zero width");
    expectTrue(rejectReason == fuse::renderer::TaaJitterGuardRejectReason::InvalidViewport,
               "preflightTaaJitterNdc reject reason is InvalidViewport");

    fuse::math::Vec2 ndcOut{};
    expectTrue(jitter.currentNdcOffsetIfReady(128u, 128u, ndcOut),
               "currentNdcOffsetIfReady succeeds for valid viewport");
    const fuse::math::Vec2 directNdc = jitter.currentNdcOffset(128u, 128u);
    expectNear(ndcOut.x, directNdc.x, 1e-6f, "currentNdcOffsetIfReady matches currentNdcOffset X");
    expectNear(ndcOut.y, directNdc.y, 1e-6f, "currentNdcOffsetIfReady matches currentNdcOffset Y");
    expectTrue(!jitter.currentNdcOffsetIfReady(0u, 128u, ndcOut),
               "currentNdcOffsetIfReady fails for zero width");

void testResolveBlendTryAndShouldSkip() {
void testTaaPassPreflightGuards() {
void testTaaPassJitterSyncAndBlendPreflight() {
void testTaaPassWarmupAndBlendPreflight() {
void testTaaPassSyncWarmupAndBlendPreflight() {

    pass->syncJitterToFrameIndex(4u);
    expectTrue(pass->isJitterSyncedToFrameIndex(4u), "pass reports synced jitter");
    expectTrue(!pass->isJitterSyncedToFrameIndex(5u), "pass reports jitter mismatch");

    expectTrue(pass->classifyHistoryReuseReject(0u) ==
               "pass classifies NotReady before init");
    expectTrue(pass->canSyncJitterToFrameIndex(), "pass jitter can sync before init");
    expectTrue(!pass->isJitterSyncedToFrameIndex(2u), "pass jitter not synced before alignment");

    expectTrue(pass->isJitterSyncedToFrameIndex(2u), "pass jitter synced to frame 2");
    expectTrue(!pass->isJitterSyncedToFrameIndex(3u), "pass jitter no longer synced to frame 3");
    expectTrue(!pass->historyWarmupRequired(), "pass warmup not required before init");
    expectTrue(!pass->historyWarmupComplete(), "pass warmup incomplete before init");
    expectTrue(!pass->isJitterSyncedToFrameIndex(4u), "pass jitter not synced before explicit sync");
    pass->syncJitterToFrameIndex(11u);
    expectTrue(pass->isJitterSyncedToFrameIndex(11u), "pass reports jitter synced after syncJitterToFrameIndex");
    expectTrue(!pass->isJitterSyncedToFrameIndex(10u), "pass reports jitter not synced to prior frame");

    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    TAA_SKIP_UNLESS_VULKAN_READY(bootstrap);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for blend try/should-skip test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass preflight guard test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass jitter/blend preflight test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass warmup/blend preflight test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass sync/warmup/blend preflight test");
    auto bootstrap = createTaaTestBootstrap();
    if (bootstrap == nullptr) {
        std::fprintf(stderr, "SKIP: bootstrap allocated for blend try/should-skip test (Vulkan device unavailable)\n");
        return;
    }



    expectTrue(history.init(resources, historyDesc), "history ready for blend try/should-skip test");

    desc.params.blend_factor = 0.35f;

    expectTrue(!fuse::renderer::shouldSkipTaaResolveBlend(desc, history),
               "warmup blend should not be skipped");
    expectTrue(fuse::renderer::preflightTaaResolveBlendWeights(desc, history),
               "warmup blend preflight passes");

    expectTrue(fuse::renderer::tryPreflightTaaResolveBlendWeights(desc, history, rejectReason),
               "tryPreflightTaaResolveBlendWeights passes for warmup");
               "tryPreflight blend reject reason is None for warmup");

    expectTrue(fuse::renderer::tryComputeTaaResolveBlendWeights(desc, history, weights, rejectReason),
               "tryComputeTaaResolveBlendWeights passes for warmup");
    expectNear(weights.current, 1.f, 1e-5f, "tryCompute warmup current weight is full");
    expectNear(weights.history, 0.f, 1e-5f, "tryCompute warmup history weight is zero");

               "tryComputeTaaResolveBlendWeights passes after warmup");
    expectNear(weights.current, 0.35f, 1e-5f, "tryCompute steady current weight");
    expectNear(weights.history, 0.65f, 1e-5f, "tryCompute steady history weight");
               "steady blend should not be skipped");


void testTaaPassDeepenGuardWrappers() {
    passDesc.params.blend_factor = 0.2f;

    expectTrue(pass->warmupFramesRemaining() == 1u, "pass warmup frames remaining before init");
    expectTrue(pass->shouldSkipHistoryReuse(0u), "pass should skip reuse before init");
    expectTrue(!pass->historyReuseReady(0u), "pass history not reuse-ready before init");

    expectTrue(pass->currentJitterNdcIfReady(ndcOut), "pass currentJitterNdcIfReady before init");
    const fuse::math::Vec2 directNdc = pass->currentJitterNdc();
    expectNear(ndcOut.x, directNdc.x, 1e-6f, "pass currentJitterNdcIfReady matches currentJitterNdc X");

    fuse::renderer::TaaJitterGuardRejectReason jitterReject =
    expectTrue(pass->preflightJitterSync(4u, &jitterReject), "pass preflightJitterSync passes");
    expectTrue(jitterReject == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass preflightJitterSync reject reason is None");

    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    TAA_SKIP_UNLESS_VULKAN_READY(bootstrap);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass deepen wrapper test");
    auto bootstrap = createTaaTestBootstrap();
    if (bootstrap == nullptr) {
        std::fprintf(stderr, "SKIP: bootstrap allocated for pass deepen wrapper test (Vulkan device unavailable)\n");
        return;
    }


    expectTrue(pass->init(resources), "TaaPass initialized for deepen wrapper test");
    expectTrue(pass->warmupFramesRemaining() == 1u, "pass warmup frames remaining after init");
    expectTrue(pass->init(resources), "TaaPass initialized for preflight guard test");

               "pass classifies NeedsWarmup before first resolve");
    expectTrue(pass->init(resources), "TaaPass initialized for jitter/blend preflight test");
    expectTrue(pass->init(resources), "TaaPass initialized for warmup/blend preflight test");
    expectTrue(pass->historyWarmupRequired(), "pass requires warmup after init");
    expectTrue(!pass->historyWarmupComplete(), "pass warmup incomplete after init");

    expectTrue(pass->isJitterSyncedToFrameIndex(4u), "pass reports jitter synced after sync");
    expectTrue(pass->init(resources), "TaaPass initialized for sync/warmup/blend preflight test");
    expectTrue(!pass->historyWarmupComplete(), "pass warmup incomplete before first resolve");

    resolveDesc.width = 128;
    resolveDesc.height = 128;
    expectTrue(!pass->shouldSkipResolveBlend(resolveDesc), "pass should not skip blend before warmup resolve");

    expectTrue(pass->warmupFramesRemaining() == 0u, "pass warmup frames remaining after resolve");
    expectTrue(!pass->shouldSkipHistoryReuse(0u), "pass should not skip reuse after warmup");
    expectTrue(pass->historyReuseReady(0u), "pass history reuse-ready after warmup");
    expectTrue(!pass->shouldSkipResolveBlend(resolveDesc), "pass should not skip blend after warmup");

    expectTrue(pass->shouldSkipHistoryReuse(0u), "pass should skip reuse after invalidate");
    expectTrue(!pass->historyReuseReady(0u), "pass history not reuse-ready after invalidate");

    fuse::renderer::TaaPassDesc zeroWidthDesc{};
    zeroWidthDesc.width = 0;
    zeroWidthDesc.height = 128;
    auto zeroPass = fuse::renderer::TaaPass::create(zeroWidthDesc);
    expectTrue(!zeroPass->currentJitterNdcIfReady(ndcOut), "zero-width pass blocks currentJitterNdcIfReady");
    expectTrue(zeroPass->preflightJitterSync(0u), "zero-width pass jitter sync still valid for sequence");


    desc.params = params;

    fuse::renderer::TaaBlendWeights blend{};
    expectTrue(fuse::renderer::preflightTaaResolveBlend(desc, history, &blend),
    expectNear(blend.current, 1.f, 1e-5f, "first resolve blend preflight uses warmup weights");

    expectNear(blend.current, 0.2f, 1e-5f, "steady resolve blend preflight uses configured weight");

    expectTrue(!fuse::renderer::preflightTaaResolveBlend(desc, history, &blend),


    fuse::renderer::TaaBlendWeights warmupPassBlend{};
    expectTrue(pass->preflightResolveBlend(desc, &warmupPassBlend), "pass preflightResolveBlend succeeds before warmup");
    expectNear(warmupPassBlend.current, 1.f, 1e-5f, "pass blend preflight uses warmup weights before first resolve");

    expectTrue(pass->resolveFrame(desc), "pass resolve warms pass-owned history");
    fuse::renderer::TaaBlendWeights passBlend{};
    expectTrue(pass->preflightResolveBlend(desc, &passBlend), "pass preflightResolveBlend succeeds after warmup");
    expectNear(passBlend.current, 0.2f, 1e-5f, "pass blend preflight uses steady weights after warmup");
    resolveDesc.params.blend_factor = 0.3f;

    expectTrue(pass->preflightResolveBlend(resolveDesc, &snapshot), "pass preflight passes before resolve");
    expectTrue(!snapshot.history_blend_allowed, "pass preflight blocks history blend on first frame");

    expectTrue(pass->preflightResolveBlend(resolveDesc, &snapshot), "pass preflight passes after warmup");
    expectTrue(snapshot.history_blend_allowed, "pass preflight allows history blend after warmup");
    resolveDesc.params.blend_factor = 0.2f;

    expectTrue(pass->preflightResolveBlend(resolveDesc, &weights),
    expectNear(weights.current, 1.f, 1e-5f, "pass blend preflight warmup current weight");

    expectTrue(pass->resolveFrame(resolveDesc), "initial resolve warms history for blend preflight");
    expectNear(weights.current, 0.2f, 1e-5f, "pass blend preflight steady current weight");

    expectTrue(!pass->preflightResolveBlend(resolveDesc, &weights),
               "pass blend preflight rejects dimension mismatch");

    expectTrue(pass->preflightResolveBlend(resolveDesc, &projected),
               "pass resolve blend preflight passes before first resolve");
    expectNear(projected.current, 1.f, 1e-5f, "pass preflight projects warmup current weight");

    expectTrue(pass->resolveFrame(resolveDesc), "initial resolve completes warmup");
    expectTrue(pass->historyWarmupComplete(), "pass warmup complete after first resolve");
    expectTrue(!pass->historyWarmupRequired(), "pass no longer requires warmup");

               "pass resolve blend preflight passes after warmup");
    expectNear(projected.current, 0.2f, 1e-5f, "pass preflight projects steady current weight");
    expectNear(projected.history, 0.8f, 1e-5f, "pass preflight projects steady history weight");
               "pass resolve-blend preflight passes before first resolve");
    expectNear(weights.current, 1.f, 1e-5f, "pass preflight warmup blend uses full current weight");

               "pass resolve-blend preflight passes after warmup");
    expectNear(weights.current, 0.2f, 1e-5f, "pass preflight steady blend uses configured current weight");

                   fuse::renderer::TaaHistoryReuseBlockReason::StaleGeneration,

    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testHistoryWarmupPreflightGuards() {
    fuse::renderer::TaaHistoryBuffer emptyHistory;
    expectTrue(!fuse::renderer::preflightTaaHistoryWarmup(emptyHistory),
               "unallocated history fails warmup preflight");
    expectTrue(!fuse::renderer::taaHistoryIsWarmupFrame(emptyHistory),
               "unallocated history is not a warmup frame");
    expectTrue(!fuse::renderer::preflightTaaHistoryReuse(emptyHistory, 0u),
               "unallocated history fails reuse preflight");

    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for warmup preflight test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaHistoryBuffer history;
    fuse::renderer::TaaHistoryBufferDesc historyDesc{64, 64};
    expectTrue(history.init(resources, historyDesc), "history ready for warmup preflight test");
    expectTrue(fuse::renderer::preflightTaaHistoryWarmup(history), "allocated history passes warmup preflight");
    expectTrue(fuse::renderer::taaHistoryIsWarmupFrame(history), "fresh history is warmup frame");
    expectTrue(history.isWarmupFrame(), "isWarmupFrame mirrors taaHistoryIsWarmupFrame");
    expectTrue(!fuse::renderer::preflightTaaHistoryReuse(history, 0u),
               "unwarmed history fails reuse preflight");
    expectTrue(!history.preflightReuse(0u), "preflightReuse false before first resolve");

    history.markResolved();
    expectTrue(!fuse::renderer::taaHistoryIsWarmupFrame(history), "warmed history is not warmup frame");
    expectTrue(fuse::renderer::preflightTaaHistoryReuse(history, 0u),
               "warmed history passes reuse preflight");
    expectTrue(history.preflightReuse(0u), "preflightReuse true after first resolve");

    history.invalidateHistory();
    expectTrue(!fuse::renderer::preflightTaaHistoryReuse(history, 0u),
               "stale generation fails reuse preflight");
    expectTrue(!history.preflightReuse(history.invalidateGeneration()),
               "invalidated history fails reuse preflight even with current generation");
    history.markResolved();
    expectTrue(history.preflightReuse(history.invalidateGeneration()),
               "current generation passes reuse preflight after re-warm");

    history.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testJitterSyncPreflightGuards() {
    using fuse::renderer::TaaJitterLayout;

    expectTrue(TaaJitterLayout::expectedSlotForMonotonicFrame(13u, 8u) == 5u,
               "expectedSlotForMonotonicFrame wraps monotonic counter");
    expectTrue(TaaJitterLayout::monotonicFrameMatchesSlot(13u, 5u, 8u),
               "monotonicFrameMatchesSlot accepts expected slot");
    expectTrue(!TaaJitterLayout::monotonicFrameMatchesSlot(13u, 6u, 8u),
               "monotonicFrameMatchesSlot rejects wrong slot");

    fuse::renderer::TaaJitter jitter;
    expectTrue(!jitter.isSyncedToFrameIndex(5u), "jitter not synced before sync call");
    expectTrue(jitter.syncToFrameIndexIfReady(5u), "syncToFrameIndexIfReady succeeds for valid sequence");
    expectTrue(jitter.isSyncedToFrameIndex(5u), "jitter synced after syncToFrameIndexIfReady");
    expectTrue(jitter.monotonicFrameIndex() == 5u, "syncToFrameIndexIfReady sets monotonic counter");

    jitter.syncToFrameIndex(13u);
    expectTrue(jitter.isSyncedToFrameIndex(13u), "isSyncedToFrameIndex true after direct sync");
    expectTrue(!jitter.isSyncedToFrameIndex(14u), "isSyncedToFrameIndex false for different frame");

    fuse::renderer::TaaJitterDesc invalidDesc{};
    invalidDesc.sequence_length = 0u;
    fuse::renderer::TaaJitter fallbackJitter(invalidDesc);
    expectTrue(fallbackJitter.sequenceLength() == 8u, "invalid desc falls back to default sequence length");
    expectTrue(fallbackJitter.syncToFrameIndexIfReady(3u), "syncToFrameIndexIfReady succeeds after fallback length");
    expectTrue(fallbackJitter.isSyncedToFrameIndex(3u), "isSyncedToFrameIndex true after guarded sync");
}

void testResolveBlendPreflightGuards() {
    fuse::renderer::TaaHistoryBuffer emptyHistory;
    fuse::renderer::TaaResolveDesc desc{};
    fuse::renderer::TaaResolveBlendPreflightRejectReason rejectReason =
        fuse::renderer::TaaResolveBlendPreflightRejectReason::None;
    expectTrue(!fuse::renderer::preflightTaaResolveBlend(desc, emptyHistory, &rejectReason),
               "blend preflight rejects unallocated history");
    expectTrue(rejectReason == fuse::renderer::TaaResolveBlendPreflightRejectReason::HistoryNotReady,
               "unallocated history reports HistoryNotReady");
    expectTrue(std::strcmp(fuse::renderer::taaResolveBlendPreflightRejectReasonLabel(
                               fuse::renderer::TaaResolveBlendPreflightRejectReason::HistoryNotReady),
                           "history_not_ready") == 0,
               "HistoryNotReady blend preflight label");

    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for blend preflight test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaHistoryBuffer history;
    fuse::renderer::TaaHistoryBufferDesc historyDesc{64, 64};
    expectTrue(history.init(resources, historyDesc), "history ready for blend preflight test");

    desc.params.blend_factor = 0.2f;
    expectTrue(fuse::renderer::preflightTaaResolveBlend(desc, history, &rejectReason),
               "warmup frame passes blend preflight");
    expectTrue(rejectReason == fuse::renderer::TaaResolveBlendPreflightRejectReason::None,
               "warmup frame blend preflight reason is None");
    expectTrue(fuse::renderer::diagnoseTaaResolveBlendPreflight(desc, history) ==
                   fuse::renderer::TaaResolveBlendPreflightRejectReason::None,
               "diagnose returns None on warmup frame");

    history.markResolved();
    expectTrue(fuse::renderer::preflightTaaResolveBlend(desc, history, &rejectReason),
               "warmed history passes blend preflight");
    expectTrue(fuse::renderer::taaResolveAppliesHistoryBlend(desc, history),
               "warmed history applies history blend");

    history.invalidateHistory();
    desc.observed_history_generation = 0u;
    expectTrue(fuse::renderer::preflightTaaResolveBlend(desc, history, &rejectReason),
               "stale generation still passes blend preflight with zeroed history weight");
    expectTrue(!fuse::renderer::taaResolveAppliesHistoryBlend(desc, history),
               "stale generation does not apply history blend");

    history.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testTaaPassPreflightGuards() {
    fuse::renderer::TaaPassDesc passDesc{};
    passDesc.width = 64;
    passDesc.height = 64;
    passDesc.params.blend_factor = 0.3f;

    auto pass = fuse::renderer::TaaPass::create(passDesc);
    expectTrue(!pass->isWarmupResolveFrame(), "pass does not report warmup frame before history init");

    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass preflight test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);
    expectTrue(pass->init(resources), "TaaPass initialized for preflight test");
    expectTrue(pass->isWarmupResolveFrame(), "pass reports warmup frame before first resolve");
    expectTrue(!pass->preflightHistoryReuse(0u), "pass reuse preflight false before warmup");

    fuse::renderer::TaaResolveDesc resolveDesc{};
    resolveDesc.params = passDesc.params;
    fuse::renderer::TaaResolveBlendPreflightRejectReason rejectReason =
        fuse::renderer::TaaResolveBlendPreflightRejectReason::None;
    expectTrue(pass->preflightResolveBlend(resolveDesc, &rejectReason),
               "pass blend preflight passes on warmup frame");
    expectTrue(rejectReason == fuse::renderer::TaaResolveBlendPreflightRejectReason::None,
               "pass blend preflight reason is None on warmup frame");

    expectTrue(pass->syncJitterToFrameIndexIfReady(7u), "pass guarded jitter sync succeeds");
    expectTrue(pass->isJitterSyncedTo(7u), "pass jitter synced after guarded sync");
    expectTrue(!pass->isJitterSyncedTo(8u), "pass jitter not synced to different frame");

    resolveDesc.width = 64;
    resolveDesc.height = 64;
    resolveDesc.surfaces.current_frame = reinterpret_cast<void*>(0x10);
    resolveDesc.surfaces.output = reinterpret_cast<void*>(0x20);
    expectTrue(pass->resolveFrame(resolveDesc), "initial resolve warms pass history");
    expectTrue(!pass->isWarmupResolveFrame(), "pass no longer reports warmup frame");
    expectTrue(pass->preflightHistoryReuse(0u), "pass reuse preflight true after warmup");
    expectTrue(pass->preflightResolveBlend(resolveDesc, &rejectReason),
               "pass blend preflight passes after warmup");

    pass->destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testJitterIndexMatchesFrameIndex() {
    using fuse::renderer::TaaJitterLayout;

    expectTrue(TaaJitterLayout::jitterIndexMatchesFrameIndex(0u, 0u, 8u),
               "frame zero matches slot zero");
    expectTrue(TaaJitterLayout::jitterIndexMatchesFrameIndex(5u, 5u, 8u),
               "frame five matches slot five");
    expectTrue(TaaJitterLayout::jitterIndexMatchesFrameIndex(13u, 5u, 8u),
               "wrapped frame thirteen matches slot five");
    expectTrue(!TaaJitterLayout::jitterIndexMatchesFrameIndex(5u, 3u, 8u),
               "mismatched frame and slot rejected");
    expectTrue(!TaaJitterLayout::jitterIndexMatchesFrameIndex(0u, 0u, 0u),
               "invalid sequence rejects index match");
}

void testJitterSyncIfReadyGuards() {
    fuse::renderer::TaaJitter jitter;
    expectTrue(jitter.syncToFrameIndexIfReady(7u), "syncToFrameIndexIfReady succeeds for valid sequence");
    expectTrue(jitter.isSyncedToFrameIndex(7u), "jitter reports synced after guarded sync");
    expectTrue(jitter.index() == 7u, "guarded sync sets slot");
    expectTrue(jitter.monotonicFrameIndex() == 7u, "guarded sync sets monotonic counter");

    expectTrue(jitter.syncToFrameIndexIfReady(15u), "guarded sync wraps large frame index");
    expectTrue(jitter.isSyncedToFrameIndex(15u), "jitter reports synced after wrapped guarded sync");
    expectTrue(jitter.index() == 7u, "wrapped guarded sync maps to slot seven");

    expectTrue(!jitter.isSyncedToFrameIndex(7u), "stale frame index fails sync check");

    fuse::renderer::TaaJitterDesc invalidDesc{};
    invalidDesc.sequence_length = 0u;
    fuse::renderer::TaaJitter fallbackJitter(invalidDesc);
    expectTrue(fallbackJitter.syncToFrameIndexIfReady(3u), "fallback jitter sync succeeds");
    expectTrue(fallbackJitter.isSyncedToFrameIndex(3u), "fallback jitter reports synced");

void testPreflightTaaHistoryReuse() {
    fuse::renderer::TaaHistoryBuffer emptyHistory;
    fuse::renderer::TaaHistoryReuseBlockReason reason = fuse::renderer::TaaHistoryReuseBlockReason::None;
    expectTrue(!fuse::renderer::preflightTaaHistoryReuse(emptyHistory, 0u, &reason),
               "empty history fails reuse preflight");
    expectTrue(reason == fuse::renderer::TaaHistoryReuseBlockReason::NotReady,
               "empty history reports NotReady");
void testHistoryWarmupStateGuards() {
    expectTrue(std::strcmp(fuse::renderer::taaHistoryWarmupStateLabel(
                               fuse::renderer::TaaHistoryWarmupState::NotReady),
                           "not_ready") == 0,
               "NotReady warmup state label");
                               fuse::renderer::TaaHistoryWarmupState::AwaitingFirstResolve),
                           "awaiting_first_resolve") == 0,
               "AwaitingFirstResolve warmup state label");
                               fuse::renderer::TaaHistoryWarmupState::Complete),
                           "complete") == 0,
               "Complete warmup state label");

    expectTrue(fuse::renderer::classifyTaaHistoryWarmupState(emptyHistory) ==
                   fuse::renderer::TaaHistoryWarmupState::NotReady,
               "empty history classified as NotReady");
    expectTrue(!fuse::renderer::taaHistoryWarmupComplete(emptyHistory),
               "empty history warmup is not complete");

    fuse::renderer::TaaHistoryWarmupState state = fuse::renderer::TaaHistoryWarmupState::Complete;
    expectTrue(!fuse::renderer::preflightTaaHistoryWarmup(emptyHistory, &state),
               "empty history warmup preflight fails");
    expectTrue(state == fuse::renderer::TaaHistoryWarmupState::NotReady,
               "empty history warmup preflight reports NotReady");

void testEffectiveObservedHistoryGeneration() {
void testWouldSkipAndTryHistoryReuseGuards() {
    expectTrue(fuse::renderer::wouldSkipTaaHistoryReuse(emptyHistory, 0u),
               "wouldSkip true for empty history");
               "warmup not complete for empty history");

    expectTrue(!fuse::renderer::tryPreflightTaaHistoryReuse(emptyHistory, 0u, reason),
               "tryPreflight fails for empty history");
               "tryPreflight reason is NotReady for empty history");
void testJitterSkipAndReadyGuards() {
void testJitterSyncShouldSkipAndReady() {
void testJitterSyncNdcShouldSkipAndReady() {
    expectTrue(fuse::renderer::taaJitterSyncReady(5u, 8u), "valid sequence is sync-ready");
    expectTrue(!fuse::renderer::shouldSkipTaaJitterSync(5u, 8u), "valid sequence should not skip sync");
    expectTrue(fuse::renderer::shouldSkipTaaJitterSync(5u, 0u), "invalid sequence should skip sync");
    expectTrue(!fuse::renderer::taaJitterSyncReady(5u, 0u), "invalid sequence is not sync-ready");

    expectTrue(fuse::renderer::taaJitterNdcReady(128u, 128u, 8u), "valid viewport is NDC-ready");
    expectTrue(!fuse::renderer::shouldSkipTaaJitterNdc(128u, 128u, 8u),
               "valid viewport should not skip NDC preflight");
    expectTrue(fuse::renderer::shouldSkipTaaJitterNdc(0u, 128u, 8u), "zero width should skip NDC preflight");
    expectTrue(!fuse::renderer::taaJitterNdcReady(0u, 128u, 8u), "zero width is not NDC-ready");
void testJitterShouldSkipAndTryNdc() {
    expectTrue(fuse::renderer::shouldSkipTaaJitterSync(5u, 0u),
               "shouldSkipTaaJitterSync true for invalid sequence");
    expectTrue(!fuse::renderer::shouldSkipTaaJitterSync(5u, 8u),
               "shouldSkipTaaJitterSync false for valid sequence");

    expectTrue(fuse::renderer::shouldSkipTaaJitterNdc(0u, 128u, 8u),
               "shouldSkipTaaJitterNdc true for zero width");
               "shouldSkipTaaJitterNdc false for valid viewport");

    fuse::renderer::TaaJitterGuardRejectReason rejectReason =
        fuse::renderer::TaaJitterGuardRejectReason::None;
    expectTrue(fuse::renderer::tryPreflightTaaJitterNdc(1920u, 1080u, 8u, rejectReason),
               "tryPreflightTaaJitterNdc passes for valid viewport");
    expectTrue(rejectReason == fuse::renderer::TaaJitterGuardRejectReason::None,
               "tryPreflightTaaJitterNdc reject reason is None");
    expectTrue(!fuse::renderer::tryPreflightTaaJitterNdc(0u, 1080u, 8u, rejectReason),
void testHistoryWarmupRejectGuards() {
    expectTrue(std::strcmp(fuse::renderer::taaHistoryWarmupRejectReasonLabel(
                               fuse::renderer::TaaHistoryWarmupRejectReason::None),
                           "none") == 0,
               "None warmup reject label");
                               fuse::renderer::TaaHistoryWarmupRejectReason::NotReady),
               "NotReady warmup reject label");
                               fuse::renderer::TaaHistoryWarmupRejectReason::AlreadyWarm),
                           "already_warm") == 0,
               "AlreadyWarm warmup reject label");

    expectTrue(fuse::renderer::classifyTaaHistoryWarmupReject(emptyHistory) ==
                   fuse::renderer::TaaHistoryWarmupRejectReason::NotReady,
               "empty history warmup classify is NotReady");
    expectTrue(fuse::renderer::shouldSkipTaaHistoryWarmup(emptyHistory),
               "empty history should skip warmup");
               "empty history warmup not complete");
                               fuse::renderer::TaaHistoryWarmupState::NeedsWarmup),
                           "needs_warmup") == 0,
               "NeedsWarmup warmup state label");
                               fuse::renderer::TaaHistoryWarmupState::Ready),
                           "ready") == 0,
               "Ready warmup state label");

               "empty history warmup state is NotReady");
               "empty history should skip warmup preflight");
void testHistoryWarmupPreflightGuards() {
    expectTrue(!fuse::renderer::preflightTaaHistoryWarmup(emptyHistory),
               "empty history fails warmup preflight");

    expectTrue(!fuse::renderer::tryPreflightTaaHistoryWarmup(emptyHistory, reason),
               "tryPreflightTaaHistoryWarmup fails for empty history");
               "empty history warmup reason is NotReady");
    expectTrue(fuse::renderer::tryPreflightTaaJitterNdc(128u, 128u, 8u, rejectReason),
               "valid viewport should not skip NDC jitter");
               "zero width should skip NDC jitter");

void testHistoryIsWarmedGuard() {
    expectTrue(!fuse::renderer::taaHistoryIsWarmed(emptyHistory), "empty history is not warmed");
    expectTrue(fuse::renderer::taaHistoryNeedsWarmup(emptyHistory), "empty history needs warmup");
    expectTrue(!emptyHistory.isWarmed(), "empty history buffer isWarmed is false");
void testHistoryWarmupPreflightFollowUp() {
    const fuse::renderer::TaaHistoryWarmupPreflight emptyPreflight =
        fuse::renderer::preflightTaaHistoryWarmup(emptyHistory);
    expectTrue(!emptyPreflight.history_ready, "empty history warmup preflight not ready");
    expectTrue(emptyPreflight.needs_warmup, "empty history warmup preflight needs warmup");
    expectTrue(emptyPreflight.warmup_frames_remaining == 1u,
               "empty history warmup preflight has one frame remaining");
    expectTrue(!emptyPreflight.isWarmed(), "empty history warmup preflight not warmed");
               "empty history should skip warmup completion");

    expectTrue(std::strcmp(fuse::renderer::taaHistoryWarmupBlockReasonLabel(
                               fuse::renderer::TaaHistoryWarmupBlockReason::None),
               "None warmup block label");
                               fuse::renderer::TaaHistoryWarmupBlockReason::NotReady),
               "NotReady warmup block label");
                               fuse::renderer::TaaHistoryWarmupBlockReason::NeedsWarmup),
               "NeedsWarmup warmup block label");

    expectTrue(fuse::renderer::classifyTaaHistoryWarmupBlock(emptyHistory) ==
                   fuse::renderer::TaaHistoryWarmupBlockReason::NotReady,
               "empty history warmup block is NotReady");

    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for warmup reject guard test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for warmup state guard test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for warmup preflight test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for is-warmed guard test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for warmup preflight follow-up test");
               "empty history warm-up state is NotReady");
               "NotReady warm-up state label");
               "empty history should skip warm-up completion");
               "empty history warm-up is not complete");

    expectTrue(!fuse::renderer::tryPreflightTaaHistoryWarmup(emptyHistory, state),
               "tryPreflight warm-up fails for empty history");
               "tryPreflight warm-up state is NotReady for empty history");

    auto bootstrap = createTaaTestBootstrap();
    if (bootstrap == nullptr) {
        std::fprintf(stderr, "SKIP: history warm-up state guards (Vulkan device unavailable)\n");
        return;

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaHistoryBuffer history;
    fuse::renderer::TaaHistoryBufferDesc historyDesc{64, 64};
    expectTrue(history.init(resources, historyDesc), "history ready for warmup reject guard test");

    fuse::renderer::TaaHistoryWarmupRejectReason reason = fuse::renderer::TaaHistoryWarmupRejectReason::None;
    expectTrue(fuse::renderer::preflightTaaHistoryWarmup(history, &reason),
               "allocated unwarmed history passes warmup preflight");
    expectTrue(reason == fuse::renderer::TaaHistoryWarmupRejectReason::None,
               "unwarmed history warmup preflight reason is None");
    expectTrue(fuse::renderer::tryPreflightTaaHistoryWarmup(history, reason),
               "tryPreflight warmup passes for unwarmed history");
    expectTrue(!fuse::renderer::shouldSkipTaaHistoryWarmup(history),
               "unwarmed history should not skip warmup");
    expectTrue(!history.warmupComplete(), "history buffer warmupComplete false before resolve");
    expectTrue(!fuse::renderer::taaHistoryWarmupComplete(history),
               "taaHistoryWarmupComplete false before resolve");

    history.markResolved();
    expectTrue(fuse::renderer::classifyTaaHistoryWarmupReject(history) ==
                   fuse::renderer::TaaHistoryWarmupRejectReason::AlreadyWarm,
               "warmed history warmup classify is AlreadyWarm");
    expectTrue(!fuse::renderer::preflightTaaHistoryWarmup(history, &reason),
               "warmed history fails warmup preflight");
    expectTrue(reason == fuse::renderer::TaaHistoryWarmupRejectReason::AlreadyWarm,
               "warmed history warmup preflight reason is AlreadyWarm");
    expectTrue(fuse::renderer::shouldSkipTaaHistoryWarmup(history),
               "warmed history should skip warmup");
    expectTrue(history.warmupComplete(), "history buffer warmupComplete after resolve");
    expectTrue(fuse::renderer::taaHistoryWarmupComplete(history),
               "taaHistoryWarmupComplete after resolve");
    expectTrue(history.init(resources, historyDesc), "history ready for warmup state guard test");
    expectTrue(history.readyForResolve(), "allocated history ready for resolve");
    expectTrue(fuse::renderer::classifyTaaHistoryWarmupState(history) ==
                   fuse::renderer::TaaHistoryWarmupState::NeedsWarmup,
               "allocated history warmup state is NeedsWarmup");
    expectTrue(history.warmupState() == fuse::renderer::TaaHistoryWarmupState::NeedsWarmup,
               "history buffer warmupState is NeedsWarmup");

    fuse::renderer::TaaHistoryWarmupState warmupState = fuse::renderer::TaaHistoryWarmupState::NotReady;
    expectTrue(!fuse::renderer::tryPreflightTaaHistoryWarmup(history, warmupState),
               "tryPreflightTaaHistoryWarmup fails before first resolve");
    expectTrue(warmupState == fuse::renderer::TaaHistoryWarmupState::NeedsWarmup,
               "tryPreflightTaaHistoryWarmup state is NeedsWarmup");

    expectTrue(fuse::renderer::taaHistoryWarmupComplete(history), "warmed history warmup complete");
    expectTrue(history.warmupComplete(), "history buffer warmupComplete true after resolve");
    expectTrue(fuse::renderer::tryPreflightTaaHistoryWarmup(history, warmupState),
               "tryPreflightTaaHistoryWarmup passes after warmup");
    expectTrue(warmupState == fuse::renderer::TaaHistoryWarmupState::Ready,
               "tryPreflightTaaHistoryWarmup state is Ready");
    expectTrue(history.init(resources, historyDesc), "history ready for warmup preflight test");
               "unwarmed history should skip warmup preflight");
    expectTrue(!fuse::renderer::tryPreflightTaaHistoryWarmup(history, reason),
               "tryPreflightTaaHistoryWarmup fails for unwarmed history");
    expectTrue(reason == fuse::renderer::TaaHistoryReuseBlockReason::NotWarm,
               "unwarmed history warmup reason is NotWarm");

               "warmed history should not skip warmup preflight");
               "warmed history passes warmup preflight");
    expectTrue(reason == fuse::renderer::TaaHistoryReuseBlockReason::None,
               "warmed history warmup reason is None");
    expectTrue(history.init(resources, historyDesc), "history ready for is-warmed guard test");
    expectTrue(!fuse::renderer::taaHistoryIsWarmed(history), "fresh history is not warmed");
    expectTrue(!history.isWarmed(), "fresh history buffer isWarmed is false");

    expectTrue(fuse::renderer::taaHistoryIsWarmed(history), "resolved history is warmed");
    expectTrue(history.isWarmed(), "resolved history buffer isWarmed is true");
    expectTrue(!fuse::renderer::taaHistoryNeedsWarmup(history), "warmed history does not need warmup");
    expectTrue(history.init(resources, historyDesc), "history ready for warmup preflight follow-up test");

    fuse::renderer::TaaHistoryWarmupPreflight unwarmedPreflight =
        fuse::renderer::preflightTaaHistoryWarmup(history);
    expectTrue(unwarmedPreflight.history_ready, "allocated history warmup preflight is ready");
    expectTrue(unwarmedPreflight.needs_warmup, "allocated history warmup preflight needs warmup");
               "unwarmed history should skip warmup completion");

    fuse::renderer::TaaHistoryWarmupPreflight tryOut{};
    expectTrue(!fuse::renderer::tryPreflightTaaHistoryWarmup(history, tryOut),
               "tryPreflight warmup fails before first resolve");
    expectTrue(tryOut.needs_warmup, "tryPreflight warmup output marks needs warmup");

    const fuse::renderer::TaaHistoryWarmupPreflight warmedPreflight =
    expectTrue(warmedPreflight.isWarmed(), "warmed history warmup preflight is warmed");
    expectTrue(warmedPreflight.canReuseHistory(), "warmed history warmup preflight can reuse");
               "warmed history should not skip warmup completion");
    expectTrue(fuse::renderer::tryPreflightTaaHistoryWarmup(history, tryOut),
               "tryPreflight warmup passes after first resolve");
    expectTrue(tryOut.isWarmed(), "tryPreflight warmup output marks warmed");
    expectTrue(history.init(resources, historyDesc), "history ready for warm-up state test");
               "allocated history warm-up state is NeedsWarmup");
    expectTrue(std::strcmp(fuse::renderer::taaHistoryWarmupStateLabel(
               "NeedsWarmup warm-up state label");
    expectTrue(history.needsWarmup(), "history needsWarmup before first resolve");
    expectTrue(!history.warmupComplete(), "history warmupComplete false before first resolve");
    expectTrue(fuse::renderer::preflightTaaHistoryWarmup(history, &state) == false,
               "warm-up preflight fails before first resolve");
    expectTrue(state == fuse::renderer::TaaHistoryWarmupState::NeedsWarmup,
               "warm-up preflight state is NeedsWarmup before first resolve");

                   fuse::renderer::TaaHistoryWarmupState::Complete,
               "warmed history warm-up state is Complete");
                               fuse::renderer::TaaHistoryWarmupState::Complete),
                           "complete") == 0,
               "Complete warm-up state label");
    expectTrue(history.warmupComplete(), "history warmupComplete after first resolve");
               "warmed history should not skip warm-up completion");
    expectTrue(fuse::renderer::tryPreflightTaaHistoryWarmup(history, state),
               "tryPreflight warm-up passes after first resolve");
    expectTrue(state == fuse::renderer::TaaHistoryWarmupState::Complete,
               "tryPreflight warm-up state is Complete after first resolve");
    expectTrue(fuse::renderer::classifyTaaHistoryWarmupBlock(history) ==
                   fuse::renderer::TaaHistoryWarmupBlockReason::NeedsWarmup,
               "allocated history warmup block is NeedsWarmup");

    fuse::renderer::TaaHistoryWarmupBlockReason warmupReason =
        fuse::renderer::TaaHistoryWarmupBlockReason::None;
    expectTrue(!fuse::renderer::tryPreflightTaaHistoryWarmup(history, warmupReason),
    expectTrue(warmupReason == fuse::renderer::TaaHistoryWarmupBlockReason::NeedsWarmup,
               "tryPreflight warmup reason is NeedsWarmup");

    expectTrue(fuse::renderer::taaHistoryWarmupComplete(history), "warmed history warmup is complete");
    expectTrue(fuse::renderer::tryPreflightTaaHistoryWarmup(history, warmupReason),
               "tryPreflightTaaHistoryWarmup passes for warmed history");
    expectTrue(warmupReason == fuse::renderer::TaaHistoryWarmupBlockReason::None,
               "tryPreflight warmup reason is None after warmup");

    history.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());

void testJitterShouldSkipAndAdvanceGuards() {


void testJitterShouldSkipAndTryNdcGuards() {
}

void testJitterShouldSkipAndPixelOffsetGuards() {
    using fuse::renderer::TaaJitterLayout;


void testJitterShouldSkipAndReadyGuards() {
    expectTrue(fuse::renderer::taaJitterSyncReady(5u, 8u), "valid sequence is sync-ready");
    expectTrue(!fuse::renderer::taaJitterSyncReady(5u, 0u), "invalid sequence is not sync-ready");
               "valid sequence should not skip jitter sync");
               "invalid sequence should skip jitter sync");
    expectTrue(!fuse::renderer::shouldSkipTaaJitterNdc(128u, 128u, 8u),
               "valid viewport should not skip jitter NDC");
               "zero width should skip jitter NDC");


    expectTrue(fuse::renderer::taaJitterSyncReady(5u, 8u),
               "valid sequence jitter sync ready");
    expectTrue(!fuse::renderer::taaJitterSyncReady(5u, 0u),
               "invalid sequence jitter sync not ready");

    expectTrue(fuse::renderer::taaJitterNdcReady(128u, 128u, 8u),
               "valid viewport jitter NDC ready");
    expectTrue(!fuse::renderer::taaJitterNdcReady(0u, 128u, 8u),
               "zero width jitter NDC not ready");



    expectTrue(fuse::renderer::taaJitterNdcReady(128u, 128u, 8u), "valid viewport is NDC-ready");

               "zero width should skip NDC preflight");

    expectTrue(fuse::renderer::tryPreflightTaaJitterNdc(128u, 128u, 8u, rejectReason),
               "tryPreflightTaaJitterNdc passes for valid viewport");
    expectTrue(rejectReason == fuse::renderer::TaaJitterGuardRejectReason::None,
               "tryPreflightTaaJitterNdc reject reason is None");
    expectTrue(!fuse::renderer::tryPreflightTaaJitterNdc(0u, 128u, 8u, rejectReason),
               "tryPreflightTaaJitterNdc rejects zero width");
    expectTrue(rejectReason == fuse::renderer::TaaJitterGuardRejectReason::InvalidViewport,
               "tryPreflightTaaJitterNdc reject reason is InvalidViewport");

void testHistoryWarmupCompleteGuard() {
    expectTrue(!fuse::renderer::taaHistoryWarmupComplete(emptyHistory), "empty history warmup not complete");
    expectTrue(!emptyHistory.warmupComplete(), "empty history buffer warmupComplete is false");

    expectTrue(fuse::renderer::classifyTaaJitterAdvanceReject(8u) ==
                   fuse::renderer::TaaJitterGuardRejectReason::None,
               "valid sequence passes advance classify");
    expectTrue(fuse::renderer::classifyTaaJitterAdvanceReject(0u) ==
                   fuse::renderer::TaaJitterGuardRejectReason::InvalidSequence,
               "zero sequence fails advance classify");
    expectTrue(!fuse::renderer::shouldSkipTaaJitterAdvance(8u),
               "valid sequence should not skip jitter advance");
    expectTrue(fuse::renderer::shouldSkipTaaJitterAdvance(0u),
               "invalid sequence should skip jitter advance");
    expectTrue(fuse::renderer::tryPreflightTaaJitterAdvance(8u, rejectReason),
               "tryPreflightTaaJitterAdvance passes for valid sequence");
    expectTrue(!fuse::renderer::tryPreflightTaaJitterAdvance(0u, rejectReason),
               "tryPreflightTaaJitterAdvance rejects invalid sequence");

    fuse::renderer::TaaJitter jitter;
    expectTrue(!fuse::renderer::shouldSkipTaaJitterAdvance(jitter.sequenceLength()),
               "default jitter should not skip advance");
    expectTrue(jitter.advanceIfReady(), "advanceIfReady still succeeds with guard helpers");

void testResolveHistoryBlendPreflightGuards() {
    fuse::math::Vec2 ndcOut{};
    expectTrue(fuse::renderer::tryComputeTaaJitterNdcOffset(3u, 128u, 128u, 8u, ndcOut, rejectReason),
               "tryComputeTaaJitterNdcOffset succeeds for valid viewport");
    const fuse::math::Vec2 expected =
        fuse::renderer::TaaJitterLayout::ndcOffsetForFrameIndex(3u, 128u, 128u, 8u);
    expectNear(ndcOut.x, expected.x, 1e-6f, "tryComputeTaaJitterNdcOffset matches layout X");
    expectNear(ndcOut.y, expected.y, 1e-6f, "tryComputeTaaJitterNdcOffset matches layout Y");
    expectTrue(!fuse::renderer::tryComputeTaaJitterNdcOffset(3u, 0u, 128u, 8u, ndcOut, rejectReason),
               "tryComputeTaaJitterNdcOffset rejects zero width");

    expectTrue(jitter.tryCurrentNdcOffset(128u, 128u, ndcOut, rejectReason),
               "TaaJitter::tryCurrentNdcOffset succeeds for valid viewport");
    const fuse::math::Vec2 directNdc = jitter.currentNdcOffset(128u, 128u);
    expectNear(ndcOut.x, directNdc.x, 1e-6f, "tryCurrentNdcOffset matches currentNdcOffset X");
    expectTrue(!jitter.tryCurrentNdcOffset(0u, 128u, ndcOut, rejectReason),
               "TaaJitter::tryCurrentNdcOffset rejects zero width");

void testResolveShouldSkipAndTryPreflight() {
    fuse::renderer::TaaHistoryBuffer emptyHistory;
    fuse::renderer::TaaResolveDesc desc{};
    desc.width = 64;
    desc.height = 64;
    desc.surfaces.current_frame = reinterpret_cast<void*>(0x10);
    desc.surfaces.output = reinterpret_cast<void*>(0x20);

    expectTrue(fuse::renderer::shouldSkipTaaResolve(desc, emptyHistory),
               "empty history should skip resolve");
}

void testResolveFramePreflightGuards() {
               "empty history should skip resolve preflight");

    fuse::renderer::TaaResolveSkipReason skipReason = fuse::renderer::TaaResolveSkipReason::None;
    expectTrue(!fuse::renderer::tryPreflightTaaResolve(desc, emptyHistory, skipReason),
               "tryPreflightTaaResolve fails for empty history");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::HistoryNotReady,
               "empty history resolve skip reason is HistoryNotReady");


void testTemporalBlendPreflight() {
    expectTrue(!jitter.shouldSkipSyncToFrameIndex(3u), "jitter should not skip sync for valid sequence");
    expectTrue(!jitter.shouldSkipNdcOffset(128u, 128u), "jitter should not skip NDC for valid viewport");
    expectTrue(jitter.shouldSkipNdcOffset(0u, 128u), "jitter should skip NDC for zero width");

void testTemporalResolveGuardBundle() {
    expectTrue(std::strcmp(fuse::renderer::taaTemporalGuardRejectReasonLabel(
                               fuse::renderer::TaaTemporalGuardRejectReason::None),
                           "none") == 0,
               "None temporal guard reject label");
                               fuse::renderer::TaaTemporalGuardRejectReason::HistoryReuseBlocked),
                           "history_reuse_blocked") == 0,
               "HistoryReuseBlocked temporal guard reject label");
                               fuse::renderer::TaaTemporalGuardRejectReason::BlendWeightsRejected),
                           "blend_weights_rejected") == 0,
               "BlendWeightsRejected temporal guard reject label");

    expectTrue(fuse::renderer::shouldSkipTaaTemporalResolve(desc, emptyHistory),
               "empty history should skip temporal resolve");
    expectTrue(fuse::renderer::classifyTaaTemporalGuardReject(desc, emptyHistory) ==
                   fuse::renderer::TaaTemporalGuardRejectReason::HistoryReuseBlocked,
               "empty history temporal guard is HistoryReuseBlocked");


void testJitterShouldSkipAndAdvancePreflights() {
    expectTrue(!fuse::renderer::shouldSkipTaaJitterSync(5u, 8u), "valid sequence should not skip sync");
    expectTrue(fuse::renderer::shouldSkipTaaJitterSync(5u, 0u), "invalid sequence should skip sync");
               "valid viewport should not skip NDC preflight");
    expectTrue(fuse::renderer::shouldSkipTaaJitterNdc(0u, 128u, 8u),
               "zero width should skip NDC preflight");
    expectTrue(fuse::renderer::preflightTaaJitterAdvance(8u), "valid sequence allows advance preflight");
    expectTrue(fuse::renderer::shouldSkipTaaJitterAdvance(0u), "invalid sequence skips advance preflight");

    expectTrue(fuse::renderer::tryPreflightTaaJitterNdc(192u, 108u, 8u, rejectReason),
    expectTrue(!fuse::renderer::tryPreflightTaaJitterNdc(0u, 108u, 8u, rejectReason),

void testResolveBlendPreflightFollowUp() {
               "tryPreflightTaaResolve skip reason is HistoryNotReady");

    expectTrue(!jitter.shouldSkipSync(4u), "jitter should not skip sync for valid sequence");
    expectTrue(!jitter.shouldSkipNdc(64u, 64u), "jitter should not skip NDC for valid viewport");
    expectTrue(jitter.shouldSkipNdc(0u, 64u), "jitter should skip NDC for zero width");

void testResolveBlendReadyGuard() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for history reuse preflight test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for warmup state guard test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for effective generation test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for wouldSkip history reuse test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for warmup-complete guard test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for history blend preflight test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for resolve should-skip test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for temporal blend preflight test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for temporal guard bundle test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for resolve blend preflight follow-up test");
    fuse::math::Vec2 pixelOut{};
    expectTrue(jitter.currentPixelOffsetIfReady(pixelOut), "currentPixelOffsetIfReady succeeds");
    const fuse::math::Vec2 directPixel = jitter.currentPixelOffset();
    expectNear(pixelOut.x, directPixel.x, 1e-6f, "currentPixelOffsetIfReady matches currentPixelOffset X");
    expectNear(pixelOut.y, directPixel.y, 1e-6f, "currentPixelOffsetIfReady matches currentPixelOffset Y");

    fuse::renderer::TaaJitterDesc invalidDesc{};
    invalidDesc.sequence_length = 0u;
    fuse::renderer::TaaJitter fallbackJitter(invalidDesc);
    expectTrue(fallbackJitter.currentPixelOffsetIfReady(pixelOut),
               "currentPixelOffsetIfReady succeeds after invalid desc fallback");
    expectTrue(fallbackJitter.sequenceLength() == 8u,
               "invalid desc still falls back to default sequence length");

    expectTrue(TaaJitterLayout::jitterSlotMatchesFrameIndex(13u, 5u, 8u),
               "wrapped frame index still matches slot for sync guard");

void testResolveTemporalBlendPreflight() {
    expectTrue(std::strcmp(fuse::renderer::taaResolveTemporalRejectReasonLabel(
                               fuse::renderer::TaaResolveTemporalRejectReason::None),
               "None temporal reject label");
                               fuse::renderer::TaaResolveTemporalRejectReason::HistoryReuseBlocked),
               "HistoryReuseBlocked temporal reject label");
                               fuse::renderer::TaaResolveTemporalRejectReason::BlendWeightsRejected),
               "BlendWeightsRejected temporal reject label");

    expectTrue(fuse::renderer::classifyTaaResolveTemporalBlendReject(desc, emptyHistory) ==
                   fuse::renderer::TaaResolveTemporalRejectReason::HistoryReuseBlocked,
               "empty history blocks temporal blend preflight");
    expectTrue(fuse::renderer::shouldSkipTaaResolveTemporalBlend(desc, emptyHistory),
               "empty history should skip temporal blend");

    auto bootstrap = createTaaTestBootstrap();
    if (bootstrap == nullptr) {
        std::fprintf(stderr, "SKIP: resolve temporal blend preflight (Vulkan device unavailable)\n");
        return;
    expectTrue(bootstrap != nullptr, "bootstrap allocated for resolve frame preflight test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for resolve blend ready test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaHistoryBuffer history;
    fuse::renderer::TaaHistoryBufferDesc historyDesc{64, 64};
    expectTrue(history.init(resources, historyDesc), "history ready for reuse preflight test");

    reason = fuse::renderer::TaaHistoryReuseBlockReason::None;
    expectTrue(!fuse::renderer::preflightTaaHistoryReuse(history, 0u, &reason),
               "unwarmed history fails reuse preflight");
    expectTrue(reason == fuse::renderer::TaaHistoryReuseBlockReason::NotWarm,
               "unwarmed history reports NotWarm");

    history.markResolved();
    expectTrue(fuse::renderer::preflightTaaHistoryReuse(history, 0u, &reason),
               "warmed history passes reuse preflight");
    expectTrue(reason == fuse::renderer::TaaHistoryReuseBlockReason::None,
               "warmed history reports no block reason");

    history.invalidateHistory();
               "stale generation fails reuse preflight");
    expectTrue(reason == fuse::renderer::TaaHistoryReuseBlockReason::StaleGeneration,
               "stale generation reports StaleGeneration");
    expectTrue(history.init(resources, historyDesc), "history ready for warmup state guard test");
    expectTrue(fuse::renderer::classifyTaaHistoryWarmupState(history) ==
                   fuse::renderer::TaaHistoryWarmupState::AwaitingFirstResolve,
               "allocated history awaits first resolve");
    expectTrue(!fuse::renderer::preflightTaaHistoryWarmup(history, &state),
               "unwarmed history warmup preflight fails");
    expectTrue(state == fuse::renderer::TaaHistoryWarmupState::AwaitingFirstResolve,
               "unwarmed history warmup preflight reports AwaitingFirstResolve");

                   fuse::renderer::TaaHistoryWarmupState::Complete,
               "warmed history classified as Complete");
    expectTrue(fuse::renderer::taaHistoryWarmupComplete(history), "warmed history warmup is complete");
    expectTrue(fuse::renderer::preflightTaaHistoryWarmup(history, &state),
               "warmed history warmup preflight passes");
    expectTrue(state == fuse::renderer::TaaHistoryWarmupState::Complete,
               "warmed history warmup preflight reports Complete");
    expectTrue(history.init(resources, historyDesc), "history ready for wouldSkip reuse test");
    expectTrue(fuse::renderer::wouldSkipTaaHistoryReuse(history, 0u),
               "wouldSkip true for unwarmed history");
    expectTrue(!fuse::renderer::tryPreflightTaaHistoryReuse(history, 0u, reason),
               "tryPreflight fails for unwarmed history");
               "tryPreflight reason is NotWarm for unwarmed history");

    expectTrue(fuse::renderer::taaHistoryWarmupComplete(history), "warmup complete after first resolve");
    expectTrue(!fuse::renderer::wouldSkipTaaHistoryReuse(history, 0u),
               "wouldSkip false for warmed history");
    expectTrue(fuse::renderer::tryPreflightTaaHistoryReuse(history, 0u, reason),
               "tryPreflight succeeds for warmed history");
               "tryPreflight reason is None for warmed history");

               "wouldSkip true after invalidate");
               "tryPreflight fails after invalidate");
               "tryPreflight reason is StaleGeneration after invalidate");
    expectTrue(history.init(resources, historyDesc), "history ready for warmup-complete guard test");
    expectTrue(!fuse::renderer::taaHistoryWarmupComplete(history), "unwarmed history warmup not complete");
    expectTrue(!history.warmupComplete(), "unwarmed history buffer warmupComplete is false");

    expectTrue(history.warmupComplete(), "warmed history buffer warmupComplete is true");

    expectTrue(!fuse::renderer::taaHistoryWarmupComplete(history), "invalidated history warmup not complete");
    expectTrue(history.init(resources, historyDesc), "history ready for history blend preflight test");
    expectTrue(history.init(resources, historyDesc), "history ready for resolve blend preflight follow-up test");

    fuse::renderer::TaaResolveDesc desc{};
    desc.params.blend_factor = 0.4f;

    expectTrue(fuse::renderer::taaResolveBlendWeightsReady(desc, history),
               "warmup blend weights are ready");
    expectTrue(fuse::renderer::shouldSkipTaaResolveHistoryBlend(desc, history),
               "warmup should skip history blend");

    fuse::renderer::TaaResolveBlendRejectReason rejectReason =
        fuse::renderer::TaaResolveBlendRejectReason::None;
    expectTrue(!fuse::renderer::tryPreflightTaaResolveHistoryBlend(desc, history, rejectReason),
               "tryPreflight history blend fails on warmup");
    expectTrue(rejectReason == fuse::renderer::TaaResolveBlendRejectReason::None,
               "warmup history blend reject reason is None");

               "steady blend weights are ready after warmup");
    expectTrue(!fuse::renderer::shouldSkipTaaResolveHistoryBlend(desc, history),
               "steady state should not skip history blend");
    expectTrue(fuse::renderer::tryPreflightTaaResolveHistoryBlend(desc, history, rejectReason),
               "tryPreflight history blend passes after warmup");

    desc.observed_history_generation = 0u;
               "stale generation should skip history blend");
    expectTrue(history.init(resources, historyDesc), "history ready for resolve should-skip test");
    expectTrue(!fuse::renderer::shouldSkipTaaResolve(desc, history),
               "valid resolve should not be skipped");
    expectTrue(fuse::renderer::tryPreflightTaaResolve(desc, history, skipReason),
               "tryPreflightTaaResolve passes for valid resolve");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::None,
               "valid resolve skip reason is None");

    desc.width = 0;
    expectTrue(fuse::renderer::shouldSkipTaaResolve(desc, history),
               "invalid dimensions should skip resolve");
    expectTrue(!fuse::renderer::tryPreflightTaaResolve(desc, history, skipReason),
               "tryPreflightTaaResolve fails for invalid dimensions");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::InvalidDimensions,
               "invalid dimensions skip reason is InvalidDimensions");
    expectTrue(history.init(resources, historyDesc), "history ready for temporal blend preflight test");

    desc.params.blend_factor = 0.2f;

    fuse::renderer::TaaHistoryReuseBlockReason reuseReason = fuse::renderer::TaaHistoryReuseBlockReason::None;
    fuse::renderer::TaaResolveBlendRejectReason blendReason =
    expectTrue(!fuse::renderer::preflightTaaTemporalBlend(desc, history, &reuseReason, &blendReason),
               "temporal blend preflight fails before warmup");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::NotWarm,
               "temporal blend reuse reason is NotWarm before warmup");
    expectTrue(blendReason == fuse::renderer::TaaResolveBlendRejectReason::None,
               "temporal blend reject reason is None before warmup");
    expectTrue(fuse::renderer::shouldSkipTaaTemporalBlend(desc, history),
               "temporal blend should be skipped before warmup");

    expectTrue(fuse::renderer::preflightTaaTemporalBlend(desc, history, &reuseReason, &blendReason),
               "temporal blend preflight passes after warmup");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::None,
               "temporal blend reuse reason is None after warmup");
    expectTrue(!fuse::renderer::shouldSkipTaaTemporalBlend(desc, history),
               "temporal blend should not be skipped after warmup");

               "temporal blend preflight fails after invalidate");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::StaleGeneration,
               "temporal blend reuse reason is StaleGeneration after invalidate");
    expectTrue(history.init(resources, historyDesc), "history ready for temporal guard bundle test");

    fuse::renderer::TaaTemporalGuardRejectReason temporalReason =
        fuse::renderer::TaaTemporalGuardRejectReason::None;
    expectTrue(!fuse::renderer::preflightTaaTemporalResolve(desc, history, &temporalReason),
               "unwarmed history fails temporal resolve preflight");
    expectTrue(temporalReason == fuse::renderer::TaaTemporalGuardRejectReason::HistoryReuseBlocked,
               "unwarmed history temporal reason is HistoryReuseBlocked");
    expectTrue(!fuse::renderer::tryPreflightTaaTemporalResolve(desc, history, temporalReason),
               "tryPreflightTaaTemporalResolve fails for unwarmed history");

    expectTrue(fuse::renderer::preflightTaaTemporalResolve(desc, history, &temporalReason),
               "warmed history passes temporal resolve preflight");
    expectTrue(temporalReason == fuse::renderer::TaaTemporalGuardRejectReason::None,
               "warmed history temporal reason is None");
    expectTrue(!fuse::renderer::shouldSkipTaaTemporalResolve(desc, history),
               "warmed history should not skip temporal resolve");
    const fuse::renderer::TaaResolveBlendPreflight warmupPreflight =
        fuse::renderer::preflightTaaResolveBlend(desc, history);
    expectTrue(warmupPreflight.can_apply, "warmup resolve blend preflight can apply");
    expectTrue(warmupPreflight.reject_reason == fuse::renderer::TaaResolveBlendRejectReason::None,
               "warmup resolve blend preflight reject reason is None");
    expectNear(warmupPreflight.weights.current, 1.f, 1e-5f, "warmup resolve blend preflight current is full");
    expectTrue(!warmupPreflight.appliesHistoryBlend(),
               "warmup resolve blend preflight does not apply history blend");

    const fuse::renderer::TaaResolveBlendPreflight steadyPreflight =
    expectTrue(steadyPreflight.can_apply, "steady resolve blend preflight can apply");
    expectNear(steadyPreflight.weights.current, 0.4f, 1e-5f, "steady resolve blend preflight current weight");
    expectTrue(steadyPreflight.appliesHistoryBlend(),
               "steady resolve blend preflight applies history blend");

    fuse::renderer::TaaResolveTemporalRejectReason rejectReason =
        fuse::renderer::TaaResolveTemporalRejectReason::None;
    expectTrue(!fuse::renderer::preflightTaaResolveTemporalBlend(desc, history, &rejectReason),
               "unwarmed history fails temporal blend preflight");
    expectTrue(rejectReason == fuse::renderer::TaaResolveTemporalRejectReason::HistoryReuseBlocked,
               "unwarmed history temporal reject reason is HistoryReuseBlocked");

    expectTrue(fuse::renderer::tryPreflightTaaResolveTemporalBlend(desc, history, rejectReason),
               "warmed history passes temporal blend preflight");
    expectTrue(rejectReason == fuse::renderer::TaaResolveTemporalRejectReason::None,
               "warmed history temporal reject reason is None");
    expectTrue(!fuse::renderer::shouldSkipTaaResolveTemporalBlend(desc, history),
               "warmed history should not skip temporal blend");
    expectTrue(history.init(resources, historyDesc), "history ready for resolve frame preflight test");

    desc.width = 64;
    desc.height = 64;
    desc.surfaces.current_frame = reinterpret_cast<void*>(0x10);
    desc.surfaces.output = reinterpret_cast<void*>(0x20);

               "tryPreflightTaaResolve passes for valid desc");
               "tryPreflightTaaResolve skip reason is None");

    expectTrue(fuse::renderer::preflightTaaResolveFrame(desc, history, &skipReason, &blendReason),
               "resolve frame preflight passes for warmup");
    expectTrue(!fuse::renderer::shouldSkipTaaResolveFrame(desc, history),
               "resolve frame should not be skipped for warmup");

    expectTrue(fuse::renderer::tryPreflightTaaResolveFrame(desc, history, skipReason, blendReason),
               "tryPreflightTaaResolveFrame passes for warmup");
               "tryPreflightTaaResolveFrame skip reason is None");
               "tryPreflightTaaResolveFrame blend reason is None");

    expectTrue(fuse::renderer::shouldSkipTaaResolveFrame(desc, history),
               "invalid dimensions should skip resolve frame");
    expectTrue(!fuse::renderer::tryPreflightTaaResolveFrame(desc, history, skipReason, blendReason),
               "tryPreflightTaaResolveFrame fails for invalid dimensions");
               "tryPreflightTaaResolveFrame skip reason is InvalidDimensions");
    expectTrue(history.init(resources, historyDesc), "history ready for resolve blend ready test");

    desc.params.blend_factor = 0.25f;

    expectTrue(fuse::renderer::taaResolveBlendReady(desc, history),
               "warmup blend is ready before first resolve");
    expectTrue(!fuse::renderer::shouldSkipTaaResolveBlend(desc, history),
               "warmup blend should not be skipped");

               "steady blend is ready after warmup");

    history.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());

void testPreflightTaaResolveBlend() {
void testJitterFrameIndexSlotDriftGuards() {

    expectTrue(TaaJitterLayout::frameIndexSlotDrift(5u, 5u, 8u) == 0u,
               "aligned slot has zero drift");
    expectTrue(TaaJitterLayout::frameIndexSlotDrift(13u, 5u, 8u) == 0u,
               "wrapped frame index has zero drift at matching slot");
    expectTrue(TaaJitterLayout::frameIndexSlotDrift(5u, 6u, 8u) == 1u,
               "adjacent slot has drift of one");
    expectTrue(TaaJitterLayout::frameIndexSlotDrift(0u, 7u, 8u) == 1u,
               "wrap-around drift is minimal circular distance");
    expectTrue(TaaJitterLayout::frameIndexSlotDrift(0u, 0u, 0u) == 0u,
               "invalid sequence yields zero drift");

    expectTrue(!TaaJitterLayout::needsSyncToFrameIndex(5u, 5u, 8u),
               "aligned slot does not need sync");
    expectTrue(TaaJitterLayout::needsSyncToFrameIndex(5u, 6u, 8u),
               "misaligned slot needs sync");
    expectTrue(!TaaJitterLayout::needsSyncToFrameIndex(0u, 0u, 0u),
               "invalid sequence does not need sync");

    expectTrue(jitter.needsSyncToFrameIndex(5u), "default jitter needs sync to frame five");
    expectTrue(jitter.frameIndexSlotDrift(5u) > 0u, "default jitter has non-zero drift from frame five");
    jitter.syncToFrameIndex(5u);
    expectTrue(!jitter.needsSyncToFrameIndex(5u), "synced jitter does not need sync");
    expectTrue(jitter.frameIndexSlotDrift(5u) == 0u, "synced jitter has zero drift");
    jitter.advance();
    expectTrue(jitter.needsSyncToFrameIndex(5u), "advanced jitter needs re-sync to frame five");

void testResolveBlendModeGuards() {
    expectTrue(std::strcmp(fuse::renderer::taaResolveBlendModeLabel(fuse::renderer::TaaResolveBlendMode::Warmup),
                           "warmup") == 0,
               "Warmup blend mode label");
    expectTrue(std::strcmp(fuse::renderer::taaResolveBlendModeLabel(fuse::renderer::TaaResolveBlendMode::Steady),
                           "steady") == 0,
               "Steady blend mode label");
    expectTrue(std::strcmp(fuse::renderer::taaResolveBlendModeLabel(
                               fuse::renderer::TaaResolveBlendMode::StaleForcedCurrent),
                           "stale_forced_current") == 0,
               "StaleForcedCurrent blend mode label");

    expectTrue(bootstrap != nullptr, "bootstrap allocated for resolve blend preflight test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for blend mode guard test");



    expectTrue(history.init(resources, historyDesc), "history ready for resolve blend preflight test");

void testWouldRejectAndTryResolveBlendGuards() {

void testResolveBlendReadyGuard() {
    expectTrue(bootstrap != nullptr, "bootstrap allocated for wouldReject blend test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for resolve blend ready test");



    expectTrue(history.init(resources, historyDesc), "history ready for wouldReject blend test");

    desc.params.blend_factor = 0.2f;

    fuse::renderer::TaaBlendWeights weights{};
    expectTrue(fuse::renderer::preflightTaaResolveBlend(desc, history, &weights),
               "warmup blend preflight passes");
    expectNear(weights.current, 1.f, 1e-5f, "preflight warmup blend uses full current");
    expectNear(weights.history, 0.f, 1e-5f, "preflight warmup blend uses zero history");

               "steady blend preflight passes after warmup");
    expectNear(weights.current, 0.2f, 1e-5f, "preflight steady blend uses configured current");
    expectNear(weights.history, 0.8f, 1e-5f, "preflight steady blend uses history complement");

               "stale generation blend preflight still passes with forced current weight");
    expectNear(weights.current, 1.f, 1e-5f, "stale generation preflight forces full current");
    expectNear(weights.history, 0.f, 1e-5f, "stale generation preflight zeroes history");
    expectTrue(history.init(resources, historyDesc), "history ready for blend mode guard test");

    expectTrue(fuse::renderer::classifyTaaResolveBlendMode(desc, history) ==
                   fuse::renderer::TaaResolveBlendMode::Warmup,
               "unwarmed history is Warmup blend mode");

                   fuse::renderer::TaaResolveBlendMode::Steady,
               "warmed history with matching generation is Steady");

    desc.observed_history_generation = 99u;
                   fuse::renderer::TaaResolveBlendMode::StaleForcedCurrent,
               "warmed history with stale observed generation is StaleForcedCurrent");

               "invalidated history reverts to Warmup blend mode");

    const fuse::renderer::TaaBlendWeights a{0.2f, 0.8f};
    const fuse::renderer::TaaBlendWeights b{0.2f, 0.8f};
    const fuse::renderer::TaaBlendWeights c{0.3f, 0.8f};
    expectTrue(fuse::renderer::taaBlendWeightsNearEqual(a, b), "identical weights are near equal");
    expectTrue(!fuse::renderer::taaBlendWeightsNearEqual(a, c), "different weights are not near equal");

    fuse::renderer::TaaResolveStats stats{};
    stats.effective_blend = 0.25f;
    stats.history_blend = 0.75f;
    expectTrue(fuse::renderer::taaResolveStatsBlendConsistent(stats),
               "valid resolve stats blend weights are consistent");
    stats.history_blend = 0.8f;
    expectTrue(!fuse::renderer::taaResolveStatsBlendConsistent(stats),
               "invalid resolve stats blend weights are inconsistent");


void testTaaPassPreflightAndSyncGuards() {
    fuse::renderer::TaaPassDesc passDesc{};
    passDesc.width = 128;
    passDesc.height = 128;
    passDesc.params.blend_factor = 0.3f;

    auto pass = fuse::renderer::TaaPass::create(passDesc);
    expectTrue(pass->syncJitterToFrameIndexIfReady(4u), "pass guarded jitter sync succeeds before init");
    expectTrue(pass->isJitterSyncedToFrameIndex(4u), "pass jitter synced before init");

    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass preflight/sync guard test");


    expectTrue(pass->init(resources), "TaaPass initialized for preflight/sync guard test");

    fuse::renderer::TaaHistoryReuseBlockReason reuseReason = fuse::renderer::TaaHistoryReuseBlockReason::None;
    expectTrue(!pass->preflightHistoryReuse(0u, &reuseReason),
               "pass reuse preflight fails before warmup");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::NotWarm,
               "pass reuse preflight reports NotWarm");
    expectTrue(history.init(resources, historyDesc), "history ready for resolve blend ready test");

    expectTrue(fuse::renderer::taaResolveBlendReady(desc, history), "warmup blend is ready");
    expectTrue(!fuse::renderer::shouldSkipTaaResolveBlend(desc, history), "warmup blend should not be skipped");

    expectTrue(fuse::renderer::taaResolveBlendReady(desc, history), "steady blend is ready");


void testTaaPassSkipReadyAndTryPreflights() {
    passDesc.params.blend_factor = 0.15f;

    expectTrue(pass->jitterSyncReady(3u), "pass jitter sync ready before init");
    expectTrue(!pass->shouldSkipJitterSync(3u), "pass should not skip jitter sync before init");
    expectTrue(pass->jitterNdcReady(), "pass jitter NDC ready before init");
    expectTrue(!pass->shouldSkipJitterNdc(), "pass should not skip jitter NDC before init");
    expectTrue(!pass->historyWarmupComplete(), "pass history warmup not complete before init");
}

void testTaaPassTemporalAndJitterPreflightWrappers() {
    passDesc.params.blend_factor = 0.2f;

    expectTrue(pass->classifyHistoryWarmupState() == fuse::renderer::TaaHistoryWarmupState::NotReady,
               "pass warm-up state is NotReady before init");
    expectTrue(pass->shouldSkipHistoryWarmup(), "pass should skip warm-up completion before init");
    expectTrue(!pass->preflightHistoryWarmup(), "pass warm-up preflight fails before init");
    expectTrue(!pass->shouldSkipJitterSync(4u), "pass jitter sync should not skip before init");
    expectTrue(!pass->shouldSkipJitterNdc(), "pass NDC jitter should not skip before init");
    expectTrue(pass->preflightJitterNdc(), "pass NDC jitter preflight passes before init");

    fuse::renderer::TaaJitterGuardRejectReason jitterReject =
        fuse::renderer::TaaJitterGuardRejectReason::None;
    expectTrue(pass->tryPreflightJitterSync(4u, jitterReject), "pass tryPreflightJitterSync passes");
    expectTrue(jitterReject == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass tryPreflightJitterSync reject reason is None");
    expectTrue(pass->tryPreflightJitterNdc(jitterReject), "pass tryPreflightJitterNdc passes");
               "pass tryPreflightJitterNdc reject reason is None");
    expectTrue(pass->preflightJitterNdc(&jitterReject), "pass preflightJitterNdc passes");

void testTaaPassWarmupAndJitterSkipWrappers() {
    passDesc.params.blend_factor = 0.25f;

    fuse::renderer::TaaHistoryWarmupRejectReason warmupReason =
        fuse::renderer::TaaHistoryWarmupRejectReason::None;
    expectTrue(!pass->preflightHistoryWarmup(&warmupReason),
               "pass warmup preflight fails before init");
    expectTrue(warmupReason == fuse::renderer::TaaHistoryWarmupRejectReason::NotReady,
               "pass warmup preflight reason is NotReady before init");
    expectTrue(pass->shouldSkipHistoryWarmup(), "pass should skip warmup before init");
    expectTrue(!pass->shouldSkipJitterAdvance(), "pass should not skip jitter advance before init");

    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass skip/ready test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass warmup/jitter skip test");


    expectTrue(pass->init(resources), "TaaPass initialized for skip/ready test");
    expectTrue(pass->init(resources), "TaaPass initialized for warmup/jitter skip test");

    expectTrue(pass->preflightHistoryWarmup(&warmupReason),
               "pass warmup preflight passes after init before resolve");
    expectTrue(!pass->shouldSkipHistoryWarmup(),
               "pass should not skip warmup after init before resolve");

void testTaaPassDeepenShouldSkipWrappers() {

    expectTrue(!pass->preflightHistoryWarmup(), "pass warmup preflight fails before init");
    expectTrue(!pass->shouldSkipJitterSync(4u), "pass jitter sync valid before init");
    expectTrue(!pass->shouldSkipJitterNdc(), "pass jitter NDC valid before init");
    expectTrue(pass->preflightJitterNdc(), "pass jitter NDC preflight passes before init");

void testTaaPassTemporalAndJitterPreflights() {

    expectTrue(!pass->isHistoryWarmed(), "pass history not warmed before init");
    expectTrue(!pass->shouldSkipJitterSync(3u), "pass jitter sync should not skip before init");

void testTaaPassTemporalAndJitterSkipGuards() {

    expectTrue(!pass->shouldSkipJitterSync(2u), "pass should not skip jitter sync before init");

    expectTrue(pass->preflightJitterNdc(&jitterReject), "pass preflightJitterNdc passes before init");
               "pass preflightJitterNdc reject reason is None");

void testTaaPassWarmupAndJitterPreflightFollowUp() {
    passDesc.width = 96;
    passDesc.height = 96;
    passDesc.params.blend_factor = 0.18f;

    const fuse::renderer::TaaHistoryWarmupPreflight preInitWarmup = pass->preflightHistoryWarmup();
    expectTrue(preInitWarmup.needs_warmup, "pass warmup preflight needs warmup before init");
    expectTrue(pass->shouldSkipHistoryWarmup(), "pass should skip warmed state before init");
    expectTrue(!pass->shouldSkipJitterNdc(), "pass jitter NDC should not skip before init");

void testTaaPassWarmupAndResolveFrameGuards() {

    expectTrue(!pass->historyWarmupComplete(), "pass warmup not complete before init");
    expectTrue(!pass->shouldSkipJitterNdc(), "pass jitter NDC ready before init");
    expectTrue(!pass->shouldSkipJitterSync(3u), "pass jitter sync ready before init");

void testTaaPassJitterAndBlendReadyWrappers() {


    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass should-skip wrapper test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass temporal/jitter preflight test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass temporal/jitter skip test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass warmup/jitter preflight follow-up test");

    auto bootstrap = createTaaTestBootstrap();
    if (bootstrap == nullptr) {
        std::fprintf(stderr, "SKIP: pass temporal/jitter preflight wrappers (Vulkan device unavailable)\n");
        return;
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass warmup/resolve frame test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass jitter/blend ready wrappers");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);
    expectTrue(pass->init(resources), "TaaPass initialized for should-skip wrapper test");
    expectTrue(pass->shouldSkipHistoryWarmup(), "pass should skip warmup after init");
    expectTrue(pass->init(resources), "TaaPass initialized for temporal/jitter preflight test");
    expectTrue(pass->init(resources), "TaaPass initialized for temporal/jitter skip test");
    expectTrue(pass->init(resources), "TaaPass initialized for temporal/jitter wrapper test");
    expectTrue(pass->classifyHistoryWarmupState() == fuse::renderer::TaaHistoryWarmupState::NeedsWarmup,
               "pass warm-up state is NeedsWarmup after init");
    expectTrue(pass->shouldSkipHistoryWarmup(), "pass should skip warm-up completion after init");
    expectTrue(pass->init(resources), "TaaPass initialized for warmup/resolve frame test");

    expectTrue(pass->shouldSkipHistoryWarmup(), "pass should skip warmup before first resolve");
    expectTrue(pass->preflightJitterNdc(), "pass jitter NDC preflight passes");
    expectTrue(pass->preflightJitterSync(4u), "pass jitter sync preflight passes");
    expectTrue(pass->init(resources), "TaaPass initialized for jitter/blend ready wrappers");

    fuse::renderer::TaaResolveDesc resolveDesc{};
    resolveDesc.width = 128;
    resolveDesc.height = 128;
    resolveDesc.surfaces.current_frame = reinterpret_cast<void*>(0x10);
    resolveDesc.surfaces.output = reinterpret_cast<void*>(0x20);
    resolveDesc.params = passDesc.params;
    expectTrue(pass->preflightResolveBlend(resolveDesc), "pass blend preflight passes on warmup frame");

    expectTrue(pass->resolveFrame(resolveDesc), "initial resolve warms pass history");
    expectTrue(pass->preflightHistoryReuse(0u, &reuseReason), "pass reuse preflight passes after warmup");
    expectTrue(pass->preflightResolveBlend(resolveDesc), "pass blend preflight passes after warmup");

    expectTrue(pass->syncJitterToFrameIndexIfReady(11u), "pass guarded jitter sync succeeds after init");
    expectTrue(pass->isJitterSyncedToFrameIndex(11u), "pass jitter synced after guarded sync");

    expectTrue(!pass->invalidateHistoryIfStale(0u), "current generation does not invalidate pass history");
    expectTrue(pass->preflightHistoryReuse(0u), "pass reuse still valid with current generation");

    expectTrue(pass->invalidateHistoryIfStale(99u), "stale generation invalidates pass history");
               "pass reuse preflight fails after stale invalidate");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::StaleGeneration,
               "pass reuse preflight reports StaleGeneration after invalidate");

    pass->destroy();

void testHistoryWarmupPreflight() {
void testPreflightTaaHistoryWarmup() {
void testHistoryWarmupPreflightGuards() {
    expectTrue(std::strcmp(fuse::renderer::taaHistoryWarmupBlockReasonLabel(
                               fuse::renderer::TaaHistoryWarmupBlockReason::None),
                           "none") == 0,
               "None warmup block label");
                               fuse::renderer::TaaHistoryWarmupBlockReason::NotReady),
               "NotReady warmup block label");
                               fuse::renderer::TaaHistoryWarmupBlockReason::NeedsResolve),
                           "needs_resolve") == 0,
               "NeedsResolve warmup block label");

    fuse::renderer::TaaHistoryWarmupBlockReason reason = fuse::renderer::TaaHistoryWarmupBlockReason::None;
    expectTrue(fuse::renderer::classifyTaaHistoryWarmupBlock(emptyHistory) ==
                   fuse::renderer::TaaHistoryWarmupBlockReason::NotReady,
               "empty history classified as NotReady for warmup");
    expectTrue(!fuse::renderer::preflightTaaHistoryWarmup(emptyHistory, &reason),
               "empty history fails warmup preflight");
    expectTrue(reason == fuse::renderer::TaaHistoryWarmupBlockReason::NotReady,
               "empty history warmup preflight reason is NotReady");
    expectTrue(!fuse::renderer::taaHistoryIsWarmed(emptyHistory), "empty history is not warmed");

    expectTrue(bootstrap != nullptr, "bootstrap allocated for warmup preflight test");



    expectTrue(history.init(resources, historyDesc), "history ready for warmup preflight test");
    expectTrue(fuse::renderer::preflightTaaHistoryWarmup(history, &reason),
               "allocated history passes warmup preflight");
    expectTrue(reason == fuse::renderer::TaaHistoryWarmupBlockReason::None,
               "allocated history warmup preflight reason is None");
    expectTrue(!fuse::renderer::taaHistoryIsWarmed(history), "allocated but unwarmed history is not warmed");

    expectTrue(fuse::renderer::taaHistoryIsWarmed(history), "resolved history is warmed");


void testPreflightTaaHistoryReuseForResolve() {
    expectTrue(bootstrap != nullptr, "bootstrap allocated for reuse-for-resolve preflight test");



    expectTrue(history.init(resources, historyDesc), "history ready for reuse-for-resolve preflight test");

    desc.observed_history_generation = fuse::renderer::kTaaResolveNoHistoryGeneration;

    expectTrue(!fuse::renderer::preflightTaaHistoryReuseForResolve(desc, history, &reason),
               "unwarmed history fails reuse-for-resolve preflight with sentinel");
               "reuse-for-resolve preflight reason is NotWarm before warmup");

    expectTrue(fuse::renderer::preflightTaaHistoryReuseForResolve(desc, history, &reason),
               "warmed history passes reuse-for-resolve preflight with sentinel");
               "warmed history reuse-for-resolve preflight reason is None");

               "stale generation fails reuse-for-resolve preflight");
               "stale generation reuse-for-resolve preflight reason is StaleGeneration");


void testJitterSyncPreflight() {

    expectTrue(std::strcmp(fuse::renderer::taaJitterSyncBlockReasonLabel(
                               fuse::renderer::TaaJitterSyncBlockReason::None),
               "None jitter sync block label");
                               fuse::renderer::TaaJitterSyncBlockReason::InvalidSequence),
                           "invalid_sequence") == 0,
               "InvalidSequence jitter sync block label");
                               fuse::renderer::TaaJitterSyncBlockReason::Misaligned),
                           "misaligned") == 0,
               "Misaligned jitter sync block label");

    expectTrue(TaaJitterLayout::expectedSlotForFrameIndex(5u, 8u) == 5u,
               "expectedSlotForFrameIndex matches frameIndexInSequence");
    expectTrue(TaaJitterLayout::expectedSlotForFrameIndex(13u, 8u) == 5u,
               "expectedSlotForFrameIndex wraps with sequence period");

    expectTrue(jitter.needsResyncToFrameIndex(5u), "default jitter needs resync to frame five");
    expectTrue(fuse::renderer::classifyTaaJitterSyncBlock(jitter, 5u) ==
                   fuse::renderer::TaaJitterSyncBlockReason::Misaligned,
               "unsynced jitter classified as Misaligned");

    fuse::renderer::TaaJitterSyncBlockReason reason = fuse::renderer::TaaJitterSyncBlockReason::None;
    expectTrue(!fuse::renderer::preflightTaaJitterSync(jitter, 5u, &reason),
               "unsynced jitter fails sync preflight");
    expectTrue(reason == fuse::renderer::TaaJitterSyncBlockReason::Misaligned,
               "unsynced jitter sync preflight reason is Misaligned");

    expectTrue(!jitter.needsResyncToFrameIndex(5u), "synced jitter does not need resync");
    expectTrue(fuse::renderer::preflightTaaJitterSync(jitter, 5u, &reason),
               "synced jitter passes sync preflight");
    expectTrue(reason == fuse::renderer::TaaJitterSyncBlockReason::None,
               "synced jitter sync preflight reason is None");

    expectTrue(fallbackJitter.needsResyncToFrameIndex(3u), "fallback jitter needs resync to frame three from default state");
    expectTrue(fallbackJitter.syncToFrameIndexIfReady(3u), "fallback jitter syncs after default-length fallback");
    expectTrue(!fallbackJitter.needsResyncToFrameIndex(3u), "fallback jitter aligned after sync");

void testPreflightTaaResolveFrame() {
    expectTrue(bootstrap != nullptr, "bootstrap allocated for resolve-frame preflight test");



    expectTrue(history.init(resources, historyDesc), "history ready for resolve-frame preflight test");

    desc.width = 64;
    desc.height = 64;
    desc.surfaces.current_frame = reinterpret_cast<void*>(0x1);
    desc.surfaces.output = reinterpret_cast<void*>(0x2);

    fuse::renderer::TaaResolveSkipReason skipReason = fuse::renderer::TaaResolveSkipReason::None;
    fuse::renderer::TaaResolveBlendRejectReason blendReason =
    expectTrue(fuse::renderer::preflightTaaResolveFrame(desc, history, &skipReason, &blendReason),
               "valid resolve desc passes combined preflight");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::None,
               "combined preflight skip reason is None");
    expectTrue(blendReason == fuse::renderer::TaaResolveBlendRejectReason::None,
               "combined preflight blend reject reason is None");

    desc.width = 0;
    expectTrue(!fuse::renderer::preflightTaaResolveFrame(desc, history, &skipReason, &blendReason),
               "invalid dimensions fail combined preflight");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::InvalidDimensions,
               "combined preflight reports InvalidDimensions");


void testTaaPassWarmupAndResolveFramePreflight() {
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass warmup/resolve preflight test");



    passDesc.width = 64;
    passDesc.height = 64;

    expectTrue(pass->init(resources), "TaaPass initialized for warmup/resolve preflight test");

    fuse::renderer::TaaHistoryWarmupBlockReason warmupReason = fuse::renderer::TaaHistoryWarmupBlockReason::None;
    expectTrue(pass->preflightHistoryWarmup(&warmupReason), "pass warmup preflight passes after init");
    expectTrue(pass->warmupFramesRemaining() == 1u, "pass reports one warmup frame remaining before resolve");

    resolveDesc.width = 64;
    resolveDesc.height = 64;

    expectTrue(!pass->preflightHistoryReuseForResolve(resolveDesc, &reuseReason),
               "pass reuse-for-resolve preflight fails before warmup");
               "pass reuse-for-resolve preflight reason is NotWarm before warmup");

    fuse::renderer::TaaJitterSyncBlockReason jitterReason = fuse::renderer::TaaJitterSyncBlockReason::None;
    expectTrue(!pass->preflightJitterSync(4u, &jitterReason), "pass jitter not synced to frame four");
    expectTrue(jitterReason == fuse::renderer::TaaJitterSyncBlockReason::Misaligned,
               "pass jitter sync preflight reason is Misaligned");
    expectTrue(pass->syncJitterToFrameIndexIfReady(4u), "pass syncs jitter to frame four");
    expectTrue(pass->preflightJitterSync(4u, &jitterReason), "pass jitter sync preflight passes after sync");

    expectTrue(pass->preflightResolveFrame(resolveDesc, &skipReason, &blendReason),
               "pass combined resolve preflight passes before first resolve");

    expectTrue(pass->warmupFramesRemaining() == 0u, "pass reports zero warmup frames after resolve");
    expectTrue(pass->preflightHistoryReuseForResolve(resolveDesc, &reuseReason),
               "pass reuse-for-resolve preflight passes after warmup");


void testJitterSyncRejectGuards() {

    expectTrue(std::strcmp(fuse::renderer::taaJitterSyncRejectReasonLabel(
                               fuse::renderer::TaaJitterSyncRejectReason::None),
               "None jitter sync reject label");
                               fuse::renderer::TaaJitterSyncRejectReason::InvalidSequence),
               "InvalidSequence jitter sync reject label");

    expectTrue(fuse::renderer::classifyTaaJitterSyncReject(5u, 8u) ==
                   fuse::renderer::TaaJitterSyncRejectReason::None,
               "valid sequence passes jitter sync classify");
    expectTrue(fuse::renderer::classifyTaaJitterSyncReject(5u, 0u) ==
                   fuse::renderer::TaaJitterSyncRejectReason::InvalidSequence,
               "invalid sequence fails jitter sync classify");

    fuse::renderer::TaaJitterSyncRejectReason syncReason = fuse::renderer::TaaJitterSyncRejectReason::None;
    expectTrue(fuse::renderer::preflightTaaJitterSync(13u, 8u, &syncReason),
               "preflightTaaJitterSync passes for valid sequence");
    expectTrue(syncReason == fuse::renderer::TaaJitterSyncRejectReason::None,
               "valid jitter sync preflight reason is None");
    expectTrue(fuse::renderer::canPreflightTaaJitterSync(13u, 8u),
               "canPreflightTaaJitterSync passes for valid sequence");
    expectTrue(!fuse::renderer::preflightTaaJitterSync(0u, 0u, &syncReason),
               "preflightTaaJitterSync rejects invalid sequence");
    expectTrue(syncReason == fuse::renderer::TaaJitterSyncRejectReason::InvalidSequence,
               "invalid jitter sync preflight reason is InvalidSequence");

    expectTrue(jitter.preflightSyncToFrameIndex(4u, &syncReason),
               "jitter preflightSyncToFrameIndex passes for valid sequence");
    expectTrue(jitter.classifySyncReject(4u) == fuse::renderer::TaaJitterSyncRejectReason::None,
               "jitter classifySyncReject passes for valid sequence");

void testJitterNdcRejectGuards() {
    expectTrue(std::strcmp(fuse::renderer::taaJitterNdcRejectReasonLabel(
                               fuse::renderer::TaaJitterNdcRejectReason::None),
               "None jitter NDC reject label");
                               fuse::renderer::TaaJitterNdcRejectReason::InvalidViewport),
                           "invalid_viewport") == 0,
               "InvalidViewport jitter NDC reject label");

    expectTrue(fuse::renderer::classifyTaaJitterNdcReject(1920u, 1080u, 8u) ==
                   fuse::renderer::TaaJitterNdcRejectReason::None,
               "valid viewport passes jitter NDC classify");
    expectTrue(fuse::renderer::classifyTaaJitterNdcReject(0u, 1080u, 8u) ==
                   fuse::renderer::TaaJitterNdcRejectReason::InvalidViewport,
               "zero width fails jitter NDC classify");
    expectTrue(fuse::renderer::classifyTaaJitterNdcReject(1920u, 1080u, 0u) ==
                   fuse::renderer::TaaJitterNdcRejectReason::InvalidSequence,
               "invalid sequence fails jitter NDC classify");

    fuse::renderer::TaaJitterNdcRejectReason ndcReason = fuse::renderer::TaaJitterNdcRejectReason::None;
    expectTrue(fuse::renderer::preflightTaaJitterNdc(128u, 128u, 8u, &ndcReason),
               "preflightTaaJitterNdc passes for valid viewport");
    expectTrue(fuse::renderer::canPreflightTaaJitterNdc(128u, 128u, 8u),
               "canPreflightTaaJitterNdc passes for valid viewport");
    expectTrue(!fuse::renderer::preflightTaaJitterNdc(0u, 128u, 8u, &ndcReason),
               "preflightTaaJitterNdc rejects zero width");
    expectTrue(ndcReason == fuse::renderer::TaaJitterNdcRejectReason::InvalidViewport,
               "zero width NDC preflight reason is InvalidViewport");

    expectTrue(jitter.preflightCurrentNdcOffset(128u, 128u, &ndcReason),
               "jitter preflightCurrentNdcOffset passes for valid viewport");
    expectTrue(jitter.classifyNdcReject(128u, 128u) == fuse::renderer::TaaJitterNdcRejectReason::None,
               "jitter classifyNdcReject passes for valid viewport");


    fuse::renderer::TaaHistoryBuffer emptyHistory;
                               fuse::renderer::TaaHistoryWarmupBlockReason::NeedsWarmup),
                           "needs_warmup") == 0,
               "NeedsWarmup warmup block label");





    fuse::renderer::TaaHistoryWarmupBlockReason warmupReason =
        fuse::renderer::TaaHistoryWarmupBlockReason::None;
               "empty history warmup block is NotReady");
    expectTrue(!fuse::renderer::preflightTaaHistoryWarmup(emptyHistory, &warmupReason),
    expectTrue(warmupReason == fuse::renderer::TaaHistoryWarmupBlockReason::NotReady,




    expectTrue(fuse::renderer::classifyTaaHistoryWarmupBlock(history) ==
                   fuse::renderer::TaaHistoryWarmupBlockReason::NeedsWarmup,
               "unwarmed history warmup block is NeedsWarmup");

    expectTrue(!fuse::renderer::preflightTaaHistoryWarmup(history, &warmupReason),
               "unwarmed history fails warmup preflight");
    expectTrue(warmupReason == fuse::renderer::TaaHistoryWarmupBlockReason::NeedsWarmup,
               "unwarmed history warmup preflight reason is NeedsWarmup");
    expectTrue(!fuse::renderer::canPreflightTaaHistoryWarmup(history),
               "canPreflightTaaHistoryWarmup fails before first resolve");

    expectTrue(fuse::renderer::preflightTaaHistoryWarmup(history, &warmupReason),
               "warmed history passes warmup preflight");
    expectTrue(fuse::renderer::canPreflightTaaHistoryWarmup(history),
               "canPreflightTaaHistoryWarmup passes after warmup");
    expectTrue(fuse::renderer::canPreflightTaaHistoryReuse(history, 0u),
               "canPreflightTaaHistoryReuse passes after warmup");


void testResolveTemporalBlendPreflightGuards() {
    expectTrue(std::strcmp(fuse::renderer::taaResolveTemporalRejectReasonLabel(
                               fuse::renderer::TaaResolveTemporalRejectReason::None),
               "None temporal reject label");
                               fuse::renderer::TaaResolveTemporalRejectReason::HistoryReuseBlocked),
                           "history_reuse_blocked") == 0,
               "HistoryReuseBlocked temporal reject label");

    expectTrue(bootstrap != nullptr, "bootstrap allocated for temporal blend preflight test");



    expectTrue(history.init(resources, historyDesc), "history ready for temporal blend preflight test");






    fuse::renderer::TaaResolveTemporalRejectReason temporalReason =
        fuse::renderer::TaaResolveTemporalRejectReason::None;
    expectTrue(fuse::renderer::classifyTaaResolveTemporalReject(desc, history) ==
                   fuse::renderer::TaaResolveTemporalRejectReason::HistoryReuseBlocked,
               "unwarmed history blocks temporal blend classify");
    expectTrue(!fuse::renderer::preflightTaaResolveTemporalBlend(desc, history, &temporalReason),
               "unwarmed history fails temporal blend preflight");
    expectTrue(temporalReason == fuse::renderer::TaaResolveTemporalRejectReason::HistoryReuseBlocked,
               "unwarmed temporal preflight reason is HistoryReuseBlocked");
    expectTrue(!fuse::renderer::taaHistoryTemporalBlendReady(desc, history),
               "temporal blend not ready before warmup");

    expectTrue(fuse::renderer::preflightTaaResolveTemporalBlend(desc, history, &temporalReason),
               "warmed history passes temporal blend preflight");
    expectTrue(fuse::renderer::canPreflightTaaResolveTemporalBlend(desc, history),
               "canPreflightTaaResolveTemporalBlend passes after warmup");
    expectTrue(fuse::renderer::taaHistoryTemporalBlendReady(desc, history),
               "temporal blend ready after warmup");

    expectTrue(fuse::renderer::tryComputeTaaResolveBlendWeights(desc, history, weights, &blendReason),
               "tryComputeTaaResolveBlendWeights succeeds after warmup");
    expectNear(weights.current, 0.2f, 1e-5f, "tryCompute fills configured current blend");
    expectNear(weights.history, 0.8f, 1e-5f, "tryCompute fills history blend complement");
    expectTrue(fuse::renderer::canPreflightTaaResolveBlendWeights(desc, history),
               "canPreflightTaaResolveBlendWeights passes after warmup");


void testTaaPassDeepenPreflightWrappers() {
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass deepen preflight wrappers test");



    passDesc.params.blend_factor = 0.35f;






    expectTrue(pass->init(resources), "TaaPass initialized for deepen preflight wrappers test");
    expectTrue(pass->canSyncJitterToFrameIndex(9u), "pass canSyncJitterToFrameIndex passes");
    expectTrue(pass->classifyJitterSyncReject(9u) == fuse::renderer::TaaJitterSyncRejectReason::None,
               "pass classifyJitterSyncReject passes for valid frame");

    expectTrue(pass->preflightJitterSync(9u, &syncReason), "pass preflightJitterSync passes");
    expectTrue(pass->syncJitterToFrameIndexIfReady(9u), "pass syncJitterToFrameIndexIfReady succeeds");
    expectTrue(pass->jitterAlignedToFrameIndex(9u), "pass jitter aligned after sync");

    expectTrue(!pass->canPreflightHistoryWarmup(), "pass warmup preflight fails before resolve");
    expectTrue(pass->classifyHistoryWarmupBlock() ==
               "pass classifies unwarmed history as NeedsWarmup");


    expectTrue(pass->preflightResolveBlendWeights(resolveDesc),
               "pass blend preflight passes before first resolve");
    expectTrue(!pass->temporalBlendReady(resolveDesc), "pass temporal blend not ready before resolve");

    expectTrue(pass->canPreflightHistoryWarmup(), "pass warmup preflight passes after resolve");
    expectTrue(pass->canPreflightHistoryReuse(0u), "pass reuse preflight passes after resolve");
    expectTrue(pass->temporalBlendReady(resolveDesc), "pass temporal blend ready after resolve");
    expectTrue(pass->canPreflightResolveTemporalBlend(resolveDesc),
               "pass temporal blend preflight passes after resolve");

    expectTrue(pass->tryExpectedResolveBlendWeights(resolveDesc, weights),
               "pass tryExpectedResolveBlendWeights succeeds after resolve");
    expectNear(weights.current, 0.35f, 1e-5f, "pass tryExpected fills configured current blend");


void testSafeNdcOffsetGuards() {

    const fuse::math::Vec2 safe =
        TaaJitterLayout::safeHaltonNdcOffset(0u, 1920u, 1080u, 8u);
    const fuse::math::Vec2 direct =
        TaaJitterLayout::haltonNdcOffset(0u, 1920u, 1080u, 8u);
    expectNear(safe.x, direct.x, 1e-6f, "safeHaltonNdcOffset matches haltonNdcOffset for valid viewport");
    expectNear(safe.y, direct.y, 1e-6f, "safeHaltonNdcOffset Y matches haltonNdcOffset for valid viewport");

    const fuse::math::Vec2 invalid = TaaJitterLayout::safeHaltonNdcOffset(0u, 0u, 1080u, 8u);
    expectNear(invalid.x, 0.f, 1e-6f, "safeHaltonNdcOffset returns zero X for invalid viewport");
    expectNear(invalid.y, 0.f, 1e-6f, "safeHaltonNdcOffset returns zero Y for invalid viewport");

    const fuse::math::Vec2 safeFrame =
        TaaJitterLayout::safeNdcOffsetForFrameIndex(3u, 128u, 128u, 8u);
    const fuse::math::Vec2 frameDirect =
        TaaJitterLayout::ndcOffsetForFrameIndex(3u, 128u, 128u, 8u);
    expectNear(safeFrame.x, frameDirect.x, 1e-6f, "safeNdcOffsetForFrameIndex matches ndcOffsetForFrameIndex");
    expectNear(safeFrame.y, frameDirect.y, 1e-6f, "safeNdcOffsetForFrameIndex Y matches ndcOffsetForFrameIndex");

    const fuse::math::Vec2 invalidFrame = TaaJitterLayout::safeNdcOffsetForFrameIndex(3u, 0u, 128u, 8u);
    expectNear(invalidFrame.x, 0.f, 1e-6f, "safeNdcOffsetForFrameIndex returns zero for invalid viewport");

void testJitterSyncBlockGuards() {
    using fuse::renderer::TaaJitterSyncBlockReason;

    expectTrue(fuse::renderer::classifyTaaJitterSyncBlock(jitter, 0u) == TaaJitterSyncBlockReason::None,
               "default jitter aligned to frame zero");
    expectTrue(fuse::renderer::preflightTaaJitterSync(jitter, 0u), "default jitter passes sync preflight");
    expectTrue(!fuse::renderer::taaJitterNeedsResyncToFrameIndex(jitter, 0u),
               "default jitter does not need resync to frame zero");

    expectTrue(fuse::renderer::classifyTaaJitterSyncBlock(jitter, 0u) ==
                   TaaJitterSyncBlockReason::DriftedFromFrame,
               "advanced jitter drifted from frame zero");
    expectTrue(fuse::renderer::taaJitterNeedsResyncToFrameIndex(jitter, 0u),
               "advanced jitter needs resync to frame zero");
    expectTrue(std::strcmp(fuse::renderer::taaJitterSyncBlockReasonLabel(TaaJitterSyncBlockReason::DriftedFromFrame),
                           "drifted_from_frame") == 0,
               "DriftedFromFrame sync block label");

    expectTrue(fuse::renderer::preflightTaaJitterSync(jitter, 5u), "synced jitter passes preflight");
    expectTrue(!fuse::renderer::taaJitterNeedsResyncToFrameIndex(jitter, 5u),
               "synced jitter does not need resync");

    expectTrue(fallbackJitter.sequenceLength() == 8u, "invalid desc falls back for sync block test");
    expectTrue(fuse::renderer::preflightTaaJitterSync(fallbackJitter, 0u),
               "fallback jitter passes sync preflight");

void testBlendFactorRangeGuards() {
    expectTrue(fuse::renderer::isTaaBlendFactorInRange(0.f), "zero blend factor in range");
    expectTrue(fuse::renderer::isTaaBlendFactorInRange(1.f), "unit blend factor in range");
    expectTrue(!fuse::renderer::isTaaBlendFactorInRange(-0.1f), "negative blend factor out of range");
    expectTrue(!fuse::renderer::isTaaBlendFactorInRange(1.1f), "blend factor above one out of range");

    expectTrue(fuse::renderer::taaUsesWarmupBlend(true), "first frame uses warmup blend");
    expectTrue(!fuse::renderer::taaUsesWarmupBlend(false), "subsequent frame does not use warmup blend");

    expectTrue(!fuse::renderer::taaBlendUsesHistory(1.f), "full current weight does not use history");
    expectTrue(fuse::renderer::taaBlendUsesHistory(0.2f), "partial current weight uses history");
    expectTrue(fuse::renderer::taaBlendSkipsHistoryReuse(1.f), "full current weight skips history reuse");
    expectTrue(!fuse::renderer::taaBlendSkipsHistoryReuse(0.2f), "partial current weight does not skip history");

    fuse::renderer::TAAParams params{};
    params.blend_factor = 0.3f;
    expectNear(fuse::renderer::computeEffectiveBlend(false, true, params), 0.3f, 1e-5f,
               "reusable history uses configured blend");
    expectNear(fuse::renderer::computeEffectiveBlend(false, false, params), 1.f, 1e-5f,
               "non-reusable history forces full current blend");

    const fuse::renderer::TaaBlendWeights guarded =
        fuse::renderer::computeTaaBlendWeightsWithReuseGuard(false, true, params);
    expectNear(guarded.current, 0.3f, 1e-5f, "reuse guard blend uses configured current");
    expectNear(guarded.history, 0.7f, 1e-5f, "reuse guard blend uses history complement");

    expectTrue(!fuse::renderer::taaHistoryWarmupComplete(emptyHistory),
               "empty history warmup is not complete");
    expectTrue(reason == fuse::renderer::TaaHistoryReuseBlockReason::NotReady,




    expectTrue(!fuse::renderer::preflightTaaHistoryWarmup(history, &reason),
               "unwarmed history warmup preflight reason is NotWarm");

               "warmed history warmup preflight reason is None");


void testResolveFramePreflightGuards() {
    expectTrue(bootstrap != nullptr, "bootstrap allocated for resolve frame preflight test");



    expectTrue(history.init(resources, historyDesc), "history ready for resolve frame preflight test");

    desc.params.blend_factor = 0.25f;

    expectTrue(fuse::renderer::taaResolveSurfacesSatisfied(desc), "resolve surfaces satisfied");
    expectTrue(fuse::renderer::canAttemptTaaResolve(desc, history), "can attempt resolve before warmup");
    expectTrue(!fuse::renderer::taaResolveWillReuseHistory(desc, history),
               "resolve will not reuse history before warmup");

               "resolve frame preflight passes before warmup");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::None, "resolve frame skip reason is None");
               "resolve frame blend reject reason is None");

    fuse::renderer::TaaResolveDesc prepared = desc;
    expectTrue(fuse::renderer::prepareTaaResolveDesc(prepared, history),
               "prepareTaaResolveDesc succeeds before warmup");
    expectTrue(prepared.observed_history_generation == 0u, "prepare stamps observed generation");

    expectTrue(fuse::renderer::taaResolveWillReuseHistory(desc, history),
               "resolve will reuse history after warmup");
               "resolve frame preflight passes for valid desc");
               "resolve frame preflight skip reason is None");
               "resolve frame preflight blend reason is None");

               "resolve frame preflight rejects invalid dimensions");
               "resolve frame preflight reports InvalidDimensions");
               "resolve frame preflight skips blend check when skip fails");




                   fuse::renderer::TaaHistoryWarmupBlockReason::NeedsResolve,
               "allocated history classified as NeedsResolve before first resolve");
    expectTrue(reason == fuse::renderer::TaaHistoryWarmupBlockReason::NeedsResolve,
               "unwarmed history warmup preflight reason is NeedsResolve");

                   fuse::renderer::TaaHistoryWarmupBlockReason::None,
               "warmed history classified as None for warmup");
    expectTrue(fuse::renderer::taaHistoryWarmupComplete(history),
               "warmed history warmup is complete");


    expectTrue(fuse::renderer::taaHistoryWarmupComplete(history), "warmed history warmup complete");
    expectTrue(warmupReason == fuse::renderer::TaaHistoryWarmupBlockReason::None,


void testTaaPassResolveFrameAndJitterGuards() {
void testTaaPassWarmupBlendAndJitterDriftGuards() {

    expectTrue(pass->classifyHistoryWarmupState() == fuse::renderer::TaaHistoryWarmupState::NotReady,
               "pass warmup state is NotReady before init");
    expectTrue(pass->jitterNeedsSyncToFrameIndex(4u), "pass jitter needs sync before init");
void testPreflightTaaResolveHistoryReuse() {
    fuse::renderer::TaaHistoryReuseBlockReason reason = fuse::renderer::TaaHistoryReuseBlockReason::None;
    expectTrue(!fuse::renderer::preflightTaaResolveHistoryReuse(desc, emptyHistory, &reason),
               "empty history fails resolve-history reuse preflight");
               "empty history resolve-history reuse reason is NotReady");

    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass resolve frame guard test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass warmup/blend/drift test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for resolve-history reuse test");



    passDesc.params.blend_factor = 0.2f;

    expectTrue(pass->init(resources), "TaaPass initialized for resolve frame guard test");
    expectTrue(pass->historyWarmupFramesRemaining() == 1u, "pass reports one warmup frame remaining");
    expectTrue(!pass->historyWarmupComplete(), "pass warmup not complete before first resolve");


    expectTrue(pass->canResolveFrame(resolveDesc), "pass can resolve before warmup");
    expectTrue(!pass->resolveWouldReuseHistory(resolveDesc), "pass would not reuse history before warmup");
    expectTrue(pass->init(resources), "TaaPass initialized for warmup/blend/drift test");

    fuse::renderer::TaaHistoryWarmupState warmupState = fuse::renderer::TaaHistoryWarmupState::Complete;
    expectTrue(!pass->preflightHistoryWarmup(&warmupState),
               "pass warmup preflight fails before first resolve");
    expectTrue(warmupState == fuse::renderer::TaaHistoryWarmupState::AwaitingFirstResolve,
               "pass warmup preflight reports AwaitingFirstResolve");

    expectTrue(pass->classifyResolveBlendMode(resolveDesc) == fuse::renderer::TaaResolveBlendMode::Warmup,
               "pass blend mode is Warmup before first resolve");
    expectTrue(history.init(resources, historyDesc), "history ready for resolve-history reuse test");

    expectTrue(!fuse::renderer::preflightTaaResolveHistoryReuse(desc, history, &reason),
               "unwarmed history fails resolve-history reuse preflight");
               "unwarmed history resolve-history reuse reason is NotWarm");

    expectTrue(fuse::renderer::preflightTaaResolveHistoryReuse(desc, history, &reason),
               "warmed history passes resolve-history reuse preflight with matching generation");

               "sentinel observed generation bypasses stale guard for resolve-history reuse");

               "stale generation fails resolve-history reuse preflight");
               "stale generation resolve-history reuse reason is StaleGeneration");


void testJitterSyncPreflightGuards() {
    using fuse::renderer::TaaJitterLayout;

                               fuse::renderer::TaaJitterSyncBlockReason::InvalidViewport),
               "InvalidViewport jitter sync block label");

    fuse::renderer::TaaJitterSyncBlockReason syncReason = fuse::renderer::TaaJitterSyncBlockReason::None;
    expectTrue(fuse::renderer::preflightTaaJitterSync(5u, 1920u, 1080u, 8u, &syncReason),
               "valid viewport and sequence pass jitter sync preflight");
    expectTrue(syncReason == fuse::renderer::TaaJitterSyncBlockReason::None,

    expectTrue(fuse::renderer::classifyTaaJitterSyncBlock(5u, 1920u, 1080u, 8u) ==
                   fuse::renderer::TaaJitterSyncBlockReason::None,
               "valid jitter sync classify returns None");

    expectTrue(!fuse::renderer::preflightTaaJitterSync(0u, 0u, 1080u, 8u, &syncReason),
               "zero width fails jitter sync preflight");
    expectTrue(syncReason == fuse::renderer::TaaJitterSyncBlockReason::InvalidViewport,
               "zero width jitter sync preflight reason is InvalidViewport");

    expectTrue(!fuse::renderer::preflightTaaJitterSync(0u, 1920u, 1080u, 0u, &syncReason),
               "invalid sequence fails jitter sync preflight");
    expectTrue(syncReason == fuse::renderer::TaaJitterSyncBlockReason::InvalidSequence,
               "invalid sequence jitter sync preflight reason is InvalidSequence");

    expectTrue(jitter.preflightSyncToFrameIndex(4u, 128u, 128u, &syncReason),
               "jitter instance preflight sync passes for valid viewport");
    expectTrue(!jitter.isAlignedToFrameIndex(4u), "jitter not aligned before viewport sync");
    expectTrue(jitter.syncToFrameIndexIfViewportReady(4u, 128u, 128u),
               "syncToFrameIndexIfViewportReady succeeds for valid viewport");
    expectTrue(jitter.isAlignedToFrameIndex(4u), "jitter aligned after viewport sync");
    expectTrue(!jitter.syncToFrameIndexIfViewportReady(6u, 0u, 128u),
               "syncToFrameIndexIfViewportReady blocks zero-width viewport");

    const fuse::math::Vec2 expected =
        TaaJitterLayout::ndcOffsetForFrameIndex(4u, 128u, 128u, 8u);
    const fuse::math::Vec2 synced = jitter.currentNdcOffset(128u, 128u);
    expectNear(synced.x, expected.x, 1e-6f, "viewport sync produces expected NDC X");
    expectNear(synced.y, expected.y, 1e-6f, "viewport sync produces expected NDC Y");

void testPreflightTaaResolveWithBlend() {
    expectTrue(bootstrap != nullptr, "bootstrap allocated for combined preflight test");



    expectTrue(history.init(resources, historyDesc), "history ready for combined preflight test");


               "pass resolve frame preflight passes before warmup");

    fuse::renderer::TaaResolveDesc prepared = resolveDesc;
    expectTrue(pass->prepareAndCanResolve(prepared), "pass prepareAndCanResolve succeeds");
    expectTrue(pass->historyWarmupComplete(), "pass warmup complete after first resolve");
    expectTrue(pass->historyWarmupFramesRemaining() == 0u, "pass reports zero warmup frames remaining");
    expectTrue(pass->resolveWouldReuseHistory(resolveDesc), "pass would reuse history after warmup");

    expectTrue(pass->jitterNeedsResyncToFrameIndex(99u), "pass jitter needs resync to distant frame");
    expectTrue(!pass->preflightJitterSync(99u, &jitterReason), "pass jitter sync preflight fails when drifted");
    expectTrue(jitterReason == fuse::renderer::TaaJitterSyncBlockReason::DriftedFromFrame,
               "pass jitter sync preflight reason is DriftedFromFrame");
    expectTrue(pass->syncJitterToFrameIndexIfReady(99u), "pass syncIfReady realigns jitter");
    expectTrue(pass->preflightJitterSync(99u, &jitterReason), "pass jitter sync preflight passes after sync");
    expectTrue(!pass->jitterNeedsResyncToFrameIndex(99u), "pass jitter aligned after sync");

    expectTrue(history.init(resources, historyDesc), "history ready for effective generation test");

    expectTrue(fuse::renderer::effectiveObservedHistoryGeneration(desc, history) == 0u,
               "sentinel resolves to current invalidate generation");

    expectTrue(fuse::renderer::effectiveObservedHistoryGeneration(desc, history) == 1u,
               "sentinel tracks bumped invalidate generation");

               "explicit generation is preserved");


    expectTrue(bootstrap != nullptr, "bootstrap allocated for resolve history reuse preflight test");



    expectTrue(history.init(resources, historyDesc), "history ready for resolve history reuse preflight test");


               "unwarmed history fails resolve reuse preflight");
               "resolve reuse preflight reason is NotWarm before warmup");
    expectTrue(!fuse::renderer::taaHistoryReuseAllowedForResolve(desc, history),
               "resolve reuse not allowed before warmup");

               "warmed history passes resolve reuse preflight with sentinel");
    expectTrue(fuse::renderer::taaHistoryReuseAllowedForResolve(desc, history),
               "resolve reuse allowed after warmup with sentinel");

               "stale explicit generation fails resolve reuse preflight");
               "resolve reuse preflight reason is StaleGeneration");

    desc.observed_history_generation = history.invalidateGeneration();
               "current explicit generation passes resolve reuse preflight");


void testBlockingReasonHelpers() {
    expectTrue(!fuse::renderer::taaHistoryReuseBlockReasonIsBlocking(
                   fuse::renderer::TaaHistoryReuseBlockReason::None),
               "None history reuse block reason is not blocking");
    expectTrue(fuse::renderer::taaHistoryReuseBlockReasonIsBlocking(
                   fuse::renderer::TaaHistoryReuseBlockReason::NotWarm),
               "NotWarm history reuse block reason is blocking");

    expectTrue(!fuse::renderer::taaResolveBlendRejectReasonIsBlocking(
                   fuse::renderer::TaaResolveBlendRejectReason::None),
               "None blend reject reason is not blocking");
    expectTrue(fuse::renderer::taaResolveBlendRejectReasonIsBlocking(
                   fuse::renderer::TaaResolveBlendRejectReason::InvalidWeights),
               "InvalidWeights blend reject reason is blocking");

    expectTrue(!fuse::renderer::taaJitterSyncBlockReasonIsBlocking(
               "None jitter sync block reason is not blocking");
    expectTrue(fuse::renderer::taaJitterSyncBlockReasonIsBlocking(
               "InvalidSequence jitter sync block reason is blocking");

void testHistoryWarmupSatisfied() {
    expectTrue(!fuse::renderer::taaHistoryWarmupSatisfied(emptyHistory),
               "empty history does not satisfy warmup");

    expectTrue(bootstrap != nullptr, "bootstrap allocated for warmup satisfied test");



    expectTrue(history.init(resources, historyDesc), "history ready for warmup satisfied test");
    expectTrue(!fuse::renderer::taaHistoryWarmupSatisfied(history),
               "allocated but unwarmed history does not satisfy warmup");

    expectTrue(fuse::renderer::taaHistoryWarmupSatisfied(history), "warmed history satisfies warmup");


void testPreflightTaaHistoryReuseForDesc() {
    expectTrue(!fuse::renderer::preflightTaaHistoryReuseForDesc(desc, emptyHistory, &reason),
               "empty history fails desc reuse preflight");
               "empty history desc reuse preflight reason is NotReady");

    expectTrue(bootstrap != nullptr, "bootstrap allocated for desc reuse preflight test");



    expectTrue(history.init(resources, historyDesc), "history ready for desc reuse preflight test");

    expectTrue(!fuse::renderer::preflightTaaHistoryReuseForDesc(desc, history, &reason),
               "unwarmed history fails desc reuse preflight with sentinel");
               "unwarmed sentinel desc reuse preflight reason is NotWarm");

    expectTrue(fuse::renderer::preflightTaaHistoryReuseForDesc(desc, history, &reason),
               "warmed history passes desc reuse preflight with sentinel");

               "stale generation fails desc reuse preflight");
               "stale generation desc reuse preflight reason is StaleGeneration");

               "current generation passes desc reuse preflight");


void testPreflightTaaResolveGuards() {
    expectTrue(bootstrap != nullptr, "bootstrap allocated for resolve guards preflight test");



    expectTrue(history.init(resources, historyDesc), "history ready for resolve guards preflight test");


    expectTrue(fuse::renderer::preflightTaaResolveGuards(desc, history, &skipReason, &blendReason),
               "combined guards pass for valid warmup resolve desc");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::None, "combined guards skip reason is None");
               "combined guards blend reason is None");

    expectTrue(!fuse::renderer::preflightTaaResolveGuards(desc, history, &skipReason, &blendReason),
               "combined guards fail on invalid dimensions");
               "combined guards report InvalidDimensions");
               "combined guards clear blend reason when skip fails");


void testJitterSyncPreflightHelpers() {

    expectTrue(fuse::renderer::preflightTaaJitterSync(5u, 8u, &reason),
               "valid sequence passes jitter sync preflight");
               "valid sequence jitter sync preflight reason is None");
    expectTrue(fuse::renderer::classifyTaaJitterSyncBlock(1008u, 8u) ==
               "large frame index passes jitter sync classify");

    expectTrue(!fuse::renderer::preflightTaaJitterSync(0u, 0u, &reason),
    expectTrue(reason == fuse::renderer::TaaJitterSyncBlockReason::InvalidSequence,

    expectTrue(jitter.preflightSync(4u, &reason), "jitter instance preflight sync passes");
    expectTrue(jitter.classifySyncBlock(4u) == fuse::renderer::TaaJitterSyncBlockReason::None,
               "jitter classify sync block is None for valid sequence");
void testPreflightTaaResolveTemporal() {
    expectTrue(bootstrap != nullptr, "bootstrap allocated for temporal preflight test");



    expectTrue(history.init(resources, historyDesc), "history ready for temporal preflight test");


    expectTrue(fuse::renderer::preflightTaaResolveTemporal(desc, history, &reuseReason, &blendReason),
               "temporal preflight passes on warmup frame without history blend");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::None,
               "temporal preflight reuse reason is None on warmup");

               "temporal preflight passes after warmup");
               "temporal preflight reuse reason is None when warmed");

    expectTrue(!fuse::renderer::preflightTaaResolveTemporal(desc, history, &reuseReason, &blendReason),
               "temporal preflight fails when history blend is allowed but generation is stale");
               "temporal preflight reuse reason is StaleGeneration");


void testTryComputeTaaResolveBlendWeights() {
    expectTrue(bootstrap != nullptr, "bootstrap allocated for try compute blend weights test");



    expectTrue(history.init(resources, historyDesc), "history ready for try compute blend weights test");

    desc.params.blend_factor = 0.35f;

    expectTrue(fuse::renderer::tryComputeTaaResolveBlendWeights(desc, history, &weights),
               "tryCompute succeeds on warmup frame");
    expectNear(weights.current, 1.f, 1e-5f, "tryCompute warmup current weight");
    expectNear(weights.history, 0.f, 1e-5f, "tryCompute warmup history weight");
    expectTrue(!fuse::renderer::tryComputeTaaResolveBlendWeights(desc, history, nullptr),
               "tryCompute rejects null output");

               "tryCompute succeeds after warmup");
    expectNear(weights.current, 0.35f, 1e-5f, "tryCompute steady current weight");
    expectNear(weights.history, 0.65f, 1e-5f, "tryCompute steady history weight");


void testJitterNdcOffsetForFrameIndexIfReady() {

    fuse::math::Vec2 ndc{};
    expectTrue(TaaJitterLayout::ndcOffsetForFrameIndexIfReady(3u, 1920u, 1080u, &ndc, 8u),
               "ndcOffsetForFrameIndexIfReady succeeds for valid viewport");
    const fuse::math::Vec2 direct = TaaJitterLayout::ndcOffsetForFrameIndex(3u, 1920u, 1080u, 8u);
    expectNear(ndc.x, direct.x, 1e-6f, "ndcOffsetForFrameIndexIfReady matches direct offset X");
    expectNear(ndc.y, direct.y, 1e-6f, "ndcOffsetForFrameIndexIfReady matches direct offset Y");

    expectTrue(!TaaJitterLayout::ndcOffsetForFrameIndexIfReady(3u, 0u, 1080u, &ndc, 8u),
               "ndcOffsetForFrameIndexIfReady rejects zero width");
    expectTrue(!TaaJitterLayout::ndcOffsetForFrameIndexIfReady(3u, 1920u, 1080u, nullptr, 8u),
               "ndcOffsetForFrameIndexIfReady rejects null output");

void testJitterSyncIfMisalignedGuards() {
    expectTrue(jitter.needsSyncToFrameIndex(4u), "default jitter needs sync to frame four");
    expectTrue(jitter.syncToFrameIndexIfMisaligned(4u), "syncToFrameIndexIfMisaligned succeeds");
    expectTrue(jitter.isAlignedToFrameIndex(4u), "jitter aligned after syncIfMisaligned");
    expectTrue(!jitter.needsSyncToFrameIndex(4u), "aligned jitter does not need sync");

    const fuse::u32 indexBefore = jitter.index();
    expectTrue(jitter.syncToFrameIndexIfMisaligned(4u), "syncToFrameIndexIfMisaligned is idempotent when aligned");
    expectTrue(jitter.index() == indexBefore, "syncToFrameIndexIfMisaligned preserves slot when aligned");
    expectTrue(!fuse::renderer::wouldRejectTaaResolveBlendWeights(desc, history),
               "wouldReject false for warmup blend weights");

    expectTrue(fuse::renderer::tryPreflightTaaResolveBlendWeights(desc, history, rejectReason),
               "tryPreflight blend succeeds for warmup weights");
               "tryPreflight blend reject reason is None for warmup");

               "wouldReject false for steady blend weights");
               "tryPreflight blend succeeds after warmup");




    expectTrue(TaaJitterLayout::classifyTaaJitterSyncReject(5u, 8u) ==
               "valid sequence classified as None");
    expectTrue(TaaJitterLayout::classifyTaaJitterSyncReject(0u, 0u) ==
               "zero sequence classified as InvalidSequence");
    expectTrue(!TaaJitterLayout::wouldSkipSyncToFrameIndex(5u, 8u),
               "wouldSkip false for valid sequence");
    expectTrue(TaaJitterLayout::wouldSkipSyncToFrameIndex(0u, 0u),
               "wouldSkip true for invalid sequence");

    expectTrue(!jitter.wouldSkipSyncToFrameIndex(5u), "jitter wouldSkip false for valid sequence");
    expectTrue(jitter.trySyncToFrameIndexIfReady(5u, syncReason), "trySync succeeds for valid sequence");
               "trySync reason is None on success");
    expectTrue(jitter.isAlignedToFrameIndex(5u), "jitter aligned after trySync");

    fuse::renderer::TaaJitterDesc invalidDesc{};
    invalidDesc.sequence_length = 0u;
    fuse::renderer::TaaJitter fallbackJitter(invalidDesc);
    expectTrue(fallbackJitter.sequenceLength() == 8u, "fallback jitter uses default sequence length");
    expectTrue(fallbackJitter.preflightSync(2u, &reason), "fallback jitter preflight sync succeeds after fallback");
               "fallback jitter preflight sync reason is None after fallback");

void testJitterAdvanceIfAligned() {
    jitter.syncToFrameIndex(3u);
    expectTrue(jitter.isAlignedToFrameIndex(3u), "jitter aligned after sync");

    expectTrue(!jitter.advanceIfAlignedToFrameIndex(5u),
               "advanceIfAlignedToFrameIndex rejects mismatched frame index");
    expectTrue(jitter.index() == 3u, "jitter slot unchanged after rejected advance");

    expectTrue(jitter.advanceIfAlignedToFrameIndex(3u), "advanceIfAlignedToFrameIndex succeeds when aligned");
    expectTrue(jitter.monotonicFrameIndex() == 4u, "advanceIfAlignedToFrameIndex increments monotonic counter");
    expectTrue(jitter.index() == 4u, "advanceIfAlignedToFrameIndex advances slot");
void testJitterAdvanceIfAlignedGuards() {
               "advanceIfAlignedToFrameIndex blocked when not aligned");

    expectTrue(jitter.isAlignedToFrameIndex(5u), "jitter aligned after sync");
    expectTrue(jitter.advanceIfAlignedToFrameIndex(5u), "advanceIfAlignedToFrameIndex succeeds when aligned");
    expectTrue(jitter.monotonicFrameIndex() == 6u, "advanceIfAlignedToFrameIndex increments monotonic counter");
    expectTrue(jitter.index() == 6u, "advanceIfAlignedToFrameIndex advances slot");

               "advanceIfAlignedToFrameIndex blocked after drift");
    expectTrue(fuse::renderer::preflightTaaJitterAlignment(6u, jitter),
               "preflightTaaJitterAlignment passes for current frame");
    expectTrue(!fuse::renderer::preflightTaaJitterAlignment(5u, jitter),
               "preflightTaaJitterAlignment fails for stale frame");
    expectTrue(fallbackJitter.needsSyncToFrameIndex(2u), "fallback jitter needs sync after default length recovery");
    expectTrue(fallbackJitter.syncToFrameIndexIfMisaligned(2u), "syncToFrameIndexIfMisaligned succeeds after fallback");
    expectTrue(fallbackJitter.isAlignedToFrameIndex(2u), "fallback jitter aligned after syncIfMisaligned");

    pass->syncJitterToFrameIndex(2u);
    expectTrue(pass->preflightJitterSync(2u), "pass jitter sync preflight passes when aligned");
    expectTrue(pass->advanceJitterIfAlignedToFrameIndex(2u),
               "pass advanceJitterIfAlignedToFrameIndex succeeds when aligned");
    expectTrue(pass->jitter().monotonicFrameIndex() == 3u, "pass jitter advanced to frame three");
    expectTrue(!pass->advanceJitterIfAlignedToFrameIndex(2u),
               "pass advanceJitterIfAlignedToFrameIndex rejects stale frame index");

void testTaaPassResolveGuardsPreflight() {
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass resolve guards test");



    expectTrue(pass->needsJitterSyncToFrameIndex(6u), "pass jitter needs sync before alignment");
    expectTrue(pass->syncJitterToFrameIndexIfMisaligned(6u), "pass syncJitterToFrameIndexIfMisaligned succeeds");
    expectTrue(pass->jitterAlignedToFrameIndex(6u), "pass jitter aligned after syncIfMisaligned");
    expectTrue(!pass->needsJitterSyncToFrameIndex(6u), "pass jitter does not need sync when aligned");

void testTaaPassWarmupAndTemporalPreflight() {
    expectTrue(fallbackJitter.wouldSkipSyncToFrameIndex(3u) == false,
               "fallback jitter does not skip sync after default-length fallback");
    expectTrue(fallbackJitter.trySyncToFrameIndexIfReady(3u, syncReason),
               "trySync succeeds after fallback to default length");

void testTaaPassWouldSkipAndTryHelpers() {
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass warmup/temporal preflight test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass wouldSkip/try test");




    expectTrue(pass->init(resources), "TaaPass initialized for resolve guards test");
    expectTrue(pass->init(resources), "TaaPass initialized for warmup/temporal preflight test");
    expectTrue(pass->historyReadyForResolve(), "pass history ready for resolve after init");
    expectTrue(pass->warmupFramesRemaining() == 1u, "pass reports one warmup frame remaining");
    expectTrue(pass->init(resources), "TaaPass initialized for wouldSkip/try test");
    expectTrue(pass->wouldSkipHistoryReuse(0u), "pass wouldSkip history reuse before warmup");
    expectTrue(!pass->wouldSkipJitterSync(5u), "pass wouldSkip jitter sync false for valid sequence");

    expectTrue(!pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflight history reuse fails before warmup");
               "pass tryPreflight reuse reason is NotWarm before warmup");

    resolveDesc.observed_history_generation = fuse::renderer::kTaaResolveNoHistoryGeneration;

    expectTrue(!pass->preflightHistoryReuseForDesc(resolveDesc, &reuseReason),
               "pass desc reuse preflight fails before warmup");
               "pass desc reuse preflight reason is NotWarm before warmup");

    expectTrue(pass->preflightResolveGuards(resolveDesc, &skipReason, &blendReason),
               "pass combined guards pass before first resolve");

    expectTrue(pass->preflightHistoryReuseForDesc(resolveDesc, &reuseReason),
               "pass desc reuse preflight passes after warmup");
               "pass combined guards pass after warmup");

    pass->invalidateHistory();
               "pass desc reuse preflight fails after invalidate");
               "pass resolve frame preflight passes before first resolve");

    expectTrue(pass->syncJitterToFrameIndexIfReady(4u), "pass sync jitter to frame four");
    expectTrue(!pass->jitterNeedsSyncToFrameIndex(4u), "pass jitter aligned after sync");
    expectTrue(pass->jitterAlignedToFrameIndex(4u), "pass jitter reports alignment after sync");

    expectTrue(pass->preflightHistoryWarmup(&warmupState), "pass warmup preflight passes after resolve");
    expectTrue(warmupState == fuse::renderer::TaaHistoryWarmupState::Complete,
               "pass warmup preflight reports Complete after resolve");
    expectTrue(pass->classifyResolveBlendMode(resolveDesc) == fuse::renderer::TaaResolveBlendMode::Steady,
               "pass blend mode is Steady after warmup");

    expectTrue(pass->classifyResolveBlendMode(resolveDesc) ==
               "pass blend mode reverts to Warmup after invalidate");

    expectTrue(fuse::renderer::preflightTaaResolveWithBlend(desc, history, &skipReason, &blendReason),
               "combined preflight passes for valid warmup resolve");
               "combined preflight blend reason is None");

    expectTrue(!fuse::renderer::preflightTaaResolveWithBlend(desc, history, &skipReason, &blendReason),
               "combined preflight fails on invalid dimensions");
               "combined preflight reports InvalidDimensions skip reason");

               "combined preflight passes after warmup");


    expectTrue(!pass->preflightResolveHistoryReuse(resolveDesc, &reuseReason),
               "pass resolve reuse preflight fails before warmup");
    expectTrue(pass->preflightResolveTemporal(resolveDesc, &reuseReason), "pass temporal preflight passes on warmup");

    expectTrue(pass->preflightResolveHistoryReuse(resolveDesc, &reuseReason),
               "pass resolve reuse preflight passes after warmup");
    expectTrue(pass->preflightResolveTemporal(resolveDesc), "pass temporal preflight passes after warmup");

    expectTrue(!pass->wouldRejectResolveBlendWeights(resolveDesc),
               "pass wouldReject blend false before first resolve");
    expectTrue(pass->tryPreflightResolveBlendWeights(resolveDesc, blendReason),
               "pass tryPreflight blend succeeds before first resolve");

    expectTrue(pass->trySyncJitterToFrameIndex(4u, syncReason), "pass trySync jitter succeeds");
    expectTrue(pass->jitterAlignedToFrameIndex(4u), "pass jitter aligned after trySync");

    expectTrue(!pass->wouldSkipHistoryReuse(0u), "pass wouldSkip history reuse false after warmup");
    expectTrue(pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflight history reuse succeeds after warmup");

    expectTrue(pass->resolveBlendReady(resolveDesc), "pass resolve blend ready before warmup resolve");
    fuse::renderer::TaaResolveBlendRejectReason blendReject =
    expectTrue(pass->tryPreflightResolveBlendWeights(resolveDesc, blendReject),
               "pass tryPreflightResolveBlendWeights passes before warmup");
    expectTrue(pass->tryComputeResolveBlendWeights(resolveDesc, weights, blendReject),
               "pass tryComputeResolveBlendWeights passes before warmup");
    expectNear(weights.current, 1.f, 1e-5f, "pass tryCompute warmup current weight is full");

               "pass tryPreflightHistoryReuse fails before warmup");
               "pass tryPreflightHistoryReuse reason is NotWarm before warmup");

    expectTrue(pass->historyWarmupComplete(), "pass history warmup complete after resolve");
    expectTrue(pass->resolveBlendReady(resolveDesc), "pass resolve blend ready after warmup");
               "pass tryPreflightHistoryReuse passes after warmup");
               "pass tryComputeResolveBlendWeights passes after warmup");
    expectNear(weights.current, 0.15f, 1e-5f, "pass tryCompute steady current weight");
    expectTrue(!pass->shouldSkipResolve(resolveDesc), "pass should not skip valid resolve");
    expectTrue(!pass->shouldSkipHistoryWarmup(), "pass should not skip warmup after resolve");
    expectTrue(pass->preflightHistoryWarmup(), "pass warmup preflight passes after resolve");

    resolveDesc.width = 0;
    expectTrue(pass->shouldSkipResolve(resolveDesc), "pass should skip invalid resolve");

    fuse::renderer::TaaHistoryReuseBlockReason reuseReason = fuse::renderer::TaaHistoryReuseBlockReason::None;
        fuse::renderer::TaaResolveBlendRejectReason::None;
    expectTrue(!pass->preflightTemporalBlend(resolveDesc, &reuseReason, &blendReason),
               "pass temporal blend preflight fails before warmup");
    expectTrue(pass->shouldSkipTemporalBlend(resolveDesc), "pass should skip temporal blend before warmup");

    expectTrue(pass->isHistoryWarmed(), "pass history warmed after resolve");
    expectTrue(pass->preflightTemporalBlend(resolveDesc, &reuseReason, &blendReason),
               "pass temporal blend preflight passes after warmup");
    expectTrue(!pass->shouldSkipTemporalBlend(resolveDesc),
               "pass should not skip temporal blend after warmup");

    expectTrue(pass->shouldSkipTemporalBlend(resolveDesc),
               "pass should skip temporal blend after invalidate");

    fuse::renderer::TaaTemporalGuardRejectReason temporalReason =
        fuse::renderer::TaaTemporalGuardRejectReason::None;
    expectTrue(pass->shouldSkipTemporalResolve(resolveDesc),
               "pass should skip temporal resolve before warmup");
    expectTrue(!pass->preflightTemporalResolve(resolveDesc, &temporalReason),
               "pass temporal preflight fails before warmup");
    expectTrue(temporalReason == fuse::renderer::TaaTemporalGuardRejectReason::HistoryReuseBlocked,
               "pass temporal reason is HistoryReuseBlocked before warmup");

    expectTrue(!pass->shouldSkipTemporalResolve(resolveDesc),
               "pass should not skip temporal resolve after warmup");
    expectTrue(pass->preflightTemporalResolve(resolveDesc, &temporalReason),
               "pass temporal preflight passes after warmup");

    const fuse::u32 generationBefore = pass->historyInvalidateGeneration();
    expectTrue(!pass->invalidateHistoryIfStale(generationBefore),
               "invalidateHistoryIfStale no-op when generation matches");
    expectTrue(pass->historyReuseReady(generationBefore), "history still reuse-ready when generation matches");

    const fuse::u32 staleObservedGeneration = generationBefore + 100u;
    expectTrue(pass->invalidateHistoryIfStale(staleObservedGeneration),
               "invalidateHistoryIfStale invalidates stale generation");
    expectTrue(pass->historyInvalidateGeneration() > generationBefore,
               "invalidate epoch bumped after stale invalidate");
               "pass should skip temporal resolve after stale invalidate");
    expectTrue(!pass->historyReuseReady(staleObservedGeneration),
               "history not reuse-ready for stale observed generation");

    expectTrue(pass->resolveBlendReady(resolveDesc), "pass resolve blend ready before first resolve");
    expectTrue(pass->preflightJitterNdc(), "pass jitter NDC preflight passes");

               "tryPreflightHistoryReuse fails before warmup");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::NotWarm,
               "tryPreflightHistoryReuse reason is NotWarm before warmup");

               "tryPreflightResolveBlendWeights passes before first resolve");
               "tryPreflightResolveBlendWeights reason is None before first resolve");

               "tryPreflightHistoryReuse passes after warmup");

    fuse::renderer::TaaPassDesc zeroWidthDesc{};
    zeroWidthDesc.width = 0;
    zeroWidthDesc.height = 128;
    auto zeroPass = fuse::renderer::TaaPass::create(zeroWidthDesc);
    expectTrue(zeroPass->shouldSkipJitterNdc(), "zero-width pass should skip jitter NDC");
    expectTrue(!zeroPass->jitterNdcReady(), "zero-width pass jitter NDC not ready");
    expectTrue(zeroPass->jitterSyncReady(0u), "zero-width pass jitter sync still ready");
    expectTrue(pass->resolveBlendWeightsReady(resolveDesc),
               "pass resolve blend weights ready before warmup resolve");
    expectTrue(pass->shouldSkipResolveHistoryBlend(resolveDesc),
               "pass should skip history blend before warmup resolve");

    expectTrue(pass->shouldSkipHistoryWarmup(), "pass should skip warmup after resolve");
    expectTrue(!pass->shouldSkipResolveHistoryBlend(resolveDesc),
               "pass should not skip history blend after warmup");

    expectTrue(!pass->shouldSkipJitterNdc(), "pass should not skip jitter NDC");
    expectTrue(pass->preflightJitterAdvance(&jitterReject), "pass preflightJitterAdvance passes");
    expectTrue(!pass->shouldSkipJitterAdvance(), "pass should not skip jitter advance");


void testHistoryWarmupPhaseGuards() {
    expectTrue(std::strcmp(fuse::renderer::taaHistoryWarmupPhaseLabel(
                               fuse::renderer::TaaHistoryWarmupPhase::NotAllocated),
                           "not_allocated") == 0,
               "NotAllocated warmup phase label");
                               fuse::renderer::TaaHistoryWarmupPhase::NeedsWarmup),
               "NeedsWarmup warmup phase label");
                               fuse::renderer::TaaHistoryWarmupPhase::Warm),
                           "warm") == 0,
               "Warm warmup phase label");

    expectTrue(fuse::renderer::classifyTaaHistoryWarmupPhase(emptyHistory) ==
                   fuse::renderer::TaaHistoryWarmupPhase::NotAllocated,
               "empty history classified as NotAllocated");
    expectTrue(!fuse::renderer::preflightTaaHistoryWarmup(emptyHistory),
    pass->syncJitterToFrameIndex(4u);
    expectTrue(pass->preflightJitterAlignment(4u), "pass preflightJitterAlignment passes when synced");
    expectTrue(pass->advanceJitterIfAligned(4u), "pass advanceJitterIfAligned succeeds when aligned");
    expectTrue(pass->jitter().monotonicFrameIndex() == 5u, "pass advanceJitterIfAligned increments counter");
    expectTrue(!pass->advanceJitterIfAligned(4u), "pass advanceJitterIfAligned blocked after drift");

void testPreflightTaaResolveDesc() {

    fuse::renderer::TaaResolveDescPreflight result{};
    expectTrue(!fuse::renderer::preflightTaaResolveDesc(desc, emptyHistory, &result),
               "empty history fails combined resolve-desc preflight");
    expectTrue(result.skip_reason == fuse::renderer::TaaResolveSkipReason::HistoryNotReady,
               "combined preflight reports HistoryNotReady for empty history");
    expectTrue(!result.passes, "combined preflight passes flag is false on skip");
void testTaaPassWarmupAndSyncPreflightHelpers() {

    expectTrue(!pass->historyWarmupComplete(), "pass warmup not complete before init");
    expectTrue(!pass->shouldSkipJitterSync(4u), "pass should not skip jitter sync");
    expectTrue(pass->jitterNdcReady(), "pass jitter NDC ready");
    expectTrue(pass->preflightJitterNdc(&rejectReason), "pass preflightJitterNdc passes");

void testResolveTemporalPreflightGuards() {
                               fuse::renderer::TaaResolveTemporalRejectReason::BlendWeightsRejected),
                           "blend_weights_rejected") == 0,
               "BlendWeightsRejected temporal reject label");

    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for warmup phase test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for resolve-desc preflight test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass warmup/sync preflight test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    expectTrue(history.init(resources, historyDesc), "history ready for warmup phase test");
    expectTrue(fuse::renderer::classifyTaaHistoryWarmupPhase(history) ==
                   fuse::renderer::TaaHistoryWarmupPhase::NeedsWarmup,
               "allocated unwarmed history classified as NeedsWarmup");

    fuse::renderer::TaaHistoryWarmupPhase phase = fuse::renderer::TaaHistoryWarmupPhase::NotAllocated;
    expectTrue(!fuse::renderer::preflightTaaHistoryWarmup(history, &phase),
    expectTrue(phase == fuse::renderer::TaaHistoryWarmupPhase::NeedsWarmup,
               "unwarmed history warmup preflight phase is NeedsWarmup");
    expectTrue(!fuse::renderer::taaHistoryTemporalReuseReady(history, 0u),
               "temporal reuse not ready before warmup");

                   fuse::renderer::TaaHistoryWarmupPhase::Warm,
               "warmed history classified as Warm");
    expectTrue(fuse::renderer::preflightTaaHistoryWarmup(history, &phase),
    expectTrue(phase == fuse::renderer::TaaHistoryWarmupPhase::Warm,
               "warmed history warmup preflight phase is Warm");
    expectTrue(fuse::renderer::taaHistoryTemporalReuseReady(history, 0u),
               "temporal reuse ready when warmed and generation matches");

               "temporal reuse not ready after invalidate with stale generation");

    expectTrue(history.init(resources, historyDesc), "history ready for resolve-desc preflight test");

    expectTrue(fuse::renderer::preflightTaaResolveDesc(desc, history, &result),
               "valid desc passes combined resolve-desc preflight");
    expectTrue(result.passes, "combined preflight passes flag is true for valid desc");
    expectTrue(result.skip_reason == fuse::renderer::TaaResolveSkipReason::None,
               "combined preflight skip reason is None for valid desc");
    expectTrue(result.blend_reject_reason == fuse::renderer::TaaResolveBlendRejectReason::None,
               "combined preflight blend reject reason is None for valid desc");

    fuse::renderer::TaaResolve resolve;
    expectTrue(resolve.preflightDesc(desc, history, &result),
               "TaaResolve::preflightDesc passes for valid desc");

    expectTrue(!fuse::renderer::preflightTaaResolveDesc(desc, history, &result),
               "invalid dimensions fail combined resolve-desc preflight");
    expectTrue(result.skip_reason == fuse::renderer::TaaResolveSkipReason::InvalidDimensions,

    expectTrue(pass->init(resources), "TaaPass initialized for resolve-desc preflight test");

    expectTrue(!pass->preflightHistoryWarmup(&warmupReason),
    expectTrue(warmupReason == fuse::renderer::TaaHistoryWarmupBlockReason::NeedsResolve,
               "pass warmup preflight reason is NeedsResolve before resolve");

    expectTrue(pass->preflightResolveDesc(desc, &result), "pass preflightResolveDesc passes valid desc");
    expectTrue(pass->resolveFrame(desc), "resolveFrame succeeds after preflightResolveDesc");
    expectTrue(pass->preflightHistoryWarmup(&warmupReason), "pass warmup preflight passes after resolve");

    expectTrue(pass->init(resources), "TaaPass initialized for warmup/sync preflight test");

    expectTrue(!pass->preflightHistoryWarmup(&warmupReason), "pass warmup preflight fails before resolve");
               "pass warmup preflight reason is NeedsWarmup before resolve");

    expectTrue(pass->preflightJitterSync(2u, &syncReason), "pass jitter sync preflight passes");
    expectTrue(!pass->jitterAlignedToFrameIndex(2u), "pass jitter not aligned before sync");
    expectTrue(pass->syncJitterToFrameIndexIfViewportReady(2u), "pass viewport sync succeeds");
    expectTrue(pass->jitterAlignedToFrameIndex(2u), "pass jitter aligned after viewport sync");

    resolveDesc.params.blend_factor = 0.25f;

               "pass resolve-history reuse preflight fails before warmup");
               "pass resolve-history reuse reason is NotWarm before warmup");

    expectTrue(pass->preflightResolveWithBlend(resolveDesc, &skipReason, &blendReason),
               "pass combined preflight passes before first resolve");

               "pass resolve-history reuse preflight passes after warmup");
               "pass combined preflight passes after warmup");




    expectTrue(TaaJitterLayout::classifySyncReject(8u) == fuse::renderer::TaaJitterSyncRejectReason::None,
               "valid sequence has no sync reject reason");
    expectTrue(TaaJitterLayout::classifySyncReject(0u) ==

    fuse::renderer::TaaJitterSyncRejectReason reason = fuse::renderer::TaaJitterSyncRejectReason::None;
    expectTrue(TaaJitterLayout::preflightSyncToFrameIndex(5u, 8u, &reason),
               "valid sequence passes sync preflight");
    expectTrue(reason == fuse::renderer::TaaJitterSyncRejectReason::None,
               "valid sequence sync preflight reason is None");
    expectTrue(!TaaJitterLayout::preflightSyncToFrameIndex(5u, 0u, &reason),
               "invalid sequence fails sync preflight");
    expectTrue(reason == fuse::renderer::TaaJitterSyncRejectReason::InvalidSequence,
               "invalid sequence sync preflight reason is InvalidSequence");

    expectTrue(jitter.classifySyncReject(5u) == fuse::renderer::TaaJitterSyncRejectReason::None,
               "default jitter has no sync reject reason");
    expectTrue(jitter.preflightSyncToFrameIndex(5u, &reason), "jitter preflightSyncToFrameIndex succeeds");
    expectTrue(jitter.isAlignedToFrameIndex(5u), "jitter aligned after preflightSyncToFrameIndex");
    expectTrue(jitter.monotonicFrameIndex() == 5u, "jitter monotonic counter set by preflightSyncToFrameIndex");

    expectTrue(pass->classifyJitterSyncReject(7u) == fuse::renderer::TaaJitterSyncRejectReason::None,
               "pass jitter sync reject is None for valid sequence");
    expectTrue(pass->preflightJitterSync(7u, &reason), "pass preflightJitterSync succeeds");
    expectTrue(pass->syncJitterToFrameIndexIfReady(7u), "pass syncIfReady succeeds after preflight");
    expectTrue(pass->jitterAlignedToFrameIndex(7u), "pass jitter aligned after sync");

void testResolveTemporalBlendPreflight() {





    const fuse::renderer::TaaBlendWeights expected =
        fuse::renderer::computeTaaResolveBlendWeights(desc, history);
    expectTrue(fuse::renderer::taaResolveBlendWeightsMatchExpected(expected, desc, history),
               "computed blend weights match expected on warmup frame");
    expectTrue(!fuse::renderer::taaResolveTemporalBlendAllowed(desc, history),
               "temporal blend not allowed before warmup");

    fuse::renderer::TaaHistoryReuseBlockReason reuseReason =
        fuse::renderer::TaaHistoryReuseBlockReason::None;
    expectTrue(!fuse::renderer::preflightTaaResolveTemporalBlend(desc, history, &blendReason, &reuseReason),
               "temporal blend preflight fails before warmup");
               "temporal blend preflight blend reason is None before warmup");
               "temporal blend preflight reuse reason is NotWarm before warmup");

    expectTrue(fuse::renderer::taaResolveTemporalBlendAllowed(desc, history),
               "temporal blend allowed after warmup");
    expectTrue(fuse::renderer::preflightTaaResolveTemporalBlend(desc, history, &blendReason, &reuseReason),
               "temporal blend preflight passes after warmup");
               "temporal blend preflight blend reason is None after warmup");
               "temporal blend preflight reuse reason is None after warmup");

               "temporal blend preflight passes with generation sentinel");

    passDesc.params = desc.params;
    expectTrue(pass->init(resources), "TaaPass initialized for temporal blend preflight test");
    expectTrue(pass->historyWarmupPhase() == fuse::renderer::TaaHistoryWarmupPhase::NeedsWarmup,
               "pass warmup phase is NeedsWarmup before resolve");
    expectTrue(!pass->resolveTemporalBlendAllowed(desc),
               "pass temporal blend not allowed before resolve");

    desc.surfaces.current_frame = reinterpret_cast<void*>(0x10);
    desc.surfaces.output = reinterpret_cast<void*>(0x20);
    expectTrue(pass->resolveFrame(desc), "initial resolve warms pass history");
    expectTrue(pass->historyWarmupPhase() == fuse::renderer::TaaHistoryWarmupPhase::Warm,
               "pass warmup phase is Warm after resolve");
    expectTrue(pass->resolveTemporalBlendAllowed(desc),
               "pass temporal blend allowed after warmup");
    expectTrue(pass->preflightResolveTemporalBlend(desc, &blendReason, &reuseReason),

    fuse::renderer::TaaHistoryBuffer history;
    fuse::renderer::TaaHistoryBufferDesc historyDesc{64, 64};

    fuse::renderer::TaaResolveDesc desc{};

    expectTrue(fuse::renderer::preflightTaaResolveTemporal(desc, history, &temporalReason),
               "warmup temporal preflight passes before first resolve");
    expectTrue(temporalReason == fuse::renderer::TaaResolveTemporalRejectReason::None,
               "warmup temporal preflight reason is None");
    expectTrue(!fuse::renderer::shouldSkipTaaResolveTemporal(desc, history),
               "warmup temporal preflight should not skip");

    history.markResolved();
    expectTrue(fuse::renderer::tryPreflightTaaResolveTemporal(desc, history, temporalReason),
               "steady temporal preflight passes after warmup");
               "steady temporal preflight reason is None");

    history.invalidateHistory();
    desc.observed_history_generation = 0u;
               "stale generation with full-current blend still passes temporal preflight");
               "stale generation full-current temporal reason is None");

               "stale generation degrades to full-current and passes temporal preflight");
               "stale generation degraded temporal reason is None");
               "stale generation degraded temporal preflight should not skip");

    fuse::renderer::TaaPassDesc passDesc{};
    passDesc.params.blend_factor = 0.3f;
    auto pass = fuse::renderer::TaaPass::create(passDesc);
    expectTrue(pass->init(resources), "TaaPass initialized for temporal preflight test");
    expectTrue(!pass->historyWarmupComplete(), "pass history warmup incomplete before resolve");
    expectTrue(pass->historyWarmupState() == fuse::renderer::TaaHistoryWarmupState::NeedsWarmup,
               "pass history warmup state is NeedsWarmup");

    expectTrue(pass->preflightResolveTemporal(resolveDesc, &temporalReason),
               "pass temporal preflight passes before warmup resolve");
    expectTrue(pass->preflightHistoryWarmup(), "pass preflightHistoryWarmup passes after resolve");

    history.destroy();
    expectTrue(!zeroPass->preflightJitterNdc(), "zero-width pass jitter NDC preflight fails");

    expectTrue(zeroPass->shouldSkipJitterNdc(), "zero-width pass should skip NDC jitter");
    expectTrue(!zeroPass->preflightJitterNdc(), "zero-width pass NDC preflight fails");

    expectTrue(!zeroPass->preflightJitterNdc(&jitterReject),
               "zero-width pass preflightJitterNdc fails");

    expectTrue(pass->init(resources), "TaaPass initialized for warmup/jitter preflight follow-up test");

    resolveDesc.width = 96;
    resolveDesc.height = 96;

    const fuse::renderer::TaaResolveBlendPreflight preResolveBlend = pass->preflightResolveBlend(resolveDesc);
    expectTrue(preResolveBlend.can_apply, "pass resolve blend preflight can apply before warmup resolve");
    expectTrue(pass->shouldSkipHistoryWarmup(), "pass should skip warmed state before first resolve");

    expectTrue(!pass->shouldSkipHistoryWarmup(), "pass should not skip warmed state after resolve");

    const fuse::renderer::TaaHistoryWarmupPreflight postWarmup = pass->preflightHistoryWarmup();
    expectTrue(postWarmup.isWarmed(), "pass warmup preflight warmed after resolve");
    expectTrue(postWarmup.canReuseHistory(), "pass warmup preflight can reuse after resolve");

    const fuse::renderer::TaaResolveBlendPreflight postResolveBlend = pass->preflightResolveBlend(resolveDesc);
    expectTrue(postResolveBlend.appliesHistoryBlend(),
               "pass resolve blend preflight applies history blend after warmup");

    zeroWidthDesc.height = 96;
    expectTrue(zeroPass->shouldSkipJitterNdc(), "zero-width pass should skip jitter NDC preflight");


    fuse::renderer::TaaResolveTemporalRejectReason temporalReject =
    expectTrue(!pass->preflightResolveTemporalBlend(resolveDesc, &temporalReject),
               "pass temporal blend preflight fails before warmup resolve");
    expectTrue(temporalReject == fuse::renderer::TaaResolveTemporalRejectReason::HistoryReuseBlocked,
               "pass temporal reject reason is HistoryReuseBlocked before warmup");

    expectTrue(pass->historyWarmupComplete(), "pass historyWarmupComplete after resolve");
    expectTrue(!pass->shouldSkipHistoryWarmup(), "pass should not skip warm-up after resolve");
    expectTrue(pass->preflightResolveTemporalBlend(resolveDesc, &temporalReject),
    expectTrue(!pass->shouldSkipResolveTemporalBlend(resolveDesc),

               "pass tryPreflightResolveBlendWeights passes after warmup");


    expectTrue(!pass->shouldSkipResolveFrame(resolveDesc),
               "pass resolve frame should not be skipped before warmup");

    expectTrue(!pass->shouldSkipHistoryWarmup(), "pass should not skip warmup after first resolve");

               "pass warmup preflight reason is None after resolve");

               "pass resolve frame preflight passes after warmup");
    expectTrue(pass->resolveFrame(resolveDesc), "second resolve uses warmed history");

    expectTrue(pass->shouldSkipResolveFrame(resolveDesc),
               "pass resolve frame skipped for invalid dimensions");


    pass->destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testJitterFramePreflightAndShouldSkip() {
    using fuse::renderer::TaaJitterLayout;

    expectTrue(fuse::renderer::shouldSkipTaaJitterSync(0u), "zero sequence should skip jitter sync");
    expectTrue(!fuse::renderer::shouldSkipTaaJitterSync(8u), "valid sequence should not skip jitter sync");
    expectTrue(fuse::renderer::shouldSkipTaaJitterNdc(0u, 128u, 8u),
               "zero width should skip NDC jitter preflight");
    expectTrue(!fuse::renderer::shouldSkipTaaJitterNdc(128u, 128u, 8u),
               "valid viewport should not skip NDC jitter preflight");

    fuse::renderer::TaaJitterGuardRejectReason rejectReason =
        fuse::renderer::TaaJitterGuardRejectReason::None;
    expectTrue(fuse::renderer::tryPreflightTaaJitterNdc(128u, 128u, 8u, rejectReason),
               "tryPreflightTaaJitterNdc passes for valid viewport");
    expectTrue(rejectReason == fuse::renderer::TaaJitterGuardRejectReason::None,
               "tryPreflightTaaJitterNdc reject reason is None");
    expectTrue(!fuse::renderer::tryPreflightTaaJitterNdc(0u, 128u, 8u, rejectReason),
               "tryPreflightTaaJitterNdc rejects zero width");
    expectTrue(rejectReason == fuse::renderer::TaaJitterGuardRejectReason::InvalidViewport,
               "tryPreflightTaaJitterNdc reject reason is InvalidViewport");

    const fuse::renderer::TaaJitterFramePreflight framePreflight =
        fuse::renderer::preflightTaaJitterFrame(13u, 1920u, 1080u, 8u);
    expectTrue(framePreflight.canSync(), "frame preflight allows sync for valid sequence");
    expectTrue(framePreflight.canProduceNdc(), "frame preflight allows NDC for valid viewport");
    expectTrue(framePreflight.passes(), "frame preflight passes for valid inputs");
    expectTrue(framePreflight.slot == TaaJitterLayout::frameIndexInSequence(13u, 8u),
               "frame preflight records wrapped jitter slot");

    const fuse::renderer::TaaJitterFramePreflight invalidPreflight =
        fuse::renderer::preflightTaaJitterFrame(0u, 0u, 1080u, 8u);
    expectTrue(invalidPreflight.canSync(), "frame preflight sync still valid with invalid viewport");
    expectTrue(!invalidPreflight.canProduceNdc(), "frame preflight blocks NDC for zero width");
    expectTrue(!invalidPreflight.passes(), "frame preflight fails when NDC is blocked");
    expectTrue(invalidPreflight.ndcReject == fuse::renderer::TaaJitterGuardRejectReason::InvalidViewport,
               "frame preflight NDC reject reason is InvalidViewport");
}

void testHistoryWarmupPreflightGuards() {
    fuse::renderer::TaaHistoryBuffer emptyHistory;
    expectTrue(fuse::renderer::shouldSkipTaaHistoryWarmup(emptyHistory),
               "empty history should skip warmup completion");
    expectTrue(!fuse::renderer::isTaaHistoryWarmupComplete(emptyHistory),
               "empty history warmup is not complete");

    const fuse::renderer::TaaHistoryWarmupPreflight emptyPreflight =
        fuse::renderer::preflightTaaHistoryWarmup(emptyHistory, 0u);
    expectTrue(!emptyPreflight.isWarmupComplete(), "empty history preflight warmup incomplete");
    expectTrue(!emptyPreflight.canReuse(), "empty history preflight cannot reuse");
    expectTrue(!emptyPreflight.passes(), "empty history warmup preflight fails");
    expectTrue(emptyPreflight.reuseBlock == fuse::renderer::TaaHistoryReuseBlockReason::NotReady,
               "empty history warmup preflight reuse block is NotReady");

    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for history warmup preflight test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaHistoryBuffer history;
    fuse::renderer::TaaHistoryBufferDesc historyDesc{64, 64};
    expectTrue(history.init(resources, historyDesc), "history ready for warmup preflight test");

    const fuse::renderer::TaaHistoryWarmupPreflight unwarmedPreflight =
        fuse::renderer::preflightTaaHistoryWarmup(history, 0u);
    expectTrue(fuse::renderer::shouldSkipTaaHistoryWarmup(history),
               "unwarmed history should skip warmup completion");
    expectTrue(unwarmedPreflight.framesRemaining == 1u, "unwarmed preflight has one frame remaining");
    expectTrue(!unwarmedPreflight.isWarmupComplete(), "unwarmed preflight warmup incomplete");
    expectTrue(unwarmedPreflight.reuseBlock == fuse::renderer::TaaHistoryReuseBlockReason::NotWarm,
               "unwarmed preflight reuse block is NotWarm");

    history.markResolved();
    expectTrue(fuse::renderer::isTaaHistoryWarmupComplete(history),
               "warmed history warmup is complete");
    expectTrue(!fuse::renderer::shouldSkipTaaHistoryWarmup(history),
               "warmed history should not skip warmup completion");

    const fuse::renderer::TaaHistoryWarmupPreflight warmedPreflight =
        fuse::renderer::preflightTaaHistoryWarmup(history, 0u);
    expectTrue(warmedPreflight.isWarmupComplete(), "warmed preflight warmup complete");
    expectTrue(warmedPreflight.canReuse(), "warmed preflight can reuse");
    expectTrue(warmedPreflight.passes(), "warmed preflight passes");

    const fuse::renderer::TaaHistoryWarmupPreflight staleWarmPreflight =
        fuse::renderer::preflightTaaHistoryWarmup(history, 999u);
    expectTrue(staleWarmPreflight.isWarmupComplete(), "stale observed gen preflight warmup still complete");
    expectTrue(!staleWarmPreflight.canReuse(), "stale observed gen preflight cannot reuse");
    expectTrue(staleWarmPreflight.reuseBlock == fuse::renderer::TaaHistoryReuseBlockReason::StaleGeneration,
               "stale observed gen preflight reuse block is StaleGeneration");

    history.invalidateHistory();
    const fuse::renderer::TaaHistoryWarmupPreflight postInvalidatePreflight =
        fuse::renderer::preflightTaaHistoryWarmup(history, history.invalidateGeneration());
    expectTrue(!postInvalidatePreflight.isWarmupComplete(), "invalidated history preflight warmup incomplete");
    expectTrue(!postInvalidatePreflight.canReuse(), "invalidated history preflight cannot reuse");
    expectTrue(postInvalidatePreflight.reuseBlock == fuse::renderer::TaaHistoryReuseBlockReason::NotWarm,
               "invalidated history preflight reuse block is NotWarm");

    history.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testResolveBlendFramePreflightGuards() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for blend frame preflight test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaHistoryBuffer history;
    fuse::renderer::TaaHistoryBufferDesc historyDesc{64, 64};
    expectTrue(history.init(resources, historyDesc), "history ready for blend frame preflight test");

    fuse::renderer::TaaResolveDesc desc{};
    desc.params.blend_factor = 0.4f;

    const fuse::renderer::TaaResolveBlendPreflight warmupPreflight =
        fuse::renderer::preflightTaaResolveBlendFrame(desc, history);
    expectTrue(warmupPreflight.passes(), "warmup blend frame preflight passes");
    expectTrue(!warmupPreflight.historyBlendAllowed, "warmup blend frame preflight blocks history blend");
    expectTrue(!warmupPreflight.historyDegraded, "warmup blend frame preflight is not degraded");
    expectNear(warmupPreflight.weights.current, 1.f, 1e-5f, "warmup blend frame preflight current is full");
    expectTrue(fuse::renderer::canApplyTaaResolveBlendWeights(desc, history),
               "warmup blend weights can be applied");

    history.markResolved();
    const fuse::renderer::TaaResolveBlendPreflight steadyPreflight =
        fuse::renderer::preflightTaaResolveBlendFrame(desc, history);
    expectTrue(steadyPreflight.passes(), "steady blend frame preflight passes");
    expectTrue(steadyPreflight.historyBlendAllowed, "steady blend frame preflight allows history blend");
    expectTrue(!steadyPreflight.historyDegraded, "steady blend frame preflight is not degraded");
    expectNear(steadyPreflight.weights.current, 0.4f, 1e-5f, "steady blend frame preflight current weight");
    expectTrue(!fuse::renderer::isHistoryBlendDegraded(desc, history),
               "steady blend is not degraded");

    desc.observed_history_generation = 999u;
    const fuse::renderer::TaaResolveBlendPreflight stalePreflight =
        fuse::renderer::preflightTaaResolveBlendFrame(desc, history);
    expectTrue(stalePreflight.passes(), "stale blend frame preflight still passes weight validation");
    expectTrue(!stalePreflight.historyBlendAllowed, "stale blend frame preflight blocks history blend");
    expectTrue(stalePreflight.historyDegraded, "stale blend frame preflight is degraded");
    expectTrue(fuse::renderer::isHistoryBlendDegraded(desc, history),
               "stale observed generation marks history blend as degraded");

    history.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testTaaFrameGuardPreflight() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for frame guard preflight test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaPassDesc passDesc{};
    passDesc.width = 64;
    passDesc.height = 64;
    passDesc.params.blend_factor = 0.3f;

    auto pass = fuse::renderer::TaaPass::create(passDesc);
    expectTrue(pass->shouldSkipHistoryWarmup(), "pass should skip warmup completion before init");
    expectTrue(!pass->isHistoryWarmupComplete(), "pass warmup not complete before init");

    const fuse::renderer::TaaJitterFramePreflight jitterPreflight = pass->preflightJitterFrame(9u);
    expectTrue(jitterPreflight.passes(), "pass jitter frame preflight passes before init");

    fuse::renderer::TaaResolveDesc resolveDesc{};
    resolveDesc.width = 64;
    resolveDesc.height = 64;
    resolveDesc.params = passDesc.params;

    const fuse::renderer::TaaFrameGuardPreflight preInitGuards =
        pass->preflightFrameGuards(resolveDesc, 0u);
    expectTrue(!preInitGuards.passes(), "frame guards fail before init");
    expectTrue(!preInitGuards.history.canReuse(), "frame guards block reuse before init");

    expectTrue(pass->init(resources), "TaaPass initialized for frame guard preflight test");
    expectTrue(pass->shouldSkipHistoryWarmup(), "pass should skip warmup completion after init");
    expectTrue(!pass->isHistoryWarmupComplete(), "pass warmup not complete after init");

    const fuse::renderer::TaaHistoryWarmupPreflight warmupPreflight = pass->preflightHistoryWarmup(0u);
    expectTrue(!warmupPreflight.passes(), "pass history warmup preflight fails before resolve");
    expectTrue(warmupPreflight.reuseBlock == fuse::renderer::TaaHistoryReuseBlockReason::NotWarm,
               "pass history warmup preflight reason is NotWarm");

    resolveDesc.surfaces.current_frame = reinterpret_cast<void*>(0x10);
    resolveDesc.surfaces.output = reinterpret_cast<void*>(0x20);
    const fuse::renderer::TaaResolveBlendPreflight blendPreflight =
        pass->preflightResolveBlendFrame(resolveDesc);
    expectTrue(blendPreflight.passes(), "pass blend frame preflight passes before first resolve");
    expectTrue(!blendPreflight.historyBlendAllowed, "pass blend frame preflight blocks history before warmup");

    expectTrue(pass->resolveFrame(resolveDesc), "initial resolve warms pass history");
    expectTrue(pass->isHistoryWarmupComplete(), "pass warmup complete after resolve");
    expectTrue(!pass->shouldSkipHistoryWarmup(), "pass should not skip warmup after resolve");

    const fuse::renderer::TaaFrameGuardPreflight warmedGuards = pass->preflightFrameGuards(resolveDesc, 0u);
    expectTrue(warmedGuards.passes(), "frame guards pass after warmup resolve");
    expectTrue(warmedGuards.history.canReuse(), "frame guards allow reuse after warmup");
    expectTrue(warmedGuards.blend.historyBlendAllowed, "frame guards allow history blend after warmup");

    resolveDesc.observed_history_generation = 999u;
    const fuse::renderer::TaaFrameGuardPreflight staleGuards = pass->preflightFrameGuards(resolveDesc, 999u);
    expectTrue(staleGuards.history.isWarmupComplete(), "frame guards warmup complete with warmed history");
    expectTrue(!staleGuards.history.canReuse(), "frame guards block reuse for stale observed generation");
    expectTrue(staleGuards.blend.historyDegraded, "frame guards mark blend degraded for stale observed generation");

    pass->invalidateHistory();
    const fuse::renderer::TaaFrameGuardPreflight postInvalidateGuards =
        pass->preflightFrameGuards(resolveDesc, pass->historyInvalidateGeneration());
    expectTrue(!postInvalidateGuards.history.isWarmupComplete(), "frame guards warmup incomplete after invalidate");
    expectTrue(!postInvalidateGuards.history.canReuse(), "frame guards block reuse after invalidate");
    expectTrue(!postInvalidateGuards.blend.historyDegraded, "frame guards blend not degraded after invalidate");

    pass->destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testJitterShouldSkipAndTryNdcPreflights() {
    using fuse::renderer::TaaJitterLayout;

    expectTrue(!fuse::renderer::shouldSkipTaaJitterSync(0u, 8u), "valid sequence should not skip sync");
    expectTrue(fuse::renderer::shouldSkipTaaJitterSync(0u, 0u), "invalid sequence should skip sync");
    expectTrue(!fuse::renderer::shouldSkipTaaJitterNdc(1920u, 1080u, 8u),
               "valid viewport should not skip NDC preflight");
    expectTrue(fuse::renderer::shouldSkipTaaJitterNdc(0u, 1080u, 8u),
               "zero width should skip NDC preflight");

    fuse::renderer::TaaJitterGuardRejectReason rejectReason =
        fuse::renderer::TaaJitterGuardRejectReason::None;
    expectTrue(fuse::renderer::tryPreflightTaaJitterNdc(128u, 128u, 8u, rejectReason),
               "tryPreflightTaaJitterNdc passes for valid viewport");
    expectTrue(rejectReason == fuse::renderer::TaaJitterGuardRejectReason::None,
               "tryPreflight NDC reject reason is None for valid viewport");

    expectTrue(!fuse::renderer::tryPreflightTaaJitterNdc(0u, 128u, 8u, rejectReason),
               "tryPreflightTaaJitterNdc rejects zero width");
    expectTrue(rejectReason == fuse::renderer::TaaJitterGuardRejectReason::InvalidViewport,
               "tryPreflight NDC reject reason is InvalidViewport for zero width");

    fuse::math::Vec2 ndcOut{};
    expectTrue(TaaJitterLayout::tryNdcOffsetForFrameIndex(3u, 1920u, 1080u, ndcOut, 8u),
               "tryNdcOffsetForFrameIndex succeeds for valid inputs");
    const fuse::math::Vec2 expected = TaaJitterLayout::ndcOffsetForFrameIndex(3u, 1920u, 1080u, 8u);
    expectNear(ndcOut.x, expected.x, 1e-6f, "tryNdcOffsetForFrameIndex X matches ndcOffsetForFrameIndex");
    expectNear(ndcOut.y, expected.y, 1e-6f, "tryNdcOffsetForFrameIndex Y matches ndcOffsetForFrameIndex");

    expectTrue(!TaaJitterLayout::tryNdcOffsetForFrameIndex(0u, 0u, 1080u, ndcOut, 8u),
               "tryNdcOffsetForFrameIndex rejects zero width");
}

void testHistoryWarmupPreflights() {
    fuse::renderer::TaaHistoryBuffer emptyHistory;
    expectTrue(fuse::renderer::shouldSkipTaaHistoryWarmup(emptyHistory),
               "uninitialized history should skip warmup completion");
    expectTrue(!fuse::renderer::taaHistoryWarmupComplete(emptyHistory),
               "uninitialized history warmup not complete");

    fuse::renderer::TaaHistoryReuseBlockReason warmupReason =
        fuse::renderer::TaaHistoryReuseBlockReason::None;
    expectTrue(!fuse::renderer::tryPreflightTaaHistoryWarmup(emptyHistory, warmupReason),
               "tryPreflightTaaHistoryWarmup rejects uninitialized history");
    expectTrue(warmupReason == fuse::renderer::TaaHistoryReuseBlockReason::NotReady,
               "uninitialized history warmup reason is NotReady");

    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for history warmup preflight test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaHistoryBuffer history;
    fuse::renderer::TaaHistoryBufferDesc historyDesc{64, 64};
    expectTrue(history.init(resources, historyDesc), "history ready for warmup preflight test");
    expectTrue(fuse::renderer::shouldSkipTaaHistoryWarmup(history),
               "fresh history should skip warmup completion");
    expectTrue(!fuse::renderer::taaHistoryWarmupComplete(history),
               "fresh history warmup not complete");
    expectTrue(!fuse::renderer::preflightTaaHistoryWarmup(history),
               "preflightTaaHistoryWarmup rejects unwarmed history");
    expectTrue(!fuse::renderer::tryPreflightTaaHistoryWarmup(history, warmupReason),
               "tryPreflightTaaHistoryWarmup rejects unwarmed history");
    expectTrue(warmupReason == fuse::renderer::TaaHistoryReuseBlockReason::NotWarm,
               "unwarmed history warmup reason is NotWarm");

    history.markResolved();
    expectTrue(!fuse::renderer::shouldSkipTaaHistoryWarmup(history),
               "warmed history should not skip warmup completion");
    expectTrue(fuse::renderer::taaHistoryWarmupComplete(history),
               "warmed history warmup is complete");
    expectTrue(fuse::renderer::preflightTaaHistoryWarmup(history),
               "preflightTaaHistoryWarmup passes after warmup");
    expectTrue(fuse::renderer::tryPreflightTaaHistoryWarmup(history, warmupReason),
               "tryPreflightTaaHistoryWarmup passes after warmup");
    expectTrue(warmupReason == fuse::renderer::TaaHistoryReuseBlockReason::None,
               "warmed history warmup reason is None");

    history.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testResolveShouldSkipAndTryPreflights() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for resolve shouldSkip/try test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaHistoryBuffer history;
    fuse::renderer::TaaHistoryBufferDesc historyDesc{64, 64};
    expectTrue(history.init(resources, historyDesc), "history ready for resolve shouldSkip/try test");

    fuse::renderer::TaaResolveDesc desc{};
    desc.width = 64;
    desc.height = 64;
    desc.surfaces.current_frame = reinterpret_cast<void*>(0x1);
    desc.surfaces.output = reinterpret_cast<void*>(0x2);

    expectTrue(!fuse::renderer::shouldSkipTaaResolve(desc, history),
               "valid resolve should not be skipped");
    fuse::renderer::TaaResolveSkipReason skipReason = fuse::renderer::TaaResolveSkipReason::None;
    expectTrue(fuse::renderer::tryPreflightTaaResolve(desc, history, skipReason),
               "tryPreflightTaaResolve passes for valid resolve");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::None,
               "tryPreflight resolve skip reason is None for valid resolve");

    desc.width = 0;
    expectTrue(fuse::renderer::shouldSkipTaaResolve(desc, history),
               "invalid dimensions should skip resolve");
    expectTrue(!fuse::renderer::tryPreflightTaaResolve(desc, history, skipReason),
               "tryPreflightTaaResolve rejects invalid dimensions");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::InvalidDimensions,
               "tryPreflight resolve skip reason is InvalidDimensions");

    history.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testTaaPassTryAndShouldSkipPreflights() {
    fuse::renderer::TaaPassDesc passDesc{};
    passDesc.width = 128;
    passDesc.height = 128;
    passDesc.params.blend_factor = 0.2f;

    auto pass = fuse::renderer::TaaPass::create(passDesc);
    expectTrue(pass->shouldSkipHistoryWarmup(), "pass should skip warmup completion before init");
    expectTrue(pass->preflightJitterNdc(), "pass NDC preflight passes with valid viewport before init");
    expectTrue(!pass->shouldSkipJitterNdc(), "pass should not skip NDC with valid viewport before init");

    fuse::renderer::TaaJitterGuardRejectReason jitterReject =
        fuse::renderer::TaaJitterGuardRejectReason::None;
    expectTrue(pass->tryPreflightJitterSync(4u, jitterReject), "pass tryPreflightJitterSync passes");
    expectTrue(jitterReject == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass tryPreflightJitterSync reject reason is None");

    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass try/shouldSkip test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);
    expectTrue(pass->init(resources), "TaaPass initialized for try/shouldSkip test");

    expectTrue(!pass->shouldSkipJitterNdc(), "pass should not skip NDC after init");
    expectTrue(pass->preflightJitterNdc(), "pass preflightJitterNdc passes after init");
    expectTrue(pass->tryPreflightJitterNdc(jitterReject), "pass tryPreflightJitterNdc passes after init");
    expectTrue(!pass->shouldSkipJitterSync(7u), "pass should not skip jitter sync after init");

    expectTrue(pass->shouldSkipHistoryWarmup(), "pass should skip warmup completion before resolve");
    fuse::renderer::TaaHistoryReuseBlockReason warmupReason =
        fuse::renderer::TaaHistoryReuseBlockReason::None;
    expectTrue(!pass->tryPreflightHistoryWarmup(warmupReason),
               "pass tryPreflightHistoryWarmup rejects before resolve");
    expectTrue(warmupReason == fuse::renderer::TaaHistoryReuseBlockReason::NotWarm,
               "pass warmup reason is NotWarm before resolve");

    fuse::renderer::TaaResolveDesc resolveDesc{};
    resolveDesc.width = 128;
    resolveDesc.height = 128;
    resolveDesc.surfaces.current_frame = reinterpret_cast<void*>(0x10);
    resolveDesc.surfaces.output = reinterpret_cast<void*>(0x20);
    resolveDesc.params = passDesc.params;

    expectTrue(!pass->shouldSkipResolve(resolveDesc), "pass should not skip valid resolve");
    fuse::renderer::TaaResolveSkipReason skipReason = fuse::renderer::TaaResolveSkipReason::None;
    expectTrue(pass->tryPreflightResolve(resolveDesc, skipReason),
               "pass tryPreflightResolve passes for valid resolve");

    fuse::renderer::TaaResolveBlendRejectReason blendReason =
        fuse::renderer::TaaResolveBlendRejectReason::None;
    expectTrue(pass->tryPreflightResolveBlendWeights(resolveDesc, blendReason),
               "pass tryPreflightResolveBlendWeights passes before warmup");

    fuse::renderer::TaaHistoryReuseBlockReason reuseReason =
        fuse::renderer::TaaHistoryReuseBlockReason::None;
    expectTrue(!pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse rejects before warmup");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::NotWarm,
               "pass reuse reason is NotWarm before warmup");

    expectTrue(pass->resolveFrame(resolveDesc), "initial resolve warms pass history");
    expectTrue(!pass->shouldSkipHistoryWarmup(), "pass should not skip warmup after resolve");
    expectTrue(pass->tryPreflightHistoryWarmup(warmupReason), "pass warmup preflight passes after resolve");
    expectTrue(pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse passes after warmup");
    expectTrue(!pass->shouldSkipResolve(resolveDesc), "pass should not skip resolve after warmup");

    pass->destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testJitterShouldSkipAndReadyGuards() {
    expectTrue(fuse::renderer::taaJitterSyncReady(8u), "default sequence is sync-ready");
    expectTrue(!fuse::renderer::shouldSkipTaaJitterSync(8u), "default sequence should not skip sync");
    expectTrue(fuse::renderer::shouldSkipTaaJitterSync(0u), "invalid sequence should skip sync");
    expectTrue(!fuse::renderer::taaJitterSyncReady(0u), "invalid sequence is not sync-ready");

    expectTrue(fuse::renderer::taaJitterNdcReady(128u, 128u, 8u), "valid viewport is NDC-ready");
    expectTrue(!fuse::renderer::shouldSkipTaaJitterNdc(128u, 128u, 8u),
               "valid viewport should not skip NDC production");
    expectTrue(fuse::renderer::shouldSkipTaaJitterNdc(0u, 128u, 8u),
               "zero width should skip NDC production");
    expectTrue(!fuse::renderer::taaJitterNdcReady(0u, 128u, 8u), "zero width is not NDC-ready");

    fuse::renderer::TaaJitterGuardRejectReason rejectReason =
        fuse::renderer::TaaJitterGuardRejectReason::None;
    expectTrue(fuse::renderer::tryPreflightTaaJitterNdc(1920u, 1080u, 8u, rejectReason),
               "tryPreflightTaaJitterNdc passes for valid viewport");
    expectTrue(rejectReason == fuse::renderer::TaaJitterGuardRejectReason::None,
               "tryPreflightTaaJitterNdc reject reason is None");
    expectTrue(!fuse::renderer::tryPreflightTaaJitterNdc(0u, 1080u, 8u, rejectReason),
               "tryPreflightTaaJitterNdc rejects zero width");
    expectTrue(rejectReason == fuse::renderer::TaaJitterGuardRejectReason::InvalidViewport,
               "tryPreflightTaaJitterNdc reject reason is InvalidViewport");

    fuse::renderer::TaaJitter jitter;
    expectTrue(!jitter.shouldSkipNdcOffset(128u, 128u), "jitter should not skip NDC for valid viewport");
    expectTrue(jitter.shouldSkipNdcOffset(0u, 128u), "jitter should skip NDC for zero width");
}

void testHistoryWarmupCompleteGuards() {
    fuse::renderer::TaaHistoryBuffer emptyHistory;
    expectTrue(!fuse::renderer::taaHistoryWarmupComplete(emptyHistory),
               "empty history warmup is not complete");
    expectTrue(fuse::renderer::shouldSkipTaaHistoryWarmup(emptyHistory),
               "empty history should skip past warmup gate");
    expectTrue(!fuse::renderer::taaHistoryWarmupReady(emptyHistory),
               "empty history is not warmup-ready");

    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for warmup complete guard test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaHistoryBuffer history;
    fuse::renderer::TaaHistoryBufferDesc historyDesc{64, 64};
    expectTrue(history.init(resources, historyDesc), "history ready for warmup complete guard test");
    expectTrue(!history.warmupComplete(), "fresh history warmup is not complete");
    expectTrue(fuse::renderer::shouldSkipTaaHistoryWarmup(history),
               "unwarmed history should skip warmup gate");
    expectTrue(!fuse::renderer::taaHistoryWarmupReady(history), "unwarmed history is not warmup-ready");

    history.markResolved();
    expectTrue(history.warmupComplete(), "warmed history warmup is complete");
    expectTrue(fuse::renderer::taaHistoryWarmupComplete(history), "warmed history reports warmup complete");
    expectTrue(!fuse::renderer::shouldSkipTaaHistoryWarmup(history),
               "warmed history should not skip warmup gate");
    expectTrue(fuse::renderer::taaHistoryWarmupReady(history), "warmed history is warmup-ready");

    history.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testResolveTemporalPreflightGuards() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for temporal preflight test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaHistoryBuffer history;
    fuse::renderer::TaaHistoryBufferDesc historyDesc{64, 64};
    expectTrue(history.init(resources, historyDesc), "history ready for temporal preflight test");

    fuse::renderer::TaaResolveDesc desc{};
    desc.params.blend_factor = 0.25f;
    desc.observed_history_generation = 0u;

    expectTrue(fuse::renderer::taaResolveBlendReady(desc, history),
               "warmup blend is ready before first resolve");
    expectTrue(fuse::renderer::shouldSkipTaaResolveTemporal(desc, history),
               "unwarmed history temporal preflight should skip reuse");

    fuse::renderer::TaaHistoryReuseBlockReason reuseReason = fuse::renderer::TaaHistoryReuseBlockReason::None;
    fuse::renderer::TaaResolveBlendRejectReason blendReason =
        fuse::renderer::TaaResolveBlendRejectReason::None;
    expectTrue(!fuse::renderer::preflightTaaResolveTemporal(desc, history, &reuseReason, &blendReason),
               "unwarmed history temporal preflight fails reuse gate");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::NotWarm,
               "unwarmed temporal preflight reuse reason is NotWarm");

    history.markResolved();
    expectTrue(fuse::renderer::tryPreflightTaaResolveTemporal(desc, history, reuseReason, blendReason),
               "tryPreflightTaaResolveTemporal passes after warmup");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::None,
               "warmed temporal preflight reuse reason is None");
    expectTrue(fuse::renderer::taaResolveBlendReady(desc, history),
               "steady blend is ready after warmup");

    history.invalidateHistory();
    expectTrue(fuse::renderer::shouldSkipTaaResolveTemporal(desc, history),
               "stale generation temporal preflight should skip");
    expectTrue(!fuse::renderer::tryPreflightTaaResolveTemporal(desc, history, reuseReason, blendReason),
               "tryPreflightTaaResolveTemporal fails for stale generation");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::StaleGeneration,
               "stale temporal preflight reuse reason is StaleGeneration");

    history.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testTaaPassTemporalAndJitterPreflightWrappers() {
    fuse::renderer::TaaPassDesc passDesc{};
    passDesc.width = 128;
    passDesc.height = 128;
    passDesc.params.blend_factor = 0.2f;

    auto pass = fuse::renderer::TaaPass::create(passDesc);
    expectTrue(pass->shouldSkipWarmup(), "pass should skip warmup before init");
    expectTrue(!pass->isWarmupComplete(), "pass warmup not complete before init");
    expectTrue(!pass->shouldSkipJitterSync(), "pass jitter sync valid before init");
    expectTrue(!pass->shouldSkipJitterNdc(), "pass jitter NDC valid before init");

    fuse::renderer::TaaJitterGuardRejectReason jitterReject =
        fuse::renderer::TaaJitterGuardRejectReason::None;
    expectTrue(pass->preflightJitterNdc(&jitterReject), "pass preflightJitterNdc passes before init");
    expectTrue(jitterReject == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass preflightJitterNdc reject reason is None");

    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass temporal preflight wrapper test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);
    expectTrue(pass->init(resources), "TaaPass initialized for temporal preflight wrapper test");
    expectTrue(pass->shouldSkipWarmup(), "pass should skip warmup after init");

    fuse::renderer::TaaResolveDesc resolveDesc{};
    resolveDesc.width = 128;
    resolveDesc.height = 128;
    resolveDesc.surfaces.current_frame = reinterpret_cast<void*>(0x10);
    resolveDesc.surfaces.output = reinterpret_cast<void*>(0x20);
    resolveDesc.params = passDesc.params;
    resolveDesc.observed_history_generation = 0u;

    expectTrue(pass->shouldSkipTemporalResolve(resolveDesc),
               "pass temporal preflight should skip before warmup resolve");
    expectTrue(!pass->preflightTemporalResolve(resolveDesc),
               "pass temporal preflight fails before warmup resolve");

    expectTrue(pass->resolveFrame(resolveDesc), "initial resolve warms pass history");
    expectTrue(pass->isWarmupComplete(), "pass warmup complete after resolve");
    expectTrue(!pass->shouldSkipWarmup(), "pass should not skip warmup after resolve");
    expectTrue(!pass->shouldSkipTemporalResolve(resolveDesc),
               "pass temporal preflight should not skip after warmup");

    pass->invalidateHistory();
    expectTrue(pass->shouldSkipTemporalResolve(resolveDesc),
               "pass temporal preflight should skip after invalidate");
    expectTrue(pass->shouldSkipWarmup(), "pass should skip warmup after invalidate");

    fuse::renderer::TaaPassDesc zeroWidthDesc{};
    zeroWidthDesc.width = 0;
    zeroWidthDesc.height = 128;
    auto zeroPass = fuse::renderer::TaaPass::create(zeroWidthDesc);
    expectTrue(zeroPass->shouldSkipJitterNdc(), "zero-width pass should skip jitter NDC");
    expectTrue(!zeroPass->preflightJitterNdc(&jitterReject), "zero-width pass preflightJitterNdc fails");

    pass->destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testHistoryWarmupBlockGuards() {
    expectTrue(std::strcmp(fuse::renderer::taaHistoryWarmupBlockReasonLabel(
                               fuse::renderer::TaaHistoryWarmupBlockReason::None),
                           "none") == 0,
               "None warmup block label");
    expectTrue(std::strcmp(fuse::renderer::taaHistoryWarmupBlockReasonLabel(
                               fuse::renderer::TaaHistoryWarmupBlockReason::NotReady),
                           "not_ready") == 0,
               "NotReady warmup block label");
    expectTrue(std::strcmp(fuse::renderer::taaHistoryWarmupBlockReasonLabel(
                               fuse::renderer::TaaHistoryWarmupBlockReason::NeedsWarmup),
                           "needs_warmup") == 0,
               "NeedsWarmup warmup block label");

    fuse::renderer::TaaHistoryBuffer emptyHistory;
    expectTrue(fuse::renderer::classifyTaaHistoryWarmupBlock(emptyHistory) ==
                   fuse::renderer::TaaHistoryWarmupBlockReason::NotReady,
               "empty history warmup block is NotReady");
    expectTrue(fuse::renderer::shouldSkipTaaHistoryWarmup(emptyHistory),
               "empty history should skip warmup preflight");

    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for warmup block test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaHistoryBuffer history;
    fuse::renderer::TaaHistoryBufferDesc historyDesc{64, 64};
    expectTrue(history.init(resources, historyDesc), "history ready for warmup block test");
    expectTrue(fuse::renderer::classifyTaaHistoryWarmupBlock(history) ==
                   fuse::renderer::TaaHistoryWarmupBlockReason::NeedsWarmup,
               "unwarmed history warmup block is NeedsWarmup");
    expectTrue(fuse::renderer::shouldSkipTaaHistoryWarmup(history),
               "unwarmed history should skip warmup preflight");

    fuse::renderer::TaaHistoryWarmupBlockReason reason = fuse::renderer::TaaHistoryWarmupBlockReason::None;
    expectTrue(!fuse::renderer::tryPreflightTaaHistoryWarmup(history, reason),
               "tryPreflight warmup fails for unwarmed history");
    expectTrue(reason == fuse::renderer::TaaHistoryWarmupBlockReason::NeedsWarmup,
               "tryPreflight warmup reason is NeedsWarmup");

    history.markResolved();
    expectTrue(fuse::renderer::preflightTaaHistoryWarmup(history, &reason),
               "warmed history passes warmup preflight");
    expectTrue(reason == fuse::renderer::TaaHistoryWarmupBlockReason::None,
               "warmed history warmup preflight reason is None");
    expectTrue(!fuse::renderer::shouldSkipTaaHistoryWarmup(history),
               "warmed history should not skip warmup preflight");

    history.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testJitterShouldSkipAndAdvanceGuards() {
    using fuse::renderer::TaaJitterLayout;

    expectTrue(fuse::renderer::classifyTaaJitterAdvanceReject(8u) ==
                   fuse::renderer::TaaJitterGuardRejectReason::None,
               "valid sequence passes advance classify");
    expectTrue(fuse::renderer::classifyTaaJitterAdvanceReject(0u) ==
                   fuse::renderer::TaaJitterGuardRejectReason::InvalidSequence,
               "zero sequence fails advance classify");

    expectTrue(!fuse::renderer::shouldSkipTaaJitterSync(5u, 8u),
               "valid sequence should not skip jitter sync");
    expectTrue(fuse::renderer::shouldSkipTaaJitterSync(5u, 0u),
               "invalid sequence should skip jitter sync");
    expectTrue(!fuse::renderer::shouldSkipTaaJitterAdvance(8u),
               "valid sequence should not skip jitter advance");
    expectTrue(fuse::renderer::shouldSkipTaaJitterAdvance(0u),
               "invalid sequence should skip jitter advance");

    fuse::renderer::TaaJitterGuardRejectReason rejectReason =
        fuse::renderer::TaaJitterGuardRejectReason::None;
    expectTrue(fuse::renderer::tryPreflightTaaJitterNdc(128u, 128u, 8u, rejectReason),
               "tryPreflightTaaJitterNdc passes for valid viewport");
    expectTrue(rejectReason == fuse::renderer::TaaJitterGuardRejectReason::None,
               "tryPreflightTaaJitterNdc reject reason is None");
    expectTrue(fuse::renderer::shouldSkipTaaJitterNdc(0u, 128u, 8u),
               "zero width should skip NDC jitter preflight");
    expectTrue(!fuse::renderer::shouldSkipTaaJitterNdc(128u, 128u, 8u),
               "valid viewport should not skip NDC jitter preflight");

    expectTrue(fuse::renderer::tryPreflightTaaJitterAdvance(8u, rejectReason),
               "tryPreflightTaaJitterAdvance passes for valid sequence");
    expectTrue(!fuse::renderer::tryPreflightTaaJitterAdvance(0u, rejectReason),
               "tryPreflightTaaJitterAdvance rejects invalid sequence");
    expectTrue(rejectReason == fuse::renderer::TaaJitterGuardRejectReason::InvalidSequence,
               "tryPreflightTaaJitterAdvance reject reason is InvalidSequence");

    fuse::renderer::TaaJitter jitter;
    expectTrue(!fuse::renderer::shouldSkipTaaJitterAdvance(jitter.sequenceLength()),
               "default jitter should not skip advance");
    expectTrue(jitter.advanceIfReady(), "advanceIfReady succeeds for default jitter");
    expectTrue(TaaJitterLayout::jitterSlotMatchesFrameIndex(jitter.monotonicFrameIndex(), jitter.index(),
                                                            jitter.sequenceLength()),
               "advanced jitter slot matches monotonic frame counter");
}

void testComputeResolveBlendWeightsIfReady() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for compute-if-ready test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaHistoryBuffer history;
    fuse::renderer::TaaHistoryBufferDesc historyDesc{64, 64};
    expectTrue(history.init(resources, historyDesc), "history ready for compute-if-ready test");

    fuse::renderer::TaaResolveDesc desc{};
    desc.params.blend_factor = 0.4f;

    fuse::renderer::TaaBlendWeights weights{};
    fuse::renderer::TaaResolveBlendRejectReason rejectReason =
        fuse::renderer::TaaResolveBlendRejectReason::None;
    expectTrue(fuse::renderer::computeTaaResolveBlendWeightsIfReady(desc, history, weights, &rejectReason),
               "computeIfReady passes for warmup blend");
    expectNear(weights.current, 1.f, 1e-5f, "computeIfReady warmup current weight is full");
    expectNear(weights.history, 0.f, 1e-5f, "computeIfReady warmup history weight is zero");
    expectTrue(rejectReason == fuse::renderer::TaaResolveBlendRejectReason::None,
               "computeIfReady reject reason is None for warmup");

    history.markResolved();
    expectTrue(fuse::renderer::computeTaaResolveBlendWeightsIfReady(desc, history, weights, &rejectReason),
               "computeIfReady passes after warmup");
    expectNear(weights.current, 0.4f, 1e-5f, "computeIfReady steady current weight");
    expectNear(weights.history, 0.6f, 1e-5f, "computeIfReady steady history weight");

    history.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testTaaPassWarmupAndJitterPreflightWrappers() {
    fuse::renderer::TaaPassDesc passDesc{};
    passDesc.width = 128;
    passDesc.height = 128;
    passDesc.params.blend_factor = 0.15f;

    auto pass = fuse::renderer::TaaPass::create(passDesc);
    expectTrue(pass->shouldSkipHistoryWarmup(), "pass should skip history warmup before init");
    expectTrue(pass->preflightJitterNdc(), "pass preflightJitterNdc passes before init");
    expectTrue(!pass->shouldSkipJitterSync(3u), "pass should not skip jitter sync before init");
    expectTrue(!pass->shouldSkipJitterNdc(), "pass should not skip NDC jitter before init");
    expectTrue(!pass->shouldSkipJitterAdvance(), "pass should not skip jitter advance before init");

    fuse::renderer::TaaJitterGuardRejectReason jitterReject =
        fuse::renderer::TaaJitterGuardRejectReason::None;
    expectTrue(pass->preflightJitterNdc(&jitterReject), "pass preflightJitterNdc passes");
    expectTrue(jitterReject == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass preflightJitterNdc reject reason is None");
    expectTrue(pass->preflightJitterAdvance(&jitterReject), "pass preflightJitterAdvance passes");

    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass warmup/jitter wrapper test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);
    expectTrue(pass->init(resources), "TaaPass initialized for warmup/jitter wrapper test");

    fuse::renderer::TaaHistoryWarmupBlockReason warmupReason =
        fuse::renderer::TaaHistoryWarmupBlockReason::None;
    expectTrue(pass->shouldSkipHistoryWarmup(), "pass should skip history warmup before resolve");
    expectTrue(!pass->preflightHistoryWarmup(&warmupReason), "pass warmup preflight fails before resolve");
    expectTrue(warmupReason == fuse::renderer::TaaHistoryWarmupBlockReason::NeedsWarmup,
               "pass warmup preflight reason is NeedsWarmup before resolve");

    fuse::renderer::TaaResolveDesc resolveDesc{};
    resolveDesc.width = 128;
    resolveDesc.height = 128;
    resolveDesc.surfaces.current_frame = reinterpret_cast<void*>(0x10);
    resolveDesc.surfaces.output = reinterpret_cast<void*>(0x20);
    resolveDesc.params = passDesc.params;
    expectTrue(pass->resolveFrame(resolveDesc), "initial resolve warms pass history");

    expectTrue(!pass->shouldSkipHistoryWarmup(), "pass should not skip history warmup after resolve");
    expectTrue(pass->preflightHistoryWarmup(&warmupReason), "pass warmup preflight passes after resolve");

    fuse::renderer::TaaBlendWeights weights{};
    fuse::renderer::TaaResolveBlendRejectReason blendReason =
        fuse::renderer::TaaResolveBlendRejectReason::None;
    expectTrue(pass->tryComputeResolveBlendWeights(resolveDesc, weights, blendReason),
               "pass tryComputeResolveBlendWeights passes after warmup");
    expectNear(weights.current, 0.15f, 1e-5f, "pass tryCompute steady current weight");
    expectNear(weights.history, 0.85f, 1e-5f, "pass tryCompute steady history weight");

    fuse::renderer::TaaPassDesc zeroWidthDesc{};
    zeroWidthDesc.width = 0;
    zeroWidthDesc.height = 128;
    auto zeroPass = fuse::renderer::TaaPass::create(zeroWidthDesc);
    expectTrue(zeroPass->shouldSkipJitterNdc(), "zero-width pass should skip NDC jitter");
    expectTrue(!zeroPass->shouldSkipJitterSync(0u), "zero-width pass jitter sync still valid for sequence");

    pass->destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testTaaPassReuseAndJitterGuards() {
    fuse::renderer::TaaPassDesc passDesc{};
    passDesc.width = 128;
    passDesc.height = 128;

    auto pass = fuse::renderer::TaaPass::create(passDesc);
    expectTrue(!pass->canReuseHistory(), "pass cannot reuse history before init");
    expectTrue(!pass->historyBlendAllowed(), "pass history blend blocked before init");
    expectTrue(pass->canProduceJitterNdc(), "pass jitter can produce NDC before init");

    auto bootstrap = createTaaTestBootstrap();
    if (bootstrap == nullptr) {
        std::fprintf(stderr, "SKIP: bootstrap allocated for pass reuse/jitter guard test (Vulkan device unavailable)\n");
        return;
    }
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    TAA_SKIP_UNLESS_VULKAN_READY(bootstrap);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass reuse/jitter guard test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);
    expectTrue(pass->init(resources), "TaaPass initialized for reuse/jitter guard test");
    expectTrue(!pass->canReuseHistory(), "pass cannot reuse history before first resolve");
    expectTrue(!pass->historyBlendAllowed(), "pass history blend blocked before first resolve");
    expectTrue(pass->canProduceJitterNdc(), "pass jitter can produce NDC after init");

    fuse::renderer::TaaResolveDesc resolveDesc{};
    resolveDesc.width = 128;
    resolveDesc.height = 128;
    resolveDesc.surfaces.current_frame = reinterpret_cast<void*>(0x10);
    resolveDesc.surfaces.output = reinterpret_cast<void*>(0x20);
    expectTrue(!pass->historyBlendAllowed(), "pass history blend blocked before first resolve");
    expectTrue(pass->resolveFrame(resolveDesc), "initial resolve warms pass history");
    expectTrue(pass->canReuseHistory(), "pass can reuse history after first resolve");
    expectTrue(pass->historyBlendAllowed(), "pass history blend allowed once history is warm");

    expectTrue(pass->resolveFrame(resolveDesc), "second resolve uses history blend");
    expectTrue(pass->historyBlendAllowed(), "pass history blend remains allowed");

    fuse::renderer::TaaPassDesc zeroWidthDesc{};
    zeroWidthDesc.width = 0;
    zeroWidthDesc.height = 128;
    auto zeroPass = fuse::renderer::TaaPass::create(zeroWidthDesc);
    expectTrue(!zeroPass->canProduceJitterNdc(), "zero-width pass blocks jitter NDC production");

    pass->destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testHistoryWarmupAndResolveShouldSkip() {
    fuse::renderer::TaaHistoryBuffer emptyHistory;
    expectTrue(fuse::renderer::shouldSkipTaaHistoryWarmup(emptyHistory),
               "empty history should skip warmup check (needs warmup)");
    expectTrue(fuse::renderer::shouldSkipTaaHistoryResolve(emptyHistory),
               "empty history should skip resolve");
    expectTrue(!emptyHistory.readyForResolve(), "empty history buffer not ready for resolve");

    fuse::renderer::TaaHistoryReuseBlockReason reason = fuse::renderer::TaaHistoryReuseBlockReason::None;
    expectTrue(!fuse::renderer::tryPreflightTaaHistoryReadyForResolve(emptyHistory, reason),
               "tryPreflightTaaHistoryReadyForResolve fails for empty history");
    expectTrue(reason == fuse::renderer::TaaHistoryReuseBlockReason::NotReady,
               "empty history resolve preflight reason is NotReady");

void testHistoryWarmupPreflight() {
void testHistoryWarmupGuardHelpers() {
    expectTrue(fuse::renderer::taaHistoryNeedsWarmup(emptyHistory),
               "default history needs warmup via free helper");
    expectTrue(!fuse::renderer::taaHistoryWarmupComplete(emptyHistory),
               "default history warmup incomplete via free helper");
    expectTrue(fuse::renderer::classifyTaaHistoryReuseBlock(emptyHistory, 0u) ==
                   fuse::renderer::TaaHistoryReuseBlockReason::NotReady,
               "empty history classified as NotReady");
void testHistoryWarmupCompositeGuards() {
               "empty history warmup is not complete");
    expectNear(fuse::renderer::taaHistoryWarmupProgress(emptyHistory), 0.f, 1e-5f,
               "empty history warmup progress is zero");
    expectTrue(!fuse::renderer::taaHistoryReuseReady(emptyHistory, 0u),
               "empty history is not reuse-ready");
void testHistoryWarmupBlockReason() {
void testHistoryWarmupPreflightGuards() {
void testJitterNdcTryAndShouldSkip() {
    expectTrue(std::strcmp(fuse::renderer::taaJitterGuardRejectReasonLabel(
                               fuse::renderer::TaaJitterGuardRejectReason::None),
                           "none") == 0,
               "None jitter guard label still valid");

    fuse::renderer::TaaJitterGuardRejectReason rejectReason =
        fuse::renderer::TaaJitterGuardRejectReason::None;
    expectTrue(!fuse::renderer::shouldSkipTaaJitterSync(5u, 8u), "valid sequence should not skip sync");
    expectTrue(fuse::renderer::shouldSkipTaaJitterSync(5u, 0u), "invalid sequence should skip sync");
    expectTrue(fuse::renderer::tryPreflightTaaJitterSync(5u, 8u, rejectReason),
               "tryPreflightTaaJitterSync passes for valid sequence");
    expectTrue(rejectReason == fuse::renderer::TaaJitterGuardRejectReason::None,
               "tryPreflight sync reject reason is None");

    expectTrue(!fuse::renderer::shouldSkipTaaJitterNdc(128u, 128u, 8u),
               "valid viewport should not skip NDC");
    expectTrue(fuse::renderer::shouldSkipTaaJitterNdc(0u, 128u, 8u),
               "zero width should skip NDC");
    expectTrue(fuse::renderer::tryPreflightTaaJitterNdc(128u, 128u, 8u, rejectReason),
               "tryPreflightTaaJitterNdc passes for valid viewport");
               "tryPreflight NDC reject reason is None");
    expectTrue(!fuse::renderer::tryPreflightTaaJitterNdc(0u, 128u, 8u, rejectReason),
               "tryPreflightTaaJitterNdc fails for zero width");
    expectTrue(rejectReason == fuse::renderer::TaaJitterGuardRejectReason::InvalidViewport,
               "tryPreflight NDC reject reason is InvalidViewport");

    fuse::renderer::TaaJitter jitter;
    fuse::math::Vec2 pixelOut{};
    expectTrue(jitter.currentPixelOffsetIfReady(pixelOut), "currentPixelOffsetIfReady succeeds");
    const fuse::math::Vec2 directPixel = jitter.currentPixelOffset();
    expectNear(pixelOut.x, directPixel.x, 1e-6f, "currentPixelOffsetIfReady matches currentPixelOffset X");
    expectNear(pixelOut.y, directPixel.y, 1e-6f, "currentPixelOffsetIfReady matches currentPixelOffset Y");
}

void testHistoryWarmupPreflights() {
    expectTrue(std::strcmp(fuse::renderer::taaHistoryWarmupBlockReasonLabel(
                               fuse::renderer::TaaHistoryWarmupBlockReason::None),
               "None warmup block label");
                               fuse::renderer::TaaHistoryWarmupBlockReason::NeedsWarmup),
                           "needs_warmup") == 0,
               "NeedsWarmup block label");

    expectTrue(fuse::renderer::classifyTaaHistoryWarmupBlock(emptyHistory) ==
                   fuse::renderer::TaaHistoryWarmupBlockReason::NotReady,
               "empty history warmup block is NotReady");
void testHistoryWarmupCompleteGuard() {
    expectTrue(!emptyHistory.warmupComplete(), "empty history buffer warmupComplete is false");
    expectTrue(std::strcmp(fuse::renderer::taaHistoryWarmupRejectReasonLabel(
                               fuse::renderer::TaaHistoryWarmupRejectReason::None),
               "None warmup reject label");
                               fuse::renderer::TaaHistoryWarmupRejectReason::NotReady),
                           "not_ready") == 0,
               "NotReady warmup reject label");
                               fuse::renderer::TaaHistoryWarmupRejectReason::AlreadyWarm),
                           "already_warm") == 0,
               "AlreadyWarm warmup reject label");

    expectTrue(fuse::renderer::classifyTaaHistoryWarmupReject(emptyHistory) ==
                   fuse::renderer::TaaHistoryWarmupRejectReason::NotReady,
               "empty history warmup classify is NotReady");
               "empty history should skip warmup");

    fuse::renderer::TaaHistoryWarmupRejectReason reason =
        fuse::renderer::TaaHistoryWarmupRejectReason::None;
    expectTrue(!fuse::renderer::tryPreflightTaaHistoryWarmup(emptyHistory, reason),
               "tryPreflight warmup fails for empty history");
    expectTrue(reason == fuse::renderer::TaaHistoryWarmupRejectReason::NotReady,
               "tryPreflight warmup reason is NotReady for empty history");
                               fuse::renderer::TaaHistoryWarmupBlockReason::NotReady),
               "NotReady warmup block label");
               "NeedsWarmup warmup block label");

               "empty history classified as NotReady for warmup");
               "empty history should skip warmup-complete path");
void testJitterShouldSkipAndTryNdcGuards() {
    expectTrue(!fuse::renderer::shouldSkipTaaJitterSync(5u, 8u),
               "shouldSkipTaaJitterSync passes for valid sequence");
    expectTrue(fuse::renderer::shouldSkipTaaJitterSync(5u, 0u),
               "shouldSkipTaaJitterSync rejects invalid sequence");

               "shouldSkipTaaJitterNdc passes for valid viewport");
               "shouldSkipTaaJitterNdc rejects zero width");
    using fuse::renderer::TaaJitterLayout;

    expectTrue(!fuse::renderer::shouldSkipTaaJitterSync(5u, 8u), "valid sequence does not skip sync");
    expectTrue(fuse::renderer::shouldSkipTaaJitterSync(5u, 0u), "invalid sequence skips sync");
    expectTrue(!fuse::renderer::shouldSkipTaaJitterNdc(1920u, 1080u, 8u),
               "valid viewport does not skip NDC");
    expectTrue(fuse::renderer::shouldSkipTaaJitterNdc(0u, 1080u, 8u), "zero width skips NDC");

    expectTrue(fuse::renderer::tryPreflightTaaJitterNdc(1920u, 1080u, 8u, rejectReason),
               "tryPreflightTaaJitterNdc reject reason is None");
    expectTrue(!fuse::renderer::tryPreflightTaaJitterNdc(0u, 1080u, 8u, rejectReason),
               "tryPreflightTaaJitterNdc rejects zero width");
               "tryPreflightTaaJitterNdc reject reason is InvalidViewport");

void testHistoryWarmupCompleteAndShouldSkipReuse() {
    expectTrue(emptyHistory.shouldSkipReuse(0u), "empty history should skip reuse");

    fuse::renderer::TaaHistoryWarmupBlockReason reason = fuse::renderer::TaaHistoryWarmupBlockReason::None;
               "empty history should skip warmup completion");

    jitter.syncToFrameIndex(3u);
    expectTrue(!jitter.needsResyncToFrameIndex(3u), "aligned jitter does not need resync");
    expectTrue(jitter.needsResyncToFrameIndex(4u), "drifted jitter needs resync");
    expectTrue(TaaJitterLayout::jitterSlotMatchesFrameIndex(11u, 3u, 8u),
               "jitter slot matches wrapped frame index");

               "empty history should skip warmup preflight");

    expectTrue(!fuse::renderer::shouldSkipTaaJitterSync(8u), "valid sequence should not skip jitter sync");
    expectTrue(fuse::renderer::shouldSkipTaaJitterSync(0u), "invalid sequence should skip jitter sync");

               "valid viewport should not skip jitter NDC");
               "zero width should skip jitter NDC");


    fuse::math::Vec2 ndcOut{};
    expectTrue(TaaJitterLayout::tryComputeNdcOffsetForFrameIndex(3u, 128u, 128u, 8u, ndcOut, rejectReason),
               "tryComputeNdcOffsetForFrameIndex passes for valid inputs");
    const fuse::math::Vec2 expected = TaaJitterLayout::ndcOffsetForFrameIndex(3u, 128u, 128u, 8u);
    expectNear(ndcOut.x, expected.x, 1e-6f, "tryComputeNdcOffsetForFrameIndex matches ndcOffsetForFrameIndex X");
    expectNear(ndcOut.y, expected.y, 1e-6f, "tryComputeNdcOffsetForFrameIndex matches ndcOffsetForFrameIndex Y");

    expectTrue(!TaaJitterLayout::tryComputeNdcOffsetForFrameIndex(3u, 0u, 128u, 8u, ndcOut, rejectReason),
               "tryComputeNdcOffsetForFrameIndex rejects zero width");
               "tryComputeNdcOffsetForFrameIndex reject reason is InvalidViewport");

    expectTrue(jitter.tryCurrentNdcOffsetIfReady(128u, 128u, ndcOut, rejectReason),
               "tryCurrentNdcOffsetIfReady passes for valid viewport");
    const fuse::math::Vec2 directNdc = jitter.currentNdcOffset(128u, 128u);
    expectNear(ndcOut.x, directNdc.x, 1e-6f, "tryCurrentNdcOffsetIfReady matches currentNdcOffset X");
    expectTrue(!jitter.tryCurrentNdcOffsetIfReady(0u, 128u, ndcOut, rejectReason),
               "tryCurrentNdcOffsetIfReady rejects zero width");

void testHistoryWarmupTryAndShouldSkip() {
               "tryPreflightTaaHistoryWarmup fails for empty history");
               "empty history warmup reject reason is NotReady");
void testJitterShouldSkipAndTryNdcPreflight() {
               "valid sequence should not skip jitter sync");
               "invalid sequence should skip jitter sync");


    expectTrue(fuse::renderer::tryPreflightTaaJitterNdc(192u, 108u, 8u, rejectReason),
    expectTrue(!fuse::renderer::tryPreflightTaaJitterNdc(0u, 108u, 8u, rejectReason),

void testHistoryIsWarmGuard() {
    expectTrue(!fuse::renderer::taaHistoryIsWarm(emptyHistory), "empty history is not warm");
    expectTrue(!emptyHistory.isWarm(), "empty history buffer isWarm is false");
void testHistoryWarmupPhaseClassification() {
    expectTrue(std::strcmp(fuse::renderer::taaHistoryWarmupPhaseLabel(
                               fuse::renderer::TaaHistoryWarmupPhase::NotReady),
               "NotReady warmup phase label");
                               fuse::renderer::TaaHistoryWarmupPhase::Cold),
                           "cold") == 0,
               "Cold warmup phase label");
                               fuse::renderer::TaaHistoryWarmupPhase::Warm),
                           "warm") == 0,
               "Warm warmup phase label");

    expectTrue(fuse::renderer::classifyTaaHistoryWarmupPhase(emptyHistory) ==
                   fuse::renderer::TaaHistoryWarmupPhase::NotReady,
               "empty history classified as NotReady warmup phase");
    expectTrue(!fuse::renderer::taaHistoryWarmupPhaseAllowsReuse(
               "NotReady phase does not allow reuse");

    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    TAA_SKIP_UNLESS_VULKAN_READY(bootstrap);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for warmup/resolve should-skip test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for history warmup preflight test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for warmup guard helper test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for warmup composite guard test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for warmup block test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for warmup-complete guard test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for warmup preflight test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for warmup complete test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for warmup try/should-skip test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for isWarm guard test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for warmup phase test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaHistoryBuffer history;
    fuse::renderer::TaaHistoryBufferDesc historyDesc{64, 64};
    expectTrue(history.init(resources, historyDesc), "history ready for warmup/resolve should-skip test");
    expectTrue(history.readyForResolve(), "allocated history buffer ready for resolve");
    expectTrue(!fuse::renderer::shouldSkipTaaHistoryResolve(history),
               "allocated history should not skip resolve");
    expectTrue(fuse::renderer::tryPreflightTaaHistoryReadyForResolve(history, reason),
               "tryPreflightTaaHistoryReadyForResolve passes for allocated history");
    expectTrue(reason == fuse::renderer::TaaHistoryReuseBlockReason::None,
               "allocated history resolve preflight reason is None");
    expectTrue(fuse::renderer::shouldSkipTaaHistoryWarmup(history),
               "unwarmed history should skip warmup (needs warmup)");
    history.markResolved();
    expectTrue(!fuse::renderer::shouldSkipTaaHistoryWarmup(history),
               "warmed history should not skip warmup");
    expectTrue(history.init(resources, historyDesc), "history ready for warmup preflight test");

    const fuse::renderer::TaaHistoryWarmupPreflight unwarmed = fuse::renderer::preflightTaaHistoryWarmup(history);
    expectTrue(unwarmed.readyForResolve(), "initialized history is ready for resolve");
    expectTrue(unwarmed.needs_warmup, "initialized history needs warmup");
    expectTrue(!unwarmed.warmupComplete(), "warmup incomplete before first resolve");
    expectTrue(!unwarmed.can_reuse, "unwarmed history cannot be reused");

    const fuse::renderer::TaaHistoryWarmupPreflight warmed = fuse::renderer::preflightTaaHistoryWarmup(history);
    expectTrue(warmed.warmupComplete(), "warmup complete after first resolve");
    expectTrue(warmed.can_reuse, "warmed history can be reused");
    expectTrue(warmed.accumulated_frames == 1u, "warmup preflight reports accumulated frames");
    expectTrue(history.init(resources, historyDesc), "history ready for warmup composite guard test");
    expectTrue(!fuse::renderer::taaHistoryWarmupComplete(history),
               "allocated but unwarmed history is not warmup-complete");
    expectNear(fuse::renderer::taaHistoryWarmupProgress(history), 0.f, 1e-5f,
               "unwarmed history warmup progress is zero");
    expectTrue(!fuse::renderer::taaHistoryReuseReady(history, 0u),
               "unwarmed history is not reuse-ready");

    expectTrue(fuse::renderer::taaHistoryWarmupComplete(history),
               "warmed history warmup is complete");
    expectNear(fuse::renderer::taaHistoryWarmupProgress(history), 1.f, 1e-5f,
               "warmed history warmup progress is one");
    expectTrue(fuse::renderer::taaHistoryReuseReady(history, 0u),
               "warmed history with matching generation is reuse-ready");

    history.invalidateHistory();
               "invalidated history is not reuse-ready with stale generation");
    expectTrue(!fuse::renderer::taaHistoryReuseReady(history, history.invalidateGeneration()),
               "invalidated history is not reuse-ready until re-warmed");
    expectTrue(fuse::renderer::taaHistoryReuseReady(history, history.invalidateGeneration()),
               "re-warmed history is reuse-ready with current generation");
    expectTrue(history.init(resources, historyDesc), "history ready for warmup block test");
    expectTrue(fuse::renderer::classifyTaaHistoryWarmupBlock(history) ==
                   fuse::renderer::TaaHistoryWarmupBlockReason::NeedsWarmup,
               "allocated unwarmed history warmup block is NeedsWarmup");

    fuse::renderer::TaaHistoryWarmupBlockReason reason = fuse::renderer::TaaHistoryWarmupBlockReason::None;
    expectTrue(!fuse::renderer::tryPreflightTaaHistoryWarmup(history, reason),
               "tryPreflightTaaHistoryWarmup fails before first resolve");
    expectTrue(reason == fuse::renderer::TaaHistoryWarmupBlockReason::NeedsWarmup,
               "tryPreflightTaaHistoryWarmup reason is NeedsWarmup");

               "allocated history warmup block is NeedsWarmup");
               "unwarmed history should skip warmup completion");
               "tryPreflightTaaHistoryWarmup fails for unwarmed history");
               "tryPreflight warmup reason is NeedsWarmup");

    expectTrue(fuse::renderer::taaHistoryWarmupComplete(history), "warmed history warmup is complete");
               "warmed history should not skip warmup completion");
    expectTrue(fuse::renderer::preflightTaaHistoryWarmup(history, &reason),
               "warmed history passes warmup preflight");
    expectTrue(reason == fuse::renderer::TaaHistoryWarmupBlockReason::None,
               "warmed history warmup preflight reason is None");
    expectTrue(history.init(resources, historyDesc), "history ready for warmup-complete guard test");
               "allocated but unwarmed history warmup is not complete");
    expectTrue(!history.warmupComplete(), "unwarmed history buffer warmupComplete is false");
    expectTrue(fuse::renderer::taaHistoryNeedsWarmup(history),
               "unwarmed history still needs warmup");

    expectTrue(history.warmupComplete(), "warmed history buffer warmupComplete is true");
    expectTrue(!fuse::renderer::taaHistoryNeedsWarmup(history),
               "warmed history no longer needs warmup");

               "invalidated history warmup is not complete again");
               "unwarmed history passes warmup preflight");
    expectTrue(reason == fuse::renderer::TaaHistoryWarmupRejectReason::None,
               "unwarmed history warmup preflight reason is None");
               "unwarmed history should not skip warmup");
    expectTrue(!history.warmupComplete(), "history buffer warmupComplete false before resolve");

    expectTrue(history.warmupComplete(), "history buffer warmupComplete true after resolve");
    expectTrue(fuse::renderer::classifyTaaHistoryWarmupReject(history) ==
                   fuse::renderer::TaaHistoryWarmupRejectReason::AlreadyWarm,
               "warmed history warmup classify is AlreadyWarm");
               "warmed history should skip warmup");
    expectTrue(!fuse::renderer::preflightTaaHistoryWarmup(history, &reason),
               "warmed history fails warmup preflight");
    expectTrue(reason == fuse::renderer::TaaHistoryWarmupRejectReason::AlreadyWarm,
               "warmed history warmup preflight reason is AlreadyWarm");
               "unwarmed history classified as NeedsWarmup");
               "unwarmed history should skip warmup-complete path");
    expectTrue(!history.warmupComplete(), "history buffer warmup not complete before resolve");

               "tryPreflight warmup fails for unwarmed history");

                   fuse::renderer::TaaHistoryWarmupBlockReason::None,
               "warmed history warmup block is None");
               "warmed history should not skip warmup-complete path");
    expectTrue(history.warmupComplete(), "history buffer warmupComplete after resolve");
    expectTrue(fuse::renderer::tryPreflightTaaHistoryWarmup(history, reason),
               "tryPreflight warmup passes for warmed history");
               "tryPreflight warmup reason is None for warmed history");
    expectTrue(history.init(resources, historyDesc), "history ready for warmup complete test");
               "unwarmed history warmup is not complete");
    expectTrue(history.shouldSkipReuse(0u), "unwarmed history should skip reuse");

    expectTrue(!history.shouldSkipReuse(0u), "warmed history should not skip reuse");

    expectTrue(history.shouldSkipReuse(0u), "stale generation should skip reuse");
               "unwarmed history should skip warmup preflight");
               "unwarmed history warmup block is NeedsWarmup");

    fuse::renderer::TaaHistoryWarmupBlockReason reason =
        fuse::renderer::TaaHistoryWarmupBlockReason::None;

               "warmed history should not skip warmup preflight");
               "tryPreflightTaaHistoryWarmup passes for warmed history");
               "tryPreflightTaaHistoryWarmup reason is None after warmup");
    expectTrue(history.init(resources, historyDesc), "history ready for warmup try/should-skip test");
    expectTrue(reason == fuse::renderer::TaaHistoryReuseBlockReason::NotWarm,
               "unwarmed history warmup reject reason is NotWarm");

               "warmed history warmup reject reason is None");
    expectTrue(history.init(resources, historyDesc), "history ready for isWarm guard test");
    expectTrue(!fuse::renderer::taaHistoryIsWarm(history), "allocated but unwarmed history is not warm");
    expectTrue(!history.isWarm(), "history buffer isWarm false before resolve");

    expectTrue(fuse::renderer::taaHistoryIsWarm(history), "resolved history is warm");
    expectTrue(history.isWarm(), "history buffer isWarm true after resolve");
    expectTrue(!fuse::renderer::taaHistoryNeedsWarmup(history), "warm history does not need warmup");

    expectTrue(!fuse::renderer::taaHistoryIsWarm(history), "invalidated history is not warm");
    expectTrue(!history.isWarm(), "history buffer isWarm false after invalidate");
    expectTrue(history.init(resources, historyDesc), "history ready for warmup phase test");
    expectTrue(fuse::renderer::classifyTaaHistoryWarmupPhase(history) ==
                   fuse::renderer::TaaHistoryWarmupPhase::Cold,
               "allocated history classified as Cold warmup phase");
    expectTrue(!fuse::renderer::taaHistoryWarmupPhaseAllowsReuse(
                   fuse::renderer::TaaHistoryWarmupPhase::Cold),
               "Cold phase does not allow reuse");

                   fuse::renderer::TaaHistoryWarmupPhase::Warm,
               "warmed history classified as Warm warmup phase");
    expectTrue(fuse::renderer::taaHistoryWarmupPhaseAllowsReuse(
                   fuse::renderer::TaaHistoryWarmupPhase::Warm),
               "Warm phase allows reuse");

    history.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());

void testJitterShouldSkipAndAdvancePreflight() {
    using fuse::renderer::TaaJitterLayout;

               "valid sequence should not skip jitter sync");
               "invalid sequence should skip jitter sync");
               "valid viewport should not skip NDC jitter");
               "zero width should skip NDC jitter");
void testJitterShouldSkipAndReadyGuards() {
    expectTrue(fuse::renderer::taaJitterSyncReady(12u, 8u), "valid sequence is jitter-sync-ready");
    expectTrue(!fuse::renderer::shouldSkipTaaJitterSync(12u, 8u),
    expectTrue(fuse::renderer::shouldSkipTaaJitterSync(12u, 0u),
    expectTrue(!fuse::renderer::taaJitterSyncReady(12u, 0u),
               "invalid sequence is not jitter-sync-ready");

    expectTrue(fuse::renderer::taaJitterNdcReady(1920u, 1080u, 8u),
               "valid viewport is jitter-NDC-ready");
    expectTrue(!fuse::renderer::shouldSkipTaaJitterNdc(1920u, 1080u, 8u),
               "valid viewport should not skip jitter NDC");
    expectTrue(fuse::renderer::shouldSkipTaaJitterNdc(0u, 1080u, 8u),
               "zero width should skip jitter NDC");
    expectTrue(!fuse::renderer::taaJitterNdcReady(0u, 1080u, 8u),
               "zero width is not jitter-NDC-ready");


    expectTrue(fuse::renderer::tryPreflightTaaJitterNdc(128u, 128u, 8u, rejectReason),

    expectTrue(!fuse::renderer::tryPreflightTaaJitterNdc(0u, 128u, 8u, rejectReason),

    expectTrue(fuse::renderer::classifyTaaJitterAdvanceReject(8u) ==
                   fuse::renderer::TaaJitterGuardRejectReason::None,
               "valid sequence passes advance classify");
    expectTrue(fuse::renderer::classifyTaaJitterAdvanceReject(0u) ==
                   fuse::renderer::TaaJitterGuardRejectReason::InvalidSequence,
               "zero sequence fails advance classify");
    expectTrue(!fuse::renderer::shouldSkipTaaJitterAdvance(8u),
               "valid sequence should not skip jitter advance");
    expectTrue(fuse::renderer::shouldSkipTaaJitterAdvance(0u),
               "invalid sequence should skip jitter advance");
    expectTrue(fuse::renderer::tryPreflightTaaJitterAdvance(8u, rejectReason),
               "tryPreflightTaaJitterAdvance passes for valid sequence");

    fuse::math::Vec2 pixelOut{};
    expectTrue(TaaJitterLayout::offsetForFrameIndexIfReady(3u, 8u, pixelOut),
               "offsetForFrameIndexIfReady succeeds for valid sequence");
    const fuse::math::Vec2 directPixel = TaaJitterLayout::offsetForFrameIndex(3u, 8u);
    expectNear(pixelOut.x, directPixel.x, 1e-6f, "offsetForFrameIndexIfReady matches offsetForFrameIndex X");
    expectTrue(!TaaJitterLayout::offsetForFrameIndexIfReady(3u, 0u, pixelOut),
               "offsetForFrameIndexIfReady fails for invalid sequence");

    fuse::math::Vec2 ndcOut{};
    expectTrue(TaaJitterLayout::ndcOffsetForFrameIndexIfReady(3u, 128u, 128u, 8u, ndcOut),
               "ndcOffsetForFrameIndexIfReady succeeds for valid viewport");
    const fuse::math::Vec2 directNdc = TaaJitterLayout::ndcOffsetForFrameIndex(3u, 128u, 128u, 8u);
    expectNear(ndcOut.x, directNdc.x, 1e-6f, "ndcOffsetForFrameIndexIfReady matches ndcOffsetForFrameIndex X");
    expectTrue(!TaaJitterLayout::ndcOffsetForFrameIndexIfReady(3u, 0u, 128u, 8u, ndcOut),
               "ndcOffsetForFrameIndexIfReady fails for zero width");

void testResolveShouldSkipAndTryPreflight() {
void testHistoryReuseForResolveGuards() {
    expectTrue(bootstrap != nullptr, "bootstrap allocated for reuse-for-resolve test");



    expectTrue(history.init(resources, historyDesc), "history ready for reuse-for-resolve test");

    fuse::renderer::TaaResolveDesc desc{};
    desc.observed_history_generation = fuse::renderer::kTaaResolveNoHistoryGeneration;
    expectTrue(fuse::renderer::classifyTaaHistoryReuseBlockForResolve(desc, history) ==
                   fuse::renderer::TaaHistoryReuseBlockReason::NotWarm,
               "sentinel desc reuse block is NotWarm before warmup");

    expectTrue(fuse::renderer::preflightTaaHistoryReuseForResolve(desc, history),
               "sentinel desc reuse preflight passes after warmup");

    desc.observed_history_generation = 0u;
                   fuse::renderer::TaaHistoryReuseBlockReason::StaleGeneration,
               "explicit generation reuse block is StaleGeneration after invalidate");

    expectTrue(!fuse::renderer::tryPreflightTaaHistoryReuseForResolve(desc, history, reason),
               "tryPreflightTaaHistoryReuseForResolve fails on stale generation");
    expectTrue(reason == fuse::renderer::TaaHistoryReuseBlockReason::StaleGeneration,
               "tryPreflightTaaHistoryReuseForResolve reason is StaleGeneration");


void testJitterSyncViewportGuards() {
    expectTrue(std::strcmp(fuse::renderer::taaJitterSyncBlockReasonLabel(
                               fuse::renderer::TaaJitterSyncBlockReason::InvalidViewport),
                           "invalid_viewport") == 0,
               "InvalidViewport sync block label");
    expectTrue(fuse::renderer::classifyTaaJitterSyncViewportBlock(0u, 1080u, 8u) ==
                   fuse::renderer::TaaJitterSyncBlockReason::InvalidViewport,
               "zero width blocks viewport-aware sync");
    expectTrue(fuse::renderer::classifyTaaJitterSyncViewportBlock(1920u, 1080u, 0u) ==
                   fuse::renderer::TaaJitterSyncBlockReason::InvalidSequence,
               "invalid sequence blocks viewport-aware sync after viewport passes");

    fuse::renderer::TaaJitter jitter;
    fuse::renderer::TaaJitterSyncBlockReason reason = fuse::renderer::TaaJitterSyncBlockReason::None;
    expectTrue(jitter.trySyncToFrameIndexIfReady(4u, reason), "trySyncToFrameIndexIfReady succeeds");
    expectTrue(reason == fuse::renderer::TaaJitterSyncBlockReason::None, "sync try reason is None");
    expectTrue(jitter.isAlignedToFrameIndex(4u), "jitter aligned after try sync");

    expectTrue(!jitter.syncToFrameIndexIfViewportReady(5u, 0u, 1080u),
               "syncToFrameIndexIfViewportReady blocks zero width");
    expectTrue(jitter.trySyncToFrameIndexIfViewportReady(6u, 1920u, 1080u, reason),
               "trySyncToFrameIndexIfViewportReady succeeds with valid viewport");
    expectTrue(jitter.isAlignedToFrameIndex(6u), "jitter aligned after viewport try sync");

    fuse::renderer::TaaJitterAdvanceBlockReason advanceReason = fuse::renderer::TaaJitterAdvanceBlockReason::None;
    expectTrue(jitter.tryAdvanceIfReady(advanceReason), "tryAdvanceIfReady succeeds");
    expectTrue(jitter.tryAdvanceIfViewportReady(1920u, 1080u, advanceReason),
               "tryAdvanceIfViewportReady succeeds with valid viewport");
    expectTrue(!jitter.advanceIfViewportReady(0u, 1080u), "advanceIfViewportReady blocks zero width");

    fuse::renderer::TaaPassDesc passDesc{};
    passDesc.width = 0;
    passDesc.height = 128;
    auto pass = fuse::renderer::TaaPass::create(passDesc);
    expectTrue(!pass->syncJitterToFrameIndexIfViewportReady(2u),
               "pass syncJitterToFrameIndexIfViewportReady blocks zero width");
    fuse::renderer::TaaJitterSyncBlockReason passReason = fuse::renderer::TaaJitterSyncBlockReason::None;
    expectTrue(!pass->trySyncJitterToFrameIndexIfReady(2u, passReason),
               "pass trySyncJitterToFrameIndexIfReady blocks zero width");
    expectTrue(passReason == fuse::renderer::TaaJitterSyncBlockReason::InvalidViewport,
               "pass sync try reason is InvalidViewport");

void testResolveTemporalAccumulationPreflight() {
    expectTrue(bootstrap != nullptr, "bootstrap allocated for temporal preflight test");



    expectTrue(history.init(resources, historyDesc), "history ready for temporal preflight test");
void testTaaJitterSyncRejectReasonGuards() {
    expectTrue(std::strcmp(fuse::renderer::taaJitterSyncRejectReasonLabel(
                               fuse::renderer::TaaJitterSyncRejectReason::None),
               "None jitter sync reject label");
void testJitterSyncRejectReasonGuards() {

                               fuse::renderer::TaaJitterSyncRejectReason::InvalidSequence),
                           "invalid_sequence") == 0,
               "InvalidSequence jitter sync reject label");

    expectTrue(fuse::renderer::classifyTaaJitterSyncReject(5u, 8u) ==
                   fuse::renderer::TaaJitterSyncRejectReason::None,
               "valid sequence classifies as None");
    expectTrue(fuse::renderer::classifyTaaJitterSyncReject(0u, 0u) ==
                   fuse::renderer::TaaJitterSyncRejectReason::InvalidSequence,
               "invalid sequence classifies as InvalidSequence");

    fuse::renderer::TaaJitterSyncRejectReason reason = fuse::renderer::TaaJitterSyncRejectReason::None;
    expectTrue(fuse::renderer::preflightTaaJitterSync(12u, 8u, &reason),
               "preflightTaaJitterSync passes for valid sequence");
    expectTrue(reason == fuse::renderer::TaaJitterSyncRejectReason::None,
               "valid sequence preflight reason is None");
    expectTrue(!fuse::renderer::preflightTaaJitterSync(0u, 0u, &reason),
               "preflightTaaJitterSync fails for invalid sequence");
    expectTrue(reason == fuse::renderer::TaaJitterSyncRejectReason::InvalidSequence,
               "invalid sequence preflight reason is InvalidSequence");

    expectTrue(jitter.needsResyncToFrameIndex(5u), "default jitter needs resync to frame five");
    fuse::renderer::TaaJitterSyncRejectReason syncReason = fuse::renderer::TaaJitterSyncRejectReason::None;
    expectTrue(jitter.syncToFrameIndexIfReady(5u, &syncReason), "syncToFrameIndexIfReady with reason succeeds");
    expectTrue(syncReason == fuse::renderer::TaaJitterSyncRejectReason::None,
               "successful sync reason is None");
    expectTrue(!jitter.needsResyncToFrameIndex(5u), "aligned jitter does not need resync");
    expectTrue(jitter.needsResyncToFrameIndex(6u), "aligned jitter needs resync for different frame");
    expectTrue(fuse::renderer::classifyTaaJitterSyncReject(8u) ==
               "valid sequence has no sync reject reason");
    expectTrue(fuse::renderer::classifyTaaJitterSyncReject(0u) ==
               "invalid sequence classified as InvalidSequence");

    expectTrue(fuse::renderer::jitterNeedsResyncToFrameIndex(jitter, 5u),
               "default jitter needs resync to frame five");
    expectTrue(fuse::renderer::wouldResyncJitterToFrameIndex(jitter, 5u),
               "wouldResync mirrors jitterNeedsResync");

    expectTrue(fuse::renderer::trySyncJitterToFrameIndexIfReady(jitter, 5u, syncReason),
               "trySync succeeds for valid sequence");
               "trySync reject reason is None on success");
    expectTrue(!fuse::renderer::jitterNeedsResyncToFrameIndex(jitter, 5u),
               "jitter aligned after trySync");

    fuse::renderer::TaaJitterDesc invalidDesc{};
    invalidDesc.sequence_length = 0u;
    fuse::renderer::TaaJitter fallbackJitter(invalidDesc);
    expectTrue(fallbackJitter.sequenceLength() == 8u, "invalid desc falls back to default sequence length");
    expectTrue(fallbackJitter.syncToFrameIndexIfReady(3u, &syncReason),
               "fallback jitter sync succeeds with default sequence length");
               "fallback jitter sync reason is None");
    expectTrue(!fallbackJitter.needsResyncToFrameIndex(3u), "fallback jitter aligned after sync");

void testTaaHistoryWarmupCompleteAndTemporalReuse() {
    expectTrue(!fuse::renderer::taaHistoryWarmupComplete(history), "empty history warmup incomplete");
    expectTrue(fuse::renderer::taaHistoryWarmupFramesRemaining(history) == 1u,
               "empty history has one warmup frame remaining");





    expectTrue(!fuse::renderer::tryCanBeginTemporalReuse(history, 0u, &reason),
               "tryCanBeginTemporalReuse fails before warmup");
    expectTrue(reason == fuse::renderer::TaaHistoryReuseBlockReason::NotWarm,
               "tryCanBeginTemporalReuse reason is NotWarm before warmup");
               "allocated unwarmed history warmup incomplete");

    expectTrue(fuse::renderer::taaHistoryWarmupComplete(history), "warmed history warmup complete");
    expectTrue(fuse::renderer::tryCanBeginTemporalReuse(history, 0u, &reason),
               "tryCanBeginTemporalReuse passes after warmup");
               "tryCanBeginTemporalReuse reason is None after warmup");

               "tryCanBeginTemporalReuse fails after invalidate");
               "tryCanBeginTemporalReuse reason is StaleGeneration after invalidate");


void testPreflightTaaResolveFrame() {
    expectTrue(bootstrap != nullptr, "bootstrap allocated for resolve frame preflight test");



    expectTrue(history.init(resources, historyDesc), "history ready for resolve frame preflight test");

void testResolveFramePreflightGuards() {




}

void testResolveFrameCompositePreflight() {
    fuse::renderer::TaaHistoryBuffer emptyHistory;
    expectTrue(fuse::renderer::shouldSkipTaaResolveFrame({}, emptyHistory),
               "empty resolve frame should skip composite preflight");


void testTemporalResolveGuardPreflights() {

void testHistoryReuseForResolvePreflight() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for temporal resolve guard test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for resolve reuse preflight test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaHistoryBuffer history;
    fuse::renderer::TaaHistoryBufferDesc historyDesc{64, 64};


void testResolveTryAndShouldSkip() {
    expectTrue(history.init(resources, historyDesc), "history ready for resolve reuse preflight test");

    fuse::renderer::TaaResolveDesc desc{};
    fuse::renderer::TaaHistoryReuseBlockReason reason = fuse::renderer::TaaHistoryReuseBlockReason::None;
    expectTrue(fuse::renderer::shouldSkipTaaHistoryReuseForResolve(desc, history),
               "unwarmed history should skip resolve reuse");
    expectTrue(fuse::renderer::classifyTaaHistoryReuseBlockForResolve(desc, history) ==
                   fuse::renderer::TaaHistoryReuseBlockReason::NotWarm,
               "unwarmed history resolve reuse block is NotWarm");

    history.markResolved();
    expectTrue(fuse::renderer::preflightTaaHistoryReuseForResolve(desc, history, &reason),
               "warmed history passes resolve reuse preflight with sentinel generation");
    expectTrue(fuse::renderer::tryPreflightTaaHistoryReuseForResolve(desc, history, reason),
               "tryPreflight passes for warmed history with sentinel generation");

    history.invalidateHistory();
    desc.observed_history_generation = 0u;
    expectTrue(!fuse::renderer::preflightTaaHistoryReuseForResolve(desc, history, &reason),
               "stale observed generation fails resolve reuse preflight");
    expectTrue(reason == fuse::renderer::TaaHistoryReuseBlockReason::StaleGeneration,
               "stale generation resolve reuse reason is StaleGeneration");

    desc.observed_history_generation = history.invalidateGeneration();
               "current generation passes resolve reuse preflight");

    history.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testJitterShouldSkipAndTryNdcPreflight() {
    expectTrue(!fuse::renderer::shouldSkipTaaJitterSync(8u), "valid sequence does not skip jitter sync");
    expectTrue(fuse::renderer::shouldSkipTaaJitterSync(0u), "invalid sequence skips jitter sync");
    expectTrue(!fuse::renderer::shouldSkipTaaJitterNdc(128u, 128u, 8u),
               "valid viewport does not skip jitter NDC");
    expectTrue(fuse::renderer::shouldSkipTaaJitterNdc(0u, 128u, 8u),
               "zero width skips jitter NDC");

    fuse::renderer::TaaJitterGuardRejectReason rejectReason =
        fuse::renderer::TaaJitterGuardRejectReason::None;
    expectTrue(fuse::renderer::tryPreflightTaaJitterNdc(1920u, 1080u, 8u, rejectReason),
               "tryPreflightTaaJitterNdc passes for valid viewport");
    expectTrue(rejectReason == fuse::renderer::TaaJitterGuardRejectReason::None,
               "tryPreflightTaaJitterNdc reject reason is None");
    expectTrue(!fuse::renderer::tryPreflightTaaJitterNdc(0u, 1080u, 8u, rejectReason),
               "tryPreflightTaaJitterNdc rejects zero width");
    expectTrue(rejectReason == fuse::renderer::TaaJitterGuardRejectReason::InvalidViewport,
               "tryPreflightTaaJitterNdc reject reason is InvalidViewport");

    fuse::renderer::TaaJitter jitter;
    expectTrue(jitter.trySyncToFrameIndex(9u, rejectReason), "trySyncToFrameIndex succeeds for valid sequence");
    expectTrue(jitter.isAlignedToFrameIndex(9u), "jitter aligned after trySyncToFrameIndex");
               "trySyncToFrameIndex reject reason is None");

void testResolveFrameGuardsPreflight() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for resolve frame guards test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaHistoryBuffer history;
    fuse::renderer::TaaHistoryBufferDesc historyDesc{64, 64};
    expectTrue(history.init(resources, historyDesc), "history ready for resolve frame guards test");

    desc.width = 64;
    desc.height = 64;
    desc.surfaces.current_frame = reinterpret_cast<void*>(0x10);
    desc.surfaces.output = reinterpret_cast<void*>(0x20);

    expectTrue(fuse::renderer::shouldSkipTaaResolve(desc, emptyHistory),
               "resolve should skip when history not ready");
    fuse::renderer::TaaResolveSkipReason skipReason = fuse::renderer::TaaResolveSkipReason::None;
    expectTrue(!fuse::renderer::tryPreflightTaaResolve(desc, emptyHistory, skipReason),
               "tryPreflightTaaResolve fails when history not ready");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::HistoryNotReady,
               "tryPreflightTaaResolve skip reason is HistoryNotReady");

void testHistoryReusePreflight() {
void testJitterSyncBlockGuards() {

                               fuse::renderer::TaaJitterSyncBlockReason::None),
               "None jitter sync block label");
                               fuse::renderer::TaaJitterSyncBlockReason::InvalidSequence),
               "InvalidSequence jitter sync block label");
               "InvalidViewport jitter sync block label");

    expectTrue(fuse::renderer::classifyTaaJitterSyncBlock(1920u, 1080u, 8u) ==
                   fuse::renderer::TaaJitterSyncBlockReason::None,
               "valid viewport and sequence are not blocked");
    expectTrue(fuse::renderer::classifyTaaJitterSyncBlock(1920u, 1080u, 0u) ==
               "zero sequence classified as InvalidSequence");
    expectTrue(fuse::renderer::classifyTaaJitterSyncBlock(0u, 1080u, 8u) ==
               "zero width classified as InvalidViewport");

    expectTrue(fuse::renderer::preflightTaaJitterSync(5u, 128u, 128u, 8u, &reason),
               "valid jitter sync preflight passes");
    expectTrue(reason == fuse::renderer::TaaJitterSyncBlockReason::None,
               "valid jitter sync preflight reason is None");
    expectTrue(!fuse::renderer::preflightTaaJitterSync(5u, 0u, 128u, 8u, &reason),
               "zero-width jitter sync preflight fails");
    expectTrue(reason == fuse::renderer::TaaJitterSyncBlockReason::InvalidViewport,
               "zero-width jitter sync preflight reason is InvalidViewport");

    expectTrue(TaaJitterLayout::canSyncAndProduceNdc(1920u, 1080u, 8u),
               "valid viewport and sequence can sync and produce NDC");
    expectTrue(!TaaJitterLayout::canSyncAndProduceNdc(0u, 1080u, 8u),
               "zero width blocks sync and NDC production");

    expectTrue(jitter.classifySyncBlock(128u, 128u) == fuse::renderer::TaaJitterSyncBlockReason::None,
               "default jitter sync block is None for valid viewport");
    expectTrue(jitter.preflightSync(5u, 128u, 128u, &reason),
               "default jitter preflightSync passes for valid viewport");
    expectTrue(!jitter.preflightSync(5u, 0u, 128u, &reason),
               "default jitter preflightSync fails for zero width");

void testPreflightTaaResolveWithBlend() {
    syncReason = fuse::renderer::TaaJitterSyncRejectReason::None;
    expectTrue(fuse::renderer::trySyncJitterToFrameIndexIfReady(fallbackJitter, 2u, syncReason),
               "fallback jitter sync reject reason is None");
    expectTrue(!fuse::renderer::jitterNeedsResyncToFrameIndex(fallbackJitter, 2u),
               "fallback jitter aligned after sync");

void testHistorySampleAndWarmupGuards() {
    expectTrue(fuse::renderer::shouldSkipTaaHistoryReuse(emptyHistory, 0u),
               "empty history should skip reuse");
    expectTrue(fuse::renderer::wouldInvalidateHistoryIfStale(emptyHistory, 99u),
               "empty history would invalidate stale observed generation");

    fuse::renderer::TaaHistoryReuseBlockReason reuseReason = fuse::renderer::TaaHistoryReuseBlockReason::None;
    expectTrue(!fuse::renderer::tryPreflightTaaHistoryReuse(emptyHistory, 0u, reuseReason),
               "tryPreflight fails for empty history");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::NotReady,
               "empty history tryPreflight reason is NotReady");


void testResolveBlendReadyGuard() {
void testJitterSyncAndNdcShouldSkip() {
    expectTrue(fuse::renderer::taaJitterSyncReady(5u, 8u), "valid sequence is jitter-sync ready");
    expectTrue(!fuse::renderer::taaJitterSyncReady(5u, 0u), "invalid sequence is not jitter-sync ready");

    expectTrue(fuse::renderer::taaJitterSyncReady(12u, 8u, &rejectReason),
               "taaJitterSyncReady passes for valid sequence");
               "taaJitterSyncReady reject reason is None");

    expectTrue(fuse::renderer::taaJitterNdcReady(128u, 128u, 8u), "valid viewport is NDC jitter ready");
    expectTrue(!fuse::renderer::taaJitterNdcReady(0u, 128u, 8u),
               "zero width is not NDC jitter ready");


void testResolvePreflightShouldSkip() {
void testTaaDeepenDiagnosticGuardOverloads() {
               "empty history should skip warmup completion");
    expectTrue(fuse::renderer::wouldSkipTaaHistoryReuse(emptyHistory, 0u),
               "wouldSkip alias matches shouldSkip for empty history");
               "shouldSkip and wouldSkip agree for empty history");

    expectTrue(bootstrap != nullptr, "bootstrap allocated for resolve-with-blend preflight test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for history sample guard test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for resolve-blend-ready test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for resolve should-skip test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for diagnostic guard overload test");



    expectTrue(history.init(resources, historyDesc), "history ready for resolve-with-blend preflight test");

    desc.surfaces.current_frame = reinterpret_cast<void*>(0x1);
    desc.surfaces.output = reinterpret_cast<void*>(0x2);
    desc.params.blend_factor = 0.2f;

    fuse::renderer::TaaResolveBlendRejectReason blendReason =
        fuse::renderer::TaaResolveBlendRejectReason::None;
    expectTrue(fuse::renderer::preflightTaaResolveWithBlend(desc, history, &skipReason, &blendReason),
               "warmup resolve-with-blend preflight passes");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::None,
               "warmup resolve-with-blend skip reason is None");
    expectTrue(blendReason == fuse::renderer::TaaResolveBlendRejectReason::None,
               "warmup resolve-with-blend blend reason is None");

    desc.width = 0;
    expectTrue(!fuse::renderer::preflightTaaResolveWithBlend(desc, history, &skipReason, &blendReason),
               "invalid dimensions fail resolve-with-blend preflight");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::InvalidDimensions,
               "invalid dimensions resolve-with-blend skip reason is InvalidDimensions");
               "invalid dimensions resolve-with-blend blend reason is None");
    expectTrue(history.init(resources, historyDesc), "history ready for resolve-blend-ready test");

    desc.params.blend_factor = 0.4f;

    expectTrue(fuse::renderer::taaResolveBlendReady(desc, history),
               "warmup blend is ready before first resolve");
    expectTrue(!fuse::renderer::shouldSkipTaaResolveBlend(desc, history),
               "warmup blend should not be skipped when ready");

               "steady blend is ready after warmup");
               "steady blend should not be skipped when ready");
    expectTrue(history.init(resources, historyDesc), "history ready for resolve should-skip test");


    expectTrue(!fuse::renderer::shouldSkipTaaResolve(desc, history),
               "valid resolve should not be skipped");
    expectTrue(fuse::renderer::taaResolveReady(desc, history), "valid resolve is ready");

    expectTrue(fuse::renderer::tryPreflightTaaResolve(desc, history, skipReason),
               "tryPreflightTaaResolve passes for valid resolve");
               "tryPreflightTaaResolve skip reason is None");

    expectTrue(fuse::renderer::shouldSkipTaaResolve(desc, history),
               "invalid dimensions should skip resolve");
    expectTrue(!fuse::renderer::taaResolveReady(desc, history), "invalid resolve is not ready");
    expectTrue(!fuse::renderer::tryPreflightTaaResolve(desc, history, skipReason),
               "tryPreflightTaaResolve rejects invalid dimensions");
               "tryPreflightTaaResolve skip reason is InvalidDimensions");

               "warmup blend weights are ready before first resolve");

               "steady blend weights are ready after warmup");

    expectTrue(fuse::renderer::preflightTaaResolveFrame(desc, history, &skipReason, &blendReason),
               "resolve frame preflight passes for valid desc");
               "resolve frame preflight skip reason is None");
               "resolve frame preflight blend reason is None");
    expectTrue(!fuse::renderer::shouldSkipTaaResolveFrame(desc, history),
               "shouldSkipTaaResolveFrame passes for valid desc");

    expectTrue(!fuse::renderer::preflightTaaResolveFrame(desc, history, &skipReason, &blendReason),
               "resolve frame preflight rejects invalid dimensions");
               "resolve frame preflight skip reason is InvalidDimensions");
    expectTrue(fuse::renderer::shouldSkipTaaResolveFrame(desc, history),
               "shouldSkipTaaResolveFrame rejects invalid dimensions");


void testResolveStatsBlendConsistent() {
    expectTrue(bootstrap != nullptr, "bootstrap allocated for history reuse preflight test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for stats blend consistency test");



               "resolve should not skip with valid desc and history");
               "tryPreflightTaaResolve passes with valid desc and history");

    desc.width = 0u;
               "resolve should skip with invalid dimensions");
               "tryPreflightTaaResolve fails with invalid dimensions");
    expectTrue(history.init(resources, historyDesc), "history ready for reuse preflight test");

    const fuse::renderer::TaaHistoryReusePreflight current =
        fuse::renderer::preflightTaaHistoryReuse(history, 0u);
    expectTrue(current.generation_matches, "current generation matches after init");
    expectTrue(current.reuse_allowed, "reuse allowed with current generation");
    expectTrue(current.canReuseHistory(), "reuse preflight canReuseHistory mirrors reuse_allowed");

    const fuse::renderer::TaaHistoryReusePreflight stale =
    expectTrue(!stale.generation_matches, "stale observed generation fails match");
    expectTrue(!stale.reuse_allowed, "stale generation blocks reuse");

    const fuse::renderer::TaaHistoryReusePreflight bypass =
        fuse::renderer::preflightTaaHistoryReuseForDesc(history, desc);
    expectTrue(bypass.reuse_allowed, "sentinel bypass allows reuse when history is warm");
    expectTrue(history.init(resources, historyDesc), "history ready for warmup guard helper test");
    expectTrue(fuse::renderer::taaHistoryNeedsWarmup(history), "fresh history needs warmup");
    expectTrue(fuse::renderer::classifyTaaHistoryReuseBlock(history, 0u) ==
                   fuse::renderer::TaaHistoryReuseBlockReason::NeedsWarmup,
    expectTrue(fuse::renderer::taaHistoryReuseBlocked(history, 0u),
               "unwarmed history reuse blocked");

    expectTrue(fuse::renderer::taaHistoryWarmupComplete(history), "warmed history reports warmup complete");
                   fuse::renderer::TaaHistoryReuseBlockReason::None,
               "warmed history has no reuse block reason");
    expectTrue(!fuse::renderer::taaHistoryReuseBlocked(history, 0u),
               "warmed history reuse not blocked");

               "invalidated history needs warmup again");

               "stale observed generation classified after warmup");

    expectTrue(std::strcmp(fuse::renderer::taaHistoryReuseBlockReasonLabel(
                               fuse::renderer::TaaHistoryReuseBlockReason::NeedsWarmup),
               "NeedsWarmup reuse block label");
    expectTrue(history.init(resources, historyDesc), "history ready for stats blend consistency test");

    fuse::renderer::TaaResolve resolve;
    desc.params.blend_factor = 0.25f;

    expectTrue(resolve.resolve(desc, history), "first resolve succeeds");
    expectTrue(fuse::renderer::taaResolveStatsBlendConsistent(resolve.lastStats(), desc, history),
               "first resolve stats blend is consistent");

    expectTrue(resolve.resolve(desc, history), "second resolve succeeds");
               "second resolve stats blend is consistent");

    fuse::renderer::TaaResolveStats inconsistent{};
    inconsistent.resolved = true;
    inconsistent.first_frame = false;
    inconsistent.effective_blend = 0.6f;
    inconsistent.history_blend = 0.6f;
    expectTrue(!fuse::renderer::taaResolveStatsBlendConsistent(inconsistent, desc, history),
               "invalid stats blend weights fail consistency check");


void testTaaPassTryClassifyWrappers() {
void testTaaPassCompositeGuards() {
    passDesc.width = 128;
    passDesc.params.blend_factor = 0.3f;


    expectTrue(pass->classifyJitterSyncReject() == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass classifyJitterSyncReject passes before init");

    fuse::renderer::TaaJitterGuardRejectReason jitterReject =
    expectTrue(pass->tryPreflightJitterSync(5u, jitterReject),
               "pass tryPreflightJitterSync passes before init");
    expectTrue(jitterReject == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass tryPreflightJitterSync reject reason is None before init");

    expectTrue(!pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse fails before init");
               "pass tryPreflightHistoryReuse reason is NotReady before init");
    expectTrue(!pass->tryPreflightHistoryReadyForResolve(reuseReason),
               "pass tryPreflightHistoryReadyForResolve fails before init");
               "pass tryPreflightHistoryReadyForResolve reason is NotReady before init");

    fuse::renderer::TaaResolveDesc preInitResolveDesc{};
    preInitResolveDesc.width = 128;
    preInitResolveDesc.height = 128;
    preInitResolveDesc.surfaces.current_frame = reinterpret_cast<void*>(0x10);
    preInitResolveDesc.surfaces.output = reinterpret_cast<void*>(0x20);
    fuse::renderer::TaaResolveSkipReason preInitSkip = fuse::renderer::TaaResolveSkipReason::None;
    expectTrue(!pass->tryPreflightResolve(preInitResolveDesc, preInitSkip),
               "pass tryPreflightResolve fails before init");
    expectTrue(preInitSkip == fuse::renderer::TaaResolveSkipReason::HistoryNotReady,
               "pass tryPreflightResolve skip reason is HistoryNotReady before init");
    expectTrue(pass->classifyResolveSkip(preInitResolveDesc) ==
                   fuse::renderer::TaaResolveSkipReason::HistoryNotReady,
               "pass classifyResolveSkip matches tryPreflight before init");

void testJitterSyncPreflight() {
    expectTrue(!jitter.isSyncedToFrameIndex(5u), "default jitter is not synced to frame 5");

    jitter.syncToFrameIndex(5u);
    expectTrue(jitter.isSyncedToFrameIndex(5u), "syncToFrameIndex marks jitter as synced");
    expectTrue(!jitter.isSyncedToFrameIndex(6u), "synced jitter does not match other frames");

    const fuse::renderer::TaaJitterSyncPreflight synced =
        fuse::renderer::preflightTaaJitterSync(jitter, 5u, 128u, 128u);
    expectTrue(synced.sequence_valid, "sync preflight sequence valid");
    expectTrue(synced.viewport_valid, "sync preflight viewport valid");
    expectTrue(synced.can_produce_ndc, "sync preflight can produce NDC");
    expectTrue(synced.monotonic_matches, "sync preflight monotonic counter matches");
    expectTrue(synced.slot_matches, "sync preflight slot matches expected");
    expectTrue(synced.synced(), "sync preflight reports synced state");

    jitter.advance();
    const fuse::renderer::TaaJitterSyncPreflight drifted =
    expectTrue(!drifted.synced(), "advanced jitter no longer synced to prior frame");
    expectTrue(!drifted.monotonic_matches, "drifted preflight reports monotonic mismatch");

    const fuse::renderer::TaaJitterSyncPreflight invalidViewport =
        fuse::renderer::preflightTaaJitterSync(jitter, 6u, 0u, 128u);
    expectTrue(!invalidViewport.viewport_valid, "zero width fails viewport guard");
    expectTrue(!invalidViewport.can_produce_ndc, "zero width blocks NDC production");

void testResolveBlendPreflight() {
void testJitterSyncGuards() {

    expectTrue(TaaJitterLayout::monotonicFrameMatchesSlot(13u, 5u, 8u),
               "monotonic frame 13 maps to slot 5");
    expectTrue(!TaaJitterLayout::monotonicFrameMatchesSlot(13u, 6u, 8u),
               "wrong slot fails monotonicFrameMatchesSlot");

    expectTrue(fuse::renderer::classifyTaaJitterSync(13u, 5u, 13u, 8u) ==
                   fuse::renderer::TaaJitterSyncStatus::Synced,
               "synced jitter classification");
    expectTrue(fuse::renderer::classifyTaaJitterSync(13u, 5u, 14u, 8u) ==
                   fuse::renderer::TaaJitterSyncStatus::MonotonicMismatch,
               "monotonic mismatch classification");
    expectTrue(fuse::renderer::classifyTaaJitterSync(13u, 6u, 13u, 8u) ==
                   fuse::renderer::TaaJitterSyncStatus::SlotMismatch,
               "slot mismatch classification");

    jitter.syncToFrameIndex(13u);
    expectTrue(jitter.isSyncedToFrameIndex(13u), "jitter reports synced after syncToFrameIndex");
    expectTrue(!jitter.isSyncedToFrameIndex(14u), "jitter reports unsynced for different frame");
    expectTrue(jitter.syncStatusForFrameIndex(13u) == fuse::renderer::TaaJitterSyncStatus::Synced,
               "syncStatusForFrameIndex reports Synced");

    expectTrue(jitter.isSyncedToFrameIndex(14u), "advance leaves jitter synced to next frame");
    expectTrue(jitter.syncStatusForFrameIndex(13u) == fuse::renderer::TaaJitterSyncStatus::MonotonicMismatch,
               "prior frame reports monotonic mismatch after advance");

    pass->syncJitterToFrameIndex(7u);
    expectTrue(pass->isJitterSyncedToFrameIndex(7u), "pass jitter synced after syncJitterToFrameIndex");
    expectTrue(pass->jitterMonotonicFrameIndex() == 7u, "pass exposes monotonic jitter frame counter");
    expectTrue(!pass->isJitterSyncedToFrameIndex(8u), "pass jitter unsynced for different frame");

void testResolveBlendPreflightGuards() {

    const fuse::renderer::TaaResolveBlendPreflight emptyPreflight =
        fuse::renderer::preflightTaaResolveBlend(desc, emptyHistory);
    expectTrue(!fuse::renderer::taaResolveBlendPreflightValid(emptyPreflight),
               "preflight invalid for empty history");
    expectTrue(!fuse::renderer::taaResolveWouldBlendHistory(desc, emptyHistory),
               "wouldBlendHistory false for empty history");
    expectTrue(!fuse::renderer::taaResolveBlendConsistentWithHistory(desc, emptyHistory),
               "blend consistency false for empty history");
    expectNear(pass->historyWarmupProgress(), 0.f, 1e-5f,
               "pass warmup progress is zero before init");
    expectTrue(!pass->historyWarmupComplete(), "pass warmup not complete before init");
    expectTrue(pass->classifyJitterSyncBlock() == fuse::renderer::TaaJitterSyncBlockReason::None,
               "pass jitter sync block is None before init");

    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass try/classify wrapper test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for resolve blend preflight test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass composite guard test");


    expectTrue(pass->init(resources), "TaaPass initialized for try/classify wrapper test");

    expectTrue(pass->tryPreflightHistoryReadyForResolve(reuseReason),
               "pass tryPreflightHistoryReadyForResolve passes after init");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::None,
               "pass tryPreflightHistoryReadyForResolve reason is None after init");
               "pass tryPreflightHistoryReuse fails before warmup");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::NotWarm,
               "pass tryPreflightHistoryReuse reason is NotWarm before warmup");
    expectTrue(pass->classifyHistoryReuseBlock(0u) == fuse::renderer::TaaHistoryReuseBlockReason::NotWarm,
               "pass classifyHistoryReuseBlock matches tryPreflight before warmup");

    expectTrue(history.init(resources, historyDesc), "history ready for blend preflight test");

    fuse::renderer::TAAParams params{};
    params.blend_factor = 0.3f;

    const fuse::renderer::TaaResolveBlendPreflight warmup =
        fuse::renderer::preflightTaaResolveBlend(true, params, history);
    expectTrue(warmup.first_frame, "warmup blend preflight marks first frame");
    expectTrue(!warmup.history_blend_allowed, "warmup blend preflight blocks history blend");
    expectTrue(!warmup.history_reuse_allowed, "warmup blend preflight blocks history reuse");
    expectTrue(warmup.weights_valid, "warmup blend weights are valid");
    expectTrue(warmup.readyForBlend(), "warmup blend preflight is ready for blend");
    expectNear(warmup.weights.current, 1.f, 1e-5f, "warmup blend preflight uses full current weight");

    const fuse::renderer::TaaResolveBlendPreflight steady =
        fuse::renderer::preflightTaaResolveBlend(false, params, history);
    expectTrue(!steady.first_frame, "steady blend preflight is not first frame");
    expectTrue(steady.history_blend_allowed, "steady blend preflight allows history blend");
    expectTrue(steady.history_reuse_allowed, "steady blend preflight allows history reuse");
    expectNear(steady.weights.current, 0.3f, 1e-5f, "steady blend preflight uses configured current weight");

    desc.params = params;
    const fuse::renderer::TaaResolveBlendPreflight descPreflight =
        fuse::renderer::preflightTaaResolveBlendForDesc(desc, history);
    expectTrue(!descPreflight.history_reuse_allowed, "desc blend preflight blocks stale generation reuse");

    desc.observed_history_generation = history.invalidateGeneration();
    const fuse::renderer::TaaResolveBlendPreflight currentDescPreflight =
    expectTrue(currentDescPreflight.history_reuse_allowed,
               "desc blend preflight allows reuse with current generation");


    expectTrue(history.init(resources, historyDesc), "history ready for resolve blend preflight test");


    const fuse::renderer::TaaResolveBlendPreflight warmupPreflight =
        fuse::renderer::preflightTaaResolveBlend(desc, history);
    expectTrue(fuse::renderer::taaResolveBlendPreflightValid(warmupPreflight),
               "warmup preflight is valid");
    expectTrue(warmupPreflight.first_frame, "warmup preflight marks first frame");
    expectTrue(!warmupPreflight.history_reuse, "warmup preflight blocks history reuse");
    expectNear(warmupPreflight.weights.current, 1.f, 1e-5f, "warmup preflight uses full current weight");
    expectTrue(fuse::renderer::taaResolveBlendConsistentWithHistory(desc, history),
               "warmup blend consistent with unwarmed history");
    expectTrue(!fuse::renderer::taaResolveWouldBlendHistory(desc, history),
               "wouldBlendHistory false on warmup frame");

    const fuse::renderer::TaaResolveBlendPreflight steadyPreflight =
    expectTrue(steadyPreflight.history_reuse, "steady preflight allows history reuse");
    expectTrue(!steadyPreflight.first_frame, "steady preflight is not first frame");
    expectTrue(fuse::renderer::taaResolveWouldBlendHistory(desc, history),
               "wouldBlendHistory true after warmup");
               "steady blend consistent with warmed history");

    fuse::renderer::TaaResolveTemporalPreflight preflight{};
    expectTrue(fuse::renderer::preflightTaaResolveTemporalAccumulation(desc, history, &preflight),
               "temporal preflight passes on warmup frame with valid surfaces");
    expectTrue(preflight.skip_reason == fuse::renderer::TaaResolveSkipReason::None,
               "warmup temporal preflight skip reason is None");
    expectTrue(preflight.reuse_block == fuse::renderer::TaaHistoryReuseBlockReason::NotWarm,
               "warmup temporal preflight reuse block is NotWarm");
    expectTrue(preflight.blend_reject == fuse::renderer::TaaResolveBlendRejectReason::None,
               "warmup temporal preflight blend reject is None");

               "temporal preflight passes after warmup");
    expectTrue(preflight.reuse_block == fuse::renderer::TaaHistoryReuseBlockReason::None,
               "steady temporal preflight reuse block is None");

    expectTrue(!fuse::renderer::preflightTaaResolveTemporalAccumulation(desc, history, &preflight),
               "temporal preflight fails on invalid dimensions");
    expectTrue(preflight.skip_reason == fuse::renderer::TaaResolveSkipReason::InvalidDimensions,
               "invalid dimensions temporal preflight skip reason");

    expectTrue(fuse::renderer::tryPreflightTaaResolveBlendWeights(
                   fuse::renderer::TaaResolveDesc{.width = 64, .height = 64}, history, blendReason),
               "tryPreflightTaaResolveBlendWeights succeeds for steady history");

    passDesc.width = 64;
    passDesc.height = 64;
    expectTrue(pass->init(resources), "TaaPass initialized for blend preflight test");

    fuse::renderer::TaaResolveDesc resolveDesc{};
    resolveDesc.width = 64;
    resolveDesc.height = 64;
    resolveDesc.surfaces.current_frame = reinterpret_cast<void*>(0x10);
    resolveDesc.surfaces.output = reinterpret_cast<void*>(0x20);
    expectTrue(pass->resolveFrame(resolveDesc), "pass resolve warms pass history for preflight");

    const fuse::renderer::TaaResolveBlendPreflight passPreflight = pass->preflightResolveBlend(desc);
    expectTrue(fuse::renderer::taaResolveBlendPreflightValid(passPreflight),
               "pass preflightResolveBlend is valid for warmed history");
    expectTrue(passPreflight.history_reuse, "pass preflight allows history reuse");
    expectTrue(pass->init(resources), "TaaPass initialized for temporal preflight test");
    fuse::renderer::TaaHistoryWarmupBlockReason warmupReason = fuse::renderer::TaaHistoryWarmupBlockReason::None;
    expectTrue(!pass->preflightHistoryWarmup(&warmupReason),
               "pass warmup preflight fails before first resolve");
    expectTrue(warmupReason == fuse::renderer::TaaHistoryWarmupBlockReason::NeedsWarmup,
               "pass warmup preflight reason is NeedsWarmup before first resolve");
    expectTrue(pass->historyWarmupFramesRemaining() == 1u, "pass reports one warmup frame remaining");
    expectTrue(pass->resolveFrame(desc), "initial resolve warms pass history");
    expectTrue(pass->preflightHistoryWarmup(), "pass warmup preflight passes after first resolve");
    expectTrue(pass->preflightResolveTemporalAccumulation(desc, &preflight),
               "pass temporal preflight passes after warmup");

    pass->destroy();

    fuse::renderer::TaaResolveFramePreflight preflight{};
    expectTrue(fuse::renderer::preflightTaaResolveFrame(desc, history, &preflight),
               "valid resolve frame passes combined preflight");
    expectTrue(preflight.canProceed, "resolve frame preflight canProceed is true");
    expectTrue(preflight.blend_reason == fuse::renderer::TaaResolveBlendRejectReason::None,

    fuse::renderer::TaaBlendWeights weights{};
    expectTrue(fuse::renderer::tryComputeTaaResolveBlendWeights(desc, history, weights, &blendReason),
               "tryComputeTaaResolveBlendWeights succeeds for warmup frame");
    expectNear(weights.current, 1.f, 1e-5f, "tryCompute blend uses full current on warmup");
    expectNear(weights.history, 0.f, 1e-5f, "tryCompute blend uses zero history on warmup");

    expectTrue(!fuse::renderer::preflightTaaResolveFrame(desc, history, &preflight),
               "invalid dimensions fail combined preflight");
    expectTrue(!preflight.canProceed, "invalid dimensions preflight canProceed is false");
               "invalid dimensions preflight skip reason is InvalidDimensions");

               "tryComputeTaaResolveBlendWeights succeeds after warmup");
    expectNear(weights.current, 0.2f, 1e-5f, "tryCompute blend uses configured current after warmup");
    expectNear(weights.history, 0.8f, 1e-5f, "tryCompute blend uses history complement after warmup");
    expectTrue(history.init(resources, historyDesc), "history ready for sample guard test");

    expectTrue(!fuse::renderer::canSampleHistoryForResolve(desc, history),
               "unwarmed history cannot be sampled for resolve");
    expectTrue(!fuse::renderer::tryPreflightTaaHistoryReuseForResolve(desc, history, reuseReason),
               "resolve reuse preflight fails before warmup");
               "resolve reuse preflight reason is NotWarm before warmup");

               "warmed history reports warmup complete");
    expectTrue(fuse::renderer::canSampleHistoryForResolve(desc, history),
               "warmed history can be sampled for resolve");
    expectTrue(fuse::renderer::tryPreflightTaaHistoryReuseForResolve(desc, history, reuseReason),
               "resolve reuse preflight passes after warmup");
    expectTrue(!fuse::renderer::shouldSkipTaaHistoryReuse(history, 0u),
               "warmed history does not skip reuse");

    expectTrue(fuse::renderer::wouldInvalidateHistoryIfStale(history, 0u),
               "stale observed generation would trigger invalidate-if-stale");
               "resolve reuse preflight fails after invalidate");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::StaleGeneration,
               "resolve reuse preflight reason is StaleGeneration after invalidate");


void testTaaPassPreflightGuards() {

    pass->syncJitterToFrameIndex(4u);
    expectTrue(pass->isJitterSyncedToFrameIndex(4u), "pass reports synced jitter after sync");

    const fuse::renderer::TaaJitterSyncPreflight jitterPreflight = pass->preflightJitterSync(4u);
    expectTrue(jitterPreflight.synced(), "pass jitter preflight reports synced state");
void testTaaPassDeepenGuardWrappers() {
    passDesc.params.blend_factor = 0.35f;

    expectTrue(pass->needsJitterResync(4u), "pass needs jitter resync before sync");

void testResolveBlendApplyGuards() {

void testTaaPassWarmupAndJitterReadyGuards() {

    expectTrue(!pass->historyWarmupComplete(), "pass history warmup not complete before init");
    expectTrue(pass->jitterSyncReady(7u), "pass jitter sync ready before init");
    expectTrue(!pass->shouldSkipJitterSync(7u), "pass should not skip jitter sync before init");
    expectTrue(pass->jitterNdcReady(), "pass jitter NDC ready before init");
    expectTrue(!pass->shouldSkipJitterNdc(), "pass should not skip jitter NDC before init");

    expectTrue(pass->preflightJitterNdc(&rejectReason), "pass preflightJitterNdc passes before init");
               "pass preflightJitterNdc reject reason is None before init");

void testTaaPassWarmupAndJitterSkipGuards() {

    expectTrue(pass->shouldSkipHistoryWarmup(), "pass should skip warmup before init");

    fuse::renderer::TaaHistoryWarmupRejectReason warmupReason =
               "pass warmup preflight fails before init");
    expectTrue(warmupReason == fuse::renderer::TaaHistoryWarmupRejectReason::NotReady,
               "pass warmup preflight reason is NotReady before init");

    expectTrue(!pass->shouldSkipJitterSync(4u), "pass should not skip jitter sync before init");
    expectTrue(pass->preflightJitterSync(4u), "pass jitter sync preflight passes before init");
    expectTrue(!pass->shouldSkipJitterNdc(), "pass should not skip NDC jitter before init");
    expectTrue(pass->preflightJitterNdc(), "pass NDC jitter preflight passes before init");

void testTaaPassCombinedPreflightGuards() {
    passDesc.params.blend_factor = 0.25f;

    expectTrue(!pass->shouldSkipJitterNdc(), "pass NDC jitter preflight passes before init");
    expectTrue(!pass->shouldSkipJitterSync(3u), "pass jitter sync preflight passes before init");

    expectTrue(pass->tryPreflightJitterNdc(jitterReject), "pass tryPreflightJitterNdc passes");
               "pass tryPreflightJitterNdc reject reason is None");
    expectTrue(pass->tryPreflightJitterSync(3u, jitterReject), "pass tryPreflightJitterSync passes");
    expectTrue(pass->preflightJitterNdc(&jitterReject), "pass preflightJitterNdc passes");
                               fuse::renderer::TaaHistoryWarmupRejectReason::NeedsWarmup),
               "NeedsWarmup reject label");

               "empty history warmup classified as NotReady");
               "empty history warmup not complete");
               "should skip warmup check for empty history");
    expectTrue(!fuse::renderer::tryPreflightTaaHistoryWarmup(emptyHistory, warmupReason),
               "tryPreflightTaaHistoryWarmup fails for empty history");
               "empty history warmup reject reason is NotReady");
    expectTrue(!fuse::renderer::taaHistoryReuseBlockedByWarmup(emptyHistory),
               "empty history not blocked by warmup specifically");
               "empty history should skip resolve");
               "tryPreflightTaaResolve fails for empty history");
               "empty history resolve skip reason is HistoryNotReady");

    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for blend apply guard test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass warmup/jitter-ready test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass warmup/jitter skip test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass combined preflight test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for warmup preflight test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for resolve try/should-skip test");
    TAA_SKIP_UNLESS_VULKAN_READY(bootstrap);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for resolve should-skip test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaHistoryBuffer history;
    fuse::renderer::TaaHistoryBufferDesc historyDesc{64, 64};
    expectTrue(history.init(resources, historyDesc), "history ready for blend apply guard test");


    expectTrue(fuse::renderer::shouldSkipHistoryBlendAtResolve(desc, history),
               "warmup frame skips history blend");
    expectTrue(!fuse::renderer::canApplyHistoryBlendAtResolve(desc, history),
               "warmup frame cannot apply history blend");

    fuse::renderer::TaaResolveBlendRejectReason rejectReason =
    expectTrue(fuse::renderer::tryPreflightTaaResolveBlendWeights(desc, history, rejectReason),
               "tryPreflight blend passes on warmup frame");
    expectTrue(rejectReason == fuse::renderer::TaaResolveBlendRejectReason::None,
               "warmup tryPreflight reject reason is None");

    expectTrue(!fuse::renderer::shouldSkipHistoryBlendAtResolve(desc, history),
               "steady frame does not skip history blend");
    expectTrue(fuse::renderer::canApplyHistoryBlendAtResolve(desc, history),
               "steady frame can apply history blend");
               "tryPreflight blend passes after warmup");

               "stale generation skips history blend");
               "stale generation cannot apply history blend");


    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass preflight guard test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass deepen wrapper test");


    expectTrue(pass->init(resources), "TaaPass initialized for preflight guard test");

    const fuse::renderer::TaaHistoryWarmupPreflight warmupPreflight = pass->preflightHistoryWarmup();
    expectTrue(warmupPreflight.readyForResolve(), "pass warmup preflight ready for resolve");
    expectTrue(warmupPreflight.needs_warmup, "pass warmup preflight needs warmup before resolve");
    expectTrue(pass->init(resources), "TaaPass initialized for composite guard test");
    expectTrue(!pass->historyWarmupComplete(), "pass warmup not complete before first resolve");
    expectTrue(!pass->historyReuseReady(0u), "pass history not reuse-ready before first resolve");

    fuse::renderer::TaaJitterSyncBlockReason syncReason = fuse::renderer::TaaJitterSyncBlockReason::None;
    expectTrue(pass->preflightJitterSync(4u, &syncReason), "pass jitter sync preflight passes");
    expectTrue(syncReason == fuse::renderer::TaaJitterSyncBlockReason::None,
               "pass jitter sync preflight reason is None");
    expectTrue(pass->init(resources), "TaaPass initialized for deepen wrapper test");

    expectTrue(!pass->historyWarmupComplete(), "pass history warmup incomplete before resolve");
    expectTrue(!pass->tryCanBeginTemporalReuse(0u, &reuseReason),
               "pass tryCanBeginTemporalReuse fails before warmup");
    expectTrue(pass->init(resources), "TaaPass initialized for warmup/jitter-ready test");
    expectTrue(!pass->historyWarmupComplete(), "pass history warmup not complete after init");
    expectTrue(pass->init(resources), "TaaPass initialized for warmup/jitter skip test");
    expectTrue(!pass->historyWarmupComplete(), "pass warmup not complete after init");
    expectTrue(!pass->shouldSkipHistoryWarmup(), "pass should not skip warmup after init");

void testTaaPassWarmupAndJitterShouldSkipWrappers() {
    passDesc.params.blend_factor = 0.2f;

    expectTrue(pass->shouldSkipHistoryWarmup(), "pass should skip warmup-complete path before init");
    expectTrue(pass->classifyHistoryWarmupBlock() ==
                   fuse::renderer::TaaHistoryWarmupBlockReason::NotReady,
               "pass classifies unwarmed pre-init history as NotReady");
    expectTrue(!pass->shouldSkipJitterSync(4u), "pass jitter sync valid before init");
    expectTrue(!pass->shouldSkipJitterNdc(), "pass NDC jitter valid before init");

    expectTrue(pass->tryPreflightJitterSync(4u, jitterReject), "pass tryPreflightJitterSync passes");

    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass should-skip wrapper test");


    expectTrue(pass->init(resources), "TaaPass initialized for should-skip wrapper test");

    expectTrue(pass->shouldSkipHistoryWarmup(), "pass should skip warmup-complete path after init");
                   fuse::renderer::TaaHistoryWarmupBlockReason::NeedsWarmup,
               "pass classifies unwarmed history as NeedsWarmup after init");

    fuse::renderer::TaaHistoryWarmupBlockReason warmupReason =
        fuse::renderer::TaaHistoryWarmupBlockReason::None;
    expectTrue(!pass->tryPreflightHistoryWarmup(warmupReason),
               "pass tryPreflightHistoryWarmup fails before resolve");
               "pass tryPreflightHistoryWarmup reason is NeedsWarmup");
    expectTrue(pass->init(resources), "TaaPass initialized for combined preflight test");
    expectTrue(history.init(resources, historyDesc), "history ready for diagnostic guard overload test");
    expectTrue(history.needsWarmup(), "history needs warmup before first resolve");
    expectTrue(!history.warmupComplete(), "history warmupComplete false before first resolve");
    expectTrue(fuse::renderer::shouldSkipTaaHistoryWarmup(history),
               "shouldSkipTaaHistoryWarmup true before first resolve");

    expectTrue(history.warmupComplete(), "history warmupComplete true after resolve");
               "taaHistoryWarmupComplete true after resolve");
    expectTrue(!fuse::renderer::shouldSkipTaaHistoryWarmup(history),
               "shouldSkipTaaHistoryWarmup false after resolve");
    expectTrue(!fuse::renderer::wouldSkipTaaHistoryReuse(history, 0u),
               "wouldSkip alias passes for warmed history");
    expectTrue(fuse::renderer::taaHistoryReuseReady(history, 0u),
               "reuse-ready after warmup");

    expectTrue(!fuse::renderer::wouldSkipTaaResolveBlend(desc, history),
               "wouldSkip alias passes for steady blend");
    expectTrue(fuse::renderer::wouldSkipTaaResolveBlend(desc, history) ==
                   fuse::renderer::shouldSkipTaaResolveBlend(desc, history),
               "wouldSkip and shouldSkip agree for resolve blend");

    expectTrue(fuse::renderer::taaJitterSyncReady(9u, 8u),
               "taaJitterSyncReady true for valid sequence");
    expectTrue(!fuse::renderer::shouldSkipTaaJitterSync(9u, 8u),
               "shouldSkipTaaJitterSync false for valid sequence");
    expectTrue(fuse::renderer::shouldSkipTaaJitterSync(9u, 0u),
               "shouldSkipTaaJitterSync true for invalid sequence");

    fuse::renderer::TaaJitterGuardRejectReason jitterReason =
    expectTrue(fuse::renderer::tryPreflightTaaJitterNdc(128u, 128u, 8u, jitterReason),
               "tryPreflightTaaJitterNdc passes for valid viewport");
    expectTrue(jitterReason == fuse::renderer::TaaJitterGuardRejectReason::None,
               "tryPreflightTaaJitterNdc reject reason is None");
    expectTrue(fuse::renderer::taaJitterNdcReady(128u, 128u, 8u),
               "taaJitterNdcReady true for valid viewport");
    expectTrue(!fuse::renderer::shouldSkipTaaJitterNdc(128u, 128u, 8u),
               "shouldSkipTaaJitterNdc false for valid viewport");
    expectTrue(fuse::renderer::shouldSkipTaaJitterNdc(0u, 128u, 8u),
               "shouldSkipTaaJitterNdc true for zero width");
    expectTrue(!fuse::renderer::tryPreflightTaaJitterNdc(0u, 128u, 8u, jitterReason),
               "tryPreflightTaaJitterNdc rejects zero width");
    expectTrue(jitterReason == fuse::renderer::TaaJitterGuardRejectReason::InvalidViewport,
               "tryPreflightTaaJitterNdc reject reason is InvalidViewport");

    expectTrue(pass->preflightJitterNdc(), "pass preflightJitterNdc passes before init");

    expectTrue(pass->init(resources), "TaaPass initialized for diagnostic guard overload test");
    expectTrue(pass->shouldSkipHistoryWarmup(), "pass should skip warmup after init");
    expectTrue(pass->wouldSkipHistoryReuse(0u), "pass wouldSkipHistoryReuse before warmup");
    expectTrue(history.init(resources, historyDesc), "history ready for warmup preflight test");
    expectTrue(fuse::renderer::taaHistoryReuseBlockedByWarmup(history),
               "allocated unwarmed history blocked by warmup for reuse");
               "should skip reuse while history needs warmup");
    expectTrue(!fuse::renderer::tryPreflightTaaHistoryWarmup(history, warmupReason),
               "tryPreflightTaaHistoryWarmup fails before warmup");
    expectTrue(warmupReason == fuse::renderer::TaaHistoryWarmupRejectReason::NeedsWarmup,
               "unwarmed history warmup reject reason is NeedsWarmup");

    history.markResolved();
    expectTrue(fuse::renderer::tryPreflightTaaHistoryWarmup(history, warmupReason),
               "tryPreflightTaaHistoryWarmup passes after warmup");
    expectTrue(warmupReason == fuse::renderer::TaaHistoryWarmupRejectReason::None,
               "warmed history warmup reject reason is None");
    expectTrue(!fuse::renderer::taaHistoryReuseBlockedByWarmup(history),
               "warmed history not blocked by warmup");

               "composite preflight passes for valid unwarmed resolve");
               "composite preflight skip reason is None");
               "composite preflight blend reason is None");
    expectTrue(fuse::renderer::tryPreflightTaaResolveFrame(desc, history, skipReason, blendReason),
               "tryPreflightTaaResolveFrame passes for valid unwarmed resolve");

               "composite preflight rejects invalid dimensions");
               "composite preflight reports InvalidDimensions");
    expectTrue(history.init(resources, historyDesc), "history ready for resolve try/should-skip test");
               "valid resolve skip reason is None");

               "tryPreflightTaaResolve fails for invalid dimensions");
               "invalid dimensions resolve skip reason is InvalidDimensions");

    expectTrue(fuse::renderer::preflightTaaResolveFrameGuards(desc, history, &skipReason, &blendReason),
               "resolve frame guards pass for valid warmup resolve");
               "resolve frame guards skip reason is None");
               "resolve frame guards blend reason is None");
    expectTrue(!fuse::renderer::shouldSkipTaaResolveFrameGuards(desc, history),
               "resolve frame guards should not skip valid warmup resolve");

    expectTrue(fuse::renderer::shouldSkipTaaResolveFrameGuards(desc, history),
               "resolve frame guards skip invalid dimensions");
    expectTrue(fuse::renderer::tryPreflightTaaResolveFrameGuards(desc, history, skipReason, blendReason) == false,
               "tryPreflightTaaResolveFrameGuards fails for invalid dimensions");
               "tryPreflight resolve frame guards skip reason is InvalidDimensions");

    history.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testJitterNdcShouldSkipAndTryGuards() {
    using fuse::renderer::TaaJitterLayout;

    expectTrue(!fuse::renderer::shouldSkipTaaJitterSync(8u), "default sequence sync should not be skipped");
    expectTrue(fuse::renderer::shouldSkipTaaJitterSync(0u), "invalid sequence sync should be skipped");
    expectTrue(!fuse::renderer::shouldSkipTaaJitterNdc(1920u, 1080u, 8u),
               "valid viewport NDC should not be skipped");
    expectTrue(fuse::renderer::shouldSkipTaaJitterNdc(0u, 1080u, 8u),
               "zero-width NDC should be skipped");

    fuse::renderer::TaaJitterGuardRejectReason rejectReason =
        fuse::renderer::TaaJitterGuardRejectReason::None;
    expectTrue(fuse::renderer::tryPreflightTaaJitterNdc(128u, 128u, 8u, rejectReason),
    expectTrue(rejectReason == fuse::renderer::TaaJitterGuardRejectReason::None,
               "valid viewport NDC reject reason is None");
    expectTrue(!fuse::renderer::tryPreflightTaaJitterNdc(0u, 128u, 8u, rejectReason),
               "tryPreflightTaaJitterNdc fails for zero width");
    expectTrue(rejectReason == fuse::renderer::TaaJitterGuardRejectReason::InvalidViewport,
               "zero width NDC reject reason is InvalidViewport");

    fuse::math::Vec2 ndcOut{};
    expectTrue(TaaJitterLayout::tryNdcOffsetForFrameIndex(3u, 128u, 128u, 8u, ndcOut, rejectReason),
               "tryNdcOffsetForFrameIndex passes for valid inputs");
    const fuse::math::Vec2 expected =
        TaaJitterLayout::ndcOffsetForFrameIndex(3u, 128u, 128u, 8u);
    expectNear(ndcOut.x, expected.x, 1e-6f, "tryNdcOffsetForFrameIndex X matches ndcOffsetForFrameIndex");
    expectNear(ndcOut.y, expected.y, 1e-6f, "tryNdcOffsetForFrameIndex Y matches ndcOffsetForFrameIndex");
    expectTrue(!TaaJitterLayout::tryNdcOffsetForFrameIndex(3u, 0u, 128u, 8u, ndcOut, rejectReason),
               "tryNdcOffsetForFrameIndex fails for zero width");

    fuse::renderer::TaaJitter jitter;
    expectTrue(jitter.tryCurrentNdcOffsetIfReady(128u, 128u, ndcOut, rejectReason),
               "tryCurrentNdcOffsetIfReady passes for valid viewport");
    const fuse::math::Vec2 direct = jitter.currentNdcOffset(128u, 128u);
    expectNear(ndcOut.x, direct.x, 1e-6f, "tryCurrentNdcOffsetIfReady X matches currentNdcOffset");
    expectTrue(!jitter.tryCurrentNdcOffsetIfReady(0u, 128u, ndcOut, rejectReason),
               "tryCurrentNdcOffsetIfReady fails for zero width");

    expectTrue(jitter.trySyncToFrameIndexIfReady(11u, rejectReason),
               "trySyncToFrameIndexIfReady passes for valid sequence");
    expectTrue(jitter.isAlignedToFrameIndex(11u), "jitter aligned after trySyncToFrameIndexIfReady");

void testResolveBlendWeightsIfReadyGuards() {
void testResolveReuseAndBlendPreflights() {
    expectTrue(std::strcmp(fuse::renderer::taaResolveReuseBlendRejectReasonLabel(
                               fuse::renderer::TaaResolveReuseBlendRejectReason::None),
                           "none") == 0,
               "None reuse-blend reject label");
                               fuse::renderer::TaaResolveReuseBlendRejectReason::HistoryReuseBlocked),
                           "history_reuse_blocked") == 0,
               "HistoryReuseBlocked reuse-blend reject label");
                               fuse::renderer::TaaResolveReuseBlendRejectReason::BlendWeightsRejected),
                           "blend_weights_rejected") == 0,
               "BlendWeightsRejected reuse-blend reject label");

    fuse::renderer::TaaHistoryBuffer emptyHistory;
    fuse::renderer::TaaResolveDesc desc{};
    fuse::renderer::TaaResolveReuseBlendRejectReason rejectReason =
        fuse::renderer::TaaResolveReuseBlendRejectReason::None;
    expectTrue(fuse::renderer::shouldSkipTaaResolveReuseAndBlend(desc, emptyHistory, 0u),
               "empty history should skip reuse-and-blend");
    expectTrue(fuse::renderer::classifyTaaResolveReuseBlendReject(desc, emptyHistory, 0u) ==
                   fuse::renderer::TaaResolveReuseBlendRejectReason::HistoryReuseBlocked,
               "empty history reuse-blend classify is HistoryReuseBlocked");

    expectTrue(bootstrap != nullptr, "bootstrap allocated for blend weights-if-ready test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for reuse-blend preflight test");



    expectTrue(history.init(resources, historyDesc), "history ready for blend weights-if-ready test");


    expectTrue(fuse::renderer::taaResolveBlendWeightsReady(desc, history),
               "warmup blend weights reported ready");
    expectTrue(fuse::renderer::computeTaaResolveBlendWeightsIfReady(desc, history, weights, &rejectReason),
               "computeTaaResolveBlendWeightsIfReady passes for warmup");
    expectNear(weights.current, 1.f, 1e-5f, "computeIfReady warmup current weight is full");
    expectNear(weights.history, 0.f, 1e-5f, "computeIfReady warmup history weight is zero");

               "computeTaaResolveBlendWeightsIfReady passes after warmup");
    expectNear(weights.current, 0.4f, 1e-5f, "computeIfReady steady current weight");
    expectNear(weights.history, 0.6f, 1e-5f, "computeIfReady steady history weight");

void testHistoryWarmupBlockPreflight() {
    expectTrue(std::strcmp(fuse::renderer::taaHistoryWarmupBlockReasonLabel(
                               fuse::renderer::TaaHistoryWarmupBlockReason::None),
               "None warmup block label");
                               fuse::renderer::TaaHistoryWarmupBlockReason::NeedsWarmup),
                           "needs_warmup") == 0,
               "NeedsWarmup block label");

    fuse::renderer::TaaHistoryWarmupBlockReason reason = fuse::renderer::TaaHistoryWarmupBlockReason::None;
    expectTrue(fuse::renderer::classifyTaaHistoryWarmupBlock(emptyHistory) ==
                   fuse::renderer::TaaHistoryWarmupBlockReason::NotReady,
               "empty history warmup block is NotReady");
    expectTrue(!fuse::renderer::taaHistoryWarmupComplete(emptyHistory),
               "empty history warmup is not complete");
    expectTrue(!fuse::renderer::preflightTaaHistoryWarmup(emptyHistory, &reason),
               "empty history fails warmup preflight");
    expectTrue(reason == fuse::renderer::TaaHistoryWarmupBlockReason::NotReady,
               "empty history warmup preflight reason is NotReady");
    expectTrue(!fuse::renderer::tryPreflightTaaHistoryWarmup(emptyHistory, reason),
               "tryPreflightTaaHistoryWarmup fails for empty history");
void testHistoryWarmupClassifyAndTryPreflight() {
                               fuse::renderer::TaaHistoryWarmupBlockReason::NotReady),
                           "not_ready") == 0,
               "NotReady warmup block label");
               "NeedsWarmup warmup block label");

               "empty history warmup classified as NotReady");

    fuse::renderer::TaaHistoryWarmupBlockReason warmupReason =
        fuse::renderer::TaaHistoryWarmupBlockReason::None;
    expectTrue(!fuse::renderer::preflightTaaHistoryWarmup(emptyHistory, &warmupReason),
    expectTrue(warmupReason == fuse::renderer::TaaHistoryWarmupBlockReason::NotReady,
    expectTrue(!fuse::renderer::tryPreflightTaaHistoryWarmup(emptyHistory, warmupReason),
               "tryPreflight warmup reason is NotReady for empty history");
void testHistoryWarmupPreflight() {
    fuse::renderer::TaaHistoryReuseBlockReason reason = fuse::renderer::TaaHistoryReuseBlockReason::None;
    expectTrue(reason == fuse::renderer::TaaHistoryReuseBlockReason::NotReady,
                   fuse::renderer::TaaHistoryReuseBlockReason::NotReady,
               "empty history warmup classify is NotReady");
void testHistoryWarmupPreflightDeepen() {
    expectTrue(!fuse::renderer::taaHistoryWarmupReady(emptyHistory),
               "empty history is not warmup-ready");
    expectTrue(fuse::renderer::shouldSkipTaaHistoryWarmup(emptyHistory),
               "empty history should skip warmup");
    expectTrue(!fuse::renderer::taaHistoryResolveReady(emptyHistory),
               "empty history is not resolve-ready");
    expectTrue(fuse::renderer::shouldSkipTaaHistoryResolve(emptyHistory),
               "empty history should skip resolve");

               "empty history warmup preflight fails");
               "tryPreflightTaaHistoryWarmup reason is NotReady for empty history");

    expectTrue(!fuse::renderer::preflightTaaHistoryReadyForResolve(emptyHistory, &reason),
               "empty history resolve preflight fails");
               "empty history resolve preflight reason is NotReady");
    expectTrue(!fuse::renderer::tryPreflightTaaHistoryReadyForResolve(emptyHistory, reason),
               "tryPreflightTaaHistoryReadyForResolve fails for empty history");
void testTaaPassTryPreflightWrappers() {
void testTaaPassTryAndClassifyGuards() {
void testTaaPassTryPreflightAndClassifyWrappers() {
void testTaaPassTryPreflightGuardWrappers() {
void testTaaPassTryClassifyWrappers() {
void testTaaPassTryAndClassifyWrappers() {
    fuse::renderer::TaaPassDesc passDesc{};
    passDesc.width = 128;
    passDesc.height = 128;
    passDesc.params.blend_factor = 0.25f;

    auto pass = fuse::renderer::TaaPass::create(passDesc);
    expectTrue(pass->classifyJitterSyncReject() == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass classifyJitterSyncReject is None before init");
    expectTrue(pass->classifyJitterNdcReject() == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass classifyJitterNdcReject is None before init");
    expectTrue(pass->classifyJitterAdvanceReject() == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass classifyJitterAdvanceReject is None before init");
    expectTrue(!pass->shouldSkipJitterAdvance(), "pass should not skip jitter advance before init");

    fuse::renderer::TaaJitterGuardRejectReason jitterReject =
    expectTrue(pass->tryPreflightJitterSync(6u, jitterReject),
               "pass tryPreflightJitterSync passes before init");
    expectTrue(jitterReject == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass tryPreflightJitterSync reject reason is None");

    expectTrue(pass->tryPreflightJitterSync(3u, jitterReject),
void testTaaPassTryPreflightGuards() {
    passDesc.params.blend_factor = 0.2f;

    expectTrue(pass->preflightJitterAdvance(), "pass preflightJitterAdvance passes before init");

    expectTrue(pass->tryPreflightJitterSync(4u, jitterReject),
               "pass tryPreflightJitterSync reject reason is None before init");
    expectTrue(pass->tryPreflightJitterNdc(jitterReject),
               "pass tryPreflightJitterNdc passes before init");
    expectTrue(pass->tryPreflightJitterAdvance(jitterReject),
               "pass tryPreflightJitterAdvance passes before init");
    expectTrue(pass->preflightJitterAdvance(&jitterReject),
               "pass preflightJitterAdvance passes before init");
void testTaaPassTryPreflightAndClassifyGuards() {
    passDesc.params.blend_factor = 0.35f;


               "pass classifyJitterSyncReject passes before init");
               "pass classifyJitterNdcReject passes before init");
               "pass classifyJitterAdvanceReject passes before init");

    expectTrue(pass->tryPreflightJitterSync(4u, jitterReject), "pass tryPreflightJitterSync passes before init");
    expectTrue(pass->tryPreflightJitterNdc(jitterReject), "pass tryPreflightJitterNdc passes before init");
               "pass tryPreflightJitterNdc reject reason is None before init");
    expectTrue(pass->tryPreflightJitterAdvance(jitterReject), "pass tryPreflightJitterAdvance passes before init");
               "pass tryPreflightJitterAdvance reject reason is None before init");

    fuse::renderer::TaaHistoryReuseBlockReason reuseReason = fuse::renderer::TaaHistoryReuseBlockReason::None;

    expectTrue(pass->ndcJitterForFrameIndexIfReady(3u, ndcOut),
               "pass ndcJitterForFrameIndexIfReady succeeds before init");
    const fuse::math::Vec2 directNdc =
        fuse::renderer::TaaJitterLayout::ndcOffsetForFrameIndex(3u, 128u, 128u, 8u);
    expectNear(ndcOut.x, directNdc.x, 1e-6f,
               "pass ndcJitterForFrameIndexIfReady matches layout helper X");

               "pass tryPreflightJitterSync passes for valid sequence");
               "pass classifyJitterSyncReject returns None for valid sequence");

               "pass tryPreflightJitterNdc passes for valid viewport");
               "pass tryPreflightJitterNdc reject reason is None");
               "pass classifyJitterNdcReject returns None for valid viewport");

               "pass tryPreflightJitterAdvance passes for valid sequence");
               "pass tryPreflightJitterAdvance reject reason is None");
    expectTrue(pass->preflightJitterAdvance(), "pass preflightJitterAdvance passes");
    expectTrue(!pass->shouldSkipJitterAdvance(), "pass should not skip jitter advance");
               "pass classifyJitterSyncReject is None for valid sequence");
               "pass classifyJitterNdcReject is None for valid viewport");
               "pass classifyJitterAdvanceReject is None for valid sequence");
    expectTrue(!pass->shouldSkipJitterAdvance(), "pass should not skip jitter advance for valid sequence");

    fuse::renderer::TaaJitterGuardRejectReason jitterReason =
    expectTrue(pass->tryPreflightJitterSync(6u, jitterReason), "pass tryPreflightJitterSync passes");
    expectTrue(jitterReason == fuse::renderer::TaaJitterGuardRejectReason::None,
    expectTrue(pass->tryPreflightJitterNdc(jitterReason), "pass tryPreflightJitterNdc passes");
    expectTrue(pass->tryPreflightJitterAdvance(jitterReason), "pass tryPreflightJitterAdvance passes");

    fuse::renderer::TaaHistoryReuseBlockReason reuseReason =
        fuse::renderer::TaaHistoryReuseBlockReason::None;
    passDesc.params.blend_factor = 0.3f;





    expectTrue(pass->tryPreflightJitterSync(3u, jitterReject), "pass tryPreflightJitterSync passes before init");

void testTaaPassTryAndClassifyGuardWrappers() {

    expectTrue(pass->canAdvanceJitter(), "pass can advance jitter before init");
    expectTrue(pass->canSyncJitterToFrameIndex(4u), "pass can sync jitter before init");
               "pass jitter sync classify is None before init");
               "pass jitter NDC classify is None before init");
               "pass jitter advance classify is None before init");



    expectTrue(pass->tryPreflightJitterSync(3u, jitterReject), "pass tryPreflightJitterSync passes");
    expectTrue(pass->tryPreflightJitterNdc(jitterReject), "pass tryPreflightJitterNdc passes");
    expectTrue(pass->tryPreflightJitterAdvance(jitterReject), "pass tryPreflightJitterAdvance passes");

               "pass classifyJitterSyncReject passes for default sequence");
               "pass classifyJitterNdcReject passes for valid viewport");
               "pass classifyJitterAdvanceReject passes for default sequence");

    expectTrue(pass->tryPreflightJitterSync(3u, jitterReason), "pass tryPreflightJitterSync passes");

void testTaaPassTryClassifyGuardWrappers() {




    expectTrue(pass->preflightJitterAdvance(&jitterReject), "pass preflightJitterAdvance passes before init");



    expectTrue(pass->tryPreflightJitterSync(5u, jitterReject), "pass tryPreflightJitterSync passes before init");



        fuse::renderer::TaaJitterGuardRejectReason::None;






    expectTrue(!pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse fails before init");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::NotReady,
               "pass tryPreflightHistoryReuse reason is NotReady before init");
    expectTrue(!pass->tryPreflightHistoryReadyForResolve(reuseReason),
               "pass tryPreflightHistoryReadyForResolve fails before init");
               "pass tryPreflightHistoryReadyForResolve reason is NotReady before init");



    fuse::math::Vec2 pixelOut{};
    expectTrue(pass->offsetForFrameIndexIfReady(3u, pixelOut),
               "pass offsetForFrameIndexIfReady succeeds before init");
    const fuse::math::Vec2 directPixel =
        fuse::renderer::TaaJitterLayout::offsetForFrameIndex(3u, pass->jitter().sequenceLength());
    expectNear(pixelOut.x, directPixel.x, 1e-6f, "pass offsetForFrameIndexIfReady matches layout helper X");

    expectTrue(pass->ndcOffsetForFrameIndexIfReady(3u, ndcOut),
               "pass ndcOffsetForFrameIndexIfReady succeeds before init");
    const fuse::math::Vec2 directNdc = fuse::renderer::TaaJitterLayout::ndcOffsetForFrameIndex(
        3u, passDesc.width, passDesc.height, pass->jitter().sequenceLength());
    expectNear(ndcOut.x, directNdc.x, 1e-6f, "pass ndcOffsetForFrameIndexIfReady matches layout helper X");

void testTaaPassB59DeepenTryAndCompositeGuards() {

               "pass classifyJitterNdcReject passes for default viewport");

    expectTrue(pass->preflightJitterSyncAndNdc(6u, &jitterReason),
               "pass preflightJitterSyncAndNdc passes for valid viewport");
    expectTrue(!pass->jitterAlignedToFrameIndex(6u), "pass jitter not aligned before trySync");
    expectTrue(pass->trySyncJitterToFrameIndex(6u, jitterReason), "pass trySyncJitterToFrameIndex succeeds");
    expectTrue(pass->jitterAlignedToFrameIndex(6u), "pass jitter aligned after trySync");

    const fuse::u32 jitterIndexBefore = pass->jitter().index();
    expectTrue(pass->tryAdvanceJitter(jitterReason), "pass tryAdvanceJitter succeeds");
    expectTrue(pass->jitter().index() != jitterIndexBefore, "pass tryAdvanceJitter advances jitter");

    expectTrue(!pass->tryPreflightHistoryWarmup(reuseReason),
               "tryPreflightHistoryWarmup fails before init");
               "warmup tryPreflight reason is NotReady before init");
               "tryPreflightHistoryReadyForResolve fails before init");
               "resolve tryPreflight reason is NotReady before init");

    expectTrue(pass->tryPreflightJitterSync(6u, jitterReason), "tryPreflightJitterSync passes before init");
               "tryPreflightJitterSync reject reason is None before init");
    expectTrue(pass->tryPreflightJitterNdc(jitterReason), "tryPreflightJitterNdc passes before init");
void testTaaPassTryPreflightAndCompositeGuards() {
    passDesc.params.blend_factor = 0.4f;


    expectTrue(pass->tryPreflightJitterSync(4u, jitterReason),
    expectTrue(pass->tryPreflightJitterNdc(jitterReason),
    expectTrue(pass->tryPreflightJitterAdvance(jitterReason),
    expectTrue(pass->preflightJitterSyncAndNdc(4u, &jitterReason),
               "pass preflightJitterSyncAndNdc passes before init");
    expectTrue(!pass->shouldSkipJitterSyncAndNdc(4u),
               "pass should not skip jitter sync+NDC before init");
    expectTrue(!pass->shouldSkipJitterAdvance(),
               "pass should not skip jitter advance before init");

    expectTrue(pass->shouldSkipHistoryWarmupAndReuse(0u),
               "pass should skip warmup/reuse before init");
void testTaaPassHistoryWarmupPreflight() {
    passDesc.width = 64;
    passDesc.height = 64;

    expectTrue(pass->classifyHistoryWarmupBlock() == fuse::renderer::TaaHistoryReuseBlockReason::NotReady,
               "pass classifyHistoryWarmupBlock is NotReady before init");

    expectTrue(!pass->preflightHistoryWarmup(&reason), "pass warmup preflight fails before init");
               "pass warmup preflight reason is NotReady before init");
    expectTrue(!pass->tryPreflightHistoryWarmup(reason),
               "pass tryPreflightHistoryWarmup fails before init");
               "pass tryPreflightHistoryWarmup reason is NotReady before init");
    expectTrue(!pass->tryPreflightHistoryReadyForResolve(reason),

               "pass classifyJitterSyncReject is None for default sequence");
               "pass classifyJitterAdvanceReject is None for default sequence");

    expectTrue(pass->tryPreflightJitterSync(4u, jitterReject), "pass tryPreflightJitterSync passes");
    expectTrue(pass->tryPreflightJitterFrame(4u, jitterReject), "pass tryPreflightJitterFrame passes");
    expectTrue(pass->preflightJitterFrame(4u), "pass preflightJitterFrame passes");
    expectTrue(!pass->shouldSkipJitterFrame(4u), "pass should not skip jitter frame");

               "pass history ready-for-resolve reason is NotReady before init");
    expectTrue(pass->shouldSkipHistoryTemporal(0u), "pass should skip history temporal before init");

    expectTrue(pass->classifyHistoryReuseBlock(0u) == fuse::renderer::TaaHistoryReuseBlockReason::NotReady,
               "pass classifyHistoryReuseBlock is NotReady before init");




    expectTrue(pass->classifyHistoryReuseBlock(0u) ==
                   fuse::renderer::TaaHistoryReuseBlockReason::NotReady,


    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for warmup block preflight test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for warmup classify test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for warmup preflight test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for warmup preflight deepen test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass try-preflight wrapper test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass try-preflight test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass try/classify guard test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for B5.9 deepen try/composite guard test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass tryPreflight wrapper test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass warmup preflight test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass try/classify wrapper test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaHistoryBuffer history;
    fuse::renderer::TaaHistoryBufferDesc historyDesc{64, 64};
    expectTrue(history.init(resources, historyDesc), "history ready for warmup block preflight test");
    expectTrue(fuse::renderer::classifyTaaHistoryWarmupBlock(history) ==
                   fuse::renderer::TaaHistoryWarmupBlockReason::NeedsWarmup,
               "allocated history warmup block is NeedsWarmup");
    expectTrue(!fuse::renderer::taaHistoryWarmupComplete(history),
               "allocated history warmup is not complete");
    expectTrue(!fuse::renderer::preflightTaaHistoryWarmup(history, &reason),
               "unwarmed history fails warmup preflight");
    expectTrue(reason == fuse::renderer::TaaHistoryWarmupBlockReason::NeedsWarmup,
               "unwarmed history warmup preflight reason is NeedsWarmup");

    history.markResolved();
                   fuse::renderer::TaaHistoryWarmupBlockReason::None,
               "warmed history warmup block is None");
    expectTrue(fuse::renderer::taaHistoryWarmupComplete(history), "warmed history warmup is complete");
    expectTrue(fuse::renderer::tryPreflightTaaHistoryWarmup(history, reason),
               "tryPreflightTaaHistoryWarmup passes for warmed history");
    expectTrue(reason == fuse::renderer::TaaHistoryWarmupBlockReason::None,
               "warmed history warmup preflight reason is None");
    expectTrue(history.init(resources, historyDesc), "history ready for warmup classify test");
               "unwarmed history warmup classified as NeedsWarmup");
    expectTrue(!fuse::renderer::preflightTaaHistoryWarmup(history, &warmupReason),
    expectTrue(warmupReason == fuse::renderer::TaaHistoryWarmupBlockReason::NeedsWarmup,

               "warmed history warmup classified as None");
    expectTrue(fuse::renderer::preflightTaaHistoryWarmup(history, &warmupReason),
               "warmed history passes warmup preflight");
    expectTrue(fuse::renderer::tryPreflightTaaHistoryWarmup(history, warmupReason),
    expectTrue(warmupReason == fuse::renderer::TaaHistoryWarmupBlockReason::None,
               "tryPreflight warmup reason is None for warmed history");
    expectTrue(history.init(resources, historyDesc), "history ready for warmup preflight test");
    expectTrue(history.init(resources, historyDesc), "history ready for warmup preflight deepen test");
    expectTrue(fuse::renderer::taaHistoryResolveReady(history), "allocated history is resolve-ready");
    expectTrue(fuse::renderer::preflightTaaHistoryReadyForResolve(history, &reason),
               "allocated history resolve preflight passes");
    expectTrue(reason == fuse::renderer::TaaHistoryReuseBlockReason::None,
               "allocated history resolve preflight reason is None");
    expectTrue(fuse::renderer::tryPreflightTaaHistoryReadyForResolve(history, reason),
               "tryPreflightTaaHistoryReadyForResolve passes for allocated history");

    expectTrue(!fuse::renderer::taaHistoryWarmupReady(history),
               "unwarmed history is not warmup-ready");
               "unwarmed history warmup preflight fails");
    expectTrue(reason == fuse::renderer::TaaHistoryReuseBlockReason::NotWarm,
               "unwarmed history warmup preflight reason is NotWarm");
    expectTrue(!fuse::renderer::tryPreflightTaaHistoryWarmup(history, reason),
               "tryPreflightTaaHistoryWarmup fails for unwarmed history");
               "tryPreflight warmup reason is NotWarm for unwarmed history");

    expectTrue(fuse::renderer::preflightTaaHistoryWarmup(history, &reason),
               "tryPreflightTaaHistoryWarmup reason is NotWarm for unwarmed history");

    expectTrue(fuse::renderer::taaHistoryWarmupReady(history), "warmed history is warmup-ready");
               "warmed history warmup preflight passes");

    history.destroy();
    expectTrue(pass->init(resources), "TaaPass initialized for warmup preflight test");

    expectTrue(pass->tryPreflightHistoryReadyForResolve(reason),
               "pass tryPreflightHistoryReadyForResolve passes after init");
               "pass tryPreflightHistoryReadyForResolve reason is None after init");
    expectTrue(pass->classifyHistoryWarmupBlock() == fuse::renderer::TaaHistoryReuseBlockReason::NotWarm,
               "pass classifyHistoryWarmupBlock is NotWarm before first resolve");
    expectTrue(!pass->preflightHistoryWarmup(&reason), "pass warmup preflight fails before first resolve");
               "pass warmup preflight reason is NotWarm before first resolve");

    fuse::renderer::TaaResolveDesc resolveDesc{};
    resolveDesc.width = 64;
    resolveDesc.height = 64;
    resolveDesc.surfaces.current_frame = reinterpret_cast<void*>(0x10);
    resolveDesc.surfaces.output = reinterpret_cast<void*>(0x20);
    expectTrue(pass->resolveFrame(resolveDesc), "initial resolve warms pass history");

    expectTrue(pass->classifyHistoryWarmupBlock() == fuse::renderer::TaaHistoryReuseBlockReason::None,
               "pass classifyHistoryWarmupBlock is None after warmup");
    expectTrue(pass->preflightHistoryWarmup(&reason), "pass warmup preflight passes after resolve");
    expectTrue(pass->tryPreflightHistoryWarmup(reason), "pass tryPreflightHistoryWarmup passes after resolve");

    pass->destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testJitterAlignmentPreflight() {
void testJitterAlignmentPreflightGuards() {

    expectTrue(std::strcmp(fuse::renderer::taaJitterGuardRejectReasonLabel(
                               fuse::renderer::TaaJitterGuardRejectReason::Misaligned),
                           "misaligned") == 0,
               "Misaligned jitter guard reject label");

    expectTrue(fuse::renderer::classifyTaaJitterAlignmentReject(jitter, 5u) ==
                   fuse::renderer::TaaJitterGuardRejectReason::Misaligned,
               "default jitter is misaligned to frame five");
    expectTrue(fuse::renderer::shouldSkipTaaJitterAlignment(jitter, 5u),
               "misaligned jitter should skip alignment preflight");

    expectTrue(!fuse::renderer::preflightTaaJitterAlignment(jitter, 5u, &rejectReason),
               "preflightTaaJitterAlignment fails when misaligned");
    expectTrue(rejectReason == fuse::renderer::TaaJitterGuardRejectReason::Misaligned,
               "misaligned jitter alignment reject reason is Misaligned");

    jitter.syncToFrameIndex(5u);
                   fuse::renderer::TaaJitterGuardRejectReason::None,
               "synced jitter passes alignment classify");
    expectTrue(fuse::renderer::tryPreflightTaaJitterAlignment(jitter, 5u, rejectReason),
               "tryPreflightTaaJitterAlignment passes when aligned");
    expectTrue(!fuse::renderer::shouldSkipTaaJitterAlignment(jitter, 5u),
               "aligned jitter should not skip alignment preflight");
void testJitterTryIfReadyDeepen() {
    expectTrue(jitter.currentPixelOffsetIfReady(pixelOut), "currentPixelOffsetIfReady succeeds for valid sequence");
    const fuse::math::Vec2 directPixel = jitter.currentPixelOffset();
    expectNear(pixelOut.x, directPixel.x, 1e-6f, "currentPixelOffsetIfReady matches currentPixelOffset X");

               "tryCurrentNdcOffsetIfReady succeeds for valid viewport");
               "tryCurrentNdcOffsetIfReady reject reason is None");
    const fuse::math::Vec2 directNdc = jitter.currentNdcOffset(128u, 128u);
    expectNear(ndcOut.x, directNdc.x, 1e-6f, "tryCurrentNdcOffsetIfReady matches currentNdcOffset X");
               "tryCurrentNdcOffsetIfReady rejects zero width");
               "tryCurrentNdcOffsetIfReady reject reason is InvalidViewport");

    const fuse::u32 indexBefore = jitter.index();
    expectTrue(jitter.tryAdvanceIfReady(rejectReason), "tryAdvanceIfReady succeeds for valid sequence");
               "tryAdvanceIfReady reject reason is None");
    expectTrue(jitter.index() != indexBefore, "tryAdvanceIfReady advances jitter");

    jitter.reset();
    expectTrue(jitter.trySyncToFrameIndexIfReady(9u, rejectReason),
               "trySyncToFrameIndexIfReady succeeds for valid sequence");
               "trySyncToFrameIndexIfReady reject reason is None");
    expectTrue(jitter.monotonicFrameIndex() == 9u, "trySyncToFrameIndexIfReady sets monotonic counter");
    expectTrue(jitter.isAlignedToFrameIndex(9u), "jitter aligned after trySyncToFrameIndexIfReady");

    fuse::renderer::TaaJitterDesc invalidDesc{};
    invalidDesc.sequence_length = 0u;
    fuse::renderer::TaaJitter fallbackJitter(invalidDesc);
    expectTrue(fuse::renderer::classifyTaaJitterAlignmentReject(fallbackJitter, 0u) ==
               "fallback jitter aligned to frame zero after default sequence");

void testResolveWithBlendPreflight() {
    expectTrue(std::strcmp(fuse::renderer::taaResolveWithBlendRejectReasonLabel(
                               fuse::renderer::TaaResolveWithBlendRejectReason::ResolveBlocked),
                           "resolve_blocked") == 0,
               "ResolveBlocked combined reject label");
                               fuse::renderer::TaaResolveWithBlendRejectReason::BlendRejected),
                           "blend_rejected") == 0,
               "BlendRejected combined reject label");

void testJitterTrySyncAdvanceAndNdc() {

    expectTrue(jitter.trySyncToFrameIndexIfReady(4u, rejectReason),
               "trySync reject reason is None");
    expectTrue(jitter.isAlignedToFrameIndex(4u), "jitter aligned after trySync");

               "tryAdvance reject reason is None");

               "tryCurrentNdc reject reason is None");
               "tryCurrentNdc reject reason is InvalidViewport");

void testResolveFrameCompositePreflight() {
void testJitterTryIfReadyWithRejectReason() {



    expectTrue(jitter.isAlignedToFrameIndex(4u), "jitter aligned after trySyncToFrameIndexIfReady");


    expectTrue(TaaJitterLayout::tryOffsetForFrameIndexIfReady(2u, 8u, pixelOut, rejectReason),
               "tryOffsetForFrameIndexIfReady succeeds for valid sequence");
    const fuse::math::Vec2 directPixel = TaaJitterLayout::offsetForFrameIndex(2u, 8u);
    expectNear(pixelOut.x, directPixel.x, 1e-6f, "tryOffsetForFrameIndexIfReady matches offsetForFrameIndex X");
    expectTrue(!TaaJitterLayout::tryOffsetForFrameIndexIfReady(2u, 0u, pixelOut, rejectReason),
               "tryOffsetForFrameIndexIfReady rejects invalid sequence");
    expectTrue(rejectReason == fuse::renderer::TaaJitterGuardRejectReason::InvalidSequence,
               "tryOffsetForFrameIndexIfReady reject reason is InvalidSequence");

    expectTrue(TaaJitterLayout::tryNdcOffsetForFrameIndexIfReady(2u, 128u, 128u, 8u, ndcOut, rejectReason),
               "tryNdcOffsetForFrameIndexIfReady succeeds for valid viewport");
    const fuse::math::Vec2 directNdc = TaaJitterLayout::ndcOffsetForFrameIndex(2u, 128u, 128u, 8u);
    expectNear(ndcOut.x, directNdc.x, 1e-6f, "tryNdcOffsetForFrameIndexIfReady matches ndcOffsetForFrameIndex X");
    expectTrue(!TaaJitterLayout::tryNdcOffsetForFrameIndexIfReady(2u, 0u, 128u, 8u, ndcOut, rejectReason),
               "tryNdcOffsetForFrameIndexIfReady rejects zero width");
               "tryNdcOffsetForFrameIndexIfReady reject reason is InvalidViewport");

void testResolveFramePreflight() {
    desc.width = 64;
    desc.height = 64;
    desc.surfaces.current_frame = reinterpret_cast<void*>(0x10);
    desc.surfaces.output = reinterpret_cast<void*>(0x20);

    expectTrue(fuse::renderer::classifyTaaResolveWithBlendReject(desc, emptyHistory) ==
                   fuse::renderer::TaaResolveWithBlendRejectReason::ResolveBlocked,
               "empty history blocks combined preflight with resolve");
    expectTrue(fuse::renderer::shouldSkipTaaResolveWithBlend(desc, emptyHistory),
               "empty history should skip combined resolve+blend");

    fuse::renderer::TaaResolveWithBlendRejectReason rejectReason =
        fuse::renderer::TaaResolveWithBlendRejectReason::None;
    expectTrue(!fuse::renderer::tryPreflightTaaResolveWithBlend(desc, emptyHistory, rejectReason),
               "tryPreflightTaaResolveWithBlend fails for empty history");
    expectTrue(rejectReason == fuse::renderer::TaaResolveWithBlendRejectReason::ResolveBlocked,
               "empty history combined reject reason is ResolveBlocked");

    expectTrue(bootstrap != nullptr, "bootstrap allocated for resolve+blend preflight test");



    expectTrue(history.init(resources, historyDesc), "history ready for resolve+blend preflight test");
    expectTrue(fuse::renderer::preflightTaaResolveWithBlend(desc, history, &rejectReason),
               "valid resolve+blend preflight passes for allocated history");
    expectTrue(rejectReason == fuse::renderer::TaaResolveWithBlendRejectReason::None,
               "valid resolve+blend preflight reject reason is None");

    fuse::renderer::TaaBlendWeights weights{};
    expectTrue(fuse::renderer::tryComputeTaaResolveBlendWeightsIfResolveReady(desc, history, weights,
                                                                              rejectReason),
               "tryComputeTaaResolveBlendWeightsIfResolveReady passes for valid resolve");
    expectNear(weights.current, 1.f, 1e-5f, "resolve-ready compute uses full current on warmup");
    expectNear(weights.history, 0.f, 1e-5f, "resolve-ready compute uses zero history on warmup");

               "tryComputeTaaResolveBlendWeightsIfResolveReady passes after warmup");
    expectNear(weights.history, 0.9f, 1e-5f, "resolve-ready compute uses history after warmup");

    desc.width = 0u;
    expectTrue(fuse::renderer::classifyTaaResolveWithBlendReject(desc, history) ==
               "invalid dimensions block combined preflight before blend");


void testTaaPassWarmupAndCompositePreflightWrappers() {
    fuse::renderer::TaaResolveSkipReason skipReason = fuse::renderer::TaaResolveSkipReason::None;
    fuse::renderer::TaaResolveBlendRejectReason blendReason =
        fuse::renderer::TaaResolveBlendRejectReason::None;
    expectTrue(fuse::renderer::shouldSkipTaaResolveFrame(desc, emptyHistory),
               "resolve frame should skip when history not ready");
    expectTrue(!fuse::renderer::preflightTaaResolveFrame(desc, emptyHistory, &skipReason, &blendReason),
               "resolve frame preflight fails when history not ready");
    expectTrue(!fuse::renderer::tryPreflightTaaResolveFrame(desc, emptyHistory, skipReason, blendReason),
               "tryPreflightTaaResolveFrame fails when history not ready");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::HistoryNotReady,
               "resolve frame skip reason is HistoryNotReady");
    expectTrue(blendReason == fuse::renderer::TaaResolveBlendRejectReason::None,
               "resolve frame blend reason is None when resolve skips");
    expectTrue(!fuse::renderer::shouldSkipTaaJitterAlignment(jitter, 0u),
               "default jitter is aligned to frame zero");
    expectTrue(fuse::renderer::classifyTaaJitterAlignmentReject(jitter, 0u) ==
               "default jitter classifies as aligned for frame zero");
               "default jitter is not aligned to frame five");

    expectTrue(jitter.syncToFrameIndexIfReady(5u), "syncToFrameIndexIfReady succeeds");
    expectTrue(fuse::renderer::preflightTaaJitterAlignment(jitter, 5u, &rejectReason),
               "preflightTaaJitterAlignment passes after sync");
               "alignment reject reason is None after sync");
               "tryPreflightTaaJitterAlignment passes after sync");
    expectTrue(!fuse::renderer::preflightTaaJitterAlignment(jitter, 6u, &rejectReason),
               "preflightTaaJitterAlignment rejects misaligned frame");
               "alignment reject reason is Misaligned");

    jitter.advance();
    expectTrue(TaaJitterLayout::jitterSlotMatchesFrameIndex(6u, jitter.index(), jitter.sequenceLength()),
               "advanced jitter slot matches frame six");
    expectTrue(fuse::renderer::preflightTaaJitterAlignment(jitter, 6u, &rejectReason),
               "preflightTaaJitterAlignment passes after advance to frame six");

    expectTrue(fuse::renderer::classifyTaaJitterSyncReject(0u) ==
                   fuse::renderer::TaaJitterGuardRejectReason::InvalidSequence,
               "zero sequence length fails sync classify before alignment");

void testHistoryWarmupSatisfiedPreflight() {
    expectTrue(!fuse::renderer::taaHistoryWarmupSatisfied(emptyHistory),
               "empty history warmup is not satisfied");

    expectTrue(!fuse::renderer::tryPreflightTaaHistoryWarmupSatisfied(emptyHistory, reason),
               "tryPreflight warmup fails for empty history");
void testJitterAlignmentAndSyncNdcGuards() {

                               fuse::renderer::TaaJitterGuardRejectReason::MisalignedFrame),
                           "misaligned_frame") == 0,
               "MisalignedFrame jitter guard reject label");

    expectTrue(fuse::renderer::classifyTaaJitterAlignmentReject(5u, 5u, 5u, 8u) ==
               "aligned jitter state passes alignment classify");
    expectTrue(fuse::renderer::classifyTaaJitterAlignmentReject(5u, 4u, 5u, 8u) ==
                   fuse::renderer::TaaJitterGuardRejectReason::MisalignedFrame,
               "monotonic mismatch fails alignment classify");
    expectTrue(fuse::renderer::classifyTaaJitterAlignmentReject(5u, 5u, 6u, 8u) ==
               "slot mismatch fails alignment classify");

    expectTrue(fuse::renderer::tryPreflightTaaJitterAlignment(13u, 13u, 5u, 8u, rejectReason),
               "wrapped frame alignment passes tryPreflight");
               "wrapped frame alignment reject reason is None");
    expectTrue(fuse::renderer::shouldSkipTaaJitterAlignment(5u, 4u, 5u, 8u),
               "misaligned state should skip alignment preflight");

    expectTrue(TaaJitterLayout::canSyncAndProduceNdc(128u, 128u, 8u),
               "valid viewport allows sync and NDC production");
    expectTrue(!TaaJitterLayout::canSyncAndProduceNdc(0u, 128u, 8u),
               "zero width blocks sync and NDC production");

    expectTrue(fuse::renderer::classifyTaaJitterSyncAndNdcReject(128u, 128u, 8u) ==
               "valid viewport passes sync+NDC classify");
    expectTrue(fuse::renderer::preflightTaaJitterSyncAndNdc(5u, 128u, 128u, 8u, &rejectReason),
               "preflightTaaJitterSyncAndNdc passes for valid viewport");
    expectTrue(fuse::renderer::tryPreflightTaaJitterSyncAndNdc(5u, 128u, 128u, 8u, rejectReason),
               "tryPreflightTaaJitterSyncAndNdc passes for valid viewport");
    expectTrue(!fuse::renderer::preflightTaaJitterSyncAndNdc(5u, 0u, 128u, 8u, &rejectReason),
               "preflightTaaJitterSyncAndNdc rejects zero width");
               "sync+NDC reject reason is InvalidViewport for zero width");
    expectTrue(fuse::renderer::shouldSkipTaaJitterSyncAndNdc(5u, 0u, 128u, 8u),
               "zero width should skip sync+NDC preflight");

    expectTrue(!jitter.preflightAlignmentToFrameIndex(3u), "default jitter fails alignment preflight");
    fuse::renderer::TaaJitterGuardRejectReason syncReason =
    expectTrue(jitter.trySyncToFrameIndexIfReady(3u, syncReason), "trySyncToFrameIndexIfReady succeeds");
    expectTrue(syncReason == fuse::renderer::TaaJitterGuardRejectReason::None,
    expectTrue(jitter.preflightAlignmentToFrameIndex(3u), "synced jitter passes alignment preflight");

    expectTrue(jitter.syncToFrameIndexAndProduceNdcIfReady(7u, 128u, 128u, ndcOut),
               "syncToFrameIndexAndProduceNdcIfReady succeeds");
    const fuse::math::Vec2 expectedNdc =
        fuse::renderer::TaaJitterLayout::ndcOffsetForFrameIndex(7u, 128u, 128u, 8u);
    expectNear(ndcOut.x, expectedNdc.x, 1e-6f, "sync+NDC output matches layout NDC X");
    expectTrue(!jitter.syncToFrameIndexAndProduceNdcIfReady(7u, 0u, 128u, ndcOut),
               "syncToFrameIndexAndProduceNdcIfReady fails for zero width");

void testHistoryWarmupAndTemporalSampleGuards() {
               "NeedsWarmup history warmup block label");



    expectTrue(bootstrap != nullptr, "bootstrap allocated for resolve frame composite test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for warmup satisfied test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for warmup/temporal sample test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for resolve frame preflight test");



    expectTrue(history.init(resources, historyDesc), "history ready for resolve frame composite test");
    expectTrue(fuse::renderer::preflightTaaResolveFrame(desc, history, &skipReason, &blendReason),
               "resolve frame preflight passes for valid desc and unwarmed history");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::None,
               "resolve frame skip reason is None for valid desc");
               "resolve frame blend reason is None for warmup weights");
    expectTrue(fuse::renderer::tryPreflightTaaResolveFrame(desc, history, skipReason, blendReason),
               "tryPreflightTaaResolveFrame passes for valid desc");

    expectTrue(!fuse::renderer::shouldSkipTaaResolveFrame(desc, history),
               "resolve frame should not skip after warmup");
               "tryPreflightTaaResolveFrame passes after warmup");

    expectTrue(fuse::renderer::shouldSkipTaaResolveFrame(desc, history),
               "resolve frame should skip with invalid dimensions");
    expectTrue(!fuse::renderer::tryPreflightTaaResolveFrame(desc, history, skipReason, blendReason),
               "tryPreflightTaaResolveFrame fails with invalid dimensions");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::InvalidDimensions,
               "resolve frame skip reason is InvalidDimensions");
    expectTrue(history.init(resources, historyDesc), "history ready for warmup satisfied test");
                   fuse::renderer::TaaHistoryReuseBlockReason::NotWarm,
               "allocated history warmup block is NotWarm");
    expectTrue(!fuse::renderer::preflightTaaHistoryWarmupSatisfied(history, &reason),
               "unwarmed history fails warmup satisfied preflight");

    expectTrue(fuse::renderer::taaHistoryWarmupSatisfied(history),
               "warmed history warmup is satisfied");
    expectTrue(fuse::renderer::preflightTaaHistoryWarmupSatisfied(history, &reason),
               "warmed history passes warmup satisfied preflight");
    expectTrue(history.init(resources, historyDesc), "history ready for warmup/temporal sample test");
               "unwarmed history warmup classify is NeedsWarmup");

    expectTrue(!fuse::renderer::preflightTaaHistoryTemporalSample(history, 0u, &reuseReason),
               "unwarmed history fails temporal sample preflight");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::NotWarm,
               "unwarmed temporal sample preflight reason is NotWarm");
    expectTrue(fuse::renderer::shouldSkipTaaHistoryTemporalSample(history, 0u),
               "unwarmed history should skip temporal sample");

               "tryPreflightTaaHistoryWarmup passes after warmup");
    expectTrue(fuse::renderer::tryPreflightTaaHistoryTemporalSample(history, 0u, reuseReason),
               "tryPreflightTaaHistoryTemporalSample passes after warmup");
    expectTrue(!fuse::renderer::shouldSkipTaaHistoryTemporalSample(history, 0u),
               "warmed history should not skip temporal sample");
    expectTrue(history.init(resources, historyDesc), "history ready for resolve frame preflight test");
               "resolve frame preflight passes with valid desc and history");
               "resolve frame skip reason is None");
               "resolve frame blend reason is None");

    expectTrue(fuse::renderer::preflightTaaResolveFrame(desc, history),
               "resolve frame preflight passes after warmup");

    expectTrue(!fuse::renderer::preflightTaaResolveFrame(desc, history, &skipReason, &blendReason),
               "resolve frame preflight fails with invalid dimensions");


void testResolveTemporalBlendGuards() {
    expectTrue(std::strcmp(fuse::renderer::taaResolveTemporalBlendRejectReasonLabel(
                               fuse::renderer::TaaResolveTemporalBlendRejectReason::HistoryReuseBlocked),
               "HistoryReuseBlocked temporal blend reject label");

    desc.params.blend_factor = 0.25f;

    fuse::renderer::TaaResolveTemporalBlendRejectReason temporalReason =
        fuse::renderer::TaaResolveTemporalBlendRejectReason::None;
    expectTrue(fuse::renderer::preflightTaaResolveTemporalBlend(desc, emptyHistory, &temporalReason),
               "warmup temporal blend preflight passes without warmed history");
    expectTrue(temporalReason == fuse::renderer::TaaResolveTemporalBlendRejectReason::None,
               "warmup temporal blend reject reason is None");

    expectTrue(bootstrap != nullptr, "bootstrap allocated for temporal blend guard test");



    expectTrue(history.init(resources, historyDesc), "history ready for temporal blend guard test");
    expectTrue(!fuse::renderer::shouldSkipTaaResolveTemporalBlend(desc, history),
               "warmup should not skip temporal blend preflight");

    expectTrue(fuse::renderer::tryPreflightTaaResolveTemporalBlend(desc, history, temporalReason),
               "steady temporal blend preflight passes after warmup");
    expectTrue(fuse::renderer::classifyTaaResolveTemporalBlendReject(desc, history) ==
                   fuse::renderer::TaaResolveTemporalBlendRejectReason::None,
               "steady temporal blend classify returns None");

    history.invalidateHistory();
    expectTrue(fuse::renderer::preflightTaaResolveTemporalBlend(desc, history, &temporalReason),
               "stale generation still passes temporal blend when history weight is zero");
               "stale generation temporal blend reject reason is None");

    desc.observed_history_generation = 0u;
               "matching generation passes temporal blend preflight");


void testTaaPassDeepenFollowUpGuards() {
    passDesc.params.blend_factor = 0.15f;


    expectTrue(!pass->preflightHistoryWarmup(&warmupReason),
               "pass warmup preflight fails before init");


    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass try preflight test");


    expectTrue(pass->init(resources), "TaaPass initialized for try preflight test");

    expectTrue(fallbackJitter.sequenceLength() == 8u,
               "invalid sequence length falls back to default in TaaJitter ctor");
    expectTrue(fallbackJitter.currentPixelOffsetIfReady(pixelOut),
               "fallback jitter currentPixelOffsetIfReady succeeds with default sequence");

    expectTrue(!TaaJitterLayout::offsetForFrameIndexIfReady(3u, 0u, pixelOut),
               "offsetForFrameIndexIfReady fails for invalid sequence length");
    expectTrue(!fuse::renderer::preflightTaaJitterAdvance(0u, &rejectReason),
               "preflightTaaJitterAdvance rejects invalid sequence");
               "preflightTaaJitterAdvance reject reason is InvalidSequence");
    expectTrue(!fuse::renderer::tryPreflightTaaJitterSync(3u, 0u, rejectReason),
               "tryPreflightTaaJitterSync rejects invalid sequence");
               "tryPreflightTaaJitterSync reject reason is InvalidSequence");

void testTaaPassTryPreflightDeepen() {



    expectTrue(pass->tryPreflightJitterSync(6u, jitterReject), "pass tryPreflightJitterSync passes before init");

    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass try-preflight deepen test");


    expectTrue(pass->init(resources), "TaaPass initialized for try-preflight deepen test");

               "pass tryPreflightHistoryWarmup fails before first resolve");
               "pass tryPreflightHistoryWarmup reason is NotWarm before resolve");
    expectTrue(pass->init(resources), "TaaPass initialized for try-preflight wrapper test");
    expectTrue(pass->init(resources), "TaaPass initialized for try-preflight test");

    expectTrue(pass->init(resources), "TaaPass initialized for try/classify guard test");

    expectTrue(pass->init(resources), "TaaPass initialized for B5.9 deepen try/composite guard test");

    expectTrue(pass->tryPreflightHistoryReadyForResolve(reuseReason),
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::None,

               "pass tryPreflightHistoryReuse fails before warmup");
               "pass tryPreflightHistoryReuse reason is NotWarm before warmup");
    expectTrue(!pass->tryPreflightHistoryWarmup(warmupReason),
               "pass tryPreflightHistoryWarmup reason is NeedsWarmup before resolve");
void testResolveTemporalBlendPreflight() {
    desc.params.blend_factor = 0.4f;

    expectTrue(fuse::renderer::shouldSkipTaaResolveTemporalBlend(desc, emptyHistory),
               "temporal blend should skip for empty history");
    expectTrue(!fuse::renderer::taaResolveTemporalBlendReady(desc, emptyHistory),
               "temporal blend not ready for empty history");
    expectTrue(!pass->historyWarmupComplete(), "pass warmup not complete before init");
    expectTrue(!pass->preflightHistoryWarmup(), "pass warmup preflight fails before init");

    expectTrue(pass->preflightJitterSyncAndNdc(9u, &jitterReason), "pass sync+NDC preflight before init");
    expectTrue(!pass->shouldSkipJitterSyncAndNdc(9u), "pass should not skip sync+NDC before init");

    expectTrue(pass->syncJitterToFrameIndexAndProduceNdcIfReady(9u, ndcOut),
               "pass sync+NDC succeeds before init");
    expectTrue(pass->preflightJitterAlignment(9u, &jitterReason), "pass alignment passes after sync+NDC");

    expectTrue(pass->shouldSkipJitterAdvance() == false, "pass should not skip jitter advance before init");


    expectTrue(bootstrap != nullptr, "bootstrap allocated for temporal blend preflight test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass deepen follow-up test");



    expectTrue(history.init(resources, historyDesc), "history ready for temporal blend preflight test");

    expectTrue(!fuse::renderer::tryPreflightTaaResolveTemporalBlend(desc, history, reuseReason, blendReason),
               "temporal blend preflight fails before warmup");
               "temporal blend reuse reason is NotWarm before warmup");
               "temporal blend weights still valid before warmup");

    expectTrue(fuse::renderer::tryPreflightTaaResolveTemporalBlend(desc, history, reuseReason, blendReason),
               "temporal blend preflight passes after warmup");
    expectTrue(fuse::renderer::taaResolveTemporalBlendReady(desc, history),
               "temporal blend ready after warmup");
               "temporal blend should not skip after warmup");

    const fuse::u32 observedGeneration = history.invalidateGeneration();
    desc.observed_history_generation = observedGeneration;
    expectTrue(!fuse::renderer::preflightTaaResolveTemporalBlend(desc, history, &reuseReason, &blendReason),
               "stale generation fails temporal blend preflight");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::StaleGeneration,
               "stale generation temporal blend reuse reason is StaleGeneration");


void testTaaPassDeepenAlignmentAndTemporalBlend() {

    expectTrue(pass->shouldSkipJitterAlignment(5u), "pass jitter not aligned to frame five before sync");
               "pass warmup block is NotReady before init");

    expectTrue(pass->syncJitterToFrameIndexIfReady(4u), "pass syncJitterToFrameIndexIfReady succeeds");
    expectTrue(pass->preflightJitterAlignment(4u, &jitterReject),
               "pass preflightJitterAlignment passes after sync");
    expectTrue(pass->tryPreflightJitterAlignment(4u, jitterReject),
               "pass tryPreflightJitterAlignment passes after sync");
    expectTrue(!pass->shouldSkipJitterAlignment(4u), "pass should not skip alignment after sync");

    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass alignment/temporal blend test");


    expectTrue(pass->init(resources), "TaaPass initialized for alignment/temporal blend test");
               "pass warmup block is NotWarm after init");

    fuse::renderer::TaaHistoryReuseBlockReason warmupReason = fuse::renderer::TaaHistoryReuseBlockReason::None;
    expectTrue(!pass->tryPreflightHistoryWarmupSatisfied(warmupReason),
               "pass warmup not satisfied before first resolve");
    expectTrue(warmupReason == fuse::renderer::TaaHistoryReuseBlockReason::NotWarm,
               "pass warmup reason is NotWarm before resolve");
    expectTrue(pass->init(resources), "TaaPass initialized for deepen follow-up test");
    expectTrue(pass->shouldSkipHistoryWarmup(), "pass should skip warmup before first resolve");

    expectTrue(!pass->preflightHistoryWarmup(&warmupReason), "pass warmup preflight fails before resolve");
               "pass warmup preflight reason is NeedsWarmup before resolve");
    expectTrue(pass->shouldSkipHistoryTemporalSample(0u),
               "pass should skip temporal sample before resolve");
    expectTrue(pass->init(resources), "TaaPass initialized for tryPreflight wrapper test");

    expectTrue(!pass->preflightHistoryWarmup(&reuseReason),
               "preflightHistoryWarmup fails before first resolve");
               "warmup preflight reason is NotWarm before resolve");
    expectTrue(pass->preflightJitterAdvance(), "preflightJitterAdvance passes after init");
    expectTrue(!pass->shouldSkipJitterAdvance(), "pass should not skip jitter advance after init");

    expectTrue(pass->trySyncJitterToFrameIndexIfReady(6u, jitterReason),
               "trySyncJitterToFrameIndexIfReady succeeds");

    expectTrue(pass->tryAdvanceJitterIfReady(jitterReason), "tryAdvanceJitterIfReady succeeds");
    expectTrue(pass->jitter().index() != jitterIndexBefore, "tryAdvanceJitterIfReady advances jitter");


    expectTrue(pass->tryPreflightJitterSync(6u, jitterReject), "pass tryPreflightJitterSync passes");
    expectTrue(pass->preflightJitterAdvance(&jitterReject), "pass preflightJitterAdvance passes");



    fuse::renderer::TaaHistoryReuseBlockReason historyReason =
    expectTrue(!pass->tryPreflightHistoryReuse(0u, historyReason),
    expectTrue(historyReason == fuse::renderer::TaaHistoryReuseBlockReason::NotReady,
    expectTrue(!pass->tryPreflightHistoryReadyForResolve(historyReason),

    expectTrue(pass->tryPreflightJitterSync(5u, jitterReason),
               "pass tryPreflightJitterSync reason is None before init");

    expectTrue(pass->tryPreflightJitterSync(4u, jitterReason), "pass tryPreflightJitterSync passes before init");
    expectTrue(pass->tryPreflightJitterNdc(jitterReason), "pass tryPreflightJitterNdc passes before init");
    expectTrue(pass->tryPreflightJitterAdvance(jitterReason), "pass tryPreflightJitterAdvance passes before init");
    expectTrue(!pass->preflightHistoryWarmupAndReuse(0u, &reuseReason),
               "pass preflightHistoryWarmupAndReuse fails before warmup");
               "pass preflightHistoryWarmupAndReuse reason is NotWarm before warmup");
    fuse::renderer::TaaPassDesc passDesc{};
    passDesc.width = 128;
    passDesc.height = 128;









               "pass tryPreflightJitterNdc reason is None before init");
               "pass tryPreflightJitterAdvance reason is None before init");

    expectTrue(!pass->tryPreflightHistoryTemporal(0u, reuseReason),
               "pass tryPreflightHistoryTemporal fails before warmup");
    expectTrue(pass->shouldSkipHistoryTemporal(0u), "pass should skip history temporal before warmup");
    expectTrue(pass->init(resources), "TaaPass initialized for try/classify wrapper test");





    resolveDesc.width = 128;
    resolveDesc.height = 128;
    resolveDesc.params = passDesc.params;

    expectTrue(pass->tryPreflightResolveBlendWeights(resolveDesc, blendReason),
               "pass tryPreflightResolveBlendWeights passes before warmup");
    expectTrue(pass->preflightResolveFrame(resolveDesc), "pass preflightResolveFrame passes before warmup");

    expectTrue(pass->tryPreflightResolve(resolveDesc, skipReason),
               "pass tryPreflightResolve passes before warmup");
    expectTrue(pass->tryPreflightResolveFrame(resolveDesc, skipReason, blendReason),
               "pass tryPreflightResolveFrame passes before warmup");

    expectTrue(pass->tryPreflightHistoryWarmup(warmupReason),
               "pass tryPreflightHistoryWarmup passes after resolve");
    expectTrue(pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse passes after warmup");
    expectTrue(!pass->shouldSkipResolveFrame(resolveDesc),
               "pass should not skip resolve frame after warmup");

    expectTrue(pass->shouldSkipResolveTemporalBlend(resolveDesc),
               "pass should skip temporal blend before warmup");


               "pass reuse reason is NotWarm before warmup");

    expectTrue(pass->tryPreflightHistoryWarmupSatisfied(warmupReason),
               "pass warmup satisfied after resolve");
    expectTrue(pass->tryPreflightResolveTemporalBlend(resolveDesc, reuseReason, blendReason),
               "pass tryPreflightResolveTemporalBlend passes after warmup");
    expectTrue(!pass->shouldSkipResolveTemporalBlend(resolveDesc),
               "pass should not skip temporal blend after warmup");

               "pass should not skip temporal blend before warmup resolve");

    expectTrue(pass->historyWarmupComplete(), "pass warmup complete after resolve");
    expectTrue(pass->preflightHistoryWarmup(&warmupReason), "pass warmup preflight passes after resolve");
    expectTrue(!pass->shouldSkipHistoryTemporalSample(0u),
               "pass should not skip temporal sample after resolve");
    expectTrue(pass->preflightResolveTemporalBlend(resolveDesc),
               "pass temporal blend preflight passes after resolve");

    expectTrue(pass->trySyncJitterToFrameIndexIfReady(11u, jitterReason),
               "pass trySyncJitterToFrameIndexIfReady succeeds");
    expectTrue(pass->jitterAlignedToFrameIndex(11u), "pass jitter aligned after trySync");

               "tryPreflightResolve passes before first resolve");
    expectTrue(pass->preflightResolveFrame(resolveDesc, &skipReason, &blendReason),
               "preflightResolveFrame passes before first resolve");
    expectTrue(!pass->shouldSkipResolveFrame(resolveDesc), "shouldSkipResolveFrame false before resolve");

    expectTrue(pass->tryPreflightHistoryWarmup(reuseReason), "tryPreflightHistoryWarmup passes after resolve");
    expectTrue(pass->preflightHistoryWarmup(), "preflightHistoryWarmup passes after resolve");

               "tryPreflightHistoryReuse passes with current generation after resolve");

    pass->invalidateHistory();
               "tryPreflightHistoryReuse fails with stale generation after invalidate");
               "tryPreflightHistoryReuse reason is StaleGeneration after invalidate");
    expectTrue(!pass->tryPreflightHistoryReuse(pass->historyInvalidateGeneration(), reuseReason),
               "tryPreflightHistoryReuse fails after invalidate even with current generation");


               "tryPreflightHistoryReuse reason is NotWarm after invalidate clears warmth");

    expectTrue(pass->resolveFrame(resolveDesc), "resolve after invalidate re-warms history");
    expectTrue(pass->tryPreflightHistoryReuse(pass->historyInvalidateGeneration(), reuseReason),
               "tryPreflightHistoryReuse passes after re-warm with current generation");

    fuse::renderer::TaaResolveBlendRejectReason blendReject =
    expectTrue(pass->tryPreflightResolveBlendWeights(resolveDesc, blendReject),
               "tryPreflightResolveBlendWeights passes after warmup");
               "tryPreflightResolveFrame passes after warmup");

               "tryPreflightHistoryReadyForResolve passes after init");

               "pass tryPreflightResolveBlendWeights reject reason is None before warmup");

               "pass tryPreflightResolve passes before warmup resolve");
               "pass tryPreflightResolve skip reason is None");

    expectTrue(pass->trySyncJitterToFrameIndexIfReady(11u, jitterReject),
    expectTrue(pass->tryAdvanceJitterIfReady(jitterReject), "pass tryAdvanceJitterIfReady succeeds");
    expectTrue(pass->jitter().index() != jitterIndexBefore, "pass tryAdvanceJitterIfReady advances jitter");

    expectTrue(pass->tryPreflightHistoryWarmup(reuseReason), "pass tryPreflightHistoryWarmup passes after resolve");
               "pass tryPreflightResolveBlendWeights passes after warmup");

               "pass tryPreflightHistoryReuse fails after invalidate");
    expectTrue(pass->classifyResolveBlendReject(resolveDesc) ==
                   fuse::renderer::TaaResolveBlendRejectReason::None,
               "pass classifyResolveBlendReject is None before init");
               "pass tryPreflightResolveBlendWeights passes before init");
               "pass tryPreflightResolveBlendWeights reject reason is None before init");



    expectTrue(pass->tryPreflightJitterSync(6u, jitterReason),

               "pass tryPreflightResolveBlendWeights reason is None before init");

    expectTrue(pass->tryComputeResolveBlendWeights(resolveDesc, weights, blendReason),
               "pass classifyResolveBlendReject passes before init");
    expectTrue(blendReject == fuse::renderer::TaaResolveBlendRejectReason::None,
               "pass tryPreflightResolveBlendWeights reject reason is None");

    expectTrue(pass->tryComputeResolveBlendWeights(resolveDesc, weights, blendReject),
               "pass tryComputeResolveBlendWeights passes before init");
    expectNear(weights.current, 1.f, 1e-5f, "pass tryCompute warmup current weight is full");
    expectNear(weights.history, 0.f, 1e-5f, "pass tryCompute warmup history weight is zero");

    expectNear(weights.current, 1.f, 1e-5f, "pass tryCompute warmup current weight before init");
    expectNear(weights.history, 0.f, 1e-5f, "pass tryCompute warmup history weight before init");
               "pass tryComputeResolveBlendWeights passes before warmup");
    expectNear(weights.current, 1.f, 1e-5f, "pass tryComputeResolveBlendWeights current is 1 before warmup");

    expectNear(weights.current, 1.f, 1e-5f, "pass tryCompute warmup current weight is full before init");
    expectNear(weights.history, 0.f, 1e-5f, "pass tryCompute warmup history weight is zero before init");

    expectTrue(pass->classifyResolveSkip(resolveDesc) == fuse::renderer::TaaResolveSkipReason::HistoryNotReady,
               "pass classifyResolveSkip reports HistoryNotReady before init");
    expectTrue(pass->classifyResolveSkip(resolveDesc) ==
                   fuse::renderer::TaaResolveSkipReason::HistoryNotReady,







               "pass classifyResolveSkip is HistoryNotReady before init");
    expectTrue(!pass->tryPreflightResolve(resolveDesc, skipReason),
               "pass tryPreflightResolve fails before init");
               "pass tryPreflightResolve skip reason is HistoryNotReady before init");
    expectTrue(!pass->preflightResolve(resolveDesc, &skipReason),
               "pass preflightResolve fails before init");

               "pass tryPreflightResolveBlendWeights passes before first resolve");
               "pass classifyResolveBlendReject is None before first resolve");

               "pass tryComputeResolveBlendWeights passes before first resolve");

    expectTrue(pass->classifyResolveBlendReject(resolveDesc) == fuse::renderer::TaaResolveBlendRejectReason::None,


    fuse::renderer::TaaJitterGuardRejectReason jitterReason = fuse::renderer::TaaJitterGuardRejectReason::None;
    expectTrue(pass->tryPreflightJitterSync(5u, jitterReason), "pass tryPreflightJitterSync passes before init");


    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass try-preflight guard test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass try/classify guard test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass tryPreflight wrapper test");
    if (!bootstrap->status().deviceReady || bootstrap->device() == nullptr) {
        std::printf("SKIP: Vulkan device not available for pass tryPreflight wrapper test\n");
        return;
    }
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass try/classify wrapper test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass try-preflight wrapper test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);
    expectTrue(pass->init(resources), "TaaPass initialized for try-preflight guard test");

               "pass tryPreflightHistoryReadyForResolve passes after init");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::None,
               "pass tryPreflightHistoryReadyForResolve reason is None after init");
               "pass tryPreflightHistoryReuse reason is NotWarm before warmup");

               "pass tryPreflightResolve passes after init");
               "pass tryPreflightResolve skip reason is None after init");

               "pass tryPreflightHistoryReuse reason is None after warmup");
               "pass tryPreflightResolveBlendWeights passes before warmup resolve");
               "pass tryComputeResolveBlendWeights passes before warmup resolve");

    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::None,
               "pass tryPreflightResolve skip reason is None before warmup resolve");

               "pass tryPreflightHistoryReuse fails before warmup");
    expectTrue(!pass->preflightHistoryWarmupAndReuse(0u, &reuseReason),
               "pass preflightHistoryWarmupAndReuse fails before warmup");
               "pass preflightHistoryWarmupAndReuse reason is NotWarm before warmup");

    expectTrue(pass->syncJitterToFrameIndexIfReady(6u), "pass syncIfReady succeeds for frame six");
    expectTrue(pass->jitterAlignedToFrameIndex(6u), "pass jitter aligned after syncIfReady");
    expectTrue(pass->preflightJitterSyncAndNdc(6u, &jitterReason),
               "pass preflightJitterSyncAndNdc passes when aligned");

    expectTrue(pass->preflightHistoryWarmupAndReuse(0u, &reuseReason),
               "pass preflightHistoryWarmupAndReuse passes after warmup");
    expectTrue(!pass->shouldSkipHistoryWarmupAndReuse(0u),
               "pass should not skip warmup/reuse after warmup");

               "pass tryComputeResolveBlendWeights passes after warmup");
    expectNear(weights.current, 0.4f, 1e-5f, "pass tryCompute steady current weight");
    expectNear(weights.history, 0.6f, 1e-5f, "pass tryCompute steady history weight");

    expectTrue(pass->init(resources), "TaaPass initialized for try/classify guard test");

    expectTrue(pass->tryPreflightHistoryReadyForResolve(reuseReason),

    resolveDesc.surfaces.current_frame = reinterpret_cast<void*>(0x10);
    resolveDesc.surfaces.output = reinterpret_cast<void*>(0x20);
    expectTrue(pass->resolveFrame(resolveDesc), "initial resolve warms pass history for try/classify test");
               "pass classifyResolveBlendReject is None before warmup");

    expectTrue(pass->preflightResolve(resolveDesc, &skipReason),
               "pass preflightResolve passes before warmup");


    expectNear(weights.current, 0.25f, 1e-5f, "pass tryCompute steady current weight");
    expectNear(weights.history, 0.75f, 1e-5f, "pass tryCompute steady history weight");



               "pass tryPreflightResolve passes with valid desc after init");



    expectNear(weights.current, 0.35f, 1e-5f, "pass tryCompute steady current weight");
    expectNear(weights.history, 0.65f, 1e-5f, "pass tryCompute steady history weight");

    expectTrue(pass->init(resources), "TaaPass initialized for tryPreflight wrapper test");







               "pass preflightResolve passes after init");



               "pass classifyResolveBlendReject is None after warmup");

    pass->syncJitterToFrameIndexIfReady(6u);
               "pass tryPreflightJitterSync passes after sync");
    expectTrue(pass->jitterAlignedToFrameIndex(6u), "pass jitter aligned after sync for tryPreflight");

               "pass classifyResolveBlendReject returns None before warmup");





    expectNear(weights.current, 0.2f, 1e-5f, "pass tryComputeResolveBlendWeights current matches blend after warmup");
    expectTrue(pass->init(resources), "TaaPass initialized for try/classify wrapper test");






    expectNear(weights.current, 0.3f, 1e-5f, "pass tryCompute steady current weight");
    expectNear(weights.history, 0.7f, 1e-5f, "pass tryCompute steady history weight");

               "pass tryPreflightHistoryReuse reason is StaleGeneration after invalidate");

    fuse::renderer::TaaPassDesc invalidJitterDesc{};
    invalidJitterDesc.width = 128;
    invalidJitterDesc.height = 128;
    invalidJitterDesc.jitter.sequence_length = 0u;
    auto invalidJitterPass = fuse::renderer::TaaPass::create(invalidJitterDesc);
    expectTrue(invalidJitterPass->init(resources), "invalid jitter pass init falls back to default sequence");
    expectTrue(invalidJitterPass->tryPreflightJitterAdvance(jitterReject),
               "pass tryPreflightJitterAdvance passes after fallback sequence");
    expectTrue(!invalidJitterPass->shouldSkipJitterAdvance(),
               "pass should not skip jitter advance after fallback sequence");
               "pass classifyResolveBlendReject passes before warmup");
    expectTrue(!pass->preflightTemporalResolve(resolveDesc, &reuseReason, &blendReason),
               "pass preflightTemporalResolve fails before warmup");
               "pass preflightTemporalResolve reuse reason is NotWarm before warmup");

               "pass preflightHistoryWarmupAndReuse reason is None after warmup");

    expectTrue(pass->tryComputeExpectedResolveBlendWeights(resolveDesc, weights, blendReason),
               "pass tryComputeExpectedResolveBlendWeights passes after warmup");
    expectNear(weights.current, 0.2f, 1e-5f, "pass tryCompute current weight after warmup");
    expectNear(weights.history, 0.8f, 1e-5f, "pass tryCompute history weight after warmup");
    expectTrue(pass->preflightTemporalResolve(resolveDesc, &reuseReason, &blendReason),
               "pass preflightTemporalResolve passes after warmup");
               "pass preflightTemporalResolve reuse reason is None");
               "pass preflightTemporalResolve blend reason is None");

               "pass preflightHistoryWarmupAndReuse fails after invalidate");
               "pass preflightHistoryWarmupAndReuse reason is NotWarm after invalidate");

    pass->resolveFrame(resolveDesc);
               "pass preflightHistoryWarmupAndReuse fails with stale observed generation");
               "pass preflightHistoryWarmupAndReuse reason is StaleGeneration with stale generation");


    expectTrue(pass->shouldSkipHistoryWarmupAndReuse(0u),
               "pass should skip warmup/reuse after invalidate");


    expectTrue(pass->preflightResolve(resolveDesc, &skipReason), "pass preflightResolve passes before warmup");

    const fuse::u32 jitterIndexBefore = pass->jitter().index();


    expectTrue(pass->init(resources), "TaaPass initialized for try-preflight wrapper test");



    expectTrue(pass->preflightResolveTemporal(resolveDesc, &blendReason, &reuseReason),
               "pass preflightResolveTemporal passes before warmup (no history blend)");


    expectTrue(pass->tryPreflightResolve(resolveDesc, skipReason), "pass tryPreflightResolve passes");


    expectNear(weights.current, 0.35f, 1e-5f, "pass tryCompute steady current weight after warmup");
    expectNear(weights.history, 0.65f, 1e-5f, "pass tryCompute steady history weight after warmup");

    pass->syncJitterToFrameIndexIfReady(3u);
    expectTrue(pass->tryPreflightJitterSync(3u, jitterReason),
    expectTrue(pass->jitterAlignedToFrameIndex(3u), "pass jitter aligned after sync for try preflight");

    expectTrue(pass->tryPreflightJitterAdvance(jitterReason), "pass tryPreflightJitterAdvance passes after init");
    expectTrue(!pass->shouldSkipJitterAdvance(), "pass should not skip jitter advance after init");
    expectTrue(pass->advanceJitterIfReady(), "pass advanceJitterIfReady succeeds after try preflight");
    expectTrue(pass->jitter().index() != jitterIndexBefore, "pass advanceJitterIfReady advances jitter");

    expectNear(weights.current, 0.25f, 1e-5f, "pass tryCompute steady current weight after warmup");
    expectNear(weights.history, 0.75f, 1e-5f, "pass tryCompute steady history weight after warmup");

    expectTrue(pass->tryPreflightHistoryTemporal(0u, reuseReason),
               "pass tryPreflightHistoryTemporal passes after warmup");
    expectTrue(!pass->shouldSkipHistoryTemporal(0u), "pass should not skip history temporal after warmup");
               "pass preflightResolveTemporal passes after warmup");
    expectNear(weights.current, 0.2f, 1e-5f, "pass tryCompute steady current weight");
    expectNear(weights.history, 0.8f, 1e-5f, "pass tryCompute steady history weight");

    expectTrue(!pass->tryPreflightHistoryTemporal(0u, reuseReason),
               "pass tryPreflightHistoryTemporal fails after invalidate");
               "pass tryPreflightHistoryTemporal reason is StaleGeneration after invalidate");



    resolveDesc.observed_history_generation = 0u;
               "pass tryPreflightHistoryReuse passes after warmup with current generation");



    expectTrue(pass->syncJitterToFrameIndexIfReady(6u), "pass syncJitterToFrameIndexIfReady succeeds");
    expectTrue(pass->tryPreflightJitterSync(6u, jitterReject),
    expectTrue(pass->canSyncJitterToFrameIndex(6u), "pass canSyncJitterToFrameIndex after sync");



               "pass tryPreflightResolve skip reason is None before warmup");

    resolveDesc.observed_history_generation = pass->historyInvalidateGeneration();
    expectTrue(pass->tryPreflightHistoryReuse(resolveDesc.observed_history_generation, reuseReason),


    expectTrue(!pass->tryPreflightHistoryReuse(resolveDesc.observed_history_generation, reuseReason),

    expectTrue(pass->classifyResolveSkip(resolveDesc) == fuse::renderer::TaaResolveSkipReason::None,
               "pass classifyResolveSkip returns None for valid resolve");





    expectTrue(pass->classifyHistoryReuseBlock(0u) ==
                   fuse::renderer::TaaHistoryReuseBlockReason::StaleGeneration,
               "pass classifyHistoryReuseBlock is StaleGeneration after invalidate");
               "pass classifyResolveSkip is None before first resolve");
               "pass tryPreflightResolve passes before first resolve");
               "pass tryPreflightResolve skip reason is None before first resolve");

               "pass classifyResolveBlendReject is None before warmup resolve");






               "pass classifyResolveSkip is None for valid resolve desc");
               "pass tryPreflightResolve passes for valid resolve desc");

    expectTrue(pass->classifyHistoryReuseBlock(0u) == fuse::renderer::TaaHistoryReuseBlockReason::None,
               "pass classifyHistoryReuseBlock is None after warmup");


               "pass classifyResolveSkip passes after init");

    fuse::renderer::TaaHistoryReuseBlockReason reuseReason = fuse::renderer::TaaHistoryReuseBlockReason::None;


    expectTrue(pass->tryPreflightResolve(resolveDesc, skipReason), "pass tryPreflightResolve passes after init");

    expectTrue(pass->resolveWouldReuseHistory(resolveDesc), "pass would reuse history after warmup");




               "pass classifyResolveSkip reports None after init");



               "pass tryPreflightHistoryReuse fails before warmup resolve");



               "pass classifyResolveBlendReject passes after warmup");

               "pass classifyHistoryReuseBlock reports StaleGeneration after invalidate");

               "pass classifyResolveSkip is None with valid desc after init");






    expectTrue(pass->tryPreflightResolve(resolveDesc, skipReason),

    expectTrue(!pass->tryPreflightHistoryReuse(0u, reuseReason),
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::NotWarm,

               "pass classifyResolveSkip is None after init");

    expectTrue(pass->resolveFrame(resolveDesc), "initial resolve warms pass history");
    expectTrue(pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse passes after warmup");

    expectTrue(pass->tryComputeResolveBlendWeights(resolveDesc, weights, blendReason),

    expectTrue(pass->tryComputeResolveBlendWeights(resolveDesc, weights, blendReject),
    expectNear(weights.current, 0.3f, 1e-5f, "pass tryCompute steady current weight after warmup");
    expectNear(weights.history, 0.7f, 1e-5f, "pass tryCompute steady history weight after warmup");
    expectTrue(pass->classifyResolveBlendReject(resolveDesc) ==
                   fuse::renderer::TaaResolveBlendRejectReason::None,
    expectTrue(pass->resolveWouldReuseHistory(resolveDesc),
               "pass resolveWouldReuseHistory after warmup");

    pass->invalidateHistory();

    pass->syncJitterToFrameIndexIfReady(11u);
    expectTrue(pass->tryPreflightJitterSync(11u, jitterReject),
    expectTrue(pass->jitterAlignedToFrameIndex(11u), "pass jitter aligned after sync");


    fuse::renderer::TaaResolveDesc resolveDesc{};
    resolveDesc.width = 128;
    resolveDesc.height = 128;
    resolveDesc.params = passDesc.params;

    fuse::renderer::TaaResolveBlendRejectReason blendReason =
        fuse::renderer::TaaResolveBlendRejectReason::None;
    expectTrue(pass->tryPreflightResolveBlendWeights(resolveDesc, blendReason),
               "pass tryPreflightResolveBlendWeights passes before warmup");
    fuse::renderer::TaaBlendWeights weights{};
               "pass tryComputeResolveBlendWeights passes before warmup");
    expectNear(weights.current, 1.f, 1e-5f, "pass tryCompute warmup current weight is full");

    fuse::renderer::TaaResolveSkipReason skipReason = fuse::renderer::TaaResolveSkipReason::None;


    expectTrue(pass->classifyHistoryReuseBlock(0u) ==
                   fuse::renderer::TaaHistoryReuseBlockReason::StaleGeneration,
               "pass classifyHistoryReuseBlock is StaleGeneration after invalidate");
    expectTrue(!pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse fails after invalidate");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::StaleGeneration,
               "pass tryPreflightHistoryReuse reason is StaleGeneration after invalidate");

    fuse::renderer::TaaPassDesc zeroWidthDesc{};
    zeroWidthDesc.width = 0;
    zeroWidthDesc.height = 128;
    auto zeroPass = fuse::renderer::TaaPass::create(zeroWidthDesc);
    expectTrue(!zeroPass->shouldSkipJitterAdvance(),
               "zero-width pass jitter advance still valid when sequence is valid");
    expectTrue(zeroPass->classifyJitterNdcReject() ==
                   fuse::renderer::TaaJitterGuardRejectReason::InvalidViewport,
               "zero-width pass classifyJitterNdcReject is InvalidViewport");
    expectTrue(!zeroPass->tryPreflightJitterNdc(jitterReject),
    fuse::renderer::TaaPassDesc zeroSeqDesc{};
    zeroSeqDesc.width = 128;
    zeroSeqDesc.height = 128;
    zeroSeqDesc.jitter.sequence_length = 0u;
    auto zeroSeqPass = fuse::renderer::TaaPass::create(zeroSeqDesc);
    expectTrue(zeroSeqPass->classifyJitterSyncReject() ==
                   fuse::renderer::TaaJitterGuardRejectReason::None,
               "zero-length desc falls back so sync classify still passes");
    expectTrue(!zeroSeqPass->shouldSkipJitterAdvance(),
               "zero-length desc falls back so jitter advance is not skipped");
    expectTrue(zeroSeqPass->advanceJitterIfReady(),
               "zero-length desc falls back so advanceJitterIfReady succeeds");

    auto zeroWidthPass = fuse::renderer::TaaPass::create(zeroWidthDesc);
    expectTrue(zeroWidthPass->classifyJitterNdcReject() ==
    expectTrue(!zeroWidthPass->tryPreflightJitterNdc(jitterReject),
    expectTrue(zeroPass->classifyJitterNdcReject() == fuse::renderer::TaaJitterGuardRejectReason::InvalidViewport,
               "zero-width pass classifyJitterNdcReject reports InvalidViewport");
               "zero-width pass tryPreflightJitterNdc fails");
    expectTrue(jitterReject == fuse::renderer::TaaJitterGuardRejectReason::InvalidViewport,
               "zero-width pass tryPreflightJitterNdc reject reason is InvalidViewport");

               "zero-width pass tryPreflightJitterNdc rejects");
    expectTrue(zeroPass->shouldSkipJitterNdc(), "zero-width pass should skip NDC jitter");
    expectTrue(!zeroPass->shouldSkipJitterAdvance(), "zero-width pass should not skip jitter advance");
    expectTrue(!zeroPass->tryPreflightJitterNdc(jitterReason),
    expectTrue(jitterReason == fuse::renderer::TaaJitterGuardRejectReason::InvalidViewport,
               "zero-width pass tryPreflightJitterNdc reason is InvalidViewport");
    expectTrue(zeroPass->shouldSkipJitterAdvance() == false,
               "zero-width pass jitter advance still valid for default sequence");
    expectTrue(pass->tryPreflightHistoryReadyForResolve(historyReason),
    expectTrue(historyReason == fuse::renderer::TaaHistoryReuseBlockReason::None,
    expectTrue(!pass->tryPreflightHistoryReuse(0u, historyReason),
    expectTrue(historyReason == fuse::renderer::TaaHistoryReuseBlockReason::NotWarm,

    expectTrue(pass->tryPreflightResolve(resolveDesc, skipReason),
    expectTrue(pass->resolveFrame(resolveDesc), "initial resolve warms pass history");

    expectTrue(pass->tryPreflightHistoryReuse(0u, historyReason),
               "pass tryPreflightHistoryReuse passes after warmup");

    expectTrue(pass->tryComputeResolveBlendWeights(resolveDesc, weights, blendReason),
    expectNear(weights.current, 0.35f, 1e-5f, "pass tryCompute steady current weight after warmup");
    expectNear(weights.history, 0.65f, 1e-5f, "pass tryCompute steady history weight after warmup");

    pass->syncJitterToFrameIndex(9u);
    expectTrue(pass->tryPreflightJitterSync(9u, jitterReason),
               "pass tryPreflightJitterSync passes after sync");
    expectTrue(pass->jitterAlignedToFrameIndex(9u), "pass jitter aligned after sync");

    expectTrue(historyReason == fuse::renderer::TaaHistoryReuseBlockReason::StaleGeneration,

    expectTrue(pass->preflightJitterAdvance(), "pass preflightJitterAdvance passes after init");
    expectTrue(pass->tryPreflightJitterAdvance(jitterReason),
               "pass tryPreflightJitterAdvance passes after init");
    expectTrue(jitterReason == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass tryPreflightJitterAdvance reason is None after init");

    expectTrue(!zeroWidthPass->tryPreflightJitterNdc(jitterReason),
    expectTrue(zeroPass->tryPreflightJitterSync(0u, jitterReason),
               "zero-width pass tryPreflightJitterSync still valid for sequence");

    resolveDesc.width = 0u;
    expectTrue(!pass->tryPreflightResolve(resolveDesc, skipReason),
               "pass tryPreflightResolve fails with invalid dimensions");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::InvalidDimensions,
               "pass tryPreflightResolve skip reason is InvalidDimensions");
    expectTrue(!zeroPass->ndcOffsetForFrameIndexIfReady(0u, ndcOut),
               "zero-width pass ndcOffsetForFrameIndexIfReady fails");

    pass->destroy();

    invalidJitterPass->destroy();
    expectTrue(!zeroPass->ndcJitterForFrameIndexIfReady(0u, ndcOut),
               "zero-width pass ndcJitterForFrameIndexIfReady fails");

    expectTrue(!pass->shouldSkipJitterAdvance(), "pass should not skip jitter advance after init");
    expectTrue(pass->tryPreflightJitterAdvance(jitterReject),
    expectTrue(jitterReject == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass tryPreflightJitterAdvance reject reason is None after init");

               "zero-width pass classifyJitterNdcReject returns InvalidViewport");


    expectTrue(!zeroPass->preflightJitterSyncAndNdc(0u, &jitterReason),
               "zero-width pass preflightJitterSyncAndNdc fails");
               "zero-width pass preflightJitterSyncAndNdc reason is InvalidViewport");


    expectTrue(!pass->preflightResolveFrame(resolveDesc, &skipReason, &blendReason),
               "preflightResolveFrame fails with invalid dimensions");
               "preflightResolveFrame skip reason is InvalidDimensions");
    expectTrue(pass->shouldSkipResolveFrame(resolveDesc),
               "shouldSkipResolveFrame true with invalid dimensions");

    expectTrue(zeroPass->shouldSkipJitterSyncAndNdc(0u),
               "zero-width pass should skip jitter sync+NDC");
               "zero-width pass sync+NDC reject reason is InvalidViewport");


    expectTrue(zeroPass->tryPreflightJitterSync(0u, jitterReject),
               "zero-width pass should not skip jitter advance when sequence is valid");

    expectTrue(zeroPass->shouldSkipJitterFrame(0u), "zero-width pass should skip jitter frame");
               "zero-width pass jitter sync still valid for sequence");

    expectTrue(zeroPass->preflightJitterSync(0u), "zero-width pass jitter sync still valid for sequence");



               "zero-width pass still allows jitter advance when sequence is valid");

    expectTrue(pass->classifyResolveSkip(resolveDesc) ==
                   fuse::renderer::TaaResolveSkipReason::InvalidDimensions,
               "pass classifyResolveSkip is InvalidDimensions for zero width");
               "pass tryPreflightResolve fails for zero width");


    expectTrue(pass->classifyJitterSyncReject() ==
                   fuse::renderer::classifyTaaJitterSyncReject(pass->jitter().sequenceLength()),
               "pass classifyJitterSyncReject matches free helper");
    expectTrue(pass->classifyJitterNdcReject() ==
                   fuse::renderer::classifyTaaJitterNdcReject(passDesc.width, passDesc.height,
                                                              pass->jitter().sequenceLength()),
               "pass classifyJitterNdcReject matches free helper");
    expectTrue(pass->classifyJitterAdvanceReject() ==
                   fuse::renderer::classifyTaaJitterAdvanceReject(pass->jitter().sequenceLength()),
               "pass classifyJitterAdvanceReject matches free helper");
    expectTrue(pass->classifyResolveBlendReject(resolveDesc) ==
                   fuse::renderer::classifyTaaResolveBlendReject(resolveDesc, pass->history()),
               "pass classifyResolveBlendReject matches free helper");
                   fuse::renderer::classifyTaaResolveSkip(resolveDesc, pass->history()),
               "pass classifyResolveSkip matches free helper");
    expectTrue(pass->tryPreflightJitterSync(3u, jitterReject),
               "pass tryPreflightJitterSync succeeds after init");
    expectTrue(fuse::renderer::tryPreflightTaaJitterSync(3u, pass->jitter().sequenceLength(), jitterReject),
               "free tryPreflightJitterSync matches pass sequence length");







    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testTaaPassShouldSkipGuardWrappers() {
    fuse::renderer::TaaPassDesc passDesc{};
    passDesc.width = 128;
    passDesc.height = 128;
    passDesc.params.blend_factor = 0.2f;

    auto pass = fuse::renderer::TaaPass::create(passDesc);
    expectTrue(!pass->historyWarmupComplete(), "pass warmup not complete before init");
    expectTrue(pass->shouldSkipJitterAlignment(3u), "pass jitter misaligned before sync");
    expectTrue(pass->preflightJitterAdvance(), "pass preflightJitterAdvance passes with default sequence");

    fuse::renderer::TaaHistoryWarmupBlockReason warmupReason =
        fuse::renderer::TaaHistoryWarmupBlockReason::None;
    expectTrue(!pass->preflightHistoryWarmup(&warmupReason),
               "pass warmup preflight fails before init");
    expectTrue(warmupReason == fuse::renderer::TaaHistoryWarmupBlockReason::NotReady,
               "pass warmup preflight reason is NotReady before init");

    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass composite preflight test");


    expectTrue(pass->init(resources), "TaaPass initialized for composite preflight test");
    expectTrue(!pass->historyWarmupComplete(), "pass warmup not complete after init");
    expectTrue(!pass->tryPreflightHistoryWarmup(warmupReason),
               "pass tryPreflightHistoryWarmup fails before first resolve");
    expectTrue(warmupReason == fuse::renderer::TaaHistoryWarmupBlockReason::NeedsWarmup,
               "pass warmup preflight reason is NeedsWarmup after init");

    fuse::renderer::TaaJitterGuardRejectReason jitterReason =
    expectTrue(pass->tryPreflightJitterSync(4u, jitterReason), "pass tryPreflightJitterSync succeeds");
    expectTrue(pass->syncJitterToFrameIndexIfReady(4u), "pass syncJitterToFrameIndexIfReady succeeds");
    expectTrue(pass->preflightJitterAlignment(4u, &jitterReason),
               "pass preflightJitterAlignment passes after sync");
    expectTrue(!pass->shouldSkipJitterAdvance(), "pass should not skip jitter advance");
    expectTrue(pass->preflightJitterAdvance(&jitterReason), "pass preflightJitterAdvance passes");

    fuse::renderer::TaaResolveDesc resolveDesc{};
    resolveDesc.width = 128;
    resolveDesc.height = 128;
    resolveDesc.surfaces.current_frame = reinterpret_cast<void*>(0x10);
    resolveDesc.surfaces.output = reinterpret_cast<void*>(0x20);
    resolveDesc.params = passDesc.params;

    fuse::renderer::TaaResolveWithBlendRejectReason compositeReason =
    expectTrue(pass->preflightResolveWithBlend(resolveDesc, &compositeReason),
               "pass combined preflight passes before warmup resolve");
    expectTrue(!pass->shouldSkipResolveWithBlend(resolveDesc),
               "pass should not skip combined preflight before warmup");

    fuse::renderer::TaaResolveBlendRejectReason blendReason =
        fuse::renderer::TaaResolveBlendRejectReason::None;
    expectTrue(pass->tryPreflightResolveBlendWeights(resolveDesc, blendReason),
               "pass tryPreflightResolveBlendWeights passes before warmup");

    expectTrue(pass->resolveFrame(resolveDesc), "initial resolve completes pass warmup");
    expectTrue(pass->historyWarmupComplete(), "pass warmup complete after resolve");
    expectTrue(pass->tryPreflightHistoryWarmup(warmupReason),
               "pass tryPreflightHistoryWarmup passes after resolve");
               "pass combined preflight passes after warmup");

    pass->destroy();

void testTaaPassShouldSkipGuardWrappers() {
    fuse::renderer::TaaPassDesc passDesc{};
    passDesc.params = desc.params;
    auto pass = fuse::renderer::TaaPass::create(passDesc);
    expectTrue(pass->init(resources), "TaaPass initialized for blend weights-if-ready test");
    expectTrue(pass->resolveBlendWeightsReady(desc), "pass resolveBlendWeightsReady before warmup");
    expectTrue(pass->computeResolveBlendWeightsIfReady(desc, weights, &rejectReason),
               "pass computeResolveBlendWeightsIfReady before warmup");
    expectTrue(!pass->preflightHistoryWarmup(), "pass preflightHistoryWarmup fails before first resolve");
    expectTrue(pass->shouldSkipHistoryWarmup(), "pass shouldSkipHistoryWarmup before first resolve");
    expectTrue(!pass->historyWarmupComplete(), "pass historyWarmupComplete false before resolve");
    expectTrue(pass->preflightJitterNdc(), "pass preflightJitterNdc passes for valid viewport");
    expectTrue(!pass->shouldSkipJitterNdc(), "pass shouldSkipJitterNdc false for valid viewport");

    fuse::math::Vec2 jitterNdc{};
    expectTrue(pass->tryCurrentJitterNdcIfReady(jitterNdc, jitterReject),
               "pass tryCurrentJitterNdcIfReady succeeds for valid viewport");
               "pass tryCurrentJitterNdcIfReady reject reason is None");
    expectTrue(pass->trySyncJitterToFrameIndexIfReady(7u, jitterReject),
               "pass trySyncJitterToFrameIndexIfReady succeeds");


void testTaaPassWarmupAndJitterPreflightWrappers() {
    passDesc.height = 128;

        fuse::renderer::TaaHistoryWarmupRejectReason::None;
    expectTrue(!pass->preflightHistoryWarmup(&warmupReason), "pass warmup preflight fails before init");
               "pass warmup preflight NotReady before init");

    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass warmup/jitter wrapper test");


    expectTrue(pass->init(resources), "TaaPass initialized for warmup/jitter wrapper test");
    expectTrue(pass->shouldSkipHistoryWarmup(), "pass should skip history warmup after init");
    expectTrue(!pass->preflightHistoryWarmup(&warmupReason), "pass warmup preflight fails after init");
               "pass warmup preflight NeedsWarmup after init");
    expectTrue(history.init(resources, historyDesc), "history ready for reuse-blend preflight test");
    desc.params.blend_factor = 0.3f;

    expectTrue(fuse::renderer::shouldSkipTaaResolveReuseAndBlend(desc, history, 0u),
               "unwarmed history should skip reuse-and-blend");
    expectTrue(fuse::renderer::preflightTaaResolveBlendWeights(desc, history),
               "blend preflight alone still passes for warmup path");

    expectTrue(!fuse::renderer::shouldSkipTaaResolveReuseAndBlend(desc, history, 0u),
               "warmed history should not skip reuse-and-blend");
    expectTrue(fuse::renderer::tryPreflightTaaResolveReuseAndBlend(desc, history, 0u, rejectReason),
               "tryPreflightTaaResolveReuseAndBlend passes after warmup");
    expectTrue(rejectReason == fuse::renderer::TaaResolveReuseBlendRejectReason::None,
               "tryPreflight reuse-blend reject reason is None");

    history.invalidateHistory();
               "stale generation should skip reuse-and-blend");
    expectTrue(fuse::renderer::classifyTaaResolveReuseBlendReject(desc, history, 0u) ==
               "stale generation reuse-blend classify is HistoryReuseBlocked");
    expectTrue(history.init(resources, historyDesc), "history ready for temporal resolve guard test");


    expectTrue(fuse::renderer::shouldSkipTaaTemporalResolveGuards(desc, history, 0u),
               "unwarmed history should skip temporal resolve guards");
    expectTrue(!fuse::renderer::preflightTaaTemporalResolveGuards(desc, history, 0u, &reuseReason, &blendReason),
               "temporal guards fail before warmup");
               "temporal guard reuse reason is NotWarm before warmup");
               "temporal guard blend reason is None before warmup");

    expectTrue(!fuse::renderer::shouldSkipTaaTemporalResolveGuards(desc, history, 0u),
               "warmed history should not skip temporal resolve guards");
    expectTrue(fuse::renderer::preflightTaaTemporalResolveGuards(desc, history, 0u, &reuseReason, &blendReason),
               "temporal guards pass after warmup");
               "temporal guard reuse reason is None after warmup");

               "stale generation should skip temporal resolve guards");
               "temporal guards fail after invalidate");
               "temporal guard reuse reason is StaleGeneration after invalidate");

    history.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testTaaPassWarmupAndJitterPreflights() {

    expectTrue(!pass->historyWarmupComplete(), "pass warmup not complete before init");
    expectTrue(pass->shouldSkipHistoryWarmup(), "pass should skip warmup completion before init");

    fuse::math::Vec2 pixelOut{};
    expectTrue(pass->currentJitterPixelOffsetIfReady(pixelOut), "pass pixel offset ready before init");
    expectTrue(!pass->shouldSkipJitterSync(4u), "pass should not skip jitter sync");
    expectTrue(!pass->shouldSkipJitterNdc(), "pass should not skip jitter NDC");
void testTaaPassWarmupAndCompositeGuards() {
    passDesc.params.blend_factor = 0.2f;

    expectTrue(pass->shouldSkipHistoryWarmup(), "pass should skip warmup preflight before init");
    expectTrue(pass->classifyHistoryWarmupBlock() ==
                   fuse::renderer::TaaHistoryWarmupBlockReason::NotReady,
               "pass warmup block is NotReady before init");

    expectTrue(pass->tryPreflightJitterNdc(jitterReject), "pass tryPreflightJitterNdc before init");
    expectTrue(pass->preflightJitterNdc(), "pass preflightJitterNdc before init");
    expectTrue(pass->jitterNeedsResync(5u), "pass jitter needs resync when frame counter drifts");
    expectTrue(!pass->jitterNeedsResync(0u), "default jitter is aligned to frame zero");
    expectTrue(pass->tryPreflightJitterSync(0u, jitterReject), "pass tryPreflightJitterSync passes");
    expectTrue(pass->syncJitterToFrameIndexIfReady(0u), "pass syncJitterToFrameIndexIfReady succeeds");
    expectTrue(!pass->jitterNeedsResync(0u), "pass jitter aligned after sync");

    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass warmup/jitter preflight test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass composite guard test");


    expectTrue(pass->init(resources), "TaaPass initialized for warmup/jitter preflight test");

    fuse::renderer::TaaHistoryWarmupBlockReason warmupReason = fuse::renderer::TaaHistoryWarmupBlockReason::None;
    expectTrue(!pass->historyWarmupComplete(), "pass warmup not complete after init");
    expectTrue(pass->shouldSkipHistoryWarmup(), "pass should skip warmup completion after init");
    expectTrue(!pass->preflightHistoryWarmup(&warmupReason),
               "pass warmup preflight fails after init");
    expectTrue(warmupReason == fuse::renderer::TaaHistoryWarmupBlockReason::NeedsWarmup,
               "pass warmup preflight reason is NeedsWarmup");
    expectTrue(pass->init(resources), "TaaPass initialized for composite guard test");
    expectTrue(pass->shouldSkipHistoryWarmup(), "pass should skip warmup after init");
                   fuse::renderer::TaaHistoryWarmupBlockReason::NeedsWarmup,
               "pass warmup block is NeedsWarmup after init");
void testTaaPassTryAndShouldSkipWrappers() {

    expectTrue(!pass->shouldSkipJitterSync(), "pass should not skip jitter sync with default sequence");
    expectTrue(!pass->shouldSkipJitterNdc(), "pass should not skip jitter NDC with valid viewport");

    expectTrue(pass->tryPreflightJitterNdc(jitterReject), "pass tryPreflightJitterNdc passes before init");
               "pass tryPreflightJitterNdc reject reason is None");
void testTaaPassTemporalAndJitterGuardWrappers() {
    passDesc.width = 128;

    expectTrue(!pass->isHistoryWarm(), "pass history not warm before init");

    fuse::renderer::TaaJitterGuardRejectReason jitterReject =
    expectTrue(pass->preflightJitterSync(3u, &jitterReject), "pass preflightJitterSync before init");
    expectTrue(!pass->shouldSkipJitterSync(3u), "pass should not skip jitter sync before init");
void testTaaPassDeepenFrameGuards() {

    expectTrue(pass->historyWarmupPhase() == fuse::renderer::TaaHistoryWarmupPhase::NotReady,
               "pass warmup phase is NotReady before init");
    expectTrue(!pass->shouldSkipJitterSync(), "pass jitter sync not skipped before init");
    expectTrue(!pass->shouldSkipJitterNdc(), "pass jitter NDC not skipped before init");

    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass temporal/jitter wrapper test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass frame guards test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);
    expectTrue(pass->init(resources), "TaaPass initialized for temporal/jitter wrapper test");

    expectTrue(pass->preflightJitterNdc(128u, 128u, &jitterReject), "pass preflightJitterNdc succeeds");
    expectTrue(pass->preflightJitterNdcIfReady(&jitterReject), "pass preflightJitterNdcIfReady succeeds");
    expectTrue(!pass->shouldSkipJitterNdc(), "pass should not skip jitter NDC after init");
    expectTrue(pass->init(resources), "TaaPass initialized for frame guards test");
    expectTrue(pass->historyWarmupPhase() == fuse::renderer::TaaHistoryWarmupPhase::Cold,
               "pass warmup phase is Cold after init");

    fuse::renderer::TaaResolveDesc resolveDesc{};
    resolveDesc.width = 128;
    resolveDesc.height = 128;

    fuse::renderer::TaaPassDesc passDesc{};
    passDesc.width = 64;
    passDesc.height = 64;
    passDesc.params.blend_factor = 0.25f;

    auto pass = fuse::renderer::TaaPass::create(passDesc);

    expectTrue(pass->jitterNeedsResyncToFrameIndex(4u), "pass jitter needs resync before sync");
    fuse::renderer::TaaJitterSyncRejectReason syncReason = fuse::renderer::TaaJitterSyncRejectReason::None;
    expectTrue(pass->trySyncJitterToFrameIndexIfReady(4u, syncReason),
               "pass trySyncJitterToFrameIndexIfReady succeeds");
    expectTrue(!pass->jitterNeedsResyncToFrameIndex(4u), "pass jitter aligned after trySync");

    expectTrue(pass->shouldSkipHistoryReuse(0u), "pass should skip reuse before warmup");

    resolveDesc.width = 64;
    resolveDesc.height = 64;
    resolveDesc.surfaces.current_frame = reinterpret_cast<void*>(0x10);
    resolveDesc.surfaces.output = reinterpret_cast<void*>(0x20);
    resolveDesc.params = passDesc.params;

    fuse::renderer::TaaResolveBlendRejectReason blendReject =
        fuse::renderer::TaaResolveBlendRejectReason::None;
    expectTrue(pass->classifyResolveBlendReject(resolveDesc) == fuse::renderer::TaaResolveBlendRejectReason::None,
               "pass classifyResolveBlendReject passes before warmup");
    expectTrue(pass->tryPreflightResolveBlendWeights(resolveDesc, blendReject),
               "pass tryPreflightResolveBlendWeights passes before warmup");
    expectTrue(blendReject == fuse::renderer::TaaResolveBlendRejectReason::None,
               "pass tryPreflightResolveBlendWeights reject reason is None before warmup");

    fuse::renderer::TaaBlendWeights weights{};
    expectTrue(pass->tryComputeResolveBlendWeights(resolveDesc, weights, blendReject),
               "pass tryComputeResolveBlendWeights passes before warmup");
    expectNear(weights.current, 1.f, 1e-5f, "pass tryCompute warmup current weight is full");
    expectNear(weights.history, 0.f, 1e-5f, "pass tryCompute warmup history weight is zero");

    fuse::renderer::TaaResolveSkipReason skipReason = fuse::renderer::TaaResolveSkipReason::None;
    expectTrue(pass->tryPreflightResolve(resolveDesc, skipReason),
               "pass tryPreflightResolve passes after init with valid desc");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::None,
               "pass tryPreflightResolve skip reason is None after init");
    expectTrue(pass->classifyResolveSkip(resolveDesc) == fuse::renderer::TaaResolveSkipReason::None,
               "pass classifyResolveSkip matches tryPreflight after init");

    expectTrue(pass->resolveFrame(resolveDesc), "initial resolve warms pass history");

    expectTrue(!pass->shouldSkipResolve(resolveDesc), "pass should not skip valid resolve");
    expectTrue(pass->tryPreflightResolve(resolveDesc, skipReason), "pass tryPreflightResolve passes");

    expectTrue(!pass->shouldSkipHistoryWarmup(), "pass should not skip warmup after resolve");
    expectTrue(pass->historyWarmupComplete(), "pass history warmup complete after resolve");
    expectTrue(pass->tryPreflightHistoryWarmup(warmupReason), "pass tryPreflightHistoryWarmup passes");
    expectTrue(warmupReason == fuse::renderer::TaaHistoryWarmupBlockReason::None,
               "pass tryPreflightHistoryWarmup reason is None after resolve");

    fuse::renderer::TaaHistoryReuseBlockReason reuseReason =
        fuse::renderer::TaaHistoryReuseBlockReason::None;
    expectTrue(pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse passes after warmup");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::None,
               "pass tryPreflightHistoryReuse reason is None after warmup");
               "pass tryPreflightResolveBlendWeights passes after warmup");
               "pass tryComputeResolveBlendWeights passes after warmup");
    expectNear(weights.current, 0.3f, 1e-5f, "pass tryCompute steady current weight");
    expectNear(weights.history, 0.7f, 1e-5f, "pass tryCompute steady history weight");
               "pass tryPreflightResolve passes after warmup");
               "pass tryPreflightResolve skip reason is None after warmup");
               "pass classifyResolveSkip matches tryPreflight after warmup");

    pass->invalidateHistory();
    expectTrue(!pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse fails after invalidate");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::StaleGeneration,
               "pass tryPreflightHistoryReuse reason is StaleGeneration after invalidate");
    expectTrue(pass->classifyHistoryReuseBlock(0u) ==
                   fuse::renderer::TaaHistoryReuseBlockReason::StaleGeneration,
               "pass classifyHistoryReuseBlock matches tryPreflight after invalidate");

    expectTrue(pass->classifyJitterSyncReject() ==
                   fuse::renderer::classifyTaaJitterSyncReject(pass->jitter().sequenceLength()),
               "pass classifyJitterSyncReject matches free helper");

    resolveDesc.width = 0u;
    expectTrue(pass->classifyResolveSkip(resolveDesc) == fuse::renderer::TaaResolveSkipReason::InvalidDimensions,
               "pass classifyResolveSkip reports InvalidDimensions");
    expectTrue(!pass->tryPreflightResolve(resolveDesc, skipReason),
               "pass tryPreflightResolve fails with invalid dimensions");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::InvalidDimensions,
               "pass tryPreflightResolve skip reason is InvalidDimensions");
    fuse::renderer::TaaResolveBlendRejectReason blendReason =
    expectTrue(pass->preflightResolveWithBlend(resolveDesc, &skipReason, &blendReason),
               "pass resolve-with-blend preflight passes before first resolve");

    expectTrue(pass->historyWarmupComplete(), "pass warmup complete after first resolve");
    expectNear(pass->historyWarmupProgress(), 1.f, 1e-5f, "pass warmup progress is one after first resolve");
    expectTrue(pass->historyReuseReady(0u), "pass history reuse-ready after first resolve");
    expectTrue(pass->lastResolveStatsBlendConsistent(resolveDesc),
               "pass last resolve stats blend is consistent");

    expectTrue(pass->resolveFrame(resolveDesc), "second resolve succeeds");
               "pass second resolve stats blend is consistent");
    fuse::renderer::TaaResolveFramePreflight framePreflight{};
    expectTrue(pass->preflightResolveFrame(resolveDesc, &framePreflight),
               "pass preflightResolveFrame passes before first resolve");
    expectTrue(framePreflight.canProceed, "pass resolve frame preflight canProceed before first resolve");

    expectTrue(pass->preflightJitterSync(4u, &syncReason), "pass preflightJitterSync passes");
    expectTrue(pass->trySyncJitterToFrameIndexIfReady(4u, &syncReason),
    expectTrue(!pass->needsJitterResync(4u), "pass jitter aligned after trySync");
    expectTrue(pass->jitterAlignedToFrameIndex(4u), "pass jitterAlignedToFrameIndex after trySync");

    expectTrue(pass->tryCanBeginTemporalReuse(0u, &reuseReason),
               "pass tryCanBeginTemporalReuse passes after warmup");

    expectTrue(pass->tryExpectedResolveBlendWeights(resolveDesc, weights, &blendReason),
               "pass tryExpectedResolveBlendWeights succeeds after warmup");
    expectNear(weights.current, 0.35f, 1e-5f, "pass tryExpectedResolveBlendWeights current weight");
    expectNear(weights.history, 0.65f, 1e-5f, "pass tryExpectedResolveBlendWeights history weight");
    expectTrue(!pass->canSampleHistoryForResolve(resolveDesc),
               "pass cannot sample history before warmup");
    expectTrue(pass->shouldSkipHistoryBlendAtResolve(resolveDesc),
               "pass skips history blend before warmup");
    expectTrue(!pass->canApplyHistoryBlendAtResolve(resolveDesc),
               "pass cannot apply history blend before warmup");

    fuse::renderer::TaaHistoryReuseBlockReason reuseReason = fuse::renderer::TaaHistoryReuseBlockReason::None;
    expectTrue(!pass->tryPreflightHistoryReuseForResolve(resolveDesc, reuseReason),
    expectTrue(pass->shouldSkipHistoryReuseForResolve(resolveDesc),
               "pass should skip resolve reuse before warmup");
    expectTrue(!pass->preflightHistoryReuseForResolve(resolveDesc, &reuseReason),
               "pass resolve reuse preflight fails before warmup");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::NotWarm,
               "pass resolve reuse preflight reason is NotWarm before warmup");

    expectTrue(!pass->shouldSkipHistoryReuse(0u), "pass does not skip reuse after warmup");
    expectTrue(pass->canSampleHistoryForResolve(resolveDesc),
               "pass can sample history after warmup");
    expectTrue(pass->tryPreflightHistoryReuseForResolve(resolveDesc, reuseReason),
               "pass resolve reuse preflight passes after warmup");
    expectTrue(!pass->shouldSkipHistoryBlendAtResolve(resolveDesc),
               "pass does not skip history blend after warmup");
    expectTrue(pass->canApplyHistoryBlendAtResolve(resolveDesc),
               "pass can apply history blend after warmup");

    pass->destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testTaaPassShouldSkipGuardWrappers() {

    expectTrue(pass->shouldSkipHistoryWarmup(), "pass should skip warmup before init");
    expectTrue(pass->shouldSkipHistoryResolve(), "pass should skip resolve before init");
    expectTrue(!pass->historyReadyForResolve(), "pass history not ready for resolve before init");
    expectTrue(!pass->shouldSkipJitterSync(4u), "pass should not skip jitter sync before init");
    expectTrue(!pass->shouldSkipJitterNdc(), "pass should not skip NDC jitter before init");
    expectTrue(pass->preflightJitterNdc(), "pass preflightJitterNdc passes before init");

    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass should-skip wrapper test");


    expectTrue(pass->init(resources), "TaaPass initialized for should-skip wrapper test");
    expectTrue(pass->historyReadyForResolve(), "pass history ready for resolve after init");
    expectTrue(!pass->shouldSkipHistoryResolve(), "pass should not skip resolve after init");
    expectTrue(pass->shouldSkipHistoryWarmup(), "pass should skip warmup before first resolve");

    expectTrue(pass->resolveBlendReady(resolveDesc),
               "pass resolve blend ready before first resolve");
               "pass resolve blend ready after warmup");
    expectTrue(pass->resolveFrame(resolveDesc), "initial resolve completes pass warmup");
    expectTrue(pass->historyWarmupComplete(), "pass warmup complete after resolve");
    expectTrue(pass->shouldSkipHistoryWarmup(), "pass should skip warmup after resolve");

    expectTrue(pass->tryPreflightResolveBlendWeights(resolveDesc, blendReason),

    expectTrue(pass->shouldSkipHistoryWarmup(), "pass should skip warmup after invalidate");
    expectTrue(!pass->historyWarmupComplete(), "pass history warmup not complete after invalidate");
    expectTrue(pass->preflightResolveFrame(resolveDesc, &skipReason, &blendReason),
               "pass resolve frame preflight passes before warmup resolve");
    expectTrue(!pass->shouldSkipResolveFrame(resolveDesc),
               "pass should not skip resolve frame before warmup resolve");

               "pass resolve frame preflight passes after warmup");
               "pass should not skip resolve frame after warmup");
    expectTrue(pass->resolveFrame(resolveDesc), "initial resolve completes warmup");
    expectTrue(pass->historyWarmupComplete(), "pass historyWarmupComplete after resolve");
    expectTrue(pass->preflightHistoryWarmup(&warmupReason), "pass warmup preflight passes after resolve");
    expectTrue(warmupReason == fuse::renderer::TaaHistoryWarmupRejectReason::None,
               "pass warmup preflight None after resolve");
    fuse::renderer::TaaResolveReuseBlendRejectReason reuseBlendReason =
        fuse::renderer::TaaResolveReuseBlendRejectReason::None;
    expectTrue(pass->shouldSkipResolveReuseAndBlend(resolveDesc, 0u),
               "pass should skip reuse-and-blend before warmup");

    expectTrue(!pass->shouldSkipResolveReuseAndBlend(resolveDesc, 0u),
               "pass should not skip reuse-and-blend after warmup");
    expectTrue(pass->preflightResolveReuseAndBlend(resolveDesc, 0u, &reuseBlendReason),
               "pass reuse-and-blend preflight passes after warmup");

    const fuse::u32 generationBefore = pass->historyInvalidateGeneration();
    expectTrue(!pass->invalidateHistoryIfStale(generationBefore),
               "current generation does not invalidate pass history");
    expectTrue(pass->invalidateHistoryIfStale(generationBefore + 99u),
               "stale generation invalidates pass history");
    expectTrue(pass->shouldSkipHistoryWarmup(), "pass should skip warmup after stale invalidate");
    expectTrue(pass->shouldSkipResolve(resolveDesc), "pass should skip resolve before init");

               "pass tryPreflightResolve fails before init");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::HistoryNotReady,
               "pass tryPreflightResolve skip reason is HistoryNotReady before init");

    fuse::renderer::TaaHistoryReuseBlockReason warmupReason = fuse::renderer::TaaHistoryReuseBlockReason::None;
    expectTrue(!pass->tryPreflightHistoryWarmup(warmupReason),
               "pass tryPreflightHistoryWarmup fails before init");
    expectTrue(warmupReason == fuse::renderer::TaaHistoryReuseBlockReason::NotReady,
               "pass tryPreflightHistoryWarmup reason is NotReady before init");

    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass try/should-skip wrapper test");
    TAA_SKIP_UNLESS_VULKAN_READY(bootstrap);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass should-skip wrapper test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);
    expectTrue(pass->init(resources), "TaaPass initialized for try/should-skip wrapper test");

    expectTrue(pass->shouldSkipHistoryWarmup(), "pass should skip warmup completion before first resolve");
    expectTrue(!pass->shouldSkipResolve(resolveDesc), "pass should not skip valid resolve after init");
               "pass tryPreflightResolve passes after init");
    expectTrue(!pass->shouldSkipHistoryWarmup(), "pass should not skip warmup completion after resolve");
    expectTrue(pass->tryPreflightHistoryWarmup(warmupReason),
               "pass tryPreflightHistoryWarmup passes after resolve");
    expectTrue(!pass->shouldSkipResolve(resolveDesc), "pass should not skip resolve after warmup");

    fuse::renderer::TaaPassDesc zeroWidthDesc{};
    zeroWidthDesc.width = 0;
    zeroWidthDesc.height = 128;
    auto zeroPass = fuse::renderer::TaaPass::create(zeroWidthDesc);
    expectTrue(zeroPass->shouldSkipJitterNdc(), "zero-width pass should skip NDC jitter");
    expectTrue(!zeroPass->preflightJitterNdc(), "zero-width pass preflightJitterNdc fails");


void testTaaPassTryClassifyWrappers() {

    expectTrue(pass->classifyJitterSyncReject() == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass classifyJitterSyncReject is None for default sequence");
    expectTrue(pass->classifyJitterNdcReject() == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass classifyJitterNdcReject is None for valid viewport");
    expectTrue(pass->classifyJitterAdvanceReject() == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass classifyJitterAdvanceReject is None for default sequence");

    fuse::renderer::TaaJitterGuardRejectReason jitterReason =
        fuse::renderer::TaaJitterGuardRejectReason::None;
    expectTrue(pass->tryPreflightJitterSync(3u, jitterReason), "pass tryPreflightJitterSync passes");
    expectTrue(jitterReason == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass tryPreflightJitterSync reject reason is None");
    expectTrue(pass->tryPreflightJitterNdc(jitterReason), "pass tryPreflightJitterNdc passes");
               "pass tryPreflightJitterNdc reject reason is None");
    expectTrue(pass->tryPreflightJitterAdvance(jitterReason), "pass tryPreflightJitterAdvance passes");
               "pass tryPreflightJitterAdvance reject reason is None");
    expectTrue(pass->preflightJitterAdvance(), "pass preflightJitterAdvance passes");
    expectTrue(!pass->shouldSkipJitterAdvance(), "pass should not skip jitter advance");

    expectTrue(!pass->tryPreflightHistoryReadyForResolve(reuseReason),
               "pass tryPreflightHistoryReadyForResolve fails before init");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::NotReady,
               "pass tryPreflightHistoryReadyForResolve reason is NotReady before init");
               "pass tryPreflightHistoryReuse fails before init");
               "pass tryPreflightHistoryReuse reason is NotReady before init");


    expectTrue(pass->classifyResolveSkip(resolveDesc) == fuse::renderer::TaaResolveSkipReason::HistoryNotReady,
               "pass classifyResolveSkip is HistoryNotReady before init");


    expectTrue(pass->classifyResolveBlendReject(resolveDesc) ==
                   fuse::renderer::TaaResolveBlendRejectReason::None,
               "pass classifyResolveBlendReject is None before init");
               "pass tryPreflightResolveBlendWeights passes before init");
    expectTrue(pass->tryComputeResolveBlendWeights(resolveDesc, weights, blendReason),
               "pass tryComputeResolveBlendWeights passes before init");

    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass try/classify wrapper test");


    expectTrue(pass->init(resources), "TaaPass initialized for try/classify wrapper test");

    expectTrue(pass->tryPreflightHistoryReadyForResolve(reuseReason),
               "pass tryPreflightHistoryReadyForResolve passes after init");
               "pass tryPreflightHistoryReadyForResolve reason is None after init");
               "pass tryPreflightHistoryReuse fails before warmup");
               "pass tryPreflightHistoryReuse reason is NotWarm before warmup");

               "pass classifyResolveSkip is None after init with valid desc");


    expectNear(weights.current, 0.25f, 1e-5f, "pass tryCompute steady current weight");
    expectNear(weights.history, 0.75f, 1e-5f, "pass tryCompute steady history weight");

               "pass classifyHistoryReuseBlock is StaleGeneration after invalidate");

    expectTrue(zeroPass->classifyJitterNdcReject() ==
                   fuse::renderer::TaaJitterGuardRejectReason::InvalidViewport,
               "zero-width pass classifyJitterNdcReject is InvalidViewport");
    expectTrue(!zeroPass->tryPreflightJitterNdc(jitterReason),
               "zero-width pass tryPreflightJitterNdc fails");
    expectTrue(jitterReason == fuse::renderer::TaaJitterGuardRejectReason::InvalidViewport,
               "zero-width pass tryPreflightJitterNdc reason is InvalidViewport");
    expectTrue(zeroPass->classifyJitterSyncReject() == fuse::renderer::TaaJitterGuardRejectReason::None,
               "zero-width pass classifyJitterSyncReject still None for valid sequence");

    fuse::renderer::TaaPassDesc invalidSeqDesc{};
    invalidSeqDesc.width = 128;
    invalidSeqDesc.height = 128;
    invalidSeqDesc.jitter.sequence_length = 0u;
    auto invalidSeqPass = fuse::renderer::TaaPass::create(invalidSeqDesc);
    expectTrue(invalidSeqPass->classifyJitterSyncReject() ==
                   fuse::renderer::TaaJitterGuardRejectReason::None,
               "invalid sequence desc falls back so classifyJitterSyncReject is None");
    expectTrue(invalidSeqPass->classifyJitterAdvanceReject() ==
               "invalid sequence desc falls back so classifyJitterAdvanceReject is None");
    resolveDesc.params.blend_factor = 0.2f;

    const fuse::renderer::TaaResolveBlendPreflight blendPreflight = pass->preflightResolveBlend(resolveDesc);
    expectTrue(blendPreflight.first_frame, "pass blend preflight marks first frame before resolve");
    expectTrue(!blendPreflight.history_blend_allowed, "pass blend preflight blocks history before resolve");

    expectTrue(pass->resolveFrame(resolveDesc), "resolve warms pass history for preflight guards");
    const fuse::renderer::TaaHistoryReusePreflight reusePreflight = pass->preflightHistoryReuse(0u);
    expectTrue(reusePreflight.reuse_allowed, "pass reuse preflight allows current generation after resolve");

    const fuse::renderer::TaaHistoryReusePreflight staleReusePreflight = pass->preflightHistoryReuse(0u);
    expectTrue(!staleReusePreflight.reuse_allowed,
               "pass reuse preflight blocks stale generation after invalidate");
    const fuse::renderer::TaaHistoryReusePreflight descReusePreflight =
        pass->preflightHistoryReuseForDesc(resolveDesc);
    expectTrue(!descReusePreflight.reuse_allowed, "pass desc reuse preflight blocks after invalidate");

    resolveDesc.observed_history_generation = pass->historyInvalidateGeneration();
    const fuse::renderer::TaaResolveBlendPreflight warmedBlendPreflight = pass->preflightResolveBlend(resolveDesc);
    expectTrue(warmedBlendPreflight.first_frame, "pass blend preflight marks first frame after invalidate");
    expectTrue(!warmedBlendPreflight.history_blend_allowed,
               "pass blend preflight blocks history blend on first frame after invalidate");

    expectTrue(!zeroPass->jitterNdcReady(), "zero-width pass jitter NDC not ready");
    expectTrue(zeroPass->shouldSkipJitterNdc(), "zero-width pass should skip jitter NDC");
    expectTrue(!zeroPass->preflightJitterNdc(&rejectReason),
               "zero-width pass preflightJitterNdc fails");
    expectTrue(rejectReason == fuse::renderer::TaaJitterGuardRejectReason::InvalidViewport,
               "zero-width pass preflightJitterNdc reject reason is InvalidViewport");
    expectTrue(zeroPass->jitterSyncReady(0u), "zero-width pass jitter sync still ready");
    expectTrue(!zeroPass->preflightJitterNdc(), "zero-width pass NDC preflight fails");
    expectTrue(!zeroPass->tryPreflightJitterNdc(jitterReject),
    expectTrue(jitterReject == fuse::renderer::TaaJitterGuardRejectReason::InvalidViewport,
               "zero-width pass NDC reject reason is InvalidViewport");
    expectTrue(!zeroPass->preflightJitterNdc(&jitterReject),
               "zero-width pass jitter reject reason is InvalidViewport");

    expectTrue(!pass->wouldSkipResolveBlend(resolveDesc), "pass wouldSkipResolveBlend before warmup resolve");

    expectTrue(!pass->wouldSkipHistoryReuse(0u), "pass wouldSkipHistoryReuse after warmup");
    expectTrue(!pass->wouldSkipResolveBlend(resolveDesc), "pass wouldSkipResolveBlend after warmup");

    history.destroy();
    expectTrue(zeroPass->shouldSkipJitterNdc(), "zero-width pass shouldSkipJitterNdc");
    fuse::renderer::TaaJitterGuardRejectReason jitterReject =
    expectTrue(!zeroPass->preflightJitterNdc(&jitterReject), "zero-width pass preflightJitterNdc fails");
               "zero-width pass jitter reject is InvalidViewport");
    expectTrue(zeroPass->currentJitterPixelOffsetIfReady(pixelOut),
               "zero-width pass still produces pixel offset when sequence is valid");
               "pass should not skip composite resolve preflight");
    expectTrue(pass->preflightResolveFrame(resolveDesc), "pass preflightResolveFrame passes");

               "pass composite preflight still passes after warmup");
               "pass tryPreflightHistoryReuse reason is NotWarm");



    expectTrue(pass->shouldSkipTemporalResolveGuards(resolveDesc, 0u),
               "pass should skip temporal guards before warmup");

    expectTrue(pass->isHistoryWarm(), "pass history warm after resolve");
    expectTrue(!pass->shouldSkipTemporalResolveGuards(resolveDesc, 0u),
               "pass should not skip temporal guards after warmup");
    expectTrue(pass->preflightTemporalResolveGuards(resolveDesc, 0u, &reuseReason, &blendReason),
               "pass preflightTemporalResolveGuards passes after warmup");

               "pass should skip temporal guards after invalidate");
    expectTrue(pass->preflightJitterNdc(&jitterReject), "pass preflightJitterNdc passes after init");
    expectTrue(jitterReject == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass preflightJitterNdc reject reason is None");

    fuse::renderer::TaaResolveSkipReason skipReason = fuse::renderer::TaaResolveSkipReason::None;
    fuse::renderer::TaaResolveBlendRejectReason blendReason =
        fuse::renderer::TaaResolveBlendRejectReason::None;
    expectTrue(pass->preflightResolveFrameGuards(resolveDesc, &skipReason, &blendReason),
               "pass resolve frame guards pass before first resolve");
    expectTrue(pass->resolveFrame(resolveDesc), "initial resolve warms pass history");

    expectTrue(pass->historyWarmupPhase() == fuse::renderer::TaaHistoryWarmupPhase::Warm,
               "pass warmup phase is Warm after resolve");
    expectTrue(!pass->shouldSkipHistoryReuseForResolve(resolveDesc),
               "pass should not skip resolve reuse after warmup");
    expectTrue(!pass->shouldSkipResolveFrameGuards(resolveDesc),
               "pass should not skip resolve frame guards after warmup");

    const fuse::u32 currentGeneration = pass->historyInvalidateGeneration();
    expectTrue(!pass->invalidateHistoryIfStale(currentGeneration),
               "current generation does not trigger pass invalidateHistoryIfStale");
    expectTrue(pass->invalidateHistoryIfStale(currentGeneration - 1u),
               "stale generation triggers pass invalidateHistoryIfStale");
    expectTrue(pass->historyWarmupPhase() == fuse::renderer::TaaHistoryWarmupPhase::Cold,
               "pass warmup phase is Cold after stale invalidate");

    pass->destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testJitterSlotSyncAdvanceTryGuards() {
    expectTrue(fuse::renderer::classifyTaaJitterSlotReject(3u, 8u) ==
                   fuse::renderer::TaaJitterGuardRejectReason::None,
               "in-range slot passes slot classify");
    expectTrue(fuse::renderer::classifyTaaJitterSlotReject(8u, 8u) ==
                   fuse::renderer::TaaJitterGuardRejectReason::InvalidSequence,
               "out-of-range slot fails slot classify");
    expectTrue(fuse::renderer::classifyTaaJitterSlotReject(0u, 0u) ==
                   fuse::renderer::TaaJitterGuardRejectReason::InvalidSequence,
               "invalid sequence fails slot classify");

    fuse::renderer::TaaJitterGuardRejectReason rejectReason =
        fuse::renderer::TaaJitterGuardRejectReason::None;
    expectTrue(fuse::renderer::preflightTaaJitterSlot(4u, 8u, &rejectReason),
               "preflightTaaJitterSlot passes for in-range slot");
    expectTrue(fuse::renderer::tryPreflightTaaJitterSlot(4u, 8u, rejectReason),
               "tryPreflightTaaJitterSlot passes for in-range slot");
    expectTrue(!fuse::renderer::shouldSkipTaaJitterSlot(4u, 8u),
               "in-range slot should not be skipped");
    expectTrue(fuse::renderer::shouldSkipTaaJitterSlot(9u, 8u),
               "out-of-range slot should be skipped");

    fuse::renderer::TaaJitter jitter;
    expectTrue(fuse::renderer::taaJitterNeedsResync(5u, jitter),
               "default jitter needs resync to frame 5");
    expectTrue(fuse::renderer::shouldResyncTaaJitter(5u, jitter),
               "shouldResyncTaaJitter true before sync");
    expectTrue(fuse::renderer::trySyncTaaJitter(jitter, 5u, rejectReason),
               "trySyncTaaJitter succeeds for valid sequence");
    expectTrue(jitter.isAlignedToFrameIndex(5u), "jitter aligned after trySyncTaaJitter");
    expectTrue(!fuse::renderer::taaJitterNeedsResync(5u, jitter),
               "jitter no longer needs resync after sync");

    const fuse::u32 indexBefore = jitter.index();
    expectTrue(fuse::renderer::tryAdvanceTaaJitter(jitter, rejectReason),
               "tryAdvanceTaaJitter succeeds for valid sequence");
    expectTrue(jitter.index() != indexBefore, "tryAdvanceTaaJitter advances jitter");

    fuse::math::Vec2 pixelOut{};
    expectTrue(jitter.currentPixelOffsetIfReady(pixelOut), "currentPixelOffsetIfReady succeeds");
    const fuse::math::Vec2 directPixel = jitter.currentPixelOffset();
    expectNear(pixelOut.x, directPixel.x, 1e-6f, "currentPixelOffsetIfReady matches currentPixelOffset X");
}

void testHistoryWarmupPreflightGuards() {
    fuse::renderer::TaaHistoryBuffer emptyHistory;
    expectTrue(fuse::renderer::classifyTaaHistoryWarmupBlock(emptyHistory) ==
                   fuse::renderer::TaaHistoryReuseBlockReason::NotReady,
               "empty history warmup block is NotReady");
    expectTrue(!fuse::renderer::taaHistoryWarmupComplete(emptyHistory),
               "empty history warmup is not complete");
    expectTrue(fuse::renderer::shouldSkipTaaHistoryWarmup(emptyHistory),
               "empty history should skip warmup reuse");

    fuse::renderer::TaaHistoryReuseBlockReason reason = fuse::renderer::TaaHistoryReuseBlockReason::None;
    expectTrue(!fuse::renderer::preflightTaaHistoryWarmup(emptyHistory, &reason),
               "empty history fails warmup preflight");
    expectTrue(reason == fuse::renderer::TaaHistoryReuseBlockReason::NotReady,
               "empty history warmup preflight reason is NotReady");
    expectTrue(!fuse::renderer::tryPreflightTaaHistoryWarmup(emptyHistory, reason),
               "tryPreflightTaaHistoryWarmup fails for empty history");

    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for warmup preflight test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaHistoryBuffer history;
    fuse::renderer::TaaHistoryBufferDesc historyDesc{64, 64};
    expectTrue(history.init(resources, historyDesc), "history ready for warmup preflight test");
    expectTrue(fuse::renderer::classifyTaaHistoryWarmupBlock(history) ==
                   fuse::renderer::TaaHistoryReuseBlockReason::NotWarm,
               "allocated unwarmed history warmup block is NotWarm");
    expectTrue(!fuse::renderer::preflightTaaHistoryWarmup(history, &reason),
               "unwarmed history fails warmup preflight");
    expectTrue(reason == fuse::renderer::TaaHistoryReuseBlockReason::NotWarm,
               "unwarmed history warmup preflight reason is NotWarm");

    history.markResolved();
    expectTrue(fuse::renderer::classifyTaaHistoryWarmupBlock(history) ==
                   fuse::renderer::TaaHistoryReuseBlockReason::None,
               "warmed history warmup block is None");
    expectTrue(fuse::renderer::taaHistoryWarmupComplete(history),
               "warmed history warmup is complete");
    expectTrue(fuse::renderer::preflightTaaHistoryWarmup(history, &reason),
               "warmed history passes warmup preflight");
    expectTrue(fuse::renderer::tryPreflightTaaHistoryWarmup(history, reason),
               "tryPreflightTaaHistoryWarmup passes after warmup");
    expectTrue(!fuse::renderer::shouldSkipTaaHistoryWarmup(history),
               "warmed history should not skip warmup");

    history.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testResolveFramePreflightGuards() {
    fuse::renderer::TaaHistoryBuffer emptyHistory;
    fuse::renderer::TaaResolveDesc desc{};
    desc.width = 64;
    desc.height = 64;
    desc.surfaces.current_frame = reinterpret_cast<void*>(0x1);
    desc.surfaces.output = reinterpret_cast<void*>(0x2);

    fuse::renderer::TaaResolveSkipReason skipReason = fuse::renderer::TaaResolveSkipReason::None;
    fuse::renderer::TaaResolveBlendRejectReason blendReject =
        fuse::renderer::TaaResolveBlendRejectReason::None;
    expectTrue(!fuse::renderer::preflightTaaResolveFrame(desc, emptyHistory, &skipReason, &blendReject),
               "empty history fails resolve-frame preflight");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::HistoryNotReady,
               "resolve-frame preflight skip reason is HistoryNotReady");
    expectTrue(blendReject == fuse::renderer::TaaResolveBlendRejectReason::None,
               "resolve-frame preflight blend reject is None for warmup weights");
    expectTrue(fuse::renderer::shouldSkipTaaResolveFrame(desc, emptyHistory),
               "empty history should skip resolve frame");

    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for resolve-frame preflight test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaHistoryBuffer history;
    fuse::renderer::TaaHistoryBufferDesc historyDesc{64, 64};
    expectTrue(history.init(resources, historyDesc), "history ready for resolve-frame preflight test");

    expectTrue(fuse::renderer::tryPreflightTaaResolveFrame(desc, history, skipReason, blendReject),
               "tryPreflightTaaResolveFrame passes for valid warmup resolve");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::None,
               "resolve-frame try preflight skip reason is None");
    expectTrue(blendReject == fuse::renderer::TaaResolveBlendRejectReason::None,
               "resolve-frame try preflight blend reject is None");
    expectTrue(!fuse::renderer::shouldSkipTaaResolveFrame(desc, history),
               "valid warmup resolve frame should not be skipped");

    desc.width = 128;
    expectTrue(!fuse::renderer::preflightTaaResolveFrame(desc, history, &skipReason, &blendReject),
               "dimension mismatch fails resolve-frame preflight");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::DimensionMismatch,
               "resolve-frame preflight reports DimensionMismatch");
    expectTrue(fuse::renderer::shouldSkipTaaResolveFrame(desc, history),
               "dimension mismatch should skip resolve frame");

    history.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testTaaPassDeepenTryAndFrameGuards() {
    fuse::renderer::TaaPassDesc passDesc{};
    passDesc.width = 128;
    passDesc.height = 128;
    auto pass = fuse::renderer::TaaPass::create(passDesc);

    fuse::renderer::TaaJitterGuardRejectReason jitterReject =
        fuse::renderer::TaaJitterGuardRejectReason::None;
    expectTrue(pass->trySyncJitterToFrameIndex(6u, jitterReject),
               "pass trySyncJitterToFrameIndex succeeds before init");
    expectTrue(pass->jitterAlignedToFrameIndex(6u), "pass jitter aligned after trySync");
    expectTrue(!pass->jitterNeedsResync(6u), "pass jitter does not need resync after trySync");
    expectTrue(pass->tryAdvanceJitterIfReady(jitterReject),
               "pass tryAdvanceJitterIfReady succeeds before init");
    expectTrue(pass->preflightJitterSlot(6u, &jitterReject),
               "pass preflightJitterSlot passes for in-range slot");
    expectTrue(!pass->shouldSkipJitterSlot(6u), "pass should not skip in-range jitter slot");
    expectTrue(pass->shouldSkipJitterSlot(9u), "pass should skip out-of-range jitter slot");

    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass try/frame guard test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);
    expectTrue(pass->init(resources), "TaaPass initialized for try/frame guard test");

    fuse::renderer::TaaHistoryReuseBlockReason warmupReason = fuse::renderer::TaaHistoryReuseBlockReason::None;
    expectTrue(!pass->preflightHistoryWarmup(&warmupReason),
               "pass warmup preflight fails before first resolve");
    expectTrue(warmupReason == fuse::renderer::TaaHistoryReuseBlockReason::NotWarm,
               "pass warmup preflight reason is NotWarm before resolve");
    expectTrue(!pass->historyWarmupComplete(), "pass history warmup not complete before resolve");

    fuse::renderer::TaaResolveDesc resolveDesc{};
    resolveDesc.width = 128;
    resolveDesc.height = 128;
    resolveDesc.surfaces.current_frame = reinterpret_cast<void*>(0x10);
    resolveDesc.surfaces.output = reinterpret_cast<void*>(0x20);

    fuse::renderer::TaaResolveSkipReason skipReason = fuse::renderer::TaaResolveSkipReason::None;
    fuse::renderer::TaaResolveBlendRejectReason blendReject =
        fuse::renderer::TaaResolveBlendRejectReason::None;
    expectTrue(pass->preflightResolveFrame(resolveDesc, &skipReason, &blendReject),
               "pass resolve-frame preflight passes before first resolve");
    expectTrue(pass->tryPreflightResolveFrame(resolveDesc, skipReason, blendReject),
               "pass tryPreflightResolveFrame passes before first resolve");
    expectTrue(!pass->shouldSkipResolveFrame(resolveDesc),
               "pass should not skip resolve frame before first resolve");

    expectTrue(pass->resolveFrame(resolveDesc), "pass resolve warms history");
    expectTrue(pass->historyWarmupComplete(), "pass history warmup complete after resolve");
    expectTrue(pass->preflightHistoryWarmup(&warmupReason),
               "pass warmup preflight passes after resolve");
    expectTrue(pass->tryPreflightHistoryWarmup(warmupReason),
               "pass tryPreflightHistoryWarmup passes after resolve");

    pass->syncJitterToFrameIndex(0u);
    expectTrue(pass->shouldResyncJitter(4u), "pass should resync jitter to frame 4");
    expectTrue(pass->trySyncJitterToFrameIndex(4u, jitterReject),
               "pass trySyncJitterToFrameIndex succeeds after init");

    pass->destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testRejectReasonIsBlockingHelpers() {
    expectTrue(!fuse::renderer::taaJitterGuardRejectReasonIsBlocking(
                   fuse::renderer::TaaJitterGuardRejectReason::None),
               "jitter guard None is not blocking");
    expectTrue(fuse::renderer::taaJitterGuardRejectReasonIsBlocking(
                   fuse::renderer::TaaJitterGuardRejectReason::InvalidSequence),
               "jitter guard InvalidSequence is blocking");
    expectTrue(fuse::renderer::taaJitterGuardRejectReasonIsBlocking(
                   fuse::renderer::TaaJitterGuardRejectReason::MisalignedFrame),
               "jitter guard MisalignedFrame is blocking");
    expectTrue(std::strcmp(fuse::renderer::taaJitterGuardRejectReasonLabel(
                               fuse::renderer::TaaJitterGuardRejectReason::MisalignedFrame),
                           "misaligned_frame") == 0,
               "misaligned_frame jitter guard label");

    expectTrue(!fuse::renderer::taaHistoryReuseBlockReasonIsBlocking(
                   fuse::renderer::TaaHistoryReuseBlockReason::None),
               "history reuse None is not blocking");
    expectTrue(fuse::renderer::taaHistoryReuseBlockReasonIsBlocking(
                   fuse::renderer::TaaHistoryReuseBlockReason::NotWarm),
               "history reuse NotWarm is blocking");

    expectTrue(!fuse::renderer::taaResolveBlendRejectReasonIsBlocking(
                   fuse::renderer::TaaResolveBlendRejectReason::None),
               "resolve blend None is not blocking");
    expectTrue(fuse::renderer::taaResolveBlendRejectReasonIsBlocking(
                   fuse::renderer::TaaResolveBlendRejectReason::InvalidWeights),
               "resolve blend InvalidWeights is blocking");
}

void testJitterAlignmentGuards() {
    fuse::renderer::TaaJitter jitter;
    expectTrue(fuse::renderer::classifyTaaJitterAlignmentReject(jitter, 0u) ==
                   fuse::renderer::TaaJitterGuardRejectReason::None,
               "default jitter aligned to frame zero");
    expectTrue(fuse::renderer::preflightTaaJitterAlignment(jitter, 0u),
               "preflightTaaJitterAlignment passes at frame zero");

    jitter.advance();
    expectTrue(fuse::renderer::classifyTaaJitterAlignmentReject(jitter, 0u) ==
                   fuse::renderer::TaaJitterGuardRejectReason::MisalignedFrame,
               "advanced jitter misaligned to frame zero");
    expectTrue(fuse::renderer::shouldSkipTaaJitterAlignment(jitter, 0u),
               "shouldSkipTaaJitterAlignment after advance");

    fuse::renderer::TaaJitterGuardRejectReason rejectReason =
        fuse::renderer::TaaJitterGuardRejectReason::None;
    expectTrue(!fuse::renderer::tryPreflightTaaJitterAlignment(jitter, 0u, rejectReason),
               "tryPreflightTaaJitterAlignment rejects misaligned frame");
    expectTrue(rejectReason == fuse::renderer::TaaJitterGuardRejectReason::MisalignedFrame,
               "misaligned frame reject reason");

    jitter.syncToFrameIndex(5u);
    expectTrue(fuse::renderer::tryPreflightTaaJitterAlignment(jitter, 5u, rejectReason),
               "tryPreflightTaaJitterAlignment passes after sync");
    expectTrue(rejectReason == fuse::renderer::TaaJitterGuardRejectReason::None,
               "aligned frame reject reason is None");
}

void testHistoryWarmupPreflight() {
    fuse::renderer::TaaHistoryBuffer emptyHistory;
    fuse::renderer::TaaHistoryReuseBlockReason reason = fuse::renderer::TaaHistoryReuseBlockReason::None;
    expectTrue(fuse::renderer::classifyTaaHistoryWarmupBlock(emptyHistory) ==
                   fuse::renderer::TaaHistoryReuseBlockReason::NotReady,
               "empty history warmup block is NotReady");
    expectTrue(!fuse::renderer::preflightTaaHistoryWarmup(emptyHistory, &reason),
               "empty history fails warmup preflight");
    expectTrue(reason == fuse::renderer::TaaHistoryReuseBlockReason::NotReady,
               "empty history warmup preflight reason is NotReady");

    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for warmup preflight test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaHistoryBuffer history;
    fuse::renderer::TaaHistoryBufferDesc historyDesc{64, 64};
    expectTrue(history.init(resources, historyDesc), "history ready for warmup preflight test");
    expectTrue(fuse::renderer::classifyTaaHistoryWarmupBlock(history) ==
                   fuse::renderer::TaaHistoryReuseBlockReason::NotWarm,
               "unwarmed history warmup block is NotWarm");
    expectTrue(!fuse::renderer::tryPreflightTaaHistoryWarmup(history, reason),
               "unwarmed history fails tryPreflightTaaHistoryWarmup");
    expectTrue(reason == fuse::renderer::TaaHistoryReuseBlockReason::NotWarm,
               "unwarmed history warmup preflight reason is NotWarm");

    history.markResolved();
    expectTrue(fuse::renderer::tryPreflightTaaHistoryWarmup(history, reason),
               "warmed history passes tryPreflightTaaHistoryWarmup");
    expectTrue(reason == fuse::renderer::TaaHistoryReuseBlockReason::None,
               "warmed history warmup preflight reason is None");

    history.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testTaaPassTryPreflightWrappers() {
    fuse::renderer::TaaPassDesc passDesc{};
    passDesc.width = 128;
    passDesc.height = 128;
    passDesc.params.blend_factor = 0.2f;

    auto pass = fuse::renderer::TaaPass::create(passDesc);

    fuse::renderer::TaaJitterGuardRejectReason jitterReason =
        fuse::renderer::TaaJitterGuardRejectReason::None;
    expectTrue(pass->tryPreflightJitterSync(3u, jitterReason), "pass tryPreflightJitterSync passes");
    expectTrue(jitterReason == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass tryPreflightJitterSync reject reason is None");
    expectTrue(pass->tryPreflightJitterAlignment(0u, jitterReason),
               "pass tryPreflightJitterAlignment passes at frame zero");
    expectTrue(pass->tryPreflightJitterNdc(jitterReason), "pass tryPreflightJitterNdc passes");

    fuse::renderer::TaaHistoryReuseBlockReason historyReason =
        fuse::renderer::TaaHistoryReuseBlockReason::None;
    expectTrue(!pass->tryPreflightHistoryWarmup(historyReason),
               "pass tryPreflightHistoryWarmup fails before init");
    expectTrue(historyReason == fuse::renderer::TaaHistoryReuseBlockReason::NotReady,
               "pass warmup preflight reason is NotReady before init");
    expectTrue(!pass->tryPreflightHistoryReuse(0u, historyReason),
               "pass tryPreflightHistoryReuse fails before init");
    expectTrue(historyReason == fuse::renderer::TaaHistoryReuseBlockReason::NotReady,
               "pass reuse preflight reason is NotReady before init");
    expectTrue(!pass->tryPreflightHistoryReadyForResolve(historyReason),
               "pass tryPreflightHistoryReadyForResolve fails before init");

    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass try-preflight test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);
    expectTrue(pass->init(resources), "TaaPass initialized for try-preflight test");

    expectTrue(pass->tryPreflightHistoryReadyForResolve(historyReason),
               "pass tryPreflightHistoryReadyForResolve passes after init");
    expectTrue(!pass->tryPreflightHistoryWarmup(historyReason),
               "pass tryPreflightHistoryWarmup fails before first resolve");
    expectTrue(historyReason == fuse::renderer::TaaHistoryReuseBlockReason::NotWarm,
               "pass warmup preflight reason is NotWarm before resolve");

    fuse::renderer::TaaResolveDesc resolveDesc{};
    resolveDesc.width = 128;
    resolveDesc.height = 128;
    resolveDesc.surfaces.current_frame = reinterpret_cast<void*>(0x10);
    resolveDesc.surfaces.output = reinterpret_cast<void*>(0x20);
    resolveDesc.params = passDesc.params;

    fuse::renderer::TaaResolveBlendRejectReason blendReason =
        fuse::renderer::TaaResolveBlendRejectReason::None;
    expectTrue(pass->tryPreflightResolveBlendWeights(resolveDesc, blendReason),
               "pass tryPreflightResolveBlendWeights passes before warmup resolve");

    fuse::renderer::TaaResolveSkipReason skipReason = fuse::renderer::TaaResolveSkipReason::None;
    expectTrue(pass->tryPreflightResolve(resolveDesc, skipReason),
               "pass tryPreflightResolve passes for valid resolve");
    expectTrue(pass->resolveFrame(resolveDesc), "initial resolve warms pass history");

    expectTrue(pass->tryPreflightHistoryWarmup(historyReason),
               "pass tryPreflightHistoryWarmup passes after resolve");
    expectTrue(pass->tryPreflightHistoryReuse(0u, historyReason),
               "pass tryPreflightHistoryReuse passes after resolve");

    pass->advanceJitter();
    expectTrue(pass->shouldSkipJitterAlignment(0u), "pass should skip jitter alignment after advance");
    expectTrue(!pass->tryPreflightJitterAlignment(0u, jitterReason),
               "pass tryPreflightJitterAlignment rejects after advance");
    expectTrue(jitterReason == fuse::renderer::TaaJitterGuardRejectReason::MisalignedFrame,
               "pass misaligned jitter reject reason");

    pass->destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testHistoryWarmupCompleteGuards() {
    fuse::renderer::TaaHistoryBuffer emptyHistory;
    expectTrue(std::strcmp(fuse::renderer::taaHistoryWarmupRejectReasonLabel(
                               fuse::renderer::TaaHistoryWarmupRejectReason::NotReady),
                           "not_ready") == 0,
               "NotReady warmup reject label");
    expectTrue(std::strcmp(fuse::renderer::taaHistoryWarmupRejectReasonLabel(
                               fuse::renderer::TaaHistoryWarmupRejectReason::Incomplete),
                           "incomplete") == 0,
               "Incomplete warmup reject label");

    expectTrue(fuse::renderer::classifyTaaHistoryWarmupReject(emptyHistory) ==
                   fuse::renderer::TaaHistoryWarmupRejectReason::NotReady,
               "empty history warmup reject is NotReady");
    expectTrue(fuse::renderer::shouldSkipTaaHistoryWarmupComplete(emptyHistory),
               "empty history should skip warmup-complete preflight");

    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    TAA_SKIP_UNLESS_VULKAN_READY(bootstrap);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for warmup-complete guard test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaHistoryBuffer history;
    fuse::renderer::TaaHistoryBufferDesc historyDesc{64, 64};
    expectTrue(history.init(resources, historyDesc), "history ready for warmup-complete guard test");
    expectTrue(fuse::renderer::classifyTaaHistoryWarmupReject(history) ==
                   fuse::renderer::TaaHistoryWarmupRejectReason::Incomplete,
               "allocated unwarmed history warmup reject is Incomplete");
    expectTrue(fuse::renderer::shouldSkipTaaHistoryWarmupComplete(history),
               "unwarmed history should skip warmup-complete preflight");

    fuse::renderer::TaaHistoryWarmupRejectReason reason = fuse::renderer::TaaHistoryWarmupRejectReason::None;
    expectTrue(!fuse::renderer::tryPreflightTaaHistoryWarmupComplete(history, reason),
               "tryPreflight warmup-complete fails for unwarmed history");
    expectTrue(reason == fuse::renderer::TaaHistoryWarmupRejectReason::Incomplete,
               "tryPreflight warmup-complete reason is Incomplete");

    history.markResolved();
    expectTrue(fuse::renderer::taaHistoryWarmupComplete(history), "warmed history warmup is complete");
    expectTrue(fuse::renderer::preflightTaaHistoryWarmupComplete(history, &reason),
               "warmed history passes warmup-complete preflight");
    expectTrue(reason == fuse::renderer::TaaHistoryWarmupRejectReason::None,
               "warmed history warmup-complete reason is None");
    expectTrue(!fuse::renderer::shouldSkipTaaHistoryWarmupComplete(history),
               "warmed history should not skip warmup-complete preflight");

    history.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testJitterAlignmentGuards() {
    using fuse::renderer::TaaJitterLayout;

    expectTrue(std::strcmp(fuse::renderer::taaJitterAlignmentRejectReasonLabel(
                               fuse::renderer::TaaJitterAlignmentRejectReason::SyncBlocked),
                           "sync_blocked") == 0,
               "SyncBlocked jitter alignment reject label");
    expectTrue(std::strcmp(fuse::renderer::taaJitterAlignmentRejectReasonLabel(
                               fuse::renderer::TaaJitterAlignmentRejectReason::Misaligned),
                           "misaligned") == 0,
               "Misaligned jitter alignment reject label");

    fuse::renderer::TaaJitter jitter;
    expectTrue(fuse::renderer::classifyTaaJitterAlignmentReject(5u, jitter) ==
                   fuse::renderer::TaaJitterAlignmentRejectReason::Misaligned,
               "default jitter is misaligned before sync");
    expectTrue(fuse::renderer::shouldSkipTaaJitterAlignment(5u, jitter),
               "misaligned jitter should skip alignment preflight");

    jitter.syncToFrameIndex(5u);
    expectTrue(fuse::renderer::preflightTaaJitterAligned(5u, jitter),
               "synced jitter passes alignment preflight");
    expectTrue(jitter.isAlignedToFrameIndex(5u), "synced jitter reports aligned");
    expectTrue(!fuse::renderer::shouldSkipTaaJitterAlignment(5u, jitter),
               "aligned jitter should not skip alignment preflight");

    fuse::renderer::TaaJitterAlignmentRejectReason alignReason =
        fuse::renderer::TaaJitterAlignmentRejectReason::None;
    expectTrue(fuse::renderer::tryPreflightTaaJitterAligned(5u, jitter, alignReason),
               "tryPreflight jitter alignment passes after sync");
    expectTrue(alignReason == fuse::renderer::TaaJitterAlignmentRejectReason::None,
               "tryPreflight jitter alignment reason is None");

    jitter.advance();
    expectTrue(fuse::renderer::classifyTaaJitterAlignmentReject(5u, jitter) ==
                   fuse::renderer::TaaJitterAlignmentRejectReason::Misaligned,
               "advanced jitter is misaligned to prior frame index");
    expectTrue(TaaJitterLayout::jitterSlotMatchesFrameIndex(6u, jitter.index(), 8u),
               "advanced jitter slot matches frame 6");
    expectTrue(fuse::renderer::preflightTaaJitterAligned(6u, jitter),
               "advanced jitter aligns to next frame index");

    fuse::renderer::TaaJitterDesc invalidDesc{};
    invalidDesc.sequence_length = 0u;
    fuse::renderer::TaaJitter fallbackJitter(invalidDesc);
    expectTrue(fuse::renderer::classifyTaaJitterAlignmentReject(0u, fallbackJitter) ==
                   fuse::renderer::TaaJitterAlignmentRejectReason::None,
               "fallback jitter is aligned at frame zero after construction");
}

void testResolveTemporalPreflightGuards() {
    expectTrue(std::strcmp(fuse::renderer::taaResolveTemporalRejectReasonLabel(
                               fuse::renderer::TaaResolveTemporalRejectReason::HistoryReuseBlocked),
                           "history_reuse_blocked") == 0,
               "HistoryReuseBlocked temporal reject label");
    expectTrue(std::strcmp(fuse::renderer::taaResolveTemporalRejectReasonLabel(
                               fuse::renderer::TaaResolveTemporalRejectReason::BlendWeightsRejected),
                           "blend_weights_rejected") == 0,
               "BlendWeightsRejected temporal reject label");

    fuse::renderer::TaaHistoryBuffer emptyHistory;
    fuse::renderer::TaaResolveDesc desc{};
    desc.params.blend_factor = 0.2f;
    expectTrue(fuse::renderer::classifyTaaResolveTemporalReject(desc, emptyHistory, 0u) ==
                   fuse::renderer::TaaResolveTemporalRejectReason::HistoryReuseBlocked,
               "empty history blocks temporal preflight");
    expectTrue(fuse::renderer::shouldSkipTaaResolveTemporal(desc, emptyHistory, 0u),
               "empty history should skip temporal preflight");

    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    TAA_SKIP_UNLESS_VULKAN_READY(bootstrap);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for temporal preflight test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaHistoryBuffer history;
    fuse::renderer::TaaHistoryBufferDesc historyDesc{64, 64};
    expectTrue(history.init(resources, historyDesc), "history ready for temporal preflight test");

    fuse::renderer::TaaResolveTemporalRejectReason temporalReason =
        fuse::renderer::TaaResolveTemporalRejectReason::None;
    expectTrue(!fuse::renderer::tryPreflightTaaResolveTemporal(desc, history, 0u, temporalReason),
               "unwarmed history fails temporal preflight");
    expectTrue(temporalReason == fuse::renderer::TaaResolveTemporalRejectReason::HistoryReuseBlocked,
               "unwarmed history temporal reject is HistoryReuseBlocked");

    history.markResolved();
    expectTrue(fuse::renderer::preflightTaaResolveTemporal(desc, history, 0u, &temporalReason),
               "warmed history passes temporal preflight");
    expectTrue(temporalReason == fuse::renderer::TaaResolveTemporalRejectReason::None,
               "warmed history temporal reject is None");
    expectTrue(!fuse::renderer::shouldSkipTaaResolveTemporal(desc, history, 0u),
               "warmed history should not skip temporal preflight");

    history.invalidateHistory();
    expectTrue(fuse::renderer::shouldSkipTaaResolveTemporal(desc, history, 0u),
               "stale generation should skip temporal preflight");

    history.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testTaaPassDeepenTemporalAndAlignmentWrappers() {
    fuse::renderer::TaaPassDesc passDesc{};
    passDesc.width = 128;
    passDesc.height = 128;
    passDesc.params.blend_factor = 0.15f;

    auto pass = fuse::renderer::TaaPass::create(passDesc);
    expectTrue(!pass->historyWarmupComplete(), "pass warmup incomplete before init");
    expectTrue(pass->shouldSkipHistoryWarmupComplete(), "pass should skip warmup-complete before init");
    expectTrue(!pass->preflightJitterAligned(5u), "pass jitter misaligned before sync to frame 5");
    expectTrue(pass->shouldSkipJitterAlignment(5u), "pass should skip alignment preflight before sync");
    expectTrue(pass->preflightJitterAdvance(), "pass preflightJitterAdvance passes with default sequence");

    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    TAA_SKIP_UNLESS_VULKAN_READY(bootstrap);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass temporal wrapper test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);
    expectTrue(pass->init(resources), "TaaPass initialized for temporal wrapper test");
    expectTrue(pass->preflightJitterAdvance(), "pass preflightJitterAdvance passes after init");
    expectTrue(!pass->shouldSkipJitterAdvance(), "pass should not skip jitter advance after init");

    pass->syncJitterToFrameIndex(4u);
    expectTrue(pass->preflightJitterAligned(4u), "pass jitter aligned after sync");
    expectTrue(!pass->shouldSkipJitterAlignment(4u), "pass should not skip jitter alignment after sync");

    fuse::renderer::TaaResolveDesc resolveDesc{};
    resolveDesc.width = 128;
    resolveDesc.height = 128;
    resolveDesc.surfaces.current_frame = reinterpret_cast<void*>(0x10);
    resolveDesc.surfaces.output = reinterpret_cast<void*>(0x20);
    resolveDesc.params = passDesc.params;

    fuse::renderer::TaaHistoryReuseBlockReason reuseReason = fuse::renderer::TaaHistoryReuseBlockReason::None;
    expectTrue(!pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse fails before warmup");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::NotWarm,
               "pass tryPreflightHistoryReuse reason is NotWarm before warmup");

    fuse::renderer::TaaResolveBlendRejectReason blendReason =
        fuse::renderer::TaaResolveBlendRejectReason::None;
    fuse::renderer::TaaBlendWeights weights{};
    expectTrue(pass->tryComputeExpectedResolveBlendWeights(resolveDesc, weights, blendReason),
               "pass tryComputeExpectedResolveBlendWeights passes before warmup");
    expectNear(weights.current, 1.f, 1e-5f, "pass expected warmup current weight is full");

    expectTrue(pass->shouldSkipResolveTemporal(resolveDesc, 0u),
               "pass should skip temporal preflight before warmup");
    expectTrue(pass->resolveFrame(resolveDesc), "initial resolve warms pass history");

    expectTrue(pass->historyWarmupComplete(), "pass warmup complete after resolve");
    expectTrue(!pass->shouldSkipHistoryWarmupComplete(), "pass should not skip warmup-complete after resolve");
    expectTrue(pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse passes after warmup");
    expectTrue(!pass->shouldSkipResolveTemporal(resolveDesc, 0u),
               "pass should not skip temporal preflight after warmup");

    const fuse::u32 observedGeneration = pass->historyInvalidateGeneration();
    expectTrue(!pass->invalidateHistoryIfStale(observedGeneration),
               "pass invalidateHistoryIfStale is no-op for current generation");
    expectTrue(pass->invalidateHistoryIfStale(observedGeneration + 1u),
               "pass invalidateHistoryIfStale invalidates stale generation");
    expectTrue(pass->shouldSkipResolveTemporal(resolveDesc, observedGeneration),
               "pass should skip temporal preflight after stale invalidate");

    pass->destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testJitterAlignmentGuards() {
    using fuse::renderer::TaaJitterLayout;

    expectTrue(std::strcmp(fuse::renderer::taaJitterGuardRejectReasonLabel(
                               fuse::renderer::TaaJitterGuardRejectReason::Misaligned),
                           "misaligned") == 0,
               "Misaligned jitter guard reject label");

    expectTrue(fuse::renderer::classifyTaaJitterAlignmentReject(5u, 5u, 5u, 8u) ==
                   fuse::renderer::TaaJitterGuardRejectReason::None,
               "aligned slot and monotonic counter pass alignment classify");
    expectTrue(fuse::renderer::classifyTaaJitterAlignmentReject(5u, 4u, 5u, 8u) ==
                   fuse::renderer::TaaJitterGuardRejectReason::Misaligned,
               "slot mismatch fails alignment classify");
    expectTrue(fuse::renderer::classifyTaaJitterAlignmentReject(5u, 5u, 4u, 8u) ==
                   fuse::renderer::TaaJitterGuardRejectReason::Misaligned,
               "monotonic mismatch fails alignment classify");
    expectTrue(fuse::renderer::classifyTaaJitterAlignmentReject(5u, 5u, 5u, 0u) ==
                   fuse::renderer::TaaJitterGuardRejectReason::InvalidSequence,
               "invalid sequence fails alignment classify before slot check");

    fuse::renderer::TaaJitterGuardRejectReason rejectReason =
        fuse::renderer::TaaJitterGuardRejectReason::None;
    expectTrue(fuse::renderer::preflightTaaJitterAlignment(13u, 5u, 13u, 8u, &rejectReason),
               "wrapped slot passes alignment preflight");
    expectTrue(rejectReason == fuse::renderer::TaaJitterGuardRejectReason::None,
               "alignment preflight reject reason is None");
    expectTrue(fuse::renderer::tryPreflightTaaJitterAlignment(13u, 5u, 13u, 8u, rejectReason),
               "tryPreflightTaaJitterAlignment passes for aligned state");
    expectTrue(!fuse::renderer::preflightTaaJitterAlignment(13u, 4u, 13u, 8u, &rejectReason),
               "misaligned slot fails alignment preflight");
    expectTrue(rejectReason == fuse::renderer::TaaJitterGuardRejectReason::Misaligned,
               "alignment preflight reject reason is Misaligned");
    expectTrue(fuse::renderer::shouldSkipTaaJitterAlignment(13u, 4u, 13u, 8u),
               "misaligned slot should skip alignment preflight");

    fuse::renderer::TaaJitter jitter;
    expectTrue(jitter.needsSyncToFrameIndex(5u), "fresh jitter needs sync to frame 5");
    jitter.syncToFrameIndex(5u);
    expectTrue(!jitter.needsSyncToFrameIndex(5u), "synced jitter does not need resync");
    expectTrue(jitter.isAlignedToFrameIndex(5u), "synced jitter is aligned to frame 5");
    expectTrue(jitter.needsSyncToFrameIndex(6u), "synced jitter needs resync for next frame");
    expectTrue(TaaJitterLayout::jitterSlotMatchesFrameIndex(13u, 5u, 8u),
               "wrapped frame index matches slot");

    fuse::renderer::TaaPassDesc passDesc{};
    passDesc.width = 128;
    passDesc.height = 128;
    auto pass = fuse::renderer::TaaPass::create(passDesc);
    expectTrue(pass->jitterNeedsSyncToFrameIndex(9u), "pass jitter needs sync before alignment");
    pass->syncJitterToFrameIndex(9u);
    expectTrue(!pass->jitterNeedsSyncToFrameIndex(9u), "pass jitter aligned after sync");
    expectTrue(pass->preflightJitterAlignment(9u, &rejectReason),
               "pass preflightJitterAlignment passes after sync");
    expectTrue(!pass->shouldSkipJitterAlignment(9u), "pass should not skip alignment after sync");
    expectTrue(pass->shouldSkipJitterAlignment(10u), "pass should skip alignment for mismatched frame");
}

void testHistoryWarmupStateGuards() {
    fuse::renderer::TaaHistoryBuffer emptyHistory;
    expectTrue(std::strcmp(fuse::renderer::taaHistoryWarmupStateLabel(
                               fuse::renderer::TaaHistoryWarmupState::NotReady),
                           "not_ready") == 0,
               "NotReady warmup state label");
    expectTrue(std::strcmp(fuse::renderer::taaHistoryWarmupStateLabel(
                               fuse::renderer::TaaHistoryWarmupState::NeedsWarmup),
                           "needs_warmup") == 0,
               "NeedsWarmup warmup state label");
    expectTrue(std::strcmp(fuse::renderer::taaHistoryWarmupStateLabel(
                               fuse::renderer::TaaHistoryWarmupState::Complete),
                           "complete") == 0,
               "Complete warmup state label");

    expectTrue(fuse::renderer::classifyTaaHistoryWarmupState(emptyHistory) ==
                   fuse::renderer::TaaHistoryWarmupState::NotReady,
               "empty history warmup state is NotReady");
    expectTrue(fuse::renderer::shouldSkipTaaHistoryWarmupComplete(emptyHistory),
               "empty history should skip warmup-complete preflight");
    expectTrue(fuse::renderer::shouldSkipTaaHistoryForTemporalBlend(emptyHistory, 0u),
               "empty history should skip temporal blend");

    fuse::renderer::TaaHistoryWarmupState warmupState = fuse::renderer::TaaHistoryWarmupState::Complete;
    expectTrue(!fuse::renderer::tryPreflightTaaHistoryWarmupComplete(emptyHistory, warmupState),
               "tryPreflightTaaHistoryWarmupComplete fails for empty history");
    expectTrue(warmupState == fuse::renderer::TaaHistoryWarmupState::NotReady,
               "empty history warmup preflight state is NotReady");

    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for warmup state test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaHistoryBuffer history;
    fuse::renderer::TaaHistoryBufferDesc historyDesc{64, 64};
    expectTrue(history.init(resources, historyDesc), "history ready for warmup state test");
    expectTrue(fuse::renderer::classifyTaaHistoryWarmupState(history) ==
                   fuse::renderer::TaaHistoryWarmupState::NeedsWarmup,
               "allocated history warmup state is NeedsWarmup");
    expectTrue(fuse::renderer::shouldSkipTaaHistoryWarmupComplete(history),
               "unwarmed history should skip warmup-complete preflight");
    expectTrue(fuse::renderer::shouldSkipTaaHistoryForTemporalBlend(history, 0u),
               "unwarmed history should skip temporal blend");

    fuse::renderer::TaaHistoryReuseBlockReason reuseReason = fuse::renderer::TaaHistoryReuseBlockReason::None;
    expectTrue(!fuse::renderer::tryPreflightTaaHistoryForTemporalBlend(history, 0u, reuseReason),
               "tryPreflightTaaHistoryForTemporalBlend fails before warmup");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::NotWarm,
               "temporal blend preflight reason is NotWarm before warmup");

    history.markResolved();
    expectTrue(fuse::renderer::classifyTaaHistoryWarmupState(history) ==
                   fuse::renderer::TaaHistoryWarmupState::Complete,
               "warmed history warmup state is Complete");
    expectTrue(fuse::renderer::taaHistoryWarmupComplete(history), "warmed history warmup is complete");
    expectTrue(!fuse::renderer::shouldSkipTaaHistoryWarmupComplete(history),
               "warmed history should not skip warmup-complete preflight");
    expectTrue(fuse::renderer::preflightTaaHistoryWarmupComplete(history, &warmupState),
               "warmup-complete preflight passes after resolve");
    expectTrue(warmupState == fuse::renderer::TaaHistoryWarmupState::Complete,
               "warmup-complete preflight state is Complete");
    expectTrue(fuse::renderer::tryPreflightTaaHistoryForTemporalBlend(history, 0u, reuseReason),
               "tryPreflightTaaHistoryForTemporalBlend passes after warmup");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::None,
               "temporal blend preflight reason is None after warmup");

    fuse::renderer::TaaPassDesc passDesc{};
    passDesc.width = 64;
    passDesc.height = 64;
    auto pass = fuse::renderer::TaaPass::create(passDesc);
    expectTrue(pass->classifyHistoryWarmupState() == fuse::renderer::TaaHistoryWarmupState::NotReady,
               "pass warmup state is NotReady before init");
    expectTrue(pass->init(resources), "TaaPass initialized for warmup state test");
    expectTrue(pass->classifyHistoryWarmupState() == fuse::renderer::TaaHistoryWarmupState::NeedsWarmup,
               "pass warmup state is NeedsWarmup after init");
    expectTrue(pass->shouldSkipHistoryWarmupComplete(), "pass should skip warmup-complete before resolve");
    expectTrue(pass->shouldSkipHistoryForTemporalBlend(0u), "pass should skip temporal blend before warmup");

    fuse::renderer::TaaResolveDesc resolveDesc{};
    resolveDesc.width = 64;
    resolveDesc.height = 64;
    resolveDesc.surfaces.current_frame = reinterpret_cast<void*>(0x10);
    resolveDesc.surfaces.output = reinterpret_cast<void*>(0x20);
    expectTrue(pass->resolveFrame(resolveDesc), "initial resolve completes pass warmup");
    expectTrue(pass->classifyHistoryWarmupState() == fuse::renderer::TaaHistoryWarmupState::Complete,
               "pass warmup state is Complete after resolve");
    expectTrue(pass->preflightHistoryWarmupComplete(), "pass warmup-complete preflight passes after resolve");
    expectTrue(!pass->shouldSkipHistoryForTemporalBlend(0u),
               "pass should not skip temporal blend after warmup");

    history.destroy();
    pass->destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testResolvePipelineBlendPreflights() {
    fuse::renderer::TaaHistoryBuffer emptyHistory;
    fuse::renderer::TaaResolveDesc desc{};
    desc.width = 64;
    desc.height = 64;
    desc.surfaces.current_frame = reinterpret_cast<void*>(0x10);
    desc.surfaces.output = reinterpret_cast<void*>(0x20);

    fuse::renderer::TaaResolveSkipReason skipReason = fuse::renderer::TaaResolveSkipReason::None;
    fuse::renderer::TaaResolveBlendRejectReason blendReason =
        fuse::renderer::TaaResolveBlendRejectReason::None;
    expectTrue(fuse::renderer::shouldSkipTaaResolveWithBlendWeights(desc, emptyHistory),
               "combined preflight should skip when history not ready");
    expectTrue(!fuse::renderer::tryPreflightTaaResolveWithBlendWeights(desc, emptyHistory, skipReason, blendReason),
               "tryPreflightTaaResolveWithBlendWeights fails when history not ready");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::HistoryNotReady,
               "combined preflight skip reason is HistoryNotReady");
    expectTrue(blendReason == fuse::renderer::TaaResolveBlendRejectReason::None,
               "combined preflight blend reason stays None when resolve skips");

    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for resolve pipeline preflight test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaHistoryBuffer history;
    fuse::renderer::TaaHistoryBufferDesc historyDesc{64, 64};
    expectTrue(history.init(resources, historyDesc), "history ready for resolve pipeline preflight test");
    expectTrue(fuse::renderer::preflightTaaResolveWithBlendWeights(desc, history, &skipReason, &blendReason),
               "combined preflight passes for valid warmup resolve");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::None,
               "combined preflight skip reason is None for valid desc");
    expectTrue(blendReason == fuse::renderer::TaaResolveBlendRejectReason::None,
               "combined preflight blend reason is None for valid desc");
    expectTrue(fuse::renderer::tryPreflightTaaResolvePipeline(desc, history, skipReason, blendReason),
               "tryPreflightTaaResolvePipeline passes for valid desc");
    expectTrue(!fuse::renderer::shouldSkipTaaResolvePipeline(desc, history),
               "resolve pipeline should not skip for valid desc");

    history.markResolved();
    desc.params.blend_factor = 0.4f;
    expectTrue(fuse::renderer::preflightTaaResolveWithBlendWeights(desc, history, &skipReason, &blendReason),
               "combined preflight passes after warmup");
    expectTrue(!fuse::renderer::shouldSkipTaaResolveWithBlendWeights(desc, history),
               "combined preflight should not skip after warmup");

    fuse::renderer::TaaPassDesc passDesc{};
    passDesc.width = 64;
    passDesc.height = 64;
    passDesc.params.blend_factor = 0.4f;
    auto pass = fuse::renderer::TaaPass::create(passDesc);
    expectTrue(pass->init(resources), "TaaPass initialized for resolve pipeline preflight test");
    expectTrue(pass->preflightResolveWithBlendWeights(desc, &skipReason, &blendReason),
               "pass combined preflight passes before warmup resolve");
    expectTrue(!pass->shouldSkipResolveWithBlendWeights(desc),
               "pass combined preflight should not skip before warmup resolve");

    desc.width = 0u;
    expectTrue(pass->shouldSkipResolveWithBlendWeights(desc),
               "pass combined preflight should skip with invalid dimensions");

    history.destroy();
    pass->destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testJitterSyncAlignmentGuards() {
    using fuse::renderer::TaaJitterLayout;

    expectTrue(fuse::renderer::classifyTaaJitterSyncAlignmentReject(5u, 5u, 8u) ==
                   fuse::renderer::TaaJitterGuardRejectReason::None,
               "matching slot passes sync alignment classify");
    expectTrue(fuse::renderer::classifyTaaJitterSyncAlignmentReject(13u, 5u, 8u) ==
                   fuse::renderer::TaaJitterGuardRejectReason::None,
               "wrapped frame slot passes sync alignment classify");
    expectTrue(fuse::renderer::classifyTaaJitterSyncAlignmentReject(5u, 6u, 8u) ==
                   fuse::renderer::TaaJitterGuardRejectReason::MisalignedSlot,
               "mismatched slot fails sync alignment classify");
    expectTrue(fuse::renderer::classifyTaaJitterSyncAlignmentReject(0u, 0u, 0u) ==
                   fuse::renderer::TaaJitterGuardRejectReason::InvalidSequence,
               "invalid sequence fails sync alignment classify before slot check");

    fuse::renderer::TaaJitterGuardRejectReason rejectReason =
        fuse::renderer::TaaJitterGuardRejectReason::None;
    expectTrue(fuse::renderer::preflightTaaJitterSyncAlignment(5u, 5u, 8u, &rejectReason),
               "preflightTaaJitterSyncAlignment passes for matching slot");
    expectTrue(rejectReason == fuse::renderer::TaaJitterGuardRejectReason::None,
               "sync alignment preflight reject reason is None");
    expectTrue(fuse::renderer::tryPreflightTaaJitterSyncAlignment(5u, 5u, 8u, rejectReason),
               "tryPreflightTaaJitterSyncAlignment passes for matching slot");
    expectTrue(!fuse::renderer::preflightTaaJitterSyncAlignment(5u, 6u, 8u, &rejectReason),
               "preflightTaaJitterSyncAlignment rejects mismatched slot");
    expectTrue(rejectReason == fuse::renderer::TaaJitterGuardRejectReason::MisalignedSlot,
               "sync alignment preflight reject reason is MisalignedSlot");
    expectTrue(fuse::renderer::shouldSkipTaaJitterSyncAlignment(5u, 6u, 8u),
               "shouldSkipTaaJitterSyncAlignment for mismatched slot");
    expectTrue(!fuse::renderer::shouldSkipTaaJitterSyncAlignment(5u, 5u, 8u),
               "should not skip sync alignment for matching slot");

    expectTrue(std::strcmp(fuse::renderer::taaJitterGuardRejectReasonLabel(
                               fuse::renderer::TaaJitterGuardRejectReason::MisalignedSlot),
                           "misaligned_slot") == 0,
               "MisalignedSlot reject label");

    fuse::renderer::TaaJitter jitter;
    jitter.syncToFrameIndex(5u);
    expectTrue(jitter.slotAlignedToFrameIndex(5u), "jitter slot aligned after sync");
    expectTrue(!jitter.slotAlignedToFrameIndex(6u), "jitter slot not aligned to different frame");

    fuse::renderer::TaaPassDesc passDesc{};
    passDesc.width = 128;
    passDesc.height = 128;
    auto pass = fuse::renderer::TaaPass::create(passDesc);
    pass->syncJitterToFrameIndex(7u);
    expectTrue(pass->jitterSlotAlignedToFrameIndex(7u), "pass jitter slot aligned after sync");
    expectTrue(!pass->shouldSkipJitterSyncAlignment(7u), "pass should not skip sync alignment after sync");
    expectTrue(pass->preflightJitterSyncAlignment(7u, &rejectReason),
               "pass preflightJitterSyncAlignment passes after sync");
    expectTrue(!pass->shouldSkipJitterAdvance(), "pass should not skip jitter advance");
    expectTrue(pass->tryPreflightJitterSync(7u, rejectReason), "pass tryPreflightJitterSync passes");
    expectTrue(pass->tryPreflightJitterAdvance(rejectReason), "pass tryPreflightJitterAdvance passes");
}

void testHistoryWarmupPreflightGuards() {
    fuse::renderer::TaaHistoryBuffer emptyHistory;
    expectTrue(fuse::renderer::classifyTaaHistoryWarmupBlock(emptyHistory) ==
                   fuse::renderer::TaaHistoryReuseBlockReason::NotReady,
               "empty history warmup block is NotReady");
    expectTrue(!fuse::renderer::taaHistoryWarmupComplete(emptyHistory),
               "empty history warmup is not complete");
    expectTrue(fuse::renderer::shouldSkipTaaHistoryWarmup(emptyHistory),
               "empty history should skip warmup (needs warmup)");

    fuse::renderer::TaaHistoryReuseBlockReason reason = fuse::renderer::TaaHistoryReuseBlockReason::None;
    expectTrue(!fuse::renderer::tryPreflightTaaHistoryWarmup(emptyHistory, reason),
               "tryPreflightTaaHistoryWarmup fails for empty history");
    expectTrue(reason == fuse::renderer::TaaHistoryReuseBlockReason::NotReady,
               "empty history warmup tryPreflight reason is NotReady");

    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for warmup preflight test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaHistoryBuffer history;
    fuse::renderer::TaaHistoryBufferDesc historyDesc{64, 64};
    expectTrue(history.init(resources, historyDesc), "history ready for warmup preflight test");
    expectTrue(!history.warmupComplete(), "allocated history warmup is not complete");
    expectTrue(fuse::renderer::classifyTaaHistoryWarmupBlock(history) ==
                   fuse::renderer::TaaHistoryReuseBlockReason::NotWarm,
               "unwarmed history warmup block is NotWarm");
    expectTrue(!fuse::renderer::preflightTaaHistoryWarmup(history, &reason),
               "unwarmed history fails warmup preflight");
    expectTrue(reason == fuse::renderer::TaaHistoryReuseBlockReason::NotWarm,
               "unwarmed history warmup preflight reason is NotWarm");

    history.markResolved();
    expectTrue(history.warmupComplete(), "warmed history buffer warmup is complete");
    expectTrue(fuse::renderer::taaHistoryWarmupComplete(history),
               "taaHistoryWarmupComplete passes after markResolved");
    expectTrue(fuse::renderer::preflightTaaHistoryWarmup(history, &reason),
               "warmed history passes warmup preflight");
    expectTrue(reason == fuse::renderer::TaaHistoryReuseBlockReason::None,
               "warmed history warmup preflight reason is None");
    expectTrue(!fuse::renderer::shouldSkipTaaHistoryWarmup(history),
               "warmed history should not skip warmup");

    fuse::renderer::TaaPassDesc passDesc{};
    passDesc.width = 64;
    passDesc.height = 64;
    auto pass = fuse::renderer::TaaPass::create(passDesc);
    expectTrue(pass->init(resources), "TaaPass initialized for warmup preflight test");
    expectTrue(!pass->historyWarmupComplete(), "pass history warmup not complete before resolve");
    expectTrue(!pass->tryPreflightHistoryWarmup(reason), "pass tryPreflightHistoryWarmup fails before resolve");
    expectTrue(reason == fuse::renderer::TaaHistoryReuseBlockReason::NotWarm,
               "pass warmup tryPreflight reason is NotWarm before resolve");

    fuse::renderer::TaaResolveDesc resolveDesc{};
    resolveDesc.width = 64;
    resolveDesc.height = 64;
    resolveDesc.surfaces.current_frame = reinterpret_cast<void*>(0x10);
    resolveDesc.surfaces.output = reinterpret_cast<void*>(0x20);
    expectTrue(pass->resolveFrame(resolveDesc), "initial resolve completes pass warmup");
    expectTrue(pass->historyWarmupComplete(), "pass history warmup complete after resolve");
    expectTrue(pass->tryPreflightHistoryWarmup(reason), "pass tryPreflightHistoryWarmup passes after resolve");

    history.destroy();
    pass->destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testResolveBlendPolicyAndCombinedPreflight() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for blend policy preflight test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaHistoryBuffer history;
    fuse::renderer::TaaHistoryBufferDesc historyDesc{64, 64};
    expectTrue(history.init(resources, historyDesc), "history ready for blend policy preflight test");

    fuse::renderer::TaaResolveDesc desc{};
    desc.width = 64;
    desc.height = 64;
    desc.surfaces.current_frame = reinterpret_cast<void*>(0x10);
    desc.surfaces.output = reinterpret_cast<void*>(0x20);
    desc.params.blend_factor = 0.2f;

    fuse::renderer::TaaResolveBlendRejectReason blendReason =
        fuse::renderer::TaaResolveBlendRejectReason::None;
    expectTrue(fuse::renderer::preflightTaaResolveBlendPolicy(desc, history, &blendReason),
               "warmup blend policy preflight passes");
    expectTrue(fuse::renderer::tryPreflightTaaResolveBlendPolicy(desc, history, blendReason),
               "tryPreflightTaaResolveBlendPolicy passes for warmup");
    expectTrue(!fuse::renderer::shouldSkipTaaResolveBlendPolicy(desc, history),
               "warmup blend policy should not be skipped");

    fuse::renderer::TaaResolveSkipReason skipReason = fuse::renderer::TaaResolveSkipReason::None;
    expectTrue(fuse::renderer::tryPreflightTaaResolveCombined(desc, history, skipReason, blendReason),
               "tryPreflightTaaResolveCombined passes for valid warmup resolve");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::None,
               "combined preflight skip reason is None");
    expectTrue(blendReason == fuse::renderer::TaaResolveBlendRejectReason::None,
               "combined preflight blend reason is None");
    expectTrue(!fuse::renderer::shouldSkipTaaResolveCombined(desc, history),
               "combined preflight should not skip valid warmup resolve");

    history.markResolved();
    expectTrue(fuse::renderer::preflightTaaResolveBlendPolicy(desc, history),
               "steady blend policy preflight passes after warmup");
    expectTrue(fuse::renderer::tryPreflightTaaResolveCombined(desc, history, skipReason, blendReason),
               "combined preflight passes after warmup");

    fuse::renderer::TaaPassDesc passDesc{};
    passDesc.width = 64;
    passDesc.height = 64;
    passDesc.params = desc.params;
    auto pass = fuse::renderer::TaaPass::create(passDesc);
    expectTrue(pass->init(resources), "TaaPass initialized for blend policy preflight test");
    expectTrue(pass->tryPreflightResolveBlendPolicy(desc, blendReason),
               "pass tryPreflightResolveBlendPolicy passes before resolve");
    expectTrue(pass->tryPreflightResolveCombined(desc, skipReason, blendReason),
               "pass tryPreflightResolveCombined passes before resolve");
    expectTrue(!pass->shouldSkipResolveBlendPolicy(desc),
               "pass should not skip blend policy before resolve");
    expectTrue(!pass->shouldSkipResolveCombined(desc),
               "pass should not skip combined preflight before resolve");

    fuse::renderer::TaaHistoryReuseBlockReason reuseReason =
        fuse::renderer::TaaHistoryReuseBlockReason::None;
    expectTrue(!pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse fails before warmup");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::NotWarm,
               "pass tryPreflightHistoryReuse reason is NotWarm before warmup");
    expectTrue(pass->tryPreflightResolveBlendWeights(desc, blendReason),
               "pass tryPreflightResolveBlendWeights passes before resolve");

    history.destroy();
    pass->destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testJitterAlignmentGuards() {
    using fuse::renderer::TaaJitterLayout;

    expectTrue(std::strcmp(fuse::renderer::taaJitterGuardRejectReasonLabel(
                               fuse::renderer::TaaJitterGuardRejectReason::MisalignedFrame),
                           "misaligned_frame") == 0,
               "MisalignedFrame jitter guard reject label");

    expectTrue(fuse::renderer::classifyTaaJitterAlignmentReject(5u, 5u, 5u, 8u) ==
                   fuse::renderer::TaaJitterGuardRejectReason::None,
               "aligned jitter passes alignment classify");
    expectTrue(fuse::renderer::classifyTaaJitterAlignmentReject(5u, 4u, 5u, 8u) ==
                   fuse::renderer::TaaJitterGuardRejectReason::MisalignedFrame,
               "monotonic mismatch fails alignment classify");
    expectTrue(fuse::renderer::classifyTaaJitterAlignmentReject(5u, 5u, 4u, 8u) ==
                   fuse::renderer::TaaJitterGuardRejectReason::MisalignedFrame,
               "slot mismatch fails alignment classify");
    expectTrue(fuse::renderer::classifyTaaJitterAlignmentReject(5u, 5u, 5u, 0u) ==
                   fuse::renderer::TaaJitterGuardRejectReason::InvalidSequence,
               "invalid sequence fails alignment classify before misalignment");

    fuse::renderer::TaaJitter jitter;
    jitter.syncToFrameIndex(5u);
    expectTrue(!fuse::renderer::shouldSkipTaaJitterAlignment(5u, jitter.monotonicFrameIndex(), jitter.index(), 8u),
               "synced jitter should not skip alignment");
    expectTrue(fuse::renderer::shouldSkipTaaJitterAlignment(6u, jitter.monotonicFrameIndex(), jitter.index(), 8u),
               "unsynced frame should skip alignment");

    fuse::renderer::TaaJitterGuardRejectReason rejectReason =
        fuse::renderer::TaaJitterGuardRejectReason::None;
    expectTrue(fuse::renderer::tryPreflightTaaJitterAlignment(5u, jitter.monotonicFrameIndex(), jitter.index(), 8u,
                                                              rejectReason),
               "tryPreflightTaaJitterAlignment passes for synced jitter");
    expectTrue(rejectReason == fuse::renderer::TaaJitterGuardRejectReason::None,
               "tryPreflightTaaJitterAlignment reject reason is None");

    fuse::math::Vec2 pixelOut{};
    expectTrue(jitter.currentPixelOffsetIfReady(pixelOut), "currentPixelOffsetIfReady succeeds for valid sequence");
    const fuse::math::Vec2 directPixel = jitter.currentPixelOffset();
    expectNear(pixelOut.x, directPixel.x, 1e-6f, "currentPixelOffsetIfReady matches currentPixelOffset X");
}

void testHistoryWarmupPreflightGuards() {
    fuse::renderer::TaaHistoryBuffer emptyHistory;
    fuse::renderer::TaaHistoryReuseBlockReason reason = fuse::renderer::TaaHistoryReuseBlockReason::None;
    expectTrue(!fuse::renderer::preflightTaaHistoryWarmup(emptyHistory, &reason),
               "empty history fails warmup preflight");
    expectTrue(reason == fuse::renderer::TaaHistoryReuseBlockReason::NotReady,
               "empty history warmup preflight reason is NotReady");
    expectTrue(!fuse::renderer::tryPreflightTaaHistoryWarmup(emptyHistory, reason),
               "tryPreflightTaaHistoryWarmup fails for empty history");
    expectTrue(reason == fuse::renderer::TaaHistoryReuseBlockReason::NotReady,
               "tryPreflightTaaHistoryWarmup reason is NotReady for empty history");

    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for warmup preflight test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaHistoryBuffer history;
    fuse::renderer::TaaHistoryBufferDesc historyDesc{64, 64};
    expectTrue(history.init(resources, historyDesc), "history ready for warmup preflight test");
    expectTrue(fuse::renderer::preflightTaaHistoryReadyForResolve(history, &reason),
               "allocated history passes resolve-readiness preflight");
    expectTrue(reason == fuse::renderer::TaaHistoryReuseBlockReason::None,
               "allocated history resolve-readiness preflight reason is None");
    expectTrue(!fuse::renderer::preflightTaaHistoryWarmup(history, &reason),
               "unwarmed history fails warmup preflight");
    expectTrue(reason == fuse::renderer::TaaHistoryReuseBlockReason::NotWarm,
               "unwarmed history warmup preflight reason is NotWarm");

    history.markResolved();
    expectTrue(fuse::renderer::preflightTaaHistoryWarmup(history, &reason),
               "warmed history passes warmup preflight");
    expectTrue(fuse::renderer::tryPreflightTaaHistoryWarmup(history, reason),
               "tryPreflightTaaHistoryWarmup passes for warmed history");
    expectTrue(reason == fuse::renderer::TaaHistoryReuseBlockReason::None,
               "warmed history warmup preflight reason is None");

    history.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testTaaPassTryPreflightWrappers() {
    fuse::renderer::TaaPassDesc passDesc{};
    passDesc.width = 128;
    passDesc.height = 128;
    passDesc.params.blend_factor = 0.3f;

    auto pass = fuse::renderer::TaaPass::create(passDesc);
    expectTrue(!pass->shouldSkipJitterAlignment(0u), "pass default jitter is aligned to frame zero");
    expectTrue(pass->shouldSkipJitterAlignment(4u), "pass should skip jitter alignment to unsynced frame");
    expectTrue(!pass->shouldSkipJitterAdvance(), "pass should not skip jitter advance before init");

    fuse::renderer::TaaJitterGuardRejectReason jitterReason =
        fuse::renderer::TaaJitterGuardRejectReason::None;
    expectTrue(pass->tryPreflightJitterSync(0u, jitterReason), "pass tryPreflightJitterSync passes before init");
    expectTrue(pass->tryPreflightJitterNdc(jitterReason), "pass tryPreflightJitterNdc passes before init");
    expectTrue(pass->tryPreflightJitterAdvance(jitterReason), "pass tryPreflightJitterAdvance passes before init");

    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass try-preflight wrapper test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);
    expectTrue(pass->init(resources), "TaaPass initialized for try-preflight wrapper test");

    pass->syncJitterToFrameIndex(4u);
    expectTrue(!pass->shouldSkipJitterAlignment(4u), "pass should not skip jitter alignment after sync");
    expectTrue(pass->preflightJitterAlignment(4u, &jitterReason),
               "pass preflightJitterAlignment passes after sync");
    expectTrue(pass->tryPreflightJitterAlignment(4u, jitterReason),
               "pass tryPreflightJitterAlignment passes after sync");

    fuse::renderer::TaaHistoryReuseBlockReason reuseReason = fuse::renderer::TaaHistoryReuseBlockReason::None;
    expectTrue(!pass->tryPreflightHistoryWarmup(reuseReason),
               "pass tryPreflightHistoryWarmup fails before first resolve");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::NotWarm,
               "pass tryPreflightHistoryWarmup reason is NotWarm before resolve");
    expectTrue(pass->tryPreflightHistoryReadyForResolve(reuseReason),
               "pass tryPreflightHistoryReadyForResolve passes after init");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::None,
               "pass tryPreflightHistoryReadyForResolve reason is None after init");

    fuse::renderer::TaaResolveDesc resolveDesc{};
    resolveDesc.width = 128;
    resolveDesc.height = 128;
    resolveDesc.surfaces.current_frame = reinterpret_cast<void*>(0x10);
    resolveDesc.surfaces.output = reinterpret_cast<void*>(0x20);
    resolveDesc.params = passDesc.params;

    fuse::renderer::TaaResolveSkipReason skipReason = fuse::renderer::TaaResolveSkipReason::None;
    expectTrue(pass->preflightResolve(resolveDesc, &skipReason), "pass preflightResolve passes before warmup resolve");
    expectTrue(pass->tryPreflightResolve(resolveDesc, skipReason),
               "pass tryPreflightResolve passes before warmup resolve");

    fuse::renderer::TaaResolveBlendRejectReason blendReason =
        fuse::renderer::TaaResolveBlendRejectReason::None;
    expectTrue(pass->tryPreflightResolveBlendWeights(resolveDesc, blendReason),
               "pass tryPreflightResolveBlendWeights passes before warmup resolve");
    fuse::renderer::TaaBlendWeights weights{};
    expectTrue(pass->tryComputeResolveBlendWeights(resolveDesc, weights, blendReason),
               "pass tryComputeResolveBlendWeights passes before warmup resolve");
    expectNear(weights.current, 1.f, 1e-5f, "pass tryCompute warmup current weight is full");

    expectTrue(!pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse fails before warmup resolve");
    expectTrue(pass->resolveFrame(resolveDesc), "initial resolve warms pass history");
    expectTrue(pass->tryPreflightHistoryWarmup(reuseReason),
               "pass tryPreflightHistoryWarmup passes after resolve");
    expectTrue(pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse passes after warmup");

    pass->destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testTaaPassTryPreflightAndClassifyWrappers() {
    fuse::renderer::TaaPassDesc passDesc{};
    passDesc.width = 128;
    passDesc.height = 128;
    passDesc.params.blend_factor = 0.2f;

    auto pass = fuse::renderer::TaaPass::create(passDesc);
    expectTrue(pass->canAdvanceJitter(), "pass canAdvanceJitter before init");
    expectTrue(pass->canSyncJitterToFrameIndex(4u), "pass canSyncJitterToFrameIndex before init");
    expectTrue(pass->classifyJitterSyncReject() == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass classifyJitterSyncReject is None before init");
    expectTrue(pass->classifyJitterNdcReject() == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass classifyJitterNdcReject is None before init");
    expectTrue(pass->classifyJitterAdvanceReject() == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass classifyJitterAdvanceReject is None before init");
    expectTrue(pass->preflightJitterAdvance(), "pass preflightJitterAdvance passes before init");
    expectTrue(!pass->shouldSkipJitterAdvance(), "pass should not skip jitter advance before init");

    fuse::renderer::TaaJitterGuardRejectReason jitterReason =
        fuse::renderer::TaaJitterGuardRejectReason::None;
    expectTrue(pass->tryPreflightJitterSync(4u, jitterReason), "pass tryPreflightJitterSync passes before init");
    expectTrue(jitterReason == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass tryPreflightJitterSync reject reason is None before init");
    expectTrue(pass->tryPreflightJitterNdc(jitterReason), "pass tryPreflightJitterNdc passes before init");
    expectTrue(jitterReason == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass tryPreflightJitterNdc reject reason is None before init");
    expectTrue(pass->tryPreflightJitterAdvance(jitterReason), "pass tryPreflightJitterAdvance passes before init");
    expectTrue(jitterReason == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass tryPreflightJitterAdvance reject reason is None before init");

    fuse::renderer::TaaHistoryReuseBlockReason reuseReason = fuse::renderer::TaaHistoryReuseBlockReason::None;
    expectTrue(!pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse fails before init");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::NotReady,
               "pass tryPreflightHistoryReuse reason is NotReady before init");
    expectTrue(!pass->tryPreflightHistoryReadyForResolve(reuseReason),
               "pass tryPreflightHistoryReadyForResolve fails before init");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::NotReady,
               "pass tryPreflightHistoryReadyForResolve reason is NotReady before init");

    fuse::renderer::TaaResolveDesc resolveDesc{};
    resolveDesc.width = 128;
    resolveDesc.height = 128;
    resolveDesc.surfaces.current_frame = reinterpret_cast<void*>(0x10);
    resolveDesc.surfaces.output = reinterpret_cast<void*>(0x20);
    resolveDesc.params = passDesc.params;

    fuse::renderer::TaaResolveBlendRejectReason blendReason =
        fuse::renderer::TaaResolveBlendRejectReason::None;
    expectTrue(pass->tryPreflightResolveBlendWeights(resolveDesc, blendReason),
               "pass tryPreflightResolveBlendWeights passes before init");
    expectTrue(blendReason == fuse::renderer::TaaResolveBlendRejectReason::None,
               "pass tryPreflightResolveBlendWeights reject reason is None before init");
    expectTrue(pass->classifyResolveBlendReject(resolveDesc) ==
                   fuse::renderer::TaaResolveBlendRejectReason::None,
               "pass classifyResolveBlendReject is None before init");

    fuse::renderer::TaaBlendWeights weights{};
    expectTrue(pass->tryComputeResolveBlendWeights(resolveDesc, weights, blendReason),
               "pass tryComputeResolveBlendWeights passes before init");
    expectNear(weights.current, 1.f, 1e-5f, "pass tryCompute warmup current weight is full");
    expectNear(weights.history, 0.f, 1e-5f, "pass tryCompute warmup history weight is zero");

    fuse::renderer::TaaResolveSkipReason skipReason = fuse::renderer::TaaResolveSkipReason::None;
    expectTrue(!pass->tryPreflightResolve(resolveDesc, skipReason),
               "pass tryPreflightResolve fails before init");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::HistoryNotReady,
               "pass tryPreflightResolve skip reason is HistoryNotReady before init");
    expectTrue(pass->classifyResolveSkip(resolveDesc) == fuse::renderer::TaaResolveSkipReason::HistoryNotReady,
               "pass classifyResolveSkip is HistoryNotReady before init");
    expectTrue(!pass->preflightResolve(resolveDesc, &skipReason),
               "pass preflightResolve fails before init");

    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass try/classify wrapper test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);
    expectTrue(pass->init(resources), "TaaPass initialized for try/classify wrapper test");

    expectTrue(pass->tryPreflightHistoryReadyForResolve(reuseReason),
               "pass tryPreflightHistoryReadyForResolve passes after init");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::None,
               "pass tryPreflightHistoryReadyForResolve reason is None after init");
    expectTrue(!pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse fails before warmup");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::NotWarm,
               "pass tryPreflightHistoryReuse reason is NotWarm before warmup");

    expectTrue(pass->tryPreflightResolve(resolveDesc, skipReason),
               "pass tryPreflightResolve passes after init");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::None,
               "pass tryPreflightResolve skip reason is None after init");
    expectTrue(pass->classifyResolveSkip(resolveDesc) == fuse::renderer::TaaResolveSkipReason::None,
               "pass classifyResolveSkip is None after init");

    expectTrue(pass->resolveFrame(resolveDesc), "initial resolve warms pass history");

    expectTrue(pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse passes after warmup");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::None,
               "pass tryPreflightHistoryReuse reason is None after warmup");
    expectTrue(pass->tryPreflightResolveBlendWeights(resolveDesc, blendReason),
               "pass tryPreflightResolveBlendWeights passes after warmup");
    expectTrue(pass->tryComputeResolveBlendWeights(resolveDesc, weights, blendReason),
               "pass tryComputeResolveBlendWeights passes after warmup");
    expectNear(weights.current, 0.2f, 1e-5f, "pass tryCompute steady current weight matches blend factor");
    expectNear(weights.history, 0.8f, 1e-5f, "pass tryCompute steady history weight is complement");

    pass->invalidateHistory();
    expectTrue(!pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse fails after invalidate");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::StaleGeneration,
               "pass tryPreflightHistoryReuse reason is StaleGeneration after invalidate");

    fuse::renderer::TaaPassDesc zeroSeqDesc{};
    zeroSeqDesc.width = 128;
    zeroSeqDesc.height = 128;
    zeroSeqDesc.jitter.sequence_length = 0u;
    auto zeroSeqPass = fuse::renderer::TaaPass::create(zeroSeqDesc);
    expectTrue(zeroSeqPass->canAdvanceJitter(), "zero-seq pass canAdvanceJitter after fallback");
    expectTrue(zeroSeqPass->classifyJitterAdvanceReject() == fuse::renderer::TaaJitterGuardRejectReason::None,
               "zero-seq pass classifyJitterAdvanceReject is None after fallback");
    expectTrue(!zeroSeqPass->shouldSkipJitterAdvance(), "zero-seq pass should not skip jitter advance after fallback");

    fuse::renderer::TaaPassDesc zeroWidthDesc{};
    zeroWidthDesc.width = 0;
    zeroWidthDesc.height = 128;
    auto zeroWidthPass = fuse::renderer::TaaPass::create(zeroWidthDesc);
    expectTrue(zeroWidthPass->classifyJitterNdcReject() ==
                   fuse::renderer::TaaJitterGuardRejectReason::InvalidViewport,
               "zero-width pass classifyJitterNdcReject is InvalidViewport");
    expectTrue(!zeroWidthPass->tryPreflightJitterNdc(jitterReason),
               "zero-width pass tryPreflightJitterNdc fails");
    expectTrue(jitterReason == fuse::renderer::TaaJitterGuardRejectReason::InvalidViewport,
               "zero-width pass tryPreflightJitterNdc reject reason is InvalidViewport");

    pass->destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testTaaPassTryAndClassifyWrappers() {
    fuse::renderer::TaaPassDesc passDesc{};
    passDesc.width = 128;
    passDesc.height = 128;
    passDesc.params.blend_factor = 0.25f;

    auto pass = fuse::renderer::TaaPass::create(passDesc);
    expectTrue(pass->classifyJitterSyncReject() == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass classifyJitterSyncReject passes before init");
    expectTrue(pass->classifyJitterNdcReject() == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass classifyJitterNdcReject passes before init");
    expectTrue(pass->classifyJitterAdvanceReject() == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass classifyJitterAdvanceReject passes before init");
    expectTrue(!pass->shouldSkipJitterAdvance(), "pass should not skip jitter advance before init");

    fuse::renderer::TaaJitterGuardRejectReason jitterReason =
        fuse::renderer::TaaJitterGuardRejectReason::None;
    expectTrue(pass->tryPreflightJitterSync(5u, jitterReason), "pass tryPreflightJitterSync passes before init");
    expectTrue(jitterReason == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass tryPreflightJitterSync reject reason is None before init");
    expectTrue(pass->tryPreflightJitterNdc(jitterReason), "pass tryPreflightJitterNdc passes before init");
    expectTrue(pass->tryPreflightJitterAdvance(jitterReason), "pass tryPreflightJitterAdvance passes before init");

    fuse::renderer::TaaHistoryReuseBlockReason reuseReason = fuse::renderer::TaaHistoryReuseBlockReason::None;
    expectTrue(!pass->tryPreflightHistoryReadyForResolve(reuseReason),
               "pass tryPreflightHistoryReadyForResolve fails before init");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::NotReady,
               "pass tryPreflightHistoryReadyForResolve reason is NotReady before init");
    expectTrue(!pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse fails before init");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::NotReady,
               "pass tryPreflightHistoryReuse reason is NotReady before init");

    fuse::renderer::TaaResolveDesc resolveDesc{};
    resolveDesc.width = 128;
    resolveDesc.height = 128;
    resolveDesc.surfaces.current_frame = reinterpret_cast<void*>(0x10);
    resolveDesc.surfaces.output = reinterpret_cast<void*>(0x20);
    resolveDesc.params = passDesc.params;

    expectTrue(pass->classifyResolveBlendReject(resolveDesc) ==
                   fuse::renderer::TaaResolveBlendRejectReason::None,
               "pass classifyResolveBlendReject returns None before init");
    fuse::renderer::TaaResolveBlendRejectReason blendReason =
        fuse::renderer::TaaResolveBlendRejectReason::None;
    expectTrue(pass->tryPreflightResolveBlendWeights(resolveDesc, blendReason),
               "pass tryPreflightResolveBlendWeights passes before init");
    fuse::renderer::TaaBlendWeights weights{};
    expectTrue(pass->tryComputeResolveBlendWeights(resolveDesc, weights, blendReason),
               "pass tryComputeResolveBlendWeights passes before init");
    expectNear(weights.current, 1.f, 1e-5f, "pass tryCompute warmup current weight is full");

    fuse::renderer::TaaResolveSkipReason skipReason = fuse::renderer::TaaResolveSkipReason::None;
    expectTrue(pass->classifyResolveSkip(resolveDesc) == fuse::renderer::TaaResolveSkipReason::HistoryNotReady,
               "pass classifyResolveSkip is HistoryNotReady before init");
    expectTrue(!pass->tryPreflightResolve(resolveDesc, skipReason),
               "pass tryPreflightResolve fails before init");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::HistoryNotReady,
               "pass tryPreflightResolve skip reason is HistoryNotReady before init");

    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass try/classify wrapper test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);
    expectTrue(pass->init(resources), "TaaPass initialized for try/classify wrapper test");

    expectTrue(pass->tryPreflightHistoryReadyForResolve(reuseReason),
               "pass tryPreflightHistoryReadyForResolve passes after init");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::None,
               "pass tryPreflightHistoryReadyForResolve reason is None after init");
    expectTrue(pass->tryPreflightResolve(resolveDesc, skipReason),
               "pass tryPreflightResolve passes after init");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::None,
               "pass tryPreflightResolve skip reason is None after init");
    expectTrue(pass->classifyResolveSkip(resolveDesc) == fuse::renderer::TaaResolveSkipReason::None,
               "pass classifyResolveSkip is None after init");

    expectTrue(pass->resolveFrame(resolveDesc), "initial resolve warms pass history for try/classify test");
    expectTrue(pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse passes after warmup");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::None,
               "pass tryPreflightHistoryReuse reason is None after warmup");
    expectTrue(pass->tryComputeResolveBlendWeights(resolveDesc, weights, blendReason),
               "pass tryComputeResolveBlendWeights passes after warmup");
    expectNear(weights.current, 0.25f, 1e-5f, "pass tryCompute steady current weight");
    expectNear(weights.history, 0.75f, 1e-5f, "pass tryCompute steady history weight");
    expectTrue(pass->classifyResolveBlendReject(resolveDesc) ==
                   fuse::renderer::TaaResolveBlendRejectReason::None,
               "pass classifyResolveBlendReject returns None after warmup");

    pass->invalidateHistory();
    expectTrue(!pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse fails after invalidate");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::StaleGeneration,
               "pass tryPreflightHistoryReuse reason is StaleGeneration after invalidate");

    fuse::renderer::TaaPassDesc zeroWidthDesc{};
    zeroWidthDesc.width = 0;
    zeroWidthDesc.height = 128;
    auto zeroPass = fuse::renderer::TaaPass::create(zeroWidthDesc);
    expectTrue(zeroPass->classifyJitterNdcReject() ==
                   fuse::renderer::TaaJitterGuardRejectReason::InvalidViewport,
               "zero-width pass classifyJitterNdcReject is InvalidViewport");
    expectTrue(!zeroPass->tryPreflightJitterNdc(jitterReason),
               "zero-width pass tryPreflightJitterNdc fails");
    expectTrue(zeroPass->shouldSkipJitterNdc(), "zero-width pass should skip NDC jitter");

    pass->destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testTaaPassTryAndClassifyGuards() {
    fuse::renderer::TaaPassDesc passDesc{};
    passDesc.width = 128;
    passDesc.height = 128;
    passDesc.params.blend_factor = 0.3f;

    auto pass = fuse::renderer::TaaPass::create(passDesc);
    expectTrue(pass->classifyJitterSyncReject() == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass classifyJitterSyncReject passes before init");
    expectTrue(pass->classifyJitterNdcReject() == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass classifyJitterNdcReject passes before init");
    expectTrue(pass->classifyJitterAdvanceReject() == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass classifyJitterAdvanceReject passes before init");
    expectTrue(!pass->shouldSkipJitterAdvance(), "pass should not skip jitter advance before init");
    expectTrue(pass->preflightJitterAdvance(), "pass preflightJitterAdvance passes before init");

    fuse::renderer::TaaJitterGuardRejectReason jitterReason =
        fuse::renderer::TaaJitterGuardRejectReason::None;
    expectTrue(pass->tryPreflightJitterSync(6u, jitterReason), "pass tryPreflightJitterSync passes before init");
    expectTrue(jitterReason == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass tryPreflightJitterSync reject reason is None before init");
    expectTrue(pass->tryPreflightJitterNdc(jitterReason), "pass tryPreflightJitterNdc passes before init");
    expectTrue(jitterReason == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass tryPreflightJitterNdc reject reason is None before init");
    expectTrue(pass->tryPreflightJitterAdvance(jitterReason), "pass tryPreflightJitterAdvance passes before init");
    expectTrue(jitterReason == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass tryPreflightJitterAdvance reject reason is None before init");

    fuse::renderer::TaaHistoryReuseBlockReason reuseReason =
        fuse::renderer::TaaHistoryReuseBlockReason::None;
    expectTrue(!pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse fails before init");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::NotReady,
               "pass tryPreflightHistoryReuse reason is NotReady before init");
    expectTrue(!pass->tryPreflightHistoryReadyForResolve(reuseReason),
               "pass tryPreflightHistoryReadyForResolve fails before init");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::NotReady,
               "pass tryPreflightHistoryReadyForResolve reason is NotReady before init");

    fuse::renderer::TaaResolveDesc resolveDesc{};
    resolveDesc.width = 128;
    resolveDesc.height = 128;
    resolveDesc.surfaces.current_frame = reinterpret_cast<void*>(0x10);
    resolveDesc.surfaces.output = reinterpret_cast<void*>(0x20);
    resolveDesc.params = passDesc.params;

    expectTrue(pass->classifyResolveBlendReject(resolveDesc) == fuse::renderer::TaaResolveBlendRejectReason::None,
               "pass classifyResolveBlendReject passes before init");
    expectTrue(pass->classifyResolveSkip(resolveDesc) == fuse::renderer::TaaResolveSkipReason::HistoryNotReady,
               "pass classifyResolveSkip reports HistoryNotReady before init");

    fuse::renderer::TaaResolveBlendRejectReason blendReason =
        fuse::renderer::TaaResolveBlendRejectReason::None;
    expectTrue(pass->tryPreflightResolveBlendWeights(resolveDesc, blendReason),
               "pass tryPreflightResolveBlendWeights passes before init");
    expectTrue(blendReason == fuse::renderer::TaaResolveBlendRejectReason::None,
               "pass tryPreflightResolveBlendWeights reject reason is None before init");

    fuse::renderer::TaaBlendWeights weights{};
    expectTrue(pass->tryComputeResolveBlendWeights(resolveDesc, weights, blendReason),
               "pass tryComputeResolveBlendWeights passes before init");
    expectNear(weights.current, 1.f, 1e-5f, "pass tryCompute warmup current weight is full");
    expectNear(weights.history, 0.f, 1e-5f, "pass tryCompute warmup history weight is zero");

    fuse::renderer::TaaResolveSkipReason skipReason = fuse::renderer::TaaResolveSkipReason::None;
    expectTrue(!pass->tryPreflightResolve(resolveDesc, skipReason),
               "pass tryPreflightResolve fails before init");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::HistoryNotReady,
               "pass tryPreflightResolve skip reason is HistoryNotReady before init");

    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass try/classify guard test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);
    expectTrue(pass->init(resources), "TaaPass initialized for try/classify guard test");

    expectTrue(pass->tryPreflightHistoryReadyForResolve(reuseReason),
               "pass tryPreflightHistoryReadyForResolve passes after init");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::None,
               "pass tryPreflightHistoryReadyForResolve reason is None after init");
    expectTrue(!pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse fails before warmup");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::NotWarm,
               "pass tryPreflightHistoryReuse reason is NotWarm before warmup");

    expectTrue(pass->tryPreflightResolve(resolveDesc, skipReason),
               "pass tryPreflightResolve passes after init");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::None,
               "pass tryPreflightResolve skip reason is None after init");
    expectTrue(pass->classifyResolveSkip(resolveDesc) == fuse::renderer::TaaResolveSkipReason::None,
               "pass classifyResolveSkip is None after init");

    expectTrue(pass->resolveFrame(resolveDesc), "initial resolve warms pass history");

    expectTrue(pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse passes after warmup");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::None,
               "pass tryPreflightHistoryReuse reason is None after warmup");
    expectTrue(pass->tryPreflightResolveBlendWeights(resolveDesc, blendReason),
               "pass tryPreflightResolveBlendWeights passes after warmup");
    expectTrue(pass->tryComputeResolveBlendWeights(resolveDesc, weights, blendReason),
               "pass tryComputeResolveBlendWeights passes after warmup");
    expectNear(weights.current, 0.3f, 1e-5f, "pass tryCompute steady current weight");
    expectNear(weights.history, 0.7f, 1e-5f, "pass tryCompute steady history weight");

    pass->invalidateHistory();
    expectTrue(!pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse fails after invalidate");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::StaleGeneration,
               "pass tryPreflightHistoryReuse reason is StaleGeneration after invalidate");

    expectTrue(pass->syncJitterToFrameIndexIfReady(9u), "pass syncJitterToFrameIndexIfReady succeeds after init");
    expectTrue(pass->jitterAlignedToFrameIndex(9u), "pass jitter aligned after syncIfReady");
    expectTrue(pass->tryPreflightJitterSync(9u, jitterReason), "pass tryPreflightJitterSync passes after sync");
    expectTrue(pass->tryPreflightJitterAdvance(jitterReason), "pass tryPreflightJitterAdvance still passes");

    fuse::renderer::TaaPassDesc zeroWidthDesc{};
    zeroWidthDesc.width = 0;
    zeroWidthDesc.height = 128;
    auto zeroWidthPass = fuse::renderer::TaaPass::create(zeroWidthDesc);
    expectTrue(zeroWidthPass->classifyJitterNdcReject() ==
                   fuse::renderer::TaaJitterGuardRejectReason::InvalidViewport,
               "zero-width pass classifyJitterNdcReject is InvalidViewport");
    expectTrue(!zeroWidthPass->tryPreflightJitterNdc(jitterReason),
               "zero-width pass tryPreflightJitterNdc fails");
    expectTrue(jitterReason == fuse::renderer::TaaJitterGuardRejectReason::InvalidViewport,
               "zero-width pass tryPreflightJitterNdc reject reason is InvalidViewport");

    pass->destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testTaaPassTryAndClassifyPreflights() {
    fuse::renderer::TaaPassDesc passDesc{};
    passDesc.width = 128;
    passDesc.height = 128;
    passDesc.params.blend_factor = 0.25f;

    auto pass = fuse::renderer::TaaPass::create(passDesc);

    fuse::renderer::TaaHistoryReuseBlockReason reuseReason = fuse::renderer::TaaHistoryReuseBlockReason::None;
    expectTrue(!pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse fails before init");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::NotReady,
               "pass tryPreflightHistoryReuse reason is NotReady before init");

    expectTrue(!pass->tryPreflightHistoryReadyForResolve(reuseReason),
               "pass tryPreflightHistoryReadyForResolve fails before init");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::NotReady,
               "pass tryPreflightHistoryReadyForResolve reason is NotReady before init");

    fuse::renderer::TaaJitterGuardRejectReason jitterReason =
        fuse::renderer::TaaJitterGuardRejectReason::None;
    expectTrue(pass->tryPreflightJitterSync(3u, jitterReason),
               "pass tryPreflightJitterSync passes before init");
    expectTrue(jitterReason == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass tryPreflightJitterSync reason is None before init");
    expectTrue(pass->classifyJitterSyncReject() == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass classifyJitterSyncReject is None before init");
    expectTrue(pass->tryPreflightJitterNdc(jitterReason),
               "pass tryPreflightJitterNdc passes before init");
    expectTrue(pass->classifyJitterNdcReject() == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass classifyJitterNdcReject is None before init");
    expectTrue(pass->tryPreflightJitterAdvance(jitterReason),
               "pass tryPreflightJitterAdvance passes before init");
    expectTrue(pass->classifyJitterAdvanceReject() == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass classifyJitterAdvanceReject is None before init");
    expectTrue(!pass->shouldSkipJitterAdvance(), "pass should not skip jitter advance before init");

    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass try/classify preflight test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);
    expectTrue(pass->init(resources), "TaaPass initialized for try/classify preflight test");

    expectTrue(pass->tryPreflightHistoryReadyForResolve(reuseReason),
               "pass tryPreflightHistoryReadyForResolve passes after init");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::None,
               "pass tryPreflightHistoryReadyForResolve reason is None after init");

    fuse::renderer::TaaResolveDesc resolveDesc{};
    resolveDesc.width = 128;
    resolveDesc.height = 128;
    resolveDesc.surfaces.current_frame = reinterpret_cast<void*>(0x10);
    resolveDesc.surfaces.output = reinterpret_cast<void*>(0x20);
    resolveDesc.params = passDesc.params;

    fuse::renderer::TaaResolveSkipReason skipReason = fuse::renderer::TaaResolveSkipReason::None;
    expectTrue(pass->tryPreflightResolve(resolveDesc, skipReason),
               "pass tryPreflightResolve passes after init");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::None,
               "pass tryPreflightResolve skip reason is None after init");

    fuse::renderer::TaaResolveBlendRejectReason blendReason =
        fuse::renderer::TaaResolveBlendRejectReason::None;
    expectTrue(pass->tryPreflightResolveBlendWeights(resolveDesc, blendReason),
               "pass tryPreflightResolveBlendWeights passes before warmup resolve");
    expectTrue(blendReason == fuse::renderer::TaaResolveBlendRejectReason::None,
               "pass tryPreflightResolveBlendWeights reason is None before warmup resolve");
    expectTrue(pass->classifyResolveBlendReject(resolveDesc) ==
                   fuse::renderer::TaaResolveBlendRejectReason::None,
               "pass classifyResolveBlendReject is None before warmup resolve");

    fuse::renderer::TaaBlendWeights blendWeights{};
    expectTrue(pass->tryComputeResolveBlendWeights(resolveDesc, blendWeights, blendReason),
               "pass tryComputeResolveBlendWeights passes before warmup resolve");
    expectNear(blendWeights.current, 1.0f, 1e-6f,
               "pass tryComputeResolveBlendWeights current is 1.0 on warmup frame");

    expectTrue(!pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse fails before warmup resolve");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::NotWarm,
               "pass tryPreflightHistoryReuse reason is NotWarm before warmup resolve");

    expectTrue(pass->resolveFrame(resolveDesc), "initial resolve warms pass history");

    expectTrue(pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse passes after warmup resolve");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::None,
               "pass tryPreflightHistoryReuse reason is None after warmup resolve");

    expectTrue(pass->tryComputeResolveBlendWeights(resolveDesc, blendWeights, blendReason),
               "pass tryComputeResolveBlendWeights passes after warmup resolve");
    expectNear(blendWeights.history, 0.75f, 1e-6f,
               "pass tryComputeResolveBlendWeights history matches blend factor");

    pass->invalidateHistory();
    expectTrue(!pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse fails after invalidate");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::StaleGeneration,
               "pass tryPreflightHistoryReuse reason is StaleGeneration after invalidate");

    fuse::renderer::TaaPassDesc invalidSeqDesc{};
    invalidSeqDesc.width = 128;
    invalidSeqDesc.height = 128;
    invalidSeqDesc.jitter.sequence_length = 0u;
    auto invalidSeqPass = fuse::renderer::TaaPass::create(invalidSeqDesc);
    expectTrue(invalidSeqPass->jitter().sequenceLength() == 8u,
               "pass normalizes invalid jitter sequence length to default");
    expectTrue(invalidSeqPass->tryPreflightJitterSync(0u, jitterReason),
               "pass tryPreflightJitterSync passes with normalized sequence");
    expectTrue(invalidSeqPass->classifyJitterSyncReject() ==
                   fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass classifyJitterSyncReject is None with normalized sequence");
    expectTrue(!invalidSeqPass->shouldSkipJitterAdvance(),
               "pass should not skip jitter advance with normalized sequence");

    fuse::renderer::TaaPassDesc zeroWidthDesc{};
    zeroWidthDesc.width = 0;
    zeroWidthDesc.height = 128;
    auto zeroPass = fuse::renderer::TaaPass::create(zeroWidthDesc);
    expectTrue(!zeroPass->tryPreflightJitterNdc(jitterReason),
               "pass tryPreflightJitterNdc rejects zero width");
    expectTrue(jitterReason == fuse::renderer::TaaJitterGuardRejectReason::InvalidViewport,
               "pass tryPreflightJitterNdc reason is InvalidViewport");
    expectTrue(zeroPass->classifyJitterNdcReject() ==
                   fuse::renderer::TaaJitterGuardRejectReason::InvalidViewport,
               "pass classifyJitterNdcReject is InvalidViewport");

    pass->destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testTaaPassTemporalGuardsAndTryPreflight() {
    fuse::renderer::TaaPassDesc passDesc{};
    passDesc.width = 128;
    passDesc.height = 128;
    passDesc.params.blend_factor = 0.25f;

    auto pass = fuse::renderer::TaaPass::create(passDesc);

    fuse::renderer::TaaJitterGuardRejectReason jitterReject =
        fuse::renderer::TaaJitterGuardRejectReason::None;
    expectTrue(pass->tryPreflightJitterSync(3u, jitterReject),
               "pass tryPreflightJitterSync passes before init");
    expectTrue(jitterReject == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass tryPreflightJitterSync reject reason is None before init");
    expectTrue(pass->tryPreflightJitterNdc(jitterReject), "pass tryPreflightJitterNdc passes before init");
    expectTrue(pass->tryPreflightJitterAdvance(jitterReject),
               "pass tryPreflightJitterAdvance passes before init");
    expectTrue(pass->preflightJitterAdvance(), "pass preflightJitterAdvance passes before init");
    expectTrue(!pass->shouldSkipJitterAdvance(), "pass should not skip jitter advance before init");

    fuse::renderer::TaaHistoryReuseBlockReason historyReject =
        fuse::renderer::TaaHistoryReuseBlockReason::None;
    expectTrue(!pass->tryPreflightHistoryReuse(0u, historyReject),
               "pass tryPreflightHistoryReuse fails before init");
    expectTrue(historyReject == fuse::renderer::TaaHistoryReuseBlockReason::NotReady,
               "pass tryPreflightHistoryReuse reason is NotReady before init");
    expectTrue(!pass->tryPreflightHistoryReadyForResolve(historyReject),
               "pass tryPreflightHistoryReadyForResolve fails before init");
    expectTrue(historyReject == fuse::renderer::TaaHistoryReuseBlockReason::NotReady,
               "pass tryPreflightHistoryReadyForResolve reason is NotReady before init");

    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for temporal guard test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);
    expectTrue(pass->init(resources), "TaaPass initialized for temporal guard test");

    fuse::renderer::TaaResolveDesc resolveDesc{};
    resolveDesc.width = 128;
    resolveDesc.height = 128;
    resolveDesc.surfaces.current_frame = reinterpret_cast<void*>(0x10);
    resolveDesc.surfaces.output = reinterpret_cast<void*>(0x20);
    resolveDesc.params = passDesc.params;

    fuse::renderer::TaaResolveBlendRejectReason blendReject =
        fuse::renderer::TaaResolveBlendRejectReason::None;
    expectTrue(pass->tryPreflightResolveBlendWeights(resolveDesc, blendReject),
               "pass tryPreflightResolveBlendWeights passes on warmup frame");
    expectTrue(blendReject == fuse::renderer::TaaResolveBlendRejectReason::None,
               "pass tryPreflightResolveBlendWeights reject reason is None on warmup");

    fuse::renderer::TaaBlendWeights weights{};
    expectTrue(pass->tryComputeResolveBlendWeights(resolveDesc, weights, blendReject),
               "pass tryComputeResolveBlendWeights passes on warmup frame");
    expectNear(weights.current, 1.f, 1e-5f, "pass tryCompute warmup current weight is full");
    expectNear(weights.history, 0.f, 1e-5f, "pass tryCompute warmup history weight is zero");

    fuse::renderer::TaaResolveSkipReason skipReason = fuse::renderer::TaaResolveSkipReason::None;
    expectTrue(pass->tryPreflightResolve(resolveDesc, skipReason),
               "pass tryPreflightResolve passes after init");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::None,
               "pass tryPreflightResolve skip reason is None after init");

    fuse::renderer::TaaPassTemporalGuardVerdict verdict{};
    pass->evaluateTemporalGuards(5u, resolveDesc, 0u, verdict);
    expectTrue(verdict.jitterSyncOk, "temporal verdict jitter sync ok before warmup resolve");
    expectTrue(verdict.jitterNdcOk, "temporal verdict jitter NDC ok before warmup resolve");
    expectTrue(!verdict.historyWarmupComplete, "temporal verdict history not warmed before first resolve");
    expectTrue(!verdict.historyReuseOk, "temporal verdict history reuse blocked before first resolve");
    expectTrue(verdict.resolveBlendOk, "temporal verdict resolve blend ok on warmup frame");
    expectTrue(pass->preflightTemporalGuards(5u, resolveDesc, 0u, &verdict),
               "temporal guards pass on warmup frame with valid jitter and blend");
    expectTrue(!pass->shouldSkipTemporalGuards(5u, resolveDesc, 0u),
               "should not skip temporal guards on warmup frame");

    expectTrue(pass->syncJitterToFrameIndexIfReady(5u), "pass syncs jitter for temporal guard frame");
    expectTrue(pass->jitterAlignedToFrameIndex(5u), "pass jitter aligned after sync");

    expectTrue(pass->resolveFrame(resolveDesc), "initial resolve warms pass history");

    expectTrue(pass->tryPreflightHistoryReuse(0u, historyReject),
               "pass tryPreflightHistoryReuse passes after warmup");
    expectTrue(historyReject == fuse::renderer::TaaHistoryReuseBlockReason::None,
               "pass tryPreflightHistoryReuse reason is None after warmup");
    expectTrue(pass->tryPreflightHistoryReadyForResolve(historyReject),
               "pass tryPreflightHistoryReadyForResolve passes after warmup");

    pass->evaluateTemporalGuards(5u, resolveDesc, 0u, verdict);
    expectTrue(verdict.historyWarmupComplete, "temporal verdict history warmed after resolve");
    expectTrue(verdict.historyReuseOk, "temporal verdict history reuse ok after warmup");
    expectTrue(pass->preflightTemporalGuards(5u, resolveDesc, 0u, &verdict),
               "temporal guards pass after warmup");
    expectTrue(pass->tryComputeResolveBlendWeights(resolveDesc, weights, blendReject),
               "pass tryComputeResolveBlendWeights passes after warmup");
    expectNear(weights.current, 0.25f, 1e-5f, "pass tryCompute steady current weight after warmup");
    expectNear(weights.history, 0.75f, 1e-5f, "pass tryCompute steady history weight after warmup");

    pass->invalidateHistory();
    expectTrue(!pass->tryPreflightHistoryReuse(0u, historyReject),
               "pass tryPreflightHistoryReuse fails after invalidate");
    expectTrue(historyReject == fuse::renderer::TaaHistoryReuseBlockReason::StaleGeneration,
               "pass tryPreflightHistoryReuse reason is StaleGeneration after invalidate");
    expectTrue(pass->shouldSkipTemporalGuards(5u, resolveDesc, 0u),
               "should skip temporal guards after invalidate with stale generation");

    fuse::renderer::TaaPassDesc zeroWidthDesc{};
    zeroWidthDesc.width = 0;
    zeroWidthDesc.height = 128;
    auto zeroPass = fuse::renderer::TaaPass::create(zeroWidthDesc);
    expectTrue(!zeroPass->tryPreflightJitterNdc(jitterReject),
               "zero-width pass tryPreflightJitterNdc rejects invalid viewport");
    expectTrue(jitterReject == fuse::renderer::TaaJitterGuardRejectReason::InvalidViewport,
               "zero-width pass tryPreflightJitterNdc reason is InvalidViewport");
    expectTrue(zeroPass->shouldSkipJitterNdc(), "zero-width pass should skip NDC jitter");
    expectTrue(zeroPass->shouldSkipTemporalGuards(0u, resolveDesc, 0u),
               "zero-width pass should skip temporal guards");

    pass->destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testTaaPassTryPreflightAndClassifyWrappers() {
    fuse::renderer::TaaPassDesc passDesc{};
    passDesc.width = 128;
    passDesc.height = 128;
    passDesc.params.blend_factor = 0.25f;

    auto pass = fuse::renderer::TaaPass::create(passDesc);

    fuse::renderer::TaaJitterGuardRejectReason jitterReject =
        fuse::renderer::TaaJitterGuardRejectReason::None;
    expectTrue(pass->classifyJitterSyncReject() == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass classifyJitterSyncReject passes before init");
    expectTrue(pass->tryPreflightJitterSync(4u, jitterReject),
               "pass tryPreflightJitterSync passes before init");
    expectTrue(jitterReject == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass tryPreflightJitterSync reject reason is None before init");
    expectTrue(pass->classifyJitterNdcReject() == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass classifyJitterNdcReject passes before init");
    expectTrue(pass->tryPreflightJitterNdc(jitterReject),
               "pass tryPreflightJitterNdc passes before init");
    expectTrue(pass->classifyJitterAdvanceReject() == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass classifyJitterAdvanceReject passes before init");
    expectTrue(pass->tryPreflightJitterAdvance(jitterReject),
               "pass tryPreflightJitterAdvance passes before init");
    expectTrue(!pass->shouldSkipJitterAdvance(), "pass should not skip jitter advance before init");

    fuse::renderer::TaaHistoryReuseBlockReason reuseReason = fuse::renderer::TaaHistoryReuseBlockReason::None;
    expectTrue(!pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse fails before init");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::NotReady,
               "pass tryPreflightHistoryReuse reason is NotReady before init");
    expectTrue(!pass->tryPreflightHistoryReadyForResolve(reuseReason),
               "pass tryPreflightHistoryReadyForResolve fails before init");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::NotReady,
               "pass tryPreflightHistoryReadyForResolve reason is NotReady before init");

    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass try/classify wrapper test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);
    expectTrue(pass->init(resources), "TaaPass initialized for try/classify wrapper test");

    expectTrue(pass->tryPreflightHistoryReadyForResolve(reuseReason),
               "pass tryPreflightHistoryReadyForResolve passes after init");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::None,
               "pass tryPreflightHistoryReadyForResolve reason is None after init");
    expectTrue(!pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse fails before warmup");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::NotWarm,
               "pass tryPreflightHistoryReuse reason is NotWarm before warmup");

    fuse::renderer::TaaResolveDesc resolveDesc{};
    resolveDesc.width = 128;
    resolveDesc.height = 128;
    resolveDesc.surfaces.current_frame = reinterpret_cast<void*>(0x10);
    resolveDesc.surfaces.output = reinterpret_cast<void*>(0x20);
    resolveDesc.params = passDesc.params;

    fuse::renderer::TaaResolveBlendRejectReason blendReason =
        fuse::renderer::TaaResolveBlendRejectReason::None;
    expectTrue(pass->classifyResolveBlendReject(resolveDesc) ==
                   fuse::renderer::TaaResolveBlendRejectReason::None,
               "pass classifyResolveBlendReject passes before first resolve");
    expectTrue(pass->tryPreflightResolveBlendWeights(resolveDesc, blendReason),
               "pass tryPreflightResolveBlendWeights passes before first resolve");
    fuse::renderer::TaaBlendWeights weights{};
    expectTrue(pass->tryComputeResolveBlendWeights(resolveDesc, weights, blendReason),
               "pass tryComputeResolveBlendWeights passes before first resolve");
    expectNear(weights.current, 1.f, 1e-5f, "pass tryCompute warmup current weight is full");
    expectNear(weights.history, 0.f, 1e-5f, "pass tryCompute warmup history weight is zero");

    expectTrue(pass->resolveFrame(resolveDesc), "initial resolve warms pass history");

    expectTrue(pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse passes after warmup");
    expectTrue(pass->tryPreflightResolveBlendWeights(resolveDesc, blendReason),
               "pass tryPreflightResolveBlendWeights passes after warmup");
    expectTrue(pass->tryComputeResolveBlendWeights(resolveDesc, weights, blendReason),
               "pass tryComputeResolveBlendWeights passes after warmup");
    expectNear(weights.current, 0.25f, 1e-5f, "pass tryCompute steady current weight");
    expectNear(weights.history, 0.75f, 1e-5f, "pass tryCompute steady history weight");

    pass->syncJitterToFrameIndex(11u);
    expectTrue(pass->jitterAlignedToFrameIndex(11u), "pass jitter aligned after sync for classify check");
    expectTrue(pass->classifyJitterSyncReject() == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass classifyJitterSyncReject passes after init");
    expectTrue(pass->preflightJitterAdvance(), "pass preflightJitterAdvance passes after init");
    expectTrue(!pass->shouldSkipJitterAdvance(), "pass should not skip jitter advance after init");

    fuse::renderer::TaaPassDesc fallbackJitterDesc{};
    fallbackJitterDesc.width = 128;
    fallbackJitterDesc.height = 128;
    fallbackJitterDesc.jitter.sequence_length = 0u;
    auto fallbackPass = fuse::renderer::TaaPass::create(fallbackJitterDesc);
    expectTrue(fallbackPass->jitter().sequenceLength() == 8u,
               "invalid pass jitter desc falls back to default sequence length");
    expectTrue(fallbackPass->classifyJitterSyncReject() == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass classifyJitterSyncReject passes after invalid desc fallback");
    expectTrue(fallbackPass->tryPreflightJitterSync(2u, jitterReject),
               "pass tryPreflightJitterSync passes after invalid desc fallback");

    fuse::renderer::TaaPassDesc zeroWidthDesc{};
    zeroWidthDesc.width = 0;
    zeroWidthDesc.height = 128;
    auto zeroPass = fuse::renderer::TaaPass::create(zeroWidthDesc);
    expectTrue(zeroPass->classifyJitterNdcReject() ==
                   fuse::renderer::TaaJitterGuardRejectReason::InvalidViewport,
               "pass classifyJitterNdcReject detects zero width");
    expectTrue(!zeroPass->tryPreflightJitterNdc(jitterReject),
               "pass tryPreflightJitterNdc rejects zero width");
    expectTrue(jitterReject == fuse::renderer::TaaJitterGuardRejectReason::InvalidViewport,
               "pass tryPreflightJitterNdc reject reason is InvalidViewport");

    pass->invalidateHistory();
    expectTrue(!pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse fails after invalidate");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::StaleGeneration,
               "pass tryPreflightHistoryReuse reason is StaleGeneration after invalidate");

    pass->destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testTaaPassTryPreflightAndCompositeGuards() {
    fuse::renderer::TaaPassDesc passDesc{};
    passDesc.width = 128;
    passDesc.height = 128;
    passDesc.params.blend_factor = 0.25f;

    auto pass = fuse::renderer::TaaPass::create(passDesc);

    fuse::renderer::TaaHistoryReuseBlockReason reuseReason = fuse::renderer::TaaHistoryReuseBlockReason::None;
    expectTrue(!pass->tryPreflightHistoryReadyForResolve(reuseReason),
               "tryPreflightHistoryReadyForResolve fails before init");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::NotReady,
               "history ready preflight reason is NotReady before init");
    expectTrue(!pass->tryPreflightHistoryWarmup(reuseReason),
               "tryPreflightHistoryWarmup fails before init");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::NotWarm,
               "history warmup preflight reason is NotWarm before init");
    expectTrue(pass->shouldSkipHistoryTemporal(0u),
               "pass should skip temporal history before init");
    expectTrue(!pass->tryPreflightHistoryReuse(0u, reuseReason),
               "tryPreflightHistoryReuse fails before init");

    fuse::renderer::TaaJitterGuardRejectReason jitterReason =
        fuse::renderer::TaaJitterGuardRejectReason::None;
    expectTrue(pass->tryPreflightJitterSync(4u, jitterReason),
               "tryPreflightJitterSync passes before init");
    expectTrue(jitterReason == fuse::renderer::TaaJitterGuardRejectReason::None,
               "jitter sync try preflight reason is None before init");
    expectTrue(pass->tryPreflightJitterNdc(jitterReason),
               "tryPreflightJitterNdc passes before init");
    expectTrue(pass->tryPreflightJitterAdvance(jitterReason),
               "tryPreflightJitterAdvance passes before init");
    expectTrue(!pass->shouldSkipJitterAdvance(), "pass should not skip jitter advance before init");
    expectTrue(pass->preflightJitterFrame(4u, &jitterReason),
               "preflightJitterFrame passes before init");
    expectTrue(!pass->shouldSkipJitterFrame(4u), "pass should not skip jitter frame before init");

    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for try/composite guard test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);
    expectTrue(pass->init(resources), "TaaPass initialized for try/composite guard test");

    expectTrue(pass->tryPreflightHistoryReadyForResolve(reuseReason),
               "tryPreflightHistoryReadyForResolve passes after init");
    expectTrue(!pass->tryPreflightHistoryWarmup(reuseReason),
               "tryPreflightHistoryWarmup fails before first resolve");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::NotWarm,
               "history warmup preflight reason is NotWarm before first resolve");
    expectTrue(pass->shouldSkipHistoryTemporal(0u),
               "pass should skip temporal history before first resolve");

    fuse::renderer::TaaResolveDesc resolveDesc{};
    resolveDesc.width = 128;
    resolveDesc.height = 128;
    resolveDesc.surfaces.current_frame = reinterpret_cast<void*>(0x10);
    resolveDesc.surfaces.output = reinterpret_cast<void*>(0x20);
    resolveDesc.params = passDesc.params;

    fuse::renderer::TaaResolveSkipReason skipReason = fuse::renderer::TaaResolveSkipReason::None;
    expectTrue(pass->tryPreflightResolve(resolveDesc, skipReason),
               "tryPreflightResolve passes with valid desc after init");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::None,
               "tryPreflightResolve skip reason is None after init");

    fuse::renderer::TaaResolveBlendRejectReason blendReason =
        fuse::renderer::TaaResolveBlendRejectReason::None;
    expectTrue(pass->tryPreflightResolveBlendWeights(resolveDesc, blendReason),
               "tryPreflightResolveBlendWeights passes before first resolve");
    expectTrue(pass->preflightResolveWithBlend(resolveDesc, &skipReason, &blendReason),
               "preflightResolveWithBlend passes before first resolve");
    expectTrue(!pass->shouldSkipResolveWithBlend(resolveDesc),
               "pass should not skip resolve-with-blend before first resolve");

    fuse::renderer::TaaBlendWeights weights{};
    expectTrue(pass->tryComputeResolveBlendWeights(resolveDesc, weights, blendReason),
               "tryComputeResolveBlendWeights passes before first resolve");
    expectNear(weights.current, 1.f, 1e-5f, "tryCompute warmup current weight is full");
    expectNear(weights.history, 0.f, 1e-5f, "tryCompute warmup history weight is zero");

    expectTrue(pass->resolveFrame(resolveDesc), "initial resolve warms pass history");

    expectTrue(pass->tryPreflightHistoryWarmup(reuseReason),
               "tryPreflightHistoryWarmup passes after resolve");
    expectTrue(pass->tryPreflightHistoryReuse(0u, reuseReason),
               "tryPreflightHistoryReuse passes after resolve");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::None,
               "history reuse try preflight reason is None after resolve");
    expectTrue(!pass->shouldSkipHistoryTemporal(0u),
               "pass should not skip temporal history after resolve");
    expectTrue(pass->preflightHistoryTemporal(0u, &reuseReason),
               "preflightHistoryTemporal passes after resolve");

    expectTrue(pass->tryComputeResolveBlendWeights(resolveDesc, weights, blendReason),
               "tryComputeResolveBlendWeights passes after resolve");
    expectNear(weights.current, 0.25f, 1e-5f, "tryCompute steady current weight");
    expectNear(weights.history, 0.75f, 1e-5f, "tryCompute steady history weight");

    pass->syncJitterToFrameIndex(9u);
    expectTrue(pass->preflightJitterFrame(9u, &jitterReason),
               "preflightJitterFrame passes after sync");
    expectTrue(pass->jitterAlignedToFrameIndex(9u), "pass jitter aligned after sync");

    pass->invalidateHistory();
    expectTrue(!pass->tryPreflightHistoryReuse(0u, reuseReason),
               "tryPreflightHistoryReuse fails after invalidate");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::StaleGeneration,
               "history reuse try preflight reason is StaleGeneration after invalidate");
    expectTrue(pass->shouldSkipHistoryTemporal(0u),
               "pass should skip temporal history after invalidate");

    fuse::renderer::TaaPassDesc zeroWidthDesc{};
    zeroWidthDesc.width = 0;
    zeroWidthDesc.height = 128;
    auto zeroPass = fuse::renderer::TaaPass::create(zeroWidthDesc);
    expectTrue(zeroPass->shouldSkipJitterFrame(0u), "zero-width pass should skip jitter frame");
    expectTrue(!zeroPass->tryPreflightJitterNdc(jitterReason),
               "tryPreflightJitterNdc fails for zero-width pass");
    expectTrue(jitterReason == fuse::renderer::TaaJitterGuardRejectReason::InvalidViewport,
               "zero-width jitter NDC try preflight reason is InvalidViewport");

    resolveDesc.width = 0u;
    expectTrue(!pass->tryPreflightResolve(resolveDesc, skipReason),
               "tryPreflightResolve fails with invalid dimensions");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::InvalidDimensions,
               "tryPreflightResolve skip reason is InvalidDimensions");
    expectTrue(pass->shouldSkipResolveWithBlend(resolveDesc),
               "pass should skip resolve-with-blend with invalid dimensions");

    pass->destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testTaaPassTryPreflightWrappers() {
    fuse::renderer::TaaPassDesc passDesc{};
    passDesc.width = 128;
    passDesc.height = 128;
    passDesc.params.blend_factor = 0.3f;

    auto pass = fuse::renderer::TaaPass::create(passDesc);

    expectTrue(pass->classifyJitterSyncReject() == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass classifyJitterSyncReject passes before init");
    expectTrue(pass->classifyJitterNdcReject() == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass classifyJitterNdcReject passes before init");
    expectTrue(pass->classifyJitterAdvanceReject() == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass classifyJitterAdvanceReject passes before init");
    expectTrue(!pass->shouldSkipJitterAdvance(), "pass should not skip jitter advance before init");
    expectTrue(pass->preflightJitterAdvance(), "pass preflightJitterAdvance passes before init");

    fuse::renderer::TaaJitterGuardRejectReason jitterReject =
        fuse::renderer::TaaJitterGuardRejectReason::None;
    expectTrue(pass->tryPreflightJitterSync(4u, jitterReject), "pass tryPreflightJitterSync passes");
    expectTrue(jitterReject == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass tryPreflightJitterSync reject reason is None");
    expectTrue(pass->tryPreflightJitterNdc(jitterReject), "pass tryPreflightJitterNdc passes");
    expectTrue(pass->tryPreflightJitterAdvance(jitterReject), "pass tryPreflightJitterAdvance passes");

    fuse::renderer::TaaHistoryReuseBlockReason reuseReason =
        fuse::renderer::TaaHistoryReuseBlockReason::None;
    expectTrue(!pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse fails before init");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::NotReady,
               "pass tryPreflightHistoryReuse reason is NotReady before init");
    expectTrue(!pass->tryPreflightHistoryReadyForResolve(reuseReason),
               "pass tryPreflightHistoryReadyForResolve fails before init");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::NotReady,
               "pass tryPreflightHistoryReadyForResolve reason is NotReady before init");

    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass try-preflight wrapper test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);
    expectTrue(pass->init(resources), "TaaPass initialized for try-preflight wrapper test");

    expectTrue(pass->tryPreflightHistoryReadyForResolve(reuseReason),
               "pass tryPreflightHistoryReadyForResolve passes after init");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::None,
               "pass tryPreflightHistoryReadyForResolve reason is None after init");
    expectTrue(!pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse fails before warmup");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::NotWarm,
               "pass tryPreflightHistoryReuse reason is NotWarm before warmup");

    fuse::renderer::TaaResolveDesc resolveDesc{};
    resolveDesc.width = 128;
    resolveDesc.height = 128;
    resolveDesc.surfaces.current_frame = reinterpret_cast<void*>(0x10);
    resolveDesc.surfaces.output = reinterpret_cast<void*>(0x20);
    resolveDesc.params = passDesc.params;

    fuse::renderer::TaaResolveBlendRejectReason blendReject =
        fuse::renderer::TaaResolveBlendRejectReason::None;
    expectTrue(pass->classifyResolveBlendReject(resolveDesc) ==
                   fuse::renderer::TaaResolveBlendRejectReason::None,
               "pass classifyResolveBlendReject is None before warmup");
    expectTrue(pass->tryPreflightResolveBlendWeights(resolveDesc, blendReject),
               "pass tryPreflightResolveBlendWeights passes before warmup");
    fuse::renderer::TaaBlendWeights weights{};
    expectTrue(pass->tryComputeResolveBlendWeights(resolveDesc, weights, blendReject),
               "pass tryComputeResolveBlendWeights passes before warmup");
    expectNear(weights.current, 1.f, 1e-5f, "pass tryCompute warmup current weight is full");
    expectNear(weights.history, 0.f, 1e-5f, "pass tryCompute warmup history weight is zero");

    fuse::renderer::TaaResolveSkipReason skipReason = fuse::renderer::TaaResolveSkipReason::None;
    expectTrue(pass->tryPreflightResolve(resolveDesc, skipReason),
               "pass tryPreflightResolve passes with valid desc after init");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::None,
               "pass tryPreflightResolve skip reason is None");

    expectTrue(pass->resolveFrame(resolveDesc), "initial resolve warms pass history");
    expectTrue(pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse passes after warmup");
    expectTrue(pass->tryPreflightResolveBlendWeights(resolveDesc, blendReject),
               "pass tryPreflightResolveBlendWeights passes after warmup");
    expectTrue(pass->tryComputeResolveBlendWeights(resolveDesc, weights, blendReject),
               "pass tryComputeResolveBlendWeights passes after warmup");
    expectNear(weights.current, 0.3f, 1e-5f, "pass tryCompute steady current weight");
    expectNear(weights.history, 0.7f, 1e-5f, "pass tryCompute steady history weight");

    pass->invalidateHistory();
    expectTrue(!pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse fails after invalidate");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::StaleGeneration,
               "pass tryPreflightHistoryReuse reason is StaleGeneration after invalidate");

    fuse::renderer::TaaPassDesc zeroWidthDesc{};
    zeroWidthDesc.width = 0;
    zeroWidthDesc.height = 128;
    auto zeroPass = fuse::renderer::TaaPass::create(zeroWidthDesc);
    expectTrue(zeroPass->classifyJitterNdcReject() ==
                   fuse::renderer::TaaJitterGuardRejectReason::InvalidViewport,
               "zero-width pass classifyJitterNdcReject is InvalidViewport");
    expectTrue(!zeroPass->tryPreflightJitterNdc(jitterReject),
               "zero-width pass tryPreflightJitterNdc fails");
    expectTrue(jitterReject == fuse::renderer::TaaJitterGuardRejectReason::InvalidViewport,
               "zero-width pass tryPreflightJitterNdc reject reason is InvalidViewport");

    pass->destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testTaaPassTryPreflightWrappers() {
    fuse::renderer::TaaPassDesc passDesc{};
    passDesc.width = 128;
    passDesc.height = 128;
    passDesc.params.blend_factor = 0.3f;

    auto pass = fuse::renderer::TaaPass::create(passDesc);

    fuse::renderer::TaaJitterGuardRejectReason jitterReason =
        fuse::renderer::TaaJitterGuardRejectReason::None;
    expectTrue(pass->tryPreflightJitterSync(4u, jitterReason),
               "pass tryPreflightJitterSync passes before init");
    expectTrue(jitterReason == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass tryPreflightJitterSync reason is None before init");
    expectTrue(pass->tryPreflightJitterNdc(jitterReason),
               "pass tryPreflightJitterNdc passes before init");
    expectTrue(pass->tryPreflightJitterAdvance(jitterReason),
               "pass tryPreflightJitterAdvance passes before init");
    expectTrue(pass->preflightJitterAdvance(), "pass preflightJitterAdvance passes before init");
    expectTrue(!pass->shouldSkipJitterAdvance(), "pass should not skip jitter advance before init");

    fuse::renderer::TaaHistoryReuseBlockReason reuseReason =
        fuse::renderer::TaaHistoryReuseBlockReason::None;
    expectTrue(!pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse fails before init");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::NotReady,
               "pass tryPreflightHistoryReuse reason is NotReady before init");
    expectTrue(!pass->tryPreflightHistoryReadyForResolve(reuseReason),
               "pass tryPreflightHistoryReadyForResolve fails before init");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::NotReady,
               "pass tryPreflightHistoryReadyForResolve reason is NotReady before init");

    fuse::renderer::TaaResolveDesc resolveDesc{};
    resolveDesc.width = 128;
    resolveDesc.height = 128;
    resolveDesc.params = passDesc.params;
    resolveDesc.surfaces.current_frame = reinterpret_cast<void*>(0x10);
    resolveDesc.surfaces.output = reinterpret_cast<void*>(0x20);

    fuse::renderer::TaaResolveBlendRejectReason blendReason =
        fuse::renderer::TaaResolveBlendRejectReason::None;
    expectTrue(pass->tryPreflightResolveBlendWeights(resolveDesc, blendReason),
               "pass tryPreflightResolveBlendWeights passes before init");
    expectTrue(blendReason == fuse::renderer::TaaResolveBlendRejectReason::None,
               "pass tryPreflightResolveBlendWeights reason is None before init");

    fuse::renderer::TaaBlendWeights weights{};
    expectTrue(pass->tryComputeResolveBlendWeights(resolveDesc, weights, blendReason),
               "pass tryComputeResolveBlendWeights passes before init");
    expectNear(weights.current, 1.f, 1e-5f, "pass tryCompute warmup current weight before init");
    expectNear(weights.history, 0.f, 1e-5f, "pass tryCompute warmup history weight before init");

    fuse::renderer::TaaResolveSkipReason skipReason = fuse::renderer::TaaResolveSkipReason::None;
    expectTrue(!pass->tryPreflightResolve(resolveDesc, skipReason),
               "pass tryPreflightResolve fails before init");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::HistoryNotReady,
               "pass tryPreflightResolve reason is HistoryNotReady before init");
    expectTrue(pass->shouldSkipResolveFrame(resolveDesc),
               "pass shouldSkipResolveFrame before init");
    expectTrue(!pass->preflightResolveFrame(resolveDesc, &skipReason, &blendReason),
               "pass preflightResolveFrame fails before init");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::HistoryNotReady,
               "pass preflightResolveFrame skip reason is HistoryNotReady before init");

    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass try-preflight wrapper test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);
    expectTrue(pass->init(resources), "TaaPass initialized for try-preflight wrapper test");

    expectTrue(pass->tryPreflightHistoryReadyForResolve(reuseReason),
               "pass tryPreflightHistoryReadyForResolve passes after init");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::None,
               "pass tryPreflightHistoryReadyForResolve reason is None after init");
    expectTrue(!pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse fails before warmup");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::NotWarm,
               "pass tryPreflightHistoryReuse reason is NotWarm before warmup");

    expectTrue(pass->tryPreflightResolve(resolveDesc, skipReason),
               "pass tryPreflightResolve passes after init");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::None,
               "pass tryPreflightResolve reason is None after init");
    expectTrue(pass->preflightResolveFrame(resolveDesc, &skipReason, &blendReason),
               "pass preflightResolveFrame passes after init");
    expectTrue(!pass->shouldSkipResolveFrame(resolveDesc),
               "pass should not skip resolve frame after init");

    expectTrue(pass->resolveFrame(resolveDesc), "initial resolve warms pass history");

    expectTrue(pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse passes after warmup");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::None,
               "pass tryPreflightHistoryReuse reason is None after warmup");
    expectTrue(pass->tryComputeResolveBlendWeights(resolveDesc, weights, blendReason),
               "pass tryComputeResolveBlendWeights passes after warmup");
    expectNear(weights.current, 0.3f, 1e-5f, "pass tryCompute steady current weight after warmup");
    expectNear(weights.history, 0.7f, 1e-5f, "pass tryCompute steady history weight after warmup");

    pass->syncJitterToFrameIndexIfReady(11u);
    expectTrue(pass->tryPreflightJitterSync(11u, jitterReason),
               "pass tryPreflightJitterSync passes after sync");
    expectTrue(pass->jitterAlignedToFrameIndex(11u),
               "pass jitter aligned after sync for tryPreflight check");

    pass->invalidateHistory();
    expectTrue(!pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse fails after invalidate");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::StaleGeneration,
               "pass tryPreflightHistoryReuse reason is StaleGeneration after invalidate");

    fuse::renderer::TaaPassDesc zeroSeqDesc{};
    zeroSeqDesc.jitter.sequence_length = 0u;
    auto zeroSeqPass = fuse::renderer::TaaPass::create(zeroSeqDesc);
    expectTrue(zeroSeqPass->jitter().sequenceLength() == 8u,
               "zero sequence desc falls back to default length in pass jitter");
    expectTrue(!zeroSeqPass->shouldSkipJitterAdvance(),
               "fallback sequence pass should not skip jitter advance");
    expectTrue(zeroSeqPass->tryPreflightJitterAdvance(jitterReason),
               "fallback sequence pass tryPreflightJitterAdvance passes");
    expectTrue(jitterReason == fuse::renderer::TaaJitterGuardRejectReason::None,
               "fallback sequence pass tryPreflightJitterAdvance reason is None");

    fuse::renderer::TaaPassDesc zeroWidthDesc{};
    zeroWidthDesc.width = 0;
    zeroWidthDesc.height = 128;
    auto zeroWidthPass = fuse::renderer::TaaPass::create(zeroWidthDesc);
    expectTrue(!zeroWidthPass->tryPreflightJitterNdc(jitterReason),
               "zero-width pass tryPreflightJitterNdc fails");
    expectTrue(jitterReason == fuse::renderer::TaaJitterGuardRejectReason::InvalidViewport,
               "zero-width pass tryPreflightJitterNdc reason is InvalidViewport");

    pass->destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testTaaPassTryPreflightWrappers() {
    fuse::renderer::TaaPassDesc passDesc{};
    passDesc.width = 128;
    passDesc.height = 128;
    passDesc.params.blend_factor = 0.3f;

    auto pass = fuse::renderer::TaaPass::create(passDesc);

    fuse::renderer::TaaJitterGuardRejectReason jitterReject =
        fuse::renderer::TaaJitterGuardRejectReason::None;
    expectTrue(pass->tryPreflightJitterSync(4u, jitterReject), "pass tryPreflightJitterSync passes before init");
    expectTrue(jitterReject == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass tryPreflightJitterSync reject reason is None before init");
    expectTrue(pass->trySyncJitterToFrameIndex(4u, jitterReject),
               "pass trySyncJitterToFrameIndex succeeds before init");
    expectTrue(pass->jitterAlignedToFrameIndex(4u), "pass jitter aligned after trySyncJitterToFrameIndex");

    expectTrue(pass->tryPreflightJitterNdc(jitterReject), "pass tryPreflightJitterNdc passes before init");
    expectTrue(jitterReject == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass tryPreflightJitterNdc reject reason is None before init");

    const fuse::u32 jitterIndexBefore = pass->jitter().index();
    expectTrue(pass->tryPreflightJitterAdvance(jitterReject), "pass tryPreflightJitterAdvance passes before init");
    expectTrue(pass->tryAdvanceJitter(jitterReject), "pass tryAdvanceJitter succeeds before init");
    expectTrue(pass->jitter().index() != jitterIndexBefore, "pass tryAdvanceJitter advances jitter");

    fuse::renderer::TaaHistoryReuseBlockReason reuseReason =
        fuse::renderer::TaaHistoryReuseBlockReason::None;
    expectTrue(!pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse fails before init");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::NotReady,
               "pass tryPreflightHistoryReuse reason is NotReady before init");
    expectTrue(!pass->tryPreflightHistoryReadyForResolve(reuseReason),
               "pass tryPreflightHistoryReadyForResolve fails before init");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::NotReady,
               "pass tryPreflightHistoryReadyForResolve reason is NotReady before init");

    fuse::renderer::TaaResolveDesc resolveDesc{};
    resolveDesc.width = 128;
    resolveDesc.height = 128;
    resolveDesc.params = passDesc.params;
    resolveDesc.surfaces.current_frame = reinterpret_cast<void*>(0x10);
    resolveDesc.surfaces.output = reinterpret_cast<void*>(0x20);

    fuse::renderer::TaaResolveBlendRejectReason blendReject =
        fuse::renderer::TaaResolveBlendRejectReason::None;
    expectTrue(pass->tryPreflightResolveBlendWeights(resolveDesc, blendReject),
               "pass tryPreflightResolveBlendWeights passes before init");
    expectTrue(blendReject == fuse::renderer::TaaResolveBlendRejectReason::None,
               "pass tryPreflightResolveBlendWeights reject reason is None before init");

    fuse::renderer::TaaBlendWeights weights{};
    expectTrue(pass->tryComputeResolveBlendWeights(resolveDesc, weights, blendReject),
               "pass tryComputeResolveBlendWeights passes before init");
    expectNear(weights.current, 1.f, 1e-5f, "pass tryCompute warmup current weight is full");
    expectNear(weights.history, 0.f, 1e-5f, "pass tryCompute warmup history weight is zero");

    fuse::renderer::TaaResolveSkipReason skipReason = fuse::renderer::TaaResolveSkipReason::None;
    expectTrue(!pass->tryPreflightResolve(resolveDesc, skipReason),
               "pass tryPreflightResolve fails before init");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::HistoryNotReady,
               "pass tryPreflightResolve skip reason is HistoryNotReady before init");

    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass try-preflight wrapper test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);
    expectTrue(pass->init(resources), "TaaPass initialized for try-preflight wrapper test");

    expectTrue(pass->tryPreflightHistoryReadyForResolve(reuseReason),
               "pass tryPreflightHistoryReadyForResolve passes after init");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::None,
               "pass tryPreflightHistoryReadyForResolve reason is None after init");
    expectTrue(!pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse fails before warmup resolve");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::NotWarm,
               "pass tryPreflightHistoryReuse reason is NotWarm before warmup resolve");

    expectTrue(pass->tryPreflightResolve(resolveDesc, skipReason),
               "pass tryPreflightResolve passes after init");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::None,
               "pass tryPreflightResolve skip reason is None after init");
    expectTrue(pass->resolveFrame(resolveDesc), "initial resolve warms pass history");

    expectTrue(pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse passes after warmup");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::None,
               "pass tryPreflightHistoryReuse reason is None after warmup");

    expectTrue(pass->tryComputeResolveBlendWeights(resolveDesc, weights, blendReject),
               "pass tryComputeResolveBlendWeights passes after warmup");
    expectNear(weights.current, 0.3f, 1e-5f, "pass tryCompute steady current weight");
    expectNear(weights.history, 0.7f, 1e-5f, "pass tryCompute steady history weight");

    pass->invalidateHistory();
    expectTrue(!pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse fails after invalidate");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::StaleGeneration,
               "pass tryPreflightHistoryReuse reason is StaleGeneration after invalidate");

    fuse::renderer::TaaPassDesc zeroWidthDesc{};
    zeroWidthDesc.width = 0;
    zeroWidthDesc.height = 128;
    auto zeroPass = fuse::renderer::TaaPass::create(zeroWidthDesc);
    expectTrue(!zeroPass->tryPreflightJitterNdc(jitterReject),
               "zero-width pass tryPreflightJitterNdc fails");
    expectTrue(jitterReject == fuse::renderer::TaaJitterGuardRejectReason::InvalidViewport,
               "zero-width pass tryPreflightJitterNdc reject reason is InvalidViewport");
    expectTrue(zeroPass->trySyncJitterToFrameIndex(0u, jitterReject),
               "zero-width pass trySyncJitterToFrameIndex still succeeds for sequence");
    expectTrue(jitterReject == fuse::renderer::TaaJitterGuardRejectReason::None,
               "zero-width pass trySyncJitterToFrameIndex reject reason is None");

    pass->destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testTaaPassTryPreflightAndFrameGuards() {
    fuse::renderer::TaaPassDesc passDesc{};
    passDesc.width = 128;
    passDesc.height = 128;
    passDesc.params.blend_factor = 0.25f;

    auto pass = fuse::renderer::TaaPass::create(passDesc);
    expectTrue(pass->classifyJitterSyncReject() == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass classifyJitterSyncReject passes before init");
    expectTrue(pass->classifyJitterNdcReject() == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass classifyJitterNdcReject passes before init");
    expectTrue(pass->classifyJitterAdvanceReject() == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass classifyJitterAdvanceReject passes before init");
    expectTrue(pass->canAdvanceJitter(), "pass canAdvanceJitter before init");
    expectTrue(!pass->shouldSkipJitterAdvance(), "pass should not skip jitter advance before init");

    fuse::renderer::TaaJitterGuardRejectReason jitterReason =
        fuse::renderer::TaaJitterGuardRejectReason::None;
    expectTrue(pass->tryPreflightJitterSync(6u, jitterReason),
               "pass tryPreflightJitterSync passes before init");
    expectTrue(jitterReason == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass tryPreflightJitterSync reason is None before init");
    expectTrue(pass->tryPreflightJitterNdc(jitterReason),
               "pass tryPreflightJitterNdc passes before init");
    expectTrue(pass->tryPreflightJitterAdvance(jitterReason),
               "pass tryPreflightJitterAdvance passes before init");
    expectTrue(pass->preflightJitterFrame(6u, &jitterReason),
               "pass preflightJitterFrame passes before init");
    expectTrue(!pass->shouldSkipJitterFrame(6u), "pass should not skip jitter frame before init");

    fuse::renderer::TaaHistoryReuseBlockReason historyReason =
        fuse::renderer::TaaHistoryReuseBlockReason::None;
    expectTrue(!pass->tryPreflightHistoryReadyForResolve(historyReason),
               "pass tryPreflightHistoryReadyForResolve fails before init");
    expectTrue(historyReason == fuse::renderer::TaaHistoryReuseBlockReason::NotReady,
               "pass history ready reason is NotReady before init");
    expectTrue(!pass->preflightHistoryFrame(&historyReason),
               "pass preflightHistoryFrame fails before init");
    expectTrue(pass->shouldSkipHistoryFrame(), "pass should skip history frame before init");

    fuse::renderer::TaaResolveDesc resolveDesc{};
    resolveDesc.width = 128;
    resolveDesc.height = 128;
    resolveDesc.surfaces.current_frame = reinterpret_cast<void*>(0x10);
    resolveDesc.surfaces.output = reinterpret_cast<void*>(0x20);
    resolveDesc.params = passDesc.params;
    expectTrue(pass->classifyResolveBlendReject(resolveDesc) ==
                   fuse::renderer::TaaResolveBlendRejectReason::None,
               "pass classifyResolveBlendReject passes before init");

    fuse::renderer::TaaResolveBlendRejectReason blendReason =
        fuse::renderer::TaaResolveBlendRejectReason::None;
    expectTrue(pass->tryPreflightResolveBlendWeights(resolveDesc, blendReason),
               "pass tryPreflightResolveBlendWeights passes before init");
    fuse::renderer::TaaBlendWeights weights{};
    expectTrue(pass->tryComputeResolveBlendWeights(resolveDesc, weights, blendReason),
               "pass tryComputeResolveBlendWeights passes before init");
    expectNear(weights.current, 1.f, 1e-5f, "pass tryCompute warmup current weight before init");

    fuse::renderer::TaaResolveSkipReason skipReason = fuse::renderer::TaaResolveSkipReason::None;
    expectTrue(!pass->tryPreflightResolve(resolveDesc, skipReason),
               "pass tryPreflightResolve fails before init");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::HistoryNotReady,
               "pass tryPreflightResolve skip reason is HistoryNotReady before init");
    expectTrue(!pass->preflightResolveFrameGuards(resolveDesc, 6u, 0u, &skipReason),
               "pass preflightResolveFrameGuards fails before init");
    expectTrue(pass->shouldSkipResolveFrameGuards(resolveDesc, 6u, 0u),
               "pass should skip resolve frame guards before init");

    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass try/frame guard test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);
    expectTrue(pass->init(resources), "TaaPass initialized for try/frame guard test");

    expectTrue(pass->tryPreflightHistoryReadyForResolve(historyReason),
               "pass tryPreflightHistoryReadyForResolve passes after init");
    expectTrue(pass->preflightHistoryFrame(&historyReason),
               "pass preflightHistoryFrame passes after init");
    expectTrue(!pass->shouldSkipHistoryFrame(), "pass should not skip history frame after init");

    expectTrue(!pass->tryPreflightHistoryReuse(0u, historyReason),
               "pass tryPreflightHistoryReuse fails before warmup");
    expectTrue(historyReason == fuse::renderer::TaaHistoryReuseBlockReason::NotWarm,
               "pass tryPreflightHistoryReuse reason is NotWarm before warmup");
    expectTrue(pass->preflightResolveFrameGuards(resolveDesc, 6u, 0u, &skipReason),
               "pass preflightResolveFrameGuards passes after init before warmup resolve");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::None,
               "pass resolve frame guard skip reason is None after init");

    expectTrue(pass->resolveFrame(resolveDesc), "initial resolve warms pass history");
    expectTrue(pass->tryPreflightHistoryReuse(0u, historyReason),
               "pass tryPreflightHistoryReuse passes after warmup");
    expectTrue(pass->preflightResolveFrameGuards(resolveDesc, 6u, 0u, &skipReason),
               "pass preflightResolveFrameGuards passes after warmup");
    expectTrue(!pass->shouldSkipResolveFrameGuards(resolveDesc, 6u, 0u),
               "pass should not skip resolve frame guards after warmup");

    pass->syncJitterToFrameIndexIfReady(6u);
    expectTrue(pass->jitterAlignedToFrameIndex(6u),
               "pass jitter aligned after sync for frame guard test");
    expectTrue(pass->preflightJitterFrame(6u, &jitterReason),
               "pass preflightJitterFrame passes after sync");

    pass->invalidateHistory();
    expectTrue(!pass->tryPreflightHistoryReuse(0u, historyReason),
               "pass tryPreflightHistoryReuse fails after invalidate");
    expectTrue(historyReason == fuse::renderer::TaaHistoryReuseBlockReason::StaleGeneration,
               "pass tryPreflightHistoryReuse reason is StaleGeneration after invalidate");

    fuse::renderer::TaaPassDesc zeroWidthDesc{};
    zeroWidthDesc.width = 0;
    zeroWidthDesc.height = 128;
    auto zeroPass = fuse::renderer::TaaPass::create(zeroWidthDesc);
    expectTrue(zeroPass->classifyJitterNdcReject() ==
                   fuse::renderer::TaaJitterGuardRejectReason::InvalidViewport,
               "zero-width pass classifyJitterNdcReject is InvalidViewport");
    expectTrue(!zeroPass->tryPreflightJitterNdc(jitterReason),
               "zero-width pass tryPreflightJitterNdc fails");
    expectTrue(!zeroPass->preflightJitterFrame(0u, &jitterReason),
               "zero-width pass preflightJitterFrame fails");
    expectTrue(zeroPass->shouldSkipJitterFrame(0u), "zero-width pass should skip jitter frame");

    pass->destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testTaaPassTryPreflightWrappers() {
    fuse::renderer::TaaPassDesc passDesc{};
    passDesc.width = 128;
    passDesc.height = 128;
    passDesc.params.blend_factor = 0.35f;

    auto pass = fuse::renderer::TaaPass::create(passDesc);

    fuse::renderer::TaaHistoryReuseBlockReason reuseReason = fuse::renderer::TaaHistoryReuseBlockReason::None;
    expectTrue(!pass->tryPreflightHistoryReadyForResolve(reuseReason),
               "pass tryPreflightHistoryReadyForResolve fails before init");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::NotReady,
               "pass tryPreflightHistoryReadyForResolve reason is NotReady before init");

    expectTrue(!pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse fails before init");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::NotReady,
               "pass tryPreflightHistoryReuse reason is NotReady before init");

    fuse::renderer::TaaJitterGuardRejectReason jitterReject =
        fuse::renderer::TaaJitterGuardRejectReason::None;
    expectTrue(pass->tryPreflightJitterSync(4u, jitterReject), "pass tryPreflightJitterSync passes before init");
    expectTrue(jitterReject == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass tryPreflightJitterSync reject reason is None before init");
    expectTrue(pass->tryPreflightJitterNdc(jitterReject), "pass tryPreflightJitterNdc passes before init");
    expectTrue(jitterReject == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass tryPreflightJitterNdc reject reason is None before init");
    expectTrue(pass->tryPreflightJitterAdvance(jitterReject),
               "pass tryPreflightJitterAdvance passes before init");
    expectTrue(!pass->shouldSkipJitterAdvance(), "pass should not skip jitter advance before init");

    fuse::renderer::TaaResolveDesc resolveDesc{};
    resolveDesc.width = 128;
    resolveDesc.height = 128;
    resolveDesc.surfaces.current_frame = reinterpret_cast<void*>(0x10);
    resolveDesc.surfaces.output = reinterpret_cast<void*>(0x20);
    resolveDesc.params = passDesc.params;

    fuse::renderer::TaaResolveBlendRejectReason blendReason =
        fuse::renderer::TaaResolveBlendRejectReason::None;
    expectTrue(pass->tryPreflightResolveBlendWeights(resolveDesc, blendReason),
               "pass tryPreflightResolveBlendWeights passes before init");
    fuse::renderer::TaaBlendWeights weights{};
    expectTrue(pass->tryComputeResolveBlendWeights(resolveDesc, weights, blendReason),
               "pass tryComputeResolveBlendWeights passes before init");
    expectNear(weights.current, 1.f, 1e-5f, "pass tryCompute warmup current weight is full");

    fuse::renderer::TaaResolveSkipReason skipReason = fuse::renderer::TaaResolveSkipReason::None;
    expectTrue(!pass->tryPreflightResolve(resolveDesc, skipReason),
               "pass tryPreflightResolve fails before init");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::HistoryNotReady,
               "pass tryPreflightResolve skip reason is HistoryNotReady before init");

    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass try-preflight wrapper test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);
    expectTrue(pass->init(resources), "TaaPass initialized for try-preflight wrapper test");

    expectTrue(pass->tryPreflightHistoryReadyForResolve(reuseReason),
               "pass tryPreflightHistoryReadyForResolve passes after init");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::None,
               "pass tryPreflightHistoryReadyForResolve reason is None after init");
    expectTrue(!pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse fails before warmup");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::NotWarm,
               "pass tryPreflightHistoryReuse reason is NotWarm before warmup");

    expectTrue(pass->tryPreflightResolve(resolveDesc, skipReason),
               "pass tryPreflightResolve passes after init");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::None,
               "pass tryPreflightResolve skip reason is None after init");
    expectTrue(pass->resolveFrame(resolveDesc), "initial resolve warms pass history");

    expectTrue(pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse passes after warmup");
    expectTrue(pass->tryPreflightResolveBlendWeights(resolveDesc, blendReason),
               "pass tryPreflightResolveBlendWeights passes after warmup");
    expectTrue(pass->tryComputeResolveBlendWeights(resolveDesc, weights, blendReason),
               "pass tryComputeResolveBlendWeights passes after warmup");
    expectNear(weights.current, 0.35f, 1e-5f, "pass tryCompute steady current weight");
    expectNear(weights.history, 0.65f, 1e-5f, "pass tryCompute steady history weight");

    pass->invalidateHistory();
    expectTrue(!pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse fails after invalidate");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::StaleGeneration,
               "pass tryPreflightHistoryReuse reason is StaleGeneration after invalidate");

    fuse::renderer::TaaPassDesc zeroWidthDesc{};
    zeroWidthDesc.width = 0;
    zeroWidthDesc.height = 128;
    auto zeroPass = fuse::renderer::TaaPass::create(zeroWidthDesc);
    expectTrue(!zeroPass->tryPreflightJitterNdc(jitterReject),
               "zero-width pass tryPreflightJitterNdc rejects");
    expectTrue(jitterReject == fuse::renderer::TaaJitterGuardRejectReason::InvalidViewport,
               "zero-width pass tryPreflightJitterNdc reject reason is InvalidViewport");
    expectTrue(zeroPass->shouldSkipJitterNdc(), "zero-width pass should skip NDC jitter");

    pass->destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testTaaPassTryPreflightWrappers() {
    fuse::renderer::TaaPassDesc passDesc{};
    passDesc.width = 128;
    passDesc.height = 128;
    passDesc.params.blend_factor = 0.25f;

    auto pass = fuse::renderer::TaaPass::create(passDesc);

    fuse::renderer::TaaHistoryReuseBlockReason reuseReason = fuse::renderer::TaaHistoryReuseBlockReason::None;
    expectTrue(!pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse fails before init");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::NotReady,
               "pass tryPreflightHistoryReuse reason is NotReady before init");

    expectTrue(!pass->tryPreflightHistoryReadyForResolve(reuseReason),
               "pass tryPreflightHistoryReadyForResolve fails before init");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::NotReady,
               "pass tryPreflightHistoryReadyForResolve reason is NotReady before init");

    fuse::renderer::TaaJitterGuardRejectReason jitterReject =
        fuse::renderer::TaaJitterGuardRejectReason::None;
    expectTrue(pass->tryPreflightJitterSync(6u, jitterReject),
               "pass tryPreflightJitterSync passes before init");
    expectTrue(jitterReject == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass tryPreflightJitterSync reject reason is None before init");
    expectTrue(pass->classifyJitterSyncReject() == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass classifyJitterSyncReject is None before init");

    expectTrue(pass->tryPreflightJitterNdc(jitterReject),
               "pass tryPreflightJitterNdc passes before init");
    expectTrue(pass->classifyJitterNdcReject() == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass classifyJitterNdcReject is None before init");

    fuse::math::Vec2 ndcOut{};
    expectTrue(pass->ndcOffsetForFrameIndexIfReady(4u, ndcOut),
               "pass ndcOffsetForFrameIndexIfReady succeeds before init");
    const fuse::math::Vec2 directNdc =
        fuse::renderer::TaaJitterLayout::ndcOffsetForFrameIndex(4u, passDesc.width, passDesc.height);
    expectNear(ndcOut.x, directNdc.x, 1e-6f, "pass ndcOffsetForFrameIndexIfReady matches layout helper X");

    expectTrue(pass->tryPreflightJitterAdvance(jitterReject),
               "pass tryPreflightJitterAdvance passes before init");
    expectTrue(pass->classifyJitterAdvanceReject() == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass classifyJitterAdvanceReject is None before init");
    expectTrue(!pass->shouldSkipJitterAdvance(), "pass should not skip jitter advance before init");

    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass try-preflight wrapper test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);
    expectTrue(pass->init(resources), "TaaPass initialized for try-preflight wrapper test");
    expectTrue(pass->tryPreflightHistoryReadyForResolve(reuseReason),
               "pass tryPreflightHistoryReadyForResolve passes after init");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::None,
               "pass tryPreflightHistoryReadyForResolve reason is None after init");

    fuse::renderer::TaaResolveDesc resolveDesc{};
    resolveDesc.width = 128;
    resolveDesc.height = 128;
    resolveDesc.surfaces.current_frame = reinterpret_cast<void*>(0x10);
    resolveDesc.surfaces.output = reinterpret_cast<void*>(0x20);
    resolveDesc.params = passDesc.params;

    fuse::renderer::TaaResolveBlendRejectReason blendReason =
        fuse::renderer::TaaResolveBlendRejectReason::None;
    expectTrue(pass->tryPreflightResolveBlendWeights(resolveDesc, blendReason),
               "pass tryPreflightResolveBlendWeights passes before warmup resolve");
    expectTrue(pass->classifyResolveBlendReject(resolveDesc) ==
                   fuse::renderer::TaaResolveBlendRejectReason::None,
               "pass classifyResolveBlendReject is None before warmup resolve");

    fuse::renderer::TaaBlendWeights weights{};
    expectTrue(pass->tryComputeExpectedResolveBlendWeights(resolveDesc, weights, blendReason),
               "pass tryComputeExpectedResolveBlendWeights passes before warmup resolve");
    expectNear(weights.current, 1.f, 1e-5f, "pass tryCompute warmup current weight is full");
    expectNear(weights.history, 0.f, 1e-5f, "pass tryCompute warmup history weight is zero");

    fuse::renderer::TaaResolveSkipReason skipReason = fuse::renderer::TaaResolveSkipReason::None;
    expectTrue(pass->tryPreflightResolve(resolveDesc, skipReason),
               "pass tryPreflightResolve passes before warmup resolve");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::None,
               "pass tryPreflightResolve skip reason is None before warmup resolve");
    expectTrue(pass->preflightResolve(resolveDesc, &skipReason),
               "pass preflightResolve passes before warmup resolve");

    expectTrue(pass->resolveFrame(resolveDesc), "initial resolve warms pass history");

    expectTrue(pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse passes with matching generation after warmup");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::None,
               "pass tryPreflightHistoryReuse reason is None after warmup");

    expectTrue(pass->tryComputeExpectedResolveBlendWeights(resolveDesc, weights, blendReason),
               "pass tryComputeExpectedResolveBlendWeights passes after warmup");
    expectNear(weights.current, 0.25f, 1e-5f, "pass tryCompute steady current weight");
    expectNear(weights.history, 0.75f, 1e-5f, "pass tryCompute steady history weight");

    pass->syncJitterToFrameIndexIfReady(9u);
    expectTrue(pass->tryPreflightJitterSync(9u, jitterReject),
               "pass tryPreflightJitterSync passes after sync");
    expectTrue(pass->jitterAlignedToFrameIndex(9u), "pass jitter aligned after sync");

    pass->invalidateHistory();
    expectTrue(!pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse fails after invalidate");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::StaleGeneration,
               "pass tryPreflightHistoryReuse reason is StaleGeneration after invalidate");

    pass->stampObservedHistoryGeneration(resolveDesc);
    expectTrue(!pass->tryPreflightHistoryReuse(resolveDesc.observed_history_generation, reuseReason),
               "pass tryPreflightHistoryReuse fails with current generation after invalidate");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::NotWarm,
               "pass tryPreflightHistoryReuse reason is NotWarm after invalidate with current generation");

    fuse::renderer::TaaPassDesc zeroWidthDesc{};
    zeroWidthDesc.width = 0;
    zeroWidthDesc.height = 128;
    auto zeroPass = fuse::renderer::TaaPass::create(zeroWidthDesc);
    expectTrue(zeroPass->classifyJitterNdcReject() ==
                   fuse::renderer::TaaJitterGuardRejectReason::InvalidViewport,
               "zero-width pass classifyJitterNdcReject is InvalidViewport");
    expectTrue(!zeroPass->tryPreflightJitterNdc(jitterReject),
               "zero-width pass tryPreflightJitterNdc fails");
    expectTrue(!zeroPass->ndcOffsetForFrameIndexIfReady(0u, ndcOut),
               "zero-width pass ndcOffsetForFrameIndexIfReady fails");

    pass->destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testTaaPassTryPreflightWrappers() {
    fuse::renderer::TaaPassDesc passDesc{};
    passDesc.width = 128;
    passDesc.height = 128;
    passDesc.params.blend_factor = 0.4f;

    auto pass = fuse::renderer::TaaPass::create(passDesc);

    fuse::renderer::TaaHistoryReuseBlockReason reuseReason = fuse::renderer::TaaHistoryReuseBlockReason::None;
    expectTrue(!pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse fails before init");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::NotReady,
               "pass tryPreflightHistoryReuse reason is NotReady before init");
    expectTrue(!pass->tryPreflightHistoryReadyForResolve(reuseReason),
               "pass tryPreflightHistoryReadyForResolve fails before init");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::NotReady,
               "pass tryPreflightHistoryReadyForResolve reason is NotReady before init");

    fuse::renderer::TaaJitterGuardRejectReason jitterReject =
        fuse::renderer::TaaJitterGuardRejectReason::None;
    expectTrue(pass->tryPreflightJitterSync(6u, jitterReject),
               "pass tryPreflightJitterSync passes before init");
    expectTrue(jitterReject == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass tryPreflightJitterSync reject reason is None before init");
    expectTrue(pass->tryPreflightJitterNdc(jitterReject),
               "pass tryPreflightJitterNdc passes before init");
    expectTrue(jitterReject == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass tryPreflightJitterNdc reject reason is None before init");
    expectTrue(pass->tryPreflightJitterAdvance(jitterReject),
               "pass tryPreflightJitterAdvance passes before init");
    expectTrue(jitterReject == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass tryPreflightJitterAdvance reject reason is None before init");
    expectTrue(pass->preflightJitterAdvance(), "pass preflightJitterAdvance passes before init");
    expectTrue(!pass->shouldSkipJitterAdvance(), "pass should not skip jitter advance before init");

    fuse::renderer::TaaResolveDesc resolveDesc{};
    resolveDesc.width = 128;
    resolveDesc.height = 128;
    resolveDesc.surfaces.current_frame = reinterpret_cast<void*>(0x10);
    resolveDesc.surfaces.output = reinterpret_cast<void*>(0x20);
    resolveDesc.params = passDesc.params;

    fuse::renderer::TaaResolveSkipReason skipReason = fuse::renderer::TaaResolveSkipReason::None;
    expectTrue(!pass->tryPreflightResolve(resolveDesc, skipReason),
               "pass tryPreflightResolve fails before init");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::HistoryNotReady,
               "pass tryPreflightResolve skip reason is HistoryNotReady before init");

    fuse::renderer::TaaResolveBlendRejectReason blendReason =
        fuse::renderer::TaaResolveBlendRejectReason::None;
    fuse::renderer::TaaBlendWeights weights{};
    expectTrue(pass->tryPreflightResolveBlendWeights(resolveDesc, blendReason),
               "pass tryPreflightResolveBlendWeights passes before init");
    expectTrue(blendReason == fuse::renderer::TaaResolveBlendRejectReason::None,
               "pass tryPreflightResolveBlendWeights reject reason is None before init");
    expectTrue(pass->tryComputeExpectedResolveBlendWeights(resolveDesc, weights, blendReason),
               "pass tryComputeExpectedResolveBlendWeights passes before init");
    expectNear(weights.current, 1.f, 1e-5f, "pass tryCompute warmup current weight is full");
    expectNear(weights.history, 0.f, 1e-5f, "pass tryCompute warmup history weight is zero");

    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass tryPreflight wrapper test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);
    expectTrue(pass->init(resources), "TaaPass initialized for tryPreflight wrapper test");

    expectTrue(pass->tryPreflightHistoryReadyForResolve(reuseReason),
               "pass tryPreflightHistoryReadyForResolve passes after init");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::None,
               "pass tryPreflightHistoryReadyForResolve reason is None after init");
    expectTrue(!pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse fails before warmup");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::NotWarm,
               "pass tryPreflightHistoryReuse reason is NotWarm before warmup");
    expectTrue(pass->tryPreflightResolve(resolveDesc, skipReason),
               "pass tryPreflightResolve passes after init");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::None,
               "pass tryPreflightResolve skip reason is None after init");

    expectTrue(pass->resolveFrame(resolveDesc), "initial resolve warms pass history");

    expectTrue(pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse passes after warmup");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::None,
               "pass tryPreflightHistoryReuse reason is None after warmup");
    expectTrue(pass->tryComputeExpectedResolveBlendWeights(resolveDesc, weights, blendReason),
               "pass tryComputeExpectedResolveBlendWeights passes after warmup");
    expectNear(weights.current, 0.4f, 1e-5f, "pass tryCompute steady current weight");
    expectNear(weights.history, 0.6f, 1e-5f, "pass tryCompute steady history weight");

    pass->invalidateHistory();
    expectTrue(!pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse fails after invalidate");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::StaleGeneration,
               "pass tryPreflightHistoryReuse reason is StaleGeneration after invalidate");

    fuse::renderer::TaaPassDesc zeroWidthDesc{};
    zeroWidthDesc.width = 0;
    zeroWidthDesc.height = 128;
    auto zeroPass = fuse::renderer::TaaPass::create(zeroWidthDesc);
    expectTrue(!zeroPass->tryPreflightJitterNdc(jitterReject),
               "zero-width pass tryPreflightJitterNdc rejects");
    expectTrue(jitterReject == fuse::renderer::TaaJitterGuardRejectReason::InvalidViewport,
               "zero-width pass tryPreflightJitterNdc reject reason is InvalidViewport");
    expectTrue(zeroPass->shouldSkipJitterNdc(), "zero-width pass should skip NDC jitter");

    fuse::renderer::TaaPassDesc invalidSeqDesc{};
    invalidSeqDesc.width = 128;
    invalidSeqDesc.height = 128;
    invalidSeqDesc.jitter.sequence_length = 0u;
    auto invalidSeqPass = fuse::renderer::TaaPass::create(invalidSeqDesc);
    expectTrue(!invalidSeqPass->shouldSkipJitterAdvance(),
               "invalid configured sequence falls back so advance is not skipped");
    expectTrue(invalidSeqPass->tryPreflightJitterAdvance(jitterReject),
               "fallback pass tryPreflightJitterAdvance passes after defaulting sequence");
    expectTrue(jitterReject == fuse::renderer::TaaJitterGuardRejectReason::None,
               "fallback pass tryPreflightJitterAdvance reject reason is None");

    pass->destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testTaaPassTryPreflightGuards() {
    fuse::renderer::TaaPassDesc passDesc{};
    passDesc.width = 128;
    passDesc.height = 128;
    passDesc.params.blend_factor = 0.25f;

    auto pass = fuse::renderer::TaaPass::create(passDesc);

    fuse::renderer::TaaJitterGuardRejectReason jitterReject =
        fuse::renderer::TaaJitterGuardRejectReason::None;
    expectTrue(pass->tryPreflightJitterSync(4u, jitterReject),
               "pass tryPreflightJitterSync passes before init");
    expectTrue(jitterReject == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass tryPreflightJitterSync reject reason is None before init");
    expectTrue(pass->classifyJitterSyncReject() == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass classifyJitterSyncReject is None before init");
    expectTrue(pass->tryPreflightJitterNdc(jitterReject),
               "pass tryPreflightJitterNdc passes before init");
    expectTrue(jitterReject == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass tryPreflightJitterNdc reject reason is None before init");
    expectTrue(pass->classifyJitterNdcReject() == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass classifyJitterNdcReject is None before init");
    expectTrue(pass->tryPreflightJitterAdvance(jitterReject),
               "pass tryPreflightJitterAdvance passes before init");
    expectTrue(!pass->shouldSkipJitterAdvance(), "pass should not skip jitter advance before init");
    expectTrue(pass->preflightJitterAdvance(), "pass preflightJitterAdvance passes before init");

    fuse::renderer::TaaHistoryReuseBlockReason reuseReason = fuse::renderer::TaaHistoryReuseBlockReason::None;
    expectTrue(!pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse fails before init");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::NotReady,
               "pass tryPreflightHistoryReuse reason is NotReady before init");
    expectTrue(!pass->tryPreflightHistoryReadyForResolve(reuseReason),
               "pass tryPreflightHistoryReadyForResolve fails before init");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::NotReady,
               "pass tryPreflightHistoryReadyForResolve reason is NotReady before init");

    fuse::renderer::TaaResolveDesc resolveDesc{};
    resolveDesc.width = 128;
    resolveDesc.height = 128;
    resolveDesc.surfaces.current_frame = reinterpret_cast<void*>(0x10);
    resolveDesc.surfaces.output = reinterpret_cast<void*>(0x20);
    resolveDesc.params = passDesc.params;

    fuse::renderer::TaaResolveBlendRejectReason blendReason =
        fuse::renderer::TaaResolveBlendRejectReason::None;
    expectTrue(pass->tryPreflightResolveBlendWeights(resolveDesc, blendReason),
               "pass tryPreflightResolveBlendWeights passes before init");
    expectTrue(blendReason == fuse::renderer::TaaResolveBlendRejectReason::None,
               "pass tryPreflightResolveBlendWeights reject reason is None before init");
    expectTrue(pass->classifyResolveBlendReject(resolveDesc) ==
                   fuse::renderer::TaaResolveBlendRejectReason::None,
               "pass classifyResolveBlendReject is None before init");

    fuse::renderer::TaaBlendWeights weights{};
    expectTrue(pass->tryComputeResolveBlendWeights(resolveDesc, weights, blendReason),
               "pass tryComputeResolveBlendWeights passes before init");
    expectNear(weights.current, 1.f, 1e-5f, "pass tryCompute warmup current weight is full");
    expectNear(weights.history, 0.f, 1e-5f, "pass tryCompute warmup history weight is zero");

    fuse::renderer::TaaResolveSkipReason skipReason = fuse::renderer::TaaResolveSkipReason::None;
    expectTrue(!pass->tryPreflightResolve(resolveDesc, skipReason),
               "pass tryPreflightResolve fails before init");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::HistoryNotReady,
               "pass tryPreflightResolve skip reason is HistoryNotReady before init");

    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass try-preflight guard test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);
    expectTrue(pass->init(resources), "TaaPass initialized for try-preflight guard test");

    expectTrue(pass->tryPreflightHistoryReadyForResolve(reuseReason),
               "pass tryPreflightHistoryReadyForResolve passes after init");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::None,
               "pass tryPreflightHistoryReadyForResolve reason is None after init");
    expectTrue(!pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse fails before warmup");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::NotWarm,
               "pass tryPreflightHistoryReuse reason is NotWarm before warmup");

    expectTrue(pass->tryPreflightResolve(resolveDesc, skipReason),
               "pass tryPreflightResolve passes after init");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::None,
               "pass tryPreflightResolve skip reason is None after init");
    expectTrue(pass->resolveFrame(resolveDesc), "initial resolve warms pass history");

    expectTrue(pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse passes after warmup");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::None,
               "pass tryPreflightHistoryReuse reason is None after warmup");
    expectTrue(pass->tryComputeResolveBlendWeights(resolveDesc, weights, blendReason),
               "pass tryComputeResolveBlendWeights passes after warmup");
    expectNear(weights.current, 0.25f, 1e-5f, "pass tryCompute steady current weight");
    expectNear(weights.history, 0.75f, 1e-5f, "pass tryCompute steady history weight");

    pass->invalidateHistory();
    expectTrue(!pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse fails after invalidate");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::StaleGeneration,
               "pass tryPreflightHistoryReuse reason is StaleGeneration after invalidate");

    fuse::renderer::TaaPassDesc zeroWidthDesc{};
    zeroWidthDesc.width = 0;
    zeroWidthDesc.height = 128;
    auto zeroPass = fuse::renderer::TaaPass::create(zeroWidthDesc);
    expectTrue(!zeroPass->tryPreflightJitterNdc(jitterReject),
               "zero-width pass tryPreflightJitterNdc fails");
    expectTrue(jitterReject == fuse::renderer::TaaJitterGuardRejectReason::InvalidViewport,
               "zero-width pass tryPreflightJitterNdc reject reason is InvalidViewport");
    expectTrue(zeroPass->classifyJitterNdcReject() ==
                   fuse::renderer::TaaJitterGuardRejectReason::InvalidViewport,
               "zero-width pass classifyJitterNdcReject is InvalidViewport");
    expectTrue(zeroPass->shouldSkipJitterNdc(), "zero-width pass should skip NDC jitter");
    expectTrue(zeroPass->tryPreflightJitterSync(0u, jitterReject),
               "zero-width pass tryPreflightJitterSync still valid for sequence");

    fuse::renderer::TaaPassDesc invalidSeqDesc{};
    invalidSeqDesc.width = 128;
    invalidSeqDesc.height = 128;
    invalidSeqDesc.jitter.sequence_length = 0u;
    auto invalidSeqPass = fuse::renderer::TaaPass::create(invalidSeqDesc);
    expectTrue(invalidSeqPass->jitter().sequenceLength() == 8u,
               "invalid sequence desc falls back to default sequence length on pass");
    expectTrue(invalidSeqPass->tryPreflightJitterAdvance(jitterReject),
               "pass tryPreflightJitterAdvance passes after fallback sequence length");
    expectTrue(!invalidSeqPass->shouldSkipJitterAdvance(),
               "pass should not skip jitter advance after fallback sequence length");

    pass->destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testTaaPassTryPreflightAndClassifyWrappers() {
    fuse::renderer::TaaPassDesc passDesc{};
    passDesc.width = 128;
    passDesc.height = 128;
    passDesc.params.blend_factor = 0.25f;

    auto pass = fuse::renderer::TaaPass::create(passDesc);
    expectTrue(pass->classifyJitterSyncReject() == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass classifyJitterSyncReject is None for valid sequence");
    expectTrue(pass->classifyJitterNdcReject() == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass classifyJitterNdcReject is None for valid viewport");
    expectTrue(pass->classifyJitterAdvanceReject() == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass classifyJitterAdvanceReject is None for valid sequence");
    expectTrue(!pass->shouldSkipJitterAdvance(), "pass should not skip jitter advance for valid sequence");
    expectTrue(pass->preflightJitterAdvance(), "pass preflightJitterAdvance passes for valid sequence");

    fuse::renderer::TaaJitterGuardRejectReason jitterReject =
        fuse::renderer::TaaJitterGuardRejectReason::None;
    expectTrue(pass->tryPreflightJitterSync(6u, jitterReject),
               "pass tryPreflightJitterSync passes for valid sequence");
    expectTrue(jitterReject == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass tryPreflightJitterSync reject reason is None");
    expectTrue(pass->tryPreflightJitterNdc(jitterReject),
               "pass tryPreflightJitterNdc passes for valid viewport");
    expectTrue(pass->tryPreflightJitterAdvance(jitterReject),
               "pass tryPreflightJitterAdvance passes for valid sequence");

    fuse::math::Vec2 ndcOut{};
    expectTrue(pass->ndcOffsetForFrameIndexIfReady(6u, ndcOut),
               "pass ndcOffsetForFrameIndexIfReady succeeds for valid viewport");
    const fuse::math::Vec2 directNdc =
        fuse::renderer::TaaJitterLayout::ndcOffsetForFrameIndex(6u, 128u, 128u, 8u);
    expectNear(ndcOut.x, directNdc.x, 1e-6f,
               "pass ndcOffsetForFrameIndexIfReady matches layout helper X");

    fuse::renderer::TaaHistoryReuseBlockReason reuseReason = fuse::renderer::TaaHistoryReuseBlockReason::None;
    expectTrue(!pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse fails before init");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::NotReady,
               "pass tryPreflightHistoryReuse reason is NotReady before init");
    expectTrue(!pass->preflightHistoryReadyForResolve(&reuseReason),
               "pass preflightHistoryReadyForResolve fails before init");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::NotReady,
               "pass preflightHistoryReadyForResolve reason is NotReady before init");

    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass try/classify wrapper test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);
    expectTrue(pass->init(resources), "TaaPass initialized for try/classify wrapper test");
    expectTrue(pass->tryPreflightHistoryReadyForResolve(reuseReason),
               "pass tryPreflightHistoryReadyForResolve passes after init");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::None,
               "pass tryPreflightHistoryReadyForResolve reason is None after init");

    fuse::renderer::TaaResolveDesc resolveDesc{};
    resolveDesc.width = 128;
    resolveDesc.height = 128;
    resolveDesc.surfaces.current_frame = reinterpret_cast<void*>(0x10);
    resolveDesc.surfaces.output = reinterpret_cast<void*>(0x20);
    resolveDesc.params = passDesc.params;

    fuse::renderer::TaaResolveBlendRejectReason blendReason =
        fuse::renderer::TaaResolveBlendRejectReason::None;
    expectTrue(pass->classifyResolveBlendReject(resolveDesc) ==
                   fuse::renderer::TaaResolveBlendRejectReason::None,
               "pass classifyResolveBlendReject is None before warmup resolve");
    expectTrue(pass->tryPreflightResolveBlendWeights(resolveDesc, blendReason),
               "pass tryPreflightResolveBlendWeights passes before warmup resolve");
    fuse::renderer::TaaBlendWeights blendWeights{};
    expectTrue(pass->tryComputeExpectedResolveBlendWeights(resolveDesc, blendWeights, blendReason),
               "pass tryComputeExpectedResolveBlendWeights passes before warmup resolve");
    expectNear(blendWeights.current, 1.f, 1e-5f,
               "pass tryComputeExpectedResolveBlendWeights warmup current weight is 1");

    fuse::renderer::TaaResolveSkipReason skipReason = fuse::renderer::TaaResolveSkipReason::None;
    expectTrue(pass->tryPreflightResolve(resolveDesc, skipReason),
               "pass tryPreflightResolve passes with valid desc after init");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::None,
               "pass tryPreflightResolve skip reason is None after init");

    expectTrue(pass->resolveFrame(resolveDesc), "initial resolve warms pass history");
    expectTrue(pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse passes after warmup");
    expectTrue(pass->classifyHistoryReuseBlock(0u) == fuse::renderer::TaaHistoryReuseBlockReason::None,
               "pass classifyHistoryReuseBlock is None after warmup");

    pass->invalidateHistory();
    expectTrue(pass->classifyHistoryReuseBlock(0u) ==
                   fuse::renderer::TaaHistoryReuseBlockReason::StaleGeneration,
               "pass classifyHistoryReuseBlock is StaleGeneration after invalidate");

    fuse::renderer::TaaPassDesc invalidJitterDesc{};
    invalidJitterDesc.width = 128;
    invalidJitterDesc.height = 128;
    invalidJitterDesc.jitter.sequence_length = 0u;
    auto invalidPass = fuse::renderer::TaaPass::create(invalidJitterDesc);
    expectTrue(invalidPass->classifyJitterSyncReject() ==
                   fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass classifyJitterSyncReject uses fallback sequence length");
    expectTrue(!invalidPass->shouldSkipJitterAdvance(),
               "pass should not skip jitter advance after invalid desc fallback");
    expectTrue(invalidPass->tryPreflightJitterSync(2u, jitterReject),
               "pass tryPreflightJitterSync succeeds after invalid desc fallback");

    fuse::renderer::TaaPassDesc zeroWidthDesc{};
    zeroWidthDesc.width = 0;
    zeroWidthDesc.height = 128;
    auto zeroPass = fuse::renderer::TaaPass::create(zeroWidthDesc);
    expectTrue(zeroPass->classifyJitterNdcReject() ==
                   fuse::renderer::TaaJitterGuardRejectReason::InvalidViewport,
               "zero-width pass classifyJitterNdcReject is InvalidViewport");
    expectTrue(!zeroPass->tryPreflightJitterNdc(jitterReject),
               "zero-width pass tryPreflightJitterNdc rejects");
    expectTrue(!zeroPass->ndcOffsetForFrameIndexIfReady(0u, ndcOut),
               "zero-width pass ndcOffsetForFrameIndexIfReady fails");

    pass->destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testTaaPassTryPreflightWrappers() {
    fuse::renderer::TaaPassDesc passDesc{};
    passDesc.width = 128;
    passDesc.height = 128;
    passDesc.params.blend_factor = 0.25f;

    auto pass = fuse::renderer::TaaPass::create(passDesc);

    fuse::renderer::TaaJitterGuardRejectReason jitterReason =
        fuse::renderer::TaaJitterGuardRejectReason::None;
    expectTrue(pass->tryPreflightJitterSync(4u, jitterReason),
               "pass tryPreflightJitterSync passes before init");
    expectTrue(jitterReason == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass tryPreflightJitterSync reason is None before init");
    expectTrue(pass->tryPreflightJitterNdc(jitterReason),
               "pass tryPreflightJitterNdc passes before init");
    expectTrue(jitterReason == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass tryPreflightJitterNdc reason is None before init");
    expectTrue(pass->tryPreflightJitterAdvance(jitterReason),
               "pass tryPreflightJitterAdvance passes before init");
    expectTrue(!pass->shouldSkipJitterAdvance(), "pass should not skip jitter advance before init");
    expectTrue(pass->preflightJitterAdvance(), "pass preflightJitterAdvance passes before init");

    fuse::renderer::TaaHistoryReuseBlockReason reuseReason =
        fuse::renderer::TaaHistoryReuseBlockReason::None;
    expectTrue(!pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse fails before init");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::NotReady,
               "pass tryPreflightHistoryReuse reason is NotReady before init");
    expectTrue(!pass->tryPreflightHistoryReadyForResolve(reuseReason),
               "pass tryPreflightHistoryReadyForResolve fails before init");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::NotReady,
               "pass tryPreflightHistoryReadyForResolve reason is NotReady before init");

    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass try-preflight wrapper test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);
    expectTrue(pass->init(resources), "TaaPass initialized for try-preflight wrapper test");

    expectTrue(pass->tryPreflightHistoryReadyForResolve(reuseReason),
               "pass tryPreflightHistoryReadyForResolve passes after init");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::None,
               "pass tryPreflightHistoryReadyForResolve reason is None after init");
    expectTrue(!pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse fails before warmup");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::NotWarm,
               "pass tryPreflightHistoryReuse reason is NotWarm before warmup");

    fuse::renderer::TaaResolveDesc resolveDesc{};
    resolveDesc.width = 128;
    resolveDesc.height = 128;
    resolveDesc.surfaces.current_frame = reinterpret_cast<void*>(0x10);
    resolveDesc.surfaces.output = reinterpret_cast<void*>(0x20);
    resolveDesc.params = passDesc.params;

    fuse::renderer::TaaResolveBlendRejectReason blendReason =
        fuse::renderer::TaaResolveBlendRejectReason::None;
    expectTrue(pass->tryPreflightResolveBlendWeights(resolveDesc, blendReason),
               "pass tryPreflightResolveBlendWeights passes before warmup");
    expectTrue(blendReason == fuse::renderer::TaaResolveBlendRejectReason::None,
               "pass tryPreflightResolveBlendWeights reason is None before warmup");
    expectTrue(pass->classifyResolveBlendReject(resolveDesc) ==
                   fuse::renderer::TaaResolveBlendRejectReason::None,
               "pass classifyResolveBlendReject is None before warmup");

    fuse::renderer::TaaBlendWeights weights{};
    expectTrue(pass->tryComputeResolveBlendWeights(resolveDesc, weights, blendReason),
               "pass tryComputeResolveBlendWeights passes before warmup");
    expectNear(weights.current, 1.f, 1e-5f, "pass tryCompute warmup current weight is full");
    expectNear(weights.history, 0.f, 1e-5f, "pass tryCompute warmup history weight is zero");

    expectTrue(pass->resolveFrame(resolveDesc), "initial resolve warms pass history");

    expectTrue(pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse passes after warmup");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::None,
               "pass tryPreflightHistoryReuse reason is None after warmup");
    expectTrue(pass->tryComputeResolveBlendWeights(resolveDesc, weights, blendReason),
               "pass tryComputeResolveBlendWeights passes after warmup");
    expectNear(weights.current, 0.25f, 1e-5f, "pass tryCompute steady current weight");
    expectNear(weights.history, 0.75f, 1e-5f, "pass tryCompute steady history weight");

    pass->invalidateHistory();
    expectTrue(!pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse fails after invalidate");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::StaleGeneration,
               "pass tryPreflightHistoryReuse reason is StaleGeneration after invalidate");

    fuse::renderer::TaaPassDesc zeroWidthDesc{};
    zeroWidthDesc.width = 0;
    zeroWidthDesc.height = 128;
    auto zeroPass = fuse::renderer::TaaPass::create(zeroWidthDesc);
    expectTrue(!zeroPass->tryPreflightJitterNdc(jitterReason),
               "zero-width pass tryPreflightJitterNdc fails");
    expectTrue(jitterReason == fuse::renderer::TaaJitterGuardRejectReason::InvalidViewport,
               "zero-width pass tryPreflightJitterNdc reason is InvalidViewport");
    expectTrue(!zeroPass->shouldSkipJitterAdvance(), "zero-width pass should not skip jitter advance");

    pass->destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testTaaPassTryPreflightGuardWrappers() {
    fuse::renderer::TaaPassDesc passDesc{};
    passDesc.width = 128;
    passDesc.height = 128;
    passDesc.params.blend_factor = 0.25f;

    auto pass = fuse::renderer::TaaPass::create(passDesc);

    fuse::renderer::TaaJitterGuardRejectReason jitterReject =
        fuse::renderer::TaaJitterGuardRejectReason::None;
    expectTrue(pass->tryPreflightJitterSync(4u, jitterReject),
               "pass tryPreflightJitterSync passes before init");
    expectTrue(jitterReject == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass tryPreflightJitterSync reject reason is None before init");
    expectTrue(pass->classifyJitterSyncReject(4u) == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass classifyJitterSyncReject is None before init");
    expectTrue(pass->classifyJitterNdcReject() == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass classifyJitterNdcReject is None before init");
    expectTrue(pass->classifyJitterAdvanceReject() == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass classifyJitterAdvanceReject is None before init");
    expectTrue(pass->preflightJitterAdvance(), "pass preflightJitterAdvance passes before init");
    expectTrue(pass->tryPreflightJitterAdvance(jitterReject),
               "pass tryPreflightJitterAdvance passes before init");
    expectTrue(!pass->shouldSkipJitterAdvance(), "pass should not skip jitter advance before init");

    fuse::math::Vec2 ndcOut{};
    expectTrue(pass->ndcOffsetForFrameIndexIfReady(3u, ndcOut),
               "pass ndcOffsetForFrameIndexIfReady succeeds before init");
    const fuse::math::Vec2 directNdc =
        fuse::renderer::TaaJitterLayout::ndcOffsetForFrameIndex(3u, 128u, 128u, 8u);
    expectNear(ndcOut.x, directNdc.x, 1e-6f, "pass ndcOffsetForFrameIndexIfReady matches layout helper X");

    expectTrue(pass->trySyncJitterToFrameIndex(6u, jitterReject),
               "pass trySyncJitterToFrameIndex succeeds before init");
    expectTrue(pass->jitterAlignedToFrameIndex(6u), "pass jitter aligned after trySyncJitterToFrameIndex");

    const fuse::u32 jitterIndexBefore = pass->jitter().index();
    expectTrue(pass->tryAdvanceJitter(jitterReject), "pass tryAdvanceJitter succeeds before init");
    expectTrue(pass->jitter().index() != jitterIndexBefore, "pass tryAdvanceJitter advances jitter");

    fuse::renderer::TaaHistoryReuseBlockReason reuseReason = fuse::renderer::TaaHistoryReuseBlockReason::None;
    expectTrue(!pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse fails before init");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::NotReady,
               "pass tryPreflightHistoryReuse reason is NotReady before init");
    expectTrue(!pass->tryPreflightHistoryReadyForResolve(reuseReason),
               "pass tryPreflightHistoryReadyForResolve fails before init");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::NotReady,
               "pass tryPreflightHistoryReadyForResolve reason is NotReady before init");

    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass try-preflight wrapper test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);
    expectTrue(pass->init(resources), "TaaPass initialized for try-preflight wrapper test");

    expectTrue(pass->tryPreflightHistoryReadyForResolve(reuseReason),
               "pass tryPreflightHistoryReadyForResolve passes after init");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::None,
               "pass tryPreflightHistoryReadyForResolve reason is None after init");
    expectTrue(!pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse fails before warmup");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::NotWarm,
               "pass tryPreflightHistoryReuse reason is NotWarm before warmup");

    fuse::renderer::TaaResolveDesc resolveDesc{};
    resolveDesc.width = 128;
    resolveDesc.height = 128;
    resolveDesc.surfaces.current_frame = reinterpret_cast<void*>(0x10);
    resolveDesc.surfaces.output = reinterpret_cast<void*>(0x20);
    resolveDesc.params = passDesc.params;

    fuse::renderer::TaaResolveBlendRejectReason blendReason =
        fuse::renderer::TaaResolveBlendRejectReason::None;
    expectTrue(pass->tryPreflightResolveBlendWeights(resolveDesc, blendReason),
               "pass tryPreflightResolveBlendWeights passes before warmup");
    expectTrue(pass->classifyResolveBlendReject(resolveDesc) ==
                   fuse::renderer::TaaResolveBlendRejectReason::None,
               "pass classifyResolveBlendReject is None before warmup");

    fuse::renderer::TaaBlendWeights weights{};
    expectTrue(pass->tryComputeResolveBlendWeights(resolveDesc, weights, blendReason),
               "pass tryComputeResolveBlendWeights passes before warmup");
    expectNear(weights.current, 1.f, 1e-5f, "pass tryCompute warmup current weight is full");
    expectNear(weights.history, 0.f, 1e-5f, "pass tryCompute warmup history weight is zero");

    fuse::renderer::TaaResolveSkipReason skipReason = fuse::renderer::TaaResolveSkipReason::None;
    expectTrue(pass->tryPreflightResolve(resolveDesc, skipReason),
               "pass tryPreflightResolve passes before warmup");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::None,
               "pass tryPreflightResolve skip reason is None before warmup");

    expectTrue(pass->resolveFrame(resolveDesc), "initial resolve warms pass history");

    expectTrue(pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse passes after warmup");
    expectTrue(pass->tryPreflightResolveBlendWeights(resolveDesc, blendReason),
               "pass tryPreflightResolveBlendWeights passes after warmup");
    expectTrue(pass->tryComputeResolveBlendWeights(resolveDesc, weights, blendReason),
               "pass tryComputeResolveBlendWeights passes after warmup");
    expectNear(weights.current, 0.25f, 1e-5f, "pass tryCompute steady current weight");
    expectNear(weights.history, 0.75f, 1e-5f, "pass tryCompute steady history weight");

    pass->invalidateHistory();
    expectTrue(!pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse fails after invalidate");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::StaleGeneration,
               "pass tryPreflightHistoryReuse reason is StaleGeneration after invalidate");

    fuse::renderer::TaaPassDesc zeroWidthDesc{};
    zeroWidthDesc.width = 0;
    zeroWidthDesc.height = 128;
    auto zeroPass = fuse::renderer::TaaPass::create(zeroWidthDesc);
    expectTrue(!zeroPass->tryPreflightJitterNdc(jitterReject),
               "zero-width pass tryPreflightJitterNdc fails");
    expectTrue(zeroPass->classifyJitterNdcReject() ==
                   fuse::renderer::TaaJitterGuardRejectReason::InvalidViewport,
               "zero-width pass classifyJitterNdcReject is InvalidViewport");
    expectTrue(!zeroPass->ndcOffsetForFrameIndexIfReady(0u, ndcOut),
               "zero-width pass ndcOffsetForFrameIndexIfReady fails");

    pass->destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testTaaPassTryClassifyWrappers() {
    fuse::renderer::TaaPassDesc passDesc{};
    passDesc.width = 128;
    passDesc.height = 128;
    passDesc.params.blend_factor = 0.2f;

    auto pass = fuse::renderer::TaaPass::create(passDesc);

    fuse::renderer::TaaHistoryReuseBlockReason reuseReason = fuse::renderer::TaaHistoryReuseBlockReason::None;
    expectTrue(!pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse fails before init");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::NotReady,
               "pass tryPreflightHistoryReuse reason is NotReady before init");
    expectTrue(!pass->tryPreflightHistoryReadyForResolve(reuseReason),
               "pass tryPreflightHistoryReadyForResolve fails before init");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::NotReady,
               "pass tryPreflightHistoryReadyForResolve reason is NotReady before init");

    expectTrue(pass->classifyJitterSyncReject() == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass classifyJitterSyncReject passes before init");
    expectTrue(pass->classifyJitterNdcReject() == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass classifyJitterNdcReject passes before init");
    expectTrue(pass->classifyJitterAdvanceReject() == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass classifyJitterAdvanceReject passes before init");
    expectTrue(!pass->shouldSkipJitterAdvance(), "pass should not skip jitter advance before init");

    fuse::renderer::TaaJitterGuardRejectReason jitterReason =
        fuse::renderer::TaaJitterGuardRejectReason::None;
    expectTrue(pass->tryPreflightJitterSync(4u, jitterReason), "pass tryPreflightJitterSync passes before init");
    expectTrue(jitterReason == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass tryPreflightJitterSync reject reason is None");
    expectTrue(pass->tryPreflightJitterNdc(jitterReason), "pass tryPreflightJitterNdc passes before init");
    expectTrue(pass->tryPreflightJitterAdvance(jitterReason), "pass tryPreflightJitterAdvance passes before init");
    expectTrue(pass->trySyncJitterToFrameIndexIfReady(4u, jitterReason),
               "pass trySyncJitterToFrameIndexIfReady succeeds before init");
    expectTrue(pass->jitterAlignedToFrameIndex(4u), "pass jitter aligned after trySync");

    const fuse::u32 jitterIndexBefore = pass->jitter().index();
    expectTrue(pass->tryAdvanceJitterIfReady(jitterReason), "pass tryAdvanceJitterIfReady succeeds before init");
    expectTrue(pass->jitter().index() != jitterIndexBefore, "pass tryAdvanceJitterIfReady advances jitter");

    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass try/classify wrapper test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);
    expectTrue(pass->init(resources), "TaaPass initialized for try/classify wrapper test");
    expectTrue(pass->tryPreflightHistoryReadyForResolve(reuseReason),
               "pass tryPreflightHistoryReadyForResolve passes after init");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::None,
               "pass tryPreflightHistoryReadyForResolve reason is None after init");

    fuse::renderer::TaaResolveDesc resolveDesc{};
    resolveDesc.width = 128;
    resolveDesc.height = 128;
    resolveDesc.surfaces.current_frame = reinterpret_cast<void*>(0x10);
    resolveDesc.surfaces.output = reinterpret_cast<void*>(0x20);
    resolveDesc.params = passDesc.params;

    fuse::renderer::TaaResolveBlendRejectReason blendReason =
        fuse::renderer::TaaResolveBlendRejectReason::None;
    expectTrue(pass->tryPreflightResolveBlendWeights(resolveDesc, blendReason),
               "pass tryPreflightResolveBlendWeights passes before warmup");
    expectTrue(pass->classifyResolveBlendReject(resolveDesc) ==
                   fuse::renderer::TaaResolveBlendRejectReason::None,
               "pass classifyResolveBlendReject is None before warmup");

    fuse::renderer::TaaBlendWeights weights{};
    expectTrue(pass->tryComputeResolveBlendWeights(resolveDesc, weights, blendReason),
               "pass tryComputeResolveBlendWeights passes before warmup");
    expectNear(weights.current, 1.f, 1e-5f, "pass tryCompute warmup current weight is full");
    expectNear(weights.history, 0.f, 1e-5f, "pass tryCompute warmup history weight is zero");

    fuse::renderer::TaaResolveSkipReason skipReason = fuse::renderer::TaaResolveSkipReason::None;
    expectTrue(pass->tryPreflightResolve(resolveDesc, skipReason),
               "pass tryPreflightResolve passes before warmup resolve");
    expectTrue(pass->classifyResolveSkip(resolveDesc) == fuse::renderer::TaaResolveSkipReason::None,
               "pass classifyResolveSkip is None before warmup resolve");

    expectTrue(pass->resolveFrame(resolveDesc), "initial resolve warms pass history");

    expectTrue(pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse passes after warmup");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::None,
               "pass tryPreflightHistoryReuse reason is None after warmup");

    expectTrue(pass->tryPreflightResolveBlendWeights(resolveDesc, blendReason),
               "pass tryPreflightResolveBlendWeights passes after warmup");
    expectTrue(pass->tryComputeResolveBlendWeights(resolveDesc, weights, blendReason),
               "pass tryComputeResolveBlendWeights passes after warmup");
    expectNear(weights.current, 0.2f, 1e-5f, "pass tryCompute steady current weight");
    expectNear(weights.history, 0.8f, 1e-5f, "pass tryCompute steady history weight");

    pass->invalidateHistory();
    expectTrue(!pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse fails after invalidate");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::StaleGeneration,
               "pass tryPreflightHistoryReuse reason is StaleGeneration after invalidate");

    fuse::renderer::TaaPassDesc zeroWidthDesc{};
    zeroWidthDesc.width = 0;
    zeroWidthDesc.height = 128;
    auto zeroPass = fuse::renderer::TaaPass::create(zeroWidthDesc);
    expectTrue(zeroPass->classifyJitterNdcReject() == fuse::renderer::TaaJitterGuardRejectReason::InvalidViewport,
               "zero-width pass classifyJitterNdcReject is InvalidViewport");
    expectTrue(!zeroPass->tryPreflightJitterNdc(jitterReason),
               "zero-width pass tryPreflightJitterNdc fails");
    expectTrue(jitterReason == fuse::renderer::TaaJitterGuardRejectReason::InvalidViewport,
               "zero-width pass tryPreflightJitterNdc reject reason is InvalidViewport");
    expectTrue(zeroPass->trySyncJitterToFrameIndexIfReady(0u, jitterReason),
               "zero-width pass trySync still valid for sequence");
    expectTrue(jitterReason == fuse::renderer::TaaJitterGuardRejectReason::None,
               "zero-width pass trySync reject reason is None for valid sequence");

    fuse::renderer::TaaPassDesc invalidSeqDesc{};
    invalidSeqDesc.width = 128;
    invalidSeqDesc.height = 128;
    invalidSeqDesc.jitter.sequence_length = 0u;
    auto invalidSeqPass = fuse::renderer::TaaPass::create(invalidSeqDesc);
    expectTrue(invalidSeqPass->jitter().sequenceLength() == 8u,
               "invalid sequence desc falls back to default length on pass");
    expectTrue(invalidSeqPass->classifyJitterSyncReject() == fuse::renderer::TaaJitterGuardRejectReason::None,
               "fallback sequence pass classifyJitterSyncReject is None");
    expectTrue(!invalidSeqPass->shouldSkipJitterAdvance(),
               "fallback sequence pass should not skip jitter advance");
    expectTrue(invalidSeqPass->tryPreflightJitterAdvance(jitterReason),
               "fallback sequence pass tryPreflightJitterAdvance passes");
    expectTrue(invalidSeqPass->tryAdvanceJitterIfReady(jitterReason),
               "fallback sequence pass tryAdvanceJitterIfReady passes");
    expectTrue(invalidSeqPass->trySyncJitterToFrameIndexIfReady(0u, jitterReason),
               "fallback sequence pass trySyncJitterToFrameIndexIfReady passes");

    pass->destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testTaaPassTryClassifyWrappers() {
    fuse::renderer::TaaPassDesc passDesc{};
    passDesc.width = 128;
    passDesc.height = 128;
    passDesc.params.blend_factor = 0.35f;

    auto pass = fuse::renderer::TaaPass::create(passDesc);

    expectTrue(pass->classifyJitterSyncReject() == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass classifyJitterSyncReject passes before init");
    expectTrue(pass->classifyJitterNdcReject() == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass classifyJitterNdcReject passes before init");
    expectTrue(pass->classifyJitterAdvanceReject() == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass classifyJitterAdvanceReject passes before init");
    expectTrue(!pass->shouldSkipJitterAdvance(), "pass should not skip jitter advance before init");

    fuse::renderer::TaaJitterGuardRejectReason jitterReject =
        fuse::renderer::TaaJitterGuardRejectReason::None;
    expectTrue(pass->tryPreflightJitterSync(3u, jitterReject), "pass tryPreflightJitterSync passes");
    expectTrue(jitterReject == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass tryPreflightJitterSync reject reason is None");
    expectTrue(pass->tryPreflightJitterNdc(jitterReject), "pass tryPreflightJitterNdc passes");
    expectTrue(jitterReject == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass tryPreflightJitterNdc reject reason is None");
    expectTrue(pass->tryPreflightJitterAdvance(jitterReject), "pass tryPreflightJitterAdvance passes");
    expectTrue(jitterReject == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass tryPreflightJitterAdvance reject reason is None");
    expectTrue(pass->preflightJitterAdvance(), "pass preflightJitterAdvance passes before init");

    fuse::renderer::TaaHistoryReuseBlockReason reuseReason =
        fuse::renderer::TaaHistoryReuseBlockReason::None;
    expectTrue(!pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse fails before init");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::NotReady,
               "pass tryPreflightHistoryReuse reason is NotReady before init");
    expectTrue(!pass->tryPreflightHistoryReadyForResolve(reuseReason),
               "pass tryPreflightHistoryReadyForResolve fails before init");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::NotReady,
               "pass tryPreflightHistoryReadyForResolve reason is NotReady before init");

    fuse::renderer::TaaResolveDesc resolveDesc{};
    resolveDesc.width = 128;
    resolveDesc.height = 128;
    resolveDesc.surfaces.current_frame = reinterpret_cast<void*>(0x10);
    resolveDesc.surfaces.output = reinterpret_cast<void*>(0x20);
    resolveDesc.params = passDesc.params;

    expectTrue(pass->classifyResolveSkip(resolveDesc) ==
                   fuse::renderer::TaaResolveSkipReason::HistoryNotReady,
               "pass classifyResolveSkip reports HistoryNotReady before init");
    fuse::renderer::TaaResolveSkipReason skipReason = fuse::renderer::TaaResolveSkipReason::None;
    expectTrue(!pass->tryPreflightResolve(resolveDesc, skipReason),
               "pass tryPreflightResolve fails before init");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::HistoryNotReady,
               "pass tryPreflightResolve skip reason is HistoryNotReady before init");

    fuse::renderer::TaaResolveBlendRejectReason blendReject =
        fuse::renderer::TaaResolveBlendRejectReason::None;
    expectTrue(pass->classifyResolveBlendReject(resolveDesc) ==
                   fuse::renderer::TaaResolveBlendRejectReason::None,
               "pass classifyResolveBlendReject passes before init");
    expectTrue(pass->tryPreflightResolveBlendWeights(resolveDesc, blendReject),
               "pass tryPreflightResolveBlendWeights passes before init");
    fuse::renderer::TaaBlendWeights weights{};
    expectTrue(pass->tryComputeResolveBlendWeights(resolveDesc, weights, blendReject),
               "pass tryComputeResolveBlendWeights passes before init");
    expectNear(weights.current, 1.f, 1e-5f, "pass tryCompute warmup current weight is full");

    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass try/classify wrapper test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);
    expectTrue(pass->init(resources), "TaaPass initialized for try/classify wrapper test");

    expectTrue(pass->tryPreflightHistoryReadyForResolve(reuseReason),
               "pass tryPreflightHistoryReadyForResolve passes after init");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::None,
               "pass tryPreflightHistoryReadyForResolve reason is None after init");
    expectTrue(!pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse fails before warmup");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::NotWarm,
               "pass tryPreflightHistoryReuse reason is NotWarm before warmup");

    expectTrue(pass->tryPreflightResolve(resolveDesc, skipReason),
               "pass tryPreflightResolve passes after init");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::None,
               "pass tryPreflightResolve skip reason is None after init");
    expectTrue(pass->classifyResolveSkip(resolveDesc) == fuse::renderer::TaaResolveSkipReason::None,
               "pass classifyResolveSkip is None after init");

    expectTrue(pass->resolveFrame(resolveDesc), "initial resolve warms pass history");
    expectTrue(pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse passes after warmup");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::None,
               "pass tryPreflightHistoryReuse reason is None after warmup");

    expectTrue(pass->tryComputeResolveBlendWeights(resolveDesc, weights, blendReject),
               "pass tryComputeResolveBlendWeights passes after warmup");
    expectNear(weights.current, 0.35f, 1e-5f, "pass tryCompute steady current weight");
    expectNear(weights.history, 0.65f, 1e-5f, "pass tryCompute steady history weight");
    expectTrue(pass->classifyResolveBlendReject(resolveDesc) ==
                   fuse::renderer::TaaResolveBlendRejectReason::None,
               "pass classifyResolveBlendReject passes after warmup");

    fuse::renderer::TaaPassDesc zeroWidthDesc{};
    zeroWidthDesc.width = 0;
    zeroWidthDesc.height = 128;
    auto zeroPass = fuse::renderer::TaaPass::create(zeroWidthDesc);
    expectTrue(zeroPass->classifyJitterNdcReject() ==
                   fuse::renderer::TaaJitterGuardRejectReason::InvalidViewport,
               "zero-width pass classifyJitterNdcReject is InvalidViewport");
    expectTrue(!zeroPass->tryPreflightJitterNdc(jitterReject),
               "zero-width pass tryPreflightJitterNdc fails");
    expectTrue(jitterReject == fuse::renderer::TaaJitterGuardRejectReason::InvalidViewport,
               "zero-width pass tryPreflightJitterNdc reject reason is InvalidViewport");
    expectTrue(zeroPass->shouldSkipJitterNdc(), "zero-width pass should skip NDC jitter");

    pass->invalidateHistory();
    expectTrue(!pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse fails after invalidate");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::StaleGeneration,
               "pass tryPreflightHistoryReuse reason is StaleGeneration after invalidate");

    pass->destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testTaaPassTryAndClassifyWrappers() {
    fuse::renderer::TaaPassDesc passDesc{};
    passDesc.width = 128;
    passDesc.height = 128;
    passDesc.params.blend_factor = 0.25f;

    auto pass = fuse::renderer::TaaPass::create(passDesc);

    expectTrue(pass->classifyJitterSyncReject() == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass classifyJitterSyncReject passes before init");
    expectTrue(pass->classifyJitterNdcReject() == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass classifyJitterNdcReject passes before init");
    expectTrue(pass->classifyJitterAdvanceReject() == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass classifyJitterAdvanceReject passes before init");

    fuse::renderer::TaaJitterGuardRejectReason jitterReject =
        fuse::renderer::TaaJitterGuardRejectReason::None;
    expectTrue(pass->tryPreflightJitterSync(3u, jitterReject),
               "pass tryPreflightJitterSync passes before init");
    expectTrue(jitterReject == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass tryPreflightJitterSync reject reason is None");
    expectTrue(pass->tryPreflightJitterNdc(jitterReject),
               "pass tryPreflightJitterNdc passes before init");
    expectTrue(pass->tryPreflightJitterAdvance(jitterReject),
               "pass tryPreflightJitterAdvance passes before init");
    expectTrue(!pass->shouldSkipJitterAdvance(), "pass should not skip jitter advance before init");

    fuse::renderer::TaaHistoryReuseBlockReason reuseReason =
        fuse::renderer::TaaHistoryReuseBlockReason::None;
    expectTrue(!pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse fails before init");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::NotReady,
               "pass tryPreflightHistoryReuse reason is NotReady before init");
    expectTrue(!pass->tryPreflightHistoryReadyForResolve(reuseReason),
               "pass tryPreflightHistoryReadyForResolve fails before init");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::NotReady,
               "pass tryPreflightHistoryReadyForResolve reason is NotReady before init");

    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass try/classify wrapper test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);
    expectTrue(pass->init(resources), "TaaPass initialized for try/classify wrapper test");

    expectTrue(pass->tryPreflightHistoryReadyForResolve(reuseReason),
               "pass tryPreflightHistoryReadyForResolve passes after init");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::None,
               "pass tryPreflightHistoryReadyForResolve reason is None after init");

    fuse::renderer::TaaResolveDesc resolveDesc{};
    resolveDesc.width = 128;
    resolveDesc.height = 128;
    resolveDesc.surfaces.current_frame = reinterpret_cast<void*>(0x10);
    resolveDesc.surfaces.output = reinterpret_cast<void*>(0x20);
    resolveDesc.params = passDesc.params;

    expectTrue(pass->classifyResolveSkip(resolveDesc) == fuse::renderer::TaaResolveSkipReason::None,
               "pass classifyResolveSkip passes for valid desc after init");
    expectTrue(pass->classifyResolveBlendReject(resolveDesc) ==
                   fuse::renderer::TaaResolveBlendRejectReason::None,
               "pass classifyResolveBlendReject passes before warmup resolve");

    fuse::renderer::TaaResolveSkipReason skipReason = fuse::renderer::TaaResolveSkipReason::None;
    expectTrue(pass->tryPreflightResolve(resolveDesc, skipReason),
               "pass tryPreflightResolve passes for valid desc after init");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::None,
               "pass tryPreflightResolve skip reason is None");
    expectTrue(pass->preflightResolve(resolveDesc, &skipReason),
               "pass preflightResolve passes for valid desc after init");

    fuse::renderer::TaaResolveBlendRejectReason blendReason =
        fuse::renderer::TaaResolveBlendRejectReason::None;
    expectTrue(pass->tryPreflightResolveBlendWeights(resolveDesc, blendReason),
               "pass tryPreflightResolveBlendWeights passes before warmup resolve");
    expectTrue(blendReason == fuse::renderer::TaaResolveBlendRejectReason::None,
               "pass tryPreflightResolveBlendWeights reject reason is None");

    fuse::renderer::TaaBlendWeights weights{};
    expectTrue(pass->tryComputeResolveBlendWeights(resolveDesc, weights, blendReason),
               "pass tryComputeResolveBlendWeights passes before warmup resolve");
    expectNear(weights.current, 1.f, 1e-5f, "pass tryCompute warmup current weight is full");
    expectNear(weights.history, 0.f, 1e-5f, "pass tryCompute warmup history weight is zero");

    expectTrue(pass->resolveFrame(resolveDesc), "initial resolve warms pass history");

    expectTrue(pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse passes after warmup");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::None,
               "pass tryPreflightHistoryReuse reason is None after warmup");
    expectTrue(pass->classifyHistoryReuseBlock(0u) == fuse::renderer::TaaHistoryReuseBlockReason::None,
               "pass classifyHistoryReuseBlock is None after warmup");

    expectTrue(pass->tryComputeResolveBlendWeights(resolveDesc, weights, blendReason),
               "pass tryComputeResolveBlendWeights passes after warmup");
    expectNear(weights.current, 0.25f, 1e-5f, "pass tryCompute steady current weight");
    expectNear(weights.history, 0.75f, 1e-5f, "pass tryCompute steady history weight");

    pass->invalidateHistory();
    expectTrue(pass->classifyHistoryReuseBlock(0u) ==
                   fuse::renderer::TaaHistoryReuseBlockReason::StaleGeneration,
               "pass classifyHistoryReuseBlock is StaleGeneration after invalidate");
    expectTrue(!pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse fails after invalidate");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::StaleGeneration,
               "pass tryPreflightHistoryReuse reason is StaleGeneration after invalidate");

    fuse::renderer::TaaPassDesc zeroWidthDesc{};
    zeroWidthDesc.width = 0;
    zeroWidthDesc.height = 128;
    auto zeroPass = fuse::renderer::TaaPass::create(zeroWidthDesc);
    expectTrue(zeroPass->classifyJitterNdcReject() ==
                   fuse::renderer::TaaJitterGuardRejectReason::InvalidViewport,
               "zero-width pass classifyJitterNdcReject is InvalidViewport");
    expectTrue(!zeroPass->tryPreflightJitterNdc(jitterReject),
               "zero-width pass tryPreflightJitterNdc fails");
    expectTrue(zeroPass->shouldSkipJitterNdc(), "zero-width pass should skip NDC jitter");
    expectTrue(zeroPass->classifyJitterSyncReject() == fuse::renderer::TaaJitterGuardRejectReason::None,
               "zero-width pass classifyJitterSyncReject still valid for sequence");

    pass->destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testTaaPassTryAndClassifyWrappers() {
    fuse::renderer::TaaPassDesc passDesc{};
    passDesc.width = 128;
    passDesc.height = 128;
    passDesc.params.blend_factor = 0.25f;

    auto pass = fuse::renderer::TaaPass::create(passDesc);
    expectTrue(pass->classifyJitterSyncReject() == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass classifyJitterSyncReject passes before init");
    expectTrue(pass->classifyJitterNdcReject() == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass classifyJitterNdcReject passes before init");
    expectTrue(pass->classifyJitterAdvanceReject() == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass classifyJitterAdvanceReject passes before init");
    expectTrue(pass->classifyHistoryReuseBlock(0u) == fuse::renderer::TaaHistoryReuseBlockReason::NotReady,
               "pass classifyHistoryReuseBlock is NotReady before init");
    expectTrue(pass->preflightJitterAdvance(), "pass preflightJitterAdvance passes before init");
    expectTrue(!pass->shouldSkipJitterAdvance(), "pass should not skip jitter advance before init");

    fuse::renderer::TaaJitterGuardRejectReason jitterReject =
        fuse::renderer::TaaJitterGuardRejectReason::None;
    expectTrue(pass->tryPreflightJitterSync(6u, jitterReject),
               "pass tryPreflightJitterSync passes before init");
    expectTrue(jitterReject == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass tryPreflightJitterSync reject reason is None");
    expectTrue(pass->tryPreflightJitterNdc(jitterReject),
               "pass tryPreflightJitterNdc passes before init");
    expectTrue(pass->tryPreflightJitterAdvance(jitterReject),
               "pass tryPreflightJitterAdvance passes before init");

    fuse::renderer::TaaHistoryReuseBlockReason historyReason =
        fuse::renderer::TaaHistoryReuseBlockReason::None;
    expectTrue(!pass->tryPreflightHistoryReadyForResolve(historyReason),
               "pass tryPreflightHistoryReadyForResolve fails before init");
    expectTrue(historyReason == fuse::renderer::TaaHistoryReuseBlockReason::NotReady,
               "pass tryPreflightHistoryReadyForResolve reason is NotReady before init");
    expectTrue(!pass->tryPreflightHistoryReuse(0u, historyReason),
               "pass tryPreflightHistoryReuse fails before init");
    expectTrue(historyReason == fuse::renderer::TaaHistoryReuseBlockReason::NotReady,
               "pass tryPreflightHistoryReuse reason is NotReady before init");

    fuse::renderer::TaaResolveDesc resolveDesc{};
    resolveDesc.width = 128;
    resolveDesc.height = 128;
    resolveDesc.surfaces.current_frame = reinterpret_cast<void*>(0x10);
    resolveDesc.surfaces.output = reinterpret_cast<void*>(0x20);
    resolveDesc.params = passDesc.params;

    expectTrue(pass->classifyResolveSkip(resolveDesc) ==
                   fuse::renderer::TaaResolveSkipReason::HistoryNotReady,
               "pass classifyResolveSkip is HistoryNotReady before init");
    fuse::renderer::TaaResolveSkipReason skipReason = fuse::renderer::TaaResolveSkipReason::None;
    expectTrue(!pass->tryPreflightResolve(resolveDesc, skipReason),
               "pass tryPreflightResolve fails before init");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::HistoryNotReady,
               "pass tryPreflightResolve skip reason is HistoryNotReady before init");

    fuse::renderer::TaaResolveBlendRejectReason blendReason =
        fuse::renderer::TaaResolveBlendRejectReason::None;
    expectTrue(pass->classifyResolveBlendReject(resolveDesc) ==
                   fuse::renderer::TaaResolveBlendRejectReason::None,
               "pass classifyResolveBlendReject is None before init");
    expectTrue(pass->tryPreflightResolveBlendWeights(resolveDesc, blendReason),
               "pass tryPreflightResolveBlendWeights passes before init");
    fuse::renderer::TaaBlendWeights weights{};
    expectTrue(pass->tryComputeResolveBlendWeights(resolveDesc, weights, blendReason),
               "pass tryComputeResolveBlendWeights passes before init");
    expectNear(weights.current, 1.f, 1e-5f, "pass tryCompute warmup current weight is full");

    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass try/classify wrapper test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);
    expectTrue(pass->init(resources), "TaaPass initialized for try/classify wrapper test");

    expectTrue(pass->tryPreflightHistoryReadyForResolve(historyReason),
               "pass tryPreflightHistoryReadyForResolve passes after init");
    expectTrue(historyReason == fuse::renderer::TaaHistoryReuseBlockReason::None,
               "pass tryPreflightHistoryReadyForResolve reason is None after init");
    expectTrue(!pass->tryPreflightHistoryReuse(0u, historyReason),
               "pass tryPreflightHistoryReuse fails before warmup");
    expectTrue(historyReason == fuse::renderer::TaaHistoryReuseBlockReason::NotWarm,
               "pass tryPreflightHistoryReuse reason is NotWarm before warmup");

    expectTrue(pass->resolveFrame(resolveDesc), "initial resolve warms pass history");
    expectTrue(pass->tryPreflightHistoryReuse(0u, historyReason),
               "pass tryPreflightHistoryReuse passes after warmup");
    expectTrue(historyReason == fuse::renderer::TaaHistoryReuseBlockReason::None,
               "pass tryPreflightHistoryReuse reason is None after warmup");
    expectTrue(pass->tryComputeResolveBlendWeights(resolveDesc, weights, blendReason),
               "pass tryComputeResolveBlendWeights passes after warmup");
    expectNear(weights.current, 0.25f, 1e-5f, "pass tryCompute steady current weight");
    expectNear(weights.history, 0.75f, 1e-5f, "pass tryCompute steady history weight");

    expectTrue(pass->tryPreflightResolve(resolveDesc, skipReason),
               "pass tryPreflightResolve passes after init");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::None,
               "pass tryPreflightResolve skip reason is None after init");
    expectTrue(pass->classifyResolveSkip(resolveDesc) == fuse::renderer::TaaResolveSkipReason::None,
               "pass classifyResolveSkip is None after init");

    pass->invalidateHistory();
    expectTrue(pass->classifyHistoryReuseBlock(0u) ==
                   fuse::renderer::TaaHistoryReuseBlockReason::StaleGeneration,
               "pass classifyHistoryReuseBlock is StaleGeneration after invalidate");
    expectTrue(!pass->tryPreflightHistoryReuse(0u, historyReason),
               "pass tryPreflightHistoryReuse fails after invalidate");
    expectTrue(historyReason == fuse::renderer::TaaHistoryReuseBlockReason::StaleGeneration,
               "pass tryPreflightHistoryReuse reason is StaleGeneration after invalidate");

    fuse::renderer::TaaPassDesc fallbackSeqDesc{};
    fallbackSeqDesc.width = 128;
    fallbackSeqDesc.height = 128;
    fallbackSeqDesc.jitter.sequence_length = 0;
    auto fallbackSeqPass = fuse::renderer::TaaPass::create(fallbackSeqDesc);
    expectTrue(fallbackSeqPass->jitter().sequenceLength() == 8u,
               "invalid pass jitter desc falls back to default sequence length");
    expectTrue(fallbackSeqPass->classifyJitterSyncReject() ==
                   fuse::renderer::TaaJitterGuardRejectReason::None,
               "fallback pass classifyJitterSyncReject passes after normalization");
    expectTrue(fallbackSeqPass->tryPreflightJitterAdvance(jitterReject),
               "fallback pass tryPreflightJitterAdvance passes after normalization");
    expectTrue(!fallbackSeqPass->shouldSkipJitterAdvance(),
               "fallback pass should not skip jitter advance after normalization");

    fuse::renderer::TaaPassDesc zeroWidthDesc{};
    zeroWidthDesc.width = 0;
    zeroWidthDesc.height = 128;
    auto zeroWidthPass = fuse::renderer::TaaPass::create(zeroWidthDesc);
    expectTrue(zeroWidthPass->classifyJitterNdcReject() ==
                   fuse::renderer::TaaJitterGuardRejectReason::InvalidViewport,
               "zero-width pass classifyJitterNdcReject is InvalidViewport");
    expectTrue(!zeroWidthPass->tryPreflightJitterNdc(jitterReject),
               "zero-width pass tryPreflightJitterNdc fails");
    expectTrue(jitterReject == fuse::renderer::TaaJitterGuardRejectReason::InvalidViewport,
               "zero-width pass tryPreflightJitterNdc reason is InvalidViewport");

    pass->destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

} // namespace

int main() {
    fuse::core::initialize();

    testHaltonJitterSequence();
    testJitterSequenceLayout();
    testJitterSequencePeriod();
    testJitterLargeFrameWrap();
    testNdcOffsetForFrameIndex();
    testJitterSyncToFrameIndex();
    testCustomJitterSequenceLength();
    testHaltonComputeMatchesTable();
    testJitterNdcOffset();
    testHistoryBufferPingPong();
    testHistoryValidityFlags();
    testBlendWeightGuards();
    testHistoryReuseGuards();
    testJitterNdcGuards();
    testResolvePreflightGuards();
    testTaaPassCanResolveFrame();
    testClampTaaParams();
    testBlendWeightGuards();
    testJitterSafeNdcOffset();
    testHistoryReusableGuard();
    testResolveHistoryReusedStat();
    testTaaPassBlendAndReuseGuards();
    testRejectionSurfaceGuards();
    testSanitizeAndPreflightResolve();
    testHistoryGenerationGuardPasses();
    testInvalidateHistoryIfStale();
    testTaaPassSanitizeAndGenerationCurrent();
    testRejectionSurfaceHelpers();
    testComputeEffectiveBlendForHistory();
    testHistoryGenerationAndAcceptanceHelpers();
    testPreflightAndCanProceedHelpers();
    testViewportDimensionHelpers();
    testTaaPassResolveFrameNoOpOnSkip();
    testHistoryBlendHelpers();
    testHistoryReadGuards();
    testResolveSurfaceGuards();
    testShouldSkipTaaResolve();
    testTaaResolveSkipCounts();
    testTaaPassEffectiveBlend();
    testHistoryGenerationCurrentHelpers();
    testCanAttemptAndPrepareResolve();
    testTaaPassPrepareAndCanResolve();
    testBlendWeightGuards();
    testJitterSafeNdcOffset();
    testHistoryReusableGuard();
    testResolveHistoryReusedStat();
    testResolveSkipReasonLabels();
    testHistoryBufferDescValid();
    testResolveDimensionMismatchHelper();
    testHistoryGenerationMatchHelpers();
    testJitterViewportDimensions();
    testTaaPassAutoStampResolveFrame();
    testResolveDimensionHelpers();
    testClassifyTaaResolveSkipPriority();
    testHistoryGenerationGuardBypass();
    testStampObservedHistoryGeneration();
    testInvalidDimensionsSkipReason();
    testDimensionMismatchResolve();
    testMissingSurfacesSkipReason();
    testMissingVelocityDepthSkipReason();
    testStaleHistoryGenerationSkipReason();
    testHistoryInvalidateGeneration();
    testEmptyHistoryResolve();
    testValidityResetAfterInvalidate();
    testResolveStub();
    testTaaPassLifecycle();
    testTaaPassInvalidateHistory();
    testTaaPassResize();
    testTaaPassWouldSkipResolve();
    testTaaPassSyncJitterToFrameIndex();
    testTaaPassGraphHook();
    testBlendWeightGuards();
    testHistoryReuseGuards();
    testJitterSafeNdcOffset();
    testJitterAdvanceIfPossible();
    testHistoryWarmupGuards();
    testJitterSyncGuards();
    testHistoryWarmupPreflightGuards();
    testResolveSurfaceAndPreflightHelpers();
    testResolveBlendPreflightGuards();
    testResolveBlendPreflight();
    testJitterSyncPreflightGuards();
    testJitterProduceNdcGuards();
    testResolveSurfaceGuards();
    testResolvePreflightHelpers();
    testResolveWillReuseHistory();
    testTaaPassResolvePreflight();
    testTaaPassJitterSyncGuards();
    testPreflightTaaResolveBlend();
    testResolveHistoryBlendStats();
    testHistoryNeedsWarmupGuard();
    testComputeTaaResolveBlendWeights();
    testBlendWeightsConsistentWithReuse();
    testClassifyTaaHistoryReuseBlock();
    testJitterSyncAndAdvanceGuards();
    testJitterIndexMatchesFrameIndex();
    testJitterSyncIfReadyGuards();
    testPreflightTaaHistoryReuse();
    testPreflightTaaResolveBlend();
    testTaaPassPreflightAndSyncGuards();
    testTaaPassExpectedBlendAndReuseGuards();
    testPreflightTaaHistoryReuse();
    testPreflightTaaResolveBlendWeights();
    testJitterSyncIfReadyGuards();
    testTaaPassPreflightHelpers();
    testHistoryReuseShouldSkipAndReady();
    testJitterGuardRejectReasons();
    testResolveBlendTryAndShouldSkip();
    testTaaPassDeepenGuardWrappers();
    testHistoryReuseRejectReasons();
    testJitterSyncPreflightGuards();
    testResolveBlendPreflightGuards();
    testTaaPassPreflightGuards();
    testTaaPassJitterSyncAndBlendPreflight();
    testJitterSyncGuards();
    testHistoryWarmupPreflightGuards();
    testTaaPassWarmupAndBlendPreflight();
    testHistoryWarmupReuseGuards();
    testTaaPassSyncWarmupAndBlendPreflight();
    testHistoryWarmupPreflight();
    testPreflightTaaHistoryReuseForResolve();
    testJitterSyncPreflight();
    testPreflightTaaResolveFrame();
    testTaaPassWarmupAndResolveFramePreflight();
    testJitterSyncRejectGuards();
    testJitterNdcRejectGuards();
    testResolveTemporalBlendPreflightGuards();
    testTaaPassDeepenPreflightWrappers();
    testSafeNdcOffsetGuards();
    testJitterSyncBlockGuards();
    testBlendFactorRangeGuards();
    testResolveFramePreflightGuards();
    testTaaPassResolveFrameAndJitterGuards();
    testBlockingReasonHelpers();
    testHistoryWarmupSatisfied();
    testPreflightTaaHistoryReuseForDesc();
    testPreflightTaaResolveGuards();
    testJitterSyncPreflightHelpers();
    testJitterAdvanceIfAligned();
    testTaaPassResolveGuardsPreflight();
    testHistoryWarmupPhaseGuards();
    testResolveTemporalBlendPreflight();
    testHistoryWarmupStateGuards();
    testJitterFrameIndexSlotDriftGuards();
    testResolveBlendModeGuards();
    testTaaPassWarmupBlendAndJitterDriftGuards();
    testPreflightTaaResolveHistoryReuse();
    testPreflightTaaResolveWithBlend();
    testTaaPassWarmupAndSyncPreflightHelpers();
    testEffectiveObservedHistoryGeneration();
    testPreflightTaaResolveTemporal();
    testTryComputeTaaResolveBlendWeights();
    testJitterNdcOffsetForFrameIndexIfReady();
    testJitterSyncIfMisalignedGuards();
    testTaaPassWarmupAndTemporalPreflight();
    testWouldSkipAndTryHistoryReuseGuards();
    testWouldRejectAndTryResolveBlendGuards();
    testTaaPassWouldSkipAndTryHelpers();
    testHistoryWarmupRejectGuards();
    testJitterShouldSkipAndAdvanceGuards();
    testResolveHistoryBlendPreflightGuards();
    testTaaPassWarmupAndJitterSkipWrappers();
    testJitterShouldSkipAndReadyGuards();
    testResolveTemporalPreflightGuards();
    testJitterShouldSkipAndTryNdcGuards();
    testResolveShouldSkipAndTryPreflight();
    testTaaPassDeepenShouldSkipWrappers();
    testJitterSyncShouldSkipAndReady();
    testHistoryIsWarmedGuard();
    testTemporalBlendPreflight();
    testTaaPassTemporalAndJitterPreflights();
    testJitterShouldSkipAndTryNdc();
    testTemporalResolveGuardBundle();
    testTaaPassTemporalAndJitterSkipGuards();
    testJitterSyncNdcShouldSkipAndReady();
    testResolveBlendReadyGuard();
    testTaaPassJitterAndBlendReadyWrappers();
    testJitterFramePreflightAndShouldSkip();
    testResolveBlendFramePreflightGuards();
    testTaaFrameGuardPreflight();
    testJitterShouldSkipAndTryNdcPreflights();
    testHistoryWarmupPreflights();
    testResolveShouldSkipAndTryPreflights();
    testTaaPassTryAndShouldSkipPreflights();
    testHistoryWarmupCompleteGuards();
    testTaaPassTemporalAndJitterPreflightWrappers();
    testHistoryWarmupBlockGuards();
    testComputeResolveBlendWeightsIfReady();
    testTaaPassWarmupAndJitterPreflightWrappers();
    testTaaPassReuseAndJitterGuards();
    testHistoryWarmupAndResolveShouldSkip();
    testJitterShouldSkipAndAdvancePreflight();
    testResolveShouldSkipAndTryPreflight();
    testTaaPassTryClassifyWrappers();
    testHistoryWarmupBlockPreflight();
    testJitterAlignmentPreflight();
    testResolveWithBlendPreflight();
    testTaaPassWarmupAndCompositePreflightWrappers();
    testHistoryWarmupPreflightDeepen();
    testJitterTryIfReadyDeepen();
    testTaaPassTryPreflightDeepen();
    testTaaPassTryPreflightAndClassifyGuards();
    testTaaPassTryPreflightWrappers();
    testTaaPassTryPreflightAndClassifyWrappers();
    testTaaPassHistoryWarmupPreflight();
    testTaaPassTryClassifyGuardWrappers();
    testTaaPassTryAndClassifyWrappers();
    testTaaPassShouldSkipGuardWrappers();
    testHistoryWarmupPreflight();
    testHistoryReusePreflight();
    testJitterSyncPreflight();
    testResolveBlendPreflight();
    testTaaPassPreflightGuards();
    testHistoryWarmupGuardHelpers();
    testJitterSyncGuards();
    testResolveBlendPreflightGuards();
    testHistoryWarmupCompositeGuards();
    testJitterSyncBlockGuards();
    testPreflightTaaResolveWithBlend();
    testResolveStatsBlendConsistent();
    testTaaPassCompositeGuards();
    testPreflightTaaHistoryWarmup();
    testJitterAdvanceIfAlignedGuards();
    testPreflightTaaResolveDesc();
    testHistoryWarmupBlockReason();
    testHistoryReuseForResolveGuards();
    testJitterSyncViewportGuards();
    testResolveTemporalAccumulationPreflight();
    testTaaJitterSyncRejectReasonGuards();
    testTaaHistoryWarmupCompleteAndTemporalReuse();
    testPreflightTaaResolveFrame();
    testJitterSyncRejectReasonGuards();
    testHistorySampleAndWarmupGuards();
    testResolveBlendApplyGuards();
    testTaaPassDeepenGuardWrappers();
    testHistoryWarmupCompleteGuard();
    testJitterShouldSkipAndReadyGuards();
    testResolveBlendReadyGuard();
    testTaaPassWarmupAndJitterReadyGuards();
    testHistoryWarmupPreflightGuards();
    testJitterShouldSkipAndTryNdcGuards();
    testTaaPassWarmupAndJitterSkipGuards();
    testJitterSkipAndReadyGuards();
    testTaaPassSkipReadyAndTryPreflights();
    testJitterSyncAndNdcShouldSkip();
    testResolvePreflightShouldSkip();
    testTaaPassWarmupAndJitterShouldSkipWrappers();
    testHistoryWarmupCompleteAndShouldSkipReuse();
    testResolveFramePreflightGuards();
    testTaaPassCombinedPreflightGuards();
    testTaaDeepenDiagnosticGuardOverloads();
    testJitterNdcShouldSkipAndTryGuards();
    testResolveBlendWeightsIfReadyGuards();
    testTaaPassWarmupAndJitterPreflightWrappers();
    testJitterNdcTryAndShouldSkip();
    testHistoryWarmupPreflights();
    testResolveReuseAndBlendPreflights();
    testTaaPassWarmupAndJitterPreflights();
    testHistoryWarmupPreflightFollowUp();
    testJitterShouldSkipAndAdvancePreflights();
    testResolveBlendPreflightFollowUp();
    testTaaPassWarmupAndJitterPreflightFollowUp();
    testHistoryWarmupStateGuards();
    testJitterShouldSkipAndPixelOffsetGuards();
    testResolveTemporalBlendPreflight();
    testTaaPassTemporalAndJitterPreflightWrappers();
    testResolveFrameCompositePreflight();
    testTaaPassWarmupAndCompositeGuards();
    testHistoryWarmupTryAndShouldSkip();
    testResolveTryAndShouldSkip();
    testTaaPassTryAndShouldSkipWrappers();
    testJitterShouldSkipAndTryNdcPreflight();
    testHistoryIsWarmGuard();
    testTemporalResolveGuardPreflights();
    testTaaPassTemporalAndJitterGuardWrappers();
    testTaaPassWarmupAndResolveFrameGuards();
    testHistoryWarmupPhaseClassification();
    testHistoryReuseForResolvePreflight();
    testResolveFrameGuardsPreflight();
    testTaaPassDeepenFrameGuards();
    testJitterSlotSyncAdvanceTryGuards();
    testTaaPassDeepenTryAndFrameGuards();
    testRejectReasonIsBlockingHelpers();
    testJitterAlignmentGuards();
    testTaaPassTryPreflightWrappers();
    testHistoryWarmupCompleteGuards();
    testResolveTemporalPreflightGuards();
    testTaaPassDeepenTemporalAndAlignmentWrappers();
    testHistoryWarmupClassifyAndTryPreflight();
    testJitterTrySyncAdvanceAndNdc();
    testJitterAlignmentPreflightGuards();
    testHistoryWarmupSatisfiedPreflight();
    testTaaPassDeepenAlignmentAndTemporalBlend();
    testResolvePipelineBlendPreflights();
    testJitterSyncAlignmentGuards();
    testResolveBlendPolicyAndCombinedPreflight();
    testJitterAlignmentAndSyncNdcGuards();
    testHistoryWarmupAndTemporalSampleGuards();
    testResolveTemporalBlendGuards();
    testTaaPassDeepenFollowUpGuards();
    testJitterTryIfReadyWithRejectReason();
    testResolveFramePreflight();
    testTaaPassTryPreflightGuards();
    testTaaPassTryPreflightAndClassifyWrappers();
    testTaaPassTryAndClassifyWrappers();
    testTaaPassTryAndClassifyGuards();
    testTaaPassTryAndClassifyPreflights();
    testTaaPassTemporalGuardsAndTryPreflight();
    testTaaPassTryPreflightAndCompositeGuards();
    testTaaPassB59DeepenTryAndCompositeGuards();
    testTaaPassTryPreflightAndFrameGuards();
    testTaaPassTryPreflightGuardWrappers();
    testTaaPassTryAndClassifyGuardWrappers();
    testTaaPassTryClassifyWrappers();

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_taa_pass: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_taa_pass: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
