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
// Ported from dxvk-remix src/dxvk/rtx_render/rtx_option_layer.h@0867d3c
//
// One sparse source of option values (Remix RtxOptionLayer): a config file, a code-driven layer or a
// dynamic layer controlled by Logic graphs. System layers, strongest first:
//
//   Quality Presets       0xFFFFFFFF  values applied by the selected graphics preset
//   User Settings         0xFFFFFFFE  user.conf
//   dynamic layers        100 … 10,000,000  (OptionManager::acquireLayer, Logic graphs)
//   Derived Settings      6           code-driven values, never saved
//   Environment Variables 5           per-option environment overrides
//   baseGameMod Config    4           <baseGameModPath>/rtx.conf (deprecated upstream)
//   Remix Config          3           rtx.conf (DXVK_RTX_CONFIG_FILE)
//   Hardcoded EXE Config  2           per-application defaults supplied by the host
//   DXVK Config           1           dxvk.conf (DXVK_CONFIG_FILE)
//   Default Values        0           declared defaults
//
// Layers with equal priority order by name, and the alphabetically earlier name wins.
#pragma once

#include <fuse/relight/options/option_config.hpp>
#include <fuse/relight/options/option_types.hpp>
#include <fuse/relight/options/option_value.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace fuse::relight::options {

class OptionBase;
class OptionManager;
class OptionSystem;
class OptionLayerHandle;

/// Who is editing: the user (any UI) or code (presets, callbacks, automation). Together with the
/// option's UserSetting flag it picks the layer a write goes to (OptionBase::getTargetLayer).
enum class OptionEditTarget {
    User,
    Derived,
};

/// What save() writes.
struct LayerSaveOptions {
    /// Relight default (true): the file gets this layer's values of registered, saveable options
    /// and keeps every key of the file that no registered option claims (unregistered rtx.* keys,
    /// d3d9.* / dxvk.* keys), so saving never deletes a mod's settings Relight does not know yet.
    /// Lines stay sorted by key, so the round trip is still byte-stable.
    /// False is the strict upstream rule: only registered, saveable options (keys containing "rtx."
    /// or "relight.") are written and every other line is dropped.
    bool preserveUnknownKeys = true;
};

class OptionLayer {
    friend class OptionBase;
    friend class OptionManager;
    friend class OptionSystem;

public:
    enum class EnabledRequest : std::int8_t {
        NoRequest = -1,       ///< no request this frame
        RequestDisabled = 0,  ///< only "disabled" requests
        RequestEnabled = 1,   ///< at least one "enabled" request (wins)
    };

    /// Use OptionManager::acquireLayer() instead of constructing layers directly.
    OptionLayer(OptionConfig config, std::string filePath, const OptionLayerKey& layerKey, float blendStrength,
                float blendThreshold);
    ~OptionLayer();
    OptionLayer(const OptionLayer&) = delete;
    OptionLayer& operator=(const OptionLayer&) = delete;

    // --- Per-frame requests (several components may share a layer) ---------------------------

    /// Enabled if any request this frame says enabled.
    void requestEnabled(bool enabled);
    /// The strongest request this frame wins (MAX).
    void requestBlendStrength(float strength);
    /// The lowest request this frame wins (MIN).
    void requestBlendThreshold(float threshold);
    /// Apply this frame's requests (called by OptionManager::applyPendingValues()).
    void resolvePendingRequests();
    /// Re-apply or remove the layer's values after enable/disable, and push blend changes to the
    /// options. Returns true when anything changed.
    bool applyPendingChanges();

    void markDirty() { m_dirty = true; }
    /// Read this layer's config into every registered option.
    void applyToAllOptions();
    /// Remove this layer's values from every option, except NoReset options.
    void removeFromAllOptions() const;

    void setConfig(const OptionConfig& config) { m_config = config; }

