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

namespace {

int g_failures = 0;

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
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
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
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
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
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
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
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
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
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
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

    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
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

    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
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
    expectTrue(bootstrap != nullptr, "bootstrap allocated for sanitize/preflight test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for generation current test");

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
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
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
    expectTrue(bootstrap != nullptr, "bootstrap allocated for invalidate-if-stale test");

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
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass sanitize test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass prepare test");

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
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
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
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
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
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
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
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
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
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
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
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
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
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
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
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
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
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
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
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
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
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
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
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
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
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
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
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
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

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaHistoryBuffer history;
    fuse::renderer::TaaHistoryBufferDesc historyDesc{64, 64};
    expectTrue(history.init(resources, historyDesc), "history ready for resolve will-reuse test");

    fuse::renderer::TaaResolveDesc desc{};
    desc.width = 64;
    desc.height = 64;
    desc.surfaces.current_frame = reinterpret_cast<void*>(0x1);
    desc.surfaces.output = reinterpret_cast<void*>(0x2);
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
    expectTrue(!fuse::renderer::taaResolveWillReuseHistory(desc, history),
               "full blend will not reuse history");
    expectTrue(resolve.resolve(desc, history), "full blend resolve succeeds");
    expectTrue(!resolve.lastStats().history_reused, "full blend resolve does not reuse history");

    history.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testTaaPassResolvePreflight() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass resolve preflight test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaPassDesc passDesc{};
    passDesc.width = 64;
    passDesc.height = 64;

    auto pass = fuse::renderer::TaaPass::create(passDesc);
    expectTrue(pass->init(resources), "TaaPass initialized for resolve preflight test");

    fuse::renderer::TaaResolveDesc desc{};
    desc.width = 64;
    desc.height = 64;
    desc.surfaces.current_frame = reinterpret_cast<void*>(0x1);
    desc.surfaces.output = reinterpret_cast<void*>(0x2);
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
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testResolveHistoryBlendStats() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
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
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
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
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
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

    history.markResolved();
    const fuse::renderer::TaaBlendWeights steady =
        fuse::renderer::computeTaaResolveBlendWeights(desc, history);
    expectNear(steady.current, 0.3f, 1e-5f, "resolve blend weights use configured current after warmup");
    expectNear(steady.history, 0.7f, 1e-5f, "resolve blend weights use history complement after warmup");
    expectTrue(fuse::renderer::taaResolveAppliesHistoryBlend(desc, history),
               "resolve applies history blend after warmup");

    history.invalidateHistory();
    desc.observed_history_generation = 0u;
    const fuse::renderer::TaaBlendWeights stale =
        fuse::renderer::computeTaaResolveBlendWeights(desc, history);
    expectNear(stale.current, 1.f, 1e-5f, "stale generation forces full current blend");
    expectNear(stale.history, 0.f, 1e-5f, "stale generation zeroes history blend");
    expectTrue(!fuse::renderer::taaResolveAppliesHistoryBlend(desc, history),
               "resolve does not apply history blend when generation is stale");

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

    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
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

    history.markResolved();
    expectTrue(fuse::renderer::classifyTaaHistoryReuseBlock(history, 0u) ==
                   fuse::renderer::TaaHistoryReuseBlockReason::None,
               "warmed history with matching generation is not blocked");

    history.invalidateHistory();
    expectTrue(fuse::renderer::classifyTaaHistoryReuseBlock(history, 0u) ==
                   fuse::renderer::TaaHistoryReuseBlockReason::StaleGeneration,
               "stale generation classified as StaleGeneration");
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
}

