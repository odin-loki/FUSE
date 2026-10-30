// Android backend (G4), see fuse/app/android.hpp.
#include <android/asset_manager.h>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include "fuse/app/android.hpp"
#include "android_map.hpp"
#include "native_internal.hpp"

#include <android/log.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>
#include <jni.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace fuse::app {
using namespace fuse::ui;
namespace am = fuse::app::android;

#define FUSE_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "fuse_ui", __VA_ARGS__)

namespace {
struct TextEvent {
    bool commit;
    std::string text;
    int32_t caret;
};
struct A11yNode {
    A11ySnapshotNode n;
    std::string cls;
};
} // namespace

std::mutex g_mu; // guards Impl::text / a11y / clicks and g_impl (Java UI thread vs glue thread)

struct AndroidPlatform::Impl {
    Application& app;
    android_app* state;
    EventSource previous;
    AppWindow* win = nullptr;
    ANativeWindow* surface = nullptr;
    bool imeShown = false;
    u64 frames = 0;
    // Shared with the Java UI thread (JNI natives below).
    std::vector<TextEvent> text;
    std::vector<A11yNode> a11y;
    std::vector<u32> clicks;
    f32 a11yScale = 1.f;

    NativeOptions opts;
    detail::VkCtx* vk = nullptr;
    detail::VkWin* vkWin = nullptr;
    bool vkCtxTried = false, vkFailed = false;
    std::string vkError;

    Impl(Application& a, android_app* s, const NativeOptions& o) : app(a), state(s), opts(o) {}

    /// Swapchain on the current ANativeWindow (lazily, on the first present after APP_CMD_INIT_WINDOW).
    detail::VkWin* vulkan() {
        if (vkWin || vkFailed || !surface || !opts.vulkan) return vkWin;
        if (!vkCtxTried) {
            vkCtxTried = true;
            vk = opts.adoptDevice.instance && opts.adoptDevice.device
                     ? detail::vkCtxAdopt(detail::SurfaceSource::Android, opts.adoptDevice, vkError)
                     : detail::vkCtxCreate(detail::SurfaceSource::Android, vkError);
        }
        if (vk) {
            detail::SurfaceSource src;
            src.kind = detail::SurfaceSource::Android;
            src.surface = surface;
            vkWin = detail::vkWinCreate(vk, src, opts, vkError);
        }
        if (!vkWin) {
            vkFailed = true; // CPU blit from now on (the window may have been locked by nobody yet)
            FUSE_LOGI("fuse_ui: Vulkan present unavailable (%s), CPU blit", vkError.c_str());
            if (surface) ANativeWindow_setBuffersGeometry(surface, 0, 0, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);
        } else {
            FUSE_LOGI("fuse_ui: Vulkan present on %s (%s)", detail::vkCtxDeviceName(vk),
                      detail::vkWinGpuDraws(vkWin) ? "GPU draw" : "CPU upload");
        }
        return vkWin;
    }
    void dropSwapchain() {
        if (vkWin) detail::vkWinDestroy(vkWin);
        vkWin = nullptr;
    }

    // ---- JNI helpers (glue thread) ----
    JNIEnv* env() {
        JNIEnv* e = nullptr;
        if (state->activity->vm->GetEnv(reinterpret_cast<void**>(&e), JNI_VERSION_1_6) == JNI_OK) return e;
        state->activity->vm->AttachCurrentThread(&e, nullptr);
        return e;
    }
    int unicodeOf(AInputEvent* ev) {
        JNIEnv* e = env();
        if (!e) return 0;
        jclass cls = e->FindClass("android/view/KeyEvent");
        if (!cls) { e->ExceptionClear(); return 0; }
        jmethodID ctor = e->GetMethodID(cls, "<init>", "(II)V");
        jmethodID get = e->GetMethodID(cls, "getUnicodeChar", "(I)I");
        jobject ke = e->NewObject(cls, ctor, jint(AKeyEvent_getAction(ev)), jint(AKeyEvent_getKeyCode(ev)));
        const int u = ke ? e->CallIntMethod(ke, get, jint(AKeyEvent_getMetaState(ev))) : 0;
        if (ke) e->DeleteLocalRef(ke);
        e->DeleteLocalRef(cls);
        if (e->ExceptionCheck()) { e->ExceptionClear(); return 0; }
        return u;
    }
    /// FuseActivity.setImeVisible(boolean) when available, else ANativeActivity_*SoftInput.
    void setIme(bool on) {
        if (on == imeShown) return;
        imeShown = on;
        JNIEnv* e = env();
        jclass cls = e ? e->GetObjectClass(state->activity->clazz) : nullptr;
        jmethodID mid = cls ? e->GetMethodID(cls, "setImeVisible", "(Z)V") : nullptr;
        if (e && e->ExceptionCheck()) e->ExceptionClear();
        if (mid) e->CallVoidMethod(state->activity->clazz, mid, jboolean(on));
        else if (on) ANativeActivity_showSoftInput(state->activity, ANATIVEACTIVITY_SHOW_SOFT_INPUT_IMPLICIT);
        else ANativeActivity_hideSoftInput(state->activity, 0);
        if (cls) e->DeleteLocalRef(cls);
    }

