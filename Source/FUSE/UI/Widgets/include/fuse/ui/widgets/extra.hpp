// FUSE UI widgets: the Qt-parity widgets the catalogue did not have yet, ported from the gui-framework branch
// (docs/ui/archive/PORT_gui_framework.md §3.12): QToolBox, QInputDialog, QDialogButtonBox (+ dialog default / reject
// buttons), QLCDNumber, QKeySequenceEdit, QMdiArea / QMdiSubWindow, QGraphicsScene / QGraphicsView and an on-screen
// keyboard. Included by widgets.hpp; the classes are part of install() / installClasses and the default theme.
// (The branch's own group box, stacked widget, status bar, message box and wizard are not here: main has them as
// groupBox, stack, statusBar, messageBox and wizard.)
#pragma once

#include "fuse/ui/actions.hpp"
#include "fuse/ui/ui.hpp"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace fuse::ui::widgets {

/// Registers lcdnumber, keysequenceedit, mdiarea, mdisubwindow, mdititle, graphicsview, vkeyboard; install() calls it.
void registerExtraWidgets(UiContext& ui);

// ---- QDialog default / reject buttons --------------------------------------------------------------------------------
/// QDialog keys. Escape presses the dialog's reject button (class "dialog-reject": its Clicked slots decide what
/// happens) when it has one that is enabled, else closes the dialog as before. Enter that no widget inside consumed
/// (a line edit keeps it for Submitted, a focused button presses itself) presses the default button (class
/// "dialog-default"). An invalid `button` clears the choice. A dialog button box sets both automatically.
void setDialogDefaultButton(UiContext& ui, NodeHandle dlg, NodeHandle button);
void setDialogRejectButton(UiContext& ui, NodeHandle dlg, NodeHandle button);
NodeHandle dialogDefaultButton(const UiContext& ui, NodeHandle dlg);
NodeHandle dialogRejectButton(const UiContext& ui, NodeHandle dlg);

/// QWizard::validateCurrentPage for main's wizard: asked before Next / Finish leaves page `page`; false keeps the
/// wizard on it (Back and Cancel are never blocked). One validator per wizard; nullptr removes it.
using WizardValidator = bool (*)(UiContext& ui, NodeHandle wizard, u32 page, void* user);
void setWizardValidator(UiContext& ui, NodeHandle wizard, WizardValidator fn, void* user = nullptr);

// ---- QDialogButtonBox --------------------------------------------------------------------------------------------------
/// Standard buttons (Qt's bit values) and custom ones, each with a role, laid out in the platform's order (Windows /
/// macOS / KDE / GNOME button orders, as Qt's QPlatformDialogHelper layouts).
namespace StdButton {
enum : u32 {
    Ok = 0x400, Save = 0x800, SaveAll = 0x1000, Open = 0x2000, Yes = 0x4000, YesToAll = 0x8000, No = 0x10000,
    NoToAll = 0x20000, Abort = 0x40000, Retry = 0x80000, Ignore = 0x100000, Close = 0x200000, Cancel = 0x400000,
    Discard = 0x800000, Help = 0x1000000, Apply = 0x2000000, Reset = 0x4000000, RestoreDefaults = 0x8000000
};
}
enum class ButtonRole : u8 { Accept, Reject, Destructive, Action, Help, Yes, No, Reset, Apply };
enum class ButtonBoxLayout : u8 { Platform, Windows, Mac, Kde, Gnome };
struct DialogButtonBoxOptions {
    ButtonBoxLayout layout = ButtonBoxLayout::Platform; ///< Platform = Mac on Apple, else Windows (Qt's generic theme)
    bool centerButtons = false;                         ///< QDialogButtonBox::centerButtons
    bool vertical = false;                              ///< Qt::Vertical: a column, same order
};
/// Clicking a button emits on the box: Clicked {value = role, text} (QDialogButtonBox::clicked; Help role =
/// helpRequested), then Submitted {value = 1} for Accept / Yes roles (accepted) or {value = 0} for Reject / No
/// (rejected). The first Accept (else Yes) button is the default button (classes "primary" and "dialog-default"),
/// the first Reject (else No) button the reject button ("dialog-reject") of the enclosing dialog.
NodeHandle dialogButtonBox(UiContext& ui, NodeHandle parent, u32 standardButtons, const DialogButtonBoxOptions& o = {},
                           std::string_view id = {});
