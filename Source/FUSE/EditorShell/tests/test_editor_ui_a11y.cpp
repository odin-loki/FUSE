// fuse_editor_ui_a11y: the FUSE editor shell on a real Win32 window with WindowAccessibility (AccessKit UI Automation),
// read and driven by a UIA client (IUIAutomation, COM) on a helper thread while the main thread runs the editor:
//   * the menu bar and its File / Edit / Entity / Play / View items, the toolbar buttons and the visible panel pages
//     (Hierarchy, Inspector, Console) are in the UIA tree with their control types and names
//   * the selected entity's inspector fields are named "<component> <field>" (Transform Position, ...)
//   * ValuePattern.SetValue on the Position field edits the entity (through the controller, undoable)
//   * InvokePattern on the Undo toolbar button reverts it; ExpandCollapse opens the Entity menu
// Exit 77 without AccessKit or without a native window.
#include "fuse/app/accessibility.hpp"
#include "fuse/app/native.hpp"
#include "fuse/editor/fui/editor_shell.hpp"
#include "scripted_services.hpp"

#include <fuse/ecs/components/transform.hpp>
#include <fuse/editor/editor_host.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <ole2.h>
#include <uiautomation.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace fuse::editor;
namespace app = fuse::app;

namespace {
std::atomic<int> g_fail{0};
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

bool waitFor(const std::function<bool()>& pred, double seconds = 10.0) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < end) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return pred();
}

template <class T> struct Com {
    T* p = nullptr;
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    ~Com() { reset(); }
    void reset() { if (p) p->Release(); p = nullptr; }
    T** put() { reset(); return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

std::wstring bstr(BSTR b) {
    std::wstring s = b ? std::wstring(b, SysStringLen(b)) : std::wstring();
    SysFreeString(b);
    return s;
}
std::string narrow(const std::wstring& s) {
    std::string o;
    for (wchar_t c : s) o += c < 128 ? char(c) : '?';
    return o;
}

/// Written by the UI thread, read by the client.
struct Observed {
    std::atomic<float> posX{0.f};
};

void client(HWND hwnd, Observed& ob, std::atomic<bool>& done) {
    CHECK(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)));
    {
        Com<IUIAutomation> uia;
        CHECK(SUCCEEDED(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, __uuidof(IUIAutomation),
                                         reinterpret_cast<void**>(uia.put()))) && uia);
        if (!uia) { done = true; CoUninitialize(); return; }
        Com<IUIAutomationElement> root;
        CHECK(SUCCEEDED(uia->ElementFromHandle(hwnd, root.put())) && root);
        Com<IUIAutomationCondition> all;
        uia->CreateTrueCondition(all.put());
        struct Found { std::wstring name; CONTROLTYPEID type = 0; };
        std::vector<Found> seen;
        auto scan = [&] {
            seen.clear();
            Com<IUIAutomationElementArray> arr;
            if (!root || FAILED(root->FindAll(TreeScope_Descendants, all.p, arr.put())) || !arr) return;
            int n = 0;
            arr->get_Length(&n);
            for (int i = 0; i < n; ++i) {
                Com<IUIAutomationElement> e;
                if (FAILED(arr->GetElement(i, e.put()))) continue;
                Found f;
                BSTR b = nullptr;
                if (SUCCEEDED(e->get_CurrentName(&b))) f.name = bstr(b);
                e->get_CurrentControlType(&f.type);
                seen.push_back(f);
            }
        };
        auto has = [&](const wchar_t* name, CONTROLTYPEID t) {
            for (const auto& f : seen)
                if (f.name == name && f.type == t) return true;
            return false;
        };
        auto find = [&](const wchar_t* name) {
            Com<IUIAutomationCondition> c;
            VARIANT v;
            v.vt = VT_BSTR;
            v.bstrVal = SysAllocString(name);
            uia->CreatePropertyCondition(UIA_NamePropertyId, v, c.put());
            VariantClear(&v);
            IUIAutomationElement* e = nullptr;
            if (c) root->FindFirst(TreeScope_Descendants, c.p, &e);
            return e;
        };
        auto pattern = [](IUIAutomationElement* e, PATTERNID id, REFIID iid, void** out) {
            *out = nullptr;
            return e && SUCCEEDED(e->GetCurrentPatternAs(id, iid, out)) && *out;
        };
        CHECK(waitFor([&] { scan(); return has(L"File", UIA_MenuItemControlTypeId); }));
        std::printf("  UIA tree: %zu descendants\n", seen.size());
        for (const auto& f : seen)
            if (!f.name.empty()) std::printf("    %d \"%s\"\n", int(f.type), narrow(f.name).c_str());
        // menus
        bool bar = false;
        for (const auto& f : seen) bar |= f.type == UIA_MenuBarControlTypeId;
        CHECK(bar);
        for (const wchar_t* m : {L"File", L"Edit", L"Entity", L"Play", L"View"}) CHECK(has(m, UIA_MenuItemControlTypeId));
        // panels (visible tab pages) and the toolbar
        for (const wchar_t* p : {L"Hierarchy", L"Inspector", L"Console"}) CHECK(has(p, UIA_GroupControlTypeId));
        for (const wchar_t* b : {L"Undo", L"Redo", L"Save"}) CHECK(has(b, UIA_ButtonControlTypeId));
        // inspector fields of the selection
        CHECK(has(L"Transform Position", UIA_EditControlTypeId));
        Com<IUIAutomationElement> pos;
        pos.p = find(L"Transform Position");
        Com<IUIAutomationValuePattern> val;
        CHECK(pattern(pos.p, UIA_ValuePatternId, __uuidof(IUIAutomationValuePattern), reinterpret_cast<void**>(val.put())));
        if (val) {
            BSTR nv = SysAllocString(L"3,0,0");
            CHECK(SUCCEEDED(val->SetValue(nv)));
            SysFreeString(nv);
            CHECK(waitFor([&] { return std::fabs(ob.posX.load() - 3.f) < 1e-4f; }));
        }
        // Undo through the toolbar button
        Com<IUIAutomationElement> undo;
        undo.p = find(L"Undo");
        Com<IUIAutomationInvokePattern> inv;
        CHECK(pattern(undo.p, UIA_InvokePatternId, __uuidof(IUIAutomationInvokePattern), reinterpret_cast<void**>(inv.put())));
        if (inv) {
            CHECK(SUCCEEDED(inv->Invoke()));
            CHECK(waitFor([&] { return std::fabs(ob.posX.load() - 3.f) > 1e-3f; }));
        }
        // the Entity menu opens and lists its actions
        Com<IUIAutomationElement> menu;
        menu.p = find(L"Entity");
        Com<IUIAutomationExpandCollapsePattern> ec;
        CHECK(pattern(menu.p, UIA_ExpandCollapsePatternId, __uuidof(IUIAutomationExpandCollapsePattern), reinterpret_cast<void**>(ec.put())));
        if (ec) {
            CHECK(SUCCEEDED(ec->Expand()));
            CHECK(waitFor([&] {
                scan();
                for (const auto& f : seen)
                    if (f.type == UIA_MenuItemControlTypeId && f.name.find(L"Create SDF Sphere") == 0) return true;
                return false;
            }));
            CHECK(SUCCEEDED(ec->Collapse()));
        }
        uia->RemoveAllEventHandlers();
    }
    CoUninitialize();
    done = true;
}
} // namespace

