// WebKitGTK backend: GTK 3 + libwebkit2gtk-4.1 (or -4.0) loaded at runtime through the os seam. A GtkWindow
// holds the WebKitWebView; with a parent X11 window it is an undecorated popup reparented into it (X11 only).
#if !defined(_WIN32) && !defined(__APPLE__) && !defined(__ANDROID__)
#include "webview_internal.hpp"

#include "fuse/ui/os.hpp"

#include <cstdint>
#include <cstring>
#include <vector>

namespace fuse::ui::web {
namespace {
using gboolean = int;
struct GError { u32 domain; int code; char* message; };
using GCallback = void (*)();
using AsyncCb = void (*)(void* source, void* result, void* user);

struct Gtk {
    std::unique_ptr<os::Library> gtk, wk, x11;
    const char* name = "none";
    gboolean (*gtk_init_check)(int*, char***);
    void* (*gtk_window_new)(int);
    void (*gtk_window_set_default_size)(void*, int, int);
    void (*gtk_window_set_title)(void*, const char*);
    void (*gtk_window_move)(void*, int, int);
    void (*gtk_window_resize)(void*, int, int);
    void (*gtk_container_add)(void*, void*);
    void (*gtk_widget_show_all)(void*);
    void (*gtk_widget_hide)(void*);
    void (*gtk_widget_realize)(void*);
    void (*gtk_widget_destroy)(void*);
    void (*gtk_widget_set_size_request)(void*, int, int);
    void* (*gtk_widget_get_window)(void*);
    unsigned long (*gdk_x11_window_get_xid)(void*);
    void* (*gdk_display_get_default)();
    void* (*gdk_x11_display_get_xdisplay)(void*);
    int (*XReparentWindow)(void*, unsigned long, unsigned long, int, int);
    int (*XFlush)(void*);
    unsigned long (*g_signal_connect_data)(void*, const char*, GCallback, void*, void*, int);
    gboolean (*g_main_context_iteration)(void*, gboolean);
    void (*g_free)(void*);
    void (*g_error_free)(GError*);
    void (*g_object_unref)(void*);
    void* (*webkit_web_view_new)();
    void (*webkit_web_view_load_uri)(void*, const char*);
    void (*webkit_web_view_load_html)(void*, const char*, const char*);
    void (*webkit_web_view_reload)(void*);
    void (*webkit_web_view_go_back)(void*);
    void (*webkit_web_view_go_forward)(void*);
    void (*webkit_web_view_stop_loading)(void*);
    const char* (*webkit_web_view_get_uri)(void*);
    const char* (*webkit_web_view_get_title)(void*);
    void* (*webkit_web_view_get_user_content_manager)(void*);
    // >= 2.40
    void (*webkit_web_view_evaluate_javascript)(void*, const char*, long, const char*, const char*, void*, AsyncCb, void*);
    void* (*webkit_web_view_evaluate_javascript_finish)(void*, void*, GError**);
    // < 2.40
    void (*webkit_web_view_run_javascript)(void*, const char*, void*, AsyncCb, void*);
    void* (*webkit_web_view_run_javascript_finish)(void*, void*, GError**);
    void* (*webkit_javascript_result_get_js_value)(void*);
    void (*webkit_javascript_result_unref)(void*);
    char* (*jsc_value_to_string)(void*);
    gboolean (*jsc_value_is_undefined)(void*);
    gboolean (*webkit_user_content_manager_register_script_message_handler)(void*, const char*, const char*);
    void* (*webkit_user_script_new)(const char*, int, int, const char* const*, const char* const*);
    void (*webkit_user_content_manager_add_script)(void*, void*);
    void (*webkit_user_script_unref)(void*);
    bool ok = false;
};

Gtk* gtk() {
    // Never unloaded: GTK / WebKit keep threads and atexit hooks alive past static destruction.
    static Gtk& g = *new Gtk{};
    static bool done = [] {
        if (os::env("DISPLAY").empty() && os::env("WAYLAND_DISPLAY").empty()) return false;
        g.gtk = os::loadLibrary({"libgtk-3.so.0"});
        g.wk = os::loadLibrary({"libwebkit2gtk-4.1.so.0"});
        g.name = "webkitgtk-4.1";
        if (!g.wk) { g.wk = os::loadLibrary({"libwebkit2gtk-4.0.so.37"}); g.name = "webkitgtk-4.0"; }
        if (!g.gtk || !g.wk) return false;
        bool all = true;
        auto L = [&](os::Library& lib, auto& f, const char* n, bool req = true) {
            f = lib.get<std::remove_reference_t<decltype(f)>>(n);
            if (req) all = all && f;
        };
#define G(n) L(*g.gtk, g.n, #n)
#define W(n) L(*g.wk, g.n, #n)
#define WO(n) L(*g.wk, g.n, #n, false)
        G(gtk_init_check); G(gtk_window_new); G(gtk_window_set_default_size); G(gtk_window_set_title); G(gtk_window_move);
        G(gtk_window_resize); G(gtk_container_add); G(gtk_widget_show_all); G(gtk_widget_hide); G(gtk_widget_realize);
        G(gtk_widget_destroy); G(gtk_widget_set_size_request); G(gtk_widget_get_window); G(g_signal_connect_data);
        G(g_main_context_iteration); G(g_free); G(g_error_free); G(g_object_unref);
        L(*g.gtk, g.gdk_x11_window_get_xid, "gdk_x11_window_get_xid", false);
        L(*g.gtk, g.gdk_display_get_default, "gdk_display_get_default", false);
        L(*g.gtk, g.gdk_x11_display_get_xdisplay, "gdk_x11_display_get_xdisplay", false);
        W(webkit_web_view_new); W(webkit_web_view_load_uri); W(webkit_web_view_load_html); W(webkit_web_view_reload);
        W(webkit_web_view_go_back); W(webkit_web_view_go_forward); W(webkit_web_view_stop_loading); W(webkit_web_view_get_uri);
        W(webkit_web_view_get_title); W(webkit_web_view_get_user_content_manager); W(jsc_value_to_string);
        W(jsc_value_is_undefined); W(webkit_user_content_manager_register_script_message_handler); W(webkit_user_script_new);
        W(webkit_user_content_manager_add_script); W(webkit_user_script_unref);
        WO(webkit_web_view_evaluate_javascript); WO(webkit_web_view_evaluate_javascript_finish);
        WO(webkit_web_view_run_javascript); WO(webkit_web_view_run_javascript_finish);
        WO(webkit_javascript_result_get_js_value); WO(webkit_javascript_result_unref);
#undef G
#undef W
#undef WO
        if (!all || !(g.webkit_web_view_evaluate_javascript || g.webkit_web_view_run_javascript)) return false;
        g.x11 = os::loadLibrary({"libX11.so.6"});
        if (g.x11) { L(*g.x11, g.XReparentWindow, "XReparentWindow", false); L(*g.x11, g.XFlush, "XFlush", false); }
        return bool(g.gtk_init_check(nullptr, nullptr));
    }();
    g.ok = done;
    return done ? &g : nullptr;
}

std::string takeString(Gtk& g, void* jsc) {
    if (!jsc || g.jsc_value_is_undefined(jsc)) return {};
    char* s = g.jsc_value_to_string(jsc);
    std::string r = s ? s : "";
    g.g_free(s);
    return r;
}

class GtkWebView final : public WebViewBackend {
public:
    using WebViewBackend::WebViewBackend;
    ~GtkWebView() override {
        *m_alive = false;
        if (m_window) gtk()->gtk_widget_destroy(m_window);
        while (gtk()->g_main_context_iteration(nullptr, 0)) {}
    }
    bool create(void* parent, Rect b) override {
        Gtk& g = *gtk();
        m_popup = parent != nullptr && g.XReparentWindow && g.gdk_x11_window_get_xid;
        m_window = g.gtk_window_new(m_popup ? 1 : 0); // GTK_WINDOW_POPUP is undecorated
        m_view = g.webkit_web_view_new();
        if (!m_window || !m_view) return false;
        g.gtk_window_set_default_size(m_window, int(b.w), int(b.h));
        g.gtk_widget_set_size_request(m_view, int(b.w), int(b.h));
        g.gtk_container_add(m_window, m_view);
        void* ucm = g.webkit_web_view_get_user_content_manager(m_view);
        void* script = g.webkit_user_script_new(kBridgeScript, 0 /*all frames*/, 0 /*document start*/, nullptr, nullptr);
        g.webkit_user_content_manager_add_script(ucm, script);
        g.webkit_user_script_unref(script);
        g.webkit_user_content_manager_register_script_message_handler(ucm, "fuse", nullptr);
        g.g_signal_connect_data(ucm, "script-message-received::fuse", GCallback(&onScriptMessage), this, nullptr, 0);
        g.g_signal_connect_data(m_view, "notify::title", GCallback(&onTitle), this, nullptr, 0);
        g.g_signal_connect_data(m_view, "load-changed", GCallback(&onLoadChanged), this, nullptr, 0);
        g.g_signal_connect_data(m_window, "delete-event", GCallback(&onDelete), this, nullptr, 0);
        if (m_popup) {
            g.gtk_widget_realize(m_window);
            void* dpy = g.gdk_x11_display_get_xdisplay ? g.gdk_x11_display_get_xdisplay(g.gdk_display_get_default()) : nullptr;
            const unsigned long xid = g.gdk_x11_window_get_xid(g.gtk_widget_get_window(m_window));
            if (dpy && xid) {
                g.XReparentWindow(dpy, xid, (unsigned long)(std::uintptr_t)parent, int(b.x), int(b.y));
                g.XFlush(dpy);
            }
        } else {
            g.gtk_window_move(m_window, int(b.x), int(b.y));
        }
        g.gtk_widget_show_all(m_window);
        return true;
    }
    void navigate(std::string_view url) override { gtk()->webkit_web_view_load_uri(m_view, std::string(url).c_str()); }
    void loadHtml(std::string_view html, std::string_view base) override {
        gtk()->webkit_web_view_load_html(m_view, std::string(html).c_str(), base.empty() ? nullptr : std::string(base).c_str());
    }
    void reload() override { gtk()->webkit_web_view_reload(m_view); }
    void back() override { gtk()->webkit_web_view_go_back(m_view); }
    void forward() override { gtk()->webkit_web_view_go_forward(m_view); }
    void stop() override { gtk()->webkit_web_view_stop_loading(m_view); }
    void evaluate(std::string_view js, std::function<void(bool, std::string)> done) override {
        Gtk& g = *gtk();
        auto* p = new Pending{m_alive, std::move(done)};
        const std::string src(js);
        if (g.webkit_web_view_evaluate_javascript)
            g.webkit_web_view_evaluate_javascript(m_view, src.c_str(), long(src.size()), nullptr, nullptr, nullptr, &onEvaluated, p);
        else g.webkit_web_view_run_javascript(m_view, src.c_str(), nullptr, &onEvaluated, p);
    }
    std::string url() const override { const char* u = gtk()->webkit_web_view_get_uri(m_view); return u ? u : ""; }
    std::string title() const override { const char* t = gtk()->webkit_web_view_get_title(m_view); return t ? t : ""; }
    void setBounds(Rect r) override {
        Gtk& g = *gtk();
        g.gtk_widget_set_size_request(m_view, int(r.w), int(r.h));
        g.gtk_window_resize(m_window, int(r.w), int(r.h));
        g.gtk_window_move(m_window, int(r.x), int(r.y));
    }
    void setVisible(bool v) override { v ? gtk()->gtk_widget_show_all(m_window) : gtk()->gtk_widget_hide(m_window); }
    void setTitle(std::string_view t) override { gtk()->gtk_window_set_title(m_window, std::string(t).c_str()); }
    bool poll() override {
        bool any = false;
        for (int i = 0; i < 256 && gtk()->g_main_context_iteration(nullptr, 0); ++i) any = true;
        return any;
    }

private:
    struct Pending { std::shared_ptr<bool> alive; std::function<void(bool, std::string)> done; };
    static void onEvaluated(void* source, void* result, void* user) {
        std::unique_ptr<Pending> p(static_cast<Pending*>(user));
        Gtk& g = *gtk();
        GError* err = nullptr;
        std::string out;
        bool ok = false;
        if (g.webkit_web_view_evaluate_javascript_finish) {
            void* v = g.webkit_web_view_evaluate_javascript_finish(source, result, &err);
            if (v) { ok = true; out = takeString(g, v); g.g_object_unref(v); }
        } else {
            void* r = g.webkit_web_view_run_javascript_finish(source, result, &err);
            if (r) { ok = true; out = takeString(g, g.webkit_javascript_result_get_js_value(r)); g.webkit_javascript_result_unref(r); }
        }
        if (err) { out = err->message ? err->message : "error"; g.g_error_free(err); }
        if (*p->alive && p->done) p->done(ok, out);
    }
    static void onScriptMessage(void*, void* arg, void* user) {
        auto* self = static_cast<GtkWebView*>(user);
        Gtk& g = *gtk();
        // 4.x passes a WebKitJavascriptResult; 6.0 would pass the JSCValue itself
        void* v = g.webkit_javascript_result_get_js_value ? g.webkit_javascript_result_get_js_value(arg) : arg;
        if (self->view.onMessage) self->view.onMessage(takeString(g, v));
    }
    static void onTitle(void*, void*, void* user) {
        auto* self = static_cast<GtkWebView*>(user);
        if (self->view.onTitleChanged) self->view.onTitleChanged(self->title());
    }
    static void onLoadChanged(void*, int event, void* user) {
        auto* self = static_cast<GtkWebView*>(user);
        if (event == 3 /*WEBKIT_LOAD_FINISHED*/ && self->view.onLoadFinished) self->view.onLoadFinished(self->url());
    }
    static gboolean onDelete(void* w, void*, void*) { gtk()->gtk_widget_hide(w); return 1; }

    void* m_window = nullptr;
    void* m_view = nullptr;
    bool m_popup = false;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};
} // namespace

bool platformWebViewAvailable() { return gtk() != nullptr; }
const char* platformWebViewName() { return gtk() ? gtk()->name : "none"; }
std::unique_ptr<WebViewBackend> makePlatformWebView(WebView& owner) { return std::make_unique<GtkWebView>(owner); }

} // namespace fuse::ui::web
#endif
