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
    qtLivePresentAttempts = 0;
    qtLivePresentTicks = 0;
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
}

} // namespace fuse::editor
