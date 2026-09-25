// FUSE Relight RL-3.4: unit tests of the runtime replacement engine (ctest rl_replace_unit), and the fixture stager
// of the Wine goldens (rl_replace_golden.py).
//
//   fuse_relight_replace_tests [--fixtures <dir>]   the tests (below); temporary mods under ./rl_replace_tmp,
//                                                   removed at the end; with --fixtures, the golden fixture mods
//                                                   (Tests/relight/fixtures/replace) are staged and loaded too
//   fuse_relight_replace_tests --stage <fix> <out>  copies a fixture mod (Tests/relight/fixtures/replace/mods/<name>)
//                                                   to <out> without gen_textures.txt, and generates the DDS textures
//                                                   that file lists, so no binary is committed:
//                                                     dds <path> <width> <height> <seed>   RGBA8, full mip chain
//
// Tests: matrix helpers; stacking order (relight.modOrder, relight.mod.priority, FUSE-native over Remix, name);
// the stacked index and its diff; mods on disk (a Remix USD mod imported in memory, a FUSE-native imported store
// with several sources and a legacy asset rule) through the engine with synthetic draws: mesh replacement
// (hidden / preserveOriginalDrawCall / empty = removed), category overrides, parts and their materials (bound,
// the draw's mat_<H>, legacy), material replacement of kept draws, light replace / delete / attach (attached lights
// follow the instance transform), per-mod rtx.conf option layers (the strongest mod's value wins; released with the
// engine), rtx.enableReplacement* switches; hot reload with every available notification backend (a change is
// applied at the first frame boundary after the notification: latency <= 1 frame; mods added / removed; a broken
// write keeps the last good content; per-hash invalidation); the texture residency policy; and the capture tap
// integration (CaptureTap + CaptureReplaceProcessor on a synthetic D3D9 stream: the record's "replacement" members
// and "replace_frame" lines; the injection-time half, processPending before Present + previewLights, leaves the record
// identical to the flush alone).
#include <fuse/relight/capture/export/json.hpp>
#include <fuse/relight/hash/hash_string.hpp>
#include <fuse/relight/mods/assets/dds.hpp>
#include <fuse/relight/mods/assets/texture_format.hpp>
#include <fuse/relight/mods/import/mod_importer.hpp>
#include <fuse/relight/options/option_manager.hpp>
#include <fuse/relight/replace/file_watcher.hpp>
#include <fuse/relight/replace/mod_stack.hpp>
#include <fuse/relight/replace/replace_json.hpp>
#include <fuse/relight/replace/replace_live.hpp>
#include <fuse/relight/replace/replace_options.hpp>
#include <fuse/relight/replace/replacement_engine.hpp>
#include <fuse/relight/replace/texture_residency.hpp>
#include <fuse/relight/tap/capture_tap.hpp>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
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
using namespace fuse::relight;
using namespace fuse::relight::replace;
using capture::exporter::json::Value;

bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }

void writeText(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text;
}

/// RGBA8 texture with a full mip chain as DDS bytes.
std::vector<std::uint8_t> makeDds(std::uint32_t w, std::uint32_t h, std::uint32_t seed) {
    namespace as = mods::assets;
    as::TextureImage img;
    img.format = as::TexFormat::R8G8B8A8_UNORM;
    img.width = w;
    img.height = h;
    img.mipLevels = as::fullMipCount(w, h, 1);
    const std::uint64_t size = as::layoutSubresources(img);
    img.data.resize(size);
    std::uint32_t x = seed * 2654435761u + 1;
    for (std::uint8_t& b : img.data) {
        x = x * 1664525u + 1013904223u;
        b = std::uint8_t(x >> 24);
    }
    return as::writeDds(img);
}

void writeDds(const fs::path& p, std::uint32_t w, std::uint32_t h, std::uint32_t seed) {
    fs::create_directories(p.parent_path());
    const auto bytes = makeDds(w, h, seed);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
}

std::string H(Hash64 h) { return hash::hashToString(h); }

std::string fmt(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", v);
    return buf;
}

/// A Remix mod layer, written the way the Toolkit lays out replacements.
struct ModWriter {
    std::string meshes, looks, lights;

    static std::string quad(const std::string& name, const std::string& binding, double tx) {
        std::string s = "            def Mesh \"" + name + "\"";
        if (!binding.empty()) {
            s += " (\n                prepend apiSchemas = [\"MaterialBindingAPI\"]\n            )";
        }
        s += "\n            {\n"
             "                int[] faceVertexCounts = [4]\n"
             "                int[] faceVertexIndices = [0, 1, 2, 3]\n"
             "                point3f[] points = [(-1, -1, 0), (1, -1, 0), (1, 1, 0), (-1, 1, 0)]\n"
             "                uniform token subdivisionScheme = \"none\"\n";
        if (!binding.empty()) {
            s += "                rel material:binding = <" + binding + ">\n";
        }
        s += "                double3 xformOp:translate = (" + fmt(tx) + ", 0, 0)\n"
             "                uniform token[] xformOpOrder = [\"xformOp:translate\"]\n"
             "            }\n";
        return s;
    }

    /// mesh_<key>: `part` adds a quad (bound to `binding` when set) translated by `tx`; `light` an attached sphere
    /// light at (0, 2, 0); `category` a remix_category attribute set true.
    void mesh(Hash64 key, std::optional<bool> preserve, bool part, const std::string& binding = "", bool light = false,
              const std::string& category = "", double tx = 0.0) {
        meshes += "        def Xform \"mesh_" + H(key) + "\"\n        {\n";
        if (preserve) {
            meshes += std::string("            int preserveOriginalDrawCall = ") + (*preserve ? "1" : "0") + "\n";
        }
        if (!category.empty()) {
            meshes += "            custom uniform bool remix_category:" + category + " = 1\n";
        }
        if (part) {
            meshes += quad("part", binding, tx);
        }
        if (light) {
            meshes += "            def SphereLight \"attached\"\n            {\n"
                      "                float inputs:intensity = 50\n"
                      "                float inputs:radius = 0.1\n"
                      "                double3 xformOp:translate = (0, 2, 0)\n"
                      "                uniform token[] xformOpOrder = [\"xformOp:translate\"]\n"
                      "            }\n";
        }
        meshes += "        }\n";
    }

    void material(Hash64 tex, const std::string& dds, double roughness, bool preload = false) {
        const std::string name = "mat_" + H(tex);
        looks += "        def Material \"" + name + "\"\n        {\n"
                 "            token outputs:mdl:surface.connect = </RootNode/Looks/" + name + "/Shader.outputs:out>\n"
                 "            def Shader \"Shader\"\n            {\n"
                 "                uniform token info:implementationSource = \"sourceAsset\"\n"
                 "                uniform asset info:mdl:sourceAsset = @AperturePBR_Opacity.mdl@\n"
                 "                uniform token info:mdl:sourceAsset:subIdentifier = \"AperturePBR_Opacity\"\n";
        if (!dds.empty()) {
            looks += "                asset inputs:diffuse_texture = @" + dds + "@\n";
        }
        if (preload) {
            looks += "                bool inputs:preload_textures = 1\n";
        }
        looks += "                float inputs:reflection_roughness_constant = " + fmt(roughness) + "\n"
                 "                token outputs:out\n            }\n        }\n";
    }

