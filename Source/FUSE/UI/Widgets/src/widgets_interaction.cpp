// FUSE UI widgets: row drag and drop for list / table / grid views (on the core drag-and-drop of ui.hpp) and menus
// built from ActionRegistry lists. The views themselves are untouched: this is glue on their Event signal plus the
// core's drag source / drop filter hooks, using the row geometry helpers of interaction.hpp.
#include "fuse/ui/widgets/widgets.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace fuse::ui::widgets {
namespace {

enum class ViewKind : u8 { List, Table, Grid };
constexpr u32 kDragRowLocal = hashId("application/x-fuse-row-local"); ///< a row that may not leave its view

struct ViewDnd {
    const UiContext* ui = nullptr;
    u64 uid = 0; ///< ui->uid(): the table outlives contexts, `ui` is only used while UiContext::exists(uid)
    NodeHandle node;
    ViewKind kind = ViewKind::List;
    u32 flags = 0;
    ConnectionHandle conn;
};
// Process-wide table (the UI is single-threaded); an entry is reused once its node or its context is gone.
std::vector<ViewDnd>& viewTable() { static std::vector<ViewDnd> t; return t; }
ViewDnd* viewOf(const UiContext& ui, NodeHandle n) {
    for (ViewDnd& v : viewTable())
        if (v.uid == ui.uid() && v.node == n) return &v;
    return nullptr;
}

u32 viewCount(UiContext& ui, NodeHandle n, ViewKind k) {
    if (k == ViewKind::List) return listCount(ui, n);
    const ItemModel* m = k == ViewKind::Table ? tableModel(ui, n) : gridModel(ui, n);
    return m && m->rows ? m->rows(m->user) : 0u;
}
i32 viewRowAt(UiContext& ui, NodeHandle n, ViewKind k, Vec2 p) {
    return k == ViewKind::List ? listRowAt(ui, n, p) : k == ViewKind::Table ? tableRowAt(ui, n, p) : gridItemAt(ui, n, p);
}
Rect viewRowRect(UiContext& ui, NodeHandle n, ViewKind k, u32 row) {
    return k == ViewKind::List ? listRowRect(ui, n, row) : k == ViewKind::Table ? tableRowRect(ui, n, row) : gridItemRect(ui, n, row);
}
std::string viewText(UiContext& ui, NodeHandle n, ViewKind k, u32 row) {
    if (k == ViewKind::List) return std::string(listItem(ui, n, row));
    const ItemModel* m = k == ViewKind::Table ? tableModel(ui, n) : gridModel(ui, n);
    std::string scratch;
    if (!m || !m->data) return scratch;
    return std::string(m->data(m->user, row, 0, scratch));
}

/// Insert position for a drop at `p`: before the row under the pointer, or after it in its far half; past the end below
/// the last row.
u32 dropIndex(UiContext& ui, NodeHandle n, ViewKind k, Vec2 p) {
    const u32 count = viewCount(ui, n, k);
    const i32 row = viewRowAt(ui, n, k, p);
    if (row < 0) {
        if (count == 0) return 0;
        const Rect first = viewRowRect(ui, n, k, 0);
        return p.y < first.y ? 0u : count;
    }
    const Rect r = viewRowRect(ui, n, k, u32(row));
    const bool after = k == ViewKind::Grid ? p.x >= r.x + r.w * 0.5f : p.y >= r.y + r.h * 0.5f;
    return u32(row) + (after ? 1u : 0u);
}
Rect indicatorRect(UiContext& ui, NodeHandle n, ViewKind k, u32 index) {
    const Rect c = ui.contentRect(n);
    const u32 count = viewCount(ui, n, k);
    Rect line;
    if (count == 0) {
        line = {c.x, c.y, c.w, 2.f};
    } else if (k == ViewKind::Grid) {
        const Rect r = viewRowRect(ui, n, k, std::min(index, count - 1));
        line = {(index < count ? r.x : r.right()) - 1.f, r.y + 3.f, 2.f, r.h - 6.f};
    } else {
        const Rect r = viewRowRect(ui, n, k, std::min(index, count - 1));
        line = {r.x, (index < count ? r.y : r.bottom()) - 1.f, r.w, 2.f};
    }
    // keep the line inside the view
    line.x = std::clamp(line.x, c.x, std::max(c.x, c.right() - line.w));
    line.y = std::clamp(line.y, c.y, std::max(c.y, c.bottom() - line.h));
    return line;
}

// '\n'-separated list items
void splitLines(std::string_view s, std::vector<std::string>& out) {
    out.clear();
    if (s.empty()) return;
    std::size_t at = 0;
    for (;;) {
        const std::size_t nl = s.find('\n', at);
        out.emplace_back(s.substr(at, nl == std::string_view::npos ? std::string_view::npos : nl - at));
        if (nl == std::string_view::npos) break;
        at = nl + 1;
    }
}
void setLines(UiContext& ui, NodeHandle list, const std::vector<std::string>& lines) {
    std::string joined;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i) joined += '\n';
        joined += lines[i];
    }
    ui.setText(list, joined);
}

