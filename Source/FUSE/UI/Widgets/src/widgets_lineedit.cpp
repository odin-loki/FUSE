// FUSE UI line edit options (QLineEdit parity; ported from the gui-framework branch): echo modes, max length,
// placeholder, read-only, validators with fixup, input masks, EditingFinished. A line edit with options (or a
// completer) paints and handles events here; one without keeps the Tier A path in widgets.cpp untouched.
#include "fuse/ui/widgets/widgets.hpp"
#include "widgets_internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace fuse::ui::widgets {
namespace {

struct MaskCell {
    char kind = 0;            ///< 0: literal; else the mask letter (A a N n X x 9 0 D d # H h B b)
    char32_t lit = 0;
    signed char caseMode = 0; ///< 1 upper, -1 lower
};
struct LineEditState {
    NodeHandle node;
    LineEditOptions o;
    std::vector<MaskCell> cells; ///< input mask (empty: none)
    std::u32string vals;         ///< per cell: entered code point, 0 = blank
    char32_t blank = U' ';
    std::string text;            ///< masked fields: literals + filled cells (lineEditText)
    bool dirty = false;          ///< edited since the last EditingFinished
    std::string shown;           ///< scratch for the echo mask (kept, so a paint does not allocate)
};
// Keyed by (context uid << 32 | node index); a record whose node died is dropped on the next setLineEditOptions.
std::unordered_map<u64, std::unique_ptr<LineEditState>>& table() {
    static std::unordered_map<u64, std::unique_ptr<LineEditState>> t;
    return t;
}
u64 keyOf(const UiContext& ui, NodeHandle n) { return (ui.uid() << 32) | n.index; }
LineEditState* leState(const UiContext& ui, NodeHandle n) {
    auto& t = table();
    if (t.empty()) return nullptr;
    const auto it = t.find(keyOf(ui, n));
    return it != t.end() && it->second->node == n ? it->second.get() : nullptr;
}
LineEditState& leEnsure(UiContext& ui, NodeHandle n) {
    if (LineEditState* s = leState(ui, n)) return *s;
    auto& t = table();
    std::erase_if(t, [&](const auto& kv) { return (kv.first >> 32) == ui.uid() ? !ui.alive(kv.second->node) : !UiContext::exists(kv.first >> 32); });
    auto& s = t[keyOf(ui, n)];
    s = std::make_unique<LineEditState>();
    s->node = n;
    return *s;
}
const LineEditOptions& leOpts(const UiContext& ui, NodeHandle n) {
    static const LineEditOptions kDefault;
    const LineEditState* s = leState(ui, n);
    return s ? s->o : kDefault;
}
std::size_t codePoints(std::string_view s) {
    std::size_t c = 0;
    for (unsigned char ch : s) c += (ch & 0xC0) != 0x80;
    return c;
}
std::string_view clipCodePoints(std::string_view s, std::size_t n) {
    std::size_t i = 0, c = 0;
    while (i < s.size()) {
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) {
            if (c == n) break;
            ++c;
        }
        ++i;
    }
    return s.substr(0, i);
}
std::string utf8Of(char32_t cp) {
    std::string o;
    if (cp < 0x80) o += char(cp);
    else if (cp < 0x800) { o += char(0xC0 | (cp >> 6)); o += char(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { o += char(0xE0 | (cp >> 12)); o += char(0x80 | ((cp >> 6) & 0x3F)); o += char(0x80 | (cp & 0x3F)); }
    else { o += char(0xF0 | (cp >> 18)); o += char(0x80 | ((cp >> 12) & 0x3F)); o += char(0x80 | ((cp >> 6) & 0x3F)); o += char(0x80 | (cp & 0x3F)); }
    return o;
}
/// How a field shows its text: the shown string, and a byte offset in the real text -> offset in the shown one.
struct Shown {
    std::string_view text; ///< the real text, or the mask characters in the caller's scratch buffer
    bool masked = false, hidden = false;
    std::size_t mask = 0;
    std::string_view real;
    std::size_t map(std::size_t off) const {
        if (hidden) return 0;
        if (!masked) return off;
        std::size_t g = 0;
        for (std::size_t i = 0; i < off && i < real.size(); i = fuse::text::nextBoundary(real, i)) ++g;
        return g * mask;
    }
};
bool leMasked(const LineEditOptions& o, bool focused) {
    return o.echo == EchoMode::Password || (o.echo == EchoMode::PasswordEchoOnEdit && !focused);
}
Shown shownText(const LineEditOptions& o, std::string_view real, bool focused, std::string& scratch) {
    Shown s;
    s.real = real;
    if (o.echo == EchoMode::NoEcho) { s.hidden = true; return s; }
    if (!leMasked(o, focused)) { s.text = real; return s; }
    s.masked = true;
    const std::string m = utf8Of(o.maskChar); // (at most 4 bytes: no heap)
    s.mask = m.size();
    scratch.clear();
    for (std::size_t i = 0; i < real.size(); i = fuse::text::nextBoundary(real, i)) scratch += m;
    s.text = scratch;
    return s;
}
/// The node text: the real text, or for masked / hidden fields what is on screen (never the secret).
void publishText(UiContext& ui, NodeHandle n) {
    const LineEditOptions& o = leOpts(ui, n);
    const LineEditState* st = leState(ui, n);
    const std::string& t = st && !st->cells.empty() ? st->text : ui.editBuffer(n).text();
    if (o.echo == EchoMode::Normal) { ui.setText(n, t); return; }
    std::string mask;
    const std::string_view shown = shownText(o, t, false, mask).text;
    ui.setText(n, shown);
}
Validation leValidate(const LineEditOptions& o, std::string_view t) { return o.validator ? o.validator(t) : Validation::Acceptable; }
void repaint(UiContext& ui, NodeHandle n) { ui.setValue(n, ui.value(n) + 1.f); } // value counts caret moves

// ---- input mask (QLineEdit::inputMask) ------------------------------------------------------------------------------
std::u32string decode32(std::string_view s) {
    std::u32string o;
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        int k = c < 0x80 ? 0 : c < 0xE0 ? 1 : c < 0xF0 ? 2 : 3;
        char32_t cp = k == 0 ? c : k == 1 ? (c & 0x1F) : k == 2 ? (c & 0x0F) : (c & 0x07);
        ++i;
        for (; k > 0 && i < s.size(); --k, ++i) cp = (cp << 6) | (static_cast<unsigned char>(s[i]) & 0x3F);
        o += cp;
    }
    return o;
}
void parseMask(std::string_view mask, LineEditState& st) {
    st.cells.clear();
    st.blank = U' ';
    std::u32string m = decode32(mask);
    // trailing ";c": blank character
    for (std::size_t k = m.size(); k-- > 0;) {
        if (m[k] != U';') continue;
        std::size_t bs = 0;
        for (std::size_t j = k; j-- > 0 && m[j] == U'\\';) ++bs;
        if (bs % 2) continue;
        if (k + 1 < m.size()) st.blank = m[k + 1];
        m.resize(k);
        break;
    }
    signed char cm = 0;
    for (std::size_t k = 0; k < m.size(); ++k) {
        const char32_t c = m[k];
        if (c == U'>') { cm = 1; continue; }
        if (c == U'<') { cm = -1; continue; }
        if (c == U'!') { cm = 0; continue; }
        MaskCell cell;
        if (c == U'\\' && k + 1 < m.size()) cell.lit = m[++k];
        else if (c < 128 && std::string_view("AaNnXx90Dd#HhBb").find(char(c)) != std::string_view::npos) cell.kind = char(c);
        else cell.lit = c;
        cell.caseMode = cm;
        st.cells.push_back(cell);
    }
    st.vals.assign(st.cells.size(), 0);
}
bool maskRequired(char k) { return k >= 'A' && k <= 'Z' ? true : k == '9' || k == 'D'; }
bool maskAccepts(char k, char32_t c) {
    const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c > 127 && c != 0xA0);
    const bool digit = c >= '0' && c <= '9';
    switch (k) {
    case 'A': case 'a': return alpha;
    case 'N': case 'n': return alpha || digit;
    case 'X': case 'x': return c >= 32 && c != 127;
    case '9': case '0': return digit;
    case 'D': case 'd': return c >= '1' && c <= '9';
    case '#': return digit || c == '+' || c == '-';
    case 'H': case 'h': return digit || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    case 'B': case 'b': return c == '0' || c == '1';
    default: return false;
    }
}
char32_t maskCase(signed char cm, char32_t c) {
    if (cm > 0 && c >= 'a' && c <= 'z') return c - 32;
    if (cm < 0 && c >= 'A' && c <= 'Z') return c + 32;
    return c;
}
std::string maskDisplay(const LineEditState& st) {
    std::string o;
    for (std::size_t k = 0; k < st.cells.size(); ++k)
        o += utf8Of(st.cells[k].kind ? (st.vals[k] ? st.vals[k] : st.blank) : st.cells[k].lit);
    return o;
}
std::string maskStripped(const LineEditState& st, std::size_t from = 0, std::size_t to = ~std::size_t(0)) {
    std::string o;
    for (std::size_t k = from; k < st.cells.size() && k < to; ++k) {
        if (!st.cells[k].kind) o += utf8Of(st.cells[k].lit);
        else if (st.vals[k]) o += utf8Of(st.vals[k]);
    }
    return o;
}
bool maskComplete(const LineEditState& st) {
    for (std::size_t k = 0; k < st.cells.size(); ++k)
        if (st.cells[k].kind && maskRequired(st.cells[k].kind) && !st.vals[k]) return false;
    return true;
}
/// Types code points from cell `cur`; returns the new caret cell.
std::size_t maskType(LineEditState& st, std::size_t cur, std::u32string_view in) {
    const std::size_t n = st.cells.size();
    for (char32_t c : in) {
        if (cur < n && !st.cells[cur].kind && st.cells[cur].lit == c) { ++cur; continue; }
        if (cur > 0 && cur <= n && !st.cells[cur - 1].kind && st.cells[cur - 1].lit == c) continue; // stepped over already
        std::size_t j = cur;
        while (j < n && !st.cells[j].kind) ++j;
        if (j >= n) break;
        if (!maskAccepts(st.cells[j].kind, c)) {
            // a separator typed mid-section jumps past the next matching literal (Qt: "1." in "000.000")
            std::size_t k = j;
            while (k < n && st.cells[k].kind) ++k;
            if (k < n && st.cells[k].lit == c) {
                cur = k + 1;
                while (cur < n && !st.cells[cur].kind) ++cur;
            }
            continue;
        }
        st.vals[j] = maskCase(st.cells[j].caseMode, c);
        cur = j + 1;
        while (cur < n && !st.cells[cur].kind) ++cur;
    }
    return cur;
}
void maskClear(LineEditState& st, std::size_t a, std::size_t b) {
    for (std::size_t k = a; k < b && k < st.cells.size(); ++k) st.vals[k] = 0;
}
/// buffer <- display, caret at cell `cur`.
void maskSync(UiContext& ui, NodeHandle n, LineEditState& st, std::size_t cur) {
    st.text = maskStripped(st);
    auto& eb = ui.editBuffer(n);
    eb.setText(maskDisplay(st));
    eb.home(false);
    for (std::size_t k = 0; k < cur; ++k) eb.moveRight(false);
}
Validation leStateOf(UiContext& ui, NodeHandle n) {
    const LineEditState* st = leState(ui, n);
    if (st && !st->cells.empty()) {
        const Validation v = leValidate(st->o, st->text);
        return v == Validation::Acceptable && !maskComplete(*st) ? Validation::Intermediate : v;
    }
    return leValidate(leOpts(ui, n), ui.editBuffer(n).text());
}
std::string_view leText(UiContext& ui, NodeHandle n) {
    const LineEditState* st = leState(ui, n);
    if (st && !st->cells.empty()) return st->text;
    return ui.editBuffer(n).text();
}
bool isLineEdit(const UiContext& ui, NodeHandle n) { return ui.alive(n) && ui.classOf(n) == ui.registry().find("lineedit"); }

