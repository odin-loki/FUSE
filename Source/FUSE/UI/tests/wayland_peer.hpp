// A second Wayland client for the fuse_app Wayland gates (test only): its own connection on its own thread, so the
// app's backend and this "other application" talk only through the compositor. libwayland-client and libxkbcommon
// are dlopen'd like the backend does. What it can do:
//   * a small xdg_toplevel of its own (app_id "peer"): takes the keyboard focus, so selections reach it
//   * clipboard (wl_data_device) and primary selection (zwp_primary_selection_v1): read the current selection, set one
//   * real seat input on wlroots compositors: zwlr_virtual_pointer_v1 (absolute motion in layout coordinates, buttons)
//     and zwp_virtual_keyboard_v1 (evdev keys through an xkbcommon US keymap)
//   * an input method (zwp_input_method_v2): pre-edit and commit strings for the focused text-input-v3 client
//   * drag and drop source (wl_data_device.start_drag from its own window, with a press of the virtual pointer)
// Every operation runs on the peer's thread; Peer::call() pumps the app's Application meanwhile.
#pragma once

#include "fuse/app/app.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <poll.h>
#include <string>
#include <thread>
#include <vector>

#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>

namespace peer {

struct wl_interface;
struct wl_message {
    const char* name;
    const char* signature;
    const wl_interface** types;
};
struct wl_interface {
    const char* name;
    int version;
    int method_count;
    const wl_message* methods;
    int event_count;
    const wl_message* events;
};
struct wl_array {
    std::size_t size, alloc;
    void* data;
};
struct wl_proxy;
struct wl_display;
using Fn = void (*)(void);
template <class F>
inline Fn fn(F f) { return reinterpret_cast<Fn>(f); }

struct Api {
    void* so = nullptr;
    void* xkb = nullptr;
    wl_display* (*connect)(const char*) = nullptr;
    void (*disconnect)(wl_display*) = nullptr;
    int (*get_fd)(wl_display*) = nullptr;
    int (*dispatch_pending)(wl_display*) = nullptr;
    int (*roundtrip)(wl_display*) = nullptr;
    int (*flush)(wl_display*) = nullptr;
    int (*prepare_read)(wl_display*) = nullptr;
    int (*read_events)(wl_display*) = nullptr;
    void (*cancel_read)(wl_display*) = nullptr;
    wl_proxy* (*ctor)(wl_proxy*, uint32_t, const wl_interface*, ...) = nullptr;
    wl_proxy* (*ctorv)(wl_proxy*, uint32_t, const wl_interface*, uint32_t, ...) = nullptr;
    void (*marshal)(wl_proxy*, uint32_t, ...) = nullptr;
    int (*add_listener)(wl_proxy*, Fn*, void*) = nullptr;
    void (*destroy)(wl_proxy*) = nullptr;
    const wl_interface *registry = nullptr, *compositor = nullptr, *surface = nullptr, *shm = nullptr, *shmPool = nullptr,
                       *buffer = nullptr, *seat = nullptr, *pointer = nullptr, *keyboard = nullptr, *ddManager = nullptr,
                       *dataDevice = nullptr, *dataSource = nullptr, *dataOffer = nullptr, *output = nullptr;
    // xkbcommon: a keymap string for the virtual keyboard
    void* (*xkb_context_new)(int) = nullptr;
    void* (*xkb_keymap_new_from_names)(void*, const void*, int) = nullptr;
    char* (*xkb_keymap_get_as_string)(void*, int) = nullptr;
    bool load() {
        if (so) return true;
        so = dlopen("libwayland-client.so.0", RTLD_NOW | RTLD_LOCAL);
        if (!so) return false;
        bool ok = true;
        auto f = [&](auto& p, const char* n) { p = reinterpret_cast<std::remove_reference_t<decltype(p)>>(dlsym(so, n)); ok = ok && p; };
        auto d = [&](const wl_interface*& p, const char* n) { p = static_cast<const wl_interface*>(dlsym(so, n)); ok = ok && p; };
        f(connect, "wl_display_connect"); f(disconnect, "wl_display_disconnect"); f(get_fd, "wl_display_get_fd");
        f(dispatch_pending, "wl_display_dispatch_pending"); f(roundtrip, "wl_display_roundtrip"); f(flush, "wl_display_flush");
        f(prepare_read, "wl_display_prepare_read"); f(read_events, "wl_display_read_events"); f(cancel_read, "wl_display_cancel_read");
        f(ctor, "wl_proxy_marshal_constructor"); f(ctorv, "wl_proxy_marshal_constructor_versioned"); f(marshal, "wl_proxy_marshal");
        f(add_listener, "wl_proxy_add_listener"); f(destroy, "wl_proxy_destroy");
        d(registry, "wl_registry_interface"); d(compositor, "wl_compositor_interface"); d(surface, "wl_surface_interface");
        d(shm, "wl_shm_interface"); d(shmPool, "wl_shm_pool_interface"); d(buffer, "wl_buffer_interface"); d(seat, "wl_seat_interface");
        d(pointer, "wl_pointer_interface"); d(keyboard, "wl_keyboard_interface"); d(ddManager, "wl_data_device_manager_interface");
        d(dataDevice, "wl_data_device_interface"); d(dataSource, "wl_data_source_interface"); d(dataOffer, "wl_data_offer_interface");
        d(output, "wl_output_interface");
        if ((xkb = dlopen("libxkbcommon.so.0", RTLD_NOW | RTLD_LOCAL))) {
            xkb_context_new = reinterpret_cast<void* (*)(int)>(dlsym(xkb, "xkb_context_new"));
            xkb_keymap_new_from_names = reinterpret_cast<void* (*)(void*, const void*, int)>(dlsym(xkb, "xkb_keymap_new_from_names"));
            xkb_keymap_get_as_string = reinterpret_cast<char* (*)(void*, int)>(dlsym(xkb, "xkb_keymap_get_as_string"));
        }
        return ok;
    }
};
inline Api& api() {
    static Api a;
    return a;
}

// ---- protocol tables (only what the peer sends / receives) ----
extern const wl_interface wmBaseI, positionerI, xdgSurfaceI, toplevelI, popupI, vptrMgrI, vptrI, vkbdMgrI, vkbdI, imMgrI, imI,
    imPopupI, imGrabI, psMgrI, psDevI, psSrcI, psOfferI;
const wl_interface* tNone[8] = {};
const wl_interface* tPositioner[] = {&positionerI};
const wl_interface* tGetXdgSurface[] = {&xdgSurfaceI, nullptr};
const wl_interface* tToplevel[] = {&toplevelI};
const wl_interface* tPopup[] = {&popupI, &xdgSurfaceI, &positionerI};
const wl_interface* tParent[] = {&toplevelI};
const wl_interface* tVptr[] = {nullptr /*seat*/, &vptrI};
const wl_interface* tVkbd[] = {nullptr /*seat*/, &vkbdI};
const wl_interface* tIm[] = {nullptr /*seat*/, &imI};
const wl_interface* tImPopup[] = {&imPopupI, nullptr /*surface*/};
const wl_interface* tImGrab[] = {&imGrabI};
const wl_interface* tPsSrc[] = {&psSrcI};
const wl_interface* tPsDev[] = {&psDevI, nullptr /*seat*/};
const wl_interface* tPsSet[] = {&psSrcI, nullptr};
const wl_interface* tPsOffer[] = {&psOfferI};
const wl_message wmBaseReq[] = {{"destroy", "", tNone}, {"create_positioner", "n", tPositioner}, {"get_xdg_surface", "no", tGetXdgSurface}, {"pong", "u", tNone}};
const wl_message wmBaseEv[] = {{"ping", "u", tNone}};
const wl_message xsReq[] = {{"destroy", "", tNone}, {"get_toplevel", "n", tToplevel}, {"get_popup", "n?oo", tPopup},
                                   {"set_window_geometry", "iiii", tNone}, {"ack_configure", "u", tNone}};
const wl_message xsEv[] = {{"configure", "u", tNone}};
const wl_message tlReq[] = {{"destroy", "", tNone}, {"set_parent", "?o", tParent}, {"set_title", "s", tNone}, {"set_app_id", "s", tNone}};
const wl_message tlEv[] = {{"configure", "iia", tNone}, {"close", "", tNone}};
const wl_message vptrMgrReq[] = {{"create_virtual_pointer", "?on", tVptr}, {"destroy", "", tNone}};
const wl_message vptrReq[] = {{"motion", "uff", tNone}, {"motion_absolute", "uuuuu", tNone}, {"button", "uuu", tNone},
                                     {"axis", "uuf", tNone}, {"frame", "", tNone}, {"axis_source", "u", tNone},
                                     {"axis_stop", "uu", tNone}, {"axis_discrete", "uufi", tNone}, {"destroy", "", tNone}};
const wl_message vkbdMgrReq[] = {{"create_virtual_keyboard", "on", tVkbd}};
const wl_message vkbdReq[] = {{"keymap", "uhu", tNone}, {"key", "uuu", tNone}, {"modifiers", "uuuu", tNone}, {"destroy", "", tNone}};
const wl_message imMgrReq[] = {{"get_input_method", "on", tIm}, {"destroy", "", tNone}};
const wl_message imReq[] = {{"commit_string", "s", tNone}, {"set_preedit_string", "sii", tNone}, {"delete_surrounding_text", "uu", tNone},
                                   {"commit", "u", tNone}, {"get_input_popup_surface", "no", tImPopup}, {"grab_keyboard", "n", tImGrab},
                                   {"destroy", "", tNone}};
const wl_message imEv[] = {{"activate", "", tNone}, {"deactivate", "", tNone}, {"surrounding_text", "suu", tNone},
                                  {"text_change_cause", "u", tNone}, {"content_type", "uu", tNone}, {"done", "", tNone}, {"unavailable", "", tNone}};
const wl_message psMgrReq[] = {{"create_source", "n", tPsSrc}, {"get_device", "no", tPsDev}, {"destroy", "", tNone}};
const wl_message psDevReq[] = {{"set_selection", "?ou", tPsSet}, {"destroy", "", tNone}};
const wl_message psDevEv[] = {{"data_offer", "n", tPsOffer}, {"selection", "?o", tPsOffer}};
const wl_message psOfferReq[] = {{"receive", "sh", tNone}, {"destroy", "", tNone}};
const wl_message psOfferEv[] = {{"offer", "s", tNone}};
const wl_message psSrcReq[] = {{"offer", "s", tNone}, {"destroy", "", tNone}};
const wl_message psSrcEv[] = {{"send", "sh", tNone}, {"cancelled", "", tNone}};
template <std::size_t N>
constexpr int cnt(const wl_message (&)[N]) { return int(N); }
const wl_interface wmBaseI{"xdg_wm_base", 1, cnt(wmBaseReq), wmBaseReq, cnt(wmBaseEv), wmBaseEv};
const wl_interface positionerI{"xdg_positioner", 1, 0, nullptr, 0, nullptr};
const wl_interface xdgSurfaceI{"xdg_surface", 1, cnt(xsReq), xsReq, cnt(xsEv), xsEv};
const wl_interface toplevelI{"xdg_toplevel", 1, cnt(tlReq), tlReq, cnt(tlEv), tlEv};
const wl_interface popupI{"xdg_popup", 1, 0, nullptr, 0, nullptr};
const wl_interface vptrMgrI{"zwlr_virtual_pointer_manager_v1", 1, cnt(vptrMgrReq), vptrMgrReq, 0, nullptr};
const wl_interface vptrI{"zwlr_virtual_pointer_v1", 1, cnt(vptrReq), vptrReq, 0, nullptr};
const wl_interface vkbdMgrI{"zwp_virtual_keyboard_manager_v1", 1, cnt(vkbdMgrReq), vkbdMgrReq, 0, nullptr};
const wl_interface vkbdI{"zwp_virtual_keyboard_v1", 1, cnt(vkbdReq), vkbdReq, 0, nullptr};
const wl_interface imMgrI{"zwp_input_method_manager_v2", 1, cnt(imMgrReq), imMgrReq, 0, nullptr};
const wl_interface imI{"zwp_input_method_v2", 1, cnt(imReq), imReq, cnt(imEv), imEv};
const wl_interface imPopupI{"zwp_input_popup_surface_v2", 1, 0, nullptr, 0, nullptr};
const wl_interface imGrabI{"zwp_input_method_keyboard_grab_v2", 1, 0, nullptr, 0, nullptr};
const wl_interface psMgrI{"zwp_primary_selection_device_manager_v1", 1, cnt(psMgrReq), psMgrReq, 0, nullptr};
const wl_interface psDevI{"zwp_primary_selection_device_v1", 1, cnt(psDevReq), psDevReq, cnt(psDevEv), psDevEv};
const wl_interface psSrcI{"zwp_primary_selection_source_v1", 1, cnt(psSrcReq), psSrcReq, cnt(psSrcEv), psSrcEv};
const wl_interface psOfferI{"zwp_primary_selection_offer_v1", 1, cnt(psOfferReq), psOfferReq, cnt(psOfferEv), psOfferEv};

struct Offer {
    wl_proxy* p = nullptr;
    bool primary = false;
    std::vector<std::string> mimes;
};

class Peer {
public:
    // ---- state (peer thread) ----
    wl_display* d = nullptr;
    wl_proxy *reg = nullptr, *comp = nullptr, *shm = nullptr, *seat = nullptr, *wm = nullptr, *ddm = nullptr, *dd = nullptr, *psm = nullptr,
             *psd = nullptr, *vptrMgr = nullptr, *vptr = nullptr, *vkbdMgr = nullptr, *vkbd = nullptr, *imMgr = nullptr, *im = nullptr,
             *kbd = nullptr, *ptr = nullptr;
    wl_proxy *surf = nullptr, *xs = nullptr, *tl = nullptr, *buf = nullptr;
    bool configured = false, focused = false;
    uint32_t kbdSerial = 0, ptrSerial = 0, ptrButtonSerial = 0, ddVersion = 1;
    bool ptrOnUs = false;
    std::vector<Offer*> offers;
    Offer* sel[2] = {nullptr, nullptr};
    std::map<std::string, std::string> serve[2]; ///< our selection's data by MIME type
    wl_proxy* src[2] = {nullptr, nullptr};
    bool cancelled[2] = {false, false};
    std::vector<std::string> sentMimes; ///< MIME types another client asked our sources for
    // input method
    int imActivations = 0, imDone = 0;
    bool imActive = false;
    // drag source
    wl_proxy* dragSrc = nullptr;
    std::string dragData;
    bool dragFinished = false, dragDropped = false;
    double extentW = 1, extentH = 1; ///< layout size for absolute motion
    uint32_t time = 1;

