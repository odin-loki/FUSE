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

    expectNear(fuse::renderer::computeEffectiveBlend(true, raw), 1.f, 1e-5f,
               "first frame effective blend is full current weight");
    expectNear(fuse::renderer::computeEffectiveBlend(false, raw), 1.f, 1e-5f,
               "subsequent effective blend uses clamped blend_factor");
    expectNear(fuse::renderer::computeHistoryBlend(0.15f), 0.85f, 1e-5f,
               "history blend is complement of effective blend");
    expectNear(fuse::renderer::computeHistoryBlend(1.f), 0.f, 1e-5f,
               "full current weight yields zero history blend");
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
    expectTrue(!fuse::renderer::taaResolveRejectionSurfacesSatisfied(desc),
               "velocity alone does not satisfy depth rejection");
    desc.surfaces.depth_buffer = reinterpret_cast<void*>(0x2);
    expectTrue(fuse::renderer::taaResolveRejectionSurfacesSatisfied(desc),
               "velocity and depth satisfy rejection guards");

    desc.params.velocity_rejection = 0.f;
    desc.surfaces.velocity_buffer = nullptr;
    expectTrue(fuse::renderer::taaResolveRejectionSurfacesSatisfied(desc),
               "depth-only rejection satisfied with depth buffer");
}

