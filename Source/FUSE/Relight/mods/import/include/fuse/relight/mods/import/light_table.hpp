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
// Modifications Copyright (c) 2026 FUSE contributors (MIT)
// Ported from dxvk-remix src/dxvk/rtx_render/rtx_lights_data.h@0867d3c (LIST_LIGHT_CONSTANTS: USD token, type,
// range, default) and the LightData USD facts of rtx_lights_data.cpp@0867d3c (the supported UsdLux types; every
// attribute is read as `inputs:<token>` with the pre-USD-21.02 bare `<token>` as a fallback).
//
// FUSE Relight RL-3.2: UsdLux lights of a mod -> POCO `Light` (Remaster §2.3) + the Relight extension fields
// (plan §4.4: shaping cone / softness / focus, volumetric radiance scale, the cylinder type).
//
// Units: values stay in USD authoring units (angles in degrees, as authored; upstream converts to radians when
// it builds the RtLight). The cone default is 180 degrees (upstream: 180 * kDegreesToRadians). FUSE mapping of the
// POCO fields: type SphereLight -> Point (Spot when the cone angle is below 180), DistantLight -> Directional,
// RectLight -> Rect, DiskLight -> Disk, CylinderLight -> Point with relight.usd_type "CylinderLight", DomeLight ->
// Dome (not a Remix light type: reported); colorLinear = color; intensity = intensity * 2^exposure (Remix units);
// outerCone = cone angle and innerCone = cone angle * (1 - softness), both in radians; size = width x height
// (rect), 2r x 2r (disk), length x 2r (cylinder).
#pragma once

#include <fuse/relight/capture/export/json.hpp>
#include <fuse/relight/mods/usd/usd_stage.hpp>

#include <array>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fuse::relight::mods::import {

struct LightParamDesc {
    std::string_view name; ///< USD token without "inputs:"
    bool vec3 = false;
    bool boolean = false;
    std::array<float, 3> minValue{};
    std::array<float, 3> maxValue{};
    std::array<float, 3> defaultValue{};
};

/// LIST_LIGHT_CONSTANTS in upstream order.
std::span<const LightParamDesc> lightParamTable();

struct LightParams {
    std::string usdType; ///< SphereLight, DistantLight, RectLight, DiskLight, CylinderLight, DomeLight
    std::map<std::string, std::array<float, 3>> values; ///< every table parameter (defaults filled in)
    std::set<std::string> authored;
    friend bool operator==(const LightParams&, const LightParams&) = default;
};

bool isUsdLightType(std::string_view typeName);
/// False for types the Remix light path does not create (DomeLight).
bool isRemixLightType(std::string_view typeName);

/// Reads the table parameters of a light prim (clamped to the upstream ranges; clamps are reported).
LightParams readLightParams(const usd::Prim& light, std::vector<std::string>* issues);

/// The POCO Light payload: type, colorLinear, intensity, range, innerCone, outerCone, size, castsShadows and
/// `relight` (usd_type + every table parameter by token).
capture::exporter::json::Value lightPayload(const LightParams& p);

/// Inverse of lightPayload's `relight` block.
std::optional<LightParams> lightParamsFromPoco(const capture::exporter::json::Value& payload, std::string* error = nullptr);

} // namespace fuse::relight::mods::import
