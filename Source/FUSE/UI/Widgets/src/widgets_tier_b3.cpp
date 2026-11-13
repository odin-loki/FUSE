// FUSE UI widgets that complete the plan §5 catalogue: Tier B app chrome (group box, stack, tool bar, status bar,
// main window, context menu, message box, wizard, popover, toast), the standalone Tier C header and the Tier D text
// widgets (word-wrapped text edit, plain text edit, rich text view).
#include "fuse/ui/widgets/widgets.hpp"
#include "widgets_internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace fuse::ui::widgets {
namespace {

NodeHandle make(UiContext& ui, std::string_view cls, NodeHandle parent, std::string_view id = {}) {
    auto h = ui.create(cls, parent, id);
    return h ? *h : NodeHandle{};
}
void setPx(UiContext& ui, NodeHandle n, Prop p, f32 v) { ui.set(n, p, PropValue::ofLength(Length::px(v))); }
NodeHandle nthChild(const UiContext& ui, NodeHandle parent, u32 index) {
    NodeHandle c = ui.firstChild(parent);
    for (u32 i = 0; i < index && ui.alive(c); ++i) c = ui.nextSibling(c);
    return ui.alive(c) ? c : NodeHandle{};
}
u32 count(std::string_view s) { return s.empty() ? 0u : u32(std::count(s.begin(), s.end(), '\n') + 1); }
std::string_view nth(std::string_view s, u32 index) {
    std::size_t at = 0;
    for (u32 i = 0;; ++i) {
        const std::size_t nl = s.find('\n', at);
        if (i == index) return s.substr(at, nl == std::string_view::npos ? std::string_view::npos : nl - at);
        if (nl == std::string_view::npos) return {};
        at = nl + 1;
    }
}
Color accentOf(const PaintContext& pc) { return pc.tint(pc.style.background2.a > 0.f ? pc.style.background2 : themed(pc, tok::accent, 0x3D8BFD)); }
void destroyCmd(UiContext& ui, NodeHandle n, u64, u64) {
    if (ui.alive(n)) ui.destroy(n);
}
u32 prevCp(std::string_view s, u32 c) { if (!c) return 0; --c; while (c && (u8(s[c]) & 0xC0) == 0x80) --c; return c; }
u32 nextCp(std::string_view s, u32 c) { if (c >= s.size()) return u32(s.size()); ++c; while (c < s.size() && (u8(s[c]) & 0xC0) == 0x80) ++c; return c; }

/// Per-node state pool keyed by user data (slot + 1); see widgets_tier_cd.cpp.
template <class S>
struct Pool {
    struct Slot { NodeHandle node; std::unique_ptr<S> s; };
    std::vector<Slot> slots;
    S* get(const UiContext& ui, NodeHandle n) {
        const u64 k = ui.alive(n) ? ui.userData(n) : 0;
        if (k == 0 || k > slots.size() || slots[k - 1].node != n) return nullptr;
        return slots[k - 1].s.get();
    }
    S& create(UiContext& ui, NodeHandle n) {
        for (std::size_t i = 0; i < slots.size(); ++i)
            if (!ui.alive(slots[i].node) || ui.userData(slots[i].node) != i + 1) {
                slots[i] = {n, std::make_unique<S>()};
                ui.setUserData(n, i + 1);
                return *slots[i].s;
            }
        slots.push_back({n, std::make_unique<S>()});
        ui.setUserData(n, slots.size());
        return *slots.back().s;
    }
};

// =====================================================================================================================
// Header (standalone QHeaderView): sections with widths, click to sort, drag an edge to resize
struct HeaderState {
    std::vector<f32> widths;
    i32 sortCol = -1;
    bool asc = true;
    i32 dragCol = -1;
    f32 dragStart = 0.f, dragWidth = 0.f, defaultWidth = 120.f;
};
Pool<HeaderState>& headerPool() { static Pool<HeaderState> p; return p; }
struct HeaderW {
    static constexpr std::string_view kName = "header";
    // touch: a drag on a section edge resizes it
    static TouchDrag touchDrag(UiContext& ui, NodeHandle n, Vec2) {
        const HeaderState* s = headerPool().get(ui, n);
        return {s && s->dragCol >= 0 ? TouchDrag::Mode::Capture : TouchDrag::Mode::Pan, {}, {}};
    }
    static constexpr A11yRole kRole = A11yRole::Group;
    static constexpr bool kInteractive = true;
    static constexpr f32 kGrip = 4.f;
    static f32 total(const HeaderState& s) {
        f32 w = 0.f;
        for (f32 x : s.widths) w += x;
        return w;
    }
    static Size measure(UiContext& ui, NodeHandle n, Size) {
        const HeaderState* s = headerPool().get(ui, n);
        const f32 px = ui.computed(n).fontSize;
        return {s ? total(*s) : 0.f, std::ceil(ui.font() ? ui.font()->metrics(px).lineHeight() : px * 1.25f) + 8.f};
    }
    static void paint(const PaintContext& pc, DrawList& dl) {
        const HeaderState* s = headerPool().get(pc.ui, pc.node);
        if (!s) return;
        const Rect c = pc.content;
        const Color line = pc.tint(pc.style.borderColor.a > 0.f ? pc.style.borderColor : themed(pc, tok::border, 0x474955));
        const Color text = pc.tint(pc.style.color);
        dl.pushClip(c);
        f32 x = c.x;
        for (u32 i = 0; i < s->widths.size(); ++i) {
            const f32 w = s->widths[i];
            const bool sorted = i32(i) == s->sortCol;
            pc.ui.paintString(dl, nth(pc.ui.text(pc.node), i), {x + 6.f, c.y, std::max(0.f, w - (sorted ? 26.f : 12.f)), c.h}, text,
                              pc.style.fontSize, TextAlignment::Start, pc.node.index);
            if (sorted) { // sort arrow
                const f32 ax = x + w - 14.f, ay = c.y + c.h * 0.5f, d = s->asc ? -3.f : 3.f;
                dl.line({ax - 4.f, ay - d}, {ax, ay + d}, accentOf(pc), 1.5f);
                dl.line({ax, ay + d}, {ax + 4.f, ay - d}, accentOf(pc), 1.5f);
            }
            x += w;
            dl.rect({x - 1.f, c.y + 3.f, 1.f, c.h - 6.f}, line);
        }
        dl.popClip();
        dl.rect({c.x, c.bottom() - 1.f, c.w, 1.f}, line);
    }
    /// Section under x, and whether x is on its right edge grip.
    static i32 sectionAt(const HeaderState& s, f32 x, bool& edge) {
        f32 at = 0.f;
        edge = false;
        for (u32 i = 0; i < s.widths.size(); ++i) {
            at += s.widths[i];
            if (std::abs(x - at) <= kGrip) { edge = true; return i32(i); }
            if (x < at) return i32(i);
        }
        return -1;
    }
    /// Section edges resize (and a running section drag keeps the cursor).
    static Cursor cursor(UiContext& ui, NodeHandle n, Vec2 p) {
        const HeaderState* s = headerPool().get(ui, n);
        if (!s) return Cursor::Auto;
        if (s->dragCol >= 0) return Cursor::ResizeH;
        bool edge = false;
        sectionAt(*s, p.x - ui.contentRect(n).x, edge);
        return edge ? Cursor::ResizeH : Cursor::Auto;
    }
    static bool onEvent(EventContext& ec, const Event& e) {
        if (ec.phase != Phase::Target) return false;
        UiContext& ui = ec.ui;
        HeaderState* s = headerPool().get(ui, ec.node);
        if (!s) return false;
        const f32 x = e.pos.x - ui.contentRect(ec.node).x;
        switch (e.type) {
        case EventType::PointerDown: {
            bool edge = false;
            const i32 col = sectionAt(*s, x, edge);
            if (col < 0) return true;
            if (edge) {
                s->dragCol = col;
                s->dragStart = x;
                s->dragWidth = s->widths[std::size_t(col)];
            } else {
                s->asc = s->sortCol == col ? !s->asc : true;
                s->sortCol = col;
                ui.invalidate(ec.node);
                ui.emit(ec.node, SignalKind::Toggled, {.value = f32(col), .flag = s->asc});
            }
            return true;
        }
        case EventType::PointerMove:
            if (s->dragCol < 0) return false;
            setHeaderSectionWidth(ui, ec.node, u32(s->dragCol), s->dragWidth + (x - s->dragStart));
            return true;
        case EventType::PointerUp: s->dragCol = -1; return true;
        default: return false;
        }
    }
};

// =====================================================================================================================
// Text edit: multi-line, word wrapped (QTextEdit / QPlainTextEdit with wrapping)
struct TextEditState {
    std::string text;
    u32 caret = 0, anchor = 0; ///< byte offsets
    f32 scroll = 0.f;          ///< px
    f32 wantX = -1.f;          ///< column kept by Up / Down
    bool dragging = false, readOnly = false;
    std::vector<std::pair<std::string, u32>> undo, redo;
    // wrap cache
    struct Line { u32 start, end; }; ///< [start, end) bytes; the '\n' (if any) is not part of a line
    std::vector<Line> lines;
    f32 wrapWidth = -1.f, wrapFont = 0.f;
    bool wrapDirty = true;
};
Pool<TextEditState>& textPool() { static Pool<TextEditState> p; return p; }
struct TextEditW {
    static constexpr std::string_view kName = "textedit";
    // touch: a drag scrolls the text pixel for pixel (drag selection stays a mouse feature)
    static TouchDrag touchDrag(UiContext& ui, NodeHandle n, Vec2) {
        TextEditState* s = textPool().get(ui, n);
        if (!s) return {};
        wrap(ui, n, *s);
        const f32 lh = lineH(ui, n);
        if (f32(s->lines.size()) * lh <= ui.contentRect(n).h) return {};
        return {TouchDrag::Mode::Wheel, {0.f, 1.f}, {0.f, 1.f / (3.f * lh)}};
    }
    static constexpr A11yRole kRole = A11yRole::TextField;
    static constexpr bool kFocusable = true;
    static constexpr bool kInteractive = true;
    static Size measure(UiContext&, NodeHandle, Size) { return {320.f, 140.f}; }
    static f32 lineH(UiContext& ui, NodeHandle n) {
        const f32 px = ui.computed(n).fontSize;
        return std::ceil(ui.font() ? ui.font()->metrics(px).lineHeight() : px * 1.25f) + 2.f;
    }
    /// Greedy word wrap of every paragraph into visual lines for the content width.
    static void wrap(UiContext& ui, NodeHandle n, TextEditState& s) {
        const f32 width = std::max(8.f, ui.contentRect(n).w - 4.f), fs = ui.computed(n).fontSize;
        if (!s.wrapDirty && width == s.wrapWidth && fs == s.wrapFont) return;
        s.wrapDirty = false;
        s.wrapWidth = width;
        s.wrapFont = fs;
        s.lines.clear();
        const std::string_view t = s.text;
        u32 at = 0;
        for (;;) {
            const std::size_t nl = t.find('\n', at);
            const u32 pend = nl == std::string_view::npos ? u32(t.size()) : u32(nl);
            u32 ls = at;
            while (ls < pend) {
                // the longest prefix of [ls, pend) that fits, cut at the last space when one exists
                u32 end = ls, lastSpace = 0;
                bool have = false;
                while (end < pend) {
                    const u32 nx = nextCp(t, end);
                    if (ui.measureString(t.substr(ls, nx - ls), fs) > width && end > ls) break;
                    if (t[end] == ' ') { lastSpace = nx; have = true; }
                    end = nx;
                }
                if (end < pend && have) end = lastSpace;
                s.lines.push_back({ls, end});
                ls = end;
            }
            if (pend == at) s.lines.push_back({at, at}); // empty paragraph
            if (nl == std::string_view::npos) break;
            at = pend + 1;
        }
        if (s.lines.empty()) s.lines.push_back({0, 0});
    }
    static u32 lineOf(const TextEditState& s, u32 pos) {
        for (u32 i = 0; i < s.lines.size(); ++i) {
            const auto& l = s.lines[i];
            // a caret at a soft-wrap point belongs to the next line; at a paragraph end to this one
            const bool last = i + 1 == s.lines.size() || s.lines[i + 1].start != l.end;
            if (pos >= l.start && (pos < l.end || (pos == l.end && last))) return i;
        }
        return u32(s.lines.size()) - 1;
    }
    static f32 xOf(UiContext& ui, NodeHandle n, const TextEditState& s, u32 line, u32 pos) {
        const auto& l = s.lines[line];
        return ui.measureString(std::string_view(s.text).substr(l.start, std::min(pos, l.end) - l.start), ui.computed(n).fontSize);
    }
    static u32 posAt(UiContext& ui, NodeHandle n, const TextEditState& s, u32 line, f32 x) {
        const auto& l = s.lines[line];
        const std::string_view t = s.text;
        const f32 fs = ui.computed(n).fontSize;
        u32 p = l.start;
        while (p < l.end) {
            const u32 nx = nextCp(t, p);
            const f32 mid = (ui.measureString(t.substr(l.start, p - l.start), fs) + ui.measureString(t.substr(l.start, nx - l.start), fs)) * 0.5f;
            if (x < mid) break;
            p = nx;
        }
        // stay on this visual line: the position after a soft-wrapped line's last character is the next line's start
        if (p == l.end && line + 1 < s.lines.size() && s.lines[line + 1].start == l.end && p > l.start) p = prevCp(t, p);
        return p;
    }
    static u32 hit(UiContext& ui, NodeHandle n, TextEditState& s, Vec2 p) {
        wrap(ui, n, s);
        const Rect c = ui.contentRect(n);
        const i64 line = std::clamp<i64>(i64(std::floor((p.y - c.y + s.scroll) / lineH(ui, n))), 0, i64(s.lines.size()) - 1);
        return posAt(ui, n, s, u32(line), p.x - c.x - 2.f);
    }
    static void reveal(UiContext& ui, NodeHandle n, TextEditState& s) {
        wrap(ui, n, s);
        const f32 lh = lineH(ui, n), h = ui.contentRect(n).h;
        const f32 y = f32(lineOf(s, s.caret)) * lh;
        if (y < s.scroll) s.scroll = y;
        else if (y + lh > s.scroll + h) s.scroll = y + lh - h;
        s.scroll = std::clamp(s.scroll, 0.f, std::max(0.f, f32(s.lines.size()) * lh - h));
    }
    static void publish(UiContext& ui, NodeHandle n, TextEditState& s, bool edited) {
        if (edited) {
            s.wrapDirty = true;
            ui.setText(n, s.text);
        }
        reveal(ui, n, s);
        if (edited) ui.emit(n, SignalKind::TextChanged, {.text = s.text});
        ui.invalidate(n);
    }
    static void snapshot(TextEditState& s) {
        s.undo.emplace_back(s.text, s.caret);
        if (s.undo.size() > 128) s.undo.erase(s.undo.begin());
        s.redo.clear();
    }
    static bool eraseSel(TextEditState& s) {
        if (s.caret == s.anchor) return false;
        const u32 a = std::min(s.caret, s.anchor), b = std::max(s.caret, s.anchor);
        s.text.erase(a, b - a);
        s.caret = s.anchor = a;
        return true;
    }
    static void insert(TextEditState& s, std::string_view t) {
        eraseSel(s);
        s.text.insert(s.caret, t);
        s.caret = s.anchor = s.caret + u32(t.size());
    }
    static void paint(const PaintContext& pc, DrawList& dl) {
        UiContext& ui = pc.ui;
        TextEditState* sp = textPool().get(ui, pc.node);
        if (!sp) return;
        TextEditState& s = *sp;
        wrap(ui, pc.node, s);
        const Rect c = pc.content;
        const f32 lh = lineH(ui, pc.node), fs = pc.style.fontSize;
        const bool focused = ui.focused() == pc.node;
        const u32 a = std::min(s.caret, s.anchor), b = std::max(s.caret, s.anchor);
        const u32 first = u32(std::max(0.f, std::floor(s.scroll / lh)));
        const u32 caretLine = lineOf(s, s.caret);
        const std::string_view comp = focused ? ui.composition() : std::string_view{};
        dl.pushClip(c);
        for (u32 i = first; i < s.lines.size(); ++i) {
            const f32 y = c.y + f32(i) * lh - s.scroll;
            if (y > c.bottom()) break;
            const auto& l = s.lines[i];
            const f32 x0 = c.x + 2.f;
            if (a != b && b > l.start && a <= l.end) { // selection
                const f32 sx = xOf(ui, pc.node, s, i, std::max(a, l.start)), ex = xOf(ui, pc.node, s, i, std::min(b, l.end));
                const f32 tail = b > l.end ? 5.f : 0.f; // the line break is selected too
                if (ex + tail > sx) dl.rect({x0 + sx, y, ex - sx + tail, lh}, accentOf(pc).withAlpha(0.45f * pc.opacity));
            }
            ui.paintString(dl, std::string_view(s.text).substr(l.start, l.end - l.start), {x0, y, c.w + 400.f, lh}, pc.tint(pc.style.color), fs,
                           TextAlignment::Start, pc.node.index);
            if (focused && i == caretLine) {
                f32 cx = x0 + xOf(ui, pc.node, s, i, s.caret);
                if (!comp.empty()) { // IME pre-edit inline at the caret, underlined
                    const f32 cw = ui.measureString(comp, fs);
                    ui.paintString(dl, comp, {cx, y, cw + 4.f, lh}, pc.tint(pc.style.color), fs, TextAlignment::Start, pc.node.index);
                    dl.rect({cx, y + lh - 2.f, cw, 1.f}, pc.tint(pc.style.color));
                    cx += ui.measureString(comp.substr(0, std::size_t(std::clamp(ui.compositionCaret(), 0, i32(comp.size())))), fs);
                }
                dl.rect({cx, y + 2.f, 1.5f, lh - 4.f}, pc.tint(pc.style.color));
                ui.setTextCaret(pc.node, {cx, y + 2.f, 1.5f, lh - 4.f});
            }
        }
        dl.popClip();
        const f32 total = f32(s.lines.size()) * lh;
        if (total > c.h) { // scroll indicator
            const f32 th = std::max(14.f, c.h * c.h / total);
            dl.rect({c.right() - 4.f, c.y + (c.h - th) * s.scroll / (total - c.h), 3.f, th}, pc.tint(pc.style.color).withAlpha(0.35f * pc.opacity), 1.5f);
        }
    }
    static bool onEvent(EventContext& ec, const Event& e) {
        if (ec.phase != Phase::Target) return false;
        UiContext& ui = ec.ui;
        const NodeHandle n = ec.node;
        TextEditState* sp = textPool().get(ui, n);
        if (!sp) return false;
        TextEditState& s = *sp;
        const bool shift = e.mods & kModShift, ctrl = e.mods & kModCtrl;
        switch (e.type) {
        case EventType::PointerDown:
            ui.setFocus(n, FocusReason::Pointer);
            s.caret = hit(ui, n, s, e.pos);
            if (!shift) s.anchor = s.caret;
            s.dragging = true;
            s.wantX = -1.f;
            publish(ui, n, s, false);
            return true;
        case EventType::PointerMove:
            if (!s.dragging) return false;
            s.caret = hit(ui, n, s, e.pos);
            publish(ui, n, s, false);
            return true;
        case EventType::PointerUp: s.dragging = false; return true;
        case EventType::Wheel: {
            wrap(ui, n, s);
            const f32 lh = lineH(ui, n);
            s.scroll = std::clamp(s.scroll - e.wheel.y * 3.f * lh, 0.f, std::max(0.f, f32(s.lines.size()) * lh - ui.contentRect(n).h));
            ui.invalidate(n);
            return true;
        }
        case EventType::Composition: ui.invalidate(n); return true;
        case EventType::FocusIn:
        case EventType::FocusOut: ui.invalidate(n); return false;
        case EventType::Text:
            if (e.text.empty() || ctrl || s.readOnly) return false;
            snapshot(s);
            insert(s, e.text);
            s.wantX = -1.f;
            publish(ui, n, s, true);
            return true;
        case EventType::KeyDown: break;
        default: return false;
        }
        wrap(ui, n, s);
        auto move = [&](u32 p, bool keepX = false) {
            s.caret = p;
            if (!shift) s.anchor = p;
            if (!keepX) s.wantX = -1.f;
            publish(ui, n, s, false);
            return true;
        };
        const u32 line = lineOf(s, s.caret);
        switch (e.key) {
        case Key::Left:
            if (!shift && s.caret != s.anchor) return move(std::min(s.caret, s.anchor));
            return move(prevCp(s.text, s.caret));
        case Key::Right:
            if (!shift && s.caret != s.anchor) return move(std::max(s.caret, s.anchor));
            return move(nextCp(s.text, s.caret));
        case Key::Up: case Key::Down: case Key::PageUp: case Key::PageDown: {
            const i64 page = std::max<i64>(1, i64(ui.contentRect(n).h / lineH(ui, n)) - 1);
            const i64 d = e.key == Key::Up ? -1 : e.key == Key::Down ? 1 : e.key == Key::PageUp ? -page : page;
            const i64 to = i64(line) + d;
            if (s.wantX < 0.f) s.wantX = xOf(ui, n, s, line, s.caret);
            if (to < 0) return move(0, true);
            if (to >= i64(s.lines.size())) return move(u32(s.text.size()), true);
            return move(posAt(ui, n, s, u32(to), s.wantX), true);
        }
        case Key::Home: return move(ctrl ? 0u : s.lines[line].start);
        case Key::End: {
            if (ctrl) return move(u32(s.text.size()));
            const auto& l = s.lines[line];
            const bool soft = line + 1 < s.lines.size() && s.lines[line + 1].start == l.end && l.end > l.start;
            return move(soft ? prevCp(s.text, l.end) : l.end);
        }
        case Key::A:
            if (!ctrl) return false;
            s.anchor = 0;
            s.caret = u32(s.text.size());
            publish(ui, n, s, false);
            return true;
        case Key::C: case Key::X: {
            if (!ctrl) return false;
            if (s.caret == s.anchor) return true;
            const u32 a = std::min(s.caret, s.anchor), b = std::max(s.caret, s.anchor);
            ui.setClipboardText(std::string_view(s.text).substr(a, b - a));
            if (e.key == Key::X && !s.readOnly) {
                snapshot(s);
                eraseSel(s);
                publish(ui, n, s, true);
            }
            return true;
        }
        case Key::V: {
            if (!ctrl || s.readOnly) return false;
            std::string clip;
            if (!ui.clipboardText(clip) || clip.empty()) return true;
            snapshot(s);
            insert(s, clip);
            publish(ui, n, s, true);
            return true;
        }
        case Key::Z: case Key::Y: {
            if (!ctrl || s.readOnly) return false;
            auto& from = (e.key == Key::Z && !shift) ? s.undo : s.redo;
            auto& to = (e.key == Key::Z && !shift) ? s.redo : s.undo;
            if (from.empty()) return true;
            to.emplace_back(s.text, s.caret);
            s.text = std::move(from.back().first);
            s.caret = s.anchor = std::min(from.back().second, u32(s.text.size()));
            from.pop_back();
            publish(ui, n, s, true);
            return true;
        }
        default: break;
        }
        if (s.readOnly) return false;
        switch (e.key) {
        case Key::Enter:
            snapshot(s);
            insert(s, "\n");
            publish(ui, n, s, true);
            return true;
        case Key::Backspace:
        case Key::Delete: {
            if (s.caret == s.anchor) {
                const u32 to = e.key == Key::Backspace ? prevCp(s.text, s.caret) : nextCp(s.text, s.caret);
                if (to == s.caret) return true;
                s.anchor = to;
            }
            snapshot(s);
            eraseSel(s);
            s.wantX = -1.f;
            publish(ui, n, s, true);
            return true;
        }
        default: return false;
        }
    }
};

// =====================================================================================================================
// Rich text view: a small inline markup, word wrapped
struct RichRun {
    std::string text;
    Color color;       ///< a == 0: the node's colour
    f32 size = 0.f;    ///< 0: the node's font size
    bool bold = false, italic = false, underline = false, strike = false, lineBreak = false;
    i32 link = -1;     ///< index into RichState::links
};
struct RichState {
    std::vector<RichRun> runs;
    std::vector<std::string> links;
    struct Piece { u32 run; u32 start, len; f32 x, y, w, h, size; };
    std::vector<Piece> pieces;
    f32 layoutWidth = -1.f, layoutFont = 0.f, height = 0.f;
    u32 layoutFace = 0; ///< weight / slant / family of the node's style at the last layout
    bool dirty = true;
    i32 hoverLink = -1;
};
Pool<RichState>& richPool() { static Pool<RichState> p; return p; }
/// The face of a run: the node's family with <b> as weight 700 (or the node's weight when heavier) and <i> as italic.
/// A real bold / italic face of the family when there is one, otherwise synthesised. Null without a context font.
const fuse::text::Font* richRunFont(UiContext& ui, const ComputedStyle& st, const RichRun& run) {
    return ui.fontFor(std::span<const u32>(st.fontFamily), run.bold ? std::max<int>(st.fontWeight, 700) : int(st.fontWeight),
                      run.italic || st.fontItalic);
}

bool parseHexColor(std::string_view v, Color& out) {
    if (!v.empty() && v[0] == '#') v.remove_prefix(1);
    if (v.size() != 6 && v.size() != 8) return false;
    u32 x = 0;
    for (char ch : v) {
        const int d = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
        if (d < 0) return false;
        x = (x << 4) | u32(d);
    }
    out = v.size() == 6 ? Color::rgb(x) : Color::rgb(x >> 8, f32(x & 0xFFu) / 255.f);
    return true;
}
/// <b> <i> <u> <s> <color=#RRGGBB[AA]> <size=N> <a href="..."> <br>; &lt; &gt; &amp;. Unknown tags are dropped.
void parseRichMarkup(std::string_view src, RichState& out) {
    out.runs.clear();
    out.links.clear();
    RichRun cur;
    std::vector<RichRun> stack; // style before each open tag
    std::vector<std::string> names;
    auto flush = [&] {
        if (!cur.text.empty()) out.runs.push_back(cur);
        cur.text.clear();
    };
    for (std::size_t i = 0; i < src.size();) {
        const char ch = src[i];
        if (ch == '&') {
            static constexpr std::pair<std::string_view, char> kEnt[] = {{"&lt;", '<'}, {"&gt;", '>'}, {"&amp;", '&'}, {"&quot;", '"'}};
            bool hitEnt = false;
            for (const auto& [name, c] : kEnt)
                if (src.substr(i, name.size()) == name) { cur.text += c; i += name.size(); hitEnt = true; break; }
            if (hitEnt) continue;
        }
        if (ch == '\n') {
            flush();
            RichRun br = cur;
            br.lineBreak = true;
            out.runs.push_back(br);
            ++i;
            continue;
        }
        const std::size_t close = ch == '<' ? src.find('>', i) : std::string_view::npos;
        if (close == std::string_view::npos) { cur.text += ch; ++i; continue; }
        std::string_view tag = src.substr(i + 1, close - i - 1);
        i = close + 1;
        if (!tag.empty() && tag[0] == '/') {
            tag.remove_prefix(1);
            for (std::size_t k = names.size(); k-- > 0;)
                if (names[k] == tag) { // restore the style from before the matching open tag
                    flush();
                    const std::string keep = std::move(cur.text);
                    cur = stack[k];
                    cur.text = keep;
                    stack.resize(k);
                    names.resize(k);
                    break;
                }
            continue;
        }
        const std::size_t sep = tag.find_first_of(" =");
        const std::string_view name = tag.substr(0, sep);
        std::string_view arg = sep == std::string_view::npos ? std::string_view{} : tag.substr(sep + 1);
        if (const std::size_t eq = arg.find('='); name == "a" && eq != std::string_view::npos) arg = arg.substr(eq + 1);
        while (!arg.empty() && (arg.front() == '"' || arg.front() == '\'' || arg.front() == ' ')) arg.remove_prefix(1);
        while (!arg.empty() && (arg.back() == '"' || arg.back() == '\'' || arg.back() == ' ' || arg.back() == '/')) arg.remove_suffix(1);
        if (name == "br" || name == "br/") {
            flush();
            RichRun br = cur;
            br.lineBreak = true;
            out.runs.push_back(br);
            continue;
        }
        const bool known = name == "b" || name == "i" || name == "u" || name == "s" || name == "color" || name == "size" || name == "a";
        if (!known) continue;
        flush();
        RichRun before = cur;
        before.text.clear();
        stack.push_back(before);
        names.emplace_back(name);
        if (name == "b") cur.bold = true;
        else if (name == "i") cur.italic = true;
        else if (name == "u") cur.underline = true;
        else if (name == "s") cur.strike = true;
        else if (name == "color") parseHexColor(arg, cur.color);
        else if (name == "size") cur.size = std::clamp(f32(std::atof(std::string(arg).c_str())), 6.f, 96.f);
        else if (name == "a") {
            cur.link = i32(out.links.size());
            out.links.emplace_back(arg);
            cur.underline = true;
        }
    }
    flush();
    out.dirty = true;
}
struct RichTextW {
    static constexpr std::string_view kName = "richtext";
    static constexpr A11yRole kRole = A11yRole::Text;
    static constexpr bool kInteractive = true;
    static void layout(UiContext& ui, NodeHandle n, RichState& s, f32 width) {
        const ComputedStyle st = ui.computed(n);
        const f32 base = st.fontSize;
        const u32 face = u32(st.fontWeight) | (st.fontItalic ? 0x10000u : 0u) | (st.fontFamily[0] << 8);
        if (!s.dirty && width == s.layoutWidth && base == s.layoutFont && face == s.layoutFace) return;
        s.dirty = false;
        s.layoutWidth = width;
        s.layoutFont = base;
        s.layoutFace = face;
        s.pieces.clear();
        auto lineHeight = [&](f32 px) { return std::ceil(ui.font() ? ui.font()->metrics(px).lineHeight() : px * 1.25f); };
        f32 x = 0.f, y = 0.f, lineH = lineHeight(base);
        std::size_t lineStart = 0;
        auto newLine = [&] {
            for (std::size_t k = lineStart; k < s.pieces.size(); ++k) { // bottom-align mixed sizes on the line
                s.pieces[k].y = y + (lineH - s.pieces[k].h);
            }
            y += lineH;
            x = 0.f;
            lineH = lineHeight(base);
            lineStart = s.pieces.size();
        };
        for (u32 r = 0; r < s.runs.size(); ++r) {
            const RichRun& run = s.runs[r];
            if (run.lineBreak) { newLine(); continue; }
            const f32 px = run.size > 0.f ? run.size : base;
            const f32 h = lineHeight(px);
            const std::string_view t = run.text;
            const fuse::text::Font* rf = richRunFont(ui, st, run);
            auto measureRun = [&](std::string_view w) { return rf ? ui.measureString(*rf, w, px) : 0.f; };
            for (std::size_t at = 0; at < t.size();) { // words keep their trailing spaces
                std::size_t end = at;
                while (end < t.size() && t[end] != ' ') ++end;
                while (end < t.size() && t[end] == ' ') ++end;
                const std::string_view word = t.substr(at, end - at);
                std::size_t vis = word.size();
                while (vis > 0 && word[vis - 1] == ' ') --vis;
                const f32 w = measureRun(word), wVis = measureRun(word.substr(0, vis));
                if (x > 0.f && x + wVis > width) newLine();
                lineH = std::max(lineH, h);
                s.pieces.push_back({r, u32(at), u32(word.size()), x, y, w, h, px});
                x += w;
                at = end;
            }
        }
        newLine();
        s.height = y;
    }
    static Size measure(UiContext& ui, NodeHandle n, Size avail) {
        RichState* s = richPool().get(ui, n);
        if (!s) return {};
        const f32 width = std::isfinite(avail.w) && avail.w > 0.f ? avail.w : 100000.f;
        layout(ui, n, *s, width);
        f32 w = 0.f;
        for (const auto& p : s->pieces) w = std::max(w, p.x + p.w);
        return {std::min(w, width), s->height};
    }
    static void paint(const PaintContext& pc, DrawList& dl) {
        RichState* s = richPool().get(pc.ui, pc.node);
        if (!s) return;
        layout(pc.ui, pc.node, *s, std::max(1.f, pc.content.w));
        const Rect c = pc.content;
        for (const auto& p : s->pieces) {
            const RichRun& run = s->runs[p.run];
            Color col = run.color.a > 0.f ? run.color : run.link >= 0 ? themed(pc, tok::accent_hi, 0x5A9DFF) : pc.style.color;
            if (run.link >= 0 && run.link == s->hoverLink) col = themed(pc, tok::link_hover, 0x8FBCFF);
            col = pc.tint(col);
            const std::string_view word = std::string_view(run.text).substr(p.start, p.len);
            const Rect box{c.x + p.x, c.y + p.y, p.w + 2.f, p.h};
            const fuse::text::Font* rf = richRunFont(pc.ui, pc.style, run);
            if (!rf) continue;
            pc.ui.paintString(dl, *rf, word, box, col, p.size, TextAlignment::Start, pc.node.index);
            if (run.underline || run.strike) {
                // rules run across the piece (its trailing spaces included, so an underlined phrase is continuous) at
                // the font's underline / strike-through position
                const auto dm = rf->decorationMetrics(p.size);
                const f32 baseline = std::round(box.y) + std::ceil(rf->metrics(p.size).ascent);
                const f32 x0 = std::round(box.x), x1 = std::round(box.x + p.w);
                if (run.underline) dl.rect({x0, baseline + dm.underlineOffset, x1 - x0, dm.underlineThickness}, col);
                if (run.strike) dl.rect({x0, baseline + dm.strikeOffset, x1 - x0, dm.strikeThickness}, col);
            }
        }
    }
    static i32 linkAt(UiContext& ui, NodeHandle n, const RichState& s, Vec2 pos) {
        const Rect c = ui.contentRect(n);
        for (const auto& p : s.pieces)
            if (s.runs[p.run].link >= 0 && pos.x >= c.x + p.x && pos.x < c.x + p.x + p.w && pos.y >= c.y + p.y && pos.y < c.y + p.y + p.h)
                return s.runs[p.run].link;
        return -1;
    }
    static bool onEvent(EventContext& ec, const Event& e) {
        if (ec.phase != Phase::Target) return false;
        RichState* s = richPool().get(ec.ui, ec.node);
        if (!s) return false;
        if (e.type == EventType::PointerMove || e.type == EventType::PointerLeave) {
            const i32 l = e.type == EventType::PointerLeave ? -1 : linkAt(ec.ui, ec.node, *s, e.pos);
            if (l != s->hoverLink) {
                s->hoverLink = l;
                ec.ui.invalidate(ec.node);
            }
            return false;
        }
        if (e.type == EventType::Click) {
            const i32 l = linkAt(ec.ui, ec.node, *s, e.pos);
            if (l < 0) return false;
            ec.ui.emit(ec.node, SignalKind::Submitted, {.value = f32(l), .text = s->links[std::size_t(l)]});
            return true;
        }
        return false;
    }
};

// =====================================================================================================================
// Toast, status bar message expiry
struct ToastW {
    static constexpr std::string_view kName = "toast";
    static constexpr A11yRole kRole = A11yRole::Text;
    static ComputedStyle defaults() {
        ComputedStyle s;
        s.pointerEvents = false;
        return s;
    }
};
/// Body of a group box. While the (checkable) group is unchecked the body is disabled: it swallows input on its way
/// down to the children (capture phase), since disabling a container does not disable what is inside it.
struct GroupBodyW {
    static constexpr std::string_view kName = "groupbody";
    static constexpr A11yRole kRole = A11yRole::Group;
    static bool onEvent(EventContext& ec, const Event& e) {
        if (ec.phase != Phase::Capture || !ec.ui.disabled(ec.node)) return false;
        switch (e.type) {
        case EventType::PointerDown: case EventType::PointerUp: case EventType::Click: case EventType::Wheel:
        case EventType::KeyDown: case EventType::KeyUp: case EventType::Text: case EventType::Composition: return true;
        default: return false;
        }
    }
};
/// A wizard page: a panel whose text() is the page title for the step line. The core paints the text of a class that
/// has no paint function, which would put the title over the page's content, so this class paints nothing itself.
struct WizardPageW {
    static constexpr std::string_view kName = "wizardpage";
    static constexpr A11yRole kRole = A11yRole::Group;
    static void paint(const PaintContext&, DrawList&) {}
};
constexpr std::string_view kToastHostId = "__fuse_ui_toasts";

void showStackPage(UiContext& ui, NodeHandle stack, u32 current) {
    u32 i = 0;
    for (NodeHandle p = ui.firstChild(stack); ui.alive(p); p = ui.nextSibling(p), ++i)
        ui.setCss(p, "display", i == current ? "flex" : "none");
}

// wizard: card children = title, step label, pages stack, button row (Back, Next, Cancel)
NodeHandle wizardPages(const UiContext& ui, NodeHandle wiz) { return nthChild(ui, wiz, 2); }
NodeHandle wizardButtons(const UiContext& ui, NodeHandle wiz) { return nthChild(ui, wiz, 3); }
void wizardShow(UiContext& ui, NodeHandle wiz, u32 index) {
    const NodeHandle pages = wizardPages(ui, wiz);
    const u32 n = ui.childCount(pages);
    if (!n) return;
    index = std::min(index, n - 1);
    ui.setValue(wiz, f32(index));
    showStackPage(ui, pages, index);
    const NodeHandle page = nthChild(ui, pages, index);
    char step[48];
    std::snprintf(step, sizeof step, "Step %u of %u", index + 1, n);
    std::string head(step);
    if (const std::string_view t = ui.text(page); !t.empty()) { head += " \xE2\x80\x94 "; head += t; }
    ui.setText(nthChild(ui, wiz, 1), head);
    const NodeHandle row = wizardButtons(ui, wiz);
    ui.setDisabled(nthChild(ui, row, 0), index == 0);
    ui.setText(nthChild(ui, row, 1), index + 1 == n ? "Finish" : "Next");
}

} // namespace

