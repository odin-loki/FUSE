/*
* Copyright (c) 2021-2026, NVIDIA CORPORATION. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/
// Modifications Copyright (c) 2026 FUSE contributors (AGPL-3.0)
// Ported from dxvk-remix src/lssusd/game_exporter.cpp@0867d3c (GameExporter::exportUsd: createInstanceStage,
// setCommonStageMetaData, exportMaterials, exportMeshes, exportSkeletons, exportInstances, exportCamera,
// exportSphereLights, exportDistantLights, setTimeSampledXforms, setVisibilityTimeSpan,
// setLightIntensityOnTimeSpan) and src/lssusd/game_exporter_{common,paths}.h@0867d3c (the prim layout).
//
// FUSE Relight RL-1.8: the Remix-compatible capture as USDA text (plan §1.9 layout), written without
// OpenUSD. One layer per asset, referenced from the instance stage, as upstream:
//
//   <stage>.usda                   /RootNode (defaultPrim; customLayerData lightspeed_* incl. the geometry
//                                  hash rule, cameraSettings.boundCamera)
//                                    lights/light_<H>        SphereLight (reference) or DistantLight
//                                    meshes/mesh_<H>         Xform (SkelRoot when skinned), invisible,
//                                                            references ./meshes/mesh_<H>.usda
//                                    Looks/mat_<H>           Material, references ./materials/mat_<H>.usda
//                                    instances/inst_<M>_<n>  (sky_<M>_<n>) internal reference to the mesh,
//                                                            time-sampled xformOp:transform and visibility,
//                                                            mesh/_remix_metadata:* primvars
//                                    cameras/Camera
//   meshes/mesh_<H>.usda           /mesh_<H>/mesh (UsdGeomMesh: points, faceVertexIndices, normals,
//                                  primvars:st, displayColor / displayOpacity, skel primvars, the
//                                  remix_category:* flags; customLayerData = the 9 hash components)
//   materials/mat_<H>.usda         /Looks/mat_<H>/Shader, AperturePBR_Opacity by name, diffuse_texture =
//                                  ../textures/<H>.dds
//   lights/light_<H>.usda          /light_<H> (SphereLight + ShapingAPI)
//   skeletons/skel_<H>.usda        /skel_<H>/skel (Skeleton: joints, bind / rest transforms)
//
// Differences from upstream: every layer is .usda (upstream follows the instance stage's extension, .usd
// by default); every sampled transform is written as a time sample (upstream calls AddTransformOp per
// sample on one prim, which USD rejects after the first); UsdLux attributes carry the "inputs:" prefix of
// USD 21.02+ and MaterialBindingAPI / ShapingAPI / SkelBindingAPI are applied explicitly; the AperturePBR
// MDL modules are referenced by name only (they are NVIDIA's files and are never written by FUSE).
#pragma once

#include <fuse/relight/capture/export/capture_model.hpp>

#include <map>
#include <string>

namespace fuse::relight::capture::exporter {

namespace usd_dir {
inline constexpr const char* kTextures = "textures";
inline constexpr const char* kMeshes = "meshes";
inline constexpr const char* kSkeletons = "skeletons";
inline constexpr const char* kLights = "lights";
inline constexpr const char* kMaterials = "materials";
} // namespace usd_dir

/// The instance stage's file name: meta.stageName + ".usda".
std::string captureStageFileName(const CaptureMeta& meta);
/// textures/<hashToString(h)>.dds (relative to the capture directory).
std::string captureTexturePath(Hash64 textureHash);

/// Every USDA layer of the capture: relative path ('/' separated) -> file contents. Deterministic: the
/// same CaptureData always gives byte-identical text.
std::map<std::string, std::string> writeRemixUsda(const CaptureData& capture);

/// Shortest decimal text that reads back to the same float / double (USDA number formatting).
std::string formatFloat(float v);
std::string formatDouble(double v);

} // namespace fuse::relight::capture::exporter
