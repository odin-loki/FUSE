// fuse_asset_cook_kinds (asset plan §5.1 manifest kinds, task 4d). CPU gate, labels asset;gate;cook.
//
// For each new CookAssetKind (Material, Anim, Look), through AssetCooker's manifest path (cook_manifest):
//   cooks     the output loads with its runtime reader (asset::read_cooked_material_file / read_cooked_anim_file /
//             the canonical-look check) and carries the source's values
//   cache     a second cook of the same manifest is a cache hit; the cache saved to disk and loaded again keeps the
//             kinds (a new cooker with the loaded cache hits too)
//   stale     editing the source (or the anim's events sidecar) re-cooks and the output follows the edit
//   refusal   a strict-schema violation fails with MalformedSource and writes nothing
// plus kind names / manifest parsing of "material" / "anim" / "look" and the asset graph's output-extension map.

#include "anim_fixture.hpp"

#include <fuse/animation/cooked_anim.hpp>
#include <fuse/asset/cooked_material.hpp>
#include <fuse/cook/anim_cook.hpp>
#include <fuse/project/asset_cooker.hpp>
#include <fuse/project/cook_kinds.hpp>
#include <fuse/project/cook_manifest.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

#ifndef FUSE_SOURCE_DIR
#define FUSE_SOURCE_DIR "."
#endif

namespace {

namespace fs = std::filesystem;
namespace fp = fuse::project;
using fuse::u32;

int g_failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        ++g_failures;
    }
}

std::string read_text(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void write_text(const std::string& path, const std::string& text) {
    std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
}

/// Rewrite a source so both its bytes and its mtime differ from the cached key.
void edit(const std::string& path, const std::string& text) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    write_text(path, text);
}

const char* kMaterialJson = R"({
  "fusemat": 1, "name": "rock/alpine/granite_a", "shading_model": "default_lit", "category": "stone",
  "base": { "albedo": [0.35, 0.33, 0.3], "roughness": ROUGH, "metallic": 0.0, "normal_strength": 1.0,
            "textures": { "albedo": "granite_albedo", "normal": "granite_normal" } },
  "uv_scale": 2.0,
  "triplanar": { "enabled": true, "sharpness": 6 },
  "detail": { "albedo": "detail_rock", "scale": 8, "strength": 0.5, "fade": [5, 30] },
  "layers": [ { "name": "moss", "mode": "height", "mask": "slope_up", "mask_bias": 0.6, "mask_scale": 4,
                "coverage": 0.5, "contrast": 8, "albedo": [0.2, 0.3, 0.1], "roughness": 0.8 } ]
}
)";

std::string material_json(const char* roughness) {
    std::string s = kMaterialJson;
    s.replace(s.find("ROUGH"), 5, roughness);
    return s;
}

fp::CookManifest manifest_of(const std::string& root, fp::CookAssetKind kind, const std::string& src,
                             const std::string& out) {
    fp::CookManifest m;
    m.project_root = root;
    fp::CookManifestEntry e;
    e.kind = kind;
    e.source_path = src;
    e.output_path = out;
    m.assets.push_back(e);
    return m;
}

/// Cook once through the manifest path; returns the single record.
fp::CookRecord cook_once(fp::AssetCooker& cooker, const fp::CookManifest& m) {
    const fp::CookBatchResult batch = cooker.cook_manifest(m);
    return batch.records.empty() ? fp::CookRecord{} : batch.records.front();
}

} // namespace