    void light(Hash64 h, double intensity, double x, double y, double z) {
        lights += "        def SphereLight \"light_" + H(h) + "\"\n        {\n"
                  "            float inputs:intensity = " + fmt(intensity) + "\n"
                  "            float inputs:radius = 0.5\n"
                  "            color3f inputs:color = (1, 0.5, 0.25)\n"
                  "            double3 xformOp:translate = (" + fmt(x) + ", " + fmt(y) + ", " + fmt(z) + ")\n"
                  "            uniform token[] xformOpOrder = [\"xformOp:translate\"]\n        }\n";
    }

    void deleteLight(Hash64 h) { lights += "        def Xform \"light_" + H(h) + "\"\n        {\n        }\n"; }

    std::string text() const {
        return "#usda 1.0\n(\n    defaultPrim = \"RootNode\"\n    metersPerUnit = 1\n    upAxis = \"Y\"\n)\n\n"
               "def Xform \"RootNode\"\n{\n"
               "    def Scope \"meshes\"\n    {\n" + meshes + "    }\n"
               "    def Scope \"Looks\"\n    {\n" + looks + "    }\n"
               "    def Scope \"lights\"\n    {\n" + lights + "    }\n}\n";
    }

    void write(const fs::path& dir, const std::string& rtxConf = "") const {
        writeText(dir / "mod.usda", text());
        if (!rtxConf.empty()) {
            writeText(dir / "rtx.conf", rtxConf);
        }
    }
};

/// A draw whose default-rule asset hash is `key` (only the geometrydescriptor component set: the rule hash takes
/// the first selected non-zero component as-is).
DrawInput drawWithKey(std::uint32_t index, Hash64 key, Hash64 texture, const Mat4d& objectToWorld = identity4d(),
                      std::uint64_t instance = 1) {
    DrawInput d;
    d.index = index;
    d.geometryValid = true;
    d.hashes[hash::HashComponent::GeometryDescriptor] = key;
    d.materialHash = texture;
    d.objectToWorld = objectToWorld;
    d.instanceId = instance;
    return d;
}

Mat4d translation(double x, double y, double z) {
    Mat4d m = identity4d();
    m[12] = x;
    m[13] = y;
    m[14] = z;
    return m;
}

scene::LightRecord gameLight(Hash64 h, float x) {
    scene::LightRecord l;
    l.hash = h;
    l.type = hash::LightType::Sphere;
    l.position = {x, 1.f, 0.f};
    l.radiance = {1.f, 1.f, 1.f};
    l.intensity = 1.f;
    return l;
}

// ---- tests ---------------------------------------------------------------------------------------------------------

void testMath() {
    const Mat4d a = translation(1, 2, 3);
    Mat4d s = identity4d();
    s[0] = s[5] = s[10] = 2.0;
    const Mat4d m = multiply(s, a); // scale, then translate
    const Vec3d p = transformPoint(m, {1, 1, 1});
    CHECK(near(p[0], 3) && near(p[1], 4) && near(p[2], 5));
    const Vec3d d = transformDirection(s, {0, 0, -1});
    CHECK(near(d[2], -1) && near(d[0], 0));
    CHECK(parseModOrder(" a, b ;c,,") == (std::vector<std::string>{"a", "b", "c"}));
    const auto roots = parseModRootList("fuse:x/y, remix:z;w\\v/");
    CHECK(roots.size() == 3);
    CHECK(roots.size() == 3 && roots[0].kind == ModKind::FuseNative && roots[0].dir == "x/y");
    CHECK(roots.size() == 3 && roots[1].kind == ModKind::Remix && roots[1].dir == "z");
    CHECK(roots.size() == 3 && roots[2].kind == ModKind::Remix && roots[2].dir == "w/v");
    CHECK(modPriorityFromConf("# x\nrelight.mod.priority = 7\n") == std::optional<std::int64_t>(7));
    CHECK(modPriorityFromConf("rtx.mod.priority = -3\n") == std::optional<std::int64_t>(-3));
    CHECK(!modPriorityFromConf("relight.mod.priority = high\n"));
}

ModContent content(const std::string& name, ModKind kind, std::optional<std::int64_t> priority) {
    ModContent c;
    c.ok = true;
    c.location.name = name;
    c.location.dir = "/mods/" + name;
    c.location.kind = kind;
    c.priority = priority;
    return c;
}

void testStackAndIndex() {
    ModContent a = content("a", ModKind::Remix, std::nullopt);
    ModContent b = content("b", ModKind::Remix, std::nullopt);
    ModContent f = content("z_fuse", ModKind::FuseNative, std::nullopt);
    ModContent p = content("p", ModKind::Remix, 3);
    std::vector<const ModContent*> stack{&a, &b, &f, &p};
    sortStack(stack, {});
    CHECK(stack[0] == &p && stack[1] == &f && stack[2] == &a && stack[3] == &b); // priority, kind, name
    sortStack(stack, {"b", "a"});
    CHECK(stack[0] == &b && stack[1] == &a && stack[2] == &p && stack[3] == &f);

    // Index: the strongest mod wins a key whole; the others are shadowed. Diff: per hash.
    const hash::HashRule rule = hash::parseHashRule(hash::rules::kDefaultAssetRuleString);
    auto mesh = [&](ModContent& c, Hash64 key, Hash64 fp) {
        MeshReplacementDef m;
        m.key = key;
        m.rule = rule;
        m.algo = "remix.geom.asset";
        m.recordId = c.location.name + "/mesh";
        m.fingerprint = fp;
        c.meshes[{rule.bits, key}] = m;
    };
    auto mat = [&](ModContent& c, Hash64 tex, Hash64 fp) {
        MaterialDef m;
        m.textureHash = tex;
        m.recordId = c.location.name + "/mat";
        m.fingerprint = fp;
        c.materials[tex] = m;
    };
    mesh(a, 0x10, 1);
    mesh(b, 0x10, 2);
    mesh(b, 0x20, 3);
    mat(a, 0x30, 4);
    mat(f, 0x30, 5);
    ModContent broken = content("broken", ModKind::Remix, 99);
    broken.ok = false;
    mesh(broken, 0x10, 6);
    std::vector<const ModContent*> s1{&a, &b, &f, &broken};
    sortStack(s1, {});
    ReplacementIndex i1;
    i1.build(s1);
    const auto* h10 = i1.mesh(rule, 0x10);
    CHECK(h10 && h10->mod == &a && h10->shadowed.size() == 1 && h10->shadowed[0] == &b);
    CHECK(i1.mesh(rule, 0x20) && i1.mesh(rule, 0x20)->mod == &b);
    CHECK(i1.material(0x30) && i1.material(0x30)->mod == &f);
    CHECK(!i1.mesh(hash::rules::kLegacyAsset0, 0x10));
    CHECK(i1.meshRules().size() == 1);
    std::vector<const ModContent*> s2{&a, &b, &f};
    sortStack(s2, {"b"});
    ReplacementIndex i2;
    i2.build(s2);
    CHECK(i2.mesh(rule, 0x10)->mod == &b);
    const auto d = ReplacementIndex::diff(i1, i2);
    CHECK(d.meshes == std::vector<Hash64>{0x10});
    CHECK(d.materials.empty() && d.lights.empty());
    CHECK(ReplacementIndex::diff(i2, i2).empty());
}

