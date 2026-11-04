// Minimal init path for schema 2 app / hybrid projects (see project_host.hpp).
#include "fuse/app/project_host.hpp"
#include "fuse/ui/doc/document.hpp"

#include <fuse/core/init.hpp>
#include <fuse/core/track_b.hpp>
#include <fuse/project/loader.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fuse::app {

namespace {
using project::HostFeature;
using project::hostFeatureBit;

constexpr f32 kViewportPoll = 0.25f;       // while the game side is down / no viewport shows
constexpr f32 kViewportFrame = 1.f / 60.f; // while a viewport shows

std::expected<std::string, std::string> readText(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::unexpected("cannot open " + path.string());
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool knownTheme(std::string_view t) { return t == "system" || t == "dark" || t == "light"; }

/// ui.theme: "system" follows the OS colour scheme (live); "dark" / "light" pin the widget theme.
void applyProjectTheme(Application& app, AppWindow& w, std::string_view theme) {
    if (theme == "system") return; // Application::setFollowSystemTheme does it for every window
    (void)w.ui().setThemeStyleSheet(fuse::ui::widgets::themeCss(theme == "light" ? fuse::ui::widgets::ThemeVariant::Light : fuse::ui::widgets::ThemeVariant::Dark));
    (void)app;
    w.requestRedraw();
}
} // namespace

project::HostFeatureSet ProjectHost::supportedFeatures() {
    return hostFeatureBit(HostFeature::AppPresent) | hostFeatureBit(HostFeature::Jobs) |
           hostFeatureBit(HostFeature::Viewport);
}

std::expected<std::unique_ptr<ProjectHost>, std::string> ProjectHost::start(const ProjectHostDesc& desc) {
    using Clock = std::chrono::steady_clock;
    const auto t0 = Clock::now();
    const auto ms = [&] { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); };

    std::error_code ec;
    const bool isDir = std::filesystem::is_directory(desc.projectPath, ec);
    const project::LoadResult loaded =
        isDir ? project::loadFromDirectory(desc.projectPath) : project::loadFromFile(desc.projectPath);
    if (loaded.status != project::LoadStatus::Ok) return std::unexpected("project: " + loaded.error);
    std::unique_ptr<ProjectHost> host(new ProjectHost());
    host->m_manifest = loaded.manifest;
    const project::ProjectManifest& m = host->m_manifest;
    if (m.kind == project::ProjectKind::Game)
        return std::unexpected("project '" + m.name + "' is a game project (kind \"game\"): run it with the game runtime");
    if (std::string invalid = project::validateManifest(m); !invalid.empty()) return std::unexpected("project: " + invalid);
    if (!knownTheme(m.ui.theme))
        return std::unexpected("project: ui.theme \"" + m.ui.theme + "\" is not one of system, dark, light");
    if (m.kind == project::ProjectKind::Hybrid && !(desc.gameSide.create && desc.gameSide.frame && desc.gameSide.destroy))
        return std::unexpected("project '" + m.name + "' is a hybrid project: this host has no game side "
                               "(ProjectHostDesc::gameSide, e.g. hybridGameSide() from fuse_app_project_hybrid)");
    host->m_backend = desc.gameSide;
    host->m_plan = project::planInit(m);
    // game-side features of a hybrid project are deferred (ensureGameSide), not refused
    const project::HostFeatureSet unsupported =
        (host->m_plan.startup | (m.requiredFeatures & ~host->m_plan.deferred)) & ~supportedFeatures();
    if (unsupported != 0u)
        return std::unexpected("project '" + m.name + "' needs host features this app host does not provide: " +
                               project::hostFeatureList(unsupported));
    host->m_times.manifest = ms();

    // Subsystems: only what the plan asks for. No worlds, GpuScene, FrameComposer or physics.
    if (host->m_plan.has(HostFeature::Jobs)) {
        if (!core::isInitialized()) {
            core::initialize();
            host->m_startedJobs = true;
        }
        host->m_up |= hostFeatureBit(HostFeature::Jobs);
    }
    if (host->m_plan.has(HostFeature::AppPresent)) {
        core::setTrackBHostFeature(core::TrackBHostFeature::AppPresent, true);
        host->m_up |= hostFeatureBit(HostFeature::AppPresent);
    }
    if (host->m_plan.has(HostFeature::Viewport)) host->m_up |= hostFeatureBit(HostFeature::Viewport);
    host->m_times.subsystems = ms();