void registerTierB3(UiContext& ui) {
    WidgetRegistry& r = ui.registry();
    if (r.find("textedit") != 0xFFFFFFFFu) return;
    r.registerWidget<HeaderW>();
    r.registerWidget<TextEditW>();
    r.registerWidget<RichTextW>();
    r.registerWidget<ToastW>();
    r.registerWidget<GroupBodyW>();
    r.registerWidget<WizardPageW>();
    detail::a11yTierB3(r);
}

// ---- group box ---------------------------------------------------------------------------------------------------------
NodeHandle groupBox(UiContext& ui, NodeHandle parent, std::string_view title, bool checkable, bool checked, std::string_view id) {
    registerTierB3(ui);
    const NodeHandle g = panel(ui, parent, id);
    ui.addClass(g, "groupbox");
    ui.setA11y(g, A11yRole::Group, title);
    NodeHandle head;
    if (checkable) {
        head = checkBox(ui, g, title, checked);
    } else {
        head = label(ui, g, title);
    }
    ui.addClass(head, "grouptitle");
    const NodeHandle body = make(ui, "groupbody", g);
    if (checkable) {
        ui.setChecked(g, checked);
        auto apply = [](UiContext& c, NodeHandle b, bool on) {
            c.setDisabled(b, !on);
            c.setCss(b, "opacity", on ? "1" : "0.45");
        };
        apply(ui, body, checked);
        ui.connect(head, SignalKind::Toggled, [g, body, apply](UiContext& c, NodeHandle, const SignalArgs& a) {
            if (!c.alive(g) || !c.alive(body)) return;
            c.setChecked(g, a.flag);
            apply(c, body, a.flag);
            c.emit(g, SignalKind::Toggled, {.flag = a.flag});
        });
    }
    return g;
}
NodeHandle groupBoxBody(const UiContext& ui, NodeHandle group) { return nthChild(ui, group, 1); }

