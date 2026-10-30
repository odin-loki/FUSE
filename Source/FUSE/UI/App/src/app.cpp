// fuse_app (plan UI5): Application, AppWindow, on-demand loop, settings store, command line, single instance.
#include "fuse/ui/from_chars.hpp"
#include <cctype>
#include "fuse/app/app.hpp"

#include "fuse/ui/doc/document.hpp"
#include "fuse/ui/os.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <sstream>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace fuse::app {
using namespace fuse::ui;

namespace {
bool parseDouble(std::string_view s, double& out) {
    if (s.empty()) return false;
    const auto [p, ec] = fuse::ui::fromChars(s.data(), s.data() + s.size(), out);
    return ec == std::errc{} && p == s.data() + s.size();
}
double steadyNow(void*) { return fuse::ui::os::monotonicSeconds(); }
/// Default event source (no OS windows): sleeps for the wait, capped so quit() from a callback is seen.
bool sleepPump(void*, Application&, f32 wait) {
    if (wait > 0.f) fuse::ui::os::sleepSeconds(std::min(wait, 0.25f));
    return true;
}
std::string escape(std::string_view s) {
    std::string o;
    for (char c : s) {
        if (c == '\\') o += "\\\\";
        else if (c == '\n') o += "\\n";
        else if (c == '=') o += "\\e";
        else o += c;
    }
    return o;
}
std::string unescape(std::string_view s) {
    std::string o;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            const char c = s[++i];
            o += c == 'n' ? '\n' : c == 'e' ? '=' : c;
        } else {
            o += s[i];
        }
    }
    return o;
}
} // namespace

const fuse::text::Font& defaultFont() {
    static fuse::text::Font f = [] {
        auto r = fuse::text::Font::bundled();
        if (!r) { std::fprintf(stderr, "fuse_app: bundled font missing\n"); std::exit(2); }
        return std::move(*r);
    }();
    // Bundled fallback chain (Hebrew, Arabic, Devanagari, colour emoji, symbols), attached once.
    static const std::vector<fuse::text::Font> fallbacks = [] {
        auto v = fuse::text::Font::bundledFallbacks();
        std::vector<const fuse::text::Font*> chain;
        for (const auto& x : v) chain.push_back(&x);
        f.setFallbacks(std::move(chain));
        return v;
    }();
    (void)fallbacks;
    return f;
}

// ---- CommandLine ---------------------------------------------------------------------------------------------------
CommandLine::CommandLine(int argc, const char* const* argv) {
    if (argc > 0 && argv && argv[0]) m_program = argv[0];
    bool opts = true;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i] ? argv[i] : "";
        if (opts && a == "--") { opts = false; continue; }
        if (opts && a.size() > 2 && a.substr(0, 2) == "--") {
            const std::size_t eq = a.find('=');
            if (eq == std::string_view::npos) m_options.emplace_back(std::string(a.substr(2)), "1");
            else m_options.emplace_back(std::string(a.substr(2, eq - 2)), std::string(a.substr(eq + 1)));
        } else {
            m_positional.emplace_back(a);
        }
    }
}
bool CommandLine::has(std::string_view key) const {
    return std::any_of(m_options.begin(), m_options.end(), [&](const auto& o) { return o.first == key; });
}
std::string_view CommandLine::get(std::string_view key, std::string_view fallback) const {
    for (auto it = m_options.rbegin(); it != m_options.rend(); ++it) // last one wins
        if (it->first == key) return it->second;
    return fallback;
}
double CommandLine::number(std::string_view key, double fallback) const {
    double v;
    return parseDouble(get(key), v) ? v : fallback;
}

// ---- Settings ------------------------------------------------------------------------------------------------------
std::string Settings::defaultDir(std::string_view app) { return fuse::ui::os::configDir(app); }
bool Settings::load() {
    m_items.clear();
    m_dirty = false;
    auto text = fuse::ui::os::readFile(m_path);
    if (!text) return false;
    std::istringstream f(std::move(*text));
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        m_items.emplace_back(unescape(std::string_view(line).substr(0, eq)), unescape(std::string_view(line).substr(eq + 1)));
    }
    return true;
}
bool Settings::save() {
    if (!m_dirty) return true;
    if (m_path.empty()) return false;
    std::string out;
    for (const auto& [k, v] : m_items) out += escape(k) + '=' + escape(v) + '\n';
    if (!fuse::ui::os::writeFileAtomic(m_path, out)) return false; // creates parent dirs, atomic rename
    m_dirty = false;
    return true;
}
std::string_view Settings::get(std::string_view key, std::string_view fallback) const {
    for (const auto& [k, v] : m_items)
        if (k == key) return v;
    return fallback;
}
double Settings::number(std::string_view key, double fallback) const {
    double v;
    return parseDouble(get(key), v) ? v : fallback;
}
void Settings::set(std::string_view key, std::string_view value) {
    for (auto& [k, v] : m_items)
        if (k == key) {
            if (v != value) { v = value; m_dirty = true; }
            return;
        }
    m_items.emplace_back(std::string(key), std::string(value));
    m_dirty = true;
}
void Settings::set(std::string_view key, double value) {
    char buf[64];
    const auto [p, ec] = std::to_chars(buf, buf + sizeof buf, value);
    set(key, std::string_view(buf, ec == std::errc{} ? std::size_t(p - buf) : 0));
}
void Settings::remove(std::string_view key) {
    const auto it = std::find_if(m_items.begin(), m_items.end(), [&](const auto& kv) { return kv.first == key; });
    if (it != m_items.end()) { m_items.erase(it); m_dirty = true; }
}

