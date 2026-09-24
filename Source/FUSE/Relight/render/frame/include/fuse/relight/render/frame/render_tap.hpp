// FUSE Relight RL-4.1: the tap that renders (docs/plans/FUSE_REMIX_PORT_PLAN.md §2.3).
//
// RenderTap wraps the capture tap (relight.tap.mode = capture) when relight.frame.* asks for frame work
// (FrameConfig::enabled). Every event goes to the capture tap first, unchanged, so the capture record, the
// replacement engine and the logic graphs see exactly what they see without it; then:
//
//   device        adopts the device's Vulkan device into the FUSE renderer (RendererContext: volk from the host's
//                 loader, VulkanDevice::adopt, the WP-0.4 bindless heap, the WP-1.1 GPU scene) when the host
//                 describes it, and attaches a FrameOrchestrator to the device's host (DeviceEvent::host);
//   textures      every DXVK image is registered in the bindless registry as external / non-owned
//                 (BindlessImageRegistry: the renderer's heap with real descriptors when adopted, else the CPU
//                 slot table), released on onImageDestroy; with relight.frame.textureSwap the swappable ones get
//                 their FUSE-owned passthrough twin;
//   draws         the first draw the RL-1.2 classifier marks as the RTX injection point (the first UI draw,
//                 Remix semantics) injects before DXVK records it: FUSE's image is composited below the UI;
//   Present       the dispatcher's onInjectPoint injects when no UI draw did (relight.frame.injectAtUi false:
//                 always here);
//   scene feed    at the injection point the draws recorded so far go through RL-1.7's SceneModel into the scene
//                 feed (scene_feed.hpp): AdapterDraws / AdapterLights for the IGpuSceneSink (the adopted
//                 renderer's GpuSceneAdapter, or setSceneSink's), so the GPU scene holds this frame's scene when
//                 FUSE renders it; the draws after the injection point join the SceneModel at the flush. With
//                 RL-3.4's replacement engine running, the whole feed stays at the flush (its replaced draws exist
//                 only then) and the GPU scene is one frame behind (record: "feed":"flush");
//   raster        relight.frame.mode = raster: the RL-4.2 frame renderer (frame_renderer.hpp) prepares the
//                 frame from the same draws and records its graph into FUSE's frame submission.
//
// Decisions stay Raster (the capture is advisory): only the composite and the texture swap touch DXVK's output.
#pragma once

#include <fuse/relight/render/frame/bindless_images.hpp>
#include <fuse/relight/render/frame/frame_options.hpp>
#include <fuse/relight/render/frame/frame_orchestrator.hpp>
#include <fuse/relight/render/frame/frame_renderer.hpp>
#include <fuse/relight/render/frame/frame_tap.hpp>
#include <fuse/relight/render/frame/renderer_context.hpp>
#include <fuse/relight/render/frame/scene_feed.hpp>
#include <fuse/relight/tap/capture_tap.hpp>

#include <cstdio>
#include <memory>
#include <string>

namespace fuse::relight::render::frame {

/// The scene feed of one frame.
struct SceneRecord {
    std::uint32_t draws = 0;          ///< committed draws fed
    std::uint32_t instances = 0;      ///< RL-1.7 instances alive after the frame
    std::uint32_t replaced = 0;       ///< draws with a RL-3.4 replacement
    std::uint32_t lights = 0;
    bool sink = false;                ///< a GPU scene received the feed
    std::uint32_t gpuInstances = 0;   ///< its live instances
    const char* feed = "flush";       ///< when the sink got this frame: "inject" (before FUSE rendered it) or "flush"
    std::uint64_t gpuFrame = 0;       ///< the frame whose draws the GPU scene held when FUSE rendered (inject feed)
    std::uint32_t lateDraws = 0;      ///< committed draws after the injection point (SceneModel only)
};

/// What happened in one frame (also the relight.frame.statsPath record).
struct FrameRecord {
    std::uint64_t frame = 0;
    std::string inject = "none"; ///< ui, present or none
    std::uint32_t injectDraw = 0; ///< draw index in the frame (ui)
    InjectResult result;
    SceneRecord scene;
    BindlessImageStats bindless;
    OrchestratorStats orchestrator;
};

class RenderTap final : public tap::IRelightTap {
public:
    RenderTap(std::unique_ptr<tap::CaptureTap> capture, FrameConfig config);
    ~RenderTap() override;
    RenderTap(const RenderTap&) = delete;
    RenderTap& operator=(const RenderTap&) = delete;