void testSanitizeAndPreflightResolve() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for sanitize/preflight test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaHistoryBuffer history;
    fuse::renderer::TaaHistoryBufferDesc historyDesc{64, 64};
    expectTrue(history.init(resources, historyDesc), "history ready for sanitize/preflight test");

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
    desc.observed_history_generation = fuse::renderer::kTaaResolveNoHistoryGeneration;
    expectTrue(fuse::renderer::taaResolveHistoryGenerationGuardPasses(desc, history),
               "sentinel bypasses generation guard");

    desc.observed_history_generation = 0u;
    expectTrue(fuse::renderer::taaResolveHistoryGenerationGuardPasses(desc, history),
               "current generation passes guard");

    history.invalidateHistory();
    expectTrue(!fuse::renderer::taaResolveHistoryGenerationGuardPasses(desc, history),
               "stale generation fails guard");

    history.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testInvalidateHistoryIfStale() {
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

    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for warmup/resolve should-skip test");

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

    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for resolve should-skip test");

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
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::NotReady,
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
    expectTrue(pass->classifyHistoryReuseBlock(0u) == fuse::renderer::TaaHistoryReuseBlockReason::NotWarm,
               "pass classifyHistoryReuseBlock matches tryPreflight before warmup");

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
    expectTrue(pass->tryPreflightResolveBlendWeights(resolveDesc, blendReject),
               "pass tryPreflightResolveBlendWeights passes after warmup");
    expectTrue(pass->tryComputeResolveBlendWeights(resolveDesc, weights, blendReject),
               "pass tryComputeResolveBlendWeights passes after warmup");
    expectNear(weights.current, 0.3f, 1e-5f, "pass tryCompute steady current weight");
    expectNear(weights.history, 0.7f, 1e-5f, "pass tryCompute steady history weight");
    expectTrue(pass->tryPreflightResolve(resolveDesc, skipReason),
               "pass tryPreflightResolve passes after warmup");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::None,
               "pass tryPreflightResolve skip reason is None after warmup");
    expectTrue(pass->classifyResolveSkip(resolveDesc) == fuse::renderer::TaaResolveSkipReason::None,
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
    expectTrue(pass->resolveFrame(resolveDesc), "initial resolve warms pass history");
    expectTrue(!pass->shouldSkipHistoryWarmup(), "pass should not skip warmup after resolve");

    fuse::renderer::TaaPassDesc zeroWidthDesc{};
    zeroWidthDesc.width = 0;
    zeroWidthDesc.height = 128;
    auto zeroPass = fuse::renderer::TaaPass::create(zeroWidthDesc);
    expectTrue(zeroPass->shouldSkipJitterNdc(), "zero-width pass should skip NDC jitter");
    expectTrue(!zeroPass->preflightJitterNdc(), "zero-width pass preflightJitterNdc fails");

    pass->destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testTaaPassTryClassifyWrappers() {
    fuse::renderer::TaaPassDesc passDesc{};
    passDesc.width = 128;
    passDesc.height = 128;
    passDesc.params.blend_factor = 0.25f;

    auto pass = fuse::renderer::TaaPass::create(passDesc);
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
    expectTrue(jitterReason == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass tryPreflightJitterNdc reject reason is None");
    expectTrue(pass->tryPreflightJitterAdvance(jitterReason), "pass tryPreflightJitterAdvance passes");
    expectTrue(jitterReason == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass tryPreflightJitterAdvance reject reason is None");
    expectTrue(pass->preflightJitterAdvance(), "pass preflightJitterAdvance passes");
    expectTrue(!pass->shouldSkipJitterAdvance(), "pass should not skip jitter advance");

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

    expectTrue(pass->classifyResolveSkip(resolveDesc) == fuse::renderer::TaaResolveSkipReason::HistoryNotReady,
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
    expectNear(weights.history, 0.f, 1e-5f, "pass tryCompute warmup history weight is zero");

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

    expectTrue(pass->classifyResolveSkip(resolveDesc) == fuse::renderer::TaaResolveSkipReason::None,
               "pass classifyResolveSkip is None after init with valid desc");
    expectTrue(pass->tryPreflightResolve(resolveDesc, skipReason),
               "pass tryPreflightResolve passes after init");
    expectTrue(skipReason == fuse::renderer::TaaResolveSkipReason::None,
               "pass tryPreflightResolve skip reason is None after init");

    expectTrue(pass->resolveFrame(resolveDesc), "initial resolve warms pass history");

    expectTrue(pass->tryPreflightHistoryReuse(0u, reuseReason),
               "pass tryPreflightHistoryReuse passes after warmup");
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseBlockReason::None,
               "pass tryPreflightHistoryReuse reason is None after warmup");
    expectTrue(pass->tryPreflightResolveBlendWeights(resolveDesc, blendReason),
               "pass tryPreflightResolveBlendWeights passes after warmup");
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
    expectTrue(!zeroPass->tryPreflightJitterNdc(jitterReason),
               "zero-width pass tryPreflightJitterNdc fails");
    expectTrue(jitterReason == fuse::renderer::TaaJitterGuardRejectReason::InvalidViewport,
               "zero-width pass tryPreflightJitterNdc reason is InvalidViewport");
    expectTrue(zeroPass->shouldSkipJitterNdc(), "zero-width pass should skip NDC jitter");
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
                   fuse::renderer::TaaJitterGuardRejectReason::None,
               "invalid sequence desc falls back so classifyJitterAdvanceReject is None");

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
    testClampTaaParams();
    testRejectionSurfaceGuards();
    testSanitizeAndPreflightResolve();
    testHistoryGenerationGuardPasses();
    testInvalidateHistoryIfStale();
    testTaaPassSanitizeAndGenerationCurrent();
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
    testJitterProduceNdcGuards();
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
    testTaaPassTryClassifyWrappers();

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_taa_pass: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_taa_pass: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}

// --- deepen additive from deepen-b59-taa-history-resolve-skip-b406 ---
void testRejectionSurfaceHelpers() {
    expectTrue(!fuse::renderer::taaResolveRequiresRejectionSurfaces(params),
    expectTrue(fuse::renderer::taaResolveRequiresRejectionSurfaces(params),
void testPreflightAndCanProceedHelpers() {
    expectTrue(fuse::renderer::preflightTaaResolve(desc, history) == fuse::renderer::TaaResolveSkipReason::None,
    expectTrue(fuse::renderer::preflightTaaResolve(desc, history) ==
    testPreflightAndCanProceedHelpers();

// --- deepen additive from deepen-b59-taa-history-resolve-skip-guards-1865 ---
void testHistoryReadGuards() {
void testResolveSurfaceGuards() {
    expectTrue(fuse::renderer::taaResolveRejectionSurfacesComplete(desc),
    expectTrue(!fuse::renderer::taaResolveRejectionSurfacesComplete(desc),
    expectTrue(resolve.wouldSkip(desc, history, &skipReason) ==
               "shouldSkipTaaResolve matches TaaResolve::wouldSkip");

// --- deepen additive from deepen-b59-taa-history-resolve-skip-guards-878f ---
void testHistoryReusableGuard() {

// --- deepen additive from deepen-b59-taa-history-resolve-guards-f513 ---
void testTaaPassBlendAndReuseGuards() {

// --- deepen additive from deepen-b59-taa-history-blend-guards-748d ---
void testResolvePreflightHelpers() {
void testTaaPassResolvePreflight() {
    testResolvePreflightHelpers();
    testTaaPassResolvePreflight();

// --- deepen additive from deepen-b59-taa-guards-8293 ---
void testHistoryWarmupPreflight() {
    const fuse::renderer::TaaHistoryWarmupPreflight unwarmed = fuse::renderer::preflightTaaHistoryWarmup(history);
    const fuse::renderer::TaaHistoryWarmupPreflight warmed = fuse::renderer::preflightTaaHistoryWarmup(history);
void testHistoryReusePreflight() {
    const fuse::renderer::TaaHistoryReusePreflight current =
        fuse::renderer::preflightTaaHistoryReuse(history, 0u);
    const fuse::renderer::TaaHistoryReusePreflight stale =
    const fuse::renderer::TaaHistoryReusePreflight bypass =
        fuse::renderer::preflightTaaHistoryReuseForDesc(history, desc);
void testJitterSyncPreflight() {
    const fuse::renderer::TaaJitterSyncPreflight synced =
        fuse::renderer::preflightTaaJitterSync(jitter, 5u, 128u, 128u);
    const fuse::renderer::TaaJitterSyncPreflight drifted =
    const fuse::renderer::TaaJitterSyncPreflight invalidViewport =
        fuse::renderer::preflightTaaJitterSync(jitter, 6u, 0u, 128u);
void testResolveBlendPreflight() {
    const fuse::renderer::TaaResolveBlendPreflight warmup =
        fuse::renderer::preflightTaaResolveBlend(true, params, history);
    const fuse::renderer::TaaResolveBlendPreflight steady =
        fuse::renderer::preflightTaaResolveBlend(false, params, history);
    const fuse::renderer::TaaResolveBlendPreflight descPreflight =
        fuse::renderer::preflightTaaResolveBlendForDesc(desc, history);
    expectTrue(!descPreflight.history_reuse_allowed, "desc blend preflight blocks stale generation reuse");
    const fuse::renderer::TaaResolveBlendPreflight currentDescPreflight =
    expectTrue(currentDescPreflight.history_reuse_allowed,
void testTaaPassPreflightGuards() {
    const fuse::renderer::TaaJitterSyncPreflight jitterPreflight = pass->preflightJitterSync(4u);
    expectTrue(jitterPreflight.synced(), "pass jitter preflight reports synced state");
    const fuse::renderer::TaaHistoryWarmupPreflight warmupPreflight = pass->preflightHistoryWarmup();
    expectTrue(warmupPreflight.readyForResolve(), "pass warmup preflight ready for resolve");
    expectTrue(warmupPreflight.needs_warmup, "pass warmup preflight needs warmup before resolve");
    const fuse::renderer::TaaResolveBlendPreflight blendPreflight = pass->preflightResolveBlend(resolveDesc);
    expectTrue(blendPreflight.first_frame, "pass blend preflight marks first frame before resolve");
    expectTrue(!blendPreflight.history_blend_allowed, "pass blend preflight blocks history before resolve");
    const fuse::renderer::TaaHistoryReusePreflight reusePreflight = pass->preflightHistoryReuse(0u);
    expectTrue(reusePreflight.reuse_allowed, "pass reuse preflight allows current generation after resolve");
    const fuse::renderer::TaaHistoryReusePreflight staleReusePreflight = pass->preflightHistoryReuse(0u);
    expectTrue(!staleReusePreflight.reuse_allowed,
    const fuse::renderer::TaaHistoryReusePreflight descReusePreflight =
        pass->preflightHistoryReuseForDesc(resolveDesc);
    expectTrue(!descReusePreflight.reuse_allowed, "pass desc reuse preflight blocks after invalidate");
    const fuse::renderer::TaaResolveBlendPreflight warmedBlendPreflight = pass->preflightResolveBlend(resolveDesc);
    expectTrue(warmedBlendPreflight.first_frame, "pass blend preflight marks first frame after invalidate");
    expectTrue(!warmedBlendPreflight.history_blend_allowed,
    testHistoryWarmupPreflight();
    testHistoryReusePreflight();
    testJitterSyncPreflight();
    testResolveBlendPreflight();
    testTaaPassPreflightGuards();

// --- deepen additive from deepen-b59-taa-guards-e811 ---
void testHistoryWarmupGuards() {
void testJitterSyncGuards() {
        fuse::renderer::preflightTaaBlendWeights(desc, history);
void testTaaPassJitterSyncGuards() {

// --- deepen additive from deepen-b59-taa-jitter-history-preflights-ddf1 ---
void testHistoryWarmupPreflightGuards() {
    expectTrue(!fuse::renderer::taaHistoryReusePreflightPasses(history, 0u),
    expectTrue(fuse::renderer::taaHistoryReusePreflightPasses(history, 0u),
void testResolveSurfaceAndPreflightHelpers() {
void testResolveBlendPreflightGuards() {
    expectTrue(fuse::renderer::preflightTaaBlendWeights(true, params, &warmup),
    expectTrue(fuse::renderer::preflightTaaBlendWeights(false, params, &steady),
    expectTrue(fuse::renderer::taaResolveBlendPreflightPasses(desc, history),
    expectTrue(fuse::renderer::preflightTaaResolveBlend(desc, history, &blend),
               "preflightTaaResolveBlend succeeds before first resolve");
               "preflightTaaResolveBlend succeeds after warmup");
    expectTrue(!fuse::renderer::taaResolveBlendPreflightPasses(desc, history),
    expectTrue(!fuse::renderer::preflightTaaResolveBlend(desc, history, &blend),
               "preflightTaaResolveBlend rejects invalid dimensions");
    expectTrue(pass->preflightResolveBlend(desc, &warmupPassBlend), "pass preflightResolveBlend succeeds before warmup");
    expectTrue(pass->preflightResolveBlend(desc, &passBlend), "pass preflightResolveBlend succeeds after warmup");
    testHistoryWarmupPreflightGuards();
    testResolveSurfaceAndPreflightHelpers();
    testResolveBlendPreflightGuards();

// --- deepen additive from deepen-b59-taa-guards-94f6 ---
void testHistoryReuseRejectReasons() {
    expectTrue(std::strcmp(fuse::renderer::taaHistoryReuseRejectReasonLabel(
                               fuse::renderer::TaaHistoryReuseRejectReason::None),
                               fuse::renderer::TaaHistoryReuseRejectReason::NotReady),
                               fuse::renderer::TaaHistoryReuseRejectReason::NeedsWarmup),
                               fuse::renderer::TaaHistoryReuseRejectReason::StaleGeneration),
    expectTrue(fuse::renderer::classifyTaaHistoryReuseReject(emptyHistory, 0u) ==
                   fuse::renderer::TaaHistoryReuseRejectReason::NotReady,
    expectTrue(!fuse::renderer::tryTaaHistoryReuse(emptyHistory, 0u),
               "empty history fails tryTaaHistoryReuse");
    expectTrue(fuse::renderer::classifyTaaHistoryReuseReject(history, 0u) ==
                   fuse::renderer::TaaHistoryReuseRejectReason::NeedsWarmup,
    expectTrue(fuse::renderer::tryTaaHistoryReuse(history, 0u),
               "warmed history passes tryTaaHistoryReuse");
    expectTrue(fuse::renderer::classifyTaaHistoryReuseReject(history, 99u) ==
                   fuse::renderer::TaaHistoryReuseRejectReason::StaleGeneration,
    expectTrue(fuse::renderer::classifyTaaHistoryReuseReject(history, history.invalidateGeneration()) ==
                   fuse::renderer::TaaHistoryReuseRejectReason::None,
    expectTrue(fuse::renderer::classifyTaaHistoryReuseReject(
void testJitterSyncPreflightGuards() {
    expectTrue(std::strcmp(fuse::renderer::taaJitterSyncRejectReasonLabel(
                               fuse::renderer::TaaJitterSyncRejectReason::None),
                               fuse::renderer::TaaJitterSyncRejectReason::InvalidSequence),
                               fuse::renderer::TaaJitterSyncRejectReason::InvalidViewport),
                               fuse::renderer::TaaJitterSyncRejectReason::FrameIndexMismatch),
    fuse::renderer::TaaJitterSyncRejectReason syncReason = fuse::renderer::TaaJitterSyncRejectReason::None;
    expectTrue(fuse::renderer::preflightTaaJitterSync(jitter, 5u, 128u, 128u, &syncReason),
    expectTrue(syncReason == fuse::renderer::TaaJitterSyncRejectReason::None,
    expectTrue(!fuse::renderer::preflightTaaJitterSync(jitter, 6u, 128u, 128u, &syncReason),
    expectTrue(syncReason == fuse::renderer::TaaJitterSyncRejectReason::FrameIndexMismatch,
    expectTrue(!fuse::renderer::preflightTaaJitterSync(jitter, 5u, 0u, 128u, &syncReason),
    expectTrue(syncReason == fuse::renderer::TaaJitterSyncRejectReason::InvalidViewport,
    expectTrue(fuse::renderer::preflightTaaResolveBlendWeights(desc, history, &weights),
    fuse::renderer::TaaResolveBlendPreflight snapshot{};
    expectTrue(fuse::renderer::preflightTaaResolveBlend(desc, history, &snapshot),
    expectTrue(!fuse::renderer::preflightTaaResolveBlend(desc, history, &snapshot),
    expectTrue(pass->classifyHistoryReuseReject(0u) ==
    expectTrue(pass->preflightResolveBlend(resolveDesc, &snapshot), "pass preflight passes before resolve");
    expectTrue(pass->preflightResolveBlend(resolveDesc, &snapshot), "pass preflight passes after warmup");
    testHistoryReuseRejectReasons();
    testJitterSyncPreflightGuards();

// --- deepen additive from deepen-b59-taa-guards-f9b0 ---
void testHistoryWarmupGuardHelpers() {
    const fuse::renderer::TaaResolveBlendPreflight emptyPreflight =
        fuse::renderer::preflightTaaResolveBlend(desc, emptyHistory);
    expectTrue(!fuse::renderer::taaResolveBlendPreflightValid(emptyPreflight),
    const fuse::renderer::TaaResolveBlendPreflight warmupPreflight =
        fuse::renderer::preflightTaaResolveBlend(desc, history);
    expectTrue(fuse::renderer::taaResolveBlendPreflightValid(warmupPreflight),
    expectTrue(warmupPreflight.first_frame, "warmup preflight marks first frame");
    expectTrue(!warmupPreflight.history_reuse, "warmup preflight blocks history reuse");
    expectNear(warmupPreflight.weights.current, 1.f, 1e-5f, "warmup preflight uses full current weight");
    const fuse::renderer::TaaResolveBlendPreflight steadyPreflight =
    expectTrue(steadyPreflight.history_reuse, "steady preflight allows history reuse");
    expectTrue(!steadyPreflight.first_frame, "steady preflight is not first frame");
    const fuse::renderer::TaaResolveBlendPreflight passPreflight = pass->preflightResolveBlend(desc);
    expectTrue(fuse::renderer::taaResolveBlendPreflightValid(passPreflight),
               "pass preflightResolveBlend is valid for warmed history");
    expectTrue(passPreflight.history_reuse, "pass preflight allows history reuse");

// --- deepen additive from deepen-b59-taa-guards-2b1e ---
    expectTrue(fuse::renderer::preflightTaaResolveBlend(desc, history, &computed),
    expectTrue(!fuse::renderer::preflightTaaResolveBlend(desc, history, &computed),
void testTaaPassJitterSyncAndBlendPreflight() {
    expectTrue(pass->preflightResolveBlend(resolveDesc, &weights),
    expectTrue(!pass->preflightResolveBlend(resolveDesc, &weights),
    testTaaPassJitterSyncAndBlendPreflight();

// --- deepen additive from deepen-b59-taa-guards-94db ---
    expectTrue(!fuse::renderer::preflightTaaHistoryReuse(emptyHistory, 0u),
    expectTrue(fuse::renderer::preflightTaaHistoryReuse(history, 0u),
    expectTrue(!fuse::renderer::preflightTaaHistoryReuse(history, 0u),
        fuse::renderer::computeTaaResolveBlendPreflight(desc, history);
    expectTrue(fuse::renderer::preflightTaaResolveBlend(desc, history, &projected),
    expectTrue(fuse::renderer::preflightTaaResolveBlend(desc, history),
    expectTrue(!fuse::renderer::preflightTaaResolveBlend(desc, history, &projected),
void testTaaPassWarmupAndBlendPreflight() {
    expectTrue(pass->preflightResolveBlend(resolveDesc, &projected),
    testTaaPassWarmupAndBlendPreflight();

// --- deepen additive from deepen-b59-taa-guards-108b ---
void testHistoryWarmupReuseGuards() {
    expectTrue(fuse::renderer::tryComputeTaaResolveBlendWeights(desc, history, weights),
    expectTrue(fuse::renderer::preflightTaaResolveBlend(desc, history, &weights),
    expectTrue(fuse::renderer::taaResolveHistoryBlendPreflightPasses(desc, history),
    expectTrue(!fuse::renderer::taaResolveHistoryBlendPreflightPasses(desc, history),
    expectTrue(!fuse::renderer::tryComputeTaaResolveBlendWeights(desc, history, weights),
    expectTrue(!fuse::renderer::preflightTaaResolveBlend(desc, history),
void testTaaPassSyncWarmupAndBlendPreflight() {
    testTaaPassSyncWarmupAndBlendPreflight();

// --- deepen additive from deepen-b59-taa-guards-5b8c ---
    fuse::renderer::TaaHistoryReuseRejectReason reuseReason = fuse::renderer::TaaHistoryReuseRejectReason::None;
    expectTrue(!fuse::renderer::taaHistoryReusePreflight(history, 0u, &reuseReason),
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseRejectReason::HistoryNotWarmed,
    expectTrue(fuse::renderer::taaHistoryReusePreflight(history, 0u, &reuseReason),
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseRejectReason::None,
    reuseReason = fuse::renderer::TaaHistoryReuseRejectReason::None;
    expectTrue(!fuse::renderer::taaHistoryReusePreflight(history, 99u, &reuseReason),
    expectTrue(reuseReason == fuse::renderer::TaaHistoryReuseRejectReason::StaleGeneration,
                               fuse::renderer::TaaHistoryReuseRejectReason::HistoryNotWarmed),
    expectTrue(fuse::renderer::taaJitterSyncPreflight(jitter, 4u, 128u, 128u, &syncReason),
    expectTrue(syncReason == fuse::renderer::TaaJitterSyncRejectReason::None, "synced jitter reason is None");
    expectTrue(!fuse::renderer::taaJitterSyncPreflight(jitter, 3u, 128u, 128u, &syncReason),
    expectTrue(!fuse::renderer::taaJitterSyncPreflight(jitter, 4u, 0u, 128u, &syncReason),
    fuse::renderer::TaaBlendPreflightRejectReason blendReason =
        fuse::renderer::TaaBlendPreflightRejectReason::None;
    expectTrue(!fuse::renderer::taaResolveBlendPreflight(desc, history, &blendReason),
    expectTrue(blendReason == fuse::renderer::TaaBlendPreflightRejectReason::WarmupRequired,
    expectTrue(fuse::renderer::taaResolveBlendPreflight(desc, history, &blendReason),
    blendReason = fuse::renderer::TaaBlendPreflightRejectReason::None;
    expectTrue(blendReason == fuse::renderer::TaaBlendPreflightRejectReason::StaleGeneration,
    expectTrue(std::strcmp(fuse::renderer::taaBlendPreflightRejectReasonLabel(
                               fuse::renderer::TaaBlendPreflightRejectReason::WarmupRequired),
    expectTrue(!pass->preflightHistoryReuse(0u), "pass reuse preflight rejects unwarmed history");
    expectTrue(!pass->preflightResolveBlend(desc), "pass blend preflight rejects unwarmed history");
    expectTrue(pass->preflightHistoryReuse(0u), "pass reuse preflight passes warmed history");
    expectTrue(pass->preflightResolveBlend(resolveDesc), "pass blend preflight passes warmed history");

// --- deepen additive from deepen-b59-taa-guards-95f2 ---
void testPreflightTaaResolveBlend() {
    expectTrue(!fuse::renderer::preflightTaaResolveBlend(desc, history, &weights),
    testPreflightTaaResolveBlend();

// --- deepen additive from deepen-b59-taa-guards-a831 ---
    expectTrue(!fuse::renderer::preflightTaaHistoryWarmup(emptyHistory),
    expectTrue(fuse::renderer::preflightTaaHistoryWarmup(history), "allocated history passes warmup preflight");
    expectTrue(!history.preflightReuse(0u), "preflightReuse false before first resolve");
    expectTrue(history.preflightReuse(0u), "preflightReuse true after first resolve");
    expectTrue(!history.preflightReuse(history.invalidateGeneration()),
    expectTrue(history.preflightReuse(history.invalidateGeneration()),
    fuse::renderer::TaaResolveBlendPreflightRejectReason rejectReason =
        fuse::renderer::TaaResolveBlendPreflightRejectReason::None;
    expectTrue(!fuse::renderer::preflightTaaResolveBlend(desc, emptyHistory, &rejectReason),
    expectTrue(rejectReason == fuse::renderer::TaaResolveBlendPreflightRejectReason::HistoryNotReady,
    expectTrue(std::strcmp(fuse::renderer::taaResolveBlendPreflightRejectReasonLabel(
                               fuse::renderer::TaaResolveBlendPreflightRejectReason::HistoryNotReady),
    expectTrue(fuse::renderer::preflightTaaResolveBlend(desc, history, &rejectReason),
    expectTrue(rejectReason == fuse::renderer::TaaResolveBlendPreflightRejectReason::None,
    expectTrue(fuse::renderer::diagnoseTaaResolveBlendPreflight(desc, history) ==
                   fuse::renderer::TaaResolveBlendPreflightRejectReason::None,
    expectTrue(!pass->preflightHistoryReuse(0u), "pass reuse preflight false before warmup");
    expectTrue(pass->preflightResolveBlend(resolveDesc, &rejectReason),
    expectTrue(pass->preflightHistoryReuse(0u), "pass reuse preflight true after warmup");

// --- deepen additive from deepen-taa-b59-guards-f7b5 ---
void testTaaPassPreflightAndSyncGuards() {
    expectTrue(pass->preflightResolveBlend(resolveDesc), "pass blend preflight passes on warmup frame");
    expectTrue(pass->preflightHistoryReuse(0u, &reuseReason), "pass reuse preflight passes after warmup");
    expectTrue(pass->preflightResolveBlend(resolveDesc), "pass blend preflight passes after warmup");
    expectTrue(pass->preflightHistoryReuse(0u), "pass reuse still valid with current generation");
    testTaaPassPreflightAndSyncGuards();

// --- deepen additive from deepen-b59-taa-guards-27d7 ---
void testHistoryWarmupCompositeGuards() {
void testJitterSyncBlockGuards() {
    expectTrue(fuse::renderer::preflightTaaJitterSync(5u, 128u, 128u, 8u, &reason),
    expectTrue(!fuse::renderer::preflightTaaJitterSync(5u, 0u, 128u, 8u, &reason),
    expectTrue(jitter.preflightSync(5u, 128u, 128u, &reason),
               "default jitter preflightSync passes for valid viewport");
    expectTrue(!jitter.preflightSync(5u, 0u, 128u, &reason),
               "default jitter preflightSync fails for zero width");
void testPreflightTaaResolveWithBlend() {
    expectTrue(fuse::renderer::preflightTaaResolveWithBlend(desc, history, &skipReason, &blendReason),
    expectTrue(blendReason == fuse::renderer::TaaResolveBlendRejectReason::None,
    expectTrue(!fuse::renderer::preflightTaaResolveWithBlend(desc, history, &skipReason, &blendReason),
void testTaaPassCompositeGuards() {
    expectTrue(pass->preflightJitterSync(4u, &syncReason), "pass jitter sync preflight passes");
    expectTrue(pass->preflightResolveWithBlend(resolveDesc, &skipReason, &blendReason),
    testPreflightTaaResolveWithBlend();

// --- deepen additive from deepen-b59-taa-guards-117f ---
    expectTrue(!fuse::renderer::preflightTaaHistoryWarmup(emptyHistory, &reason),
    expectTrue(fuse::renderer::preflightTaaHistoryWarmup(history, &reason),
void testPreflightTaaHistoryReuseForResolve() {
    expectTrue(!fuse::renderer::preflightTaaHistoryReuseForResolve(desc, history, &reason),
    expectTrue(fuse::renderer::preflightTaaHistoryReuseForResolve(desc, history, &reason),
    expectTrue(!fuse::renderer::preflightTaaJitterSync(jitter, 5u, &reason),
    expectTrue(fuse::renderer::preflightTaaJitterSync(jitter, 5u, &reason),
void testPreflightTaaResolveFrame() {
    expectTrue(fuse::renderer::preflightTaaResolveFrame(desc, history, &skipReason, &blendReason),
    expectTrue(!fuse::renderer::preflightTaaResolveFrame(desc, history, &skipReason, &blendReason),
void testTaaPassWarmupAndResolveFramePreflight() {
    expectTrue(pass->preflightHistoryWarmup(&warmupReason), "pass warmup preflight passes after init");
    expectTrue(!pass->preflightHistoryReuseForResolve(resolveDesc, &reuseReason),
    expectTrue(!pass->preflightJitterSync(4u, &jitterReason), "pass jitter not synced to frame four");
    expectTrue(pass->preflightJitterSync(4u, &jitterReason), "pass jitter sync preflight passes after sync");
    expectTrue(pass->preflightResolveFrame(resolveDesc, &skipReason, &blendReason),
    expectTrue(pass->preflightHistoryReuseForResolve(resolveDesc, &reuseReason),
    testPreflightTaaHistoryReuseForResolve();
    testPreflightTaaResolveFrame();
    testTaaPassWarmupAndResolveFramePreflight();

// --- deepen additive from deepen-fuse-b59-taa-cd32 ---
void testJitterSyncRejectGuards() {
    expectTrue(fuse::renderer::classifyTaaJitterSyncReject(5u, 8u) ==
    expectTrue(fuse::renderer::classifyTaaJitterSyncReject(5u, 0u) ==
                   fuse::renderer::TaaJitterSyncRejectReason::InvalidSequence,
    expectTrue(fuse::renderer::preflightTaaJitterSync(13u, 8u, &syncReason),
    expectTrue(fuse::renderer::canPreflightTaaJitterSync(13u, 8u),
               "canPreflightTaaJitterSync passes for valid sequence");
    expectTrue(!fuse::renderer::preflightTaaJitterSync(0u, 0u, &syncReason),
    expectTrue(syncReason == fuse::renderer::TaaJitterSyncRejectReason::InvalidSequence,
    expectTrue(jitter.preflightSyncToFrameIndex(4u, &syncReason),
               "jitter preflightSyncToFrameIndex passes for valid sequence");
    expectTrue(jitter.classifySyncReject(4u) == fuse::renderer::TaaJitterSyncRejectReason::None,
               "jitter classifySyncReject passes for valid sequence");
void testJitterNdcRejectGuards() {
    expectTrue(std::strcmp(fuse::renderer::taaJitterNdcRejectReasonLabel(
                               fuse::renderer::TaaJitterNdcRejectReason::None),
                               fuse::renderer::TaaJitterNdcRejectReason::InvalidViewport),
                   fuse::renderer::TaaJitterNdcRejectReason::None,
                   fuse::renderer::TaaJitterNdcRejectReason::InvalidViewport,
                   fuse::renderer::TaaJitterNdcRejectReason::InvalidSequence,
    fuse::renderer::TaaJitterNdcRejectReason ndcReason = fuse::renderer::TaaJitterNdcRejectReason::None;
    expectTrue(fuse::renderer::preflightTaaJitterNdc(128u, 128u, 8u, &ndcReason),
    expectTrue(fuse::renderer::canPreflightTaaJitterNdc(128u, 128u, 8u),
               "canPreflightTaaJitterNdc passes for valid viewport");
    expectTrue(!fuse::renderer::preflightTaaJitterNdc(0u, 128u, 8u, &ndcReason),
    expectTrue(ndcReason == fuse::renderer::TaaJitterNdcRejectReason::InvalidViewport,
    expectTrue(jitter.preflightCurrentNdcOffset(128u, 128u, &ndcReason),
               "jitter preflightCurrentNdcOffset passes for valid viewport");
    expectTrue(jitter.classifyNdcReject(128u, 128u) == fuse::renderer::TaaJitterNdcRejectReason::None,
               "jitter classifyNdcReject passes for valid viewport");
    expectTrue(!fuse::renderer::preflightTaaHistoryWarmup(history, &warmupReason),
    expectTrue(!fuse::renderer::canPreflightTaaHistoryWarmup(history),
               "canPreflightTaaHistoryWarmup fails before first resolve");
    expectTrue(fuse::renderer::preflightTaaHistoryWarmup(history, &warmupReason),
    expectTrue(fuse::renderer::canPreflightTaaHistoryWarmup(history),
               "canPreflightTaaHistoryWarmup passes after warmup");
    expectTrue(fuse::renderer::canPreflightTaaHistoryReuse(history, 0u),
               "canPreflightTaaHistoryReuse passes after warmup");
void testResolveTemporalBlendPreflightGuards() {
    expectTrue(std::strcmp(fuse::renderer::taaResolveTemporalRejectReasonLabel(
                               fuse::renderer::TaaResolveTemporalRejectReason::None),
                               fuse::renderer::TaaResolveTemporalRejectReason::HistoryReuseBlocked),
    fuse::renderer::TaaResolveTemporalRejectReason temporalReason =
        fuse::renderer::TaaResolveTemporalRejectReason::None;
    expectTrue(fuse::renderer::classifyTaaResolveTemporalReject(desc, history) ==
                   fuse::renderer::TaaResolveTemporalRejectReason::HistoryReuseBlocked,
    expectTrue(!fuse::renderer::preflightTaaResolveTemporalBlend(desc, history, &temporalReason),
    expectTrue(temporalReason == fuse::renderer::TaaResolveTemporalRejectReason::HistoryReuseBlocked,
    expectTrue(fuse::renderer::preflightTaaResolveTemporalBlend(desc, history, &temporalReason),
    expectTrue(fuse::renderer::canPreflightTaaResolveTemporalBlend(desc, history),
               "canPreflightTaaResolveTemporalBlend passes after warmup");
    expectTrue(fuse::renderer::tryComputeTaaResolveBlendWeights(desc, history, weights, &blendReason),
               "tryComputeTaaResolveBlendWeights succeeds after warmup");
    expectNear(weights.current, 0.2f, 1e-5f, "tryCompute fills configured current blend");
    expectNear(weights.history, 0.8f, 1e-5f, "tryCompute fills history blend complement");
    expectTrue(fuse::renderer::canPreflightTaaResolveBlendWeights(desc, history),
               "canPreflightTaaResolveBlendWeights passes after warmup");
void testTaaPassDeepenPreflightWrappers() {
    expectTrue(pass->classifyJitterSyncReject(9u) == fuse::renderer::TaaJitterSyncRejectReason::None,
               "pass classifyJitterSyncReject passes for valid frame");
    expectTrue(pass->preflightJitterSync(9u, &syncReason), "pass preflightJitterSync passes");
    expectTrue(!pass->canPreflightHistoryWarmup(), "pass warmup preflight fails before resolve");
    expectTrue(pass->preflightResolveBlendWeights(resolveDesc),
    expectTrue(pass->canPreflightHistoryWarmup(), "pass warmup preflight passes after resolve");
    expectTrue(pass->canPreflightHistoryReuse(0u), "pass reuse preflight passes after resolve");
    expectTrue(pass->canPreflightResolveTemporalBlend(resolveDesc),
    expectTrue(pass->tryExpectedResolveBlendWeights(resolveDesc, weights),
               "pass tryExpectedResolveBlendWeights succeeds after resolve");
    expectNear(weights.current, 0.35f, 1e-5f, "pass tryExpected fills configured current blend");
    testResolveTemporalBlendPreflightGuards();
    testTaaPassDeepenPreflightWrappers();

// --- deepen additive from deepen-b59-taa-guards-298c ---
void testSafeNdcOffsetGuards() {
    expectTrue(fuse::renderer::preflightTaaJitterSync(jitter, 0u), "default jitter passes sync preflight");
    expectTrue(fuse::renderer::preflightTaaJitterSync(jitter, 5u), "synced jitter passes preflight");
    expectTrue(fuse::renderer::preflightTaaJitterSync(fallbackJitter, 0u),
void testBlendFactorRangeGuards() {
    expectTrue(!fuse::renderer::preflightTaaHistoryWarmup(history, &reason),
void testResolveFramePreflightGuards() {
void testTaaPassResolveFrameAndJitterGuards() {
    expectTrue(!pass->preflightJitterSync(99u, &jitterReason), "pass jitter sync preflight fails when drifted");
    expectTrue(pass->preflightJitterSync(99u, &jitterReason), "pass jitter sync preflight passes after sync");
    testResolveFramePreflightGuards();

// --- deepen additive from deepen-b59-taa-guards-1d2e ---
    expectTrue(!fuse::renderer::taaHistoryReuseBlockReasonIsBlocking(
    expectTrue(fuse::renderer::taaHistoryReuseBlockReasonIsBlocking(
    expectTrue(!fuse::renderer::taaResolveBlendRejectReasonIsBlocking(
    expectTrue(fuse::renderer::taaResolveBlendRejectReasonIsBlocking(
    expectTrue(!fuse::renderer::taaJitterSyncBlockReasonIsBlocking(
    expectTrue(fuse::renderer::taaJitterSyncBlockReasonIsBlocking(
void testPreflightTaaHistoryReuseForDesc() {
    expectTrue(!fuse::renderer::preflightTaaHistoryReuseForDesc(desc, emptyHistory, &reason),
    expectTrue(!fuse::renderer::preflightTaaHistoryReuseForDesc(desc, history, &reason),
    expectTrue(fuse::renderer::preflightTaaHistoryReuseForDesc(desc, history, &reason),
void testPreflightTaaResolveGuards() {
    expectTrue(fuse::renderer::preflightTaaResolveGuards(desc, history, &skipReason, &blendReason),
    expectTrue(!fuse::renderer::preflightTaaResolveGuards(desc, history, &skipReason, &blendReason),
void testJitterSyncPreflightHelpers() {
    expectTrue(fuse::renderer::preflightTaaJitterSync(5u, 8u, &reason),
    expectTrue(!fuse::renderer::preflightTaaJitterSync(0u, 0u, &reason),
    expectTrue(jitter.preflightSync(4u, &reason), "jitter instance preflight sync passes");
    expectTrue(fallbackJitter.preflightSync(2u, &reason), "fallback jitter preflight sync succeeds after fallback");
    expectTrue(pass->preflightJitterSync(2u), "pass jitter sync preflight passes when aligned");
void testTaaPassResolveGuardsPreflight() {
    expectTrue(!pass->preflightHistoryReuseForDesc(resolveDesc, &reuseReason),
    expectTrue(pass->preflightResolveGuards(resolveDesc, &skipReason, &blendReason),
    expectTrue(pass->preflightHistoryReuseForDesc(resolveDesc, &reuseReason),
    testPreflightTaaHistoryReuseForDesc();
    testPreflightTaaResolveGuards();
    testJitterSyncPreflightHelpers();
    testTaaPassResolveGuardsPreflight();

// --- deepen additive from deepen-b59-taa-guards-3c58 ---
void testHistoryWarmupPhaseGuards() {
    expectTrue(!fuse::renderer::preflightTaaHistoryWarmup(history, &phase),
    expectTrue(fuse::renderer::preflightTaaHistoryWarmup(history, &phase),
    expectTrue(TaaJitterLayout::classifySyncReject(8u) == fuse::renderer::TaaJitterSyncRejectReason::None,
    expectTrue(TaaJitterLayout::classifySyncReject(0u) ==
    fuse::renderer::TaaJitterSyncRejectReason reason = fuse::renderer::TaaJitterSyncRejectReason::None;
    expectTrue(TaaJitterLayout::preflightSyncToFrameIndex(5u, 8u, &reason),
    expectTrue(reason == fuse::renderer::TaaJitterSyncRejectReason::None,
    expectTrue(!TaaJitterLayout::preflightSyncToFrameIndex(5u, 0u, &reason),
    expectTrue(reason == fuse::renderer::TaaJitterSyncRejectReason::InvalidSequence,
    expectTrue(jitter.classifySyncReject(5u) == fuse::renderer::TaaJitterSyncRejectReason::None,
    expectTrue(jitter.preflightSyncToFrameIndex(5u, &reason), "jitter preflightSyncToFrameIndex succeeds");
    expectTrue(jitter.isAlignedToFrameIndex(5u), "jitter aligned after preflightSyncToFrameIndex");
    expectTrue(jitter.monotonicFrameIndex() == 5u, "jitter monotonic counter set by preflightSyncToFrameIndex");
    expectTrue(pass->classifyJitterSyncReject(7u) == fuse::renderer::TaaJitterSyncRejectReason::None,
    expectTrue(pass->preflightJitterSync(7u, &reason), "pass preflightJitterSync succeeds");
void testResolveTemporalBlendPreflight() {
    expectTrue(!fuse::renderer::preflightTaaResolveTemporalBlend(desc, history, &blendReason, &reuseReason),
    expectTrue(fuse::renderer::preflightTaaResolveTemporalBlend(desc, history, &blendReason, &reuseReason),
    expectTrue(pass->preflightHistoryWarmup(), "pass warmup preflight passes after resolve");
    expectTrue(pass->preflightResolveTemporalBlend(desc, &blendReason, &reuseReason),
    testResolveTemporalBlendPreflight();

// --- deepen additive from deepen-b59-taa-guards-d966 ---
void testHistoryWarmupStateGuards() {
    expectTrue(!fuse::renderer::preflightTaaHistoryWarmup(emptyHistory, &state),
    expectTrue(!fuse::renderer::preflightTaaHistoryWarmup(history, &state),
    expectTrue(fuse::renderer::preflightTaaHistoryWarmup(history, &state),
void testJitterFrameIndexSlotDriftGuards() {
void testResolveBlendModeGuards() {
void testTaaPassWarmupBlendAndJitterDriftGuards() {
    expectTrue(!pass->preflightHistoryWarmup(&warmupState),
    expectTrue(pass->preflightHistoryWarmup(&warmupState), "pass warmup preflight passes after resolve");

// --- deepen additive from deepen-b59-taa-guards-53dc ---
void testPreflightTaaHistoryWarmup() {
void testJitterAdvanceIfAlignedGuards() {
    expectTrue(fuse::renderer::preflightTaaJitterAlignment(6u, jitter),
               "preflightTaaJitterAlignment passes for current frame");
    expectTrue(!fuse::renderer::preflightTaaJitterAlignment(5u, jitter),
               "preflightTaaJitterAlignment fails for stale frame");
    expectTrue(pass->preflightJitterAlignment(4u), "pass preflightJitterAlignment passes when synced");
void testPreflightTaaResolveDesc() {
    fuse::renderer::TaaResolveDescPreflight result{};
    expectTrue(!fuse::renderer::preflightTaaResolveDesc(desc, emptyHistory, &result),
    expectTrue(fuse::renderer::preflightTaaResolveDesc(desc, history, &result),
    expectTrue(result.blend_reject_reason == fuse::renderer::TaaResolveBlendRejectReason::None,
    expectTrue(resolve.preflightDesc(desc, history, &result),
               "TaaResolve::preflightDesc passes for valid desc");
    expectTrue(!fuse::renderer::preflightTaaResolveDesc(desc, history, &result),
    expectTrue(!pass->preflightHistoryWarmup(&warmupReason),
    expectTrue(pass->preflightResolveDesc(desc, &result), "pass preflightResolveDesc passes valid desc");
    expectTrue(pass->resolveFrame(desc), "resolveFrame succeeds after preflightResolveDesc");
    expectTrue(pass->preflightHistoryWarmup(&warmupReason), "pass warmup preflight passes after resolve");
    testPreflightTaaHistoryWarmup();
    testPreflightTaaResolveDesc();

// --- deepen additive from deepen-b59-taa-guards-efe8 ---
    expectTrue(!fuse::renderer::tryPreflightTaaHistoryWarmup(history, reason),
               "tryPreflightTaaHistoryWarmup fails before first resolve");
               "tryPreflightTaaHistoryWarmup reason is NeedsWarmup");
void testHistoryReuseForResolveGuards() {
    expectTrue(fuse::renderer::preflightTaaHistoryReuseForResolve(desc, history),
    expectTrue(!fuse::renderer::tryPreflightTaaHistoryReuseForResolve(desc, history, reason),
               "tryPreflightTaaHistoryReuseForResolve fails on stale generation");
               "tryPreflightTaaHistoryReuseForResolve reason is StaleGeneration");
void testJitterSyncViewportGuards() {
    expectTrue(jitter.trySyncToFrameIndexIfReady(4u, reason), "trySyncToFrameIndexIfReady succeeds");
    expectTrue(jitter.trySyncToFrameIndexIfViewportReady(6u, 1920u, 1080u, reason),
               "trySyncToFrameIndexIfViewportReady succeeds with valid viewport");
    expectTrue(jitter.tryAdvanceIfReady(advanceReason), "tryAdvanceIfReady succeeds");
    expectTrue(jitter.tryAdvanceIfViewportReady(1920u, 1080u, advanceReason),
               "tryAdvanceIfViewportReady succeeds with valid viewport");
    expectTrue(!pass->trySyncJitterToFrameIndexIfReady(2u, passReason),
               "pass trySyncJitterToFrameIndexIfReady blocks zero width");
void testResolveTemporalAccumulationPreflight() {
    fuse::renderer::TaaResolveTemporalPreflight preflight{};
    expectTrue(fuse::renderer::preflightTaaResolveTemporalAccumulation(desc, history, &preflight),
    expectTrue(preflight.blend_reject == fuse::renderer::TaaResolveBlendRejectReason::None,
    expectTrue(!fuse::renderer::preflightTaaResolveTemporalAccumulation(desc, history, &preflight),
               "tryPreflightTaaResolveBlendWeights succeeds for steady history");
    expectTrue(pass->preflightHistoryWarmup(), "pass warmup preflight passes after first resolve");
    expectTrue(pass->preflightResolveTemporalAccumulation(desc, &preflight),
    testResolveTemporalAccumulationPreflight();

// --- deepen additive from deepen-b59-taa-guards-8394 ---
void testTaaJitterSyncRejectReasonGuards() {
    expectTrue(fuse::renderer::classifyTaaJitterSyncReject(0u, 0u) ==
    expectTrue(fuse::renderer::preflightTaaJitterSync(12u, 8u, &reason),
               "preflightTaaJitterSync fails for invalid sequence");
    expectTrue(!fuse::renderer::tryCanBeginTemporalReuse(history, 0u, &reason),
               "tryCanBeginTemporalReuse fails before warmup");
               "tryCanBeginTemporalReuse reason is NotWarm before warmup");
    expectTrue(fuse::renderer::tryCanBeginTemporalReuse(history, 0u, &reason),
               "tryCanBeginTemporalReuse passes after warmup");
               "tryCanBeginTemporalReuse reason is None after warmup");
               "tryCanBeginTemporalReuse fails after invalidate");
               "tryCanBeginTemporalReuse reason is StaleGeneration after invalidate");
    fuse::renderer::TaaResolveFramePreflight preflight{};
    expectTrue(fuse::renderer::preflightTaaResolveFrame(desc, history, &preflight),
    expectTrue(preflight.blend_reason == fuse::renderer::TaaResolveBlendRejectReason::None,
               "tryComputeTaaResolveBlendWeights succeeds for warmup frame");
    expectNear(weights.current, 1.f, 1e-5f, "tryCompute blend uses full current on warmup");
    expectNear(weights.history, 0.f, 1e-5f, "tryCompute blend uses zero history on warmup");
    expectTrue(!fuse::renderer::preflightTaaResolveFrame(desc, history, &preflight),
    expectNear(weights.current, 0.2f, 1e-5f, "tryCompute blend uses configured current after warmup");
    expectNear(weights.history, 0.8f, 1e-5f, "tryCompute blend uses history complement after warmup");
    expectTrue(!pass->tryCanBeginTemporalReuse(0u, &reuseReason),
               "pass tryCanBeginTemporalReuse fails before warmup");
    fuse::renderer::TaaResolveFramePreflight framePreflight{};
    expectTrue(pass->preflightResolveFrame(resolveDesc, &framePreflight),
               "pass preflightResolveFrame passes before first resolve");
    expectTrue(framePreflight.canProceed, "pass resolve frame preflight canProceed before first resolve");
    expectTrue(pass->preflightJitterSync(4u, &syncReason), "pass preflightJitterSync passes");
    expectTrue(pass->trySyncJitterToFrameIndexIfReady(4u, &syncReason),
               "pass trySyncJitterToFrameIndexIfReady succeeds");
    expectTrue(!pass->needsJitterResync(4u), "pass jitter aligned after trySync");
    expectTrue(pass->jitterAlignedToFrameIndex(4u), "pass jitterAlignedToFrameIndex after trySync");
    expectTrue(pass->tryCanBeginTemporalReuse(0u, &reuseReason),
               "pass tryCanBeginTemporalReuse passes after warmup");
    expectTrue(pass->tryExpectedResolveBlendWeights(resolveDesc, weights, &blendReason),
               "pass tryExpectedResolveBlendWeights succeeds after warmup");
    expectNear(weights.current, 0.35f, 1e-5f, "pass tryExpectedResolveBlendWeights current weight");
    expectNear(weights.history, 0.65f, 1e-5f, "pass tryExpectedResolveBlendWeights history weight");
    testTaaJitterSyncRejectReasonGuards();

// --- deepen additive from deepen-b59-taa-guards-9737 ---
void testJitterSyncRejectReasonGuards() {
    expectTrue(fuse::renderer::trySyncJitterToFrameIndexIfReady(jitter, 5u, syncReason),
               "trySync succeeds for valid sequence");
               "trySync reject reason is None on success");
               "jitter aligned after trySync");
    expectTrue(fuse::renderer::trySyncJitterToFrameIndexIfReady(fallbackJitter, 2u, syncReason),
void testHistorySampleAndWarmupGuards() {
    expectTrue(!fuse::renderer::tryPreflightTaaHistoryReuse(emptyHistory, 0u, reuseReason),
               "tryPreflight fails for empty history");
               "empty history tryPreflight reason is NotReady");
    expectTrue(!fuse::renderer::tryPreflightTaaHistoryReuseForResolve(desc, history, reuseReason),
    expectTrue(fuse::renderer::tryPreflightTaaHistoryReuseForResolve(desc, history, reuseReason),
void testResolveBlendApplyGuards() {
               "tryPreflight blend passes on warmup frame");
               "warmup tryPreflight reject reason is None");
               "tryPreflight blend passes after warmup");
    expectTrue(pass->trySyncJitterToFrameIndexIfReady(4u, syncReason),
    expectTrue(!pass->jitterNeedsResyncToFrameIndex(4u), "pass jitter aligned after trySync");
    expectTrue(!pass->tryPreflightHistoryReuseForResolve(resolveDesc, reuseReason),
    expectTrue(pass->tryPreflightHistoryReuseForResolve(resolveDesc, reuseReason),
    testJitterSyncRejectReasonGuards();

// --- deepen additive from deepen-taa-b59-guards-fd0d ---
    expectTrue(!fuse::renderer::preflightTaaHistoryWarmup(emptyHistory, &warmupReason),
void testPreflightTaaResolveHistoryReuse() {
    expectTrue(!fuse::renderer::preflightTaaResolveHistoryReuse(desc, emptyHistory, &reason),
    expectTrue(!fuse::renderer::preflightTaaResolveHistoryReuse(desc, history, &reason),
    expectTrue(fuse::renderer::preflightTaaResolveHistoryReuse(desc, history, &reason),
    expectTrue(fuse::renderer::preflightTaaJitterSync(5u, 1920u, 1080u, 8u, &syncReason),
    expectTrue(!fuse::renderer::preflightTaaJitterSync(0u, 0u, 1080u, 8u, &syncReason),
    expectTrue(!fuse::renderer::preflightTaaJitterSync(0u, 1920u, 1080u, 0u, &syncReason),
    expectTrue(jitter.preflightSyncToFrameIndex(4u, 128u, 128u, &syncReason),
void testTaaPassWarmupAndSyncPreflightHelpers() {
    expectTrue(!pass->preflightHistoryWarmup(&warmupReason), "pass warmup preflight fails before resolve");
    expectTrue(pass->preflightJitterSync(2u, &syncReason), "pass jitter sync preflight passes");
    expectTrue(!pass->preflightResolveHistoryReuse(resolveDesc, &reuseReason),
    expectTrue(pass->preflightResolveHistoryReuse(resolveDesc, &reuseReason),
    testPreflightTaaResolveHistoryReuse();
    testTaaPassWarmupAndSyncPreflightHelpers();

// --- deepen additive from deepen-b59-taa-guards-0400 ---
void testPreflightTaaResolveTemporal() {
    expectTrue(fuse::renderer::preflightTaaResolveTemporal(desc, history, &reuseReason, &blendReason),
    expectTrue(!fuse::renderer::preflightTaaResolveTemporal(desc, history, &reuseReason, &blendReason),
    expectTrue(fuse::renderer::tryComputeTaaResolveBlendWeights(desc, history, &weights),
               "tryCompute succeeds on warmup frame");
    expectNear(weights.current, 1.f, 1e-5f, "tryCompute warmup current weight");
    expectNear(weights.history, 0.f, 1e-5f, "tryCompute warmup history weight");
    expectTrue(!fuse::renderer::tryComputeTaaResolveBlendWeights(desc, history, nullptr),
               "tryCompute rejects null output");
               "tryCompute succeeds after warmup");
void testJitterSyncIfMisalignedGuards() {
void testTaaPassWarmupAndTemporalPreflight() {
    expectTrue(pass->preflightResolveTemporal(resolveDesc, &reuseReason), "pass temporal preflight passes on warmup");
    expectTrue(pass->preflightResolveTemporal(resolveDesc), "pass temporal preflight passes after warmup");
    testPreflightTaaResolveTemporal();
    testTaaPassWarmupAndTemporalPreflight();

// --- deepen additive from deepen-b59-taa-guards-614c ---
void testWouldSkipAndTryHistoryReuseGuards() {
    expectTrue(fuse::renderer::wouldSkipTaaHistoryReuse(emptyHistory, 0u),
               "wouldSkip true for empty history");
    expectTrue(!fuse::renderer::tryPreflightTaaHistoryReuse(emptyHistory, 0u, reason),
               "tryPreflight reason is NotReady for empty history");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for wouldSkip history reuse test");
    expectTrue(history.init(resources, historyDesc), "history ready for wouldSkip reuse test");
    expectTrue(fuse::renderer::wouldSkipTaaHistoryReuse(history, 0u),
               "wouldSkip true for unwarmed history");
    expectTrue(!fuse::renderer::wouldSkipTaaHistoryReuse(history, 0u),
               "wouldSkip false for warmed history");
               "tryPreflight succeeds for warmed history");
               "wouldSkip true after invalidate");
               "tryPreflight fails after invalidate");
               "tryPreflight reason is StaleGeneration after invalidate");
void testWouldRejectAndTryResolveBlendGuards() {
    expectTrue(bootstrap != nullptr, "bootstrap allocated for wouldReject blend test");
    expectTrue(history.init(resources, historyDesc), "history ready for wouldReject blend test");
    expectTrue(!fuse::renderer::wouldRejectTaaResolveBlendWeights(desc, history),
               "tryPreflight blend succeeds for warmup weights");
               "tryPreflight blend succeeds after warmup");
    expectTrue(TaaJitterLayout::classifyTaaJitterSyncReject(5u, 8u) ==
    expectTrue(TaaJitterLayout::classifyTaaJitterSyncReject(0u, 0u) ==
    expectTrue(!TaaJitterLayout::wouldSkipSyncToFrameIndex(5u, 8u),
               "wouldSkip false for valid sequence");
    expectTrue(TaaJitterLayout::wouldSkipSyncToFrameIndex(0u, 0u),
               "wouldSkip true for invalid sequence");
    expectTrue(!jitter.wouldSkipSyncToFrameIndex(5u), "jitter wouldSkip false for valid sequence");
    expectTrue(jitter.trySyncToFrameIndexIfReady(5u, syncReason), "trySync succeeds for valid sequence");
               "trySync reason is None on success");
    expectTrue(jitter.isAlignedToFrameIndex(5u), "jitter aligned after trySync");
    expectTrue(fallbackJitter.wouldSkipSyncToFrameIndex(3u) == false,
    expectTrue(fallbackJitter.trySyncToFrameIndexIfReady(3u, syncReason),
               "trySync succeeds after fallback to default length");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass wouldSkip/try test");
    expectTrue(pass->init(resources), "TaaPass initialized for wouldSkip/try test");
    expectTrue(pass->wouldSkipHistoryReuse(0u), "pass wouldSkip history reuse before warmup");
    expectTrue(!pass->wouldSkipJitterSync(5u), "pass wouldSkip jitter sync false for valid sequence");
               "pass tryPreflight history reuse fails before warmup");
               "pass tryPreflight reuse reason is NotWarm before warmup");
    expectTrue(!pass->wouldRejectResolveBlendWeights(resolveDesc),
               "pass tryPreflight blend succeeds before first resolve");
    expectTrue(pass->trySyncJitterToFrameIndex(4u, syncReason), "pass trySync jitter succeeds");
    expectTrue(pass->jitterAlignedToFrameIndex(4u), "pass jitter aligned after trySync");
    expectTrue(!pass->wouldSkipHistoryReuse(0u), "pass wouldSkip history reuse false after warmup");
               "pass tryPreflight history reuse succeeds after warmup");

// --- deepen additive from deepen-b59-taa-guards-8a91 ---
void testHistoryWarmupCompleteGuard() {
void testJitterShouldSkipAndReadyGuards() {
void testResolveBlendReadyGuard() {
void testTaaPassWarmupAndJitterReadyGuards() {
    expectTrue(pass->preflightJitterNdc(&rejectReason), "pass preflightJitterNdc passes before init");
               "pass preflightJitterNdc reject reason is None before init");
    expectTrue(!zeroPass->preflightJitterNdc(&rejectReason),
               "zero-width pass preflightJitterNdc reject reason is InvalidViewport");

// --- deepen additive from deepen-b59-taa-guards-ceb9 ---
    expectTrue(std::strcmp(fuse::renderer::taaHistoryWarmupRejectReasonLabel(
                               fuse::renderer::TaaHistoryWarmupRejectReason::None),
                               fuse::renderer::TaaHistoryWarmupRejectReason::NotReady),
                               fuse::renderer::TaaHistoryWarmupRejectReason::AlreadyWarm),
    expectTrue(fuse::renderer::classifyTaaHistoryWarmupReject(emptyHistory) ==
                   fuse::renderer::TaaHistoryWarmupRejectReason::NotReady,
    fuse::renderer::TaaHistoryWarmupRejectReason reason =
        fuse::renderer::TaaHistoryWarmupRejectReason::None;
    expectTrue(!fuse::renderer::tryPreflightTaaHistoryWarmup(emptyHistory, reason),
               "tryPreflight warmup fails for empty history");
    expectTrue(reason == fuse::renderer::TaaHistoryWarmupRejectReason::NotReady,
               "tryPreflight warmup reason is NotReady for empty history");
    expectTrue(reason == fuse::renderer::TaaHistoryWarmupRejectReason::None,
    expectTrue(fuse::renderer::classifyTaaHistoryWarmupReject(history) ==
                   fuse::renderer::TaaHistoryWarmupRejectReason::AlreadyWarm,
    expectTrue(reason == fuse::renderer::TaaHistoryWarmupRejectReason::AlreadyWarm,
void testJitterShouldSkipAndTryNdcGuards() {
void testTaaPassWarmupAndJitterSkipGuards() {
    fuse::renderer::TaaHistoryWarmupRejectReason warmupReason =
    expectTrue(warmupReason == fuse::renderer::TaaHistoryWarmupRejectReason::NotReady,
    expectTrue(pass->preflightJitterSync(4u), "pass jitter sync preflight passes before init");
    expectTrue(pass->preflightJitterNdc(), "pass NDC jitter preflight passes before init");
    expectTrue(!zeroPass->preflightJitterNdc(), "zero-width pass NDC preflight fails");

// --- deepen additive from deepen-b59-taa-guards-2510 ---
void testJitterSkipAndReadyGuards() {
    expectTrue(fuse::renderer::tryPreflightTaaJitterNdc(1920u, 1080u, 8u, rejectReason),
    expectTrue(!fuse::renderer::tryPreflightTaaJitterNdc(0u, 1080u, 8u, rejectReason),
void testTaaPassSkipReadyAndTryPreflights() {
    expectTrue(pass->tryPreflightJitterSync(4u, jitterReject), "pass tryPreflightJitterSync passes");
    expectTrue(pass->tryPreflightJitterNdc(jitterReject), "pass tryPreflightJitterNdc passes");
    expectTrue(pass->preflightJitterNdc(&jitterReject), "pass preflightJitterNdc passes");
    expectNear(weights.current, 0.15f, 1e-5f, "pass tryCompute steady current weight");
    testTaaPassSkipReadyAndTryPreflights();

// --- deepen additive from deepen-b59-taa-guards-3780 ---
               "tryPreflight warmup fails for unwarmed history");
               "tryPreflight warmup reason is NeedsWarmup");
    expectTrue(fuse::renderer::tryPreflightTaaHistoryWarmup(history, reason),
               "tryPreflight warmup passes for warmed history");
               "tryPreflight warmup reason is None for warmed history");
void testResolvePreflightShouldSkip() {
               "tryPreflightTaaResolve passes for valid resolve");
               "tryPreflightTaaResolve rejects invalid dimensions");
    expectTrue(!pass->tryPreflightHistoryWarmup(warmupReason),
               "pass tryPreflightHistoryWarmup fails before resolve");
               "pass tryPreflightHistoryWarmup reason is NeedsWarmup");
    expectTrue(pass->tryPreflightResolve(resolveDesc, skipReason), "pass tryPreflightResolve passes");
    expectTrue(pass->tryPreflightHistoryWarmup(warmupReason), "pass tryPreflightHistoryWarmup passes");
               "pass tryPreflightHistoryWarmup reason is None after resolve");
    expectTrue(!zeroPass->tryPreflightJitterNdc(jitterReject),
    expectTrue(jitterReject == fuse::renderer::TaaJitterGuardRejectReason::InvalidViewport,
    testResolvePreflightShouldSkip();

// --- deepen additive from deepen-b59-taa-guards-b05b ---
void testTaaPassCombinedPreflightGuards() {
    expectTrue(pass->tryPreflightJitterSync(3u, jitterReject), "pass tryPreflightJitterSync passes");
    expectTrue(!zeroPass->preflightJitterNdc(&jitterReject),
    testTaaPassCombinedPreflightGuards();

// --- deepen additive from deepen-b59-taa-guards-bd40 ---
void testTaaDeepenDiagnosticGuardOverloads() {
               "wouldSkip alias matches shouldSkip for empty history");
               "shouldSkip and wouldSkip agree for empty history");
               "wouldSkip alias passes for warmed history");
    expectTrue(!fuse::renderer::wouldSkipTaaResolveBlend(desc, history),
               "wouldSkip alias passes for steady blend");
    expectTrue(fuse::renderer::wouldSkipTaaResolveBlend(desc, history) ==
               "wouldSkip and shouldSkip agree for resolve blend");
    expectTrue(fuse::renderer::tryPreflightTaaJitterNdc(128u, 128u, 8u, jitterReason),
    expectTrue(!fuse::renderer::tryPreflightTaaJitterNdc(0u, 128u, 8u, jitterReason),
    expectTrue(pass->wouldSkipHistoryReuse(0u), "pass wouldSkipHistoryReuse before warmup");
    expectTrue(!pass->wouldSkipResolveBlend(resolveDesc), "pass wouldSkipResolveBlend before warmup resolve");
    expectTrue(!pass->wouldSkipHistoryReuse(0u), "pass wouldSkipHistoryReuse after warmup");
    expectTrue(!pass->wouldSkipResolveBlend(resolveDesc), "pass wouldSkipResolveBlend after warmup");

// --- deepen additive from deepen-b59-taa-guards-8e7a ---
void testHistoryWarmupRejectGuards() {
    fuse::renderer::TaaHistoryWarmupRejectReason reason = fuse::renderer::TaaHistoryWarmupRejectReason::None;
               "tryPreflight warmup passes for unwarmed history");
void testJitterShouldSkipAndAdvanceGuards() {
    expectTrue(!fuse::renderer::tryPreflightTaaJitterAdvance(0u, rejectReason),
               "tryPreflightTaaJitterAdvance rejects invalid sequence");
void testResolveHistoryBlendPreflightGuards() {
    expectTrue(!fuse::renderer::tryPreflightTaaResolveHistoryBlend(desc, history, rejectReason),
               "tryPreflight history blend fails on warmup");
    expectTrue(fuse::renderer::tryPreflightTaaResolveHistoryBlend(desc, history, rejectReason),
               "tryPreflight history blend passes after warmup");
    expectTrue(pass->preflightJitterAdvance(&jitterReject), "pass preflightJitterAdvance passes");
    testResolveHistoryBlendPreflightGuards();

// --- deepen additive from deepen-b59-taa-guards-aa69 ---
    expectTrue(!fuse::renderer::tryPreflightTaaHistoryWarmup(history, warmupState),
               "tryPreflightTaaHistoryWarmup state is NeedsWarmup");
    expectTrue(fuse::renderer::tryPreflightTaaHistoryWarmup(history, warmupState),
               "tryPreflightTaaHistoryWarmup passes after warmup");
               "tryPreflightTaaHistoryWarmup state is Ready");
    expectTrue(pass->preflightJitterNdc(&rejectReason), "pass preflightJitterNdc passes");
void testResolveTemporalPreflightGuards() {
                               fuse::renderer::TaaResolveTemporalRejectReason::BlendWeightsRejected),
    expectTrue(fuse::renderer::preflightTaaResolveTemporal(desc, history, &temporalReason),
    expectTrue(temporalReason == fuse::renderer::TaaResolveTemporalRejectReason::None,
    expectTrue(fuse::renderer::tryPreflightTaaResolveTemporal(desc, history, temporalReason),
    expectTrue(pass->preflightResolveTemporal(resolveDesc, &temporalReason),
    expectTrue(pass->preflightHistoryWarmup(), "pass preflightHistoryWarmup passes after resolve");
    testResolveTemporalPreflightGuards();

// --- deepen additive from deepen-b59-taa-guards-6ba7 ---
               "tryPreflightTaaHistoryWarmup fails for empty history");
               "tryPreflightTaaHistoryWarmup fails for unwarmed history");
    expectTrue(fuse::renderer::tryComputeTaaJitterNdcOffset(3u, 128u, 128u, 8u, ndcOut, rejectReason),
               "tryComputeTaaJitterNdcOffset succeeds for valid viewport");
    expectNear(ndcOut.x, expected.x, 1e-6f, "tryComputeTaaJitterNdcOffset matches layout X");
    expectNear(ndcOut.y, expected.y, 1e-6f, "tryComputeTaaJitterNdcOffset matches layout Y");
    expectTrue(!fuse::renderer::tryComputeTaaJitterNdcOffset(3u, 0u, 128u, 8u, ndcOut, rejectReason),
               "tryComputeTaaJitterNdcOffset rejects zero width");
    expectTrue(jitter.tryCurrentNdcOffset(128u, 128u, ndcOut, rejectReason),
               "TaaJitter::tryCurrentNdcOffset succeeds for valid viewport");
    expectNear(ndcOut.x, directNdc.x, 1e-6f, "tryCurrentNdcOffset matches currentNdcOffset X");
    expectTrue(!jitter.tryCurrentNdcOffset(0u, 128u, ndcOut, rejectReason),
               "TaaJitter::tryCurrentNdcOffset rejects zero width");
               "tryPreflightTaaResolve fails for empty history");
               "tryPreflightTaaResolve fails for invalid dimensions");
    expectTrue(!pass->preflightHistoryWarmup(), "pass warmup preflight fails before init");
    expectTrue(pass->preflightJitterNdc(), "pass jitter NDC preflight passes before init");
    expectTrue(!zeroPass->preflightJitterNdc(), "zero-width pass jitter NDC preflight fails");

// --- deepen additive from deepen-b59-taa-guards-e107 ---
                               fuse::renderer::TaaHistoryWarmupRejectReason::NeedsWarmup),
    expectTrue(!fuse::renderer::tryPreflightTaaHistoryWarmup(emptyHistory, warmupReason),
    expectTrue(!fuse::renderer::tryPreflightTaaHistoryWarmup(history, warmupReason),
               "tryPreflightTaaHistoryWarmup fails before warmup");
    expectTrue(warmupReason == fuse::renderer::TaaHistoryWarmupRejectReason::NeedsWarmup,
    expectTrue(fuse::renderer::tryPreflightTaaHistoryWarmup(history, warmupReason),
    expectTrue(warmupReason == fuse::renderer::TaaHistoryWarmupRejectReason::None,
void testJitterNdcShouldSkipAndTryGuards() {
               "tryPreflightTaaJitterNdc fails for zero width");
    expectTrue(TaaJitterLayout::tryNdcOffsetForFrameIndex(3u, 128u, 128u, 8u, ndcOut, rejectReason),
               "tryNdcOffsetForFrameIndex passes for valid inputs");
    expectNear(ndcOut.x, expected.x, 1e-6f, "tryNdcOffsetForFrameIndex X matches ndcOffsetForFrameIndex");
    expectNear(ndcOut.y, expected.y, 1e-6f, "tryNdcOffsetForFrameIndex Y matches ndcOffsetForFrameIndex");
    expectTrue(!TaaJitterLayout::tryNdcOffsetForFrameIndex(3u, 0u, 128u, 8u, ndcOut, rejectReason),
               "tryNdcOffsetForFrameIndex fails for zero width");
    expectTrue(jitter.tryCurrentNdcOffsetIfReady(128u, 128u, ndcOut, rejectReason),
               "tryCurrentNdcOffsetIfReady passes for valid viewport");
    expectNear(ndcOut.x, direct.x, 1e-6f, "tryCurrentNdcOffsetIfReady X matches currentNdcOffset");
    expectTrue(!jitter.tryCurrentNdcOffsetIfReady(0u, 128u, ndcOut, rejectReason),
               "tryCurrentNdcOffsetIfReady fails for zero width");
    expectTrue(jitter.trySyncToFrameIndexIfReady(11u, rejectReason),
               "trySyncToFrameIndexIfReady passes for valid sequence");
    expectTrue(jitter.isAlignedToFrameIndex(11u), "jitter aligned after trySyncToFrameIndexIfReady");
void testResolveBlendWeightsIfReadyGuards() {
    expectTrue(!pass->preflightHistoryWarmup(), "pass preflightHistoryWarmup fails before first resolve");
    expectTrue(pass->preflightJitterNdc(), "pass preflightJitterNdc passes for valid viewport");
    expectTrue(pass->tryCurrentJitterNdcIfReady(jitterNdc, jitterReject),
               "pass tryCurrentJitterNdcIfReady succeeds for valid viewport");
               "pass tryCurrentJitterNdcIfReady reject reason is None");
    expectTrue(pass->trySyncJitterToFrameIndexIfReady(7u, jitterReject),
void testTaaPassWarmupAndJitterPreflightWrappers() {
    expectTrue(!pass->preflightHistoryWarmup(&warmupReason), "pass warmup preflight fails before init");
    expectTrue(!pass->preflightHistoryWarmup(&warmupReason), "pass warmup preflight fails after init");
    expectTrue(!zeroPass->preflightJitterNdc(&jitterReject), "zero-width pass preflightJitterNdc fails");
    testTaaPassWarmupAndJitterPreflightWrappers();

// --- deepen additive from deepen-taa-b59-guards-ea2b ---
void testHistoryIsWarmedGuard() {
void testTemporalBlendPreflight() {
    expectTrue(!fuse::renderer::preflightTaaTemporalBlend(desc, history, &reuseReason, &blendReason),
    expectTrue(fuse::renderer::preflightTaaTemporalBlend(desc, history, &reuseReason, &blendReason),
void testTaaPassTemporalAndJitterPreflights() {
    expectTrue(!pass->preflightTemporalBlend(resolveDesc, &reuseReason, &blendReason),
    expectTrue(pass->preflightTemporalBlend(resolveDesc, &reuseReason, &blendReason),
    testTemporalBlendPreflight();
    testTaaPassTemporalAndJitterPreflights();

// --- deepen additive from deepen-b59-taa-guards-eb8c ---
               "tryPreflight sync reject reason is None");
               "tryPreflight NDC reject reason is None");
               "tryPreflight NDC reject reason is InvalidViewport");
void testHistoryWarmupPreflights() {
void testResolveReuseAndBlendPreflights() {
    expectTrue(std::strcmp(fuse::renderer::taaResolveReuseBlendRejectReasonLabel(
                               fuse::renderer::TaaResolveReuseBlendRejectReason::None),
                               fuse::renderer::TaaResolveReuseBlendRejectReason::HistoryReuseBlocked),
                               fuse::renderer::TaaResolveReuseBlendRejectReason::BlendWeightsRejected),
    fuse::renderer::TaaResolveReuseBlendRejectReason rejectReason =
        fuse::renderer::TaaResolveReuseBlendRejectReason::None;
    expectTrue(fuse::renderer::classifyTaaResolveReuseBlendReject(desc, emptyHistory, 0u) ==
                   fuse::renderer::TaaResolveReuseBlendRejectReason::HistoryReuseBlocked,
    expectTrue(fuse::renderer::tryPreflightTaaResolveReuseAndBlend(desc, history, 0u, rejectReason),
               "tryPreflightTaaResolveReuseAndBlend passes after warmup");
    expectTrue(rejectReason == fuse::renderer::TaaResolveReuseBlendRejectReason::None,
               "tryPreflight reuse-blend reject reason is None");
    expectTrue(fuse::renderer::classifyTaaResolveReuseBlendReject(desc, history, 0u) ==
void testTaaPassWarmupAndJitterPreflights() {
    fuse::renderer::TaaResolveReuseBlendRejectReason reuseBlendReason =
    expectTrue(pass->preflightResolveReuseAndBlend(resolveDesc, 0u, &reuseBlendReason),
    testHistoryWarmupPreflights();
    testResolveReuseAndBlendPreflights();
    testTaaPassWarmupAndJitterPreflights();

// --- deepen additive from deepen-b59-taa-guards-6172 ---
void testTemporalResolveGuardBundle() {
    expectTrue(std::strcmp(fuse::renderer::taaTemporalGuardRejectReasonLabel(
                               fuse::renderer::TaaTemporalGuardRejectReason::None),
                               fuse::renderer::TaaTemporalGuardRejectReason::HistoryReuseBlocked),
                               fuse::renderer::TaaTemporalGuardRejectReason::BlendWeightsRejected),
    expectTrue(fuse::renderer::classifyTaaTemporalGuardReject(desc, emptyHistory) ==
                   fuse::renderer::TaaTemporalGuardRejectReason::HistoryReuseBlocked,
    fuse::renderer::TaaTemporalGuardRejectReason temporalReason =
        fuse::renderer::TaaTemporalGuardRejectReason::None;
    expectTrue(!fuse::renderer::preflightTaaTemporalResolve(desc, history, &temporalReason),
    expectTrue(temporalReason == fuse::renderer::TaaTemporalGuardRejectReason::HistoryReuseBlocked,
    expectTrue(!fuse::renderer::tryPreflightTaaTemporalResolve(desc, history, temporalReason),
               "tryPreflightTaaTemporalResolve fails for unwarmed history");
    expectTrue(fuse::renderer::preflightTaaTemporalResolve(desc, history, &temporalReason),
    expectTrue(temporalReason == fuse::renderer::TaaTemporalGuardRejectReason::None,
void testTaaPassTemporalAndJitterSkipGuards() {
    expectTrue(pass->preflightJitterNdc(&jitterReject), "pass preflightJitterNdc passes before init");
               "pass preflightJitterNdc reject reason is None");
    expectTrue(!pass->preflightTemporalResolve(resolveDesc, &temporalReason),
    expectTrue(pass->preflightTemporalResolve(resolveDesc, &temporalReason),

// --- deepen additive from deepen-b59-taa-guards-2077 ---
void testHistoryWarmupPreflightFollowUp() {
    const fuse::renderer::TaaHistoryWarmupPreflight emptyPreflight =
        fuse::renderer::preflightTaaHistoryWarmup(emptyHistory);
    expectTrue(!emptyPreflight.history_ready, "empty history warmup preflight not ready");
    expectTrue(emptyPreflight.needs_warmup, "empty history warmup preflight needs warmup");
    expectTrue(emptyPreflight.warmup_frames_remaining == 1u,
    expectTrue(!emptyPreflight.isWarmed(), "empty history warmup preflight not warmed");
    fuse::renderer::TaaHistoryWarmupPreflight unwarmedPreflight =
    expectTrue(unwarmedPreflight.history_ready, "allocated history warmup preflight is ready");
    expectTrue(unwarmedPreflight.needs_warmup, "allocated history warmup preflight needs warmup");
    fuse::renderer::TaaHistoryWarmupPreflight tryOut{};
    expectTrue(!fuse::renderer::tryPreflightTaaHistoryWarmup(history, tryOut),
               "tryPreflight warmup fails before first resolve");
    expectTrue(tryOut.needs_warmup, "tryPreflight warmup output marks needs warmup");
    const fuse::renderer::TaaHistoryWarmupPreflight warmedPreflight =
    expectTrue(warmedPreflight.isWarmed(), "warmed history warmup preflight is warmed");
    expectTrue(warmedPreflight.canReuseHistory(), "warmed history warmup preflight can reuse");
    expectTrue(fuse::renderer::tryPreflightTaaHistoryWarmup(history, tryOut),
               "tryPreflight warmup passes after first resolve");
    expectTrue(tryOut.isWarmed(), "tryPreflight warmup output marks warmed");
void testJitterShouldSkipAndAdvancePreflights() {
    expectTrue(fuse::renderer::preflightTaaJitterAdvance(8u), "valid sequence allows advance preflight");
    expectTrue(fuse::renderer::tryPreflightTaaJitterNdc(192u, 108u, 8u, rejectReason),
    expectTrue(!fuse::renderer::tryPreflightTaaJitterNdc(0u, 108u, 8u, rejectReason),
void testResolveBlendPreflightFollowUp() {
    expectTrue(warmupPreflight.can_apply, "warmup resolve blend preflight can apply");
    expectTrue(warmupPreflight.reject_reason == fuse::renderer::TaaResolveBlendRejectReason::None,
    expectNear(warmupPreflight.weights.current, 1.f, 1e-5f, "warmup resolve blend preflight current is full");
    expectTrue(!warmupPreflight.appliesHistoryBlend(),
    expectTrue(steadyPreflight.can_apply, "steady resolve blend preflight can apply");
    expectNear(steadyPreflight.weights.current, 0.4f, 1e-5f, "steady resolve blend preflight current weight");
    expectTrue(steadyPreflight.appliesHistoryBlend(),
void testTaaPassWarmupAndJitterPreflightFollowUp() {
    const fuse::renderer::TaaHistoryWarmupPreflight preInitWarmup = pass->preflightHistoryWarmup();
    const fuse::renderer::TaaResolveBlendPreflight preResolveBlend = pass->preflightResolveBlend(resolveDesc);
    const fuse::renderer::TaaHistoryWarmupPreflight postWarmup = pass->preflightHistoryWarmup();
    const fuse::renderer::TaaResolveBlendPreflight postResolveBlend = pass->preflightResolveBlend(resolveDesc);
    testHistoryWarmupPreflightFollowUp();
    testJitterShouldSkipAndAdvancePreflights();
    testResolveBlendPreflightFollowUp();
    testTaaPassWarmupAndJitterPreflightFollowUp();

// --- deepen additive from deepen-b59-taa-guards-3066 ---
        std::fprintf(stderr, "SKIP: bootstrap allocated for pass wouldSkip test (Vulkan device unavailable)\n");
    expectTrue(!fuse::renderer::tryPreflightTaaHistoryWarmup(emptyHistory, state),
               "tryPreflight warm-up fails for empty history");
               "tryPreflight warm-up state is NotReady for empty history");
    expectTrue(fuse::renderer::preflightTaaHistoryWarmup(history, &state) == false,
    expectTrue(fuse::renderer::tryPreflightTaaHistoryWarmup(history, state),
               "tryPreflight warm-up passes after first resolve");
               "tryPreflight warm-up state is Complete after first resolve");
void testJitterShouldSkipAndPixelOffsetGuards() {
    expectTrue(fuse::renderer::classifyTaaResolveTemporalBlendReject(desc, emptyHistory) ==
    fuse::renderer::TaaResolveTemporalRejectReason rejectReason =
    expectTrue(!fuse::renderer::preflightTaaResolveTemporalBlend(desc, history, &rejectReason),
    expectTrue(rejectReason == fuse::renderer::TaaResolveTemporalRejectReason::HistoryReuseBlocked,
    expectTrue(fuse::renderer::tryPreflightTaaResolveTemporalBlend(desc, history, rejectReason),
    expectTrue(rejectReason == fuse::renderer::TaaResolveTemporalRejectReason::None,
void testTaaPassTemporalAndJitterPreflightWrappers() {
    expectTrue(!pass->preflightHistoryWarmup(), "pass warm-up preflight fails before init");
    fuse::renderer::TaaResolveTemporalRejectReason temporalReject =
    expectTrue(!pass->preflightResolveTemporalBlend(resolveDesc, &temporalReject),
    expectTrue(temporalReject == fuse::renderer::TaaResolveTemporalRejectReason::HistoryReuseBlocked,
    expectTrue(pass->preflightResolveTemporalBlend(resolveDesc, &temporalReject),
    testTaaPassTemporalAndJitterPreflightWrappers();

// --- deepen additive from deepen-taa-b59-guards-9bd6 ---
               "tryPreflightTaaHistoryWarmup passes for warmed history");
               "tryPreflightTaaHistoryWarmup reason is None after warmup");
void testResolveFrameCompositePreflight() {
    expectTrue(fuse::renderer::tryPreflightTaaResolveFrame(desc, history, skipReason, blendReason),
               "tryPreflightTaaResolveFrame passes for valid unwarmed resolve");
void testTaaPassWarmupAndCompositeGuards() {
    expectTrue(pass->tryPreflightJitterNdc(jitterReject), "pass tryPreflightJitterNdc before init");
    expectTrue(pass->preflightJitterNdc(), "pass preflightJitterNdc before init");
    expectTrue(pass->tryPreflightJitterSync(0u, jitterReject), "pass tryPreflightJitterSync passes");
    expectTrue(pass->preflightResolveFrame(resolveDesc), "pass preflightResolveFrame passes");
    testResolveFrameCompositePreflight();

// --- deepen additive from deepen-b59-taa-guards-61ca ---
    expectTrue(TaaJitterLayout::tryComputeNdcOffsetForFrameIndex(3u, 128u, 128u, 8u, ndcOut, rejectReason),
               "tryComputeNdcOffsetForFrameIndex passes for valid inputs");
    expectNear(ndcOut.x, expected.x, 1e-6f, "tryComputeNdcOffsetForFrameIndex matches ndcOffsetForFrameIndex X");
    expectNear(ndcOut.y, expected.y, 1e-6f, "tryComputeNdcOffsetForFrameIndex matches ndcOffsetForFrameIndex Y");
    expectTrue(!TaaJitterLayout::tryComputeNdcOffsetForFrameIndex(3u, 0u, 128u, 8u, ndcOut, rejectReason),
               "tryComputeNdcOffsetForFrameIndex rejects zero width");
               "tryComputeNdcOffsetForFrameIndex reject reason is InvalidViewport");
    expectNear(ndcOut.x, directNdc.x, 1e-6f, "tryCurrentNdcOffsetIfReady matches currentNdcOffset X");
               "tryCurrentNdcOffsetIfReady rejects zero width");
    expectTrue(pass->tryPreflightJitterNdc(jitterReject), "pass tryPreflightJitterNdc passes before init");
               "pass tryPreflightHistoryWarmup fails before init");
               "pass tryPreflightHistoryWarmup reason is NotReady before init");
               "pass tryPreflightHistoryWarmup passes after resolve");

// --- deepen additive from deepen-b59-taa-guards-7381 ---
void testJitterShouldSkipAndTryNdcPreflight() {
void testHistoryIsWarmGuard() {
void testTemporalResolveGuardPreflights() {
    expectTrue(!fuse::renderer::preflightTaaTemporalResolveGuards(desc, history, 0u, &reuseReason, &blendReason),
    expectTrue(fuse::renderer::preflightTaaTemporalResolveGuards(desc, history, 0u, &reuseReason, &blendReason),
void testTaaPassTemporalAndJitterGuardWrappers() {
    expectTrue(pass->preflightJitterSync(3u, &jitterReject), "pass preflightJitterSync before init");
    expectTrue(pass->preflightJitterNdc(128u, 128u, &jitterReject), "pass preflightJitterNdc succeeds");
    expectTrue(pass->preflightJitterNdcIfReady(&jitterReject), "pass preflightJitterNdcIfReady succeeds");
               "pass tryPreflightHistoryReuse reason is NotWarm");
    expectTrue(pass->preflightTemporalResolveGuards(resolveDesc, 0u, &reuseReason, &blendReason),
               "pass preflightTemporalResolveGuards passes after warmup");
    testJitterShouldSkipAndTryNdcPreflight();
    testTemporalResolveGuardPreflights();

// --- deepen additive from deepen-b59-taa-guards-f66d ---
               "tryPreflight warmup reason is None after warmup");
               "tryPreflightTaaResolve passes for valid desc");
               "tryPreflightTaaResolveFrame passes for warmup");
               "tryPreflightTaaResolveFrame skip reason is None");
               "tryPreflightTaaResolveFrame blend reason is None");
    expectTrue(!fuse::renderer::tryPreflightTaaResolveFrame(desc, history, skipReason, blendReason),
               "tryPreflightTaaResolveFrame fails for invalid dimensions");
               "tryPreflightTaaResolveFrame skip reason is InvalidDimensions");
void testTaaPassWarmupAndResolveFrameGuards() {
    expectTrue(pass->preflightJitterNdc(), "pass jitter NDC preflight passes");
    expectTrue(pass->preflightJitterSync(4u), "pass jitter sync preflight passes");

// --- deepen additive from deepen-b59-taa-guards-facc ---
               "tryPreflightHistoryReuse fails before warmup");
               "tryPreflightHistoryReuse reason is NotWarm before warmup");
               "tryPreflightResolveBlendWeights passes before first resolve");
               "tryPreflightResolveBlendWeights reason is None before first resolve");
               "tryPreflightHistoryReuse passes after warmup");

// --- deepen additive from b59-taa-deepen-guards-602b ---
void testJitterFramePreflightAndShouldSkip() {
    const fuse::renderer::TaaJitterFramePreflight framePreflight =
        fuse::renderer::preflightTaaJitterFrame(13u, 1920u, 1080u, 8u);
    expectTrue(framePreflight.canSync(), "frame preflight allows sync for valid sequence");
    expectTrue(framePreflight.canProduceNdc(), "frame preflight allows NDC for valid viewport");
    expectTrue(framePreflight.passes(), "frame preflight passes for valid inputs");
    expectTrue(framePreflight.slot == TaaJitterLayout::frameIndexInSequence(13u, 8u),
    const fuse::renderer::TaaJitterFramePreflight invalidPreflight =
        fuse::renderer::preflightTaaJitterFrame(0u, 0u, 1080u, 8u);
    expectTrue(invalidPreflight.canSync(), "frame preflight sync still valid with invalid viewport");
    expectTrue(!invalidPreflight.canProduceNdc(), "frame preflight blocks NDC for zero width");
    expectTrue(!invalidPreflight.passes(), "frame preflight fails when NDC is blocked");
    expectTrue(invalidPreflight.ndcReject == fuse::renderer::TaaJitterGuardRejectReason::InvalidViewport,
        fuse::renderer::preflightTaaHistoryWarmup(emptyHistory, 0u);
    expectTrue(!emptyPreflight.isWarmupComplete(), "empty history preflight warmup incomplete");
    expectTrue(!emptyPreflight.canReuse(), "empty history preflight cannot reuse");
    expectTrue(!emptyPreflight.passes(), "empty history warmup preflight fails");
    expectTrue(emptyPreflight.reuseBlock == fuse::renderer::TaaHistoryReuseBlockReason::NotReady,
    const fuse::renderer::TaaHistoryWarmupPreflight unwarmedPreflight =
        fuse::renderer::preflightTaaHistoryWarmup(history, 0u);
    expectTrue(unwarmedPreflight.framesRemaining == 1u, "unwarmed preflight has one frame remaining");
    expectTrue(!unwarmedPreflight.isWarmupComplete(), "unwarmed preflight warmup incomplete");
    expectTrue(unwarmedPreflight.reuseBlock == fuse::renderer::TaaHistoryReuseBlockReason::NotWarm,
    expectTrue(warmedPreflight.isWarmupComplete(), "warmed preflight warmup complete");
    expectTrue(warmedPreflight.canReuse(), "warmed preflight can reuse");
    expectTrue(warmedPreflight.passes(), "warmed preflight passes");
    const fuse::renderer::TaaHistoryWarmupPreflight staleWarmPreflight =
        fuse::renderer::preflightTaaHistoryWarmup(history, 999u);
    expectTrue(staleWarmPreflight.isWarmupComplete(), "stale observed gen preflight warmup still complete");
    expectTrue(!staleWarmPreflight.canReuse(), "stale observed gen preflight cannot reuse");
    expectTrue(staleWarmPreflight.reuseBlock == fuse::renderer::TaaHistoryReuseBlockReason::StaleGeneration,
    const fuse::renderer::TaaHistoryWarmupPreflight postInvalidatePreflight =
        fuse::renderer::preflightTaaHistoryWarmup(history, history.invalidateGeneration());
    expectTrue(!postInvalidatePreflight.isWarmupComplete(), "invalidated history preflight warmup incomplete");
    expectTrue(!postInvalidatePreflight.canReuse(), "invalidated history preflight cannot reuse");
    expectTrue(postInvalidatePreflight.reuseBlock == fuse::renderer::TaaHistoryReuseBlockReason::NotWarm,
void testResolveBlendFramePreflightGuards() {
        fuse::renderer::preflightTaaResolveBlendFrame(desc, history);
    expectTrue(warmupPreflight.passes(), "warmup blend frame preflight passes");
    expectTrue(!warmupPreflight.historyBlendAllowed, "warmup blend frame preflight blocks history blend");
    expectTrue(!warmupPreflight.historyDegraded, "warmup blend frame preflight is not degraded");
    expectNear(warmupPreflight.weights.current, 1.f, 1e-5f, "warmup blend frame preflight current is full");
    expectTrue(steadyPreflight.passes(), "steady blend frame preflight passes");
    expectTrue(steadyPreflight.historyBlendAllowed, "steady blend frame preflight allows history blend");
    expectTrue(!steadyPreflight.historyDegraded, "steady blend frame preflight is not degraded");
    expectNear(steadyPreflight.weights.current, 0.4f, 1e-5f, "steady blend frame preflight current weight");
    const fuse::renderer::TaaResolveBlendPreflight stalePreflight =
    expectTrue(stalePreflight.passes(), "stale blend frame preflight still passes weight validation");
    expectTrue(!stalePreflight.historyBlendAllowed, "stale blend frame preflight blocks history blend");
    expectTrue(stalePreflight.historyDegraded, "stale blend frame preflight is degraded");
void testTaaFrameGuardPreflight() {
    const fuse::renderer::TaaJitterFramePreflight jitterPreflight = pass->preflightJitterFrame(9u);
    expectTrue(jitterPreflight.passes(), "pass jitter frame preflight passes before init");
    const fuse::renderer::TaaFrameGuardPreflight preInitGuards =
        pass->preflightFrameGuards(resolveDesc, 0u);
    const fuse::renderer::TaaHistoryWarmupPreflight warmupPreflight = pass->preflightHistoryWarmup(0u);
    expectTrue(!warmupPreflight.passes(), "pass history warmup preflight fails before resolve");
    expectTrue(warmupPreflight.reuseBlock == fuse::renderer::TaaHistoryReuseBlockReason::NotWarm,
        pass->preflightResolveBlendFrame(resolveDesc);
    expectTrue(blendPreflight.passes(), "pass blend frame preflight passes before first resolve");
    expectTrue(!blendPreflight.historyBlendAllowed, "pass blend frame preflight blocks history before warmup");
    const fuse::renderer::TaaFrameGuardPreflight warmedGuards = pass->preflightFrameGuards(resolveDesc, 0u);
    const fuse::renderer::TaaFrameGuardPreflight staleGuards = pass->preflightFrameGuards(resolveDesc, 999u);
    const fuse::renderer::TaaFrameGuardPreflight postInvalidateGuards =
        pass->preflightFrameGuards(resolveDesc, pass->historyInvalidateGeneration());
    testJitterFramePreflightAndShouldSkip();
    testResolveBlendFramePreflightGuards();
    testTaaFrameGuardPreflight();

// --- deepen additive from deepen-b59-taa-guards-804f ---
void testJitterShouldSkipAndTryNdcPreflights() {
               "tryPreflight NDC reject reason is None for valid viewport");
               "tryPreflight NDC reject reason is InvalidViewport for zero width");
    expectTrue(TaaJitterLayout::tryNdcOffsetForFrameIndex(3u, 1920u, 1080u, ndcOut, 8u),
               "tryNdcOffsetForFrameIndex succeeds for valid inputs");
    expectTrue(!TaaJitterLayout::tryNdcOffsetForFrameIndex(0u, 0u, 1080u, ndcOut, 8u),
               "tryNdcOffsetForFrameIndex rejects zero width");
               "tryPreflightTaaHistoryWarmup rejects uninitialized history");
    expectTrue(!fuse::renderer::preflightTaaHistoryWarmup(history),
               "preflightTaaHistoryWarmup rejects unwarmed history");
               "tryPreflightTaaHistoryWarmup rejects unwarmed history");
               "preflightTaaHistoryWarmup passes after warmup");
void testResolveShouldSkipAndTryPreflights() {
               "tryPreflight resolve skip reason is None for valid resolve");
               "tryPreflight resolve skip reason is InvalidDimensions");
void testTaaPassTryAndShouldSkipPreflights() {
    expectTrue(pass->preflightJitterNdc(), "pass NDC preflight passes with valid viewport before init");
    expectTrue(pass->preflightJitterNdc(), "pass preflightJitterNdc passes after init");
    expectTrue(pass->tryPreflightJitterNdc(jitterReject), "pass tryPreflightJitterNdc passes after init");
               "pass tryPreflightHistoryWarmup rejects before resolve");
               "pass tryPreflightResolve passes for valid resolve");
               "pass tryPreflightHistoryReuse rejects before warmup");
    expectTrue(pass->tryPreflightHistoryWarmup(warmupReason), "pass warmup preflight passes after resolve");
    testJitterShouldSkipAndTryNdcPreflights();
    testResolveShouldSkipAndTryPreflights();
    testTaaPassTryAndShouldSkipPreflights();

// --- deepen additive from deepen-b59-taa-guards-48f5 ---
void testHistoryWarmupCompleteGuards() {
    expectTrue(fuse::renderer::tryPreflightTaaResolveTemporal(desc, history, reuseReason, blendReason),
               "tryPreflightTaaResolveTemporal passes after warmup");
    expectTrue(!fuse::renderer::tryPreflightTaaResolveTemporal(desc, history, reuseReason, blendReason),
               "tryPreflightTaaResolveTemporal fails for stale generation");
    expectTrue(!pass->preflightTemporalResolve(resolveDesc),

// --- deepen additive from deepen-taa-b59-guards-d5f5 ---
void testHistoryReuseForResolvePreflight() {
    expectTrue(fuse::renderer::tryPreflightTaaHistoryReuseForResolve(desc, history, reason),
               "tryPreflight passes for warmed history with sentinel generation");
    expectTrue(jitter.trySyncToFrameIndex(9u, rejectReason), "trySyncToFrameIndex succeeds for valid sequence");
    expectTrue(jitter.isAlignedToFrameIndex(9u), "jitter aligned after trySyncToFrameIndex");
               "trySyncToFrameIndex reject reason is None");
void testResolveFrameGuardsPreflight() {
    expectTrue(fuse::renderer::preflightTaaResolveFrameGuards(desc, history, &skipReason, &blendReason),
    expectTrue(fuse::renderer::tryPreflightTaaResolveFrameGuards(desc, history, skipReason, blendReason) == false,
               "tryPreflightTaaResolveFrameGuards fails for invalid dimensions");
               "tryPreflight resolve frame guards skip reason is InvalidDimensions");
void testTaaPassDeepenFrameGuards() {
    expectTrue(pass->preflightJitterNdc(&jitterReject), "pass preflightJitterNdc passes after init");
    expectTrue(pass->preflightResolveFrameGuards(resolveDesc, &skipReason, &blendReason),
    testHistoryReuseForResolvePreflight();
    testResolveFrameGuardsPreflight();

// --- deepen additive from deepen-b59-taa-guards-1a6e ---
void testHistoryWarmupBlockGuards() {
               "tryPreflightTaaJitterAdvance reject reason is InvalidSequence");
    expectNear(weights.history, 0.85f, 1e-5f, "pass tryCompute steady history weight");

// --- deepen additive from deepen-taa-b59-guards-2768 ---
void testHistoryWarmupBlockPreflight() {
void testJitterAlignmentPreflight() {
                               fuse::renderer::TaaJitterGuardRejectReason::Misaligned),
    expectTrue(fuse::renderer::classifyTaaJitterAlignmentReject(jitter, 5u) ==
                   fuse::renderer::TaaJitterGuardRejectReason::Misaligned,
    expectTrue(!fuse::renderer::preflightTaaJitterAlignment(jitter, 5u, &rejectReason),
               "preflightTaaJitterAlignment fails when misaligned");
    expectTrue(rejectReason == fuse::renderer::TaaJitterGuardRejectReason::Misaligned,
    expectTrue(fuse::renderer::tryPreflightTaaJitterAlignment(jitter, 5u, rejectReason),
               "tryPreflightTaaJitterAlignment passes when aligned");
    expectTrue(fuse::renderer::classifyTaaJitterAlignmentReject(fallbackJitter, 0u) ==
void testResolveWithBlendPreflight() {
    expectTrue(std::strcmp(fuse::renderer::taaResolveWithBlendRejectReasonLabel(
                               fuse::renderer::TaaResolveWithBlendRejectReason::ResolveBlocked),
                               fuse::renderer::TaaResolveWithBlendRejectReason::BlendRejected),
    expectTrue(fuse::renderer::classifyTaaResolveWithBlendReject(desc, emptyHistory) ==
                   fuse::renderer::TaaResolveWithBlendRejectReason::ResolveBlocked,
    fuse::renderer::TaaResolveWithBlendRejectReason rejectReason =
        fuse::renderer::TaaResolveWithBlendRejectReason::None;
    expectTrue(!fuse::renderer::tryPreflightTaaResolveWithBlend(desc, emptyHistory, rejectReason),
               "tryPreflightTaaResolveWithBlend fails for empty history");
    expectTrue(rejectReason == fuse::renderer::TaaResolveWithBlendRejectReason::ResolveBlocked,
    expectTrue(fuse::renderer::preflightTaaResolveWithBlend(desc, history, &rejectReason),
    expectTrue(rejectReason == fuse::renderer::TaaResolveWithBlendRejectReason::None,
    expectTrue(fuse::renderer::tryComputeTaaResolveBlendWeightsIfResolveReady(desc, history, weights,
               "tryComputeTaaResolveBlendWeightsIfResolveReady passes for valid resolve");
               "tryComputeTaaResolveBlendWeightsIfResolveReady passes after warmup");
    expectTrue(fuse::renderer::classifyTaaResolveWithBlendReject(desc, history) ==
void testTaaPassWarmupAndCompositePreflightWrappers() {
    expectTrue(pass->preflightJitterAdvance(), "pass preflightJitterAdvance passes with default sequence");
               "pass tryPreflightHistoryWarmup fails before first resolve");
    expectTrue(pass->tryPreflightJitterSync(4u, jitterReason), "pass tryPreflightJitterSync succeeds");
    expectTrue(pass->preflightJitterAlignment(4u, &jitterReason),
               "pass preflightJitterAlignment passes after sync");
    expectTrue(pass->preflightJitterAdvance(&jitterReason), "pass preflightJitterAdvance passes");
    fuse::renderer::TaaResolveWithBlendRejectReason compositeReason =
    expectTrue(pass->preflightResolveWithBlend(resolveDesc, &compositeReason),
    testHistoryWarmupBlockPreflight();
    testJitterAlignmentPreflight();
    testResolveWithBlendPreflight();
    testTaaPassWarmupAndCompositePreflightWrappers();

// --- deepen additive from deepen-b59-taa-guards-2031 ---
void testJitterSlotSyncAdvanceTryGuards() {
    expectTrue(fuse::renderer::classifyTaaJitterSlotReject(3u, 8u) ==
    expectTrue(fuse::renderer::classifyTaaJitterSlotReject(8u, 8u) ==
    expectTrue(fuse::renderer::classifyTaaJitterSlotReject(0u, 0u) ==
    expectTrue(fuse::renderer::preflightTaaJitterSlot(4u, 8u, &rejectReason),
               "preflightTaaJitterSlot passes for in-range slot");
    expectTrue(fuse::renderer::tryPreflightTaaJitterSlot(4u, 8u, rejectReason),
               "tryPreflightTaaJitterSlot passes for in-range slot");
    expectTrue(fuse::renderer::trySyncTaaJitter(jitter, 5u, rejectReason),
               "trySyncTaaJitter succeeds for valid sequence");
    expectTrue(jitter.isAlignedToFrameIndex(5u), "jitter aligned after trySyncTaaJitter");
    expectTrue(fuse::renderer::tryAdvanceTaaJitter(jitter, rejectReason),
               "tryAdvanceTaaJitter succeeds for valid sequence");
    expectTrue(jitter.index() != indexBefore, "tryAdvanceTaaJitter advances jitter");
    expectTrue(!fuse::renderer::preflightTaaResolveFrame(desc, emptyHistory, &skipReason, &blendReject),
    expectTrue(fuse::renderer::tryPreflightTaaResolveFrame(desc, history, skipReason, blendReject),
               "tryPreflightTaaResolveFrame passes for valid warmup resolve");
    expectTrue(!fuse::renderer::preflightTaaResolveFrame(desc, history, &skipReason, &blendReject),
void testTaaPassDeepenTryAndFrameGuards() {
    expectTrue(pass->trySyncJitterToFrameIndex(6u, jitterReject),
               "pass trySyncJitterToFrameIndex succeeds before init");
    expectTrue(pass->jitterAlignedToFrameIndex(6u), "pass jitter aligned after trySync");
    expectTrue(!pass->jitterNeedsResync(6u), "pass jitter does not need resync after trySync");
    expectTrue(pass->tryAdvanceJitterIfReady(jitterReject),
               "pass tryAdvanceJitterIfReady succeeds before init");
    expectTrue(pass->preflightJitterSlot(6u, &jitterReject),
               "pass preflightJitterSlot passes for in-range slot");
    expectTrue(pass->preflightResolveFrame(resolveDesc, &skipReason, &blendReject),
    expectTrue(pass->tryPreflightResolveFrame(resolveDesc, skipReason, blendReject),
               "pass tryPreflightResolveFrame passes before first resolve");
    expectTrue(pass->trySyncJitterToFrameIndex(4u, jitterReject),
               "pass trySyncJitterToFrameIndex succeeds after init");

// --- deepen additive from deepen-b59-taa-guards-985f ---
void testRejectReasonIsBlockingHelpers() {
    expectTrue(!fuse::renderer::taaJitterGuardRejectReasonIsBlocking(
    expectTrue(fuse::renderer::taaJitterGuardRejectReasonIsBlocking(
                   fuse::renderer::TaaJitterGuardRejectReason::MisalignedFrame),
void testJitterAlignmentGuards() {
    expectTrue(fuse::renderer::classifyTaaJitterAlignmentReject(jitter, 0u) ==
    expectTrue(fuse::renderer::preflightTaaJitterAlignment(jitter, 0u),
               "preflightTaaJitterAlignment passes at frame zero");
                   fuse::renderer::TaaJitterGuardRejectReason::MisalignedFrame,
    expectTrue(!fuse::renderer::tryPreflightTaaJitterAlignment(jitter, 0u, rejectReason),
               "tryPreflightTaaJitterAlignment rejects misaligned frame");
    expectTrue(rejectReason == fuse::renderer::TaaJitterGuardRejectReason::MisalignedFrame,
               "tryPreflightTaaJitterAlignment passes after sync");
               "unwarmed history fails tryPreflightTaaHistoryWarmup");
               "warmed history passes tryPreflightTaaHistoryWarmup");
void testTaaPassTryPreflightWrappers() {
    expectTrue(pass->tryPreflightJitterAlignment(0u, jitterReason),
               "pass tryPreflightJitterAlignment passes at frame zero");
    expectTrue(!pass->tryPreflightHistoryWarmup(historyReason),
    expectTrue(!pass->tryPreflightHistoryReuse(0u, historyReason),
    expectTrue(!pass->tryPreflightHistoryReadyForResolve(historyReason),
    expectTrue(pass->tryPreflightHistoryReadyForResolve(historyReason),
               "pass tryPreflightResolveBlendWeights passes before warmup resolve");
    expectTrue(pass->tryPreflightHistoryWarmup(historyReason),
    expectTrue(pass->tryPreflightHistoryReuse(0u, historyReason),
               "pass tryPreflightHistoryReuse passes after resolve");
    expectTrue(!pass->tryPreflightJitterAlignment(0u, jitterReason),
               "pass tryPreflightJitterAlignment rejects after advance");
    expectTrue(jitterReason == fuse::renderer::TaaJitterGuardRejectReason::MisalignedFrame,
    testRejectReasonIsBlockingHelpers();
    testTaaPassTryPreflightWrappers();

// --- deepen additive from deepen-taa-b59-guards-ec2a ---
                               fuse::renderer::TaaHistoryWarmupRejectReason::Incomplete),
                   fuse::renderer::TaaHistoryWarmupRejectReason::Incomplete,
    expectTrue(!fuse::renderer::tryPreflightTaaHistoryWarmupComplete(history, reason),
               "tryPreflight warmup-complete fails for unwarmed history");
    expectTrue(reason == fuse::renderer::TaaHistoryWarmupRejectReason::Incomplete,
               "tryPreflight warmup-complete reason is Incomplete");
    expectTrue(fuse::renderer::preflightTaaHistoryWarmupComplete(history, &reason),
    expectTrue(std::strcmp(fuse::renderer::taaJitterAlignmentRejectReasonLabel(
                               fuse::renderer::TaaJitterAlignmentRejectReason::SyncBlocked),
                               fuse::renderer::TaaJitterAlignmentRejectReason::Misaligned),
    expectTrue(fuse::renderer::classifyTaaJitterAlignmentReject(5u, jitter) ==
                   fuse::renderer::TaaJitterAlignmentRejectReason::Misaligned,
    expectTrue(fuse::renderer::preflightTaaJitterAligned(5u, jitter),
    fuse::renderer::TaaJitterAlignmentRejectReason alignReason =
        fuse::renderer::TaaJitterAlignmentRejectReason::None;
    expectTrue(fuse::renderer::tryPreflightTaaJitterAligned(5u, jitter, alignReason),
               "tryPreflight jitter alignment passes after sync");
    expectTrue(alignReason == fuse::renderer::TaaJitterAlignmentRejectReason::None,
               "tryPreflight jitter alignment reason is None");
    expectTrue(fuse::renderer::preflightTaaJitterAligned(6u, jitter),
    expectTrue(fuse::renderer::classifyTaaJitterAlignmentReject(0u, fallbackJitter) ==
                   fuse::renderer::TaaJitterAlignmentRejectReason::None,
    expectTrue(fuse::renderer::classifyTaaResolveTemporalReject(desc, emptyHistory, 0u) ==
    expectTrue(!fuse::renderer::tryPreflightTaaResolveTemporal(desc, history, 0u, temporalReason),
    expectTrue(fuse::renderer::preflightTaaResolveTemporal(desc, history, 0u, &temporalReason),
    expectTrue(!pass->preflightJitterAligned(5u), "pass jitter misaligned before sync to frame 5");
    expectTrue(pass->preflightJitterAdvance(), "pass preflightJitterAdvance passes after init");
    expectTrue(pass->preflightJitterAligned(4u), "pass jitter aligned after sync");
    expectTrue(pass->tryComputeExpectedResolveBlendWeights(resolveDesc, weights, blendReason),
               "pass tryComputeExpectedResolveBlendWeights passes before warmup");

// --- deepen additive from deepen-taa-b59-guards-4701 ---
void testHistoryWarmupClassifyAndTryPreflight() {
    expectTrue(jitter.trySyncToFrameIndexIfReady(4u, rejectReason),
               "trySyncToFrameIndexIfReady succeeds for valid sequence");
               "trySync reject reason is None");
    expectTrue(jitter.isAlignedToFrameIndex(4u), "jitter aligned after trySync");
    expectTrue(jitter.tryAdvanceIfReady(rejectReason), "tryAdvanceIfReady succeeds for valid sequence");
               "tryAdvance reject reason is None");
    expectTrue(jitter.index() != indexBefore, "tryAdvanceIfReady advances jitter");
               "tryCurrentNdcOffsetIfReady succeeds for valid viewport");
               "tryCurrentNdc reject reason is None");
               "tryCurrentNdc reject reason is InvalidViewport");
    expectTrue(!fuse::renderer::preflightTaaResolveFrame(desc, emptyHistory, &skipReason, &blendReason),
               "tryPreflightTaaResolveFrame passes for valid desc");
               "tryPreflightTaaResolveFrame passes after warmup");
               "tryPreflightTaaResolveFrame fails with invalid dimensions");
    expectTrue(pass->tryPreflightJitterSync(6u, jitterReject),
               "pass tryPreflightHistoryWarmup reason is NeedsWarmup before resolve");
    expectTrue(pass->preflightResolveFrame(resolveDesc), "pass preflightResolveFrame passes before warmup");
               "pass tryPreflightResolve passes before warmup");
    expectTrue(pass->tryPreflightResolveFrame(resolveDesc, skipReason, blendReason),
               "pass tryPreflightResolveFrame passes before warmup");
    testHistoryWarmupClassifyAndTryPreflight();

// --- deepen additive from deepen-taa-b59-guards-4c9c ---
void testJitterAlignmentPreflightGuards() {
    expectTrue(fuse::renderer::preflightTaaJitterAlignment(jitter, 5u, &rejectReason),
               "preflightTaaJitterAlignment passes after sync");
    expectTrue(!fuse::renderer::preflightTaaJitterAlignment(jitter, 6u, &rejectReason),
               "preflightTaaJitterAlignment rejects misaligned frame");
    expectTrue(fuse::renderer::preflightTaaJitterAlignment(jitter, 6u, &rejectReason),
               "preflightTaaJitterAlignment passes after advance to frame six");
void testHistoryWarmupSatisfiedPreflight() {
    expectTrue(!fuse::renderer::tryPreflightTaaHistoryWarmupSatisfied(emptyHistory, reason),
    expectTrue(!fuse::renderer::preflightTaaHistoryWarmupSatisfied(history, &reason),
    expectTrue(fuse::renderer::preflightTaaHistoryWarmupSatisfied(history, &reason),
    expectTrue(!fuse::renderer::tryPreflightTaaResolveTemporalBlend(desc, history, reuseReason, blendReason),
    expectTrue(fuse::renderer::tryPreflightTaaResolveTemporalBlend(desc, history, reuseReason, blendReason),
    expectTrue(!fuse::renderer::preflightTaaResolveTemporalBlend(desc, history, &reuseReason, &blendReason),
    expectTrue(pass->preflightJitterAlignment(4u, &jitterReject),
    expectTrue(pass->tryPreflightJitterAlignment(4u, jitterReject),
               "pass tryPreflightJitterAlignment passes after sync");
    expectTrue(!pass->tryPreflightHistoryWarmupSatisfied(warmupReason),
    expectTrue(pass->tryPreflightHistoryWarmupSatisfied(warmupReason),
    expectTrue(pass->tryPreflightResolveTemporalBlend(resolveDesc, reuseReason, blendReason),
               "pass tryPreflightResolveTemporalBlend passes after warmup");
    testJitterAlignmentPreflightGuards();
    testHistoryWarmupSatisfiedPreflight();

// --- deepen additive from deepen-b59-taa-guards-de0e ---
    expectTrue(fuse::renderer::classifyTaaJitterAlignmentReject(5u, 5u, 5u, 8u) ==
    expectTrue(fuse::renderer::classifyTaaJitterAlignmentReject(5u, 4u, 5u, 8u) ==
    expectTrue(fuse::renderer::classifyTaaJitterAlignmentReject(5u, 5u, 4u, 8u) ==
    expectTrue(fuse::renderer::classifyTaaJitterAlignmentReject(5u, 5u, 5u, 0u) ==
    expectTrue(fuse::renderer::preflightTaaJitterAlignment(13u, 5u, 13u, 8u, &rejectReason),
    expectTrue(fuse::renderer::tryPreflightTaaJitterAlignment(13u, 5u, 13u, 8u, rejectReason),
               "tryPreflightTaaJitterAlignment passes for aligned state");
    expectTrue(!fuse::renderer::preflightTaaJitterAlignment(13u, 4u, 13u, 8u, &rejectReason),
    expectTrue(pass->preflightJitterAlignment(9u, &rejectReason),
    expectTrue(!fuse::renderer::tryPreflightTaaHistoryWarmupComplete(emptyHistory, warmupState),
               "tryPreflightTaaHistoryWarmupComplete fails for empty history");
    expectTrue(!fuse::renderer::tryPreflightTaaHistoryForTemporalBlend(history, 0u, reuseReason),
               "tryPreflightTaaHistoryForTemporalBlend fails before warmup");
    expectTrue(fuse::renderer::preflightTaaHistoryWarmupComplete(history, &warmupState),
    expectTrue(fuse::renderer::tryPreflightTaaHistoryForTemporalBlend(history, 0u, reuseReason),
               "tryPreflightTaaHistoryForTemporalBlend passes after warmup");
    expectTrue(pass->preflightHistoryWarmupComplete(), "pass warmup-complete preflight passes after resolve");
void testResolvePipelineBlendPreflights() {
    expectTrue(!fuse::renderer::tryPreflightTaaResolveWithBlendWeights(desc, emptyHistory, skipReason, blendReason),
               "tryPreflightTaaResolveWithBlendWeights fails when history not ready");
    expectTrue(fuse::renderer::preflightTaaResolveWithBlendWeights(desc, history, &skipReason, &blendReason),
    expectTrue(fuse::renderer::tryPreflightTaaResolvePipeline(desc, history, skipReason, blendReason),
               "tryPreflightTaaResolvePipeline passes for valid desc");
    expectTrue(pass->preflightResolveWithBlendWeights(desc, &skipReason, &blendReason),
    testResolvePipelineBlendPreflights();

// --- deepen additive from deepen-b59-taa-guards-2589 ---
void testJitterSyncAlignmentGuards() {
    expectTrue(fuse::renderer::classifyTaaJitterSyncAlignmentReject(5u, 5u, 8u) ==
    expectTrue(fuse::renderer::classifyTaaJitterSyncAlignmentReject(13u, 5u, 8u) ==
    expectTrue(fuse::renderer::classifyTaaJitterSyncAlignmentReject(5u, 6u, 8u) ==
                   fuse::renderer::TaaJitterGuardRejectReason::MisalignedSlot,
    expectTrue(fuse::renderer::classifyTaaJitterSyncAlignmentReject(0u, 0u, 0u) ==
    expectTrue(fuse::renderer::preflightTaaJitterSyncAlignment(5u, 5u, 8u, &rejectReason),
               "preflightTaaJitterSyncAlignment passes for matching slot");
    expectTrue(fuse::renderer::tryPreflightTaaJitterSyncAlignment(5u, 5u, 8u, rejectReason),
               "tryPreflightTaaJitterSyncAlignment passes for matching slot");
    expectTrue(!fuse::renderer::preflightTaaJitterSyncAlignment(5u, 6u, 8u, &rejectReason),
               "preflightTaaJitterSyncAlignment rejects mismatched slot");
    expectTrue(rejectReason == fuse::renderer::TaaJitterGuardRejectReason::MisalignedSlot,
                               fuse::renderer::TaaJitterGuardRejectReason::MisalignedSlot),
    expectTrue(pass->preflightJitterSyncAlignment(7u, &rejectReason),
               "pass preflightJitterSyncAlignment passes after sync");
    expectTrue(pass->tryPreflightJitterSync(7u, rejectReason), "pass tryPreflightJitterSync passes");
    expectTrue(pass->tryPreflightJitterAdvance(rejectReason), "pass tryPreflightJitterAdvance passes");
               "empty history warmup tryPreflight reason is NotReady");
    expectTrue(!pass->tryPreflightHistoryWarmup(reason), "pass tryPreflightHistoryWarmup fails before resolve");
               "pass warmup tryPreflight reason is NotWarm before resolve");
    expectTrue(pass->tryPreflightHistoryWarmup(reason), "pass tryPreflightHistoryWarmup passes after resolve");
void testResolveBlendPolicyAndCombinedPreflight() {
    expectTrue(fuse::renderer::preflightTaaResolveBlendPolicy(desc, history, &blendReason),
    expectTrue(fuse::renderer::tryPreflightTaaResolveBlendPolicy(desc, history, blendReason),
               "tryPreflightTaaResolveBlendPolicy passes for warmup");
    expectTrue(fuse::renderer::tryPreflightTaaResolveCombined(desc, history, skipReason, blendReason),
               "tryPreflightTaaResolveCombined passes for valid warmup resolve");
    expectTrue(fuse::renderer::preflightTaaResolveBlendPolicy(desc, history),
    expectTrue(pass->tryPreflightResolveBlendPolicy(desc, blendReason),
               "pass tryPreflightResolveBlendPolicy passes before resolve");
    expectTrue(pass->tryPreflightResolveCombined(desc, skipReason, blendReason),
               "pass tryPreflightResolveCombined passes before resolve");
    expectTrue(pass->tryPreflightResolveBlendWeights(desc, blendReason),
               "pass tryPreflightResolveBlendWeights passes before resolve");
    testResolveBlendPolicyAndCombinedPreflight();

// --- deepen additive from deepen-taa-b59-guards-61db ---
void testJitterAlignmentAndSyncNdcGuards() {
    expectTrue(fuse::renderer::classifyTaaJitterAlignmentReject(5u, 5u, 6u, 8u) ==
    expectTrue(fuse::renderer::tryPreflightTaaJitterAlignment(13u, 13u, 5u, 8u, rejectReason),
               "wrapped frame alignment passes tryPreflight");
    expectTrue(fuse::renderer::classifyTaaJitterSyncAndNdcReject(128u, 128u, 8u) ==
    expectTrue(fuse::renderer::preflightTaaJitterSyncAndNdc(5u, 128u, 128u, 8u, &rejectReason),
               "preflightTaaJitterSyncAndNdc passes for valid viewport");
    expectTrue(fuse::renderer::tryPreflightTaaJitterSyncAndNdc(5u, 128u, 128u, 8u, rejectReason),
               "tryPreflightTaaJitterSyncAndNdc passes for valid viewport");
    expectTrue(!fuse::renderer::preflightTaaJitterSyncAndNdc(5u, 0u, 128u, 8u, &rejectReason),
               "preflightTaaJitterSyncAndNdc rejects zero width");
    expectTrue(!jitter.preflightAlignmentToFrameIndex(3u), "default jitter fails alignment preflight");
    fuse::renderer::TaaJitterGuardRejectReason syncReason =
    expectTrue(jitter.trySyncToFrameIndexIfReady(3u, syncReason), "trySyncToFrameIndexIfReady succeeds");
    expectTrue(syncReason == fuse::renderer::TaaJitterGuardRejectReason::None,
               "trySyncToFrameIndexIfReady reject reason is None");
    expectTrue(jitter.preflightAlignmentToFrameIndex(3u), "synced jitter passes alignment preflight");
void testHistoryWarmupAndTemporalSampleGuards() {
    expectTrue(!fuse::renderer::preflightTaaHistoryTemporalSample(history, 0u, &reuseReason),
    expectTrue(fuse::renderer::tryPreflightTaaHistoryTemporalSample(history, 0u, reuseReason),
               "tryPreflightTaaHistoryTemporalSample passes after warmup");
void testResolveTemporalBlendGuards() {
    expectTrue(std::strcmp(fuse::renderer::taaResolveTemporalBlendRejectReasonLabel(
                               fuse::renderer::TaaResolveTemporalBlendRejectReason::HistoryReuseBlocked),
    fuse::renderer::TaaResolveTemporalBlendRejectReason temporalReason =
        fuse::renderer::TaaResolveTemporalBlendRejectReason::None;
    expectTrue(fuse::renderer::preflightTaaResolveTemporalBlend(desc, emptyHistory, &temporalReason),
    expectTrue(temporalReason == fuse::renderer::TaaResolveTemporalBlendRejectReason::None,
    expectTrue(fuse::renderer::tryPreflightTaaResolveTemporalBlend(desc, history, temporalReason),
    expectTrue(fuse::renderer::classifyTaaResolveTemporalBlendReject(desc, history) ==
                   fuse::renderer::TaaResolveTemporalBlendRejectReason::None,
void testTaaPassDeepenFollowUpGuards() {
    expectTrue(pass->preflightJitterSyncAndNdc(9u, &jitterReason), "pass sync+NDC preflight before init");
    expectTrue(pass->preflightJitterAlignment(9u, &jitterReason), "pass alignment passes after sync+NDC");
    expectTrue(pass->preflightResolveTemporalBlend(resolveDesc),
    expectTrue(pass->trySyncJitterToFrameIndexIfReady(11u, jitterReason),
    expectTrue(pass->jitterAlignedToFrameIndex(11u), "pass jitter aligned after trySync");

// --- deepen additive from deepen-b59-taa-guards-258a ---
               "tryPreflight warmup reason is NotWarm for unwarmed history");
void testJitterTryIfReadyWithRejectReason() {
               "tryCurrentNdcOffsetIfReady reject reason is None");
               "tryCurrentNdcOffsetIfReady reject reason is InvalidViewport");
    expectTrue(jitter.isAlignedToFrameIndex(4u), "jitter aligned after trySyncToFrameIndexIfReady");
    expectTrue(TaaJitterLayout::tryOffsetForFrameIndexIfReady(2u, 8u, pixelOut, rejectReason),
               "tryOffsetForFrameIndexIfReady succeeds for valid sequence");
    expectNear(pixelOut.x, directPixel.x, 1e-6f, "tryOffsetForFrameIndexIfReady matches offsetForFrameIndex X");
    expectTrue(!TaaJitterLayout::tryOffsetForFrameIndexIfReady(2u, 0u, pixelOut, rejectReason),
               "tryOffsetForFrameIndexIfReady rejects invalid sequence");
               "tryOffsetForFrameIndexIfReady reject reason is InvalidSequence");
    expectTrue(TaaJitterLayout::tryNdcOffsetForFrameIndexIfReady(2u, 128u, 128u, 8u, ndcOut, rejectReason),
               "tryNdcOffsetForFrameIndexIfReady succeeds for valid viewport");
    expectNear(ndcOut.x, directNdc.x, 1e-6f, "tryNdcOffsetForFrameIndexIfReady matches ndcOffsetForFrameIndex X");
    expectTrue(!TaaJitterLayout::tryNdcOffsetForFrameIndexIfReady(2u, 0u, 128u, 8u, ndcOut, rejectReason),
               "tryNdcOffsetForFrameIndexIfReady rejects zero width");
               "tryNdcOffsetForFrameIndexIfReady reject reason is InvalidViewport");
void testResolveFramePreflight() {
    expectTrue(!fuse::renderer::tryPreflightTaaResolveFrame(desc, emptyHistory, skipReason, blendReason),
               "tryPreflightTaaResolveFrame fails when history not ready");
    expectTrue(fuse::renderer::preflightTaaResolveFrame(desc, history),
    expectTrue(!pass->tryPreflightHistoryWarmup(reuseReason),
               "tryPreflightHistoryWarmup fails before init");
               "warmup tryPreflight reason is NotReady before init");
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass tryPreflight wrapper test");
    expectTrue(pass->init(resources), "TaaPass initialized for tryPreflight wrapper test");
    expectTrue(!pass->preflightHistoryWarmup(&reuseReason),
               "preflightHistoryWarmup fails before first resolve");
    expectTrue(pass->preflightJitterAdvance(), "preflightJitterAdvance passes after init");
    expectTrue(pass->trySyncJitterToFrameIndexIfReady(6u, jitterReason),
               "trySyncJitterToFrameIndexIfReady succeeds");
    expectTrue(pass->tryAdvanceJitterIfReady(jitterReason), "tryAdvanceJitterIfReady succeeds");
    expectTrue(pass->jitter().index() != jitterIndexBefore, "tryAdvanceJitterIfReady advances jitter");
               "tryPreflightResolve passes before first resolve");
               "preflightResolveFrame passes before first resolve");
    expectTrue(pass->tryPreflightHistoryWarmup(reuseReason), "tryPreflightHistoryWarmup passes after resolve");
    expectTrue(pass->preflightHistoryWarmup(), "preflightHistoryWarmup passes after resolve");
               "tryPreflightHistoryReuse passes with current generation after resolve");
               "tryPreflightHistoryReuse fails with stale generation after invalidate");
               "tryPreflightHistoryReuse reason is StaleGeneration after invalidate");
    expectTrue(!pass->tryPreflightHistoryReuse(pass->historyInvalidateGeneration(), reuseReason),
               "tryPreflightHistoryReuse fails after invalidate even with current generation");
               "tryPreflightHistoryReuse reason is NotWarm after invalidate clears warmth");
    expectTrue(pass->tryPreflightHistoryReuse(pass->historyInvalidateGeneration(), reuseReason),
               "tryPreflightHistoryReuse passes after re-warm with current generation");
               "tryPreflightResolveBlendWeights passes after warmup");
               "tryPreflightResolveFrame passes after warmup");
               "tryPreflightHistoryReadyForResolve passes after init");
    testJitterTryIfReadyWithRejectReason();
    testResolveFramePreflight();

// --- deepen additive from deepen-b59-taa-guards-1db7 ---
    expectTrue(fuse::renderer::tryPreflightTaaJitterAlignment(5u, jitter.monotonicFrameIndex(), jitter.index(), 8u,
               "tryPreflightTaaJitterAlignment passes for synced jitter");
               "tryPreflightTaaJitterAlignment reject reason is None");
               "tryPreflightTaaHistoryWarmup reason is NotReady for empty history");
    expectTrue(fuse::renderer::preflightTaaHistoryReadyForResolve(history, &reason),
    expectTrue(pass->tryPreflightJitterSync(0u, jitterReason), "pass tryPreflightJitterSync passes before init");
    expectTrue(pass->tryPreflightJitterNdc(jitterReason), "pass tryPreflightJitterNdc passes before init");
    expectTrue(pass->tryPreflightJitterAdvance(jitterReason), "pass tryPreflightJitterAdvance passes before init");
    expectTrue(pass->tryPreflightJitterAlignment(4u, jitterReason),
               "pass tryPreflightHistoryWarmup reason is NotWarm before resolve");
    expectTrue(pass->preflightResolve(resolveDesc, &skipReason), "pass preflightResolve passes before warmup resolve");
               "pass tryPreflightResolve passes before warmup resolve");
               "pass tryComputeResolveBlendWeights passes before warmup resolve");
               "pass tryPreflightHistoryReuse fails before warmup resolve");

// --- deepen additive from deepen-b59-taa-guards-0070 ---
void testHistoryWarmupPreflightDeepen() {
    expectTrue(!fuse::renderer::preflightTaaHistoryReadyForResolve(emptyHistory, &reason),
               "tryPreflightTaaHistoryWarmup reason is NotWarm for unwarmed history");
               "tryAdvanceIfReady reject reason is None");
    expectTrue(jitter.trySyncToFrameIndexIfReady(9u, rejectReason),
    expectTrue(jitter.monotonicFrameIndex() == 9u, "trySyncToFrameIndexIfReady sets monotonic counter");
    expectTrue(jitter.isAlignedToFrameIndex(9u), "jitter aligned after trySyncToFrameIndexIfReady");
    expectTrue(!fuse::renderer::preflightTaaJitterAdvance(0u, &rejectReason),
               "preflightTaaJitterAdvance rejects invalid sequence");
               "preflightTaaJitterAdvance reject reason is InvalidSequence");
    expectTrue(!fuse::renderer::tryPreflightTaaJitterSync(3u, 0u, rejectReason),
               "tryPreflightTaaJitterSync rejects invalid sequence");
               "tryPreflightTaaJitterSync reject reason is InvalidSequence");
void testTaaPassTryPreflightDeepen() {
    expectTrue(pass->tryPreflightJitterSync(6u, jitterReject), "pass tryPreflightJitterSync passes before init");
               "pass tryPreflightResolve skip reason is None");
    expectTrue(pass->trySyncJitterToFrameIndexIfReady(11u, jitterReject),
    expectTrue(pass->tryAdvanceJitterIfReady(jitterReject), "pass tryAdvanceJitterIfReady succeeds");
    expectTrue(pass->jitter().index() != jitterIndexBefore, "pass tryAdvanceJitterIfReady advances jitter");
    expectTrue(pass->tryPreflightHistoryWarmup(reuseReason), "pass tryPreflightHistoryWarmup passes after resolve");
               "zero-width pass tryPreflightJitterNdc reject reason is InvalidViewport");
    testHistoryWarmupPreflightDeepen();
    testTaaPassTryPreflightDeepen();

// --- deepen additive from deepen-taa-b59-guards-9b65 ---
void testTaaPassTryPreflightGuards() {
               "pass classifyJitterSyncReject is None for valid sequence");
               "pass classifyJitterAdvanceReject is None for valid sequence");
    expectTrue(pass->tryPreflightJitterSync(6u, jitterReject), "pass tryPreflightJitterSync passes");
    expectTrue(pass->tryPreflightJitterAdvance(jitterReject), "pass tryPreflightJitterAdvance passes");
               "pass tryPreflightResolveBlendWeights reject reason is None before init");
    expectNear(weights.current, 0.4f, 1e-5f, "pass tryCompute steady current weight");
    expectNear(weights.history, 0.6f, 1e-5f, "pass tryCompute steady history weight");
               "zero-width pass tryPreflightJitterNdc rejects");
    testTaaPassTryPreflightGuards();

// --- deepen additive from deepen-taa-pass-guards-9b1a ---
void testTaaPassTryPreflightAndClassifyGuards() {
               "pass classifyJitterSyncReject is None before init");
               "pass classifyJitterNdcReject is None before init");
               "pass classifyJitterAdvanceReject is None before init");
    expectTrue(pass->tryPreflightJitterSync(6u, jitterReason),
               "pass tryPreflightJitterSync reason is None before init");
    expectTrue(pass->preflightJitterAdvance(), "pass preflightJitterAdvance passes before init");
    testTaaPassTryPreflightAndClassifyGuards();

// --- deepen additive from deepen-taa-b59-guards-e11c ---
void testTaaPassTryPreflightAndClassifyWrappers() {
    expectTrue(pass->tryPreflightJitterSync(4u, jitterReason), "pass tryPreflightJitterSync passes before init");
               "pass tryPreflightJitterNdc reject reason is None before init");
               "pass tryPreflightJitterAdvance reject reason is None before init");
    expectTrue(!pass->preflightResolve(resolveDesc, &skipReason),
               "pass preflightResolve fails before init");
    expectNear(weights.current, 0.2f, 1e-5f, "pass tryCompute steady current weight matches blend factor");
    expectNear(weights.history, 0.8f, 1e-5f, "pass tryCompute steady history weight is complement");
    expectTrue(zeroSeqPass->classifyJitterAdvanceReject() == fuse::renderer::TaaJitterGuardRejectReason::None,
               "zero-seq pass classifyJitterAdvanceReject is None after fallback");
    expectTrue(zeroWidthPass->classifyJitterNdcReject() ==
    expectTrue(!zeroWidthPass->tryPreflightJitterNdc(jitterReason),
    testTaaPassTryPreflightAndClassifyWrappers();

// --- deepen additive from deepen-taa-pass-guards-f637 ---
               "pass classifyResolveBlendReject is None before warmup");
               "pass preflightResolve passes before warmup");

// --- deepen additive from deepen-b59-taa-pass-try-classify-18e8 ---
               "pass classifyJitterNdcReject passes before init");
               "pass classifyJitterAdvanceReject passes before init");
    expectTrue(pass->tryPreflightJitterSync(5u, jitterReason), "pass tryPreflightJitterSync passes before init");
               "pass classifyResolveBlendReject returns None before init");
               "pass classifyResolveBlendReject returns None after warmup");

// --- deepen additive from deepen-b59-taa-try-preflights-fb09 ---
               "pass tryPreflightResolveBlendWeights reason is None before init");
    expectNear(weights.current, 1.f, 1e-5f, "pass tryCompute warmup current weight before init");
    expectNear(weights.history, 0.f, 1e-5f, "pass tryCompute warmup history weight before init");
    expectNear(weights.current, 0.35f, 1e-5f, "pass tryCompute steady current weight after warmup");
    expectNear(weights.history, 0.65f, 1e-5f, "pass tryCompute steady history weight after warmup");
    expectTrue(pass->tryPreflightJitterSync(9u, jitterReason),
               "pass tryPreflightJitterSync passes after sync");
               "pass tryPreflightJitterAdvance passes after init");
               "pass tryPreflightJitterAdvance reason is None after init");

// --- deepen additive from deepen-b59-taa-pass-guards-cc65 ---
void testTaaPassTryAndClassifyGuards() {
    expectTrue(pass->tryPreflightJitterSync(6u, jitterReason), "pass tryPreflightJitterSync passes before init");
               "pass classifyResolveBlendReject passes before init");
    expectTrue(pass->tryPreflightJitterSync(9u, jitterReason), "pass tryPreflightJitterSync passes after sync");
    expectTrue(pass->tryPreflightJitterAdvance(jitterReason), "pass tryPreflightJitterAdvance still passes");

// --- deepen additive from deepen-b59-taa-pass-preflights-c1fb ---
void testTaaPassTryAndClassifyPreflights() {
               "pass tryPreflightResolveBlendWeights reason is None before warmup resolve");
               "pass classifyResolveBlendReject is None before warmup resolve");
    expectTrue(pass->tryComputeResolveBlendWeights(resolveDesc, blendWeights, blendReason),
               "pass tryComputeResolveBlendWeights current is 1.0 on warmup frame");
               "pass tryPreflightHistoryReuse reason is NotWarm before warmup resolve");
               "pass tryPreflightHistoryReuse passes after warmup resolve");
               "pass tryPreflightHistoryReuse reason is None after warmup resolve");
               "pass tryComputeResolveBlendWeights passes after warmup resolve");
               "pass tryComputeResolveBlendWeights history matches blend factor");
    expectTrue(invalidSeqPass->tryPreflightJitterSync(0u, jitterReason),
               "pass tryPreflightJitterSync passes with normalized sequence");
               "pass classifyJitterSyncReject is None with normalized sequence");
               "pass tryPreflightJitterNdc rejects zero width");
               "pass tryPreflightJitterNdc reason is InvalidViewport");
               "pass classifyJitterNdcReject is InvalidViewport");
    testTaaPassTryAndClassifyPreflights();

// --- deepen additive from deepen-taa-b59-guards-48c3 ---
void testTaaPassTemporalGuardsAndTryPreflight() {
    expectTrue(!pass->tryPreflightHistoryReuse(0u, historyReject),
    expectTrue(historyReject == fuse::renderer::TaaHistoryReuseBlockReason::NotReady,
    expectTrue(!pass->tryPreflightHistoryReadyForResolve(historyReject),
               "pass tryPreflightResolveBlendWeights passes on warmup frame");
               "pass tryPreflightResolveBlendWeights reject reason is None on warmup");
               "pass tryComputeResolveBlendWeights passes on warmup frame");
    expectTrue(pass->preflightTemporalGuards(5u, resolveDesc, 0u, &verdict),
    expectTrue(pass->tryPreflightHistoryReuse(0u, historyReject),
    expectTrue(historyReject == fuse::renderer::TaaHistoryReuseBlockReason::None,
    expectTrue(pass->tryPreflightHistoryReadyForResolve(historyReject),
               "pass tryPreflightHistoryReadyForResolve passes after warmup");
    expectNear(weights.current, 0.25f, 1e-5f, "pass tryCompute steady current weight after warmup");
    expectNear(weights.history, 0.75f, 1e-5f, "pass tryCompute steady history weight after warmup");
    expectTrue(historyReject == fuse::renderer::TaaHistoryReuseBlockReason::StaleGeneration,
               "zero-width pass tryPreflightJitterNdc rejects invalid viewport");
    testTaaPassTemporalGuardsAndTryPreflight();

// --- deepen additive from deepen-b59-taa-pass-guards-4baf ---
               "pass tryPreflightResolve passes with valid desc after init");
    expectNear(weights.current, 0.35f, 1e-5f, "pass tryCompute steady current weight");
    expectNear(weights.history, 0.65f, 1e-5f, "pass tryCompute steady history weight");

// --- deepen additive from deepen-taa-pass-guards-ab01 ---
        std::printf("SKIP: Vulkan device not available for pass tryPreflight wrapper test\n");
               "pass classifyResolveBlendReject is None after warmup");
    expectTrue(pass->jitterAlignedToFrameIndex(6u), "pass jitter aligned after sync for tryPreflight");
    expectTrue(zeroPass->tryPreflightJitterSync(0u, jitterReason),
               "zero-width pass tryPreflightJitterSync still valid for sequence");

// --- deepen additive from deepen-b59-taa-pass-guards-82e7 ---
               "pass classifyResolveBlendReject passes before first resolve");
               "pass tryPreflightResolveBlendWeights passes before first resolve");
               "pass tryComputeResolveBlendWeights passes before first resolve");
               "pass classifyJitterSyncReject passes after init");
    expectTrue(fallbackPass->classifyJitterSyncReject() == fuse::renderer::TaaJitterGuardRejectReason::None,
               "pass classifyJitterSyncReject passes after invalid desc fallback");
    expectTrue(fallbackPass->tryPreflightJitterSync(2u, jitterReject),
               "pass tryPreflightJitterSync passes after invalid desc fallback");
               "pass classifyJitterNdcReject detects zero width");
               "pass tryPreflightJitterNdc reject reason is InvalidViewport");

// --- deepen additive from deepen-b59-taa-guards-2f25 ---
void testTaaPassTryPreflightAndCompositeGuards() {
               "tryPreflightHistoryReadyForResolve fails before init");
               "tryPreflightHistoryReuse fails before init");
               "tryPreflightJitterSync passes before init");
               "tryPreflightJitterNdc passes before init");
               "tryPreflightJitterAdvance passes before init");
    expectTrue(pass->preflightJitterFrame(4u, &jitterReason),
               "preflightJitterFrame passes before init");
               "tryPreflightHistoryWarmup fails before first resolve");
               "tryPreflightResolve passes with valid desc after init");
               "tryPreflightResolve skip reason is None after init");
               "preflightResolveWithBlend passes before first resolve");
               "tryComputeResolveBlendWeights passes before first resolve");
               "tryPreflightHistoryReuse passes after resolve");
    expectTrue(pass->preflightHistoryTemporal(0u, &reuseReason),
               "preflightHistoryTemporal passes after resolve");
               "tryComputeResolveBlendWeights passes after resolve");
    expectNear(weights.current, 0.25f, 1e-5f, "tryCompute steady current weight");
    expectNear(weights.history, 0.75f, 1e-5f, "tryCompute steady history weight");
    expectTrue(pass->preflightJitterFrame(9u, &jitterReason),
               "preflightJitterFrame passes after sync");
               "tryPreflightHistoryReuse fails after invalidate");
               "tryPreflightJitterNdc fails for zero-width pass");
               "tryPreflightResolve fails with invalid dimensions");
               "tryPreflightResolve skip reason is InvalidDimensions");
    testTaaPassTryPreflightAndCompositeGuards();

// --- deepen additive from deepen-b59-taa-try-classify-guards-505f ---
    expectTrue(pass->tryPreflightJitterSync(4u, jitterReject), "pass tryPreflightJitterSync passes before init");
    expectTrue(pass->tryPreflightJitterAdvance(jitterReject), "pass tryPreflightJitterAdvance passes before init");
    expectTrue(zeroSeqPass->classifyJitterSyncReject() ==
    expectTrue(!zeroWidthPass->tryPreflightJitterNdc(jitterReject),

// --- deepen additive from deepen-b59-taa-guards-4155 ---
               "pass classifyResolveBlendReject is None before first resolve");
               "pass preflightResolve passes after init");
    expectTrue(invalidJitterPass->tryPreflightJitterAdvance(jitterReject),
               "pass tryPreflightJitterAdvance passes after fallback sequence");

// --- deepen additive from deepen-b59-taa-try-preflights-53e7 ---
    expectNear(weights.current, 1.f, 1e-5f, "pass tryComputeResolveBlendWeights current is 1 before warmup");
    expectNear(weights.current, 0.2f, 1e-5f, "pass tryComputeResolveBlendWeights current matches blend after warmup");
               "pass tryPreflightJitterAdvance reject reason is None after init");

// --- deepen additive from deepen-b59-taa-pass-try-preflights-f032 ---
               "pass tryPreflightJitterSync passes for valid sequence");
               "pass classifyJitterSyncReject returns None for valid sequence");
               "pass tryPreflightJitterNdc passes for valid viewport");
               "pass classifyJitterNdcReject returns None for valid viewport");
               "pass tryPreflightJitterAdvance passes for valid sequence");
               "pass classifyResolveBlendReject returns None before warmup");
               "zero-width pass classifyJitterNdcReject returns InvalidViewport");

// --- deepen additive from deepen-b59-taa-pass-try-preflights-d3c4 ---
               "pass tryComputeExpectedResolveBlendWeights passes before warmup resolve");
               "pass tryPreflightResolve skip reason is None before warmup resolve");
               "pass tryPreflightHistoryReuse passes with matching generation after warmup");
               "pass tryComputeExpectedResolveBlendWeights passes after warmup");
    expectTrue(pass->tryPreflightJitterSync(9u, jitterReject),
    expectTrue(!pass->tryPreflightHistoryReuse(resolveDesc.observed_history_generation, reuseReason),
               "pass tryPreflightHistoryReuse fails with current generation after invalidate");
               "pass tryPreflightHistoryReuse reason is NotWarm after invalidate with current generation");

// --- deepen additive from deepen-b59-taa-guards-0a2f ---
               "resolve tryPreflight reason is NotReady before init");
    expectTrue(pass->tryPreflightJitterSync(6u, jitterReason), "tryPreflightJitterSync passes before init");
               "tryPreflightJitterSync reject reason is None before init");
    expectTrue(pass->tryPreflightJitterNdc(jitterReason), "tryPreflightJitterNdc passes before init");
    expectTrue(!pass->preflightResolveFrame(resolveDesc, &skipReason, &blendReason),
               "preflightResolveFrame fails with invalid dimensions");
               "preflightResolveFrame skip reason is InvalidDimensions");

// --- deepen additive from deepen-b59-taa-pass-guards-5ca7 ---
    expectTrue(pass->preflightJitterSyncAndNdc(4u, &jitterReason),
               "pass preflightJitterSyncAndNdc passes before init");
    expectTrue(!pass->preflightHistoryWarmupAndReuse(0u, &reuseReason),
               "pass preflightHistoryWarmupAndReuse fails before warmup");
               "pass preflightHistoryWarmupAndReuse reason is NotWarm before warmup");
    expectTrue(pass->preflightJitterSyncAndNdc(6u, &jitterReason),
               "pass preflightJitterSyncAndNdc passes when aligned");
    expectTrue(pass->preflightHistoryWarmupAndReuse(0u, &reuseReason),
               "pass preflightHistoryWarmupAndReuse passes after warmup");
    expectTrue(!zeroPass->preflightJitterSyncAndNdc(0u, &jitterReason),
               "zero-width pass preflightJitterSyncAndNdc fails");

// --- deepen additive from deepen-b59-taa-guards-aded ---
void testTaaPassHistoryWarmupPreflight() {
    expectTrue(!pass->preflightHistoryWarmup(&reason), "pass warmup preflight fails before init");
    expectTrue(!pass->tryPreflightHistoryReadyForResolve(reason),
    expectTrue(pass->tryPreflightHistoryReadyForResolve(reason),
    expectTrue(!pass->preflightHistoryWarmup(&reason), "pass warmup preflight fails before first resolve");
    expectTrue(pass->preflightHistoryWarmup(&reason), "pass warmup preflight passes after resolve");
    expectTrue(pass->preflightJitterAdvance(&jitterReject), "pass preflightJitterAdvance passes before init");
    expectTrue(pass->preflightResolve(resolveDesc, &skipReason), "pass preflightResolve passes before warmup");
    testTaaPassHistoryWarmupPreflight();

// --- deepen additive from deepen-b59-taa-try-preflights-199a ---
               "pass tryComputeExpectedResolveBlendWeights passes before init");
    expectTrue(invalidSeqPass->tryPreflightJitterAdvance(jitterReject),
               "fallback pass tryPreflightJitterAdvance passes after defaulting sequence");
               "fallback pass tryPreflightJitterAdvance reject reason is None");

// --- deepen additive from deepen-b59-taa-try-preflights-0d24 ---
               "pass tryPreflightJitterNdc reason is None before init");
               "pass tryPreflightJitterAdvance reason is None before init");
    expectNear(weights.current, 1.f, 1e-5f, "pass tryCompute warmup current weight is full before init");
    expectNear(weights.history, 0.f, 1e-5f, "pass tryCompute warmup history weight is zero before init");
    expectTrue(pass->tryPreflightJitterAdvance(jitterReason), "pass tryPreflightJitterAdvance passes after init");

// --- deepen additive from deepen-fuse-b59-taa-beec ---
    expectTrue(zeroPass->tryPreflightJitterSync(0u, jitterReject),
               "pass tryPreflightJitterAdvance passes after fallback sequence length");

// --- deepen additive from deepen-b59-taa-try-preflights-c552 ---
    expectTrue(pass->tryPreflightJitterSync(3u, jitterReject), "pass tryPreflightJitterSync passes before init");

// --- deepen additive from deepen-b59-taa-guards-58fa ---
    expectTrue(pass->preflightJitterAdvance(), "pass preflightJitterAdvance passes for valid sequence");
    expectTrue(!pass->preflightHistoryReadyForResolve(&reuseReason),
               "pass preflightHistoryReadyForResolve fails before init");
               "pass preflightHistoryReadyForResolve reason is NotReady before init");
    expectTrue(pass->tryComputeExpectedResolveBlendWeights(resolveDesc, blendWeights, blendReason),
               "pass tryComputeExpectedResolveBlendWeights warmup current weight is 1");
    expectTrue(invalidPass->classifyJitterSyncReject() ==
               "pass classifyJitterSyncReject uses fallback sequence length");
    expectTrue(invalidPass->tryPreflightJitterSync(2u, jitterReject),
               "pass tryPreflightJitterSync succeeds after invalid desc fallback");

// --- deepen additive from deepen-b59-taa-pass-guards-3123 ---
               "pass tryPreflightResolveBlendWeights reason is None before warmup");

// --- deepen additive from deepen-b59-taa-pass-guards-5bd0 ---
    expectTrue(pass->tryPreflightJitterFrame(4u, jitterReject), "pass tryPreflightJitterFrame passes");
    expectTrue(pass->preflightJitterFrame(4u), "pass preflightJitterFrame passes");
    expectTrue(!pass->tryPreflightHistoryTemporal(0u, reuseReason),
               "pass tryPreflightHistoryTemporal fails before warmup");
    expectTrue(pass->preflightResolveTemporal(resolveDesc, &blendReason, &reuseReason),
               "pass preflightResolveTemporal passes before warmup (no history blend)");
    expectTrue(pass->tryPreflightHistoryTemporal(0u, reuseReason),
               "pass tryPreflightHistoryTemporal passes after warmup");
               "pass preflightResolveTemporal passes after warmup");
    expectNear(weights.current, 0.2f, 1e-5f, "pass tryCompute steady current weight");
    expectNear(weights.history, 0.8f, 1e-5f, "pass tryCompute steady history weight");
               "pass tryPreflightHistoryTemporal fails after invalidate");
               "pass tryPreflightHistoryTemporal reason is StaleGeneration after invalidate");

// --- deepen additive from deepen-b59-taa-pass-guards-59bd ---
void testTaaPassTryPreflightGuardWrappers() {
    expectTrue(pass->classifyJitterSyncReject(4u) == fuse::renderer::TaaJitterGuardRejectReason::None,
    expectTrue(pass->jitterAlignedToFrameIndex(6u), "pass jitter aligned after trySyncJitterToFrameIndex");
    expectTrue(pass->tryAdvanceJitter(jitterReject), "pass tryAdvanceJitter succeeds before init");
    expectTrue(pass->jitter().index() != jitterIndexBefore, "pass tryAdvanceJitter advances jitter");
               "pass tryPreflightResolve skip reason is None before warmup");
    testTaaPassTryPreflightGuardWrappers();

// --- deepen additive from deepen-taa-b59-guards-5600 ---
void testTaaPassTryAndClassifyGuardWrappers() {
               "pass tryPreflightHistoryReuse passes after warmup with current generation");

// --- deepen additive from deepen-b59-taa-guards-a216 ---
    expectTrue(pass->tryPreflightHistoryReuse(resolveDesc.observed_history_generation, reuseReason),

// --- deepen additive from deepen-taa-pass-guards-86f6 ---
               "pass classifyJitterSyncReject passes for default sequence");
               "pass classifyJitterNdcReject passes for valid viewport");
               "pass classifyJitterAdvanceReject passes for default sequence");

// --- deepen additive from deepen-taa-pass-guards-6509 ---
void testTaaPassTryClassifyGuardWrappers() {
               "pass tryPreflightResolve passes before first resolve");
               "pass tryPreflightResolve skip reason is None before first resolve");

// --- deepen additive from deepen-taa-pass-guards-8135 ---
               "pass tryPreflightResolve passes for valid resolve desc");
               "pass tryPreflightResolve fails for zero width");

// --- deepen additive from deepen-b59-taa-pass-wrappers-7eb0 ---
    expectTrue(pass->trySyncJitterToFrameIndexIfReady(4u, jitterReason),
               "pass trySyncJitterToFrameIndexIfReady succeeds before init");
    expectTrue(pass->tryAdvanceJitterIfReady(jitterReason), "pass tryAdvanceJitterIfReady succeeds before init");
    expectTrue(zeroPass->classifyJitterNdcReject() == fuse::renderer::TaaJitterGuardRejectReason::InvalidViewport,
    expectTrue(zeroPass->trySyncJitterToFrameIndexIfReady(0u, jitterReason),
               "zero-width pass trySync still valid for sequence");
               "zero-width pass trySync reject reason is None for valid sequence");
    expectTrue(invalidSeqPass->classifyJitterSyncReject() == fuse::renderer::TaaJitterGuardRejectReason::None,
               "fallback sequence pass classifyJitterSyncReject is None");
    expectTrue(invalidSeqPass->tryPreflightJitterAdvance(jitterReason),
               "fallback sequence pass tryPreflightJitterAdvance passes");
    expectTrue(invalidSeqPass->tryAdvanceJitterIfReady(jitterReason),
               "fallback sequence pass tryAdvanceJitterIfReady passes");
    expectTrue(invalidSeqPass->trySyncJitterToFrameIndexIfReady(0u, jitterReason),
               "fallback sequence pass trySyncJitterToFrameIndexIfReady passes");

// --- deepen additive from deepen-taa-pass-guards-8544 ---
               "pass classifyResolveBlendReject passes after warmup");

// --- deepen additive from deepen-taa-pass-guards-55b4 ---
    expectTrue(pass->tryPreflightResolve(resolveDesc, skipReason), "pass tryPreflightResolve passes after init");

// --- deepen additive from deepen-b59-taa-pass-try-classify-e2ca ---
               "pass classifyResolveBlendReject passes before warmup resolve");
               "pass tryPreflightResolve passes for valid desc after init");
               "pass preflightResolve passes for valid desc after init");
               "pass tryPreflightResolveBlendWeights reject reason is None");
               "zero-width pass classifyJitterSyncReject still valid for sequence");

// --- deepen additive from b59-taa-pass-try-classify-guards-b93d ---
    expectTrue(pass->tryPreflightJitterSync(5u, jitterReject), "pass tryPreflightJitterSync passes before init");
               "zero-width pass classifyJitterNdcReject reports InvalidViewport");
                   fuse::renderer::classifyTaaJitterNdcReject(passDesc.width, passDesc.height,
               "pass classifyJitterNdcReject matches free helper");
                   fuse::renderer::classifyTaaJitterAdvanceReject(pass->jitter().sequenceLength()),
               "pass classifyJitterAdvanceReject matches free helper");
                   fuse::renderer::classifyTaaResolveBlendReject(resolveDesc, pass->history()),
               "pass classifyResolveBlendReject matches free helper");
               "pass tryPreflightJitterSync succeeds after init");
    expectTrue(fuse::renderer::tryPreflightTaaJitterSync(3u, pass->jitter().sequenceLength(), jitterReject),
               "free tryPreflightJitterSync matches pass sequence length");

// --- deepen additive from deepen-taa-pass-guards-4405 ---
    expectNear(weights.current, 0.3f, 1e-5f, "pass tryCompute steady current weight after warmup");
    expectNear(weights.history, 0.7f, 1e-5f, "pass tryCompute steady history weight after warmup");

// --- deepen additive from deepen-taa-pass-guards-b6a9 ---
    expectTrue(fallbackSeqPass->classifyJitterSyncReject() ==
               "fallback pass classifyJitterSyncReject passes after normalization");
    expectTrue(fallbackSeqPass->tryPreflightJitterAdvance(jitterReject),
               "fallback pass tryPreflightJitterAdvance passes after normalization");

// --- deepen additive from deepen-taa-pass-guards-35c6 ---
    expectTrue(pass->tryPreflightJitterSync(11u, jitterReject),

// --- deepen additive from deepen-b59-taa-pass-try-classify-e4ae ---
    fuse::renderer::TaaJitterGuardRejectReason jitterReason = fuse::renderer::TaaJitterGuardRejectReason::None;
