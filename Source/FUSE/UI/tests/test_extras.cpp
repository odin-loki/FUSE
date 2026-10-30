// Qt-parity extras gates (--gate=<name>; no argument runs all): undo gesture i18n desktop pdf containers dialogs
// lcd_keyseq mdi completer graphics widget_pdf. FUSE_EXTRAS_OUT=<dir> writes extras.png / extras.pdf.
#include "test_util.hpp"
#include <atomic>
#include <cstdlib>
#include <chrono>
#include <thread>
#include <vector>
#include <unistd.h>
#include "fuse/ui/extras/dbus.hpp"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include "fuse/ui/extras/extras.hpp"
#include "fuse/ui/extras/declarative.hpp"
#include "fuse/ui/extras/svg.hpp"
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
#ifdef _WIN32
    std::printf("tray: skipped (session D-Bus gate is POSIX-only; Win32 tray has its own gate)\n");
#else
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
    ::setenv("DBUS_SESSION_BUS_ADDRESS", addr.c_str(), 1);
    // the socket file appears at bind(), before dbus-daemon listens and finishes its startup: connecting right then
    // fails under load (seen with ctest -j32). Retry the connection itself, not the path.
    auto host = Connection::open(addr);
    for (int i = 0; i < 1000 && !host; ++i) { os::sleepSeconds(0.01); host = Connection::open(addr); }
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
#endif
}