    // ---- glue callbacks ----
    static void onCmd(android_app* s, int32_t cmd) {
        Impl& m = *static_cast<Impl*>(s->userData);
        switch (cmd) {
        case APP_CMD_INIT_WINDOW:
        case APP_CMD_WINDOW_RESIZED:
        case APP_CMD_CONFIG_CHANGED:
            if (m.surface != s->window) m.dropSwapchain(); // a new ANativeWindow: new VkSurfaceKHR
            m.surface = s->window;
            if (m.surface) {
                // the CPU blit path wants RGBA8888 buffers; a Vulkan swapchain sets its own format
                if (!m.opts.vulkan || m.vkFailed) ANativeWindow_setBuffersGeometry(m.surface, 0, 0, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);
                if (m.win) {
                    const u32 w = u32(ANativeWindow_getWidth(m.surface)), h = u32(ANativeWindow_getHeight(m.surface));
                    if (u32(m.win->size().w) != w || u32(m.win->size().h) != h) m.win->resize(w, h);
                    m.win->requestRedraw();
                }
            }
            break;
        case APP_CMD_TERM_WINDOW: m.dropSwapchain(); m.surface = nullptr; break; // before the window goes
        case APP_CMD_WINDOW_REDRAW_NEEDED: if (m.win) m.win->requestRedraw(); break;
        case APP_CMD_DESTROY: m.app.quit(0); break;
        default: break;
        }
    }
    static int32_t onInput(android_app* s, AInputEvent* ev) {
        Impl& m = *static_cast<Impl*>(s->userData);
        if (!m.win) return 0;
        UiContext& ui = m.win->ui();
        if (AInputEvent_getType(ev) == AINPUT_EVENT_TYPE_MOTION) {
            const int32_t action = AMotionEvent_getAction(ev) & AMOTION_EVENT_ACTION_MASK;
            const Vec2 p{AMotionEvent_getX(ev, 0), AMotionEvent_getY(ev, 0)};
            ui.setModifiers(am::modsFromMeta(AMotionEvent_getMetaState(ev)));
            switch (action) {
            case AMOTION_EVENT_ACTION_DOWN: {
                const int32_t bs = AMotionEvent_getButtonState(ev);
                ui.pointerMove(p);
                ui.pointerDown(p, bs & AMOTION_EVENT_BUTTON_SECONDARY ? MouseButton::Right
                                  : bs & AMOTION_EVENT_BUTTON_TERTIARY ? MouseButton::Middle : MouseButton::Left);
                break;
            }
            case AMOTION_EVENT_ACTION_MOVE: case AMOTION_EVENT_ACTION_HOVER_MOVE: ui.pointerMove(p); break;
            case AMOTION_EVENT_ACTION_UP: case AMOTION_EVENT_ACTION_CANCEL: ui.pointerUp(p); break;
            case AMOTION_EVENT_ACTION_SCROLL:
                ui.wheel(p, {-AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_HSCROLL, 0),
                             AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_VSCROLL, 0)});
                break;
            default: return 0;
            }
            m.win->requestRedraw();
            return 1;
        }
        if (AInputEvent_getType(ev) == AINPUT_EVENT_TYPE_KEY) {
            const int32_t kc = AKeyEvent_getKeyCode(ev);
            if (kc == AKEYCODE_BACK && !ui.wantsTextInput()) return 0; // let the system finish the activity
            const u8 mods = am::modsFromMeta(AKeyEvent_getMetaState(ev));
            const Key k = am::keyFromKeycode(kc);
            const bool down = AKeyEvent_getAction(ev) == AKEY_EVENT_ACTION_DOWN;
            if (k != Key::Other) down ? ui.keyDown(k, mods) : ui.keyUp(k, mods);
            if (down && !(mods & (kModCtrl | kModAlt | kModSuper))) {
                const int u = m.unicodeOf(ev);
                if (u >= 0x20 && u != 0x7F) {
                    char buf[5] = {};
                    int n = 0;
                    if (u < 0x80) buf[n++] = char(u);
                    else if (u < 0x800) { buf[n++] = char(0xC0 | (u >> 6)); buf[n++] = char(0x80 | (u & 0x3F)); }
                    else { buf[n++] = char(0xE0 | (u >> 12)); buf[n++] = char(0x80 | ((u >> 6) & 0x3F)); buf[n++] = char(0x80 | (u & 0x3F)); }
                    ui.textInput(std::string_view(buf, std::size_t(n)));
                }
            }
            m.win->requestRedraw();
            return 1;
        }
        return 0;
    }

    void drainJava() {
        std::vector<TextEvent> t;
        std::vector<u32> c;
        {
            std::lock_guard lk(g_mu);
            t.swap(text);
            c.swap(clicks);
        }
        if (!win) return;
        UiContext& ui = win->ui();
        for (auto& e : t) {
            if (e.commit) { ui.textComposition({}, 0); if (!e.text.empty()) ui.textInput(e.text); }
            else ui.textComposition(e.text, am::utf8OffsetOfUtf16(e.text.data(), i32(e.text.size()), e.caret));
        }
        for (u32 i : c) { // TalkBack double-tap: click the node's centre; high bits: expand / collapse
            std::lock_guard lk(g_mu);
            if (i & 0xC0000000u) {
                const u32 k = i & 0x3FFFFFFFu;
                if (k < a11y.size()) ui.a11yExpand(a11y[k].n.node, (i & 0x80000000u) != 0);
                continue;
            }
            if (i < a11y.size()) {
                const Rect r = a11y[i].n.bounds;
                const Vec2 p{r.x + r.w / 2, r.y + r.h / 2};
                ui.pointerMove(p);
                ui.pointerDown(p, MouseButton::Left);
                ui.pointerUp(p);
            }
        }
        if (!t.empty() || !c.empty()) win->requestRedraw();
    }
    static bool pump(void* user, Application&, f32 wait) {
        Impl& m = *static_cast<Impl*>(user);
        int events = 0;
        android_poll_source* src = nullptr;
        int timeout = wait >= 1e6f ? -1 : int(wait * 1000.f);
        if (!m.surface && (timeout < 0 || timeout > 250)) timeout = 250; // paused: idle, keep checking for destroy
        while (ALooper_pollOnce(timeout, nullptr, &events, reinterpret_cast<void**>(&src)) >= 0) {
            if (src) src->process(m.state, src);
            timeout = 0;
            if (m.state->destroyRequested) { m.app.quit(0); break; }
        }
        m.drainJava();
        return !m.state->destroyRequested; // false ends Application::step

    }
    static bool present(void* user, AppWindow& aw, Rect damage) {
        Impl& m = *static_cast<Impl*>(user);
        m.setIme(aw.ui().wantsTextInput());
        {
            std::vector<A11ySnapshotNode> snap;
            aw.ui().snapshotA11y(snap);
            std::lock_guard lk(g_mu);
            m.a11y.clear();
            for (auto& n : snap) m.a11y.push_back({n, am::className(n.role)});
        }
        if (detail::VkWin* vw = m.vulkan()) {
            if (!detail::vkWinPresent(vw, aw, damage)) return false;
            if (++m.frames == 1) FUSE_LOGI("fuse_ui: first frame %dx%d (vulkan)", int(aw.size().w), int(aw.size().h));
            return true;
        }
        aw.rasterizeCpu(damage);
        if (!m.surface) return true;
        ANativeWindow_Buffer b;
        if (ANativeWindow_lock(m.surface, &b, nullptr) != 0) return false;
        const Image& img = aw.image();
        const int w = std::min(b.width, img.width), h = std::min(b.height, img.height);
        for (int y = 0; y < h; ++y) // fuse::ui::Image (0xAABBGGRR) is RGBA8888 bytes: straight copy
            std::memcpy(static_cast<u32*>(b.bits) + std::size_t(y) * std::size_t(b.stride),
                        img.pixels.data() + std::size_t(y) * std::size_t(img.width), std::size_t(w) * 4);
        ANativeWindow_unlockAndPost(m.surface);
        if (++m.frames == 1) FUSE_LOGI("fuse_ui: first frame %dx%d", img.width, img.height);
        return true;
    }
};

