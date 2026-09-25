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
// Ported from dxvk-remix src/dxvk/rtx_render/rtx_lights_data.h@0867d3c (LIST_LIGHT_CONSTANTS). See light_table.hpp.
#include <fuse/relight/mods/import/light_table.hpp>

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace fuse::relight::mods::import {

namespace json = capture::exporter::json;

namespace {

constexpr float kF16Max = 65504.0f;
using V3 = std::array<float, 3>;
constexpr V3 s(float v) { return {v, 0.f, 0.f}; }
constexpr LightParamDesc f(std::string_view n, float lo, float hi, float def) { return {n, false, false, s(lo), s(hi), s(def)}; }

// LIST_LIGHT_CONSTANTS (angles in USD degrees; upstream stores radians after reading).
constexpr LightParamDesc kLight[] = {
    f("radius", 0.f, kF16Max, 0.f),
    f("width", 0.f, FLT_MAX, 0.f),
    f("height", 0.f, FLT_MAX, 0.f),
    f("length", 0.f, FLT_MAX, 0.f),
    f("angle", -FLT_MAX, FLT_MAX, 0.f),
    {"enableColorTemperature", false, true, s(0.f), s(1.f), s(0.f)},
    {"color", true, false, {0.f, 0.f, 0.f}, {1.f, 1.f, 1.f}, {1.f, 1.f, 1.f}},
    f("colorTemperature", 0.f, FLT_MAX, 6500.f),
    f("exposure", -FLT_MAX, FLT_MAX, 0.f),
    f("intensity", 0.f, FLT_MAX, 1.f),
    f("shaping:cone:angle", -FLT_MAX, FLT_MAX, 180.f),
    f("shaping:cone:softness", 0.f, kF16Max, 0.f),
    f("shaping:focus", 0.f, kF16Max, 0.f),
    f("volumetric_radiance_scale", 0.f, kF16Max, 1.f),
};

constexpr double kDegToRad = 3.14159265358979323846 / 180.0;

} // namespace

std::span<const LightParamDesc> lightParamTable() { return kLight; }

bool isUsdLightType(std::string_view t) {
    return t == "SphereLight" || t == "DistantLight" || t == "RectLight" || t == "DiskLight" || t == "CylinderLight" ||
           t == "DomeLight";
}

bool isRemixLightType(std::string_view t) { return isUsdLightType(t) && t != "DomeLight"; }

LightParams readLightParams(const usd::Prim& light, std::vector<std::string>* issues) {
    LightParams p;
    p.usdType = light.typeName;
    for (const LightParamDesc& d : kLight) {
        const std::string name(d.name);
        p.values[name] = d.defaultValue;
        const usd::Attribute* a = light.attribute("inputs:" + name);
        if (!a || !a->hasDefault) {
            if (const usd::Attribute* legacy = light.attribute(name); legacy && legacy->hasDefault) {
                a = legacy;
            }
        }
        if (!a || !a->hasDefault || a->defaultValue->isNone()) {
            continue;
        }
        p.authored.insert(name);
        const usd::Value& v = a->defaultValue;
        V3 out = d.defaultValue;
        if (d.vec3) {
            const auto n = v.asNumbers();
            if (!n || n->size() != 3) {
                if (issues) {
                    issues->push_back(light.path + ": " + name + " expects 3 numbers");
                }
                continue;
            }
            out = {static_cast<float>((*n)[0]), static_cast<float>((*n)[1]), static_cast<float>((*n)[2])};
        } else if (d.boolean) {
            const auto bv = v.asBool();
            if (!bv) {
                if (issues) {
                    issues->push_back(light.path + ": " + name + " expects a bool");
                }
                continue;
            }
            out = s(*bv ? 1.f : 0.f);
        } else {
            const auto n = v.asNumber();
            if (!n || std::isnan(*n)) {
                if (issues) {
                    issues->push_back(light.path + ": " + name + " expects a number");
                }
                continue;
            }
            out = s(static_cast<float>(std::clamp(*n, -double(FLT_MAX), double(FLT_MAX))));
        }
        bool clamped = false;
        for (std::size_t k = 0; k < (d.vec3 ? 3u : 1u); ++k) {
            const float c = std::clamp(out[k], d.minValue[k], d.maxValue[k]);
            clamped |= c != out[k];
            out[k] = c;
        }
        if (clamped && issues) {
            issues->push_back(light.path + ": " + name + " clamped to the Remix range");
        }
        p.values[name] = out;
    }
    return p;
}

json::Value lightPayload(const LightParams& p) {
    auto val = [&](const char* n) { return p.values.at(n)[0]; };
    json::Value o = json::Value::object();
    const float cone = val("shaping:cone:angle");
    std::string type = "Point";
    if (p.usdType == "DistantLight") {
        type = "Directional";
    } else if (p.usdType == "RectLight") {
        type = "Rect";
    } else if (p.usdType == "DiskLight") {
        type = "Disk";
    } else if (p.usdType == "DomeLight") {
        type = "Dome";
    } else if (cone < 180.f) {
        type = "Spot";
    }
    o["type"] = json::Value::string(type);
    json::Value color = json::Value::array();
    for (float c : p.values.at("color")) {
        color.push(json::Value::number(double(c)));
    }
    o["colorLinear"] = std::move(color);
    o["intensity"] = json::Value::number(double(val("intensity")) * std::exp2(double(val("exposure"))));
    o["range"] = json::Value::number(0);
    const double outer = std::min(double(cone), 180.0) * kDegToRad;
    const double soft = std::clamp(double(val("shaping:cone:softness")), 0.0, 1.0);
    o["innerCone"] = json::Value::number(type == "Spot" ? outer * (1.0 - soft) : 0.0);
    o["outerCone"] = json::Value::number(type == "Spot" ? outer : 0.0);
    json::Value size = json::Value::array();
    double sx = 0, sy = 0;
    if (p.usdType == "RectLight") {
        sx = val("width");
        sy = val("height");
    } else if (p.usdType == "DiskLight") {
        sx = sy = 2.0 * double(val("radius"));
    } else if (p.usdType == "CylinderLight") {
        sx = val("length");
        sy = 2.0 * double(val("radius"));
    }
    size.push(json::Value::number(sx));
    size.push(json::Value::number(sy));
    o["size"] = std::move(size);
    o["castsShadows"] = json::Value::boolean(true);
    json::Value rl = json::Value::object();
    rl["usd_type"] = json::Value::string(p.usdType);
    rl["intensity_units"] = json::Value::string("remix");
    json::Value params = json::Value::object();
    for (const LightParamDesc& d : kLight) {
        const V3& v = p.values.at(std::string(d.name));
        if (d.vec3) {
            json::Value a = json::Value::array();
            for (float c : v) {
                a.push(json::Value::number(double(c)));
            }
            params[d.name] = std::move(a);
        } else if (d.boolean) {
            params[d.name] = json::Value::boolean(v[0] != 0.f);
        } else {
            params[d.name] = json::Value::number(double(v[0]));
        }
    }
    rl["params"] = std::move(params);
    json::Value authored = json::Value::array();
    for (const std::string& a : p.authored) {
        authored.push(json::Value::string(a));
    }
    rl["authored"] = std::move(authored);
    o["relight"] = std::move(rl);
    return o;
}

std::optional<LightParams> lightParamsFromPoco(const json::Value& payload, std::string* error) {
    auto fail = [&](const std::string& why) -> std::optional<LightParams> {
        if (error) {
            *error = why;
        }
        return std::nullopt;
    };
    const json::Value* rl = payload.get("relight");
    const json::Value* params = rl ? rl->get("params") : nullptr;
    if (!params || !params->isObject()) {
        return fail("light payload has no relight.params");
    }
    LightParams p;
    p.usdType = rl->str("usd_type");
    for (const LightParamDesc& d : kLight) {
        const json::Value* v = params->get(d.name);
        if (!v) {
            return fail("relight.params." + std::string(d.name) + " is missing");
        }
        V3 out{};
        if (d.vec3) {
            if (!v->isArray() || v->a.size() != 3) {
                return fail("relight.params." + std::string(d.name) + " is not a 3-vector");
            }
            for (std::size_t k = 0; k < 3; ++k) {
                out[k] = static_cast<float>(v->a[k].n);
            }
        } else if (d.boolean) {
            out = s(v->b ? 1.f : 0.f);
        } else {
            out = s(static_cast<float>(v->n));
        }
        p.values[std::string(d.name)] = out;
    }
    if (const json::Value* a = rl->get("authored"); a && a->isArray()) {
        for (const json::Value& n : a->a) {
            p.authored.insert(n.s);
        }
    }
    return p;
}

} // namespace fuse::relight::mods::import
