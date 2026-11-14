// FUSE Remaster W1.8 (docs/plans/FUSE_REMASTER_PLAN.md §4.8, §7 Wave 1): golden renders of converted Torque levels on
// Lavapipe, compared with the vendored NVIDIA FLIP.
//
//   remaster_golden_render [--golden-dir D] [--artifact-dir D] [--shader-dir D] [--harness-shader-dir D] [--work D]
//
// Scenes (the track-B pipeline end to end, then the WP-0.7 G-buffer raster path):
//   example_level   Templates/BaseGame ExampleLevel.mis (MIT) -> world_converter -> .fuselevel v3 -> World3D: the
//                   ground plane, the Sun's direction (azimuth / elevation) and the LevelInfo fog colour.
//   shapes_level    Tests/fixtures/remaster/golden/shapes_level.mis (ours: ExampleLevel's environment + five TSStatics
//                   of the in-tree MIT shapes) -> cookT3DMissionShapes (the clean-room DTS reader) -> World3D cooked
//                   meshes, drawn with their entity transforms.
// For each scene: World3D -> a harness::Scene (cooked mesh triangles in world space, flat batches per (material,
// face normal); Mesh entities without a cooked mesh draw as their AABB - a quad for the plane), camera framed from
// the level content, the resolve lit by the level's sun over its fog colour. Then:
//   1. render twice from scratch: bit-identical, FLIP == 0 exactly (the W1.8 exit criterion);
//   2. compare with Tests/golden/remaster/<scene>.png: mean FLIP <= 0.05 (bit-exactness reported); artefacts on
//      failure; FUSE_UPDATE_GOLDENS=1 rewrites the goldens;
//   3. regression sensitivity: shapes_level with one TSStatic moved 0.5 m (a converter / transform regression)
//      differs from the golden (FLIP > 0, many pixels), so the gate can fail.
// Zero validation messages are required. Exit 77 without Vulkan / device / SPIR-V.
#include "flip_metric.hpp"
#include "frame_runner.hpp"
#include "golden.hpp"
#include "image_io.hpp"
#include "scene.hpp"

#include <fuse/core/init.hpp>
#include <fuse/asset/cooked_mesh.hpp>
#include <fuse/ecs/components/environment.hpp>
#include <fuse/ecs/components/light.hpp>
#include <fuse/ecs/components/mesh.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/project/world_converter.hpp>
#include <fuse/world3d/world_3d.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#ifndef FUSE_SOURCE_DIR
#define FUSE_SOURCE_DIR "."
#endif
#ifndef FUSE_REMASTER_GOLDEN_DIR
#define FUSE_REMASTER_GOLDEN_DIR "Tests/golden/remaster"
#endif
#ifndef FUSE_REMASTER_GOLDEN_ARTIFACT_DIR
#define FUSE_REMASTER_GOLDEN_ARTIFACT_DIR "remaster_golden_artifacts"
#endif
#ifndef FUSE_RP_HARNESS_SHADER_DIR
#define FUSE_RP_HARNESS_SHADER_DIR "shaders"
#endif
#ifndef FUSE_RP_STOCK_SHADER_DIR
#define FUSE_RP_STOCK_SHADER_DIR "shaders"
#endif

namespace {

namespace fs = std::filesystem;
namespace h = fuse::renderer::harness;
using fuse::u32;
using fuse::math::Vec3;

constexpr int kSkip = 77;
constexpr u32 kWidth = 960, kHeight = 540;
constexpr double kMaxMeanFlip = 0.05;
constexpr float kPlaneDrawHalfExtent = 24.f; // the infinite GroundPlane, drawn as a finite quad

int g_failures = 0;

void expect(bool condition, const std::string& message) {
    std::printf("%s: %s\n", condition ? "ok  " : "FAIL", message.c_str());
    if (!condition) {
        ++g_failures;
    }
}

std::string fmt(const char* f, double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), f, v);
    return buf;
}

struct LevelScene {
    h::Scene scene;
    h::ResolveParams resolve;
    u32 cookedShapes = 0;
    u32 drawnEntities = 0;
    std::string note;
};

Vec3 xform(const fuse::ecs::Transform& t, float x, float y, float z, float w) {
    const float* m = t.local_to_world.data.data();
    return Vec3(m[0] * x + m[4] * y + m[8] * z + m[12] * w, m[1] * x + m[5] * y + m[9] * z + m[13] * w,
                m[2] * x + m[6] * y + m[10] * z + m[14] * w);
}

