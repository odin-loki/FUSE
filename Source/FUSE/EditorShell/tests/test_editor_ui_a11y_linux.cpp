// fuse_editor_ui_a11y_linux: the FUSE editor shell on a real X11 window with WindowAccessibility (AccessKit's AT-SPI
// adapter), read and driven by an AT-SPI client (libatspi, loaded at runtime) on a helper thread while the main thread
// runs the editor. Linux twin of fuse_editor_ui_a11y (UI Automation); same private session bus + Xvfb setup as
// fuse_ui_a11y_linux (the gate starts the accessibility bus and turns org.a11y.Status.IsEnabled on):
//   * the menu bar and its File / Edit / Entity / Play / View items, the toolbar buttons (Undo / Redo / Save) and the
//     visible panel pages (Hierarchy, Inspector, Console) are in the AT-SPI tree with their roles and names
//   * the selected entity's inspector fields are named "<component> <field>" (Transform Position) and expose their
//     text through the Text interface
//   * Component.grab_focus moves UI focus to the Position field; the edit is then typed on the keyboard (AccessKit's
//     Unix adapter has no EditableText, so an AT cannot write the text itself) and the new value reaches the AT
//   * Action "click" on the Undo toolbar button reverts the edit; "click" on the Entity menu opens it (its items appear)
// Every wait is bounded. Exit 77 without AccessKit, a display, a session bus or libatspi.
#include "fuse/app/accessibility.hpp"
#include "fuse/app/native.hpp"
#include "fuse/editor/fui/editor_shell.hpp"
#include "scripted_services.hpp"

#include <fuse/ecs/components/transform.hpp>
#include <fuse/editor/editor_host.hpp>

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
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

extern char** environ;

using namespace fuse::editor;
namespace app = fuse::app;
namespace dbus = fuse::app::dbus;
using fuse::ui::NodeHandle;

namespace {
std::atomic<int> g_fail{0};
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

// ---- libatspi / GLib entry points (dlopen; no headers needed) ---------------------------------------------------
using Obj = void;
struct GError { std::uint32_t domain; int code; char* message; };
struct GEnumValue { int value; const char* name; const char* nick; };
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
    int (*grabFocus)(Obj*, GError**) = nullptr;
    Obj* (*actionIface)(Obj*) = nullptr;
    int (*nActions)(Obj*, GError**) = nullptr;
    char* (*actionName)(Obj*, int, GError**) = nullptr;
    int (*doAction)(Obj*, int, GError**) = nullptr;
    Obj* (*textIface)(Obj*) = nullptr;
    char* (*textGet)(Obj*, int, int, GError**) = nullptr;
    GType (*stateType)() = nullptr;
    void (*gFree)(void*) = nullptr;
    void (*gErrorFree)(GError*) = nullptr;
    void (*unref)(void*) = nullptr;
    int (*iterate)(void*, int) = nullptr;
    void* (*classRef)(GType) = nullptr;
    GEnumValue* (*enumByNick)(void*, const char*) = nullptr;

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
        get(grabFocus, "atspi_component_grab_focus");
        get(actionIface, "atspi_accessible_get_action_iface"); get(nActions, "atspi_action_get_n_actions");
        get(actionName, "atspi_action_get_action_name"); get(doAction, "atspi_action_do_action");
        get(textIface, "atspi_accessible_get_text_iface"); get(textGet, "atspi_text_get_text");
        get(stateType, "atspi_state_type_get_type");
        get(gFree, "g_free"); get(gErrorFree, "g_error_free"); get(unref, "g_object_unref");
        get(iterate, "g_main_context_iteration"); get(classRef, "g_type_class_ref");
        get(enumByNick, "g_enum_get_value_by_nick");
        return all;
    }
    std::string take(char* s) {
        std::string r = s ? s : "";
        if (s) gFree(s);
        return r;
    }
    void clear(GError*& e) {
        if (e) gErrorFree(e);
        e = nullptr;
    }
    bool has(Obj* acc, const char* nick) {
        const GEnumValue* v = enumByNick(classRef(stateType()), nick);
        Obj* set = stateSet(acc);
        const bool r = set && v && stateContains(set, v->value);
        if (set) unref(set);
        return r;
    }
    void pump() {
        for (int i = 0; i < 50 && iterate(nullptr, 0); ++i) {}
    }
};

bool waitFor(Atspi& at, const std::function<bool()>& pred, double seconds = 10.0) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < end) {
        at.pump();
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    at.pump();
    return pred();
}