// ---- stack --------------------------------------------------------------------------------------------------------------
NodeHandle stack(UiContext& ui, NodeHandle parent, std::string_view id) {
    const NodeHandle s = panel(ui, parent, id);
    ui.addClass(s, "stack");
    ui.setRange(s, 0.f, 1e9f, 1.f);
    return s;
}
NodeHandle stackAddPage(UiContext& ui, NodeHandle st, std::string_view id) {
    const NodeHandle p = panel(ui, st, id);
    ui.setCss(p, "flex-grow", "1");
    showStackPage(ui, st, u32(ui.value(st)));
    return p;
}
void setStackIndex(UiContext& ui, NodeHandle st, u32 index) {
    const u32 n = ui.childCount(st);
    if (!n) return;
    index = std::min(index, n - 1);
    const bool changed = u32(ui.value(st)) != index;
    ui.setValue(st, f32(index));
    showStackPage(ui, st, index);
    if (changed) ui.emit(st, SignalKind::ValueChanged, {.value = f32(index)});
}
u32 stackIndex(const UiContext& ui, NodeHandle st) { return u32(ui.value(st)); }
u32 stackCount(const UiContext& ui, NodeHandle st) { return ui.childCount(st); }
NodeHandle stackPage(const UiContext& ui, NodeHandle st, u32 index) { return nthChild(ui, st, index); }

