// fuse_ui_port_doc: the ported widgets and options as `.fuseui` document classes / properties (Doc/src/doc_classes.cpp):
// lineedit / spinbox / tabwidget / tabbar option properties, editcombo, lcdnumber, keysequenceedit, toolbox, mdiarea +
// mdiwindow, graphicsview (document-owned scene), vkeyboard, dialogbuttonbox; reload keeps them working.
#include "test_util.hpp"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif

#include "fuse/ui/doc/document.hpp"

#include <cmath>
#include <string>

using namespace fuse::ui;
using namespace fuse::ui::doc;
using namespace fuse::ui::test;
namespace w = fuse::ui::widgets;

namespace {
const char* kDoc = R"json({
  "fuseui": 1,
  "root": { "class": "panel", "id": "root", "children": [
    { "class": "lineedit", "id": "pw", "placeholder": "Password", "echo": "password", "maxLength": 6 },
    { "class": "lineedit", "id": "ip", "inputMask": "000.000;_" },
    { "class": "lineedit", "id": "ro", "text": "fixed", "readOnly": true },
    { "class": "spinbox", "id": "price", "min": 0, "max": 500, "value": 20, "prefix": "$ ", "suffix": " ea", "specialValueText": "Free", "wrapping": true },
    { "class": "doublespinbox", "id": "angle", "min": 0, "max": 360, "value": 90, "suffix": " deg", "decimals": 2 },
    { "class": "tabwidget", "id": "tabs", "tabs": ["One", "Two", "Three"], "closable": true, "movable": true, "children": [
      { "class": "label", "id": "p0", "text": "first" }, { "class": "label", "id": "p1", "text": "second" }, {}
    ] },
    { "class": "tabbar", "id": "bar", "items": ["a", "b"], "closable": true, "scrollButtons": false },
    { "class": "editcombo", "id": "combo", "items": ["Red", "Green", "Blue"], "value": 1, "insertPolicy": "top", "placeholder": "Colour" },
    { "class": "lcdnumber", "id": "lcd", "digits": 4, "value": 255, "base": 16 },
    { "class": "lcdnumber", "id": "clock", "text": "12:34" },
    { "class": "keysequenceedit", "id": "keys", "text": "Ctrl+Shift+S" },
    { "class": "toolbox", "id": "box", "pages": ["General", "Advanced"], "value": 1, "children": [
      { "class": "label", "id": "t0", "text": "general" }, { "class": "label", "id": "t1", "text": "advanced" }
    ] },
    { "class": "mdiarea", "id": "area", "style": { "height": 200 }, "children": [
      { "class": "mdiwindow", "id": "win1", "text": "Doc 1", "rect": [10, 10, 160, 100], "children": [ { "class": "label", "id": "w1", "text": "in window 1" } ] },
      { "class": "mdiwindow", "id": "win2", "text": "Doc 2" }
    ] },
    { "class": "graphicsview", "id": "view", "zoom": 2, "items": [
      { "shape": "rect", "rect": [10, 10, 40, 30], "text": "box", "fill": "#FF0000" },
      { "shape": "ellipse", "rect": [80, 10, 60, 30], "movable": false },
      { "shape": "polygon", "points": [[0, 60], [40, 60], [20, 90]], "selectable": false },
      { "shape": "line", "rect": [0, 100, 50, 0], "strokeWidth": 3, "z": 2 }
    ] },
    { "class": "vkeyboard", "id": "kb" },
    { "class": "dialogbuttonbox", "id": "buttons", "buttons": ["ok", "cancel", "apply"], "layout": "mac" }
  ] }
})json";

