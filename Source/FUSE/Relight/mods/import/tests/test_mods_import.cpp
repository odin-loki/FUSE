// FUSE Relight RL-3.2: unit tests of the mod importer (ctest rl_mods_import_unit), and the fixture staging helper
// the fixture tests use (`fuse_relight_mods_import_tests stage <fixture> <dir>`).
//
//   tables        the §4.5 parameter tables against upstream facts (counts, ranges, defaults, enum limits);
//   material      every parameter of every surface type authored with a non-default value in a mod -> import ->
//                 POCO material + material_ext -> materialParamsFromPoco == the authored set (the round trip), the
//                 defaults round trip, clamping, unknown MDL / missing shader fallbacks, the model mapping;
//   lights        the light table round trip, POCO type mapping, the legacy un-prefixed attributes;
//   mesh          fan triangulation, holes, leftHanded winding, faceVarying welding, indexed primvars, GeomSubsets
//                 and their bindings, skinning streams, xform ops (translate / rotateXYZ / orient / scale /
//                 transform / !invert! / !resetXformStack!);
//   mod           an in-memory mod: records, keys (asset rule from rtx.conf, legacy rule algos), replacement rows,
//                 licence detection and fallbacks, deterministic bytes, DB merge, the store verifier catching seeded
//                 faults;
//   capture       an RL-1.8 capture (writeCapture) imported as a mod: the hash_key and original_asset rows equal
//                 RL-1.8's own store, mesh streams are byte-identical, RL-1.8's ingestPocoStore accepts the import.
//
// Stage helper: copies <fixture> to <dir> (without expected/, args.txt and gen_textures.txt) and generates the
// textures and packages gen_textures.txt lists, so no binary file is committed:
//   dds <path> <format> <width> <height> <seed>
//   pkg <package path> <asset name> <format> <width> <height> <seed>
//   dds9 <path> a8r8g8b8 <width> <height> <seed>      (RL-1.8's DX9 writer, as in a capture; prints its remix.tex)
// formats: rgba8, rgba8_srgb, bc1, bc5, bc7.
#include <fuse/relight/mods/import/light_table.hpp>
#include <fuse/relight/mods/import/material_table.hpp>
#include <fuse/relight/mods/import/mesh_import.hpp>
#include <fuse/relight/mods/import/mod_importer.hpp>

#include <fuse/relight/capture/export/capture_writer.hpp>
#include <fuse/relight/capture/export/dds.hpp>
#include <fuse/relight/capture/export/poco_store.hpp>
#include <fuse/relight/capture/export/usda_writer.hpp>
#include <fuse/relight/hash/geometry_hash.hpp>
#include <fuse/relight/hash/hash_string.hpp>
#include <fuse/relight/hash/texture_hash.hpp>
#include <fuse/relight/mods/assets/asset_package.hpp>
#include <fuse/relight/mods/assets/dds.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            ++g_failures;                                                                          \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
        }                                                                                          \
    } while (0)

namespace fs = std::filesystem;
namespace imp = fuse::relight::mods::import;
namespace usd = fuse::relight::mods::usd;
namespace assets = fuse::relight::mods::assets;
namespace ex = fuse::relight::capture::exporter;
namespace hash = fuse::relight::hash;
namespace json = ex::json;

std::string fmt(float v) { return usd::formatNumber(double(v)); }

std::optional<json::Value> fileJson(const imp::ImportResult& r, const std::string& rel) {
    const auto it = r.files.find(rel);
    if (it == r.files.end()) {
        return std::nullopt;
    }
    return json::parse(std::string(it->second.begin(), it->second.end()));
}

bool hasDiag(const imp::ImportResult& r, const std::string& code) {
    for (const auto& d : r.diagnostics) {
        if (d.code == code) {
            return true;
        }
    }
    return false;
}

imp::ImportResult importMemory(std::map<std::string, std::string> files, const std::string& root, const std::string& rule = {}) {
    imp::ImportOptions o;
    o.root = root;
    o.gameId = "TestGame";
    o.assetRule = rule;
    o.files = usd::memoryFileSource(std::move(files));
    o.packages = std::vector<std::string>{};
    return imp::importMod(o);
}

// ---- tables ---------------------------------------------------------------------------------------------------

void testTables() {
    using imp::SurfaceType;
    auto count = [](SurfaceType t, bool textures) {
        int n = 0;
        for (const imp::ParamDesc& d : imp::materialParamTable(t)) {
            n += (d.type == imp::ParamType::Texture) == textures ? 1 : 0;
        }
        return n;
    };
    CHECK(count(SurfaceType::Opaque, true) == 12);
    CHECK(count(SurfaceType::Opaque, false) == 37);
    CHECK(count(SurfaceType::Translucent, true) == 3);
    CHECK(count(SurfaceType::Translucent, false) == 15);
    CHECK(count(SurfaceType::Portal, true) == 1);
    CHECK(count(SurfaceType::Portal, false) == 10);
    const imp::ParamDesc* ei = imp::findParam(SurfaceType::Opaque, "emissive_intensity");
    CHECK(ei && ei->defaultValue[0] == 40.f && ei->minValue[0] == 0.f && ei->maxValue[0] == 65504.f);
    const imp::ParamDesc* dc = imp::findParam(SurfaceType::Opaque, "diffuse_color_constant");
    CHECK(dc && dc->defaultValue == (std::array<float, 3>{0.2f, 0.2f, 0.2f}));
    const imp::ParamDesc* tf = imp::findParam(SurfaceType::Opaque, "thin_film_thickness_constant");
    CHECK(tf && tf->minValue[0] == .001f && tf->maxValue[0] == 1500.f && tf->defaultValue[0] == 200.f);
    const imp::ParamDesc* at = imp::findParam(SurfaceType::Opaque, "alpha_test_type");
    CHECK(at && at->defaultValue[0] == 7.f && at->maxValue[0] == 7.f);
    const imp::ParamDesc* bt = imp::findParam(SurfaceType::Opaque, "blend_type");
    CHECK(bt && bt->defaultValue[0] == 0.f && bt->maxValue[0] == 10.f);
    const imp::ParamDesc* ior = imp::findParam(SurfaceType::Translucent, "ior_constant");
    CHECK(ior && ior->minValue[0] == 1.f && ior->maxValue[0] == 3.f && ior->defaultValue[0] == 1.3f);
    const imp::ParamDesc* tc = imp::findParam(SurfaceType::Translucent, "transmittance_color");
    CHECK(tc && tc->defaultValue == (std::array<float, 3>{0.97f, 0.97f, 0.97f}));
    const imp::ParamDesc* wrap = imp::findParam(SurfaceType::Portal, "wrap_mode_u");
    CHECK(wrap && wrap->defaultValue[0] == 1.f && wrap->maxValue[0] == 3.f);
    CHECK(imp::findParam(SurfaceType::Portal, "portal_index") != nullptr);
    CHECK(imp::findParam(SurfaceType::Portal, "unused_in_usd_so_dont") == nullptr);
    CHECK(imp::surfaceTypeFromMdl("AperturePBR_Opacity.mdl", false) == SurfaceType::Opaque);
    CHECK(imp::surfaceTypeFromMdl("./AperturePBR_Translucent.mdl", false) == SurfaceType::Translucent);
    CHECK(imp::surfaceTypeFromMdl("AperturePBR_Translucent.mdl", true) == SurfaceType::Portal);
    CHECK(imp::surfaceTypeFromMdl("AperturePBR_Portal.mdl", false) == SurfaceType::Portal);
    bool known = true;
    CHECK(imp::surfaceTypeFromMdl("OmniPBR.mdl", false, &known) == SurfaceType::Opaque && !known);
    CHECK(imp::lightParamTable().size() == 14);
}

