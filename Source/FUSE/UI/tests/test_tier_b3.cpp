// fuse_ui_tier_b3: the widgets that complete the plan §5 catalogue — group box, stack, tool bar, status bar, main
// window, context menu, message box, wizard, popover, toast, header, text edit, plain text edit, rich text.
// `--gate=<name>` runs one widget's checks (ctest fuse_ui_widget_<name>); `--out=<png>` writes the demo screen.
#include "test_util.hpp"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "stb_write_impl.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

using namespace fuse::ui;
using namespace fuse::ui::test;
namespace w = fuse::ui::widgets;

namespace {
Vec2 ctr(Rect r) { return {r.x + r.w * 0.5f, r.y + r.h * 0.5f}; }
void click(UiContext& ui, Vec2 p, MouseButton b = MouseButton::Left) {
    ui.pointerMove(p);
    ui.pointerDown(p, b);
    ui.pointerUp(p, b);
    ui.update(1.f / 60.f);
}
void key(UiContext& ui, Key k, u8 mods = 0) {
    ui.keyDown(k, mods);
    ui.keyUp(k, mods);
    ui.update(1.f / 60.f);
}
struct Fixture {
    UiContext ui{{.viewport = {900, 640}, .font = &font()}};
    Fixture() {
        w::install(ui);
        ui.setCss(ui.root(), "padding", "10px");
        ui.setCss(ui.root(), "gap", "8px");
    }
};

void gateGroupBox() {
    Fixture f;
    UiContext& ui = f.ui;
    const NodeHandle plain = w::groupBox(ui, ui.root(), "Rendering", false, true, "plain");
    w::checkBox(ui, w::groupBoxBody(ui, plain), "Shadows", true, "shadows");
    const NodeHandle g = w::groupBox(ui, ui.root(), "Advanced", true, false, "adv");
    const NodeHandle inner = w::button(ui, w::groupBoxBody(ui, g), "Tune", "tune");
    int toggles = 0, clicks = 0;
    ui.connect(g, SignalKind::Toggled, [&toggles](UiContext&, NodeHandle, const SignalArgs&) { ++toggles; });
    ui.connect(inner, SignalKind::Clicked, [&clicks](UiContext&, NodeHandle, const SignalArgs&) { ++clicks; });
    ui.update();
    UI_CHECK(ui.alive(w::groupBoxBody(ui, plain)));
    UI_CHECK(ui.rect(plain).h > ui.rect(ui.find("shadows")).h);
    UI_CHECK(!ui.checked(g));
    click(ui, ctr(ui.rect(inner))); // unchecked group: the body takes no input
    UI_CHECK_EQ(clicks, 0);
    click(ui, ctr(ui.rect(ui.firstChild(g))));
    UI_CHECK_EQ(toggles, 1);
    UI_CHECK(ui.checked(g));
    click(ui, ctr(ui.rect(inner)));
    UI_CHECK_EQ(clicks, 1);
}

void gateStack() {
    Fixture f;
    UiContext& ui = f.ui;
    const NodeHandle st = w::stack(ui, ui.root(), "stack");
    const NodeHandle a = w::stackAddPage(ui, st), b = w::stackAddPage(ui, st), c = w::stackAddPage(ui, st);
    w::label(ui, a, "page a");
    w::label(ui, b, "page b");
    w::label(ui, c, "page c");
    int changes = 0;
    ui.connect(st, SignalKind::ValueChanged, [&changes](UiContext&, NodeHandle, const SignalArgs&) { ++changes; });
    ui.update();
    UI_CHECK_EQ(w::stackCount(ui, st), 3u);
    UI_CHECK(ui.computed(a).visible && !ui.computed(b).visible && !ui.computed(c).visible);
    w::setStackIndex(ui, st, 2);
    ui.update();
    UI_CHECK(!ui.computed(a).visible && ui.computed(c).visible);
    UI_CHECK_EQ(w::stackIndex(ui, st), 2u);
    w::setStackIndex(ui, st, 9); // clamped, already there
    UI_CHECK_EQ(w::stackIndex(ui, st), 2u);
    UI_CHECK_EQ(changes, 1);
    UI_CHECK(w::stackPage(ui, st, 1) == b);
}

void gateBars() {
    Fixture f;
    UiContext& ui = f.ui;
    const NodeHandle tb = w::toolBar(ui, ui.root(), "tb");
    const NodeHandle open = w::toolBarAction(ui, tb, "Open", "Open a file", "open");
    w::toolBarSeparator(ui, tb);
    const NodeHandle save = w::toolBarAction(ui, tb, "Save", {}, "save");
    const NodeHandle sb = w::statusBar(ui, ui.root(), "sb");
    const NodeHandle perm = w::statusBarPermanent(ui, sb, "Ln 1, Col 1", "pos");
    int opened = 0;
    ui.connect(open, SignalKind::Clicked, [&opened](UiContext&, NodeHandle, const SignalArgs&) { ++opened; });
    ui.update();
    UI_CHECK(ui.rect(save).x > ui.rect(open).right()); // a row
    UI_CHECK(std::abs(ui.rect(save).y - ui.rect(open).y) < 1.f);
    click(ui, ctr(ui.rect(open)));
    UI_CHECK_EQ(opened, 1);
    UI_CHECK(ui.rect(perm).right() > ui.rect(sb).right() - 20.f); // permanent widgets sit on the right
    w::statusMessage(ui, sb, "Saved", 1.f);
    ui.update(0.5f);
    UI_CHECK(w::statusText(ui, sb) == "Saved");
    w::statusMessage(ui, sb, "Building", 0.f); // a newer message outlives the older one's timer
    ui.update(1.f);
    UI_CHECK(w::statusText(ui, sb) == "Building");
    w::statusMessage(ui, sb, "Done", 0.25f);
    ui.update(0.5f);
    ui.update(0.f);
    UI_CHECK(w::statusText(ui, sb).empty());
}

void gateMainWindow() {
    Fixture f;
    UiContext& ui = f.ui;
    ui.setCss(ui.root(), "padding", "0px");
    const NodeHandle mw = w::mainWindow(ui, ui.root(), "mw");
    w::menuBarMenu(ui, w::mainWindowMenuBar(ui, mw), "File", "New\nOpen\n-\nQuit", "file");
    w::toolBarAction(ui, w::mainWindowToolBar(ui, mw), "Run", {}, "run");
    const NodeHandle central = w::mainWindowCentral(ui, mw);
    const NodeHandle doc = w::label(ui, central, "document", "doc");
    w::dockWidget(ui, w::mainWindowDockArea(ui, mw), "Outline", w::DockSide::Left, "outline");
    w::statusMessage(ui, w::mainWindowStatusBar(ui, mw), "Ready");
    ui.update();
    const Rect menu = ui.rect(w::mainWindowMenuBar(ui, mw)), tool = ui.rect(w::mainWindowToolBar(ui, mw));
    const Rect docks = ui.rect(w::mainWindowDockArea(ui, mw)), status = ui.rect(w::mainWindowStatusBar(ui, mw));
    UI_CHECK(menu.y < tool.y && tool.y < docks.y && docks.y < status.y);
    UI_CHECK(std::abs(status.bottom() - 640.f) < 1.f); // fills the viewport, status bar at the bottom
    UI_CHECK(docks.h > 400.f);
    UI_CHECK(ui.rect(doc).x > ui.rect(ui.find("outline")).x); // centre right of the left dock
}

void gateContextMenu() {
    Fixture f;
    UiContext& ui = f.ui;
    const NodeHandle target = w::button(ui, ui.root(), "Asset", "asset");
    w::setContextMenu(ui, target, "Rename\nDuplicate\n-\nDelete");
    int picked = -1;
    std::string pickedText;
    ui.connect(target, SignalKind::Submitted, [&](UiContext&, NodeHandle, const SignalArgs& a) {
        picked = int(a.value);
        pickedText = std::string(a.text);
    });
    ui.update();
    const u32 before = ui.nodeCount();
    click(ui, ctr(ui.rect(target))); // a left click opens nothing
    UI_CHECK_EQ(ui.nodeCount(), before);
    click(ui, ctr(ui.rect(target)), MouseButton::Right);
    ui.update();
    UI_CHECK(ui.nodeCount() > before);
    const u32 itemCls = ui.registry().find("menuitem");
    NodeHandle dup;
    for (u32 i = 0; i < 4096 && !dup.valid(); ++i) {
        const NodeHandle h = ui.handleOf(i);
        if (ui.alive(h) && ui.classOf(h) == itemCls && ui.text(h) == "Duplicate") dup = h;
    }
    UI_CHECK(dup.valid());
    if (dup.valid()) click(ui, ctr(ui.rect(dup)));
    ui.update();
    UI_CHECK_EQ(picked, 1);
    UI_CHECK(pickedText == "Duplicate");
}

void gateMessageBox() {
    Fixture f;
    UiContext& ui = f.ui;
    const NodeHandle owner = w::button(ui, ui.root(), "Delete", "delete");
    ui.update();
    ui.setFocus(owner);
    const NodeHandle box = w::messageBox(ui, "Delete asset", "This cannot be undone.", "Delete\nCancel", w::MessageIcon::Warning, "confirm");
    int result = -1;
    std::string name;
    ui.connect(box, SignalKind::Submitted, [&](UiContext&, NodeHandle, const SignalArgs& a) {
        result = int(a.value);
        name = std::string(a.text);
    });
    ui.update();
    UI_CHECK(w::popupOpen(ui, ui.parent(box)));
    UI_CHECK(ui.value(box) == -1.f);
    // modal: the button behind the scrim does not get the click
    int behind = 0;
    ui.connect(owner, SignalKind::Clicked, [&behind](UiContext&, NodeHandle, const SignalArgs&) { ++behind; });
    click(ui, ctr(ui.rect(owner)));
    UI_CHECK_EQ(behind, 0);
    const u32 btnCls = ui.registry().find("button");
    NodeHandle cancel;
    for (u32 i = 0; i < 4096 && !cancel.valid(); ++i) {
        const NodeHandle h = ui.handleOf(i);
        if (ui.alive(h) && ui.classOf(h) == btnCls && ui.text(h) == "Cancel") cancel = h;
    }
    UI_CHECK(cancel.valid());
    if (cancel.valid()) click(ui, ctr(ui.rect(cancel)));
    ui.update();
    UI_CHECK_EQ(result, 1);
    UI_CHECK(name == "Cancel");
    UI_CHECK(!w::popupOpen(ui, ui.parent(box)));
    UI_CHECK(ui.focused() == owner); // focus returns to where it was
}

void gateWizard() {
    Fixture f;
    UiContext& ui = f.ui;
    const NodeHandle wiz = w::wizard(ui, "New Project", "wiz");
    w::label(ui, w::wizardAddPage(ui, wiz, "Template"), "Pick a template");
    w::lineEdit(ui, w::wizardAddPage(ui, wiz, "Name"), "my_game", "name");
    w::label(ui, w::wizardAddPage(ui, wiz, "Summary"), "Ready to create");
    bool finished = false, cancelled = false;
    ui.connect(wiz, SignalKind::Submitted, [&](UiContext&, NodeHandle, const SignalArgs& a) { (a.flag ? finished : cancelled) = true; });
    w::openWizard(ui, wiz);
    ui.update();
    const NodeHandle back = w::wizardButton(ui, wiz, w::WizardButton::Back), next = w::wizardButton(ui, wiz, w::WizardButton::Next);
    UI_CHECK_EQ(w::wizardPageCount(ui, wiz), 3u);
    UI_CHECK_EQ(w::wizardPage(ui, wiz), 0u);
    UI_CHECK(ui.disabled(back));
    UI_CHECK(ui.text(next) == "Next");
    click(ui, ctr(ui.rect(next)));
    UI_CHECK_EQ(w::wizardPage(ui, wiz), 1u);
    UI_CHECK(!ui.disabled(back));
    UI_CHECK(ui.computed(ui.find("name")).visible);
    click(ui, ctr(ui.rect(next)));
    UI_CHECK_EQ(w::wizardPage(ui, wiz), 2u);
    UI_CHECK(ui.text(next) == "Finish");
    click(ui, ctr(ui.rect(back)));
    UI_CHECK_EQ(w::wizardPage(ui, wiz), 1u);
    UI_CHECK(ui.text(next) == "Next");
    click(ui, ctr(ui.rect(next)));
    click(ui, ctr(ui.rect(next))); // Finish
    ui.update();
    UI_CHECK(finished && !cancelled);
    UI_CHECK(!w::popupOpen(ui, ui.parent(wiz)));
    // reopen and cancel
    w::openWizard(ui, wiz);
    ui.update();
    UI_CHECK_EQ(w::wizardPage(ui, wiz), 0u);
    click(ui, ctr(ui.rect(w::wizardButton(ui, wiz, w::WizardButton::Cancel))));
    ui.update();
    UI_CHECK(cancelled);
}

void gatePopoverToast() {
    Fixture f;
    UiContext& ui = f.ui;
    const NodeHandle anchor = w::button(ui, ui.root(), "Filters", "filters");
    ui.update();
    ui.setFocus(anchor);
    const NodeHandle pop = w::popover(ui, anchor);
    w::checkBox(ui, pop, "Meshes", true, "meshes");
    ui.update();
    UI_CHECK(ui.rect(pop).y >= ui.rect(anchor).bottom());
    UI_CHECK(std::abs(ui.rect(pop).x - ui.rect(anchor).x) < 1.f);
    click(ui, ctr(ui.rect(ui.find("meshes")))); // inside: stays open, operates the widget
    UI_CHECK(ui.alive(pop) && !ui.checked(ui.find("meshes")));
    click(ui, {800.f, 600.f}); // outside: closes
    ui.update();
    UI_CHECK(!ui.alive(pop));
    UI_CHECK(ui.focused() == anchor);

    UI_CHECK_EQ(w::toastCount(ui), 0u);
    const NodeHandle t1 = w::toast(ui, "Project saved", 1.f);
    const NodeHandle t2 = w::toast(ui, "Build finished", 3.f);
    ui.update(0.2f);
    ui.update(0.f);
    UI_CHECK_EQ(w::toastCount(ui), 2u);
    const Rect r1 = ui.rect(t1), r2 = ui.rect(t2);
    UI_CHECK(r2.y > r1.y);                                         // newest lowest
    UI_CHECK(std::abs((r1.x + r1.w * 0.5f) - 450.f) < 2.f);        // centred
    UI_CHECK(r2.bottom() < 640.f && r2.bottom() > 560.f);          // near the bottom
    UI_CHECK(ui.hitTest(ctr(r1)) != t1);                           // toasts never take input
    for (int i = 0; i < 8; ++i) ui.update(0.25f);                  // 2 s: the first one is gone
    ui.update(0.f);
    UI_CHECK(!ui.alive(t1) && ui.alive(t2));
    for (int i = 0; i < 8; ++i) ui.update(0.25f);
    ui.update(0.f);
    UI_CHECK_EQ(w::toastCount(ui), 0u);
}

void gateHeader() {
    Fixture f;
    UiContext& ui = f.ui;
    const NodeHandle h = w::headerView(ui, ui.root(), "Name\nKind\nSize", 100.f, "header");
    int sortCol = -1, resized = -1;
    bool asc = false;
    ui.connect(h, SignalKind::Toggled, [&](UiContext&, NodeHandle, const SignalArgs& a) { sortCol = int(a.value); asc = a.flag; });
    ui.connect(h, SignalKind::ValueChanged, [&](UiContext&, NodeHandle, const SignalArgs& a) { resized = int(a.value); });
    ui.update();
    UI_CHECK_EQ(w::headerSectionCount(ui, h), 3u);
    const Rect c = ui.contentRect(h);
    click(ui, {c.x + 150.f, c.y + c.h * 0.5f});
    UI_CHECK_EQ(sortCol, 1);
    UI_CHECK(asc);
    click(ui, {c.x + 150.f, c.y + c.h * 0.5f}); // same section again: descending
    UI_CHECK(!asc);
    bool a2 = true;
    UI_CHECK_EQ(w::headerSortSection(ui, h, &a2), 1);
    UI_CHECK(!a2);
    // drag the first section's right edge 40 px to the right
    const Vec2 edge{c.x + 100.f, c.y + c.h * 0.5f};
    ui.pointerMove(edge);
    ui.pointerDown(edge);
    ui.update(1.f / 60.f);
    ui.pointerMove({edge.x + 40.f, edge.y});
    ui.update(1.f / 60.f);
    ui.pointerUp({edge.x + 40.f, edge.y});
    ui.update(1.f / 60.f);
    UI_CHECK(std::abs(w::headerSectionWidth(ui, h, 0) - 140.f) < 0.5f);
    UI_CHECK_EQ(resized, 0);
    UI_CHECK_EQ(sortCol, 1); // a resize drag is not a sort click
    w::setHeaderTitles(ui, h, "Name\nKind\nSize\nModified");
    UI_CHECK_EQ(w::headerSectionCount(ui, h), 4u);
    UI_CHECK(std::abs(w::headerSectionWidth(ui, h, 0) - 140.f) < 0.5f);
    UI_CHECK(std::abs(w::headerSectionWidth(ui, h, 3) - 100.f) < 0.5f);
}

void gateTextEdit() {
    Fixture f;
    UiContext& ui = f.ui;
    const NodeHandle ed = w::textEdit(ui, ui.root(), "", "notes");
    ui.setCss(ed, "width", "220px");
    ui.setCss(ed, "height", "120px");
    int changes = 0;
    ui.connect(ed, SignalKind::TextChanged, [&changes](UiContext&, NodeHandle, const SignalArgs&) { ++changes; });
    ui.update();
    click(ui, ctr(ui.rect(ed)));
    UI_CHECK(ui.focused() == ed);
    ui.textInput("alpha beta gamma delta epsilon zeta eta theta iota kappa");
    ui.update(1.f / 60.f);
    UI_CHECK(w::textEditText(ui, ed) == "alpha beta gamma delta epsilon zeta eta theta iota kappa");
    UI_CHECK(ui.text(ed) == w::textEditText(ui, ed));
    UI_CHECK(changes == 1);
    const u32 wrapped = w::textEditVisualLines(ui, ed);
    UI_CHECK(wrapped >= 2); // one paragraph, several visual lines at 220 px
    UI_CHECK(ui.wantsTextInput());
    key(ui, Key::Enter);
    ui.textInput("second");
    ui.update(1.f / 60.f);
    UI_CHECK_EQ(w::textEditVisualLines(ui, ed), wrapped + 1);
    // Home goes to the visual line start, Ctrl+Home to the document start
    key(ui, Key::Home);
    UI_CHECK_EQ(w::textEditCaret(ui, ed), u32(w::textEditText(ui, ed).size() - 6));
    key(ui, Key::Home, kModCtrl);
    UI_CHECK_EQ(w::textEditCaret(ui, ed), 0u);
    // Down moves one visual line inside the wrapped paragraph (not to the next paragraph)
    key(ui, Key::Down);
    const u32 afterDown = w::textEditCaret(ui, ed);
    UI_CHECK(afterDown > 0 && afterDown < 56);
    key(ui, Key::Up);
    UI_CHECK_EQ(w::textEditCaret(ui, ed), 0u);
    // select the first word with Shift+Right x5, cut, undo, redo
    for (int i = 0; i < 5; ++i) key(ui, Key::Right, kModShift);
    key(ui, Key::X, kModCtrl);
    UI_CHECK(w::textEditText(ui, ed).rfind(" beta", 0) == 0);
    std::string clip;
    UI_CHECK(ui.clipboardText(clip) && clip == "alpha");
    key(ui, Key::Z, kModCtrl);
    UI_CHECK(w::textEditText(ui, ed).rfind("alpha beta", 0) == 0);
    key(ui, Key::Y, kModCtrl);
    UI_CHECK(w::textEditText(ui, ed).rfind(" beta", 0) == 0);
    key(ui, Key::V, kModCtrl);
    UI_CHECK(w::textEditText(ui, ed).rfind("alpha beta", 0) == 0);
    // multi-byte: Backspace removes one code point
    w::textEditSetText(ui, ed, "caf\xC3\xA9");
    key(ui, Key::End, kModCtrl);
    key(ui, Key::Backspace);
    UI_CHECK(w::textEditText(ui, ed) == "caf");
    // select all + type replaces
    key(ui, Key::A, kModCtrl);
    ui.textInput("x");
    ui.update(1.f / 60.f);
    UI_CHECK(w::textEditText(ui, ed) == "x");
    // read-only refuses edits but still moves the caret
    w::textEditSetReadOnly(ui, ed, true);
    ui.textInput("y");
    key(ui, Key::Backspace);
    UI_CHECK(w::textEditText(ui, ed) == "x");
    w::textEditSetReadOnly(ui, ed, false);
    // long documents scroll to keep the caret visible
    std::string many;
    for (int i = 0; i < 40; ++i) many += "line " + std::to_string(i) + "\n";
    w::textEditSetText(ui, ed, many);
    ui.update();
    UI_CHECK_EQ(w::textEditVisualLines(ui, ed), 41u);
    key(ui, Key::End, kModCtrl);
    UI_CHECK(ui.textCaret().y > ui.rect(ed).y && ui.textCaret().bottom() <= ui.rect(ed).bottom());
    // clicking places the caret
    w::textEditSetText(ui, ed, "hello world");
    ui.update();
    const Rect c = ui.contentRect(ed);
    click(ui, {c.x + 2.f + ui.measureString("hello", ui.computed(ed).fontSize) + 1.f, c.y + 6.f});
    UI_CHECK_EQ(w::textEditCaret(ui, ed), 5u);

    // plain text edit: the code editor without gutter / colouring
    const NodeHandle plain = w::plainTextEdit(ui, ui.root(), "one\ntwo", "plain");
    ui.update();
    UI_CHECK_EQ(w::codeEditLineCount(ui, plain), 2u);
    UI_CHECK_EQ(int(w::codeEditTokenAt(ui, plain, 0, 0)), 0);
    click(ui, {ui.contentRect(plain).x + 8.f, ui.contentRect(plain).y + 6.f});
    ui.textInput("z");
    ui.update(1.f / 60.f);
    UI_CHECK(w::codeEditText(ui, plain) == "zone\ntwo"); // x = 8 px is inside the first glyph: no gutter offset
}

void gateRichText() {
    Fixture f;
    UiContext& ui = f.ui;
    const NodeHandle box = w::panel(ui, ui.root(), "box");
    ui.setCss(box, "width", "260px");
    const NodeHandle rt = w::richText(ui, box,
        "<b>Bold</b> and <color=#ff8800>orange</color> text with a <a href=\"docs/ui\">link to the docs</a> that wraps over "
        "several lines.<br><size=20>Big</size> &lt;escaped&gt; <unknown>kept</unknown>", "rich");
    std::string target;
    int linkIndex = -1;
    ui.connect(rt, SignalKind::Submitted, [&](UiContext&, NodeHandle, const SignalArgs& a) {
        target = std::string(a.text);
        linkIndex = int(a.value);
    });
    ui.update();
    UI_CHECK(w::richTextPlain(ui, rt) ==
             "Bold and orange text with a link to the docs that wraps over several lines.\nBig <escaped> kept");
    UI_CHECK_EQ(w::richTextLinkCount(ui, rt), 1u);
    UI_CHECK(w::richTextLink(ui, rt, 0) == "docs/ui");
    const f32 line = font().metrics(ui.computed(rt).fontSize).lineHeight();
    UI_CHECK(ui.rect(rt).h > line * 3.f); // wrapped at 260 px + a forced break + a taller line
    UI_CHECK(ui.rect(rt).w <= 260.5f);
    const Rect lr = w::richTextLinkRect(ui, rt, 0);
    UI_CHECK(lr.w > 0.f);
    click(ui, {lr.x + 3.f, lr.y + lr.h * 0.5f});
    UI_CHECK(target == "docs/ui");
    UI_CHECK_EQ(linkIndex, 0);
    target.clear();
    click(ui, {ui.rect(rt).x + 3.f, ui.rect(rt).y + 4.f}); // plain text: no link
    UI_CHECK(target.empty());
    // a wider box needs fewer lines
    const f32 narrow = ui.rect(rt).h;
    ui.setCss(box, "width", "700px");
    ui.update();
    UI_CHECK(ui.rect(rt).h < narrow);
    // the colour run paints in its colour
    Image img;
    render(ui, img);
    bool orange = false;
    const Rect r = ui.rect(rt);
    for (int y = int(r.y); y < int(r.bottom()) && !orange; ++y)
        for (int x = int(r.x); x < int(r.right()) && !orange; ++x) {
            const u32 p = img.at(x, y);
            const u32 R = p & 0xFF, G = (p >> 8) & 0xFF, B = (p >> 16) & 0xFF;
            orange = R > 200 && G > 100 && G < 170 && B < 60;
        }
    UI_CHECK(orange);
    w::setRichText(ui, rt, "plain");
    ui.update();
    UI_CHECK_EQ(w::richTextLinkCount(ui, rt), 0u);
    UI_CHECK(ui.rect(rt).h < line * 2.f);
}

void gateDocument() { // the new classes are creatable by name (what .fuseui documents and the designer do)
    Fixture f;
    UiContext& ui = f.ui;
    for (std::string_view cls : {"header", "textedit", "richtext", "toast"}) UI_CHECK(ui.registry().find(cls) != 0xFFFFFFFFu);
    const std::string_view names[] = {"header", "textedit", "richtext", "toast", "no-such-widget"};
    UiContext bare({.viewport = {100, 100}, .font = &font()});
    UI_CHECK_EQ(w::installClasses(bare, names, false), 1u);
    UI_CHECK(bare.registry().find("textedit") != 0xFFFFFFFFu);
}

int demo(const char* out) {
    Fixture f;
    UiContext& ui = f.ui;
    ui.setCss(ui.root(), "padding", "0px");
    const NodeHandle mw = w::mainWindow(ui, ui.root());
    w::menuBarMenu(ui, w::mainWindowMenuBar(ui, mw), "File", "New\nOpen");
    w::menuBarMenu(ui, w::mainWindowMenuBar(ui, mw), "Edit", "Undo\nRedo");
    const NodeHandle tb = w::mainWindowToolBar(ui, mw);
    w::toolBarAction(ui, tb, "New");
    w::toolBarAction(ui, tb, "Open");
    w::toolBarSeparator(ui, tb);
    w::toolBarAction(ui, tb, "Run");
    const NodeHandle central = w::mainWindowCentral(ui, mw);
    ui.setCss(central, "padding", "10px");
    ui.setCss(central, "gap", "8px");
    w::headerView(ui, central, "Name\nKind\nSize", 140.f);
    const NodeHandle g = w::groupBox(ui, central, "Notes", true, true);
    const NodeHandle ed = w::textEdit(ui, w::groupBoxBody(ui, g),
                                      "A word-wrapped text edit. Lines break at the widget width and the caret moves by visual line.");
    ui.setCss(ed, "height", "90px");
    w::richText(ui, central, "<b>Rich text</b> with <color=#ff8800>colour</color>, <u>underline</u>, <size=20>sizes</size> and <a href=\"x\">links</a>.");
    w::plainTextEdit(ui, central, "plain text edit\nno gutter");
    const NodeHandle dock = w::dockWidget(ui, w::mainWindowDockArea(ui, mw), "Stack", w::DockSide::Left);
    const NodeHandle st = w::stack(ui, w::dockBody(ui, dock));
    w::label(ui, w::stackAddPage(ui, st), "page one");
    w::label(ui, w::stackAddPage(ui, st), "page two");
    const NodeHandle sb = w::mainWindowStatusBar(ui, mw);
    w::statusMessage(ui, sb, "Ready");
    w::statusBarPermanent(ui, sb, "UTF-8");
    w::toast(ui, "Project saved");
    ui.update(0.3f);
    ui.update(0.f);
    Image img;
    render(ui, img);
    return stbi_write_png(out, img.width, img.height, 4, img.pixels.data(), img.width * 4) ? 0 : 1;
}
} // namespace

int main(int argc, char** argv) {
    struct Gate { const char* name; void (*fn)(); };
    static const Gate kGates[] = {{"groupbox", &gateGroupBox}, {"stack", &gateStack}, {"toolbar_statusbar", &gateBars},
                                  {"mainwindow", &gateMainWindow}, {"contextmenu", &gateContextMenu}, {"messagebox", &gateMessageBox},
                                  {"wizard", &gateWizard}, {"popover_toast", &gatePopoverToast}, {"header", &gateHeader},
                                  {"textedit", &gateTextEdit}, {"richtext", &gateRichText}, {"catalogue_classes", &gateDocument}};
    const char* only = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "--gate=", 7) == 0) only = argv[i] + 7;
        else if (std::strncmp(argv[i], "--out=", 6) == 0) return demo(argv[i] + 6);
    }
    bool ran = false;
    for (const Gate& g : kGates)
        if (!only || std::strcmp(only, g.name) == 0) {
            g.fn();
            ran = true;
        }
    if (!ran) {
        std::fprintf(stderr, "unknown gate %s\n", only);
        return 2;
    }
    return finish("fuse_ui_tier_b3");
}
