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
// Ported from dxvk-remix src/dxvk/rtx_render/rtx_option_manager.h@0867d3c and the system-layer setup
// of rtx_option_layer.cpp@0867d3c (RtxOptionLayer::initializeSystemLayers).
//
// Frame lifecycle (as upstream):
//   startup:     OptionSystem::initialize(desc)  -> system layers, env overrides, startup callbacks
//   every frame: code and UI call setDeferred(); Logic graphs request layer strengths
//   end of frame OptionManager::applyPendingValues(context, false)
//                 1. each layer resolves this frame's requests (enable, strength, threshold)
//                 2. dirty options resolve; onChange callbacks run for values that changed
//                 3. repeat while callbacks dirtied options, at most 4 passes (cycles terminate)
#pragma once

#include <fuse/relight/options/option.hpp>
#include <fuse/relight/options/option_layer.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>

namespace fuse::relight::options {

/// Round and clamp a (float) priority from a Logic graph into the dynamic layer range.
inline std::uint32_t clampComponentLayerPriority(float priorityValue) {
    const float rounded = std::round(priorityValue);
    if (!(rounded > static_cast<float>(kMinDynamicLayerPriority))) {
        return kMinDynamicLayerPriority;
    }
    if (rounded >= static_cast<float>(kMaxDynamicLayerPriority)) {
        return kMaxDynamicLayerPriority;
    }
    return static_cast<std::uint32_t>(rounded);
}

class OptionManager {
public:
    using OptionMap = std::map<std::string, OptionBase*, std::less<>>;
    using LayerMap = std::map<OptionLayerKey, std::unique_ptr<OptionLayer>>;

    // --- Registries -------------------------------------------------------------------------------

    /// Every registered option by full name.
    static const OptionMap& getOptions();
    /// Every layer, strongest first.
    static LayerMap& getLayerRegistry();
    static OptionLayer* getLayer(const OptionLayerKey& layerKey);

    /// Look an option up by full name, relight.* <-> rtx.* twin name, or explicit alias.
    static OptionBase* findOption(std::string_view name);
    /// Make `alias` read and look up as `target` (e.g. a renamed option's old name).
    static void addAlias(std::string alias, std::string target);
    static void removeAlias(std::string_view alias);
    /// The relight.* <-> rtx.* twin of `name` ("" when it has neither prefix).
    static std::string twinName(std::string_view name);

    // --- Layers -----------------------------------------------------------------------------------

    /// Create (or reference) a layer and return a counted handle to it. A new layer reads `config`
    /// if given, else `configPath` if non-empty, else starts empty, and is applied to every option at
    /// once. System layers keep their reserved priority; other layers are clamped into
    /// [100, 10,000,000] with a warning. Acquiring an existing key references the same layer. When
    /// the last handle goes (destructor or OptionLayerHandle::release()) the layer's values leave
    /// every option (NoReset included) and the layer is destroyed.
    static OptionLayerHandle acquireLayer(const std::string& configPath, const OptionLayerKey& layerKey,
                                          float blendStrength = kDefaultLayerBlendStrength,
                                          float blendThreshold = kDefaultLayerBlendThreshold,
                                          bool isSystemLayer = false, const OptionConfig* config = nullptr);

    // --- Serialization ----------------------------------------------------------------------------

    /// Write `layer`'s values of every saveable option into `config` (optionally only values that
    /// change the result).
    static void writeOptions(OptionConfig& config, const OptionLayer* layer, bool changedOptionsOnly);
    /// Markdown reference of every registered option (Remix RtxOptions.md format).
    static std::string generateMarkdownDocumentation();
    static bool writeMarkdownDocumentation(const std::string& outputPath);
    /// Load every option's environment variable into the Environment layer.
    static void loadAllEnvironmentVariables();

    // --- Frame ------------------------------------------------------------------------------------

    /// End-of-frame resolution (see the header comment). `forceOnChange` runs callbacks for every
    /// dirty option even when its value did not change.
    static void applyPendingValues(void* context, bool forceOnChange);
    static void logEffectiveValues();
    /// Mark every option with an onChange callback dirty (startup).
    static void markOptionsWithCallbacksDirty();
    /// Remove values from `layer` that do not change what the weaker layers resolve to.
    static std::size_t removeRedundantLayerValues(const OptionLayer* layer);

    /// An option with the InvalidatesDrawcallTranslation flag changed since the flag was cleared.
    static bool isDrawcallTranslationInvalid();
    static void clearDrawcallTranslationInvalid();

    /// Tell the router that the user's graphics preset is Custom: code-driven writes to UserSetting
    /// options then go to the User layer instead of the Quality layer.
    static void setGraphicsPresetIsCustom(bool isCustom);
    static bool isGraphicsPresetCustom();

private:
    friend class OptionBase;
    friend class OptionLayer;
    friend class OptionLayerHandle;
    friend class OptionSystem;

    /// acquireLayer() without the handle: adds one reference the caller must drop with
    /// dropLayerReference(). Used for system layers, which live until OptionSystem::shutdown().
    static OptionLayer* referenceLayer(const std::string& configPath, const OptionLayerKey& layerKey,
                                       float blendStrength, float blendThreshold, bool isSystemLayer,
                                       const OptionConfig* config);
    static void dropLayerReference(const OptionLayer* layer);
    static OptionMap& optionRegistry();
    static std::map<std::string, OptionBase*, std::less<>>& dirtyOptions();
    static bool unregisterLayer(const OptionLayer* layer);
};

/// Host inputs for OptionSystem::initialize().
struct OptionSystemDesc {
    /// Directory of the default dxvk.conf / rtx.conf / user.conf ("" = current directory, which is
    /// the game directory for an injected d3d9.dll). Paths from DXVK_CONFIG_FILE /
    /// DXVK_RTX_CONFIG_FILE are used as given.
    std::string baseDirectory;
    /// Executable name for `[Game.exe]` sections ("" = the running executable).
    std::string exeName;
    /// "Hardcoded EXE Config" layer contents (per-application defaults chosen by the host).
    OptionConfig appConfig;
    /// Returns the base-game mod directory ("" for none) given the merged dxvk.conf + app + rtx.conf
    /// config (upstream: ModManager::getBaseGameModPath from rtx.baseGameModRegex /
    /// rtx.baseGameModPathRegex). Its rtx.conf becomes the baseGameMod layer.
    std::function<std::string(const OptionConfig& mergedConfig)> baseGameModPathResolver;
    /// Load per-option environment variables into the Environment layer.
    bool loadEnvironmentVariables = true;
    /// Mark options with callbacks dirty and run applyPendingValues(nullptr, true), as the Remix
    /// runtime does right after loading its layers.
    bool runStartupCallbacks = true;
};

/// System layer setup (Remix RtxOptionLayer::initializeSystemLayers) and teardown.
class OptionSystem {
public:
    /// Create the system layers in priority order, load the files and environment overrides.
    /// Returns the merged dxvk.conf + app + rtx.conf (+ baseGameMod) config, for DXVK options.
    static const OptionConfig& initialize(const OptionSystemDesc& desc = {});
    /// Release every layer except Default Values and return all options to their defaults. For
    /// tests and full re-initialization; upstream never tears down.
    static void shutdown();
    static bool isInitialized();
    /// Merged config from initialize().
    static const OptionConfig& getMergedConfig();
    /// Parse options for every .conf read by the system (the `[section]` executable name).
    static ConfigParseOptions parseOptions();
};

} // namespace fuse::relight::options