    tap::CaptureTap& capture() { return *m_capture; }
    FrameOrchestrator& orchestrator() { return m_orchestrator; }
    BindlessImageRegistry& bindless() { return *m_bindless; }
    /// Where the per-frame scene goes instead of the adopted renderer's GPU scene (not owned; null: the renderer's
    /// when adopted, else nowhere).
    void setSceneSink(IGpuSceneSink* sink) { m_externalSink = sink; }
    /// The adopted renderer (null before a device attached; check attached()).
    RendererContext* renderer() { return m_renderer.get(); }
    const FrameRecord& lastRecord() const { return m_last; }

    void onDeviceCreate(const tap::DeviceEvent& e) override;
    void onDeviceReset(const tap::DeviceEvent& e) override;
    void onDeviceDestroy() override;
    void onTextureCreate(const tap::TextureDesc& d) override;
    void onTextureUpload(const tap::TextureUpload& u) override;
    void onTextureCopy(const tap::TextureCopy& c) override;
    void onTextureWriteLock(const tap::TextureWriteLock& l) override;
    void onImageDestroy(const tap::ImageDestroy& d) override;
    void onBufferCreate(const tap::BufferDesc& d) override;
    void onBufferWrite(const tap::BufferWrite& w) override;
    void onBufferDestroy(tap::ResourceId id) override;
    tap::DrawDecision onDraw(const tap::DrawCall& call, const tap::DrawState& state) override;
    bool substituteVertexShader(const tap::ShaderModule& m, std::vector<std::uint32_t>& replacement) override;
    bool wantsVertexCapture() override;
    void onVertexCapture(const tap::VertexCaptureFrame& f) override;
    void onQueryBegin(const tap::QueryEvent& q) override;
    void onQueryEnd(const tap::QueryEvent& q) override;
    void onClear(const tap::ClearEvent& c) override;
    void onSetRenderTarget(const tap::SetRenderTargetEvent& e) override;
    void onInjectPoint(const tap::FrameEvent& f) override;
    void onPresent(const tap::FrameEvent& f) override;

private:
    struct Scene;
    void attachHost(tap::IFrameHost* host);
    void releaseDevice();
    IGpuSceneSink* sink();
    void doInject(const char* where);
    /// SceneModel (+ `toSink`: the scene feed) for draws [begin, end) with their classifications.
    void feedDraws(const std::vector<tap::CaptureDrawRecord>& draws, std::size_t begin, std::size_t end,
                   const std::vector<scene::DrawClassification>* classifications, IGpuSceneSink* toSink,
                   tap::IFrameProcessor* processor);
    void onFlushedFrame(std::uint64_t frame, const std::vector<tap::CaptureDrawRecord>& draws);
    void writeHeader();
    void writeRecord();

    std::unique_ptr<tap::CaptureTap> m_capture;
    FrameConfig m_config;
    tap::IFrameHost* m_host = nullptr;
    CpuBindlessHeap m_heap;
    std::unique_ptr<RendererContext> m_renderer;
    std::unique_ptr<BindlessImageRegistry> m_bindless;
    FrameOrchestrator m_orchestrator;
    std::unique_ptr<IFrameRenderer> m_frameRenderer;
    IGpuSceneSink* m_externalSink = nullptr;
    std::unique_ptr<Scene> m_scene;
    std::FILE* m_stats = nullptr;
    FrameRecord m_current;
    FrameRecord m_last;
    std::uint32_t m_drawInFrame = 0;
    bool m_injected = false;
    bool m_fedAtInject = false;     ///< this frame's draws [0, m_fedCount) went to the sink at the injection point
    std::size_t m_fedCount = 0;
    tap::ResourceId m_backBuffer = tap::kNoResource;
    bool m_haveClear = false;
    std::uint32_t m_clearColor = 0;
    bool m_headerWritten = false;
    bool m_destroying = false;
};

} // namespace fuse::relight::render::frame
