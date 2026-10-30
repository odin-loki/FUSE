// FUSE UI core: UiContext, node storage, widget classes, events, focus, layout, paint, a11y. Plan §4.4–§4.7, UI2.
//
// One UiContext owns one tree and is bound to one thread (its owner): every call except post() must come from it.
// Other threads post UiCommands through a bounded lock-free MPSC queue drained at the start of update().
//
// Frame (UiContext::update):  commands → timers → animations → style (dirty nodes) → layout (dirty subtrees, measure
// cache, fixed-size subtrees are boundaries) → paint (draw list rebuilt when anything paint-dirty; damage rect = old ∪
// new bounds of changed nodes) → a11y version bump. A frame with nothing dirty does no work and allocates nothing.
#pragma once

#include "fuse/text/text.hpp"
#include "fuse/ui/draw_list.hpp"
#include "fuse/ui/handle.hpp"
#include "fuse/ui/inline_fn.hpp"
#include "fuse/ui/style.hpp"
#include "fuse/ui/types.hpp"

#include <atomic>
#include <concepts>
#include <expected>
#include <functional>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <optional>
#include <vector>

namespace fuse::ui {

/// Internal (preserve-3d paint order): a (piece of a) plane; pts = projected convex polygon (screen x, y; depth z),
/// whole = unsplit.
struct Frag3d { struct V3 { f32 x, y, z; }; u32 plane; std::vector<V3> pts; bool whole; };
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
};
enum class Phase : u8 { Capture, Target, Bubble };

struct Event {
    EventType type = EventType::PointerMove;
    Vec2 pos;              ///< pointer events: the receiving node's own (untransformed) space; NaN when its
                           ///< transform maps the point behind the viewer
    Vec2 client{std::numeric_limits<f32>::quiet_NaN(), 0.f}; ///< pointer events: context coordinates (set by dispatch)
    MouseButton button = MouseButton::Left;
    Key key = Key::None;
    u8 mods = 0;
    Vec2 wheel;            ///< wheel notches (y > 0 = up)
    std::string_view text; ///< Text: UTF-8 (valid during dispatch only)
    FocusReason focusReason = FocusReason::Programmatic;
    i32 caret = 0;         ///< Composition: caret byte offset into `text`
};

/// Clipboard backend of a context (UI1). Default: a context-local buffer; apps install the platform clipboard
/// (fuse::platform::setClipboardText / clipboardText, see platform_bridge.hpp).
struct ClipboardHooks {
    bool (*set)(void* user, std::string_view utf8) = nullptr;
    bool (*get)(void* user, std::string& out) = nullptr;
    void* user = nullptr;
};

