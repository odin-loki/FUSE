#include <fuse/renderer/look/look_system.hpp>

#include <fuse/compute_kernel/launch.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>

namespace fuse::renderer::look {

using math::Vec3;

namespace {

f32 smooth01(f32 t) {
    t = std::clamp(t, 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

std::string dirOf(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

std::string joinPath(const std::string& dir, const std::string& rel) {
    if (rel.empty()) {
        return rel;
    }
    if (rel.front() == '/' || rel.front() == '\\' || (rel.size() > 1u && rel[1] == ':')) {
        return rel;
    }
    return dir.empty() ? rel : dir + "/" + rel;
}

} // namespace

f32 look_blend_value(f32 a, f32 b, f32 t, LookBlendMode mode) {
    if (t <= 0.f) {
        return a;
    }
    if (t >= 1.f) {
        return b;
    }
    switch (mode) {
    case LookBlendMode::Step:
        return t >= 0.5f ? b : a;
    case LookBlendMode::LogLerp:
        if (a > 0.f && b > 0.f) {
            return a * std::pow(b / a, t);
        }
        return a + (b - a) * t;
    case LookBlendMode::Lerp:
    default:
        return a + (b - a) * t;
    }
}

f32 look_volume_alpha(const LookVolumeDesc& v, const Vec3& p) {
    f32 d = 0.f;
    const Vec3 rel = p - v.center;
    if (v.shape == LookVolumeShape::Sphere) {
        d = std::max(rel.length() - v.radius, 0.f);
    } else {
        const f32 qx = std::max(std::fabs(rel.x) - v.half_extents.x, 0.f);
        const f32 qy = std::max(std::fabs(rel.y) - v.half_extents.y, 0.f);
        const f32 qz = std::max(std::fabs(rel.z) - v.half_extents.z, 0.f);
        d = std::sqrt(qx * qx + qy * qy + qz * qz);
    }
    f32 influence = 0.f;
    if (v.falloff <= 0.f) {
        influence = d <= 0.f ? 1.f : 0.f;
    } else {
        influence = 1.f - smooth01(d / v.falloff);
    }
    return std::clamp(v.weight, 0.f, 1.f) * influence;
}

LookSystem::LookSystem() {
    m_luts.push_back(Lut3D::identity(m_config.lut_size));
    m_lutPaths.emplace_back();
    m_baked = Lut3D::identity(m_config.lut_size);
    m_eval.params = look_default_params();
    m_eval.resolved = look_resolve(m_eval.params);
    m_eval.lut_weights[0] = 1.f;
    m_volumes.reserve(kMaxLookVolumes);
}

void LookSystem::setConfig(const LookSystemConfig& config) {
    const bool resize = config.lut_size != m_config.lut_size;
    m_config = config;
    m_config.lut_size = std::clamp(m_config.lut_size, kLutMinSize, kLutMaxSize);
    if (resize) {
        for (Lut3D& lut : m_luts) {
            if (lut.size != m_config.lut_size) {
                lut_resample(lut, m_config.lut_size, lut);
            }
        }
        m_luts[0] = Lut3D::identity(m_config.lut_size);
        m_baked = Lut3D::identity(m_config.lut_size);
        m_bakeKey.valid = 0;
    }
}

bool LookSystem::compileProfile(const LookSettings& settings, Profile& out, const std::string& base_dir,
                                std::vector<Lut3D>& luts, std::vector<std::string>& lut_paths,
                                LookParseResult& diag) {
    out.values = settings.values;
    out.mask = settings.mask;
    out.lut_slot = -1;
    if (!settings.lut_set) {
        return true;
    }
    if (settings.lut_path.empty()) {
        out.lut_slot = 0;
        return true;
    }
    const std::string full = joinPath(base_dir, settings.lut_path);
    for (size_t i = 1; i < lut_paths.size(); ++i) {
        if (lut_paths[i] == full) {
            out.lut_slot = static_cast<s32>(i);
            return true;
        }
    }
    if (luts.size() >= kMaxLookLuts) {
        diag.error = "too many LUTs (max " + std::to_string(kMaxLookLuts - 1u) + " per look)";
        return false;
    }
    Lut3D lut;
    CubeParseResult cube;
    if (!lut_load_cube(full.c_str(), lut, cube, m_config.lut_size)) {
        diag.error = settings.lut_path + ": " + cube.error;
        return false;
    }
    out.lut_slot = static_cast<s32>(luts.size());
    luts.push_back(std::move(lut));
    lut_paths.push_back(full);
    return true;
}

bool LookSystem::load(const LookDocument& doc, const std::string& base_dir, LookParseResult& diag) {
    diag.ok = false;
    const LookGraphValidation gv = doc.graph.validate();
    if (!gv.ok()) {
        diag.error = std::string("graph: ") + look_graph_error_name(gv.error);
        return false;
    }
    std::vector<Lut3D> luts;
    std::vector<std::string> lutPaths;
    luts.push_back(Lut3D::identity(m_config.lut_size));
    lutPaths.emplace_back();

    Profile base;
    if (!compileProfile(doc.base, base, base_dir, luts, lutPaths, diag)) {
        return false;
    }
    std::vector<TimeKey> keys;
    for (const LookTimeKey& k : doc.time_keys) {
        TimeKey tk;
        tk.hour = k.hour;
        if (!compileProfile(k.settings, tk.profile, base_dir, luts, lutPaths, diag)) {
            return false;
        }
        keys.push_back(tk);
    }
    std::stable_sort(keys.begin(), keys.end(), [](const TimeKey& a, const TimeKey& b) { return a.hour < b.hour; });
    std::vector<Profile> weather;
    std::vector<std::string> weatherNames;
    for (const LookWeatherState& w : doc.weather) {
        Profile p;
        if (!compileProfile(w.settings, p, base_dir, luts, lutPaths, diag)) {
            return false;
        }
        weather.push_back(p);
        weatherNames.push_back(w.name);
    }
    std::vector<Profile> overrides;
    std::vector<std::string> overrideNames;
    for (const LookWeatherState& w : doc.overrides) {
        Profile p;
        if (!compileProfile(w.settings, p, base_dir, luts, lutPaths, diag)) {
            return false;
        }
        overrides.push_back(p);
        overrideNames.push_back(w.name);
    }
    if (doc.volumes.size() > kMaxLookVolumes) {
        diag.error = "too many volumes";
        return false;
    }
    std::vector<Volume> volumes;
    volumes.reserve(kMaxLookVolumes);
    for (const LookVolumeDesc& v : doc.volumes) {
        Volume vol;
        vol.desc = v;
        vol.desc.settings = {};
        if (!compileProfile(v.settings, vol.profile, base_dir, luts, lutPaths, diag)) {
            return false;
        }
        volumes.push_back(std::move(vol));
    }
    std::stable_sort(volumes.begin(), volumes.end(),
                     [](const Volume& a, const Volume& b) { return a.desc.priority < b.desc.priority; });

    // Commit.
    m_doc = doc;
    m_graph = doc.graph;
    m_baseDir = base_dir;
    m_base = look_default_params();
    for (u32 i = 0; i < kLookParamCount; ++i) {
        const LookParam p = static_cast<LookParam>(i);
        if (base.mask.test(p)) {
            for (u32 c = 0; c < kLookSlotsPerParam; ++c) {
                m_base.set(p, base.values.get(p, c), c);
            }
        }
    }
    look_clamp_params(m_base);
    m_baseLut = base.lut_slot;
    m_timeKeys = std::move(keys);
    m_timeInterp = doc.time_interpolation;
    m_weather = std::move(weather);
    m_weatherNames = std::move(weatherNames);
    m_overrides = std::move(overrides);
    m_overrideNames = std::move(overrideNames);
    m_volumes = std::move(volumes);
    m_luts = std::move(luts);
    m_lutPaths = std::move(lutPaths);
    m_bakeKey.valid = 0;
    m_loaded = true;
    diag.ok = true;
    if (m_hotReload) {
        rebuildWatch();
    }
    return true;
}

bool LookSystem::loadFromString(std::string_view text, const std::string& base_dir, LookParseResult& diag) {
    LookDocument doc;
    if (!look_parse(text, doc, diag)) {
        return false;
    }
    std::vector<std::string> warnings = std::move(diag.warnings);
    const bool ok = load(doc, base_dir, diag);
    diag.warnings = std::move(warnings);
    return ok;
}

bool LookSystem::loadFile(const char* path, LookParseResult& diag) {
    LookDocument doc;
    if (!look_load_file(path, doc, diag)) {
        return false;
    }
    std::vector<std::string> warnings = std::move(diag.warnings);
    const std::string p = path;
    const bool ok = load(doc, dirOf(p), diag);
    diag.warnings = std::move(warnings);
    if (ok) {
        m_path = p;
        if (m_hotReload) {
            rebuildWatch();
        }
    }
    return ok;
}

void LookSystem::rebuildWatch() {
    m_watch = ShaderFileWatch{};
    if (!m_path.empty()) {
        m_watch.watch(m_path.c_str());
    }
    for (size_t i = 1; i < m_lutPaths.size(); ++i) {
        m_watch.watch(m_lutPaths[i].c_str());
    }
}

void LookSystem::enableHotReload(bool enabled) {
    m_hotReload = enabled;
    if (enabled) {
        rebuildWatch();
    }
}

bool LookSystem::pollHotReload() {
    if (!m_hotReload || m_path.empty() || m_watch.pollChanged() == 0u) {
        return false;
    }
    LookParseResult diag;
    const std::string path = m_path; // loadFile may reassign
    if (loadFile(path.c_str(), diag)) {
        m_lastReloadError.clear();
        ++m_reloadCount;
        return true;
    }
    m_lastReloadError = diag.error;
    ++m_failedReloadCount;
    return false;
}

s32 LookSystem::weatherIndex(std::string_view name) const {
    for (size_t i = 0; i < m_weatherNames.size(); ++i) {
        if (m_weatherNames[i] == name) {
            return static_cast<s32>(i);
        }
    }
    return -1;
}

s32 LookSystem::overrideIndex(std::string_view name) const {
    for (size_t i = 0; i < m_overrideNames.size(); ++i) {
        if (m_overrideNames[i] == name) {
            return static_cast<s32>(i);
        }
    }
    return -1;
}

bool LookSystem::addVolume(const LookVolumeDesc& volume, LookParseResult& diag) {
    diag = {};
    if (m_volumes.size() >= kMaxLookVolumes) {
        diag.error = "too many volumes";
        return false;
    }
    Volume vol;
    vol.desc = volume;
    vol.desc.settings = {};
    if (!compileProfile(volume.settings, vol.profile, m_baseDir, m_luts, m_lutPaths, diag)) {
        return false;
    }
    const auto at = std::upper_bound(m_volumes.begin(), m_volumes.end(), vol.desc.priority,
                                     [](s32 pr, const Volume& v) { return pr < v.desc.priority; });
    m_volumes.insert(at, std::move(vol));
    diag.ok = true;
    return true;
}

const LookEvaluation& LookSystem::evaluate(const LookFrameInput& input) {
    LookEvaluation& e = m_eval;
    LookParamBlock& v = e.params;
    v = m_base;
    const u32 slots = static_cast<u32>(m_luts.size());
    f32 W[kMaxLookLuts] = {};
    W[m_baseLut >= 0 ? m_baseLut : 0] = 1.f;

    // ---- Time of day (24 h circle)
    e.tod_key_a = e.tod_key_b = 0;
    e.tod_fraction = 0.f;
    const u32 keyCount = static_cast<u32>(m_timeKeys.size());
    if (keyCount > 0u) {
        f32 t = std::fmod(input.time_of_day_hours, 24.f);
        if (t < 0.f) {
            t += 24.f;
        }
        u32 a = keyCount - 1u; // last key with hour <= t (wraps to the last key)
        for (u32 i = 0; i < keyCount; ++i) {
            if (m_timeKeys[i].hour <= t) {
                a = i;
            }
        }
        const u32 b = (a + 1u) % keyCount;
        f32 f = 0.f;
        if (keyCount > 1u) {
            f32 span = m_timeKeys[b].hour - m_timeKeys[a].hour;
            if (span <= 0.f) {
                span += 24.f;
            }
            f32 into = t - m_timeKeys[a].hour;
            if (into < 0.f) {
                into += 24.f;
            }
            f = std::clamp(into / span, 0.f, 1.f);
            if (m_timeInterp == LookTimeInterpolation::Smooth) {
                f = smooth01(f);
            }
        }
        e.tod_key_a = a;
        e.tod_key_b = b;
        e.tod_fraction = f;
        const Profile& A = m_timeKeys[a].profile;
        const Profile& B = m_timeKeys[b].profile;
        for (u32 i = 0; i < kLookParamCount; ++i) {
            const LookParam p = static_cast<LookParam>(i);
            const bool ma = A.mask.test(p);
            const bool mb = B.mask.test(p);
            if (!ma && !mb) {
                continue;
            }
            const LookParamInfo& info = look_param_info(p);
            for (u32 c = 0; c < look_param_components(info.type); ++c) {
                const f32 cur = v.get(p, c);
                v.set(p, look_blend_value(ma ? A.values.get(p, c) : cur, mb ? B.values.get(p, c) : cur, f, info.blend),
                      c);
            }
        }
        if (A.lut_slot >= 0 || B.lut_slot >= 0) {
            f32 WA[kMaxLookLuts];
            f32 WB[kMaxLookLuts];
            for (u32 k = 0; k < kMaxLookLuts; ++k) {
                WA[k] = A.lut_slot >= 0 ? (static_cast<s32>(k) == A.lut_slot ? 1.f : 0.f) : W[k];
                WB[k] = B.lut_slot >= 0 ? (static_cast<s32>(k) == B.lut_slot ? 1.f : 0.f) : W[k];
            }
            for (u32 k = 0; k < kMaxLookLuts; ++k) {
                W[k] = WA[k] + (WB[k] - WA[k]) * f;
            }
        }
    }

    // ---- Weather (weighted sum; weights normalised when their sum exceeds 1)
    const u32 weatherCount = std::min<u32>(input.weather_count, static_cast<u32>(m_weather.size()));
    f32 wsum = 0.f;
    for (u32 i = 0; i < weatherCount; ++i) {
        wsum += std::max(input.weather_weights[i], 0.f);
    }
    e.weather_weight_sum = wsum;
    if (wsum > 0.f) {
        const f32 norm = wsum > 1.f ? 1.f / wsum : 1.f;
        const f32 wRest = wsum > 1.f ? 0.f : 1.f - wsum;
        for (u32 i = 0; i < kLookParamCount; ++i) {
            const LookParam p = static_cast<LookParam>(i);
            const LookParamInfo& info = look_param_info(p);
            bool any = false;
            for (u32 s = 0; s < weatherCount && !any; ++s) {
                any = m_weather[s].mask.test(p) && input.weather_weights[s] > 0.f;
            }
            if (!any) {
                continue;
            }
            for (u32 c = 0; c < look_param_components(info.type); ++c) {
                const f32 cur = v.get(p, c);
                if (info.blend == LookBlendMode::Step) {
                    f32 keepWeight = wRest;
                    f32 best = -1.f;
                    f32 bestValue = cur;
                    for (u32 s = 0; s < weatherCount; ++s) {
                        const f32 w = std::max(input.weather_weights[s], 0.f) * norm;
                        if (!m_weather[s].mask.test(p)) {
                            keepWeight += w;
                        } else if (w > best) {
                            best = w;
                            bestValue = m_weather[s].values.get(p, c);
                        }
                    }
                    v.set(p, best > keepWeight ? bestValue : cur, c);
                    continue;
                }
                const bool logBlend = info.blend == LookBlendMode::LogLerp && cur > 0.f;
                f32 acc = 0.f;
                bool logOk = logBlend;
                for (u32 s = 0; s < weatherCount; ++s) {
                    const f32 w = std::max(input.weather_weights[s], 0.f) * norm;
                    if (w <= 0.f || !m_weather[s].mask.test(p)) {
                        continue;
                    }
                    const f32 x = m_weather[s].values.get(p, c);
                    if (logOk && x > 0.f) {
                        acc += w * std::log(x / cur);
                    } else {
                        logOk = false;
                    }
                }
                if (logOk) {
                    v.set(p, cur * std::exp(acc), c);
                } else {
                    acc = 0.f;
                    for (u32 s = 0; s < weatherCount; ++s) {
                        const f32 w = std::max(input.weather_weights[s], 0.f) * norm;
                        if (w > 0.f && m_weather[s].mask.test(p)) {
                            acc += w * (m_weather[s].values.get(p, c) - cur);
                        }
                    }
                    v.set(p, cur + acc, c);
                }
            }
        }
        // LUT weights: states without a LUT keep the current mix.
        f32 keep = wRest;
        f32 add[kMaxLookLuts] = {};
        for (u32 s = 0; s < weatherCount; ++s) {
            const f32 w = std::max(input.weather_weights[s], 0.f) * norm;
            if (m_weather[s].lut_slot >= 0) {
                add[m_weather[s].lut_slot] += w;
            } else {
                keep += w;
            }
        }
        for (u32 k = 0; k < kMaxLookLuts; ++k) {
            W[k] = W[k] * keep + add[k];
        }
    }

    // ---- Location volumes (ascending priority), then gameplay overrides.
    const auto applyLayer = [&v, &W](const Profile& prof, f32 alpha) {
        if (alpha <= 0.f) {
            return;
        }
        for (u32 i = 0; i < kLookParamCount; ++i) {
            const LookParam p = static_cast<LookParam>(i);
            if (!prof.mask.test(p)) {
                continue;
            }
            const LookParamInfo& info = look_param_info(p);
            for (u32 c = 0; c < look_param_components(info.type); ++c) {
                v.set(p, look_blend_value(v.get(p, c), prof.values.get(p, c), alpha, info.blend), c);
            }
        }
        if (prof.lut_slot >= 0) {
            const f32 a = std::min(alpha, 1.f);
            for (u32 k = 0; k < kMaxLookLuts; ++k) {
                W[k] = W[k] * (1.f - a) + (static_cast<s32>(k) == prof.lut_slot ? a : 0.f);
            }
        }
    };
    e.volume_count = static_cast<u32>(m_volumes.size());
    for (u32 i = 0; i < e.volume_count; ++i) {
        const f32 alpha = look_volume_alpha(m_volumes[i].desc, input.camera_position);
        e.volume_alpha[i] = alpha;
        applyLayer(m_volumes[i].profile, alpha);
    }
    const u32 overrideCount = std::min<u32>(input.override_count, static_cast<u32>(m_overrides.size()));
    for (u32 i = 0; i < overrideCount; ++i) {
        applyLayer(m_overrides[i], std::clamp(input.override_weights[i], 0.f, 1.f));
    }

    look_clamp_params(v);
    e.resolved = look_resolve(v);
    e.lut_slot_count = slots;
    for (u32 k = 0; k < kMaxLookLuts; ++k) {
        e.lut_weights[k] = k < slots ? W[k] : 0.f;
    }
    ++e.frame;
    bake(e);
    return e;
}

void LookSystem::bake(const LookEvaluation& eval) {
    BakeKey key{};
    const bool gradeOn = eval.resolved.grade.enabled;
    key.grade = make_grade_params(eval.resolved);
    if (gradeOn) {
        for (u32 k = 0; k < kMaxLookLuts; ++k) {
            key.weights[k] = eval.lut_weights[k];
        }
        key.strength = eval.resolved.grade.lut_strength;
    } else {
        key.weights[0] = 1.f;
    }
    key.valid = 1;
    m_eval.lut_rebaked = false;
    if (m_bakeKey.valid != 0u && std::memcmp(&key, &m_bakeKey, sizeof(key)) == 0) {
        return;
    }
    m_bakeKey = key;

    kernels::LutBakeParams p{};
    const u32 n = m_config.lut_size;
    const u32 n3 = n * n * n;
    if (m_baked.data.size() != n3) {
        m_baked.data.resize(n3);
    }
    m_baked.size = n;
    p.dst = {m_baked.data.data(), n3};
    p.n = n;
    p.grade = key.grade;
    p.strength = key.strength;
    p.identity_weight = key.weights[0];
    // Compact the non-zero external slots (largest weights first when more than kMaxLutBlend).
    u32 order[kMaxLookLuts];
    u32 count = 0;
    for (u32 k = 1; k < m_luts.size() && k < kMaxLookLuts; ++k) {
        if (key.weights[k] > 0.f) {
            order[count++] = k;
        }
    }
    // Stable insertion sort by descending weight (std::stable_sort may allocate a temporary buffer).
    for (u32 i = 1; i < count; ++i) {
        const u32 v = order[i];
        u32 j = i;
        while (j > 0u && key.weights[order[j - 1u]] < key.weights[v]) {
            order[j] = order[j - 1u];
            --j;
        }
        order[j] = v;
    }
    if (count > kernels::kMaxLutBlend) {
        f32 dropped = 0.f;
        for (u32 i = kernels::kMaxLutBlend; i < count; ++i) {
            dropped += key.weights[order[i]];
        }
        p.identity_weight += dropped; // keep the sum at 1
        count = kernels::kMaxLutBlend;
    }
    for (u32 i = 0; i < count; ++i) {
        p.ext[i] = m_luts[order[i]].span();
        p.ext_weight[i] = key.weights[order[i]];
    }
    p.ext_count = count;
    if (count == 0u) {
        p.identity_weight = 1.f;
    }
    const kernel::KernelLaunch desc{kernels::LutBakeKernel::kName, kernel::extent1(n3), kernels::kLinearWorkgroup};
    kernel::launch(m_config.backend, desc, kernels::LutBakeKernel{}, p);
    m_eval.lut_rebaked = true;
}

} // namespace fuse::renderer::look
