#include <fuse/renderer/look/look_post_chain.hpp>

#include <fuse/compute_kernel/launch.hpp>
#include <fuse/profiler/profiler.hpp>
#include <fuse/renderer/look/look_kernels.hpp>
#include <fuse/renderer/postprocess/dof.hpp>
#include <fuse/renderer/postprocess/motion_blur.hpp>
#include <fuse/renderer/postprocess/post_stack.hpp>
#include <fuse/renderer/postprocess/tonemap.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::renderer::look {

using math::Vec2;
using math::Vec3;

namespace {

template <typename T>
kernel::Span<T> spanOf(std::vector<T>& v, size_t count) {
    return {v.data(), static_cast<u32>(count)};
}
template <typename T>
kernel::Span<const T> cspanOf(const std::vector<T>& v, size_t count) {
    return {v.data(), static_cast<u32>(count)};
}

} // namespace

const char* look_output_encoding_name(LookOutputEncoding encoding) {
    switch (encoding) {
    case LookOutputEncoding::Srgb:
        return "srgb";
    case LookOutputEncoding::Hdr10Pq:
        return "hdr10_pq";
    case LookOutputEncoding::ScRgb:
        return "scrgb";
    }
    return "unknown";
}

void generate_procedural_lens_dirt(u64 seed, u32 width, u32 height, std::vector<f32>& out) {
    out.assign(static_cast<size_t>(width) * height, 0.f);
    if (width == 0u || height == 0u) {
        return;
    }
    // A handful of soft Gaussian smudges with hashed position / radius / strength.
    constexpr u32 kSmudges = 24u;
    const f32 fw = static_cast<f32>(width);
    const f32 fh = static_cast<f32>(height);
    for (u32 s = 0; s < kSmudges; ++s) {
        const u64 h0 = kernels::splitmix64(seed ^ (0x51ED2701ull * (s + 1u)));
        const u64 h1 = kernels::splitmix64(h0);
        const f32 cx = static_cast<f32>(h0 & 0xffffu) / 65535.f * fw;
        const f32 cy = static_cast<f32>((h0 >> 16u) & 0xffffu) / 65535.f * fh;
        const f32 radius = (0.03f + 0.12f * static_cast<f32>((h0 >> 32u) & 0xffffu) / 65535.f) * std::max(fw, fh);
        const f32 strength = 0.25f + 0.75f * static_cast<f32>(h1 & 0xffffu) / 65535.f;
        const f32 inv2s2 = 1.f / (2.f * radius * radius * 0.25f);
        for (u32 y = 0; y < height; ++y) {
            for (u32 x = 0; x < width; ++x) {
                const f32 dx = static_cast<f32>(x) + 0.5f - cx;
                const f32 dy = static_cast<f32>(y) + 0.5f - cy;
                const f32 d2 = dx * dx + dy * dy;
                if (d2 > radius * radius * 4.f) {
                    continue;
                }
                f32& t = out[static_cast<size_t>(y) * width + x];
                t = std::min(t + strength * std::exp(-d2 * inv2s2), 1.f);
            }
        }
    }
    // Faint baseline so the whole lens scatters a little.
    for (f32& t : out) {
        t = std::min(0.08f + t, 1.f);
    }
}

bool LookPostChain::init(const LookChainConfig& config) {
    destroy();
    if (config.width == 0u || config.height == 0u) {
        return false;
    }
    m_config = config;
    const size_t count = static_cast<size_t>(config.width) * config.height;
    m_a.assign(count, Vec3{});
    m_b.assign(count, Vec3{});
    m_tmp.assign(count, Vec3{});
    m_upA.assign(count, Vec3{});
    m_upB.assign(count, Vec3{});
    m_bloom.assign(count, Vec3{});
    m_spatialScratch.reserve(count);
    // Pyramid extents (bloom_level_count semantics: halve until 1x1 or kLookBloomLevels).
    u32 w = config.width;
    u32 h = config.height;
    m_levels = 0;
    for (u32 l = 0; l < kLookBloomLevels; ++l) {
        if (l > 0u && m_levelW[l - 1u] == 1u && m_levelH[l - 1u] == 1u) {
            break;
        }
        m_levelW[l] = w;
        m_levelH[l] = h;
        m_down[l].assign(static_cast<size_t>(w) * h, Vec3{});
        ++m_levels;
        w = (w + 1u) / 2u;
        h = (h + 1u) / 2u;
    }
    const u32 fl = m_levels > 1u ? 1u : 0u;
    m_flare.assign(static_cast<size_t>(m_levelW[fl]) * m_levelH[fl], Vec3{});
    generate_procedural_lens_dirt(0x0D1A7u, std::max(config.width / 4u, 1u), std::max(config.height / 4u, 1u), m_dirt);
    m_dirtW = std::max(config.width / 4u, 1u);
    m_dirtH = std::max(config.height / 4u, 1u);
    m_autoExposure.init();
    m_ready = true;
    return true;
}

