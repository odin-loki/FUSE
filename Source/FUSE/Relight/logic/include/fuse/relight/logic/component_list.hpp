/*
* Copyright (c) 2025, NVIDIA CORPORATION. All rights reserved.
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
// Ported from dxvk-remix src/dxvk/rtx_render/graph/rtx_component_list.h@0867d3c
//
// FUSE Relight RL-3.5: the component catalogue. 68 components (upstream's full list at 0867d3c):
//   Constants  ConstAssetPath ConstBool ConstColor3 ConstColor4 ConstFloat ConstFloat2 ConstFloat3 ConstFloat4
//              ConstHash ConstPrim ConstString
//   Transform  Add Subtract Multiply Divide (NumberOrVector operand pairs) Clamp Min Max Invert Normalize
//              VectorLength Floor Ceil Round ComposeVector2/3/4 DecomposeVector2/3/4 EqualTo LessThan GreaterThan
//              Between BoolAnd BoolOr BoolNot Select ConditionallyStore PreviousFrameValue Counter Toggle
//              CountToggles Smooth Velocity Loop Remap (old name InterpolateFloat)
//   Sense      Time Camera KeyboardInput MeshHashChecker TextureHashChecker LightHashChecker FogHashChecker
//              MeshProximity RayMeshIntersection AngleToMesh ReadTransform ReadBoneTransform RtxOptionLayerSensor
//              RtxOptionReadBool RtxOptionReadNumber RtxOptionReadVector2 RtxOptionReadVector3 RtxOptionReadColor3
//              RtxOptionReadColor4
//   Act        RtxOptionLayerAction
//   (TODO)     SphereLightOverride (non-functional upstream; kept so graphs using it load)
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fuse::relight::logic {

/// Registers every component (and every variant of the flexible ones). Idempotent and thread-safe; the parser and
/// runtime call it, so explicit calls are only needed before looking specs up directly.
void registerAllComponents();

/// A dynamic option layer referenced by RtxOptionLayerAction instances.
struct HeldOptionLayer {
    std::string configPath;
    std::uint32_t priority = 0;
    std::size_t references = 0; ///< component instances holding it
    bool enabled = false;
    float blendStrength = 0.0f;
    float blendThreshold = 0.0f;
};
/// The layers graph components hold now, by (priority, path) key order.
std::vector<HeldOptionLayer> heldOptionLayers();
/// Drops every reference graph components hold (tests; GraphManager::clear releases per instance).
void releaseAllHeldOptionLayers();

} // namespace fuse::relight::logic
