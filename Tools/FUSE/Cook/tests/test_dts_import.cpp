// fuse_asset_dts_import (asset plan 4a, L-DTS): Torque shape import over every template shape.
//
//   dts     every `.dts` under Templates/ parses (versions 24, 26 and 28; every section guard checked), the
//           highest detail converts to a CookedMesh with finite, non-empty bounds inside the shape's own bounds
//           (converted to FUSE space), the expected triangle counts (pinned per shape), and counter-clockwise
//           winding (face normals agree with the stored vertex normals on >= 95 % of triangles; closed unit
//           shapes enclose a positive volume). The zero-byte torusknowpreview.dts is refused as empty.
//   dae     every `.dae` under Templates/ imports through the vendored assimp (skipped without assimp).
//   cook    cook_mesh_file -> .fusemesh -> load equals the import; re-cook is byte-identical; strict AssetCooker
//           cooks a DTS (the §5.3 cook gates accept it).
//   reject  truncations at every 7th byte, a flipped guard, a bad version and a mesh index out of range are
//           refused with MalformedSource / InvalidGeometry and never crash.
//   repair  the five template DAEs with zero-area triangles (Playerbot, car, flier, hoverboat, DemoRoomLrg) are
//           refused by the strict cook by default and cook with MeshImportDesc::repair_degenerate.
//   seq     sequences + `.dsq`: no template shape carries sequences (counted, all 0; no .dsq), so the decoding is
//           gated on shapes from the clean-room writer (Remaster W1.1 arm fixture: embedded cyclic "wave" with
//           rotations, translations, ground frames and triggers; "walk" / "idle" in a .dsq with reversed node order
//           and aligned / uniform scales) and on a .dsq animating every node of the Verve soldier's real skeleton:
//           sane durations / frame counts / joint counts, keys land on their frames, root motion and events,
//           .fuseanim cooks byte-identically, skeleton hash = names + parents = fuse_assetcheck's rig hash; truncated
//           .dsq files and out-of-pool indices are refused.

#include <fuse/animation/cooked_anim.hpp>
#include <fuse/animation/math.hpp>
#include <fuse/assetcheck/asset_rules.hpp>
#include <fuse/cook/anim_cook.hpp>
#include <fuse/cook/dts_import.hpp>
#include <fuse/cook/dts_write.hpp>
#include <fuse/cook/mesh_cook.hpp>
#include <fuse/project/asset_cooker.hpp>
#include <fuse/remaster/legacy_ts/torque_fixtures.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#ifndef FUSE_SOURCE_DIR
#define FUSE_SOURCE_DIR "."
#endif

namespace {

namespace fs = std::filesystem;
using namespace fuse;
using namespace fuse::cook;

int g_failures = 0;
u32 g_repairShapes = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++g_failures;
    }
}

