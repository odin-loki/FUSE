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
#include <fuse/ecs/sdf_csg.hpp>

#include <algorithm>
#include <cmath>
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

    scene.destroy();
}

// B6.8: Subtract / Smooth / Roughen / Paint brushes as undoable steps, proven on the CPU CSG
// evaluation of the edited scene (fuse/ecs/sdf_csg.hpp).
void testSculptCsgBrushes() {
    using fuse::editor::SdfSculptPanel;
    fuse::editor::EditorScene scene;
    scene.init(64);
    const fuse::ecs::EntityID target = scene.registry().create();
    scene.registry().add(target, fuse::ecs::Transform{});
    scene.registry().add(target, fuse::ecs::SDFObject{}); // unit sphere at the origin

    fuse::editor::EditorState state;
    state.primarySelection = target;
    SdfSculptPanel panel;
    fuse::editor::UndoStack undo;
    panel.sync(state, scene);
    const fuse::ecs::vec3 normal{1.f, 0.f, 0.f, 0.f};

    fuse::ecs::SdfCsgScene csg;
    auto sdfAt = [&](float x, float y, float z) {
        csg.build(scene.registry());
        return csg.distance({x, y, z, 0.f});
    };
    auto sampleAt = [&](float x, float y, float z) {
        csg.build(scene.registry());
        return csg.sample({x, y, z, 0.f});
    };

    // Subtract: every point inside both the sphere and the brush flips from solid to empty.
    const fuse::ecs::vec3 hit{1.f, 0.f, 0.f, 0.f};
    const float brushR = 0.5f;
    std::vector<fuse::ecs::vec3> probes;
    for (int i = -4; i <= 4; ++i) {
        for (int j = -4; j <= 4; ++j) {
            for (int k = -4; k <= 4; ++k) {
                const fuse::ecs::vec3 p{hit.x + 0.11f * i, hit.y + 0.11f * j, hit.z + 0.11f * k, 0.f};
                const float db = std::sqrt((p.x - hit.x) * (p.x - hit.x) + (p.y - hit.y) * (p.y - hit.y) +
                                           (p.z - hit.z) * (p.z - hit.z));
                const float ds = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
                if (db < brushR - 1e-3f && ds < 1.f - 1e-3f) {
                    probes.push_back(p);
                }
            }
        }
    }
    bool solidBefore = !probes.empty();
    for (const auto& p : probes) {
        solidBefore = solidBefore && sdfAt(p.x, p.y, p.z) < 0.f;
    }
    expectTrue(solidBefore, "probes inside sphere ∩ brush are solid before the stroke");

    panel.setBrushOperation(SdfSculptPanel::BrushOp::Subtract);
    panel.setBrushRadius(brushR);
    expectTrue(panel.applyBrushStroke(hit, normal, scene, undo), "subtract stroke applied");
    csg.build(scene.registry());
    bool carved = true;
    for (const auto& p : probes) {
        carved = carved && csg.distance(p) > 0.f;
    }
    expectTrue(carved, "subtract: SDF sign flips to empty everywhere inside the brush sphere");
    expectTrue(sdfAt(0.f, 0.f, 0.f) < 0.f && std::fabs(sdfAt(-0.9f, 0.f, 0.f) + 0.1f) < 1e-6f,
               "subtract: solid outside the brush unchanged");
    expectTrue(std::fabs(sdfAt(0.5f, 0.f, 0.f)) < 1e-6f, "subtract: new surface on the brush sphere");
    expectTrue(sdfAt(1.4f, 0.f, 0.f) > 0.f, "subtract adds no volume");
    expectTrue(undo.undoCount() == 1u, "subtract is one undo step");
    undo.undo();
    expectTrue(sdfAt(0.9f, 0.f, 0.f) < 0.f, "undo subtract restores the solid");
    undo.redo();
    expectTrue(sdfAt(0.9f, 0.f, 0.f) > 0.f, "redo subtract carves again");

    // Order matters: a later Add inside the cavity fills it back (fold follows stroke order).
    panel.endStroke();
    panel.setBrushOperation(SdfSculptPanel::BrushOp::Add);
    panel.setBrushRadius(0.2f);
    expectTrue(panel.applyBrushStroke({0.8f, 0.f, 0.f, 0.f}, normal, scene, undo) &&
                   sdfAt(0.8f, 0.f, 0.f) < 0.f && sdfAt(0.8f, 0.3f, 0.f) > 0.f,
               "add after subtract refills only its own volume");
    undo.undo();

    // Smooth: a SmoothUnion sphere above the target blends into it continuously.
    panel.endStroke();
    panel.setBrushOperation(SdfSculptPanel::BrushOp::Smooth);
    panel.setBrushRadius(0.5f);
    panel.setBlendAlpha(0.8f); // blend radius 0.4
    const float gapMid = 1.05f; // target top at y=1, brush sphere bottom at y=1.1
    expectTrue(sdfAt(0.f, gapMid, 0.f) > 0.f, "gap is empty before the smooth stroke");
    expectTrue(panel.applyBrushStroke({0.f, 1.6f, 0.f, 0.f}, normal, scene, undo), "smooth stroke applied");
    expectTrue(sdfAt(0.f, gapMid, 0.f) < 0.f, "smooth: blend fills the gap (hard union would not)");
    csg.build(scene.registry());
    bool continuous = true;
    float prev = csg.distance({0.f, -2.f, 0.f, 0.f});
    for (int i = 1; i <= 5000; ++i) {
        const float y = -2.f + 0.001f * static_cast<float>(i);
        const float d = csg.distance({0.f, y, 0.f, 0.f});
        continuous = continuous && std::fabs(d - prev) <= 0.001f * 1.001f + 1e-6f;
        prev = d;
    }
    expectTrue(continuous, "smooth: sampled field along the seam is continuous (1-Lipschitz)");
    const float hardAtMid = std::min(std::sqrt(gapMid * gapMid) - 1.f, 1.6f - gapMid - 0.5f);
    expectTrue(hardAtMid - sdfAt(0.f, gapMid, 0.f) <= 0.4f * 0.25f + 1e-5f, "smooth: bulge bounded by k/4");
    undo.undo();
    expectTrue(sdfAt(0.f, gapMid, 0.f) > 0.f, "undo smooth removes the blend");
    undo.redo();

    // Paint: objects whose surface is within the brush get the brush material.
    panel.endStroke();
    panel.setBrushOperation(SdfSculptPanel::BrushOp::Paint);
    panel.setBrushRadius(0.3f);
    panel.setMaterialId(42u);
    expectTrue(sampleAt(-1.01f, 0.f, 0.f).material_id == 0u, "surface material 0 before paint");
    expectTrue(panel.applyBrushStroke({-1.f, 0.f, 0.f, 0.f}, normal, scene, undo), "paint stroke applied");
    expectTrue(sampleAt(-1.01f, 0.f, 0.f).material_id == 42u && sampleAt(-1.01f, 0.f, 0.f).entity == target,
               "paint: sampled surface material changed");
    expectTrue(sampleAt(0.f, 2.15f, 0.f).material_id == 0u, "paint: objects outside the brush untouched");
    undo.undo();
    expectTrue(sampleAt(-1.01f, 0.f, 0.f).material_id == 0u, "undo paint restores the material");
    undo.redo();
    panel.endStroke();
    expectTrue(!panel.applyBrushStroke({-6.f, 0.f, 0.f, 0.f}, normal, scene, undo),
               "paint touching nothing is not an undo step");

    // Roughen: surface displacement grows per stroke, capped, undoable.
    panel.endStroke();
    panel.setBrushOperation(SdfSculptPanel::BrushOp::Roughen);
    panel.setBrushRadius(1.f);
    const float smoothSurface = sdfAt(0.f, -1.f, 0.3f);
    const fuse::u32 stepsBefore = static_cast<fuse::u32>(undo.undoCount());
    expectTrue(panel.applyBrushStroke({0.f, -1.f, 0.f, 0.f}, normal, scene, undo), "roughen stroke applied");
    expectTrue(scene.registry().get<fuse::ecs::SDFObject>(target)->roughness == SdfSculptPanel::kRoughenStep,
               "roughen raises roughness by one step");
    int displaced = 0;
    csg.build(scene.registry());
    for (int i = 0; i < 64; ++i) {
        const float a = 0.0981f * static_cast<float>(i);
        const fuse::ecs::vec3 p{-std::cos(a), -std::sin(a) * 0.6f, std::sin(a) * 0.8f, 0.f}; // on the unit sphere
        displaced += std::fabs(csg.distance(p)) > 1e-3f ? 1 : 0;
    }
    expectTrue(displaced > 32, "roughen: sampled surface is displaced");
    for (int i = 0; i < 10; ++i) {
        panel.endStroke();
        panel.applyBrushStroke({0.f, -1.f, 0.f, 0.f}, normal, scene, undo);
    }
    expectTrue(scene.registry().get<fuse::ecs::SDFObject>(target)->roughness == SdfSculptPanel::kMaxRoughness,
               "roughness capped");
    while (undo.undoCount() > stepsBefore) {
        undo.undo();
    }
    expectTrue(scene.registry().get<fuse::ecs::SDFObject>(target)->roughness == 0.f &&
                   sdfAt(0.f, -1.f, 0.3f) == smoothSurface,
               "undoing every roughen stroke restores the exact surface");
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
    testSculptCsgBrushes();
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
