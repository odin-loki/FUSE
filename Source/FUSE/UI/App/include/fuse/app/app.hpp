// fuse_app (plan UI5): the application shell for UI-only projects. An Application owns AppWindows (each a UiContext
// plus a presenter), command-line arguments, a settings store and the single-instance lock, and runs an on-demand
// loop: it sleeps until input, a UI timer or an animation needs a frame (UiContext::nextWakeSeconds), so an idle app
// presents nothing. There is no second loop: an engine host calls step() from its frame instead of run().
//
// Backends are function tables (no inheritance): an EventSource pumps OS input into the windows and blocks for at
// most the wait it is given; a Presenter shows a window's draw list. The built-in headless backend rasterises on the
// CPU into the window's image (tests, screenshots, servers); desktop / mobile presenters plug in the same way.
#pragma once

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
    void fromText(std::string_view text); ///< replaces the store with parsed lines (left clean)
    /// Platform config directory for an app: $XDG_CONFIG_HOME/<app> (~/.config/<app>), %APPDATA%\<app>.
    static std::string defaultDir(std::string_view app);

private:
    std::vector<std::pair<std::string, std::string>> m_items;
    std::string m_path;
    bool m_dirty = false;
};

// ---- system theme (plan §4.13 Theme) -------------------------------------------------------------------------------
/// The OS appearance preferences. Desktop backends query it (Win32: AppsUseLightTheme, DwmGetColorizationColor,
/// SPI_GETHIGHCONTRAST, SPI_GETCLIENTAREAANIMATION, Accessibility TextScaleFactor) and update it live.
struct SystemTheme {
    bool dark = true;
    fuse::ui::Color accent = fuse::ui::Color::rgb(0x3D8BFD);
    bool highContrast = false;
    bool reducedMotion = false;
    f32 textScale = 1.f; ///< the OS "make text bigger" factor (1 .. 2.25)
    friend bool operator==(const SystemTheme&, const SystemTheme&) = default;
};

// ---- windows -------------------------------------------------------------------------------------------------------
/// OS frame of a window. Borderless and Custom draw their own title bar (fuse/app/chrome.hpp: titleBar); the OS asks
/// chromeHitTest() which parts drag (caption), resize (edges, Custom only) or act as min / max / close buttons.
enum class WindowFrame : fuse::ui::u8 {
    System,     ///< normal OS title bar and borders
    Borderless, ///< no OS frame, not resizable (splash screens, launchers)
    Custom,     ///< no OS frame, resizable from `resizeBorder` px edges; snap / maximise kept (Win32)
};
struct WindowDesc {
    std::string_view title = "FUSE";
    u32 width = 800, height = 600; ///< logical (UI units)
    f32 dpiScale = 1.f;            ///< initial device pixels per UI unit (native windows take the monitor's)
    WindowFrame frame = WindowFrame::System;
    f32 resizeBorder = 6.f; ///< Custom frame: resize edge thickness (logical px)
};
enum class RedrawPolicy : fuse::ui::u8 {
    OnDemand,   ///< update + present only when the UI needs it (default)
    Continuous, ///< every loop iteration (games, live viewports)
};

class Application;
class AppWindow;

enum class WindowCommand : fuse::ui::u8 { Minimize, ToggleMaximize, Close };
/// Client-drawn chrome of a Borderless / Custom window: the title-bar nodes (titleBar() sets them) and the backend
/// hook that carries out min / max / close (NativePlatform installs it; without one Close calls requestClose()).
struct WindowChrome {
    WindowFrame frame = WindowFrame::System;
    f32 resizeBorder = 0.f;
    fuse::ui::NodeHandle caption, minimize, maximize, close; ///< caption = drag area (its non-interactive children too)
    bool maximized = false;                                  ///< kept current by the backend
    void* user = nullptr;
    void (*command)(void* user, AppWindow& w, WindowCommand c) = nullptr;
};

/// Shows a window's frame. `damage` is the union of changed rects (whole window on the first frame / resize).
struct Presenter {
    void* user = nullptr;
    bool (*present)(void* user, AppWindow& w, fuse::ui::Rect damage) = nullptr;
    void (*destroy)(void* user) = nullptr; ///< called when the window closes
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
    void setTitle(std::string_view t) { m_title = t; }
    /// Loads a .fuseui document (file path) into the window root, replacing a previous one. `model` may be null.
    std::expected<void, std::string> load(std::string_view path, fuse::ui::doc::Model* model = nullptr);
    std::expected<void, std::string> loadText(std::string_view json, fuse::ui::doc::Model* model = nullptr);
    fuse::ui::doc::Document* document() { return m_doc.get(); }

