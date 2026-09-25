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
// Ported from dxvk-remix src/dxvk/rtx_render/graph/rtx_graph_component_macros.h@0867d3c and
// rtx_graph_flexible_types.h@0867d3c
//
// FUSE Relight RL-3.5: the X-macro machinery that turns a component's property lists into its class (typed
// references into the batch's property vectors), its ComponentSpec and its factory. Private to the logic
// library (src/). Differences from upstream: macros are FUSE_LOGIC_-prefixed; updateRange takes the
// LogicContext; variadic arguments rely on C++20 empty __VA_ARGS__ instead of `, ##__VA_ARGS__`; flexible
// variants are registered by explicit calls (registerBinaryOpVariants / registerComparisonOpVariants) instead of
// static initialisers.
//
// A property list entry is X(type, defaultValue, name, uiName, docString, optional `property.<field> = value`...).
// The component body may end with optional `spec.<field> = value;` statements.
#pragma once

#include <fuse/relight/logic/graph_batch.hpp>
#include <fuse/relight/logic/graph_types.hpp>
#include <fuse/relight/logic/logic_context.hpp>
#include <fuse/relight/logic/logic_log.hpp>

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#define FUSE_LOGIC_GENERATE_PROP_TYPE(propertyType, defaultValue, name, uiName, docString, ...) \
    static constexpr ::fuse::relight::logic::PropertyType name##PropertyType = propertyType;

#define FUSE_LOGIC_GENERATE_PROP_TYPES(X_INPUTS, X_STATES, X_OUTPUTS) \
    X_INPUTS(FUSE_LOGIC_GENERATE_PROP_TYPE)                             \
    X_STATES(FUSE_LOGIC_GENERATE_PROP_TYPE)                             \
    X_OUTPUTS(FUSE_LOGIC_GENERATE_PROP_TYPE)

#define FUSE_LOGIC_GENERATE_CLASS_TYPE(propertyType, defaultValue, name, uiName, docString, ...) \
    using name##CppType = ::fuse::relight::logic::PropertyTypeToCppType<name##PropertyType>;

#define FUSE_LOGIC_GENERATE_CONST_MEMBER(propertyType, defaultValue, name, uiName, docString, ...) \
    const std::vector<name##CppType>& m_##name;

#define FUSE_LOGIC_GENERATE_MUTABLE_MEMBER(propertyType, defaultValue, name, uiName, docString, ...) \
    std::vector<name##CppType>& m_##name;

#define FUSE_LOGIC_GENERATE_CTOR_ARG(propertyType, defaultValue, name, uiName, docString, ...) \
    m_##name(*std::get_if<std::vector<name##CppType>>(&values[indices[ctorIndex++]])),

#define FUSE_LOGIC_GENERATE_PROPERTY_SPEC(ioType, usdPrefix, propertyType, defaultValue, name, uiName, docString) \
    ::fuse::relight::logic::PropertySpec{                                                                        \
        name##PropertyType,                                                                                      \
        ::fuse::relight::logic::propertyValueForceType<name##CppType>(defaultValue),                            \
        ::fuse::relight::logic::PropertyIOType::ioType,                                                          \
        #name,                                                                                                   \
        usdPrefix #name,                                                                                         \
        uiName,                                                                                                  \
        docString,                                                                                               \
        propertyType,                                                                                            \
    },

#define FUSE_LOGIC_GENERATE_PROPERTY_SPEC_INPUT(propertyType, defaultValue, name, uiName, docString, ...) \
    FUSE_LOGIC_GENERATE_PROPERTY_SPEC(Input, "inputs:", propertyType, defaultValue, name, uiName, docString)
// States use the "inputs:" prefix, as OmniGraph requires (upstream note).
#define FUSE_LOGIC_GENERATE_PROPERTY_SPEC_STATE(propertyType, defaultValue, name, uiName, docString, ...) \
    FUSE_LOGIC_GENERATE_PROPERTY_SPEC(State, "inputs:", propertyType, defaultValue, name, uiName, docString)
#define FUSE_LOGIC_GENERATE_PROPERTY_SPEC_OUTPUT(propertyType, defaultValue, name, uiName, docString, ...) \
    FUSE_LOGIC_GENERATE_PROPERTY_SPEC(Output, "outputs:", propertyType, defaultValue, name, uiName, docString)

#define FUSE_LOGIC_GENERATE_RESOLVED_TYPE(propertyType, defaultValue, name, uiName, docString, ...) {#name, name##PropertyType},

