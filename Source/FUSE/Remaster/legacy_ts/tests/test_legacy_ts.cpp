// Remaster W1.1-W1.4 gates (docs/plans/FUSE_REMASTER_PLAN.md §7 Wave 1), ctest -L remaster.
//
//   fixtures <fixture-dir> <work>   W1.1: the generated Torque fixtures are identical on every build, match the
//                                   pinned Tests/fixtures/remaster/torque/generated.sha256, write / read back
//                                   byte-identically, parse with the clean-room readers; the committed .mis / .cs
//                                   are there and the script parses (W1.6 parser) without diagnostics
//   import <fixture-dir> <work>     W1.2: generated mesh -> DTS (writer) -> POCO: vertex positions within 1e-5 of the
//                                   generated ones (FUSE space), triangle counts per detail, collision mesh, 4 joints,
//                                   sequence durations (1, 0.8, 2 s), clip hashes = skeleton hash, DB oaid ->
//                                   replacement -> record; a second import is byte-identical (store + lock)
//   cook <fixture-dir> <work> <assetcheck>
//                                   W1.3: POCO -> FMSH v2 / .fusetex / .fusemat / .fskl + descriptor; the cooked
//                                   crate and arm pass fuse_assetcheck's asset_* gates; re-cook is byte-identical; a
//                                   POCO mesh with a zero-area triangle is refused (exit 77 without the tool)
//   world <fixture-dir> <work>      W1.4: fixture.mis -> .fuselevel (zero unexpected stubs) and -> POCO Level: every
//                                   TSStatic entity references the oaid + POCO record of its shape, the oaid
//                                   resolves in the DB to that record, and the `.fuselevel`'s MeshAssets ids name
//                                   the files cooked from the POCO records

#include <fuse/animation/cooked_anim.hpp>
#include <fuse/assetcheck/asset_rules.hpp>
#include <fuse/asset/cooked_material.hpp>
#include <fuse/cook/dts_write.hpp>
#include <fuse/cook/texture_cook.hpp>
#include <fuse/ecs/components/mesh.hpp>
#include <fuse/ecs/registry.hpp>
#include <fuse/project/world_converter.hpp>
#include <fuse/remaster/digest.hpp>
#include <fuse/remaster/legacy_ts/dts_poco.hpp>
#include <fuse/remaster/legacy_ts/mission_poco.hpp>
#include <fuse/remaster/legacy_ts/poco_cook.hpp>
#include <fuse/remaster/legacy_ts/torque_fixtures.hpp>
#include <fuse/scene/serialiser.hpp>
#if defined(FUSE_LEGACY_TS_HAS_TSCRIPT)
#include <fuse/remaster/tscript/parser.hpp>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace lt = fuse::remaster::legacy_ts;
namespace poco = fuse::remaster::poco;
namespace store = fuse::remaster::store;
namespace rdb = fuse::remaster::db;
using fuse::f32;
using fuse::s32;
using fuse::u32;
using fuse::u64;
using fuse::u8;
using fuse::usize;

int g_failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        ++g_failures;
    }
}

int finish(const char* gate) {
    if (g_failures != 0) {
        std::fprintf(stderr, "%s: %d failure(s)\n", gate, g_failures);
        return 1;
    }
    std::printf("%s: ok\n", gate);
    return 0;
}

std::string normaliseNewlines(std::string s) {
    std::string out;
    for (char c : s) {
        if (c != '\r') {
            out.push_back(c);
        }
    }
    return out;
}

void freshDir(const fs::path& p) {
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p, ec);
}

/// Fixtures (generated + committed text) laid out in `dir`.
bool stageFixtures(const fs::path& fixtureDir, const fs::path& dir) {
    std::string error;
    if (!lt::writeTorqueFixtures(dir, &error)) {
        std::fprintf(stderr, "FAIL: write fixtures: %s\n", error.c_str());
        return false;
    }
    std::error_code ec;
    for (const std::string& name : lt::committedTorqueFixtureNames()) {
        fs::copy_file(fixtureDir / name, dir / name, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            std::fprintf(stderr, "FAIL: copy %s: %s\n", name.c_str(), ec.message().c_str());
            return false;
        }
    }
    return true;
}