// ---- the mods on disk ------------------------------------------------------------------------------------------------

constexpr Hash64 kK1 = 0x1111111111111111ull, kK2 = 0x2222222222222222ull, kK3 = 0x3333333333333333ull,
                 kK4 = 0x4444444444444444ull, kK5 = 0x5555555555555555ull;
constexpr Hash64 kT1 = 0xA1A1A1A1A1A1A1A1ull, kT2 = 0xB2B2B2B2B2B2B2B2ull;
constexpr Hash64 kL1 = 0x0101010101010101ull, kL2 = 0x0202020202020202ull, kL3 = 0x0303030303030303ull;

/// Legacy key inputs of the draw the FUSE-native store mod replaces under legacypositions0,legacyindices.
hash::DrawGeometryHashes legacyDraw() {
    hash::DrawGeometryHashes g;
    g.hashes[hash::HashComponent::LegacyPositions0] = 0x7777;
    g.hashes[hash::HashComponent::LegacyIndices] = 0x8888;
    g.indexCount = 6;
    g.vertexCount = 4;
    g.topology = 3;
    g.positionStride = 20;
    g.indexType = 0;
    return g;
}

struct Fixture {
    fs::path root, remix, fuse;
    Hash64 legacyKey = 0;

    explicit Fixture(const fs::path& r) : root(r), remix(r / "rtx-remix" / "mods"), fuse(r / "fuse-relight" / "mods") {
        fs::remove_all(root);
        // alpha (Remix): K1 hidden with a part bound to its own material, K2 preserved with an attached light and a
        // category, K3 removed, K4 (shadowed by delta), mat T1, light L1 replaced, L2 deleted.
        ModWriter alpha;
        alpha.mesh(kK1, std::nullopt, true, "/RootNode/Looks/mat_" + H(kT2), false, "", 1.0);
        alpha.mesh(kK2, true, true, "", true, "ignore");
        alpha.mesh(kK3, std::nullopt, false);
        alpha.mesh(kK4, false, true);
        alpha.material(kT1, "./textures/t1.dds", 0.25);
        alpha.material(kT2, "./textures/t2.dds", 0.5, true);
        alpha.light(kL1, 100, 1, 2, 3);
        alpha.deleteLight(kL2);
        alpha.write(remix / "alpha");
        writeDds(remix / "alpha" / "textures" / "t1.dds", 16, 16, 1);
        writeDds(remix / "alpha" / "textures" / "t2.dds", 8, 8, 2);
        // delta (Remix, relight.mod.priority 5): K4, mat T1; its rtx.conf turns light replacements on.
        ModWriter delta;
        delta.mesh(kK4, std::nullopt, true);
        delta.material(kT1, "", 0.9);
        delta.write(remix / "delta", "relight.mod.priority = 5\nrtx.enableReplacementLights = True\n");
        // epsilon (Remix): K1 (shadowed by alpha), mat T1, L1; its rtx.conf turns light replacements off.
        ModWriter epsilon;
        epsilon.mesh(kK1, true, true);
        epsilon.material(kT1, "", 0.1);
        epsilon.light(kL1, 7, 0, 0, 0);
        epsilon.write(remix / "epsilon", "rtx.enableReplacementLights = False\n");
        // beta / beta2 (one FUSE-native imported store, two sources): beta has mat T1 (below delta's priority, above
        // the Remix mods) and a legacy-rule mesh; beta2 K5 and mat T2 (above alpha's).
        legacyKey = hash::meshReplacementHashLegacy(legacyDraw(), hash::rules::kLegacyAsset0, 0);
        ModWriter beta;
        beta.material(kT1, "", 0.6);
        beta.mesh(legacyKey, std::nullopt, true);
        const fs::path betaSrc = root / "src" / "beta";
        beta.write(betaSrc, "rtx.geometryAssetHashRuleString = legacypositions0,legacyindices\n");
        ModWriter beta2;
        beta2.mesh(kK5, std::nullopt, true);
        beta2.material(kT2, "", 0.7);
        const fs::path beta2Src = root / "src" / "beta2";
        beta2.write(beta2Src);
        for (const fs::path& src : {betaSrc, beta2Src}) {
            mods::import::ImportOptions o;
            o.root = src.generic_string();
            o.gameId = "unit";
            const auto r = mods::import::importMod(o);
            CHECK(r.ok);
            std::string err;
            CHECK(mods::import::writeStore(fuse / "store", r, true, &err));
        }
    }

    EngineConfig config() const {
        EngineConfig c;
        c.roots = {{fuse.generic_string(), ModKind::FuseNative}, {remix.generic_string(), ModKind::Remix}};
        c.gameId = "unit";
        c.watchBackend = WatchBackend::Poll;
        c.pollIntervalMs = 0;
        return c;
    }
};

const ModInfo* modByName(const ReplacementEngine& e, const std::string& name) {
    for (const ModInfo& m : e.mods()) {
        if (m.location.name == name) {
            return &m;
        }
    }
    return nullptr;
}

