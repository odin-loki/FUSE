// FUSE Relight RL-4.1: the tap that renders (docs/plans/FUSE_REMIX_PORT_PLAN.md §2.3).
//
// RenderTap wraps the capture tap (relight.tap.mode = capture) when relight.frame.* asks for frame work
// (FrameConfig::enabled). Every event goes to the capture tap first, unchanged, so the capture record, the
// replacement engine and the logic graphs see exactly what they see without it; then:
//
//   device        attaches a FrameOrchestrator to the device's host (DeviceEvent::host);
//   textures      every DXVK image is registered in the bindless registry as external / non-owned
//                 (BindlessImageRegistry), released on onImageDestroy; with relight.frame.textureSwap the
//                 swappable ones get their FUSE-owned passthrough twin;
//   draws         the first draw the RL-1.2 classifier marks as the RTX injection point (the first UI draw,
//                 Remix semantics) injects before DXVK records it: FUSE's image is composited below the UI;
//   Present       the dispatcher's onInjectPoint injects when no UI draw did (relight.frame.injectAtUi false:
//                 always here); the capture tap's flushed frame (FrameSink) goes through RL-1.7's SceneModel
//                 and RL-3.4's replaced draws (when the replacement engine runs) into the scene feed
//                 (scene_feed.hpp): AdapterDraws / AdapterLights for an IGpuSceneSink (GpuSceneAdapter -> the
//                 renderer's GpuScene) when one is attached (setSceneSink; none inside d3d9.dll, see scene_feed.hpp).
//
// Decisions stay Raster (the capture is advisory): only the composite and the texture swap touch DXVK's output.
#pragma once

#include <fuse/relight/render/frame/bindless_images.hpp>
#include <fuse/relight/render/frame/frame_options.hpp>
#include <fuse/relight/render/frame/frame_orchestrator.hpp>
#include <fuse/relight/render/frame/frame_tap.hpp>
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
    BindlessImageRegistry& bindless() { return m_bindless; }
    /// Where the per-frame scene goes (not owned; null: nowhere).
    void setSceneSink(IGpuSceneSink* sink) { m_sink = sink; }
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
    void doInject(const char* where);
    void onFlushedFrame(std::uint64_t frame, const std::vector<tap::CaptureDrawRecord>& draws);
    void writeRecord();

    std::unique_ptr<tap::CaptureTap> m_capture;
    FrameConfig m_config;
    tap::IFrameHost* m_host = nullptr;
    CpuBindlessHeap m_heap;
    BindlessImageRegistry m_bindless;
    FrameOrchestrator m_orchestrator;
    IGpuSceneSink* m_sink = nullptr;
    std::unique_ptr<Scene> m_scene;
    std::FILE* m_stats = nullptr;
    FrameRecord m_current;
    FrameRecord m_last;
    std::uint32_t m_drawInFrame = 0;
    bool m_injected = false;
};

} // namespace fuse::relight::render::frame
