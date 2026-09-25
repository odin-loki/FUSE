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
// Ported from dxvk-remix src/dxvk/rtx_render/graph/rtx_graph_types.h@0867d3c
//
// FUSE Relight RL-3.5: the value model of Logic graphs (component property types, values, specs, topology).
// Names drop upstream's `Rt` prefix (RtComponentPropertyType -> PropertyType, RtComponentSpec -> ComponentSpec,
// RtGraphTopology -> GraphTopology, ...); the semantics are upstream's:
//   * a component instance's properties are stored structure-of-arrays: one PropertyVector per property of the
//     graph topology, one element per graph instance, so a batch updates every instance of a graph at once;
//   * a connected input *is* the upstream output (both use the same property index), so evaluating components in
//     topological order propagates values within the frame;
//   * bool is stored as uint32_t (std::vector<bool> is not addressable); Enum is uint32_t; Hash is uint64_t;
//   * the flexible types Any and NumberOrVector are resolved to concrete types per node when a graph is parsed;
//     each concrete combination is a registered variant (ComponentSpec::resolvedTypes) of the component.
// Differences: components are registered explicitly (registerAllComponents(), component_list.hpp) instead of by
// static initialisers (static libraries drop unreferenced objects); the registry is ordered (std::map) so every
// listing is deterministic; the OGN / markdown writers are not ported (the Toolkit owns the schema).
#pragma once

#include <fuse/relight/logic/logic_math.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace fuse::relight::logic {

class LogicContext;
class GraphBatch;

enum class PropertyIOType : std::uint8_t { Input, State, Output };
const char* ioTypeName(PropertyIOType t);

/// A reference to a prim of the replacement that owns the graph instance (a USD relationship in the mod).
/// `replacementIndex` indexes the owner's prim table (graph_usd_parser.hpp PrimTable); instanceId (prims of other
/// draws) is not supported, as upstream.
struct PrimTarget {
    static constexpr std::uint32_t kInvalidReplacementIndex = 0xFFFFFFFFu;
    static constexpr std::uint64_t kInvalidInstanceId = 0xFFFFFFFFFFFFFFFFull;
    std::uint32_t replacementIndex = kInvalidReplacementIndex;
    std::uint64_t instanceId = kInvalidInstanceId;

    bool valid() const { return replacementIndex != kInvalidReplacementIndex; }
    bool operator==(const PrimTarget& o) const { return replacementIndex == o.replacementIndex && instanceId == o.instanceId; }
    bool operator!=(const PrimTarget& o) const { return !(*this == o); }
    /// Arbitrary but consistent ordering (std::variant comparisons), as upstream.
    bool operator<(const PrimTarget& o) const {
        return instanceId != o.instanceId ? instanceId < o.instanceId : replacementIndex < o.replacementIndex;
    }
};
inline constexpr PrimTarget kInvalidPrimTarget{};

enum class PropertyType : std::uint8_t {
    Bool,
    Float,
    Float2,
    Float3,
    Float4,
    Enum,
    String,
    AssetPath,
    Hash,
    Prim, ///< a USD relationship; the default value is ignored
    // Flexible types
    Any,            ///< any of the above
    NumberOrVector, ///< Float, Float2, Float3 or Float4
};
const char* propertyTypeName(PropertyType t);
bool isFlexibleType(PropertyType t);

/// Order matches upstream's RtComponentPropertyValue.
using PropertyValue = std::variant<float, Vector2, Vector3, Vector4, std::uint32_t, std::uint64_t, PrimTarget, std::string>;
using PropertyVector = std::variant<std::vector<float>, std::vector<Vector2>, std::vector<Vector3>, std::vector<Vector4>,
                                    std::vector<std::uint32_t>, std::vector<std::uint64_t>, std::vector<PrimTarget>,
                                    std::vector<std::string>>;
/// The value types NumberOrVector may resolve to (the flexible binary operators iterate over it).
using PropertyNumberOrVector = std::variant<float, Vector2, Vector3, Vector4>;

