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
// Ported from dxvk-remix src/dxvk/rtx_render/graph/rtx_graph_usd_parser.cpp@0867d3c and the graph discovery of
// src/dxvk/rtx_render/rtx_mod_usd.cpp@0867d3c (processReplacement, processReplacementRecursive, processGraph)
#include <fuse/relight/logic/graph_usd_parser.hpp>
#include <fuse/relight/logic/component_list.hpp>

#include <fuse/relight/hash/xxh.hpp>
#include <fuse/relight/mods/usd/remix_profile.hpp>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <set>

namespace fuse::relight::logic {

namespace usd = mods::usd;

namespace {

constexpr std::string_view kOmniGraphNode = "OmniGraphNode";
constexpr std::string_view kOmniGraph = "OmniGraph";

void report(std::vector<GraphDiagnostic>* diags, LogSeverity severity, std::string path, std::string message) {
    logMessage(severity, path.empty() ? message : path + ": " + message);
    if (diags != nullptr) {
        diags->push_back({severity, std::move(path), std::move(message)});
    }
}

std::string primPathOf(const std::string& propertyPath) {
    const std::size_t dot = propertyPath.rfind('.');
    return dot == std::string::npos ? propertyPath : propertyPath.substr(0, dot);
}

std::string childPath(const std::string& parent, const std::string& name) { return parent == "/" ? "/" + name : parent + "/" + name; }

/// Upstream getLastValidConnection: the last connection whose prim exists in the stage.
bool lastValidConnection(const usd::ComposedStage& stage, const usd::Attribute* attr, std::string& out) {
    if (attr == nullptr) {
        return false;
    }
    for (auto it = attr->connections.rbegin(); it != attr->connections.rend(); ++it) {
        if (stage.find(primPathOf(*it)) != nullptr) {
            out = *it;
            return true;
        }
    }
    return false;
}

bool isUsdLightType(const std::string& t) {
    return t == "SphereLight" || t == "RectLight" || t == "DiskLight" || t == "CylinderLight" || t == "DistantLight";
}

/// Upstream getPropertyIndex: the index of `propertyPath`, created (with the old-name aliases) on first use.
std::size_t getPropertyIndex(GraphTopology& topology, const std::string& propertyPath, const PropertySpec& property) {
    const auto it = topology.propertyPathToIndex.find(propertyPath);
    if (it != topology.propertyPathToIndex.end()) {
        return it->second;
    }
    const std::size_t index = topology.propertyTypes.size();
    topology.propertyTypes.push_back(property.type);
    topology.propertyPathToIndex[propertyPath] = index;
    const std::string nodePath = primPathOf(propertyPath);
    for (const std::string& oldName : property.oldUsdNames) {
        topology.propertyPathToIndex[nodePath + "." + oldName] = index;
    }
    return index;
}

std::optional<std::string> tokenOf(const usd::Value& v) {
    if (v.kind == usd::Value::Kind::String) {
        return v.text;
    }
    return std::nullopt;
}

/// A token value converted to the property type (enum names first), or the default on failure.
PropertyValue fromToken(const std::string& token, const PropertySpec& spec) {
    if (!spec.enumValues.empty() && spec.type != PropertyType::Bool) {
        const auto it = spec.enumValues.find(token);
        if (it != spec.enumValues.end()) {
            return it->second.value;
        }
    }
    PropertyValue v = propertyValueFromString(token, spec.type);
    return v == kInvalidPropertyValue ? spec.defaultValue : v;
}

template <typename V, std::size_t N>
std::optional<PropertyValue> vectorFrom(const usd::Value& v) {
    const auto numbers = v.asNumbers();
    if (v.kind != usd::Value::Kind::Tuple || !numbers || numbers->size() != N) {
        return std::nullopt;
    }
    V out;
    for (std::size_t i = 0; i < N; ++i) {
        out[i] = static_cast<float>((*numbers)[i]);
    }
    return PropertyValue(out);
}

/// Upstream getPropertyValue(UsdAttribute): the authored default, converted to the property type.
PropertyValue attributeValue(const usd::Attribute* attr, const PropertySpec& spec, const std::string& path,
                             std::vector<GraphDiagnostic>* diags) {
    if (attr == nullptr || !attr->hasDefault) {
        return spec.defaultValue;
    }
    const usd::Value& v = attr->defaultValue.get();
    if (v.isNone()) {
        return spec.defaultValue; // declared but empty (common for outputs)
    }
    const std::optional<std::string> token = tokenOf(v);
    std::optional<PropertyValue> out;
    switch (spec.type) {
    case PropertyType::Bool:
        if (token) {
            return propertyValueFromString(*token, PropertyType::Bool);
        }
        if (const auto b = v.asBool()) {
            out = PropertyValue(std::in_place_type<std::uint32_t>, *b ? 1u : 0u);
        }
        break;
    case PropertyType::Float:
        if (token) {
            return fromToken(*token, spec);
        }
        if (v.kind == usd::Value::Kind::Number) {
            out = PropertyValue(static_cast<float>(v.number));
        }
        break;
    case PropertyType::Float2:
        out = token ? std::optional<PropertyValue>(fromToken(*token, spec)) : vectorFrom<Vector2, 2>(v);
        break;
    case PropertyType::Float3:
        out = token ? std::optional<PropertyValue>(fromToken(*token, spec)) : vectorFrom<Vector3, 3>(v);
        break;
    case PropertyType::Float4:
        out = token ? std::optional<PropertyValue>(fromToken(*token, spec)) : vectorFrom<Vector4, 4>(v);
        break;
    case PropertyType::Enum:
        if (token) {
            return fromToken(*token, spec);
        }
        if (v.kind == usd::Value::Kind::Number && v.number >= 0.0) {
            out = PropertyValue(std::in_place_type<std::uint32_t>, static_cast<std::uint32_t>(v.number));
        }
        break;
    case PropertyType::String:
        if (token) {
            return *token;
        }
        if (v.kind == usd::Value::Kind::Asset) {
            out = PropertyValue(v.text);
        }
        break;
    case PropertyType::AssetPath:
        if (token) {
            if (token->empty()) {
                return spec.defaultValue;
            }
            // Relative to the layer that authored the value (upstream resolveAssetPath with ArResolver).
            return usd::anchorAssetPath(usd::parentDir(attr->definingLayer), *token);
        }
        if (v.kind == usd::Value::Kind::Asset) {
            if (v.text.empty()) {
                return spec.defaultValue;
            }
            out = PropertyValue(v.resolved.empty() ? v.text : v.resolved);
        }
        break;
    case PropertyType::Hash:
        if (token) {
            return fromToken(*token, spec);
        }
        if (v.kind == usd::Value::Kind::Number) {
            // uint64 values keep their literal (a double cannot hold every 64-bit hash).
            char* end = nullptr;
            errno = 0;
            const unsigned long long h = std::strtoull(v.text.c_str(), &end, 10);
            if (end != v.text.c_str() && errno != ERANGE) {
                out = PropertyValue(std::in_place_type<std::uint64_t>, static_cast<std::uint64_t>(h));
            }
        }
        break;
    case PropertyType::Prim:
        report(diags, LogSeverity::Error, path, "Prim target properties should be UsdRelationships, not UsdAttributes.");
        return spec.defaultValue;
    case PropertyType::Any:
    case PropertyType::NumberOrVector:
        report(diags, LogSeverity::Error, path, "Flexible types (Any, NumberOrVector) should not be loaded from USD attributes.");
        return spec.defaultValue;
    }
    if (!out) {
        report(diags, LogSeverity::Error, path,
               std::string("type mismatch: property expects type ") + propertyTypeName(spec.type) + " but got type " + attr->typeName);
        return spec.defaultValue;
    }
    return *out;
}

/// Upstream getPropertyValue(UsdRelationship): a single target resolved through the prim table.
PropertyValue relationshipValue(const usd::ComposedStage& stage, const usd::Relationship* rel, const PropertySpec& spec,
                                const std::string& path, const PrimTable& primTable, std::vector<GraphDiagnostic>* diags) {
    if (spec.type != PropertyType::Prim) {
        report(diags, LogSeverity::Error, path,
               "Incorrect type of USD property: " + spec.usdPropertyName + " should be an attribute, but was a Relationship.");
        return spec.defaultValue;
    }
    if (rel != nullptr) {
        if (rel->targets.size() == 1) {
            const std::string& target = rel->targets[0];
            PrimTarget result;
            const auto it = primTable.find(target);
            if (it == primTable.end()) {
                report(diags, LogSeverity::Error, path, "Relationship path " + target + " not found in replacement hierarchy.");
            } else if (stage.find(target) == nullptr && it->second != 0) {
                // Index 0 may be the original draw ("<root>/mesh"), which need not be a prim.
                report(diags, LogSeverity::Error, path, "Relationship path " + target + " not found in replacement hierarchy.");
            } else {
                result.replacementIndex = it->second;
            }
            return result;
        }
        if (rel->targets.size() > 1) {
            report(diags, LogSeverity::Error, path, "Relationship has multiple targets, which is not supported.");
        }
    }
    // An unconnected relationship is invalid (the default value is ignored), as upstream.
    return PrimTarget{};
}

PropertyType inferTypeFromConnections(const usd::ComposedStage& stage, const usd::Attribute* attr, const PropertySpec& property,
                                      const usd::Prim& graph, const std::string& propertyPath) {
    if (property.ioType == PropertyIOType::Output) {
        for (const std::string& name : graph.children) {
            const usd::Prim* child = stage.find(childPath(graph.path, name));
            if (child == nullptr || !child->active || child->typeName != kOmniGraphNode) {
                continue;
            }
            const ComponentSpec* spec = parser_detail::componentSpecForPrim(*child, nullptr);
            if (spec == nullptr) {
                continue;
            }
            for (const PropertySpec& other : spec->properties) {
                if (other.ioType != PropertyIOType::Input) {
                    continue;
                }
                const usd::Attribute* otherAttr = child->attribute(parser_detail::resolvePropertyName(*child, other));
                if (otherAttr == nullptr) {
                    continue;
                }
                for (const std::string& conn : otherAttr->connections) {
                    if (conn == propertyPath && other.type == other.declaredType) {
                        return other.type;
                    }
                }
            }
        }
    }
    if (property.ioType == PropertyIOType::Input) {
        std::string sourcePath;
        if (lastValidConnection(stage, attr, sourcePath)) {
            const usd::Prim* sourcePrim = stage.find(primPathOf(sourcePath));
            if (sourcePrim != nullptr) {
                if (const ComponentSpec* sourceSpec = parser_detail::componentSpecForPrim(*sourcePrim, nullptr)) {
                    for (const PropertySpec& sourceProp : sourceSpec->properties) {
                        if (sourcePrim->path + "." + parser_detail::resolvePropertyName(*sourcePrim, sourceProp) == sourcePath &&
                            sourceProp.type == sourceProp.declaredType) {
                            return sourceProp.type;
                        }
                    }
                }
            }
        }
    }
    return PropertyType::Float;
}

PropertyType resolveFlexibleTypeFromAttribute(const usd::ComposedStage& stage, const usd::Attribute* attr,
                                              const PropertySpec& property, const usd::Prim& graph, const std::string& propertyPath,
                                              std::vector<GraphDiagnostic>* diags) {
    if (!isFlexibleType(property.declaredType)) {
        return property.type;
    }
    if (attr == nullptr) {
        const PropertyType connected = inferTypeFromConnections(stage, attr, property, graph, propertyPath);
        if (connected != PropertyType::Float) {
            return connected;
        }
        report(diags, LogSeverity::Warning, propertyPath, "Could not resolve flexible type for property " + property.name +
                                                               ", defaulting to Float");
        return PropertyType::Float;
    }
    const std::string& typeStr = attr->typeName;
    if (typeStr == "token") {
        if (attr->hasDefault) {
            const std::optional<std::string> token = tokenOf(attr->defaultValue.get());
            if (token && !token->empty()) {
                return parser_detail::inferTypeFromTokenString(*token);
            }
        }
        const PropertyType connected = inferTypeFromConnections(stage, attr, property, graph, propertyPath);
        if (connected != PropertyType::Float) {
            return connected;
        }
        return property.declaredType == PropertyType::NumberOrVector ? PropertyType::Float3 : PropertyType::Float;
    }
    if (typeStr == "bool") return PropertyType::Bool;
    if (typeStr == "float" || typeStr == "double") return PropertyType::Float;
    if (typeStr == "float2" || typeStr == "double2") return PropertyType::Float2;
    if (typeStr == "float3" || typeStr == "double3" || typeStr == "normal3f" || typeStr == "normal3d" || typeStr == "color3f" ||
        typeStr == "color3d") {
        return PropertyType::Float3;
    }
    if (typeStr == "float4" || typeStr == "double4" || typeStr == "color4f" || typeStr == "color4d") return PropertyType::Float4;
    if (typeStr == "uint") return PropertyType::Enum;
    if (typeStr == "uint64") return PropertyType::Hash;
    report(diags, LogSeverity::Warning, propertyPath, "Could not resolve flexible type for property " + property.name +
                                                           " with USD type " + typeStr + ", defaulting to Float");
    return PropertyType::Float;
}

void hashInto(std::uint64_t& h, const void* data, std::size_t size) { h = hash::xxh3_64(data, size, h); }

} // namespace

namespace parser_detail {

std::string resolvePropertyName(const usd::Prim& node, const PropertySpec& property) {
    auto authored = [&](const std::string& name) { return node.attribute(name) != nullptr || node.relationship(name) != nullptr; };
    if (property.oldUsdNames.empty() || authored(property.usdPropertyName)) {
        return property.usdPropertyName;
    }
    // Upstream keeps the strongest property stack by layer offset; with the identity offsets of the Remix profile
    // that is the first authored old name.
    for (const std::string& oldName : property.oldUsdNames) {
        if (authored(oldName)) {
            return oldName;
        }
    }
    return property.usdPropertyName;
}

const ComponentSpec* componentSpecForPrim(const usd::Prim& node, std::vector<GraphDiagnostic>* diagnostics) {
    registerAllComponents();
    const usd::Attribute* typeAttr = node.attribute("node:type");
    if (typeAttr == nullptr) {
        report(diagnostics, LogSeverity::Error, node.path, "Node has no `node:type` attribute");
        return nullptr;
    }
    const std::optional<std::string> typeName = typeAttr->defaultValue->asString();
    if (!typeName || typeName->empty()) {
        report(diagnostics, LogSeverity::Error, node.path, "Node has an empty `node:type` attribute");
        return nullptr;
    }
    const ComponentSpec* spec = getComponentSpec(componentTypeFromName(*typeName));
    if (spec == nullptr) {
        report(diagnostics, LogSeverity::Error, node.path, "Node has an unknown `node:type` attribute: " + *typeName);
    }
    return spec;
}

bool versionCheck(const usd::Prim& node, const ComponentSpec& spec, std::vector<GraphDiagnostic>* diagnostics) {
    const usd::Attribute* versionAttr = node.attribute("node:typeVersion");
    const std::optional<double> version = versionAttr != nullptr ? versionAttr->defaultValue->asNumber() : std::nullopt;
    if (!version) {
        report(diagnostics, LogSeverity::Error, node.path, "Component is missing a `node:typeVersion` attribute.");
        return false;
    }
    const int dataVersion = static_cast<int>(*version);
    if (dataVersion > spec.version) {
        report(diagnostics, LogSeverity::Error, node.path,
               "Component is newer than this runtime can handle.  This means the graph was authored with a newer version of "
               "the runtime.");
        return false;
    }
    if (dataVersion < spec.version) {
        report(diagnostics, LogSeverity::Warning, node.path,
               "Component is old.  This means the graph was authored with an older version of the schema, and should be "
               "updated in the Toolkit.");
    }
    return true;
}

PropertyType inferTypeFromTokenString(const std::string& tokenStr) {
    if (tokenStr.empty()) {
        return PropertyType::String;
    }
    if ((tokenStr.front() == '(' && tokenStr.back() == ')') || (tokenStr.front() == '[' && tokenStr.back() == ']')) {
        const std::size_t dimension = static_cast<std::size_t>(std::count(tokenStr.begin(), tokenStr.end(), ',')) + 1;
        if (dimension == 2) return PropertyType::Float2;
        if (dimension == 3) return PropertyType::Float3;
        if (dimension == 4) return PropertyType::Float4;
    }
    if (tokenStr.size() >= 3 && tokenStr[0] == '0' && (tokenStr[1] == 'x' || tokenStr[1] == 'X')) {
        bool isValidHex = true;
        std::size_t digits = 0;
        for (std::size_t i = 2; i < tokenStr.size(); ++i) {
            const char c = tokenStr[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
                isValidHex = false;
                break;
            }
            digits++;
        }
        if (isValidHex && digits > 0 && digits <= 16) {
            return PropertyType::Hash;
        }
    }
    // As upstream, the exponent test runs before the bool test (so "true" / "false" infer Float).
    if (tokenStr.find('.') != std::string::npos || tokenStr.find('e') != std::string::npos ||
        tokenStr.find('E') != std::string::npos) {
        return PropertyType::Float;
    }
    if (tokenStr == "true" || tokenStr == "false" || tokenStr == "True" || tokenStr == "False") {
        return PropertyType::Bool;
    }
    // std::stod succeeds -> a number.
    char* end = nullptr;
    const char* begin = tokenStr.c_str();
    while (*begin == ' ' || *begin == '\t' || *begin == '\n') {
        ++begin;
    }
    (void)std::strtod(begin, &end);
    if (end != begin) {
        return PropertyType::Float;
    }
    return PropertyType::String;
}

std::vector<DagNode> dagSortedNodes(const usd::ComposedStage& stage, std::string_view graphPath,
                                    std::vector<GraphDiagnostic>* diagnostics) {
    struct Node {
        std::string path;
        const ComponentSpec* spec = nullptr;
        std::size_t dependencyCount = 0;
        std::set<std::size_t> dependents;
    };
    std::vector<Node> nodes;
    std::map<std::string, std::size_t> pathToIndex;
    const usd::Prim* graph = stage.find(graphPath);
    if (graph == nullptr) {
        report(diagnostics, LogSeverity::Error, std::string(graphPath), "graph prim not found");
        return {};
    }
    for (const std::string& name : graph->children) {
        const usd::Prim* child = stage.find(childPath(graph->path, name));
        if (child == nullptr || !child->active || child->typeName != kOmniGraphNode) {
            continue;
        }
        const ComponentSpec* spec = componentSpecForPrim(*child, diagnostics);
        if (spec == nullptr || !versionCheck(*child, *spec, diagnostics)) {
            continue;
        }
        pathToIndex[child->path] = nodes.size();
        nodes.push_back({child->path, spec, 0, {}});
    }
    // Edges: node -> the nodes it reads from (`dependents` in upstream's naming), counted on the source.
    for (std::size_t nodeIndex = 0; nodeIndex < nodes.size(); nodeIndex++) {
        Node& node = nodes[nodeIndex];
        const usd::Prim& prim = *stage.find(node.path);
        for (const PropertySpec& property : node.spec->properties) {
            const std::string name = resolvePropertyName(prim, property);
            std::string sourcePrimPath;
            if (property.type == PropertyType::Prim) {
                const usd::Relationship* rel = prim.relationship(name);
                if (rel == nullptr || rel->targets.size() <= 1) {
                    continue;
                }
                sourcePrimPath = primPathOf(rel->targets.back());
            } else {
                std::string connection;
                if (!lastValidConnection(stage, prim.attribute(name), connection)) {
                    continue;
                }
                sourcePrimPath = primPathOf(connection);
            }
            const auto it = pathToIndex.find(sourcePrimPath);
            if (it == pathToIndex.end()) {
                report(diagnostics, LogSeverity::Error, node.path,
                       "Node has a connection to a prim that exists but was not loaded (may have failed to load earlier in "
                       "the process): " + sourcePrimPath);
                continue;
            }
            if (node.dependents.insert(it->second).second) {
                nodes[it->second].dependencyCount++;
            }
        }
    }
    // Topological order: repeatedly take the nodes nobody still reads from, sorted by component type then path;
    // the list is reversed at the end so sources come first (upstream getDAGSortedNodes).
    std::vector<std::size_t> ready;
    for (std::size_t i = 0; i < nodes.size(); i++) {
        if (nodes[i].dependencyCount == 0) {
            ready.push_back(i);
        }
    }
    std::vector<std::size_t> sorted;
    std::size_t visited = 0;
    while (!ready.empty()) {
        std::sort(ready.begin(), ready.end(), [&nodes](std::size_t a, std::size_t b) {
            if (nodes[a].spec->componentType == nodes[b].spec->componentType) {
                return nodes[a].path < nodes[b].path;
            }
            return nodes[a].spec->componentType < nodes[b].spec->componentType;
        });
        sorted.insert(sorted.end(), ready.begin(), ready.end());
        ready.clear();
        for (; visited < sorted.size(); ++visited) {
            for (std::size_t dep : nodes[sorted[visited]].dependents) {
                if (--nodes[dep].dependencyCount == 0) {
                    ready.push_back(dep);
                }
            }
        }
    }
    if (sorted.size() != nodes.size()) {
        std::string list;
        for (std::size_t i = 0; i < nodes.size(); i++) {
            if (std::find(sorted.begin(), sorted.end(), i) == sorted.end()) {
                list += " " + nodes[i].path;
            }
        }
        report(diagnostics, LogSeverity::Error, std::string(graphPath),
               "Graph has a cycle.  These nodes will not be loaded due to unresolvable dependencies:" + list);
    }
    std::vector<DagNode> out;
    out.reserve(sorted.size());
    for (auto it = sorted.rbegin(); it != sorted.rend(); ++it) {
        out.push_back({nodes[*it].path, nodes[*it].spec});
    }
    return out;
}

} // namespace parser_detail

std::shared_ptr<const GraphState> parseGraph(const usd::ComposedStage& stage, std::string_view graphPath, const PrimTable& primTable,
                                             std::vector<GraphDiagnostic>* diagnostics) {
    registerAllComponents();
    auto topology = std::make_shared<GraphTopology>();
    auto state = std::make_shared<GraphState>();
    state->primPath = std::string(graphPath);
    const usd::Prim* graph = stage.find(graphPath);
    if (graph == nullptr) {
        report(diagnostics, LogSeverity::Error, std::string(graphPath), "graph prim not found");
        state->topology = topology;
        return state;
    }
    for (const parser_detail::DagNode& dagNode : parser_detail::dagSortedNodes(stage, graphPath, diagnostics)) {
        const ComponentSpec& baseSpec = *dagNode.spec;
        const usd::Prim& node = *stage.find(dagNode.path);
        std::vector<std::size_t> propertyIndices;

        // First pass: resolve the flexible input / state types to pick the variant.
        std::map<std::string, PropertyType> resolvedTypes;
        for (const PropertySpec& property : baseSpec.properties) {
            if (property.ioType == PropertyIOType::Output || property.declaredType == property.type) {
                continue;
            }
            const std::string propertyPath = node.path + "." + parser_detail::resolvePropertyName(node, property);
            const usd::Attribute* attr = node.attribute(parser_detail::resolvePropertyName(node, property));
            PropertyType resolved = property.declaredType;
            std::string connection;
            if (lastValidConnection(stage, attr, connection)) {
                const auto it = topology->propertyPathToIndex.find(connection);
                if (it != topology->propertyPathToIndex.end()) {
                    resolved = topology->propertyTypes[it->second];
                }
            }
            if (resolved == property.declaredType) {
                resolved = resolveFlexibleTypeFromAttribute(stage, attr, property, *graph, propertyPath, diagnostics);
            }
            resolvedTypes[property.name] = resolved;
        }
        const ComponentSpec* spec = &baseSpec;
        if (!resolvedTypes.empty()) {
            const ComponentSpec* match = nullptr;
            for (const ComponentSpec* variant : getAllComponentSpecVariants(baseSpec.componentType)) {
                bool allMatch = true;
                for (const auto& [name, type] : resolvedTypes) {
                    const auto vit = variant->resolvedTypes.find(name);
                    if (vit == variant->resolvedTypes.end() || vit->second != type) {
                        allMatch = false;
                        break;
                    }
                }
                if (allMatch) {
                    match = variant;
                    break;
                }
            }
            if (match != nullptr) {
                spec = match;
            } else {
                report(diagnostics, LogSeverity::Warning, node.path,
                       "Could not find matching variant for component " + baseSpec.name + " with resolved input types");
            }
        }

        // Second pass: every property of the chosen variant, in spec order.
        for (const PropertySpec& property : spec->properties) {
            const std::string name = parser_detail::resolvePropertyName(node, property);
            const std::string propertyPath = node.path + "." + name;
            bool connected = false;
            std::string sourcePath;
            if (property.type == PropertyType::Prim) {
                const usd::Relationship* rel = node.relationship(name);
                if (rel != nullptr && rel->targets.size() > 1) {
                    if (rel->targets.size() != 2) {
                        report(diagnostics, LogSeverity::Error, propertyPath,
                               "Multiple prims are not (currently) supported in Component prim target properties.");
                    }
                    sourcePath = rel->targets.back();
                }
            } else {
                lastValidConnection(stage, node.attribute(name), sourcePath);
            }
            if (!sourcePath.empty()) {
                const auto it = topology->propertyPathToIndex.find(sourcePath);
                if (it == topology->propertyPathToIndex.end()) {
                    report(diagnostics, LogSeverity::Error, propertyPath,
                           "Property has a connection to property " + sourcePath +
                               " that has not been loaded yet.  This may be because that prim failed to load, or it may "
                               "indicate an error in the topological sort.");
                } else if (topology->propertyTypes[it->second] != property.type) {
                    report(diagnostics, LogSeverity::Error, propertyPath,
                           std::string("Property (type ") + propertyTypeName(property.type) + ") has a connection to property " +
                               sourcePath + " (type " + propertyTypeName(topology->propertyTypes[it->second]) +
                               ") with mismatched types. Connection ignored.");
                } else {
                    propertyIndices.push_back(it->second);
                    connected = true;
                }
            }
            if (!connected) {
                propertyIndices.push_back(getPropertyIndex(*topology, propertyPath, property));
                if (property.type == PropertyType::Prim) {
                    state->values.push_back(
                        relationshipValue(stage, node.relationship(name), property, propertyPath, primTable, diagnostics));
                } else {
                    state->values.push_back(attributeValue(node.attribute(name), property, propertyPath, diagnostics));
                }
            }
        }

        topology->componentSpecs.push_back(spec);
        topology->nodePaths.push_back(node.path);
        hashInto(topology->graphHash, &spec->componentType, sizeof(spec->componentType));
        std::vector<std::uint64_t> indices64(propertyIndices.begin(), propertyIndices.end());
        hashInto(topology->graphHash, indices64.data(), indices64.size() * sizeof(std::uint64_t));
        std::vector<std::uint32_t> types;
        for (std::size_t idx : propertyIndices) {
            types.push_back(static_cast<std::uint32_t>(topology->propertyTypes[idx]));
        }
        hashInto(topology->graphHash, types.data(), types.size() * sizeof(std::uint32_t));
        topology->propertyIndices.push_back(std::move(propertyIndices));
    }
    state->topology = topology;
    return state;
}

std::size_t ModGraphs::graphCount() const {
    std::size_t n = 0;
    for (const auto& [hash, r] : meshes) {
        n += r.graphs.size();
    }
    return n;
}

ModGraphs loadModGraphs(const usd::ComposedStage& stage, std::string modName) {
    registerAllComponents();
    ModGraphs out;
    out.mod = std::move(modName);
    const usd::RemixMod remix = usd::collectRemixMod(stage);
    for (const usd::MeshReplacement& m : remix.meshes) {
        ReplacementGraphs rg;
        rg.rootPath = m.path;
        rg.hash = m.hash;
        rg.preserveOriginalDrawCall = m.preserveOriginalDrawCall.value_or(false);
        if (rg.preserveOriginalDrawCall) {
            // The original draw occupies index 0 ("<root>/mesh", lss::gTokMesh), as upstream.
            rg.prims.push_back({m.path + "/mesh", ReplacementGraphs::PrimKind::OriginalMesh});
        }
        // processReplacementRecursive: meshes, lights and (below the root) graphs, depth first; point instancer
        // subtrees are not visited.
        auto visit = [&](auto&& self, const usd::Prim& prim, bool isRoot) -> void {
            if (prim.typeName == "Mesh") {
                rg.prims.push_back({prim.path, ReplacementGraphs::PrimKind::Mesh});
            } else if (prim.typeName == "PointInstancer") {
                return;
            } else if (isUsdLightType(prim.typeName)) {
                rg.prims.push_back({prim.path, ReplacementGraphs::PrimKind::Light});
            } else if (prim.typeName == kOmniGraph && !isRoot) {
                rg.prims.push_back({prim.path, ReplacementGraphs::PrimKind::Graph});
            }
            for (const std::string& name : prim.children) {
                const usd::Prim* child = stage.find(childPath(prim.path, name));
                if (child != nullptr && child->active) {
                    self(self, *child, false);
                }
            }
        };
        if (const usd::Prim* root = stage.find(m.path)) {
            visit(visit, *root, true);
        }
        for (std::size_t i = 0; i < rg.prims.size(); ++i) {
            rg.primTable[rg.prims[i].path] = static_cast<std::uint32_t>(i); // a later prim with the same path wins
        }
        for (const ReplacementGraphs::PrimEntry& e : rg.prims) {
            if (e.kind == ReplacementGraphs::PrimKind::Graph) {
                rg.graphs.push_back(parseGraph(stage, e.path, rg.primTable, &out.diagnostics));
            }
        }
        if (!rg.graphs.empty()) {
            out.meshes.emplace(rg.hash, std::move(rg));
        }
    }
    return out;
}

} // namespace fuse::relight::logic