    // Application + windows. A manifest without windows gets one "main" window titled after the project. The native
    // platform comes first: its asynchronous Vulkan start-up overlaps document loading (software frames meanwhile).
    // Hybrid GPU viewport: the game side's device exists before the windows, so their presenters adopt it and sample
    // the game frame in place (zero-copy). Without one the viewport shows the backend's software frame.
    NativeOptions native = desc.native;
    const GameSideBackend& b = desc.gameSide;
    if (m.kind == project::ProjectKind::Hybrid && desc.gpuViewport && host->m_plan.has(HostFeature::Viewport) &&
        b.gpuCreate && b.gpuDevice && b.gpuFrame && b.gpuDestroy) {
        std::string error;
        host->m_gpu = b.gpuCreate(desc.gpuValidation, error);
        if (!host->m_gpu) {
            std::fprintf(stderr, "%s: GPU viewport unavailable (%s), software frames\n", m.name.c_str(), error.c_str());
        } else if (native.adoptDevice.device == nullptr) {
            native.adoptDevice = b.gpuDevice(host->m_gpu);
            if (native.drawMode == VkDrawMode::Auto) native.drawMode = VkDrawMode::GpuDraw;
        }
    }

    host->m_app = std::make_unique<Application>(AppDesc{.name = m.name, .argc = desc.argc, .argv = desc.argv, .loadSettings = false});
    host->m_model = std::make_unique<fuse::ui::doc::Model>();
    if (desc.showWindows) {
        host->m_native = std::make_unique<NativePlatform>(*host->m_app, native);
        if (!host->m_native->available()) host->m_native.reset(); // no display: headless presenters
    }
    host->m_app->setFollowSystemTheme(m.ui.theme == "system");
    const std::filesystem::path root = m.projectRoot.empty() ? std::filesystem::path(".") : std::filesystem::path(m.projectRoot);
    std::vector<std::string> sheets;
    for (const std::string& style : m.ui.styles) {
        auto text = readText(root / style);
        if (!text) return std::unexpected("ui.styles: " + text.error());
        sheets.push_back(std::move(*text));
    }
    std::vector<project::WindowSettings> windows = m.windows;
    if (windows.empty()) {
        project::WindowSettings w;
        w.title = m.name;
        w.redraw = m.ui.redraw;
        windows.push_back(w);
    }
    for (const project::WindowSettings& ws : windows) {
        const std::string title = ws.title.empty() ? m.name : ws.title;
        AppWindow& w = host->m_app->createWindow({.title = title, .width = ws.width, .height = ws.height});
        w.setRedrawPolicy(ws.redraw == project::RedrawMode::Continuous ? RedrawPolicy::Continuous : RedrawPolicy::OnDemand);
        applyProjectTheme(*host->m_app, w, m.ui.theme);
        for (const std::string& css : sheets)
            if (auto added = w.ui().addStyleSheet(css); !added) return std::unexpected("ui.styles: invalid style sheet");
        const std::string& doc = ws.root.empty() ? m.ui.root : ws.root;
        if (auto ok = w.load((root / doc).string(), host->m_model.get()); !ok)
            return std::unexpected("window '" + ws.id + "': " + ok.error());
        if (host->m_native) host->m_native->show(w);
        host->m_windowIds.emplace_back(ws.id, w.id());
        if (m.kind == project::ProjectKind::Hybrid) host->scanViewports(w);
    }
    host->m_times.windows = ms();

    // First frame of every window.
    Application& app = *host->m_app;
    const auto allPresented = [&] {
        for (u32 i = 0; i < app.windowCount(); ++i)
            if (app.window(i)->framesPresented() == 0) return false;
        return true;
    };
    for (int i = 0; i < 400 && !allPresented(); ++i) app.step(i == 0 ? 0.f : 0.005f);
    if (!allPresented()) return std::unexpected("windows did not present a first frame");
    host->m_times.firstFrame = ms();
    return host;
}

