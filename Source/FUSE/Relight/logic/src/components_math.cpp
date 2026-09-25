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
// Ported from dxvk-remix src/dxvk/rtx_render/graph/components/{add,subtract,multiply,divide}.{h,cpp}@0867d3c,
// {clamp,min,max,invert,normalize,vector_length,floor,ceil,round}.h@0867d3c,
// {compose,decompose}_vector{2,3,4}.h@0867d3c and rtx_component_list.cpp@0867d3c (the variant lists)
//
// FUSE Relight RL-3.5: arithmetic and vector "Transform" components.
#include "component_list_internal.hpp"
#include "component_macros.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace fuse::relight::logic::components {

namespace {

using PT = PropertyType;

/// std::clamp without the lo <= hi precondition (the libstdc++ expression, so inverted ranges behave as upstream).
inline float clampStd(float v, float lo, float hi) { return v < lo ? lo : (hi < v ? hi : v); }

// ---- Add / Subtract / Multiply / Divide ------------------------------------------------------------------------

#define LIST_INPUTS(X)                                                  \
    X(PT::NumberOrVector, 0, a, "A", "The first value to be added.")    \
    X(PT::NumberOrVector, 0, b, "B", "The second value to be added.")
#define LIST_STATES(X)
#define LIST_OUTPUTS(X) X(PT::NumberOrVector, 0, sum, "Sum", "A + B")
template <PT aPropertyType, PT bPropertyType, PT sumPropertyType>
class Add : public RegisteredComponentBatch<Add<aPropertyType, bPropertyType, sumPropertyType>> {
    FUSE_LOGIC_COMPONENT_BODY(Add, "Add", "Transform",
                              "Adds two numbers or vectors together.\n\nVector + Number will add the number to all components "
                              "of the vector. Vector + Vector will add each piece separately, to create (a.x + b.x, a.y + b.y, "
                              "...). Vector + Vector will error if the vectors aren't the same size.",
                              1, LIST_INPUTS, LIST_STATES, LIST_OUTPUTS)
    void updateRange(const LogicContext& /*ctx*/, std::size_t start, std::size_t end) final {
        for (std::size_t i = start; i < end; i++) {
            m_sum[i] = m_a[i] + m_b[i];
        }
    }
};
#undef LIST_INPUTS
#undef LIST_STATES
#undef LIST_OUTPUTS
template <typename A, typename B>
using AddCheck = decltype(std::declval<A>() + std::declval<B>());

#define LIST_INPUTS(X)                                                  \
    X(PT::NumberOrVector, 0, a, "A", "The value to subtract from.")     \
    X(PT::NumberOrVector, 0, b, "B", "The value to subtract.")
#define LIST_STATES(X)
#define LIST_OUTPUTS(X) X(PT::NumberOrVector, 0, difference, "Difference", "A - B")
template <PT aPropertyType, PT bPropertyType, PT differencePropertyType>
class Subtract : public RegisteredComponentBatch<Subtract<aPropertyType, bPropertyType, differencePropertyType>> {
    FUSE_LOGIC_COMPONENT_BODY(Subtract, "Subtract", "Transform",
                              "Subtracts one number or vector from another.\n\nVector - Number will subtract the number from all "
                              "components of the vector. Vector - Vector will error if the vectors aren't the same size.",
                              1, LIST_INPUTS, LIST_STATES, LIST_OUTPUTS)
    void updateRange(const LogicContext& /*ctx*/, std::size_t start, std::size_t end) final {
        for (std::size_t i = start; i < end; i++) {
            m_difference[i] = m_a[i] - m_b[i];
        }
    }
};
#undef LIST_INPUTS
#undef LIST_STATES
#undef LIST_OUTPUTS
template <typename A, typename B>
using SubtractCheck = decltype(std::declval<A>() - std::declval<B>());

#define LIST_INPUTS(X)                                                        \
    X(PT::NumberOrVector, 0, a, "A", "The first value to be multiplied.")     \
    X(PT::NumberOrVector, 0, b, "B", "The second value to be multiplied.")
#define LIST_STATES(X)
#define LIST_OUTPUTS(X) X(PT::NumberOrVector, 0, product, "Product", "A * B")
template <PT aPropertyType, PT bPropertyType, PT productPropertyType>
class Multiply : public RegisteredComponentBatch<Multiply<aPropertyType, bPropertyType, productPropertyType>> {
    FUSE_LOGIC_COMPONENT_BODY(Multiply, "Multiply", "Transform",
                              "Multiplies two numbers or vectors together.\n\nVector * Number will scale all components of the "
                              "vector by the number. Vector * Vector will multiply each piece separately, to create (a.x * b.x, "
                              "a.y * b.y, ...). Vector * Vector will error if the vectors aren't the same size.",
                              1, LIST_INPUTS, LIST_STATES, LIST_OUTPUTS)
    void updateRange(const LogicContext& /*ctx*/, std::size_t start, std::size_t end) final {
        for (std::size_t i = start; i < end; i++) {
            m_product[i] = m_a[i] * m_b[i];
        }
    }
};
#undef LIST_INPUTS
#undef LIST_STATES
#undef LIST_OUTPUTS
template <typename A, typename B>
using MultiplyCheck = decltype(std::declval<A>() * std::declval<B>());

#define LIST_INPUTS(X)                                                        \
    X(PT::NumberOrVector, 0, a, "A", "The dividend (value to be divided).")   \
    X(PT::NumberOrVector, 0, b, "B", "The divisor (value to divide by).")
#define LIST_STATES(X)
#define LIST_OUTPUTS(X) X(PT::NumberOrVector, 0, quotient, "Quotient", "A / B")
template <PT aPropertyType, PT bPropertyType, PT quotientPropertyType>
class Divide : public RegisteredComponentBatch<Divide<aPropertyType, bPropertyType, quotientPropertyType>> {
    FUSE_LOGIC_COMPONENT_BODY(Divide, "Divide", "Transform",
                              "Divides one number or vector by another.\n\nVector / Number will divide all components of the "
                              "vector by the number. Vector / vector will divide each piece separately, to create (a.x / b.x, "
                              "a.y / b.y, ...). Vector / Vector will error if the vectors aren't the same size.\n\nNote: Division "
                              "by zero will produce infinity or NaN.",
                              1, LIST_INPUTS, LIST_STATES, LIST_OUTPUTS)
    void updateRange(const LogicContext& /*ctx*/, std::size_t start, std::size_t end) final {
        for (std::size_t i = start; i < end; i++) {
            m_quotient[i] = m_a[i] / m_b[i];
        }
    }
};
#undef LIST_INPUTS
#undef LIST_STATES
#undef LIST_OUTPUTS
template <typename A, typename B>
using DivideCheck = decltype(std::declval<A>() / std::declval<B>());

// ---- Per-component helpers of the NumberOrVector components -----------------------------------------------------

template <typename V, typename F>
V mapComponents(const V& v, F f) {
    if constexpr (std::is_same_v<V, float>) {
        return f(v, 0);
    } else if constexpr (std::is_same_v<V, Vector2>) {
        return Vector2(f(v.x, 0), f(v.y, 1));
    } else if constexpr (std::is_same_v<V, Vector3>) {
        return Vector3(f(v.x, 0), f(v.y, 1), f(v.z, 2));
    } else {
        return Vector4(f(v.x, 0), f(v.y, 1), f(v.z, 2), f(v.w, 3));
    }
}
template <typename V>
float componentOf(const V& v, int i) {
    if constexpr (std::is_same_v<V, float>) {
        (void)i;
        return v;
    } else {
        return v[static_cast<std::size_t>(i)];
    }
}

// ---- Clamp / Min / Max / Invert -----------------------------------------------------------------------------------

#define LIST_INPUTS(X)                                                                   \
    X(PT::NumberOrVector, 0.f, value, "Value", "The value to clamp.")                    \
    X(PT::Float, 0.f, minValue, "Min Value", "The minimum allowed value.")               \
    X(PT::Float, 1.f, maxValue, "Max Value", "The maximum allowed value.")
#define LIST_STATES(X)
#define LIST_OUTPUTS(X) X(PT::NumberOrVector, 0.f, result, "Result", "The clamped value, constrained to [Min Value, Max Value].")
template <PT valueType>
class Clamp : public RegisteredComponentBatch<Clamp<valueType>> {
    static constexpr PT valuePropertyType = valueType;
    static constexpr PT minValuePropertyType = PT::Float;
    static constexpr PT maxValuePropertyType = PT::Float;
    static constexpr PT resultPropertyType = valueType;
    FUSE_LOGIC_COMPONENT_BODY(Clamp, "Clamp", "Transform",
                              "Constrains a value to a specified range.\n\nIf the value is less than Min Value, returns Min Value. "
                              "If the value is greater than Max Value, returns Max Value. Otherwise, returns the value unchanged. "
                              "Applies to each component of a vector individually.",
                              1, LIST_INPUTS, LIST_STATES, LIST_OUTPUTS)
    void updateRange(const LogicContext& /*ctx*/, std::size_t start, std::size_t end) final {
        for (std::size_t i = start; i < end; i++) {
            const float lo = m_minValue[i], hi = m_maxValue[i];
            m_result[i] = mapComponents(m_value[i], [&](float c, int) { return clampStd(c, lo, hi); });
        }
    }
};
#undef LIST_INPUTS
#undef LIST_STATES
#undef LIST_OUTPUTS

#define LIST_INPUTS(X)                                         \
    X(PT::NumberOrVector, 0.f, a, "A", "The first value.")     \
    X(PT::NumberOrVector, 0.f, b, "B", "The second value.")
#define LIST_STATES(X)
#define LIST_OUTPUTS(X) X(PT::NumberOrVector, 0.f, result, "Result", "The minimum of A and B.")
template <PT valueType>
class Min : public RegisteredComponentBatch<Min<valueType>> {
    static constexpr PT aPropertyType = valueType;
    static constexpr PT bPropertyType = valueType;
    static constexpr PT resultPropertyType = valueType;
    FUSE_LOGIC_COMPONENT_BODY(Min, "Min", "Transform", "Returns the smaller of two values.\n\nOutputs the minimum value between A and B.",
                              1, LIST_INPUTS, LIST_STATES, LIST_OUTPUTS)
    void updateRange(const LogicContext& /*ctx*/, std::size_t start, std::size_t end) final {
        for (std::size_t i = start; i < end; i++) {
            const auto& b = m_b[i];
            m_result[i] = mapComponents(m_a[i], [&](float c, int k) { return std::min(c, componentOf(b, k)); });
        }
    }
};
#undef LIST_OUTPUTS
#define LIST_OUTPUTS(X) X(PT::NumberOrVector, 0.f, result, "Result", "The maximum of A and B.")
template <PT valueType>
class Max : public RegisteredComponentBatch<Max<valueType>> {
    static constexpr PT aPropertyType = valueType;
    static constexpr PT bPropertyType = valueType;
    static constexpr PT resultPropertyType = valueType;
    FUSE_LOGIC_COMPONENT_BODY(Max, "Max", "Transform", "Returns the larger of two values.\n\nOutputs the maximum value between A and B.",
                              1, LIST_INPUTS, LIST_STATES, LIST_OUTPUTS)
    void updateRange(const LogicContext& /*ctx*/, std::size_t start, std::size_t end) final {
        for (std::size_t i = start; i < end; i++) {
            const auto& b = m_b[i];
            m_result[i] = mapComponents(m_a[i], [&](float c, int k) { return std::max(c, componentOf(b, k)); });
        }
    }
};
#undef LIST_INPUTS
#undef LIST_STATES
#undef LIST_OUTPUTS

#define LIST_INPUTS(X) X(PT::NumberOrVector, 0.f, input, "Input", "The value to invert.")
#define LIST_STATES(X)
#define LIST_OUTPUTS(X) X(PT::NumberOrVector, 1.f, output, "Output", "1 - input")
template <PT valueType>
class Invert : public RegisteredComponentBatch<Invert<valueType>> {
    static constexpr PT inputPropertyType = valueType;
    static constexpr PT outputPropertyType = valueType;
    FUSE_LOGIC_COMPONENT_BODY(Invert, "Invert", "Transform",
                              "Outputs 1 minus the input value.\n\nCalculates 1 - input. Useful for inverting normalized values "
                              "(e.g., turning 0.2 into 0.8).",
                              1, LIST_INPUTS, LIST_STATES, LIST_OUTPUTS)
    void updateRange(const LogicContext& /*ctx*/, std::size_t start, std::size_t end) final {
        for (std::size_t i = start; i < end; i++) {
            if constexpr (valueType == PT::Float) {
                m_output[i] = 1.0f - m_input[i];
            } else {
                m_output[i] = outputCppType(1.0f) - m_input[i];
            }
        }
    }
};
#undef LIST_INPUTS
#undef LIST_STATES
#undef LIST_OUTPUTS

// ---- Normalize / VectorLength ---------------------------------------------------------------------------------------

#define LIST_INPUTS(X) X(PT::NumberOrVector, Vector3(0.0f, 0.0f, 1.0f), input, "Input", "The vector to normalize.")
#define LIST_STATES(X)
#define LIST_OUTPUTS(X)                                                                                          \
    X(PT::NumberOrVector, Vector3(0.0f, 0.0f, 1.0f), output, "Output",                                           \
      "The normalized vector with length 1. Returns (0,1), (0,0,1), or (0,0,0,1) if the input vector has zero length.")
template <PT valueType>
class Normalize : public RegisteredComponentBatch<Normalize<valueType>> {
    static constexpr PT inputPropertyType = valueType;
    static constexpr PT outputPropertyType = valueType;
    FUSE_LOGIC_COMPONENT_BODY(Normalize, "Normalize", "Transform",
                              "Normalizes a vector to have length 1.\n\nDivides the vector by its length to produce a unit vector "
                              "(length 1) in the same direction. If the input vector has zero length, returns a default vector "
                              "to avoid division by zero.",
                              1, LIST_INPUTS, LIST_STATES, LIST_OUTPUTS)
    void updateRange(const LogicContext& /*ctx*/, std::size_t start, std::size_t end) final {
        for (std::size_t i = start; i < end; i++) {
            const auto vec = m_input[i];
            const float len = length(vec);
            if (len > 1e-8f) {
                m_output[i] = vec / len;
            } else if constexpr (valueType == PT::Float2) {
                m_output[i] = Vector2(0.0f, 1.0f);
            } else if constexpr (valueType == PT::Float3) {
                m_output[i] = Vector3(0.0f, 0.0f, 1.0f);
            } else {
                m_output[i] = Vector4(0.0f, 0.0f, 0.0f, 1.0f);
            }
        }
    }
};
#undef LIST_INPUTS
#undef LIST_STATES
#undef LIST_OUTPUTS

#define LIST_INPUTS(X) X(PT::NumberOrVector, 0.0f, input, "Input", "The value to measure. For vectors, returns length.")
#define LIST_STATES(X)
#define LIST_OUTPUTS(X) X(PT::Float, 0.0f, length, "Length", "The length (magnitude) of the input vector.")
template <PT valueType>
class VectorLength : public RegisteredComponentBatch<VectorLength<valueType>> {
    static constexpr PT inputPropertyType = valueType;
    static constexpr PT lengthPropertyType = PT::Float;
    FUSE_LOGIC_COMPONENT_BODY(VectorLength, "Vector Length", "Transform",
                              "Calculates the length (magnitude) of a vector.\n\nComputes the Euclidean length of the vector using "
                              "the formula: sqrt(x\xC2\xB2 + y\xC2\xB2 + z\xC2\xB2 + ...).",
                              1, LIST_INPUTS, LIST_STATES, LIST_OUTPUTS)
    void updateRange(const LogicContext& /*ctx*/, std::size_t start, std::size_t end) final {
        for (std::size_t i = start; i < end; i++) {
            m_length[i] = logic::length(m_input[i]);
        }
    }
};
#undef LIST_INPUTS
#undef LIST_STATES
#undef LIST_OUTPUTS

// ---- Floor / Ceil / Round -----------------------------------------------------------------------------------------

#define LIST_INPUTS(X) X(PT::Float, 0.f, input, "Input", "The value to round down.")
#define LIST_STATES(X)
#define LIST_OUTPUTS(X) X(PT::Float, 0.f, result, "Result", "The input value rounded down to the previous integer.")
FUSE_LOGIC_COMPONENT(Floor, "Floor", "Transform",
                     "Rounds a value down to the previous integer.\n\nReturns the largest integer less than or equal to the "
                     "input. For example: 1.1 becomes 1.0, 1.9 becomes 1.0, -1.1 becomes -2.0.",
                     1, LIST_INPUTS, LIST_STATES, LIST_OUTPUTS)
#undef LIST_INPUTS
#undef LIST_OUTPUTS
void Floor::updateRange(const LogicContext& /*ctx*/, std::size_t start, std::size_t end) {
    for (std::size_t i = start; i < end; i++) {
        m_result[i] = std::floor(m_input[i]);
    }
}

#define LIST_INPUTS(X) X(PT::Float, 0.f, input, "Input", "The value to round up.")
#define LIST_OUTPUTS(X) X(PT::Float, 0.f, result, "Result", "The input value rounded up to the next integer.")
FUSE_LOGIC_COMPONENT(Ceil, "Ceil", "Transform",
                     "Rounds a value up to the next integer.\n\nReturns the smallest integer greater than or equal to the "
                     "input. For example: 1.1 becomes 2.0, 1.9 becomes 2.0, -1.1 becomes -1.0.",
                     1, LIST_INPUTS, LIST_STATES, LIST_OUTPUTS)
#undef LIST_INPUTS
#undef LIST_OUTPUTS
void Ceil::updateRange(const LogicContext& /*ctx*/, std::size_t start, std::size_t end) {
    for (std::size_t i = start; i < end; i++) {
        m_result[i] = std::ceil(m_input[i]);
    }
}

#define LIST_INPUTS(X) X(PT::Float, 0.f, input, "Input", "The value to round.")
#define LIST_OUTPUTS(X) X(PT::Float, 0.f, result, "Result", "The input value rounded to the nearest integer.")
FUSE_LOGIC_COMPONENT(Round, "Round", "Transform",
                     "Rounds a value to the nearest integer.\n\nRounds to the nearest whole number. For example: 1.4 becomes "
                     "1.0, 1.5 becomes 2.0, 1.6 becomes 2.0.",
                     1, LIST_INPUTS, LIST_STATES, LIST_OUTPUTS)
#undef LIST_INPUTS
#undef LIST_OUTPUTS
#undef LIST_STATES
void Round::updateRange(const LogicContext& /*ctx*/, std::size_t start, std::size_t end) {
    for (std::size_t i = start; i < end; i++) {
        m_result[i] = std::round(m_input[i]);
    }
}

// ---- Compose / Decompose ------------------------------------------------------------------------------------------

#define LIST_STATES(X)
#define LIST_INPUTS(X)                                            \
    X(PT::Float, 0.0f, x, "X", "The X component of the vector.")  \
    X(PT::Float, 0.0f, y, "Y", "The Y component of the vector.")
#define LIST_OUTPUTS(X) X(PT::Float2, Vector2(0.0f), vector, "Vector", "The composed vector.")
FUSE_LOGIC_COMPONENT(ComposeVector2, "Compose Vector2", "Transform", "Combines two separate numbers into a single Vector2.", 1,
                     LIST_INPUTS, LIST_STATES, LIST_OUTPUTS)
#undef LIST_INPUTS
#undef LIST_OUTPUTS
void ComposeVector2::updateRange(const LogicContext& /*ctx*/, std::size_t start, std::size_t end) {
    for (std::size_t i = start; i < end; i++) {
        m_vector[i] = Vector2(m_x[i], m_y[i]);
    }
}

#define LIST_INPUTS(X)                                                             \
    X(PT::Float, 0.0f, x, "X", "The X component of the vector (Red channel).")     \
    X(PT::Float, 0.0f, y, "Y", "The Y component of the vector (Green channel).")   \
    X(PT::Float, 0.0f, z, "Z", "The Z component of the vector (Blue channel).")
#define LIST_OUTPUTS(X) X(PT::Float3, Vector3(0.0f), vector, "Vector", "The composed vector.")
FUSE_LOGIC_COMPONENT(ComposeVector3, "Compose Vector3", "Transform", "Combines three separate numbers into a single Vector3.", 1,
                     LIST_INPUTS, LIST_STATES, LIST_OUTPUTS)
#undef LIST_INPUTS
#undef LIST_OUTPUTS
void ComposeVector3::updateRange(const LogicContext& /*ctx*/, std::size_t start, std::size_t end) {
    for (std::size_t i = start; i < end; i++) {
        m_vector[i] = Vector3(m_x[i], m_y[i], m_z[i]);
    }
}

#define LIST_INPUTS(X)                                                             \
    X(PT::Float, 0.0f, x, "X", "The X component of the vector (Red channel).")     \
    X(PT::Float, 0.0f, y, "Y", "The Y component of the vector (Green channel).")   \
    X(PT::Float, 0.0f, z, "Z", "The Z component of the vector (Blue channel).")    \
    X(PT::Float, 0.0f, w, "W", "The W component of the vector (Alpha channel).")
#define LIST_OUTPUTS(X) X(PT::Float4, Vector4(0.0f), vector, "Vector", "The composed vector.")
FUSE_LOGIC_COMPONENT(ComposeVector4, "Compose Vector4", "Transform", "Combines four separate numbers into a single Vector4.", 1,
                     LIST_INPUTS, LIST_STATES, LIST_OUTPUTS)
#undef LIST_INPUTS
#undef LIST_OUTPUTS
void ComposeVector4::updateRange(const LogicContext& /*ctx*/, std::size_t start, std::size_t end) {
    for (std::size_t i = start; i < end; i++) {
        m_vector[i] = Vector4(m_x[i], m_y[i], m_z[i], m_w[i]);
    }
}

#define LIST_INPUTS(X) X(PT::Float2, Vector2(0.0f), vector, "Vector", "The vector to decompose.")
#define LIST_OUTPUTS(X)                                            \
    X(PT::Float, 0.0f, x, "X", "The X component of the vector.")   \
    X(PT::Float, 0.0f, y, "Y", "The Y component of the vector.")
FUSE_LOGIC_COMPONENT(DecomposeVector2, "Decompose Vector2", "Transform", "Splits a Vector2 into two separate numbers.", 1,
                     LIST_INPUTS, LIST_STATES, LIST_OUTPUTS)
#undef LIST_INPUTS
#undef LIST_OUTPUTS
void DecomposeVector2::updateRange(const LogicContext& /*ctx*/, std::size_t start, std::size_t end) {
    for (std::size_t i = start; i < end; i++) {
        m_x[i] = m_vector[i].x;
        m_y[i] = m_vector[i].y;
    }
}

#define LIST_INPUTS(X) X(PT::Float3, Vector3(0.0f), vector, "Vector", "The vector to decompose.")
#define LIST_OUTPUTS(X)                                                            \
    X(PT::Float, 0.0f, x, "X", "The X component of the vector (Red channel).")     \
    X(PT::Float, 0.0f, y, "Y", "The Y component of the vector (Green channel).")   \
    X(PT::Float, 0.0f, z, "Z", "The Z component of the vector (Blue channel).")
FUSE_LOGIC_COMPONENT(DecomposeVector3, "Decompose Vector3", "Transform", "Splits a Vector3 into three separate numbers.", 1,
                     LIST_INPUTS, LIST_STATES, LIST_OUTPUTS)
#undef LIST_INPUTS
#undef LIST_OUTPUTS
void DecomposeVector3::updateRange(const LogicContext& /*ctx*/, std::size_t start, std::size_t end) {
    for (std::size_t i = start; i < end; i++) {
        m_x[i] = m_vector[i].x;
        m_y[i] = m_vector[i].y;
        m_z[i] = m_vector[i].z;
    }
}

#define LIST_INPUTS(X) X(PT::Float4, Vector4(0.0f), vector, "Vector", "The vector to decompose.")
#define LIST_OUTPUTS(X)                                                            \
    X(PT::Float, 0.0f, x, "X", "The X component of the vector (Red channel).")     \
    X(PT::Float, 0.0f, y, "Y", "The Y component of the vector (Green channel).")   \
    X(PT::Float, 0.0f, z, "Z", "The Z component of the vector (Blue channel).")    \
    X(PT::Float, 0.0f, w, "W", "The W component of the vector (Alpha channel).")
FUSE_LOGIC_COMPONENT(DecomposeVector4, "Decompose Vector4", "Transform", "Splits a Vector4 into four separate numbers.", 1,
                     LIST_INPUTS, LIST_STATES, LIST_OUTPUTS)
#undef LIST_INPUTS
#undef LIST_OUTPUTS
#undef LIST_STATES
void DecomposeVector4::updateRange(const LogicContext& /*ctx*/, std::size_t start, std::size_t end) {
    for (std::size_t i = start; i < end; i++) {
        m_x[i] = m_vector[i].x;
        m_y[i] = m_vector[i].y;
        m_z[i] = m_vector[i].z;
        m_w[i] = m_vector[i].w;
    }
}

} // namespace