Color accent(const PaintContext& pc) { return pc.tint(pc.style.background2.a > 0.f ? pc.style.background2 : themed(pc, tok::accent, 0x3D8BFD)); }
Color textCol(const PaintContext& pc) { return pc.tint(pc.style.color); }
void labelAt(const PaintContext& pc, DrawList& dl, std::string_view s, Rect box) {
    pc.ui.paintString(dl, s, box, textCol(pc), pc.style.fontSize, TextAlignment::Start, pc.node.index);
}

void changed(UiContext& ui, NodeHandle n, bool deletion = false) {
    publishText(ui, n);
    leEnsure(ui, n).dirty = true;
    ui.emit(n, SignalKind::TextChanged, {.text = leText(ui, n)});
    if (ui.alive(n)) detail::completerEdited(ui, n, deletion);
}
/// Enter / focus loss: fixup when not Acceptable, then Submitted (Enter) and EditingFinished when Acceptable.
void finish(UiContext& ui, NodeHandle n, bool enter) {
    Validation v = leStateOf(ui, n);
    const LineEditState* st0 = leState(ui, n);
    if (v != Validation::Acceptable && st0 && st0->o.fixup) {
        std::string t(leText(ui, n));
        st0->o.fixup(t);
        if (t != leText(ui, n)) {
            setLineEditText(ui, n, t);
            v = leStateOf(ui, n);
        }
    }
    if (v != Validation::Acceptable || !ui.alive(n)) return;
    LineEditState& st = leEnsure(ui, n);
    if (!enter && !st.dirty) return;
    st.dirty = false;
    const std::string t(leText(ui, n));
    if (enter) ui.emit(n, SignalKind::Submitted, {.text = t});
    if (ui.alive(n)) ui.emit(n, SignalKind::EditingFinished, {.text = t});
}
/// Input-mask editing: true when consumed.
bool maskEvent(UiContext& ui, NodeHandle n, LineEditState& st, const Event& e) {
    auto& eb = ui.editBuffer(n);
    const std::string& d = eb.text();
    std::size_t cur = codePoints(std::string_view(d).substr(0, eb.cursor()));
    const std::size_t anc = codePoints(std::string_view(d).substr(0, eb.anchor()));
    const std::size_t a = std::min(cur, anc), b = std::max(cur, anc);
    const bool ctrl = e.mods & kModCtrl;
    // caret keys and everything else that does not touch the cells go on to the common handling (no copy for them)
    if (e.type == EventType::KeyDown) {
        const bool own = e.key == Key::Backspace || e.key == Key::Delete ||
                         (ctrl && (e.key == Key::X || e.key == Key::C || e.key == Key::V || e.key == Key::Z || e.key == Key::Y));
        if (!own) return false;
    } else if (e.type != EventType::Text) {
        return false;
    }
    const std::u32string before = st.vals;
    if (e.type == EventType::Text) {
        if (st.o.readOnly) return true;
        if (a != b) { maskClear(st, a, b); cur = a; }
        cur = maskType(st, cur, decode32(e.text));
    } else if (e.type == EventType::KeyDown) {
        if (e.key == Key::Backspace || e.key == Key::Delete) {
            if (st.o.readOnly) return true;
            if (a != b) { maskClear(st, a, b); cur = a; }
            else if (e.key == Key::Backspace) {
                std::size_t j = cur;
                while (j > 0 && !st.cells[j - 1].kind) --j;
                if (j > 0) { st.vals[j - 1] = 0; cur = j - 1; }
            } else {
                std::size_t j = cur;
                while (j < st.cells.size() && !st.cells[j].kind) ++j;
                if (j < st.cells.size()) st.vals[j] = 0;
            }
        } else if (ctrl && (e.key == Key::X || e.key == Key::C)) {
            if (a == b || leMasked(st.o, true) || st.o.echo == EchoMode::NoEcho) return true;
            ui.setClipboardText(maskStripped(st, a, b));
            if (e.key == Key::C || st.o.readOnly) return true;
            maskClear(st, a, b);
            cur = a;
        } else if (ctrl && e.key == Key::V) {
            if (st.o.readOnly) return true;
            std::string paste;
            if (!ui.clipboardText(paste)) return true;
            if (a != b) { maskClear(st, a, b); cur = a; }
            cur = maskType(st, cur, decode32(paste));
        } else if (ctrl && (e.key == Key::Z || e.key == Key::Y)) {
            return true; // no undo log for masked input (the buffer is rebuilt per edit)
        } else {
            return false;
        }
    } else {
        return false;
    }
    if (st.vals == before) { maskSync(ui, n, st, cur); repaint(ui, n); return true; }
    if (leValidate(st.o, maskStripped(st)) == Validation::Invalid) { st.vals = before; return true; }
    maskSync(ui, n, st, cur);
    changed(ui, n);
    return true;
}
/// Runs one edit; rejected (buffer restored) when it makes the text Invalid.
template <class F>
bool guarded(UiContext& ui, NodeHandle n, F&& edit) {
    auto& eb = ui.editBuffer(n);
    const LineEditOptions& o = leOpts(ui, n);
    if (o.readOnly) return false;
    if (!o.validator) { edit(eb); return true; }
    fuse::text::TextEditBuffer saved = eb;
    edit(eb);
    if (eb.text() != saved.text() && leValidate(o, eb.text()) == Validation::Invalid) {
        eb = std::move(saved);
        return false;
    }
    return true;
}
/// Insert, truncated to the code points maxLength leaves (the selection is replaced, so it counts as free).
bool insertText(UiContext& ui, NodeHandle n, std::string_view s) {
    auto& eb = ui.editBuffer(n);
    const LineEditOptions& o = leOpts(ui, n);
    const std::size_t have = codePoints(eb.text()) - codePoints(eb.selection());
    const std::size_t room = o.maxLength > have ? o.maxLength - have : 0;
    const std::string_view put = clipCodePoints(s, room);
    if (put.empty() && !s.empty() && !eb.hasSelection()) return false;
    return guarded(ui, n, [&](fuse::text::TextEditBuffer& b) { b.insert(put); });
}

} // namespace