// ---- Signals -------------------------------------------------------------------------------------------------------
enum class SignalKind : u8 { Clicked, Toggled, ValueChanged, TextChanged, Submitted, FocusChanged, HoverChanged, Event,
                              EditingFinished, ///< line edit: Enter or focus loss with Acceptable text (QLineEdit::editingFinished)
                              Highlighted,     ///< completer: a completion was highlighted {value = row, text} (QCompleter::highlighted)
                              Activated,       ///< completer: a completion was chosen {value = row, text} (QCompleter::activated)
                              // CSS motion events (text: property or animation name; value: elapsedTime in seconds)
                              TransitionRun, TransitionStart, TransitionEnd, TransitionCancel,
                              AnimationStart, AnimationIteration, AnimationEnd, AnimationCancel,
                              // scripted animations (UiContext::animate): text = the animation id, value = currentTime
                              ScriptFinish, ScriptCancel,
                              ScriptRemove, ///< a finished, filling animation fully overridden by later ones was removed
                              SelectionChanged ///< item views: the selection set changed (QItemSelectionModel::selectionChanged)
};
/// Pointer cursor the UI asks the window for (backends map it to the OS cursor; drag and drop feedback).
enum class CursorShape : u8 { Default, Pointer, Text, Copy, Move, Link, Forbidden, Grabbing };
// ---- Scripted animations (Web Animations-style) -------------------------------------------------------------------
/// One keyframe of UiContext::animate (Web Animations "Keyframe").
struct AnimKeyframe {
    f32 offset = -1.f;          ///< 0..1; < 0: computed (first 0, last 1, others evenly spaced between neighbours)
    bool hasEase = false;       ///< easing from this keyframe to the next (default linear)
    Easing ease;
    bool hasComposite = false;  ///< overrides AnimationOptions::composite for this keyframe's values
    AnimComposite composite = AnimComposite::Replace;
    std::vector<std::pair<Prop, PropValue>> values;
};
/// The timeline an animation follows (Web Animations 1 / Scroll-driven Animations 1). Document: wall time from
/// UiContext::update. Scroll: progress of a scroll container's offset along an axis (0 at the start, 1 at the end of
/// its range). View: progress of a subject crossing its scroll container's scrollport (`cover` range: 0 when its
/// start edge enters at the end of the scrollport, 1 when its end edge leaves at the start), shrunk by the insets.
/// A progress-based timeline maps its 0..1 progress onto the animation's whole end time (a zero duration becomes 1 s,
/// the CSS `auto` duration); it never finishes and is inactive (no effect) while the container cannot scroll.
struct AnimTimeline {
    enum class Kind : u8 { Document, Scroll, View };
    Kind kind = Kind::Document;
    NodeHandle source; ///< Scroll: the container (none: nearest scrolling ancestor); View: the subject (none: the target)
    u8 axis = 1;       ///< 0 = x (inline), 1 = y (block)
    f32 insetStart = 0.f, insetEnd = 0.f; ///< View: px
    f32 insetStartPct = 0.f, insetEndPct = 0.f; ///< View: plus these % of the scrollport (view-timeline-inset)
};
/// animation-range-start / -end (Scroll-driven Animations 1): a named range of a view timeline (cover, contain,
/// entry, exit, entry-crossing, exit-crossing) plus an offset into it (% of the range or px; Auto: 0% for the start,
/// 100% for the end). Normal: the timeline's start / end; Normal with an offset measures from the timeline's start
/// (cover). Scroll timelines know no names (they use the whole range). Outside a non-normal range the progress runs
/// below 0 / above 1, so the animation is in its before / after phase (fill-mode applies).
struct AnimRange {
    TimelineRange name = TimelineRange::Normal;
    Length offset{};
};
/// KeyframeEffectOptions + Animation settings.
struct AnimationOptions {
    f32 duration = 0.f, delay = 0.f, endDelay = 0.f; ///< seconds
    f32 iterations = 1.f;      ///< inf: infinite
    f32 iterationStart = 0.f;
    f32 playbackRate = 1.f;
    AnimDirection direction = AnimDirection::Normal;
    AnimFill fill = AnimFill::None;
    Easing easing{Easing::Kind::Bezier, 0.f, 0.f, 1.f, 1.f}; ///< whole-iteration timing function (default linear)
    AnimComposite composite = AnimComposite::Replace;
    bool autoplay = true;
    AnimTimeline timeline;
    AnimRange rangeStart, rangeEnd; ///< progress-based timelines: the attachment range
    std::string id;
    std::function<void(UiContext&, AnimationHandle)> onFinish, onCancel, onRemove; ///< after the frame's signals
};
enum class AnimPlayState : u8 { Idle, Running, Paused, Finished };
/// Web Animations "replace state": a finished, filling document-timeline animation whose every property is overridden
/// by later finished, filling animations on the same target is Removed after the frame (no effect, ScriptRemove signal,
/// onRemove) unless persist() made it Persisted.
enum class AnimReplaceState : u8 { Active, Removed, Persisted };

struct SignalArgs {
    f32 value = 0.f;
    bool flag = false;
    std::string_view text{};
    const Event* event = nullptr;
};
using Slot = InlineFn<void(UiContext&, NodeHandle, const SignalArgs&)>;
using CommandFn = void (*)(UiContext&, NodeHandle, u64 a, u64 b);

