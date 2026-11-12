// fuse_ui_a11y_linux gate (plan §4.14, D7): a real fuse_app X11 window with WindowAccessibility (AccessKit's AT-SPI
// adapter) is read and driven by an AT-SPI client (libatspi, loaded at runtime) on a helper thread of the same process
// while the main thread runs the app loop. Runs on a private session bus (dbus-run-session) under Xvfb; the gate
// starts the accessibility bus (at-spi-bus-launcher, or D-Bus activation of org.a11y.Bus) and turns AT-SPI on
// (org.a11y.Status.IsEnabled) the way a screen reader does.
//   * the registry lists the app (by pid); its window holds the widgets with their AT-SPI roles and names (push
//     button, check box, slider, entry, list, tree, menu bar, menu item)
//   * states: enabled / sensitive / focusable / showing; a disabled check box is not enabled, a disabled button is
//     not focusable and has no action; the check box' checked state follows toggling
//   * Component extents of the button (screen coordinates) are its UI rect at the window's screen origin
//   * Action "click" fires the button's Clicked signal and toggles the check box; Value sets the slider; Component
//     grab_focus moves UI focus to the entry, which then reports the focused state and a focus event arrives
//   * Text reads the entry's text, and a text change in the app reaches the AT (AccessKit's Unix adapter has no
//     EditableText interface, so an AT cannot write text; that half is covered on Windows by fuse_ui_a11y_windows)
//   * "click" on the File menu title opens the menu (its items appear) and closes it again
// Every wait is bounded; on a hang the gate fails and exits. Exit 77 without AccessKit, a display, a session bus or
// libatspi.
#include "fuse/app/accessibility.hpp"
#include "fuse/ui/widgets/widgets.hpp"

#include "dbus_internal.hpp"

#include <dlfcn.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

extern char** environ;

using namespace fuse::ui;
namespace app = fuse::app;
namespace w = fuse::ui::widgets;
namespace dbus = fuse::app::dbus;

namespace {
std::atomic<int> g_fail{0};
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

// ---- libatspi / GLib entry points (dlopen; no headers needed) ---------------------------------------------------
using Obj = void; // AtspiAccessible / AtspiComponent / ... (GObjects)
struct GError { std::uint32_t domain; int code; char* message; };
struct GEnumValue { int value; const char* name; const char* nick; };
struct AtspiRect { int x, y, width, height; };
struct AtspiEvent { char* type; Obj* source; int detail1, detail2; /* GValue any_data; sender */ };
using GType = unsigned long;

struct Atspi {
    void* so = nullptr;
    int (*init)() = nullptr;
    Obj* (*getDesktop)(int) = nullptr;
    int (*childCount)(Obj*, GError**) = nullptr;
    Obj* (*childAt)(Obj*, int, GError**) = nullptr;
    char* (*name)(Obj*, GError**) = nullptr;
    int (*role)(Obj*, GError**) = nullptr;
    char* (*roleName)(int) = nullptr;
    unsigned (*pid)(Obj*, GError**) = nullptr;
    void (*setCacheMask)(Obj*, int) = nullptr;
    void (*clearCache)(Obj*) = nullptr;
    Obj* (*stateSet)(Obj*) = nullptr;
    int (*stateContains)(Obj*, int) = nullptr;
    Obj* (*componentIface)(Obj*) = nullptr;
    AtspiRect* (*extents)(Obj*, int, GError**) = nullptr;
    int (*grabFocus)(Obj*, GError**) = nullptr;
    Obj* (*actionIface)(Obj*) = nullptr;
    int (*nActions)(Obj*, GError**) = nullptr;
    char* (*actionName)(Obj*, int, GError**) = nullptr;
    int (*doAction)(Obj*, int, GError**) = nullptr;
    Obj* (*valueIface)(Obj*) = nullptr;
    double (*valueMax)(Obj*, GError**) = nullptr;
    double (*valueGet)(Obj*, GError**) = nullptr;
    int (*valueSet)(Obj*, double, GError**) = nullptr;
    Obj* (*textIface)(Obj*) = nullptr;
    char* (*textGet)(Obj*, int, int, GError**) = nullptr;
    Obj* (*listenerNew)(void (*)(AtspiEvent*, void*), void*, void (*)(void*)) = nullptr;
    int (*listenerRegister)(Obj*, const char*, GError**) = nullptr;
    GType (*stateType)() = nullptr;
    GType (*eventType)() = nullptr;
    // GLib / GObject (dependencies of libatspi, found through its handle)
    void (*gFree)(void*) = nullptr;
    void (*gErrorFree)(GError*) = nullptr;
    void (*unref)(void*) = nullptr;
    int (*iterate)(void*, int) = nullptr;
    void* (*classRef)(GType) = nullptr;
    GEnumValue* (*enumByNick)(void*, const char*) = nullptr;
    void (*boxedFree)(GType, void*) = nullptr;

