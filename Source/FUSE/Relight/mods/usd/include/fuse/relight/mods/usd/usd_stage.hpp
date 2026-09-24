// FUSE Relight RL-3.1: the USD reader with the Remix profile (docs/plans/FUSE_REMIX_PORT_PLAN.md §4.3).
//
// Two levels:
//   * LayerData: one layer as authored (TinyUSDZ reads USDA and USDC; src/layer_loader.cpp converts its
//     PrimSpecs into these FUSE-owned structs). No composition.
//   * ComposedStage: the "Remix profile" composition implemented by Relight, flattened into prims with
//     resolved properties:
//       - sublayers (layer offsets ignored), recursively, strongest first;
//       - references and payloads to external layers (default prim or an explicit prim path) and internal
//         references, with list-op editing (explicit / prepend / append / add / delete) and namespace mapping
//         of relationship targets and attribute connections;
//       - `over` specifiers merged by strongest opinion (a prim's specifier is its strongest def / class);
//       - variant sets: the strongest authored selection anywhere in the prim's index (optionally fallbacks
//         per set), variant contents nested arbitrarily (a variant may add references, payloads, children and
//         variant sets; TinyUSDZ's USDA parser drops a variantSet written directly inside a variant body, see
//         Engine/lib/tinyusdz/PATCHES.md);
//       - instanceable prims flattened (instances are expanded; `instanceable` is kept as a flag);
//       - inactive prims keep their own opinions but, as on a UsdStage, their subtree is not populated;
//       - asset paths anchored to the directory of the layer that authored them.
//     Strength order is LIVRPS restricted to L, V, R, P (depth first over the arc graph; for siblings of one
//     arc type, direct arcs are stronger than ancestral ones and list order decides among list items).
//     Anything outside the profile (inherits, specializes, relocates, value clips, `reorder` list edits of arcs,
//     non-identity layer offsets on sublayers and references) produces a Diagnostic with the prim path (the
//     layer, for layer-level metadata) and is ignored. Missing / unreadable layers, missing default prims,
//     unresolved arc targets and arc cycles are error Diagnostics; composition continues without that arc.
#pragma once

#include <fuse/relight/mods/usd/usd_value.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fuse::relight::mods::usd {

enum class Specifier : unsigned char { Def, Over, Class };
const char* specifierName(Specifier s);

/// SdfListOp item edit, in authored form.
enum class ListOpKind : unsigned char { Explicit, Prepend, Append, Add, Delete, Order };
const char* listOpKindName(ListOpKind k);

template <typename T>
struct ListOp {
    ListOpKind kind = ListOpKind::Explicit;
    std::vector<T> items;
};

/// A reference or payload item.
struct ArcTarget {
    std::string asset;    ///< authored asset path; empty = internal reference
    std::string primPath; ///< authored target prim path; empty = the target layer's defaultPrim
    bool hasLayerOffset = false; ///< non-identity layer offset authored (ignored by the profile)
    friend bool operator==(const ArcTarget&, const ArcTarget&) = default;
};

struct PropertySpec {
    std::string name;
    bool relationship = false;
    bool custom = false;
    bool uniform = false;
    std::string typeName;                 ///< attributes: "point3f[]", ...; empty for relationships
    bool hasDefault = false;              ///< `= value` (None = a value block) authored
    SharedValue defaultValue;
    bool hasTimeSamples = false;
    std::vector<std::pair<double, SharedValue>> timeSamples;
    ListOpKind targetsOp = ListOpKind::Explicit; ///< relationships: the list edit of `targets`
    bool hasTargets = false;              ///< relationships: targets authored (possibly empty / blocked)
    std::vector<std::string> targets;     ///< relationship targets / attribute connections (paths as authored)
    bool hasConnections = false;
    std::vector<std::pair<std::string, Value>> metadata; ///< interpolation, elementSize, colorSpace, customData, ...
};

struct PrimSpecData {
    std::string name;
    Specifier specifier = Specifier::Def;
    std::string typeName;
    std::vector<ListOp<ArcTarget>> references;
    std::vector<ListOp<ArcTarget>> payloads;
    std::vector<ListOp<std::string>> apiSchemas;
    std::vector<ListOp<std::string>> variantSetNames;
    std::vector<std::pair<std::string, std::string>> variantSelections; ///< `variants = { string set = "sel" }`
    /// variantSet "set" = { "variant" { ... } }: set -> variant -> the variant's prim spec (name = variant).
    std::vector<std::pair<std::string, std::vector<std::pair<std::string, PrimSpecData>>>> variantSets;
    bool hasInherits = false, hasSpecializes = false, hasClips = false, hasRelocates = false;
    std::optional<bool> active;
    std::optional<bool> instanceable;
    std::optional<bool> hidden;
    std::optional<std::string> kind;
    std::vector<std::pair<std::string, Value>> metadata; ///< other prim metadata (customData, doc, ...)
    std::vector<PropertySpec> properties;
    std::vector<PrimSpecData> children;
};

struct LayerData {
    std::string identifier; ///< normalized path ('/' separators) the layer was opened with
    std::string format;     ///< "usda" or "usdc"
    std::string defaultPrim;
    std::vector<std::string> subLayers; ///< as authored, strongest first
    bool subLayerOffsets = false;       ///< a non-identity sublayer offset was authored (ignored)
    bool layerRelocates = false;        ///< layer-level `relocates` authored (outside the profile, ignored)
    std::vector<std::pair<std::string, Value>> metadata; ///< upAxis, metersPerUnit, timeCodesPerSecond, ...
    Value customLayerData;              ///< Dict (None when not authored)
    PrimSpecData pseudoRoot;            ///< children = the root prims (name empty)
};

