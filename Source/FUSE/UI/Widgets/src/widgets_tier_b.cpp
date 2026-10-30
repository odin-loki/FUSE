// FUSE UI Tier B widgets (plan §5): overlay layer, popup scope, popup menu + combo box list, menu bar, modal dialog,
// tooltip, virtualised list view, tree view, tab bar / tab widget, scroll area.
#include <set>
#include <unordered_map>
#include "fuse/ui/widgets/widgets.hpp"
#include "widgets_internal.hpp"
#include "item_selection.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace fuse::ui::widgets {
namespace {

constexpr std::string_view kLayerId = "__fuse_ui_layer";

u64 pack(NodeHandle h) { return (u64(h.generation) << 32) | u64(h.index); }
NodeHandle unpack(u64 v) { return NodeHandle{u32(v & 0xFFFFFFFFu), u32(v >> 32)}; }

NodeHandle make(UiContext& ui, std::string_view cls, NodeHandle parent, std::string_view id = {}) {
    auto h = ui.create(cls, parent, id);
    return h ? *h : NodeHandle{};
}
bool isClass(const UiContext& ui, NodeHandle n, std::string_view cls) {
    return ui.alive(n) && ui.classOf(n) == ui.registry().find(cls);
}
void setPx(UiContext& ui, NodeHandle n, Prop p, f32 v) { ui.set(n, p, PropValue::ofLength(Length::px(v))); }

f32 rowHeight(UiContext& ui, NodeHandle n) {
    const f32 px = ui.computed(n).fontSize;
    return std::ceil(ui.font() ? ui.font()->metrics(px).lineHeight() : px * 1.25f) + 6.f;
}
Color textCol(const PaintContext& pc) { return pc.tint(pc.style.color); }
Color accent(const PaintContext& pc) { return pc.tint(pc.style.background2.a > 0.f ? pc.style.background2 : Color::rgb(0x3D8BFD)); }
void labelAt(const PaintContext& pc, DrawList& dl, std::string_view s, Rect box, TextAlignment al = TextAlignment::Start) {
    pc.ui.paintString(dl, s, box, textCol(pc), pc.style.fontSize, al, pc.node.index);
}

// '\n'-separated items
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
/// Calls f(index, line) for each line from `first`; stops when f returns false.
template <class F>
void eachLine(std::string_view s, F&& f) {
    std::size_t at = 0;
    for (u32 i = 0; at <= s.size(); ++i) {
        const std::size_t nl = s.find('\n', at);
        const std::string_view line = s.substr(at, nl == std::string_view::npos ? std::string_view::npos : nl - at);
        if (!f(i, line)) return;
        if (nl == std::string_view::npos) return;
        at = nl + 1;
    }
}

// ---- popup scope -----------------------------------------------------------------------------------------------
NodeHandle scopeOf(const UiContext& ui, NodeHandle n) {
    const u32 cls = ui.registry().find("popup");
    while (ui.alive(n) && ui.classOf(n) != cls) n = ui.parent(n);
    return ui.alive(n) ? n : NodeHandle{};
}
NodeHandle firstFocusable(const UiContext& ui, NodeHandle n) {
    for (NodeHandle c = ui.firstChild(n); ui.alive(c); c = ui.nextSibling(c)) {
        if (ui.registry().get(ui.classOf(c)).focusable && !ui.disabled(c)) return c;
        const NodeHandle d = firstFocusable(ui, c);
        if (d.valid()) return d;
    }
    return {};
}
/// First enabled descendant with class `cls` (dialog default / reject buttons).
NodeHandle findClass(const UiContext& ui, NodeHandle n, std::string_view cls) {
    for (NodeHandle c = ui.firstChild(n); ui.alive(c); c = ui.nextSibling(c)) {
        if (ui.hasClass(c, cls) && !ui.disabled(c)) return c;
        const NodeHandle d = findClass(ui, c, cls);
        if (d.valid()) return d;
    }
    return {};
}
void destroyCmd(UiContext& ui, NodeHandle n, u64, u64) {
    if (ui.alive(n)) ui.destroy(n);
}

struct Popup {
    static constexpr std::string_view kName = "popup";
    static constexpr A11yRole kRole = A11yRole::Group;
    static ComputedStyle defaults() {
        ComputedStyle s;
        s.layout = LayoutKind::Anchor;
        return s;
    }
    static bool onEvent(EventContext& ec, const Event& e) {
        const bool modal = ec.ui.hasClass(ec.node, "modal");
        if (e.type == EventType::PointerDown && ec.phase == Phase::Target) {
            if (!modal) closePopup(ec.ui, ec.node); // click on the scrim: outside the panel
            else if (const NodeHandle f = firstFocusable(ec.ui, ec.node); f.valid()) ec.ui.setFocus(f, FocusReason::Pointer);
            return true; // a modal scrim keeps focus inside the dialog
        }
        if (e.type == EventType::KeyDown && e.key == Key::Escape) {
            // QDialog::reject: a dialog's reject button handles Escape (its slots decide), else the dialog closes
            if (const NodeHandle r = modal ? findClass(ec.ui, ec.node, "dialog-reject") : NodeHandle{}; r.valid())
                ec.ui.emit(r, SignalKind::Clicked, {});
            else closePopup(ec.ui, ec.node);
            return true;
        }
        if (modal && e.type == EventType::KeyDown && e.key == Key::Enter && !(e.mods & (kModCtrl | kModAlt | kModSuper))) {
            // QDialog: Enter presses the default button unless a push button has focus (autoDefault: it presses itself)
            const NodeHandle f = ec.ui.focused();
            if (ec.ui.alive(f) && ec.ui.registry().get(ec.ui.classOf(f)).name == "button") return false;
            if (const NodeHandle d = findClass(ec.ui, ec.node, "dialog-default"); d.valid()) {
                ec.ui.emit(d, SignalKind::Clicked, {});
                return true;
            }
        }
        return false;
    }
};

struct Menu {
    static constexpr std::string_view kName = "menu";
    static constexpr A11yRole kRole = A11yRole::List;
    // Up / Down / Home / End move between items (wrapping), independent of layout so it works on the opening frame.
    static bool onEvent(EventContext& ec, const Event& e) {
        if (e.type != EventType::KeyDown) return false;
        UiContext& ui = ec.ui;
        const u32 itemCls = ui.registry().find("menuitem");
        NodeHandle items[256];
        u32 n = 0, cur = 0xFFFFFFFFu;
        for (NodeHandle it = ui.firstChild(ec.node); ui.alive(it) && n < 256; it = ui.nextSibling(it)) {
            if (ui.classOf(it) != itemCls || ui.disabled(it)) continue;
            if (it == ui.focused()) cur = n;
            items[n++] = it;
        }
        if (!n) return false;
        u32 to;
        switch (e.key) {
        case Key::Down: to = cur == 0xFFFFFFFFu ? 0 : (cur + 1) % n; break;
        case Key::Up: to = cur == 0xFFFFFFFFu ? n - 1 : (cur + n - 1) % n; break;
        case Key::Home: to = 0; break;
        case Key::End: to = n - 1; break;
        default: return false;
        }
        ui.setFocus(items[to], FocusReason::Keyboard);
        return true;
    }
};
struct MenuItem { // text = "label" or "label\n<sub items, one tab less>" (a submenu); value = line index in the root list
    static constexpr std::string_view kName = "menuitem";
    static constexpr A11yRole kRole = A11yRole::ListItem;
    static constexpr bool kFocusable = true;
    static constexpr bool kInteractive = true;
    static std::string_view label(std::string_view t) { return t.substr(0, t.find('\n')); }
    static bool hasSub(std::string_view t) { return t.find('\n') != std::string_view::npos; }
    static Size measure(UiContext& ui, NodeHandle n, Size) {
        const std::string_view t = ui.text(n);
        return {std::ceil(ui.measureString(label(t), ui.computed(n).fontSize)) + (hasSub(t) ? 18.f : 0.f), rowHeight(ui, n) - 6.f};
    }
    static void paint(const PaintContext& pc, DrawList& dl) {
        const std::string_view t = pc.ui.text(pc.node);
        if (pc.ui.checked(pc.node)) dl.rect({pc.rect.x + 2.f, pc.rect.y + 3.f, 3.f, pc.rect.h - 6.f}, accent(pc));
        labelAt(pc, dl, label(t), pc.content);
        if (hasSub(t)) { // chevron
            const Rect c = pc.content;
            const f32 x = c.right() - 8.f, y = c.y + c.h * 0.5f;
            dl.line({x, y - 4.f}, {x + 4.f, y}, textCol(pc), 1.5f);
            dl.line({x + 4.f, y}, {x, y + 4.f}, textCol(pc), 1.5f);
        }
    }
    static void openSub(UiContext& ui, NodeHandle n) {
        const std::string_view t = ui.text(n);
        const Rect r = ui.rect(n);
        const NodeHandle sc = scopeOf(ui, n);
        const Rect panel = ui.alive(sc) ? ui.rect(ui.firstChild(sc)) : r;
        const std::string_view sub = t.substr(t.find('\n') + 1);
        f32 w = 0.f; // estimated panel width: open to the left when the right side has no room
        eachLine(sub, [&](u32, std::string_view l) {
            if (l.empty() || l[0] != '\t') w = std::max(w, ui.measureString(l, ui.computed(n).fontSize) + 66.f);
            return true;
        });
        const f32 x = panel.right() - 2.f + w > ui.popupArea().x + ui.popupArea().w ? std::max(ui.popupArea().x, panel.x + 2.f - w) : panel.right() - 2.f;
        detail::popupMenuAt(ui, n, sub, {x, r.y - 4.f}, u32(ui.value(n)) + 1u);
    }
    static bool onEvent(EventContext& ec, const Event& e) {
        if (e.type == EventType::PointerEnter) { ec.ui.setFocus(ec.node, FocusReason::Pointer); return false; }
        UiContext& ui = ec.ui;
        const NodeHandle scope = scopeOf(ui, ec.node);
        NodeHandle owner = ui.alive(scope) ? unpack(ui.userData(scope)) : NodeHandle{};
        if (e.type == EventType::KeyDown && ec.phase == Phase::Target) {
            if (e.key == Key::Right && hasSub(ui.text(ec.node))) { openSub(ui, ec.node); return true; }
            if (e.key == Key::Left && isClass(ui, owner, "menuitem")) { closePopup(ui, ec.node); return true; }
            return false;
        }
        if (e.type != EventType::Click || ec.phase != Phase::Target) return false;
        if (hasSub(ui.text(ec.node))) { openSub(ui, ec.node); return true; }
        const u32 idx = u32(ui.value(ec.node));
        const std::string_view text = ui.text(ec.node);
        if (ui.alive(owner) && isClass(ui, owner, "combobox")) {
            ui.setValue(owner, f32(idx), true);
            ui.emit(owner, SignalKind::TextChanged, {.text = text});
        }
        // a submenu pick closes the whole cascade and reports to the root owner
        NodeHandle sc = scope;
        while (ui.alive(sc)) {
            ui.emit(sc, SignalKind::Submitted, {.value = f32(idx), .text = text});
            owner = unpack(ui.userData(sc));
            closePopup(ui, sc);
            sc = isClass(ui, owner, "menuitem") ? scopeOf(ui, owner) : NodeHandle{};
        }
        if (ui.alive(owner)) ui.emit(owner, SignalKind::Submitted, {.value = f32(idx), .text = text});
        return true;
    }
};

// ---- menu bar ----------------------------------------------------------------------------------------------------
struct MenuBar {
    static constexpr std::string_view kName = "menubar";
    static constexpr A11yRole kRole = A11yRole::Group;
    static ComputedStyle defaults() {
        ComputedStyle s;
        s.direction = Direction::Row;
        return s;
    }
};
struct ToolBar {
    static constexpr std::string_view kName = "toolbar";
    static constexpr A11yRole kRole = A11yRole::ToolBar;
    static ComputedStyle defaults() {
        ComputedStyle s;
        s.direction = Direction::Row;
        return s;
    }
};
} // namespace
namespace detail {
void toolBarGripPress(UiContext& ui, NodeHandle bar, Vec2 p);
void toolBarGripMove(UiContext& ui, NodeHandle bar, Vec2 p);
void toolBarGripRelease(UiContext& ui, NodeHandle bar);
} // namespace detail
namespace {
struct ToolBarGrip { // QToolBar handle: drag to move the bar between tool bar areas or float it
    static constexpr std::string_view kName = "toolbargrip";
    static constexpr A11yRole kRole = A11yRole::None;
    static constexpr bool kInteractive = true;
    static Size measure(UiContext&, NodeHandle, Size) { return {8.f, 8.f}; }
    static void paint(const PaintContext& pc, DrawList& dl) {
        const Rect r = pc.rect;
        const Color c = pc.tint(pc.states & (kStateHover | kStatePressed) ? Color::rgb(0x9A9AA6) : Color::rgb(0x5A5A66));
        const bool v = r.w > r.h; // in a vertical bar the grip lies across the top
        for (int i = -1; i <= 1; ++i) {
            const Vec2 p = v ? Vec2{r.x + r.w * 0.5f + f32(i) * 5.f, r.y + r.h * 0.5f} : Vec2{r.x + r.w * 0.5f, r.y + r.h * 0.5f + f32(i) * 5.f};
            dl.rect({p.x - 1.f, p.y - 1.f, 2.f, 2.f}, c, 1.f);
        }
    }
    static bool onEvent(EventContext& ec, const Event& e) {
        if (ec.phase != Phase::Target) return false;
        UiContext& ui = ec.ui;
        const NodeHandle bar = ui.parent(ec.node);
        if (e.type == EventType::PointerDown && e.button == MouseButton::Left) { detail::toolBarGripPress(ui, bar, e.client); return true; }
        if (e.type == EventType::PointerMove && ui.pressed() == ec.node) { detail::toolBarGripMove(ui, bar, e.client); return true; }
        if (e.type == EventType::PointerUp) { detail::toolBarGripRelease(ui, bar); return true; }
        return false;
    }
};
struct ToolBarSep {
    static constexpr std::string_view kName = "toolbarseparator";
    static constexpr A11yRole kRole = A11yRole::Separator;
};
struct MenuButton { // text = "Title\nitem\nitem..."
    static constexpr std::string_view kName = "menubutton";
    static constexpr A11yRole kRole = A11yRole::Button;
    static constexpr bool kFocusable = true;
    static constexpr bool kInteractive = true;
    static Size measure(UiContext& ui, NodeHandle n, Size) {
        return {std::ceil(ui.measureString(nth(ui.text(n), 0), ui.computed(n).fontSize)), rowHeight(ui, n) - 6.f};
    }
    static void paint(const PaintContext& pc, DrawList& dl) {
        labelAt(pc, dl, nth(pc.ui.text(pc.node), 0), pc.content, TextAlignment::Center);
    }
    static bool onEvent(EventContext& ec, const Event& e) {
        if (ec.phase != Phase::Target) return false;
        if (e.type == EventType::Click || (e.type == EventType::KeyDown && e.key == Key::Down)) {
            const std::string_view t = ec.ui.text(ec.node);
            const std::size_t nl = t.find('\n');
            if (nl == std::string_view::npos) return true;
            const Rect r = ec.ui.rect(ec.node);
            popupMenu(ec.ui, ec.node, t.substr(nl + 1), {r.x, r.bottom()});
            return true;
        }
        return false;
    }
};

// ---- tooltip -----------------------------------------------------------------------------------------------------
struct Tooltip {
    static constexpr std::string_view kName = "tooltip";
    static constexpr A11yRole kRole = A11yRole::Text;
    static ComputedStyle defaults() {
        ComputedStyle s;
        s.pointerEvents = false;
        s.visible = false;
        return s;
    }
};

// ---- list view ---------------------------------------------------------------------------------------------------
struct ListView {
    static constexpr std::string_view kName = "listview";
    static constexpr A11yRole kRole = A11yRole::List;
    static constexpr bool kFocusable = true;
    static constexpr bool kInteractive = true;
    static Size measure(UiContext& ui, NodeHandle n, Size) {
        return {160.f, rowHeight(ui, n) * f32(std::clamp(count(ui.text(n)), 1u, 8u))};
    }
    static u32 visibleRows(UiContext& ui, NodeHandle n) {
        return std::max(1u, u32(ui.contentRect(n).h / rowHeight(ui, n)));
    }
    static void setFirst(UiContext& ui, NodeHandle n, i64 first) {
        const u32 c = count(ui.text(n)), vis = visibleRows(ui, n);
        const i64 mx = c > vis ? i64(c - vis) : 0;
        const u64 f = u64(std::clamp<i64>(first, 0, mx));
        if (f != ui.userData(n)) {
            ui.setUserData(n, f);
            ui.setValue(n, ui.value(n)); // repaint
            ui.setState(n, kStateHover, ui.states(n) & kStateHover);
        }
    }
    static void select(UiContext& ui, NodeHandle n, i64 idx) {
        const u32 c = count(ui.text(n));
        if (!c) return;
        idx = std::clamp<i64>(idx, 0, i64(c) - 1);
        const u32 vis = visibleRows(ui, n), first = u32(ui.userData(n));
        if (idx < i64(first)) setFirst(ui, n, idx);
        else if (idx >= i64(first + vis)) setFirst(ui, n, idx - i64(vis) + 1);
        if (i64(ui.value(n)) == idx) return;
        ui.setValue(n, f32(idx), true);
        ui.emit(n, SignalKind::TextChanged, {.text = nth(ui.text(n), u32(idx))});
    }
    static void paint(const PaintContext& pc, DrawList& dl) {
        UiContext& ui = pc.ui;
        const Rect c = pc.content;
        const f32 rh = rowHeight(ui, pc.node);
        const u32 first = u32(ui.userData(pc.node));
        const i64 sel = i64(ui.value(pc.node));
        const u32 vis = u32(std::ceil(c.h / rh));
        dl.pushClip(c);
        eachLine(ui.text(pc.node), [&](u32 i, std::string_view line) {
            if (i < first) return true;
            if (i >= first + vis) return false; // virtualised: rows past the view are never touched
            const Rect row{c.x, c.y + f32(i - first) * rh, c.w, rh};
            if (i64(i) == sel) dl.rect(row, accent(pc).withAlpha((pc.states & kStateFocus ? 0.55f : 0.3f) * pc.opacity), 3.f);
            labelAt(pc, dl, line, {row.x + 6.f, row.y + 3.f, row.w - 12.f, rh - 6.f});
            return true;
        });
        dl.popClip();
        const u32 total = count(ui.text(pc.node)), shown = u32(c.h / rh);
        if (total > shown && shown) { // scroll indicator
            const f32 th = std::max(12.f, c.h * f32(shown) / f32(total));
            const f32 ty = c.y + (c.h - th) * f32(first) / f32(total - shown);
            dl.rect({c.right() - 4.f, ty, 3.f, th}, textCol(pc).withAlpha(0.35f * pc.opacity), 1.5f);
        }
    }
    static bool onEvent(EventContext& ec, const Event& e) {
        if (ec.phase != Phase::Target) return false;
        UiContext& ui = ec.ui;
        const NodeHandle n = ec.node;
        const i64 cur = i64(ui.value(n));
        const u32 c = count(ui.text(n)), vis = visibleRows(ui, n);
        switch (e.type) {
        case EventType::PointerDown: {
            const i64 row = i64(ui.userData(n)) + i64((e.pos.y - ui.contentRect(n).y) / rowHeight(ui, n));
            if (row >= 0 && row < i64(c)) select(ui, n, row);
            return true;
        }
        case EventType::Wheel: setFirst(ui, n, i64(ui.userData(n)) - i64(std::lround(e.wheel.y * 3.f))); return true;
        case EventType::KeyDown:
            switch (e.key) {
            case Key::Up: select(ui, n, cur - 1); return true;
            case Key::Down: select(ui, n, cur + 1); return true;
            case Key::PageUp: select(ui, n, cur - i64(vis)); return true;
            case Key::PageDown: select(ui, n, cur + i64(vis)); return true;
            case Key::Home: select(ui, n, 0); return true;
            case Key::End: select(ui, n, i64(c) - 1); return true;
            case Key::Enter:
                if (cur >= 0) ui.emit(n, SignalKind::Submitted, {.value = f32(cur), .text = nth(ui.text(n), u32(cur))});
                return true;
            default: return false;
            }
        default: return false;
        }
    }
};

// ---- tree view ---------------------------------------------------------------------------------------------------
u32 depthOf(std::string_view line) {
    u32 d = 0;
    while (d < line.size() && line[d] == '\t') ++d;
    return d;
}
bool lineOpen(u64 collapsed, u32 i) { return i >= 64 || !(collapsed >> i & 1u); }
/// Calls f(row, line, depth, hasChildren, open, text) for each shown row; stops when f returns false.
template <class F>
void eachTreeRow(std::string_view s, u64 collapsed, F&& f) {
    // look-ahead of one line to know whether a line has children
    u32 skipBelow = 0xFFFFFFFFu, row = 0;
    std::string_view prev;
    u32 prevIdx = 0;
    bool havePrev = false, stop = false;
    auto flush = [&](u32 nextDepth) {
        if (!havePrev || stop) return;
        const u32 d = depthOf(prev);
        if (d > skipBelow) return;
        skipBelow = 0xFFFFFFFFu;
        const bool kids = nextDepth > d && nextDepth != 0xFFFFFFFFu;
        const bool open = lineOpen(collapsed, prevIdx);
        if (kids && !open) skipBelow = d;
        if (!f(row++, prevIdx, d, kids, open, prev.substr(d))) stop = true;
    };
    eachLine(s, [&](u32 i, std::string_view line) {
        flush(depthOf(line));
        prev = line;
        prevIdx = i;
        havePrev = true;
        return !stop;
    });
    flush(0xFFFFFFFFu);
}
struct TreeState {
    u32 gen = 0;
    TreeDragDrop o;
    detail::ItemSelection sel;
    u32 first = 0;
    i64 pendingLine = -1; ///< Extended / Multi press on a selected row: narrow the selection on release unless dragged
    std::vector<u32> dragHeads; ///< rows of the drag this tree started (removed when it ends as a Move elsewhere)
    bool droppedSelf = false;   ///< that drag was dropped (moved / copied) on this tree itself
    bool barDrag = false;
    f32 grab = 0.f;
    bool band = false; ///< rubber-band selection from empty space (Multi / Extended)
    Vec2 bandA, bandB;
    std::set<u64> bandBase;
};
std::unordered_map<const UiContext*, std::unordered_map<u32, TreeState>>& treeStates() {
    static std::unordered_map<const UiContext*, std::unordered_map<u32, TreeState>> m;
    return m;
}
TreeState& treeState(const UiContext& ui, NodeHandle n) {
    TreeState& s = treeStates()[&ui][n.index];
    if (s.gen != n.generation) s = TreeState{n.generation};
    return s;
}
const TreeDragDrop* treeDndOf(const UiContext& ui, NodeHandle n) {
    const TreeState& s = treeState(ui, n);
    return (s.o.drag || s.o.internalMove) ? &s.o : nullptr;
}
/// Rebuilds the collapsed-line mask (userData, one bit per line < 64) after lines moved: `newOf[old]` = new index or -1.
u64 remapMask(u64 old, const std::vector<i64>& newOf) {
    u64 m = 0;
    for (std::size_t o = 0; o < newOf.size() && o < 64; ++o)
        if ((old >> o & 1u) && newOf[o] >= 0 && newOf[o] < 64) m |= u64(1) << newOf[o];
    return m;
}
/// The blocks (row + subtree) headed by `heads`, each re-based to depth 0, '\n'-joined.
std::string treeBlocks(const UiContext& ui, NodeHandle n, std::vector<u32> heads) {
    std::vector<std::string_view> lines;
    eachLine(ui.text(n), [&](u32, std::string_view l) { lines.push_back(l); return true; });
    std::sort(heads.begin(), heads.end());
    std::string out;
    u32 skipTo = 0;
    for (u32 h : heads) {
        if (h >= lines.size() || h < skipTo) continue; // inside a block already taken
        const u32 d0 = depthOf(lines[h]);
        u32 e = h + 1;
        while (e < lines.size() && depthOf(lines[e]) > d0) ++e;
        for (u32 k = h; k < e; ++k) {
            if (!out.empty()) out.push_back('\n');
            out.append(depthOf(lines[k]) - d0, '\t');
            out.append(lines[k].substr(depthOf(lines[k])));
        }
        skipTo = e;
    }
    return out;
}
enum class DropZone : u8;
std::vector<u32> treeInsert(UiContext& ui, NodeHandle n, std::string_view block, i64 to, DropZone z);
/// Removes the blocks headed by `heads` (descending old heads returned).
std::vector<u32> treeRemove(UiContext& ui, NodeHandle n, std::vector<u32> heads) {
    std::vector<std::string_view> lines;
    eachLine(ui.text(n), [&](u32, std::string_view l) { lines.push_back(l); return true; });
    std::sort(heads.begin(), heads.end());
    std::vector<bool> gone(lines.size(), false);
    std::vector<u32> removed;
    for (u32 h : heads) {
        if (h >= lines.size() || gone[h]) continue;
        const u32 d0 = depthOf(lines[h]);
        gone[h] = true;
        for (u32 k = h + 1; k < lines.size() && depthOf(lines[k]) > d0; ++k) gone[k] = true;
        removed.push_back(h);
    }
    std::vector<i64> newOf(lines.size(), -1);
    std::string out;
    i64 i = 0;
    for (u32 k = 0; k < lines.size(); ++k) {
        if (gone[k]) continue;
        if (i) out.push_back('\n');
        out.append(lines[k]);
        newOf[k] = i++;
    }
    ui.setUserData(n, remapMask(ui.userData(n), newOf));
    ui.setText(n, out);
    std::reverse(removed.begin(), removed.end());
    return removed;
}
enum class DropZone : u8 { Above, Below, Onto };
/// Inserts `block` (lines, tabs = depth relative to the block) above / below / onto line `to` (to < 0: appended at
/// depth 0); returns the new indices of the block's top-level heads.
std::vector<u32> treeInsert(UiContext& ui, NodeHandle n, std::string_view block, i64 to, DropZone z) {
    std::vector<std::string_view> lines, add;
    eachLine(ui.text(n), [&](u32, std::string_view l) { lines.push_back(l); return true; });
    if (ui.text(n).empty()) lines.clear();
    eachLine(block, [&](u32, std::string_view l) { if (!l.empty()) add.push_back(l); return true; });
    if (add.empty() || to >= i64(lines.size())) return {};
    u32 at = u32(lines.size()), depth = 0;
    if (to >= 0) {
        const u32 dt = depthOf(lines[std::size_t(to)]);
        u32 tEnd = u32(to) + 1;
        while (tEnd < lines.size() && depthOf(lines[tEnd]) > dt) ++tEnd;
        depth = z == DropZone::Onto ? dt + 1 : dt;
        at = z == DropZone::Above ? u32(to) : tEnd;
    }
    u32 base = 0xFFFFFFFFu;
    for (std::string_view l : add) base = std::min(base, depthOf(l));
    std::vector<i64> newOf(lines.size(), -1);
    std::vector<u32> heads;
    std::string out;
    u32 i = 0;
    auto line = [&](std::string_view l) { if (i) out.push_back('\n'); out.append(l); ++i; };
    for (u32 k = 0; k <= lines.size(); ++k) {
        if (k == at)
            for (std::string_view l : add) {
                const u32 d = depthOf(l) - base;
                if (d == 0) heads.push_back(i);
                std::string s(depth + d, '\t');
                s.append(l.substr(depthOf(l)));
                line(s);
            }
        if (k < lines.size()) { newOf[k] = i; line(lines[k]); }
    }
    u64 mask = remapMask(ui.userData(n), newOf);
    if (z == DropZone::Onto && to >= 0 && newOf[std::size_t(to)] < 64) mask &= ~(u64(1) << newOf[std::size_t(to)]); // expand the new parent
    ui.setUserData(n, mask);
    ui.setText(n, out);
    return heads;
}
/// Moves lines `froms` (each with its subtree; lines inside another moved subtree go along with it) above / below /
/// onto line `to`, keeping their order; returns (old, new) index of each moved head (empty = refused).
std::vector<std::pair<u32, u32>> treeMove(UiContext& ui, NodeHandle n, std::vector<u32> froms, u32 to, DropZone z) {
    std::vector<std::string_view> lines;
    eachLine(ui.text(n), [&](u32, std::string_view l) { lines.push_back(l); return true; });
    std::sort(froms.begin(), froms.end());
    std::vector<i32> blockOf(lines.size(), -1); // line -> index of its block head in `heads`
    std::vector<u32> heads;
    for (u32 f : froms) {
        if (f >= lines.size() || blockOf[f] >= 0) continue;
        const u32 d0 = depthOf(lines[f]);
        blockOf[f] = i32(heads.size());
        for (u32 k = f + 1; k < lines.size() && depthOf(lines[k]) > d0; ++k) blockOf[k] = i32(heads.size());
        heads.push_back(f);
    }
    if (heads.empty() || to >= lines.size() || blockOf[to] >= 0) return {};
    const u32 dt = depthOf(lines[to]);
    u32 tEnd = to + 1;
    while (tEnd < lines.size() && depthOf(lines[tEnd]) > dt) ++tEnd;
    const u32 newDepth = z == DropZone::Onto ? dt + 1 : dt;
    const u32 at = z == DropZone::Above ? to : tEnd;
    std::vector<u32> order, moved;
    for (u32 k = 0; k < lines.size(); ++k) if (blockOf[k] >= 0) moved.push_back(k);
    for (u32 i = 0; i <= lines.size(); ++i) {
        if (i == at) order.insert(order.end(), moved.begin(), moved.end());
        if (i < lines.size() && blockOf[i] < 0) order.push_back(i);
    }
    const u64 oldMask = ui.userData(n);
    u64 mask = 0;
    std::string out;
    std::vector<std::pair<u32, u32>> newHeads;
    for (u32 i = 0; i < order.size(); ++i) {
        const u32 o = order[i];
        std::string_view l = lines[o];
        if (blockOf[o] >= 0) {
            const u32 d = depthOf(l) - depthOf(lines[heads[u32(blockOf[o])]]) + newDepth;
            out.append(d, '\t');
            l = l.substr(depthOf(l));
            if (o == heads[u32(blockOf[o])]) newHeads.emplace_back(o, i);
        }
        out.append(l);
        if (i + 1 < order.size()) out.push_back('\n');
        if (i < 64 && o < 64 && (oldMask >> o & 1u)) mask |= u64(1) << i;
        if (z == DropZone::Onto && o == to && i < 64) mask &= ~(u64(1) << i); // expand the new parent
    }
    ui.setUserData(n, mask);
    ui.setText(n, out);
    return newHeads;
}
struct TreeView {
    static constexpr std::string_view kName = "treeview";
    static constexpr A11yRole kRole = A11yRole::Grid;
    static constexpr bool kFocusable = true;
    static constexpr bool kInteractive = true;
    static constexpr f32 kIndent = 16.f;
    static constexpr f32 kBar = 8.f;
    static Size measure(UiContext& ui, NodeHandle n, Size) {
        return {200.f, rowHeight(ui, n) * f32(std::clamp(treeVisibleRows(ui, n), 1u, 10u))};
    }
    static u32 visible(UiContext& ui, NodeHandle n) { return std::max(1u, u32(ui.contentRect(n).h / rowHeight(ui, n))); }
    static void setFirst(UiContext& ui, NodeHandle n, i64 f) {
        TreeState& s = treeState(ui, n);
        const u32 total = treeVisibleRows(ui, n), vis = visible(ui, n);
        const u32 nf = u32(std::clamp<i64>(f, 0, total > vis ? i64(total - vis) : 0));
        if (nf != s.first) { s.first = nf; ui.invalidate(n); }
    }
    static void ensureVisible(UiContext& ui, NodeHandle n, i64 row) {
        if (row < 0) return;
        const TreeState& s = treeState(ui, n);
        const u32 vis = visible(ui, n);
        if (row < i64(s.first)) setFirst(ui, n, row);
        else if (row >= i64(s.first + vis)) setFirst(ui, n, row - i64(vis) + 1);
    }
    static bool barGeom(UiContext& ui, NodeHandle n, Rect& track, Rect& thumb) {
        const u32 total = treeVisibleRows(ui, n), vis = visible(ui, n);
        if (total <= vis) return false;
        const Rect c = ui.contentRect(n);
        track = {c.right() - kBar, c.y, kBar, c.h};
        const f32 tl = std::max(16.f, c.h * f32(vis) / f32(total));
        thumb = {track.x, c.y + (c.h - tl) * f32(treeState(ui, n).first) / f32(total - vis), kBar, tl};
        return true;
    }
    static bool lineSelected(const UiContext& ui, NodeHandle n, i64 line) {
        return treeState(ui, n).sel.has(line, 0, i64(ui.value(n)), 0);
    }
    static void paint(const PaintContext& pc, DrawList& dl) {
        UiContext& ui = pc.ui;
        const Rect c = pc.content;
        const f32 rh = rowHeight(ui, pc.node);
        const i64 cur = i64(ui.value(pc.node));
        const TreeState& s = treeState(ui, pc.node);
        Rect tr, th;
        const bool bar = barGeom(ui, pc.node, tr, th);
        const f32 w = c.w - (bar ? kBar : 0.f);
        dl.pushClip(c);
        eachTreeRow(ui.text(pc.node), ui.userData(pc.node), [&](u32 row, u32 line, u32 depth, bool kids, bool open, std::string_view t) {
            if (row < s.first) return true;
            const Rect r{c.x, c.y + f32(row - s.first) * rh, w, rh};
            if (r.y > c.bottom()) return false;
            if (lineSelected(ui, pc.node, line)) dl.rect(r, accent(pc).withAlpha((pc.states & kStateFocus ? 0.55f : 0.3f) * pc.opacity), 3.f);
            else if (i64(line) == cur && s.sel.explicitSet() && (pc.states & kStateFocus)) // current item outline
                dl.roundedRect(r, {}, {}, accent(pc).withAlpha(0.7f * pc.opacity), 1.f);
            const f32 x = r.x + 4.f + f32(depth) * kIndent, my = r.y + rh * 0.5f;
            if (kids) {
                const Color a = textCol(pc);
                if (open) { dl.line({x + 2.f, my - 2.f}, {x + 6.f, my + 2.f}, a, 1.5f); dl.line({x + 6.f, my + 2.f}, {x + 10.f, my - 2.f}, a, 1.5f); }
                else { dl.line({x + 4.f, my - 4.f}, {x + 8.f, my}, a, 1.5f); dl.line({x + 8.f, my}, {x + 4.f, my + 4.f}, a, 1.5f); }
            }
            labelAt(pc, dl, t, {x + 14.f, r.y + 3.f, r.right() - x - 14.f, rh - 6.f});
            return true;
        });
        if (Rect ind; dropIndicator(ui, pc.node, ind)) { // QAbstractItemView drop indicator: a line between rows, a box onto one
            if (ind.h <= 2.f) dl.rect(ind, accent(pc).withAlpha(pc.opacity));
            else dl.roundedRect(ind, {}, {}, accent(pc).withAlpha(pc.opacity), 1.5f);
        }
        if (s.band) { // QRubberBand
            const Rect b{std::min(s.bandA.x, s.bandB.x), std::min(s.bandA.y, s.bandB.y), std::abs(s.bandB.x - s.bandA.x), std::abs(s.bandB.y - s.bandA.y)};
            dl.rect(b, accent(pc).withAlpha(pc.opacity * 0.2f));
            dl.roundedRect(b, {}, {}, accent(pc).withAlpha(pc.opacity * 0.8f), 1.f);
        }
        if (bar) {
            dl.rect(tr, pc.tint(Color::rgb(0x000000, 0.15f)), kBar * 0.5f);
            dl.rect(th, pc.tint(Color::rgb(s.barDrag ? 0xB4B4C0 : 0x8A8A96, 0.85f)), kBar * 0.5f);
        }
        dl.popClip();
    }
    /// ARIA treegrid: one Row per shown line (level = depth + 1, expanded when it has children, selected) holding one
    /// GridCell with the text. Part ids: row = 2 * line + 1, cell = 2 * line + 2 (stable while lines above toggle).
    static void a11yParts(const UiContext& cui, NodeHandle n, A11ySnapshotNode& self, std::vector<A11ySnapshotNode>& out) {
        UiContext& ui = const_cast<UiContext&>(cui);
        const Rect c = ui.contentRect(n);
        const f32 rh = rowHeight(ui, n);
        const i64 cur = i64(ui.value(n));
        const u32 first = treeState(ui, n).first;
        self.rowCount = treeVisibleRows(ui, n);
        self.colCount = 1;
        eachTreeRow(ui.text(n), ui.userData(n), [&](u32 row, u32 line, u32 depth, bool kids, bool open, std::string_view t) {
            if (row < first) return true;
            const Rect b{c.x, c.y + f32(row - first) * rh, c.w, rh};
            if (b.y > c.bottom()) return false;
            const bool sel = lineSelected(ui, n, line);
            const u32 rowAt = u32(out.size());
            A11ySnapshotNode r;
            r.role = A11yRole::Row; r.part = 2u * line + 1u; r.rowIndex = i32(row); r.level = depth + 1u; r.bounds = b;
            r.name = std::string(t);
            r.state.selected = sel;
            r.state.expanded = kids ? (open ? 1 : 0) : -1;
            out.push_back(std::move(r));
            A11ySnapshotNode gc;
            gc.role = A11yRole::GridCell; gc.part = 2u * line + 2u; gc.parent = rowAt; gc.rowIndex = i32(row); gc.colIndex = 0;
            gc.name = std::string(t);
            const f32 x = b.x + 18.f + f32(depth) * kIndent;
            gc.bounds = {x, b.y, b.right() - x, rh}; // the text, not the arrow: a click selects
            gc.state.selected = sel;
            gc.focused = i64(line) == cur;
            out.push_back(std::move(gc));
            return true;
        });
    }
    /// AT Expand / Collapse on a row (or its cell) part.
    static bool a11yPartExpand(UiContext& ui, NodeHandle n, u32 part, bool open) {
        const u32 line = (part - 1u) / 2u;
        const Hit h = rowInfo(ui, n, [&](u32, u32 l) { return l == line; });
        if (h.line < 0 || !h.kids || h.open == open) return false;
        setTreeExpanded(ui, n, line, open);
        return true;
    }
    struct Hit { i64 line = -1; u32 depth = 0; bool kids = false, open = false; i64 row = -1; };
    static Hit rowInfo(UiContext& ui, NodeHandle n, auto pred) {
        Hit h;
        eachTreeRow(ui.text(n), ui.userData(n), [&](u32 row, u32 line, u32 depth, bool kids, bool open, std::string_view) {
            if (!pred(row, line)) return true;
            h = {i64(line), depth, kids, open, i64(row)};
            return false;
        });
        return h;
    }
    /// Visual rows from line a to line b (inclusive) as lines.
    static auto rowsFn(UiContext& ui, NodeHandle n) {
        return [&ui, n](i64 a, i64 b, std::vector<i64>& out) {
            const i64 ra = rowInfo(ui, n, [&](u32, u32 l) { return i64(l) == a; }).row, rb = rowInfo(ui, n, [&](u32, u32 l) { return i64(l) == b; }).row;
            const i64 lo = std::min(ra, rb), hi = std::max(ra, rb);
            eachTreeRow(ui.text(n), ui.userData(n), [&](u32 row, u32 line, u32, bool, bool, std::string_view) {
                if (i64(row) > hi) return false;
                if (i64(row) >= lo) out.push_back(line);
                return true;
            });
        };
    }
    /// Makes `line` current (value()); scrolls it into view.
    static void select(UiContext& ui, NodeHandle n, i64 line) {
        if (line < 0) return;
        ensureVisible(ui, n, rowInfo(ui, n, [&](u32, u32 l) { return i64(l) == line; }).row);
        if (i64(ui.value(n)) == line) return;
        ui.setValue(n, f32(line), true);
        std::string_view t = nth(ui.text(n), u32(line));
        ui.emit(n, SignalKind::TextChanged, {.text = t.substr(depthOf(t))});
        if (treeState(ui, n).sel.mode == SelectionMode::Single) ui.emit(n, SignalKind::SelectionChanged, {.value = f32(line)});
    }
    static void changed(UiContext& ui, NodeHandle n) {
        ui.invalidate(n);
        ui.emit(n, SignalKind::SelectionChanged, {.value = ui.value(n)});
    }
    /// keyboard move to `line` with selection per mode
    static void navTo(UiContext& ui, NodeHandle n, i64 line, u8 mods) {
        if (line < 0) return;
        TreeState& s = treeState(ui, n);
        select(ui, n, line);
        if (s.sel.navigate(line, 0, mods, rowsFn(ui, n))) changed(ui, n);
    }
    /// rows the band's vertical extent touches (whole rows: QTreeView selects by row)
    static void bandSelect(UiContext& ui, NodeHandle n, TreeState& st) {
        const Rect c = ui.contentRect(n);
        const f32 rh = rowHeight(ui, n);
        const i64 r0 = std::max<i64>(0, i64(st.first) + i64(std::floor((std::min(st.bandA.y, st.bandB.y) - c.y) / rh)));
        const i64 r1 = i64(st.first) + i64(std::floor((std::max(st.bandA.y, st.bandB.y) - c.y) / rh));
        std::set<u64> keys = st.bandBase;
        rowInfo(ui, n, [&](u32 r, u32 l) {
            if (i64(r) >= r0 && i64(r) <= r1) keys.insert(u64(l));
            return i64(r) > r1;
        });
        if (keys != st.sel.keys) { st.sel.keys = std::move(keys); changed(ui, n); }
        ui.invalidate(n);
    }
    static bool onEvent(EventContext& ec, const Event& e) {
        if (ec.phase != Phase::Target) return false;
        UiContext& ui = ec.ui;
        const NodeHandle n = ec.node;
        TreeState& st = treeState(ui, n);
        const i64 sel = i64(ui.value(n));
        if (e.type == EventType::Wheel) { setFirst(ui, n, i64(st.first) - i64(std::lround(e.wheel.y * 3.f))); return true; }
        if (e.type == EventType::PointerDown) {
            const Rect c = ui.contentRect(n);
            if (Rect tr, th; barGeom(ui, n, tr, th) && tr.contains(e.pos)) {
                st.barDrag = true;
                st.grab = th.contains(e.pos) ? e.pos.y - th.y : th.h * 0.5f;
                Event mv = e;
                mv.type = EventType::PointerMove;
                return onEvent(ec, mv);
            }
            const i64 row = i64(st.first) + i64((e.pos.y - c.y) / rowHeight(ui, n));
            const Hit h = rowInfo(ui, n, [&](u32 r, u32) { return i64(r) == row; });
            if (h.line < 0) {
                if (e.button == MouseButton::Left && (st.sel.mode == SelectionMode::Multi || st.sel.mode == SelectionMode::Extended)) {
                    st.band = true;
                    st.bandA = st.bandB = e.pos;
                    st.bandBase = (e.mods & (kModCtrl | kModSuper)) ? st.sel.keys : std::set<u64>{};
                    bandSelect(ui, n, st);
                }
                return true;
            }
            const f32 ax = c.x + 4.f + f32(h.depth) * kIndent;
            if (h.kids && e.pos.x >= ax && e.pos.x < ax + 14.f) { setTreeExpanded(ui, n, u32(h.line), !h.open); return true; }
            st.pendingLine = -1;
            const bool dragging = (st.o.drag || st.o.internalMove) && e.button == MouseButton::Left;
            if (dragging && !(e.mods & (kModShift | kModCtrl | kModSuper)) && st.sel.explicitSet() && st.sel.keys.contains(u64(h.line)) &&
                st.sel.mode != SelectionMode::Multi) {
                st.pendingLine = h.line; // keep the selection for a multi-row drag; narrow on release
                select(ui, n, h.line);
            } else {
                select(ui, n, h.line);
                if (st.sel.click(h.line, 0, e.mods, rowsFn(ui, n))) changed(ui, n);
            }
            if (dragging) {
                const bool internal = st.o.internalMove && !st.o.drag;
                const u32 line = u32(h.line);
                dnd::press(ui, n, e.pos, [line, internal](UiContext& c2, NodeHandle self, Vec2, DragPayload& p) {
                    TreeState& s2 = treeState(c2, self);
                    s2.pendingLine = -1;
                    std::vector<u32> lines;
                    if (s2.sel.explicitSet() && s2.sel.keys.contains(line))
                        for (u64 k : s2.sel.keys) lines.push_back(u32(k));
                    else lines.push_back(line);
                    s2.dragHeads = lines;
                    s2.droppedSelf = false;
                    p.set("application/x-fuse-tree-subtree", treeBlocks(c2, self, lines));
                    std::string txt, idx;
                    for (u32 l : lines) {
                        const std::string_view t = nth(c2.text(self), l);
                        if (!txt.empty()) { txt.push_back('\n'); idx.push_back(','); }
                        txt.append(t.substr(depthOf(t)));
                        idx.append(std::to_string(l));
                    }
                    p.set("text/plain", txt);
                    p.set("application/x-fuse-tree-line", idx);
                    p.label = lines.size() > 1 ? std::to_string(lines.size()) + " items" : txt;
                    p.actions = internal ? u8(DropMove) : u8(DropCopy | DropMove);
                    return true;
                }, [](UiContext& c2, NodeHandle self, u8 action) {
                    // QAbstractItemView::startDrag: a Move into another view (or app) takes the rows out here
                    TreeState& s2 = treeState(c2, self);
                    std::vector<u32> heads = std::move(s2.dragHeads);
                    const bool self2 = s2.droppedSelf;
                    s2.dragHeads.clear();
                    s2.droppedSelf = false;
                    if (action != DropMove || self2 || heads.empty()) return;
                    const auto gone = treeRemove(c2, self, heads);
                    c2.setValue(self, -1.f);
                    if (s2.sel.explicitSet()) { s2.sel.keys.clear(); TreeView::changed(c2, self); }
                    if (s2.o.removed) { auto cb = s2.o.removed; for (u32 h : gone) cb(c2, self, h); }
                });
            }
            return true;
        }
        if (e.type == EventType::PointerMove) {
            if (st.band) {
                if (ui.pressed() != n) { st.band = false; return false; }
                st.bandB = e.pos;
                const Rect c = ui.contentRect(n);
                if (e.pos.y > c.bottom()) setFirst(ui, n, i64(st.first) + 1);
                else if (e.pos.y < c.y) setFirst(ui, n, i64(st.first) - 1);
                bandSelect(ui, n, st);
                return true;
            }
            if (st.barDrag) {
                Rect tr, th;
                if (!barGeom(ui, n, tr, th)) return true;
                const u32 total = treeVisibleRows(ui, n), vis = visible(ui, n);
                const f32 f = std::clamp((e.pos.y - tr.y - st.grab) / std::max(1.f, tr.h - th.h), 0.f, 1.f);
                setFirst(ui, n, i64(std::lround(f * f32(total - vis))));
                return true;
            }
            return dnd::move(ui, n, e.pos, e.mods);
        }
        if (e.type == EventType::PointerUp) {
            if (st.barDrag) { st.barDrag = false; ui.invalidate(n); return true; }
            if (st.band) { st.band = false; ui.invalidate(n); return true; }
            if (dnd::release(ui, n, e.pos, e.mods)) return true;
            if (st.pendingLine >= 0) { // a click (no drag) on a selected row selects just it
                const i64 l = st.pendingLine;
                st.pendingLine = -1;
                if (st.sel.click(l, 0, 0, rowsFn(ui, n))) changed(ui, n);
            }
            return false;
        }
        if (e.type == EventType::KeyDown && e.key == Key::Escape && dragActive(ui)) { cancelDrag(ui); return true; }
        if (e.type != EventType::KeyDown) return false;
        const Hit cur = rowInfo(ui, n, [&](u32, u32 l) { return i64(l) == sel; });
        auto lineAtRow = [&](i64 want) { return rowInfo(ui, n, [&](u32 r, u32) { return i64(r) == want; }).line; };
        const i64 total = treeVisibleRows(ui, n), vis = visible(ui, n);
        switch (e.key) {
        case Key::Down: navTo(ui, n, lineAtRow(std::min(cur.row + 1, total - 1)), e.mods); return true;
        case Key::Up: navTo(ui, n, lineAtRow(cur.row < 0 ? 0 : std::max<i64>(cur.row - 1, 0)), e.mods); return true;
        case Key::PageDown: navTo(ui, n, lineAtRow(std::clamp<i64>(cur.row + vis, 0, total - 1)), e.mods); return true;
        case Key::PageUp: navTo(ui, n, lineAtRow(std::clamp<i64>(cur.row - vis, 0, total - 1)), e.mods); return true;
        case Key::Home: navTo(ui, n, lineAtRow(0), e.mods); return true;
        case Key::End: navTo(ui, n, lineAtRow(total - 1), e.mods); return true;
        case Key::Space:
            if (cur.line < 0) return false;
            if (st.sel.mode == SelectionMode::Multi || (st.sel.mode == SelectionMode::Extended && (e.mods & (kModCtrl | kModSuper)))) {
                st.sel.toggle(cur.line, 0);
                changed(ui, n);
                return true;
            }
            return false;
        case Key::A:
            if (!(e.mods & (kModCtrl | kModSuper))) return false;
            selectAllItems(ui, n);
            return true;
        case Key::Right: if (cur.kids && !cur.open) setTreeExpanded(ui, n, u32(cur.line), true); return true;
        case Key::Left:
            if (cur.kids && cur.open) setTreeExpanded(ui, n, u32(cur.line), false);
            else if (cur.depth > 0) { // to the parent
                i64 parent = -1;
                eachTreeRow(ui.text(n), ui.userData(n), [&](u32, u32 l, u32 d, bool, bool, std::string_view) {
                    if (i64(l) >= cur.line) return false;
                    if (d < cur.depth) parent = l;
                    return true;
                });
                navTo(ui, n, parent, 0);
            }
            return true;
        case Key::Enter:
            if (sel >= 0) ui.emit(n, SignalKind::Submitted, {.value = f32(sel)});
            return true;
        default: return false;
        }
    }
};

// ---- tab bar -----------------------------------------------------------------------------------------------------
/// QTabBar state beyond titles / current: closable / movable, disabled tabs, scroll offset, the live tab drag.
struct TabBarState {
    u32 gen = 0;
    bool closable = false, movable = false;
    std::set<u32> disabled;
    f32 scroll = 0.f;
    i32 lastCur = -1;
    i32 drag = -1; ///< tab being dragged (current index)
    f32 pressX = 0.f;
    bool dragging = false;
};
std::unordered_map<const UiContext*, std::unordered_map<u32, TabBarState>>& tabBars() {
    static std::unordered_map<const UiContext*, std::unordered_map<u32, TabBarState>> m;
    return m;
}
TabBarState& tabState(const UiContext& ui, NodeHandle n) {
    TabBarState& s = tabBars()[&ui][n.index];
    if (s.gen != n.generation) s = TabBarState{n.generation};
    return s;
}
void showPage(UiContext& ui, NodeHandle stack, u32 current);
struct TabBar {
    static constexpr std::string_view kName = "tabbar";
    static constexpr A11yRole kRole = A11yRole::Tab;
    static constexpr bool kFocusable = true;
    static constexpr bool kInteractive = true;
    static constexpr f32 kPad = 12.f, kClose = 18.f, kArrow = 18.f;
    static f32 tabW(UiContext& ui, NodeHandle n, std::string_view t) {
        return std::ceil(ui.measureString(t, ui.computed(n).fontSize)) + 2.f * kPad + (tabState(ui, n).closable ? kClose : 0.f);
    }
    static Size measure(UiContext& ui, NodeHandle n, Size) {
        f32 w = 0.f;
        eachLine(ui.text(n), [&](u32, std::string_view t) { w += tabW(ui, n, t); return true; });
        return {w, rowHeight(ui, n)};
    }
    static f32 total(UiContext& ui, NodeHandle n) { return measure(ui, n, {}).w; }
    /// Tabs scroll when they overflow (QTabBar::usesScrollButtons): two arrow buttons take the right end.
    static bool overflow(UiContext& ui, NodeHandle n) { return total(ui, n) > ui.contentRect(n).w + 0.5f; }
    static f32 viewW(UiContext& ui, NodeHandle n) { return ui.contentRect(n).w - (overflow(ui, n) ? 2.f * kArrow : 0.f); }
    static Rect tabRect(UiContext& ui, NodeHandle n, u32 index) {
        const Rect c = ui.contentRect(n);
        f32 x = c.x - tabState(ui, n).scroll;
        Rect out{};
        eachLine(ui.text(n), [&](u32 i, std::string_view t) {
            const f32 w = tabW(ui, n, t);
            if (i == index) { out = {x, c.y, w, c.h}; return false; }
            x += w;
            return true;
        });
        return out;
    }
    static Rect closeBox(const Rect& r) { return {r.right() - kPad * 0.5f - 14.f, r.y + r.h * 0.5f - 7.f, 14.f, 14.f}; }
    static i32 tabAt(UiContext& ui, NodeHandle n, f32 x) {
        const Rect c = ui.contentRect(n);
        if (x >= c.x + viewW(ui, n)) return -1;
        for (u32 i = 0, k = count(ui.text(n)); i < k; ++i) { const Rect r = tabRect(ui, n, i); if (x >= r.x && x < r.right()) return i32(i); }
        return -1;
    }
    static void setScroll(UiContext& ui, NodeHandle n, f32 v) {
        TabBarState& s = tabState(ui, n);
        const f32 mx = overflow(ui, n) ? std::max(0.f, total(ui, n) - viewW(ui, n)) : 0.f;
        const f32 nv = std::clamp(v, 0.f, mx);
        if (nv != s.scroll) { s.scroll = nv; ui.invalidate(n); }
    }
    static void ensureVisible(UiContext& ui, NodeHandle n, u32 i) {
        const Rect c = ui.contentRect(n), r = tabRect(ui, n, i);
        TabBarState& s = tabState(ui, n);
        const f32 x0 = r.x - c.x + s.scroll, x1 = x0 + r.w, vw = viewW(ui, n);
        if (x0 < s.scroll) setScroll(ui, n, x0);
        else if (x1 > s.scroll + vw) setScroll(ui, n, x1 - vw);
    }
    static void layoutHook(UiContext& ui, NodeHandle n) { // keep the scroll in range and the current tab in view
        TabBarState& s = tabState(ui, n);
        setScroll(ui, n, s.scroll);
        const i32 cur = i32(ui.value(n));
        if (cur != s.lastCur) { s.lastCur = cur; if (cur >= 0) ensureVisible(ui, n, u32(cur)); }
    }
    static void paint(const PaintContext& pc, DrawList& dl) {
        UiContext& ui = pc.ui;
        const Rect c = pc.content;
        const TabBarState& s = tabState(ui, pc.node);
        const u32 sel = u32(ui.value(pc.node));
        const bool ov = overflow(ui, pc.node);
        const f32 vw = viewW(ui, pc.node);
        dl.pushClip({c.x, c.y, vw, c.h});
        eachLine(ui.text(pc.node), [&](u32 i, std::string_view t) {
            const Rect r = tabRect(ui, pc.node, i);
            if (r.right() < c.x || r.x > c.x + vw) return true;
            if (i == sel) {
                dl.rect(r, pc.tint(Color::rgb(s.dragging && i32(i) == s.drag ? 0x40414C : 0x34353E)), 4.f);
                dl.rect({r.x + 4.f, c.bottom() - 2.f, r.w - 8.f, 2.f}, accent(pc));
            }
            const bool off = s.disabled.contains(i);
            const Rect tr{r.x, r.y, r.w - (s.closable ? kClose : 0.f), r.h};
            if (off) ui.paintString(dl, t, {tr.x + kPad, tr.y, tr.w - 2.f * kPad, tr.h}, pc.tint(pc.style.color.withAlpha(pc.style.color.a * 0.4f)),
                                    pc.style.fontSize, TextAlignment::Center, pc.node.index);
            else labelAt(pc, dl, t, tr, TextAlignment::Center);
            if (s.closable) {
                const Rect x = closeBox(r);
                const bool hot = ui.hovered() == pc.node && x.contains(ui.pointer());
                const Color col = pc.tint(hot ? Color::rgb(0xFF6B6B) : Color::rgb(0x9A9AA6));
                const f32 m = 4.f;
                dl.line({x.x + m, x.y + m}, {x.right() - m, x.bottom() - m}, col, 1.5f);
                dl.line({x.right() - m, x.y + m}, {x.x + m, x.bottom() - m}, col, 1.5f);
            }
            return true;
        });
        dl.popClip();
        if (ov) { // scroll buttons
            const Color col = pc.tint(Color::rgb(0x9A9AA6));
            for (int k = 0; k < 2; ++k) {
                const Rect b{c.x + vw + f32(k) * kArrow, c.y, kArrow, c.h};
                const f32 cx = b.x + b.w * 0.5f, cy = b.y + b.h * 0.5f, d = k ? 3.f : -3.f;
                dl.line({cx - d, cy - 5.f}, {cx + d, cy}, col, 1.5f);
                dl.line({cx + d, cy}, {cx - d, cy + 5.f}, col, 1.5f);
            }
        }
        dl.rect({c.x, c.bottom() - 1.f, c.w, 1.f}, pc.tint(Color::rgb(0x474955)));
    }
    /// next enabled tab from `from` stepping `dir` (no wrap); -1 if none
    static i32 step(UiContext& ui, NodeHandle n, i32 from, i32 dir) {
        const TabBarState& s = tabState(ui, n);
        const i32 c = i32(count(ui.text(n)));
        for (i32 i = from + dir; i >= 0 && i < c; i += dir) if (!s.disabled.contains(u32(i))) return i;
        return -1;
    }
    static bool onEvent(EventContext& ec, const Event& e) {
        if (ec.phase != Phase::Target) return false;
        UiContext& ui = ec.ui;
        const NodeHandle n = ec.node;
        TabBarState& s = tabState(ui, n);
        const u32 c = count(ui.text(n)), cur = u32(ui.value(n));
        auto pick = [&](i32 i) {
            if (c && i >= 0 && u32(i) < c && u32(i) != cur && !s.disabled.contains(u32(i))) { ui.setValue(n, f32(i), true); ensureVisible(ui, n, u32(i)); }
        };
        const Rect cr = ui.contentRect(n);
        switch (e.type) {
        case EventType::PointerDown: {
            if (overflow(ui, n) && e.pos.x >= cr.x + viewW(ui, n)) {
                setScroll(ui, n, s.scroll + (e.pos.x >= cr.x + viewW(ui, n) + kArrow ? 80.f : -80.f));
                return true;
            }
            const i32 i = tabAt(ui, n, e.pos.x);
            if (i < 0 || s.disabled.contains(u32(i))) return true;
            if (s.closable && closeBox(tabRect(ui, n, u32(i))).contains(e.pos)) { // QTabBar::tabCloseRequested
                ui.emit(n, SignalKind::Event, {.value = f32(i), .text = "tabCloseRequested"});
                return true;
            }
            pick(i);
            s.drag = s.movable ? i : -1;
            s.pressX = e.pos.x;
            s.dragging = false;
            return true;
        }
        case EventType::PointerMove: {
            if (ui.pressed() != n || s.drag < 0) { if (s.closable) ui.invalidate(n); return false; }
            if (!s.dragging && std::abs(e.pos.x - s.pressX) < 10.f) return true;
            s.dragging = true;
            // live reorder (QTabBar::setMovable): swap with a neighbour once the pointer passes its middle
            for (i32 to : {s.drag - 1, s.drag + 1}) {
                if (to < 0 || u32(to) >= c) continue;
                const Rect r = tabRect(ui, n, u32(to));
                if ((to < s.drag && e.pos.x < r.x + r.w * 0.5f) || (to > s.drag && e.pos.x > r.x + r.w * 0.5f)) {
                    moveTab(ui, n, u32(s.drag), u32(to));
                    s.drag = to;
                    break;
                }
            }
            ui.invalidate(n);
            return true;
        }
        case EventType::PointerUp:
            if (s.dragging) ui.invalidate(n);
            s.drag = -1;
            s.dragging = false;
            return false;
        case EventType::Wheel: { // QTabBar: the wheel changes the current tab
            const i32 t = step(ui, n, i32(cur), e.wheel.y > 0.f || e.wheel.x > 0.f ? -1 : 1);
            if (t >= 0) pick(t);
            return true;
        }
        case EventType::KeyDown:
            switch (e.key) {
            case Key::Right: if (const i32 t = step(ui, n, i32(cur), 1); t >= 0) pick(t); return true;
            case Key::Left: if (const i32 t = step(ui, n, i32(cur), -1); t >= 0) pick(t); return true;
            case Key::Home: if (const i32 t = step(ui, n, -1, 1); t >= 0) pick(t); return true;
            case Key::End: if (const i32 t = step(ui, n, i32(c), -1); t >= 0) pick(t); return true;
            default: return false;
            }
        default: return false;
        }
    }
    /// Moves tab `from` to index `to` (titles, current, disabled flags and, in a tab widget, its pages).
    static void moveTab(UiContext& ui, NodeHandle n, u32 from, u32 to) {
        const u32 c = count(ui.text(n));
        if (from >= c || to >= c || from == to) return;
        std::vector<std::string> t;
        eachLine(ui.text(n), [&](u32, std::string_view l) { t.emplace_back(l); return true; });
        std::string moved = std::move(t[from]);
        t.erase(t.begin() + from);
        t.insert(t.begin() + to, std::move(moved));
        std::string joined;
        for (std::size_t i = 0; i < t.size(); ++i) { if (i) joined += '\n'; joined += t[i]; }
        ui.setText(n, joined);
        auto remap = [&](u32 i) -> u32 {
            if (i == from) return to;
            if (from < to && i > from && i <= to) return i - 1;
            if (to < from && i >= to && i < from) return i + 1;
            return i;
        };
        TabBarState& s = tabState(ui, n);
        std::set<u32> dis;
        for (u32 i : s.disabled) dis.insert(remap(i));
        s.disabled = std::move(dis);
        const u32 cur = remap(u32(ui.value(n)));
        ui.setValue(n, f32(cur));
        s.lastCur = i32(cur);
        const NodeHandle w = ui.parent(n);
        if (ui.alive(w) && ui.hasClass(w, "tabwidget") && ui.firstChild(w) == n) { // pages follow
            const NodeHandle stack = ui.nextSibling(n);
            NodeHandle pg = ui.firstChild(stack);
            for (u32 i = 0; i < from && ui.alive(pg); ++i) pg = ui.nextSibling(pg);
            NodeHandle ref = ui.firstChild(stack);
            for (u32 i = 0; i < to + (from < to ? 1u : 0u) && ui.alive(ref); ++i) ref = ui.nextSibling(ref);
            if (ui.alive(pg)) {
                if (ui.alive(ref)) ui.insertBefore(pg, ref);
                else ui.moveTo(pg, stack);
            }
            showPage(ui, stack, cur);
        }
        char b[48];
        std::snprintf(b, sizeof b, "tabMoved %u %u", from, to);
        ui.emit(n, SignalKind::Event, {.value = f32(to), .text = b});
        ui.invalidate(n);
    }
};

void showPage(UiContext& ui, NodeHandle stack, u32 current) {
    u32 i = 0;
    for (NodeHandle p = ui.firstChild(stack); ui.alive(p); p = ui.nextSibling(p), ++i)
        ui.setCss(p, "display", i == current ? "flex" : "none");
}

} // namespace