// ---- Accessibility -------------------------------------------------------------------------------------------------
enum class A11yRole : u8 {
    None, Window, Group, Text, Button, CheckBox, RadioButton, Switch, Slider, ProgressBar, TextField, ScrollArea,
    List, ListItem, Image, Separator, Link, ComboBox, SpinBox, Dial, ScrollBar, Tab,
    DateInput, TimeInput, DateTimeInput, ///< sectioned date / time fields (AccessKit DateInput / TimeInput / DateTimeInput)
    Grid, Row, GridCell, ColumnHeader,   ///< item views (table / tree / calendar grids); usually virtual parts
    ToolBar                              ///< QToolBar / ARIA toolbar
};
const char* a11yRoleName(A11yRole r);
/// Extra accessible state set by widgets (UiContext::setA11yState): listbox / combobox semantics.
enum class A11yAutoComplete : u8 { None, Inline, List, Both };
struct A11yState {
    signed char expanded = -1;          ///< -1 not expandable, 0 collapsed, 1 expanded (popup shown)
    bool selected = false;              ///< list item selection (ARIA aria-selected)
    A11yAutoComplete autoComplete = A11yAutoComplete::None;
    NodeHandle activeDescendant;        ///< the highlighted option while focus stays here (aria-activedescendant)
    NodeHandle controls;                ///< the popup this field drives (aria-controls)
    bool operator==(const A11yState&) const = default;
};
/// One accessible node in a flattened snapshot (UiContext::snapshotA11y). Nodes with role None are skipped and
/// their children re-parented, as in dumpA11y. `parent` indexes the snapshot vector (~0u for the top node).
struct A11ySnapshotNode {
    NodeHandle node;
    u32 parent = ~0u;
    A11yRole role = A11yRole::None;
    std::string name; ///< a11y name, else the node text
    Rect bounds;      ///< window coordinates (logical px)
    bool focusable = false, focused = false, checked = false, disabled = false;
    bool protectedText = false; ///< password field: AccessKit PasswordInput (UIA IsPassword, AT-SPI password text)
    f32 value = 0.f, lo = 0.f, hi = 0.f;
    A11yState state;
    std::string valueText; ///< what the widget shows for its value (spin box "$ 5 ea", a date) - read instead of the number
    /// Virtual part of `node` (WidgetClass::a11yParts): nonzero, unique per owner. Parts have no NodeHandle of their own.
    u32 part = 0;
    i32 rowIndex = -1, colIndex = -1; ///< Row / GridCell / ColumnHeader position
    u32 rowCount = 0, colCount = 0;   ///< Grid size
    u32 level = 0;                    ///< tree row depth, 1-based (0 = not hierarchical)
    u8 sortDirection = 0;             ///< ColumnHeader: 0 none, 1 ascending, 2 descending
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
    /// Optional: the shown value text for assistive tech (A11ySnapshotNode::valueText).
    std::string (*a11yValueText)(const UiContext&, NodeHandle) = nullptr;
    /// Optional: append virtual a11y children (grid rows / cells). Set `part` (nonzero) on each; `parent` is an index into
    /// the appended nodes or ~0u for the owner. The core fills `node` and rebases `parent`. `self` is the owner's own
    /// snapshot entry (set its rowCount / colCount, activeDescendant-like state).
    void (*a11yParts)(const UiContext&, NodeHandle, A11ySnapshotNode& self, std::vector<A11ySnapshotNode>&) = nullptr;
    /// Optional: AT Expand / Collapse on a virtual part (tree row). True when handled.
    bool (*a11yPartExpand)(UiContext&, NodeHandle, u32 part, bool open) = nullptr;
    /// Default style (applied before style sheets).
    ComputedStyle defaults{};
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
        if constexpr (requires { &W::a11yValueText; }) c.a11yValueText = &W::a11yValueText;
        if constexpr (requires { &W::a11yParts; }) c.a11yParts = &W::a11yParts;
        if constexpr (requires { &W::a11yPartExpand; }) c.a11yPartExpand = &W::a11yPartExpand;
        if constexpr (requires { W::defaults(); }) c.defaults = W::defaults();
        return add(std::move(c));
    }
    u32 find(std::string_view name) const; ///< 0xFFFFFFFF when unknown
    const WidgetClass& get(u32 id) const { return m_classes[id]; }
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
    /// Names a filter list for CSS `filter` / `backdrop-filter: url(#id)` (the SVG <filter> reference, here a CSS
    /// filter list: "blur(2px) sepia(1) drop-shadow(0 2px 4px #0008)"; url() inside it is ignored). False when
    /// `cssFilters` does not parse. Redefining replaces; affects the next paint.
    /// `cssFilters` is a CSS filter list, or SVG markup with a <filter> element (its primitives become a program).
    bool defineFilter(std::string_view id, std::string_view cssFilters);
    /// Register every <filter id="..."> of an SVG document for url(#id); returns how many compiled.
    int defineSvgFilters(std::string_view svgText);
    /// Names a premultiplied RGBA8 image for SVG feImage href="name" (or "#name") in filters defined after this.
    void defineFilterImage(std::string_view name, Image image);
    /// clip-path: url(#id) targets (an SVG <clipPath>): `path` in px from the node's border-box origin, or with
    /// `objectBoundingBox` in 0..1 units of the border box. An undefined id does not clip.
    void defineClipPath(std::string_view id, Path path, bool evenOdd = false, bool objectBoundingBox = false);
    void defineClipPath(std::string_view id, std::string_view svgPathData, bool evenOdd = false, bool objectBoundingBox = false);
    /// mask-image: url(name) images (premultiplied RGBA8). An undefined name is a transparent layer.
    void defineMaskImage(std::string_view name, Image image);
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
    NodeHandle lastChild(NodeHandle n) const;
    NodeHandle prevSibling(NodeHandle n) const;
    u32 childCount(NodeHandle n) const;
    // reordering (restyles :nth-* / sibling / :has selectors, relayouts the parent; focus and a11y follow tree order)
    NodeHandle childAt(NodeHandle parent, u32 index) const;
    i32 childIndex(NodeHandle n) const; ///< -1: invalid or the root
    bool insertBefore(NodeHandle n, NodeHandle reference); ///< moves n to just before reference (any parent)
    bool insertAfter(NodeHandle n, NodeHandle reference);
    bool setChildIndex(NodeHandle n, u32 index); ///< within its parent; index clamped to the last position
    bool swapNodes(NodeHandle a, NodeHandle b);  ///< neither may contain the other
    /// Re-links `parent`'s children in `order`, which must be a permutation of them. False (unchanged) otherwise.
    bool reorderChildren(NodeHandle parent, std::span<const NodeHandle> order);
    /// Stable sort of `parent`'s children by `less`.
    void sortChildren(NodeHandle parent, const std::function<bool(NodeHandle, NodeHandle)>& less);
    /// Called after each layout pass that arranged `n` (ResizeObserver / QLayout::setGeometry style): the hook may read
    /// rects and change styles (show / hide overflow items); one more style + layout pass then runs in the same
    /// update(). Null removes it. One hook per node; dropped with the node.
    using LayoutHook = void (*)(UiContext&, NodeHandle);
    void setLayoutHook(NodeHandle n, LayoutHook hook);
    u32 nodeCount() const { return m_live; }
    NodeHandle find(std::string_view id) const;
    NodeHandle handleOf(u32 index) const;
    std::string_view idOf(NodeHandle n) const;
    u32 classOf(NodeHandle n) const;

    // properties
    void addClass(NodeHandle n, std::string_view cls);
    /// Attributes for CSS attribute selectors ([name], [name="v"], ...).
    void setAttribute(NodeHandle n, std::string_view name, std::string_view value);
    void removeAttribute(NodeHandle n, std::string_view name);
    std::optional<std::string_view> attribute(NodeHandle n, std::string_view name) const;
    bool attributeAt(u32 index, u32 nameHash, std::string_view& out) const; ///< style matching
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
    void setA11y(NodeHandle n, A11yRole role, std::string_view name = {});
    /// Marks a text field as protected (password): platform adapters expose it as a password input.
    void setA11yProtected(NodeHandle n, bool on);
    bool a11yProtected(NodeHandle n) const;
    /// Expanded / selected / auto-complete / active-descendant / controls (reset when the node is destroyed).
    void setA11yState(NodeHandle n, const A11yState& st);
    A11yState a11yState(NodeHandle n) const;
    /// Assistive-tech expand / collapse (AccessKit Expand / Collapse, Android ACTION_EXPAND / COLLAPSE) of a node with
    /// an expanded state: focuses it (or its first focusable descendant) and sends Alt+Down to open, Escape to close.
    /// False when the node is not expandable or already in that state.
    bool a11yExpand(NodeHandle n, bool open);
    /// AT Expand / Collapse on virtual part `part` of `n` (WidgetClass::a11yPartExpand).
    bool a11yPartExpand(NodeHandle n, u32 part, bool open);
    /// Cursor the window should show (backends poll cursorRevision()); drag and drop sets Copy / Move / Link / Forbidden.
    void setCursorShape(CursorShape c) { if (c != m_cursor) { m_cursor = c; ++m_cursorRev; } }
    CursorShape cursorShape() const { return m_cursor; }
    u32 cursorRevision() const { return m_cursorRev; }
    A11yRole role(NodeHandle n) const;
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
    /// @media (prefers-color-scheme: dark) / (prefers-reduced-motion: reduce) inputs (the viewport feeds width / height).
    void setMediaPreferences(bool dark, bool reducedMotion);
    /// @media hover / any-hover / pointer / any-pointer inputs (pointer: 0 none, 1 coarse, 2 fine). Default: a mouse.
    void setInputMedia(bool hover, bool anyHover, u8 pointer, u8 anyPointer);
    /// @media resolution (device pixels per CSS px).
    void setMediaResolution(f32 dppx);
    void update(f32 dtSeconds = 0.f);
    bool needsUpdate() const; ///< false when an on-demand app can sleep
    /// Seconds until the context needs an update: 0 when dirty / animating / commands pending, the earliest timer
    /// otherwise, +inf when idle (an on-demand loop sleeps this long or until input).
    f32 nextWakeSeconds() const;
    const DrawList& drawList() const { return m_draw; }
    /// OS-window popups (Qt: a QMenu / combo list is its own top-level window). When on, overlay popup panels (the
    /// "clamp" nodes popupMenuAt / combo boxes / submenus create) are left out of drawList(); each is painted into its
    /// own list in popup-local coordinates (popupSurfaces()) for the host to show in a borderless OS window at
    /// window origin + rect, so a popup may extend past the window. They then clamp to `screen` (the work area in
    /// context coordinates; empty = the viewport) instead of the viewport. Pointer input for a popup window is fed
    /// back in context coordinates (local + rect origin).
    struct PopupSurface {
        NodeHandle node;
        Rect rect;      ///< context coordinates (may lie outside the viewport)
        DrawList draw;  ///< origin = rect top-left
        u64 revision = 0; ///< bumps whenever `draw` or `rect` changes
    };
    void setOsPopups(bool on, Rect screen = {});
    bool osPopups() const { return m_osPopups; }
    /// Where popups may be placed: the screen bounds with OS popups, the viewport otherwise.
    Rect popupArea() const;
    const std::vector<PopupSurface>& popupSurfaces() const { return m_popupSurfaces; }
    Rect damage() const { return m_damage; }
    const FrameStats& stats() const { return m_stats; }
    u64 frameIndex() const { return m_frame; }
    void markAllDirty();
    /// Physical pixels per logical unit (HiDPI). Layout stays in logical units; glyphs are rasterised at
    /// px * scale and emitted at logical size (rect = texel size / scale) so scaleDrawList() renders them 1:1.
    void setPixelScale(f32 scale);
    f32 pixelScale() const { return m_pixelScale; }
    /// Repaints `n` next update (widgets whose paint reads external state, e.g. item models).
    void invalidate(NodeHandle n) { if (node(n)) markPaint(n.index); }

    // input (returns true when the UI consumed it — game input arbitration)
    bool pointerMove(Vec2 p);
    bool pointerDown(Vec2 p, MouseButton b = MouseButton::Left);
    bool pointerUp(Vec2 p, MouseButton b = MouseButton::Left);
    bool wheel(Vec2 p, Vec2 notches);
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
    /// A viewport point in the node's own layout (untransformed) space, through every transform (2D, 3D,
    /// perspective, preserve-3d) above and on it; false when singular or behind the viewer.
    bool toLocal(NodeHandle n, Vec2 p, Vec2& out) const;
    /// The inverse: a point of the node's layout space on the viewport.
    bool fromLocal(NodeHandle n, Vec2 p, Vec2& out) const;
    /// The node's layout space to the viewport (row-major 3x3 homography, screen = H (x, y, 1) / w).
    std::array<f32, 9> screenMap(NodeHandle n) const;
    NodeHandle hovered() const { return m_hover; }
    Vec2 pointer() const { return m_pointer; } ///< last pointer position
    NodeHandle pressed() const { return m_pressed; }
    NodeHandle focused() const { return m_focus; }
    bool focusVisible() const { return m_focusVisible; }
    void setFocus(NodeHandle n, FocusReason why = FocusReason::Programmatic);
    bool focusNext(bool backward = false);
    bool navigate(NavDir d);
    void setModal(NodeHandle scope); ///< restricts focus and pointer input to `scope` (invalid handle clears)
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
    enum class Ease : u8 { Linear, InOutCubic, OutBack };
    void animate(NodeHandle n, AnimProp p, f32 to, f32 seconds, Ease e = Ease::InOutCubic);
    bool animating() const { return m_animCount > 0 || motionLive(); }
    /// Nodes with CSS transitions running (tests, inspector).
    u32 transitionCount() const;
    /// Nodes with CSS animations that still advance (not finished, not paused).
    u32 animationCount() const;
    /// Web Animations-style keyframe animation of `n` (Element.animate): composited over CSS animations in creation
    /// order, below CSS transitions. Invalid handle: dead node, no keyframes, or keyframe offsets out of order / range.
    AnimationHandle animate(NodeHandle n, std::vector<AnimKeyframe> keyframes, AnimationOptions options);
    /// Keyframes as CSS (`from { opacity: 0 } 50% { width: 10px; animation-timing-function: ease-in } to { ... }`).
    AnimationHandle animate(NodeHandle n, std::string_view keyframesCss, AnimationOptions options);
    bool animPlay(AnimationHandle a);
    bool animPause(AnimationHandle a);
    bool animReverse(AnimationHandle a);
    bool animFinish(AnimationHandle a); ///< false for a zero playback rate or an infinite end time
    bool animCancel(AnimationHandle a);
    bool animSetCurrentTime(AnimationHandle a, f32 seconds);
    bool animSetPlaybackRate(AnimationHandle a, f32 rate);
    /// Writes the animation's current values of its properties into n's inline style (Animation.commitStyles).
    bool animCommitStyles(AnimationHandle a);
    /// Animation.persist(): never auto-removed (a Removed one comes back into effect).
    bool animPersist(AnimationHandle a);
    AnimReplaceState animReplaceState(AnimationHandle a) const;
    /// KeyframeEffect.setKeyframes: new keyframes (offsets resolved as in animate); timing and play state are kept.
    bool animSetKeyframes(AnimationHandle a, std::vector<AnimKeyframe> keyframes);
    bool animSetKeyframes(AnimationHandle a, std::string_view keyframesCss);
    /// KeyframeEffect.target = n: the effect moves to another node (last in its composite order); both restyle.
    bool animSetTarget(AnimationHandle a, NodeHandle n);
    f32 animCurrentTime(AnimationHandle a) const;
    f32 animPlaybackRate(AnimationHandle a) const;
    AnimPlayState animPlayState(AnimationHandle a) const;
    NodeHandle animTarget(AnimationHandle a) const;
    std::string_view animId(AnimationHandle a) const;
    bool animValid(AnimationHandle a) const;
    /// Overall progress 0..1 of the active interval (computed timing); NaN when idle.
    f32 animProgress(AnimationHandle a) const;
    /// The animation's timeline progress 0..1 (Scroll / View); NaN for document timelines and inactive ones.
    f32 animTimelineProgress(AnimationHandle a) const;
    /// Progress 0..1 of a timeline for `target` (as an animation on `target` would see it); NaN when inactive.
    f32 timelineProgress(NodeHandle target, const AnimTimeline& t) const;
    /// Progress through an animation-range (unclamped when either end is not normal; NaN: inactive).
    f32 timelineProgress(NodeHandle target, const AnimTimeline& t, const AnimRange& start, const AnimRange& end) const;
    /// The document timeline: seconds of update(dt) time since the context was created (document.timeline.currentTime).
    double documentTime() const { return m_docTime; }
    /// The document time at which a running document-timeline animation started (Animation.startTime); NaN otherwise.
    double animStartTime(AnimationHandle a) const;
    /// The scripted animations of n (and its descendants with subtree) that are not idle (getAnimations()).
    std::vector<AnimationHandle> animations(NodeHandle n, bool subtree = false) const;

    // cross-thread commands (any thread)
    bool post(CommandFn fn, NodeHandle n, u64 a = 0, u64 b = 0);

    // dumps (tests, inspector)
    void dumpLayout(std::string& out) const;
    void dumpA11y(std::string& out) const;
    /// Flattened accessible tree of the visible nodes, root first, parents before children (for platform
    /// accessibility adapters: fuse_ui_a11y / AccessKit). Clears `out` first.
    void snapshotA11y(std::vector<A11ySnapshotNode>& out) const;

    // text
    const fuse::text::Font* font() const { return m_font; }
    fuse::text::GlyphAtlas& atlas() { return m_atlas; }
    /// Emits glyph commands for a laid-out block at `origin` (top-left).
    void paintTextLayout(DrawList& dl, const fuse::text::TextLayout& tl, Vec2 origin, Color c, f32 pixelSize, u32 node);
    /// Lays out and paints a one-off string (widgets' secondary labels). Uses an internal scratch layout.
    void paintString(DrawList& dl, std::string_view s, Rect box, Color c, f32 pixelSize, TextAlignment align, u32 node);
    f32 measureString(std::string_view s, f32 pixelSize);

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
    u32 applyMotion(u32 i, u32 idx, const ComputedStyle& shown, bool styled);
    void dropTransitions(u32 i);
    bool motionLive() const;
    bool nodeLive(u32 i) const;
    void markSiblings(u32 parent);
    void markRelational(u32 i);
    /// Marks the nodes whose selector match may change when a feature with reach `v` changes on node i.
    void invalidate(u32 i, const StyleEngine::Invalidation& v);
    void markHasAnchors(u32 i, const StyleEngine::Invalidation& v, bool self);
    void markDown(u32 i, u8 levels);
    void invalidateClasses(u32 i, std::span<const u32> classHashes);
    void relinkChildren(u32 parent, const std::vector<u32>& order);
    struct ScriptAnim;
    ScriptAnim* scriptAnim(AnimationHandle a);
    const ScriptAnim* scriptAnim(AnimationHandle a) const;
    void dropScriptAnims(u32 node);
    void advanceScriptAnim(u32 index, f32 dt);
    void scriptEvent(u32 index, SignalKind k);
    void settleScriptAnim(u32 index); ///< running past an end: finish
    bool scriptTiming(const ScriptAnim& a, f32& dp) const;
    bool sampleScriptAnim(const ScriptAnim& a, ComputedStyle& d) const;
    bool syncContainers(); ///< @container: re-styles subtrees of containers whose content size changed; true when any did
    std::unordered_map<u32, Size> m_containerSizes; ///< container node -> content size its subtree was styled against
    bool syncTimelines(); ///< progress-based animations follow their timelines; true when one moved
    std::vector<u32> m_timelineAnims;
    std::vector<u32> m_cssTlNodes; ///< nodes whose CSS animations follow animation-timeline (progress-based)
    /// animation-timeline of CSS animation slot k on node i: 0 document (auto), 1 none / unresolved, 2 progress (t)
    int cssTimeline(u32 i, const ComputedStyle& s, u8 k, AnimTimeline& t) const;
    void removeReplaced(); ///< after the frame: auto-remove fully replaced finished animations
    static bool resolveKeyframes(std::vector<AnimKeyframe>& frames);
    std::vector<AnimKeyframe> parseKeyframes(std::string_view css) const;
    double m_docTime = 0.0;
    std::vector<std::unique_ptr<ScriptAnim>> m_scriptAnims;
    std::vector<u32> m_freeScriptAnims;
    void queueMotion(u32 i, SignalKind k, f32 elapsed, std::string_view text);
    Size measure(u32 i, Size avail);
    void arrange(u32 i, Rect box);
    void arrangeChildren(u32 i);
    bool outOfFlow(u32 k) const;
    Rect containingBlock(u32 i, PositionKind pos) const;
    void placeOutOfFlow(u32 i, u32 k, const Rect& c);
    Rect stickyRect(u32 i, const ComputedStyle& ks, Rect r, const Rect& c) const;
    void paintPass();
    void paintRec(u32 i, f32 opacity, DrawList& dl);
    struct ZItem { u32 node; i32 z; };
    bool zItem(u32 k) const;
    bool stackingContext(u32 i) const;
    void collectZ(u32 sc, u32 i, std::vector<ZItem>& out) const;
    void zClips(u32 sc, u32 k, std::vector<u32>& out) const;
    void paintZ(u32 sc, u32 k, f32 opacity, DrawList& dl);
    NodeHandle hitZRec(u32 sc, u32 k, Vec2 q) const;
    bool maskClipFor(u32 i, const ComputedStyle& s, const MaskImageLookup& images, ClipLayer& out);
    struct MaskCacheEntry { std::array<u32, 8> key; f32 w, h, x, y; u32 style; ClipLayer layer; bool ok; };
    std::unordered_map<u32, MaskCacheEntry> m_maskCache;
    bool m_zActive = false;
    struct FilterDef {
        u8 count = 0; ///< CSS definition: ordered list (same encoding as ComputedStyle::filterFns)
        std::array<FilterFn, kMaxGridTracks> fns{};
        std::array<f32, kMaxGridTracks> amounts{};
        std::array<FilterShadow, kMaxFilterShadows> shadows{};
        FilterProgram program; ///< SVG <filter> definition (non-empty instead of the list)
    };
    /// one nested filter layer / backdrop pass: blur, then the matrix, then one drop shadow (or an SVG program)
    struct FilterStage {
        f32 blur = 0.f;
        ColorMatrix matrix;
        bool hasMatrix = false;
        DropShadow shadow{};
        const FilterProgram* program = nullptr;
        f32 reach() const;
    };
    /// split a CSS-order filter list into stages (url() expanded from m_filterDefs)
    void filterStages(const FilterFn* fns, const f32* amounts, u8 count, const FilterShadow* shadows,
                      std::vector<FilterStage>& out, int depth = 0) const;
    std::unordered_map<u32, FilterDef> m_filterDefs;
    std::unordered_map<std::string, Image> m_filterImages; ///< defineFilterImage
    struct ClipPathDef { Path path; bool evenOdd = false; bool objectBoundingBox = false; };
    std::unordered_map<std::string, ClipPathDef> m_clipPathDefs; ///< defineClipPath
    std::unordered_map<std::string, Image> m_maskImages;         ///< defineMaskImage
    FilterImageResolver filterImageResolver() const;
    u32 m_xfNode = ~0u;     ///< node whose transform paintRec is inside (its second, unmapped visit)
    u32 m_selfOnly = ~0u;   ///< preserve-3d plane being painted without its children (separate planes)
    Mat4 nodeMatrix(u32 i) const;        ///< parent's perspective * own transform (about the origins)
    bool preserves3d(u32 i) const;       ///< transform-style: preserve-3d in effect (no grouping property)
    struct Plane3d { u32 node; Mat4 m; bool selfOnly; f32 depth; };
    void collectPlanes(u32 i, const Mat4& acc, std::vector<Plane3d>& out) const;
    bool bspOrder(const std::vector<Plane3d>& planes, std::vector<Frag3d>& out) const;
    Rect paintBounds(u32 i) const;
    bool paintsSelf(u32 i, f32 opacity);
    DrawList m_scratch3d;
    bool paintTransformed(u32 i, f32 opacity, DrawList& dl);
    u32 m_clipPathNode = ~0u; ///< node whose clip-path paintRec is inside (its second visit)
    u32 m_filterNode = ~0u; ///< node whose filter layer paintRec is inside (its second, unwrapped visit)
    /// Overlay scrollbar geometry of a scroll container along an axis (0 = x, 1 = y); false when it does not overflow.
    bool scrollBarGeom(u32 i, u8 axis, Rect& track, Rect& thumb) const;
    NodeHandle hitRec(u32 i, Vec2 p, const Mat4& acc, f32* depth = nullptr) const;
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

    WidgetRegistry m_registry;
    StyleEngine m_styles;
    const fuse::text::Font* m_font = nullptr;
    fuse::text::GlyphAtlas m_atlas;
    f32 m_pixelScale = 1.f;
    std::vector<Node> m_nodes;
    std::vector<std::unique_ptr<Cold>> m_cold;
    std::vector<u32> m_free;
    u32 m_live = 0;
    NodeHandle m_root;
    Size m_viewport;
    u32 m_layerClass = 0xFFFFFFFFu;
    DrawList m_draw;
    Rect m_damage;
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
    bool m_focusBackward = false; ///< focusNext(backward) in progress: FocusIn carries kModShift
    bool m_spatialNav = false;
    std::string m_composition;
    i32 m_compositionCaret = 0;
    NodeHandle m_caretNode;
    Rect m_caret;
    ClipboardHooks m_clipboard;
    std::string m_localClipboard;
    Vec2 m_pointer{-1.f, -1.f};
    u8 m_mods = 0;
    struct ScrollDrag { NodeHandle node; u8 axis = 0; f32 grab = 0.f; } m_scrollDrag;
    CursorShape m_cursor = CursorShape::Default;
    u32 m_cursorRev = 0;
    std::vector<u32> m_path;
    std::vector<u32> m_scratchOrder;
    std::vector<std::pair<u32, i32>> m_scratchTab;
    std::vector<fuse::text::ShapedGlyph> m_shapeScratch;
    fuse::text::TextLayout m_stringScratch;

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
    std::vector<u32> m_transNodes; ///< nodes with CSS transitions or animations (scratch styles)
    struct MotionEvent {
        u32 node, gen;
        SignalKind kind;
        f32 elapsed;
        std::string_view text; ///< static (property css name / interned animation name)
        u32 anim = ~0u, animGen = 0; ///< scripted animation (callbacks)
    };
    std::vector<MotionEvent> m_motionEvents; ///< dispatched at the end of update()
    struct LayoutHookEntry { u32 index, gen; LayoutHook fn; };
    std::vector<LayoutHookEntry> m_layoutHooks;

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
