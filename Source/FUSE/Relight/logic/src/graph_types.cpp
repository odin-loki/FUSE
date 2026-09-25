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
// Ported from dxvk-remix src/dxvk/rtx_render/graph/rtx_graph_types.cpp@0867d3c
#include <fuse/relight/logic/graph_types.hpp>
#include <fuse/relight/logic/logic_context.hpp>
#include <fuse/relight/logic/logic_log.hpp>

#include <fuse/relight/hash/xxh.hpp>

#include <cerrno>
#include <cinttypes>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <set>

namespace fuse::relight::logic {

namespace {

struct Registry {
    std::mutex mutex;
    std::map<ComponentType, std::vector<const ComponentSpec*>> variants;
};
Registry& registry() {
    static Registry r;
    return r;
}

/// Upstream parseVector: skip to '(' when present, then read N floats separated by whitespace / commas.
template <typename T, std::size_t N>
PropertyValue parseVector(const std::string& input) {
    if (input.empty() || input.size() > 1024) {
        logMessage(LogSeverity::Error, "parseVector: invalid input '" + input + "'");
        return T(0.f);
    }
    std::size_t start = input.find('(');
    start = start == std::string::npos ? 0 : start + 1;
    if (start >= input.size()) {
        logMessage(LogSeverity::Error, "parseVector: Invalid input format - empty after opening parenthesis");
        return T(0.f);
    }
    const char* inputEnd = input.c_str() + input.size();
    const char* ptr = input.c_str() + start;
    T result(0.f);
    for (std::size_t i = 0; i < N; i++) {
        if (ptr >= inputEnd) {
            logMessage(LogSeverity::Error, "parseVector: Unexpected end of string while parsing component " + std::to_string(i) +
                                               " from: " + input);
            return T(0.f);
        }
        char* endptr = nullptr;
        errno = 0;
        const float value = std::strtof(ptr, &endptr);
        if (endptr == ptr || errno == ERANGE) {
            logMessage(LogSeverity::Error, "parseVector: Failed to parse component " + std::to_string(i) + " from string: `" + input + "`");
            return T(0.f);
        }
        result[i] = value;
        ptr = endptr;
        while (ptr < inputEnd && (std::isspace(static_cast<unsigned char>(*ptr)) || *ptr == ',')) {
            ptr++;
        }
    }
    return result;
}

std::string formatFloat(float f) {
    char buf[64];
    // Shortest text that parses back to the same float.
    const auto res = std::to_chars(buf, buf + sizeof(buf), f);
    return std::string(buf, res.ptr);
}

} // namespace

const char* ioTypeName(PropertyIOType t) {
    switch (t) {
    case PropertyIOType::Input: return "Input";
    case PropertyIOType::State: return "State";
    case PropertyIOType::Output: return "Output";
    }
    return "?";
}

const char* propertyTypeName(PropertyType t) {
    switch (t) {
    case PropertyType::Bool: return "Bool";
    case PropertyType::Float: return "Float";
    case PropertyType::Float2: return "Float2";
    case PropertyType::Float3: return "Float3";
    case PropertyType::Float4: return "Float4";
    case PropertyType::Enum: return "Enum";
    case PropertyType::String: return "String";
    case PropertyType::AssetPath: return "AssetPath";
    case PropertyType::Hash: return "Hash";
    case PropertyType::Prim: return "Prim";
    case PropertyType::Any: return "Any";
    case PropertyType::NumberOrVector: return "NumberOrVector";
    }
    return "?";
}

bool isFlexibleType(PropertyType t) { return t == PropertyType::Any || t == PropertyType::NumberOrVector; }

const char* primTypeName(PrimType t) {
    switch (t) {
    case PrimType::UsdGeomMesh: return "UsdGeomMesh";
    case PrimType::UsdLuxSphereLight: return "UsdLuxSphereLight";
    case PrimType::UsdLuxCylinderLight: return "UsdLuxCylinderLight";
    case PrimType::UsdLuxDiskLight: return "UsdLuxDiskLight";
    case PrimType::UsdLuxDistantLight: return "UsdLuxDistantLight";
    case PrimType::UsdLuxRectLight: return "UsdLuxRectLight";
    case PrimType::OmniGraph: return "OmniGraph";
    }
    return "";
}

PropertyValue propertyValueFromString(const std::string& str, PropertyType type) {
    switch (type) {
    case PropertyType::Bool:
        return (str == "true" || str == "True" || str == "TRUE" || str == "1") ? kTruePropertyValue : kFalsePropertyValue;
    case PropertyType::Float: {
        char* end = nullptr;
        errno = 0;
        const float v = std::strtof(str.c_str(), &end);
        if (end == str.c_str() || errno == ERANGE) {
            break;
        }
        return v;
    }
    case PropertyType::Float2: return parseVector<Vector2, 2>(str);
    case PropertyType::Float3: return parseVector<Vector3, 3>(str);
    case PropertyType::Float4: return parseVector<Vector4, 4>(str);
    case PropertyType::Enum: {
        // std::stoul semantics: leading whitespace, optional sign, base 10; out of range for uint32 is an error.
        char* end = nullptr;
        errno = 0;
        const unsigned long long v = std::strtoull(str.c_str(), &end, 10);
        if (end == str.c_str() || errno == ERANGE || v > 0xFFFFFFFFull) {
            break;
        }
        return PropertyValue(std::in_place_type<std::uint32_t>, static_cast<std::uint32_t>(v));
    }
    case PropertyType::String:
    case PropertyType::AssetPath:
        return str;
    case PropertyType::Hash: {
        // Hex with or without 0x (std::stoull(str, nullptr, 16)).
        char* end = nullptr;
        errno = 0;
        const unsigned long long v = std::strtoull(str.c_str(), &end, 16);
        if (end == str.c_str() || errno == ERANGE) {
            break;
        }
        return PropertyValue(std::in_place_type<std::uint64_t>, static_cast<std::uint64_t>(v));
    }
    case PropertyType::Prim:
        return kInvalidPropertyValue;
    case PropertyType::Any:
    case PropertyType::NumberOrVector:
        logMessage(LogSeverity::Error, std::string("Flexible types (Any, NumberOrVector) cannot be parsed from strings directly. "
                                                   "type: ") + propertyTypeName(type) + ", string: " + str);
        return kInvalidPropertyValue;
    }
    logMessage(LogSeverity::Error, std::string("propertyValueFromString: cannot convert '") + str + "' to " + propertyTypeName(type));
    return kInvalidPropertyValue;
}

PropertyVector propertyVectorFromType(PropertyType type) {
    switch (type) {
    case PropertyType::Bool:
    case PropertyType::Enum: return std::vector<std::uint32_t>{};
    case PropertyType::Float: return std::vector<float>{};
    case PropertyType::Float2: return std::vector<Vector2>{};
    case PropertyType::Float3: return std::vector<Vector3>{};
    case PropertyType::Float4: return std::vector<Vector4>{};
    case PropertyType::String:
    case PropertyType::AssetPath: return std::vector<std::string>{};
    case PropertyType::Hash: return std::vector<std::uint64_t>{};
    case PropertyType::Prim: return std::vector<PrimTarget>{};
    case PropertyType::Any:
    case PropertyType::NumberOrVector:
        logMessage(LogSeverity::Error, "Flexible types (Any, NumberOrVector) cannot be used to create property vectors directly.");
        return std::vector<float>{};
    }
    return std::vector<float>{};
}

bool valueMatchesType(const PropertyValue& value, PropertyType type) {
    switch (type) {
    case PropertyType::Bool:
    case PropertyType::Enum: return std::holds_alternative<std::uint32_t>(value);
    case PropertyType::Float: return std::holds_alternative<float>(value);
    case PropertyType::Float2: return std::holds_alternative<Vector2>(value);
    case PropertyType::Float3: return std::holds_alternative<Vector3>(value);
    case PropertyType::Float4: return std::holds_alternative<Vector4>(value);
    case PropertyType::String:
    case PropertyType::AssetPath: return std::holds_alternative<std::string>(value);
    case PropertyType::Hash: return std::holds_alternative<std::uint64_t>(value);
    case PropertyType::Prim: return std::holds_alternative<PrimTarget>(value);
    case PropertyType::Any:
    case PropertyType::NumberOrVector: return false;
    }
    return false;
}

std::string formatPropertyValue(const PropertyValue& value, PropertyType type) {
    return std::visit(
        [type](const auto& v) -> std::string {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, float>) {
                return formatFloat(v);
            } else if constexpr (std::is_same_v<T, Vector2>) {
                return "(" + formatFloat(v.x) + ", " + formatFloat(v.y) + ")";
            } else if constexpr (std::is_same_v<T, Vector3>) {
                return "(" + formatFloat(v.x) + ", " + formatFloat(v.y) + ", " + formatFloat(v.z) + ")";
            } else if constexpr (std::is_same_v<T, Vector4>) {
                return "(" + formatFloat(v.x) + ", " + formatFloat(v.y) + ", " + formatFloat(v.z) + ", " + formatFloat(v.w) + ")";
            } else if constexpr (std::is_same_v<T, std::uint32_t>) {
                if (type == PropertyType::Bool) {
                    return v ? "true" : "false";
                }
                return std::to_string(v);
            } else if constexpr (std::is_same_v<T, std::uint64_t>) {
                char buf[24];
                std::snprintf(buf, sizeof(buf), "0x%016" PRIX64, v);
                return buf;
            } else if constexpr (std::is_same_v<T, PrimTarget>) {
                return v.valid() ? "prim:" + std::to_string(v.replacementIndex) : "prim:none";
            } else {
                return v;
            }
        },
        value);
}

