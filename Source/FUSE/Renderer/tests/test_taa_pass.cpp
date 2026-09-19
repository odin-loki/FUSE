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

void testHistoryWarmupCompositeGuards() {
    fuse::renderer::TaaHistoryBuffer emptyHistory;
    expectTrue(!fuse::renderer::taaHistoryWarmupComplete(emptyHistory),
               "empty history warmup is not complete");
    expectNear(fuse::renderer::taaHistoryWarmupProgress(emptyHistory), 0.f, 1e-5f,
               "empty history warmup progress is zero");
    expectTrue(!fuse::renderer::taaHistoryReuseReady(emptyHistory, 0u),
               "empty history is not reuse-ready");

    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for warmup composite guard test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaHistoryBuffer history;
    fuse::renderer::TaaHistoryBufferDesc historyDesc{64, 64};
    expectTrue(history.init(resources, historyDesc), "history ready for warmup composite guard test");
    expectTrue(!fuse::renderer::taaHistoryWarmupComplete(history),
               "allocated but unwarmed history is not warmup-complete");
    expectNear(fuse::renderer::taaHistoryWarmupProgress(history), 0.f, 1e-5f,
               "unwarmed history warmup progress is zero");
    expectTrue(!fuse::renderer::taaHistoryReuseReady(history, 0u),
               "unwarmed history is not reuse-ready");

    history.markResolved();
    expectTrue(fuse::renderer::taaHistoryWarmupComplete(history),
               "warmed history warmup is complete");
    expectNear(fuse::renderer::taaHistoryWarmupProgress(history), 1.f, 1e-5f,
               "warmed history warmup progress is one");
    expectTrue(fuse::renderer::taaHistoryReuseReady(history, 0u),
               "warmed history with matching generation is reuse-ready");

    history.invalidateHistory();
    expectTrue(!fuse::renderer::taaHistoryReuseReady(history, 0u),
               "invalidated history is not reuse-ready with stale generation");
    expectTrue(!fuse::renderer::taaHistoryReuseReady(history, history.invalidateGeneration()),
               "invalidated history is not reuse-ready until re-warmed");
    history.markResolved();
    expectTrue(fuse::renderer::taaHistoryReuseReady(history, history.invalidateGeneration()),
               "re-warmed history is reuse-ready with current generation");

    history.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testJitterSyncBlockGuards() {
    using fuse::renderer::TaaJitterLayout;

    expectTrue(std::strcmp(fuse::renderer::taaJitterSyncBlockReasonLabel(
                               fuse::renderer::TaaJitterSyncBlockReason::None),
                           "none") == 0,
               "None jitter sync block label");
    expectTrue(std::strcmp(fuse::renderer::taaJitterSyncBlockReasonLabel(
                               fuse::renderer::TaaJitterSyncBlockReason::InvalidSequence),
                           "invalid_sequence") == 0,
               "InvalidSequence jitter sync block label");
    expectTrue(std::strcmp(fuse::renderer::taaJitterSyncBlockReasonLabel(
                               fuse::renderer::TaaJitterSyncBlockReason::InvalidViewport),
                           "invalid_viewport") == 0,
               "InvalidViewport jitter sync block label");

    expectTrue(fuse::renderer::classifyTaaJitterSyncBlock(1920u, 1080u, 8u) ==
                   fuse::renderer::TaaJitterSyncBlockReason::None,
               "valid viewport and sequence are not blocked");
    expectTrue(fuse::renderer::classifyTaaJitterSyncBlock(1920u, 1080u, 0u) ==
                   fuse::renderer::TaaJitterSyncBlockReason::InvalidSequence,
               "zero sequence classified as InvalidSequence");
    expectTrue(fuse::renderer::classifyTaaJitterSyncBlock(0u, 1080u, 8u) ==
                   fuse::renderer::TaaJitterSyncBlockReason::InvalidViewport,
               "zero width classified as InvalidViewport");

    fuse::renderer::TaaJitterSyncBlockReason reason = fuse::renderer::TaaJitterSyncBlockReason::None;
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

    fuse::renderer::TaaJitter jitter;
    expectTrue(jitter.classifySyncBlock(128u, 128u) == fuse::renderer::TaaJitterSyncBlockReason::None,
               "default jitter sync block is None for valid viewport");
    expectTrue(jitter.preflightSync(5u, 128u, 128u, &reason),
               "default jitter preflightSync passes for valid viewport");
    expectTrue(!jitter.preflightSync(5u, 0u, 128u, &reason),
               "default jitter preflightSync fails for zero width");
}

void testPreflightTaaResolveWithBlend() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for resolve-with-blend preflight test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaHistoryBuffer history;
    fuse::renderer::TaaHistoryBufferDesc historyDesc{64, 64};
    expectTrue(history.init(resources, historyDesc), "history ready for resolve-with-blend preflight test");

    fuse::renderer::TaaResolveDesc desc{};
    desc.width = 64;
    desc.height = 64;
    desc.surfaces.current_frame = reinterpret_cast<void*>(0x1);
    desc.surfaces.output = reinterpret_cast<void*>(0x2);
    desc.params.blend_factor = 0.2f;

    fuse::renderer::TaaResolveSkipReason skipReason = fuse::renderer::TaaResolveSkipReason::None;
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
    expectTrue(blendReason == fuse::renderer::TaaResolveBlendRejectReason::None,
               "invalid dimensions resolve-with-blend blend reason is None");

    history.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testResolveStatsBlendConsistent() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for stats blend consistency test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    fuse::renderer::TaaHistoryBuffer history;
    fuse::renderer::TaaHistoryBufferDesc historyDesc{64, 64};
    expectTrue(history.init(resources, historyDesc), "history ready for stats blend consistency test");

    fuse::renderer::TaaResolve resolve;
    fuse::renderer::TaaResolveDesc desc{};
    desc.width = 64;
    desc.height = 64;
    desc.surfaces.current_frame = reinterpret_cast<void*>(0x1);
    desc.surfaces.output = reinterpret_cast<void*>(0x2);
    desc.params.blend_factor = 0.25f;

    expectTrue(resolve.resolve(desc, history), "first resolve succeeds");
    expectTrue(fuse::renderer::taaResolveStatsBlendConsistent(resolve.lastStats(), desc, history),
               "first resolve stats blend is consistent");

    expectTrue(resolve.resolve(desc, history), "second resolve succeeds");
    expectTrue(fuse::renderer::taaResolveStatsBlendConsistent(resolve.lastStats(), desc, history),
               "second resolve stats blend is consistent");

    fuse::renderer::TaaResolveStats inconsistent{};
    inconsistent.resolved = true;
    inconsistent.first_frame = false;
    inconsistent.effective_blend = 0.6f;
    inconsistent.history_blend = 0.6f;
    expectTrue(!fuse::renderer::taaResolveStatsBlendConsistent(inconsistent, desc, history),
               "invalid stats blend weights fail consistency check");

    history.destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testTaaPassCompositeGuards() {
    fuse::renderer::TaaPassDesc passDesc{};
    passDesc.width = 128;
    passDesc.height = 128;
    passDesc.params.blend_factor = 0.3f;

    auto pass = fuse::renderer::TaaPass::create(passDesc);
    expectNear(pass->historyWarmupProgress(), 0.f, 1e-5f,
               "pass warmup progress is zero before init");
    expectTrue(!pass->historyWarmupComplete(), "pass warmup not complete before init");
    expectTrue(pass->classifyJitterSyncBlock() == fuse::renderer::TaaJitterSyncBlockReason::None,
               "pass jitter sync block is None before init");

    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for pass composite guard test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);
    expectTrue(pass->init(resources), "TaaPass initialized for composite guard test");
    expectTrue(!pass->historyWarmupComplete(), "pass warmup not complete before first resolve");
    expectTrue(!pass->historyReuseReady(0u), "pass history not reuse-ready before first resolve");

    fuse::renderer::TaaJitterSyncBlockReason syncReason = fuse::renderer::TaaJitterSyncBlockReason::None;
    expectTrue(pass->preflightJitterSync(4u, &syncReason), "pass jitter sync preflight passes");
    expectTrue(syncReason == fuse::renderer::TaaJitterSyncBlockReason::None,
               "pass jitter sync preflight reason is None");

    fuse::renderer::TaaResolveDesc resolveDesc{};
    resolveDesc.width = 128;
    resolveDesc.height = 128;
    resolveDesc.surfaces.current_frame = reinterpret_cast<void*>(0x10);
    resolveDesc.surfaces.output = reinterpret_cast<void*>(0x20);
    resolveDesc.params = passDesc.params;

    fuse::renderer::TaaResolveSkipReason skipReason = fuse::renderer::TaaResolveSkipReason::None;
    fuse::renderer::TaaResolveBlendRejectReason blendReason =
        fuse::renderer::TaaResolveBlendRejectReason::None;
    expectTrue(pass->preflightResolveWithBlend(resolveDesc, &skipReason, &blendReason),
               "pass resolve-with-blend preflight passes before first resolve");

    expectTrue(pass->resolveFrame(resolveDesc), "initial resolve warms pass history");
    expectTrue(pass->historyWarmupComplete(), "pass warmup complete after first resolve");
    expectNear(pass->historyWarmupProgress(), 1.f, 1e-5f, "pass warmup progress is one after first resolve");
    expectTrue(pass->historyReuseReady(0u), "pass history reuse-ready after first resolve");
    expectTrue(pass->lastResolveStatsBlendConsistent(resolveDesc),
               "pass last resolve stats blend is consistent");

    expectTrue(pass->resolveFrame(resolveDesc), "second resolve succeeds");
    expectTrue(pass->lastResolveStatsBlendConsistent(resolveDesc),
               "pass second resolve stats blend is consistent");

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
    testTaaPassReuseAndJitterGuards();
    testHistoryWarmupCompositeGuards();
    testJitterSyncBlockGuards();
    testPreflightTaaResolveWithBlend();
    testResolveStatsBlendConsistent();
    testTaaPassCompositeGuards();

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_taa_pass: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_taa_pass: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