inline const PropertyValue kInvalidPropertyValue{std::in_place_type<PrimTarget>, PrimTarget()};
inline const PropertyValue kFalsePropertyValue{std::in_place_type<std::uint32_t>, 0u};
inline const PropertyValue kTruePropertyValue{std::in_place_type<std::uint32_t>, 1u};

template <PropertyType T>
struct PropertyTypeToCppTypeImpl;
template <> struct PropertyTypeToCppTypeImpl<PropertyType::Bool> { using Type = std::uint32_t; };
template <> struct PropertyTypeToCppTypeImpl<PropertyType::Float> { using Type = float; };
template <> struct PropertyTypeToCppTypeImpl<PropertyType::Float2> { using Type = Vector2; };
template <> struct PropertyTypeToCppTypeImpl<PropertyType::Float3> { using Type = Vector3; };
template <> struct PropertyTypeToCppTypeImpl<PropertyType::Float4> { using Type = Vector4; };
template <> struct PropertyTypeToCppTypeImpl<PropertyType::Enum> { using Type = std::uint32_t; };
template <> struct PropertyTypeToCppTypeImpl<PropertyType::String> { using Type = std::string; };
template <> struct PropertyTypeToCppTypeImpl<PropertyType::AssetPath> { using Type = std::string; };
template <> struct PropertyTypeToCppTypeImpl<PropertyType::Hash> { using Type = std::uint64_t; };
template <> struct PropertyTypeToCppTypeImpl<PropertyType::Prim> { using Type = PrimTarget; };
// Flexible declarations resolve to a concrete type before use; Float stands in for the unresolved spec.
template <> struct PropertyTypeToCppTypeImpl<PropertyType::Any> { using Type = float; };
template <> struct PropertyTypeToCppTypeImpl<PropertyType::NumberOrVector> { using Type = float; };
template <PropertyType T>
using PropertyTypeToCppType = typename PropertyTypeToCppTypeImpl<T>::Type;

/// C++ type -> property type (uint32_t maps to Enum, which is enough to instantiate templates).
template <typename T> struct CppTypeToPropertyType;
template <> struct CppTypeToPropertyType<float> { static constexpr PropertyType value = PropertyType::Float; };
template <> struct CppTypeToPropertyType<Vector2> { static constexpr PropertyType value = PropertyType::Float2; };
template <> struct CppTypeToPropertyType<Vector3> { static constexpr PropertyType value = PropertyType::Float3; };
template <> struct CppTypeToPropertyType<Vector4> { static constexpr PropertyType value = PropertyType::Float4; };
template <> struct CppTypeToPropertyType<std::uint32_t> { static constexpr PropertyType value = PropertyType::Enum; };
template <> struct CppTypeToPropertyType<PrimTarget> { static constexpr PropertyType value = PropertyType::Prim; };
template <> struct CppTypeToPropertyType<std::string> { static constexpr PropertyType value = PropertyType::String; };
template <> struct CppTypeToPropertyType<std::uint64_t> { static constexpr PropertyType value = PropertyType::Hash; };

/// Parses a token string into a value of `type` (upstream propertyValueFromString); kInvalidPropertyValue on
/// failure (flexible types never parse).
PropertyValue propertyValueFromString(const std::string& str, PropertyType type);
/// An empty PropertyVector of the storage type of `type`.
PropertyVector propertyVectorFromType(PropertyType type);
/// The storage type of `type` holds `value`'s alternative.
bool valueMatchesType(const PropertyValue& value, PropertyType type);
/// Text form of a value for diagnostics and the per-frame JSON (floats with shortest round-trip digits).
std::string formatPropertyValue(const PropertyValue& value, PropertyType type);

/// Builds a value of storage type T from an ambiguous literal (`0`, `1.f`, `"text"`): converts when T is
/// constructible from E, else default-constructs (upstream propertyValueForceType).
template <typename T, typename E>
PropertyValue propertyValueForceType(const E& value) {
    if constexpr (std::is_same_v<T, std::string> && !std::is_constructible_v<std::string, E>) {
        return PropertyValue(std::in_place_type<T>, T());
    } else if constexpr (std::is_arithmetic_v<T> && std::is_arithmetic_v<E>) {
        return PropertyValue(std::in_place_type<T>, static_cast<T>(value));
    } else if constexpr (std::is_constructible_v<T, E>) {
        return PropertyValue(std::in_place_type<T>, T(value));
    } else {
        return PropertyValue(std::in_place_type<T>, T());
    }
}

