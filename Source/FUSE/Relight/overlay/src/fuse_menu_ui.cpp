// FUSE Relight RL-6.1 developer menu UI on FUSE UI (see fuse_menu_ui.hpp).
#include <fuse/relight/overlay/fuse_menu_ui.hpp>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace fuse::relight::overlay {

namespace ui = fuse::ui;

namespace {
constexpr float kFontPx = 9.f; ///< per scale unit; cap height ~6.5 px fits the overlay's 7 px glyph cell
constexpr int kAtlasSize = 512;

std::unique_ptr<fuse::text::Font> loadFont() {
    if (const char* path = std::getenv("FUSE_RELIGHT_OVERLAY_FONT"); path && *path) {
        if (auto f = fuse::text::Font::fromFile(path)) return std::make_unique<fuse::text::Font>(std::move(*f));
    }
    if (auto f = fuse::text::Font::bundled()) return std::make_unique<fuse::text::Font>(std::move(*f));
    return nullptr;
}
} // namespace

ui::Color FuseMenuUi::toUi(Color c) {
    const float a = float(c >> 24) / 255.f;
    if (a <= 0.f) return {};
    auto ch = [&](int shift) { return std::min(1.f, float((c >> shift) & 0xffu) / 255.f / a); };
    return {ch(0), ch(8), ch(16), a};
}

FuseMenuUi::FuseMenuUi() : m_font(loadFont()) {
    ui::immediate::Theme t;
    t.panel = toUi(palette::kPanel);
    t.title = toUi(palette::kTitle);
    t.text = toUi(palette::kText);
    t.dim = toUi(palette::kDim);
    t.accent = toUi(palette::kAccent);
    t.button = toUi(palette::kButton);
    t.buttonHot = toUi(palette::kButtonHot);
    t.buttonOn = toUi(palette::kButtonOn);
    t.field = toUi(palette::kField);
    t.radius = 0.f; // the overlay draws axis-aligned fills
    m_ui.setTheme(t);
    if (m_font) {
        m_atlas = std::make_unique<fuse::text::GlyphAtlas>(kAtlasSize);
        m_ui.setFont(m_font.get(), m_atlas.get(), kFontPx);
    }
    m_draws.reserve(512, 4096);
}

void FuseMenuUi::begin(const Rect& panel, std::int32_t scale, const InputEvent* event) {
    m_panel = panel;
    m_scale = scale < 1 ? 1 : scale;
    m_event = event ? *event : InputEvent{};
    ui::immediate::InputEvent e;
    e.type = ui::immediate::EventType(static_cast<std::uint8_t>(m_event.type));
    e.x = m_event.x;
    e.y = m_event.y;
    e.value = m_event.value;
    m_ui.begin({panel.x, panel.y, panel.w, panel.h}, scale, event ? &e : nullptr);
}

void FuseMenuUi::text(std::string_view s, Color color) { m_ui.text(s, toUi(color)); }

void FuseMenuUi::textf(Color color, const char* format, ...) {
    va_list args;
    va_start(args, format);
    const int n = std::vsnprintf(m_fmt.data(), m_fmt.size(), format, args);
    va_end(args);
    if (n < 0) return;
    text(std::string_view(m_fmt.data(), std::min<std::size_t>(std::size_t(n), m_fmt.size() - 1)), color);
}

void FuseMenuUi::end() {
    m_ui.end();
    // Translate the pass: rects -> Fill, glyph quads -> Glyph, each text run -> a semantic Text command in front of its
    // glyphs (or a 5x7 Text command when there is no font).
    m_draws.clear();
    m_draws.setGlyphAtlas(m_atlas ? m_atlas->pixels() : nullptr, m_atlas ? kAtlasSize : 0);
    const auto& cmds = m_ui.draws().cmds();
    const auto& runs = m_ui.textRuns();
    std::size_t r = 0;
    auto emitRuns = [&](std::size_t upTo) {
        for (; r < runs.size() && runs[r].firstCmd <= upTo; ++r) {
            const auto& run = runs[r];
            const Color col = ui::packPremultiplied(run.color);
            const std::string_view s = m_ui.text(run);
            if (m_font) m_draws.shapedText(run.x, run.y, s, col, m_scale, run.clipW);
            else m_draws.text(run.x, run.y, s, col, m_scale, run.clipW);
        }
    };
    for (std::size_t i = 0; i < cmds.size(); ++i) {
        emitRuns(i);
        const ui::DrawCmd& c = cmds[i];
        const Color col = ui::packPremultiplied(c.fill);
        if (c.op == ui::DrawOp::Rect) {
            const auto x = std::int32_t(std::lround(c.rect.x)), y = std::int32_t(std::lround(c.rect.y));
            m_draws.fill({x, y, std::int32_t(std::lround(c.rect.w)), std::int32_t(std::lround(c.rect.h))}, col);
        } else if (c.op == ui::DrawOp::Glyph) {
            m_draws.glyph(std::int32_t(std::lround(c.rect.x)), std::int32_t(std::lround(c.rect.y)), c.tw, c.th, c.u, c.v, col);
        }
    }
    emitRuns(cmds.size());
}

bool FuseMenuUi::widgetRect(std::string_view id, Rect& out) const {
    ui::immediate::IRect r;
    if (!m_ui.widgetRect(id, r)) return false;
    out = {r.x, r.y, r.w, r.h};
    return true;
}

} // namespace fuse::relight::overlay
