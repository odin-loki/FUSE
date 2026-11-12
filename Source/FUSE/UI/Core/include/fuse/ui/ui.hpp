// FUSE UI core: UiContext, node storage, widget classes, events, focus, layout, paint, a11y. Plan §4.4–§4.7, UI2.
//
// One UiContext owns one tree and is bound to one thread (its owner): every call except post() must come from it.
// Other threads post UiCommands through a bounded lock-free MPSC queue drained at the start of update().
//
// Frame (UiContext::update):  commands → timers → animations → style (dirty nodes) → layout (dirty subtrees, measure
// cache, fixed-size subtrees are boundaries) → paint (draw list rebuilt when anything paint-dirty; damage rect = old ∪
// new bounds of changed nodes) → a11y version bump. A frame with nothing dirty does no work and allocates nothing.
#pragma once

#include "fuse/text/font_family.hpp"
#include "fuse/text/text.hpp"
#include "fuse/ui/draw_list.hpp"
#include "fuse/ui/handle.hpp"
#include "fuse/ui/inline_fn.hpp"
#include "fuse/ui/style.hpp"
#include "fuse/ui/types.hpp"

#include <atomic>
#include <concepts>
#include <expected>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fuse::ui {

class UiContext;

// ---- Input ---------------------------------------------------------------------------------------------------------
/// UI key set (maps 1:1 onto fuse::platform key set v2 names; see platform/events_v2.hpp).
enum class Key : u16 {
    None, Tab, Enter, Escape, Space, Backspace, Delete, Left, Right, Up, Down, Home, End, PageUp, PageDown,
    A, C, V, X, Y, Z, F2,
    W, S, D, Q, E, N, O, F5, F10, ///< appended for the editor shell (fly camera, File / Play shortcuts)
    Num1, Num2, Num3, Num4, ///< editor transform tools (Select / Move / Rotate / Scale)
    B, F, G, H, I, J, K, L, M, P, R, T, U, Num0, Num5, Num6, Num7, Num8, Num9, ///< rebindable editor shortcuts
    F1, F3, F4, F6, F7, F8, F9, F11, F12,
    BracketLeft, BracketRight, ///< timeline previous / next key
    Other
};
enum Mods : u8 { kModShift = 1, kModCtrl = 2, kModAlt = 4, kModSuper = 8 };
enum class MouseButton : u8 { Left, Right, Middle };
enum class NavDir : u8 { Left, Right, Up, Down };
enum class FocusReason : u8 { Pointer, Keyboard, Programmatic };

enum class EventType : u8 {
    PointerMove, PointerDown, PointerUp, PointerEnter, PointerLeave, Click, Wheel, KeyDown, KeyUp, Text, FocusIn,
    FocusOut,
    Composition, ///< IME pre-edit changed: `text` = pre-edit (empty ends it), `caret` = byte offset in it
    DoubleClick, ///< the second click of a double click, after its Click (not consumed: SignalKind::DoubleClicked)
    DragEnter, DragMove, DragLeave, Drop, ///< to the drop target of a drag: `drag`, `dropAction` (proposed), `pos`
    DragEnd,     ///< to the drag source when its drag ends: `dropAction` = result (None: cancelled / not accepted)
    GestureTap, GestureDoubleTap, GestureLongPress, ///< touch gestures, to the node under the finger: `pos`
    GesturePinch,  ///< two fingers: `scale` = distance / distance at the start, `pos` = centre, `gesturePhase`
    GestureRotate, ///< two fingers: `rotation` = radians turned since the start (y down: clockwise > 0), `pos`, `gesturePhase`
};
enum class DropAction : u8 { None, Move, Copy };
enum class GesturePhase : u8 { Begin, Update, End };
struct DragData;
enum class Phase : u8 { Capture, Target, Bubble };

struct Event {
    EventType type = EventType::PointerMove;
    Vec2 pos;              ///< pointer events: context coordinates
    MouseButton button = MouseButton::Left;
    Key key = Key::None;
    u8 mods = 0;
    Vec2 wheel;            ///< wheel notches (y > 0 = up)
    std::string_view text; ///< Text: UTF-8 (valid during dispatch only)
    FocusReason focusReason = FocusReason::Programmatic;
    i32 caret = 0;         ///< Composition: caret byte offset into `text`
    bool cancel = false;   ///< PointerUp: the press was cancelled (a touch became a pan, touchCancel): no Click follows
    u8 clicks = 1;         ///< PointerDown / PointerUp / Click (left button): 2 on the second click of a double click
    const DragData* drag = nullptr;              ///< Drag* / Drop: the dragged data (valid during dispatch only)
    DropAction dropAction = DropAction::None;    ///< DragEnter / DragMove / Drop: proposed action; DragEnd: the result
    GesturePhase gesturePhase = GesturePhase::Update; ///< GesturePinch / GestureRotate
    f32 scale = 1.f;       ///< GesturePinch
    f32 rotation = 0.f;    ///< GestureRotate
};

// ---- Drag and drop -------------------------------------------------------------------------------------------------
/// What a drag carries. `type` is the hash of a MIME-like string; targets accept by type (UiContext::setDropTypes,
/// setDropFilter, WidgetClass::acceptsDrop).
struct DragData {
    u32 type = 0;            ///< dragType("application/x-my-thing")
    u64 payload[2]{};        ///< small inline payload (row index, entity id, packed handle)
    void* pointer = nullptr; ///< optional non-owning pointer (must outlive the drag)
    std::string text;        ///< optional text: label of the default ghost; OS file drops: '\n'-separated paths
    NodeHandle source;       ///< set by beginDrag; invalid for external (OS) drops
};
constexpr u32 dragType(std::string_view mime) { return hashId(mime); }
inline constexpr u32 kDragText = hashId("text/plain");
inline constexpr u32 kDragFiles = hashId("text/uri-list");         ///< OS file drop (UiContext::dropExternal)
inline constexpr u32 kDragRow = hashId("application/x-fuse-row"); ///< an item of a list / table / grid view
/// The ghost that follows the pointer: `draw` if set, else a translucent copy of `node`, else a chip with the text.
struct DragImage {
    NodeHandle node;
    void (*draw)(UiContext&, DrawList&, Vec2 at, const DragData&, void* user) = nullptr; ///< `at` = pointer position
    void* user = nullptr;
    Vec2 hotspot;         ///< `node`: the point of the node (relative to its top-left) that stays under the pointer
    f32 opacity = 0.7f;   ///< `node`
};
/// Asked when a press on a node (or a descendant) is dragged past the threshold: fill `out` (and optionally `image`)
/// and return true to start a drag. `press` is where the button / finger went down.
using DragSourceFn = bool (*)(UiContext&, NodeHandle, Vec2 press, DragData& out, DragImage& image);
/// Per-node drop filter: true when `n` takes `data` at `pos`.
using DropFilterFn = bool (*)(UiContext&, NodeHandle n, const DragData& data, Vec2 pos);