void registerMathComponents() {
    registerBinaryOpVariants<Add, AddCheck>(static_cast<PropertyNumberOrVector*>(nullptr));
    registerBinaryOpVariants<Subtract, SubtractCheck>(static_cast<PropertyNumberOrVector*>(nullptr));
    registerBinaryOpVariants<Multiply, MultiplyCheck>(static_cast<PropertyNumberOrVector*>(nullptr));
    registerBinaryOpVariants<Divide, DivideCheck>(static_cast<PropertyNumberOrVector*>(nullptr));
    registerVariants<Clamp, PT::Float, PT::Float2, PT::Float3, PT::Float4>();
    registerVariants<Invert, PT::Float, PT::Float2, PT::Float3, PT::Float4>();
    registerVariants<Max, PT::Float, PT::Float2, PT::Float3, PT::Float4>();
    registerVariants<Min, PT::Float, PT::Float2, PT::Float3, PT::Float4>();
    registerVariants<Normalize, PT::Float2, PT::Float3, PT::Float4>();
    registerVariants<VectorLength, PT::Float2, PT::Float3, PT::Float4>();
    Floor::registerType();
    Ceil::registerType();
    Round::registerType();
    ComposeVector2::registerType();
    ComposeVector3::registerType();
    ComposeVector4::registerType();
    DecomposeVector2::registerType();
    DecomposeVector3::registerType();
    DecomposeVector4::registerType();
}

} // namespace fuse::relight::logic::components
