// FUSE Relight RL-3.4: one mod's replacements, typed for the runtime (docs/plans/FUSE_REMIX_PORT_PLAN.md §4.4, §4.7).
//
// The runtime reads what RL-3.2 (mods/import) produces, in the RL-1.8 store-shim layout:
//
//   db/remaster_db.json   `replacement` rows (oaid, variant, tier, poco_id, kind, source): the index. `kind` names
//                         the record kind (relight_replacement, material, light); `source` is the import prefix
//                         ("mod/<game>/<name>"), so one store may hold several stacked mods;
//   poco/<kind>/<id>.poco.json   the records the rows name (+ material_ext, mesh, light records they reference);
//   poco/relight_mod/<source>.poco.json   the mod record: its rtx.conf blob and the import diagnostics;
//   blobs/sha256/<ab>/<sha>      payloads (the rtx.conf text is read from here).
//
// A Remix mod directory (mod.usda / .usdc / .usd) is imported in memory with mods::import::importMod (the same
// records and rows, never written to disk); a FUSE-native mod is an imported store directory (fuse_relight_import
// output) and is read from disk. Either way parseModStore turns the rows into:
//
//   meshes     mesh_<H> (relight_replacement): key under the mod's asset rule (legacy rules too), preserve-
//              OriginalDrawCall, category overrides, parts (mesh + transform relative to the replacement root +
//              bound materials), attached lights (transform relative to the replacement root). A mesh_<H> with no
//              parts and no lights removes the original draw (unless preserveOriginalDrawCall).
//   materials  mat_<H> (material + material_ext + texture_set): keyed by the stage-0 texture hash H.
//   lights     light_<H> (light): a game light's replacement. A light_<H> prim that is not a UsdLux light (the
//              importer's "not_a_light" diagnostic in the mod record) deletes the game light (Remix: a light
//              replacement without a light).
//
// Matrices are row-major doubles in the D3D / USD row-vector convention (translation in m[12..14]), as the
// records store them: world = local * objectToWorld.
#pragma once

#include <fuse/relight/capture/export/json.hpp>
#include <fuse/relight/hash/geometry_hash.hpp>
#include <fuse/relight/scene/classify/instance_categories.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fuse::relight::replace {

using hash::Hash64;
using Mat4d = std::array<double, 16>;
using Vec3d = std::array<double, 3>;

Mat4d identity4d();
/// Row-vector product: v * a * b.
Mat4d multiply(const Mat4d& a, const Mat4d& b);
Mat4d toMat4d(const std::array<float, 16>& m);
/// p * m (w = 1).
Vec3d transformPoint(const Mat4d& m, const Vec3d& p);
/// d * m (w = 0), normalized (d unchanged when the result has zero length).
Vec3d transformDirection(const Mat4d& m, const Vec3d& d);

enum class ModKind : std::uint8_t { Remix, FuseNative };
enum class ModFormat : std::uint8_t { Usd, Store };
const char* modKindName(ModKind kind);
const char* modFormatName(ModFormat format);

struct ModLocation {
    std::string name;   ///< mod name: the directory name (USD), or the store source's last segment
    std::string dir;    ///< '/' separators, no trailing '/'
    ModKind kind = ModKind::Remix;
    ModFormat format = ModFormat::Usd;
    std::string source; ///< store mods: the DB `source`; empty for USD mods
    /// Identity of the mod across reloads.
    std::string id() const { return source.empty() ? dir : dir + "#" + source; }
};

struct LightDef {
    std::string recordId;
    std::string name;
    std::string type;    ///< POCO light type: Point, Spot, Directional, Rect, Disk, Dome
    std::string usdType; ///< SphereLight, DistantLight, ...
    Vec3d color{1.0, 1.0, 1.0};
    double intensity = 0.0;
    double innerCone = 0.0, outerCone = 0.0;
    std::array<double, 2> size{0.0, 0.0};
    Mat4d transform = identity4d(); ///< light_<H>: the stage transform; attached: relative to the replacement root
};

