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
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#if defined(FUSE_APP_HAS_PLATFORM_LIFECYCLE)
#include <fuse/platform/lifecycle.hpp>
#include <fuse/platform/surface_loss.hpp>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
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
bool vkCtxCreateDevice(VkCtx*, std::string&) { return false; }
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
std::string g_savedState; ///< what the previous process saved (the glue frees its copy at the first resume)

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

// ---- the UI (main) thread: views and the IME live there. Jobs reach it through a pipe on its looper, which
// ANativeActivity_onCreate (wrapped below, it runs on the main thread) registers.
ALooper* g_mainLooper = nullptr;
ANativeActivity* g_mainActivity = nullptr;
int g_mainPipe[2] = {-1, -1};
std::mutex g_mainMutex;
std::vector<std::function<void(JNIEnv*)>> g_mainJobs;

int mainJobsCallback(int fd, int, void*) {
    char buf[64];
    while (::read(fd, buf, sizeof buf) > 0) {
    }
    std::vector<std::function<void(JNIEnv*)>> jobs;
    {
        std::lock_guard<std::mutex> lock(g_mainMutex);
        jobs.swap(g_mainJobs);
    }
    JNIEnv* env = g_mainActivity ? g_mainActivity->env : nullptr;
    for (auto& j : jobs)
        if (env) j(env);
    return 1;
}
bool runOnMain(std::function<void(JNIEnv*)> job) {
    if (g_mainPipe[1] < 0) return false;
    {
        std::lock_guard<std::mutex> lock(g_mainMutex);
        g_mainJobs.push_back(std::move(job));
    }
    const char c = 1;
    return ::write(g_mainPipe[1], &c, 1) == 1;
}

void appendUtf16(std::string& out, const jchar* s, std::size_t n);

// ---- IME composition without Java: an invisible EditText (a framework view, created through JNI) takes the IME's
// InputConnection; the main thread polls its Editable (composing span = pre-edit) and hands committed text and the
// pre-edit to the app thread. Keys still reach the native input queue first (NativeActivity takes the input queue).
struct ImeShared {
    std::mutex mutex;
    std::string committed;   ///< text the IME committed since the app thread last took it
    unsigned backspaces = 0; ///< committed characters the IME deleted again
    std::string composing;
    int caret = 0;           ///< byte offset into composing
    bool composingChanged = false;
} g_ime;
struct ImeMain {          // main thread only
    jobject edit = nullptr; ///< global ref
    int timer = -1;
    std::size_t consumed = 0; ///< UTF-16 units of the Editable already delivered as committed text
    std::u16string lastComposing;
    int lastCaret = -1;
} g_imeMain;

jmethodID method(JNIEnv* env, jobject o, const char* name, const char* sig) {
    jclass c = env->GetObjectClass(o);
    jmethodID m = env->GetMethodID(c, name, sig);
    env->DeleteLocalRef(c);
    return m;
}