// ---- material round trip --------------------------------------------------------------------------------------

/// A non-default, in-range value for every table parameter.
imp::ParamValue pick(const imp::ParamDesc& d, int salt) {
    imp::ParamValue v;
    v.type = d.type;
    auto within = [&](std::size_t k, double t) {
        double lo = d.minValue[k], hi = d.maxValue[k];
        if (hi - lo > 1000.0) {
            hi = lo + 8.0;
        }
        return static_cast<float>(lo + (hi - lo) * t);
    };
    switch (d.type) {
    case imp::ParamType::Texture:
        v.asset = "./tex/" + std::string(d.name) + ".dds";
        v.colorSpace = salt % 2 ? "raw" : "sRGB";
        break;
    case imp::ParamType::Float:
        v.value[0] = within(0, 0.3125 + 0.0625 * (salt % 4));
        if (v.value[0] == d.defaultValue[0]) {
            v.value[0] = within(0, 0.8125);
        }
        break;
    case imp::ParamType::Vec3:
        for (std::size_t k = 0; k < 3; ++k) {
            v.value[k] = within(k, 0.25 * double(k + 1) - 0.0625 * (salt % 3));
        }
        break;
    case imp::ParamType::Bool: v.value[0] = d.defaultValue[0] != 0.f ? 0.f : 1.f; break;
    default: {
        const int lo = int(d.minValue[0]), hi = int(d.maxValue[0]);
        int x = lo + (int(d.defaultValue[0]) - lo + 1 + salt) % (hi - lo + 1);
        if (x == int(d.defaultValue[0])) {
            x = x == hi ? lo : x + 1;
        }
        v.value[0] = float(x);
        break;
    }
    }
    return v;
}

std::string usdaAttr(const imp::ParamDesc& d, const imp::ParamValue& v) {
    const std::string n = "inputs:" + std::string(d.name);
    switch (d.type) {
    case imp::ParamType::Texture:
        return "asset " + n + " = @" + v.asset + "@ (\n    colorSpace = \"" + v.colorSpace + "\"\n)";
    case imp::ParamType::Float: return "float " + n + " = " + fmt(v.value[0]);
    case imp::ParamType::Vec3:
        return "color3f " + n + " = (" + fmt(v.value[0]) + ", " + fmt(v.value[1]) + ", " + fmt(v.value[2]) + ")";
    case imp::ParamType::Bool: return "bool " + n + " = " + (v.value[0] != 0.f ? "1" : "0");
    default: return "int " + n + " = " + std::to_string(int(v.value[0]));
    }
}

std::string materialUsda(const std::string& name, const std::string& mdl, const std::vector<std::string>& attrs) {
    std::string s = "        def Material \"" + name + "\"\n        {\n";
    s += "            token outputs:mdl:surface.connect = </RootNode/Looks/" + name + "/Shader.outputs:out>\n";
    s += "            def Shader \"Shader\"\n            {\n";
    if (!mdl.empty()) {
        s += "                uniform asset info:mdl:sourceAsset = @" + mdl + "@\n";
    }
    for (const std::string& a : attrs) {
        std::string line = a;
        std::string indented;
        for (char c : line) {
            indented += c;
            if (c == '\n') {
                indented += "                ";
            }
        }
        s += "                " + indented + "\n";
    }
    s += "                token outputs:out\n            }\n        }\n";
    return s;
}

std::string modUsda(const std::string& looks, const std::string& extra = {}) {
    return "#usda 1.0\n(\n    defaultPrim = \"RootNode\"\n)\n\ndef Xform \"RootNode\"\n{\n    def Scope \"Looks\"\n    {\n" + looks +
           "    }\n" + extra + "}\n";
}

