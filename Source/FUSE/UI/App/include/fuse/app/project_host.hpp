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

    /// Features this host can bring up (AppPresent, Jobs; Viewport is accepted and deferred).
    static project::HostFeatureSet supportedFeatures();

private:
    ProjectHost() = default;
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
