// fuse_ui_a11y_windows gate (plan §4.14, D7): a real fuse_app Win32 window with WindowAccessibility (AccessKit's UI
// Automation adapter) is read and driven by a UIA client (IUIAutomation, COM) on a helper thread of the same process
// while the main thread runs the app loop:
//   * ElementFromHandle + FindAll see the widgets with their UIA control types and names (button, check box, slider,
//     edit, list, tree, menu bar, menu item)
//   * the button's bounding rectangle is its UI rect in screen coordinates (client origin + DPI scale)
//   * InvokePattern.Invoke fires the button's Clicked signal; TogglePattern.Toggle checks the check box (Toggled);
//     ValuePattern.SetValue replaces the line edit's text (TextChanged); RangeValuePattern.SetValue moves the slider
//   * SetFocus on the edit moves UI focus and a UIA focus-changed event arrives for it
//   * ExpandCollapsePattern opens the File menu (its items appear) and closes it again
// Every wait is bounded; on a hang the gate fails and exits. Exit 77 without AccessKit in the build.
#include "fuse/app/accessibility.hpp"
#include "fuse/ui/widgets/widgets.hpp"

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
#include <cstdlib>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace fuse::ui;
namespace app = fuse::app;
namespace w = fuse::ui::widgets;

namespace {
std::atomic<int> g_fail{0};
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

bool waitFor(const std::function<bool()>& pred, double seconds = 5.0) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < end) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return pred();
}