/// Pointer / touch gesture thresholds (UiContext::setGestureConfig). Times are in the context's time domain (the sum of
/// update(dt)) for clicks and long presses, and in the touch event time for taps.
struct GestureConfig {
    f32 doubleClickTime = 0.4f; ///< s between the two presses of a double click
    f32 doubleClickSlop = 5.f;  ///< px the second press may be away from the first
    f32 dragThreshold = 4.f;    ///< px a mouse press must travel before a drag source is asked
    f32 longPressTime = 0.5f;   ///< s a finger must rest (within TouchScrollConfig::slop) for GestureLongPress
    f32 doubleTapTime = 0.3f;   ///< s between two taps of a GestureDoubleTap
    f32 doubleTapSlop = 24.f;   ///< px
    f32 pinchSlop = 6.f;        ///< px the finger distance must change before GesturePinch begins
    f32 rotateSlop = 0.12f;     ///< radians the finger pair must turn before GestureRotate begins
};

/// Keyboard shortcut hook (the ActionRegistry of actions.hpp installs itself here): called for a KeyDown the focused
/// widget did not consume, before Tab / arrow navigation. True = consumed.
struct ShortcutHook {
    bool (*fn)(void* user, UiContext&, Key, u8 mods) = nullptr;
    void* user = nullptr;
};

/// One key of UiContext::animateKeyframes.
struct PropKeyframe {
    f32 at = 0.f; ///< 0..1 of the duration (ascending)
    PropValue value;
};
/// How UiContext::animateProperty moves a property.
struct AnimSpec {
    f32 duration = 0.2f;                 ///< s (tween)
    Easing ease = Easing::InOutCubic;    ///< tween
    f32 delay = 0.f;                     ///< s before it starts
    bool spring = false;                 ///< damped spring instead of a tween: `duration` and `ease` are ignored
    f32 stiffness = 170.f, damping = 26.f, mass = 1.f;
    static AnimSpec tween(f32 seconds, Easing e = Easing::InOutCubic) { AnimSpec s; s.duration = seconds; s.ease = e; return s; }
    static AnimSpec springy(f32 stiffness = 170.f, f32 damping = 26.f, f32 mass = 1.f) {
        AnimSpec s; s.spring = true; s.stiffness = stiffness; s.damping = damping; s.mass = mass; return s;
    }
};

/// Clipboard backend of a context (UI1). Default: a context-local buffer; apps install the platform clipboard
/// (fuse::platform::setClipboardText / clipboardText, see platform_bridge.hpp).
struct ClipboardHooks {
    bool (*set)(void* user, std::string_view utf8) = nullptr;
    bool (*get)(void* user, std::string& out) = nullptr;
    void* user = nullptr;
};

// ---- Signals -------------------------------------------------------------------------------------------------------
enum class SignalKind : u8 {
    Clicked, Toggled, ValueChanged, TextChanged, Submitted, FocusChanged, HoverChanged, Event,
    DoubleClicked, ///< a double click nothing consumed
    RowMoved,      ///< item views with drag reorder: {value = from, value2 = to (index after the move)}
    Dropped,       ///< item views: a drag from another view landed {value = insert index, text = drag text, event = Drop}
    DragEnded,     ///< item views: the drag that started here ended {value = source row, flag = accepted, value2 = DropAction}
};
struct SignalArgs {
    f32 value = 0.f;
    bool flag = false;
    std::string_view text{};
    const Event* event = nullptr;
    f32 value2 = 0.f;
};
using Slot = InlineFn<void(UiContext&, NodeHandle, const SignalArgs&)>;
using CommandFn = void (*)(UiContext&, NodeHandle, u64 a, u64 b);

// ---- Accessibility -------------------------------------------------------------------------------------------------
enum class A11yRole : u8 {
    None, Window, Group, Text, Button, CheckBox, RadioButton, Switch, Slider, ProgressBar, TextField, ScrollArea,
    List, ListItem, Image, Separator, Link, ComboBox, SpinBox, Dial, ScrollBar, Tab,
    // appended (the values above are stable): menus, dialogs, trees, tables, tab lists, bars, transient messages
    Menu, MenuItem, MenuBar, Dialog, Tree, TreeItem, Table, Row, Cell, ColumnHeader, TabList, TabPanel, ToolBar,
    StatusBar, Tooltip, Alert
};
const char* a11yRoleName(A11yRole r);
/// What assistive technology can ask a node to do (UiContext::performA11yAction). The first five keep the order of
/// the fuse_ui_a11y bridge's original Action enum.
enum class A11yAction : u8 {
    Focus, Click, Increment, Decrement,
    SetValue,       ///< ranged widgets: `value`; text fields: `text`
    Expand, Collapse,
    Select,         ///< rows, cells, tabs, menu items: make it the current one
    ScrollUp, ScrollDown, ScrollLeft, ScrollRight, ///< one step of a scroll container or a self-scrolling view
    ScrollIntoView, ///< scroll every scrolling ancestor (and the owning view, for a virtual item) until it shows
};
constexpr u32 a11yActionBit(A11yAction a) { return 1u << u32(a); }
const char* a11yActionName(A11yAction a);
enum class A11yLive : u8 { Off, Polite, Assertive };
/// One accessible node in a flattened snapshot (UiContext::snapshotA11y). Nodes with role None are skipped and
/// their children re-parented, as in dumpA11y. `parent` indexes the snapshot vector (~0u for the top node).
struct A11ySnapshotNode {
    NodeHandle node;
    u32 parent = ~0u;
    A11yRole role = A11yRole::None;
    std::string name; ///< a11y name, else the node text
    Rect bounds;      ///< window coordinates (logical px)
    bool focusable = false, focused = false, checked = false, disabled = false;
    f32 value = 0.f, lo = 0.f, hi = 0.f;
    /// 0: the node itself. Otherwise this entry is a VIRTUAL child of `node` - a visible row / cell / tab / link of a
    /// widget that paints its items itself (list, tree, table, grid, tab bar, header, rich text) - and `child` is its
    /// id inside that widget: stable while the item exists (a row keeps its id when the view scrolls), never 0.
    u32 child = 0;
    std::string valueText; ///< textual value: text field content, combo selection, date, colour ("" = none)
    bool expandable = false, expanded = false; ///< tree rows, combo boxes, menu buttons, foldable groups
    bool selectable = false, selected = false; ///< rows, cells, tabs, menu items
    bool modal = false;                        ///< dialogs
    A11yLive live = A11yLive::Off;             ///< live region: a change of name / valueText is announced
    u32 actions = 0;                           ///< a11yActionBit(A11yAction) of every action performA11yAction accepts
    u32 level = 0;                             ///< tree rows: depth, 1 = top level (0 = not in a hierarchy)
    u32 posInSet = 0, setSize = 0;             ///< virtual items: 1-based position among ALL items, and their count
    bool hasAction(A11yAction a) const { return (actions & a11yActionBit(a)) != 0; }
};

