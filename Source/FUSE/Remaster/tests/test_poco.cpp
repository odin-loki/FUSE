// FUSE Remaster W0.1 gates (plan §7 Wave 0, W0.1 exit criteria):
//   remaster_poco_roundtrip       every kind (and an "other" kind, and a record with unknown members) survives
//                                 toJson -> text -> fromJson unchanged, and re-serialises to the same bytes;
//   remaster_poco_schema_parity   Content/schemas/poco/*.schema.json and the C++ structs list the same fields with
//                                 the same JSON shapes (type, enum values, $ref, nullability);
//   remaster_poco_fuzz            1,000 mutated documents (byte-level and tree-level) never crash the reader, and
//                                 every document it accepts round-trips stably.
// Usage: remaster_poco_tests <roundtrip|parity <schema dir>|fuzz [cases]>
#include "check.hpp"

#include <fuse/remaster/poco.hpp>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

namespace poco = fuse::remaster::poco;
namespace json = fuse::remaster::json;

namespace {

poco::BlobRef blob(char c, std::uint64_t size, const char* media) { return {std::string(64, c), size, media}; }

poco::Record header(poco::Kind kind, const std::string& id) {
    poco::Record r;
    r.kind = kind;
    r.id = id;
    r.name = "Name of " + id;
    r.units = {"m", 0.0254, "+Y", "left"};
    r.provenance.origin = poco::Origin::Generated;
    r.provenance.derived_from = {"poco:mat/rock/granite_01"};
    r.provenance.recipe = "recipe:tex-upscale/6f1c@3";
    r.provenance.tool = "Tools/FUSE/Remaster/remaster@abc";
    r.provenance.ai = poco::AiInfo{"local-onnx", "pbrify-remix-upscale-4x", std::string(64, 'e'), 7};
    r.provenance.human_authorship = poco::HumanAuthorship::Selection;
    r.licence_id = id;
    r.distribution = poco::Distribution::Ship;
    r.tags = {"stone", "medieval"};
    r.review = {poco::ReviewState::Approved, "odin", "2026-10-02"};
    return r;
}

std::vector<poco::Record> sampleRecords() {
    std::vector<poco::Record> out;
    {
        poco::Record r = header(poco::Kind::Mesh, "mesh/prop/barrel_a");
        poco::Mesh m;
        m.streams = {{poco::Semantic::Position, poco::Format::F32x3, blob('a', 36, "fuse/mesh-stream")},
                     {poco::Semantic::Color0, poco::Format::Unorm8x4, blob('b', 12, "fuse/mesh-stream")},
                     {poco::Semantic::Weights0, poco::Format::Unorm16x4, blob('c', 24, "fuse/mesh-stream")}};
        m.indices32 = blob('d', 12, "fuse/mesh-stream");
        m.submeshes = {{0, 3, "mat/rock"}, {3, 0, ""}};
        m.bounds = {-1.5f, -0.1f, 0.f, 1.5f, 2.25f, 1e-7f};
        m.skeletonId = "skel/barrel";
        m.lodIds = {"mesh/prop/barrel_a_lod1"};
        r.payload = m;
        out.push_back(r);
    }
    {
        poco::Record r = header(poco::Kind::TextureSet, "tex/castle/wall_stone_01");
        poco::TextureSet t;
        t.albedo = blob('1', 4096, "image/png");
        t.normal = blob('2', 2048, "image/png");
        t.emissive = blob('3', 1, "image/x-exr");
        t.normalConvention = poco::NormalConvention::DX;
        t.texelsPerMetre = 512.f;
        t.albedoSrgb = false;
        r.payload = t;
        r.provenance.origin = poco::Origin::Derived;
        r.provenance.derived_from = {"oaid:6f1c9a00-0000-5000-8000-000000000000"};
        r.replaces = {"oaid:6f1c9a00-0000-5000-8000-000000000000"};
        r.distribution = poco::Distribution::RecipeOnly;
        out.push_back(r);
    }
    {
        poco::Record r = header(poco::Kind::Material, "mat/rock/granite_01");
        poco::Material m;
        m.model = poco::MaterialModel::ClearCoat;
        m.textureSetId = "tex/castle/wall_stone_01";
        m.baseColor = {0.1f, 0.2f, 0.3f, 0.75f};
        m.roughness = 0.33f;
        m.metallic = 1.f;
        m.emissiveNits = 1000.f;
        m.alphaCutoff = 0.125f;
        m.twoSided = true;
        m.physicalCategory = "stone";
        m.layerRecipe = "layer/moss";
        r.payload = m;
        out.push_back(r);
    }
    {
        poco::Record r = header(poco::Kind::Light, "light/torch_01");
        poco::Light l;
        l.type = poco::LightType::Spot;
        l.colorLinear = {1.f, 0.6f, 0.2f};
        l.intensity = 800.f;
        l.range = 12.5f;
        l.innerCone = 0.3f;
        l.outerCone = 0.6f;
        l.size = {0.1f, 0.2f};
        l.castsShadows = false;
        r.payload = l;
        out.push_back(r);
    }
    {
        poco::Record r = header(poco::Kind::Skeleton, "skel/barrel");
        poco::Skeleton s;
        s.joints = {"root", "lid"};
        s.parents = {-1, 0};
        s.inverseBind = blob('4', 128, "fuse/mat4-f32");
        r.payload = s;
        out.push_back(r);
    }
    {
        poco::Record r = header(poco::Kind::AnimClip, "anim/barrel/open");
        poco::AnimClip a;
        a.skeletonId = "skel/barrel";
        a.sampleRate = 60.f;
        a.duration = 1.25f;
        a.tracks = blob('5', 999, "fuse/anim-tracks");
        a.rootMotion = true;
        a.events = {{0.5f, "creak"}, {1.f, "thud"}};
        r.payload = a;
        out.push_back(r);
    }
    {
        poco::Record r = header(poco::Kind::Sound, "snd/door/creak");
        poco::Sound s;
        s.pcm = blob('6', 1u << 20, "audio/wav");
        s.sampleRate = 44100;
        s.channels = 2;
        s.loop = true;
        s.loudnessLufs = -16.f;
        s.category = "sfx";
        r.payload = s;
        out.push_back(r);
    }
    poco::Entity door;
    door.archetype = "fuse.door";
    for (int i = 0; i < 16; ++i) {
        door.transform[std::size_t(i)] = i % 5 == 0 ? 1.f : 0.f;
    }
    door.transform[12] = 3.5f;
    door.assetRefs = {"poco:mesh/prop/door"};
    door.components = {{"fuse.trigger", R"({"radius":2})"}};
    door.legacyClass = "StaticShape";
    door.legacyName = "door01";
    {
        poco::Record r = header(poco::Kind::Entity, "ent/door01");
        r.payload = door;
        out.push_back(r);
    }
    {
        poco::Record r = header(poco::Kind::Level, "level/castle");
        poco::Level l;
        l.entities = {door, poco::Entity{}};
        l.lightIds = {"light/torch_01"};
        l.lookId = "look/remastered";
        l.skyId = "sky/dusk";
        r.payload = l;
        out.push_back(r);
    }
    {
        // A kind outside the plan's set (Relight's material_ext) and unknown header / payload members.
        poco::Record r = header(poco::Kind::Other, "cap/game/mat_0123456789ABCDEF");
        r.otherKind = "material_ext";
        auto raw = json::parse(R"({"blend_enabled":true,"tfactor":4294967295,"nested":{"a":[1,2.5,"x",null]}})");
        r.payload = poco::OtherPayload{*raw};
        r.extra["original_asset"] = "oaid:abc";
        r.provenance.origin = poco::Origin::Original;
        r.distribution = poco::Distribution::Never;
        r.provenance.ai.reset();
        out.push_back(r);
    }
    {
        poco::Record r = header(poco::Kind::Light, "cap/game/light_X");
        poco::Light l;
        l.extra["relight"] = json::parse(R"({"radius":0.5,"intensity_units":"remix"})").value();
        r.payload = l;
        out.push_back(r);
    }
    return out;
}

int roundtrip() {
    const auto records = sampleRecords();
    std::map<std::string, int> kinds;
    for (const poco::Record& r : records) {
        const std::string text = poco::toJsonText(r);
        std::string err;
        const auto back = poco::fromJsonText(text, &err);
        CHECK(back.has_value());
        if (!back) {
            std::fprintf(stderr, "  %s: %s\n", r.id.c_str(), err.c_str());
            continue;
        }
        CHECK(*back == r);
        CHECK(poco::toJsonText(*back) == text);
        CHECK(poco::validate(*back).empty());
        ++kinds[back->kindName()];
    }
    // Every plan kind is covered.
    for (const char* k : poco::EnumNames<poco::Kind>::names) {
        if (std::strcmp(k, "other") != 0) {
            CHECK(kinds.count(k) == 1);
        }
    }
    CHECK(kinds.count("material_ext") == 1);

    // A sparse record (missing fields keep their defaults; null strings read as empty) - the Relight shape.
    {
        const char* text = R"({"schema":"fuse.poco/1","kind":"light","id":"x","provenance":{"recipe":null,"ai":null},
                              "payload":{"type":"Directional","colorLinear":[1,1,1],"intensity":2}})";
        std::string err;
        const auto r = poco::fromJsonText(text, &err);
        CHECK(r.has_value());
        if (r) {
            const auto& l = std::get<poco::Light>(r->payload);
            CHECK(l.type == poco::LightType::Directional && l.intensity == 2.f && l.castsShadows && l.range == 0.f);
            CHECK(r->provenance.recipe.empty() && !r->provenance.ai);
        }
    }
    // Rejections.
    for (const char* bad : {
             R"({"schema":"fuse.poco/2","kind":"mesh","id":"x"})",                       // wrong schema
             R"({"schema":"fuse.poco/1","kind":"mesh","id":""})",                        // empty id
             R"({"schema":"fuse.poco/1","id":"x"})",                                     // no kind
             R"({"schema":"fuse.poco/1","kind":"Mesh!","id":"x"})",                      // bad kind name
             R"({"schema":"fuse.poco/1","kind":"light","id":"x","payload":{"type":"Laser"}})",          // enum
             R"({"schema":"fuse.poco/1","kind":"mesh","id":"x","payload":{"bounds":[1,2,3]}})",          // array size
             R"({"schema":"fuse.poco/1","kind":"sound","id":"x","payload":{"channels":256}})",           // u8 range
             R"({"schema":"fuse.poco/1","kind":"sound","id":"x","payload":{"sampleRate":-1}})",          // unsigned
             R"({"schema":"fuse.poco/1","kind":"sound","id":"x","payload":{"loop":1}})",                 // bool type
             R"({"schema":"fuse.poco/1","kind":"mesh","id":"x","payload":{"lodIds":"a"}})",             // vector type
             R"({"schema":"fuse.poco/1","kind":"mesh","id":"x","payload":{"bounds":[1,2,3,4,5,1e39]}})", // float range
             R"({"schema":"fuse.poco/1","kind":"mesh","id":"x","payload":[]})",                          // payload type
             R"({"schema":"fuse.poco/1","kind":"mesh","id":"x","distribution":"public"})",               // enum
         }) {
        std::string err;
        CHECK(!poco::fromJsonText(bad, &err).has_value());
        CHECK(!err.empty());
    }
    // Policy: distribution is computed, never more permissive than provenance allows (§2.2).
    {
        poco::Record r = header(poco::Kind::TextureSet, "tex/x");
        r.payload = poco::TextureSet{};
        r.provenance.origin = poco::Origin::Derived;
        r.provenance.derived_from = {"oaid:1"};
        r.distribution = poco::Distribution::Ship;
        CHECK(poco::computeDistribution(r) == poco::Distribution::RecipeOnly);
        CHECK(!poco::validate(r).empty());
        r.provenance.origin = poco::Origin::Original;
        CHECK(poco::computeDistribution(r) == poco::Distribution::Never);
        r.licence_id = "LicenseRef-RightsHolderGrant-acme";
        CHECK(poco::computeDistribution(r) == poco::Distribution::Ship);
        poco::Record bad = header(poco::Kind::TextureSet, "../escape");
        poco::TextureSet badMaps;
        badMaps.albedo = poco::BlobRef{"XYZ", 1, "image/png"};
        bad.payload = badMaps;
        CHECK(poco::validate(bad).size() == 2);
    }
    CHECK(poco::recordPath(records.front()) == "poco/mesh/mesh/prop/barrel_a.poco.json");
    return remaster_test::finish("remaster_poco_roundtrip");
}