bool ComponentSpec::isValid() const {
    if (componentType == kInvalidComponentType || name.empty() || createComponentBatch == nullptr) {
        return false;
    }
    for (const PropertySpec& p : properties) {
        if (!p.isValid()) {
            return false;
        }
    }
    return true;
}

std::string ComponentSpec::getClassName() const {
    const std::size_t lastPeriod = name.find_last_of('.');
    return lastPeriod == std::string::npos ? name : name.substr(lastPeriod + 1);
}

ComponentType componentTypeFromName(std::string_view fullName) { return hash::xxh3_64(fullName.data(), fullName.size()); }

void registerComponentSpec(const ComponentSpec* spec) {
    if (spec == nullptr) {
        logMessage(LogSeverity::Error, "Cannot register null component spec");
        return;
    }
    if (!spec->isValid()) {
        logMessage(LogSeverity::Error, "Cannot register invalid component spec: " + spec->name);
        return;
    }
    Registry& r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    auto add = [&](ComponentType type, const std::string& label) {
        std::vector<const ComponentSpec*>& list = r.variants[type];
        for (const ComponentSpec* existing : list) {
            if (existing->resolvedTypes == spec->resolvedTypes) {
                if (existing != spec) {
                    logMessage(LogSeverity::Error, "Component spec variant for type " + label +
                                                       " already registered with different spec pointer.");
                }
                return;
            }
        }
        list.push_back(spec);
    };
    add(spec->componentType, spec->name);
    for (const std::string& oldName : spec->oldNames) {
        const std::string fullOldName = std::string(PropertySpec::kUsdNamePrefix) + oldName;
        add(componentTypeFromName(fullOldName), fullOldName);
    }
}

