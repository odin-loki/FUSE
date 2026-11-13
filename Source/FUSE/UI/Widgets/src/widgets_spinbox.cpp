// FUSE UI spin box options (QSpinBox / QDoubleSpinBox parity; ported from the gui-framework branch): typed entry
// with selection and clipboard, prefix / suffix, decimals, wrapping, special value text, adaptive stepping, keyboard
// tracking, correction mode, accelerated auto-repeat, textFromValue / valueFromText, EditingFinished. A spin box with
// options (setSpinBoxOptions) measures, paints and handles events here; one without keeps the Tier A path.
#include "fuse/ui/widgets/widgets.hpp"
#include "widgets_internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <unordered_map>

namespace fuse::ui::widgets {
namespace {

f32 lineHeight(UiContext& ui, f32 px) { return ui.font() ? std::ceil(ui.font()->metrics(px).lineHeight()) : std::ceil(px * 1.25f); }
Size textSize(UiContext& ui, NodeHandle n, std::string_view s) {
    const f32 px = ui.computed(n).fontSize;
    return {std::ceil(ui.measureString(s, px)), lineHeight(ui, px)};
}
Color accent(const PaintContext& pc) { return pc.tint(pc.style.background2.a > 0.f ? pc.style.background2 : Color::rgb(0x3D8BFD)); }
Color textCol(const PaintContext& pc) { return pc.tint(pc.style.color); }
void labelAt(const PaintContext& pc, DrawList& dl, std::string_view s, Rect box) {
    pc.ui.paintString(dl, s, box, textCol(pc), pc.style.fontSize, TextAlignment::Start, pc.node.index);
}
bool isSpinBox(const UiContext& ui, NodeHandle n) { return ui.alive(n) && ui.classOf(n) == ui.registry().find("spinbox"); }

struct SpinState {
    u32 gen = 0;
    SpinBoxOptions o;
    bool editing = false;   ///< the user changed the text (buf holds it) since the last commit
    std::string buf;        ///< typed clean text while editing
    std::size_t caret = std::string::npos, anchor = 0; ///< byte offsets into the editable (clean) text; npos = end
    bool dragging = false;
    TimerHandle timer{};    ///< arrow-button auto-repeat
    i32 repeatDir = 0;
    f32 interval = 0.f, accel = 0.f;
    std::string shown; ///< scratch for the painted text (kept, so a paint does not allocate)
};
// Keyed by UiContext::uid(), then node index (generation-checked). Empty until the first setSpinBoxOptions.
std::unordered_map<u64, std::unordered_map<u32, SpinState>>& spinStores() {
    static std::unordered_map<u64, std::unordered_map<u32, SpinState>> m;
    return m;
}
SpinState* spinFind(const UiContext& ui, NodeHandle n) {
    auto& m = spinStores();
    if (m.empty()) return nullptr;
    auto it = m.find(ui.uid());
    if (it == m.end()) return nullptr;
    auto j = it->second.find(n.index);
    return j != it->second.end() && j->second.gen == n.generation ? &j->second : nullptr;
}
SpinState& spinEnsure(const UiContext& ui, NodeHandle n) {
    SpinState& s = spinStores()[ui.uid()][n.index];
    if (s.gen != n.generation) {
        s = SpinState{};
        s.gen = n.generation;
    }
    return s;
}
int spinDecimals(const UiContext& ui, NodeHandle n, const SpinState* s) {
    if (s && s->o.decimals >= 0) return std::min(s->o.decimals, 9);
    const f32 st = ui.rangeStep(n);
    return st > 0.f && st < 1.f ? (st < 0.1f ? 2 : 1) : 0;
}
/// The value as text ("%.*f" with the box's decimals, "-0" shown as "0") in `buf`; the view points into it.
std::string_view spinFormat(const UiContext& ui, NodeHandle n, const SpinState* s, double v, char* buf, std::size_t cap) {
    const int len = std::snprintf(buf, cap, "%.*f", spinDecimals(ui, n, s), v);
    std::string_view out(buf, std::size_t(std::clamp(len, 0, int(cap) - 1)));
    if (out == "-0" || (out.starts_with("-0.") && out.find_first_not_of("0.", 1) == std::string_view::npos)) out.remove_prefix(1);
    return out;
}
std::string spinClean(const UiContext& ui, NodeHandle n, const SpinState* s, double v) {
    if (s && s->o.textFromValue) return s->o.textFromValue(v);
    char buf[64];
    return std::string(spinFormat(ui, n, s, v, buf, sizeof buf));
}
bool spinSpecial(const UiContext& ui, NodeHandle n, const SpinState* s) {
    return s && !s->editing && !s->o.specialValueText.empty() && ui.value(n) <= ui.rangeMin(n);
}
/// the editable part of the shown text (no prefix / suffix): the typed text, the special text, or the value
std::string spinEditable(const UiContext& ui, NodeHandle n, const SpinState* s) {
    if (s && s->editing) return s->buf;
    if (spinSpecial(ui, n, s)) return s->o.specialValueText;
    return spinClean(ui, n, s, double(ui.value(n)));
}
/// The same without a string: a view into the typed buffer, the special text, `scratch` (textFromValue) or `buf`.
std::string_view spinEditableView(const UiContext& ui, NodeHandle n, const SpinState* s, std::string& scratch, char* buf, std::size_t cap) {
    if (s && s->editing) return s->buf;
    if (spinSpecial(ui, n, s)) return s->o.specialValueText;
    if (s && s->o.textFromValue) { scratch = s->o.textFromValue(double(ui.value(n))); return scratch; }
    return spinFormat(ui, n, s, double(ui.value(n)), buf, cap);
}
std::string_view spinPrefix(const UiContext& ui, NodeHandle n, const SpinState* s) {
    return s && !spinSpecial(ui, n, s) ? std::string_view(s->o.prefix) : std::string_view{};
}
std::string spinDisplay(const UiContext& ui, NodeHandle n) {
    const SpinState* s = spinFind(ui, n);
    if (!s) return spinClean(ui, n, s, double(ui.value(n)));
    if (spinSpecial(ui, n, s)) return s->o.specialValueText;
    return s->o.prefix + spinEditable(ui, n, s) + s->o.suffix;
}
/// QSpinBox::valueFromText on the clean text (prefix / suffix already stripped); nullopt = not a number
std::optional<double> spinParse(const UiContext& ui, NodeHandle n, const SpinState& s, std::string_view t) {
    if (!s.o.specialValueText.empty() && t == s.o.specialValueText) return double(ui.rangeMin(n));
    if (s.o.valueFromText) return s.o.valueFromText(t);
    while (!t.empty() && t.front() == ' ') t.remove_prefix(1);
    while (!t.empty() && t.back() == ' ') t.remove_suffix(1);
    if (t.empty()) return std::nullopt;
    std::string z(t);
    for (char& c : z) if (c == ',') c = '.';
    char* end = nullptr;
    const double v = std::strtod(z.c_str(), &end);
    if (end != z.c_str() + z.size() || !std::isfinite(v)) return std::nullopt;
    return v;
}
void spinSelectAll(SpinState& s) { s.anchor = 0; s.caret = std::string::npos; }
/// caret / anchor clamped to the editable text (npos = end)
void spinClamp(const UiContext& ui, NodeHandle n, SpinState& s) {
    char buf[64];
    const std::size_t len = spinEditableView(ui, n, &s, s.shown, buf, sizeof buf).size();
    s.caret = std::min(s.caret, len);
    s.anchor = std::min(s.anchor, len);
}
/// QAbstractSpinBox::stepBy: wrapping (past either end jumps to the other end, as Qt), adaptive decimal step
void spinStep(UiContext& ui, NodeHandle n, i32 steps) {
    const SpinState* s = spinFind(ui, n);
    const double cur = ui.value(n), lo = ui.rangeMin(n), hi = ui.rangeMax(n);
    double st = ui.rangeStep(n) > 0.f ? ui.rangeStep(n) : 1.0;
    if (s && s->o.adaptiveStep) {
        const int dec = spinDecimals(ui, n, s);
        const double minStep = std::pow(10.0, -dec), a = std::abs(cur);
        const bool comp = (cur < 0) != (steps < 0);
        if (dec == 0) st = a < 100 ? 1.0 : std::pow(10.0, std::floor(std::log10(a - (comp ? 1 : 0))) - 1);
        else st = a < minStep ? minStep : std::max(minStep, std::pow(10.0, std::floor(std::log10(a - (comp ? minStep : 0))) - 1));
    }
    double v = cur + st * steps;
    if (s && s->o.wrapping && hi > lo) {
        if (v > hi) v = cur >= hi ? lo : hi;
        else if (v < lo) v = cur <= lo ? hi : lo;
    }
    ui.setValue(n, f32(v), true);
    if (SpinState* m = spinFind(ui, n)) spinSelectAll(*m); // Qt selects the stepped value
}
bool spinCanStep(const UiContext& ui, NodeHandle n, i32 dir) {
    const SpinState* s = spinFind(ui, n);
    if (s && s->o.wrapping) return true;
    return dir > 0 ? ui.value(n) < ui.rangeMax(n) : ui.value(n) > ui.rangeMin(n);
}
/// Enter / focus out / stepping while typing: an in-range number is taken (clamped), anything else reverts
void spinCommit(UiContext& ui, NodeHandle n, bool finished) {
    SpinState* s = spinFind(ui, n);
    if (!s || !s->editing) {
        if (finished) ui.emit(n, SignalKind::EditingFinished, {.value = ui.value(n)});
        return;
    }
    s->editing = false;
    if (const auto v = spinParse(ui, n, *s, s->buf)) {
        const bool in = *v >= ui.rangeMin(n) && *v <= ui.rangeMax(n);
        if (in || s->o.correction == SpinCorrection::NearestValue)
            ui.setValue(n, f32(std::clamp(*v, double(ui.rangeMin(n)), double(ui.rangeMax(n)))), true);
    }
    s->buf.clear();
    spinSelectAll(*s);
    ui.invalidate(n);
    if (finished) ui.emit(n, SignalKind::EditingFinished, {.value = ui.value(n)});
}
void spinEdited(UiContext& ui, NodeHandle n, SpinState& s) {
    ui.invalidate(n);
    if (!s.o.keyboardTracking) return;
    if (const auto v = spinParse(ui, n, s, s.buf); v && *v >= ui.rangeMin(n) && *v <= ui.rangeMax(n)) ui.setValue(n, f32(*v), true);
}
/// start typing: the editable text becomes the buffer (caret / selection kept)
void spinBegin(const UiContext& ui, NodeHandle n, SpinState& s) {
    spinClamp(ui, n, s);
    if (!s.editing) { s.buf = spinEditable(ui, n, &s); s.editing = true; }
}
bool spinEraseSel(SpinState& s) {
    const std::size_t a = std::min(s.anchor, s.caret), b = std::max(s.anchor, s.caret);
    if (a == b) return false;
    s.buf.erase(a, b - a);
    s.caret = s.anchor = a;
    return true;
}
void spinInsert(UiContext& ui, NodeHandle n, SpinState& s, std::string_view text) {
    std::string ins;
    for (char ch : text) {
        const bool numeric = (ch >= '0' && ch <= '9') || ch == '-' || ch == '+' || ch == '.' || ch == ',' || ch == 'e' || ch == 'E';
        if ((numeric || s.o.valueFromText || !s.o.specialValueText.empty()) && (u8(ch) >= 0x20 || (u8(ch) & 0x80))) ins += ch;
    }
    if (ins.empty()) return;
    spinBegin(ui, n, s);
    spinEraseSel(s);
    if (s.buf.size() + ins.size() > 64) return;
    s.buf.insert(s.caret, ins);
    s.caret += ins.size();
    s.anchor = s.caret;
    spinEdited(ui, n, s);
}
std::size_t u8Prev(std::string_view t, std::size_t i) { if (!i) return 0; --i; while (i && (u8(t[i]) & 0xC0) == 0x80) --i; return i; }
std::size_t u8Next(std::string_view t, std::size_t i) { if (i >= t.size()) return t.size(); ++i; while (i < t.size() && (u8(t[i]) & 0xC0) == 0x80) ++i; return i; }
/// byte offset in the editable text nearest to window x
std::size_t spinHit(const UiContext& ui, NodeHandle n, const SpinState& s, f32 x) {
    const Rect c = ui.contentRect(n);
    const f32 px = ui.computed(n).fontSize;
    // (pointer presses are rare: the prefix + text is built once here)
    std::string all(spinPrefix(ui, n, &s));
    const std::size_t from = all.size();
    const std::string t = spinEditable(ui, n, &s);
    all += t;
    std::size_t best = 0;
    f32 bestD = 1e30f;
    for (std::size_t i = 0;; i = u8Next(t, i)) {
        const f32 cx = c.x + const_cast<UiContext&>(ui).measureString(std::string_view(all).substr(0, from + i), px);
        if (std::abs(cx - x) < bestD) { bestD = std::abs(cx - x); best = i; }
        if (i >= t.size()) break;
    }
    return best;
}
constexpr f32 kSpinRepeatDelay = 0.5f, kSpinRepeatRate = 0.15f; // Qt SH_SpinBox_ClickAutoRepeatThreshold / Rate
void spinStopRepeat(UiContext& ui, SpinState& s) {
    if (s.repeatDir) ui.cancelTimer(s.timer);
    s.repeatDir = 0;
}
void spinArm(UiContext& ui, NodeHandle n, f32 delay) {
    SpinState* s = spinFind(ui, n);
    if (!s) return;
    s->timer = ui.startTimer(delay, false, [n](UiContext& u) {
        SpinState* st = spinFind(u, n);
        if (!st || !st->repeatDir || !u.alive(n)) return;
        spinStep(u, n, st->repeatDir);
        if (st->o.accelerated) { // QAbstractSpinBox::accelerated: the repeat interval shrinks by 5% of the rate per tick
            st->accel += kSpinRepeatRate * 0.05f;
            st->interval = std::max(0.01f, kSpinRepeatRate - st->accel);
        }
        spinArm(u, n, st->interval);
    });
}


} // namespace

namespace detail {

bool spinBoxHasState(const UiContext& ui, NodeHandle n) { return spinFind(ui, n) != nullptr; }

Size spinBoxMeasure(UiContext& ui, NodeHandle n) {
    const SpinState* s = spinFind(ui, n);
    f32 w = textSize(ui, n, spinDisplay(ui, n)).w;
    if (s) { // widest of min / max / special text, as QAbstractSpinBox::sizeHint
        w = std::max(w, textSize(ui, n, s->o.prefix + spinClean(ui, n, s, ui.rangeMin(n)) + s->o.suffix).w);
        w = std::max(w, textSize(ui, n, s->o.prefix + spinClean(ui, n, s, ui.rangeMax(n)) + s->o.suffix).w);
        w = std::max(w, textSize(ui, n, s->o.specialValueText).w);
    }
    return {std::max(48.f, w) + 20.f, textSize(ui, n, "0").h};
}
void spinBoxPaint(const PaintContext& pc, DrawList& dl) {
    UiContext& ui = pc.ui;
    const Rect c = pc.content;
    SpinState* s = spinFind(ui, pc.node);
    if (!s) return;
    // the shown text, built in the record's buffer (no allocation per paint): [prefix] editable [suffix]
    std::string& t = s->shown;
    t.clear();
    std::size_t edFrom = 0, edLen = 0;
    if (spinSpecial(ui, pc.node, s)) {
        t += s->o.specialValueText;
        edLen = t.size();
    } else {
        t += s->o.prefix;
        edFrom = t.size();
        if (s->editing) t += s->buf;
        else if (s->o.textFromValue) t += s->o.textFromValue(double(ui.value(pc.node)));
        else {
            char buf[64];
            t += spinFormat(ui, pc.node, s, double(ui.value(pc.node)), buf, sizeof buf);
        }
        edLen = t.size() - edFrom;
        t += s->o.suffix;
    }
    if (ui.focused() == pc.node) {
        auto xAt = [&](std::size_t i) {
            return c.x + std::ceil(ui.measureString(std::string_view(t).substr(0, edFrom + std::min(i, edLen)), pc.style.fontSize));
        };
        const std::size_t a = std::min(s->anchor, s->caret), b = std::max(s->anchor, s->caret);
        if (std::min(a, edLen) != std::min(b, edLen))
            dl.rect({xAt(a), c.y + 1.f, xAt(b) - xAt(a), c.h - 2.f}, accent(pc).withAlpha(0.45f * pc.opacity));
        else dl.rect({xAt(s->caret), c.y + 1.f, 1.f, c.h - 2.f}, textCol(pc));
    }
    labelAt(pc, dl, t, {c.x, c.y, c.w - 20.f, c.h});
    const f32 ax = c.right() - 10.f, my = c.y + c.h * 0.5f;
    const Color a = textCol(pc), off = a.withAlpha(a.a * 0.3f); // stepEnabled: a disabled arrow is dimmed
    const Color au = spinCanStep(ui, pc.node, 1) ? a : off, ad = spinCanStep(ui, pc.node, -1) ? a : off;
    dl.line({ax - 4.f, my - 3.f}, {ax, my - 7.f}, au, 1.5f);
    dl.line({ax, my - 7.f}, {ax + 4.f, my - 3.f}, au, 1.5f);
    dl.line({ax - 4.f, my + 3.f}, {ax, my + 7.f}, ad, 1.5f);
    dl.line({ax, my + 7.f}, {ax + 4.f, my + 3.f}, ad, 1.5f);
}
bool spinBoxEvent(EventContext& ec, const Event& e) {
    if (ec.phase != Phase::Target) return false;
    UiContext& ui = ec.ui;
    const NodeHandle n = ec.node;
    if (e.type == EventType::FocusIn) { // Qt selects the whole value on focus
        spinSelectAll(spinEnsure(ui, n));
        ui.setTextCaret(n, ui.contentRect(n));
        ui.invalidate(n);
        return false;
    }
    if (e.type == EventType::FocusOut) {
        if (SpinState* s = spinFind(ui, n)) spinStopRepeat(ui, *s);
        spinCommit(ui, n, true);
        return false;
    }
    if (e.type == EventType::PointerDown) {
        SpinState& s = spinEnsure(ui, n);
        const Rect c = ui.contentRect(n);
        if (e.pos.x >= c.right() - 20.f) { // arrow: step now, auto-repeat while held
            spinCommit(ui, n, false);
            spinStopRepeat(ui, s);
            const i32 dir = e.pos.y < c.y + c.h * 0.5f ? 1 : -1;
            if (!spinCanStep(ui, n, dir)) return true;
            s.repeatDir = dir;
            s.accel = 0.f;
            s.interval = kSpinRepeatRate;
            spinStep(ui, n, s.repeatDir);
            spinArm(ui, n, kSpinRepeatDelay);
            return true;
        }
        spinClamp(ui, n, s);
        s.caret = spinHit(ui, n, s, e.pos.x);
        if (!(e.mods & kModShift)) s.anchor = s.caret;
        s.dragging = true;
        ui.invalidate(n);
        return true;
    }
    if (e.type == EventType::PointerMove) {
        SpinState* s = spinFind(ui, n);
        if (s && s->dragging && ui.pressed() == n) { s->caret = spinHit(ui, n, *s, e.pos.x); ui.invalidate(n); return true; }
        return false;
    }
    if (e.type == EventType::PointerUp) {
        if (SpinState* s = spinFind(ui, n)) { s->dragging = false; spinStopRepeat(ui, *s); }
        return false;
    }
    if (e.type == EventType::Wheel) {
        spinCommit(ui, n, false);
        const i32 dir = e.wheel.y > 0.f ? 1 : -1;
        if (spinCanStep(ui, n, dir)) spinStep(ui, n, dir);
        return true;
    }
    if (e.type == EventType::Text) {
        SpinState& s = spinEnsure(ui, n);
        spinInsert(ui, n, s, e.text);
        return true;
    }
    if (e.type != EventType::KeyDown) return false;
    SpinState& s = spinEnsure(ui, n);
    spinClamp(ui, n, s);
    const bool shift = (e.mods & kModShift) != 0, ctrl = (e.mods & (kModCtrl | kModSuper)) != 0;
    char edBuf[64];
    const std::string_view ed = spinEditableView(ui, n, &s, s.shown, edBuf, sizeof edBuf); // (valid until the text changes)
    auto move = [&](std::size_t to) { s.caret = to; if (!shift) s.anchor = to; ui.invalidate(n); return true; };
    switch (e.key) {
    case Key::Left: {
        if (!shift && s.anchor != s.caret) return move(std::min(s.anchor, s.caret));
        return move(u8Prev(ed, s.caret));
    }
    case Key::Right: {
        if (!shift && s.anchor != s.caret) return move(std::max(s.anchor, s.caret));
        return move(u8Next(ed, s.caret));
    }
    case Key::Home: return move(0);
    case Key::End: return move(ed.size());
    case Key::A: if (ctrl) { s.anchor = 0; s.caret = ed.size(); ui.invalidate(n); return true; } return false;
    case Key::C: case Key::X: {
        if (!ctrl) return false;
        const std::size_t a = std::min(s.anchor, s.caret), b = std::max(s.anchor, s.caret);
        if (a == b) return true;
        ui.setClipboardText(ed.substr(a, b - a));
        if (e.key == Key::X) { spinBegin(ui, n, s); spinEraseSel(s); spinEdited(ui, n, s); }
        return true;
    }
    case Key::V: {
        if (!ctrl) return false;
        std::string clip;
        if (ui.clipboardText(clip)) spinInsert(ui, n, s, clip);
        return true;
    }
    case Key::Backspace: case Key::Delete: {
        spinBegin(ui, n, s);
        if (!spinEraseSel(s)) {
            if (e.key == Key::Backspace) { const std::size_t p = u8Prev(s.buf, s.caret); s.buf.erase(p, s.caret - p); s.caret = s.anchor = p; }
            else { const std::size_t q = u8Next(s.buf, s.caret); s.buf.erase(s.caret, q - s.caret); s.anchor = s.caret; }
        }
        spinEdited(ui, n, s);
        return true;
    }
    case Key::Escape:
        if (!s.editing) return false;
        s.editing = false;
        s.buf.clear();
        spinSelectAll(s);
        ui.invalidate(n);
        return true;
    case Key::Enter: spinCommit(ui, n, true); return true;
    default: break;
    }
    const i32 dir = e.key == Key::Up ? 1 : e.key == Key::Down ? -1 : e.key == Key::PageUp ? 10 : e.key == Key::PageDown ? -10 : 0;
    if (dir) { spinCommit(ui, n, false); if (spinCanStep(ui, n, dir)) spinStep(ui, n, dir); return true; }
    return false;
}

} // namespace detail

void setSpinBoxOptions(UiContext& ui, NodeHandle spin, SpinBoxOptions o) {
    if (!isSpinBox(ui, spin)) return;
    std::erase_if(spinStores(), [](const auto& kv) { return !UiContext::exists(kv.first); });
    if (auto it = spinStores().find(ui.uid()); it != spinStores().end()) // records of nodes destroyed since
        std::erase_if(it->second, [&](const auto& kv) { return !ui.alive(NodeHandle{kv.first, kv.second.gen}); });
    if (!ui.alive(spin)) return;
    SpinState& s = spinEnsure(ui, spin);
    s.o = std::move(o);
    s.editing = false;
    s.buf.clear();
    spinSelectAll(s);
    ui.invalidateLayout(spin); // measure depends on the options
    ui.invalidate(spin);
}
std::string spinBoxText(const UiContext& ui, NodeHandle spin) { return ui.alive(spin) ? spinDisplay(ui, spin) : std::string{}; }
std::string spinBoxCleanText(const UiContext& ui, NodeHandle spin) {
    if (!ui.alive(spin)) return {};
    const SpinState* s = spinFind(ui, spin);
    return s && s->editing ? s->buf : spinClean(ui, spin, s, double(ui.value(spin)));
}
bool spinBoxStepEnabled(const UiContext& ui, NodeHandle spin, bool up) { return ui.alive(spin) && spinCanStep(ui, spin, up ? 1 : -1); }
void spinBoxSelection(const UiContext& ui, NodeHandle spin, u32& from, u32& to, u32& caret) {
    from = to = caret = 0;
    SpinState* s = ui.alive(spin) ? spinFind(ui, spin) : nullptr;
    if (!s) return;
    const std::size_t len = spinEditable(ui, spin, s).size();
    const std::size_t c = std::min(s->caret, len), a = std::min(s->anchor, len);
    from = u32(std::min(a, c));
    to = u32(std::max(a, c));
    caret = u32(c);
}
void setSpinBoxSelection(UiContext& ui, NodeHandle spin, u32 from, u32 to) {
    if (!ui.alive(spin)) return;
    SpinState& s = spinEnsure(ui, spin);
    s.anchor = from;
    s.caret = to;
    spinClamp(ui, spin, s);
    ui.invalidate(spin);
}
void spinBoxStepBy(UiContext& ui, NodeHandle spin, i32 steps) {
    if (!ui.alive(spin)) return;
    spinCommit(ui, spin, false);
    spinStep(ui, spin, steps);
}

const SpinBoxOptions& spinBoxOptions(const UiContext& ui, NodeHandle spin) {
    static const SpinBoxOptions kDefault;
    const SpinState* s = spinFind(ui, spin);
    return s ? s->o : kDefault;
}

// ---- accessibility: the value text is what the box shows (prefix / suffix / special text) -------------------------------
namespace {
void spinA11y(const UiContext& ui, NodeHandle n, A11ySnapshotNode& self, std::vector<A11ySnapshotNode>&) {
    if (spinFind(ui, n)) self.valueText = spinDisplay(ui, n);
}
} // namespace
void detail::a11ySpinBox(WidgetRegistry& r) { a11yHook(r, "spinbox", A11yRole::SpinBox, &spinA11y, nullptr); }

} // namespace fuse::ui::widgets