// ---- tool bar / status bar --------------------------------------------------------------------------------------------
NodeHandle toolBar(UiContext& ui, NodeHandle parent, std::string_view id) {
    const NodeHandle b = panel(ui, parent, id);
    ui.addClass(b, "toolbar");
    ui.setA11y(b, A11yRole::ToolBar, "Tool bar");
    return b;
}
NodeHandle toolBarAction(UiContext& ui, NodeHandle bar, std::string_view text, std::string_view tooltip, std::string_view id) {
    const NodeHandle a = toolButton(ui, bar, text, id);
    if (!tooltip.empty()) setTooltip(ui, a, tooltip);
    return a;
}
NodeHandle toolBarSeparator(UiContext& ui, NodeHandle bar) {
    const NodeHandle s = panel(ui, bar);
    ui.addClass(s, "toolsep");
    return s;
}

NodeHandle statusBar(UiContext& ui, NodeHandle parent, std::string_view id) {
    const NodeHandle b = panel(ui, parent, id);
    ui.addClass(b, "statusbar");
    ui.setA11y(b, A11yRole::StatusBar);
    const NodeHandle msg = label(ui, b, "");
    ui.addClass(msg, "statusmsg");
    ui.setA11yLive(msg, A11yLive::Polite); // a new message is announced
    ui.setCss(msg, "flex-grow", "1");
    return b;
}
void statusMessage(UiContext& ui, NodeHandle bar, std::string_view text, f32 timeoutSeconds) {
    const NodeHandle msg = ui.firstChild(bar);
    if (!ui.alive(msg)) return;
    ui.setText(msg, text);
    const u64 serial = ui.userData(msg) + 1; // a later message outlives an earlier message's timer
    ui.setUserData(msg, serial);
    if (timeoutSeconds > 0.f)
        ui.startTimer(timeoutSeconds, false, [msg, serial](UiContext& c) {
            if (c.alive(msg) && c.userData(msg) == serial) c.setText(msg, "");
        });
}
std::string_view statusText(const UiContext& ui, NodeHandle bar) { return ui.text(ui.firstChild(bar)); }
NodeHandle statusBarPermanent(UiContext& ui, NodeHandle bar, std::string_view text, std::string_view id) {
    const NodeHandle l = label(ui, bar, text, id);
    ui.addClass(l, "statusperm");
    return l;
}