void testMaterialRoundTrip() {
    using imp::SurfaceType;
    const std::pair<SurfaceType, const char*> surfaces[] = {
        {SurfaceType::Opaque, "AperturePBR_Opacity.mdl"},
        {SurfaceType::Translucent, "AperturePBR_Translucent.mdl"},
        {SurfaceType::Portal, "AperturePBR_Portal.mdl"}};
    const char* hashes[] = {"0000000000000A01", "0000000000000A02", "0000000000000A03"};
    std::string looks;
    std::map<std::string, imp::MaterialParams> expected;
    int salt = 0;
    for (std::size_t i = 0; i < 3; ++i) {
        const auto [surface, mdl] = surfaces[i];
        imp::MaterialParams exp = imp::defaultMaterialParams(surface);
        exp.mdlSourceAsset = mdl;
        exp.ignoreMaterial = true;
        exp.preloadTextures = true;
        std::vector<std::string> attrs{"bool inputs:ignore_material = 1", "bool inputs:preload_textures = 1"};
        for (const imp::ParamDesc& d : imp::materialParamTable(surface)) {
            const imp::ParamValue v = pick(d, salt++);
            exp.values[std::string(d.name)] = v;
            exp.authored.insert(std::string(d.name));
            attrs.push_back(usdaAttr(d, v));
        }
        const std::string name = std::string("mat_") + hashes[i];
        looks += materialUsda(name, mdl, attrs);
        expected[name] = exp;
    }
    // Defaults: a shader without parameters; an unknown MDL; a material without a shader.
    looks += materialUsda("mat_0000000000000B01", "AperturePBR_Opacity.mdl", {});
    looks += materialUsda("mat_0000000000000B02", "SomethingElse.mdl", {"float inputs:metallic_constant = 0.75"});
    looks += "        def Material \"mat_0000000000000B03\"\n        {\n        }\n";
    // Out-of-range values are clamped as upstream sanitizes them.
    looks += materialUsda("mat_0000000000000C01", "AperturePBR_Opacity.mdl",
                          {"float inputs:emissive_intensity = 70000", "int inputs:alpha_test_type = 9",
                           "color3f inputs:diffuse_color_constant = (-1, 0.5, 2)", "int inputs:alpha_test_reference_value = 128",
                           "bool inputs:blend_enabled = 1"});
    const auto res = importMemory({{"/mem/rt/mod.usda", modUsda(looks)}}, "/mem/rt");
    CHECK(res.ok);
    CHECK(res.counts.materials == 7);
    auto load = [&](const std::string& name) -> std::optional<imp::MaterialParams> {
        const auto m = fileJson(res, "poco/material/mod/testgame/rt/" + name + ".poco.json");
        const auto e = fileJson(res, "poco/material_ext/mod/testgame/rt/" + name + ".poco.json");
        CHECK(m && e);
        if (!m || !e) {
            return std::nullopt;
        }
        CHECK(m->str("licence_id") == "LicenseRef-ThirdPartyMod-rt" && m->str("distribution") == "never");
        std::string err;
        auto p = imp::materialParamsFromPoco(*m->get("payload"), *e->get("payload"), &err);
        if (!p) {
            std::fprintf(stderr, "materialParamsFromPoco(%s): %s\n", name.c_str(), err.c_str());
        }
        return p;
    };
    for (const auto& [name, exp] : expected) {
        const auto got = load(name);
        CHECK(got.has_value());
        if (!got) {
            continue;
        }
        int bad = 0;
        for (const auto& [param, v] : exp.values) {
            const auto it = got->values.find(param);
            if (it == got->values.end() || !(it->second == v)) {
                ++bad;
                std::fprintf(stderr, "round trip %s.%s differs\n", name.c_str(), param.c_str());
            }
        }
        CHECK(bad == 0);
        CHECK(got->authored == exp.authored);
        CHECK(*got == exp);
        // Every table parameter is somewhere in the two records.
        CHECK(got->values.size() == imp::materialParamTable(exp.surface).size());
    }
    // Model mapping of the fully authored opaque material: blend_enabled flipped to true -> Translucent.
    if (const auto m = fileJson(res, "poco/material/mod/testgame/rt/mat_0000000000000A01.poco.json")) {
        CHECK(m->get("payload")->str("model") == "Translucent");
    }
    if (const auto m = fileJson(res, "poco/material/mod/testgame/rt/mat_0000000000000A02.poco.json")) {
        CHECK(m->get("payload")->str("model") == "Translucent");
    }
    // Defaults round trip.
    for (const char* name : {"mat_0000000000000B01", "mat_0000000000000B02", "mat_0000000000000B03"}) {
        const auto got = load(name);
        CHECK(got.has_value());
        if (got) {
            imp::MaterialParams d = imp::defaultMaterialParams(imp::SurfaceType::Opaque);
            d.mdlSourceAsset = got->mdlSourceAsset;
            if (std::string(name) == "mat_0000000000000B02") {
                // An unknown MDL is read as AperturePBR_Opacity (upstream processMaterial's default type).
                d.values["metallic_constant"].value[0] = 0.75f;
                d.authored.insert("metallic_constant");
            }
            CHECK(got->values == d.values);
            CHECK(got->authored == d.authored);
        }
    }
    CHECK(hasDiag(res, "unknown_mdl"));
    CHECK(hasDiag(res, "no_shader"));
    if (const auto m = fileJson(res, "poco/material/mod/testgame/rt/mat_0000000000000B01.poco.json")) {
        const json::Value& p = *m->get("payload");
        CHECK(p.str("model") == "Opaque" && std::fabs(p.num("roughness") - 0.5) < 1e-9 && p.num("emissiveNits") == 0.0);
        CHECK(p.get("baseColor") && std::fabs(p.get("baseColor")->a[0].n - 0.2) < 1e-6);
    }
    // Clamping.
    if (const auto got = load("mat_0000000000000C01")) {
        CHECK(got->values.at("emissive_intensity").value[0] == 65504.f);
        CHECK(got->values.at("alpha_test_type").value[0] == 7.f);
        CHECK((got->values.at("diffuse_color_constant").value == std::array<float, 3>{0.f, 0.5f, 1.f}));
        CHECK(got->values.at("alpha_test_reference_value").value[0] == 128.f);
    }
    CHECK(hasDiag(res, "material_param"));
    // Keys: mat_<H> -> remix.tex, one replacement row per material.
    CHECK(res.db.get("hash_key") && res.db.get("hash_key")->a.size() == 7);
    CHECK(res.db.get("replacement") && res.db.get("replacement")->a.size() == 7);
    if (res.db.get("hash_key") && !res.db.get("hash_key")->a.empty()) {
        CHECK(res.db.get("hash_key")->a[0].str("algo") == "remix.tex");
    }
    // Missing textures are reported, never fatal.
    CHECK(hasDiag(res, "missing_texture"));
}

// ---- lights ---------------------------------------------------------------------------------------------------

void testLights() {
    const std::string lights = R"(    def Scope "lights"
    {
        def SphereLight "light_00000000000000L1"
        {
        }
        def SphereLight "light_0000000000000D01" (
            prepend apiSchemas = ["ShapingAPI"]
        )
        {
            float inputs:radius = 2.5
            float inputs:width = 1
            float inputs:height = 2
            float inputs:length = 3
            float inputs:angle = 4
            bool inputs:enableColorTemperature = 1
            color3f inputs:color = (0.25, 0.5, 0.75)
            float inputs:colorTemperature = 3200
            float inputs:exposure = 1
            float inputs:intensity = 300
            float inputs:shaping:cone:angle = 45
            float inputs:shaping:cone:softness = 0.5
            float inputs:shaping:focus = 2
            float inputs:volumetric_radiance_scale = 0.5
            double3 xformOp:translate = (1, 2, 3)
            uniform token[] xformOpOrder = ["xformOp:translate"]
        }
        def RectLight "light_0000000000000D02"
        {
            float width = 4
            float height = 5
            float intensity = 7
        }
        def DistantLight "light_0000000000000D03"
        {
            float inputs:angle = 0.53
        }
        def DomeLight "light_0000000000000D04"
        {
        }
    }
)";
    const auto res = importMemory({{"/mem/lt/mod.usda", modUsda("", lights)}}, "/mem/lt/mod.usda");
    CHECK(res.ok);
    CHECK(res.counts.lights == 4); // light_00000000000000L1 is not a hex name -> unrecognized, skipped by RL-3.1
    const auto spot = fileJson(res, "poco/light/mod/testgame/lt/light_0000000000000D01.poco.json");
    CHECK(spot.has_value());
    if (spot) {
        const json::Value& p = *spot->get("payload");
        CHECK(p.str("type") == "Spot");
        CHECK(std::fabs(p.num("intensity") - 600.0) < 1e-9);
        CHECK(std::fabs(p.num("outerCone") - 45.0 * 3.14159265358979323846 / 180.0) < 1e-9);
        CHECK(std::fabs(p.num("innerCone") - 0.5 * p.num("outerCone")) < 1e-9);
        const auto lp = imp::lightParamsFromPoco(p);
        CHECK(lp.has_value());
        if (lp) {
            CHECK(lp->values.at("radius")[0] == 2.5f && lp->values.at("colorTemperature")[0] == 3200.f);
            CHECK((lp->values.at("color") == std::array<float, 3>{0.25f, 0.5f, 0.75f}));
            CHECK(lp->values.at("enableColorTemperature")[0] == 1.f && lp->values.at("volumetric_radiance_scale")[0] == 0.5f);
            CHECK(lp->authored.size() == 14);
        }
        const json::Value* xf = p.get("relight")->get("transform");
        CHECK(xf && xf->a.size() == 16 && xf->a[12].n == 1.0 && xf->a[13].n == 2.0 && xf->a[14].n == 3.0);
        CHECK(spot->get("replaces") && spot->get("replaces")->a.size() == 1);
    }
    const auto rect = fileJson(res, "poco/light/mod/testgame/lt/light_0000000000000D02.poco.json");
    CHECK(rect && rect->get("payload")->str("type") == "Rect");
    if (rect) {
        const json::Value* size = rect->get("payload")->get("size");
        CHECK(size && size->a[0].n == 4.0 && size->a[1].n == 5.0); // legacy un-prefixed attributes
        CHECK(rect->get("payload")->num("intensity") == 7.0);
    }
    const auto dist = fileJson(res, "poco/light/mod/testgame/lt/light_0000000000000D03.poco.json");
    CHECK(dist && dist->get("payload")->str("type") == "Directional");
    CHECK(hasDiag(res, "unsupported_light")); // DomeLight
    CHECK(res.db.get("replacement") && res.db.get("replacement")->a.size() == 4);
    bool lightKeys = true;
    for (const json::Value& k : res.db.get("hash_key")->a) {
        lightKeys &= k.str("algo") == "remix.light" && k.str("kind") == "light";
    }
    CHECK(lightKeys);
}

