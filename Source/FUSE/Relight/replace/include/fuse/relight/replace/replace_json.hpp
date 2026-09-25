// FUSE Relight RL-3.4: the replaced scene in the capture record (schema fuse.relight.capture/1 additions).
//
// With runtime replacements active, CaptureTap's record (tap/capture/capture_tap.hpp) carries:
//   draw lines   "replacement": null for a committed draw nothing applies to, else
//                  {"instance", "mesh": null | {"mod", "record", "key", "rule", "preserve", "shadowed",
//                   "parts": [{"mesh", "material", "material_mod", "material_source", "translation"}]},
//                   "draw_original", "material": null | {"mod", "record", "ignore", "shadowed"},
//                   "categories" (after the replacement's overrides), "lights": [light]}
//                (draws that are not committed carry no "replacement" member);
//   replace_frame  one line per presented frame, after the frame's draws:
//                  {"ev": "replace_frame", "frame", "generation", "watch", "mods": [{"name", "kind", "format",
//                   "rank", "priority", "layer", "meshes", "materials", "lights", "deleted_lights", "errors"}],
//                   "stats": {...}, "lights": [light], "deleted_lights": [hash],
//                   "reload": null | {"generation", "notified_frame", "applied_frame", "latency_frames", "changed",
//                   "added", "removed", "failed", "invalidated": {"meshes", "materials", "lights"}},
//                   "residency": {...}}
//   light        {"origin": game | replaced | attached, "hash", "mod", "record", "type", "position",
//                 "direction", "color", "intensity", "instance", "draw"}
// Hashes are 16 upper-case hex digits (hashToString, the mod prim names' spelling). Numbers are rounded to 6
// significant digits so the record does not depend on the last bits of float arithmetic.
#pragma once

#include <fuse/relight/capture/export/json.hpp>
#include <fuse/relight/replace/replacement_engine.hpp>

#include <string>

namespace fuse::relight::replace {

capture::exporter::json::Value replacedDrawJson(const ReplacedDraw& draw);
capture::exporter::json::Value replacedLightJson(const ReplacedLight& light);
/// The replace_frame line's members (without "ev").
capture::exporter::json::Value replacedFrameJson(const ReplacedFrame& frame, const std::vector<ModInfo>& mods,
                                                 const std::string& watchBackend);

} // namespace fuse::relight::replace
