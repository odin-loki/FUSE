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
    TaaJitterLayout::fillHaltonSequence(8u, sequence);
    expectNear(sequence[0].x, 0.5f, 1e-5f, "filled sequence slot 0 X");
    expectNear(sequence[7].x, 0.0625f, 1e-5f, "filled sequence slot 7 X");
    expectNear(sequence[7].y, 0.889f, 1e-3f, "filled sequence slot 7 Y");
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
        jitter.advance();
    }
    expectTrue(jitter.index() == 0u, "default sequence wraps after one period");
    const fuse::math::Vec2 periodEnd = jitter.currentPixelOffset();
    expectNear(periodEnd.x, periodStart.x, 1e-5f, "sequence period returns to first sample");
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
    expectTrue(history.accumulatedFrames() == 0u, "no accumulated frames before resolve");

    history.markResolved();
    expectTrue(history.hasValidHistory(), "history valid after first resolve");
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
    expectTrue(!resolve.resolve(desc, history), "resolve rejects empty history buffer");
    expectTrue(!resolve.lastStats().resolved, "empty history resolve stats not marked resolved");
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

    desc.surfaces.current_frame = reinterpret_cast<void*>(0x3);
    desc.surfaces.output = reinterpret_cast<void*>(0x4);
    expectTrue(resolve.resolve(desc, history), "second resolve succeeds");
    expectTrue(!resolve.lastStats().first_frame, "second resolve is not first frame");
    expectTrue(resolve.lastStats().accumulated_frames == 2u, "second resolve increments accumulated frames");

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

void testTaaPassGraphHook() {
    fuse::renderer::RenderGraph graph;
    fuse::renderer::resetTaaPassGraphStorage();
    fuse::renderer::addTaaPassToGraph(graph);
    graph.compile();
    expectTrue(graph.compileInfo().passCount == 1u, "TAA pass registered on render graph");
    expectTrue(graph.compileInfo().compiled, "TAA render graph compiles");
}

} // namespace

int main() {
    fuse::core::initialize();

    testHaltonJitterSequence();
    testJitterSequenceLayout();
    testJitterSequencePeriod();
    testCustomJitterSequenceLength();
    testHaltonComputeMatchesTable();
    testJitterNdcOffset();
    testHistoryBufferPingPong();
    testHistoryValidityFlags();
    testEmptyHistoryResolve();
    testValidityResetAfterInvalidate();
    testResolveStub();
    testTaaPassLifecycle();
    testTaaPassGraphHook();

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_taa_pass: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_taa_pass: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
