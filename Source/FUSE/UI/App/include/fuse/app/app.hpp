// fuse_app (plan UI5): the application shell for UI-only projects. An Application owns AppWindows (each a UiContext
// plus a presenter), command-line arguments, a settings store and the single-instance lock, and runs an on-demand
// loop: it sleeps until input, a UI timer or an animation needs a frame (UiContext::nextWakeSeconds), so an idle app
// presents nothing. There is no second loop: an engine host calls step() from its frame instead of run().
//
// Backends are function tables (no inheritance): an EventSource pumps OS input into the windows and blocks for at
// most the wait it is given; a Presenter shows a window's draw list. The built-in headless backend rasterises on the
// CPU into the window's image (tests, screenshots, servers); desktop / mobile presenters plug in the same way.
#pragma once

#include "fuse/app/services.hpp"
#include "fuse/ui/ui.hpp"
#include "fuse/ui/widgets/widgets.hpp"

#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fuse::ui::doc {
class Document;
class Model;
} // namespace fuse::ui::doc

namespace fuse::app {

using fuse::ui::f32;
using fuse::ui::u32;
using fuse::ui::u64;

// ---- command line --------------------------------------------------------------------------------------------------
/// `--key=value`, `--key value` is NOT split (ambiguous); `--flag` = "1"; `--` ends options; everything else positional.
class CommandLine {
public:
    CommandLine() = default;
    CommandLine(int argc, const char* const* argv);
    bool has(std::string_view key) const;
    std::string_view get(std::string_view key, std::string_view fallback = {}) const;
    double number(std::string_view key, double fallback) const;
    const std::vector<std::string>& positional() const { return m_positional; }
    std::string_view program() const { return m_program; }

private:
    std::string m_program;
    std::vector<std::pair<std::string, std::string>> m_options;
    std::vector<std::string> m_positional;
};

// ---- settings ------------------------------------------------------------------------------------------------------
/// Flat key = value store persisted as UTF-8 text (one `key=value` per line, '\n' and '\\' escaped). Saved by save()
/// and when the Application exits, only if something changed.
class Settings {
public:
    void setPath(std::string path) { m_path = std::move(path); }
    const std::string& path() const { return m_path; }
    bool load();       ///< false when the file is missing or unreadable (the store is then empty)
    bool save();       ///< writes atomically (temp + rename); true when clean or written
    bool dirty() const { return m_dirty; }
    std::string_view get(std::string_view key, std::string_view fallback = {}) const;
    double number(std::string_view key, double fallback = 0.0) const;
    bool flag(std::string_view key, bool fallback = false) const { return number(key, fallback ? 1.0 : 0.0) != 0.0; }
    void set(std::string_view key, std::string_view value);
    void set(std::string_view key, double value);
    void remove(std::string_view key);
    u32 size() const { return u32(m_items.size()); }
    const std::vector<std::pair<std::string, std::string>>& items() const { return m_items; }
    std::string toText() const;           ///< the `key=value` lines save() writes
    void fromText(std::string_view text); ///< replaces the store with the parsed lines (left clean)
    /// Platform config directory for an app: <dataRoot()>/<app> when a data root is set (mobile), else
    /// $XDG_CONFIG_HOME/<app> (~/.config/<app>), ~/Library/Application Support/<app> (macOS), %APPDATA%\<app>.
    static std::string defaultDir(std::string_view app);

private:
    std::vector<std::pair<std::string, std::string>> m_items;
    std::string m_path;
    bool m_dirty = false;
};

// ---- windows -------------------------------------------------------------------------------------------------------
struct WindowDesc {
    std::string_view title = "FUSE";
    u32 width = 800, height = 600;
    /// Render scale (HiDPI): device pixels per logical unit. `width` / `height` are logical; the window's image is
    /// width x scale by height x scale pixels. NativePlatform::show replaces it with the display's scale on macOS /
    /// iOS / Android (see NativeOptions::scale).
    f32 dpiScale = 1.f;
    /// OS frame (services.hpp WindowFrame). Borderless / Custom windows draw their own title bar (chrome.hpp).
    WindowFrame frame = WindowFrame::System;
    f32 resizeBorder = 6.f; ///< Custom frame: thickness of the resize edges (logical px)
};
enum class RedrawPolicy : fuse::ui::u8 {
    OnDemand,   ///< update + present only when the UI needs it (default)
    Continuous, ///< every loop iteration (games, live viewports)
};

class Application;
class AppWindow;

/// Shows a window's frame. `damage` is the union of changed rects in device pixels of AppWindow::pixelSize() (==
/// logical units at scale 1); the whole window on the first frame / resize.
struct Presenter {
    void* user = nullptr;
    bool (*present)(void* user, AppWindow& w, fuse::ui::Rect damage) = nullptr;
    void (*destroy)(void* user) = nullptr; ///< called when the window closes
    /// True when present() waits for the display (a FIFO swapchain): the display paces the window, and
    /// AppWindow::setMaxFrameRate is not applied.
    bool vsynced = false;
};

class AppWindow {
public:
    AppWindow(Application& app, const WindowDesc& d, u32 id);
    ~AppWindow();
    AppWindow(const AppWindow&) = delete;
    AppWindow& operator=(const AppWindow&) = delete;