    void resize(u32 width, u32 height); ///< logical units; forces a full repaint
    fuse::ui::Size size() const { return m_ui.viewport(); }
    /// DPI scale: device pixels per UI unit (1 = 96 dpi). The presenter renders pixelSize(); input arrives in UI
    /// units. setScale keeps the logical size (the pixel size follows), re-lays out, re-rasterises text and calls the
    /// onDpiChanged handler.
    f32 scale() const { return m_ui.scale(); }
    void setScale(f32 s);
    struct PixelSize {
        u32 w = 0, h = 0;
    };
    PixelSize pixelSize() const { return m_pixels; }
    /// The OS resized the client area to `width` x `height` device pixels (the logical size is that / scale()).
    void resizePixels(u32 width, u32 height);
    void onDpiChanged(fuse::ui::InlineFn<void(AppWindow&, f32)> fn) { m_onDpi = fn; }
    void setRedrawPolicy(RedrawPolicy p) { m_policy = p; }
    RedrawPolicy redrawPolicy() const { return m_policy; }
    void setPresenter(const Presenter& p);
    /// Frames presented so far (the idle gate checks this stays flat).
    u64 framesPresented() const { return m_frames; }
    void requestClose() { m_closing = true; }
    bool closing() const { return m_closing; }
    void requestRedraw() { m_forceRedraw = true; }
    WindowChrome& chrome() { return m_chrome; }
    const WindowChrome& chrome() const { return m_chrome; }
    /// Minimise / toggle maximise / close through the backend hook (the title-bar buttons call this).
    void command(WindowCommand c);
    /// Headless presenter target: the last CPU-rasterised frame.
    const fuse::ui::Image& image() const { return m_image; }
    bool writePng(const std::string& path) const;
    /// Registers an app-owned image with the CPU rasteriser (viewportWidget / image index). The image must outlive the
    /// window; call ui().invalidate(node) after changing its pixels.
    u32 addImage(const fuse::ui::Image* img) { return m_raster.addImage(img); }
    const fuse::ui::CpuRasterizer& rasterizer() const { return m_raster; }
    /// CPU-rasterises `damage` (whole window when empty / resized) into image(): the headless presenter, also used by
    /// native presenters that upload the CPU raster.
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

    /// UI state of the nodes that have an id, as `<prefix><kind>.<id>` keys: the focused node, text field text,
    /// values (tab bars, sliders, combo and spin boxes), check states and scroll offsets. Application::saveState
    /// uses these for every window. Restoring emits TextChanged / ValueChanged so app code follows; scroll offsets
    /// are applied after the next layout.
    void saveUiState(Settings& out, std::string_view prefix = {}) const;
    void restoreUiState(const Settings& in, std::string_view prefix = {});

private:
    friend class Application;
    bool frame(f32 dt); ///< update + present when needed; true when presented
    static bool presentHeadless(void* user, AppWindow& w, fuse::ui::Rect damage);

    Application& m_app;
    fuse::ui::UiContext m_ui;
    std::unique_ptr<fuse::ui::doc::Document> m_doc;
    std::string m_title;
    Presenter m_presenter;
    WindowChrome m_chrome;
    fuse::ui::Image m_image;
    fuse::ui::CpuRasterizer m_raster;
    RedrawPolicy m_policy = RedrawPolicy::OnDemand;
    PixelSize m_pixels;
    fuse::ui::InlineFn<void(AppWindow&, f32)> m_onDpi;
    std::vector<std::pair<std::string, fuse::ui::Vec2>> m_pendingScroll; ///< restored, applied after layout
    u64 m_frames = 0;
    u32 m_id;
    bool m_closing = false, m_forceRedraw = true;
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
    const EventSource& eventSource() const { return m_events; } ///< the current pump (wrappers chain to it)
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

    // ---- system theme ----
    const SystemTheme& systemTheme() const { return m_theme; }
    /// Backends call this at start and whenever the OS appearance changes. Every window (and later ones) honours
    /// reduced motion; with followSystemTheme the default widget theme switches dark / light and takes the system
    /// accent. Calls the onThemeChanged handler when something changed.
    void setSystemTheme(const SystemTheme& t);
    void onThemeChanged(fuse::ui::InlineFn<void(Application&, const SystemTheme&)> fn) { m_onTheme = fn; }
    /// Follow the OS colour scheme (default off: windows keep the dark default theme and app style sheets).
    void setFollowSystemTheme(bool on);
    bool followSystemTheme() const { return m_followTheme; }
    /// Applies the theme to one window (createWindow does this for new windows).
    void applyTheme(AppWindow& w) const;

    // ---- saved state (process death) ----
    /// The OS may end a background process and recreate the app later (Android). saveState() collects every
    /// window's UI state (AppWindow::saveUiState, keys "w<index>.") plus what the onSaveState handler adds; the next
    /// process passes it to restoreState(), which restores the windows and calls onRestoreState. Keep it small
    /// (Android keeps it in the activity's saved-instance Bundle).
    void onSaveState(fuse::ui::InlineFn<void(Application&, Settings&)> fn) { m_onSave = fn; }
    /// Called with the restored state; at once when restoreState already ran.
    void onRestoreState(fuse::ui::InlineFn<void(Application&, const Settings&)> fn);
    std::string saveState();                   ///< backends: the state to keep, as bytes
    void restoreState(std::string_view bytes); ///< backends: the bytes a previous process saved
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
    bool m_themeKnown = false, m_followTheme = false;
    fuse::ui::InlineFn<void(Application&, const SystemTheme&)> m_onTheme;
    fuse::ui::InlineFn<void(Application&, Settings&)> m_onSave;
    fuse::ui::InlineFn<void(Application&, const Settings&)> m_onRestore;
    Settings m_restored;
    bool m_restoredSeen = false;
};

/// Writes an RGBA8 image as PNG.
bool writePng(const fuse::ui::Image& image, const std::string& path);

/// The bundled font every window uses by default (loaded once).
const fuse::text::Font& defaultFont();

} // namespace fuse::app
