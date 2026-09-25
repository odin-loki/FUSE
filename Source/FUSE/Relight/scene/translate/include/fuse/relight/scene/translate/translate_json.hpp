// FUSE Relight RL-1.5: JSON spelling of TranslateTap's output, shared by the replay tool
// (tests/translate/rl_translate_replay) and the live capture record (tap/capture, CaptureTap), so the
// live-vs-replay check (tests/tap_capture/rl_capture_live.py) compares values, not two writers.
//
// Floats as %.9g (round-trips a float), non-finite floats as the strings "inf" / "-inf" / "nan"; hashes as
// "0x%016x" strings.
//   translatedDrawJson  {"frame":F,"index":I,"status":..,"reason":..,"categories":..,"translated":b,
//                        "texture_stage":b[,"raster_only":true][,"material":{..},"fog":{..},"texgen":..,
//                        "texture_transform":[16],"object_to_view":[16],"clip_plane":b,"clip_plane_eq":[4],
//                        "lights":[..],"viewport":[x,y,w,h],"min_z":..,"max_z":..,"z_write":b,"z_enable":b,
//                        "stencil":b],"camera":..,"alpha_swizzle":b}
//                        (the bracketed fields for translated and raster-only draws only; "raster_only" for the
//                        latter; material.emissive_source "Material" / "VertexColor0" / "VertexColor1")
//   translatedFrameJson {"frame":F,"lights":[..],"rejected_lights":N,"fog":{..},"fog_states":[..],
//                        "cameras":[{..}],"camera_cut":b}
#pragma once

#include <fuse/relight/scene/translate/translate_tap.hpp>

#include <string>

namespace fuse::relight::scene {

std::string translatedDrawJson(const TranslatedDraw& d);
std::string translatedFrameJson(const TranslatedFrame& f);

std::string legacyMaterialJson(const LegacyMaterialRecord& m);
std::string fogRecordJson(const FogRecord& f);
std::string lightRecordJson(const LightRecord& l);
std::string cameraStateJson(const CameraState& c);

} // namespace fuse::relight::scene
