// Qt-parity extras gates (--gate=<name>; no argument runs all): undo gesture i18n desktop pdf
#include "fuse/ui/extras/extras.hpp"
#include "fuse/ui/os.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <string>

using namespace fuse::ui;

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
    CHECK(pdf.find("/Count 2") != std::string::npos && pdf.find("/ImageMask true") != std::string::npos);
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
} // namespace

int main(int argc, char** argv) {
    const std::map<std::string, std::function<void()>> gates{
        {"undo", gateUndo}, {"gesture", gateGesture}, {"i18n", gateI18n}, {"desktop", gateDesktop}, {"pdf", gatePdf}};
    std::string only;
    for (int i = 1; i < argc; ++i) if (std::strncmp(argv[i], "--gate=", 7) == 0) only = argv[i] + 7;
    for (auto& [n, f] : gates) if (only.empty() || only == n) { f(); std::printf("gate %s: %s\n", n.c_str(), g_fail ? "FAIL" : "ok"); }
    return g_fail ? 1 : 0;
}
