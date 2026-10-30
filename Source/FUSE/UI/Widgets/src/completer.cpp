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
    const bool sorted = s.o.sorting == (s.o.caseSensitive ? ModelSorting::CaseSensitivelySorted : ModelSorting::CaseInsensitivelySorted);
    if (!all && sorted && mm == MatchMode::StartsWith) {
        u32 lo = 0, hi = n; // first row not less than the prefix
        while (lo < hi) {
            const u32 mid = lo + (hi - lo) / 2;
            if (fold(rowText(s, mid), s.o.caseSensitive) < pre) lo = mid + 1;
            else hi = mid;
        }
        for (u32 r = lo; r < n && matches(fold(rowText(s, r), s.o.caseSensitive), pre, mm); ++r) s.matches.push_back(r);
        return;
    }
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
        Rect tr = pc.content;
        if (const u64 ic = pc.ui.userData(pc.node)) { // icon image index + 1
            const f32 sz = std::min(16.f, tr.h);
            DrawCmd c;
            c.op = DrawOp::Image;
            c.image = u32(ic - 1);
            c.rect = {tr.x, tr.y + (tr.h - sz) * 0.5f, sz, sz};
            c.fill = Color{1.f, 1.f, 1.f, pc.opacity};
            c.node = pc.node.index;
            dl.push(c);
            tr.x += sz + 4.f;
            tr.w -= sz + 4.f;
        }
        pc.ui.paintString(dl, pc.ui.text(pc.node), tr, pc.tint(pc.style.color), pc.style.fontSize, TextAlignment::Start, pc.node.index);
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
    if (const NodeHandle p = ui.parent(edit); ui.alive(p) && ui.classOf(p) == ui.registry().find("editcombo")) {
        A11yState c = ui.a11yState(p);
        c.expanded = s.shown ? 1 : 0;
        c.controls = a.controls;
        ui.setA11yState(p, c);
    }
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
        const u32 ic = s.o.icon ? s.o.icon(s.matches[i]) : kNoIcon;
        ui.setUserData(it, ic == kNoIcon ? 0u : u64(ic) + 1u);
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