void LookPostChain::destroy() {
    m_a.clear();
    m_b.clear();
    m_tmp.clear();
    m_upA.clear();
    m_upB.clear();
    m_bloom.clear();
    for (auto& d : m_down) {
        d.clear();
    }
    m_flare.clear();
    m_levels = 0;
    m_ready = false;
    m_stats = {};
}

bool LookPostChain::setLensDirtTexture(const f32* texels, u32 width, u32 height) {
    if (texels == nullptr || width == 0u || height == 0u) {
        return false;
    }
    m_dirt.assign(texels, texels + static_cast<size_t>(width) * height);
    m_dirtW = width;
    m_dirtH = height;
    return true;
}

template <typename Kernel, typename Params>
void LookPostChain::run(const Params& params, kernel::Dim3 grid) {
    const kernel::KernelLaunch desc{Kernel::kName, grid,
                                    grid.y > 1u ? kernels::kImageWorkgroup : kernels::kLinearWorkgroup};
    kernel::launch(m_config.backend, desc, Kernel{}, params);
}

void LookPostChain::runBloom(const LookResolved& look) {
    const u32 W = m_config.width;
    const u32 H = m_config.height;
    const size_t count = static_cast<size_t>(W) * H;
    // Prefilter -> level 0.
    run<kernels::BloomPrefilterKernel>(
        kernels::BloomPrefilterParams{cspanOf(m_a, count), spanOf(m_down[0], count), look.bloom.threshold,
                                      look.bloom.knee},
        kernel::extent2(W, H));
    // Downsample chain: horizontal into m_tmp, vertical into the next level.
    for (u32 l = 1; l < m_levels; ++l) {
        const u32 sw = m_levelW[l - 1u];
        const u32 sh = m_levelH[l - 1u];
        const u32 hw = (sw + 1u) / 2u;
        kernels::BloomDownsampleParams ph{cspanOf(m_down[l - 1u], static_cast<size_t>(sw) * sh),
                                          spanOf(m_tmp, static_cast<size_t>(hw) * sh), sw, sh, hw, 1u};
        run<kernels::BloomDownsampleKernel>(ph, kernel::extent2(hw, sh));
        const u32 dw = m_levelW[l];
        const u32 dh = m_levelH[l];
        kernels::BloomDownsampleParams pv{cspanOf(m_tmp, static_cast<size_t>(hw) * sh),
                                          spanOf(m_down[l], static_cast<size_t>(dw) * dh), hw, sh, dw, 0u};
        run<kernels::BloomDownsampleKernel>(pv, kernel::extent2(dw, dh));
    }
    // Upsample + combine chain. `up` starts as the coarsest level.
    const f32 scatter = std::clamp(look.bloom.scatter, 0.f, 1.f);
    const std::vector<Vec3>* up = &m_down[m_levels - 1u];
    std::vector<Vec3>* targets[2] = {&m_upA, &m_upB};
    u32 flip = 0;
    for (u32 level = m_levels - 1u; level > 0u; --level) {
        const u32 dst = level - 1u;
        const u32 sw = m_levelW[level];
        const u32 sh = m_levelH[level];
        const u32 dw = m_levelW[dst];
        const u32 dh = m_levelH[dst];
        kernels::BloomUpsampleParams ph{cspanOf(*up, static_cast<size_t>(sw) * sh),
                                        spanOf(m_tmp, static_cast<size_t>(dw) * sh), {}, sw, sh, dw, 1u, scatter};
        run<kernels::BloomUpsampleKernel>(ph, kernel::extent2(dw, sh));
        std::vector<Vec3>& target = dst == 0u ? m_bloom : *targets[flip];
        flip ^= 1u;
        kernels::BloomUpsampleParams pv{cspanOf(m_tmp, static_cast<size_t>(dw) * sh),
                                        spanOf(target, static_cast<size_t>(dw) * dh),
                                        cspanOf(m_down[dst], static_cast<size_t>(dw) * dh),
                                        dw, sh, dw, 0u, scatter};
        run<kernels::BloomUpsampleKernel>(pv, kernel::extent2(dw, dh));
        up = &target;
    }
    if (m_levels == 1u) {
        std::copy(m_down[0].begin(), m_down[0].end(), m_bloom.begin());
    }
    m_bloomValid = true;
}

