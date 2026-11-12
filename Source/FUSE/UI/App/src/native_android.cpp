// fuse_app native backend for Android (plan UI1 / UI5): a NativeActivity through android_native_app_glue.
//
// The glue runs android_main on its own thread with an ALooper; the Application's event source polls that looper
// (blocking until input, a lifecycle command or the next UI deadline). The activity's ANativeWindow is the one OS
// window: the first AppWindow shown owns it. Presentation is the shared Vulkan swapchain presenter
// (VK_KHR_android_surface) or, without a usable Vulkan device, the CPU raster copied into the window buffer
// (ANativeWindow_lock). Either way the frame is rendered at the UI's logical size (display pixels / scale) and the
// compositor scales it to the window.
//
// Lifecycle (surface_loss / lifecycle of plan §4.13): APP_CMD_TERM_WINDOW drops the surface and the presenter falls
// back to headless until APP_CMD_INIT_WINDOW brings a new one; while the activity is paused the pump blocks, so a
// backgrounded app renders nothing. Input: touch -> pointer (first finger), mouse buttons / hover / wheel, hardware
// and soft keyboard keys (text through KeyEvent.getUnicodeChar over JNI), the Back key as Escape (unconsumed Back
// goes to the system). The soft keyboard follows UiContext::wantsTextInput().
#include "native_internal.hpp"

#include <android/asset_manager.h>
#include <android/configuration.h>
#include <android/input.h>
#include <android/keycodes.h>
#include <android/log.h>
#include <android/looper.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>
#include <jni.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace fuse::app {
using namespace fuse::ui;
using detail::SurfaceSource;