/// Torque -> FUSE (x, z, -y), then the max distance to `fuse` positions (same vertex order).
f32 maxPositionError(const std::vector<f32>& torque, const std::vector<f32>& fuse) {
    if (torque.size() != fuse.size()) {
        return 1e30f;
    }
    f32 worst = 0.f;
    for (usize v = 0; v + 2u < torque.size(); v += 3u) {
        const f32 expect[3] = {torque[v], torque[v + 2u], -torque[v + 1u]};
        for (u32 a = 0; a < 3u; ++a) {
            worst = std::max(worst, std::fabs(expect[a] - fuse[v + a]));
        }
    }
    return worst;
}

lt::ImportOptions fixtureOptions() {
    lt::ImportOptions o;
    o.gameId = "fixture";
    o.origin = poco::Origin::Authored;
    o.licenceId = "CC0-1.0";
    return o;
}

const poco::Mesh& meshOf(const poco::Record& r) { return std::get<poco::Mesh>(r.payload); }

// ---- W1.1 -----------------------------------------------------------------------------------------------------

int runFixtures(const fs::path& fixtureDir, const fs::path& work) {
    std::string error;
    const auto a = lt::buildTorqueFixtures(&error);
    const auto b = lt::buildTorqueFixtures(&error);
    check(!a.empty(), "fixtures build (" + error + ")");
    bool same = a.size() == b.size();
    for (usize i = 0; same && i < a.size(); ++i) {
        same = a[i].path == b[i].path && a[i].bytes == b[i].bytes;
    }
    check(same, "two builds are byte-identical");
    const std::string manifest = lt::fixtureManifest(a);
    std::printf("%s", manifest.c_str());
    const auto pinned = store::readFileText(fixtureDir / "generated.sha256");
    check(pinned.has_value(), "Tests/fixtures/remaster/torque/generated.sha256 exists");
    if (pinned) {
        check(normaliseNewlines(*pinned) == manifest,
              "generated fixtures match generated.sha256 (regenerate with fuse_remaster_torque_fixtures --manifest)");
    }
    freshDir(work);
    check(stageFixtures(fixtureDir, work), "stage fixtures");
    for (const lt::FixtureFile& f : a) {
        const auto onDisk = store::readFileBytes(work / f.path);
        check(onDisk && *onDisk == f.bytes, f.path + ": written bytes read back identically");
        if (!onDisk) {
            continue;
        }
        const std::string ext = fs::path(f.path).extension().string();
        if (ext == ".dts") {
            fuse::cook::DtsShape shape;
            check(fuse::cook::parse_dts(onDisk->data(), onDisk->size(), shape, &error), f.path + " parses (" + error + ")");
            // write(parse(write(x))) == write(x): the writer is the reader's exact inverse.
            fuse::cook::DtsWriteOptions options;
            options.version = shape.version;
            check(fuse::cook::write_dts(shape, options, &error) == *onDisk, f.path + ": re-written from its parse identically");
        } else if (ext == ".dsq") {
            fuse::cook::DsqFile dsq;
            check(fuse::cook::parse_dsq(onDisk->data(), onDisk->size(), dsq, &error), f.path + " parses (" + error + ")");
            check(fuse::cook::write_dsq(dsq, &error) == *onDisk, f.path + ": re-written from its parse identically");
        } else if (ext == ".png") {
            const auto image = store::decodeImage(*onDisk, &error);
            check(image && image->width == 512u && image->height == 512u, f.path + " decodes 512 x 512 (" + error + ")");
        }
    }
    for (const std::string& name : lt::committedTorqueFixtureNames()) {
        check(fs::is_regular_file(fixtureDir / name), "committed fixture " + name);
    }
#if defined(FUSE_LEGACY_TS_HAS_TSCRIPT)
    const auto script = fuse::remaster::tscript::parse_file(fixtureDir / "fixture.cs", "fixture.cs");
    check(script.ok(), "fixture.cs parses with the W1.6 TorqueScript parser (" + std::to_string(script.diags.size()) +
                           " diagnostics)");
#endif
    return finish("remaster_torque_fixtures");
}

// ---- W1.2 -----------------------------------------------------------------------------------------------------

struct ImportedPair {
    lt::ImportedShape crate, arm;
};

bool importBoth(const fs::path& src, store::BlobStore& blobs, rdb::Database* database, ImportedPair& out,
                std::string& error) {
    return lt::importTorqueShape(src / "shapes/crate.dts", {}, blobs, database, fixtureOptions(), out.crate, &error) &&
           lt::importTorqueShape(src / "shapes/arm.dts", {src / "shapes/arm_anims.dsq"}, blobs, database, fixtureOptions(),
                                 out.arm, &error);
}

