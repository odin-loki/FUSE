#pragma once

// `.fuselook` — FUSE's versioned look-profile text schema (JSON, same conventions as project.json:
// `schemaVersion` + camelCase keys). One file describes a blend space:
//
//   {
//     "schemaVersion": 1,
//     "kind": "fuse.look",
//     "name": "filmic_warm",
//     "description": "...",
//     "graph": ["depthOfField", "motionBlur", "bloom", ..., "outputTransform"],   // optional order
//     "base": { "<node>": { "<param>": value, ... }, ... },                         // overrides of the schema defaults
//     "timeOfDay": { "interpolation": "linear" | "smooth",
//                    "keys": [ { "hour": 6.5, "settings": { ... } }, ... ] },       // 24 h wraparound
//     "weather": [ { "name": "storm", "settings": { ... } }, ... ],
//     "volumes": [ { "name": "cave", "shape": "sphere" | "box", "center": [x, y, z],
//                    "radius": r | "halfExtents": [x, y, z], "falloff": m, "priority": p,
//                    "weight": w, "settings": { ... } }, ... ],
//     "overrides": [ { "name": "underwater", "settings": { ... } }, ... ]          // gameplay overrides
//   }
//
// A settings block is `{ "<node>": { "<param>": value } }` using the keys of look_params.hpp. Values:
// number (float/int), true/false (bool), [r, g, b] (colour), "name" (enum). `colorGrade.lut` names a
// `.cube` file relative to the look file, or null for "no external LUT" (identity).
//
// Versioning: `schemaVersion` is required; files newer than kLookSchemaVersion are rejected, unknown
// keys are reported as warnings (forward compatible), type errors are hard errors with a JSON path.

#include <fuse/math/vec.hpp>
#include <fuse/renderer/look/effect_graph.hpp>
#include <fuse/renderer/look/look_params.hpp>
#include <fuse/types.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace fuse::renderer::look {

inline constexpr u32 kLookSchemaVersion = 1;
inline constexpr const char* kLookSchemaKind = "fuse.look";
inline constexpr const char* kLookFileExtension = ".fuselook";

/// Sparse parameter overrides (one profile).
struct LookSettings {
    LookParamBlock values{}; ///< meaningful where `mask` is set
    LookParamMask mask{};
    bool lut_set = false;    ///< colorGrade.lut present
    std::string lut_path;    ///< empty with lut_set = identity (null)

    bool operator==(const LookSettings& o) const;
};

enum class LookTimeInterpolation : u8 { Linear = 0, Smooth = 1 };

struct LookTimeKey {
    f32 hour = 0.f; ///< [0, 24)
    LookSettings settings;
};

struct LookWeatherState {
    std::string name;
    LookSettings settings;
};

enum class LookVolumeShape : u8 { Sphere = 0, Box = 1 };

struct LookVolumeDesc {
    std::string name;
    LookVolumeShape shape = LookVolumeShape::Sphere;
    math::Vec3 center{};
    f32 radius = 1.f;              ///< sphere
    math::Vec3 half_extents{1.f, 1.f, 1.f}; ///< box (axis-aligned)
    f32 falloff = 0.f;             ///< metres outside the shape over which the weight fades to 0
    s32 priority = 0;              ///< higher priority is applied later (wins)
    f32 weight = 1.f;              ///< [0, 1]
    LookSettings settings;
};

struct LookDocument {
    u32 schema_version = kLookSchemaVersion;
    std::string name;
    std::string description;
    bool has_graph = false;
    LookEffectGraph graph = LookEffectGraph::makeDefault();
    LookSettings base;
    LookTimeInterpolation time_interpolation = LookTimeInterpolation::Linear;
    std::vector<LookTimeKey> time_keys;
    std::vector<LookWeatherState> weather;
    std::vector<LookVolumeDesc> volumes;
    std::vector<LookWeatherState> overrides;

    bool operator==(const LookDocument& o) const;
};

struct LookParseResult {
    bool ok = false;
    std::string error;                 ///< "<json path>: message" (or "line:col: message" for syntax)
    std::vector<std::string> warnings; ///< unknown keys etc.
};

bool look_parse(std::string_view text, LookDocument& out, LookParseResult& result);
bool look_load_file(const char* path, LookDocument& out, LookParseResult& result);
/// Canonical text (stable key order, %.9g numbers, only overridden parameters). parse(write(d)) == d.
std::string look_write(const LookDocument& doc);

} // namespace fuse::renderer::look
