// FUSE Relight RL-6.1: the developer overlay's immediate-mode UI (FUSE's own; no Dear ImGui in Engine/lib).
//
// Vulkan-free and deterministic. A frame of the UI is one or more *passes* of the menu's build code over the same
// widgets; each pass sees at most one input event (Ui::begin), so a press and a release that arrive in the same game
// frame still make a click (Dear ImGui's "input trickling"), and a scripted click lands on the layout the previous
// event produced. The last pass of a frame runs without an event and its draw list is what is drawn.
//
//   layout   one widget per line unless sameLine() precedes it; a line is kLineHeight * scale pixels; widgets that do
//            not fit in the panel are clipped (neither drawn nor hit-tested);
//   ids      strings hashed with FNV-1a (no allocation); widgetRect(id) finds a widget of the last finished pass
//            (scripted clicks: script.hpp);
//   draws    DrawList: filled rectangles and 5x7 text in premultiplied RGBA8 (raster.hpp rasterises them).
//
// Steady state: every container keeps its capacity; a pass allocates nothing unless the draw list outgrows it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fuse::relight::overlay {

struct Rect {
    std::int32_t x = 0, y = 0, w = 0, h = 0;
    bool contains(std::int32_t px, std::int32_t py) const { return px >= x && py >= y && px < x + w && py < y + h; }
    bool empty() const { return w <= 0 || h <= 0; }
    friend bool operator==(const Rect& a, const Rect& b) { return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h; }
};

/// Premultiplied RGBA8, R in the low byte.
using Color = std::uint32_t;
constexpr Color rgba(std::uint32_t r, std::uint32_t g, std::uint32_t b, std::uint32_t a) {
    // Premultiply (rounded): the colour channels never exceed alpha.
    return ((r * a + 127u) / 255u) | (((g * a + 127u) / 255u) << 8) | (((b * a + 127u) / 255u) << 16) | (a << 24);
}

namespace palette {
inline constexpr Color kPanel = rgba(16, 18, 26, 216);
inline constexpr Color kTitle = rgba(40, 70, 140, 255);
inline constexpr Color kText = rgba(230, 232, 238, 255);
inline constexpr Color kDim = rgba(150, 156, 170, 255);
inline constexpr Color kAccent = rgba(120, 200, 255, 255);
inline constexpr Color kWarn = rgba(255, 190, 90, 255);
inline constexpr Color kButton = rgba(52, 58, 76, 255);
inline constexpr Color kButtonHot = rgba(72, 82, 108, 255);
inline constexpr Color kButtonOn = rgba(46, 120, 200, 255);
inline constexpr Color kField = rgba(8, 8, 12, 255);
} // namespace palette

enum class EventType : std::uint8_t {
    None = 0,
    MouseMove,
    MouseDown, ///< left button
    MouseUp,
    Wheel,     ///< value: +1 up / -1 down (notches)
    KeyDown,   ///< value: Win32 virtual-key code
    Char,      ///< value: character (WM_CHAR, 32..126 used)
    Toggle,    ///< the overlay hotkey (Alt+X)
};

struct InputEvent {
    EventType type = EventType::None;
    std::int32_t x = 0, y = 0; ///< back-buffer pixels (mouse events)
    std::int32_t value = 0;
};

// Win32 virtual keys the menu uses.
inline constexpr std::int32_t kVkBack = 0x08, kVkReturn = 0x0D, kVkEscape = 0x1B, kVkPrior = 0x21, kVkNext = 0x22,
                              kVkHome = 0x24, kVkUp = 0x26, kVkDown = 0x28;

struct DrawCmd {
    enum class Kind : std::uint8_t { Fill, Text };
    Kind kind = Kind::Fill;
    std::int32_t x = 0, y = 0, w = 0, h = 0; ///< Fill: the rectangle; Text: origin (w = clip width in pixels)
    Color color = 0;
    std::uint32_t textBegin = 0, textLength = 0; ///< Text: range in DrawList::text()
    std::int32_t scale = 1;
};