int runImport(const fs::path& fixtureDir, const fs::path& work) {
    freshDir(work);
    if (!stageFixtures(fixtureDir, work / "src")) {
        return 1;
    }
    std::string error;
    store::BlobStore blobs(work / "store");
    auto database = rdb::Database::openMemory(&error);
    check(database != nullptr, "in-memory DB (" + error + ")");
    if (!database) {
        return 1;
    }
    ImportedPair imported;
    const bool ok = importBoth(work / "src", blobs, database.get(), imported, error);
    check(ok, "import crate.dts + arm.dts / arm_anims.dsq (" + error + ")");
    if (!ok) {
        return finish("remaster_legacy_ts_import");
    }
    const fuse::cook::DtsShape crateShape = lt::fixtureCrateShape();
    const fuse::cook::DtsShape armShape = lt::fixtureArmShape();
    // Crate: two LODs + one collision mesh, no skeleton.
    const lt::ImportedShape& crate = imported.crate;
    check(crate.lods.size() == 2u && crate.collision.size() == 1u && !crate.skeleton && crate.materials.size() == 1u &&
              crate.textureSets.size() == 1u,
          "crate: 2 LODs, 1 collision mesh, 1 material, 1 texture set, no skeleton");
    const u32 expectTris[2] = {768u, 48u};
    const usize sourceMesh[2] = {0u, 1u};
    for (usize l = 0; l < crate.lods.size() && l < 2u; ++l) {
        lt::PocoMeshData data;
        check(lt::loadPocoMesh(blobs, meshOf(crate.lods[l]), data, &error), "crate LOD" + std::to_string(l) + " loads");
        const f32 err = maxPositionError(crateShape.meshes[sourceMesh[l]].positions, data.positions);
        std::printf("  crate LOD%zu: %zu vertices, %zu triangles, max position error %.3g\n", l, data.positions.size() / 3u,
                    data.indices.size() / 3u, static_cast<double>(err));
        check(err <= 1e-5f, "crate LOD" + std::to_string(l) + ": positions within 1e-5 of the generated mesh");
        check(data.indices.size() / 3u == expectTris[l], "crate LOD" + std::to_string(l) + " triangle count");
        check(meshOf(crate.lods[l]).submeshes.size() == 1u &&
                  meshOf(crate.lods[l]).submeshes[0].materialSlot == crate.materials[0].id,
              "crate LOD" + std::to_string(l) + ": one submesh bound to the material record");
    }
    check(meshOf(crate.lods[0]).lodIds == std::vector<std::string>{crate.lods[1].id}, "crate LOD0 lists LOD1");
    {
        lt::PocoMeshData col;
        check(lt::loadPocoMesh(blobs, meshOf(crate.collision[0]), col, &error) && col.indices.size() == 36u &&
                  maxPositionError(crateShape.meshes[5].positions, col.positions) <= 1e-5f,
              "crate collision mesh: 12 triangles at the generated positions");
        check(std::find(crate.collision[0].tags.begin(), crate.collision[0].tags.end(), "collision") !=
                  crate.collision[0].tags.end(),
              "collision mesh tagged");
    }
    // Arm: skeleton, skin, three clips.
    const lt::ImportedShape& arm = imported.arm;
    check(arm.skeleton.has_value(), "arm: skeleton record");
    if (arm.skeleton) {
        const poco::Skeleton& sk = std::get<poco::Skeleton>(arm.skeleton->payload);
        check(sk.joints == std::vector<std::string>{"root", "upper", "lower", "tip"} &&
                  sk.parents == std::vector<s32>{-1, 0, 1, 2} && sk.inverseBind.has_value(),
              "arm: 4 joints root -> upper -> lower -> tip, inverse binds");
        const u64 hash = fuse::animation::skeleton_hash_of_joints(sk.joints, sk.parents);
        check(hash == fuse::assetcheck::skeletonHash(sk.joints, sk.parents), "skeleton hash = assetcheck rig hash");
        const std::map<std::string, f32> durations = {{"wave", 1.f}, {"walk", 0.8f}, {"idle", 2.f}};
        check(arm.clips.size() == 3u, "arm: 3 clips (1 embedded + 2 from the dsq)");
        for (const poco::Record& clip : arm.clips) {
            const poco::AnimClip& c = std::get<poco::AnimClip>(clip.payload);
            const auto expected = durations.find(clip.name);
            check(expected != durations.end() && c.duration == expected->second && c.skeletonId == arm.skeleton->id,
                  "clip '" + clip.name + "' duration " + std::to_string(c.duration));
            const auto blob = c.tracks ? blobs.get(c.tracks->sha256) : std::nullopt;
            fuse::animation::CookedAnim anim;
            check(blob && fuse::animation::read_cooked_anim(blob->data(), blob->size(), anim, &error) &&
                      anim.joints.size() == 4u && anim.skeleton_hash == hash,
                  "clip '" + clip.name + "': .fuseanim reads back, 4 joints, skeleton hash");
        }
        for (const poco::Record& clip : arm.clips) {
            if (clip.name == "wave") {
                const poco::AnimClip& c = std::get<poco::AnimClip>(clip.payload);
                check(c.rootMotion && c.events.size() == 2u && c.events[0].name == "trigger1" && c.events[0].time == 0.25f &&
                          c.events[1].name == "trigger1_off" && c.events[1].time == 0.75f,
                      "wave: root motion + trigger events");
            }
            if (clip.name == "walk") {
                const poco::AnimClip& c = std::get<poco::AnimClip>(clip.payload);
                check(c.events.size() == 2u && c.events[0].name == "trigger1" && c.events[1].name == "trigger2",
                      "walk: footstep triggers 1 and 2");
            }
        }
    }
    check(arm.lods.size() == 2u, "arm: 2 LODs");
    for (usize l = 0; l < arm.lods.size() && l < 2u; ++l) {
        lt::PocoMeshData data;
        check(lt::loadPocoMesh(blobs, meshOf(arm.lods[l]), data, &error), "arm LOD loads");
        const f32 err = maxPositionError(armShape.meshes[l].positions, data.positions);
        std::printf("  arm LOD%zu: %zu vertices, %zu triangles, max position error %.3g\n", l, data.positions.size() / 3u,
                    data.indices.size() / 3u, static_cast<double>(err));
        check(err <= 1e-5f, "arm LOD" + std::to_string(l) + ": positions within 1e-5");
        bool jointsOk = data.weights.size() == data.positions.size() / 3u * 4u;
        for (fuse::u16 j : data.joints) {
            jointsOk = jointsOk && j < 4u;
        }
        check(jointsOk && meshOf(arm.lods[l]).skeletonId == arm.skeleton->id, "arm LOD" + std::to_string(l) + ": skinned");
    }
    // Records valid and canonical in the store.
    for (const lt::ImportedShape* s : {&imported.crate, &imported.arm}) {
        for (const poco::Record* r : s->all()) {
            check(poco::validate(*r).empty(), r->id + ": valid");
            const auto back = blobs.readRecord(r->kindName(), r->id, &error);
            check(back && *back == *r, r->id + ": reads back equal");
            check(r->distribution == poco::Distribution::Ship, r->id + ": authored CC0 fixture ships");
        }
    }
    // DB: shape file -> oaid -> T0 replacement -> LOD0 record.
    const auto crateBytes = store::readFileBytes(work / "src/shapes/crate.dts");
    const std::string crateSha = fuse::remaster::sha256Hex(crateBytes->data(), crateBytes->size());
    const auto oaid = database->resolve("sha256", crateSha);
    check(oaid && *oaid == crate.oaid, "crate.dts sha256 resolves to its oaid");
    const auto replacements = oaid ? database->replacementsOf(*oaid) : std::vector<rdb::Replacement>{};
    check(replacements.size() == 1u && replacements[0].pocoId == crate.lods[0].id && replacements[0].tier == "T0",
          "crate oaid -> T0 replacement -> LOD0 mesh record");
    check(imported.arm.dsqOaids.size() == 1u && !database->replacementsOf(imported.arm.dsqOaids[0]).empty(),
          "arm_anims.dsq registered with a clip replacement");
    check(database->verify().empty(), "DB consistent");
    // Determinism: a second import into a fresh store / DB.
    store::BlobStore blobs2(work / "store2");
    auto database2 = rdb::Database::openMemory(&error);
    ImportedPair again;
    check(importBoth(work / "src", blobs2, database2.get(), again, error), "second import");
    check(blobs.listRecords() == blobs2.listRecords() && blobs.list() == blobs2.list(), "same records and blobs");
    bool sameBytes = true;
    for (const std::string& rel : blobs.listRecords()) {
        sameBytes = sameBytes && store::readFileBytes(work / "store" / rel) == store::readFileBytes(work / "store2" / rel);
    }
    check(sameBytes, "record files byte-identical");
    check(database->exportLockText() == database2->exportLockText(), "remaster.lock.json byte-identical");
    // Refusals: a truncated shape, a dsq naming nodes the shape does not have.
    {
        std::vector<u8> cut = *crateBytes;
        cut.resize(cut.size() / 2u);
        check(store::writeFileAtomic(work / "bad/cut.dts", std::span<const u8>(cut.data(), cut.size()), &error), "write cut");
        lt::ImportedShape bad;
        check(!lt::importTorqueShape(work / "bad/cut.dts", {}, blobs, nullptr, fixtureOptions(), bad, &error),
              "truncated shape refused");
        fuse::cook::DsqFile dsq = lt::fixtureArmDsq();
        dsq.nodes[1] = "elbow";
        const std::vector<u8> dsqBytes = fuse::cook::write_dsq(dsq, &error);
        check(store::writeFileAtomic(work / "bad/elbow.dsq", std::span<const u8>(dsqBytes.data(), dsqBytes.size()), &error),
              "write elbow.dsq");
        check(!lt::importTorqueShape(work / "src/shapes/arm.dts", {work / "bad/elbow.dsq"}, blobs, nullptr, fixtureOptions(),
                                     bad, &error) &&
                  error.find("not in the shape skeleton") != std::string::npos,
              "dsq with an unknown animated node refused (" + error + ")");
    }
    return finish("remaster_legacy_ts_import");
}