void testEngine(const fs::path& tmp) {
    Fixture fx(tmp / "engine");
    {
        const auto locs = discoverMods(fx.config().roots, {});
        CHECK(locs.size() == 5); // beta, beta2 (store sources), alpha, delta, epsilon
        ReplacementEngine e(fx.config());
        e.beginFrame(0);
        CHECK(e.generation() == 1);
        CHECK(e.diagnostics().empty());
        for (const std::string& d : e.diagnostics()) {
            std::fprintf(stderr, "  diagnostic: %s\n", d.c_str());
        }
        // Stack: delta (relight.mod.priority 5), beta, beta2 (FUSE-native, by name), alpha, epsilon.
        std::vector<std::string> order;
        for (const ModInfo& m : e.mods()) {
            order.push_back(m.location.name);
        }
        CHECK(order == (std::vector<std::string>{"delta", "beta", "beta2", "alpha", "epsilon"}));
        const ModInfo* alpha = modByName(e, "alpha");
        CHECK(alpha && alpha->ok && alpha->meshes == 4 && alpha->materials == 2 && alpha->lights == 1 && alpha->deletedLights == 1);
        const ModInfo* beta = modByName(e, "beta");
        CHECK(beta && beta->location.format == ModFormat::Store && beta->location.kind == ModKind::FuseNative);
        // Per-mod rtx.conf layers: delta (stronger) sets lights on over epsilon's off.
        CHECK(modByName(e, "delta") && modByName(e, "delta")->optionLayer == "Mod Remix Config 000 delta");
        CHECK(modByName(e, "epsilon") && modByName(e, "epsilon")->optionLayer == "Mod Remix Config 004 epsilon");
        CHECK(ReplaceOptions::enableReplacementLights());
        CHECK(options::OptionManager::getLayer(options::OptionLayerKey(4, "Mod Remix Config 004 epsilon")) != nullptr);

        const Mat4d world = translation(10, 0, 0);
        // K1: alpha wins (name) over epsilon; hidden; part bound to alpha's mat_T2.
        ReplacedDraw d1 = e.replaceDraw(drawWithKey(0, kK1, kT1, world, 11));
        CHECK(d1.meshReplaced && d1.meshMod == "alpha" && !d1.drawOriginal);
        CHECK(d1.meshShadowed == std::vector<std::string>{"epsilon"});
        CHECK(d1.parts.size() == 1 && d1.parts[0].materialSource == "bound" && d1.parts[0].materialMod == "alpha");
        CHECK(d1.parts.size() == 1 && d1.parts[0].material.find("mat_" + H(kT2)) != std::string::npos);
        CHECK(d1.parts.size() == 1 && near(d1.parts[0].objectToWorld[12], 11.0)); // part at x=1, instance at x=10
        CHECK(!d1.materialReplaced); // the original is hidden
        // K2: preserved; the kept original gets the strongest mat_T1 (delta); the unbound part too; attached light.
        ReplacedDraw d2 = e.replaceDraw(drawWithKey(1, kK2, kT1, world, 12));
        CHECK(d2.meshReplaced && d2.drawOriginal);
        CHECK(d2.materialReplaced && d2.materialMod == "delta");
        CHECK(d2.materialShadowed == (std::vector<std::string>{"beta", "alpha", "epsilon"}));
        CHECK(d2.parts.size() == 1 && d2.parts[0].materialSource == "replacement" && d2.parts[0].materialMod == "delta");
        CHECK(d2.categories.test(scene::InstanceCategories::Ignore) && d2.categoriesChanged);
        CHECK(d2.lights.size() == 1 && near(d2.lights[0].position[0], 10) && near(d2.lights[0].position[1], 2));
        CHECK(d2.lights.size() == 1 && d2.lights[0].instanceId == 12 && d2.lights[0].origin == ReplacedLight::Origin::Attached);
        // K3: an empty replacement removes the draw.
        ReplacedDraw d3 = e.replaceDraw(drawWithKey(2, kK3, 0));
        CHECK(d3.meshReplaced && !d3.drawOriginal && d3.parts.empty());
        // K4: delta (priority 5) over alpha.
        ReplacedDraw d4 = e.replaceDraw(drawWithKey(3, kK4, 0));
        CHECK(d4.meshReplaced && d4.meshMod == "delta" && d4.meshShadowed == std::vector<std::string>{"alpha"});
        // K5: the second store source.
        CHECK(e.replaceDraw(drawWithKey(4, kK5, 0)).meshMod == "beta2");
        // Legacy key (beta's rule).
        DrawInput legacy = drawWithKey(5, 0x9999, 0);
        legacy.legacyHashes = [] { return legacyDraw(); };
        ReplacedDraw d5 = e.replaceDraw(legacy);
        CHECK(d5.meshReplaced && d5.meshMod == "beta" && d5.meshRule == "legacypositions0,legacyindices");
        // Untextured, unknown key: untouched.
        ReplacedDraw d6 = e.replaceDraw(drawWithKey(6, 0x42, 0));
        CHECK(!d6.affected() && d6.drawOriginal && replacedDrawJson(d6).isNull());
        // A textured draw without a mesh replacement: material only; beta2 (FUSE-native) over alpha (Remix).
        ReplacedDraw d7 = e.replaceDraw(drawWithKey(7, 0x43, kT2));
        CHECK(!d7.meshReplaced && d7.materialReplaced && d7.materialMod == "beta2");
        CHECK(d7.materialShadowed == std::vector<std::string>{"alpha"});

        ReplacedFrame f = e.endFrame({gameLight(kL1, 1.f), gameLight(kL2, 2.f), gameLight(kL3, 3.f)});
        CHECK(f.reload && f.reload->generation == 1 && f.reload->added.size() == 5);
        CHECK(f.deletedLights == std::vector<Hash64>{kL2});
        CHECK(f.lights.size() == 3); // L1 replaced, L3 kept, one attached
        CHECK(f.lights.size() == 3 && f.lights[0].origin == ReplacedLight::Origin::Replaced && f.lights[0].mod == "alpha");
        CHECK(f.lights.size() == 3 && near(f.lights[0].position[2], 3) && near(f.lights[0].intensity, 100));
        CHECK(f.lights.size() == 3 && f.lights[1].origin == ReplacedLight::Origin::Game && f.lights[1].gameHash == kL3);
        CHECK(f.lights.size() == 3 && f.lights[2].origin == ReplacedLight::Origin::Attached);
        CHECK(f.stats.draws == 8 && f.stats.meshReplaced == 6 && f.stats.hidden == 5 && f.stats.preserved == 1);
        CHECK(f.stats.materialReplaced == 2 && f.stats.partMaterialReplaced == 1);
        CHECK(f.stats.lightsReplaced == 1 && f.stats.lightsDeleted == 1 && f.stats.lightsGame == 1 && f.stats.lightsAttached == 1);
        // Residency: t1 unused (delta's mat_T1 wins and has no texture); t2 (K1's bound material; preloaded).
        CHECK(f.residency.resident == 1 && f.residency.loaded == 1);
        const Value fj = replacedFrameJson(f, e.mods(), e.watchBackend());
        CHECK(fj.get("reload") && fj.get("reload")->num("latency_frames", -1) == 0);
        CHECK(fj.get("mods") && fj.get("mods")->a.size() == 5);

        // The switches (a user layer above the mods'): materials off -> no material replacement at all.
        {
            const options::OptionConfig conf = options::OptionConfig::parse("rtx.enableReplacementMaterials = False\n");
            options::OptionLayerHandle layer =
                options::OptionManager::acquireLayer("", {5000u, "rl_replace_test"}, 1.0f, 0.1f, false, &conf);
            options::OptionManager::applyPendingValues(nullptr, false);
            CHECK(!ReplaceOptions::enableReplacementMaterials());
            e.beginFrame(1);
            CHECK(!e.replaceDraw(drawWithKey(0, 0x43, kT2)).materialReplaced);
            CHECK(e.replaceDraw(drawWithKey(1, kK2, kT1)).parts[0].materialSource == "legacy");
            e.endFrame({});
        }
        options::OptionManager::applyPendingValues(nullptr, false);
        CHECK(ReplaceOptions::enableReplacementMaterials());
    }
    // Released with the engine: the layers leave the options.
    CHECK(options::OptionManager::getLayer(options::OptionLayerKey(4, "Mod Remix Config 004 epsilon")) == nullptr);
    CHECK(ReplaceOptions::enableReplacementLights());
    {
        // relight.modOrder: epsilon first -> it wins K1, mat T1, L1; its rtx.conf turns light replacements off.
        EngineConfig c = fx.config();
        c.modOrder = {"epsilon"};
        ReplacementEngine e(c);
        e.beginFrame(0);
        CHECK(!e.mods().empty() && e.mods()[0].location.name == "epsilon");
        CHECK(!ReplaceOptions::enableReplacementLights());
        ReplacedDraw d1 = e.replaceDraw(drawWithKey(0, kK1, kT1));
        CHECK(d1.meshMod == "epsilon" && d1.drawOriginal && d1.materialMod == "epsilon");
        ReplacedDraw d2 = e.replaceDraw(drawWithKey(1, kK2, 0));
        CHECK(d2.lights.empty()); // attached lights are light replacements too
        ReplacedFrame f = e.endFrame({gameLight(kL1, 1.f), gameLight(kL2, 2.f)});
        CHECK(f.deletedLights.empty() && f.lights.size() == 2 && f.stats.lightsReplaced == 0);
    }
    CHECK(ReplaceOptions::enableReplacementLights());
    fs::remove_all(fx.root);
}

