// Qt-parity extras gates (--gate=<name>; no argument runs all): undo gesture i18n desktop pdf containers dialogs
// lcd_keyseq mdi completer graphics widget_pdf. FUSE_EXTRAS_OUT=<dir> writes extras.png / extras.pdf.
#include "test_util.hpp"
#include <atomic>
#include <cstdlib>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>
#include <unistd.h>
#include "fuse/ui/extras/dbus.hpp"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
// main after the gui-framework merge builds part of fuse_ui_extras (undo, desktop, pdf, tray, web view, declarative,
// 3D data visualisation, charts). The gates of the parked modules (i18n, the widget PDF export, SVG, media, the
// widgets chart kinds) compile only with -DFUSE_UI_EXTRAS_ALL=1; see docs/ui/archive/PORT_gui_framework.md. (Rich text,
// gesture and the audio meter are live: test_richtext.cpp, test_extras_small.cpp; the extra widgets are in
// fuse_ui_widgets: test_widgets_extra.cpp, test_completer.cpp.)
#ifndef FUSE_UI_EXTRAS_ALL
#define FUSE_UI_EXTRAS_ALL 0
#endif
#if FUSE_UI_EXTRAS_ALL
#include "fuse/ui/extras/extras.hpp"
#include "fuse/ui/extras/svg.hpp"
#include "fuse/ui/extras/media.hpp"
#else
#include "fuse/ui/extras/desktop.hpp"
#include "fuse/ui/extras/pdf.hpp"
#include "fuse/ui/extras/tray.hpp"
#include "fuse/ui/extras/undo.hpp"
#include "fuse/ui/extras/webview.hpp"
#include "fuse/ui/widgets/widgets.hpp"
#endif
#include "fuse/ui/extras/declarative.hpp"
#include "fuse/ui/extras/datavis3d.hpp"
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

#if FUSE_UI_EXTRAS_ALL
// (the gesture recogniser gate moved to test_extras_small.cpp)
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

#endif // FUSE_UI_EXTRAS_ALL
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
void step(UiContext& ui) { ui.update(1.f / 60.f); }
Vec2 ctr(Rect r) { return {r.x + r.w * 0.5f, r.y + r.h * 0.5f}; }
void click(UiContext& ui, Vec2 p) { ui.pointerMove(p); ui.pointerDown(p); ui.pointerUp(p); step(ui); }
void clickNode(UiContext& ui, NodeHandle n) { click(ui, ctr(ui.rect(n))); }
struct Screen {
    UiContext ui{{.viewport = {1200, 800}, .font = &fuse::ui::test::font()}};
    Screen() { w::install(ui); step(ui); }
};
[[maybe_unused]] NodeHandle findText(UiContext& ui, NodeHandle root, std::string_view t) {
    if (ui.text(root) == t) return root;
    for (NodeHandle c = ui.firstChild(root); ui.alive(c); c = ui.nextSibling(c))
        if (NodeHandle f = findText(ui, c, t); f.valid()) return f;
    return {};
}

#if FUSE_UI_EXTRAS_ALL
// (The gates containers / dialogs / lcd_keyseq / mdi / completer / graphics / vkeyboard moved out with their widgets:
// those are part of fuse_ui_widgets now and gated by tests/test_widgets_extra.cpp, test_completer.cpp.)
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
#endif // FUSE_UI_EXTRAS_ALL
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

// (the rich text gates moved to test_richtext.cpp: the editor is live on main's text stack)
#if FUSE_UI_EXTRAS_ALL
// System web engine (WebKitGTK here; run under Xvfb). Skips cleanly when no engine / display is present.
// Embedded child in a foreign X parent: map timing (parent mapped after the child), geometry in parent coordinates
// after resizes, hide/show, explicit + click focus.
#endif // FUSE_UI_EXTRAS_ALL
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
    // The engine reports the title on its own channel (WebKitGTK: notify::title from the page-load-state commit),
    // which can trail load-finished and the script message under load; the title is only guaranteed eventually.
    CHECK(pump([&] { return v.title() == "T1" && !titles.empty() && titles.back() == "T1"; }));
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

#if FUSE_UI_EXTRAS_ALL
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
  <text x="0" y="0">t</text>
