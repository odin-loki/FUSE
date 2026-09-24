// FUSE Relight RL-1.1: the capture tap (relight.tap.mode = capture).
//
// CaptureTap runs the Wave R1 capture packages live, inside d3d9.dll, on the events the DXVK
// dispatcher reports (instead of recording them for the replay tools):
//   * TextureTracker (RL-1.4): canonical mip-0 bytes, first-upload / inherited / render-target
//     hashes, managed textures hashed at their first sampling draw; every hashed image in an
//     ExternalImageRegistry;
//   * GeometryCapture (RL-1.3): the application's real vertex / index bytes (a CPU shadow of every
//     buffer write, or DXVK's own mapping), index rebasing, the geometry hash components, the
//     asset key, bounding box and skinning data;
//   * ClassifyTap (RL-1.2): the draw classifier, fed the texture hashes TextureTracker computed.
// Per event the order is: forward tap (optional; the recording tap, so one run yields the replay
// tools' input too), TextureTracker, classifier, GeometryCapture. Geometry asks TextureTracker
// whether a texture has an image hash (Remix processTextures); the geometry categories
// (rtx.skyBoxGeometries) are applied to the classification once the asset key is known.
//
// The capture is advisory: onDraw always returns DrawDecision::Raster (Relight does not render
// yet), so DXVK's output is unchanged (rl_passthrough_golden_* checks it).
//
// Per-frame capture record (JSON Lines, schema "fuse.relight.capture/1"), written at each Present
// (and for the draws after the last Present at device destruction):
//   header    schema, tap interface version
//   draw      n (draw number in the device), frame, di (draw in the frame);
//             geometry: the fields of geometry_replay's output line, same spelling (status, tci,
//               stage, f = the 9 hash components, ic, vc, min, max, topo, it, ps, key = asset key,
//               leg0 / leg1, memo, aabb, skin), captured draws only beyond status / tci / stage;
//             textures: [{slot, texture, hash, desc}] bound sampler slots with TextureTracker's hashes;
//             classification: rl_classify_replay's fields (draw_call_id, status, reason, inject,
//               categories, decision, sky_auto, using_rt_rt, drawing_to_rt_rt, color_texture)
//   textures  frame, every live tracked texture (id, hash, desc, origin, from, pending, obsolete,
//             preview, registered): the fields of the texture replay driver's "tex" lines
//   frame     frame, draws, captured
//   device_destroy
// Hashes are 16 upper-case hex digits in "textures" entries, as the texture replay prints them;
// geometry values follow geometry_replay (lower-case hex).
#pragma once

#include <fuse/relight/capture/geometry/geometry_capture.hpp>
#include <fuse/relight/capture/texture/external_image_registry.hpp>
#include <fuse/relight/capture/texture/texture_tracker.hpp>
#include <fuse/relight/scene/classify/classify_tap.hpp>
#include <fuse/relight/tap/relight_tap.hpp>

#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace fuse::relight::tap {

struct CaptureTapConfig {
    std::string path;                     ///< capture record (JSON Lines); empty: none
    std::unique_ptr<IRelightTap> forward; ///< receives every event first (e.g. RecordingTap); may be null
    capture::texture::TextureTrackerConfig texture;
    capture::geometry::GeometryCaptureConfig geometry;

    /// The rtx.* / relight.* options of the three packages, resolved now.
    static CaptureTapConfig fromOptions();
};

/// One draw of the capture record.
struct CaptureDrawRecord {
    std::uint64_t n = 0; ///< draws before this one on the device
    std::uint64_t frame = 0;
    std::uint32_t drawInFrame = 0;
    capture::geometry::CapturedDrawPtr geometry;
    scene::DrawClassification classification;
    struct BoundTexture {
        std::uint32_t slot = 0; ///< tap sampler slot
        ResourceId texture = kNoResource;
        std::uint64_t imageHash = 0;
        std::uint64_t descriptorHash = 0;
    };
    std::vector<BoundTexture> textures;
};

class CaptureTap final : public IRelightTap {
public:
    explicit CaptureTap(CaptureTapConfig config);
    ~CaptureTap() override;
    CaptureTap(const CaptureTap&) = delete;
    CaptureTap& operator=(const CaptureTap&) = delete;

    bool isOpen() const { return m_file != nullptr; }

    /// Called with each finished frame's draws (at Present, geometry jobs complete), before the
    /// record lines are written. For tests and in-process consumers. Runs under the tap's lock: it
    /// may query the packages (textures(), geometry(), ...) but must not send events to the tap.
    using FrameSink = std::function<void(std::uint64_t frame, const std::vector<CaptureDrawRecord>& draws)>;
    void setFrameSink(FrameSink sink);

    capture::texture::TextureTracker& textures() { return m_textures; }
    capture::texture::ExternalImageRegistry& imageRegistry() { return m_registry; }
    capture::geometry::GeometryCapture& geometry() { return m_geometry; }
    scene::ClassifyTap& classifier() { return m_classify; }

    void onDeviceCreate(const DeviceEvent& e) override;
    void onDeviceReset(const DeviceEvent& e) override;
    void onDeviceDestroy() override;
    void onTextureCreate(const TextureDesc& d) override;
    void onTextureUpload(const TextureUpload& u) override;
    void onTextureCopy(const TextureCopy& c) override;
    void onTextureWriteLock(const TextureWriteLock& l) override;
    void onImageDestroy(const ImageDestroy& d) override;
    void onBufferCreate(const BufferDesc& d) override;
    void onBufferWrite(const BufferWrite& w) override;
    void onBufferDestroy(ResourceId id) override;
    DrawDecision onDraw(const DrawCall& call, const DrawState& state) override;
    bool substituteVertexShader(const ShaderModule& m, std::vector<std::uint32_t>& replacement) override;
    void onQueryBegin(const QueryEvent& q) override;
    void onQueryEnd(const QueryEvent& q) override;
    void onClear(const ClearEvent& c) override;
    void onSetRenderTarget(const SetRenderTargetEvent& e) override;
    void onInjectPoint(const FrameEvent& f) override;
    void onPresent(const FrameEvent& f) override;

private:
    capture::geometry::GeometryCaptureConfig wireGeometry(capture::geometry::GeometryCaptureConfig config);
    void syncClassifierTextures(const DrawState& state);
    void flushFrame(bool final);
    void writeLine(const std::string& line);

    std::mutex m_mutex; ///< every event (queries arrive without the D3D9 device lock)
    std::unique_ptr<IRelightTap> m_forward;
    capture::texture::ExternalImageRegistry m_registry;
    capture::texture::TextureTracker m_textures;
    scene::ClassifyTap m_classify;
    capture::geometry::GeometryCapture m_geometry;

    std::FILE* m_file = nullptr;
    FrameSink m_sink;
    bool m_destroyed = false;
    std::uint64_t m_frame = 0;
    std::uint64_t m_drawCount = 0;
    std::uint32_t m_drawInFrame = 0;
    std::vector<CaptureDrawRecord> m_pending;
    scene::ClassifiedDraw m_lastClassified;
    bool m_haveClassified = false;
    capture::geometry::CapturedDrawPtr m_lastGeometry;
};

} // namespace fuse::relight::tap
