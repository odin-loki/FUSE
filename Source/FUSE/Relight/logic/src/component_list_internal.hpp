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
// Modifications Copyright (c) 2026 FUSE contributors (MIT)
// Ported from dxvk-remix src/dxvk/rtx_render/graph/rtx_component_list.h@0867d3c and rtx_component_list.cpp@0867d3c
//
// FUSE Relight RL-3.5: the per-group registration entry points (component_list.cpp calls them all) and the
// variant lists of the flexible components (upstream INSTANTIATE_ANY_TYPES / INSTANTIATE_NUMBER_OR_VECTOR_TYPES).
#pragma once

#include <fuse/relight/logic/graph_types.hpp>

namespace fuse::relight::logic::components {

void registerConstComponents();
void registerMathComponents();
void registerCompareComponents();
void registerStateComponents();
void registerSenseComponents();
void registerOptionComponents();

/// Any: Float, Float2, Float3, Float4, Bool, Enum, Hash, Prim, String (upstream INSTANTIATE_ANY_TYPES).
template <template <PropertyType> typename Component>
void registerAnyVariants() {
    using PT = PropertyType;
    Component<PT::Float>::registerType();
    Component<PT::Float2>::registerType();
    Component<PT::Float3>::registerType();
    Component<PT::Float4>::registerType();
    Component<PT::Bool>::registerType();
    Component<PT::Enum>::registerType();
    Component<PT::Hash>::registerType();
    Component<PT::Prim>::registerType();
    Component<PT::String>::registerType();
}

/// NumberOrVector: Float, Float2, Float3, Float4 (upstream INSTANTIATE_NUMBER_OR_VECTOR_TYPES).
template <template <PropertyType> typename Component>
void registerNumberOrVectorVariants() {
    using PT = PropertyType;
    Component<PT::Float>::registerType();
    Component<PT::Float2>::registerType();
    Component<PT::Float3>::registerType();
    Component<PT::Float4>::registerType();
}

} // namespace fuse::relight::logic::components