void imePoll(JNIEnv* env) {
    if (!g_imeMain.edit) return;
    jobject editable = env->CallObjectMethod(g_imeMain.edit, method(env, g_imeMain.edit, "getText", "()Landroid/text/Editable;"));
    if (jniFailed(env) || !editable) return;
    jclass bic = env->FindClass("android/view/inputmethod/BaseInputConnection");
    jclass sel = env->FindClass("android/text/Selection");
    const jint cs = env->CallStaticIntMethod(bic, env->GetStaticMethodID(bic, "getComposingSpanStart", "(Landroid/text/Spannable;)I"), editable);
    const jint ce = env->CallStaticIntMethod(bic, env->GetStaticMethodID(bic, "getComposingSpanEnd", "(Landroid/text/Spannable;)I"), editable);
    const jint cursor = env->CallStaticIntMethod(sel, env->GetStaticMethodID(sel, "getSelectionEnd", "(Ljava/lang/CharSequence;)I"), editable);
    auto str = static_cast<jstring>(env->CallObjectMethod(editable, method(env, editable, "toString", "()Ljava/lang/String;")));
    if (jniFailed(env) || !str) {
        env->DeleteLocalRef(bic);
        env->DeleteLocalRef(sel);
        env->DeleteLocalRef(editable);
        return;
    }
    const jsize len = env->GetStringLength(str);
    const jchar* chars = env->GetStringChars(str, nullptr);
    std::u16string text(reinterpret_cast<const char16_t*>(chars), std::size_t(len));
    env->ReleaseStringChars(str, chars);
    env->DeleteLocalRef(str);
    const bool composing = cs >= 0 && ce > cs && ce <= len;
    const std::size_t commitEnd = composing ? std::size_t(cs) : text.size(); // committed text precedes the pre-edit
    std::string committed;
    unsigned deleted = 0;
    if (commitEnd >= g_imeMain.consumed) {
        appendUtf16(committed, reinterpret_cast<const jchar*>(text.data()) + g_imeMain.consumed, commitEnd - g_imeMain.consumed);
    } else {
        deleted = unsigned(g_imeMain.consumed - commitEnd); // the IME deleted delivered text
    }
    g_imeMain.consumed = commitEnd;
    const std::u16string pre = composing ? text.substr(std::size_t(cs), std::size_t(ce - cs)) : std::u16string();
    const int caret16 = composing ? std::clamp(int(cursor) - int(cs), 0, int(pre.size())) : 0;
    std::string preUtf8, caretPrefix;
    appendUtf16(preUtf8, reinterpret_cast<const jchar*>(pre.data()), pre.size());
    appendUtf16(caretPrefix, reinterpret_cast<const jchar*>(pre.data()), std::size_t(caret16));
    const bool preChanged = pre != g_imeMain.lastComposing || caret16 != g_imeMain.lastCaret;
    g_imeMain.lastComposing = pre;
    g_imeMain.lastCaret = caret16;
    if (!composing && !text.empty()) { // nothing pending: empty the field so it never grows
        env->CallVoidMethod(editable, method(env, editable, "clear", "()V"));
        g_imeMain.consumed = 0;
    }
    jniFailed(env);
    env->DeleteLocalRef(bic);
    env->DeleteLocalRef(sel);
    env->DeleteLocalRef(editable);
    if (committed.empty() && !deleted && !preChanged) return;
    {
        std::lock_guard<std::mutex> lock(g_ime.mutex);
        g_ime.committed += committed;
        g_ime.backspaces += deleted;
        if (preChanged) {
            g_ime.composing = preUtf8;
            g_ime.caret = int(caretPrefix.size());
            g_ime.composingChanged = true;
        }
    }
    if (g_mainActivity && g_mainActivity->instance) ALooper_wake(static_cast<android_app*>(g_mainActivity->instance)->looper);
}
int imeTimerCallback(int fd, int, void*) {
    std::uint64_t n = 0;
    while (::read(fd, &n, sizeof n) > 0) {
    }
    if (g_mainActivity) imePoll(g_mainActivity->env);
    return 1;
}

/// Main thread: what an IME does through the EditText's InputConnection (imeTestInput).
void imeInject(JNIEnv* env, const std::string& utf8, bool commit) {
    if (!g_imeMain.edit) return;
    jobject et = g_imeMain.edit;
    jclass eiCls = env->FindClass("android/view/inputmethod/EditorInfo");
    jobject ei = env->NewObject(eiCls, env->GetMethodID(eiCls, "<init>", "()V"));
    jobject ic = env->CallObjectMethod(et, method(env, et, "onCreateInputConnection",
                                                  "(Landroid/view/inputmethod/EditorInfo;)Landroid/view/inputmethod/InputConnection;"), ei);
    if (!jniFailed(env) && ic) {
        jstring s = env->NewStringUTF(utf8.c_str());
        env->CallBooleanMethod(ic, method(env, ic, commit ? "commitText" : "setComposingText", "(Ljava/lang/CharSequence;I)Z"), s, jint(1));
        jniFailed(env);
        env->DeleteLocalRef(s);
        env->DeleteLocalRef(ic);
    }
    jniFailed(env);
    env->DeleteLocalRef(ei);
    env->DeleteLocalRef(eiCls);
    imePoll(env);
}

