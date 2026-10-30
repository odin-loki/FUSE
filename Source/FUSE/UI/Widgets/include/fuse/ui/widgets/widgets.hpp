// FUSE UI widgets (plan §5). Tier A: Label, Button, ToolButton, CheckBox, RadioButton, Switch, Slider, ScrollBar,
// SpinBox, ComboBox, LineEdit, ProgressBar, Dial, Image, Separator, Link.
//
// Every widget is a registered WidgetClass (plain function table) over core nodes; state lives in the node (value,
// range, checked, text, edit buffer). Styling comes from the theme sheet: `color` = text, `background` = face,
// `background2` = accent / indicator fill (when no gradient is set), `border-color` = outline.
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <limits>
#include <span>

#include <string>
#include <vector>

#include "fuse/ui/ui.hpp"

#include <string_view>

namespace fuse::ui::widgets {

/// Registers every Tier A class with the context's registry.
void registerTierA(UiContext& ui);
/// The default dark theme (theme tokens in :root). Style sheet text, lives for the program.
std::string_view defaultThemeCss();
/// Registers the Tier B classes (popup, menu, menuitem, menubar, menubutton, listview, treeview, tabbar, tooltip).
void registerTierB(UiContext& ui);
/// registerTierA + registerTierB + default theme. (With Tier B registered, combo boxes open a popup list on click.)
void install(UiContext& ui);
/// Registers only the named widget classes (a cooked registration list, see fuse/ui/doc/cook.hpp) plus the default
/// theme. Names the context already has (core panel / label) are skipped. Returns how many names are unknown.
u32 installClasses(UiContext& ui, std::span<const std::string_view> classes, bool theme = true);

NodeHandle label(UiContext& ui, NodeHandle parent, std::string_view text, std::string_view id = {});
NodeHandle button(UiContext& ui, NodeHandle parent, std::string_view text, std::string_view id = {});
NodeHandle toolButton(UiContext& ui, NodeHandle parent, std::string_view text, std::string_view id = {});
NodeHandle checkBox(UiContext& ui, NodeHandle parent, std::string_view text, bool checked = false, std::string_view id = {});
NodeHandle radioButton(UiContext& ui, NodeHandle parent, std::string_view text, bool checked = false, std::string_view id = {});
NodeHandle toggleSwitch(UiContext& ui, NodeHandle parent, std::string_view text, bool on = false, std::string_view id = {});
NodeHandle slider(UiContext& ui, NodeHandle parent, f32 lo, f32 hi, f32 value, f32 step = 0.f, std::string_view id = {});
NodeHandle scrollBar(UiContext& ui, NodeHandle parent, bool vertical, std::string_view id = {});
NodeHandle spinBox(UiContext& ui, NodeHandle parent, f32 lo, f32 hi, f32 value, f32 step = 1.f, std::string_view id = {});
/// QSpinBox / QDoubleSpinBox parity on a spinBox: typed entry (focus selects the value, typing replaces it; Backspace, Escape cancels,
/// Enter / focus out commits an in-range number clamped, anything else reverts; editingFinished), prefix / suffix,
/// decimals, wrapping, specialValueText (shown at the minimum, typing it selects the minimum), adaptive decimal
/// stepping, keyboardTracking (valueChanged per keystroke or only on commit), textFromValue / valueFromText.
/// QAbstractSpinBox::CorrectionMode: what Enter / focus out do with an out-of-range number.
enum class SpinCorrection : u8 { PreviousValue, NearestValue };
struct SpinBoxOptions {
    std::string prefix, suffix;
    SpinCorrection correction = SpinCorrection::PreviousValue; ///< Qt's default: revert to the last valid value
    i32 decimals = -1; ///< -1: from the step (0 for step >= 1)
    bool wrapping = false;
    std::string specialValueText;
    bool adaptiveStep = false; ///< QAbstractSpinBox::AdaptiveDecimalStepType
    bool keyboardTracking = true;
    bool accelerated = false; ///< QAbstractSpinBox::accelerated: holding an arrow button repeats faster and faster
    std::function<std::string(double)> textFromValue;                     ///< clean text, without prefix / suffix
    std::function<std::optional<double>(std::string_view)> valueFromText; ///< nullopt: not a value
};
void setSpinBoxOptions(UiContext& ui, NodeHandle spin, SpinBoxOptions options);
std::string spinBoxText(const UiContext& ui, NodeHandle spin);      ///< QAbstractSpinBox::text (what is shown)
std::string spinBoxCleanText(const UiContext& ui, NodeHandle spin); ///< QSpinBox::cleanText
void spinBoxStepBy(UiContext& ui, NodeHandle spin, i32 steps);      ///< QAbstractSpinBox::stepBy
/// QAbstractSpinBox::stepEnabled: false at max (up) / min (down) unless wrapping. Disabled arrows are dimmed and
/// Up / Down / PageUp / PageDown / wheel / arrow clicks in that direction do nothing; stepBy still steps.
bool spinBoxStepEnabled(const UiContext& ui, NodeHandle spin, bool up);
/// Selection / caret in the editable text (byte offsets, without prefix / suffix). Focus selects all (as Qt);
/// Left / Right / Home / End (Shift extends), Ctrl+A, click / drag, Ctrl+C / X / V; typing replaces the selection.
/// Holding an arrow button repeats after 0.5 s every 0.15 s (Qt's style defaults), faster when accelerated.
void spinBoxSelection(const UiContext& ui, NodeHandle spin, u32& from, u32& to, u32& caret);
void setSpinBoxSelection(UiContext& ui, NodeHandle spin, u32 from, u32 to);
/// Items separated by '\n'; value() is the selected index. (Tier A: cycles in place; the popup list is Tier B.)
NodeHandle comboBox(UiContext& ui, NodeHandle parent, std::string_view items, u32 selected = 0, std::string_view id = {});
NodeHandle lineEdit(UiContext& ui, NodeHandle parent, std::string_view text = {}, std::string_view id = {});

// ---- QLineEdit parity: echo modes, max length, placeholder, read-only, validators -----------------------------------
/// QLineEdit::EchoMode. Password / NoEcho fields never hold the secret in the node text (UiContext::text, the a11y
/// tree, inspectors): it holds the mask, the real text is lineEditText(). Copy / cut are disabled while masked.
enum class EchoMode : u8 { Normal, NoEcho, Password, PasswordEchoOnEdit };
/// QValidator::State. Invalid edits are rejected (the buffer is left as it was); Submitted (Enter) is emitted only
/// for Acceptable text, as QLineEdit::returnPressed.
enum class Validation : u8 { Invalid, Intermediate, Acceptable };
using Validator = std::function<Validation(std::string_view text)>;
/// QValidator::fixup: on Enter / focus loss with non-Acceptable text, may rewrite it (e.g. clamp into range).
using Fixup = std::function<void(std::string& text)>;
struct LineEditOptions {
    EchoMode echo = EchoMode::Normal;
    u32 maxLength = 32767;   ///< in code points (Qt: QChars); typing / paste is truncated to fit
    std::string placeholder; ///< drawn dimmed while the field is empty
    bool readOnly = false;   ///< caret, selection and copy still work
    Validator validator;     ///< null: everything Acceptable
    Fixup fixup;             ///< null: none
    /// QLineEdit::inputMask: A a (letter) N n (letter / digit) X x (any) 9 0 (digit) D d (1-9) # (digit / sign)
    /// H h (hex) B b (binary); upper case = required. > < ! switch case conversion, \ escapes a literal, a trailing
    /// ";c" sets the blank character (default space). Every other character is a literal. Typing fills the next
    /// editable cell, a literal typed at a literal steps over it; text is Acceptable once every required cell is
    /// filled (and the validator agrees). lineEditText() = literals + filled cells (blanks removed), as Qt's text().
    std::string inputMask;
    char32_t maskChar = U'•';
};
void setLineEditOptions(UiContext& ui, NodeHandle edit, LineEditOptions options);
/// Real text of a line edit (also for masked fields), and its validator state.
std::string_view lineEditText(UiContext& ui, NodeHandle edit);
Validation lineEditState(UiContext& ui, NodeHandle edit);
/// Replace the text programmatically (not validated, truncated to maxLength; emits TextChanged). Caret to the end.
/// With an input mask the characters are typed into the cells (non-matching ones are dropped).
void setLineEditText(UiContext& ui, NodeHandle edit, std::string_view text);
/// QIntValidator / QDoubleValidator / QRegularExpressionValidator. Number validators: a partial number ("", "-",
/// "1." or a value outside the range that more digits could still fix) is Intermediate. regexValidator: a full match
/// is Acceptable, a prefix of some match is Intermediate, anything else Invalid (so non-matching keystrokes are
/// rejected, as QRegularExpressionValidator). Partial matching runs on a small Thompson NFA over code points: literals,
/// . [classes] (ranges, negation, \d \w \s), \d \D \w \W \s \S, groups (?:), |, * + ? {m} {m,} {m,n} (lazy forms
/// accepted), ^ $ at the ends. Other syntax (backreferences, lookaround) falls back to std::regex full matching, with
/// non-matches Intermediate.
Validator intValidator(long long lo, long long hi);
Validator doubleValidator(double lo, double hi, int decimals = 1000);
Validator regexValidator(std::string_view ecmaPattern);
/// Fixups: clamp into [lo, hi] (non-numbers become the bound nearest 0); round to `decimals`.
Fixup intFixup(long long lo, long long hi);
Fixup doubleFixup(double lo, double hi, int decimals);
/// Partial regex match on its own (Invalid / Intermediate / Acceptable); false in `supported` when the pattern
/// uses syntax the matcher does not have.
Validation regexPartialMatch(std::string_view ecmaPattern, std::string_view text, bool* supported = nullptr);

NodeHandle progressBar(UiContext& ui, NodeHandle parent, f32 value01, std::string_view id = {});
NodeHandle dial(UiContext& ui, NodeHandle parent, f32 lo, f32 hi, f32 value, std::string_view id = {});
/// `imageIndex` is the CpuRasterizer / renderer image index (stored as user data).
NodeHandle image(UiContext& ui, NodeHandle parent, u32 imageIndex, f32 w, f32 h, std::string_view id = {});
NodeHandle separator(UiContext& ui, NodeHandle parent, std::string_view id = {});
NodeHandle link(UiContext& ui, NodeHandle parent, std::string_view text, std::string_view id = {});
NodeHandle panel(UiContext& ui, NodeHandle parent, std::string_view id = {});

/// Selected item text of a combo box.
std::string_view comboText(const UiContext& ui, NodeHandle combo);

// ---- QCompleter parity -----------------------------------------------------------------------------------------------
struct ItemModel;
/// QCompleter::CompletionMode / Qt::MatchFlags filter mode.
enum class CompletionMode : u8 { Popup, Inline, UnfilteredPopup };
enum class MatchMode : u8 { StartsWith, Contains, EndsWith };
/// QCompleter::ModelSorting. A sorted model (by code point, after case folding for CaseInsensitivelySorted) whose
/// sorting matches `caseSensitive` is searched by binary search for StartsWith / Inline matching: O(log n + matches)
/// model reads instead of a full scan.
enum class ModelSorting : u8 { Unsorted, CaseSensitivelySorted, CaseInsensitivelySorted };
struct CompleterOptions {
    const ItemModel* model = nullptr; ///< not owned; must outlive the edit (call completerModelChanged on change)
    u32 column = 0;                   ///< QCompleter::completionColumn
    std::string items;                ///< '\n'-separated list, used when `model` is null (QStringListModel)
    CompletionMode mode = CompletionMode::Popup;
    bool caseSensitive = false;       ///< Qt default: Qt::CaseSensitive; we default to the common insensitive case
    MatchMode filter = MatchMode::StartsWith; ///< Inline mode always matches StartsWith (as Qt)
    u32 maxVisibleItems = 7;
    bool wrapAround = true;
    ModelSorting sorting = ModelSorting::Unsorted;
    /// Optional per-row icon (rasteriser image index, kNoIcon for none), painted before the row text in the popup.
    std::function<u32(u32 row)> icon;
};
inline constexpr u32 kNoIcon = 0xFFFFFFFFu;
/// Attaches a completer to a line edit (an empty model and item list removes it). The prefix is the whole text, as
/// QLineEdit: typing (not programmatic setLineEditText) refilters. Popup: a list under the field (in the overlay layer,
/// keyboard focus stays in the field); Down / Up / PageDown / PageUp / Home+Ctrl / End+Ctrl move the highlight and
/// put the completion in the field (Highlighted signal on the edit), stepping past either end returns to the typed
/// prefix; Enter / Tab / click choose (Activated, then Enter still submits as returnPressed); Escape closes and restores
/// the prefix. Inline: the first match is appended selected after the caret (not after Backspace / Delete);
/// Up / Down cycle the matches. UnfilteredPopup: every row is listed, the best match highlighted.
/// A11y: the field gets autoComplete (inline / list / both), expanded, controls -> the list and activeDescendant ->
/// the highlighted option; the list is a List of ListItem options with `selected` on the highlighted one.
void setCompleter(UiContext& ui, NodeHandle edit, CompleterOptions options);
/// Recompute matches after the model changed (keeps the popup open if it still has rows).
void completerModelChanged(UiContext& ui, NodeHandle edit);
/// QCompleter::complete(): shows the popup for the current text (all rows when the text is empty).
void complete(UiContext& ui, NodeHandle edit);
/// QCompleter::setCompletionPrefix / completionPrefix: filter for `prefix` without touching the field's text (the
/// popup, when open, follows). Typing in the field sets the prefix to the whole text again.
void setCompletionPrefix(UiContext& ui, NodeHandle edit, std::string_view prefix);
std::string completionPrefix(UiContext& ui, NodeHandle edit);
/// Number of current completions (QCompleter::completionCount) and the text of one.
u32 completionCount(UiContext& ui, NodeHandle edit);
std::string completionAt(UiContext& ui, NodeHandle edit, u32 index);
/// Highlighted row in the completion list (-1 none), QCompleter::currentRow.
i32 completerCurrentRow(UiContext& ui, NodeHandle edit);
bool completerPopupVisible(const UiContext& ui, NodeHandle edit);
/// The popup list node (invalid until first shown).
NodeHandle completerPopup(const UiContext& ui, NodeHandle edit);

/// Editable combo box (QComboBox::setEditable(true)): a line edit with an inline, case-insensitive completer over the
/// items and a drop button. value() = current index (-1 none), text = items ('\n'). Up / Down / PageUp / PageDown
/// step the current item, Alt+Down / Alt+Up / F4 / the button open the full list (current row highlighted). Enter
/// selects the item whose text matches exactly, else inserts the text per the policy (QComboBox::InsertPolicy) and
/// makes it current. Changes emit ValueChanged {index} and TextChanged {item} on the combo. A11y: ComboBox with
/// expanded / controls, holding the TextField.
enum class InsertPolicy : u8 { NoInsert, InsertAtTop, InsertAtBottom, InsertAlphabetically, InsertAtCurrent,
                               InsertAfterCurrent, InsertBeforeCurrent };
NodeHandle editableComboBox(UiContext& ui, NodeHandle parent, std::string_view items, i32 selected = 0,
                            InsertPolicy policy = InsertPolicy::InsertAtBottom, std::string_view id = {});
NodeHandle comboLineEdit(const UiContext& ui, NodeHandle combo);
i32 comboCurrentIndex(const UiContext& ui, NodeHandle combo);
void setComboCurrentIndex(UiContext& ui, NodeHandle combo, i32 index); ///< -1 clears the field
/// QComboBox::findText (exact; case-insensitive when !caseSensitive). -1 when absent.
i32 comboFindText(const UiContext& ui, NodeHandle combo, std::string_view text, bool caseSensitive = true);
void comboShowList(UiContext& ui, NodeHandle combo); ///< QComboBox::showPopup
/// QComboBox::setInsertPolicy / setMaxCount / setDuplicatesEnabled / setValidator. Insertion is refused once the
/// combo holds maxCount items (InsertAtCurrent still replaces). With duplicates enabled Enter inserts even when an
/// item matches. A validator gates Enter (only Acceptable text selects / inserts) and may fixup.
struct EditableComboOptions {
    InsertPolicy policy = InsertPolicy::InsertAtBottom;
    u32 maxCount = 0x7FFFFFFFu;
    bool duplicatesEnabled = false;
    /// Enter matches an item ignoring case (Qt's default Qt::MatchFixedString) and then shows the item's own text.
    bool caseSensitive = false;
    Validator validator; ///< null: everything Acceptable
    Fixup fixup;
};
void setComboOptions(UiContext& ui, NodeHandle combo, EditableComboOptions options);
void setComboValidator(UiContext& ui, NodeHandle combo, Validator validator, Fixup fixup = {});
/// Items from a model column (QComboBox::setModel / setModelColumn); the model is not owned and is read-only here,
/// so Enter only selects (no insertion). Call comboModelChanged after its rows change. nullptr returns to own items.
void setComboModel(UiContext& ui, NodeHandle combo, const ItemModel* model, u32 column = 0);
void comboModelChanged(UiContext& ui, NodeHandle combo);
u32 comboCount(const UiContext& ui, NodeHandle combo);
/// Per-item icon (rasteriser image index; kNoIcon none) and user data (QComboBox::setItemIcon / setItemData /
/// findData). Icons show in the list and before the field for the current item. They follow their items through
/// insertion (any policy), comboInsertItem / comboRemoveItem; with a model they are per model row.
void setComboItemIcon(UiContext& ui, NodeHandle combo, u32 index, u32 image);
u32 comboItemIcon(const UiContext& ui, NodeHandle combo, u32 index);
void setComboItemData(UiContext& ui, NodeHandle combo, u32 index, u64 data);
u64 comboItemData(const UiContext& ui, NodeHandle combo, u32 index);
i32 comboFindData(const UiContext& ui, NodeHandle combo, u64 data);
/// QComboBox::insertItem / addItem / removeItem (own items only; ignored with a model).
void comboInsertItem(UiContext& ui, NodeHandle combo, u32 index, std::string_view text, u32 image = kNoIcon, u64 data = 0);
void comboAddItem(UiContext& ui, NodeHandle combo, std::string_view text, u32 image = kNoIcon, u64 data = 0);
void comboRemoveItem(UiContext& ui, NodeHandle combo, u32 index);
std::string comboItemText(const UiContext& ui, NodeHandle combo, u32 index);

// ---- Tier B (plan §5) ------------------------------------------------------------------------------------------------
// Popups, menus, dialogs and tooltips live in the overlay layer: a core "layer" node kept as the root's last child,
// laid out over the viewport and outside the flow. A popup is a viewport-sized scope node (the scrim) holding the
// panel; outside clicks and Escape close it, focus is trapped in it (UiContext::setModal) and returns to the owner.

/// The overlay layer (created on first use; moved back to the root's last child on every call).
NodeHandle overlayLayer(UiContext& ui);

/// Opens a popup menu at `at` (context coordinates). Items separated by '\n'; "-" is a separator. Picking item i
/// emits Submitted {value = i, text} on the menu scope and on `owner` (when alive), then closes. Returns the scope.
NodeHandle popupMenu(UiContext& ui, NodeHandle owner, std::string_view items, Vec2 at, std::string_view id = {});
/// Closes a popup scope (or the scope containing `n`): releases the modal trap, refocuses the owner, destroys it on
/// the next update (dialogs are hidden instead and can be reopened).
void closePopup(UiContext& ui, NodeHandle n);
/// True while a popup scope is open.
bool popupOpen(const UiContext& ui, NodeHandle scope);

/// QToolBar: a row (or column) of tool buttons / any widgets. Items that do not fit go into the extension button's
/// menu ("»", QToolBarExtension), in order; picking one emits Clicked on that item. Separators at the cut are hidden.
NodeHandle toolBar(UiContext& ui, NodeHandle parent, bool vertical = false, std::string_view id = {});
NodeHandle toolBarSeparator(UiContext& ui, NodeHandle bar);
NodeHandle toolBarExtension(const UiContext& ui, NodeHandle bar);           ///< the "»" button (hidden while all fit)
std::vector<NodeHandle> toolBarOverflow(const UiContext& ui, NodeHandle bar); ///< items now in the extension menu
/// QMainWindow tool bar areas: a row (or column) holding tool bars. A movable bar (setToolBarMovable: a grip as its
/// first item) dragged by the grip moves live between areas, before the bar under the pointer, taking the area's
/// orientation; outside every area a floatable bar floats in the overlay layer. Changes emit SignalKind::Event
/// {text "topLevelChanged", flag = floating} and {text "orientationChanged", flag = vertical}.
NodeHandle toolBarArea(UiContext& ui, NodeHandle parent, bool vertical = false, std::string_view id = {});
void setToolBarMovable(UiContext& ui, NodeHandle bar, bool on);
void setToolBarFloatable(UiContext& ui, NodeHandle bar, bool on); ///< default on
void setToolBarOrientation(UiContext& ui, NodeHandle bar, bool vertical);
bool toolBarIsVertical(const UiContext& ui, NodeHandle bar);
void toolBarAddTo(UiContext& ui, NodeHandle area, NodeHandle bar, NodeHandle before = {}); ///< QMainWindow::insertToolBar
void toolBarFloat(UiContext& ui, NodeHandle bar, Vec2 at);
bool toolBarIsFloating(const UiContext& ui, NodeHandle bar);

/// A horizontal menu bar; menuBarMenu adds a title button whose popup lists `items` (Submitted re-emitted on it).
NodeHandle menuBar(UiContext& ui, NodeHandle parent, std::string_view id = {});
NodeHandle menuBarMenu(UiContext& ui, NodeHandle bar, std::string_view title, std::string_view items, std::string_view id = {});

/// Modal dialog: a hidden scope in the overlay layer with a centred card (title + body). Add widgets to dialogBody.
NodeHandle dialog(UiContext& ui, std::string_view title, std::string_view id = {});
NodeHandle dialogBody(const UiContext& ui, NodeHandle dlg);
void openDialog(UiContext& ui, NodeHandle dlg);
void closeDialog(UiContext& ui, NodeHandle dlg);
/// QDialog keys: Enter presses the default button (class "dialog-default") unless a push button has focus; Escape
/// presses the reject button (class "dialog-reject"), else closes. A dialog button box sets both automatically.
void setDialogDefaultButton(UiContext& ui, NodeHandle dlg, NodeHandle button);
void setDialogRejectButton(UiContext& ui, NodeHandle dlg, NodeHandle button);

/// Shows `text` under `node` after `delay` seconds of hover; hides on leave. Returns the tooltip node.
NodeHandle setTooltip(UiContext& ui, NodeHandle node, std::string_view text, f32 delay = 0.5f);

/// Virtualised single-selection list (items '\n'-separated; value() = selected index, -1 = none). Only visible rows
/// are painted. Selecting emits ValueChanged and TextChanged {item}; Enter emits Submitted.
NodeHandle listView(UiContext& ui, NodeHandle parent, std::string_view items, i32 selected = -1, std::string_view id = {});
std::string_view listItem(const UiContext& ui, NodeHandle list, u32 index);
u32 listFirstVisible(const UiContext& ui, NodeHandle list);
/// QListView::ViewMode. Icon: items flow left to right in grid cells (setListGridSize, default 96 x 80) with the
/// icon (setListIconSize, default 48) centred over the label; userData then counts rows of cells. Arrows move in 2D
/// (Up / Down by a row of cells), PageUp / PageDown by a screen. List mode draws the icon 16 px before the text.
/// Both modes expose ListItem a11y parts (part = index + 1). Icons are rasteriser image indices (kNoIcon none).
enum class ListViewMode : u8 { List, Icon };
void setListViewMode(UiContext& ui, NodeHandle list, ListViewMode mode);
ListViewMode listViewMode(const UiContext& ui, NodeHandle list);
void setListGridSize(UiContext& ui, NodeHandle list, Size grid);
Size listGridSize(const UiContext& ui, NodeHandle list);
void setListIconSize(UiContext& ui, NodeHandle list, f32 px);
void setListItemIcon(UiContext& ui, NodeHandle list, u32 index, u32 image);
u32 listItemIcon(const UiContext& ui, NodeHandle list, u32 index);
Rect listItemRect(UiContext& ui, NodeHandle list, u32 index); ///< context space, scrolled
i32 listItemAt(UiContext& ui, NodeHandle list, Vec2 p);       ///< -1 none

/// Tree view: one line per node, leading tabs give the depth. value() = selected line. Rows 0..63 can collapse
/// (expansion state is a 64-bit mask; deeper rows stay expanded). Toggling emits Toggled {value = line, flag = open}.
NodeHandle treeView(UiContext& ui, NodeHandle parent, std::string_view lines, std::string_view id = {});
bool treeExpanded(const UiContext& ui, NodeHandle tree, u32 line);
void setTreeExpanded(UiContext& ui, NodeHandle tree, u32 line, bool open);
/// Number of rows currently shown (collapsed subtrees skipped).
u32 treeVisibleRows(const UiContext& ui, NodeHandle tree);

/// QAbstractItemView::SelectionMode / SelectionBehavior for the tree and table views. Single (default): the current item
/// (value()) is the selection. Multi: a click or Space toggles. Extended: click selects one, Ctrl+click toggles,
/// Shift+click / Shift+arrows select the range from the anchor, Ctrl+arrows move the current item only, Ctrl+Space toggles,
/// Ctrl+A selects all. Contiguous: like Extended without Ctrl toggling. None: nothing is selected. Behaviour (table
/// only; the tree is always Rows): Items selects cells, Rows whole rows, Columns whole columns. Changes emit
/// SignalKind::SelectionChanged. In Extended / Multi a press on an already selected item keeps the selection until
/// release, so several items can be dragged.
enum class SelectionMode : u8 { Single, Multi, Extended, Contiguous, None };
enum class SelectionBehavior : u8 { Items, Rows, Columns };
void setItemSelectionMode(UiContext& ui, NodeHandle view, SelectionMode m);
void setItemSelectionBehavior(UiContext& ui, NodeHandle view, SelectionBehavior b);
SelectionMode itemSelectionMode(const UiContext& ui, NodeHandle view);
/// Rows (tree lines) whose every cell is selected, ascending.
std::vector<u32> selectedRows(const UiContext& ui, NodeHandle view);
/// Selected cells as (row, column), row-major (tree: column 0).
std::vector<std::pair<u32, u32>> selectedItems(const UiContext& ui, NodeHandle view);
bool itemSelected(const UiContext& ui, NodeHandle view, u32 row, u32 col = 0);
/// Selects / deselects a row (Rows behaviour, also for Items: all its cells); Single mode makes it current.
void setRowSelected(UiContext& ui, NodeHandle view, u32 row, bool on);
void clearItemSelection(UiContext& ui, NodeHandle view);
void selectAllItems(UiContext& ui, NodeHandle view);
/// Table current column (Items / Columns behaviour; Left / Right move it).
u32 tableCurrentColumn(const UiContext& ui, NodeHandle table);
/// Tree scrolling: first shown row (wheel, keys keep the current row visible, scrollbar drag).
u32 treeFirstVisible(const UiContext& ui, NodeHandle tree);
void setTreeFirstVisible(UiContext& ui, NodeHandle tree, u32 row);

/// QAbstractItemView drag and drop for the tree view. `drag`: rows drag out as "text/plain" (the line text) and
/// "application/x-fuse-tree-line" (the line index). `internalMove` (DragDropMode::InternalMove): dropping a row in the same
/// tree moves it with its subtree above / below / onto (as last child of) the row under the pointer, shown by a drop
/// indicator; a row cannot go into its own subtree. After a move the moved row is selected and `moved(from, to)` gets
/// the old and new line index.
/// `acceptDrops` (DragDropMode::DragDrop): the tree also takes drops from other trees ("application/x-fuse-tree-subtree":
/// the dragged rows with their subtrees, depth-relative) or plain text (one row per line, tabs = depth), inserted
/// above / below / onto the row under the pointer (or appended below the last row) with the proposed action (Ctrl
/// copies); `inserted(at, count)` reports the new head lines. A drag that ends with Move in another view removes the
/// dragged rows from the source tree (`removed(line)` per removed head, old index, descending).
struct TreeDragDrop {
    bool drag = false, internalMove = false;
    bool acceptDrops = false;
    bool autoScroll = true;      ///< QAbstractItemView::autoScroll: hovering within 16 px of the top / bottom edge scrolls
    f32 autoExpandDelay = -1.f;  ///< QTreeView::autoExpandDelay (s): hovering onto a collapsed row expands it; < 0 off
    std::function<void(UiContext&, NodeHandle tree, u32 from, u32 to)> moved;
    std::function<void(UiContext&, NodeHandle tree, u32 at, u32 count)> inserted;
    std::function<void(UiContext&, NodeHandle tree, u32 line)> removed;
};
void setTreeDragDrop(UiContext& ui, NodeHandle tree, TreeDragDrop opts);

/// Tab bar (titles '\n'-separated; value() = current tab) and a tab widget (tab bar + one page per title; only the
/// current page is displayed).
NodeHandle tabBar(UiContext& ui, NodeHandle parent, std::string_view titles, u32 current = 0, std::string_view id = {});
NodeHandle tabWidget(UiContext& ui, NodeHandle parent, std::string_view titles, std::string_view id = {});
NodeHandle tabPage(const UiContext& ui, NodeHandle tabs, u32 index);
/// QTabBar parity. Closable tabs draw a close box; clicking it emits SignalKind::Event text "tabCloseRequested",
/// value = index (the app removes the tab, e.g. tabWidgetRemovePage). Movable tabs reorder live while dragged
/// (start distance 10 px) and emit Event text "tabMoved <from> <to>", value = to; in a tab widget the pages
/// follow. Disabled tabs are greyed and skipped by click, arrows, Home / End and the wheel (which steps the
/// current tab). Tabs that overflow scroll, with two scroll buttons at the far end; the current tab is kept visible.
/// Accessibility: the bar is a TabList whose tabs are virtual Tab parts (part = index + 1, selected, disabled).
void setTabsClosable(UiContext& ui, NodeHandle bar, bool on);
bool tabsClosable(const UiContext& ui, NodeHandle bar);
void setTabsMovable(UiContext& ui, NodeHandle bar, bool on);
bool tabsMovable(const UiContext& ui, NodeHandle bar);
void setTabEnabled(UiContext& ui, NodeHandle bar, u32 index, bool on);
bool tabEnabled(const UiContext& ui, NodeHandle bar, u32 index);
void tabBarMoveTab(UiContext& ui, NodeHandle bar, u32 from, u32 to);
Rect tabBarTabRect(UiContext& ui, NodeHandle bar, u32 index);
f32 tabBarScroll(const UiContext& ui, NodeHandle bar);
/// QTabWidget::addTab / removeTab: appends a page (returned) / removes page `index`, keeping the current sane.
NodeHandle tabWidgetAddPage(UiContext& ui, NodeHandle tabs, std::string_view title);
void tabWidgetRemovePage(UiContext& ui, NodeHandle tabs, u32 index);
NodeHandle tabWidgetBar(const UiContext& ui, NodeHandle tabs);
/// QTabWidget::TabPosition / QTabBar::Shape. West / East stack the tabs vertically (text stays horizontal) with
/// the selection indicator on the edge facing the page; arrows Up / Down also step. On a tab widget this also
/// places the bar on that side of the pages.
enum class TabPosition : u8 { North, South, West, East };
void setTabPosition(UiContext& ui, NodeHandle barOrTabs, TabPosition pos);
TabPosition tabPosition(const UiContext& ui, NodeHandle barOrTabs);
/// QTabBar::elideMode (default None: titles clip). Elision only clips and draws an ellipsis shape; it does not
/// re-measure text. setTabUsesScrollButtons(false): an overflowing horizontal bar shrinks its tabs to fit instead.
enum class ElideMode : u8 { None, Left, Right, Middle };
void setTabElideMode(UiContext& ui, NodeHandle bar, ElideMode mode);
ElideMode tabElideMode(const UiContext& ui, NodeHandle bar);
void setTabUsesScrollButtons(UiContext& ui, NodeHandle bar, bool on);
bool tabUsesScrollButtons(const UiContext& ui, NodeHandle bar);
/// QTabBar::setTabIcon (rasteriser image index, kNoIcon none; 16 px before the title) / setTabToolTip (shown after
/// hovering the tab for 0.5 s; tabBarToolTip is its overlay node, invalid until first shown). Both follow moves.
void setTabIcon(UiContext& ui, NodeHandle bar, u32 index, u32 image);
u32 tabIcon(const UiContext& ui, NodeHandle bar, u32 index);
void setTabToolTip(UiContext& ui, NodeHandle bar, u32 index, std::string_view text);
std::string_view tabToolTip(const UiContext& ui, NodeHandle bar, u32 index);
NodeHandle tabBarToolTip(const UiContext& ui, NodeHandle bar);

/// Scroll area: a column panel with overflow: scroll (wheel scrolls; role ScrollArea).
NodeHandle scrollArea(UiContext& ui, NodeHandle parent, std::string_view id = {});

// ---- Tier B: splitters, docks, item models -----------------------------------------------------------------------
/// Splitter: panes separated by draggable handles (Row = side by side, Column = stacked). A pane with size > 0 keeps
/// that many px (dragging resizes it); size 0 panes share the rest. Dragging emits ValueChanged {px} on the splitter.
NodeHandle splitter(UiContext& ui, NodeHandle parent, bool stacked = false, std::string_view id = {});
NodeHandle splitterPane(UiContext& ui, NodeHandle split, f32 size = 0.f, std::string_view id = {});
/// QSplitter::sizes / setSizes: main-axis px of each pane. setSplitterSizes: >= 0 fixes the pane at that size,
/// < 0 makes it share the rest again.
std::vector<f32> splitterSizes(const UiContext& ui, NodeHandle split);
void setSplitterSizes(UiContext& ui, NodeHandle split, std::span<const f32> sizes);
/// QSplitter::setChildrenCollapsible: dragging a handle past half the minimum pane size collapses that pane to 0.
void setSplitterCollapsible(UiContext& ui, NodeHandle split, bool on);

/// Dock area: left / right / bottom regions around a centre, each resizable by splitter handles and hidden while
/// empty. Dock widgets are titled panels with a close button; several in one region stack.
enum class DockSide : u8 { Left, Right, Bottom, Center };
NodeHandle dockArea(UiContext& ui, NodeHandle parent, std::string_view id = {});
NodeHandle dockRegion(const UiContext& ui, NodeHandle area, DockSide side);
NodeHandle dockWidget(UiContext& ui, NodeHandle area, std::string_view title, DockSide side, std::string_view id = {});
NodeHandle dockBody(const UiContext& ui, NodeHandle dock);
void dockTo(UiContext& ui, NodeHandle dock, DockSide side);
void dockClose(UiContext& ui, NodeHandle dock); ///< hides (dockTo re-shows); emits Toggled {flag = false}
/// QDockWidget floating: the dock moves to the overlay layer at `at` (context coordinates; w / h <= 0 keep its size).
/// Dragging a title past 10 px floats it under the pointer; releasing over a docked dock's title (or a tab group's
/// bar) tabs it there, over the area's 48 px left / right / bottom band docks it on that side, else it stays
/// floating. dockTo re-docks. Changes emit SignalKind::Event {text "topLevelChanged", flag = floating}.
void dockFloat(UiContext& ui, NodeHandle dock, Rect at);
/// While a dock title is dragged: the drop-zone rect it would fill if released now (the target dock / tab group,
/// the side region, or a quarter strip of the area); false when the drop would leave it floating. A double-click on a
/// title floats a docked dock and re-docks a floating one (QDockWidget).
bool dockDropHighlight(const UiContext& ui, NodeHandle dock, Rect& out);
bool dockIsFloating(const UiContext& ui, NodeHandle dock);
Rect dockFloatGeometry(const UiContext& ui, NodeHandle dock);
void setDockMovable(UiContext& ui, NodeHandle dock, bool on); ///< QDockWidget::DockWidgetMovable (default on)
/// QMainWindow::tabifyDockWidget: `second` joins `first`'s tab group (made on demand, where `first` is) and becomes
/// current. A group shows a tab bar of its open members; the last member left dissolves it.
void dockTabify(UiContext& ui, NodeHandle first, NodeHandle second);
void dockRaise(UiContext& ui, NodeHandle dock); ///< makes a tabbed dock current (QDockWidget::raise); Event "raised"
std::vector<NodeHandle> dockTabs(const UiContext& ui, NodeHandle dock); ///< its group's members (empty if not tabbed)
NodeHandle dockCurrentTab(const UiContext& ui, NodeHandle dock);

/// Item model: a function table over caller data (no inheritance). `data` may return a view into `scratch`.
struct ItemModel {
    const void* user = nullptr;
    u32 columns = 1;
    u32 (*rows)(const void* user) = nullptr;
    std::string_view (*data)(const void* user, u32 row, u32 col, std::string& scratch) = nullptr;
    std::string_view (*header)(const void* user, u32 col) = nullptr; ///< optional
};
/// A ready-made row/column string model (owns its cells). sortBy reorders rows (stable, numeric when both parse).
class StringTableModel {
public:
    explicit StringTableModel(std::string_view headersTabSeparated);
    StringTableModel(const StringTableModel&) = delete; ///< the model table points at this object
    StringTableModel& operator=(const StringTableModel&) = delete;
    void addRow(std::string_view cellsTabSeparated);
    void clear() { m_cells.clear(); m_rows = 0; }
    void sortBy(u32 col, bool ascending);
    void moveRow(u32 from, u32 to); ///< row `from` ends up at index `to` (others shift)
    void moveRows(const std::vector<u32>& rows, u32 to); ///< see TableDragDrop::moveRows
    void insertRows(u32 at, std::string_view rowsTsv);    ///< one row per line, cells tab-separated
    void removeRows(const std::vector<u32>& rows);
    u32 rows() const { return m_rows; }
    std::string_view cell(u32 row, u32 col) const { return m_cells[std::size_t(row) * m_cols + col]; }
    const ItemModel& model() const { return m_model; }

private:
    std::vector<std::string> m_headers, m_cells;
    u32 m_cols = 1, m_rows = 0;
    ItemModel m_model;
};
/// Virtualised table view over a model (not owned; must outlive the view): header row (click sorts: Toggled
/// {value = column, flag = ascending}; drag a header edge to resize), single row selection (value() = row, -1 none;
/// ValueChanged; Enter -> Submitted), wheel / keys / scrollbar drag scroll. Only visible rows are read and painted.
NodeHandle tableView(UiContext& ui, NodeHandle parent, const ItemModel* model, std::string_view id = {});
/// QTableView drag and drop. `drag`: rows drag out as "text/plain" (cells tab-joined) and
/// "application/x-fuse-table-row" (the row indices, comma-separated). With a multi-row selection (Extended / Multi) a
/// drag from a selected row carries all selected rows. `internalMove`: dropping between rows of the same table calls
/// `moveRows(rows, to)` (the caller moves its model rows; StringTableModel::moveRows does it), with a gap indicator,
/// then re-selects the moved rows.
struct TableDragDrop {
    bool drag = false, internalMove = false;
    bool autoScroll = true; ///< hovering near the top / bottom edge scrolls (QAbstractItemView::autoScroll)
    /// Move `rows` (ascending) so that they end up contiguous, in order, starting at index `to` of the result.
    std::function<void(UiContext&, NodeHandle table, const std::vector<u32>& rows, u32 to)> moveRows;
    /// DragDrop mode: rows dragged from another view ("text/plain": one row per line, cells tab-separated) dropped in the
    /// gap before row `at`; insert them and return true. Called with the proposed action (Ctrl copies).
    bool acceptDrops = false;
    std::function<bool(UiContext&, NodeHandle table, std::string_view rowsTsv, u32 at, u8 action)> insertRows;
    /// A drag from this table that ended as a Move in another view: remove `rows` (ascending).
    std::function<void(UiContext&, NodeHandle table, const std::vector<u32>& rows)> removeRows;
};
void setTableDragDrop(UiContext& ui, NodeHandle table, TableDragDrop opts);
/// Call after the model's rows change (re-clamps the scroll, repaints).
void tableModelChanged(UiContext& ui, NodeHandle table);
u32 tableFirstVisible(const UiContext& ui, NodeHandle table);
f32 tableColumnWidth(const UiContext& ui, NodeHandle table, u32 col);
void setTableColumnWidth(UiContext& ui, NodeHandle table, u32 col, f32 px);
/// QHeaderView section moving / hiding. Columns keep their logical index (model column, sort column, curCol);
/// only the on-screen order changes. A header drag past the start-drag distance moves the section on a movable
/// header (a plain click still sorts, on release) and emits SignalKind::Event {text "sectionMoved", value logical}.
void setTableColumnsMovable(UiContext& ui, NodeHandle table, bool on);
void tableMoveColumn(UiContext& ui, NodeHandle table, u32 fromVisual, u32 toVisual);
u32 tableLogicalColumn(const UiContext& ui, NodeHandle table, u32 visual);
u32 tableVisualColumn(const UiContext& ui, NodeHandle table, u32 logical);
void setTableColumnHidden(UiContext& ui, NodeHandle table, u32 col, bool hidden);
bool tableColumnHidden(const UiContext& ui, NodeHandle table, u32 col);
/// QHeaderView::ResizeMode per section. Interactive (default): the divider drags, double-clicking it fits the section
/// to its contents (Event "sectionResized"). Fixed: no drag. Stretch: stretch sections share what the others leave of
/// the viewport width (min 24 px). ResizeToContents: the widest of the header and the first 1000 rows.
enum class HeaderResizeMode : u8 { Interactive, Fixed, Stretch, ResizeToContents };
void setTableColumnResizeMode(UiContext& ui, NodeHandle table, u32 col, HeaderResizeMode mode);
HeaderResizeMode tableColumnResizeMode(const UiContext& ui, NodeHandle table, u32 col);
void resizeTableColumnToContents(UiContext& ui, NodeHandle table, u32 col);
/// Right-clicking the header opens a menu listing every section, checked while shown (popup class "header-menu");
/// picking one hides / shows it (Event "sectionHidden" / "sectionShown", value = column). The last shown section stays.
void setTableHeaderMenu(UiContext& ui, NodeHandle table, bool on);
bool tableHeaderMenu(const UiContext& ui, NodeHandle table);
/// While a header section is being moved: the section image under the pointer and the drop marker x (context space).
bool tableHeaderDrag(UiContext& ui, NodeHandle table, Rect& image, f32& dropX);
/// List view over column 0 of a model (shares the table machinery, no header).
NodeHandle modelListView(UiContext& ui, NodeHandle parent, const ItemModel* model, std::string_view id = {});

/// Dock layout persistence (plan §5 "saved layouts"): a small text blob with the side region sizes and, per dock
/// widget with an id, its side, order in the region and open state, tab group ("tab", "current") and float
/// geometry ("float"); version 1 blobs still restore. Restoring moves / opens / closes the named docks
/// and resizes the regions; docks the blob does not name keep their place, names with no dock are ignored. A malformed
/// blob changes nothing and returns false.
std::string dockSaveLayout(const UiContext& ui, NodeHandle area);
bool dockRestoreLayout(UiContext& ui, NodeHandle area, std::string_view layout);

// ---- Drag and drop (in-app; QDrag / QMimeData / QDropEvent) ---------------------------------------------------------
enum DropAction : u8 { DropIgnore = 0, DropCopy = 1, DropMove = 2, DropLink = 4 };
/// QMimeData: MIME type -> bytes, plus the actions the source allows (QDrag::exec supportedActions).
struct DragPayload {
    std::vector<std::pair<std::string, std::string>> formats;
    u8 actions = DropCopy | DropMove;
    /// Drag image text (QDrag::setPixmap): shown in a card under the pointer while dragging; empty = the first line of
    /// text/plain, else the first format's type.
    std::string label;
    /// Drag pixmap (QDrag::setPixmap + setHotSpot): an image index registered with the renderer (as for image()),
    /// drawn `imageW` x `imageH` with the pointer at (hotX, hotY) inside it. Replaces the label card in-app;
    /// AppWindow hands its pixels to the OS as the drag icon on handoff (Wayland icon surface). -1 = no pixmap.
    i32 imageIndex = -1;
    f32 imageW = 0.f, imageH = 0.f, hotX = 0.f, hotY = 0.f;
    std::string_view get(std::string_view mime) const;
    bool has(std::string_view mime) const;
    void set(std::string_view mime, std::string_view data);
};
enum class DropPhase : u8 { Enter, Move, Leave, Drop };
struct DropEvent {
    DropPhase phase;
    Vec2 pos;
    const DragPayload* payload;
    u8 proposed;       ///< Qt proposedAction: Ctrl copies, Shift moves, Ctrl+Shift links, else Move > Copy > Link
    NodeHandle source; ///< the drag source (QDropEvent::source; in-app only)
    u8 mods;
};
/// Enter / Move: return the action you would take here (DropIgnore refuses; must be one the source allows).
/// Drop: perform it and return the action taken. Leave: return value ignored.
using DropHandler = std::function<u8(UiContext&, NodeHandle target, const DropEvent&)>;
/// Fill the payload for a drag starting at `pressPos`; false (or no formats) cancels.
using DragStarter = std::function<bool(UiContext&, NodeHandle source, Vec2 pressPos, DragPayload&)>;
/// After the drop (or cancel: action = DropIgnore). A Move source removes its data here (QDrag::exec result).
using DragFinished = std::function<void(UiContext&, NodeHandle source, u8 action)>;
inline constexpr f32 kStartDragDistance = 10.f; ///< QApplication::startDragDistance default
/// Registers a drop target (setAcceptDrops + dragEnter / dragMove / dragLeave / dropEvent); an empty handler removes.
void setDropTarget(UiContext& ui, NodeHandle n, DropHandler fn);
/// Makes a node a drag source (press, move past kStartDragDistance, drop). Works for widgets that leave pointer events
/// unconsumed (panels, labels, images); item views use their own setTreeDragDrop / setTableDragDrop.
void setDragSource(UiContext& ui, NodeHandle n, DragStarter start, DragFinished done = {});
bool dragActive(const UiContext& ui);
NodeHandle dragTarget(const UiContext& ui);        ///< the drop target under the pointer (none if not registered)
u8 dragAction(const UiContext& ui);                ///< the action the current target accepts (DropIgnore = refused)
const DragPayload* dragPayload(const UiContext& ui);
void cancelDrag(UiContext& ui);                    ///< Escape: target gets Leave, source DragFinished(DropIgnore)
/// Drop indicator a target shows while hovered (cleared when the drag leaves / ends); widgets paint it themselves.
void setDropIndicator(UiContext& ui, NodeHandle n, Rect r);
bool dropIndicator(const UiContext& ui, NodeHandle n, Rect& r);
/// For custom widgets that consume their pointer events: forward press / move / release (move and release return true
/// while a drag runs, so skip your own drag handling then).
namespace dnd {
void press(UiContext& ui, NodeHandle src, Vec2 p, DragStarter start, DragFinished done = {});
bool move(UiContext& ui, NodeHandle src, Vec2 p, u8 mods);
bool release(UiContext& ui, NodeHandle src, Vec2 p, u8 mods);
/// From a drop handler's Move: while the pointer stays within 16 px of `area`'s top / bottom edge, calls
/// `scroll(dir = -1 / +1)` every 50 ms (QAbstractItemView::autoScroll) and re-delivers Move; stops when it returns false,
/// the pointer leaves the margin or the drag ends.
void autoScroll(UiContext& ui, NodeHandle view, Rect area, std::function<bool(UiContext&, NodeHandle, i32 dir)> scroll);
/// From a drop handler's Move: runs `fn` once the pointer has hovered `key` on `view` for `delay` seconds (auto-expand).
void hoverDelay(UiContext& ui, NodeHandle view, u64 key, f32 delay, std::function<void(UiContext&, NodeHandle)> fn);
} // namespace dnd
/// OS drag handoff (QDrag leaving the window): when an in-app drag leaves the viewport, `start` gets the payload; if it
/// returns true the drag continues as an OS drag (fuse::app installs AppWindow::beginExternalDrag here) and ends with
/// finishExternalDrag. Empty `start` = drags stay in-app.
void setExternalDragHandler(UiContext& ui, std::function<bool(UiContext&, const DragPayload&)> start);
/// The OS drag ended: `action` = what the other application did (DropIgnore if refused / cancelled).
void finishExternalDrag(UiContext& ui, u8 action);
bool externalDragActive(const UiContext& ui);
Vec2 dragPosition(const UiContext& ui);
/// The drag image node (a card in the overlay layer; none when no drag runs).
NodeHandle dragImage(const UiContext& ui);

// ---- Tier C: delegates, proxies, grid view (plan §5) ------------------------------------------------------------------
/// Cell delegate: custom painting for one table column (the view still owns selection, scrolling and the row stripe).
struct CellDelegate {
    const void* user = nullptr;
    void (*paint)(const void* user, const PaintContext& pc, DrawList& dl, Rect cell, u32 row, u32 col,
                  std::string_view text, bool selected) = nullptr;
};
/// Sets (or with nullptr clears) the delegate for `col`. The delegate must outlive the view.
void setTableDelegate(UiContext& ui, NodeHandle table, u32 col, const CellDelegate* delegate);

/// Sort / filter proxy over a source model (not owned). Identity (no row map, no memory) until a filter or sort is
/// set; filtering is a case-insensitive substring match on one column (or all, kAllColumns), ANDed with an optional
/// row predicate. Call invalidate() after the source changes.
class SortFilterProxy {
public:
    explicit SortFilterProxy(const ItemModel* source);
    SortFilterProxy(const SortFilterProxy&) = delete;
    SortFilterProxy& operator=(const SortFilterProxy&) = delete;
    static constexpr u32 kAllColumns = ~0u; ///< setFilter column: match in any column (filterKeyColumn -1)
    /// Row filter beyond the needle (QSortFilterProxyModel::filterAcceptsRow): false hides source row `row`.
    using RowFilter = bool (*)(const void* user, const ItemModel& source, u32 row);
    /// Strict-weak "a sorts before b" on the sort column's text (lessThan); replaces the numeric / string default.
    using LessThan = bool (*)(const void* user, std::string_view a, std::string_view b);
    void setFilter(u32 col, std::string_view needle); ///< empty needle = no filter
    void setRowFilter(RowFilter fn, const void* user = nullptr); ///< nullptr clears
    void setLessThan(LessThan fn, const void* user = nullptr);   ///< nullptr = default ordering
    void sortBy(i32 col, bool ascending);              ///< col < 0 = source order
    i32 sortColumn() const { return m_sortCol; }
    bool sortAscending() const { return m_asc; }
    void invalidate();
    u32 rows() const;
    u32 sourceRow(u32 row) const { return m_identity ? row : m_map[row]; }        ///< mapToSource
    i64 proxyRow(u32 sourceRow) const;                                             ///< mapFromSource; -1 = filtered out
    const ItemModel& model() const { return m_model; }

private:
    const ItemModel* m_src;
    ItemModel m_model;
    std::vector<u32> m_map;
    std::string m_needle;
    u32 m_filterCol = 0;
    i32 m_sortCol = -1;
    bool m_asc = true, m_identity = true;
    RowFilter m_rowFilter = nullptr;
    const void* m_rowFilterUser = nullptr;
    LessThan m_less = nullptr;
    const void* m_lessUser = nullptr;
    mutable std::vector<i64> m_inverse; ///< lazily built source -> proxy map
    mutable bool m_inverseValid = false;
};

/// Virtualised icon / tile grid over column 0 of a model: tiles of tileW x tileH flow left to right, rows scroll.
/// value() = selected item (-1 none); arrows move in 2D, Enter -> Submitted. Only visible tiles are read.
NodeHandle gridView(UiContext& ui, NodeHandle parent, const ItemModel* model, f32 tileW = 96.f, f32 tileH = 72.f,
                    std::string_view id = {});
u32 gridColumns(const UiContext& ui, NodeHandle grid);
u32 gridFirstVisibleRow(const UiContext& ui, NodeHandle grid);

// ---- Tier D: rich and pro widgets (plan §5) ----------------------------------------------------------------------------
/// Property grid (reflection-driven inspector): name / editor rows built from descriptors. Group rows fold the rows
/// after them (until the next group). Any editor change emits Submitted {value = row index, text = name} on the grid.
enum class PropKind : u8 { Group, Bool, Number, Text, Enum, Color };
struct PropertyDesc {
    std::string_view name;
    PropKind kind = PropKind::Text;
    f32 lo = 0.f, hi = 100.f, step = 1.f; ///< Number
    std::string_view options{};           ///< Enum: '\n'-separated
};
NodeHandle propertyGrid(UiContext& ui, NodeHandle parent, std::string_view id = {});
/// Adds a row; `value` is "true"/"false", a number, text, an enum index or "#RRGGBB[AA]". Returns the editor node.
NodeHandle propertyRow(UiContext& ui, NodeHandle grid, const PropertyDesc& desc, std::string_view value = {});
/// Current value of row `index` in the same text form propertyRow takes.
std::string propertyValue(const UiContext& ui, NodeHandle grid, u32 index);
u32 propertyCount(const UiContext& ui, NodeHandle grid);
void setPropertyGroupOpen(UiContext& ui, NodeHandle grid, u32 groupIndex, bool open);

/// HSV colour picker: saturation / value square + hue bar + alpha bar. text() = "#RRGGBBAA"; drags and arrow keys
/// emit ValueChanged {value = hue} and TextChanged {text}.
NodeHandle colorPicker(UiContext& ui, NodeHandle parent, Color initial, std::string_view id = {});
Color colorPickerColor(const UiContext& ui, NodeHandle picker);
void setColorPickerColor(UiContext& ui, NodeHandle picker, Color c);

/// Month calendar: value() = days since 1970-01-01. Arrows move by a day / week, PageUp / PageDown by a month, the
/// header arrows page months, clicking a day selects it (ValueChanged), Enter -> Submitted.
NodeHandle calendar(UiContext& ui, NodeHandle parent, int year, int month, int day, std::string_view id = {});
void calendarDate(const UiContext& ui, NodeHandle cal, int& year, int& month, int& day);
/// QCalendarWidget options: the first column's weekday (0 Monday .. 6 Sunday) and an ISO week-number column.
struct CalendarOptions { u8 firstDayOfWeek = 0; bool weekNumbers = false; };
/// Keys (QCalendarWidget): arrows by day / week, Home / End to the week's first / last day (Ctrl: the month's),
/// PageUp / PageDown by a month (Ctrl: a year), Enter activates (Submitted). A click emits the usual Clicked;
/// a page change emits TextChanged {value = year * 12 + month - 1} (currentPageChanged). The chevrons dim when the
/// page beyond is out of range. A11y value text: the selected date written out.
void setCalendarOptions(UiContext& ui, NodeHandle cal, const CalendarOptions& o);
void calendarShownPage(const UiContext& ui, NodeHandle cal, int& year, int& month);
void setCalendarShownPage(UiContext& ui, NodeHandle cal, int year, int month);
int calendarIsoWeek(int year, int month, int day);
/// QCalendarWidget::setDateRange: days outside are greyed and cannot be picked; keys stop at the ends.
void setCalendarRange(UiContext& ui, NodeHandle cal, int minYear, int minMonth, int minDay, int maxYear, int maxMonth, int maxDay);
i32 daysFromCivil(int y, int m, int d);
void civilFromDays(i32 z, int& y, int& m, int& d);

/// Chart: line, bar, area or scatter series over a shared x index (auto-scaled y axis with grid lines), or pie / donut
/// slices from series 0 (slice labels = X axis categories). Hovering sets value() to the nearest x index / the slice
/// under the pointer (-1 outside) and shows a tooltip; clicking a legend entry toggles it (Toggled: value = entry,
/// flag = now visible). On x/y charts the wheel zooms the x axis around the pointer, dragging pans a zoomed view,
/// Left/Right pan and Home/Escape reset.
enum class ChartKind : u8 { Line, Bar, Area, Scatter, Pie, Donut };
enum class ChartAxisId : u8 { X, Y };
struct ChartAxis {
    std::string title;                   ///< X: under the axis; Y: above the axis; Donut: centre label (Y title)
    f32 min = std::numeric_limits<f32>::quiet_NaN(), max = std::numeric_limits<f32>::quiet_NaN(); ///< Y bounds, NaN = auto
    u32 ticks = 4;                       ///< Y: target number of intervals
    std::string format = "%.4g";         ///< printf with one floating conversion, for numeric labels and tooltip values
    std::vector<std::string> categories; ///< X: label per index (also pie slice names)
    bool grid = true;
    bool visible = true;                 ///< labels shown
};
enum class ChartLegend : u8 { None, Top, Bottom, Right };
NodeHandle chart(UiContext& ui, NodeHandle parent, ChartKind kind, std::string_view id = {});
void chartSetSeries(UiContext& ui, NodeHandle chart, u32 index, std::string_view name, std::span<const f32> values, Color color);
u32 chartSeriesCount(const UiContext& ui, NodeHandle chart);
void chartSetAxis(UiContext& ui, NodeHandle chart, ChartAxisId axis, const ChartAxis& a);
ChartAxis chartAxis(const UiContext& ui, NodeHandle chart, ChartAxisId axis);
void chartSetLegend(UiContext& ui, NodeHandle chart, ChartLegend where);
/// Legend entries: series (x/y kinds) or slices (pie / donut).
void chartSetEntryVisible(UiContext& ui, NodeHandle chart, u32 index, bool visible);
bool chartEntryVisible(const UiContext& ui, NodeHandle chart, u32 index);
void chartSetSliceColors(UiContext& ui, NodeHandle chart, std::span<const Color> colors);
/// Visible x range in index units (zoom); a range covering everything resets.
void chartSetXView(UiContext& ui, NodeHandle chart, f32 from, f32 to);
void chartXView(const UiContext& ui, NodeHandle chart, f32& from, f32& to);
void chartResetZoom(UiContext& ui, NodeHandle chart);
/// Lines of the tooltip currently shown (empty when none).
std::vector<std::string> chartTooltip(const UiContext& ui, NodeHandle chart);
Rect chartPlotRect(const UiContext& ui, NodeHandle chart);
std::vector<Rect> chartLegendRects(const UiContext& ui, NodeHandle chart);

/// Markdown view: headings (#, ##, ###), paragraphs, '-' / '*' bullets, numbered items, ``` code blocks and '>' quotes
/// become styled label children (classes md-h1..md-h3, md-p, md-li, md-code, md-quote). Inline ** and ` are stripped.
NodeHandle markdownView(UiContext& ui, NodeHandle parent, std::string_view markdown, std::string_view id = {});
void setMarkdown(UiContext& ui, NodeHandle view, std::string_view markdown);

/// Registers the Tier C / D classes (gridview, colorpicker, calendar, chart); install() calls it.
void registerTierCD(UiContext& ui);

// ---- Tier D (2): pro widgets (plan §5) ---------------------------------------------------------------------------------
/// Font picker: family list + size spin box + live preview line. Families default to the bundled faces when empty
/// ('\n'-separated otherwise). Any change emits Submitted {text = family, value = size} on the picker.
NodeHandle fontPicker(UiContext& ui, NodeHandle parent, std::string_view families = {}, f32 size = 14.f, std::string_view id = {});
std::string fontPickerFamily(const UiContext& ui, NodeHandle picker);
f32 fontPickerSize(const UiContext& ui, NodeHandle picker);

/// File dialog (modal, built on dialog()): path field, "Up" button, directory listing (folders first, then files
/// matching `filter`, e.g. "*.fuseui;*.png", empty = all), file-name field, OK / Cancel. Activating a folder enters
/// it; OK (or Enter / double activation on a file) emits Submitted {text = chosen path} on the dialog and closes it;
/// Cancel emits Submitted {text = ""}. `save` mode accepts names that do not exist yet.
NodeHandle fileDialog(UiContext& ui, std::string_view title, std::string_view directory, std::string_view filter = {},
                      bool save = false, std::string_view id = {});
bool fileDialogNavigate(UiContext& ui, NodeHandle dlg, std::string_view directory);
std::string fileDialogDirectory(const UiContext& ui, NodeHandle dlg);
u32 fileDialogEntryCount(const UiContext& ui, NodeHandle dlg);
std::string fileDialogEntry(const UiContext& ui, NodeHandle dlg, u32 index); ///< names; folders end in '/'
/// Activates entry `index` (folder: enter; file: accept). Returns the accepted path or "".
std::string fileDialogActivate(UiContext& ui, NodeHandle dlg, u32 index);

/// Node graph editor: nodes with typed input / output pins, bezier links, drag nodes, drag from an output pin to an
/// input pin to link (types must match unless either is 0 = any; an input takes one link; cycles are refused),
/// right-drag or middle-drag pans, wheel zooms, Delete removes the selected node. value() = selected node (-1).
/// Link changes emit ValueChanged {value = link count}.
struct GraphPin { std::string_view name; u32 type = 0; };
NodeHandle nodeGraph(UiContext& ui, NodeHandle parent, std::string_view id = {});
u32 nodeGraphAddNode(UiContext& ui, NodeHandle graph, std::string_view title, Vec2 pos, std::span<const GraphPin> inputs,
                     std::span<const GraphPin> outputs);
bool nodeGraphConnect(UiContext& ui, NodeHandle graph, u32 fromNode, u32 outPin, u32 toNode, u32 inPin);
bool nodeGraphDisconnect(UiContext& ui, NodeHandle graph, u32 toNode, u32 inPin);
bool nodeGraphRemoveNode(UiContext& ui, NodeHandle graph, u32 node);
u32 nodeGraphNodeCount(const UiContext& ui, NodeHandle graph);
u32 nodeGraphLinkCount(const UiContext& ui, NodeHandle graph);
Vec2 nodeGraphNodePos(const UiContext& ui, NodeHandle graph, u32 node);
/// Screen position of a pin's dot (tests, tutorials). `output` picks the side.
Vec2 nodeGraphPinPos(const UiContext& ui, NodeHandle graph, u32 node, u32 pin, bool output);
/// Topological order of live nodes (empty when the graph has a cycle, which connect never allows).
std::vector<u32> nodeGraphTopoOrder(const UiContext& ui, NodeHandle graph);

/// Curve editor: one float curve of keys (t, v) with linear / smooth (Catmull-Rom style, monotone in t) / step
/// interpolation. Click empty space adds a key, drag moves keys (t kept between neighbours), Delete removes the
/// selected key. Changes emit ValueChanged {value = key count}.
enum class CurveInterp : u8 { Linear, Smooth, Step };
NodeHandle curveEditor(UiContext& ui, NodeHandle parent, f32 tMin = 0.f, f32 tMax = 1.f, f32 vMin = 0.f, f32 vMax = 1.f,
                       std::string_view id = {});
u32 curveAddKey(UiContext& ui, NodeHandle curve, f32 t, f32 v, CurveInterp interp = CurveInterp::Smooth);
u32 curveKeyCount(const UiContext& ui, NodeHandle curve);
Vec2 curveKey(const UiContext& ui, NodeHandle curve, u32 index); ///< (t, v)
f32 curveEvaluate(const UiContext& ui, NodeHandle curve, f32 t);

/// Timeline: named tracks with clips [start, end), a time ruler and a playhead. Dragging the ruler scrubs
/// (ValueChanged {value = time}); dragging a clip moves it (snapped to `snap` seconds when > 0); wheel zooms.
/// value() = playhead time in seconds.
NodeHandle timeline(UiContext& ui, NodeHandle parent, f32 duration, std::string_view id = {});
u32 timelineAddTrack(UiContext& ui, NodeHandle tl, std::string_view name);
u32 timelineAddClip(UiContext& ui, NodeHandle tl, u32 track, f32 start, f32 end, std::string_view label, Color color);
void timelineSetSnap(UiContext& ui, NodeHandle tl, f32 seconds);
/// Visible range [start, start + span) seconds, scaled to the lane width (span 0 = free zoom: wheel / pxPerSec).
/// With a range set, Ctrl+wheel zooms about the pointer and Shift+wheel pans, then emit Event {value = 1}.
void timelineSetView(UiContext& ui, NodeHandle tl, f32 startSec, f32 spanSec);
Vec2 timelineView(const UiContext& ui, NodeHandle tl); ///< (start, span) seconds
Vec2 timelineClip(const UiContext& ui, NodeHandle tl, u32 track, u32 clip); ///< (start, end)
/// Clips under time `t` on every track (sequencer queries).
u32 timelineActiveClips(const UiContext& ui, NodeHandle tl, f32 t);

/// Viewport widget: shows an externally rendered image (a rasteriser / GPU image index, e.g. the engine's scene
/// colour target - zero copy) letterboxed to its aspect ratio, and forwards input in image pixel coordinates.
/// Each forwarded input emits SignalKind::Event {value = EventType}; read it with viewportLastInput().
struct ViewportInput { EventType type = EventType::PointerMove; Vec2 pos; MouseButton button = MouseButton::Left; Vec2 wheel; Key key = Key::None; };
NodeHandle viewportWidget(UiContext& ui, NodeHandle parent, u32 imageIndex, u32 imageW, u32 imageH, std::string_view id = {});
void viewportSetImage(UiContext& ui, NodeHandle vp, u32 imageIndex, u32 imageW, u32 imageH);
Rect viewportImageRect(const UiContext& ui, NodeHandle vp); ///< letterboxed rect in context coordinates
const ViewportInput& viewportLastInput(const UiContext& ui, NodeHandle vp);
u32 viewportInputCount(const UiContext& ui, NodeHandle vp);
/// Vector overlay painted over the viewport image (gizmos, selection markers), in image pixel coordinates; it is part
/// of the UI draw list, so it composes over an external (zero-copy) image as well as a CPU one.
struct ViewportOverlay {
    struct Line { Vec2 a, b; Color color; f32 width = 2.f; };
    struct Dot { Vec2 center; f32 radius = 4.f; Color color; Color border{}; f32 borderWidth = 0.f; };
    std::vector<Line> lines;
    std::vector<Dot> dots;
    /// Frame around the whole viewport (frameWidth > 0) and a centred top badge (badge non-empty), widget space:
    /// the Qt play-mode frame (green PLAYING / amber PAUSED).
    Color frame{};
    f32 frameWidth = 0.f;
    std::string badge;
    Color badgeColor{}, badgeText{};
};
void viewportSetOverlay(UiContext& ui, NodeHandle vp, ViewportOverlay overlay);
const ViewportOverlay& viewportOverlay(const UiContext& ui, NodeHandle vp);

/// Code editor: multi-line text with a line-number gutter, syntax colouring (Lua / C-like keywords, strings,
/// numbers, comments), caret + selection with Shift, Tab inserts spaces, auto-indent on Enter, undo (Ctrl+Z),
/// copy / cut / paste, vertical scrolling. text() = the document. Edits emit TextChanged.
enum class CodeLanguage : u8 { Plain, Lua, Cpp, Css };
NodeHandle codeEdit(UiContext& ui, NodeHandle parent, std::string_view text = {}, CodeLanguage lang = CodeLanguage::Lua,
                    std::string_view id = {});
void codeEditSetText(UiContext& ui, NodeHandle ed, std::string_view text);
std::string codeEditText(const UiContext& ui, NodeHandle ed);
u32 codeEditLineCount(const UiContext& ui, NodeHandle ed);
void codeEditCaret(const UiContext& ui, NodeHandle ed, u32& line, u32& column);
void codeEditSetCaret(UiContext& ui, NodeHandle ed, u32 line, u32 column);
/// Token class of the character at (line, column): 0 text, 1 keyword, 2 string, 3 number, 4 comment.
u8 codeEditTokenAt(const UiContext& ui, NodeHandle ed, u32 line, u32 column);

/// Civil date and time (no zone), as QDateTime with Qt::LocalTime ignored.
struct DateTime {
    int year = 2000, month = 1, day = 1, hour = 0, minute = 0, second = 0, msec = 0;
};
struct DateTimeEditOptions {
    /// QDateTimeEdit::displayFormat letters: yy yyyy, M MM MMM MMMM, d dd ddd dddd, H HH (24 h), h hh (12 h with AP),
    /// m mm, s ss, z zzz, AP ap; '...' quotes literals. Empty = "yyyy-MM-dd HH:mm" / "yyyy-MM-dd" / "HH:mm".
    std::string displayFormat;
    /// Month / weekday names: "" = English; "system" or a name such as "de_DE.UTF-8" = that locale's names from the
    /// C++ runtime / OS (std::time_put), falling back to English when the runtime lacks it (MinGW has only "C").
    std::string locale;
    /// Two-digit years (yy) typed map into [twoDigitYearStart, twoDigitYearStart + 99] (QDate baseYear style);
    /// -1 keeps the shown century.
    int twoDigitYearStart = -1;
    bool calendarPopup = true;  ///< ignored for timeEdit
    bool wrapping = false;      ///< stepping past a section's end wraps inside the section (Qt)
    bool carry = false;         ///< stepping carries into the next unit instead (the legacy dateTimeEdit overload)
    DateTime minimum{100, 1, 1}, maximum{9999, 12, 31, 23, 59, 59, 999};
};
/// QDateTimeEdit / QDateEdit / QTimeEdit: the display format's sections are edited one at a time. Left / Right /
/// Home / End and Tab / Shift+Tab (then the focus chain) pick a section, a click picks the one under the pointer;
/// Up / Down / wheel step it (PageUp / PageDown by 10) clamped to the section and the month length, digits type
/// into it and advance when no more digits fit, month / weekday names match by prefix, a / p set AM / PM, a
/// separator jumps to the next section. Space / F4 / Alt+Down / the button open the calendar popup (it keeps the
/// time). Enter emits EditingFinished. value() = days since 1970-01-01 (exact in f32); ValueChanged carries the
/// days and the ISO 8601 text. The a11y role is DateTimeInput / DateInput / TimeInput, with the shown text as value.
NodeHandle dateTimeEdit(UiContext& ui, NodeHandle parent, const DateTime& value, const DateTimeEditOptions& o = {},
                        std::string_view id = {});
NodeHandle dateEdit(UiContext& ui, NodeHandle parent, int year, int month, int day, const DateTimeEditOptions& o = {},
                    std::string_view id = {});
NodeHandle timeEdit(UiContext& ui, NodeHandle parent, int hour, int minute, int second = 0, const DateTimeEditOptions& o = {},
                    std::string_view id = {});
/// Legacy: "yyyy-MM-dd HH:mm" with carry stepping (minute 59 + 1 -> next hour).
NodeHandle dateTimeEdit(UiContext& ui, NodeHandle parent, int year, int month, int day, int hour = 0, int minute = 0,
                        std::string_view id = {});
void setDateTimeEditOptions(UiContext& ui, NodeHandle ed, const DateTimeEditOptions& o);
DateTime dateTimeEditDateTime(const UiContext& ui, NodeHandle ed);
void setDateTimeEditDateTime(UiContext& ui, NodeHandle ed, const DateTime& value); ///< clamped to min / max
void dateTimeEditValue(const UiContext& ui, NodeHandle ed, int& year, int& month, int& day, int& hour, int& minute);
void setDateTimeEditValue(UiContext& ui, NodeHandle ed, int year, int month, int day, int hour, int minute);
i64 dateTimeEditMinutes(const UiContext& ui, NodeHandle ed);
std::string dateTimeEditText(const UiContext& ui, NodeHandle ed);
u32 dateTimeEditField(const UiContext& ui, NodeHandle ed); ///< current section (index among the editable sections)
u32 dateTimeEditSectionCount(const UiContext& ui, NodeHandle ed);
void setDateTimeEditSection(UiContext& ui, NodeHandle ed, u32 index);
std::string dateTimeEditSectionText(const UiContext& ui, NodeHandle ed, u32 index);
bool dateTimeEditPopupOpen(const UiContext& ui, NodeHandle ed);
bool dateTimeEditLocalized(const UiContext& ui, NodeHandle ed); ///< names come from a runtime locale
/// Month / year changes return to the day last set on purpose when it fits (31 Oct -> Jun 30 -> Jul 31, as Qt).
/// The calendar popup greys days outside minimum / maximum.

/// Video widget: plays a VideoSource (function table). Theora .ogv files: fuse_ui_video's `video::TheoraVideo::source()`
/// (fuse/ui/video/theora_video.hpp); `testPatternSource()` is a built-in moving pattern. Audio sync is not done. Space toggles play, the scrubber seeks.
/// value() = current frame. Frames are pulled from `frame()` into an image the rasteriser shows.
struct VideoSource {
    void* user = nullptr;
    u32 width = 0, height = 0, frames = 0;
    f32 fps = 30.f;
    /// Fills `rgba` (width * height premultiplied RGBA8, R in the low byte) with frame `index`.
    void (*frame)(void* user, u32 index, u32* rgba) = nullptr;
    /// Optional: told when playback starts / stops and when the user seeks during playback (at `frame`); a source
    /// with an audio track starts / stops / repositions its audio here.
    void (*playback)(void* user, bool playing, u32 frame) = nullptr;
    /// Optional master clock (seconds of media actually heard); while it returns >= 0 during playback the widget
    /// shows the frame at that time instead of advancing one frame per tick (audio-driven A/V sync).
    double (*clock)(void* user) = nullptr;
};
const VideoSource& testPatternSource();
NodeHandle videoWidget(UiContext& ui, NodeHandle parent, const VideoSource* source, std::string_view id = {});
/// The rasteriser image index the widget paints (register it with CpuRasterizer::addImage via videoImage()).
const Image* videoImage(const UiContext& ui, NodeHandle video);
void videoSetImageIndex(UiContext& ui, NodeHandle video, u32 index);
void videoPlay(UiContext& ui, NodeHandle video, bool play);
bool videoPlaying(const UiContext& ui, NodeHandle video);
void videoSeek(UiContext& ui, NodeHandle video, u32 frame);
u32 videoFrame(const UiContext& ui, NodeHandle video);
u32 videoDecodedFrames(const UiContext& ui, NodeHandle video);
/// The transport label: "m:ss.cc" position (frame / fps, rounded to centiseconds), optionally " / " duration.
std::string videoTimeText(u32 frame, u32 frames, f32 fps, bool withDuration);

/// Registers the Tier D (2) classes; install() calls it.
void registerTierD2(UiContext& ui);

} // namespace fuse::ui::widgets