Vec3 normalize(Vec3 v) {
    const float l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    return l > 0.f ? Vec3(v.x / l, v.y / l, v.z / l) : Vec3(0.f, 1.f, 0.f);
}

/// Deterministic albedo in 0.25..0.85 from a name (FNV-1a).
h::SurfaceMaterial hashedMaterial(const std::string& name) {
    std::uint32_t x = 2166136261u;
    for (unsigned char c : name) {
        x = (x ^ c) * 16777619u;
    }
    h::SurfaceMaterial m;
    m.albedo = Vec3(0.25f + 0.6f * float(x & 0xff) / 255.f, 0.25f + 0.6f * float((x >> 8) & 0xff) / 255.f,
                    0.25f + 0.6f * float((x >> 16) & 0xff) / 255.f);
    m.roughness = 0.6f;
    return m;
}

/// Converts `mission`, cooks its shapes, loads it into World3D and builds the harness scene. `nudge` moves the
/// TSStatic named `nudgeName` by (nudge, 0, 0) metres (regression-sensitivity control).
bool buildLevelScene(const std::string& name, const fs::path& mission, const fs::path& work, LevelScene& out,
                     const std::string& nudgeName = {}, float nudge = 0.f) {
    const fs::path dir = work / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "worlds", ec);
    const std::string level = (dir / "worlds" / (name + ".fuselevel")).generic_string();
    const fuse::project::ConvertResult converted = fuse::project::convertT3DMissionToFuselevel(mission.string(), level);
    if (converted.status != fuse::project::ConvertStatus::Ok || converted.unexpectedStubCount != 0u) {
        out.note = "convert failed or left unexpected stubs: " + converted.note;
        return false;
    }
    const fuse::project::T3DShapeCookResult shapes = fuse::project::cookT3DMissionShapes(
        mission.string(), (dir / "cooked").string(), {(fs::path(FUSE_SOURCE_DIR) / "Templates/BaseGame/game").string()});
    out.cookedShapes = shapes.cooked;

    fuse::world3d::World3D world;
    world.setCookedAssetRoot((dir / "cooked").string());
    const fuse::world3d::World3DLevelLoadResult loaded = world.loadWorldFromFuselevel(level);
    if (!loaded.ok) {
        out.note = "World3D load failed: " + loaded.note;
        return false;
    }
    std::map<fuse::u64, const fuse::asset::CookedMesh*> cooked;
    std::map<fuse::u64, std::string> cookedPath;
    for (const fuse::world3d::World3DCookedMesh& m : world.cookedMeshes()) {
        cooked[m.id.value] = m.mesh.get();
        cookedPath[m.id.value] = m.path;
    }
    std::map<fuse::u64, std::string> nameOf; // mesh asset id -> object name (for the nudge)
    for (const auto& e : shapes.entries) {
        nameOf[e.id.value] = e.objectName;
    }

    h::Scene& s = out.scene;
    s.name = name;
    h::SurfaceMaterial ground;
    ground.albedo = Vec3(0.42f, 0.42f, 0.40f);
    ground.roughness = 0.9f;
    s.materials.push_back(ground);
    std::map<std::string, u32> materialIndex;
    // (material, quantised normal) -> batch index, per drawn entity order (deterministic: registry order is
    // the level's entity order).
    std::map<std::tuple<u32, int, int, int>, std::size_t> batchOf;
    auto addTriangle = [&](u32 material, Vec3 a, Vec3 b, Vec3 c) {
        const Vec3 e1(b.x - a.x, b.y - a.y, b.z - a.z), e2(c.x - a.x, c.y - a.y, c.z - a.z);
        const Vec3 n = normalize(Vec3(e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x));
        const auto key = std::make_tuple(material, int(std::lround(n.x * 64.f)), int(std::lround(n.y * 64.f)), int(std::lround(n.z * 64.f)));
        auto it = batchOf.find(key);
        if (it == batchOf.end()) {
            h::Batch batch;
            batch.material = material;
            batch.normal = n;
            s.batches.push_back(batch);
            it = batchOf.emplace(key, s.batches.size() - 1).first;
        }
        auto& p = s.batches[it->second].positions;
        p.push_back(a);
        p.push_back(b);
        p.push_back(c);
    };
    Vec3 lo(1e9f, 1e9f, 1e9f), hi(-1e9f, -1e9f, -1e9f);
    bool haveShapes = false;
    world.registry().each<fuse::ecs::Mesh, fuse::ecs::Transform>([&](fuse::ecs::EntityID e, fuse::ecs::Mesh& mesh, fuse::ecs::Transform& t) {
        const fuse::ecs::MeshAssets* assets = world.registry().get<fuse::ecs::MeshAssets>(e);
        const auto it = assets ? cooked.find(assets->mesh.value) : cooked.end();
        if (it != cooked.end() && it->second) {
            const fuse::asset::CookedMesh& cm = *it->second;
            const std::string path = cookedPath[assets->mesh.value];
            auto mit = materialIndex.find(path);
            if (mit == materialIndex.end()) {
                s.materials.push_back(hashedMaterial(fs::path(path).filename().string())); // not the absolute work dir
                mit = materialIndex.emplace(path, u32(s.materials.size() - 1)).first;
            }
            const float dx = (!nudgeName.empty() && nameOf[assets->mesh.value] == nudgeName) ? nudge : 0.f;
            for (std::size_t i = 0; i + 2 < cm.indices.size(); i += 3) {
                Vec3 v[3];
                for (int k = 0; k < 3; ++k) {
                    const std::size_t vi = std::size_t(cm.indices[i + k]) * 3;
                    v[k] = xform(t, cm.positions[vi], cm.positions[vi + 1], cm.positions[vi + 2], 1.f);
                    v[k].x += dx;
                    lo = Vec3(std::min(lo.x, v[k].x), std::min(lo.y, v[k].y), std::min(lo.z, v[k].z));
                    hi = Vec3(std::max(hi.x, v[k].x), std::max(hi.y, v[k].y), std::max(hi.z, v[k].z));
                }
                addTriangle(mit->second, v[0], v[1], v[2]);
            }
            haveShapes = true;
            ++out.drawnEntities;
            return;
        }
        // No cooked mesh: the builtin plane (flat AABB) as a clipped quad, anything else as its box.
        const fuse::ecs::vec3 a = mesh.aabb_min, b = mesh.aabb_max;
        const float ex[3] = {b.x - a.x, b.y - a.y, b.z - a.z};
        if (ex[1] < 1e-3f) {
            const float hx = std::min(kPlaneDrawHalfExtent, ex[0] / 2.f), hz = std::min(kPlaneDrawHalfExtent, ex[2] / 2.f);
            const Vec3 p0 = xform(t, -hx, a.y, -hz, 1.f), p1 = xform(t, hx, a.y, -hz, 1.f), p2 = xform(t, hx, a.y, hz, 1.f),
                       p3 = xform(t, -hx, a.y, hz, 1.f);
            addTriangle(0, p0, p2, p1);
            addTriangle(0, p0, p3, p2);
        } else {
            const Vec3 c[8] = {xform(t, a.x, a.y, a.z, 1), xform(t, b.x, a.y, a.z, 1), xform(t, b.x, b.y, a.z, 1), xform(t, a.x, b.y, a.z, 1),
                               xform(t, a.x, a.y, b.z, 1), xform(t, b.x, a.y, b.z, 1), xform(t, b.x, b.y, b.z, 1), xform(t, a.x, b.y, b.z, 1)};
            const int f[6][4] = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {3, 7, 6, 2}, {0, 4, 7, 3}, {1, 2, 6, 5}};
            for (const auto& q : f) {
                addTriangle(0, c[q[0]], c[q[1]], c[q[2]]);
                addTriangle(0, c[q[0]], c[q[2]], c[q[3]]);
            }
        }
        ++out.drawnEntities;
    });

    // Camera: frame the shapes (or the spawn point when there are none), looking slightly down from the south-east.
    Vec3 target(0.f, 0.f, 0.f);
    float radius = 6.f;
    if (haveShapes) {
        target = Vec3((lo.x + hi.x) / 2.f, (lo.y + hi.y) / 2.f, (lo.z + hi.z) / 2.f);
        radius = std::max(2.f, 0.5f * std::sqrt((hi.x - lo.x) * (hi.x - lo.x) + (hi.y - lo.y) * (hi.y - lo.y) + (hi.z - lo.z) * (hi.z - lo.z)));
    } else {
        float spawn[3] = {0.f, 0.f, 0.f};
        if (world.findSpawnPoint(spawn)) {
            target = Vec3(spawn[0], 0.f, spawn[2]);
        }
    }
    s.camera.target = target;
    s.camera.eye = Vec3(target.x + 1.1f * radius, target.y + 0.9f * radius, target.z + 1.9f * radius);
    s.camera.fovYDegrees = 45.f;
    s.camera.nearZ = 0.1f;
    s.camera.farZ = 200.f;

    // Lighting from the level: the Sun's +Z (towards the sun), the fog colour behind.
    world.registry().each<fuse::ecs::DirectionalLight, fuse::ecs::Transform>(
        [&](fuse::ecs::EntityID, fuse::ecs::DirectionalLight&, fuse::ecs::Transform& t) {
            out.resolve.lightDir = normalize(Vec3(t.local_to_world.data[8], t.local_to_world.data[9], t.local_to_world.data[10]));
        });
    world.registry().each<fuse::ecs::EnvironmentFog>([&](fuse::ecs::EntityID, fuse::ecs::EnvironmentFog& f) {
        out.resolve.background = Vec3(f.color.x, f.color.y, f.color.z);
    });
    fs::remove_all(dir, ec);
    return !s.batches.empty();
}

