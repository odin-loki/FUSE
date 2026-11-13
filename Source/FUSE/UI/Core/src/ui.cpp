#include "fuse/ui/ui.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <mutex>

namespace fuse::ui {

namespace {
constexpr u32 kNone = 0xFFFFFFFFu;
constexpr f32 kInf = std::numeric_limits<f32>::infinity();

enum NodeFlags : u32 {
    kFlagFocusable = 1u << 0,
    kFlagFocusScope = 1u << 2,
    kFlagRoleSet = 1u << 3,
    kFlagCapturesDrag = 1u << 4,
    kFlagStyled = 1u << 5,  ///< styled at least once (a first style never transitions)
    kFlagExiting = 1u << 6, ///< hidden, still painted by its exit animation: not hit, not focusable
    kFlagFx = 1u << 7,        ///< the style has effects (ComputedStyle::hasEffects): painted / hit through paintEffects
    kFlagZKids = 1u << 8,     ///< a child is positioned or has a z-index: children are painted and hit in z order
    kFlagOutOfFlow = 1u << 9, ///< position: absolute / fixed (listed in m_outOfFlow)
    kFlagContainer = 1u << 10, ///< a query container (listed in m_containers)
};
inline bool outOfFlow(const ComputedStyle& s) { return s.position == PositionKind::Absolute || s.position == PositionKind::Fixed; }
enum Dirty : u8 { kDirtyStyle = 1, kDirtyLayout = 2, kDirtyPaint = 4, kDirtyText = 8, kDirtyMeasure = 16 };

/// uids of the contexts that exist (UiContext::exists). Never destroyed: contexts with static storage duration
/// unregister during static destruction, in any order.
struct LiveContexts {
    std::mutex mutex;
    std::vector<u64> uids;
};
LiveContexts& liveContexts() {
    static LiveContexts* const live = new LiveContexts;
    return *live;
}
} // namespace

const char* a11yRoleName(A11yRole r) {
    static const char* names[] = {"none", "window", "group", "text", "button", "checkbox", "radio", "switch",
                                  "slider", "progressbar", "textfield", "scrollarea", "list", "listitem", "image",
                                  "separator", "link", "combobox", "spinbox", "dial", "scrollbar", "tab",
                                  "menu", "menuitem", "menubar", "dialog", "tree", "treeitem", "table", "row", "cell",
                                  "columnheader", "tablist", "tabpanel", "toolbar", "statusbar", "tooltip", "alert"};
    return u32(r) < std::size(names) ? names[u32(r)] : "unknown";
}
const char* a11yActionName(A11yAction a) {
    static const char* names[] = {"focus", "click", "increment", "decrement", "setvalue", "expand", "collapse", "select",
                                  "scrollup", "scrolldown", "scrollleft", "scrollright", "scrollintoview"};
    return u32(a) < std::size(names) ? names[u32(a)] : "unknown";
}

struct UiContext::Node {
    u32 gen = 1;
    bool alive = false;
    u32 parent = kNone, first = kNone, last = kNone, next = kNone, prev = kNone;
    u32 cls = 0;
    u32 idHash = 0;
    std::array<u32, 6> classes{};
    u8 classCount = 0;
    u8 dirty = 0;
    u32 state = 0;
    u32 flags = 0;
    u32 style = 0;       ///< computed style; a scratch slot of the style table while the node's style is animated
    u32 targetStyle = 0; ///< the cascade's result (== style when nothing animates)
    Rect rect, prevRect;
    Size measured{-1.f, -1.f}, measureKey{-2.f, -2.f};
    Vec2 scroll;
    Size extent;
    f32 value = 0.f, lo = 0.f, hi = 1.f, step = 0.f;
    i32 tabIndex = 0;
    A11yRole role = A11yRole::None;
    f32 animOpacity = 1.f;
    Vec2 offset;
    u64 user = 0;
};

struct UiContext::Cold {
    std::string id;
    std::string text;
    std::string a11yName;
    A11yLive a11yLive = A11yLive::Off;
    std::vector<std::pair<Prop, PropValue>> inl;
    std::vector<std::pair<u32, std::string>> attrs; ///< hashId(name) -> value (attribute selectors)
    fuse::text::TextLayout layout;
    f32 layoutWidth = -1.f, layoutSize = -1.f;
    std::unique_ptr<fuse::text::TextEditBuffer> edit;
};

// ---- Registry ------------------------------------------------------------------------------------------------------
u32 WidgetRegistry::add(WidgetClass c) {
    c.nameHash = hashId(c.name);
    for (u32 i = 0; i < m_classes.size(); ++i)
        if (m_classes[i].nameHash == c.nameHash) {
            m_classes[i] = std::move(c);
            return i;
        }
    m_classes.push_back(std::move(c));
    return u32(m_classes.size() - 1);
}
u32 WidgetRegistry::find(std::string_view name) const {
    const u32 h = hashId(name);
    for (u32 i = 0; i < m_classes.size(); ++i)
        if (m_classes[i].nameHash == h) return i;
    return kNone;
}

void registerCoreClasses(WidgetRegistry& r) {
    WidgetClass root;
    root.name = "root";
    root.role = A11yRole::Window;
    r.add(root);
    WidgetClass panel;
    panel.name = "panel";
    panel.role = A11yRole::None;
    r.add(panel);
    WidgetClass label;
    label.name = "label";
    label.role = A11yRole::Text;
    r.add(label);
    // Overlay layer (Tier B popups, menus, dialogs, tooltips): laid out over the whole viewport with Anchor layout,
    // ignored by its parent's measure and flow, transparent to the pointer itself. Keep it the root's last child.
    WidgetClass layer;
    layer.name = "layer";
    layer.role = A11yRole::None;
    layer.defaults.layout = LayoutKind::Anchor;
    layer.defaults.pointerEvents = false;
    r.add(layer);
}

// ---- Style subject -------------------------------------------------------------------------------------------------
struct ContextSubject final : StyleSubject {
    const UiContext& ui;
    const std::vector<UiContext::Node>& nodes;
    ContextSubject(const UiContext& u, const std::vector<UiContext::Node>& n) : ui(u), nodes(n) {}
    u32 typeHash(u32 n) const override { return ui.registry().get(nodes[n].cls).nameHash; }
    u32 idHash(u32 n) const override { return nodes[n].idHash; }
    bool hasClass(u32 n, u32 h) const override {
        for (u8 k = 0; k < nodes[n].classCount; ++k)
            if (nodes[n].classes[k] == h) return true;
        return false;
    }
    u32 states(u32 n) const override { return nodes[n].state; }
    u32 parent(u32 n) const override { return nodes[n].parent; }
    u32 prevSibling(u32 n) const override { return nodes[n].prev; }
    u32 nextSibling(u32 n) const override { return nodes[n].next; }
    u32 firstChild(u32 n) const override { return nodes[n].first; }
    bool empty(u32 n) const override { return nodes[n].first == kNone && ui.m_cold[n]->text.empty(); }
    bool attribute(u32 n, u32 nameHash, std::string_view& out) const override {
        for (const auto& [h, v] : ui.m_cold[n]->attrs)
            if (h == nameHash) { out = v; return true; }
        static constexpr u32 kId = hashId("id");
        if (nameHash == kId && !ui.m_cold[n]->id.empty()) { out = ui.m_cold[n]->id; return true; }
        return false;
    }
    bool containerSize(u32 n, u32 name, bool needHeight, Size& out) const override {
        for (u32 a = nodes[n].parent; a != kNone; a = nodes[a].parent) {
            if (!(nodes[a].flags & kFlagContainer)) continue;
            const ComputedStyle& s = ui.m_styles.style(nodes[a].targetStyle);
            if (!s.containerType || (name && s.containerName != name) || (needHeight && s.containerType != 2)) continue;
            out = {std::max(0.f, nodes[a].rect.w - s.padding.h()), std::max(0.f, nodes[a].rect.h - s.padding.v())};
            return true;
        }
        return false;
    }
};

// ---- Lifetime ------------------------------------------------------------------------------------------------------
UiContext::UiContext(const UiContextDesc& d) : m_font(d.font), m_atlas(d.atlasSize), m_viewport(d.viewport) {
    m_nodes.reserve(d.reserveNodes);
    m_cold.reserve(d.reserveNodes);
    m_draw.reserve(std::size_t(d.reserveNodes) * 4);
    m_path.reserve(256);
    m_scratchOrder.reserve(256);
    m_scratchTab.reserve(256);
    m_shapeScratch.reserve(256);
    m_connections.reserve(256);
    m_timers.reserve(64);
    m_anims.reserve(128);
    m_styleAnims.reserve(64);
    m_propAnims.reserve(64);
    m_dnd.reserve(32);
    m_queue = std::make_unique<Command[]>(kQueueSize);
    for (u32 i = 0; i < kQueueSize; ++i) m_queue[i].seq.store(i, std::memory_order_relaxed);
    m_families.reserve(8);
    {
        FamilySlot def; // the default family: the context's font as its regular face
        def.owned = std::make_unique<fuse::text::FontFamily>();
        if (m_font) def.owned->addExternal(m_font, fuse::text::kWeightNormal, false);
        def.family = def.owned.get();
        m_families.push_back(std::move(def));
    }
    m_keyAnims.reserve(16);
    m_zScratch.reserve(64);
    syncMedia();
    registerCoreClasses(m_registry);
    m_root = create(m_registry.find("root"), {}, "root");
    m_layerClass = m_registry.find("layer");
    LiveContexts& live = liveContexts();
    const std::lock_guard lock(live.mutex);
    live.uids.push_back(m_uid);
}

UiContext::~UiContext() {
    LiveContexts& live = liveContexts();
    const std::lock_guard lock(live.mutex);
    std::erase(live.uids, m_uid);
}

bool UiContext::exists(u64 uid) {
    LiveContexts& live = liveContexts();
    const std::lock_guard lock(live.mutex);
    return std::find(live.uids.begin(), live.uids.end(), uid) != live.uids.end();
}

UiContext::Node* UiContext::node(NodeHandle h) {
    if (h.index >= m_nodes.size()) return nullptr;
    Node& n = m_nodes[h.index];
    return (n.alive && n.gen == h.generation) ? &n : nullptr;
}
const UiContext::Node* UiContext::node(NodeHandle h) const {
    if (h.index >= m_nodes.size()) return nullptr;
    const Node& n = m_nodes[h.index];
    return (n.alive && n.gen == h.generation) ? &n : nullptr;
}
bool UiContext::alive(NodeHandle n) const { return node(n) != nullptr; }
NodeHandle UiContext::handleOf(u32 i) const {
    if (i >= m_nodes.size() || !m_nodes[i].alive) return {};
    return {i, m_nodes[i].gen};
}

u32 UiContext::alloc() {
    u32 i;
    if (!m_free.empty()) {
        i = m_free.back();
        m_free.pop_back();
        const u32 g = m_nodes[i].gen + 1;
        m_nodes[i] = Node{};
        m_nodes[i].gen = g;
        Cold& c = *m_cold[i];
        c.id.clear();
        c.text.clear();
        c.a11yName.clear();
        c.inl.clear();
        c.attrs.clear();
        c.layoutWidth = c.layoutSize = -1.f;
        c.edit.reset();
    } else {
        i = u32(m_nodes.size());
        m_nodes.emplace_back();
        m_cold.push_back(std::make_unique<Cold>());
    }
    m_nodes[i].alive = true;
    ++m_live;
    return i;
}

void UiContext::linkBefore(u32 i, u32 p, u32 before) {
    Node& n = m_nodes[i];
    Node& pn = m_nodes[p];
    n.parent = p;
    if (before == kNone) {
        n.prev = pn.last;
        n.next = kNone;
        if (pn.last != kNone) m_nodes[pn.last].next = i;
        else pn.first = i;
        pn.last = i;
    } else {
        Node& b = m_nodes[before];
        n.next = before;
        n.prev = b.prev;
        if (b.prev != kNone) m_nodes[b.prev].next = i;
        else pn.first = i;
        b.prev = i;
    }
}

void UiContext::unlink(u32 i) {
    Node& n = m_nodes[i];
    if (n.parent == kNone) return;
    Node& p = m_nodes[n.parent];
    if (n.prev != kNone) m_nodes[n.prev].next = n.next;
    else p.first = n.next;
    if (n.next != kNone) m_nodes[n.next].prev = n.prev;
    else p.last = n.prev;
    n.parent = n.prev = n.next = kNone;
}

NodeHandle UiContext::create(u32 cls, NodeHandle parent, std::string_view id) {
    if (cls >= m_registry.size()) return {};
    if (parent.valid() && !node(parent)) return {};
    const u32 i = alloc();
    Node& n = m_nodes[i];
    n.cls = cls;
    const WidgetClass& wc = m_registry.get(cls);
    n.role = wc.role;
    if (wc.focusable) n.flags |= kFlagFocusable;
    if (!id.empty()) {
        n.idHash = hashId(id);
        m_cold[i]->id.assign(id);
    }
    n.dirty = kDirtyStyle | kDirtyLayout | kDirtyPaint | kDirtyText | kDirtyMeasure;
    if (parent.valid()) {
        linkBefore(i, parent.index, kNone);
        markLayout(parent.index);
        if (m_styles.features()) structureChanged(parent.index, i);
    }
    m_anyStyleDirty = m_anyLayoutDirty = m_anyPaintDirty = true;
    return {i, m_nodes[i].gen};
}

std::expected<NodeHandle, UiError> UiContext::create(std::string_view className, NodeHandle parent, std::string_view id) {
    const u32 c = m_registry.find(className);
    if (c == kNone) return std::unexpected(UiError::UnknownClass);
    const NodeHandle h = create(c, parent, id);
    if (!h.valid()) return std::unexpected(UiError::InvalidHandle);
    return h;
}

void UiContext::destroyRec(u32 i) {
    for (u32 c = m_nodes[i].first; c != kNone;) {
        const u32 nx = m_nodes[c].next;
        destroyRec(c);
        c = nx;
    }
    Node& n = m_nodes[i];
    m_damage = m_damage.unite(n.rect);
    if (m_hover.index == i) m_hover = {};
    if (m_pressed.index == i) m_pressed = {};
    if (m_focus.index == i) m_focus = {};
    if (m_modal.index == i) m_modal = {};
    for (auto& a : m_anims)
        if (a.active && a.node.index == i) {
            a.active = false;
            --m_animCount;
        }
    if (n.style != n.targetStyle)
        for (auto& a : m_styleAnims)
            if (a.active && a.node.index == i) {
                m_styles.releaseScratch(a.slot);
                a.active = false;
                --m_styleAnimCount;
                if (a.busy) --m_styleAnimBusy;
                a.busy = false;
            }
    if (m_propAnimCount)
        for (auto& a : m_propAnims)
            if (a.active && a.node.index == i) {
                a.active = false;
                --m_propAnimCount;
            }
    if (m_keyAnimCount)
        for (auto& ka : m_keyAnims)
            if (ka.active && ka.node.index == i) endKeyAnim(ka);
    if (n.flags & (kFlagFx | kFlagOutOfFlow | kFlagContainer)) {
        if (n.flags & kFlagFx) {
            --m_fxCount;
            for (std::size_t k = 0; k < m_fxBounds.size(); ++k)
                if (m_fxBounds[k].first == i) { // what it covered has to be repainted
                    m_fxPendingDamage = m_fxDepth || m_fxCount ? Rect{0.f, 0.f, m_viewport.w, m_viewport.h} : m_fxPendingDamage.unite(m_fxBounds[k].second);
                    m_fxBounds[k] = m_fxBounds.back();
                    m_fxBounds.pop_back();
                    break;
                }
        }
        if (n.flags & kFlagOutOfFlow) std::erase(m_outOfFlow, i);
        if (n.flags & kFlagContainer) std::erase_if(m_containers, [&](const ContainerEntry& c) { return c.node == i; });
        std::erase(m_fixedOrder, i);
    }
    n.alive = false;
    n.first = n.last = n.parent = n.next = n.prev = kNone;
    m_free.push_back(i);
    --m_live;
}

void UiContext::destroy(NodeHandle h) {
    Node* n = node(h);
    if (!n || h == m_root) return;
    const u32 p = n->parent;
    unlink(h.index);
    destroyRec(h.index);
    if (p != kNone) markLayout(p);
    if (p != kNone && m_styles.features()) structureChanged(p, kNone);
    m_anyPaintDirty = true;
}

void UiContext::moveTo(NodeHandle h, NodeHandle np, NodeHandle before) {
    Node* n = node(h);
    if (!n || !node(np) || h == m_root) return;
    for (u32 a = np.index; a != kNone; a = m_nodes[a].parent)
        if (a == h.index) return; // refuse cycles
    const u32 old = n->parent;
    const u32 want = node(before) ? before.index : kNone;
    if (old == np.index && n->next == want) return;
    unlink(h.index);
    linkBefore(h.index, np.index, want);
    if (old != kNone) markLayout(old);
    markLayout(np.index);
    markStyle(h.index);
    if (const ComputedStyle& ms = m_styles.style(m_nodes[h.index].style); ms.position != PositionKind::Static || ms.zIndex != kZAuto)
        m_nodes[np.index].flags |= kFlagZKids; // the new parent paints its children in z order from now on
    if (const u32 feat = m_styles.features()) {
        if (old != kNone && old != np.index) structureChanged(old, kNone);
        structureChanged(np.index, h.index);
        if (feat & kStyleDescend) markStyleSubtree(h.index, 255); // its descendants now sit under other ancestors
    }
}

NodeHandle UiContext::parent(NodeHandle h) const {
    const Node* n = node(h);
    return n ? handleOf(n->parent) : NodeHandle{};
}
NodeHandle UiContext::firstChild(NodeHandle h) const {
    const Node* n = node(h);
    return n ? handleOf(n->first) : NodeHandle{};
}
NodeHandle UiContext::nextSibling(NodeHandle h) const {
    const Node* n = node(h);
    return n ? handleOf(n->next) : NodeHandle{};
}
u32 UiContext::childCount(NodeHandle h) const {
    const Node* n = node(h);
    u32 c = 0;
    if (n)
        for (u32 k = n->first; k != kNone; k = m_nodes[k].next) ++c;
    return c;
}
NodeHandle UiContext::find(std::string_view id) const {
    const u32 h = hashId(id);
    for (u32 i = 0; i < m_nodes.size(); ++i)
        if (m_nodes[i].alive && m_nodes[i].idHash == h && m_cold[i]->id == id) return {i, m_nodes[i].gen};
    return {};
}
std::string_view UiContext::idOf(NodeHandle h) const {
    return node(h) ? std::string_view(m_cold[h.index]->id) : std::string_view{};
}
u32 UiContext::classOf(NodeHandle h) const { return node(h) ? node(h)->cls : kNone; }

// ---- Dirty tracking ------------------------------------------------------------------------------------------------
void UiContext::markLayout(u32 i) {
    m_anyLayoutDirty = m_anyPaintDirty = true;
    while (i != kNone) {
        Node& n = m_nodes[i];
        n.measureKey = {-2.f, -2.f};
        if (n.dirty & kDirtyMeasure) return; // ancestors already invalidated
        n.dirty |= kDirtyLayout | kDirtyPaint | kDirtyMeasure;
        const ComputedStyle& s = m_styles.style(n.style);
        // layout boundary: a fixed-size node's changes never propagate up (its parent re-arranges nothing)
        if (s.width.unit == Unit::Px && s.height.unit == Unit::Px && n.parent != kNone) {
            // ancestors still need to walk down to it: mark them "path" dirty without measure invalidation
            for (u32 a = n.parent; a != kNone; a = m_nodes[a].parent) {
                if (m_nodes[a].dirty & kDirtyLayout) break;
                m_nodes[a].dirty |= kDirtyLayout;
            }
            return;
        }
        i = n.parent;
    }
}
void UiContext::markStyle(u32 i) {
    m_nodes[i].dirty |= kDirtyStyle;
    m_anyStyleDirty = true;
}
void UiContext::markPaint(u32 i) {
    m_nodes[i].dirty |= kDirtyPaint;
    m_anyPaintDirty = true;
}
void UiContext::markAllDirty() {
    for (auto& n : m_nodes)
        if (n.alive) {
            n.dirty |= kDirtyStyle | kDirtyLayout | kDirtyPaint;
            n.measureKey = {-2.f, -2.f};
        }
    m_anyStyleDirty = m_anyLayoutDirty = m_anyPaintDirty = true;
}

// ---- Properties ----------------------------------------------------------------------------------------------------
void UiContext::addClass(NodeHandle h, std::string_view cls) {
    Node* n = node(h);
    if (!n) return;
    const u32 c = hashId(cls);
    for (u8 k = 0; k < n->classCount; ++k)
        if (n->classes[k] == c) return;
    if (n->classCount < n->classes.size()) {
        n->classes[n->classCount++] = c;
        markStyle(h.index);
        for (u32 k = n->first; k != kNone; k = m_nodes[k].next) markStyle(k);
        if (m_styles.features()) invalidateStyleFor(h.index, m_styles.classInvalidation(c));
    }
}
void UiContext::removeClass(NodeHandle h, std::string_view cls) {
    Node* n = node(h);
    if (!n) return;
    const u32 c = hashId(cls);
    for (u8 k = 0; k < n->classCount; ++k)
        if (n->classes[k] == c) {
            n->classes[k] = n->classes[--n->classCount];
            markStyle(h.index);
            for (u32 j = n->first; j != kNone; j = m_nodes[j].next) markStyle(j);
            if (m_styles.features()) invalidateStyleFor(h.index, m_styles.classInvalidation(c));
            return;
        }
}
bool UiContext::hasClass(NodeHandle h, std::string_view cls) const {
    const Node* n = node(h);
    if (!n) return false;
    const u32 c = hashId(cls);
    for (u8 k = 0; k < n->classCount; ++k)
        if (n->classes[k] == c) return true;
    return false;
}
void UiContext::set(NodeHandle h, Prop p, const PropValue& v) {
    if (!node(h)) return;
    auto& inl = m_cold[h.index]->inl;
    for (auto& kv : inl)
        if (kv.first == p && (p != Prop::Transition || kv.second.e == v.e)) { // one inline transition per target property
            kv.second = v;
            markStyle(h.index);
            return;
        }
    inl.emplace_back(p, v);
    markStyle(h.index);
}
bool UiContext::setCss(NodeHandle h, std::string_view property, std::string_view value) {
    Prop p;
    PropValue v;
    if (findProp(property, p)) {
        if (!parsePropValue(p, value, v)) return false;
        set(h, p, v);
        return true;
    }
    std::string css = "x{" + std::string(property) + ":" + std::string(value) + "}"; // shorthands
    auto sheet = StyleSheet::parse(css);
    if (!sheet || sheet->rules().empty() || sheet->rules()[0].decls.empty()) return false;
    for (const auto& d : sheet->rules()[0].decls) set(h, d.prop, d.value);
    return true;
}
void UiContext::clearInline(NodeHandle h) {
    if (!node(h)) return;
    m_cold[h.index]->inl.clear();
    markStyle(h.index);
}
void UiContext::setText(NodeHandle h, std::string_view s) {
    if (!node(h)) return;
    Cold& c = *m_cold[h.index];
    if (c.text == s) return;
    if ((m_styles.features() & kStyleEmptyText) && c.text.empty() != s.empty()) { // :empty looks at the text too
        markStyle(h.index);
        StyleInvalidation inv;
        inv.has = (m_styles.features() & kStyleHas) != 0;
        inv.descend = (m_styles.features() & kStyleStructDeep) ? 255 : 0;
        inv.siblings = (m_styles.features() & kStyleSibling) != 0;
        invalidateStyleFor(h.index, inv);
    }
    c.text.assign(s);
    c.layoutWidth = -1.f;
    m_nodes[h.index].dirty |= kDirtyText;
    markLayout(h.index);
}
std::string_view UiContext::text(NodeHandle h) const {
    return node(h) ? std::string_view(m_cold[h.index]->text) : std::string_view{};
}
void UiContext::setValue(NodeHandle h, f32 v, bool emitSignal) {
    Node* n = node(h);
    if (!n) return;
    if (n->step > 0.f) v = n->lo + std::round((v - n->lo) / n->step) * n->step;
    if (n->hi > n->lo) v = std::clamp(v, n->lo, n->hi);
    if (v == n->value) return;
    n->value = v;
    markPaint(h.index);
    if (emitSignal) emit(h, SignalKind::ValueChanged, {.value = v});
}
f32 UiContext::value(NodeHandle h) const { return node(h) ? node(h)->value : 0.f; }
void UiContext::setRange(NodeHandle h, f32 lo, f32 hi, f32 step) {
    Node* n = node(h);
    if (!n) return;
    n->lo = lo;
    n->hi = hi;
    n->step = step;
    const f32 v = n->value;
    n->value = std::numeric_limits<f32>::quiet_NaN();
    setValue(h, v);
}
f32 UiContext::rangeMin(NodeHandle h) const { return node(h) ? node(h)->lo : 0.f; }
f32 UiContext::rangeMax(NodeHandle h) const { return node(h) ? node(h)->hi : 0.f; }
f32 UiContext::rangeStep(NodeHandle h) const { return node(h) ? node(h)->step : 0.f; }
void UiContext::setState(NodeHandle h, u32 bits, bool on) {
    Node* n = node(h);
    if (!n) return;
    const u32 s = on ? (n->state | bits) : (n->state & ~bits);
    if (s == n->state) return;
    const u32 changed = s ^ n->state;
    n->state = s;
    markStyle(h.index);
    markPaint(h.index);
    for (u32 k = n->first; k != kNone; k = m_nodes[k].next) markStyle(k);
    if (m_styles.features()) invalidateStyleFor(h.index, m_styles.stateInvalidation(changed));
}
u32 UiContext::states(NodeHandle h) const { return node(h) ? node(h)->state : 0u; }
void UiContext::setFocusable(NodeHandle h, bool on) {
    if (Node* n = node(h)) n->flags = on ? (n->flags | kFlagFocusable) : (n->flags & ~u32(kFlagFocusable));
}
void UiContext::setTabIndex(NodeHandle h, i32 i) {
    if (Node* n = node(h)) n->tabIndex = i;
}
void UiContext::setFocusScope(NodeHandle h, bool on) {
    if (Node* n = node(h)) n->flags = on ? (n->flags | kFlagFocusScope) : (n->flags & ~u32(kFlagFocusScope));
}
void UiContext::setCapturesDrag(NodeHandle h, bool on) {
    if (Node* n = node(h)) n->flags = on ? (n->flags | kFlagCapturesDrag) : (n->flags & ~u32(kFlagCapturesDrag));
}
void UiContext::setA11y(NodeHandle h, A11yRole r, std::string_view name) {
    Node* n = node(h);
    if (!n) return;
    n->role = r;
    n->flags |= kFlagRoleSet;
    m_cold[h.index]->a11yName.assign(name);
}
A11yRole UiContext::role(NodeHandle h) const { return node(h) ? node(h)->role : A11yRole::None; }
void UiContext::setA11yLive(NodeHandle h, A11yLive live) {
    if (node(h)) m_cold[h.index]->a11yLive = live;
}
void UiContext::setUserData(NodeHandle h, u64 v) {
    if (Node* n = node(h)) n->user = v;
}
u64 UiContext::userData(NodeHandle h) const { return node(h) ? node(h)->user : 0; }
fuse::text::TextEditBuffer& UiContext::editBuffer(NodeHandle h) {
    Cold& c = *m_cold[h.index];
    if (!c.edit) {
        c.edit = std::make_unique<fuse::text::TextEditBuffer>(256);
        c.edit->setText(c.text);
    }
    return *c.edit;
}
const fuse::text::TextLayout* UiContext::textLayout(NodeHandle h) const {
    return node(h) ? &m_cold[h.index]->layout : nullptr;
}
const ComputedStyle& UiContext::computed(NodeHandle h) const {
    const Node* n = node(h);
    return m_styles.style(n ? n->style : 0);
}
Rect UiContext::rect(NodeHandle h) const { return node(h) ? node(h)->rect : Rect{}; }
Rect UiContext::contentRect(NodeHandle h) const {
    const Node* n = node(h);
    if (!n) return {};
    const auto& p = m_styles.style(n->style).padding;
    return {n->rect.x + p.l, n->rect.y + p.t, std::max(0.f, n->rect.w - p.h()), std::max(0.f, n->rect.h - p.v())};
}
Vec2 UiContext::scroll(NodeHandle h) const { return node(h) ? node(h)->scroll : Vec2{}; }
Size UiContext::scrollExtent(NodeHandle h) const { return node(h) ? node(h)->extent : Size{}; }
void UiContext::scrollTo(NodeHandle h, Vec2 o) {
    Node* n = node(h);
    if (!n) return;
    const Rect c = contentRect(h);
    o.x = std::clamp(o.x, 0.f, std::max(0.f, n->extent.w - c.w));
    o.y = std::clamp(o.y, 0.f, std::max(0.f, n->extent.h - c.h));
    if (o == n->scroll) return;
    n->scroll = o;
    m_anyLayoutDirty = m_anyPaintDirty = true;
    for (u32 a = h.index; a != kNone; a = m_nodes[a].parent) {
        if (m_nodes[a].dirty & kDirtyLayout) break;
        m_nodes[a].dirty |= kDirtyLayout; // re-arrange path; measure caches stay valid
    }
}
void UiContext::setViewport(Size s) {
    if (s == m_viewport) return;
    m_viewport = s;
    markLayout(m_root.index);
    syncMedia();
}

// ---- Style pass ----------------------------------------------------------------------------------------------------
void UiContext::applyStyleDiff(u32 i, const ComputedStyle& before, const ComputedStyle& after) {
    Node& n = m_nodes[i];
    ComputedStyle la = before;
    const ComputedStyle& lb = after;
    const bool fontChanged = la.fontSize != lb.fontSize || la.textAlign != lb.textAlign || la.rtl != lb.rtl ||
                             la.fontWeight != lb.fontWeight || la.fontItalic != lb.fontItalic || la.fontFamily != lb.fontFamily ||
                             la.letterSpacing != lb.letterSpacing || la.lineHeight != lb.lineHeight;
    markPaint(i);
    // compare layout-relevant fields only
    la.background = lb.background; la.background2 = lb.background2; la.borderColor = lb.borderColor;
    la.color = lb.color; la.shadowColor = lb.shadowColor; la.opacity = lb.opacity; la.radius = lb.radius;
    la.gradient = lb.gradient; la.shadowBlur = lb.shadowBlur; la.shadowX = lb.shadowX; la.shadowY = lb.shadowY; la.backdropBlur = lb.backdropBlur;
    la.pointerEvents = lb.pointerEvents; la.borderWidth = lb.borderWidth; la.textAlign = lb.textAlign;
    la.textDecoration = lb.textDecoration;
    la.transitions = lb.transitions; la.transitionCount = lb.transitionCount; la.enterAnim = lb.enterAnim; la.exitAnim = lb.exitAnim;
    la.enterDuration = lb.enterDuration; la.exitDuration = lb.exitDuration;
    la.zIndex = lb.zIndex; la.transform = lb.transform; la.originX = lb.originX; la.originY = lb.originY; la.filter = lb.filter;
    la.clipPath = lb.clipPath; la.mask = lb.mask; la.maskLuminance = lb.maskLuminance; la.backgroundImage = lb.backgroundImage;
    la.animations = lb.animations; la.animationCount = lb.animationCount; la.containerType = lb.containerType;
    la.containerName = lb.containerName;
    syncStyleFlags(i, lb);
    if ((before.containerType != lb.containerType || before.containerName != lb.containerName) && m_styles.usesContainerQueries())
        markStyleSubtree(i, 255); // what its queries answer changed (the style pass reaches the subtree after this node)
    if (fontChanged) {
        m_cold[i]->layoutWidth = -1.f;
        n.dirty |= kDirtyText;
    }
    if (!(la == lb) || fontChanged) markLayout(i);
}

void UiContext::styleRec(u32 i, bool parentChanged) {
    Node& n = m_nodes[i];
    bool changed = false;
    if (parentChanged || (n.dirty & kDirtyStyle)) {
        const ContextSubject subj(*this, m_nodes);
        // inheritance follows the cascade's result, not a parent's in-flight transition values
        const ComputedStyle* ps = n.parent != kNone ? &m_styles.style(m_nodes[n.parent].targetStyle) : nullptr;
        const WidgetClass& wc = m_registry.get(n.cls);
        const u32 before = n.targetStyle;
        const u32 idx = m_styles.compute(subj, i, ps, &wc.defaults, m_cold[i]->inl);
        ++m_stats.styled;
        n.dirty &= u8(~kDirtyStyle);
        const bool first = !(n.flags & kFlagStyled);
        n.flags |= kFlagStyled;
        if (idx != before) {
            changed = true;
            // copies: starting an animation may grow the style table
            const ComputedStyle cur = m_styles.style(n.style); // what is on screen now
            const ComputedStyle next = m_styles.style(idx);
            const bool wasVisible = !first && m_styles.style(before).visible;
            n.targetStyle = idx;
            if (next.animationCount || m_keyAnimCount) syncCssAnimations(i, next);
            const bool motion = !first && !m_reducedMotion && !m_sheetSwap;
            bool trans = false;
            if (motion && next.transitionCount && next.visible && cur.visible)
                for (int p = 0; p < kPropCount && !trans; ++p) {
                    if (!propInfo(Prop(p)).animatable) continue;
                    const Transition* tr = findTransition(next, Prop(p));
                    trans = tr && tr->duration > 0.f && !sameStyleProp(cur, next, Prop(p));
                }
            const bool enter = next.visible && (first || !wasVisible) && next.enterAnim != MotionKind::None &&
                               next.enterDuration > 0.f && !m_reducedMotion;
            const bool exit = motion && cur.visible && !next.visible && next.exitAnim != MotionKind::None &&
                              next.exitDuration > 0.f && !n.rect.empty();
            if (n.style != before || trans || exit) { // already animated, or starting now
                StyleAnim& a = ensureStyleAnim(i);
                if (trans) {
                    a.from = cur;
                    a.t = 0.f;
                    a.transitioning = true;
                }
                if (exit && !a.exiting) {
                    for (auto& an : m_anims) // an enter animation still running gives way
                        if (an.active && an.node.index == i && an.prop != AnimProp::Value) { an.active = false; --m_animCount; }
                    a.exiting = true;
                    a.exitKind = next.exitAnim;
                    a.exitT = 0.f;
                    a.exitDur = next.exitDuration;
                    a.exitFromOpacity = n.animOpacity;
                    n.flags |= kFlagExiting;
                } else if (a.exiting && next.visible) { // shown again before the exit finished
                    a.exiting = false;
                    n.flags &= ~u32(kFlagExiting);
                    if (!enter) { // an enter animation continues from where the exit got to
                        n.animOpacity = 1.f;
                        n.offset.y = 0.f;
                        if (n.parent != kNone) markArrange(n.parent);
                    }
                }
                composeAnimated(a);
            } else {
                n.style = idx;
                applyStyleDiff(i, cur, next);
            }
            if (enter) startEnter(i, next.enterAnim, next.enterDuration);
        }
    }
    for (u32 c = n.first; c != kNone; c = m_nodes[c].next) styleRec(c, changed);
}

void UiContext::stylePass() {
    if (m_styles.sheetVersion() != m_seenSheetVersion) {
        m_seenSheetVersion = m_styles.sheetVersion();
        for (auto& n : m_nodes)
            if (n.alive) n.dirty |= kDirtyStyle | kDirtyPaint; // paint too: widget paint code reads theme tokens (themeColor)
        m_anyPaintDirty = true;
        m_sheetSwap = true; // a theme swap is not a state change: nothing transitions
    }
    styleRec(m_root.index, false);
    m_sheetSwap = false;
    m_anyStyleDirty = false;
}

// ---- Text ----------------------------------------------------------------------------------------------------------
void UiContext::ensureText(u32 i, f32 width) {
    Cold& c = *m_cold[i];
    const ComputedStyle& s = m_styles.style(m_nodes[i].style);
    if (!m_font) return;
    if (c.layoutWidth == width && c.layoutSize == s.fontSize && !(m_nodes[i].dirty & kDirtyText)) return;
    fuse::text::TextLayoutDesc d;
    d.pixelSize = s.fontSize;
    d.maxWidth = std::isfinite(width) ? width : 0.f;
    d.align = fuse::text::TextAlign(u8(s.textAlign));
    d.letterSpacing = s.letterSpacing;
    d.lineHeight = s.lineHeightPx();
    if (s.rtl && s.textAlign != TextAlignment::Center) // start / end follow the direction
        d.align = fuse::text::TextAlign(u8(s.textAlign == TextAlignment::Start ? TextAlignment::End : TextAlignment::Start));
    fuse::text::layoutText(*fontFor(s), fuse::text::defaultShaper(), c.text, d, c.layout);
    c.layoutWidth = width;
    c.layoutSize = s.fontSize;
    m_nodes[i].dirty &= u8(~kDirtyText);
}

f32 UiContext::measureString(std::string_view s, f32 px) {
    if (!m_font) return 0.f;
    const f32 w = fuse::text::measureText(m_textScope.font ? *m_textScope.font : *m_font, s, px, m_shapeScratch);
    if (m_textScope.letterSpacing == 0.f) return w;
    f32 extra = 0.f;
    for (const auto& g : m_shapeScratch)
        if (g.advance > 0.f) extra += m_textScope.letterSpacing;
    return w + extra;
}
f32 UiContext::measureString(const fuse::text::Font& font, std::string_view s, f32 px) {
    return fuse::text::measureText(font, s, px, m_shapeScratch);
}

// ---- Fonts ---------------------------------------------------------------------------------------------------------
namespace {
// Generic CSS families: well-known installed families tried in order.
constexpr std::string_view kMonoFamilies[] = {"DejaVu Sans Mono", "Liberation Mono", "Noto Sans Mono", "Menlo", "SF Mono",
                                              "Monaco", "Consolas", "Cascadia Mono", "Courier New", "Roboto Mono",
                                              "Droid Sans Mono", "Cousine", "Ubuntu Mono", "FreeMono"};
constexpr std::string_view kSerifFamilies[] = {"Noto Serif", "DejaVu Serif", "Liberation Serif", "Times New Roman",
                                               "Georgia", "Times", "FreeSerif"};
} // namespace

void UiContext::addFontFamily(fuse::text::FontFamily& family) {
    const u32 h = fuse::text::familyHash(family.name());
    bool replaced = false;
    for (std::size_t k = 1; k < m_families.size() && !replaced; ++k)
        if (m_families[k].hash == h) {
            m_families[k].family = &family;
            m_families[k].owned.reset();
            replaced = true;
        }
    if (!replaced) {
        FamilySlot slot;
        slot.hash = h;
        slot.family = &family;
        m_families.push_back(std::move(slot));
    }
    for (auto& n : m_nodes) // styles that named this family resolve differently now
        if (n.alive) n.dirty |= kDirtyStyle | kDirtyText;
    for (auto& c : m_cold)
        if (c) c->layoutWidth = -1.f;
    m_anyStyleDirty = m_anyLayoutDirty = m_anyPaintDirty = true;
    if (m_root.index < m_nodes.size()) markLayout(m_root.index);
}

fuse::text::FontFamily* UiContext::familyByHash(u32 hash) {
    using namespace fuse::text;
    for (std::size_t k = 1; k < m_families.size(); ++k)
        if (m_families[k].hash == hash) return m_families[k].family;
    static const u32 kSans = familyHash("sans-serif"), kSystemUi = familyHash("system-ui"), kMono = familyHash("monospace"),
                     kSerif = familyHash("serif"), kSans2 = familyHash("sans serif"), kSystemUi2 = familyHash("system ui");
    if (hash == kSans || hash == kSystemUi || hash == kSans2 || hash == kSystemUi2) return m_families[0].family;
    // the context font's own family name is the default family, installed or not
    if (m_font && !m_font->familyName().empty() && hash == familyHash(m_font->familyName())) return m_families[0].family;
    FamilySlot slot;
    slot.hash = hash;
    if (m_systemFonts && m_font) {
        std::string name;
        if (hash == kMono || hash == kSerif) {
            for (std::string_view cand : hash == kMono ? std::span<const std::string_view>(kMonoFamilies) : std::span<const std::string_view>(kSerifFamilies))
                if (const SystemFontInfo* f = findSystemFont(cand)) { name = f->family; break; }
            if (name.empty() && hash == kMono)
                for (const SystemFontInfo& f : systemFonts())
                    if (f.monospace) { name = f.family; break; }
        } else if (const SystemFontInfo* f = findSystemFontByHash(hash)) {
            name = f->family;
        }
        if (!name.empty()) {
            if (auto fam = FontFamily::fromSystem(name)) {
                slot.owned = std::make_unique<FontFamily>(std::move(*fam));
                // code points the installed family lacks fall back to the context's font and its chain
                std::vector<const Font*> chain;
                for (std::size_t k = 0; k < m_font->faceCount(); ++k) chain.push_back(&m_font->face(std::uint16_t(k)));
                slot.owned->setFallbacks(std::move(chain));
                slot.family = slot.owned.get();
            }
        }
    }
    m_families.push_back(std::move(slot)); // a miss is remembered too: one lookup per name
    return m_families.back().family;
}

const fuse::text::Font* UiContext::fontFor(std::span<const u32> families, int weight, bool italic) {
    if (!m_font) return nullptr;
    fuse::text::FontFamily* fam = nullptr;
    for (const u32 h : families) {
        if (h == 0) break;
        if ((fam = familyByHash(h))) break;
    }
    if (!fam || fam == m_families[0].family) {
        if (weight == fuse::text::kWeightNormal && !italic) return m_font;
        fam = m_families[0].family;
    }
    const fuse::text::Font* f = fam->resolve(weight, italic);
    return f ? f : m_font;
}
const fuse::text::Font* UiContext::fontFor(const ComputedStyle& s) {
    if (s.fontWeight == 400 && !s.fontItalic && s.fontFamily[0] == 0) return m_font; // the common case
    return fontFor(std::span<const u32>(s.fontFamily), s.fontWeight, s.fontItalic);
}
const fuse::text::Font* UiContext::fontFor(std::string_view family, int weight, bool italic) {
    const u32 h = family.empty() ? 0u : fuse::text::familyHash(family);
    return fontFor(std::span<const u32>(&h, 1), weight, italic);
}
const fuse::text::Font* UiContext::nodeFont(NodeHandle n) {
    return alive(n) ? fontFor(m_styles.style(m_nodes[n.index].style)) : m_font;
}
UiContext::TextScope UiContext::enterTextScope(u32 node) {
    const TextScope prev = m_textScope;
    const ComputedStyle& s = m_styles.style(m_nodes[node].style);
    m_textScope = {fontFor(s), s.letterSpacing};
    return prev;
}

void UiContext::paintTextLayout(DrawList& dl, const fuse::text::TextLayout& tl, Vec2 o, Color col, f32 px, u32 nodeIdx) {
    u8 deco = 0;
    if (nodeIdx < m_nodes.size() && m_nodes[nodeIdx].alive) deco = m_styles.style(m_nodes[nodeIdx].style).textDecoration;
    paintTextLayout(dl, tl, o, col, px, nodeIdx, deco);
}

void UiContext::paintTextLayout(DrawList& dl, const fuse::text::TextLayout& tl, Vec2 o, Color col, f32 px, u32 nodeIdx, u8 deco) {
    if (!m_font || col.a <= 0.f) return;
    const fuse::text::Font& font = tl.font ? *tl.font : *m_font;
    const f32 rs = m_painting ? m_renderScale : 1.f;
    for (const auto& ln : tl.lines) {
        for (u32 k = ln.firstGlyph; k < ln.firstGlyph + ln.glyphCount; ++k) {
            const auto& g = tl.glyphs[k];
            if (g.glyph == fuse::text::kInvisibleGlyph) continue;
            fuse::text::forEachPaintGlyph(font, g, px, [&](const fuse::text::Font& f, std::uint32_t glyph, f32 fpx,
                                                               const fuse::text::ColorLayer* layer) {
                const auto ag = m_atlas.get(f, glyph, fpx * rs);
                if (!ag.valid || ag.w == 0) return;
                DrawCmd c;
                c.op = DrawOp::Glyph;
                if (rs == 1.f) {
                    c.rect = {std::round(o.x + g.x) + f32(ag.x0), std::round(o.y + ln.baseline) + f32(ag.y0), f32(ag.w), f32(ag.h)};
                } else { // device pixels: pen position scaled, then the glyph rasterised at the scaled size
                    c.rect = {std::round((o.x + g.x) * rs) + f32(ag.x0), std::round((o.y + ln.baseline) * rs) + f32(ag.y0), f32(ag.w), f32(ag.h)};
                    c.devicePx = 1;
                }
                c.u = ag.u;
                c.v = ag.v;
                c.tw = ag.w;
                c.th = ag.h;
                c.fill = (!layer || layer->foreground)
                             ? col
                             : Color{f32(layer->r) / 255.f, f32(layer->g) / 255.f, f32(layer->b) / 255.f, f32(layer->a) / 255.f * col.a};
                c.node = nodeIdx;
                dl.push(c);
            });
        }
    }
    if (!deco) return;
    // Underline / strike-through: one rule per line across its advance width, in logical pixels (the list's render
    // scale snaps it to device pixels like any rect).
    const auto dm = font.decorationMetrics(px);
    for (const auto& ln : tl.lines) {
        if (ln.glyphCount == 0 || ln.width <= 0.f) continue;
        const f32 x = std::round(o.x + tl.glyphs[ln.firstGlyph].x), y = std::round(o.y + ln.baseline);
        const f32 w = std::ceil(ln.width);
        auto rule = [&](f32 top, f32 thickness) {
            const std::size_t at = dl.size();
            dl.rect({x, y + top, w, thickness}, col);
            for (std::size_t k = at; k < dl.size(); ++k) dl.cmds()[k].node = nodeIdx;
        };
        if (deco & kTextUnderline) rule(dm.underlineOffset, dm.underlineThickness);
        if (deco & kTextLineThrough) rule(dm.strikeOffset, dm.strikeThickness);
    }
}

void UiContext::paintString(DrawList& dl, std::string_view s, Rect box, Color col, f32 px, TextAlignment align, u32 nodeIdx) {
    if (!m_font || s.empty()) return;
    fuse::text::TextLayoutDesc d;
    d.pixelSize = px;
    d.maxWidth = box.w;
    d.elide = true;
    d.align = fuse::text::TextAlign(u8(align));
    // the node's font when it names one, else the font of the widget callback being run
    const fuse::text::Font* font = m_textScope.font ? m_textScope.font : m_font;
    if (nodeIdx < m_nodes.size() && m_nodes[nodeIdx].alive) {
        const ComputedStyle& ns = m_styles.style(m_nodes[nodeIdx].style);
        font = fontFor(ns);
        d.letterSpacing = ns.letterSpacing;
    }
    fuse::text::layoutText(*font, fuse::text::defaultShaper(), s, d, m_stringScratch);
    const f32 y = box.y + std::floor((box.h - m_stringScratch.height) * 0.5f);
    paintTextLayout(dl, m_stringScratch, {box.x, y}, col, px, nodeIdx);
}

void UiContext::paintString(DrawList& dl, const fuse::text::Font& font, std::string_view s, Rect box, Color col, f32 px,
                            TextAlignment align, u32 nodeIdx, u8 deco) {
    if (!m_font || s.empty()) return;
    fuse::text::TextLayoutDesc d;
    d.pixelSize = px;
    d.maxWidth = box.w;
    d.elide = true;
    d.align = fuse::text::TextAlign(u8(align));
    fuse::text::layoutText(font, fuse::text::defaultShaper(), s, d, m_stringScratch);
    const f32 y = box.y + std::floor((box.h - m_stringScratch.height) * 0.5f);
    paintTextLayout(dl, m_stringScratch, {box.x, y}, col, px, nodeIdx, deco);
}

// ---- Layout --------------------------------------------------------------------------------------------------------
namespace {
f32 clampSize(f32 v, const Length& lo, const Length& hi, f32 base) {
    if (hi.unit == Unit::Px || (hi.unit == Unit::Percent && std::isfinite(base))) v = std::min(v, hi.resolve(base, v));
    if (lo.unit == Unit::Px || (lo.unit == Unit::Percent && std::isfinite(base))) v = std::max(v, lo.resolve(base, v));
    return std::max(0.f, v);
}
bool fixedLen(const Length& l, f32 base) { return l.unit == Unit::Px || (l.unit == Unit::Percent && std::isfinite(base)); }
} // namespace

void UiContext::markArrange(u32 i) {
    m_anyLayoutDirty = m_anyPaintDirty = true;
    for (u32 a = i; a != kNone; a = m_nodes[a].parent) {
        if (m_nodes[a].dirty & kDirtyLayout) break;
        m_nodes[a].dirty |= kDirtyLayout;
    }
}

Size UiContext::measure(u32 i, Size avail) {
    Node& n = m_nodes[i];
    if (n.measureKey == avail && n.measured.w >= 0.f) return n.measured;
    ++m_stats.measured;
    n.dirty &= u8(~kDirtyMeasure);
    const ComputedStyle& s = m_styles.style(n.style);
    Size out{0.f, 0.f};
    if (s.visible) {
        const bool fw = fixedLen(s.width, avail.w), fh = fixedLen(s.height, avail.h);
        const f32 bw = fw ? s.width.resolve(avail.w, 0.f) : avail.w;
        const f32 bh = fh ? s.height.resolve(avail.h, 0.f) : avail.h;
        const Size in{std::max(0.f, bw - s.padding.h()), std::max(0.f, bh - s.padding.v())};
        Size c{0.f, 0.f};
        const WidgetClass& wc = m_registry.get(n.cls);
        if (wc.measure) {
            const TextScope prev = enterTextScope(i);
            c = wc.measure(*this, {i, n.gen}, in);
            m_textScope = prev;
        } else if (n.first != kNone) {
            const bool row = s.direction == Direction::Row;
            u32 count = 0;
            switch (s.layout) {
            case LayoutKind::Box:
                for (u32 k = n.first; k != kNone; k = m_nodes[k].next) {
                    const ComputedStyle& cs = m_styles.style(m_nodes[k].style);
                    if (!cs.visible || m_nodes[k].cls == m_layerClass || outOfFlow(cs)) continue;
                    const Size m = measure(k, {in.w - cs.margin.h(), in.h - cs.margin.v()});
                    const f32 mw = m.w + cs.margin.h(), mh = m.h + cs.margin.v();
                    if (row) { c.w += mw; c.h = std::max(c.h, mh); }
                    else { c.h += mh; c.w = std::max(c.w, mw); }
                    ++count;
                }
                if (count > 1) (row ? c.w : c.h) += s.gap * f32(count - 1);
                break;
            case LayoutKind::Grid: {
                const u32 cols = s.subgrid ? std::clamp<u32>(s.gridColumnSpan, 1, kMaxGridTracks) : std::max<u32>(1, s.gridColumnCount);
                std::array<f32, kMaxGridTracks> colW{};
                f32 rowH = 0.f, total = 0.f;
                u32 col = 0;
                const f32 cw = std::isfinite(in.w) ? (in.w - s.gap * f32(cols - 1)) / f32(cols) : kInf;
                for (u32 k = n.first; k != kNone; k = m_nodes[k].next) {
                    const ComputedStyle& cs = m_styles.style(m_nodes[k].style);
                    if (!cs.visible || m_nodes[k].cls == m_layerClass || outOfFlow(cs)) continue;
                    const Size m = measure(k, {cw, kInf});
                    colW[col] = std::max(colW[col], m.w + cs.margin.h());
                    rowH = std::max(rowH, m.h + cs.margin.v());
                    if (++col == cols) { total += rowH + (total > 0.f ? s.gap : 0.f); rowH = 0.f; col = 0; }
                }
                if (col) total += rowH + (total > 0.f ? s.gap : 0.f);
                for (u32 k = 0; k < cols; ++k) {
                    const Length& t = s.gridColumnCount ? s.gridColumns[k] : Length{};
                    c.w += t.unit == Unit::Px ? t.value : colW[k];
                }
                c.w += s.gap * f32(cols - 1);
                c.h = total;
                break;
            }
            case LayoutKind::Flow: {
                f32 x = 0.f, y = 0.f, lineH = 0.f;
                for (u32 k = n.first; k != kNone; k = m_nodes[k].next) {
                    const ComputedStyle& cs = m_styles.style(m_nodes[k].style);
                    if (!cs.visible || m_nodes[k].cls == m_layerClass || outOfFlow(cs)) continue;
                    const Size m = measure(k, {in.w, kInf});
                    const f32 mw = m.w + cs.margin.h(), mh = m.h + cs.margin.v();
                    if (x > 0.f && x + mw > in.w) { y += lineH + s.gap; x = 0.f; lineH = 0.f; }
                    x += mw + s.gap;
                    lineH = std::max(lineH, mh);
                    c.w = std::max(c.w, x - s.gap);
                }
                c.h = y + lineH;
                break;
            }
            default: // Stack, Anchor, Absolute: union of children
                for (u32 k = n.first; k != kNone; k = m_nodes[k].next) {
                    const ComputedStyle& cs = m_styles.style(m_nodes[k].style);
                    if (!cs.visible || m_nodes[k].cls == m_layerClass || outOfFlow(cs)) continue;
                    const Size m = measure(k, in);
                    const f32 ox = s.layout == LayoutKind::Stack ? 0.f : cs.left.resolve(in.w, 0.f);
                    const f32 oy = s.layout == LayoutKind::Stack ? 0.f : cs.top.resolve(in.h, 0.f);
                    c.w = std::max(c.w, ox + m.w + cs.margin.h());
                    c.h = std::max(c.h, oy + m.h + cs.margin.v());
                }
                break;
            }
        }
        if (!wc.measure && !m_cold[i]->text.empty()) {
            ensureText(i, in.w);
            const auto& tl = m_cold[i]->layout;
            c.w = std::max(c.w, std::ceil(tl.width));
            c.h = std::max(c.h, std::ceil(tl.height));
        }
        out.w = fw ? bw : c.w + s.padding.h();
        out.h = fh ? bh : c.h + s.padding.v();
        if (s.aspectRatio > 0.f) { // the auto side follows the other one
            if (fh && !fw) out.w = out.h * s.aspectRatio;
            else if (!fh) out.h = out.w / s.aspectRatio;
        }
        out.w = clampSize(out.w, s.minWidth, s.maxWidth, avail.w);
        out.h = clampSize(out.h, s.minHeight, s.maxHeight, avail.h);
    }
    n.measured = out;
    n.measureKey = avail;
    return out;
}

void UiContext::arrange(u32 i, Rect box) {
    Node& n = m_nodes[i];
    if (box == n.rect && !(n.dirty & kDirtyLayout)) return;
    ++m_stats.arranged;
    if (!(box == n.rect)) {
        m_damage = m_damage.unite(n.rect).unite(box);
        n.prevRect = n.rect;
        n.rect = box;
        markPaint(i);
    }
    n.dirty &= u8(~(kDirtyLayout | kDirtyMeasure));
    arrangeChildren(i);
}

void UiContext::arrangeChildren(u32 i) {
    Node& n = m_nodes[i];
    const ComputedStyle& s = m_styles.style(n.style);
    const Rect c0 = contentRect({i, n.gen});
    if (!m_cold[i]->text.empty() && !m_registry.get(n.cls).measure) ensureText(i, c0.w);
    const Rect c{c0.x - n.scroll.x, c0.y - n.scroll.y, c0.w, c0.h};
    Size extent{0.f, 0.f};
    auto place = [&](u32 k, Rect r) {
        const Node& kn = m_nodes[k];
        r.x += kn.offset.x;
        r.y += kn.offset.y;
        extent.w = std::max(extent.w, r.right() - c.x - kn.offset.x + m_styles.style(kn.style).margin.r);
        extent.h = std::max(extent.h, r.bottom() - c.y - kn.offset.y + m_styles.style(kn.style).margin.b);
        // a positioned child: relative moves by its insets, sticky stays in the scrollport (not under an Anchor /
        // Absolute layout, which has placed the child by those insets already)
        if ((n.flags & kFlagZKids) && s.layout != LayoutKind::Anchor && s.layout != LayoutKind::Absolute) {
            const ComputedStyle& ks = m_styles.style(kn.style);
            if (ks.position == PositionKind::Relative) {
                r.x += !ks.left.isAuto() ? ks.left.resolve(c.w, 0.f) : -ks.right.resolve(c.w, 0.f);
                r.y += !ks.top.isAuto() ? ks.top.resolve(c.h, 0.f) : -ks.bottom.resolve(c.h, 0.f);
            } else if (ks.position == PositionKind::Sticky) r = stickyRect(i, k, r, c);
        }
        arrange(k, r);
    };
    auto hidden = [&](u32 k) {
        if (m_nodes[k].cls == m_layerClass) { // overlay layer: covers the viewport, outside the flow
            arrange(k, {0.f, 0.f, m_viewport.w, m_viewport.h});
            return true;
        }
        const ComputedStyle& ks = m_styles.style(m_nodes[k].style);
        if (ks.visible) return outOfFlow(ks); // absolute / fixed: placed after the flow (placeOutOfFlow)
        arrange(k, {c.x, c.y, 0.f, 0.f});
        return true;
    };
    switch (s.layout) {
    case LayoutKind::Box: {
        const bool row = s.direction == Direction::Row;
        const f32 mainAvail = row ? c.w : c.h, crossAvail = row ? c.h : c.w;
        f32 used = 0.f, growSum = 0.f, shrinkSum = 0.f;
        u32 count = 0;
        // pass 1: basis sizes (kept in the node's prevRect.w scratch? no: recompute; measure is cached)
        for (u32 k = n.first; k != kNone; k = m_nodes[k].next) {
            const ComputedStyle& cs = m_styles.style(m_nodes[k].style);
            if (!cs.visible || m_nodes[k].cls == m_layerClass || outOfFlow(cs)) continue;
            const Size m = measure(k, {c.w - cs.margin.h(), c.h - cs.margin.v()});
            const f32 basis = fixedLen(cs.basis, mainAvail) ? cs.basis.resolve(mainAvail, 0.f) : (row ? m.w : m.h);
            used += basis + (row ? cs.margin.h() : cs.margin.v());
            growSum += cs.grow;
            shrinkSum += cs.shrink * basis;
            ++count;
        }
        if (count > 1) used += s.gap * f32(count - 1);
        const f32 free = mainAvail - used;
        f32 pos = 0.f, between = s.gap;
        if (free > 0.f && growSum <= 0.f) {
            switch (s.justify) {
            case Justify::Center: pos = free * 0.5f; break;
            case Justify::End: pos = free; break;
            case Justify::SpaceBetween: if (count > 1) between += free / f32(count - 1); break;
            case Justify::SpaceAround: between += free / f32(count); pos = free / f32(count) * 0.5f; break;
            default: break;
            }
        }
        for (u32 k = n.first; k != kNone; k = m_nodes[k].next) {
            if (hidden(k)) continue;
            const ComputedStyle& cs = m_styles.style(m_nodes[k].style);
            const Size m = measure(k, {c.w - cs.margin.h(), c.h - cs.margin.v()});
            f32 main = fixedLen(cs.basis, mainAvail) ? cs.basis.resolve(mainAvail, 0.f) : (row ? m.w : m.h);
            if (free > 0.f && growSum > 0.f) main += free * cs.grow / growSum;
            // a scroll container never shrinks its children: the overflow is what it scrolls
            else if (free < 0.f && shrinkSum > 0.f && s.overflow != Overflow::Scroll) main += free * cs.shrink * main / shrinkSum;
            main = row ? clampSize(main, cs.minWidth, cs.maxWidth, mainAvail)
                       : clampSize(main, cs.minHeight, cs.maxHeight, mainAvail);
            const f32 mBefore = row ? cs.margin.l : cs.margin.t, mAfter = row ? cs.margin.r : cs.margin.b;
            const f32 cmB = row ? cs.margin.t : cs.margin.l, cmA = row ? cs.margin.b : cs.margin.r;
            const Align al = cs.alignSelf != Align::Auto ? cs.alignSelf : s.alignItems;
            bool crossFixed = row ? fixedLen(cs.height, crossAvail) : fixedLen(cs.width, crossAvail);
            const f32 crossRoom = crossAvail - cmB - cmA;
            f32 cross = row ? m.h : m.w;
            if (cs.aspectRatio > 0.f) { // aspect-ratio: a stretched cross size sets the main size; a given main size the cross size
                const bool mainFixed = row ? fixedLen(cs.width, mainAvail) : fixedLen(cs.height, mainAvail);
                if (!mainFixed && !crossFixed && al == Align::Stretch && cs.grow <= 0.f && !fixedLen(cs.basis, mainAvail))
                    main = row ? std::max(0.f, crossRoom) * cs.aspectRatio : std::max(0.f, crossRoom) / cs.aspectRatio;
                else if (!crossFixed) {
                    cross = row ? main / cs.aspectRatio : main * cs.aspectRatio;
                    crossFixed = true;
                }
            }
            if (al == Align::Stretch && !crossFixed) cross = std::max(0.f, crossRoom);
            f32 co = cmB;
            if (al == Align::Center) co += (crossRoom - cross) * 0.5f;
            else if (al == Align::End) co += crossRoom - cross;
            pos += mBefore;
            Rect r = row ? Rect{c.x + pos, c.y + co, main, cross} : Rect{c.x + co, c.y + pos, cross, main};
            // direction: rtl mirrors the box along x: a row runs from the right edge, a column's start is its right side
            if (s.rtl) r.x = c.x + (c.x + c.w - r.right());
            place(k, {std::round(r.x), std::round(r.y), std::round(r.w), std::round(r.h)});
            pos += main + mAfter + between;
        }
        break;
    }
    case LayoutKind::Grid: {
        u32 cols = std::max<u32>(1, s.gridColumnCount);
        std::array<f32, kMaxGridTracks> w{}, x{};
        auto tracks = [&](const ComputedStyle& gs, f32 width, u32 count) {
            f32 fixedSum = gs.gap * f32(count - 1), frSum = 0.f;
            for (u32 k = 0; k < count; ++k) {
                const Length t = gs.gridColumnCount ? gs.gridColumns[k] : Length::fr(1.f);
                if (t.unit == Unit::Px || t.unit == Unit::Percent) { w[k] = t.resolve(width, 0.f); fixedSum += w[k]; }
                else frSum += t.unit == Unit::Fr ? t.value : 1.f;
            }
            const f32 frUnit = frSum > 0.f ? std::max(0.f, width - fixedSum) / frSum : 0.f;
            for (u32 k = 0; k < count; ++k) {
                const Length t = gs.gridColumnCount ? gs.gridColumns[k] : Length::fr(1.f);
                if (!(t.unit == Unit::Px || t.unit == Unit::Percent)) w[k] = frUnit * (t.unit == Unit::Fr ? t.value : 1.f);
                x[k] = k ? x[k - 1] + w[k - 1] + gs.gap : 0.f;
            }
        };
        const ComputedStyle* ps = n.parent != kNone ? &m_styles.style(m_nodes[n.parent].style) : nullptr;
        if (s.subgrid && ps && ps->layout == LayoutKind::Grid && !ps->subgrid) {
            // subgrid: the columns are the parent's tracks this node spans (found by where it was placed)
            const Node& pn = m_nodes[n.parent];
            const u32 pcols = std::max<u32>(1, ps->gridColumnCount);
            const f32 pw = std::max(0.f, pn.rect.w - ps->padding.h()), px = pn.rect.x + ps->padding.l - pn.scroll.x;
            tracks(*ps, pw, pcols);
            u32 first = 0;
            for (u32 k = 1; k < pcols; ++k)
                if (std::abs(px + x[k] + s.margin.l - n.rect.x) < std::abs(px + x[first] + s.margin.l - n.rect.x)) first = k;
            cols = std::clamp<u32>(s.gridColumnSpan, 1, pcols - first);
            const f32 base = c0.x - px;
            for (u32 k = 0; k < cols; ++k) {
                const f32 lo = std::max(0.f, x[first + k] - base), hi = std::min(c.w, x[first + k] + w[first + k] - base);
                x[k] = lo;
                w[k] = std::max(0.f, hi - lo);
            }
        } else tracks(s, c.w, cols);
        // auto placement row-major; explicit column / row (1-based) honoured, spans within the column count
        u32 col = 0, rowIdx = 0;
        f32 y = 0.f, rowH = 0.f;
        u32 curRow = 0;
        for (u32 k = n.first; k != kNone; k = m_nodes[k].next) {
            if (hidden(k)) continue;
            const ComputedStyle& cs = m_styles.style(m_nodes[k].style);
            const u32 span = std::clamp<u32>(cs.gridColumnSpan, 1, cols);
            if (cs.gridColumn) col = std::min<u32>(cs.gridColumn - 1, cols - 1);
            if (cs.gridRow && cs.gridRow - 1u > rowIdx) { rowIdx = cs.gridRow - 1u; col = cs.gridColumn ? col : 0; }
            if (col + span > cols) { col = 0; ++rowIdx; }
            if (rowIdx != curRow) { y += rowH + s.gap; rowH = 0.f; curRow = rowIdx; }
            const f32 cw = x[col + span - 1] + w[col + span - 1] - x[col];
            const Size m = measure(k, {cw - cs.margin.h(), kInf});
            const f32 h = cs.aspectRatio > 0.f && !fixedLen(cs.height, kInf) ? std::max(0.f, cw - cs.margin.h()) / cs.aspectRatio : m.h;
            place(k, {std::round(c.x + x[col] + cs.margin.l), std::round(c.y + y + cs.margin.t), std::round(cw - cs.margin.h()), std::round(h)});
            rowH = std::max(rowH, h + cs.margin.v());
            col += span;
            if (col >= cols) { col = 0; ++rowIdx; }
        }
        break;
    }
    case LayoutKind::Stack:
        for (u32 k = n.first; k != kNone; k = m_nodes[k].next) {
            if (hidden(k)) continue;
            const ComputedStyle& cs = m_styles.style(m_nodes[k].style);
            const Size m = measure(k, {c.w - cs.margin.h(), c.h - cs.margin.v()});
            const f32 w = fixedLen(cs.width, c.w) ? m.w : c.w - cs.margin.h();
            const f32 h = fixedLen(cs.height, c.h) ? m.h : c.h - cs.margin.v();
            place(k, {std::round(c.x + cs.margin.l + (c.w - cs.margin.h() - w) * 0.5f),
                      std::round(c.y + cs.margin.t + (c.h - cs.margin.v() - h) * 0.5f), w, h});
        }
        break;
    case LayoutKind::Anchor:
    case LayoutKind::Absolute:
        for (u32 k = n.first; k != kNone; k = m_nodes[k].next) {
            if (hidden(k)) continue;
            const ComputedStyle& cs = m_styles.style(m_nodes[k].style);
            const Size m = measure(k, {c.w, c.h});
            // direction: rtl swaps an anchored child's left / right (not for the overlay layer's children and viewport-
            // clamped popups, which are placed in screen coordinates)
            const bool clampPopup = s.layout == LayoutKind::Anchor && hasClass({k, m_nodes[k].gen}, "clamp");
            const bool mirror = s.rtl && s.layout == LayoutKind::Anchor && n.cls != m_layerClass && !clampPopup;
            const Length& csLeft = mirror ? cs.right : cs.left;
            const Length& csRight = mirror ? cs.left : cs.right;
            const bool hasL = !csLeft.isAuto(), hasR = !csRight.isAuto(), hasT = !cs.top.isAuto(), hasB = !cs.bottom.isAuto();
            const f32 l = csLeft.resolve(c.w, 0.f), r = csRight.resolve(c.w, 0.f);
            const f32 t = cs.top.resolve(c.h, 0.f), b = cs.bottom.resolve(c.h, 0.f);
            f32 w = m.w, h = m.h, px = l, py = t;
            if (s.layout == LayoutKind::Anchor) {
                if (hasL && hasR && !fixedLen(cs.width, c.w)) w = std::max(0.f, c.w - l - r);
                if (hasT && hasB && !fixedLen(cs.height, c.h)) h = std::max(0.f, c.h - t - b);
                if (cs.centerX) px = (c.w - w) * 0.5f + l - r;
                else if (!hasL && hasR) px = c.w - r - w;
                if (cs.centerY) py = (c.h - h) * 0.5f + t - b;
                else if (!hasT && hasB) py = c.h - b - h;
            }
            if (clampPopup) { // keep popups on screen
                const Rect a = popupArea();
                px = std::max(a.x - c.x, std::min(px, a.x + a.w - c.x - w - cs.margin.h()));
                py = std::max(a.y - c.y, std::min(py, a.y + a.h - c.y - h - cs.margin.v()));
            }
            place(k, {std::round(c.x + px + cs.margin.l), std::round(c.y + py + cs.margin.t), w, h});
        }
        break;
    case LayoutKind::Flow: {
        f32 x = 0.f, y = 0.f, lineH = 0.f;
        for (u32 k = n.first; k != kNone; k = m_nodes[k].next) {
            if (hidden(k)) continue;
            const ComputedStyle& cs = m_styles.style(m_nodes[k].style);
            const Size m = measure(k, {c.w, kInf});
            const f32 mw = m.w + cs.margin.h();
            if (x > 0.f && x + mw > c.w) { y += lineH + s.gap; x = 0.f; lineH = 0.f; }
            place(k, {std::round(c.x + x + cs.margin.l), std::round(c.y + y + cs.margin.t), m.w, m.h});
            x += mw + s.gap;
            lineH = std::max(lineH, m.h + cs.margin.v());
        }
        break;
    }
    }
    n.extent = extent;
    if (n.flags & kFlagZKids)
        for (u32 k = n.first; k != kNone; k = m_nodes[k].next)
            if (m_nodes[k].flags & kFlagOutOfFlow) placeOutOfFlow(i, k);
    if (s.overflow == Overflow::Scroll) {
        const Vec2 mx{std::max(0.f, extent.w - c.w), std::max(0.f, extent.h - c.h)};
        if (n.scroll.x > mx.x || n.scroll.y > mx.y) {
            n.scroll = {std::min(n.scroll.x, mx.x), std::min(n.scroll.y, mx.y)};
            n.dirty |= kDirtyLayout;
            arrangeChildren(i);
            n.dirty &= u8(~kDirtyLayout);
        }
    }
}

// ---- Paint ---------------------------------------------------------------------------------------------------------
void UiContext::paintRec(u32 i, f32 opacity, DrawList& dl) {
    if (m_nodes[i].flags & kFlagFx) paintEffects(i, opacity, dl);
    else paintBody(i, opacity, dl);
}

void UiContext::paintBody(u32 i, f32 opacity, DrawList& dl) {
    Node& n = m_nodes[i];
    const ComputedStyle& s = m_styles.style(n.style);
    if (n.dirty & kDirtyPaint) {
        m_damage = m_damage.unite(n.rect);
        n.dirty &= u8(~kDirtyPaint);
    }
    if (!s.visible) return;
    opacity *= s.opacity * n.animOpacity;
    if (opacity <= 0.001f || n.rect.empty()) return;
    ++m_stats.painted;
    const NodeHandle h{i, n.gen};
    auto tint = [&](Color c) { return c.withAlpha(c.a * opacity); };
    const f32 rad = s.radius[0];
    const std::size_t firstCmd = dl.size();
    if (s.shadowColor.a > 0.f && (s.shadowBlur > 0.f || s.shadowX != 0.f || s.shadowY != 0.f))
        dl.shadow({n.rect.x + s.shadowX, n.rect.y + s.shadowY, n.rect.w, n.rect.h}, tint(s.shadowColor), s.shadowBlur, rad);
    if (s.backdropBlur > 0.f) dl.backdrop(n.rect, s.backdropBlur, s.radius);
    if (s.gradient != GradientDir::None) {
        dl.gradientRect(n.rect, tint(s.background), tint(s.background2), s.gradient == GradientDir::Vertical, s.radius);
        if (s.borderWidth > 0.f && s.borderColor.a > 0.f) dl.roundedRect(n.rect, {}, s.radius, tint(s.borderColor), s.borderWidth);
    } else if (s.background.a > 0.f || (s.borderWidth > 0.f && s.borderColor.a > 0.f)) {
        dl.roundedRect(n.rect, tint(s.background), s.radius, tint(s.borderColor), s.borderWidth);
    }
    if (s.backgroundImage.kind != GradientSpecKind::None) { // background-image: over the background colour
        GradientStop stops[kMaxGradientStops];
        const Gradient g = gradientOf(s.backgroundImage, n.rect, stops);
        for (u8 k = 0; k < s.backgroundImage.stopCount; ++k) stops[k].color.a *= opacity;
        dl.gradientRect(n.rect, g, s.radius);
    }
    for (std::size_t k = firstCmd; k < dl.size(); ++k) dl.cmds()[k].node = i;
    const WidgetClass& wc = m_registry.get(n.cls);
    const Rect content = contentRect(h);
    if (wc.paint) {
        const PaintContext pc{*this, h, n.rect, content, s, n.state, opacity};
        const TextScope prev = enterTextScope(i);
        wc.paint(pc, dl);
        m_textScope = prev;
    } else if (!m_cold[i]->text.empty()) {
        const auto& tl = m_cold[i]->layout;
        const f32 y = content.y + std::floor(std::max(0.f, content.h - tl.height) * 0.5f);
        paintTextLayout(dl, tl, {content.x, y}, tint(s.color), s.fontSize, i);
    }
    if (n.first == kNone) return;
    const bool clip = s.overflow != Overflow::Visible;
    if (clip) dl.pushClip(n.rect, rad);
    auto paintChild = [&](u32 k) {
        // culled (not when it has effects: a transform moves it); still consume dirty flags
        if (clip && !(m_nodes[k].flags & kFlagFx) && m_nodes[k].rect.intersect(n.rect).empty()) {
            if (m_nodes[k].dirty & kDirtyPaint) { m_damage = m_damage.unite(m_nodes[k].rect.intersect(n.rect)); m_nodes[k].dirty &= u8(~kDirtyPaint); }
            return;
        }
        if ((m_osPopups || m_osWindows) && &dl == &m_draw) { // own OS window (popup / tool window)
            const NodeHandle kh{k, m_nodes[k].gen};
            if ((m_osPopups && hasClass(kh, "clamp")) || (m_osWindows && hasClass(kh, "os-window"))) {
                m_popupPending.push_back(k);
                return;
            }
        }
        paintRec(k, opacity, dl);
    };
    if (!(n.flags & kFlagZKids)) {
        for (u32 k = n.first; k != kNone; k = m_nodes[k].next) paintChild(k);
    } else { // z order: negative z-index, in-flow, positioned, positive z-index; fixed nodes go last, over everything
        const u32 base = sortChildren(i);
        for (std::size_t z = base; z < m_zScratch.size(); ++z) {
            const u32 k = m_zScratch[z].node;
            // fixed nodes of the window's own list are painted last, from paintLists (a culled or scrolled-away
            // parent does not take them along)
            if (&dl == &m_draw && m_styles.style(m_nodes[k].style).position == PositionKind::Fixed) continue;
            paintChild(k);
        }
        m_zScratch.resize(base);
    }
    if (clip) dl.popClip();
    if (s.overflow == Overflow::Scroll)
        for (u8 ax = 0; ax < 2; ++ax) {
            Rect tr, th;
            if (!scrollBarGeom(i, ax, tr, th)) continue;
            const bool hot = m_scrollDrag.node.index == i && m_scrollDrag.axis == ax;
            dl.rect(tr, tint(Color::rgb(0x000000, 0.12f)), kScrollBarSize * 0.5f);
            dl.rect(th, tint(Color::rgb(hot ? 0xB4B4C0 : 0x8A8A96, 0.85f)), kScrollBarSize * 0.5f);
        }
}

bool UiContext::scrollBarGeom(u32 i, u8 axis, Rect& track, Rect& thumb) const {
    const Node& n = m_nodes[i];
    const Rect c = contentRect({i, n.gen});
    const f32 view = axis ? c.h : c.w, ext = axis ? n.extent.h : n.extent.w;
    if (ext <= view + 0.5f || view <= 0.f) return false;
    const f32 b = kScrollBarSize;
    track = axis ? Rect{n.rect.x + n.rect.w - b - 1.f, n.rect.y + 1.f, b, n.rect.h - 2.f}
                 : Rect{n.rect.x + 1.f, n.rect.y + n.rect.h - b - 1.f, n.rect.w - 2.f - b, b};
    const f32 len = axis ? track.h : track.w;
    const f32 tl = std::max(16.f, len * view / ext);
    const f32 at = (len - tl) * std::clamp((axis ? n.scroll.y : n.scroll.x) / (ext - view), 0.f, 1.f);
    thumb = axis ? Rect{track.x, track.y + at, b, tl} : Rect{track.x + at, track.y, tl, b};
    return true;
}

void UiContext::paintLists(std::vector<PopupSurface>& next, std::vector<PopupSurface>& nextWindows) {
    const f32 rs = m_renderScale;
    m_draw.clear();
    m_popupPending.clear();
    next.clear();
    nextWindows.clear();
    m_fixedOrder.clear();
    paintRec(m_root.index, 1.f, m_draw);
    if (!m_outOfFlow.empty()) { // position: fixed: over everything, outside every clip, by z-index then creation order
        for (u32 k : m_outOfFlow)
            if (m_styles.style(m_nodes[k].style).position == PositionKind::Fixed && m_nodes[k].parent != kNone && visibleChain(m_nodes[k].parent))
                m_fixedOrder.push_back(k);
        std::stable_sort(m_fixedOrder.begin(), m_fixedOrder.end(), [&](u32 a, u32 b) {
            const i32 za = m_styles.style(m_nodes[a].style).zIndex, zb = m_styles.style(m_nodes[b].style).zIndex;
            return (za == kZAuto ? 0 : za) < (zb == kZAuto ? 0 : zb);
        });
        for (u32 k : m_fixedOrder) paintRec(k, 1.f, m_draw);
    }
    if (!m_fxPendingDamage.empty()) {
        m_damage = m_damage.unite(m_fxPendingDamage);
        m_fxPendingDamage = {};
    }
    if (m_drag.active) paintDragOverlay(m_draw); // drag ghost + drop indicator, over everything
    m_draw.scale(rs);
    for (u32 k : m_popupPending) {
        const bool tool = m_osWindows && hasClass({k, m_nodes[k].gen}, "os-window");
        const std::vector<PopupSurface>& prev = tool ? m_windowSurfaces : m_popupSurfaces;
        PopupSurface ps;
        ps.node = {k, m_nodes[k].gen};
        ps.rect = m_nodes[k].rect;
        const Rect saved = m_damage;
        paintRec(k, 1.f, ps.draw);
        m_damage = saved; // a popup window repaints itself; the main window is not damaged by it
        std::erase_if(ps.draw.cmds(), [&](const DrawCmd& c) { return c.op == DrawOp::Shadow && c.node == k; }); // OS shadow
        if (ps.draw.size() == 0 || ps.rect.empty()) continue;
        if (rs == 1.f) {
            ps.draw.translate({-ps.rect.x, -ps.rect.y});
        } else { // device pixels; a whole-pixel origin keeps glyphs and snapped edges on the pixel grid
            ps.draw.scale(rs);
            ps.draw.translate({-std::round(ps.rect.x * rs), -std::round(ps.rect.y * rs)});
        }
        ps.revision = ++m_popupRevision;
        for (const PopupSurface& old : prev)
            if (old.node == ps.node && old.rect.x == ps.rect.x && old.rect.y == ps.rect.y && old.rect.w == ps.rect.w &&
                old.rect.h == ps.rect.h && old.draw.hash() == ps.draw.hash())
                ps.revision = old.revision;
        (tool ? nextWindows : next).push_back(std::move(ps));
    }
}

void UiContext::paintPass() {
    m_stats.repainted = false;
    if (!m_anyPaintDirty) return;
    std::vector<PopupSurface> next, nextWindows;
    m_painting = true;
    const std::uint32_t atlasGen = m_atlas.generation();
    paintLists(next, nextWindows);
    if (m_atlas.generation() != atlasGen) {
        // the glyph atlas filled up and was reset while painting (likelier at high render scales): commands emitted
        // before the reset point at stale texels, so paint once more into the fresh atlas
        m_stats.painted = 0;
        paintLists(next, nextWindows);
    }
    m_painting = false;
    if (next.size() != m_popupSurfaces.size() || nextWindows.size() != m_windowSurfaces.size())
        m_damage = m_damage.unite({0.f, 0.f, m_viewport.w, m_viewport.h});
    m_popupSurfaces = std::move(next);
    m_windowSurfaces = std::move(nextWindows);
    if (m_overlayDirty) { // the drag overlay is not a node: repaint everything it may have touched
        m_damage = {0.f, 0.f, m_viewport.w, m_viewport.h};
        m_overlayDirty = false;
    }
    m_anyPaintDirty = false;
    m_stats.repainted = true;
    m_stats.drawCmds = u32(m_draw.size());
}

void UiContext::setRenderScale(f32 scale) {
    if (!(scale > 0.f) || !std::isfinite(scale)) scale = 1.f;
    if (scale == m_renderScale) return;
    m_renderScale = scale;
    markAllDirty();
    syncMedia();
}
Rect UiContext::deviceDamage() const {
    const f32 rs = m_renderScale;
    if (rs == 1.f || m_damage.empty()) return m_damage;
    const f32 x0 = std::floor(m_damage.x * rs) - 1.f, y0 = std::floor(m_damage.y * rs) - 1.f;
    const f32 x1 = std::ceil(m_damage.right() * rs) + 1.f, y1 = std::ceil(m_damage.bottom() * rs) + 1.f;
    return {x0, y0, x1 - x0, y1 - y0};
}

void UiContext::setOsPopups(bool on, Rect screen) {
    if (on == m_osPopups && screen.x == m_popupScreen.x && screen.y == m_popupScreen.y && screen.w == m_popupScreen.w &&
        screen.h == m_popupScreen.h)
        return;
    m_osPopups = on;
    m_popupScreen = screen;
    if (!on) m_popupSurfaces.clear();
    markAllDirty();
}
void UiContext::setOsWindows(bool on) {
    if (on == m_osWindows) return;
    m_osWindows = on;
    if (!on) m_windowSurfaces.clear();
    markAllDirty();
}
Rect UiContext::popupArea() const {
    if (m_osPopups && !m_popupScreen.empty()) return m_popupScreen;
    return {0.f, 0.f, m_viewport.w, m_viewport.h};
}

// ---- Frame ---------------------------------------------------------------------------------------------------------
void UiContext::update(f32 dt) {
    ++m_frame;
    m_stats = {};
    m_damage = {};
    drainCommands();
    if (dt > 0.f) {
        for (u32 t = 0; t < m_timers.size(); ++t) {
            if (!m_timers[t].active) continue;
            m_timers[t].left -= dt;
            int guard = 0;
            while (m_timers[t].active && m_timers[t].left <= 0.f && guard++ < 8) {
                const auto fn = m_timers[t].fn;
                if (m_timers[t].repeat && m_timers[t].interval > 0.f) m_timers[t].left += m_timers[t].interval;
                else m_timers[t].active = false;
                if (fn) fn(*this);
            }
        }
    }
    if (dt > 0.f) m_clock += double(dt);
    if (m_touch.fling) {
        if (!node(m_touch.target)) m_touch.fling = false;
        else if (dt > 0.f) {
            // exact integral of v0 * exp(-decay * t) over the step: the glide length does not depend on the frame rate
            const f32 k = std::exp(-m_touchCfg.decay * dt), travel = (1.f - k) / std::max(1e-3f, m_touchCfg.decay);
            const Vec2 d{m_touch.vel.x * travel, m_touch.vel.y * travel};
            m_touch.vel = {m_touch.vel.x * k, m_touch.vel.y * k};
            touchPanBy(d); // zeroes the velocity along an axis that reached its end
            if (std::hypot(m_touch.vel.x, m_touch.vel.y) < m_touchCfg.minVelocity) m_touch.fling = false;
        }
    }
    if (m_animCount) {
        for (auto& a : m_anims) {
            if (!a.active) continue;
            Node* n = node(a.node);
            if (!n) { a.active = false; --m_animCount; continue; }
            a.t += dt;
            const f32 k = applyEasing(a.ease, a.duration > 0.f ? std::clamp(a.t / a.duration, 0.f, 1.f) : 1.f);
            const f32 v = a.from + (a.to - a.from) * k;
            switch (a.prop) {
            case AnimProp::Opacity: n->animOpacity = std::clamp(v, 0.f, 1.f); markPaint(a.node.index); break;
            case AnimProp::OffsetX: n->offset.x = v; if (n->parent != kNone) markArrange(n->parent); n->dirty |= kDirtyLayout; break;
            case AnimProp::OffsetY: n->offset.y = v; if (n->parent != kNone) markArrange(n->parent); n->dirty |= kDirtyLayout; break;
            case AnimProp::Value: setValue(a.node, v, true); break;
            }
            if (a.t >= a.duration) { a.active = false; --m_animCount; }
        }
    }
    if (m_keyAnimCount || m_keyAnimDirty) tickKeyAnims(dt);
    if (m_styleAnimCount || m_propAnimCount) tickStyleAnims(dt);
    if (m_gesture.longArmed) gestureTick(dt);
    if (m_anyStyleDirty || m_styles.sheetVersion() != m_seenSheetVersion) stylePass();
    if (m_anyLayoutDirty) {
        Node& r = m_nodes[m_root.index];
        if (r.measureKey != Size{m_viewport.w, m_viewport.h}) measure(m_root.index, m_viewport);
        arrange(m_root.index, {0.f, 0.f, m_viewport.w, m_viewport.h});
        if (!m_outOfFlow.empty()) placeAllOutOfFlow();
        m_anyLayoutDirty = false;
        // container queries: a container whose size changed restyles its subtree, which may lay out again
        for (int pass = 0; pass < 4 && !m_containers.empty() && settleContainers(); ++pass) {
            stylePass();
            arrange(m_root.index, {0.f, 0.f, m_viewport.w, m_viewport.h});
            if (!m_outOfFlow.empty()) placeAllOutOfFlow();
            m_anyLayoutDirty = false;
        }
    }
    paintPass();
}

bool UiContext::needsUpdate() const {
    if (m_anyStyleDirty || m_anyLayoutDirty || m_anyPaintDirty || m_animCount || m_touch.fling) return true;
    if (m_styleAnimBusy || m_propAnimCount || m_keyAnimRunning || m_keyAnimDirty || m_gesture.longArmed) return true;
    if (m_styles.sheetVersion() != m_seenSheetVersion) return true;
    for (const auto& t : m_timers)
        if (t.active) return true;
    return m_head.load(std::memory_order_acquire) != m_tail;
}

f32 UiContext::nextWakeSeconds() const {
    if (m_anyStyleDirty || m_anyLayoutDirty || m_anyPaintDirty || m_animCount || m_touch.fling) return 0.f;
    if (m_styleAnimBusy || m_propAnimCount || m_keyAnimRunning || m_keyAnimDirty) return 0.f;
    if (m_styles.sheetVersion() != m_seenSheetVersion) return 0.f;
    if (m_head.load(std::memory_order_acquire) != m_tail) return 0.f;
    f32 t = std::numeric_limits<f32>::infinity();
    for (const auto& tm : m_timers)
        if (tm.active) t = std::min(t, std::max(0.f, tm.left));
    if (m_gesture.longArmed) t = std::min(t, std::max(0.f, m_gestureCfg.longPressTime - m_gesture.held));
    return t;
}

// ---- Hit testing and dispatch --------------------------------------------------------------------------------------
NodeHandle UiContext::hitRec(u32 i, Vec2 p) const {
    const Node& n = m_nodes[i];
    const ComputedStyle& s = m_styles.style(n.style);
    if (!s.visible || n.animOpacity <= 0.f || (n.flags & kFlagExiting)) return {};
    if ((n.flags & kFlagFx) && !mapIntoNode(i, p)) return {}; // through the inverse transform; outside clip-path / mask
    const bool inside = n.rect.contains(p);
    if (!inside && s.overflow != Overflow::Visible) return {};
    if (!(n.flags & kFlagZKids)) {
        for (u32 k = n.last; k != kNone; k = m_nodes[k].prev) {
            const NodeHandle h = hitRec(k, p);
            if (h.valid()) return h;
        }
    } else { // topmost first: the paint order backwards; fixed nodes are hit from hitTest
        const u32 base = sortChildren(i);
        NodeHandle h;
        for (std::size_t z = m_zScratch.size(); z-- > base && !h.valid();) {
            const u32 k = m_zScratch[z].node;
            if (m_styles.style(m_nodes[k].style).position != PositionKind::Fixed) h = hitRec(k, p);
        }
        m_zScratch.resize(base);
        if (h.valid()) return h;
    }
    if (inside && s.pointerEvents) return {i, n.gen};
    return {};
}
NodeHandle UiContext::hitTest(Vec2 p) const {
    for (std::size_t k = m_fixedOrder.size(); k-- > 0;) { // position: fixed nodes are on top of everything
        const u32 f = m_fixedOrder[k];
        if (f >= m_nodes.size() || !m_nodes[f].alive || !(m_nodes[f].flags & kFlagOutOfFlow) || !visibleChain(f)) continue;
        const NodeHandle h = hitRec(f, p);
        if (h.valid()) return h;
    }
    return hitRec(m_root.index, p);
}

bool UiContext::dispatch(u32 target, const Event& ev) {
    // inside transformed nodes handlers get the pointer in the target's layout space (comparable with rect())
    Event mapped;
    const Event* ep = &ev;
    if (m_fxCount && ev.type != EventType::KeyDown && ev.type != EventType::KeyUp && ev.type != EventType::Text) {
        const Vec2 local = toLocal({target, m_nodes[target].gen}, ev.pos);
        if (!(local == ev.pos)) {
            mapped = ev;
            mapped.pos = local;
            ep = &mapped;
        }
    }
    const Event& e = *ep;
    m_path.clear();
    for (u32 a = target; a != kNone; a = m_nodes[a].parent) m_path.push_back(a);
    const NodeHandle th{target, m_nodes[target].gen};
    bool ownMenu = false;
    auto call = [&](u32 k, Phase ph) {
        if (!m_nodes[k].alive) return false;
        const WidgetClass& wc = m_registry.get(m_nodes[k].cls);
        if (!wc.onEvent) return false;
        EventContext ec{*this, {k, m_nodes[k].gen}, th, ph};
        const TextScope prev = enterTextScope(k);
        const bool consumed = wc.onEvent(ec, e);
        m_textScope = prev;
        ownMenu = ownMenu || ec.contextMenuHandled;
        return consumed;
    };
    for (std::size_t k = m_path.size(); k-- > 1;)
        if (call(m_path[k], Phase::Capture)) return true;
    if (call(target, Phase::Target)) {
        // the context-menu press still reaches the node's event slots (see EventContext::contextMenuHandled)
        if (e.type == EventType::PointerDown && e.button == MouseButton::Right && !ownMenu && m_nodes[target].alive &&
            m_nodes[target].gen == th.generation)
            emit(th, SignalKind::Event, {.event = &e});
        return true;
    }
    if (m_nodes[target].alive) emit(th, SignalKind::Event, {.event = &e});
    // path may have been rebuilt by a nested dispatch; recompute the ancestors
    for (u32 a = m_nodes[target].alive ? m_nodes[target].parent : kNone; a != kNone; a = m_nodes[a].parent)
        if (call(a, Phase::Bubble)) return true;
    return false;
}

void UiContext::setHover(u32 i) {
    const u32 old = node(m_hover) ? m_hover.index : kNone;
    if (old == i) return;
    for (u32 a = old; a != kNone; a = m_nodes[a].parent) setState({a, m_nodes[a].gen}, kStateHover, false);
    for (u32 a = i; a != kNone; a = m_nodes[a].parent) setState({a, m_nodes[a].gen}, kStateHover, true);
    m_hover = handleOf(i);
    if (old != kNone) {
        Event e; e.type = EventType::PointerLeave; e.pos = m_pointer;
        dispatch(old, e);
        if (m_nodes[old].alive) emit({old, m_nodes[old].gen}, SignalKind::HoverChanged, {.flag = false});
    }
    if (i != kNone && m_nodes[i].alive) {
        Event e; e.type = EventType::PointerEnter; e.pos = m_pointer;
        dispatch(i, e);
        emit({i, m_nodes[i].gen}, SignalKind::HoverChanged, {.flag = true});
    }
}

u32 UiContext::interactiveAncestor(u32 i) const {
    for (u32 a = i; a != kNone; a = m_nodes[a].parent)
        if (m_registry.get(m_nodes[a].cls).interactive) return a;
    return kNone;
}

bool UiContext::visibleChain(u32 i) const {
    for (u32 a = i; a != kNone; a = m_nodes[a].parent)
        if (!m_styles.style(m_nodes[a].style).visible || (m_nodes[a].flags & kFlagExiting)) return false;
    return true;
}

bool UiContext::pointerMove(Vec2 p) {
    m_pointer = p;
    if (m_drag.active) return m_drag.external ? true : dragMoveTo(p);
    if (const Node* sn = node(m_scrollDrag.node)) {
        Rect tr, th;
        const u32 si = m_scrollDrag.node.index;
        if (scrollBarGeom(si, m_scrollDrag.axis, tr, th)) {
            const u8 ax = m_scrollDrag.axis;
            const Rect c = contentRect(m_scrollDrag.node);
            const f32 len = ax ? tr.h - th.h : tr.w - th.w, ext = ax ? sn->extent.h - c.h : sn->extent.w - c.w;
            const f32 f = std::clamp(((ax ? p.y - tr.y : p.x - tr.x) - m_scrollDrag.grab) / std::max(1.f, len), 0.f, 1.f);
            scrollTo(m_scrollDrag.node, ax ? Vec2{sn->scroll.x, f * ext} : Vec2{f * ext, sn->scroll.y});
        }
        return true;
    }
    NodeHandle h = hitTest(p);
    if (node(m_modal) && h.valid() && !inScope(h.index, m_modal.index)) h = {};
    setHover(h.valid() ? h.index : kNone);
    if (node(m_pressed) && maybeStartDrag(p)) return true; // the press became a drag
    Event e; e.type = EventType::PointerMove; e.pos = p; e.mods = m_mods;
    if (node(m_pressed)) return dispatch(m_pressed.index, e) || true;
    if (h.valid() && h != m_root) { dispatch(h.index, e); return true; }
    return node(m_modal) != nullptr;
}

bool UiContext::pointerDown(Vec2 p, MouseButton b) {
    m_pointer = p;
    const NodeHandle h = hitTest(p);
    if (node(m_modal) && (!h.valid() || !inScope(h.index, m_modal.index))) return true; // blocked by modal
    if (b == MouseButton::Left)
        for (u32 a = h.valid() ? h.index : kNone; a != kNone; a = m_nodes[a].parent) {
            if (m_styles.style(m_nodes[a].style).overflow != Overflow::Scroll) continue;
            for (u8 ax = 2; ax-- > 0;) {
                Rect tr, th;
                if (!scrollBarGeom(a, ax, tr, th) || !tr.contains(p)) continue;
                const f32 pp = ax ? p.y : p.x, t0 = ax ? th.y : th.x, tl = ax ? th.h : th.w;
                m_scrollDrag = {{a, m_nodes[a].gen}, ax, (pp >= t0 && pp < t0 + tl) ? pp - t0 : tl * 0.5f};
                pointerMove(p); // jump-to / no-op for a thumb grab
                markPaint(a);
                return true;
            }
        }
    if (!h.valid() || h == m_root) {
        if (b == MouseButton::Left) setFocus({}, FocusReason::Pointer);
        return false;
    }
    setHover(h.index);
    u32 target = interactiveAncestor(h.index);
    if (target == kNone) target = h.index;
    if (m_nodes[target].state & kStateDisabled) return true;
    if (b == MouseButton::Left) {
        m_pressed = {target, m_nodes[target].gen};
        setState(m_pressed, kStatePressed, true);
        u32 f = target;
        while (f != kNone && !focusableNode(f)) f = m_nodes[f].parent;
        setFocus(handleOf(f), FocusReason::Pointer);
        noteClick(target, h, p);
    }
    Event e; e.type = EventType::PointerDown; e.pos = p; e.button = b; e.mods = m_mods;
    if (b == MouseButton::Left) e.clicks = m_clickCount;
    if (!m_nodes[target].alive) return true; // a focus slot removed it
    dispatch(target, e);
    return true;
}

bool UiContext::pointerUp(Vec2 p, MouseButton b) {
    m_pointer = p;
    if (m_drag.active && !m_drag.external && b == MouseButton::Left) return dragFinish(p, true);
    if (m_scrollDrag.node.valid() && b == MouseButton::Left) {
        if (node(m_scrollDrag.node)) markPaint(m_scrollDrag.node.index);
        m_scrollDrag = {};
        return true;
    }
    const NodeHandle pr = m_pressed;
    if (!node(pr) || b != MouseButton::Left) {
        const NodeHandle h = hitTest(p);
        if (h.valid() && h != m_root) {
            Event e; e.type = EventType::PointerUp; e.pos = p; e.button = b; e.mods = m_mods;
            dispatch(h.index, e);
            return true;
        }
        return node(m_modal) != nullptr;
    }
    m_pressed = {};
    setState(pr, kStatePressed, false);
    Event e; e.type = EventType::PointerUp; e.pos = p; e.button = b; e.mods = m_mods; e.clicks = m_clickCount;
    dispatch(pr.index, e);
    const NodeHandle h = hitTest(p);
    bool inside = false;
    for (u32 a = h.valid() ? h.index : kNone; a != kNone; a = m_nodes[a].parent)
        if (a == pr.index) { inside = true; break; }
    if (inside && node(pr) && !(m_nodes[pr.index].state & kStateDisabled)) {
        Event c; c.type = EventType::Click; c.pos = p; c.button = b; c.mods = m_mods; c.clicks = m_clickCount;
        if (!dispatch(pr.index, c) && node(pr)) emit(pr, SignalKind::Clicked, {});
        if (m_clickCount == 2 && node(pr) && pr == m_clickNode) { // the second click of a double click
            c.type = EventType::DoubleClick;
            if (!dispatch(pr.index, c) && node(pr)) emit(pr, SignalKind::DoubleClicked, {});
        }
    }
    return true;
}

bool UiContext::wheel(Vec2 p, Vec2 notches) {
    const NodeHandle h = hitTest(p);
    if (!h.valid()) return false;
    if (node(m_modal) && !inScope(h.index, m_modal.index)) return true;
    Event e; e.type = EventType::Wheel; e.pos = p; e.wheel = notches; e.mods = m_mods;
    // a disabled widget takes no wheel input (as it takes no press); what is around it still scrolls
    u32 start = h.index;
    if (const u32 t = interactiveAncestor(h.index); t != kNone && (m_nodes[t].state & kStateDisabled)) start = m_nodes[t].parent;
    if (start != kNone && dispatch(start, e)) return true;
    for (u32 a = h.index; a != kNone; a = m_nodes[a].parent) {
        const Node& n = m_nodes[a];
        if (m_styles.style(n.style).overflow != Overflow::Scroll) continue;
        const Rect c = contentRect({a, n.gen});
        if (n.extent.h <= c.h && n.extent.w <= c.w) continue;
        scrollTo({a, n.gen}, {n.scroll.x - notches.x * 40.f, n.scroll.y - notches.y * 40.f});
        return true;
    }
    return h != m_root;
}

// ---- Touch: tap, pan, fling ----------------------------------------------------------------------------------------
void UiContext::cancelPress(Vec2 p) {
    const NodeHandle pr = m_pressed;
    if (!node(pr)) return;
    m_pressed = {};
    setState(pr, kStatePressed, false);
    Event e; e.type = EventType::PointerUp; e.pos = p; e.mods = m_mods; e.cancel = true;
    dispatch(pr.index, e);
}

void UiContext::touchSample(Vec2 p, double t) {
    TouchState& ts = m_touch;
    ts.samples[ts.sampleHead] = {t, p};
    ts.sampleHead = (ts.sampleHead + 1) % TouchState::kSamples;
    ts.sampleCount = std::min(ts.sampleCount + 1, TouchState::kSamples);
}

bool UiContext::touchDown(Vec2 p, double t) {
    if (t < 0.0) t = m_clock;
    const bool wasFlinging = m_touch.fling;
    if (m_touch.stage != TouchState::Stage::Idle) touchCancel(); // a down without an up: drop the old gesture
    m_touch.fling = false;
    m_touch.stage = TouchState::Stage::Pending;
    m_touch.wheel = false;
    m_touch.target = {};
    m_touch.down = m_touch.last = p;
    m_touch.acc = m_touch.vel = {};
    m_touch.sampleCount = m_touch.sampleHead = 0;
    touchSample(p, t);
    m_touch.pressed = !wasFlinging;
    if (wasFlinging) { // the finger catches the moving content: no press, no click; a drag pans again
        m_pointer = p;
        m_touch.hit = hitTest(p);
        return true;
    }
    pointerMove(p); // hover / enter, as a mouse arriving at the point
    const bool used = pointerDown(p, MouseButton::Left);
    m_touch.hit = hitTest(p);
    if (m_scrollDrag.node.valid()) m_touch.stage = TouchState::Stage::Passive; // on an overlay scrollbar
    gestureArm(p, t);
    return used;
}

bool UiContext::touchBeginPan(Vec2 p) {
    TouchState& ts = m_touch;
    const Vec2 d{p.x - ts.down.x, p.y - ts.down.y};
    const u8 ax = std::abs(d.y) >= std::abs(d.x) ? 1 : 0;
    NodeHandle h = node(ts.hit) ? ts.hit : hitTest(ts.down);
    if (node(m_modal) && h.valid() && !inScope(h.index, m_modal.index)) h = {};
    for (u32 a = h.valid() ? h.index : kNone; a != kNone; a = m_nodes[a].parent) {
        const Node& n = m_nodes[a];
        const WidgetClass& wc = m_registry.get(n.cls);
        const NodeHandle ah{a, n.gen};
        if ((n.flags & kFlagCapturesDrag) || wc.capturesDrag) { ts.stage = TouchState::Stage::Passive; return false; }
        if (wc.touchDrag) {
            const TouchDrag td = wc.touchDrag(*this, ah, ts.down);
            if (td.mode == TouchDrag::Mode::Capture) { ts.stage = TouchState::Stage::Passive; return false; }
            if (td.mode == TouchDrag::Mode::Wheel && (ax ? td.step.y : td.step.x) > 0.f) {
                ts.wheel = true;
                ts.drag = td;
                ts.target = ah;
                break;
            }
        }
        if (m_styles.style(n.style).overflow == Overflow::Scroll) {
            // chained like a browser: a container already at its end in the drag direction lets its ancestor pan
            const Rect c = contentRect(ah);
            const f32 max = ax ? n.extent.h - c.h : n.extent.w - c.w, cur = ax ? n.scroll.y : n.scroll.x;
            const f32 want = -(ax ? d.y : d.x); // scroll offset change
            if (max > 0.5f && (want > 0.f ? cur < max - 0.5f : cur > 0.5f)) {
                ts.wheel = false;
                ts.target = ah;
                break;
            }
        }
        if (node(m_modal) && a == m_modal.index) break;
    }
    if (!ts.target.valid()) return false; // nothing to pan (yet): the press stays, later moves ask again
    cancelPress(p);
    setHover(kNone);
    ts.stage = TouchState::Stage::Pan;
    ts.acc = {};
    ts.last = ts.down; // the content under the finger stays under it: the slop travel is applied too
    return true;
}

void UiContext::touchPanBy(Vec2 d) {
    TouchState& ts = m_touch;
    const Node* n = node(ts.target);
    if (!n) return;
    if (!ts.wheel) {
        const Rect c = contentRect(ts.target);
        const f32 maxX = std::max(0.f, n->extent.w - c.w), maxY = std::max(0.f, n->extent.h - c.h);
        const Vec2 o{std::clamp(n->scroll.x - d.x, 0.f, maxX), std::clamp(n->scroll.y - d.y, 0.f, maxY)};
        // a fling ends at the extent it runs into (finger velocity > 0 = offset decreasing)
        if ((o.x <= 0.f && ts.vel.x > 0.f) || (o.x >= maxX && ts.vel.x < 0.f)) ts.vel.x = 0.f;
        if ((o.y <= 0.f && ts.vel.y > 0.f) || (o.y >= maxY && ts.vel.y < 0.f)) ts.vel.y = 0.f;
        scrollTo(ts.target, o);
        return;
    }
    // Wheel-scrolled widget: whole increments now (nearest, so the rows track the finger position both ways), the
    // rest stays in the accumulator (slow drags still move)
    Vec2 notches;
    if (ts.drag.step.x > 0.f) {
        ts.acc.x += d.x;
        const f32 k = std::round(ts.acc.x / ts.drag.step.x);
        ts.acc.x -= k * ts.drag.step.x;
        notches.x = k * ts.drag.notches.x;
    } else ts.vel.x = 0.f;
    if (ts.drag.step.y > 0.f) {
        ts.acc.y += d.y;
        const f32 k = std::round(ts.acc.y / ts.drag.step.y);
        ts.acc.y -= k * ts.drag.step.y;
        notches.y = k * ts.drag.notches.y;
    } else ts.vel.y = 0.f;
    if (notches.x == 0.f && notches.y == 0.f) return;
    Event e; e.type = EventType::Wheel; e.pos = ts.down; e.wheel = notches; e.mods = m_mods;
    dispatch(ts.target.index, e);
}

bool UiContext::touchMove(Vec2 p, double t) {
    using Stage = TouchState::Stage;
    if (m_touch.stage == Stage::Idle) return pointerMove(p);
    if (m_drag.active) { // the finger carries a drag (long press / captured drag): no pan, no press
        m_pointer = p;
        return m_drag.external ? true : dragMoveTo(p);
    }
    if (t < 0.0) t = m_clock;
    touchSample(p, t);
    const bool pastSlop = std::hypot(p.x - m_touch.down.x, p.y - m_touch.down.y) >= m_touchCfg.slop;
    if (m_touch.stage == Stage::Pending && pastSlop)
        touchBeginPan(p);
    if (pastSlop) { // no longer a tap / long press; a press that did not become a pan may start a drag
        m_gesture.moved = true;
        if (m_touch.stage != Stage::Pan) m_gesture.dragOk = true;
    }
    if (m_touch.stage != Stage::Pan) {
        if (m_touch.pressed) return pointerMove(p);
        m_pointer = p;
        return true;
    }
    m_pointer = p;
    touchPanBy({p.x - m_touch.last.x, p.y - m_touch.last.y});
    m_touch.last = p;
    return true;
}

bool UiContext::touchUp(Vec2 p, double t) {
    using Stage = TouchState::Stage;
    TouchState& ts = m_touch;
    if (ts.stage == Stage::Idle) return false;
    if (t < 0.0) t = m_clock;
    const Stage was = ts.stage;
    ts.stage = Stage::Idle;
    m_gesture.longArmed = false;
    if (m_drag.active && !m_drag.external) { // drop what the finger carried
        dragFinish(p, true);
        m_pointer = {-1.f, -1.f};
        setHover(kNone);
        return true;
    }
    if (was != Stage::Pan) {
        const bool tap = ts.pressed && !m_gesture.moved;
        const bool used = ts.pressed ? pointerUp(p, MouseButton::Left) : true;
        if (tap) gestureTap(p, t);
        m_pointer = {-1.f, -1.f};
        setHover(kNone); // a lifted finger hovers nothing
        return used;
    }
    m_pointer = {-1.f, -1.f};
    touchPanBy({p.x - ts.last.x, p.y - ts.last.y});
    ts.last = p;
    touchSample(p, t);
    // release velocity: the oldest sample inside the window against the newest
    const u32 cnt = ts.sampleCount, K = TouchState::kSamples;
    const TouchState::Sample& newest = ts.samples[(ts.sampleHead + K - 1) % K];
    const TouchState::Sample* oldest = &newest;
    for (u32 k = 0; k < cnt; ++k) {
        const TouchState::Sample& s = ts.samples[(ts.sampleHead + K - cnt + k) % K];
        if (t - s.t <= double(m_touchCfg.sampleWindow) + 1e-6) { oldest = &s; break; }
    }
    ts.vel = {};
    if (const double dt = newest.t - oldest->t; dt > 1e-4) {
        const f32 mv = m_touchCfg.maxVelocity;
        ts.vel = {std::clamp(f32((newest.p.x - oldest->p.x) / dt), -mv, mv), std::clamp(f32((newest.p.y - oldest->p.y) / dt), -mv, mv)};
    }
    touchPanBy({}); // drops the velocity along axes that cannot move (at an extent / not scrollable)
    ts.fling = node(ts.target) && std::hypot(ts.vel.x, ts.vel.y) >= m_touchCfg.minVelocity;
    return true;
}

bool UiContext::touchCancel() {
    using Stage = TouchState::Stage;
    gestureReset();
    if (m_touch.stage == Stage::Idle) return false;
    m_touch.stage = Stage::Idle;
    if (m_drag.active && !m_drag.external) cancelDrag();
    if (m_scrollDrag.node.valid()) {
        if (node(m_scrollDrag.node)) markPaint(m_scrollDrag.node.index);
        m_scrollDrag = {};
    }
    cancelPress(m_pointer);
    m_pointer = {-1.f, -1.f};
    setHover(kNone);
    return true;
}

bool UiContext::keyDown(Key k, u8 mods) {
    if (m_drag.active && !m_drag.external && k == Key::Escape) { // Escape cancels a drag
        cancelDrag();
        return true;
    }
    Event e; e.type = EventType::KeyDown; e.key = k; e.mods = mods;
    if (node(m_focus)) {
        if (dispatch(m_focus.index, e)) return true;
        if ((k == Key::Enter || k == Key::Space) && node(m_focus) && m_registry.get(m_nodes[m_focus.index].cls).interactive &&
            !(m_nodes[m_focus.index].state & kStateDisabled)) {
            m_focusVisible = true;
            const NodeHandle f = m_focus;
            Event c; c.type = EventType::Click;
            if (!dispatch(f.index, c) && node(f)) emit(f, SignalKind::Clicked, {});
            return true;
        }
    }
    if (m_shortcutHook.fn && m_shortcutHook.fn(m_shortcutHook.user, *this, k, mods)) return true; // actions.hpp
    if (k == Key::Tab) return focusNext((mods & kModShift) != 0);
    if (m_spatialNav || !node(m_focus)) {
        switch (k) {
        case Key::Left: return navigate(NavDir::Left);
        case Key::Right: return navigate(NavDir::Right);
        case Key::Up: return navigate(NavDir::Up);
        case Key::Down: return navigate(NavDir::Down);
        default: break;
        }
    }
    return node(m_focus) != nullptr && m_registry.get(m_nodes[m_focus.index].cls).focusable;
}

bool UiContext::keyUp(Key k, u8 mods) {
    if (!node(m_focus)) return false;
    Event e; e.type = EventType::KeyUp; e.key = k; e.mods = mods;
    return dispatch(m_focus.index, e);
}

bool UiContext::textInput(std::string_view utf8) {
    if (!node(m_focus)) return false;
    Event e; e.type = EventType::Text; e.text = utf8;
    return dispatch(m_focus.index, e);
}

bool UiContext::textComposition(std::string_view utf8, i32 caret) {
    if (!node(m_focus)) return false;
    m_composition.assign(utf8);
    m_compositionCaret = std::clamp(caret, 0, i32(m_composition.size()));
    Event e; e.type = EventType::Composition; e.text = m_composition; e.caret = m_compositionCaret;
    return dispatch(m_focus.index, e);
}

bool UiContext::setClipboardText(std::string_view utf8) {
    if (m_clipboard.set) return m_clipboard.set(m_clipboard.user, utf8);
    m_localClipboard.assign(utf8);
    return true;
}

bool UiContext::clipboardText(std::string& out) const {
    if (m_clipboard.get) return m_clipboard.get(m_clipboard.user, out);
    out = m_localClipboard;
    return !out.empty();
}

// ---- Focus ---------------------------------------------------------------------------------------------------------
bool UiContext::focusableNode(u32 i) const {
    if (i >= m_nodes.size() || !m_nodes[i].alive) return false;
    const Node& n = m_nodes[i];
    if (!(n.flags & kFlagFocusable) || n.tabIndex < 0 || (n.state & kStateDisabled)) return false;
    for (u32 a = i; a != kNone; a = m_nodes[a].parent)
        if (m_nodes[a].state & kStateDisabled) return false;
    return visibleChain(i);
}
u32 UiContext::scopeOf(u32 i) const {
    if (node(m_modal)) return m_modal.index;
    for (u32 a = m_nodes[i].parent; a != kNone; a = m_nodes[a].parent)
        if (m_nodes[a].flags & kFlagFocusScope) return a;
    return m_root.index;
}
bool UiContext::inScope(u32 i, u32 scope) const {
    for (u32 a = i; a != kNone; a = m_nodes[a].parent)
        if (a == scope) return true;
    return false;
}

void UiContext::collectTab(std::vector<u32>& out) const {
    out.clear();
    const u32 scope = node(m_modal) ? m_modal.index : (node(m_focus) ? scopeOf(m_focus.index) : m_root.index);
    auto& tab = const_cast<std::vector<std::pair<u32, i32>>&>(m_scratchTab);
    tab.clear();
    // pre-order walk without recursion
    u32 i = scope;
    while (i != kNone) {
        if (focusableNode(i) && i != scope) tab.emplace_back(i, m_nodes[i].tabIndex);
        if (m_nodes[i].first != kNone && m_styles.style(m_nodes[i].style).visible) { i = m_nodes[i].first; continue; }
        while (i != kNone && i != scope && m_nodes[i].next == kNone) i = m_nodes[i].parent;
        if (i == kNone || i == scope) break;
        i = m_nodes[i].next;
    }
    std::stable_sort(tab.begin(), tab.end(), [](const auto& a, const auto& b) {
        const i32 ka = a.second > 0 ? a.second : 0x7FFFFFFF, kb = b.second > 0 ? b.second : 0x7FFFFFFF;
        return ka < kb;
    });
    for (const auto& t : tab) out.push_back(t.first);
}

void UiContext::tabOrder(std::vector<NodeHandle>& out) const {
    out.clear();
    auto& tmp = const_cast<std::vector<u32>&>(m_scratchOrder);
    collectTab(tmp);
    for (u32 i : tmp) out.push_back({i, m_nodes[i].gen});
}

bool UiContext::focusNext(bool backward) {
    collectTab(m_scratchOrder);
    if (m_scratchOrder.empty()) return false;
    std::size_t at = m_scratchOrder.size();
    for (std::size_t k = 0; k < m_scratchOrder.size(); ++k)
        if (node(m_focus) && m_scratchOrder[k] == m_focus.index) at = k;
    std::size_t nx;
    const std::size_t sz = m_scratchOrder.size();
    if (at == sz) nx = backward ? sz - 1 : 0;
    else nx = backward ? (at + sz - 1) % sz : (at + 1) % sz;
    setFocus(handleOf(m_scratchOrder[nx]), FocusReason::Keyboard);
    return true;
}

bool UiContext::navigate(NavDir d) {
    collectTab(m_scratchOrder);
    if (m_scratchOrder.empty()) return false;
    if (!node(m_focus)) { setFocus(handleOf(m_scratchOrder[0]), FocusReason::Keyboard); return true; }
    const Rect fr = m_nodes[m_focus.index].rect;
    const Vec2 fc{fr.x + fr.w * 0.5f, fr.y + fr.h * 0.5f};
    u32 best = kNone;
    f32 bestScore = kInf;
    for (u32 i : m_scratchOrder) {
        if (i == m_focus.index) continue;
        const Rect r = m_nodes[i].rect;
        const f32 dx = r.x + r.w * 0.5f - fc.x, dy = r.y + r.h * 0.5f - fc.y;
        f32 along, across;
        switch (d) {
        case NavDir::Left: along = -dx; across = dy; break;
        case NavDir::Right: along = dx; across = dy; break;
        case NavDir::Up: along = -dy; across = dx; break;
        default: along = dy; across = dx; break;
        }
        if (along <= 0.5f) continue;
        const f32 score = along + 2.f * std::abs(across);
        if (score < bestScore) { bestScore = score; best = i; }
    }
    if (best == kNone) return false;
    setFocus(handleOf(best), FocusReason::Keyboard);
    return true;
}

void UiContext::setFocus(NodeHandle h, FocusReason why) {
    if (h.valid() && !node(h)) h = {};
    if (h.valid() && node(m_modal) && !inScope(h.index, m_modal.index)) return;
    const bool vis = why == FocusReason::Keyboard;
    if (h == m_focus) {
        if (node(h) && vis != m_focusVisible) { m_focusVisible = vis; setState(h, kStateFocusVisible, vis); }
        return;
    }
    const NodeHandle old = m_focus;
    m_focus = h;
    m_focusVisible = vis;
    m_composition.clear(); // pre-edit belongs to the old field; the IME is reset by the platform glue
    m_compositionCaret = 0;
    m_caretNode = {};
    if (node(old)) {
        setState(old, kStateFocus | kStateFocusVisible, false);
        Event e; e.type = EventType::FocusOut; e.focusReason = why;
        dispatch(old.index, e);
        if (node(old)) emit(old, SignalKind::FocusChanged, {.flag = false});
    }
    if (node(h)) {
        setState(h, kStateFocus, true);
        setState(h, kStateFocusVisible, vis);
        Event e; e.type = EventType::FocusIn; e.focusReason = why;
        dispatch(h.index, e);
        if (node(h)) emit(h, SignalKind::FocusChanged, {.flag = true});
    }
}

void UiContext::setModal(NodeHandle scope) {
    m_modal = node(scope) ? scope : NodeHandle{};
    if (node(m_modal) && !(node(m_focus) && inScope(m_focus.index, m_modal.index))) {
        setFocus({}, FocusReason::Programmatic);
        focusNext(false);
    }
}

// ---- Signals -------------------------------------------------------------------------------------------------------
ConnectionHandle UiContext::connect(NodeHandle h, SignalKind kind, Slot slot) {
    if (!node(h) || !slot) return {};
    u32 i;
    if (!m_freeConnections.empty()) { i = m_freeConnections.back(); m_freeConnections.pop_back(); }
    else { i = u32(m_connections.size()); m_connections.emplace_back(); }
    Connection& c = m_connections[i];
    c.node = h.index;
    c.nodeGen = h.generation;
    c.kind = kind;
    c.slot = slot;
    ++c.gen;
    return {i, c.gen};
}
void UiContext::disconnect(ConnectionHandle h) {
    if (h.index >= m_connections.size() || m_connections[h.index].gen != h.generation) return;
    Connection& c = m_connections[h.index];
    c.node = kNone;
    c.slot = Slot{};
    ++c.gen;
    m_freeConnections.push_back(h.index);
}
void UiContext::emit(NodeHandle h, SignalKind kind, const SignalArgs& args) {
    const std::size_t count = m_connections.size();
    for (std::size_t i = 0; i < count; ++i) {
        const Connection& c = m_connections[i];
        if (c.node != h.index || c.nodeGen != h.generation || c.kind != kind || !c.slot) continue;
        const Slot s = c.slot;
        s(*this, h, args);
        if (!node(h)) return;
    }
}

// ---- Timers / animation --------------------------------------------------------------------------------------------
TimerHandle UiContext::startTimer(f32 seconds, bool repeat, InlineFn<void(UiContext&)> fn) {
    u32 i = 0;
    while (i < m_timers.size() && m_timers[i].active) ++i;
    if (i == m_timers.size()) m_timers.emplace_back();
    Timer& t = m_timers[i];
    t.interval = t.left = seconds;
    t.repeat = repeat;
    t.active = true;
    t.fn = fn;
    ++t.gen;
    return {i, t.gen};
}
void UiContext::cancelTimer(TimerHandle h) {
    if (h.index < m_timers.size() && m_timers[h.index].gen == h.generation) m_timers[h.index].active = false;
}
void UiContext::animate(NodeHandle h, AnimProp p, f32 to, f32 seconds, Ease e) {
    Node* n = node(h);
    if (!n) return;
    if (m_reducedMotion) seconds = 0.f; // lands on `to` at the next update
    f32 from = 0.f;
    switch (p) {
    case AnimProp::Opacity: from = n->animOpacity; break;
    case AnimProp::OffsetX: from = n->offset.x; break;
    case AnimProp::OffsetY: from = n->offset.y; break;
    case AnimProp::Value: from = n->value; break;
    }
    Anim* slot = nullptr;
    for (auto& a : m_anims)
        if (a.active && a.node == h && a.prop == p) { slot = &a; break; }
    if (!slot) {
        for (auto& a : m_anims)
            if (!a.active) { slot = &a; break; }
        if (!slot) slot = &m_anims.emplace_back();
        slot->active = true;
        ++m_animCount;
    }
    *slot = Anim{h, p, e, from, to, 0.f, std::max(0.f, seconds), true};
}

// ---- Style motion: transitions, enter / exit, property animations --------------------------------------------------
// A node whose style is being animated points at a scratch slot of the style table (StyleEngine::acquireScratch):
// composeAnimated writes target + interpolated values into it each tick, so intermediate styles are never interned.
UiContext::StyleAnim* UiContext::styleAnimOf(NodeHandle h) {
    for (auto& a : m_styleAnims)
        if (a.active && a.node == h) return &a;
    return nullptr;
}

UiContext::StyleAnim& UiContext::ensureStyleAnim(u32 i) {
    Node& n = m_nodes[i];
    const NodeHandle h{i, n.gen};
    if (n.style != n.targetStyle)
        if (StyleAnim* a = styleAnimOf(h)) return *a;
    StyleAnim* slot = nullptr;
    for (auto& a : m_styleAnims)
        if (!a.active) { slot = &a; break; }
    if (!slot) slot = &m_styleAnims.emplace_back();
    *slot = StyleAnim{};
    slot->node = h;
    slot->active = true;
    slot->from = m_styles.style(n.style);
    slot->slot = m_styles.acquireScratch(slot->from);
    n.style = slot->slot;
    ++m_styleAnimCount;
    return *slot;
}

void UiContext::releaseStyleAnim(StyleAnim& a) {
    if (Node* n = node(a.node)) {
        n->style = n->targetStyle;
        n->flags &= ~u32(kFlagExiting);
    }
    m_styles.releaseScratch(a.slot);
    a.active = false;
    --m_styleAnimCount;
    if (a.busy) --m_styleAnimBusy;
    a.busy = false;
}

void UiContext::composeAnimated(StyleAnim& a) {
    const u32 i = a.node.index;
    Node& n = m_nodes[i];
    const ComputedStyle target = m_styles.style(n.targetStyle);
    ComputedStyle out = target;
    bool running = false;
    if (a.transitioning) {
        bool any = false;
        for (int p = 0; p < kPropCount; ++p) {
            if (!propInfo(Prop(p)).animatable) continue;
            const Transition* tr = findTransition(target, Prop(p));
            if (!tr || tr->duration <= 0.f || sameStyleProp(a.from, target, Prop(p))) continue;
            const f32 k = (a.t - tr->delay) / tr->duration;
            if (k >= 1.f) continue;
            any = true;
            lerpStyleProp(out, a.from, target, Prop(p), applyEasing(tr->ease, std::max(0.f, k)));
        }
        a.transitioning = any;
        running = any;
    }
    if (m_propAnimCount)
        for (const PropAnim& pa : m_propAnims) {
            if (!pa.active || pa.node != a.node || pa.keyCount < 2) continue;
            running = true;
            // position along the keys: a spring runs between the first and the last key (and may overshoot)
            u32 j = 0;
            f32 k;
            if (pa.spec.spring) {
                j = 0;
                k = pa.x;
            } else {
                const f32 tt = applyEasing(pa.spec.ease, pa.spec.duration > 0.f ? std::clamp((pa.t - pa.spec.delay) / pa.spec.duration, 0.f, 1.f) : 1.f);
                while (j + 2 < pa.keyCount && tt >= pa.keys[j + 1].at) ++j;
                const f32 span = pa.keys[j + 1].at - pa.keys[j].at;
                k = span > 1e-6f ? (tt - pa.keys[j].at) / span : 1.f;
            }
            ComputedStyle sa = target, sb = target;
            applyProp(sa, pa.prop, pa.keys[j].value);
            applyProp(sb, pa.prop, pa.keys[pa.spec.spring ? pa.keyCount - 1 : j + 1].value);
            lerpStyleProp(out, sa, sb, pa.prop, k);
        }
    bool busy = running; // time-dependent: a held keyframe animation keeps the slot without asking for frames
    if (m_keyAnimCount) {
        // CSS animations first, then scripted ones, each group in creation order (later ones win)
        for (int pass = 0; pass < 2; ++pass) {
            u64 after = 0;
            for (;;) {
                const KeyAnim* next = nullptr;
                for (const KeyAnim& ka : m_keyAnims)
                    if (ka.active && ka.node == a.node && ka.css == (pass == 0) && ka.serial > after && (!next || ka.serial < next->serial)) next = &ka;
                if (!next) break;
                after = next->serial;
                if (applyKeyAnim(*next, out)) running = true;
            }
        }
    }
    if (a.exiting) {
        running = busy = true;
        out.visible = true; // stays laid out and painted until the exit animation is over
    }
    if (busy != a.busy) {
        a.busy = busy;
        m_styleAnimBusy += busy ? 1u : ~0u;
    }
    const ComputedStyle old = m_styles.style(n.style);
    if (!running) {
        releaseStyleAnim(a);
        if (!(old == target)) applyStyleDiff(i, old, target);
        return;
    }
    if (!(old == out)) {
        m_styles.scratch(a.slot) = out;
        applyStyleDiff(i, old, out);
    }
}

void UiContext::tickStyleAnims(f32 dt) {
    for (auto& pa : m_propAnims) {
        if (!pa.active) continue;
        if (!node(pa.node)) { pa.active = false; --m_propAnimCount; continue; }
        pa.t += dt;
        bool done = m_reducedMotion;
        if (pa.spec.spring) {
            f32 left = std::max(0.f, std::min(dt, pa.t - pa.spec.delay));
            while (left > 0.f) { // semi-implicit Euler, <= 4 ms steps: stable for stiff springs at any frame rate
                const f32 h = std::min(left, 1.f / 240.f);
                const f32 acc = (-pa.spec.stiffness * (pa.x - 1.f) - pa.spec.damping * pa.v) / std::max(1e-3f, pa.spec.mass);
                pa.v += acc * h;
                pa.x += pa.v * h;
                left -= h;
            }
            if ((std::abs(pa.x - 1.f) < 0.001f && std::abs(pa.v) < 0.01f) || pa.t - pa.spec.delay > 20.f) done = true;
        } else if (pa.t - pa.spec.delay >= pa.spec.duration) {
            if (pa.loop && pa.spec.duration > 0.f && !m_reducedMotion) pa.t -= pa.spec.duration;
            else done = true;
        }
        if (done) {
            pa.active = false;
            --m_propAnimCount;
            if (pa.persist) set(pa.node, pa.prop, pa.keys[pa.keyCount - 1].value); // the end value stays as an inline value
        }
    }
    for (std::size_t k = 0; k < m_styleAnims.size(); ++k) {
        StyleAnim& a = m_styleAnims[k];
        if (!a.active) continue;
        Node* n = node(a.node);
        if (!n) {
            m_styles.releaseScratch(a.slot);
            a.active = false;
            --m_styleAnimCount;
            if (a.busy) --m_styleAnimBusy;
            a.busy = false;
            continue;
        }
        a.t += dt;
        if (m_reducedMotion) a.transitioning = false;
        if (a.exiting) {
            a.exitT += dt;
            const f32 k = (a.exitDur > 0.f && !m_reducedMotion) ? std::clamp(a.exitT / a.exitDur, 0.f, 1.f) : 1.f;
            const f32 e = applyEasing(Easing::In, k);
            n->animOpacity = a.exitFromOpacity * (1.f - e);
            markPaint(a.node.index);
            if (a.exitKind == MotionKind::SlideUp || a.exitKind == MotionKind::SlideDown) {
                n->offset.y = (a.exitKind == MotionKind::SlideUp ? -8.f : 8.f) * e;
                if (n->parent != kNone) markArrange(n->parent);
                n->dirty |= kDirtyLayout;
            }
            if (k >= 1.f) {
                a.exiting = false;
                n->flags &= ~u32(kFlagExiting);
                n->animOpacity = 1.f;
                n->offset.y = 0.f;
                if (n->parent != kNone) markArrange(n->parent);
            }
        }
        composeAnimated(a);
    }
}

void UiContext::startEnter(u32 i, MotionKind kind, f32 seconds) {
    Node& n = m_nodes[i];
    const NodeHandle h{i, n.gen};
    if (n.animOpacity >= 1.f) n.animOpacity = 0.f; // else: continues from an interrupted exit
    animate(h, AnimProp::Opacity, 1.f, seconds, Ease::Out);
    if (kind == MotionKind::SlideUp || kind == MotionKind::SlideDown) {
        if (n.offset.y == 0.f) n.offset.y = kind == MotionKind::SlideDown ? -8.f : 8.f; // slide-down: arrives from above
        if (n.parent != kNone) markArrange(n.parent);
        animate(h, AnimProp::OffsetY, 0.f, seconds, Ease::Out);
    }
}

void UiContext::animateProperty(NodeHandle h, Prop p, const PropValue& to, const AnimSpec& spec) {
    const PropKeyframe keys[2] = {{0.f, node(h) ? readProp(m_styles.style(node(h)->style), p) : PropValue{}}, {1.f, to}};
    if (!node(h)) return;
    if (m_reducedMotion || !propInfo(p).animatable || (!spec.spring && spec.duration <= 0.f && spec.delay <= 0.f)) {
        stopPropertyAnimations(h);
        set(h, p, to);
        return;
    }
    PropAnim* slot = nullptr;
    for (auto& a : m_propAnims)
        if (a.active && a.node == h && a.prop == p) { slot = &a; break; }
    if (!slot) {
        for (auto& a : m_propAnims)
            if (!a.active) { slot = &a; break; }
        if (!slot) slot = &m_propAnims.emplace_back();
        ++m_propAnimCount;
    }
    *slot = PropAnim{};
    slot->node = h;
    slot->prop = p;
    slot->keys[0] = keys[0];
    slot->keys[1] = keys[1];
    slot->keyCount = 2;
    slot->spec = spec;
    slot->active = true;
    ensureStyleAnim(h.index);
}

void UiContext::animateKeyframes(NodeHandle h, Prop p, const PropKeyframe* keys, u32 count, f32 seconds, Easing e, bool loop) {
    if (!node(h) || !keys || count == 0) return;
    count = std::min(count, kMaxKeys);
    if (m_reducedMotion || !propInfo(p).animatable || seconds <= 0.f || count < 2) {
        if (!loop || count < 2) set(h, p, keys[count - 1].value);
        return;
    }
    PropAnim* slot = nullptr;
    for (auto& a : m_propAnims)
        if (a.active && a.node == h && a.prop == p) { slot = &a; break; }
    if (!slot) {
        for (auto& a : m_propAnims)
            if (!a.active) { slot = &a; break; }
        if (!slot) slot = &m_propAnims.emplace_back();
        ++m_propAnimCount;
    }
    *slot = PropAnim{};
    slot->node = h;
    slot->prop = p;
    for (u32 k = 0; k < count; ++k) slot->keys[k] = keys[k];
    slot->keyCount = u8(count);
    slot->spec = AnimSpec::tween(seconds, e);
    slot->loop = loop;
    slot->persist = !loop;
    slot->active = true;
    ensureStyleAnim(h.index);
}

void UiContext::stopPropertyAnimations(NodeHandle h) {
    if (!node(h)) return;
    for (auto& a : m_propAnims)
        if (a.active && a.node == h) {
            a.active = false;
            --m_propAnimCount;
            set(h, a.prop, a.keys[a.keyCount - 1].value);
        }
}

bool UiContext::styleAnimating(NodeHandle h) const {
    const Node* n = node(h);
    return n && n->style != n->targetStyle;
}

void UiContext::setReducedMotion(bool on) {
    if (on == m_reducedMotion) return;
    m_reducedMotion = on;
    syncMedia();
    if (!on) return;
    for (auto& a : m_anims) // running tweens land on their end value at the next update
        if (a.active) a.duration = 0.f;
}

f32 UiContext::exitDuration(NodeHandle h) const {
    const Node* n = node(h);
    if (!n || m_reducedMotion) return 0.f;
    const ComputedStyle& s = m_styles.style(n->targetStyle);
    return s.exitAnim != MotionKind::None ? s.exitDuration : 0.f;
}

// ---- Cross-thread commands (Vyukov bounded queue; multi-producer, single consumer) -------------------------------
bool UiContext::post(CommandFn fn, NodeHandle n, u64 a, u64 b) {
    u64 pos = m_head.load(std::memory_order_relaxed);
    for (;;) {
        Command& c = m_queue[pos & (kQueueSize - 1)];
        const u64 seq = c.seq.load(std::memory_order_acquire);
        const i64 dif = i64(seq) - i64(pos);
        if (dif == 0) {
            if (m_head.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                c.fn = fn; c.node = n; c.a = a; c.b = b;
                c.seq.store(pos + 1, std::memory_order_release);
                return true;
            }
        } else if (dif < 0) {
            return false;
        } else {
            pos = m_head.load(std::memory_order_relaxed);
        }
    }
}
void UiContext::drainCommands() {
    for (;;) {
        Command& c = m_queue[m_tail & (kQueueSize - 1)];
        if (c.seq.load(std::memory_order_acquire) != m_tail + 1) return;
        const CommandFn fn = c.fn;
        const NodeHandle n = c.node;
        const u64 a = c.a, b = c.b;
        c.seq.store(m_tail + kQueueSize, std::memory_order_release);
        ++m_tail;
        if (fn) fn(*this, n, a, b);
    }
}

// ---- Style sheets --------------------------------------------------------------------------------------------------
std::expected<void, UiError> UiContext::addStyleSheet(std::string_view css, std::vector<StyleDiagnostic>* diags) {
    auto sheet = StyleSheet::parse(css, diags);
    if (!sheet) {
        if (diags) diags->push_back(sheet.error());
        return std::unexpected(UiError::StyleParse);
    }
    m_styles.addSheet(std::move(*sheet));
    m_anyStyleDirty = true;
    return {};
}
void UiContext::addStyleSheet(StyleSheet sheet) {
    m_styles.addSheet(std::move(sheet));
    m_anyStyleDirty = true;
}
void UiContext::clearStyleSheets() {
    m_styles.clearSheets();
    m_anyStyleDirty = true;
}

// ---- Dumps ---------------------------------------------------------------------------------------------------------
namespace {
void appendf(std::string& out, const char* fmt, auto... a) {
    char buf[256];
    const int n = std::snprintf(buf, sizeof buf, fmt, a...);
    if (n > 0) out.append(buf, std::size_t(std::min(n, int(sizeof buf) - 1)));
}
} // namespace

void UiContext::dumpLayout(std::string& out) const {
    auto rec = [&](auto&& self, u32 i, int depth) -> void {
        const Node& n = m_nodes[i];
        out.append(std::size_t(depth) * 2, ' ');
        out += m_registry.get(n.cls).name;
        if (!m_cold[i]->id.empty()) { out += '#'; out += m_cold[i]->id; }
        appendf(out, " %g %g %g %g\n", double(n.rect.x), double(n.rect.y), double(n.rect.w), double(n.rect.h));
        for (u32 k = n.first; k != kNone; k = m_nodes[k].next) self(self, k, depth + 1);
    };
    rec(rec, m_root.index, 0);
}

namespace {
bool a11yRanged(A11yRole r) {
    return r == A11yRole::Slider || r == A11yRole::ProgressBar || r == A11yRole::SpinBox || r == A11yRole::Dial || r == A11yRole::ScrollBar;
}
} // namespace

void UiContext::dumpA11y(std::string& out, bool actions) const {
    std::vector<A11ySnapshotNode> snap;
    snapshotA11y(snap);
    std::vector<u32> depth(snap.size(), 0);
    for (u32 i = 0; i < snap.size(); ++i) {
        const A11ySnapshotNode& s = snap[i];
        // (the root is listed only when it carries a role of its own, as before)
        const bool listed = !(i == 0 && node(s.node)->role == A11yRole::None);
        depth[i] = s.parent == ~0u ? 0u : depth[s.parent] + ((s.parent != 0 || node(snap[0].node)->role != A11yRole::None) ? 1u : 0u);
        if (!listed) continue;
        out.append(std::size_t(depth[i]) * 2, ' ');
        out += a11yRoleName(s.role);
        if (s.child) appendf(out, " #%u", s.child);
        if (!s.name.empty()) { out += " \""; out += s.name; out += '"'; }
        if (s.focusable) out += " focusable";
        if (s.focused) out += " focused";
        if (s.checked) out += " checked";
        if (s.disabled) out += " disabled";
        if (s.expandable) out += s.expanded ? " expanded" : " collapsed";
        if (s.selected) out += " selected";
        if (s.modal) out += " modal";
        if (s.live != A11yLive::Off) out += s.live == A11yLive::Polite ? " live=polite" : " live=assertive";
        if (!s.valueText.empty()) { out += " value=\""; out += s.valueText; out += '"'; }
        if (a11yRanged(s.role) && !s.child) appendf(out, " value=%g range=[%g,%g]", double(s.value), double(s.lo), double(s.hi));
        if (s.level) appendf(out, " level=%u", s.level);
        if (s.posInSet) appendf(out, " pos=%u/%u", s.posInSet, s.setSize);
        if (actions && s.actions) {
            out += " actions=";
            bool first = true;
            for (u32 a = 0; a <= u32(A11yAction::ScrollIntoView); ++a)
                if (s.actions & (1u << a)) {
                    if (!first) out += ',';
                    out += a11yActionName(A11yAction(a));
                    first = false;
                }
        }
        out += '\n';
    }
}

void UiContext::snapshotA11y(std::vector<A11ySnapshotNode>& out) const {
    out.clear();
    std::vector<A11ySnapshotNode> items;
    auto rec = [&](auto&& self, u32 i, u32 parent) -> void {
        const Node& n = m_nodes[i];
        const ComputedStyle& cs = m_styles.style(n.style);
        if (!cs.visible) return;
        u32 p = parent;
        if (n.role != A11yRole::None || i == m_root.index) {
            const WidgetClass& wc = m_registry.get(n.cls);
            A11ySnapshotNode s;
            s.node = {i, n.gen};
            s.parent = parent;
            s.role = n.role == A11yRole::None ? A11yRole::Window : n.role;
            // a text field's text is its value, not its name
            if (s.role == A11yRole::TextField) {
                s.name = m_cold[i]->a11yName;
                s.valueText = m_cold[i]->text;
            } else {
                s.name = m_cold[i]->a11yName.empty() ? m_cold[i]->text : m_cold[i]->a11yName;
            }
            s.bounds = n.rect;
            s.focusable = (n.flags & kFlagFocusable) != 0;
            s.focused = m_focus.index == i && m_focus.generation == n.gen;
            s.checked = (n.state & kStateChecked) != 0;
            s.disabled = (n.state & kStateDisabled) != 0;
            s.value = n.value;
            s.lo = n.lo;
            s.hi = n.hi;
            s.live = m_cold[i]->a11yLive;
            s.modal = s.role == A11yRole::Dialog && node(m_modal) && inScope(i, m_modal.index);
            if (s.focusable) s.actions |= a11yActionBit(A11yAction::Focus);
            if (!s.disabled) {
                if (a11yRanged(s.role) && s.role != A11yRole::ProgressBar)
                    s.actions |= a11yActionBit(A11yAction::Increment) | a11yActionBit(A11yAction::Decrement) | a11yActionBit(A11yAction::SetValue);
                else if (s.role == A11yRole::TextField) s.actions |= a11yActionBit(A11yAction::SetValue);
                else if (wc.interactive && !a11yRanged(s.role)) s.actions |= a11yActionBit(A11yAction::Click);
                if (cs.overflow == Overflow::Scroll) {
                    const Rect c = contentRect(s.node);
                    if (n.extent.h > c.h + 0.5f) s.actions |= a11yActionBit(A11yAction::ScrollUp) | a11yActionBit(A11yAction::ScrollDown);
                    if (n.extent.w > c.w + 0.5f) s.actions |= a11yActionBit(A11yAction::ScrollLeft) | a11yActionBit(A11yAction::ScrollRight);
                }
            }
            items.clear();
            if (wc.a11y) wc.a11y(*this, s.node, s, items);
            p = u32(out.size());
            out.push_back(std::move(s));
            for (A11ySnapshotNode& it : items) { // the class's visible items, as virtual children
                it.node = {i, n.gen};
                it.parent = it.parent == ~0u ? p : p + 1u + it.parent;
                out.push_back(std::move(it));
            }
        }
        for (u32 k = n.first; k != kNone; k = m_nodes[k].next) self(self, k, p);
    };
    rec(rec, m_root.index, ~0u);
}

// ---- Mouse cursor --------------------------------------------------------------------------------------------------
Cursor UiContext::cursorAt(NodeHandle n, Vec2 p) {
    if (!node(n)) return Cursor::Arrow;
    for (u32 a = n.index; a != kNone; a = m_nodes[a].parent) {
        const WidgetClass& wc = m_registry.get(m_nodes[a].cls);
        if (wc.cursor) {
            const Cursor c = wc.cursor(*this, {a, m_nodes[a].gen}, p);
            if (c != Cursor::Auto) return c;
        }
        const Cursor c = m_styles.style(m_nodes[a].style).cursor;
        if (c != Cursor::Auto) return c;
    }
    return Cursor::Arrow;
}
Cursor UiContext::cursor() {
    if (m_cursorOverride != Cursor::Auto) return m_cursorOverride;
    if (node(m_scrollDrag.node)) return Cursor::Arrow;
    if (node(m_pressed)) return cursorAt(m_pressed, m_pointer);
    return cursorAt(m_hover, m_pointer);
}

bool UiContext::performA11yAction(NodeHandle h, A11yAction action, f32 value, u32 child, std::string_view text) {
    Node* n = node(h);
    if (!n || !visibleChain(h.index)) return false;
    const u32 i = h.index;
    if ((n->state & kStateDisabled) && action != A11yAction::ScrollIntoView) return false;
    if (action == A11yAction::ScrollIntoView && !child) {
        // every scrolling ancestor moves just far enough to show the node (layout must be current: call after update)
        Rect r = n->rect;
        bool moved = false;
        for (u32 a = n->parent; a != kNone; a = m_nodes[a].parent) {
            if (m_styles.style(m_nodes[a].style).overflow != Overflow::Scroll) continue;
            const NodeHandle ah{a, m_nodes[a].gen};
            const Rect c = contentRect(ah);
            Vec2 sc = m_nodes[a].scroll;
            const Vec2 before = sc;
            if (r.y < c.y) sc.y -= c.y - r.y;
            else if (r.bottom() > c.bottom()) sc.y += std::min(r.bottom() - c.bottom(), r.y - c.y);
            if (r.x < c.x) sc.x -= c.x - r.x;
            else if (r.right() > c.right()) sc.x += std::min(r.right() - c.right(), r.x - c.x);
            scrollTo(ah, sc);
            const Vec2 d{m_nodes[a].scroll.x - before.x, m_nodes[a].scroll.y - before.y};
            moved = moved || d.x != 0.f || d.y != 0.f;
            r.x -= d.x;
            r.y -= d.y;
        }
        return moved || visibleChain(i);
    }
    const WidgetClass& wc = m_registry.get(n->cls);
    if (wc.a11yAction && wc.a11yAction(*this, h, child, action, value, text)) return true;
    if (child) return false; // virtual items are the class's business
    n = node(h);
    if (!n) return false;
    switch (action) {
    case A11yAction::Focus:
        if (!focusableNode(i)) return false;
        setFocus(h, FocusReason::Keyboard);
        return m_focus == h;
    case A11yAction::Click: {
        if (!wc.interactive) return false;
        const Vec2 c{n->rect.x + n->rect.w * 0.5f, n->rect.y + n->rect.h * 0.5f};
        const NodeHandle hit = hitTest(c);
        bool direct = false; // the centre really hits this node (not covered, not scrolled away, not behind a modal)
        for (u32 a = hit.valid() ? hit.index : kNone; a != kNone; a = m_nodes[a].parent)
            if (a == i) { direct = interactiveAncestor(hit.index) == i; break; }
        if (direct && node(m_modal) && !inScope(i, m_modal.index)) return false;
        if (direct) {
            pointerMove(c);
            pointerDown(c);
            pointerUp(c);
            return true;
        }
        if (node(m_modal) && !inScope(i, m_modal.index)) return false;
        if (focusableNode(i)) setFocus(h, FocusReason::Keyboard);
        Event e; e.type = EventType::Click; e.pos = c;
        if (!dispatch(i, e) && node(h)) emit(h, SignalKind::Clicked, {});
        return true;
    }
    case A11yAction::Increment:
    case A11yAction::Decrement: {
        if (!a11yRanged(n->role) || n->role == A11yRole::ProgressBar) return false;
        f32 step = n->step > 0.f ? n->step : (n->hi - n->lo) * 0.01f;
        if (step <= 0.f) step = 1.f;
        setValue(h, n->value + (action == A11yAction::Increment ? step : -step), true);
        return true;
    }
    case A11yAction::SetValue:
        if (n->role == A11yRole::TextField) {
            Cold& c = *m_cold[i];
            if (c.text == text) return true;
            setText(h, text);
            if (c.edit) { c.edit->setText(text); c.edit->end(false); }
            emit(h, SignalKind::TextChanged, {.text = text});
            return true;
        }
        if (!a11yRanged(n->role) || n->role == A11yRole::ProgressBar) return false;
        setValue(h, value, true);
        return true;
    case A11yAction::ScrollUp: case A11yAction::ScrollDown: case A11yAction::ScrollLeft: case A11yAction::ScrollRight: {
        const bool vertical = action == A11yAction::ScrollUp || action == A11yAction::ScrollDown;
        const f32 dir = (action == A11yAction::ScrollDown || action == A11yAction::ScrollRight) ? 1.f : -1.f;
        if (m_styles.style(n->style).overflow == Overflow::Scroll) {
            const Rect c = contentRect(h);
            const Vec2 before = n->scroll;
            scrollTo(h, vertical ? Vec2{before.x, before.y + dir * c.h * 0.5f} : Vec2{before.x + dir * c.w * 0.5f, before.y});
            return !(node(h)->scroll == before);
        }
        Event e; e.type = EventType::Wheel; // self-scrolling views (list, table, text) scroll on wheel notches
        e.pos = {n->rect.x + n->rect.w * 0.5f, n->rect.y + n->rect.h * 0.5f};
        e.wheel = vertical ? Vec2{0.f, -dir} : Vec2{-dir, 0.f};
        return dispatch(i, e);
    }
    default: return false; // Expand / Collapse / Select: only classes that have them
    }
}

// ====================================================================================================================
// Style engine extensions: attributes, selector invalidation, @media / @container upkeep, positioned layout
// (relative / absolute / fixed / sticky, z order), effects (transform / filter / clip-path / mask groups, transformed
// hit testing) and keyframe animations (CSS `animation`, UiContext::animate).
// ====================================================================================================================

// ---- Attributes ----------------------------------------------------------------------------------------------------
void UiContext::setAttribute(NodeHandle h, std::string_view name, std::string_view value) {
    if (!node(h) || name.empty()) return;
    const u32 key = hashId(name);
    auto& attrs = m_cold[h.index]->attrs;
    bool found = false;
    for (auto& [k, v] : attrs)
        if (k == key) {
            if (v == value) return;
            v.assign(value);
            found = true;
        }
    if (!found) attrs.emplace_back(key, std::string(value));
    markStyle(h.index);
    for (u32 k = m_nodes[h.index].first; k != kNone; k = m_nodes[k].next) markStyle(k);
    if (m_styles.features()) invalidateStyleFor(h.index, m_styles.attributeInvalidation(key));
}
void UiContext::removeAttribute(NodeHandle h, std::string_view name) {
    if (!node(h)) return;
    const u32 key = hashId(name);
    auto& attrs = m_cold[h.index]->attrs;
    if (std::erase_if(attrs, [&](const auto& kv) { return kv.first == key; }) == 0) return;
    markStyle(h.index);
    for (u32 k = m_nodes[h.index].first; k != kNone; k = m_nodes[k].next) markStyle(k);
    if (m_styles.features()) invalidateStyleFor(h.index, m_styles.attributeInvalidation(key));
}
bool UiContext::attribute(NodeHandle h, std::string_view name, std::string_view& out) const {
    if (!node(h)) return false;
    return ContextSubject(*this, m_nodes).attribute(h.index, hashId(name), out);
}

// ---- Selector invalidation -----------------------------------------------------------------------------------------
// Marks the descendants of `i` down to `depth` levels for restyling.
void UiContext::markStyleSubtree(u32 i, u32 depth) {
    if (!depth) return;
    u32 level = 1;
    u32 k = m_nodes[i].first;
    while (k != kNone) {
        markStyle(k);
        if (level < depth && m_nodes[k].first != kNone) {
            k = m_nodes[k].first;
            ++level;
            continue;
        }
        while (m_nodes[k].next == kNone) {
            k = m_nodes[k].parent;
            --level;
            if (k == i || k == kNone) return;
        }
        k = m_nodes[k].next;
    }
}

// What a class / state / attribute change of `i` reaches beyond the node and its children (the callers mark those).
void UiContext::invalidateStyleFor(u32 i, const StyleInvalidation& inv) {
    if (inv.descend > 1) markStyleSubtree(i, inv.descend);
    if (inv.siblings)
        for (u32 k = m_nodes[i].next; k != kNone; k = m_nodes[k].next) {
            markStyle(k);
            if (inv.descend) markStyleSubtree(k, inv.descend);
        }
    if (inv.has) {
        if (m_styles.features() & kStyleHasDeep) { // :has() left of a subject: anything may depend on it
            for (auto& n : m_nodes)
                if (n.alive) n.dirty |= kDirtyStyle;
            m_anyStyleDirty = true;
            return;
        }
        for (u32 a = i; a != kNone; a = m_nodes[a].parent) { // the anchors: ancestors and earlier siblings on the way up
            markStyle(a);
            for (u32 p = m_nodes[a].prev; p != kNone; p = m_nodes[p].prev) markStyle(p);
        }
    }
}

// A child was added to / removed from / moved under `parent` (`child`: the new node, kNone for a removal).
void UiContext::structureChanged(u32 parent, u32 child) {
    const u32 feat = m_styles.features();
    if (feat & (kStyleStructural | kStyleSibling)) {
        markStyle(parent); // :empty
        for (u32 k = m_nodes[parent].first; k != kNone; k = m_nodes[k].next) {
            markStyle(k);
            if (feat & kStyleStructDeep) markStyleSubtree(k, 255);
        }
    }
    if (feat & kStyleHas) {
        StyleInvalidation inv;
        inv.has = true;
        markStyle(parent);
        invalidateStyleFor(child != kNone ? child : parent, inv);
    }
}

// ---- @media / @container -------------------------------------------------------------------------------------------
void UiContext::syncMedia() {
    MediaEnv env;
    env.width = m_viewport.w;
    env.height = m_viewport.h;
    env.dark = m_mediaDark;
    env.reducedMotion = m_reducedMotion;
    env.hover = m_mediaHover;
    env.pointer = m_mediaPointer;
    env.resolution = m_renderScale;
    m_styles.setMediaEnv(env);
}
void UiContext::setMediaPreferences(bool dark, bool hover, u8 pointer) {
    m_mediaDark = dark;
    m_mediaHover = hover;
    m_mediaPointer = std::min<u8>(pointer, 2);
    syncMedia();
}

// Restyles the subtree of every query container whose content box changed since its subtree was last styled.
bool UiContext::settleContainers() {
    bool changed = false;
    for (ContainerEntry& c : m_containers) {
        const Node& n = m_nodes[c.node];
        if (!n.alive) continue;
        const ComputedStyle& s = m_styles.style(n.targetStyle);
        const Size now{std::max(0.f, n.rect.w - s.padding.h()), std::max(0.f, n.rect.h - s.padding.v())};
        if (now == c.size) continue;
        c.size = now;
        if (!m_styles.usesContainerQueries()) continue;
        markStyleSubtree(c.node, 255);
        changed = changed || m_nodes[c.node].first != kNone;
    }
    return changed;
}

// Keeps the per-node flags and side lists that follow from the style on screen.
void UiContext::syncStyleFlags(u32 i, const ComputedStyle& s) {
    Node& n = m_nodes[i];
    const bool fx = s.hasEffects(), oof = outOfFlow(s), cont = s.containerType != 0;
    if (fx != ((n.flags & kFlagFx) != 0)) {
        n.flags ^= kFlagFx;
        if (fx) ++m_fxCount;
        else {
            --m_fxCount;
            for (std::size_t k = 0; k < m_fxBounds.size(); ++k)
                if (m_fxBounds[k].first == i) { // what the effect covered is repainted
                    m_fxPendingDamage = m_fxCount ? Rect{0.f, 0.f, m_viewport.w, m_viewport.h} : m_fxPendingDamage.unite(m_fxBounds[k].second);
                    m_fxBounds[k] = m_fxBounds.back();
                    m_fxBounds.pop_back();
                    break;
                }
        }
    }
    if (oof != ((n.flags & kFlagOutOfFlow) != 0)) {
        n.flags ^= kFlagOutOfFlow;
        if (oof) m_outOfFlow.push_back(i);
        else std::erase(m_outOfFlow, i);
    }
    if (cont != ((n.flags & kFlagContainer) != 0)) {
        n.flags ^= kFlagContainer;
        if (cont) m_containers.push_back({i, {-1.f, -1.f}});
        else {
            std::erase_if(m_containers, [&](const ContainerEntry& c) { return c.node == i; });
            if (m_styles.usesContainerQueries()) markStyleSubtree(i, 255);
        }
    }
    if ((s.position != PositionKind::Static || s.zIndex != kZAuto) && n.parent != kNone) m_nodes[n.parent].flags |= kFlagZKids;
}

// ---- Positioned layout ---------------------------------------------------------------------------------------------
// position: absolute / fixed child `k` of `parent`: placed against the padding box of the nearest positioned
// ancestor (absolute; the viewport when there is none) or the viewport (fixed). Without insets on an axis it sits at
// the start of its parent's content box.
void UiContext::placeOutOfFlow(u32 parent, u32 k) {
    const Node& kn = m_nodes[k];
    const ComputedStyle& cs = m_styles.style(kn.style);
    if (!cs.visible) return;
    Rect cb{0.f, 0.f, m_viewport.w, m_viewport.h};
    if (cs.position == PositionKind::Absolute)
        for (u32 a = parent; a != kNone; a = m_nodes[a].parent)
            if (m_styles.style(m_nodes[a].style).position != PositionKind::Static) {
                cb = m_nodes[a].rect;
                cb.x -= m_nodes[a].scroll.x; // scrolls with the content of its containing block
                cb.y -= m_nodes[a].scroll.y;
                break;
            }
    const Node& pn = m_nodes[parent];
    const Rect pc = contentRect({parent, pn.gen});
    const Size m = measure(k, {cb.w - cs.margin.h(), cb.h - cs.margin.v()});
    const bool hasL = !cs.left.isAuto(), hasR = !cs.right.isAuto(), hasT = !cs.top.isAuto(), hasB = !cs.bottom.isAuto();
    const f32 l = cs.left.resolve(cb.w, 0.f), r = cs.right.resolve(cb.w, 0.f), t = cs.top.resolve(cb.h, 0.f), b = cs.bottom.resolve(cb.h, 0.f);
    f32 w = m.w, h = m.h;
    if (hasL && hasR && !fixedLen(cs.width, cb.w)) w = std::max(0.f, cb.w - l - r - cs.margin.h());
    if (hasT && hasB && !fixedLen(cs.height, cb.h)) h = std::max(0.f, cb.h - t - b - cs.margin.v());
    if (cs.aspectRatio > 0.f) {
        if (!fixedLen(cs.height, cb.h) && !(hasT && hasB)) h = w / cs.aspectRatio;
        else if (!fixedLen(cs.width, cb.w) && !(hasL && hasR)) w = h * cs.aspectRatio;
    }
    const f32 x = hasL ? cb.x + l + cs.margin.l : hasR ? cb.right() - r - cs.margin.r - w : pc.x - pn.scroll.x + cs.margin.l;
    const f32 y = hasT ? cb.y + t + cs.margin.t : hasB ? cb.bottom() - b - cs.margin.b - h : pc.y - pn.scroll.y + cs.margin.t;
    arrange(k, {std::round(x + kn.offset.x), std::round(y + kn.offset.y), std::round(w), std::round(h)});
}

// After the tree is arranged: out-of-flow nodes whose containing block changed without their parent being arranged
// again (a resized ancestor, a new viewport) are placed once more. A no-op for nodes already in place.
void UiContext::placeAllOutOfFlow() {
    for (std::size_t k = 0; k < m_outOfFlow.size(); ++k) {
        const u32 i = m_outOfFlow[k];
        if (m_nodes[i].alive && m_nodes[i].parent != kNone && visibleChain(m_nodes[i].parent)) placeOutOfFlow(m_nodes[i].parent, i);
    }
}

// position: sticky child `k` laid out at `r` inside `parent` (content box `pc`, scrolled coordinates): kept inside
// the scrollport of the nearest scrolling ancestor by its insets, for as long as the parent's content box has room.
Rect UiContext::stickyRect(u32 parent, u32 k, Rect r, const Rect& pc) const {
    const ComputedStyle& cs = m_styles.style(m_nodes[k].style);
    u32 sc = parent;
    while (sc != kNone && m_styles.style(m_nodes[sc].style).overflow != Overflow::Scroll) sc = m_nodes[sc].parent;
    if (sc == kNone) return r;
    const Rect port = contentRect({sc, m_nodes[sc].gen});
    const bool own = sc == parent; // the scroller is the containing block: no limit other than its content
    if (!cs.top.isAuto()) {
        const f32 stuck = std::max(r.y, port.y + cs.top.resolve(port.h, 0.f));
        r.y = own ? stuck : std::min(stuck, std::max(r.y, pc.bottom() - r.h));
    } else if (!cs.bottom.isAuto()) {
        const f32 stuck = std::min(r.y, port.bottom() - cs.bottom.resolve(port.h, 0.f) - r.h);
        r.y = own ? stuck : std::max(stuck, std::min(r.y, pc.y));
    }
    if (!cs.left.isAuto()) {
        const f32 stuck = std::max(r.x, port.x + cs.left.resolve(port.w, 0.f));
        r.x = own ? stuck : std::min(stuck, std::max(r.x, pc.right() - r.w));
    } else if (!cs.right.isAuto()) {
        const f32 stuck = std::min(r.x, port.right() - cs.right.resolve(port.w, 0.f) - r.w);
        r.x = own ? stuck : std::max(stuck, std::min(r.x, pc.x));
    }
    return r;
}

// Children of `i` in paint order on m_zScratch (from the returned index): z-index, then in-flow before positioned,
// then tree order. The caller truncates m_zScratch back to the returned index.
u32 UiContext::sortChildren(u32 i) const {
    const u32 base = u32(m_zScratch.size());
    i64 order = 0;
    for (u32 k = m_nodes[i].first; k != kNone; k = m_nodes[k].next) {
        const ComputedStyle& s = m_styles.style(m_nodes[k].style);
        const i64 z = s.zIndex == kZAuto ? 0 : s.zIndex;
        const i64 tier = z == 0 && (s.position != PositionKind::Static || s.zIndex != kZAuto) ? 1 : 0;
        m_zScratch.push_back({z * (i64(1) << 34) + tier * (i64(1) << 32) + order++, k});
    }
    std::sort(m_zScratch.begin() + base, m_zScratch.end(), [](const ZEntry& a, const ZEntry& b) { return a.key < b.key; });
    return base;
}

// ---- Effects -------------------------------------------------------------------------------------------------------
// Maps `p` from the space outside node `i` into its own (inverse transform); false when the point misses the node's
// clip-path or lies where its mask is transparent.
bool UiContext::mapIntoNode(u32 i, Vec2& p) const {
    const Node& n = m_nodes[i];
    const ComputedStyle& s = m_styles.style(n.style);
    if (!s.transform.identity()) {
        Affine inv;
        if (!styleTransform(s, n.rect).invert(inv)) return false;
        p = inv.apply(p);
    }
    if (!clipShapeContains(s.clipPath, n.rect, p)) return false;
    if (s.mask.kind != GradientSpecKind::None) {
        const Color c = gradientColorAt(s.mask, n.rect, p);
        const f32 m = s.maskLuminance ? (0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b) * c.a : c.a;
        if (m < 1.f / 255.f) return false;
    }
    return true;
}

Vec2 UiContext::toLocal(NodeHandle h, Vec2 p) const {
    if (!node(h) || !m_fxCount) return p;
    u32 chain[64];
    int depth = 0;
    for (u32 a = h.index; a != kNone && depth < 64; a = m_nodes[a].parent)
        if (m_nodes[a].flags & kFlagFx) chain[depth++] = a;
    while (depth-- > 0) { // outermost first
        const Node& n = m_nodes[chain[depth]];
        const ComputedStyle& s = m_styles.style(n.style);
        Affine inv;
        if (!s.transform.identity() && styleTransform(s, n.rect).invert(inv)) p = inv.apply(p);
    }
    return p;
}
Vec2 UiContext::fromLocal(NodeHandle h, Vec2 p) const {
    if (!node(h) || !m_fxCount) return p;
    for (u32 a = h.index; a != kNone; a = m_nodes[a].parent) {
        if (!(m_nodes[a].flags & kFlagFx)) continue;
        const ComputedStyle& s = m_styles.style(m_nodes[a].style);
        if (!s.transform.identity()) p = styleTransform(s, m_nodes[a].rect).apply(p);
    }
    return p;
}

namespace {
Rect grown(const Rect& r, f32 e) { return r.empty() ? r : Rect{r.x - e, r.y - e, r.w + 2.f * e, r.h + 2.f * e}; }
/// Bounds (list units, before DrawList::scale) of what commands [first, last) draw. `rs`: the scale device-pixel
/// glyph commands were recorded at.
Rect commandBounds(const DrawList& dl, std::size_t first, std::size_t last, f32 rs) {
    Rect out;
    const auto& cmds = dl.cmds();
    for (std::size_t k = first; k < last && k < cmds.size(); ++k) {
        const DrawCmd& c = cmds[k];
        Rect r;
        switch (c.op) {
        case DrawOp::PopClip:
        case DrawOp::PushClipPath:
        case DrawOp::PushLayer:
        case DrawOp::PushMask: continue;
        case DrawOp::PushClip: { // what is inside, cut by the clip
            const std::size_t end = std::min(matchingPop(dl, k), last);
            r = commandBounds(dl, k + 1, end, rs).intersect(c.rect);
            k = end;
            break;
        }
        case DrawOp::PushTransform: // its content lives in another space: the mapped bounds stand for it
            r = c.rect;
            k = std::min(matchingPop(dl, k), last);
            break;
        case DrawOp::PushFilter:
            r = grown(c.rect, 2.f);
            k = std::min(matchingPop(dl, k), last);
            break;
        case DrawOp::Shadow: r = grown(c.rect, c.blur * 2.f); break;
        case DrawOp::Line: {
            const f32 hw = c.borderWidth * 0.5f + 1.f;
            r = {std::min(c.rect.x, c.rect.w) - hw, std::min(c.rect.y, c.rect.h) - hw, std::abs(c.rect.w - c.rect.x) + 2.f * hw,
                 std::abs(c.rect.h - c.rect.y) + 2.f * hw};
            break;
        }
        case DrawOp::Glyph:
            r = c.devicePx ? Rect{c.rect.x / rs, c.rect.y / rs, f32(c.tw) / rs, f32(c.th) / rs} : Rect{c.rect.x, c.rect.y, f32(c.tw), f32(c.th)};
            break;
        default: r = c.rect; break;
        }
        out = out.unite(r);
    }
    return out;
}
/// How far a filter list spreads its content, in list units at any render scale.
f32 styleFilterReach(const FilterList& f) {
    f32 reach = 0.f;
    for (u8 k = 0; k < f.count; ++k) {
        if (f.fn[k].kind == FilterKind::Blur) reach += 3.f * f.fn[k].amount + 3.f;
        else if (f.fn[k].kind == FilterKind::DropShadow)
            reach += 3.f * f.fn[k].amount + 4.f + std::max(std::abs(f.shadowOffset.x), std::abs(f.shadowOffset.y));
    }
    return reach;
}
} // namespace

// Paints a node whose style has effects. Its commands (and its subtree's) are recorded inside groups, outermost
// first: transform, mask, clip-path, filter - so the result is transform(mask(clip(filter(content)))), the CSS order.
void UiContext::paintEffects(u32 i, f32 opacity, DrawList& dl) {
    Node& n = m_nodes[i];
    const ComputedStyle s = m_styles.style(n.style);
    const bool wasDirty = (n.dirty & kDirtyPaint) != 0;
    const Rect outerDamage = m_damage;
    m_damage = {};
    const std::size_t mark = dl.size();
    constexpr std::size_t kNoCmd = ~std::size_t(0);
    std::size_t xfCmd = kNoCmd, maskCmd = kNoCmd, clipCmd = kNoCmd, filterCmd = kNoCmd;
    if (!s.transform.identity()) {
        xfCmd = dl.size();
        dl.pushTransform(styleTransform(s, n.rect), n.rect);
    }
    if (s.mask.kind != GradientSpecKind::None) {
        GradientStop stops[kMaxGradientStops];
        maskCmd = dl.size();
        dl.pushMask(gradientOf(s.mask, n.rect, stops), n.rect, s.maskLuminance ? MaskMode::Luminance : MaskMode::Alpha);
    }
    if (s.clipPath.kind != ClipShapeKind::None) {
        thread_local Path path;
        path.clear();
        clipShapePath(s.clipPath, n.rect, path);
        clipCmd = dl.size();
        dl.pushClipPath(path, s.clipPath.evenOdd ? FillRule::EvenOdd : FillRule::NonZero);
    }
    if (s.filter.count) {
        FilterOp ops[kMaxFilters];
        for (u8 k = 0; k < s.filter.count; ++k) {
            ops[k] = {s.filter.fn[k].kind, s.filter.fn[k].amount, {}, {}};
            if (ops[k].kind == FilterKind::DropShadow) {
                ops[k].offset = s.filter.shadowOffset;
                ops[k].color = s.filter.shadowColor.withAlpha(s.filter.shadowColor.a * opacity);
            }
        }
        filterCmd = dl.size();
        dl.pushFilter(std::span<const FilterOp>(ops, s.filter.count), n.rect);
    }
    const std::size_t groups = dl.size() - mark;
    const std::size_t content = dl.size();
    ++m_fxDepth;
    paintBody(i, opacity, dl);
    --m_fxDepth;
    Rect bounds = commandBounds(dl, content, dl.size(), m_renderScale);
    const Rect plain = bounds; // where this content is without the effects
    Rect visual;
    if (bounds.empty()) {
        dl.cmds().resize(mark); // nothing drawn: no groups either
    } else {
        for (std::size_t k = 0; k < groups; ++k) dl.popClip();
        if (filterCmd != kNoCmd) {
            dl.setGroupBounds(filterCmd, bounds);
            bounds = grown(bounds, styleFilterReach(s.filter));
        }
        if (clipCmd != kNoCmd) bounds = bounds.intersect(dl.cmds()[clipCmd].rect);
        if (maskCmd != kNoCmd) dl.setGroupBounds(maskCmd, bounds);
        if (xfCmd != kNoCmd) {
            dl.setGroupBounds(xfCmd, bounds);
            visual = dl.cmds()[xfCmd].rect;
        } else visual = bounds;
        for (std::size_t k = mark; k < content; ++k) dl.cmds()[k].node = i;
    }
    // damage: the effect moves / spreads pixels, so any change below repaints everything the node covers, before and
    // now. Inside another effect group this lands in that group's own damage, which makes it do the same.
    const bool changed = wasDirty || !m_damage.empty();
    m_damage = outerDamage;
    Rect* last = nullptr;
    for (auto& e : m_fxBounds)
        if (e.first == i) last = &e.second;
    if (!last) { // the effect is new: until now the node was painted in place
        m_fxBounds.emplace_back(i, plain);
        last = &m_fxBounds.back().second;
    }
    if (changed || !(*last == visual)) m_damage = m_damage.unite(*last).unite(visual);
    *last = visual;
}

// ---- Keyframe animations -------------------------------------------------------------------------------------------
namespace {
double animEnd(const AnimationTiming& t) { return double(t.delay) + double(t.duration) * double(t.iterations) + double(t.endDelay); }
/// Progress within the iteration at `time` (0..1 after direction and the iteration easing); false: no effect now.
bool animProgress(const AnimationTiming& t, double time, f32& out) {
    const double local = time - double(t.delay);
    const double active = double(t.duration) * double(t.iterations);
    double overall;
    if (local < 0.0) {
        if (t.fill != AnimFill::Backwards && t.fill != AnimFill::Both) return false;
        overall = 0.0;
    } else if (local >= active) {
        if (t.fill != AnimFill::Forwards && t.fill != AnimFill::Both) return false;
        overall = double(t.iterations);
    } else {
        overall = t.duration > 0.f ? local / double(t.duration) : double(t.iterations);
    }
    if (!std::isfinite(overall)) overall = 0.0;
    double iter = std::floor(overall), frac = overall - iter;
    if (frac == 0.0 && overall > 0.0 && local >= active) { // the end of the last iteration, not the start of the next
        iter -= 1.0;
        frac = 1.0;
    }
    const bool odd = (i64(iter) & 1) != 0;
    const bool backwards = t.direction == AnimDirection::Reverse || (t.direction == AnimDirection::Alternate && odd) ||
                           (t.direction == AnimDirection::AlternateReverse && !odd);
    out = applyEasing(t.easing, f32(backwards ? 1.0 - frac : frac));
    return true;
}
} // namespace

UiContext::KeyAnim* UiContext::keyAnim(AnimationHandle a) {
    if (a.index >= m_keyAnims.size()) return nullptr;
    KeyAnim& ka = m_keyAnims[a.index];
    return ka.active && ka.gen == a.generation ? &ka : nullptr;
}
const UiContext::KeyAnim* UiContext::keyAnim(AnimationHandle a) const {
    if (a.index >= m_keyAnims.size()) return nullptr;
    const KeyAnim& ka = m_keyAnims[a.index];
    return ka.active && ka.gen == a.generation ? &ka : nullptr;
}

UiContext::KeyAnim& UiContext::newKeyAnim(NodeHandle n) {
    KeyAnim* slot = nullptr;
    for (auto& ka : m_keyAnims)
        if (!ka.active) { slot = &ka; break; }
    if (!slot) slot = &m_keyAnims.emplace_back();
    slot->keys.clear(); // keeps its capacity
    slot->node = n;
    slot->active = true;
    slot->css = false;
    slot->name = 0;
    slot->timing = {};
    slot->keyEase = Easing::Linear;
    slot->time = 0.0;
    slot->state = AnimPlayState::Running;
    slot->serial = ++m_keyAnimSerial;
    ++m_keyAnimCount;
    m_keyAnimDirty = true;
    return *slot;
}
void UiContext::endKeyAnim(KeyAnim& ka) {
    if (!ka.active) return;
    ka.active = false;
    ka.state = AnimPlayState::Idle;
    ++ka.gen;
    --m_keyAnimCount;
    m_keyAnimDirty = true;
}

// The keyframes of a CSS animation, from the @keyframes rule of its name in the current sheets.
void UiContext::buildCssKeys(KeyAnim& ka) {
    ka.keys.clear();
    ka.sheetVersion = m_styles.sheetVersion();
    const KeyframesRule* rule = m_styles.findKeyframes(ka.name);
    if (!rule) return;
    for (const KeyframeBlock& f : rule->frames)
        for (const Declaration& d : f.decls) {
            AnimKey k;
            if (!m_styles.resolveDeclaration(d, k.value)) continue;
            k.prop = d.prop;
            k.offset = f.offset;
            k.hasEase = f.hasEase;
            k.ease = f.ease;
            ka.keys.push_back(k);
        }
    std::stable_sort(ka.keys.begin(), ka.keys.end(), [](const AnimKey& a, const AnimKey& b) {
        return a.prop != b.prop ? u8(a.prop) < u8(b.prop) : a.offset < b.offset;
    });
}

// Applies the animation's current values on top of `out`; false when it has no effect at its current time.
bool UiContext::applyKeyAnim(const KeyAnim& ka, ComputedStyle& out) const {
    f32 p;
    if (ka.keys.empty() || !animProgress(ka.timing, ka.time, p)) return false;
    const std::size_t count = ka.keys.size();
    for (std::size_t g = 0; g < count;) {
        std::size_t e = g;
        while (e < count && ka.keys[e].prop == ka.keys[g].prop) ++e;
        const Prop prop = ka.keys[g].prop;
        // the keyframes around p; a missing first / last one is the underlying value
        const AnimKey* k0 = nullptr;
        const AnimKey* k1 = nullptr;
        for (std::size_t k = g; k < e; ++k) {
            if (ka.keys[k].offset <= p) k0 = &ka.keys[k];
            else { k1 = &ka.keys[k]; break; }
        }
        const f32 o0 = k0 ? k0->offset : 0.f, o1 = k1 ? k1->offset : 1.f;
        f32 t = o1 > o0 ? (p - o0) / (o1 - o0) : 1.f;
        t = applyEasing(k0 && k0->hasEase ? k0->ease : ka.keyEase, std::clamp(t, 0.f, 1.f));
        if (k0 && (!k1 && k0->offset >= 1.f)) t = 0.f; // at the last keyframe: its value
        if (propInfo(prop).animatable) {
            ComputedStyle sa = out, sb = out;
            if (k0) applyProp(sa, prop, k0->value);
            if (k1) applyProp(sb, prop, k1->value);
            lerpStyleProp(out, sa, sb, prop, t);
        } else if (const AnimKey* pick = t < 0.5f ? k0 : k1) { // discrete: switches half-way
            applyProp(out, prop, pick->value);
        }
        g = e;
    }
    return true;
}

// The CSS `animation` list of node `i` changed (or its visibility): start what is new, drop what is gone, retime
// what stayed.
void UiContext::syncCssAnimations(u32 i, const ComputedStyle& after) {
    const NodeHandle h{i, m_nodes[i].gen};
    const u8 count = after.visible ? after.animationCount : 0;
    for (auto& ka : m_keyAnims) {
        if (!ka.active || !ka.css || ka.node != h) continue;
        bool listed = false;
        for (u8 k = 0; k < count; ++k) listed = listed || after.animations[k].name == ka.name;
        if (!listed) endKeyAnim(ka);
    }
    bool effect = false;
    for (u8 k = 0; k < count; ++k) {
        const AnimationSpec& sp = after.animations[k];
        if (!sp.name) continue;
        KeyAnim* ka = nullptr;
        for (auto& x : m_keyAnims)
            if (x.active && x.css && x.node == h && x.name == sp.name) ka = &x;
        if (!ka) {
            ka = &newKeyAnim(h);
            ka->css = true;
            ka->name = sp.name;
            buildCssKeys(*ka);
        }
        ka->timing.duration = sp.duration;
        ka->timing.delay = sp.delay;
        ka->timing.iterations = sp.iterations;
        ka->timing.direction = sp.direction;
        ka->timing.fill = sp.fill;
        ka->timing.easing = Easing::Linear;
        ka->keyEase = sp.ease;
        if (sp.paused) ka->state = AnimPlayState::Paused;
        else if (ka->state == AnimPlayState::Paused) ka->state = AnimPlayState::Running;
        f32 p;
        effect = effect || animProgress(ka->timing, ka->time, p);
    }
    if (effect) ensureStyleAnim(i); // composed by the caller (styleRec), so the first frame already shows it
    m_keyAnimDirty = true;
}

void UiContext::tickKeyAnims(f32 dt) {
    m_keyAnimDirty = false;
    u32 running = 0;
    for (std::size_t k = 0; k < m_keyAnims.size(); ++k) {
        KeyAnim* ka = &m_keyAnims[k];
        if (!ka->active) continue;
        if (!node(ka->node)) { endKeyAnim(*ka); continue; }
        if (ka->css && ka->sheetVersion != m_styles.sheetVersion()) buildCssKeys(*ka);
        const double end = animEnd(ka->timing);
        const f32 rate = ka->timing.playbackRate;
        bool finished = false;
        if (ka->state == AnimPlayState::Running) {
            const bool endless = !std::isfinite(end) && rate >= 0.f;
            if (m_reducedMotion) { // no motion: straight to the end; an endless animation stays where it is
                if (!endless) ka->time = rate >= 0.f ? end : 0.0;
            } else ka->time += double(dt) * double(rate);
            if (!endless && (rate >= 0.f ? ka->time >= end : ka->time <= 0.0)) {
                ka->time = rate >= 0.f ? end : 0.0;
                ka->state = AnimPlayState::Finished;
                finished = true;
            } else if (!(m_reducedMotion && endless) && rate != 0.f) ++running;
        }
        const NodeHandle nh = ka->node;
        const u32 gen = ka->gen;
        f32 p;
        if (m_nodes[nh.index].style == m_nodes[nh.index].targetStyle && animProgress(ka->timing, ka->time, p))
            ensureStyleAnim(nh.index); // not animated yet: composed by tickStyleAnims
        if (finished) {
            emit(nh, SignalKind::AnimationFinished, {.value = f32(k), .flag = true, .value2 = f32(gen)});
            ka = &m_keyAnims[k]; // a slot may have started animations
            // a scripted animation that leaves nothing behind is done; CSS ones live as long as the style lists them
            if (ka->active && ka->gen == gen && ka->state == AnimPlayState::Finished && !ka->css &&
                !animProgress(ka->timing, ka->time, p))
                endKeyAnim(*ka);
        }
    }
    m_keyAnimRunning = running;
}

AnimationHandle UiContext::animate(NodeHandle n, std::span<const Keyframe> keys, const AnimationTiming& timing) {
    if (!node(n) || keys.empty()) return {};
    KeyAnim& ka = newKeyAnim(n);
    ka.timing = timing;
    ka.timing.duration = std::max(0.f, timing.duration);
    ka.timing.iterations = std::max(0.f, timing.iterations);
    for (const Keyframe& k : keys) {
        if (u8(k.prop) >= kPropCount) continue;
        AnimKey a;
        a.prop = k.prop;
        a.offset = k.offset < 0.f ? -1.f : std::min(k.offset, 1.f);
        a.ease = k.ease;
        a.hasEase = true;
        a.value = k.value;
        ka.keys.push_back(a);
    }
    auto byProp = [](const AnimKey& a, const AnimKey& b) { return u8(a.prop) < u8(b.prop); };
    std::stable_sort(ka.keys.begin(), ka.keys.end(), byProp);
    for (std::size_t g = 0; g < ka.keys.size();) { // offsets left out: spread between the neighbours, per property
        std::size_t e = g;
        while (e < ka.keys.size() && ka.keys[e].prop == ka.keys[g].prop) ++e;
        if (ka.keys[g].offset < 0.f) ka.keys[g].offset = e - g == 1 ? 1.f : 0.f; // a single keyframe is the end value
        if (ka.keys[e - 1].offset < 0.f) ka.keys[e - 1].offset = 1.f;
        for (std::size_t k = g + 1; k < e; ++k) {
            if (ka.keys[k].offset >= 0.f) { ka.keys[k].offset = std::max(ka.keys[k].offset, ka.keys[k - 1].offset); continue; }
            std::size_t j = k;
            while (ka.keys[j].offset < 0.f) ++j;
            const f32 a = ka.keys[k - 1].offset, b = std::max(ka.keys[j].offset, a);
            for (std::size_t x = k; x < j; ++x) ka.keys[x].offset = a + (b - a) * f32(x - k + 1) / f32(j - k + 1);
        }
        g = e;
    }
    const AnimationHandle handle{u32(&ka - m_keyAnims.data()), ka.gen};
    f32 p;
    if (animProgress(ka.timing, ka.time, p)) composeAnimated(ensureStyleAnim(n.index));
    return handle;
}

AnimationHandle UiContext::animateCss(NodeHandle n, std::string_view keyframes, const AnimationTiming& timing) {
    KeyframesRule rule;
    if (!node(n) || !parseKeyframes(keyframes, rule)) return {};
    std::vector<Keyframe> keys;
    for (const KeyframeBlock& f : rule.frames)
        for (const Declaration& d : f.decls) {
            Keyframe k;
            if (!m_styles.resolveDeclaration(d, k.value)) continue;
            k.prop = d.prop;
            k.offset = f.offset;
            k.ease = f.hasEase ? f.ease : Easing::Linear;
            keys.push_back(k);
        }
    return animate(n, keys, timing);
}

namespace {
/// After a change made through the API: the node's style follows at once (not only at the next update).
template <class Recompose>
void afterAnimChange(bool& dirty, Recompose&& recompose) {
    dirty = true;
    recompose();
}
} // namespace

void UiContext::playAnimation(AnimationHandle a) {
    KeyAnim* ka = keyAnim(a);
    if (!ka) return;
    if (ka->state == AnimPlayState::Finished) ka->time = ka->timing.playbackRate >= 0.f ? 0.0 : animEnd(ka->timing);
    if (!std::isfinite(ka->time)) ka->time = 0.0;
    ka->state = AnimPlayState::Running;
    afterAnimChange(m_keyAnimDirty, [&] { if (node(ka->node)) composeAnimated(ensureStyleAnim(ka->node.index)); });
}
void UiContext::pauseAnimation(AnimationHandle a) {
    KeyAnim* ka = keyAnim(a);
    if (!ka || ka->state == AnimPlayState::Paused) return;
    ka->state = AnimPlayState::Paused;
    m_keyAnimDirty = true;
}
void UiContext::reverseAnimation(AnimationHandle a) {
    KeyAnim* ka = keyAnim(a);
    if (!ka) return;
    ka->timing.playbackRate = -ka->timing.playbackRate;
    const double end = animEnd(ka->timing);
    if (ka->state == AnimPlayState::Finished || !std::isfinite(ka->time)) ka->time = ka->timing.playbackRate >= 0.f ? 0.0 : (std::isfinite(end) ? end : 0.0);
    ka->state = AnimPlayState::Running;
    afterAnimChange(m_keyAnimDirty, [&] { if (node(ka->node)) composeAnimated(ensureStyleAnim(ka->node.index)); });
}
void UiContext::finishAnimation(AnimationHandle a) {
    KeyAnim* ka = keyAnim(a);
    if (!ka) return;
    const double end = animEnd(ka->timing);
    if (ka->timing.playbackRate >= 0.f && !std::isfinite(end)) return;
    ka->time = ka->timing.playbackRate >= 0.f ? end : 0.0;
    ka->state = AnimPlayState::Running; // the next update reports it as finished (AnimationFinished)
    afterAnimChange(m_keyAnimDirty, [&] { if (node(ka->node)) composeAnimated(ensureStyleAnim(ka->node.index)); });
}
void UiContext::cancelAnimation(AnimationHandle a) {
    KeyAnim* ka = keyAnim(a);
    if (!ka) return;
    const NodeHandle n = ka->node;
    endKeyAnim(*ka);
    if (node(n))
        if (StyleAnim* sa = styleAnimOf(n)) composeAnimated(*sa);
}
void UiContext::setAnimationTime(AnimationHandle a, f32 seconds) {
    KeyAnim* ka = keyAnim(a);
    if (!ka || !std::isfinite(seconds)) return;
    ka->time = double(seconds);
    if (ka->state == AnimPlayState::Finished) ka->state = AnimPlayState::Running;
    afterAnimChange(m_keyAnimDirty, [&] { if (node(ka->node)) composeAnimated(ensureStyleAnim(ka->node.index)); });
}
f32 UiContext::animationTime(AnimationHandle a) const {
    const KeyAnim* ka = keyAnim(a);
    return ka ? f32(ka->time) : 0.f;
}
void UiContext::setPlaybackRate(AnimationHandle a, f32 rate) {
    KeyAnim* ka = keyAnim(a);
    if (!ka || !std::isfinite(rate)) return;
    ka->timing.playbackRate = rate;
    if (ka->state == AnimPlayState::Finished) ka->state = AnimPlayState::Running;
    m_keyAnimDirty = true;
}
f32 UiContext::playbackRate(AnimationHandle a) const {
    const KeyAnim* ka = keyAnim(a);
    return ka ? ka->timing.playbackRate : 0.f;
}
AnimPlayState UiContext::animationState(AnimationHandle a) const {
    const KeyAnim* ka = keyAnim(a);
    return ka ? ka->state : AnimPlayState::Idle;
}
u32 UiContext::animations(NodeHandle n, std::span<AnimationHandle> out) const {
    u32 count = 0;
    if (!node(n)) return 0;
    u64 after = 0;
    for (;;) { // oldest first
        const KeyAnim* next = nullptr;
        for (const KeyAnim& ka : m_keyAnims)
            if (ka.active && ka.node == n && ka.serial > after && (!next || ka.serial < next->serial)) next = &ka;
        if (!next) break;
        after = next->serial;
        if (count < out.size()) out[count] = {u32(next - m_keyAnims.data()), next->gen};
        ++count;
    }
    return count;
}
void UiContext::commitAnimationStyles(AnimationHandle a) {
    KeyAnim* ka = keyAnim(a);
    if (!ka || !node(ka->node)) return;
    const NodeHandle n = ka->node;
    ComputedStyle now = m_styles.style(m_nodes[n.index].targetStyle);
    if (StyleAnim* sa = styleAnimOf(n)) {
        composeAnimated(*sa);
        ka = keyAnim(a);
        if (!ka) return;
    }
    now = m_styles.style(m_nodes[n.index].style);
    Prop last = Prop::Count;
    for (std::size_t k = 0; k < ka->keys.size(); ++k) {
        const Prop p = ka->keys[k].prop;
        if (p == last) continue;
        last = p;
        set(n, p, readProp(now, p));
    }
}

} // namespace fuse::ui