namespace detail {

bool lineEditHasState(const UiContext& ui, NodeHandle n) { return !table().empty() && leState(ui, n) != nullptr; }
void lineEditEnsureState(UiContext& ui, NodeHandle n) {
    if (isLineEdit(ui, n)) leEnsure(ui, n);
}

void lineEditPaint(const PaintContext& pc, DrawList& dl) {
    UiContext& ui = pc.ui;
    const Rect c = pc.content;
    auto& eb = ui.editBuffer(pc.node);
    const LineEditOptions& o = leOpts(ui, pc.node);
    const bool focused = pc.states & kStateFocus;
    std::string local;
    LineEditState* st = leState(ui, pc.node);
    const Shown sh = shownText(o, eb.text(), focused, st ? st->shown : local);
    const std::string_view t = sh.text;
    const std::size_t cur = sh.map(eb.cursor());
    const f32 px = pc.style.fontSize;
    // IME pre-edit is shown inline at the cursor, underlined, with its own caret; it is not in the buffer.
    // A masked field shows no pre-edit (as Qt: password fields turn input methods off).
    const std::string_view pre = focused && !sh.masked && !sh.hidden && !o.readOnly ? ui.composition() : std::string_view{};
    const f32 cx = ui.measureString(t.substr(0, cur), px);
    const f32 preW = pre.empty() ? 0.f : ui.measureString(pre, px);
    const f32 caretX = cx + (pre.empty() ? 0.f : ui.measureString(pre.substr(0, std::size_t(ui.compositionCaret())), px));
    const f32 scrollX = std::max(0.f, caretX - c.w + 2.f);
    dl.pushClip(c);
    if (focused && eb.hasSelection() && pre.empty() && !sh.hidden) {
        const std::size_t a = std::min(cur, sh.map(eb.anchor())), b = std::max(cur, sh.map(eb.anchor()));
        const f32 x0 = ui.measureString(t.substr(0, a), px), x1 = ui.measureString(t.substr(0, b), px);
        dl.rect({c.x + x0 - scrollX, c.y, x1 - x0, c.h}, accent(pc).withAlpha(0.45f * pc.opacity));
    }
    if (eb.text().empty() && pre.empty() && !o.placeholder.empty()) {
        const Color pcol = pc.tint(placeholderColor(pc));
        ui.paintString(dl, o.placeholder, {c.x, c.y, c.w + 1000.f, c.h}, pcol, px, TextAlignment::Start, pc.node.index);
    } else if (pre.empty()) {
        labelAt(pc, dl, t, {c.x - scrollX, c.y, c.w + scrollX + 1000.f, c.h});
    } else {
        const f32 x = c.x - scrollX;
        labelAt(pc, dl, t.substr(0, cur), {x, c.y, cx + 1000.f, c.h});
        labelAt(pc, dl, pre, {x + cx, c.y, preW + 1000.f, c.h});
        labelAt(pc, dl, t.substr(cur), {x + cx + preW, c.y, c.w + 1000.f, c.h});
        dl.rect({std::round(x + cx), std::round(c.y + c.h - 3.f), preW, 1.f}, textCol(pc));
    }
    if (focused) {
        const Rect caret{std::round(c.x + caretX - scrollX), c.y + 2.f, 1.5f, c.h - 4.f};
        if (!o.readOnly) dl.rect(caret, textCol(pc));
        ui.setTextCaret(pc.node, caret);
    }
    dl.popClip();
}

bool lineEditEvent(EventContext& ec, const Event& e) {
    if (ec.phase != Phase::Target) return false;
    UiContext& ui = ec.ui;
    if (e.type == EventType::FocusOut) {
        if (!completerFocusOut(ui, ec.node)) finish(ui, ec.node, false);
        if (ui.alive(ec.node)) repaint(ui, ec.node);
        return false;
    }
    if (e.type == EventType::FocusIn) { repaint(ui, ec.node); return false; } // echo-on-edit repaint
    if (e.type == EventType::KeyDown && completerKey(ui, ec.node, e)) return true;
    auto& eb = ui.editBuffer(ec.node);
    const LineEditOptions& o = leOpts(ui, ec.node);
    if (LineEditState* st = leState(ui, ec.node); st && !st->cells.empty() && e.type != EventType::PointerDown) {
        if (e.type == EventType::KeyDown && e.key == Key::Enter) { finish(ui, ec.node, true); return true; }
        if (maskEvent(ui, ec.node, *st, e)) return true;
    }
    const bool masked = o.echo != EchoMode::Normal && o.echo != EchoMode::PasswordEchoOnEdit;
    if (e.type == EventType::Text) {
        if (insertText(ui, ec.node, e.text)) changed(ui, ec.node);
        return true;
    }
    if (e.type == EventType::Composition) { repaint(ui, ec.node); return true; }
    if (e.type == EventType::PointerDown) {
        const Rect c = ui.contentRect(ec.node);
        const std::string& t = eb.text();
        std::string local;
        LineEditState* ls = leState(ui, ec.node);
        const Shown sh = shownText(o, t, true, ls ? ls->shown : local);
        eb.home(false);
        const f32 px = ui.computed(ec.node).fontSize;
        const std::string_view st = sh.text;
        while (eb.cursor() < t.size()) {
            const std::size_t nx = fuse::text::nextBoundary(t, eb.cursor());
            const f32 mid = (ui.measureString(st.substr(0, sh.map(eb.cursor())), px) + ui.measureString(st.substr(0, sh.map(nx)), px)) * 0.5f;
            if (c.x + mid > e.pos.x) break;
            eb.moveRight(false);
        }
        repaint(ui, ec.node);
        return true;
    }
    if (e.type != EventType::KeyDown) return false;
    const bool sh = e.mods & kModShift, ctrl = e.mods & kModCtrl;
    bool edit = false;
    const bool deletion = e.key == Key::Backspace || e.key == Key::Delete || e.key == Key::X;
    // masked fields jump by whole fields with Ctrl (no word boundaries leak through, as Qt)
    switch (e.key) {
    case Key::Left: ctrl ? (masked ? eb.home(sh) : eb.wordLeft(sh)) : eb.moveLeft(sh); break;
    case Key::Right: ctrl ? (masked ? eb.end(sh) : eb.wordRight(sh)) : eb.moveRight(sh); break;
    case Key::Home: eb.home(sh); break;
    case Key::End: eb.end(sh); break;
    case Key::Backspace: edit = guarded(ui, ec.node, [](fuse::text::TextEditBuffer& b) { b.backspace(); }); break;
    case Key::Delete: edit = guarded(ui, ec.node, [](fuse::text::TextEditBuffer& b) { b.del(); }); break;
    case Key::A: if (!ctrl) return false; eb.selectAll(); break;
    case Key::C:
    case Key::X:
        if (!ctrl) return false;
        if (!eb.hasSelection() || leMasked(o, true) || o.echo == EchoMode::NoEcho) return true; // no copying secrets
        ui.setClipboardText(eb.selection());
        if (e.key == Key::X) edit = guarded(ui, ec.node, [](fuse::text::TextEditBuffer& b) { b.insert({}); });
        break;
    case Key::V: {
        if (!ctrl) return false;
        std::string paste;
        if (!ui.clipboardText(paste)) return true;
        for (char& ch : paste) if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' '; // single line
        edit = insertText(ui, ec.node, paste);
        break;
    }
    case Key::Z: if (!ctrl) return false; edit = guarded(ui, ec.node, [&](fuse::text::TextEditBuffer& b) { sh ? b.redo() : b.undo(); }); break;
    case Key::Y: if (!ctrl) return false; edit = guarded(ui, ec.node, [](fuse::text::TextEditBuffer& b) { b.redo(); }); break;
    case Key::Enter: finish(ui, ec.node, true); return true;
    default: return false;
    }
    if (edit) changed(ui, ec.node, deletion);
    else repaint(ui, ec.node);
    return true;
}

void lineEditComplete(UiContext& ui, NodeHandle n, std::string_view text, std::size_t selFrom) {
    auto& eb = ui.editBuffer(n);
    const std::string_view t = clipCodePoints(text, leOpts(ui, n).maxLength);
    eb.setText(t);
    eb.end(false);
    if (selFrom != std::string::npos && selFrom < t.size()) {
        eb.home(false);
        while (eb.cursor() < selFrom) eb.moveRight(false);
        eb.end(true);
    }
    publishText(ui, n);
    leEnsure(ui, n).dirty = true;
    ui.emit(n, SignalKind::TextChanged, {.text = leText(ui, n)});
}

} // namespace detail

