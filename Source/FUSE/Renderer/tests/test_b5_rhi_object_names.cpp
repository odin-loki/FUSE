// B2 gate row: "All Vulkan objects named via vkSetDebugUtilsObjectNameEXT — visible in RenderDoc".
//
// Headless proof: b5_vk_call_hooks interposes every vkCreate*/vkAllocate* entry point fuse_rhi
// calls plus vkSetDebugUtilsObjectNameEXT (via vkGetDeviceProcAddr). A representative frame —
// RhiContext creation (frame ring, compute pipeline), first frame (raster path, composite GPU path,
// bindless heap, CUDA/Vulkan frame-sync pair) and a steady-state frame — must create zero device
// objects that never receive a debug name. The steady-state frame must create no objects at all.
#include "b5_rhi_test_common.hpp"
#include "b5_vk_call_hooks.hpp"

#include <fuse/core/init.hpp>
#include <fuse/renderer/postprocess/screen_flash.hpp>
#include <fuse/renderer/rhi/object_name.hpp>
#include <fuse/renderer/rhi_context.hpp>

#include <cstdio>
#include <set>
#include <string>

namespace {

using b5rhi::expectTrue;

void runFrame(fuse::renderer::RhiContext& context, fuse::u32 frameIndex) {
    fuse::renderer::RenderCommandList commands;
    commands.clear3D(0.f, 0.f, 0.f);
    commands.drawSprite2D(0.f, 0.f, 0.f, 255, 255, 255);
    expectTrue(context.beginFrame(frameIndex), "beginFrame");
    expectTrue(context.submitFrame(commands, frameIndex), "submitFrame");
}

int reportUnnamed(const char* phase) {
    int unnamed = 0;
    std::set<std::string> distinct;
    for (const b5hooks::ObjectRecord& object : b5hooks::capturedObjects()) {
        if (!object.named) {
            ++unnamed;
            distinct.insert(object.createdBy + " (VkObjectType " + std::to_string(object.type) + ")");
        }
    }
    std::printf("%s: %zu objects created, %d unnamed\n", phase, b5hooks::capturedObjects().size(), unnamed);
    for (const std::string& entry : distinct) {
        std::printf("  unnamed: %s\n", entry.c_str());
    }
    return unnamed;
}

void testRhiObjectNameOk() {
    expectTrue(fuse::renderer::rhi_object_name_ok("Shadow/Atlas"), "Shadow/Atlas is a valid object name");
    expectTrue(!fuse::renderer::rhi_object_name_ok(""), "empty object name is rejected");
    expectTrue(!fuse::renderer::rhi_object_name_ok("has space"), "object name with a space is rejected");

    const std::string maxName(64, 'A');
    const std::string tooLong(65, 'A');
    expectTrue(fuse::renderer::rhi_object_name_ok(maxName), "64-character object name is valid");
    expectTrue(!fuse::renderer::rhi_object_name_ok(tooLong), "65-character object name is rejected");
}

void testScreenFlashOverlay() {
    const fuse::math::Vec3 base{0.2f, 0.4f, 0.8f};
    const fuse::math::Vec3 flashColor{1.f, 0.25f, 0.f};

    fuse::renderer::ScreenFlash full{};
    full.color = flashColor;
    full.intensity = 1.f;
    const fuse::math::Vec3 replaced = fuse::renderer::overlay(base, full);
    expectTrue(replaced.x == flashColor.x && replaced.y == flashColor.y && replaced.z == flashColor.z,
               "intensity 1 replaces the base with the flash color");

    fuse::renderer::ScreenFlash clear{};
    clear.color = flashColor;
    clear.intensity = 0.f;
    const fuse::math::Vec3 unchanged = fuse::renderer::overlay(base, clear);
    expectTrue(unchanged.x == base.x && unchanged.y == base.y && unchanged.z == base.z,
               "intensity 0 leaves the base unchanged");
}

} // namespace

int main() {
    testRhiObjectNameOk();
    testScreenFlashOverlay();
    if (b5rhi::failures() != 0) {
        return b5rhi::finish("fuse_b5_rhi_object_names");
    }
    if (!b5hooks::available()) {
        return b5rhi::skip("fuse_b5_rhi_object_names", "Vulkan backend disabled");
    }
    fuse::core::initialize();

    fuse::renderer::RhiContext::Desc desc{};
    desc.bootstrap.instance.enableValidation = false;
    desc.bootstrap.createSwapchain = false;
    desc.raster.width = 64;
    desc.raster.height = 64;

    b5hooks::beginObjectCapture();
    auto context = fuse::renderer::RhiContext::create(desc);
    if (context == nullptr || !context->bootstrap().status().deviceReady) {
        b5hooks::endObjectCapture();
        context.reset();
        fuse::core::shutdown();
        return b5rhi::skip("fuse_b5_rhi_object_names", "no Vulkan device (needs an ICD, Lavapipe in CI)");
    }
    runFrame(*context, 0u);
    b5hooks::endObjectCapture();

    const std::size_t created = b5hooks::capturedObjects().size();
    const int unnamed = reportUnnamed("context + first frame");
    expectTrue(created >= 20u, "representative frame created the RHI object set (>= 20 objects)");
    expectTrue(b5hooks::namesApplied() >= created, "vkSetDebugUtilsObjectNameEXT reached the loader");
    expectTrue(unnamed == 0, "zero unnamed Vulkan objects after context creation + first frame");

    // Steady state: frames 1..3 cycle every frame slot and must not create anything.
    b5hooks::beginObjectCapture();
    for (fuse::u32 frame = 1; frame <= 3u; ++frame) {
        runFrame(*context, frame);
    }
    b5hooks::endObjectCapture();
    const int steadyUnnamed = reportUnnamed("steady-state frames 1..3");
    expectTrue(steadyUnnamed == 0, "zero unnamed Vulkan objects in steady-state frames");
    expectTrue(b5hooks::capturedObjects().empty(), "steady-state frames create no Vulkan objects");

    context.reset();
    fuse::core::shutdown();
    return b5rhi::finish("fuse_b5_rhi_object_names");
}
