// FUSE UI (UI2): UiContext — tree, cascade, layout, routing, focus, timers. See context.hpp.
#include <fuse/ui/context.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>

namespace fuse::ui {

struct UiContext::Node {
    u32 generation = 0;
    bool live = false;
    ClassId cls = kInvalidClass;
    NodeId parent, first, last, next, prev;
    std::string name, classes, text, a11yName;
    f32 value = 0.f, min = 0.f, max = 1.f;
    u32 cursor = 0;
    u32 state = 0;
    bool visible = true;
    bool modal = false;
    bool styleDirty = true;
    LayoutProps layout{};
    Style computed{};
    Style overrideValues{};
    u32 overrideMask = 0;
    Rect rect{};
    f32 scroll = 0.f;
    f32 contentHeight = 0.f;
    std::vector<Callback> clicked, changed;
    std::vector<EventFilter> filters;

    void reset() {
        const u32 gen = generation;
        *this = Node{};
        generation = gen;
    }
};

namespace {
const std::string kEmpty;
const LayoutProps kDefaultLayout{};
const Style kDefaultStyle{};

bool hasWord(std::string_view words, std::string_view w) {
    usize i = 0;
    while (i < words.size()) {
        while (i < words.size() && words[i] == ' ') ++i;
        usize j = i;
        while (j < words.size() && words[j] != ' ') ++j;
        if (words.substr(i, j - i) == w) return true;
        i = j;
    }
    return false;
}

void applyStyle(Style& dst, const Style& src, u32 mask) {
    if (mask & kStyleBackground) dst.background = src.background;
    if (mask & kStyleForeground) dst.foreground = src.foreground;
    if (mask & kStyleBorder) dst.border = src.border;
    if (mask & kStyleAccent) dst.accent = src.accent;
    if (mask & kStyleBorderWidth) dst.borderWidth = src.borderWidth;
    if (mask & kStyleRadius) dst.radius = src.radius;
    if (mask & kStyleTextScale) dst.textScale = src.textScale;
    if (mask & kStyleOpacity) dst.opacity = src.opacity;
}
} // namespace

UiContext::UiContext() {
    registerBuiltinWidgets(*this);
    m_root = create("Panel", NodeId{}, "root");
}

UiContext::~UiContext() {
    for (Node* n : m_nodes) delete n;
}

ClassId UiContext::registerClass(WidgetClass cls) {
    const ClassId existing = findClass(cls.name);
    if (existing != kInvalidClass) {
        m_classes[existing] = std::move(cls);
        return existing;
    }
    m_classes.push_back(std::move(cls));
    return ClassId(m_classes.size() - 1);
}

ClassId UiContext::findClass(std::string_view name) const {
    for (usize i = 0; i < m_classes.size(); ++i) {
        if (m_classes[i].name == name) return ClassId(i);
    }
    return kInvalidClass;
}

UiContext::Node* UiContext::get(NodeId id) {
    if (!id.isValid() || id.index() >= m_nodes.size()) return nullptr;
    Node* n = m_nodes[id.index()];
    return (n->live && n->generation == id.generation()) ? n : nullptr;
}

const UiContext::Node* UiContext::get(NodeId id) const { return const_cast<UiContext*>(this)->get(id); }

NodeId UiContext::create(ClassId cls, NodeId parent, std::string_view name) {
    if (cls >= m_classes.size()) return {};
    if (parent.isValid() && !get(parent)) return {};
    u32 index;
    if (!m_free.empty()) {
        index = m_free.back();
        m_free.pop_back();
    } else {
        index = u32(m_nodes.size());
        m_nodes.push_back(new Node{});
    }
    Node& n = *m_nodes[index];
    n.reset();
    n.live = true;
    n.cls = cls;
    n.name = std::string(name);
    n.layout.scroll = m_classes[cls].name == "ScrollView";
    const NodeId id{index, n.generation};
    if (Node* p = get(parent)) {
        n.parent = parent;
        n.prev = p->last;
        if (Node* last = get(p->last)) last->next = id;
        else p->first = id;
        p->last = id;
    }
    ++m_liveCount;
    markStyle(id);
    markLayout(id);
    return id;
}

NodeId UiContext::create(std::string_view className, NodeId parent, std::string_view name) {
    return create(findClass(className), parent, name);
}

void UiContext::destroy(NodeId id) {
    Node* n = get(id);
    if (!n || id == m_root) return;
    while (n->first.isValid()) destroy(n->first);
    if (Node* p = get(n->parent)) {
        if (p->first == id) p->first = n->next;
        if (p->last == id) p->last = n->prev;
    }
    if (Node* prev = get(n->prev)) prev->next = n->next;
    if (Node* next = get(n->next)) next->prev = n->prev;
    if (m_focus == id) m_focus = {};
    if (m_hover == id) m_hover = {};
    if (m_capture == id) m_capture = {};
    m_damage = m_damage.unite(n->rect);
    n->live = false;
    ++n->generation;
    m_free.push_back(id.index());
    --m_liveCount;
    m_layoutDirty = m_paintDirty = true;
}

bool UiContext::alive(NodeId id) const { return get(id) != nullptr; }
NodeId UiContext::parent(NodeId id) const { const Node* n = get(id); return n ? n->parent : NodeId{}; }
NodeId UiContext::firstChild(NodeId id) const { const Node* n = get(id); return n ? n->first : NodeId{}; }
NodeId UiContext::nextSibling(NodeId id) const { const Node* n = get(id); return n ? n->next : NodeId{}; }
ClassId UiContext::classOf(NodeId id) const { const Node* n = get(id); return n ? n->cls : kInvalidClass; }

u32 UiContext::childCount(NodeId id) const {
    u32 count = 0;
    for (NodeId c = firstChild(id); c.isValid(); c = nextSibling(c)) ++count;
    return count;
}

NodeId UiContext::find(std::string_view name, NodeId from) const {
    if (!from.isValid()) from = m_root;
    const Node* n = get(from);
    if (!n) return {};
    if (n->name == name) return from;
    for (NodeId c = n->first; c.isValid(); c = nextSibling(c)) {
        const NodeId hit = find(name, c);
        if (hit.isValid()) return hit;
    }
    return {};
}

// ---- properties ------------------------------------------------------------------------------------------

const std::string& UiContext::name(NodeId id) const { const Node* n = get(id); return n ? n->name : kEmpty; }
const std::string& UiContext::text(NodeId id) const { const Node* n = get(id); return n ? n->text : kEmpty; }

void UiContext::setClasses(NodeId id, std::string_view classes) {
    if (Node* n = get(id); n && n->classes != classes) {
        n->classes = std::string(classes);
        markStyle(id);
    }
}

void UiContext::setText(NodeId id, std::string_view text) {
    Node* n = get(id);
    if (!n || n->text == text) return;
    n->text = std::string(text);
    n->cursor = std::min<u32>(n->cursor, u32(n->text.size()));
    markLayout(id);
}

f32 UiContext::value(NodeId id) const { const Node* n = get(id); return n ? n->value : 0.f; }

void UiContext::setValue(NodeId id, f32 v) {
    Node* n = get(id);
    if (!n) return;
    v = std::clamp(v, n->min, n->max);
    if (v == n->value) return;
    n->value = v;
    markPaint(id);
    emitChanged(id);
}

void UiContext::setRange(NodeId id, f32 min, f32 max) {
    if (Node* n = get(id)) {
        n->min = min;
        n->max = std::max(min, max);
        n->value = std::clamp(n->value, n->min, n->max);
        markPaint(id);
    }
}

Vec2 UiContext::range(NodeId id) const { const Node* n = get(id); return n ? Vec2{n->min, n->max} : Vec2{}; }

void UiContext::setChecked(NodeId id, bool on) {
    if (!get(id) || hasState(id, kStateChecked) == on) return;
    setStateFlags(id, kStateChecked, on);
    emitChanged(id);
}

void UiContext::setEnabled(NodeId id, bool enabled) {
    setStateFlags(id, kStateDisabled, !enabled);
    if (!enabled && m_focus == id) setFocus({});
}

bool UiContext::hasState(NodeId id, u32 flags) const { const Node* n = get(id); return n && (n->state & flags) == flags; }
u32 UiContext::state(NodeId id) const { const Node* n = get(id); return n ? n->state : 0u; }

void UiContext::setStateFlags(NodeId id, u32 flags, bool on) {
    Node* n = get(id);
    if (!n) return;
    const u32 next = on ? (n->state | flags) : (n->state & ~flags);
    if (next == n->state) return;
    n->state = next;
    markStyle(id);
    markPaint(id);
}

void UiContext::setVisible(NodeId id, bool v) {
    if (Node* n = get(id); n && n->visible != v) {
        n->visible = v;
        m_damage = m_damage.unite(n->rect);
        markLayout(id);
    }
}

bool UiContext::visible(NodeId id) const { const Node* n = get(id); return n && n->visible; }

LayoutProps& UiContext::layout(NodeId id) {
    static LayoutProps dummy;
    Node* n = get(id);
    if (!n) return dummy;
    markLayout(id);
    return n->layout;
}

const LayoutProps& UiContext::layoutProps(NodeId id) const { const Node* n = get(id); return n ? n->layout : kDefaultLayout; }

void UiContext::setStyleOverride(NodeId id, const Style& values, u32 mask) {
    if (Node* n = get(id)) {
        applyStyle(n->overrideValues, values, mask);
        n->overrideMask |= mask;
        markStyle(id);
    }
}

const Style& UiContext::style(NodeId id) const { const Node* n = get(id); return n ? n->computed : kDefaultStyle; }
Rect UiContext::rect(NodeId id) const { const Node* n = get(id); return n ? n->rect : Rect{}; }
f32 UiContext::scrollOffset(NodeId id) const { const Node* n = get(id); return n ? n->scroll : 0.f; }

void UiContext::setScrollOffset(NodeId id, f32 offset) {
    Node* n = get(id);
    if (!n) return;
    const f32 maxScroll = std::max(0.f, n->contentHeight - n->rect.inset(n->layout.padding).h);
    offset = std::clamp(offset, 0.f, maxScroll);
    if (offset == n->scroll) return;
    n->scroll = offset;
    markLayout(id);
}

void UiContext::setModal(NodeId id, bool modal) { if (Node* n = get(id)) n->modal = modal; }
void UiContext::setAccessibleName(NodeId id, std::string_view nm) { if (Node* n = get(id)) n->a11yName = std::string(nm); }
u32 UiContext::cursor(NodeId id) const { const Node* n = get(id); return n ? n->cursor : 0u; }
void UiContext::setCursor(NodeId id, u32 c) {
    if (Node* n = get(id)) {
        n->cursor = std::min<u32>(c, u32(n->text.size()));
        markPaint(id);
    }
}

// ---- signals ---------------------------------------------------------------------------------------------

void UiContext::onClicked(NodeId id, Callback cb) { if (Node* n = get(id)) n->clicked.push_back(std::move(cb)); }
void UiContext::onChanged(NodeId id, Callback cb) { if (Node* n = get(id)) n->changed.push_back(std::move(cb)); }
void UiContext::addEventFilter(NodeId id, EventFilter f) { if (Node* n = get(id)) n->filters.push_back(std::move(f)); }

void UiContext::emitClicked(NodeId id) {
    Node* n = get(id);
    if (!n) return;
    const auto slots = n->clicked; // a slot may destroy the node
    for (const Callback& cb : slots) cb(*this, id);
}

void UiContext::emitChanged(NodeId id) {
    Node* n = get(id);
    if (!n) return;
    const auto slots = n->changed;
    for (const Callback& cb : slots) cb(*this, id);
}

// ---- dirty tracking --------------------------------------------------------------------------------------

void UiContext::markStyle(NodeId id) {
    if (Node* n = get(id)) n->styleDirty = true;
    m_styleDirty = true;
}

void UiContext::markLayout(NodeId id) {
    (void)id;
    m_layoutDirty = true;
    m_paintDirty = true;
}

void UiContext::markPaint(NodeId id) {
    if (const Node* n = get(id)) m_damage = m_damage.unite(n->rect);
    m_paintDirty = true;
}

void UiContext::invalidateStyle() {
    for (Node* n : m_nodes) n->styleDirty = true;
    m_styleDirty = true;
}

bool UiContext::needsUpdate() const {
    return m_styleDirty || m_layoutDirty || m_paintDirty || !m_timers.empty() || !m_tweens.empty() || !m_posted.empty();
}

// ---- style -----------------------------------------------------------------------------------------------

void UiContext::resolveStyles(NodeId id, UpdateStats& stats) {
    Node* n = get(id);
    if (!n) return;
    if (n->styleDirty) {
        const std::string& type = m_classes[n->cls].name;
        struct Match { int spec; usize order; };
        Match matches[64];
        int count = 0;
        const auto& rules = m_sheet.rules();
        for (usize i = 0; i < rules.size() && count < 64; ++i) {
            const StyleRule& r = rules[i];
            if (!r.type.empty() && r.type != type) continue;
            if (!r.cls.empty() && !hasWord(n->classes, r.cls)) continue;
            if (!r.name.empty() && r.name != n->name) continue;
            if ((n->state & r.states) != r.states) continue;
            const int spec = (r.name.empty() ? 0 : 100) + (r.cls.empty() ? 0 : 10) + (r.type.empty() ? 0 : 1) +
                             std::popcount(r.states) * 10;
            matches[count++] = {spec, i};
        }
        std::stable_sort(matches, matches + count, [](const Match& a, const Match& b) { return a.spec < b.spec; });
        Style s{};
        for (int i = 0; i < count; ++i) applyStyle(s, rules[matches[i].order].values, rules[matches[i].order].mask);
        applyStyle(s, n->overrideValues, n->overrideMask);
        if (!(s == n->computed)) {
            if (s.textScale != n->computed.textScale) m_layoutDirty = true;
            n->computed = s;
            m_damage = m_damage.unite(n->rect);
            m_paintDirty = true;
        }
        n->styleDirty = false;
        ++stats.nodesStyled;
    }
    for (NodeId c = n->first; c.isValid(); c = nextSibling(c)) resolveStyles(c, stats);
}

// ---- layout ----------------------------------------------------------------------------------------------

Vec2 UiContext::measureNode(NodeId id, f32 availableWidth) {
    Node* n = get(id);
    if (!n || !n->visible) return {};
    const LayoutProps& L = n->layout;
    const f32 innerAvail = (L.width >= 0.f ? L.width : availableWidth) - L.padding.horizontal();
    Vec2 content{};
    const WidgetClass& wc = m_classes[n->cls];
    if (wc.measure) content = wc.measure(*this, id, innerAvail);
    Vec2 kids{};
    u32 visibleKids = 0;
    for (NodeId c = n->first; c.isValid(); c = nextSibling(c)) {
        const Node* cn = get(c);
        if (!cn->visible) continue;
        const Vec2 cs = measureNode(c, innerAvail);
        ++visibleKids;
        switch (L.kind) {
        case LayoutKind::Column: kids.x = std::max(kids.x, cs.x); kids.y += cs.y; break;
        case LayoutKind::Row: kids.x += cs.x; kids.y = std::max(kids.y, cs.y); break;
        case LayoutKind::Stack:
        case LayoutKind::Absolute:
            kids.x = std::max(kids.x, cs.x + cn->layout.position.x + cn->layout.anchors.margin.horizontal());
            kids.y = std::max(kids.y, cs.y + cn->layout.position.y + cn->layout.anchors.margin.vertical());
            break;
        }
    }
    if (visibleKids > 1) {
        if (L.kind == LayoutKind::Column) kids.y += L.gap * f32(visibleKids - 1);
        if (L.kind == LayoutKind::Row) kids.x += L.gap * f32(visibleKids - 1);
    }
    Vec2 size{std::max(content.x, kids.x) + L.padding.horizontal(), std::max(content.y, kids.y) + L.padding.vertical()};
    if (L.width >= 0.f) size.x = L.width;
    if (L.height >= 0.f) size.y = L.height;
    size.x = std::max(size.x, L.minWidth);
    size.y = std::max(size.y, L.minHeight);
    return size;
}

void UiContext::arrange(NodeId id, Rect r, UpdateStats& stats) {
    Node* n = get(id);
    if (!n) return;
    if (!(n->rect == r)) {
        m_damage = m_damage.unite(n->rect).unite(r);
        n->rect = r;
    }
    ++stats.nodesLaidOut;
    const LayoutProps& L = n->layout;
    const Rect content = r.inset(L.padding);
    const bool column = L.kind == LayoutKind::Column;

    if (L.kind == LayoutKind::Column || L.kind == LayoutKind::Row) {
        const usize base = m_scratchKids.size();
        u32 count = 0;
        f32 total = 0.f, growSum = 0.f;
        for (NodeId c = n->first; c.isValid(); c = nextSibling(c)) {
            Node* cn = get(c);
            if (!cn->visible) continue;
            const Vec2 sz = measureNode(c, content.w);
            m_scratchSizes.push_back(sz);
            m_scratchKids.push_back(c);
            total += column ? sz.y : sz.x;
            growSum += cn->layout.grow;
            ++count;
        }
        if (count > 1) total += L.gap * f32(count - 1);
        const f32 mainSize = column ? content.h : content.w;
        f32 spare = n->layout.scroll ? 0.f : mainSize - total;
        f32 pos = column ? content.y : content.x;
        f32 between = L.gap;
        if (growSum <= 0.f && spare > 0.f) {
            if (L.justify == Justify::Center) pos += spare * 0.5f;
            else if (L.justify == Justify::End) pos += spare;
            else if (L.justify == Justify::SpaceBetween && count > 1) between += spare / f32(count - 1);
        }
        if (column && L.scroll) pos -= n->scroll;
        for (u32 i = 0; i < count; ++i) {
            const Vec2 size = m_scratchSizes[base + i];
            const NodeId kid = m_scratchKids[base + i];
            Node* cn = get(kid);
            f32 main = column ? size.y : size.x;
            if (growSum > 0.f && spare > 0.f) main += spare * (cn->layout.grow / growSum);
            const f32 crossAvail = column ? content.w : content.h;
            f32 cross = column ? size.x : size.y;
            const bool fixedCross = column ? cn->layout.width >= 0.f : cn->layout.height >= 0.f;
            f32 crossPos = column ? content.x : content.y;
            if (L.align == Align::Stretch && !fixedCross) cross = crossAvail;
            else if (L.align == Align::Center) crossPos += (crossAvail - cross) * 0.5f;
            else if (L.align == Align::End) crossPos += crossAvail - cross;
            const Rect cr = column ? Rect{crossPos, pos, cross, main} : Rect{pos, crossPos, main, cross};
            arrange(kid, cr, stats); // may push / pop above `base + count`
            pos += main + between;
        }
        m_scratchSizes.resize(base);
        m_scratchKids.resize(base);
        n->contentHeight = column ? total : content.h;
        if (L.scroll) {
            const f32 maxScroll = std::max(0.f, n->contentHeight - content.h);
            n->scroll = std::clamp(n->scroll, 0.f, maxScroll);
        }
        return;
    }

    // Stack / Absolute with anchors.
    for (NodeId c = n->first; c.isValid(); c = nextSibling(c)) {
        Node* cn = get(c);
        if (!cn->visible) continue;
        const Vec2 s = measureNode(c, content.w);
        const Anchors& a = cn->layout.anchors;
        const bool anchored = a.left || a.top || a.right || a.bottom || a.centerX || a.centerY;
        Rect cr{content.x + cn->layout.position.x, content.y + cn->layout.position.y, s.x, s.y};
        if (L.kind == LayoutKind::Stack && !anchored) {
            cr = content;
        } else if (anchored) {
            if (a.left && a.right) { cr.x = content.x + a.margin.left; cr.w = content.w - a.margin.horizontal(); }
            else if (a.left) cr.x = content.x + a.margin.left;
            else if (a.right) cr.x = content.right() - s.x - a.margin.right;
            else if (a.centerX) cr.x = content.x + (content.w - s.x) * 0.5f;
            if (a.top && a.bottom) { cr.y = content.y + a.margin.top; cr.h = content.h - a.margin.vertical(); }
            else if (a.top) cr.y = content.y + a.margin.top;
            else if (a.bottom) cr.y = content.bottom() - s.y - a.margin.bottom;
            else if (a.centerY) cr.y = content.y + (content.h - s.y) * 0.5f;
        }
        arrange(c, cr, stats);
    }
    n->contentHeight = content.h;
}

// ---- paint -----------------------------------------------------------------------------------------------

void UiContext::paintNode(NodeId id, DrawList& list, f32 opacity, const Rect& clip) const {
    const Node* n = get(id);
    if (!n || !n->visible || n->rect.empty() || n->rect.intersect(clip).empty()) return; // culled
    const Style& s = n->computed;
    opacity *= s.opacity;
    if (opacity <= 0.f) return;
    auto fade = [opacity](Color c) { c.a = u8(f32(c.a) * std::min(opacity, 1.f) + 0.5f); return c; };
    list.fillRect(n->rect, fade(s.background), s.radius);
    list.strokeRect(n->rect, fade(s.border), s.borderWidth, s.radius);
    const WidgetClass& wc = m_classes[n->cls];
    if (wc.paint) wc.paint(*this, id, list);
    const bool clips = n->layout.scroll;
    const Rect childClip = clips ? clip.intersect(n->rect.inset(n->layout.padding)) : clip;
    if (clips) list.pushClip(childClip);
    for (NodeId c = n->first; c.isValid(); c = nextSibling(c)) paintNode(c, list, opacity, childClip);
    if (clips) list.popClip();
}

// ---- frame -----------------------------------------------------------------------------------------------

void UiContext::setViewport(Vec2 size) {
    if (size == m_viewport) return;
    m_viewport = size;
    m_damage = {0.f, 0.f, size.x, size.y};
    markLayout(m_root);
}

void UiContext::post(std::function<void(UiContext&)> command) {
    std::lock_guard lock(m_postMutex);
    m_posted.push_back(std::move(command));
}

u32 UiContext::addTimer(f32 delay, std::function<void(UiContext&)> cb, bool repeat) {
    const u32 id = m_nextTimer++;
    m_timers.push_back({id, delay, delay, repeat, std::move(cb)});
    return id;
}

void UiContext::cancelTimer(u32 timerId) {
    std::erase_if(m_timers, [timerId](const Timer& t) { return t.id == timerId; });
}

void UiContext::animateValue(NodeId id, f32 target, f32 seconds) {
    std::erase_if(m_tweens, [id](const Tween& t) { return t.node == id && !t.colour; });
    if (seconds <= 0.f) { setValue(id, target); return; }
    m_tweens.push_back({id, false, value(id), target, 0.f, seconds, {}, {}});
}

void UiContext::animateBackground(NodeId id, Color target, f32 seconds) {
    std::erase_if(m_tweens, [id](const Tween& t) { return t.node == id && t.colour; });
    Style s{};
    if (seconds <= 0.f) { s.background = target; setStyleOverride(id, s, kStyleBackground); return; }
    m_tweens.push_back({id, true, 0.f, 0.f, 0.f, seconds, style(id).background, target});
}

UpdateStats UiContext::update(f32 dt) {
    UpdateStats stats{};
    {
        std::lock_guard lock(m_postMutex);
        m_draining.swap(m_posted);
    }
    for (auto& cmd : m_draining) cmd(*this);
    m_draining.clear();

    if (dt > 0.f && !m_timers.empty()) {
        std::vector<u32> due;
        for (Timer& t : m_timers) {
            t.remaining -= dt;
            if (t.remaining <= 0.f) due.push_back(t.id);
        }
        for (u32 tid : due) {
            auto it = std::find_if(m_timers.begin(), m_timers.end(), [tid](const Timer& t) { return t.id == tid; });
            if (it == m_timers.end()) continue;
            auto cb = it->cb;
            if (it->repeat) it->remaining += std::max(it->interval, 1e-6f);
            else m_timers.erase(it);
            cb(*this);
        }
    }
    if (dt > 0.f && !m_tweens.empty()) {
        for (usize i = 0; i < m_tweens.size();) {
            Tween& t = m_tweens[i];
            t.t = std::min(1.f, t.t + dt / t.duration);
            const f32 e = 1.f - (1.f - t.t) * (1.f - t.t);
            const Tween copy = t;
            const bool done = t.t >= 1.f;
            if (done) m_tweens.erase(m_tweens.begin() + i);
            else ++i;
            if (!alive(copy.node)) continue;
            if (copy.colour) {
                Style s{};
                s.background = lerp(copy.fromColour, copy.toColour, e);
                setStyleOverride(copy.node, s, kStyleBackground);
            } else {
                setValue(copy.node, copy.from + (copy.to - copy.from) * e);
            }
        }
    }

    if (m_styleDirty) {
        stats.styled = true;
        resolveStyles(m_root, stats);
        m_styleDirty = false;
    }
    if (m_layoutDirty) {
        stats.laidOut = true;
        Node* root = get(m_root);
        root->layout.width = m_viewport.x;
        root->layout.height = m_viewport.y;
        arrange(m_root, {0.f, 0.f, m_viewport.x, m_viewport.y}, stats);
        m_layoutDirty = false;
    }
    if (m_paintDirty) {
        stats.painted = true;
        m_draw.clear();
        paintNode(m_root, m_draw, 1.f, {0.f, 0.f, m_viewport.x, m_viewport.y});
        stats.damage = m_damage.intersect({0.f, 0.f, m_viewport.x, m_viewport.y});
        m_damage = {};
        m_paintDirty = false;
    }
    return stats;
}

// ---- events ----------------------------------------------------------------------------------------------

NodeId UiContext::hitTestNode(NodeId id, Vec2 p) const {
    const Node* n = get(id);
    if (!n || !n->visible || !n->rect.contains(p)) return {};
    NodeId best = id;
    for (NodeId c = n->first; c.isValid(); c = nextSibling(c)) {
        const NodeId hit = hitTestNode(c, p); // later siblings paint on top
        if (hit.isValid()) best = hit;
    }
    return best;
}

NodeId UiContext::hitTest(Vec2 p) const {
    const NodeId scope = modalScope();
    const NodeId hit = hitTestNode(scope.isValid() ? scope : m_root, p);
    return hit;
}

NodeId UiContext::modalScope() const {
    NodeId scope{};
    // The last visible modal in tree order wins (dialogs stack on top).
    std::vector<NodeId> stack{m_root};
    while (!stack.empty()) {
        const NodeId id = stack.back();
        stack.pop_back();
        const Node* n = get(id);
        if (!n || !n->visible) continue;
        if (n->modal) scope = id;
        std::vector<NodeId> kids;
        for (NodeId c = n->first; c.isValid(); c = nextSibling(c)) kids.push_back(c);
        for (auto it = kids.rbegin(); it != kids.rend(); ++it) stack.push_back(*it);
    }
    return scope;
}

bool UiContext::inSubtree(NodeId node, NodeId ancestor) const {
    for (NodeId c = node; c.isValid(); c = parent(c)) {
        if (c == ancestor) return true;
    }
    return false;
}

bool UiContext::route(NodeId target, const Event& e) {
    NodeId path[64];
    int depth = 0;
    for (NodeId c = target; c.isValid() && depth < 64; c = parent(c)) path[depth++] = c;
    // Capture: root → parent of target.
    for (int i = depth - 1; i >= 1; --i) {
        Node* n = get(path[i]);
        if (!n) return true;
        const auto filters = n->filters;
        for (const auto& f : filters) if (f(*this, path[i], e, Phase::Capture)) return true;
    }
    // Target, then bubble: target → root.
    for (int i = 0; i < depth; ++i) {
        Node* n = get(path[i]);
        if (!n) return true;
        const Phase phase = i == 0 ? Phase::Target : Phase::Bubble;
        const auto filters = n->filters;
        for (const auto& f : filters) if (f(*this, path[i], e, phase)) return true;
        n = get(path[i]);
        if (!n) return true;
        const WidgetClass& wc = m_classes[n->cls];
        if (wc.onEvent && !(n->state & kStateDisabled) && wc.onEvent(*this, path[i], e)) return true;
    }
    return false;
}

bool UiContext::dispatch(const Event& e) {
    switch (e.type) {
    case EventType::PointerMove: {
        const NodeId hit = hitTest(e.position);
        if (hit != m_hover) {
            setStateFlags(m_hover, kStateHover, false);
            m_hover = hit;
            if (hit != m_root) setStateFlags(hit, kStateHover, true);
        }
        const NodeId target = m_capture.isValid() ? m_capture : hit;
        if (!target.isValid()) return modalScope().isValid();
        const bool consumed = route(target, e);
        return consumed || target != m_root || modalScope().isValid();
    }
    case EventType::PointerDown: {
        const NodeId hit = hitTest(e.position);
        if (!hit.isValid()) return modalScope().isValid(); // blocked by a modal
        NodeId focusTarget = hit;
        while (focusTarget.isValid() && !m_classes[get(focusTarget)->cls].focusable) focusTarget = parent(focusTarget);
        setFocus(focusTarget);
        if (!hasState(hit, kStateDisabled)) setStateFlags(hit, kStatePressed, true);
        m_capture = hit;
        const bool consumed = route(hit, e);
        return consumed || hit != m_root;
    }
    case EventType::PointerUp: {
        const NodeId target = m_capture.isValid() ? m_capture : hitTest(e.position);
        if (!target.isValid()) return modalScope().isValid();
        const bool consumed = route(target, e);
        setStateFlags(target, kStatePressed, false);
        m_capture = {};
        return consumed || target != m_root;
    }
    case EventType::Wheel: {
        const NodeId hit = hitTest(e.position);
        if (!hit.isValid()) return modalScope().isValid();
        return route(hit, e) || hit != m_root;
    }
    case EventType::KeyDown: {
        if (m_focus.isValid() && route(m_focus, e)) return true;
        if (e.key == Key::Tab) {
            focusNext((e.modifiers & kModShift) != 0);
            return true;
        }
        for (const Shortcut& s : m_shortcuts) {
            if (s.key == e.key && s.mods == e.modifiers) {
                auto action = s.action;
                action(*this);
                return true;
            }
        }
        return false;
    }
    case EventType::Text:
        return m_focus.isValid() && route(m_focus, e);
    case EventType::FocusIn:
    case EventType::FocusOut:
        return false;
    }
    return false;
}

bool UiContext::setFocus(NodeId id) {
    if (id.isValid()) {
        const Node* n = get(id);
        if (!n || !m_classes[n->cls].focusable || (n->state & kStateDisabled)) return false;
        for (NodeId c = id; c.isValid(); c = parent(c)) if (!visible(c)) return false;
    }
    if (id == m_focus) return true;
    setStateFlags(m_focus, kStateFocus, false);
    m_focus = id;
    setStateFlags(m_focus, kStateFocus, true);
    return true;
}

void UiContext::collectFocusable(NodeId id, std::vector<NodeId>& out) const {
    const Node* n = get(id);
    if (!n || !n->visible) return;
    if (m_classes[n->cls].focusable && !(n->state & kStateDisabled)) out.push_back(id);
    for (NodeId c = n->first; c.isValid(); c = nextSibling(c)) collectFocusable(c, out);
}

void UiContext::focusNext(bool backwards) {
    std::vector<NodeId> order;
    const NodeId scope = modalScope();
    collectFocusable(scope.isValid() ? scope : m_root, order);
    if (order.empty()) return;
    auto it = std::find(order.begin(), order.end(), m_focus);
    usize i;
    if (it == order.end()) i = backwards ? order.size() - 1 : 0;
    else {
        const usize cur = usize(it - order.begin());
        i = backwards ? (cur + order.size() - 1) % order.size() : (cur + 1) % order.size();
    }
    setFocus(order[i]);
}

void UiContext::addShortcut(Key key, u8 mods, std::function<void(UiContext&)> action) {
    m_shortcuts.push_back({key, mods, std::move(action)});
}

// ---- introspection ---------------------------------------------------------------------------------------

void UiContext::dumpNode(NodeId id, int depth, std::string& out, bool a11y) const {
    const Node* n = get(id);
    if (!n || (a11y && !n->visible)) return;
    out.append(usize(depth) * 2u, ' ');
    char buf[160];
    const WidgetClass& wc = m_classes[n->cls];
    if (a11y) {
        const std::string& label = !n->a11yName.empty() ? n->a11yName : (!n->text.empty() ? n->text : n->name);
        out += wc.role;
        out += " \"" + label + "\"";
        if (n->state & kStateFocus) out += " focused";
        if (n->state & kStateDisabled) out += " disabled";
        if (wc.name == "CheckBox") out += (n->state & kStateChecked) ? " checked" : " unchecked";
        if (wc.name == "Slider" || wc.name == "ProgressBar") {
            std::snprintf(buf, sizeof buf, " value=%g", double(n->value));
            out += buf;
        }
    } else {
        out += wc.name;
        if (!n->name.empty()) out += "#" + n->name;
        std::snprintf(buf, sizeof buf, " [%g,%g %gx%g]", double(n->rect.x), double(n->rect.y), double(n->rect.w), double(n->rect.h));
        out += buf;
        if (!n->visible) out += " hidden";
        if (!n->text.empty()) out += " \"" + n->text + "\"";
    }
    out += '\n';
    for (NodeId c = n->first; c.isValid(); c = nextSibling(c)) dumpNode(c, depth + 1, out, a11y);
}

std::string UiContext::dumpTree() const {
    std::string out;
    dumpNode(m_root, 0, out, false);
    return out;
}

std::string UiContext::dumpAccessibility() const {
    std::string out;
    dumpNode(m_root, 0, out, true);
    return out;
}

} // namespace fuse::ui
