// fuse_app_project (plan UI5): the minimal init path for `project.json` schema 2 app projects. ProjectHost loads the
// manifest, brings up only what InitPlan asks for (the fuse_app shell, AppPresent, optionally the job scheduler),
// opens the declared windows with their .fuseui documents and style sheets, and runs the on-demand loop. Game worlds,
// GpuScene, FrameComposer and physics are never created here; a hybrid project's viewport creates them on demand
// through a GameSideBackend (fuse_app_project_hybrid: hybridGameSide()), so app-only executables do not link them.
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

/// One game frame rendered on the game side's GPU device (GameSideBackend::gpuFrame).
struct GameSideGpuFrame {
    ExternalTexture texture{};             ///< RGBA8 target: sampled in place by a window presenting on that device
    u32 width = 0, height = 0;
    const unsigned char* pixels = nullptr; ///< RGBA8 host copy of the frame (gpuFrame with readback), else null
};

/// A hybrid project's deferred game side as a function table (fuse/app/project_host_hybrid.hpp has the engine one).
struct GameSideBackend {
    /// Brings the game side up for `m` (job scheduler already running). Null on failure (`error` says why).
    void* (*create)(const project::ProjectManifest& m, std::string& error) = nullptr;
    /// One game frame of `dt` seconds; returns its RGBA8 pixels (`width` x `height`), null when there is none.
    const unsigned char* (*frame)(void* side, f32 dt, u32& width, u32& height) = nullptr;
    void (*destroy)(void* side) = nullptr;
    /// Engine object for the app (hybrid: fuse::hybrid::HybridComposer*).
    void* (*handle)(void* side) = nullptr;

    // Optional GPU viewport (all four or none). The device is created at start, before any window opens, so a GPU
    // drawn window presents on it and samples the game frame without a copy (NativeOptions::adoptDevice +
    // NativePlatform::setExternalTexture); CPU-presented and headless windows get the frame's readback instead.
    /// Creates the game side's Vulkan device. Null when there is none (`error` says why): frame() is used instead.
    void* (*gpuCreate)(bool validation, std::string& error) = nullptr;
    VulkanDeviceHandles (*gpuDevice)(void* gpu) = nullptr;
    /// One game frame rendered on `gpu` at `width` x `height`; `readback`: also copied to host memory (out.pixels).
    /// False when the GPU frame failed (the caller falls back to frame()).
    bool (*gpuFrame)(void* side, void* gpu, f32 dt, u32 width, u32 height, bool readback, GameSideGpuFrame& out) = nullptr;
    void (*gpuDestroy)(void* gpu) = nullptr; ///< after destroy() of every side rendered on it
    /// Optional: RGBA8 host copy of the last GPU frame (waits for the GPU); null before the first one.
    const unsigned char* (*gpuReadback)(void* gpu, u32& width, u32& height) = nullptr;
};

struct ProjectHostDesc {
    std::string projectPath;   ///< project directory or its project.json
    int argc = 0;
    const char* const* argv = nullptr;
    bool showWindows = true;   ///< false: headless presenters (tests, servers); true: OS windows when a display exists
    NativeOptions native{};
    /// Required for hybrid projects (start() refuses them without one); unused by app projects.
    GameSideBackend gameSide{};
    /// Hybrid: render viewports on the game side's GPU device when the backend offers one (zero-copy on GPU drawn
    /// windows). false: always the backend's software frame().
    bool gpuViewport = true;
    bool gpuValidation = false; ///< Khronos validation on that device (gates)
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
    /// Features actually brought up (plan().startup after start; plus the game side's once it is up).
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
    /// Brings the game side up now (once): the job scheduler, then GameSideBackend::create. ProjectHost calls it
    /// itself when a `viewport` widget of a window becomes visible (checked 4x a second while the game side is down;
    /// 60 Hz frames while a viewport shows). False for app projects or when bring-up failed.
    bool ensureGameSide();
    bool gameSideUp() const { return m_game != nullptr; }
    /// GameSideBackend::handle of the running game side (hybridGameSide: the HybridComposer), null until it is up.
    void* gameSideHandle() const;
    /// `viewport` widgets found in the windows' documents.
    u32 viewportCount() const { return u32(m_viewports.size()); }
    /// Game frames rendered into viewports so far.
    u64 viewportFrames() const { return m_viewportFrames; }
    /// True when viewports are rendered on the game side's GPU device (GameSideBackend::gpuCreate succeeded).
    bool viewportGpu() const { return m_gpu != nullptr; }
    /// GPU game frames shown without a copy (a GPU drawn window sampled them in place).
    u64 viewportZeroCopyFrames() const { return m_zeroCopyFrames; }
    /// The last GPU game frame as opaque RGBA8 words (waits for the GPU). False without one.
    bool viewportReadback(std::vector<u32>& out, u32& width, u32& height);
    /// External slot of the window's GPU draw kernel that samples the game frame.
    static constexpr unsigned kViewportExternalSlot = 0;

private:
    struct Viewport {
        u32 windowId = 0;
        fuse::ui::NodeHandle node{};
        std::unique_ptr<fuse::ui::Image> image; ///< registered with the window's rasteriser
        u32 imageIndex = 0;
    };
    ProjectHost() = default;
    void scanViewports(AppWindow& w);
    void armViewportTimer(AppWindow& w, bool fast);
    void viewportTick(u32 windowId);
    AppWindow* windowById(u32 id);
    bool gpuViewportFrame(AppWindow& w);
    GameSideBackend m_backend{};
    void* m_game = nullptr;
    void* m_gpu = nullptr; ///< GameSideBackend::gpuCreate: outlives m_game and the presenters adopting its device
    u64 m_zeroCopyFrames = 0;
    double m_lastGameFrame = -1.0; ///< steady seconds of the last game frame
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
