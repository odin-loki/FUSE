// UI1 X11 text services gate (runs on Xvfb via the display wrapper; 77 = skip without X11 / display):
//   - XIM: the built-in input method (C.UTF-8, no XMODIFIERS) gives every window an input context; typed keys
//     commit through Xutf8LookupString, and a dead_acute + e compose sequence commits "é" with no text for the dead key.
//   - Pre-edit: the XIM pre-edit draw logic (driven through the debug hook, since Xvfb has no IM server) produces
//     TextComposition events with the full pre-edit and byte caret in EventPump::composition(); endTextInput ends it.
//   - CLIPBOARD: setClipboardText owns the selection and serves UTF8_STRING / STRING / TARGETS to another client;
//     clipboardText reads a selection owned by another process (UTF8_STRING) and loses ownership correctly.
#include <fuse/platform/event_pump.hpp>
#include <fuse/platform/text_services.hpp>
#include <fuse/platform/window.hpp>
#include <fuse/platform/window_wsi.hpp>

#include <clocale>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(FUSE_PLATFORM_WINDOW_X11)
#include "../src/platform/x11_window.hpp"
#include <sys/wait.h>
#include <unistd.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#endif

namespace {

int g_failures = 0;

[[maybe_unused]] void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    } else {
        std::printf("  ok: %s\n", message);
    }
}

#if defined(FUSE_PLATFORM_WINDOW_X11)
using fuse::platform::EventPump;
using fuse::platform::PlatformEvent;
using fuse::platform::PlatformEventType;

std::vector<PlatformEvent> pump(Display* dpy, EventPump& p) {
    std::vector<PlatformEvent> all;
    for (int i = 0; i < 4; ++i) {
        XSync(dpy, False);
        p.processOsEvents();
        std::vector<PlatformEvent> events;
        p.drainEvents(events);
        all.insert(all.end(), events.begin(), events.end());
        usleep(2000);
    }
    return all;
}

std::string typed(const std::vector<PlatformEvent>& events) {
    std::string s;
    for (const PlatformEvent& e : events) {
        if (e.type == PlatformEventType::TextInput) {
            s += e.text;
        }
    }
    return s;
}

int countType(const std::vector<PlatformEvent>& events, PlatformEventType type) {
    int n = 0;
    for (const PlatformEvent& e : events) {
        n += e.type == type ? 1 : 0;
    }
    return n;
}

void sendKeycode(Display* dpy, ::Window xid, unsigned keycode, bool press, unsigned state = 0) {
    XEvent ev{};
    ev.xkey.type = press ? KeyPress : KeyRelease;
    ev.xkey.display = dpy;
    ev.xkey.window = xid;
    ev.xkey.root = DefaultRootWindow(dpy);
    ev.xkey.time = CurrentTime;
    ev.xkey.same_screen = True;
    ev.xkey.keycode = keycode;
    ev.xkey.state = state;
    XSendEvent(dpy, xid, False, press ? KeyPressMask : KeyReleaseMask, &ev);
}

void tap(Display* dpy, ::Window xid, unsigned keycode) {
    sendKeycode(dpy, xid, keycode, true);
    sendKeycode(dpy, xid, keycode, false);
}

// Maps `sym` onto a spare keycode (Xvfb's default map has no dead keys). Returns 0 when none is free.
unsigned bindSpareKeycode(Display* dpy, KeySym sym) {
    if (unsigned existing = XKeysymToKeycode(dpy, sym)) {
        return existing;
    }
    int minCode = 0, maxCode = 0, perCode = 0;
    XDisplayKeycodes(dpy, &minCode, &maxCode);
    KeySym* map = XGetKeyboardMapping(dpy, static_cast<KeyCode>(minCode), maxCode - minCode + 1, &perCode);
    unsigned found = 0;
    for (int code = maxCode; code >= minCode && found == 0; --code) {
        bool empty = true;
        for (int i = 0; i < perCode; ++i) {
            empty = empty && map[(code - minCode) * perCode + i] == NoSymbol;
        }
        if (empty) {
            found = static_cast<unsigned>(code);
        }
    }
    XFree(map);
    if (found != 0) {
        KeySym syms[1] = {sym};
        XChangeKeyboardMapping(dpy, static_cast<int>(found), 1, syms, 1);
        XSync(dpy, False);
    }
    return found;
}