static AndroidPlatform::Impl* g_impl = nullptr; // for the JNI natives (one activity per process)

AndroidPlatform::AndroidPlatform(Application& app, android_app* state, const NativeOptions& o)
    : m(std::make_unique<Impl>(app, state, o)) {
    state->userData = m.get();
    state->onAppCmd = &Impl::onCmd;
    state->onInputEvent = &Impl::onInput;
    m->previous = app.eventSource();
    app.setEventSource({m.get(), &Impl::pump});
    g_impl = m.get();
}
AndroidPlatform::~AndroidPlatform() {
    {
        std::lock_guard lk(g_mu);
        g_impl = nullptr;
    }
    if (m->win) m->win->setPresenter({});
    m->dropSwapchain();
    detail::vkCtxDestroy(m->vk);
    m->app.setEventSource(m->previous);
    m->state->userData = nullptr;
    m->state->onAppCmd = nullptr;
    m->state->onInputEvent = nullptr;
    m->state->activity->vm->DetachCurrentThread();
}
bool AndroidPlatform::show(AppWindow& w) {
    m->win = &w;
    w.setPresenter({m.get(), &Impl::present, nullptr});
    if (m->surface) {
        w.resize(u32(ANativeWindow_getWidth(m->surface)), u32(ANativeWindow_getHeight(m->surface)));
        w.requestRedraw();
    }
    return true;
}
bool AndroidPlatform::gpuPresents() const { return m->vkWin != nullptr; }
std::string_view AndroidPlatform::gpuName() const { return detail::vkCtxDeviceName(m->vk); }
bool androidInstallFonts(android_app* state) {
    AAssetManager* am = state->activity->assetManager;
    const char* base = state->activity->internalDataPath;
    if (!am || !base) return false;
    const std::string root = std::string(base) + "/fonts";
    bool any = false;
    for (const char* sub : {"opensans", "noto", "twemoji"}) {
        const std::string adir = std::string("fonts/") + sub;
        AAssetDir* d = AAssetManager_openDir(am, adir.c_str());
        if (!d) continue;
        std::filesystem::create_directories(root + "/" + sub);
        while (const char* name = AAssetDir_getNextFileName(d)) {
            const std::string dst = root + "/" + sub + "/" + name;
            AAsset* a = AAssetManager_open(am, (adir + "/" + name).c_str(), AASSET_MODE_STREAMING);
            if (!a) continue;
            std::error_code ec;
            if (std::filesystem::file_size(dst, ec) != std::uintmax_t(AAsset_getLength64(a))) {
                std::ofstream out(dst, std::ios::binary | std::ios::trunc);
                char buf[65536];
                for (int n; (n = AAsset_read(a, buf, sizeof buf)) > 0;) out.write(buf, n);
            }
            AAsset_close(a);
            any = true;
        }
        AAssetDir_close(d);
    }
    if (any) setenv("FUSE_TEXT_FONT_DIR", root.c_str(), 1);
    FUSE_LOGI("fuse_ui: fonts %s -> %s", any ? "installed" : "missing", root.c_str());
    return any;
}