void testHotReload(const fs::path& tmp, WatchBackend backend, const char* expectBackend) {
    const fs::path root = tmp / (std::string("hot_") + watchBackendName(backend));
    fs::remove_all(root);
    const fs::path mods = root / "rtx-remix" / "mods";
    ModWriter v1;
    v1.material(kT1, "", 0.1);
    v1.mesh(kK1, std::nullopt, true);
    v1.write(mods / "hot");
    EngineConfig c;
    c.roots = {{mods.generic_string(), ModKind::Remix}};
    c.gameId = "unit";
    c.watchBackend = backend;
    c.pollIntervalMs = 0;
    c.optionLayers = false;
    ReplacementEngine e(c);
    e.beginFrame(0);
    CHECK(std::string(e.watchBackend()) == expectBackend);
    CHECK(e.replaceDraw(drawWithKey(0, kK1, kT1)).meshReplaced);
    e.endFrame({});
    e.beginFrame(1);
    ReplacedFrame f1 = e.endFrame({});
    CHECK(!f1.reload && e.generation() == 1);

    // Edit: K1 now preserved and K2 added; the change is applied at the next frame boundary.
    ModWriter v2;
    v2.material(kT1, "", 0.2);
    v2.mesh(kK1, true, true);
    v2.mesh(kK2, std::nullopt, true);
    v2.write(mods / "hot");
    e.beginFrame(2);
    ReplacedDraw d = e.replaceDraw(drawWithKey(0, kK1, kT1));
    CHECK(d.meshReplaced && d.drawOriginal);
    CHECK(e.replaceDraw(drawWithKey(1, kK2, 0)).meshReplaced);
    ReplacedFrame f2 = e.endFrame({});
    CHECK(f2.reload.has_value());
    if (f2.reload) {
        CHECK(f2.reload->notifiedFrame == 2 && f2.reload->appliedFrame == 2);
        CHECK(f2.reload->appliedFrame - f2.reload->notifiedFrame <= 1);
        CHECK(f2.reload->changed == std::vector<std::string>{"hot"});
        CHECK(f2.reload->invalidated.meshes == (std::vector<Hash64>{kK1, kK2}));
        CHECK(f2.reload->invalidated.materials == std::vector<Hash64>{kT1});
    }
    CHECK(e.generation() == 2);

    // A broken write keeps the last good content.
    writeText(mods / "hot" / "mod.usda", "#usda 1.0\n(\n def Xform \"RootNode\" {\n");
    e.beginFrame(3);
    CHECK(e.replaceDraw(drawWithKey(1, kK2, 0)).meshReplaced);
    ReplacedFrame f3 = e.endFrame({});
    CHECK(f3.reload && f3.reload->failed == std::vector<std::string>{"hot"});
    v2.write(mods / "hot"); // repaired
    e.beginFrame(4);
    e.endFrame({});

    // A new mod appears in the search root (staged elsewhere, then renamed in: one notification).
    ModWriter added;
    added.mesh(kK3, std::nullopt, false);
    added.write(root / "staging" / "late");
    fs::rename(root / "staging" / "late", mods / "late");
    e.beginFrame(5);
    CHECK(e.replaceDraw(drawWithKey(0, kK3, 0)).meshReplaced);
    ReplacedFrame f5 = e.endFrame({});
    CHECK(f5.reload && f5.reload->added == std::vector<std::string>{"late"} && f5.reload->notifiedFrame == 5);
    CHECK(f5.reload && f5.reload->invalidated.meshes == std::vector<Hash64>{kK3});
    // ... and goes away.
    fs::remove_all(mods / "late");
    e.beginFrame(6);
    CHECK(!e.replaceDraw(drawWithKey(0, kK3, 0)).meshReplaced);
    ReplacedFrame f6 = e.endFrame({});
    CHECK(f6.reload && f6.reload->removed == std::vector<std::string>{"late"});
    CHECK(e.mods().size() == 1);
    fs::remove_all(root);
}

void testWatcher(const fs::path& tmp) {
    // The poll backend honours its interval unless forced; missing directories are fine.
    const fs::path dir = tmp / "watch";
    fs::remove_all(dir);
    fs::create_directories(dir / "a");
    DirectoryWatcher w({{(dir / "a").generic_string(), true}, {(dir / "missing").generic_string(), true}}, WatchBackend::Poll, 60000);
    CHECK(std::string(w.backend()) == "poll");
    writeText(dir / "a" / "x.usda", "1");
    CHECK(w.poll(false).empty()); // within the interval
    CHECK(w.poll(true) == std::vector<std::size_t>{0});
    CHECK(w.poll(true).empty());
    fs::create_directories(dir / "missing");
    CHECK(w.poll(true) == std::vector<std::size_t>{1});
    fs::remove_all(dir);
    CHECK(parseWatchBackend("poll") == WatchBackend::Poll && parseWatchBackend("x") == WatchBackend::Auto);
}

