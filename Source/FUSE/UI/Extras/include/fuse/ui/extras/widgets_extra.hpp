// Qt-parity widgets not in Tiers A-D: QGroupBox, QStackedWidget, QToolBox, QStatusBar, QMessageBox, QInputDialog,
// QWizard, QLCDNumber, QKeySequenceEdit, QMdiArea / QMdiSubWindow, QCompleter, QGraphicsScene / QGraphicsView.
#pragma once

#include "fuse/ui/ui.hpp"
#include "fuse/ui/widgets/widgets.hpp"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace fuse::ui::widgets {

/// Titled frame; returns the frame, content goes into groupBoxBody(). Checkable boxes disable their body when off
/// (Toggled on the frame).
NodeHandle groupBox(UiContext& ui, NodeHandle parent, std::string_view title, bool checkable = false, std::string_view id = {});
NodeHandle groupBoxBody(const UiContext& ui, NodeHandle box);

/// Pages of which only one is displayed. stackedAddPage appends a page (returned). value() = current index.
NodeHandle stackedWidget(UiContext& ui, NodeHandle parent, std::string_view id = {});
NodeHandle stackedAddPage(UiContext& ui, NodeHandle stack);
void setStackedIndex(UiContext& ui, NodeHandle stack, u32 index);
u32 stackedIndex(const UiContext& ui, NodeHandle stack);
u32 stackedCount(const UiContext& ui, NodeHandle stack);
NodeHandle stackedPage(const UiContext& ui, NodeHandle stack, u32 index);

/// Accordion: one header button + page per title ('\n'-separated); one page open (ValueChanged on the box).
NodeHandle toolBox(UiContext& ui, NodeHandle parent, std::string_view titles, std::string_view id = {});
NodeHandle toolBoxPage(const UiContext& ui, NodeHandle box, u32 index);
void setToolBoxIndex(UiContext& ui, NodeHandle box, u32 index);

/// Status bar: a temporary message (cleared after `seconds`, 0 = sticky) plus permanent widgets on the right.
NodeHandle statusBar(UiContext& ui, NodeHandle parent, std::string_view id = {});
void showStatusMessage(UiContext& ui, NodeHandle bar, std::string_view text, f32 seconds = 0.f);
std::string_view statusMessage(const UiContext& ui, NodeHandle bar);
/// Container for permanent widgets (right side).
NodeHandle statusBarPermanent(const UiContext& ui, NodeHandle bar);

enum class MessageIcon : u8 { None, Information, Warning, Critical, Question };
/// Opens a modal message box; clicking button i (buttons '\n'-separated, e.g. "Save\nDiscard\nCancel") emits
/// Submitted {value = i, text = button} on the returned dialog card and closes it. Escape = last button.
NodeHandle messageBox(UiContext& ui, std::string_view title, std::string_view text, std::string_view buttons = "OK",
                      MessageIcon icon = MessageIcon::Information, std::string_view id = {});
/// Opens a modal text prompt; OK / Enter emits Submitted {text, value = 1}, Cancel emits Submitted {value = 0}.
NodeHandle inputDialog(UiContext& ui, std::string_view title, std::string_view prompt, std::string_view initial = {}, std::string_view id = {});

/// Wizard: modal dialog with one page per title, Back / Next / Finish / Cancel. Submitted {value = 1} on Finish,
/// {value = 0} on Cancel; ValueChanged {value = page} on page change. setWizardValidator blocks Next per page.
NodeHandle wizard(UiContext& ui, std::string_view title, std::string_view pageTitles, std::string_view id = {});
NodeHandle wizardPage(const UiContext& ui, NodeHandle wiz, u32 index);
u32 wizardCurrent(const UiContext& ui, NodeHandle wiz);
void wizardNext(UiContext& ui, NodeHandle wiz);
void wizardBack(UiContext& ui, NodeHandle wiz);
void setWizardValidator(UiContext& ui, NodeHandle wiz, std::function<bool(u32 page)> canLeave);

/// Seven-segment display of value() (digits, '-', '.', ':' also via setLcdText). Hex/Bin modes via base.
NodeHandle lcdNumber(UiContext& ui, NodeHandle parent, u32 digits = 6, std::string_view id = {});
void setLcdText(UiContext& ui, NodeHandle lcd, std::string_view text);
void setLcdBase(UiContext& ui, NodeHandle lcd, u32 base);
std::string lcdText(const UiContext& ui, NodeHandle lcd);
/// Segment mask (bit 0 = a .. bit 6 = g) for a character; 0 for blank / unknown.
u8 sevenSegment(char c);

/// Records the next key chord when focused ("Ctrl+Shift+S"); Escape clears. TextChanged + Submitted on record.
NodeHandle keySequenceEdit(UiContext& ui, NodeHandle parent, std::string_view initial = {}, std::string_view id = {});
std::string keySequenceText(Key k, u8 mods);
std::string_view keyName(Key k);

