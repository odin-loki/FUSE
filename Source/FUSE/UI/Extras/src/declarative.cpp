// Declarative bindings + hierarchical state machine. See declarative.hpp.
#include <fuse/ui/extras/declarative.hpp>

#include <cstdio>

namespace fuse::ui::decl {

namespace detail {
Tracker& tracker() {
    thread_local Tracker t;
    return t;
}
} // namespace detail

PropertyBase::~PropertyBase() {
    dropDeps();
    for (PropertyBase* d : m_dependents) std::erase(d->m_deps, this);
}

void PropertyBase::dropDeps() {
    for (PropertyBase* d : m_deps) std::erase(d->m_dependents, this);
    m_deps.clear();
}

void PropertyBase::clearBinding() {
    m_eval = {};
    dropDeps();
}

int PropertyBase::onChanged(std::function<void()> fn) {
    m_observers.emplace_back(m_nextToken, std::move(fn));
    return m_nextToken++;
}

void PropertyBase::disconnect(int token) {
    std::erase_if(m_observers, [token](const auto& o) { return o.first == token; });
}

void PropertyBase::noteRead() const {
    PropertyBase* cur = detail::tracker().current;
    if (!cur) return;
    auto* self = const_cast<PropertyBase*>(this);
    if (cur == self) { cur->m_loop = true; return; } // a binding reading itself
    if (std::find(cur->m_deps.begin(), cur->m_deps.end(), self) == cur->m_deps.end()) {
        cur->m_deps.push_back(self);
        self->m_dependents.push_back(cur);
    }
}

void PropertyBase::notifyChanged() {
    const auto observers = m_observers; // observers may disconnect themselves
    for (const auto& [tok, fn] : observers) if (fn) fn();
    const auto dependents = m_dependents;
    for (PropertyBase* d : dependents)
        if (std::find(m_dependents.begin(), m_dependents.end(), d) != m_dependents.end()) d->evaluate();
}

void PropertyBase::setEvaluator(std::function<bool()> eval) {
    dropDeps();
    m_loop = false;
    m_eval = std::move(eval);
    evaluate();
}

void PropertyBase::evaluate() {
    if (!m_eval) return;
    if (m_evaluating) { // re-entered through its own dependents: binding loop
        m_loop = true;
        std::fprintf(stderr, "fuse::decl: binding loop detected; binding dropped\n");
        m_eval = {};
        dropDeps();
        return;
    }
    m_evaluating = true;
    dropDeps();
    detail::Tracker& t = detail::tracker();
    PropertyBase* prev = t.current;
    t.current = this;
    const std::function<bool()> eval = m_eval; // the binding may replace itself
    bool changed = eval();
    t.current = prev;
    if (m_loop) { m_eval = {}; dropDeps(); changed = false; }
    if (changed) notifyChanged();
    m_evaluating = false;
}

Effect::Effect(std::function<void()> fn) {
    setEvaluator([f = std::move(fn)] { f(); return false; });
}

namespace {
struct UiEffect final : Effect {
    UiEffect(std::function<void()> fn) : Effect(std::move(fn)) {}
};
struct TwoWay final : Effect {
    TwoWay(UiContext& ui, std::function<void()> fn) : Effect(std::move(fn)), ui(&ui) {}
    ~TwoWay() override { if (conn.valid()) ui->disconnect(conn); }
    UiContext* ui;
    ConnectionHandle conn{};
};
} // namespace

EffectPtr bindText(UiContext& ui, NodeHandle n, std::function<std::string()> expr) {
    return std::make_unique<UiEffect>([&ui, n, e = std::move(expr)] { const std::string s = e(); if (ui.alive(n) && ui.text(n) != s) ui.setText(n, s); });
}
EffectPtr bindValue(UiContext& ui, NodeHandle n, std::function<f32()> expr) {
    return std::make_unique<UiEffect>([&ui, n, e = std::move(expr)] { const f32 v = e(); if (ui.alive(n) && ui.value(n) != v) ui.setValue(n, v); });
}
EffectPtr bindCss(UiContext& ui, NodeHandle n, std::string prop, std::function<std::string()> expr) {
    return std::make_unique<UiEffect>([&ui, n, p = std::move(prop), e = std::move(expr)] { const std::string v = e(); if (ui.alive(n)) ui.setCss(n, p, v); });
}
EffectPtr bindVisible(UiContext& ui, NodeHandle n, std::function<bool()> expr) {
    return std::make_unique<UiEffect>([&ui, n, e = std::move(expr)] { const bool v = e(); if (ui.alive(n)) ui.setCss(n, "display", v ? "flex" : "none"); });
}
EffectPtr bindTextTwoWay(UiContext& ui, NodeHandle n, Property<std::string>& p) {
    auto fx = std::make_unique<TwoWay>(ui, [&ui, n, &p] {
        const std::string& s = p.value();
        if (!ui.alive(n)) return;
        auto& buf = ui.editBuffer(n); // editors keep their text in the edit buffer
        if (buf.text() != s) { buf.setText(s); ui.invalidate(n); }
    });
    fx->conn = ui.connect(n, SignalKind::TextChanged, [pp = &p](UiContext&, NodeHandle, const SignalArgs& a) { if (pp->peek() != a.text) pp->setValue(std::string(a.text)); });
    return fx;
}
EffectPtr bindValueTwoWay(UiContext& ui, NodeHandle n, Property<f32>& p) {
    auto fx = std::make_unique<TwoWay>(ui, [&ui, n, &p] { const f32 v = p.value(); if (ui.alive(n) && ui.value(n) != v) ui.setValue(n, v); });
    fx->conn = ui.connect(n, SignalKind::ValueChanged, [pp = &p](UiContext&, NodeHandle, const SignalArgs& a) { if (pp->peek() != a.value) pp->setValue(a.value); });
    return fx;
}

// ---------------------------------------------------------------------------------------------------------------

struct StateMachine::Impl {
    struct Transition {
        std::string event;
        StateId target;
        std::function<bool()> guard;
        std::function<void()> action;
    };
    struct State {
        std::string name;
        StateId parent = kNone;
        Kind kind = Kind::Atomic;
        std::vector<StateId> children;
        StateId initial = kNone;
        StateId historyDefault = kNone;
        std::vector<StateId> historyValue;
        std::vector<std::function<void()>> entry, exit;
        std::vector<Transition> transitions;
    };
    std::vector<State> st;
    std::vector<bool> active;
    std::deque<std::string> queue;
    bool processing = false;
    StateMachine* owner = nullptr;

