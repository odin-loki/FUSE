// Internal glue between the Tier A and Tier B widget sources.
#pragma once

#include "fuse/ui/ui.hpp"

namespace fuse::ui::widgets {
class SelectionModel;
enum class SelectionMode : u8;
} // namespace fuse::ui::widgets
#include <initializer_list>
#include <string_view>
#include <vector>

namespace fuse::ui::widgets::detail {

/// Opens the combo box's popup list (Tier B). False when the Tier B classes are not registered (Tier A cycles).
bool openComboPopup(UiContext& ui, NodeHandle combo);
void registerTierB2(UiContext& ui);
/// Popup menu whose item values start at `base` (submenus report the line index in the root list).
NodeHandle popupMenuAt(UiContext& ui, NodeHandle owner, std::string_view items, Vec2 at, u32 base, std::string_view id = {});
/// A popup scope (outside click / Escape closes it) holding an empty card at `at`; returns the card.
NodeHandle popupPanel(UiContext& ui, NodeHandle owner, Vec2 at);

/// Registers the item view classes "tableview" and "treemodelview" (widgets_modelview.cpp).
void registerModelView(UiContext& ui);
/// Grid view selection (widgets_tier_cd.cpp), behind setViewSelectionMode / setViewSelectionModel: `shared` replaces
/// the grid's selection model (null = a private one); `setMode` also sets the mode. False when `grid` is no grid view.
bool gridSetSelection(UiContext& ui, NodeHandle grid, SelectionModel* shared, bool setMode, SelectionMode mode);
SelectionModel* gridSelection(UiContext& ui, NodeHandle grid);

// widgets_svg.cpp: icons on buttons (setIcon). Both are no-ops (one empty-table test) while no icon is set anywhere.
/// The content size of a button with text size `text`: adds the icon and the gap.
Size iconContentSize(const UiContext& ui, NodeHandle n, Size text);
/// Paints the node's icon (centred with the text as one group); `textBox` receives where the text goes. False when
/// the node has no icon.
bool paintIcon(const PaintContext& pc, DrawList& dl, Rect& textBox);

// ---- theme token colours for paint code -----------------------------------------------------------------------------
// Every colour a paint function uses that is not a computed style value comes from a `:root` token of the theme
// (theme.cpp lists them with their dark / light / high-contrast values). The literal passed next to the token is the
// dark value: it is what an app without the widget theme gets. Not themed on purpose: data colours (colour picker
// gradients and markers, flame / pin / clip palettes and the text over them, images).
namespace tok {
// @tok-begin
inline constexpr u32 bg = hashId("--bg");
inline constexpr u32 surface = hashId("--surface");
inline constexpr u32 surface_hi = hashId("--surface-hi");
inline constexpr u32 border = hashId("--border");
inline constexpr u32 text = hashId("--text");
inline constexpr u32 text_dim = hashId("--text-dim");
inline constexpr u32 accent = hashId("--accent");
inline constexpr u32 accent_hi = hashId("--accent-hi");
inline constexpr u32 danger = hashId("--danger");
inline constexpr u32 hover = hashId("--hover");
inline constexpr u32 pressed = hashId("--pressed");
inline constexpr u32 on_accent = hashId("--on-accent");
inline constexpr u32 heading = hashId("--heading");
inline constexpr u32 field = hashId("--field");
inline constexpr u32 field_alt = hashId("--field-alt");
inline constexpr u32 tooltip_bg = hashId("--tooltip-bg");
inline constexpr u32 code_bg = hashId("--code-bg");
inline constexpr u32 code_text = hashId("--code-text");
inline constexpr u32 quote_bg = hashId("--quote-bg");
inline constexpr u32 canvas = hashId("--canvas");
inline constexpr u32 warning = hashId("--warning");
inline constexpr u32 shadow_card = hashId("--shadow-card");
inline constexpr u32 shadow_pop = hashId("--shadow-pop");
inline constexpr u32 scrim = hashId("--scrim");
inline constexpr u32 primary_ring = hashId("--primary-ring");
inline constexpr u32 inset = hashId("--inset");
inline constexpr u32 control_border = hashId("--control-border");
inline constexpr u32 track = hashId("--track");
inline constexpr u32 knob = hashId("--knob");
inline constexpr u32 switch_knob = hashId("--switch-knob");
inline constexpr u32 knob_hi = hashId("--knob-hi");
inline constexpr u32 scroll_track = hashId("--scroll-track");
inline constexpr u32 scroll_thumb = hashId("--scroll-thumb");
inline constexpr u32 scroll_thumb_active = hashId("--scroll-thumb-active");
inline constexpr u32 view_scroll_track = hashId("--view-scroll-track");
inline constexpr u32 progress_track = hashId("--progress-track");
inline constexpr u32 dial_face = hashId("--dial-face");
inline constexpr u32 separator = hashId("--separator");
inline constexpr u32 raised = hashId("--raised");
inline constexpr u32 grid_line = hashId("--grid-line");
inline constexpr u32 stripe = hashId("--stripe");
inline constexpr u32 hot = hashId("--hot");
inline constexpr u32 glyph_dim = hashId("--glyph-dim");
inline constexpr u32 strip = hashId("--strip");
inline constexpr u32 dropzone = hashId("--dropzone");
inline constexpr u32 dropzone_glyph = hashId("--dropzone-glyph");
inline constexpr u32 link_hover = hashId("--link-hover");
inline constexpr u32 chart_line = hashId("--chart-line");
inline constexpr u32 canvas_grid = hashId("--canvas-grid");
inline constexpr u32 canvas_grid_hi = hashId("--canvas-grid-hi");
inline constexpr u32 wire = hashId("--wire");
inline constexpr u32 shadow = hashId("--shadow");
inline constexpr u32 line_strong = hashId("--line-strong");
inline constexpr u32 node_head = hashId("--node-head");
inline constexpr u32 pin_outline = hashId("--pin-outline");
inline constexpr u32 key = hashId("--key");
inline constexpr u32 key_selected = hashId("--key-selected");
inline constexpr u32 text_faint = hashId("--text-faint");
inline constexpr u32 on_clip = hashId("--on-clip");
inline constexpr u32 row_line = hashId("--row-line");
inline constexpr u32 media_bg = hashId("--media-bg");
inline constexpr u32 media_bar = hashId("--media-bar");
inline constexpr u32 code_editor_bg = hashId("--code-editor-bg");
inline constexpr u32 gutter = hashId("--gutter");
inline constexpr u32 gutter_text = hashId("--gutter-text");
inline constexpr u32 gutter_text_hi = hashId("--gutter-text-hi");
inline constexpr u32 current_line = hashId("--current-line");
inline constexpr u32 selection = hashId("--selection");
inline constexpr u32 caret = hashId("--caret");
inline constexpr u32 code_fg = hashId("--code-fg");
inline constexpr u32 code_keyword = hashId("--code-keyword");
inline constexpr u32 code_string = hashId("--code-string");
inline constexpr u32 code_number = hashId("--code-number");
inline constexpr u32 code_comment = hashId("--code-comment");
inline constexpr u32 hover_wash = hashId("--hover-wash");
inline constexpr u32 meter_bg = hashId("--meter-bg");
inline constexpr u32 meter_ok = hashId("--meter-ok");
inline constexpr u32 meter_warn = hashId("--meter-warn");
inline constexpr u32 meter_danger = hashId("--meter-danger");
inline constexpr u32 meter_hold = hashId("--meter-hold");
inline constexpr u32 meter_lamp_off = hashId("--meter-lamp-off");
inline constexpr u32 lcd_text = hashId("--lcd-text");
inline constexpr u32 lcd_bg = hashId("--lcd-bg");
inline constexpr u32 workspace = hashId("--workspace");
inline constexpr u32 scene_bg = hashId("--scene-bg");
inline constexpr u32 item_focus = hashId("--item-focus");
inline constexpr u32 placeholder = hashId("--placeholder");
inline constexpr u32 window_close = hashId("--window-close");
inline constexpr u32 focus_ring = hashId("--focus-ring");
inline constexpr u32 text_disabled = hashId("--text-disabled");
// @tok-end
} // namespace tok
/// Token colour (fallback: the dark literal `rgb` with `alpha`). Not multiplied by the node opacity: wrap in pc.tint().
inline Color themed(const PaintContext& pc, u32 token, u32 rgb, f32 alpha = 1.f) {
    return pc.ui.themeColor(token, Color::rgb(rgb, alpha));
}
/// Placeholder / "nothing set" text of a field: the theme's --placeholder; without the widget theme the node's text
/// colour at half alpha (what the dark token is over the dark text colour).
inline Color placeholderColor(const PaintContext& pc) {
    return pc.ui.themeColor(tok::placeholder, pc.style.color.withAlpha(pc.style.color.a * 0.5f));
}
/// Token colour with its alpha scaled by `alpha` (washes, translucent thumbs).
inline Color themedA(const PaintContext& pc, u32 token, u32 rgb, f32 alpha) {
    const Color c = pc.ui.themeColor(token, Color::rgb(rgb));
    return c.withAlpha(c.a * alpha);
}

// ---- accessibility helpers for the class hooks (WidgetClass::a11y / a11yAction) --------------------------------------
constexpr u32 a11yBits(std::initializer_list<A11yAction> actions) {
    u32 m = 0;
    for (A11yAction a : actions) m |= a11yActionBit(a);
    return m;
}
/// What a selectable item (row, tile, tab) accepts.
constexpr u32 kA11yItemActions = a11yBits({A11yAction::Select, A11yAction::Click, A11yAction::ScrollIntoView});
/// A virtual child: `child` is its stable id inside the widget (never 0), `bounds` in context coordinates.
inline A11ySnapshotNode a11yItem(u32 child, A11yRole role, std::string_view name, Rect bounds, u32 actions = 0) {
    A11ySnapshotNode it;
    it.child = child;
    it.role = role;
    it.name.assign(name);
    it.bounds = bounds;
    it.actions = actions;
    return it;
}
/// Drops the core's fallback name (the node text) for a class whose text is its item list or markup.
inline void a11yDropTextName(const UiContext& ui, NodeHandle n, A11ySnapshotNode& self) {
    if (!self.name.empty() && self.name == ui.text(n)) self.name.clear();
}
/// Sets the role and hooks of a registered class (no-op when the class is not registered).
inline void a11yHook(WidgetRegistry& r, std::string_view cls, A11yRole role,
                     void (*a11y)(const UiContext&, NodeHandle, A11ySnapshotNode&, std::vector<A11ySnapshotNode>&) = nullptr,
                     bool (*action)(UiContext&, NodeHandle, u32, A11yAction, f32, std::string_view) = nullptr) {
    const u32 id = r.find(cls);
    if (id == 0xFFFFFFFFu) return;
    WidgetClass& c = r.edit(id);
    c.role = role;
    c.a11y = a11y;
    c.a11yAction = action;
}
// widgets_lineedit.cpp: line edits with options (setLineEditOptions) or a completer paint and handle events there.
/// True when `n` has a line edit record (one empty-table test while no line edit has options).
bool lineEditHasState(const UiContext& ui, NodeHandle n);
void lineEditEnsureState(UiContext& ui, NodeHandle n);
void lineEditPaint(const PaintContext& pc, DrawList& dl);
bool lineEditEvent(EventContext& ec, const Event& e);
/// Sets a line edit's text from the completer (TextChanged, no refilter); selects [selFrom, end) unless npos.
void lineEditComplete(UiContext& ui, NodeHandle edit, std::string_view text, std::size_t selFrom);
// widgets_spinbox.cpp: spin boxes with options (setSpinBoxOptions) measure, paint and handle events there.
bool spinBoxHasState(const UiContext& ui, NodeHandle n);
Size spinBoxMeasure(UiContext& ui, NodeHandle n);
void spinBoxPaint(const PaintContext& pc, DrawList& dl);
bool spinBoxEvent(EventContext& ec, const Event& e);
void a11ySpinBox(WidgetRegistry& r);
// widgets_tabs.cpp: tab bars with options (closable / movable / disabled tabs, scroll buttons).
bool tabBarHasState(const UiContext& ui, NodeHandle n);
Size tabBarMeasure(UiContext& ui, NodeHandle n);
void tabBarPaint(const PaintContext& pc, DrawList& dl);
bool tabBarEvent(EventContext& ec, const Event& e);
Rect tabBarTabBounds(UiContext& ui, NodeHandle n, u32 index);
bool tabBarTabDisabled(const UiContext& ui, NodeHandle n, u32 index);
// widgets_extra.cpp: a key in a modal dialog scope: Escape presses its reject button, an unconsumed Enter its default
// button (setDialogRejectButton / setDialogDefaultButton, the dialog button box). False: not handled here.
bool dialogKey(UiContext& ui, NodeHandle scope, const Event& e, Phase phase);
void a11yLineEdit(WidgetRegistry& r); // widgets_lineedit.cpp: SetValue through the field's options
bool wizardCanLeave(UiContext& ui, NodeHandle wizard, u32 page); // widgets_extra.cpp: setWizardValidator
// completer.cpp: completer hooks from the line edit: a KeyDown while focused (true = consumed), a user edit, focus loss.
bool completerKey(UiContext& ui, NodeHandle edit, const Event& e);
void completerEdited(UiContext& ui, NodeHandle edit, bool deletion);
/// True when the focus only left for a press on the field's own completion list / combo button (it comes back).
bool completerFocusOut(UiContext& ui, NodeHandle edit);

/// Registers class W unless the registry has it; true when it was added. (A context trimmed to a document's
/// registration list - installClasses - may hold only some of a source file's classes.)
template <class W>
bool registerMissing(WidgetRegistry& r) {
    if (r.find(W::kName) != 0xFFFFFFFFu) return false;
    r.template registerWidget<W>();
    return true;
}
/// Per source file: installs the accessibility roles / hooks of the classes it registers (defined at its end).
void a11yTierA(WidgetRegistry& r);
void a11yTierB(WidgetRegistry& r);
void a11yTierB2(WidgetRegistry& r);
void a11yModelView(WidgetRegistry& r); // widgets_modelview.cpp: tableview (table / model list), treemodelview
void a11yTierB3(WidgetRegistry& r);
void a11yTierCD(WidgetRegistry& r);
void a11yTierD2(WidgetRegistry& r);

} // namespace fuse::ui::widgets::detail

namespace fuse::ui::widgets {
namespace tok = detail::tok; // the widget sources name the tokens and helpers without `detail::`
using detail::themed;
using detail::themedA;
using detail::placeholderColor;
} // namespace fuse::ui::widgets