// ---- editable combo box (QComboBox::setEditable) ----------------------------------------------------------------------
u32 itemCount(std::string_view s) { return s.empty() ? 0u : u32(std::count(s.begin(), s.end(), '\n') + 1); }
std::string_view itemAt(std::string_view s, u32 index) {
    std::size_t at = 0;
    for (u32 i = 0;; ++i) {
        const std::size_t nl = s.find('\n', at);
        if (i == index) return s.substr(at, nl == std::string_view::npos ? std::string_view::npos : nl - at);
        if (nl == std::string_view::npos) return {};
        at = nl + 1;
    }
}
struct ComboState {
    u32 gen = 0;
    EditableComboOptions o;
    const ItemModel* model = nullptr;
    u32 column = 0;
    NodeHandle edit, icon;
    std::vector<u32> icons; ///< per item (kNoIcon none), parallel to the items / model rows
    std::vector<u64> data;
};
std::unordered_map<const UiContext*, std::unordered_map<u32, ComboState>>& comboStores() {
    static std::unordered_map<const UiContext*, std::unordered_map<u32, ComboState>> m;
    return m;
}
ComboState& comboState(const UiContext& ui, NodeHandle combo) {
    ComboState& c = comboStores()[&ui][combo.index];
    if (c.gen != combo.generation) c = ComboState{combo.generation};
    return c;
}
InsertPolicy comboPolicy(const UiContext& ui, NodeHandle combo) { return comboState(ui, combo).o.policy; }
NodeHandle comboEdit(const UiContext& ui, NodeHandle combo) {
    if (!ui.alive(combo)) return {};
    const ComboState& c = comboState(ui, combo);
    return ui.alive(c.edit) ? c.edit : ui.firstChild(combo);
}
void comboSyncIcon(UiContext& ui, NodeHandle combo) {
    const ComboState& c = comboState(ui, combo);
    if (!ui.alive(c.icon)) return;
    const i32 cur = i32(ui.value(combo));
    const u32 ic = cur >= 0 && std::size_t(cur) < c.icons.size() ? c.icons[std::size_t(cur)] : kNoIcon;
    ui.setCss(c.icon, "display", ic == kNoIcon ? "none" : "flex");
    if (ic != kNoIcon) ui.setUserData(c.icon, ic);
    ui.invalidate(c.icon);
}
void comboInsertMeta(ComboState& c, std::size_t at, u32 icon, u64 data, std::size_t count) {
    c.icons.resize(std::max(c.icons.size(), count - 1), kNoIcon);
    c.data.resize(std::max(c.data.size(), count - 1), 0);
    c.icons.insert(c.icons.begin() + std::ptrdiff_t(std::min(at, c.icons.size())), icon);
    c.data.insert(c.data.begin() + std::ptrdiff_t(std::min(at, c.data.size())), data);
}
void comboSetCurrent(UiContext& ui, NodeHandle combo, i32 idx, bool setText) {
    const std::string_view items = ui.text(combo);
    const bool changed = i32(ui.value(combo)) != idx;
    ui.setValue(combo, f32(idx), false);
    const std::string t(idx >= 0 ? itemAt(items, u32(idx)) : std::string_view{});
    if (setText && idx >= 0) detail::lineEditComplete(ui, comboEdit(ui, combo), t, std::string::npos);
    if (CState* s = stateOf(ui, comboEdit(ui, combo))) s->prefix = std::string(lineEditText(ui, comboEdit(ui, combo)));
    if (changed) {
        ui.emit(combo, SignalKind::ValueChanged, {.value = f32(idx)});
        ui.emit(combo, SignalKind::TextChanged, {.text = t});
    }
    comboSyncIcon(ui, combo);
    ui.invalidate(combo);
}
void comboSetItems(UiContext& ui, NodeHandle combo, std::string items) {
    ui.setText(combo, items);
    const ComboState& cs = comboState(ui, combo);
    UiContext* up = &ui;
    auto icon = [up, combo](u32 row) {
        const ComboState& c = comboState(*up, combo);
        return row < c.icons.size() ? c.icons[row] : kNoIcon;
    };
    if (cs.model) setCompleter(ui, comboEdit(ui, combo), {.model = cs.model, .column = cs.column, .mode = CompletionMode::Inline, .icon = icon});
    else setCompleter(ui, comboEdit(ui, combo), {.items = std::move(items), .mode = CompletionMode::Inline, .icon = icon});
    if (itemCount(ui.text(combo)) == 0) setCompleter(ui, comboEdit(ui, combo), {});
}
std::string modelItems(const ItemModel* m, u32 col) {
    std::string out, scratch;
    const u32 n = m && m->rows ? m->rows(m->user) : 0u;
    for (u32 r = 0; r < n; ++r) {
        if (r) out += '\n';
        if (m->data) {
            std::string_view t = m->data(m->user, r, col, scratch);
            for (char ch : t) out += ch == '\n' ? ' ' : ch; // one line per item
        }
    }
    return out;
}
/// Enter in the field: select the matching item, else insert the text per the policy (QComboBox::insertPolicy)
void comboCommit(UiContext& ui, NodeHandle combo, std::string_view text) {
    const ComboState& cs = comboState(ui, combo);
    if (!cs.o.duplicatesEnabled || cs.model)
        if (const i32 at = comboFindText(ui, combo, text, cs.o.caseSensitive); at >= 0) {
            comboSetCurrent(ui, combo, at, false);
            if (lineEditText(ui, comboEdit(ui, combo)) != itemAt(ui.text(combo), u32(at))) // show the item's own case
                detail::lineEditComplete(ui, comboEdit(ui, combo), itemAt(ui.text(combo), u32(at)), std::string::npos);
            return;
        }
    const InsertPolicy pol = cs.o.policy;
    if (text.empty() || pol == InsertPolicy::NoInsert || cs.model) return;
    std::vector<std::string> v;
    const std::string_view items = ui.text(combo);
    for (u32 i = 0, n = itemCount(items); i < n; ++i) v.emplace_back(itemAt(items, i));
    const i32 cur = i32(ui.value(combo));
    std::size_t pos = v.size();
    bool replace = false;
    switch (pol) {
    case InsertPolicy::InsertAtTop: pos = 0; break;
    case InsertPolicy::InsertAtBottom: pos = v.size(); break;
    case InsertPolicy::InsertAlphabetically: {
        const std::u32string key = fold(text, false);
        pos = 0;
        while (pos < v.size() && fold(v[pos], false) <= key) ++pos;
        break;
    }
    case InsertPolicy::InsertAtCurrent: replace = cur >= 0 && std::size_t(cur) < v.size(); pos = replace ? std::size_t(cur) : v.size(); break;
    case InsertPolicy::InsertAfterCurrent: pos = cur >= 0 ? std::min(v.size(), std::size_t(cur) + 1) : v.size(); break;
    case InsertPolicy::InsertBeforeCurrent: pos = cur >= 0 ? std::min(v.size(), std::size_t(cur)) : 0; break;
    case InsertPolicy::NoInsert: return;
    }
    if (replace) v[pos] = std::string(text);
    else {
        if (v.size() >= cs.o.maxCount) return; // QComboBox::maxCount: full, nothing inserted
        v.insert(v.begin() + std::ptrdiff_t(pos), std::string(text));
        comboInsertMeta(comboState(ui, combo), pos, kNoIcon, 0, v.size());
    }
    std::string joined;
    for (std::size_t i = 0; i < v.size(); ++i) { if (i) joined += '\n'; joined += v[i]; }
    comboSetItems(ui, combo, std::move(joined));
    ui.setValue(combo, -2.f, false); // force "changed"
    comboSetCurrent(ui, combo, i32(pos), false);
}
void comboToggleList(UiContext& ui, NodeHandle combo) {
    const NodeHandle edit = comboEdit(ui, combo);
    CState* s = stateOf(ui, edit);
    if (!s) return;
    if (s->shown) { hide(ui, edit, *s); return; }
    refilter(*s, lineEditText(ui, edit), true);
    s->cur = i32(ui.value(combo)) < i32(s->matches.size()) ? i32(ui.value(combo)) : -1;
    show(ui, edit, *s);
    // the list hangs under the whole combo, not just the field
    const Rect r = ui.rect(combo);
    ui.set(s->popup, Prop::Left, PropValue::ofLength(Length::px(r.x)));
    ui.set(s->popup, Prop::MinWidth, PropValue::ofLength(Length::px(r.w)));
}