namespace {
android_app* g_app = nullptr;

void logError(const char* what) { __android_log_print(ANDROID_LOG_ERROR, "fuse_app", "%s", what); }

// ---- APK assets ------------------------------------------------------------------------------------------------------
template <class Out>
bool readAsset(std::string_view path, Out& out) {
    if (!g_app || !g_app->activity || !g_app->activity->assetManager) return false;
    while (path.size() > 1 && path[0] == '.' && path[1] == '/') path.remove_prefix(2);
    while (!path.empty() && path[0] == '/') path.remove_prefix(1);
    AAsset* a = AAssetManager_open(g_app->activity->assetManager, std::string(path).c_str(), AASSET_MODE_BUFFER);
    if (!a) return false;
    const off_t n = AAsset_getLength(a);
    out.resize(std::size_t(n));
    const int got = n > 0 ? AAsset_read(a, out.data(), std::size_t(n)) : 0;
    AAsset_close(a);
    return got == n;
}
bool readResourceAsset(void*, std::string_view path, std::string& out) { return readAsset(path, out); }
bool readFontAsset(void*, std::string_view name, std::vector<std::uint8_t>& out) {
    return readAsset("fonts/" + std::string(name), out);
}

// ---- JNI (the android_main thread) ---------------------------------------------------------------------------------
JNIEnv* jniEnv() {
    static thread_local JNIEnv* env = nullptr;
    if (!env && g_app && g_app->activity && g_app->activity->vm) {
        if (g_app->activity->vm->AttachCurrentThread(&env, nullptr) != JNI_OK) env = nullptr;
    }
    return env;
}
bool jniFailed(JNIEnv* env) {
    if (!env->ExceptionCheck()) return false;
    env->ExceptionClear();
    return true;
}
/// KeyEvent(action, code).getUnicodeChar(metaState): the character a key press types (0 = none, dead keys skipped).
u32 unicodeChar(int keyCode, int metaState) {
    JNIEnv* env = jniEnv();
    if (!env) return 0;
    u32 cp = 0;
    if (jclass cls = env->FindClass("android/view/KeyEvent")) {
        const jmethodID ctor = env->GetMethodID(cls, "<init>", "(II)V");
        const jmethodID get = env->GetMethodID(cls, "getUnicodeChar", "(I)I");
        if (ctor && get) {
            if (jobject ev = env->NewObject(cls, ctor, jint(AKEY_EVENT_ACTION_DOWN), jint(keyCode))) {
                const jint c = env->CallIntMethod(ev, get, jint(metaState));
                if (c > 0 && !(u32(c) & 0x80000000u)) cp = u32(c); // bit 31 = COMBINING_ACCENT (dead key)
                env->DeleteLocalRef(ev);
            }
        }
        env->DeleteLocalRef(cls);
    }
    jniFailed(env);
    return cp;
}
/// (ClipboardManager) activity.getSystemService("clipboard"); null on failure. Local ref.
jobject clipboardManager(JNIEnv* env) {
    const jobject activity = g_app->activity->clazz;
    jclass ac = env->GetObjectClass(activity);
    const jmethodID gss = env->GetMethodID(ac, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
    env->DeleteLocalRef(ac);
    if (!gss) return nullptr;
    jstring name = env->NewStringUTF("clipboard");
    jobject cm = env->CallObjectMethod(activity, gss, name);
    env->DeleteLocalRef(name);
    return jniFailed(env) ? nullptr : cm;
}
bool clipboardSet(void*, std::string_view utf8) {
    JNIEnv* env = jniEnv();
    if (!env) return false;
    bool done = false;
    if (jobject cm = clipboardManager(env)) {
        jclass cdc = env->FindClass("android/content/ClipData");
        jclass cmc = env->GetObjectClass(cm);
        const jmethodID plain = cdc ? env->GetStaticMethodID(cdc, "newPlainText",
                                                             "(Ljava/lang/CharSequence;Ljava/lang/CharSequence;)Landroid/content/ClipData;")
                                    : nullptr;
        const jmethodID set = env->GetMethodID(cmc, "setPrimaryClip", "(Landroid/content/ClipData;)V");
        if (plain && set) {
            jstring label = env->NewStringUTF("text");
            jstring text = env->NewStringUTF(std::string(utf8).c_str());
            if (jobject clip = env->CallStaticObjectMethod(cdc, plain, label, text)) {
                env->CallVoidMethod(cm, set, clip);
                done = true;
                env->DeleteLocalRef(clip);
            }
            env->DeleteLocalRef(text);
            env->DeleteLocalRef(label);
        }
        if (cdc) env->DeleteLocalRef(cdc);
        env->DeleteLocalRef(cmc);
        env->DeleteLocalRef(cm);
    }
    return !jniFailed(env) && done;
}
bool clipboardGet(void*, std::string& out) {
    JNIEnv* env = jniEnv();
    if (!env) return false;
    bool done = false;
    if (jobject cm = clipboardManager(env)) {
        jclass cmc = env->GetObjectClass(cm);
        const jmethodID get = env->GetMethodID(cmc, "getPrimaryClip", "()Landroid/content/ClipData;");
        jobject clip = get ? env->CallObjectMethod(cm, get) : nullptr;
        if (clip && !env->ExceptionCheck()) {
            jclass cdc = env->GetObjectClass(clip);
            const jmethodID count = env->GetMethodID(cdc, "getItemCount", "()I");
            const jmethodID itemAt = env->GetMethodID(cdc, "getItemAt", "(I)Landroid/content/ClipData$Item;");
            if (count && itemAt && env->CallIntMethod(clip, count) > 0) {
                if (jobject item = env->CallObjectMethod(clip, itemAt, jint(0))) {
                    jclass ic = env->GetObjectClass(item);
                    const jmethodID coerce = env->GetMethodID(ic, "coerceToText", "(Landroid/content/Context;)Ljava/lang/CharSequence;");
                    jobject cs = coerce ? env->CallObjectMethod(item, coerce, g_app->activity->clazz) : nullptr;
                    if (cs && !env->ExceptionCheck()) {
                        jclass csc = env->GetObjectClass(cs);
                        const jmethodID toString = env->GetMethodID(csc, "toString", "()Ljava/lang/String;");
                        if (jstring js = toString ? static_cast<jstring>(env->CallObjectMethod(cs, toString)) : nullptr) {
                            if (const char* c = env->GetStringUTFChars(js, nullptr)) {
                                out = c;
                                done = true;
                                env->ReleaseStringUTFChars(js, c);
                            }
                            env->DeleteLocalRef(js);
                        }
                        env->DeleteLocalRef(csc);
                        env->DeleteLocalRef(cs);
                    }
                    env->DeleteLocalRef(ic);
                    env->DeleteLocalRef(item);
                }
            }
            env->DeleteLocalRef(cdc);
            env->DeleteLocalRef(clip);
        }
        env->DeleteLocalRef(cmc);
        env->DeleteLocalRef(cm);
    }
    return !jniFailed(env) && done;
}

// ---- keys ------------------------------------------------------------------------------------------------------------
Key mapKey(int code) {
    switch (code) {
    case AKEYCODE_TAB: return Key::Tab;
    case AKEYCODE_ENTER: case AKEYCODE_NUMPAD_ENTER: case AKEYCODE_DPAD_CENTER: return Key::Enter;
    case AKEYCODE_ESCAPE: case AKEYCODE_BACK: return Key::Escape;
    case AKEYCODE_SPACE: return Key::Space;
    case AKEYCODE_DEL: return Key::Backspace;
    case AKEYCODE_FORWARD_DEL: return Key::Delete;
    case AKEYCODE_DPAD_LEFT: return Key::Left;
    case AKEYCODE_DPAD_RIGHT: return Key::Right;
    case AKEYCODE_DPAD_UP: return Key::Up;
    case AKEYCODE_DPAD_DOWN: return Key::Down;
    case AKEYCODE_MOVE_HOME: return Key::Home;
    case AKEYCODE_MOVE_END: return Key::End;
    case AKEYCODE_PAGE_UP: return Key::PageUp;
    case AKEYCODE_PAGE_DOWN: return Key::PageDown;
    case AKEYCODE_LEFT_BRACKET: return Key::BracketLeft;
    case AKEYCODE_RIGHT_BRACKET: return Key::BracketRight;
    default: break;
    }
    static constexpr Key kF[12] = {Key::F1, Key::F2, Key::F3, Key::F4, Key::F5, Key::F6,
                                   Key::F7, Key::F8, Key::F9, Key::F10, Key::F11, Key::F12};
    if (code >= AKEYCODE_F1 && code <= AKEYCODE_F12) return kF[code - AKEYCODE_F1];
    Key k = Key::Other;
    if (code >= AKEYCODE_A && code <= AKEYCODE_Z) detail::asciiKey(k, u32('a' + (code - AKEYCODE_A)));
    else if (code >= AKEYCODE_0 && code <= AKEYCODE_9) detail::asciiKey(k, u32('0' + (code - AKEYCODE_0)));
    return k;
}
u8 mapMods(int meta) {
    return u8(((meta & AMETA_SHIFT_ON) ? kModShift : 0) | ((meta & AMETA_CTRL_ON) ? kModCtrl : 0) |
              ((meta & AMETA_ALT_ON) ? kModAlt : 0) | ((meta & AMETA_META_ON) ? kModSuper : 0));
}
} // namespace

void setAndroidApp(void* androidApp) {
    g_app = static_cast<android_app*>(androidApp);
    if (!g_app || !g_app->activity) return;
    if (const char* data = g_app->activity->internalDataPath) setDataRoot(data);
    setResourceReader({nullptr, &readResourceAsset});
    fuse::text::setFontDataSource({nullptr, &readFontAsset});
}
void* androidApp() { return g_app; }

// ---- platform --------------------------------------------------------------------------------------------------------
struct NativePlatform::Impl {
    struct Rec {
        Impl* impl = nullptr;
        AppWindow* win = nullptr;
        detail::VkWin* vk = nullptr;
        PresenterKind kind = PresenterKind::Headless;
    };

