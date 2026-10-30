// QCompleter parity for line edits: popup / inline / unfiltered-popup completion over an ItemModel column or a
// '\n' list, case and filter (starts-with / contains / ends-with) modes, keyboard navigation with focus kept in the
// field, and listbox / combobox a11y (autoComplete, expanded, controls, activeDescendant, selected).
#include "fuse/ui/widgets/widgets.hpp"
#include "widgets_internal.hpp"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <memory>
#include <unordered_map>

namespace fuse::ui::widgets {
namespace {

struct CState {
    u32 gen = 0;
    CompleterOptions o;
    std::vector<std::string> list; ///< `items` split (model == null)
    std::vector<u32> matches;      ///< model rows shown
    std::string prefix;            ///< the typed text the matches are for
    i32 cur = -1;                  ///< index into matches
    u32 first = 0;                 ///< first shown match (popup scroll)
    NodeHandle popup;
    bool shown = false;
    std::string scratch;
};
std::unordered_map<const UiContext*, std::vector<std::unique_ptr<CState>>>& stores() {
    static std::unordered_map<const UiContext*, std::vector<std::unique_ptr<CState>>> m;
    return m;
}
CState* stateOf(const UiContext& ui, NodeHandle n) {
    auto& m = stores();
    const auto it = m.find(&ui);
    if (it == m.end() || n.index >= it->second.size()) return nullptr;
    auto& s = it->second[n.index];
    return s && s->gen == n.generation && ui.alive(n) ? s.get() : nullptr;
}

u32 rowCount(const CState& s) {
    if (s.o.model) return s.o.model->rows ? s.o.model->rows(s.o.model->user) : 0u;
    return u32(s.list.size());
}
std::string rowText(CState& s, u32 row) {
    if (!s.o.model) return row < s.list.size() ? s.list[row] : std::string{};
    if (!s.o.model->data) return {};
    return std::string(s.o.model->data(s.o.model->user, row, s.o.column, s.scratch));
}

/// simple (1:1) lower-case mapping, locale-independent for Latin-1 / Latin Extended-A / Greek / Cyrillic; other
/// scripts use towlower (which depends on the C locale)
char32_t lowerCp(char32_t c) {
    if (c < 0x80) return c >= 'A' && c <= 'Z' ? c + 32 : c;
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 32;
    if (c >= 0x100 && c <= 0x137 && c != 0x130) return c | 1u;
    if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return (c & 1u) ? c + 1 : c;
    if (c >= 0x14A && c <= 0x177) return c | 1u;
    if (c == 0x178) return 0xFF;
    if (c >= 0x391 && c <= 0x3AB && c != 0x3A2) return c + 32;
    if (c >= 0x410 && c <= 0x42F) return c + 32;
    if (c >= 0x400 && c <= 0x40F) return c + 80;
    if (c < 0x530) return c;
    return sizeof(wchar_t) >= 4 || c < 0x10000 ? char32_t(std::towlower(wint_t(c))) : c;
}
std::u32string fold(std::string_view s, bool cs) {
    std::u32string o;
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        int k = c < 0x80 ? 0 : c < 0xE0 ? 1 : c < 0xF0 ? 2 : 3;
        char32_t cp = k == 0 ? c : k == 1 ? (c & 0x1F) : k == 2 ? (c & 0x0F) : (c & 0x07);
        ++i;
        for (; k > 0 && i < s.size(); --k, ++i) cp = (cp << 6) | (static_cast<unsigned char>(s[i]) & 0x3F);
        if (!cs) cp = lowerCp(cp);
        o += cp;
    }
    return o;
}
bool matches(std::u32string_view text, std::u32string_view pre, MatchMode m) {
    if (pre.size() > text.size()) return false;
    switch (m) {
    case MatchMode::StartsWith: return text.substr(0, pre.size()) == pre;
    case MatchMode::EndsWith: return text.substr(text.size() - pre.size()) == pre;
    case MatchMode::Contains: return text.find(pre) != std::u32string_view::npos;
    }
    return false;
}
/// rebuild s.matches for `prefix`; all rows when `all`
void refilter(CState& s, std::string_view prefix, bool all) {
    s.prefix.assign(prefix);
    s.matches.clear();
    s.cur = -1;
    s.first = 0;
    const MatchMode mm = s.o.mode == CompletionMode::Inline ? MatchMode::StartsWith : s.o.filter;
    const std::u32string pre = fold(prefix, s.o.caseSensitive);
    const u32 n = rowCount(s);
    for (u32 r = 0; r < n; ++r) {
        const bool hit = matches(fold(rowText(s, r), s.o.caseSensitive), pre, mm);
        if (all || hit) s.matches.push_back(r);
        if (all && hit && s.cur < 0 && !prefix.empty() && s.o.mode == CompletionMode::UnfilteredPopup) s.cur = i32(s.matches.size() - 1);
    }
}

f32 rowH(UiContext& ui, NodeHandle n) {
    const f32 px = ui.computed(n).fontSize;
    return std::ceil(ui.font() ? ui.font()->metrics(px).lineHeight() : px * 1.25f) + 6.f;
}

struct CompleterList {
    static constexpr std::string_view kName = "completer";
    static constexpr A11yRole kRole = A11yRole::List;
    static ComputedStyle defaults() {
        ComputedStyle s;
        s.direction = Direction::Column;
        return s;
    }
    static bool onEvent(EventContext& ec, const Event& e);
};
struct CompleterItem { // value = index into matches
    static constexpr std::string_view kName = "completeritem";
    static constexpr A11yRole kRole = A11yRole::ListItem;
    static constexpr bool kInteractive = true;
    static Size measure(UiContext& ui, NodeHandle n, Size) {
        return {std::ceil(ui.measureString(ui.text(n), ui.computed(n).fontSize)), rowH(ui, n) - 6.f};
    }
    static void paint(const PaintContext& pc, DrawList& dl) {
        const Color acc = pc.tint(pc.style.background2.a > 0.f ? pc.style.background2 : Color::rgb(0x3D8BFD));
        if (pc.ui.checked(pc.node)) dl.rect(pc.rect, acc.withAlpha(0.35f * pc.opacity));
        else if (pc.states & kStateHover) dl.rect(pc.rect, acc.withAlpha(0.15f * pc.opacity));
        pc.ui.paintString(dl, pc.ui.text(pc.node), pc.content, pc.tint(pc.style.color), pc.style.fontSize, TextAlignment::Start, pc.node.index);
    }
    static bool onEvent(EventContext& ec, const Event& e);
};

NodeHandle editOf(const UiContext& ui, NodeHandle popup) {
    const u64 v = ui.userData(popup);
    return NodeHandle{u32(v & 0xFFFFFFFFu), u32(v >> 32)};
}

void syncA11y(UiContext& ui, NodeHandle edit, CState& s) {
    A11yState a = ui.a11yState(edit);
    a.autoComplete = s.o.mode == CompletionMode::Inline ? A11yAutoComplete::Inline : A11yAutoComplete::List;
    a.expanded = s.o.mode == CompletionMode::Inline ? -1 : s.shown ? 1 : 0;
    a.controls = s.shown ? s.popup : NodeHandle{};
    a.activeDescendant = {};
    if (s.shown && s.cur >= 0)
        for (NodeHandle it = ui.firstChild(s.popup); ui.alive(it); it = ui.nextSibling(it))
            if (i32(ui.value(it)) == s.cur) a.activeDescendant = it;
    ui.setA11yState(edit, a);
}

/// fill the popup rows from s.first (creates / destroys row nodes to the shown count)
void layoutRows(UiContext& ui, NodeHandle edit, CState& s, bool follow = true) {
    if (!ui.alive(s.popup)) return;
    const u32 vis = std::min<u32>(std::max(1u, s.o.maxVisibleItems), u32(s.matches.size()));
    if (follow && s.cur >= 0) { // keep the highlight in view
        if (u32(s.cur) < s.first) s.first = u32(s.cur);
        else if (u32(s.cur) >= s.first + vis) s.first = u32(s.cur) - vis + 1;
    }
    s.first = std::min<u32>(s.first, u32(s.matches.size()) - vis);
    u32 have = 0;
    for (NodeHandle it = ui.firstChild(s.popup); ui.alive(it); it = ui.nextSibling(it)) ++have;
    while (have > vis) { NodeHandle last; for (NodeHandle it = ui.firstChild(s.popup); ui.alive(it); it = ui.nextSibling(it)) last = it; ui.destroy(last); --have; }
    while (have < vis) {
        auto h = ui.create("completeritem", s.popup);
        if (!h) break;
        ui.setRange(*h, 0.f, 1e9f, 0.f);
        ++have;
    }
    u32 i = s.first;
    for (NodeHandle it = ui.firstChild(s.popup); ui.alive(it); it = ui.nextSibling(it), ++i) {
        ui.setText(it, rowText(s, s.matches[i]));
        ui.setValue(it, f32(i), false);
        const bool sel = i32(i) == s.cur;
        ui.setChecked(it, sel);
        A11yState a;
        a.selected = sel;
        ui.setA11yState(it, a);
    }
    ui.invalidate(s.popup);
    syncA11y(ui, edit, s);
}

void hide(UiContext& ui, NodeHandle edit, CState& s) {
    if (s.shown && ui.alive(s.popup)) ui.setCss(s.popup, "display", "none");
    s.shown = false;
    s.cur = -1;
    syncA11y(ui, edit, s);
}

void show(UiContext& ui, NodeHandle edit, CState& s) {
    if (s.matches.empty()) { hide(ui, edit, s); return; }
    const NodeHandle layer = overlayLayer(ui);
    if (!ui.alive(s.popup)) {
        auto h = ui.create("completer", layer);
        if (!h) return;
        s.popup = *h;
        ui.setUserData(s.popup, (u64(edit.generation) << 32) | u64(edit.index));
        ui.addClass(s.popup, "card");
        ui.addClass(s.popup, "clamp"); // kept on screen; its own OS window with OS popups
        ui.setA11y(s.popup, A11yRole::List, "Completions");
    } else if (ui.parent(s.popup) != layer) {
        ui.moveTo(s.popup, layer);
    }
    const Rect r = ui.rect(edit);
    ui.set(s.popup, Prop::Left, PropValue::ofLength(Length::px(r.x)));
    ui.set(s.popup, Prop::Top, PropValue::ofLength(Length::px(r.bottom() + 2.f)));
    ui.set(s.popup, Prop::MinWidth, PropValue::ofLength(Length::px(r.w)));
    ui.setCss(s.popup, "display", "flex");
    s.shown = true;
    layoutRows(ui, edit, s);
}

/// put the highlighted completion (or the prefix for -1) in the field and emit Highlighted
void highlight(UiContext& ui, NodeHandle edit, CState& s, i32 to) {
    s.cur = to;
    if (to < 0) detail::lineEditComplete(ui, edit, s.prefix, std::string::npos);
    else {
        const std::string t = rowText(s, s.matches[u32(to)]);
        if (s.o.mode == CompletionMode::Popup) detail::lineEditComplete(ui, edit, t, std::string::npos);
        ui.emit(edit, SignalKind::Highlighted, {.value = f32(s.matches[u32(to)]), .text = t});
    }
    if (s.shown) layoutRows(ui, edit, s);
}

void activate(UiContext& ui, NodeHandle edit, CState& s, i32 idx) {
    if (idx < 0 || u32(idx) >= s.matches.size()) return;
    const u32 row = s.matches[u32(idx)];
    const std::string t = rowText(s, row);
    hide(ui, edit, s);
    detail::lineEditComplete(ui, edit, t, std::string::npos);
    s.prefix = t;
    ui.emit(edit, SignalKind::Activated, {.value = f32(row), .text = t});
}

bool CompleterList::onEvent(EventContext& ec, const Event& e) {
    if (e.type != EventType::Wheel) return false;
    const NodeHandle edit = editOf(ec.ui, ec.node);
    CState* s = stateOf(ec.ui, edit);
    if (!s || !s->shown) return false;
    const i64 f = i64(s->first) - i64(std::lround(e.wheel.y * 3.f));
    s->first = u32(std::max<i64>(0, f));
    layoutRows(ec.ui, edit, *s, false); // scrolling does not drag the highlight into view
    return true;
}
bool CompleterItem::onEvent(EventContext& ec, const Event& e) {
    if (e.type != EventType::Click || ec.phase != Phase::Target) return false;
    const NodeHandle popup = ec.ui.parent(ec.node);
    const NodeHandle edit = editOf(ec.ui, popup);
    if (CState* s = stateOf(ec.ui, edit)) activate(ec.ui, edit, *s, i32(ec.ui.value(ec.node)));
    return true;
}

void registerCompleter(UiContext& ui) {
    auto& r = ui.registry();
    if (r.find("completer") != 0xFFFFFFFFu) return;
    r.registerWidget<CompleterList>();
    r.registerWidget<CompleterItem>();
}

} // namespace

