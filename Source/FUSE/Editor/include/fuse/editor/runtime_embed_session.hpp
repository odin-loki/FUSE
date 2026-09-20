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
    u32 qtPresentEligibleTicks = 0;
    u32 qtPresentPathReadyTicks = 0;
    u32 qtPresentPathEligibleTicks = 0;
    u32 softwarePlaceholderRetiredTicks = 0;
    u32 realPresentCallCount = 0;
    u32 qtRealPresentCallCount = 0;
    u32 wireDatablockEntries = 0;
    u32 wireMaterialEntries = 0;
    u32 wireEcsMaterialApplied = 0;
    u32 wireEcsSpawnApplied = 0;
    u32 projectVfsMounts = 0;
    u32 materialVfsResolved = 0;
    u32 materialVfsUnresolved = 0;
    u32 materialCookCacheHits = 0;
    u32 materialCookCacheStores = 0;
    u32 materialAsyncLoadsSubmitted = 0;
    u32 materialAsyncLoadsDrained = 0;
    u32 qtLivePresentAttempts = 0;
    u32 qtLivePresentTicks = 0;
    u32 hybridComposerFrames = 0;
    u32 ecsWorld3DObjectCount = 0;
    u32 ecsWorld3DSyncTicks = 0;
    u32 materialTextureCooks = 0;
    u32 vfsAssetPathsRemapped = 0;
    u32 shaderVfsResolved = 0;
    u32 shaderVfsUnresolved = 0;
    u32 shaderCookCacheHits = 0;
    u32 shaderCookCacheStores = 0;
    u32 shaderAsyncLoadsSubmitted = 0;
    u32 shaderAsyncLoadsDrained = 0;
    u32 ecsWorld3DSnapshotVisible = 0;
    u32 cookedMaterialBindings = 0;
    u32 cookedShaderBindings = 0;
    u32 meshPreviewHints = 0;
    u32 sdfPreviewHints = 0;
    u32 meshPreviewDraws = 0;
    u32 sdfPreviewDraws = 0;
    bool qtLivePresentReady = false;
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
    bool qVulkanInstanceReady = false;
    bool qVulkanExtensionsProbed = false;
    bool qVulkanWindowCreated = false;
    bool qVulkanWindowDestroyed = false;
    u32 qVulkanSupportedExtensionCount = 0;
    u32 qVulkanInstanceVersionMajor = 0;
    u32 qVulkanInstanceVersionMinor = 0;

    void reset();
};

} // namespace fuse::editor
