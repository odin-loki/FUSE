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
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fuse::ui {

class UiContext;
class IconSet;
class EffectRegistry;

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
    Vec2 pos;              ///< pointer events: context coordinates
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
enum class SignalKind : u8 { Clicked, Toggled, ValueChanged, TextChanged, Submitted, FocusChanged, HoverChanged, Event };
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
    List, ListItem, Image, Separator, Link, ComboBox, SpinBox, Dial, ScrollBar, Tab
};
const char* a11yRoleName(A11yRole r);
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
};

// ---- Widget classes ------------------------------------------------------------------------------------------------
/// Horizontal mirror of `r` / `x` inside `box` (right-to-left widgets compute left-to-right geometry, then mirror it).
inline Rect mirrorX(const Rect& r, const Rect& box) { return {2.f * box.x + box.w - r.x - r.w, r.y, r.w, r.h}; }
inline f32 mirrorX(f32 x, const Rect& box) { return 2.f * box.x + box.w - x; }

struct PaintContext {
    UiContext& ui;
    NodeHandle node;
    Rect rect;    ///< border box
    Rect content; ///< inside padding
    const ComputedStyle& style;
    u32 states;
    f32 opacity;  ///< accumulated (multiply colours by it)
    Color tint(Color c) const { return c.withAlpha(c.a * opacity); }
    /// `direction: rtl`: mirrored in the content box; unchanged left-to-right.
    Rect flip(const Rect& r) const { return style.rtl ? mirrorX(r, content) : r; }
    Vec2 flip(Vec2 p) const { return style.rtl ? Vec2{mirrorX(p.x, content), p.y} : p; }
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
    std::expected<void, UiError> addStyleSheet(std::string_view css, std::vector<StyleDiagnostic>* diags = nullptr);
    void addStyleSheet(StyleSheet sheet); ///< an already parsed (or cooked) sheet
    void clearStyleSheets();
    /// The theme sheet slot: ordered before every other sheet and replaced by the next call (widgets::install puts
    /// the default theme here; switching dark / light replaces it without touching app sheets).
    std::expected<void, UiError> setThemeStyleSheet(std::string_view css, std::vector<StyleDiagnostic>* diags = nullptr);

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
    void setA11y(NodeHandle n, A11yRole role, std::string_view name = {});
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
    /// Goes to the focused node (capture / target / bubble). Unconsumed: Enter / Space click an interactive node, Tab /
    /// Shift+Tab move focus (Ctrl+Tab always does, before the node sees it), arrows navigate spatially when enabled,
    /// and anything left is emitted as SignalKind::Event {value = Key, event} on the root (application shortcuts).
    /// Keyboard focus scrolls into view inside scroll containers.
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
    // ---- platform services (UI1): DPI scale, cursor, reduced motion, touch / pen ----
    /// Device pixels per UI unit (the window's DPI scale). Layout, hit testing, events, rect() and damage() stay in
    /// UI units; drawList() and popupSurfaces() are emitted in device pixels and text is rasterised at
    /// font-size * scale (crisp glyphs, not a stretched bitmap). Changing it re-lays out and repaints everything.
    void setScale(f32 s);
    f32 scale() const { return m_scale; }
    /// Text whose device pixel size (font-size * scale) is at least `devicePx` is drawn from multi-channel signed
    /// distance fields (one MSDF per glyph at GlyphAtlas::kMsdfPixelSize, scaled; crisp at any size, e.g. zoomed or
    /// world-space UI) instead of per-size coverage. 0 (default) turns it off. Calls fuse::text::enableMsdf().
    void setMsdfThreshold(f32 devicePx);
    f32 msdfThreshold() const { return m_msdfThreshold; }
    /// The cursor the pointer should show: the pressed node's while it is held (a drag keeps its resize cursor), else
    /// the hovered node's `cursor` style, inherited from ancestors; Arrow when none sets one.
    CursorShape cursor() const;
    /// Reduced motion (the OS accessibility setting): animate() jumps straight to the target value.
    void setReducedMotion(bool on) { m_reducedMotion = on; }
    bool reducedMotion() const { return m_reducedMotion; }
    /// Touch contacts in UI units. The first finger down (while none is down) is the primary touch and drives the
    /// pointer like a mouse (move + press / release); the others are only tracked (gestures: touches(), pinchScale()).
    /// A primary drag that goes mostly vertical by more than kTouchSlop over a scroll container that does not scroll
    /// from the drag itself becomes a pan: the press is released without a click and the finger scrolls the container
    /// (touchPanning()).
    struct TouchPoint {
        u32 id = 0;
        Vec2 pos, start;
    };
    static constexpr u32 kMaxTouches = 10;
    bool touchDown(u32 id, Vec2 p);
    bool touchMove(u32 id, Vec2 p);
    bool touchUp(u32 id, Vec2 p);
    void touchCancel(); ///< drops every contact (the primary releases without a click)
    static constexpr f32 kTouchSlop = 8.f; ///< UI units a finger moves before a drag can become a pan
    bool touchPanning() const { return m_touchPan; }
    std::span<const TouchPoint> touches() const { return {m_touches.data(), m_touchCount}; }
    /// Two-finger pinch: current distance of the first two contacts / their distance when the second went down.
    f32 pinchScale() const;
    /// What produced the last pointer event (platform glue sets it; pen pressure 0..1).
    enum class PointerKind : u8 { Mouse, Touch, Pen };
    void setPointerKind(PointerKind k, f32 pressure = 1.f) { m_pointerKind = k; m_pressure = pressure; }
    PointerKind pointerKind() const { return m_pointerKind; }
    f32 pressure() const { return m_pressure; }

