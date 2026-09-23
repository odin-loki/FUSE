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
// Ported from dxvk-remix src/dxvk/rtx_render/rtx_option_manager.cpp@0867d3c and
// RtxOptionLayer::initializeSystemLayers() in rtx_option_layer.cpp@0867d3c

#include <fuse/relight/options/option_manager.hpp>

#include "options_log.hpp"
#include "options_state.hpp"

#include <fstream>
#include <vector>

namespace fuse::relight::options {

// ============================================================================
// Shared state
// ============================================================================

namespace detail {

std::recursive_mutex& optionMutex() {
    static std::recursive_mutex mutex;
    return mutex;
}

SystemLayerState& systemLayerState() {
    static SystemLayerState state;
    return state;
}

std::map<std::string, std::string, std::less<>>& aliasTable() {
    static std::map<std::string, std::string, std::less<>> table;
    return table;
}

std::atomic<bool>& graphicsPresetIsCustom() {
    static std::atomic<bool> custom{false};
    return custom;
}

bool& drawcallTranslationInvalid() {
    static bool invalid = false;
    return invalid;
}

} // namespace detail

OptionManager::OptionMap& OptionManager::optionRegistry() {
    static OptionMap registry;
    return registry;
}

std::map<std::string, OptionBase*, std::less<>>& OptionManager::dirtyOptions() {
    static std::map<std::string, OptionBase*, std::less<>> dirty;
    return dirty;
}

const OptionManager::OptionMap& OptionManager::getOptions() { return optionRegistry(); }

OptionManager::LayerMap& OptionManager::getLayerRegistry() {
    static LayerMap registry;
    return registry;
}

OptionLayer* OptionManager::getLayer(const OptionLayerKey& layerKey) {
    std::lock_guard<std::recursive_mutex> lock(detail::optionMutex());
    auto& registry = getLayerRegistry();
    auto it = registry.find(layerKey);
    return it != registry.end() ? it->second.get() : nullptr;
}

// ============================================================================
// Names and aliases
// ============================================================================

std::string OptionManager::twinName(std::string_view name) {
    constexpr std::string_view kRtx = "rtx.";
    constexpr std::string_view kRelight = "relight.";
    if (name.substr(0, kRtx.size()) == kRtx) {
        return std::string(kRelight) + std::string(name.substr(kRtx.size()));
    }
    if (name.substr(0, kRelight.size()) == kRelight) {
        return std::string(kRtx) + std::string(name.substr(kRelight.size()));
    }
    return std::string();
}

OptionBase* OptionManager::findOption(std::string_view name) {
    std::lock_guard<std::recursive_mutex> lock(detail::optionMutex());
    const OptionMap& registry = optionRegistry();
    if (auto it = registry.find(name); it != registry.end()) {
        return it->second;
    }
    const auto& aliases = detail::aliasTable();
    if (auto alias = aliases.find(name); alias != aliases.end()) {
        if (auto it = registry.find(alias->second); it != registry.end()) {
            return it->second;
        }
    }
    const std::string twin = twinName(name);
    if (!twin.empty()) {
        if (auto it = registry.find(twin); it != registry.end()) {
            return it->second;
        }
    }
    return nullptr;
}

void OptionManager::addAlias(std::string alias, std::string target) {
    std::lock_guard<std::recursive_mutex> lock(detail::optionMutex());
    detail::aliasTable().insert_or_assign(std::move(alias), std::move(target));
}

void OptionManager::removeAlias(std::string_view alias) {
    std::lock_guard<std::recursive_mutex> lock(detail::optionMutex());
    auto& table = detail::aliasTable();
    if (auto it = table.find(alias); it != table.end()) {
        table.erase(it);
    }
}

// ============================================================================
// Layers
// ============================================================================

OptionLayerHandle OptionManager::acquireLayer(const std::string& configPath, const OptionLayerKey& layerKey,
                                              float blendStrength, float blendThreshold, bool isSystemLayer,
                                              const OptionConfig* config) {
    return OptionLayerHandle(referenceLayer(configPath, layerKey, blendStrength, blendThreshold, isSystemLayer, config));
}

void OptionLayerHandle::release() noexcept {
    if (m_layer) {
        OptionManager::dropLayerReference(m_layer);
        m_layer = nullptr;
    }
}

OptionLayer* OptionManager::referenceLayer(const std::string& configPath, const OptionLayerKey& layerKey,
                                           float blendStrength, float blendThreshold, bool isSystemLayer,
                                           const OptionConfig* config) {
    std::lock_guard<std::recursive_mutex> lock(detail::optionMutex());
    auto& registry = getLayerRegistry();
    if (auto it = registry.find(layerKey); it != registry.end()) {
        it->second->incrementRefCount();
        return it->second.get();
    }

    const std::uint32_t priority = layerKey.priority;
    const bool inDynamicRange = priority >= kMinDynamicLayerPriority && priority <= kMaxDynamicLayerPriority;
    std::uint32_t effectivePriority = priority;
    if (isSystemLayer) {
        if (inDynamicRange) {
            detail::logError("System layer %s uses a dynamic-layer priority", layerKey.toString().c_str());
            assert(!inDynamicRange && "System layer priority must be outside the dynamic layer range");
        }
    } else if (!inDynamicRange) {
        effectivePriority = priority < kMinDynamicLayerPriority ? kMinDynamicLayerPriority : kMaxDynamicLayerPriority;
        detail::logWarn("Priority %u for '%s' is outside the dynamic layer range. Clamping to %u.",
                        static_cast<unsigned>(priority), layerKey.name.c_str(),
                        static_cast<unsigned>(effectivePriority));
    }
    const OptionLayerKey effectiveKey(effectivePriority, layerKey.name);
    if (effectivePriority != priority) {
        if (auto it = registry.find(effectiveKey); it != registry.end()) {
            it->second->incrementRefCount();
            return it->second.get();
        }
    }

    OptionConfig layerConfig;
    if (config) {
        layerConfig = *config;
    } else if (!configPath.empty()) {
        detail::logInfo("Attempting to parse option layer: %s...", configPath.c_str());
        layerConfig = OptionConfig::loadFile(configPath, OptionSystem::parseOptions());
    }
    auto layer = std::make_unique<OptionLayer>(std::move(layerConfig), configPath, effectiveKey, blendStrength,
                                               blendThreshold);
    OptionLayer* result = layer.get();
    registry.emplace(effectiveKey, std::move(layer));
    result->incrementRefCount();
    if (result->isEnabled()) {
        result->applyToAllOptions();
    }
    return result;
}

bool OptionManager::unregisterLayer(const OptionLayer* layer) {
    if (!layer) {
        return false;
    }
    auto& registry = getLayerRegistry();
    auto it = registry.find(layer->getLayerKey());
    if (it == registry.end()) {
        return false;
    }
    // A released layer takes every value with it, NoReset included (NoReset only protects against
    // clearing and disabling).
    for (const auto& [name, option] : optionRegistry()) {
        option->disableLayerValue(layer);
    }
    detail::SystemLayerState& state = detail::systemLayerState();
    if (state.rtxConfLayer == layer) state.rtxConfLayer = nullptr;
    if (state.userLayer == layer) state.userLayer = nullptr;
    if (state.environmentLayer == layer) state.environmentLayer = nullptr;
    if (state.qualityLayer == layer) state.qualityLayer = nullptr;
    if (state.derivedLayer == layer) state.derivedLayer = nullptr;
    if (state.defaultLayer == layer) state.defaultLayer = nullptr;
    registry.erase(it);
    return true;
}

void OptionManager::dropLayerReference(const OptionLayer* layer) {
    if (!layer) {
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(detail::optionMutex());
    auto& registry = getLayerRegistry();
    auto it = registry.find(layer->getLayerKey());
    if (it == registry.end() || it->second.get() != layer) {
        detail::logWarn("Attempted to release unknown layer %s.", layer->getLayerKey().toString().c_str());
        return;
    }
    if (layer->getRefCount() == 0) {
        detail::logWarn("Layer %s already has zero references.", layer->getLayerKey().toString().c_str());
        return;
    }
    layer->decrementRefCount();
    if (layer->getRefCount() == 0) {
        unregisterLayer(layer);
    }
}

// ============================================================================
// Frame
// ============================================================================

void OptionManager::applyPendingValues(void* context, bool forceOnChange) {
    {
        std::lock_guard<std::recursive_mutex> lock(detail::optionMutex());
        for (auto& [key, layer] : getLayerRegistry()) {
            layer->resolvePendingRequests();
            layer->applyPendingChanges();
        }
    }

    // Resolve dirty options and run callbacks; callbacks may dirty more options, which resolve in
    // the next pass. At most kMaxResolves passes, so cyclic callbacks terminate.
    constexpr int kMaxResolves = 4;
    for (int pass = 0; pass < kMaxResolves; ++pass) {
        std::vector<OptionBase*> changed;
        {
            std::lock_guard<std::recursive_mutex> lock(detail::optionMutex());
            std::map<std::string, OptionBase*, std::less<>> dirty;
            dirty.swap(dirtyOptions());
            changed.reserve(dirty.size());
            for (auto& [name, option] : dirty) {
                const bool valueChanged = option->resolveNow();
                if (forceOnChange || valueChanged) {
                    changed.push_back(option);
                }
            }
        }
        for (OptionBase* option : changed) {
            if ((option->getFlags() & OptionFlags::InvalidatesDrawcallTranslation) != 0) {
                detail::drawcallTranslationInvalid() = true;
            }
            option->invokeOnChangeCallback(context);
        }
        std::lock_guard<std::recursive_mutex> lock(detail::optionMutex());
        if (dirtyOptions().empty()) {
            break;
        }
    }

    // Dirty options never carry over to the next frame (abandoned cycles stay abandoned).
    std::lock_guard<std::recursive_mutex> lock(detail::optionMutex());
    dirtyOptions().clear();
}

void OptionManager::logEffectiveValues() {
    std::lock_guard<std::recursive_mutex> lock(detail::optionMutex());
    detail::logInfo("Effective option values (after all config layers and migrations):");
    for (const auto& [name, option] : optionRegistry()) {
        if (!option->isDefault()) {
            detail::logInfo("  %s = %s", name.c_str(), option->getResolvedValueAsString().c_str());
        }
    }
}

void OptionManager::markOptionsWithCallbacksDirty() {
    std::lock_guard<std::recursive_mutex> lock(detail::optionMutex());
    for (const auto& [name, option] : optionRegistry()) {
        if (option->hasOnChangeCallback()) {
            option->markDirty();
        }
    }
}

std::size_t OptionManager::removeRedundantLayerValues(const OptionLayer* layer) {
    if (!layer) {
        return 0;
    }
    std::lock_guard<std::recursive_mutex> lock(detail::optionMutex());
    std::size_t removed = 0;
    for (const auto& [name, option] : optionRegistry()) {
        if (!option->hasValueInLayer(layer)) {
            continue;
        }
        if (option->isLayerValueRedundant(layer)) {
            option->disableLayerValue(layer);
            option->markDirty();
            ++removed;
        }
    }
    if (removed > 0) {
        bool remaining = false;
        for (const auto& [name, option] : optionRegistry()) {
            if (option->hasValueInLayer(layer)) {
                remaining = true;
                break;
            }
        }
        layer->setHasValues(remaining);
        layer->onLayerValueChanged();
    }
    return removed;
}

bool OptionManager::isDrawcallTranslationInvalid() { return detail::drawcallTranslationInvalid(); }
void OptionManager::clearDrawcallTranslationInvalid() { detail::drawcallTranslationInvalid() = false; }

void OptionManager::setGraphicsPresetIsCustom(bool isCustom) { detail::graphicsPresetIsCustom().store(isCustom); }
bool OptionManager::isGraphicsPresetCustom() { return detail::graphicsPresetIsCustom().load(); }

// ============================================================================
// Serialization
// ============================================================================

void OptionManager::writeOptions(OptionConfig& config, const OptionLayer* layer, bool changedOptionsOnly) {
    std::lock_guard<std::recursive_mutex> lock(detail::optionMutex());
    for (const auto& [name, option] : optionRegistry()) {
        option->writeOption(config, layer, changedOptionsOnly);
    }
}

void OptionManager::loadAllEnvironmentVariables() {
    std::lock_guard<std::recursive_mutex> lock(detail::optionMutex());
    const OptionLayer* environmentLayer = OptionLayer::getEnvironmentLayer();
    if (!environmentLayer) {
        detail::logWarn("No environment layer; environment variable overrides not loaded.");
        return;
    }
    bool headerPrinted = false;
    for (const auto& [name, option] : optionRegistry()) {
        std::string value;
        if (option->loadFromEnvironmentVariable(environmentLayer, &value)) {
            if (!headerPrinted) {
                detail::logInfo("Loading environment variable overrides:");
                headerPrinted = true;
            }
            detail::logInfo("  %s = %s (from %s)", name.c_str(), value.c_str(), option->getEnvironmentVariable());
        }
    }
}

std::string OptionManager::generateMarkdownDocumentation() {
    std::lock_guard<std::recursive_mutex> lock(detail::optionMutex());
    std::string out;
    out += "# Relight Options\n";
    out += "\nThis file lists every option registered in this build of FUSE Relight. Options named `rtx.*` read and "
           "write the same keys as RTX Remix (`rtx.conf`, `user.conf`, `dxvk.conf`); each also answers to its "
           "`relight.*` twin name.\n\nIt is generated at build time; do not edit it.\n\n";

    auto escape = [](const char* text) {
        std::string escaped;
        for (const char* p = text; p && *p; ++p) {
            switch (*p) {
            case '<': escaped += "\\<"; break;
            case '>': escaped += "\\>"; break;
            case '\n': escaped += "<br>"; break;
            case '\\': escaped += "\\\\"; break;
            case '`': escaped += "\\`"; break;
            case '*': escaped += "\\*"; break;
            case '_': escaped += "\\_"; break;
            case '{': escaped += "\\{"; break;
            case '}': escaped += "\\}"; break;
            case '[': escaped += "\\["; break;
            case ']': escaped += "\\]"; break;
            case '(': escaped += "\\("; break;
            case ')': escaped += "\\)"; break;
            case '#': escaped += "\\#"; break;
            case '+': escaped += "\\+"; break;
            case '-': escaped += "\\-"; break;
            case '.': escaped += "\\."; break;
            case '!': escaped += "\\!"; break;
            case '|': escaped += "\\|"; break;
            default: escaped += *p; break;
            }
        }
        return escaped;
    };

    auto writeTable = [&](bool longTypes) {
        out += "| RTX Option | Type | Default Value | Min Value | Max Value | Description |\n";
        out += "| :-- | :-: | :-: | :-: | :-: | :-- |\n";
        for (const auto& [name, option] : optionRegistry()) { // std::map: sorted by full name
            const OptionType type = option->getType();
            const bool isLong = type == OptionType::HashSet || type == OptionType::HashVector ||
                                type == OptionType::String;
            if (isLong != longTypes) {
                continue;
            }
            const OptionValue* defaultValue = option->findLayerValue(OptionLayer::getDefaultLayer());
            out += "|" + name;
            out += "|" + std::string(option->getTypeString());
            out += "|" + (defaultValue ? formatOptionValue(*defaultValue) : std::string());
            out += "|" + (option->m_minValue ? formatOptionValue(*option->m_minValue) : std::string());
            out += "|" + (option->m_maxValue ? formatOptionValue(*option->m_maxValue) : std::string());
            out += "|" + escape(option->getDescription());
            out += "|\n";
        }
    };

    out += "## Simple Types\n";
    writeTable(false);
    out += "\n## Complex Types\n";
    writeTable(true);
    return out;
}

bool OptionManager::writeMarkdownDocumentation(const std::string& outputPath) {
    std::ofstream file(outputPath, std::ios::binary | std::ios::trunc);
    if (!file) {
        detail::logError("Failed to open %s for writing", outputPath.c_str());
        return false;
    }
    const std::string text = generateMarkdownDocumentation();
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(file);
}

// ============================================================================
// OptionSystem
// ============================================================================

namespace {

std::string joinPath(const std::string& directory, const char* file) {
    if (directory.empty()) {
        return file;
    }
    const char last = directory.back();
    return (last == '/' || last == '\\') ? directory + file : directory + "/" + file;
}

} // namespace

ConfigParseOptions OptionSystem::parseOptions() {
    ConfigParseOptions options;
    std::lock_guard<std::recursive_mutex> lock(detail::optionMutex());
    options.exeName = detail::systemLayerState().exeName;
    return options;
}

bool OptionSystem::isInitialized() {
    std::lock_guard<std::recursive_mutex> lock(detail::optionMutex());
    return detail::systemLayerState().initialized;
}

const OptionConfig& OptionSystem::getMergedConfig() { return detail::systemLayerState().mergedConfig; }

const OptionConfig& OptionSystem::initialize(const OptionSystemDesc& desc) {
    if (isInitialized()) {
        detail::logWarn("OptionSystem::initialize called twice; shutting down the previous system layers first.");
        shutdown();
    }

    {
        std::lock_guard<std::recursive_mutex> lock(detail::optionMutex());
        detail::SystemLayerState& state = detail::systemLayerState();
        detail::logInfo("Initializing option system layers...");

        state.exeName = desc.exeName.empty() ? currentExecutableName() : desc.exeName;
        state.mergedConfig = OptionConfig();
        OptionLayer::getDefaultLayer();

        auto merge = [&state](const OptionLayer* layer) {
            if (layer && layer->isValid()) {
                state.mergedConfig.merge(layer->getConfig());
            }
        };

        // 1. dxvk.conf (possibly several files through DXVK_CONFIG_FILE).
        for (const OptionLayer* layer : OptionLayer::createLayersFromEnvVar(
                 kDxvkConfEnvVar, joinPath(desc.baseDirectory, kDxvkConfFileName), kDxvkConfLayerId)) {
            merge(layer);
        }

        // 2. Per-application defaults supplied by the host (upstream: the config.cpp table).
        merge(OptionManager::referenceLayer("", kAppConfigLayerId, kDefaultLayerBlendStrength,
                                          kDefaultLayerBlendThreshold, true, &desc.appConfig));

        // 3. rtx.conf (possibly several through DXVK_RTX_CONFIG_FILE). The last one is the rtx.conf
        //    layer that user-driven edits target.
        const std::vector<OptionLayer*> rtxLayers = OptionLayer::createLayersFromEnvVar(
            kRtxConfEnvVar, joinPath(desc.baseDirectory, kRtxConfFileName), kRtxConfLayerId);
        for (const OptionLayer* layer : rtxLayers) {
            merge(layer);
        }
        state.rtxConfLayer = rtxLayers.empty() ? nullptr : rtxLayers.back();

        // 4. <baseGameModPath>/rtx.conf.
        if (desc.baseGameModPathResolver) {
            const std::string modPath = desc.baseGameModPathResolver(state.mergedConfig);
            if (!modPath.empty()) {
                detail::logInfo("Found base game mod path: %s", modPath.c_str());
                merge(OptionManager::referenceLayer(joinPath(modPath, kRtxConfFileName), kBaseGameModLayerId,
                                                  kDefaultLayerBlendStrength, kDefaultLayerBlendThreshold, true,
                                                  nullptr));
            }
        }
        state.mergedConfig.logEntries("Effective Combined Config for DXVK Options");

        // 5. Code-driven layers without files (not part of the merged config).
        state.derivedLayer = OptionManager::referenceLayer("", kDerivedLayerId, kDefaultLayerBlendStrength,
                                                         kDefaultLayerBlendThreshold, true, nullptr);
        state.environmentLayer = OptionManager::referenceLayer("", kEnvironmentLayerId, kDefaultLayerBlendStrength,
                                                             kDefaultLayerBlendThreshold, true, nullptr);
        state.qualityLayer = OptionManager::referenceLayer("", kQualityLayerId, kDefaultLayerBlendStrength,
                                                         kDefaultLayerBlendThreshold, true, nullptr);

        // 6. user.conf, reserved for UserSetting options.
        state.userLayer = OptionManager::referenceLayer(joinPath(desc.baseDirectory, kUserConfFileName), kUserLayerId,
                                                      kDefaultLayerBlendStrength, kDefaultLayerBlendThreshold, true,
                                                      nullptr);
        if (state.userLayer) {
            state.userLayer->setCategoryFlags(OptionFlags::UserSetting);
        }

        if (desc.loadEnvironmentVariables) {
            OptionManager::loadAllEnvironmentVariables();
        }
        state.initialized = true;
        detail::logInfo("Option system layer initialization complete.");
    }

    if (desc.runStartupCallbacks) {
        OptionManager::markOptionsWithCallbacksDirty();
        OptionManager::applyPendingValues(nullptr, true);
    }
    return detail::systemLayerState().mergedConfig;
}

void OptionSystem::shutdown() {
    {
        std::lock_guard<std::recursive_mutex> lock(detail::optionMutex());
        detail::SystemLayerState& state = detail::systemLayerState();
        auto& registry = OptionManager::getLayerRegistry();
        std::vector<const OptionLayer*> layers;
        for (const auto& [key, layer] : registry) {
            if (layer.get() != state.defaultLayer) {
                layers.push_back(layer.get());
            }
        }
        for (const OptionLayer* layer : layers) {
            OptionManager::unregisterLayer(layer);
        }
        state.rtxConfLayer = nullptr;
        state.userLayer = nullptr;
        state.environmentLayer = nullptr;
        state.qualityLayer = nullptr;
        state.derivedLayer = nullptr;
        state.mergedConfig = OptionConfig();
        state.initialized = false;
    }
    // Resolve every option back to its defaults (callbacks run for values that change).
    OptionManager::applyPendingValues(nullptr, false);
}

} // namespace fuse::relight::options
