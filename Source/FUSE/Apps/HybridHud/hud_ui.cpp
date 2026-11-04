// demo_hybrid_hud's HUD on a FUSE UI context: see hud_ui.hpp.
#if defined(FUSE_HYBRID_HUD_HAS_UI)

#include "hud_ui.hpp"

#include <fuse/log/logger.hpp>
#include <fuse/renderer/rhi_context.hpp>
#include <fuse/ui/game/game_ui.hpp>
#include <fuse/ui/widgets/widgets.hpp>

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace fuse::hybrid::hud {

namespace {

HybridUiOverlay overlayOf(ui::render::UiFrameSource& source) {
    return HybridUiOverlay{&ui::render::UiFrameSource::recordOver, &source};
}

ui::NodeHandle hudLabel(ui::UiContext& ui, ui::NodeHandle parent, const char* text, const char* id) {
    const ui::NodeHandle l = ui::widgets::label(ui, parent, text, id);
    ui.setCss(l, "font-size", "15px");
    ui.setCss(l, "color", "#FFFFFF");
    return l;
}

} // namespace

bool HudUi::init(HybridComposer& composer, u32 width, u32 height) {
    renderer::RhiContext* rhi = composer.rhiContext();
    renderer::VulkanDevice* device = rhi != nullptr ? rhi->bootstrap().device() : nullptr;
    auto font = text::Font::bundled();
    if (!font) {
        log::warn("demo_hybrid_hud: no bundled UI font, HUD stays the placeholder quad");
        return false;
    }
    if (device == nullptr || !m_source.init(device)) {
        log::warn("demo_hybrid_hud: ui.draw_list kernel unavailable, HUD stays the placeholder quad");
        return false;
    }
    m_font.emplace(std::move(*font));
    m_ui = std::make_unique<ui::UiContext>(
        ui::UiContextDesc{.viewport = {static_cast<f32>(width), static_cast<f32>(height)}, .font = &*m_font});
    ui::UiContext& ui = *m_ui;
    ui::widgets::install(ui);
    ui::game::install(ui);
    // Game screen: anchored layout over the 3D frame, nothing painted where no widget is.
    ui.setCss(ui.root(), "layout", "anchor");
    ui.setCss(ui.root(), "padding", "0px");
    ui.setCss(ui.root(), "background", "transparent");

    // Top band: title + objective (the adventure HUD prompt), frame counter on the right.
    const ui::NodeHandle top = ui::widgets::panel(ui, ui.root(), "hud.top");
    ui.setCss(top, "left", "6px");
    ui.setCss(top, "top", "4px");
    ui.setCss(top, "padding", "4px");
    ui.setCss(top, "background", "#000000A0");
    hudLabel(ui, top, "OUTPOST", "hud.title");
    m_objective = hudLabel(ui, top, "Find the lever", "hud.objective");
    m_frame = hudLabel(ui, ui.root(), "F 0", "hud.frame");
    ui.setCss(m_frame, "right", "6px");
    ui.setCss(m_frame, "top", "6px");

    // Bottom band: the guard's health as a HUD bar with its label.
    const ui::NodeHandle vitals = ui::widgets::panel(ui, ui.root(), "hud.vitals");
    ui.setCss(vitals, "left", "6px");
    ui.setCss(vitals, "bottom", "6px");
    ui.setCss(vitals, "width", "140px");
    ui.setCss(vitals, "background", "transparent");
    m_health = ui::game::hudBar(ui, vitals, 1.f, "hud.health");
    ui.setText(m_health, "GUARD");

    composer.setUiOverlay(overlayOf(m_source));
    m_active = true;
    return true;
}

void HudUi::update(HybridComposer& composer, const gates::State& state, const frame::FrameCtx& ctx) {
    if (!m_active) {
        return;
    }
    ui::UiContext& ui = *m_ui;
    // Follow the GPU frame's extent: the draw list is packed at the UI stage's output extent.
    if (const HybridSceneRenderer* gpu = composer.gpuScene(); gpu != nullptr) {
        const ui::Size extent{static_cast<f32>(gpu->width()), static_cast<f32>(gpu->height())};
        if (!(ui.viewport() == extent)) {
            ui.setViewport(extent);
        }
    }
    if (!state.hudPromptText.empty()) {
        ui.setText(m_objective, state.hudPromptText);
    }
    char frameText[32];
    std::snprintf(frameText, sizeof(frameText), "F %u", ctx.frameIndex);
    ui.setText(m_frame, frameText);
    const s32 maxHealth = state.guardHealth.maxHealth();
    ui::game::setHudBar(ui, m_health,
                        maxHealth > 0 ? static_cast<f32>(state.guardHealth.currentHealth()) / static_cast<f32>(maxHealth)
                                      : 0.f);
    ui.update(ctx.dt);
    m_source.setDrawList(&ui.drawList(), ui.atlas().pixels(), static_cast<u32>(ui.atlas().size()));
}

void HudUi::shutdown(HybridComposer& composer) {
    if (!m_active) {
        return;
    }
    if (HybridSceneRenderer* gpu = composer.gpuScene(); gpu != nullptr) {
        (void)gpu->waitIdle();
    }
    composer.setUiOverlay({});
    m_source.destroy();
    m_active = false;
}

HudCheck HudUi::check(const HybridSceneRenderer& gpu) const {
    HudCheck c{};
    c.overlayRan = gpu.lastFrame().uiOverlayRan;
    const u8* px = gpu.readbackPixels();
    if (!m_active || px == nullptr) {
        return c;
    }
    ui::UiContext& ui = *m_ui; // atlas() has no const overload
    const u32 w = gpu.width();
    const u32 h = gpu.height();
    if (static_cast<u32>(ui.viewport().w) != w || static_cast<u32>(ui.viewport().h) != h) {
        return c;
    }
    // CPU reference of the last frame's draw list over transparent (premultiplied RGBA8, R in the low byte).
    ui::Image ref;
    ref.resize(static_cast<int>(w), static_cast<int>(h), 0u);
    ui::CpuRasterizer raster;
    raster.setGlyphAtlas(ui.atlas().pixels(), ui.atlas().size());
    raster.render(ui.drawList(), ref);
    // The same list without its glyph quads: pixels that differ are the text.
    ui::DrawList noText = ui.drawList();
    std::erase_if(noText.cmds(), [](const ui::DrawCmd& cmd) { return cmd.op == ui::DrawOp::Glyph; });
    c.glyphs = static_cast<u32>(ui.drawList().size() - noText.size());
    ui::Image bare;
    bare.resize(static_cast<int>(w), static_cast<int>(h), 0u);
    raster.render(noText, bare);
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            const u32 r = ref.at(static_cast<int>(x), static_cast<int>(y));
            if ((r >> 24) != 0xFFu) {
                continue; // only opaque UI pixels are independent of the scene below
            }
            ++c.covered;
            const u8* g = px + (static_cast<usize>(y) * w + x) * 4u;
            bool bad = false;
            for (u32 ch = 0; ch < 3u; ++ch) {
                bad = bad || std::abs(static_cast<int>(g[ch]) - static_cast<int>((r >> (8u * ch)) & 0xFFu)) > 1;
            }
            c.mismatched += bad ? 1u : 0u;
            // Text visible: a matched pixel the glyphs changed.
            if (!bad && r != bare.at(static_cast<int>(x), static_cast<int>(y))) {
                ++c.textPixels;
            }
        }
    }
    return c;
}

} // namespace fuse::hybrid::hud

#endif // defined(FUSE_HYBRID_HUD_HAS_UI)
