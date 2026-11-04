#include "fuse/app/accessibility.hpp"

namespace fuse::app {

namespace a11y = fuse::ui::a11y;

struct WindowAccessibility::Impl {
    Application& app;
    NativePlatform& native;
    AppWindow& win;
    a11y::Bridge bridge;
    EventSource prev{};
    EventSource self{};
    fuse::ui::u64 syncedFrame = ~0ull;
    bool focused = false; ///< last window focus told to the adapter (Linux)
#if FUSE_UI_HAVE_ACCESSKIT && defined(_WIN32)
    std::unique_ptr<a11y::WindowsAdapter> adapter;
#elif FUSE_UI_HAVE_ACCESSKIT && defined(__linux__)
    std::unique_ptr<a11y::UnixAdapter> adapter;
#endif

    Impl(NativePlatform& n, AppWindow& w) : app(w.app()), native(n), win(w), bridge(w.ui()) {}

    bool active() const {
#if FUSE_UI_HAVE_ACCESSKIT && (defined(_WIN32) || defined(__linux__))
        return adapter && adapter->valid();
#else
        return false;
#endif
    }
    /// Push the last frame's changes (only when a frame ran since the last push).
    void push(bool force) {
        if (!force && win.ui().frameIndex() == syncedFrame) return;
        syncedFrame = win.ui().frameIndex();
        bridge.setScale(win.scale()); // a11y bounds in device pixels (the window's DPI scale)
#if FUSE_UI_HAVE_ACCESSKIT && (defined(_WIN32) || defined(__linux__))
        if (adapter) { adapter->update(); return; }
#endif
        bridge.drain();
        bridge.sync();
    }
    static bool pump(void* user, Application& a, fuse::ui::f32 wait) {
        Impl* m = static_cast<Impl*>(user);
        m->push(false);
        const bool go = m->prev.pump ? m->prev.pump(m->prev.user, a, wait) : true;
        if (m->bridge.drain()) m->win.requestRedraw(); // actions from AT threads, before this iteration's frame
#if FUSE_UI_HAVE_ACCESSKIT && defined(__linux__)
        // AT-SPI has no window message to watch: report the X11 focus the event pump tracked
        if (const bool f = m->native.windowFocused(m->win); m->adapter && f != m->focused) {
            m->focused = f;
            m->adapter->setWindowFocused(f);
        }
#endif
        return go;
    }
#if FUSE_UI_HAVE_ACCESSKIT && defined(_WIN32)
    static bool hook(void* user, unsigned msg, std::uintptr_t wp, std::intptr_t lp, std::intptr_t& result) {
        Impl* m = static_cast<Impl*>(user);
        return m->adapter && m->adapter->handleMessage(msg, wp, lp, result);
    }
#endif
};

WindowAccessibility::WindowAccessibility(NativePlatform& native, AppWindow& w) : m(std::make_unique<Impl>(native, w)) {
#if FUSE_UI_HAVE_ACCESSKIT && defined(_WIN32)
    if (void* hwnd = reinterpret_cast<void*>(native.nativeWindow(w))) {
        m->adapter = std::make_unique<a11y::WindowsAdapter>(m->bridge, hwnd);
        native.setMessageHook(w, {m.get(), &Impl::hook});
    }
#elif FUSE_UI_HAVE_ACCESSKIT && defined(__linux__)
    if (native.nativeWindow(w)) m->adapter = std::make_unique<a11y::UnixAdapter>(m->bridge);
#endif
    // AT threads queue actions; without a wake the X11 pump would sleep until the next X event
    m->bridge.setWakeHandler([](void* n) { static_cast<NativePlatform*>(n)->wake(); }, &native);
    m->bridge.sync();
    m->prev = m->app.eventSource();
    m->self = {m.get(), &Impl::pump};
    m->app.setEventSource(m->self);
}

WindowAccessibility::~WindowAccessibility() {
    m->bridge.setWakeHandler(nullptr, nullptr);
    // restore the pump only when it is still ours (a later wrapper or a destroyed NativePlatform replaced it)
    if (m->app.eventSource().user == m.get()) m->app.setEventSource(m->prev);
#if FUSE_UI_HAVE_ACCESSKIT && defined(_WIN32)
    m->native.setMessageHook(m->win, {});
#endif
}

bool WindowAccessibility::active() const { return m->active(); }
a11y::Bridge& WindowAccessibility::bridge() { return m->bridge; }
void WindowAccessibility::update() { m->push(true); }
void WindowAccessibility::setWindowFocused(bool focused) {
#if FUSE_UI_HAVE_ACCESSKIT && (defined(_WIN32) || defined(__linux__))
    if (m->adapter) m->adapter->setWindowFocused(focused);
#else
    (void)focused;
#endif
}

} // namespace fuse::app