bool LookPostChain::process(const LookChainInput& input, const LookEffectGraph& graph, const LookResolved& look,
                            const Lut3D& grade_lut, Vec3* out) {
    FUSE_PROFILE_SCOPE("look_post_chain");
    m_stats = {};
    if (!m_ready || input.hdr == nullptr || out == nullptr || !graph.validate().ok()) {
        return false;
    }
    const u32 W = m_config.width;
    const u32 H = m_config.height;
    const size_t count = static_cast<size_t>(W) * H;
    const kernel::Dim3 grid = kernel::extent2(W, H);
    const bool hdrOut = m_config.encoding != LookOutputEncoding::Srgb;
    std::copy(input.hdr, input.hdr + count, m_a.begin());
    m_bloomValid = false;
    const auto record = [this](LookEffect e) { m_stats.executed[m_stats.executed_count++] = e; };
    const auto swapAB = [this]() { m_a.swap(m_b); };

    f32 peak = look.output.peak_nits;
    if (m_config.display_peak_nits > 0.f) {
        peak = std::min(peak, m_config.display_peak_nits);
    }
    const f32 paperWhite = std::min(look.output.paper_white_nits, peak);

    for (u32 n = 0; n < graph.size(); ++n) {
        const LookEffect effect = graph.at(n);
        switch (effect) {
        case LookEffect::AmbientOcclusion:
            break; // exported parameters; the screen-space passes run pre-upscale
        case LookEffect::DepthOfField:
            if (look.dof.enabled && input.linear_depth_m != nullptr) {
                DOFParams p{};
                p.focal_distance = look.dof.focus_distance_m;
                p.focal_length = look.dof.focal_length_mm;
                p.f_stop = look.dof.f_stop;
                p.bokeh_blades = look.dof.bokeh_blades;
                p.near_blur = look.dof.near_blur;
                p.max_coc_radius_px = look.dof.max_coc_radius_px;
                dof_pass(m_a.data(), input.linear_depth_m, W, H, p, m_spatialScratch);
                std::copy(m_spatialScratch.begin(), m_spatialScratch.end(), m_a.begin());
                record(effect);
            }
            break;
        case LookEffect::MotionBlur:
            if (look.motion_blur.enabled && input.velocity_px != nullptr) {
                MotionBlurParams p{};
                p.max_samples = std::max(look.motion_blur.max_samples, 1u);
                p.shutter_angle = look.motion_blur.shutter_angle;
                p.max_blur_px = look.motion_blur.max_blur_px;
                motion_blur_pass(m_a.data(), input.velocity_px, input.linear_depth_m, W, H, p, m_spatialScratch);
                std::copy(m_spatialScratch.begin(), m_spatialScratch.end(), m_a.begin());
                record(effect);
            }
            break;
        case LookEffect::Bloom:
            if (look.bloom.enabled) {
                runBloom(look);
                m_stats.bloom_levels = m_levels;
                run<kernels::BloomCompositeKernel>(
                    kernels::BloomCompositeParams{spanOf(m_a, count), cspanOf(m_bloom, count), look.bloom.intensity,
                                                  look.bloom.tint},
                    grid);
                record(effect);
            }
            break;
        case LookEffect::LensDirt:
            if (look.lens_dirt.enabled && m_bloomValid && !m_dirt.empty()) {
                run<kernels::LensDirtKernel>(kernels::LensDirtParams{spanOf(m_a, count), cspanOf(m_bloom, count),
                                                                     cspanOf(m_dirt, m_dirt.size()), W, H, m_dirtW,
                                                                     m_dirtH, look.lens_dirt.intensity,
                                                                     look.lens_dirt.tint},
                                             grid);
                record(effect);
            }
            break;
        case LookEffect::LensFlare:
            if (look.lens_flare.enabled && m_bloomValid && m_levels > 1u) {
                const u32 fw = m_levelW[1];
                const u32 fh = m_levelH[1];
                kernels::LensFlareParams fp{};
                fp.bright = cspanOf(m_down[1], static_cast<size_t>(fw) * fh);
                fp.dst = spanOf(m_flare, static_cast<size_t>(fw) * fh);
                fp.w = fw;
                fp.h = fh;
                fp.ghost_count = look.lens_flare.ghost_count;
                fp.ghost_spacing = look.lens_flare.ghost_spacing;
                fp.threshold = look.lens_flare.threshold;
                fp.halo_radius = look.lens_flare.halo_radius;
                fp.halo_thickness = std::max(look.lens_flare.halo_thickness, 1e-3f);
                fp.halo_intensity = look.lens_flare.halo_intensity;
                fp.chromatic_shift = look.lens_flare.chromatic_shift;
                run<kernels::LensFlareKernel>(fp, kernel::extent2(fw, fh));
                run<kernels::FlareCompositeKernel>(
                    kernels::FlareCompositeParams{spanOf(m_a, count), cspanOf(m_flare, static_cast<size_t>(fw) * fh), W,
                                                  H, fw, fh, look.lens_flare.intensity, look.lens_flare.tint},
                    grid);
                record(effect);
            }
            break;
        case LookEffect::Exposure: {
            AutoExposureParams ap = m_autoExposure.params();
            ap.enabled = look.exposure.auto_enabled;
            ap.min_ev = std::min(look.exposure.min_ev, look.exposure.max_ev);
            ap.max_ev = std::max(look.exposure.min_ev, look.exposure.max_ev);
            ap.adaptation_speed_up = look.exposure.adapt_speed_up;
            ap.adaptation_speed_down = look.exposure.adapt_speed_down;
            m_autoExposure.setParams(ap);
            f32 autoEv = 0.f;
            if (ap.enabled) {
                // Arithmetic mean luminance over a 4x4-strided subset (deterministic, allocation-free).
                f64 sum = 0.0;
                u32 samples = 0;
                for (u32 y = 0; y < H; y += 4u) {
                    for (u32 x = 0; x < W; x += 4u) {
                        sum += kernels::luminance709(m_a[static_cast<size_t>(y) * W + x]);
                        ++samples;
                    }
                }
                autoEv = m_autoExposure.updateFromLuminance(static_cast<f32>(sum / samples), input.delta_seconds);
            }
            f32 ev = look.exposure.bias_ev - autoEv;
            if (!hdrOut && look.tonemap.calibrate_mid_grey) {
                ev += tone_mapper_mid_grey_calibration_ev(static_cast<ToneMapper>(look.tonemap.op));
            }
            m_stats.exposure_ev = ev;
            m_stats.auto_exposure_ev = autoEv;
            run<kernels::ExposureKernel>(kernels::ExposureParams{spanOf(m_a, count), std::exp2(ev)}, grid);
            record(effect);
            break;
        }
        case LookEffect::ToneMap: {
            const f32 lmax = hdrOut ? std::max(peak / paperWhite, 1.f) : 0.f;
            m_stats.hdr_lmax = lmax;
            run<kernels::ToneMapKernel>(
                kernels::ToneMapParams{spanOf(m_a, count), static_cast<u32>(look.tonemap.op), lmax}, grid);
            record(effect);
            break;
        }
        case LookEffect::ColorGrade:
            if (look.grade.enabled && grade_lut.valid()) {
                run<kernels::GradeLutKernel>(kernels::GradeLutParams{spanOf(m_a, count), grade_lut.span(), grade_lut.size},
                                             grid);
                record(effect);
            }
            break;
        case LookEffect::Sharpen:
            if (look.sharpen.enabled && look.sharpen.sharpness > 0.f) {
                bool done = false;
                if (m_sharpenHook != nullptr) {
                    done = m_sharpenHook(m_a.data(), m_b.data(), W, H, look.sharpen.sharpness, m_sharpenUser);
                    m_stats.sharpen_hook_used = done;
                }
                if (!done) {
                    run<kernels::SharpenKernel>(
                        kernels::SharpenParams{cspanOf(m_a, count), spanOf(m_b, count), W, H, look.sharpen.sharpness},
                        grid);
                }
                swapAB();
                record(effect);
            }
            break;
        case LookEffect::ChromaticAberration:
            if (look.chromatic_aberration.enabled && look.chromatic_aberration.intensity > 0.f) {
                run<kernels::ChromaticAberrationKernel>(
                    kernels::ChromaticAberrationParams{cspanOf(m_a, count), spanOf(m_b, count), W, H,
                                                       look.chromatic_aberration.intensity},
                    grid);
                swapAB();
                record(effect);
            }
            break;
        case LookEffect::Vignette:
            if (look.vignette.enabled && look.vignette.intensity > 0.f) {
                run<kernels::VignetteKernel>(kernels::VignetteParams{spanOf(m_a, count), W, H, look.vignette.intensity,
                                                                     look.vignette.falloff, look.vignette.roundness,
                                                                     look.vignette.color},
                                             grid);
                record(effect);
            }
            break;
        case LookEffect::FilmGrain:
            if (look.film_grain.enabled && look.film_grain.intensity > 0.f && input.frame_seed != 0u) {
                run<kernels::FilmGrainKernel>(kernels::FilmGrainParams{spanOf(m_a, count), input.frame_seed,
                                                                       look.film_grain.intensity,
                                                                       look.film_grain.response},
                                              grid);
                record(effect);
            }
            break;
        case LookEffect::OutputTransform: {
            u32 mode = kernels::kEncodeSrgb;
            if (m_config.encoding == LookOutputEncoding::Hdr10Pq) {
                mode = kernels::kEncodeHdr10Pq;
            } else if (m_config.encoding == LookOutputEncoding::ScRgb) {
                mode = kernels::kEncodeScRgb;
            }
            run<kernels::OutputEncodeKernel>(
                kernels::OutputEncodeParams{cspanOf(m_a, count), {out, static_cast<u32>(count)}, mode, paperWhite, peak},
                grid);
            record(effect);
            break;
        }
        case LookEffect::Count:
            break;
        }
    }
    return true;
}