// ---- AppWindow -----------------------------------------------------------------------------------------------------
AppWindow::AppWindow(Application& app, const WindowDesc& d, u32 id)
    : m_app(app), m_ui({.viewport = {f32(d.width), f32(d.height)}, .font = &defaultFont()}), m_title(d.title), m_id(id) {
    widgets::install(m_ui);
    // a widget drag (setDragSource / item views) leaving the window continues as an OS drag (X11 XDND, Wayland
    // wl_data_device, Win32 OLE): the backend picks it up on its next pointer motion and reports back through
    // deliverExternalDragEnd
    widgets::setExternalDragHandler(m_ui, [this](UiContext&, const widgets::DragPayload& p) {
        ExternalDrag d;
        if (p.has("text/uri-list")) d.paths = pathsFromUriList(p.get("text/uri-list"));
        if (d.paths.empty()) {
            if (p.has("text/plain")) { d.mime = "text/plain"; d.data = std::string(p.get("text/plain")); }
            else if (!p.formats.empty()) { d.mime = p.formats.front().first; d.data = p.formats.front().second; }
            else return false;
        }
        return beginExternalDrag(std::move(d));
    });
    m_presenter = {this, &AppWindow::presentHeadless, nullptr, true}; // offscreen: unpaced
    setScale(d.dpiScale); // WindowDesc::dpiScale seeds the HiDPI scale; backends update it on monitor changes
}
void AppWindow::deliverExternalDragEnd(bool accepted) {
    if (widgets::externalDragActive(m_ui)) widgets::finishExternalDrag(m_ui, accepted ? widgets::DropCopy : widgets::DropIgnore);
    if (m_onDragEnd) m_onDragEnd(accepted);
    requestRedraw();
}
AppWindow::~AppWindow() {
    m_doc.reset();
    if (m_presenter.destroy) m_presenter.destroy(m_presenter.user);
}
void AppWindow::setPresenter(const Presenter& p) {
    if (m_presenter.destroy) m_presenter.destroy(m_presenter.user);
    m_presenter = p.present ? p : Presenter{this, &AppWindow::presentHeadless, nullptr, true} /* offscreen: unpaced */;
    m_forceRedraw = true;
}
void AppWindow::resize(u32 width, u32 height) {
    m_ui.setViewport({f32(width), f32(height)});
    m_ui.markAllDirty();
    m_forceRedraw = true;
}
std::expected<void, std::string> AppWindow::loadText(std::string_view json, doc::Model* model) {
    m_doc.reset();
    auto d = doc::Document::load(m_ui, json, model);
    if (!d) {
        const auto& e = d.error();
        return std::unexpected(e.path + ": " + e.message + (e.line ? " (line " + std::to_string(e.line) + ")" : ""));
    }
    m_doc = std::move(*d);
    m_forceRedraw = true;
    return {};
}
std::expected<void, std::string> AppWindow::load(std::string_view path, doc::Model* model) {
    auto text = fuse::ui::os::readFile(path);
    if (!text) return std::unexpected("cannot open " + std::string(path) + ": " + text.error());
    return loadText(*text, model);
}
void AppWindow::setScale(f32 s) {
    s = s > 0.f ? s : 1.f;
    if (s == m_scale) return;
    m_scale = s;
    m_ui.setPixelScale(s);
    m_forceRedraw = true;
}
Size AppWindow::pixelSize() const {
    const Size v = m_ui.viewport();
    return {std::max(1.f, std::round(v.w * m_scale)), std::max(1.f, std::round(v.h * m_scale))};
}
const DrawList& AppWindow::pixelDrawList() {
    if (m_scale == 1.f) return m_ui.drawList();
    scaleDrawList(m_ui.drawList(), m_scale, m_scaledList);
    return m_scaledList;
}
bool AppWindow::presentHeadless(void*, AppWindow& w, Rect damage) {
    const Size px = w.pixelSize();
    const int iw = int(px.w), ih = int(px.h);
    const f32 s = w.m_scale;
    const bool full = w.m_image.width != iw || w.m_image.height != ih;
    if (full) w.m_image.resize(iw, ih, 0xFF000000u);
    w.m_raster.setGlyphAtlas(w.m_ui.atlas().pixels(), w.m_ui.atlas().size());
    const Rect r = full || damage.empty() ? Rect{0.f, 0.f, f32(iw), f32(ih)}
                                          : Rect{damage.x * s, damage.y * s, damage.w * s, damage.h * s};
    // clear the damaged region, then rasterise the list clipped to it (incremental present)
    const int x0 = std::max(0, int(std::floor(r.x))), y0 = std::max(0, int(std::floor(r.y)));
    const int x1 = std::min(iw, int(std::ceil(r.x + r.w))), y1 = std::min(ih, int(std::ceil(r.y + r.h)));
    for (int y = y0; y < y1; ++y)
        std::fill_n(w.m_image.pixels.begin() + std::ptrdiff_t(y) * iw + x0, std::max(0, x1 - x0), 0xFF000000u);
    w.m_raster.render(w.pixelDrawList(), w.m_image, {f32(x0), f32(y0), f32(x1 - x0), f32(y1 - y0)});
    return true;
}
const HdrImage& AppWindow::rasterizeHdr(Rect damage, PixelFormat format) {
    const Size px = pixelSize();
    const int iw = int(px.w), ih = int(px.h);
    const f32 s = m_scale;
    const PixelFormat fmt = format == PixelFormat::Rgba32F ? PixelFormat::Rgba32F : PixelFormat::Rgba16F;
    const bool full = m_hdrImage.width != iw || m_hdrImage.height != ih || m_hdrImage.format != fmt;
    if (full) {
        m_hdrImage.format = fmt;
        m_hdrImage.resize(iw, ih);
    }
    m_raster.setGlyphAtlas(m_ui.atlas().pixels(), m_ui.atlas().size());
    const Rect r = full || damage.empty() ? Rect{0.f, 0.f, f32(iw), f32(ih)}
                                          : Rect{damage.x * s, damage.y * s, damage.w * s, damage.h * s};
    const int x0 = std::max(0, int(std::floor(r.x))), y0 = std::max(0, int(std::floor(r.y)));
    const int x1 = std::min(iw, int(std::ceil(r.x + r.w))), y1 = std::min(ih, int(std::ceil(r.y + r.h)));
    for (int y = y0; y < y1; ++y)
        std::fill_n(m_hdrImage.pixels.begin() + std::ptrdiff_t(y) * iw + x0, std::max(0, x1 - x0), PixelF{0.f, 0.f, 0.f, 1.f});
    m_raster.render(pixelDrawList(), m_hdrImage, {f32(x0), f32(y0), f32(x1 - x0), f32(y1 - y0)});
    return m_hdrImage;
}
bool writePng(const Image& img, const std::string& path) {
    if (img.width <= 0) return false;
    return stbi_write_png(path.c_str(), img.width, img.height, 4, img.pixels.data(), img.width * 4) != 0;
}
bool AppWindow::writePng(const std::string& path) const {
    if (m_image.width <= 0) return false;
    return stbi_write_png(path.c_str(), m_image.width, m_image.height, 4, m_image.pixels.data(), m_image.width * 4) != 0;
}
bool AppWindow::wantsFrame() const {
    return m_forceRedraw || m_policy == RedrawPolicy::Continuous || m_ui.nextWakeSeconds() <= 0.f;
}
f32 AppWindow::wakeIn(double now) const {
    if (m_closing) return 0.f;
    if (m_framePending) return std::numeric_limits<f32>::infinity(); // the compositor's frame event wakes the pump
    f32 t = wantsFrame() ? 0.f : m_ui.nextWakeSeconds();
    if (paced() && t < std::numeric_limits<f32>::infinity() && m_nextFrame > now)
        t = std::max(t, f32(m_nextFrame - now));
    return t;
}
bool AppWindow::frame(f32 dt) {
    if (!m_forceRedraw && m_policy == RedrawPolicy::OnDemand && !m_ui.needsUpdate()) return false;
    const double now = m_app.now();
    if (m_framePending || (paced() && m_nextFrame > now)) { // too early: nextWake sleeps until the deadline / event
        m_heldDt += dt; // skipped frames' time still reaches UI timers and animations
        return false;
    }
    dt += m_heldDt;
    m_heldDt = 0.f;
    m_ui.update(dt); // advances timers; presents only when something repainted
    if (m_doc && m_doc->refresh()) m_ui.update(0.f);
    if (!m_ui.stats().repainted && !m_forceRedraw && m_policy == RedrawPolicy::OnDemand) return false;
    const Rect dmg = m_forceRedraw ? Rect{0.f, 0.f, m_ui.viewport().w, m_ui.viewport().h} : m_ui.damage();
    m_forceRedraw = false;
    if (!m_presenter.present || !m_presenter.present(m_presenter.user, *this, dmg)) return false;
    ++m_frames;
    if (paced()) {
        // a fixed cadence; after a stall (more than one interval behind) restart it from now instead of bursting
        const double interval = 1.0 / double(m_maxRate);
        m_nextFrame = m_nextFrame < 0.0 || now - m_nextFrame > interval ? now + interval : m_nextFrame + interval;
    }
    return true;
}