// ---- W1.3 -----------------------------------------------------------------------------------------------------

int runAssetcheck(const std::string& exe, const fs::path& descriptor, const std::string& gates) {
    std::string cmd = "\"" + exe + "\" check \"" + descriptor.string() + "\" --gates " + gates;
#if defined(_WIN32)
    cmd = "\"" + cmd + "\""; // cmd.exe strips one pair of outer quotes
#endif
    std::fflush(stdout);
    return std::system(cmd.c_str());
}

int runCook(const fs::path& fixtureDir, const fs::path& work, const std::string& assetcheck) {
    if (assetcheck.empty() || !fs::is_regular_file(assetcheck)) {
        std::printf("remaster_legacy_ts_cook: fuse_assetcheck not built; skipped\n");
        return 77;
    }
    freshDir(work);
    if (!stageFixtures(fixtureDir, work / "src")) {
        return 1;
    }
    std::string error;
    store::BlobStore blobs(work / "store");
    ImportedPair imported;
    check(importBoth(work / "src", blobs, nullptr, imported, error), "import (" + error + ")");
    if (g_failures != 0) {
        return finish("remaster_legacy_ts_cook");
    }
    lt::PocoRecordSet records;
    for (const lt::ImportedShape* s : {&imported.crate, &imported.arm}) {
        for (const poco::Record* r : s->all()) {
            records.records.push_back(*r);
        }
    }
    struct Case {
        const lt::ImportedShape* shape;
        lt::PocoShapeCookOptions options;
        std::string gates;
    };
    lt::PocoShapeCookOptions crateOptions;
    crateOptions.assetClass = "prop";
    crateOptions.category = "crate";
    crateOptions.base = "crate_a";
    crateOptions.lodErrors = {0.f, 0.02f};
    lt::PocoShapeCookOptions armOptions;
    armOptions.assetClass = "handheld";
    armOptions.category = "handheld";
    armOptions.base = "arm_a";
    armOptions.lodErrors = {0.f, 0.01f};
    const Case cases[] = {
        {&imported.crate, crateOptions,
         "naming,budget_mesh,lod_chain,budget_texture,texel_density,color_space,uv_bounds,geometry,lod_presence,skin"},
        {&imported.arm, armOptions, "naming,budget_mesh,lod_chain,uv_bounds,geometry,lod_presence,skin"},
    };
    for (const Case& c : cases) {
        const fs::path dirA = work / "cooked_a" / c.options.base;
        const fs::path dirB = work / "cooked_b" / c.options.base;
        lt::PocoShapeCookResult a, b;
        const bool cookedA = lt::cookPocoShape(blobs, records, c.shape->lods[0].id, dirA, c.options, a, &error);
        check(cookedA, c.options.base + ": cook (" + error + ")");
        check(lt::cookPocoShape(blobs, records, c.shape->lods[0].id, dirB, c.options, b, &error), c.options.base + ": re-cook");
        std::vector<std::string> outputs = a.meshes;
        for (const std::string* f : {&a.texture, &a.material, &a.rig, &a.descriptor}) {
            if (!f->empty()) {
                outputs.push_back(*f);
            }
        }
        for (const std::string& f : outputs) {
            const auto x = store::readFileBytes(dirA / f);
            check(x && x == store::readFileBytes(dirB / f), c.options.base + "/" + f + ": re-cook byte-identical");
        }
        for (const std::string& m : a.meshes) {
            const auto bytes = store::readFileBytes(dirA / m);
            u32 version = 0;
            if (bytes && bytes->size() >= 8u) {
                std::memcpy(&version, bytes->data() + 4, 4u);
            }
            fuse::cook::CookedMesh mesh;
            check(version == 2u && fuse::cook::load_cooked_mesh((dirA / m).string(), mesh, &error) &&
                      mesh.material_slots.size() == mesh.submeshes.size(),
                  c.options.base + "/" + m + ": FMSH v2 with material slots");
        }
        if (!a.texture.empty()) {
            fuse::cook::CookedTexture tex;
            check(fuse::cook::load_cooked_texture((dirA / a.texture).string(), tex, &error), a.texture + " loads");
        }
        if (!a.material.empty()) {
            fuse::asset::CookedMaterial mat;
            check(fuse::asset::read_cooked_material_file((dirA / a.material).string(), mat, &error) &&
                      mat.textures.albedo == (a.texture.empty() ? std::string() : fs::path(a.texture).stem().string()),
                  a.material + " reads, albedo slot = the .fusetex cook id");
        }
        const int rc = runAssetcheck(assetcheck, dirA / a.descriptor, c.gates);
        check(rc == 0, c.options.base + ": fuse_assetcheck " + c.gates + " (exit " + std::to_string(rc) + ")");
    }
    // A POCO mesh with a zero-area triangle is refused by the strict mesh gates.
    {
        poco::Mesh broken = meshOf(imported.crate.lods[1]);
        lt::PocoMeshData data;
        check(lt::loadPocoMesh(blobs, broken, data, &error), "load LOD1");
        data.indices[1] = data.indices[0];
        const std::vector<u8> idx(reinterpret_cast<const u8*>(data.indices.data()),
                                  reinterpret_cast<const u8*>(data.indices.data()) + data.indices.size() * 4u);
        auto ref = blobs.put(std::span<const u8>(idx.data(), idx.size()), "fuse/mesh-stream", &error);
        broken.indices32 = *ref;
        check(!lt::cookPocoMesh(blobs, broken, records, work / "bad.fusemesh", &error) &&
                  error.find("degenerate") != std::string::npos && !fs::exists(work / "bad.fusemesh"),
              "zero-area triangle refused (" + error + ")");
    }
    return finish("remaster_legacy_ts_cook");
}