    bool load() {
        so = dlopen("libatspi.so.0", RTLD_NOW | RTLD_LOCAL);
        if (!so) return false;
        bool all = true;
        auto get = [&](auto& fn, const char* n) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(so, n));
            all = all && fn;
        };
        get(init, "atspi_init"); get(getDesktop, "atspi_get_desktop"); get(childCount, "atspi_accessible_get_child_count");
        get(childAt, "atspi_accessible_get_child_at_index"); get(name, "atspi_accessible_get_name");
        get(role, "atspi_accessible_get_role"); get(roleName, "atspi_role_get_name");
        get(pid, "atspi_accessible_get_process_id"); get(setCacheMask, "atspi_accessible_set_cache_mask");
        get(clearCache, "atspi_accessible_clear_cache"); get(stateSet, "atspi_accessible_get_state_set");
        get(stateContains, "atspi_state_set_contains"); get(componentIface, "atspi_accessible_get_component_iface");
        get(extents, "atspi_component_get_extents"); get(grabFocus, "atspi_component_grab_focus");
        get(actionIface, "atspi_accessible_get_action_iface"); get(nActions, "atspi_action_get_n_actions");
        get(actionName, "atspi_action_get_action_name"); get(doAction, "atspi_action_do_action");
        get(valueIface, "atspi_accessible_get_value_iface"); get(valueMax, "atspi_value_get_maximum_value");
        get(valueGet, "atspi_value_get_current_value"); get(valueSet, "atspi_value_set_current_value");
        get(textIface, "atspi_accessible_get_text_iface"); get(textGet, "atspi_text_get_text");
        get(listenerNew, "atspi_event_listener_new"); get(listenerRegister, "atspi_event_listener_register");
        get(stateType, "atspi_state_type_get_type"); get(eventType, "atspi_event_get_type");
        get(gFree, "g_free"); get(gErrorFree, "g_error_free"); get(unref, "g_object_unref");
        get(iterate, "g_main_context_iteration"); get(classRef, "g_type_class_ref");
        get(enumByNick, "g_enum_get_value_by_nick"); get(boxedFree, "g_boxed_free");
        return all;
    }
    std::string take(char* s) { // gchar* -> std::string (frees)
        std::string r = s ? s : "";
        if (s) gFree(s);
        return r;
    }
    void clear(GError*& e) {
        if (e) gErrorFree(e);
        e = nullptr;
    }
    int state(const char* nick) { // AtspiStateType by nick ("checked", "focused", ...): no constants compiled in
        const GEnumValue* v = enumByNick(classRef(stateType()), nick);
        return v ? v->value : -1;
    }
    bool has(Obj* acc, const char* nick) {
        Obj* set = stateSet(acc);
        const bool r = set && stateContains(set, state(nick));
        if (set) unref(set);
        return r;
    }
    void pump() { // dispatch D-Bus events (cache updates, listeners) queued for this thread's default context
        for (int i = 0; i < 50 && iterate(nullptr, 0); ++i) {}
    }
};

bool waitFor(Atspi& at, const std::function<bool()>& pred, double seconds = 5.0) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < end) {
        at.pump();
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    at.pump();
    return pred();
}

/// UI-thread state the client checks (written by signal slots on the UI thread), and requests to the UI thread.
struct Observed {
    std::atomic<int> clicked{0};
    std::atomic<bool> toggled{false};
    std::atomic<f32> slider{0.f};
    std::atomic<u64> focused{0};
    std::atomic<bool> wantText{false}; ///< client -> UI thread: change the entry's text
};

struct Expect {
    Rect saveScreen; ///< the Save button in screen px
};

struct Found { std::string name, role; Obj* acc = nullptr; };

// ---- the AT-SPI client (helper thread) --------------------------------------------------------------------------
std::mutex g_evMu;
std::vector<std::string> g_focusEvents; ///< names of sources of object:state-changed:focused (detail1 = 1)
Atspi* g_at = nullptr;