struct EditCombo { // text = items, value = current index (-1 none), userData = InsertPolicy
    static constexpr std::string_view kName = "editcombo";
    static constexpr A11yRole kRole = A11yRole::ComboBox;
    static ComputedStyle defaults() {
        ComputedStyle s;
        s.direction = Direction::Row;
        return s;
    }
    static bool onEvent(EventContext& ec, const Event& e) {
        UiContext& ui = ec.ui;
        const NodeHandle edit = comboEdit(ui, ec.node);
        if (e.type == EventType::Wheel && ec.phase != Phase::Capture && !completerPopupVisible(ui, edit)) {
            const i32 n = i32(itemCount(ui.text(ec.node))), cur = i32(ui.value(ec.node));
            if (n) comboSetCurrent(ui, ec.node, std::clamp(cur + (e.wheel.y < 0.f ? 1 : -1), 0, n - 1), true);
            return n > 0;
        }
        if (e.type != EventType::KeyDown || ec.phase != Phase::Capture || ec.target != edit) return false;
        const bool alt = e.mods & kModAlt;
        if (e.key == Key::F4 || ((e.key == Key::Down || e.key == Key::Up) && alt)) { comboToggleList(ui, ec.node); return true; }
        if (completerPopupVisible(ui, edit)) return false; // the list navigates
        if (e.key == Key::Up || e.key == Key::Down || e.key == Key::PageUp || e.key == Key::PageDown) {
            const i32 n = i32(itemCount(ui.text(ec.node)));
            if (!n) return true;
            const i32 cur = i32(ui.value(ec.node));
            i32 to = e.key == Key::Down ? cur + 1 : e.key == Key::Up ? cur - 1 : e.key == Key::PageDown ? n - 1 : 0;
            if (cur < 0) to = e.key == Key::Up ? n - 1 : 0;
            comboSetCurrent(ui, ec.node, std::clamp(to, 0, n - 1), true);
            return true;
        }
        return false;
    }
};
struct ComboDrop {
    static constexpr std::string_view kName = "combodrop";
    static constexpr A11yRole kRole = A11yRole::Button;
    static constexpr bool kInteractive = true;
    static Size measure(UiContext& ui, NodeHandle n, Size) { return {16.f, rowH(ui, n) - 6.f}; }
    static void paint(const PaintContext& pc, DrawList& dl) {
        const Rect c = pc.content;
        const Color col = pc.tint(pc.style.color);
        const f32 ax = c.x + c.w * 0.5f, my = c.y + c.h * 0.5f;
        dl.line({ax - 4.f, my - 2.f}, {ax, my + 2.f}, col, 1.5f);
        dl.line({ax, my + 2.f}, {ax + 4.f, my - 2.f}, col, 1.5f);
    }
    static bool onEvent(EventContext& ec, const Event& e) {
        if (e.type != EventType::Click || ec.phase != Phase::Target) return false;
        const NodeHandle combo = ec.ui.parent(ec.node);
        ec.ui.setFocus(comboEdit(ec.ui, combo), FocusReason::Pointer);
        comboToggleList(ec.ui, combo);
        return true;
    }
};

