/*
* Copyright (c) 2022-2026, NVIDIA CORPORATION. All rights reserved.
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
// Ported from dxvk-remix src/dxvk/rtx_render/rtx_option.cpp@0867d3c

#include <fuse/relight/options/option.hpp>
#include <fuse/relight/options/option_layer.hpp>
#include <fuse/relight/options/option_manager.hpp>

#include "options_log.hpp"
#include "options_state.hpp"

#include <algorithm>

namespace fuse::relight::options {

namespace {

std::string joinErrors(const std::vector<std::string>& errors) {
    std::string out;
    for (const std::string& error : errors) {
        if (!out.empty()) {
            out += "; ";
        }
        out += error;
    }
    return out;
}

template <typename V>
V componentMax(V a, const V& b, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        a[i] = std::max(a[i], b[i]);
    }
    return a;
}

template <typename V>
V componentMin(V a, const V& b, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        a[i] = std::min(a[i], b[i]);
    }
    return a;
}

template <typename V>
bool clampVector(V& value, const std::optional<OptionValue>& minValue, const std::optional<OptionValue>& maxValue,
                 std::size_t n) {
    const V old = value;
    if (minValue) {
        value = componentMax(value, std::get<V>(*minValue), n);
    }
    if (maxValue) {
        value = componentMin(value, std::get<V>(*maxValue), n);
    }
    return value != old;
}

template <typename V>
void addScaled(V& target, const V& source, float weight, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        target[i] += source[i] * weight;
    }
}

} // namespace

// ============================================================================
// Construction and registration
// ============================================================================

OptionBase::OptionBase(const char* category, const char* name, OptionType type, const char* description,
                       const char* environment, std::uint32_t flags, OptionChangeCallback onChange)
    : m_category(category ? category : ""),
      m_name(name ? name : ""),
      m_fullName(m_category + "." + m_name),
      m_description(description ? description : ""),
      m_environment(environment),
      m_type(type),
      m_flags(flags),
      m_onChange(onChange),
      m_resolved(makeDefaultValue(type)) {}

OptionBase::~OptionBase() {
    std::lock_guard<std::recursive_mutex> lock(detail::optionMutex());
    if (m_registered) {
        auto& registry = OptionManager::optionRegistry();
        auto it = registry.find(m_fullName);
        if (it != registry.end() && it->second == this) {
            registry.erase(it);
        }
        auto& dirty = OptionManager::dirtyOptions();
        auto dirtyIt = dirty.find(m_fullName);
        if (dirtyIt != dirty.end() && dirtyIt->second == this) {
            dirty.erase(dirtyIt);
        }
    }
}

bool OptionBase::registerAndInitialize(OptionValue defaultValue) {
    std::lock_guard<std::recursive_mutex> lock(detail::optionMutex());
    auto& registry = OptionManager::optionRegistry();
    if (!registry.emplace(m_fullName, this).second) {
        detail::logError("Option with the same name already exists: %s", m_fullName.c_str());
        assert(false && "Relight option registered twice");
        return false;
    }
    m_registered = true;
    m_resolved = std::move(defaultValue);
    onResolvedValueChanged();
    if (const OptionLayer* defaultLayer = OptionLayer::getDefaultLayer()) {
        insertLayerValue(m_resolved, defaultLayer);
    }
    return true;
}

// ============================================================================
// Layer value storage
// ============================================================================

const OptionValue* OptionBase::findLayerValue(const OptionLayer* layer) const {
    if (!layer) {
        return nullptr;
    }
    auto it = m_layerValues.find(layer->getLayerKey());
    return it != m_layerValues.end() ? &it->second.value : nullptr;
}

OptionValue* OptionBase::getOrCreateLayerValue(const OptionLayer* layer, bool* created) {
    if (created) {
        *created = false;
    }
    if (!layer) {
        detail::logWarn("getOrCreateLayerValue called with a null layer (%s)", m_fullName.c_str());
        return nullptr;
    }
    auto it = m_layerValues.find(layer->getLayerKey());
    if (it != m_layerValues.end()) {
        layer->setHasValues(true);
        return &it->second.value;
    }
    auto inserted = m_layerValues.emplace(
        layer->getLayerKey(),
        LayerValue{makeDefaultValue(m_type), layer->getBlendStrength(), layer->getBlendStrengthThreshold()});
    layer->setHasValues(true);
    if (created) {
        *created = true;
    }
    return &inserted.first->second.value;
}

void OptionBase::insertLayerValue(const OptionValue& value, const OptionLayer* layer) {
    if (!layer) {
        detail::logWarn("Cannot insert a layer value with a null layer (%s)", m_fullName.c_str());
        return;
    }
    auto it = m_layerValues.find(layer->getLayerKey());
    if (it != m_layerValues.end()) {
        it->second.value = value;
        it->second.blendStrength = layer->getBlendStrength();
        it->second.blendThreshold = layer->getBlendStrengthThreshold();
    } else {
        m_layerValues.emplace(layer->getLayerKey(),
                              LayerValue{value, layer->getBlendStrength(), layer->getBlendStrengthThreshold()});
    }
    layer->setHasValues(true);
}

std::optional<OptionValue> OptionBase::getLayerValue(const OptionLayer* layer) const {
    std::lock_guard<std::recursive_mutex> lock(detail::optionMutex());
    const OptionValue* value = findLayerValue(layer);
    return value ? std::optional<OptionValue>(*value) : std::nullopt;
}

OptionBase::LayerValueMap OptionBase::getLayerValues() const {
    std::lock_guard<std::recursive_mutex> lock(detail::optionMutex());
    return m_layerValues;
}

std::string OptionBase::getResolvedValueAsString() const {
    std::lock_guard<std::recursive_mutex> lock(detail::optionMutex());
    return formatOptionValue(m_resolved);
}

OptionValue OptionBase::getResolvedValue() const {
    std::lock_guard<std::recursive_mutex> lock(detail::optionMutex());
    tagInvalidationScope();
    return m_resolved;
}

std::optional<OptionValue> OptionBase::getMinValueGeneric() const {
    std::lock_guard<std::recursive_mutex> lock(detail::optionMutex());
    return m_minValue;
}

std::optional<OptionValue> OptionBase::getMaxValueGeneric() const {
    std::lock_guard<std::recursive_mutex> lock(detail::optionMutex());
    return m_maxValue;
}

// ============================================================================
// Config keys and aliases
// ============================================================================

std::vector<std::string> OptionBase::getConfigKeys() const {
    std::vector<std::string> keys{m_fullName};
    std::string twin = OptionManager::twinName(m_fullName);
    if (!twin.empty()) {
        keys.push_back(std::move(twin));
    }
    std::lock_guard<std::recursive_mutex> lock(detail::optionMutex());
    for (const auto& [alias, target] : detail::aliasTable()) {
        if (target == m_fullName && std::find(keys.begin(), keys.end(), alias) == keys.end()) {
            keys.push_back(alias);
        }
    }
    return keys;
}

const std::string* OptionBase::findConfigValue(const OptionConfig& config, std::string* keyUsed) const {
    const std::string* found = nullptr;
    std::string foundKey;
    for (const std::string& key : getConfigKeys()) {
        const std::string* value = config.find(key);
        if (!value) {
            continue;
        }
        if (!found) {
            found = value;
            foundKey = key;
        } else {
            detail::logWarn("'%s' and '%s' both set %s; using '%s'", foundKey.c_str(), key.c_str(),
                            m_fullName.c_str(), foundKey.c_str());
        }
    }
    if (found && keyUsed) {
        *keyUsed = foundKey;
    }
    return found;
}

// ============================================================================
// Reading values into layers
// ============================================================================

void OptionBase::readOptionLayer(const OptionLayer& layer) {
    std::lock_guard<std::recursive_mutex> lock(detail::optionMutex());
    std::string key;
    const std::string* raw = findConfigValue(layer.getConfig(), &key);
    if (!raw) {
        return;
    }
    OptionValue value = makeDefaultValue(m_type);
    std::vector<std::string> errors;
    if (!parseValueInto(*raw, value, &errors)) {
        detail::logWarn("Layer '%s': %s = '%s': %s; using '%s'", layer.getName().c_str(), key.c_str(), raw->c_str(),
                        joinErrors(errors).c_str(), formatOptionValue(value).c_str());
    }
    insertLayerValue(value, &layer);
    markDirty();
}

void OptionBase::readOption(const OptionConfig& config, const OptionLayer* layer) {
    std::lock_guard<std::recursive_mutex> lock(detail::optionMutex());
    if (!layer) {
        return;
    }
    std::string key;
    const std::string* raw = findConfigValue(config, &key);
    if (!raw) {
        return;
    }
    OptionValue* value = getOrCreateLayerValue(layer);
    if (!value) {
        return;
    }
    std::vector<std::string> errors;
    if (!parseValueInto(*raw, *value, &errors)) {
        detail::logWarn("%s = '%s': %s", key.c_str(), raw->c_str(), joinErrors(errors).c_str());
    }
    markDirty();
}

bool OptionBase::loadFromEnvironmentVariable(const OptionLayer* envLayer, std::string* outValue) {
    if (m_environment == nullptr || m_environment[0] == '\0') {
        return false;
    }
    const std::string envValue = ::fuse::relight::options::getEnvironmentVariable(m_environment);
    if (envValue.empty()) {
        return false;
    }
    if (outValue) {
        *outValue = envValue;
    }
    std::lock_guard<std::recursive_mutex> lock(detail::optionMutex());
    OptionValue* value = getOrCreateLayerValue(envLayer);
    if (!value) {
        detail::logWarn("Failed to create the environment layer value for %s", m_fullName.c_str());
        return false;
    }
    std::vector<std::string> errors;
    const bool parsed = parseValueInto(envValue, *value, &errors);
    if (!parsed) {
        detail::logWarn("Environment variable %s failed to parse for %s: %s", m_environment, m_fullName.c_str(),
                        joinErrors(errors).c_str());
        // Hash lists keep their valid entries (upstream never fails them); other types report failure.
        if (m_type != OptionType::HashSet && m_type != OptionType::HashVector) {
            return false;
        }
    }
    markDirty();
    return true;
}

// ============================================================================
// Layer value edits
// ============================================================================

void OptionBase::disableLayerValue(const OptionLayer* layer) {
    std::lock_guard<std::recursive_mutex> lock(detail::optionMutex());
    if (!layer) {
        return;
    }
    auto it = m_layerValues.find(layer->getLayerKey());
    if (it != m_layerValues.end()) {
        markDirty();
        m_layerValues.erase(it);
        layer->onLayerValueChanged(); // upstream leaves the layer's unsaved-changes cache stale here
    }
}

void OptionBase::updateLayerBlendStrength(const OptionLayer& layer) {
    std::lock_guard<std::recursive_mutex> lock(detail::optionMutex());
    auto it = m_layerValues.find(layer.getLayerKey());
    if (it == m_layerValues.end()) {
        return;
    }
    const float strength = layer.getBlendStrength();
    const float threshold = layer.getBlendStrengthThreshold();
    if (it->second.blendStrength != strength || it->second.blendThreshold != threshold) {
        it->second.blendStrength = strength;
        it->second.blendThreshold = threshold;
        markDirty();
    }
}

void OptionBase::moveLayerValue(const OptionLayer* sourceLayer, const OptionLayer* destinationLayer) {
    std::lock_guard<std::recursive_mutex> lock(detail::optionMutex());
    if (!sourceLayer || !destinationLayer) {
        return;
    }
    if (destinationLayer->getLayerKey() == OptionLayerKey(kDefaultLayerId)) {
        return; // the default layer is never edited
    }
    auto sourceIt = m_layerValues.find(sourceLayer->getLayerKey());
    if (sourceIt == m_layerValues.end()) {
        return;
    }
    OptionValue* destination = getOrCreateLayerValue(destinationLayer);
    if (!destination) {
        return;
    }
    // Inserting into the std::map keeps sourceIt valid.
    if (m_type == OptionType::HashSet) {
        HashSetLayer& source = std::get<HashSetLayer>(sourceIt->second.value);
        if (!source.empty()) {
            std::get<HashSetLayer>(*destination).mergeFrom(source);
            source.clearAll();
        }
    } else {
        *destination = sourceIt->second.value;
    }
    destinationLayer->onLayerValueChanged();
    disableLayerValue(sourceLayer);
    sourceLayer->onLayerValueChanged();
    markDirty();
}

void OptionBase::clearFromStrongerLayers(const OptionLayer* targetLayer, std::optional<Hash64> hash) {
    std::lock_guard<std::recursive_mutex> lock(detail::optionMutex());
    const OptionLayerKey targetKey = targetLayer ? targetLayer->getLayerKey() : OptionLayerKey(kDefaultLayerId);
    bool anyModified = false;
    auto it = m_layerValues.begin();
    while (it != m_layerValues.end() && it->first < targetKey) {
        OptionLayer* layer = OptionManager::getLayer(it->first);
        bool modified = false;
        bool erase = false;
        if (m_type == OptionType::HashSet && hash.has_value()) {
            HashSetLayer& set = std::get<HashSetLayer>(it->second.value);
            if (set.hasPositive(*hash) || set.hasNegative(*hash)) {
                set.clear(*hash);
                modified = true;
                erase = set.empty();
            }
        } else {
            modified = true;
            erase = true;
        }
        if (modified) {
            anyModified = true;
            if (layer) {
                layer->onLayerValueChanged();
            }
        }
        if (erase) {
            it = m_layerValues.erase(it);
        } else {
            ++it;
        }
    }
    if (anyModified) {
        markDirty();
    }
}

void OptionBase::forEachLayerValue(const std::function<bool(const OptionLayer*, const OptionValue&)>& callback,
                                   std::optional<Hash64> hash, bool includeInactiveLayers) const {
    std::lock_guard<std::recursive_mutex> lock(detail::optionMutex());
    const bool blendable = isBlendableType(m_type);
    for (const auto& [key, layerValue] : m_layerValues) {
        const bool active = blendable || layerValue.blendStrength >= layerValue.blendThreshold;
        if (!active && !includeInactiveLayers) {
            continue;
        }
        if (m_type == OptionType::HashSet && hash.has_value()) {
            const HashSetLayer& set = std::get<HashSetLayer>(layerValue.value);
            if (set.empty() || (!set.hasPositive(*hash) && !set.hasNegative(*hash))) {
                continue;
            }
        }
        const OptionLayer* layer = OptionManager::getLayer(key);
        if (layer && !callback(layer, layerValue.value)) {
            break;
        }
    }
}

const OptionLayer* OptionBase::getBlockingLayer(const OptionLayer* targetLayer, std::optional<Hash64> hash) const {
    std::lock_guard<std::recursive_mutex> lock(detail::optionMutex());
    const OptionLayer* target = getTargetLayer(targetLayer);
    const OptionLayerKey targetKey = target ? target->getLayerKey() : OptionLayerKey(kDefaultLayerId);
    const OptionLayer* blocking = nullptr;
    forEachLayerValue(
        [&](const OptionLayer* layer, const OptionValue&) {
            if (layer->getLayerKey() < targetKey) {
                blocking = layer;
                return false;
            }
            return true;
        },
        hash);
    return blocking;
}

bool OptionBase::migrateValuesTo(
    OptionBase* destination,
    const std::function<bool(const OptionValue& source, OptionValue& destination, bool destinationHadValue)>& transform) {
    std::lock_guard<std::recursive_mutex> lock(detail::optionMutex());
    if (!destination) {
        return false;
    }
    const OptionLayerKey defaultKey(kDefaultLayerId);
    bool migrated = false;
    for (const auto& [key, layerValue] : m_layerValues) {
        if (key == defaultKey) {
            continue;
        }
        const OptionLayer* layer = OptionManager::getLayer(key);
        if (!layer) {
            continue;
        }
        bool created = false;
        OptionValue* destinationValue = destination->getOrCreateLayerValue(layer, &created);
        if (!destinationValue) {
            detail::logError("[Migration] failed migrating %s to %s", m_fullName.c_str(),
                             destination->m_fullName.c_str());
            return false;
        }
        if (transform(layerValue.value, *destinationValue, !created)) {
            destination->markDirty();
            migrated = true;
        } else if (created) {
            // Upstream leaves a zero value behind here; a declined migration should not add one.
            destination->disableLayerValue(layer);
        }
    }
    if (migrated) {
        detail::logInfo("[Migration] Migrating from %s to %s", m_fullName.c_str(), destination->m_fullName.c_str());
    }
    return migrated;
}

// ============================================================================
// Routing, state queries, dirty tracking
// ============================================================================

const OptionLayer* OptionBase::getTargetLayer(const OptionLayer* explicitLayer) const {
    if ((getFlags() & OptionFlags::NoSave) != 0) {
        return OptionLayer::getDerivedLayer();
    }
    if (explicitLayer) {
        return explicitLayer;
    }
    const bool userSetting = (getFlags() & OptionFlags::UserSetting) != 0;
    if (OptionLayerTarget::current() == OptionEditTarget::User) {
        return userSetting ? OptionLayer::getUserLayer() : OptionLayer::getRtxConfLayer();
    }
    if (userSetting) {
        return OptionManager::isGraphicsPresetCustom() ? OptionLayer::getUserLayer() : OptionLayer::getQualityLayer();
    }
    return OptionLayer::getDerivedLayer();
}

bool OptionBase::isDefault() const {
    std::lock_guard<std::recursive_mutex> lock(detail::optionMutex());
    const OptionValue* defaultValue = findLayerValue(OptionLayer::getDefaultLayer());
    return defaultValue && *defaultValue == m_resolved;
}

bool OptionBase::hasValueInLayer(const OptionLayer* layer, std::optional<Hash64> hash) const {
    std::lock_guard<std::recursive_mutex> lock(detail::optionMutex());
    const OptionValue* value = findLayerValue(layer);
    if (!value) {
        return false;
    }
    if (m_type == OptionType::HashSet) {
        const HashSetLayer& set = std::get<HashSetLayer>(*value);
        if (hash.has_value()) {
            return set.hasPositive(*hash) || set.hasNegative(*hash);
        }
        return !set.empty();
    }
    return true;
}

bool OptionBase::isLayerValueRedundant(const OptionLayer* layer) const {
    std::lock_guard<std::recursive_mutex> lock(detail::optionMutex());
    const OptionValue* layerValue = findLayerValue(layer);
    if (!layerValue) {
        return true;
    }
    OptionValue withoutLayer = makeDefaultValue(m_type);
    resolveInto(withoutLayer, layer);
    return *layerValue == withoutLayer;
}

void OptionBase::markDirty() {
    std::lock_guard<std::recursive_mutex> lock(detail::optionMutex());
    OptionManager::dirtyOptions()[m_fullName] = this;
}

bool OptionBase::isDirty() const {
    std::lock_guard<std::recursive_mutex> lock(detail::optionMutex());
    const auto& dirty = OptionManager::dirtyOptions();
    auto it = dirty.find(m_fullName);
    return it != dirty.end() && it->second == this;
}

void OptionBase::invokeOnChangeCallback(void* context) const {
    if (m_onChange) {
        m_onChange(context);
    }
}

// ============================================================================
// Resolution
// ============================================================================

bool OptionBase::clampValue(OptionValue& value) const {
    switch (m_type) {
    case OptionType::Int: {
        std::int32_t& v = std::get<std::int32_t>(value);
        const std::int32_t old = v;
        if (m_minValue) {
            v = std::max(v, std::get<std::int32_t>(*m_minValue));
        }
        if (m_maxValue) {
            v = std::min(v, std::get<std::int32_t>(*m_maxValue));
        }
        return v != old;
    }
    case OptionType::Float: {
        float& v = std::get<float>(value);
        const float old = v;
        if (m_minValue) {
            v = std::max(v, std::get<float>(*m_minValue));
        }
        if (m_maxValue) {
            v = std::min(v, std::get<float>(*m_maxValue));
        }
        return v != old;
    }
    case OptionType::Vector2: return clampVector(std::get<Vec2f>(value), m_minValue, m_maxValue, 2);
    case OptionType::Vector3: return clampVector(std::get<Vec3f>(value), m_minValue, m_maxValue, 3);
    case OptionType::Vector4: return clampVector(std::get<Vec4f>(value), m_minValue, m_maxValue, 4);
    case OptionType::Vector2i: return clampVector(std::get<Vec2i>(value), m_minValue, m_maxValue, 2);
    default: return false;
    }
}

bool OptionBase::resolveInto(OptionValue& out, const OptionLayer* excludeLayer) const {
    // Layers are visited strongest first. Float types lerp: v = lerp(lerp(C, B, b), A, a) is
    // accumulated with a throughput weight, stopping at the first layer at full strength (or when
    // the remaining weight is negligible). Other types take the strongest layer whose strength
    // reaches its threshold; hash sets merge every such layer instead.
    OptionValue accumulated = makeDefaultValue(m_type);
    float throughput = 1.0f;
    bool passedExcluded = excludeLayer == nullptr;
    const bool blendable = isBlendableType(m_type);

    for (const auto& [key, layerValue] : m_layerValues) {
        if (!passedExcluded) {
            // Skip the excluded layer and every layer stronger than it.
            if (key == excludeLayer->getLayerKey()) {
                passedExcluded = true;
            }
            continue;
        }
        if (blendable) {
            const float weight = layerValue.blendStrength >= 1.0f ? throughput : layerValue.blendStrength * throughput;
            switch (m_type) {
            case OptionType::Float:
                std::get<float>(accumulated) += std::get<float>(layerValue.value) * weight;
                break;
            case OptionType::Vector2:
                addScaled(std::get<Vec2f>(accumulated), std::get<Vec2f>(layerValue.value), weight, 2);
                break;
            case OptionType::Vector3:
                addScaled(std::get<Vec3f>(accumulated), std::get<Vec3f>(layerValue.value), weight, 3);
                break;
            case OptionType::Vector4:
                addScaled(std::get<Vec4f>(accumulated), std::get<Vec4f>(layerValue.value), weight, 4);
                break;
            default: break;
            }
            if (layerValue.blendStrength >= 1.0f) {
                break;
            }
            throughput *= 1.0f - layerValue.blendStrength;
            if (throughput < 0.0001f) {
                break;
            }
        } else {
            if (layerValue.blendStrength < layerValue.blendThreshold) {
                continue;
            }
            if (m_type == OptionType::HashSet) {
                std::get<HashSetLayer>(accumulated).mergeFrom(std::get<HashSetLayer>(layerValue.value));
                continue;
            }
            accumulated = layerValue.value;
            break;
        }
    }

    clampValue(accumulated);
    if (accumulated == out) {
        return false;
    }
    out = std::move(accumulated);
    return true;
}

bool OptionBase::resolveNow() {
    const bool changed = resolveInto(m_resolved);
    if (changed) {
        onResolvedValueChanged();
    }
    return changed;
}

bool OptionBase::setBound(std::optional<OptionValue>& slot, const OptionValue& value) {
    const bool changed = !slot.has_value() || !(*slot == value);
    slot = value;
    return changed;
}

void OptionBase::writeOption(OptionConfig& config, const OptionLayer* layer, bool changedOptionOnly) const {
    if ((getFlags() & OptionFlags::NoSave) != 0 || !layer) {
        return;
    }
    const OptionValue* value = findLayerValue(layer);
    if (!value) {
        return;
    }
    if (changedOptionOnly && isLayerValueRedundant(layer)) {
        return;
    }
    config.set(m_fullName, formatOptionValue(*value));
}

} // namespace fuse::relight::options
