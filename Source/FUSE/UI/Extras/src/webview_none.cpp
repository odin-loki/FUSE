// No system web engine on this target (Android: use the platform WebView from Java/Kotlin).
#if defined(__ANDROID__)
#include "webview_internal.hpp"
namespace fuse::ui::web {
bool platformWebViewAvailable() { return false; }
const char* platformWebViewName() { return "none"; }
std::unique_ptr<WebViewBackend> makePlatformWebView(WebView&) { return nullptr; }
} // namespace fuse::ui::web
#endif