</svg>)svg";
    sv::Document d;
    CHECK(d.load(kDoc));
    CHECK(d.size().w == 200.f && d.size().h == 100.f && d.viewBox().w == 100.f);
    CHECK(d.shapes().size() == 7); // bg, box, circle, arc, polyline, use'd dot, text (line has no paint)
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

    // ---- multi-stop gradients at any angle, <text>, clipPath, mask, pattern ----
    const char* kAdv = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="400" height="300" viewBox="0 0 400 300">
  <defs>
    <linearGradient id="diag" x1="0" y1="0" x2="1" y2="1">
      <stop offset="0" stop-color="#ff0000"/><stop offset="50%" stop-color="#00ff00"/><stop offset="1" stop-color="#0000ff"/>
    </linearGradient>
    <linearGradient id="rep" href="#diag" x1="10" y1="0" x2="30" y2="0" gradientUnits="userSpaceOnUse" spreadMethod="reflect"/>
    <radialGradient id="rg" gradientTransform="rotate(30 0.5 0.5)" r="0.5">
      <stop offset="0" stop-color="white"/><stop offset="0.5" stop-color="yellow"/><stop offset="1" stop-color="black"/>
    </radialGradient>
    <clipPath id="circ"><circle cx="250" cy="50" r="40"/></clipPath>
    <clipPath id="bb" clipPathUnits="objectBoundingBox"><rect x="0" y="0" width="0.5" height="1"/></clipPath>
    <mask id="half"><rect x="0" y="0" width="400" height="300" fill="white" fill-opacity="0.5"/></mask>
    <pattern id="chk" x="0" y="0" width="20" height="20" patternUnits="userSpaceOnUse">
      <rect width="10" height="10" fill="red"/><rect x="10" y="10" width="10" height="10" fill="red"/>
    </pattern>
  </defs>
  <rect width="400" height="300" fill="black"/>
  <rect id="g1" x="10" y="10" width="100" height="100" fill="url(#diag)"/>
  <rect x="200" y="0" width="100" height="100" fill="#00ffff" clip-path="url(#circ)"/>
  <g clip-path="url(#bb)"><rect x="320" y="10" width="60" height="60" fill="white"/></g>
  <rect x="10" y="130" width="100" height="40" fill="white" mask="url(#half)"/>
  <rect x="130" y="130" width="80" height="80" fill="url(#chk)"/>
  <circle cx="300" cy="170" r="40" fill="url(#rg)"/>
  <rect x="10" y="190" width="100" height="10" fill="url(#rep)"/>
  <text id="t1" x="200" y="280" font-size="40" text-anchor="middle" fill="white">Hi <tspan fill="#ff00ff" dy="-5">SVG</tspan></text>
  <text id="t2" x="10" y="250" style="font-size:20px" fill="none" stroke="yellow" stroke-width="1">Outline</text>
</svg>)svg";
    sv::Document a;
    CHECK(a.load(kAdv));
    CHECK(a.clips().size() == 3 && a.patterns().size() == 1);
    {
        const Rect t1 = a.boundsOf("t1");
        CHECK(t1.w > 60.f && std::abs((t1.x + t1.w * 0.5f) - 200.f) < 6.f && t1.bottom() <= 282.f && t1.y > 230.f); // centred
        CHECK(!a.boundsOf("t2").empty());
    }
    DrawList adl;
    a.render(adl, {0, 0, 400, 300});
    Image ai;
    ai.resize(400, 300, 0xFF000000u);
    CpuRasterizer ar;
    ar.render(adl, ai);
    auto ap = [&](int x, int y) {
        const u32 v = ai.at(x, y);
        return std::array<int, 3>{int(v & 0xFF), int((v >> 8) & 0xFF), int((v >> 16) & 0xFF)};
    };
    const auto gTL = ap(14, 14), gMid = ap(60, 60), gBR = ap(106, 106), gOff = ap(100, 20); // 3 stops on the diagonal
    CHECK(gTL[0] > 200 && gTL[1] < 60);
    CHECK(gMid[1] > 200 && gMid[0] < 60 && gMid[2] < 60);
    CHECK(gBR[2] > 200 && gBR[1] < 60);
    CHECK(gOff[1] > 200); // (100,20) is on the t = 0.5 diagonal: green
    CHECK(ap(250, 50)[2] > 200 && ap(205, 5)[2] < 30); // clipPath circle: inside cyan, rect corner clipped
    CHECK(ap(330, 40)[0] > 200 && ap(370, 40)[0] < 30); // bbox clip: left half only
    const auto m = ap(60, 150);
    CHECK(m[0] > 100 && m[0] < 160); // mask: 50% luminance
    CHECK(ap(135, 135)[0] > 200 && ap(145, 135)[0] < 30 && ap(145, 145)[0] > 200 && ap(135, 145)[0] < 30); // checker tiles
    CHECK(ap(300, 170)[0] > 200 && ap(300, 170)[2] > 200); // radial centre white
    CHECK(ap(12, 195)[0] > 150 && ap(28, 195)[2] > 150 && ap(48, 195)[0] > 150); // reflect: red, blue, red again
    int textWhite = 0, textMagenta = 0, outline = 0;
    for (int y = 230; y < 290; ++y)
        for (int x = 100; x < 300; ++x) {
            const auto c = ap(x, y);
            textWhite += c[0] > 200 && c[1] > 200 && c[2] > 200;
            textMagenta += c[0] > 200 && c[1] < 60 && c[2] > 200;
        }
    for (int y = 230; y < 255; ++y)
        for (int x = 10; x < 90; ++x) { const auto c = ap(x, y); outline += c[0] > 150 && c[1] > 150 && c[2] < 60; }
    CHECK(textWhite > 100 && textMagenta > 150 && outline > 60);
    if (const std::string dir = fuse::ui::os::env("FUSE_EXTRAS_OUT"); !dir.empty())
        stbi_write_png((dir + "/svg_advanced.png").c_str(), ai.width, ai.height, 4, ai.pixels.data(), ai.width * 4);
}