    Application& app;
    NativeOptions opts;
    std::vector<Rec*> recs;
    Rec* owner = nullptr;          ///< the record that has the activity's window
    ANativeWindow* window = nullptr;
    detail::VkCtx* vk = nullptr;
    bool vkTried = false;
    std::string error;
    f32 scale = 1.f;
    int physW = 0, physH = 0;
    bool resumed = true, keyboard = false, touching = false;
    void (*prevCmd)(android_app*, int32_t) = nullptr;
    int32_t (*prevInput)(android_app*, AInputEvent*) = nullptr;
    void* prevUser = nullptr;

    Impl(Application& a, const NativeOptions& o) : app(a), opts(o) {}

    const Rec* find(const AppWindow& w) const {
        for (const Rec* r : recs)
            if (r->win == &w) return r;
        return nullptr;
    }
    detail::VkCtx* vkContext() {
        if (!vkTried && opts.vulkan) {
            vkTried = true;
            vk = opts.adoptDevice.instance && opts.adoptDevice.device
                     ? detail::vkCtxAdopt(SurfaceSource::Android, opts.adoptDevice, error)
                     : detail::vkCtxCreate(SurfaceSource::Android, error);
        }
        return vk;
    }
    f32 displayScale() const {
        if (opts.scale > 0.f) return opts.scale;
        const int32_t d = g_app && g_app->config ? AConfiguration_getDensity(g_app->config) : 0;
        if (d <= 0 || d >= ACONFIGURATION_DENSITY_ANY) return 1.f; // DEFAULT / ANY / NONE
        return std::max(0.5f, f32(d) / 160.f);
    }
    /// (Re)reads the window's pixel size and applies the logical size to the buffers and the AppWindow.
    void measure() {
        if (!window || !owner) return;
        ANativeWindow_setBuffersGeometry(window, 0, 0, 0); // back to the window's own size
        physW = std::max(1, int(ANativeWindow_getWidth(window)));
        physH = std::max(1, int(ANativeWindow_getHeight(window)));
        scale = displayScale();
        const int lw = std::max(1, int(std::lround(f32(physW) / scale))), lh = std::max(1, int(std::lround(f32(physH) / scale)));
        ANativeWindow_setBuffersGeometry(window, lw, lh, owner->kind == PresenterKind::Software ? WINDOW_FORMAT_RGBA_8888 : 0);
        const Size s = owner->win->size();
        if (u32(s.w) != u32(lw) || u32(s.h) != u32(lh)) owner->win->resize(u32(lw), u32(lh));
        owner->win->requestRedraw();
    }
    void attach() {
        if (window || !owner || !g_app || !g_app->window) return;
        window = g_app->window;
        owner->kind = PresenterKind::Headless;
        measure();
        if (detail::VkCtx* c = vkContext()) {
            SurfaceSource s;
            s.kind = SurfaceSource::Android;
            s.nativeWindow = window;
            owner->vk = detail::vkWinCreate(c, s, opts, error);
        }
        owner->kind = owner->vk ? PresenterKind::Vulkan : PresenterKind::Software;
        if (!owner->vk) {
            if (!error.empty()) logError(error.c_str());
            measure(); // the CPU path needs an RGBA_8888 buffer format
        }
    }
    void detach() {
        if (owner) {
            detail::vkWinDestroy(owner->vk);
            owner->vk = nullptr;
            owner->kind = PresenterKind::Headless;
        }
        window = nullptr;
    }
    void syncKeyboard() {
        if (!owner || !g_app || !g_app->activity) return;
        const bool want = owner->win->ui().wantsTextInput();
        if (want == keyboard) return;
        keyboard = want;
        if (want) ANativeActivity_showSoftInput(g_app->activity, ANATIVEACTIVITY_SHOW_SOFT_INPUT_IMPLICIT);
        else ANativeActivity_hideSoftInput(g_app->activity, 0);
    }

