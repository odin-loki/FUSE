#include <fuse/editor/runtime_embed_session.hpp>

namespace fuse::editor {

void RuntimeEmbedSession::reset() {
    projectRoot.clear();
    loadedWorldPath.clear();
    wsiBackendName.clear();
    worldEntityCount = 0;
    mirroredEditorEntityCount = 0;
    headlessPresentTicks = 0;
    wsiPresentPathTicks = 0;
    presentSkippedNoWsiCount = 0;
    surfaceHandoffCount = 0;
    swapchainWiringAttempts = 0;
    swapchainWiringReady = 0;
    swapchainRecreateAttempts = 0;
    swapchainRecreateCount = 0;
    swapchainPresentAfterRecreateCount = 0;
    consumedSwapchainPresentTicks = 0;
    qtPresentEligibleTicks = 0;
    qtPresentPathReadyTicks = 0;
    qtPresentPathEligibleTicks = 0;
    softwarePlaceholderRetiredTicks = 0;
    realPresentCallCount = 0;
    qtRealPresentCallCount = 0;
    wireDatablockEntries = 0;
    wireMaterialEntries = 0;
    wireEcsMaterialApplied = 0;
    wireEcsSpawnApplied = 0;
    projectVfsMounts = 0;
    materialVfsResolved = 0;
    materialVfsUnresolved = 0;
    materialCookCacheHits = 0;
    materialCookCacheStores = 0;
    materialAsyncLoadsSubmitted = 0;
    materialAsyncLoadsDrained = 0;
    qtLivePresentAttempts = 0;
    qtLivePresentTicks = 0;
    hybridComposerFrames = 0;
    ecsWorld3DObjectCount = 0;
    ecsWorld3DSyncTicks = 0;
    materialTextureCooks = 0;
    vfsAssetPathsRemapped = 0;
    shaderVfsResolved = 0;
    shaderVfsUnresolved = 0;
    shaderCookCacheHits = 0;
    shaderCookCacheStores = 0;
    shaderAsyncLoadsSubmitted = 0;
    shaderAsyncLoadsDrained = 0;
    ecsWorld3DSnapshotVisible = 0;
    cookedMaterialBindings = 0;
    cookedShaderBindings = 0;
    meshPreviewHints = 0;
    sdfPreviewHints = 0;
    meshPreviewDraws = 0;
    sdfPreviewDraws = 0;
    qtLivePresentReady = false;
    usesExternalSwapchain = false;
    submittedFrames = 0;
    worldLoaded = false;
    headlessGpuReady = false;
    usesHeadlessGpuPath = false;
    wsiPresentPathReady = false;
    surfaceHandoffPending = false;
    surfaceHandoffConsumed = false;
    qVulkanWindowWsiProbed = false;
    qVulkanWindowWsiReady = false;
    qVulkanInstanceReady = false;
    qVulkanExtensionsProbed = false;
    qVulkanWindowCreated = false;
    qVulkanWindowDestroyed = false;
    qVulkanSupportedExtensionCount = 0;
    qVulkanInstanceVersionMajor = 0;
    qVulkanInstanceVersionMinor = 0;
}

} // namespace fuse::editor