// ---- registration --------------------------------------------------------------------------------------------------
void registerTierB(UiContext& ui) {
    WidgetRegistry& r = ui.registry();
    if (r.find("popup") != 0xFFFFFFFFu) return;
    r.registerWidget<Popup>();
    r.registerWidget<Menu>();
    r.registerWidget<MenuItem>();
    r.registerWidget<MenuBar>();
    r.registerWidget<ToolBar>();
    r.registerWidget<ToolBarGrip>();
    r.registerWidget<ToolBarSep>();
    r.registerWidget<MenuButton>();
    r.registerWidget<Tooltip>();
    r.registerWidget<ListView>();
    r.registerWidget<TreeView>();
    r.registerWidget<TabBar>();
    detail::registerTierB2(ui);
}

namespace detail {
bool openComboPopup(UiContext& ui, NodeHandle combo) {
    if (ui.registry().find("popup") == 0xFFFFFFFFu) return false;
    const Rect r = ui.rect(combo);
    const NodeHandle scope = popupMenu(ui, combo, ui.text(combo), {r.x, r.bottom() + 2.f});
    if (!ui.alive(scope)) return false;
    // check and focus the current item; the list is at least as wide as the box
    const NodeHandle menu = ui.firstChild(scope);
    setPx(ui, menu, Prop::MinWidth, r.w);
    const u32 cur = u32(ui.value(combo));
    for (NodeHandle it = ui.firstChild(menu); ui.alive(it); it = ui.nextSibling(it))
        if (isClass(ui, it, "menuitem") && u32(ui.value(it)) == cur) {
            ui.setChecked(it, true);
            ui.setFocus(it, FocusReason::Keyboard);
        }
    return true;
}
} // namespace detail