// ---- Touch ---------------------------------------------------------------------------------------------------------
/// What a touch drag that starts on a widget does once the finger leaves the slop (UiContext::touchMove).
struct TouchDrag {
    enum class Mode : u8 {
        Pan,     ///< default: the press is cancelled and the nearest scrollable ancestor pans with the finger
        Capture, ///< the widget keeps the drag (slider, splitter, canvas): pointer moves reach it as with a mouse
        Wheel,   ///< the widget scrolls itself on Wheel events: the drag is delivered as Wheel in `step` increments
    };
    Mode mode = Mode::Pan;
    Vec2 step;    ///< Wheel: finger travel (px) per wheel increment along x / y; 0 = that axis does not scroll now
    Vec2 notches; ///< Wheel: wheel notches sent per increment (e.g. a list that scrolls 3 rows per notch: 1/3)
};
/// Touch pan / fling tuning (UiContext::setTouchScroll).
struct TouchScrollConfig {
    f32 slop = 8.f;           ///< px a finger may move before a press becomes a pan
    f32 decay = 3.5f;         ///< fling velocity decays as exp(-decay * t)
    f32 minVelocity = 12.f;   ///< px/s: below this a fling stops (and a release does not start one)
    f32 maxVelocity = 8000.f; ///< px/s clamp on release
    f32 sampleWindow = 0.1f;  ///< s of finger history used for the release velocity
};

// ---- Widget classes ------------------------------------------------------------------------------------------------
struct PaintContext {
    UiContext& ui;
    NodeHandle node;
    Rect rect;    ///< border box
    Rect content; ///< inside padding
    const ComputedStyle& style;
    u32 states;
    f32 opacity;  ///< accumulated (multiply colours by it)
    Color tint(Color c) const { return c.withAlpha(c.a * opacity); }
};

struct EventContext {
    UiContext& ui;
    NodeHandle node; ///< node whose handler runs
    NodeHandle target;
    Phase phase;
};

/// A registered table of functions (no inheritance, no moc). Null entries use core defaults.
struct WidgetClass {
    std::string name;
    u32 nameHash = 0;
    A11yRole role = A11yRole::Group;
    bool focusable = false;
    bool interactive = false; ///< receives pointer press / click (buttons, fields)
    /// Content size (excluding padding) for `available` content width (may be +inf). Null: text or zero.
    Size (*measure)(UiContext&, NodeHandle, Size available) = nullptr;
    /// Paints after the core background / border; null: paints the node's text.
    void (*paint)(const PaintContext&, DrawList&) = nullptr;
    /// Returns true when consumed (stops propagation).
    bool (*onEvent)(EventContext&, const Event&) = nullptr;
    /// Default style (applied before style sheets).
    ComputedStyle defaults{};
    /// Touch: the class consumes pointer drags itself, so a touch drag on or inside it never becomes a pan.
    bool capturesDrag = false;
    /// Touch: per-node / per-press answer (null: TouchDrag::Mode::Pan). `down` = where the finger went down; called
    /// after the PointerDown was delivered, when the finger leaves the slop.
    TouchDrag (*touchDrag)(UiContext&, NodeHandle, Vec2 down) = nullptr;
    /// Position-dependent mouse cursor (splitter handles, header / column edges): the shape for pointer position `p`
    /// (context coordinates), or Cursor::Auto to fall back to the node's `cursor` style. See UiContext::cursor.
    Cursor (*cursor)(UiContext&, NodeHandle, Vec2 p) = nullptr;
    /// Drag and drop: the class starts drags itself (as UiContext::setDragSource does for one node).
    DragSourceFn dragStart = nullptr;
    /// Drag and drop: true when the node takes `data` at `pos` (as UiContext::setDropFilter does for one node).
    DropFilterFn acceptsDrop = nullptr;
    /// Accessibility: called by snapshotA11y after the core filled `self` (role, name, states, value, default
    /// actions). The class may refine `self` (valueText, expanded, extra actions) and append its VISIBLE items to
    /// `items` as virtual children: each needs `child` (stable id, never 0), role, name and bounds; `parent` is an
    /// index into `items` (an earlier entry) or ~0u for a direct child of the widget. Null: nothing to add.
    void (*a11y)(const UiContext&, NodeHandle, A11ySnapshotNode& self, std::vector<A11ySnapshotNode>& items) = nullptr;
    /// Accessibility: performs `action` on the node (`child` 0) or on one of its virtual children. Returns false to
    /// let the core's default handle it (Focus, Click, Increment / Decrement / SetValue, scrolling).
    bool (*a11yAction)(UiContext&, NodeHandle, u32 child, A11yAction action, f32 value, std::string_view text) = nullptr;
};

/// Concept turning a plain struct with static members into a WidgetClass.
template <class W>
concept WidgetType = requires {
    { W::kName } -> std::convertible_to<std::string_view>;
};

class WidgetRegistry {
public:
    u32 add(WidgetClass cls); ///< returns class id (re-registering a name replaces it)
    template <WidgetType W>
    u32 registerWidget() {
        WidgetClass c;
        c.name = std::string(W::kName);
        if constexpr (requires { W::kRole; }) c.role = W::kRole;
        if constexpr (requires { W::kFocusable; }) c.focusable = W::kFocusable;
        if constexpr (requires { W::kInteractive; }) c.interactive = W::kInteractive;
        if constexpr (requires { &W::measure; }) c.measure = &W::measure;
        if constexpr (requires { &W::paint; }) c.paint = &W::paint;
        if constexpr (requires { &W::onEvent; }) c.onEvent = &W::onEvent;
        if constexpr (requires { W::defaults(); }) c.defaults = W::defaults();
        if constexpr (requires { W::kCapturesDrag; }) c.capturesDrag = W::kCapturesDrag;
        if constexpr (requires { &W::touchDrag; }) c.touchDrag = &W::touchDrag;
        if constexpr (requires { &W::cursor; }) c.cursor = &W::cursor;
        if constexpr (requires { &W::dragStart; }) c.dragStart = &W::dragStart;
        if constexpr (requires { &W::acceptsDrop; }) c.acceptsDrop = &W::acceptsDrop;
        return add(std::move(c));
    }
    u32 find(std::string_view name) const; ///< 0xFFFFFFFF when unknown
    const WidgetClass& get(u32 id) const { return m_classes[id]; }
    /// Mutable access to a registered class (hooks installed after registration, e.g. the accessibility hooks).
    WidgetClass& edit(u32 id) { return m_classes[id]; }
    u32 size() const { return u32(m_classes.size()); }

private:
    std::vector<WidgetClass> m_classes;
};

// ---- Context -------------------------------------------------------------------------------------------------------
struct UiContextDesc {
    Size viewport{800.f, 600.f};
    const fuse::text::Font* font = nullptr; ///< non-owning; null = no text (measure 0)
    u32 reserveNodes = 1024;
    int atlasSize = 1024;
};

enum class UiError : u8 { InvalidHandle, UnknownClass, QueueFull, StyleParse };

struct FrameStats {
    u32 styled = 0, measured = 0, arranged = 0, painted = 0, drawCmds = 0;
    bool repainted = false;
};

class UiContext {
public:
    explicit UiContext(const UiContextDesc& desc = {});
    ~UiContext();
    UiContext(const UiContext&) = delete;
    UiContext& operator=(const UiContext&) = delete;