    fuse::ui::UiContext& ui() { return m_ui; }
    const fuse::ui::UiContext& ui() const { return m_ui; }
    Application& app() { return m_app; }
    u32 id() const { return m_id; }
    const std::string& title() const { return m_title; }
    void setTitle(std::string_view t); ///< also retitles the OS window when shown
    /// Loads a .fuseui document (file path) into the window root, replacing a previous one. `model` may be null.
    std::expected<void, std::string> load(std::string_view path, fuse::ui::doc::Model* model = nullptr);
    std::expected<void, std::string> loadText(std::string_view json, fuse::ui::doc::Model* model = nullptr);
    fuse::ui::doc::Document* document() { return m_doc.get(); }

    void resize(u32 width, u32 height); ///< logical units (pixelSize() = size x scale, rounded); forces a full repaint
    fuse::ui::Size size() const { return m_ui.viewport(); } ///< logical units
    /// Render scale (HiDPI): device pixels per logical unit (WindowDesc::dpiScale, default 1). size(), resize(), input
    /// positions and every UiContext coordinate stay logical; image() / rasterize() / writePng() and the native
    /// presenters work at pixelSize() = size() x scale, and text is rasterised at that resolution
    /// (UiContext::setRenderScale). Forces a full repaint.
    void setScale(f32 scale);
    f32 scale() const { return m_ui.renderScale(); }
    /// The frame's size in device pixels.
    fuse::ui::Size pixelSize() const { return {f32(m_pixelW), f32(m_pixelH)}; }
    /// Native backends: the surface is exactly `pixelWidth` x `pixelHeight` device pixels at `scale`; the logical size
    /// becomes pixels / scale (fractional when the scale does not divide the surface). Forces a full repaint.
    void resizePixels(u32 pixelWidth, u32 pixelHeight, f32 scale);
    void setRedrawPolicy(RedrawPolicy p) { m_policy = p; }
    RedrawPolicy redrawPolicy() const { return m_policy; }
    void setPresenter(const Presenter& p);
    /// Frames presented so far (the idle gate checks this stays flat).
    u64 framesPresented() const { return m_frames; }
    /// Frame pacing for presenters that do not wait for the display (Presenter::vsynced false): at most `hz` frames
    /// per second on the app clock; a Continuous window then sleeps between frames instead of spinning. 0 (the
    /// default) = unlimited. A forced redraw (resize, requestRedraw) is paced too.
    void setMaxFrameRate(f32 hz) { m_maxRate = hz > 0.f ? hz : 0.f; }
    f32 maxFrameRate() const { return m_maxRate; }
    /// Closes unconditionally (programmatic). The user's close (title-bar button, Alt+F4, Cmd+W) goes through
    /// tryClose(), which asks the close guard first.
    void requestClose() { m_closing = true; }
    bool closing() const { return m_closing; }
    void requestRedraw() { m_forceRedraw = true; }
    /// Headless presenter target: the last CPU-rasterised frame, pixelSize() pixels.
    const fuse::ui::Image& image() const { return m_image; }
    bool writePng(const std::string& path) const;
    /// Registers an app-owned image with the CPU rasteriser (viewportWidget / image index). The image must outlive the
    /// window; call ui().invalidate(node) after changing its pixels.
    u32 addImage(const fuse::ui::Image* img) { return m_raster.addImage(img); }
    const fuse::ui::CpuRasterizer& rasterizer() const { return m_raster; }
    /// CPU-rasterises `damage` (device pixels; whole window when empty / resized) into image(): the headless presenter,
    /// also used by native presenters that upload the CPU raster.
    const fuse::ui::Image& rasterize(fuse::ui::Rect damage) {
        presentHeadless(nullptr, *this, damage);
        return m_image;
    }

