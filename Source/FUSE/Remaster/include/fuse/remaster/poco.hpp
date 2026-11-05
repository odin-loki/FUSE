// FUSE Remaster W0.1: fuse_poco, the POCO asset model (docs/plans/FUSE_REMASTER_PLAN.md §2.2-§2.3).
//
// Plain-data records: aggregates of std::string, std::vector, std::array, std::optional, enums and nested
// structs, with no virtual methods and no engine handles. Every struct lists its fields once in a static
// `fields(self, visitor)` function; the JSON reader, the JSON writer and the schema-parity gate all walk that one
// list, so the C++ view, the JSON form and Content/schemas/poco/*.schema.json cannot drift apart unnoticed
// (remaster_poco_schema_parity). Python mirrors the same schemas (Tools/FUSE/Remaster/poco_codegen.py).
//
// On-disk form (§2.1): <store>/poco/<kind>/<poco_id>.poco.json, written by toJsonText (sorted keys, two-space
// indent, shortest round-trip floats), so a record serialises to the same bytes every time.
//
// Reader rules:
//   * the header needs `schema` == "fuse.poco/1", a known-or-other `kind` and a non-empty `id`;
//   * missing fields keep their C++ defaults (records written by older or narrower writers, such as the Relight
//     capture store, still read), present fields must have the right JSON type and range;
//   * members the struct does not know are kept verbatim in `extra` (header and payload) and written back, so a
//     read-write cycle never loses data (Relight's `relight` payload blocks, `original_asset`, ...);
//   * kinds outside the plan's set (Relight's material_ext, relight_replacement) read as Kind::Other with the
//     payload kept as raw JSON.
//
// Deviations from the §2.3 sketch, recorded in the plan: AnimClip events are {time, name} objects (not
// std::pair), optional maps are std::optional<BlobRef> (null in JSON), Units carries metersPerUnit for legacy
// game units, and Entity / Level are both record kinds.
#pragma once

#include <fuse/remaster/digest.hpp>
#include <fuse/remaster/json.hpp>

#include <array>
#include <charconv>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace fuse::remaster::poco {

inline constexpr const char* kSchema = "fuse.poco/1";

// ---- enums (names are the JSON spelling) ------------------------------------------------------------------

template <class E>
struct EnumNames; // specialised below: static constexpr std::array<const char*, N> names

#define FUSE_POCO_ENUM(Type, ...)                                                                                   \
    template <>                                                                                                    \
    struct EnumNames<Type> {                                                                                       \
        static constexpr auto names = std::to_array<const char*>({__VA_ARGS__});                                   \
    };

enum class Semantic : std::uint8_t { Position, Normal, Tangent, Uv0, Uv1, Color0, Joints0, Weights0 };
FUSE_POCO_ENUM(Semantic, "Position", "Normal", "Tangent", "Uv0", "Uv1", "Color0", "Joints0", "Weights0")
enum class Format : std::uint8_t { F32x2, F32x3, F32x4, Unorm8x4, U16x4, Unorm16x4 };
FUSE_POCO_ENUM(Format, "F32x2", "F32x3", "F32x4", "Unorm8x4", "U16x4", "Unorm16x4")
enum class NormalConvention : std::uint8_t { GL, DX };
FUSE_POCO_ENUM(NormalConvention, "GL", "DX")
enum class MaterialModel : std::uint8_t { Opaque, Masked, Translucent, Emissive, Subsurface, ClearCoat, Cloth };
FUSE_POCO_ENUM(MaterialModel, "Opaque", "Masked", "Translucent", "Emissive", "Subsurface", "ClearCoat", "Cloth")
enum class LightType : std::uint8_t { Point, Spot, Directional, Rect, Disk, Dome };
FUSE_POCO_ENUM(LightType, "Point", "Spot", "Directional", "Rect", "Disk", "Dome")
enum class Origin : std::uint8_t { Original, Library, Generated, Derived, Authored };
FUSE_POCO_ENUM(Origin, "original", "library", "generated", "derived", "authored")
enum class HumanAuthorship : std::uint8_t { None, Selection, Substantial };
FUSE_POCO_ENUM(HumanAuthorship, "none", "selection", "substantial")
enum class Distribution : std::uint8_t { Ship, RecipeOnly, Never };
FUSE_POCO_ENUM(Distribution, "ship", "recipe-only", "never")
enum class ReviewState : std::uint8_t { Draft, Approved, Rejected };
FUSE_POCO_ENUM(ReviewState, "draft", "approved", "rejected")
enum class Kind : std::uint8_t { Mesh, TextureSet, Material, Light, Skeleton, AnimClip, Sound, Entity, Level, Other };
FUSE_POCO_ENUM(Kind, "mesh", "texture_set", "material", "light", "skeleton", "anim_clip", "sound", "entity", "level",
               "other")