// ---- main window ------------------------------------------------------------------------------------------------------
NodeHandle mainWindow(UiContext& ui, NodeHandle parent, std::string_view id) {
    const NodeHandle w = panel(ui, parent, id);
    ui.addClass(w, "mainwindow");
    ui.setCss(w, "flex-grow", "1");
    ui.setA11y(w, A11yRole::Window);
    menuBar(ui, w);
    toolBar(ui, w);
    const NodeHandle docks = dockArea(ui, w);
    ui.setCss(docks, "flex-grow", "1");
    statusBar(ui, w);
    return w;
}
NodeHandle mainWindowMenuBar(const UiContext& ui, NodeHandle w) { return nthChild(ui, w, 0); }
NodeHandle mainWindowToolBar(const UiContext& ui, NodeHandle w) { return nthChild(ui, w, 1); }
NodeHandle mainWindowDockArea(const UiContext& ui, NodeHandle w) { return nthChild(ui, w, 2); }
NodeHandle mainWindowCentral(const UiContext& ui, NodeHandle w) { return dockRegion(ui, mainWindowDockArea(ui, w), DockSide::Center); }
NodeHandle mainWindowStatusBar(const UiContext& ui, NodeHandle w) { return nthChild(ui, w, 3); }

// ---- context menu -----------------------------------------------------------------------------------------------------
void setContextMenu(UiContext& ui, NodeHandle node, std::string_view items) {
    registerTierB(ui);
    // Slots hold trivially copyable captures only: the item list lives in a hidden holder node in the overlay layer
    const NodeHandle holder = label(ui, overlayLayer(ui), items);
    ui.setCss(holder, "display", "none");
    // right press on the node, or the keyboard menu shortcut (Shift+F10), opens the menu at the pointer / the node
    ui.connect(node, SignalKind::Event, [holder](UiContext& c, NodeHandle n, const SignalArgs& a) {
        if (!a.event || !c.alive(holder)) return;
        const Event& e = *a.event;
        const bool pointer = e.type == EventType::PointerDown && e.button == MouseButton::Right;
        const bool keyboard = e.type == EventType::KeyDown && e.key == Key::F10 && (e.mods & kModShift);
        if (!pointer && !keyboard) return;
        const Rect r = c.rect(n);
        const std::string text(c.text(holder)); // popupMenu creates nodes: do not hold a view into node storage
        popupMenu(c, n, text, pointer ? e.pos : Vec2{r.x + 8.f, r.bottom()});
    });
}