// ---- Application ---------------------------------------------------------------------------------------------------
Application::Application(const AppDesc& d) : m_name(d.name), m_args(d.argc, d.argv) {
    m_events = {nullptr, &sleepPump};
    m_clock = {nullptr, &steadyNow};
    if (d.loadSettings) {
        const std::string_view over = m_args.get("settings");
        m_settings.setPath(over.empty() ? Settings::defaultDir(m_name) + "/settings.ini" : std::string(over));
        m_settings.load();
    }
    if (d.singleInstance) {
        m_lock = fuse::ui::os::tryLockInstance(m_name);
        m_primary = m_lock != nullptr;
    }
}
Application::~Application() {
    m_windows.clear();
    m_settings.save();
}
AppWindow& Application::createWindow(const WindowDesc& d) {
    m_windows.push_back(std::make_unique<AppWindow>(*this, d, m_nextId++));
    return *m_windows.back();
}
void Application::setEventSource(const EventSource& s) { m_events = s.pump ? s : EventSource{nullptr, &sleepPump}; }
double Application::now() const { return m_clock.now(m_clock.user); }
f32 Application::nextWake() const {
    f32 t = std::numeric_limits<f32>::infinity();
    const double n = now();
    for (const auto& w : m_windows) t = std::min(t, w->wakeIn(n));
    return t;
}
bool Application::step(f32 maxWait) {
    ++m_stats.iterations;
    if (m_quit) return false;
    const f32 wait = std::min(nextWake(), maxWait);
    const double before = now();
    if (m_events.pump && !m_events.pump(m_events.user, *this, wait)) m_quit = true;
    const double t = now();
    if (wait > 0.f && t > before) { ++m_stats.sleeps; m_stats.sleptSeconds += t - before; }
    // no clamp: after an idle sleep the elapsed time is real and UI timers must see all of it
    const f32 dt = m_last < 0.0 ? 0.f : f32(std::max(0.0, t - m_last));
    m_last = t;
    for (auto& w : m_windows)
        if (!w->m_closing && w->frame(dt)) ++m_stats.frames;
    std::erase_if(m_windows, [](const auto& w) { return w->m_closing; });
    if (m_windows.empty()) m_quit = true;
    return !m_quit;
}
int Application::run() {
    while (step()) {
    }
    return m_exit;
}
int Application::fail(std::string_view message) {
    std::fprintf(stderr, "%s: %.*s\n", m_name.c_str(), int(message.size()), message.data());
    m_exit = 1;
    return 1;
}