void onEvent(AtspiEvent* e, void*) {
    if (e && e->type && e->detail1 == 1 && std::strcmp(e->type, "object:state-changed:focused") == 0 && e->source) {
        GError* err = nullptr;
        std::string n = g_at->take(g_at->name(e->source, &err));
        g_at->clear(err);
        std::lock_guard<std::mutex> lock(g_evMu);
        g_focusEvents.push_back(n);
    }
    if (e) g_at->boxedFree(g_at->eventType(), e); // the callback owns the event
}
bool sawFocus(const std::string& n) {
    std::lock_guard<std::mutex> lock(g_evMu);
    for (const auto& s : g_focusEvents)
        if (s == n) return true;
    return false;
}

void client(Atspi& at, const Expect& ex, Observed& ob, std::atomic<bool>& done) {
    g_at = &at;
    GError* err = nullptr;
    at.init();
    Obj* listener = at.listenerNew(&onEvent, nullptr, nullptr);
    CHECK(listener && at.listenerRegister(listener, "object:state-changed:focused", &err));
    at.clear(err);

    // the registry lists the app once the adapter registered (it does when IsEnabled turns true)
    Obj* desktop = at.getDesktop(0);
    Obj* appAcc = nullptr;
    waitFor(at, [&] {
        at.clearCache(desktop);
        const int n = at.childCount(desktop, &err);
        at.clear(err);
        for (int i = 0; i < n && !appAcc; ++i) {
            Obj* c = at.childAt(desktop, i, &err);
            at.clear(err);
            if (!c) continue;
            if (int(at.pid(c, &err)) == int(getpid())) appAcc = c;
            else at.unref(c);
            at.clear(err);
        }
        return appAcc != nullptr;
    }, 15.0);
    CHECK(appAcc);
    if (!appAcc) { done = true; return; }
    at.setCacheMask(appAcc, 0); // ATSPI_CACHE_NONE: every query asks the app
    std::printf("  AT-SPI app \"%s\" (pid %d)\n", at.take(at.name(appAcc, &err)).c_str(), int(getpid()));
    at.clear(err);

    std::vector<Found> seen;
    auto release = [&] {
        for (Found& f : seen) at.unref(f.acc);
        seen.clear();
    };
    std::function<void(Obj*, int)> walk = [&](Obj* acc, int depth) {
        const int n = at.childCount(acc, &err);
        at.clear(err);
        for (int i = 0; i < n && depth < 32; ++i) {
            Obj* c = at.childAt(acc, i, &err);
            at.clear(err);
            if (!c) continue;
            Found f;
            f.acc = c;
            f.name = at.take(at.name(c, &err));
            at.clear(err);
            const int r = at.role(c, &err);
            at.clear(err);
            f.role = at.take(at.roleName(r));
            seen.push_back(f);
            walk(c, depth + 1);
        }
    };
    auto scan = [&] { release(); walk(appAcc, 0); };
    auto roleIs = [](const std::string& r, std::initializer_list<const char*> any) {
        for (const char* a : any)
            if (r == a) return true;
        return false;
    };
    auto find = [&](const char* name, std::initializer_list<const char*> roles) -> Obj* {
        for (const Found& f : seen)
            if (f.name == name && roleIs(f.role, roles)) return f.acc;
        return nullptr;
    };
    auto any = [&](std::initializer_list<const char*> roles) {
        for (const Found& f : seen)
            if (roleIs(f.role, roles)) return true;
        return false;
    };
    const std::initializer_list<const char*> kButton{"push button", "button"};
    const std::initializer_list<const char*> kEntry{"entry", "text"};
    const std::initializer_list<const char*> kList{"list", "list box"};

    CHECK(waitFor(at, [&] { scan(); return find("Save", kButton) != nullptr; }));
    std::printf("  AT-SPI tree: %zu descendants\n", seen.size());
    for (const Found& f : seen) std::printf("    %s \"%s\"\n", f.role.c_str(), f.name.c_str());
    CHECK(find("Autosave", {"check box"}));
    CHECK(find("Volume", {"slider"}));
    CHECK(find("Search", kEntry));
    CHECK(find("Fruits", kList));
    CHECK(find("Scene", {"tree"}));
    CHECK(find("File", {"menu item"}));
    CHECK(any({"menu bar"}));
    CHECK(any({"frame", "window"}));

    // states
    Obj* save = find("Save", kButton);
    Obj* off = find("Disabled", kButton);
    if (save) {
        CHECK(at.has(save, "enabled") && at.has(save, "sensitive") && at.has(save, "focusable"));
        CHECK(at.has(save, "visible") && at.has(save, "showing"));
    }
    // disabled: AccessKit reports enabled / sensitive off only for roles with a read-only notion (check box, entry,
    // ...); a disabled button shows it by having no focusable state and no action
    CHECK(off && !at.has(off, "focusable"));
    if (Obj* a = off ? at.actionIface(off) : nullptr) {
        CHECK(at.nActions(a, &err) == 0);
        at.clear(err);
        at.unref(a);
    }
    Obj* locked = find("Locked", {"check box"});
    CHECK(locked && !at.has(locked, "enabled") && !at.has(locked, "sensitive") && !at.has(locked, "focusable"));
    if (Obj* c = find("Autosave", {"check box"})) CHECK(at.has(c, "enabled") && at.has(c, "sensitive") && at.has(c, "checkable"));

    // bounds: screen coordinates of the UI rect
    if (Obj* comp = save ? at.componentIface(save) : nullptr) {
        AtspiRect* r = at.extents(comp, 0 /*ATSPI_COORD_TYPE_SCREEN*/, &err);
        at.clear(err);
        CHECK(r);
        if (r) {
            std::printf("  Save extents %d,%d %dx%d (expected %.0f,%.0f %.0fx%.0f)\n", r->x, r->y, r->width, r->height,
                        ex.saveScreen.x, ex.saveScreen.y, ex.saveScreen.w, ex.saveScreen.h);
            CHECK(std::abs(r->x - ex.saveScreen.x) <= 1 && std::abs(r->y - ex.saveScreen.y) <= 1 &&
                  std::abs(r->width - ex.saveScreen.w) <= 1 && std::abs(r->height - ex.saveScreen.h) <= 1);
            at.gFree(r);
        }
        at.unref(comp);
    } else {
        CHECK(!"Save has a Component interface");
    }

    // Action "click": button, check box
    auto click = [&](Obj* acc) {
        Obj* act = acc ? at.actionIface(acc) : nullptr;
        bool ok = false;
        if (act) {
            const int n = at.nActions(act, &err);
            at.clear(err);
            for (int i = 0; i < n && !ok; ++i)
                if (at.take(at.actionName(act, i, &err)) == "click") {
                    at.clear(err);
                    ok = at.doAction(act, i, &err) != 0;
                }
            at.clear(err);
            at.unref(act);
        }
        return ok;
    };
    CHECK(click(save));
    CHECK(waitFor(at, [&] { return ob.clicked.load() == 1; }));
    Obj* cb = find("Autosave", {"check box"});
    if (cb) {
        CHECK(!at.has(cb, "checked"));
        CHECK(click(cb));
        CHECK(waitFor(at, [&] { return ob.toggled.load(); }));
        CHECK(waitFor(at, [&] { return at.has(cb, "checked"); }));
    }
    // Value (slider)
    Obj* sl = find("Volume", {"slider"});
    if (Obj* val = sl ? at.valueIface(sl) : nullptr) {
        CHECK(at.valueMax(val, &err) == 100.0);
        at.clear(err);
        CHECK(std::abs(at.valueGet(val, &err) - 40.0) < 1e-6);
        at.clear(err);
        CHECK(at.valueSet(val, 75.0, &err));
        at.clear(err);
        CHECK(waitFor(at, [&] { return ob.slider.load() == 75.f; }));
        CHECK(waitFor(at, [&] { const double v = at.valueGet(val, &err); at.clear(err); return v == 75.0; }));
        at.unref(val);
    } else {
        CHECK(!"Volume has a Value interface");
    }
    // Text (entry): read, then a change made by the app shows up
    Obj* edit = find("Search", kEntry);
    if (Obj* tx = edit ? at.textIface(edit) : nullptr) {
        CHECK(at.take(at.textGet(tx, 0, -1, &err)) == "hello");
        at.clear(err);
        ob.wantText = true;
        CHECK(waitFor(at, [&] { const std::string s = at.take(at.textGet(tx, 0, -1, &err)); at.clear(err); return s == "set by the app"; }));
        at.unref(tx);
    } else {
        CHECK(!"Search has a Text interface");
    }
    // Focus: grab_focus -> UI focus -> focused state + focus event
    if (Obj* comp = edit ? at.componentIface(edit) : nullptr) {
        CHECK(!at.has(edit, "focused") && at.has(edit, "focusable"));
        CHECK(at.grabFocus(comp, &err));
        at.clear(err);
        CHECK(waitFor(at, [&] { return ob.focused.load() == 1; }));
        CHECK(waitFor(at, [&] { return at.has(edit, "focused"); }));
        CHECK(waitFor(at, [&] { return sawFocus("Search"); }));
        at.unref(comp);
    }
    // menu: "click" on the File title opens it (items appear), again closes it
    Obj* file = find("File", {"menu item"});
    if (file) {
        CHECK(click(file));
        CHECK(waitFor(at, [&] { scan(); return find("Open", {"menu item"}) != nullptr; }));
        CHECK(any({"menu"}));
        file = find("File", {"menu item"});
        CHECK(file);
        CHECK(click(file));
        CHECK(waitFor(at, [&] { scan(); return find("Open", {"menu item"}) == nullptr; }));
    }
    {
        std::lock_guard<std::mutex> lock(g_evMu);
        std::printf("  focus events: %zu\n", g_focusEvents.size());
    }
    release();
    at.unref(appAcc);
    done = true;
}

