// B6.3 / B6.13 gate — "viewport resizes cleanly": ViewportPanel::setDimensions drives
// ViewportFramebuffer, which drains the device and rebuilds the renderer's offscreen colour + depth
// targets and VkFramebuffer (renderer::RasterPath) at the new size. Runs on a real Vulkan device
// (Lavapipe in CI) with VK_LAYER_KHRONOS_validation enabled and requires zero validation errors.
// Without a Vulkan loader / device it reports the headless model only.
#include <fuse/core/init.hpp>
#include <fuse/editor/viewport_framebuffer.hpp>
#include <fuse/editor/viewport_panel.hpp>
#include <fuse/renderer/vk/bootstrap.hpp>
#include <fuse/renderer/vk/instance.hpp>
#include <fuse/renderer/vk/raster_path.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

using fuse::u32;
using fuse::u8;

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

std::string fixturePath(const char* name) {
    return std::string(FUSE_SHADER_FIXTURE_DIR) + "/" + name;
}

bool validationLayerEnabled(const fuse::renderer::VulkanBootstrap& bootstrap) {
    if (bootstrap.instance() == nullptr) {
        return false;
    }
    for (const char* layer : bootstrap.instance()->info().enabledLayers) {
        if (layer != nullptr && std::strcmp(layer, "VK_LAYER_KHRONOS_validation") == 0) {
            return true;
        }
    }
    return false;
}

void testViewportResizeOnDevice() {
    fuse::renderer::resetVulkanValidationCounters();

    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = true;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated");
    fuse::renderer::VulkanDevice* device = bootstrap != nullptr ? bootstrap->device() : nullptr;
    if (device == nullptr || !bootstrap->status().deviceReady) {
        std::printf("fuse_editor_b6_viewport_resize_vk_gates: no Vulkan device - headless model only\n");
        fuse::editor::ViewportPanel panel;
        fuse::editor::ViewportFramebuffer framebuffer;
        panel.setDimensions(640, 360);
        expectTrue(framebuffer.sync(panel) && framebuffer.width() == 640u && !framebuffer.gpuBacked(),
                   "headless framebuffer follows the panel");
        return;
    }
    const bool validation = validationLayerEnabled(*bootstrap);
    std::printf("fuse_editor_b6_viewport_resize_vk_gates: device ready, validation layer %s\n",
                validation ? "ENABLED" : "unavailable");
    expectTrue(validation, "VK_LAYER_KHRONOS_validation is active for the resize gate");

    const std::string vert = fixturePath("minimal.vert.spv");
    const std::string frag = fixturePath("minimal.frag.spv");
    fuse::renderer::RasterPathDesc rasterDesc{};
    rasterDesc.vertexSpirvPath = vert.c_str();
    rasterDesc.fragmentSpirvPath = frag.c_str();
    auto raster = fuse::renderer::RasterPath::create(*device, rasterDesc);
    expectTrue(raster != nullptr && raster->isReady(), "viewport raster path ready");
    if (raster == nullptr || !raster->isReady()) {
        return;
    }

    fuse::editor::ViewportPanel panel;
    fuse::editor::ViewportFramebuffer framebuffer;
    framebuffer.attach(raster.get(), device);
    expectTrue(framebuffer.gpuBacked(), "framebuffer bound to the renderer's raster targets");

    struct Size {
        u32 w;
        u32 h;
    };
    // Grow, shrink, portrait, odd sizes, minimised (1x1), back up, and a repeated size (no-op).
    const Size sizes[] = {{640, 360}, {1280, 720}, {333, 777}, {1, 1}, {1920, 1080}, {800, 600}, {800, 600}};
    u32 expectedRebuilds = 0;
    u32 lastW = 0;
    u32 lastH = 0;
    for (const Size& size : sizes) {
        panel.setDimensions(size.w, size.h);
        const bool changed = size.w != lastW || size.h != lastH;
        expectTrue(framebuffer.sync(panel), "framebuffer rebuild succeeds");
        expectedRebuilds += changed ? 1u : 0u;
        lastW = size.w;
        lastH = size.h;
        expectTrue(!panel.needsResize(), "resize flag consumed");

        const fuse::renderer::VkFrameEncodeContext encode = raster->vulkanEncodeContext();
        expectTrue(encode.width == size.w && encode.height == size.h, "render pass extent == viewport size");
        expectTrue(encode.framebuffer != nullptr && raster->depthImageHandle() != nullptr &&
                       raster->colorViewHandle() != nullptr,
                   "framebuffer, depth and colour targets exist after the rebuild");

        // Touch the new targets on the GPU: readback transitions and copies the colour image.
        std::vector<u8> rgba;
        expectTrue(raster->readbackColor(rgba) && rgba.size() == static_cast<std::size_t>(size.w) * size.h * 4u,
                   "colour target readback has the new size");

        const fuse::renderer::VulkanValidationCounters counters = fuse::renderer::vulkanValidationCounters();
        if (counters.errors != 0u) {
            std::fprintf(stderr, "validation error after resize to %ux%u: %s\n", size.w, size.h,
                         counters.lastError.c_str());
        }
        expectTrue(counters.errors == 0u, "no validation errors after the resize");
    }
    expectTrue(framebuffer.rebuildCount() == expectedRebuilds, "one rebuild per real size change");
    // RasterPath starts at 320x240, so every listed size change (incl. the first) recreates targets.
    expectTrue(raster->lastStats().resizeCount == expectedRebuilds, "GPU targets recreated once per size change");

    framebuffer.detach();
    raster.reset();
    bootstrap.reset(); // device + instance teardown also runs under validation
    const fuse::renderer::VulkanValidationCounters final = fuse::renderer::vulkanValidationCounters();
    std::printf("fuse_editor_b6_viewport_resize_vk_gates: validation errors=%u warnings=%u\n", final.errors,
                final.warnings);
    expectTrue(final.errors == 0u, "zero validation errors across create / resize / teardown");
}

} // namespace

int main() {
    fuse::core::initialize();
    testViewportResizeOnDevice();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_editor_b6_viewport_resize_vk_gates: all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "fuse_editor_b6_viewport_resize_vk_gates: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
