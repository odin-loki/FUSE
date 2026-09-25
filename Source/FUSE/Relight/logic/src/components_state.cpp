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
// Ported from dxvk-remix src/dxvk/rtx_render/graph/components/{animation_utils,counter,toggle,count_toggles,
// conditionally_store,previous_frame_value,smooth,velocity,time,loop,remap}.h@0867d3c and rtx_component_list.cpp@0867d3c
//
// FUSE Relight RL-3.5: stateful and animation "Transform" components, plus Time ("Sense"). Time-based components
// read LogicContext::deltaTime() (upstream GlobalTime::deltaTime()).
//
// Floating point follows the upstream build (MSVC): pow / exp2 of float arguments are the float overloads, sin /
// cos of `x * M_PI` are evaluated in double and the result narrowed to float, so the easing curves match
// upstream's values bit for bit where the C library agrees.
#include <fuse/relight/logic/animation_utils.hpp>

#include "component_list_internal.hpp"
#include "component_macros.hpp"

#include <algorithm>
#include <cmath>

namespace fuse::relight::logic {

float applyInterpolation(InterpolationType interpolation, float time) {
    constexpr double kPiD = 3.14159265358979323846;
    switch (interpolation) {
    case InterpolationType::Linear:
        return time;
    case InterpolationType::Cubic:
        return time * time * time;
    case InterpolationType::EaseIn:
        return time * time;
    case InterpolationType::EaseOut:
        return 1.0f - (1.0f - time) * (1.0f - time);
    case InterpolationType::EaseInOut:
        return time < 0.5f ? 2.0f * time * time : 1.0f - 2.0f * (1.0f - time) * (1.0f - time);
    case InterpolationType::Sine:
        return static_cast<float>(std::sin(time * kPiD * 0.5f));
    case InterpolationType::Exponential:
        return time == 0.0f ? 0.0f : std::pow(2.0f, 10.0f * (time - 1.0f));
    case InterpolationType::Bounce:
        return static_cast<float>(1.0f - std::pow(1.0f - time, 3.0f) * std::cos(time * kPiD * 3.0f));
    case InterpolationType::Elastic:
        return static_cast<float>(std::pow(2.0f, -10.0f * time) * std::sin((time - 0.075f) * kPiD * 2.0f / 0.3f) + 1.0f);
    }
    return time; // fallback to linear
}

std::pair<float, bool> applyLooping(float value, float minRange, float maxRange, LoopingType loopingType) {
    if (maxRange == minRange && loopingType != LoopingType::NoLoop) {
        return {minRange, false}; // no range to loop in
    }
    const float range = maxRange - minRange;
    float normalizedValue = (value - minRange) / range;
    bool isReversing = false;
    switch (loopingType) {
    case LoopingType::Loop:
        normalizedValue = normalizedValue - std::floor(normalizedValue);
        break;
    case LoopingType::PingPong: {
        const float cyclePosition = normalizedValue - std::floor(normalizedValue / 2.0f) * 2.0f;
        if (cyclePosition >= 1.0f) {
            normalizedValue = 2.0f - cyclePosition;
            isReversing = true;
        } else {
            normalizedValue = cyclePosition;
        }
        break;
    }
    case LoopingType::NoLoop:
        return {value, false};
    case LoopingType::Clamp:
    default:
        normalizedValue = clampf(normalizedValue, 0.0f, 1.0f);
        break;
    }
    return {minRange + normalizedValue * range, isReversing};
}

const PropertySpec::EnumPropertyMap& loopingTypeEnumValues() {
    static const PropertySpec::EnumPropertyMap kValues{
        {"Loop", {LoopingType::Loop, "The value will wrap around from max to min."}},
        {"PingPong", {LoopingType::PingPong, "The value will bounce back and forth between min and max."}},
        {"NoLoop", {LoopingType::NoLoop, "The value will be unchanged."}},
        {"Clamp", {LoopingType::Clamp, "The value will be clamped to the range."}}};
    return kValues;
}

const PropertySpec::EnumPropertyMap& interpolationTypeEnumValues() {
    static const PropertySpec::EnumPropertyMap kValues{
        {"Linear", {InterpolationType::Linear, "The float will have a constant velocity."}},
        {"Cubic", {InterpolationType::Cubic, "The float will change in a cubic curve over time."}},
        {"EaseIn", {InterpolationType::EaseIn, "The float will start slow, then accelerate."}},
        {"EaseOut", {InterpolationType::EaseOut, "The float will start fast, then decelerate."}},
        {"EaseInOut", {InterpolationType::EaseInOut, "The float will start slow, accelerate, then decelerate."}},
        {"Sine", {InterpolationType::Sine, "Smooth, natural motion using a sine wave."}},
        {"Exponential", {InterpolationType::Exponential, "Dramatic acceleration effect."}},
        {"Bounce", {InterpolationType::Bounce, "Bouncy, playful motion."}},
        {"Elastic", {InterpolationType::Elastic, "Spring-like motion."}}};
    return kValues;
}

namespace components {

namespace {

using PT = PropertyType;

// ---- Counter ------------------------------------------------------------------------------------------------------

#define LIST_INPUTS(X)                                                                                                  \
    X(PT::Bool, false, increment, "Increment", "When true, the counter increments by the increment value each frame.") \
    X(PT::Float, 1.0f, incrementValue, "Increment Value", "The value to add to the counter each frame when increment is true.", \
      property.optional = true)                                                                                         \
    X(PT::Float, 0.0f, defaultValue, "Starting Value", "The initial value of the counter when the component is created.")
#define LIST_STATES(X) X(PT::Float, 0.0f, count, "", "The current counter value.")
#define LIST_OUTPUTS(X) X(PT::Float, 0.0f, value, "Value", "The current counter value.")
class Counter : public RegisteredComponentBatch<Counter> {
    FUSE_LOGIC_GENERATE_PROP_TYPES(LIST_INPUTS, LIST_STATES, LIST_OUTPUTS)
    FUSE_LOGIC_COMPONENT_BODY(Counter, "Counter", "Transform",
                              "Counts up by a value every frame when a condition is true.\n\nIncrements a counter by a specified "
                              "value every frame that the input bool is true. Use `Starting Value` to set the initial counter "
                              "value. Useful for tracking how many frames a condition has been active.",
                              1, LIST_INPUTS, LIST_STATES, LIST_OUTPUTS, spec.initialize = initialize)
    void updateRange(const LogicContext& /*ctx*/, std::size_t start, std::size_t end) final {
        for (std::size_t i = start; i < end; i++) {
            if (m_increment[i]) {
                m_count[i] += m_incrementValue[i];
            }
            m_value[i] = m_count[i];
        }
    }
    static void initialize(const LogicContext& /*ctx*/, ComponentBatch& batch, std::size_t index) {
        auto& self = static_cast<Counter&>(batch);
        self.m_count[index] = self.m_defaultValue[index];
        self.m_value[index] = self.m_count[index];
    }
};
#undef LIST_INPUTS
#undef LIST_STATES
#undef LIST_OUTPUTS

// ---- Toggle -------------------------------------------------------------------------------------------------------

#define LIST_INPUTS(X)                                                                                                  \
    X(PT::Bool, false, triggerToggle, "Trigger Toggle",                                                                 \
      "When this is true, the toggle switches to its opposite state (on becomes off, or off becomes on). Set this to "  \
      "true each time you want to flip the switch.")                                                                    \
    X(PT::Bool, false, defaultState, "Starting State",                                                                  \
      "The initial state of the toggle when the component is created. Set to true to start in the 'on' state, or "     \
      "false to start in the 'off' state.")
#define LIST_STATES(X)
#define LIST_OUTPUTS(X)                                                                                                 \
    X(PT::Bool, false, isOn, "Is On",                                                                                   \
      "The current state of the toggle: true means 'on', false means 'off'. This starts at the `Starting State` value " \
      "and changes each time `Trigger Toggle` becomes true.")
class Toggle : public RegisteredComponentBatch<Toggle> {
    FUSE_LOGIC_GENERATE_PROP_TYPES(LIST_INPUTS, LIST_STATES, LIST_OUTPUTS)
    FUSE_LOGIC_COMPONENT_BODY(Toggle, "Toggle", "Transform",
                              "A switch that alternates between on (true) and off (false) states.\n\nThink of this like a light "
                              "switch: each frame `Trigger Toggle` is true, the switch flips to the opposite position. Use "
                              "`Starting State` to choose whether the switch begins in the on or off position.",
                              1, LIST_INPUTS, LIST_STATES, LIST_OUTPUTS, spec.initialize = initialize)
    void updateRange(const LogicContext& /*ctx*/, std::size_t start, std::size_t end) final {
        for (std::size_t i = start; i < end; i++) {
            if (m_triggerToggle[i]) {
                m_isOn[i] = !m_isOn[i];
            }
        }
    }
    static void initialize(const LogicContext& /*ctx*/, ComponentBatch& batch, std::size_t index) {
        auto& self = static_cast<Toggle&>(batch);
        self.m_isOn[index] = self.m_defaultState[index];
    }
};
#undef LIST_INPUTS
#undef LIST_STATES
#undef LIST_OUTPUTS

// ---- CountToggles -------------------------------------------------------------------------------------------------

#define LIST_INPUTS(X)                                                                                                  \
    X(PT::Bool, false, value, "Value", "An input boolean.  Every time this goes from false to true, the count is incremented.") \
    X(PT::Float, 0.0f, resetValue, "Reset Value", "If count reaches this value, it is reset to 0.  Does nothing if left as 0.")
#define LIST_STATES(X) X(PT::Bool, true, prevFrameValue, "", "The value of the boolean from the previous frame.")
#define LIST_OUTPUTS(X) X(PT::Float, 0.0f, count, "Count", "The current count value.")
FUSE_LOGIC_COMPONENT(CountToggles, "Count Toggles", "Transform",
                     "Counts how many times an input switches from off to on.\n\nTracks the number of times a boolean input "
                     "transitions from false to true, useful for counting button presses or state changes.",
                     1, LIST_INPUTS, LIST_STATES, LIST_OUTPUTS)
#undef LIST_INPUTS
#undef LIST_STATES
#undef LIST_OUTPUTS
void CountToggles::updateRange(const LogicContext& /*ctx*/, std::size_t start, std::size_t end) {
    for (std::size_t i = start; i < end; i++) {
        if (m_value[i] && !m_prevFrameValue[i]) {
            m_count[i] += 1.0f;
            if (m_resetValue[i] > 0.0f && m_count[i] >= m_resetValue[i]) {
                m_count[i] = 0.0f;
            }
        }
        m_prevFrameValue[i] = m_value[i];
    }
}

// ---- ConditionallyStore / PreviousFrameValue ----------------------------------------------------------------------

#define LIST_INPUTS(X)                                                                                                    \
    X(PT::Bool, false, store, "Store", "If true, write the input value to state. If false, keep the previous stored value.") \
    X(PT::Any, 0.0f, input, "Input", "The value to store when store is true.")
#define LIST_STATES(X) X(PT::Any, 0.0f, storedValue, "", "The stored value.")
#define LIST_OUTPUTS(X) X(PT::Any, 0.0f, output, "Output", "The currently stored value.")
template <PT valueType>
class ConditionallyStore : public RegisteredComponentBatch<ConditionallyStore<valueType>> {
    static constexpr PT storePropertyType = PT::Bool;
    static constexpr PT inputPropertyType = valueType;
    static constexpr PT storedValuePropertyType = valueType;
    static constexpr PT outputPropertyType = valueType;
    FUSE_LOGIC_COMPONENT_BODY(ConditionallyStore, "Conditionally Store", "Transform",
                              "Stores a value when a condition is true, otherwise keeps the previous value.\n\nIf the store input "
                              "is true, captures the input value and stores it. If the store input is false, continues outputting "
                              "the previously stored value. Useful for sample-and-hold behavior.",
                              1, LIST_INPUTS, LIST_STATES, LIST_OUTPUTS)
    void updateRange(const LogicContext& /*ctx*/, std::size_t start, std::size_t end) final {
        for (std::size_t i = start; i < end; i++) {
            if (m_store[i]) {
                m_storedValue[i] = m_input[i];
            }
            m_output[i] = m_storedValue[i];
        }
    }
};
#undef LIST_INPUTS
#undef LIST_STATES
#undef LIST_OUTPUTS

#define LIST_INPUTS(X) X(PT::Any, 0.0f, input, "Input", "The value to store for the next frame.")
#define LIST_STATES(X) X(PT::Any, 0.0f, previousValue, "", "The value from the previous frame.")
#define LIST_OUTPUTS(X) X(PT::Any, 0.0f, output, "Output", "The value from the previous frame.")
template <PT valueType>
class PreviousFrameValue : public RegisteredComponentBatch<PreviousFrameValue<valueType>> {
    static constexpr PT inputPropertyType = valueType;
    static constexpr PT previousValuePropertyType = valueType;
    static constexpr PT outputPropertyType = valueType;
    FUSE_LOGIC_COMPONENT_BODY(PreviousFrameValue, "Previous Frame Value", "Transform",
                              "Outputs the value from the previous frame.\n\nStores the input value and outputs it on the next "
                              "frame. Useful for detecting changes between frames or implementing delay effects.",
                              1, LIST_INPUTS, LIST_STATES, LIST_OUTPUTS)
    void updateRange(const LogicContext& /*ctx*/, std::size_t start, std::size_t end) final {
        for (std::size_t i = start; i < end; i++) {
            m_output[i] = m_previousValue[i];
            m_previousValue[i] = m_input[i];
        }
    }
};
#undef LIST_INPUTS
#undef LIST_STATES
#undef LIST_OUTPUTS

// ---- Smooth / Velocity / Time --------------------------------------------------------------------------------------

#define LIST_INPUTS(X)                                                                                                  \
    X(PT::NumberOrVector, 0.0f, input, "Input", "The value to smooth.")                                                 \
    X(PT::Float, 0.1f, smoothingFactor, "Smoothing Factor",                                                             \
      "The smoothing factor (0-1000). 0 means output never changes. Larger values = faster changes.\n\n"                \
      "Time for output to be within 1% of input for different factors:\n- 1: 6.6 seconds\n- 10: 0.66 seconds\n"        \
      "- 100: 0.066 seconds\n- 1000: 0.0066 seconds\n\n"                                                                \
      "Formula: output = lerp(input, previousOutput, exp2(-smoothingFactor*deltaTime))",                                \
      property.hardMin = 0.0f, property.hardMax = 1000.0f, property.uiStep = 1.0f, property.optional = true)
#define LIST_STATES(X) X(PT::Bool, false, initialized, "", "Tracks if the smooth value has been initialized.")
#define LIST_OUTPUTS(X) X(PT::NumberOrVector, 0.0f, output, "Output", "The smoothed output value.")
template <PT valueType>
class Smooth : public RegisteredComponentBatch<Smooth<valueType>> {
    static constexpr PT inputPropertyType = valueType;
    static constexpr PT smoothingFactorPropertyType = PT::Float;
    static constexpr PT initializedPropertyType = PT::Bool;
    static constexpr PT outputPropertyType = valueType;
    FUSE_LOGIC_COMPONENT_BODY(Smooth, "Smooth", "Transform",
                              "Applies exponential smoothing to a value over time.\n\nUses a moving average filter to smooth out "
                              "rapid changes in the input value. The smoothing factor controls how much smoothing is applied: 0 "
                              "means output never changes. Larger values = faster changes. \n",
                              1, LIST_INPUTS, LIST_STATES, LIST_OUTPUTS)
    void updateRange(const LogicContext& ctx, std::size_t start, std::size_t end) final {
        const float deltaTime = ctx.deltaTime();
        for (std::size_t i = start; i < end; i++) {
            if (!m_initialized[i]) {
                m_output[i] = m_input[i];
                m_initialized[i] = true;
                continue;
            }
            float factor = std::clamp(m_smoothingFactor[i], 0.0f, 1000.0f);
            factor = std::exp2(-factor * deltaTime);
            m_output[i] = lerp(m_input[i], m_output[i], factor);
        }
    }
};
#undef LIST_INPUTS
#undef LIST_STATES
#undef LIST_OUTPUTS

#define LIST_INPUTS(X) X(PT::NumberOrVector, 0.0f, input, "Input", "The value to detect changes from.")
#define LIST_STATES(X) X(PT::NumberOrVector, 0.0f, previousValue, "", "The value from the previous frame.")
#define LIST_OUTPUTS(X)                                                                                                 \
    X(PT::NumberOrVector, 0.0f, velocity, "Velocity", "The change in value from the previous frame (current - previous) / deltaTime.")
template <PT valueType>
class Velocity : public RegisteredComponentBatch<Velocity<valueType>> {
    static constexpr PT inputPropertyType = valueType;
    static constexpr PT previousValuePropertyType = valueType;
    static constexpr PT velocityPropertyType = valueType;
    FUSE_LOGIC_COMPONENT_BODY(Velocity, "Velocity", "Transform",
                              "Detects the rate of change of a value from frame to frame.\n\nCalculates the difference between the "
                              "current value and the previous frame's value. Outputs the change per frame (velocity = (current - "
                              "previous) / deltaTime).",
                              1, LIST_INPUTS, LIST_STATES, LIST_OUTPUTS)
    void updateRange(const LogicContext& ctx, std::size_t start, std::size_t end) final {
        const float deltaTime = ctx.deltaTime();
        for (std::size_t i = start; i < end; i++) {
            m_velocity[i] = (m_input[i] - m_previousValue[i]) / deltaTime;
            m_previousValue[i] = m_input[i];
        }
    }
};
#undef LIST_INPUTS
#undef LIST_STATES
#undef LIST_OUTPUTS

#define LIST_INPUTS(X)                                                                                                  \
    X(PT::Bool, true, enabled, "Enabled", "If true, time accumulation continues. If false, time is paused.",            \
      property.optional = true)                                                                                         \
    X(PT::Bool, true, resetWhenDisabled, "Reset When Disabled",                                                         \
      "If true and `enabled` is false, the accumulated time is reset to 0.", property.optional = true)                  \
    X(PT::Float, 1.0f, speedMultiplier, "Speed Multiplier",                                                             \
      "Multiplier for time speed. 1.0 = normal speed, 2.0 = double speed, 0.5 = half speed.", property.hardMin = 0.0f,   \
      property.softMax = 3.0f, property.uiStep = 0.1f, property.optional = true)
#define LIST_STATES(X) X(PT::Float, 0.0f, accumulatedTime, "", "The accumulated time since component creation (in seconds).")
#define LIST_OUTPUTS(X) X(PT::Float, 0.0f, currentTime, "Current Time", "The time in seconds since component creation.")
FUSE_LOGIC_COMPONENT(Time, "Time", "Sense",
                     "Provides a continuously increasing time value for creating animations and time-based effects.\n\nOutputs the "
                     "time in seconds since the component was created. Can be paused and speed-adjusted.",
                     1, LIST_INPUTS, LIST_STATES, LIST_OUTPUTS)
#undef LIST_INPUTS
#undef LIST_STATES
#undef LIST_OUTPUTS
void Time::updateRange(const LogicContext& ctx, std::size_t start, std::size_t end) {
    const float deltaTime = ctx.deltaTime();
    for (std::size_t i = start; i < end; i++) {
        if (m_enabled[i]) {
            m_accumulatedTime[i] += deltaTime * std::max(0.0f, m_speedMultiplier[i]);
        } else if (m_resetWhenDisabled[i]) {
            m_accumulatedTime[i] = 0.0f;
        }
        m_currentTime[i] = m_accumulatedTime[i];
    }
}

// ---- Loop / Remap ---------------------------------------------------------------------------------------------------

#define LIST_INPUTS(X)                                                                                                  \
    X(PT::NumberOrVector, 0.0f, value, "Value", "The input value to apply looping to.")                                 \
    X(PT::NumberOrVector, 0.0f, minRange, "Min Range", "The minimum value of the looping range.")                       \
    X(PT::NumberOrVector, 1.0f, maxRange, "Max Range", "The maximum value of the looping range.")                       \
    X(PT::Enum, static_cast<std::uint32_t>(LoopingType::Loop), loopingType, "Looping Type",                              \
      "How the value should loop within the range.", property.enumValues = loopingTypeEnumValues())
#define LIST_STATES(X)
#define LIST_OUTPUTS(X)                                                                                                 \
    X(PT::NumberOrVector, 0.0f, loopedValue, "Looped Value", "The value with looping applied.")                         \
    X(PT::Bool, false, isReversing, "Is Reversing",                                                                     \
      "True if the value is in the reverse phase of ping pong looping. If passing `loopedValue` to a `Remap` "          \
      "component, hook this up to `shouldReverse` from that component.")
template <PT valueType>
class Loop : public RegisteredComponentBatch<Loop<valueType>> {
    static constexpr PT valuePropertyType = valueType;
    static constexpr PT minRangePropertyType = valueType;
    static constexpr PT maxRangePropertyType = valueType;
    static constexpr PT loopingTypePropertyType = PT::Enum;
    static constexpr PT loopedValuePropertyType = valueType;
    static constexpr PT isReversingPropertyType = PT::Bool;
    FUSE_LOGIC_COMPONENT_BODY(Loop, "Loop", "Transform",
                              "Wraps a number back into a range when it goes outside the boundaries.\n\nApplies looping behavior "
                              "to a value. Value is unchanged if it is inside the range.\nComponent outputs Min Range if Min Range "
                              "== Max Range and looping type is not None.\nInverted ranges (max < min) are supported, but the "
                              "results are undefined and may change without warning.",
                              1, LIST_INPUTS, LIST_STATES, LIST_OUTPUTS)
    void updateRange(const LogicContext& /*ctx*/, std::size_t start, std::size_t end) final {
        for (std::size_t i = start; i < end; i++) {
            const LoopingType type = static_cast<LoopingType>(m_loopingType[i]);
            if constexpr (valueType == PT::Float) {
                const auto r = applyLooping(m_value[i], m_minRange[i], m_maxRange[i], type);
                m_loopedValue[i] = r.first;
                m_isReversing[i] = r.second;
            } else {
                auto looped = m_value[i];
                bool anyReversing = false;
                constexpr std::size_t n = valueType == PT::Float2 ? 2 : (valueType == PT::Float3 ? 3 : 4);
                for (std::size_t c = 0; c < n; ++c) {
                    const auto r = applyLooping(m_value[i][c], m_minRange[i][c], m_maxRange[i][c], type);
                    looped[c] = r.first;
                    anyReversing = anyReversing || r.second;
                }
                m_loopedValue[i] = looped;
                m_isReversing[i] = anyReversing;
            }
        }
    }
};
#undef LIST_INPUTS
#undef LIST_STATES
#undef LIST_OUTPUTS

#define LIST_INPUTS(X)                                                                                                  \
    X(PT::Float, 0.0f, value, "Value", "The input value to interpolate.")                                               \
    X(PT::Float, 0.0f, inputMin, "Input Min", "If `Value` equals `Input Min`, the output will be `Output Min`.")        \
    X(PT::Float, 1.0f, inputMax, "Input Max", "If `Value` equals `Input Max`, the output will be `Output Max`.")        \
    X(PT::Bool, false, clampInput, "Clamp Input", "If true, `value` will be clamped to the input range.",               \
      property.optional = true)                                                                                         \
    X(PT::Enum, static_cast<std::uint32_t>(InterpolationType::Linear), easingType, "Easing Type",                        \
      "The type of easing to apply.", property.enumValues = interpolationTypeEnumValues())                              \
    X(PT::Bool, false, shouldReverse, "Should Reverse",                                                                 \
      "If true, the easing is applied backwards. If `Value` is coming from a Loop component that is using `pingpong`, " \
      "hook this up to `isReversing` from that component.", property.optional = true)                                   \
    X(PT::NumberOrVector, 0.0f, outputMin, "Output Min", "What a `Value` of `Input Min` maps to.")                      \
    X(PT::NumberOrVector, 1.0f, outputMax, "Output Max", "What a `Value` of `Input Max` maps to.")
#define LIST_STATES(X)
#define LIST_OUTPUTS(X)                                                                                                 \
    X(PT::NumberOrVector, 0.0f, output, "Output",                                                                       \
      "The final remapped value after applying input normalization, easing, and output mapping.")
template <PT rangeType>
class Remap : public RegisteredComponentBatch<Remap<rangeType>> {
    static constexpr PT valuePropertyType = PT::Float;
    static constexpr PT inputMinPropertyType = PT::Float;
    static constexpr PT inputMaxPropertyType = PT::Float;
    static constexpr PT clampInputPropertyType = PT::Bool;
    static constexpr PT easingTypePropertyType = PT::Enum;
    static constexpr PT shouldReversePropertyType = PT::Bool;
    static constexpr PT outputMinPropertyType = rangeType;
    static constexpr PT outputMaxPropertyType = rangeType;
    static constexpr PT outputPropertyType = rangeType;
    FUSE_LOGIC_COMPONENT_BODY(Remap, "Remap", "Transform",
                              "Smoothly maps a value from one range to another range with customizable easing curves.\n\nRemaps a "
                              "value from an input range to an output range with optional easing. Values will be normalized "
                              "(mapped from input range to 0-1), eased (changed from linear to some curve), then mapped (0-1 value "
                              "to output range).\n\nNote: Input values outside of input range are valid, and easing can lead to "
                              "the output value being outside of the output range even when input is inside the input "
                              "range.\n\nInverted ranges (max < min) are supported.",
                              1, LIST_INPUTS, LIST_STATES, LIST_OUTPUTS, spec.oldNames = {"InterpolateFloat"})
    void updateRange(const LogicContext& /*ctx*/, std::size_t start, std::size_t end) final {
        for (std::size_t i = start; i < end; i++) {
            float normalizedValue = m_value[i];
            if (m_inputMax[i] == m_inputMin[i]) {
                logOnce(LogSeverity::Error, "Remap: Input Min and Input Max are the same. Setting normalized value to 0.0f.");
                normalizedValue = 0.0f;
            } else {
                if (m_clampInput[i]) {
                    if (m_inputMin[i] > m_inputMax[i]) {
                        normalizedValue = clampf(normalizedValue, m_inputMax[i], m_inputMin[i]);
                    } else {
                        normalizedValue = clampf(normalizedValue, m_inputMin[i], m_inputMax[i]);
                    }
                }
                normalizedValue = (normalizedValue - m_inputMin[i]) / (m_inputMax[i] - m_inputMin[i]);
            }
            const bool shouldReverse = m_shouldReverse[i];
            if (shouldReverse) {
                normalizedValue = 1.0f - normalizedValue;
            }
            float easedValue = applyInterpolation(static_cast<InterpolationType>(m_easingType[i]), normalizedValue);
            if (shouldReverse) {
                easedValue = 1.0f - easedValue;
            }
            m_output[i] = lerp(m_outputMin[i], m_outputMax[i], easedValue);
        }
    }
};
#undef LIST_INPUTS
#undef LIST_STATES
#undef LIST_OUTPUTS

} // namespace

void registerStateComponents() {
    Counter::registerType();
    Toggle::registerType();
    CountToggles::registerType();
    registerAnyVariants<ConditionallyStore>();
    registerAnyVariants<PreviousFrameValue>();
    registerNumberOrVectorVariants<Smooth>();
    registerNumberOrVectorVariants<Velocity>();
    Time::registerType();
    registerNumberOrVectorVariants<Loop>();
    registerNumberOrVectorVariants<Remap>();
}

} // namespace components

} // namespace fuse::relight::logic