/// Main thread: shows (creating it on first use) or hides the IME on the invisible EditText.
bool imeEditShow(JNIEnv* env, bool show) {
    jobject activity = g_mainActivity->clazz;
    if (!g_imeMain.edit && show) {
        jclass etCls = env->FindClass("android/widget/EditText");
        jobject et = env->NewObject(etCls, env->GetMethodID(etCls, "<init>", "(Landroid/content/Context;)V"), activity);
        if (jniFailed(env) || !et) return false;
        env->CallVoidMethod(et, method(env, et, "setInputType", "(I)V"), jint(0x8001)); // TYPE_CLASS_TEXT | TYPE_TEXT_FLAG_AUTO_CORRECT
        // IME_ACTION_NONE | IME_FLAG_NO_FULLSCREEN | IME_FLAG_NO_EXTRACT_UI
        env->CallVoidMethod(et, method(env, et, "setImeOptions", "(I)V"), jint(0x1 | 0x2000000 | 0x10000000));
        env->CallVoidMethod(et, method(env, et, "setAlpha", "(F)V"), jfloat(0.f));
        env->CallVoidMethod(et, method(env, et, "setFocusableInTouchMode", "(Z)V"), jboolean(true));
        jclass lpCls = env->FindClass("android/view/ViewGroup$LayoutParams");
        jobject lp = env->NewObject(lpCls, env->GetMethodID(lpCls, "<init>", "(II)V"), jint(1), jint(1));
        env->CallVoidMethod(activity, method(env, activity, "addContentView", "(Landroid/view/View;Landroid/view/ViewGroup$LayoutParams;)V"), et, lp);
        if (jniFailed(env)) return false;
        g_imeMain.edit = env->NewGlobalRef(et);
        env->DeleteLocalRef(lp);
        env->DeleteLocalRef(lpCls);
        env->DeleteLocalRef(et);
        env->DeleteLocalRef(etCls);
    }
    if (!g_imeMain.edit) return !show;
    jobject et = g_imeMain.edit;
    jstring svc = env->NewStringUTF("input_method");
    jobject imm = env->CallObjectMethod(activity, method(env, activity, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;"), svc);
    bool ok = false;
    if (!jniFailed(env) && imm) {
        if (show) {
            env->CallBooleanMethod(et, method(env, et, "requestFocus", "()Z"));
            ok = env->CallBooleanMethod(imm, method(env, imm, "showSoftInput", "(Landroid/view/View;I)Z"), et, 0);
        } else {
            jobject token = env->CallObjectMethod(et, method(env, et, "getWindowToken", "()Landroid/os/IBinder;"));
            ok = env->CallBooleanMethod(imm, method(env, imm, "hideSoftInputFromWindow", "(Landroid/os/IBinder;I)Z"), token, 0);
            if (token) env->DeleteLocalRef(token);
            env->CallVoidMethod(et, method(env, et, "clearFocus", "()V"));
        }
        ok = !jniFailed(env) && ok;
        env->DeleteLocalRef(imm);
    }
    jniFailed(env);
    env->DeleteLocalRef(svc);
    if (!show) { // drop what is left (an unfinished pre-edit ends with the session)
        jobject editable = env->CallObjectMethod(et, method(env, et, "getText", "()Landroid/text/Editable;"));
        if (!jniFailed(env) && editable) {
            env->CallVoidMethod(editable, method(env, editable, "clear", "()V"));
            env->DeleteLocalRef(editable);
        }
        jniFailed(env);
        g_imeMain.consumed = 0;
        g_imeMain.lastComposing.clear();
    }
    // poll the Editable at ~30 Hz while the IME is up
    if (show && g_imeMain.timer < 0) {
        g_imeMain.timer = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        if (g_imeMain.timer >= 0) {
            itimerspec its{};
            its.it_interval.tv_nsec = its.it_value.tv_nsec = 33 * 1000 * 1000;
            timerfd_settime(g_imeMain.timer, 0, &its, nullptr);
            ALooper_addFd(g_mainLooper, g_imeMain.timer, ALOOPER_POLL_CALLBACK, ALOOPER_EVENT_INPUT, &imeTimerCallback, nullptr);
        }
    } else if (!show && g_imeMain.timer >= 0) {
        ALooper_removeFd(g_mainLooper, g_imeMain.timer);
        ::close(g_imeMain.timer);
        g_imeMain.timer = -1;
    }
    return ok;
}

/// getIntent().getStringExtra(name): `am start ... --es args "--a=1 --b"` (extra argv words), `--es env "A=1 B=2"`.
std::string intentExtra(const char* name) {
    JNIEnv* env = jni();
    if (!env) return {};
    jobject activity = g_app->activity->clazz;
    jclass actCls = env->GetObjectClass(activity);
    jobject intent = env->CallObjectMethod(activity, env->GetMethodID(actCls, "getIntent", "()Landroid/content/Intent;"));
    std::string out;
    if (!jniFailed(env) && intent) {
        jclass inCls = env->GetObjectClass(intent);
        jstring key = env->NewStringUTF(name);
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

/// Milliseconds since the zygote forked this process (/proc/self/stat start time, CLOCK_BOOTTIME).
double msSinceProcessStart() {
    std::FILE* f = std::fopen("/proc/self/stat", "rb");
    if (!f) return -1.0;
    char buf[1024];
    const std::size_t n = std::fread(buf, 1, sizeof buf - 1, f);
    std::fclose(f);
    buf[n] = 0;
    const char* p = std::strrchr(buf, ')');
    if (!p) return -1.0;
    p += 2;
    for (int field = 3; field < 22 && p; ++field) {
        p = std::strchr(p, ' ');
        if (p) ++p;
    }
    if (!p) return -1.0;
    const double ticks = std::strtod(p, nullptr);
    timespec ts{};
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return (double(ts.tv_sec) + double(ts.tv_nsec) * 1e-9 - ticks / double(sysconf(_SC_CLK_TCK))) * 1000.0;
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

/// Ends the CPU producer connection that ANativeWindow_lock made, so a Vulkan swapchain can connect to the same
/// window. The NDK has no call for it: this is native_window_api_disconnect (NATIVE_WINDOW_API_DISCONNECT with
/// NATIVE_WINDOW_API_CPU) through the window's `perform` hook, after checking the struct's magic.
bool disconnectCpuProducer(ANativeWindow* w) {
    struct Base { // android_native_base_t
        int magic, version;
        void* reserved[4];
        void (*incRef)(void*);
        void (*decRef)(void*);
    };
    struct Window { // the leading part of struct ANativeWindow (system/window.h)
        Base common;
        uint32_t flags;
        int minSwapInterval, maxSwapInterval;
        float xdpi, ydpi;
        intptr_t oem[4];
        int (*setSwapInterval)(void*, int);
        void* dequeueBufferDeprecated;
        void* lockBufferDeprecated;
        void* queueBufferDeprecated;
        int (*query)(const void*, int, int*);
        int (*perform)(void*, int, ...);
    };
    constexpr int kMagic = ('_' << 24) | ('w' << 16) | ('n' << 8) | 'd';
    constexpr int kApiDisconnect = 14, kApiCpu = 2;
    auto* n = reinterpret_cast<Window*>(w);
    if (!w || n->common.magic != kMagic || !n->perform) return false;
    return n->perform(w, kApiDisconnect, kApiCpu) == 0;
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
void appendUtf16(std::string& out, const jchar* s, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        u32 c = s[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < n && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000)
            c = 0x10000 + ((c - 0xD800) << 10) + (u32(s[++i]) - 0xDC00);
        appendUtf8(out, c);
    }
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
    bool cpuConnected = false;         ///< ANativeWindow_lock connected `window` as a CPU producer
    // asyncVulkan: a worker creates instance, device and surface while the window shows software frames; the UI
    // thread then disconnects the CPU producer and creates the swapchain
    std::thread worker;
    std::atomic<bool> vkDone{false};
    bool vkPending = false;
    bool vkDeferred = false;           ///< pending, the worker starts after the first software frame
    detail::VkCtx* vkCtxNew = nullptr;
    std::string vkErr;

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
    bool wantsAsyncVk() const {
        return !vkTried && !vkPending && opts.vulkan && opts.asyncVulkan && !(opts.adoptDevice.instance && opts.adoptDevice.device);
    }
    /// The worker starts once the first software frame is posted: on a few cores (or an emulator, where the
    /// driver and the window buffers share the host connection) it would otherwise delay that frame.
    void deferVkAsync() {
        vkPending = true;
        vkDeferred = true;
    }
    void startVkAsync() {
        vkPending = true;
        vkDeferred = false;
        vkDone.store(false, std::memory_order_relaxed);
        // instance + device only: Android connects the window when its VkSurfaceKHR is created, which fails while
        // ANativeWindow_lock holds it as a CPU producer
        worker = std::thread([this] {
            std::string err;
            detail::VkCtx* c = detail::vkCtxCreate(SurfaceSource::Android, err);
            if (c && !detail::vkCtxCreateDevice(c, err)) {
                detail::vkCtxDestroy(c);
                c = nullptr;
            }
            vkCtxNew = c;
            vkErr = std::move(err);
            vkDone.store(true, std::memory_order_release);
        });
    }
    /// Takes over the worker's device. `use`: switch the current window to Vulkan (disconnect the CPU producer,
    /// then surface + swapchain); otherwise (the window is going away) the next window gets it.
    void finishVk(bool use) {
        if (!worker.joinable()) return;
        worker.join();
        vkPending = false;
        vkTried = true;
        vk = vkCtxNew;
        vkCtxNew = nullptr;
        if (!vk) {
            error = vkErr;
            __android_log_print(ANDROID_LOG_WARN, kTag, "no Vulkan: %s", error.c_str());
            return;
        }
        if (!use || !window || !win) return;
        if (cpuConnected && !disconnectCpuProducer(window)) {
            __android_log_print(ANDROID_LOG_WARN, kTag, "cannot disconnect the CPU producer: software until the next surface");
            return;
        }
        cpuConnected = false;
        SurfaceSource s;
        s.kind = SurfaceSource::Android;
        s.nativeWindow = window;
        vkw = detail::vkWinCreate(vk, s, opts, error);
        if (!vkw) {
            __android_log_print(ANDROID_LOG_WARN, kTag, "Vulkan surface failed: %s", error.c_str());
            ANativeWindow_setBuffersGeometry(window, 0, 0, WINDOW_FORMAT_RGBA_8888); // back to the CPU producer
            return;
        }
        std::string e;
        const auto px = win->pixelSize();
        if (!detail::vkWinPrepare(vkw, std::max(1u, px.w), std::max(1u, px.h), e))
            __android_log_print(ANDROID_LOG_WARN, kTag, "swapchain: %s", e.c_str()); // retried by the present
        kind = PresenterKind::Vulkan;
        win->requestRedraw();
        __android_log_print(ANDROID_LOG_INFO, kTag, "switched to the Vulkan presenter");
    }
    /// The activity's window appeared (start, back from background): surface + swapchain for it.
    void attach() {
        if (!win || !g_app->window || window == g_app->window) return;
        window = g_app->window;
        cpuConnected = false;
        win->setScale(osScale());
        syncSize();
        if (wantsAsyncVk()) deferVkAsync(); // software frames now, Vulkan when the worker is done
        if (detail::VkCtx* c = vkPending ? nullptr : vkContext()) {
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
                            vkw ? "vulkan" : vkPending ? "software, Vulkan starting" : "software");
#if defined(FUSE_APP_HAS_PLATFORM_LIFECYCLE)
        fuse::platform::notifySurfaceRestored();
#endif
    }
    /// The window is going away: release the swapchain and surface now (the UI state stays).
    void detach() {
        if (!window) return;
        finishVk(false); // the next window gets the worker's device
        if (vkDeferred) vkPending = vkDeferred = false; // never started: the next window starts it
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
        if (opts.imeComposition && runOnMain([show](JNIEnv* env) { imeEditShow(env, show); })) return;
        if (imeShow(show)) return;
        if (show) ANativeActivity_showSoftInput(g_app->activity, ANATIVEACTIVITY_SHOW_SOFT_INPUT_FORCED);
        else ANativeActivity_hideSoftInput(g_app->activity, 0);
    }

    /// What the IME did since the last pump: backspaces, committed text, the pre-edit.
    void takeIme() {
        std::string text, pre;
        unsigned back = 0;
        int caret = 0;
        bool preChanged = false;
        {
            std::lock_guard<std::mutex> lock(g_ime.mutex);
            text.swap(g_ime.committed);
            back = g_ime.backspaces;
            g_ime.backspaces = 0;
            preChanged = g_ime.composingChanged;
            g_ime.composingChanged = false;
            pre = g_ime.composing;
            caret = g_ime.caret;
        }
        UiContext& ui = win->ui();
        for (unsigned i = 0; i < back; ++i) {
            ui.keyDown(Key::Backspace, 0);
            ui.keyUp(Key::Backspace, 0);
        }
        if (!text.empty()) {
            if (!ui.composition().empty()) ui.textComposition({}, 0); // the commit replaces the pre-edit
            ui.textInput(text);
        }
        if (preChanged || (!text.empty() && !pre.empty())) ui.textComposition(pre, caret);
        if (back || !text.empty() || preChanged) win->requestRedraw();
    }

    /// The OS may kill the process once the activity is stopped: hand it the app's state (the glue frees it).
    void saveState() {
        const std::string s = app.saveState();
        void* p = s.empty() ? nullptr : std::malloc(s.size());
        if (p) std::memcpy(p, s.data(), s.size());
        g_app->savedState = p;
        g_app->savedStateSize = p ? s.size() : 0;
        ++stats.statesSaved;
        __android_log_print(ANDROID_LOG_INFO, kTag, "state saved (%zu bytes)", s.size());
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
        case APP_CMD_SAVE_STATE: saveState(); break;
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
        const bool alive = pollOnce(m.vkPending && !m.vkDeferred ? std::min(ms < 0 ? 4 : ms, 4) : ms); // look for the worker soon
        if (m.vkPending && m.vkDone.load(std::memory_order_acquire)) m.finishVk(true);
        if (m.win) m.takeIme();
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
            using Clock = std::chrono::steady_clock;
            const Clock::time_point t0 = Clock::now();
            const Image& img = w.rasterize(damage);
            // only the redrawn rect: lock() widens it to what the new buffer lacks (img holds the whole frame)
            const Rect rr = w.rasterized();
            ARect dirty{int32_t(rr.x), int32_t(rr.y), int32_t(rr.x + rr.w), int32_t(rr.y + rr.h)};
            const Clock::time_point t1 = Clock::now();
            ANativeWindow_Buffer buf{};
            if (rr.empty()) {
                ok = true;
            } else if (ANativeWindow_lock(m.window, &buf, &dirty) == 0) {
                const Clock::time_point t2 = Clock::now();
                const int x0 = std::max(0, int(dirty.left)), y0 = std::max(0, int(dirty.top));
                const int x1 = std::min({int(dirty.right), buf.width, img.width});
                const int y1 = std::min({int(dirty.bottom), buf.height, img.height});
                for (int y = y0; y < y1 && x0 < x1; ++y)
                    std::memcpy(static_cast<u32*>(buf.bits) + std::size_t(y) * std::size_t(buf.stride) + std::size_t(x0),
                                img.pixels.data() + std::size_t(y) * std::size_t(img.width) + std::size_t(x0),
                                std::size_t(x1 - x0) * 4);
                ANativeWindow_unlockAndPost(m.window);
                static const bool trace = std::getenv("FUSE_APP_VK_TRACE") != nullptr;
                if (trace && !m.cpuConnected) { // the first buffer of a surface: connect + allocation in the lock
                    auto ms = [](Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
                    std::fprintf(stderr, "fuse_app: software present: raster %.1f ms, lock %.1f ms, copy + post %.1f ms\n",
                                 ms(t0, t1), ms(t1, t2), ms(t2, Clock::now()));
                }
                m.cpuConnected = true;
                ok = true;
                if (m.vkDeferred) m.startVkAsync();
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
    m->finishVk(false);
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
    if (!g_savedState.empty()) { // the window is sized: scroll offsets and focus land where they were
        __android_log_print(ANDROID_LOG_INFO, kTag, "restoring saved state (%zu bytes)", g_savedState.size());
        m->stats.stateRestored = true;
        m->app.restoreState(g_savedState);
        g_savedState.clear();
    }
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
std::string defaultCacheDir() { // the app's private files dir (internalDataPath) + /cache
    const char* base = g_app && g_app->activity ? g_app->activity->internalDataPath : nullptr;
    return base && *base ? std::string(base) + "/cache" : std::string();
}
// popups are in-window overlays on mobile
std::size_t NativePlatform::popupCount(const AppWindow&) const { return 0; }
unsigned long long NativePlatform::popupWindow(const AppWindow&, std::size_t) const { return 0; }
Rect NativePlatform::popupScreenRect(const AppWindow&, std::size_t) const { return {}; }
bool NativePlatform::capturePopup(const AppWindow&, std::size_t, Image&) { return false; }
Vec2 NativePlatform::screenOrigin(const AppWindow&) const { return {}; }
bool NativePlatform::postClick(unsigned long long, Vec2) { return false; }
Insets NativePlatform::safeAreaInsets(const AppWindow& w) const { return m->win == &w ? m->insets : Insets{}; }
void NativePlatform::setSoftKeyboard(bool show) { m->softKeyboard(show); }
bool NativePlatform::imeTestInput(std::string_view utf8, bool commit) {
    if (!m->opts.imeComposition || !m->stats.softKeyboard) return false;
    return runOnMain([s = std::string(utf8), commit](JNIEnv* env) { imeInject(env, s, commit); });
}
void NativePlatform::setHooks(const NativeHooks& h) { m->hooks = h; }
LifecycleStats NativePlatform::lifecycle() const { return m->stats; }
// asyncVulkan: software frames (ANativeWindow_lock) until the worker has the device; the switch disconnects the CPU
// producer and creates the swapchain on the same window
bool NativePlatform::presenterPending(const AppWindow& w) const { return m->win == &w && m->vkPending; }
PresenterKind NativePlatform::waitForPresenter(const AppWindow& w) {
    if (m->win != &w) return PresenterKind::Headless;
    if (m->vkDeferred) m->startVkAsync();
    while (m->vkPending && !m->vkDone.load(std::memory_order_acquire) && !g_app->destroyRequested) Impl::pollOnce(4);
    if (m->vkPending) m->finishVk(true);
    return m->kind;
}
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
bool NativePlatform::postPointerButton(unsigned long long, Vec2, bool) { return false; }
std::size_t NativePlatform::toolWindowCount(const AppWindow&) const { return 0; }
unsigned long long NativePlatform::toolWindow(const AppWindow&, std::size_t) const { return 0; }
Rect NativePlatform::toolWindowScreenRect(const AppWindow&, std::size_t) const { return {}; }
bool NativePlatform::captureToolWindow(const AppWindow&, std::size_t, Image&) { return false; }
bool NativePlatform::postClose(unsigned long long nativeWin) {
    if (!m->window || nativeWin != reinterpret_cast<unsigned long long>(m->window)) return false;
    ANativeActivity_finish(g_app->activity);
    return true;
}
// the soft keyboard is the text-input session here (committed text only, no pre-edit)
bool NativePlatform::textInputActive(const AppWindow& w) const { return m->win == &w && w.ui().wantsTextInput(); }
Rect NativePlatform::textInputArea(const AppWindow&) const { return {}; }
bool NativePlatform::inputMethodAvailable() const { return true; }
bool NativePlatform::inputMethodPreedit() const { return false; }

} // namespace fuse::app

// ---- entry point -----------------------------------------------------------------------------------------------------
// The glue's ANativeActivity_onCreate is compiled as fuse_glue_ANativeActivity_onCreate (CMake); this one runs first on
// the main thread to hook the main looper for runOnMain.
extern "C" void fuse_glue_ANativeActivity_onCreate(ANativeActivity* activity, void* savedState, size_t savedStateSize);
extern "C" JNIEXPORT void ANativeActivity_onCreate(ANativeActivity* activity, void* savedState, size_t savedStateSize) {
    using namespace fuse::app;
    if (g_mainPipe[0] < 0 && ::pipe2(g_mainPipe, O_NONBLOCK | O_CLOEXEC) == 0) {
        g_mainLooper = ALooper_forThread();
        ALooper_acquire(g_mainLooper);
        ALooper_addFd(g_mainLooper, g_mainPipe[0], ALOOPER_POLL_CALLBACK, ALOOPER_EVENT_INPUT, &mainJobsCallback, nullptr);
    }
    if (g_imeMain.edit) { // a new activity: the old one's EditText is gone with it
        activity->env->DeleteGlobalRef(g_imeMain.edit);
        g_imeMain.edit = nullptr;
    }
    g_mainActivity = activity;
    fuse_glue_ANativeActivity_onCreate(activity, savedState, savedStateSize);
}

extern "C" void android_main(android_app* app) {
    using namespace fuse::app;
    g_app = app;
    g_env = nullptr;
    g_savedState.clear();
    if (app->savedState) g_savedState.assign(static_cast<const char*>(app->savedState), app->savedStateSize);
    const double tEntry = msSinceProcessStart();
    redirectStdio();
    const std::string files = app->activity->internalDataPath ? app->activity->internalDataPath : ".";
    extractAssets(app, files);
    if (::chdir(files.c_str()) != 0) __android_log_print(ANDROID_LOG_WARN, kTag, "chdir %s failed", files.c_str());
    ::setenv("HOME", files.c_str(), 1);
    ::setenv("XDG_CONFIG_HOME", (files + "/config").c_str(), 1);
    ::setenv("XDG_RUNTIME_DIR", files.c_str(), 1);
    // argv: the library name, then the intent's "args" extra split at spaces
    std::vector<std::string> words{"fuse_app"};
    const std::string extra = intentExtra("args");
    const std::string envs = intentExtra("env");
    for (std::size_t i = 0; i < envs.size();) { // NAME=value words
        const std::size_t e = std::min(envs.find(' ', i), envs.size());
        const std::string w = envs.substr(i, e - i);
        if (const std::size_t eq = w.find('='); eq != std::string::npos && eq > 0)
            ::setenv(w.substr(0, eq).c_str(), w.substr(eq + 1).c_str(), 1);
        i = e + 1;
    }
    for (std::size_t i = 0; i < extra.size();) {
        const std::size_t e = std::min(extra.find(' ', i), extra.size());
        if (e > i) words.push_back(extra.substr(i, e - i));
        i = e + 1;
    }
    std::vector<char*> argv;
    for (std::string& w : words) argv.push_back(w.data());
    argv.push_back(nullptr);
    __android_log_print(ANDROID_LOG_INFO, kTag, "android_main at %.1f ms after process start, main at %.1f ms", tEntry,
                        msSinceProcessStart());
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
