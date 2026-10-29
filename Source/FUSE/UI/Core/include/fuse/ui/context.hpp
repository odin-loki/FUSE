// FUSE UI (UI2): the headless retained UI core — FUSE_UI_PLAN.md §4.4–4.7.
//
//  - UiContext owns one tree of nodes in pooled storage addressed by generation-checked NodeIds.
//  - Widget behaviour is a registered WidgetClass function table (no virtuals per node, no moc).
//  - Separate dirty bits for style / layout / paint; update() runs only the passes that are dirty and
//    re-records the draw list only when something painted changed (an idle update does nothing).
//  - Style: a QSS-like cascade (type, .class, #name, :hover :pressed :focus :disabled :checked) over themes.
//  - Layout: Column / Row (flex grow, gap, padding, align / justify), Stack, Absolute, Anchor; Scroll clips.
//  - Events: hit test against the last layout; capture (root→target) then bubble (target→root); pointer
//    capture while pressed; hover tracking; Tab / Shift+Tab focus order inside the innermost modal scope;
//    shortcuts; a thread-safe command queue drained at update(); timers and tweens.
//  - Accessibility tree data (role, name, state) via dumpAccessibility(); adapters come later (D7).
//
// Owning-thread rule: only the thread that calls update() mutates the tree; others use post().
#pragma once

#include <fuse/handle.hpp>
#include <fuse/ui/draw_list.hpp>

#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace fuse::ui {

struct NodeTag;
using NodeId = Handle<NodeTag>;
class UiContext;

enum class LayoutKind : u8 { Column, Row, Stack, Absolute };
enum class Align : u8 { Start, Center, End, Stretch };
enum class Justify : u8 { Start, Center, End, SpaceBetween };

/// Anchor edges for children of Stack / Absolute parents (HUD style). Unset edges fall back to `position`.
struct Anchors {
    bool left = false, top = false, right = false, bottom = false, centerX = false, centerY = false;
    Insets margin{};
};

struct LayoutProps {
    LayoutKind kind = LayoutKind::Column;
    f32 gap = 0.f;
    Insets padding{};
    Align align = Align::Stretch;       ///< cross-axis alignment of children
    Justify justify = Justify::Start;   ///< main-axis distribution of children
    f32 grow = 0.f;                     ///< share of the parent's spare main-axis space
    f32 width = -1.f;                   ///< fixed width (< 0 = content)
    f32 height = -1.f;                  ///< fixed height (< 0 = content)
    f32 minWidth = 0.f, minHeight = 0.f;
    Vec2 position{};                    ///< Absolute / Stack offset
    Anchors anchors{};
    bool scroll = false;                ///< clip children and scroll vertically with the wheel
};

enum StateFlag : u32 {
    kStateHover = 1u << 0,
    kStatePressed = 1u << 1,
    kStateFocus = 1u << 2,
    kStateDisabled = 1u << 3,
    kStateChecked = 1u << 4,
};

/// Computed visual style. `StyleRule` sets a subset through `mask`.
struct Style {
    Color background{};
    Color foreground = Color::rgb(0xe6e6e6);
    Color border{};
    Color accent = Color::rgb(0x3d8bfd);
    f32 borderWidth = 0.f;
    f32 radius = 0.f;
    u32 textScale = 2;
    f32 opacity = 1.f;
    friend bool operator==(const Style&, const Style&) = default;
};

enum StyleField : u32 {
    kStyleBackground = 1u << 0,
    kStyleForeground = 1u << 1,
    kStyleBorder = 1u << 2,
    kStyleAccent = 1u << 3,
    kStyleBorderWidth = 1u << 4,
    kStyleRadius = 1u << 5,
    kStyleTextScale = 1u << 6,
    kStyleOpacity = 1u << 7,
};

/// One rule: `Type.class#name:state { ... }` — empty parts match anything; `states` must all be set.
struct StyleRule {
    std::string type;
    std::string cls;
    std::string name;
    u32 states = 0;
    u32 mask = 0;
    Style values{};
};

class StyleSheet {
public:
    /// Parse a QSS-like subset: `Button.primary:hover { background: #3d8bfd; radius: 4; }`. Properties:
    /// background, color, border-color, accent, border-width, radius, text-scale, opacity. Returns false and
    /// fills `error` on a syntax error (rules parsed before the error are kept).
    bool parse(std::string_view source, std::string* error = nullptr);
    void add(StyleRule rule) { m_rules.push_back(std::move(rule)); }
    const std::vector<StyleRule>& rules() const { return m_rules; }
    void clear() { m_rules.clear(); }

private:
    std::vector<StyleRule> m_rules;
};

/// Built-in dark theme (the editor default) as a style sheet source.
std::string_view darkThemeSource();

enum class EventType : u8 { PointerMove, PointerDown, PointerUp, Wheel, KeyDown, Text, FocusIn, FocusOut };