#undef FUSE_POCO_ENUM

template <class E>
const char* enumName(E e) {
    const auto i = static_cast<std::size_t>(e);
    return i < EnumNames<E>::names.size() ? EnumNames<E>::names[i] : "";
}
template <class E>
std::optional<E> enumFromName(std::string_view s) {
    for (std::size_t i = 0; i < EnumNames<E>::names.size(); ++i) {
        if (s == EnumNames<E>::names[i]) {
            return static_cast<E>(i);
        }
    }
    return std::nullopt;
}

// ---- structs ----------------------------------------------------------------------------------------------

struct BlobRef { // a content-addressed payload in <store>/blobs/sha256/<ab>/<sha256>
    std::string sha256;
    std::uint64_t size = 0;
    std::string media; // "fuse/mesh-stream", "image/png", "audio/wav", ...
    template <class S, class V>
    static void fields(S& s, V& v) {
        v("sha256", s.sha256);
        v("size", s.size);
        v("media", s.media);
    }
    friend bool operator==(const BlobRef&, const BlobRef&) = default;
};

struct MeshStream { // one vertex attribute
    Semantic semantic = Semantic::Position;
    Format format = Format::F32x3;
    BlobRef data;
    template <class S, class V>
    static void fields(S& s, V& v) {
        v("semantic", s.semantic);
        v("format", s.format);
        v("data", s.data);
    }
    friend bool operator==(const MeshStream&, const MeshStream&) = default;
};

struct Submesh {
    std::uint32_t indexOffset = 0, indexCount = 0;
    std::string materialSlot;
    template <class S, class V>
    static void fields(S& s, V& v) {
        v("indexOffset", s.indexOffset);
        v("indexCount", s.indexCount);
        v("materialSlot", s.materialSlot);
    }
    friend bool operator==(const Submesh&, const Submesh&) = default;
};

struct Mesh {
    std::vector<MeshStream> streams;
    BlobRef indices32;
    std::vector<Submesh> submeshes;
    std::array<float, 6> bounds{}; // min xyz, max xyz in metres
    std::string skeletonId;        // empty = static
    std::vector<std::string> lodIds;
    json::Value extra = json::Value::object();
    static constexpr Kind kKind = Kind::Mesh;
    template <class S, class V>
    static void fields(S& s, V& v) {
        v("streams", s.streams);
        v("indices32", s.indices32);
        v("submeshes", s.submeshes);
        v("bounds", s.bounds);
        v("skeletonId", s.skeletonId);
        v("lodIds", s.lodIds);
    }
    friend bool operator==(const Mesh&, const Mesh&) = default;
};

struct TextureSet { // one material's maps, all optional
    std::optional<BlobRef> albedo, normal, orm, height, emissive, opacity;
    NormalConvention normalConvention = NormalConvention::GL;
    float texelsPerMetre = 0.f; // 0 = unknown (legacy)
    bool albedoSrgb = true;
    json::Value extra = json::Value::object();
    static constexpr Kind kKind = Kind::TextureSet;
    template <class S, class V>
    static void fields(S& s, V& v) {
        v("albedo", s.albedo);
        v("normal", s.normal);
        v("orm", s.orm);
        v("height", s.height);
        v("emissive", s.emissive);
        v("opacity", s.opacity);
        v("normalConvention", s.normalConvention);
        v("texelsPerMetre", s.texelsPerMetre);
        v("albedoSrgb", s.albedoSrgb);
    }
    friend bool operator==(const TextureSet&, const TextureSet&) = default;
};