ProjectHost::~ProjectHost() {
    m_native.reset();
    m_app.reset(); // windows (and their timers) go before the game side they draw
    m_viewports.clear();
    if (m_game && m_backend.destroy) m_backend.destroy(m_game);
    m_game = nullptr;
    if (m_gpu && m_backend.gpuDestroy) m_backend.gpuDestroy(m_gpu); // after the presenters that adopted its device
    m_gpu = nullptr;
    m_model.reset();
    if ((m_up & hostFeatureBit(HostFeature::AppPresent)) != 0u)
        core::setTrackBHostFeature(core::TrackBHostFeature::AppPresent, false);
    if (m_startedJobs) core::shutdown();
}

AppWindow* ProjectHost::window(std::string_view id) {
    for (const auto& [name, windowId] : m_windowIds)
        if (name == id) return windowById(windowId);
    return nullptr;
}

AppWindow* ProjectHost::windowById(u32 id) {
    for (u32 i = 0; m_app && i < m_app->windowCount(); ++i)
        if (m_app->window(i)->id() == id) return m_app->window(i);
    return nullptr;
}

// ---- hybrid: deferred game side ------------------------------------------------------------------------------------
void ProjectHost::scanViewports(AppWindow& w) {
    fuse::ui::UiContext& ui = w.ui();
    const u32 cls = ui.registry().find("viewport");
    if (cls == 0xFFFFFFFFu) return;
    std::vector<fuse::ui::NodeHandle> stack{ui.root()};
    bool found = false;
    while (!stack.empty()) {
        const fuse::ui::NodeHandle n = stack.back();
        stack.pop_back();
        if (ui.classOf(n) == cls) {
            Viewport v;
            v.windowId = w.id();
            v.node = n;
            v.image = std::make_unique<fuse::ui::Image>();
            v.image->resize(1, 1, 0xFF000000u);
            v.imageIndex = w.addImage(v.image.get());
            fuse::ui::widgets::viewportSetImage(ui, n, v.imageIndex, 1, 1);
            m_viewports.push_back(std::move(v));
            found = true;
        }
        for (fuse::ui::NodeHandle c = ui.firstChild(n); c.valid(); c = ui.nextSibling(c)) stack.push_back(c);
    }
    if (found) armViewportTimer(w, false);
}

void ProjectHost::armViewportTimer(AppWindow& w, bool fast) {
    for (auto& [id, isFast] : m_viewportFast)
        if (id == w.id()) {
            if (isFast == fast) return;
            isFast = fast;
        }
    if (std::none_of(m_viewportFast.begin(), m_viewportFast.end(), [&](const auto& p) { return p.first == w.id(); }))
        m_viewportFast.emplace_back(w.id(), fast);
    for (auto it = m_viewportTimers.begin(); it != m_viewportTimers.end(); ++it)
        if (it->first == w.id()) {
            w.ui().cancelTimer(it->second);
            m_viewportTimers.erase(it);
            break;
        }
    const u32 id = w.id();
    m_viewportTimers.emplace_back(id, w.ui().startTimer(fast ? kViewportFrame : kViewportPoll, true,
                                                        [this, id](fuse::ui::UiContext&) { viewportTick(id); }));
}

void ProjectHost::viewportTick(u32 windowId) {
    AppWindow* w = windowById(windowId);
    if (!w) return;
    fuse::ui::UiContext& ui = w->ui();
    bool visible = false;
    for (const Viewport& v : m_viewports)
        if (v.windowId == windowId && ui.alive(v.node)) {
            const fuse::ui::Rect r = ui.rect(v.node);
            visible = visible || (r.w > 1.f && r.h > 1.f);
        }
    if (!visible) {
        armViewportTimer(*w, false);
        return;
    }
    if (!ensureGameSide()) return;
    // a late update fires the repeating timer several times in a row: one game frame per update
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (m_lastGameFrame >= 0.0 && now - m_lastGameFrame < 0.004) return;
    m_lastGameFrame = now;
    if (m_gpu && gpuViewportFrame(*w)) {
        ++m_viewportFrames;
        armViewportTimer(*w, true);
        return;
    }
    u32 fw = 0, fh = 0;
    const unsigned char* px = m_backend.frame(m_game, kViewportFrame, fw, fh);
    for (Viewport& v : m_viewports) {
        if (v.windowId != windowId || !ui.alive(v.node) || !px) continue;
        v.image->externalSlot = -1;
        v.image->resize(int(fw), int(fh));
        std::memcpy(v.image->pixels.data(), px, std::size_t(fw) * fh * 4);
        fuse::ui::widgets::viewportSetImage(ui, v.node, v.imageIndex, fw, fh);
        ui.invalidate(v.node);
    }
    ++m_viewportFrames;
    armViewportTimer(*w, true);
}