void gateRichText() {
    using namespace fuse::ui::rich;
    // headless editor
    RichTextEditor ed;
    ed.insertText("Hello world");
    ed.setCursor(6); ed.setCursor(11, true); // select "world"
    ed.toggleBold();
    CHECK(ed.doc.formatOfByte(6).bold && !ed.doc.formatOfByte(5).bold);
    ed.toggleBold();
    CHECK(!ed.doc.formatOfByte(6).bold);
    ed.toggleBold();
    ed.setColor(Color::rgb(0xFF0000));
    ed.setCursor(11);
    ed.newParagraph();
    ed.setList(ListKind::Ordered);
    ed.typing = {};
    ed.insertText("one");
    ed.newParagraph();
    ed.insertText("two");
    CHECK(ed.doc.blockCount() == 3 && ed.doc.blockFormat(2).list == ListKind::Ordered && ed.doc.listNumber(2) == 2);
    ed.newParagraph();
    ed.newParagraph(); // empty item leaves the list
    CHECK(ed.doc.blockCount() == 4 && ed.doc.blockFormat(3).list == ListKind::None);
    ed.setCursor(0);
    ed.setHeading(1);
    ed.setAlign(fuse::ui::rich::Align::Center);
    CHECK(ed.doc.plainText() == "Hello world\none\ntwo\n");
    // undo / redo
    const RichDocument before = ed.doc;
    ed.setCursor(ed.doc.size());
    ed.insertText("abc");
    ed.insertText("def");
    ed.undo();
    CHECK(ed.doc == before);
    ed.redo();
    CHECK(ed.doc.plainText() == "Hello world\none\ntwo\nabcdef");
    ed.backspace(); ed.backspace();
    CHECK(ed.doc.plainText().ends_with("abcd"));
    ed.undo();
    CHECK(ed.doc.plainText().ends_with("abcdef"));
    // HTML round trip
    const std::string html = ed.doc.toHtml();
    CHECK(html.find("<h1 style=\"text-align:center;\">Hello <b><span style=\"color:#ff0000;\">world</span></b></h1>") != std::string::npos);
    CHECK(html.find("<ol>\n<li>one</li>\n<li>two</li>\n</ol>") != std::string::npos);
    RichDocument h2;
    h2.fromHtml(html);
    CHECK(h2 == ed.doc);
    // foreign HTML
    RichDocument fh;
    fh.fromHtml("<html><head><style>p{}</style></head><body><p>A &amp; <a href=\"https://x.org\">link</a><br>next <span style=\"font-weight:700; font-size:12pt; text-decoration: underline line-through\">styled</span></p>\n<ul><li>x<ul><li>y</li></ul></li></ul></body></html>");
    CHECK(fh.plainText() == "A & link\nnext styled\nx\ny");
    CHECK(fh.formatOfByte(4).link == "https://x.org");
    const CharFormat st = fh.formatOfByte(fh.plainText().find("styled"));
    CHECK(st.bold && st.underline && st.strike && std::abs(st.size - 16.f) < 0.01f);
    CHECK(fh.blockFormat(3).list == ListKind::Bullet && fh.blockFormat(3).indent == 1);
    // Markdown round trip
    const std::string md = ed.doc.toMarkdown();
    CHECK(md.starts_with("# Hello **world**\n\n1. one\n2. two\n\n"));
    RichDocument m2;
    m2.fromMarkdown("## Title\n\nSome **bold _both_** and ~~gone~~ [site](https://a.b) <u>u</u> \\*lit\\*\n\n- a\n  - b\n3. c\n");
    CHECK(m2.plainText() == "Title\nSome bold both and gone site u *lit*\na\nb\nc");
    CHECK(m2.blockFormat(0).heading == 2 && m2.blockFormat(3).indent == 1 && m2.blockFormat(4).list == ListKind::Ordered);
    const std::size_t both = m2.plainText().find("both");
    CHECK(m2.formatOfByte(both).bold && m2.formatOfByte(both).italic && !m2.formatOfByte(both - 2).italic);
    CHECK(m2.formatOfByte(m2.plainText().find("site")).link == "https://a.b" && m2.formatOfByte(m2.plainText().find("gone")).strike);
    RichDocument m3;
    m3.fromMarkdown(m2.toMarkdown());
    CHECK(m3 == m2);
    // layout: wrapping, caret and hit testing with a fixed-advance measure
    auto meas = [](std::string_view s, f32 px) { return f32(s.size()) * px * 0.5f; };
    auto lh = [](f32 px) { return px * 1.25f; };
    RichDocument ld;
    ld.setPlainText("aaaa bbbb cccc\nxxxxxxxxxxxxxxxxxxxx");
    const RichLayout L = layout(ld, 100.f, 10.f, meas, lh); // 20 chars per line
    CHECK(L.lines.size() == 2); // 20 x fit exactly
    const RichLayout L2 = layout(ld, 50.f, 10.f, meas, lh); // 10 chars per line
    CHECK(L2.lines.size() == 2 + 2 && L2.lines[0].b == 10 && L2.lines[2].b - L2.lines[2].a == 10);
    CHECK(L2.hitTest({26.f, 2.f}, ld, meas) == 5 && L2.caretRect(5, ld, meas).x == 25.f);
    CHECK(L2.lineOf(10) == 1 && L2.hitTest({0.f, 13.f}, ld, meas) == 10);

    // widget
    Screen s;
    UiContext& ui = s.ui;
    const NodeHandle te = w::richTextEdit(ui, ui.root());
    ui.setCss(te, "width", "400px");
    ui.setCss(te, "height", "200px");
    std::string changed, link;
    ui.connect(te, SignalKind::TextChanged, [&](UiContext&, NodeHandle, const SignalArgs& a) { changed = a.text; });
    ui.connect(te, SignalKind::Submitted, [&](UiContext&, NodeHandle, const SignalArgs& a) { link = a.text; });
    step(ui);
    clickNode(ui, te);
    CHECK(ui.focused() == te);
    ui.textInput("Hi ");
    ui.keyDown(Key::B, kModCtrl);
    ui.textInput("there");
    CHECK(changed == "Hi there");
    RichTextEditor* re = w::richEditor(ui, te);
    CHECK(re && re->doc.formatOfByte(4).bold && !re->doc.formatOfByte(1).bold);
    ui.keyDown(Key::Home, kModShift);
    ui.keyDown(Key::U, kModCtrl);
    CHECK(re->doc.formatOfByte(0).underline && re->doc.formatOfByte(7).underline);
    ui.keyDown(Key::C, kModCtrl);
    ui.keyDown(Key::End);
    ui.keyDown(Key::Enter);
    ui.keyDown(Key::V, kModCtrl);
    CHECK(changed == "Hi there\nHi there");
    ui.keyDown(Key::Z, kModCtrl);
    CHECK(changed == "Hi there\n");
    ui.keyDown(Key::Y, kModCtrl);
    ui.keyDown(Key::Up);
    CHECK(re->doc.blockAt(re->cursor) == 0);
    w::setRichHtml(ui, te, "<p>Go <a href=\"https://fuse.dev\">here</a> now</p><p>second</p>");
    step(ui);
    const Rect cr = ui.contentRect(te);
    const f32 hx = cr.x + ui.measureString("Go h", ui.computed(te).fontSize);
    ui.setModifiers(kModCtrl);
    click(ui, {hx, cr.y + 6.f});
    ui.setModifiers(0);
    CHECK(link == "https://fuse.dev");
    click(ui, {cr.x + 2.f, cr.y + ui.font()->metrics(ui.computed(te).fontSize).lineHeight() * 1.5f});
    CHECK(re->cursor == re->doc.blockStart(1));
    w::setRichMarkdown(ui, te, "# Big\n\n- **b** _i_ ~~s~~ <u>u</u> [l](https://l)\n");
    CHECK(w::richMarkdown(ui, te) == "# Big\n\n- **b** _i_ ~~s~~ <u>u</u> [l](https://l)\n");
    step(ui);
    ui.setFocus(te, FocusReason::Programmatic);
    w::richEditor(ui, te)->selectAll();
    ui.invalidate(te);
    step(ui); // paints selection, list marker, heading, decorations and caret
    {   // justify: inner lines fill the width exactly, the paragraph's last line stays left
        fuse::ui::rich::RichDocument jd;
        jd.fromHtml("<p style=\"text-align:justify\">aa bb cc dd ee ff gg</p>");
        const auto m = [](std::string_view s, f32) { return f32(s.size()) * 10.f; };
        const auto L = fuse::ui::rich::layout(jd, 75.f, 14.f, m, [](f32) { return 16.f; });
        CHECK(L.lines.size() >= 2);
        if (L.lines.size() >= 2) {
            const auto& l0 = L.lines[0];
            CHECK(l0.frags.size() >= 2);
            const auto& last = l0.frags.back();
            f32 endX = last.x + last.w;
            for (std::size_t k = last.b; k > last.a && jd.plainText()[k - 1] == ' '; --k) endX -= 10.f;
            CHECK(std::abs(endX - (l0.x + 75.f)) < 0.5f);
            const auto& lz = L.lines.back();
            CHECK(lz.frags.back().x + lz.frags.back().w <= 75.f);
            CHECK(std::abs(lz.frags.front().x - lz.x) < 0.01f);
        }
    }
    {   // synthetic oblique: one strip per texel row, upper rows further right
        DrawList dl;
        DrawCmd g;
        g.op = DrawOp::Glyph;
        g.rect = {10.f, 0.f, 8.f, 10.f};
        g.u = 0; g.v = 5; g.tw = 8; g.th = 10;
        dl.push(g);
        fuse::ui::rich::obliqueGlyphs(dl, 0, 10.f, 0.2f);
        CHECK(dl.size() == 10);
        CHECK(dl.cmds().front().v == 5 && dl.cmds().front().th == 1 && dl.cmds().back().v == 14);
        CHECK(dl.cmds().front().rect.x == 12.f && dl.cmds().back().rect.x == 10.f);
    }
    if (const std::string dir = fuse::ui::os::env("FUSE_EXTRAS_OUT"); !dir.empty()) {
        w::setRichHtml(ui, te, "<p style=\"text-align:justify\">Justified text spreads the slack of every line but the last over its spaces, <i>and italic is a synthetic oblique</i> sheared from the regular face.</p>"
                               "<h2>Rich <i>text</i></h2><p>Plain, <b>bold</b>, <u>under</u>, <s>strike</s>, "
                               "<span style=\"color:#e0703a\">colour</span>, <span style=\"font-size:22px\">big</span> and a "
                               "<a href=\"x\">link</a> wrapping across the width of the editor.</p><ol><li>first</li><li>second</li></ol>"
                               "<p style=\"text-align:center\">centred</p>");
        w::richEditor(ui, te)->setCursor(9);
        w::richEditor(ui, te)->setCursor(14, true);
        ui.setCss(te, "height", "300px");
        for (int i = 0; i < 3; ++i) step(ui);
        Image img;
        fuse::ui::test::render(ui, img);
        stbi_write_png((dir + "/richtext.png").c_str(), img.width, img.height, 4, img.pixels.data(), img.width * 4);
        w::setRichMarkdown(ui, te, "# Big\n\n- **b** _i_ ~~s~~ <u>u</u> [l](https://l)\n");
    }
    w::setRichReadOnly(ui, te, true);
    link.clear();
    ui.textInput("x");
    CHECK(re->doc.plainText() == "Big\nb i s u l");
}

