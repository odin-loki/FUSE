// WebView front end: forwards to the platform engine backend (webview_*.cpp / .mm).
#include "webview_internal.hpp"

namespace fuse::ui::web {

WebView::WebView() = default;
WebView::~WebView() { destroy(); }
bool WebView::available() { return platformWebViewAvailable(); }
const char* WebView::backendName() { return platformWebViewAvailable() ? platformWebViewName() : "none"; }
bool WebView::create(void* parent, Rect bounds) {
    destroy();
    if (!platformWebViewAvailable()) return false;
    auto b = makePlatformWebView(*this);
    if (!b || !b->create(parent, bounds)) return false;
    m_b = std::move(b);
    return true;
}
void WebView::destroy() { m_b.reset(); }
void WebView::navigate(std::string_view url) { if (m_b) m_b->navigate(url); }
void WebView::loadHtml(std::string_view html, std::string_view baseUrl) { if (m_b) m_b->loadHtml(html, baseUrl); }
void WebView::reload() { if (m_b) m_b->reload(); }
void WebView::back() { if (m_b) m_b->back(); }
void WebView::forward() { if (m_b) m_b->forward(); }
void WebView::stop() { if (m_b) m_b->stop(); }
void WebView::evaluate(std::string_view js, std::function<void(bool, std::string)> done) {
    if (m_b) m_b->evaluate(js, std::move(done));
    else if (done) done(false, "no web engine");
}
std::string WebView::url() const { return m_b ? m_b->url() : std::string{}; }
std::string WebView::title() const { return m_b ? m_b->title() : std::string{}; }
void WebView::setBounds(Rect r) { if (m_b) m_b->setBounds(r); }
void WebView::setVisible(bool v) { if (m_b) m_b->setVisible(v); }
void WebView::setTitle(std::string_view t) { if (m_b) m_b->setTitle(t); }
bool WebView::poll() { return m_b && m_b->poll(); }

} // namespace fuse::ui::web