    static void onCmd(android_app* a, int32_t cmd);
    static int32_t onInput(android_app* a, AInputEvent* ev);
    static bool present(void* user, AppWindow& w, Rect damage);
    static void destroy(void* user);
    static bool pump(void* user, Application& app, f32 wait);
};

void NativePlatform::Impl::onCmd(android_app* a, int32_t cmd) {
    Impl& m = *static_cast<Impl*>(a->userData);
    switch (cmd) {
    case APP_CMD_INIT_WINDOW: m.attach(); break;
    case APP_CMD_TERM_WINDOW: m.detach(); break;
    case APP_CMD_WINDOW_RESIZED:
    case APP_CMD_CONFIG_CHANGED:
    case APP_CMD_CONTENT_RECT_CHANGED:
    case APP_CMD_WINDOW_REDRAW_NEEDED: m.measure(); break;
    case APP_CMD_RESUME: m.resumed = true; if (m.owner) m.owner->win->requestRedraw(); break;
    case APP_CMD_PAUSE: m.resumed = false; break;
    case APP_CMD_LOST_FOCUS:
        if (m.owner && m.touching) { // the gesture is gone with the focus
            m.touching = false;
            m.owner->win->ui().pointerUp({-1.f, -1.f});
        }
        break;
    case APP_CMD_LOW_MEMORY: break;
    case APP_CMD_DESTROY:
        for (Rec* r : m.recs) r->win->requestClose();
        break;
    default: break;
    }
}

int32_t NativePlatform::Impl::onInput(android_app* a, AInputEvent* ev) {
    Impl& m = *static_cast<Impl*>(a->userData);
    if (!m.owner) return 0;
    AppWindow& w = *m.owner->win;
    UiContext& ui = w.ui();
    const int32_t type = AInputEvent_getType(ev);
    if (type == AINPUT_EVENT_TYPE_MOTION) {
        const int32_t action = AMotionEvent_getAction(ev);
        const int32_t masked = action & AMOTION_EVENT_ACTION_MASK;
        const std::size_t index = std::size_t((action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
        const Vec2 pos{AMotionEvent_getX(ev, 0) / m.scale, AMotionEvent_getY(ev, 0) / m.scale};
        ui.setModifiers(mapMods(AMotionEvent_getMetaState(ev)));
        const bool mouse = (AInputEvent_getSource(ev) & AINPUT_SOURCE_MOUSE) == AINPUT_SOURCE_MOUSE;
        switch (masked) {
        case AMOTION_EVENT_ACTION_DOWN: {
            MouseButton b = MouseButton::Left;
            if (mouse) {
                const int32_t bs = AMotionEvent_getButtonState(ev);
                b = (bs & AMOTION_EVENT_BUTTON_SECONDARY) ? MouseButton::Right
                    : (bs & AMOTION_EVENT_BUTTON_TERTIARY) ? MouseButton::Middle : MouseButton::Left;
            }
            m.touching = true;
            ui.pointerMove(pos);
            ui.pointerDown(pos, b);
            break;
        }
        case AMOTION_EVENT_ACTION_MOVE:
        case AMOTION_EVENT_ACTION_HOVER_MOVE: ui.pointerMove(pos); break;
        case AMOTION_EVENT_ACTION_UP:
        case AMOTION_EVENT_ACTION_CANCEL:
            m.touching = false;
            ui.pointerUp(pos);
            if (!mouse) ui.pointerMove({-1.f, -1.f}); // a lifted finger hovers nothing
            break;
        case AMOTION_EVENT_ACTION_POINTER_UP:
            if (index == 0) { // the tracked finger left while others stay down
                m.touching = false;
                ui.pointerUp(pos);
                ui.pointerMove({-1.f, -1.f});
            }
            break;
        case AMOTION_EVENT_ACTION_SCROLL:
            ui.wheel(pos, {AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_HSCROLL, 0), AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_VSCROLL, 0)});
            break;
        default: return 0;
        }
        w.requestRedraw();
        return 1;
    }
    if (type == AINPUT_EVENT_TYPE_KEY) {
        const int32_t code = AKeyEvent_getKeyCode(ev), action = AKeyEvent_getAction(ev), meta = AKeyEvent_getMetaState(ev);
        if (code == AKEYCODE_VOLUME_UP || code == AKEYCODE_VOLUME_DOWN || code == AKEYCODE_VOLUME_MUTE || code == AKEYCODE_POWER)
            return 0; // system keys
        const Key k = mapKey(code);
        const u8 md = mapMods(meta);
        bool used = false;
        if (k != Key::Other) used = action == AKEY_EVENT_ACTION_DOWN ? ui.keyDown(k, md) : action == AKEY_EVENT_ACTION_UP ? ui.keyUp(k, md) : false;
        if (action == AKEY_EVENT_ACTION_DOWN && !(md & (kModCtrl | kModAlt)) && k != Key::Enter && k != Key::Tab && k != Key::Escape &&
            k != Key::Backspace && k != Key::Delete) {
            if (const u32 cp = unicodeChar(code, meta); cp >= 0x20 && cp != 0x7F) {
                std::string s;
                detail::appendUtf8(s, cp);
                used = ui.textInput(s) || used;
            }
        }
        w.requestRedraw();
        // an unconsumed Back leaves the activity the way the system expects
        return code == AKEYCODE_BACK ? (used ? 1 : 0) : 1;
    }
    return 0;
}

bool NativePlatform::Impl::present(void* user, AppWindow& w, Rect damage) {
    Rec& r = *static_cast<Rec*>(user);
    Impl& m = *r.impl;
    if (&r == m.owner) m.syncKeyboard();
    if (&r != m.owner || !m.window) { // no surface (backgrounded / not the window owner): keep the CPU image current
        w.rasterize(damage);
        return true;
    }
    if (r.kind == PresenterKind::Vulkan) return detail::vkWinPresent(r.vk, w, damage);
    const Image& img = w.rasterize(damage);
    ANativeWindow_Buffer buf{};
    if (ANativeWindow_lock(m.window, &buf, nullptr) != 0) return false;
    const int rows = std::min(int(buf.height), img.height), cols = std::min(int(buf.width), img.width);
    auto* dst = static_cast<std::uint8_t*>(buf.bits);
    for (int y = 0; y < rows; ++y) // Image pixels are 0xAABBGGRR = R, G, B, A bytes = WINDOW_FORMAT_RGBA_8888
        std::memcpy(dst + std::size_t(y) * std::size_t(buf.stride) * 4u, img.pixels.data() + std::size_t(y) * std::size_t(img.width),
                    std::size_t(cols) * 4u);
    ANativeWindow_unlockAndPost(m.window);
    return true;
}

void NativePlatform::Impl::destroy(void* user) {
    Rec* r = static_cast<Rec*>(user);
    Impl& m = *r->impl;
    if (r == m.owner) {
        m.detach();
        m.owner = nullptr;
    }
    std::erase(m.recs, r);
    delete r;
}

bool NativePlatform::Impl::pump(void* user, Application&, f32 wait) {
    Impl& m = *static_cast<Impl*>(user);
    if (!g_app) return true;
    // paused (backgrounded) or without a surface nothing can be shown: block until the system says otherwise
    int timeout = (!m.resumed || !g_app->window) ? -1
                  : wait <= 0.f ? 0 : std::isinf(wait) ? -1 : int(std::min(std::ceil(double(wait) * 1000.0), 2.0e9));
    for (;;) {
        int events = 0;
        android_poll_source* source = nullptr;
        const int id = ALooper_pollOnce(timeout, nullptr, &events, reinterpret_cast<void**>(&source));
        if (id == ALOOPER_POLL_TIMEOUT || id == ALOOPER_POLL_ERROR) break;
        if (id >= 0 && source) source->process(g_app, source);
        if (g_app->destroyRequested) {
            for (Rec* r : m.recs) r->win->requestClose();
            return false;
        }
        timeout = (!m.resumed || !g_app->window) ? -1 : 0; // drain; keep blocking while backgrounded
    }
    return true;
}

NativePlatform::NativePlatform(Application& app, const NativeOptions& o) : m(std::make_unique<Impl>(app, o)) {
    if (!g_app) {
        m->error = "no android_app (call fuse::app::setAndroidApp from android_main)";
        return;
    }
    m->prevUser = g_app->userData;
    m->prevCmd = g_app->onAppCmd;
    m->prevInput = g_app->onInputEvent;
    g_app->userData = m.get();
    g_app->onAppCmd = &Impl::onCmd;
    g_app->onInputEvent = &Impl::onInput;
    app.setEventSource({m.get(), &Impl::pump});
}
NativePlatform::~NativePlatform() {
    while (!m->recs.empty()) m->recs.back()->win->setPresenter({});
    detail::vkCtxDestroy(m->vk);
    if (g_app && g_app->userData == m.get()) {
        g_app->userData = m->prevUser;
        g_app->onAppCmd = m->prevCmd;
        g_app->onInputEvent = m->prevInput;
        m->app.setEventSource({});
    }
}
bool NativePlatform::available() const { return g_app != nullptr; }
void* NativePlatform::nativeDisplay() const { return g_app; }

PresenterKind NativePlatform::show(AppWindow& w) {
    if (const Impl::Rec* existing = m->find(w)) return existing->kind;
    if (!available()) return PresenterKind::Headless;
    auto* r = new Impl::Rec{};
    r->impl = m.get();
    r->win = &w;
    m->recs.push_back(r);
    w.ui().setClipboard({&clipboardSet, &clipboardGet, nullptr});
    w.setPresenter({r, &Impl::present, &Impl::destroy});
    if (!m->owner) {
        m->owner = r;
        // the surface usually arrives a little after start-up: wait for it so show() can report the presenter
        for (int i = 0; i < 200 && !g_app->window && !g_app->destroyRequested; ++i) {
            int events = 0;
            android_poll_source* source = nullptr;
            const int id = ALooper_pollOnce(10, nullptr, &events, reinterpret_cast<void**>(&source));
            if (id >= 0 && source) source->process(g_app, source);
        }
        m->attach();
    }
    return r->kind;
}

PresenterKind NativePlatform::kind(const AppWindow& w) const {
    const Impl::Rec* r = m->find(w);
    return r ? r->kind : PresenterKind::Headless;
}
std::string_view NativePlatform::gpuName() const { return detail::vkCtxDeviceName(m->vk); }
bool NativePlatform::gpuDraws(const AppWindow& w) const {
    const Impl::Rec* r = m->find(w);
    return r && detail::vkWinGpuDraws(r->vk);
}
std::string_view NativePlatform::lastError() const { return m->error; }
bool NativePlatform::setExternalTexture(const AppWindow& w, unsigned slot, const ExternalTexture& tex) {
    const Impl::Rec* r = m->find(w);
    return r && detail::vkWinSetExternal(r->vk, slot, tex);
}
bool NativePlatform::capture(const AppWindow& w, Image& out) {
    const Impl::Rec* r = m->find(w);
    if (!r) return false;
    if (r->vk) return detail::vkWinReadback(r->vk, out);
    out = w.image(); // the window buffer cannot be read back: the frame that was posted to it
    return !out.pixels.empty();
}
unsigned long long NativePlatform::nativeWindow(const AppWindow& w) const {
    const Impl::Rec* r = m->find(w);
    return r && r == m->owner ? static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(m->window)) : 0;
}
Rect NativePlatform::safeArea(const AppWindow& w) const {
    const Size s = w.size();
    const Impl::Rec* r = m->find(w);
    if (!r || r != m->owner || !m->window || !g_app) return {0.f, 0.f, s.w, s.h};
    const ARect c = g_app->contentRect; // display pixels; empty until the first content-rect command
    if (c.right <= c.left || c.bottom <= c.top) return {0.f, 0.f, s.w, s.h};
    return {f32(c.left) / m->scale, f32(c.top) / m->scale, f32(c.right - c.left) / m->scale, f32(c.bottom - c.top) / m->scale};
}
float NativePlatform::scale(const AppWindow& w) const {
    const Impl::Rec* r = m->find(w);
    return r && r == m->owner ? m->scale : 1.f;
}