using FlipResultAlias = fuse::content_golden::FlipResult;

FlipResultAlias flip(const h::ImageRgba8& reference, const h::ImageRgba8& test) {
    return fuse::content_golden::computeFlip(reference.pixels.data(), test.pixels.data(), reference.width, reference.height);
}

void writeArtifacts(const std::string& dir, const std::string& name, const h::ImageRgba8& actual, const FlipResultAlias& f) {
    h::ensureDirectory(dir);
    h::writePng(dir + "/" + name + ".actual.png", actual);
    if (!f.map.empty()) {
        h::ImageRgba8 heat(actual.width, actual.height);
        heat.pixels = fuse::content_golden::flipHeatmap(f.map, actual.width, actual.height);
        h::writePng(dir + "/" + name + ".flip.png", heat);
    }
    std::printf("      artefacts: %s/%s.{actual,flip}.png\n", dir.c_str(), name.c_str());
}

bool renderLit(h::HeadlessFrameRunner& runner, const LevelScene& ls, h::FrameCapture& cap, std::string& error) {
    if (!runner.render(ls.scene, h::RasterShaders::Projected, cap, error)) {
        return false;
    }
    const Vec3 viewDir(ls.scene.camera.eye.x - ls.scene.camera.target.x, ls.scene.camera.eye.y - ls.scene.camera.target.y,
                       ls.scene.camera.eye.z - ls.scene.camera.target.z);
    h::resolveCapture(cap, viewDir, ls.resolve);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    std::string goldenDir = FUSE_REMASTER_GOLDEN_DIR;
    std::string artifactDir = FUSE_REMASTER_GOLDEN_ARTIFACT_DIR;
    fs::path work = fs::temp_directory_path() / "fuse_remaster_golden";
    h::HarnessOptions options;
    options.width = kWidth;
    options.height = kHeight;
    options.shaderDir = FUSE_RP_STOCK_SHADER_DIR;
    options.harnessShaderDir = FUSE_RP_HARNESS_SHADER_DIR;
    for (int i = 1; i + 1 < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--golden-dir") goldenDir = argv[++i];
        else if (a == "--artifact-dir") artifactDir = argv[++i];
        else if (a == "--shader-dir") options.shaderDir = argv[++i];
        else if (a == "--harness-shader-dir") options.harnessShaderDir = argv[++i];
        else if (a == "--work") work = argv[++i];
    }
    const bool update = h::goldenUpdateRequestedByEnv();
    const fs::path src(FUSE_SOURCE_DIR);

    fuse::core::initialize();
    // The level scenes are pure CPU (converter, cook, World3D): build them before asking for a device.
    struct Entry {
        std::string name;
        fs::path mission;
    };
    const std::vector<Entry> entries = {
        {"example_level", src / "Templates/BaseGame/game/data/ExampleModule/levels/ExampleLevel.mis"},
        {"shapes_level", src / "Tests/fixtures/remaster/golden/shapes_level.mis"},
    };
    std::vector<LevelScene> scenes(entries.size());
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const bool ok = buildLevelScene(entries[i].name, entries[i].mission, work, scenes[i]);
        expect(ok, entries[i].name + ": converted level -> harness scene (" + std::to_string(scenes[i].drawnEntities) +
                       " drawn entities, " + std::to_string(scenes[i].cookedShapes) + " cooked shapes, " +
                       std::to_string(scenes[i].scene.batches.size()) + " batches, " +
                       std::to_string(scenes[i].scene.triangleCount()) + " triangles) " + scenes[i].note);
        LevelScene again;
        buildLevelScene(entries[i].name, entries[i].mission, work / "again", again); // another dir: no path dependence
        expect(again.scene.contentHash() == scenes[i].scene.contentHash(), entries[i].name + ": scene build is deterministic");
    }
    expect(scenes[0].drawnEntities == 1u, "example_level: one drawn entity (the ground plane)");
    expect(scenes[1].cookedShapes == 5u && scenes[1].drawnEntities == 6u, "shapes_level: 5 TSStatics cooked and drawn + the ground plane");
    LevelScene nudged;
    buildLevelScene("shapes_level", entries[1].mission, work, nudged, "Cube", 0.5f);

    std::string reason;
    auto runner = h::HeadlessFrameRunner::create(options, reason);
    if (!runner) {
        std::printf("SKIP: %s\n", reason.c_str());
        fuse::core::shutdown();
        return g_failures ? 1 : kSkip;
    }
    std::printf("device: %s | %s\n", runner->deviceName().c_str(), update ? "UPDATE MODE" : "compare");
    std::map<std::string, h::ImageRgba8> frames;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const std::string& name = entries[i].name;
        h::FrameCapture first, second;
        std::string error;
        const bool ok1 = renderLit(*runner, scenes[i], first, error);
        const bool ok2 = ok1 && renderLit(*runner, scenes[i], second, error);
        expect(ok2, name + ": render + re-render (" + error + ")");
        if (!ok2) {
            continue;
        }
        frames[name] = first.lit;
        std::printf("%s: %u draws, %llu triangles, %u covered pixels\n", name.c_str(), first.draws,
                    static_cast<unsigned long long>(first.triangles), first.coveredPixels());
        expect(first.coveredPixels() > kWidth * kHeight / 5u, name + ": the level covers > 20 % of the frame");
        const FlipResultAlias again = flip(first.lit, second.lit);
        expect(first.lit.pixels == second.lit.pixels && again.valid && again.mean == 0.0 && again.nonZero == 0u,
               name + ": deterministic re-render is bit-identical, FLIP == 0 exactly (got " + fmt("%.9f", again.mean) + ")");
        const std::string goldenPath = goldenDir + "/" + name + ".png";
        if (update) {
            expect(h::ensureDirectory(goldenDir) && h::writePng(goldenPath, first.lit), name + ": golden rewritten -> " + goldenPath);
            continue;
        }
        h::ImageRgba8 golden;
        std::string perr;
        if (!h::readPng(goldenPath, golden, &perr) || golden.width != first.lit.width || golden.height != first.lit.height) {
            expect(false, name + ": golden " + goldenPath + " missing or wrong size (" + perr + "); regenerate with FUSE_UPDATE_GOLDENS=1");
            writeArtifacts(artifactDir, name, first.lit, FlipResultAlias{});
            continue;
        }
        const FlipResultAlias g = flip(golden, first.lit);
        const bool exact = golden.pixels == first.lit.pixels;
        expect(g.valid && g.mean <= kMaxMeanFlip,
               name + ": mean FLIP vs golden " + fmt("%.6f", g.mean) + " <= 0.05" + (exact ? " (bit-exact)" : " (NOT bit-exact)"));
        if (!(g.valid && g.mean <= kMaxMeanFlip)) {
            writeArtifacts(artifactDir, name, first.lit, g);
        }
    }
    // Regression sensitivity: a TSStatic moved by half a metre changes the frame.
    if (!update && frames.count("shapes_level")) {
        h::FrameCapture moved;
        std::string error;
        if (renderLit(*runner, nudged, moved, error)) {
            const FlipResultAlias f = flip(frames["shapes_level"], moved.lit);
            expect(f.valid && f.mean > 0.0 && f.nonZero > 500u,
                   "sanity: the Cube TSStatic moved 0.5 m -> FLIP " + fmt("%.6f", f.mean) + ", " + std::to_string(f.nonZero) +
                       " changed pixels (the gate sees transform regressions)");
        } else {
            expect(false, "sanity render: " + error);
        }
    }
    const h::ValidationReport v = runner->validation();
    std::printf("validation: %u error(s), %u warning(s)%s%s\n", v.errors, v.warnings, v.lastError.empty() ? "" : " last: ", v.lastError.c_str());
    expect(v.errors == 0u && v.warnings == 0u, "zero validation / sync-validation messages");
    runner.reset();
    fuse::core::shutdown();
    std::printf("%s (%d failure(s))\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