void registerCompleter(UiContext& ui) {
    auto& r = ui.registry();
    if (r.find("completer") != 0xFFFFFFFFu) return;
    r.registerWidget<CompleterList>();
    r.registerWidget<CompleterItem>();
    r.registerWidget<EditCombo>();
    r.registerWidget<ComboDrop>();
}

} // namespace

namespace detail {
bool completerKey(UiContext& ui, NodeHandle edit, const Event& e) {
    CState* s = stateOf(ui, edit);
    if (!s || e.type != EventType::KeyDown) return false;
    const i32 n = i32(s->matches.size());
    const bool ctrl = e.mods & kModCtrl;
    if (s->o.mode == CompletionMode::Inline && !s->shown) {
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
        if (e.key == Key::Down && !ctrl && s->o.mode != CompletionMode::Inline) {
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
        if (s->shown) hide(ui, edit, *s); // typing closes an opened full list (editable combo)
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


NodeHandle editableComboBox(UiContext& ui, NodeHandle parent, std::string_view items, i32 selected, InsertPolicy policy,
                            std::string_view id) {
    registerCompleter(ui);
    auto h = ui.create("editcombo", parent, id);
    if (!h) return {};
    const NodeHandle combo = *h;
    comboState(ui, combo).o.policy = policy;
    ui.setRange(combo, -1.f, 1e9f, 1.f);
    if (auto ic = ui.create("image", combo)) {
        comboState(ui, combo).icon = *ic;
        ui.setCss(*ic, "width", "16px");
        ui.setCss(*ic, "height", "16px");
        ui.setCss(*ic, "align-self", "center");
        ui.setCss(*ic, "display", "none");
    }
    const NodeHandle edit = lineEdit(ui, combo);
    comboState(ui, combo).edit = edit;
    ui.setCss(edit, "flex-grow", "1");
    if (auto d = ui.create("combodrop", combo)) ui.setA11y(*d, A11yRole::Button, "Open list");
    comboSetItems(ui, combo, std::string(items));
    const i32 n = i32(itemCount(items));
    ui.setValue(combo, -1.f, false);
    if (selected >= 0 && selected < n) {
        ui.setValue(combo, f32(selected), false);
        setLineEditText(ui, edit, itemAt(items, u32(selected)));
        if (CState* s = stateOf(ui, edit)) s->prefix.assign(itemAt(items, u32(selected)));
    }
    ui.connect(edit, SignalKind::Activated, [combo](UiContext& u, NodeHandle, const SignalArgs& a) {
        CState* s = stateOf(u, comboEdit(u, combo));
        // a pick from the full list is an index into the items; an inline completion is matched by text on Enter
        const bool cs = comboState(u, combo).o.caseSensitive;
        if (s && s->o.mode == CompletionMode::Inline && comboFindText(u, combo, a.text, cs) >= 0) comboSetCurrent(u, combo, comboFindText(u, combo, a.text, cs), false);
    });
    ui.connect(edit, SignalKind::Submitted, [combo](UiContext& u, NodeHandle, const SignalArgs& a) { comboCommit(u, combo, a.text); });
    return combo;
}
NodeHandle comboLineEdit(const UiContext& ui, NodeHandle combo) { return comboEdit(ui, combo); }
i32 comboCurrentIndex(const UiContext& ui, NodeHandle combo) { return ui.alive(combo) ? i32(ui.value(combo)) : -1; }
void setComboCurrentIndex(UiContext& ui, NodeHandle combo, i32 index) {
    const i32 n = i32(itemCount(ui.text(combo)));
    if (index < -1 || index >= n) return;
    if (index < 0) { ui.setValue(combo, -1.f, false); setLineEditText(ui, comboEdit(ui, combo), {}); return; }
    comboSetCurrent(ui, combo, index, true);
}
i32 comboFindText(const UiContext& ui, NodeHandle combo, std::string_view text, bool caseSensitive) {
    const std::string_view items = ui.text(combo);
    const std::u32string key = fold(text, caseSensitive);
    for (u32 i = 0, n = itemCount(items); i < n; ++i)
        if (caseSensitive ? itemAt(items, i) == text : fold(itemAt(items, i), false) == key) return i32(i);
    return -1;
}
void comboShowList(UiContext& ui, NodeHandle combo) {
    if (!completerPopupVisible(ui, comboEdit(ui, combo))) comboToggleList(ui, combo);
}


void setComboOptions(UiContext& ui, NodeHandle combo, EditableComboOptions o) {
    if (!ui.alive(combo)) return;
    LineEditOptions le;
    le.validator = o.validator;
    le.fixup = o.fixup;
    setLineEditOptions(ui, comboEdit(ui, combo), std::move(le));
    comboState(ui, combo).o = std::move(o);
}
void setComboValidator(UiContext& ui, NodeHandle combo, Validator v, Fixup f) {
    if (!ui.alive(combo)) return;
    EditableComboOptions o = comboState(ui, combo).o;
    o.validator = std::move(v);
    o.fixup = std::move(f);
    setComboOptions(ui, combo, std::move(o));
}
void setComboModel(UiContext& ui, NodeHandle combo, const ItemModel* model, u32 column) {
    if (!ui.alive(combo)) return;
    ComboState& cs = comboState(ui, combo);
    cs.model = model;
    cs.column = column;
    comboSetItems(ui, combo, model ? modelItems(model, column) : std::string(ui.text(combo)));
    const i32 n = i32(comboCount(ui, combo));
    if (i32(ui.value(combo)) >= n) setComboCurrentIndex(ui, combo, n ? 0 : -1);
}
void comboModelChanged(UiContext& ui, NodeHandle combo) {
    const ComboState& cs = comboState(ui, combo);
    if (!cs.model) return;
    ui.setText(combo, modelItems(cs.model, cs.column));
    completerModelChanged(ui, comboEdit(ui, combo));
    const i32 n = i32(comboCount(ui, combo));
    if (i32(ui.value(combo)) >= n) setComboCurrentIndex(ui, combo, n ? n - 1 : -1);
}
u32 comboCount(const UiContext& ui, NodeHandle combo) { return ui.alive(combo) ? itemCount(ui.text(combo)) : 0u; }
std::string comboItemText(const UiContext& ui, NodeHandle combo, u32 index) {
    return index < comboCount(ui, combo) ? std::string(itemAt(ui.text(combo), index)) : std::string{};
}
void setCompletionPrefix(UiContext& ui, NodeHandle edit, std::string_view prefix) {
    CState* s = stateOf(ui, edit);
    if (!s) return;
    const bool was = s->shown;
    refilter(*s, prefix, s->o.mode == CompletionMode::UnfilteredPopup);
    if (was) show(ui, edit, *s);
}
std::string completionPrefix(UiContext& ui, NodeHandle edit) {
    const CState* s = stateOf(ui, edit);
    return s ? s->prefix : std::string{};
}

void setComboItemIcon(UiContext& ui, NodeHandle combo, u32 index, u32 image) {
    if (index >= comboCount(ui, combo)) return;
    ComboState& c = comboState(ui, combo);
    if (c.icons.size() <= index) c.icons.resize(index + 1, kNoIcon);
    c.icons[index] = image;
    comboSyncIcon(ui, combo);
    if (CState* s = stateOf(ui, comboEdit(ui, combo)); s && s->shown) layoutRows(ui, comboEdit(ui, combo), *s, false);
}
u32 comboItemIcon(const UiContext& ui, NodeHandle combo, u32 index) {
    if (!ui.alive(combo)) return kNoIcon;
    const ComboState& c = comboState(ui, combo);
    return index < c.icons.size() ? c.icons[index] : kNoIcon;
}
void setComboItemData(UiContext& ui, NodeHandle combo, u32 index, u64 data) {
    if (index >= comboCount(ui, combo)) return;
    ComboState& c = comboState(ui, combo);
    if (c.data.size() <= index) c.data.resize(index + 1, 0);
    c.data[index] = data;
}
u64 comboItemData(const UiContext& ui, NodeHandle combo, u32 index) {
    if (!ui.alive(combo)) return 0;
    const ComboState& c = comboState(ui, combo);
    return index < c.data.size() ? c.data[index] : 0;
}
i32 comboFindData(const UiContext& ui, NodeHandle combo, u64 data) {
    const u32 n = comboCount(ui, combo);
    for (u32 i = 0; i < n; ++i)
        if (comboItemData(ui, combo, i) == data) return i32(i);
    return -1;
}
void comboInsertItem(UiContext& ui, NodeHandle combo, u32 index, std::string_view text, u32 image, u64 data) {
    if (!ui.alive(combo) || comboState(ui, combo).model) return;
    std::vector<std::string> v;
    const std::string_view items = ui.text(combo);
    for (u32 i = 0, n = itemCount(items); i < n; ++i) v.emplace_back(itemAt(items, i));
    const std::size_t at = std::min<std::size_t>(index, v.size());
    v.insert(v.begin() + std::ptrdiff_t(at), std::string(text));
    comboInsertMeta(comboState(ui, combo), at, image, data, v.size());
    std::string joined;
    for (std::size_t i = 0; i < v.size(); ++i) { if (i) joined += '\n'; joined += v[i]; }
    const i32 cur = i32(ui.value(combo));
    comboSetItems(ui, combo, std::move(joined));
    if (cur >= 0 && std::size_t(cur) >= at) ui.setValue(combo, f32(cur + 1), false); // the current item stays current (Qt)
    else if (cur < 0 && v.size() == 1) comboSetCurrent(ui, combo, 0, true);         // first item becomes current
    comboSyncIcon(ui, combo);
}
void comboAddItem(UiContext& ui, NodeHandle combo, std::string_view text, u32 image, u64 data) {
    comboInsertItem(ui, combo, 0xFFFFFFFFu, text, image, data);
}
void comboRemoveItem(UiContext& ui, NodeHandle combo, u32 index) {
    if (!ui.alive(combo) || comboState(ui, combo).model || index >= comboCount(ui, combo)) return;
    std::vector<std::string> v;
    const std::string_view items = ui.text(combo);
    for (u32 i = 0, n = itemCount(items); i < n; ++i) v.emplace_back(itemAt(items, i));
    v.erase(v.begin() + index);
    ComboState& c = comboState(ui, combo);
    if (index < c.icons.size()) c.icons.erase(c.icons.begin() + index);
    if (index < c.data.size()) c.data.erase(c.data.begin() + index);
    std::string joined;
    for (std::size_t i = 0; i < v.size(); ++i) { if (i) joined += '\n'; joined += v[i]; }
    const i32 cur = i32(ui.value(combo));
    comboSetItems(ui, combo, std::move(joined));
    if (cur > i32(index)) ui.setValue(combo, f32(cur - 1), false);
    else if (cur == i32(index)) { // Qt: the next item (or the previous at the end) becomes current
        ui.setValue(combo, -2.f, false);
        if (v.empty()) setComboCurrentIndex(ui, combo, -1);
        else comboSetCurrent(ui, combo, std::min<i32>(cur, i32(v.size()) - 1), true);
    }
    comboSyncIcon(ui, combo);
}

} // namespace fuse::ui::widgets