    bool isAncestor(StateId a, StateId d) const { // a is a proper ancestor of d
        for (StateId p = st[std::size_t(d)].parent; p != kNone; p = st[std::size_t(p)].parent)
            if (p == a) return true;
        return false;
    }
    bool compoundLike(StateId s) const { const Kind k = st[std::size_t(s)].kind; return k == Kind::Compound || k == Kind::Parallel || s == 0; }
    bool isHistory(StateId s) const { const Kind k = st[std::size_t(s)].kind; return k == Kind::ShallowHistory || k == Kind::DeepHistory; }
    StateId initialOf(StateId s) const {
        const State& x = st[std::size_t(s)];
        if (x.initial != kNone) return x.initial;
        for (StateId c : x.children) if (!isHistory(c)) return c;
        return kNone;
    }

    void activate(StateId s) {
        if (active[std::size_t(s)]) return;
        active[std::size_t(s)] = true;
        for (auto& f : st[std::size_t(s)].entry) if (f) f();
        const State& x = st[std::size_t(s)];
        if (x.kind == Kind::Final) {
            if (x.parent == 0) {
                if (owner->onFinished) owner->onFinished();
            } else {
                queue.push_back("done.state." + st[std::size_t(x.parent)].name);
            }
        }
    }
    void enterAncestors(StateId s) {
        if (s == kNone || s == 0 || active[std::size_t(s)]) return;
        enterAncestors(st[std::size_t(s)].parent);
        activate(s);
    }
    /// Default-enter every active compound without an active child and every inactive region of active parallels.
    void complete() {
        for (std::size_t i = 0; i < st.size(); ++i) {
            if (!active[i] || !compoundLike(StateId(i))) continue;
            const State& x = st[i];
            if (x.kind == Kind::Parallel) {
                for (StateId c : x.children) if (!isHistory(c) && !active[std::size_t(c)]) activate(c);
            } else {
                bool any = false;
                for (StateId c : x.children) any = any || active[std::size_t(c)];
                if (!any) { if (const StateId c = initialOf(StateId(i)); c != kNone) enterTarget(c, false); }
            }
        }
    }
    void enterTarget(StateId t, bool finish = true) {
        if (isHistory(t)) {
            State& h = st[std::size_t(t)];
            enterAncestors(h.parent);
            if (!h.historyValue.empty()) {
                for (StateId s : h.historyValue) enterAncestors(s);
            } else if (h.historyDefault != kNone) {
                enterTarget(h.historyDefault, false);
            }
        } else {
            enterAncestors(t);
        }
        if (finish) complete();
    }
    void exitDescendants(StateId lca) {
        std::vector<StateId> out;
        for (std::size_t i = 0; i < st.size(); ++i)
            if (active[i] && isAncestor(lca, StateId(i))) out.push_back(StateId(i));
        // record history before anything leaves
        for (StateId s : out) {
            for (StateId h : st[std::size_t(s)].children) {
                if (!isHistory(h)) continue;
                std::vector<StateId>& v = st[std::size_t(h)].historyValue;
                v.clear();
                const bool deep = st[std::size_t(h)].kind == Kind::DeepHistory;
                for (std::size_t i = 0; i < st.size(); ++i) {
                    if (!active[i] || !isAncestor(s, StateId(i))) continue;
                    if (deep ? st[i].children.empty() || st[i].kind == Kind::Final : st[i].parent == s) v.push_back(StateId(i));
                }
            }
        }
        auto depth = [this](StateId s) { int d = 0; for (; s != kNone; s = st[std::size_t(s)].parent) ++d; return d; };
        std::stable_sort(out.begin(), out.end(), [&](StateId a, StateId b) { const int da = depth(a), db = depth(b); return da != db ? da > db : a > b; });
        for (StateId s : out) {
            for (auto& f : st[std::size_t(s)].exit) if (f) f();
            active[std::size_t(s)] = false;
        }
    }
    StateId lcca(StateId src, StateId tgt) const {
        for (StateId p = st[std::size_t(src)].parent; p != kNone; p = st[std::size_t(p)].parent)
            if (st[std::size_t(p)].kind != Kind::Parallel && (p == 0 || isAncestor(p, tgt))) return p;
        return 0;
    }
    /// One microstep for `event` ("" = eventless). Returns true when any transition fired.
    bool step(const std::string& event) {
        std::vector<std::pair<StateId, const Transition*>> chosen;
        for (std::size_t i = 0; i < st.size(); ++i) {
            if (!active[i]) continue;
            bool childActive = false;
            for (StateId c : st[i].children) childActive = childActive || active[std::size_t(c)];
            if (childActive) continue; // innermost active states only
            for (StateId s = StateId(i); s != kNone; s = st[std::size_t(s)].parent) {
                const Transition* hit = nullptr;
                for (const Transition& t : st[std::size_t(s)].transitions)
                    if (t.event == event && (!t.guard || t.guard())) { hit = &t; break; }
                if (hit) {
                    if (std::none_of(chosen.begin(), chosen.end(), [hit](auto& c) { return c.second == hit; })) chosen.emplace_back(s, hit);
                    break;
                }
            }
        }
        bool fired = false;
        for (auto [src, t] : chosen) {
            if (!active[std::size_t(src)]) continue; // exited by an earlier transition this step
            fired = true;
            if (t->target == kNone) { if (t->action) t->action(); continue; }
            const StateId lca = lcca(src, t->target);
            exitDescendants(lca);
            if (t->action) t->action();
            enterTarget(t->target);
        }
        return fired;
    }
    void drainEventless() {
        for (int guard = 0; guard < 1000 && step(std::string{}); ++guard) {}
    }
    void run() {
        if (processing) return;
        processing = true;
        drainEventless();
        while (!queue.empty() && owner->running()) {
            std::string ev = std::move(queue.front());
            queue.pop_front();
            step(ev);
            drainEventless();
        }
        processing = false;
    }
};

StateMachine::StateMachine() : m(std::make_unique<Impl>()) {
    m->owner = this;
    m->st.push_back({.name = "machine", .kind = Kind::Compound});
    m->active.push_back(false);
}
StateMachine::~StateMachine() = default;

StateMachine::StateId StateMachine::addState(std::string name, StateId parent, Kind kind) {
    const StateId id = StateId(m->st.size());
    m->st.push_back({.name = std::move(name), .parent = parent, .kind = kind});
    m->active.push_back(false);
    auto& p = m->st[std::size_t(parent)];
    p.children.push_back(id);
    if (p.kind == Kind::Atomic) p.kind = Kind::Compound; // a child makes it compound
    return id;
}
StateMachine::StateId StateMachine::addHistory(StateId parent, bool deep, StateId defaultTarget) {
    const StateId h = addState(m->st[std::size_t(parent)].name + (deep ? ".H*" : ".H"), parent, deep ? Kind::DeepHistory : Kind::ShallowHistory);
    m->st[std::size_t(h)].historyDefault = defaultTarget;
    return h;
}
void StateMachine::setInitial(StateId compound, StateId child) { m->st[std::size_t(compound)].initial = child; }
void StateMachine::onEntry(StateId s, std::function<void()> fn) { m->st[std::size_t(s)].entry.push_back(std::move(fn)); }
void StateMachine::onExit(StateId s, std::function<void()> fn) { m->st[std::size_t(s)].exit.push_back(std::move(fn)); }
int StateMachine::addTransition(StateId from, std::string event, StateId target, std::function<bool()> guard, std::function<void()> action) {
    auto& v = m->st[std::size_t(from)].transitions;
    v.push_back({std::move(event), target, std::move(guard), std::move(action)});
    return int(v.size()) - 1;
}
void StateMachine::start() {
    if (m_running) return;
    m_running = true;
    m->active[0] = true;
    m->complete();
    m->run();
}
void StateMachine::stop() {
    if (!m_running) return;
    m->exitDescendants(0);
    m->active[0] = false;
    m->queue.clear();
    m_running = false;
}
void StateMachine::postEvent(std::string event) {
    if (!m_running) return;
    m->queue.push_back(std::move(event));
    m->run();
}
bool StateMachine::isActive(StateId s) const { return s >= 0 && std::size_t(s) < m->active.size() && m->active[std::size_t(s)]; }
bool StateMachine::isActive(std::string_view name) const { return isActive(find(name)); }
StateMachine::StateId StateMachine::find(std::string_view name) const {
    for (std::size_t i = 0; i < m->st.size(); ++i) if (m->st[i].name == name) return StateId(i);
    return kNone;
}
const std::string& StateMachine::name(StateId s) const { return m->st[std::size_t(s)].name; }
std::vector<std::string> StateMachine::configuration() const {
    std::vector<std::string> out;
    for (std::size_t i = 1; i < m->st.size(); ++i) {
        if (!m->active[i]) continue;
        bool leaf = true;
        for (StateId c : m->st[i].children) leaf = leaf && !m->active[std::size_t(c)];
        if (leaf) out.push_back(m->st[i].name);
    }
    return out;
}
void StateMachine::evaluate() { if (m_running) m->run(); }
void StateMachine::watch(PropertyBase& p) {
    p.onChanged([this] { evaluate(); });
}

} // namespace fuse::ui::decl