#define FUSE_LOGIC_GENERATE_OPTIONAL_SPEC_PROPERTIES(propertyType, defaultValue, name, uiName, docString, ...)          \
    {                                                                                                                  \
        ::fuse::relight::logic::PropertySpec& property = s_spec.properties[index];                                     \
        (void)property;                                                                                                \
        __VA_ARGS__;                                                                                                   \
        using namespace ::fuse::relight::logic;                                                                        \
        if (property.hardMin != kFalsePropertyValue) property.hardMin = convertPropertyValueToType<name##CppType>(property.hardMin); \
        if (property.hardMax != kFalsePropertyValue) property.hardMax = convertPropertyValueToType<name##CppType>(property.hardMax); \
        if (property.softMin != kFalsePropertyValue) property.softMin = convertPropertyValueToType<name##CppType>(property.softMin); \
        if (property.softMax != kFalsePropertyValue) property.softMax = convertPropertyValueToType<name##CppType>(property.softMax); \
        if (property.uiStep != kFalsePropertyValue) property.uiStep = convertPropertyValueToType<name##CppType>(property.uiStep);    \
        if (!property.oldUsdNames.empty()) {                                                                           \
            const std::string prefix = property.ioType == PropertyIOType::Output ? "outputs:" : "inputs:";             \
            for (std::string& oldName : property.oldUsdNames) {                                                        \
                oldName = prefix + oldName;                                                                            \
            }                                                                                                          \
        }                                                                                                              \
        index++;                                                                                                       \
    }

// The common component body (types, members, constructor, factory and spec). Used directly by templated
// (flexible) components and by components with initialize / cleanup / applySceneOverrides callbacks.
#define FUSE_LOGIC_COMPONENT_BODY(componentClass, uiNameText, categoriesText, docText, versionNumber, X_INPUTS, X_STATES, X_OUTPUTS, ...) \
protected: /* not private: components that ignore a property must not trip clang's -Wunused-private-field */         \
    X_INPUTS(FUSE_LOGIC_GENERATE_CLASS_TYPE)                                                                      \
    X_STATES(FUSE_LOGIC_GENERATE_CLASS_TYPE)                                                                      \
    X_OUTPUTS(FUSE_LOGIC_GENERATE_CLASS_TYPE)                                                                     \
    X_INPUTS(FUSE_LOGIC_GENERATE_CONST_MEMBER)                                                                    \
    X_STATES(FUSE_LOGIC_GENERATE_MUTABLE_MEMBER)                                                                  \
    X_OUTPUTS(FUSE_LOGIC_GENERATE_MUTABLE_MEMBER)                                                                 \
    const ::fuse::relight::logic::GraphBatch& m_batch;                                                            \
                                                                                                                  \
