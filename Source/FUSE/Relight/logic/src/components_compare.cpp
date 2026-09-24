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
// Ported from dxvk-remix src/dxvk/rtx_render/graph/components/equal_to.{h,cpp}@0867d3c,
// {less_than,greater_than,between,bool_and,bool_or,bool_not,select}.h@0867d3c and rtx_component_list.cpp@0867d3c
//
// FUSE Relight RL-3.5: comparison, boolean and selection "Transform" components.
#include "component_list_internal.hpp"
#include "component_macros.hpp"

#include <cmath>
#include <utility>

namespace fuse::relight::logic::components {

namespace {

using PT = PropertyType;

constexpr bool isVectorPropertyType(PT t) { return t == PT::Float2 || t == PT::Float3 || t == PT::Float4; }

#define LIST_INPUTS(X)                                                                                                  \
    X(PT::NumberOrVector, 0, a, "A", "The first value to compare.")                                                     \
    X(PT::NumberOrVector, 0, b, "B", "The second value to compare.")                                                    \
    X(PT::Float, 0.00001f, tolerance, "Tolerance",                                                                      \
      "The tolerance for rounding errors. Math operations with floating point values are not exact, so equality "       \
      "comparisons should allow for slightly different values. If the difference between A and B is less than "        \
      "Tolerance, the result will be true.")
#define LIST_STATES(X)
#define LIST_OUTPUTS(X) X(PT::Bool, false, result, "Result", "True if A == B, false otherwise")
template <PT aPropertyType, PT bPropertyType>
class EqualTo : public RegisteredComponentBatch<EqualTo<aPropertyType, bPropertyType>> {
    static constexpr PT tolerancePropertyType = PT::Float;
    static constexpr PT resultPropertyType = PT::Bool;
    FUSE_LOGIC_COMPONENT_BODY(EqualTo, "Equal To", "Transform",
                              "Returns true if A is equal to B, false otherwise.\n\nFor floating point values, this performs exact "
                              "equality comparison. Vector == Vector compares all components.",
                              1, LIST_INPUTS, LIST_STATES, LIST_OUTPUTS)
    void updateRange(const LogicContext& /*ctx*/, std::size_t start, std::size_t end) final {
        for (std::size_t i = start; i < end; i++) {
            const auto diff = m_a[i] - m_b[i];
            if constexpr (isVectorPropertyType(aPropertyType)) {
                const float tolSqr = m_tolerance[i] * m_tolerance[i];
                m_result[i] = lengthSqr(diff) < tolSqr;
            } else {
                m_result[i] = std::abs(diff) < m_tolerance[i];
            }
        }
    }
};
#undef LIST_INPUTS
#undef LIST_STATES
#undef LIST_OUTPUTS
template <typename A, typename B>
using EqualToCheck = decltype(std::declval<A>() == std::declval<B>());

#define LIST_STATES(X)
#define LIST_INPUTS(X)                                          \
    X(PT::Float, 0.f, a, "A", "The first value to compare.")    \
    X(PT::Float, 0.f, b, "B", "The second value to compare.")
#define LIST_OUTPUTS(X) X(PT::Bool, false, result, "Result", "True if A < B, false otherwise")
FUSE_LOGIC_COMPONENT(LessThan, "Less Than", "Transform", "Returns true if A is less than B, false otherwise.", 1, LIST_INPUTS,
                     LIST_STATES, LIST_OUTPUTS)
#undef LIST_OUTPUTS
void LessThan::updateRange(const LogicContext& /*ctx*/, std::size_t start, std::size_t end) {
    for (std::size_t i = start; i < end; i++) {
        m_result[i] = m_a[i] < m_b[i];
    }
}

#define LIST_OUTPUTS(X) X(PT::Bool, false, result, "Result", "True if A > B, false otherwise")
FUSE_LOGIC_COMPONENT(GreaterThan, "Greater Than", "Transform", "Returns true if A is greater than B, false otherwise.", 1,
                     LIST_INPUTS, LIST_STATES, LIST_OUTPUTS)
#undef LIST_INPUTS
#undef LIST_OUTPUTS
void GreaterThan::updateRange(const LogicContext& /*ctx*/, std::size_t start, std::size_t end) {
    for (std::size_t i = start; i < end; i++) {
        m_result[i] = m_a[i] > m_b[i];
    }
}

#define LIST_INPUTS(X)                                                                        \
    X(PT::Float, 0.f, value, "Value", "The value to test.")                                   \
    X(PT::Float, 0.f, minValue, "Min Value", "The minimum value of the range (inclusive).")   \
    X(PT::Float, 1.f, maxValue, "Max Value", "The maximum value of the range (inclusive).")
#define LIST_OUTPUTS(X)                                                                                                \
    X(PT::Bool, false, result, "Result",                                                                               \
      "True if value is greater than or equal to Min Value AND less than or equal to Max Value.")
FUSE_LOGIC_COMPONENT(Between, "Between", "Transform",
                     "Tests if a value is within a range (inclusive).\n\nReturns true if the value is >= Min Value AND <= Max "
                     "Value. Combines greater-than-or-equal, less-than-or-equal, and boolean AND into a single component.",
                     1, LIST_INPUTS, LIST_STATES, LIST_OUTPUTS)
#undef LIST_INPUTS
#undef LIST_OUTPUTS
void Between::updateRange(const LogicContext& /*ctx*/, std::size_t start, std::size_t end) {
    for (std::size_t i = start; i < end; i++) {
        m_result[i] = (m_value[i] >= m_minValue[i]) && (m_value[i] <= m_maxValue[i]);
    }
}

#define LIST_INPUTS(X)                                    \
    X(PT::Bool, false, a, "A", "First boolean input.")    \
    X(PT::Bool, false, b, "B", "Second boolean input.")
#define LIST_OUTPUTS(X) X(PT::Bool, false, result, "Result", "The logical AND of A and B (true if both A and B are true).")
FUSE_LOGIC_COMPONENT(BoolAnd, "Bool AND", "Transform", "Returns true only if both A and B are true.", 1, LIST_INPUTS, LIST_STATES,
                     LIST_OUTPUTS)
#undef LIST_OUTPUTS
void BoolAnd::updateRange(const LogicContext& /*ctx*/, std::size_t start, std::size_t end) {
    for (std::size_t i = start; i < end; i++) {
        m_result[i] = m_a[i] && m_b[i];
    }
}

#define LIST_OUTPUTS(X) X(PT::Bool, false, result, "Result", "The logical OR of A and B (true if either A or B is true).")
FUSE_LOGIC_COMPONENT(BoolOr, "Bool OR", "Transform", "Returns true if either A or B (or both) are true.", 1, LIST_INPUTS,
                     LIST_STATES, LIST_OUTPUTS)
#undef LIST_INPUTS
#undef LIST_OUTPUTS
void BoolOr::updateRange(const LogicContext& /*ctx*/, std::size_t start, std::size_t end) {
    for (std::size_t i = start; i < end; i++) {
        m_result[i] = m_a[i] || m_b[i];
    }
}

#define LIST_INPUTS(X) X(PT::Bool, false, input, "Input", "Boolean input value.")
#define LIST_OUTPUTS(X) X(PT::Bool, true, result, "Result", "The logical NOT of the input (inverted boolean value).")
FUSE_LOGIC_COMPONENT(BoolNot, "Bool NOT", "Transform", "Flips a true/false value to its opposite.", 1, LIST_INPUTS, LIST_STATES,
                     LIST_OUTPUTS)
#undef LIST_INPUTS
#undef LIST_OUTPUTS
#undef LIST_STATES
void BoolNot::updateRange(const LogicContext& /*ctx*/, std::size_t start, std::size_t end) {
    for (std::size_t i = start; i < end; i++) {
        m_result[i] = !m_input[i];
    }
}

#define LIST_INPUTS(X)                                                                    \
    X(PT::Bool, false, condition, "Condition", "If true, output A. If false, output B.")  \
    X(PT::Any, 0.0f, inputA, "Input A", "The value to output when condition is true.")    \
    X(PT::Any, 0.0f, inputB, "Input B", "The value to output when condition is false.")
#define LIST_STATES(X)
#define LIST_OUTPUTS(X) X(PT::Any, 0.0f, output, "Output", "The selected value based on the condition.")
template <PT valueType>
class Select : public RegisteredComponentBatch<Select<valueType>> {
    static constexpr PT conditionPropertyType = PT::Bool;
    static constexpr PT inputAPropertyType = valueType;
    static constexpr PT inputBPropertyType = valueType;
    static constexpr PT outputPropertyType = valueType;
    FUSE_LOGIC_COMPONENT_BODY(Select, "Select", "Transform",
                              "Selects between two values based on a boolean condition.\n\nIf the condition is true, outputs Input "
                              "A. If the condition is false, outputs Input B. Acts like a ternary operator or if-else statement.",
                              1, LIST_INPUTS, LIST_STATES, LIST_OUTPUTS)
    void updateRange(const LogicContext& /*ctx*/, std::size_t start, std::size_t end) final {
        for (std::size_t i = start; i < end; i++) {
            m_output[i] = m_condition[i] ? m_inputA[i] : m_inputB[i];
        }
    }
};
#undef LIST_INPUTS
#undef LIST_STATES
#undef LIST_OUTPUTS

} // namespace

void registerCompareComponents() {
    registerComparisonOpVariants<EqualTo, EqualToCheck>(static_cast<PropertyNumberOrVector*>(nullptr));
    LessThan::registerType();
    GreaterThan::registerType();
    Between::registerType();
    BoolAnd::registerType();
    BoolOr::registerType();
    BoolNot::registerType();
    registerAnyVariants<Select>();
}

} // namespace fuse::relight::logic::components