bool viewDragStart(UiContext& ui, NodeHandle n, Vec2 press, DragData& out, DragImage&) {
    const ViewDnd* v = viewOf(ui, n);
    if (!v || !(v->flags & (kViewDragReorder | kViewDragOut))) return false;
    const i32 row = viewRowAt(ui, n, v->kind, press);
    if (row < 0) return false;
    out.type = (v->flags & kViewDragOut) ? kDragRow : kDragRowLocal;
    out.payload[0] = u64(row);
    out.text = viewText(ui, n, v->kind, u32(row));
    return true;
}

bool viewDropFilter(UiContext& ui, NodeHandle n, const DragData& d, Vec2) {
    const ViewDnd* v = viewOf(ui, n);
    if (!v) return false;
    if (d.source == n) return (v->flags & kViewDragReorder) != 0;
    return d.type == kDragRow && (v->flags & kViewAcceptDrops) != 0;
}

void onViewEvent(UiContext& ui, NodeHandle n, const Event& e) {
    const ViewDnd* v = viewOf(ui, n);
    if (!v || !e.drag) return;
    const ViewKind kind = v->kind;
    switch (e.type) {
    case EventType::DragEnter:
    case EventType::DragMove: {
        // near the top / bottom edge the view scrolls one step per move, so any row can be reached
        const Rect c = ui.contentRect(n);
        const f32 edge = 10.f;
        if (c.contains(e.pos) && (e.pos.y < c.y + edge || e.pos.y >= c.bottom() - edge)) {
            Event w; w.type = EventType::Wheel; w.pos = e.pos;
            w.wheel.y = (e.pos.y < c.y + edge ? 1.f : -1.f) * (kind == ViewKind::Grid ? 1.f : 1.f / 3.f);
            ui.sendEvent(n, w);
        }
        ui.setDropIndicator(indicatorRect(ui, n, kind, dropIndex(ui, n, kind, e.pos)));
        break;
    }
    case EventType::Drop: {
        const DragData& d = *e.drag;
        const u32 to = dropIndex(ui, n, kind, e.pos);
        if (d.source == n) { // reorder inside the view: always a move
            ui.setDropAction(DropAction::Move);
            const u32 from = u32(d.payload[0]), dest = to > from ? to - 1 : to;
            if (dest == from) return;
            if (kind == ViewKind::List) {
                std::vector<std::string> lines;
                splitLines(ui.text(n), lines);
                if (from >= lines.size() || dest >= lines.size()) return;
                std::string moved = std::move(lines[from]);
                lines.erase(lines.begin() + from);
                lines.insert(lines.begin() + dest, std::move(moved));
                setLines(ui, n, lines);
                ui.setValue(n, f32(dest), true);
            }
            ui.emit(n, SignalKind::RowMoved, {.value = f32(from), .value2 = f32(dest)});
        } else {
            if (kind == ViewKind::List) {
                std::vector<std::string> lines;
                splitLines(ui.text(n), lines);
                const u32 at = std::min<u32>(to, u32(lines.size()));
                lines.insert(lines.begin() + at, d.text);
                setLines(ui, n, lines);
                ui.setValue(n, f32(at), true);
            }
            ui.emit(n, SignalKind::Dropped, {.value = f32(to), .text = d.text, .event = &e});
        }
        break;
    }
    case EventType::DragEnd: {
        if (e.drag->source != n) return;
        const bool accepted = e.dropAction != DropAction::None;
        if (accepted && ui.lastDropTarget() == n) return; // a reorder: RowMoved said it all
        const u32 row = u32(e.drag->payload[0]);
        if (kind == ViewKind::List && accepted && e.dropAction == DropAction::Move) { // the row moved to another view
            std::vector<std::string> lines;
            splitLines(ui.text(n), lines);
            if (row < lines.size()) {
                lines.erase(lines.begin() + row);
                setLines(ui, n, lines);
                const f32 sel = ui.value(n);
                if (sel == f32(row)) ui.setValue(n, -1.f, true);
                else if (sel > f32(row)) ui.setValue(n, sel - 1.f, true);
            }
        }
        ui.emit(n, SignalKind::DragEnded, {.value = f32(row), .flag = accepted, .value2 = f32(u8(e.dropAction))});
        break;
    }
    default: break;
    }
}