void apply_look_to_post_stack(const LookResolved& look, PostStack& stack) {
    BloomParams bloom = stack.bloom().params();
    bloom.threshold = look.bloom.threshold;
    bloom.knee = look.bloom.knee;
    bloom.intensity = look.bloom.enabled ? look.bloom.intensity : 0.f;
    bloom.scatter = look.bloom.scatter;
    stack.setBloomParams(bloom);

    DOFParams dof = stack.dofParams();
    dof.enabled = look.dof.enabled;
    dof.focal_distance = look.dof.focus_distance_m;
    dof.focal_length = look.dof.focal_length_mm;
    dof.f_stop = look.dof.f_stop;
    dof.bokeh_blades = look.dof.bokeh_blades;
    dof.near_blur = look.dof.near_blur;
    dof.max_coc_radius_px = look.dof.max_coc_radius_px;
    stack.setDofParams(dof);

    MotionBlurParams mb = stack.motionBlurParams();
    mb.enabled = look.motion_blur.enabled;
    mb.shutter_angle = look.motion_blur.shutter_angle;
    mb.max_samples = std::max(look.motion_blur.max_samples, 1u);
    mb.max_blur_px = look.motion_blur.max_blur_px;
    stack.setMotionBlurParams(mb);

    AutoExposureParams ae = stack.autoExposure().params();
    ae.enabled = look.exposure.auto_enabled;
    ae.min_ev = std::min(look.exposure.min_ev, look.exposure.max_ev);
    ae.max_ev = std::max(look.exposure.min_ev, look.exposure.max_ev);
    ae.adaptation_speed_up = look.exposure.adapt_speed_up;
    ae.adaptation_speed_down = look.exposure.adapt_speed_down;
    stack.setAutoExposureParams(ae);

    ColorGradeParams grade = stack.colorGrade().params();
    grade.tone_mapper = static_cast<ToneMapper>(look.tonemap.op);
    grade.calibrate_mid_grey = look.tonemap.calibrate_mid_grey;
    grade.exposure = look.exposure.bias_ev;
    if (look.grade.enabled) {
        grade.lift = look.grade.lift;
        grade.gamma = look.grade.gamma;
        grade.gain = look.grade.gain;
        grade.saturation = look.grade.saturation;
        grade.contrast = look.grade.contrast;
    } else {
        grade.lift = {};
        grade.gamma = {1.f, 1.f, 1.f};
        grade.gain = {1.f, 1.f, 1.f};
        grade.saturation = 1.f;
        grade.contrast = 1.f;
    }
    grade.vignette = look.vignette.enabled ? look.vignette.intensity : 0.f;
    grade.film_grain = look.film_grain.enabled ? look.film_grain.intensity : 0.f;
    stack.setColorGradeParams(grade);
}

} // namespace fuse::renderer::look
