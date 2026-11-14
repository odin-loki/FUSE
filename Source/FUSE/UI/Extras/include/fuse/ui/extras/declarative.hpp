// Declarative bindings and state machines (Qt QML property bindings / QProperty / QBindable and Qt State Machine
// parity, as a C++ library rather than a language):
//  * Property<T>: a value with change observers. setBinding(fn) makes it a live expression; every Property read
//    while fn evaluates becomes a dependency (automatic tracking, like QML), so the binding re-evaluates when any of
//    them changes. setValue() breaks the binding, as assigning a QML property does. Cycles are detected and broken.
//  * bindText / bindValue / bindCss / bindVisible: drive UI node properties from Property expressions.
//  * StateMachine: hierarchical states (compound + parallel regions), event / guard / eventless transitions, entry /
//    exit actions, shallow + deep history, final states and assignProperty() restored on exit (QState::assignProperty
//    with RestoreProperties), run-to-completion with an internal event queue.
#pragma once

#include <fuse/ui/ui.hpp>

#include <algorithm>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fuse::ui::decl {

class PropertyBase;

namespace detail {
/// The binding being evaluated on this thread (dependency capture target).
struct Tracker {
    PropertyBase* current = nullptr;
};
Tracker& tracker();
} // namespace detail

class PropertyBase {
public:
    PropertyBase() = default;
    PropertyBase(const PropertyBase&) = delete;
    PropertyBase& operator=(const PropertyBase&) = delete;
    virtual ~PropertyBase();

    bool hasBinding() const { return static_cast<bool>(m_eval); }
    /// Detach the binding, keeping the current value.
    void clearBinding();
    /// Observer called after the value changes; returns a token for disconnect().
    int onChanged(std::function<void()> fn);
    void disconnect(int token);
    /// Number of properties this binding read in its last evaluation.
    std::size_t dependencyCount() const { return m_deps.size(); }
    /// Set when a binding loop was detected (the binding was then dropped, as QML warns and breaks it).
    bool bindingLoop() const { return m_loop; }

protected:
    void noteRead() const;          ///< record this as a dependency of the evaluating binding
    void notifyChanged();           ///< run observers and re-evaluate dependents
    void setEvaluator(std::function<bool()> eval); ///< eval recomputes; returns true when the value changed
    void evaluate();

private:
    void dropDeps();
    std::function<bool()> m_eval;
    std::vector<PropertyBase*> m_deps;       ///< what my binding reads
    std::vector<PropertyBase*> m_dependents; ///< bindings reading me
    std::vector<std::pair<int, std::function<void()>>> m_observers;
    int m_nextToken = 1;
    bool m_evaluating = false;
    bool m_loop = false;
};

template <class T>
class Property final : public PropertyBase {
public:
    Property() = default;
    explicit Property(T v) : m_value(std::move(v)) {}
    template <class F, class = std::enable_if_t<std::is_invocable_r_v<T, F&>>>
    explicit Property(F&& fn) { setBinding(std::forward<F>(fn)); }

    const T& value() const { noteRead(); return m_value; }
    const T& operator()() const { return value(); }
    operator const T&() const { return value(); }
    /// Untracked read (does not become a dependency).
    const T& peek() const { return m_value; }

    /// Assign a value (breaks any binding).
    void setValue(T v) {
        clearBinding();
        assign(std::move(v));
    }
    Property& operator=(T v) { setValue(std::move(v)); return *this; }

    /// Live expression; evaluated now and whenever a property it read changes.
    template <class F>
    void setBinding(F&& fn) {
        setEvaluator([this, f = std::function<T()>(std::forward<F>(fn))]() { return store(f()); });
    }

private:
    bool store(T v) {
        if constexpr (requires(const T& a, const T& b) { a == b; })
            if (v == m_value) return false;
        m_value = std::move(v);
        return true;
    }
    void assign(T v) { if (store(std::move(v))) notifyChanged(); }
    T m_value{};
};

/// Runs `fn` now and again whenever a property it read changes (QML `Connections` / a side-effect binding).
/// Destroying the returned effect stops it.
class Effect : public PropertyBase {
public:
    explicit Effect(std::function<void()> fn);
};
using EffectPtr = std::unique_ptr<PropertyBase>;

/// UI bindings: the node property follows the expression for as long as the returned effect lives.
EffectPtr bindText(UiContext& ui, NodeHandle n, std::function<std::string()> expr);
EffectPtr bindValue(UiContext& ui, NodeHandle n, std::function<f32()> expr);
EffectPtr bindCss(UiContext& ui, NodeHandle n, std::string prop, std::function<std::string()> expr);
EffectPtr bindVisible(UiContext& ui, NodeHandle n, std::function<bool()> expr);
/// Two-way: the property follows the node's TextChanged / ValueChanged and the node (its edit buffer for text
/// editors) follows the property.
EffectPtr bindTextTwoWay(UiContext& ui, NodeHandle n, Property<std::string>& p);
EffectPtr bindValueTwoWay(UiContext& ui, NodeHandle n, Property<f32>& p);

// ---------------------------------------------------------------------------------------------------------------
// State machine
// ---------------------------------------------------------------------------------------------------------------

class StateMachine {
public:
    using StateId = int;
    static constexpr StateId kNone = -1;
    enum class Kind : unsigned char { Atomic, Compound, Parallel, Final, ShallowHistory, DeepHistory };

    StateMachine();
    ~StateMachine();
    StateMachine(const StateMachine&) = delete;
    StateMachine& operator=(const StateMachine&) = delete;

    /// The machine itself is state 0 (compound root).
    StateId root() const { return 0; }
    StateId addState(std::string name, StateId parent = 0, Kind kind = Kind::Atomic);
    StateId addFinal(std::string name, StateId parent = 0) { return addState(std::move(name), parent, Kind::Final); }
    StateId addHistory(StateId parent, bool deep = false, StateId defaultTarget = kNone);
    /// Initial child of a compound state (default: its first child).
    void setInitial(StateId compound, StateId child);
    void onEntry(StateId s, std::function<void()> fn);
    void onExit(StateId s, std::function<void()> fn);
    /// While `s` is active the setter receives `value`; on exit it gets the value captured on entry (getter).
    template <class T>
    void assignProperty(StateId s, Property<T>& p, T value) {
        auto saved = std::make_shared<T>();
        onEntry(s, [&p, saved, value] { *saved = p.peek(); p.setValue(value); });
        onExit(s, [&p, saved] { p.setValue(*saved); });
    }
    /// Transition on `event` ("" = eventless, taken whenever the guard holds after a step). Targetless transitions
    /// (target kNone) run the action without leaving the state.
    int addTransition(StateId from, std::string event, StateId target, std::function<bool()> guard = {},
                      std::function<void()> action = {});
    /// Signals finished() when a top-level final state is entered.
    std::function<void()> onFinished;

    void start();
    void stop();
    bool running() const { return m_running; }
    /// Queue an event and process it (run to completion). Events posted from actions are queued.
    void postEvent(std::string event);
    bool isActive(StateId s) const;
    bool isActive(std::string_view name) const;
    std::vector<std::string> configuration() const; ///< active atomic/final state names, document order
    StateId find(std::string_view name) const;
    const std::string& name(StateId s) const;
    /// Re-check eventless transitions (call after changing what guards read, or use watch()).
    void evaluate();
    /// Re-evaluate eventless transitions whenever `p` changes.
    void watch(PropertyBase& p);

private:
    struct Impl;
    std::unique_ptr<Impl> m;
    bool m_running = false;
};

} // namespace fuse::ui::decl