enum class Key : u16 {
    None, Tab, Enter, Escape, Space, Backspace, Delete, Left, Right, Up, Down, Home, End, A, C, V, X, Z, F1,
};

enum Modifier : u8 { kModShift = 1, kModCtrl = 2, kModAlt = 4 };

struct Event {
    EventType type = EventType::PointerMove;
    Vec2 position{};
    f32 wheelDelta = 0.f;
    Key key = Key::None;
    u8 modifiers = 0;
    u32 codepoint = 0;
};

enum class Phase : u8 { Capture, Target, Bubble };

/// Behaviour table for a widget class. Every function is optional.
struct WidgetClass {
    std::string name;
    std::string role = "group";  ///< accessibility role
    bool focusable = false;
    /// Intrinsic content size (excluding padding) for an available width.
    Vec2 (*measure)(const UiContext&, NodeId, f32 availableWidth) = nullptr;
    /// Paint the node's own content (the background / border are painted by the core first).
    void (*paint)(const UiContext&, NodeId, DrawList&) = nullptr;
    /// Default handling at the target / bubble phase; return true to consume.
    bool (*onEvent)(UiContext&, NodeId, const Event&) = nullptr;
};

using ClassId = u32;
inline constexpr ClassId kInvalidClass = ~0u;

struct UpdateStats {
    bool styled = false;
    bool laidOut = false;
    bool painted = false;
    u32 nodesStyled = 0;
    u32 nodesLaidOut = 0;
    Rect damage{};
};

class UiContext {
public:
    using Callback = std::function<void(UiContext&, NodeId)>;
    using EventFilter = std::function<bool(UiContext&, NodeId, const Event&, Phase)>;

    UiContext();
    ~UiContext();
    UiContext(const UiContext&) = delete;
    UiContext& operator=(const UiContext&) = delete;

    // ---- classes -------------------------------------------------------------------------------------------
    ClassId registerClass(WidgetClass cls);
    ClassId findClass(std::string_view name) const;
    const WidgetClass& widgetClass(ClassId id) const { return m_classes[id]; }

    // ---- tree ----------------------------------------------------------------------------------------------
    NodeId root() const { return m_root; }
    NodeId create(ClassId cls, NodeId parent, std::string_view name = {});
    NodeId create(std::string_view className, NodeId parent, std::string_view name = {});
    /// Destroy a node and its subtree. Stale ids stop resolving (generation check).
    void destroy(NodeId id);
    bool alive(NodeId id) const;
    NodeId parent(NodeId id) const;
    NodeId firstChild(NodeId id) const;
    NodeId nextSibling(NodeId id) const;
    u32 childCount(NodeId id) const;
    u32 nodeCount() const { return m_liveCount; }
    /// Depth-first search by name below `from` (root when invalid).
    NodeId find(std::string_view name, NodeId from = {}) const;
    ClassId classOf(NodeId id) const;

    // ---- properties ----------------------------------------------------------------------------------------
    const std::string& name(NodeId id) const;
    void setClasses(NodeId id, std::string_view classes); ///< space-separated style classes
    const std::string& text(NodeId id) const;
    void setText(NodeId id, std::string_view text);
    f32 value(NodeId id) const;
    void setValue(NodeId id, f32 value); ///< clamped to the range; fires onChanged when it changes
    void setRange(NodeId id, f32 min, f32 max);
    Vec2 range(NodeId id) const;
    bool checked(NodeId id) const { return hasState(id, kStateChecked); }
    void setChecked(NodeId id, bool on); ///< fires onChanged when it changes
    void setEnabled(NodeId id, bool enabled);
    bool hasState(NodeId id, u32 flags) const;
    u32 state(NodeId id) const;
    void setVisible(NodeId id, bool visible);
    bool visible(NodeId id) const;
    LayoutProps& layout(NodeId id); ///< mutable; marks layout dirty
    const LayoutProps& layoutProps(NodeId id) const;
    /// Per-node style override applied after the cascade (fields in `mask`).
    void setStyleOverride(NodeId id, const Style& values, u32 mask);
    const Style& style(NodeId id) const; ///< computed style (valid after update)
    Rect rect(NodeId id) const;          ///< computed layout rect in window coordinates (after update)
    f32 scrollOffset(NodeId id) const;
    void setScrollOffset(NodeId id, f32 offset);
    void setModal(NodeId id, bool modal); ///< restricts pointer + focus to this subtree while visible
    void setAccessibleName(NodeId id, std::string_view name);
    u32 cursor(NodeId id) const;          ///< LineEdit caret (byte index)
    void setCursor(NodeId id, u32 cursor);

    // ---- signals -------------------------------------------------------------------------------------------
    void onClicked(NodeId id, Callback cb);
    void onChanged(NodeId id, Callback cb);
    void addEventFilter(NodeId id, EventFilter filter);
    void emitClicked(NodeId id);
    void emitChanged(NodeId id);