void checkTree(UiContext& ui) {
    auto id = [&](std::string_view s) { return ui.find(s); };
    // line edit options
    const NodeHandle pw = id("pw");
    UI_CHECK(w::lineEditOptions(ui, pw).echo == w::EchoMode::Password && w::lineEditOptions(ui, pw).placeholder == "Password");
    ui.setFocus(pw);
    for (const char* c : {"s", "e", "c", "r", "e", "t", "!", "!"}) ui.textInput(c);
    UI_CHECK(w::lineEditText(ui, pw) == "secret" && ui.text(pw).find("secret") == std::string::npos); // maxLength 6, masked
    const NodeHandle ip = id("ip");
    ui.setFocus(ip);
    for (const char* c : {"1", "2", "x", "3", "4"}) ui.textInput(c);
    UI_CHECK(w::lineEditText(ui, ip) == "123.4" && ui.editBuffer(ip).text() == "123.4__");
    const NodeHandle ro = id("ro");
    ui.setFocus(ro);
    ui.textInput("x");
    UI_CHECK(w::lineEditText(ui, ro) == "fixed");
    // spin box options
    const NodeHandle price = id("price");
    UI_CHECK(w::spinBoxText(ui, price) == "$ 20 ea" && w::spinBoxOptions(ui, price).wrapping);
    ui.setValue(price, 0.f);
    UI_CHECK(w::spinBoxText(ui, price) == "Free" && w::spinBoxStepEnabled(ui, price, false)); // wrapping: down from the minimum
    w::spinBoxStepBy(ui, price, -1);
    UI_CHECK(ui.value(price) == 500.f);
    UI_CHECK(w::spinBoxText(ui, id("angle")) == "90.00 deg");
    // tab options
    const NodeHandle bar = w::tabWidgetBar(ui, id("tabs"));
    UI_CHECK(w::tabsClosable(ui, bar) && w::tabsMovable(ui, bar) && ui.parent(id("p1")) == w::tabPage(ui, id("tabs"), 1));
    UI_CHECK(w::tabsClosable(ui, id("bar")) && !w::tabUsesScrollButtons(ui, id("bar")) && !w::tabsMovable(ui, id("bar")));
    // editable combo
    const NodeHandle combo = id("combo");
    UI_CHECK(w::comboCount(ui, combo) == 3 && w::comboCurrentIndex(ui, combo) == 1 && w::comboItemText(ui, combo, 2) == "Blue");
    UI_CHECK(w::lineEditText(ui, w::comboLineEdit(ui, combo)) == "Green" && w::lineEditOptions(ui, w::comboLineEdit(ui, combo)).placeholder == "Colour");
    ui.setFocus(w::comboLineEdit(ui, combo));
    ui.keyDown(Key::A, kModCtrl);
    for (const char* c : {"T", "e", "a", "l"}) ui.textInput(c);
    ui.keyDown(Key::Enter);
    UI_CHECK(w::comboCount(ui, combo) == 4 && w::comboItemText(ui, combo, 0) == "Teal" && w::comboCurrentIndex(ui, combo) == 0); // insertPolicy top
    // LCD, key sequence
    UI_CHECK(w::lcdText(ui, id("lcd")) == "ff" && w::lcdText(ui, id("clock")) == "12:34");
    UI_CHECK((w::keySequence(ui, id("keys")) == Shortcut{Key::S, kModCtrl | kModShift}));
    // tool box: children are the pages' content, "value" opens a page
    const NodeHandle box = id("box");
    UI_CHECK(w::toolBoxCount(ui, box) == 2 && ui.parent(id("t1")) == w::toolBoxPage(ui, box, 1) && u32(ui.value(box)) == 1);
    UI_CHECK(ui.computed(id("t1")).visible && !ui.computed(w::toolBoxPage(ui, box, 0)).visible);
    // MDI: mdiwindow children of the area, their children in the body
    const NodeHandle area = id("area"), win1 = id("win1");
    UI_CHECK(w::mdiSubWindows(ui, area).size() == 2 && ui.parent(id("w1")) == w::mdiBody(ui, win1));
    UI_CHECK(std::abs(w::mdiGeometry(ui, win1).w - 160.f) < 0.5f && w::mdiActive(ui, area) == id("win2"));
    UI_CHECK(w::mdiGeometry(ui, id("win2")).w > 100.f); // the default rect
    // graphics view: the document owns the scene
    const NodeHandle view = id("view");
    w::GraphicsScene* sc = w::graphicsScene(ui, view);
    UI_CHECK(sc && sc->size() == 4 && w::viewZoom(ui, view) == 2.f);
    if (sc && sc->size() == 4) {
        UI_CHECK(sc->item(0)->text == "box" && sc->item(0)->fill.r > 0.9f && sc->item(0)->fill.g < 0.1f);
        UI_CHECK(sc->item(1)->shape == w::GraphicsShape::Ellipse && !sc->item(1)->movable);
        UI_CHECK(sc->item(2)->points.size() == 3 && !sc->item(2)->selectable && sc->item(3)->z == 2.f && sc->item(3)->strokeWidth == 3.f);
        const Vec2 p = w::sceneToView(ui, view, {20, 20});
        ui.pointerMove(p);
        ui.pointerDown(p);
        ui.pointerUp(p);
        UI_CHECK(sc->item(0)->selected);
    }
    // keyboard and button box
    UI_CHECK(w::virtualKeyboardKey(ui, id("kb"), "Enter").valid());
    const NodeHandle buttons = id("buttons");
    std::string order;
    for (NodeHandle b : w::dialogButtonBoxButtons(ui, buttons)) order += std::string(ui.text(b)) + ",";
    UI_CHECK_EQ(order, std::string("Apply,Cancel,OK,")); // the mac order
    UI_CHECK(w::dialogButtonBoxDefault(ui, buttons) == w::dialogButtonBoxButton(ui, buttons, w::StdButton::Ok));
}
} // namespace

int main() {
    UiContext ui({.viewport = {900, 900}, .font = &font()});
    w::install(ui);
    Model model;
    const u32 base = ui.nodeCount();
    {
        auto doc = Document::load(ui, kDoc, &model);
        if (!doc) std::fprintf(stderr, "load: %s: %s\n", doc.error().path.c_str(), doc.error().message.c_str());
        UI_CHECK(doc.has_value());
        if (!doc) return finish("fuse_ui_port_doc");
        ui.update();
        checkTree(ui);
        ui.update();
    }
    // the document is gone: so are its nodes (and with them the scene and option records are unreachable)
    ui.setFocus({});
    ui.update();
    UI_CHECK(!ui.alive(ui.find("view")) && !ui.alive(ui.find("pw")));
    UI_CHECK(ui.nodeCount() <= base + 2); // (the overlay layer may stay)
    // loaded again into the same context: fresh state, same behaviour
    {
        auto doc = Document::load(ui, kDoc, &model);
        UI_CHECK(doc.has_value());
        if (doc) {
            ui.update();
            checkTree(ui);
        }
    }
    // errors stay errors: a sub-window outside an MDI area, an option on the wrong class
    UI_CHECK(!Document::load(ui, R"({"fuseui":1,"root":{"class":"mdiwindow","text":"alone"}})", &model));
    UI_CHECK(!Document::load(ui, R"({"fuseui":1,"root":{"class":"button","echo":"password"}})", &model));
    UI_CHECK(!Document::load(ui, R"({"fuseui":1,"root":{"class":"label","closable":true}})", &model));
    return finish("fuse_ui_port_doc");
}
