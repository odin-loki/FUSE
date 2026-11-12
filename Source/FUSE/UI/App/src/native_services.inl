// fuse_app native platform services for X11 and Win32 (plan §4.13): mouse cursors, the system clipboard, window
// management, system theme, displays, shell helpers and native file dialogs. Included by native.cpp at file scope
// after its own namespace fuse::app block (it needs NativePlatform::Impl and the file-local helpers there). X11 goes
// through the runtime-loaded function table only; every optional symbol is checked, so a missing one turns that
// feature off (false / no-op).

namespace fuse::app {
#if !defined(_WIN32)
// ---- POSIX process helpers (xdg-open, zenity / kdialog, gsettings) ---------------------------------------------------
namespace {
bool findInPath(std::string_view name) {
    const char* path = std::getenv("PATH");
    if (!path) return false;
    std::string_view p(path);
    while (!p.empty()) {
        const std::size_t c = p.find(':');
        const std::string full = std::string(p.substr(0, c)) + "/" + std::string(name);
        if (c != 0 && ::access(full.c_str(), X_OK) == 0) return true;
        p = c == std::string_view::npos ? std::string_view{} : p.substr(c + 1);
    }
    return false;
}
std::vector<char*> argvOf(const std::vector<std::string>& args) {
    std::vector<char*> v;
    for (const std::string& a : args) v.push_back(const_cast<char*>(a.c_str()));
    v.push_back(nullptr);
    return v;
}
/// Runs args[0] (looked up in PATH), waits for it and captures its stdout. False when it could not be started.
bool runCapture(const std::vector<std::string>& args, std::string& out, int& exitCode) {
    int fds[2];
    if (args.empty() || ::pipe(fds) != 0) return false;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
    posix_spawn_file_actions_addclose(&fa, fds[0]);
    posix_spawn_file_actions_addclose(&fa, fds[1]);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0); // toolkit warnings are not ours to print
    std::vector<char*> argv = argvOf(args);
    pid_t pid = 0;
    const int rc = ::posix_spawnp(&pid, argv[0], &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    ::close(fds[1]);
    if (rc != 0) {
        ::close(fds[0]);
        return false;
    }
    char buf[512];
    for (;;) {
        const ssize_t n = ::read(fds[0], buf, sizeof buf);
        if (n > 0) out.append(buf, std::size_t(n));
        else if (n == 0 || errno != EINTR) break;
    }
    ::close(fds[0]);
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return true;
}
/// Starts args[0] detached (double fork: no zombie, it outlives us). False when it is not in PATH.
bool runDetached(const std::vector<std::string>& args) {
    if (args.empty() || !findInPath(args[0])) return false;
    std::vector<char*> argv = argvOf(args); // built before the fork: the child only execs
    const pid_t pid = ::fork();
    if (pid < 0) return false;
    if (pid == 0) {
        if (::fork() == 0) {
            ::setsid();
            const int nul = ::open("/dev/null", O_RDWR);
            if (nul >= 0) {
                ::dup2(nul, 0);
                ::dup2(nul, 1);
            }
            ::execvp(argv[0], argv.data());
        }
        ::_exit(0);
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    return true;
}
} // namespace

bool openUrl(std::string_view url) { return !url.empty() && runDetached({"xdg-open", std::string(url)}); }
bool revealFile(std::string_view path) {
    if (path.empty()) return false;
    // no portable "select this file": open the folder that holds it
    const std::size_t slash = path.find_last_of('/');
    const std::string dir(slash == std::string_view::npos ? std::string_view(".") : slash == 0 ? std::string_view("/") : path.substr(0, slash));
    return runDetached({"xdg-open", dir});
}

// ---- X11: cursors ------------------------------------------------------------------------------------------------------
unsigned long NativePlatform::Impl::cursorFor(Cursor c) {
    const int i = int(c);
    if (i < 0 || i >= kCursorCount) return 0;
    if (cursors[i]) return cursors[i];
    // themed cursors by their CSS / freedesktop names (libXcursor), else the core cursor font (XC_* glyph index)
    static constexpr const char* kNames[kCursorCount] = {"default", "default", "text", "pointer", "ew-resize", "ns-resize", "nwse-resize",
                                                         "nesw-resize", "move", "crosshair", "not-allowed", "wait", nullptr};
    static constexpr unsigned kFont[kCursorCount] = {68 /*left_ptr*/, 68, 152 /*xterm*/, 60 /*hand2*/, 108 /*sb_h_double_arrow*/,
                                                     116 /*sb_v_double_arrow*/, 14 /*bottom_right_corner*/, 12 /*bottom_left_corner*/,
                                                     52 /*fleur*/, 34 /*crosshair*/, 0 /*X_cursor*/, 150 /*watch*/, 0};
    unsigned long id = 0;
    if (c == Cursor::Hidden) {
        if (x.CreateBitmapFromData && x.CreatePixmapCursor) { // a fully transparent 1x1 cursor
            const char zero = 0;
            const x11::XID pm = x.CreateBitmapFromData(dpy, x.RootWindow(dpy, x.DefaultScreen(dpy)), &zero, 1, 1);
            x11::Color col{};
            if (pm) id = x.CreatePixmapCursor(dpy, pm, pm, &col, &col, 0, 0);
            if (pm && x.FreePixmap) x.FreePixmap(dpy, pm);
        }
    } else {
        if (x.CursorLoad) id = x.CursorLoad(dpy, kNames[i]);
        if (!id && x.CreateFontCursor) id = x.CreateFontCursor(dpy, kFont[i]);
    }
    return cursors[i] = id;
}
void NativePlatform::Impl::applyCursor(Rec& r) {
    const Cursor want = r.win->ui().cursor();
    if (r.cursorApplied && want == r.cursor) return;
    if (!x.DefineCursor) return;
    const unsigned long id = cursorFor(want);
    if (!id) return; // the shape cannot be made here: keep what is shown
    x.DefineCursor(dpy, x11::Window(r.handle), id);
    x.Flush(dpy);
    r.cursor = want;
    r.cursorApplied = true;
}

// ---- X11: clipboard (CLIPBOARD selection, UTF8_STRING; no INCR, no clipboard-manager hand-over at exit) ---------------
bool NativePlatform::Impl::clipboardUsable() const {
    return dpy && helper && x.SetSelectionOwner && x.GetSelectionOwner && x.ConvertSelection && x.CheckTypedWindowEvent;
}
void NativePlatform::Impl::serveSelection(const x11::SelectionRequestEv& rq) {
    x11::Event out{};
    out.selection.type = x11::SelectionNotify;
    out.selection.display = dpy;
    out.selection.requestor = rq.requestor;
    out.selection.selection = rq.selection;
    out.selection.target = rq.target;
    out.selection.time = rq.time;
    out.selection.property = 0; // refused unless filled below
    const x11::Atom prop = rq.property ? rq.property : rq.target; // obsolete clients send None
    if (clipOwner && rq.selection == aClipboard) {
        if (rq.target == aTargets) {
            const unsigned long list[] = {aTargets, aUtf8, aTextPlain};
            x.ChangeProperty(dpy, rq.requestor, prop, 4 /*XA_ATOM*/, 32, 0, reinterpret_cast<const unsigned char*>(list), 3);
            out.selection.property = prop;
        } else if (rq.target == aUtf8 || rq.target == aTextPlain) {
            x.ChangeProperty(dpy, rq.requestor, prop, rq.target, 8, 0, reinterpret_cast<const unsigned char*>(clipText.data()),
                             int(clipText.size()));
            out.selection.property = prop;
        }
    }
    x.SendEvent(dpy, rq.requestor, 0, 0, &out);
    x.Flush(dpy);
}
bool NativePlatform::Impl::clipSet(void* user, std::string_view utf8) {
    Impl& m = *static_cast<Impl*>(user);
    m.clipText = utf8;
    m.x.SetSelectionOwner(m.dpy, m.aClipboard, m.helper, 0 /*CurrentTime*/);
    m.clipOwner = m.x.GetSelectionOwner(m.dpy, m.aClipboard) == m.helper;
    m.x.Flush(m.dpy);
    return m.clipOwner;
}
bool NativePlatform::Impl::clipGet(void* user, std::string& out) {
    Impl& m = *static_cast<Impl*>(user);
    auto& x = m.x;
    if (m.clipOwner) { // our own copy: no round trip
        out = m.clipText;
        return true;
    }
    if (!x.GetSelectionOwner(m.dpy, m.aClipboard)) return false; // nothing copied anywhere
    if (x.DeleteProperty) x.DeleteProperty(m.dpy, m.helper, m.aClipProp);
    x.ConvertSelection(m.dpy, m.aClipboard, m.aUtf8, m.aClipProp, m.helper, 0);
    x.Flush(m.dpy);
    x11::Event ev{};
    bool got = false;
    for (int waited = 0; waited < 1000 && !got; waited += 10) { // the owner answers with SelectionNotify
        got = x.CheckTypedWindowEvent(m.dpy, m.helper, x11::SelectionNotify, &ev) != 0;
        if (!got) {
            pollfd p{x.ConnectionNumber(m.dpy), POLLIN, 0};
            ::poll(&p, 1, 10);
        }
    }
    if (!got || !ev.selection.property) return false; // no answer / the owner has no text
    x11::Atom type = 0;
    int format = 0;
    unsigned long n = 0, after = 0;
    unsigned char* data = nullptr;
    if (x.GetWindowProperty(m.dpy, m.helper, m.aClipProp, 0, 1L << 24, 1 /*delete*/, 0 /*AnyPropertyType*/, &type, &format, &n, &after,
                            &data) != 0 || !data)
        return false;
    const bool text = format == 8 && type != m.aIncr; // INCR (very large transfers) is not implemented
    if (text) out.assign(reinterpret_cast<const char*>(data), std::size_t(n));
    x.Free(data);
    return text;
}

// ---- X11: window management (ICCCM / EWMH) ---------------------------------------------------------------------------
bool NativePlatform::Impl::windowLongs(x11::Window win, x11::Atom prop, x11::Atom type, std::vector<unsigned long>& out) const {
    out.clear();
    x11::Atom actual = 0;
    int format = 0;
    unsigned long n = 0, after = 0;
    unsigned char* data = nullptr;
    if (x.GetWindowProperty(dpy, win, prop, 0, 1024, 0, type, &actual, &format, &n, &after, &data) != 0 || !data) return false;
    if (format == 32) out.assign(reinterpret_cast<const unsigned long*>(data), reinterpret_cast<const unsigned long*>(data) + n);
    x.Free(data);
    return format == 32;
}
/// An EWMH window manager is running (maximise / fullscreen / minimise / activate are requests to it).
bool NativePlatform::Impl::hasWm() const {
    std::vector<unsigned long> v;
    return windowLongs(x.RootWindow(dpy, x.DefaultScreen(dpy)), aWmCheck, 33 /*XA_WINDOW*/, v) && !v.empty() && v[0] != 0;
}
WindowMode NativePlatform::Impl::readMode(const Rec& r) const {
    std::vector<unsigned long> atoms;
    if (!windowLongs(x11::Window(r.handle), aNetWmState, 4 /*XA_ATOM*/, atoms)) return r.win->mode() == WindowMode::Minimized ? WindowMode::Normal : r.win->mode();
    bool hidden = false, full = false, maxV = false, maxH = false;
    for (unsigned long a : atoms) {
        hidden = hidden || a == aHidden;
        full = full || a == aFullscreen;
        maxV = maxV || a == aMaxV;
        maxH = maxH || a == aMaxH;
    }
    return hidden ? WindowMode::Minimized : full ? WindowMode::Fullscreen : (maxV && maxH) ? WindowMode::Maximized : WindowMode::Normal;
}
bool NativePlatform::Impl::windowRequest(void* user, AppWindow& w, WindowRequest what) {
    Rec& r = *static_cast<Rec*>(user);
    Impl& m = *r.impl;
    if (!r.handle || !m.dpy) return false;
    auto& x = m.x;
    const x11::Window win = x11::Window(r.handle);
    const x11::Window root = x.RootWindow(m.dpy, x.DefaultScreen(m.dpy));
    auto toRoot = [&](x11::Atom type, long a, long b, long c, long d) { // a client message for the window manager
        x11::Event ev{};
        ev.client.type = x11::ClientMessage;
        ev.client.display = m.dpy;
        ev.client.window = win;
        ev.client.message_type = type;
        ev.client.format = 32;
        ev.client.l[0] = a; ev.client.l[1] = b; ev.client.l[2] = c; ev.client.l[3] = d;
        x.SendEvent(m.dpy, root, 0, (1L << 19) | (1L << 20) /*SubstructureNotify | SubstructureRedirect*/, &ev);
    };
    switch (what) {
    case WindowRequest::Title: {
        const std::string& t = w.title();
        x.StoreName(m.dpy, win, t.c_str());
        x.ChangeProperty(m.dpy, win, m.aNetWmName, m.aUtf8, 8, 0, reinterpret_cast<const unsigned char*>(t.data()), int(t.size()));
        break;
    }
    case WindowRequest::SizeLimits: { // WM_NORMAL_HINTS (XSizeHints as a property: 18 longs)
        long h[18] = {};
        const Size mn = w.minimumSize(), mx = w.maximumSize();
        auto px = [&](f32 v) { return long(std::max(1.f, std::round(v * r.scale))); };
        if (mn.w > 0.f || mn.h > 0.f) {
            h[0] |= 1L << 4; // PMinSize
            h[5] = mn.w > 0.f ? px(mn.w) : 1;
            h[6] = mn.h > 0.f ? px(mn.h) : 1;
        }
        if (mx.w > 0.f || mx.h > 0.f) {
            h[0] |= 1L << 5; // PMaxSize
            h[7] = mx.w > 0.f ? px(mx.w) : 32767;
            h[8] = mx.h > 0.f ? px(mx.h) : 32767;
        }
        x.ChangeProperty(m.dpy, win, 40 /*XA_WM_NORMAL_HINTS*/, 41 /*XA_WM_SIZE_HINTS*/, 32, 0, reinterpret_cast<const unsigned char*>(h), 18);
        // no window manager to enforce them: make the current size obey now
        const Size now = w.size(), want = w.clampSize(now);
        if (want.w != now.w || want.h != now.h) x.ResizeWindow(m.dpy, win, unsigned(px(want.w)), unsigned(px(want.h)));
        break;
    }
    case WindowRequest::Mode: {
        if (!m.hasWm()) return false; // maximise / fullscreen / minimise are the window manager's to do
        const WindowMode want = w.requestedMode();
        if (want == WindowMode::Minimized) {
            toRoot(m.aChangeState, 3 /*IconicState*/, 0, 0, 0);
            break;
        }
        if (w.mode() == WindowMode::Minimized) {
            x.MapWindow(m.dpy, win);
            toRoot(m.aActive, 1, 0, 0, 0);
        }
        toRoot(m.aNetWmState, want == WindowMode::Fullscreen ? 1 : 0, long(m.aFullscreen), 0, 1);
        toRoot(m.aNetWmState, want == WindowMode::Maximized ? 1 : 0, long(m.aMaxV), long(m.aMaxH), 1);
        break;
    }
    case WindowRequest::Visible:
        if (w.requestedVisible()) {
            x.MapWindow(m.dpy, win);
        } else {
            if (!x.UnmapWindow) return false;
            r.hideAsked = true;
            x.UnmapWindow(m.dpy, win);
        }
        break;
    case WindowRequest::Position: {
        if (!x.MoveWindow) return false;
        const Vec2 p = w.requestedPosition();
        x.MoveWindow(m.dpy, win, int(std::lround(p.x * r.scale)), int(std::lround(p.y * r.scale)));
        break;
    }
    case WindowRequest::Activate:
        if (x.RaiseWindow) x.RaiseWindow(m.dpy, win);
        if (m.hasWm()) toRoot(m.aActive, 1, 0, 0, 0);
        else if (x.SetInputFocus && r.mapped) x.SetInputFocus(m.dpy, win, 2 /*RevertToParent*/, 0);
        else return false;
        break;
    case WindowRequest::Menu: return false; // no global menu bar: AppWindow builds the in-window one
    }
    x.Flush(m.dpy);
    return true;
}

// ---- X11: theme, displays, file dialog -------------------------------------------------------------------------------
SystemTheme NativePlatform::systemTheme() {
    // X11 has no theme protocol of its own. GTK_THEME was read at start; the GNOME / portal setting is asked once
    // through gsettings when present. No change notification (XSETTINGS is not implemented): Unknown stays Unknown.
    if (m->dpy && !m->themeQueried) {
        m->themeQueried = true;
        if (m->app.systemTheme().scheme == ColorScheme::Unknown && findInPath("gsettings")) {
            std::string out;
            int code = -1;
            if (runCapture({"gsettings", "get", "org.gnome.desktop.interface", "color-scheme"}, out, code) && code == 0)
                if (const ColorScheme cs = schemeFromGsettings(out); cs != ColorScheme::Unknown) m->app.setSystemTheme({.scheme = cs});
        }
    }
    return m->app.systemTheme();
}
std::vector<DisplayInfo> NativePlatform::displays() const {
    std::vector<DisplayInfo> out;
    if (!m->dpy) return out;
    auto& x = m->x;
    const int scr = x.DefaultScreen(m->dpy);
    const x11::Window root = x.RootWindow(m->dpy, scr);
    const Rect all{0.f, 0.f, f32(x.DisplayWidth(m->dpy, scr)), f32(x.DisplayHeight(m->dpy, scr))};
    Rect work = all;
    std::vector<unsigned long> wa;
    if (m->windowLongs(root, m->aWorkArea, 6 /*XA_CARDINAL*/, wa) && wa.size() >= 4 && wa[2] > 0 && wa[3] > 0)
        work = {f32(long(wa[0])), f32(long(wa[1])), f32(wa[2]), f32(wa[3])};
    f32 hz = 0.f;
    if (x.RRGetScreenInfo && x.RRConfigCurrentRate && x.RRFreeScreenConfigInfo)
        if (void* conf = x.RRGetScreenInfo(m->dpy, root)) {
            hz = f32(x.RRConfigCurrentRate(conf));
            x.RRFreeScreenConfigInfo(conf);
        }
    auto intersect = [](Rect a, Rect b) {
        const f32 x0 = std::max(a.x, b.x), y0 = std::max(a.y, b.y), x1 = std::min(a.x + a.w, b.x + b.w), y1 = std::min(a.y + a.h, b.y + b.h);
        return x1 > x0 && y1 > y0 ? Rect{x0, y0, x1 - x0, y1 - y0} : a;
    };
    if (x.RRGetMonitors && x.RRFreeMonitors) {
        int n = 0;
        if (x11::RRMonitor* mons = x.RRGetMonitors(m->dpy, root, 1, &n)) {
            for (int i = 0; i < n; ++i) {
                DisplayInfo d;
                d.bounds = {f32(mons[i].x), f32(mons[i].y), f32(mons[i].width), f32(mons[i].height)};
                d.workArea = intersect(d.bounds, work);
                d.scale = m->detectedScale;
                d.refreshHz = hz;
                d.primary = mons[i].primary != 0;
                if (x.GetAtomName && mons[i].name)
                    if (char* name = x.GetAtomName(m->dpy, mons[i].name)) {
                        d.name = name;
                        x.Free(name);
                    }
                out.push_back(std::move(d));
            }
            x.RRFreeMonitors(mons);
        }
    }
    if (out.empty()) out.push_back({"screen", all, work, m->detectedScale, hz, true});
    return out;
}
FileDialogStatus NativePlatform::fileDialog(const AppWindow*, const FileDialogOptions& o, std::string& path) {
    path.clear();
    // the desktop's own dialog through its command-line helper; blocks until the user answers
    const char* xd = std::getenv("DISPLAY");
    const char* wd = std::getenv("WAYLAND_DISPLAY");
    if (!(xd && *xd) && !(wd && *wd)) return FileDialogStatus::Unavailable;
    for (const std::vector<std::string>& args : {zenityArgs(o), kdialogArgs(o)}) {
        if (!findInPath(args[0])) continue;
        std::string out;
        int code = -1;
        if (!runCapture(args, out, code)) continue;
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
        if (code != 0 || out.empty()) return FileDialogStatus::Cancelled;
        path = out;
        return FileDialogStatus::Accepted;
    }
    return FileDialogStatus::Unavailable;
}

#else
// ---- Win32 -------------------------------------------------------------------------------------------------------------
#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif
#ifndef WM_DWMCOLORIZATIONCOLORCHANGED
#define WM_DWMCOLORIZATIONCOLORCHANGED 0x0320
#endif
namespace {
std::wstring widen(std::string_view s) {
    std::wstring w(std::size_t(MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0)), L'\0');
    if (!w.empty()) MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), int(w.size()));
    return w;
}
std::string narrow(const wchar_t* w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(std::size_t(std::max(1, n) - 1), '\0');
    if (!s.empty()) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}
