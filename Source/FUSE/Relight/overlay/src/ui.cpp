// FUSE Relight RL-6.1: the developer overlay's immediate-mode UI (see ui.hpp).
#include <fuse/relight/overlay/ui.hpp>

#include <fuse/relight/overlay/font5x7.hpp>

#include <algorithm>
#include <cstdarg>
#include <cstdio>

namespace fuse::relight::overlay {

void DrawList::fill(const Rect& r, Color color) {
    if (r.empty() || (color >> 24) == 0) {
        return;
    }
    DrawCmd c;
    c.kind = DrawCmd::Kind::Fill;
    c.x = r.x;
    c.y = r.y;
    c.w = r.w;
    c.h = r.h;
    c.color = color;
    m_cmds.push_back(c);
}

void DrawList::text(std::int32_t x, std::int32_t y, std::string_view s, Color color, std::int32_t scale,
                    std::int32_t clipWidth) {
    const std::int32_t advance = kCellWidth * scale;
    const std::int32_t fit = advance > 0 ? std::max<std::int32_t>(0, (clipWidth + scale) / advance) : 0;
    const std::size_t n = std::min<std::size_t>(s.size(), static_cast<std::size_t>(fit));
    if (n == 0) {
        return;
    }
    DrawCmd c;
    c.kind = DrawCmd::Kind::Text;
    c.x = x;
    c.y = y;
    c.w = clipWidth;
    c.h = kGlyphHeight * scale;
    c.color = color;
    c.scale = scale;
    c.textBegin = static_cast<std::uint32_t>(m_text.size());
    c.textLength = static_cast<std::uint32_t>(n);
    m_text.insert(m_text.end(), s.begin(), s.begin() + static_cast<std::ptrdiff_t>(n));
    m_cmds.push_back(c);
}

std::uint32_t widgetId(std::string_view id) {
    std::uint32_t h = 2166136261u;
    for (char c : id) {
        h ^= static_cast<unsigned char>(c);
        h *= 16777619u;
    }
    return h ? h : 1u;
}

Ui::Ui() {
    m_draws.reserve(1024, 16384);
    m_widgets.reserve(512);
    m_lastWidgets.reserve(512);
}

void Ui::begin(const Rect& panel, std::int32_t scale, const InputEvent* event) {
    m_panel = panel;
    m_scale = scale < 1 ? 1 : scale;
    m_pad = 2 * m_scale;
    m_cursorX = m_panel.x + m_pad;
    m_cursorY = m_panel.y + m_pad;
    m_lineY = m_cursorY;
    m_sameLine = false;
    m_lineOpen = false;
    m_keyConsumed = false;
    m_event = event ? *event : InputEvent{};
    switch (m_event.type) {
    case EventType::MouseMove:
        m_mouseX = m_event.x;
        m_mouseY = m_event.y;
        break;
    case EventType::MouseDown:
        m_mouseX = m_event.x;
        m_mouseY = m_event.y;
        m_mouseDown = true;
        m_focus = 0; // a text field under the cursor takes it back in this pass
        break;
    case EventType::MouseUp:
        m_mouseX = m_event.x;
        m_mouseY = m_event.y;
        m_mouseDown = false;
        break;
    default:
        break;
    }
    m_draws.clear();
    m_widgets.clear();
    m_draws.fill(m_panel, palette::kPanel);
}

void Ui::end() {
    if (m_event.type == EventType::MouseUp) {
        m_active = 0;
    }
    m_lastWidgets.swap(m_widgets);
}

bool Ui::place(std::int32_t widthPx, Rect& out) {
    const std::int32_t lineH = kLineHeight * m_scale;
    const std::int32_t right = m_panel.x + m_panel.w - m_pad;
    const std::int32_t bottom = m_panel.y + m_panel.h - m_pad;
    std::int32_t x = m_panel.x + m_pad;
    std::int32_t y = m_lineOpen ? m_lineY + lineH : m_panel.y + m_pad;
    if (m_sameLine && m_lineOpen) {
        x = m_cursorX + m_scale * 2;
        y = m_lineY;
    }
    m_sameLine = false;
    const std::int32_t h = (kGlyphHeight + 2) * m_scale;
    if (y + h > bottom) {
        // The cursor moves below the panel: every later item is clipped too.
        m_lineY = y;
        m_lineOpen = true;
        m_cursorX = x;
        return false;
    }
    if (x >= right) {
        return false; // no room left on this line (sameLine): the line stays as it is
    }
    out = Rect{x, y, std::min(widthPx, right - x), h};
    m_lineY = y;
    m_lineOpen = true;
    m_cursorX = x + out.w;
    return !out.empty();
}

void Ui::spacing(std::int32_t lines) {
    for (std::int32_t i = 0; i < lines; ++i) {
        Rect r;
        place(1, r);
    }
}

void Ui::separator() {
    Rect r;
    if (place(m_panel.w, r)) {
        m_draws.fill(Rect{r.x, r.y + r.h / 2, r.w, m_scale}, palette::kDim);
    }
}

bool Ui::hasRoom(std::int32_t lines) const { return linesLeft() >= lines; }

std::int32_t Ui::linesLeft() const {
    const std::int32_t lineH = kLineHeight * m_scale;
    const std::int32_t bottom = m_panel.y + m_panel.h - m_pad;
    const std::int32_t next = m_lineOpen ? m_lineY + lineH : m_panel.y + m_pad;
    const std::int32_t h = (kGlyphHeight + 2) * m_scale;
    if (next + h > bottom) {
        return 0;
    }
    return 1 + (bottom - next - h) / lineH;
}

std::int32_t Ui::charsLeft() const {
    const std::int32_t right = m_panel.x + m_panel.w - m_pad;
    const std::int32_t x = (m_sameLine && m_lineOpen) ? m_cursorX + m_scale * 2 : m_panel.x + m_pad;
    return std::max<std::int32_t>(0, (right - x) / (kCellWidth * m_scale));
}

void Ui::text(std::string_view s, Color color) {
    Rect r;
    if (place(static_cast<std::int32_t>(s.size()) * kCellWidth * m_scale, r)) {
        m_draws.text(r.x, r.y + m_scale, s, color, m_scale, r.w);
    }
}

void Ui::textf(Color color, const char* format, ...) {
    char buffer[256];
    va_list args;
    va_start(args, format);
    const int n = std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (n < 0) {
        return;
    }
    text(std::string_view(buffer, std::min<std::size_t>(static_cast<std::size_t>(n), sizeof(buffer) - 1)), color);
}

void Ui::title(std::string_view s) {
    Rect r;
    if (place(m_panel.w, r)) {
        m_draws.fill(r, palette::kTitle);
        m_draws.text(r.x + 2 * m_scale, r.y + m_scale, s, palette::kText, m_scale, r.w - 2 * m_scale);
    }
}

bool Ui::interact(std::uint32_t id, const Rect& r, bool& hot) {
    hot = r.contains(m_mouseX, m_mouseY);
    bool clicked = false;
    if (m_event.type == EventType::MouseDown && hot) {
        m_active = id;
    } else if (m_event.type == EventType::MouseUp && m_active == id && hot) {
        clicked = true;
    }
    m_widgets.push_back(Widget{id, r});
    return clicked;
}

bool Ui::button(std::string_view idText, std::string_view label, bool selected) {
    Rect r;
    if (!place((static_cast<std::int32_t>(label.size()) * kCellWidth + 3) * m_scale, r)) {
        return false;
    }
    const std::uint32_t id = widgetId(idText);
    bool hot = false;
    const bool clicked = interact(id, r, hot);
    m_draws.fill(r, selected ? palette::kButtonOn : (hot ? palette::kButtonHot : palette::kButton));
    m_draws.text(r.x + 2 * m_scale, r.y + m_scale, label, palette::kText, m_scale, r.w - 2 * m_scale);
    return clicked;
}

bool Ui::textField(std::string_view idText, std::string& value, std::int32_t widthChars, std::size_t maxLength) {
    Rect r;
    if (!place((widthChars * kCellWidth + 3) * m_scale, r)) {
        return false;
    }
    const std::uint32_t id = widgetId(idText);
    bool hot = false;
    interact(id, r, hot);
    if (m_event.type == EventType::MouseDown && hot) {
        m_focus = id;
    }
    bool changed = false;
    if (m_focus == id) {
        if (m_event.type == EventType::Char && m_event.value >= 32 && m_event.value <= 126 && value.size() < maxLength) {
            value.push_back(static_cast<char>(m_event.value));
            changed = true;
        } else if (m_event.type == EventType::KeyDown && m_event.value == kVkBack) {
            if (!value.empty()) {
                value.pop_back();
                changed = true;
            }
            m_keyConsumed = true;
        } else if (m_event.type == EventType::KeyDown && (m_event.value == kVkReturn || m_event.value == kVkEscape)) {
            m_focus = 0;
            m_keyConsumed = true;
        }
    }
    m_draws.fill(r, m_focus == id ? palette::kButtonHot : palette::kField);
    // Show the tail when the text is longer than the field.
    const std::size_t fit = static_cast<std::size_t>(std::max<std::int32_t>(0, widthChars - (m_focus == id ? 1 : 0)));
    std::string_view shown(value);
    if (shown.size() > fit) {
        shown.remove_prefix(shown.size() - fit);
    }
    m_draws.text(r.x + 2 * m_scale, r.y + m_scale, shown, palette::kText, m_scale, r.w - 2 * m_scale);
    if (m_focus == id) {
        const std::int32_t cx = r.x + 2 * m_scale + static_cast<std::int32_t>(shown.size()) * kCellWidth * m_scale;
        m_draws.text(cx, r.y + m_scale, "_", palette::kAccent, m_scale, r.x + r.w - cx);
    }
    return changed;
}

std::int32_t Ui::keyPressed() const {
    return (m_event.type == EventType::KeyDown && !m_keyConsumed) ? m_event.value : 0;
}

bool Ui::widgetRect(std::string_view idText, Rect& out) const {
    const std::uint32_t id = widgetId(idText);
    for (const Widget& w : m_lastWidgets) {
        if (w.id == id) {
            out = w.rect;
            return true;
        }
    }
    return false;
}

} // namespace fuse::relight::overlay