    // input (backends call these; they forward to the UiContext and wake the loop)
    void pointerMove(fuse::ui::Vec2 p) { m_ui.pointerMove(p); }
    void pointerDown(fuse::ui::Vec2 p) { m_ui.pointerDown(p); }
    void pointerUp(fuse::ui::Vec2 p) { m_ui.pointerUp(p); }
    void click(fuse::ui::Vec2 p) { m_ui.pointerMove(p); m_ui.pointerDown(p); m_ui.pointerUp(p); }
    void key(fuse::ui::Key k, fuse::ui::u8 mods = 0) { m_ui.keyDown(k, mods); m_ui.keyUp(k, mods); }
    void text(std::string_view utf8) { m_ui.textInput(utf8); }

    // ---- window management (services.hpp). Without an OS backend (headless) the window is its own window system:
    // requests take effect at once and emit the matching WindowEvent. With one (NativePlatform::show) they are
    // forwarded and the state follows what the OS reports back; a platform that cannot do something leaves it as is.
    /// Size limits in logical units (0 = none). Headless: also clamps the current size.
    void setMinimumSize(fuse::ui::Size s);
    void setMaximumSize(fuse::ui::Size s);
    fuse::ui::Size minimumSize() const { return m_minSize; }
    fuse::ui::Size maximumSize() const { return m_maxSize; }
    fuse::ui::Size clampSize(fuse::ui::Size s) const; ///< `s` within the limits
    void setMode(WindowMode m);
    WindowMode mode() const { return m_mode; }
    WindowMode requestedMode() const { return m_wantMode; }
    void maximize() { setMode(WindowMode::Maximized); }
    void minimize() { setMode(WindowMode::Minimized); }
    /// Back from minimised to what it was before, else to Normal.
    void restore() { setMode(m_mode == WindowMode::Minimized && m_restoreMode != WindowMode::Minimized ? m_restoreMode : WindowMode::Normal); }
    void setFullscreen(bool on) { setMode(on ? WindowMode::Fullscreen : WindowMode::Normal); }
    bool fullscreen() const { return m_mode == WindowMode::Fullscreen; }
    void setVisible(bool on);
    bool visible() const { return m_visible; }
    bool requestedVisible() const { return m_wantVisible; }
    /// Top-left of the client area on screen, logical units.
    void setPosition(fuse::ui::Vec2 p);
    fuse::ui::Vec2 position() const { return m_position; }
    fuse::ui::Vec2 requestedPosition() const { return m_wantPosition; }
    bool positionRequested() const { return m_positionAsked; } ///< setPosition was called (show() then places the window)
    void activate(); ///< raise + keyboard focus
    bool active() const { return m_active; }
    void setEventHandler(const WindowEventHandler& h) { m_events = h; }
    void setCloseGuard(const CloseGuard& g) { m_closeGuard = g; }
    /// The user asked to close: runs the close guard; closes (true) unless it vetoes. Backends call this.
    bool tryClose();
    /// Follow the system light / dark preference and accent: applyTheme() now (when known) and on every change.
    void setFollowSystemTheme(bool on);
    /// Re-colours the default widget theme with one token override sheet (themeTokensCss; light / dark / high
    /// contrast from widgets::themeTokensCss), replaced in place on every call; restyles the window.
    void applyTheme(const SystemTheme& t);

    // ---- client-side chrome (chrome.hpp) and saved state
    WindowChrome& chrome() { return m_chrome; }
    const WindowChrome& chrome() const { return m_chrome; }
    /// What the title-bar buttons do: minimize(), maximise or restore, tryClose() (the close guard may veto).
    void command(WindowCommand c);
    /// UI state of the nodes that have an id, as `<prefix><kind>.<id>` keys: the focused node, text-field text,
    /// values (tab bars, sliders, combo and spin boxes, dials), check states and scroll offsets. Restoring emits
    /// TextChanged / ValueChanged / Toggled so app code follows; scroll offsets are applied after the next layout.
    void saveUiState(Settings& out, std::string_view prefix = {}) const;
    void restoreUiState(const Settings& in, std::string_view prefix = {});
    /// The window's geometry as `<prefix>x / y / w / h / mode` (position and size in logical units, taken while the
    /// window is Normal; a maximised / fullscreen window keeps the last normal geometry it saved before).
    /// restoreWindowState resizes, moves and re-enters the mode (Minimized is restored as Normal); false when
    /// `in` holds no size for the prefix. Call it before or after NativePlatform::show.
    void saveWindowState(Settings& out, std::string_view prefix = "window.") const;
    bool restoreWindowState(const Settings& in, std::string_view prefix = "window.");

