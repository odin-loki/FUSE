// fuse_app Wayland client (Linux / FreeBSD): libwayland-client, libxkbcommon and libwayland-cursor are loaded at
// runtime (dlopen), like libX11 on the X11 path, so neither the Wayland headers nor the libraries are needed to build.
// The protocol tables of the extensions used (xdg-shell, xdg-decoration, viewporter, fractional-scale, cursor-shape,
// text-input-v3, xdg-toplevel-icon, primary-selection) are written out in native_wayland.cpp instead of being generated
// by wayland-scanner (the wire format needs names, signatures and opcodes only; see THIRD_PARTY.md for the protocol
// licences). The core protocol tables (wl_surface, wl_seat ...) are data symbols of libwayland-client itself.
//
// This file is the protocol layer; NativePlatform's Wayland backend on top of it is native_wayland_platform.cpp.
//
// What it does: xdg_toplevel windows, wl_shm software present (the CPU raster) throttled by wl_surface.frame, the
// wl_surface handed to the Vulkan presenter (VK_KHR_wayland_surface), wl_pointer (buttons, discrete + smooth axis),
// wl_keyboard (xkbcommon keymaps, key repeat at the compositor's rate; a US evdev table without xkbcommon), wl_touch,
// output scale (wp_fractional_scale_v1 preferred scale + wp_viewporter destination when offered, else the integer
// wl_output.scale -> wl_surface.set_buffer_scale), xdg-decoration server-side frames when the compositor offers them,
// cursors (wp_cursor_shape_v1 when offered, else libwayland-cursor theme cursors), maximise / minimise / fullscreen /
// size limits / title through xdg_toplevel, interactive move / resize (beginMove / beginResize, for a client-side
// frame), window icons (xdg_toplevel_icon_v1 when offered). Child surfaces: xdg_popups (nested for submenus) placed
// by an xdg_positioner relative to their parent and grabbed with the serial of the input that opened them, and child
// xdg_toplevels (set_parent) for tool windows. The clipboard (wl_data_device; zwp_primary_selection_v1 for the
// primary selection), drops from other clients (wl_data_device enter / motion / drop), IME pre-edit / commit
// (zwp_text_input_v3 -> UiContext::textComposition / textInput).
#pragma once

#if !defined(_WIN32) && !defined(__ANDROID__) && !defined(__APPLE__)
#include "fuse/app/app.hpp"
#include "fuse/app/native.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace fuse::app::wl {

struct Backend;
struct Window;

/// Which window system NativePlatform should try first. `option` (NativeOptions::backend) wins when not Auto, then
/// `env` ($FUSE_UI_BACKEND = "x11" | "wayland", anything else is ignored), then Wayland when `waylandDisplay`
/// ($WAYLAND_DISPLAY) is set, else X11. Wayland is only a first choice: without libwayland-client or a compositor
/// NativePlatform falls back to X11 unless Wayland was forced.
NativeBackend chooseBackend(NativeBackend option, std::string_view env, std::string_view waylandDisplay);
/// True when `option` / `env` name a backend explicitly (no fallback to the other one then).
bool backendForced(NativeBackend option, std::string_view env);

/// dlopens libwayland-client and connects ($WAYLAND_DISPLAY); null (and `err`) without the library, a compositor or
/// wl_compositor / wl_shm / xdg_wm_base. `scaleOption` is NativeOptions::scale (> 0: every window uses it; 0: follow
/// $FUSE_UI_SCALE, else the outputs / the compositor's preferred fractional scale).
Backend* open(std::string& err, fuse::ui::f32 scaleOption);
void close(Backend* b);
/// The open backend (one per process), null when the NativePlatform runs on X11 / headless.
Backend* current();

int fd(const Backend* b);
void* display(const Backend* b); ///< wl_display*
/// Event pump halves: before poll() (prepare_read + flush; returns the connection fd), after it (read or cancel,
/// dispatch). Key repeat and overdue frame callbacks run in after().
int beforePoll(Backend* b);
void afterPoll(Backend* b, bool readable);
/// Seconds until the next key repeat / frame-callback deadline (inf when none is due).
fuse::ui::f32 nextTimeout(const Backend* b);
/// Blocks until the compositor answered everything sent so far.
bool roundtrip(Backend* b);
/// False once the connection broke (compositor gone / protocol error).
bool alive(const Backend* b);