void testTaaPassExpectedBlendAndReuseGuards() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
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
                   fuse::renderer::TaaHistoryReuseBlockReason::StaleGeneration,
               "pass classifies stale generation after invalidate");

    pass->destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testPreflightTaaHistoryReuse() {
    fuse::renderer::TaaHistoryBuffer emptyHistory;
    fuse::renderer::TaaHistoryReuseBlockReason reason = fuse::renderer::TaaHistoryReuseBlockReason::None;
    expectTrue(!fuse::renderer::preflightTaaHistoryReuse(emptyHistory, 0u, &reason),
               "empty history fails reuse preflight");
    expectTrue(reason == fuse::renderer::TaaHistoryReuseBlockReason::NotReady,
               "empty history reuse preflight reason is NotReady");

    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for history reuse preflight test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaHistoryBuffer history;
    fuse::renderer::TaaHistoryBufferDesc historyDesc{64, 64};
    expectTrue(history.init(resources, historyDesc), "history ready for reuse preflight test");
    expectTrue(fuse::renderer::taaHistoryReadyForResolve(history), "allocated history ready for resolve");
    expectTrue(fuse::renderer::taaHistoryWarmupFramesRemaining(history) == 1u,
               "unwarmed history has one warmup frame remaining");
    expectTrue(!fuse::renderer::preflightTaaHistoryReuse(history, 0u, &reason),
               "unwarmed history fails reuse preflight");
    expectTrue(reason == fuse::renderer::TaaHistoryReuseBlockReason::NotWarm,
               "unwarmed history reuse preflight reason is NotWarm");

    history.markResolved();
    expectTrue(fuse::renderer::taaHistoryWarmupFramesRemaining(history) == 0u,
               "warmed history has zero warmup frames remaining");
    expectTrue(fuse::renderer::preflightTaaHistoryReuse(history, 0u, &reason),
               "warmed history passes reuse preflight");
    expectTrue(reason == fuse::renderer::TaaHistoryReuseBlockReason::None,
               "warmed history reuse preflight reason is None");

    history.invalidateHistory();
    expectTrue(!fuse::renderer::preflightTaaHistoryReuse(history, 0u, &reason),
               "stale generation fails reuse preflight");
    expectTrue(reason == fuse::renderer::TaaHistoryReuseBlockReason::StaleGeneration,
               "stale generation reuse preflight reason is StaleGeneration");

    history.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testPreflightTaaResolveBlendWeights() {
    expectTrue(std::strcmp(fuse::renderer::taaResolveBlendRejectReasonLabel(
                               fuse::renderer::TaaResolveBlendRejectReason::None),
                           "none") == 0,
               "None blend reject label");
    expectTrue(std::strcmp(fuse::renderer::taaResolveBlendRejectReasonLabel(
                               fuse::renderer::TaaResolveBlendRejectReason::InvalidWeights),
                           "invalid_weights") == 0,
               "InvalidWeights blend reject label");
    expectTrue(std::strcmp(fuse::renderer::taaResolveBlendRejectReasonLabel(
                               fuse::renderer::TaaResolveBlendRejectReason::InconsistentWithReuse),
                           "inconsistent_with_reuse") == 0,
               "InconsistentWithReuse blend reject label");

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

    fuse::renderer::TaaResolveDesc desc{};
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

    history.markResolved();
    expectTrue(fuse::renderer::preflightTaaResolveBlendWeights(desc, history, &rejectReason),
               "steady blend weights pass preflight after warmup");

    history.invalidateHistory();
    desc.observed_history_generation = 0u;
    expectTrue(fuse::renderer::preflightTaaResolveBlendWeights(desc, history, &rejectReason),
               "stale generation forces full-current blend that still passes preflight");
    expectTrue(rejectReason == fuse::renderer::TaaResolveBlendRejectReason::None,
               "stale generation blend preflight reject reason is None");

    const fuse::renderer::TaaBlendWeights invalid{0.6f, 0.6f};
    expectTrue(!fuse::renderer::taaBlendWeightsConsistentWithReuse(invalid, true),
               "invalid weights fail reuse consistency");
    expectTrue(!fuse::renderer::taaBlendWeightsValid(invalid), "invalid weights fail validity check");

    history.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testJitterSyncIfReadyGuards() {
    using fuse::renderer::TaaJitterLayout;

    expectTrue(TaaJitterLayout::jitterSlotMatchesFrameIndex(5u, 5u, 8u),
               "slot five matches frame index five");
    expectTrue(TaaJitterLayout::jitterSlotMatchesFrameIndex(13u, 5u, 8u),
               "wrapped frame index maps to matching slot");
    expectTrue(!TaaJitterLayout::jitterSlotMatchesFrameIndex(5u, 6u, 8u),
               "mismatched slot fails frame-index alignment check");
    expectTrue(!TaaJitterLayout::jitterSlotMatchesFrameIndex(0u, 0u, 0u),
               "invalid sequence fails slot alignment check");

    fuse::renderer::TaaJitter jitter;
    expectTrue(!jitter.isAlignedToFrameIndex(5u), "default jitter is not aligned to frame five");
    expectTrue(jitter.syncToFrameIndexIfReady(5u), "syncToFrameIndexIfReady succeeds for valid sequence");
    expectTrue(jitter.isAlignedToFrameIndex(5u), "jitter aligned after syncToFrameIndexIfReady");
    expectTrue(jitter.monotonicFrameIndex() == 5u, "syncToFrameIndexIfReady sets monotonic counter");

    fuse::renderer::TaaJitterDesc invalidDesc{};
    invalidDesc.sequence_length = 0u;
    fuse::renderer::TaaJitter fallbackJitter(invalidDesc);
    expectTrue(fallbackJitter.sequenceLength() == 8u, "invalid desc falls back to default sequence length");
    expectTrue(fallbackJitter.syncToFrameIndexIfReady(3u), "syncToFrameIndexIfReady succeeds after fallback");
    expectTrue(fallbackJitter.isAlignedToFrameIndex(3u), "fallback jitter aligned after sync");

    fuse::renderer::TaaPassDesc passDesc{};
    passDesc.width = 128;
    passDesc.height = 128;
    auto pass = fuse::renderer::TaaPass::create(passDesc);
    expectTrue(!pass->jitterAlignedToFrameIndex(7u), "pass jitter not aligned before sync");
    expectTrue(pass->syncJitterToFrameIndexIfReady(7u), "pass syncJitterToFrameIndexIfReady succeeds");
    expectTrue(pass->jitterAlignedToFrameIndex(7u), "pass jitter aligned after syncIfReady");
    expectTrue(pass->jitter().monotonicFrameIndex() == 7u, "pass syncIfReady sets monotonic counter");
}

void testTaaPassPreflightHelpers() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass preflight helper test");

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
        fuse::renderer::TaaResolveBlendRejectReason::None;
    expectTrue(pass->preflightResolveBlendWeights(resolveDesc, &blendReason),
               "pass blend preflight passes before first resolve");
    expectTrue(pass->resolveFrame(resolveDesc), "initial resolve warms pass history");

    expectTrue(pass->preflightHistoryReuse(0u, &reuseReason),
               "pass reuse preflight passes after warmup");
    expectTrue(pass->preflightResolveBlendWeights(resolveDesc, &blendReason),
               "pass blend preflight passes after warmup");

    pass->invalidateHistory();
    expectTrue(!pass->preflightHistoryReuse(0u, &reuseReason),
               "pass reuse preflight fails after invalidate");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::StaleGeneration,
               "pass reuse preflight reason is StaleGeneration after invalidate");

    pass->destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testHistoryReuseShouldSkipAndReady() {
    fuse::renderer::TaaHistoryBuffer emptyHistory;
    expectTrue(fuse::renderer::shouldSkipTaaHistoryReuse(emptyHistory, 0u),
               "empty history should skip reuse");
    expectTrue(!fuse::renderer::taaHistoryReuseReady(emptyHistory, 0u),
               "empty history is not reuse-ready");

    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for should-skip reuse test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaHistoryBuffer history;
    fuse::renderer::TaaHistoryBufferDesc historyDesc{64, 64};
    expectTrue(history.init(resources, historyDesc), "history ready for should-skip reuse test");
    expectTrue(history.warmupFramesRemaining() == 1u, "history buffer warmup frames remaining is one");
    expectTrue(fuse::renderer::shouldSkipTaaHistoryReuse(history, 0u),
               "unwarmed history should skip reuse");
    expectTrue(!history.reuseReady(0u), "unwarmed history buffer is not reuse-ready");

    fuse::renderer::TaaHistoryReuseBlockReason reason = fuse::renderer::TaaHistoryReuseBlockReason::None;
    expectTrue(!fuse::renderer::tryPreflightTaaHistoryReuse(history, 0u, reason),
               "tryPreflight fails for unwarmed history");
    expectTrue(reason == fuse::renderer::TaaHistoryReuseBlockReason::NotWarm,
               "tryPreflight reason is NotWarm for unwarmed history");

    history.markResolved();
    expectTrue(history.warmupFramesRemaining() == 0u, "warmed history buffer has zero warmup frames");
    expectTrue(!fuse::renderer::shouldSkipTaaHistoryReuse(history, 0u),
               "warmed history should not skip reuse");
    expectTrue(fuse::renderer::taaHistoryReuseReady(history, 0u), "warmed history is reuse-ready");
    expectTrue(history.reuseReady(0u), "history buffer reuseReady after warmup");
    expectTrue(fuse::renderer::tryPreflightTaaHistoryReuse(history, 0u, reason),
               "tryPreflight passes for warmed history");
    expectTrue(reason == fuse::renderer::TaaHistoryReuseBlockReason::None,
               "tryPreflight reason is None for warmed history");

    history.invalidateHistory();
    expectTrue(fuse::renderer::shouldSkipTaaHistoryReuse(history, 0u),
               "stale generation should skip reuse");
    expectTrue(!fuse::renderer::taaHistoryReuseReady(history, 0u),
               "stale generation is not reuse-ready");

    history.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testJitterGuardRejectReasons() {
    using fuse::renderer::TaaJitterLayout;

    expectTrue(std::strcmp(fuse::renderer::taaJitterGuardRejectReasonLabel(
                               fuse::renderer::TaaJitterGuardRejectReason::None),
                           "none") == 0,
               "None jitter guard reject label");
    expectTrue(std::strcmp(fuse::renderer::taaJitterGuardRejectReasonLabel(
                               fuse::renderer::TaaJitterGuardRejectReason::InvalidSequence),
                           "invalid_sequence") == 0,
               "InvalidSequence jitter guard reject label");
    expectTrue(std::strcmp(fuse::renderer::taaJitterGuardRejectReasonLabel(
                               fuse::renderer::TaaJitterGuardRejectReason::InvalidViewport),
                           "invalid_viewport") == 0,
               "InvalidViewport jitter guard reject label");

    expectTrue(fuse::renderer::classifyTaaJitterSyncReject(8u) ==
                   fuse::renderer::TaaJitterGuardRejectReason::None,
               "valid sequence passes sync classify");
    expectTrue(fuse::renderer::classifyTaaJitterSyncReject(0u) ==
                   fuse::renderer::TaaJitterGuardRejectReason::InvalidSequence,
               "zero sequence fails sync classify");

    expectTrue(fuse::renderer::classifyTaaJitterNdcReject(1920u, 1080u, 8u) ==
                   fuse::renderer::TaaJitterGuardRejectReason::None,
               "valid viewport passes NDC classify");
    expectTrue(fuse::renderer::classifyTaaJitterNdcReject(0u, 1080u, 8u) ==
                   fuse::renderer::TaaJitterGuardRejectReason::InvalidViewport,
               "zero width fails NDC classify");
    expectTrue(fuse::renderer::classifyTaaJitterNdcReject(1920u, 1080u, 0u) ==
                   fuse::renderer::TaaJitterGuardRejectReason::InvalidSequence,
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

    fuse::renderer::TaaJitter jitter;
    fuse::math::Vec2 ndcOut{};
    expectTrue(jitter.currentNdcOffsetIfReady(128u, 128u, ndcOut),
               "currentNdcOffsetIfReady succeeds for valid viewport");
    const fuse::math::Vec2 directNdc = jitter.currentNdcOffset(128u, 128u);
    expectNear(ndcOut.x, directNdc.x, 1e-6f, "currentNdcOffsetIfReady matches currentNdcOffset X");
    expectNear(ndcOut.y, directNdc.y, 1e-6f, "currentNdcOffsetIfReady matches currentNdcOffset Y");
    expectTrue(!jitter.currentNdcOffsetIfReady(0u, 128u, ndcOut),
               "currentNdcOffsetIfReady fails for zero width");
}

void testResolveBlendTryAndShouldSkip() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for blend try/should-skip test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaHistoryBuffer history;
    fuse::renderer::TaaHistoryBufferDesc historyDesc{64, 64};
    expectTrue(history.init(resources, historyDesc), "history ready for blend try/should-skip test");

    fuse::renderer::TaaResolveDesc desc{};
    desc.params.blend_factor = 0.35f;

    expectTrue(!fuse::renderer::shouldSkipTaaResolveBlend(desc, history),
               "warmup blend should not be skipped");
    expectTrue(fuse::renderer::preflightTaaResolveBlendWeights(desc, history),
               "warmup blend preflight passes");

    fuse::renderer::TaaResolveBlendRejectReason rejectReason =
        fuse::renderer::TaaResolveBlendRejectReason::None;
    expectTrue(fuse::renderer::tryPreflightTaaResolveBlendWeights(desc, history, rejectReason),
               "tryPreflightTaaResolveBlendWeights passes for warmup");
    expectTrue(rejectReason == fuse::renderer::TaaResolveBlendRejectReason::None,
               "tryPreflight blend reject reason is None for warmup");

    fuse::renderer::TaaBlendWeights weights{};
    expectTrue(fuse::renderer::tryComputeTaaResolveBlendWeights(desc, history, weights, rejectReason),
               "tryComputeTaaResolveBlendWeights passes for warmup");
    expectNear(weights.current, 1.f, 1e-5f, "tryCompute warmup current weight is full");
    expectNear(weights.history, 0.f, 1e-5f, "tryCompute warmup history weight is zero");

    history.markResolved();
    expectTrue(fuse::renderer::tryComputeTaaResolveBlendWeights(desc, history, weights, rejectReason),
               "tryComputeTaaResolveBlendWeights passes after warmup");
    expectNear(weights.current, 0.35f, 1e-5f, "tryCompute steady current weight");
    expectNear(weights.history, 0.65f, 1e-5f, "tryCompute steady history weight");
    expectTrue(!fuse::renderer::shouldSkipTaaResolveBlend(desc, history),
               "steady blend should not be skipped");

    history.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testTaaPassDeepenGuardWrappers() {
    fuse::renderer::TaaPassDesc passDesc{};
    passDesc.width = 128;
    passDesc.height = 128;
    passDesc.params.blend_factor = 0.2f;

    auto pass = fuse::renderer::TaaPass::create(passDesc);
    expectTrue(pass->warmupFramesRemaining() == 1u, "pass warmup frames remaining before init");
    expectTrue(pass->shouldSkipHistoryReuse(0u), "pass should skip reuse before init");
    expectTrue(!pass->historyReuseReady(0u), "pass history not reuse-ready before init");

    fuse::math::Vec2 ndcOut{};
    expectTrue(pass->currentJitterNdcIfReady(ndcOut), "pass currentJitterNdcIfReady before init");
    const fuse::math::Vec2 directNdc = pass->currentJitterNdc();
    expectNear(ndcOut.x, directNdc.x, 1e-6f, "pass currentJitterNdcIfReady matches currentJitterNdc X");

    fuse::renderer::TaaJitterGuardRejectReason jitterReject =
        fuse::renderer::TaaJitterGuardRejectReason::None;
    expectTrue(pass->preflightJitterSync(4u, &jitterReject), "pass preflightJitterSync passes");
    expectTrue(jitterReject == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass preflightJitterSync reject reason is None");

    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass deepen wrapper test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);
    expectTrue(pass->init(resources), "TaaPass initialized for deepen wrapper test");
    expectTrue(pass->warmupFramesRemaining() == 1u, "pass warmup frames remaining after init");

    fuse::renderer::TaaResolveDesc resolveDesc{};
    resolveDesc.width = 128;
    resolveDesc.height = 128;
    resolveDesc.surfaces.current_frame = reinterpret_cast<void*>(0x10);
    resolveDesc.surfaces.output = reinterpret_cast<void*>(0x20);
    resolveDesc.params = passDesc.params;
    expectTrue(!pass->shouldSkipResolveBlend(resolveDesc), "pass should not skip blend before warmup resolve");
    expectTrue(pass->resolveFrame(resolveDesc), "initial resolve warms pass history");

    expectTrue(pass->warmupFramesRemaining() == 0u, "pass warmup frames remaining after resolve");
    expectTrue(!pass->shouldSkipHistoryReuse(0u), "pass should not skip reuse after warmup");
    expectTrue(pass->historyReuseReady(0u), "pass history reuse-ready after warmup");
    expectTrue(!pass->shouldSkipResolveBlend(resolveDesc), "pass should not skip blend after warmup");

    pass->invalidateHistory();
    expectTrue(pass->shouldSkipHistoryReuse(0u), "pass should skip reuse after invalidate");
    expectTrue(!pass->historyReuseReady(0u), "pass history not reuse-ready after invalidate");

    fuse::renderer::TaaPassDesc zeroWidthDesc{};
    zeroWidthDesc.width = 0;
    zeroWidthDesc.height = 128;
    auto zeroPass = fuse::renderer::TaaPass::create(zeroWidthDesc);
    expectTrue(!zeroPass->currentJitterNdcIfReady(ndcOut), "zero-width pass blocks currentJitterNdcIfReady");
    expectTrue(zeroPass->preflightJitterSync(0u), "zero-width pass jitter sync still valid for sequence");

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

    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
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
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for warmup/resolve should-skip test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for history warmup preflight test");

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

    history.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testJitterShouldSkipAndAdvancePreflight() {
    using fuse::renderer::TaaJitterLayout;

    expectTrue(!fuse::renderer::shouldSkipTaaJitterSync(5u, 8u),
               "valid sequence should not skip jitter sync");
    expectTrue(fuse::renderer::shouldSkipTaaJitterSync(5u, 0u),
               "invalid sequence should skip jitter sync");
    expectTrue(!fuse::renderer::shouldSkipTaaJitterNdc(128u, 128u, 8u),
               "valid viewport should not skip NDC jitter");
    expectTrue(fuse::renderer::shouldSkipTaaJitterNdc(0u, 128u, 8u),
               "zero width should skip NDC jitter");

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
}

void testResolveShouldSkipAndTryPreflight() {
    fuse::renderer::TaaHistoryBuffer emptyHistory;
    fuse::renderer::TaaResolveDesc desc{};
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
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for resolve should-skip test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for history reuse preflight test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaHistoryBuffer history;
    fuse::renderer::TaaHistoryBufferDesc historyDesc{64, 64};
    expectTrue(history.init(resources, historyDesc), "history ready for resolve should-skip test");
    expectTrue(!fuse::renderer::shouldSkipTaaResolve(desc, history),
               "resolve should not skip with valid desc and history");
    expectTrue(fuse::renderer::tryPreflightTaaResolve(desc, history, skipReason),
               "tryPreflightTaaResolve passes with valid desc and history");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::None,
               "tryPreflightTaaResolve skip reason is None");

    desc.width = 0u;
    expectTrue(fuse::renderer::shouldSkipTaaResolve(desc, history),
               "resolve should skip with invalid dimensions");
    expectTrue(!fuse::renderer::tryPreflightTaaResolve(desc, history, skipReason),
               "tryPreflightTaaResolve fails with invalid dimensions");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::InvalidDimensions,
               "tryPreflightTaaResolve skip reason is InvalidDimensions");
    expectTrue(history.init(resources, historyDesc), "history ready for reuse preflight test");
    history.markResolved();

    const fuse::renderer::TaaHistoryReusePreflight current =
        fuse::renderer::preflightTaaHistoryReuse(history, 0u);
    expectTrue(current.generation_matches, "current generation matches after init");
    expectTrue(current.reuse_allowed, "reuse allowed with current generation");
    expectTrue(current.canReuseHistory(), "reuse preflight canReuseHistory mirrors reuse_allowed");

    history.invalidateHistory();
    const fuse::renderer::TaaHistoryReusePreflight stale =
    expectTrue(!stale.generation_matches, "stale observed generation fails match");
    expectTrue(!stale.reuse_allowed, "stale generation blocks reuse");

    fuse::renderer::TaaResolveDesc desc{};
    desc.observed_history_generation = fuse::renderer::kTaaResolveNoHistoryGeneration;
    const fuse::renderer::TaaHistoryReusePreflight bypass =
        fuse::renderer::preflightTaaHistoryReuseForDesc(history, desc);
    expectTrue(bypass.reuse_allowed, "sentinel bypass allows reuse when history is warm");

    history.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testTaaPassTryClassifyWrappers() {
    fuse::renderer::TaaPassDesc passDesc{};
    passDesc.width = 128;
    passDesc.height = 128;
    passDesc.params.blend_factor = 0.3f;

    auto pass = fuse::renderer::TaaPass::create(passDesc);

    expectTrue(pass->classifyJitterSyncReject() == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass classifyJitterSyncReject passes before init");

    fuse::renderer::TaaJitterGuardRejectReason jitterReject =
        fuse::renderer::TaaJitterGuardRejectReason::None;
    expectTrue(pass->tryPreflightJitterSync(5u, jitterReject),
               "pass tryPreflightJitterSync passes before init");
    expectTrue(jitterReject == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass tryPreflightJitterSync reject reason is None before init");

    fuse::renderer::TaaHistoryReuseBlockReason reuseReason = fuse::renderer::TaaHistoryReuseBlockReason::None;
    expectTrue(!pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse fails before init");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::NotReady,
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
    fuse::renderer::TaaJitter jitter;
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
}

void testResolveBlendPreflight() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass try/classify wrapper test");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for resolve blend preflight test");

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
    expectTrue(pass->classifyHistoryReuseBlock(0u) == fuse::renderer::TaaHistoryReuseBlockReason::NotWarm,
               "pass classifyHistoryReuseBlock matches tryPreflight before warmup");

    fuse::renderer::TaaHistoryBuffer history;
    fuse::renderer::TaaHistoryBufferDesc historyDesc{64, 64};
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

    history.markResolved();
    const fuse::renderer::TaaResolveBlendPreflight steady =
        fuse::renderer::preflightTaaResolveBlend(false, params, history);
    expectTrue(!steady.first_frame, "steady blend preflight is not first frame");
    expectTrue(steady.history_blend_allowed, "steady blend preflight allows history blend");
    expectTrue(steady.history_reuse_allowed, "steady blend preflight allows history reuse");
    expectNear(steady.weights.current, 0.3f, 1e-5f, "steady blend preflight uses configured current weight");

    fuse::renderer::TaaResolveDesc desc{};
    desc.params = params;
    desc.observed_history_generation = 0u;
    history.invalidateHistory();
    const fuse::renderer::TaaResolveBlendPreflight descPreflight =
        fuse::renderer::preflightTaaResolveBlendForDesc(desc, history);
    expectTrue(!descPreflight.history_reuse_allowed, "desc blend preflight blocks stale generation reuse");

    desc.observed_history_generation = history.invalidateGeneration();
    const fuse::renderer::TaaResolveBlendPreflight currentDescPreflight =
    expectTrue(currentDescPreflight.history_reuse_allowed,
               "desc blend preflight allows reuse with current generation");

    history.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testTaaPassPreflightGuards() {
    fuse::renderer::TaaPassDesc passDesc{};
    passDesc.width = 128;
    passDesc.height = 128;

    auto pass = fuse::renderer::TaaPass::create(passDesc);
    pass->syncJitterToFrameIndex(4u);
    expectTrue(pass->isJitterSyncedToFrameIndex(4u), "pass reports synced jitter after sync");

    const fuse::renderer::TaaJitterSyncPreflight jitterPreflight = pass->preflightJitterSync(4u);
    expectTrue(jitterPreflight.synced(), "pass jitter preflight reports synced state");

    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass preflight guard test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);
    expectTrue(pass->init(resources), "TaaPass initialized for preflight guard test");

    const fuse::renderer::TaaHistoryWarmupPreflight warmupPreflight = pass->preflightHistoryWarmup();
    expectTrue(warmupPreflight.readyForResolve(), "pass warmup preflight ready for resolve");
    expectTrue(warmupPreflight.needs_warmup, "pass warmup preflight needs warmup before resolve");

    fuse::renderer::TaaResolveDesc resolveDesc{};
    resolveDesc.width = 128;
    resolveDesc.height = 128;
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

    pass->destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testTaaPassShouldSkipGuardWrappers() {
    fuse::renderer::TaaPassDesc passDesc{};
    passDesc.width = 128;
    passDesc.height = 128;

    auto pass = fuse::renderer::TaaPass::create(passDesc);
    expectTrue(pass->shouldSkipHistoryWarmup(), "pass should skip warmup before init");
    expectTrue(pass->shouldSkipHistoryResolve(), "pass should skip resolve before init");
    expectTrue(!pass->historyReadyForResolve(), "pass history not ready for resolve before init");
    expectTrue(!pass->shouldSkipJitterSync(4u), "pass should not skip jitter sync before init");
    expectTrue(!pass->shouldSkipJitterNdc(), "pass should not skip NDC jitter before init");
    expectTrue(pass->preflightJitterNdc(), "pass preflightJitterNdc passes before init");

    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass should-skip wrapper test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);
    expectTrue(pass->init(resources), "TaaPass initialized for should-skip wrapper test");
    expectTrue(pass->historyReadyForResolve(), "pass history ready for resolve after init");
    expectTrue(!pass->shouldSkipHistoryResolve(), "pass should not skip resolve after init");
    expectTrue(pass->shouldSkipHistoryWarmup(), "pass should skip warmup before first resolve");

    fuse::renderer::TaaResolveDesc resolveDesc{};
    resolveDesc.width = 128;
    resolveDesc.height = 128;
    resolveDesc.surfaces.current_frame = reinterpret_cast<void*>(0x10);
    resolveDesc.surfaces.output = reinterpret_cast<void*>(0x20);
    expectTrue(!pass->shouldSkipResolve(resolveDesc), "pass should not skip valid resolve");
    expectTrue(!pass->shouldSkipHistoryWarmup(), "pass should not skip warmup after resolve");

    fuse::renderer::TaaPassDesc zeroWidthDesc{};
    zeroWidthDesc.width = 0;
    zeroWidthDesc.height = 128;
    auto zeroPass = fuse::renderer::TaaPass::create(zeroWidthDesc);
    expectTrue(zeroPass->shouldSkipJitterNdc(), "zero-width pass should skip NDC jitter");
    expectTrue(!zeroPass->preflightJitterNdc(), "zero-width pass preflightJitterNdc fails");


void testTaaPassTryClassifyWrappers() {
    passDesc.params.blend_factor = 0.25f;

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

    fuse::renderer::TaaHistoryReuseBlockReason reuseReason = fuse::renderer::TaaHistoryReuseBlockReason::None;
    expectTrue(!pass->tryPreflightHistoryReadyForResolve(reuseReason),
               "pass tryPreflightHistoryReadyForResolve fails before init");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::NotReady,
               "pass tryPreflightHistoryReadyForResolve reason is NotReady before init");
               "pass tryPreflightHistoryReuse fails before init");
               "pass tryPreflightHistoryReuse reason is NotReady before init");


    expectTrue(pass->classifyResolveSkip(resolveDesc) == fuse::renderer::TaaResolveSkipReason::HistoryNotReady,
               "pass classifyResolveSkip is HistoryNotReady before init");

               "pass tryPreflightResolve fails before init");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::HistoryNotReady,
               "pass tryPreflightResolve skip reason is HistoryNotReady before init");

    fuse::renderer::TaaResolveBlendRejectReason blendReason =
    expectTrue(pass->classifyResolveBlendReject(resolveDesc) ==
                   fuse::renderer::TaaResolveBlendRejectReason::None,
               "pass classifyResolveBlendReject is None before init");
    expectTrue(pass->tryPreflightResolveBlendWeights(resolveDesc, blendReason),
               "pass tryPreflightResolveBlendWeights passes before init");
    expectTrue(pass->tryComputeResolveBlendWeights(resolveDesc, weights, blendReason),
               "pass tryComputeResolveBlendWeights passes before init");

    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass try/classify wrapper test");


    expectTrue(pass->init(resources), "TaaPass initialized for try/classify wrapper test");

    expectTrue(pass->tryPreflightHistoryReadyForResolve(reuseReason),
               "pass tryPreflightHistoryReadyForResolve passes after init");
               "pass tryPreflightHistoryReadyForResolve reason is None after init");
               "pass tryPreflightHistoryReuse fails before warmup");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::NotWarm,
               "pass tryPreflightHistoryReuse reason is NotWarm before warmup");

               "pass classifyResolveSkip is None after init with valid desc");
               "pass tryPreflightResolve passes after init");


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
    testJitterProduceNdcGuards();
    testResolveSurfaceGuards();
    testResolvePreflightHelpers();
    testResolveWillReuseHistory();
    testTaaPassResolvePreflight();
    testResolveHistoryBlendStats();
    testHistoryNeedsWarmupGuard();
    testComputeTaaResolveBlendWeights();
    testBlendWeightsConsistentWithReuse();
    testClassifyTaaHistoryReuseBlock();
    testJitterSyncAndAdvanceGuards();
    testTaaPassExpectedBlendAndReuseGuards();
    testPreflightTaaHistoryReuse();
    testPreflightTaaResolveBlendWeights();
    testJitterSyncIfReadyGuards();
    testTaaPassPreflightHelpers();
    testHistoryReuseShouldSkipAndReady();
    testJitterGuardRejectReasons();
    testResolveBlendTryAndShouldSkip();
    testTaaPassDeepenGuardWrappers();
    testTaaPassReuseAndJitterGuards();
    testHistoryWarmupAndResolveShouldSkip();
    testJitterShouldSkipAndAdvancePreflight();
    testResolveShouldSkipAndTryPreflight();
    testTaaPassTryClassifyWrappers();
    testTaaPassShouldSkipGuardWrappers();
    testHistoryWarmupPreflight();
    testHistoryReusePreflight();
    testJitterSyncPreflight();
    testResolveBlendPreflight();
    testTaaPassPreflightGuards();

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_taa_pass: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_taa_pass: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
