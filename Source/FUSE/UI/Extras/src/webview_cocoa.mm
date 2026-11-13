// WKWebView backend (macOS). WebKit.framework is loaded at runtime (dlopen + NSClassFromString), so nothing links
// against it; headers are used for the types only. UNVERIFIED: written without a macOS toolchain, never compiled.
#include "webview_internal.hpp"

#import <AppKit/AppKit.h>
#import <WebKit/WebKit.h>

#include <dlfcn.h>

namespace fuse::ui::web { class WkWebView; }

@interface FuseWkDelegate : NSObject <WKScriptMessageHandler, WKNavigationDelegate>
@property(nonatomic, assign) fuse::ui::web::WkWebView* owner;
@end

namespace fuse::ui::web {
namespace {
bool loadWebKit() {
    static bool ok = dlopen("/System/Library/Frameworks/WebKit.framework/WebKit", RTLD_LAZY) != nullptr && NSClassFromString(@"WKWebView") != nil;
    return ok;
}
NSString* ns(std::string_view s) { return [[NSString alloc] initWithBytes:s.data() length:s.size() encoding:NSUTF8StringEncoding]; }
std::string str(NSString* s) { return s ? std::string([s UTF8String]) : std::string{}; }
} // namespace

class WkWebView final : public WebViewBackend {
public:
    using WebViewBackend::WebViewBackend;
    ~WkWebView() override {
        [m_view.configuration.userContentController removeScriptMessageHandlerForName:@"fuse"];
        [m_view removeObserver:m_delegate forKeyPath:@"title"];
        [m_view removeFromSuperview];
        if (m_window) [m_window close];
        m_delegate.owner = nullptr;
    }
    bool create(void* parent, Rect b) override {
        WKWebViewConfiguration* cfg = [[NSClassFromString(@"WKWebViewConfiguration") alloc] init];
        m_delegate = [[FuseWkDelegate alloc] init];
        m_delegate.owner = this;
        WKUserScript* bridge = [[NSClassFromString(@"WKUserScript") alloc] initWithSource:ns(kBridgeScript)
                                                                            injectionTime:WKUserScriptInjectionTimeAtDocumentStart
                                                                         forMainFrameOnly:NO];
        [cfg.userContentController addUserScript:bridge];
        [cfg.userContentController addScriptMessageHandler:m_delegate name:@"fuse"];
        NSView* host = (__bridge NSView*)parent;
        NSRect frame = NSMakeRect(b.x, b.y, b.w, b.h);
        if (host) frame.origin.y = host.isFlipped ? b.y : host.bounds.size.height - b.y - b.h;
        m_view = [[NSClassFromString(@"WKWebView") alloc] initWithFrame:frame configuration:cfg];
        if (!m_view) return false;
        m_view.navigationDelegate = m_delegate;
        [m_view addObserver:m_delegate forKeyPath:@"title" options:NSKeyValueObservingOptionNew context:nullptr];
        if (host) [host addSubview:m_view];
        else {
            m_window = [[NSWindow alloc] initWithContentRect:NSMakeRect(b.x, b.y, b.w, b.h)
                                                   styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable
                                                     backing:NSBackingStoreBuffered
                                                       defer:NO];
            m_window.releasedWhenClosed = NO;
            m_window.contentView = m_view;
            [m_window makeKeyAndOrderFront:nil];
        }
        return true;
    }
    void navigate(std::string_view url) override { [m_view loadRequest:[NSURLRequest requestWithURL:[NSURL URLWithString:ns(url)]]]; }
    void loadHtml(std::string_view html, std::string_view base) override {
        [m_view loadHTMLString:ns(html) baseURL:base.empty() ? nil : [NSURL URLWithString:ns(base)]];
    }
    void reload() override { [m_view reload]; }
    void back() override { [m_view goBack]; }
    void forward() override { [m_view goForward]; }
    void stop() override { [m_view stopLoading]; }
    void evaluate(std::string_view js, std::function<void(bool, std::string)> done) override {
        auto alive = m_alive;
        auto cb = std::make_shared<std::function<void(bool, std::string)>>(std::move(done));
        [m_view evaluateJavaScript:ns(js)
                 completionHandler:^(id result, NSError* error) {
                   if (!*alive || !*cb) return;
                   if (error) (*cb)(false, str(error.localizedDescription));
                   else (*cb)(true, result ? str([NSString stringWithFormat:@"%@", result]) : std::string{});
                 }];
    }
    std::string url() const override { return str(m_view.URL.absoluteString); }
    std::string title() const override { return str(m_view.title); }
    void setBounds(Rect r) override {
        if (m_window) { [m_window setFrame:[m_window frameRectForContentRect:NSMakeRect(r.x, r.y, r.w, r.h)] display:YES]; return; }
        NSView* host = m_view.superview;
        const CGFloat y = host && !host.isFlipped ? host.bounds.size.height - r.y - r.h : r.y;
        m_view.frame = NSMakeRect(r.x, y, r.w, r.h);
    }
    void setVisible(bool v) override {
        if (m_window) v ? [m_window orderFront:nil] : [m_window orderOut:nil];
        else m_view.hidden = !v;
    }
    void setTitle(std::string_view t) override { if (m_window) m_window.title = ns(t); }
    bool poll() override {
        bool any = false;
        while (NSEvent* e = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:nil inMode:NSDefaultRunLoopMode dequeue:YES]) {
            [NSApp sendEvent:e];
            any = true;
        }
        return any;
    }
    void message(const std::string& m) { if (view.onMessage) view.onMessage(m); }
    void titleChanged() { if (view.onTitleChanged) view.onTitleChanged(title()); }
    void finished() { if (view.onLoadFinished) view.onLoadFinished(url()); }

private:
    WKWebView* m_view = nil;
    NSWindow* m_window = nil;
    FuseWkDelegate* m_delegate = nil;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};

bool platformWebViewAvailable() { return loadWebKit(); }
const char* platformWebViewName() { return "wkwebview"; }
std::unique_ptr<WebViewBackend> makePlatformWebView(WebView& owner) { return std::make_unique<WkWebView>(owner); }
} // namespace fuse::ui::web

@implementation FuseWkDelegate
- (void)userContentController:(WKUserContentController*)c didReceiveScriptMessage:(WKScriptMessage*)message {
    if (self.owner) self.owner->message(fuse::ui::web::str([NSString stringWithFormat:@"%@", message.body]));
}
- (void)webView:(WKWebView*)webView didFinishNavigation:(WKNavigation*)navigation {
    if (self.owner) self.owner->finished();
}
- (void)observeValueForKeyPath:(NSString*)keyPath ofObject:(id)object change:(NSDictionary*)change context:(void*)context {
    if (self.owner && [keyPath isEqualToString:@"title"]) self.owner->titleChanged();
}
@end