// BSP depth ordering: two interpenetrating quads (A: z = 0, B: z = x) cross at x = 0, so no whole-polygon order is
// right; bspOrder splits one and the painted result shows B in front for x > 0 and A in front for x < 0, in both
// orthographic and perspective views. A cyclic overlap of three slats also resolves.
#endif // FUSE_UI_EXTRAS_ALL
void gateDataViz3dBsp() {
    namespace d3 = fuse::ui::dv3d;
    const std::vector<std::vector<d3::Vec3>> polys = {
        {{-1, -1, 0}, {1, -1, 0}, {1, 1, 0}, {-1, 1, 0}},     // A
        {{-1, -0.5f, -1}, {1, -0.5f, 1}, {1, 0.5f, 1}, {-1, 0.5f, -1}}}; // B
    for (bool persp : {false, true}) {
        const auto frags = d3::bspOrder(polys, persp, 5.f);
        CHECK(frags.size() == 3);
        // the split pieces mark exactly their shared edge (on x = 0) as a cut; all other edges are source edges
        int cuts = 0;
        for (const auto& f : frags) {
            CHECK(f.cut.empty() || f.cut.size() == f.pts.size());
            for (std::size_t j = 0; j < f.cut.size(); ++j)
                if (f.cut[j]) {
                    ++cuts;
                    const auto &p0 = f.pts[j], &p1 = f.pts[(j + 1) % f.pts.size()];
                    CHECK(std::abs(p0.x) < 1e-3f && std::abs(p1.x) < 1e-3f && std::abs(p0.z) < 1e-3f);
                }
        }
        CHECK(cuts == 2);
        Image img;
        img.resize(300, 300, 0xFF000000u);
        DrawList dl;
        for (const auto& f : frags) {
            Path p;
            for (std::size_t j = 0; j < f.pts.size(); ++j) {
                const f32 k = persp ? 5.f / (5.f - f.pts[j].z) : 1.f;
                const Vec2 sp{150.f + f.pts[j].x * k * 100.f, 150.f - f.pts[j].y * k * 100.f};
                if (j == 0) p.moveTo(sp);
                else p.lineTo(sp);
            }
            p.close();
            dl.fillPath(p, Brush(f.source == 0 ? Color::rgb(0xFF0000) : Color::rgb(0x0000FF)));
        }
        CpuRasterizer().render(dl, img);
        const u32 right = img.at(persp ? 215 : 210, 150), left = img.at(persp ? 92 : 90, 150);
        CHECK((right & 0xFF) < 40 && ((right >> 16) & 0xFF) > 200); // blue in front at x > 0
        CHECK((left & 0xFF) > 200 && ((left >> 16) & 0xFF) < 40);   // red in front at x < 0
    }
    // cyclic overlap: three slats each over the next (tilted in z along their length)
    const std::vector<std::vector<d3::Vec3>> cyc = {
        {{-1, 0.2f, 0.3f}, {1, 0.2f, -0.3f}, {1, 0.4f, -0.3f}, {-1, 0.4f, 0.3f}},
        {{0.3f, -1, -0.3f}, {0.5f, -1, -0.3f}, {0.5f, 1, 0.3f}, {0.3f, 1, 0.3f}},
        {{-0.5f, -1, 0.3f}, {-0.3f, -1, 0.3f}, {-0.3f, 1, -0.3f}, {-0.5f, 1, -0.3f}}};
    const auto cf = d3::bspOrder(cyc, false);
    CHECK(cf.size() > 3); // at least one split was needed
    // every overlapping pair of fragments is painted far before near: sample the crossing points
    auto zAt = [](const std::vector<d3::Vec3>& q, f32 x, f32 y, f32& z) {
        // planar quad: barycentric over its two triangles
        for (std::size_t t = 1; t + 1 < q.size(); ++t) {
            const d3::Vec3 a = q[0], b = q[t], c = q[t + 1];
            const f32 d = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
            if (std::abs(d) < 1e-9f) continue;
            const f32 l0 = ((b.y - c.y) * (x - c.x) + (c.x - b.x) * (y - c.y)) / d;
            const f32 l1 = ((c.y - a.y) * (x - c.x) + (a.x - c.x) * (y - c.y)) / d;
            const f32 l2 = 1.f - l0 - l1;
            if (l0 >= -1e-4f && l1 >= -1e-4f && l2 >= -1e-4f) { z = l0 * a.z + l1 * b.z + l2 * c.z; return true; }
        }
        return false;
    };
    int bad = 0, pairs = 0;
    for (f32 x = -0.95f; x < 1.f; x += 0.05f)
        for (f32 y = -0.95f; y < 1.f; y += 0.05f) {
            f32 lastZ = -1e9f;
            for (const auto& f : cf) {
                f32 z;
                if (!zAt(f.pts, x, y, z)) continue;
                ++pairs;
                if (z < lastZ - 1e-3f) ++bad;
                lastZ = z;
            }
        }
    CHECK(pairs > 0 && bad == 0);
    // translucent split surface: the cut edges between pieces of one face are covered (no lighter seam lines).
    // A seam pixel is a local brightness dip across a 1-px line: darker than both neighbours on one axis while those
    // neighbours agree. Dump: FUSE_DUMP_DV3D=<png>.
    {
        d3::Scene sc;
        sc.kind = d3::Kind::Surface;
        sc.rows = sc.cols = 14;
        for (u32 r = 0; r < 14; ++r)
            for (u32 c = 0; c < 14; ++c) sc.values.push_back(std::sin(f32(c) * 0.9f) * std::cos(f32(r) * 0.7f));
        sc.colormap = {{0.f, Color::rgb(0x40A0FF, 0.5f)}, {1.f, Color::rgb(0xFFC040, 0.5f)}};
        sc.wireframe = false, sc.shading = false, sc.grid = false;
        sc.camera.pitch = 35.f;
        DrawList dl;
        d3::render(sc, dl, {0, 0, 640, 480}, Color::rgb(0x808080));
        Image img;
        img.resize(640, 480, 0xFF101010u);
        CpuRasterizer().render(dl, img);
        int dips = 0;
        auto lum = [&](int x, int y) { const u32 p = img.at(x, y); return int(p & 0xFF) + int((p >> 8) & 0xFF) + int((p >> 16) & 0xFF); };
        for (int y = 1; y < 479; ++y)
            for (int x = 1; x < 639; ++x) {
                const int m = lum(x, y);
                const int l = lum(x - 1, y), r = lum(x + 1, y), u = lum(x, y - 1), d = lum(x, y + 1);
                dips += (std::abs(l - r) < 6 && m < std::min(l, r) - 24) || (std::abs(u - d) < 6 && m < std::min(u, d) - 24);
            }
        std::printf("  dataviz3d translucent seams: %d dip pixels\n", dips);
        if (const char* out = std::getenv("FUSE_DUMP_DV3D")) stbi_write_png(out, 640, 480, 4, img.pixels.data(), 640 * 4);
        CHECK(dips < 120); // 767 without the cover
    }
}