/// UI thread -> client (state) and client -> UI thread (requests).
struct Observed {
    std::atomic<float> posX{0.f};
    std::atomic<bool> positionFocused{false};
    std::atomic<bool> wantType{false}; ///< client: type "3,0,0" + Enter into the focused Position field
};

struct Found { std::string name, role; Obj* acc = nullptr; };

void client(Atspi& at, Observed& ob, std::atomic<bool>& done) {
    GError* err = nullptr;
    at.init();
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
    }, 20.0);
    CHECK(appAcc);
    if (!appAcc) { done = true; return; }
    at.setCacheMask(appAcc, 0); // ATSPI_CACHE_NONE: every query asks the app

    std::vector<Found> seen;
    auto release = [&] {
        for (Found& f : seen) at.unref(f.acc);
        seen.clear();
    };
    std::function<void(Obj*, int)> walk = [&](Obj* acc, int depth) {
        const int n = at.childCount(acc, &err);
        at.clear(err);
        for (int i = 0; i < n && depth < 48; ++i) {
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
    const std::initializer_list<const char*> kButton{"push button", "button", "toggle button"};
    const std::initializer_list<const char*> kEntry{"entry", "text"};
    const std::initializer_list<const char*> kGroup{"panel", "grouping", "section", "filler"};

    CHECK(waitFor(at, [&] { scan(); return find("File", {"menu item"}) != nullptr && find("Transform Position", kEntry); }, 20.0));
    std::printf("  AT-SPI tree: %zu descendants\n", seen.size());
    for (const Found& f : seen)
        if (!f.name.empty()) std::printf("    %s \"%s\"\n", f.role.c_str(), f.name.c_str());
    CHECK(any({"menu bar"}));
    for (const char* m : {"File", "Edit", "Entity", "Play", "View"}) CHECK(find(m, {"menu item"}));
    for (const char* p : {"Hierarchy", "Inspector", "Console"}) CHECK(find(p, kGroup));
    for (const char* b : {"Undo", "Redo", "Save"}) CHECK(find(b, kButton));

    auto text = [&](Obj* acc) {
        std::string s;
        if (Obj* tx = acc ? at.textIface(acc) : nullptr) {
            s = at.take(at.textGet(tx, 0, -1, &err));
            at.clear(err);
            at.unref(tx);
        }
        return s;
    };
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

    // the inspector Position field: named after its component, text readable, focus by grab_focus, edit by keyboard
    Obj* pos = find("Transform Position", kEntry);
    CHECK(pos);
    if (pos) {
        const std::string before = text(pos);
        std::printf("  Transform Position text \"%s\"\n", before.c_str());
        CHECK(before.rfind("0", 0) == 0);
        CHECK(at.has(pos, "focusable"));
        if (Obj* comp = at.componentIface(pos)) {
            CHECK(at.grabFocus(comp, &err));
            at.clear(err);
            at.unref(comp);
        }
        CHECK(waitFor(at, [&] { return ob.positionFocused.load(); }));
        CHECK(waitFor(at, [&] { return at.has(pos, "focused"); }));
        ob.wantType = true;
        CHECK(waitFor(at, [&] { return std::fabs(ob.posX.load() - 3.f) < 1e-4f; }));
    }
    // Undo through the toolbar button (Action "click"); the inspector shows the old value again
    CHECK(click(find("Undo", kButton)));
    CHECK(waitFor(at, [&] { return std::fabs(ob.posX.load() - 3.f) > 1e-3f; }));
    CHECK(waitFor(at, [&] {
        scan();
        Obj* p = find("Transform Position", kEntry);
        return p && text(p).rfind("0", 0) == 0;
    }));
    // the Entity menu opens and lists its actions
    Obj* menu = find("Entity", {"menu item"});
    CHECK(menu && click(menu));
    auto entityItems = [&] {
        scan();
        for (const Found& f : seen)
            if (f.role == "menu item" && f.name.rfind("Create SDF Sphere", 0) == 0) return true;
        return false;
    };
    CHECK(waitFor(at, entityItems));
    if ((menu = find("Entity", {"menu item"}))) CHECK(click(menu));
    CHECK(waitFor(at, [&] { return !entityItems(); }));
    release();
    at.unref(appAcc);
    done = true;
}

// ---- the accessibility bus (as fuse_ui_a11y_linux) -----------------------------------------------------------------
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

/// The inspector's Position editor (the row labelled "Position"; syncInspector_ names it "Transform Position").
NodeHandle positionEditor(fuse::ui::UiContext& ui) {
    const NodeHandle grid = ui.find("inspector.grid");
    for (NodeHandle row = ui.alive(grid) ? ui.firstChild(grid) : NodeHandle{}; ui.alive(row); row = ui.nextSibling(row))
        if (ui.text(ui.firstChild(row)) == "Position") return ui.nextSibling(ui.firstChild(row));
    return {};
}
} // namespace

