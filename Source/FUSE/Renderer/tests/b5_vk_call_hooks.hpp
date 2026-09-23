// Test-only Vulkan call interposition for the B5 RHI gate rows (fuse_b5_rhi_* tests).
//
// b5_vk_call_hooks.cpp defines selected `vk*` entry points inside the test executable. The
// statically linked fuse_rhi objects resolve to these definitions at link time (the executable's
// own symbols win over libvulkan's), and each hook forwards to the loader via dlsym(RTLD_NEXT).
// This gives the tests a RenderDoc-like view of what the engine actually records/creates:
//   * object creation + vkSetDebugUtilsObjectNameEXT (row: "all Vulkan objects named")
//   * vkCmdBind* / vkCmdPushConstants / vkCmdDraw* counts (row: "no redundant state changes")
// Stub (non-Vulkan) builds compile the hooks out; the API then reports nothing.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace b5hooks {

struct CmdCounters {
    std::uint32_t bindPipelineGraphics = 0;
    std::uint32_t bindPipelineCompute = 0;
    std::uint32_t bindVertexBuffers = 0;
    std::uint32_t bindIndexBuffer = 0;
    std::uint32_t bindDescriptorSets = 0;
    std::uint32_t pushConstants = 0;
    std::uint32_t drawIndexed = 0;
    std::uint32_t draw = 0;
    /// First 32-bit word of every vkCmdPushConstants payload, in record order.
    std::vector<std::uint32_t> pushFirstWords;
    /// Graphics pipelines bound, in record order.
    std::vector<std::uint64_t> graphicsPipelines;
    /// Push-constant first word in effect at each vkCmdDrawIndexed (UINT32_MAX when none).
    std::vector<std::uint32_t> drawIndexedMaterial;
};

struct ObjectRecord {
    std::uint32_t type = 0; // VkObjectType
    std::uint64_t handle = 0;
    std::string name;
    std::string createdBy; // vk entry point
    bool named = false;
};

/// True when the hooks are compiled in (Vulkan backend build).
bool available();

void resetCmdCounters();
const CmdCounters& cmdCounters();

/// Start recording every device-child object created from now on (clears previous capture).
void beginObjectCapture();
/// Stop recording new objects; objects already captured still observe names/destroys.
void endObjectCapture();
/// Objects created during the capture window (destroyed ones included, flagged by name state).
const std::vector<ObjectRecord>& capturedObjects();
/// Names applied through vkSetDebugUtilsObjectNameEXT since process start.
std::uint32_t namesApplied();

} // namespace b5hooks