/// A user32 export that newer Windows versions have (per-monitor DPI): null on older ones.
template <class Fn>
Fn dllProc(const wchar_t* dll, const char* name) {
    HMODULE mod = LoadLibraryW(dll);
    return mod ? reinterpret_cast<Fn>(reinterpret_cast<void (*)()>(GetProcAddress(mod, name))) : nullptr;
}
UINT dpiOf(HWND hwnd) {
    using Fn = UINT(WINAPI*)(HWND);
    static const Fn fn = dllProc<Fn>(L"user32.dll", "GetDpiForWindow"); // Windows 10 1607+
    if (fn)
        if (const UINT d = fn(hwnd)) return d;
    HDC dc = GetDC(hwnd);
    const int d = GetDeviceCaps(dc, LOGPIXELSX); // the system DPI
    ReleaseDC(hwnd, dc);
    return d > 0 ? UINT(d) : 96u;
}
Rect rectOf(const RECT& r) { return {f32(r.left), f32(r.top), f32(r.right - r.left), f32(r.bottom - r.top)}; }
BOOL CALLBACK monitorProc(HMONITOR mon, HDC, LPRECT, LPARAM lp) {
    auto& out = *reinterpret_cast<std::vector<DisplayInfo>*>(lp);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof mi;
    if (!GetMonitorInfoW(mon, reinterpret_cast<MONITORINFO*>(&mi))) return TRUE;
    DisplayInfo d;
    d.name = narrow(mi.szDevice);
    d.bounds = rectOf(mi.rcMonitor);
    d.workArea = rectOf(mi.rcWork);
    d.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
    DEVMODEW dm{};
    dm.dmSize = sizeof dm;
    if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1) d.refreshHz = f32(dm.dmDisplayFrequency);
    using Fn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    static const Fn fn = dllProc<Fn>(L"shcore.dll", "GetDpiForMonitor"); // Windows 8.1+
    UINT dx = 96, dy = 96;
    if (fn && fn(mon, 0 /*MDT_EFFECTIVE_DPI*/, &dx, &dy) == S_OK) d.scale = dpi::scaleFromDpi(f32(dx));
    out.push_back(std::move(d));
    return TRUE;
}
} // namespace