struct Material {
    MaterialModel model = MaterialModel::Opaque;
    std::string textureSetId;
    std::array<float, 4> baseColor{1, 1, 1, 1};
    float roughness = 0.5f, metallic = 0.f, emissiveNits = 0.f;
    float alphaCutoff = 0.5f;
    bool twoSided = false;
    std::string physicalCategory; // footsteps / impacts (asset plan .fusemat)
    std::string layerRecipe;      // optional FUSE layered / procedural material recipe id
    json::Value extra = json::Value::object();
    static constexpr Kind kKind = Kind::Material;
    template <class S, class V>
    static void fields(S& s, V& v) {
        v("model", s.model);
        v("textureSetId", s.textureSetId);
        v("baseColor", s.baseColor);
        v("roughness", s.roughness);
        v("metallic", s.metallic);
        v("emissiveNits", s.emissiveNits);
        v("alphaCutoff", s.alphaCutoff);
        v("twoSided", s.twoSided);
        v("physicalCategory", s.physicalCategory);
        v("layerRecipe", s.layerRecipe);
    }
    friend bool operator==(const Material&, const Material&) = default;
};

struct Light {
    LightType type = LightType::Point;
    std::array<float, 3> colorLinear{};
    float intensity = 0.f; // lumens (point / spot), lux (directional), nits (area)
    float range = 0.f, innerCone = 0.f, outerCone = 0.f;
    std::array<float, 2> size{};
    bool castsShadows = true;
    json::Value extra = json::Value::object();
    static constexpr Kind kKind = Kind::Light;
    template <class S, class V>
    static void fields(S& s, V& v) {
        v("type", s.type);
        v("colorLinear", s.colorLinear);
        v("intensity", s.intensity);
        v("range", s.range);
        v("innerCone", s.innerCone);
        v("outerCone", s.outerCone);
        v("size", s.size);
        v("castsShadows", s.castsShadows);
    }
    friend bool operator==(const Light&, const Light&) = default;
};

struct Skeleton {
    std::vector<std::string> joints;
    std::vector<std::int32_t> parents;
    std::optional<BlobRef> inverseBind;
    json::Value extra = json::Value::object();
    static constexpr Kind kKind = Kind::Skeleton;
    template <class S, class V>
    static void fields(S& s, V& v) {
        v("joints", s.joints);
        v("parents", s.parents);
        v("inverseBind", s.inverseBind);
    }
    friend bool operator==(const Skeleton&, const Skeleton&) = default;
};

struct AnimEvent {
    float time = 0.f;
    std::string name;
    template <class S, class V>
    static void fields(S& s, V& v) {
        v("time", s.time);
        v("name", s.name);
    }
    friend bool operator==(const AnimEvent&, const AnimEvent&) = default;
};

struct AnimClip {
    std::string skeletonId;
    float sampleRate = 30.f;
    float duration = 0.f;
    std::optional<BlobRef> tracks;
    bool rootMotion = false;
    std::vector<AnimEvent> events;
    json::Value extra = json::Value::object();
    static constexpr Kind kKind = Kind::AnimClip;
    template <class S, class V>
    static void fields(S& s, V& v) {
        v("skeletonId", s.skeletonId);
        v("sampleRate", s.sampleRate);
        v("duration", s.duration);
        v("tracks", s.tracks);
        v("rootMotion", s.rootMotion);
        v("events", s.events);
    }
    friend bool operator==(const AnimClip&, const AnimClip&) = default;
};

struct Sound {
    std::optional<BlobRef> pcm;
    std::uint32_t sampleRate = 48000;
    std::uint8_t channels = 1;
    bool loop = false;
    float loudnessLufs = -23.f;
    std::string category;
    json::Value extra = json::Value::object();
    static constexpr Kind kKind = Kind::Sound;
    template <class S, class V>
    static void fields(S& s, V& v) {
        v("pcm", s.pcm);
        v("sampleRate", s.sampleRate);
        v("channels", s.channels);
        v("loop", s.loop);
        v("loudnessLufs", s.loudnessLufs);
        v("category", s.category);
    }
    friend bool operator==(const Sound&, const Sound&) = default;
};