const ComponentSpec* getComponentSpec(ComponentType componentType) {
    if (componentType == kInvalidComponentType) {
        return nullptr;
    }
    return getAnyComponentSpecVariant(componentType);
}

const std::vector<const ComponentSpec*>& getAllComponentSpecVariants(ComponentType componentType) {
    static const std::vector<const ComponentSpec*> kEmpty;
    Registry& r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    const auto it = r.variants.find(componentType);
    return it == r.variants.end() ? kEmpty : it->second;
}

const ComponentSpec* getAnyComponentSpecVariant(ComponentType componentType) {
    Registry& r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    const auto it = r.variants.find(componentType);
    return it == r.variants.end() || it->second.empty() ? nullptr : it->second.front();
}

std::vector<const ComponentSpec*> listComponents() {
    Registry& r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    std::map<std::string, const ComponentSpec*> byName;
    for (const auto& [type, list] : r.variants) {
        if (!list.empty() && list.front()->componentType == type) {
            byName.emplace(list.front()->name, list.front());
        }
    }
    std::vector<const ComponentSpec*> out;
    for (const auto& [name, spec] : byName) {
        out.push_back(spec);
    }
    return out;
}

const PrimSnapshot* LogicContext::resolvePrim(std::uint64_t owner, const PrimTarget& target) const {
    if (target.replacementIndex == PrimTarget::kInvalidReplacementIndex) {
        if (target.instanceId != PrimTarget::kInvalidInstanceId) {
            logOnce(LogSeverity::Error, "components targetting prims in other draw calls is not supported yet.");
        }
        return nullptr;
    }
    const auto it = m_inputs.prims.find(owner);
    if (it == m_inputs.prims.end() || target.replacementIndex >= it->second.size()) {
        return nullptr;
    }
    const PrimSnapshot& snapshot = it->second[target.replacementIndex];
    return snapshot.kind == PrimSnapshot::Kind::None ? nullptr : &snapshot;
}

} // namespace fuse::relight::logic