// ---- schema parity ------------------------------------------------------------------------------------------

template <class T>
struct SchemaName;
#define SCHEMA_NAME(T, n)                                                                                          \
    template <>                                                                                                    \
    struct SchemaName<T> {                                                                                         \
        static constexpr const char* value = n;                                                                    \
    };
SCHEMA_NAME(poco::BlobRef, "blob_ref")
SCHEMA_NAME(poco::MeshStream, "mesh_stream")
SCHEMA_NAME(poco::Submesh, "submesh")
SCHEMA_NAME(poco::Mesh, "mesh")
SCHEMA_NAME(poco::TextureSet, "texture_set")
SCHEMA_NAME(poco::Material, "material")
SCHEMA_NAME(poco::Light, "light")
SCHEMA_NAME(poco::Skeleton, "skeleton")
SCHEMA_NAME(poco::AnimEvent, "anim_event")
SCHEMA_NAME(poco::AnimClip, "anim_clip")
SCHEMA_NAME(poco::Sound, "sound")
SCHEMA_NAME(poco::EntityComponent, "entity_component")
SCHEMA_NAME(poco::Entity, "entity")
SCHEMA_NAME(poco::Level, "level")
SCHEMA_NAME(poco::Units, "units")
SCHEMA_NAME(poco::AiInfo, "ai")
SCHEMA_NAME(poco::Provenance, "provenance")
SCHEMA_NAME(poco::Review, "review")
#undef SCHEMA_NAME