// ---- mesh and transforms --------------------------------------------------------------------------------------

bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

void testTransforms() {
    const std::string text = R"(#usda 1.0
def Xform "a"
{
    double3 xformOp:translate = (1, 2, 3)
    float3 xformOp:rotateXYZ = (0, 90, 0)
    float3 xformOp:scale = (2, 2, 2)
    uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:rotateXYZ", "xformOp:scale"]
    def Xform "b"
    {
        quatf xformOp:orient = (0.70710678, 0, 0, 0.70710678)
        double3 xformOp:translate:pivot = (5, 0, 0)
        uniform token[] xformOpOrder = ["xformOp:translate:pivot", "xformOp:orient", "!invert!xformOp:translate:pivot"]
        def Xform "c"
        {
            matrix4d xformOp:transform = ( (1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (10, 20, 30, 1) )
            uniform token[] xformOpOrder = ["!resetXformStack!", "xformOp:transform"]
        }
    }
}
)";
    usd::ReadOptions o;
    o.files = usd::memoryFileSource({{"/mem/x.usda", text}});
    const usd::ComposedStage st = usd::readStage("/mem/x.usda", o);
    CHECK(st.ok);
    const usd::Prim* a = st.find("/a");
    CHECK(a != nullptr);
    if (!a) {
        return;
    }
    // (1,0,0) scaled -> (2,0,0), rotated 90 about Y -> (0,0,-2), translated -> (1,2,1).
    const imp::Mat4d m = imp::localTransform(*a);
    const double x = 1 * m[0] + 0 * m[4] + 0 * m[8] + m[12];
    const double y = 1 * m[1] + m[13];
    const double z = 1 * m[2] + m[14];
    CHECK(near(x, 1) && near(y, 2) && near(z, 1));
    // b: rotate 90 about Z around the pivot (5,0,0): (6,0,0) -> (5,1,0); then a: (5,1,0) -> scale (10,2,0) -> rotY
    // (0,2,-10) -> translate (1,4,-7).
    const imp::Mat4d mb = imp::relativeTransform(st, "/a/b", "");
    const double bx = 6 * mb[0] + mb[12], by = 6 * mb[1] + mb[13], bz = 6 * mb[2] + mb[14];
    CHECK(std::fabs(bx - 1) < 1e-6 && std::fabs(by - 4) < 1e-6 && std::fabs(bz + 7) < 1e-6);
    // c resets the stack: only its own transform.
    const imp::Mat4d mc = imp::relativeTransform(st, "/a/b/c", "");
    CHECK(mc[12] == 10 && mc[13] == 20 && mc[14] == 30 && mc[0] == 1);
    // Relative to b (b's own ops included): c still resets.
    bool reset = false;
    imp::localTransform(*st.find("/a/b/c"), &reset);
    CHECK(reset);
}