void testResidency() {
    TextureInfo a{"aa", "R8G8B8A8_UNORM", 64, 64, 7, 0, false};
    TextureInfo b{"bb", "R8G8B8A8_UNORM", 64, 64, 7, 0, false};
    TextureInfo c{"cc", "R8G8B8A8_UNORM", 64, 64, 7, 0, true};
    const std::uint64_t full = textureBytes(a, 0);
    CHECK(full == 4 * (64 * 64 + 32 * 32 + 16 * 16 + 8 * 8 + 4 * 4 + 2 * 2 + 1));
    CHECK(textureBytes(a, 1) == full - 4 * 64 * 64);
    TextureInfo bc7{"x", "BC7_UNORM_BLOCK", 16, 16, 5, 0, false};
    CHECK(textureBytes(bc7, 0) == 256 + 64 + 16 + 16 + 16);
    TextureInfo unknown{"y", "", 0, 0, 3, 128 + 1024, false};
    CHECK(textureBytes(unknown, 0) == 1024 && textureBytes(unknown, 1) == 256);

    TextureResidency r({0, 0, false});
    r.setCatalog({{"aa", a}, {"bb", b}, {"cc", c}});
    ResidencyStats s = r.update(1, {"aa"});
    CHECK(s.requested == 2 && s.resident == 2 && s.loaded == 2 && s.evicted == 0); // aa + preloaded cc
    s = r.update(2, {"aa"});
    CHECK(s.loaded == 0 && s.resident == 2);

    // Budget for two full textures: bb in -> the least recently used unrequested one is evicted first.
    TextureResidency lru({2 * full, 0, false});
    a.preload = b.preload = c.preload = false;
    lru.setCatalog({{"aa", a}, {"bb", b}, {"cc", c}});
    lru.update(1, {"aa"});
    lru.update(2, {"bb"});
    s = lru.update(3, {"cc"});
    CHECK(s.resident == 2 && s.evicted == 1 && lru.resident().count("bb") && lru.resident().count("cc"));
    CHECK(s.residentBytes == 2 * full && !s.overBudget);

    // Pressure: three requested in a budget of two -> demotion, largest first (ties: sha order).
    s = lru.update(4, {"aa", "bb", "cc"});
    CHECK(s.demoted >= 1 && !s.overBudget && s.residentBytes <= 2 * full);
    CHECK(lru.resident().at("aa").minMip >= 1);
    // A budget below even the last mips: over budget, everything at its last mip.
    TextureResidency tiny({8, 0, false});
    tiny.setCatalog({{"aa", a}, {"bb", b}, {"cc", c}});
    s = tiny.update(1, {"aa", "bb", "cc"});
    CHECK(s.overBudget && tiny.resident().at("aa").minMip == 6);

    // Mip bias and catalog removal.
    TextureResidency bias({0, 2, false});
    bias.setCatalog({{"aa", a}});
    s = bias.update(1, {"aa"});
    CHECK(s.residentBytes == textureBytes(a, 2) && bias.resident().at("aa").minMip == 2);
    bias.setCatalog({});
    s = bias.update(2, {});
    CHECK(s.evicted == 1 && s.resident == 0);
    // Preload everything.
    TextureResidency all({0, 0, true});
    all.setCatalog({{"aa", a}, {"bb", b}});
    CHECK(all.update(1, {}).resident == 2);
}

// ---- capture tap integration -----------------------------------------------------------------------------------

using namespace fuse::relight::tap;

constexpr std::uint32_t kFmtA8R8G8B8 = 21, kFmtIndex16 = 101;
constexpr std::uint32_t kPoolDefault = 0, kPoolManaged = 1;
constexpr std::uint32_t kTypeSurface = 1, kTypeTexture = 3;

/// A minimal D3D9 stream: one textured indexed quad per frame, `frames` frames, the quad moving along x.
struct App {
    std::uint32_t renderStates[kRenderStateCount] = {};
    std::uint32_t tss[kTextureStageCount][32] = {};
    std::uint32_t samplers[kSamplerSlotCount][kSamplerStateCount] = {};
    float transforms[kTransformCount][16] = {};
    std::vector<std::uint8_t> vb, ib, tex;

    App() {
        renderStates[7] = 1;
        renderStates[14] = 1;
        renderStates[168] = 0xF;
        for (std::uint32_t s = 0; s < kTextureStageCount; ++s) {
            tss[s][0] = s == 0 ? 4u : 1u;
            tss[s][1] = 2;
            tss[s][2] = 1;
            tss[s][3] = s == 0 ? 2u : 1u;
            tss[s][4] = 2;
            tss[s][5] = 1;
            tss[s][10] = s;
        }
        for (auto& m : transforms) {
            for (int k = 0; k < 16; ++k) {
                m[k] = (k % 5 == 0) ? 1.0f : 0.0f;
            }
        }
        const float proj[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 1, 0, 0, -0.1f, 0};
        std::memcpy(transforms[kTransformProjection], proj, sizeof proj);
        const float v[4][5] = {{-1, -1, 1, 0, 1}, {-1, 1, 1, 0, 0}, {1, -1, 1, 1, 1}, {1, 1, 1, 1, 0}};
        vb.assign(reinterpret_cast<const std::uint8_t*>(v), reinterpret_cast<const std::uint8_t*>(v) + sizeof v);
        const std::uint16_t idx[6] = {0, 1, 2, 2, 1, 3};
        ib.assign(reinterpret_cast<const std::uint8_t*>(idx), reinterpret_cast<const std::uint8_t*>(idx) + 12);
        tex.resize(4 * 4 * 4);
        for (std::size_t i = 0; i < tex.size(); ++i) {
            tex[i] = std::uint8_t(i * 29 + 3);
        }
    }

    void run(IRelightTap& t, int frames) {
        DeviceEvent dev;
        dev.present.backBufferWidth = dev.present.backBufferHeight = 64;
        dev.present.backBufferFormat = kFmtA8R8G8B8;
        dev.backBuffer = 1;
        TextureDesc bb;
        bb.id = 1;
        bb.type = kTypeSurface;
        bb.width = bb.height = 64;
        bb.depth = bb.mipLevels = bb.arraySize = 1;
        bb.format = kFmtA8R8G8B8;
        bb.usage = 1;
        bb.pool = kPoolDefault;
        bb.isBackBuffer = true;
        bb.vkImage = 0x1001;
        t.onTextureCreate(bb);
        t.onDeviceCreate(dev);
        TextureDesc td = bb;
        td.id = 2;
        td.type = kTypeTexture;
        td.width = td.height = 4;
        td.usage = 0;
        td.pool = kPoolManaged;
        td.isBackBuffer = false;
        td.vkImage = 0x1002;
        t.onTextureCreate(td);
        t.onTextureWriteLock(TextureWriteLock{2, 0, 0, 0});
        TextureUpload u;
        u.texture = 2;
        u.width = u.height = 4;
        u.depth = 1;
        u.data = tex.data();
        u.rowPitch = 16;
        u.slicePitch = 64;
        u.rows = 4;
        u.fullUpdate = true;
        u.box = Box{0, 0, 4, 4, 0, 1};
        t.onTextureUpload(u);
        t.onBufferCreate(BufferDesc{1, BufferKind::Vertex, 80, 0, kPoolDefault, 0x102, 0});
        t.onBufferCreate(BufferDesc{2, BufferKind::Index, 12, 0, kPoolDefault, 0, kFmtIndex16});
        for (const auto& [id, data] : {std::pair<ResourceId, const std::vector<std::uint8_t>*>{1, &vb}, {2, &ib}}) {
            BufferWrite w;
            w.buffer = id;
            w.size = std::uint32_t(data->size());
            w.data = data->data();
            w.base = data->data();
            w.bufferSize = std::uint32_t(data->size());
            t.onBufferWrite(w);
        }
        for (int f = 0; f < frames; ++f) {
            transforms[kTransformWorld0][12] = float(f); // world translation x = frame
            DrawCall c;
            c.call = DrawCallType::DrawIndexedPrimitive;
            c.primitiveType = 4;
            c.primitiveCount = 2;
            c.numVertices = 4;
            c.indexCount = 6;
            DrawState s;
            s.renderStates = renderStates;
            s.textureStageStates = tss;
            s.samplerStates = samplers;
            s.transforms = transforms;
            s.textures[0] = 2;
            s.elements[0] = VertexElement{0, 0, 2, 0, 0, 0};
            s.elements[1] = VertexElement{0, 12, 1, 0, 5, 0};
            s.elementCount = 2;
            s.fvf = 0x102;
            s.viewport = Viewport{0, 0, 64, 64, 0, 1};
            s.renderTargets[0] = 1;
            s.streams[0] = StreamBinding{1, 0, 20, 1, vb.data(), 80};
            s.indices = IndexBinding{2, kFmtIndex16, ib.data(), 12};
            CHECK(t.onDraw(c, s) == DrawDecision::Raster);
            t.onPresent(FrameEvent{std::uint64_t(f), 1, 0x1001, 64, 64, kFmtA8R8G8B8});
        }
        t.onDeviceDestroy();
    }
};