namespace detail {
bool completerKey(UiContext& ui, NodeHandle edit, const Event& e) {
    CState* s = stateOf(ui, edit);
    if (!s || e.type != EventType::KeyDown) return false;
    const i32 n = i32(s->matches.size());
    const bool ctrl = e.mods & kModCtrl;
    if (s->o.mode == CompletionMode::Inline) {
        if ((e.key == Key::Up || e.key == Key::Down) && n > 0) { // cycle the inline completion
            const i32 to = s->cur < 0 ? (e.key == Key::Down ? 0 : n - 1) : (s->cur + (e.key == Key::Down ? 1 : n - 1)) % n;
            s->cur = to;
            const std::string t = rowText(*s, s->matches[u32(to)]);
            std::string full = s->prefix;
            full += std::string_view(t).substr(std::min(t.size(), s->prefix.size()));
            lineEditComplete(ui, edit, full, s->prefix.size());
            ui.emit(edit, SignalKind::Highlighted, {.value = f32(s->matches[u32(to)]), .text = t});
            return true;
        }
        if (e.key == Key::Enter && s->cur >= 0 && ui.editBuffer(edit).hasSelection()) {
            ui.editBuffer(edit).end(false);
            const std::string t(lineEditText(ui, edit));
            ui.emit(edit, SignalKind::Activated, {.value = f32(s->matches[u32(s->cur)]), .text = t});
            s->prefix = t;
            s->cur = -1;
        }
        return false;
    }
    if (!s->shown) {
        if (e.key == Key::Down && !ctrl) {
            complete(ui, edit);
            return true;
        }
        return false;
    }
    const i32 page = i32(std::max(1u, s->o.maxVisibleItems));
    switch (e.key) {
    case Key::Down:
        highlight(ui, edit, *s, s->cur < 0 ? 0 : s->cur + 1 < n ? s->cur + 1 : s->o.wrapAround ? -1 : s->cur);
        return true;
    case Key::Up:
        highlight(ui, edit, *s, s->cur < 0 ? (s->o.wrapAround ? n - 1 : -1) : s->cur - 1);
        return true;
    case Key::PageDown: highlight(ui, edit, *s, std::min(n - 1, std::max(0, s->cur) + page)); return true;
    case Key::PageUp: highlight(ui, edit, *s, std::max(0, s->cur - page)); return true;
    case Key::Home: if (!ctrl) return false; highlight(ui, edit, *s, 0); return true;
    case Key::End: if (!ctrl) return false; highlight(ui, edit, *s, n - 1); return true;
    case Key::Escape:
        if (s->cur >= 0 && s->o.mode == CompletionMode::Popup) lineEditComplete(ui, edit, s->prefix, std::string::npos);
        hide(ui, edit, *s);
        return true;
    case Key::Enter:
    case Key::Tab:
        if (s->cur >= 0) activate(ui, edit, *s, s->cur);
        else hide(ui, edit, *s);
        return false; // Enter still submits (returnPressed), Tab still moves focus
    default: return false;
    }
}
void completerEdited(UiContext& ui, NodeHandle edit, bool deletion) {
    CState* s = stateOf(ui, edit);
    if (!s) return;
    const std::string text(lineEditText(ui, edit));
    if (s->o.mode == CompletionMode::Inline) {
        refilter(*s, text, false);
        auto& eb = ui.editBuffer(edit);
        if (deletion || s->matches.empty() || text.empty() || eb.hasSelection() || eb.cursor() != eb.text().size()) return;
        s->cur = 0;
        const std::string t = rowText(*s, s->matches[0]);
        if (t.size() <= text.size()) return;
        lineEditComplete(ui, edit, text + t.substr(text.size()), text.size());
        return;
    }
    if (text.empty() && s->o.mode == CompletionMode::Popup) { refilter(*s, text, false); hide(ui, edit, *s); return; }
    refilter(*s, text, s->o.mode == CompletionMode::UnfilteredPopup);
    show(ui, edit, *s);
}
void completerFocusOut(UiContext& ui, NodeHandle edit) {
    if (CState* s = stateOf(ui, edit); s && s->shown) hide(ui, edit, *s);
}
} // namespace detail

