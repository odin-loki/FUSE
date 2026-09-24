// WP-4.2 CPU gates (stub-safe): the clean-room FSR 3.1 host against the vendored SDK helpers and the SDK's own
// expressions, the embedded passes' reflected layouts against the C++ records, and the jitter / depth / motion
// conventions. Lavapipe gates: test_rp_fsr3.cpp.
//
//   layout    reflection of every embedded pass: cbFSR3Upscaler / cbSPD / cbRCAS member offsets and sizes ==
//             Fsr3UpscalerConstants / Fsr3SpdConstants / Fsr3RcasConstants (offsetof / sizeof), the convert
//             pass's push block (Slang and GLSL) == Fsr3ConvertPush, workgroup sizes
//   bindings  every reflected binding of every pass resolves by name (the SDK's binding tables) to a resource of
//             the reflected kind, set 0, no duplicate binding numbers; each pass binds its output
//   host      fsr3_setup_frame: RCAS / SPD constants bit-identical to the vendored FsrRcasCon / ffxSpdSetup, jitter
//             history, frame index / reset / first execution, ping-pong parity (mod 16), jitter-phase stepping,
//             pre-exposure, motion-vector scale, downscale factor, tan(half horizontal fov), dispatch sizes,
//             invalid input rejected with the state untouched
//   jitter    sign conventions: FSR Jitter() = -jitter_px puts the FSR sample position (upsample.h) exactly on
//             the FUSE sample position; projection_jitter_ndc == temporal::jitter_view_proj and moves points by
//             -jitter (the sample convention); SDK Halton offsets == upscaleJitterOffset; phase counts
//   convert   CPU twin of fsr3.convert: reverse-Z device depth -> the SDK's inverted transform returns the linear
//             depth (rel 2e-6), sky -> far plane, monotonic
#include <fuse/renderer/temporal/temporal_types.hpp>
#include <fuse/renderer/upscale/upscale_inputs.hpp>
#include <fuse/renderer/upscale_backends/fsr3/fsr3_gpu.hpp>
#include <fuse/renderer/upscale_backends/fsr3/fsr3_reflect.hpp>
#include <fuse/renderer/upscale_backends/fsr3/fsr3_types.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#define FFX_CPU 1
#include <FidelityFX/gpu/ffx_core.h>
#include <FidelityFX/gpu/fsr1/ffx_fsr1.h>
#include <FidelityFX/gpu/spd/ffx_spd.h>