void testMesh() {
    const std::string text = R"(#usda 1.0
def Xform "root"
{
    rel material:binding = </root/M1>
    def Material "M1" {}
    def Material "M2" {}
    def Mesh "quad" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
        int[] faceVertexCounts = [4, 3, 2, 3]
        int[] faceVertexIndices = [0, 1, 2, 3, 0, 2, 4, 0, 1, 3, 2, 4]
        int[] holeIndices = [3]
        point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0), (2, 2, 0)]
        uniform token orientation = "leftHanded"
        uniform bool doubleSided = 1
        texCoord2f[] primvars:st = [(0, 0), (1, 0), (1, 1), (0, 1)] (
            interpolation = "faceVarying"
        )
        int[] primvars:st:indices = [0, 1, 2, 3, 0, 2, 1, 0, 1, 0, 0, 0]
        normal3f[] normals = [(0, 0, 1)] (
            interpolation = "constant"
        )
        def GeomSubset "second" (
            prepend apiSchemas = ["MaterialBindingAPI"]
        )
        {
            uniform token elementType = "face"
            int[] indices = [1, 1, 7]
            rel material:binding = </root/M2>
        }
    }
    def Mesh "vertexOnly"
    {
        int[] faceVertexCounts = [3]
        int[] faceVertexIndices = [0, 1, 2]
        point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
        color3f[] primvars:displayColor = [(1, 0, 0), (0, 1, 0), (0, 0, 1)] (
            interpolation = "vertex"
        )
        int[] primvars:skel:jointIndices = [0, 1, 1, 2, 2, 3] (
            elementSize = 2
            interpolation = "vertex"
        )
        float[] primvars:skel:jointWeights = [0.5, 0.5, 1, 0, 0.25, 0.75] (
            elementSize = 2
            interpolation = "vertex"
        )
    }
}
)";
    usd::ReadOptions o;
    o.files = usd::memoryFileSource({{"/mem/m.usda", text}});
    const usd::ComposedStage st = usd::readStage("/mem/m.usda", o);
    CHECK(st.ok);
    const usd::Prim* quad = st.find("/root/quad");
    CHECK(quad != nullptr);
    if (!quad) {
        return;
    }
    std::vector<imp::MeshIssue> issues;
    const auto m = imp::importMesh(st, *quad, "/root", &issues);
    CHECK(m.has_value());
    if (!m) {
        return;
    }
    CHECK(m->expanded && m->leftHanded && m->doubleSided);
    // Faces: quad (2 tris, subset-less), triangle (subset "second"), degenerate (skipped), hole (skipped).
    CHECK(m->indices.size() == 9);
    CHECK(m->submeshes.size() == 2);
    if (m->submeshes.size() == 2) {
        CHECK(m->submeshes[0].subsetPath == "/root/quad/second" && m->submeshes[0].materialPath == "/root/M2");
        CHECK(m->submeshes[0].indexOffset == 0 && m->submeshes[0].indexCount == 3);
        CHECK(m->submeshes[1].subsetPath.empty() && m->submeshes[1].materialPath == "/root/M1");
        CHECK(m->submeshes[1].indexCount == 6);
    }
    // Welding: corners (v0,st0) (v1,st1) (v2,st2) (v3,st3) (v0,st0) (v2,st2) (v4,st1) -> 5 unique vertices.
    CHECK(m->points.size() == 5);
    CHECK(m->uv0.size() == 5 && m->normals.size() == 5);
    // leftHanded: the quad's first fan triangle (0,1,2) becomes (0,2,1).
    if (m->indices.size() == 9) {
        CHECK(m->points[m->indices[3]] == (std::array<float, 3>{0.f, 0.f, 0.f}));
        CHECK(m->points[m->indices[4]] == (std::array<float, 3>{1.f, 1.f, 0.f}));
        CHECK(m->points[m->indices[5]] == (std::array<float, 3>{1.f, 0.f, 0.f}));
    }
    bool warned = false;
    for (const auto& i : issues) {
        warned |= i.message.find("fewer than 3") != std::string::npos;
    }
    CHECK(warned);
    CHECK(m->bounds == (std::array<float, 6>{0.f, 0.f, 0.f, 2.f, 2.f, 0.f}));

    const usd::Prim* vo = st.find("/root/vertexOnly");
    const auto v = vo ? imp::importMesh(st, *vo, "/root", nullptr) : std::nullopt;
    CHECK(v.has_value());
    if (v) {
        CHECK(!v->expanded && v->points.size() == 3 && v->indices == (std::vector<std::uint32_t>{0, 1, 2}));
        CHECK(v->colors.size() == 3 && v->colors[1] == (std::array<float, 4>{0.f, 1.f, 0.f, 1.f}));
        CHECK(v->influences == 2 && v->jointIndices == (std::vector<std::int32_t>{0, 1, 1, 2, 2, 3}));
        CHECK(imp::joints0Stream(*v).size() == 3 * 8 && imp::weights0Stream(*v).size() == 3 * 16);
        CHECK(imp::color0Stream(*v) == (std::vector<std::uint8_t>{255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255}));
        CHECK(v->submeshes.size() == 1 && v->submeshes[0].materialPath == "/root/M1"); // inherited binding
    }
}

// ---- a whole mod ----------------------------------------------------------------------------------------------

std::map<std::string, std::string> sampleMod(bool licence) {
    std::map<std::string, std::string> f;
    f["/mem/sample/mod.usda"] = R"(#usda 1.0
(
    customLayerData = {
        string lightspeed_game_name = "Sample"
        string lightspeed_layer_type = "replacement"
    }
    defaultPrim = "RootNode"
    upAxis = "Y"
    metersPerUnit = 1
)
def Xform "RootNode"
{
    def Scope "meshes"
    {
        def Xform "mesh_00000000000000C1" (
            prepend apiSchemas = ["MaterialBindingAPI", "ParticleSystemAPI"]
        )
        {
            int preserveOriginalDrawCall = 0
            custom bool remix_category:sky = 1
            custom bool remix_category:ignore = 0
            float primvars:particle:minTimeToLive = 1.5
            rel material:binding = </RootNode/Looks/mat_00000000000000A1>
            def Mesh "tri" (
                prepend references = @./assets/tri.usda@
            )
            {
                double3 xformOp:translate = (0, 5, 0)
                uniform token[] xformOpOrder = ["xformOp:translate"]
            }
            def SphereLight "lamp"
            {
                float inputs:intensity = 10
            }
        }
    }
    def Scope "Looks"
    {
        def Material "mat_00000000000000A1"
        {
            def Shader "Shader"
            {
                uniform asset info:mdl:sourceAsset = @AperturePBR_Opacity.mdl@
                asset inputs:diffuse_texture = @./tex/albedo.dds@ (
                    colorSpace = "auto"
                )
                asset inputs:reflectionroughness_texture = @./tex/rough.dds@
                token outputs:out
            }
        }
    }
    def Scope "lights"
    {
        def DistantLight "light_00000000000000D1"
        {
            float inputs:intensity = 3
        }
    }
}
)";
    f["/mem/sample/assets/tri.usda"] = R"(#usda 1.0
(
    defaultPrim = "tri"
)
def Mesh "tri"
{
    int[] faceVertexCounts = [3]
    int[] faceVertexIndices = [0, 1, 2]
    point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
    normal3f[] normals = [(0, 0, 1), (0, 0, 1), (0, 0, 1)] (
        interpolation = "vertex"
    )
    texCoord2f[] primvars:st = [(0, 0), (1, 0), (0, 1)] (
        interpolation = "vertex"
    )
}
)";
    f["/mem/sample/rtx.conf"] = "# rules\nrtx.geometryAssetHashRuleString = positions, indices\n";
    if (licence) {
        f["/mem/sample/LICENSE.txt"] = "Creative Commons\nAttribution 4.0\nInternational Public License\n";
    }
    assets::TextureImage img;
    img.format = assets::TexFormat::R8G8B8A8_UNORM;
    img.width = img.height = 4;
    img.data.assign(64, 0x7f);
    assets::layoutSubresources(img);
    const auto dds = assets::writeDds(img);
    f["/mem/sample/tex/albedo.dds"] = std::string(dds.begin(), dds.end());
    return f;
}