void gateDataViz3d() {
    namespace d3 = fuse::ui::dv3d;
    Screen s;
    UiContext& ui = s.ui;
    ui.setCss(ui.root(), "flex-direction", "row");
    ui.setCss(ui.root(), "flex-wrap", "wrap");
    const NodeHandle surf = w::dataViz3d(ui, ui.root(), d3::Kind::Surface, "surf");
    const NodeHandle scat = w::dataViz3d(ui, ui.root(), d3::Kind::Scatter, "scat");
    const NodeHandle bars = w::dataViz3d(ui, ui.root(), d3::Kind::Bars, "bars");
    for (NodeHandle n : {surf, scat, bars}) { ui.setCss(n, "width", "320px"); ui.setCss(n, "height", "260px"); }
    std::vector<f32> grid;
    for (u32 r = 0; r < 16; ++r)
        for (u32 c = 0; c < 16; ++c) grid.push_back(std::exp(-((f32(c) - 7.5f) * (f32(c) - 7.5f) + (f32(r) - 7.5f) * (f32(r) - 7.5f)) / 18.f));
    w::dataViz3dSetGrid(ui, surf, 16, 16, grid);
    w::dataViz3dScene(ui, surf)->axis[1].title = "height";
    std::vector<d3::Vec3> pts;
    for (int i = 0; i < 80; ++i) pts.push_back({std::cos(f32(i) * 0.25f), f32(i) / 80.f, std::sin(f32(i) * 0.25f)});
    w::dataViz3dSetPoints(ui, scat, pts);
    const f32 bv[] = {1, 2, 3, 2, 4, 6, 3, 6, 9};
    w::dataViz3dSetGrid(ui, bars, 3, 3, bv);
    for (int i = 0; i < 3; ++i) step(ui);
    Image img;
    fuse::ui::test::render(ui, img);
    auto px = [&](Vec2 p) {
        const u32 v = img.at(int(p.x), int(p.y));
        return std::array<int, 3>{int(v & 0xFF), int((v >> 8) & 0xFF), int((v >> 16) & 0xFF)};
    };
    auto content = [&](NodeHandle n) { return ui.rect(n); };
    // surface: the peak (top of the colormap) is red, the rim (bottom) blue
    const d3::Scene* ss = w::dataViz3dScene(ui, surf);
    const Vec2 peak = d3::project(*ss, content(surf), {7.5f, 0.95f, 7.5f});
    const auto pk = px({peak.x, peak.y + 3.f});
    CHECK(pk[0] > 150 && pk[2] < 120);
    const Vec2 rim = d3::project(*ss, content(surf), {1.f, 0.f, 14.f});
    const auto rm = px(rim);
    CHECK(rm[2] > 120 && rm[0] < 120);
    // hover a scatter point -> value + tooltip
    const d3::Scene* sc = w::dataViz3dScene(ui, scat);
    const Vec2 p40 = d3::project(*sc, content(scat), pts[40]);
    ui.pointerMove(p40);
    step(ui);
    const i32 hov = w::dataViz3dHovered(ui, scat);
    CHECK(hov >= 0 && std::abs(pts[std::size_t(hov)].x - pts[40].x) < 0.2f);
    const auto tip = w::dataViz3dTooltip(ui, scat);
    CHECK(tip.size() == 3 && tip[0].rfind("x: ", 0) == 0);
    // drag orbits, wheel zooms, Home resets
    const f32 yaw0 = w::dataViz3dCamera(ui, scat).yaw;
    ui.pointerDown(p40);
    ui.pointerMove({p40.x + 40.f, p40.y + 20.f});
    ui.pointerUp({p40.x + 40.f, p40.y + 20.f});
    step(ui);
    const d3::Camera c1 = w::dataViz3dCamera(ui, scat);
    CHECK(std::abs(c1.yaw - (yaw0 + 20.f)) < 0.01f && std::abs(c1.pitch - 35.f) < 0.01f);
    ui.wheel(p40, {0.f, 2.f});
    step(ui);
    CHECK(std::abs(w::dataViz3dCamera(ui, scat).zoom - 1.21f) < 0.01f);
    ui.keyDown(Key::Home);
    step(ui);
    CHECK(std::abs(w::dataViz3dCamera(ui, scat).yaw - yaw0) < 0.01f);
    // bars: the tallest bar's top is pickable and lit; tooltip has row / column / value
    const d3::Scene* sb = w::dataViz3dScene(ui, bars);
    const Vec2 top = d3::project(*sb, content(bars), {2.f, 9.f, 2.f});
    ui.pointerMove(top);
    step(ui);
    CHECK(w::dataViz3dHovered(ui, bars) == 8);
    const auto bt = w::dataViz3dTooltip(ui, bars);
    CHECK(bt.size() == 3 && bt[2] == "value: 9");
    fuse::ui::test::render(ui, img);
    const auto barTop = px({top.x, top.y + 4.f});
    CHECK(barTop[0] + barTop[1] + barTop[2] > 200);
    // orthographic camera keeps the projection centred and finite
    d3::Scene o = *sb;
    o.camera.perspective = false;
    const Vec2 oc = d3::project(o, {0, 0, 200, 200}, {1.f, 4.5f, 1.f});
    CHECK(std::abs(oc.x - 100.f) < 1.f && std::abs(oc.y - 100.f) < 1.f);
    if (const std::string dir = fuse::ui::os::env("FUSE_EXTRAS_OUT"); !dir.empty())
        stbi_write_png((dir + "/dataviz3d.png").c_str(), img.width, img.height, 4, img.pixels.data(), img.width * 4);
}

