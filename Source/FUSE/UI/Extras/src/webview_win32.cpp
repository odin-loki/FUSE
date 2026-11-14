// WebView2 backend (Windows). WebView2Loader.dll is loaded at runtime through the os seam; the interfaces come
// from the WebView2 SDK header (configure with -DFUSE_UI_WEBVIEW2_DIR=<sdk>/build/native). Without the SDK, or
// without the Edge WebView2 runtime installed, available() is false.
// UNVERIFIED: compiled with llvm-mingw against SDK 1.0.4314.35; not run (Wine has no WebView2 runtime).
#if defined(_WIN32)
#include "webview_internal.hpp"

#if defined(FUSE_UI_HAVE_WEBVIEW2)
#include "fuse/ui/os.hpp"

#include <windows.h>
#include <objbase.h>
#include "WebView2.h"

#include <deque>
#include <vector>

namespace fuse::ui::web {
namespace {
std::wstring wide(std::string_view s) {
    if (s.empty()) return {};
    std::wstring w(std::size_t(MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), int(w.size()));
    return w;
}
std::string narrow(const wchar_t* w) {
    if (!w || !*w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(std::size_t(n > 0 ? n - 1 : 0), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}
/// ExecuteScript yields JSON; strings are unquoted so results match the WebKit backends' toString().
std::string fromJson(const std::string& j) {
    if (j.size() < 2 || j.front() != '"') return j;
    std::string o;
    for (std::size_t i = 1; i + 1 < j.size(); ++i) {
        if (j[i] != '\\' || i + 2 >= j.size()) { o += j[i]; continue; }
        const char c = j[++i];
        o += c == 'n' ? '\n' : c == 't' ? '\t' : c == 'r' ? '\r' : c;
    }
    return o;
}

using CreateEnvFn = HRESULT(STDAPICALLTYPE*)(PCWSTR, PCWSTR, ICoreWebView2EnvironmentOptions*, ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*);
using VersionFn = HRESULT(STDAPICALLTYPE*)(PCWSTR, LPWSTR*);
struct Loader {
    std::unique_ptr<os::Library> lib;
    CreateEnvFn create = nullptr;
    bool ok = false;
};
Loader* loader() {
    static Loader& l = *new Loader{};
    static bool done = [] {
        l.lib = os::loadLibrary({"WebView2Loader.dll"});
        if (!l.lib) return false;
        l.create = l.lib->get<CreateEnvFn>("CreateCoreWebView2EnvironmentWithOptions");
        auto ver = l.lib->get<VersionFn>("GetAvailableCoreWebView2BrowserVersionString");
        LPWSTR v = nullptr;
        const bool runtime = ver && SUCCEEDED(ver(nullptr, &v)) && v;
        if (v) CoTaskMemFree(v);
        return l.create && runtime;
    }();
    return done ? &l : nullptr;
}

/// Minimal COM object for one handler interface, forwarding Invoke to a std::function.
template <class I, class... A>
class Handler final : public I {
public:
    Handler(const IID& iid, std::function<HRESULT(A...)> f) : m_iid(iid), m_f(std::move(f)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, m_iid)) { *out = static_cast<I*>(this); AddRef(); return S_OK; }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refs; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = --m_refs;
        if (!r) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE Invoke(A... a) override { return m_f ? m_f(a...) : S_OK; }

private:
    IID m_iid;
    std::function<HRESULT(A...)> m_f;
    ULONG m_refs = 1;
};
template <class I, class... A, class F>
I* handler(const IID& iid, F f) { return new Handler<I, A...>(iid, std::function<HRESULT(A...)>(std::move(f))); }

LRESULT CALLBACK hostProc(HWND h, UINT m, WPARAM w, LPARAM l);
const wchar_t* hostClass() {
    static const wchar_t* name = [] {
        WNDCLASSEXW wc{sizeof wc};
        wc.lpfnWndProc = hostProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        wc.lpszClassName = L"FuseWebViewHost";
        RegisterClassExW(&wc);
        return L"FuseWebViewHost";
    }();
    return name;
}

class Wv2 final : public WebViewBackend {
public:
    using WebViewBackend::WebViewBackend;
    ~Wv2() override {
        *m_alive = false;
        if (m_ctrl) { m_ctrl->Close(); m_ctrl->Release(); }
        if (m_wv) m_wv->Release();
        if (m_env) m_env->Release();
        if (m_hwnd) DestroyWindow(m_hwnd);
    }
    bool create(void* parent, Rect b) override {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const DWORD style = parent ? (WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN) : (WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        m_hwnd = CreateWindowExW(0, hostClass(), L"", style, int(b.x), int(b.y), int(b.w), int(b.h), static_cast<HWND>(parent), nullptr,
                                 GetModuleHandleW(nullptr), nullptr);
        if (!m_hwnd) return false;
        SetPropW(m_hwnd, L"FuseWebView", this);
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        const std::wstring data = std::wstring(tmp) + L"FuseWebView2";
        auto alive = m_alive;
        auto* envDone = handler<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler, HRESULT, ICoreWebView2Environment*>(IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler,
            [this, alive](HRESULT hr, ICoreWebView2Environment* env) -> HRESULT {
                if (!*alive || FAILED(hr) || !env) return S_OK;
                m_env = env;
                m_env->AddRef();
                auto* ctrlDone = handler<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler, HRESULT, ICoreWebView2Controller*>(IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler,
                    [this, alive](HRESULT hr2, ICoreWebView2Controller* c) -> HRESULT {
                        if (!*alive || FAILED(hr2) || !c) return S_OK;
                        ready(c);
                        return S_OK;
                    });
                m_env->CreateCoreWebView2Controller(m_hwnd, ctrlDone);
                ctrlDone->Release();
                return S_OK;
            });
        const HRESULT hr = loader()->create(nullptr, data.c_str(), nullptr, envDone);
        envDone->Release();
        return SUCCEEDED(hr);
    }
    void navigate(std::string_view url) override { run([u = wide(url)](ICoreWebView2* w) { w->Navigate(u.c_str()); }); }
    void loadHtml(std::string_view html, std::string_view) override {
        run([h = wide(html)](ICoreWebView2* w) { w->NavigateToString(h.c_str()); }); // base URL unsupported by NavigateToString
    }
    void reload() override { run([](ICoreWebView2* w) { w->Reload(); }); }
    void back() override { run([](ICoreWebView2* w) { w->GoBack(); }); }
    void forward() override { run([](ICoreWebView2* w) { w->GoForward(); }); }
    void stop() override { run([](ICoreWebView2* w) { w->Stop(); }); }
    void evaluate(std::string_view js, std::function<void(bool, std::string)> done) override {
        auto alive = m_alive;
        run([s = wide(js), done = std::move(done), alive](ICoreWebView2* w) {
            auto* h = handler<ICoreWebView2ExecuteScriptCompletedHandler, HRESULT, LPCWSTR>(IID_ICoreWebView2ExecuteScriptCompletedHandler,
            [done, alive](HRESULT hr, LPCWSTR json) -> HRESULT {
                if (*alive && done) done(SUCCEEDED(hr), fromJson(narrow(json)));
                return S_OK;
            });
            w->ExecuteScript(s.c_str(), h);
            h->Release();
        });
    }
    std::string url() const override { return str(&ICoreWebView2::get_Source); }
    std::string title() const override { return str(&ICoreWebView2::get_DocumentTitle); }
    void setBounds(Rect r) override {
        if (!m_hwnd) return;
        MoveWindow(m_hwnd, int(r.x), int(r.y), int(r.w), int(r.h), TRUE);
        fit();
    }
    void setVisible(bool v) override {
        if (m_hwnd) ShowWindow(m_hwnd, v ? SW_SHOW : SW_HIDE);
        if (m_ctrl) m_ctrl->put_IsVisible(v);
    }
    void setTitle(std::string_view t) override { if (m_hwnd) SetWindowTextW(m_hwnd, wide(t).c_str()); }
    bool poll() override {
        bool any = false;
        MSG msg;
        for (int i = 0; i < 256 && PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE); ++i) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            any = true;
        }
        return any;
    }
    void fit() {
        if (!m_ctrl) return;
        RECT rc;
        GetClientRect(m_hwnd, &rc);
        m_ctrl->put_Bounds(rc);
    }

private:
    void ready(ICoreWebView2Controller* c) {
        m_ctrl = c;
        m_ctrl->AddRef();
        m_ctrl->get_CoreWebView2(&m_wv);
        fit();
        const std::wstring bridge = wide(kBridgeScript);
        m_wv->AddScriptToExecuteOnDocumentCreated(bridge.c_str(), nullptr);
        EventRegistrationToken tok;
        auto* msg = handler<ICoreWebView2WebMessageReceivedEventHandler, ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs*>(IID_ICoreWebView2WebMessageReceivedEventHandler,
            [this](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* a) -> HRESULT {
                LPWSTR s = nullptr;
                if (SUCCEEDED(a->TryGetWebMessageAsString(&s)) && view.onMessage) view.onMessage(narrow(s));
                if (s) CoTaskMemFree(s);
                return S_OK;
            });
        m_wv->add_WebMessageReceived(msg, &tok);
        msg->Release();
        auto* title = handler<ICoreWebView2DocumentTitleChangedEventHandler, ICoreWebView2*, IUnknown*>(IID_ICoreWebView2DocumentTitleChangedEventHandler,
            [this](ICoreWebView2*, IUnknown*) -> HRESULT {
            if (view.onTitleChanged) view.onTitleChanged(this->title());
            return S_OK;
        });
        m_wv->add_DocumentTitleChanged(title, &tok);
        title->Release();
        auto* nav = handler<ICoreWebView2NavigationCompletedEventHandler, ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*>(IID_ICoreWebView2NavigationCompletedEventHandler,
            [this](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT {
                if (view.onLoadFinished) view.onLoadFinished(url());
                return S_OK;
            });
        m_wv->add_NavigationCompleted(nav, &tok);
        nav->Release();
        while (!m_queue.empty()) { m_queue.front()(m_wv); m_queue.pop_front(); }
    }
    void run(std::function<void(ICoreWebView2*)> f) {
        if (m_wv) f(m_wv);
        else m_queue.push_back(std::move(f));
    }
    std::string str(HRESULT (STDMETHODCALLTYPE ICoreWebView2::*get)(LPWSTR*)) const {
        if (!m_wv) return {};
        LPWSTR s = nullptr;
        std::string r;
        if (SUCCEEDED((m_wv->*get)(&s))) r = narrow(s);
        if (s) CoTaskMemFree(s);
        return r;
    }
    HWND m_hwnd = nullptr;
    ICoreWebView2Environment* m_env = nullptr;
    ICoreWebView2Controller* m_ctrl = nullptr;
    ICoreWebView2* m_wv = nullptr;
    std::deque<std::function<void(ICoreWebView2*)>> m_queue;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};

LRESULT CALLBACK hostProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_SIZE) if (auto* v = static_cast<Wv2*>(GetPropW(h, L"FuseWebView"))) v->fit();
    if (m == WM_CLOSE && !(GetWindowLongW(h, GWL_STYLE) & WS_CHILD)) { ShowWindow(h, SW_HIDE); return 0; }
    return DefWindowProcW(h, m, w, l);
}
} // namespace

bool platformWebViewAvailable() { return loader() != nullptr; }
const char* platformWebViewName() { return "webview2"; }
std::unique_ptr<WebViewBackend> makePlatformWebView(WebView& owner) { return std::make_unique<Wv2>(owner); }
} // namespace fuse::ui::web

#else // no SDK at configure time
namespace fuse::ui::web {
bool platformWebViewAvailable() { return false; }
const char* platformWebViewName() { return "none"; }
std::unique_ptr<WebViewBackend> makePlatformWebView(WebView&) { return nullptr; }
} // namespace fuse::ui::web
#endif
#endif