bool ProjectHost::gpuViewportFrame(AppWindow& w) {
    fuse::ui::UiContext& ui = w.ui();
    // one frame per window, at the device-pixel size of its largest viewport
    u32 width = 0, height = 0;
    for (const Viewport& v : m_viewports)
        if (v.windowId == w.id() && ui.alive(v.node)) {
            const fuse::ui::Rect r = ui.contentRect(v.node); // the image area (viewportImageRect at 1:1)
            width = std::max(width, u32(std::lround(r.w * w.scale())));
            height = std::max(height, u32(std::lround(r.h * w.scale())));
        }
    if (width < 2 || height < 2) return false;
    const bool gpuDrawn = m_native && m_native->gpuDraws(w);
    GameSideGpuFrame f;
    if (!m_backend.gpuFrame(m_game, m_gpu, kViewportFrame, width, height, !gpuDrawn, f)) return false;
    // zero-copy: the window's draw kernel samples the game frame on the shared device
    const bool zeroCopy = gpuDrawn && f.texture.image && m_native->setExternalTexture(w, kViewportExternalSlot, f.texture);
    const unsigned char* px = f.pixels;
    if (!zeroCopy && !px && m_backend.gpuReadback) px = m_backend.gpuReadback(m_gpu, f.width, f.height);
    if (!zeroCopy && !px) return false;
    for (Viewport& v : m_viewports) {
        if (v.windowId != w.id() || !ui.alive(v.node)) continue;
        if (zeroCopy) {
            // the CPU pixels are only the rasteriser's fallback: written once per size
            v.image->externalSlot = int(kViewportExternalSlot);
            if (v.image->width != int(f.width) || v.image->height != int(f.height))
                v.image->resize(int(f.width), int(f.height), 0xFF000000u);
        } else {
            v.image->externalSlot = -1;
            v.image->resize(int(f.width), int(f.height));
            std::memcpy(v.image->pixels.data(), px, std::size_t(f.width) * f.height * 4);
            for (u32& p : v.image->pixels) p |= 0xFF000000u; // the scene frame is opaque
        }
        fuse::ui::widgets::viewportSetImage(ui, v.node, v.imageIndex, f.width, f.height);
        ui.invalidate(v.node);
    }
    if (zeroCopy) ++m_zeroCopyFrames;
    return true;
}

bool ProjectHost::viewportReadback(std::vector<u32>& out, u32& width, u32& height) {
    if (!m_gpu || !m_backend.gpuReadback) return false;
    const unsigned char* px = m_backend.gpuReadback(m_gpu, width, height);
    if (!px || width == 0 || height == 0) return false;
    out.resize(std::size_t(width) * height);
    std::memcpy(out.data(), px, out.size() * 4);
    for (u32& p : out) p |= 0xFF000000u;
    return true;
}

bool ProjectHost::ensureGameSide() {
    if (m_game) return true;
    if (m_manifest.kind != project::ProjectKind::Hybrid || !m_backend.create) return false;
    if (!core::isInitialized()) {
        core::initialize();
        m_startedJobs = true;
    }
    m_up |= hostFeatureBit(HostFeature::Jobs);
    std::string error;
    m_game = m_backend.create(m_manifest, error);
    if (!error.empty()) std::fprintf(stderr, "%s: game side: %s\n", m_manifest.name.c_str(), error.c_str());
    if (!m_game) return false;
    m_up |= m_plan.deferred; // GameFrame + Renderer3D / 2D + Physics per the dimensions
    return true;
}

void* ProjectHost::gameSideHandle() const { return m_game && m_backend.handle ? m_backend.handle(m_game) : nullptr; }

} // namespace fuse::app