// ---- the accessibility bus ----------------------------------------------------------------------------------------
/// Makes sure org.a11y.Bus is up (starts at-spi-bus-launcher, else relies on D-Bus activation) and sets
/// org.a11y.Status.IsEnabled = true. Returns the launcher's pid (0: not started by us), -1 on failure.
pid_t startA11yBus(std::string& why) {
    dbus::Lib& d = dbus::lib();
    if (!d.load()) { why = "libdbus-1.so.3 not found"; return -1; }
    dbus::Error e{};
    d.errorInit(&e);
    dbus::Connection* c = d.busGetPrivate(dbus::kBusSession, &e);
    if (!c) { why = std::string("no session bus: ") + (e.message ? e.message : "?"); d.errorFree(&e); return -1; }
    d.exitOnDisconnect(c, 0);
    pid_t pid = 0;
#ifdef FUSE_ATSPI_BUS_LAUNCHER
    if (!d.busNameHasOwner(c, "org.a11y.Bus", nullptr) && access(FUSE_ATSPI_BUS_LAUNCHER, X_OK) == 0) {
        char arg0[] = FUSE_ATSPI_BUS_LAUNCHER;
        char arg1[] = "--launch-immediately";
        char* argv[] = {arg0, arg1, nullptr};
        if (posix_spawn(&pid, FUSE_ATSPI_BUS_LAUNCHER, nullptr, nullptr, argv, environ) != 0) pid = 0;
        for (int i = 0; i < 500 && pid && !d.busNameHasOwner(c, "org.a11y.Bus", nullptr); ++i) usleep(10000);
    }
#endif
    // Properties.Set(org.a11y.Status, IsEnabled, <true>) (activates org.a11y.Bus when nobody owns it yet)
    dbus::Message* m = d.newCall("org.a11y.Bus", "/org/a11y/bus", "org.freedesktop.DBus.Properties", "Set");
    dbus::Writer(d, m).str("org.a11y.Status").str("IsEnabled").variant("b", [](dbus::Writer& v) { v.boolean(true); });
    dbus::Message* r = d.sendBlocking(c, m, 10000, &e);
    d.unref(m);
    if (!r) {
        why = std::string("org.a11y.Bus: ") + (e.message ? e.message : "no reply");
        d.errorFree(&e);
        if (pid > 0) { kill(pid, SIGTERM); waitpid(pid, nullptr, 0); }
        pid = -1;
    } else {
        d.unref(r);
    }
    d.connClose(c);
    d.connUnref(c);
    return pid;
}
} // namespace

