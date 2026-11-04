// fuse_app Android backend (plan UI1 / UI5): NativeActivity through the NDK's android_native_app_glue.
//
// android_main (below) extracts the APK assets to the app's files dir, makes it the working directory and runs the
// app's own main() (compiled as fuse_app_main by fuse_app_add_android_app). NativePlatform then pumps the glue's
// looper as the Application's event source: activity commands (window init / term, pause / resume, config, low
// memory) and input (touch, keys, back). The window presents through the shared Vulkan swapchain presenter
// (vk_present.cpp, VK_KHR_android_surface) or, without Vulkan, by copying the CPU raster into ANativeWindow_lock.
#include "native_internal.hpp"
#include "platform_internal.hpp"

#include <android/configuration.h>
#include <android/input.h>
#include <android/keycodes.h>
#include <android/log.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>
#include <jni.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <unistd.h>

#if defined(FUSE_APP_HAS_PLATFORM_LIFECYCLE)
#include <fuse/platform/lifecycle.hpp>
#include <fuse/platform/surface_loss.hpp>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

int fuse_app_main(int argc, char** argv); // the app's main(), renamed by fuse_app_add_android_app

namespace fuse::app {
using namespace fuse::ui;
using detail::SurfaceSource;

#if !defined(FUSE_APP_HAS_VULKAN)
namespace detail {
struct VkCtx {};
struct VkWin {};
VkCtx* vkCtxCreate(SurfaceSource::Kind, std::string& err) { err = "built without Vulkan"; return nullptr; }
VkCtx* vkCtxAdopt(SurfaceSource::Kind, const VulkanDeviceHandles&, std::string& err) { err = "built without Vulkan"; return nullptr; }
void vkCtxDestroy(VkCtx*) {}
bool vkWinSetExternal(VkWin*, unsigned, const ExternalTexture&) { return false; }
const char* vkCtxDeviceName(const VkCtx*) { return ""; }
VkWin* vkWinCreate(VkCtx*, const SurfaceSource&, const NativeOptions&, std::string&) { return nullptr; }
bool vkWinPrepare(VkWin*, unsigned, unsigned, std::string&) { return false; }
bool vkWinPresent(VkWin*, AppWindow&, Rect) { return false; }
bool vkWinGpuDraws(const VkWin*) { return false; }
void vkWinDestroy(VkWin*) {}
bool vkWinReadback(VkWin*, Image&) { return false; }
} // namespace detail
#endif

namespace {
constexpr const char* kTag = "fuse_app";
android_app* g_app = nullptr;

// ---- JNI (attached once on the app thread, detached when android_main ends) ----
JNIEnv* g_env = nullptr;
JNIEnv* jni() {
    if (g_env || !g_app) return g_env;
    JavaVM* vm = g_app->activity->vm;
    if (vm->AttachCurrentThread(&g_env, nullptr) != JNI_OK) g_env = nullptr;
    return g_env;
}
bool jniFailed(JNIEnv* env) {
    if (!env->ExceptionCheck()) return false;
    env->ExceptionClear();
    return true;
}
int sdkLevel() {
    char v[PROP_VALUE_MAX] = {};
    return __system_property_get("ro.build.version.sdk", v) > 0 ? std::atoi(v) : 0;
}

/// KeyCharacterMap.load(device).get(keyCode, meta): the character a key event types (0 = none).
u32 unicodeChar(int deviceId, int keyCode, int meta) {
    JNIEnv* env = jni();
    if (!env) return 0;
    jclass kcm = env->FindClass("android/view/KeyCharacterMap");
    if (!kcm || jniFailed(env)) return 0;
    jmethodID load = env->GetStaticMethodID(kcm, "load", "(I)Landroid/view/KeyCharacterMap;");
    jmethodID get = env->GetMethodID(kcm, "get", "(II)I");
    u32 c = 0;
    if (load && get && !jniFailed(env)) {
        jobject map = env->CallStaticObjectMethod(kcm, load, jint(deviceId));
        if (jniFailed(env) || !map) map = env->CallStaticObjectMethod(kcm, load, jint(-1)); // VIRTUAL_KEYBOARD
        if (!jniFailed(env) && map) {
            const jint v = env->CallIntMethod(map, get, jint(keyCode), jint(meta));
            if (!jniFailed(env) && v > 0) c = u32(v); // < 0: COMBINING_ACCENT (dead key)
            env->DeleteLocalRef(map);
        }
    }
    jniFailed(env);
    env->DeleteLocalRef(kcm);
    return c;
}

/// Window insets of the system bars and display cutouts, in pixels (false when the decor view has none yet).
bool windowInsetsPx(int& l, int& t, int& r, int& b) {
    JNIEnv* env = jni();
    if (!env) return false;
    jobject activity = g_app->activity->clazz;
    jclass actCls = env->GetObjectClass(activity);
    jobject window = env->CallObjectMethod(activity, env->GetMethodID(actCls, "getWindow", "()Landroid/view/Window;"));
    bool ok = false;
    if (!jniFailed(env) && window) {
        jclass winCls = env->GetObjectClass(window);
        jobject decor = env->CallObjectMethod(window, env->GetMethodID(winCls, "getDecorView", "()Landroid/view/View;"));
        if (!jniFailed(env) && decor) {
            jclass viewCls = env->GetObjectClass(decor);
            jobject wi = env->CallObjectMethod(decor, env->GetMethodID(viewCls, "getRootWindowInsets", "()Landroid/view/WindowInsets;"));
            if (!jniFailed(env) && wi) {
                jclass wiCls = env->GetObjectClass(wi);
                if (sdkLevel() >= 30) {
                    jclass typeCls = env->FindClass("android/view/WindowInsets$Type");
                    const jint bars = env->CallStaticIntMethod(typeCls, env->GetStaticMethodID(typeCls, "systemBars", "()I"));
                    const jint cut = env->CallStaticIntMethod(typeCls, env->GetStaticMethodID(typeCls, "displayCutout", "()I"));
                    jobject in = env->CallObjectMethod(wi, env->GetMethodID(wiCls, "getInsets", "(I)Landroid/graphics/Insets;"), bars | cut);
                    if (!jniFailed(env) && in) {
                        jclass inCls = env->GetObjectClass(in);
                        l = env->GetIntField(in, env->GetFieldID(inCls, "left", "I"));
                        t = env->GetIntField(in, env->GetFieldID(inCls, "top", "I"));
                        r = env->GetIntField(in, env->GetFieldID(inCls, "right", "I"));
                        b = env->GetIntField(in, env->GetFieldID(inCls, "bottom", "I"));
                        ok = !jniFailed(env);
                        env->DeleteLocalRef(inCls);
                        env->DeleteLocalRef(in);
                    }
                    if (typeCls) env->DeleteLocalRef(typeCls);
                } else {
                    auto inset = [&](const char* name) { return env->CallIntMethod(wi, env->GetMethodID(wiCls, name, "()I")); };
                    l = inset("getSystemWindowInsetLeft");
                    t = inset("getSystemWindowInsetTop");
                    r = inset("getSystemWindowInsetRight");
                    b = inset("getSystemWindowInsetBottom");
                    ok = !jniFailed(env);
                }
                env->DeleteLocalRef(wiCls);
                env->DeleteLocalRef(wi);
            }
            env->DeleteLocalRef(viewCls);
            env->DeleteLocalRef(decor);
        }
        env->DeleteLocalRef(winCls);
        env->DeleteLocalRef(window);
    }
    jniFailed(env);
    env->DeleteLocalRef(actCls);
    return ok;
}

/// InputMethodManager.showSoftInput / hideSoftInputFromWindow on the decor view (the view the IME serves for a
/// NativeActivity; ANativeActivity_showSoftInput targets its content view, which the IME ignores).
bool imeShow(bool show) {
    JNIEnv* env = jni();
    if (!env) return false;
    jobject activity = g_app->activity->clazz;
    jclass actCls = env->GetObjectClass(activity);
    bool ok = false;
    jstring svc = env->NewStringUTF("input_method");
    jobject imm = env->CallObjectMethod(activity, env->GetMethodID(actCls, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;"), svc);
    jobject window = env->CallObjectMethod(activity, env->GetMethodID(actCls, "getWindow", "()Landroid/view/Window;"));
    if (!jniFailed(env) && imm && window) {
        jclass winCls = env->GetObjectClass(window);
        jobject decor = env->CallObjectMethod(window, env->GetMethodID(winCls, "getDecorView", "()Landroid/view/View;"));
        jclass immCls = env->GetObjectClass(imm);
        if (!jniFailed(env) && decor) {
            if (show) {
                ok = env->CallBooleanMethod(imm, env->GetMethodID(immCls, "showSoftInput", "(Landroid/view/View;I)Z"), decor, 0);
            } else {
                jclass viewCls = env->GetObjectClass(decor);
                jobject token = env->CallObjectMethod(decor, env->GetMethodID(viewCls, "getWindowToken", "()Landroid/os/IBinder;"));
                ok = env->CallBooleanMethod(imm, env->GetMethodID(immCls, "hideSoftInputFromWindow", "(Landroid/os/IBinder;I)Z"), token, 0);
                if (token) env->DeleteLocalRef(token);
                env->DeleteLocalRef(viewCls);
            }
            ok = !jniFailed(env) && ok;
            env->DeleteLocalRef(decor);
        }
        env->DeleteLocalRef(immCls);
        env->DeleteLocalRef(winCls);
    }
    jniFailed(env);
    if (window) env->DeleteLocalRef(window);
    if (imm) env->DeleteLocalRef(imm);
    env->DeleteLocalRef(svc);
    env->DeleteLocalRef(actCls);
    return ok;
}

/// getIntent().getStringExtra("args"): extra command-line arguments (`am start ... --es args "--a=1 --b"`).
std::string intentArgs() {
    JNIEnv* env = jni();
    if (!env) return {};
    jobject activity = g_app->activity->clazz;
    jclass actCls = env->GetObjectClass(activity);
    jobject intent = env->CallObjectMethod(activity, env->GetMethodID(actCls, "getIntent", "()Landroid/content/Intent;"));
    std::string out;
    if (!jniFailed(env) && intent) {
        jclass inCls = env->GetObjectClass(intent);
        jstring key = env->NewStringUTF("args");
        auto s = static_cast<jstring>(env->CallObjectMethod(intent, env->GetMethodID(inCls, "getStringExtra",
                                                                                     "(Ljava/lang/String;)Ljava/lang/String;"), key));
        if (!jniFailed(env) && s) {
            const char* c = env->GetStringUTFChars(s, nullptr);
            out = c ? c : "";
            env->ReleaseStringUTFChars(s, c);
            env->DeleteLocalRef(s);
        }
        env->DeleteLocalRef(key);
        env->DeleteLocalRef(inCls);
        env->DeleteLocalRef(intent);
    }
    jniFailed(env);
    env->DeleteLocalRef(actCls);
    return out;
}

// ---- assets: copied to the files dir once per APK build (fuse_assets.txt lists them, its first line is a build id)
bool readAsset(AAssetManager* am, const char* path, std::string& out) {
    AAsset* a = AAssetManager_open(am, path, AASSET_MODE_STREAMING);
    if (!a) return false;
    out.clear();
    char buf[16384];
    for (int n; (n = AAsset_read(a, buf, sizeof buf)) > 0;) out.append(buf, std::size_t(n));
    AAsset_close(a);
    return true;
}
void makeDirs(const std::string& path) {
    for (std::size_t i = 1; i < path.size(); ++i)
        if (path[i] == '/') ::mkdir(path.substr(0, i).c_str(), 0700);
}
bool writeFile(const std::string& path, const std::string& data) {
    makeDirs(path);
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    return std::fclose(f) == 0 && ok;
}
void extractAssets(android_app* a, const std::string& dir) {
    AAssetManager* am = a->activity->assetManager;
    std::string manifest, have;
    if (!readAsset(am, "fuse_assets.txt", manifest)) return;
    if (std::FILE* f = std::fopen((dir + "/fuse_assets.txt").c_str(), "rb")) {
        char buf[4096];
        for (std::size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) have.append(buf, n);
        std::fclose(f);
    }
    if (have == manifest) return;
    std::size_t files = 0, pos = manifest.find('\n'); // line 1: build id
    std::string data;
    while (pos != std::string::npos && pos + 1 < manifest.size()) {
        const std::size_t end = manifest.find('\n', pos + 1);
        const std::string line = manifest.substr(pos + 1, end == std::string::npos ? std::string::npos : end - pos - 1);
        pos = end;
        if (line.empty()) continue;
        if (readAsset(am, line.c_str(), data) && writeFile(dir + "/" + line, data)) ++files;
        else __android_log_print(ANDROID_LOG_WARN, kTag, "asset %s not extracted", line.c_str());
    }
    writeFile(dir + "/fuse_assets.txt", manifest);
    __android_log_print(ANDROID_LOG_INFO, kTag, "extracted %zu assets to %s", files, dir.c_str());
}

// stdout / stderr -> logcat (no reader thread: funopen write callbacks)
int logWrite(void* prio, const char* buf, int n) {
    static std::string line[2];
    std::string& l = line[prio ? 1 : 0];
    for (int i = 0; i < n; ++i) {
        if (buf[i] == '\n') {
            __android_log_write(prio ? ANDROID_LOG_WARN : ANDROID_LOG_INFO, kTag, l.c_str());
            l.clear();
        } else {
            l += buf[i];
        }
    }
    return n;
}
void redirectStdio() {
    stdout = funopen(nullptr, nullptr, &logWrite, nullptr, nullptr);
    stderr = funopen(reinterpret_cast<void*>(1), nullptr, &logWrite, nullptr, nullptr);
    setvbuf(stdout, nullptr, _IOLBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
}

Key mapKeycode(int code) {
    if (code >= AKEYCODE_A && code <= AKEYCODE_Z) {
        static constexpr Key kLetters[26] = {Key::A, Key::B, Key::C, Key::D, Key::E, Key::F, Key::G, Key::H, Key::I,
                                             Key::J, Key::K, Key::L, Key::M, Key::N, Key::O, Key::P, Key::Q, Key::R,
                                             Key::S, Key::T, Key::U, Key::V, Key::W, Key::X, Key::Y, Key::Z};
        return kLetters[code - AKEYCODE_A];
    }
    if (code >= AKEYCODE_0 && code <= AKEYCODE_9) {
        static constexpr Key kDigits[10] = {Key::Num0, Key::Num1, Key::Num2, Key::Num3, Key::Num4,
                                            Key::Num5, Key::Num6, Key::Num7, Key::Num8, Key::Num9};
        return kDigits[code - AKEYCODE_0];
    }
    switch (code) {
    case AKEYCODE_TAB: return Key::Tab;
    case AKEYCODE_ENTER: case AKEYCODE_NUMPAD_ENTER: return Key::Enter;
    case AKEYCODE_ESCAPE: return Key::Escape;
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
    default: return Key::None;
    }
}
u8 metaMods(int meta) {
    return u8(((meta & AMETA_SHIFT_ON) ? kModShift : 0) | ((meta & AMETA_CTRL_ON) ? kModCtrl : 0) |
              ((meta & AMETA_ALT_ON) ? kModAlt : 0) | ((meta & AMETA_META_ON) ? kModSuper : 0));
}
void appendUtf8(std::string& s, u32 cp) {
    if (cp < 0x80) s += char(cp);
    else if (cp < 0x800) { s += char(0xC0 | (cp >> 6)); s += char(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { s += char(0xE0 | (cp >> 12)); s += char(0x80 | ((cp >> 6) & 0x3F)); s += char(0x80 | (cp & 0x3F)); }
    else { s += char(0xF0 | (cp >> 18)); s += char(0x80 | ((cp >> 12) & 0x3F)); s += char(0x80 | ((cp >> 6) & 0x3F)); s += char(0x80 | (cp & 0x3F)); }
}
} // namespace

struct NativePlatform::Impl {
    Application& app;
    NativeOptions opts;
    NativeHooks hooks{};
    EventSource previous{};
    AppWindow* win = nullptr;          ///< the activity's window (one per activity)
    PresenterKind kind = PresenterKind::Headless;
    detail::VkCtx* vk = nullptr;
    detail::VkWin* vkw = nullptr;
    bool vkTried = false;
    ANativeWindow* window = nullptr;
    std::string error;
    LifecycleStats stats;
    Insets insets;
    unsigned insetPolls = 0;           ///< presents left to re-read insets after a window / config change

    Impl(Application& a, const NativeOptions& o) : app(a), opts(o) {}

    detail::VkCtx* vkContext() {
        if (!vkTried && opts.vulkan) {
            vkTried = true;
            vk = opts.adoptDevice.instance && opts.adoptDevice.device
                     ? detail::vkCtxAdopt(SurfaceSource::Android, opts.adoptDevice, error)
                     : detail::vkCtxCreate(SurfaceSource::Android, error);
            if (!vk) __android_log_print(ANDROID_LOG_WARN, kTag, "no Vulkan: %s", error.c_str());
        }
        return vk;
    }
    f32 osScale() const {
        if (opts.scale > 0.f) return opts.scale;
        const int dpi = g_app && g_app->config ? AConfiguration_getDensity(g_app->config) : 0;
        return dpi > 0 && dpi != ACONFIGURATION_DENSITY_NONE ? f32(dpi) / 160.f : 1.f;
    }
    void updateInsets() {
        if (!win || !window) return;
        int l = 0, t = 0, r = 0, b = 0;
        if (!windowInsetsPx(l, t, r, b)) { // the glue's content rect: the area not covered by system windows
            const ARect& c = g_app->contentRect;
            if (c.right <= c.left) return;
            l = c.left;
            t = c.top;
            r = std::max(0, ANativeWindow_getWidth(window) - c.right);
            b = std::max(0, ANativeWindow_getHeight(window) - c.bottom);
        }
        const f32 s = win->scale();
        const Insets in{f32(l) / s, f32(t) / s, f32(r) / s, f32(b) / s};
        if (in.left == insets.left && in.top == insets.top && in.right == insets.right && in.bottom == insets.bottom) return;
        insets = in;
        __android_log_print(ANDROID_LOG_INFO, kTag, "safe area insets %d %d %d %d px", l, t, r, b);
        if (hooks.insetsChanged) hooks.insetsChanged(hooks.user, *win, insets);
        win->requestRedraw();
    }
    void syncSize() {
        if (!win || !window) return;
        const int w = ANativeWindow_getWidth(window), h = ANativeWindow_getHeight(window);
        const auto px = win->pixelSize();
        if (w > 0 && h > 0 && (u32(w) != u32(px.w) || u32(h) != u32(px.h))) win->resizePixels(u32(w), u32(h));
    }
    /// The activity's window appeared (start, back from background): surface + swapchain for it.
    void attach() {
        if (!win || !g_app->window || window == g_app->window) return;
        window = g_app->window;
        win->setScale(osScale());
        syncSize();
        if (detail::VkCtx* c = vkContext()) {
            SurfaceSource s;
            s.kind = SurfaceSource::Android;
            s.nativeWindow = window;
            vkw = detail::vkWinCreate(c, s, opts, error);
            if (!vkw) __android_log_print(ANDROID_LOG_WARN, kTag, "Vulkan surface failed: %s", error.c_str());
        }
        if (!vkw) ANativeWindow_setBuffersGeometry(window, 0, 0, WINDOW_FORMAT_RGBA_8888);
        kind = vkw ? PresenterKind::Vulkan : PresenterKind::Software;
        ++stats.surfacesCreated;
        stats.hasSurface = true;
        insetPolls = 30;
        updateInsets();
        win->requestRedraw();
        __android_log_print(ANDROID_LOG_INFO, kTag, "surface %u ready: %dx%d scale %.3f (%s)", stats.surfacesCreated,
                            ANativeWindow_getWidth(window), ANativeWindow_getHeight(window), double(win->scale()),
                            vkw ? "vulkan" : "software");
#if defined(FUSE_APP_HAS_PLATFORM_LIFECYCLE)
        fuse::platform::notifySurfaceRestored();
#endif
    }
    /// The window is going away: release the swapchain and surface now (the UI state stays).
    void detach() {
        if (!window) return;
        detail::vkWinDestroy(vkw);
        vkw = nullptr;
        window = nullptr;
        ++stats.surfacesLost;
        stats.hasSurface = false;
        if (win) win->ui().touchCancel();
        __android_log_print(ANDROID_LOG_INFO, kTag, "surface released (%u)", stats.surfacesLost);
#if defined(FUSE_APP_HAS_PLATFORM_LIFECYCLE)
        fuse::platform::notifySurfaceLost();
#endif
    }
    void setForeground(bool fg) {
        if (fg == stats.foreground) return;
        stats.foreground = fg;
        fg ? ++stats.resumes : ++stats.pauses;
        if (hooks.visibility) hooks.visibility(hooks.user, fg);
#if defined(FUSE_APP_HAS_PLATFORM_LIFECYCLE)
        fuse::platform::notifyAppVisibility(fg ? fuse::platform::AppVisibility::Foreground : fuse::platform::AppVisibility::Background);
#endif
        if (fg && win) win->requestRedraw();
    }
    /// The OS theme: night mode from the configuration (FUSE_UI_THEME / NativeOptions::themeQuery still win).
    SystemTheme theme() const {
        if (opts.themeQuery) return opts.themeQuery(opts.themeQueryUser);
        SystemTheme t = detail::queryTheme(opts);
        if (!std::getenv("FUSE_UI_THEME") && g_app && g_app->config) {
            const int32_t night = AConfiguration_getUiModeNight(g_app->config);
            if (night == ACONFIGURATION_UI_MODE_NIGHT_NO) t.dark = false;
            else if (night == ACONFIGURATION_UI_MODE_NIGHT_YES) t.dark = true;
        }
        return t;
    }
    void softKeyboard(bool show) {
        if (!g_app || show == stats.softKeyboard) return;
        stats.softKeyboard = show;
        if (imeShow(show)) return;
        if (show) ANativeActivity_showSoftInput(g_app->activity, ANATIVEACTIVITY_SHOW_SOFT_INPUT_FORCED);
        else ANativeActivity_hideSoftInput(g_app->activity, 0);
    }

    void onCmd(int32_t cmd) {
        switch (cmd) {
        case APP_CMD_INIT_WINDOW: attach(); break;
        case APP_CMD_TERM_WINDOW: detach(); break;
        case APP_CMD_WINDOW_RESIZED:
        case APP_CMD_CONTENT_RECT_CHANGED: syncSize(); insetPolls = 30; updateInsets(); break;
        case APP_CMD_WINDOW_REDRAW_NEEDED: if (win) win->requestRedraw(); break;
        case APP_CMD_CONFIG_CHANGED:
            ++stats.configChanges;
            app.setSystemTheme(theme()); // night mode is a configuration
            if (win && window) win->setScale(osScale());
            syncSize();
            insetPolls = 30;
            break;
        case APP_CMD_PAUSE: setForeground(false); break;
        case APP_CMD_RESUME: setForeground(true); break;
        case APP_CMD_LOW_MEMORY:
            ++stats.lowMemory;
            if (hooks.lowMemory) hooks.lowMemory(hooks.user);
            break;
        case APP_CMD_DESTROY: app.quit(); break;
        default: break;
        }
    }

    int32_t onMotion(const AInputEvent* e) {
        UiContext& ui = win->ui();
        const f32 s = win->scale();
        const int32_t action = AMotionEvent_getAction(e);
        const int32_t masked = action & AMOTION_EVENT_ACTION_MASK;
        const std::size_t changed = std::size_t((action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
        const std::size_t count = AMotionEvent_getPointerCount(e);
        auto pos = [&](std::size_t i) { return Vec2{AMotionEvent_getX(e, i) / s, AMotionEvent_getY(e, i) / s}; };
        ui.setModifiers(metaMods(AMotionEvent_getMetaState(e)));
        if (masked == AMOTION_EVENT_ACTION_SCROLL) { // mouse wheel / trackpad
            ui.wheel(pos(0), {AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_HSCROLL, 0), AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_VSCROLL, 0)});
            return 1;
        }
        if (masked == AMOTION_EVENT_ACTION_HOVER_MOVE) {
            ui.setPointerKind(UiContext::PointerKind::Mouse);
            ui.pointerMove(pos(0));
            return 1;
        }
        // every contact goes to the UiContext touch API: the primary one drives the pointer (tap = click, a vertical
        // drag over a scroll container pans it), the others feed touches() / pinchScale()
        switch (masked) {
        case AMOTION_EVENT_ACTION_DOWN:
        case AMOTION_EVENT_ACTION_POINTER_DOWN: {
            const std::size_t i = masked == AMOTION_EVENT_ACTION_DOWN ? 0 : changed;
            ui.touchDown(u32(AMotionEvent_getPointerId(e, i)), pos(i));
            break;
        }
        case AMOTION_EVENT_ACTION_MOVE:
            for (std::size_t i = 0; i < count; ++i) ui.touchMove(u32(AMotionEvent_getPointerId(e, i)), pos(i));
            break;
        case AMOTION_EVENT_ACTION_UP:
        case AMOTION_EVENT_ACTION_POINTER_UP: {
            const std::size_t i = masked == AMOTION_EVENT_ACTION_UP ? 0 : changed;
            ui.touchUp(u32(AMotionEvent_getPointerId(e, i)), pos(i));
            break;
        }
        case AMOTION_EVENT_ACTION_CANCEL: ui.touchCancel(); break;
        default: break;
        }
        return 1;
    }
    int32_t onKey(const AInputEvent* e) {
        UiContext& ui = win->ui();
        const int32_t action = AKeyEvent_getAction(e), code = AKeyEvent_getKeyCode(e), meta = AKeyEvent_getMetaState(e);
        const u8 mods = metaMods(meta);
        if (code == AKEYCODE_BACK) { // closes a popup / dialog first (Escape), else finishes the activity
            if (action == AKEY_EVENT_ACTION_UP) {
                const bool used = ui.keyDown(Key::Escape, 0);
                ui.keyUp(Key::Escape, 0);
                if (!used) ANativeActivity_finish(g_app->activity);
            }
            return 1;
        }
        const Key k = mapKeycode(code);
        bool handled = false;
        if (action == AKEY_EVENT_ACTION_DOWN) {
            if (k != Key::None) { ui.keyDown(k, mods); handled = true; }
            if (!(mods & (kModCtrl | kModAlt))) {
                const u32 c = unicodeChar(AInputEvent_getDeviceId(e), code, meta);
                if (c >= 0x20 && c != 0x7F) {
                    std::string t;
                    appendUtf8(t, c);
                    ui.textInput(t);
                    handled = true;
                }
            }
        } else if (action == AKEY_EVENT_ACTION_UP && k != Key::None) {
            ui.keyUp(k, mods);
            handled = true;
        }
        return handled ? 1 : 0;
    }

    static NativePlatform::Impl* current;
    static void cmdThunk(android_app*, int32_t cmd) {
        if (current) current->onCmd(cmd);
    }
    static int32_t inputThunk(android_app*, AInputEvent* e) {
        if (!current || !current->win) return 0;
        switch (AInputEvent_getType(e)) {
        case AINPUT_EVENT_TYPE_MOTION: return current->onMotion(e);
        case AINPUT_EVENT_TYPE_KEY: return current->onKey(e);
        default: return 0;
        }
    }
    /// Runs the glue's looper for at most `ms` (-1 = until an event), then drains what is ready.
    static bool pollOnce(int ms) {
        int events = 0;
        android_poll_source* src = nullptr;
        for (int id = ALooper_pollOnce(ms, nullptr, &events, reinterpret_cast<void**>(&src)); id >= 0;
             id = ALooper_pollOnce(0, nullptr, &events, reinterpret_cast<void**>(&src))) {
            if (src) src->process(g_app, src);
            src = nullptr;
            if (g_app->destroyRequested) break;
        }
        return !g_app->destroyRequested;
    }
    static bool pump(void* user, Application&, f32 wait) {
        Impl& m = *static_cast<Impl*>(user);
        if (!g_app) return true;
        const int ms = std::isinf(wait) ? -1 : int(std::min(std::ceil(double(wait) * 1000.0), 2.0e9));
        const bool alive = pollOnce(ms);
        if (m.win && m.window) m.syncSize();
        return alive;
    }
    static bool present(void* user, AppWindow& w, Rect damage) {
        Impl& m = *static_cast<Impl*>(user);
        if (!m.window) return false; // no surface (background): nothing to show; INIT_WINDOW redraws everything
        m.syncSize();
        bool ok = false;
        if (m.vkw) {
            ok = detail::vkWinPresent(m.vkw, w, damage);
        } else {
            const Image& img = w.rasterize(damage);
            // only the redrawn rect: lock() widens it to what the new buffer lacks (img holds the whole frame)
            const Rect rr = w.rasterized();
            ARect dirty{int32_t(rr.x), int32_t(rr.y), int32_t(rr.x + rr.w), int32_t(rr.y + rr.h)};
            ANativeWindow_Buffer buf{};
            if (rr.empty()) {
                ok = true;
            } else if (ANativeWindow_lock(m.window, &buf, &dirty) == 0) {
                const int x0 = std::max(0, int(dirty.left)), y0 = std::max(0, int(dirty.top));
                const int x1 = std::min({int(dirty.right), buf.width, img.width});
                const int y1 = std::min({int(dirty.bottom), buf.height, img.height});
                for (int y = y0; y < y1 && x0 < x1; ++y)
                    std::memcpy(static_cast<u32*>(buf.bits) + std::size_t(y) * std::size_t(buf.stride) + std::size_t(x0),
                                img.pixels.data() + std::size_t(y) * std::size_t(img.width) + std::size_t(x0),
                                std::size_t(x1 - x0) * 4);
                ANativeWindow_unlockAndPost(m.window);
                ok = true;
            }
        }
        if (m.insetPolls) { --m.insetPolls; m.updateInsets(); } // insets arrive after the window, re-read for a while
        if (m.opts.autoSoftKeyboard) m.softKeyboard(w.ui().wantsTextInput());
        return ok;
    }
    static void destroy(void* user) {
        Impl& m = *static_cast<Impl*>(user);
        m.detach();
        m.win = nullptr;
        m.kind = PresenterKind::Headless;
    }
};
NativePlatform::Impl* NativePlatform::Impl::current = nullptr;

NativePlatform::NativePlatform(Application& app, const NativeOptions& o) : m(std::make_unique<Impl>(app, o)) {
    if (!g_app) {
        m->error = "no android_app (not started through android_main)";
        return;
    }
    Impl::current = m.get();
    g_app->onAppCmd = &Impl::cmdThunk;
    g_app->onInputEvent = &Impl::inputThunk;
    app.setEventSource({m.get(), &Impl::pump});
    app.setSystemTheme(m->theme());
}
NativePlatform::~NativePlatform() {
    if (m->win) m->win->setPresenter({}); // calls Impl::destroy: surface released
    detail::vkCtxDestroy(m->vk);
    if (Impl::current == m.get()) {
        Impl::current = nullptr;
        m->app.setEventSource({});
    }
}
bool NativePlatform::available() const { return g_app != nullptr; }
void* NativePlatform::nativeDisplay() const { return g_app; }
void* NativePlatform::androidApp() { return g_app; }

PresenterKind NativePlatform::show(AppWindow& w) {
    if (m->win == &w) return m->kind;
    if (!available() || m->win) return PresenterKind::Headless; // one window per activity
    m->win = &w;
    w.setPresenter({m.get(), &Impl::present, &Impl::destroy});
    // the window usually arrives a little after start: wait for it (bounded) so the presenter kind is known
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!g_app->window && !g_app->destroyRequested && std::chrono::steady_clock::now() < until) Impl::pollOnce(50);
    m->attach();
    return m->kind;
}
PresenterKind NativePlatform::kind(const AppWindow& w) const { return m->win == &w ? m->kind : PresenterKind::Headless; }
std::string_view NativePlatform::gpuName() const { return detail::vkCtxDeviceName(m->vk); }
bool NativePlatform::gpuDraws(const AppWindow& w) const { return m->win == &w && detail::vkWinGpuDraws(m->vkw); }
std::string_view NativePlatform::lastError() const { return m->error; }
bool NativePlatform::setExternalTexture(const AppWindow& w, unsigned slot, const ExternalTexture& tex) {
    return m->win == &w && detail::vkWinSetExternal(m->vkw, slot, tex);
}
bool NativePlatform::capture(const AppWindow& w, Image& out) {
    if (m->win != &w || !m->window) return false;
    if (m->vkw) return detail::vkWinReadback(m->vkw, out);
    out = w.image(); // software: the CPU raster that was posted
    return out.width > 0;
}
unsigned long long NativePlatform::nativeWindow(const AppWindow& w) const {
    return m->win == &w ? reinterpret_cast<unsigned long long>(m->window) : 0;
}
bool NativePlatform::setIcon(const AppWindow&, std::span<const Image>) { return false; } // the launcher icon is in the APK
bool NativePlatform::resizeWindow(const AppWindow&, unsigned, unsigned) { return false; } // the activity decides
std::vector<int> NativePlatform::iconSizes(const AppWindow&) const { return {}; }
// popups are in-window overlays on mobile
std::size_t NativePlatform::popupCount(const AppWindow&) const { return 0; }
unsigned long long NativePlatform::popupWindow(const AppWindow&, std::size_t) const { return 0; }
Rect NativePlatform::popupScreenRect(const AppWindow&, std::size_t) const { return {}; }
bool NativePlatform::capturePopup(const AppWindow&, std::size_t, Image&) { return false; }
Vec2 NativePlatform::screenOrigin(const AppWindow&) const { return {}; }
bool NativePlatform::postClick(unsigned long long, Vec2) { return false; }
Insets NativePlatform::safeAreaInsets(const AppWindow& w) const { return m->win == &w ? m->insets : Insets{}; }
void NativePlatform::setSoftKeyboard(bool show) { m->softKeyboard(show); }
void NativePlatform::setHooks(const NativeHooks& h) { m->hooks = h; }
LifecycleStats NativePlatform::lifecycle() const { return m->stats; }
// Vulkan starts synchronously: a window that ANativeWindow_lock connected as a CPU producer cannot be handed to the
// Vulkan swapchain afterwards, so there is no software first frame to switch from (asyncVulkan is ignored)
bool NativePlatform::presenterPending(const AppWindow&) const { return false; }
PresenterKind NativePlatform::waitForPresenter(const AppWindow& w) { return kind(w); }
bool NativePlatform::setMessageHook(const AppWindow&, const MessageHook&) { return false; }
std::vector<DisplayInfo> NativePlatform::displays() const {
    DisplayInfo d;
    d.name = "android";
    d.primary = true;
    if (m->window) {
        d.bounds = {0.f, 0.f, f32(ANativeWindow_getWidth(m->window)), f32(ANativeWindow_getHeight(m->window))};
        const Insets& in = m->insets;
        const f32 s = m->win ? m->win->scale() : 1.f;
        d.work = {in.left * s, in.top * s, d.bounds.w - (in.left + in.right) * s, d.bounds.h - (in.top + in.bottom) * s};
    }
    d.scale = m->osScale();
    return {d};
}
SystemTheme NativePlatform::querySystemTheme() const { return m->theme(); }
void NativePlatform::refreshSystemTheme() { m->app.setSystemTheme(m->theme()); }
bool NativePlatform::setWindowScale(const AppWindow& w, float scale) {
    if (m->win != &w || !(scale > 0.f)) return false;
    m->win->setScale(scale);
    m->syncSize(); // the activity keeps its pixel size: the logical size follows
    return true;
}
CursorShape NativePlatform::appliedCursor(const AppWindow&) const { return CursorShape::Auto; } // no pointer icon
bool NativePlatform::darkTitleBar(const AppWindow&) const { return false; }
bool NativePlatform::postPointerMove(unsigned long long, Vec2) { return false; }
bool NativePlatform::postClose(unsigned long long nativeWin) {
    if (!m->window || nativeWin != reinterpret_cast<unsigned long long>(m->window)) return false;
    ANativeActivity_finish(g_app->activity);
    return true;
}

} // namespace fuse::app

// ---- entry point -----------------------------------------------------------------------------------------------------
extern "C" void android_main(android_app* app) {
    using namespace fuse::app;
    g_app = app;
    g_env = nullptr;
    redirectStdio();
    const std::string files = app->activity->internalDataPath ? app->activity->internalDataPath : ".";
    extractAssets(app, files);
    if (::chdir(files.c_str()) != 0) __android_log_print(ANDROID_LOG_WARN, kTag, "chdir %s failed", files.c_str());
    ::setenv("HOME", files.c_str(), 1);
    ::setenv("XDG_CONFIG_HOME", (files + "/config").c_str(), 1);
    ::setenv("XDG_RUNTIME_DIR", files.c_str(), 1);
    // argv: the library name, then the intent's "args" extra split at spaces
    std::vector<std::string> words{"fuse_app"};
    const std::string extra = intentArgs();
    for (std::size_t i = 0; i < extra.size();) {
        const std::size_t e = std::min(extra.find(' ', i), extra.size());
        if (e > i) words.push_back(extra.substr(i, e - i));
        i = e + 1;
    }
    std::vector<char*> argv;
    for (std::string& w : words) argv.push_back(w.data());
    argv.push_back(nullptr);
    const int code = fuse_app_main(int(words.size()), argv.data());
    __android_log_print(ANDROID_LOG_INFO, kTag, "main returned %d", code);
    std::fflush(stdout);
    // the glue needs the activity gone before android_main returns
    if (!app->destroyRequested) ANativeActivity_finish(app->activity);
    app->onAppCmd = nullptr;
    app->onInputEvent = nullptr;
    while (!app->destroyRequested) NativePlatform::Impl::pollOnce(-1);
    if (g_env) app->activity->vm->DetachCurrentThread();
    g_env = nullptr;
    g_app = nullptr;
}
