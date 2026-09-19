#pragma once

#include <fuse/types.hpp>

#include <string>

namespace fuse::editor {

/// Headless-safe embed session toward in-process `fuse_runtime` viewport (U6).
struct RuntimeEmbedSession {
    std::string projectRoot;
    std::string loadedWorldPath;
    std::string wsiBackendName;
    u32 worldEntityCount = 0;
    u32 mirroredEditorEntityCount = 0;
    u32 headlessPresentTicks = 0;
    u32 wsiPresentPathTicks = 0;
    u32 presentSkippedNoWsiCount = 0;
    u32 surfaceHandoffCount = 0;
    u32 swapchainWiringAttempts = 0;
    u32 swapchainWiringReady = 0;
    u32 swapchainRecreateAttempts = 0;
    u32 swapchainRecreateCount = 0;
    u32 swapchainPresentAfterRecreateCount = 0;
    u32 consumedSwapchainPresentTicks = 0;
    u32 wireDatablockEntries = 0;
    u32 wireMaterialEntries = 0;
    u32 wireEcsMaterialApplied = 0;
    u32 wireEcsSpawnApplied = 0;
    bool usesExternalSwapchain = false;
    u32 submittedFrames = 0;
    bool worldLoaded = false;
    bool headlessGpuReady = false;
    bool usesHeadlessGpuPath = false;
    bool wsiPresentPathReady = false;
    bool surfaceHandoffPending = false;
    bool surfaceHandoffConsumed = false;
    bool qVulkanWindowWsiProbed = false;
    bool qVulkanWindowWsiReady = false;

    void reset();
};

} // namespace fuse::editor
