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
// Ported from dxvk-remix src/dxvk/rtx_render/graph/components/{rtx_option_layer_action,rtx_option_layer_sensor,
// rtx_option_read_bool,rtx_option_read_number,rtx_option_read_vector2,rtx_option_read_vector3,rtx_option_read_color3,
// rtx_option_read_color4}.h@0867d3c
//
// FUSE Relight RL-3.5: the option-layer actions and option sensors, on the RL-0.6 option system (options/).
//
// RtxOptionLayerAction acquires a dynamic option layer (key: rounded, clamped priority + config path; the layer
// reads the .conf once when created) at instance creation, re-acquires it when the path or priority input
// changes, and every update requests enabled / blend strength / blend threshold for the frame; the layer resolves
// the requests (ANY enabled, MAX strength, MIN threshold) in OptionManager::applyPendingValues at the end of the
// frame, so the options change for the next frame, as upstream. Upstream counts references on the layer itself
// (RtxOptionManager::acquireLayer / releaseLayer); here every reference is an options::OptionLayerHandle kept in a
// process-wide table (the handle's destruction is the release), because graph property vectors hold plain values.
// The Read* components look options up by full name (OptionManager::findOption also accepts the relight.* / rtx.*
// twin and registered aliases, a superset of upstream's exact-name lookup).
#include <fuse/relight/logic/component_list.hpp>

#include "component_list_internal.hpp"
#include "component_macros.hpp"

#include <fuse/relight/options/option_manager.hpp>

#include <algorithm>
#include <map>
#include <mutex>
#include <string>