#if FUSE_UI_EXTRAS_ALL
void gateMedia() {
    Screen s;
    UiContext& ui = s.ui;
    ui.setCss(ui.root(), "flex-direction", "row");
    ui.setCss(ui.root(), "align-items", "flex-start");
    // (the audio level meter gate moved to test_extras_small.cpp)
    Image img;
    // ---- system video decoder (runtime-loaded); missing files fail cleanly
    fuse::ui::media::SystemVideo bad;
    std::string err;
    CHECK(!bad.openFile("/nonexistent/clip.mp4", 10, &err) && !err.empty() && !bad.valid());
    std::printf("system video: %s\n", fuse::ui::media::SystemVideo::available() ? fuse::ui::media::SystemVideo::backendName().c_str() : "unavailable");
    fuse::ui::media::SystemVideo sv;
    std::vector<const Image*> images;
    NodeHandle vid{};
    if (fuse::ui::media::SystemVideo::available()) {
        // a checked-in MJPEG + PCM AVI clip (15 frames 32x24 @ 15 fps SMPTE bars, 1 s 440 Hz sine at 0.5, mono 22050)
        // through whichever backend this platform has: GStreamer, Media Foundation (on Wine via winegstreamer), AVF
        std::string clip = FUSE_UI_TEST_DATA "/media_clip.avi";
#ifdef _WIN32
        if (clip[0] == '/') clip = "Z:" + clip; // Wine: the unix root is drive Z
#endif
        fuse::ui::media::SystemVideo cv;
        if (cv.openFile(clip, 1000, &err)) {
            CHECK(cv.width() == 32 && cv.height() == 24 && std::abs(cv.fps() - 15.f) < 0.01f);
            CHECK(cv.frameCount() >= 14 && cv.frameCount() <= 16);
            const u32* f0 = cv.frame(0);
            CHECK(f0 && (f0[2 * 32 + 1] & 0xFF) > 140 && ((f0[2 * 32 + 1] >> 16) & 0xFF) > 140);
            CHECK(f0 && ((f0[2 * 32 + 6] >> 16) & 0xFF) < 60 && (f0[2 * 32 + 6] & 0xFF) > 150); // second bar yellow
            CHECK(f0 && (f0[22 * 32 + 1] & 0xFF) < 60); // bottom-left dark blue: rows are not flipped
            if (f0) std::printf("  clip px top-left %08x, bar2 %08x, bottom %08x\n", f0[2 * 32 + 1], f0[2 * 32 + 6], f0[22 * 32 + 1]);
            CHECK(cv.frame(cv.frameCount() - 1) && cv.frame(1) && cv.frame(1) != nullptr);
            CHECK(cv.hasAudio() && cv.audioRate() == 22050 && cv.audioChannels() == 1);
            std::atomic<u32> n{0};
            std::atomic<f32> pk{0.f};
            cv.setAudioTap([&](std::span<const f32> s, u32 ch, u32) {
                f32 p = pk;
                for (f32 x : s) p = std::max(p, std::abs(x));
                pk = p, n += u32(s.size() / ch);
            });
            cv.setAudioOutputEnabled(false); // paced by the clock: no device needed
            CHECK(cv.playAudio(0.0, &err));
            for (int i = 0; i < 400 && cv.audioPlaying(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            CHECK(n > 20000 && n < 24000 && pk > 0.4f && pk < 0.6f);
            std::printf("  clip via %s: %u frames, audio %u frames peak %.3f, %u decoded %u seeks\n", fuse::ui::media::SystemVideo::backendName().c_str(),
                        cv.frameCount(), n.load(), double(pk.load()), cv.decodedFrames(), cv.seekCount());
        } else {
            std::printf("  clip not decodable here (%s)\n", err.c_str());
            CHECK(fuse::ui::media::SystemVideo::backendName().rfind("gstreamer", 0) != 0); // GStreamer must play it
        }
    }
    if (fuse::ui::media::SystemVideo::available() && fuse::ui::media::SystemVideo::backendName().rfind("gstreamer", 0) == 0) {
        CHECK(!fuse::ui::media::SystemVideo::backendName().empty());
        const bool ok = sv.openPipeline("videotestsrc num-buffers=12 pattern=smpte ! video/x-raw,width=64,height=48,framerate=25/1", 100, &err);
        CHECK(ok);
        if (!ok) std::printf("  %s\n", err.c_str());
        CHECK(sv.width() == 64 && sv.height() == 48 && sv.frameCount() == 12 && std::abs(sv.fps() - 25.f) < 1e-3f);
        if (sv.valid()) {
            const u32 tl = sv.frame(0)[2 * 64 + 2]; // SMPTE: first bar ~75% white
            CHECK((tl >> 24) == 0xFF && (tl & 0xFF) > 150 && ((tl >> 8) & 0xFF) > 150 && ((tl >> 16) & 0xFF) > 150);
            const u32 bar2 = sv.frame(0)[2 * 64 + 12]; // second bar yellow: blue low
            CHECK((bar2 & 0xFF) > 150 && ((bar2 >> 16) & 0xFF) < 60);
            CHECK(!sv.frame(12) && sv.source().frames == 12 && sv.source().frame);
            {   // streaming: sequential reads never seek, the cache holds cacheSlots() frames, a jump back seeks
                fuse::ui::media::SystemVideo st;
                CHECK(st.openPipeline("videotestsrc num-buffers=40 ! video/x-raw,width=16,height=8,framerate=30/1", 1000, &err));
                const u32 seeks0 = st.seekCount();
                for (u32 i = 0; i < 40; ++i) CHECK(st.frame(i));
                CHECK(st.frameCount() == 40 && st.cachedFrames() == fuse::ui::media::SystemVideo::cacheSlots());
                const u32 seq = st.seekCount() - seeks0;
                CHECK(seq <= 1); // at most the rewind after counting frames
                CHECK(st.frame(39) && st.seekCount() - seeks0 == seq); // cached
                CHECK(st.frame(3) && st.seekCount() - seeks0 == seq + 1);
                CHECK(st.frame(20) && st.seekCount() - seeks0 == seq + 2); // > 15 ahead: seek, not decode 16 frames
                CHECK(st.decodedFrames() < 60);
                std::printf("  streaming: decoded %u frames, %u seeks\n", st.decodedFrames(), st.seekCount());
            }
            // max-frame cap
            fuse::ui::media::SystemVideo cap;
            CHECK(cap.openPipeline("videotestsrc num-buffers=30 ! video/x-raw,width=16,height=8", 5) && cap.frameCount() == 5);
            // a real container file round-trip through decodebin (Theora/Ogg written by the system encoder when present)
            const std::string tmp = fuse::ui::os::env("TMPDIR").empty() ? "/tmp" : fuse::ui::os::env("TMPDIR");
            const std::string file = tmp + "/fuse_media_gate.ogv";
            {   // a path with spaces and both quote kinds goes to the decoder verbatim
                const std::string odd = tmp + "/fuse media 'gate\" (1).avi";
                {
                    std::FILE* in = std::fopen(FUSE_UI_TEST_DATA "/media_clip.avi", "rb");
                    std::FILE* out = std::fopen(odd.c_str(), "wb");
                    CHECK(in && out);
                    char b[4096];
                    for (std::size_t k; in && out && (k = std::fread(b, 1, sizeof b, in)) > 0;) std::fwrite(b, 1, k, out);
                    if (in) std::fclose(in);
                    if (out) std::fclose(out);
                }
                fuse::ui::media::SystemVideo fv;
                CHECK(fv.openFile(odd, 100, &err) && fv.width() == 32 && fv.height() == 24 && fv.hasAudio());
                if (!fv.valid()) std::printf("  odd path: %s\n", err.c_str());
                CHECK(fv.frame(5) && fv.frame(0));
                std::remove(odd.c_str());
            }
            {   // closing a pipeline that also writes a file runs it to the end, so the file is complete
                {
                    fuse::ui::media::SystemVideo e;
                    CHECK(e.openPipeline("videotestsrc num-buffers=6 ! video/x-raw,width=32,height=24 ! tee name=t t. ! queue ! theoraenc ! oggmux ! filesink location=" + file + " t. ! queue", 100));
                }
                fuse::ui::media::SystemVideo fv;
                CHECK(fv.openFile(file, 100, &err) && fv.frameCount() == 6);
                std::remove(file.c_str());
            }
            vid = w::videoWidget(ui, ui.root(), &sv.source(), "sysvideo");
            ui.setCss(vid, "width", "320px");
            ui.setCss(vid, "height", "280px");
            images.push_back(w::videoImage(ui, vid));
            w::videoSetImageIndex(ui, vid, 0);
            step(ui);
            CHECK(w::videoImage(ui, vid)->width == 64 && w::videoImage(ui, vid)->at(2, 2) == (sv.frame(0)[2 * 64 + 2]));
            w::videoSeek(ui, vid, 7);
            step(ui);
            CHECK(w::videoFrame(ui, vid) == 7 && w::videoImage(ui, vid)->at(2, 2) == sv.frame(7)[2 * 64 + 2]);
            // audio track: audiotestsrc through os::AudioOutput (ALSA "null" device when ALSA exists, else paced)
#ifndef _WIN32
            ::setenv("FUSE_UI_AUDIO_DEVICE", "alsa:null", 1);
#endif
            fuse::ui::media::SystemVideo av;
            CHECK(av.openPipeline("videotestsrc num-buffers=12 ! video/x-raw,width=16,height=8,framerate=25/1", 100, &err,
                                  "audiotestsrc num-buffers=20 samplesperbuffer=480 wave=sine freq=1000 volume=0.5 ! audio/x-raw,rate=48000,channels=2"));
            CHECK(av.hasAudio() && av.audioRate() == 48000 && av.audioChannels() == 2);
            std::atomic<u32> tapped{0};
            std::atomic<f32> peak{0.f};
            av.setAudioTap([&](std::span<const f32> s, u32 ch, u32) {
                f32 p = peak;
                for (f32 x : s) p = std::max(p, std::abs(x));
                peak = p;
                tapped += u32(s.size() / ch);
            });
            CHECK(av.playAudio(0.0, &err));
            for (int i = 0; i < 300 && av.audioPlaying(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            CHECK(!av.audioPlaying() && tapped == 9600 && av.audioFramesPlayed() == 9600 && av.audioClock() < 0.0);
            CHECK(peak > 0.45f && peak < 0.55f);
            std::printf("  audio: backend %s, %u frames, peak %.3f\n", av.audioBackend().c_str(), tapped.load(), double(peak.load()));
            // the widget drives the audio: play starts it at the shown frame, the audio clock paces the frames
            tapped = 0;
            NodeHandle avw = w::videoWidget(ui, ui.root(), &av.source(), "avvideo");
            CHECK(av.source().playback && av.source().clock);
            w::videoSeek(ui, avw, 5);
            w::videoPlay(ui, avw, true);
            CHECK(av.audioPlaying());
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            w::videoPlay(ui, avw, false);
            CHECK(!av.audioPlaying() && tapped > 0 && tapped < 9600); // started 0.2 s in: fewer than all frames
            ui.destroy(avw);
        }
    }
    step(ui);
    if (const std::string dir = fuse::ui::os::env("FUSE_EXTRAS_OUT"); !dir.empty()) {
        ui.update();
        Image out;
        out.resize(int(ui.viewport().w), int(ui.viewport().h), 0xFF000000u);
        CpuRasterizer r;
        r.setGlyphAtlas(ui.atlas().pixels(), ui.atlas().size());
        for (const Image* im : images) r.addImage(im);
        r.render(ui.drawList(), out);
        stbi_write_png((dir + "/media.png").c_str(), out.width, out.height, 4, out.pixels.data(), out.width * 4);
    }
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
#endif // FUSE_UI_EXTRAS_ALL

// Native desktop notifications (desktop::Notifier, TrayIcon::showMessage) against a private session bus with an
// in-process org.freedesktop.Notifications service on a second connection. Skips (passes) without libdbus / dbus-daemon.
void gateNotify() {
#ifdef _WIN32
    std::printf("notify: skipped (session D-Bus gate is POSIX-only)\n");
#else
    using namespace fuse::ui::dbus;
    using namespace fuse::ui::desktop;
    namespace os = fuse::ui::os;
    if (!available() || os::findExecutable("dbus-daemon").empty() || os::findExecutable("bash").empty()) {
        std::printf("notify: skipped (no libdbus / dbus-daemon / bash)\n");
        return;
    }
    const std::string sock = "/tmp/fuse_notify_bus_" + std::to_string(::getpid());
    const std::string addr = "unix:path=" + sock;
    std::string closeFds; // see gateTray: the daemon must not inherit the pipe ends of runProcess
    for (int fd = 3; fd < 32; ++fd) closeFds += " " + std::to_string(fd) + ">&-";
    auto d = os::runProcess({"bash", "-c", "setsid dbus-daemon --session --address=" + addr + " --nofork --nopidfile </dev/null >/dev/null 2>&1" + closeFds + " & echo $!"}, {}, 5000);
    CHECK(d && d->exitCode == 0);
    if (!d || d->exitCode != 0) return;
    const std::string pid = d->out.substr(0, d->out.find('\n'));
    ::setenv("DBUS_SESSION_BUS_ADDRESS", addr.c_str(), 1);
    auto host = Connection::open(addr);
    for (int i = 0; i < 1000 && !host; ++i) { os::sleepSeconds(0.01); host = Connection::open(addr); }
    CHECK(host != nullptr);
    if (!host) return;

    // nobody owns the name yet: honest "unsupported", and the one-shot call says the same
    {
        Notifier none;
        CHECK(!none.supported());
        CHECK(none.show({.title = "nobody listens"}) == NotifyResult::Unsupported);
        CHECK(notifyNative({.title = "nobody listens"}) == NotifyResult::Unsupported);
    }

    // the fake service: records what Notify carries, hands out ids, takes CloseNotification
    struct Seen {
        std::string app, icon, title, body;
        std::vector<std::string> actions;
        std::string hintKey;
        int hintType = 0;
        u32 replaces = 0;
        i32 timeout = 0;
        int notifies = 0;
        u32 closed = 0;
    } seen;
    std::mutex mu;
    u32 nextId = 41;
    CHECK(host->requestName("org.freedesktop.Notifications"));
    host->exportObject("/org/freedesktop/Notifications", [&](Connection& c, const Message& m) {
        std::lock_guard<std::mutex> lock(mu);
        if (m.isCall("org.freedesktop.Notifications", "Notify")) {
            Reader r = m.read();
            seen.app = r.str(); r.next();
            seen.replaces = r.u32v(); r.next();
            seen.icon = r.str(); r.next();
            seen.title = r.str(); r.next();
            seen.body = r.str(); r.next();
            seen.actions.clear();
            for (Reader a = r.recurse(); a.type() == 's'; a.next()) seen.actions.push_back(a.str());
            r.next();
            for (Reader h = r.recurse(); h.type() == 'e'; h.next()) {
                Reader e = h.recurse();
                seen.hintKey = e.str();
                e.next();
                seen.hintType = e.recurse().type();
            }
            r.next();
            seen.timeout = r.i32v();
            ++seen.notifies;
            Message reply = m.reply();
            reply.args().u(seen.replaces ? seen.replaces : ++nextId);
            c.send(reply);
            return true;
        }
        if (m.isCall("org.freedesktop.Notifications", "CloseNotification")) {
            seen.closed = m.read().u32v();
            Message reply = m.reply();
            c.send(reply);
            return true;
        }
        return false;
    });
    std::atomic<bool> stop{false};
    std::thread service([&] {
        while (!stop.load()) host->poll(5);
    });

    Notifier notifier;
    CHECK(notifier.supported());
    u32 actionId = 0, closedId = 0, closedReason = 0;
    std::string actionKey;
    notifier.onAction = [&](std::uint32_t id, std::string_view key) { actionId = id; actionKey = key; };
    notifier.onClosed = [&](std::uint32_t id, std::uint32_t reason) { closedId = id; closedReason = reason; };
    u32 id = 0;
    const Notification n{.appName = "FUSE gate", .title = "Build finished", .body = "3 warnings <b>", .icon = "dialog-information",
                         .urgency = Urgency::Critical, .timeoutMs = 2500, .actions = {{"default", "Open"}, {"log", "Show log"}}};
    CHECK(notifier.show(n, &id) == NotifyResult::Shown);
    CHECK(id == 42);
    {
        std::lock_guard<std::mutex> lock(mu);
        CHECK(seen.notifies == 1 && seen.app == "FUSE gate" && seen.title == "Build finished" && seen.body == "3 warnings <b>");
        CHECK(seen.icon == "dialog-information" && seen.replaces == 0 && seen.timeout == 2500);
        CHECK(seen.actions == std::vector<std::string>({"default", "Open", "log", "Show log"}));
        CHECK(seen.hintKey == "urgency" && seen.hintType == 'y');
    }
    // replacing keeps the id
    Notification again = n;
    again.title = "Build finished (2)";
    again.replacesId = id;
    u32 id2 = 0;
    CHECK(notifier.show(again, &id2) == NotifyResult::Shown && id2 == id);
    { std::lock_guard<std::mutex> lock(mu); CHECK(seen.replaces == 42 && seen.title == "Build finished (2)"); }
    // the service reports an action and the close: the callbacks run from poll()
    auto emit = [&](const char* member, u32 a, const char* text, u32 b) {
        Message sgl = Message::signal("/org/freedesktop/Notifications", "org.freedesktop.Notifications", member);
        if (text) sgl.args().u(a).s(text);
        else sgl.args().u(a).u(b);
        std::lock_guard<std::mutex> lock(mu); // one sender at a time on the host connection
        host->send(sgl);
        host->flush();
    };
    emit("ActionInvoked", 42, "log", 0);
    emit("NotificationClosed", 42, nullptr, 2);
    const double t0 = os::monotonicSeconds();
    while ((actionId == 0 || closedId == 0) && os::monotonicSeconds() - t0 < 5) { notifier.poll(); os::sleepSeconds(0.005); }
    CHECK(actionId == 42 && actionKey == "log");
    CHECK(closedId == 42 && closedReason == 2);
    CHECK(notifier.close(id));
    { std::lock_guard<std::mutex> lock(mu); CHECK(seen.closed == 42); }
    CHECK(!notifier.close(0));
    // the one-shot forms take the native route (no helper process is needed)
    CHECK(notifyNative({.appName = "one", .title = "shot"}) == NotifyResult::Shown);
    CHECK(notificationsSupported());
    CHECK(notify({.appName = "auto", .title = "native first"}));
    { std::lock_guard<std::mutex> lock(mu); CHECK(seen.app == "auto" && seen.title == "native first" && seen.actions.empty()); }

    // a tray icon's message: its title is the sender, a click on the notification reaches onMessageClicked
    {
        TrayIcon tray;
        tray.setTitle("FUSE Tray");
        int clicked = 0;
        tray.onMessageClicked = [&] { ++clicked; };
        CHECK(tray.showMessage("Export done", "scene.fbx", 4000));
        u32 trayNote = 0;
        {
            std::lock_guard<std::mutex> lock(mu);
            CHECK(seen.app == "FUSE Tray" && seen.title == "Export done" && seen.body == "scene.fbx" && seen.timeout == 4000);
            CHECK(seen.actions == std::vector<std::string>({"default", "Open"}));
            trayNote = nextId;
        }
        emit("ActionInvoked", trayNote, "default", 0);
        const double t1 = os::monotonicSeconds();
        while (clicked == 0 && os::monotonicSeconds() - t1 < 5) { tray.poll(); os::sleepSeconds(0.005); }
        CHECK(clicked == 1);
    }
    stop = true;
    service.join();
    host.reset();
    os::runProcess({"kill", pid});
#endif
}
} // namespace

int main(int argc, char** argv) {
    const std::map<std::string, std::function<void()>> gates{
        {"undo", gateUndo}, {"desktop", gateDesktop}, {"pdf", gatePdf}, {"tray", gateTray}, {"webview", gateWebView}, {"notify", gateNotify},
        {"declarative", gateDeclarative}, {"dataviz3d", gateDataViz3d}, {"dataviz3d_bsp", gateDataViz3dBsp},
#if FUSE_UI_EXTRAS_ALL
        {"i18n", gateI18n}, {"widget_pdf", gateWidgetPdf}, {"svg", gateSvg}, {"charts", gateCharts}, {"media", gateMedia},
#endif
    };
    bool ran = false;
    std::string only;
    for (int i = 1; i < argc; ++i) if (std::strncmp(argv[i], "--gate=", 7) == 0) only = argv[i] + 7;
    for (auto& [n, f] : gates) if (only.empty() || only == n) { ran = true; f(); std::printf("gate %s: %s\n", n.c_str(), g_fail ? "FAIL" : "ok"); }
    if (!ran) { std::fprintf(stderr, "no such gate in this build: %s\n", only.c_str()); return 2; }
    return g_fail ? 1 : 0;
}