void NativePlatform::Impl::initServices() {
    // per-monitor DPI awareness, unless the manifest / host already chose (the call then fails harmlessly)
    using Fn = BOOL(WINAPI*)(HANDLE);
    if (const Fn fn = dllProc<Fn>(L"user32.dll", "SetProcessDpiAwarenessContext")) fn(reinterpret_cast<HANDLE>(std::intptr_t(-4)));
    app.setSystemTheme(queryTheme());
}
f32 NativePlatform::Impl::windowScale(HWND hwnd) const { return dpi::resolveScale(opts.scale, dpi::scaleFromDpi(f32(dpiOf(hwnd)))); }

void NativePlatform::Impl::applyCursor(Rec& r) {
    static const WORD kIds[kCursorCount] = {32512 /*IDC_ARROW*/, 32512, 32513 /*IBEAM*/, 32649 /*HAND*/, 32644 /*SIZEWE*/, 32645 /*SIZENS*/,
                                            32642 /*SIZENWSE*/, 32643 /*SIZENESW*/, 32646 /*SIZEALL*/, 32515 /*CROSS*/, 32648 /*NO*/,
                                            32514 /*WAIT*/, 0};
    const Cursor want = r.win->ui().cursor();
    const int i = int(want);
    if (want != Cursor::Hidden && !cursors[i]) cursors[i] = LoadCursorW(nullptr, MAKEINTRESOURCEW(kIds[i]));
    SetCursor(want == Cursor::Hidden ? nullptr : cursors[i]);
    r.cursor = want;
    r.cursorApplied = true;
}