/// Replaces the standard buttons (custom ones stay).
void setDialogButtonBoxStandardButtons(UiContext& ui, NodeHandle box, u32 standardButtons);
NodeHandle dialogButtonBoxAdd(UiContext& ui, NodeHandle box, std::string_view text, ButtonRole role);
void dialogButtonBoxRemove(UiContext& ui, NodeHandle box, NodeHandle button);
void setDialogButtonBoxOptions(UiContext& ui, NodeHandle box, const DialogButtonBoxOptions& o);
NodeHandle dialogButtonBoxButton(const UiContext& ui, NodeHandle box, u32 standardButton); ///< invalid when absent
u32 dialogButtonBoxStandardOf(const UiContext& ui, NodeHandle box, NodeHandle button);     ///< 0 for custom buttons
ButtonRole dialogButtonBoxRole(const UiContext& ui, NodeHandle box, NodeHandle button);
/// The buttons in visual order (left to right / top to bottom).
std::vector<NodeHandle> dialogButtonBoxButtons(const UiContext& ui, NodeHandle box);
NodeHandle dialogButtonBoxDefault(const UiContext& ui, NodeHandle box);
NodeHandle dialogButtonBoxRejectButton(const UiContext& ui, NodeHandle box);
/// Qt's text for a standard button under a layout ("Don't Save" / "Close without Saving" for Discard on macOS / GNOME).
std::string standardButtonText(u32 standardButton, ButtonBoxLayout layout = ButtonBoxLayout::Platform);

// ---- QInputDialog --------------------------------------------------------------------------------------------------------
/// Opens a modal text prompt (a dialog with a label, a line edit and an OK / Cancel button box). OK, or Enter in the
/// field, emits Submitted {text, value = 1} on the returned dialog card; Cancel / Escape emit Submitted {value = 0}.
NodeHandle inputDialog(UiContext& ui, std::string_view title, std::string_view prompt, std::string_view initial = {}, std::string_view id = {});
NodeHandle inputDialogEdit(const UiContext& ui, NodeHandle dlg); ///< the prompt's line edit (set a validator / mask on it)

// ---- QToolBox ----------------------------------------------------------------------------------------------------------
/// Accordion: one header button + page per title ('\n'-separated); exactly one page is open. value() = the open page;
/// ValueChanged on the box when it changes. Up / Down on a focused header move to the neighbouring header.
/// A11y: each header is a Button (`checked` on the open page's), each page a Group named after its title.
NodeHandle toolBox(UiContext& ui, NodeHandle parent, std::string_view titles, std::string_view id = {});
NodeHandle toolBoxPage(const UiContext& ui, NodeHandle box, u32 index);
NodeHandle toolBoxHeader(const UiContext& ui, NodeHandle box, u32 index);
u32 toolBoxCount(const UiContext& ui, NodeHandle box);
void setToolBoxIndex(UiContext& ui, NodeHandle box, u32 index);
NodeHandle toolBoxAddPage(UiContext& ui, NodeHandle box, std::string_view title); ///< QToolBox::addItem; returns the page

// ---- QLCDNumber ----------------------------------------------------------------------------------------------------------
/// Seven-segment display of value() (digits, '-', '.', ':'), or of a text (setLcdText: also a b c d e f h l n o p r
/// s u y _). Hex / octal / binary through the base. Drawn in the node's `color`. A11y: Text with the shown string as value.
NodeHandle lcdNumber(UiContext& ui, NodeHandle parent, u32 digits = 6, std::string_view id = {});
void setLcdText(UiContext& ui, NodeHandle lcd, std::string_view text); ///< shows `text` until setLcdBase / setLcdValue
void setLcdValue(UiContext& ui, NodeHandle lcd, f32 value);            ///< back to showing value()
void setLcdBase(UiContext& ui, NodeHandle lcd, u32 base);              ///< 2, 8, 10 or 16
std::string lcdText(const UiContext& ui, NodeHandle lcd);              ///< what is shown ("E" when it does not fit)
/// Segment mask (bit 0 = a .. bit 6 = g) for a character; 0 for blank / unknown.
u8 sevenSegment(char c);