/// An xdg_toplevel for `w`: title, the scale of the output(s) known, waits for the first configure. The window's
/// AppWindow follows the compositor from then on (resize, setScale, notifyMode, notifyActive, tryClose).
Window* createWindow(Backend* b, AppWindow& w, std::string& err);
void destroyWindow(Backend* b, Window* win);
void* surface(const Window* win); ///< wl_surface*
Window* findWindow(const Backend* b, const AppWindow& w);
/// Frame throttle of the software presenter: false while the wl_surface.frame callback of the last commit is
/// outstanding (the compositor has not shown it yet). The window is then asked to redraw when the callback arrives,
/// or after 100 ms without one (an occluded surface gets none). Call before rasterising.
bool frameReady(Backend* b, Window* win);
/// The software presenter: the window's whole CPU raster into a wl_shm buffer, attached, damaged, committed with a
/// wl_surface.frame request. False when no buffer is free (the window is asked to redraw).
bool presentShm(Backend* b, Window* win, const fuse::ui::Image& img);
/// Before a Vulkan present: sends a changed buffer scale / viewport (they apply with the swapchain's commit).
void beforeVulkanPresent(Backend* b, Window* win);
/// The last frame committed through presentShm (what the compositor shows for a software window).
bool capture(const Window* win, fuse::ui::Image& out);
/// Shows the cursor the window's UI wants now (while the pointer is over it).
void syncCursor(Backend* b, Window* win);
fuse::ui::Cursor cursorShown(const Window* win); ///< Arrow until a pointer entered
/// The client picks a toplevel's size on Wayland: resizes the window (logical units) and redraws.
void resize(Backend* b, Window* win, unsigned width, unsigned height);
std::vector<DisplayInfo> displays(const Backend* b);
int bufferScale(const Window* win);
unsigned long long serialOf(const Window* win); ///< configures acknowledged (tests)
unsigned long long framesThrottled(const Window* win); ///< presentShm calls deferred by an outstanding frame callback

/// True when the window follows wp_fractional_scale_v1 (buffer through a wp_viewport destination).
bool fractional(const Window* win);
void setTitle(Backend* b, Window* win, const std::string& title);
/// xdg_toplevel set_min_size / set_max_size (logical units; 0 = no limit).
void setSizeLimits(Backend* b, Window* win, fuse::ui::Size min, fuse::ui::Size max);
/// xdg_toplevel requests. Minimized is one way: the protocol has no "minimized" state and no un-minimise request.
void setMode(Backend* b, Window* win, WindowMode mode);
WindowMode mode(const Window* win);     ///< as last configured (Normal / Maximized / Fullscreen)
/// Hide: unmaps the surface (a null buffer). Show: maps it again (title / limits re-sent, waits for the configure);
/// the compositor places it anew. False when the compositor does not configure it.
bool setVisible(Backend* b, Window* win, bool on);
bool mapped(const Window* win);
bool activated(const Window* win);      ///< xdg_toplevel "activated" state
bool keyboardFocus(const Window* win);  ///< wl_keyboard focus
bool serverDecorated(const Window* win); ///< xdg-decoration answered "server side"
/// Hands a press in progress to the compositor: interactive move / resize (`edge`: xdg_toplevel.resize_edge, 1 top,
/// 2 bottom, 4 left, 8 right and their sums). False without a seat or a press serial. For client-side frames.
bool beginMove(Backend* b, Window* win);
bool beginResize(Backend* b, Window* win, unsigned edge);
/// Client-side chrome (chrome.hpp, WindowFrame::Borderless / Custom): a left press on the caption or a resize edge is
/// handed to the compositor through beginMove / beginResize instead of reaching the UI; a caption double click
/// toggles maximise. How many presses went each way (tests).
unsigned chromeMoves(const Window* win);
unsigned chromeResizes(const Window* win);
/// The size (px) theme cursors are loaded at ($XCURSOR_SIZE, else 24); 0 when the compositor draws the cursors
/// (wp_cursor_shape_v1) or libwayland-cursor is missing.
int cursorThemeSize(const Backend* b);
/// xdg_toplevel_icon_v1 (square RGBA rasters): false when the compositor has no icon manager.
bool setIcon(Backend* b, Window* win, const std::vector<const fuse::ui::Image*>& sizes);
std::vector<int> iconSizes(const Window* win);
/// The cursor last sent: wp_cursor_shape_v1 shape (> 0) or 0 when it came from the cursor theme / nothing.
unsigned cursorShapeSent(const Window* win);

// ---- child surfaces: OS popups (xdg_popup) and tool windows (child xdg_toplevel) ----
struct Child;
/// Input on a child surface reaches the owner window's UI at `origin` + surface-local position (UI units).
/// Popups: `anchor` is the popup's top-left relative to `parent`'s (or the owner window's) top-left, `size` its logical
/// size; grabbed with the serial of the last button / key press when there is one. Waits for the first configure.
Child* createPopup(Backend* b, Window* owner, Child* parent, fuse::ui::Vec2 anchor, fuse::ui::Size size, fuse::ui::Vec2 origin,
                   std::string& err);