NodeHandle overlayLayer(UiContext& ui) {
    NodeHandle l = ui.find(kLayerId);
    if (!ui.alive(l)) return make(ui, "layer", ui.root(), kLayerId);
    if (ui.nextSibling(l).valid()) ui.moveTo(l, ui.root());
    return l;
}

namespace detail {
NodeHandle popupMenuAt(UiContext& ui, NodeHandle owner, std::string_view items, Vec2 at, u32 base, std::string_view id) {
    registerTierB(ui);
    const NodeHandle scope = make(ui, "popup", overlayLayer(ui), id);
    if (!ui.alive(scope)) return {};
    ui.setUserData(scope, pack(owner));
    for (Prop p : {Prop::Left, Prop::Top, Prop::Right, Prop::Bottom}) setPx(ui, scope, p, 0.f);
    const NodeHandle menu = make(ui, "menu", scope);
    // the popup layer clamps the measured panel into the viewport ("clamp" class); open upward when it would not fit
    ui.addClass(menu, "clamp");
    const Rect area = ui.popupArea(); // the screen with OS-window popups, the viewport otherwise
    u32 rows = 0;
    eachLine(items, [&](u32, std::string_view t) { rows += t.empty() || t[0] != '\t'; return true; });
    const f32 estH = f32(rows) * 26.f + 8.f;
    setPx(ui, menu, Prop::Left, at.x);
    setPx(ui, menu, Prop::Top, at.y + estH > area.y + area.h && at.y - estH >= area.y ? at.y - estH : at.y);
    NodeHandle firstItem, item;
    std::string sub; // creation-time only
    auto flush = [&] {
        if (ui.alive(item) && !sub.empty()) { sub.insert(0, "\n"); sub.insert(0, ui.text(item)); ui.setText(item, sub); }
        sub.clear();
    };
    eachLine(items, [&](u32 i, std::string_view t) {
        if (!t.empty() && t[0] == '\t') { // child of the previous item
            if (!sub.empty()) sub += '\n';
            sub += t.substr(1);
            return true;
        }
        flush();
        item = {};
        if (t == "-") { separator(ui, menu); return true; }
        item = make(ui, "menuitem", menu);
        ui.setText(item, t);
        ui.setRange(item, 0.f, 1e9f, 0.f);
        ui.setValue(item, f32(base + i));
        if (!firstItem.valid()) firstItem = item;
        return true;
    });
    flush();
    ui.setModal(scope);
    if (ui.alive(firstItem)) ui.setFocus(firstItem, FocusReason::Keyboard);
    return scope;
}
NodeHandle popupPanel(UiContext& ui, NodeHandle owner, Vec2 at) {
    registerTierB(ui);
    const NodeHandle scope = make(ui, "popup", overlayLayer(ui));
    if (!ui.alive(scope)) return {};
    ui.setUserData(scope, pack(owner));
    for (Prop p : {Prop::Left, Prop::Top, Prop::Right, Prop::Bottom}) setPx(ui, scope, p, 0.f);
    const NodeHandle card = panel(ui, scope);
    ui.addClass(card, "card");
    ui.addClass(card, "clamp");
    setPx(ui, card, Prop::Left, at.x);
    setPx(ui, card, Prop::Top, at.y);
    ui.setModal(scope);
    return card;
}
} // namespace detail