    NodeHandle hitTest(Vec2 p) const;
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
    /// FontWeight tweens the node's font-weight (variable UI fonts: the wght axis) over its style value.
    enum class AnimProp : u8 { Opacity, OffsetX, OffsetY, Value, FontWeight };
    enum class Ease : u8 { Linear, InOutCubic, OutBack };
    void animate(NodeHandle n, AnimProp p, f32 to, f32 seconds, Ease e = Ease::InOutCubic);
    bool animating() const { return m_animCount > 0; }

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
    /// The font text of `node` is drawn with: the UI font at the node's font-weight / font-stretch /
    /// font-variation-settings (font-optical-sizing auto: opsz follows font-size) when it is variable.
    const fuse::text::Font& textFont(NodeHandle node);
    /// Draws `node`'s own text (its label text, paintString / measureString from its widget) with `font` instead of the
    /// UI font (non-owning; nullptr restores the UI font). Style weight / stretch / variations still apply.
    void setFont(NodeHandle node, const fuse::text::Font* font);
    fuse::text::GlyphAtlas& atlas() { return m_atlas; }
    /// Emits glyph commands for a laid-out block at `origin` (top-left).
    void paintTextLayout(DrawList& dl, const fuse::text::TextLayout& tl, Vec2 origin, Color c, f32 pixelSize, u32 node);
    /// Lays out and paints a one-off string (widgets' secondary labels). Uses an internal scratch layout.
    void paintString(DrawList& dl, std::string_view s, Rect box, Color c, f32 pixelSize, TextAlignment align, u32 node);
    /// paintString with an explicit base font (list rows previewing other faces); nullptr: the node's font.
    void paintString(DrawList& dl, std::string_view s, Rect box, Color c, f32 pixelSize, TextAlignment align, u32 node,
                     const fuse::text::Font* font);
    f32 measureString(std::string_view s, f32 pixelSize);

    // icons (icons.hpp): `icon` / `icon-size` style properties, widgets::icon
    /// Attaches an icon pack (non-owning; must outlive the context or be detached). Re-lays out every node.
    void setIcons(const IconSet* icons);
    const IconSet* icons() const { return m_icons; }
    static constexpr f32 kIconGap = 6.f; ///< between an icon and its label
    /// Icon cell size for a style: 0 without an icon (or when the attached set has no such icon).
    f32 iconExtent(const ComputedStyle& s) const;
    /// Draws icon `nameHash` as a size x size cell with its top-left at `at`, tinted `c`. False when unknown.
    bool paintIcon(DrawList& dl, u32 nameHash, Vec2 at, f32 size, Color c, u32 node);
    bool paintIcon(DrawList& dl, std::string_view name, Vec2 at, f32 size, Color c, u32 node) {
        return paintIcon(dl, hashId(name), at, size, c, node);
    }
    // custom shader effects (effects.hpp): `effect: name(params)` style property
    /// Attaches an effect registry (non-owning; must outlive the context or be detached). Repaints every node.
    void setEffects(const EffectRegistry* effects);
    const EffectRegistry* effects() const { return m_effects; }
    /// Seconds of update(dt) so far: the time animated effects see.
    f32 elapsed() const { return m_elapsed; }
    /// Content size of the style's icon followed by a one-line `text` (either may be absent).
    Size measureIconLabel(const ComputedStyle& s, std::string_view text);
    /// Paints the style's icon then `text` in `box` (both vertically centred; the pair aligned by `align`).
    void paintIconLabel(DrawList& dl, const ComputedStyle& s, std::string_view text, Rect box, Color c, TextAlignment align, u32 node);