// System web engine (WebKitGTK here; run under Xvfb). Skips cleanly when no engine / display is present.
// Embedded child in a foreign X parent: map timing (parent mapped after the child), geometry in parent coordinates
// after resizes, hide/show, explicit + click focus.
void gateWebViewEmbedded() {
    using fuse::ui::web::WebView;
    auto x11 = fuse::ui::os::loadLibrary({"libX11.so.6"});
    if (!x11 || !std::getenv("DISPLAY")) { std::printf("webview embedded: no X11, skipped\n"); return; }
    auto openDpy = x11->get<void* (*)(const char*)>("XOpenDisplay");
    auto closeDpy = x11->get<int (*)(void*)>("XCloseDisplay");
    auto rootOf = x11->get<unsigned long (*)(void*, int)>("XRootWindow");
    auto create = x11->get<unsigned long (*)(void*, unsigned long, int, int, unsigned, unsigned, unsigned, unsigned long, unsigned long)>("XCreateSimpleWindow");
    auto mapW = x11->get<int (*)(void*, unsigned long)>("XMapWindow");
    auto destroyW = x11->get<int (*)(void*, unsigned long)>("XDestroyWindow");
    auto sync = x11->get<int (*)(void*, int)>("XSync");
    void* dpy = openDpy(nullptr);
    if (!dpy) { std::printf("webview embedded: XOpenDisplay failed, skipped\n"); return; }
    const unsigned long parent = create(dpy, rootOf(dpy, 0), 0, 0, 800, 600, 0, 0, 0xffffff);
    sync(dpy, 0);
    WebView v;
    CHECK(v.create(reinterpret_cast<void*>(parent), {20, 30, 300, 200}));
    auto pump = [&](const std::function<bool()>& until) {
        for (int i = 0; i < 500 && !until(); ++i) { v.poll(); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
        return until();
    };
    fuse::ui::Rect g{};
    bool viewable = true;
    if (!v.nativeGeometry(g, viewable)) { std::printf("webview embedded: backend has no X embedding, skipped\n"); v.destroy(); destroyW(dpy, parent); closeDpy(dpy); return; }
    CHECK(!viewable); // parent not mapped yet
    CHECK(g.x == 20 && g.y == 30 && g.w == 300 && g.h == 200);
    mapW(dpy, parent);
    sync(dpy, 0);
    CHECK(pump([&] { return v.nativeGeometry(g, viewable) && viewable; }));
    CHECK(g.x == 20 && g.y == 30 && g.w == 300 && g.h == 200);
    v.setBounds({50, 60, 410, 250});
    v.poll();
    CHECK(pump([&] { return v.nativeGeometry(g, viewable) && g.w == 410 && g.h == 250; }));
    CHECK(g.x == 50 && g.y == 60); // parent-relative, not root coordinates
    v.setVisible(false);
    CHECK(v.nativeGeometry(g, viewable) && !viewable);
    v.setVisible(true);
    CHECK(pump([&] { return v.nativeGeometry(g, viewable) && viewable; }));
    CHECK(g.x == 50 && g.y == 60 && g.w == 410 && g.h == 250); // geometry survives hide/show
    v.setBounds({0, 0, 1, 1}); // degenerate sizes clamp instead of X BadValue
    v.setBounds({5, 5, 200, 100});
    CHECK(pump([&] { return v.nativeGeometry(g, viewable) && g.w == 200 && g.h == 100 && g.x == 5; }));
    CHECK(v.focus());
    CHECK(pump([&] { return v.hasFocus(); }));
    v.destroy();
    destroyW(dpy, parent);
    sync(dpy, 0);
    closeDpy(dpy);
}

void gateWebView() {
    using fuse::ui::web::WebView;
    if (!WebView::available()) { std::printf("webview: no system engine (%s), skipped\n", WebView::backendName()); return; }
    std::printf("webview backend: %s\n", WebView::backendName());
    WebView v;
    CHECK(v.create(nullptr, {40, 40, 640, 480}));
    std::vector<std::string> msgs, titles, loads;
    v.onMessage = [&](const std::string& m) { msgs.push_back(m); };
    v.onTitleChanged = [&](const std::string& t) { titles.push_back(t); };
    v.onLoadFinished = [&](const std::string& u) { loads.push_back(u); };
    auto pump = [&](const std::function<bool()>& until) {
        for (int i = 0; i < 1500 && !until(); ++i) { v.poll(); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
        return until();
    };
    v.loadHtml("<html><head><title>T1</title></head><body><script>fuse.postMessage('hello ' + (2 + 3));</script></body></html>", "https://fuse.invalid/");
    CHECK(pump([&] { return !loads.empty() && !msgs.empty(); }));
    CHECK(!msgs.empty() && msgs[0] == "hello 5");
    CHECK(v.title() == "T1" && !titles.empty() && titles.back() == "T1");
    CHECK(v.url() == "https://fuse.invalid/");
    int done = 0;
    std::string res;
    bool ok = false;
    v.evaluate("document.title = 'T2'; 6 * 7", [&](bool k, std::string r) { ok = k; res = std::move(r); ++done; });
    CHECK(pump([&] { return done == 1; }));
    CHECK(ok && res == "42");
    CHECK(pump([&] { return v.title() == "T2"; }) && titles.back() == "T2");
    v.evaluate("throw new Error('boom')", [&](bool k, std::string r) { ok = k; res = std::move(r); ++done; });
    CHECK(pump([&] { return done == 2; }) && !ok && res.find("boom") != std::string::npos);
    const std::string file = "/tmp/fuse_webview_" + std::to_string(::getpid()) + ".html";
    if (FILE* f = std::fopen(file.c_str(), "w")) { std::fputs("<title>File</title><p>file</p>", f); std::fclose(f); }
    loads.clear();
    v.navigate("file://" + file);
    CHECK(pump([&] { return !loads.empty() && v.title() == "File"; }));
    CHECK(v.url() == "file://" + file);
    const std::string file2 = file + ".2.html";
    if (FILE* f = std::fopen(file2.c_str(), "w")) { std::fputs("<title>Two</title>", f); std::fclose(f); }
    loads.clear();
    v.navigate("file://" + file2);
    CHECK(pump([&] { return !loads.empty() && v.title() == "Two"; }));
    loads.clear();
    v.back();
    CHECK(pump([&] { return !loads.empty() && v.title() == "File"; }) && v.url() == "file://" + file);
    loads.clear();
    v.forward();
    CHECK(pump([&] { return !loads.empty() && v.title() == "Two"; }));
    std::remove(file2.c_str());
    v.setBounds({10, 10, 320, 240});
    v.setVisible(false);
    v.poll();
    v.destroy();
    CHECK(!v.valid());
    std::remove(file.c_str());
    gateWebViewEmbedded();
}

void gateDeclarative() {
    namespace d = fuse::ui::decl;
    // bindings with automatic dependency tracking
    d::Property<int> w(10), h(20);
    d::Property<int> area([&] { return w() * h(); });
    int changes = 0;
    area.onChanged([&] { ++changes; });
    CHECK(area.peek() == 200 && area.dependencyCount() == 2);
    w = 3;
    CHECK(area.peek() == 60 && changes == 1);
    h = 20; // unchanged: no notification
    CHECK(changes == 1);
    // conditional dependencies are re-tracked each evaluation
    d::Property<bool> useW(true);
    d::Property<int> pick([&] { return useW() ? w() : h(); });
    CHECK(pick.peek() == 3 && pick.dependencyCount() == 2);
    useW = false;
    CHECK(pick.peek() == 20 && pick.dependencyCount() == 2);
    w = 7; // no longer a dependency of pick
    CHECK(pick.peek() == 20 && area.peek() == 140);
    // chained bindings + assignment breaks a binding
    d::Property<std::string> label([&] { return "area " + std::to_string(area()); });
    CHECK(label.peek() == "area 140");
    h = 2;
    CHECK(label.peek() == "area 14");
    area = 1;
    CHECK(!area.hasBinding() && label.peek() == "area 1");
    w = 100;
    CHECK(area.peek() == 1);
    // binding loop detected and broken
    d::Property<int> a(1), b(1);
    a.setBinding([&] { return b() + 1; });
    b.setBinding([&] { return a() + 1; });
    CHECK(a.bindingLoop() || b.bindingLoop());
    // destruction of a dependency detaches it
    {
        auto tmp = std::make_unique<d::Property<int>>(5);
        d::Property<int> dep([&] { return tmp->value() * 2; });
        CHECK(dep.peek() == 10);
        tmp.reset();
        CHECK(dep.dependencyCount() == 0);
    }
    // UI bindings
    Screen s;
    UiContext& ui = s.ui;
    const NodeHandle lbl = w::label(ui, ui.root(), "");
    const NodeHandle le = w::lineEdit(ui, ui.root(), "");
    const NodeHandle sl = w::slider(ui, ui.root(), 0.f, 100.f, 0.f);
    d::Property<std::string> name("Odin");
    d::Property<f32> vol(25.f);
    auto t1 = d::bindText(ui, lbl, [&] { return name() + " @ " + std::to_string(int(vol())); });
    auto t2 = d::bindTextTwoWay(ui, le, name);
    auto t3 = d::bindValueTwoWay(ui, sl, vol);
    step(ui);
    CHECK(ui.text(lbl) == "Odin @ 25" && ui.editBuffer(le).text() == "Odin" && ui.value(sl) == 25.f);
    clickNode(ui, le);
    ui.keyDown(Key::End); ui.keyUp(Key::End);
    ui.textInput("!");
    step(ui);
    CHECK(name.peek() == "Odin!" && ui.text(lbl) == "Odin! @ 25");
    ui.setValue(sl, 60.f, true);
    step(ui);
    CHECK(vol.peek() == 60.f && ui.text(lbl) == "Odin! @ 60");
    t1.reset();
    name = "x";
    CHECK(ui.text(lbl) == "Odin! @ 60"); // effect gone

    // state machine: hierarchy, guards, history, parallel, assignProperty, eventless, done events
    d::StateMachine sm;
    std::vector<std::string> log;
    const auto off = sm.addState("off");
    const auto on = sm.addState("on");
    const auto idle = sm.addState("idle", on);
    const auto play = sm.addState("playing", on);
    const auto hist = sm.addHistory(on);
    const auto broken = sm.addState("broken");
    const auto done = sm.addFinal("done");
    d::Property<int> brightness(0);
    d::Property<int> errors(0);
    sm.assignProperty(on, brightness, 100);
    sm.onEntry(on, [&] { log.push_back("+on"); });
    sm.onExit(on, [&] { log.push_back("-on"); });
    sm.onEntry(play, [&] { log.push_back("+play"); });
    sm.addTransition(off, "power", hist);
    sm.addTransition(on, "power", off);
    sm.addTransition(idle, "play", play);
    sm.addTransition(play, "stop", idle);
    int pings = 0;
    sm.addTransition(on, "ping", d::StateMachine::kNone, {}, [&] { ++pings; }); // targetless
    sm.addTransition(play, "play", idle, [&] { return false; });                  // guard blocks
    sm.addTransition(on, "", broken, [&] { return errors.peek() > 2; });          // eventless
    sm.watch(errors);
    sm.addTransition(broken, "reset", done);
    bool finished = false;
    sm.onFinished = [&] { finished = true; };
    sm.start();
    CHECK(sm.configuration() == std::vector<std::string>{"off"} && brightness.peek() == 0);
    sm.postEvent("power");
    CHECK(sm.isActive(idle) && brightness.peek() == 100);
    sm.postEvent("play");
    sm.postEvent("play"); // guarded: stays
    sm.postEvent("ping");
    CHECK(sm.isActive(play) && pings == 1);
    sm.postEvent("power");
    CHECK(sm.isActive(off) && brightness.peek() == 0);
    sm.postEvent("power"); // history restores "playing"
    CHECK(sm.isActive("playing") && sm.isActive(on));
    CHECK(log == (std::vector<std::string>{"+on", "+play", "-on", "+on", "+play"}));
    errors = 3; // eventless transition via watch()
    CHECK(sm.isActive(broken) && !sm.isActive(on) && brightness.peek() == 0);
    sm.postEvent("reset");
    CHECK(sm.isActive(done) && finished);
    // parallel regions + done.state
    d::StateMachine pm;
    const auto par = pm.addState("par", 0, d::StateMachine::Kind::Parallel);
    const auto r1 = pm.addState("r1", par);
    const auto a1 = pm.addState("a1", r1);
    const auto f1 = pm.addFinal("f1", r1);
    const auto r2 = pm.addState("r2", par);
    pm.addState("b1", r2);
    const auto b2 = pm.addState("b2", r2);
    pm.addTransition(a1, "go", f1);
    pm.addTransition(pm.find("b1"), "go", b2);
    int doneR1 = 0;
    pm.addTransition(par, "done.state.r1", d::StateMachine::kNone, {}, [&] { ++doneR1; });
    pm.start();
    CHECK(pm.configuration() == (std::vector<std::string>{"a1", "b1"}));
    pm.postEvent("go"); // both regions move in one step
    CHECK(pm.configuration() == (std::vector<std::string>{"f1", "b2"}) && doneR1 == 1);
    pm.stop();
    CHECK(pm.configuration().empty());
}

void gateVirtualKeyboard() {
    Screen s;
    UiContext& ui = s.ui;
    const NodeHandle le = w::lineEdit(ui, ui.root(), "");
    const NodeHandle kb = w::virtualKeyboard(ui, ui.root(), "vk");
    int submitted = 0;
    ui.connect(le, SignalKind::Submitted, [&](UiContext&, NodeHandle, const SignalArgs&) { ++submitted; });
    step(ui);
    clickNode(ui, le);
    step(ui);
    w::virtualKeyboardAutoShow(ui, kb);
    step(ui);
    CHECK(ui.rect(kb).h > 60.f);
    auto press = [&](std::string_view l) {
        const NodeHandle k = w::virtualKeyboardKey(ui, kb, l);
        CHECK(k.valid());
        if (k.valid()) clickNode(ui, k);
        step(ui);
        CHECK(ui.focused() == le); // keys never steal focus
    };
    press("Shift");
    press("H"); // shift is one-shot
    press("i");
    press("Space");
    press("?123");
    press("4");
    press("2");
    press("?123");
    press("x");
    press("Backspace");
    CHECK(ui.editBuffer(le).text() == "Hi 42");
    press("Shift");
    press("Shift"); // caps lock
    press("O");
    press("K");
    CHECK(ui.editBuffer(le).text() == "Hi 42OK");
    press("Enter");
    CHECK(submitted == 1);
    ui.setFocus({});
    step(ui);
    w::virtualKeyboardAutoShow(ui, kb);
    step(ui);
    CHECK(ui.rect(kb).h < 1.f);
}

void gateSvg() {
    namespace sv = fuse::ui::svg;
    // transforms
    const sv::Affine t = sv::parseTransform("translate(10,20) scale(2)");
    CHECK(t[0] == 2.f && t[3] == 2.f && t[4] == 10.f && t[5] == 20.f);
    const sv::Affine r = sv::parseTransform("rotate(90 5 5)");
    CHECK(std::abs(r[0]) < 1e-5f && std::abs(r[1] - 1.f) < 1e-5f && std::abs(r[4] - 10.f) < 1e-4f && std::abs(r[5]) < 1e-4f);
    // arcs are real arcs now: a half circle of radius 50 reaches y = 50 below the chord
    {
        Path p = Path::fromSvg("M0 0 A50 50 0 0 0 100 0");
        CHECK(std::abs(p.bounds().h - 50.f) < 1.f && std::abs(p.bounds().w - 100.f) < 0.5f);
    }
    const char* kDoc = R"svg(<?xml version="1.0"?>
<!DOCTYPE svg>
<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="200" height="100" viewBox="0 0 100 50">
  <!-- a comment -->
  <defs>
    <linearGradient id="lg" x1="0" x2="1"><stop offset="0" stop-color="#ff0000"/><stop offset="1" style="stop-color:#0000ff"/></linearGradient>
    <circle id="dot" r="4" fill="lime"/>
  </defs>
  <rect x="0" y="0" width="100" height="50" fill="#202020"/>
  <g id="grp" transform="translate(10 10)" style="fill: rgb(255, 255, 0); stroke:none">
    <rect id="box" width="20" height="20" rx="4"/>
    <circle cx="40" cy="10" r="10" opacity="0.5"/>
  </g>
  <path id="arc" d="M60 40 a10 10 0 1 1 20 0 z" fill="url(#lg)"/>
  <polyline points="0,49 50,30 100,49" fill="none" stroke="white" stroke-width="2"/>
  <line x1="0" y1="0" x2="100" y2="50" stroke="none"/>
  <use xlink:href="#dot" x="90" y="10"/>
  <text x="0" y="0">ignored</text>
</svg>)svg";
    sv::Document d;
    CHECK(d.load(kDoc));
    CHECK(d.size().w == 200.f && d.size().h == 100.f && d.viewBox().w == 100.f);
    CHECK(d.shapes().size() == 6); // bg, box, circle, arc, polyline, use'd dot (line has no paint)
    const Rect box = d.boundsOf("box");
    CHECK(std::abs(box.x - 10.f) < 0.01f && std::abs(box.w - 20.f) < 0.01f);
    const Rect grp = d.boundsOf("grp");
    CHECK(std::abs(grp.right() - 60.f) < 0.5f && std::abs(grp.bottom() - 30.f) < 0.5f);
    CHECK(d.shapes()[3].gradient == kGradientHorizontal);
    CHECK(!sv::Document{}.load("<svg><g></svg>"));
    sv::Document bad;
    CHECK(!bad.load("<html/>") && !bad.error().empty());
    // widget renders it
    Screen s;
    UiContext& ui = s.ui;
    const NodeHandle wdg = w::svgWidget(ui, ui.root(), kDoc, "svg");
    ui.setCss(wdg, "width", "200px");
    for (int i = 0; i < 3; ++i) step(ui);
    const Rect wr = ui.rect(wdg);
    CHECK(std::abs(wr.w - 200.f) < 1.f && std::abs(wr.h - 100.f) < 1.f);
    Image img;
    fuse::ui::test::render(ui, img);
    auto px = [&](f32 dx, f32 dy) {
        const int x = int(wr.x + dx), y = int(wr.y + dy);
        const u8* p = reinterpret_cast<const u8*>(img.pixels.data()) + (std::size_t(y) * img.width + std::size_t(x)) * 4;
        return std::array<int, 3>{p[0], p[1], p[2]};
    };
    const auto yellow = px(40, 40), bg = px(190, 80), dot = px(180, 20), arcL = px(124, 74), arcR = px(156, 74);
    CHECK(yellow[0] > 200 && yellow[1] > 200 && yellow[2] < 60);
    CHECK(bg[0] < 60 && bg[1] < 60 && bg[2] < 60);
    CHECK(dot[1] > 200 && dot[0] < 60);
    CHECK(arcL[0] > arcL[2] && arcR[2] > arcR[0]); // red -> blue gradient
    if (const std::string dir = fuse::ui::os::env("FUSE_EXTRAS_OUT"); !dir.empty())
        stbi_write_png((dir + "/svg.png").c_str(), img.width, img.height, 4, img.pixels.data(), img.width * 4);
}

void gateCharts() {
    Screen s;
    UiContext& ui = s.ui;
    ui.setCss(ui.root(), "flex-direction", "column");
    // pie: three slices, legend on the right, explicit colours
    const NodeHandle pie = w::chart(ui, ui.root(), w::ChartKind::Pie, "pie");
    ui.setCss(pie, "width", "360px");
    ui.setCss(pie, "height", "220px");
    const f32 pv[] = {1.f, 1.f, 2.f};
    w::chartSetSeries(ui, pie, 0, "share", pv, Color::rgb(0xFFFFFF));
    const Color sc[] = {Color::rgb(0xFF0000), Color::rgb(0x00FF00), Color::rgb(0x0000FF)};
    w::chartSetSliceColors(ui, pie, sc);
    w::ChartAxis cats;
    cats.categories = {"Red", "Green", "Blue"};
    w::chartSetAxis(ui, pie, w::ChartAxisId::X, cats);
    w::chartSetLegend(ui, pie, w::ChartLegend::Right);
    // donut + line chart with axis customisation
    const NodeHandle donut = w::chart(ui, ui.root(), w::ChartKind::Donut, "donut");
    ui.setCss(donut, "width", "300px");
    ui.setCss(donut, "height", "200px");
    w::chartSetSeries(ui, donut, 0, "d", pv, Color::rgb(0xFFFFFF));
    w::ChartAxis dy;
    dy.title = "Total";
    w::chartSetAxis(ui, donut, w::ChartAxisId::Y, dy);
    const NodeHandle line = w::chart(ui, ui.root(), w::ChartKind::Line, "line");
    ui.setCss(line, "width", "500px");
    ui.setCss(line, "height", "240px");
    std::vector<f32> a(40), b(40);
    for (u32 i = 0; i < 40; ++i) { a[i] = f32(i); b[i] = 40.f - f32(i); }
    w::chartSetSeries(ui, line, 0, "Up", a, Color::rgb(0x3D8BFD));
    w::chartSetSeries(ui, line, 1, "Down", b, Color::rgb(0xFFB347));
    w::ChartAxis yx;
    yx.min = 0.f; yx.max = 50.f; yx.ticks = 5; yx.format = "%.0f ms"; yx.title = "Latency";
    w::chartSetAxis(ui, line, w::ChartAxisId::Y, yx);
    w::ChartAxis xx;
    xx.title = "Frame";
    w::chartSetAxis(ui, line, w::ChartAxisId::X, xx);
    w::chartSetLegend(ui, line, w::ChartLegend::Bottom);
    CHECK(w::chartAxis(ui, line, w::ChartAxisId::Y).format == "%.0f ms");
    for (int i = 0; i < 3; ++i) step(ui);
    // pie pixels: slice 0 top-right, 1 bottom-right, 2 left
    Image img;
    fuse::ui::test::render(ui, img);
    auto px = [&](f32 x, f32 y) {
        const u8* p = reinterpret_cast<const u8*>(img.pixels.data()) + (std::size_t(int(y)) * img.width + std::size_t(int(x))) * 4;
        return std::array<int, 3>{p[0], p[1], p[2]};
    };
    const Rect pp = w::chartPlotRect(ui, pie);
    const Vec2 c{pp.x + pp.w * 0.5f, pp.y + pp.h * 0.5f};
    const f32 r = std::min(pp.w, pp.h) * 0.25f;
    const auto tr = px(c.x + r, c.y - r), br = px(c.x + r, c.y + r), lf = px(c.x - r, c.y);
    CHECK(tr[0] > 200 && tr[1] < 60 && tr[2] < 60);
    CHECK(br[1] > 200 && br[0] < 60);
    CHECK(lf[2] > 200 && lf[0] < 60);
    const auto legend = w::chartLegendRects(ui, pie);
    CHECK(legend.size() == 3 && legend[0].x > pp.right());
    // donut hole is background
    const Rect dp = w::chartPlotRect(ui, donut);
    const Vec2 dc{dp.x + dp.w * 0.5f, dp.y + dp.h * 0.5f};
    const auto hole = px(dc.x + 2.f, dc.y + 14.f), ring = px(dc.x + (std::min(dp.w, dp.h) * 0.5f - 8.f) * 0.8f, dc.y - 3.f);
    CHECK(hole[0] < 80 && hole[1] < 80 && hole[2] < 80);
    CHECK(ring[0] > 150 || ring[1] > 150 || ring[2] > 150);
    if (const std::string dir = fuse::ui::os::env("FUSE_EXTRAS_OUT"); !dir.empty())
        stbi_write_png((dir + "/charts.png").c_str(), img.width, img.height, 4, img.pixels.data(), img.width * 4);
    // pie tooltip on hover: name, value, percentage
    ui.pointerMove({c.x - r, c.y});
    step(ui);
    CHECK(ui.value(pie) == 2.f);
    auto tip = w::chartTooltip(ui, pie);
    CHECK(tip.size() == 1 && tip[0] == "Blue: 2 (50.0%)");
    // legend click hides a slice: the remaining two split 50/50
    std::vector<std::pair<u32, bool>> toggles;
    ui.connect(pie, SignalKind::Toggled, [&](UiContext&, NodeHandle, const SignalArgs& g) { toggles.push_back({u32(g.value), g.flag}); });
    const Vec2 l2{legend[2].x + 5.f, legend[2].y + legend[2].h * 0.5f};
    ui.pointerMove(l2);
    ui.pointerDown(l2);
    ui.pointerUp(l2);
    step(ui);
    CHECK(toggles.size() == 1 && toggles[0].first == 2 && !toggles[0].second && !w::chartEntryVisible(ui, pie, 2));
    ui.pointerMove({c.x + r, c.y - r});
    step(ui);
    tip = w::chartTooltip(ui, pie);
    CHECK(tip.size() == 1 && tip[0] == "Red: 1 (50.0%)");
    w::chartSetEntryVisible(ui, pie, 2, true);
    // line: hover tooltip uses the axis format; legend at the bottom
    const Rect lp = w::chartPlotRect(ui, line);
    const auto ll = w::chartLegendRects(ui, line);
    CHECK(ll.size() == 2 && ll[0].y > lp.bottom());
    ui.pointerMove({lp.x + lp.w * 10.f / 39.f, lp.y + lp.h * 0.5f});
    step(ui);
    CHECK(ui.value(line) == 10.f);
    tip = w::chartTooltip(ui, line);
    CHECK(tip.size() == 3 && tip[0] == "Frame 10" && tip[1] == "Up: 10 ms" && tip[2] == "Down: 30 ms");
    // wheel zoom around the pointer keeps the pointed index in place
    f32 v0, v1;
    w::chartXView(ui, line, v0, v1);
    CHECK(v0 == 0.f && v1 == 39.f);
    ui.wheel({lp.x + lp.w * 10.f / 39.f, lp.y + lp.h * 0.5f}, {0.f, 2.f});
    step(ui);
    w::chartXView(ui, line, v0, v1);
    CHECK(std::abs((v1 - v0) - 39.f * 0.64f) < 0.01f && std::abs(v0 + (v1 - v0) * 10.f / 39.f - 10.f) < 0.01f);
    // drag pans; pans clamp to the data
    const f32 span = v1 - v0;
    ui.pointerDown({lp.x + 200.f, lp.y + 20.f});
    ui.pointerMove({lp.right() - 1.f, lp.y + 20.f});
    ui.pointerMove({lp.right() + 400.f, lp.y + 20.f});
    ui.pointerUp({lp.right() + 400.f, lp.y + 20.f});
    step(ui);
    w::chartXView(ui, line, v0, v1);
    CHECK(v0 == 0.f && std::abs(v1 - v0 - span) < 0.01f);
    ui.setFocus(line);
    ui.keyDown(Key::Right);
    w::chartXView(ui, line, v0, v1);
    CHECK(v0 > 0.f);
    ui.keyDown(Key::Home);
    w::chartXView(ui, line, v0, v1);
    CHECK(v0 == 0.f && v1 == 39.f);
    // hiding a series removes it from the tooltip
    w::chartSetEntryVisible(ui, line, 1, false);
    ui.pointerMove({lp.x + lp.w * 10.f / 39.f + 1.f, lp.y + lp.h * 0.5f});
    step(ui);
    tip = w::chartTooltip(ui, line);
    CHECK(tip.size() == 2 && tip[1] == "Up: 10 ms");
    ui.pointerMove({1190.f, 790.f});
    step(ui);
    CHECK(w::chartTooltip(ui, line).empty());
}
} // namespace

int main(int argc, char** argv) {
    const std::map<std::string, std::function<void()>> gates{
        {"undo", gateUndo}, {"gesture", gateGesture}, {"i18n", gateI18n}, {"desktop", gateDesktop}, {"pdf", gatePdf}, {"containers", gateContainers}, {"dialogs", gateDialogs}, {"lcd_keyseq", gateLcdKeyseq},
        {"mdi", gateMdi}, {"completer", gateCompleter}, {"graphics", gateGraphics}, {"widget_pdf", gateWidgetPdf}, {"tray", gateTray}, {"richtext", gateRichText}, {"webview", gateWebView}, {"declarative", gateDeclarative}, {"vkeyboard", gateVirtualKeyboard}, {"svg", gateSvg}, {"charts", gateCharts}};
    std::string only;
    for (int i = 1; i < argc; ++i) if (std::strncmp(argv[i], "--gate=", 7) == 0) only = argv[i] + 7;
    for (auto& [n, f] : gates) if (only.empty() || only == n) { f(); std::printf("gate %s: %s\n", n.c_str(), g_fail ? "FAIL" : "ok"); }
    return g_fail ? 1 : 0;
}