    WidgetRegistry& registry() { return m_registry; }
    const WidgetRegistry& registry() const { return m_registry; }
    StyleEngine& styles() { return m_styles; }
    std::expected<void, UiError> addStyleSheet(std::string_view css, std::vector<StyleDiagnostic>* diags = nullptr);
    void addStyleSheet(StyleSheet sheet); ///< an already parsed (or cooked) sheet
    void clearStyleSheets();

    // tree
    NodeHandle root() const { return m_root; }
    std::expected<NodeHandle, UiError> create(std::string_view className, NodeHandle parent, std::string_view id = {});
    NodeHandle create(u32 classId, NodeHandle parent, std::string_view id = {});
    void destroy(NodeHandle n);
    bool alive(NodeHandle n) const;
    void moveTo(NodeHandle n, NodeHandle newParent, NodeHandle before = {});
    NodeHandle parent(NodeHandle n) const;
    NodeHandle firstChild(NodeHandle n) const;
    NodeHandle nextSibling(NodeHandle n) const;
    u32 childCount(NodeHandle n) const;
    u32 nodeCount() const { return m_live; }
    NodeHandle find(std::string_view id) const;
    NodeHandle handleOf(u32 index) const;
    std::string_view idOf(NodeHandle n) const;
    u32 classOf(NodeHandle n) const;

    // properties
    void addClass(NodeHandle n, std::string_view cls);
    void removeClass(NodeHandle n, std::string_view cls);
    bool hasClass(NodeHandle n, std::string_view cls) const;
    void set(NodeHandle n, Prop p, const PropValue& v);
    bool setCss(NodeHandle n, std::string_view property, std::string_view value);
    void clearInline(NodeHandle n);
    void setText(NodeHandle n, std::string_view s);
    std::string_view text(NodeHandle n) const;
    void setValue(NodeHandle n, f32 v, bool emit = false);
    f32 value(NodeHandle n) const;
    void setRange(NodeHandle n, f32 lo, f32 hi, f32 step = 0.f);
    f32 rangeMin(NodeHandle n) const;
    f32 rangeMax(NodeHandle n) const;
    f32 rangeStep(NodeHandle n) const;
    void setState(NodeHandle n, u32 stateBits, bool on);
    u32 states(NodeHandle n) const;
    void setChecked(NodeHandle n, bool on) { setState(n, kStateChecked, on); }
    bool checked(NodeHandle n) const { return states(n) & kStateChecked; }
    void setDisabled(NodeHandle n, bool on) { setState(n, kStateDisabled, on); }
    bool disabled(NodeHandle n) const { return states(n) & kStateDisabled; }
    void setFocusable(NodeHandle n, bool on);
    void setTabIndex(NodeHandle n, i32 index);
    void setFocusScope(NodeHandle n, bool on);
    /// Marks one node as consuming pointer drags (as WidgetClass::capturesDrag does for a class): a touch drag that
    /// starts on it or inside it is never turned into a pan. For drags implemented with a SignalKind::Event slot.
    void setCapturesDrag(NodeHandle n, bool on);
    void setA11y(NodeHandle n, A11yRole role, std::string_view name = {});
    A11yRole role(NodeHandle n) const;
    /// Marks a node as a live region: assistive technology announces its name when it appears or changes (status
    /// messages, toasts). Reported in the snapshot; fuse_ui_a11y turns changes into announcements.
    void setA11yLive(NodeHandle n, A11yLive live);
    /// Performs an accessibility action on `node`, or on virtual child `child` of it (A11ySnapshotNode::child).
    /// The widget class is asked first (WidgetClass::a11yAction); the core's defaults are: Focus = keyboard focus;
    /// Click = a pointer click at the centre when the node is what is hit there, else the widget's Click event and
    /// Clicked signal as for Enter; Increment / Decrement = one step (the range step, else 1 % of the range);
    /// SetValue = setValue clamped to the range, or for a text field the text (TextChanged is emitted); Scroll* =
    /// a scroll container moves by half its size, any other node receives one wheel notch; ScrollIntoView scrolls
    /// the scrolling ancestors. Returns false when the node is dead, disabled (everything but ScrollIntoView), or
    /// the action does not apply (see A11ySnapshotNode::actions). This is the entry point platform adapters and
    /// tests use; it must be called on the context's thread.
    bool performA11yAction(NodeHandle node, A11yAction action, f32 value = 0.f, u32 child = 0, std::string_view text = {});
    void setUserData(NodeHandle n, u64 v);
    u64 userData(NodeHandle n) const;
    /// Per-node text edit buffer (created on first use; LineEdit and friends).
    fuse::text::TextEditBuffer& editBuffer(NodeHandle n);
    const fuse::text::TextLayout* textLayout(NodeHandle n) const;

    // style / geometry results (valid after update)
    const ComputedStyle& computed(NodeHandle n) const;
    Rect rect(NodeHandle n) const;
    Rect contentRect(NodeHandle n) const;
    Vec2 scroll(NodeHandle n) const;
    Size scrollExtent(NodeHandle n) const;
    void scrollTo(NodeHandle n, Vec2 offset);
    static constexpr f32 kScrollBarSize = 8.f; ///< overlay scrollbar thickness (drawn and draggable on scroll containers)
    bool scrollDragging() const { return m_scrollDrag.node.valid(); }