bool NativePlatform::Impl::clipboardUsable() const { return true; }
bool NativePlatform::Impl::clipSet(void*, std::string_view utf8) {
    const std::wstring w = widen(utf8);
    if (!OpenClipboard(nullptr)) return false;
    EmptyClipboard();
    bool ok = false;
    const std::size_t bytes = (w.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        if (void* p = GlobalLock(h)) {
            std::memcpy(p, w.c_str(), bytes);
            GlobalUnlock(h);
            ok = SetClipboardData(CF_UNICODETEXT, h) != nullptr; // the system owns the block now
        }
        if (!ok) GlobalFree(h);
    }
    CloseClipboard();
    return ok;
}
bool NativePlatform::Impl::clipGet(void*, std::string& out) {
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !OpenClipboard(nullptr)) return false;
    bool ok = false;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT))
        if (const auto* p = static_cast<const wchar_t*>(GlobalLock(h))) {
            out = narrow(p);
            std::erase(out, '\r'); // CRLF -> LF
            GlobalUnlock(h);
            ok = true;
        }
    CloseClipboard();
    return ok;
}

void NativePlatform::Impl::leaveFullscreen(Rec& r) {
    HWND hwnd = reinterpret_cast<HWND>(r.handle);
    r.fullscreen = false;
    SetWindowLongW(hwnd, GWL_STYLE, r.restoreStyle);
    SetWindowPlacement(hwnd, &r.restorePlacement);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}