template <class T>
std::string cppShape() {
    using poco::detail::IsArray;
    using poco::detail::IsOptional;
    using poco::detail::IsVector;
    if constexpr (std::is_same_v<T, std::string>) {
        return "string";
    } else if constexpr (std::is_same_v<T, bool>) {
        return "boolean";
    } else if constexpr (std::is_enum_v<T>) {
        std::string s = "enum:";
        for (const char* n : poco::EnumNames<T>::names) {
            s += std::string(n) + "|";
        }
        return s;
    } else if constexpr (std::is_floating_point_v<T>) {
        return "number";
    } else if constexpr (std::is_integral_v<T>) {
        return "integer";
    } else if constexpr (IsOptional<T>::value) {
        return "nullable(" + cppShape<typename T::value_type>() + ")";
    } else if constexpr (IsVector<T>::value) {
        return "array<" + cppShape<typename T::value_type>() + ">";
    } else if constexpr (IsArray<T>::value) {
        return "array" + std::to_string(std::tuple_size_v<T>) + "<" + cppShape<typename T::value_type>() + ">";
    } else {
        return std::string("ref:") + SchemaName<T>::value;
    }
}

std::string schemaShape(const json::Value& p) {
    if (const json::Value* r = p.get("$ref")) {
        std::string n = r->asString();
        const auto dot = n.find(".schema.json");
        return "ref:" + (dot == std::string::npos ? n : n.substr(0, dot));
    }
    if (const json::Value* one = p.get("oneOf")) {
        const auto& alts = one->asArray();
        if (alts.size() == 2 && alts[1].get("type") && alts[1].get("type")->asString() == "null") {
            return "nullable(" + schemaShape(alts[0]) + ")";
        }
        return "oneOf?";
    }
    if (p.get("const")) {
        return "string";
    }
    if (const json::Value* e = p.get("enum")) {
        std::string s = "enum:";
        for (const auto& v : e->asArray()) {
            s += v.asString() + "|";
        }
        return s;
    }
    const std::string type = p.get("type") ? p.get("type")->asString() : "?";
    if (type == "array") {
        const json::Value* items = p.get("items");
        const json::Value* minI = p.get("minItems");
        const json::Value* maxI = p.get("maxItems");
        std::string prefix = "array";
        if (minI && maxI && minI->asInt() == maxI->asInt()) {
            prefix += std::to_string(minI->asInt());
        }
        return prefix + "<" + (items ? schemaShape(*items) : "?") + ">";
    }
    return type;
}