namespace {

using namespace fuse::renderer;
using namespace fuse::renderer::fsr3;
using fuse::f32;
using fuse::f64;
using fuse::i32;
using fuse::u32;
using fuse::u64;
using fuse::usize;
namespace math = fuse::math;

constexpr int kSkip = 77;
int g_failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

bool reflectPass(Fsr3Pass pass, Fsr3KernelLanguage language, SpirvReflection& out) {
    const Fsr3KernelCode c = fsr3_kernel_code(pass, language);
    return c.words != nullptr && reflect_spirv(c.words, c.bytes / 4u, out);
}

const SpirvBinding* findBinding(const SpirvReflection& r, const char* name) {
    for (const SpirvBinding& b : r.bindings) {
        if (b.name == name) {
            return &b;
        }
    }
    return nullptr;
}

struct Expected {
    u32 offset;
    u32 size;
};

bool checkMembers(const std::vector<SpirvBlockMember>& members, const Expected* expected, usize count, const char* what) {
    bool ok = members.size() == count;
    for (usize i = 0; ok && i < count; ++i) {
        ok = members[i].offset == expected[i].offset && members[i].size == expected[i].size;
        if (!ok) {
            std::fprintf(stderr, "  %s member %zu (%s): offset %u size %u, expected %u / %u\n", what, i, members[i].name.c_str(),
                         members[i].offset, members[i].size, expected[i].offset, expected[i].size);
        }
    }
    if (members.size() != count) {
        std::fprintf(stderr, "  %s: %zu members, expected %zu\n", what, members.size(), count);
    }
    return ok;
}

#define FSR3_MEMBER(field) {static_cast<u32>(offsetof(Fsr3UpscalerConstants, field)), static_cast<u32>(sizeof(Fsr3UpscalerConstants::field))}

int runLayout() {
    const Expected mainBlock[] = {
        FSR3_MEMBER(renderSize), FSR3_MEMBER(previousFrameRenderSize), FSR3_MEMBER(upscaleSize),
        FSR3_MEMBER(previousFrameUpscaleSize), FSR3_MEMBER(maxRenderSize), FSR3_MEMBER(maxUpscaleSize),
        FSR3_MEMBER(deviceToViewDepth), FSR3_MEMBER(jitterOffset), FSR3_MEMBER(previousFrameJitterOffset),
        FSR3_MEMBER(motionVectorScale), FSR3_MEMBER(downscaleFactor), FSR3_MEMBER(motionVectorJitterCancellation),
        FSR3_MEMBER(tanHalfFOV), FSR3_MEMBER(jitterPhaseCount), FSR3_MEMBER(deltaTime), FSR3_MEMBER(deltaPreExposure),
        FSR3_MEMBER(viewSpaceToMetersFactor), FSR3_MEMBER(frameIndex), FSR3_MEMBER(velocityFactor),
        FSR3_MEMBER(reactivenessScale), FSR3_MEMBER(shadingChangeScale), FSR3_MEMBER(accumulationAddedPerFrame),
        FSR3_MEMBER(minDisocclusionAccumulation)};
    const Expected spdBlock[] = {{0, 4}, {4, 4}, {8, 8}, {16, 8}};
    const Expected rcasBlock[] = {{0, 16}};
    const Expected pushBlock[] = {{0, 4}, {4, 4}, {8, 4}, {12, 4}, {16, 4}, {20, 4}, {24, 4}, {28, 4}};
    u32 passes = 0, mainBlocks = 0, spdBlocks = 0, rcasBlocks = 0;
    for (u32 p = 1; p < kFsr3PassCount; ++p) {
        SpirvReflection r;
        if (!reflectPass(static_cast<Fsr3Pass>(p), Fsr3KernelLanguage::Auto, r)) {
            std::printf("SKIP: FSR 3.1 passes not built (glslangValidator missing)\n");
            return kSkip;
        }
        ++passes;
        for (const SpirvBinding& b : r.bindings) {
            if (b.name == "cbFSR3Upscaler") {
                ++mainBlocks;
                expect(checkMembers(b.members, mainBlock, std::size(mainBlock), "cbFSR3Upscaler"),
                       "cbFSR3Upscaler == Fsr3UpscalerConstants");
            } else if (b.name == "cbSPD") {
                ++spdBlocks;
                expect(checkMembers(b.members, spdBlock, std::size(spdBlock), "cbSPD"), "cbSPD == Fsr3SpdConstants");
            } else if (b.name == "cbRCAS") {
                ++rcasBlocks;
                expect(checkMembers(b.members, rcasBlock, std::size(rcasBlock), "cbRCAS"), "cbRCAS == Fsr3RcasConstants");
            }
        }
        std::printf("  %-28s workgroup %ux%ux%u, %zu bindings%s\n", fsr3_pass_name(static_cast<Fsr3Pass>(p)), r.localSize[0],
                    r.localSize[1], r.localSize[2], r.bindings.size(),
                    findBinding(r, "cbFSR3Upscaler") != nullptr ? "" : " (no cbFSR3Upscaler)");
    }
    static_assert(offsetof(Fsr3SpdConstants, workGroupOffset) == 8u && offsetof(Fsr3SpdConstants, renderSize) == 16u, "cbSPD");
    expect(mainBlocks + 1u >= passes, "every pass but RCAS reads cbFSR3Upscaler (-Os strips unused blocks)");
    expect(spdBlocks == 2u, "the two SPD pyramids read cbSPD");
    expect(rcasBlocks == 1u, "RCAS reads cbRCAS");
    u32 languages = 0;
    for (const Fsr3KernelLanguage lang : {Fsr3KernelLanguage::Slang, Fsr3KernelLanguage::Glsl}) {
        SpirvReflection r;
        if (!reflectPass(Fsr3Pass::Convert, lang, r)) {
            continue;
        }
        ++languages;
        const char* label = lang == Fsr3KernelLanguage::Slang ? "convert push (slang)" : "convert push (glsl)";
        expect(checkMembers(r.pushConstants, pushBlock, std::size(pushBlock), label), "Fsr3ConvertPush == push block");
        expect(r.pushConstantBytes == sizeof(Fsr3ConvertPush), "push block size");
        expect(r.localSize[0] == 8u && r.localSize[1] == 8u, "convert workgroup 8x8");
        std::printf("  %-28s %s, %zu bindings, push %u bytes\n", "fsr3.convert", lang == Fsr3KernelLanguage::Slang ? "slang" : "glsl",
                    r.bindings.size(), r.pushConstantBytes);
    }
    expect(languages >= 1u, "at least one convert kernel built");
    std::printf("layout: %u vendored passes + %u convert kernels checked\n", passes, languages);
    return 0;
}

int runBindings() {
    // The SDK's per-pass binding lists (the FSR3UPSCALER_BIND_* defines of each vendored pass file). -Os strips
    // bindings a permutation never reads, so the reflected set must be a subset, and the outputs must be present.
    struct PassNames {
        Fsr3Pass pass;
        std::vector<const char*> allowed;
        std::vector<const char*> required;
    };
    const std::vector<PassNames> table = {
        {Fsr3Pass::PrepareInputs,
         {"r_input_motion_vectors", "r_input_depth", "r_input_color_jittered", "rw_dilated_motion_vectors", "rw_dilated_depth",
          "rw_reconstructed_previous_nearest_depth", "rw_farthest_depth", "rw_current_luma", "cbFSR3Upscaler", "s_PointClamp",
          "s_LinearClamp"},
         {"rw_dilated_motion_vectors", "rw_dilated_depth", "rw_reconstructed_previous_nearest_depth", "rw_current_luma"}},
        {Fsr3Pass::LumaPyramid,
         {"r_current_luma", "r_farthest_depth", "rw_spd_global_atomic", "rw_frame_info", "rw_spd_mip0", "rw_spd_mip1",
          "rw_spd_mip2", "rw_spd_mip3", "rw_spd_mip4", "rw_spd_mip5", "rw_farthest_depth_mip1", "cbFSR3Upscaler", "cbSPD",
          "s_PointClamp", "s_LinearClamp"},
         {"rw_frame_info", "rw_spd_global_atomic", "rw_farthest_depth_mip1"}},
        {Fsr3Pass::ShadingChangePyramid,
         {"r_current_luma", "r_previous_luma", "r_dilated_motion_vectors", "r_input_exposure", "rw_spd_global_atomic",
          "rw_spd_mip0", "rw_spd_mip1", "rw_spd_mip2", "rw_spd_mip3", "rw_spd_mip4", "rw_spd_mip5", "cbFSR3Upscaler", "cbSPD",
          "s_PointClamp", "s_LinearClamp"},
         {"rw_spd_global_atomic"}},
        {Fsr3Pass::ShadingChange, {"r_spd_mips", "rw_shading_change", "cbFSR3Upscaler", "s_PointClamp", "s_LinearClamp"},
         {"r_spd_mips", "rw_shading_change"}},
        {Fsr3Pass::PrepareReactivity,
         {"r_reconstructed_previous_nearest_depth", "r_dilated_motion_vectors", "r_dilated_depth", "r_reactive_mask",
          "r_transparency_and_composition_mask", "r_accumulation", "r_shading_change", "r_current_luma", "r_input_exposure",
          "rw_dilated_reactive_masks", "rw_new_locks", "rw_accumulation", "cbFSR3Upscaler", "s_PointClamp", "s_LinearClamp"},
         {"rw_dilated_reactive_masks", "rw_accumulation"}},
        {Fsr3Pass::LumaInstability,
         {"r_input_exposure", "r_dilated_reactive_masks", "r_dilated_motion_vectors", "r_frame_info", "r_luma_history",
          "r_farthest_depth_mip1", "r_current_luma", "rw_luma_history", "rw_luma_instability", "cbFSR3Upscaler", "s_PointClamp",
          "s_LinearClamp"},
         {"rw_luma_history", "rw_luma_instability"}},
        {Fsr3Pass::Accumulate,
         {"r_input_exposure", "r_dilated_reactive_masks", "r_dilated_motion_vectors", "r_internal_upscaled_color", "r_lanczos_lut",
          "r_farthest_depth_mip1", "r_current_luma", "r_luma_instability", "r_input_color_jittered", "rw_internal_upscaled_color",
          "rw_upscaled_output", "rw_new_locks", "cbFSR3Upscaler", "s_PointClamp", "s_LinearClamp"},
         {"rw_internal_upscaled_color", "rw_upscaled_output", "r_internal_upscaled_color", "r_input_color_jittered"}},
        {Fsr3Pass::AccumulateSharpen,
         {"r_input_exposure", "r_dilated_reactive_masks", "r_dilated_motion_vectors", "r_internal_upscaled_color", "r_lanczos_lut",
          "r_farthest_depth_mip1", "r_current_luma", "r_luma_instability", "r_input_color_jittered", "rw_internal_upscaled_color",
          "rw_upscaled_output", "rw_new_locks", "cbFSR3Upscaler", "s_PointClamp", "s_LinearClamp"},
         {"rw_internal_upscaled_color", "r_internal_upscaled_color"}},
        {Fsr3Pass::Rcas, {"r_input_exposure", "r_rcas_input", "rw_upscaled_output", "cbFSR3Upscaler", "cbRCAS", "s_PointClamp", "s_LinearClamp"},
         {"r_rcas_input", "rw_upscaled_output", "cbRCAS"}},
        {Fsr3Pass::Convert,
         {"fsr3SourceDepth", "fsr3SourceMotion", "fsr3OutDepth", "fsr3OutMotion", "fsr3OutExposure"},
         {"fsr3SourceDepth", "fsr3SourceMotion", "fsr3OutDepth", "fsr3OutMotion", "fsr3OutExposure"}},
    };
    u32 checked = 0;
    for (const PassNames& pn : table) {
        for (const Fsr3KernelLanguage lang : {Fsr3KernelLanguage::Slang, Fsr3KernelLanguage::Glsl}) {
            if (pn.pass != Fsr3Pass::Convert && lang == Fsr3KernelLanguage::Slang) {
                continue; // the vendored passes exist as GLSL only
            }
            SpirvReflection r;
            if (!reflectPass(pn.pass, lang, r)) {
                if (pn.pass != Fsr3Pass::Convert) {
                    std::printf("SKIP: FSR 3.1 passes not built\n");
                    return kSkip;
                }
                continue;
            }
            ++checked;
            std::set<u32> numbers;
            for (const SpirvBinding& b : r.bindings) {
                Fsr3BindingTarget t{};
                const bool resolved = fsr3_resolve_binding(b.name.c_str(), t);
                if (!resolved) {
                    std::fprintf(stderr, "  %s: unknown binding %s\n", fsr3_pass_name(pn.pass), b.name.c_str());
                }
                expect(resolved, "binding name resolves (SDK binding tables)");
                const bool kindOk = (t.kind == Fsr3BindingKind::Sampled && b.kind == SpirvDescriptorKind::SampledImage) ||
                                    (t.kind == Fsr3BindingKind::Storage && b.kind == SpirvDescriptorKind::StorageImage) ||
                                    (t.kind == Fsr3BindingKind::Uniform && b.kind == SpirvDescriptorKind::UniformBuffer) ||
                                    (t.kind == Fsr3BindingKind::Sampler && b.kind == SpirvDescriptorKind::Sampler) ||
                                    (t.kind == Fsr3BindingKind::StorageBuffer && b.kind == SpirvDescriptorKind::StorageBuffer);
                if (resolved && !kindOk) {
                    std::fprintf(stderr, "  %s: %s has the wrong descriptor kind\n", fsr3_pass_name(pn.pass), b.name.c_str());
                }
                expect(!resolved || kindOk, "reflected descriptor kind == the table's");
                expect(b.set == 0u, "set 0");
                expect(numbers.insert(b.binding).second, "unique binding numbers");
                bool allowed = false;
                for (const char* a : pn.allowed) {
                    allowed = allowed || b.name == a;
                }
                if (!allowed) {
                    std::fprintf(stderr, "  %s: unexpected binding %s\n", fsr3_pass_name(pn.pass), b.name.c_str());
                }
                expect(allowed, "binding belongs to the pass's SDK binding list");
            }
            for (const char* req : pn.required) {
                if (findBinding(r, req) == nullptr) {
                    std::fprintf(stderr, "  %s: missing %s\n", fsr3_pass_name(pn.pass), req);
                }
                expect(findBinding(r, req) != nullptr, "required binding present");
            }
        }
    }
    std::printf("bindings: %u pass kernels resolved by name\n", checked);
    return 0;
}

Fsr3FrameParams params(u32 rw, u32 rh, u32 dw, u32 dh) {
    Fsr3FrameParams p{};
    p.resolution.render_width = rw;
    p.resolution.render_height = rh;
    p.resolution.display_width = dw;
    p.resolution.display_height = dh;
    p.near_plane = 0.1f;
    p.far_plane = 200.f;
    p.vertical_fov_rad = 1.0f;
    p.frame_time_s = 1.f / 60.f;
    return p;
}

int runHost() {
    Fsr3Settings settings{};
    Fsr3HostState st{};
    fsr3_init_state(st, settings, 128, 72, 192, 108);
    expect(st.firstExecution && st.constants.maxRenderSize[0] == 128 && st.constants.maxUpscaleSize[1] == 108 &&
               st.constants.accumulationAddedPerFrame == 1.f / 3.f && st.constants.minDisocclusionAccumulation == -1.f / 3.f,
           "fsr3upscalerCreate defaults");
    Fsr3FrameSetup s{};
    // Invalid inputs leave the state untouched.
    const Fsr3HostState before = st;
    Fsr3FrameParams bad = params(256, 72, 192, 108); // above max render
    expect(!fsr3_setup_frame(st, settings, bad, s), "render above max rejected");
    bad = params(128, 72, 192, 108);
    bad.far_plane = 0.05f;
    expect(!fsr3_setup_frame(st, settings, bad, s), "far <= near rejected");
    expect(std::memcmp(&before.constants, &st.constants, sizeof(st.constants)) == 0 && st.firstExecution, "state untouched");

    math::Vec2 prevJitter(0.f, 0.f);
    u32 frames = 0;
    for (u32 f = 0; f < 40u; ++f) {
        Fsr3FrameParams p = params(128, 72, 192, 108);
        p.jitter_px = upscaleJitterOffset(f, 18u);
        p.reset = f == 25u;
        p.pre_exposure = f < 30u ? 1.f : 2.f;
        p.sharpness = 0.25f;
        const u32 parityBefore = st.resourceFrameIndex;
        expect(fsr3_setup_frame(st, settings, p, s), "setup");
        ++frames;
        const Fsr3UpscalerConstants& c = s.constants;
        expect(s.oddFrame == ((parityBefore & 1u) != 0u), "ping-pong parity = resourceFrameIndex & 1");
        expect(st.resourceFrameIndex == (parityBefore + 1u) % 16u, "resourceFrameIndex mod 16");
        expect(s.firstExecution == (f == 0u), "firstExecution on the first dispatch only");
        expect(s.resetAccumulation == (f == 0u || f == 25u), "reset on first dispatch and on request");
        expect(c.frameIndex == static_cast<f32>(f >= 25u ? f - 25u : f), "frameIndex: 0 on reset, +1 per frame");
        expect(c.jitterOffset[0] == -p.jitter_px.x && c.jitterOffset[1] == -p.jitter_px.y, "FSR jitter = -FUSE jitter");
        expect(c.previousFrameJitterOffset[0] == -prevJitter.x && c.previousFrameJitterOffset[1] == -prevJitter.y,
               "previous-frame jitter");
        prevJitter = p.jitter_px;
        expect(c.motionVectorScale[0] == -1.f && c.motionVectorScale[1] == -1.f, "motion scale -1 (FUSE cur - prev -> FSR prev - cur)");
        expect(c.motionVectorJitterCancellation[0] == 0.f && c.motionVectorJitterCancellation[1] == 0.f, "no jitter cancellation");
        expect(c.downscaleFactor[0] == 128.f / 192.f && c.downscaleFactor[1] == 72.f / 108.f, "downscale factor");
        expect(c.renderSize[0] == 128 && c.upscaleSize[1] == 108, "sizes");
        expect(c.deltaPreExposure == (f == 30u ? 2.f : 1.f), "delta pre-exposure = pre / previous pre");
        expect(c.jitterPhaseCount == 18.f, "jitter phase count int(8 * 1.5^2) = 18");
        expect(std::fabs(c.tanHalfFOV - std::tan(0.5f) * (128.f / 72.f)) < 1e-5f, "tan(half horizontal fov)");
        expect(c.deltaTime == 1.f / 60.f, "delta time (s)");
        // Vendored helpers, bit for bit.
        FfxUInt32x4 rcas = {0, 0, 0, 0};
        FsrRcasCon(rcas, -2.f * 0.25f + 2.f);
        expect(std::memcmp(rcas, s.rcas.rcasConfig, sizeof(rcas)) == 0, "RCAS constants == FsrRcasCon");
        FfxUInt32x2 groups = {0, 0}, offset = {0, 0}, wm = {0, 0};
        FfxUInt32x4 rect = {0, 0, 128, 72};
        ffxSpdSetup(groups, offset, wm, rect);
        expect(s.spd.numWorkGroups == wm[0] && s.spd.mips == wm[1] && s.spd.workGroupOffset[0] == offset[0] &&
                   s.groups[static_cast<u32>(Fsr3Pass::LumaPyramid)][0] == groups[0] &&
                   s.groups[static_cast<u32>(Fsr3Pass::LumaPyramid)][1] == groups[1],
               "SPD constants == ffxSpdSetup");
        expect(s.groups[static_cast<u32>(Fsr3Pass::PrepareInputs)][0] == 16u && s.groups[static_cast<u32>(Fsr3Pass::PrepareInputs)][1] == 9u &&
                   s.groups[static_cast<u32>(Fsr3Pass::Accumulate)][0] == 24u && s.groups[static_cast<u32>(Fsr3Pass::Accumulate)][1] == 14u &&
                   s.groups[static_cast<u32>(Fsr3Pass::ShadingChange)][0] == 8u &&
                   s.groups[static_cast<u32>(Fsr3Pass::ShadingChange)][1] == 5u && s.groups[static_cast<u32>(Fsr3Pass::Rcas)][0] == 12u,
               "dispatch sizes (8x8 tiles, shading change at half res, RCAS 16x16)");
    }
    // Dynamic resolution: the jitter phase count moves by one per frame towards the new ratio.
    Fsr3FrameParams p = params(96, 54, 192, 108); // 2x -> int(8 * 4) = 32
    expect(fsr3_setup_frame(st, settings, p, s) && s.constants.jitterPhaseCount == 19.f, "phase count steps +1");
    expect(s.constants.previousFrameRenderSize[0] == 128 && s.constants.renderSize[0] == 96, "previous render size");
    for (u32 i = 0; i < 20u; ++i) {
        fsr3_setup_frame(st, settings, p, s);
    }
    expect(s.constants.jitterPhaseCount == 32.f, "phase count converges to int(8 * 2^2)");
    p.reset = true;
    p = params(128, 72, 192, 108);
    p.reset = true;
    expect(fsr3_setup_frame(st, settings, p, s) && s.constants.jitterPhaseCount == 18.f, "reset re-initialises the phase count");
    // Settings flow into the constants.
    settings.velocity_factor = 0.5f;
    settings.reactiveness_scale = 2.f;
    expect(fsr3_setup_frame(st, settings, params(128, 72, 192, 108), s) && s.constants.velocityFactor == 0.5f &&
               s.constants.reactivenessScale == 2.f,
           "ffxFsr3UpscalerSetConstant tunables");
    std::printf("host: %u frames of the SDK constant setup checked\n", frames + 23u);
    return 0;
}

// Pixel x of a clip-space point (Vulkan: NDC y down, pixel = (ndc + 1) / 2 * size).
void toPixel(const f32 m[16], const f64 p[3], u32 w, u32 h, f64 out[2]) {
    f64 c[4];
    for (u32 r = 0; r < 4u; ++r) {
        c[r] = m[0 * 4 + r] * p[0] + m[1 * 4 + r] * p[1] + m[2 * 4 + r] * p[2] + m[3 * 4 + r];
    }
    out[0] = (c[0] / c[3] * 0.5 + 0.5) * w;
    out[1] = (c[1] / c[3] * 0.5 + 0.5) * h;
}

int runJitter() {
    const u32 w = 160, h = 90;
    // A Vulkan perspective (column-major, NDC y down) looking down -Z.
    f32 proj[16] = {};
    const f32 f = 1.f / std::tan(0.5f);
    proj[0] = f / (static_cast<f32>(w) / static_cast<f32>(h));
    proj[5] = -f;
    proj[10] = 100.f / (0.1f - 100.f);
    proj[11] = -1.f;
    proj[14] = 0.1f * 100.f / (0.1f - 100.f);
    f64 worstMine = 0.0, worstTemporal = 0.0;
    f64 renderer[2] = {0.0, 0.0};
    for (u32 i = 0; i < 32u; ++i) {
        const math::Vec2 j = upscaleJitterOffset(i, 32u);
        // projection_jitter_ndc as a translation: clip.xy += ndc * clip.w.
        const math::Vec2 ndc = projection_jitter_ndc(j, w, h);
        f32 mine[16];
        std::memcpy(mine, proj, sizeof(mine));
        for (u32 c = 0; c < 4u; ++c) {
            mine[c * 4 + 0] += ndc.x * proj[c * 4 + 3];
            mine[c * 4 + 1] += ndc.y * proj[c * 4 + 3];
        }
        f32 wp[16];
        temporal::jitter_view_proj(proj, j.x, j.y, w, h, wp);
        // The renderer's upscaleJitterNdc through jitterProjection (the open WP-4.1 question).
        const math::Vec2 rn = upscaleJitterNdc(j, w, h);
        f32 rp[16];
        std::memcpy(rp, proj, sizeof(rp));
        for (u32 c = 0; c < 4u; ++c) {
            rp[c * 4 + 0] += rn.x * proj[c * 4 + 3];
            rp[c * 4 + 1] += rn.y * proj[c * 4 + 3];
        }
        for (u32 k = 0; k < 8u; ++k) {
            const f64 pt[3] = {-2.0 + 0.5 * k, 1.0 - 0.3 * k, -3.0 - 2.0 * k};
            f64 base[2], a[2], b[2], r[2];
            toPixel(proj, pt, w, h, base);
            toPixel(mine, pt, w, h, a);
            toPixel(wp, pt, w, h, b);
            toPixel(rp, pt, w, h, r);
            // Sample convention: the pixel centre c sees the unjittered point c + j, i.e. points move by -j.
            worstMine = std::max({worstMine, std::fabs(a[0] - (base[0] - j.x)), std::fabs(a[1] - (base[1] - j.y))});
            worstTemporal = std::max({worstTemporal, std::fabs(b[0] - a[0]), std::fabs(b[1] - a[1])});
            renderer[0] = std::max(renderer[0], std::fabs(r[0] - (base[0] - j.x)));
            renderer[1] = std::max(renderer[1], std::fabs(r[1] - (base[1] - j.y)));
        }
        // FSR shader positions with Jitter() = fsr3_jitter_offset(j):
        //   upsample.h   fSrcUnjitteredPos = iPx + 0.5 - Jitter()  must equal the FUSE sample iPx + 0.5 + j
        //   luma_instability.h  uvJittered = uv + Jitter() / size    must be where the unjittered uv lands (uv - j / size)
        const math::Vec2 J = fsr3_jitter_offset(j);
        for (u32 px = 0; px < 4u; ++px) {
            const f32 fsrSample = static_cast<f32>(px) + 0.5f - J.x;
            const f32 fuseSample = static_cast<f32>(px) + 0.5f + j.x;
            expect(fsrSample == fuseSample, "FSR sample position == FUSE sample position");
        }
        const f32 uv = 0.37f;
        expect(std::fabs((uv + J.y / static_cast<f32>(h)) - (uv - j.y / static_cast<f32>(h))) < 1e-7f,
               "FSR jittered uv == where the unjittered point lands");
    }
    std::printf("jitter: projection_jitter_ndc moves points by -jitter (max err %.2e px), == temporal::jitter_view_proj "
                "(max %.2e px)\n",
                worstMine, worstTemporal);
    std::printf("jitter: INFO renderer upscaleJitterNdc (+2jx/w, -2jy/h) through jitterProjection on a Vulkan projection: "
                "x off by up to %.3f px (moves points by +jx), y off by %.2e px — the open WP-4.1 sign question; use "
                "projection_jitter_ndc / temporal::jitter_view_proj\n",
                renderer[0], renderer[1]);
    expect(worstMine < 1e-3 && worstTemporal < 1e-3, "projection jitter follows the sample convention");
    // SDK Halton == the renderer's jitter sequence.
    f32 worstHalton = 0.f;
    for (i32 n : {8, 18, 23, 32, 72}) {
        for (i32 i = 0; i < 2 * n; ++i) {
            const math::Vec2 a = fsr3_sdk_jitter_offset(i, n);
            const math::Vec2 b = upscaleJitterOffset(static_cast<u32>(i), static_cast<u32>(n));
            worstHalton = std::max({worstHalton, std::fabs(a.x - b.x), std::fabs(a.y - b.y)});
        }
    }
    expect(worstHalton < 1e-6f, "ffxFsr3UpscalerGetJitterOffset == upscaleJitterOffset");
    // Phase counts: SDK truncates, the renderer rounds up.
    for (const f32 ratio : {1.f, 1.3f, 1.5f, 1.7f, 2.f, 3.f}) {
        const UpscaleResolution r = makeUpscaleResolution(1920u, 1080u, ratio);
        const i32 sdk = fsr3_jitter_phase_count(static_cast<i32>(r.render_width), static_cast<i32>(r.display_width));
        const u32 fuse = upscaleJitterPhaseCount(r);
        std::printf("  ratio %.1f (%u -> %u): SDK phase count %d, renderer %u\n", ratio, r.render_width, r.display_width, sdk, fuse);
        expect(static_cast<u32>(sdk) <= fuse && fuse <= static_cast<u32>(sdk) + 1u, "phase counts agree up to rounding");
    }
    std::printf("jitter: Halton offsets identical (max %.1e)\n", static_cast<f64>(worstHalton));
    return 0;
}

int runConvert() {
    const f32 n = 0.05f, fr = 500.f;
    f32 d2v[4];
    fsr3_device_to_view_depth(n, fr, true, false, 160, 90, 1.f, d2v);
    f64 worst = 0.0;
    f32 prev = 2.f;
    bool monotonic = true;
    for (u32 i = 0; i <= 4000u; ++i) {
        const f32 L = n * std::pow(fr / n, static_cast<f32>(i) / 4000.f);
        const f32 z = fsr3_device_depth(L, n, fr);
        const f64 back = fsr3_view_depth(z, d2v);
        worst = std::max(worst, std::fabs(back - L) / L);
        monotonic = monotonic && z <= prev;
        prev = z;
    }
    expect(worst < 2e-6, "reverse-Z device depth round-trips through the SDK inverted transform");
    expect(monotonic, "device depth decreases with distance (reverse-Z)");
    expect(fsr3_device_depth(0.f, n, fr) == 0.f && fsr3_device_depth(-1.f, n, fr) == 0.f, "sky -> 0");
    expect(std::fabs(fsr3_view_depth(0.f, d2v) - fr) / fr < 1e-6f, "device 0 = far plane");
    expect(std::fabs(fsr3_view_depth(1.f, d2v) - n) / n < 1e-6, "device 1 = near plane");
    // The non-inverted branch against the D3D-style perspective depth z = f / (f - n) (1 - n / L).
    fsr3_device_to_view_depth(n, fr, false, false, 160, 90, 1.f, d2v);
    f64 worstForward = 0.0;
    for (const f32 L : {0.06f, 1.f, 10.f, 123.f, 499.f}) {
        // f64 depth then rounded once (the f32 forward depth itself loses ~L / n ulps near the far plane).
        const f32 z = static_cast<f32>(static_cast<f64>(fr) / (static_cast<f64>(fr) - n) * (1.0 - static_cast<f64>(n) / L));
        worstForward = std::max(worstForward, static_cast<f64>(std::fabs(fsr3_view_depth(z, d2v) - L) / L));
    }
    expect(worstForward < 1e-3, "SDK non-inverted transform inverts the forward perspective depth");
    std::printf("convert: reverse-Z round trip max rel %.2e over [%.2f, %.0f], forward branch %.2e\n", worst, n, fr, worstForward);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    const std::string suite = argc > 1 ? argv[1] : "layout";
    int rc = 0;
    if (suite == "layout") {
        rc = runLayout();
    } else if (suite == "bindings") {
        rc = runBindings();
    } else if (suite == "host") {
        rc = runHost();
    } else if (suite == "jitter") {
        rc = runJitter();
    } else if (suite == "convert") {
        rc = runConvert();
    } else {
        std::fprintf(stderr, "unknown suite %s\n", suite.c_str());
        return 2;
    }
    if (rc == kSkip) {
        return kSkip;
    }
    if (rc != 0 || g_failures != 0) {
        std::fprintf(stderr, "FAIL: %d failure(s)\n", g_failures + rc);
        return 1;
    }
    std::printf("PASS %s\n", suite.c_str());
    return 0;
}
