// FUSE UI widgets: Qt-parity options of the Tier A / B input widgets (ported from the gui-framework branch,
// docs/ui/archive/PORT_gui_framework.md §3.7): QLineEdit echo modes / max length / placeholder / read-only /
// validators / input masks, QCompleter, editable QComboBox, QSpinBox options, QTabBar options.
// Included by widgets.hpp. A widget without options set behaves (and paints) exactly as before: options are per-node
// records beside the context, looked up only while at least one exists.
#pragma once

#include "fuse/ui/ui.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace fuse::ui::widgets {

struct ItemModel;

// ---- QLineEdit parity: echo modes, max length, placeholder, read-only, validators, input mask ------------------------
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
    char32_t maskChar = U'•'; ///< what Password shows per character
};
/// Sets the options of a lineEdit (replacing earlier ones). The current text is kept: re-typed into an input mask,
/// truncated to maxLength.
void setLineEditOptions(UiContext& ui, NodeHandle edit, LineEditOptions options);
/// The options in effect (defaults for a field that has none).
const LineEditOptions& lineEditOptions(const UiContext& ui, NodeHandle edit);
/// Real text of a line edit (also for masked fields), and its validator state.
std::string_view lineEditText(UiContext& ui, NodeHandle edit);
Validation lineEditState(UiContext& ui, NodeHandle edit);
/// Replace the text programmatically (not validated, truncated to maxLength; emits TextChanged). Caret to the end.
/// With an input mask the characters are typed into the cells (non-matching ones are dropped).
void setLineEditText(UiContext& ui, NodeHandle edit, std::string_view text);
/// Signals of a line edit with options: TextChanged {text = real text} per edit; Enter emits Submitted {text} and
/// then EditingFinished {text}, both only for Acceptable text (after fixup); losing the focus emits EditingFinished
/// when the text was edited since the last one and is Acceptable.
///
/// QIntValidator / QDoubleValidator / QRegularExpressionValidator. Number validators: a partial number ("", "-",
/// "1." or a value outside the range that more digits could still fix) is Intermediate. regexValidator: a full match
/// is Acceptable, a prefix of some match is Intermediate, anything else Invalid (so non-matching keystrokes are
/// rejected, as QRegularExpressionValidator). Matching runs on a small Thompson NFA over code points: literals,
/// . [classes] (ranges, negation, \d \w \s), \d \D \w \W \s \S, groups (?:), |, * + ? {m} {m,} {m,n} (lazy forms
/// accepted), ^ $ at the ends. A pattern with other syntax (backreferences, lookaround) is NOT supported: its
/// validator accepts every text (regexPartialMatch reports it through `supported`).
Validator intValidator(long long lo, long long hi);
Validator doubleValidator(double lo, double hi, int decimals = 1000);
Validator regexValidator(std::string_view ecmaPattern);
/// Fixups: clamp into [lo, hi] (non-numbers become the bound nearest 0); round to `decimals`.
Fixup intFixup(long long lo, long long hi);
Fixup doubleFixup(double lo, double hi, int decimals);
/// Partial regex match on its own (Invalid / Intermediate / Acceptable); false in `supported` when the pattern
/// uses syntax the matcher does not have (the result is then Acceptable).
Validation regexPartialMatch(std::string_view ecmaPattern, std::string_view text, bool* supported = nullptr);

// ---- QSpinBox / QDoubleSpinBox parity --------------------------------------------------------------------------------
/// On a spinBox / doubleSpinBox with options: typed entry (focus selects the value, typing replaces it; Backspace,
/// Escape cancels, Enter / focus out commits an in-range number, anything else reverts; EditingFinished {value}),
/// prefix / suffix, decimals, wrapping, specialValueText (shown at the minimum, typing it selects the minimum),
/// adaptive decimal stepping, keyboardTracking (ValueChanged per keystroke or only on commit), textFromValue /
/// valueFromText. A spin box without options keeps the plain arrows / wheel / keys behaviour.
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
const SpinBoxOptions& spinBoxOptions(const UiContext& ui, NodeHandle spin); ///< defaults when none were set
std::string spinBoxText(const UiContext& ui, NodeHandle spin);      ///< QAbstractSpinBox::text (what is shown)
std::string spinBoxCleanText(const UiContext& ui, NodeHandle spin); ///< QSpinBox::cleanText
void spinBoxStepBy(UiContext& ui, NodeHandle spin, i32 steps);      ///< QAbstractSpinBox::stepBy
/// QAbstractSpinBox::stepEnabled: false at max (up) / min (down) unless wrapping. With options, disabled arrows are
/// dimmed and Up / Down / PageUp / PageDown / wheel / arrow clicks in that direction do nothing; stepBy still steps.
bool spinBoxStepEnabled(const UiContext& ui, NodeHandle spin, bool up);
/// Selection / caret in the editable text (byte offsets, without prefix / suffix). Focus selects all (as Qt);
/// Left / Right / Home / End (Shift extends), Ctrl+A, click / drag, Ctrl+C / X / V; typing replaces the selection.
/// Holding an arrow button repeats after 0.5 s every 0.15 s (Qt's style defaults), faster when accelerated.
void spinBoxSelection(const UiContext& ui, NodeHandle spin, u32& from, u32& to, u32& caret);
void setSpinBoxSelection(UiContext& ui, NodeHandle spin, u32 from, u32 to);