std::vector<Value> readJsonl(const fs::path& p) {
    std::vector<Value> out;
    std::ifstream in(p);
    for (std::string l; std::getline(in, l);) {
        if (auto v = capture::exporter::json::parse(l)) {
            out.push_back(std::move(*v));
        }
    }
    return out;
}

void testCaptureTap(const fs::path& tmp) {
    const fs::path root = tmp / "tap";
    fs::remove_all(root);
    fs::create_directories(root);
    // 1. The draw's keys, from a plain capture.
    Hash64 key = 0, texture = 0;
    bool committed = false;
    {
        std::atomic<std::uint32_t> counter{0};
        CaptureTapConfig config;
        config.texture.renderTargetCounter = &counter;
        CaptureTap tap(std::move(config));
        tap.setFrameSink([&](std::uint64_t, const std::vector<CaptureDrawRecord>& draws) {
            for (const CaptureDrawRecord& r : draws) {
                if (r.geometry && r.geometry->captured()) {
                    key = r.geometry->assetHash(hash::parseHashRule(hash::rules::kDefaultAssetRuleString));
                    texture = r.translation.material.hash();
                    committed = r.translated && r.classification.committed();
                }
            }
        });
        App app;
        app.run(tap, 1);
    }
    CHECK(key != 0 && texture != 0 && committed);
    // 2. A mod for it: the quad preserved (+ a part and an attached light), its texture's material replaced.
    ModWriter w;
    w.mesh(key, true, true, "", true, "", 0.5);
    w.material(texture, "./textures/a.dds", 0.3);
    const fs::path mods = root / "rtx-remix" / "mods";
    w.write(mods / "tapmod");
    writeDds(mods / "tapmod" / "textures" / "a.dds", 8, 8, 5);
    // 3. The capture with the processor: the record carries the replaced scene.
    const fs::path record = root / "record.jsonl";
    {
        std::atomic<std::uint32_t> counter{0};
        EngineConfig ec;
        ec.roots = {{mods.generic_string(), ModKind::Remix}};
        ec.gameId = "unit";
        ec.watchBackend = WatchBackend::Poll;
        ec.optionLayers = false;
        CaptureTapConfig config;
        config.path = record.string();
        config.texture.renderTargetCounter = &counter;
        config.processor = std::make_unique<CaptureReplaceProcessor>(ec, hash::parseHashRule(hash::rules::kDefaultAssetRuleString));
        CaptureTap tap(std::move(config));
        CHECK(tap.frameProcessor() != nullptr);
        App app;
        app.run(tap, 3);
        auto* p = static_cast<CaptureReplaceProcessor*>(tap.frameProcessor());
        CHECK(p->lastFrame().frame == 2 && p->lastDraws().size() == 1);
    }
    const auto lines = readJsonl(record);
    int draws = 0, frames = 0;
    for (const Value& l : lines) {
        if (l.str("ev") == "draw") {
            ++draws;
            const Value* r = l.get("replacement");
            CHECK(r && r->isObject());
            if (!r || !r->isObject()) {
                continue;
            }
            const Value* mesh = r->get("mesh");
            CHECK(mesh && mesh->str("mod") == "tapmod" && mesh->str("key") == H(key) && mesh->flag("preserve"));
            CHECK(r->get("material") && r->get("material")->str("mod") == "tapmod");
            CHECK(r->get("lights") && r->get("lights")->a.size() == 1);
            // The attached light follows the instance: world x = frame (the part at +0.5 does not move it).
            const double frame = l.num("frame");
            const Value& light = r->get("lights")->a[0];
            CHECK(light.get("position") && near(light.get("position")->a[0].n, frame) && near(light.get("position")->a[1].n, 2));
            CHECK(mesh && mesh->get("parts") && near(mesh->get("parts")->a[0].get("translation")->a[0].n, frame + 0.5));
        } else if (l.str("ev") == "replace_frame") {
            ++frames;
            const Value* st = l.get("stats");
            CHECK(st && st->num("mesh_replaced") == 1 && st->num("preserved") == 1 && st->num("material_replaced") == 1);
            CHECK(st && st->num("lights_attached") == 1);
            CHECK(l.get("reload") && (l.num("frame") == 0) == !l.get("reload")->isNull());
            CHECK(l.get("residency") && l.get("residency")->num("resident") == 1);
        }
    }
    CHECK(draws == 3 && frames == 3);
    // 4. The injection-time half (RL-4.x RenderTap): processPending on the frame's draws before Present, then the
    //    flush continues after them. The record is identical to the flush alone; the preview has the attached light.
    const fs::path record2 = root / "record_pending.jsonl";
    std::size_t pendingCalls = 0, previewLights = 0;
    {
        std::atomic<std::uint32_t> counter{0};
        EngineConfig ec;
        ec.roots = {{mods.generic_string(), ModKind::Remix}};
        ec.gameId = "unit";
        ec.watchBackend = WatchBackend::Poll;
        ec.optionLayers = false;
        CaptureTapConfig config;
        config.path = record2.string();
        config.texture.renderTargetCounter = &counter;
        config.processor = std::make_unique<CaptureReplaceProcessor>(ec, hash::parseHashRule(hash::rules::kDefaultAssetRuleString));
        CaptureTap tap(std::move(config));
        auto* p = static_cast<CaptureReplaceProcessor*>(tap.frameProcessor());
        struct PendingTap final : IRelightTap {
            CaptureTap& tap;
            CaptureReplaceProcessor& p;
            std::size_t& calls;
            std::size_t& preview;
            PendingTap(CaptureTap& t, CaptureReplaceProcessor& pr, std::size_t& c, std::size_t& v)
                : tap(t), p(pr), calls(c), preview(v) {}
            void onDeviceCreate(const DeviceEvent& e) override { tap.onDeviceCreate(e); }
            void onDeviceDestroy() override { tap.onDeviceDestroy(); }
            void onTextureCreate(const TextureDesc& d) override { tap.onTextureCreate(d); }
            void onTextureUpload(const TextureUpload& u) override { tap.onTextureUpload(u); }
            void onTextureWriteLock(const TextureWriteLock& l) override { tap.onTextureWriteLock(l); }
            void onBufferCreate(const BufferDesc& d) override { tap.onBufferCreate(d); }
            void onBufferWrite(const BufferWrite& w) override { tap.onBufferWrite(w); }
            DrawDecision onDraw(const DrawCall& c, const DrawState& st) override { return tap.onDraw(c, st); }
            void onPresent(const FrameEvent& f) override {
                tap.visitPendingDraws([&](std::uint64_t frame, const std::vector<CaptureDrawRecord>& draws) {
                    std::vector<scene::DrawClassification> cls;
                    for (const CaptureDrawRecord& r : draws) {
                        cls.push_back(r.classification);
                        if (r.geometry && r.geometry->captured()) {
                            scene::DrawClassifier::applyGeometryCategories(
                                cls.back(), r.geometry->assetHash(tap.geometry().config().assetRule));
                        }
                    }
                    // Twice: a repeated call for the same frame processes nothing new.
                    calls += p.processPending(frame, draws, draws.size(), cls) == draws.size() ? 1u : 0u;
                    calls += p.processPending(frame, draws, draws.size(), cls) == draws.size() ? 1u : 0u;
                    preview = p.previewLights(tap.translator().lights().frameLights()).size();
                });
                tap.onPresent(f);
            }
        } pending(tap, *p, pendingCalls, previewLights);
        App app;
        app.run(pending, 3);
    }
    CHECK(pendingCalls == 6 && previewLights == 1);
    auto replaceLines = [](const fs::path& path) {
        std::vector<std::string> out;
        std::ifstream in(path);
        for (std::string l; std::getline(in, l);) {
            if (l.find("\"replace_frame\"") != std::string::npos || l.find("\"replacement\"") != std::string::npos) {
                out.push_back(l);
            }
        }
        return out;
    };
    const std::vector<std::string> flushOnly = replaceLines(record), injected = replaceLines(record2);
    CHECK(flushOnly.size() == 6 && flushOnly == injected);
    // No roots, no processor.
    CHECK(createCaptureReplaceProcessor(0) == nullptr);
    fs::remove_all(root);
}