    /// The layer has a non-empty config.
    bool isValid() const { return !m_config.empty(); }
    bool isEnabled() const { return m_enabled; }
    /// Enabled and blend strength at or above the threshold (non-float values apply only then).
    bool isActive() const { return m_enabled && m_blendStrength >= m_blendThreshold; }
    const OptionConfig& getConfig() const { return m_config; }
    float getBlendStrength() const { return m_blendStrength; }
    float getBlendStrengthThreshold() const { return m_blendThreshold; }
    bool isDirty() const { return m_dirty; }
    bool isBlendStrengthDirty() const { return m_blendStrengthDirty; }
    const std::string& getName() const { return m_layerKey.name; }
    const std::string& getFilePath() const { return m_filePath; }
    const OptionLayerKey& getLayerKey() const { return m_layerKey; }

    /// Some registered option holds a value in this layer.
    bool hasValues() const;
    void setHasValues(bool hasValues) const { m_hasValues = hasValues; }

    /// Runtime values differ from the file (additions, changes or removals). File layers only.
    bool hasUnsavedChanges() const;
    /// Invalidate cached change and miscategorization state (called on every value change).
    void onLayerValueChanged() const {
        m_unsavedChangesCacheDirty = true;
        m_miscategorizedOptionCountDirty = true;
    }

    /// Options held here whose UserSetting flag says they belong in another layer.
    std::uint32_t countMiscategorizedOptions() const;
    /// Move those options to their layer (UserSetting -> User layer, others -> rtx.conf layer).
    std::uint32_t migrateMiscategorizedOptions();
    /// The file has option values that save() would remove (keys no registered option claims are
    /// kept by the default save, so they do not count).
    bool hasPendingRemovals() const;
    /// The layer is backed by a file that save() writes.
    bool hasSaveableConfigFile() const { return !m_filePath.empty(); }

    bool getPendingEnabled() const;
    float getPendingBlendStrength() const;
    float getPendingBlendThreshold() const;

    // --- Files ----------------------------------------------------------------------------------

    /// Write this layer's values to its file (sorted `key = value` lines; NoSave options are never
    /// written; unknown keys are kept unless options.preserveUnknownKeys is false). Returns false
    /// without a file or on I/O errors.
    bool save(const LayerSaveOptions& options = {});
    /// Discard runtime changes and re-read the file.
    bool reload();
    /// Write only the unsaved changes to `exportPath`, merging into an existing file: hash sets add
    /// their new opinions (overriding conflicting ones in the file), other options overwrite.
    /// Returns false when there is nothing to export.
    bool exportUnsavedChanges(const std::string& exportPath) const;

    using OptionChangeCallback = std::function<void(OptionBase*, const OptionValue*)>;
    using RemovedOptionCallback = std::function<void(OptionBase*, const std::string& savedValue)>;

    /// Classify this layer's values against its file. Any callback may be empty.
    ///  - added:     in the layer, not in the file
    ///  - modified:  in both, different
    ///  - removed:   in the file, not in the layer (file layers only)
    ///  - unchanged: in both, equal (every value, for layers without a file)
    void forEachChange(const OptionChangeCallback& addedCallback, const OptionChangeCallback& modifiedCallback,
                       const RemovedOptionCallback& removedCallback,
                       const OptionChangeCallback& unchangedCallback) const;

    // --- Config paths ----------------------------------------------------------------------------

    /// Paths from a comma-separated environment variable, or `{defaultPath}` when it is unset.
    static std::vector<std::string> resolveConfigPaths(const char* envVarName, const std::string& defaultPath);

    // --- System layers (valid after OptionSystem::initialize) -----------------------------------