// ---- QCompleter parity -----------------------------------------------------------------------------------------------
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
/// Attaches a completer to a lineEdit (an empty model and item list removes it). The prefix is the whole text, as
/// QLineEdit: typing (not programmatic setLineEditText) refilters. Popup: a list under the field (class "completer"
/// in the overlay layer, keyboard focus stays in the field); Down / Up / PageDown / PageUp / Ctrl+Home / Ctrl+End move
/// the highlight and put the completion in the field (Highlighted signal on the edit), stepping past either end
/// returns to the typed prefix; Enter / Tab / click choose (Activated, then Enter still submits as returnPressed);
/// Escape closes and restores the prefix. Inline: the first match is appended selected after the caret (not after
/// Backspace / Delete); Up / Down cycle the matches. UnfilteredPopup: every row is listed, the best match highlighted.
/// A11y: the list is a List of ListItem nodes with `selected` on the highlighted one (Click / Select choose it).
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

// ---- editable combo box (QComboBox::setEditable(true)) -------------------------------------------------------------
/// A lineEdit with an inline, case-insensitive completer over the items and a drop button (class "editcombo").
/// value() = current index (-1 none), text = items ('\n'). Up / Down / PageUp / PageDown step the current item,
/// Alt+Down / Alt+Up / F4 / the button open the full list (current row highlighted). Enter selects the item whose text
/// matches exactly, else inserts the text per the policy (QComboBox::InsertPolicy) and makes it current. Changes emit
/// ValueChanged {index} and TextChanged {item} on the combo. A11y: ComboBox (value = the field text, expanded while
/// the list shows, Expand / Collapse / Increment / Decrement), holding the TextField.
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

// ---- QTabBar / QTabWidget parity -----------------------------------------------------------------------------------
/// Options of a tabBar (or tabWidgetBar(tabs)). Closable tabs draw a close box; clicking it emits SignalKind::Event
/// {text = "tabCloseRequested", value = index} (the app removes the tab, e.g. tabWidgetRemovePage). Movable tabs
/// reorder live while dragged (start distance 10 px) and emit Event {text = "tabMoved", value = to, value2 = from};
/// in a tab widget the pages follow. Disabled tabs are greyed and skipped by click, arrows, Home / End and the wheel
/// (which steps the current tab). Tabs that overflow scroll, with two scroll buttons at the far end, and the current
/// tab is kept in view; setTabUsesScrollButtons(false) shrinks the tabs to fit instead (titles end in an ellipsis).
/// Accessibility: the tabs are the bar's Tab items with their real bounds, `disabled` on disabled tabs.
/// A tab bar no option was set on keeps its plain behaviour (and pixels). Not ported from the branch: tab position
/// (South / West / East), elide modes, per-tab icons and tooltips.
void setTabsClosable(UiContext& ui, NodeHandle bar, bool on);
bool tabsClosable(const UiContext& ui, NodeHandle bar);
void setTabsMovable(UiContext& ui, NodeHandle bar, bool on);
bool tabsMovable(const UiContext& ui, NodeHandle bar);
void setTabEnabled(UiContext& ui, NodeHandle bar, u32 index, bool on);
bool tabEnabled(const UiContext& ui, NodeHandle bar, u32 index);
void setTabUsesScrollButtons(UiContext& ui, NodeHandle bar, bool on);
bool tabUsesScrollButtons(const UiContext& ui, NodeHandle bar);
/// Moves a tab (QTabBar::moveTab): the same tab stays current, a tab widget's page follows; emits "tabMoved".
void tabBarMoveTab(UiContext& ui, NodeHandle bar, u32 from, u32 to);
Rect tabBarTabRect(UiContext& ui, NodeHandle bar, u32 index); ///< context coordinates, scrolled (after update)
f32 tabBarScroll(const UiContext& ui, NodeHandle bar);        ///< px the tabs are scrolled by
/// QTabWidget::addTab / removeTab: appends a page (returned) / removes page `index` (destroying it). Removing a tab
/// before the current one keeps the same page current; removing the current one shows the next (ValueChanged).
NodeHandle tabWidgetAddPage(UiContext& ui, NodeHandle tabs, std::string_view title);
void tabWidgetRemovePage(UiContext& ui, NodeHandle tabs, u32 index);

/// Registers the classes of this header that are not Tier A (completer, completeritem, editcombo, combodrop);
/// setCompleter / editableComboBox call it, and so does installClasses for "editcombo".
void registerEditWidgets(UiContext& ui);

} // namespace fuse::ui::widgets