    // frame
    void setViewport(Size s);
    Size viewport() const { return m_viewport; }
    void update(f32 dtSeconds = 0.f);
    bool needsUpdate() const; ///< false when an on-demand app can sleep
    /// Seconds until the context needs an update: 0 when dirty / animating / commands pending, the earliest timer
    /// otherwise, +inf when idle (an on-demand loop sleeps this long or until input).
    f32 nextWakeSeconds() const;
    /// Render scale (HiDPI): device pixels per logical unit, default 1. Style, layout, events, hit testing, rect(),
    /// viewport(), textCaret(), damage() and PopupSurface::rect stay in LOGICAL units whatever the scale. Only the
    /// painted output is in DEVICE pixels (logical x scale): drawList() and PopupSurface::draw are recorded by the
    /// widgets in logical units and scaled once at the end of the paint pass (DrawList::scale), and glyphs are shaped
    /// at the logical font size but rasterised at fontSize x scale, so text is as sharp as the display. A host
    /// rasterises drawList() into a target of viewport() x scale pixels and scissors with deviceDamage(). Scale 1
    /// produces exactly the unscaled list. Changing the scale repaints everything.
    void setRenderScale(f32 scale);
    f32 renderScale() const { return m_renderScale; }
    /// The frame's draw list, in device pixels (== logical units at render scale 1).
    const DrawList& drawList() const { return m_draw; }
    /// OS-window popups (Qt: a QMenu / combo list is its own top-level window). When on, overlay popup panels (the
    /// "clamp" nodes popupMenuAt / combo boxes / submenus create) are left out of drawList(); each is painted into its
    /// own list in popup-local coordinates (popupSurfaces()) for the host to show in a borderless OS window at
    /// window origin + rect, so a popup may extend past the window. They then clamp to `screen` (the work area in
    /// context coordinates; empty = the viewport) instead of the viewport. Pointer input for a popup window is fed
    /// back in context coordinates (local + rect origin).
    struct PopupSurface {
        NodeHandle node;
        Rect rect;      ///< context coordinates, logical units (may lie outside the viewport)
        DrawList draw;  ///< device pixels (see setRenderScale); origin = round(rect top-left x renderScale)
        u64 revision = 0; ///< bumps whenever `draw` or `rect` changes
    };
    void setOsPopups(bool on, Rect screen = {});
    bool osPopups() const { return m_osPopups; }
    /// Where popups may be placed: the screen bounds with OS popups, the viewport otherwise.
    Rect popupArea() const;
    const std::vector<PopupSurface>& popupSurfaces() const { return m_popupSurfaces; }
    /// Union of the rects that changed in the last update(), in logical units.
    Rect damage() const { return m_damage; }
    /// damage() in device pixels of drawList(): scaled by renderScale(), rounded outwards (plus 1 px at scales != 1,
    /// where edges and glyphs are re-rounded). Equals damage() at scale 1.
    Rect deviceDamage() const;
    const FrameStats& stats() const { return m_stats; }
    u64 frameIndex() const { return m_frame; }
    void markAllDirty();
    /// Repaints `n` next update (widgets whose paint reads external state, e.g. item models).
    void invalidate(NodeHandle n) { if (node(n)) markPaint(n.index); }
    /// Re-measures and re-lays out `n` next update (widgets whose content size depends on state the core does not
    /// see, e.g. a button's icon).
    void invalidateLayout(NodeHandle n) { if (node(n)) markLayout(n.index); }
    /// Process-unique id of this context, never reused (side tables key on it rather than on the address).
    u64 uid() const { return m_uid; }

    // input (returns true when the UI consumed it — game input arbitration)
    bool pointerMove(Vec2 p);
    bool pointerDown(Vec2 p, MouseButton b = MouseButton::Left);
    bool pointerUp(Vec2 p, MouseButton b = MouseButton::Left);
    bool wheel(Vec2 p, Vec2 notches);
    /// Touch input (one finger; platform layers feed the primary touch here instead of pointerDown / Move / Up).
    /// A touch presses the widget under it like the left mouse button, so a tap clicks. Once the finger moves past
    /// TouchScrollConfig::slop the gesture becomes a pan, unless a widget on the way up captures drags
    /// (WidgetClass::capturesDrag / touchDrag, setCapturesDrag): the press is cancelled (PointerUp with `cancel`, no
    /// Click) and the nearest ancestor that can scroll along the dominant drag axis follows the finger 1:1 - a scroll
    /// container (`overflow: scroll`, both of its axes), or a widget that scrolls on Wheel events (TouchDrag::Wheel).
    /// The target is latched for the gesture. Releasing a moving pan starts a fling that update(dt) advances and
    /// decays; it stops at the scroll extents, when slow, or on the next touchDown (which then does not press).
    /// `timeSeconds` is the event time on any monotonic clock (velocity estimate); negative = the context's own
    /// clock (the sum of update(dt)), which only resolves one sample per frame.
    bool touchDown(Vec2 p, double timeSeconds = -1.0);
    bool touchMove(Vec2 p, double timeSeconds = -1.0);
    bool touchUp(Vec2 p, double timeSeconds = -1.0);
    /// The gesture was taken away (system gesture, focus loss): releases the press without a Click and ends a pan
    /// without a fling.
    bool touchCancel();
    /// Multi-touch: the same calls with a pointer id per finger. The first finger down is the primary one and behaves
    /// exactly like the id-less calls (press, tap, pan, fling). A second finger ends the primary press / pan (no Click,
    /// no fling) and starts a two-finger gesture on the node under the midpoint: GesturePinch and GestureRotate events
    /// (each begins once past its slop) until one of the two lifts; the remaining finger then does nothing until all
    /// are up. Fingers beyond two are tracked but ignored.
    bool touchDown(u32 pointerId, Vec2 p, double timeSeconds = -1.0);
    bool touchMove(u32 pointerId, Vec2 p, double timeSeconds = -1.0);
    bool touchUp(u32 pointerId, Vec2 p, double timeSeconds = -1.0);
    u32 touchCount() const { return m_gesture.count; }
    bool gestureActive() const { return m_gesture.two; }
    void setGestureConfig(const GestureConfig& c) { m_gestureCfg = c; }
    const GestureConfig& gestureConfig() const { return m_gestureCfg; }