std::optional<json::Value> loadSchema(const std::filesystem::path& dir, const std::string& name) {
    std::ifstream in(dir / (name + ".schema.json"), std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "  missing schema %s.schema.json\n", name.c_str());
        return std::nullopt;
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    json::ParseError pe;
    auto v = json::parse(text, &pe);
    if (!v) {
        std::fprintf(stderr, "  %s.schema.json: %s\n", name.c_str(), pe.message.c_str());
    }
    return v;
}

struct ShapeCollector {
    std::map<std::string, std::string>* out;
    template <class T>
    void operator()(const char* name, T&) {
        (*out)[name] = cppShape<T>();
    }
};

int compareShapes(const std::string& schemaName, const std::map<std::string, std::string>& cpp, const json::Value& schema) {
    int bad = 0;
    const json::Value* props = schema.get("properties");
    if (!props) {
        std::fprintf(stderr, "  %s: no properties\n", schemaName.c_str());
        return 1;
    }
    for (const auto& [name, shape] : cpp) {
        const json::Value* p = props->get(name);
        if (!p) {
            std::fprintf(stderr, "  %s: C++ field '%s' missing from the schema\n", schemaName.c_str(), name.c_str());
            ++bad;
            continue;
        }
        const std::string s = schemaShape(*p);
        // A plain C++ string also reads JSON null (Relight's recipe: null), so the schema may make it nullable.
        if (s != shape && !(shape == "string" && s == "nullable(string)")) {
            std::fprintf(stderr, "  %s.%s: C++ %s vs schema %s\n", schemaName.c_str(), name.c_str(), shape.c_str(), s.c_str());
            ++bad;
        }
    }
    for (const auto& [name, p] : props->asObject()) {
        if (!cpp.contains(name)) {
            std::fprintf(stderr, "  %s: schema property '%s' missing from the C++ struct\n", schemaName.c_str(), name.c_str());
            ++bad;
        }
    }
    return bad;
}