NodeHandle popupMenu(UiContext& ui, NodeHandle owner, std::string_view items, Vec2 at, std::string_view id) {
    return detail::popupMenuAt(ui, owner, items, at, 0u, id);
}

void closePopup(UiContext& ui, NodeHandle n) {
    const NodeHandle scope = scopeOf(ui, n);
    if (!ui.alive(scope)) return;
    ui.setModal({});
    const NodeHandle owner = unpack(ui.userData(scope));
    if (ui.hasClass(scope, "modal")) {
        ui.setCss(scope, "display", "none"); // dialogs persist
    } else {
        ui.setCss(scope, "display", "none");
        ui.post(&destroyCmd, scope);
    }
    // closing a submenu hands input back to the parent menu
    if (const NodeHandle parentScope = scopeOf(ui, owner); ui.alive(parentScope) && popupOpen(ui, parentScope)) ui.setModal(parentScope);
    if (ui.alive(owner)) ui.setFocus(owner, FocusReason::Programmatic);
    else ui.setFocus({}, FocusReason::Programmatic);
}

bool popupOpen(const UiContext& ui, NodeHandle scope) { return ui.alive(scope) && ui.computed(scope).visible; }

NodeHandle menuBar(UiContext& ui, NodeHandle parent, std::string_view id) {
    registerTierB(ui);
    return make(ui, "menubar", parent, id);
}
namespace {
struct ToolBarState {
    u32 gen = 0;
    bool vertical = false;
    NodeHandle ext;
    NodeHandle menu; ///< the open extension menu
    std::unordered_map<u32, f32> size; ///< last shown main-axis size per item (node index)
    std::vector<NodeHandle> hidden;    ///< items this bar hid (in order)
    NodeHandle grip;                   ///< movable: the handle (first child)
    bool floatable = true;
    Vec2 press, grab;
    bool dragging = false;
};
std::unordered_map<const UiContext*, std::unordered_map<u32, ToolBarState>>& toolBars() {
    static std::unordered_map<const UiContext*, std::unordered_map<u32, ToolBarState>> m;
    return m;
}
ToolBarState* toolBarState(const UiContext& ui, NodeHandle bar) {
    auto& m = toolBars()[&ui];
    auto it = m.find(bar.index);
    return it != m.end() && it->second.gen == bar.generation && ui.alive(bar) ? &it->second : nullptr;
}
bool isSep(const UiContext& ui, NodeHandle n) { return ui.classOf(n) == ui.registry().find("toolbarseparator"); }
/// After layout: keep the extension button last, then hide what does not fit (QToolBarLayout).
void toolBarLayout(UiContext& ui, NodeHandle bar) {
    ToolBarState* st = toolBarState(ui, bar);
    if (!st || !ui.alive(st->ext)) return;
    for (NodeHandle c = ui.nextSibling(st->ext); ui.alive(c); c = ui.nextSibling(st->ext)) ui.insertBefore(c, st->ext);
    const bool v = st->vertical;
    const Rect cr = ui.contentRect(bar);
    const f32 availAll = v ? cr.h : cr.w, gap = ui.computed(bar).gap;
    bool reshrink = false; // items keep their natural size (no flex squeeze): measured on the next pass
    for (NodeHandle c = ui.firstChild(bar); ui.alive(c); c = ui.nextSibling(c))
        if (ui.computed(c).shrink != 0.f) { ui.setCss(c, "flex-shrink", "0"); reshrink = true; }
    if (reshrink) { ui.invalidate(bar); return; }
    std::vector<NodeHandle> items;
    f32 avail = availAll;
    for (NodeHandle c = ui.firstChild(bar); ui.alive(c) && c != st->ext; c = ui.nextSibling(c)) {
        if (c == st->grip) { const Rect r = ui.rect(c); avail -= (v ? r.h : r.w) + gap; continue; } // never overflows
        const bool ours = std::find(st->hidden.begin(), st->hidden.end(), c) != st->hidden.end();
        if (!ours && !ui.computed(c).visible) continue; // hidden by the app: not ours to show
        if (!ours) { const Rect r = ui.rect(c); st->size[c.index] = v ? r.h : r.w; }
        items.push_back(c);
    }
    auto sz = [&](NodeHandle c) { auto it = st->size.find(c.index); return it == st->size.end() ? 0.f : it->second; };
    f32 total = 0.f;
    for (std::size_t i = 0; i < items.size(); ++i) total += sz(items[i]) + (i ? gap : 0.f);
    std::vector<NodeHandle> hide;
    if (total > avail + 0.5f) {
        const Rect er = ui.rect(st->ext);
        f32 extSz = ui.computed(st->ext).visible ? (v ? er.h : er.w) : 0.f;
        if (extSz <= 0.f) extSz = st->size.count(st->ext.index) ? st->size[st->ext.index] : 24.f;
        const f32 budget = avail - extSz - gap;
        f32 run = 0.f;
        std::size_t keep = 0;
        for (; keep < items.size(); ++keep) {
            const f32 next = run + (keep ? gap : 0.f) + sz(items[keep]);
            if (next > budget + 0.5f) break;
            run = next;
        }
        while (keep > 0 && isSep(ui, items[keep - 1])) --keep; // no separator left dangling before the cut
        hide.assign(items.begin() + std::ptrdiff_t(keep), items.end());
    }
    if (hide == st->hidden) return; // stable: no style churn
    for (NodeHandle c : st->hidden) if (ui.alive(c)) ui.setCss(c, "display", "flex");
    for (NodeHandle c : hide) ui.setCss(c, "display", "none");
    st->hidden = std::move(hide);
    if (st->hidden.empty()) ui.setCss(st->ext, "display", "none");
    else {
        ui.setCss(st->ext, "display", "flex");
        const Rect er = ui.rect(st->ext);
        if ((v ? er.h : er.w) > 0.f) st->size[st->ext.index] = v ? er.h : er.w;
    }
}
} // namespace