namespace fuse::relight::logic {

namespace {

std::mutex& heldLayerMutex() {
    static std::mutex m;
    return m;
}
std::map<options::OptionLayerKey, std::vector<options::OptionLayerHandle>>& heldLayers() {
    static std::map<options::OptionLayerKey, std::vector<options::OptionLayerHandle>> m;
    return m;
}

bool acquireGraphLayer(const std::string& configPath, std::uint32_t priority) {
    options::OptionLayerHandle handle = options::OptionManager::acquireLayer(configPath, options::OptionLayerKey(priority, configPath),
                                                                             1.0f, 0.1f);
    if (!handle) {
        return false;
    }
    std::lock_guard<std::mutex> lock(heldLayerMutex());
    heldLayers()[options::OptionLayerKey(priority, configPath)].push_back(std::move(handle));
    return true;
}

void releaseGraphLayer(const std::string& configPath, std::uint32_t priority) {
    options::OptionLayerHandle released;
    {
        std::lock_guard<std::mutex> lock(heldLayerMutex());
        auto it = heldLayers().find(options::OptionLayerKey(priority, configPath));
        if (it == heldLayers().end() || it->second.empty()) {
            return;
        }
        released = std::move(it->second.back());
        it->second.pop_back();
        if (it->second.empty()) {
            heldLayers().erase(it);
        }
    }
    // `released` drops its reference here, outside the table lock (the option system takes its own lock).
}

} // namespace

std::vector<HeldOptionLayer> heldOptionLayers() {
    std::vector<HeldOptionLayer> out;
    std::lock_guard<std::mutex> lock(heldLayerMutex());
    for (const auto& [key, handles] : heldLayers()) {
        HeldOptionLayer h;
        h.configPath = key.name;
        h.priority = key.priority;
        h.references = handles.size();
        if (!handles.empty() && handles.front()) {
            const options::OptionLayer& layer = *handles.front();
            h.enabled = layer.isEnabled();
            h.blendStrength = layer.getBlendStrength();
            h.blendThreshold = layer.getBlendStrengthThreshold();
        }
        out.push_back(std::move(h));
    }
    return out;
}

void releaseAllHeldOptionLayers() {
    std::map<options::OptionLayerKey, std::vector<options::OptionLayerHandle>> released;
    {
        std::lock_guard<std::mutex> lock(heldLayerMutex());
        released.swap(heldLayers());
    }
}

namespace components {

namespace {

using PT = PropertyType;

// ---- RtxOptionLayerAction -----------------------------------------------------------------------------------------

#define LIST_INPUTS(X)                                                                                                  \
    X(PT::AssetPath, "", configPath, "Config Path", "The config file for the RtxOptionLayer to control.")               \
    X(PT::Bool, true, enabled, "Enabled",                                                                               \
      "If true, the option layer is enabled and its settings are applied. If false, the layer is disabled. If "        \
      "multiple components control the same layer, it will be enabled if ANY of them request it.",                     \
      property.optional = true)                                                                                         \
    X(PT::Float, 1.0f, blendStrength, "Blend Strength",                                                                 \
      "The blend strength for the option layer (0.0 = no effect, 1.0 = full effect.)\n\nLowest priority layer uses "   \
      "LERP to blend with default value, then each higher priority layer uses LERP to blend with the previous "        \
      "layer's result.\n\nIf multiple components control the same layer, the MAX blend strength will be used.",        \
      property.hardMin = 0.0f, property.hardMax = 1.0f, property.optional = true)                                       \
    X(PT::Float, 0.1f, blendThreshold, "Blend Threshold",                                                               \
      "The blend threshold for non-float options (0.0 to 1.0). Non-float options are only applied when blend strength " \
      "exceeds this threshold. If multiple components control the same layer, the MINIMUM blend threshold will be "     \
      "used.", property.hardMin = 0.0f, property.hardMax = 1.0f, property.optional = true)                              \
    X(PT::Float, options::kDefaultDynamicLayerPriority, priority, "Priority",                                           \
      "The priority for the option layer. Numbers are rounded to the nearest positive integer. Higher values are "      \
      "blended on top of lower values. If two components specify the same priority but different config paths, the "   \
      "layers will be prioritized alphabetically (a.conf will override values from z.conf).",                          \
      property.hardMin = options::kMinDynamicLayerPriority, property.hardMax = options::kMaxDynamicLayerPriority,       \
      property.softMin = options::kDefaultDynamicLayerPriority / 10, property.softMax = options::kDefaultDynamicLayerPriority * 2, \
      property.uiStep = options::kDefaultDynamicLayerPriority / 100, property.optional = true)
#define LIST_STATES(X)                                                                                                  \
    X(PT::Bool, false, holdsReference, "", "True if the component is holding a reference to the RtxOptionLayer.")       \
    X(PT::AssetPath, "", cachedConfigPath, "", "Cached config path from when the layer was acquired.")                 \
    X(PT::Float, 0.0f, cachedPriority, "", "Cached priority from when the layer was acquired.")
#define LIST_OUTPUTS(X)
class RtxOptionLayerAction : public RegisteredComponentBatch<RtxOptionLayerAction> {
    FUSE_LOGIC_GENERATE_PROP_TYPES(LIST_INPUTS, LIST_STATES, LIST_OUTPUTS)
    FUSE_LOGIC_COMPONENT_BODY(RtxOptionLayerAction, "Rtx Option Layer Action", "Act",
                              "Activates and controls configuration layers at runtime based on game conditions.\n\nControls an "
                              "RtxOptionLayer by name, allowing dynamic enable/disable, strength adjustment, and threshold "
                              "control. This can be used to activate configuration layers at runtime based on game state or other "
                              "conditions.\n\nThe layer is created if it doesn't exist, and managed with reference counting.\nIf "
                              "two components specify the same priority and config path, they will both control the same layer "
                              "(for enabled components, uses the MAX of the blend strengths and the MIN of the blend "
                              "thresholds).\nIf two components specify the same priority but different config paths, the layers "
                              "will be prioritized alphabetically (a.conf will override values from z.conf).",
                              1, LIST_INPUTS, LIST_STATES, LIST_OUTPUTS, spec.initialize = initialize; spec.cleanup = cleanup)
    void updateRange(const LogicContext& /*ctx*/, std::size_t start, std::size_t end) final {
        for (std::size_t i = start; i < end; i++) {
            if (m_holdsReference[i]) {
                const bool configPathChanged = m_configPath[i] != m_cachedConfigPath[i];
                const bool priorityChanged = m_priority[i] != m_cachedPriority[i];
                if (configPathChanged || priorityChanged) {
                    cleanupInstance(i);
                    initializeInstance(i);
                }
            }
            if (!m_holdsReference[i]) {
                continue;
            }
            options::OptionLayer* layer = options::OptionManager::getLayer(
                options::OptionLayerKey(options::clampComponentLayerPriority(m_cachedPriority[i]), m_cachedConfigPath[i]));
            if (layer == nullptr) {
                continue;
            }
            layer->requestEnabled(m_enabled[i] != 0);
            if (m_enabled[i]) {
                layer->requestBlendStrength(std::clamp(m_blendStrength[i], 0.0f, 1.0f));
                layer->requestBlendThreshold(std::clamp(m_blendThreshold[i], 0.0f, 1.0f));
            }
        }
    }
    static void initialize(const LogicContext& /*ctx*/, ComponentBatch& batch, std::size_t index) {
        static_cast<RtxOptionLayerAction&>(batch).initializeInstance(index);
    }
    static void cleanup(ComponentBatch& batch, std::size_t index) { static_cast<RtxOptionLayerAction&>(batch).cleanupInstance(index); }
    void initializeInstance(std::size_t index) {
        if (m_configPath[index].empty()) {
            m_holdsReference[index] = false;
            return;
        }
        const std::uint32_t priority = options::clampComponentLayerPriority(m_priority[index]);
        if (acquireGraphLayer(m_configPath[index], priority)) {
            m_holdsReference[index] = true;
            m_cachedConfigPath[index] = m_configPath[index];
            m_cachedPriority[index] = m_priority[index];
        } else {
            logMessage(LogSeverity::Error, "RtxOptionLayerAction: Failed to acquire layer with key: '" + m_configPath[index] +
                                               "' (priority " + std::to_string(priority) + ").");
            m_holdsReference[index] = false;
        }
    }
    void cleanupInstance(std::size_t index) {
        if (!m_holdsReference[index]) {
            return;
        }
        releaseGraphLayer(m_cachedConfigPath[index], options::clampComponentLayerPriority(m_cachedPriority[index]));
        m_holdsReference[index] = false;
    }
};
#undef LIST_INPUTS
#undef LIST_STATES
#undef LIST_OUTPUTS

// ---- RtxOptionLayerSensor -----------------------------------------------------------------------------------------

#define LIST_INPUTS(X)                                                                                                  \
    X(PT::AssetPath, "", configPath, "Config Path", "The config file for the RtxOptionLayer to read.")                  \
    X(PT::Float, options::kDefaultDynamicLayerPriority, priority, "Priority",                                           \
      "The priority for the option layer. Numbers are rounded to the nearest positive integer. Higher values are "      \
      "blended on top of lower values. If multiple layers share the same priority, they are ordered alphabetically by " \
      "config path.",                                                                                                   \
      property.hardMin = options::kMinDynamicLayerPriority, property.hardMax = options::kMaxDynamicLayerPriority,       \
      property.softMin = options::kDefaultDynamicLayerPriority / 10, property.softMax = options::kDefaultDynamicLayerPriority * 2, \
      property.uiStep = options::kDefaultDynamicLayerPriority / 100, property.optional = true)
#define LIST_STATES(X)
#define LIST_OUTPUTS(X)                                                                                                 \
    X(PT::Bool, false, isEnabled, "Is Enabled", "True if the option layer is currently enabled.")                       \
    X(PT::Float, 0.0f, blendStrength, "Blend Strength",                                                                 \
      "The current blend strength of the option layer (0.0 = no effect, 1.0 = full effect).")                           \
    X(PT::Float, 0.0f, blendThreshold, "Blend Threshold", "The current blend threshold for non-float options (0.0 to 1.0).")
FUSE_LOGIC_COMPONENT(RtxOptionLayerSensor, "Rtx Option Layer Sensor", "Sense",
                     "Reads the state of a configuration layer.\n\nOutputs whether a given RtxOptionLayer is enabled, along with "
                     "its blend strength and threshold values. This can be used to create logic that responds to the state of "
                     "configuration layers.",
                     1, LIST_INPUTS, LIST_STATES, LIST_OUTPUTS)
#undef LIST_INPUTS
#undef LIST_OUTPUTS
void RtxOptionLayerSensor::updateRange(const LogicContext& /*ctx*/, std::size_t start, std::size_t end) {
    for (std::size_t i = start; i < end; i++) {
        bool isEnabled = false;
        float blendStrength = 0.0f;
        float blendThreshold = 0.0f;
        if (!m_configPath[i].empty()) {
            const std::uint32_t priority = options::clampComponentLayerPriority(m_priority[i]);
            if (const options::OptionLayer* layer = options::OptionManager::getLayer(options::OptionLayerKey(priority, m_configPath[i]))) {
                isEnabled = layer->isEnabled();
                blendStrength = layer->getPendingBlendStrength();
                blendThreshold = layer->getPendingBlendThreshold();
            }
        }
        m_isEnabled[i] = isEnabled;
        m_blendStrength[i] = blendStrength;
        m_blendThreshold[i] = blendThreshold;
    }
}

// ---- RtxOptionRead* -----------------------------------------------------------------------------------------------

/// The option named `name` when it has one of `types` (warns once otherwise; `what` names the component).
const options::OptionBase* findTypedOption(const std::string& name, std::initializer_list<options::OptionType> types, const char* what,
                                           const char* typeText) {
    if (name.empty()) {
        return nullptr;
    }
    const options::OptionBase* option = options::OptionManager::findOption(name);
    if (option == nullptr) {
        logOnce(LogSeverity::Warning, std::string(what) + ": Option '" + name + "' not found.");
        return nullptr;
    }
    for (options::OptionType t : types) {
        if (option->getType() == t) {
            return option;
        }
    }
    logOnce(LogSeverity::Warning, std::string(what) + ": Option '" + name + "' is not a " + typeText + " type.");
    return nullptr;
}

#define LIST_INPUTS(X)                                                                                                  \
    X(PT::String, "", optionName, "Option Name", "The full name of the RTX option to read (e.g., 'rtx.someOption').")
#define LIST_OUTPUTS(X)                                                                                                 \
    X(PT::Bool, false, value, "Value",                                                                                  \
      "The current value of the RTX option as a bool. Returns false if the option is not found or is not a bool type.")
FUSE_LOGIC_COMPONENT(RtxOptionReadBool, "Rtx Option Read Bool", "Sense",
                     "Reads the current value of a boolean RTX option.\n\nOutputs the current value of a given RTX option bool. "
                     "The option name should be the full name including category (e.g., 'rtx.enableRaytracing').",
                     1, LIST_INPUTS, LIST_STATES, LIST_OUTPUTS)
#undef LIST_OUTPUTS
void RtxOptionReadBool::updateRange(const LogicContext& /*ctx*/, std::size_t start, std::size_t end) {
    for (std::size_t i = start; i < end; i++) {
        bool value = false;
        if (const auto* o = findTypedOption(m_optionName[i], {options::OptionType::Bool}, "RtxOptionReadBool", "bool")) {
            value = std::get<bool>(o->getResolvedValue());
        }
        m_value[i] = value;
    }
}

#define LIST_OUTPUTS(X)                                                                                                 \
    X(PT::Float, 0.0f, value, "Value",                                                                                  \
      "The current value of the RTX option as a float. Returns 0 if the option is not found or is not a numeric type.")
FUSE_LOGIC_COMPONENT(RtxOptionReadNumber, "Rtx Option Read Number", "Sense",
                     "Reads the current value of a numeric RTX option.\n\nOutputs the current value of a given RTX option. "
                     "Supports both float and int types. The option name should be the full name including category (e.g., "
                     "'rtx.pathTracing.enableReSTIRGI').",
                     1, LIST_INPUTS, LIST_STATES, LIST_OUTPUTS)
#undef LIST_OUTPUTS
void RtxOptionReadNumber::updateRange(const LogicContext& /*ctx*/, std::size_t start, std::size_t end) {
    for (std::size_t i = start; i < end; i++) {
        float value = 0.0f;
        if (const auto* o = findTypedOption(m_optionName[i], {options::OptionType::Float, options::OptionType::Int},
                                            "RtxOptionReadNumber", "numeric (float or int)")) {
            const options::OptionValue v = o->getResolvedValue();
            value = o->getType() == options::OptionType::Float ? std::get<float>(v) : static_cast<float>(std::get<std::int32_t>(v));
        }
        m_value[i] = value;
    }
}

#define LIST_OUTPUTS(X)                                                                                                 \
    X(PT::Float2, Vector2(0.0f, 0.0f), value, "Value",                                                                  \
      "The current value of the RTX option as a Vector2. Returns (0,0) if the option is not found or is not a Vector2 type.")
FUSE_LOGIC_COMPONENT(RtxOptionReadVector2, "Rtx Option Read Vector2", "Sense",
                     "Reads the current value of a Vector2 RTX option.\n\nOutputs the current value of a given RTX option "
                     "Vector2. The option name should be the full name including category.",
                     1, LIST_INPUTS, LIST_STATES, LIST_OUTPUTS)
#undef LIST_OUTPUTS
void RtxOptionReadVector2::updateRange(const LogicContext& /*ctx*/, std::size_t start, std::size_t end) {
    for (std::size_t i = start; i < end; i++) {
        Vector2 value(0.0f, 0.0f);
        if (const auto* o = findTypedOption(m_optionName[i], {options::OptionType::Vector2}, "RtxOptionReadVector2", "Vector2")) {
            const auto v = std::get<options::Vec2f>(o->getResolvedValue());
            value = Vector2(v[0], v[1]);
        }
        m_value[i] = value;
    }
}

#define LIST_OUTPUTS(X)                                                                                                 \
    X(PT::Float3, Vector3(0.0f, 0.0f, 0.0f), value, "Value",                                                            \
      "The current value of the RTX option as a Vector3. Returns (0,0,0) if the option is not found or is not a Vector3 type.")
FUSE_LOGIC_COMPONENT(RtxOptionReadVector3, "Rtx Option Read Vector3", "Sense",
                     "Reads the current value of a Vector3 RTX option.\n\nOutputs the current value of a given RTX option "
                     "Vector3. The option name should be the full name including category.",
                     1, LIST_INPUTS, LIST_STATES, LIST_OUTPUTS)
#undef LIST_OUTPUTS
void RtxOptionReadVector3::updateRange(const LogicContext& /*ctx*/, std::size_t start, std::size_t end) {
    for (std::size_t i = start; i < end; i++) {
        Vector3 value(0.0f, 0.0f, 0.0f);
        if (const auto* o = findTypedOption(m_optionName[i], {options::OptionType::Vector3}, "RtxOptionReadVector3", "Vector3")) {
            const auto v = std::get<options::Vec3f>(o->getResolvedValue());
            value = Vector3(v[0], v[1], v[2]);
        }
        m_value[i] = value;
    }
}

#define LIST_OUTPUTS(X)                                                                                                 \
    X(PT::Float3, Vector3(0.0f, 0.0f, 0.0f), value, "Value",                                                            \
      "The current value of the RTX option as a Color3 (RGB). Returns black (0,0,0) if the option is not found or is "  \
      "not a Vector3 type.", property.treatAsColor = true)
FUSE_LOGIC_COMPONENT(RtxOptionReadColor3, "Rtx Option Read Color3", "Sense",
                     "Reads the current value of a Color3 (RGB) RTX option.\n\nOutputs the current value of a given RTX option "
                     "as a Color3. Internally, Color3 is stored as Vector3. The option name should be the full name including "
                     "category (e.g., 'rtx.fallbackLightRadiance').",
                     1, LIST_INPUTS, LIST_STATES, LIST_OUTPUTS)
#undef LIST_OUTPUTS
void RtxOptionReadColor3::updateRange(const LogicContext& /*ctx*/, std::size_t start, std::size_t end) {
    for (std::size_t i = start; i < end; i++) {
        Vector3 value(0.0f, 0.0f, 0.0f);
        if (const auto* o = findTypedOption(m_optionName[i], {options::OptionType::Vector3}, "RtxOptionReadColor3", "Vector3/Color3")) {
            const auto v = std::get<options::Vec3f>(o->getResolvedValue());
            value = Vector3(v[0], v[1], v[2]);
        }
        m_value[i] = value;
    }
}

#define LIST_OUTPUTS(X)                                                                                                 \
    X(PT::Float4, Vector4(0.0f, 0.0f, 0.0f, 1.0f), value, "Value",                                                      \
      "The current value of the RTX option as a Color4 (RGBA). Returns black with full alpha (0,0,0,1) if the option "  \
      "is not found or is not a Vector4 type.", property.treatAsColor = true)
FUSE_LOGIC_COMPONENT(RtxOptionReadColor4, "Rtx Option Read Color4", "Sense",
                     "Reads the current value of a Color4 (RGBA) RTX option.\n\nOutputs the current value of a given RTX option "
                     "as a Color4. Internally, Color4 is stored as Vector4. The option name should be the full name including "
                     "category.",
                     1, LIST_INPUTS, LIST_STATES, LIST_OUTPUTS)
#undef LIST_OUTPUTS
#undef LIST_INPUTS
#undef LIST_STATES
void RtxOptionReadColor4::updateRange(const LogicContext& /*ctx*/, std::size_t start, std::size_t end) {
    for (std::size_t i = start; i < end; i++) {
        Vector4 value(0.0f, 0.0f, 0.0f, 1.0f);
        if (const auto* o = findTypedOption(m_optionName[i], {options::OptionType::Vector4}, "RtxOptionReadColor4", "Vector4/Color4")) {
            const auto v = std::get<options::Vec4f>(o->getResolvedValue());
            value = Vector4(v[0], v[1], v[2], v[3]);
        }
        m_value[i] = value;
    }
}

} // namespace

void registerOptionComponents() {
    RtxOptionLayerAction::registerType();
    RtxOptionLayerSensor::registerType();
    RtxOptionReadBool::registerType();
    RtxOptionReadNumber::registerType();
    RtxOptionReadVector2::registerType();
    RtxOptionReadVector3::registerType();
    RtxOptionReadColor3::registerType();
    RtxOptionReadColor4::registerType();
}

} // namespace components

} // namespace fuse::relight::logic