// ---- QKeySequenceEdit ------------------------------------------------------------------------------------------------------
/// Records the next key chord while focused and shows it as shortcutToString writes it ("Ctrl+Shift+S"); a lone
/// modifier is not a chord. Escape / Backspace clear. Recording emits TextChanged {text}, Submitted {text} and
/// EditingFinished {text}; clearing emits TextChanged {""}. Tab / Shift+Tab still move the focus.
/// A11y: TextField with the chord as value; SetValue {text} takes what parseShortcut reads (other text is ignored).
NodeHandle keySequenceEdit(UiContext& ui, NodeHandle parent, Shortcut initial = {}, std::string_view id = {});
Shortcut keySequence(const UiContext& ui, NodeHandle edit);
void setKeySequence(UiContext& ui, NodeHandle edit, Shortcut s); ///< an invalid shortcut clears; no signal

// ---- QMdiArea / QMdiSubWindow ------------------------------------------------------------------------------------------------
/// MDI area: free-floating sub-windows inside a clipped area. Drag a title bar to move (the title stays reachable:
/// at least 40 px of it inside the area), the bottom-right grip to resize (minimum 80 x 48), press anywhere in a
/// sub-window to raise and activate it, the title's x closes it. Mouse and touch (title bars and grips capture drags).
NodeHandle mdiArea(UiContext& ui, NodeHandle parent, std::string_view id = {});
/// Returns the sub-window; content goes into mdiBody(). `geometry` is relative to the area. Closing emits
/// Submitted {value = 0} on the sub-window before it is destroyed; activation emits Toggled {flag} on the sub-windows
/// that gain / lose it. A11y: Window named after the title.
NodeHandle mdiSubWindow(UiContext& ui, NodeHandle area, std::string_view title, Rect geometry, std::string_view id = {});
NodeHandle mdiBody(const UiContext& ui, NodeHandle sub);
Rect mdiGeometry(const UiContext& ui, NodeHandle sub);
void setMdiGeometry(UiContext& ui, NodeHandle sub, Rect r);
void mdiActivate(UiContext& ui, NodeHandle sub); ///< raise + activate (QMdiArea::setActiveSubWindow)
void mdiClose(UiContext& ui, NodeHandle sub);    ///< as the title's x
void mdiTile(UiContext& ui, NodeHandle area);    ///< QMdiArea::tileSubWindows (a grid over the area; after update)
void mdiCascade(UiContext& ui, NodeHandle area); ///< QMdiArea::cascadeSubWindows
NodeHandle mdiActive(const UiContext& ui, NodeHandle area);
std::vector<NodeHandle> mdiSubWindows(const UiContext& ui, NodeHandle area); ///< back to front