    // drag and drop (plan §4.7). A drag starts from beginDrag, or by itself when a press on a drag source
    // (setDragSource / WidgetClass::dragStart) moves past GestureConfig::dragThreshold; with touch a drag source
    // inside something that pans starts on a long press instead (a plain swipe still pans), elsewhere when the finger
    // leaves the slop. While dragging, pointer / touch moves go to the drag: the target is the nearest node under the
    // pointer that accepts the data's type (inside the modal scope); it gets DragEnter / DragMove / DragLeave / Drop,
    // has kStateDragOver (`:drag-over`) set, and may call setDropAction / setDropIndicator from its handlers. Releasing
    // drops (action Move, Copy with Ctrl; None when there is no target or the target refused), Escape / cancelDrag /
    // touchCancel cancel. The source then gets DragEnd with the result. The ghost and the indicator are painted over
    // everything in drawList().
    bool beginDrag(NodeHandle source, DragData data, const DragImage& image = {});
    void cancelDrag();
    bool dragging() const { return m_drag.active; }
    const DragData* dragData() const { return m_drag.active ? &m_drag.data : nullptr; }
    NodeHandle dragTarget() const { return m_drag.active ? m_drag.target : NodeHandle{}; }
    DropAction dropAction() const { return m_drag.action; }
    /// From a DragEnter / DragMove / Drop handler: overrides the proposed action (None refuses the drop).
    void setDropAction(DropAction a) { m_drag.action = a; }
    /// From a DragEnter / DragMove handler: where the data would land (an insertion line, a cell); painted in the
    /// overlay until the target changes or the drag ends. An empty rect clears it.
    void setDropIndicator(Rect r);
    /// Result of the last finished drag / external drop.
    DropAction lastDropAction() const { return m_lastDrop; }
    NodeHandle lastDropTarget() const { return m_lastDropTarget; } ///< invalid when it was not accepted
    void setDragSource(NodeHandle n, DragSourceFn fn);             ///< null removes it
    void setDropTypes(NodeHandle n, std::initializer_list<u32> types); ///< up to 4 accepted DragData::type; empty clears
    void setDropFilter(NodeHandle n, DropFilterFn fn);             ///< asked instead of / in addition to the type list
    /// Drops from outside the UI (OS file drops; a platform layer calls these, nothing in the core does). dropExternal
    /// delivers DragEnter (unless a dragExternalMove already entered) + Drop at `pos` and returns the accepted action;
    /// dragExternalMove gives hover feedback while the OS drag is over the window, dragExternalLeave ends it.
    DropAction dropExternal(Vec2 pos, const DragData& data);
    bool dragExternalMove(Vec2 pos, const DragData& data);
    void dragExternalLeave();
    /// Delivers `e` to `n` through the normal capture / target / bubble route (synthetic events, tests).
    bool sendEvent(NodeHandle n, const Event& e);
    void setShortcutHook(const ShortcutHook& h) { m_shortcutHook = h; }
    const ShortcutHook& shortcutHook() const { return m_shortcutHook; }
    void setTouchScroll(const TouchScrollConfig& c) { m_touchCfg = c; }
    const TouchScrollConfig& touchScroll() const { return m_touchCfg; }
    bool touchPanning() const { return m_touch.stage == TouchState::Stage::Pan; }
    bool flinging() const { return m_touch.fling; }
    Vec2 flingVelocity() const { return m_touch.fling ? m_touch.vel : Vec2{}; } ///< finger space, px/s
    NodeHandle touchPanTarget() const { return (touchPanning() || m_touch.fling) ? m_touch.target : NodeHandle{}; }
    void stopFling() { m_touch.fling = false; }
    /// Modifier state (kMod*) stamped on pointer events; platform layers update it on every modifier change.
    void setModifiers(u8 mods) { m_mods = mods; }
    u8 modifiers() const { return m_mods; }
    bool keyDown(Key k, u8 mods = 0);
    bool keyUp(Key k, u8 mods = 0);
    bool textInput(std::string_view utf8);
    /// IME pre-edit for the focused node (empty `utf8` ends it); `caret` is a byte offset into it. The pre-edit is
    /// dropped when focus moves.
    bool textComposition(std::string_view utf8, i32 caret);
    std::string_view composition() const { return m_composition; }
    i32 compositionCaret() const { return m_compositionCaret; }
    /// Text-input session: true while the focused node edits text (a widget reported a caret this frame via
    /// setTextCaret). The platform glue opens the IME / virtual keyboard and places the candidate window at
    /// textCaret().
    bool wantsTextInput() const { return node(m_focus) && m_caretNode == m_focus; }
    Rect textCaret() const { return m_caret; }
    void setTextCaret(NodeHandle n, Rect caret) { m_caretNode = n; m_caret = caret; }
    // clipboard
    void setClipboard(const ClipboardHooks& hooks) { m_clipboard = hooks; }
    bool setClipboardText(std::string_view utf8);
    bool clipboardText(std::string& out) const;
    NodeHandle hitTest(Vec2 p) const;
    /// The mouse cursor the UI wants now (never Auto). The pressed node (a drag keeps its cursor) or else the hovered
    /// node decides: walking up from it, the first WidgetClass::cursor hook or `cursor` style that is not Auto wins;
    /// Arrow when none. A scroll-bar drag shows Arrow. setCursorOverride (not Auto) beats everything (busy / tool
    /// cursors). Platform layers apply it after feeding pointer input; it does no allocation.
    Cursor cursor();
    Cursor cursorAt(NodeHandle n, Vec2 p);
    void setCursorOverride(Cursor c) { m_cursorOverride = c; }
    Cursor cursorOverride() const { return m_cursorOverride; }
    NodeHandle hovered() const { return m_hover; }
    Vec2 pointer() const { return m_pointer; } ///< last pointer position
    NodeHandle pressed() const { return m_pressed; }
    NodeHandle focused() const { return m_focus; }
    bool focusVisible() const { return m_focusVisible; }
    void setFocus(NodeHandle n, FocusReason why = FocusReason::Programmatic);
    bool focusNext(bool backward = false);
    bool navigate(NavDir d);
    void setModal(NodeHandle scope); ///< restricts focus and pointer input to `scope` (invalid handle clears)
    NodeHandle modal() const { return m_modal; }
    void setSpatialNavigation(bool on) { m_spatialNav = on; }
    /// Tab order of the current focus scope (tests, inspector).
    void tabOrder(std::vector<NodeHandle>& out) const;

    // signals
    ConnectionHandle connect(NodeHandle n, SignalKind kind, Slot slot);
    void disconnect(ConnectionHandle c);
    void emit(NodeHandle n, SignalKind kind, const SignalArgs& args);

    // timers and animation (seconds in the context's time domain)
    TimerHandle startTimer(f32 seconds, bool repeat, InlineFn<void(UiContext&)> fn);
    void cancelTimer(TimerHandle t);
    enum class AnimProp : u8 { Opacity, OffsetX, OffsetY, Value };
    using Ease = Easing; ///< Linear, InOutCubic, OutBack, In, Out
    void animate(NodeHandle n, AnimProp p, f32 to, f32 seconds, Ease e = Ease::InOutCubic);
    bool animating() const { return m_animCount > 0 || m_styleAnimCount > 0; }
    /// Style motion (plan §4.6 / §4.10). CSS `transition` fades animatable properties when a node's style changes
    /// (state, class, inline value); `enter-animation` / `exit-animation` play when a node first shows / is hidden.
    /// animateProperty moves one animatable style property from its current value to `to` with a tween or a spring and
    /// leaves `to` as the node's inline value; animateKeyframes runs a property through up to 6 keys (the last key's
    /// value stays, `loop` repeats until stopPropertyAnimations). While any of these run the node's computed() style
    /// is the interpolated one; an idle context does no work (needsUpdate() false).
    void animateProperty(NodeHandle n, Prop p, const PropValue& to, const AnimSpec& spec = {});
    void animateKeyframes(NodeHandle n, Prop p, const PropKeyframe* keys, u32 count, f32 seconds, Easing e = Easing::Linear, bool loop = false);
    void stopPropertyAnimations(NodeHandle n); ///< jumps running property animations of `n` to their end values
    bool styleAnimating(NodeHandle n) const;   ///< a transition, property animation or exit animation runs on `n`
    /// Reduced motion (the OS accessibility setting; platform layers set it): transitions, enter / exit animations,
    /// property animations and animate() jump straight to their end state.
    void setReducedMotion(bool on);
    bool reducedMotion() const { return m_reducedMotion; }
    /// Seconds a hidden `n` stays on screen for its exit animation (0: none / reduced motion). Popups delay their
    /// destruction by this.
    f32 exitDuration(NodeHandle n) const;

    // cross-thread commands (any thread)
    bool post(CommandFn fn, NodeHandle n, u64 a = 0, u64 b = 0);

    // dumps (tests, inspector)
    void dumpLayout(std::string& out) const;
    /// One line per accessible node, indented by depth: role "name" states [value="text"] [value=v range=[lo,hi]]
    /// [level=n] [pos=i/n]; virtual children are listed under their widget with their id (#child). `actions` adds
    /// the action list of each node.
    void dumpA11y(std::string& out, bool actions = false) const;
    /// Flattened accessible tree of the visible nodes, root first, parents before children (for platform
    /// accessibility adapters: fuse_ui_a11y / AccessKit). Clears `out` first.
    void snapshotA11y(std::vector<A11ySnapshotNode>& out) const;