    // ---- menu bar (plan UI10): one description, the native bar on macOS, the in-window widgets::menuBar elsewhere
    /// Installs the menu. In-window: a menu bar is created as the first child of `parent` (the root when invalid).
    /// Activation (click, shortcut through menuShortcut, native menu) toggles a checkable item and calls `handler`.
    void setMenu(const MenuDesc& d, MenuHandler handler, fuse::ui::NodeHandle parent = {});
    const MenuDesc& menu() const { return m_menu; }
    bool nativeMenu() const { return m_nativeMenu; }           ///< the OS shows it (no in-window bar)
    fuse::ui::NodeHandle menuBarNode() const { return m_menuBar; } ///< the in-window bar (invalid with a native menu)
    bool activateMenuItem(fuse::ui::u32 id);                    ///< false: unknown, disabled or a submenu
    bool menuShortcut(fuse::ui::Key key, fuse::ui::u8 mods);    ///< backends call it for keys the UI did not consume
    bool setMenuItemEnabled(fuse::ui::u32 id, bool on);
    bool setMenuItemChecked(fuse::ui::u32 id, bool on);
    bool menuItemChecked(fuse::ui::u32 id) const;

    // backends: the OS side of the above
    void setWindowBackend(const WindowBackend& b);
    void notifyMode(WindowMode m);
    void notifyActive(bool on);
    void notifyMoved(fuse::ui::Vec2 p);
    void notifyVisible(bool on);
    void notifyTheme(const SystemTheme& t);

private:
    friend class Application;
    bool frame(f32 dt); ///< update + present when needed; true when presented
    static bool presentHeadless(void* user, AppWindow& w, fuse::ui::Rect damage);
    void emitWindowEvent(const WindowEvent& e);
    bool ask(WindowRequest r);
    void clampToLimits();
    void notifyScale(fuse::ui::f32 before);
    void rebuildMenu();

    Application& m_app;
    fuse::ui::UiContext m_ui;
    std::unique_ptr<fuse::ui::doc::Document> m_doc;
    std::string m_title;
    Presenter m_presenter;
    fuse::ui::Image m_image;
    fuse::ui::CpuRasterizer m_raster;
    RedrawPolicy m_policy = RedrawPolicy::OnDemand;
    u64 m_frames = 0;
    u32 m_id;
    u32 m_pixelW = 0, m_pixelH = 0; ///< device-pixel size of the frame (logical size x scale)
    bool m_closing = false, m_forceRedraw = true;
    // window state (services.cpp)
    WindowBackend m_backend;
    WindowEventHandler m_events;
    CloseGuard m_closeGuard;
    fuse::ui::Size m_minSize, m_maxSize;
    fuse::ui::Vec2 m_position, m_wantPosition;
    WindowMode m_mode = WindowMode::Normal, m_wantMode = WindowMode::Normal, m_restoreMode = WindowMode::Normal;
    bool m_visible = true, m_wantVisible = true, m_active = false, m_followTheme = false, m_nativeMenu = false;
    fuse::ui::u32 m_themeOverride = 0; ///< applyTheme's token override sheet (UiContext::addStyleSheetTracked handle)
    MenuDesc m_menu;
    MenuHandler m_menuHandler;
    fuse::ui::NodeHandle m_menuBar, m_menuParent;
    std::vector<fuse::ui::NodeHandle> m_menuButtons;
    std::vector<std::vector<fuse::ui::u32>> m_menuLines; ///< per top-level menu: item id of each line
    WindowChrome m_chrome;
    bool m_positionAsked = false;
    f32 m_maxRate = 0.f;
    double m_nextFrame = -1.0; ///< app-clock time the next paced frame may present
    std::vector<std::pair<std::string, fuse::ui::Vec2>> m_pendingScroll; ///< restored, applied after layout
    bool paced() const { return !m_presenter.vsynced && m_maxRate > 0.f; }
};

/// Saved state across process death: the OS may end a background process and recreate the app later (Android).
/// `save` adds app state to what Application::saveState collects; `restore` gets the state a previous process saved.
struct StateHooks {
    void* user = nullptr;
    void (*save)(void* user, Application& app, Settings& out) = nullptr;
    void (*restore)(void* user, Application& app, const Settings& in) = nullptr;
};

// ---- application ---------------------------------------------------------------------------------------------------
struct AppDesc {
    std::string_view name = "fuse_app";
    int argc = 0;
    const char* const* argv = nullptr;
    bool singleInstance = false;   ///< take the per-user instance lock (primary() is false when another holds it)
    bool loadSettings = true;      ///< read <config dir>/settings.ini at start (--settings=<path> overrides)
};

/// Pumps OS events into the windows, blocking for at most `waitSeconds` (+inf = until an event). Returns false to stop.
struct EventSource {
    void* user = nullptr;
    bool (*pump)(void* user, Application& app, f32 waitSeconds) = nullptr;
};
/// Monotonic clock in seconds (tests drive a virtual one).
struct Clock {
    void* user = nullptr;
    double (*now)(void* user) = nullptr;
};

struct LoopStats {
    u64 iterations = 0, frames = 0, sleeps = 0;
    double sleptSeconds = 0.0;
};

class Application {
public:
    explicit Application(const AppDesc& d);
    ~Application(); ///< saves dirty settings, releases the instance lock
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    AppWindow& createWindow(const WindowDesc& d);
    u32 windowCount() const { return u32(m_windows.size()); }
    AppWindow* window(u32 index) { return index < m_windows.size() ? m_windows[index].get() : nullptr; }