void testMod() {
    const auto a = importMemory(sampleMod(true), "/mem/sample");
    const auto b = importMemory(sampleMod(true), "/mem/sample");
    CHECK(a.ok && b.ok);
    CHECK(a.files == b.files); // deterministic bytes
    CHECK(a.licenceId == "CC-BY-4.0");
    CHECK(a.counts.meshReplacements == 1 && a.counts.meshes == 1 && a.counts.materials == 1 && a.counts.lights == 2);
    CHECK(a.counts.textures == 1 && a.counts.particles == 1);
    CHECK(hasDiag(a, "missing_texture")); // rough.dds
    const auto repl = fileJson(a, "poco/relight_replacement/mod/testgame/sample/mesh_00000000000000C1.poco.json");
    CHECK(repl.has_value());
    if (repl) {
        const json::Value& p = *repl->get("payload");
        CHECK(p.get("preserveOriginalDrawCall") && p.get("preserveOriginalDrawCall")->kind == json::Value::Kind::Bool &&
              !p.get("preserveOriginalDrawCall")->b);
        CHECK(p.get("key")->str("algo") == "remix.geom.asset");
        CHECK(p.get("key")->str("rule") == "positions,indices");
        CHECK(p.get("categories")->get("set")->a.size() == 1 && p.get("categories")->get("set")->a[0].s == "Sky");
        CHECK(p.get("categories")->get("cleared")->a.size() == 1 && p.get("categories")->get("cleared")->a[0].s == "Ignore");
        CHECK(p.get("parts")->a.size() == 1);
        if (p.get("parts")->a.size() == 1) {
            const json::Value& part = p.get("parts")->a[0];
            CHECK(part.str("mesh") == "poco:mod/testgame/sample/mesh_00000000000000C1/tri");
            CHECK(part.get("transform")->a[13].n == 5.0);
            CHECK(part.get("materials")->a[0].s == "poco:mod/testgame/sample/mat_00000000000000A1");
        }
        CHECK(p.get("lights")->a.size() == 1 && p.str("particles") == "poco:mod/testgame/sample/mesh_00000000000000C1");
        CHECK(repl->str("licence_id") == "CC-BY-4.0" && repl->str("distribution") == "never");
        CHECK(repl->get("provenance")->str("origin") == "derived" && repl->get("provenance")->get("derived_from")->a[0].s == "mod:sample");
    }
    const auto ts = fileJson(a, "poco/texture_set/mod/testgame/sample/mat_00000000000000A1.poco.json");
    CHECK(ts && ts->get("payload")->get("albedo") && ts->get("payload")->flag("albedoSrgb"));
    const auto ext = fileJson(a, "poco/material_ext/mod/testgame/sample/mat_00000000000000A1.poco.json");
    CHECK(ext.has_value());
    if (ext) {
        const json::Value* t = ext->get("payload")->get("textures");
        CHECK(t && t->get("diffuse_texture") && t->get("diffuse_texture")->str("status") == "ok");
        CHECK(t && t->get("diffuse_texture")->str("format") == "R8G8B8A8_UNORM" && t->get("diffuse_texture")->flag("srgb"));
        CHECK(t && t->get("reflectionroughness_texture") && t->get("reflectionroughness_texture")->str("status") == "missing");
    }
    // DB: rule id of "positions,indices"; one key + one replacement per replaced hash.
    const std::string ruleId = hash::hashToString(hash::hashRuleId(hash::parseHashRule("positions,indices")));
    int geom = 0;
    for (const json::Value& k : a.db.get("hash_key")->a) {
        if (k.str("algo") == "remix.geom.asset") {
            ++geom;
            CHECK(k.str("value") == "00000000000000C1" && k.str("rule_id") == ruleId);
            CHECK(k.str("oaid") == ex::originalAssetId("testgame", "remix.geom.asset", "00000000000000C1"));
        }
    }
    CHECK(geom == 1);
    CHECK(a.db.get("hash_key")->a.size() == 3 && a.db.get("replacement")->a.size() == 3);
    CHECK(a.db.get("original_asset")->a.size() == 3);

    // Licence fallbacks.
    const auto noLicence = importMemory(sampleMod(false), "/mem/sample");
    CHECK(noLicence.licenceId == "LicenseRef-ThirdPartyMod-sample" && hasDiag(noLicence, "no_licence_file"));
    CHECK(imp::detectSpdxLicence("// SPDX-License-Identifier: CC0-1.0\n") == "CC0-1.0");
    CHECK(imp::detectSpdxLicence("Permission is hereby granted, free of charge, to any person\nobtaining a copy of this") == "MIT");
    CHECK(imp::detectSpdxLicence("Attribution-NonCommercial-ShareAlike 4.0 International") == "CC-BY-NC-SA-4.0");
    CHECK(imp::detectSpdxLicence("all rights reserved").empty());

    // Legacy rule: the key algo follows the rule.
    const auto legacy = importMemory(sampleMod(true), "/mem/sample", "legacypositions0,legacyindices");
    bool legacyKey = false;
    for (const json::Value& k : legacy.db.get("hash_key")->a) {
        legacyKey |= k.str("algo") == "remix.geom.legacy0" && k.str("value") == "00000000000000C1";
    }
    CHECK(legacyKey);

    // Store round trip: write, verify, merge, seeded faults.
    const fs::path dir = fs::current_path() / "rl_mods_import_unit_store";
    std::error_code ec;
    fs::remove_all(dir, ec);
    std::string err;
    CHECK(imp::writeStore(dir, a, true, &err));
    imp::VerifyResult v = imp::verifyStore(dir);
    for (const auto& e : v.errors) {
        std::fprintf(stderr, "verify: %s\n", e.c_str());
    }
    CHECK(v.ok() && v.records == a.counts.records && v.replacements == 3);
    CHECK(imp::writeStore(dir, legacy, true, &err)); // merge: legacy key rows join the asset-rule rows
    std::string dbText;
    ex::readFile(dir / "db" / "remaster_db.json", dbText);
    const auto merged = json::parse(dbText);
    CHECK(merged && merged->get("hash_key")->a.size() == 4);
    CHECK(imp::verifyStore(dir).ok());
    // Corrupt a blob and remove a referenced record.
    const json::Value* blobRef = ts ? ts->get("payload")->get("albedo") : nullptr;
    if (blobRef) {
        const std::string sha = blobRef->str("sha256");
        ex::writeFile(dir / "blobs" / "sha256" / sha.substr(0, 2) / sha, std::string("corrupt"));
    }
    fs::remove(dir / "poco" / "mesh" / "mod" / "testgame" / "sample" / "mesh_00000000000000C1" / "tri.poco.json", ec);
    v = imp::verifyStore(dir);
    bool blobErr = false, refErr = false;
    for (const auto& e : v.errors) {
        blobErr |= e.find("wrong size or content") != std::string::npos;
        refErr |= e.find("references missing record") != std::string::npos;
    }
    CHECK(blobErr && refErr);
    fs::remove_all(dir, ec);
}

// ---- captures are mods too ------------------------------------------------------------------------------------