int main(int argc, char** argv) {
    app::Application a{{.name = "fuse_editor_ui_a11y_linux", .argc = argc, .argv = argv, .loadSettings = false}};
    if (!fuse::ui::a11y::accessKitAvailable()) {
        std::printf("fuse_editor_ui_a11y_linux: skipped (built without AccessKit)\n");
        return 77;
    }
    if (!std::getenv("DBUS_SESSION_BUS_ADDRESS")) {
        std::printf("fuse_editor_ui_a11y_linux: skipped (no session bus; run under dbus-run-session)\n");
        return 77;
    }
    Atspi at;
    if (!at.load()) {
        std::printf("fuse_editor_ui_a11y_linux: skipped (libatspi.so.0 not found)\n");
        return 77;
    }
    std::string why;
    const pid_t launcher = startA11yBus(why);
    if (launcher < 0) {
        std::printf("fuse_editor_ui_a11y_linux: skipped (accessibility bus unavailable: %s)\n", why.c_str());
        return 77;
    }
    struct Reap { pid_t p; ~Reap() { if (p > 0) { kill(p, SIGTERM); waitpid(p, nullptr, 0); } } } reap{launcher};

    auto& win = a.createWindow({.title = "FUSE Editor", .width = 1600, .height = 900});
    EditorHost host;
    test::ScriptedServices sv;
    EditorController c(host, sv);
    fui::EditorShellWindow shell(win, c);
    CHECK(c.trigger(ActionId::EntityCreateEmpty));
    c.tick();
    shell.sync();
    const EntityRef e = c.lastCreated();
    c.selectOnly(e);
    shell.showPanel("dock.hierarchy");
    shell.showPanel("dock.inspector");
    shell.showPanel("dock.console");
    shell.sync();

    app::NativePlatform native(a, {.vulkan = false, .dpiAware = false}); // software presenter: no GPU needed
    if (!native.available()) {
        std::printf("fuse_editor_ui_a11y_linux: skipped (no X display)\n");
        return 77;
    }
    native.show(win);
    if (!native.nativeWindow(win)) {
        std::printf("fuse_editor_ui_a11y_linux: skipped (no native window)\n");
        return 77;
    }
    app::WindowAccessibility acc(native, win);
    CHECK(acc.active());
    for (int i = 0; i < 400 && win.framesPresented() < 1; ++i) a.step(0.01f);
    for (int i = 0; i < 10; ++i) a.step(0.005f);
    acc.setWindowFocused(true); // no window manager gives the window focus under Xvfb

    fuse::ui::UiContext& ui = win.ui();
    Observed ob;
    auto readX = [&] {
        float x = 0.f;
        shell.panels().withScene([&](EditorHost& h, const ViewportPanel&) {
            if (auto* t = h.editorScene().registry().get<fuse::ecs::Transform>(fuse::ecs::EntityID{e.index, e.generation})) x = t->position.x;
        });
        return x;
    };
    std::atomic<bool> done{false};
    std::thread th(client, std::ref(at), std::ref(ob), std::ref(done));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(100);
    while (!done && std::chrono::steady_clock::now() < deadline) {
        c.tick(); // posted inspector edits run on the "game thread"
        a.step(0.01f);
        ob.posX = readX();
        const NodeHandle pos = positionEditor(ui);
        ob.positionFocused = pos.valid() && ui.focused() == pos;
        if (ob.wantType.exchange(false) && ob.positionFocused) {
            // the keyboard half of the edit (what the user types once the screen reader put focus in the field)
            ui.keyDown(fuse::ui::Key::A, fuse::ui::kModCtrl);
            ui.textInput("3,0,0");
            ui.keyDown(fuse::ui::Key::Enter);
            ui.setFocus(shell.viewportNode()); // leave the field so the inspector refreshes (Undo shows the old value)
            win.requestRedraw();
        }
    }
    if (!done) {
        std::fprintf(stderr, "fuse_editor_ui_a11y_linux: FAILED (AT-SPI client did not finish within 100 s)\n");
        std::fflush(stderr);
        std::_Exit(1);
    }
    th.join();
    if (g_fail) {
        std::fprintf(stderr, "fuse_editor_ui_a11y_linux: FAILED (%d checks)\n", g_fail.load());
        return 1;
    }
    std::printf("fuse_editor_ui_a11y_linux: OK\n");
    return 0;
}