std::string AppWindow::uriListFromPaths(const std::vector<std::string>& paths) {
    static const char* hexd = "0123456789ABCDEF";
    std::string out;
    for (const auto& p0 : paths) {
        std::string p = p0;
#if defined(_WIN32)
        for (char& c : p) if (c == '\\') c = '/';
        if (p.size() > 1 && p[1] == ':') p.insert(0, "/");
#endif
        out += "file://";
        for (unsigned char c : p) {
            if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') out += char(c);
            else { out += '%'; out += hexd[c >> 4]; out += hexd[c & 15]; }
        }
        out += "\r\n";
    }
    return out;
}

std::vector<std::string> AppWindow::pathsFromUriList(std::string_view list) {
    std::vector<std::string> out;
    auto hex = [](char c) { return c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : -1; };
    std::size_t a = 0;
    while (a < list.size()) {
        std::size_t b = list.find('\n', a);
        if (b == std::string_view::npos) b = list.size();
        std::string_view line = list.substr(a, b - a);
        a = b + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
        if (line.empty() || line[0] == '#' || !line.starts_with("file://")) continue;
        line.remove_prefix(7);
        if (std::size_t slash = line.find('/'); slash != 0 && slash != std::string_view::npos) line.remove_prefix(slash); // host
        std::string p;
        for (std::size_t i = 0; i < line.size(); ++i) {
            if (line[i] == '%' && i + 2 < line.size() && hex(line[i + 1]) >= 0 && hex(line[i + 2]) >= 0) {
                p += char(hex(line[i + 1]) * 16 + hex(line[i + 2]));
                i += 2;
            } else p += line[i];
        }
#if defined(_WIN32)
        if (p.size() > 2 && p[0] == '/' && p[2] == ':') p.erase(0, 1);
#endif
        out.push_back(std::move(p));
    }
    return out;
}

} // namespace fuse::app
