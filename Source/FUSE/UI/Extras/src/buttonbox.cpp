// QDialogButtonBox: standard / custom buttons with roles, laid out in the platform's button order.
#include "fuse/ui/extras/widgets_extra.hpp"

#include <algorithm>
#include <unordered_map>

namespace fuse::ui::widgets {
namespace {

struct Entry { NodeHandle btn; ButtonRole role; u32 std = 0; u32 seq = 0; };
struct BoxState {
    u32 gen = 0;
    DialogButtonBoxOptions o;
    std::vector<Entry> items;
    u32 seq = 0;
    NodeHandle def, rej;
};
std::unordered_map<const UiContext*, std::unordered_map<u32, BoxState>>& boxes() {
    static std::unordered_map<const UiContext*, std::unordered_map<u32, BoxState>> m;
    return m;
}
BoxState* boxOf(const UiContext& ui, NodeHandle b) {
    auto it = boxes().find(&ui);
    if (it == boxes().end()) return nullptr;
    auto j = it->second.find(b.index);
    return j != it->second.end() && j->second.gen == b.generation && ui.alive(b) ? &j->second : nullptr;
}

ButtonBoxLayout resolve(ButtonBoxLayout l) {
    if (l != ButtonBoxLayout::Platform) return l;
#if defined(__APPLE__)
    return ButtonBoxLayout::Mac;
#else
    return ButtonBoxLayout::Windows;
#endif
}
ButtonRole stdRole(u32 b) {
    switch (b) {
    case StdButton::Yes: case StdButton::YesToAll: return ButtonRole::Yes;
    case StdButton::No: case StdButton::NoToAll: return ButtonRole::No;
    case StdButton::Abort: case StdButton::Close: case StdButton::Cancel: return ButtonRole::Reject;
    case StdButton::Discard: return ButtonRole::Destructive;
    case StdButton::Help: return ButtonRole::Help;
    case StdButton::Apply: return ButtonRole::Apply;
    case StdButton::Reset: case StdButton::RestoreDefaults: return ButtonRole::Reset;
    default: return ButtonRole::Accept; // Ok, Save, SaveAll, Open, Retry, Ignore
    }
}

// Qt's QPlatformDialogHelper::buttonLayout sequences; a role marked reversed lists its buttons last-added first.
struct Slot { int role; bool rev; }; // role -1 = stretch
constexpr int S = -1;
constexpr Slot kWin[] = {{int(ButtonRole::Reset), false}, {S, false}, {int(ButtonRole::Yes), false}, {int(ButtonRole::Accept), false},
                         {int(ButtonRole::Destructive), false}, {int(ButtonRole::No), false}, {int(ButtonRole::Action), false},
                         {int(ButtonRole::Reject), false}, {int(ButtonRole::Apply), false}, {int(ButtonRole::Help), false}};
constexpr Slot kMac[] = {{int(ButtonRole::Help), false}, {int(ButtonRole::Reset), false}, {int(ButtonRole::Apply), false},
                         {int(ButtonRole::Action), false}, {S, false}, {int(ButtonRole::Destructive), true},
                         {int(ButtonRole::Reject), true}, {int(ButtonRole::Accept), true}, {int(ButtonRole::No), true},
                         {int(ButtonRole::Yes), true}};
constexpr Slot kKde[] = {{int(ButtonRole::Help), false}, {int(ButtonRole::Reset), false}, {S, false}, {int(ButtonRole::Yes), false},
                         {int(ButtonRole::No), false}, {int(ButtonRole::Action), false}, {int(ButtonRole::Accept), false},
                         {int(ButtonRole::Apply), false}, {int(ButtonRole::Destructive), false}, {int(ButtonRole::Reject), false}};
constexpr Slot kGnome[] = {{int(ButtonRole::Help), false}, {int(ButtonRole::Reset), false}, {S, false}, {int(ButtonRole::Action), false},
                           {int(ButtonRole::Apply), true}, {int(ButtonRole::Destructive), true}, {int(ButtonRole::Reject), true},
                           {int(ButtonRole::Accept), true}, {int(ButtonRole::No), true}, {int(ButtonRole::Yes), true}};

NodeHandle stretch(UiContext& ui, NodeHandle box) {
    const NodeHandle s = panel(ui, box);
    ui.addClass(s, "buttonbox-stretch");
    ui.setCss(s, "flex-grow", "1");
    return s;
}

/// Rebuilds the order: stretches are recreated, buttons moved in sequence (stable within a role: standard buttons in
/// Qt's enum order, then custom ones as added).
void relayout(UiContext& ui, NodeHandle box, BoxState& st) {
    std::vector<NodeHandle> old;
    for (NodeHandle c = ui.firstChild(box); ui.alive(c); c = ui.nextSibling(c))
        if (ui.hasClass(c, "buttonbox-stretch")) old.push_back(c);
    for (NodeHandle c : old) ui.destroy(c);
    ui.setCss(box, "flex-direction", st.o.vertical ? "column" : "row");
    std::span<const Slot> seq;
    switch (resolve(st.o.layout)) {
    case ButtonBoxLayout::Mac: seq = kMac; break;
    case ButtonBoxLayout::Kde: seq = kKde; break;
    case ButtonBoxLayout::Gnome: seq = kGnome; break;
    default: seq = kWin; break;
    }
    auto order = st.items;
    std::stable_sort(order.begin(), order.end(), [](const Entry& a, const Entry& b) {
        if ((a.std != 0) != (b.std != 0)) return a.std != 0;
        return a.std ? a.std < b.std : a.seq < b.seq;
    });
    if (st.o.centerButtons) stretch(ui, box);
    for (const Slot& sl : seq) {
        if (sl.role == S) { if (!st.o.centerButtons) stretch(ui, box); continue; }
        std::vector<NodeHandle> group;
        for (const Entry& e : order) if (int(e.role) == sl.role) group.push_back(e.btn);
        if (sl.rev) std::reverse(group.begin(), group.end());
        for (NodeHandle b : group) ui.moveTo(b, box);
    }
    if (st.o.centerButtons) stretch(ui, box);
    // default button: first Accept, else first Yes (in visual order)
    if (ui.alive(st.def)) { ui.removeClass(st.def, "default"); ui.removeClass(st.def, "dialog-default"); }
    if (ui.alive(st.rej)) ui.removeClass(st.rej, "dialog-reject");
    st.def = st.rej = {};
    for (ButtonRole want : {ButtonRole::Accept, ButtonRole::Yes}) {
        for (NodeHandle c = ui.firstChild(box); ui.alive(c) && !ui.alive(st.def); c = ui.nextSibling(c))
            for (const Entry& e : st.items) if (e.btn == c && e.role == want) { st.def = c; break; }
        if (ui.alive(st.def)) break;
    }
    if (ui.alive(st.def)) { ui.addClass(st.def, "default"); ui.addClass(st.def, "dialog-default"); }
    // the enclosing dialog's Escape presses the first Reject, else No, button (QDialog::reject)
    for (ButtonRole want : {ButtonRole::Reject, ButtonRole::No}) {
        for (NodeHandle c = ui.firstChild(box); ui.alive(c) && !ui.alive(st.rej); c = ui.nextSibling(c))
            for (const Entry& e : st.items) if (e.btn == c && e.role == want) { st.rej = c; break; }
        if (ui.alive(st.rej)) break;
    }
    if (ui.alive(st.rej)) ui.addClass(st.rej, "dialog-reject");
    ui.markAllDirty();
}

NodeHandle addEntry(UiContext& ui, NodeHandle box, BoxState& st, std::string_view text, ButtonRole role, u32 stdb) {
    const NodeHandle b = button(ui, box, text);
    if (!ui.alive(b)) return b;
    st.items.push_back({b, role, stdb, st.seq++});
    ui.connect(b, SignalKind::Clicked, [box, role](UiContext& u, NodeHandle self, const SignalArgs&) {
        if (!u.alive(box)) return;
        const std::string t(u.text(self));
        u.emit(box, SignalKind::Clicked, {.value = f32(u8(role)), .text = t});
        if (role == ButtonRole::Accept || role == ButtonRole::Yes) u.emit(box, SignalKind::Submitted, {.value = 1.f, .text = t});
        else if (role == ButtonRole::Reject || role == ButtonRole::No) u.emit(box, SignalKind::Submitted, {.value = 0.f, .text = t});
    });
    return b;
}
void setStandard(UiContext& ui, NodeHandle box, BoxState& st, u32 mask) {
    for (auto it = st.items.begin(); it != st.items.end();)
        if (it->std) { ui.destroy(it->btn); it = st.items.erase(it); } else ++it;
    for (u32 bit = StdButton::Ok; bit <= StdButton::RestoreDefaults; bit <<= 1)
        if (mask & bit) addEntry(ui, box, st, standardButtonText(bit, st.o.layout), stdRole(bit), bit);
}
} // namespace

std::string standardButtonText(u32 b, ButtonBoxLayout layout) {
    switch (b) {
    case StdButton::Ok: return "OK";
    case StdButton::Save: return "Save";
    case StdButton::SaveAll: return "Save All";
    case StdButton::Open: return "Open";
    case StdButton::Yes: return "Yes";
    case StdButton::YesToAll: return "Yes to All";
    case StdButton::No: return "No";
    case StdButton::NoToAll: return "No to All";
    case StdButton::Abort: return "Abort";
    case StdButton::Retry: return "Retry";
    case StdButton::Ignore: return "Ignore";
    case StdButton::Close: return "Close";
    case StdButton::Cancel: return "Cancel";
    case StdButton::Discard:
        switch (resolve(layout)) {
        case ButtonBoxLayout::Mac: return "Don't Save";
        case ButtonBoxLayout::Gnome: return "Close without Saving";
        default: return "Discard";
        }
    case StdButton::Help: return "Help";
    case StdButton::Apply: return "Apply";
    case StdButton::Reset: return "Reset";
    case StdButton::RestoreDefaults: return "Restore Defaults";
    default: return {};
    }
}

NodeHandle dialogButtonBox(UiContext& ui, NodeHandle parent, u32 standardButtons, const DialogButtonBoxOptions& o, std::string_view id) {
    registerExtraWidgets(ui);
    const NodeHandle box = panel(ui, parent, id);
    if (!ui.alive(box)) return box;
    ui.addClass(box, "dialog-buttonbox");
    ui.setCss(box, "gap", "8px");
    ui.setA11y(box, A11yRole::Group, "Dialog buttons");
    BoxState& st = boxes()[&ui][box.index];
    st = BoxState{.gen = box.generation, .o = o};
    setStandard(ui, box, st, standardButtons);
    relayout(ui, box, st);
    return box;
}
void setDialogButtonBoxStandardButtons(UiContext& ui, NodeHandle box, u32 mask) {
    if (BoxState* st = boxOf(ui, box)) { setStandard(ui, box, *st, mask); relayout(ui, box, *st); }
}
NodeHandle dialogButtonBoxAdd(UiContext& ui, NodeHandle box, std::string_view text, ButtonRole role) {
    BoxState* st = boxOf(ui, box);
    if (!st) return {};
    const NodeHandle b = addEntry(ui, box, *st, text, role, 0);
    relayout(ui, box, *st);
    return b;
}
void dialogButtonBoxRemove(UiContext& ui, NodeHandle box, NodeHandle button) {
    BoxState* st = boxOf(ui, box);
    if (!st) return;
    const auto it = std::find_if(st->items.begin(), st->items.end(), [&](const Entry& e) { return e.btn == button; });
    if (it == st->items.end()) return;
    ui.destroy(it->btn);
    st->items.erase(it);
    relayout(ui, box, *st);
}
void setDialogButtonBoxOptions(UiContext& ui, NodeHandle box, const DialogButtonBoxOptions& o) {
    BoxState* st = boxOf(ui, box);
    if (!st) return;
    const bool relabel = resolve(st->o.layout) != resolve(o.layout);
    st->o = o;
    if (relabel)
        for (const Entry& e : st->items) if (e.std == StdButton::Discard) ui.setText(e.btn, standardButtonText(e.std, o.layout));
    relayout(ui, box, *st);
}
NodeHandle dialogButtonBoxButton(const UiContext& ui, NodeHandle box, u32 standardButton) {
    if (const BoxState* st = boxOf(ui, box))
        for (const Entry& e : st->items) if (e.std == standardButton) return e.btn;
    return {};
}
u32 dialogButtonBoxStandardOf(const UiContext& ui, NodeHandle box, NodeHandle button) {
    if (const BoxState* st = boxOf(ui, box))
        for (const Entry& e : st->items) if (e.btn == button) return e.std;
    return 0;
}
ButtonRole dialogButtonBoxRole(const UiContext& ui, NodeHandle box, NodeHandle button) {
    if (const BoxState* st = boxOf(ui, box))
        for (const Entry& e : st->items) if (e.btn == button) return e.role;
    return ButtonRole::Action;
}
std::vector<NodeHandle> dialogButtonBoxButtons(const UiContext& ui, NodeHandle box) {
    std::vector<NodeHandle> v;
    if (!boxOf(ui, box)) return v;
    for (NodeHandle c = ui.firstChild(box); ui.alive(c); c = ui.nextSibling(c))
        if (!ui.hasClass(c, "buttonbox-stretch")) v.push_back(c);
    return v;
}
NodeHandle dialogButtonBoxRejectButton(const UiContext& ui, NodeHandle box) {
    const BoxState* st = boxOf(ui, box);
    return st && ui.alive(st->rej) ? st->rej : NodeHandle{};
}
NodeHandle dialogButtonBoxDefault(const UiContext& ui, NodeHandle box) {
    const BoxState* st = boxOf(ui, box);
    return st && ui.alive(st->def) ? st->def : NodeHandle{};
}

} // namespace fuse::ui::widgets
