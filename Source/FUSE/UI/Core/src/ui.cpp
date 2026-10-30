#include "fuse/ui/ui.hpp"
#include "fuse/ui/css_masking.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace fuse::ui {

namespace {
constexpr u32 kNone = 0xFFFFFFFFu;
constexpr f32 kInf = std::numeric_limits<f32>::infinity();

enum NodeFlags : u32 {
    kFlagFocusable = 1u << 0,
    kFlagFocusScope = 1u << 2,
    kFlagRoleSet = 1u << 3,
};
enum Dirty : u8 { kDirtyStyle = 1, kDirtyLayout = 2, kDirtyPaint = 4, kDirtyText = 8, kDirtyMeasure = 16 };
} // namespace

const char* a11yRoleName(A11yRole r) {
    static const char* names[] = {"none", "window", "group", "text", "button", "checkbox", "radio", "switch",
                                  "slider", "progressbar", "textfield", "scrollarea", "list", "listitem", "image",
                                  "separator", "link", "combobox", "spinbox", "dial", "scrollbar", "tab",
                                  "dateinput", "timeinput", "datetimeinput", "grid", "row", "gridcell", "columnheader", "toolbar", "tablist"};
    return names[u32(r)];
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
    u32 style = 0;
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
    bool a11yProtected = false;
    A11yState a11y;
    std::vector<std::pair<Prop, PropValue>> inl;
    fuse::text::TextLayout layout;
    f32 layoutWidth = -1.f, layoutSize = -1.f;
    std::unique_ptr<fuse::text::TextEditBuffer> edit;
    Rect xfPainted{}; ///< CSS transform: where the transformed subtree painted last (damage when it moves)
    // CSS transitions: running per property; the node shows a scratch style (target + interpolated values)
    struct Running {
        u8 prop = 0;
        PropValue from, to, revStart; ///< revStart: reversing-adjusted start value (CSS Transitions 1 §3.3)
        f32 t = 0.f;
        TransitionSpec spec;
        f32 factor = 1.f; ///< reversing shortening factor
        bool started = false;
        bool discrete = false; ///< transition-behavior: allow-discrete (flips at the midpoint)
    };
    std::vector<Running> trans;
    // CSS animations (one per animation-name entry with @keyframes)
    struct Anim {
        u32 name = 0;
        u8 slot = 0;
        f32 t = 0.f;    ///< time since the animation was applied (excludes paused time)
        signed char phase = -1;  ///< -1 idle, 0 before, 1 active, 2 after
        i64 iter = -1;
        bool paused = false;
        bool tl = false; ///< follows a progress-based animation-timeline (time comes from tlP, not the clock)
        f32 tlP = std::numeric_limits<f32>::quiet_NaN();
    };
    std::vector<Anim> anims;
    std::vector<std::pair<u32, std::string>> attrs;
    std::string classNames; ///< space-separated class list ([class~=x] and the other attribute operators)
    std::vector<u32> scripted; ///< UiContext::animate animations (m_scriptAnims indices), creation order
    bool inMotionList = false;
    u32 transTarget = 0, transScratch = ~0u;
    u32 inhScratch = ~0u; ///< style slot while an ancestor's transition changes inherited values every frame
    bool styled = false; ///< styled once (no transitions on the first style)
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
    bool attribute(u32 n, u32 h, std::string_view& out) const override { return ui.attributeAt(n, h, out); }
    u32 firstChild(u32 n) const override { return nodes[n].first; }
    bool isEmpty(u32 n) const override { return nodes[n].first == 0xFFFFFFFFu && ui.text(ui.handleOf(n)).empty(); }
    bool containerSize(u32 n, u32 name, bool block, f32& w, f32& h) const override {
        for (u32 a = nodes[n].parent; a != 0xFFFFFFFFu; a = nodes[a].parent) {
            const ComputedStyle& cs = ui.m_styles.style(nodes[a].style);
            if (cs.containerType == 0 || (block && cs.containerType != 1)) continue;
            bool named = name == 0;
            for (u8 k = 0; !named && k < cs.containerNameCount; ++k) named = cs.containerName[k] == name;
            if (!named) continue;
            const Rect& r = nodes[a].rect;
            w = std::max(0.f, r.w - cs.padding.h());
            h = std::max(0.f, r.h - cs.padding.v());
            return true;
        }
        return false;
    }
    u32 styleContainer(u32 n, u32 name) const override {
        for (u32 a = nodes[n].parent; a != 0xFFFFFFFFu; a = nodes[a].parent) {
            if (!name) return a;
            const ComputedStyle& cs = ui.m_styles.style(nodes[a].style);
            for (u8 k = 0; k < cs.containerNameCount; ++k)
                if (cs.containerName[k] == name) return a;
        }
        return 0xFFFFFFFFu;
    }
};

// A Web Animations-style animation (UiContext::animate). Times in seconds on the animation's own timeline.
struct UiContext::ScriptAnim {
    u32 gen = 1;
    bool alive = false;
    u32 node = kNone, nodeGen = 0;
    std::vector<AnimKeyframe> frames; // offsets resolved, ascending
    AnimationOptions o;
    std::string_view id; // interned
    f32 t = 0.f;
    AnimPlayState state = AnimPlayState::Idle;
    bool tlActive = true; // progress-based timeline active
    AnimReplaceState replace = AnimReplaceState::Active;
    bool progressBased() const { return o.timeline.kind != AnimTimeline::Kind::Document; }
    f32 active() const { return o.duration > 0.f && o.iterations > 0.f ? o.duration * o.iterations : 0.f; }
    f32 end() const { return std::max(0.f, o.delay + active() + o.endDelay); }
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
    m_queue = std::make_unique<Command[]>(kQueueSize);
    for (u32 i = 0; i < kQueueSize; ++i) m_queue[i].seq.store(i, std::memory_order_relaxed);
    registerCoreClasses(m_registry);
    m_root = create(m_registry.find("root"), {}, "root");
    m_layerClass = m_registry.find("layer");
}

UiContext::~UiContext() = default;

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
        c.a11yProtected = false;
        c.a11y = {};
        c.inl.clear();
        c.layoutWidth = c.layoutSize = -1.f;
        c.edit.reset();
        c.trans.clear();
        c.anims.clear();
        c.attrs.clear();
        c.classNames.clear();
        c.scripted.clear();
        c.inMotionList = false;
        c.transScratch = ~0u;
        c.inhScratch = ~0u;
        c.styled = false;
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
    markSiblings(p);
    markRelational(i);
    markStyle(i);
    markDown(i, m_styles.treeInvalidation().down); // ancestors changed for the whole subtree
}
void UiContext::markSiblings(u32 p) {
    if (!m_styles.structural() || p == kNone) return; // :nth-* / sibling combinators / :empty depend on positions
    markStyle(p);
    for (u32 k = m_nodes[p].first; k != kNone; k = m_nodes[k].next) markStyle(k);
    markRelational(p);
}
// Tree change at i (insert / remove / reorder / :empty): the :has() anchors that can see i, and i's subtree when
// descendant selectors reach it (a moved subtree changes ancestors).
void UiContext::markRelational(u32 i) {
    const StyleEngine::Invalidation& v = m_styles.treeInvalidation();
    if (v.has) markHasAnchors(i, v, true);
}
void UiContext::markDown(u32 i, u8 levels) {
    if (!levels) return;
    const u8 next = levels == StyleEngine::Invalidation::kDeep ? levels : u8(levels - 1);
    for (u32 c = m_nodes[i].first; c != kNone; c = m_nodes[c].next) {
        markStyle(c);
        markDown(c, next);
    }
}
void UiContext::markHasAnchors(u32 i, const StyleEngine::Invalidation& v, bool self) {
    auto anchor = [&](u32 a) {
        markStyle(a);
        markDown(a, v.hasDown);
        if (v.hasSib)
            for (u32 s = m_nodes[a].next; s != kNone; s = m_nodes[s].next) markStyle(s), markDown(s, v.hasDown);
    };
    u32 a = i;
    for (u32 lvl = 0; a != kNone; ++lvl) {
        if (lvl > 0 || self) anchor(a);
        if (v.hasPrev)
            for (u32 p = m_nodes[a].prev; p != kNone; p = m_nodes[p].prev) anchor(p);
        if (v.hasUp != StyleEngine::Invalidation::kDeep && lvl >= v.hasUp) break;
        a = m_nodes[a].parent;
    }
}
void UiContext::invalidate(u32 i, const StyleEngine::Invalidation& v) {
    markStyle(i);
    markDown(i, v.down);
    // `+` reaches only the next v.sib siblings; `~` (kDeep) all later ones
    u32 n = 0;
    for (u32 s = m_nodes[i].next; s != kNone && n < v.sib; s = m_nodes[s].next, n += v.sib != StyleEngine::Invalidation::kDeep)
        markStyle(s), markDown(s, v.down);
    if (v.has) markHasAnchors(i, v, false);
}
void UiContext::invalidateClasses(u32 i, std::span<const u32> classHashes) {
    static const u32 kClassAttr = hashId("class");
    StyleEngine::Invalidation v = m_styles.attrInvalidation(kClassAttr);
    for (u32 c : classHashes) v.merge(m_styles.classInvalidation(c));
    invalidate(i, v);
}

void UiContext::unlink(u32 i) {
    Node& n = m_nodes[i];
    if (n.parent == kNone) return;
    Node& p = m_nodes[n.parent];
    if (n.prev != kNone) m_nodes[n.prev].next = n.next;
    else p.first = n.next;
    if (n.next != kNone) m_nodes[n.next].prev = n.prev;
    else p.last = n.prev;
    const u32 parent = n.parent;
    const u32 before = n.prev; // earlier siblings may have matched :has(+ / ~ this)
    n.parent = n.prev = n.next = kNone;
    markSiblings(parent);
    markRelational(before != kNone ? before : parent);
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
    }
    m_anyStyleDirty = m_anyLayoutDirty = m_anyPaintDirty = true;
    return {i, n.gen};
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
    dropTransitions(i);
    m_cold[i]->styled = false;
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
}