struct EntityComponent { // opaque to the POCO layer
    std::string type;
    std::string json;
    template <class S, class V>
    static void fields(S& s, V& v) {
        v("type", s.type);
        v("json", s.json);
    }
    friend bool operator==(const EntityComponent&, const EntityComponent&) = default;
};

struct Entity {
    std::string archetype;
    std::array<float, 16> transform{};
    std::vector<std::string> assetRefs;
    std::vector<EntityComponent> components;
    std::string legacyClass, legacyName; // e.g. "StaticShape", "door01"
    json::Value extra = json::Value::object();
    static constexpr Kind kKind = Kind::Entity;
    template <class S, class V>
    static void fields(S& s, V& v) {
        v("archetype", s.archetype);
        v("transform", s.transform);
        v("assetRefs", s.assetRefs);
        v("components", s.components);
        v("legacyClass", s.legacyClass);
        v("legacyName", s.legacyName);
    }
    friend bool operator==(const Entity&, const Entity&) = default;
};

struct Level {
    std::vector<Entity> entities;
    std::vector<std::string> lightIds;
    std::string lookId;
    std::string skyId;
    json::Value extra = json::Value::object();
    static constexpr Kind kKind = Kind::Level;
    template <class S, class V>
    static void fields(S& s, V& v) {
        v("entities", s.entities);
        v("lightIds", s.lightIds);
        v("lookId", s.lookId);
        v("skyId", s.skyId);
    }
    friend bool operator==(const Level&, const Level&) = default;
};

struct OtherPayload { // a kind outside the plan's set: the payload stays raw JSON
    json::Value raw = json::Value::object();
    friend bool operator==(const OtherPayload&, const OtherPayload&) = default;
};

using Payload = std::variant<Mesh, TextureSet, Material, Light, Skeleton, AnimClip, Sound, Entity, Level, OtherPayload>;

struct Units {
    std::string length = "m";
    double metersPerUnit = 1.0; // legacy game units -> metres (the payload stays in `length` units)
    std::string up = "+Z";
    std::string handedness = "right";
    template <class S, class V>
    static void fields(S& s, V& v) {
        v("length", s.length);
        v("metersPerUnit", s.metersPerUnit);
        v("up", s.up);
        v("handedness", s.handedness);
    }
    friend bool operator==(const Units&, const Units&) = default;
};

struct AiInfo {
    std::string provider; // "local-onnx", "gemini", "mock", ...
    std::string model;
    std::string model_sha256;
    std::int64_t seed = 0;
    template <class S, class V>
    static void fields(S& s, V& v) {
        v("provider", s.provider);
        v("model", s.model);
        v("model_sha256", s.model_sha256);
        v("seed", s.seed);
    }
    friend bool operator==(const AiInfo&, const AiInfo&) = default;
};

struct Provenance {
    Origin origin = Origin::Authored;
    std::vector<std::string> derived_from; // "oaid:<uuid>" originals, "poco:<id>" records
    std::string recipe;                    // "recipe:<id>@<rev>"; empty = none
    std::string tool;
    std::optional<AiInfo> ai;
    HumanAuthorship human_authorship = HumanAuthorship::Substantial;
    template <class S, class V>
    static void fields(S& s, V& v) {
        v("origin", s.origin);
        v("derived_from", s.derived_from);
        v("recipe", s.recipe);
        v("tool", s.tool);
        v("ai", s.ai);
        v("human_authorship", s.human_authorship);
    }
    friend bool operator==(const Provenance&, const Provenance&) = default;
};

struct Review {
    ReviewState state = ReviewState::Draft;
    std::string by;
    std::string date;
    template <class S, class V>
    static void fields(S& s, V& v) {
        v("state", s.state);
        v("by", s.by);
        v("date", s.date);
    }
    friend bool operator==(const Review&, const Review&) = default;
};

