// FUSE Relight RL-1.2: tap events -> D3DStateModel -> DrawClassifier.
//
// D3DStateTracker keeps what the classifier needs from earlier tap events (present parameters,
// texture descriptors and hashes, open occlusion queries, the frame boundary) and turns each
// onDraw(DrawCall, DrawState) into a D3DStateModel. ClassifyTap is an IRelightTap that runs the
// tracker and the classifier on every event, optionally forwards the events to another tap (e.g. the
// recording tap) and reports each classification to a sink.
//
// Texture hashes: Remix keeps the colour-texture hash and the render-target descriptor hash on the
// DxvkImage. In the live pipeline RL-1.4 computes them (setTextureHash / setDescriptorHash); until it
// does, render-target textures get the descriptor hash computed from the tap's TextureDesc
// (hash::hashTextureDescriptor, D3D9_COMMON_TEXTURE_DESC::CalculateHash) and no image hash.
#pragma once

#include <fuse/relight/scene/classify/d3d_state_model.hpp>
#include <fuse/relight/scene/classify/draw_classifier.hpp>
#include <fuse/relight/tap/relight_tap.hpp>

#include <functional>
#include <unordered_map>

namespace fuse::relight::scene {

class D3DStateTracker {
public:
    D3DStateTracker();

    void onDevice(const tap::DeviceEvent& e);
    void onTextureCreate(const tap::TextureDesc& d);
    void onImageDestroy(const tap::ImageDestroy& d);
    void onQueryBegin(const tap::QueryEvent& q);
    void onQueryEnd(const tap::QueryEvent& q);

    /// Remix texture hash of a texture (DxvkImage::getHash()).
    void setTextureHash(tap::ResourceId texture, Hash64 hash);
    /// Render-target descriptor hash (DxvkImage::getDescriptorHash()); overrides the computed one.
    void setDescriptorHash(tap::ResourceId texture, Hash64 hash);
    const TextureRecord* texture(tap::ResourceId id) const;

    /// The model of one draw. A shader whose bytecode the event does not carry samples every slot.
    D3DStateModel buildModel(const tap::DrawCall& call, const tap::DrawState& state) const;

    std::uint32_t backBufferWidth() const { return m_backBufferWidth; }
    std::uint32_t backBufferHeight() const { return m_backBufferHeight; }
    std::int32_t activeOcclusionQueries() const { return m_activeOcclusionQueries; }

    /// Environment overrides read at construction (DXVK_RESOLUTION_WIDTH / _HEIGHT).
    bool resolutionOverride = false;

private:
    std::unordered_map<tap::ResourceId, TextureRecord> m_textures;
    std::uint32_t m_backBufferWidth = 0, m_backBufferHeight = 0;
    bool m_d3d8 = false;
    std::int32_t m_activeOcclusionQueries = 0;
};

/// Converts a tap TextureDesc into a TextureRecord (descriptor hash computed for render targets).
TextureRecord textureRecordFromDesc(const tap::TextureDesc& d);

/// One classified draw, as the sink receives it.
struct ClassifiedDraw {
    std::uint64_t frame = 0;       ///< presents so far
    std::uint32_t indexInFrame = 0; ///< every draw of the frame, from 0
    DrawClassification result;
};

class ClassifyTap final : public tap::IRelightTap {
public:
    using Sink = std::function<void(const ClassifiedDraw&)>;

    /// `forward`: optional tap that receives every event first (not owned). `applyDecisions`:
    /// onDraw returns the classifier's decision (toTapDecision); otherwise the forwarded tap's
    /// decision (Raster without one), so classification is advisory until Relight renders.
    ClassifyTap(tap::IRelightTap* forward, Sink sink, bool applyDecisions = false);

    D3DStateTracker& tracker() { return m_tracker; }
    DrawClassifier& classifier() { return m_classifier; }

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
    void onQueryBegin(const tap::QueryEvent& q) override;
    void onQueryEnd(const tap::QueryEvent& q) override;
    void onClear(const tap::ClearEvent& c) override;
    void onSetRenderTarget(const tap::SetRenderTargetEvent& e) override;
    void onInjectPoint(const tap::FrameEvent& f) override;
    void onPresent(const tap::FrameEvent& f) override;

private:
    tap::IRelightTap* m_forward;
    Sink m_sink;
    bool m_applyDecisions;
    D3DStateTracker m_tracker;
    DrawClassifier m_classifier;
    std::uint64_t m_frame = 0;
    std::uint32_t m_drawInFrame = 0;
};

} // namespace fuse::relight::scene