// ---- message box ------------------------------------------------------------------------------------------------------
NodeHandle messageBox(UiContext& ui, std::string_view title, std::string_view text, std::string_view buttons, MessageIcon icon,
                      std::string_view id) {
    const NodeHandle dlg = dialog(ui, title, id);
    const NodeHandle body = dialogBody(ui, dlg);
    ui.addClass(body, "msgbody");
    const NodeHandle row = panel(ui, body);
    ui.addClass(row, "row");
    if (icon != MessageIcon::None) {
        static constexpr std::string_view kGlyph[] = {"", "i", "!", "x", "?"};
        static constexpr std::string_view kClass[] = {"", "info", "warning", "error", "question"};
        const NodeHandle badge = label(ui, row, kGlyph[std::size_t(icon)]);
        ui.addClass(badge, "msgicon");
        ui.addClass(badge, kClass[std::size_t(icon)]);
    }
    const NodeHandle msg = label(ui, row, text);
    ui.addClass(msg, "msgtext");
    const NodeHandle btns = panel(ui, body);
    ui.addClass(btns, "row");
    ui.addClass(btns, "buttons");
    const std::string_view names = buttons.empty() ? std::string_view("OK") : buttons;
    for (u32 i = 0, c = count(names); i < c; ++i) {
        const NodeHandle b = button(ui, btns, nth(names, i));
        if (i == 0) ui.addClass(b, "primary");
        ui.connect(b, SignalKind::Clicked, [dlg, i](UiContext& cx, NodeHandle btn, const SignalArgs&) {
            if (!cx.alive(dlg)) return;
            const std::string name(cx.text(btn)); // closing may free nothing, but keep the text stable for the slot
            cx.setValue(dlg, f32(i));
            closeDialog(cx, dlg);
            cx.emit(dlg, SignalKind::Submitted, {.value = f32(i), .text = name});
        });
    }
    ui.setRange(dlg, -1.f, 1e6f, 1.f);
    ui.setValue(dlg, -1.f);
    openDialog(ui, dlg);
    return dlg;
}