void setViewDragDrop(UiContext& ui, NodeHandle n, ViewKind kind, u32 flags) {
    if (!ui.alive(n)) return;
    ViewDnd* v = viewOf(ui, n);
    if (!v) {
        if (!flags) return;
        for (ViewDnd& e : viewTable())
            if (!e.ui || !UiContext::exists(e.uid) || !e.ui->alive(e.node)) { v = &e; break; }
        if (!v) v = &viewTable().emplace_back();
        *v = ViewDnd{};
        v->ui = &ui;
        v->uid = ui.uid();
        v->node = n;
        v->conn = ui.connect(n, SignalKind::Event, [](UiContext& c, NodeHandle node, const SignalArgs& a) {
            if (a.event) onViewEvent(c, node, *a.event);
        });
    }
    v->kind = kind;
    v->flags = flags;
    ui.setDragSource(n, (flags & (kViewDragReorder | kViewDragOut)) ? &viewDragStart : nullptr);
    if (flags & (kViewDragReorder | kViewAcceptDrops)) {
        ui.setDropTypes(n, {kDragRow, kDragRowLocal});
        ui.setDropFilter(n, &viewDropFilter);
    } else {
        ui.setDropTypes(n, {});
        ui.setDropFilter(n, nullptr);
    }
    if (!flags) {
        ui.disconnect(v->conn);
        *v = ViewDnd{};
    }
}

u64 packAction(ActionHandle a) { return (u64(a.generation) << 32) | u64(a.index); }
void triggerCmd(UiContext&, NodeHandle, u64 reg, u64 action) {
    reinterpret_cast<ActionRegistry*>(static_cast<std::uintptr_t>(reg))->trigger(ActionHandle{u32(action & 0xFFFFFFFFu), u32(action >> 32)});
}

void syncActionMenu(ActionRegistry& reg, NodeHandle button, u64 listId) {
    UiContext& ui = reg.ui();
    const std::string_view cur = ui.text(button);
    std::string text(cur.substr(0, cur.find('\n'))); // the title
    text += '\n';
    text += actionMenuItems(reg, u32(listId));
    if (text != cur) ui.setText(button, text);
}

} // namespace

void setListDragDrop(UiContext& ui, NodeHandle list, u32 flags) { setViewDragDrop(ui, list, ViewKind::List, flags); }
void setTableDragDrop(UiContext& ui, NodeHandle table, u32 flags) { setViewDragDrop(ui, table, ViewKind::Table, flags); }
void setGridDragDrop(UiContext& ui, NodeHandle grid, u32 flags) { setViewDragDrop(ui, grid, ViewKind::Grid, flags); }
u32 listDropIndex(UiContext& ui, NodeHandle list, Vec2 p) { return dropIndex(ui, list, ViewKind::List, p); }

// ---- action menus --------------------------------------------------------------------------------------------------
std::string actionMenuItems(const ActionRegistry& reg, u32 listId) {
    std::string out;
    bool firstLine = true;
    for (const ActionHandle a : reg.list(listId)) {
        if (!firstLine) out += '\n';
        firstLine = false;
        if (!reg.alive(a)) { out += '-'; continue; }
        if (!reg.enabled(a)) out += '\x01';
        if (reg.checked(a)) out += '\x02';
        out += reg.text(a);
        if (const std::string sc = shortcutToString(reg.shortcut(a)); !sc.empty()) {
            out += '\t';
            out += sc;
        }
    }
    return out;
}

NodeHandle actionMenuBarMenu(UiContext& ui, ActionRegistry& reg, NodeHandle bar, std::string_view title, u32 listId, std::string_view id) {
    const NodeHandle b = menuBarMenu(ui, bar, title, actionMenuItems(reg, listId), id);
    if (!ui.alive(b)) return b;
    ActionRegistry* r = &reg;
    // the menu button reports the pick after the popup is closed: the action may open a dialog right away
    ui.connect(b, SignalKind::Submitted, [r, listId](UiContext&, NodeHandle, const SignalArgs& a) {
        const auto& list = r->list(listId);
        const u32 i = u32(a.value);
        if (i < list.size()) r->trigger(list[i]);
    });
    reg.observe(b, &syncActionMenu, listId);
    return b;
}

NodeHandle actionPopupMenu(UiContext& ui, ActionRegistry& reg, NodeHandle owner, u32 listId, Vec2 at, std::string_view id) {
    const NodeHandle scope = popupMenu(ui, owner, actionMenuItems(reg, listId), at, id);
    if (!ui.alive(scope)) return scope;
    ActionRegistry* r = &reg;
    // the scope reports the pick while the popup is still open: trigger at the next update, once it is closed
    ui.connect(scope, SignalKind::Submitted, [r, listId](UiContext& c, NodeHandle, const SignalArgs& a) {
        const auto& list = r->list(listId);
        const u32 i = u32(a.value);
        if (i < list.size()) c.post(&triggerCmd, {}, u64(reinterpret_cast<std::uintptr_t>(r)), packAction(list[i]));
    });
    return scope;
}

} // namespace fuse::ui::widgets
