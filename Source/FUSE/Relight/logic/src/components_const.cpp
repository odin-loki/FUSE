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
// Ported from dxvk-remix src/dxvk/rtx_render/graph/components/const_{asset_path,bool,color3,color4,float,float2,float3,
// float4,hash,prim,string}.h@0867d3c
//
// FUSE Relight RL-3.5: "Constants" components. Each has one settable-output input and no update logic: other
// nodes connect to `inputs:value` directly (isSettableOutput).
#include "component_list_internal.hpp"
#include "component_macros.hpp"

#include <string>

namespace fuse::relight::logic::components {

namespace {

using PT = PropertyType;

#define LIST_STATES(X)
#define LIST_OUTPUTS(X)

// One constant component per value type; its input list is the macro `<Class>Inputs`.
#define FUSE_LOGIC_CONST_COMPONENT(componentClass, uiNameText, doc)                                                     \
    FUSE_LOGIC_COMPONENT(componentClass, uiNameText, "Constants", doc, 1, componentClass##Inputs, LIST_STATES, LIST_OUTPUTS) \
    void componentClass::updateRange(const LogicContext& /*ctx*/, std::size_t /*start*/, std::size_t /*end*/) {}

#define ConstAssetPathInputs(X) X(PT::AssetPath, std::string(""), value, "Value", "The constant file or asset path.", property.isSettableOutput = true)
FUSE_LOGIC_CONST_COMPONENT(ConstAssetPath, "Constant Asset Path",
                           "Provides a constant file or asset path that you can set.\n\nUse this to provide fixed paths to "
                           "textures, models, configuration files, or other assets.")
#undef ConstAssetPathInputs

#define ConstBoolInputs(X) X(PT::Bool, false, value, "Value", "The constant true/false value.", property.isSettableOutput = true)
FUSE_LOGIC_CONST_COMPONENT(ConstBool, "Constant Bool",
                           "Provides a constant true/false value that you can set.\n\nUse this to provide fixed on/off, yes/no, "
                           "or enabled/disabled values to other components.")
#undef ConstBoolInputs

#define ConstColor3Inputs(X) \
    X(PT::Float3, Vector3(0.0f), value, "Value", "The constant RGB color (Red, Green, Blue).", property.isSettableOutput = true)
FUSE_LOGIC_CONST_COMPONENT(ConstColor3, "Constant Color3",
                           "Provides a constant RGB color that you can set.\n\nUse this to provide fixed colors to materials, "
                           "lights, or other components. Each channel ranges from 0.0 to 1.0.")
#undef ConstColor3Inputs

#define ConstColor4Inputs(X)                                                                                      \
    X(PT::Float4, Vector4(0.0f), value, "Value", "The constant RGBA color (Red, Green, Blue, Alpha).",            \
      property.isSettableOutput = true)
FUSE_LOGIC_CONST_COMPONENT(ConstColor4, "Constant Color4",
                           "Provides a constant RGBA color with transparency that you can set.\n\nUse this for fixed colors "
                           "with transparency. Each channel ranges from 0.0 to 1.0.")
#undef ConstColor4Inputs

#define ConstFloatInputs(X) X(PT::Float, 0.0f, value, "Value", "The constant decimal number.", property.isSettableOutput = true)
FUSE_LOGIC_CONST_COMPONENT(ConstFloat, "Constant Number",
                           "Provides a constant decimal number that you can set.\n\nUse this to provide fixed values like 0.5, "
                           "3.14, or 100.0 to other components.")
#undef ConstFloatInputs

#define ConstFloat2Inputs(X) \
    X(PT::Float2, Vector2(0.0f), value, "Value", "The constant 2D vector (X, Y).", property.isSettableOutput = true)
FUSE_LOGIC_CONST_COMPONENT(ConstFloat2, "Constant Float2",
                           "Provides a constant 2D vector that you can set.\n\nUse this for fixed 2D coordinates, texture "
                           "coordinates, or any pair of values.")
#undef ConstFloat2Inputs

#define ConstFloat3Inputs(X) \
    X(PT::Float3, Vector3(0.0f), value, "Value", "The constant 3D vector (X, Y, Z).", property.isSettableOutput = true)
FUSE_LOGIC_CONST_COMPONENT(ConstFloat3, "Constant Float3",
                           "Provides a constant 3D vector that you can set.\n\nUse this for fixed 3D positions, RGB colors, "
                           "directions, or any set of three values.")
#undef ConstFloat3Inputs

#define ConstFloat4Inputs(X) \
    X(PT::Float4, Vector4(0.0f), value, "Value", "The constant 4D vector (X, Y, Z, W).", property.isSettableOutput = true)
FUSE_LOGIC_CONST_COMPONENT(ConstFloat4, "Constant Float4",
                           "Provides a constant 4D vector that you can set.\n\nUse this for fixed 4D values like quaternions, "
                           "RGBA colors, or any set of four values.")
#undef ConstFloat4Inputs

#define ConstHashInputs(X) X(PT::Hash, 0x0, value, "Value", "The constant hash value.", property.isSettableOutput = true)
FUSE_LOGIC_CONST_COMPONENT(ConstHash, "Constant Hash",
                           "Provides a constant hash value that you can set.\n\nUse this to provide fixed hash identifiers for "
                           "meshes, textures, or other resources. Hash values are typically displayed in hexadecimal format "
                           "(e.g., 0x1A2B3C4D).")
#undef ConstHashInputs

#define ConstPrimInputs(X)                                                                                                 \
    X(PT::Prim, 0, value, "Value", "The constant prim reference.", property.isSettableOutput = true,                       \
      property.allowedPrimTypes = {PrimType::UsdGeomMesh, PrimType::UsdLuxSphereLight, PrimType::UsdLuxCylinderLight,      \
                                   PrimType::UsdLuxDiskLight, PrimType::UsdLuxDistantLight, PrimType::UsdLuxRectLight,     \
                                   PrimType::OmniGraph})
FUSE_LOGIC_CONST_COMPONENT(ConstPrim, "Constant Prim",
                           "Provides a constant reference to a scene object (prim) that you can set.\n\nUse this to provide "
                           "fixed references to meshes, lights, cameras, or other scene elements.")
#undef ConstPrimInputs

#define ConstStringInputs(X) X(PT::String, std::string(""), value, "Value", "The constant text string.", property.isSettableOutput = true)
FUSE_LOGIC_CONST_COMPONENT(ConstString, "Constant String",
                           "Provides a constant text string that you can set.\n\nUse this to provide fixed text values, labels, "
                           "or identifiers to other components.")
#undef ConstStringInputs

#undef FUSE_LOGIC_CONST_COMPONENT
#undef LIST_STATES
#undef LIST_OUTPUTS

} // namespace

void registerConstComponents() {
    ConstAssetPath::registerType();
    ConstBool::registerType();
    ConstColor3::registerType();
    ConstColor4::registerType();
    ConstFloat::registerType();
    ConstFloat2::registerType();
    ConstFloat3::registerType();
    ConstFloat4::registerType();
    ConstHash::registerType();
    ConstPrim::registerType();
    ConstString::registerType();
}

} // namespace fuse::relight::logic::components
