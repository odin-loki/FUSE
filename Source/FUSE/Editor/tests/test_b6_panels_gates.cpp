// B6.8–B6.11 / B6.13 gates — SDF sculpt ops, asset browser model, profiler ring buffer, console.
#include <fuse/core/init.hpp>
#include <fuse/editor/asset_browser.hpp>
#include <fuse/editor/console_panel.hpp>
#include <fuse/editor/editor_scene.hpp>
#include <fuse/editor/editor_state.hpp>
#include <fuse/editor/profiler_panel.hpp>
#include <fuse/editor/sdf_sculpt_panel.hpp>
#include <fuse/editor/undo_stack.hpp>
#include <fuse/ecs/components/sdf_object.hpp>
#include <fuse/ecs/components/transform.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

struct SdfSpawn {
    fuse::ecs::vec3 position;
    fuse::ecs::SDFObject sdf;
};

std::vector<SdfSpawn> spawnedPrimitives(fuse::editor::EditorScene& scene, fuse::ecs::EntityID skip) {
    std::vector<SdfSpawn> out;
    scene.registry().each<fuse::ecs::Transform, fuse::ecs::SDFObject>(
        [&](fuse::ecs::EntityID id, const fuse::ecs::Transform& t, const fuse::ecs::SDFObject& sdf) {
            if (id != skip) {
                out.push_back({t.position, sdf});
            }
        });
    return out;
}

// B6.8: Add brush creates an SDF primitive at the hit point; X symmetry mirrors across X=0;
// stroke spacing suppresses redundant samples; one undo step per sample.
void testSculptAddBrushAndSymmetry() {
    fuse::editor::EditorScene scene;
    scene.init(64);
    const fuse::ecs::EntityID target = scene.registry().create();
    scene.registry().add(target, fuse::ecs::Transform{});
    scene.registry().add(target, fuse::ecs::SDFObject{});

    fuse::editor::EditorState state;
    state.primarySelection = target;

    fuse::editor::SdfSculptPanel panel;
    fuse::editor::UndoStack undo;
    panel.sync(state, scene);
    panel.setBrushRadius(0.75f);
    panel.setBlendAlpha(0.2f);
    panel.setMaterialId(9u);
    panel.setBrushShape(fuse::ecs::SDFPrimitive::Box);

    const fuse::ecs::vec3 normal{0.f, 1.f, 0.f, 0.f};
    expectTrue(panel.applyBrushStroke({2.f, 1.f, -3.f, 0.f}, normal, scene, undo), "add stroke applied");
    std::vector<SdfSpawn> spawned = spawnedPrimitives(scene, target);
    expectTrue(spawned.size() == 1u, "one primitive spawned");
    expectTrue(spawned.size() == 1u && spawned[0].position.x == 2.f && spawned[0].position.y == 1.f &&
                   spawned[0].position.z == -3.f,
               "primitive at the exact hit point");
    expectTrue(spawned.size() == 1u && spawned[0].sdf.type == fuse::ecs::SDFPrimitive::Box &&
                   spawned[0].sdf.params.x == 0.75f && spawned[0].sdf.blend_alpha == 0.2f &&
                   spawned[0].sdf.material_id == 9u,
               "primitive carries brush shape, radius, alpha and material");

    expectTrue(!panel.applyBrushStroke({2.1f, 1.f, -3.f, 0.f}, normal, scene, undo),
               "sample closer than the spacing is suppressed");
    expectTrue(spawnedPrimitives(scene, target).size() == 1u, "no redundant dispatch at slow cursor speed");

    panel.setSymmetryX(true);
    expectTrue(panel.applyBrushStroke({3.f, 0.5f, 1.f, 0.f}, normal, scene, undo), "symmetric stroke applied");
    spawned = spawnedPrimitives(scene, target);
    bool mirrored = false;
    for (const SdfSpawn& s : spawned) {
        mirrored = mirrored || (s.position.x == -3.f && s.position.y == 0.5f && s.position.z == 1.f);
    }
    expectTrue(spawned.size() == 3u && mirrored, "X symmetry mirrors the stroke across X=0");
    expectTrue(undo.undoCount() == 2u, "stroke + mirror are one undo step");

    panel.endStroke();
    expectTrue(panel.applyBrushStroke({3.f, 0.5f, 1.f, 0.f}, normal, scene, undo) &&
                   spawnedPrimitives(scene, target).size() == 5u,
               "a new stroke at the same spot emits after endStroke");

    undo.undo();
    undo.undo();
    expectTrue(spawnedPrimitives(scene, target).size() == 1u, "undo removes stroke primitives incl. mirror");
    undo.redo();
    expectTrue(spawnedPrimitives(scene, target).size() == 3u, "redo recreates them");

    panel.setBrushOperation(fuse::editor::SdfSculptPanel::BrushOp::Subtract);
    panel.endStroke();
    expectTrue(!panel.applyBrushStroke({5.f, 0.f, 0.f, 0.f}, normal, scene, undo),
               "subtract needs an SDF CSG op on SDFObject (not modelled) — rejected, not faked");
    scene.destroy();
}