// ---- wizard -----------------------------------------------------------------------------------------------------------
NodeHandle wizard(UiContext& ui, std::string_view title, std::string_view id) {
    const NodeHandle wiz = dialog(ui, title, id);
    ui.addClass(wiz, "wizard");
    // dialog() made [title, body]; the wizard layout is [title, step, pages, buttons]
    const NodeHandle body = dialogBody(ui, wiz);
    const NodeHandle step = label(ui, wiz, "");
    ui.addClass(step, "dim");
    ui.moveTo(step, wiz, body);
    ui.addClass(body, "wizardpages");
    const NodeHandle row = panel(ui, wiz);
    ui.addClass(row, "row");
    ui.addClass(row, "buttons");
    const NodeHandle back = button(ui, row, "Back");
    const NodeHandle next = button(ui, row, "Next");
    ui.addClass(next, "primary");
    const NodeHandle cancel = button(ui, row, "Cancel");
    ui.setRange(wiz, 0.f, 1e6f, 1.f);
    ui.connect(back, SignalKind::Clicked, [wiz](UiContext& c, NodeHandle, const SignalArgs&) {
        if (c.alive(wiz) && c.value(wiz) > 0.f) wizardSetPage(c, wiz, u32(c.value(wiz)) - 1);
    });
    ui.connect(next, SignalKind::Clicked, [wiz](UiContext& c, NodeHandle, const SignalArgs&) {
        if (!c.alive(wiz)) return;
        const u32 cur = u32(c.value(wiz)), n = wizardPageCount(c, wiz);
        if (cur + 1 < n) { wizardSetPage(c, wiz, cur + 1); return; }
        closeDialog(c, wiz);
        c.emit(wiz, SignalKind::Submitted, {.value = f32(cur), .flag = true, .text = "finish"});
    });
    ui.connect(cancel, SignalKind::Clicked, [wiz](UiContext& c, NodeHandle, const SignalArgs&) {
        if (!c.alive(wiz)) return;
        closeDialog(c, wiz);
        c.emit(wiz, SignalKind::Submitted, {.value = c.value(wiz), .flag = false, .text = "cancel"});
    });
    return wiz;
}
NodeHandle wizardAddPage(UiContext& ui, NodeHandle wiz, std::string_view title, std::string_view id) {
    registerTierB3(ui);
    const NodeHandle pages = wizardPages(ui, wiz);
    const NodeHandle p = make(ui, "wizardpage", pages, id);
    if (!ui.alive(p)) return {};
    ui.addClass(p, "wizardpage");
    ui.setText(p, title); // not painted (WizardPageW): this is the page title shown in the step line
    ui.setA11y(p, A11yRole::Group, title);
    wizardShow(ui, wiz, u32(ui.value(wiz)));
    return p;
}
void wizardSetPage(UiContext& ui, NodeHandle wiz, u32 index) {
    const u32 before = u32(ui.value(wiz));
    wizardShow(ui, wiz, index);
    if (u32(ui.value(wiz)) != before) ui.emit(wiz, SignalKind::ValueChanged, {.value = ui.value(wiz)});
}
u32 wizardPage(const UiContext& ui, NodeHandle wiz) { return u32(ui.value(wiz)); }
u32 wizardPageCount(const UiContext& ui, NodeHandle wiz) { return ui.childCount(wizardPages(ui, wiz)); }
NodeHandle wizardButton(const UiContext& ui, NodeHandle wiz, WizardButton which) { return nthChild(ui, wizardButtons(ui, wiz), u32(which)); }
void openWizard(UiContext& ui, NodeHandle wiz) {
    wizardShow(ui, wiz, 0);
    openDialog(ui, wiz);
}

// ---- popover ----------------------------------------------------------------------------------------------------------
NodeHandle popover(UiContext& ui, NodeHandle anchor, std::string_view id) {
    const Rect r = ui.alive(anchor) ? ui.rect(anchor) : Rect{};
    const NodeHandle card = detail::popupPanel(ui, anchor, {r.x, r.bottom() + 6.f});
    if (!ui.alive(card)) return {};
    ui.addClass(card, "popover");
    (void)id;
    return card;
}
void closePopover(UiContext& ui, NodeHandle pop) { closePopup(ui, pop); }

// ---- toast ------------------------------------------------------------------------------------------------------------
NodeHandle toast(UiContext& ui, std::string_view text, f32 seconds, std::string_view id) {
    registerTierB3(ui);
    const NodeHandle layer = overlayLayer(ui);
    NodeHandle host = ui.find(kToastHostId);
    if (!ui.alive(host)) { // a bottom-centred column in the overlay layer; newest toast at the bottom
        host = panel(ui, layer, kToastHostId);
        ui.addClass(host, "toasthost");
        ui.setCss(host, "pointer-events", "none");
        setPx(ui, host, Prop::Bottom, 24.f);
        ui.set(host, Prop::CenterX, PropValue::ofBool(true));
    }
    const NodeHandle t = make(ui, "toast", host, id);
    if (!ui.alive(t)) return {};
    ui.setText(t, text);
    const f32 life = std::max(0.2f, seconds);
    ui.startTimer(life, false, [t](UiContext& c) {
        if (c.alive(t)) c.animate(t, UiContext::AnimProp::Opacity, 0.f, 0.25f);
    });
    ui.startTimer(life + 0.3f, false, [t](UiContext& c) {
        if (c.alive(t)) c.post(&destroyCmd, t);
    });
    return t;
}
u32 toastCount(const UiContext& ui) {
    const NodeHandle host = ui.find(kToastHostId);
    return ui.alive(host) ? ui.childCount(host) : 0u;
}

// ---- header -----------------------------------------------------------------------------------------------------------
NodeHandle headerView(UiContext& ui, NodeHandle parent, std::string_view titles, f32 sectionWidth, std::string_view id) {
    registerTierB3(ui);
    const NodeHandle h = make(ui, "header", parent, id);
    if (!ui.alive(h)) return h;
    headerPool().create(ui, h).defaultWidth = std::max(24.f, sectionWidth);
    setHeaderTitles(ui, h, titles);
    return h;
}
void setHeaderTitles(UiContext& ui, NodeHandle header, std::string_view titles) {
    HeaderState* s = headerPool().get(ui, header);
    if (!s) return;
    s->widths.resize(count(titles), s->defaultWidth);
    if (s->sortCol >= i32(s->widths.size())) s->sortCol = -1;
    ui.setText(header, titles);
}
u32 headerSectionCount(const UiContext& ui, NodeHandle header) {
    const HeaderState* s = headerPool().get(ui, header);
    return s ? u32(s->widths.size()) : 0u;
}
f32 headerSectionWidth(const UiContext& ui, NodeHandle header, u32 section) {
    const HeaderState* s = headerPool().get(ui, header);
    return s && section < s->widths.size() ? s->widths[section] : 0.f;
}
void setHeaderSectionWidth(UiContext& ui, NodeHandle header, u32 section, f32 px) {
    HeaderState* s = headerPool().get(ui, header);
    if (!s || section >= s->widths.size()) return;
    px = std::max(24.f, px);
    if (s->widths[section] == px) return;
    s->widths[section] = px;
    ui.setText(header, std::string(ui.text(header))); // re-measure + repaint
    ui.emit(header, SignalKind::ValueChanged, {.value = f32(section)});
}
i32 headerSortSection(const UiContext& ui, NodeHandle header, bool* ascending) {
    const HeaderState* s = headerPool().get(ui, header);
    if (ascending) *ascending = s ? s->asc : true;
    return s ? s->sortCol : -1;
}