template <class T> struct Com { // minimal owning COM pointer
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

/// Focus-changed handler: records the names of the elements that received focus.
struct FocusHandler : IUIAutomationFocusChangedEventHandler {
    std::atomic<ULONG> refs{1};
    std::mutex mu;
    std::vector<std::wstring> names;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if (id == __uuidof(IUnknown) || id == __uuidof(IUIAutomationFocusChangedEventHandler)) {
            *out = static_cast<IUIAutomationFocusChangedEventHandler*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { return --refs; } // stack object: never deleted here
    HRESULT STDMETHODCALLTYPE HandleFocusChangedEvent(IUIAutomationElement* sender) override {
        BSTR n = nullptr;
        if (sender && SUCCEEDED(sender->get_CurrentName(&n))) {
            std::lock_guard<std::mutex> lock(mu);
            names.push_back(bstr(n));
        }
        return S_OK;
    }
    bool saw(const std::wstring& name) {
        std::lock_guard<std::mutex> lock(mu);
        for (const auto& n : names)
            if (n == name) return true;
        return false;
    }
};

/// UI-thread state the client checks (written by signal slots on the UI thread).
struct Observed {
    std::atomic<int> clicked{0};
    std::atomic<bool> toggled{false};
    std::atomic<f32> slider{0.f};
    std::mutex mu;
    std::string text;
    std::atomic<u64> focused{0};
    std::string textNow() { std::lock_guard<std::mutex> l(mu); return text; }
};

struct Expect {
    HWND hwnd = nullptr;
    RECT saveScreen{}; ///< the Save button in screen px
};

// ---- the UIA client (helper thread) -----------------------------------------------------------------------------
void client(const Expect& ex, Observed& ob, std::atomic<bool>& done) {
    CHECK(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)));
    {
        Com<IUIAutomation> uia;
        HRESULT hr = CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, __uuidof(IUIAutomation),
                                      reinterpret_cast<void**>(uia.put()));
        CHECK(SUCCEEDED(hr) && uia);
        if (!uia) { done = true; CoUninitialize(); return; }
        Com<IUIAutomationElement> root;
        CHECK(SUCCEEDED(uia->ElementFromHandle(ex.hwnd, root.put())) && root);
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
        // the provider activates on the first WM_GETOBJECT: wait for the tree
        CHECK(waitFor([&] { scan(); return has(L"Save", UIA_ButtonControlTypeId); }));
        std::printf("  UIA tree: %zu descendants\n", seen.size());
        for (const auto& f : seen) std::printf("    %d \"%s\"\n", int(f.type), narrow(f.name).c_str());
        CHECK(has(L"Autosave", UIA_CheckBoxControlTypeId));
        CHECK(has(L"Volume", UIA_SliderControlTypeId));
        CHECK(has(L"Search", UIA_EditControlTypeId));
        CHECK(has(L"Fruits", UIA_ListControlTypeId));
        CHECK(has(L"Scene", UIA_TreeControlTypeId));
        CHECK(has(L"File", UIA_MenuItemControlTypeId));
        bool bar = false;
        for (const auto& f : seen) bar |= f.type == UIA_MenuBarControlTypeId;
        CHECK(bar);

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

        // bounds: screen coordinates of the UI rect
        Com<IUIAutomationElement> save;
        save.p = find(L"Save");
        CHECK(save);
        if (save) {
            RECT r{};
            CHECK(SUCCEEDED(save->get_CurrentBoundingRectangle(&r)));
            std::printf("  Save bounds %ld,%ld..%ld,%ld (expected %ld,%ld..%ld,%ld)\n", r.left, r.top, r.right, r.bottom,
                        ex.saveScreen.left, ex.saveScreen.top, ex.saveScreen.right, ex.saveScreen.bottom);
            CHECK(std::abs(r.left - ex.saveScreen.left) <= 1 && std::abs(r.top - ex.saveScreen.top) <= 1 &&
                  std::abs(r.right - ex.saveScreen.right) <= 1 && std::abs(r.bottom - ex.saveScreen.bottom) <= 1);
        }

        FocusHandler focus;
        CHECK(SUCCEEDED(uia->AddFocusChangedEventHandler(nullptr, &focus)));

        // Invoke
        Com<IUIAutomationInvokePattern> inv;
        CHECK(pattern(save.p, UIA_InvokePatternId, __uuidof(IUIAutomationInvokePattern), reinterpret_cast<void**>(inv.put())));
        if (inv) {
            CHECK(SUCCEEDED(inv->Invoke()));
            CHECK(waitFor([&] { return ob.clicked.load() == 1; }));
        }
        // Toggle
        Com<IUIAutomationElement> cb;
        cb.p = find(L"Autosave");
        Com<IUIAutomationTogglePattern> tog;
        CHECK(pattern(cb.p, UIA_TogglePatternId, __uuidof(IUIAutomationTogglePattern), reinterpret_cast<void**>(tog.put())));
        if (tog) {
            ToggleState st = ToggleState_Indeterminate;
            CHECK(SUCCEEDED(tog->get_CurrentToggleState(&st)) && st == ToggleState_Off);
            CHECK(SUCCEEDED(tog->Toggle()));
            CHECK(waitFor([&] { return ob.toggled.load(); }));
            CHECK(waitFor([&] { return SUCCEEDED(tog->get_CurrentToggleState(&st)) && st == ToggleState_On; }));
        }
        // Value (line edit)
        Com<IUIAutomationElement> edit;
        edit.p = find(L"Search");
        Com<IUIAutomationValuePattern> val;
        CHECK(pattern(edit.p, UIA_ValuePatternId, __uuidof(IUIAutomationValuePattern), reinterpret_cast<void**>(val.put())));
        if (val) {
            BSTR b = nullptr;
            CHECK(SUCCEEDED(val->get_CurrentValue(&b)) && bstr(b) == L"hello");
            BSTR nv = SysAllocString(L"typed by UIA");
            CHECK(SUCCEEDED(val->SetValue(nv)));
            SysFreeString(nv);
            CHECK(waitFor([&] { return ob.textNow() == "typed by UIA"; }));
            CHECK(waitFor([&] { BSTR v = nullptr; return SUCCEEDED(val->get_CurrentValue(&v)) && bstr(v) == L"typed by UIA"; }));
        }
        // RangeValue (slider)
        Com<IUIAutomationElement> sl;
        sl.p = find(L"Volume");
        Com<IUIAutomationRangeValuePattern> range;
        CHECK(pattern(sl.p, UIA_RangeValuePatternId, __uuidof(IUIAutomationRangeValuePattern), reinterpret_cast<void**>(range.put())));
        if (range) {
            double mx = 0.0;
            CHECK(SUCCEEDED(range->get_CurrentMaximum(&mx)) && mx == 100.0);
            CHECK(SUCCEEDED(range->SetValue(75.0)));
            CHECK(waitFor([&] { return ob.slider.load() == 75.f; }));
        }
        // Focus: UIA SetFocus -> UI focus -> focus-changed event
        if (edit) {
            CHECK(SUCCEEDED(edit->SetFocus()));
            CHECK(waitFor([&] { return ob.focused.load() == 1; }));
            CHECK(waitFor([&] { return focus.saw(L"Search"); }));
            BOOL kf = FALSE;
            CHECK(waitFor([&] { return SUCCEEDED(edit->get_CurrentHasKeyboardFocus(&kf)) && kf; }));
        }
        // ExpandCollapse (menu bar title)
        Com<IUIAutomationElement> file;
        file.p = find(L"File");
        Com<IUIAutomationExpandCollapsePattern> ec;
        CHECK(pattern(file.p, UIA_ExpandCollapsePatternId, __uuidof(IUIAutomationExpandCollapsePattern), reinterpret_cast<void**>(ec.put())));
        if (ec) {
            ExpandCollapseState st = ExpandCollapseState_LeafNode;
            CHECK(SUCCEEDED(ec->get_CurrentExpandCollapseState(&st)) && st == ExpandCollapseState_Collapsed);
            CHECK(SUCCEEDED(ec->Expand()));
            CHECK(waitFor([&] { scan(); return has(L"Open", UIA_MenuItemControlTypeId); }));
            CHECK(waitFor([&] { return SUCCEEDED(ec->get_CurrentExpandCollapseState(&st)) && st == ExpandCollapseState_Expanded; }));
            bool menu = false;
            for (const auto& f : seen) menu |= f.type == UIA_MenuControlTypeId;
            CHECK(menu);
            CHECK(SUCCEEDED(ec->Collapse()));
            CHECK(waitFor([&] { scan(); return !has(L"Open", UIA_MenuItemControlTypeId); }));
            CHECK(waitFor([&] { return SUCCEEDED(ec->get_CurrentExpandCollapseState(&st)) && st == ExpandCollapseState_Collapsed; }));
        }
        {
            std::lock_guard<std::mutex> lock(focus.mu);
            std::printf("  focus events: %zu\n", focus.names.size());
        }
        uia->RemoveAllEventHandlers();
    }
    CoUninitialize();
    done = true;
}
} // namespace