NodeHandle toolBar(UiContext& ui, NodeHandle parent, bool vertical, std::string_view id) {
    registerTierB(ui);
    const NodeHandle bar = make(ui, "toolbar", parent, id);
    ui.setA11y(bar, A11yRole::ToolBar, vertical ? "vertical toolbar" : "toolbar");
    if (vertical) ui.setCss(bar, "flex-direction", "column");
    ui.setCss(bar, "overflow", "hidden");
    ToolBarState& st = toolBars()[&ui][bar.index];
    st = ToolBarState{bar.generation, vertical};
    st.ext = toolButton(ui, bar, "\u00BB");
    ui.addClass(st.ext, "toolbar-extension");
    ui.setA11y(st.ext, A11yRole::Button, "More");
    ui.setCss(st.ext, "flex-shrink", "0");
    ui.connect(st.ext, SignalKind::Clicked, [bar](UiContext& c, NodeHandle ext, const SignalArgs&) {
        ToolBarState* s = toolBarState(c, bar);
        if (!s || s->hidden.empty()) return;
        std::string items;
        for (NodeHandle h : s->hidden) {
            if (!items.empty()) items.push_back('\n');
            if (isSep(c, h)) { items += "-"; continue; }
            std::string t(c.text(h));
            items += t.empty() ? std::string("(item)") : t;
        }
        const Rect r = c.rect(ext);
        s->menu = popupMenu(c, ext, items, s->vertical ? Vec2{r.right(), r.y} : Vec2{r.x, r.bottom()});
    });
    ui.connect(st.ext, SignalKind::Submitted, [bar](UiContext& c, NodeHandle, const SignalArgs& a) {
        ToolBarState* s = toolBarState(c, bar);
        if (!s) return;
        const u32 i = u32(a.value);
        if (c.alive(s->menu)) closePopup(c, s->menu); // also when the pick was signalled on the button directly
        s->menu = {};
        if (i < s->hidden.size() && c.alive(s->hidden[i])) c.emit(s->hidden[i], SignalKind::Clicked, {});
    });
    ui.setCss(st.ext, "display", "none");
    ui.setLayoutHook(bar, &toolBarLayout);
    return bar;
}
NodeHandle toolBarSeparator(UiContext& ui, NodeHandle bar) {
    const NodeHandle s = make(ui, "toolbarseparator", bar, {});
    const ToolBarState* st = toolBarState(ui, bar);
    const bool v = st && st->vertical;
    ui.setCss(s, v ? "height" : "width", "1px");
    ui.setCss(s, v ? "width" : "height", "auto");
    ui.setCss(s, "align-self", "stretch");
    ui.setCss(s, "margin", v ? "4px 2px" : "2px 4px");
    ui.setCss(s, "background", "#55555f");
    ui.setCss(s, "flex-shrink", "0");
    return s;
}
// ---- movable / floating tool bars (QToolBar::setMovable / setFloatable, QMainWindow tool bar areas) ----------------
namespace {
bool barVertical(const UiContext& ui, NodeHandle bar) { const ToolBarState* st = toolBarState(ui, bar); return st && st->vertical; }
void floatClear(UiContext& ui, NodeHandle bar) {
    for (const char* p : {"left", "top"}) ui.setCss(bar, p, "auto");
}
/// Insertion point among an area's bars for a pointer at `p` (before the first bar whose middle is past it).
NodeHandle areaBefore(const UiContext& ui, NodeHandle area, NodeHandle self, Vec2 p) {
    const bool v = ui.hasClass(area, "vertical");
    for (NodeHandle c = ui.firstChild(area); ui.alive(c); c = ui.nextSibling(c)) {
        if (c == self) continue;
        const Rect r = ui.rect(c);
        if ((v ? p.y < r.y + r.h * 0.5f : p.x < r.x + r.w * 0.5f)) return c;
    }
    return {};
}
std::vector<NodeHandle> allAreas(const UiContext& ui) {
    std::vector<NodeHandle> out, stack{ui.root()};
    while (!stack.empty()) {
        const NodeHandle n = stack.back();
        stack.pop_back();
        for (NodeHandle c = ui.firstChild(n); ui.alive(c); c = ui.nextSibling(c)) {
            if (!ui.computed(c).visible) continue;
            if (ui.hasClass(c, "toolbararea")) out.push_back(c);
            else stack.push_back(c);
        }
    }
    return out;
}
} // namespace
NodeHandle toolBarArea(UiContext& ui, NodeHandle parent, bool vertical, std::string_view id) {
    registerTierB(ui);
    const NodeHandle a = panel(ui, parent, id);
    ui.addClass(a, "toolbararea");
    if (vertical) ui.addClass(a, "vertical");
    ui.setCss(a, "flex-direction", vertical ? "column" : "row");
    ui.setCss(a, "padding", "0px");
    ui.setCss(a, "gap", "2px");
    ui.setCss(a, vertical ? "min-width" : "min-height", "6px"); // an empty area stays a drop target
    return a;
}
void setToolBarOrientation(UiContext& ui, NodeHandle bar, bool vertical) {
    ToolBarState* st = toolBarState(ui, bar);
    if (!st || st->vertical == vertical) return;
    st->vertical = vertical;
    st->size.clear();
    ui.setCss(bar, "flex-direction", vertical ? "column" : "row");
    ui.setA11y(bar, A11yRole::ToolBar, vertical ? "vertical toolbar" : "toolbar");
    for (NodeHandle c = ui.firstChild(bar); ui.alive(c); c = ui.nextSibling(c))
        if (isSep(ui, c)) {
            ui.setCss(c, vertical ? "height" : "width", "1px");
            ui.setCss(c, vertical ? "width" : "height", "auto");
            ui.setCss(c, "margin", vertical ? "4px 2px" : "2px 4px");
        }
    ui.emit(bar, SignalKind::Event, {.flag = vertical, .text = "orientationChanged"});
}
bool toolBarIsVertical(const UiContext& ui, NodeHandle bar) { return barVertical(ui, bar); }
void setToolBarMovable(UiContext& ui, NodeHandle bar, bool on) {
    ToolBarState* st = toolBarState(ui, bar);
    if (!st || on == ui.alive(st->grip)) return;
    if (on) {
        st->grip = make(ui, "toolbargrip", bar);
        ui.setCss(st->grip, "flex-shrink", "0");
        ui.setCss(st->grip, "align-self", "stretch");
        if (const NodeHandle f = ui.firstChild(bar); f != st->grip) ui.insertBefore(st->grip, f);
    } else {
        ui.destroy(st->grip);
        st->grip = {};
    }
}
void setToolBarFloatable(UiContext& ui, NodeHandle bar, bool on) { if (ToolBarState* st = toolBarState(ui, bar)) st->floatable = on; }
void toolBarAddTo(UiContext& ui, NodeHandle area, NodeHandle bar, NodeHandle before) {
    if (!toolBarState(ui, bar) || !ui.alive(area)) return;
    const bool wasFloating = ui.hasClass(bar, "toolbar-floating");
    if (wasFloating) { ui.removeClass(bar, "toolbar-floating"); floatClear(ui, bar); }
    if (ui.alive(before) && ui.parent(before) == area) ui.insertBefore(bar, before);
    else ui.moveTo(bar, area);
    setToolBarOrientation(ui, bar, ui.hasClass(area, "vertical"));
    if (wasFloating) ui.emit(bar, SignalKind::Event, {.flag = false, .text = "topLevelChanged"});
}
void toolBarFloat(UiContext& ui, NodeHandle bar, Vec2 at) {
    if (!toolBarState(ui, bar)) return;
    const bool was = ui.hasClass(bar, "toolbar-floating");
    ui.moveTo(bar, overlayLayer(ui));
    ui.addClass(bar, "toolbar-floating");
    char b[32];
    std::snprintf(b, sizeof b, "%.0fpx", double(at.x));
    ui.setCss(bar, "left", b);
    std::snprintf(b, sizeof b, "%.0fpx", double(at.y));
    ui.setCss(bar, "top", b);
    if (!was) ui.emit(bar, SignalKind::Event, {.flag = true, .text = "topLevelChanged"});
}
bool toolBarIsFloating(const UiContext& ui, NodeHandle bar) { return ui.alive(bar) && ui.hasClass(bar, "toolbar-floating"); }
namespace detail {
void toolBarGripPress(UiContext& ui, NodeHandle bar, Vec2 p) {
    ToolBarState* st = toolBarState(ui, bar);
    if (!st) return;
    const Rect r = ui.rect(bar);
    st->press = p;
    st->grab = {p.x - r.x, p.y - r.y};
    st->dragging = false;
}
void toolBarGripMove(UiContext& ui, NodeHandle bar, Vec2 p) {
    ToolBarState* st = toolBarState(ui, bar);
    if (!st) return;
    if (!st->dragging && std::hypot(p.x - st->press.x, p.y - st->press.y) < 10.f) return;
    st->dragging = true;
    for (NodeHandle a : allAreas(ui)) {
        if (!ui.rect(a).contains(p)) continue;
        const NodeHandle before = areaBefore(ui, a, bar, p);
        if (ui.parent(bar) != a || ui.nextSibling(bar) != before) toolBarAddTo(ui, a, bar, before);
        return;
    }
    if (st->floatable) toolBarFloat(ui, bar, {p.x - st->grab.x, p.y - st->grab.y});
}
void toolBarGripRelease(UiContext& ui, NodeHandle bar) { if (ToolBarState* st = toolBarState(ui, bar)) st->dragging = false; }
} // namespace detail
NodeHandle toolBarExtension(const UiContext& ui, NodeHandle bar) {
    const ToolBarState* st = toolBarState(ui, bar);
    return st ? st->ext : NodeHandle{};
}
std::vector<NodeHandle> toolBarOverflow(const UiContext& ui, NodeHandle bar) {
    const ToolBarState* st = toolBarState(ui, bar);
    return st ? st->hidden : std::vector<NodeHandle>{};
}
NodeHandle menuBarMenu(UiContext& ui, NodeHandle bar, std::string_view title, std::string_view items, std::string_view id) {
    const NodeHandle b = make(ui, "menubutton", bar, id);
    std::string t(title);
    t += '\n';
    t += items;
    ui.setText(b, t);
    ui.setA11y(b, A11yRole::Button, title);
    return b;
}