    bool start(std::string& err) {
        if (!api().load()) {
            err = "libwayland-client";
            return false;
        }
        if (::pipe2(wake, O_CLOEXEC | O_NONBLOCK) != 0) return false;
        std::promise<bool> ok;
        auto f = ok.get_future();
        th = std::thread([this, &ok] {
            const bool r = init();
            ok.set_value(r);
            if (r) loop();
        });
        if (!f.get()) {
            err = "peer could not connect / bind";
            stop = true;
            th.join();
            return false;
        }
        return true;
    }
    ~Peer() {
        if (th.joinable()) {
            post([this] {
                if (d) {
                    api().flush(d);
                    api().disconnect(d);
                    d = nullptr;
                }
                stop = true;
            });
            th.join();
        }
        if (wake[0] >= 0) ::close(wake[0]);
        if (wake[1] >= 0) ::close(wake[1]);
    }
    /// Runs `f` on the peer thread while the app keeps pumping; returns its result.
    template <class F>
    auto call(fuse::app::Application& app, F f) -> decltype(f()) {
        using R = decltype(f());
        std::promise<R> p;
        auto fut = p.get_future();
        post([&] {
            if constexpr (std::is_void_v<R>) {
                f();
                p.set_value();
            } else {
                p.set_value(f());
            }
        });
        while (fut.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) app.step(0.005f);
        return fut.get();
    }