void setCompleter(UiContext& ui, NodeHandle edit, CompleterOptions o) {
    if (!ui.alive(edit)) return;
    registerCompleter(ui);
    auto& v = stores()[&ui];
    if (v.size() <= edit.index) v.resize(edit.index + 1);
    auto& slot = v[edit.index];
    if (slot && slot->gen == edit.generation && ui.alive(slot->popup)) ui.destroy(slot->popup);
    if (!o.model && o.items.empty()) {
        slot.reset();
        ui.setA11yState(edit, {});
        return;
    }
    slot = std::make_unique<CState>();
    slot->gen = edit.generation;
    slot->o = std::move(o);
    if (!slot->o.model) {
        std::string_view it = slot->o.items;
        for (std::size_t at = 0;;) {
            const std::size_t nl = it.find('\n', at);
            slot->list.emplace_back(it.substr(at, nl == std::string_view::npos ? std::string_view::npos : nl - at));
            if (nl == std::string_view::npos) break;
            at = nl + 1;
        }
    }
    syncA11y(ui, edit, *slot);
}
void completerModelChanged(UiContext& ui, NodeHandle edit) {
    CState* s = stateOf(ui, edit);
    if (!s) return;
    const bool was = s->shown;
    refilter(*s, s->prefix, s->o.mode == CompletionMode::UnfilteredPopup || (was && s->prefix.empty()));
    if (was) show(ui, edit, *s);
}
void complete(UiContext& ui, NodeHandle edit) {
    CState* s = stateOf(ui, edit);
    if (!s) return;
    const std::string text(lineEditText(ui, edit));
    refilter(*s, text, text.empty() || s->o.mode == CompletionMode::UnfilteredPopup);
    if (s->o.mode == CompletionMode::Inline) return;
    show(ui, edit, *s);
}
u32 completionCount(UiContext& ui, NodeHandle edit) {
    const CState* s = stateOf(ui, edit);
    return s ? u32(s->matches.size()) : 0u;
}
std::string completionAt(UiContext& ui, NodeHandle edit, u32 i) {
    CState* s = stateOf(ui, edit);
    return s && i < s->matches.size() ? rowText(*s, s->matches[i]) : std::string{};
}
i32 completerCurrentRow(UiContext& ui, NodeHandle edit) {
    const CState* s = stateOf(ui, edit);
    return s ? s->cur : -1;
}
bool completerPopupVisible(const UiContext& ui, NodeHandle edit) {
    const CState* s = stateOf(ui, edit);
    return s && s->shown;
}
NodeHandle completerPopup(const UiContext& ui, NodeHandle edit) {
    const CState* s = stateOf(ui, edit);
    return s ? s->popup : NodeHandle{};
}

} // namespace fuse::ui::widgets