std::vector<u8> read_bytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<u8>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::vector<fs::path> find_files(const fs::path& root, const std::string& extLower) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, ec), end; it != end; it.increment(ec)) {
        if (ec || !it->is_regular_file()) {
            continue;
        }
        std::string ext = it->path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext == extLower) {
            out.push_back(it->path());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

struct Orientation {
    u32 agree = 0;
    u32 disagree = 0;
    f64 volume = 0.0;
};

Orientation orientation(const CookedMesh& mesh) {
    Orientation o;
    for (usize t = 0; t + 2u < mesh.indices.size(); t += 3u) {
        const u32 i[3] = {mesh.indices[t], mesh.indices[t + 1u], mesh.indices[t + 2u]};
        const f32* A = &mesh.positions[i[0] * 3u];
        const f32* B = &mesh.positions[i[1] * 3u];
        const f32* C = &mesh.positions[i[2] * 3u];
        const f64 e1[3] = {B[0] - A[0], B[1] - A[1], B[2] - A[2]};
        const f64 e2[3] = {C[0] - A[0], C[1] - A[1], C[2] - A[2]};
        const f64 n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        f64 vn[3] = {0.0, 0.0, 0.0};
        for (u32 c = 0; c < 3u; ++c) {
            for (u32 k = 0; k < 3u; ++k) {
                vn[k] += mesh.normals[i[c] * 3u + k];
            }
        }
        const f64 d = n[0] * vn[0] + n[1] * vn[1] + n[2] * vn[2];
        if (d > 0.0) {
            ++o.agree;
        } else if (d < 0.0) {
            ++o.disagree;
        }
        // Signed tetrahedron volume against the origin.
        o.volume += (A[0] * (B[1] * C[2] - B[2] * C[1]) - A[1] * (B[0] * C[2] - B[2] * C[0]) +
                     A[2] * (B[0] * C[1] - B[1] * C[0])) /
                    6.0;
    }
    return o;
}

bool finite_bounds(const CookedMesh& mesh) {
    f32 extent = 0.f;
    for (u32 a = 0; a < 3u; ++a) {
        if (!std::isfinite(mesh.bounds_min[a]) || !std::isfinite(mesh.bounds_max[a]) || mesh.bounds_min[a] > mesh.bounds_max[a]) {
            return false;
        }
        extent = std::max(extent, mesh.bounds_max[a] - mesh.bounds_min[a]);
    }
    return extent > 1e-4f && extent < 1e4f;
}

/// Pinned triangle counts of the highest detail (file stem -> triangles), measured once from the shapes.
const std::map<std::string, u32>& expected_triangles() {
    static const std::map<std::string, u32> table = {
        {"unit_cube", 12u}, {"unit_sphere", 64u}, {"unit_capsule", 64u}, {"octahedron", 8u},
        {"cubepreview", 12u}, {"cubemaped_cubepreview", 12u}, {"pyramidpreview", 8u}, {"noshape", 160u},
        {"cylinderpreview", 80u}, {"spherepreview", 960u}, {"toruspreview", 576u}, {"torusknotpreview", 1920u},
    };
    return table;
}

void test_dts_fixtures(const fs::path& templates) {
    const std::vector<fs::path> files = find_files(templates, ".dts");
    expect(files.size() >= 21u, "at least 21 template .dts shapes (found " + std::to_string(files.size()) + ")");
    u32 parsed = 0;
    u32 versions[32] = {};
    for (const fs::path& file : files) {
        const std::string name = file.lexically_relative(templates).generic_string();
        const std::string stem = file.stem().string();
        const std::vector<u8> bytes = read_bytes(file);
        DtsShape shape;
        std::string error;
        const bool ok = parse_dts(bytes.data(), bytes.size(), shape, &error);
        if (bytes.empty()) {
            // tools/materialEditor/shapes/torusknowpreview.dts ships as a zero-byte file (a typo'd duplicate).
            expect(!ok && error.find("empty") != std::string::npos, name + ": zero-byte file refused as empty");
            continue;
        }
        expect(ok, name + ": parses (" + error + ")");
        if (!ok) {
            continue;
        }
        ++parsed;
        ++versions[shape.version & 31u];
        MeshCookOptions options;
        options.import_material_names = true;
        CookedMesh mesh;
        DtsImportStats stats;
        const bool converted = dts_to_cooked_mesh(shape, options, mesh, &error, &stats);
        expect(converted, name + ": converts (" + error + ")");
        if (!converted) {
            continue;
        }
        const u32 triangles = static_cast<u32>(mesh.indices.size() / 3u);
        std::printf("  %-70s v%u  detail '%s'  %u meshes  %u verts  %u tris  (%u stitch dropped)  bounds [%.3f %.3f %.3f]..[%.3f %.3f %.3f]\n",
                    name.c_str(), shape.version, shape.details[static_cast<usize>(stats.detail)].name.c_str(), stats.meshes,
                    mesh.vertex_count(), triangles, stats.dropped_stitch, mesh.bounds_min[0], mesh.bounds_min[1],
                    mesh.bounds_min[2], mesh.bounds_max[0], mesh.bounds_max[1], mesh.bounds_max[2]);
        expect(triangles > 0u && stats.triangles == triangles, name + ": triangles");
        expect(finite_bounds(mesh), name + ": finite, non-empty bounds");
        expect(mesh.submeshes.size() == mesh.material_slots.size(), name + ": one material slot per submesh");
        // Inside the shape bounds (Torque space -> FUSE (x, z, -y)), with a small tolerance for exporter rounding.
        // Skins are imported in their bind pose; the shape bounds describe the default pose, so they are not compared.
        bool skinned = false;
        for (const DtsMesh& m : shape.meshes) {
            skinned = skinned || m.type == DtsMeshType::Skin;
        }
        const f32 smin[3] = {shape.bounds_min[0], shape.bounds_min[2], -shape.bounds_max[1]};
        const f32 smax[3] = {shape.bounds_max[0], shape.bounds_max[2], -shape.bounds_min[1]};
        bool inside = true;
        for (u32 a = 0; a < 3u; ++a) {
            const f32 tol = 1e-3f + 0.01f * (smax[a] - smin[a]);
            inside = inside && mesh.bounds_min[a] >= smin[a] - tol && mesh.bounds_max[a] <= smax[a] + tol;
        }
        expect(inside || skinned, name + ": cooked bounds inside the shape bounds");
        const auto pinned = expected_triangles().find(stem);
        if (pinned != expected_triangles().end()) {
            expect(triangles == pinned->second, name + ": " + std::to_string(triangles) + " triangles, expected " +
                                                    std::to_string(pinned->second));
        }
        const Orientation o = orientation(mesh);
        expect(o.agree * 100u >= (o.agree + o.disagree) * 95u,
               name + ": counter-clockwise winding (" + std::to_string(o.agree) + " agree / " + std::to_string(o.disagree) +
                   " disagree with the vertex normals)");
        if (stem == "unit_cube" || stem == "unit_sphere" || stem == "unit_capsule") {
            expect(o.volume > 0.0, name + ": closed shape encloses a positive volume");
        }
        if (stem == "unit_cube") {
            expect(std::fabs(o.volume - 1.0) < 1e-3, name + ": unit cube volume 1");
            for (u32 a = 0; a < 3u; ++a) {
                expect(std::fabs(mesh.bounds_min[a] + 0.5f) < 1e-4f && std::fabs(mesh.bounds_max[a] - 0.5f) < 1e-4f,
                       name + ": unit cube bounds +-0.5");
            }
        }
        if (stem == "simplecone") {
            // Torque Z-up -> FUSE Y-up: Torque z in [-0.375, 0.75] becomes y; the tip at Torque y = 1.25 becomes z = -1.25.
            expect(std::fabs(mesh.bounds_max[1] - shape.bounds_max[2]) < 1e-3f &&
                       std::fabs(mesh.bounds_min[2] + shape.bounds_max[1]) < 1e-3f,
                   name + ": Z-up -> Y-up (p' = (x, z, -y))");
        }
        if (stem == "soldier_rigged") {
            MeshCookOptions skin = options;
            skin.import_skin = true;
            CookedMesh skinMesh;
            expect(dts_to_cooked_mesh(shape, skin, skinMesh, &error) && skinMesh.weights.size() == skinMesh.vertex_count() * 4u,
                   name + ": skin weights import (" + error + ")");
            bool normalised = true;
            for (usize v = 0; v + 3u < skinMesh.weights.size(); v += 4u) {
                const f32 sum = skinMesh.weights[v] + skinMesh.weights[v + 1u] + skinMesh.weights[v + 2u] + skinMesh.weights[v + 3u];
                normalised = normalised && std::fabs(sum - 1.f) < 1e-4f;
            }
            expect(normalised, name + ": skin weights sum to 1");
        }
    }
    expect(versions[24] > 0u && versions[26] > 0u && versions[28] > 0u, "fixtures cover .dts v24, v26 and v28");
    std::printf("dts: %u of %zu shapes parsed\n", parsed, files.size());
}

void test_dae_fixtures(const fs::path& templates) {
    const std::vector<fs::path> files = find_files(templates, ".dae");
    expect(files.size() >= 11u, "at least 11 template .dae shapes (found " + std::to_string(files.size()) + ")");
    u32 imported = 0;
    for (const fs::path& file : files) {
        const std::string name = file.lexically_relative(templates).generic_string();
        CookedMesh mesh;
        std::string error;
        CookFailure failure = CookFailure::None;
        MeshCookOptions options;
        const bool ok = import_mesh_file(file.string(), options, mesh, &error, &failure);
        if (!ok && failure == CookFailure::ImporterUnavailable) {
            std::printf("dae: assimp not linked; COLLADA import skipped\n");
            return;
        }
        expect(ok, name + ": imports (" + error + ")");
        if (!ok) {
            continue;
        }
        ++imported;
        const u32 triangles = static_cast<u32>(mesh.indices.size() / 3u);
        std::printf("  %-70s %u verts  %u tris  bounds [%.3f %.3f %.3f]..[%.3f %.3f %.3f]\n", name.c_str(),
                    mesh.vertex_count(), triangles, mesh.bounds_min[0], mesh.bounds_min[1], mesh.bounds_min[2],
                    mesh.bounds_max[0], mesh.bounds_max[1], mesh.bounds_max[2]);
        expect(triangles > 0u && finite_bounds(mesh), name + ": triangles + finite bounds");
        // The strict cook's §5.3 gates: five template DAEs carry a few zero-area triangles from their exporter and
        // are refused by default; with the opt-in repair_degenerate they cook (and every other one cooks either way).
        fuse::project::MeshImportDesc desc;
        desc.input_path = file.string();
        desc.output_path = (fs::temp_directory_path() / "fuse_asset_dts_import_dae.fusemesh").string();
        fuse::project::AssetCooker cooker;
        const fuse::project::CookRecord record = cooker.cook_mesh(desc);
        desc.repair_degenerate = true;
        fuse::project::AssetCooker repairCooker;
        const fuse::project::CookRecord repaired = repairCooker.cook_mesh(desc);
        u32 dropped = 0;
        const std::size_t at = repaired.note.find("repair_degenerate=");
        if (at != std::string::npos) {
            dropped = static_cast<u32>(std::strtoul(repaired.note.c_str() + at + 18u, nullptr, 10));
        }
        std::printf("      strict cook: %s%s%s | repair_degenerate: %s (%u dropped)\n", record.ok ? "ok" : "refused",
                    record.ok ? "" : " - ", record.ok ? "" : record.note.c_str(), repaired.ok ? "ok" : "refused", dropped);
        const std::string stem = file.stem().string();
        const bool degenerateSource = stem == "Playerbot" || stem == "car" || stem == "flier" || stem == "hoverboat" ||
                                      stem == "DemoRoomLrg";
        if (degenerateSource) {
            ++g_repairShapes;
            expect(!record.ok && record.status == fuse::project::CookStatus::AssetGateFailed,
                   name + ": refused by the strict cook without repair_degenerate (zero-area triangles)");
            expect(repaired.ok && dropped >= 1u && dropped <= 8u,
                   name + ": cooks with repair_degenerate (" + std::to_string(dropped) + " triangles dropped; " +
                       repaired.note + ")");
        } else {
            expect(record.ok && repaired.ok, name + ": strict cook ok with and without repair_degenerate");
        }
    }
    std::printf("dae: %u of %zu shapes imported\n", imported, files.size());
    expect(imported == 0u || g_repairShapes == 5u,
           "the five degenerate-triangle DAEs were found (" + std::to_string(g_repairShapes) + ")");
}

void test_cook(const fs::path& templates, const fs::path& scratch) {
    const fs::path source = templates / "BaseGame/game/core/gameObjects/shapes/unit_cube.dts";
    const fs::path out1 = scratch / "unit_cube_a.fusemesh";
    const fs::path out2 = scratch / "unit_cube_b.fusemesh";
    const CookStubWriteResult a = cook_mesh_file(source.string(), out1.string());
    const CookStubWriteResult b = cook_mesh_file(source.string(), out2.string());
    expect(a.ok && b.ok, "cook_mesh_file cooks unit_cube.dts");
    expect(read_bytes(out1) == read_bytes(out2) && !read_bytes(out1).empty(), "re-cook is byte-identical");
    CookedMesh imported;
    CookedMesh loaded;
    std::string error;
    expect(import_mesh_file(source.string(), MeshCookOptions{}, imported, &error), "import_mesh_file dispatches .dts");
    expect(load_cooked_mesh(out1.string(), loaded, &error) && loaded == imported, "cooked .fusemesh loads equal to the import");

    fuse::project::MeshImportDesc desc;
    desc.input_path = (templates / "BaseGame/game/core/gameObjects/shapes/unit_sphere.dts").string();
    desc.output_path = (scratch / "unit_sphere.fusemesh").string();
    fuse::project::AssetCooker cooker;
    const fuse::project::CookRecord record = cooker.cook_mesh(desc);
    expect(record.ok && record.status == fuse::project::CookStatus::Ok,
           "strict AssetCooker cooks unit_sphere.dts (" + record.note + ")");
}

void test_rejects(const fs::path& templates) {
    const std::vector<u8> good = read_bytes(templates / "BaseGame/game/core/gameObjects/shapes/unit_cube.dts");
    DtsShape shape;
    std::string error;
    expect(parse_dts(good.data(), good.size(), shape, &error), "unit_cube parses");
    // Truncation anywhere inside the memory buffer must be refused (never read past the end).
    const usize bufferEnd = 16u + static_cast<usize>(reinterpret_cast<const s32*>(good.data())[1]) * 4u;
    u32 refused = 0;
    u32 tried = 0;
    for (usize cut = 0; cut < bufferEnd; cut += 7u) {
        DtsShape s;
        ++tried;
        if (!parse_dts(good.data(), cut, s, &error)) {
            ++refused;
        }
    }
    expect(refused == tried, "every truncation inside the memory buffer refused (" + std::to_string(refused) + "/" +
                                 std::to_string(tried) + ")");
    std::vector<u8> bad = good;
    // Version 23.
    bad[0] = 23;
    expect(!parse_dts(bad.data(), bad.size(), shape, &error) && error.find("version") != std::string::npos,
           "unsupported version refused");
    // Corrupt the first guard word (word 19 of the 32-bit stream).
    bad = good;
    bad[16u + 19u * 4u] ^= 0x5Au;
    expect(!parse_dts(bad.data(), bad.size(), shape, &error) && error.find("guard") != std::string::npos,
           "corrupted guard refused (" + error + ")");
    // Index out of range in the converted mesh.
    DtsShape broken = shape;
    expect(parse_dts(good.data(), good.size(), broken, &error), "reparse");
    for (DtsMesh& mesh : broken.meshes) {
        if (!mesh.indices.empty()) {
            mesh.indices[0] = 100000u;
        }
    }
    CookedMesh mesh;
    expect(!dts_to_cooked_mesh(broken, MeshCookOptions{}, mesh, &error) && error.find("out of range") != std::string::npos,
           "out-of-range index refused");
    CookFailure failure = CookFailure::None;
    const fs::path empty = templates / "BaseGame/game/tools/materialEditor/shapes/torusknowpreview.dts";
    if (fs::exists(empty)) {
        expect(!import_mesh_file(empty.string(), MeshCookOptions{}, mesh, &error, &failure) &&
                   failure == CookFailure::MalformedSource,
               "zero-byte .dts: import_mesh_file -> MalformedSource");
    }
}

namespace an = fuse::animation;

f32 quatAngle(const an::quat& a, const an::quat& b) {
    const f32 d = std::fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
    return 2.f * std::acos(std::min(d, 1.f));
}

/// Sequences + DSQ (asset plan 4a): every template shape's sequence table; the writer's arm fixture and dsq (W1.1)
/// decode to the authored keys; a dsq on the Verve soldier's real skeleton cooks to .fuseanim byte-identically
/// with the soldier's joint count and skeleton hash.
void test_sequences(const fs::path& templates, const fs::path& scratch) {
    // 1. Template shapes: none carries a sequence (all 20 non-empty .dts have a count of 0; there is no .dsq), and
    //    the material lists now parse through the (empty) sequence table.
    u32 withSequences = 0;
    for (const fs::path& file : find_files(templates, ".dts")) {
        const std::vector<u8> bytes = read_bytes(file);
        DtsShape shape;
        if (bytes.empty() || !parse_dts(bytes.data(), bytes.size(), shape)) {
            continue;
        }
        withSequences += shape.sequences.empty() ? 0u : 1u;
        if (file.stem() == "soldier_rigged") {
            expect(shape.materials.size() == 3u && shape.materials[1] == "base_Soldier_Main",
                   "soldier_rigged: material list after the sequence table");
        }
    }
    expect(find_files(templates, ".dsq").empty(), "no .dsq under Templates/");
    std::printf("sequences: %u template .dts with sequences, %zu .dsq\n", withSequences, find_files(templates, ".dsq").size());
    expect(withSequences == 0u, "no template .dts carries sequences (the gate uses written shapes)");

    // 2. Written arm (embedded "wave") -> parse -> CookedAnim.
    const DtsShape arm = fuse::remaster::legacy_ts::fixtureArmShape();
    std::string error;
    const std::vector<u8> armBytes = write_dts(arm, DtsWriteOptions{26u, 0u}, &error);
    DtsShape parsed;
    expect(!armBytes.empty() && parse_dts(armBytes.data(), armBytes.size(), parsed, &error), "arm.dts parses (" + error + ")");
    expect(parsed.sequences.size() == 1u && parsed.sequences[0] == arm.sequences[0] && parsed.triggers == arm.triggers &&
               parsed.keyframes == arm.keyframes && parsed.materials == arm.materials,
           "arm.dts: sequence record, triggers, keyframe pools and materials round-trip exactly");
    an::CookedAnim wave;
    expect(dts_sequence_to_cooked_anim(parsed, 0u, DtsAnimOptions{}, wave, &error), "wave converts (" + error + ")");
    expect(wave.joints.size() == 4u && wave.duration == 1.f && wave.frame_count == 31u && wave.looping,
           "wave: 4 joints, 1 s, 31 frames, cyclic");
    // Keys at k / 8 s (cyclic spacing); rotations about Torque +X = FUSE +X.
    f32 worst = 0.f;
    for (u32 k = 0; k < 8u; ++k) {
        const f32 t = static_cast<f32>(k) / 8.f;
        const u32 frame = static_cast<u32>(std::lround(t * 30.f));
        if (std::fabs(static_cast<f32>(frame) / 30.f - t) > 1e-6f) {
            continue; // only frames that land on a key (k = 0, 4)
        }
        const DtsQuat16& q = arm.keyframes.node_rotations[k];
        const an::quat key{q[0] / 32767.f, q[2] / 32767.f, -q[1] / 32767.f, q[3] / 32767.f};
        worst = std::max(worst, quatAngle(an::quat_normalize(key), wave.joints[1].rotation[frame]));
    }
    expect(worst < 1e-4f, "wave: upper rotation at key frames = the key (FUSE space)");
    expect(wave.has_root_motion && std::fabs(wave.root_motion.back().dz + 1.f) < 1e-5f &&
               std::fabs(wave.root_motion.back().dx) < 1e-5f &&
               std::fabs(wave.root_motion.back().dyaw - 1.5707963f) < 1e-3f,
           "wave: root motion = 4 ground frames (Torque +Y 1 m -> FUSE -Z, 90 deg yaw)");
    expect(wave.events.size() == 2u && wave.events[0].name == "trigger1" && wave.events[0].time == 0.25f &&
               wave.events[1].name == "trigger1_off" && wave.events[1].time == 0.75f,
           "wave: triggers -> events");
    std::vector<std::string> names;
    std::vector<s32> parents;
    dts_skeleton(parsed, names, parents);
    const fs::path armPath = scratch / "arm.dts";
    expect(write_binary_file(armPath.string(), armBytes, &error), "write arm.dts");
    expect(count_anim_clips(armPath.string()) == 1u, "count_anim_clips(.dts)");
    const fs::path animA = scratch / "wave_a.fuseanim";
    const fs::path animB = scratch / "wave_b.fuseanim";
    AnimCookOptions animOptions;
    animOptions.clip_name = "wave";
    const CookStubWriteResult ca = cook_anim_file(armPath.string(), animA.string(), animOptions);
    const CookStubWriteResult cb = cook_anim_file(armPath.string(), animB.string(), animOptions);
    expect(ca.ok && cb.ok && read_bytes(animA) == read_bytes(animB), "wave cooks to .fuseanim byte-identically (" + ca.note + ")");
    an::CookedAnim loaded;
    expect(an::read_cooked_anim_file(animA.string(), loaded, &error) &&
               loaded.skeleton_hash == an::skeleton_hash_of_joints(names, parents) &&
               loaded.skeleton_hash == fuse::assetcheck::skeletonHash(names, parents),
           "wave .fuseanim: skeleton hash = names + parents = the assetcheck rig hash");

    // 3. DSQ (nodes in reversed order) bound to the arm by name.
    const DsqFile dsq = fuse::remaster::legacy_ts::fixtureArmDsq();
    const std::vector<u8> dsqBytes = write_dsq(dsq, &error);
    DsqFile dsqParsed;
    expect(parse_dsq(dsqBytes.data(), dsqBytes.size(), dsqParsed, &error) && dsqParsed.nodes == dsq.nodes &&
               dsqParsed.sequences == dsq.sequences && dsqParsed.keyframes == dsq.keyframes && dsqParsed.triggers == dsq.triggers,
           "arm_anims.dsq round-trips (" + error + ")");
    an::CookedAnim walk, idle;
    u32 unmatched = 99;
    expect(dsq_sequence_to_cooked_anim(dsqParsed, 0u, &parsed, DtsAnimOptions{}, walk, &error, &unmatched) && unmatched == 0u,
           "walk binds to the arm skeleton by name");
    expect(dsq_sequence_to_cooked_anim(dsqParsed, 1u, &parsed, DtsAnimOptions{}, idle, &error), "idle converts");
    expect(walk.duration == 0.8f && !walk.looping && walk.frame_count == 25u && idle.duration == 2.f && idle.looping &&
               walk.joints.size() == 4u && idle.joints.size() == 4u,
           "walk 0.8 s one-shot, idle 2 s cyclic, 4 joints each");
    // walk: tip (joint 3) aligned scale ends at the last key (1.4, 1, 0.8) Torque -> FUSE (1.4, 0.8, 1).
    expect(walk.joints[3].scale.size() == walk.frame_count && std::fabs(walk.joints[3].scale.back().x - 1.4f) < 1e-5f &&
               std::fabs(walk.joints[3].scale.back().y - 0.8f) < 1e-5f && std::fabs(walk.joints[3].scale.back().z - 1.f) < 1e-5f,
           "walk: tip aligned scale (y / z swapped into FUSE space)");
    expect(idle.joints[0].scale.size() == idle.frame_count && walk.joints[2].rotation.size() == walk.frame_count &&
               walk.joints[1].rotation.empty() && walk.joints[0].translation.empty(),
           "dsq tracks land on the named joints only");
    const fs::path dsqPath = scratch / "arm_anims.dsq";
    expect(write_binary_file(dsqPath.string(), dsqBytes, &error), "write arm_anims.dsq");
    AnimCookOptions dsqOptions;
    dsqOptions.clip_name = "idle";
    dsqOptions.skeleton_path = armPath.string();
    const fs::path idleA = scratch / "idle_a.fuseanim";
    const fs::path idleB = scratch / "idle_b.fuseanim";
    expect(cook_anim_file(dsqPath.string(), idleA.string(), dsqOptions).ok &&
               cook_anim_file(dsqPath.string(), idleB.string(), dsqOptions).ok && read_bytes(idleA) == read_bytes(idleB),
           "idle (.dsq + skeleton) cooks byte-identically");

    // 4. A dsq on the Verve soldier's own skeleton (the only template shape with a rig): every node rotates.
    const fs::path soldierPath = templates / "Modules/Verve/art/shapes/actors/Soldier/soldier_rigged.dts";
    DtsShape soldier;
    if (!read_dts_file(soldierPath.string(), soldier, &error)) {
        expect(false, "soldier_rigged.dts reads (" + error + ")");
        return;
    }
    DsqFile soldierDsq;
    soldierDsq.version = 26;
    soldierDsq.shape_object_count = static_cast<s32>(soldier.objects.size());
    DtsSequence sway;
    sway.name = "sway";
    sway.flags = kDtsSeqCyclic;
    sway.keyframes = 10;
    sway.duration = 1.5f;
    for (u32 n = 0; n < soldier.nodes.size(); ++n) {
        soldierDsq.nodes.push_back(soldier.nodes[n].name);
        sway.rotation_matters.set(n);
        for (s32 k = 0; k < sway.keyframes; ++k) {
            const f64 a = 0.1 * std::sin(6.283185307179586 * k / sway.keyframes);
            soldierDsq.keyframes.node_rotations.push_back(
                dts_quat16(0.f, static_cast<f32>(std::sin(a * 0.5)), 0.f, static_cast<f32>(std::cos(a * 0.5))));
        }
    }
    soldierDsq.sequences = {sway};
    const fs::path soldierDsqPath = scratch / "soldier_sway.dsq";
    expect(write_binary_file(soldierDsqPath.string(), write_dsq(soldierDsq, &error), &error), "write soldier_sway.dsq");
    AnimCookOptions soldierOptions;
    soldierOptions.skeleton_path = soldierPath.string();
    const fs::path swayA = scratch / "sway_a.fuseanim";
    const fs::path swayB = scratch / "sway_b.fuseanim";
    const CookStubWriteResult sa = cook_anim_file(soldierDsqPath.string(), swayA.string(), soldierOptions);
    const CookStubWriteResult sb = cook_anim_file(soldierDsqPath.string(), swayB.string(), soldierOptions);
    expect(sa.ok && sb.ok && read_bytes(swayA) == read_bytes(swayB), "soldier sway cooks byte-identically (" + sa.note + ")");
    an::CookedAnim swayAnim;
    dts_skeleton(soldier, names, parents);
    expect(an::read_cooked_anim_file(swayA.string(), swayAnim, &error) && swayAnim.joints.size() == soldier.nodes.size() &&
               swayAnim.duration == 1.5f && swayAnim.frame_count == 46u &&
               swayAnim.skeleton_hash == fuse::assetcheck::skeletonHash(names, parents),
           "soldier sway: " + std::to_string(swayAnim.joints.size()) + " joints (= nodes), 1.5 s, rig hash");
    std::printf("sequences: soldier skeleton %zu joints, sway %u frames, %u bytes\n", swayAnim.joints.size(),
                swayAnim.frame_count, sa.byteCount);

    // 5. Refusals: truncated dsq, keys past the pool.
    u32 refused = 0, tried = 0;
    for (usize cut = 0; cut < dsqBytes.size(); cut += 5u) {
        DsqFile d;
        ++tried;
        refused += parse_dsq(dsqBytes.data(), cut, d) ? 0u : 1u;
    }
    expect(refused == tried, "every truncated .dsq refused (" + std::to_string(refused) + "/" + std::to_string(tried) + ")");
    DsqFile overrun = dsq;
    overrun.sequences[1].base_rotation = 7;
    const std::vector<u8> overrunBytes = write_dsq(overrun, &error);
    DsqFile d;
    expect(!parse_dsq(overrunBytes.data(), overrunBytes.size(), d, &error) && error.find("outside") != std::string::npos,
           "sequence keys past the rotation pool refused (" + error + ")");
    DtsShape badShape = arm;
    badShape.sequences[0].trigger_count = 3;
    const std::vector<u8> badBytes = write_dts(badShape, DtsWriteOptions{}, &error);
    DtsShape bad;
    expect(!parse_dts(badBytes.data(), badBytes.size(), bad, &error), "triggers past the trigger table refused");
}

} // namespace

int main() {
    const fs::path templates = fs::path(FUSE_SOURCE_DIR) / "Templates";
    if (!fs::is_directory(templates)) {
        std::fprintf(stderr, "FAIL: Templates/ not found under %s\n", FUSE_SOURCE_DIR);
        return 1;
    }
    std::error_code ec;
    const fs::path scratch = fs::temp_directory_path(ec) / "fuse_asset_dts_import";
    fs::remove_all(scratch, ec);
    fs::create_directories(scratch, ec);
    test_dts_fixtures(templates);
    test_dae_fixtures(templates);
    test_cook(templates, scratch);
    test_rejects(templates);
    test_sequences(templates, scratch);
    fs::remove_all(scratch, ec);
    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_asset_dts_import: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("fuse_asset_dts_import: ok\n");
    return 0;
}