// Second client: converts CLIPBOARD to `target` and reads the reply while `owner` services requests.
bool otherClientRead(Display* other, Display* ownerDpy, EventPump& p, Atom target, std::vector<unsigned char>& out,
                     Atom& gotType) {
    const ::Window w = XCreateSimpleWindow(other, DefaultRootWindow(other), 0, 0, 1, 1, 0, 0, 0);
    const Atom clip = XInternAtom(other, "CLIPBOARD", False);
    const Atom prop = XInternAtom(other, "GATE_PROP", False);
    XConvertSelection(other, clip, target, prop, w, CurrentTime);
    XFlush(other);
    bool ok = false;
    for (int i = 0; i < 500 && !ok; ++i) {
        XSync(ownerDpy, False);
        p.processOsEvents(); // owner answers SelectionRequest here
        std::vector<PlatformEvent> drop;
        p.drainEvents(drop);
        XEvent ev;
        if (XCheckTypedWindowEvent(other, w, SelectionNotify, &ev) == True) {
            if (ev.xselection.property != None) {
                Atom type = None;
                int format = 0;
                unsigned long count = 0, after = 0;
                unsigned char* data = nullptr;
                XGetWindowProperty(other, w, prop, 0, 1 << 20, True, AnyPropertyType, &type, &format, &count, &after,
                                   &data);
                const std::size_t bytes = format == 32 ? count * sizeof(long) : count;
                out.assign(data, data + bytes);
                gotType = type;
                XFree(data);
                ok = true;
            } else {
                break;
            }
        }
        usleep(2000);
    }
    XDestroyWindow(other, w);
    return ok;
}

// Child process that owns CLIPBOARD with `text` and serves UTF8_STRING for ~3 s.
[[noreturn]] void runOwnerChild(int readyFd, const char* text) {
    Display* dpy = XOpenDisplay(nullptr);
    if (dpy == nullptr) {
        _exit(2);
    }
    const ::Window w = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy), 0, 0, 1, 1, 0, 0, 0);
    const Atom clip = XInternAtom(dpy, "CLIPBOARD", False);
    const Atom utf8 = XInternAtom(dpy, "UTF8_STRING", False);
    XSetSelectionOwner(dpy, clip, w, CurrentTime);
    XSync(dpy, False);
    const char ready = XGetSelectionOwner(dpy, clip) == w ? 'y' : 'n';
    (void)!write(readyFd, &ready, 1);
    for (int i = 0; i < 1500; ++i) {
        while (XPending(dpy) > 0) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            if (ev.type == SelectionRequest) {
                XSelectionEvent reply{};
                reply.type = SelectionNotify;
                reply.requestor = ev.xselectionrequest.requestor;
                reply.selection = ev.xselectionrequest.selection;
                reply.target = ev.xselectionrequest.target;
                reply.time = ev.xselectionrequest.time;
                reply.property = None;
                if (ev.xselectionrequest.target == utf8) {
                    XChangeProperty(dpy, reply.requestor, ev.xselectionrequest.property, utf8, 8, PropModeReplace,
                                    reinterpret_cast<const unsigned char*>(text), static_cast<int>(std::strlen(text)));
                    reply.property = ev.xselectionrequest.property;
                }
                XSendEvent(dpy, reply.requestor, False, 0, reinterpret_cast<XEvent*>(&reply));
                XFlush(dpy);
            }
        }
        usleep(2000);
    }
    XCloseDisplay(dpy);
    _exit(0);
}

