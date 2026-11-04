// Minimal init path for schema 2 app projects (see project_host.hpp).
#include "fuse/app/project_host.hpp"
#include "fuse/ui/doc/document.hpp"

#include <fuse/core/init.hpp>
#include <fuse/core/track_b.hpp>
#include <fuse/project/loader.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fuse::app {

namespace {
using project::HostFeature;
using project::hostFeatureBit;

std::expected<std::string, std::string> readText(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::unexpected("cannot open " + path.string());
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
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
    host->m_plan = project::planInit(m);
    const project::HostFeatureSet unsupported = (host->m_plan.startup | m.requiredFeatures) & ~supportedFeatures();
    if (unsupported != 0u)
        return std::unexpected("project '" + m.name + "' needs host features this app host does not provide: " +
                               project::hostFeatureList(unsupported));
    host->m_times.manifest = ms();

    // Subsystems: only what the plan asks for. No worlds, GpuScene, FrameComposer or physics.
    if (host->m_plan.has(HostFeature::Jobs) && !core::isInitialized()) {
        core::initialize();
        host->m_startedJobs = true;
        host->m_up |= hostFeatureBit(HostFeature::Jobs);
    } else if (host->m_plan.has(HostFeature::Jobs)) {
        host->m_up |= hostFeatureBit(HostFeature::Jobs);
    }
    if (host->m_plan.has(HostFeature::AppPresent)) {
        core::setTrackBHostFeature(core::TrackBHostFeature::AppPresent, true);
        host->m_up |= hostFeatureBit(HostFeature::AppPresent);
    }
    host->m_times.subsystems = ms();

    // Application + windows. A manifest without windows gets one "main" window titled after the project.
    host->m_app = std::make_unique<Application>(AppDesc{.name = m.name, .argc = desc.argc, .argv = desc.argv, .loadSettings = false});
    host->m_model = std::make_unique<fuse::ui::doc::Model>();
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
    if (desc.showWindows) {
        host->m_native = std::make_unique<NativePlatform>(*host->m_app, desc.native);
        if (!host->m_native->available()) host->m_native.reset(); // no display: headless presenters
    }
    for (const project::WindowSettings& ws : windows) {
        const std::string title = ws.title.empty() ? m.name : ws.title;
        AppWindow& w = host->m_app->createWindow({.title = title, .width = ws.width, .height = ws.height});
        w.setRedrawPolicy(ws.redraw == project::RedrawMode::Continuous ? RedrawPolicy::Continuous : RedrawPolicy::OnDemand);
        for (const std::string& css : sheets)
            if (auto added = w.ui().addStyleSheet(css); !added) return std::unexpected("ui.styles: invalid style sheet");
        const std::string& doc = ws.root.empty() ? m.ui.root : ws.root;
        if (auto ok = w.load((root / doc).string(), host->m_model.get()); !ok)
            return std::unexpected("window '" + ws.id + "': " + ok.error());
        if (host->m_native) host->m_native->show(w);
        host->m_windowIds.emplace_back(ws.id, w.id());
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
    m_app.reset();
    m_model.reset();
    if ((m_up & hostFeatureBit(HostFeature::AppPresent)) != 0u)
        core::setTrackBHostFeature(core::TrackBHostFeature::AppPresent, false);
    if (m_startedJobs) core::shutdown();
}

AppWindow* ProjectHost::window(std::string_view id) {
    for (const auto& [name, windowId] : m_windowIds) {
        if (name != id) continue;
        for (u32 i = 0; i < m_app->windowCount(); ++i)
            if (m_app->window(i)->id() == windowId) return m_app->window(i);
    }
    return nullptr;
}

} // namespace fuse::app
