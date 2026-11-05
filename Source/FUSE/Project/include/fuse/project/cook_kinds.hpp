#pragma once

// 4d (asset plan §5.1 "The cook manifest gains Material, ..., Anim, ..., Look"): the cook steps behind
// CookAssetKind::Material / Anim / Look. All deterministic (output depends only on the source contents and options).
//
//   Material  `.fusemat.json` (the renderer's schema v1, material_layers/fusemat.hpp: strict, unknown keys / wrong
//             types are errors with a JSON path) or an already cooked binary `.fusemat` -> validated
//             asset::CookedMaterial -> binary `.fusemat` (encodeCookedMaterial, the layout the runtime reads).
//   Anim      assimp clip -> `.fuseanim` (Tools/FUSE/Cook anim_cook). Events come from the JSON sidecar
//             `<source>.events.json` = { "events": [ { "name": "footstep_l", "time": 0.25 }, ... ] } when present.
//   Look      `.fuselook` (renderer look_schema.hpp, schemaVersion 1, kind "fuse.look") -> structurally validated
//             -> canonical JSON (object keys sorted, compact, numbers in shortest round-trip form). The output is
//             still a `.fuselook` the renderer's parser reads; unknown keys are kept (the schema is forward
//             compatible).

#include <fuse/asset/cooked_material.hpp>
#include <fuse/cook/anim_cook.hpp>
#include <fuse/project/asset_cooker.hpp>
#include <fuse/project/import_desc.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace fuse::project {

/// `.fusemat.json` text -> CookedMaterial (validated). False with "json.path: message" in `error`.
bool parseFusematJson(std::string_view text, asset::CookedMaterial& out, std::string* error = nullptr);
CookWriteOutcome cookMaterialFile(const std::string& source_path, const std::string& output_path);

/// Validate a `.fuselook` document and produce its canonical form.
bool canonicalizeLookJson(std::string_view text, std::string& out, std::string* error = nullptr);
CookWriteOutcome cookLookFile(const std::string& source_path, const std::string& output_path);
/// True when `path` is a canonical cooked look (parses, validates and is already canonical).
[[nodiscard]] bool cookedLookLoads(const std::string& path);

/// `<source>.events.json` (the default events sidecar of an anim source).
[[nodiscard]] std::string animEventsSidecarPath(const std::string& source_path);
bool readAnimEventsJson(std::string_view text, std::vector<fuse::cook::AnimCookEvent>& out,
                        std::string* error = nullptr);
CookWriteOutcome cookAnimFile(const AnimImportDesc& desc);

} // namespace fuse::project