    static const OptionLayer* getDefaultLayer();
    static const OptionLayer* getUserLayer();
    static OptionLayer* getRtxConfLayer();
    static const OptionLayer* getEnvironmentLayer();
    static const OptionLayer* getQualityLayer();
    static const OptionLayer* getDerivedLayer();

private:
    /// One system layer per path at `baseLayer`'s priority (references owned by OptionSystem). With several paths the earlier ones get
    /// "00_", "01_" … name prefixes and the last keeps the plain name; as names sort, the FIRST
    /// listed file is the strongest (upstream behaviour).
    static std::vector<OptionLayer*> createLayersFromEnvVar(const char* envVarName, const std::string& defaultPath,
                                                            const SystemLayerId& baseLayer);
    void setCategoryFlags(std::uint32_t flags);
    void recalculateUnsavedChangesInternal() const;
    bool isValueEqualToSaved(const OptionBase& option, const OptionValue& value, const std::string& savedValue) const;

    std::size_t getRefCount() const { return m_refCount.load(std::memory_order_acquire); }
    void incrementRefCount() const { m_refCount.fetch_add(1, std::memory_order_acq_rel); }
    void decrementRefCount() const;

    std::string m_filePath;
    OptionLayerKey m_layerKey;

    bool m_enabled = true;
    bool m_dirty = false;
    bool m_blendStrengthDirty = false;

    mutable bool m_hasValues = false;
    mutable bool m_hasUnsavedChanges = false;
    mutable bool m_unsavedChangesCacheDirty = false;
    mutable std::atomic<std::size_t> m_refCount{0};

    std::uint32_t m_categoryFlags = 0;
    mutable std::uint32_t m_miscategorizedOptionCount = 0;
    mutable bool m_miscategorizedOptionCountDirty = true;

    OptionConfig m_config;
    float m_blendStrength;
    float m_blendThreshold;

    EnabledRequest m_pendingEnabledRequest = EnabledRequest::NoRequest;
    float m_pendingMaxBlendStrength = kEmptyBlendStrengthRequest;
    float m_pendingMinBlendThreshold = kEmptyBlendThresholdRequest;
};

/// RAII: route option edits on this thread to `target` (thread-local; restores the previous target).
/// Defaults to Derived on every thread.
class OptionLayerTarget {
public:
    explicit OptionLayerTarget(OptionEditTarget target) : m_previousTarget(s_currentTarget) { s_currentTarget = target; }
    ~OptionLayerTarget() { s_currentTarget = m_previousTarget; }
    OptionLayerTarget(const OptionLayerTarget&) = delete;
    OptionLayerTarget& operator=(const OptionLayerTarget&) = delete;

    static OptionEditTarget current() { return s_currentTarget; }

private:
    OptionEditTarget m_previousTarget;
    inline static thread_local OptionEditTarget s_currentTarget = OptionEditTarget::Derived;
};

/// Counted reference to a layer from OptionManager::acquireLayer(). Move-only. The layer lives while
/// any handle to it does; destroying or release()-ing the last one removes the layer's values from
/// every option and destroys it. Dynamic layers (Logic graphs, tests, tools) are held this way.
class OptionLayerHandle {
public:
    OptionLayerHandle() noexcept = default;
    ~OptionLayerHandle() { release(); }
    OptionLayerHandle(const OptionLayerHandle&) = delete;
    OptionLayerHandle& operator=(const OptionLayerHandle&) = delete;
    OptionLayerHandle(OptionLayerHandle&& other) noexcept : m_layer(other.m_layer) { other.m_layer = nullptr; }
    OptionLayerHandle& operator=(OptionLayerHandle&& other) noexcept {
        if (this != &other) {
            release();
            m_layer = other.m_layer;
            other.m_layer = nullptr;
        }
        return *this;
    }

    /// The layer (non-owning), or nullptr for an empty handle.
    OptionLayer* get() const noexcept { return m_layer; }
    OptionLayer* operator->() const noexcept { return m_layer; }
    OptionLayer& operator*() const noexcept { return *m_layer; }
    explicit operator bool() const noexcept { return m_layer != nullptr; }

    /// Drop this reference now (the handle becomes empty). No-op on an empty handle.
    void release() noexcept;

private:
    friend class OptionManager;
    explicit OptionLayerHandle(OptionLayer* layer) noexcept : m_layer(layer) {}

    OptionLayer* m_layer = nullptr;
};

} // namespace fuse::relight::options