/// Converts limit metadata (hardMin, ...) to the property's numeric type; non-numeric values stay (upstream
/// convertPropertyValueToType).
template <typename Target>
PropertyValue convertPropertyValueToType(const PropertyValue& value) {
    if constexpr (std::is_arithmetic_v<Target>) {
        return std::visit(
            [](const auto& v) -> PropertyValue {
                using S = std::decay_t<decltype(v)>;
                if constexpr (std::is_arithmetic_v<S>) {
                    return PropertyValue(std::in_place_type<Target>, static_cast<Target>(v));
                } else {
                    return PropertyValue(v);
                }
            },
            value);
    } else {
        return value;
    }
}

using ComponentType = std::uint64_t; ///< XXH3_64bits of the full component name
inline constexpr ComponentType kInvalidComponentType = 0;

/// USD prim types a Prim property may target (OGN metadata; informational at runtime).
enum class PrimType : std::uint32_t {
    UsdGeomMesh = 0,
    UsdLuxSphereLight = 1,
    UsdLuxCylinderLight = 2,
    UsdLuxDiskLight = 3,
    UsdLuxDistantLight = 4,
    UsdLuxRectLight = 5,
    OmniGraph = 6,
};
const char* primTypeName(PrimType t);

struct PropertySpec {
    static constexpr std::string_view kUsdNamePrefix = "lightspeed.trex.logic.";

    PropertySpec() = default;
    PropertySpec(PropertyType type_, PropertyValue defaultValue_, PropertyIOType ioType_, std::string name_,
                 std::string usdPropertyName_, std::string_view uiName_, std::string_view docString_,
                 PropertyType declaredType_)
        : type(type_), defaultValue(std::move(defaultValue_)), ioType(ioType_), name(std::move(name_)),
          usdPropertyName(std::move(usdPropertyName_)), uiName(uiName_), docString(docString_),
          declaredType(declaredType_) {}

    PropertyType type = PropertyType::Float; ///< resolved concrete type (flexible declarations: the variant's)
    PropertyValue defaultValue;
    PropertyIOType ioType = PropertyIOType::Input;
    std::string name;
    std::string usdPropertyName; ///< "inputs:<name>" (inputs and states) or "outputs:<name>"
    std::string_view uiName;
    std::string_view docString;
    PropertyType declaredType = PropertyType::Float; ///< as declared (Any / NumberOrVector for flexible ones)

    // Optional values (set from the component macros as `property.<name> = <value>`).
    std::vector<std::string> oldUsdNames; ///< renamed properties: old USD names (prefixed at registration)
    PropertyValue hardMin = kFalsePropertyValue, hardMax = kFalsePropertyValue;
    PropertyValue softMin = kFalsePropertyValue, softMax = kFalsePropertyValue, uiStep = kFalsePropertyValue;
    bool optional = false;
    bool isSettableOutput = false; ///< constants: the input is also readable as an output

    struct EnumProperty {
        template <typename T, typename = std::enable_if_t<std::is_enum_v<T>>>
        EnumProperty(const T& v, const std::string& doc)
            : value(std::in_place_type<std::uint32_t>, static_cast<std::uint32_t>(v)), docString(doc) {
            static_assert(std::is_same_v<std::underlying_type_t<T>, std::uint32_t>, "enum properties are uint32_t");
        }
        PropertyValue value;
        std::string docString;
    };
    using EnumPropertyMap = std::map<std::string, EnumProperty>;
    EnumPropertyMap enumValues;
    bool treatAsColor = false;
    std::vector<PrimType> allowedPrimTypes;

    bool isValid() const { return !name.empty() && !usdPropertyName.empty(); }
};