void writeFile(const std::filesystem::path& path) {
    std::ofstream(path) << "x";
}

// B6.9: asset browser classification, folders-first ordering, case-insensitive search, selection.
void testAssetBrowserModel() {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "fuse_b6_asset_browser_gate";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "Textures", ec);
    fs::create_directories(root / "meshes", ec);
    writeFile(root / "Rock.GLB");
    writeFile(root / "grass_albedo.png");
    writeFile(root / "Level01.fuse");
    writeFile(root / "footstep.wav");
    writeFile(root / "readme.txt");
    writeFile(root / "Textures" / "rock_normal.dds");

    fuse::editor::AssetBrowser browser;
    browser.init(root.string().c_str());
    const std::vector<fuse::editor::AssetBrowser::AssetEntry>& entries = browser.entries();
    expectTrue(entries.size() == 7u, "all top-level entries listed");
    expectTrue(entries.size() >= 2u && entries[0].type == fuse::editor::AssetBrowser::AssetType::Folder &&
                   entries[1].type == fuse::editor::AssetBrowser::AssetType::Folder,
               "folders sort first");
    int meshes = 0;
    int textures = 0;
    int scenes = 0;
    int audio = 0;
    int unknown = 0;
    for (const auto& e : entries) {
        meshes += e.type == fuse::editor::AssetBrowser::AssetType::Mesh;
        textures += e.type == fuse::editor::AssetBrowser::AssetType::Texture;
        scenes += e.type == fuse::editor::AssetBrowser::AssetType::Scene;
        audio += e.type == fuse::editor::AssetBrowser::AssetType::Audio;
        unknown += e.type == fuse::editor::AssetBrowser::AssetType::Unknown;
    }
    expectTrue(meshes == 1 && textures == 1 && scenes == 1 && audio == 1 && unknown == 1,
               "extensions classified case-insensitively (.GLB is a mesh)");

    browser.setSearchFilter("ROCK");
    expectTrue(browser.entries().size() == 1u && browser.entries()[0].name == "Rock.GLB",
               "search is case-insensitive and filters the current folder");
    browser.setSearchFilter("");
    expectTrue(browser.selectEntry("grass_albedo.png") && browser.selectedEntry() != nullptr,
               "selection by relative path");
    browser.setSearchFilter("level");
    expectTrue(browser.selectedEntry() == nullptr, "selection hidden by the filter is not reported");
    browser.setSearchFilter("");
    browser.setCurrentDirectory("Textures");
    expectTrue(browser.entries().size() == 1u && browser.entries()[0].path == "Textures/rock_normal.dds",
               "navigating into a folder lists project-relative paths");
    fs::remove_all(root, ec);
}

// B6.10: ring buffer order has no off-by-one; pause freezes history while frames keep arriving.
void testProfilerRingBuffer() {
    fuse::editor::ProfilerPanel profiler;
    constexpr fuse::u32 kN = fuse::editor::ProfilerPanel::kHistoryFrames;
    for (fuse::u32 i = 0; i < kN + 37u; ++i) {
        fuse::editor::ProfilerPanel::FrameProfileData frame{};
        frame.cpuMs = static_cast<float>(i);
        frame.drawCalls = i;
        profiler.pushFrameData(frame);
    }
    expectTrue(profiler.frameCount() == kN, "history saturates at kHistoryFrames");
    bool ordered = true;
    for (fuse::u32 k = 0; k < kN; ++k) {
        ordered = ordered && profiler.frameAt(k).drawCalls == 37u + k;
    }
    expectTrue(ordered, "frameAt(0) is the oldest retained frame and indices are contiguous");
    expectTrue(profiler.latestFrame().drawCalls == kN + 36u, "latest frame is the last pushed");

    profiler.setPaused(true);
    for (fuse::u32 i = 0; i < 10u; ++i) {
        fuse::editor::ProfilerPanel::FrameProfileData frame{};
        frame.drawCalls = 9999u;
        profiler.pushFrameData(frame); // engine keeps producing frames
    }
    expectTrue(profiler.latestFrame().drawCalls == kN + 36u && profiler.frameAt(0).drawCalls == 37u,
               "paused display is frozen");
    profiler.setPaused(false);
    fuse::editor::ProfilerPanel::FrameProfileData resumed{};
    resumed.drawCalls = 7u;
    profiler.pushFrameData(resumed);
    expectTrue(profiler.latestFrame().drawCalls == 7u && profiler.frameAt(0).drawCalls == 38u,
               "resume continues scrolling from the frozen history");

    fuse::editor::ProfilerPanel partial;
    fuse::editor::ProfilerPanel::FrameProfileData one{};
    one.drawCalls = 1u;
    partial.pushFrameData(one);
    one.drawCalls = 2u;
    partial.pushFrameData(one);
    expectTrue(partial.frameCount() == 2u && partial.frameAt(0).drawCalls == 1u &&
                   partial.frameAt(1).drawCalls == 2u,
               "partially filled history is ordered oldest..newest");
}