ex::CaptureData buildCapture() {
    ex::CaptureData c;
    c.meta.gameId = "CapGame";
    c.meta.windowTitle = "Capture Game";
    c.meta.exeName = "game.exe";
    c.meta.geometryHashRule = std::string(hash::rules::kDefaultAssetRuleString);
    c.meta.stageName = "capture";
    c.meta.numFramesCaptured = 1;
    std::vector<hash::Hash64> texHashes;
    for (int t = 0; t < 2; ++t) {
        ex::CaptureTexture tex;
        tex.d3dFormat = static_cast<std::uint32_t>(hash::D3DFormat::A8R8G8B8);
        tex.width = tex.height = 4;
        std::uint32_t seed = 0x1234u + std::uint32_t(t);
        for (int i = 0; i < 64; ++i) {
            seed = seed * 1664525u + 1013904223u;
            tex.mip0.push_back(static_cast<std::uint8_t>(seed >> 24));
        }
        tex.hash = hash::hashTextureMip0(tex.mip0.data(), tex.mip0.size());
        c.textures[tex.hash] = tex;
        ex::CaptureMaterial m;
        m.hash = m.albedoTexture = tex.hash;
        m.wrapU = ex::mdl::kWrapClamp;
        m.filter = ex::mdl::kFilterNearest;
        c.materials[m.hash] = m;
        texHashes.push_back(tex.hash);
    }
    ex::CaptureMesh a;
    a.hash = 0xA1B2C3D4E5F60718ull;
    a.points = {{0.f, 0.f, 0.f}, {1.5f, 0.f, 0.f}, {0.f, 2.25f, 0.f}, {0.1f, 0.2f, 0.3f}};
    a.normals = {{0.f, 0.f, 1.f}, {0.f, 0.f, 1.f}, {0.f, 0.f, 1.f}, {0.f, 0.f, 1.f}};
    a.texcoords = {{0.f, 1.f}, {1.f, 1.f}, {0.f, 0.f}, {0.33f, 0.66f}};
    a.colors = {{1.f, 0.f, 0.f, 1.f}, {0.f, 1.f, 0.f, 0.5f}, {0.f, 0.f, 1.f, 0.25f}, {1.f, 1.f, 1.f, 1.f}};
    a.indices = {0, 1, 2, 2, 1, 3};
    a.materialHash = texHashes[0];
    a.isDoubleSided = true;
    c.meshes[a.hash] = a;
    ex::CaptureMesh b;
    b.hash = 0x0000000000000042ull;
    b.points = {{-1.f, -1.f, 0.f}, {1.f, -1.f, 0.f}, {0.f, 1.f, 0.f}};
    b.indices = {0, 1, 2};
    b.materialHash = texHashes[1];
    c.meshes[b.hash] = b;
    ex::CaptureMesh d = b; // same content under another hash: one shared canonical key
    d.hash = 0x0000000000000043ull;
    d.materialHash = 0;
    c.meshes[d.hash] = d;
    for (const auto& [h, mesh] : c.meshes) {
        ex::CaptureInstance i;
        i.id = h & 0xff;
        i.mesh = h;
        i.material = mesh.materialHash;
        i.xforms.push_back({0.0, ex::identity4d()});
        c.instances[i.id] = i;
    }
    ex::CaptureSphereLight s;
    s.hash = 0x5151515151515151ull;
    s.color = {1.f, 0.5f, 0.25f};
    s.radius = 0.5f;
    s.intensity = 100.f;
    s.xforms.push_back({0.0, ex::identity4d()});
    c.sphereLights[s.hash] = s;
    ex::CaptureDistantLight dl;
    dl.hash = 0xD1D1D1D1D1D1D1D1ull;
    dl.color = {1.f, 1.f, 1.f};
    dl.intensity = 2.f;
    dl.angleDegrees = 0.5f;
    c.distantLights[dl.hash] = dl;
    return c;
}

std::string arrayText(const json::Value* v) { return v ? json::write(*v) : std::string("<missing>"); }

void testCaptureImport() {
    const fs::path dir = fs::current_path() / "rl_mods_import_unit_capture";
    std::error_code ec;
    fs::remove_all(dir, ec);
    const ex::CaptureData c = buildCapture();
    ex::CaptureWriteReport rep;
    CHECK(ex::writeCapture(dir, c, hash::parseHashRule(hash::rules::kDefaultAssetRuleString), &rep));
    CHECK(rep.ok());

    imp::ImportOptions o;
    o.root = (dir / ex::captureStageFileName(c.meta)).generic_string();
    o.gameId = c.meta.gameId;
    const imp::ImportResult r = imp::importMod(o);
    CHECK(r.ok && r.capture);
    for (const auto& d : r.diagnostics) {
        std::fprintf(stderr, "capture import: %s %s %s: %s\n", d.severity.c_str(), d.code.c_str(), d.where.c_str(), d.message.c_str());
    }
    CHECK(r.diagnostics.empty());
    CHECK(r.counts.meshes == 3 && r.counts.materials == 2 && r.counts.lights == 2 && r.counts.textures == 2);
    CHECK(r.counts.replacements == 0);

    // The DB rows equal RL-1.8's own store.
    std::string dbText;
    CHECK(ex::readFile(dir / "store" / "db" / "remaster_db.json", dbText));
    const auto rl18 = json::parse(dbText);
    CHECK(rl18.has_value());
    if (rl18) {
        CHECK(arrayText(rl18->get("hash_key")) == arrayText(r.db.get("hash_key")));
        CHECK(arrayText(rl18->get("original_asset")) == arrayText(r.db.get("original_asset")));
        CHECK(rl18->str("game_id") == r.db.str("game_id"));
        if (arrayText(rl18->get("hash_key")) != arrayText(r.db.get("hash_key"))) {
            std::fprintf(stderr, "RL-1.8 keys: %s\nimported:    %s\n", arrayText(rl18->get("hash_key")).c_str(),
                         arrayText(r.db.get("hash_key")).c_str());
        }
    }
    // Mesh streams are byte-identical to RL-1.8's POCO mesh records.
    const std::string name = dir.filename().string();
    for (const auto& [h, mesh] : c.meshes) {
        (void)mesh;
        const std::string hs = hash::hashToString(h);
        std::string text;
        CHECK(ex::readFile(dir / "store" / "poco" / "mesh" / "cap" / "capgame" / ("mesh_" + hs + ".poco.json"), text));
        const auto theirs = json::parse(text);
        const auto ours = fileJson(r, "poco/mesh/capture/capgame/" + name + "/mesh_" + hs + "/mesh.poco.json");
        CHECK(theirs && ours);
        if (!theirs || !ours) {
            continue;
        }
        CHECK(arrayText(theirs->get("payload")->get("streams")) == arrayText(ours->get("payload")->get("streams")));
        CHECK(arrayText(theirs->get("payload")->get("indices32")) == arrayText(ours->get("payload")->get("indices32")));
        CHECK(ours->str("original_asset") == theirs->str("original_asset"));
        CHECK(ours->str("licence_id") == "LicenseRef-Original-capgame" && ours->get("provenance")->str("origin") == "original");
    }
    // Material sampler state comes back from the capture's un-prefixed attributes.
    for (const auto& [h, m] : c.materials) {
        (void)m;
        const auto mr = fileJson(r, "poco/material/capture/capgame/" + name + "/mat_" + hash::hashToString(h) + ".poco.json");
        const auto er = fileJson(r, "poco/material_ext/capture/capgame/" + name + "/mat_" + hash::hashToString(h) + ".poco.json");
        CHECK(mr && er);
        if (mr && er) {
            const auto p = imp::materialParamsFromPoco(*mr->get("payload"), *er->get("payload"));
            CHECK(p && p->values.at("wrap_mode_u").value[0] == 0.f && p->values.at("filter_mode").value[0] == 0.f);
        }
    }
    // Written store: our verifier and RL-1.8's re-ingest both accept it, with RL-1.8's key set.
    std::string err;
    CHECK(imp::writeStore(dir / "imported", r, true, &err));
    const imp::VerifyResult v = imp::verifyStore(dir / "imported");
    for (const auto& e : v.errors) {
        std::fprintf(stderr, "verify: %s\n", e.c_str());
    }
    CHECK(v.ok());
    const ex::IngestResult in = ex::ingestPocoStore(dir / "imported");
    for (const auto& e : in.errors) {
        std::fprintf(stderr, "ingest: %s\n", e.c_str());
    }
    CHECK(in.ok());
    CHECK(in.keys == rep.keys);
    // Importing into the capture's own store merges without conflicts (same oaids).
    CHECK(imp::writeStore(dir / "store", r, true, &err));
    CHECK(imp::verifyStore(dir / "store").ok());
    fs::remove_all(dir, ec);
}