bool AndroidPlatform::hasSurface() const { return m->surface != nullptr; }

} // namespace fuse::app

// ---- JNI natives for org.fuse.ui.FuseActivity (Java UI thread) ----
using fuse::app::g_impl;
using fuse::app::g_mu;
namespace {
std::string utf8(JNIEnv* e, jstring s) {
    if (!s) return {};
    const char* c = e->GetStringUTFChars(s, nullptr); // modified UTF-8: fine for BMP text
    std::string r = c ? c : "";
    if (c) e->ReleaseStringUTFChars(s, c);
    return r;
}
} // namespace
extern "C" {
JNIEXPORT void JNICALL Java_org_fuse_ui_FuseActivity_nativeText(JNIEnv* e, jclass, jboolean commit, jstring s, jint caret) {
    std::string t = utf8(e, s);
    std::lock_guard lk(g_mu);
    if (auto* m = g_impl) {
        m->text.push_back({bool(commit), std::move(t), caret});
        ALooper_wake(m->state->looper);
    }
}
JNIEXPORT jint JNICALL Java_org_fuse_ui_FuseActivity_nativeA11yCount(JNIEnv*, jclass) {
    std::lock_guard lk(g_mu);
    auto* m = g_impl;
    if (!m) return 0;
    return jint(m->a11y.size());
}
/// [parent, focusable, focused, checked, disabled, x, y, w, h, value, lo, hi, protected, expanded, selected, active]
JNIEXPORT jfloatArray JNICALL Java_org_fuse_ui_FuseActivity_nativeA11yInfo(JNIEnv* e, jclass, jint i) {
    float f[16] = {};
    {
        std::lock_guard lk(g_mu);
        auto* m = g_impl;
        if (!m) return nullptr;
        if (i < 0 || std::size_t(i) >= m->a11y.size()) return nullptr;
        const auto& n = m->a11y[std::size_t(i)].n;
        f[0] = n.parent == ~0u ? -1.f : float(n.parent);
        f[1] = n.focusable; f[2] = n.focused; f[3] = n.checked; f[4] = n.disabled;
        f[5] = n.bounds.x; f[6] = n.bounds.y; f[7] = n.bounds.w; f[8] = n.bounds.h;
        f[9] = n.value; f[10] = n.lo; f[11] = n.hi; f[12] = n.protectedText;
        float active = -1.f;
        if (n.state.activeDescendant.valid())
            for (std::size_t k = 0; k < m->a11y.size(); ++k)
                if (m->a11y[k].n.node == n.state.activeDescendant) active = float(k);
        fuse::app::android::a11yStateFloats(n.state, f + 13, active);
    }
    jfloatArray a = e->NewFloatArray(16);
    e->SetFloatArrayRegion(a, 0, 16, f);
    return a;
}
JNIEXPORT jstring JNICALL Java_org_fuse_ui_FuseActivity_nativeA11yString(JNIEnv* e, jclass, jint i, jint which) {
    std::string s;
    {
        std::lock_guard lk(g_mu);
        auto* m = g_impl;
        if (!m) return nullptr;
        if (i < 0 || std::size_t(i) >= m->a11y.size()) return nullptr;
        const auto& an = m->a11y[std::size_t(i)];
        s = which == 0 ? an.n.name : which == 2 ? fuse::app::android::stateDescription(an.n) : an.cls;
    }
    return e->NewStringUTF(s.c_str());
}
JNIEXPORT void JNICALL Java_org_fuse_ui_FuseActivity_nativeA11yExpand(JNIEnv*, jclass, jint i, jboolean open) {
    std::lock_guard lk(g_mu);
    if (auto* m = g_impl; m && i >= 0) {
        m->clicks.push_back(fuse::ui::u32(i) | (open ? 0x80000000u : 0x40000000u));
        ALooper_wake(m->state->looper);
    }
}
JNIEXPORT void JNICALL Java_org_fuse_ui_FuseActivity_nativeA11yClick(JNIEnv*, jclass, jint i) {
    std::lock_guard lk(g_mu);
    if (auto* m = g_impl) {
        m->clicks.push_back(fuse::ui::u32(i));
        ALooper_wake(m->state->looper);
    }
}
}