template <class T>
void parityOf(const std::filesystem::path& dir, std::set<std::string>& covered) {
    const std::string name = SchemaName<T>::value;
    covered.insert(name);
    const auto schema = loadSchema(dir, name);
    CHECK(schema.has_value());
    if (!schema) {
        return;
    }
    std::map<std::string, std::string> cpp;
    T tmp{};
    ShapeCollector c{&cpp};
    T::fields(tmp, c);
    CHECK(compareShapes(name, cpp, *schema) == 0);
    CHECK(schema->get("$id") && schema->get("$id")->asString() == "fuse.poco/1/" + name + ".schema.json");
}

int parity(const std::filesystem::path& dir) {
    std::set<std::string> covered;
    parityOf<poco::BlobRef>(dir, covered);
    parityOf<poco::MeshStream>(dir, covered);
    parityOf<poco::Submesh>(dir, covered);
    parityOf<poco::Mesh>(dir, covered);
    parityOf<poco::TextureSet>(dir, covered);
    parityOf<poco::Material>(dir, covered);
    parityOf<poco::Light>(dir, covered);
    parityOf<poco::Skeleton>(dir, covered);
    parityOf<poco::AnimEvent>(dir, covered);
    parityOf<poco::AnimClip>(dir, covered);
    parityOf<poco::Sound>(dir, covered);
    parityOf<poco::EntityComponent>(dir, covered);
    parityOf<poco::Entity>(dir, covered);
    parityOf<poco::Level>(dir, covered);
    parityOf<poco::Units>(dir, covered);
    parityOf<poco::AiInfo>(dir, covered);
    parityOf<poco::Provenance>(dir, covered);
    parityOf<poco::Review>(dir, covered);
    // The header: Record::fields + kind + payload, and one payload schema per plan kind.
    if (const auto schema = loadSchema(dir, "poco")) {
        std::map<std::string, std::string> cpp;
        poco::Record tmp;
        ShapeCollector c{&cpp};
        poco::Record::fields(tmp, c);
        const json::Value* props = schema->get("properties");
        CHECK(props && props->get("kind") && props->get("payload"));
        json::Value trimmed = *schema;
        trimmed["properties"].obj().erase("kind");
        trimmed["properties"].obj().erase("payload");
        CHECK(compareShapes("poco", cpp, trimmed) == 0);
        std::set<std::string> kindRefs;
        for (const auto& branch : schema->get("allOf") ? schema->get("allOf")->asArray() : json::Value::Array{}) {
            const json::Value* ifs = branch.get("if");
            const json::Value* k = ifs && ifs->get("properties") ? ifs->get("properties")->get("kind") : nullptr;
            if (k && k->get("const")) {
                kindRefs.insert(k->get("const")->asString());
            }
        }
        for (const char* k : poco::EnumNames<poco::Kind>::names) {
            if (std::strcmp(k, "other") != 0) {
                CHECK(kindRefs.contains(k));
                CHECK(covered.contains(k));
            }
        }
        CHECK(schema->get("properties")->get("schema")->get("const")->asString() == poco::kSchema);
        covered.insert("poco");
    } else {
        CHECK(false);
    }
    // No schema file without a struct.
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        const std::string f = e.path().filename().string();
        if (f.ends_with(".schema.json")) {
            const std::string n = f.substr(0, f.size() - std::strlen(".schema.json"));
            if (!covered.contains(n)) {
                std::fprintf(stderr, "  schema %s has no C++ struct\n", f.c_str());
                CHECK(false);
            }
        }
    }
    std::printf("schemas checked: %zu\n", covered.size());
    return remaster_test::finish("remaster_poco_schema_parity");
}