public:                                                                                                           \
    componentClass(const ::fuse::relight::logic::GraphBatch& batch, std::vector<::fuse::relight::logic::PropertyVector>& values, \
                   const std::vector<std::size_t>& indices, std::size_t& ctorIndex)                               \
        : X_INPUTS(FUSE_LOGIC_GENERATE_CTOR_ARG) X_STATES(FUSE_LOGIC_GENERATE_CTOR_ARG)                           \
              X_OUTPUTS(FUSE_LOGIC_GENERATE_CTOR_ARG) m_batch(batch) {                                            \
        (void)values;                                                                                             \
        (void)indices;                                                                                            \
        (void)ctorIndex;                                                                                          \
    }                                                                                                             \
    static std::unique_ptr<::fuse::relight::logic::ComponentBatch> createBatch(                                   \
        const ::fuse::relight::logic::GraphBatch& batch, std::vector<::fuse::relight::logic::PropertyVector>& values, \
        const std::vector<std::size_t>& indices) {                                                                \
        std::size_t ctorIndex = 0;                                                                                \
        return std::make_unique<componentClass>(batch, values, indices, ctorIndex);                               \
    }                                                                                                             \
    static const ::fuse::relight::logic::ComponentSpec* getStaticSpec() {                                         \
        static std::once_flag s_onceFlag;                                                                         \
        static const std::string s_fullName = std::string(::fuse::relight::logic::PropertySpec::kUsdNamePrefix) + #componentClass; \
        static ::fuse::relight::logic::ComponentSpec s_spec = {                                                   \
            {X_INPUTS(FUSE_LOGIC_GENERATE_PROPERTY_SPEC_INPUT) X_STATES(FUSE_LOGIC_GENERATE_PROPERTY_SPEC_STATE)  \
                 X_OUTPUTS(FUSE_LOGIC_GENERATE_PROPERTY_SPEC_OUTPUT)},                                            \
            ::fuse::relight::logic::componentTypeFromName(s_fullName),                                            \
            versionNumber,                                                                                        \
            s_fullName,                                                                                           \
            uiNameText,                                                                                           \
            categoriesText,                                                                                       \
            docText,                                                                                              \
            {X_INPUTS(FUSE_LOGIC_GENERATE_RESOLVED_TYPE) X_STATES(FUSE_LOGIC_GENERATE_RESOLVED_TYPE)              \
                 X_OUTPUTS(FUSE_LOGIC_GENERATE_RESOLVED_TYPE)},                                                   \
            &componentClass::createBatch,                                                                         \
        };                                                                                                        \
        std::call_once(s_onceFlag, [&]() {                                                                        \
            if (!s_spec.properties.empty()) {                                                                     \
                std::size_t index = 0;                                                                            \
                X_INPUTS(FUSE_LOGIC_GENERATE_OPTIONAL_SPEC_PROPERTIES)                                            \
                X_STATES(FUSE_LOGIC_GENERATE_OPTIONAL_SPEC_PROPERTIES)                                            \
                X_OUTPUTS(FUSE_LOGIC_GENERATE_OPTIONAL_SPEC_PROPERTIES)                                           \
                (void)index;                                                                                      \
            }                                                                                                     \
            {                                                                                                     \
                ::fuse::relight::logic::ComponentSpec& spec = s_spec;                                             \
                (void)spec;                                                                                       \
                __VA_ARGS__;                                                                                      \
            }                                                                                                     \
            if (!s_spec.isValid()) {                                                                              \
                ::fuse::relight::logic::logMessage(::fuse::relight::logic::LogSeverity::Error,                    \
                                                   "Invalid component spec for " #componentClass);                \
            }                                                                                                     \
        });                                                                                                       \
        return &s_spec;                                                                                           \
    }

// A component with concrete types: declares the class; updateRange is defined after the macro.
#define FUSE_LOGIC_COMPONENT(componentClass, uiNameText, categoriesText, docText, versionNumber, X_INPUTS, X_STATES, X_OUTPUTS, ...) \
    class componentClass : public ::fuse::relight::logic::RegisteredComponentBatch<componentClass> {             \
    private:                                                                                                      \
        FUSE_LOGIC_GENERATE_PROP_TYPES(X_INPUTS, X_STATES, X_OUTPUTS)                                             \
        FUSE_LOGIC_COMPONENT_BODY(componentClass, uiNameText, categoriesText, docText, versionNumber, X_INPUTS, X_STATES, X_OUTPUTS, __VA_ARGS__) \
        void updateRange(const ::fuse::relight::logic::LogicContext& ctx, std::size_t start, std::size_t end) final; \
    };

namespace fuse::relight::logic {

// ---- Flexible variants (upstream rtx_graph_flexible_types.h) ---------------------------------------------------
// A binary operator component is registered for every operand pair (A, B) of PropertyNumberOrVector for which the
// C++ expression is valid; the result type is the expression's type (e.g. Vector3 * float -> Vector3).

template <typename A, typename B, template <typename, typename> typename OpCheck, typename = void>
struct IsBinaryOpValid : std::false_type {};
template <typename A, typename B, template <typename, typename> typename OpCheck>
struct IsBinaryOpValid<A, B, OpCheck, std::void_t<OpCheck<A, B>>> : std::true_type {};

template <template <PropertyType, PropertyType, PropertyType> typename Component, template <typename, typename> typename OpCheck,
          typename A, typename B>
void registerBinaryOpIfValid() {
    if constexpr (IsBinaryOpValid<A, B, OpCheck>::value) {
        using R = OpCheck<A, B>;
        Component<CppTypeToPropertyType<A>::value, CppTypeToPropertyType<B>::value, CppTypeToPropertyType<R>::value>::registerType();
    }
}
template <template <PropertyType, PropertyType, PropertyType> typename Component, template <typename, typename> typename OpCheck,
          typename A, typename... Bs>
void registerBinaryOpForAllB() {
    (registerBinaryOpIfValid<Component, OpCheck, A, Bs>(), ...);
}
template <template <PropertyType, PropertyType, PropertyType> typename Component, template <typename, typename> typename OpCheck,
          typename... Ts>
void registerBinaryOpVariants(std::variant<Ts...>*) {
    (registerBinaryOpForAllB<Component, OpCheck, Ts, Ts...>(), ...);
}

template <template <PropertyType, PropertyType> typename Component, template <typename, typename> typename OpCheck,
          typename A, typename B>
void registerComparisonOpIfValid() {
    if constexpr (IsBinaryOpValid<A, B, OpCheck>::value) {
        Component<CppTypeToPropertyType<A>::value, CppTypeToPropertyType<B>::value>::registerType();
    }
}
template <template <PropertyType, PropertyType> typename Component, template <typename, typename> typename OpCheck,
          typename A, typename... Bs>
void registerComparisonOpForAllB() {
    (registerComparisonOpIfValid<Component, OpCheck, A, Bs>(), ...);
}
template <template <PropertyType, PropertyType> typename Component, template <typename, typename> typename OpCheck,
          typename... Ts>
void registerComparisonOpVariants(std::variant<Ts...>*) {
    (registerComparisonOpForAllB<Component, OpCheck, Ts, Ts...>(), ...);
}

/// Registers Component<T> for every T in the list.
template <template <PropertyType> typename Component, PropertyType... Ts>
void registerVariants() {
    (Component<Ts>::registerType(), ...);
}

} // namespace fuse::relight::logic