class ComponentBatch;
using CreateComponentBatchFunc = std::unique_ptr<ComponentBatch> (*)(const GraphBatch& batch, std::vector<PropertyVector>& values,
                                                                      const std::vector<std::size_t>& indices);
using ApplySceneOverridesFunc = void (*)(const LogicContext& ctx, ComponentBatch& batch, std::size_t start, std::size_t end);
using InitializeFunc = void (*)(const LogicContext& ctx, ComponentBatch& batch, std::size_t index);
using CleanupFunc = void (*)(ComponentBatch& batch, std::size_t index);

struct ComponentSpec {
    ComponentSpec() = default;
    ComponentSpec(std::vector<PropertySpec> properties_, ComponentType componentType_, int version_, std::string name_,
                  std::string_view uiName_, std::string_view categories_, std::string_view docString_,
                  std::map<std::string, PropertyType> resolvedTypes_, CreateComponentBatchFunc create_)
        : properties(std::move(properties_)), componentType(componentType_), version(version_), name(std::move(name_)),
          uiName(uiName_), categories(categories_), docString(docString_), resolvedTypes(std::move(resolvedTypes_)),
          createComponentBatch(create_) {}

    std::vector<PropertySpec> properties;
    ComponentType componentType = kInvalidComponentType;
    int version = 0;
    std::string name; ///< full name, "lightspeed.trex.logic.<Class>"
    std::string_view uiName;
    std::string_view categories; ///< "Sense", "Transform", "Act", "Constants" (upstream's UI categories)
    std::string_view docString;
    std::map<std::string, PropertyType> resolvedTypes; ///< templated components: property -> concrete type
    CreateComponentBatchFunc createComponentBatch = nullptr;

    // Optional values (`spec.<name> = <value>` in the component macros).
    std::vector<std::string> oldNames; ///< renamed components (without the prefix)
    ApplySceneOverridesFunc applySceneOverrides = nullptr;
    InitializeFunc initialize = nullptr;
    CleanupFunc cleanup = nullptr;

    bool isValid() const;
    std::string getClassName() const;
};

/// Registry (explicit; see the header comment). Idempotent per spec pointer.
void registerComponentSpec(const ComponentSpec* spec);
/// First registered variant of a component type (nullptr when unknown).
const ComponentSpec* getComponentSpec(ComponentType componentType);
/// Every variant of a component type (empty when unknown), in registration order.
const std::vector<const ComponentSpec*>& getAllComponentSpecVariants(ComponentType componentType);
const ComponentSpec* getAnyComponentSpecVariant(ComponentType componentType);
/// XXH3_64bits of `fullName`.
ComponentType componentTypeFromName(std::string_view fullName);
/// Every registered component (one spec per base type, by full name; old names excluded).
std::vector<const ComponentSpec*> listComponents();

/// What components a graph contains and how their properties connect.
struct GraphTopology {
    std::vector<PropertyType> propertyTypes;
    std::map<std::string, std::size_t> propertyPathToIndex; ///< "<node path>.<usd property name>" -> index
    std::vector<std::vector<std::size_t>> propertyIndices;  ///< per component: its properties' indices
    std::vector<const ComponentSpec*> componentSpecs;       ///< in evaluation (topological) order
    std::vector<std::string> nodePaths;                     ///< per component: the node prim path
    std::uint64_t graphHash = 0; ///< same hash => same topology (depends on prim order, as upstream)
};

/// The initial values of a graph instance.
struct GraphState {
    std::shared_ptr<const GraphTopology> topology;
    std::vector<PropertyValue> values;
    std::string primPath;
};

class ComponentBatch {
public:
    virtual ~ComponentBatch() = default;
    /// Updates instances [start, end).
    virtual void updateRange(const LogicContext& ctx, std::size_t start, std::size_t end) = 0;
    virtual const ComponentSpec* getSpec() const = 0;
};

template <typename Derived>
class RegisteredComponentBatch : public ComponentBatch {
public:
    const ComponentSpec* getSpec() const final { return Derived::getStaticSpec(); }
    static void registerType() { registerComponentSpec(Derived::getStaticSpec()); }
};

} // namespace fuse::relight::logic