NodeHandle dialog(UiContext& ui, std::string_view title, std::string_view id) {
    registerTierB(ui);
    const NodeHandle scope = make(ui, "popup", overlayLayer(ui), id);
    ui.addClass(scope, "modal");
    for (Prop p : {Prop::Left, Prop::Top, Prop::Right, Prop::Bottom}) setPx(ui, scope, p, 0.f);
    ui.setCss(scope, "display", "none");
    const NodeHandle card = panel(ui, scope);
    ui.addClass(card, "card");
    ui.addClass(card, "dialog");
    ui.set(card, Prop::CenterX, PropValue::ofBool(true));
    ui.set(card, Prop::CenterY, PropValue::ofBool(true));
    ui.setA11y(card, A11yRole::Window, title);
    const NodeHandle t = label(ui, card, title);
    ui.addClass(t, "title");
    panel(ui, card); // body
    return card;
}
NodeHandle dialogBody(const UiContext& ui, NodeHandle dlg) {
    const NodeHandle t = ui.firstChild(dlg);
    return ui.alive(t) ? ui.nextSibling(t) : NodeHandle{};
}
void openDialog(UiContext& ui, NodeHandle dlg) {
    const NodeHandle scope = ui.parent(dlg);
    if (!ui.alive(scope)) return;
    ui.setUserData(scope, pack(ui.focused()));
    overlayLayer(ui);
    ui.moveTo(scope, ui.parent(scope)); // topmost popup
    ui.setCss(scope, "display", "flex");
    ui.setModal(scope);
    const NodeHandle f = firstFocusable(ui, scope); // styles resolve on the next update: pick the first one directly
    if (f.valid()) ui.setFocus(f, FocusReason::Keyboard);
}
void closeDialog(UiContext& ui, NodeHandle dlg) { closePopup(ui, dlg); }
void setDialogDefaultButton(UiContext& ui, NodeHandle dlg, NodeHandle button) {
    const NodeHandle scope = ui.parent(dlg);
    if (!ui.alive(scope)) return;
    for (NodeHandle old = findClass(ui, scope, "dialog-default"); old.valid(); old = findClass(ui, scope, "dialog-default"))
        ui.removeClass(old, "dialog-default");
    if (ui.alive(button)) ui.addClass(button, "dialog-default");
}
void setDialogRejectButton(UiContext& ui, NodeHandle dlg, NodeHandle button) {
    const NodeHandle scope = ui.parent(dlg);
    if (!ui.alive(scope)) return;
    for (NodeHandle old = findClass(ui, scope, "dialog-reject"); old.valid(); old = findClass(ui, scope, "dialog-reject"))
        ui.removeClass(old, "dialog-reject");
    if (ui.alive(button)) ui.addClass(button, "dialog-reject");
}