int runGate() {
    std::setlocale(LC_CTYPE, "C.UTF-8");
    unsetenv("XMODIFIERS");
    if (!fuse::platform::windowWsiAvailable()) {
        std::printf("SKIP: no X display reachable\n");
        return 77;
    }
    auto* dpy = static_cast<Display*>(fuse::platform::nativeDisplayHandle());

    fuse::platform::WindowDesc desc{};
    desc.title = "FUSE UI1 X11 text services";
    desc.width = 320;
    desc.height = 200;
    desc.createNative = true;
    fuse::platform::Window window(desc);
    const auto xid = static_cast<::Window>(reinterpret_cast<std::uintptr_t>(window.nativeHandle().value));
    expectTrue(xid != 0, "native X11 window");
    EventPump p;
    (void)pump(dpy, p);

    // --- XIM ---
    const bool im = fuse::platform::x11::inputMethodAvailable();
    expectTrue(im, "an X input method opens under C.UTF-8 (built-in compose IM)");
    fuse::platform::beginTextInput(window);
    tap(dpy, xid, XKeysymToKeycode(dpy, XK_a));
    sendKeycode(dpy, xid, XKeysymToKeycode(dpy, XK_Shift_L), true);
    sendKeycode(dpy, xid, XKeysymToKeycode(dpy, XK_b), true, ShiftMask);
    sendKeycode(dpy, xid, XKeysymToKeycode(dpy, XK_b), false, ShiftMask);
    sendKeycode(dpy, xid, XKeysymToKeycode(dpy, XK_Shift_L), false, ShiftMask);
    auto events = pump(dpy, p);
    expectTrue(typed(events) == "aB", "typed keys commit UTF-8 text through the input context");
    expectTrue(countType(events, PlatformEventType::KeyDown) >= 2, "key events still arrive alongside text");

    const unsigned dead = bindSpareKeycode(dpy, XK_dead_acute);
    expectTrue(dead != 0, "dead_acute bound to a spare keycode");
    if (im && dead != 0) {
        tap(dpy, xid, dead);
        events = pump(dpy, p);
        expectTrue(typed(events).empty(), "dead key alone commits nothing (filtered by the IM)");
        tap(dpy, xid, XKeysymToKeycode(dpy, XK_e));
        events = pump(dpy, p);
        std::printf("    compose result: '%s'\n", typed(events).c_str());
        expectTrue(typed(events) == "\xC3\xA9", "dead_acute + e composes to U+00E9 through XIM");
    }

    // --- pre-edit (callback logic via the debug hook) ---
    fuse::platform::x11::debugPreeditDraw(window.nativeHandle().value, 0, 0, "\xE3\x81\x8B\xE3\x81\xAA", 2);
    events = pump(dpy, p);
    expectTrue(countType(events, PlatformEventType::TextComposition) == 1, "pre-edit draw -> one TextComposition");
    expectTrue(p.composition() == "\xE3\x81\x8B\xE3\x81\xAA" && p.compositionCursor() == 6,
               "pre-edit text and byte caret reach EventPump::composition()");
    fuse::platform::x11::debugPreeditDraw(window.nativeHandle().value, 1, 1, "\xE3\x82\x93", 1);
    (void)pump(dpy, p);
    expectTrue(p.composition() == "\xE3\x81\x8B\xE3\x82\x93" && p.compositionCursor() == 3,
               "partial pre-edit replacement keeps the head and moves the caret");
    fuse::platform::endTextInput(window);
    events = pump(dpy, p);
    expectTrue(countType(events, PlatformEventType::TextComposition) >= 1 && p.composition().empty(),
               "endTextInput cancels the pending pre-edit");
    fuse::platform::setTextInputArea(window, 10, 20, 2, 16);
    expectTrue(fuse::platform::lastTextInputArea().y == 20, "text input area recorded (XNSpotLocation set)");

    // --- CLIPBOARD as owner ---
    const char* text = "h\xC3\xA9llo \xE2\x9C\x93";
    expectTrue(fuse::platform::setClipboardText(text), "setClipboardText owns CLIPBOARD");
    const Atom clip = XInternAtom(dpy, "CLIPBOARD", False);
    expectTrue(XGetSelectionOwner(dpy, clip) != None, "CLIPBOARD has an owner");
    Display* other = XOpenDisplay(nullptr);
    expectTrue(other != nullptr, "second client connects");
    if (other != nullptr) {
        std::vector<unsigned char> bytes;
        Atom type = None;
        const bool gotUtf8 = otherClientRead(other, dpy, p, XInternAtom(other, "UTF8_STRING", False), bytes, type);
        expectTrue(gotUtf8 && std::string(bytes.begin(), bytes.end()) == text,
                   "another client reads UTF8_STRING from FUSE's CLIPBOARD");
        const bool gotLatin = otherClientRead(other, dpy, p, XA_STRING, bytes, type);
        expectTrue(gotLatin && std::string(bytes.begin(), bytes.end()) == "h\xE9llo ?",
                   "STRING target is Latin-1 with '?' for code points above U+00FF");
        const bool gotTargets = otherClientRead(other, dpy, p, XInternAtom(other, "TARGETS", False), bytes, type);
        bool hasUtf8 = false;
        for (std::size_t i = 0; gotTargets && i + sizeof(long) <= bytes.size(); i += sizeof(long)) {
            long atom = 0;
            std::memcpy(&atom, bytes.data() + i, sizeof(long));
            hasUtf8 = hasUtf8 || static_cast<Atom>(atom) == XInternAtom(other, "UTF8_STRING", False);
        }
        expectTrue(gotTargets && type == XA_ATOM && hasUtf8, "TARGETS lists UTF8_STRING");
        XCloseDisplay(other);
    }
    std::string back;
    expectTrue(fuse::platform::clipboardText(back) && back == text, "owner reads its own clipboard back");

    // --- CLIPBOARD owned by another process ---
    int fds[2];
    if (pipe(fds) == 0) {
        const char* childText = "from another client \xE2\x86\x92 ok";
        const pid_t child = fork();
        if (child == 0) {
            close(fds[0]);
            runOwnerChild(fds[1], childText);
        }
        close(fds[1]);
        char ready = 0;
        (void)!read(fds[0], &ready, 1);
        close(fds[0]);
        expectTrue(ready == 'y', "child process owns CLIPBOARD");
        (void)pump(dpy, p); // SelectionClear for us
        std::string got;
        const bool ok = fuse::platform::clipboardText(got);
        expectTrue(ok && got == childText, "clipboardText reads a selection owned by another process");
        kill(child, SIGTERM);
        int status = 0;
        waitpid(child, &status, 0);
        expectTrue(fuse::platform::setClipboardText("again") && fuse::platform::clipboardText(back) && back == "again",
                   "taking the clipboard back after another owner");
    }
    return EXIT_SUCCESS;
}
#endif

} // namespace

int main() {
#if !defined(FUSE_PLATFORM_WINDOW_X11)
    std::printf("SKIP: X11 window backend not compiled in\n");
    return 77;
#else
    const int rc = runGate();
    if (rc == 77) {
        return 77;
    }
    if (g_failures == 0 && rc == EXIT_SUCCESS) {
        std::printf("fuse_core_ui1_x11_text_services: all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "fuse_core_ui1_x11_text_services: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
#endif
}