/// One POCO record: the common header (§2.2) plus the kind payload.
struct Record {
    std::string schema = kSchema;
    Kind kind = Kind::Mesh;
    std::string otherKind; // the kind name when kind == Kind::Other
    std::string id;
    std::string name;
    Units units;
    Payload payload;
    Provenance provenance;
    std::string licence_id;
    Distribution distribution = Distribution::Ship;
    std::vector<std::string> replaces; // "oaid:<uuid>"
    std::vector<std::string> tags;
    Review review;
    json::Value extra = json::Value::object(); // unknown header members, kept verbatim
    // The header fields other than kind / payload (written and read generically).
    template <class S, class V>
    static void fields(S& s, V& v) {
        v("schema", s.schema);
        v("id", s.id);
        v("name", s.name);
        v("units", s.units);
        v("provenance", s.provenance);
        v("licence_id", s.licence_id);
        v("distribution", s.distribution);
        v("replaces", s.replaces);
        v("tags", s.tags);
        v("review", s.review);
    }
    std::string kindName() const { return kind == Kind::Other ? otherKind : std::string(enumName(kind)); }
    friend bool operator==(const Record&, const Record&) = default;
};

/// The store-relative path of a record: poco/<kind>/<id>.poco.json.
inline std::string recordPath(const Record& r) { return "poco/" + r.kindName() + "/" + r.id + ".poco.json"; }

// ---- generic field IO ---------------------------------------------------------------------------------------