// ---- fixture staging ------------------------------------------------------------------------------------------

std::optional<assets::TexFormat> formatByName(const std::string& n) {
    if (n == "rgba8") {
        return assets::TexFormat::R8G8B8A8_UNORM;
    }
    if (n == "rgba8_srgb") {
        return assets::TexFormat::R8G8B8A8_SRGB;
    }
    if (n == "bc1") {
        return assets::TexFormat::BC1_RGBA_UNORM_BLOCK;
    }
    if (n == "bc5") {
        return assets::TexFormat::BC5_UNORM_BLOCK;
    }
    if (n == "bc7") {
        return assets::TexFormat::BC7_UNORM_BLOCK;
    }
    return std::nullopt;
}

std::optional<assets::TextureImage> makeImage(const std::string& format, std::uint32_t w, std::uint32_t h, std::uint32_t seed) {
    const auto f = formatByName(format);
    if (!f) {
        return std::nullopt;
    }
    assets::TextureImage img;
    img.format = *f;
    img.width = w;
    img.height = h;
    const std::uint64_t size = assets::layoutSubresources(img);
    if (size == 0) {
        return std::nullopt;
    }
    img.data.resize(size);
    for (auto& b : img.data) {
        seed = seed * 1664525u + 1013904223u;
        b = static_cast<std::uint8_t>(seed >> 24);
    }
    return img;
}

int stage(const fs::path& fixture, const fs::path& out) {
    std::error_code ec;
    fs::remove_all(out, ec);
    fs::create_directories(out, ec);
    for (const auto& e : fs::recursive_directory_iterator(fixture, ec)) {
        const fs::path rel = e.path().lexically_relative(fixture);
        const std::string first = rel.begin()->string();
        if (first == "expected" || rel == "args.txt" || rel == "gen_textures.txt") {
            continue;
        }
        if (e.is_directory()) {
            fs::create_directories(out / rel, ec);
        } else {
            fs::create_directories((out / rel).parent_path(), ec);
            fs::copy_file(e.path(), out / rel, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                std::fprintf(stderr, "stage: cannot copy %s\n", rel.generic_string().c_str());
                return 1;
            }
        }
    }
    std::ifstream spec(fixture / "gen_textures.txt");
    std::map<std::string, assets::AssetPackageWriter> packages;
    std::string line;
    while (std::getline(spec, line)) {
        std::istringstream ls(line);
        std::string kind;
        if (!(ls >> kind) || kind[0] == '#') {
            continue;
        }
        std::string path, asset, format;
        std::uint32_t w = 0, h = 0, seed = 0;
        if (kind == "dds9") {
            // RL-1.8's DX9 writer (A8R8G8B8): what a capture's textures/<H>.dds looks like.
            ls >> path >> format >> w >> h >> seed;
            ex::DdsImage img9;
            img9.format = hash::D3DFormat::A8R8G8B8;
            img9.width = w;
            img9.height = h;
            img9.mips.emplace_back(std::size_t(w) * h * 4);
            for (auto& b : img9.mips[0]) {
                seed = seed * 1664525u + 1013904223u;
                b = static_cast<std::uint8_t>(seed >> 24);
            }
            std::string err;
            const auto bytes = ex::writeDds(img9, &err);
            if (!ls || format != "a8r8g8b8" || bytes.empty() || !ex::writeFile(out / path, bytes, &err)) {
                std::fprintf(stderr, "stage: bad dds9 line: %s %s\n", line.c_str(), err.c_str());
                return 1;
            }
            std::printf("%s remix.tex %s\n", path.c_str(), hash::hashToString(hash::hashTextureMip0(img9.mips[0].data(), img9.mips[0].size())).c_str());
            continue;
        }
        if (kind == "dds") {
            ls >> path >> format >> w >> h >> seed;
        } else if (kind == "pkg") {
            ls >> path >> asset >> format >> w >> h >> seed;
        }
        const auto img = makeImage(format, w, h, seed);
        if (!ls || !img) {
            std::fprintf(stderr, "stage: bad gen_textures.txt line: %s\n", line.c_str());
            return 1;
        }
        std::string err;
        if (kind == "dds") {
            const auto bytes = assets::writeDds(*img, &err);
            if (bytes.empty() || !ex::writeFile(out / path, bytes, &err)) {
                std::fprintf(stderr, "stage: %s: %s\n", path.c_str(), err.c_str());
                return 1;
            }
        } else if (!packages[path].addImage(asset, *img, 0, 0, &err)) {
            std::fprintf(stderr, "stage: %s#%s: %s\n", path.c_str(), asset.c_str(), err.c_str());
            return 1;
        }
    }
    for (const auto& [path, writer] : packages) {
        std::string err;
        if (!ex::writeFile(out / path, writer.finish(), &err)) {
            std::fprintf(stderr, "stage: %s\n", err.c_str());
            return 1;
        }
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 4 && std::strcmp(argv[1], "stage") == 0) {
        return stage(argv[2], argv[3]);
    }
    testTables();
    testMaterialRoundTrip();
    testLights();
    testTransforms();
    testMesh();
    testMod();
    testCaptureImport();
    std::printf("rl_mods_import_unit: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
