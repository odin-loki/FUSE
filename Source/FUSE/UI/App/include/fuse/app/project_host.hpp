// fuse_app_project (plan UI5): the minimal init path for `project.json` schema 2 app projects. ProjectHost loads the
// manifest, brings up only what InitPlan asks for (the fuse_app shell, AppPresent, optionally the job scheduler),
// opens the declared windows with their .fuseui documents and style sheets, and runs the on-demand loop. Game worlds,
// GpuScene, FrameComposer and physics are never created here; a hybrid project's viewport creates them on demand.
//
//   auto host = fuse::app::ProjectHost::start({.projectPath = "MyTool"});
//   if (!host) return std::fprintf(stderr, "%s\n", host.error().c_str()), 1;
//   return (*host)->run();
//
// In-tree only (needs fuse_project and fuse_core); `fuse_app_host <project>` is the runner executable.
#pragma once

#include "fuse/app/app.hpp"
#include "fuse/app/native.hpp"

#include <fuse/project/manifest.hpp>

#include <expected>
#include <memory>
#include <string>
#include <vector>

namespace fuse::ui::doc {
class Model;
}
namespace fuse::hybrid {
class HybridComposer;
}

namespace fuse::app {

struct ProjectHostDesc {
    std::string projectPath;   ///< project directory or its project.json
    int argc = 0;
    const char* const* argv = nullptr;
    bool showWindows = true;   ///< false: headless presenters (tests, servers); true: OS windows when a display exists
    NativeOptions native{};
};

/// Milliseconds from start() entry to each step (steady clock).
struct StartupTimes {
    double manifest = 0.0;   ///< project.json read + validated
    double subsystems = 0.0; ///< InitPlan features up
    double windows = 0.0;    ///< windows created, documents loaded, presenters installed
    double firstFrame = 0.0; ///< every window presented once
};

class ProjectHost {
public:
    /// Loads and validates the project, brings up its InitPlan and presents the first frame of every window. Fails for
    /// game projects (run them with the game runtime) and for required features this host cannot provide.
    static std::expected<std::unique_ptr<ProjectHost>, std::string> start(const ProjectHostDesc& desc);
    ~ProjectHost(); ///< closes windows, clears AppPresent, shuts the job scheduler down if it started it
    ProjectHost(const ProjectHost&) = delete;
    ProjectHost& operator=(const ProjectHost&) = delete;

    Application& app() { return *m_app; }
    NativePlatform* native() { return m_native.get(); }
    const project::ProjectManifest& manifest() const { return m_manifest; }
    const project::InitPlan& plan() const { return m_plan; }
    /// Features actually brought up (equals plan().startup on success).
    project::HostFeatureSet broughtUp() const { return m_up; }
    const StartupTimes& times() const { return m_times; }
    /// Window by manifest id (null when unknown or closed).
    AppWindow* window(std::string_view id);
    /// The binding model shared by every window's document.
    fuse::ui::doc::Model& model() { return *m_model; }
    /// Runs the on-demand loop until quit or the last window closes.
    int run() { return m_app->run(); }

    /// Features this host can bring up at start (AppPresent, Jobs, Viewport); a hybrid project's deferred game side
    /// (GameFrame, Renderer3D / 2D, Physics) comes up through ensureGameSide().
    static project::HostFeatureSet supportedFeatures();

    // ---- hybrid projects: the deferred game side ----
    /// Brings the game side up now (once): the job scheduler, a HybridComposer with the project's dimension flags, the
    /// default 3D / 2D worlds when their .fuselevel exists, physics for 3D. ProjectHost calls it itself when a
    /// `viewport` widget of a window becomes visible (checked 4x a second while the game side is down; 60 Hz frames
    /// while a viewport shows). False for app projects or when bring-up failed.
    bool ensureGameSide();
    bool gameSideUp() const { return m_game != nullptr; }
    /// The game side's composer (null until it is up). Its software frame feeds every visible viewport widget.
    hybrid::HybridComposer* composer();
    /// `viewport` widgets found in the windows' documents.
    u32 viewportCount() const { return u32(m_viewports.size()); }
    /// Game frames rendered into viewports so far.
    u64 viewportFrames() const { return m_viewportFrames; }

private:
    struct GameSide;
    struct Viewport {
        u32 windowId = 0;
        fuse::ui::NodeHandle node{};
        std::unique_ptr<fuse::ui::Image> image; ///< registered with the window's rasteriser
        u32 imageIndex = 0;
    };
    ProjectHost();
    void scanViewports(AppWindow& w);
    void armViewportTimer(AppWindow& w, bool fast);
    void viewportTick(u32 windowId);
    AppWindow* windowById(u32 id);
    std::unique_ptr<GameSide> m_game;
    std::vector<Viewport> m_viewports;
    std::vector<std::pair<u32, fuse::ui::TimerHandle>> m_viewportTimers; ///< window id -> poll / frame timer
    std::vector<std::pair<u32, bool>> m_viewportFast;                     ///< window id -> 60 Hz (true) or polling
    u64 m_viewportFrames = 0;
    project::ProjectManifest m_manifest;
    project::InitPlan m_plan;
    project::HostFeatureSet m_up = 0;
    StartupTimes m_times;
    std::unique_ptr<fuse::ui::doc::Model> m_model;
    std::unique_ptr<Application> m_app;
    std::unique_ptr<NativePlatform> m_native;
    std::vector<std::pair<std::string, u32>> m_windowIds; ///< manifest id -> AppWindow::id()
    bool m_startedJobs = false;
};

} // namespace fuse::app