namespace detail {

template <class T>
struct IsVector : std::false_type {};
template <class T>
struct IsVector<std::vector<T>> : std::true_type {};
template <class T>
struct IsArray : std::false_type {};
template <class T, std::size_t N>
struct IsArray<std::array<T, N>> : std::true_type {};
template <class T>
struct IsOptional : std::false_type {};
template <class T>
struct IsOptional<std::optional<T>> : std::true_type {};

struct NullVisitor {
    template <class T>
    void operator()(const char*, T&) {}
};
template <class T>
concept HasFields = requires(T& t, NullVisitor& v) { T::fields(t, v); };
template <class T>
concept HasExtra = requires(T& t) {
    { t.extra } -> std::convertible_to<json::Value>;
};

/// A float as the double whose shortest form equals the float's shortest form (canonical, round-trips).
inline double floatToDouble(float f) {
    if (!std::isfinite(f)) {
        return static_cast<double>(f);
    }
    char buf[32];
    const auto r = std::to_chars(buf, buf + sizeof(buf), f);
    double d = 0.0;
    std::from_chars(buf, r.ptr, d);
    return d;
}

template <class T>
json::Value toValue(const T& f);

struct Writer {
    json::Value* obj;
    template <class T>
    void operator()(const char* name, const T& f) {
        (*obj)[name] = toValue(f);
    }
};

template <class T>
json::Value toValue(const T& f) {
    if constexpr (std::is_same_v<T, std::string>) {
        return json::Value(f);
    } else if constexpr (std::is_same_v<T, bool>) {
        return json::Value(f);
    } else if constexpr (std::is_enum_v<T>) {
        return json::Value(enumName(f));
    } else if constexpr (std::is_same_v<T, float>) {
        return json::Value(floatToDouble(f));
    } else if constexpr (std::is_floating_point_v<T>) {
        return json::Value(static_cast<double>(f));
    } else if constexpr (std::is_integral_v<T> && std::is_unsigned_v<T>) {
        return json::Value(static_cast<std::uint64_t>(f));
    } else if constexpr (std::is_integral_v<T>) {
        return json::Value(static_cast<std::int64_t>(f));
    } else if constexpr (std::is_same_v<T, json::Value>) {
        return f;
    } else if constexpr (IsOptional<T>::value) {
        return f ? toValue(*f) : json::Value();
    } else if constexpr (IsVector<T>::value || IsArray<T>::value) {
        json::Value a = json::Value::array();
        for (const auto& e : f) {
            a.push(toValue(e));
        }
        return a;
    } else if constexpr (HasFields<T>) {
        json::Value o = json::Value::object();
        if constexpr (HasExtra<T>) {
            for (const auto& [k, m] : f.extra.asObject()) {
                o[k] = m;
            }
        }
        Writer w{&o};
        T::fields(f, w);
        return o;
    } else {
        static_assert(sizeof(T) == 0, "fuse_poco: unsupported field type");
    }
}

struct ReadContext {
    std::string error;
    bool fail(const std::string& path, const char* what) {
        if (error.empty()) {
            error = path + ": " + what;
        }
        return false;
    }
};

template <class T>
bool fromValue(const json::Value& v, T& f, const std::string& path, ReadContext& ctx);

struct Reader {
    const json::Value* obj;
    std::string path;
    ReadContext* ctx;
    std::set<std::string, std::less<>> seen;
    bool ok = true;
    template <class T>
    void operator()(const char* name, T& f) {
        seen.emplace(name);
        if (const json::Value* m = obj->get(name)) {
            if (!fromValue(*m, f, path + "." + name, *ctx)) {
                ok = false;
            }
        }
    }
};

template <class T>
bool fromValue(const json::Value& v, T& f, const std::string& path, ReadContext& ctx) {
    if constexpr (std::is_same_v<T, std::string>) {
        if (v.isNull()) { // null reads as "" (Relight writes recipe: null)
            f.clear();
            return true;
        }
        if (!v.isString()) {
            return ctx.fail(path, "expected a string");
        }
        f = v.asString();
        return true;
    } else if constexpr (std::is_same_v<T, bool>) {
        if (!v.isBool()) {
            return ctx.fail(path, "expected a boolean");
        }
        f = v.asBool();
        return true;
    } else if constexpr (std::is_enum_v<T>) {
        const auto e = v.isString() ? enumFromName<T>(v.asString()) : std::nullopt;
        if (!e) {
            return ctx.fail(path, "unknown enum value");
        }
        f = *e;
        return true;
    } else if constexpr (std::is_floating_point_v<T>) {
        if (!v.isNumber()) {
            return ctx.fail(path, "expected a number");
        }
        const double d = v.asDouble();
        if constexpr (std::is_same_v<T, float>) {
            if (std::isfinite(d) && std::fabs(d) > static_cast<double>(std::numeric_limits<float>::max())) {
                return ctx.fail(path, "number out of float range");
            }
        }
        f = static_cast<T>(d);
        return true;
    } else if constexpr (std::is_integral_v<T>) {
        if (!v.isNumber()) {
            return ctx.fail(path, "expected an integer");
        }
        if (!v.isInt()) {
            const double d = v.asDouble();
            if (d != std::floor(d) || d < -9.2e18 || d > 1.8e19) {
                return ctx.fail(path, "expected an integer");
            }
            if constexpr (std::is_unsigned_v<T>) {
                if (d < 0 || d > static_cast<double>(std::numeric_limits<T>::max())) {
                    return ctx.fail(path, "integer out of range");
                }
                f = static_cast<T>(d);
            } else {
                if (d < static_cast<double>(std::numeric_limits<T>::min()) || d > static_cast<double>(std::numeric_limits<T>::max())) {
                    return ctx.fail(path, "integer out of range");
                }
                f = static_cast<T>(d);
            }
            return true;
        }
        const std::int64_t i = v.asInt();
        if constexpr (std::is_unsigned_v<T>) {
            if (i < 0 || static_cast<std::uint64_t>(i) > std::numeric_limits<T>::max()) {
                return ctx.fail(path, "integer out of range");
            }
        } else {
            if (i < static_cast<std::int64_t>(std::numeric_limits<T>::min()) || i > static_cast<std::int64_t>(std::numeric_limits<T>::max())) {
                return ctx.fail(path, "integer out of range");
            }
        }
        f = static_cast<T>(i);
        return true;
    } else if constexpr (std::is_same_v<T, json::Value>) {
        f = v;
        return true;
    } else if constexpr (IsOptional<T>::value) {
        if (v.isNull()) {
            f.reset();
            return true;
        }
        typename T::value_type inner{};
        if (!fromValue(v, inner, path, ctx)) {
            return false;
        }
        f = std::move(inner);
        return true;
    } else if constexpr (IsVector<T>::value) {
        if (!v.isArray()) {
            return ctx.fail(path, "expected an array");
        }
        T out;
        out.reserve(v.asArray().size());
        for (std::size_t i = 0; i < v.asArray().size(); ++i) {
            typename T::value_type e{};
            if (!fromValue(v.asArray()[i], e, path + "[" + std::to_string(i) + "]", ctx)) {
                return false;
            }
            out.push_back(std::move(e));
        }
        f = std::move(out);
        return true;
    } else if constexpr (IsArray<T>::value) {
        if (!v.isArray() || v.asArray().size() != f.size()) {
            return ctx.fail(path, "expected a fixed-size array");
        }
        for (std::size_t i = 0; i < f.size(); ++i) {
            if (!fromValue(v.asArray()[i], f[i], path + "[" + std::to_string(i) + "]", ctx)) {
                return false;
            }
        }
        return true;
    } else if constexpr (HasFields<T>) {
        if (!v.isObject()) {
            return ctx.fail(path, "expected an object");
        }
        Reader r{&v, path, &ctx, {}, true};
        T::fields(f, r);
        if (!r.ok) {
            return false;
        }
        if constexpr (HasExtra<T>) {
            f.extra = json::Value::object();
            for (const auto& [k, m] : v.asObject()) {
                if (!r.seen.contains(k)) {
                    f.extra[k] = m;
                }
            }
        }
        return true;
    } else {
        static_assert(sizeof(T) == 0, "fuse_poco: unsupported field type");
    }
}

/// Field names of a struct, in declaration order (the schema-parity gate compares them with the schemas).
struct NameCollector {
    std::vector<std::string>* names;
    template <class T>
    void operator()(const char* name, T&) {
        names->emplace_back(name);
    }
};

} // namespace detail