// ---- W1.4 -----------------------------------------------------------------------------------------------------

int runWorld(const fs::path& fixtureDir, const fs::path& work) {
    freshDir(work);
    const fs::path level = work / "level";
    if (!stageFixtures(fixtureDir, level)) {
        return 1;
    }
    std::string error;
    const fs::path mission = level / "fixture.mis";
    const fuse::project::ConvertResult converted =
        fuse::project::convertT3DMissionToFuselevel(mission.string(), (work / "fixture.fuselevel").string());
    std::printf("  .fuselevel: %u entities, %u wiring stubs, %u unexpected, %u meshes\n", converted.entityCount,
                converted.wiringStubCount, converted.unexpectedStubCount, converted.components.meshes);
    check(converted.status == fuse::project::ConvertStatus::Ok && converted.unexpectedStubCount == 0u &&
              converted.components.meshes == 3u,
          "fixture.mis -> .fuselevel: zero unexpected wiring stubs, 3 TSStatic meshes (" + converted.note + ")");

    store::BlobStore blobs(work / "store");
    auto database = rdb::Database::openMemory(&error);
    lt::MissionPocoOptions options;
    options.gameId = "fixture";
    options.origin = poco::Origin::Authored;
    options.licenceId = "CC0-1.0";
    options.cookedRoot = work / "cooked";
    lt::MissionPocoResult result;
    const bool ok = lt::convertT3DMissionToPoco(mission, blobs, *database, options, result, &error);
    check(ok, "fixture.mis -> POCO level (" + error + ")");
    if (!ok) {
        return finish("remaster_world_converter_poco");
    }
    const poco::Level& lvl = std::get<poco::Level>(result.level.payload);
    std::printf("  POCO level '%s': %u entities, %zu shapes, %u without archetype\n", result.level.id.c_str(),
                result.entities, result.shapes.size(), result.unexpected);
    check(result.unexpected == 0u && result.entities == 8u, "8 entities, all with an archetype");
    check(result.shapes.size() == 2u, "two distinct shapes (crate, arm)");
    u32 tsStatics = 0;
    for (const poco::Entity& e : lvl.entities) {
        if (e.legacyClass != "TSStatic") {
            check(e.assetRefs.empty(), e.legacyName + ": no asset refs");
            continue;
        }
        ++tsStatics;
        check(e.archetype == "static_mesh" && e.assetRefs.size() == 2u && e.assetRefs[0].starts_with("oaid:") &&
                  e.assetRefs[1].starts_with("poco:"),
              e.legacyName + ": static_mesh with oaid + poco refs");
        if (e.assetRefs.size() != 2u) {
            continue;
        }
        const std::string oaid = e.assetRefs[0].substr(5);
        const std::string pocoId = e.assetRefs[1].substr(5);
        const auto replacements = database->replacementsOf(oaid);
        check(!replacements.empty() && replacements[0].pocoId == pocoId, e.legacyName + ": oaid -> replacement -> " + pocoId);
        check(blobs.readRecord("mesh", pocoId, &error).has_value(), e.legacyName + ": POCO mesh record in the store");
    }
    check(tsStatics == 3u, "3 TSStatic entities");
    // The .fuselevel's MeshAssets ids name exactly the files cooked from the POCO records.
    fuse::scene::Scene scene;
    fuse::ecs::Registry registry;
    fuse::scene::SceneFileInfo info;
    check(fuse::scene::SceneSerialiser::loadWithRegistry((work / "fixture.fuselevel").string(), scene, registry, &info).status ==
              fuse::scene::SerialiseStatus::Ok,
          ".fuselevel loads with its registry");
    std::map<u64, u32> uses;
    registry.each<fuse::ecs::MeshAssets>([&](fuse::ecs::EntityID, fuse::ecs::MeshAssets& assets) {
        if (assets.mesh.valid()) {
            ++uses[assets.mesh.value];
        }
    });
    for (const lt::MissionShapeBinding& b : result.shapes) {
        const auto it = uses.find(b.cookedAssetId.value);
        check(it != uses.end() && it->second == b.entities.size(),
              b.shapeRef + ": .fuselevel MeshAssets id used by its " + std::to_string(b.entities.size()) + " entities");
        check(b.cookedAssetId == fuse::asset::asset_id_of("game:/cooked/" + b.cookedRelPath),
              b.shapeRef + ": asset id = game:/cooked/" + b.cookedRelPath);
        fuse::cook::CookedMesh cooked;
        check(fuse::cook::load_cooked_mesh((options.cookedRoot / b.cookedRelPath).string(), cooked, &error),
              b.shapeRef + ": cooked from the POCO record at the level's asset path");
        const auto record = blobs.readRecord("mesh", b.pocoMeshId, &error);
        lt::PocoMeshData data;
        check(record && lt::loadPocoMesh(blobs, std::get<poco::Mesh>(record->payload), data, &error) &&
                  data.positions == cooked.positions && data.indices == cooked.indices,
              b.shapeRef + ": cooked mesh = POCO record streams");
    }
    check(blobs.readRecord("level", result.level.id, &error).has_value(), "level record written");
    return finish("remaster_world_converter_poco");
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: remaster_legacy_ts_tests fixtures|import|cook|world <fixture-dir> <work> [assetcheck]\n");
        return 2;
    }
    const std::string mode = argv[1];
    const fs::path fixtureDir = argv[2];
    const fs::path work = argv[3];
    if (mode == "fixtures") return runFixtures(fixtureDir, work);
    if (mode == "import") return runImport(fixtureDir, work);
    if (mode == "cook") return runCook(fixtureDir, work, argc > 4 ? argv[4] : "");
    if (mode == "world") return runWorld(fixtureDir, work);
    std::fprintf(stderr, "unknown mode '%s'\n", mode.c_str());
    return 2;
}
