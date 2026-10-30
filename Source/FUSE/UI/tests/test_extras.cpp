// Qt-parity extras gates (--gate=<name>; no argument runs all): undo gesture i18n desktop pdf containers dialogs
// lcd_keyseq mdi completer graphics widget_pdf. FUSE_EXTRAS_OUT=<dir> writes extras.png / extras.pdf.
#include "test_util.hpp"
#include <atomic>
#include <cstdlib>
#include <thread>
#include <unistd.h>
#include "fuse/ui/extras/dbus.hpp"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include "fuse/ui/extras/extras.hpp"
#include "fuse/ui/os.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <string>

using namespace fuse::ui;
namespace pdf = fuse::ui::pdf;

namespace {
int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

void gateUndo() {
    UndoStack st;
    int v = 0, changes = 0;
    st.onChanged = [&] { ++changes; };
    st.push("add 1", [&] { v += 1; }, [&] { v -= 1; });
    st.push("add 10", [&] { v += 10; }, [&] { v -= 10; });
    CHECK(v == 11 && st.count() == 2 && st.undoText() == "add 10");
    st.undo();
    CHECK(v == 1 && st.redoText() == "add 10" && st.canRedo());
    st.push("add 100", [&] { v += 100; }, [&] { v -= 100; }); // drops redo tail
    CHECK(v == 101 && st.count() == 2 && !st.canRedo());
    st.setClean();
    CHECK(st.isClean());
    st.undo();
    CHECK(!st.isClean());
    st.redo();
    CHECK(st.isClean());
    st.setIndex(0);
    CHECK(v == 0 && !st.canUndo());
    st.setIndex(2);
    CHECK(v == 101);
    CHECK(changes > 0);

    // merging (typing compression)
    struct Type final : UndoCommand {
        std::string& doc; std::string s;
        Type(std::string& d, std::string t) : UndoCommand("typing"), doc(d), s(std::move(t)) {}
        void redo() override { doc += s; }
        void undo() override { doc.resize(doc.size() - s.size()); }
        int id() const override { return 7; }
        bool mergeWith(const UndoCommand& o) override { s += static_cast<const Type&>(o).s; return true; }
    };
    std::string doc;
    UndoStack ts;
    for (const char* c : {"h", "e", "l", "l", "o"}) ts.push(std::make_unique<Type>(doc, c));
    CHECK(doc == "hello" && ts.count() == 1);
    ts.undo();
    CHECK(doc.empty());
    ts.redo();
    // macro
    ts.beginMacro("bulk");
    ts.push("a", [&] { doc += "A"; }, [&] { doc.pop_back(); });
    ts.push("b", [&] { doc += "B"; }, [&] { doc.pop_back(); });
    CHECK(!ts.canUndo()); // inside a macro
    ts.endMacro();
    CHECK(doc == "helloAB" && ts.count() == 2 && ts.undoText() == "bulk");
    ts.undo();
    CHECK(doc == "hello");
    // limit
    UndoStack ls;
    CHECK(ls.setUndoLimit(3));
    int x = 0;
    for (int i = 0; i < 5; ++i) ls.push("i", [&] { ++x; }, [&] { --x; });
    CHECK(ls.count() == 3 && x == 5);
    ls.setIndex(0);
    CHECK(x == 2);
    CHECK(!ls.setUndoLimit(1));
    // group
    UndoGroup g;
    g.setActiveStack(&ls);
    CHECK(g.canRedo() && !g.canUndo());
    g.redo();
    CHECK(x == 3);
}

void gateGesture() {
    GestureRecognizer r;
    auto has = [](const std::vector<Gesture>& v, GestureType t, GestureState s) {
        for (auto& g : v) if (g.type == t && g.state == s) return true;
        return false;
    };
    // tap, then double tap
    r.feed({1, {100, 100}, TouchPhase::Down}, 0.0);
    auto o = r.feed({1, {102, 101}, TouchPhase::Up}, 0.1);
    CHECK(has(o, GestureType::Tap, GestureState::Finished) && !has(o, GestureType::DoubleTap, GestureState::Finished));
    r.feed({1, {104, 100}, TouchPhase::Down}, 0.2);
    o = r.feed({1, {104, 100}, TouchPhase::Up}, 0.25);
    CHECK(has(o, GestureType::DoubleTap, GestureState::Finished));
    // long press
    r.feed({1, {50, 50}, TouchPhase::Down}, 5.0);
    o = r.tick(5.6);
    CHECK(has(o, GestureType::LongPress, GestureState::Started));
    o = r.feed({1, {50, 50}, TouchPhase::Up}, 5.8);
    CHECK(has(o, GestureType::LongPress, GestureState::Finished) && !has(o, GestureType::Tap, GestureState::Finished));
    // pan -> swipe right
    r.feed({1, {0, 0}, TouchPhase::Down}, 10.0);
    bool started = false;
    Vec2 off;
    for (int i = 1; i <= 10; ++i) {
        o = r.feed({1, {f32(i * 20), 2}, TouchPhase::Move}, 10.0 + i * 0.01);
        started |= has(o, GestureType::Pan, GestureState::Started);
        for (auto& g : o) if (g.type == GestureType::Pan) off = g.offset;
    }
    CHECK(started && std::abs(off.x - 200.f) < 0.5f);
    o = r.feed({1, {200, 2}, TouchPhase::Up}, 10.11);
    CHECK(has(o, GestureType::Pan, GestureState::Finished));
    bool right = false;
    for (auto& g : o) if (g.type == GestureType::Swipe) right = g.direction == SwipeDirection::Right;
    CHECK(right);
    // pinch: spread x2 and rotate 90deg
    r.feed({1, {100, 100}, TouchPhase::Down}, 20.0);
    o = r.feed({2, {200, 100}, TouchPhase::Down}, 20.01);
    CHECK(has(o, GestureType::Pinch, GestureState::Started));
    r.feed({1, {150, 50}, TouchPhase::Move}, 20.05);
    o = r.feed({2, {150, 250}, TouchPhase::Move}, 20.06);
    f32 scale = 0, rot = 0;
    for (auto& g : o) if (g.type == GestureType::Pinch) { scale = g.scale; rot = g.rotation; }
    CHECK(std::abs(scale - 2.f) < 0.01f);
    CHECK(std::abs(rot - 1.5708f) < 0.01f);
    o = r.feed({2, {150, 250}, TouchPhase::Up}, 20.1);
    CHECK(has(o, GestureType::Pinch, GestureState::Finished));
    r.feed({1, {150, 50}, TouchPhase::Up}, 20.12);
}

void gateI18n() {
    using namespace fuse::ui::i18n;
    CHECK(directionOf("ar_EG") == LayoutDirection::RightToLeft && directionOf("he") == LayoutDirection::RightToLeft);
    CHECK(directionOf("en_US.UTF-8") == LayoutDirection::LeftToRight);
    // Russian: 1 файл, 2 файла, 5 файлов, 21 файл, 11 файлов
    CHECK(pluralIndex("ru", 1) == 0 && pluralIndex("ru", 2) == 1 && pluralIndex("ru", 5) == 2 && pluralIndex("ru", 21) == 0 && pluralIndex("ru", 11) == 2);
    CHECK(pluralIndex("pl", 22) == 1 && pluralIndex("pl", 12) == 2);
    CHECK(pluralIndex("ar", 0) == 0 && pluralIndex("ar", 3) == 3 && pluralIndex("ar", 11) == 4 && pluralIndex("ar", 100) == 5);
    CHECK(pluralIndex("fr", 0) == 0 && pluralIndex("fr", 2) == 1 && pluralIndex("ja", 5) == 0);
    CHECK(format("%1 of %2 (100%%)", {"3", "7"}) == "3 of 7 (100%)");
    const char* po =
        "msgid \"\"\nmsgstr \"\"\n\"Language: ru\\n\"\n\n"
        "msgctxt \"menu\"\nmsgid \"Open\"\nmsgstr \"Открыть\"\n\n"
        "msgid \"Open\"\nmsgstr \"Открыть файл\"\n\n"
        "#, fuzzy\nmsgid \"Close\"\nmsgstr \"Закрыть\"\n\n"
        "msgid \"%n file\"\nmsgid_plural \"%n files\"\nmsgstr[0] \"%n файл\"\nmsgstr[1] \"%n файла\"\nmsgstr[2] \"%n файлов\"\n\n"
        "msgid \"multi\"\nmsgstr \"\"\n\"line \"\n\"two\\n\"\n";
    Translator t;
    CHECK(t.loadPo(po));
    CHECK(t.language() == "ru");
    CHECK(t.tr("menu", "Open") == "Открыть");
    CHECK(t.tr("", "Open") == "Открыть файл");
    CHECK(t.tr("", "Close") == "Close"); // fuzzy skipped
    CHECK(t.tr("", "multi") == "line two\n");
    CHECK(t.trn("", "%n file", "%n files", 5) == "5 файлов");
    CHECK(t.trn("", "%n file", "%n files", 22) == "22 файла");
    installTranslator(&t);
    CHECK(tr("menu", "Open") == "Открыть" && tr("x", "untranslated") == "untranslated");
    CHECK(applicationDirection() == LayoutDirection::LeftToRight);
    removeTranslator(&t);
    CHECK(tr("menu", "Open") == "Open");
    CHECK(Locale::fromName("en_US").formatNumber(1234567.891, 2) == "1,234,567.89");
    CHECK(Locale::fromName("de_DE").formatNumber(-1234.5, 1) == "-1.234,5");
    CHECK(Locale::fromName("hi_IN").formatInteger(12345678) == "1,23,45,678");
    CHECK(Locale::fromName("ar_SA").direction == LayoutDirection::RightToLeft);
}

void gateDesktop() {
    using namespace fuse::ui::desktop;
    Notification n{.appName = "app", .title = "Hi \"you\"", .body = "it's done", .urgency = Urgency::Critical};
    auto c = notifyCommand(n, "linux");
    CHECK(c.size() >= 7 && c[0] == "notify-send" && c[4] == "critical" && c.back() == "it's done");
    c = notifyCommand(n, "macos");
    CHECK(c.size() == 3 && c[2] == "display notification \"it's done\" with title \"Hi \\\"you\\\"\"");
    c = notifyCommand(n, "windows");
    CHECK(c.size() == 5 && c[4].find("it''s done") != std::string::npos && c[4].find("&quot;you&quot;") != std::string::npos);
    CHECK(notifyCommand(n, "android").empty());
    CHECK(openUrlCommand("https://x.y", "freebsd") == std::vector<std::string>({"xdg-open", "https://x.y"}));
    CHECK(openUrlCommand("https://x.y", "macos")[0] == "open");
    PrintJob j{.file = "/tmp/a.pdf", .printer = "lab", .title = "Doc", .copies = 2, .duplex = true};
    c = printCommand(j, "linux");
    const std::vector<std::string> want{"lp", "-d", "lab", "-n", "2", "-t", "Doc", "-o", "sides=two-sided-long-edge", "--", "/tmp/a.pdf"};
    CHECK(c == want);
    c = printCommand(j, "windows");
    CHECK(c.size() == 5 && c[4].find("-Verb PrintTo") != std::string::npos);
    CHECK(listPrintersCommand("linux")[0] == "lpstat");
    (void)printers(); // must not crash without CUPS
    CHECK(!fuse::ui::os::platformName().empty());
}

void gatePdf() {
    using namespace fuse::ui::pdf;
    PdfDocument d;
    d.setTitle("FUSE test");
    d.newPage(A4);
    d.fillRect({mm(20), mm(20), 200, 80}, Color::rgb(0x3D8BFD), 8.f);
    d.strokeRect({mm(20), mm(60), 200, 80}, Color::rgb(0xFF0000, 0.5f), 2.f);
    d.line({0, 0}, {100, 100}, Color::rgb(0x000000), 1.f);
    d.pushClip({0, 0, 300, 300});
    d.text({mm(20), mm(100)}, "Hello PDF (FUSE) caf\xC3\xA9", 18.f, Color::rgb(0x111111), Font::HelveticaBold);
    d.popClip();
    const f32 y = d.paragraph({mm(20), mm(110), 200, 400}, "The quick brown fox jumps over the lazy dog again and again until the line wraps.", 11.f, Color::rgb(0x222222));
    CHECK(y > mm(110) + 22.f);
    Image img;
    img.resize(4, 4, 0x800000FFu);
    d.image(img, {300, 300, 40, 40});
    // a DrawList with a glyph from a tiny atlas
    static u8 atlas[16 * 16];
    for (int i = 0; i < 16 * 16; ++i) atlas[i] = (i % 16) < 8 ? 255 : 0;
    DrawList dl;
    dl.rect({0, 0, 50, 20}, Color::rgb(0x00FF00));
    dl.line({0, 0}, {50, 20}, Color::rgb(0xFF00FF), 2.f);
    DrawCmd g;
    g.op = DrawOp::Glyph;
    g.rect = {10, 2, 8, 8};
    g.u = 0; g.v = 0; g.tw = 8; g.th = 8;
    g.fill = Color::rgb(0x000000);
    dl.push(g);
    d.newPage(landscape(Letter));
    d.renderDrawList(dl, {72, 72}, 2.f, atlas, 16);
    CHECK(d.pageCount() == 2);
    const std::string pdf = d.finish();
    CHECK(pdf.starts_with("%PDF-1.4") && pdf.ends_with("%%EOF\n"));
    CHECK(pdf.find("/Count 2") != std::string::npos && pdf.find("/SMask") != std::string::npos);
    CHECK(pdf.find("/SMask") != std::string::npos && pdf.find("(Hello PDF \\(FUSE\\) caf\xE9)") != std::string::npos);
    // xref offsets point at "N 0 obj"
    const auto sx = pdf.rfind("startxref\n");
    const std::size_t xref = std::stoul(pdf.substr(sx + 10));
    CHECK(pdf.compare(xref, 4, "xref") == 0);
    const std::size_t first = std::stoul(pdf.substr(xref + pdf.substr(xref).find("65535 f \n") + 9, 10));
    CHECK(pdf.compare(first, 7, "1 0 obj") == 0);
    CHECK(PdfDocument::textWidth("iiii", 10.f) < PdfDocument::textWidth("MMMM", 10.f));
    CHECK(PrintPreview::paginate(2500.f, 1000.f).size() == 3);
    const std::string out = fuse::ui::os::env("FUSE_EXTRAS_PDF_OUT");
    if (!out.empty()) CHECK(d.save(out));
}

namespace w = fuse::ui::widgets;
using w::GraphicsScene; using w::GraphicsShape;
void step(UiContext& ui) { ui.update(1.f / 60.f); }
Vec2 ctr(Rect r) { return {r.x + r.w * 0.5f, r.y + r.h * 0.5f}; }
void click(UiContext& ui, Vec2 p) { ui.pointerMove(p); ui.pointerDown(p); ui.pointerUp(p); step(ui); }
void clickNode(UiContext& ui, NodeHandle n) { click(ui, ctr(ui.rect(n))); }
struct Screen {
    UiContext ui{{.viewport = {1200, 800}, .font = &fuse::ui::test::font()}};
    Screen() { w::install(ui); w::registerExtraWidgets(ui); step(ui); }
};
NodeHandle findText(UiContext& ui, NodeHandle root, std::string_view t) {
    if (ui.text(root) == t) return root;
    for (NodeHandle c = ui.firstChild(root); ui.alive(c); c = ui.nextSibling(c))
        if (NodeHandle f = findText(ui, c, t); f.valid()) return f;
    return {};
}

void gateContainers() {
    Screen s;
    UiContext& ui = s.ui;
    const NodeHandle gb = w::groupBox(ui, ui.root(), "Options", true);
    const NodeHandle body = w::groupBoxBody(ui, gb);
    w::label(ui, body, "inside");
    bool toggled = true;
    ui.connect(gb, SignalKind::Toggled, [&toggled](UiContext&, NodeHandle, const SignalArgs& a) { toggled = a.flag; });
    step(ui);
    clickNode(ui, ui.firstChild(gb));
    CHECK(!toggled && ui.disabled(body));

    const NodeHandle st = w::stackedWidget(ui, ui.root());
    const NodeHandle p0 = w::stackedAddPage(ui, st), p1 = w::stackedAddPage(ui, st);
    w::label(ui, p0, "zero"); w::label(ui, p1, "one");
    step(ui);
    CHECK(ui.computed(p0).visible && !ui.computed(p1).visible && w::stackedCount(ui, st) == 2);
    int changed = 0;
    ui.connect(st, SignalKind::ValueChanged, [&changed](UiContext&, NodeHandle, const SignalArgs&) { ++changed; });
    w::setStackedIndex(ui, st, 1);
    step(ui);
    CHECK(!ui.computed(p0).visible && ui.computed(p1).visible && changed == 1 && w::stackedIndex(ui, st) == 1);

    const NodeHandle tb = w::toolBox(ui, ui.root(), "General\nAdvanced\nAbout");
    step(ui);
    CHECK(ui.computed(w::toolBoxPage(ui, tb, 0)).visible && !ui.computed(w::toolBoxPage(ui, tb, 2)).visible);
    clickNode(ui, ui.nextSibling(w::toolBoxPage(ui, tb, 1)));
    step(ui);
    CHECK(ui.computed(w::toolBoxPage(ui, tb, 2)).visible && !ui.computed(w::toolBoxPage(ui, tb, 0)).visible && u32(ui.value(tb)) == 2);

    const NodeHandle sb = w::statusBar(ui, ui.root());
    w::label(ui, w::statusBarPermanent(ui, sb), "Ln 1, Col 1");
    w::showStatusMessage(ui, sb, "Saved", 0.5f);
    CHECK(w::statusMessage(ui, sb) == "Saved");
    for (int i = 0; i < 40; ++i) step(ui);
    CHECK(w::statusMessage(ui, sb).empty());
    w::showStatusMessage(ui, sb, "Sticky");
    for (int i = 0; i < 40; ++i) step(ui);
    CHECK(w::statusMessage(ui, sb) == "Sticky");
}

void gateDialogs() {
    Screen s;
    UiContext& ui = s.ui;
    int picked = -1;
    std::string pickedText;
    const NodeHandle mb = w::messageBox(ui, "Unsaved", "Save changes?", "Save\nDiscard\nCancel", w::MessageIcon::Question);
    ui.connect(mb, SignalKind::Submitted, [&](UiContext&, NodeHandle, const SignalArgs& a) { picked = int(a.value); pickedText = a.text; });
    step(ui);
    CHECK(ui.computed(ui.parent(mb)).visible);
    clickNode(ui, findText(ui, mb, "Discard"));
    step(ui);
    CHECK(picked == 1 && pickedText == "Discard" && !ui.computed(ui.parent(mb)).visible);

    std::string got;
    float ok = -1;
    const NodeHandle in = w::inputDialog(ui, "Rename", "New name:", "old");
    ui.connect(in, SignalKind::Submitted, [&](UiContext&, NodeHandle, const SignalArgs& a) { ok = a.value; got = a.text; });
    step(ui);
    ui.keyDown(Key::End); ui.keyUp(Key::End);
    ui.textInput("er");
    step(ui);
    ui.keyDown(Key::Enter); ui.keyUp(Key::Enter);
    step(ui);
    CHECK(ok == 1.f && got == "older");

    const NodeHandle wz = w::wizard(ui, "Setup", "Welcome\nLicence\nDone");
    float fin = -1;
    ui.connect(wz, SignalKind::Submitted, [&](UiContext&, NodeHandle, const SignalArgs& a) { fin = a.value; });
    bool accepted = false;
    w::setWizardValidator(ui, wz, [&accepted](u32 page) { return page != 1 || accepted; });
    w::label(ui, w::wizardPage(ui, wz, 1), "Accept?");
    step(ui);
    CHECK(w::wizardCurrent(ui, wz) == 0 && ui.disabled(findText(ui, wz, "< Back")));
    clickNode(ui, findText(ui, wz, "Next >"));
    CHECK(w::wizardCurrent(ui, wz) == 1 && findText(ui, wz, "Licence").valid());
    clickNode(ui, findText(ui, wz, "Next >"));
    CHECK(w::wizardCurrent(ui, wz) == 1); // blocked by validator
    accepted = true;
    clickNode(ui, findText(ui, wz, "Next >"));
    step(ui);
    CHECK(w::wizardCurrent(ui, wz) == 2 && ui.computed(findText(ui, wz, "Finish")).visible);
    clickNode(ui, findText(ui, wz, "Finish"));
    CHECK(fin == 1.f);
}

void gateLcdKeyseq() {
    Screen s;
    UiContext& ui = s.ui;
    CHECK(w::sevenSegment('8') == 0x7F && w::sevenSegment('1') == 0x06 && w::sevenSegment(' ') == 0);
    const NodeHandle lcd = w::lcdNumber(ui, ui.root(), 6);
    ui.setValue(lcd, 42.5f);
    CHECK(w::lcdText(ui, lcd) == "42.5");
    w::setLcdBase(ui, lcd, 16);
    ui.setValue(lcd, 255.f);
    CHECK(w::lcdText(ui, lcd) == "ff");
    w::setLcdText(ui, lcd, "12:34");
    CHECK(w::lcdText(ui, lcd) == "12:34");
    step(ui);
    CHECK(w::keySequenceText(Key::S, kModCtrl | kModShift) == "Ctrl+Shift+S" && w::keySequenceText(Key::F5, 0) == "F5");
    const NodeHandle ks = w::keySequenceEdit(ui, ui.root());
    std::string rec;
    ui.connect(ks, SignalKind::Submitted, [&rec](UiContext&, NodeHandle, const SignalArgs& a) { rec = a.text; });
    step(ui);
    clickNode(ui, ks);
    ui.keyDown(Key::P, kModCtrl | kModAlt);
    CHECK(rec == "Ctrl+Alt+P" && ui.text(ks) == "Ctrl+Alt+P");
    ui.keyDown(Key::Escape);
    CHECK(ui.text(ks).empty());
}

void gateMdi() {
    Screen s;
    UiContext& ui = s.ui;
    ui.setCss(ui.root(), "padding", "0px");
    const NodeHandle area = w::mdiArea(ui, ui.root());
    ui.setCss(area, "height", "600px");
    const NodeHandle a = w::mdiSubWindow(ui, area, "Doc A", {20, 20, 300, 200});
    const NodeHandle b = w::mdiSubWindow(ui, area, "Doc B", {100, 80, 300, 200});
    w::label(ui, w::mdiBody(ui, a), "A body");
    step(ui);
    CHECK(w::mdiActive(ui, area) == b);
    const Rect ra = ui.rect(a);
    CHECK(std::abs(ra.w - 300.f) < 1.f && std::abs(ra.h - 200.f) < 1.f);
    click(ui, {ra.x + 10, ra.y + ra.h - 20}); // part of A not covered by B
    step(ui);
    CHECK(w::mdiActive(ui, area) == a);
    // drag A's title bar by (+50, +30)
    const Rect tbar = ui.rect(ui.firstChild(a));
    const Vec2 p0{tbar.x + 30, tbar.y + tbar.h * 0.5f};
    ui.pointerMove(p0); ui.pointerDown(p0); step(ui);
    ui.pointerMove({p0.x + 25, p0.y + 15}); step(ui);
    ui.pointerMove({p0.x + 50, p0.y + 30}); step(ui);
    ui.pointerUp({p0.x + 50, p0.y + 30}); step(ui);
    const Rect g = w::mdiGeometry(ui, a);
    CHECK(std::abs(g.x - 70.f) < 1.f && std::abs(g.y - 50.f) < 1.f);
    // resize grip
    const NodeHandle grip = ui.nextSibling(w::mdiBody(ui, a));
    const Vec2 q = ctr(ui.rect(grip));
    ui.pointerMove(q); ui.pointerDown(q); step(ui);
    ui.pointerMove({q.x + 40, q.y + 20}); step(ui);
    ui.pointerUp({q.x + 40, q.y + 20}); step(ui);
    CHECK(std::abs(w::mdiGeometry(ui, a).w - 340.f) < 1.f && std::abs(w::mdiGeometry(ui, a).h - 220.f) < 1.f);
    w::mdiTile(ui, area);
    step(ui);
    CHECK(std::abs(w::mdiGeometry(ui, a).w - w::mdiGeometry(ui, b).w) < 1.f && w::mdiGeometry(ui, a).y == w::mdiGeometry(ui, b).y);
    w::mdiCascade(ui, area);
    // close B
    bool closed = false;
    ui.connect(b, SignalKind::Submitted, [&closed](UiContext&, NodeHandle, const SignalArgs&) { closed = true; });
    step(ui);
    w::mdiTile(ui, area);
    step(ui);
    const NodeHandle x = ui.nextSibling(ui.firstChild(ui.firstChild(b)));
    clickNode(ui, x);
    step(ui);
    CHECK(closed && !ui.alive(b) && w::mdiSubWindows(ui, area).size() == 1);
}

void gateCompleter() {
    const std::vector<std::string> e{"apple", "apricot", "banana", "grape", "pineapple"};
    const auto m = w::completions(e, "ap");
    CHECK(m.size() == 4 && m[0] == "apple" && m[1] == "apricot" && m[2] == "grape" && m[3] == "pineapple");
    CHECK(w::completions(e, "", 2).size() == 2);
    Screen s;
    UiContext& ui = s.ui;
    const NodeHandle le = w::lineEdit(ui, ui.root(), "");
    w::attachCompleter(ui, le, e);
    step(ui);
    clickNode(ui, le);
    ui.textInput("ban");
    step(ui);
    ui.keyDown(Key::Down); ui.keyUp(Key::Down);
    step(ui);
    const NodeHandle item = findText(ui, ui.root(), "banana");
    CHECK(item.valid());
    if (item.valid()) { clickNode(ui, item); step(ui); }
    CHECK(ui.editBuffer(le).text() == "banana");
}

void gateGraphics() {
    GraphicsScene sc;
    const u32 r = sc.add({.shape = GraphicsShape::Rect, .rect = {0, 0, 100, 100}});
    const u32 e = sc.add({.shape = GraphicsShape::Ellipse, .rect = {50, 50, 100, 100}, .fill = Color::rgb(0xFF5555), .z = 1});
    const u32 l = sc.add({.shape = GraphicsShape::Line, .rect = {300, 0, 100, 100}, .strokeWidth = 2});
    const u32 p = sc.add({.shape = GraphicsShape::Polygon, .points = {{500, 0}, {600, 0}, {550, 100}}});
    sc.add({.shape = GraphicsShape::Text, .rect = {0, 200, 200, 30}, .fill = Color::rgb(0xFFFFFF), .text = "scene text"});
    CHECK(sc.itemAt({90, 90}) == e);         // ellipse is above (z = 1)
    CHECK(sc.itemAt({10, 10}) == r);
    CHECK(sc.itemAt({52, 52}) == r);         // outside the ellipse's curve
    CHECK(sc.itemAt({350, 50}) == l && sc.itemAt({350, 60}) == ~0u);
    CHECK(sc.itemAt({550, 50}) == p && sc.itemAt({505, 90}) == ~0u);
    CHECK(sc.itemsIn({0, 0, 200, 200}).size() == 2);
    // spatial index under many items
    for (int i = 0; i < 20000; ++i) sc.add({.rect = {f32(1000 + (i % 200) * 20), f32((i / 200) * 20), 10, 10}, .z = -1});
    CHECK(sc.itemAt({1000 + 20 * 5 + 5, 20 * 7 + 5}) != ~0u && sc.size() == 20005);
    const Rect b = sc.bounds();
    CHECK(b.x == 0 && b.right() >= 4990);
    Screen s;
    UiContext& ui = s.ui;
    ui.setCss(ui.root(), "padding", "0px");
    GraphicsScene small;
    const u32 a = small.add({.rect = {10, 10, 50, 50}});
    const u32 c = small.add({.shape = GraphicsShape::Ellipse, .rect = {100, 10, 50, 50}, .fill = Color::rgb(0x55FF55)});
    const NodeHandle v = w::graphicsView(ui, ui.root(), &small);
    step(ui);
    i32 sel = -2;
    ui.connect(v, SignalKind::ValueChanged, [&sel](UiContext&, NodeHandle, const SignalArgs& g) { sel = i32(g.value); });
    const Vec2 pa = w::sceneToView(ui, v, {35, 35});
    click(ui, pa);
    CHECK(sel == i32(a) && small.item(a)->selected);
    // drag moves the selected item
    ui.pointerMove(pa); ui.pointerDown(pa); step(ui);
    ui.pointerMove({pa.x + 20, pa.y + 10}); step(ui);
    ui.pointerUp({pa.x + 20, pa.y + 10}); step(ui);
    CHECK(std::abs(small.item(a)->rect.x - 30.f) < 0.01f && std::abs(small.item(a)->rect.y - 20.f) < 0.01f);
    // zoom around the pointer keeps the scene point fixed
    const Vec2 sp0 = w::viewToScene(ui, v, {200, 150});
    ui.wheel({200, 150}, {0, 2});
    const Vec2 sp1 = w::viewToScene(ui, v, {200, 150});
    CHECK(w::viewZoom(ui, v) > 1.3f && std::abs(sp0.x - sp1.x) < 0.01f && std::abs(sp0.y - sp1.y) < 0.01f);
    // rubber band selects both
    w::setViewTransform(ui, v, 1.f, {0, 0});
    const Vec2 o = w::sceneToView(ui, v, {0, 0}), q = w::sceneToView(ui, v, {200, 100});
    ui.pointerMove({o.x + 1, o.y + 1}); ui.pointerDown({o.x + 1, o.y + 1}); step(ui);
    ui.pointerMove(q); step(ui);
    ui.pointerUp(q); step(ui);
    CHECK(small.selection().size() == 2);
    ui.keyDown(Key::Delete);
    CHECK(small.size() == 0 && !small.item(c));
    small.add({.rect = {0, 0, 1000, 500}});
    w::fitInView(ui, v);
    CHECK(w::viewZoom(ui, v) < 1.2f);
}

void gateWidgetPdf() {
    Screen s;
    UiContext& ui = s.ui;
    ui.setCss(ui.root(), "padding", "12px");
    ui.setCss(ui.root(), "gap", "8px");
    const NodeHandle gb = w::groupBox(ui, ui.root(), "Printing");
    w::label(ui, w::groupBoxBody(ui, gb), "Widget tree rendered straight to vector PDF");
    w::button(ui, w::groupBoxBody(ui, gb), "A button");
    const NodeHandle lcd = w::lcdNumber(ui, ui.root(), 5);
    ui.setValue(lcd, 1234.5f);
    static GraphicsScene sc;
    sc.add({.rect = {10, 10, 80, 50}});
    sc.add({.shape = GraphicsShape::Ellipse, .rect = {60, 30, 70, 70}, .fill = Color::rgb(0xFF7043), .z = 1});
    const NodeHandle v = w::graphicsView(ui, ui.root(), &sc);
    ui.setCss(v, "width", "300px");
    ui.setCss(v, "height", "160px");
    const NodeHandle sb = w::statusBar(ui, ui.root());
    w::showStatusMessage(ui, sb, "Ready");
    for (int i = 0; i < 3; ++i) step(ui);
    pdf::PdfDocument d;
    d.newPage(pdf::A4);
    const f32 k = (pdf::A4.w - pdf::mm(30)) / ui.viewport().w;
    d.renderDrawList(ui.drawList(), {pdf::mm(15), pdf::mm(15)}, k, ui.atlas().pixels(), ui.atlas().size());
    const std::string out = d.finish();
    CHECK(out.find("/SMask") != std::string::npos && out.size() > 2000);
    const std::string dir = fuse::ui::os::env("FUSE_EXTRAS_OUT");
    if (!dir.empty()) {
        CHECK(d.save(dir + "/extras.pdf"));
        Image img;
        fuse::ui::test::render(ui, img);
        stbi_write_png((dir + "/extras.png").c_str(), img.width, img.height, 4, img.pixels.data(), img.width * 4);
    }
}

// Private session bus (dbus-daemon) + an in-process fake StatusNotifierWatcher / AppMenu registrar on a second
// connection: drives the real SNI + dbusmenu exports end to end. Skips (passes) when dbus-daemon / libdbus are absent.
void gateTray() {
    using namespace fuse::ui::dbus;
    namespace os = fuse::ui::os;
        if (!available() || os::findExecutable("dbus-daemon").empty()) { std::printf("tray: skipped (no libdbus / dbus-daemon)\n"); return; }
    const std::string sock = "/tmp/fuse_tray_bus_" + std::to_string(::getpid());
    const std::string addr = "unix:path=" + sock;
    // forge::os::spawn_wait leaks the child's pipe write ends (pout[1] / perr[1] are not closed after dup2), so a
    // background daemon would hold them and spawn_wait would block until it exits: close every inherited fd.
    if (os::findExecutable("bash").empty()) { std::printf("tray: skipped (no bash)\n"); return; }
    std::string closeFds;
    for (int fd = 3; fd < 32; ++fd) closeFds += " " + std::to_string(fd) + ">&-";
    auto d = os::runProcess({"bash", "-c", "setsid dbus-daemon --session --address=" + addr + " --nofork --nopidfile </dev/null >/dev/null 2>&1" + closeFds + " & echo $!"}, {}, 5000);
    CHECK(d && d->exitCode == 0);
    if (!d || d->exitCode != 0) return;
    const std::string pid = d->out.substr(0, d->out.find('\n'));
    for (int i = 0; i < 200 && !os::exists(sock); ++i) os::sleepSeconds(0.01);
    ::setenv("DBUS_SESSION_BUS_ADDRESS", addr.c_str(), 1);

    auto host = Connection::open(addr);
    CHECK(host != nullptr);
    if (!host) return;
    CHECK(host->requestName("org.kde.StatusNotifierWatcher") && host->requestName("com.canonical.AppMenu.Registrar"));
    std::string registered, menuPath;
    u32 window = 0;
    host->exportObject("/StatusNotifierWatcher", [&](Connection& c, const Message& m) {
        if (!m.isCall("org.kde.StatusNotifierWatcher", "RegisterStatusNotifierItem")) return false;
        registered = m.read().str();
        Message r = m.reply(); c.send(r);
        return true;
    });
    host->exportObject("/com/canonical/AppMenu/Registrar", [&](Connection& c, const Message& m) {
        if (!m.isCall("com.canonical.AppMenu.Registrar", "RegisterWindow")) return false;
        Reader rd = m.read();
        window = rd.u32v(); rd.next(); menuPath = rd.str();
        Message r = m.reply(); c.send(r);
        return true;
    });
    CHECK(TrayIcon::available() && std::string(TrayIcon::backendName()) == "sni");

    TrayIcon tray;
    tray.setTitle("FUSE Tray");
    tray.setToolTip("FUSE", "running");
    Image icon; icon.resize(2, 1);
    icon.pixels = {0xFF0000FFu, 0x80000080u}; // opaque red, half-alpha premultiplied red
    tray.setIcon(icon);
    int quit = 0, act = 0, scroll = 0;
    bool darkChecked = false;
    tray.setMenu({{.label = "&Open", .shortcut = "Ctrl+O"}, MenuItem::sep(),
                  {.label = "Theme", .children = {{.label = "Dark", .onTriggered = [&] { darkChecked = true; }, .checkable = true}}},
                  {.label = "&Quit", .onTriggered = [&] { ++quit; }}});
    tray.onActivated = [&](int x, int y) { act = x * 1000 + y; };
    tray.onScroll = [&](int dlt, bool h) { scroll = h ? -dlt : dlt; };
    CHECK(tray.show());
    const std::string svc = tray.serviceName();
    std::atomic<bool> done{false};
    std::string title, pixels, layoutDump, busctlOut;
    u32 rev = 0;
    std::thread client([&] {
        auto c = Connection::open(addr);
        if (!c) { done = true; return; }
        Message g = Message::call(svc, "/StatusNotifierItem", "org.freedesktop.DBus.Properties", "Get");
        g.args().s("org.kde.StatusNotifierItem").s("Title");
        Message r = c->callBlocking(g);
        title = r.read().recurse().str();
        Message gp = Message::call(svc, "/StatusNotifierItem", "org.freedesktop.DBus.Properties", "Get");
        gp.args().s("org.kde.StatusNotifierItem").s("IconPixmap");
        Message rp = c->callBlocking(gp);
        Reader st = rp.read().recurse().recurse().recurse(); // v -> a -> (iiay)
        st.next(); st.next();
        pixels = st.bytes();
        Message gl = Message::call(svc, "/MenuBar", "com.canonical.dbusmenu", "GetLayout");
        gl.args().i(0).i(-1).open('a', "s").close();
        Message rl = c->callBlocking(gl);
        Reader lr = rl.read();
        rev = lr.u32v();
        // layout: (i a{sv} av) -> walk top-level children labels
        lr.next();
        Reader root = lr.recurse();
        root.next(); root.next();
        for (Reader kids = root.recurse(); kids.type() == 'v'; kids.next()) {
            Reader n = kids.recurse().recurse();
            const i32 id = n.i32v();
            n.next();
            for (Reader p = n.recurse(); p.type() == 'e'; p.next()) {
                Reader e = p.recurse();
                const std::string k = e.str();
                e.next();
                if (k == "label" || k == "type") layoutDump += std::to_string(id) + ":" + e.recurse().str() + ";";
            }
        }
        auto event = [&](i32 id) {
            Message ev = Message::call(svc, "/MenuBar", "com.canonical.dbusmenu", "Event");
            ev.args().i(id).s("clicked").open('v', "s").s("").close().u(0);
            c->callBlocking(ev);
        };
        event(5);  // Quit (ids: 1 Open, 2 sep, 3 Theme, 4 Dark, 5 Quit)
        event(4);  // Dark (checkable)
        Message a = Message::call(svc, "/StatusNotifierItem", "org.kde.StatusNotifierItem", "Activate");
        a.args().i(12).i(34);
        c->callBlocking(a);
        Message sc = Message::call(svc, "/StatusNotifierItem", "org.kde.StatusNotifierItem", "Scroll");
        sc.args().i(3).s("horizontal");
        c->callBlocking(sc);
        if (auto b = os::runProcess({"busctl", "--address=" + addr, "--", "call", svc, "/MenuBar", "com.canonical.dbusmenu", "GetLayout", "iias", "3", "-1", "0"}, {}, 5000))
            busctlOut = b->out + b->err;
        done = true;
    });
    const double t0 = os::monotonicSeconds();
    while (!done && os::monotonicSeconds() - t0 < 15) { tray.poll(); host->poll(5); }
    client.join();
    CHECK(registered == svc);
    CHECK(title == "FUSE Tray");
    CHECK(pixels.size() == 8 && u8(pixels[0]) == 255 && u8(pixels[1]) == 255 && u8(pixels[2]) == 0 && u8(pixels[4]) == 0x80 && u8(pixels[5]) == 255);
    CHECK(rev >= 1 && layoutDump == "1:_Open;2:separator;3:Theme;5:_Quit;");
    CHECK(quit == 1 && darkChecked && tray.menu()[2].children[0].checked);
    CHECK(act == 12034 && scroll == -3);
    CHECK(os::findExecutable("busctl").empty() || busctlOut.find("\"Dark\" \"toggle-type\" s \"checkmark\" \"toggle-state\" i 1") != std::string::npos);
    std::printf("tray: busctl GetLayout(3): %s", busctlOut.c_str());

    GlobalMenuBar bar(0x4200001);
    CHECK(bar.setMenus({{.label = "&File", .children = {{.label = "&New"}, {.label = "E&xit"}}}, {.label = "&Help"}}));
    for (int i = 0; i < 50 && menuPath.empty(); ++i) { host->poll(5); bar.poll(); }
    CHECK(window == 0x4200001 && menuPath == bar.objectPath() && bar.active());
    tray.hide();
    os::runProcess({"kill", pid});
}
} // namespace

int main(int argc, char** argv) {
    const std::map<std::string, std::function<void()>> gates{
        {"undo", gateUndo}, {"gesture", gateGesture}, {"i18n", gateI18n}, {"desktop", gateDesktop}, {"pdf", gatePdf}, {"containers", gateContainers}, {"dialogs", gateDialogs}, {"lcd_keyseq", gateLcdKeyseq},
        {"mdi", gateMdi}, {"completer", gateCompleter}, {"graphics", gateGraphics}, {"widget_pdf", gateWidgetPdf}, {"tray", gateTray}};
    std::string only;
    for (int i = 1; i < argc; ++i) if (std::strncmp(argv[i], "--gate=", 7) == 0) only = argv[i] + 7;
    for (auto& [n, f] : gates) if (only.empty() || only == n) { f(); std::printf("gate %s: %s\n", n.c_str(), g_fail ? "FAIL" : "ok"); }
    return g_fail ? 1 : 0;
}