NodeHandle UiContext::lastChild(NodeHandle h) const {
    const Node* n = node(h);
    return n ? handleOf(n->last) : NodeHandle{};
}
NodeHandle UiContext::prevSibling(NodeHandle h) const {
    const Node* n = node(h);
    return n ? handleOf(n->prev) : NodeHandle{};
}
NodeHandle UiContext::childAt(NodeHandle h, u32 index) const {
    const Node* n = node(h);
    if (!n) return {};
    u32 k = n->first;
    for (u32 j = 0; k != kNone && j < index; ++j) k = m_nodes[k].next;
    return handleOf(k);
}
i32 UiContext::childIndex(NodeHandle h) const {
    const Node* n = node(h);
    if (!n || n->parent == kNone) return -1;
    i32 c = 0;
    for (u32 k = n->prev; k != kNone; k = m_nodes[k].prev) ++c;
    return c;
}
bool UiContext::insertBefore(NodeHandle h, NodeHandle ref) {
    const Node* r = node(ref);
    if (!node(h) || !r || r->parent == kNone || h == ref) return false;
    for (u32 a = r->parent; a != kNone; a = m_nodes[a].parent)
        if (a == h.index) return false;
    moveTo(h, handleOf(r->parent), ref);
    return true;
}
bool UiContext::insertAfter(NodeHandle h, NodeHandle ref) {
    const Node* r = node(ref);
    if (!node(h) || !r || r->parent == kNone || h == ref) return false;
    for (u32 a = r->parent; a != kNone; a = m_nodes[a].parent)
        if (a == h.index) return false;
    if (r->next == h.index) return true;
    moveTo(h, handleOf(r->parent), handleOf(r->next));
    return true;
}
bool UiContext::setChildIndex(NodeHandle h, u32 index) {
    const Node* n = node(h);
    if (!n || n->parent == kNone) return false;
    const u32 p = n->parent;
    std::vector<u32> order;
    for (u32 k = m_nodes[p].first; k != kNone; k = m_nodes[k].next)
        if (k != h.index) order.push_back(k);
    order.insert(order.begin() + std::min<std::size_t>(index, order.size()), h.index);
    relinkChildren(p, order);
    return true;
}
bool UiContext::swapNodes(NodeHandle a, NodeHandle b) {
    if (!node(a) || !node(b) || a == m_root || b == m_root) return false;
    if (a == b) return true;
    for (u32 x = m_nodes[a.index].parent; x != kNone; x = m_nodes[x].parent)
        if (x == b.index) return false;
    for (u32 x = m_nodes[b.index].parent; x != kNone; x = m_nodes[x].parent)
        if (x == a.index) return false;
    const u32 pa = m_nodes[a.index].parent, pb = m_nodes[b.index].parent;
    if (pa == pb) {
        std::vector<u32> order;
        for (u32 k = m_nodes[pa].first; k != kNone; k = m_nodes[k].next)
            order.push_back(k == a.index ? b.index : k == b.index ? a.index : k);
        relinkChildren(pa, order);
        return true;
    }
    const u32 na = m_nodes[a.index].next, nb = m_nodes[b.index].next;
    moveTo(a, handleOf(pb), handleOf(nb));
    moveTo(b, handleOf(pa), handleOf(na));
    return true;
}
bool UiContext::reorderChildren(NodeHandle h, std::span<const NodeHandle> order) {
    const Node* n = node(h);
    if (!n || order.size() != childCount(h)) return false;
    std::vector<u32> idx;
    idx.reserve(order.size());
    for (const NodeHandle& c : order) {
        const Node* cn = node(c);
        if (!cn || cn->parent != h.index || std::find(idx.begin(), idx.end(), c.index) != idx.end()) return false;
        idx.push_back(c.index);
    }
    relinkChildren(h.index, idx);
    return true;
}
void UiContext::setLayoutHook(NodeHandle n, LayoutHook hook) {
    std::erase_if(m_layoutHooks, [&](const LayoutHookEntry& h) { return h.index == n.index; });
    if (hook && alive(n)) {
        m_layoutHooks.push_back({n.index, n.generation, hook});
        markLayout(n.index);
    }
}
void UiContext::sortChildren(NodeHandle h, const std::function<bool(NodeHandle, NodeHandle)>& less) {
    if (!node(h) || !less) return;
    std::vector<u32> idx;
    for (u32 k = m_nodes[h.index].first; k != kNone; k = m_nodes[k].next) idx.push_back(k);
    std::stable_sort(idx.begin(), idx.end(), [&](u32 x, u32 y) { return less(handleOf(x), handleOf(y)); });
    relinkChildren(h.index, idx);
}
// Re-links p's children (a permutation of the current list) in one pass; one restyle / relayout.
void UiContext::relinkChildren(u32 p, const std::vector<u32>& order) {
    bool same = true;
    {
        u32 k = m_nodes[p].first;
        for (u32 c : order) {
            if (k != c) { same = false; break; }
            k = m_nodes[k].next;
        }
    }
    if (same) return;
    u32 prev = kNone;
    for (u32 c : order) {
        m_nodes[c].prev = prev;
        if (prev != kNone) m_nodes[prev].next = c;
        else m_nodes[p].first = c;
        prev = c;
    }
    if (prev != kNone) m_nodes[prev].next = kNone;
    m_nodes[p].last = prev;
    markSiblings(p);
    for (u32 c : order) markRelational(c);
    markLayout(p);
    m_anyPaintDirty = true;
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
        std::string& names = m_cold[h.index]->classNames;
        if (!names.empty()) names += ' ';
        names.append(cls);
        invalidateClasses(h.index, std::span<const u32>(&c, 1));
    }
}
void UiContext::setAttribute(NodeHandle h, std::string_view name, std::string_view value) {
    Node* n = node(h);
    if (!n) return;
    if (name == "class") { // the class list itself
        const std::string list(value); // value may view classNames
        invalidateClasses(h.index, std::span<const u32>(n->classes.data(), n->classCount)); // the old classes
        n->classCount = 0;
        m_cold[h.index]->classNames.clear();
        value = list;
        std::size_t i = 0;
        while (i < value.size()) {
            while (i < value.size() && std::isspace(static_cast<unsigned char>(value[i]))) ++i;
            std::size_t e = i;
            while (e < value.size() && !std::isspace(static_cast<unsigned char>(value[e]))) ++e;
            if (e > i) addClass(h, value.substr(i, e - i));
            i = e;
        }
        return;
    }
    auto& at = m_cold[h.index]->attrs;
    const u32 k = hashId(name);
    auto it = std::find_if(at.begin(), at.end(), [&](auto& a) { return a.first == k; });
    if (it != at.end()) {
        if (it->second == value) return;
        it->second.assign(value);
    } else at.emplace_back(k, std::string(value));
    invalidate(h.index, m_styles.attrInvalidation(k));
}
void UiContext::removeAttribute(NodeHandle h, std::string_view name) {
    Node* n = node(h);
    if (!n) return;
    if (name == "class") { setAttribute(h, "class", {}); return; }
    auto& at = m_cold[h.index]->attrs;
    const u32 k = hashId(name);
    if (std::erase_if(at, [&](auto& a) { return a.first == k; }) == 0) return;
    invalidate(h.index, m_styles.attrInvalidation(k));
}
std::optional<std::string_view> UiContext::attribute(NodeHandle h, std::string_view name) const {
    std::string_view v;
    if (!node(h) || !attributeAt(h.index, hashId(name), v)) return std::nullopt;
    return v;
}
bool UiContext::attributeAt(u32 i, u32 nameHash, std::string_view& out) const {
    for (const auto& [k, v] : m_cold[i]->attrs)
        if (k == nameHash) { out = v; return true; }
    if (nameHash == hashId("id") && !m_cold[i]->id.empty()) { out = m_cold[i]->id; return true; } // [id=...]
    static const u32 kClass = hashId("class");
    if (nameHash == kClass && m_nodes[i].classCount) { out = m_cold[i]->classNames; return true; } // [class~=x]
    return false;
}
void UiContext::removeClass(NodeHandle h, std::string_view cls) {
    Node* n = node(h);
    if (!n) return;
    const u32 c = hashId(cls);
    for (u8 k = 0; k < n->classCount; ++k)
        if (n->classes[k] == c) {
            n->classes[k] = n->classes[--n->classCount];
            std::string& names = m_cold[h.index]->classNames;
            for (std::size_t i = 0; i < names.size();) { // drop the word
                std::size_t e = names.find(' ', i);
                if (e == std::string::npos) e = names.size();
                if (std::string_view(names).substr(i, e - i) == cls) {
                    names.erase(i > 0 ? i - 1 : i, e - i + 1);
                    break;
                }
                i = e + 1;
            }
            invalidateClasses(h.index, std::span<const u32>(&c, 1));
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
        if (kv.first == p) {
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
    const bool wasEmpty = c.text.empty();
    c.text.assign(s);
    c.layoutWidth = -1.f;
    m_nodes[h.index].dirty |= kDirtyText;
    markLayout(h.index);
    if (m_styles.structural() && wasEmpty != c.text.empty()) { markStyle(h.index); markRelational(h.index); } // :empty
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
    n->state = s;
    markPaint(h.index);
    invalidate(h.index, m_styles.stateInvalidation());
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
void UiContext::setA11y(NodeHandle h, A11yRole r, std::string_view name) {
    Node* n = node(h);
    if (!n) return;
    n->role = r;
    n->flags |= kFlagRoleSet;
    m_cold[h.index]->a11yName.assign(name);
}
void UiContext::setA11yProtected(NodeHandle h, bool on) {
    if (node(h)) m_cold[h.index]->a11yProtected = on;
}
bool UiContext::a11yProtected(NodeHandle h) const { return node(h) && m_cold[h.index]->a11yProtected; }
void UiContext::setA11yState(NodeHandle h, const A11yState& st) {
    if (node(h)) m_cold[h.index]->a11y = st;
}
bool UiContext::a11yPartExpand(NodeHandle h, u32 part, bool open) {
    if (!node(h) || !part) return false;
    auto f = m_registry.get(m_nodes[h.index].cls).a11yPartExpand;
    return f && f(*this, h, part, open);
}
bool UiContext::a11yExpand(NodeHandle h, bool open) {
    if (!node(h) || m_cold[h.index]->a11y.expanded < 0 || (m_cold[h.index]->a11y.expanded > 0) == open) return false;
    NodeHandle f = h;
    auto firstFocusable = [&](auto&& self, u32 i) -> u32 {
        if (focusableNode(i)) return i;
        for (u32 k = m_nodes[i].first; k != kNone; k = m_nodes[k].next)
            if (const u32 r = self(self, k); r != kNone) return r;
        return kNone;
    };
    if (const u32 i = firstFocusable(firstFocusable, h.index); i != kNone) f = handleOf(i);
    setFocus(f, FocusReason::Keyboard);
    keyDown(open ? Key::Down : Key::Escape, open ? u8(kModAlt) : u8(0));
    return true;
}
A11yState UiContext::a11yState(NodeHandle h) const { return node(h) ? m_cold[h.index]->a11y : A11yState{}; }
A11yRole UiContext::role(NodeHandle h) const { return node(h) ? node(h)->role : A11yRole::None; }
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
void UiContext::setMediaPreferences(bool dark, bool reducedMotion) {
    MediaEnv env = m_styles.mediaEnv();
    env.dark = dark, env.reducedMotion = reducedMotion;
    m_styles.setMediaEnv(env);
}
void UiContext::setInputMedia(bool hover, bool anyHover, u8 pointer, u8 anyPointer) {
    MediaEnv env = m_styles.mediaEnv();
    env.hover = hover, env.anyHover = anyHover, env.pointer = std::min<u8>(pointer, 2), env.anyPointer = std::min<u8>(anyPointer, 2);
    m_styles.setMediaEnv(env);
}
void UiContext::setMediaResolution(f32 dppx) {
    MediaEnv env = m_styles.mediaEnv();
    env.resolution = dppx > 0.f ? dppx : 1.f;
    m_styles.setMediaEnv(env);
}
bool UiContext::syncContainers() {
    bool any = false;
    for (u32 i = 0; i < u32(m_nodes.size()); ++i) {
        const Node& n = m_nodes[i];
        if (!n.alive) continue;
        const ComputedStyle& cs = m_styles.style(n.style);
        if (cs.containerType == 0) {
            if (!m_containerSizes.empty()) m_containerSizes.erase(i);
            continue;
        }
        const Size sz{std::max(0.f, n.rect.w - cs.padding.h()), std::max(0.f, n.rect.h - cs.padding.v())};
        auto [it, fresh] = m_containerSizes.try_emplace(i, sz);
        if (!fresh && it->second == sz) continue; // fresh: its subtree was styled before the first layout
        it->second = sz;
        markDown(i, StyleEngine::Invalidation::kDeep);
        any = true;
    }
    return any;
}
void UiContext::setViewport(Size s) {
    if (s == m_viewport) return;
    m_viewport = s;
    markLayout(m_root.index);
}

// ---- Style pass ----------------------------------------------------------------------------------------------------
void UiContext::styleRec(u32 i, bool parentChanged) {
    Node& n = m_nodes[i];
    bool changed = false;
    if (parentChanged || (n.dirty & kDirtyStyle)) {
        const ContextSubject subj(*this, m_nodes);
        const ComputedStyle* ps = n.parent != kNone ? &m_styles.style(m_nodes[n.parent].style) : nullptr;
        const WidgetClass& wc = m_registry.get(n.cls);
        const u32 before = n.style;
        Cold& cold = *m_cold[i];
        ComputedStyle shown; // previous shown style when it lives in a scratch slot or transitions may run
        bool copied = false;
        if (m_styles.isScratch(before)) shown = m_styles.style(before), copied = true;
        u32 idx;
        const bool parentTweens = ps && m_styles.isScratch(m_nodes[n.parent].style);
        ComputedStyle inhCs;
        if (parentTweens) inhCs = m_styles.computeValue(subj, i, ps, &wc.defaults, cold.inl);
        if (parentTweens && cold.trans.empty() && cold.anims.empty() && cold.scripted.empty() && !inhCs.hasAnimations()) {
            // the parent tweens: inherited values change every frame; keep this node in its own slot meanwhile
            const ComputedStyle& cs = inhCs;
            if (cold.inhScratch == ~0u) cold.inhScratch = m_styles.allocScratch(cs);
            else m_styles.setScratch(cold.inhScratch, cs);
            idx = cold.inhScratch;
        } else {
            idx = m_styles.compute(subj, i, ps, &wc.defaults, cold.inl);
            if (cold.inhScratch != ~0u && idx != cold.inhScratch) {
                if (!copied) shown = m_styles.style(before), copied = true;
                m_styles.freeScratch(cold.inhScratch);
                cold.inhScratch = ~0u;
            }
            const ComputedStyle& ns = m_styles.style(idx);
            if (!cold.trans.empty() || !cold.anims.empty() || !cold.scripted.empty() || ns.hasAnimations() || (cold.styled && ns.hasTransitions())) {
                if (!copied) shown = m_styles.style(before), copied = true;
                idx = applyMotion(i, idx, shown, cold.styled);
            }
        }
        ++m_stats.styled;
        n.dirty &= u8(~kDirtyStyle);
        const bool scratchStep = idx == before && m_styles.isScratch(idx) && !(shown == m_styles.style(idx));
        cold.styled = true;
        if (idx != before || scratchStep) {
            ComputedStyle la = copied ? shown : m_styles.style(before);
            const ComputedStyle& lb = m_styles.style(idx);
            const bool fontChanged = la.fontSize != lb.fontSize || la.textAlign != lb.textAlign;
            n.style = idx;
            changed = true;
            markPaint(i);
            // compare layout-relevant fields only
            la.background = lb.background; la.background2 = lb.background2; la.borderColor = lb.borderColor;
            la.color = lb.color; la.shadowColor = lb.shadowColor; la.opacity = lb.opacity; la.radius = lb.radius;
            la.gradient = lb.gradient; la.gradientAngle = lb.gradientAngle; la.gradientStopCount = lb.gradientStopCount; la.gradientOffsets = lb.gradientOffsets; la.gradientColors = lb.gradientColors; la.shadowBlur = lb.shadowBlur; la.shadowX = lb.shadowX; la.shadowY = lb.shadowY; la.backdropBlur = lb.backdropBlur; la.backgroundClip = lb.backgroundClip; la.backdropFilterCount = lb.backdropFilterCount; la.backdropFilterFns = lb.backdropFilterFns; la.backdropFilterAmounts = lb.backdropFilterAmounts; la.backdropShadows = lb.backdropShadows; la.filterCount = lb.filterCount; la.filterFns = lb.filterFns; la.filterAmounts = lb.filterAmounts; la.filterShadows = lb.filterShadows;
            la.pointerEvents = lb.pointerEvents; la.borderWidth = lb.borderWidth; la.textAlign = lb.textAlign;
            if (fontChanged) {
                m_cold[i]->layoutWidth = -1.f;
                n.dirty |= kDirtyText;
            }
            if (!(la == lb) || fontChanged) markLayout(i);
        }
    }
    for (u32 c = n.first; c != kNone; c = m_nodes[c].next) styleRec(c, changed);
}

// CSS Transitions 1 / CSS Animations 1. A node with motion shows a per-node scratch style: the interned target
// (`transTarget`) with animation values applied (in animation-name order), then running transitions on top. The
// scratch is rebuilt each frame while anything advances; events are queued and dispatched after update().
void UiContext::dropTransitions(u32 i) {
    Cold& c = *m_cold[i];
    if (c.inhScratch != ~0u) {
        m_styles.freeScratch(c.inhScratch);
        c.inhScratch = ~0u;
    }
    c.trans.clear();
    c.anims.clear();
    dropScriptAnims(i);
    if (c.inMotionList) {
        std::erase(m_transNodes, i);
        c.inMotionList = false;
    }
    if (c.transScratch != ~0u) {
        if (m_nodes[i].style == c.transScratch) m_nodes[i].style = c.transTarget;
        m_styles.freeScratch(c.transScratch);
        c.transScratch = ~0u;
    }
}
void UiContext::queueMotion(u32 i, SignalKind k, f32 elapsed, std::string_view text) {
    m_motionEvents.push_back({i, m_nodes[i].gen, k, elapsed, text});
}

namespace {
// The value of property p at directed progress dp of a @keyframes rule; false: the rule does not animate p.
bool keyframeValue(const StyleEngine& se, const KeyframesRule& kr, Prop p, const ComputedStyle& base, f32 dp,
                   const Easing& animEase, AnimComposite comp, PropValue& out) {
    struct Stop { f32 off; PropValue v; Easing e; };
    Stop stops[16];
    int n = 0;
    for (const Keyframe& f : kr.frames) {
        const Declaration* d = nullptr;
        for (const Declaration& x : f.decls)
            if (x.prop == p) d = &x; // last declaration in the keyframe wins
        if (!d) continue;
        PropValue v;
        if (!se.resolveDecl(*d, v)) continue;
        const Easing e = f.hasEase ? f.ease : animEase;
        if (n > 0 && stops[n - 1].off == f.offset) { stops[n - 1].v = v; stops[n - 1].e = e; continue; } // later wins
        if (n == 15) break;
        stops[n++] = {f.offset, v, e};
    }
    if (n == 0) return false;
    const PropValue b = readProp(base, p);
    if (comp != AnimComposite::Replace)
        for (int k = 0; k < n; ++k) stops[k].v = compositeProp(b, stops[k].v, comp);
    if (stops[0].off > 0.f) { // implicit 0% / 100% keyframes take the underlying value
        for (int k = n; k > 0; --k) stops[k] = stops[k - 1];
        stops[0] = {0.f, b, animEase};
        ++n;
    }
    if (stops[n - 1].off < 1.f) stops[n++] = {1.f, b, animEase};
    int k = 0;
    while (k + 2 < n && dp >= stops[k + 1].off) ++k;
    if (n == 1) { out = stops[0].v; return true; }
    const f32 span = stops[k + 1].off - stops[k].off;
    const f32 local = span > 0.f ? std::clamp((dp - stops[k].off) / span, 0.f, 1.f) : 1.f;
    const f32 eased = easeValue(stops[k].e, local);
    out = propInfo(p).animatable ? interpolateProp(stops[k].v, stops[k + 1].v, eased)
                                 : interpolateDiscrete(p, stops[k].v, stops[k + 1].v, eased);
    return true;
}
// Types a discrete (allow-discrete) transition can carry.
bool discreteCapable(PropType t) {
    return t == PropType::Bool || t == PropType::Enum || t == PropType::Length || t == PropType::Number || t == PropType::Color;
}
template <class T, std::size_t N> const T& cyc(const std::array<T, N>& a, u8 count, u8 k) { return a[std::size_t(count ? k % count : 0)]; }
// animation-range-start / -end of animation entry k
AnimRange cssRange(const ComputedStyle& s, u8 k, bool end) {
    return end ? AnimRange{TimelineRange(cyc(s.animationRangeEndName, s.animationRangeEndCount, k)), cyc(s.animationRangeEndOffset, s.animationRangeEndCount, k)}
               : AnimRange{TimelineRange(cyc(s.animationRangeStartName, s.animationRangeStartCount, k)), cyc(s.animationRangeStartOffset, s.animationRangeStartCount, k)};
}
} // namespace

u32 UiContext::applyMotion(u32 i, u32 idx, const ComputedStyle& shown, bool styled) {
    Cold& c = *m_cold[i];
    const u32 prevTarget = c.transScratch != ~0u ? c.transTarget : m_nodes[i].style;
    const bool retarget = idx != prevTarget || !styled;
    const ComputedStyle& ns = m_styles.style(idx);
    // transitions: start, retarget, reverse (with shortening) or cancel per animatable property
    if (styled && idx != prevTarget) {
        const ComputedStyle os = m_styles.style(prevTarget);
        for (int p = 0; p < kPropCount; ++p) {
            const PropInfo& pi = propInfo(Prop(p));
            if (pi.type == PropType::List || (!pi.animatable && !discreteCapable(pi.type))) continue;
            const std::string_view pname = propInfo(Prop(p)).cssName;
            auto it = std::find_if(c.trans.begin(), c.trans.end(), [&](const Cold::Running& r) { return r.prop == p; });
            const PropValue to = readProp(ns, Prop(p));
            if (it == c.trans.end() && samePropValue(readProp(os, Prop(p)), to)) continue;
            TransitionSpec sp;
            const bool has = ns.transitionFor(u32(p), sp);
            const PropValue from = readProp(shown, Prop(p));
            const bool lengthMismatch = from.type == PropType::Length && from.len.unit != to.len.unit;
            const bool discrete = !pi.animatable || lengthMismatch;
            if (discrete && it == c.trans.end() && !(has && sp.allowDiscrete)) continue;
            if (!has || sp.duration + sp.delay <= 0.f || (discrete && !sp.allowDiscrete) || samePropValue(from, to)) {
                if (it != c.trans.end()) {
                    if (!samePropValue(it->to, to) || !has) queueMotion(i, SignalKind::TransitionCancel, std::max(0.f, it->t - it->spec.delay), pname);
                    c.trans.erase(it);
                }
                continue;
            }
            if (it != c.trans.end() && samePropValue(it->to, to)) continue;
            Cold::Running r{u8(p), from, to, from, 0.f, sp, 1.f, false, discrete};
            if (it != c.trans.end() && samePropValue(it->revStart, to)) {
                // reversing an unfinished transition: shorten by how far the old one got
                const Cold::Running& o = *it;
                const f32 e = o.t - o.spec.delay;
                const f32 out = e < 0.f ? 0.f : easeValue(o.spec.ease, o.spec.duration > 0.f ? e / o.spec.duration : 1.f);
                r.factor = std::clamp(std::fabs(out * o.factor + 1.f - o.factor), 0.f, 1.f);
                r.revStart = o.to;
                r.spec.duration *= r.factor;
                if (r.spec.delay < 0.f) r.spec.delay *= r.factor;
            }
            if (it != c.trans.end()) {
                queueMotion(i, SignalKind::TransitionCancel, std::max(0.f, it->t - it->spec.delay), pname);
                *it = r;
            } else c.trans.push_back(r);
            queueMotion(i, SignalKind::TransitionRun, std::max(0.f, -r.spec.delay), pname);
        }
    }
    c.transTarget = idx;
    // animations: match the animation-name list (by name, in order) to the running ones
    if (retarget) {
        std::vector<Cold::Anim> next;
        for (u8 k = 0; k < ns.animationNameCount; ++k) {
            const u32 name = ns.animationName[k];
            if (!name || !m_styles.keyframes(name)) continue;
            auto it = std::find_if(c.anims.begin(), c.anims.end(), [&](const Cold::Anim& a) { return a.name == name; });
            if (it != c.anims.end()) {
                next.push_back(*it);
                c.anims.erase(it);
            } else next.push_back(Cold::Anim{name, k});
            next.back().slot = k;
        }
        for (const Cold::Anim& a : c.anims)
            if (a.phase == 1) queueMotion(i, SignalKind::AnimationCancel, 0.f, animationName(a.name));
        c.anims = std::move(next);
    }
    ComputedStyle d = ns;
    bool applied = false;
    for (Cold::Anim& a : c.anims) {
        const KeyframesRule* kr = m_styles.keyframes(a.name);
        if (!kr) continue;
        const u8 k = a.slot;
        f32 dur = cyc(ns.animationDuration, ns.animationDurationCount, k);
        const f32 delay = cyc(ns.animationDelay, ns.animationDelayCount, k);
        f32 iters = cyc(ns.animationIterations, ns.animationIterationCount, k);
        AnimTimeline tl;
        const int tk = cssTimeline(i, ns, k, tl);
        a.tl = tk != 0;
        if (a.tl) { // progress-based: the timeline's 0..1 maps onto the active interval (auto duration = 1, inf = 1)
            const f32 p = tk == 2 ? timelineProgress({i, m_nodes[i].gen}, tl, cssRange(ns, k, false), cssRange(ns, k, true))
                                  : std::numeric_limits<f32>::quiet_NaN();
            a.tlP = p;
            if (!(dur > 0.f)) dur = 1.f;
            if (!std::isfinite(iters)) iters = 1.f;
            if (std::isnan(p)) continue; // inactive timeline: no effect
            a.t = delay + p * dur * iters;
        }
        const AnimDirection dir = cyc(ns.animationDirection, ns.animationDirectionCount, k);
        const AnimFill fill = cyc(ns.animationFill, ns.animationFillCount, k);
        const Easing& ease = cyc(ns.animationEase, ns.animationEaseCount, k);
        const AnimComposite comp = cyc(ns.animationComposite, ns.animationCompositeCount, k);
        a.paused = cyc(ns.animationPaused, ns.animationPlayCount, k) != 0;
        const f32 active = dur > 0.f && iters > 0.f ? dur * iters : 0.f; // may be inf
        const f32 e = a.t - delay;
        const signed char phase = e < 0.f ? 0 : (std::isfinite(active) && e >= active) ? 2 : 1;
        double overall = 0.0;
        if (phase == 1) overall = double(e) / double(dur);
        else if (phase == 2) overall = double(iters);
        i64 iter = i64(std::floor(overall));
        double prog = overall - double(iter);
        if (phase == 2 && prog == 0.0 && iters > 0.f) iter -= 1, prog = 1.0; // ends at the end of the last iteration
        const std::string_view nm = animationName(a.name);
        if (phase >= 1 && a.phase < 1) queueMotion(i, SignalKind::AnimationStart, std::clamp(-delay, 0.f, active), nm);
        if (phase == 1 && a.phase == 1 && iter > a.iter) queueMotion(i, SignalKind::AnimationIteration, f32(iter) * dur, nm);
        if (phase == 2 && a.phase != 2) queueMotion(i, SignalKind::AnimationEnd, active, nm);
        a.phase = phase, a.iter = iter;
        const bool fills = phase == 1 || (phase == 0 && (fill == AnimFill::Backwards || fill == AnimFill::Both)) ||
                           (phase == 2 && (fill == AnimFill::Forwards || fill == AnimFill::Both));
        if (!fills) continue;
        if (phase == 0) iter = 0, prog = 0.0;
        const bool odd = (iter & 1) != 0;
        const bool rev = dir == AnimDirection::Reverse || (dir == AnimDirection::Alternate && odd) ||
                         (dir == AnimDirection::AlternateReverse && !odd);
        const f32 dp = f32(rev ? 1.0 - prog : prog);
        PropMask seen;
        for (const Keyframe& f : kr->frames)
            for (const Declaration& x : f.decls) {
                if (seen.has(u8(x.prop))) continue;
                seen.set(u8(x.prop));
                PropValue v;
                if (keyframeValue(m_styles, *kr, x.prop, d, dp, ease, comp, v)) applyProp(d, x.prop, v), applied = true;
            }
    }
    // scripted (Web Animations) effects in creation order, over CSS animations
    for (u32 id : c.scripted)
        if (sampleScriptAnim(*m_scriptAnims[id], d)) applied = true;
    // running transitions on top; start / end events
    for (auto it = c.trans.begin(); it != c.trans.end();) {
        Cold::Running& r = *it;
        const std::string_view pname = propInfo(Prop(r.prop)).cssName;
        const f32 e = r.t - r.spec.delay;
        if (!r.started && e >= 0.f) {
            r.started = true;
            queueMotion(i, SignalKind::TransitionStart, std::clamp(-r.spec.delay, 0.f, r.spec.duration), pname);
        }
        if (e >= r.spec.duration) {
            queueMotion(i, SignalKind::TransitionEnd, r.spec.duration, pname);
            it = c.trans.erase(it);
            continue;
        }
        PropValue v = r.from;
        if (e >= 0.f) {
            const f32 k = easeValue(r.spec.ease, r.spec.duration > 0.f ? e / r.spec.duration : 1.f);
            v = r.discrete ? interpolateDiscrete(Prop(r.prop), r.from, r.to, k) : interpolateProp(r.from, r.to, k);
        }
        applyProp(d, Prop(r.prop), v);
        applied = true;
        ++it;
    }
    for (const Cold::Anim& a : c.anims)
        if (a.tl) {
            if (std::find(m_cssTlNodes.begin(), m_cssTlNodes.end(), i) == m_cssTlNodes.end()) m_cssTlNodes.push_back(i);
            break;
        }
    if ((!c.trans.empty() || !c.anims.empty() || !c.scripted.empty()) && !c.inMotionList) {
        c.inMotionList = true;
        m_transNodes.push_back(i);
    }
    if (!applied) {
        if (c.transScratch != ~0u) {
            m_styles.freeScratch(c.transScratch);
            c.transScratch = ~0u;
        }
        return idx;
    }
    if (c.transScratch == ~0u) c.transScratch = m_styles.allocScratch(d);
    else m_styles.setScratch(c.transScratch, d);
    return c.transScratch;
}

bool UiContext::nodeLive(u32 i) const {
    const Cold& c = *m_cold[i];
    if (!c.trans.empty()) return true;
    for (const Cold::Anim& a : c.anims)
        if (!a.paused && !a.tl && a.phase != 2) return true;
    for (u32 id : c.scripted)
        if (m_scriptAnims[id]->state == AnimPlayState::Running && !m_scriptAnims[id]->progressBased()) return true;
    return false;
}
bool UiContext::motionLive() const {
    if (!m_motionEvents.empty()) return true;
    for (u32 i : m_transNodes)
        if (m_nodes[i].alive && nodeLive(i)) return true;
    return false;
}
// ---- Scripted animations (Web Animations 1 subset) -----------------------------------------------------------------
UiContext::ScriptAnim* UiContext::scriptAnim(AnimationHandle h) {
    if (h.index >= m_scriptAnims.size()) return nullptr;
    ScriptAnim& a = *m_scriptAnims[h.index];
    return a.alive && a.gen == h.generation ? &a : nullptr;
}
const UiContext::ScriptAnim* UiContext::scriptAnim(AnimationHandle h) const {
    return const_cast<UiContext*>(this)->scriptAnim(h);
}
void UiContext::dropScriptAnims(u32 node) {
    Cold& c = *m_cold[node];
    for (u32 id : c.scripted) {
        ScriptAnim& a = *m_scriptAnims[id];
        a.alive = false;
        ++a.gen;
        a.frames.clear();
        a.o = {};
        m_freeScriptAnims.push_back(id);
    }
    c.scripted.clear();
}
void UiContext::scriptEvent(u32 id, SignalKind k) {
    const ScriptAnim& a = *m_scriptAnims[id];
    m_motionEvents.push_back({a.node, a.nodeGen, k, a.t, a.id, id, a.gen});
}
void UiContext::settleScriptAnim(u32 id) {
    ScriptAnim& a = *m_scriptAnims[id];
    if (a.state != AnimPlayState::Running || a.progressBased()) return;
    const f32 end = a.end();
    const f32 r = a.o.playbackRate;
    if ((r > 0.f && a.t >= end) || (r < 0.f && a.t <= 0.f)) {
        a.t = r > 0.f ? end : 0.f;
        a.state = AnimPlayState::Finished;
        scriptEvent(id, SignalKind::ScriptFinish);
    }
}
void UiContext::advanceScriptAnim(u32 id, f32 dt) {
    ScriptAnim& a = *m_scriptAnims[id];
    if (a.state != AnimPlayState::Running || a.progressBased()) return;
    a.t += dt * a.o.playbackRate;
    settleScriptAnim(id);
}
// Directed, eased iteration progress at the animation's current time; false: no effect (idle / not filling).
bool UiContext::scriptTiming(const ScriptAnim& a, f32& dp) const {
    if (a.state == AnimPlayState::Idle || !a.tlActive || a.replace == AnimReplaceState::Removed) return false;
    const AnimationOptions& o = a.o;
    const f32 active = a.active();
    const f32 e = a.t - o.delay;
    const bool backwardsEdge = o.playbackRate < 0.f; // the boundary belongs to the phase the animation runs into
    const int phase = (e < 0.f || (backwardsEdge && e == 0.f)) ? 0
                      : (std::isfinite(active) && (e > active || (!backwardsEdge && e >= active))) ? 2 : 1;
    const bool fills = phase == 1 || (phase == 0 && (o.fill == AnimFill::Backwards || o.fill == AnimFill::Both)) ||
                       (phase == 2 && (o.fill == AnimFill::Forwards || o.fill == AnimFill::Both));
    if (!fills) return false;
    double overall = o.iterationStart;
    if (phase == 1) overall += o.duration > 0.f ? double(e) / double(o.duration) : double(o.iterations);
    else if (phase == 2) overall += double(o.iterations);
    i64 iter = i64(std::floor(overall));
    double prog = overall - double(iter);
    if ((phase == 2 || (phase == 1 && o.duration <= 0.f)) && prog == 0.0 && overall > 0.0) iter -= 1, prog = 1.0;
    const bool odd = (iter & 1) != 0;
    const bool rev = o.direction == AnimDirection::Reverse || (o.direction == AnimDirection::Alternate && odd) ||
                     (o.direction == AnimDirection::AlternateReverse && !odd);
    dp = easeValue(o.easing, f32(rev ? 1.0 - prog : prog));
    return true;
}
bool UiContext::sampleScriptAnim(const ScriptAnim& a, ComputedStyle& d) const {
    f32 k;
    if (!scriptTiming(a, k)) return false;
    bool applied = false;
    PropMask seen;
    struct Stop { f32 off; PropValue v; Easing e; };
    std::vector<Stop> stops;
    const Easing linear{Easing::Kind::Bezier, 0.f, 0.f, 1.f, 1.f};
    for (const AnimKeyframe& f0 : a.frames)
        for (const auto& [p, v0] : f0.values) {
            if (seen.has(u8(p))) continue;
            seen.set(u8(p));
            const PropValue b = readProp(d, p);
            stops.clear();
            for (const AnimKeyframe& f : a.frames)
                for (const auto& [q, v] : f.values)
                    if (q == p) {
                        const AnimComposite cm = f.hasComposite ? f.composite : a.o.composite;
                        stops.push_back({f.offset, cm == AnimComposite::Replace ? v : compositeProp(b, v, cm), f.hasEase ? f.ease : linear});
                    }
            if (stops.front().off > 0.f) stops.insert(stops.begin(), Stop{0.f, b, linear});
            if (stops.back().off < 1.f) stops.push_back({1.f, b, linear});
            std::size_t s = 0;
            if (stops.size() == 1) { applyProp(d, p, stops[0].v); applied = true; continue; }
            if (k >= 1.f) s = stops.size() - 2;
            // the last stop with offset <= k starts the segment (equal offsets: the later one)
            else if (k > 0.f) while (s + 2 < stops.size() && k >= stops[s + 1].off) ++s;
            const f32 span = stops[s + 1].off - stops[s].off;
            const f32 local = span > 0.f ? (k - stops[s].off) / span : 1.f;
            const f32 eased = local >= 0.f && local <= 1.f ? easeValue(stops[s].e, local) : local; // overshoot: extrapolate
            const PropValue v = propInfo(p).animatable ? interpolateProp(stops[s].v, stops[s + 1].v, eased)
                                                       : interpolateDiscrete(p, stops[s].v, stops[s + 1].v, eased);
            applyProp(d, p, v);
            applied = true;
        }
    return applied;
}

f32 UiContext::timelineProgress(NodeHandle target, const AnimTimeline& t) const {
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    if (t.kind == AnimTimeline::Kind::Document || !node(target)) return nan;
    const u8 ax = t.axis ? 1 : 0;
    auto scroller = [&](u32 from) { // nearest ancestor scroll container
        for (u32 a = m_nodes[from].parent; a != kNone; a = m_nodes[a].parent)
            if (m_styles.style(m_nodes[a].style).overflow == Overflow::Scroll) return a;
        return kNone;
    };
    if (t.kind == AnimTimeline::Kind::Scroll) {
        const u32 sc = node(t.source) ? t.source.index : scroller(target.index);
        if (sc == kNone) return nan;
        const Node& n = m_nodes[sc];
        const Rect c = contentRect({sc, n.gen});
        const f32 range = ax ? n.extent.h - c.h : n.extent.w - c.w;
        if (!(range > 0.f)) return nan;
        return std::clamp((ax ? n.scroll.y : n.scroll.x) / range, 0.f, 1.f);
    }
    const u32 subj = node(t.source) ? t.source.index : target.index;
    const u32 sc = scroller(subj);
    if (sc == kNone) return nan;
    const Rect port = contentRect({sc, m_nodes[sc].gen}), r = m_nodes[subj].rect;
    const f32 ps = (ax ? port.y : port.x) + t.insetStart, pe = (ax ? port.bottom() : port.right()) - t.insetEnd;
    const f32 rs = ax ? r.y : r.x, rl = ax ? r.h : r.w;
    const f32 span = (pe - ps) + rl;
    if (!(span > 0.f)) return nan;
    return std::clamp((pe - rs) / span, 0.f, 1.f);
}
f32 UiContext::timelineProgress(NodeHandle target, const AnimTimeline& t, const AnimRange& rs, const AnimRange& re) const {
    if (rs.name == TimelineRange::Normal && re.name == TimelineRange::Normal && rs.offset.isAuto() && re.offset.isAuto())
        return timelineProgress(target, t);
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    if (std::isnan(timelineProgress(target, t))) return nan; // inactive
    const u8 ax = t.axis ? 1 : 0;
    f32 pos = 0.f, P = 0.f, S = 0.f; // position along the cover range [0, P + S]; scroll: S = 0, P = the scroll range
    bool view = false;
    if (t.kind == AnimTimeline::Kind::Scroll) {
        u32 sc = node(t.source) ? t.source.index : kNone;
        for (u32 a = m_nodes[target.index].parent; sc == kNone && a != kNone; a = m_nodes[a].parent)
            if (m_styles.style(m_nodes[a].style).overflow == Overflow::Scroll) sc = a;
        const Node& n = m_nodes[sc];
        const Rect c = contentRect({sc, n.gen});
        P = ax ? n.extent.h - c.h : n.extent.w - c.w;
        pos = ax ? n.scroll.y : n.scroll.x;
    } else {
        view = true;
        const u32 subj = node(t.source) ? t.source.index : target.index;
        u32 sc = kNone;
        for (u32 a = m_nodes[subj].parent; sc == kNone && a != kNone; a = m_nodes[a].parent)
            if (m_styles.style(m_nodes[a].style).overflow == Overflow::Scroll) sc = a;
        const Rect port = contentRect({sc, m_nodes[sc].gen}), r = m_nodes[subj].rect;
        const f32 plen = ax ? port.h : port.w;
        const f32 ps = (ax ? port.y : port.x) + t.insetStart + plen * t.insetStartPct / 100.f;
        const f32 pe = (ax ? port.bottom() : port.right()) - t.insetEnd - plen * t.insetEndPct / 100.f;
        P = pe - ps, S = ax ? r.h : r.w;
        pos = pe - (ax ? r.y : r.x);
    }
    auto at = [&](const AnimRange& r, bool isEnd) {
        f32 lo = 0.f, hi = P + S;
        if (view) switch (r.name) {
            case TimelineRange::Contain: lo = std::min(S, P), hi = std::max(S, P); break;
            case TimelineRange::Entry: lo = 0.f, hi = std::min(S, P); break;
            case TimelineRange::Exit: lo = std::max(S, P), hi = P + S; break;
            case TimelineRange::EntryCrossing: lo = 0.f, hi = S; break;
            case TimelineRange::ExitCrossing: lo = P, hi = P + S; break;
            default: break;
            }
        if (r.offset.isAuto()) return isEnd ? hi : lo;
        return lo + r.offset.resolve(hi - lo, 0.f);
    };
    const f32 a = at(rs, false), b = at(re, true);
    if (!(b - a > 0.f)) return nan;
    return (pos - a) / (b - a);
}
f32 UiContext::animTimelineProgress(AnimationHandle h) const {
    const ScriptAnim* a = scriptAnim(h);
    if (!a || !a->progressBased()) return std::numeric_limits<f32>::quiet_NaN();
    return timelineProgress({a->node, a->nodeGen}, a->o.timeline, a->o.rangeStart, a->o.rangeEnd);
}
double UiContext::animStartTime(AnimationHandle h) const {
    const ScriptAnim* a = scriptAnim(h);
    if (!a || a->progressBased() || a->state != AnimPlayState::Running || a->o.playbackRate == 0.f)
        return std::numeric_limits<double>::quiet_NaN();
    return m_docTime - double(a->t) / double(a->o.playbackRate);
}
bool UiContext::syncTimelines() {
    bool moved = false;
    for (std::size_t k = 0; k < m_timelineAnims.size();) {
        const u32 id = m_timelineAnims[k];
        ScriptAnim& a = *m_scriptAnims[id];
        if (!a.alive || !a.progressBased()) {
            m_timelineAnims[k] = m_timelineAnims.back();
            m_timelineAnims.pop_back();
            continue;
        }
        ++k;
        if (a.state != AnimPlayState::Running) continue;
        const f32 p = timelineProgress({a.node, a.nodeGen}, a.o.timeline, a.o.rangeStart, a.o.rangeEnd);
        const bool active = !std::isnan(p);
        f32 t = a.t;
        if (active) {
            const f32 end = a.end();
            t = a.o.playbackRate < 0.f ? (1.f - p) * end : p * end;
        }
        if (active != a.tlActive || t != a.t) {
            a.tlActive = active;
            a.t = t;
            markStyle(a.node);
            moved = true;
        }
    }
    for (std::size_t k = 0; k < m_cssTlNodes.size();) { // CSS animation-timeline
        const u32 i = m_cssTlNodes[k];
        bool any = false, changed = false;
        if (m_nodes[i].alive) {
            const ComputedStyle& ns = m_styles.style(m_cold[i]->transTarget);
            for (const Cold::Anim& a : m_cold[i]->anims) {
                if (!a.tl) continue;
                any = true;
                AnimTimeline tl;
                const int tk = cssTimeline(i, ns, a.slot, tl);
                const f32 p = tk == 2 ? timelineProgress({i, m_nodes[i].gen}, tl, cssRange(ns, a.slot, false), cssRange(ns, a.slot, true))
                                      : std::numeric_limits<f32>::quiet_NaN();
                if (tk == 0 || (p != a.tlP && !(std::isnan(p) && std::isnan(a.tlP)))) changed = true;
            }
        }
        if (!any) {
            m_cssTlNodes[k] = m_cssTlNodes.back();
            m_cssTlNodes.pop_back();
            continue;
        }
        ++k;
        if (changed) markStyle(i), moved = true;
    }
    return moved;
}
namespace {
void setInsets(AnimTimeline& t, const std::array<Length, 2>& in) {
    t.insetStart = in[0].unit == Unit::Px ? in[0].value : 0.f;
    t.insetStartPct = in[0].unit == Unit::Percent ? in[0].value : 0.f;
    t.insetEnd = in[1].unit == Unit::Px ? in[1].value : 0.f;
    t.insetEndPct = in[1].unit == Unit::Percent ? in[1].value : 0.f;
}
} // namespace
int UiContext::cssTimeline(u32 i, const ComputedStyle& s, u8 k, AnimTimeline& t) const {
    if (!s.animationTimelineCount) return 0;
    const u8 kind = cyc(s.animationTimelineKind, s.animationTimelineCount, k);
    const u8 arg = cyc(s.animationTimelineArg, s.animationTimelineCount, k);
    t = {};
    switch (kind) {
    case 0: return 0;
    case 3: // scroll(<scroller> <axis>)
        t.kind = AnimTimeline::Kind::Scroll;
        t.axis = arg & 1;
        if ((arg >> 1) == 1) t.source = m_root;
        else if ((arg >> 1) == 2) t.source = {i, m_nodes[i].gen};
        return 2;
    case 4: { // view(<axis> <inset>{0,2}): the target is the subject
        t.kind = AnimTimeline::Kind::View;
        t.axis = arg & 1;
        setInsets(t, s.animationTimelineInset[k % s.animationTimelineCount]);
        return 2;
    }
    case 2: { // named: scroll-timeline / view-timeline declared on the node or an ancestor (nearest wins)
        const u32 name = cyc(s.animationTimelineName, s.animationTimelineCount, k);
        auto declared = [&](u32 a, const ComputedStyle& as) {
            if (as.scrollTimelineName == name) {
                t.kind = AnimTimeline::Kind::Scroll, t.axis = as.scrollTimelineAxis, t.source = {a, m_nodes[a].gen};
                return true;
            }
            if (as.viewTimelineName == name) {
                t.kind = AnimTimeline::Kind::View, t.axis = as.viewTimelineAxis, t.source = {a, m_nodes[a].gen};
                setInsets(t, as.viewTimelineInset);
                return true;
            }
            return false;
        };
        for (u32 a = i; a != kNone; a = m_nodes[a].parent) {
            const ComputedStyle& as = a == i ? s : m_styles.style(m_nodes[a].style);
            if (declared(a, as)) return 2;
            bool scoped = as.timelineScopeAll;
            for (u8 q = 0; q < as.timelineScopeCount && !scoped; ++q) scoped = as.timelineScope[q] == name;
            if (!scoped) continue;
            // timeline-scope: the name's timeline declared by exactly one descendant (several: inactive)
            int found = 0;
            AnimTimeline hit;
            auto walk = [&](auto&& self, u32 p) -> void {
                for (u32 c = m_nodes[p].first; c != kNone && found < 2; c = m_nodes[c].next) {
                    if (declared(c, m_styles.style(m_nodes[c].style))) ++found, hit = t;
                    self(self, c);
                }
            };
            walk(walk, a);
            if (found == 1) { t = hit; return 2; }
            t = {};
            return 1;
        }
        return 1;
    }
    default: return 1; // none
    }
}
void UiContext::removeReplaced() {
    for (u32 i : m_transNodes) {
        if (!m_nodes[i].alive) continue;
        const Cold& c = *m_cold[i];
        PropMask covered;
        for (std::size_t k = c.scripted.size(); k-- > 0;) { // latest first
            const u32 id = c.scripted[k];
            ScriptAnim& a = *m_scriptAnims[id];
            f32 dp;
            if (a.state != AnimPlayState::Finished || a.progressBased() || !scriptTiming(a, dp)) continue;
            PropMask mine;
            bool all = true, none = true;
            for (const AnimKeyframe& f : a.frames)
                for (const auto& pv : f.values) mine.set(u8(pv.first)), none = false, all = all && covered.has(u8(pv.first));
            if (a.replace == AnimReplaceState::Active && all && !none) {
                a.replace = AnimReplaceState::Removed;
                scriptEvent(id, SignalKind::ScriptRemove);
                continue;
            }
            for (int w = 0; w < 2; ++w) covered.w[w] |= mine.w[w];
        }
    }
}

bool UiContext::resolveKeyframes(std::vector<AnimKeyframe>& frames) {
    if (frames.empty()) return false;
    // resolve offsets: explicit ones must ascend within [0,1]; missing ones are spaced evenly (Web Animations §4.4.3)
    f32 prev = 0.f;
    for (const AnimKeyframe& f : frames)
        if (f.offset >= 0.f) {
            if (f.offset > 1.f || f.offset < prev) return false;
            prev = f.offset;
        }
    const std::size_t n = frames.size();
    if (frames.front().offset < 0.f) frames.front().offset = n > 1 ? 0.f : 1.f;
    if (n > 1 && frames.back().offset < 0.f) frames.back().offset = 1.f;
    for (std::size_t i = 0; i < n;) {
        std::size_t j = i + 1;
        while (j < n && frames[j].offset < 0.f) ++j;
        if (j < n)
            for (std::size_t m = i + 1; m < j; ++m)
                frames[m].offset = frames[i].offset + (frames[j].offset - frames[i].offset) * f32(m - i) / f32(j - i);
        i = j;
    }
    for (AnimKeyframe& f : frames)
        std::erase_if(f.values, [](const auto& pv) { return propInfo(pv.first).type == PropType::List; });
    return true;
}
AnimationHandle UiContext::animate(NodeHandle h, std::vector<AnimKeyframe> frames, AnimationOptions o) {
    if (!node(h) || !resolveKeyframes(frames)) return {};
    if (!(o.duration >= 0.f) || !(o.iterations >= 0.f) || !(o.iterationStart >= 0.f) || !std::isfinite(o.playbackRate)) return {};
    Cold& c = *m_cold[h.index];
    // lazily forget idle animations and finished ones without fill so a node animated repeatedly stays small
    if (c.scripted.size() >= 16) {
        for (std::size_t k = 0; k < c.scripted.size();) {
            ScriptAnim& a = *m_scriptAnims[c.scripted[k]];
            f32 dp;
            const bool dead = a.state == AnimPlayState::Idle || (a.state == AnimPlayState::Finished && !scriptTiming(a, dp)); // incl. removed
            if (dead && c.scripted.size() >= 16) {
                a.alive = false, ++a.gen, a.frames.clear(), a.o = {};
                m_freeScriptAnims.push_back(c.scripted[k]);
                c.scripted.erase(c.scripted.begin() + std::ptrdiff_t(k));
            } else ++k;
        }
    }
    u32 id;
    if (!m_freeScriptAnims.empty()) id = m_freeScriptAnims.back(), m_freeScriptAnims.pop_back();
    else id = u32(m_scriptAnims.size()), m_scriptAnims.push_back(std::make_unique<ScriptAnim>());
    ScriptAnim& a = *m_scriptAnims[id];
    a.alive = true;
    a.node = h.index, a.nodeGen = h.generation;
    a.frames = std::move(frames);
    a.o = std::move(o);
    a.id = a.o.id.empty() ? std::string_view{} : animationName(animationNameId(a.o.id));
    a.t = 0.f;
    a.state = AnimPlayState::Idle;
    a.tlActive = true;
    a.replace = AnimReplaceState::Active;
    if (a.progressBased()) {
        if (a.o.duration <= 0.f) a.o.duration = 1.f; // CSS `auto` duration: the timeline's whole range
        if (std::find(m_timelineAnims.begin(), m_timelineAnims.end(), id) == m_timelineAnims.end()) m_timelineAnims.push_back(id);
    }
    c.scripted.push_back(id);
    const AnimationHandle ah{id, a.gen};
    if (a.o.autoplay) animPlay(ah);
    markStyle(h.index);
    return ah;
}
std::vector<AnimKeyframe> UiContext::parseKeyframes(std::string_view css) const {
    std::string text = "@keyframes fuse-script-anim {";
    text.append(css);
    text += "}";
    auto sheet = StyleSheet::parse(text);
    if (!sheet || sheet->keyframes().empty()) return {};
    std::vector<AnimKeyframe> frames;
    for (const Keyframe& f : sheet->keyframes().front().frames) {
        AnimKeyframe k;
        k.offset = f.offset;
        k.hasEase = f.hasEase, k.ease = f.ease;
        for (const Declaration& d : f.decls) {
            PropValue v;
            if (!m_styles.resolveDecl(d, v)) continue;
            auto it = std::find_if(k.values.begin(), k.values.end(), [&](const auto& pv) { return pv.first == d.prop; });
            if (it != k.values.end()) it->second = v;
            else k.values.push_back({d.prop, v});
        }
        frames.push_back(std::move(k));
    }
    std::stable_sort(frames.begin(), frames.end(), [](const AnimKeyframe& x, const AnimKeyframe& y) { return x.offset < y.offset; });
    return frames;
}
AnimationHandle UiContext::animate(NodeHandle h, std::string_view css, AnimationOptions o) {
    std::vector<AnimKeyframe> frames = parseKeyframes(css);
    if (frames.empty()) return {};
    return animate(h, std::move(frames), std::move(o));
}
bool UiContext::animSetKeyframes(AnimationHandle h, std::vector<AnimKeyframe> frames) {
    ScriptAnim* a = scriptAnim(h);
    if (!a || !resolveKeyframes(frames)) return false;
    a->frames = std::move(frames);
    markStyle(a->node);
    return true;
}
bool UiContext::animSetKeyframes(AnimationHandle h, std::string_view css) {
    std::vector<AnimKeyframe> frames = parseKeyframes(css);
    return !frames.empty() && animSetKeyframes(h, std::move(frames));
}
bool UiContext::animSetTarget(AnimationHandle h, NodeHandle n) {
    ScriptAnim* a = scriptAnim(h);
    if (!a || !node(n)) return false;
    if (a->node == n.index && a->nodeGen == n.generation) return true;
    std::erase(m_cold[a->node]->scripted, h.index);
    markStyle(a->node);
    a->node = n.index, a->nodeGen = n.generation;
    m_cold[n.index]->scripted.push_back(h.index);
    markStyle(n.index);
    return true;
}
bool UiContext::animPersist(AnimationHandle h) {
    ScriptAnim* a = scriptAnim(h);
    if (!a) return false;
    if (a->replace == AnimReplaceState::Removed) markStyle(a->node);
    a->replace = AnimReplaceState::Persisted;
    return true;
}
AnimReplaceState UiContext::animReplaceState(AnimationHandle h) const {
    const ScriptAnim* a = scriptAnim(h);
    return a ? a->replace : AnimReplaceState::Active;
}

bool UiContext::animPlay(AnimationHandle h) {
    ScriptAnim* a = scriptAnim(h);
    if (!a) return false;
    const f32 end = a->end(), r = a->o.playbackRate;
    if (r > 0.f && (a->state == AnimPlayState::Idle || a->t < 0.f || a->t >= end)) a->t = 0.f;
    else if (r < 0.f && (a->state == AnimPlayState::Idle || a->t <= 0.f || a->t > end)) {
        if (!std::isfinite(end)) return false;
        a->t = end;
    } else if (a->state == AnimPlayState::Idle) a->t = 0.f;
    a->state = AnimPlayState::Running;
    markStyle(a->node);
    return true;
}
bool UiContext::animPause(AnimationHandle h) {
    ScriptAnim* a = scriptAnim(h);
    if (!a) return false;
    if (a->state == AnimPlayState::Idle) {
        if (a->o.playbackRate < 0.f) {
            if (!std::isfinite(a->end())) return false;
            a->t = a->end();
        } else a->t = 0.f;
    }
    a->state = AnimPlayState::Paused;
    markStyle(a->node);
    return true;
}
bool UiContext::animReverse(AnimationHandle h) {
    ScriptAnim* a = scriptAnim(h);
    if (!a) return false;
    a->o.playbackRate = -a->o.playbackRate;
    if (!animPlay(h)) {
        a->o.playbackRate = -a->o.playbackRate;
        return false;
    }
    return true;
}
bool UiContext::animFinish(AnimationHandle h) {
    ScriptAnim* a = scriptAnim(h);
    if (!a) return false;
    const f32 r = a->o.playbackRate, end = a->end();
    if (r == 0.f || (r > 0.f && !std::isfinite(end))) return false;
    a->t = r > 0.f ? end : 0.f;
    a->state = AnimPlayState::Finished;
    scriptEvent(h.index, SignalKind::ScriptFinish);
    markStyle(a->node);
    return true;
}
bool UiContext::animCancel(AnimationHandle h) {
    ScriptAnim* a = scriptAnim(h);
    if (!a) return false;
    if (a->state != AnimPlayState::Idle) {
        a->state = AnimPlayState::Idle;
        scriptEvent(h.index, SignalKind::ScriptCancel);
        a->t = 0.f;
        markStyle(a->node);
    }
    return true;
}
bool UiContext::animSetCurrentTime(AnimationHandle h, f32 s) {
    ScriptAnim* a = scriptAnim(h);
    if (!a || !std::isfinite(s)) return false;
    a->t = s;
    if (a->state == AnimPlayState::Idle) a->state = AnimPlayState::Paused;
    else if (a->state == AnimPlayState::Finished && s >= 0.f && s < a->end()) a->state = AnimPlayState::Running;
    settleScriptAnim(h.index);
    markStyle(a->node);
    return true;
}
bool UiContext::animSetPlaybackRate(AnimationHandle h, f32 r) {
    ScriptAnim* a = scriptAnim(h);
    if (!a || !std::isfinite(r)) return false;
    a->o.playbackRate = r;
    if (a->state == AnimPlayState::Finished && ((r > 0.f && a->t < a->end()) || (r < 0.f && a->t > 0.f))) a->state = AnimPlayState::Running;
    settleScriptAnim(h.index);
    markStyle(a->node);
    return true;
}
bool UiContext::animCommitStyles(AnimationHandle h) {
    ScriptAnim* a = scriptAnim(h);
    if (!a) return false;
    const NodeHandle nh{a->node, a->nodeGen};
    if (!node(nh)) return false;
    const Cold& c = *m_cold[a->node];
    const ComputedStyle& base = m_styles.style(c.transScratch != ~0u ? c.transTarget : m_nodes[a->node].style);
    ComputedStyle d = base;
    if (!sampleScriptAnim(*a, d)) return true;
    PropMask seen;
    std::vector<std::pair<Prop, PropValue>> out;
    for (const AnimKeyframe& f : a->frames)
        for (const auto& [p, v] : f.values)
            if (!seen.has(u8(p))) seen.set(u8(p)), out.push_back({p, readProp(d, p)});
    for (const auto& [p, v] : out) set(nh, p, v);
    return true;
}
f32 UiContext::animCurrentTime(AnimationHandle h) const {
    const ScriptAnim* a = scriptAnim(h);
    return a && a->state != AnimPlayState::Idle ? a->t : std::numeric_limits<f32>::quiet_NaN();
}
f32 UiContext::animPlaybackRate(AnimationHandle h) const {
    const ScriptAnim* a = scriptAnim(h);
    return a ? a->o.playbackRate : 0.f;
}
AnimPlayState UiContext::animPlayState(AnimationHandle h) const {
    const ScriptAnim* a = scriptAnim(h);
    return a ? a->state : AnimPlayState::Idle;
}
NodeHandle UiContext::animTarget(AnimationHandle h) const {
    const ScriptAnim* a = scriptAnim(h);
    return a ? NodeHandle{a->node, a->nodeGen} : NodeHandle{};
}
std::string_view UiContext::animId(AnimationHandle h) const {
    const ScriptAnim* a = scriptAnim(h);
    return a ? a->id : std::string_view{};
}
bool UiContext::animValid(AnimationHandle h) const { return scriptAnim(h) != nullptr; }
f32 UiContext::animProgress(AnimationHandle h) const {
    const ScriptAnim* a = scriptAnim(h);
    if (!a || a->state == AnimPlayState::Idle) return std::numeric_limits<f32>::quiet_NaN();
    const f32 active = a->active();
    if (!std::isfinite(active)) return 0.f;
    if (active <= 0.f) return a->t >= a->o.delay ? 1.f : 0.f;
    return std::clamp((a->t - a->o.delay) / active, 0.f, 1.f);
}
std::vector<AnimationHandle> UiContext::animations(NodeHandle h, bool subtree) const {
    std::vector<AnimationHandle> out;
    if (!node(h)) return out;
    auto visit = [&](auto&& self, u32 i) -> void {
        for (u32 id : m_cold[i]->scripted) {
            const ScriptAnim& a = *m_scriptAnims[id];
            f32 dp;
            if (a.state == AnimPlayState::Running || a.state == AnimPlayState::Paused || scriptTiming(a, dp))
                out.push_back({id, a.gen});
        }
        if (subtree)
            for (u32 c = m_nodes[i].first; c != kNone; c = m_nodes[c].next) self(self, c);
    };
    visit(visit, h.index);
    return out;
}

u32 UiContext::transitionCount() const {
    u32 n = 0;
    for (u32 i : m_transNodes) n += m_nodes[i].alive && !m_cold[i]->trans.empty();
    return n;
}
u32 UiContext::animationCount() const {
    u32 n = 0;
    for (u32 i : m_transNodes) {
        if (!m_nodes[i].alive) continue;
        for (const Cold::Anim& a : m_cold[i]->anims)
            if (!a.paused && a.phase != 2) { ++n; break; }
    }
    return n;
}

void UiContext::stylePass() {
    if (m_styles.sheetVersion() != m_seenSheetVersion) {
        m_seenSheetVersion = m_styles.sheetVersion();
        for (auto& n : m_nodes)
            if (n.alive) n.dirty |= kDirtyStyle;
    }
    styleRec(m_root.index, false);
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
    fuse::text::layoutText(*m_font, fuse::text::defaultShaper(), c.text, d, c.layout);
    c.layoutWidth = width;
    c.layoutSize = s.fontSize;
    m_nodes[i].dirty &= u8(~kDirtyText);
}

f32 UiContext::measureString(std::string_view s, f32 px) {
    if (!m_font) return 0.f;
    return fuse::text::measureText(*m_font, s, px, m_shapeScratch);
}

void UiContext::setPixelScale(f32 scale) {
    scale = scale > 0.f ? scale : 1.f;
    if (scale == m_pixelScale) return;
    m_pixelScale = scale;
    markAllDirty();
}
void UiContext::paintTextLayout(DrawList& dl, const fuse::text::TextLayout& tl, Vec2 o, Color col, f32 px, u32 nodeIdx) {
    if (!m_font || col.a <= 0.f) return;
    for (const auto& ln : tl.lines) {
        for (u32 k = ln.firstGlyph; k < ln.firstGlyph + ln.glyphCount; ++k) {
            const auto& g = tl.glyphs[k];
            if (g.glyph == fuse::text::kInvisibleGlyph) continue;
            fuse::text::forEachPaintGlyph(*m_font, g, px, [&](const fuse::text::Font& f, std::uint32_t glyph, f32 fpx,
                                                               const fuse::text::ColorLayer* layer) {
                const f32 ps = m_pixelScale;
                const auto ag = m_atlas.get(f, glyph, ps == 1.f ? fpx : fpx * ps);
                if (!ag.valid || ag.w == 0) return;
                DrawCmd c;
                c.op = DrawOp::Glyph;
                if (ps == 1.f)
                    c.rect = {std::round(o.x + g.x) + f32(ag.x0), std::round(o.y + ln.baseline) + f32(ag.y0), f32(ag.w), f32(ag.h)};
                else // pixel-snapped at the physical size, stored in logical units (scaleDrawList undoes the / ps)
                    c.rect = {(std::round((o.x + g.x) * ps) + f32(ag.x0)) / ps, (std::round((o.y + ln.baseline) * ps) + f32(ag.y0)) / ps,
                              f32(ag.w) / ps, f32(ag.h) / ps};
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
}

void UiContext::paintString(DrawList& dl, std::string_view s, Rect box, Color col, f32 px, TextAlignment align, u32 nodeIdx) {
    if (!m_font || s.empty()) return;
    fuse::text::TextLayoutDesc d;
    d.pixelSize = px;
    d.maxWidth = box.w;
    d.elide = true;
    d.align = fuse::text::TextAlign(u8(align));
    fuse::text::layoutText(*m_font, fuse::text::defaultShaper(), s, d, m_stringScratch);
    const f32 y = box.y + std::floor((box.h - m_stringScratch.height) * 0.5f);
    paintTextLayout(dl, m_stringScratch, {box.x, y}, col, px, nodeIdx);
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
            c = wc.measure(*this, {i, n.gen}, in);
        } else if (n.first != kNone) {
            const bool row = s.direction == Direction::Row;
            u32 count = 0;
            switch (s.layout) {
            case LayoutKind::Box:
                for (u32 k = n.first; k != kNone; k = m_nodes[k].next) {
                    const ComputedStyle& cs = m_styles.style(m_nodes[k].style);
                    if (!cs.visible || m_nodes[k].cls == m_layerClass) continue;
                    const Size m = measure(k, {in.w - cs.margin.h(), in.h - cs.margin.v()});
                    const f32 mw = m.w + cs.margin.h(), mh = m.h + cs.margin.v();
                    if (row) { c.w += mw; c.h = std::max(c.h, mh); }
                    else { c.h += mh; c.w = std::max(c.w, mw); }
                    ++count;
                }
                if (count > 1) (row ? c.w : c.h) += s.gap * f32(count - 1);
                break;
            case LayoutKind::Grid: {
                const u32 cols = std::max<u32>(1, s.gridColumnCount);
                std::array<f32, kMaxGridTracks> colW{};
                f32 rowH = 0.f, total = 0.f;
                u32 col = 0;
                const f32 cw = std::isfinite(in.w) ? (in.w - s.gap * f32(cols - 1)) / f32(cols) : kInf;
                for (u32 k = n.first; k != kNone; k = m_nodes[k].next) {
                    const ComputedStyle& cs = m_styles.style(m_nodes[k].style);
                    if (!cs.visible || m_nodes[k].cls == m_layerClass) continue;
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
                    if (!cs.visible || m_nodes[k].cls == m_layerClass) continue;
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
                    if (!cs.visible || m_nodes[k].cls == m_layerClass) continue;
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
        arrange(k, r);
    };
    auto hidden = [&](u32 k) {
        if (m_nodes[k].cls == m_layerClass) { // overlay layer: covers the viewport, outside the flow
            arrange(k, {0.f, 0.f, m_viewport.w, m_viewport.h});
            return true;
        }
        if (m_styles.style(m_nodes[k].style).visible) return false;
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
            if (!cs.visible || m_nodes[k].cls == m_layerClass) continue;
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
            const bool crossFixed = row ? fixedLen(cs.height, crossAvail) : fixedLen(cs.width, crossAvail);
            const f32 crossRoom = crossAvail - cmB - cmA;
            f32 cross = row ? m.h : m.w;
            if (al == Align::Stretch && !crossFixed) cross = std::max(0.f, crossRoom);
            f32 co = cmB;
            if (al == Align::Center) co += (crossRoom - cross) * 0.5f;
            else if (al == Align::End) co += crossRoom - cross;
            pos += mBefore;
            const Rect r = row ? Rect{c.x + pos, c.y + co, main, cross} : Rect{c.x + co, c.y + pos, cross, main};
            place(k, {std::round(r.x), std::round(r.y), std::round(r.w), std::round(r.h)});
            pos += main + mAfter + between;
        }
        break;
    }
    case LayoutKind::Grid: {
        const u32 cols = std::max<u32>(1, s.gridColumnCount);
        std::array<f32, kMaxGridTracks> w{}, x{};
        f32 fixedSum = s.gap * f32(cols - 1), frSum = 0.f;
        for (u32 k = 0; k < cols; ++k) {
            const Length t = s.gridColumnCount ? s.gridColumns[k] : Length::fr(1.f);
            if (t.unit == Unit::Px || t.unit == Unit::Percent) { w[k] = t.resolve(c.w, 0.f); fixedSum += w[k]; }
            else frSum += t.unit == Unit::Fr ? t.value : 1.f;
        }
        const f32 frUnit = frSum > 0.f ? std::max(0.f, c.w - fixedSum) / frSum : 0.f;
        for (u32 k = 0; k < cols; ++k) {
            const Length t = s.gridColumnCount ? s.gridColumns[k] : Length::fr(1.f);
            if (!(t.unit == Unit::Px || t.unit == Unit::Percent)) w[k] = frUnit * (t.unit == Unit::Fr ? t.value : 1.f);
            x[k] = k ? x[k - 1] + w[k - 1] + s.gap : 0.f;
        }
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
            const f32 h = m.h;
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
            const bool hasL = !cs.left.isAuto(), hasR = !cs.right.isAuto(), hasT = !cs.top.isAuto(), hasB = !cs.bottom.isAuto();
            const f32 l = cs.left.resolve(c.w, 0.f), r = cs.right.resolve(c.w, 0.f);
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
            if (s.layout == LayoutKind::Anchor && (hasClass({k, m_nodes[k].gen}, "clamp") || hasClass({k, m_nodes[k].gen}, "ostool"))) { // keep popups / tool windows on screen
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
bool UiContext::defineFilter(std::string_view id, std::string_view cssFilters) {
    FilterDef d;
    const std::size_t lead = cssFilters.find_first_not_of(" \t\r\n");
    if (lead != std::string_view::npos && cssFilters[lead] == '<') {
        if (!parseSvgFilter(cssFilters, d.program, {}, filterImageResolver())) return false;
    } else {
        PropValue v;
        if (!parsePropValue(Prop::Filter, cssFilters, v)) return false;
        ComputedStyle tmp;
        applyProp(tmp, Prop::Filter, v);
        d.count = tmp.filterCount, d.fns = tmp.filterFns, d.amounts = tmp.filterAmounts, d.shadows = tmp.filterShadows;
    }
    m_filterDefs[filterNameId(id)] = std::move(d);
    for (Node& n : m_nodes) n.dirty |= kDirtyPaint;
    return true;
}

int UiContext::defineSvgFilters(std::string_view svgText) {
    int n = 0;
    for (const std::string& id : svgFilterIds(svgText)) {
        FilterDef d;
        if (!parseSvgFilter(svgText, d.program, id, filterImageResolver())) continue;
        m_filterDefs[filterNameId(id)] = std::move(d);
        ++n;
    }
    for (Node& k : m_nodes) k.dirty |= kDirtyPaint;
    return n;
}

void UiContext::defineFilterImage(std::string_view name, Image image) { m_filterImages[std::string(name)] = std::move(image); }
void UiContext::defineClipPath(std::string_view id, Path path, bool evenOdd, bool objectBoundingBox) {
    m_clipPathDefs[std::string(id)] = {std::move(path), evenOdd, objectBoundingBox};
    for (Node& k : m_nodes) k.dirty |= kDirtyPaint;
}
void UiContext::defineClipPath(std::string_view id, std::string_view svgPathData, bool evenOdd, bool objectBoundingBox) {
    defineClipPath(id, Path::fromSvg(svgPathData), evenOdd, objectBoundingBox);
}
void UiContext::defineMaskImage(std::string_view name, Image image) {
    m_maskImages[std::string(name)] = std::move(image);
    for (Node& k : m_nodes) k.dirty |= kDirtyPaint;
}

FilterImageResolver UiContext::filterImageResolver() const {
    return [this](std::string_view name) -> const Image* {
        const auto it = m_filterImages.find(std::string(name));
        return it == m_filterImages.end() ? nullptr : &it->second;
    };
}

f32 UiContext::FilterStage::reach() const {
    if (program) return f32(program->reach()) + 2.f;
    return blur + 2.f + (shadow.color.a > 0.f ? std::max(std::abs(shadow.offset.x), std::abs(shadow.offset.y)) + shadow.blur : 0.f);
}

void UiContext::filterStages(const FilterFn* fns, const f32* amounts, u8 count, const FilterShadow* shadows,
                             std::vector<FilterStage>& out, int depth) const {
    // canonical stage order is blur -> matrix -> shadow; a function that would run earlier than what the open stage
    // already holds starts a new (outer) stage, so the nested layers reproduce the CSS order exactly
    FilterStage st;
    bool open = false;
    const auto close = [&] {
        if (open) out.push_back(st);
        st = {};
        open = false;
    };
    for (u8 k = 0; k < count; ++k) {
        const f32 a = amounts[k];
        ColorMatrix m;
        switch (fns[k]) {
        case FilterFn::Blur:
            if (open) close();
            st.blur = std::max(0.f, a), open = true;
            continue;
        case FilterFn::DropShadow: {
            if (st.shadow.color.a > 0.f) close();
            const FilterShadow& sh = shadows[std::clamp(int(a), 0, kMaxFilterShadows - 1)];
            if (sh.color.a <= 0.f) continue;
            st.shadow = DropShadow{{sh.x, sh.y}, sh.blur, sh.color}, open = true;
            continue;
        }
        case FilterFn::Url: {
            u32 id;
            std::memcpy(&id, &a, 4);
            const auto it = m_filterDefs.find(id);
            if (it == m_filterDefs.end() || depth > 4) continue; // unknown reference: no effect
            const FilterDef& d = it->second;
            close();
            if (!d.program.empty()) {
                FilterStage p;
                p.program = &d.program;
                out.push_back(p);
            } else {
                filterStages(d.fns.data(), d.amounts.data(), d.count, d.shadows.data(), out, depth + 1);
            }
            continue;
        }
        case FilterFn::Brightness: m = ColorMatrix::brightness(a); break;
        case FilterFn::Contrast: m = ColorMatrix::contrast(a); break;
        case FilterFn::Saturate: m = ColorMatrix::saturate(a); break;
        case FilterFn::Grayscale: m = ColorMatrix::grayscale(a); break;
        case FilterFn::HueRotate: m = ColorMatrix::hueRotate(a); break;
        case FilterFn::Invert: m = ColorMatrix::invert(a); break;
        case FilterFn::Sepia: m = ColorMatrix::sepia(a); break;
        case FilterFn::Opacity: m = ColorMatrix::opacity(a); break;
        }
        if (st.shadow.color.a > 0.f) close();
        st.matrix = st.hasMatrix ? st.matrix.then(m) : m;
        st.hasMatrix = open = true;
    }
    close();
}

Mat4 UiContext::nodeMatrix(u32 i) const {
    const Node& n = m_nodes[i];
    const ComputedStyle& s = m_styles.style(n.style);
    Mat4 m = s.transformCount ? transform3dOf(s, n.rect) : Mat4{};
    if (n.parent != kNone) {
        const ComputedStyle& ps = m_styles.style(m_nodes[n.parent].style);
        if (ps.perspective > 0.f) m = perspectiveOf(ps, m_nodes[n.parent].rect) * m;
    }
    return m;
}
bool UiContext::preserves3d(u32 i) const {
    const ComputedStyle& s = m_styles.style(m_nodes[i].style);
    // grouping properties force flattening (CSS Transforms 2 §transform-style)
    return s.preserve3d && s.overflow == Overflow::Visible && s.filterCount == 0 && s.opacity >= 1.f && s.backdropFilterCount == 0;
}
namespace {
f32 planeDepth(const Mat4& m, const Rect& r) {
    const f32 cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f;
    const f32 z = m.at(2, 0) * cx + m.at(2, 1) * cy + m.at(2, 3), w = m.at(3, 0) * cx + m.at(3, 1) * cy + m.at(3, 3);
    return w > 0.f ? z / w : -std::numeric_limits<f32>::infinity();
}
// z / w of the local point q (z = 0) under m: the plane's depth at that pixel (larger = nearer)
f32 depthAt(const Mat4& m, Vec2 q) {
    const f32 z = m.at(2, 0) * q.x + m.at(2, 1) * q.y + m.at(2, 3), w = m.at(3, 0) * q.x + m.at(3, 1) * q.y + m.at(3, 3);
    return w > 0.f ? z / w : -std::numeric_limits<f32>::infinity();
}
// backface-visibility: hidden: the back faces the viewer when the inverse's (3, 3) entry is negative (CSS Transforms 2)
bool backFacing(const Mat4& m) {
    Mat4 inv;
    return m.inverse(inv) && inv.at(2, 2) < 0.f;
}
constexpr std::array<f32, 9> kIdentity3{1.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f};
std::array<f32, 9> mul3(const std::array<f32, 9>& a, const std::array<f32, 9>& b) {
    std::array<f32, 9> r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k) r[std::size_t(i * 3 + j)] += a[std::size_t(i * 3 + k)] * b[std::size_t(k * 3 + j)];
    return r;
}
} // namespace
namespace {
using V3 = Frag3d::V3;
struct Plane4 { f32 nx, ny, nz, d; };
// Newell normal + offset; false for a degenerate (zero-area) polygon
bool planeOf(const std::vector<V3>& p, Plane4& out) {
    f32 nx = 0.f, ny = 0.f, nz = 0.f, cx = 0.f, cy = 0.f, cz = 0.f;
    for (std::size_t k = 0; k < p.size(); ++k) {
        const V3& a = p[k];
        const V3& b = p[(k + 1) % p.size()];
        nx += (a.y - b.y) * (a.z + b.z), ny += (a.z - b.z) * (a.x + b.x), nz += (a.x - b.x) * (a.y + b.y);
        cx += a.x, cy += a.y, cz += a.z;
    }
    const f32 len = std::sqrt(nx * nx + ny * ny + nz * nz);
    if (!(len > 1e-6f)) return false;
    const f32 inv = 1.f / f32(p.size());
    out = {nx / len, ny / len, nz / len, 0.f};
    out.d = -(out.nx * cx + out.ny * cy + out.nz * cz) * inv;
    return true;
}
constexpr f32 kPlaneEps = 0.01f; // px (projected space)
constexpr std::size_t kMaxFrags = 512;
// splits convex poly by pl into front (>= 0) and back parts
void splitPoly(const std::vector<V3>& poly, const std::vector<f32>& sd, std::vector<V3>& front, std::vector<V3>& back) {
    for (std::size_t k = 0; k < poly.size(); ++k) {
        const std::size_t j = (k + 1) % poly.size();
        const V3& a = poly[k];
        const V3& b = poly[j];
        const f32 da = sd[k], db = sd[j];
        if (da >= -kPlaneEps) front.push_back(a);
        if (da <= kPlaneEps) back.push_back(a);
        if ((da > kPlaneEps && db < -kPlaneEps) || (da < -kPlaneEps && db > kPlaneEps)) {
            const f32 t = da / (da - db);
            const V3 m{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
            front.push_back(m), back.push_back(m);
        }
    }
}
// back-to-front order for a viewer at z = +inf looking down -z (larger z is nearer, as translateZ)
bool bspRec(std::vector<Frag3d>&& in, std::vector<Frag3d>& out, std::size_t& budget, int depth) {
    if (in.empty()) return true;
    if (depth > 64) return false;
    Plane4 pl;
    std::size_t si = 0;
    while (si < in.size() && !planeOf(in[si].pts, pl)) ++si;
    if (si == in.size()) { // all degenerate (edge-on): nothing visible
        for (auto& f : in) out.push_back(std::move(f));
        return true;
    }
    std::vector<Frag3d> front, back, on;
    std::vector<f32> sd;
    for (std::size_t k = 0; k < in.size(); ++k) {
        Frag3d& f = in[k];
        if (k == si) { on.push_back(std::move(f)); continue; }
        sd.resize(f.pts.size());
        bool pos = false, neg = false;
        for (std::size_t v = 0; v < f.pts.size(); ++v) {
            sd[v] = pl.nx * f.pts[v].x + pl.ny * f.pts[v].y + pl.nz * f.pts[v].z + pl.d;
            pos |= sd[v] > kPlaneEps, neg |= sd[v] < -kPlaneEps;
        }
        if (!pos && !neg) on.push_back(std::move(f));
        else if (!neg) front.push_back(std::move(f));
        else if (!pos) back.push_back(std::move(f));
        else {
            if (budget == 0) return false;
            --budget;
            Frag3d a{f.plane, {}, false}, b{f.plane, {}, false};
            splitPoly(f.pts, sd, a.pts, b.pts);
            if (a.pts.size() >= 3) front.push_back(std::move(a));
            if (b.pts.size() >= 3) back.push_back(std::move(b));
        }
    }
    // coplanar fragments keep document order (the splitter may not be first)
    std::stable_sort(on.begin(), on.end(), [](const Frag3d& x, const Frag3d& y) { return x.plane < y.plane; });
    const bool frontFaces = pl.nz >= 0.f;
    auto& first = frontFaces ? back : front;
    auto& second = frontFaces ? front : back;
    if (!bspRec(std::move(first), out, budget, depth + 1)) return false;
    for (auto& f : on) out.push_back(std::move(f));
    return bspRec(std::move(second), out, budget, depth + 1);
}
} // namespace
bool UiContext::bspOrder(const std::vector<Plane3d>& planes, std::vector<Frag3d>& out) const {
    std::vector<Frag3d> in;
    in.reserve(planes.size());
    for (u32 k = 0; k < planes.size(); ++k) {
        const Plane3d& pl = planes[k];
        const Rect r = pl.selfOnly ? m_nodes[pl.node].rect : paintBounds(pl.node);
        Frag3d f{k, {}, true};
        const Vec2 cs[4] = {{r.x, r.y}, {r.x + r.w, r.y}, {r.x + r.w, r.y + r.h}, {r.x, r.y + r.h}};
        for (const Vec2 c : cs) {
            const Mat4& m = pl.m;
            const f32 w = m.at(3, 0) * c.x + m.at(3, 1) * c.y + m.at(3, 3);
            if (!(w > 1e-4f)) return false;
            f.pts.push_back({(m.at(0, 0) * c.x + m.at(0, 1) * c.y + m.at(0, 3)) / w, (m.at(1, 0) * c.x + m.at(1, 1) * c.y + m.at(1, 3)) / w,
                             (m.at(2, 0) * c.x + m.at(2, 1) * c.y + m.at(2, 3)) / w});
        }
        in.push_back(std::move(f));
    }
    std::size_t budget = kMaxFrags;
    out.clear();
    return bspRec(std::move(in), out, budget, 0);
}
// the node's box united with its in-flow descendants' boxes (overflow-visible content a flat plane paints); a
// clipping node bounds its subtree
// trial paint of node i alone into a scratch list (state restored): does it emit anything?
bool UiContext::paintsSelf(u32 i, f32 opacity) {
    Node& n = m_nodes[i];
    const auto dirty = n.dirty;
    const Rect xp = m_cold[i]->xfPainted, dmg = m_damage;
    const u32 outer = m_xfNode, outerSelf = m_selfOnly;
    m_xfNode = i, m_selfOnly = i;
    m_scratch3d.clear();
    paintRec(i, opacity, m_scratch3d);
    m_xfNode = outer, m_selfOnly = outerSelf;
    n.dirty = dirty, m_cold[i]->xfPainted = xp, m_damage = dmg;
    if (m_scratch3d.size() > 0) return true;
    n.dirty &= u8(~kDirtyPaint); // not painted: settled
    return false;
}
Rect UiContext::paintBounds(u32 i) const {
    const Node& n = m_nodes[i];
    Rect r = n.rect;
    if (m_styles.style(n.style).overflow != Overflow::Visible) return r;
    for (u32 k = n.first; k != kNone; k = m_nodes[k].next)
        if (m_styles.style(m_nodes[k].style).visible) r = r.unite(paintBounds(k));
    return r;
}
void UiContext::collectPlanes(u32 i, const Mat4& acc, std::vector<Plane3d>& out) const {
    const Node& n = m_nodes[i];
    if (!m_styles.style(n.style).visible) return;
    if (!preserves3d(i)) { out.push_back({i, acc, false, planeDepth(acc, n.rect)}); return; }
    out.push_back({i, acc, true, planeDepth(acc, n.rect)});
    for (u32 k = n.first; k != kNone; k = m_nodes[k].next) collectPlanes(k, acc * nodeMatrix(k), out);
}
// Paints node i through its transform; false when it has none (paint as usual). 2D maps: translate / scale map the
// commands exactly (transformRange), others wrap them in a transform layer (DrawList::pushTransform). 3D maps flatten
// to a homography (DrawList::pushProjective). preserve-3d: the node and its descendants up to the first flat one are
// separate planes in one 3D context, each its own layer, painted back to front by the depth of their centres (no
// plane splitting). Damage: 2D maps map the inner damage; otherwise any change repaints the whole box.
bool UiContext::paintTransformed(u32 i, f32 opacity, DrawList& dl) {
    Node& n = m_nodes[i];
    const ComputedStyle& s = m_styles.style(n.style);
    const bool pres = preserves3d(i);
    const bool parentPersp = n.parent != kNone && m_styles.style(m_nodes[n.parent].style).perspective > 0.f;
    if (!s.transformCount && !pres && !parentPersp) return false;
    const Mat4 M = nodeMatrix(i);
    Rect& last = m_cold[i]->xfPainted;
    const bool dirty = n.dirty & kDirtyPaint;
    auto drop = [&] { // not rendered (singular / back face)
        if (!last.empty()) m_damage = m_damage.unite(last), last = {};
        return true;
    };
    if (!pres) {
        const std::array<f32, 9> H = M.flatten();
        std::array<f32, 9> Hi;
        if (s.backfaceHidden && backFacing(M)) return drop();
        if (!invert3(H, Hi)) return drop();
        if (H == kIdentity3) return false;
        if (H[6] == 0.f && H[7] == 0.f) { // affine
            const Affine2 m{H[0] / H[8], H[3] / H[8], H[1] / H[8], H[4] / H[8], H[2] / H[8], H[5] / H[8]};
            const std::size_t c0 = dl.size(), e0 = dl.edgeCount();
            const u32 outer = m_xfNode;
            const Rect outerDamage = m_damage;
            m_damage = {};
            m_xfNode = i;
            const bool layer = !m.axisAligned(); // translate / scale map the commands exactly
            if (layer) dl.pushTransform(m.array());
            paintRec(i, opacity, dl);
            if (layer) dl.popTransform();
            m_xfNode = outer;
            Rect box;
            if (!layer) {
                dl.transformRange(c0, e0, {m.a, m.d}, {m.e, m.f});
                box = m.mapRect(n.rect);
            } else box = c0 < dl.size() && dl.cmds()[c0].op == DrawOp::PushFilter ? dl.cmds()[c0].rect : m.mapRect(n.rect);
            const Rect inner = m_damage;
            m_damage = outerDamage;
            if (!inner.empty()) m_damage = m_damage.unite(m.mapRect(inner));
            if (dirty || !(box == last)) m_damage = m_damage.unite(box).unite(last);
            last = box;
            return true;
        }
    }
    std::vector<Plane3d> planes;
    if (pres) collectPlanes(i, M, planes);
    else planes.push_back({i, M, false, 0.f});
    if (planes.size() > 1) { // a preserve-3d node that paints nothing itself is no plane (it would only split others)
        std::erase_if(planes, [&](const Plane3d& pl) { return pl.selfOnly && !paintsSelf(pl.node, opacity); });
    }
    // paint order: a BSP tree over the planes' quads in projected space (x / w, y / w, z / w; projective maps keep
    // planes planar), splitting planes that intersect; falls back to centre depth when a corner is behind the viewer
    std::vector<Frag3d> frags;
    if (planes.size() < 2 || !bspOrder(planes, frags)) {
        std::stable_sort(planes.begin(), planes.end(), [](const Plane3d& a, const Plane3d& b) { return a.depth < b.depth; });
        frags.clear();
        for (u32 k = 0; k < planes.size(); ++k) frags.push_back({k, {}, true});
    }
    const u32 outer = m_xfNode, outerSelf = m_selfOnly;
    const Rect outerDamage = m_damage;
    Rect box, tight;
    bool wide = false; // inner damage could not be mapped: repaint the whole box
    for (const Frag3d& f : frags) {
        const Plane3d& pl = planes[f.plane];
        const ComputedStyle& ps = m_styles.style(m_nodes[pl.node].style);
        if (ps.backfaceHidden && backFacing(pl.m)) continue;
        const std::array<f32, 9> H = pl.m.flatten();
        std::array<f32, 9> Hi;
        if (!invert3(H, Hi)) continue;
        if (!f.whole) {
            Path clip;
            for (std::size_t v = 0; v < f.pts.size(); ++v) v ? clip.lineTo({f.pts[v].x, f.pts[v].y}) : clip.moveTo({f.pts[v].x, f.pts[v].y});
            clip.close();
            dl.pushClipPath(clip);
        }
        const std::size_t c0 = dl.size();
        const bool affine = H[6] == 0.f && H[7] == 0.f;
        if (affine) dl.pushTransform({H[0] / H[8], H[3] / H[8], H[1] / H[8], H[4] / H[8], H[2] / H[8], H[5] / H[8]});
        else dl.pushProjective(H);
        m_xfNode = pl.node;
        m_selfOnly = pl.selfOnly ? pl.node : ~0u;
        m_damage = {};
        paintRec(pl.node, opacity, dl);
        dl.popTransform();
        if (!f.whole) dl.popClip();
        Rect fb;
        if (c0 < dl.size() && dl.cmds()[c0].op == DrawOp::PushFilter) fb = dl.cmds()[c0].rect, box = box.unite(fb);
        if (!m_damage.empty()) { // tight: the plane's inner damage through its homography (within the layer box)
            const Rect r = mapRectHomography(H, m_damage);
            if (r.empty()) wide = true;
            else tight = tight.unite(fb.empty() ? r : r.intersect(fb));
        }
    }
    m_xfNode = outer, m_selfOnly = outerSelf;
    m_damage = outerDamage;
    if (dirty || wide || !(box == last)) m_damage = m_damage.unite(box).unite(last);
    else if (!tight.empty()) m_damage = m_damage.unite(tight);
    last = box;
    return true;
}


void UiContext::paintRec(u32 i, f32 opacity, DrawList& dl) {
    Node& n = m_nodes[i];
    const ComputedStyle& s = m_styles.style(n.style);
    // CSS transform (2D / 3D / perspective / preserve-3d): see paintTransformed
    if (s.visible && m_xfNode != i && paintTransformed(i, opacity, dl)) return;
    if (n.dirty & kDirtyPaint) {
        Rect& last = m_cold[i]->xfPainted;
        if (!last.empty()) m_damage = m_damage.unite(last), last = {};
    }
    // clip-path / mask: the node and its subtree inside the shape, times the mask (a second visit paints as usual)
    if (s.visible && (s.clipPath || s.maskImage) && m_clipPathNode != i) {
        int pushed = 0;
        bool hidden = false;
        if (s.clipPath) {
            const ClipPathSpec& spec = clipPathSpec(s.clipPath);
            if (spec.kind == ClipPathSpec::Kind::Url) {
                if (const auto it = m_clipPathDefs.find(spec.url); it != m_clipPathDefs.end()) {
                    Path p = it->second.path;
                    if (it->second.objectBoundingBox) p.transform(Vec2{n.rect.w, n.rect.h}, Vec2{n.rect.x, n.rect.y});
                    else p.transform(Vec2{1.f, 1.f}, Vec2{n.rect.x, n.rect.y});
                    dl.pushClipPath(p, it->second.evenOdd);
                    ++pushed;
                } // a missing reference: no clipping
            } else if (spec.kind != ClipPathSpec::Kind::None) {
                const Path p = clipPathShape(spec, cssBox(s, n.rect, spec.box));
                if (p.empty()) hidden = true;
                else {
                    dl.pushClipPath(p, spec.evenOdd);
                    ++pushed;
                }
            }
            if (pushed) dl.cmds()[dl.size() - 1].node = i;
        }
        if (hidden) return; // an empty shape clips everything
        if (s.maskImage) {
            ClipLayer layer;
            const MaskImageLookup images = [this](std::string_view nm) -> const Image* {
                const auto it = m_maskImages.find(std::string(nm));
                return it == m_maskImages.end() ? nullptr : &it->second;
            };
            if (buildMaskClip(s, n.rect, images, layer)) {
                dl.pushClipLayers(std::span<const ClipLayer>(&layer, 1));
                dl.cmds()[dl.size() - 1].node = i;
                ++pushed;
            }
        }
        const u32 outer = m_clipPathNode;
        m_clipPathNode = i;
        paintRec(i, opacity, dl);
        for (int k = 0; k < pushed; ++k) dl.popClip();
        m_clipPathNode = outer;
        return;
    }
    // CSS filter: the node and its subtree go to one isolated layer (DrawList::pushFilter); this visit wraps a second
    // one that paints as usual. The damage grows by the filter's reach (blur, shadow offset + blur).
    if (s.visible && m_filterNode != i && s.filterCount) {
        std::vector<FilterStage> stages;
        filterStages(s.filterFns.data(), s.filterAmounts.data(), s.filterCount, s.filterShadows.data(), stages);
        if (!stages.empty()) {
            if (n.dirty & kDirtyPaint) {
                f32 reach = 0.f;
                for (const FilterStage& st : stages) reach += st.reach();
                m_damage = m_damage.unite({n.rect.x - reach, n.rect.y - reach, n.rect.w + 2.f * reach, n.rect.h + 2.f * reach});
                for (const FilterStage& st : stages) // an SVG program draws over its filter region
                    if (st.program) m_damage = m_damage.unite(st.program->resolve(n.rect).regionPx);
            }
            const u32 outer = m_filterNode;
            m_filterNode = i;
            // the first stage runs first, so it is the innermost layer; only the outermost applies clip coverage
            for (std::size_t k = stages.size(); k-- > 0;) {
                const FilterStage& st = stages[k];
                if (st.program) dl.pushFilter(*st.program, k + 1 < stages.size(), n.rect);
                else dl.pushFilter(st.matrix, st.blur, st.shadow, k + 1 < stages.size());
                dl.cmds()[dl.size() - 1].node = i;
            }
            paintRec(i, opacity, dl);
            for (std::size_t k = 0; k < stages.size(); ++k) dl.popFilter();
            m_filterNode = outer;
            return;
        }
    }
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
    if (s.shadowColor.a > 0.f && (s.shadowBlur > 0.f || s.shadowX != 0.f || s.shadowY != 0.f))
        dl.shadow({n.rect.x + s.shadowX, n.rect.y + s.shadowY, n.rect.w, n.rect.h}, tint(s.shadowColor), s.shadowBlur, rad);
    // background-clip: background + backdrop painted in the border / padding / content box (inner radii shrink by
    // the inset, as CSS); the border itself always strokes the border box
    Rect bg = n.rect;
    std::array<f32, 4> bgRad = s.radius;
    if (s.backgroundClip != BackgroundClip::BorderBox) {
        const f32 bw = std::max(0.f, s.borderWidth);
        Edges in{bw, bw, bw, bw};
        if (s.backgroundClip == BackgroundClip::ContentBox) in = {bw + s.padding.l, bw + s.padding.t, bw + s.padding.r, bw + s.padding.b};
        bg = {n.rect.x + in.l, n.rect.y + in.t, std::max(0.f, n.rect.w - in.h()), std::max(0.f, n.rect.h - in.v())};
        const f32 ins[4] = {std::max(in.l, in.t), std::max(in.r, in.t), std::max(in.r, in.b), std::max(in.l, in.b)};
        for (int k = 0; k < 4; ++k) bgRad[std::size_t(k)] = std::max(0.f, s.radius[std::size_t(k)] - ins[k]);
    }
    const bool clipped = s.backgroundClip != BackgroundClip::BorderBox;
    const bool hasBorder = s.borderWidth > 0.f && s.borderColor.a > 0.f;
    // background-clip: text — background and backdrop only inside the node's glyph outlines (an empty clip without text)
    const bool textClip = s.backgroundClip == BackgroundClip::Text;
    if (textClip) {
        const Rect content = contentRect(h);
        Path glyphs;
        if (m_font && !m_cold[i]->text.empty()) {
            const auto& tl = m_cold[i]->layout;
            const Vec2 o{content.x, content.y + std::floor(std::max(0.f, content.h - tl.height) * 0.5f)};
            std::vector<fuse::text::GlyphOutlineCmd> oc;
            for (const auto& ln : tl.lines)
                for (u32 k = ln.firstGlyph; k < ln.firstGlyph + ln.glyphCount; ++k) {
                    const auto& g = tl.glyphs[k];
                    if (g.glyph == fuse::text::kInvisibleGlyph) continue;
                    fuse::text::forEachPaintGlyph(*m_font, g, s.fontSize, [&](const fuse::text::Font& f, std::uint32_t glyph, f32 fpx,
                                                                             const fuse::text::ColorLayer* layer) {
                        if (layer && !layer->foreground) return;
                        oc.clear();
                        if (!f.glyphOutline(glyph, fpx, oc)) return;
                        const f32 bx = std::round(o.x + g.x), by = std::round(o.y + ln.baseline);
                        for (const auto& c : oc) {
                            const Vec2 p{bx + c.x, by + c.y};
                            switch (c.kind) {
                            case fuse::text::GlyphOutlineCmd::Move: glyphs.moveTo(p); break;
                            case fuse::text::GlyphOutlineCmd::Line: glyphs.lineTo(p); break;
                            case fuse::text::GlyphOutlineCmd::Quad: glyphs.quadTo({bx + c.cx, by + c.cy}, p); break;
                            case fuse::text::GlyphOutlineCmd::Cubic: glyphs.cubicTo({bx + c.cx, by + c.cy}, {bx + c.cx1, by + c.cy1}, p); break;
                            }
                        }
                        glyphs.close();
                    });
                }
        }
        if (glyphs.empty()) glyphs.rect({n.rect.x, n.rect.y, 0.f, 0.f});
        dl.pushClipPath(glyphs);
        bg = n.rect;
        bgRad = s.radius;
    }
    if ((s.backdropBlur > 0.f || s.backdropFilterCount) && !bg.empty()) {
        // backdrop-filter in CSS order: one backdrop pass per stage over the same box (each reads the previous one's
        // result); an SVG program runs over its filter region (resolved against the box)
        std::vector<FilterStage> stages;
        filterStages(s.backdropFilterFns.data(), s.backdropFilterAmounts.data(), s.backdropFilterCount, s.backdropShadows.data(), stages);
        if (s.backdropBlur > 0.f) {
            FilterStage b;
            b.blur = s.backdropBlur;
            stages.insert(stages.begin(), b);
        }
        for (const FilterStage& st : stages)
            if (st.program) dl.backdrop(bg, *st.program, bgRad);
            else dl.backdrop(bg, st.blur, bgRad, st.matrix, st.shadow);
    }
    if (s.gradient == GradientDir::Angle && s.gradientStopCount >= 2) {
        std::vector<GradientStop> st(s.gradientStopCount);
        for (u8 k = 0; k < s.gradientStopCount; ++k) st[k] = {s.gradientOffsets[k], tint(s.gradientColors[k])};
        dl.gradientRect(bg, Gradient::linearAngle(bg, s.gradientAngle, std::move(st)), bgRad);
        if (hasBorder) dl.roundedRect(n.rect, {}, s.radius, tint(s.borderColor), s.borderWidth);
    } else if (s.gradient != GradientDir::None && s.gradient != GradientDir::Angle) {
        dl.gradientRect(bg, tint(s.background), tint(s.background2), s.gradient == GradientDir::Vertical, bgRad);
        if (hasBorder) dl.roundedRect(n.rect, {}, s.radius, tint(s.borderColor), s.borderWidth);
    } else if (textClip) {
        if (s.background.a > 0.f) dl.roundedRect(bg, tint(s.background), bgRad);
    } else if (clipped && (s.background.a > 0.f || hasBorder)) {
        if (s.background.a > 0.f && !bg.empty()) dl.roundedRect(bg, tint(s.background), bgRad);
        if (hasBorder) dl.roundedRect(n.rect, {}, s.radius, tint(s.borderColor), s.borderWidth);
    } else if (s.background.a > 0.f || hasBorder) {
        dl.roundedRect(n.rect, tint(s.background), s.radius, tint(s.borderColor), s.borderWidth);
    }
    if (textClip) {
        dl.popClip();
        if (hasBorder) dl.roundedRect(n.rect, {}, s.radius, tint(s.borderColor), s.borderWidth);
    }
    for (std::size_t k = dl.size() - std::min<std::size_t>(dl.size(), 6); k < dl.size(); ++k)
        if (dl.cmds()[k].node == 0xFFFFFFFFu) dl.cmds()[k].node = i;
    const WidgetClass& wc = m_registry.get(n.cls);
    const Rect content = contentRect(h);
    if (wc.paint) {
        const PaintContext pc{*this, h, n.rect, content, s, n.state, opacity};
        wc.paint(pc, dl);
    } else if (!m_cold[i]->text.empty()) {
        const auto& tl = m_cold[i]->layout;
        const f32 y = content.y + std::floor(std::max(0.f, content.h - tl.height) * 0.5f);
        paintTextLayout(dl, tl, {content.x, y}, tint(s.color), s.fontSize, i);
    }
    if (n.first == kNone || m_selfOnly == i) return; // a preserve-3d plane: the children are planes of their own
    const bool clip = s.overflow != Overflow::Visible;
    if (clip) dl.pushClip(n.rect, rad);
    for (u32 k = n.first; k != kNone; k = m_nodes[k].next) {
        if (clip && m_nodes[k].rect.intersect(n.rect).empty()) { // culled; still consume dirty flags
            if (m_nodes[k].dirty & kDirtyPaint) { m_damage = m_damage.unite(m_nodes[k].rect.intersect(n.rect)); m_nodes[k].dirty &= u8(~kDirtyPaint); }
            continue;
        }
        if ((m_osPopups || m_osTools) && &dl == &m_draw &&
            ((m_osPopups && hasClass({k, m_nodes[k].gen}, "clamp")) || hasClass({k, m_nodes[k].gen}, "ostool"))) { // own OS window
            m_popupPending.push_back(k);
            continue;
        }
        paintRec(k, opacity, dl);
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

void UiContext::paintPass() {
    m_stats.repainted = false;
    if (!m_anyPaintDirty) return;
    m_draw.clear();
    m_popupPending.clear();
    paintRec(m_root.index, 1.f, m_draw);
    // A transform layer resamples its whole box: a partial repaint (scissor = damage) touching one must cover it all,
    // else the resampled pixels would read content clipped away by the scissor.
    for (bool grew = !m_damage.empty(); grew;) {
        grew = false;
        for (const DrawCmd& c : m_draw.cmds())
            if (c.op == DrawOp::PushFilter && isTransformLayer(c) && !c.rect.empty()) {
                const Rect u = m_damage.unite(c.rect);
                if (!m_damage.intersect(c.rect).empty() && !(u == m_damage)) m_damage = u, grew = true;
            }
    }
    std::vector<PopupSurface> next;
    for (u32 k : m_popupPending) {
        PopupSurface ps;
        ps.node = {k, m_nodes[k].gen};
        ps.rect = m_nodes[k].rect;
        ps.tool = hasClass(ps.node, "ostool");
        const Rect saved = m_damage;
        paintRec(k, 1.f, ps.draw);
        m_damage = saved; // a popup window repaints itself; the main window is not damaged by it
        std::erase_if(ps.draw.cmds(), [&](const DrawCmd& c) { return c.op == DrawOp::Shadow && c.node == k; }); // OS shadow
        if (ps.draw.size() == 0 || ps.rect.empty()) continue;
        ps.draw.translate({-ps.rect.x, -ps.rect.y});
        ps.revision = ++m_popupRevision;
        for (const PopupSurface& old : m_popupSurfaces)
            if (old.node == ps.node && old.rect.x == ps.rect.x && old.rect.y == ps.rect.y && old.rect.w == ps.rect.w &&
                old.rect.h == ps.rect.h && old.draw.hash() == ps.draw.hash())
                ps.revision = old.revision;
        next.push_back(std::move(ps));
    }
    if (next.size() != m_popupSurfaces.size()) m_damage = m_damage.unite({0.f, 0.f, m_viewport.w, m_viewport.h});
    m_popupSurfaces = std::move(next);
    m_anyPaintDirty = false;
    m_stats.repainted = true;
    m_stats.drawCmds = u32(m_draw.size());
}

void UiContext::setOsPopups(bool on, Rect screen) {
    if (on == m_osPopups && screen.x == m_popupScreen.x && screen.y == m_popupScreen.y && screen.w == m_popupScreen.w &&
        screen.h == m_popupScreen.h)
        return;
    m_osPopups = on;
    m_popupScreen = screen;
    if (!on && !m_osTools) m_popupSurfaces.clear();
    markAllDirty();
}
void UiContext::setOsToolWindows(bool on) {
    if (on == m_osTools) return;
    m_osTools = on;
    if (!on && !m_osPopups) m_popupSurfaces.clear();
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
    if (dt > 0.f) m_docTime += double(dt);
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
    if (m_animCount) {
        for (auto& a : m_anims) {
            if (!a.active) continue;
            Node* n = node(a.node);
            if (!n) { a.active = false; --m_animCount; continue; }
            a.t += dt;
            f32 k = a.duration > 0.f ? std::clamp(a.t / a.duration, 0.f, 1.f) : 1.f;
            switch (a.ease) {
            case Ease::Linear: break;
            case Ease::InOutCubic: k = k < 0.5f ? 4.f * k * k * k : 1.f - std::pow(-2.f * k + 2.f, 3.f) * 0.5f; break;
            case Ease::OutBack: { const f32 c1 = 1.70158f, c3 = c1 + 1.f; k = 1.f + c3 * std::pow(k - 1.f, 3.f) + c1 * std::pow(k - 1.f, 2.f); break; }
            }
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
    for (std::size_t k = 0; k < m_transNodes.size();) { // advance CSS transitions / animations (applied in the style pass)
        const u32 i = m_transNodes[k];
        Cold& c = *m_cold[i];
        if (!m_nodes[i].alive || (c.trans.empty() && c.anims.empty() && c.scripted.empty())) {
            c.inMotionList = false;
            m_transNodes[k] = m_transNodes.back();
            m_transNodes.pop_back();
            continue;
        }
        if (nodeLive(i)) {
            for (auto& r : c.trans) r.t += std::max(dt, 0.f);
            for (auto& a : c.anims)
                if (!a.paused && !a.tl) a.t += std::max(dt, 0.f);
            for (std::size_t q = 0; q < c.scripted.size(); ++q) advanceScriptAnim(c.scripted[q], std::max(dt, 0.f));
            markStyle(i);
        }
        ++k;
    }
    if (!m_timelineAnims.empty() || !m_cssTlNodes.empty()) syncTimelines();
    {
        MediaEnv env = m_styles.mediaEnv();
        env.width = m_viewport.w, env.height = m_viewport.h;
        m_styles.setMediaEnv(env); // a changed @media result bumps the sheet version: full restyle
    }
    auto styleAndLayout = [&] {
        if (m_anyStyleDirty || m_styles.sheetVersion() != m_seenSheetVersion) stylePass();
        if (m_anyLayoutDirty) {
            Node& r = m_nodes[m_root.index];
            if (r.measureKey != Size{m_viewport.w, m_viewport.h}) measure(m_root.index, m_viewport);
            arrange(m_root.index, {0.f, 0.f, m_viewport.w, m_viewport.h});
            m_anyLayoutDirty = false;
        }
    };
    styleAndLayout();
    // @container: sizes come from layout; a container whose size changed re-styles its subtree and lays out again
    // (bounded: container-type size / inline-size contain the queried axes, so the second pass settles)
    for (int k = 0; k < 2 && m_styles.usesContainerQueries() && syncContainers(); ++k) styleAndLayout();
    if (!m_layoutHooks.empty()) { // widgets reacting to their new geometry (tool bar overflow)
        std::erase_if(m_layoutHooks, [&](const LayoutHookEntry& h) { return !alive(NodeHandle{h.index, h.gen}); });
        const auto hooks = m_layoutHooks; // a hook may add / remove hooks
        for (const LayoutHookEntry& h : hooks)
            if (alive(NodeHandle{h.index, h.gen})) h.fn(*this, NodeHandle{h.index, h.gen});
        if (m_anyStyleDirty || m_anyLayoutDirty) styleAndLayout();
    }
    // scroll / view timelines read the new layout (scroll offsets, subject boxes): one more pass when they moved
    if ((!m_timelineAnims.empty() || !m_cssTlNodes.empty()) && syncTimelines()) styleAndLayout();
    removeReplaced();
    paintPass();
    if (!m_motionEvents.empty()) { // CSS transition / animation events, after the frame is consistent
        std::vector<MotionEvent> ev;
        ev.swap(m_motionEvents);
        for (const MotionEvent& e : ev) {
            const NodeHandle h{e.node, e.gen};
            if (node(h)) emit(h, e.kind, {.value = e.elapsed, .text = e.text});
            if (e.anim == ~0u) continue;
            const AnimationHandle ah{e.anim, e.animGen};
            if (ScriptAnim* a = scriptAnim(ah)) {
                auto fn = e.kind == SignalKind::ScriptFinish ? a->o.onFinish : e.kind == SignalKind::ScriptRemove ? a->o.onRemove : a->o.onCancel; // copy: may free a
                if (fn) fn(*this, ah);
            }
        }
    }
}

bool UiContext::needsUpdate() const {
    if (m_anyStyleDirty || m_anyLayoutDirty || m_anyPaintDirty || m_animCount || motionLive()) return true;
    if (m_styles.sheetVersion() != m_seenSheetVersion) return true;
    for (const auto& t : m_timers)
        if (t.active) return true;
    return m_head.load(std::memory_order_acquire) != m_tail;
}

f32 UiContext::nextWakeSeconds() const {
    if (m_anyStyleDirty || m_anyLayoutDirty || m_anyPaintDirty || m_animCount || motionLive()) return 0.f;
    if (m_styles.sheetVersion() != m_seenSheetVersion) return 0.f;
    if (m_head.load(std::memory_order_acquire) != m_tail) return 0.f;
    f32 t = std::numeric_limits<f32>::infinity();
    for (const auto& tm : m_timers)
        if (tm.active) t = std::min(t, std::max(0.f, tm.left));
    return t;
}

// ---- Hit testing and dispatch --------------------------------------------------------------------------------------
// p is in the current flat plane; acc is the 3D context's accumulated map when the parent preserves 3D (else
// identity). A flat node maps p into its own space for itself and its children; a preserve-3d node hands p and
// its map on, and returns the child hit nearest the viewer at p (depth of each hit plane at p, so intersecting planes
// pick the visible one; ties go to the later sibling, as painted); its own box only when nothing nearer is hit.
NodeHandle UiContext::hitRec(u32 i, Vec2 p, const Mat4& acc, f32* depth) const {
    const Node& n = m_nodes[i];
    const ComputedStyle& s = m_styles.style(n.style);
    if (!s.visible || n.animOpacity <= 0.f) return {};
    const bool inCtx = n.parent != kNone && preserves3d(n.parent);
    const bool parentPersp = n.parent != kNone && m_styles.style(m_nodes[n.parent].style).perspective > 0.f;
    const bool pres = preserves3d(i);
    Mat4 a = acc;
    if (s.transformCount || parentPersp) a = inCtx ? acc * nodeMatrix(i) : nodeMatrix(i);
    else if (!inCtx) a = Mat4{};
    Vec2 q = p;
    bool selfHidden = false;
    if (!a.identity()) {
        if (s.backfaceHidden && backFacing(a)) {
            if (!pres) return {};
            selfHidden = true;
        }
        std::array<f32, 9> Hi;
        if (!invert3(a.flatten(), Hi) || !mapHomography(Hi, p, q)) return {};
    }
    const bool inside = n.rect.contains(q);
    if (!inside && s.overflow != Overflow::Visible) return {};
    if (s.clipPath) { // clip-path clips hit testing (subtree too); masks do not
        const ClipPathSpec& spec = clipPathSpec(s.clipPath);
        if (spec.kind == ClipPathSpec::Kind::Url) {
            if (const auto it = m_clipPathDefs.find(spec.url); it != m_clipPathDefs.end()) {
                const Vec2 local = it->second.objectBoundingBox
                                       ? Vec2{n.rect.w > 0.f ? (q.x - n.rect.x) / n.rect.w : 0.f, n.rect.h > 0.f ? (q.y - n.rect.y) / n.rect.h : 0.f}
                                       : Vec2{q.x - n.rect.x, q.y - n.rect.y};
                const auto& e = it->second.path.edges();
                if (it->second.objectBoundingBox) {
                    Path p = it->second.path;
                    p.transform(Vec2{n.rect.w, n.rect.h}, Vec2{n.rect.x, n.rect.y});
                    if (pathFillCoverage(p.edges().data(), p.edges().size(), q.x, q.y, it->second.evenOdd) < 0.5f) return {};
                } else if (pathFillCoverage(e.data(), e.size(), local.x, local.y, it->second.evenOdd) < 0.5f) return {};
            }
        } else if (spec.kind != ClipPathSpec::Kind::None && !clipPathContains(spec, cssBox(s, n.rect, spec.box), q)) return {};
    }
    const f32 selfDepth = depthAt(a, q);
    if (pres) {
        NodeHandle best;
        f32 bestD = -std::numeric_limits<f32>::infinity();
        for (u32 k = n.first; k != kNone; k = m_nodes[k].next) {
            f32 d = -std::numeric_limits<f32>::infinity();
            const NodeHandle h = hitRec(k, p, a, &d);
            if (h.valid() && (!best.valid() || d >= bestD)) best = h, bestD = d;
        }
        if (best.valid() && (!(inside && !selfHidden && s.pointerEvents) || bestD >= selfDepth)) {
            if (depth) *depth = bestD;
            return best;
        }
    } else
        for (u32 k = n.last; k != kNone; k = m_nodes[k].prev) {
            const NodeHandle h = hitRec(k, q, Mat4{});
            if (h.valid()) {
                if (depth) *depth = selfDepth;
                return h;
            }
        }
    if (inside && !selfHidden && s.pointerEvents) {
        if (depth) *depth = selfDepth;
        return {i, n.gen};
    }
    return {};
}
std::array<f32, 9> UiContext::screenMap(NodeHandle h) const {
    std::array<f32, 9> total = kIdentity3;
    if (!node(h)) return total;
    bool any = false; // fast path (no allocation): nothing transforms on the chain
    for (u32 a = h.index; a != kNone && !any; a = m_nodes[a].parent) {
        const Node& n = m_nodes[a];
        any = m_styles.style(n.style).transformCount || (n.parent != kNone && m_styles.style(m_nodes[n.parent].style).perspective > 0.f);
    }
    if (!any) return total;
    // root to node: a flat ancestor ends a stage (its plane), a preserve-3d parent extends the 3D context's map
    Mat4 acc;
    auto step = [&](auto&& self, u32 a) -> void {
        const Node& n = m_nodes[a];
        if (n.parent != kNone) self(self, n.parent);
        const bool inCtx = n.parent != kNone && preserves3d(n.parent);
        const bool parentPersp = n.parent != kNone && m_styles.style(m_nodes[n.parent].style).perspective > 0.f;
        const bool has = m_styles.style(n.style).transformCount || parentPersp;
        if (inCtx) { if (has) acc = acc * nodeMatrix(a); return; }
        if (!acc.identity()) total = mul3(total, acc.flatten());
        acc = has ? nodeMatrix(a) : Mat4{};
    };
    step(step, h.index);
    if (!acc.identity()) total = mul3(total, acc.flatten());
    return total;
}
bool UiContext::toLocal(NodeHandle h, Vec2 p, Vec2& out) const {
    const std::array<f32, 9> H = screenMap(h);
    if (H == kIdentity3) { out = p; return true; }
    std::array<f32, 9> Hi;
    return invert3(H, Hi) && mapHomography(Hi, p, out);
}
bool UiContext::fromLocal(NodeHandle h, Vec2 p, Vec2& out) const {
    const std::array<f32, 9> H = screenMap(h);
    if (H == kIdentity3) { out = p; return true; }
    return mapHomography(H, p, out);
}
NodeHandle UiContext::hitTest(Vec2 p) const { return hitRec(m_root.index, p, Mat4{}); }

bool UiContext::dispatch(u32 target, const Event& ev) {
    Event e = ev;
    if (std::isnan(e.client.x)) e.client = e.pos;
    m_path.clear();
    for (u32 a = target; a != kNone; a = m_nodes[a].parent) m_path.push_back(a);
    const NodeHandle th{target, m_nodes[target].gen};
    auto call = [&](u32 k, Phase ph) {
        if (!m_nodes[k].alive) return false;
        const WidgetClass& wc = m_registry.get(m_nodes[k].cls);
        if (!wc.onEvent) return false;
        EventContext ec{*this, {k, m_nodes[k].gen}, th, ph};
        // pointer positions in the receiving node's own (untransformed) space; client keeps context coordinates
        const std::array<f32, 9> H = screenMap({k, m_nodes[k].gen});
        if (H == kIdentity3) return wc.onEvent(ec, e);
        Event le = e;
        if (!toLocal({k, m_nodes[k].gen}, e.client, le.pos)) le.pos = {std::numeric_limits<f32>::quiet_NaN(), std::numeric_limits<f32>::quiet_NaN()};
        return wc.onEvent(ec, le);
    };
    for (std::size_t k = m_path.size(); k-- > 1;)
        if (call(m_path[k], Phase::Capture)) return true;
    if (call(target, Phase::Target)) return true;
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
        if (!m_styles.style(m_nodes[a].style).visible) return false;
    return true;
}

bool UiContext::pointerMove(Vec2 p) {
    m_pointer = p;
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
        // like Qt's click focus: a press with no focus-accepting ancestor leaves focus where it is (tool buttons,
        // on-screen keyboard keys); the background (root) still clears it above
        if (f != kNone) setFocus(handleOf(f), FocusReason::Pointer);
    }
    Event e; e.type = EventType::PointerDown; e.pos = p; e.button = b; e.mods = m_mods;
    dispatch(target, e);
    return true;
}

bool UiContext::pointerUp(Vec2 p, MouseButton b) {
    m_pointer = p;
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
    Event e; e.type = EventType::PointerUp; e.pos = p; e.button = b; e.mods = m_mods;
    dispatch(pr.index, e);
    const NodeHandle h = hitTest(p);
    bool inside = false;
    for (u32 a = h.valid() ? h.index : kNone; a != kNone; a = m_nodes[a].parent)
        if (a == pr.index) { inside = true; break; }
    if (inside && node(pr) && !(m_nodes[pr.index].state & kStateDisabled)) {
        Event c; c.type = EventType::Click; c.pos = p; c.button = b; c.mods = m_mods;
        if (!dispatch(pr.index, c) && node(pr)) emit(pr, SignalKind::Clicked, {});
    }
    return true;
}

bool UiContext::wheel(Vec2 p, Vec2 notches) {
    const NodeHandle h = hitTest(p);
    if (!h.valid()) return false;
    if (node(m_modal) && !inScope(h.index, m_modal.index)) return true;
    Event e; e.type = EventType::Wheel; e.pos = p; e.wheel = notches; e.mods = m_mods;
    if (dispatch(h.index, e)) return true;
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

bool UiContext::keyDown(Key k, u8 mods) {
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
    m_focusBackward = backward;
    setFocus(handleOf(m_scratchOrder[nx]), FocusReason::Keyboard);
    m_focusBackward = false;
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
        if (m_focusBackward) e.mods = kModShift; // Shift+Tab: widgets with inner parts (date sections) enter at the end
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

void UiContext::dumpA11y(std::string& out) const {
    auto rec = [&](auto&& self, u32 i, int depth) -> void {
        const Node& n = m_nodes[i];
        if (!m_styles.style(n.style).visible) return;
        int d = depth;
        if (n.role != A11yRole::None) {
            out.append(std::size_t(depth) * 2, ' ');
            out += a11yRoleName(n.role);
            const std::string& nm = m_cold[i]->a11yName.empty() ? m_cold[i]->text : m_cold[i]->a11yName;
            if (!nm.empty()) { out += " \""; out += nm; out += '"'; }
            if (n.flags & kFlagFocusable) out += " focusable";
            if (m_focus.index == i && m_focus.generation == n.gen) out += " focused";
            if (n.state & kStateChecked) out += " checked";
            if (n.state & kStateDisabled) out += " disabled";
            switch (n.role) {
            case A11yRole::Slider: case A11yRole::ProgressBar: case A11yRole::SpinBox: case A11yRole::Dial: case A11yRole::ScrollBar:
                appendf(out, " value=%g range=[%g,%g]", double(n.value), double(n.lo), double(n.hi));
                break;
            default: break;
            }
            out += '\n';
            d = depth + 1;
        }
        for (u32 k = n.first; k != kNone; k = m_nodes[k].next) self(self, k, d);
    };
    rec(rec, m_root.index, 0);
}

void UiContext::snapshotA11y(std::vector<A11ySnapshotNode>& out) const {
    out.clear();
    auto rec = [&](auto&& self, u32 i, u32 parent) -> void {
        const Node& n = m_nodes[i];
        if (!m_styles.style(n.style).visible) return;
        u32 p = parent;
        if (n.role != A11yRole::None || i == m_root.index) {
            A11ySnapshotNode s;
            s.node = {i, n.gen};
            s.parent = parent;
            s.role = n.role == A11yRole::None ? A11yRole::Window : n.role;
            s.name = m_cold[i]->a11yName.empty() ? m_cold[i]->text : m_cold[i]->a11yName;
            s.bounds = n.rect;
            s.focusable = (n.flags & kFlagFocusable) != 0;
            s.focused = m_focus.index == i && m_focus.generation == n.gen;
            s.checked = (n.state & kStateChecked) != 0;
            s.disabled = (n.state & kStateDisabled) != 0;
            s.protectedText = m_cold[i]->a11yProtected;
            s.state = m_cold[i]->a11y;
            if (!node(s.state.activeDescendant)) s.state.activeDescendant = {};
            if (!node(s.state.controls)) s.state.controls = {};
            s.value = n.value;
            s.lo = n.lo;
            s.hi = n.hi;
            if (auto f = m_registry.get(n.cls).a11yValueText) s.valueText = f(*this, s.node);
            p = u32(out.size());
            const NodeHandle owner = s.node;
            const bool ownerFocused = s.focused;
            out.push_back(std::move(s));
            if (auto f = m_registry.get(n.cls).a11yParts) {
                std::vector<A11ySnapshotNode> parts;
                f(*this, owner, out[p], parts);
                const u32 base = u32(out.size());
                for (auto& q : parts) {
                    q.node = owner;
                    q.parent = q.parent == ~0u ? p : base + q.parent;
                    q.focused = q.focused && ownerFocused;
                    out.push_back(std::move(q));
                }
            }
        }
        for (u32 k = n.first; k != kNone; k = m_nodes[k].next) self(self, k, p);
    };
    rec(rec, m_root.index, ~0u);
}

} // namespace fuse::ui