class DrawList {
public:
    void clear() {
        m_cmds.clear();
        m_text.clear();
    }
    void fill(const Rect& r, Color color);
    /// Text clipped to `clipWidth` pixels (whole characters).
    void text(std::int32_t x, std::int32_t y, std::string_view s, Color color, std::int32_t scale, std::int32_t clipWidth);
    const std::vector<DrawCmd>& cmds() const { return m_cmds; }
    std::string_view text(const DrawCmd& c) const { return {m_text.data() + c.textBegin, c.textLength}; }
    void reserve(std::size_t cmds, std::size_t chars) {
        m_cmds.reserve(cmds);
        m_text.reserve(chars);
    }

private:
    std::vector<DrawCmd> m_cmds;
    std::vector<char> m_text;
};

/// FNV-1a 32 of an id string (0 is never returned).
std::uint32_t widgetId(std::string_view id);

class Ui {
public:
    Ui();

    /// Starts a pass over `panel` (back-buffer pixels) with `event` (may be null / None).
    void begin(const Rect& panel, std::int32_t scale, const InputEvent* event);
    /// Ends the pass: the widget registry of this pass becomes widgetRect()'s.
    void end();

    // ---- layout ----------------------------------------------------------------------------------------------
    void sameLine() { m_sameLine = true; }
    /// Blank space of `lines` lines.
    void spacing(std::int32_t lines = 1);
    /// A full-width rule.
    void separator();
    /// True while at least `lines` more lines fit in the panel.
    bool hasRoom(std::int32_t lines = 1) const;
    /// Lines left below the cursor.
    std::int32_t linesLeft() const;
    /// Characters that fit from the current position to the right edge (after sameLine: of this line).
    std::int32_t charsLeft() const;

    // ---- widgets ---------------------------------------------------------------------------------------------
    void text(std::string_view s, Color color = palette::kText);
    /// printf-style text (formatted into a fixed buffer: at most 255 characters).
    void textf(Color color, const char* format, ...)
#if defined(__GNUC__)
        __attribute__((format(printf, 3, 4)))
#endif
        ;
    /// A filled title bar with `s`.
    void title(std::string_view s);
    /// True when clicked (press and release over it). `selected` draws it highlighted.
    bool button(std::string_view id, std::string_view label, bool selected = false);
    /// A one-line text field of `widthChars` characters; true when its content changed this pass.
    bool textField(std::string_view id, std::string& value, std::int32_t widthChars, std::size_t maxLength = 64);

    // ---- input of this pass ----------------------------------------------------------------------------------
    const InputEvent& event() const { return m_event; }
    /// The virtual key of a KeyDown event not consumed by a focused text field, else 0.
    std::int32_t keyPressed() const;
    std::int32_t wheel() const { return m_event.type == EventType::Wheel ? m_event.value : 0; }
    bool mouseInPanel() const { return m_panel.contains(m_mouseX, m_mouseY); }

    // ---- results ---------------------------------------------------------------------------------------------
    const DrawList& draws() const { return m_draws; }
    /// The widget `id` of the last finished pass. False when it was not laid out (or clipped).
    bool widgetRect(std::string_view id, Rect& out) const;
    const Rect& panel() const { return m_panel; }
    std::int32_t scale() const { return m_scale; }
    std::uint32_t focused() const { return m_focus; }
    std::uint32_t widgetCount() const { return static_cast<std::uint32_t>(m_lastWidgets.size()); }

private:
    struct Widget {
        std::uint32_t id = 0;
        Rect rect;
    };
    /// Places an item `widthPx` wide on the current line; false (nothing placed) when it does not fit vertically.
    bool place(std::int32_t widthPx, Rect& out);
    /// Hot / active / clicked logic of an interactive item.
    bool interact(std::uint32_t id, const Rect& r, bool& hot);

    Rect m_panel;
    std::int32_t m_scale = 1;
    std::int32_t m_pad = 2;
    std::int32_t m_cursorX = 0, m_cursorY = 0; ///< next line's origin (or, after sameLine, this line's end)
    std::int32_t m_lineY = 0;                   ///< top of the current line
    bool m_sameLine = false;
    bool m_lineOpen = false;
    InputEvent m_event;
    std::int32_t m_mouseX = -1, m_mouseY = -1;
    bool m_mouseDown = false;
    std::uint32_t m_active = 0; ///< widget pressed and not yet released
    std::uint32_t m_focus = 0;  ///< text field receiving keys
    bool m_keyConsumed = false;
    DrawList m_draws;
    std::vector<Widget> m_widgets;
    std::vector<Widget> m_lastWidgets;
};

} // namespace fuse::relight::overlay