    // ---- style ---------------------------------------------------------------------------------------------
    StyleSheet& styleSheet() { return m_sheet; }
    void invalidateStyle(); ///< after editing styleSheet()

    // ---- events / focus / shortcuts ------------------------------------------------------------------------
    /// Dispatch one input event. Returns true if the UI consumed it (game input must not see it).
    bool dispatch(const Event& event);
    NodeId hitTest(Vec2 point) const;
    NodeId focused() const { return m_focus; }
    bool setFocus(NodeId id);
    void focusNext(bool backwards = false);
    NodeId hovered() const { return m_hover; }
    void addShortcut(Key key, u8 modifiers, std::function<void(UiContext&)> action);

    // ---- frame ---------------------------------------------------------------------------------------------
    void setViewport(Vec2 size);
    Vec2 viewport() const { return m_viewport; }
    /// Thread-safe: queue work for the owning thread's next update().
    void post(std::function<void(UiContext&)> command);
    /// Call `cb` after `delaySeconds` of update() time (repeat when `repeat`). Returns a timer id.
    u32 addTimer(f32 delaySeconds, std::function<void(UiContext&)> cb, bool repeat = false);
    void cancelTimer(u32 timerId);
    /// Tween the node's value() to `target` over `seconds` (linear, ease-out).
    void animateValue(NodeId id, f32 target, f32 seconds);
    /// Tween the node's background colour override.
    void animateBackground(NodeId id, Color target, f32 seconds);
    bool animating() const { return !m_tweens.empty(); }
    /// Advance time, drain commands, then run the dirty style / layout / paint passes.
    UpdateStats update(f32 dtSeconds = 0.f);
    const DrawList& drawList() const { return m_draw; }
    bool needsUpdate() const;

    // ---- introspection (tests, inspector) ------------------------------------------------------------------
    std::string dumpTree() const;          ///< "Class#name [x,y wxh] state" per line, indented
    std::string dumpAccessibility() const; ///< "role \"name\" states" per line, indented

    struct Node;

private:
    Node* get(NodeId id);
    const Node* get(NodeId id) const;
    void markStyle(NodeId id);
    void markLayout(NodeId id);
    void markPaint(NodeId id);
    void setStateFlags(NodeId id, u32 flags, bool on);
    void resolveStyles(NodeId id, UpdateStats& stats);
    Vec2 measureNode(NodeId id, f32 availableWidth);
    void arrange(NodeId id, Rect r, UpdateStats& stats);
    void paintNode(NodeId id, DrawList& list, f32 opacity, const Rect& clip) const;
    NodeId hitTestNode(NodeId id, Vec2 p) const;
    bool route(NodeId target, const Event& e);
    NodeId modalScope() const;
    bool inSubtree(NodeId node, NodeId ancestor) const;
    void collectFocusable(NodeId id, std::vector<NodeId>& out) const;
    void dumpNode(NodeId id, int depth, std::string& out, bool a11y) const;

    std::vector<WidgetClass> m_classes;
    std::vector<Node*> m_nodes;
    std::vector<u32> m_free;
    u32 m_liveCount = 0;
    NodeId m_root;
    NodeId m_focus;
    NodeId m_hover;
    NodeId m_capture;
    StyleSheet m_sheet;
    Vec2 m_viewport{800.f, 600.f};
    DrawList m_draw;
    bool m_styleDirty = true;
    bool m_layoutDirty = true;
    bool m_paintDirty = true;
    Rect m_damage{};
    std::vector<Vec2> m_scratchSizes; ///< arrange() child sizes, used as a stack (capacity reused)
    std::vector<NodeId> m_scratchKids;

    struct Shortcut {
        Key key;
        u8 mods;
        std::function<void(UiContext&)> action;
    };
    std::vector<Shortcut> m_shortcuts;

    std::mutex m_postMutex;
    std::vector<std::function<void(UiContext&)>> m_posted;
    std::vector<std::function<void(UiContext&)>> m_draining;

    struct Timer {
        u32 id;
        f32 remaining;
        f32 interval;
        bool repeat;
        std::function<void(UiContext&)> cb;
    };
    std::vector<Timer> m_timers;
    u32 m_nextTimer = 1;

    struct Tween {
        NodeId node;
        bool colour;
        f32 from, to, t, duration;
        Color fromColour, toColour;
    };
    std::vector<Tween> m_tweens;
};

/// Register the built-in widget classes (Panel, Label, Button, CheckBox, Slider, LineEdit, ScrollView,
/// Spacer, ProgressBar) into `ctx` and load the dark theme. UiContext's constructor calls this.
void registerBuiltinWidgets(UiContext& ctx);

} // namespace fuse::ui