int main(int argc, char** argv) {
    app::Application a{{.name = "fuse_editor_ui_a11y", .argc = argc, .argv = argv, .loadSettings = false}};
    if (!fuse::ui::a11y::accessKitAvailable()) {
        std::printf("fuse_editor_ui_a11y: skipped (built without AccessKit)\n");
        return 77;
    }
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

    app::NativePlatform native(a, {.vulkan = false});
    native.show(win);
    const HWND hwnd = reinterpret_cast<HWND>(native.nativeWindow(win));
    if (!hwnd) {
        std::printf("fuse_editor_ui_a11y: skipped (no native window)\n");
        return 77;
    }
    app::WindowAccessibility acc(native, win);
    CHECK(acc.active());
    for (int i = 0; i < 400 && win.framesPresented() < 1; ++i) a.step(0.01f);
    SetForegroundWindow(hwnd);
    for (int i = 0; i < 10; ++i) a.step(0.005f);
    if (GetForegroundWindow() != hwnd) acc.setWindowFocused(true);

    Observed ob;
    auto readX = [&] {
        float x = 0.f;
        shell.panels().withScene([&](EditorHost& h, const ViewportPanel&) {
            if (auto* t = h.editorScene().registry().get<fuse::ecs::Transform>(fuse::ecs::EntityID{e.index, e.generation})) x = t->position.x;
        });
        return x;
    };
    std::atomic<bool> done{false};
    std::thread th(client, hwnd, std::ref(ob), std::ref(done));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
    while (!done && std::chrono::steady_clock::now() < deadline) {
        c.tick(); // posted inspector edits run on the "game thread"
        a.step(0.01f);
        ob.posX = readX();
    }
    if (!done) {
        std::fprintf(stderr, "fuse_editor_ui_a11y: FAILED (UIA client did not finish within 90 s)\n");
        std::fflush(stderr);
        TerminateProcess(GetCurrentProcess(), 1);
    }
    th.join();
    if (g_fail) {
        std::fprintf(stderr, "fuse_editor_ui_a11y: FAILED (%d checks)\n", g_fail.load());
        return 1;
    }
    std::printf("fuse_editor_ui_a11y: OK\n");
    return 0;
}
