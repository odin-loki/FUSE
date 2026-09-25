// FUSE Relight RL-1.7: building SceneDrawInput from the Wave R1 capture packages.
//
//   RL-1.5 TranslatedDraw  classification (categories, raytraced render target), legacy material (hash and
//                          identity hash), transforms (objectToWorld, texture transform, texgen), camera type;
//   RL-1.3 CapturedDraw    geometry hash components, bounding box, skinning (bone hash / count), and the
//                          asset hash under rtx.geometryAssetHashRuleString.
// The render-material facts (material type, subsurface) default to a legacy opaque material: they come from
// replacements (RL-4.x).
#pragma once

#include <fuse/relight/scene/instances/scene_draw.hpp>
#include <fuse/relight/scene/translate/translate_tap.hpp>

namespace fuse::relight::capture::geometry {
struct CapturedDraw;
struct BoundingBox;
}

namespace fuse::relight::scene::instances {

/// The TranslatedDraw half (geometry left empty).
SceneDrawInput sceneDrawInput(const TranslatedDraw& draw);

/// Converts RL-1.3's bounding box.
AxisAlignedBoundingBox toBoundingBox(const capture::geometry::BoundingBox& box);

/// TranslatedDraw + CapturedDraw (waits for the draw's hash, bounding box and skinning jobs). `assetRule`:
/// the parsed rtx.geometryAssetHashRuleString.
SceneDrawInput sceneDrawInput(const TranslatedDraw& draw, const capture::geometry::CapturedDraw& geometry,
                              hash::HashRule assetRule);

} // namespace fuse::relight::scene::instances
