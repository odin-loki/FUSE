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
// Modifications Copyright (c) 2026 FUSE contributors (AGPL-3.0)
// Ported from dxvk-remix src/dxvk/rtx_render/rtx_option_layer.cpp@0867d3c

#include <fuse/relight/options/option_layer.hpp>
#include <fuse/relight/options/option.hpp>
#include <fuse/relight/options/option_manager.hpp>

#include "options_log.hpp"
#include "options_state.hpp"

#include <cstdio>
#include <memory>

namespace fuse::relight::options {

namespace {

std::vector<std::string> splitPaths(const std::string& paths) {
    std::vector<std::string> out;
    for (std::string& piece : splitConfigList(paths)) {
        if (!piece.empty()) {
            out.push_back(std::move(piece));
        }
    }
    return out;
}

/// With several files at one priority, earlier entries get "00_", "01_" … prefixes and the last
/// keeps the plain name (so the system key lookup still finds it).
std::string makeLayerName(std::size_t index, std::size_t total, const std::string& baseName) {
    if (total <= 1 || index == total - 1) {
        return baseName;
    }
    char prefix[16];
    std::snprintf(prefix, sizeof(prefix), "%02u_", static_cast<unsigned>(index));
    return prefix + baseName;
}

bool isManagedKey(const std::string& key) {
    for (const std::string& filter : defaultSaveKeyFilters()) {
        if (key.find(filter) != std::string::npos) {
            return true;
        }
    }
    return false;
}

} // namespace

OptionLayer::OptionLayer(OptionConfig config, std::string filePath, const OptionLayerKey& layerKey,
                         float blendStrength, float blendThreshold)
    : m_filePath(std::move(filePath)),
      m_layerKey(layerKey),
      m_config(std::move(config)),
      m_blendStrength(blendStrength),
      m_blendThreshold(blendThreshold) {}

OptionLayer::~OptionLayer() = default;

// ============================================================================
// Requests
// ============================================================================

void OptionLayer::requestEnabled(bool enabled) {
    std::lock_guard<std::recursive_mutex> lock(detail::optionMutex());
    if (enabled) {
        m_pendingEnabledRequest = EnabledRequest::RequestEnabled;
    } else if (m_pendingEnabledRequest == EnabledRequest::NoRequest) {
        m_pendingEnabledRequest = EnabledRequest::RequestDisabled;
    }
}

void OptionLayer::requestBlendStrength(float strength) {
    std::lock_guard<std::recursive_mutex> lock(detail::optionMutex());
    if (strength > m_pendingMaxBlendStrength) {
        m_pendingMaxBlendStrength = strength;
    }
}

void OptionLayer::requestBlendThreshold(float threshold) {
    std::lock_guard<std::recursive_mutex> lock(detail::optionMutex());
    if (threshold < m_pendingMinBlendThreshold) {
        m_pendingMinBlendThreshold = threshold;
    }
}

void OptionLayer::resolvePendingRequests() {
    std::lock_guard<std::recursive_mutex> lock(detail::optionMutex());
    if (m_pendingEnabledRequest != EnabledRequest::NoRequest) {
        const bool enabled = m_pendingEnabledRequest == EnabledRequest::RequestEnabled;
        if (m_enabled != enabled) {
            m_enabled = enabled;
            m_dirty = true;
        }
        m_pendingEnabledRequest = EnabledRequest::NoRequest;
    }
    // Blend changes only need the options' captured strengths updated, not a config re-read.
    if (m_pendingMaxBlendStrength > kEmptyBlendStrengthRequest) {
        if (m_blendStrength != m_pendingMaxBlendStrength) {
            m_blendStrength = m_pendingMaxBlendStrength;
            m_blendStrengthDirty = true;
        }
        m_pendingMaxBlendStrength = kEmptyBlendStrengthRequest;
    }
    if (m_pendingMinBlendThreshold < kEmptyBlendThresholdRequest) {
        if (m_blendThreshold != m_pendingMinBlendThreshold) {
            m_blendThreshold = m_pendingMinBlendThreshold;
            m_blendStrengthDirty = true;
        }
        m_pendingMinBlendThreshold = kEmptyBlendThresholdRequest;
    }
}

bool OptionLayer::applyPendingChanges() {
    std::lock_guard<std::recursive_mutex> lock(detail::optionMutex());
    bool anyChanges = false;
    if (m_dirty) {
        if (m_enabled) {
            applyToAllOptions();
        } else {
            removeFromAllOptions();
        }
        m_dirty = false;
        anyChanges = true;
    }
    if (m_blendStrengthDirty) {
        // Also covers runtime values set with setDeferred() that are not in the config.
        for (const auto& [name, option] : OptionManager::getOptions()) {
            option->updateLayerBlendStrength(*this);
        }
        m_blendStrengthDirty = false;
        anyChanges = true;
    }
    return anyChanges;
}

bool OptionLayer::getPendingEnabled() const {
    if (m_pendingEnabledRequest != EnabledRequest::NoRequest) {
        return m_pendingEnabledRequest == EnabledRequest::RequestEnabled;
    }
    return m_enabled;
}

float OptionLayer::getPendingBlendStrength() const {
    return m_pendingMaxBlendStrength > kEmptyBlendStrengthRequest ? m_pendingMaxBlendStrength : m_blendStrength;
}

float OptionLayer::getPendingBlendThreshold() const {
    return m_pendingMinBlendThreshold < kEmptyBlendThresholdRequest ? m_pendingMinBlendThreshold : m_blendThreshold;
}

// ============================================================================
// Applying values
// ============================================================================

void OptionLayer::applyToAllOptions() {
    std::lock_guard<std::recursive_mutex> lock(detail::optionMutex());
    if (!isValid()) {
        return;
    }
    for (const auto& [name, option] : OptionManager::getOptions()) {
        option->readOptionLayer(*this);
    }
    // Config-loaded values captured the current blend strength on insertion.
    m_blendStrengthDirty = false;
}

void OptionLayer::removeFromAllOptions() const {
    std::lock_guard<std::recursive_mutex> lock(detail::optionMutex());
    for (const auto& [name, option] : OptionManager::getOptions()) {
        if ((option->getFlags() & OptionFlags::NoReset) != 0) {
            continue;
        }
        option->disableLayerValue(this);
    }
    onLayerValueChanged();
}

bool OptionLayer::hasValues() const {
    std::lock_guard<std::recursive_mutex> lock(detail::optionMutex());
    if (!m_hasValues) {
        return false;
    }
    for (const auto& [name, option] : OptionManager::getOptions()) {
        if (option->hasValueInLayer(this)) {
            m_hasValues = true;
            return true;
        }
    }
    m_hasValues = false;
    return false;
}

// ============================================================================
// Unsaved changes
// ============================================================================

bool OptionLayer::isValueEqualToSaved(const OptionBase& option, const OptionValue& value,
                                      const std::string& savedValue) const {
    if (option.getType() == OptionType::HashSet) {
        // Order-independent comparison.
        HashSetLayer saved;
        saved.parseFromStrings(splitConfigList(savedValue));
        return std::get<HashSetLayer>(value) == saved;
    }
    return formatOptionValue(value) == savedValue;
}

bool OptionLayer::hasUnsavedChanges() const {
    std::lock_guard<std::recursive_mutex> lock(detail::optionMutex());
    if (!hasSaveableConfigFile()) {
        return false;
    }
    if (m_unsavedChangesCacheDirty) {
        recalculateUnsavedChangesInternal();
    }
    return m_hasUnsavedChanges;
}

void OptionLayer::recalculateUnsavedChangesInternal() const {
    m_unsavedChangesCacheDirty = false;
    if (!hasSaveableConfigFile()) {
        m_hasUnsavedChanges = false;
        return;
    }
    for (const auto& [name, option] : OptionManager::getOptions()) {
        const OptionValue* value = option->findLayerValue(this);
        if (!value || formatOptionValue(*value).empty()) {
            continue;
        }
        const std::string* saved = option->findConfigValue(m_config);
        if (!saved || !isValueEqualToSaved(*option, *value, *saved)) {
            m_hasUnsavedChanges = true;
            return;
        }
    }
    m_hasUnsavedChanges = hasPendingRemovals();
}

bool OptionLayer::hasPendingRemovals() const {
    std::lock_guard<std::recursive_mutex> lock(detail::optionMutex());
    if (!hasSaveableConfigFile()) {
        return false;
    }
    for (const auto& [key, savedValue] : m_config.entries()) {
        if (!isManagedKey(key)) {
            continue; // not an option key; kept out of option management
        }
        const OptionBase* option = OptionManager::findOption(key);
        if (!option) {
            continue; // unknown keys survive the default save (preserveUnknownKeys)
        }
        const OptionValue* value = option->findLayerValue(this);
        if (!value || formatOptionValue(*value).empty()) {
            return true;
        }
    }
    return false;
}

// ============================================================================
// Layer placement (UserSetting)
// ============================================================================

void OptionLayer::setCategoryFlags(std::uint32_t flags) {
    if (m_categoryFlags != flags) {
        m_categoryFlags = flags;
        m_miscategorizedOptionCountDirty = true;
    }
}

std::uint32_t OptionLayer::countMiscategorizedOptions() const {
    std::lock_guard<std::recursive_mutex> lock(detail::optionMutex());
    if (!m_miscategorizedOptionCountDirty) {
        return m_miscategorizedOptionCount;
    }
    m_miscategorizedOptionCountDirty = false;
    m_miscategorizedOptionCount = 0;
    for (const auto& [name, option] : OptionManager::getOptions()) {
        if (!option->hasValueInLayer(this)) {
            continue;
        }
        const std::uint32_t layerFlags = option->getFlags() & kOptionCategoryFlags;
        const bool misplaced = m_categoryFlags != 0 ? (layerFlags & m_categoryFlags) == 0 : layerFlags != 0;
        if (misplaced) {
            ++m_miscategorizedOptionCount;
        }
    }
    return m_miscategorizedOptionCount;
}

std::uint32_t OptionLayer::migrateMiscategorizedOptions() {
    std::lock_guard<std::recursive_mutex> lock(detail::optionMutex());
    std::uint32_t migrated = 0;
    for (const auto& [name, option] : OptionManager::getOptions()) {
        if (!option->hasValueInLayer(this)) {
            continue;
        }
        const std::uint32_t layerFlags = option->getFlags() & kOptionCategoryFlags;
        const bool misplaced = m_categoryFlags != 0 ? (layerFlags & m_categoryFlags) == 0 : layerFlags != 0;
        if (!misplaced) {
            continue;
        }
        OptionLayer* destination = (layerFlags & OptionFlags::UserSetting) != 0 ? detail::systemLayerState().userLayer
                                                                                  : getRtxConfLayer();
        if (destination && destination != this) {
            option->moveLayerValue(this, destination);
            ++migrated;
        }
    }
    return migrated;
}

// ============================================================================
// Files
// ============================================================================

bool OptionLayer::save(const LayerSaveOptions& options) {
    std::lock_guard<std::recursive_mutex> lock(detail::optionMutex());
    if (!hasSaveableConfigFile()) {
        detail::logWarn("Cannot save layer '%s' - no associated config file.", getName().c_str());
        return false;
    }
    OptionConfig layerConfig;
    OptionManager::writeOptions(layerConfig, this, false);
    if (options.preserveUnknownKeys) {
        for (const auto& [key, value] : m_config.entries()) {
            if (!OptionManager::findOption(key) && !layerConfig.find(key)) {
                layerConfig.set(key, value);
            }
        }
    }
    setConfig(layerConfig);
    // Preserved files keep every line they had (d3d9.* included); otherwise only option keys.
    const bool written = m_config.saveFile(
        m_filePath, options.preserveUnknownKeys ? std::vector<std::string>{} : defaultSaveKeyFilters());
    m_hasUnsavedChanges = false;
    m_unsavedChangesCacheDirty = false;
    if (written) {
        detail::logInfo("Saved layer config to '%s'", m_filePath.c_str());
    }
    return written;
}

bool OptionLayer::reload() {
    std::lock_guard<std::recursive_mutex> lock(detail::optionMutex());
    if (!hasSaveableConfigFile()) {
        detail::logWarn("Cannot reload layer '%s' - no associated config file.", getName().c_str());
        return false;
    }
    removeFromAllOptions();
    setConfig(OptionConfig::loadFile(m_filePath, OptionSystem::parseOptions()));
    if (isValid()) {
        applyToAllOptions();
    }
    m_hasUnsavedChanges = false;
    m_unsavedChangesCacheDirty = false;
    m_miscategorizedOptionCountDirty = true;
    setHasValues(isValid());
    detail::logInfo("Reloaded layer config from '%s'", m_filePath.c_str());
    return true;
}

bool OptionLayer::exportUnsavedChanges(const std::string& exportPath) const {
    std::lock_guard<std::recursive_mutex> lock(detail::optionMutex());
    if (!hasUnsavedChanges()) {
        detail::logWarn("No unsaved changes to export from layer '%s'.", getName().c_str());
        return false;
    }
    OptionConfig exportConfig = OptionConfig::loadFile(exportPath, OptionSystem::parseOptions());
    const bool isNewFile = exportConfig.empty();

    auto processOption = [&](OptionBase* option, const OptionValue* value) {
        const std::string& fullName = option->getFullName();
        if (option->getType() == OptionType::HashSet) {
            // Export only the opinions added since the file was saved.
            HashSetLayer saved;
            if (const std::string* savedText = option->findConfigValue(m_config)) {
                saved.parseFromStrings(splitConfigList(*savedText));
            }
            HashSetLayer added = std::get<HashSetLayer>(*value).computeAddedOpinions(saved);
            if (added.empty()) {
                return;
            }
            if (const std::string* existingText = option->findConfigValue(exportConfig)) {
                HashSetLayer existing;
                existing.parseFromStrings(splitConfigList(*existingText));
                added.mergeFrom(existing); // new opinions win, the file fills the rest
            }
            exportConfig.set(fullName, added.toString());
        } else {
            const std::string current = formatOptionValue(*value);
            if (!current.empty()) {
                exportConfig.set(fullName, current);
            }
        }
    };
    forEachChange(processOption, processOption, nullptr, nullptr);

    if (!exportConfig.saveFile(exportPath, defaultSaveKeyFilters())) {
        return false;
    }
    if (isNewFile) {
        detail::logInfo("Created new config file with unsaved changes: %s", exportPath.c_str());
    } else {
        detail::logInfo("Merged unsaved changes into existing config file: %s", exportPath.c_str());
    }
    return true;
}

void OptionLayer::forEachChange(const OptionChangeCallback& addedCallback,
                                const OptionChangeCallback& modifiedCallback,
                                const RemovedOptionCallback& removedCallback,
                                const OptionChangeCallback& unchangedCallback) const {
    std::lock_guard<std::recursive_mutex> lock(detail::optionMutex());
    const bool saveable = hasSaveableConfigFile();
    if (addedCallback || modifiedCallback || unchangedCallback) {
        for (const auto& [name, option] : OptionManager::getOptions()) {
            const OptionValue* value = option->findLayerValue(this);
            if (!value) {
                continue;
            }
            if (!saveable) {
                if (unchangedCallback) {
                    unchangedCallback(option, value);
                }
                continue;
            }
            const std::string* saved = option->findConfigValue(m_config);
            if (!saved) {
                if (addedCallback) {
                    addedCallback(option, value);
                }
            } else if (!isValueEqualToSaved(*option, *value, *saved)) {
                if (modifiedCallback) {
                    modifiedCallback(option, value);
                }
            } else if (unchangedCallback) {
                unchangedCallback(option, value);
            }
        }
    }
    if (removedCallback && saveable) {
        for (const auto& [key, savedValue] : m_config.entries()) {
            OptionBase* option = OptionManager::findOption(key);
            if (option && !option->findLayerValue(this)) {
                removedCallback(option, savedValue);
            }
        }
    }
}

// ============================================================================
// Config paths and system layer accessors
// ============================================================================

std::vector<std::string> OptionLayer::resolveConfigPaths(const char* envVarName, const std::string& defaultPath) {
    const std::string envValue = getEnvironmentVariable(envVarName);
    if (!envValue.empty()) {
        detail::logInfo("Using config paths from %s: %s", envVarName, envValue.c_str());
        return splitPaths(envValue);
    }
    return {defaultPath};
}

std::vector<OptionLayer*> OptionLayer::createLayersFromEnvVar(const char* envVarName, const std::string& defaultPath,
                                                              const SystemLayerId& baseLayer) {
    std::vector<OptionLayer*> layers;
    const std::vector<std::string> paths = resolveConfigPaths(envVarName, defaultPath);
    for (std::size_t i = 0; i < paths.size(); ++i) {
        const OptionLayerKey key(baseLayer.priority, makeLayerName(i, paths.size(), baseLayer.name));
        if (OptionLayer* layer = OptionManager::referenceLayer(paths[i], key, kDefaultLayerBlendStrength,
                                                             kDefaultLayerBlendThreshold, true, nullptr)) {
            layers.push_back(layer);
        }
    }
    return layers;
}

const OptionLayer* OptionLayer::getDefaultLayer() {
    // Created on first use: option constructors call this during static initialization, before
    // OptionSystem::initialize() creates the other system layers.
    std::lock_guard<std::recursive_mutex> lock(detail::optionMutex());
    detail::SystemLayerState& state = detail::systemLayerState();
    if (!state.defaultLayer) {
        auto layer = std::make_unique<OptionLayer>(OptionConfig(), std::string(), OptionLayerKey(kDefaultLayerId),
                                                   kDefaultLayerBlendStrength, kDefaultLayerBlendThreshold);
        layer->incrementRefCount();
        auto inserted = OptionManager::getLayerRegistry().emplace(layer->getLayerKey(), std::move(layer));
        state.defaultLayer = inserted.first->second.get();
    }
    return state.defaultLayer;
}

const OptionLayer* OptionLayer::getUserLayer() { return detail::systemLayerState().userLayer; }
OptionLayer* OptionLayer::getRtxConfLayer() { return detail::systemLayerState().rtxConfLayer; }
const OptionLayer* OptionLayer::getEnvironmentLayer() { return detail::systemLayerState().environmentLayer; }
const OptionLayer* OptionLayer::getQualityLayer() { return detail::systemLayerState().qualityLayer; }
const OptionLayer* OptionLayer::getDerivedLayer() { return detail::systemLayerState().derivedLayer; }

void OptionLayer::decrementRefCount() const {
    std::size_t expected = m_refCount.load(std::memory_order_acquire);
    while (expected > 0) {
        if (m_refCount.compare_exchange_weak(expected, expected - 1, std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
            break;
        }
    }
}

} // namespace fuse::relight::options