template <class T>
std::vector<std::string> fieldNames() {
    std::vector<std::string> out;
    T tmp{};
    detail::NameCollector c{&out};
    T::fields(tmp, c);
    return out;
}

// ---- record (de)serialisation -------------------------------------------------------------------------------

inline json::Value toJson(const Record& r) {
    json::Value o = json::Value::object();
    for (const auto& [k, m] : r.extra.asObject()) {
        o[k] = m;
    }
    detail::Writer w{&o};
    Record::fields(r, w);
    o["kind"] = json::Value(r.kindName());
    o["payload"] = std::visit(
        [](const auto& p) -> json::Value {
            using P = std::decay_t<decltype(p)>;
            if constexpr (std::is_same_v<P, OtherPayload>) {
                return p.raw;
            } else {
                return detail::toValue(p);
            }
        },
        r.payload);
    return o;
}

inline std::string toJsonText(const Record& r) { return json::writePretty(toJson(r)); }

/// The default payload of a kind.
inline Payload defaultPayload(Kind k) {
    switch (k) {
    case Kind::Mesh: return Mesh{};
    case Kind::TextureSet: return TextureSet{};
    case Kind::Material: return Material{};
    case Kind::Light: return Light{};
    case Kind::Skeleton: return Skeleton{};
    case Kind::AnimClip: return AnimClip{};
    case Kind::Sound: return Sound{};
    case Kind::Entity: return Entity{};
    case Kind::Level: return Level{};
    case Kind::Other: break;
    }
    return OtherPayload{};
}