void setLineEditOptions(UiContext& ui, NodeHandle n, LineEditOptions o) {
    if (!isLineEdit(ui, n)) return;
    LineEditState& s = leEnsure(ui, n);
    const std::string old(leText(ui, n));
    const bool wasMasked = !s.cells.empty();
    s.o = std::move(o);
    s.cells.clear();
    s.vals.clear();
    s.text.clear();
    auto& eb = ui.editBuffer(n);
    if (!s.o.inputMask.empty()) {
        parseMask(s.o.inputMask, s);
        maskType(s, 0, decode32(old));
        maskSync(ui, n, s, 0);
    } else {
        if (wasMasked) { eb.setText(old); eb.end(false); }
        if (codePoints(eb.text()) > s.o.maxLength) { eb.setText(clipCodePoints(eb.text(), s.o.maxLength)); eb.end(false); }
    }
    publishText(ui, n);
    repaint(ui, n);
}
const LineEditOptions& lineEditOptions(const UiContext& ui, NodeHandle n) { return leOpts(ui, n); }
std::string_view lineEditText(UiContext& ui, NodeHandle n) { return isLineEdit(ui, n) ? leText(ui, n) : std::string_view{}; }
Validation lineEditState(UiContext& ui, NodeHandle n) { return isLineEdit(ui, n) ? leStateOf(ui, n) : Validation::Invalid; }
void setLineEditText(UiContext& ui, NodeHandle n, std::string_view t) {
    if (!isLineEdit(ui, n)) return;
    auto& eb = ui.editBuffer(n);
    if (LineEditState* st = leState(ui, n); st && !st->cells.empty()) {
        st->vals.assign(st->cells.size(), 0);
        maskType(*st, 0, decode32(t));
        maskSync(ui, n, *st, st->cells.size());
    } else {
        eb.setText(clipCodePoints(t, leOpts(ui, n).maxLength));
        eb.end(false);
    }
    publishText(ui, n);
    ui.emit(n, SignalKind::TextChanged, {.text = leText(ui, n)});
}