/// MDI area: free-floating sub-windows (drag title to move, bottom-right grip to resize, click to raise, x closes).
NodeHandle mdiArea(UiContext& ui, NodeHandle parent, std::string_view id = {});
/// Returns the sub-window; content goes into mdiBody(). Submitted {value = 0} on the sub-window when closed.
NodeHandle mdiSubWindow(UiContext& ui, NodeHandle area, std::string_view title, Rect geometry, std::string_view id = {});
NodeHandle mdiBody(const UiContext& ui, NodeHandle sub);
Rect mdiGeometry(const UiContext& ui, NodeHandle sub);
void setMdiGeometry(UiContext& ui, NodeHandle sub, Rect r);
void mdiTile(UiContext& ui, NodeHandle area);
void mdiCascade(UiContext& ui, NodeHandle area);
NodeHandle mdiActive(const UiContext& ui, NodeHandle area);
std::vector<NodeHandle> mdiSubWindows(const UiContext& ui, NodeHandle area);

/// Completer for a line edit: Down / typing opens a popup of entries matching the text (prefix, then substring,
/// case-insensitive); picking one sets the text and emits Submitted on the line edit.
void attachCompleter(UiContext& ui, NodeHandle lineEdit, std::vector<std::string> entries, u32 maxShown = 8);
std::vector<std::string> completions(const std::vector<std::string>& entries, std::string_view prefix, u32 max = 8);

// ---- graphics scene / view -------------------------------------------------------------------------------------
enum class GraphicsShape : u8 { Rect, Ellipse, Line, Text, Polygon };
struct GraphicsItem {
    GraphicsShape shape = GraphicsShape::Rect;
    Rect rect;                 ///< scene coordinates (Line: x0,y0 = rect.x,y ; x1,y1 = rect.right, bottom)
    Color fill = Color::rgb(0x3D8BFD), stroke = Color::rgb(0xFFFFFF, 0.8f);
    f32 strokeWidth = 1.f;
    f32 z = 0.f;
    std::string text;          ///< Text items (and tooltips)
    std::vector<Vec2> points;  ///< Polygon (scene coordinates)
    bool movable = true, selectable = true, visible = true, selected = false;
    u64 user = 0;
};
/// Item container with z-ordering, hit testing, selection, a uniform-grid spatial index and change callbacks.
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
    Rect bounds() const;
    u32 size() const;
    u64 revision() const { return m_rev; }
    std::function<void(u32 id)> onChanged;
    /// Items in paint order (z ascending, insertion order for ties).
    std::vector<u32> paintOrder() const;

private:
    void index(u32 id, bool insert);
    static bool hit(const GraphicsItem& it, Vec2 p);
    std::vector<GraphicsItem> m_items;
    std::vector<u8> m_alive;
    std::vector<u32> m_order;
    static constexpr f32 kCell = 128.f;
    std::vector<std::pair<u64, std::vector<u32>>> m_cells;
    std::vector<u32>* cell(i32 cx, i32 cy, bool create);
    u64 m_rev = 0;
};
/// View onto a scene: wheel zooms around the pointer, drag on an item moves it (movable) or rubber-band selects on
/// empty space, middle-drag / Space+drag pans. Clicking selects (Ctrl toggles). Emits ValueChanged {value = id}
/// when the selection changes and Submitted {value = id} on double click.
NodeHandle graphicsView(UiContext& ui, NodeHandle parent, GraphicsScene* scene, std::string_view id = {});
void setViewTransform(UiContext& ui, NodeHandle view, f32 zoom, Vec2 pan);
f32 viewZoom(const UiContext& ui, NodeHandle view);
Vec2 viewPan(const UiContext& ui, NodeHandle view);
Vec2 viewToScene(const UiContext& ui, NodeHandle view, Vec2 p);
Vec2 sceneToView(const UiContext& ui, NodeHandle view, Vec2 s);
/// Fits the scene bounds into the view.
void fitInView(UiContext& ui, NodeHandle view);

/// On-screen keyboard (Qt Virtual Keyboard parity): QWERTY letters, one-shot shift (double press = caps lock),
/// "?123" symbols page, backspace, space and enter. Keys never take focus; they type into the focused text node
/// (or the last one focused outside the keyboard) through UiContext::textInput / keyDown, exactly as hardware keys.
/// value() on the keyboard = page (0 letters, 1 symbols); Toggled {flag} reports shift.
NodeHandle virtualKeyboard(UiContext& ui, NodeHandle parent, std::string_view id = {});
/// The key whose label is `label` ("a", "Shift", "?123", "Backspace", "Space", "Enter", ...), for tests / automation.
NodeHandle virtualKeyboardKey(const UiContext& ui, NodeHandle keyboard, std::string_view label);
/// Show the keyboard while the UI wants text input (UiContext::wantsTextInput), hide it otherwise; call per frame.
void virtualKeyboardAutoShow(UiContext& ui, NodeHandle keyboard);

void registerExtraWidgets(UiContext& ui);

} // namespace fuse::ui::widgets