// One full-screen surface, no window manager: icons, programmatic resizes and OS-window popups do not apply.
bool NativePlatform::setIcon(const AppWindow&, std::span<const Image>) { return false; }
bool NativePlatform::resizeWindow(const AppWindow&, unsigned, unsigned) { return false; }
std::vector<int> NativePlatform::iconSizes(const AppWindow&) const { return {}; }
std::size_t NativePlatform::popupCount(const AppWindow&) const { return 0; }
unsigned long long NativePlatform::popupWindow(const AppWindow&, std::size_t) const { return 0; }
Rect NativePlatform::popupScreenRect(const AppWindow&, std::size_t) const { return {}; }
bool NativePlatform::capturePopup(const AppWindow&, std::size_t, Image&) { return false; }
Vec2 NativePlatform::screenOrigin(const AppWindow&) const { return {}; }
bool NativePlatform::postClick(unsigned long long nativeWin, Vec2 at) {
    // no OS event injection without INJECT_EVENTS: feed the owning window's context directly
    if (!m->owner || !nativeWin || nativeWin != static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(m->window))) return false;
    m->owner->win->click(at);
    return true;
}

namespace detail {
/// android_main body: runs the app's main on the glue thread, then finishes the activity and drains the looper
/// until the system destroys it (the glue must see APP_CMD_DESTROY before android_main returns).
void androidRunMain(android_app* app, int (*appMain)(int, char**)) {
    setAndroidApp(app);
    char arg0[] = "fuse_app";
    char* argv[] = {arg0, nullptr};
    const int rc = appMain(1, argv);
    if (rc != 0) __android_log_print(ANDROID_LOG_ERROR, "fuse_app", "main returned %d", rc);
    if (!app->destroyRequested) ANativeActivity_finish(app->activity);
    while (!app->destroyRequested) {
        int events = 0;
        android_poll_source* source = nullptr;
        const int id = ALooper_pollOnce(-1, nullptr, &events, reinterpret_cast<void**>(&source));
        if (id >= 0 && source) source->process(app, source);
    }
    if (app->activity && app->activity->vm) app->activity->vm->DetachCurrentThread();
    g_app = nullptr;
}
} // namespace detail

} // namespace fuse::app