// ---- accessibility: SetValue on a field with options goes through them ------------------------------------------------
// (The core's default for a text field writes the node text and the edit buffer directly: that would put a password
// in the node text and skip validators, masks and read-only.)
namespace {
bool lineEditA11yAction(UiContext& ui, NodeHandle n, u32 child, A11yAction a, f32, std::string_view text) {
    if (child || a != A11yAction::SetValue) return false;
    LineEditState* st = leState(ui, n);
    if (!st) return false; // a plain field: the core sets the text
    if (st->o.readOnly) return true;
    const std::string t(clipCodePoints(text, st->o.maxLength));
    if (st->cells.empty() && leValidate(st->o, t) == Validation::Invalid) return true; // refused: nothing changes
    setLineEditText(ui, n, t); // (an input mask types the characters into its cells)
    if (ui.alive(n)) leEnsure(ui, n).dirty = true;
    return true;
}
} // namespace
void detail::a11yLineEdit(WidgetRegistry& r) { a11yHook(r, "lineedit", A11yRole::TextField, nullptr, &lineEditA11yAction); }

Fixup intFixup(long long lo, long long hi) {
    return [lo, hi](std::string& t) {
        char* end = nullptr;
        const std::string s = t;
        long long v = std::strtoll(s.c_str(), &end, 10);
        if (end == s.c_str()) v = lo > 0 ? lo : hi < 0 ? hi : 0;
        t = std::to_string(std::clamp(v, lo, hi));
    };
}
Fixup doubleFixup(double lo, double hi, int decimals) {
    return [lo, hi, decimals](std::string& t) {
        char* end = nullptr;
        const std::string s = t;
        double v = std::strtod(s.c_str(), &end);
        if (end == s.c_str()) v = lo > 0 ? lo : hi < 0 ? hi : 0.0;
        v = std::clamp(v, lo, hi);
        char buf[64];
        if (decimals >= 17) std::snprintf(buf, sizeof buf, "%.17g", v);
        else std::snprintf(buf, sizeof buf, "%.*f", std::max(decimals, 0), v);
        t = buf;
    };
}