    struct Node;
    struct Cold;
    struct GlyphQuad;

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
    /// Scrolls the node's scroll-container ancestors (vertically) so keyboard focus is in view.
    void revealFocus(u32 i);
    void drainCommands();
    void ensureText(u32 i, f32 width);
    const fuse::text::Font& fontFor(u32 i);
    const fuse::text::Font& fontFor(const fuse::text::Font& base, const ComputedStyle& s, f32 weight);
    const fuse::text::Font& nodeFont(u32 i); ///< fontFor(i) for a live node, else the current text font
    f32 labelIconWidth(const ComputedStyle& s, bool hasText) const;
    /// paintTextLayout through the node's cached glyph quads (`src` 0: its text layout, 1: its paintString layout).
    void paintTextQuads(DrawList& dl, u32 i, u8 src, u32 rev, const fuse::text::TextLayout& tl, Vec2 o, Color c, f32 px,
                        const fuse::text::Font* layoutFont = nullptr);
    void paintLayoutWith(DrawList& dl, const fuse::text::Font& font, const fuse::text::TextLayout& tl, Vec2 o, Color c, f32 px,
                         u32 node);
    bool resolveGlyph(const fuse::text::Font& f, std::uint32_t glyph, f32 fpx, const fuse::text::ColorLayer* layer, Color col,
                      GlyphQuad& q);
    void emitGlyph(DrawList& dl, const GlyphQuad& q, Vec2 o, Color col, f32 dpx, u32 nodeIdx) const;
    void collectTab(std::vector<u32>& out) const;
    void markArrange(u32 i);
    bool visibleChain(u32 i) const;
    void scaleDrawList(DrawList& dl) const;

    WidgetRegistry m_registry;
    StyleEngine m_styles;
    const fuse::text::Font* m_font = nullptr;
    const fuse::text::Font* m_textFont = nullptr; ///< font of the node being measured / painted (measureString)
    fuse::text::GlyphAtlas m_atlas;
    std::vector<Node> m_nodes;
    std::vector<Cold*> m_cold; ///< per node, into m_coldChunks
    std::vector<std::unique_ptr<Cold[]>> m_coldChunks; ///< Cold in fixed chunks: neighbouring nodes stay adjacent in memory
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
    bool m_spatialNav = false;
    std::string m_composition;
    i32 m_compositionCaret = 0;
    NodeHandle m_caretNode;
    Rect m_caret;
    ClipboardHooks m_clipboard;
    std::string m_localClipboard;
    Vec2 m_pointer{-1.f, -1.f};
    u8 m_mods = 0;
    f32 m_scale = 1.f;
    f32 m_msdfThreshold = 0.f;
    bool m_reducedMotion = false;
    PointerKind m_pointerKind = PointerKind::Mouse;
    f32 m_pressure = 1.f;
    std::array<TouchPoint, kMaxTouches> m_touches{};
    u32 m_touchCount = 0;
    u32 m_primaryTouch = 0xFFFFFFFFu;
    f32 m_pinchStart = 0.f;
    bool m_touchPan = false, m_touchPanChecked = false;
    struct ScrollDrag { NodeHandle node; u8 axis = 0; f32 grab = 0.f; } m_scrollDrag;
    std::vector<u32> m_path;
    std::vector<u32> m_scratchOrder;
    std::vector<std::pair<u32, i32>> m_scratchTab;
    std::vector<fuse::text::ShapedGlyph> m_shapeScratch;
    fuse::text::TextLayout m_stringScratch;
    const IconSet* m_icons = nullptr;
    std::vector<u8> m_iconScratch;
    const EffectRegistry* m_effects = nullptr;
    std::vector<u32> m_fxAnimated; ///< nodes painted with an animated effect last paint (repainted every update)
    f32 m_elapsed = 0.f;
    void paintEffect(u32 i, const ComputedStyle& s, DrawList& dl);
    // atlas-resolved glyphs of every painted text node in one array, in paint order (no per-glyph atlas lookups, no
    // walks of scattered per-node layout vectors); rebuilt per node when its layout changes, wholesale on atlas reset
    std::vector<GlyphQuad> m_quads;
    u32 m_quadEpoch = 1, m_quadAtlasGen = 0;
    std::size_t m_quadLive = 0;

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