    // text
    const fuse::text::Font* font() const { return m_font; }
    fuse::text::GlyphAtlas& atlas() { return m_atlas; }
    /// Emits glyph commands for a laid-out block at `origin` (top-left). `origin` and `pixelSize` are logical; during
    /// the paint pass (widget paint callbacks) the glyphs are rasterised at pixelSize x renderScale(). Outside the paint
    /// pass (a caller's own list) the commands are plain logical-size glyphs.
    void paintTextLayout(DrawList& dl, const fuse::text::TextLayout& tl, Vec2 origin, Color c, f32 pixelSize, u32 node);
    /// Lays out and paints a one-off string (widgets' secondary labels). Uses an internal scratch layout.
    void paintString(DrawList& dl, std::string_view s, Rect box, Color c, f32 pixelSize, TextAlignment align, u32 node);
    f32 measureString(std::string_view s, f32 pixelSize);

    // fonts: family / weight / style (CSS font-family, font-weight, font-style; Qt: QFont)
    /// The font a style asks for: the first listed family that is registered (addFontFamily) or installed, else the
    /// context's font; in that family the real face for the weight and slant when there is one, otherwise a
    /// synthetic bold / oblique of the nearest face. Regular text in the default family is the context's font itself.
    /// Allocates only the first time a (family, weight, slant) is asked for. Null when the context has no font.
    const fuse::text::Font* fontFor(const ComputedStyle& s);
    const fuse::text::Font* fontFor(std::span<const u32> familyHashes, int weight, bool italic);
    /// By family name ("" = the context's font).
    const fuse::text::Font* fontFor(std::string_view family, int weight, bool italic);
    /// The font of a node's computed style (the context's font for a dead handle).
    const fuse::text::Font* nodeFont(NodeHandle n);
    /// Registers an application family under its name() for `font-family` (non-owning: it must outlive the context).
    /// Registered families win over installed fonts of the same name.
    void addFontFamily(fuse::text::FontFamily& family);
    /// Whether unregistered `font-family` names are looked up among the installed fonts (fuse::text::systemFonts();
    /// the first lookup scans the font directories). Default on.
    void setSystemFontLookup(bool on) { m_systemFonts = on; }
    /// paintTextLayout with the decoration given instead of read from `node`'s style (kTextUnderline |
    /// kTextLineThrough): one rule per line, across the line's width. The glyphs are drawn with the font the
    /// layout was made with (TextLayout::font), so a block laid out with fontFor(...) paints in that face.
    void paintTextLayout(DrawList& dl, const fuse::text::TextLayout& tl, Vec2 origin, Color c, f32 pixelSize, u32 node,
                         u8 textDecoration);
    /// paintString with an explicit font and decoration (rich text runs): the box, elision and alignment rules of
    /// paintString; the rules span the glyphs' advance width.
    void paintString(DrawList& dl, const fuse::text::Font& font, std::string_view s, Rect box, Color c, f32 pixelSize,
                     TextAlignment align, u32 node, u8 textDecoration = 0);
    /// measureString with an explicit font (rich text runs).
    f32 measureString(const fuse::text::Font& font, std::string_view s, f32 pixelSize);

    struct Node;
    struct Cold;

private:
    friend struct ContextSubject;
    Node* node(NodeHandle h);
    const Node* node(NodeHandle h) const;
    u32 alloc();
    void unlink(u32 i);
    void linkBefore(u32 i, u32 parent, u32 before);
    void destroyRec(u32 i);
    void markLayout(u32 i);
    void markStyle(u32 i);
    void markPaint(u32 i);
    void stylePass();
    void styleRec(u32 i, bool parentChanged);
    Size measure(u32 i, Size avail);
    void arrange(u32 i, Rect box);
    void arrangeChildren(u32 i);
    void paintPass();
    void paintLists(std::vector<PopupSurface>& next);
    void paintRec(u32 i, f32 opacity, DrawList& dl);
    /// Overlay scrollbar geometry of a scroll container along an axis (0 = x, 1 = y); false when it does not overflow.
    bool scrollBarGeom(u32 i, u8 axis, Rect& track, Rect& thumb) const;
    NodeHandle hitRec(u32 i, Vec2 p) const;
    bool dispatch(u32 target, const Event& e);
    void setHover(u32 i);
    u32 interactiveAncestor(u32 i) const;
    u32 scopeOf(u32 i) const;
    bool inScope(u32 i, u32 scope) const;
    bool focusableNode(u32 i) const;
    void drainCommands();
    void ensureText(u32 i, f32 width);
    void collectTab(std::vector<u32>& out) const;
    void markArrange(u32 i);
    bool visibleChain(u32 i) const;
    void cancelPress(Vec2 p);
    bool touchBeginPan(Vec2 p);
    void touchPanBy(Vec2 d);
    void touchSample(Vec2 p, double t);
    // interaction.cpp
    struct DndNode;
    DndNode* dndEntry(NodeHandle n, bool create);
    NodeHandle dropTargetAt(Vec2 p, const DragData& data);
    bool dragMoveTo(Vec2 p);
    bool dragFinish(Vec2 p, bool drop);
    void dragSetTarget(NodeHandle t, Vec2 p);
    bool maybeStartDrag(Vec2 p);
    bool startDragFrom(NodeHandle from, Vec2 press);
    void paintDragOverlay(DrawList& dl);
    void gestureArm(Vec2 p, double t);
    void gestureTap(Vec2 p, double t);
    void gestureTick(f32 dt);
    void gestureEndTwo();
    void gestureReset();
    void noteClick(u32 target, NodeHandle hit, Vec2 p);
    // motion (ui.cpp)
    struct StyleAnim;
    StyleAnim* styleAnimOf(NodeHandle n);
    StyleAnim& ensureStyleAnim(u32 i);
    void composeAnimated(StyleAnim& a);
    void releaseStyleAnim(StyleAnim& a);
    void tickStyleAnims(f32 dt);
    void applyStyleDiff(u32 i, const ComputedStyle& before, const ComputedStyle& after);
    void startEnter(u32 i, MotionKind kind, f32 seconds);