// B6.11: command parsing, dispatch, errors, and history recall.
void testConsoleParsingAndHistory() {
    fuse::editor::ConsolePanel::ParsedCommand parsed;
    expectTrue(fuse::editor::ConsolePanel::parseCommandLine("  Spawn  crate \"big box\" 3  ", parsed) &&
                   parsed.name == "spawn" && parsed.args.size() == 3u && parsed.args[0] == "crate" &&
                   parsed.args[1] == "big box" && parsed.args[2] == "3",
               "whitespace split, quoted token, lower-cased name");
    expectTrue(fuse::editor::ConsolePanel::parseCommandLine("say \"a \\\"quoted\\\" word\" \"\"", parsed) &&
                   parsed.args.size() == 2u && parsed.args[0] == "a \"quoted\" word" && parsed.args[1].empty(),
               "escaped quotes and empty quoted argument");
    expectTrue(!fuse::editor::ConsolePanel::parseCommandLine("   ", parsed), "blank line rejected");
    expectTrue(!fuse::editor::ConsolePanel::parseCommandLine("echo \"unterminated", parsed),
               "unterminated quote rejected");

    fuse::editor::ConsolePanel console;
    std::vector<std::string> received;
    console.registerCommand("Echo", [&received](const std::vector<std::string>& args,
                                                fuse::editor::ConsolePanel&) {
        received = args;
        return !args.empty();
    });
    expectTrue(console.hasCommand("echo") && console.hasCommand("HELP"), "registered + built-in commands");
    expectTrue(console.executeCommand("ECHO hello \"big world\""), "dispatch to registered handler");
    expectTrue(received.size() == 2u && received[1] == "big world", "handler receives parsed arguments");
    expectTrue(!console.executeCommand("echo"), "handler failure propagates");
    expectTrue(!console.executeCommand("nosuchcmd 1"), "unknown command fails");
    bool loggedUnknown = false;
    for (const auto& line : console.lines()) {
        loggedUnknown = loggedUnknown || (line.level == fuse::log::Level::Error &&
                                          line.text == "unknown command: nosuchcmd");
    }
    expectTrue(loggedUnknown, "unknown command logged as error");
    expectTrue(!console.executeCommand(""), "empty line is not executed");

    console.executeCommand("help");
    console.executeCommand("help");
    const std::vector<std::string>& history = console.commandHistory();
    expectTrue(history.size() == 4u && history.back() == "help", "history records executed lines, dedupes repeats");
    expectTrue(console.historyPrevious() == "help", "up recalls newest");
    expectTrue(console.historyPrevious() == "nosuchcmd 1", "up again recalls older");
    expectTrue(console.historyNext() == "help", "down steps back toward newest");
    expectTrue(console.historyNext().empty(), "down past newest yields an empty line");
    for (int i = 0; i < 6; ++i) {
        console.historyPrevious();
    }
    expectTrue(console.historyPrevious() == "ECHO hello \"big world\"", "up stops at the oldest entry");

    for (int i = 0; i < 100; ++i) {
        console.executeCommand(("echo " + std::to_string(i)).c_str());
    }
    expectTrue(console.commandHistory().size() == fuse::editor::ConsolePanel::kMaxCommandHistory &&
                   console.commandHistory().back() == "echo 99",
               "history is bounded, keeps newest");

    expectTrue(console.executeCommand("clear") && console.lines().empty(), "built-in clear empties the log");
}

} // namespace

int main() {
    fuse::core::initialize();

    testSculptAddBrushAndSymmetry();
    testAssetBrowserModel();
    testProfilerRingBuffer();
    testConsoleParsingAndHistory();

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_editor_b6_panels_gates: all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "fuse_editor_b6_panels_gates: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