    const CommandLine& args() const { return m_args; }
    Settings& settings() { return m_settings; }
    const std::string& name() const { return m_name; }
    bool primary() const { return m_primary; } ///< false when singleInstance and another instance runs

    /// Replaces the event pump (an empty source restores the default sleep pump).
    void setEventSource(const EventSource& s);
    void setClock(const Clock& c) { m_clock = c; }
    /// One loop iteration: pump events (waiting at most until the next UI deadline, capped by `maxWait`), then
    /// update / present every window that needs it and drop closed windows. False once the app should exit.
    bool step(f32 maxWait = 1e30f);
    /// Runs until quit() or the last window closes. Returns the exit code.
    int run();
    void quit(int code = 0) { m_quit = true; m_exit = code; }
    bool quitting() const { return m_quit; }
    /// Prints `message` to stderr and returns the failure exit code (for `return app.fail(err)`).
    int fail(std::string_view message);
    const LoopStats& stats() const { return m_stats; }
    /// Seconds until some window needs a frame (0 = now, +inf = idle).
    f32 nextWake() const;
    double now() const;
    /// The desktop's light / dark preference and accent as last reported (NativePlatform reports it at start and on
    /// change; tests and hosts may set it). A change notifies every window (WindowEvent::ThemeChanged).
    const SystemTheme& systemTheme() const { return m_theme; }
    void setSystemTheme(const SystemTheme& t);

    // ---- saved state (process death / session restore)
    /// saveState() collects every window's UI state (AppWindow::saveUiState, keys "w<index>.") and geometry
    /// ("w<index>.window.") plus what StateHooks::save adds, as Settings text; the next process passes it to
    /// restoreState(), which restores the windows that exist (by index) and calls StateHooks::restore. Keep it small
    /// (Android keeps it in the activity's saved-instance state). saveStateTo / restoreStateFrom do the same through
    /// a file (desktop session restore); restoreStateFrom is false when the file is missing.
    void setStateHooks(const StateHooks& h) { m_stateHooks = h; }
    std::string saveState();
    void restoreState(std::string_view bytes);
    bool saveStateTo(const std::string& path);
    bool restoreStateFrom(const std::string& path);
    bool restored() const { return m_restoredSeen; } ///< restoreState ran with state from a previous process
    const Settings& restoredState() const { return m_restored; }

private:
    std::string m_name;
    CommandLine m_args;
    Settings m_settings;
    std::vector<std::unique_ptr<AppWindow>> m_windows;
    EventSource m_events;
    Clock m_clock;
    LoopStats m_stats;
    double m_last = -1.0;
    void* m_lock = nullptr; ///< platform instance-lock handle
    u32 m_nextId = 1;
    int m_exit = 0;
    bool m_quit = false, m_primary = true;
    SystemTheme m_theme;
    StateHooks m_stateHooks;
    Settings m_restored;
    bool m_restoredSeen = false;
};

/// Per-user data root for platforms without XDG / %APPDATA% conventions (Android: the activity's internal data path;
/// iOS: <sandbox>/Library/Application Support). The mobile backends set it before the user's main runs; when set,
/// Settings::defaultDir(app) is <root>/<app> and the single-instance lock lives under it. Empty = desktop defaults.
void setDataRoot(std::string path);
const std::string& dataRoot();

/// Reads app resources that are not plain files (Android APK assets, an iOS / macOS bundle's Resources).
/// AppWindow::load() tries the file system first, then this. Mobile backends install it before main runs.
struct ResourceReader {
    void* user = nullptr;
    bool (*read)(void* user, std::string_view path, std::string& out) = nullptr;
};
void setResourceReader(const ResourceReader& reader);
/// File system first, then the ResourceReader. False when neither has `path`.
bool readResource(std::string_view path, std::string& out);

/// Writes an RGBA8 image as PNG.
bool writePng(const fuse::ui::Image& image, const std::string& path);

/// The bundled font every window uses by default (loaded once).
const fuse::text::Font& defaultFont();

} // namespace fuse::app