// ---- stager -------------------------------------------------------------------------------------------------------

int stage(const fs::path& fixture, const fs::path& out);

/// The Wine goldens' fixture mods (Tests/relight/fixtures/replace/mods) import cleanly and hold what their comments
/// say (the goldens then check what they do to the apps).
void testFixtureMods(const fs::path& fixtures, const fs::path& tmp) {
    struct Expect {
        const char* name;
        std::size_t meshes, materials, lights, deleted;
        bool rtxConf;
    };
    const Expect expects[] = {
        {"textured_a", 3, 1, 0, 0, false}, {"textured_b", 1, 2, 0, 0, false}, {"lit_lights", 1, 0, 1, 1, false},
        {"lit_conf", 0, 0, 0, 0, true},     {"multi_v1", 1, 0, 0, 0, false},   {"multi_v2", 1, 1, 0, 0, false},
    };
    for (const Expect& x : expects) {
        const fs::path dir = tmp / "fixtures" / x.name;
        CHECK(stage(fixtures / "mods" / x.name, dir) == 0);
        ModLocation loc{x.name, dir.generic_string(), ModKind::Remix, ModFormat::Usd, {}};
        const ModContent c = loadUsdMod(loc, "fixture");
        std::size_t errors = 0, lights = 0, deleted = 0;
        for (const ModDiagnostic& d : c.diagnostics) {
            if (d.severity == "error") {
                ++errors;
                std::fprintf(stderr, "  %s: %s %s %s\n", x.name, d.code.c_str(), d.where.c_str(), d.message.c_str());
            }
        }
        for (const auto& [h, l] : c.lights) {
            (l.deleted ? deleted : lights) += 1;
        }
        CHECK(c.ok && errors == 0);
        CHECK(c.meshes.size() == x.meshes && c.materials.size() == x.materials);
        CHECK(lights == x.lights && deleted == x.deleted);
        CHECK(c.rtxConf.empty() != x.rtxConf);
        if (std::string(x.name) == "textured_a") {
            // mesh_06A0...: a part bound to the mod's own Brick material, whose texture was generated.
            const auto it = c.meshes.find({hash::parseHashRule(hash::rules::kDefaultAssetRuleString).bits, 0x06A0E3ABADEB3249ull});
            CHECK(it != c.meshes.end() && it->second.parts.size() == 1 && !it->second.preserve());
            if (it != c.meshes.end() && !it->second.parts.empty() && !it->second.parts[0].materials.empty()) {
                const auto bm = c.boundMaterials.find(it->second.parts[0].materials[0]);
                CHECK(bm != c.boundMaterials.end() && bm->second.textures.size() == 1);
            }
        }
        if (std::string(x.name) == "lit_lights") {
            const auto it = c.meshes.begin();
            CHECK(it != c.meshes.end() && it->second.preserve() && it->second.lights.size() == 1 && it->second.parts.empty());
        }
    }
}

int stage(const fs::path& fixture, const fs::path& out) {
    std::error_code ec;
    fs::remove_all(out, ec);
    for (fs::recursive_directory_iterator it(fixture), end; it != end; ++it) {
        const fs::path rel = it->path().lexically_relative(fixture);
        if (rel == "gen_textures.txt") {
            continue;
        }
        if (it->is_directory()) {
            fs::create_directories(out / rel);
        } else {
            fs::create_directories((out / rel).parent_path());
            fs::copy_file(it->path(), out / rel, fs::copy_options::overwrite_existing);
        }
    }
    std::ifstream spec(fixture / "gen_textures.txt");
    for (std::string line; std::getline(spec, line);) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::istringstream in(line);
        std::string kind, path;
        std::uint32_t w = 0, h = 0, seed = 0;
        if (!(in >> kind >> path >> w >> h >> seed) || kind != "dds" || w == 0 || h == 0) {
            std::fprintf(stderr, "stage: bad gen_textures.txt line: %s\n", line.c_str());
            return 1;
        }
        writeDds(out / path, w, h, seed);
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 4 && std::string(argv[1]) == "--stage") {
        return stage(argv[2], argv[3]);
    }
    std::optional<fs::path> fixtures;
    if (argc == 3 && std::string(argv[1]) == "--fixtures") {
        fixtures = fs::path(argv[2]);
    }
    const fs::path tmp = fs::absolute("rl_replace_tmp");
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    testMath();
    testStackAndIndex();
    testResidency();
    testWatcher(tmp);
    testEngine(tmp);
    testHotReload(tmp, WatchBackend::Poll, "poll");
#if defined(__linux__)
    testHotReload(tmp, WatchBackend::Notify, "inotify");
    testHotReload(tmp, WatchBackend::Auto, "inotify");
#elif defined(_WIN32)
    if (runningUnderWine()) {
        testHotReload(tmp, WatchBackend::Auto, "poll"); // Wine: auto falls back to polling
    } else {
        testHotReload(tmp, WatchBackend::Notify, "win32");
    }
#endif
    testCaptureTap(tmp);
    if (fixtures) {
        testFixtureMods(*fixtures, tmp);
    }
    fs::remove_all(tmp);
    std::printf("rl_replace_unit: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