// ---- fuzz ---------------------------------------------------------------------------------------------------

struct Rng {
    std::uint64_t s;
    std::uint64_t next() {
        std::uint64_t z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    std::size_t below(std::size_t n) { return n ? static_cast<std::size_t>(next() % n) : 0; }
};

json::Value randomValue(Rng& rng, int depth) {
    switch (rng.below(depth > 2 ? 6 : 8)) {
    case 0: return json::Value();
    case 1: return json::Value(rng.below(2) == 0);
    case 2: return json::Value(static_cast<std::int64_t>(rng.next()) >> rng.below(64));
    case 3: {
        static const double kDoubles[] = {0.5, -1e-300, 3.5e38, 3.4e38, 1e308, -0.0, 1.0 / 3.0, 255.0, 256.0, 4294967296.0};
        return json::Value(kDoubles[rng.below(std::size(kDoubles))]);
    }
    case 4: {
        static const char* kStrings[] = {"", "Position", "F32x3", "GL", "Spot", "original", "recipe-only", "approved",
                                         "\x01\xff", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "mesh", "../x"};
        return json::Value(kStrings[rng.below(std::size(kStrings))]);
    }
    case 5: return json::Value::object();
    case 6: {
        json::Value a = json::Value::array();
        const std::size_t n = rng.below(5);
        for (std::size_t i = 0; i < n; ++i) {
            a.push(randomValue(rng, depth + 1));
        }
        return a;
    }
    default: {
        json::Value o = json::Value::object();
        static const char* kKeys[] = {"sha256", "size", "media", "type", "streams", "semantic", "x", "data", "events", "time"};
        const std::size_t n = rng.below(4);
        for (std::size_t i = 0; i < n; ++i) {
            o[kKeys[rng.below(std::size(kKeys))]] = randomValue(rng, depth + 1);
        }
        return o;
    }
    }
}

/// Picks a random node of the tree (by pre-order index) and returns it.
json::Value* pickNode(json::Value& v, Rng& rng) {
    std::vector<json::Value*> nodes;
    std::vector<json::Value*> stack{&v};
    while (!stack.empty()) {
        json::Value* n = stack.back();
        stack.pop_back();
        nodes.push_back(n);
        if (n->isArray()) {
            for (auto& c : n->arr()) {
                stack.push_back(&c);
            }
        } else if (n->isObject()) {
            for (auto& [k, c] : n->obj()) {
                stack.push_back(&c);
            }
        }
    }
    return nodes[rng.below(nodes.size())];
}

int fuzz(int cases) {
    const auto records = sampleRecords();
    std::vector<std::string> seeds;
    for (const auto& r : records) {
        seeds.push_back(poco::toJsonText(r));
    }
    Rng rng{0xF05E'2026'1105ull};
    int accepted = 0, rejected = 0;
    for (int i = 0; i < cases; ++i) {
        std::string text;
        const std::string& seed = seeds[rng.below(seeds.size())];
        if (i % 2 == 0) { // byte-level: flips, truncation, insertion, deletion
            text = seed;
            const std::size_t edits = 1 + rng.below(4);
            for (std::size_t e = 0; e < edits && !text.empty(); ++e) {
                const std::size_t at = rng.below(text.size());
                switch (rng.below(4)) {
                case 0: text[at] = static_cast<char>(text[at] ^ (1 << rng.below(8))); break;
                case 1: text.resize(at); break;
                case 2: text.insert(at, 1, "{}[]\":,0-e.\\u"[rng.below(14)]); break;
                default: text.erase(at, 1 + rng.below(8)); break;
                }
            }
        } else { // tree-level: replace / delete / add members with values of random type
            auto v = json::parse(seed);
            const std::size_t edits = 1 + rng.below(3);
            for (std::size_t e = 0; e < edits; ++e) {
                json::Value* n = pickNode(*v, rng);
                switch (rng.below(3)) {
                case 0: *n = randomValue(rng, 0); break;
                case 1:
                    if (n->isObject() && n->size() > 0) {
                        auto it = n->obj().begin();
                        std::advance(it, static_cast<std::ptrdiff_t>(rng.below(n->size())));
                        n->obj().erase(it);
                    }
                    break;
                default:
                    if (n->isObject()) {
                        (*n)["fuzz" + std::to_string(rng.below(3))] = randomValue(rng, 0);
                    } else if (n->isArray()) {
                        n->push(randomValue(rng, 0));
                    }
                    break;
                }
            }
            text = json::writeCompact(*v);
        }
        std::string err;
        const auto r = poco::fromJsonText(text, &err);
        if (!r) {
            ++rejected;
            CHECK(!err.empty());
            continue;
        }
        ++accepted;
        (void)poco::validate(*r);
        const std::string once = poco::toJsonText(*r);
        const auto again = poco::fromJsonText(once, &err);
        CHECK(again.has_value());
        if (again) {
            CHECK(*again == *r);
            CHECK(poco::toJsonText(*again) == once);
        }
    }
    std::printf("fuzz cases: %d (accepted %d, rejected %d)\n", cases, accepted, rejected);
    CHECK(accepted > cases / 20 && rejected > cases / 20); // both paths exercised
    return remaster_test::finish("remaster_poco_fuzz");
}

} // namespace

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "";
    if (mode == "roundtrip") {
        return roundtrip();
    }
    if (mode == "parity" && argc > 2) {
        return parity(argv[2]);
    }
    if (mode == "fuzz") {
        return fuzz(argc > 2 ? std::atoi(argv[2]) : 1000);
    }
    std::fprintf(stderr, "usage: remaster_poco_tests roundtrip | parity <schema dir> | fuzz [cases]\n");
    return 2;
}