struct MeshPartDef {
    std::string meshId;
    std::string prim;
    Mat4d transform = identity4d(); ///< relative to the replacement root
    std::vector<std::string> materials; ///< per submesh: bound material record id ("" when unbound)
    std::array<double, 6> bounds{};     ///< min xyz, max xyz (mesh space)
};

struct MeshReplacementDef {
    Hash64 key = 0;
    hash::HashRule rule{};
    std::string ruleString;
    std::string algo; ///< remix.geom.asset / remix.geom.legacy0 / remix.geom.legacy1
    std::string recordId;
    std::optional<bool> preserveOriginalDrawCall;
    scene::CategoryFlags categoriesSet, categoriesCleared;
    std::vector<MeshPartDef> parts;
    std::vector<LightDef> lights; ///< attached lights
    Hash64 fingerprint = 0;       ///< XXH64 of the records read (reload diffing)

    bool preserve() const { return preserveOriginalDrawCall.value_or(false); }
    bool legacy() const { return algo != "remix.geom.asset"; }
};

struct TextureRef {
    std::string param;  ///< MDL parameter (diffuse_texture, normalmap_texture, ...)
    std::string sha256; ///< content address of the DDS blob
    std::string format; ///< RL-3.3 TexFormatInfo name
    std::uint64_t fileBytes = 0;
    std::uint32_t width = 0, height = 0, mips = 0;
    bool srgb = false;
};

struct MaterialDef {
    Hash64 textureHash = 0; ///< mat_<H>: H; 0 for a material only bound by mesh replacements
    std::string recordId;
    std::string name;
    std::string model;   ///< POCO material model
    std::string surface; ///< Opaque / Translucent / Portal
    std::array<double, 4> baseColor{1.0, 1.0, 1.0, 1.0};
    double roughness = 0.5, metallic = 0.0, emissiveNits = 0.0;
    bool ignoreMaterial = false;
    bool preloadTextures = false;
    std::vector<TextureRef> textures; ///< loaded (status ok) textures
    Hash64 fingerprint = 0;
};

struct LightReplacementDef {
    Hash64 hash = 0;
    bool deleted = false;
    std::string primPath;
    LightDef light; ///< unused when deleted
    Hash64 fingerprint = 0;
};

struct ModDiagnostic {
    std::string severity; ///< "warning" | "error"
    std::string code;
    std::string where;
    std::string message;
};

struct ModContent {
    ModLocation location;
    bool ok = false;      ///< false: the mod could not be read at all (no layer, no DB); it contributes nothing
    std::string idPrefix; ///< the import prefix ("mod/<game>/<name>")
    std::string rtxConf;  ///< the mod's rtx.conf text ("" when it has none)
    std::string assetRule;
    std::optional<std::int64_t> priority; ///< `relight.mod.priority` in the mod's rtx.conf
    std::map<std::pair<std::uint32_t, Hash64>, MeshReplacementDef> meshes; ///< (rule bits, key)
    std::map<Hash64, MaterialDef> materials;                              ///< by texture hash
    std::map<std::string, MaterialDef> boundMaterials;                    ///< every material record read, by id
    std::map<Hash64, LightReplacementDef> lights;
    std::vector<ModDiagnostic> diagnostics;
};

/// Store-relative path -> bytes (nullopt when absent).
using StoreReader = std::function<std::optional<std::string>(const std::string& relPath)>;

/// The replacement rows of `db` whose source is `source` ("" : every row), resolved through `read`.
ModContent parseModStore(const ModLocation& location, const StoreReader& read, const capture::exporter::json::Value& db,
                         const std::string& source);

/// A Remix mod directory: mods::import::importMod in memory (game id `gameId`), then parseModStore.
ModContent loadUsdMod(const ModLocation& location, const std::string& gameId);

/// The `source` values of a store directory's DB (sorted); empty when it has no readable DB.
std::vector<std::string> storeSources(const std::string& dir);
/// One store source (location.source) of a store directory.
ModContent loadStoreMod(const ModLocation& location);

/// A mod as its location says (USD or store).
ModContent loadMod(const ModLocation& location, const std::string& gameId);

/// `relight.mod.priority` of an rtx.conf text.
std::optional<std::int64_t> modPriorityFromConf(const std::string& rtxConf);

} // namespace fuse::relight::replace
