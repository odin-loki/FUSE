#pragma once

// LookSystem — loads a `.fuselook` blend space, evaluates the blended parameters every frame from the
// camera position, time of day, weather weights and gameplay overrides, and keeps the baked grading
// LUT (grade parameters + blended external LUTs) current. Editor-friendly: hot reload of the look file
// and every referenced `.cube` through the renderer's poll-based file watch (last good look is kept
// when an edited file fails to parse).
//
// Blend composition (deterministic, research §4.2):
//   v = defaults ⊕ base
//   v = ToD(v)                       keys on a 24 h circle, linear or smoothstep between neighbours
//   v = v + Σ_i w_i (weather_i - v)  weather weights (normalised when Σ > 1)
//   v = lerp(v, volume_k, α_k)       volumes in ascending (priority, declaration) order; α = weight ·
//                                    (1 - smoothstep(0, falloff, distance outside the shape))
//   v = lerp(v, override_j, w_j)     gameplay overrides in declaration order
// Per-parameter blend modes: Lerp, LogLerp (geometric), Step (bools/enums switch at weight 0.5 / argmax).
// Only the parameters a profile overrides take part; the LUT is blended as a weight vector over LUT slots
// (slot 0 = identity) so the baked LUT is Σ w_k · LUT_k — weights always sum to 1.
//
// evaluate() performs no heap allocation.

#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/math/vec.hpp>
#include <fuse/renderer/look/effect_graph.hpp>
#include <fuse/renderer/look/look_params.hpp>
#include <fuse/renderer/look/look_schema.hpp>
#include <fuse/renderer/look/lut3d.hpp>
#include <fuse/renderer/shader/shader_watch.hpp>
#include <fuse/types.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace fuse::renderer::look {

/// LUT slots per look (slot 0 is the implicit identity).
inline constexpr u32 kMaxLookLuts = 16u;
inline constexpr u32 kMaxLookVolumes = 256u;

struct LookFrameInput {
    math::Vec3 camera_position{};
    f32 time_of_day_hours = 12.f;
    const f32* weather_weights = nullptr; ///< indexed by LookSystem::weatherIndex
    u32 weather_count = 0;
    const f32* override_weights = nullptr; ///< indexed by LookSystem::overrideIndex
    u32 override_count = 0;
};

struct LookEvaluation {
    LookParamBlock params{};
    LookResolved resolved{};
    f32 lut_weights[kMaxLookLuts] = {};
    u32 lut_slot_count = 1;
    // Diagnostics (editor / gates).
    u32 tod_key_a = 0;
    u32 tod_key_b = 0;
    f32 tod_fraction = 0.f;
    f32 weather_weight_sum = 0.f;
    f32 volume_alpha[kMaxLookVolumes] = {}; ///< in evaluation (priority) order
    u32 volume_count = 0;
    bool lut_rebaked = false;
    u64 frame = 0;
};

struct LookSystemConfig {
    u32 lut_size = kLutSizeDefault; ///< 32 or 64 (2..128 accepted)
    kernel::Backend backend = kernel::Backend::CpuParallel;
};

/// Continuous blend helper (exposed for tests / tools).
f32 look_blend_value(f32 a, f32 b, f32 t, LookBlendMode mode);
/// Volume influence: weight * (1 - smoothstep(0, falloff, outside distance)); hard edge when falloff == 0.
f32 look_volume_alpha(const LookVolumeDesc& volume, const math::Vec3& position);

class LookSystem {
public:
    LookSystem();

    void setConfig(const LookSystemConfig& config);
    const LookSystemConfig& config() const { return m_config; }

    /// Compiles a document (LUT paths resolve against `base_dir`). On failure the previous look stays active.
    bool load(const LookDocument& doc, const std::string& base_dir, LookParseResult& diag);
    bool loadFromString(std::string_view text, const std::string& base_dir, LookParseResult& diag);
    bool loadFile(const char* path, LookParseResult& diag);

    /// Hot reload: watch the loaded file and its LUTs (poll based, no thread).
    void enableHotReload(bool enabled);
    bool hotReloadEnabled() const { return m_hotReload; }
    /// Returns true when a changed file was reloaded successfully this call. A failed reload keeps the
    /// last good look and records `lastReloadError()`.
    bool pollHotReload();
    const std::string& lastReloadError() const { return m_lastReloadError; }
    u32 reloadCount() const { return m_reloadCount; }
    u32 failedReloadCount() const { return m_failedReloadCount; }

    s32 weatherIndex(std::string_view name) const;
    s32 overrideIndex(std::string_view name) const;
    u32 weatherCount() const { return static_cast<u32>(m_weather.size()); }
    u32 overrideCount() const { return static_cast<u32>(m_overrides.size()); }

    /// Adds a runtime location volume (e.g. placed in a level); returns false when full.
    bool addVolume(const LookVolumeDesc& volume, LookParseResult& diag);
    u32 volumeCount() const { return static_cast<u32>(m_volumes.size()); }

    /// Per-frame evaluation (allocation-free). Also re-bakes the grading LUT when its inputs changed.
    const LookEvaluation& evaluate(const LookFrameInput& input);
    const LookEvaluation& lastEvaluation() const { return m_eval; }
    const LookResolved& resolved() const { return m_eval.resolved; }

    const Lut3D& gradeLut() const { return m_baked; }
    const Lut3D& lutSlot(u32 slot) const { return m_luts[slot]; }
    u32 lutSlotCount() const { return static_cast<u32>(m_luts.size()); }
    const LookEffectGraph& graph() const { return m_graph; }
    const LookDocument& document() const { return m_doc; }
    bool loaded() const { return m_loaded; }
    const std::string& path() const { return m_path; }

private:
    struct Profile {
        LookParamBlock values{};
        LookParamMask mask{};
        s32 lut_slot = -1; ///< -1 = does not override the LUT
    };
    struct Volume {
        LookVolumeDesc desc; ///< settings unused after compile
        Profile profile;
    };
    struct TimeKey {
        f32 hour = 0.f;
        Profile profile;
    };
    struct BakeKey {
        kernels::GradeParams grade{};
        f32 weights[kMaxLookLuts] = {};
        f32 strength = 0.f;
        u32 valid = 0;
    };

    bool compileProfile(const LookSettings& settings, Profile& out, const std::string& base_dir,
                        std::vector<Lut3D>& luts, std::vector<std::string>& lut_paths, LookParseResult& diag);
    void rebuildWatch();
    void bake(const LookEvaluation& eval);

    LookSystemConfig m_config{};
    LookDocument m_doc{};
    LookEffectGraph m_graph = LookEffectGraph::makeDefault();
    std::string m_path;
    std::string m_baseDir;
    bool m_loaded = false;

    LookParamBlock m_base{};
    s32 m_baseLut = -1;
    std::vector<TimeKey> m_timeKeys;
    LookTimeInterpolation m_timeInterp = LookTimeInterpolation::Linear;
    std::vector<Profile> m_weather;
    std::vector<std::string> m_weatherNames;
    std::vector<Profile> m_overrides;
    std::vector<std::string> m_overrideNames;
    std::vector<Volume> m_volumes; ///< sorted by (priority, insertion)
    std::vector<Lut3D> m_luts;     ///< slot 0 = identity
    std::vector<std::string> m_lutPaths;

    LookEvaluation m_eval{};
    Lut3D m_baked;
    BakeKey m_bakeKey{};

    ShaderFileWatch m_watch;
    bool m_hotReload = false;
    std::string m_lastReloadError;
    u32 m_reloadCount = 0;
    u32 m_failedReloadCount = 0;
};

} // namespace fuse::renderer::look