// ---- QGraphicsScene / QGraphicsView --------------------------------------------------------------------------------------
enum class GraphicsShape : u8 { Rect, Ellipse, Line, Text, Polygon };
struct GraphicsItem {
    GraphicsShape shape = GraphicsShape::Rect;
    Rect rect;                 ///< scene coordinates (Line: from (x, y) to (right, bottom); Polygon: unused, see points)
    Color fill = Color::rgb(0x3D8BFD), stroke = Color::rgb(0xFFFFFF, 0.8f);
    f32 strokeWidth = 1.f;
    f32 z = 0.f;
    std::string text;          ///< Text items; the accessible name of any item
    std::vector<Vec2> points;  ///< Polygon (scene coordinates)
    bool movable = true, selectable = true, visible = true, selected = false;
    u64 user = 0;
};
/// Item container with z-ordering, hit testing, selection, a uniform-grid spatial index and a change callback.
/// Owned by the application; a view only points at it (the scene must outlive its views).
class GraphicsScene {
public:
    u32 add(GraphicsItem it);
    void remove(u32 id);
    GraphicsItem* item(u32 id);
    const GraphicsItem* item(u32 id) const;
    void move(u32 id, Vec2 delta);
    /// Topmost visible item under `p` (scene coordinates); ~0u when none.
    u32 itemAt(Vec2 p) const;
    /// Visible items intersecting `r`, bottom to top.
    std::vector<u32> itemsIn(const Rect& r) const;
    void select(u32 id, bool on, bool exclusive = false);
    void clearSelection();
    std::vector<u32> selection() const;
    u32 selectedCount() const;
    u32 lastSelected() const; ///< the selected item added last; ~0u when nothing is selected
    Rect bounds() const;
    u32 size() const;
    u64 revision() const { return m_rev; }
    std::function<void(u32 id)> onChanged;
    /// Items in paint order (z ascending, insertion order for ties).
    std::vector<u32> paintOrder() const;
    /// The same list without a copy: cached, valid until the scene is edited (what the view paints from).
    const std::vector<u32>& orderedItems() const;

private:
    void index(u32 id, bool insert);
    static bool hit(const GraphicsItem& it, Vec2 p);
    std::vector<GraphicsItem> m_items;
    std::vector<u8> m_alive;
    std::vector<u32> m_order;
    static constexpr f32 kCell = 128.f;
    std::vector<std::pair<u64, std::vector<u32>>> m_cells;
    std::vector<u32>* cell(i32 cx, i32 cy, bool create);
    u64 m_rev = 0, m_orderRev = 0;
    mutable std::vector<u32> m_sorted; ///< orderedItems() cache
    mutable std::vector<f32> m_sortedZ;
    mutable u64 m_sortedRev = ~0ull;
};
/// View onto a scene: wheel zooms around the pointer, a drag on an item moves the selected movable items, a drag on
/// empty space rubber-band selects (Ctrl adds), middle-drag / Alt+drag pans. A press selects (Ctrl toggles). Arrow
/// keys move the selection by 10 px, Delete removes it. Emits ValueChanged {value = id, -1 none} when the selection
/// changes and Submitted {value = id} on a double click. Mouse and touch (the view captures drags).
/// A11y: every visible item is a child of the view (Image, or Text for text items; named by its text) with
/// `selected` and a Select action.
NodeHandle graphicsView(UiContext& ui, NodeHandle parent, GraphicsScene* scene, std::string_view id = {});
void setGraphicsScene(UiContext& ui, NodeHandle view, GraphicsScene* scene);
GraphicsScene* graphicsScene(const UiContext& ui, NodeHandle view);
void setViewTransform(UiContext& ui, NodeHandle view, f32 zoom, Vec2 pan);
f32 viewZoom(const UiContext& ui, NodeHandle view);
Vec2 viewPan(const UiContext& ui, NodeHandle view);
Vec2 viewToScene(const UiContext& ui, NodeHandle view, Vec2 p);
Vec2 sceneToView(const UiContext& ui, NodeHandle view, Vec2 s);
/// Fits the scene bounds into the view (after update, so the view has its size).
void fitInView(UiContext& ui, NodeHandle view);
/// The rubber band while one is dragged (context coordinates); false otherwise.
bool viewRubberBand(const UiContext& ui, NodeHandle view, Rect& band);

// ---- on-screen keyboard (Qt Virtual Keyboard parity) -----------------------------------------------------------------------
/// QWERTY letters, one-shot shift (a second press = caps lock), a "?123" symbols page, backspace, space and enter.
/// Keys never take the keyboard focus; they type into the keyboard's target - the text widget that had the focus
/// before the key was pressed (tracked while the keyboard paints and by virtualKeyboardAutoShow, or set with
/// setVirtualKeyboardTarget) - through UiContext::textInput / keyDown, exactly as hardware keys.
/// value() on the keyboard = page (0 letters, 1 symbols); Toggled {flag} reports shift.
NodeHandle virtualKeyboard(UiContext& ui, NodeHandle parent, std::string_view id = {});
/// The key whose label is `label` ("a", "Shift", "?123", "Backspace", "Space", "Enter", ...), for tests / automation.
NodeHandle virtualKeyboardKey(const UiContext& ui, NodeHandle keyboard, std::string_view label);
void setVirtualKeyboardTarget(UiContext& ui, NodeHandle keyboard, NodeHandle target);
NodeHandle virtualKeyboardTarget(const UiContext& ui, NodeHandle keyboard);
/// Show the keyboard while the UI wants text input (UiContext::wantsTextInput) or its target is still being typed
/// into, hide it otherwise; call once per frame.
void virtualKeyboardAutoShow(UiContext& ui, NodeHandle keyboard);

} // namespace fuse::ui::widgets