bool NativePlatform::Impl::windowRequest(void* user, AppWindow& w, WindowRequest what) {
    Rec& r = *static_cast<Rec*>(user);
    HWND hwnd = reinterpret_cast<HWND>(r.handle);
    if (!hwnd) return false;
    switch (what) {
    case WindowRequest::Title: return SetWindowTextW(hwnd, widen(w.title()).c_str()) != 0;
    case WindowRequest::SizeLimits: { // WM_GETMINMAXINFO reads the limits; re-apply the size so they hold now
        RECT rc;
        if (!GetWindowRect(hwnd, &rc)) return false;
        SetWindowPos(hwnd, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        return true;
    }
    case WindowRequest::Mode: {
        const WindowMode want = w.requestedMode();
        if (want == WindowMode::Fullscreen) { // borderless over the whole monitor
            if (r.fullscreen) return true;
            MONITORINFO mi{};
            mi.cbSize = sizeof mi;
            if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) return false;
            r.restoreStyle = GetWindowLongW(hwnd, GWL_STYLE);
            r.restorePlacement.length = sizeof(WINDOWPLACEMENT);
            GetWindowPlacement(hwnd, &r.restorePlacement);
            r.fullscreen = true;
            SetWindowLongW(hwnd, GWL_STYLE, (r.restoreStyle & ~LONG(WS_OVERLAPPEDWINDOW)) | LONG(WS_POPUP));
            SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
            ShowWindow(hwnd, SW_SHOW);
            w.notifyMode(WindowMode::Fullscreen);
            return true;
        }
        if (r.fullscreen) r.impl->leaveFullscreen(r);
        ShowWindow(hwnd, want == WindowMode::Minimized ? SW_MINIMIZE : want == WindowMode::Maximized ? SW_MAXIMIZE : SW_SHOWNORMAL);
        // WM_SIZE reports it too, but not when the size did not change
        w.notifyMode(IsIconic(hwnd) ? WindowMode::Minimized : IsZoomed(hwnd) ? WindowMode::Maximized : WindowMode::Normal);
        return true;
    }
    case WindowRequest::Visible:
        ShowWindow(hwnd, w.requestedVisible() ? SW_SHOW : SW_HIDE); // WM_SHOWWINDOW reports it
        return true;
    case WindowRequest::Position: {
        POINT o{0, 0};
        RECT wr;
        if (!ClientToScreen(hwnd, &o) || !GetWindowRect(hwnd, &wr)) return false;
        const Vec2 p = w.requestedPosition(); // of the client area
        return SetWindowPos(hwnd, nullptr, int(std::lround(p.x * r.scale)) - int(o.x - wr.left), int(std::lround(p.y * r.scale)) - int(o.y - wr.top),
                            0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE) != 0;
    }
    case WindowRequest::Activate:
        if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
        BringWindowToTop(hwnd);
        return SetForegroundWindow(hwnd) != 0;
    case WindowRequest::Menu: return false; // AppWindow builds the in-window menu bar
    }
    return false;
}

