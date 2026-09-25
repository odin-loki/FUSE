// FUSE Relight RL-3.5: unit tests of the Logic graph runtime (ctest rl_logic_unit).
//
//   fuse_relight_logic_tests [--fixtures <dir>] [--update]
//     the component, parser and runtime tests (test_logic_components.cpp, test_logic_parser.cpp) and, with
//     --fixtures (Tests/relight/fixtures/logic), the fixture graphs: graphs/<case>.json names a USDA mod stage, the
//     owners, N frames of inputs and prim snapshots; the graphs run N frames and every frame's outputs (plus the
//     option layers the graphs hold and watched option values) must equal graphs/<case>.expected.txt (--update
//     rewrites it). Temporary files live under ./rl_logic_tmp and are removed at the end.
#include "test_logic_common.hpp"

#include <fuse/relight/capture/export/json.hpp>
#include <fuse/relight/logic/keybind.hpp>
#include <fuse/relight/logic/logic_log.hpp>
#include <fuse/relight/logic/logic_runtime.hpp>
#include <fuse/relight/mods/usd/usd_stage.hpp>
#include <fuse/relight/options/option_manager.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>

namespace rl_logic_test {

int g_checks = 0;
int g_failures = 0;

namespace {

namespace fs = std::filesystem;
namespace json = fuse::relight::capture::exporter::json;
namespace usd = fuse::relight::mods::usd;

/// Options the fixture graphs read and drive (RtxOptionRead*, RtxOptionLayerAction .conf files).
struct FixtureOptions {
    FUSE_RELIGHT_OPTION("rtx.logicFixture", bool, flag, true, "RL-3.5 fixture option (bool).");
    FUSE_RELIGHT_OPTION("rtx.logicFixture", float, value, 1.0f, "RL-3.5 fixture option (float).");
    FUSE_RELIGHT_OPTION("rtx.logicFixture", fuse::relight::options::Vec3f, color, fuse::relight::options::Vec3f(1.0f, 1.0f, 1.0f),
                        "RL-3.5 fixture option (Vector3).");
};

std::string readText(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream s;
    s << f.rdbuf();
    return s.str();
}

std::uint64_t hexOf(const std::string& s) { return std::strtoull(s.c_str(), nullptr, 16); }

Vector3 vec3Of(const json::Value* v, const Vector3& fallback) {
    if (v == nullptr || !v->isArray() || v->a.size() != 3) {
        return fallback;
    }
    return Vector3(static_cast<float>(v->a[0].n), static_cast<float>(v->a[1].n), static_cast<float>(v->a[2].n));
}

PrimSnapshot snapshotOf(const json::Value& p) {
    PrimSnapshot s;
    const std::string kind = p.str("kind", "mesh");
    s.kind = kind == "light" ? PrimSnapshot::Kind::Light : (kind == "graph" ? PrimSnapshot::Kind::Graph : PrimSnapshot::Kind::Mesh);
    const Vector3 t = vec3Of(p.get("translation"), Vector3(0.0f));
    const Vector3 sc = vec3Of(p.get("scale"), Vector3(1.0f));
    s.objectToWorld[0] = Vector4(sc.x, 0, 0, 0);
    s.objectToWorld[1] = Vector4(0, sc.y, 0, 0);
    s.objectToWorld[2] = Vector4(0, 0, sc.z, 0);
    s.objectToWorld[3] = Vector4(t, 1.0f);
    s.lightPosition = t;
    if (const json::Value* b = p.get("bounds"); b != nullptr && b->isArray() && b->a.size() == 6) {
        s.bounds.minPos = Vector3(static_cast<float>(b->a[0].n), static_cast<float>(b->a[1].n), static_cast<float>(b->a[2].n));
        s.bounds.maxPos = Vector3(static_cast<float>(b->a[3].n), static_cast<float>(b->a[4].n), static_cast<float>(b->a[5].n));
    }
    if (const json::Value* bones = p.get("bones"); bones != nullptr && bones->isArray()) {
        for (const json::Value& bt : bones->a) {
            Matrix4 m;
            m[3] = Vector4(vec3Of(&bt, Vector3(0.0f)), 1.0f);
            s.boneMatrices.push_back(m);
        }
    }
    return s;
}

std::set<std::uint32_t> keysOf(const json::Value* v) {
    std::set<std::uint32_t> out;
    if (v != nullptr && v->isArray()) {
        for (const json::Value& k : v->a) {
            std::vector<std::uint32_t> codes;
            if (parseVirtualKeys(k.s, codes)) {
                out.insert(codes.begin(), codes.end());
            }
        }
    }
    return out;
}

/// Applies one "inputs" entry: listed fields replace the running state from this frame on (keys_pressed: this
/// frame only).
void applyInputs(const json::Value& e, FrameInputs& in) {
    if (const json::Value* c = e.get("camera")) {
        in.camera.valid = c->flag("valid", true);
        in.camera.position = vec3Of(c->get("position"), in.camera.position);
        in.camera.forward = vec3Of(c->get("forward"), in.camera.forward);
        in.camera.right = vec3Of(c->get("right"), in.camera.right);
        in.camera.up = vec3Of(c->get("up"), in.camera.up);
        in.camera.fovRadians = static_cast<float>(c->num("fov_degrees", 60.0)) * kPi / 180.0f;
        in.camera.aspectRatio = static_cast<float>(c->num("aspect", 1.0));
    }
    auto table = [&](const char* key, std::map<std::uint64_t, std::uint32_t>& out) {
        if (const json::Value* t = e.get(key); t != nullptr && t->isObject()) {
            out.clear();
            for (const auto& [h, n] : t->o) {
                out[hexOf(h)] = static_cast<std::uint32_t>(n.n);
            }
        }
    };
    table("mesh_hashes", in.meshHashUsage);
    table("texture_hashes", in.textureHashUsage);
    if (const json::Value* l = e.get("light_hashes"); l != nullptr && l->isArray()) {
        in.lightHashes.clear();
        for (const json::Value& h : l->a) {
            in.lightHashes.insert(hexOf(h.s));
        }
    }
    if (e.get("fog_hash") != nullptr) {
        in.fogHash = hexOf(e.str("fog_hash"));
    }
    if (e.get("keys_down") != nullptr) {
        in.keysDown = keysOf(e.get("keys_down"));
    }
}

std::string relative(const std::string& path, const std::string& base) {
    return path.compare(0, base.size() + 1, base + "/") == 0 ? path.substr(base.size() + 1) : path;
}

/// Runs one fixture case and returns its per-frame dump.
std::string runCase(const fs::path& caseFile, const fs::path& fixturesDir) {
    // Layers released by an earlier runtime leave the options at the next resolve: start from resolved defaults.
    fuse::relight::options::OptionManager::applyPendingValues(nullptr, false);
    std::string err;
    const auto doc = json::parse(readText(caseFile), &err);
    CHECK_MSG(doc.has_value(), caseFile.string() + ": " + err);
    if (!doc) {
        return {};
    }
    const fs::path dir = caseFile.parent_path();
    const std::string base = fs::weakly_canonical(fixturesDir).generic_string();
    const usd::ComposedStage stage = usd::readStage(fs::weakly_canonical(dir / doc->str("stage")).generic_string());
    CHECK_MSG(stage.ok, caseFile.string());
    const ModGraphs mods = loadModGraphs(stage, caseFile.stem().string());
    std::ostringstream out;
    for (const GraphDiagnostic& d : mods.diagnostics) {
        out << "diagnostic " << logSeverityName(d.severity) << " " << relative(d.path, base) << ": " << d.message << "\n";
    }
    LogicRuntime runtime;
    runtime.setModGraphs({mods});
    const ReplacementGraphs* graphs = runtime.find(caseFile.stem().string(), hexOf(doc->str("mesh")));
    CHECK_MSG(graphs != nullptr, caseFile.string() + ": no graphs on the mesh replacement");
    if (graphs == nullptr) {
        return out.str();
    }
    std::vector<std::uint64_t> owners;
    if (const json::Value* o = doc->get("owners"); o != nullptr && o->isArray()) {
        for (const json::Value& v : o->a) {
            owners.push_back(static_cast<std::uint64_t>(v.n));
        }
    }
    if (owners.empty()) {
        owners.push_back(1);
    }
    std::vector<PrimSnapshot> prims(graphs->prims.size());
    if (const json::Value* p = doc->get("prims"); p != nullptr && p->isObject()) {
        for (const auto& [path, snap] : p->o) {
            const auto it = graphs->primTable.find(path);
            CHECK_MSG(it != graphs->primTable.end(), caseFile.string() + ": prim " + path + " is not in the prim table");
            if (it != graphs->primTable.end()) {
                prims[it->second] = snapshotOf(snap);
            }
        }
    }
    std::vector<std::string> watched;
    if (const json::Value* w = doc->get("watch_options"); w != nullptr && w->isArray()) {
        for (const json::Value& v : w->a) {
            watched.push_back(v.s);
        }
    }
    FrameInputs inputs;
    inputs.deltaTime = static_cast<float>(doc->num("dt", 1.0 / 60.0));
    const std::uint32_t frames = doc->u32("frames", 1);
    for (std::uint32_t f = 0; f < frames; ++f) {
        inputs.frame = f;
        inputs.keysPressed.clear();
        if (const json::Value* all = doc->get("inputs"); all != nullptr && all->isObject()) {
            if (const json::Value* e = all->get(std::to_string(f))) {
                applyInputs(*e, inputs);
                inputs.keysPressed = keysOf(e->get("keys_pressed"));
            }
        }
        std::vector<GraphOwnerFrame> frameOwners;
        for (std::uint64_t o : owners) {
            inputs.prims[o] = prims;
            frameOwners.push_back({o, graphs, "fixture"});
        }
        const LogicFrameReport r = runtime.runFrame(inputs, frameOwners, LogicRunOptions{});
        out << "frame " << f << "\n";
        for (const LogicFrameReport::InstanceValues& v : r.values) {
            for (const auto& [name, value] : v.outputs) {
                out << "  owner " << v.owner << " " << v.graph.substr(v.graph.rfind('/') + 1) << " " << name << " = " << value << "\n";
            }
        }
        for (const HeldOptionLayer& l : r.layers) {
            out << "  layer " << relative(l.configPath, base) << " priority " << l.priority << " refs " << l.references << " enabled "
                << (l.enabled ? "true" : "false") << " strength " << formatPropertyValue(PropertyValue(l.blendStrength), PT::Float)
                << " threshold " << formatPropertyValue(PropertyValue(l.blendThreshold), PT::Float) << "\n";
        }
        for (const std::string& name : watched) {
            const fuse::relight::options::OptionBase* o = fuse::relight::options::OptionManager::findOption(name);
            out << "  option " << name << " = " << (o != nullptr ? o->getResolvedValueAsString() : std::string("(missing)")) << "\n";
        }
    }
    return out.str();
}

void testFixtureGraphs(const fs::path& fixtures, bool update) {
    (void)FixtureOptions::flag;
    const fs::path dir = fixtures / "graphs";
    std::vector<fs::path> cases;
    for (const auto& e : fs::directory_iterator(dir)) {
        if (e.path().extension() == ".json") {
            cases.push_back(e.path());
        }
    }
    std::sort(cases.begin(), cases.end());
    CHECK(cases.size() >= 5);
    for (const fs::path& c : cases) {
        const std::string dump = runCase(c, fixtures);
        // A second run from scratch is identical (deterministic evaluation).
        CHECK_MSG(runCase(c, fixtures) == dump, c.filename().string() + ": two runs differ");
        const fs::path expected = fs::path(c).replace_extension(".expected.txt");
        if (update) {
            std::ofstream(expected, std::ios::binary) << dump;
            std::printf("updated %s\n", expected.string().c_str());
            continue;
        }
        const std::string want = readText(expected);
        CHECK_MSG(!want.empty() && dump == want, c.filename().string() + ": output differs from " + expected.filename().string());
        if (dump != want) {
            std::fprintf(stderr, "---- %s (run)\n%s", c.filename().string().c_str(), dump.c_str());
        }
        fuse::relight::options::OptionManager::applyPendingValues(nullptr, false);
    }
    CHECK(heldOptionLayers().empty());
}

void testRegistry() {
    registerAllComponents();
    const std::vector<const ComponentSpec*> all = listComponents();
    CHECK_MSG(all.size() == 68, std::to_string(all.size()));
    std::map<std::string, int> byCategory;
    for (const ComponentSpec* s : all) {
        byCategory[std::string(s->categories)]++;
        CHECK_MSG(s->name.rfind("lightspeed.trex.logic.", 0) == 0 && s->version == 1 && s->isValid(), s->name);
        for (const PropertySpec& p : s->properties) {
            const bool prefixed = p.usdPropertyName == (p.ioType == PropertyIOType::Output ? "outputs:" : "inputs:") + p.name;
            CHECK_MSG(prefixed, s->name + "." + p.name);
            if (!isFlexibleType(p.declaredType)) {
                CHECK_MSG(p.type == p.declaredType && valueMatchesType(p.defaultValue, p.type), s->name + "." + p.name);
            }
        }
    }
    CHECK(byCategory["Constants"] == 11 && byCategory["Act"] == 1 && byCategory["Sense"] == 19 && byCategory["Transform"] == 36 &&
          byCategory["TODO"] == 1);
    // Every variant's defaults match its resolved types.
    for (const ComponentSpec* s : all) {
        for (const ComponentSpec* v : getAllComponentSpecVariants(s->componentType)) {
            for (const PropertySpec& p : v->properties) {
                CHECK_MSG(!isFlexibleType(p.type) && valueMatchesType(p.defaultValue, p.type), v->name + "." + p.name);
            }
        }
    }
    CHECK(specOf("Remap")->oldNames == std::vector<std::string>{"InterpolateFloat"});
    const PropertySpec& priority = specOf("RtxOptionLayerAction")->properties[4];
    CHECK(priority.name == "priority" && priority.hardMin == PropertyValue(100.0f) && priority.hardMax == PropertyValue(10000000.0f) &&
          priority.defaultValue == PropertyValue(10000.0f));
    CHECK(specOf("Loop")->properties[3].enumValues.size() == 4 && specOf("Remap")->properties[4].enumValues.size() == 9);
}

} // namespace

// ---- Shared helpers --------------------------------------------------------------------------------------------------

const ComponentSpec* variantOf(const std::string& cls, const std::map<std::string, PropertyType>& types) {
    registerAllComponents();
    for (const ComponentSpec* v : getAllComponentSpecVariants(componentTypeFromName(fullName(cls)))) {
        bool all = true;
        for (const auto& [name, type] : types) {
            const auto it = v->resolvedTypes.find(name);
            all = all && it != v->resolvedTypes.end() && it->second == type;
        }
        if (all) {
            return v;
        }
    }
    return nullptr;
}

const ComponentSpec* specOf(const std::string& cls) {
    registerAllComponents();
    return getComponentSpec(componentTypeFromName(fullName(cls)));
}

std::size_t variantCount(const std::string& cls) {
    registerAllComponents();
    return getAllComponentSpecVariants(componentTypeFromName(fullName(cls))).size();
}

Direct::Direct(const ComponentSpec* s, std::vector<PropertyVector> p) : props(std::move(p)), spec(s) {
    CHECK(spec != nullptr);
    if (spec == nullptr) {
        return;
    }
    CHECK(props.size() == spec->properties.size());
    std::vector<std::size_t> indices;
    for (std::size_t i = 0; i < props.size(); ++i) {
        indices.push_back(i);
    }
    comp = spec->createComponentBatch(batch, props, indices);
}

void Direct::initialize(std::size_t count) {
    const LogicContext ctx(inputs);
    for (std::size_t i = 0; comp && spec->initialize != nullptr && i < count; ++i) {
        spec->initialize(ctx, *comp, i);
    }
}

void Direct::update(std::size_t start, std::size_t end) {
    const LogicContext ctx(inputs);
    if (comp) {
        comp->updateRange(ctx, start, end);
    }
}

Single::Single(const ComponentSpec* s, const std::map<std::string, PropertyValue>& values, const FrameInputs& in) : inputs(in), spec(s) {
    CHECK(spec != nullptr);
    if (spec == nullptr) {
        return;
    }
    auto topology = std::make_shared<GraphTopology>();
    auto state = std::make_shared<GraphState>();
    std::vector<std::size_t> indices;
    for (const PropertySpec& p : spec->properties) {
        indices.push_back(topology->propertyTypes.size());
        topology->propertyTypes.push_back(p.type);
        const auto it = values.find(p.name);
        state->values.push_back(it != values.end() ? it->second : p.defaultValue);
    }
    topology->propertyIndices.push_back(indices);
    topology->componentSpecs.push_back(spec);
    topology->nodePaths.push_back("/test/" + spec->getClassName());
    topology->graphHash = spec->componentType ^ reinterpret_cast<std::uintptr_t>(spec);
    state->topology = topology;
    state->primPath = "/test";
    const LogicContext ctx(inputs);
    instance = manager.addInstance(ctx, state, kOwner);
    CHECK_MSG(instance != nullptr, spec->name);
}

Single::~Single() { manager.clear(); }

void Single::update() {
    const LogicContext ctx(inputs);
    manager.update(ctx);
}

PropertyValue Single::get(const std::string& property) const {
    const GraphBatch* batch = instance != nullptr ? manager.batchOf(*instance) : nullptr;
    for (std::size_t p = 0; batch != nullptr && p < spec->properties.size(); ++p) {
        if (spec->properties[p].name == property) {
            return batch->value(batch->topology().propertyIndices[0][p], instance->batchIndex());
        }
    }
    CHECK_MSG(false, "no property " + property);
    return kInvalidPropertyValue;
}

void Single::set(const std::string& property, const PropertyValue& value) {
    if (instance == nullptr) {
        return;
    }
    GraphBatch& batch = *manager.batches().at(instance->graphHash());
    for (std::size_t p = 0; p < spec->properties.size(); ++p) {
        if (spec->properties[p].name != property) {
            continue;
        }
        PropertyVector& vec = batch.properties()[batch.topology().propertyIndices[0][p]];
        const std::size_t i = instance->batchIndex();
        std::visit(
            [&](auto& v) {
                using T = typename std::decay_t<decltype(v)>::value_type;
                CHECK_MSG(std::holds_alternative<T>(value), "set " + property + ": wrong type");
                if (std::holds_alternative<T>(value)) {
                    v[i] = std::get<T>(value);
                }
            },
            vec);
        return;
    }
    CHECK_MSG(false, "no property " + property);
}

} // namespace rl_logic_test

int main(int argc, char** argv) {
    using namespace rl_logic_test;
    std::optional<fs::path> fixtures;
    bool update = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--fixtures" && i + 1 < argc) {
            fixtures = fs::path(argv[++i]);
        } else if (a == "--update") {
            update = true;
        }
    }
    const fs::path tmp = fs::absolute("rl_logic_tmp");
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    testRegistry();
    testComponents();
    testParser();
    if (fixtures) {
        testFixtureGraphs(*fixtures, update);
    }
    fs::remove_all(tmp);
    std::printf("rl_logic_unit: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