NodeHandle setTooltip(UiContext& ui, NodeHandle node, std::string_view text, f32 delay) {
    registerTierB(ui);
    const NodeHandle tip = make(ui, "tooltip", overlayLayer(ui));
    ui.setText(tip, text);
    ui.addClass(tip, "tooltip");
    ui.connect(node, SignalKind::HoverChanged, [tip, delay](UiContext& c, NodeHandle n, const SignalArgs& a) {
        if (!c.alive(tip)) return;
        if (!a.flag) { c.setCss(tip, "display", "none"); return; }
        c.startTimer(delay, false, [tip, n](UiContext& cx) {
            if (!cx.alive(tip) || !cx.alive(n) || cx.hovered().index != n.index) return;
            const Rect r = cx.rect(n);
            setPx(cx, tip, Prop::Left, r.x);
            setPx(cx, tip, Prop::Top, r.bottom() + 4.f);
            overlayLayer(cx);
            cx.setCss(tip, "display", "flex");
        });
    });
    return tip;
}

NodeHandle listView(UiContext& ui, NodeHandle parent, std::string_view items, i32 selected, std::string_view id) {
    registerTierB(ui);
    const NodeHandle n = make(ui, "listview", parent, id);
    ui.setText(n, items);
    ui.setRange(n, -1.f, 1e9f, 1.f);
    ui.setValue(n, f32(selected));
    return n;
}
std::string_view listItem(const UiContext& ui, NodeHandle list, u32 index) { return nth(ui.text(list), index); }
u32 listFirstVisible(const UiContext& ui, NodeHandle list) { return u32(ui.userData(list)); }

