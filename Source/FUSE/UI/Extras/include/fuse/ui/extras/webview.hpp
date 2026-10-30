// Web view (QWebEngineView parity) on the system engine, loaded at runtime: WebKitGTK (libwebkit2gtk-4.1 / 4.0 +
// GTK 3) on Linux / BSD, WebView2 (WebView2Loader.dll; built when FUSE_UI_WEBVIEW2_DIR names the SDK) on
// Windows, WKWebView on macOS. No engine is linked: when none can be loaded `available()` is false and every call
// is a no-op. Verified: WebKitGTK (gate fuse_ui_extras_webview under Xvfb). Written, not run here: WebView2, WKWebView.
//
// The view is a native child of `parent` (X11 Window id / HWND / NSView*) placed at `bounds` (parent pixels), or
// its own top-level window when parent is null. JavaScript reaches the host with `fuse.postMessage(string)`.
#pragma once

#include "fuse/ui/types.hpp"

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace fuse::ui::web {

class WebViewBackend;

class WebView {
public:
    WebView();
    ~WebView();
    WebView(const WebView&) = delete;
    WebView& operator=(const WebView&) = delete;

    /// True when a system engine can be loaded on this machine.
    static bool available();
    /// "webkitgtk-4.1", "webkitgtk-4.0", "webview2", "wkwebview" or "none".
    static const char* backendName();

    bool create(void* parent = nullptr, Rect bounds = {0, 0, 800, 600});
    bool valid() const { return m_b != nullptr; }
    void destroy();

    void navigate(std::string_view url);
    void loadHtml(std::string_view html, std::string_view baseUrl = {});
    void reload();
    void back();
    void forward();
    void stop();
    /// Runs `js` in the page; `done(ok, resultAsString)` fires from poll().
    void evaluate(std::string_view js, std::function<void(bool, std::string)> done = {});
    std::string url() const;
    std::string title() const;
    void setBounds(Rect r);
    void setVisible(bool v);
    void setTitle(std::string_view windowTitle); ///< top-level mode
    /// Pumps the engine's events (call every frame on the UI thread). Returns true if anything was handled.
    bool poll();

    std::function<void(const std::string&)> onTitleChanged;
    std::function<void(const std::string&)> onLoadFinished; ///< url
    std::function<void(const std::string&)> onMessage;      ///< fuse.postMessage(text)

private:
    std::unique_ptr<WebViewBackend> m_b;
};

} // namespace fuse::ui::web