namespace {
/// Sign + digits prefix of an integer literal: "", "-", "+" are partial.
bool intLiteral(std::string_view t, bool& partial) {
    std::size_t i = 0;
    if (i < t.size() && (t[i] == '-' || t[i] == '+')) ++i;
    partial = i == t.size();
    for (std::size_t k = i; k < t.size(); ++k)
        if (t[k] < '0' || t[k] > '9') return false;
    return true;
}
} // namespace
Validator intValidator(long long lo, long long hi) {
    return [lo, hi](std::string_view t) {
        bool partial = false;
        if (!intLiteral(t, partial)) return Validation::Invalid;
        if (partial) return (t.empty() || (t[0] == '-' ? lo < 0 : hi >= 0)) ? Validation::Intermediate : Validation::Invalid;
        if (t[0] == '-' && lo >= 0) return Validation::Invalid;
        const std::size_t digits = t.size() - (t[0] == '-' || t[0] == '+');
        if (digits > 19) return Validation::Invalid;
        const long long v = std::strtoll(std::string(t).c_str(), nullptr, 10);
        if (v >= lo && v <= hi) return Validation::Acceptable;
        // more digits only grow |v|: out of range for good when |v| is already past the bound on its side
        if (v > hi && v >= 0) return Validation::Invalid;
        if (v < lo && v < 0) return Validation::Invalid;
        return Validation::Intermediate;
    };
}
Validator doubleValidator(double lo, double hi, int decimals) {
    return [lo, hi, decimals](std::string_view t) {
        std::size_t i = 0;
        if (i < t.size() && (t[i] == '-' || t[i] == '+')) ++i;
        if (i < t.size() && t[0] == '-' && lo >= 0) return Validation::Invalid;
        std::size_t intDigits = 0, frac = 0;
        bool dot = false, expo = false, expDigits = false;
        for (std::size_t k = i; k < t.size(); ++k) {
            const char ch = t[k];
            if (ch >= '0' && ch <= '9') {
                if (expo) expDigits = true;
                else if (dot) ++frac;
                else ++intDigits;
                continue;
            }
            if (ch == '.' && !dot && !expo) { dot = true; continue; }
            if ((ch == 'e' || ch == 'E') && !expo && (intDigits || frac)) {
                expo = true;
                if (k + 1 < t.size() && (t[k + 1] == '-' || t[k + 1] == '+')) ++k;
                continue;
            }
            return Validation::Invalid;
        }
        if (frac > std::size_t(std::max(decimals, 0))) return Validation::Invalid;
        if (intDigits + frac == 0 || (expo && !expDigits)) return Validation::Intermediate;
        const double v = std::strtod(std::string(t).c_str(), nullptr);
        if (v >= lo && v <= hi) return Validation::Acceptable;
        if (!expo && !dot && ((v > hi && v >= 0) || (v < lo && v < 0))) return Validation::Invalid;
        return Validation::Intermediate;
    };
}

} // namespace fuse::ui::widgets