/// A tool window: xdg_toplevel with set_parent(owner), its own size (min = max = size, so tiling shells float it).
Child* createTool(Backend* b, Window* owner, fuse::ui::Size size, fuse::ui::Vec2 origin, const std::string& title, std::string& err);
void destroyChild(Backend* b, Child* c);
void setChildOrigin(Child* c, fuse::ui::Vec2 origin);
/// Tool windows: new logical size (the next present has it).
void resizeChild(Backend* b, Child* c, fuse::ui::Size size);
/// `img` is device pixels (size * the owner's scale).
void presentChild(Backend* b, Child* c, const fuse::ui::Image& img);
bool captureChild(const Child* c, fuse::ui::Image& out);
/// Popups: where the compositor placed it, logical, relative to the owner window (nested popups accumulated).
fuse::ui::Rect childPlacement(const Child* c);
bool childGrabbed(const Child* c);
/// The compositor dismissed the popup (popup_done: an outside click, or the grab could not be taken).
bool childDismissed(const Child* c);
/// Tool windows: a press on the surface is in progress (its serial can start xdg_toplevel.move).
bool childPressed(const Child* c);
/// Hands an in-progress press-drag on a tool window to the compositor (xdg_toplevel.move with the press serial).
bool moveChild(Backend* b, Child* c);
/// Called from the event dispatch when a popup is dismissed by the compositor.
void setDismissHandler(Backend* b, void (*fn)(void* user, Window* owner), void* user);

// ---- clipboard (wl_data_device) and primary selection (zwp_primary_selection_v1) ----
/// Provides the bytes of `mime` for a paste (called on the UI thread when another client asks).
using SelectionProvider = bool (*)(void* user, const std::string& mime, std::vector<unsigned char>& out);
bool hasDataDevice(const Backend* b, bool primary);
/// Offers `mimes` as the selection (serial: the last input event). False without a data device / seat.
bool setSelection(Backend* b, bool primary, const std::vector<std::string>& mimes, SelectionProvider fn, void* user);
/// True while our source is the selection (no cancelled event since).
bool ownsSelection(const Backend* b, bool primary);
/// MIME types of the current selection offer (another client's, or ours).
std::vector<std::string> selectionMimes(Backend* b, bool primary);
/// Reads the selection in `mime` (receive + pipe, up to `ms` milliseconds).
bool receiveSelection(Backend* b, bool primary, const std::string& mime, std::vector<unsigned char>& out, int ms = 2000);

// ---- drag and drop from other clients (wl_data_device enter / motion / leave / drop) ----
/// `phase` 0: the drag moved over `win` at `at` (enter / motion; `data` empty), 1: it left, 2: dropped (`data` = the
/// payload in `mime`: text/uri-list, else UTF-8 text).
using DropHandler = void (*)(void* user, Window* win, int phase, fuse::ui::Vec2 at, const std::string& mime, const std::string& data);
void setDropHandler(Backend* b, DropHandler fn, void* user);
/// Window under the drag, if any (tests).
bool dragOver(const Window* win);
/// "file:///a%20b\r\nfile:///c\r\n" -> "/a b\n/c" (comment lines and non-file URIs are dropped).
std::string uriListToPaths(std::string_view uriList);

// ---- IME (zwp_text_input_v3) ----
bool hasTextInput(const Backend* b);
/// enable (caret rect in logical units) / disable the text-input for the focused window; pre-edit / commit arrive in
/// the window's UI. Returns whether the text-input is enabled for `win`.
bool syncTextInput(Backend* b, Window* win, bool want, fuse::ui::Rect caret);

/// Input injection for tests (Wayland has no way to post events to another client's surface): delivered as if the
/// compositor had sent them. `at` in device pixels; kind 0 move, 1 left down, 2 left up.
void injectPointer(Backend* b, Window* win, fuse::ui::Vec2 at, int kind);
void injectChildPointer(Backend* b, Child* c, fuse::ui::Vec2 at, int kind);
/// Delivers a key press + release by Linux evdev code (KEY_A = 30 ...) through the keymap path.
void injectKey(Backend* b, Window* win, unsigned evdevCode);
void injectClose(Window* win); ///< xdg_toplevel.close
/// The globals the compositor announced (tests): "wp_fractional_scale_manager_v1", "zwp_text_input_manager_v3", ...
bool hasGlobal(const Backend* b, const char* iface);

/// The hand-written wl_interface table of an extension interface by name ("xdg_wm_base", "xdg_toplevel",
/// "wp_viewport" ...), null when unknown. Null for everything until libwayland-client was loaded (the tables name
/// core interfaces). The in-process test compositor serves the same tables through libwayland-server.
const void* protocolInterface(std::string_view name);
/// Loads libwayland-client (no connection) so protocolInterface works; false (and `err`) when it is not installed.
bool loadLibrary(std::string& err);

/// No-compositor checks for the fuse_app_wayland gate: libraries resolve, protocol tables are consistent,
/// xkbcommon turns evdev codes into the expected keysyms / text. Empty string = pass, else what failed.
std::string selfTest();
/// Wheel notches of one wl_pointer frame: per axis the `steps` (wl_pointer.axis_discrete) when the frame carried
/// them, else the smooth `value` (wl_fixed as float, surface units): 10 units per notch for a wheel, 40 for finger /
/// continuous sources (`continuous`; a notch scrolls 40 px, so the content follows the fingers 1:1).
/// Positive Wayland values scroll down / right; UiContext::wheel wants +y = up, +x = right.
fuse::ui::Vec2 wheelNotches(fuse::ui::Vec2 value, fuse::ui::Vec2 steps, bool hasStepsX, bool hasStepsY, bool continuous);

} // namespace fuse::app::wl
#endif
