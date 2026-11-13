// fuse_ui_doc_catalogue: every catalogue widget is creatable from a .fuseui document by class name (registered
// classes and the document classes of doc_classes.cpp), with its properties; the cooker's registration list covers
// what those documents build (compound widgets, popups created later) and a context with only that list loads them.
// `--gate=<name>` runs one group (ctest fuse_ui_doc_<name>). The catalogue document is the widget_gallery sample's.
#include "fuse/ui/doc/cook.hpp"
#include "fuse/ui/icons.hpp"
#include "fuse/ui/widgets/modelview.hpp"
#include "fuse/ui/widgets/platform_feel.hpp"
#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using namespace fuse::ui;
using namespace fuse::ui::test;
using namespace fuse::ui::doc;
namespace w = fuse::ui::widgets;

namespace {
std::string gallery() {
    std::string s;
    if (!readTextFile(FUSE_UI_GALLERY_DOC, s)) {
        std::fprintf(stderr, "missing %s\n", FUSE_UI_GALLERY_DOC);
        std::exit(2);
    }
    return s;
}
void seed(Model& m) {
    m.set("clicks", 0.0);
    m.set("flag", 1.0);
    m.set("level", 40.0);
    m.set("name", "Odin");
    m.set("stackPage", 0.0);
    m.set("page", 0.0);
    m.set("dialogOpen", 0.0);
}
bool has(const std::vector<std::string>& v, std::string_view s) { return std::find(v.begin(), v.end(), s) != v.end(); }
std::string_view classOf(const UiContext& ui, NodeHandle n) { return ui.registry().get(ui.classOf(n)).name; }
bool inside(const UiContext& ui, NodeHandle n, NodeHandle ancestor) {
    for (; ui.alive(n); n = ui.parent(n))
        if (n == ancestor) return true;
    return false;
}

// ---- the catalogue document: every class, its properties ------------------------------------------------------------
void gateCatalogue() {
    UiContext ui({.viewport = {1100, 760}, .font = &font()});
    w::install(ui);
    const u32 base = ui.nodeCount();
    Model model;
    seed(model);
    {
        auto loaded = Document::load(ui, gallery(), &model);
        if (!loaded) std::fprintf(stderr, "gallery: %s: %s\n", loaded.error().path.c_str(), loaded.error().message.c_str());
        UI_CHECK(loaded.has_value());
        if (!loaded) return;
        Document& doc = **loaded;
        ui.update();
        auto id = [&](std::string_view s) { return ui.find(s); };
        // every class of the catalogue is in the document, under the registered class its factory gives it
        static constexpr std::pair<std::string_view, std::string_view> kWant[] = {
            {"g.button", "button"}, {"g.toolbutton", "toolbutton"}, {"g.link", "link"}, {"g.checkbox", "checkbox"},
            {"g.switch", "switch"}, {"g.radio1", "radio"}, {"g.slider", "slider"}, {"g.progress", "progressbar"},
            {"g.dial", "dial"}, {"g.spinbox", "spinbox"}, {"g.doublespinbox", "spinbox"}, {"g.combobox", "combobox"},
            {"g.scrollbar", "scrollbar"}, {"g.lineedit", "lineedit"}, {"g.image", "image"}, {"g.tabbar", "tabbar"},
            {"g.listview", "listview"}, {"g.treeview", "treeview"}, {"g.modellistview", "tableview"}, {"g.header", "header"},
            {"g.tableview", "tableview"}, {"g.gridview", "gridview"}, {"g.textedit", "textedit"},
            {"g.plaintextedit", "codeedit"}, {"g.codeedit", "codeedit"}, {"g.richtext", "richtext"},
            {"g.colorpicker", "colorpicker"}, {"g.calendar", "calendar"}, {"g.datetimeedit", "datetimeedit"},
            {"g.chart", "chart"}, {"g.flame", "chart"}, {"g.nodegraph", "nodegraph"}, {"g.curveeditor", "curveeditor"},
            {"g.timeline", "timeline"}, {"g.viewport", "viewport"}, {"g.video", "video"}, {"g.kineticscroll", "kineticscroll"},
            {"menubar", "menubar"}, {"menu.file", "menubutton"},
            // compound widgets: a panel that carries the widget
            {"g.groupbox", "panel"}, {"g.stack", "panel"}, {"g.splitter", "panel"}, {"g.scrollarea", "panel"},
            {"g.statusbar", "panel"}, {"pages", "panel"}, {"g.dockarea", "panel"}, {"dock.outline", "panel"},
            {"g.propertygrid", "panel"}, {"g.markdown", "panel"}, {"g.fontpicker", "panel"}, {"toolbar", "panel"},
            {"g.dialog", "popup"}}; // a dialog's document node is its popup scope
        for (const auto& [nodeId, cls] : kWant) {
            const NodeHandle n = id(nodeId);
            if (!ui.alive(n) || classOf(ui, n) != cls)
                std::fprintf(stderr, "  %.*s: want %.*s, got %s\n", int(nodeId.size()), nodeId.data(), int(cls.size()), cls.data(),
                             ui.alive(n) ? std::string(classOf(ui, n)).c_str() : "(missing)");
            UI_CHECK(ui.alive(n) && classOf(ui, n) == cls);
        }
        // values and ranges
        UI_CHECK(ui.value(id("g.slider")) == 40.f && ui.rangeMax(id("g.slider")) == 100.f && ui.value(id("g.progress")) == 40.f);
        UI_CHECK(std::abs(ui.rangeStep(id("g.doublespinbox")) - 0.05f) < 1e-5f && ui.value(id("g.doublespinbox")) == 0.25f);
        UI_CHECK(w::comboText(ui, id("g.combobox")) == "CPU raster" && ui.checked(id("g.checkbox")) && ui.checked(id("g.switch")));
        UI_CHECK(ui.text(id("g.lineedit")) == "Odin" && ui.editBuffer(id("g.lineedit")).text() == "Odin");
        // item views
        UI_CHECK(w::listItem(ui, id("g.listview"), 9) == "List item 9" && ui.value(id("g.listview")) == 1.f);
        UI_CHECK(ui.rangeMin(id("g.listview")) == -1.f); // the list keeps "no selection"
        UI_CHECK_EQ(w::treeVisibleRows(ui, id("g.treeview")), 7u);
        UI_CHECK_EQ(w::headerSectionCount(ui, id("g.header")), 3u);
        std::vector<A11ySnapshotNode> snap;
        ui.setValue(w::tabWidgetBar(ui, id("pages")), 2.f, true); // the Views page
        ui.update();
        ui.snapshotA11y(snap);
        bool cell = false, tile = false, modelRow = false;
        for (const A11ySnapshotNode& s : snap) {
            cell = cell || (s.node == id("g.tableview") && s.role == A11yRole::Cell && s.name == "1204");
            tile = tile || (s.node == id("g.gridview") && s.role == A11yRole::ListItem && s.name == "Tile 0");
            modelRow = modelRow || (s.node == id("g.modellistview") && s.name == "Model row 0");
        }
        UI_CHECK(cell && tile && modelRow); // the document's inline data reaches the views through its models
        UI_CHECK(w::tableColumnWidth(ui, id("g.tableview"), 2) > 0.f && w::gridColumns(ui, id("g.gridview")) >= 2u);
        UI_CHECK(w::propertyCount(ui, id("g.propertygrid")) == 7u && w::propertyValue(ui, id("g.propertygrid"), 2) == "1.5");
        UI_CHECK(w::propertyValue(ui, id("g.propertygrid"), 4) == "brushed steel" && w::propertyValue(ui, id("g.propertygrid"), 1) == "true");
        // text widgets
        UI_CHECK(w::textEditText(ui, id("g.textedit")).find("second paragraph") != std::string::npos);
        UI_CHECK(w::codeEditLineCount(ui, id("g.codeedit")) == 5u && w::codeEditTokenAt(ui, id("g.codeedit"), 1, 0) == 1); // "local": Lua
        UI_CHECK(w::codeEditLineCount(ui, id("g.plaintextedit")) == 3u && w::codeEditTokenAt(ui, id("g.plaintextedit"), 0, 0) == 0);
        UI_CHECK(w::richTextLinkCount(ui, id("g.richtext")) == 1u && w::richTextLink(ui, id("g.richtext"), 0) == "docs/ui");
        UI_CHECK(ui.childCount(id("g.markdown")) >= 5u && w::fontPickerSize(ui, id("g.fontpicker")) == 16.f);
        // pro widgets
        const Color c = w::colorPickerColor(ui, id("g.colorpicker"));
        UI_CHECK(std::abs(c.r - 1.f) < 0.02f && std::abs(c.g - 0x88 / 255.f) < 0.02f && c.b < 0.02f);
        int y = 0, mo = 0, d = 0, h = 0, mi = 0;
        w::calendarDate(ui, id("g.calendar"), y, mo, d);
        UI_CHECK(y == 2026 && mo == 11 && d == 12);
        w::dateTimeEditValue(ui, id("g.datetimeedit"), y, mo, d, h, mi);
        UI_CHECK(y == 2026 && mo == 11 && d == 12 && h == 9 && mi == 30);
        UI_CHECK(w::chartSeriesCount(ui, id("g.chart")) == 2u && w::chartFlameSpanCount(ui, id("g.flame")) == 4u);
        UI_CHECK(w::nodeGraphNodeCount(ui, id("g.nodegraph")) == 2u && w::nodeGraphLinkCount(ui, id("g.nodegraph")) == 1u);
        UI_CHECK(w::curveKeyCount(ui, id("g.curveeditor")) == 3u && std::abs(w::curveKey(ui, id("g.curveeditor"), 1).y - 0.9f) < 1e-4f);
        UI_CHECK(w::timelineActiveClips(ui, id("g.timeline"), 2.f) == 2u && w::timelineClip(ui, id("g.timeline"), 0, 1).x == 6.f);
        UI_CHECK(w::videoImage(ui, id("g.video")) != nullptr && w::kineticScroller(ui, id("g.kineticscroll")) != nullptr);
        // containers put the document's children where they belong
        UI_CHECK(inside(ui, id("g.button"), w::tabPage(ui, id("pages"), 0)) && inside(ui, id("g.listview"), w::tabPage(ui, id("pages"), 2)));
        UI_CHECK(ui.childCount(w::groupBoxBody(ui, id("g.groupbox"))) == 2u && ui.text(ui.firstChild(id("g.groupbox"))) == "Group box");
        UI_CHECK_EQ(w::stackCount(ui, id("g.stack")), 3u);
        UI_CHECK(ui.parent(id("dock.outline")) == w::dockRegion(ui, id("g.dockarea"), w::DockSide::Left));
        UI_CHECK(ui.parent(id("dock.props")) == w::dockRegion(ui, id("g.dockarea"), w::DockSide::Right));
        UI_CHECK(ui.parent(id("dock.log")) == w::dockRegion(ui, id("g.dockarea"), w::DockSide::Bottom));
        UI_CHECK(ui.parent(id("g.central")) == w::dockRegion(ui, id("g.dockarea"), w::DockSide::Center));
        UI_CHECK(ui.text(ui.firstChild(id("dock.props"))) == "Properties" && ui.childCount(w::dockBody(ui, id("dock.log"))) == 1u);
        UI_CHECK(w::statusText(ui, id("g.statusbar")) == "Status bar message");
        UI_CHECK(ui.text(id("menu.file")).starts_with("File\nNew\nOpen") && ui.childCount(id("g.splitter")) == 3u); // pane, handle, pane
        // bindings drive the compound widgets: the stack page, the current tab, the dialog
        model.set("stackPage", 2.0);
        model.set("page", 4.0);
        doc.refresh();
        ui.update();
        UI_CHECK(w::stackIndex(ui, id("g.stack")) == 2u && ui.value(id("g.tabbar")) == 2.f);
        UI_CHECK(ui.value(w::tabWidgetBar(ui, id("pages"))) == 4.f && ui.computed(w::tabPage(ui, id("pages"), 4)).visible);
        UI_CHECK(!ui.computed(w::tabPage(ui, id("pages"), 0)).visible);
        const NodeHandle dlg = id("g.dialog");
        UI_CHECK(!w::popupOpen(ui, dlg) && ui.role(ui.firstChild(dlg)) == A11yRole::Dialog);
        model.set("dialogOpen", 1.0);
        doc.refresh();
        ui.update();
        UI_CHECK(w::popupOpen(ui, dlg) && inside(ui, id("g.dialog.close"), w::dialogBody(ui, ui.firstChild(dlg))));
        model.set("dialogOpen", 0.0);
        doc.refresh();
        ui.update();
        UI_CHECK(!w::popupOpen(ui, dlg));
        // two-way: the slider writes the model, the label follows
        ui.setValue(id("g.slider"), 75.f, true);
        doc.refresh();
        UI_CHECK(model.number("level") == 75.0 && ui.value(id("g.dial")) == 75.f);
        // actions: a menu entry, a rich-text link
        std::string action;
        f32 value = -1.f;
        doc.onAction([&](std::string_view a, NodeHandle, const SignalArgs& args) { action = std::string(a); value = args.value; });
        ui.emit(id("menu.file"), SignalKind::Submitted, {.value = 1.f, .text = "Open"});
        UI_CHECK(action == "menu" && value == 1.f);
        // the context-menu property: a right press on the tool button opens its menu
        model.set("page", 0.0);
        doc.refresh();
        ui.update();
        const u32 before = ui.nodeCount();
        const Rect tr = ui.rect(id("g.toolbutton"));
        ui.pointerMove({tr.x + 8.f, tr.y + 8.f});
        ui.pointerDown({tr.x + 8.f, tr.y + 8.f}, MouseButton::Right);
        ui.pointerUp({tr.x + 8.f, tr.y + 8.f}, MouseButton::Right);
        ui.update();
        UI_CHECK(ui.nodeCount() > before);
        ui.keyDown(Key::Escape);
        ui.update();
        ui.update();
        // the whole thing paints
        UI_CHECK(ui.drawList().size() > 50);
    }
    // the document is gone: so is everything it built, the dialog in the overlay layer included
    ui.update();
    ui.update();
    u32 left = 0;
    for (u32 i = 0; i < 8192; ++i) {
        const NodeHandle h = ui.handleOf(i);
        if (!ui.alive(h)) continue;
        const std::string_view cls = classOf(ui, h);
        left += cls != "root" && cls != "layer" && cls != "tooltip" && !ui.hasClass(h, "toasthost") &&
                !(cls == "label" && ui.parent(h) == w::overlayLayer(ui)); // (tooltips / context-menu holders stay with the layer)
    }
    UI_CHECK_EQ(left, 0u);
    (void)base;
}

// ---- every name is creatable on its own -----------------------------------------------------------------------------
void gateEveryClass() {
    UiContext ui({.viewport = {640, 480}, .font = &font()});
    w::install(ui);
    std::vector<std::string> names = documentClassNames();
    for (u32 i = 0; i < ui.registry().size(); ++i) names.push_back(ui.registry().get(i).name);
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    UI_CHECK(names.size() > 60);
    for (std::string_view want : {"groupbox", "tabwidget", "stack", "splitter", "dockarea", "dock", "dialog", "mainwindow", "scrollarea",
                                  "toolbar", "statusbar", "propertygrid", "markdown", "fontpicker", "modellistview", "doublespinbox",
                                  "plaintextedit", "tableview", "gridview", "codeedit", "chart", "calendar", "colorpicker", "timeline"})
        UI_CHECK(isDocumentClass(want));
    Model model;
    u32 made = 0;
    for (const std::string& name : names) {
        if (name == "root" || name == "layer") continue; // the context's own
        // a dock lives in a dock area; everything else stands alone
        const std::string json = name == "dock"
            ? "{\"fuseui\":1,\"root\":{\"class\":\"dockarea\",\"children\":[{\"class\":\"dock\",\"id\":\"it\",\"text\":\"D\"}]}}"
            : name == "mdiwindow" // an MDI sub-window lives in an MDI area
            ? "{\"fuseui\":1,\"root\":{\"class\":\"mdiarea\",\"children\":[{\"class\":\"mdiwindow\",\"id\":\"it\",\"text\":\"W\"}]}}"
            : "{\"fuseui\":1,\"root\":{\"class\":\"" + name + "\",\"id\":\"it\"}}";
        auto doc = Document::load(ui, json, &model);
        if (!doc) std::fprintf(stderr, "  %s: %s\n", name.c_str(), doc.error().message.c_str());
        UI_CHECK(doc.has_value());
        if (!doc) continue;
        UI_CHECK(ui.alive(ui.find("it")));
        ui.update(1.f / 60.f); // it lays out and paints without its optional properties
        ++made;
    }
    UI_CHECK(made + 2 >= names.size());
    // errors stay errors
    auto bad = Document::load(ui, "{\"fuseui\":1,\"root\":{\"class\":\"no-such-widget\"}}", &model);
    UI_CHECK(!bad && bad.error().message.find("unknown class") != std::string::npos);
    auto stray = Document::load(ui, "{\"fuseui\":1,\"root\":{\"class\":\"dock\",\"text\":\"alone\"}}", &model);
    UI_CHECK(!stray); // a dock outside a dock area
    auto over = Document::load(ui, "{\"fuseui\":1,\"root\":{\"class\":\"tabwidget\",\"tabs\":[\"A\"],\"children\":[{},{}]}}", &model);
    UI_CHECK(!over && over.error().path.find("children[1]") != std::string::npos); // more children than tabs
    auto prop = Document::load(ui, "{\"fuseui\":1,\"root\":{\"class\":\"button\",\"tabs\":[\"A\"]}}", &model);
    UI_CHECK(!prop && prop.error().message.find("unknown property") != std::string::npos); // class properties stay with their class

    // an app's own document class
    static int props = 0;
    registerDocumentClass({"meter",
                           [](UiContext& u, NodeHandle parent, std::string_view id, const JsonValue& n) {
                               const NodeHandle m = w::progressBar(u, parent, 0.f, id);
                               if (const JsonValue* v = n.get("units"); v && v->isString()) u.setA11y(m, A11yRole::ProgressBar, v->string);
                               return m;
                           },
                           [](UiContext&, NodeHandle, std::string_view key, const JsonValue&) { props += key == "units"; return key == "units"; },
                           nullptr});
    UI_CHECK(isDocumentClass("meter"));
    auto custom = Document::load(ui, "{\"fuseui\":1,\"root\":{\"class\":\"meter\",\"id\":\"m\",\"units\":\"dB\",\"value\":0.5}}", &model);
    UI_CHECK(custom.has_value() && props == 1 && ui.value(ui.find("m")) == 0.5f && classOf(ui, ui.find("m")) == "progressbar");
}

// ---- hot reload with compound widgets -------------------------------------------------------------------------------
void gateReload() {
    UiContext ui({.viewport = {800, 600}, .font = &font()});
    w::install(ui);
    Model model;
    const std::string v1 =
        "{\"fuseui\":1,\"root\":{\"id\":\"root\",\"children\":["
        "{\"class\":\"groupbox\",\"id\":\"box\",\"text\":\"Audio\",\"children\":[{\"class\":\"lineedit\",\"id\":\"edit\",\"text\":\"seed\"},"
        "{\"class\":\"slider\",\"id\":\"vol\",\"value\":0.3}]},"
        "{\"class\":\"tabwidget\",\"id\":\"tabs\",\"tabs\":[\"A\",\"B\"],\"children\":[{\"class\":\"label\",\"text\":\"a\"},{\"class\":\"label\",\"text\":\"b\"}]},"
        "{\"class\":\"dialog\",\"id\":\"dlg\",\"text\":\"Sure?\",\"children\":[{\"class\":\"button\",\"id\":\"yes\",\"text\":\"Yes\"}]}]}}";
    auto doc = Document::load(ui, v1, &model);
    UI_CHECK(doc.has_value());
    if (!doc) return;
    ui.update();
    const NodeHandle edit = ui.find("edit"), vol = ui.find("vol");
    ui.setValue(vol, 0.8f);
    ui.setFocus(edit);
    ui.textInput("X");
    ui.update();
    const u32 nodes = ui.nodeCount();
    // the same document again: nothing leaks (compound widgets and the dialog are rebuilt, not duplicated) ...
    for (int i = 0; i < 3; ++i) {
        UI_CHECK((*doc)->reload(v1).has_value());
        ui.update();
        ui.update();
        UI_CHECK_EQ(ui.nodeCount(), nodes);
    }
    // ... and plain widgets with an id inside them keep their identity and state
    UI_CHECK(ui.find("edit") == edit && ui.find("vol") == vol && ui.value(vol) == 0.8f);
    UI_CHECK(ui.text(edit).find('X') != std::string_view::npos && inside(ui, edit, w::groupBoxBody(ui, ui.find("box"))));
    UI_CHECK(ui.text(ui.firstChild(ui.find("box"))) == "Audio" && ui.childCount(w::dialogBody(ui, ui.firstChild(ui.find("dlg")))) == 1u);
    // a changed document: the group box gets a new title and a third tab appears
    std::string v2 = v1;
    v2.replace(v2.find("Audio"), 5, "Sound");
    v2.replace(v2.find("[\"A\",\"B\"]"), 9, "[\"A\",\"B\",\"C\"]");
    UI_CHECK((*doc)->reload(v2).has_value());
    ui.update();
    UI_CHECK(ui.text(ui.firstChild(ui.find("box"))) == "Sound" && w::tabPage(ui, ui.find("tabs"), 2).valid());
    UI_CHECK(ui.find("edit") == edit);
    doc->reset();
    ui.update();
    ui.update();
    UI_CHECK(!ui.alive(edit) && !ui.alive(ui.find("dlg")));
}

// ---- cooker: registration list for documents with compound widgets --------------------------------------------------
void gateCooked() {
    CookInfo info;
    auto cooked = cookDocument(gallery(), &info);
    if (!cooked) std::fprintf(stderr, "cook: %s: %s\n", cooked.error().path.c_str(), cooked.error().message.c_str());
    UI_CHECK(cooked.has_value());
    if (!cooked) return;
    UI_CHECK(has(info.classes, "groupbox") && has(info.classes, "tabwidget") && has(info.classes, "dock") && has(info.classes, "tooltip"));
    const std::vector<std::string> list = registrationList({info.classes});
    // document-only names are resolved to what they build; registered classes stay
    for (std::string_view gone : {"groupbox", "tabwidget", "stack", "splitter", "dockarea", "dock", "dialog", "scrollarea", "toolbar",
                                  "statusbar", "propertygrid", "markdown", "fontpicker", "modellistview", "doublespinbox", "plaintextedit"})
        UI_CHECK(!has(list, gone));
    for (std::string_view kept : {"panel", "label", "button", "checkbox", "groupbody", "tabbar", "splithandle", "docktitle", "docktabs",
                                  "dockdrop", "popup", "menu", "menuitem", "menubutton", "menubar", "tooltip", "tableview", "gridview",
                                  "codeedit", "spinbox", "listview", "header", "textedit", "richtext", "colorpicker", "calendar",
                                  "datetimeedit", "chart", "nodegraph", "curveeditor", "timeline", "viewport", "video", "kineticscroll",
                                  "toolbutton", "separator", "lineedit", "combobox", "treeview", "scrollbar", "dial", "slider"})
        if (!has(list, kept)) {
            std::fprintf(stderr, "  registration list lacks %.*s\n", int(kept.size()), kept.data());
            UI_CHECK(false);
        }
    // a class of another library passes through (the app registers it); nothing is dropped silently
    const std::vector<std::string> withGame = registrationList({{"minimap", "ammocounter", "groupbox"}});
    UI_CHECK(has(withGame, "minimap") && has(withGame, "ammocounter") && has(withGame, "groupbody") && !has(withGame, "groupbox"));
    const std::string src = registrationSource(withGame);
    UI_CHECK(src.find("\"minimap\"") != std::string::npos && src.find("missing") != std::string::npos);

    // a context with only the listed classes loads the cooked document and runs it
    UiContext ui({.viewport = {1100, 760}, .font = &font()});
    std::vector<std::string_view> views(list.begin(), list.end());
    UI_CHECK_EQ(w::installClasses(ui, views), 0u);
    UiContext full({.viewport = {10, 10}, .font = &font()});
    w::install(full);
    UI_CHECK(ui.registry().size() <= full.registry().size());
    UI_CHECK(ui.registry().find("toast") == 0xFFFFFFFFu && ui.registry().find("wizardpage") == 0xFFFFFFFFu); // not in the document
    Model model;
    seed(model);
    const u64 compiles = Expr::compileCount();
    auto doc = Document::loadCooked(ui, *cooked, &model);
    if (!doc) std::fprintf(stderr, "cooked: %s: %s\n", doc.error().path.c_str(), doc.error().message.c_str());
    UI_CHECK(doc.has_value());
    if (!doc) return;
    UI_CHECK_EQ(Expr::compileCount(), compiles); // no expression parsing on the cooked path
    ui.update();
    UI_CHECK(w::stackCount(ui, ui.find("g.stack")) == 3u && w::propertyCount(ui, ui.find("g.propertygrid")) == 7u);
    // the pieces created later are registered too: a combo popup, a dock drag (drop layer), the dialog
    const Rect cb = ui.rect(ui.find("g.combobox"));
    const u32 before = ui.nodeCount();
    ui.pointerMove({cb.x + 5.f, cb.y + 5.f});
    ui.pointerDown({cb.x + 5.f, cb.y + 5.f});
    ui.pointerUp({cb.x + 5.f, cb.y + 5.f});
    ui.update();
    UI_CHECK(ui.nodeCount() > before);
    ui.keyDown(Key::Escape);
    ui.update();
    ui.update();
    model.set("page", 5.0);
    (*doc)->refresh();
    ui.update();
    const Rect t = ui.rect(ui.firstChild(ui.find("dock.outline")));
    ui.pointerMove({t.x + 30.f, t.y + t.h * 0.5f});
    ui.pointerDown({t.x + 30.f, t.y + t.h * 0.5f});
    ui.pointerMove({t.x + 300.f, t.y + 200.f});
    ui.update();
    UI_CHECK(w::dockDragged(ui) == ui.find("dock.outline"));
    ui.pointerUp({t.x + 300.f, t.y + 200.f});
    ui.update();
    w::dockTabify(ui, ui.find("dock.outline"), ui.find("dock.history"));
    ui.update();
    UI_CHECK(w::dockTabBar(ui, ui.find("dock.history")).valid());
    model.set("dialogOpen", 1.0);
    (*doc)->refresh();
    ui.update();
    UI_CHECK(w::popupOpen(ui, ui.find("g.dialog")));
}
// ---- classes that came with model / view and vector drawing: tree model view, svg image, icons on buttons ----------
void gateMergedClasses() {
    const std::string json = R"({ "fuseui": 1, "root": { "class": "panel", "children": [
        { "class": "treemodelview", "id": "tree", "columns": ["Name", "Kind"], "editable": true,
          "items": ["Scene\tgroup", "\tCamera\tcamera", "\tLights\tgroup", "\t\tSun\tlight", "Assets\tgroup", "\tTextures\tfolder"],
          "expanded": true, "contextMenu": ["Rename", "Delete"] },
        { "class": "treemodelview", "id": "plain", "items": ["Root", "\tLeaf"] },
        { "class": "svgimage", "id": "logo", "icon": "folder", "style": { "width": 32, "height": 32 } },
        { "class": "svgimage", "id": "inline", "svg": "<svg viewBox='0 0 8 8'><rect width='8' height='8' fill='#f00'/></svg>" },
        { "class": "button", "id": "save", "text": "Save", "icon": "save" },
        { "class": "toolbutton", "id": "undo", "icon": "undo", "a11yName": "Undo" }
    ] } })";
    UiContext ui({.viewport = {640, 480}, .font = &font()});
    w::install(ui);
    Model model;
    auto loaded = Document::load(ui, json, &model);
    if (!loaded) std::fprintf(stderr, "merged: %s: %s\n", loaded.error().path.c_str(), loaded.error().message.c_str());
    UI_CHECK(loaded.has_value());
    if (!loaded) return;
    ui.update();
    const NodeHandle tree = ui.find("tree"), plain = ui.find("plain");
    UI_CHECK(classOf(ui, tree) == "treemodelview" && classOf(ui, plain) == "treemodelview");
    // the inline tree: every node expanded, two columns, cells in their columns, depth from the leading tabs
    UI_CHECK_EQ(w::viewRowCount(ui, tree), 6u);
    UI_CHECK(w::treeRowDepth(ui, tree, 3) == 2u && w::treeRowExpanded(ui, tree, 0) && w::treeRowExpanded(ui, tree, 2));
    const w::ItemModel* m = w::tableModel(ui, tree);
    UI_CHECK(m && m->columns == 2u);
    std::string scratch;
    if (m) UI_CHECK(m->itemData(m->user, w::treeItemAt(ui, tree, 3), 0, scratch) == "Sun" && m->itemData(m->user, w::treeItemAt(ui, tree, 3), 1, scratch) == "light");
    UI_CHECK(w::viewEdit(ui, tree, 1, 1)); // "editable": true
    w::viewCancelEdit(ui, tree);
    UI_CHECK(w::viewRowCount(ui, plain) == 1u && !w::treeRowExpanded(ui, plain, 0) && !w::viewEdit(ui, plain, 0, 0));
    // svg images and icons
    Icon folder{}, save{};
    UI_CHECK(findIcon("folder", folder) && findIcon("save", save));
    UI_CHECK(w::svgImageOf(ui, ui.find("logo")) == w::iconHandle(folder) && ui.rect(ui.find("logo")).w == 32.f);
    UI_CHECK(w::svgGet(w::svgImageOf(ui, ui.find("inline"))) != nullptr && ui.role(ui.find("logo")) == A11yRole::Image);
    UI_CHECK(w::iconOf(ui, ui.find("save")) == w::iconHandle(save) && w::iconOf(ui, ui.find("undo")).valid());
    // unknown icon names and icons on other classes are errors, not silence
    auto bad = Document::load(ui, "{\"fuseui\":1,\"root\":{\"class\":\"button\",\"icon\":\"no-such-icon\"}}", &model);
    UI_CHECK(!bad && bad.error().message.find("unknown property") != std::string::npos);
    auto wrong = Document::load(ui, "{\"fuseui\":1,\"root\":{\"class\":\"label\",\"icon\":\"save\"}}", &model);
    UI_CHECK(!wrong);

    // cooked: the same document, a registration list that covers what the views create later, a trimmed context
    CookInfo info;
    auto cooked = cookDocument(json, &info);
    UI_CHECK(cooked.has_value());
    if (!cooked) return;
    const std::vector<std::string> list = registrationList({info.classes});
    for (std::string_view kept : {"treemodelview", "svgimage", "button", "toolbutton", "tooltip", "lineedit", "menu", "menuitem", "popup"})
        if (!has(list, kept)) {
            std::fprintf(stderr, "  registration list lacks %.*s\n", int(kept.size()), kept.data());
            UI_CHECK(false);
        }
    UiContext trimmed({.viewport = {640, 480}, .font = &font()});
    std::vector<std::string_view> views(list.begin(), list.end());
    UI_CHECK_EQ(w::installClasses(trimmed, views), 0u);
    UI_CHECK(trimmed.registry().find("tableview") == 0xFFFFFFFFu); // only what the document needs
    auto doc = Document::loadCooked(trimmed, *cooked, &model);
    if (!doc) std::fprintf(stderr, "merged cooked: %s: %s\n", doc.error().path.c_str(), doc.error().message.c_str());
    UI_CHECK(doc.has_value());
    if (!doc) return;
    trimmed.update();
    const NodeHandle ct = trimmed.find("tree");
    UI_CHECK(w::viewRowCount(trimmed, ct) == 6u && w::iconOf(trimmed, trimmed.find("save")).valid());
    UI_CHECK(w::viewEdit(trimmed, ct, 0, 0) && trimmed.alive(w::viewEditor(trimmed, ct))); // the editor's class is registered
    w::viewCancelEdit(trimmed, ct);
    // a cooked blob of another format version is refused (the version moves with the property set)
    std::vector<u8> old(cooked->begin(), cooked->end());
    UI_CHECK(kCookedVersion == 5u && old.size() > 8 && old[4] == 5u);
    for (u8 v : {u8(3), u8(4), u8(6)}) { // 4: the style engine port alone, before the text / icon properties
        old[4] = v;
        UI_CHECK(!Document::loadCooked(trimmed, old, &model).has_value());
    }
}
} // namespace

int main(int argc, char** argv) {
    struct Gate { const char* name; void (*fn)(); };
    static const Gate kGates[] = {{"all_classes", &gateCatalogue}, {"every_class", &gateEveryClass}, {"compound_reload", &gateReload},
                                  {"cooked_catalogue", &gateCooked}, {"merged_classes", &gateMergedClasses}};
    const char* only = nullptr;
    for (int i = 1; i < argc; ++i)
        if (std::strncmp(argv[i], "--gate=", 7) == 0) only = argv[i] + 7;
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
    return finish("fuse_ui_doc_catalogue");
}