    WidgetRegistry m_registry;
    StyleEngine m_styles;
    const fuse::text::Font* m_font = nullptr;
    fuse::text::GlyphAtlas m_atlas;
    std::vector<Node> m_nodes;
    std::vector<std::unique_ptr<Cold>> m_cold;
    std::vector<u32> m_free;
    u32 m_live = 0;
    NodeHandle m_root;
    Size m_viewport;
    u32 m_layerClass = 0xFFFFFFFFu;
    DrawList m_draw;
    u64 m_uid = [] {
        static std::atomic<u64> next{1};
        return next.fetch_add(1, std::memory_order_relaxed);
    }();
    Rect m_damage;
    f32 m_renderScale = 1.f;
    bool m_painting = false; ///< inside paintPass: glyphs are emitted in device pixels when m_renderScale != 1
    bool m_osPopups = false;
    Rect m_popupScreen;
    std::vector<u32> m_popupPending;
    std::vector<PopupSurface> m_popupSurfaces;
    u64 m_popupRevision = 0;
    FrameStats m_stats;
    u64 m_frame = 0;
    bool m_anyStyleDirty = true, m_anyLayoutDirty = true, m_anyPaintDirty = true;
    u32 m_seenSheetVersion = 0;
    NodeHandle m_hover, m_pressed, m_focus, m_modal;
    bool m_focusVisible = false;
    bool m_spatialNav = false;
    std::string m_composition;
    i32 m_compositionCaret = 0;
    NodeHandle m_caretNode;
    Rect m_caret;
    ClipboardHooks m_clipboard;
    std::string m_localClipboard;
    Vec2 m_pointer{-1.f, -1.f};
    u8 m_mods = 0;
    Cursor m_cursorOverride = Cursor::Auto;
    struct ScrollDrag { NodeHandle node; u8 axis = 0; f32 grab = 0.f; } m_scrollDrag;
    struct TouchState {
        enum class Stage : u8 { Idle, Pending, Passive, Pan }; ///< Passive: down, will never pan (captured drag)
        Stage stage = Stage::Idle;
        bool wheel = false;   ///< the target scrolls on Wheel events (else: a scroll container)
        bool fling = false;
        bool pressed = false; ///< the down was delivered as a press (false when it stopped a fling)
        NodeHandle hit, target;
        Vec2 down, last, acc, vel;
        TouchDrag drag;
        struct Sample { double t = 0.0; Vec2 p; };
        static constexpr u32 kSamples = 32;
        Sample samples[kSamples];
        u32 sampleCount = 0, sampleHead = 0; ///< ring; head = next write
    } m_touch;
    TouchScrollConfig m_touchCfg;
    double m_clock = 0.0; ///< sum of update(dt)
    std::vector<u32> m_path;
    std::vector<u32> m_scratchOrder;
    std::vector<std::pair<u32, i32>> m_scratchTab;
    std::vector<fuse::text::ShapedGlyph> m_shapeScratch;
    fuse::text::TextLayout m_stringScratch;
    // font families by familyHash: [0] is the context's font (bold / italic variants are synthesised from it)
    struct FamilySlot {
        u32 hash = 0;
        fuse::text::FontFamily* family = nullptr; ///< null: looked up and not found
        std::unique_ptr<fuse::text::FontFamily> owned;
    };
    fuse::text::FontFamily* familyByHash(u32 hash);
    /// Scope of a widget callback (measure / paint / event): measureString(s, px) and paintString use the node's font.
    struct TextScope {
        const fuse::text::Font* font = nullptr;
        f32 letterSpacing = 0.f;
    };
    TextScope enterTextScope(u32 node);
    std::vector<FamilySlot> m_families;
    TextScope m_textScope;
    bool m_systemFonts = true;

    struct Connection {
        u32 node = 0xFFFFFFFFu, nodeGen = 0, gen = 0;
        SignalKind kind = SignalKind::Clicked;
        Slot slot;
    };
    std::vector<Connection> m_connections;
    std::vector<u32> m_freeConnections;

    struct Timer {
        f32 interval = 0.f, left = 0.f;
        bool repeat = false, active = false;
        u32 gen = 0;
        InlineFn<void(UiContext&)> fn;
    };
    std::vector<Timer> m_timers;

    struct Anim {
        NodeHandle node;
        AnimProp prop = AnimProp::Opacity;
        Ease ease = Ease::Linear;
        f32 from = 0.f, to = 0.f, t = 0.f, duration = 0.f;
        bool active = false;
    };
    std::vector<Anim> m_anims;
    u32 m_animCount = 0;

    // drag and drop
    struct DndNode {
        NodeHandle node;
        DragSourceFn source = nullptr;
        DropFilterFn filter = nullptr;
        u32 types[4]{};
        u8 typeCount = 0;
    };
    std::vector<DndNode> m_dnd;
    struct DragState {
        bool active = false, external = false, hasIndicator = false;
        DragData data;
        DragImage image;
        NodeHandle target;
        Vec2 pos;
        DropAction action = DropAction::None;
        Rect indicator;
    } m_drag;
    DropAction m_lastDrop = DropAction::None;
    NodeHandle m_lastDropTarget;
    bool m_overlayDirty = false; ///< the drag overlay changed: the next paint damages the whole viewport
    DrawList m_dragList;
    Vec2 m_pressPos;
    NodeHandle m_pressHit;
    bool m_dragAsked = false;
    // click counting
    NodeHandle m_clickNode;
    double m_clickTime = -1e9;
    Vec2 m_clickPos;
    u8 m_clickCount = 0;
    // gestures / multi-touch
    GestureConfig m_gestureCfg;
    struct GestureState {
        static constexpr u32 kMaxTouches = 5;
        struct Point { u32 id = 0; Vec2 pos; };
        Point pts[kMaxTouches];
        u32 count = 0, primary = 0;
        bool primaryDown = false;
        bool two = false;   ///< a two-finger gesture runs (pts[0], pts[1])
        bool spent = false; ///< the sequence had a two-finger gesture: remaining fingers do nothing
        NodeHandle target;
        f32 d0 = 1.f, lastAngle = 0.f, turned = 0.f, scale = 1.f;
        Vec2 centre;
        bool pinching = false, rotating = false;
        bool longArmed = false, moved = false, dragOk = false;
        f32 held = 0.f;
        double downTime = 0.0, lastTapTime = -1e9;
        Vec2 lastTapPos;
    } m_gesture;
    ShortcutHook m_shortcutHook;
    // style motion
    struct StyleAnim {
        NodeHandle node;
        u32 slot = 0;        ///< scratch style index the node points at while animated
        ComputedStyle from;  ///< transition start values
        f32 t = 0.f;         ///< s since the transition (re)started
        bool active = false, transitioning = false, exiting = false;
        MotionKind exitKind = MotionKind::None;
        f32 exitT = 0.f, exitDur = 0.f, exitFromOpacity = 1.f;
    };
    std::vector<StyleAnim> m_styleAnims;
    static constexpr u32 kMaxKeys = 6;
    struct PropAnim {
        NodeHandle node;
        Prop prop = Prop::Opacity;
        PropKeyframe keys[kMaxKeys];
        u8 keyCount = 0;
        AnimSpec spec;
        f32 t = 0.f, x = 0.f, v = 0.f; ///< time; spring position (0..1 towards the last key) and velocity
        bool active = false, loop = false, persist = true;
    };
    std::vector<PropAnim> m_propAnims;
    u32 m_styleAnimCount = 0, m_propAnimCount = 0;
    bool m_reducedMotion = false, m_sheetSwap = false;

    struct Command {
        std::atomic<u64> seq{0};
        CommandFn fn = nullptr;
        NodeHandle node;
        u64 a = 0, b = 0;
    };
    static constexpr u32 kQueueSize = 256;
    std::unique_ptr<Command[]> m_queue;
    std::atomic<u64> m_head{0};
    u64 m_tail = 0;
};

/// Registers the core classes "panel" (generic box), "label" (text) and "root".
void registerCoreClasses(WidgetRegistry& r);

} // namespace fuse::ui
