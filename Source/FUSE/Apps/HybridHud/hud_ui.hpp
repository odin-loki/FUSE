#pragma once
// demo_hybrid_hud's HUD on a FUSE UI context (UI4 game path): fuse_ui_game / widget nodes with real text, drawn by
// "ui.draw_list" over the sprite layer in the hybrid frame's UI stage (HybridComposer::setUiOverlay ->
// ui::render::UiFrameSource::recordOver). Replaces the placeholder UI quad.
#if defined(FUSE_HYBRID_HUD_HAS_UI)

#include "hybrid_module_gates.hpp"

#include <fuse/hybrid/hybrid_composer.hpp>
#include <fuse/text/text.hpp>
#include <fuse/ui/render/ui_layer_gpu.hpp>
#include <fuse/ui/ui.hpp>

#include <memory>
#include <optional>

namespace fuse::hybrid::hud {

/// Readback check of the HUD (after HybridSceneRenderer::waitIdle): every pixel the UI covers opaquely must equal the
/// CPU rasteriser's pixel (the GPU kernel matches it bit-exactly; the UI composite runs in half floats and the frame
/// is blitted to RGBA8, so allow 1 / 255 like checkOverlayReadback).
struct HudCheck {
    bool overlayRan = false; ///< the last frame drew the UI overlay
    u32 covered = 0;         ///< opaque UI pixels compared
    u32 mismatched = 0;      ///< of them, off by more than 1 / 255 in a channel
    u32 glyphs = 0;          ///< glyph commands in the draw list (real text)
    u32 textPixels = 0;      ///< matched opaque pixels the glyphs changed (text visible in the frame)
};

class HudUi {
public:
    /// Builds the HUD and installs the overlay on `composer`. False (the placeholder quad stays) without the bundled
    /// font, a Vulkan device or the ui.draw_list kernel.
    bool init(HybridComposer& composer, u32 width, u32 height);
    /// Per frame, before HybridComposer::render: texts and bar from the slice state, then ui.update.
    void update(HybridComposer& composer, const gates::State& state, const frame::FrameCtx& ctx);
    /// Removes the overlay (waits for the GPU) and releases the kernel.
    void shutdown(HybridComposer& composer);
    bool active() const { return m_active; }
    HudCheck check(const HybridSceneRenderer& gpu) const;

private:
    std::optional<text::Font> m_font;
    std::unique_ptr<ui::UiContext> m_ui;
    ui::render::UiFrameSource m_source;
    ui::NodeHandle m_objective, m_frame, m_health;
    bool m_active = false;
};

} // namespace fuse::hybrid::hud

#endif // defined(FUSE_HYBRID_HUD_HAS_UI)