int main(int argc, char** argv) {
    app::Application a{{.name = "gate_a11y_linux", .argc = argc, .argv = argv, .loadSettings = false}};
    if (!fuse::ui::a11y::accessKitAvailable()) {
        std::printf("fuse_ui_a11y_linux: skipped (built without AccessKit: set FUSE_UI_ACCESSKIT_BUILD=ON or FUSE_UI_ACCESSKIT_DIR)\n");
        return 77;
    }
    if (!std::getenv("DBUS_SESSION_BUS_ADDRESS")) {
        std::printf("fuse_ui_a11y_linux: skipped (no session bus; run under dbus-run-session)\n");
        return 77;
    }
    Atspi at;
    if (!at.load()) {
        std::printf("fuse_ui_a11y_linux: skipped (libatspi.so.0 not found: install libatspi2.0-0 / at-spi2-core)\n");
        return 77;
    }
    std::string why;
    const pid_t launcher = startA11yBus(why);
    if (launcher < 0) {
        std::printf("fuse_ui_a11y_linux: skipped (accessibility bus unavailable: %s)\n", why.c_str());
        return 77;
    }
    struct Reap { pid_t p; ~Reap() { if (p > 0) { kill(p, SIGTERM); waitpid(p, nullptr, 0); } } } reap{launcher};

    app::NativePlatform native(a, {.vulkan = false, .dpiAware = false}); // software presenter: no GPU needed
    if (!native.available()) {
        std::printf("fuse_ui_a11y_linux: skipped (no X display)\n");
        return 77;
    }
    auto& win = a.createWindow({.title = "fuse_ui_a11y_linux", .width = 480, .height = 400});
    UiContext& ui = win.ui();
    const NodeHandle bar = w::menuBar(ui, ui.root());
    w::menuBarMenu(ui, bar, "File", "Open\nSave As\nQuit");
    const NodeHandle panel = w::panel(ui, ui.root());
    const NodeHandle save = w::button(ui, panel, "Save", "save");
    const NodeHandle off = w::button(ui, panel, "Disabled", "off");
    ui.setDisabled(off, true);
    const NodeHandle cb = w::checkBox(ui, panel, "Autosave", false, "autosave");
    ui.setDisabled(w::checkBox(ui, panel, "Locked", true, "locked"), true);
    const NodeHandle sl = w::slider(ui, panel, 0.f, 100.f, 40.f, 0.f, "volume");
    ui.setA11y(sl, A11yRole::Slider, "Volume");
    const NodeHandle le = w::lineEdit(ui, panel, "hello", "search");
    ui.setA11y(le, A11yRole::TextField, "Search");
    const NodeHandle list = w::listView(ui, panel, "Apple\nBanana\nCherry", 0, "fruits");
    ui.setA11y(list, A11yRole::List, "Fruits");
    const NodeHandle tree = w::treeView(ui, panel, "World\n\tCamera\n\tLight", "scene");
    ui.setA11y(tree, A11yRole::List, "Scene");

    Observed ob;
    ui.connect(save, SignalKind::Clicked, [&](UiContext&, NodeHandle, const SignalArgs&) { ++ob.clicked; });
    ui.connect(cb, SignalKind::Toggled, [&](UiContext&, NodeHandle, const SignalArgs& s) { ob.toggled = s.flag; });
    ui.connect(sl, SignalKind::ValueChanged, [&](UiContext&, NodeHandle, const SignalArgs& s) { ob.slider = s.value; });

    native.show(win);
    if (!native.nativeWindow(win)) {
        std::printf("fuse_ui_a11y_linux: skipped (no native window: %.*s)\n", int(native.lastError().size()), native.lastError().data());
        return 77;
    }
    app::WindowAccessibility acc(native, win);
    CHECK(acc.active());
    for (int i = 0; i < 400 && win.framesPresented() < 1; ++i) a.step(0.01f);
    for (int i = 0; i < 10; ++i) a.step(0.005f);
    acc.setWindowFocused(true); // no window manager gives the window focus under Xvfb: focus events need it

    Expect ex;
    {
        const Vec2 o = native.screenOrigin(win);
        const f32 k = f32(win.pixelSize().w) / ui.viewport().w;
        const Rect r = ui.rect(save);
        ex.saveScreen = {std::round(o.x + r.x * k), std::round(o.y + r.y * k), std::round(r.w * k), std::round(r.h * k)};
    }

    std::atomic<bool> done{false};
    std::thread th(client, std::ref(at), std::cref(ex), std::ref(ob), std::ref(done));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
    while (!done && std::chrono::steady_clock::now() < deadline) {
        a.step(0.01f);
        ob.focused = ui.focused() == le ? 1 : 0;
        if (ob.wantText.exchange(false)) {
            ui.setText(le, "set by the app");
            win.requestRedraw();
        }
    }
    if (!done) {
        std::fprintf(stderr, "fuse_ui_a11y_linux: FAILED (AT-SPI client did not finish within 90 s)\n");
        std::fflush(stderr);
        std::_Exit(1); // the client thread may be stuck inside a D-Bus call
    }
    th.join();
    for (int i = 0; i < 5; ++i) a.step(0.005f);
    CHECK(ui.checked(cb));
    CHECK(ui.value(sl) == 75.f);
    if (g_fail) {
        std::fprintf(stderr, "fuse_ui_a11y_linux: FAILED (%d checks)\n", g_fail.load());
        return 1;
    }
    std::printf("fuse_ui_a11y_linux: OK\n");
    return 0;
}
