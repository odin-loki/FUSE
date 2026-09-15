#include <fuse/core/init.hpp>
#include <fuse/editor/asset_browser.hpp>
#include <fuse/editor/console_panel.hpp>
#include <fuse/editor/play_mode_controller.hpp>
#include <fuse/editor/profiler_panel.hpp>
#include <fuse/scene/scene.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void testAssetBrowserScansProjectRoot() {
    const std::filesystem::path tempRoot =
        std::filesystem::temp_directory_path() / "fuse_editor_b69";
    std::filesystem::remove_all(tempRoot);
    std::filesystem::create_directories(tempRoot / "meshes");

    {
        std::ofstream mesh(tempRoot / "hero.obj");
        mesh << "o hero\n";
    }
    {
        std::ofstream script(tempRoot / "bootstrap.lua");
        script << "function on_start() end\n";
    }
    {
        std::ofstream ignored(tempRoot / "notes.txt");
        ignored << "readme\n";
    }

    fuse::editor::AssetBrowser browser;
    browser.init(tempRoot.string().c_str());
    expectTrue(!browser.entries().empty(), "asset browser lists project root entries");

    bool foundMesh = false;
    bool foundScript = false;
    bool foundFolder = false;
    for (const fuse::editor::AssetBrowser::AssetEntry& entry : browser.entries()) {
        if (entry.type == fuse::editor::AssetBrowser::AssetType::Mesh) {
            foundMesh = true;
        }
        if (entry.type == fuse::editor::AssetBrowser::AssetType::Script) {
            foundScript = true;
        }
        if (entry.type == fuse::editor::AssetBrowser::AssetType::Folder) {
            foundFolder = true;
        }
    }

    expectTrue(foundMesh, "asset browser classifies mesh file");
    expectTrue(foundScript, "asset browser classifies script file");
    expectTrue(foundFolder, "asset browser lists folders");

    browser.setSearchFilter("hero");
    expectTrue(browser.entries().size() == 1u, "asset browser search filter narrows results");
    expectTrue(browser.selectEntry(browser.entries().front().path.c_str()),
               "asset browser selects filtered entry");
    expectTrue(browser.selectedEntry() != nullptr, "asset browser exposes selected entry");

    browser.setSearchFilter("");
    browser.setCurrentDirectory("meshes");
    expectTrue(browser.entries().empty(), "asset browser lists empty child directory");
}

void testProfilerPanelRingBuffer() {
    fuse::editor::ProfilerPanel panel;
    panel.setTargetFps(120.f);

    fuse::editor::ProfilerPanel::FrameProfileData first{};
    first.cpuMs = 4.f;
    first.gpuMs = 8.f;
    first.drawCalls = 12;
    panel.pushFrameData(first);

    fuse::editor::ProfilerPanel::FrameProfileData second{};
    second.cpuMs = 5.f;
    second.gpuMs = 9.f;
    second.drawCalls = 14;
    panel.pushFrameData(second);

    expectTrue(panel.frameCount() == 2u, "profiler records pushed frames");
    expectTrue(panel.latestFrame().gpuMs == 9.f, "profiler latest frame matches last push");
    expectTrue(panel.frameAt(0).gpuMs == 8.f, "profiler history preserves oldest frame");

    panel.setPaused(true);
    fuse::editor::ProfilerPanel::FrameProfileData ignored{};
    ignored.gpuMs = 99.f;
    panel.pushFrameData(ignored);
    expectTrue(panel.latestFrame().gpuMs == 9.f, "profiler ignores pushes while paused");

    panel.clearHistory();
    expectTrue(panel.frameCount() == 0u, "profiler clear resets history");
}

void testConsolePanelFiltersAndRepeat() {
    fuse::editor::ConsolePanel console;
    console.addLog(fuse::editor::ConsolePanel::LogLevel::Info, "boot", 1000);
    console.addLog(fuse::editor::ConsolePanel::LogLevel::Info, "boot", 1001);
    console.addLog(fuse::editor::ConsolePanel::LogLevel::Warn, "shader warning", 1002);
    console.addLog(fuse::editor::ConsolePanel::LogLevel::Error, "fatal", 1003);

    expectTrue(console.lines().size() == 3u, "console coalesces duplicate info lines");
    expectTrue(console.lines().front().repeatCount == 2u, "console tracks repeat count");

    console.setShowWarnings(false);
    const auto filtered = console.filteredLines();
    expectTrue(filtered.size() == 2u, "console level filter hides warnings");

    console.setShowWarnings(true);
    console.setTextFilter("boot");
    expectTrue(console.filteredLines().size() == 1u, "console text filter matches message");

    expectTrue(console.executeCommand("help"), "console command execution succeeds");
    expectTrue(console.lastExecutedCommand() == "help", "console stores last executed command");
}

void testPlayModeControllerSnapshotRestore() {
    fuse::scene::Scene scene("EditorScene");
    scene.addObjectName("Player");
    scene.addObjectName("CameraRig");
    scene.camera().setPosition(1.f, 2.f, 3.f);

    fuse::editor::PlayModeController controller;
    fuse::editor::PlayModePhysicsState physics;

    controller.enterPlay(scene, physics);
    expectTrue(controller.isPlaying(), "play mode enters playing state");
    expectTrue(physics.simulationActive, "play mode enables physics simulation flag");
    expectTrue(controller.hasSnapshot(), "play mode captures scene snapshot");

    scene.addObjectName("RuntimeSpawn");
    scene.setName("Mutated");
    scene.camera().setPosition(9.f, 9.f, 9.f);

    controller.pause(scene, physics);
    expectTrue(controller.isPaused(), "play mode pauses");
    expectTrue(!physics.simulationActive, "play mode pauses physics simulation");

    controller.resume(scene, physics);
    expectTrue(controller.isPlaying(), "play mode resumes");
    expectTrue(physics.simulationActive, "play mode resumes physics simulation");

    controller.stop(scene, physics);
    expectTrue(!controller.isPlaying(), "play mode stops");
    expectTrue(!controller.hasSnapshot(), "play mode clears snapshot on stop");
    expectTrue(scene.name() == "EditorScene", "play mode restores scene name");
    expectTrue(scene.objectCount() == 2u, "play mode restores object table");
    expectTrue(scene.objectNames().back() == "CameraRig", "play mode restores object order");
    expectTrue(scene.camera().positionX == 1.f, "play mode restores camera position");
}

} // namespace

int main() {
    fuse::core::initialize();
    testAssetBrowserScansProjectRoot();
    testProfilerPanelRingBuffer();
    testConsolePanelFiltersAndRepeat();
    testPlayModeControllerSnapshotRestore();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_editor_panels_b69_b612_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_editor_panels_b69_b612_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