int main(int argc, char** argv) {
    app::Application a{{.name = "gate_a11y_windows", .argc = argc, .argv = argv, .loadSettings = false}};
    if (!fuse::ui::a11y::accessKitAvailable()) {
        std::printf("fuse_ui_a11y_windows: skipped (built without AccessKit: set FUSE_UI_ACCESSKIT_BUILD=ON or FUSE_UI_ACCESSKIT_DIR)\n");
        return 77;
    }
    app::NativePlatform native(a, {.vulkan = false}); // GDI presenter: no GPU needed
    auto& win = a.createWindow({.title = "fuse_ui_a11y_windows", .width = 480, .height = 360});
    UiContext& ui = win.ui();
    const NodeHandle bar = w::menuBar(ui, ui.root());
    w::menuBarMenu(ui, bar, "File", "Open\nSave As\nQuit");
    const NodeHandle panel = w::panel(ui, ui.root());
    const NodeHandle save = w::button(ui, panel, "Save", "save");
    const NodeHandle cb = w::checkBox(ui, panel, "Autosave", false, "autosave");
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
    ui.connect(le, SignalKind::TextChanged, [&](UiContext&, NodeHandle, const SignalArgs& s) {
        std::lock_guard<std::mutex> l(ob.mu);
        ob.text = s.text;
    });

    native.show(win);
    const HWND hwnd = reinterpret_cast<HWND>(native.nativeWindow(win));
    if (!hwnd) {
        std::printf("fuse_ui_a11y_windows: skipped (no native window: %.*s)\n", int(native.lastError().size()), native.lastError().data());
        return 77;
    }
    app::WindowAccessibility acc(native, win);
    CHECK(acc.active());
    for (int i = 0; i < 400 && win.framesPresented() < 1; ++i) a.step(0.01f);
    for (int i = 0; i < 10; ++i) a.step(0.005f);
    SetForegroundWindow(hwnd);
    for (int i = 0; i < 10; ++i) a.step(0.005f);
    if (GetForegroundWindow() != hwnd) {
        // a background process may not take the foreground; report the window as focused so focus events flow
        std::printf("  window is not foreground (another app holds it): reporting it focused to the adapter\n");
        acc.setWindowFocused(true);
    }

    Expect ex;
    ex.hwnd = hwnd;
    {
        RECT cr{};
        GetClientRect(hwnd, &cr);
        const double k = double(cr.right) / double(ui.viewport().w);
        POINT o{0, 0};
        ClientToScreen(hwnd, &o);
        const Rect r = ui.rect(save);
        ex.saveScreen = {LONG(std::lround(o.x + r.x * k)), LONG(std::lround(o.y + r.y * k)),
                         LONG(std::lround(o.x + (r.x + r.w) * k)), LONG(std::lround(o.y + (r.y + r.h) * k))};
    }

    std::atomic<bool> done{false};
    std::thread th(client, std::cref(ex), std::ref(ob), std::ref(done));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (!done && std::chrono::steady_clock::now() < deadline) {
        a.step(0.01f);
        ob.focused = ui.focused() == le ? 1 : 0;
    }
    if (!done) {
        std::fprintf(stderr, "fuse_ui_a11y_windows: FAILED (UIA client did not finish within 60 s)\n");
        std::fflush(stderr);
        TerminateProcess(GetCurrentProcess(), 1); // the client thread may be stuck inside a COM call
    }
    th.join();
    for (int i = 0; i < 5; ++i) a.step(0.005f);
    CHECK(ui.checked(cb));
    CHECK(ui.text(le) == "typed by UIA");
    if (g_fail) {
        std::fprintf(stderr, "fuse_ui_a11y_windows: FAILED (%d checks)\n", g_fail.load());
        return 1;
    }
    std::printf("fuse_ui_a11y_windows: OK\n");
    return 0;
}
