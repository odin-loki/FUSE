// Backend seam of fuse::ui::web::WebView (one implementation per platform engine).
#pragma once

#include "fuse/ui/extras/webview.hpp"

namespace fuse::ui::web {

/// Injected at document start by every backend: `fuse.postMessage(s)` forwards to the engine's native channel.
inline constexpr const char* kBridgeScript =
    "(function(){var h=(window.webkit&&window.webkit.messageHandlers&&window.webkit.messageHandlers.fuse)?"
    "function(m){window.webkit.messageHandlers.fuse.postMessage(String(m));}:"
    "(window.chrome&&window.chrome.webview)?function(m){window.chrome.webview.postMessage(String(m));}:function(){};"
    "window.fuse={postMessage:h};})();";

class WebViewBackend {
public:
    explicit WebViewBackend(WebView& owner) : view(owner) {}
    virtual ~WebViewBackend() = default;
    virtual bool create(void* parent, Rect bounds) = 0;
    virtual void navigate(std::string_view url) = 0;
    virtual void loadHtml(std::string_view html, std::string_view baseUrl) = 0;
    virtual void reload() = 0;
    virtual void back() = 0;
    virtual void forward() = 0;
    virtual void stop() = 0;
    virtual void evaluate(std::string_view js, std::function<void(bool, std::string)> done) = 0;
    virtual std::string url() const = 0;
    virtual std::string title() const = 0;
    virtual void setBounds(Rect r) = 0;
    virtual void setVisible(bool v) = 0;
    virtual void setTitle(std::string_view t) = 0;
    virtual bool poll() = 0;
    // embedded-child helpers (default: not supported)
    virtual bool focus() { return false; }
    virtual bool hasFocus() const { return false; }
    virtual bool nativeGeometry(Rect& out, bool& viewable) const { (void)out; (void)viewable; return false; }
    WebView& view;
};

/// Implemented by the platform file compiled for this target.
bool platformWebViewAvailable();
const char* platformWebViewName();
std::unique_ptr<WebViewBackend> makePlatformWebView(WebView& owner);

} // namespace fuse::ui::web