int main() {
    const fs::path scratch = fs::temp_directory_path() / "fuse_asset_cook_kinds";
    std::error_code ec;
    fs::remove_all(scratch, ec);
    fs::create_directories(scratch / "cooked", ec);
    const std::string dir = scratch.generic_string();

    // --- names / manifest parsing --------------------------------------------------------------------------------
    for (const fp::CookAssetKind kind : {fp::CookAssetKind::Material, fp::CookAssetKind::Anim, fp::CookAssetKind::Look}) {
        fp::CookAssetKind parsed = fp::CookAssetKind::Mesh;
        check(fp::parseCookAssetKindName(fp::cookAssetKindName(kind), parsed) && parsed == kind,
              std::string("kind name round trip: ") + fp::cookAssetKindName(kind));
    }
    const fp::CookManifestLoadResult parsedManifest = fp::parseCookManifest(
        R"({"schemaVersion": 1, "assets": [
             {"kind": "material", "sourcePath": "m.fusemat.json", "outputPath": "m.fusemat"},
             {"kind": "anim", "sourcePath": "walk.gltf", "outputPath": "walk.fuseanim"},
             {"kind": "look", "sourcePath": "warm.fuselook", "outputPath": "warm.fuselook"}]})",
        dir);
    check(parsedManifest.status == fp::CookManifestLoadStatus::Ok && parsedManifest.manifest.assets.size() == 3u &&
              parsedManifest.manifest.assets[0].kind == fp::CookAssetKind::Material &&
              parsedManifest.manifest.assets[1].kind == fp::CookAssetKind::Anim &&
              parsedManifest.manifest.assets[2].kind == fp::CookAssetKind::Look,
          "cook_manifest.json parses material / anim / look entries");

    fp::AssetCooker cooker;

    // --- Material ------------------------------------------------------------------------------------------------
    const std::string matSrc = dir + "/granite.fusemat.json";
    const std::string matOut = dir + "/cooked/granite.fusemat";
    write_text(matSrc, material_json("0.7"));
    const fp::CookManifest matManifest = manifest_of(dir, fp::CookAssetKind::Material, matSrc, matOut);
    fp::CookRecord r = cook_once(cooker, matManifest);
    check(r.ok && !r.cache_hit, "material cooks (" + r.note + ")");
    fuse::asset::CookedMaterial mat;
    std::string error;
    check(fuse::asset::read_cooked_material_file(matOut, mat, &error), "cooked .fusemat loads: " + error);
    check(mat.name == "rock/alpine/granite_a" && mat.category == 1u && mat.roughness == 0.7f && mat.triplanar &&
              mat.textures.normal == "granite_normal" && mat.layers.size() == 1u && mat.layers[0].mask == 5u &&
              mat.detail_fade[1] == 30.f,
          "material fields carried");
    r = cook_once(cooker, matManifest);
    check(r.ok && r.cache_hit, "material: second cook is a cache hit");
    edit(matSrc, material_json("0.4"));
    r = cook_once(cooker, matManifest);
    check(r.ok && !r.cache_hit, "material: edited source re-cooks");
    check(fuse::asset::read_cooked_material_file(matOut, mat) && mat.roughness == 0.4f, "material output follows the edit");
    // A binary .fusemat as the source is validated and re-encoded byte for byte.
    const std::string matCopy = dir + "/cooked/granite_copy.fusemat";
    r = cook_once(cooker, manifest_of(dir, fp::CookAssetKind::Material, matOut, matCopy));
    check(r.ok && read_text(matCopy) == read_text(matOut), "binary .fusemat source re-encodes identically");
    {
        const std::string badSrc = dir + "/bad.fusemat.json";
        const std::string badOut = dir + "/cooked/bad.fusemat";
        write_text(badSrc, R"({"fusemat": 1, "name": "x", "base": {"albedo": [0.5, 0.5, 0.5], "shiny": true}})");
        r = cook_once(cooker, manifest_of(dir, fp::CookAssetKind::Material, badSrc, badOut));
        check(!r.ok && r.status == fp::CookStatus::MalformedSource && !fs::exists(badOut),
              "material with an unknown key -> MalformedSource, nothing written (" + r.note + ")");
        write_text(badSrc, R"({"fusemat": 1, "name": "x", "base": {"albedo": [0.99, 0.99, 0.99]}})");
        r = cook_once(cooker, manifest_of(dir, fp::CookAssetKind::Material, badSrc, badOut));
        check(!r.ok && r.status == fp::CookStatus::MalformedSource, "material outside the §1.6 albedo range refused");
    }

    // --- Look ----------------------------------------------------------------------------------------------------
    const std::string lookSrc = dir + "/filmic_warm.fuselook";
    const std::string lookOut = dir + "/cooked/filmic_warm.fuselook";
    const std::string lookText = read_text(std::string(FUSE_SOURCE_DIR) + "/Samples/Looks/filmic_warm.fuselook");
    check(!lookText.empty(), "Samples/Looks/filmic_warm.fuselook readable");
    write_text(lookSrc, lookText);
    const fp::CookManifest lookManifest = manifest_of(dir, fp::CookAssetKind::Look, lookSrc, lookOut);
    r = cook_once(cooker, lookManifest);
    check(r.ok && !r.cache_hit, "look cooks (" + r.note + ")");
    check(fp::cookedLookLoads(lookOut), "cooked look is canonical");
    const std::string canonical = read_text(lookOut);
    check(canonical.find("\"bloom\":{\"intensity\":0.07,\"knee\":0.6,") != std::string::npos,
          "canonical form: sorted keys, compact, shortest numbers");
    std::string again;
    check(fp::canonicalizeLookJson(canonical, again) && again == canonical, "canonicalisation is idempotent");
    for (const char* sample : {"neutral.fuselook", "stormy_night.fuselook"}) {
        std::string out;
        check(fp::canonicalizeLookJson(read_text(std::string(FUSE_SOURCE_DIR) + "/Samples/Looks/" + sample), out, &error),
              std::string("sample look validates: ") + sample + " " + error);
    }
    r = cook_once(cooker, lookManifest);
    check(r.ok && r.cache_hit, "look: second cook is a cache hit");
    std::string edited = lookText;
    edited.replace(edited.find("\"intensity\": 0.07"), 17, "\"intensity\": 0.09");
    edit(lookSrc, edited);
    r = cook_once(cooker, lookManifest);
    check(r.ok && !r.cache_hit && read_text(lookOut).find("\"intensity\":0.09") != std::string::npos,
          "look: edited source re-cooks");
    {
        const std::string badSrc = dir + "/bad.fuselook";
        const std::string badOut = dir + "/cooked/bad.fuselook";
        write_text(badSrc, R"({"schemaVersion": 1, "kind": "fuse.material", "name": "x"})");
        r = cook_once(cooker, manifest_of(dir, fp::CookAssetKind::Look, badSrc, badOut));
        check(!r.ok && r.status == fp::CookStatus::MalformedSource && !fs::exists(badOut), "look with a wrong kind refused");
        write_text(badSrc, R"({"schemaVersion": 2, "kind": "fuse.look", "name": "x"})");
        r = cook_once(cooker, manifest_of(dir, fp::CookAssetKind::Look, badSrc, badOut));
        check(!r.ok && r.status == fp::CookStatus::MalformedSource, "look from a newer schema refused");
    }

    // --- Anim ----------------------------------------------------------------------------------------------------
    const std::string gltf = anim_fixture::write_gltf(dir, "walk");
    const std::string animOut = dir + "/cooked/walk.fuseanim";
    const std::string sidecar = fp::animEventsSidecarPath(gltf);
    write_text(sidecar, R"({"events": [{"name": "footstep_r", "time": 0.75}, {"name": "footstep_l", "time": 0.25}]})");
    const fp::CookManifest animManifest = manifest_of(dir, fp::CookAssetKind::Anim, gltf, animOut);
    r = cook_once(cooker, animManifest);
    if (!r.ok && r.status == fp::CookStatus::ImporterUnavailable) {
        std::printf("anim: assimp not linked, anim checks skipped (%s)\n", r.note.c_str());
    } else {
        check(r.ok && !r.cache_hit, "anim cooks (" + r.note + ")");
        fuse::animation::CookedAnim anim;
        check(fuse::animation::read_cooked_anim_file(animOut, anim, &error), "cooked .fuseanim loads: " + error);
        check(anim.joints.size() == 2u && anim.events.size() == 2u && anim.events[0].name == "footstep_l",
              "anim joints + sidecar events");
        r = cook_once(cooker, animManifest);
        check(r.ok && r.cache_hit, "anim: second cook is a cache hit");
        edit(sidecar, R"({"events": [{"name": "land", "time": 0.5}]})");
        r = cook_once(cooker, animManifest);
        check(r.ok && !r.cache_hit, "anim: edited events sidecar re-cooks");
        check(fuse::animation::read_cooked_anim_file(animOut, anim) && anim.events.size() == 1u &&
                  anim.events[0].name == "land",
              "anim output follows the sidecar");
        r = cook_once(cooker, animManifest);
        check(r.ok && r.cache_hit, "anim: cache hit after the re-cook");
        (void)anim_fixture::write_gltf(dir, "walk", 120.0);
        r = cook_once(cooker, animManifest);
        check(r.ok && !r.cache_hit, "anim: edited source re-cooks");
        check(fuse::animation::read_cooked_anim_file(animOut, anim) && anim.has_root_motion &&
                  std::fabs(anim.root_motion.back().dyaw - static_cast<float>(anim_fixture::kPi * 120.0 / 180.0)) < 1e-4f,
              "anim output follows the source (120 degree yaw)");
        write_text(sidecar, R"({"events": [{"name": "late", "time": 3.0}]})");
        const std::string badOut = dir + "/cooked/walk_bad.fuseanim";
        r = cook_once(cooker, manifest_of(dir, fp::CookAssetKind::Anim, gltf, badOut));
        check(!r.ok && !fs::exists(badOut), "anim with an event outside the clip refused (" + r.note + ")");
        fs::remove(sidecar, ec);
    }

    // --- cache persistence keeps the kinds -----------------------------------------------------------------------
    const std::string cachePath = dir + "/cook_cache.json";
    check(cooker.cache().save(cachePath), "cache saves");
    fp::AssetCooker reloaded;
    check(reloaded.cache().load(cachePath), "cache loads");
    r = cook_once(reloaded, matManifest);
    check(r.ok && r.cache_hit, "material cache hit after a cache reload");
    r = cook_once(reloaded, lookManifest);
    check(r.ok && r.cache_hit, "look cache hit after a cache reload");
    // A cached output damaged behind the cache's back is not served (strict liveness via the runtime reader).
    write_text(matOut, "FMAT broken");
    r = cook_once(reloaded, matManifest);
    check(r.ok && !r.cache_hit && fuse::asset::read_cooked_material_file(matOut, mat), "damaged cached material re-cooks");

    fs::remove_all(scratch, ec);
    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_asset_cook_kinds: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("fuse_asset_cook_kinds: OK\n");
    return 0;
}