// ---- text edit --------------------------------------------------------------------------------------------------------
NodeHandle textEdit(UiContext& ui, NodeHandle parent, std::string_view text, std::string_view id) {
    registerTierB3(ui);
    const NodeHandle n = make(ui, "textedit", parent, id);
    if (!ui.alive(n)) return n;
    textPool().create(ui, n);
    ui.setA11y(n, A11yRole::TextField, "Text");
    textEditSetText(ui, n, text);
    return n;
}
void textEditSetText(UiContext& ui, NodeHandle ed, std::string_view text) {
    TextEditState* s = textPool().get(ui, ed);
    if (!s) return;
    s->text.assign(text);
    s->caret = s->anchor = 0;
    s->scroll = 0.f;
    s->undo.clear();
    s->redo.clear();
    s->wrapDirty = true;
    ui.setText(ed, s->text);
    ui.invalidate(ed);
}
std::string textEditText(const UiContext& ui, NodeHandle ed) {
    const TextEditState* s = textPool().get(ui, ed);
    return s ? s->text : std::string();
}
void textEditSetReadOnly(UiContext& ui, NodeHandle ed, bool readOnly) {
    if (TextEditState* s = textPool().get(ui, ed)) s->readOnly = readOnly;
}
u32 textEditCaret(const UiContext& ui, NodeHandle ed) {
    const TextEditState* s = textPool().get(ui, ed);
    return s ? s->caret : 0u;
}
void textEditSetSelection(UiContext& ui, NodeHandle ed, u32 anchor, u32 caret) {
    TextEditState* s = textPool().get(ui, ed);
    if (!s) return;
    s->anchor = std::min(anchor, u32(s->text.size()));
    s->caret = std::min(caret, u32(s->text.size()));
    ui.invalidate(ed);
}
u32 textEditVisualLines(UiContext& ui, NodeHandle ed) {
    TextEditState* s = textPool().get(ui, ed);
    if (!s) return 0;
    TextEditW::wrap(ui, ed, *s);
    return u32(s->lines.size());
}
NodeHandle plainTextEdit(UiContext& ui, NodeHandle parent, std::string_view text, std::string_view id) {
    // no wrapping, no colouring, no gutter: the code editor's document model in its plainest form
    const NodeHandle n = codeEdit(ui, parent, text, CodeLanguage::Plain, id);
    ui.addClass(n, "plain");
    return n;
}

// ---- rich text --------------------------------------------------------------------------------------------------------
NodeHandle richText(UiContext& ui, NodeHandle parent, std::string_view markup, std::string_view id) {
    registerTierB3(ui);
    const NodeHandle n = make(ui, "richtext", parent, id);
    if (!ui.alive(n)) return n;
    richPool().create(ui, n);
    setRichText(ui, n, markup);
    return n;
}
void setRichText(UiContext& ui, NodeHandle view, std::string_view markup) {
    RichState* s = richPool().get(ui, view);
    if (!s) return;
    parseRichMarkup(markup, *s);
    std::string plain; // the node text (a11y name, bindings) is the markup without tags
    for (const RichRun& r : s->runs) plain += r.lineBreak ? std::string_view("\n") : std::string_view(r.text);
    ui.setA11y(view, A11yRole::Text, plain);
    ui.setText(view, markup); // re-measure + repaint
}
u32 richTextLinkCount(const UiContext& ui, NodeHandle view) {
    const RichState* s = richPool().get(ui, view);
    return s ? u32(s->links.size()) : 0u;
}
std::string richTextLink(const UiContext& ui, NodeHandle view, u32 index) {
    const RichState* s = richPool().get(ui, view);
    return s && index < s->links.size() ? s->links[index] : std::string();
}
Rect richTextLinkRect(UiContext& ui, NodeHandle view, u32 index) {
    RichState* s = richPool().get(ui, view);
    if (!s) return {};
    const Rect c = ui.contentRect(view);
    RichTextW::layout(ui, view, *s, std::max(1.f, c.w));
    for (const auto& p : s->pieces)
        if (s->runs[p.run].link == i32(index)) return {c.x + p.x, c.y + p.y, p.w, p.h};
    return {};
}
std::string richTextPlain(const UiContext& ui, NodeHandle view) {
    const RichState* s = richPool().get(ui, view);
    std::string plain;
    if (s)
        for (const RichRun& r : s->runs) plain += r.lineBreak ? std::string_view("\n") : std::string_view(r.text);
    return plain;
}

NodeHandle doubleSpinBox(UiContext& ui, NodeHandle parent, f32 lo, f32 hi, f32 value, f32 step, std::string_view id) {
    return spinBox(ui, parent, lo, hi, value, step > 0.f ? step : 0.1f, id);
}

} // namespace fuse::ui::widgets

#include "widgets_internal.hpp"
#include <cstdio>
// ---- accessibility (plan §4.14): header, rich text, toast, text edit -------------------------------------------------------
namespace fuse::ui::widgets {
namespace {
// Header: a row of column headers (id = section + 1); Click sorts by the section.
void headerA11y(const UiContext& ui, NodeHandle n, A11ySnapshotNode& self, std::vector<A11ySnapshotNode>& items) {
    const HeaderState* s = headerPool().get(ui, n);
    if (!s) return;
    detail::a11yDropTextName(ui, n, self);
    const Rect c = ui.contentRect(n);
    f32 x = c.x;
    for (u32 i = 0; i < s->widths.size(); ++i) {
        A11ySnapshotNode h = detail::a11yItem(i + 1, A11yRole::ColumnHeader, nth(ui.text(n), i), {x, c.y, s->widths[i], c.h},
                                              self.disabled ? 0u : a11yActionBit(A11yAction::Click));
        h.posInSet = i + 1;
        h.setSize = u32(s->widths.size());
        if (s->sortCol == i32(i)) h.valueText = s->asc ? "ascending" : "descending";
        items.push_back(std::move(h));
        x += s->widths[i];
    }
}
bool headerA11yAction(UiContext& ui, NodeHandle n, u32 child, A11yAction a, f32, std::string_view) {
    HeaderState* s = headerPool().get(ui, n);
    if (!s || !child || child > s->widths.size() || a != A11yAction::Click) return false;
    const i32 col = i32(child - 1);
    s->asc = s->sortCol == col ? !s->asc : true;
    s->sortCol = col;
    ui.invalidate(n);
    ui.emit(n, SignalKind::Toggled, {.value = f32(col), .flag = s->asc});
    return true;
}
// Rich text: the name is the plain text (set by setRichText); every link is a child (id = link index + 1).
void richTextA11y(const UiContext& cui, NodeHandle n, A11ySnapshotNode& self, std::vector<A11ySnapshotNode>& items) {
    UiContext& ui = const_cast<UiContext&>(cui); // link rects come from the wrapped layout
    for (u32 i = 0; i < richTextLinkCount(ui, n); ++i)
        items.push_back(detail::a11yItem(i + 1, A11yRole::Link, richTextLink(ui, n, i), richTextLinkRect(ui, n, i),
                                         self.disabled ? 0u : a11yActionBit(A11yAction::Click)));
}
bool richTextA11yAction(UiContext& ui, NodeHandle n, u32 child, A11yAction a, f32, std::string_view) {
    if (!child || child > richTextLinkCount(ui, n) || a != A11yAction::Click) return false;
    const std::string target = richTextLink(ui, n, child - 1);
    ui.emit(n, SignalKind::Submitted, {.value = f32(child - 1), .text = target});
    return true;
}
// Toast: an alert that is announced when it appears.
void toastA11y(const UiContext&, NodeHandle, A11ySnapshotNode& self, std::vector<A11ySnapshotNode>&) {
    if (self.live == A11yLive::Off) self.live = A11yLive::Polite;
}
// Text edit: SetValue replaces the document.
bool textEditA11yAction(UiContext& ui, NodeHandle n, u32 child, A11yAction a, f32, std::string_view text) {
    if (child || a != A11yAction::SetValue) return false;
    if (textEditText(ui, n) == text) return true;
    textEditSetText(ui, n, text);
    ui.emit(n, SignalKind::TextChanged, {.text = text});
    return true;
}
} // namespace
void detail::a11yTierB3(WidgetRegistry& r) {
    a11yHook(r, "header", A11yRole::Row, &headerA11y, &headerA11yAction);
    a11yHook(r, "richtext", A11yRole::Text, &richTextA11y, &richTextA11yAction);
    a11yHook(r, "toast", A11yRole::Alert, &toastA11y);
    a11yHook(r, "textedit", A11yRole::TextField, nullptr, &textEditA11yAction);
}
} // namespace fuse::ui::widgets