inline std::optional<Record> fromJson(const json::Value& v, std::string* error = nullptr) {
    auto fail = [&](const std::string& e) -> std::optional<Record> {
        if (error) {
            *error = e;
        }
        return std::nullopt;
    };
    if (!v.isObject()) {
        return fail("record: expected an object");
    }
    Record r;
    const json::Value* kind = v.get("kind");
    if (!kind || !kind->isString() || kind->asString().empty()) {
        return fail("record.kind: expected a non-empty string");
    }
    if (const auto k = enumFromName<Kind>(kind->asString()); k && *k != Kind::Other) {
        r.kind = *k;
    } else {
        r.kind = Kind::Other;
        r.otherKind = kind->asString();
        for (const char c : r.otherKind) {
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
                return fail("record.kind: kind names are [a-z0-9_]");
            }
        }
    }
    detail::ReadContext ctx;
    detail::Reader rd{&v, "record", &ctx, {}, true};
    Record::fields(r, rd);
    if (!rd.ok) {
        return fail(ctx.error);
    }
    if (r.schema != kSchema) {
        return fail("record.schema: expected \"" + std::string(kSchema) + "\"");
    }
    if (r.id.empty()) {
        return fail("record.id: must not be empty");
    }
    rd.seen.emplace("kind");
    rd.seen.emplace("payload");
    for (const auto& [k, m] : v.asObject()) {
        if (!rd.seen.contains(k)) {
            r.extra[k] = m;
        }
    }
    r.payload = defaultPayload(r.kind);
    const json::Value* payload = v.get("payload");
    if (payload && !payload->isObject()) {
        return fail("record.payload: expected an object");
    }
    if (payload) {
        bool ok = std::visit(
            [&](auto& p) -> bool {
                using P = std::decay_t<decltype(p)>;
                if constexpr (std::is_same_v<P, OtherPayload>) {
                    p.raw = *payload;
                    return true;
                } else {
                    return detail::fromValue(*payload, p, "record.payload", ctx);
                }
            },
            r.payload);
        if (!ok) {
            return fail(ctx.error);
        }
    }
    return r;
}

inline std::optional<Record> fromJsonText(std::string_view text, std::string* error = nullptr) {
    json::ParseError pe;
    const auto v = json::parse(text, &pe);
    if (!v) {
        if (error) {
            *error = "json: " + pe.message + " at byte " + std::to_string(pe.offset);
        }
        return std::nullopt;
    }
    return fromJson(*v, error);
}

// ---- policy helpers (§0.1.2-4, §2.2) -----------------------------------------------------------------------

/// The distribution a record is allowed, computed from provenance and licence (§2.2: never set by hand).
/// Originals never ship; anything with an original in its derivation chain (an "oaid:" input, or origin
/// derived, or a LicenseRef-Original-* licence) is recipe-only, unless a rights-holder grant covers it.
inline Distribution computeDistribution(const Record& r) {
    if (r.licence_id.starts_with("LicenseRef-RightsHolderGrant-")) {
        return Distribution::Ship;
    }
    if (r.provenance.origin == Origin::Original || r.licence_id.starts_with("LicenseRef-Original-")) {
        return Distribution::Never;
    }
    bool fromOriginal = !r.replaces.empty() && r.provenance.origin == Origin::Derived;
    for (const std::string& d : r.provenance.derived_from) {
        fromOriginal = fromOriginal || d.starts_with("oaid:");
    }
    return fromOriginal ? Distribution::RecipeOnly : Distribution::Ship;
}

/// Consistency checks beyond the JSON types: id shape, blob hashes, distribution not more permissive than the
/// computed one. Empty = valid.
inline std::vector<std::string> validate(const Record& r) {
    std::vector<std::string> errs;
    if (r.id.find("..") != std::string::npos || r.id.starts_with('/') || r.id.find('\\') != std::string::npos) {
        errs.push_back("id '" + r.id + "' is not a safe store-relative id");
    }
    auto rank = [](Distribution d) { return d == Distribution::Ship ? 0 : d == Distribution::RecipeOnly ? 1 : 2; };
    if (rank(r.distribution) < rank(computeDistribution(r))) {
        errs.push_back(std::string("distribution '") + enumName(r.distribution) + "' is more permissive than the computed '" +
                       enumName(computeDistribution(r)) + "'");
    }
    // Every BlobRef in the payload carries a lower-case sha256.
    const json::Value p = toJson(r)["payload"];
    std::vector<const json::Value*> stack{&p};
    while (!stack.empty()) {
        const json::Value* v = stack.back();
        stack.pop_back();
        if (v->isObject()) {
            const json::Value* sha = v->get("sha256");
            if (sha && v->get("media") && !isSha256Hex(sha->asString())) {
                errs.push_back("blob reference with a malformed sha256 '" + sha->asString() + "'");
            }
            for (const auto& [k, m] : v->asObject()) {
                stack.push_back(&m);
            }
        } else if (v->isArray()) {
            for (const auto& m : v->asArray()) {
                stack.push_back(&m);
            }
        }
    }
    return errs;
}

} // namespace fuse::remaster::poco
