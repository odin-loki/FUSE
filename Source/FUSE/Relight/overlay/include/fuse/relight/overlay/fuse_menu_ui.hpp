// FUSE Relight RL-6.1 on FUSE UI: the developer menu's UI built on fuse::ui::immediate::ImmediateUi (the first client of
// FUSE UI, docs/plans/FUSE_UI_PLAN.md). Same API as overlay::Ui so DevMenu, scripts and tests are unchanged; layout,
// ids, clicks and focus are identical (gate fuse_ui_relight_equivalence). Text is drawn with a real font: each text
// draw becomes a semantic `shaped` Text command (the string, as before) followed by Glyph commands sampling
// glyphAtlas(). Without a font (FUSE_RELIGHT_OVERLAY_FONT unset and no bundled font) it falls back to the 5x7 font.
#pragma once

#include <fuse/relight/overlay/ui.hpp>

#include <fuse/text/text.hpp>
#include <fuse/ui/immediate.hpp>

#include <array>
#include <memory>
#include <string>
#include <string_view>

namespace fuse::relight::overlay {

class FuseMenuUi {
public:
    FuseMenuUi();

    void begin(const Rect& panel, std::int32_t scale, const InputEvent* event);
    void end();

    void sameLine() { m_ui.sameLine(); }
    void spacing(std::int32_t lines = 1) { m_ui.spacing(lines); }
    void separator() { m_ui.separator(); }
    bool hasRoom(std::int32_t lines = 1) const { return m_ui.hasRoom(lines); }
    std::int32_t linesLeft() const { return m_ui.linesLeft(); }
    std::int32_t charsLeft() const { return m_ui.charsLeft(); }

    void text(std::string_view s, Color color = palette::kText);
    void textf(Color color, const char* format, ...)
#if defined(__GNUC__)
        __attribute__((format(printf, 3, 4)))
#endif
        ;
    void title(std::string_view s) { m_ui.title(s); }
    bool button(std::string_view id, std::string_view label, bool selected = false) { return m_ui.button(id, label, selected); }
    bool textField(std::string_view id, std::string& value, std::int32_t widthChars, std::size_t maxLength = 64) {
        return m_ui.textField(id, value, widthChars, maxLength);
    }

    const InputEvent& event() const { return m_event; }
    std::int32_t keyPressed() const { return m_ui.keyPressed(); }
    std::int32_t wheel() const { return m_ui.wheel(); }
    bool mouseInPanel() const { return m_ui.mouseInPanel(); }

    const DrawList& draws() const { return m_draws; }
    bool widgetRect(std::string_view id, Rect& out) const;
    const Rect& panel() const { return m_panel; }
    std::int32_t scale() const { return m_scale; }
    std::uint32_t focused() const { return m_ui.focused(); }
    std::uint32_t widgetCount() const { return m_ui.widgetCount(); }
    bool hasFont() const { return m_font != nullptr; }

    static fuse::ui::Color toUi(Color premultiplied);

private:
    fuse::ui::immediate::ImmediateUi m_ui;
    std::unique_ptr<fuse::text::Font> m_font;
    std::unique_ptr<fuse::text::GlyphAtlas> m_atlas;
    DrawList m_draws;
    InputEvent m_event;
    Rect m_panel;
    std::int32_t m_scale = 1;
    std::array<char, 256> m_fmt{};
};

} // namespace fuse::relight::overlay