NodeHandle treeView(UiContext& ui, NodeHandle parent, std::string_view lines, std::string_view id) {
    registerTierB(ui);
    const NodeHandle n = make(ui, "treeview", parent, id);
    ui.setText(n, lines);
    ui.setRange(n, -1.f, 1e9f, 1.f);
    ui.setValue(n, -1.f);
    return n;
}
bool treeExpanded(const UiContext& ui, NodeHandle tree, u32 line) { return lineOpen(ui.userData(tree), line); }
void setTreeExpanded(UiContext& ui, NodeHandle tree, u32 line, bool open) {
    if (line >= 64 || treeExpanded(ui, tree, line) == open) return;
    u64 m = ui.userData(tree);
    m = open ? (m & ~(u64(1) << line)) : (m | (u64(1) << line));
    ui.setUserData(tree, m);
    ui.setText(tree, std::string(ui.text(tree))); // re-measure + repaint
    ui.emit(tree, SignalKind::Toggled, {.value = f32(line), .flag = open});
}
void setTreeDragDrop(UiContext& ui, NodeHandle tree, TreeDragDrop opts) {
    const bool target = opts.internalMove || opts.acceptDrops;
    treeState(ui, tree).o = std::move(opts);
    if (!target) { setDropTarget(ui, tree, {}); return; }
    setDropTarget(ui, tree, [](UiContext& c, NodeHandle t, const DropEvent& e) -> u8 {
        if (e.phase == DropPhase::Leave) { setDropIndicator(c, t, {}); return 0; }
        TreeState& st = treeState(c, t);
        const bool self = e.source == t && e.payload->has("application/x-fuse-tree-line");
        // own rows: InternalMove moves; DragDrop moves or (Ctrl) copies. Foreign: DragDrop inserts.
        u8 action = 0;
        if (self) action = st.o.internalMove && !st.o.acceptDrops ? u8(DropMove) : (e.proposed & (DropCopy | DropMove));
        else if (st.o.acceptDrops && (e.payload->has("application/x-fuse-tree-subtree") || e.payload->has("text/plain")))
            action = e.proposed & DropCopy ? u8(DropCopy) : e.proposed & DropMove ? u8(DropMove) : (e.payload->actions & DropCopy ? u8(DropCopy) : u8(0));
        if (!action) return DropIgnore;
        std::vector<u32> froms;
        if (self)
            for (std::string_view v = e.payload->get("application/x-fuse-tree-line"); !v.empty();) {
                const std::size_t k = v.find(',');
                froms.push_back(u32(std::stoul(std::string(v.substr(0, k)))));
                v = k == std::string_view::npos ? std::string_view{} : v.substr(k + 1);
            }
        const Rect r = c.contentRect(t);
        const f32 rh = rowHeight(c, t);
        if (e.phase == DropPhase::Move && st.o.autoScroll) // QAbstractItemView::autoScroll near the top / bottom edge
            dnd::autoScroll(c, t, r, [](UiContext& c2, NodeHandle v, i32 dir) {
                const u32 before = treeState(c2, v).first;
                TreeView::setFirst(c2, v, i64(before) + dir);
                return treeState(c2, v).first != before;
            });
        const i64 row = i64(st.first) + i64((e.pos.y - r.y) / rh);
        const auto h = TreeView::rowInfo(c, t, [&](u32 rr, u32) { return i64(rr) == row; });
        const bool copySelf = self && action == DropCopy;
        if (h.line < 0) { // below the last row: foreign rows / copies append at the end
            if (self && !copySelf) { setDropIndicator(c, t, {}); return DropIgnore; }
            const f32 y = r.y + f32(i64(treeVisibleRows(c, t)) - i64(st.first)) * rh;
            if (e.phase != DropPhase::Drop) { setDropIndicator(c, t, {r.x + 4.f, y - 1.f, r.w - 8.f, 2.f}); return action; }
            setDropIndicator(c, t, {});
            const std::string block = self ? treeBlocks(c, t, froms) : std::string(e.payload->has("application/x-fuse-tree-subtree")
                                                                                       ? e.payload->get("application/x-fuse-tree-subtree")
                                                                                       : e.payload->get("text/plain"));
            const auto heads = treeInsert(c, t, block, -1, DropZone::Below);
            if (heads.empty()) return DropIgnore;
            if (self) st.droppedSelf = true;
            TreeView::select(c, t, heads.front());
            if (st.o.inserted) { auto cb = st.o.inserted; cb(c, t, heads.front(), u32(heads.size())); }
            return action;
        }
        const f32 y0 = r.y + f32(h.row - i64(st.first)) * rh, fy = (e.pos.y - y0) / rh;
        const DropZone z = fy < 0.25f ? DropZone::Above : fy > 0.75f ? DropZone::Below : DropZone::Onto;
        if (self && !copySelf) { // not into a moved row's own subtree
            std::vector<u32> depth;
            eachLine(c.text(t), [&](u32, std::string_view l) { depth.push_back(depthOf(l)); return true; });
            for (u32 from : froms) {
                if (from >= depth.size()) return DropIgnore;
                u32 end = from + 1;
                while (end < depth.size() && depth[end] > depth[from]) ++end;
                if (u32(h.line) >= from && u32(h.line) < end) { setDropIndicator(c, t, {}); return DropIgnore; }
            }
        }
        if (e.phase == DropPhase::Move && z == DropZone::Onto && h.kids && !h.open && st.o.autoExpandDelay >= 0.f) {
            const u32 line = u32(h.line);
            dnd::hoverDelay(c, t, line, st.o.autoExpandDelay, [line](UiContext& c2, NodeHandle v) { setTreeExpanded(c2, v, line, true); });
        }
        const f32 x = r.x + 4.f + f32(h.depth + (z == DropZone::Onto ? 1u : 0u)) * TreeView::kIndent;
        if (e.phase != DropPhase::Drop) {
            setDropIndicator(c, t, z == DropZone::Onto ? Rect{r.x + 1.f, y0 + 1.f, r.w - 2.f, rh - 2.f}
                                                       : Rect{x, z == DropZone::Above ? y0 - 1.f : y0 + rh - 1.f, r.right() - x, 2.f});
            return action;
        }
        setDropIndicator(c, t, {});
        std::vector<u32> newHeads;
        if (self && !copySelf) {
            const auto heads = treeMove(c, t, froms, u32(h.line), z);
            if (heads.empty()) return DropIgnore;
            for (const auto& hd : heads) newHeads.push_back(hd.second);
            st.droppedSelf = true;
            if (st.o.moved) { auto cb = st.o.moved; for (const auto& [o, nw] : heads) cb(c, t, o, nw); }
        } else {
            const std::string block = self ? treeBlocks(c, t, froms) : std::string(e.payload->has("application/x-fuse-tree-subtree")
                                                                                       ? e.payload->get("application/x-fuse-tree-subtree")
                                                                                       : e.payload->get("text/plain"));
            newHeads = treeInsert(c, t, block, h.line, z);
            if (newHeads.empty()) return DropIgnore;
            if (self) st.droppedSelf = true;
            if (st.o.inserted) { auto cb = st.o.inserted; cb(c, t, newHeads.front(), u32(newHeads.size())); }
        }
        c.setValue(t, -1.f);
        TreeView::select(c, t, newHeads.front());
        if (st.sel.explicitSet()) {
            st.sel.keys.clear();
            for (u32 hd : newHeads) st.sel.keys.insert(hd);
            st.sel.anchorR = newHeads.front();
            TreeView::changed(c, t);
        }
        return action;
    });
}
namespace detail {
ItemSelection* treeSelection(const UiContext& ui, NodeHandle tree) { return &treeState(ui, tree).sel; }
u32 treeLineCount(const UiContext& ui, NodeHandle tree) { return count(ui.text(tree)); }
void treeSelectionChanged(UiContext& ui, NodeHandle tree) { TreeView::changed(ui, tree); }
} // namespace detail
u32 treeFirstVisible(const UiContext& ui, NodeHandle tree) { return treeState(ui, tree).first; }
void setTreeFirstVisible(UiContext& ui, NodeHandle tree, u32 row) { TreeView::setFirst(ui, tree, row); }
u32 treeVisibleRows(const UiContext& ui, NodeHandle tree) {
    u32 rows = 0;
    eachTreeRow(ui.text(tree), ui.userData(tree), [&](u32, u32, u32, bool, bool, std::string_view) { ++rows; return true; });
    return rows;
}

NodeHandle tabBar(UiContext& ui, NodeHandle parent, std::string_view titles, u32 current, std::string_view id) {
    registerTierB(ui);
    const NodeHandle n = make(ui, "tabbar", parent, id);
    ui.setText(n, titles);
    ui.setRange(n, 0.f, f32(std::max(1u, count(titles)) - 1), 1.f);
    ui.setValue(n, f32(current));
    ui.setLayoutHook(n, &TabBar::layoutHook);
    return n;
}
void setTabsClosable(UiContext& ui, NodeHandle bar, bool on) { tabState(ui, bar).closable = on;
    const std::string t(ui.text(bar)); // re-measure: tab widths include the close box
    ui.setText(bar, t + "\n");
    ui.setText(bar, t); }
bool tabsClosable(const UiContext& ui, NodeHandle bar) { return tabState(ui, bar).closable; }
void setTabsMovable(UiContext& ui, NodeHandle bar, bool on) { tabState(ui, bar).movable = on; }
bool tabsMovable(const UiContext& ui, NodeHandle bar) { return tabState(ui, bar).movable; }
void setTabEnabled(UiContext& ui, NodeHandle bar, u32 index, bool on) {
    TabBarState& s = tabState(ui, bar);
    if (on) s.disabled.erase(index); else s.disabled.insert(index);
    if (!on && u32(ui.value(bar)) == index) { // QTabBar moves off a disabled current tab
        i32 t = TabBar::step(ui, bar, i32(index), 1);
        if (t < 0) t = TabBar::step(ui, bar, i32(index), -1);
        if (t >= 0) ui.setValue(bar, f32(t), true);
    }
    ui.invalidate(bar);
}
bool tabEnabled(const UiContext& ui, NodeHandle bar, u32 index) { return !tabState(ui, bar).disabled.contains(index); }
void tabBarMoveTab(UiContext& ui, NodeHandle bar, u32 from, u32 to) { TabBar::moveTab(ui, bar, from, to); }
Rect tabBarTabRect(UiContext& ui, NodeHandle bar, u32 index) { return TabBar::tabRect(ui, bar, index); }
f32 tabBarScroll(const UiContext& ui, NodeHandle bar) { return tabState(ui, bar).scroll; }
NodeHandle tabWidgetAddPage(UiContext& ui, NodeHandle tabs, std::string_view title) {
    const NodeHandle bar = ui.firstChild(tabs), stack = ui.nextSibling(bar);
    std::string t(ui.text(bar));
    if (!t.empty() || ui.alive(ui.firstChild(stack))) t += '\n';
    t += title;
    ui.setText(bar, t);
    ui.setRange(bar, 0.f, f32(std::max(1u, count(t)) - 1), 1.f);
    const NodeHandle p = panel(ui, stack);
    ui.setA11y(p, A11yRole::Group, title);
    showPage(ui, stack, u32(ui.value(bar)));
    return p;
}
void tabWidgetRemovePage(UiContext& ui, NodeHandle tabs, u32 index) {
    const NodeHandle bar = ui.firstChild(tabs), stack = ui.nextSibling(bar);
    const u32 c = count(ui.text(bar));
    if (index >= c) return;
    std::string t;
    bool first = true;
    eachLine(ui.text(bar), [&](u32 i, std::string_view l) {
        if (i != index) { if (!first) t += '\n'; t += l; first = false; }
        return true;
    });
    ui.setText(bar, t);
    ui.setRange(bar, 0.f, f32(std::max(1u, c - 1) - 1), 1.f);
    TabBarState& s = tabState(ui, bar);
    std::set<u32> dis;
    for (u32 i : s.disabled) if (i != index) dis.insert(i > index ? i - 1 : i);
    s.disabled = std::move(dis);
    if (const NodeHandle pg = tabPage(ui, tabs, index); ui.alive(pg)) ui.destroy(pg);
    u32 cur = u32(ui.value(bar));
    const u32 nc = cur > index || (cur == index && cur + 1 >= c) ? (cur ? cur - 1 : 0) : cur;
    ui.setValue(bar, f32(nc), nc != cur || cur == index);
    showPage(ui, stack, nc);
}
NodeHandle tabWidget(UiContext& ui, NodeHandle parent, std::string_view titles, std::string_view id) {
    const NodeHandle w = panel(ui, parent, id);
    ui.addClass(w, "tabwidget");
    const NodeHandle bar = tabBar(ui, w, titles, 0);
    const NodeHandle stack = panel(ui, w);
    ui.setCss(stack, "flex-grow", "1");
    for (u32 i = 0, c = count(titles); i < c; ++i) {
        const NodeHandle p = panel(ui, stack);
        ui.setA11y(p, A11yRole::Group, nth(titles, i));
    }
    showPage(ui, stack, 0);
    ui.connect(bar, SignalKind::ValueChanged, [stack](UiContext& c, NodeHandle, const SignalArgs& a) { showPage(c, stack, u32(a.value)); });
    return w;
}
NodeHandle tabWidgetBar(const UiContext& ui, NodeHandle tabs) { return ui.firstChild(tabs); }
NodeHandle tabPage(const UiContext& ui, NodeHandle tabs, u32 index) {
    const NodeHandle bar = ui.firstChild(tabs);
    NodeHandle p = ui.alive(bar) ? ui.firstChild(ui.nextSibling(bar)) : NodeHandle{};
    for (u32 i = 0; i < index && ui.alive(p); ++i) p = ui.nextSibling(p);
    return ui.alive(p) ? p : NodeHandle{};
}

NodeHandle scrollArea(UiContext& ui, NodeHandle parent, std::string_view id) {
    const NodeHandle n = panel(ui, parent, id);
    ui.setCss(n, "overflow", "scroll");
    ui.setA11y(n, A11yRole::ScrollArea);
    return n;
}

} // namespace fuse::ui::widgets