/// Returns the bytes of a layer (nullopt when it does not exist). The default reads files from disk.
using FileSource = std::function<std::optional<std::string>(const std::string& path)>;
FileSource diskFileSource();
/// An in-memory file system: path -> bytes (paths normalized like layer identifiers).
FileSource memoryFileSource(std::map<std::string, std::string> files);

/// Loads one layer (USDA or USDC, detected by content) through TinyUSDZ. `identifier` is the normalized path.
std::optional<LayerData> loadLayer(const std::string& identifier, std::string_view bytes, std::string* err);

/// TinyUSDZ 0.9.4's USDA parser rejects layer offsets on sublayers (`@a.usd@ (offset = 10; scale = 2)`), which
/// the Remix profile ignores anyway: removes them from the layer header's `subLayers = [...]` in place and
/// returns (offset, scale) per sublayer (empty when nothing was removed). loadLayer applies it to every USDA.
std::vector<std::pair<double, double>> stripSubLayerOffsets(std::string& usdaText);

struct Diagnostic {
    enum class Severity : unsigned char { Warning, Error };
    Severity severity = Severity::Warning;
    std::string code;     ///< stable id: "inherits", "missing_layer", "reference_cycle", ...
    std::string primPath; ///< composed prim path ("" for layer-level issues)
    std::string layer;    ///< layer identifier involved
    std::string message;
};

struct Attribute {
    std::string typeName;
    bool custom = false;
    bool uniform = false;
    bool hasDefault = false; ///< a value (or None) was authored in some layer
    SharedValue defaultValue; ///< strongest default opinion (None when blocked or declared only), shared with the layer
    std::vector<std::pair<double, SharedValue>> timeSamples; ///< strongest time-sample opinion
    std::vector<std::string> connections;              ///< composed, mapped to stage namespace
    std::vector<std::pair<std::string, Value>> metadata; ///< strongest opinion per key
    std::string definingLayer; ///< layer of the strongest spec with a value / samples (else strongest spec)
};

struct Relationship {
    bool custom = false;
    std::vector<std::string> targets; ///< composed list op, mapped to stage namespace
};

/// One contributing prim spec, strongest first (the "prim stack").
struct SpecSite {
    std::string layer; ///< layer identifier
    std::string path;  ///< spec path in that layer ("/a/b{set=v}c" for variant contents)
    std::vector<std::string> propertyNames;
};

struct Prim {
    std::string path; ///< "/RootNode/meshes/mesh_0123..."
    std::string name;
    std::string typeName;
    Specifier specifier = Specifier::Over;
    bool active = true;
    bool instanceable = false;
    std::string kind;
    std::vector<std::string> apiSchemas; ///< composed list op
    std::map<std::string, std::string> variantSelections; ///< selections in effect (set -> variant)
    std::map<std::string, std::vector<std::string>> variantSets; ///< available variants per set
    std::map<std::string, Attribute> attributes;
    std::map<std::string, Relationship> relationships;
    std::vector<std::pair<std::string, Value>> metadata; ///< strongest opinion per key (customData, ...)
    std::vector<std::string> children; ///< child names, composed order
    std::vector<SpecSite> stack;

    bool hasApiSchema(std::string_view name) const;
    const Attribute* attribute(std::string_view name) const;
    const Relationship* relationship(std::string_view name) const;
};

struct ReadOptions {
    FileSource files;           ///< default: diskFileSource()
    bool loadPayloads = true;
    /// Variant fallbacks when no selection is authored: set -> preferred variants (first existing wins).
    std::map<std::string, std::vector<std::string>> variantFallbacks;
    std::uint32_t maxArcDepth = 64; ///< reference / payload nesting limit (cycles are detected separately)
};

struct ComposedStage {
    std::string rootLayer; ///< identifier of the root layer
    std::vector<std::string> layers; ///< every layer that was opened, in load order
    std::string defaultPrim;
    std::vector<std::pair<std::string, Value>> layerMetadata; ///< the root layer's metadata
    Value customLayerData;                                    ///< the root layer's customLayerData
    std::vector<std::string> rootPrims;                       ///< root prim names, composed order
    std::map<std::string, Prim> prims;                        ///< by path (pseudo-root excluded)
    std::vector<Diagnostic> diagnostics;
    bool ok = false; ///< root layer loaded (errors in other layers are diagnostics)

    const Prim* find(std::string_view path) const;
    std::size_t count(Diagnostic::Severity s) const;
    /// Every prim below `path` (depth first, composed child order), not including `path` itself.
    std::vector<const Prim*> descendants(std::string_view path) const;
};

/// Opens `rootLayerPath` and composes it with the Remix profile.
ComposedStage readStage(const std::string& rootLayerPath, const ReadOptions& options = {});

/// Canonical flattened dump: one JSON object per line, prims sorted by path, properties sorted by name;
/// asset paths relative to the root layer's directory. Deterministic and independent of the layer format
/// (USDA vs USDC), so both parse paths and the usd-core cross-check compare equal.
std::string canonicalDump(const ComposedStage& stage, bool includeDiagnostics = true);

} // namespace fuse::relight::mods::usd