    // ---- operations (call them through call()) ----
    /// Waits (round trips) until `pred` holds or `ms` pass.
    template <class P>
    bool waitFor(P pred, int ms = 3000) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (!pred()) {
            if (std::chrono::steady_clock::now() > end) return false;
            api().roundtrip(d);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return true;
    }
    bool openWindow(const char* title, int w = 100, int h = 100) {
        Api& A = api();
        surf = A.ctor(comp, 0, A.surface, nullptr);
        static const Fn sl[] = {fn(&nop2), fn(&nop2), fn(&nopI), fn(&nopU)};
        A.add_listener(surf, const_cast<Fn*>(sl), this);
        xs = A.ctor(wm, 2, &xdgSurfaceI, nullptr, surf);
        static const Fn xl[] = {fn(&xsConfigure)};
        A.add_listener(xs, const_cast<Fn*>(xl), this);
        tl = A.ctor(xs, 1, &toplevelI, nullptr);
        static const Fn tll[] = {fn(&tlConfigure), fn(&nop1)};
        A.add_listener(tl, const_cast<Fn*>(tll), this);
        A.marshal(tl, 2, title);
        A.marshal(tl, 3, "peer");
        A.marshal(surf, 6); // commit: first configure
        configured = false;
        if (!waitFor([&] { return configured; })) return false;
        // a buffer maps the window
        const int stride = w * 4, size = stride * h;
        const int fd = ::memfd_create("peer", MFD_CLOEXEC);
        if (fd < 0 || ::ftruncate(fd, size) != 0) return false;
        void* px = ::mmap(nullptr, std::size_t(size), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (px != MAP_FAILED) {
            for (int i = 0; i < w * h; ++i) static_cast<uint32_t*>(px)[i] = 0xFF3060A0u;
            ::munmap(px, std::size_t(size));
        }
        wl_proxy* pool = A.ctor(shm, 0, A.shmPool, nullptr, fd, int32_t(size));
        buf = A.ctor(pool, 0, A.buffer, nullptr, int32_t(0), int32_t(w), int32_t(h), int32_t(stride), uint32_t(1));
        A.marshal(pool, 1);
        A.destroy(pool);
        ::close(fd);
        A.marshal(surf, 1, buf, int32_t(0), int32_t(0));
        A.marshal(surf, 2, int32_t(0), int32_t(0), int32_t(w), int32_t(h));
        A.marshal(surf, 6);
        return waitFor([&] { return focused; });
    }
    void closeWindow() {
        Api& A = api();
        if (tl) { A.marshal(tl, 0); A.destroy(tl); tl = nullptr; }
        if (xs) { A.marshal(xs, 0); A.destroy(xs); xs = nullptr; }
        if (surf) { A.marshal(surf, 0); A.destroy(surf); surf = nullptr; }
        if (buf) { A.marshal(buf, 0); A.destroy(buf); buf = nullptr; }
        focused = false;
        A.roundtrip(d);
    }
    /// The current selection (0 clipboard, 1 primary) in `mime`, waiting up to `ms` for it to be offered.
    bool readSelection(int which, const std::string& mime, std::string& out, int ms = 3000) {
        out.clear();
        if (!waitFor([&] { return sel[which] && has(sel[which]->mimes, mime); }, ms)) return false;
        int fds[2];
        if (::pipe2(fds, O_CLOEXEC) != 0) return false;
        api().marshal(sel[which]->p, which ? 0u : 1u, mime.c_str(), fds[1]);
        ::close(fds[1]);
        api().flush(d);
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        for (;;) {
            pollfd p{fds[0], POLLIN, 0};
            if (::poll(&p, 1, 50) > 0) {
                char b[4096];
                const ssize_t n = ::read(fds[0], b, sizeof b);
                if (n > 0) { out.append(b, std::size_t(n)); continue; }
                if (n == 0) break;
            }
            api().roundtrip(d);
            if (std::chrono::steady_clock::now() > end) { ::close(fds[0]); return false; }
        }
        ::close(fds[0]);
        return true;
    }
    std::vector<std::string> selectionMimes(int which, int ms = 2000) {
        waitFor([&] { return sel[which] != nullptr; }, ms);
        return sel[which] ? sel[which]->mimes : std::vector<std::string>{};
    }
    /// Takes the selection with `data` (MIME -> bytes), with the serial of our keyboard focus.
    bool setSelection(int which, std::map<std::string, std::string> data) {
        Api& A = api();
        if (which == 0 && !dd) return false;
        if (which == 1 && !psd) return false;
        wl_proxy* s = which ? A.ctor(psm, 0, &psSrcI, nullptr) : A.ctor(ddm, 0, A.dataSource, nullptr);
        if (which) {
            static const Fn l[] = {fn(&srcSend<1>), fn(&srcCancelled<1>)};
            A.add_listener(s, const_cast<Fn*>(l), this);
        } else {
            static const Fn l[] = {fn(&nopS), fn(&srcSend<0>), fn(&srcCancelled<0>), fn(&dragDroppedEv), fn(&dragFinishedEv), fn(&nopU)};
            A.add_listener(s, const_cast<Fn*>(l), this);
        }
        for (const auto& [m, v] : data) A.marshal(s, 0, m.c_str());
        serve[which] = std::move(data);
        src[which] = s;
        cancelled[which] = false;
        A.marshal(which ? psd : dd, which ? 0u : 1u, s, kbdSerial);
        A.roundtrip(d);
        return true;
    }
    // virtual pointer (layout coordinates, logical px)
    bool pointerReady() const { return vptr != nullptr; }
    void moveTo(double x, double y) {
        Api& A = api();
        A.marshal(vptr, 1, time++, uint32_t(std::lround(x * 100.0)), uint32_t(std::lround(y * 100.0)), uint32_t(std::lround(extentW * 100.0)),
                  uint32_t(std::lround(extentH * 100.0)));
        A.marshal(vptr, 4);
        A.roundtrip(d);
    }
    void button(bool down, uint32_t code = 0x110) {
        Api& A = api();
        A.marshal(vptr, 2, time++, code, uint32_t(down ? 1 : 0));
        A.marshal(vptr, 4);
        A.roundtrip(d);
    }
    // virtual keyboard (evdev codes)
    bool keyboardReady() const { return vkbd != nullptr; }
    void key(uint32_t code, bool down) {
        api().marshal(vkbd, 1, time++, code, uint32_t(down ? 1 : 0));
        api().roundtrip(d);
    }
    void tap(uint32_t code) {
        key(code, true);
        key(code, false);
    }
    // input method
    bool imReady() const { return im != nullptr; }
    void imPreedit(const char* text, int32_t begin, int32_t end) {
        api().marshal(im, 1, text, begin, end);
        api().marshal(im, 3, uint32_t(imDone));
        api().roundtrip(d);
    }
    void imCommit(const char* text) {
        api().marshal(im, 0, text);
        api().marshal(im, 1, "", int32_t(0), int32_t(0)); // the pre-edit ends with the commit
        api().marshal(im, 3, uint32_t(imDone));
        api().roundtrip(d);
    }
    /// Starts a drag of `text` (text/plain;charset=utf-8 + text/uri-list) from our window: the virtual pointer must be
    /// pressed on it (ptrButtonSerial).
    bool startDrag(const std::string& uriList) {
        Api& A = api();
        if (!dd || !surf || !ptrButtonSerial) return false;
        dragSrc = A.ctor(ddm, 0, A.dataSource, nullptr);
        static const Fn l[] = {fn(&nopS), fn(&dragSend), fn(&dragCancelled), fn(&dragDroppedEv), fn(&dragFinishedEv), fn(&nopU)};
        A.add_listener(dragSrc, const_cast<Fn*>(l), this);
        A.marshal(dragSrc, 0, "text/uri-list");
        if (ddVersion >= 3) A.marshal(dragSrc, 2, uint32_t(1)); // set_actions copy
        dragData = uriList;
        A.marshal(dd, 0, dragSrc, surf, static_cast<wl_proxy*>(nullptr), ptrButtonSerial); // start_drag
        A.roundtrip(d);
        return true;
    }

private:
    std::thread th;
    std::mutex mu;
    std::deque<std::function<void()>> q;
    int wake[2] = {-1, -1};
    std::atomic<bool> stop{false};

    static bool has(const std::vector<std::string>& v, const std::string& s) {
        for (const std::string& x : v)
            if (x == s) return true;
        return false;
    }
    void post(std::function<void()> f) {
        {
            std::lock_guard<std::mutex> l(mu);
            q.push_back(std::move(f));
        }
        const char c = 1;
        [[maybe_unused]] ssize_t n = ::write(wake[1], &c, 1);
    }
    void loop() {
        Api& A = api();
        while (!stop) {
            if (d) {
                while (A.prepare_read(d) != 0) A.dispatch_pending(d);
                A.flush(d);
            }
            pollfd fds[2] = {{d ? A.get_fd(d) : -1, POLLIN, 0}, {wake[0], POLLIN, 0}};
            ::poll(fds, 2, 20);
            if (d) {
                if (fds[0].revents & POLLIN) A.read_events(d);
                else A.cancel_read(d);
                A.dispatch_pending(d);
            }
            char b[64];
            while (::read(wake[0], b, sizeof b) > 0) {}
            for (;;) {
                std::function<void()> f;
                {
                    std::lock_guard<std::mutex> l(mu);
                    if (q.empty()) break;
                    f = std::move(q.front());
                    q.pop_front();
                }
                f();
                if (stop) return;
            }
        }
    }
    bool init() {
        Api& A = api();
        tVptr[0] = A.seat;
        tVkbd[0] = A.seat;
        tIm[0] = A.seat;
        tImPopup[1] = A.surface;
        tGetXdgSurface[1] = A.surface;
        tPsDev[1] = A.seat;
        d = A.connect(nullptr);
        if (!d) return false;
        reg = A.ctor(reinterpret_cast<wl_proxy*>(d), 1, A.registry, nullptr);
        static const Fn rl[] = {fn(&global), fn(&nopU)};
        A.add_listener(reg, const_cast<Fn*>(rl), this);
        A.roundtrip(d);
        A.roundtrip(d);
        if (!comp || !shm || !wm || !seat) return false;
        if (ddm) {
            dd = A.ctor(ddm, 1, A.dataDevice, nullptr, seat);
            static const Fn l[] = {fn(&ddOffer), fn(&ddEnter), fn(&nop1), fn(&ddMotion), fn(&nop1), fn(&ddSelection)};
            A.add_listener(dd, const_cast<Fn*>(l), this);
        }
        if (psm) {
            psd = A.ctor(psm, 1, &psDevI, nullptr, seat);
            static const Fn l[] = {fn(&psOffer), fn(&psSelection)};
            A.add_listener(psd, const_cast<Fn*>(l), this);
        }
        if (vptrMgr) vptr = A.ctor(vptrMgr, 0, &vptrI, seat, nullptr);
        if (vkbdMgr) {
            vkbd = A.ctor(vkbdMgr, 0, &vkbdI, seat, nullptr);
            if (vkbd && !sendKeymap()) vkbd = nullptr;
        }
        if (imMgr) {
            im = A.ctor(imMgr, 0, &imI, seat, nullptr);
            static const Fn l[] = {fn(&imActivate), fn(&imDeactivate), fn(&imSurrounding), fn(&nopU), fn(&nopUU), fn(&imDoneEv), fn(&nop1)};
            A.add_listener(im, const_cast<Fn*>(l), this);
        }
        A.roundtrip(d);
        return true;
    }
    bool sendKeymap() {
        Api& A = api();
        if (!A.xkb_context_new || !A.xkb_keymap_new_from_names || !A.xkb_keymap_get_as_string) return false;
        struct Names { const char *rules, *model, *layout, *variant, *options; } names{"evdev", "pc105", "us", "", ""};
        void* ctx = A.xkb_context_new(0);
        void* km = ctx ? A.xkb_keymap_new_from_names(ctx, &names, 0) : nullptr;
        char* s = km ? A.xkb_keymap_get_as_string(km, 1) : nullptr;
        if (!s) return false;
        const std::size_t n = std::strlen(s) + 1;
        const int fd = ::memfd_create("peer-keymap", MFD_CLOEXEC);
        bool ok = fd >= 0 && ::ftruncate(fd, off_t(n)) == 0 && ::pwrite(fd, s, n, 0) == ssize_t(n);
        std::free(s);
        if (ok) A.marshal(vkbd, 0, uint32_t(1) /*xkb_v1*/, fd, uint32_t(n));
        if (fd >= 0) ::close(fd);
        return ok; // ctx / keymap are left to the process exit (test only)
    }

    // ---- listeners ----
    static void nop1(void*, wl_proxy*) {}
    static void nop2(void*, wl_proxy*, wl_proxy*) {}
    static void nopI(void*, wl_proxy*, int32_t) {}
    static void nopU(void*, wl_proxy*, uint32_t) {}
    static void nopUU(void*, wl_proxy*, uint32_t, uint32_t) {}
    static void nopS(void*, wl_proxy*, const char*) {}
    static void global(void* data, wl_proxy* r, uint32_t name, const char* iface, uint32_t version) {
        auto* p = static_cast<Peer*>(data);
        Api& A = api();
        const std::string n(iface);
        auto bind = [&](const wl_interface* i, uint32_t v) { return A.ctorv(r, 0, i, v, name, i->name, v, nullptr); };
        if (n == "wl_compositor") p->comp = bind(A.compositor, 4);
        else if (n == "wl_shm") p->shm = bind(A.shm, 1);
        else if (n == "xdg_wm_base") {
            p->wm = bind(&wmBaseI, 1);
            static const Fn l[] = {fn(&ping)};
            A.add_listener(p->wm, const_cast<Fn*>(l), p);
        } else if (n == "wl_seat" && !p->seat) {
            p->seat = bind(A.seat, std::min(version, 5u));
            static const Fn l[] = {fn(&seatCaps), fn(&nopS)};
            A.add_listener(p->seat, const_cast<Fn*>(l), p);
        } else if (n == "wl_data_device_manager") {
            p->ddVersion = std::min(version, 3u);
            p->ddm = bind(A.ddManager, p->ddVersion);
        } else if (n == "zwp_primary_selection_device_manager_v1") p->psm = bind(&psMgrI, 1);
        else if (n == "zwlr_virtual_pointer_manager_v1") p->vptrMgr = bind(&vptrMgrI, 1);
        else if (n == "zwp_virtual_keyboard_manager_v1") p->vkbdMgr = bind(&vkbdMgrI, 1);
        else if (n == "zwp_input_method_manager_v2") p->imMgr = bind(&imMgrI, 1);
    }
    static void ping(void*, wl_proxy* wm, uint32_t serial) { api().marshal(wm, 3, serial); }
    static void seatCaps(void* data, wl_proxy* seat, uint32_t caps) {
        auto* p = static_cast<Peer*>(data);
        if ((caps & 2u) && !p->kbd) {
            p->kbd = api().ctor(seat, 1, api().keyboard, nullptr);
            static const Fn l[] = {fn(&kbKeymap), fn(&kbEnter), fn(&kbLeave), fn(&kbKey), fn(&kbMods), fn(&kbRepeat)};
            api().add_listener(p->kbd, const_cast<Fn*>(l), p);
        }
        if ((caps & 1u) && !p->ptr) {
            p->ptr = api().ctor(seat, 0, api().pointer, nullptr);
            static const Fn l[] = {fn(&ptEnter), fn(&ptLeave), fn(&ptMotion), fn(&ptButton), fn(&ptAxis), fn(&nop1), fn(&nopU), fn(&nopUU), fn(&ptAxisDiscrete)};
            api().add_listener(p->ptr, const_cast<Fn*>(l), p);
        }
    }
    static void kbKeymap(void*, wl_proxy*, uint32_t, int32_t fd, uint32_t) { ::close(fd); }
    static void kbEnter(void* data, wl_proxy*, uint32_t serial, wl_proxy* s, wl_array*) {
        auto* p = static_cast<Peer*>(data);
        p->kbdSerial = serial;
        p->focused = s == p->surf;
    }
    static void kbLeave(void* data, wl_proxy*, uint32_t, wl_proxy*) { static_cast<Peer*>(data)->focused = false; }
    static void kbKey(void* data, wl_proxy*, uint32_t serial, uint32_t, uint32_t, uint32_t) { static_cast<Peer*>(data)->kbdSerial = serial; }
    static void kbMods(void*, wl_proxy*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {}
    static void kbRepeat(void*, wl_proxy*, int32_t, int32_t) {}
    static void ptEnter(void* data, wl_proxy*, uint32_t serial, wl_proxy* s, int32_t, int32_t) {
        auto* p = static_cast<Peer*>(data);
        p->ptrSerial = serial;
        p->ptrOnUs = s == p->surf;
    }
    static void ptLeave(void* data, wl_proxy*, uint32_t, wl_proxy*) { static_cast<Peer*>(data)->ptrOnUs = false; }
    static void ptMotion(void*, wl_proxy*, uint32_t, int32_t, int32_t) {}
    static void ptButton(void* data, wl_proxy*, uint32_t serial, uint32_t, uint32_t, uint32_t state) {
        if (state == 1) static_cast<Peer*>(data)->ptrButtonSerial = serial;
    }
    static void ptAxis(void*, wl_proxy*, uint32_t, uint32_t, int32_t) {}
    static void ptAxisDiscrete(void*, wl_proxy*, uint32_t, int32_t) {}
    static void xsConfigure(void* data, wl_proxy* xs, uint32_t serial) {
        api().marshal(xs, 4, serial);
        static_cast<Peer*>(data)->configured = true;
    }
    static void tlConfigure(void*, wl_proxy*, int32_t, int32_t, wl_array*) {}
    Offer* offerOf(wl_proxy* o) {
        for (Offer* x : offers)
            if (x->p == o) return x;
        return nullptr;
    }
    static void offerMime(void* data, wl_proxy*, const char* m) { static_cast<Offer*>(data)->mimes.emplace_back(m ? m : ""); }
    static void newOffer(Peer* p, wl_proxy* id, bool primary) {
        auto* o = new Offer{id, primary, {}};
        p->offers.push_back(o);
        static const Fn dl[] = {fn(&offerMime), fn(&nopU), fn(&nopU)};
        static const Fn pl[] = {fn(&offerMime)};
        api().add_listener(id, const_cast<Fn*>(primary ? pl : dl), o);
    }
    static void ddOffer(void* data, wl_proxy*, wl_proxy* id) { newOffer(static_cast<Peer*>(data), id, false); }
    static void psOffer(void* data, wl_proxy*, wl_proxy* id) { newOffer(static_cast<Peer*>(data), id, true); }
    static void ddSelection(void* data, wl_proxy*, wl_proxy* o) { auto* p = static_cast<Peer*>(data); p->sel[0] = o ? p->offerOf(o) : nullptr; }
    static void psSelection(void* data, wl_proxy*, wl_proxy* o) { auto* p = static_cast<Peer*>(data); p->sel[1] = o ? p->offerOf(o) : nullptr; }
    static void ddEnter(void*, wl_proxy*, uint32_t, wl_proxy*, int32_t, int32_t, wl_proxy*) {}
    static void ddMotion(void*, wl_proxy*, uint32_t, int32_t, int32_t) {}
    static void writeAll(int fd, const std::string& s) {
        std::size_t at = 0;
        while (at < s.size()) {
            const ssize_t n = ::write(fd, s.data() + at, s.size() - at);
            if (n <= 0) break;
            at += std::size_t(n);
        }
        ::close(fd);
    }
    template <int W>
    static void srcSend(void* data, wl_proxy*, const char* mime, int32_t fd) {
        auto* p = static_cast<Peer*>(data);
        p->sentMimes.emplace_back(mime ? mime : "");
        auto it = p->serve[W].find(mime ? mime : "");
        const std::string v = it == p->serve[W].end() ? std::string() : it->second;
        std::thread([fd, v] { writeAll(fd, v); }).detach();
    }
    template <int W>
    static void srcCancelled(void* data, wl_proxy* s) {
        auto* p = static_cast<Peer*>(data);
        if (p->src[W] == s) { p->src[W] = nullptr; p->cancelled[W] = true; }
        api().destroy(s);
    }
    static void dragSend(void* data, wl_proxy*, const char*, int32_t fd) {
        const std::string v = static_cast<Peer*>(data)->dragData;
        std::thread([fd, v] { writeAll(fd, v); }).detach();
    }
    static void dragCancelled(void* data, wl_proxy*) { static_cast<Peer*>(data)->dragFinished = true; }
    static void dragDroppedEv(void* data, wl_proxy*) { static_cast<Peer*>(data)->dragDropped = true; }
    static void dragFinishedEv(void* data, wl_proxy*) { static_cast<Peer*>(data)->dragFinished = true; }
    static void imActivate(void* data, wl_proxy*) { auto* p = static_cast<Peer*>(data); p->imActive = true; ++p->imActivations; }
    static void imDeactivate(void* data, wl_proxy*) { static_cast<Peer*>(data)->imActive = false; }
    static void imSurrounding(void*, wl_proxy*, const char*, uint32_t, uint32_t) {}
    static void imDoneEv(void* data, wl_proxy*) { ++static_cast<Peer*>(data)->imDone; }
};

} // namespace peer