SystemTheme NativePlatform::Impl::queryTheme() const {
    SystemTheme t;
    DWORD v = 0, n = sizeof v;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme",
                     RRF_RT_REG_DWORD, nullptr, &v, &n) == ERROR_SUCCESS)
        t.scheme = v ? ColorScheme::Light : ColorScheme::Dark;
    n = sizeof v;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", RRF_RT_REG_DWORD, nullptr, &v, &n) == ERROR_SUCCESS) {
        t.hasAccent = true; // 0xAABBGGRR
        t.accent = {f32(v & 0xFF) / 255.f, f32((v >> 8) & 0xFF) / 255.f, f32((v >> 16) & 0xFF) / 255.f, 1.f};
    }
    return t;
}
SystemTheme NativePlatform::systemTheme() {
    m->app.setSystemTheme(m->queryTheme());
    return m->app.systemTheme();
}
std::vector<DisplayInfo> NativePlatform::displays() const {
    std::vector<DisplayInfo> out;
    EnumDisplayMonitors(nullptr, nullptr, &monitorProc, reinterpret_cast<LPARAM>(&out));
    return out;
}
FileDialogStatus NativePlatform::fileDialog(const AppWindow* parent, const FileDialogOptions& o, std::string& path) {
    path.clear();
    HWND owner = parent ? reinterpret_cast<HWND>(nativeWindow(*parent)) : nullptr;
    const std::wstring title = widen(o.title), dir = widen(o.directory);
    if (o.pickDirectory) {
        BROWSEINFOW bi{};
        bi.hwndOwner = owner;
        bi.lpszTitle = title.c_str();
        bi.ulFlags = BIF_RETURNONLYFSDIRS;
        const auto id = SHBrowseForFolderW(&bi);
        if (!id) return FileDialogStatus::Cancelled;
        wchar_t buf[MAX_PATH] = {};
        const bool got = SHGetPathFromIDListW(id, buf) != 0;
        CoTaskMemFree(id);
        if (!got) return FileDialogStatus::Cancelled;
        path = narrow(buf);
        return FileDialogStatus::Accepted;
    }
    std::wstring filter; // "Images\0*.png;*.jpg\0All files\0*.*\0\0"
    if (!o.filter.empty()) {
        filter += widen(o.filterName.empty() ? o.filter : o.filterName);
        filter.push_back(L'\0');
        filter += widen(o.filter);
        filter.push_back(L'\0');
    }
    filter += L"All files";
    filter.push_back(L'\0');
    filter += L"*.*";
    filter.push_back(L'\0');
    std::vector<wchar_t> file(32768, L'\0');
    const std::wstring def = widen(o.defaultName);
    if (o.save && def.size() + 1 < file.size()) std::copy(def.begin(), def.end(), file.begin());
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = file.data();
    ofn.nMaxFile = DWORD(file.size());
    ofn.lpstrTitle = title.c_str();
    ofn.lpstrInitialDir = dir.empty() ? nullptr : dir.c_str();
    ofn.Flags = OFN_NOCHANGEDIR | OFN_HIDEREADONLY | OFN_EXPLORER | (o.save ? OFN_OVERWRITEPROMPT : (OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST));
    if (!(o.save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn))) return FileDialogStatus::Cancelled;
    path = narrow(file.data());
    return FileDialogStatus::Accepted;
}
bool openUrl(std::string_view url) {
    return !url.empty() &&
           reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
}
bool revealFile(std::string_view path) {
    if (path.empty()) return false;
    std::wstring p = widen(path);
    std::replace(p.begin(), p.end(), L'/', L'\\');
    const std::wstring args = L"/select,\"" + p + L"\"";
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL)) > 32;
}
#endif
} // namespace fuse::app
