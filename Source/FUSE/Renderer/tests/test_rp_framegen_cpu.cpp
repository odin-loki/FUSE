// WP-4.4 CPU gates for the FSR 3.1 frame-generation host (stub tree too): the clean-room port of the SDK's constant
// setup / reset logic / ping-pong / dispatch plan, the embedded passes' reflected layouts against the C++ records, the
// binding-name tables and the FUSE passes' CPU twins.
//
//   layout     reflection of every embedded pass (17 vendored + portable search + the FUSE convert / UI composite in
//              each built language): cbFI / cbInpaintingPyramid / cbOF / cbOF_SPD member offsets and sizes == the C++
//              records; push blocks == FgConvertPush / FgCompositePush; workgroup sizes as the host plans them.
//   bindings   every binding of every pass resolves by name (frame-interpolation vs optical-flow tables: the two effects
//              use "r_optical_flow" for different resources) to the kind it is declared as; unknown names rejected.
//   host       constants vs the SDK expressions: device-to-view depth (== fsr3::fsr3_device_to_view_depth, inverted /
//              finite, x view-space-to-meters), reverse-Z round trip, tan(half horizontal FOV), motion-vector scale -1,
//              jitter sign, optical-flow scale 1 / display, SPD setups == the vendored ffxSpdSetup; reset logic (first
//              dispatch, explicit reset, frame-id jump / decrease, invalidate); optical-flow frame index and ping-pong
//              parity (mod 16); resource sizes of opticalflowCreate / frameinterpolationCreate.
//   plan       the ordered dispatch plan vs an independent transcription of the SDK dispatch loops: pass order, counts
//              (37 per frame, 32 on reset), group counts, per-level ping-pong images (search / filter / scale), the
//              level-0 filter writing the shared optical-flow vector, constant ranges per dispatch.
//   composite  CPU twins of the FUSE passes: fg_device_depth == fsr3::fsr3_device_depth bit for bit; UI composite rules.
#include <fuse/renderer/framegen/fg_gpu.hpp>
#include <fuse/renderer/framegen/fg_types.hpp>
#include <fuse/renderer/upscale_backends/fsr3/fsr3_types.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#define FFX_CPU 1
#include <FidelityFX/gpu/ffx_core.h>
#include <FidelityFX/gpu/spd/ffx_spd.h>

namespace {

using namespace fuse::renderer::framegen;
namespace fsr3 = fuse::renderer::fsr3;
using fuse::f32;
using fuse::f64;
using fuse::i32;
using fuse::u16;
using fuse::u32;
using fuse::u64;
using fuse::usize;
using fuse::math::Vec4;

constexpr int kSkip = 77;
int g_failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

struct Expected {
    u32 offset;
    u32 size;
};

bool checkMembers(const std::vector<fsr3::SpirvBlockMember>& members, const Expected* expected, usize count, const char* what) {
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

#define FI_MEMBER(field) {static_cast<u32>(offsetof(FgFiConstants, field)), static_cast<u32>(sizeof(FgFiConstants::field))}
#define OF_MEMBER(field) {static_cast<u32>(offsetof(FgOfConstants, field)), static_cast<u32>(sizeof(FgOfConstants::field))}

bool reflectPass(FgPass pass, FgKernelLanguage language, fsr3::SpirvReflection& out) {
    const FgKernelCode c = fg_kernel_code(pass, language);
    return c.words != nullptr && fsr3::reflect_spirv(c.words, c.bytes / 4u, out);
}

// ---- layout -----------------------------------------------------------------------------------------------------
int runLayout() {
    const Expected fiBlock[] = {
        FI_MEMBER(renderSize), FI_MEMBER(displaySize), FI_MEMBER(displaySizeRcp), FI_MEMBER(cameraNear), FI_MEMBER(cameraFar),
        FI_MEMBER(upscalerTargetSize), FI_MEMBER(mode), FI_MEMBER(reset), FI_MEMBER(deviceToViewDepth), FI_MEMBER(deltaTime),
        FI_MEMBER(hudLessAttachedFactor), FI_MEMBER(distortionFieldSize), FI_MEMBER(opticalFlowScale),
        FI_MEMBER(opticalFlowBlockSize), FI_MEMBER(dispatchFlags), FI_MEMBER(maxRenderSize), FI_MEMBER(opticalFlowHalfResMode),
        FI_MEMBER(numInstances), FI_MEMBER(interpolationRectBase), FI_MEMBER(interpolationRectSize), FI_MEMBER(debugBarColor),
        FI_MEMBER(backBufferTransferFunction), FI_MEMBER(minMaxLuminance), FI_MEMBER(tanHalfFov), FI_MEMBER(pad1),
        FI_MEMBER(jitter), FI_MEMBER(motionVectorScale)};
    const Expected ipBlock[] = {{0, 4}, {4, 4}, {8, 8}};
    const Expected ofBlock[] = {OF_MEMBER(inputLumaResolution), OF_MEMBER(opticalFlowPyramidLevel),
                                OF_MEMBER(opticalFlowPyramidLevelCount), OF_MEMBER(frameIndex),
                                OF_MEMBER(backbufferTransferFunction), OF_MEMBER(minMaxLuminance)};
    const Expected ofSpdBlock[] = {{0, 4}, {4, 4}, {8, 8}, {16, 4}, {20, 4}, {24, 4}, {28, 4}};
    const Expected push16[] = {{0, 4}, {4, 4}, {8, 4}, {12, 4}};
    // Workgroup sizes the host's dispatch sizes assume (ffx_*_pass.glsl).
    struct Wg {
        FgPass pass;
        u32 x, y;
        u32 z = 1u;
    };
    const Wg wg[] = {{FgPass::OfPrepareLuma, 16, 16}, {FgPass::OfLuminancePyramid, 256, 1}, {FgPass::OfScdHistogram, 32, 8},
                     {FgPass::OfScdDivergence, 256, 1}, {FgPass::OfSearch, 64, 1}, {FgPass::OfSearchPortable, 64, 1},
                     {FgPass::OfFilter, 16, 4}, {FgPass::OfScale, 4, 4, 4}, {FgPass::FiReconstructAndDilate, 8, 8},
                     {FgPass::FiSetup, 8, 8}, {FgPass::FiReconstructPrevDepth, 8, 8}, {FgPass::FiGameMotionVectorField, 8, 8},
                     {FgPass::FiGameVectorFieldInpaintingPyramid, 256, 1}, {FgPass::FiOpticalFlowVectorField, 8, 8},
                     {FgPass::FiDisocclusionMask, 8, 8}, {FgPass::FiInterpolation, 8, 8}, {FgPass::FiInpaintingPyramid, 256, 1},
                     {FgPass::FiInpainting, 8, 8}};
    u32 passes = 0, fiBlocks = 0, ipBlocks = 0, ofBlocks = 0, ofSpdBlocks = 0;
    for (const Wg& w : wg) {
        fsr3::SpirvReflection r;
        if (!reflectPass(w.pass, FgKernelLanguage::Auto, r)) {
            std::printf("SKIP: frame-generation passes not built (glslangValidator missing)\n");
            return kSkip;
        }
        ++passes;
        const bool wgOk = r.localSize[0] == w.x && r.localSize[1] == w.y && r.localSize[2] == w.z;
        if (!wgOk) {
            std::fprintf(stderr, "  %s: workgroup %ux%ux%u, expected %ux%ux%u\n", fg_pass_name(w.pass), r.localSize[0], r.localSize[1],
                         r.localSize[2], w.x, w.y, w.z);
        }
        expect(wgOk, "workgroup size as planned");
        for (const fsr3::SpirvBinding& b : r.bindings) {
            if (b.name == "cbFI") {
                ++fiBlocks;
                expect(checkMembers(b.members, fiBlock, std::size(fiBlock), "cbFI"), "cbFI == FgFiConstants");
            } else if (b.name == "cbInpaintingPyramid") {
                ++ipBlocks;
                expect(checkMembers(b.members, ipBlock, std::size(ipBlock), "cbInpaintingPyramid"), "cbInpaintingPyramid");
            } else if (b.name == "cbOF") {
                ++ofBlocks;
                expect(checkMembers(b.members, ofBlock, std::size(ofBlock), "cbOF"), "cbOF == FgOfConstants");
            } else if (b.name == "cbOF_SPD") {
                ++ofSpdBlocks;
                expect(checkMembers(b.members, ofSpdBlock, std::size(ofSpdBlock), "cbOF_SPD"), "cbOF_SPD == FgOfSpdConstants");
            }
        }
        std::printf("  %-44s workgroup %3ux%ux%u %2zu bindings\n", fg_pass_name(w.pass), r.localSize[0], r.localSize[1], r.localSize[2], r.bindings.size());
    }
    expect(fiBlocks == 10u, "the ten frame-interpolation passes read cbFI");
    expect(ipBlocks == 2u, "the two inpainting pyramids read cbInpaintingPyramid");
    expect(ofBlocks >= 5u, "optical-flow passes read cbOF");
    std::printf("  (cbOF_SPD read by %u pass(es): -Os strips it where the pyramid does not use it)\n", ofSpdBlocks);
    u32 languages = 0;
    for (const FgKernelLanguage lang : {FgKernelLanguage::Slang, FgKernelLanguage::Glsl}) {
        fsr3::SpirvReflection conv, comp;
        if (!reflectPass(FgPass::Convert, lang, conv) || !reflectPass(FgPass::UiComposite, lang, comp)) {
            continue;
        }
        ++languages;
        expect(checkMembers(conv.pushConstants, push16, std::size(push16), "convert push"), "FgConvertPush == push block");
        expect(checkMembers(comp.pushConstants, push16, std::size(push16), "composite push"), "FgCompositePush == push block");
        expect(conv.pushConstantBytes == sizeof(FgConvertPush) && comp.pushConstantBytes == sizeof(FgCompositePush), "push sizes");
        expect(conv.localSize[0] == 8u && conv.localSize[1] == 8u && comp.localSize[0] == 8u && comp.localSize[1] == 8u, "FUSE passes 8x8");
        std::printf("  fg.convert / fg.ui_composite (%s): %zu / %zu bindings, push %u / %u bytes\n",
                    lang == FgKernelLanguage::Slang ? "slang" : "glsl", conv.bindings.size(), comp.bindings.size(), conv.pushConstantBytes,
                    comp.pushConstantBytes);
    }
    expect(languages >= 1u, "at least one language of the FUSE passes built");
    std::printf("layout: %u passes + FUSE passes in %u language(s) checked\n", passes, languages);
    return 0;
}

// ---- bindings ----------------------------------------------------------------------------------------------------
int runBindings() {
    u32 checked = 0;
    for (u32 p = 0; p < kFgPassCount; ++p) {
        const FgPass pass = static_cast<FgPass>(p);
        for (const FgKernelLanguage lang : {FgKernelLanguage::Slang, FgKernelLanguage::Glsl}) {
            fsr3::SpirvReflection r;
            if (!reflectPass(pass, lang, r)) {
                continue;
            }
            for (const fsr3::SpirvBinding& b : r.bindings) {
                FgBindingTarget t{};
                const bool ok = fg_resolve_binding(b.name.c_str(), fg_pass_is_optical_flow(pass), t);
                if (!ok) {
                    std::fprintf(stderr, "  %s: unresolved binding '%s'\n", fg_pass_name(pass), b.name.c_str());
                }
                expect(ok, "every binding resolves by name");
                const bool kind = (b.kind == fsr3::SpirvDescriptorKind::SampledImage && t.kind == FgBindingKind::Sampled) ||
                                  (b.kind == fsr3::SpirvDescriptorKind::StorageImage && t.kind == FgBindingKind::Storage) ||
                                  (b.kind == fsr3::SpirvDescriptorKind::Sampler && t.kind == FgBindingKind::Sampler) ||
                                  (b.kind == fsr3::SpirvDescriptorKind::UniformBuffer && t.kind == FgBindingKind::Uniform) ||
                                  (b.kind == fsr3::SpirvDescriptorKind::StorageBuffer && t.kind == FgBindingKind::StorageBuffer);
                if (ok && !kind) {
                    std::fprintf(stderr, "  %s: binding '%s' kind mismatch\n", fg_pass_name(pass), b.name.c_str());
                }
                expect(!ok || kind, "binding kind == name table");
                expect(b.set == 0u, "set 0");
                ++checked;
            }
            if (fg_pass_is_vendored(pass)) {
                break; // one language
            }
        }
    }
    FgBindingTarget t{};
    expect(fg_resolve_binding("r_optical_flow", false, t) && t.resource == FgResource::OpticalFlowVector, "FI r_optical_flow = OF result");
    expect(fg_resolve_binding("r_optical_flow", true, t) && t.resource == FgResource::OfFlowSrv, "OF r_optical_flow = per-level flow");
    expect(fg_resolve_binding("rw_inpainting_pyramid7", false, t) && t.mip == 7u && t.kind == FgBindingKind::Storage, "pyramid mip views");
    expect(fg_resolve_binding("rw_optical_flow_input_level_3", true, t) && t.resource == FgResource::OfInputLevel && t.mip == 3u,
           "OF input levels");
    expect(!fg_resolve_binding("r_not_a_resource", false, t) && !fg_resolve_binding(nullptr, true, t), "unknown names rejected");
    expect(!fg_resolve_binding("rw_optical_flow_scd_output", false, t), "OF-only names are not frame-interpolation names");
    std::printf("bindings: %u bindings resolved\n", checked);
    return 0;
}

// ---- host --------------------------------------------------------------------------------------------------------
FgFrameParams params(u64 frameId) {
    FgFrameParams p{};
    p.renderW = 192;
    p.renderH = 108;
    p.jitter_px = fuse::math::Vec2(0.25f, -0.125f);
    p.near_plane = 0.1f;
    p.far_plane = 1000.f;
    p.vertical_fov_rad = 0.9f;
    p.frame_time_ms = 16.667f;
    p.frame_id = frameId;
    return p;
}

void spdReference(u32 w, u32 h, i32 mips, u32 groups[2], u32 off[2], u32 num[2]) {
    FfxUInt32x2 g = {0u, 0u}, o = {0u, 0u}, n = {0u, 0u};
    FfxUInt32x4 rect = {0u, 0u, w, h};
    ffxSpdSetup(g, o, n, rect, mips);
    groups[0] = g[0];
    groups[1] = g[1];
    off[0] = o[0];
    off[1] = o[1];
    num[0] = n[0];
    num[1] = n[1];
}

int runHost() {
    FgHostState s{};
    expect(!fg_init_state(s, 48, 144, 192, 108, true), "display < 64 rejected (OF level 6)");
    expect(fg_init_state(s, 256, 144, 192, 108, true), "init");
    const FgSizes& z = s.sizes;
    expect(z.ofW[0] == 32u && z.ofH[0] == 18u && z.ofW[1] == 16u && z.ofH[1] == 9u && z.ofH[2] == 5u && z.ofW[6] == 1u && z.ofH[6] == 1u,
           "optical-flow texture sizes (8x8 blocks, align-up halving)");
    expect(z.ofInputW[6] == 4u && z.ofInputH[6] == 2u && z.ofInputW[0] == 256u, "luma pyramid sizes (w >> level)");
    expect(z.pyramidW == 128u && z.pyramidH == 72u && z.pyramidMips == 8u, "inpainting pyramid display/2 full chain");

    FgFrameSetup f{};
    FgFrameParams p = params(0);
    expect(fg_setup_frame(s, p, f), "frame 0");
    expect(f.fiReset && f.ofReset && f.fi.reset == 1, "first dispatch resets");
    const FgFiConstants& c = f.fi;
    expect(c.motionVectorScale[0] == -1.f && c.motionVectorScale[1] == -1.f, "motion-vector scale -1 (FUSE cur - prev -> SDK prev - cur)");
    expect(c.jitter[0] == -0.25f && c.jitter[1] == 0.125f, "jitter = FSR sign (-jitter_px)");
    expect(c.opticalFlowScale[0] == 1.f / 256.f && c.opticalFlowScale[1] == 1.f / 144.f && c.opticalFlowBlockSize == 8, "OF scale / block");
    expect(c.displaySize[0] == 256 && c.renderSize[0] == 192 && c.maxRenderSize[1] == 108 && c.interpolationRectSize[1] == 144 &&
               c.upscalerTargetSize[0] == 256 && c.displaySizeRcp[0] == 1.f / 256.f,
           "sizes");
    expect(c.backBufferTransferFunction == 0u && c.hudLessAttachedFactor == 0 && c.distortionFieldSize[0] == 1 && c.mode == 0,
           "sRGB source, HUD-less, default distortion field");
    f32 ref[4];
    fsr3::fsr3_device_to_view_depth(p.near_plane, p.far_plane, true, false, p.renderW, p.renderH, p.vertical_fov_rad, ref);
    expect(std::memcmp(ref, c.deviceToViewDepth, sizeof(ref)) == 0, "deviceToViewDepth == the WP-4.2 port (inverted, finite)");
    f64 worst = 0.0;
    for (f32 L = 0.2f; L < 900.f; L *= 1.37f) {
        const f32 z0 = fg_device_depth(L, p.near_plane, p.far_plane);
        const f32 back = std::fabs(fsr3::fsr3_view_depth(z0, c.deviceToViewDepth));
        worst = std::max(worst, std::fabs(static_cast<f64>(back) - L) / L);
    }
    std::printf("host: reverse-Z round trip max rel error %.3g\n", worst);
    expect(worst < 1e-5, "reverse-Z round trip");
    const f32 aspect = 192.f / 108.f;
    const f32 h = std::atan(std::tan(0.9f / 2.f) * aspect) * 2.f;
    expect(c.tanHalfFov == std::tan(h * 0.5f), "tan(half horizontal FOV) (SDK expression)");
    {
        FgHostState m{};
        fg_init_state(m, 256, 144, 192, 108, true);
        FgFrameParams q = params(0);
        q.view_space_to_meters = 2.f;
        FgFrameSetup g{};
        fg_setup_frame(m, q, g);
        expect(g.fi.deviceToViewDepth[1] == 2.f * ref[1] && g.fi.deviceToViewDepth[0] == ref[0], "viewSpaceToMetersFactor scales e");
    }
    u32 groups[2], off[2], num[2];
    spdReference(192, 108, -1, groups, off, num);
    expect(f.ipRender.numWorkGroups == num[0] && f.ipRender.mips == num[1] && f.ipRender.workGroupOffset[0] == off[0], "ipRender == ffxSpdSetup");
    spdReference(256, 144, -1, groups, off, num);
    expect(f.ipDisplay.numWorkGroups == num[0] && f.ipDisplay.mips == num[1] && f.ipDisplay.mips == 8u, "ipDisplay == ffxSpdSetup");
    spdReference(256, 144, 4, groups, off, num);
    expect(f.ofSpd.mips == 4u && f.ofSpd.numWorkGroups == num[0] && f.ofSpd.numWorkGroupsOpticalFlowInputPyramid == num[0], "OF SPD (4 mips)");
    expect(f.ofBase.frameIndex == 0 && f.ofBase.opticalFlowPyramidLevelCount == 0u && f.ofLevel[6].opticalFlowPyramidLevel == 6u &&
               f.ofLevel[3].opticalFlowPyramidLevelCount == 7u && f.ofBase.inputLumaResolution[0] == 256,
           "OF constants (first dispatch: stale level fields 0 / 0 like the SDK)");
    expect(!f.ofOddFrame, "OF parity starts even");

    // Steady frames.
    bool steady = true;
    for (u64 id = 1; id <= 20; ++id) {
        FgFrameSetup g{};
        steady = steady && fg_setup_frame(s, params(id), g) && !g.fiReset && !g.ofReset && g.ofBase.frameIndex == static_cast<i32>(id) &&
                 g.ofOddFrame == ((id % 16u) % 2u == 1u) && g.ofBase.opticalFlowPyramidLevelCount == 7u;
    }
    expect(steady, "steady frames: no reset, OF frame index increments, parity alternates");
    expect(s.ofResourceFrameIndex == 21u % 16u, "resourceFrameIndex mod 16");
    FgFrameSetup g{};
    fg_setup_frame(s, params(23), g);
    expect(g.fiReset && !g.ofReset, "frame-id skip resets the interpolation (disjoint frame id), not the optical flow");
    fg_setup_frame(s, params(24), g);
    expect(!g.fiReset, "then continues");
    fg_setup_frame(s, params(10), g);
    expect(g.fiReset, "decreasing frame id resets");
    FgFrameParams r = params(11);
    r.reset = true;
    fg_setup_frame(s, r, g);
    expect(g.fiReset && g.ofReset && g.ofBase.frameIndex == 0, "explicit reset resets both");
    FgFrameParams bad = params(12);
    bad.renderW = 193;
    const FgHostState before = s;
    expect(!fg_setup_frame(s, bad, g) && std::memcmp(&before, &s, sizeof(s)) == 0, "render size > max rejected, state untouched");
    bad = params(12);
    bad.far_plane = 0.05f;
    expect(!fg_setup_frame(s, bad, g), "far <= near rejected");
    return 0;
}

// ---- plan --------------------------------------------------------------------------------------------------------
int runPlan() {
    for (int portable = 0; portable < 2; ++portable) {
        FgHostState s{};
        fg_init_state(s, 256, 144, 192, 108, portable != 0);
        for (u64 id = 0; id < 4; ++id) {
            FgFrameSetup f{};
            fg_setup_frame(s, params(id), f);
            const bool reset = id == 0u;
            expect(f.dispatchCount == (reset ? 32u : 37u), "37 dispatches (32 on reset)");
            u32 k = 0;
            auto next = [&](FgPass pass) -> const FgDispatch& {
                const FgDispatch& d = f.dispatches[k < f.dispatchCount ? k : 0u];
                expect(k < f.dispatchCount && d.pass == pass, "pass order");
                if (k < f.dispatchCount && d.pass != pass) {
                    std::fprintf(stderr, "  dispatch %u: %s, expected %s\n", k, fg_pass_name(d.pass), fg_pass_name(pass));
                }
                ++k;
                return d;
            };
            expect(next(FgPass::Convert).groups[0] == 24u, "convert groups");
            next(FgPass::FiReconstructAndDilate);
            // SDK ffxOpticalflowContextDispatch transcription.
            const bool odd = f.ofOddFrame;
            const u16 inA = odd ? kSlotOfInput2 : kSlotOfInput1, inB = odd ? kSlotOfInput1 : kSlotOfInput2;
            const FgDispatch& luma = next(FgPass::OfPrepareLuma);
            expect(luma.ofInput == inA && luma.groups[0] == ((256u + 1u) / 2u + 15u) / 16u && luma.groups[1] == ((144u + 1u) / 2u + 15u) / 16u,
                   "prepare luma");
            const FgDispatch& pyr = next(FgPass::OfLuminancePyramid);
            expect(pyr.ofInputLevelBase == inA, "luma pyramid writes the current input levels");
            const FgDispatch& hist = next(FgPass::OfScdHistogram);
            expect(hist.groups[0] == ((256u / 4u) / 3u + 31u) / 32u && hist.groups[1] == 16u && hist.groups[2] == 9u && hist.ofInput == inA,
                   "SCD histogram");
            const FgDispatch& div = next(FgPass::OfScdDivergence);
            expect(div.groups[0] == 9u && div.groups[1] == 3u, "SCD divergence");
            for (i32 level = 6; level >= 0; --level) {
                const u32 l = static_cast<u32>(level);
                const bool oddLevel = (l & 1u) != 0u;
                const u16 fA = static_cast<u16>(((odd != oddLevel) ? kSlotOfFlow2 : kSlotOfFlow1) + l);
                const u16 fB = static_cast<u16>(((odd != oddLevel) ? kSlotOfFlow1 : kSlotOfFlow2) + l);
                const FgDispatch& se = next(portable != 0 ? FgPass::OfSearchPortable : FgPass::OfSearch);
                const u32 inW = 256u >> l, inH = 144u >> l;
                expect(se.ofInput == inA + l && se.ofPreviousInput == inB + l && se.ofFlow == fA && se.ofLevel == l && se.cbOf == FgCb::OfLevel0,
                       "search bindings");
                expect(se.groups[0] == std::max(((inW + 3u) / 4u * 16u + 63u) / 64u, 1u) && se.groups[1] == std::max((inH + 15u) / 16u, 1u),
                       "search groups");
                const FgDispatch& fi = next(FgPass::OfFilter);
                expect(fi.ofFlowPrevious == fA && fi.ofFlow == (l == 0u ? static_cast<u16>(kSlotOfVector) : fB), "filter bindings");
                if (l > 0u) {
                    const FgDispatch& sc = next(FgPass::OfScale);
                    expect(sc.ofFlowSrv == fB && sc.ofFlowNextLevel == fB - 1u && sc.ofInput == inA + l, "scale bindings");
                    // The next level's search reads (as prediction) exactly what this scale wrote.
                    const bool oddNext = ((l - 1u) & 1u) != 0u;
                    const u16 fANext = static_cast<u16>(((odd != oddNext) ? kSlotOfFlow2 : kSlotOfFlow1) + l - 1u);
                    expect(sc.ofFlowNextLevel == fANext, "scale output = next level's search target");
                }
            }
            next(FgPass::FiSetup);
            if (!reset) {
                next(FgPass::FiReconstructPrevDepth);
                next(FgPass::FiGameMotionVectorField);
                const FgDispatch& gvp = next(FgPass::FiGameVectorFieldInpaintingPyramid);
                expect(gvp.cbIp == FgCb::IpRender, "game-vector pyramid uses the render-rect SPD constants");
                const FgDispatch& ofv = next(FgPass::FiOpticalFlowVectorField);
                expect(ofv.groups[0] == static_cast<u32>(256.f / 8.f + 7.f) / 8u && ofv.groups[1] == static_cast<u32>(144.f / 8.f + 7.f) / 8u,
                       "OF vector field groups");
                next(FgPass::FiDisocclusionMask);
            }
            const FgDispatch& interp = next(FgPass::FiInterpolation);
            expect(interp.groups[0] == 32u && interp.groups[1] == 18u, "interpolation groups (display / 8)");
            expect(next(FgPass::FiInpaintingPyramid).cbIp == FgCb::IpDisplay, "inpainting pyramid display rect");
            next(FgPass::FiInpainting);
            const FgDispatch& c0 = next(FgPass::UiComposite);
            const FgDispatch& c1 = next(FgPass::UiComposite);
            expect(c0.compositeSource == kSlotOutput && c0.compositeTarget == kSlotPresentInterpolated && c1.compositeSource == kSlotCurrentSource &&
                       c1.compositeTarget == kSlotPresentReal,
                   "UI composite over the interpolated and the real frame");
            expect(k == f.dispatchCount, "plan fully checked");
        }
    }
    expect(fg_cb_offset(FgCb::OfLevel0, 6) + 256u == kFgCbSlotBytes && kFgCbSlotBytes == 3072u, "constant ring layout");
    std::printf("plan: order, group counts and ping-pong match the SDK transcription (vendored and portable search)\n");
    return 0;
}

// ---- composite / convert twins -------------------------------------------------------------------------------------
int runComposite() {
    u32 same = 0;
    for (f32 L = -1.f; L < 2000.f; L = L < 0.f ? 0.013f : L * 1.011f) {
        const f32 a = fg_device_depth(L, 0.1f, 1000.f);
        const f32 b = fsr3::fsr3_device_depth(L, 0.1f, 1000.f);
        same += std::memcmp(&a, &b, sizeof(a)) == 0 ? 1u : 0u;
        expect(std::memcmp(&a, &b, sizeof(a)) == 0, "fg_device_depth == fsr3_device_depth");
    }
    const Vec4 src(0.2f, 0.4f, 0.6f, 0.3f);
    const Vec4 noUi = fg_composite(src, Vec4(1.f, 1.f, 1.f, 1.f), false);
    expect(noUi.x == 0.2f && noUi.y == 0.4f && noUi.z == 0.6f && noUi.w == 1.f, "no UI: copy, alpha 1");
    const Vec4 opaque = fg_composite(src, Vec4(0.9f, 0.1f, 0.3f, 1.f), true);
    expect(opaque.x == 0.9f && opaque.y == 0.1f && opaque.z == 0.3f, "opaque UI replaces");
    const Vec4 clear = fg_composite(src, Vec4(0.f, 0.f, 0.f, 0.f), true);
    expect(clear.x == 0.2f && clear.y == 0.4f && clear.z == 0.6f, "transparent UI passes through");
    const Vec4 half = fg_composite(src, Vec4(0.25f, 0.f, 0.5f, 0.5f), true);
    expect(half.x == 0.2f * 0.5f + 0.25f && half.z == 0.6f * 0.5f + 0.5f, "premultiplied over");
    std::printf("composite: %u depth values bit-identical to the WP-4.2 convert twin; UI rules hold\n", same);
    return 0;
}

/// CPU twin of fg.of.search_portable (fg_of_search_cpu): the search semantics on synthetic 8-bit luma.
int runOfSearch() {
    constexpr u32 kW = 96, kH = 64, kFw = kW / 8u, kFh = kH / 8u;
    auto noise = [](i32 x, i32 y) {
        u32 hsh = static_cast<u32>(x) * 73856093u ^ static_cast<u32>(y) * 19349663u;
        hsh ^= hsh >> 13;
        hsh *= 0x5bd1e995u;
        hsh ^= hsh >> 15;
        return static_cast<std::uint8_t>(hsh & 0xffu);
    };
    std::vector<std::uint8_t> prev(kW * kH), cur(kW * kH), flat(kW * kH, 128u);
    std::vector<std::int16_t> flow(kFw * kFh * 2u), pred(kFw * kFh * 2u, 0);
    auto interiorExact = [&](i32 ex, i32 ey, u32 margin) {
        u32 good = 0, total = 0;
        for (u32 fy = margin; fy + margin < kFh; ++fy) {
            for (u32 fx = margin; fx + margin < kFw; ++fx) {
                ++total;
                good += (flow[(fy * kFw + fx) * 2u] == ex && flow[(fy * kFw + fx) * 2u + 1u] == ey) ? 1u : 0u;
            }
        }
        return good == total && total > 0u;
    };
    // Translation v = (3, -5): content at p in `cur` was at p - v in `prev` -> vector -v.
    const i32 vx = 3, vy = -5;
    for (i32 y = 0; y < static_cast<i32>(kH); ++y) {
        for (i32 x = 0; x < static_cast<i32>(kW); ++x) {
            prev[static_cast<usize>(y) * kW + x] = noise(x, y);
            cur[static_cast<usize>(y) * kW + x] = noise(x - vx, y - vy);
        }
    }
    fg_of_search_cpu(cur.data(), prev.data(), kW, kH, 0u, false, pred.data(), false, flow.data());
    expect(interiorExact(-vx, -vy, 1u), "search: interior blocks recover -v exactly");
    // With a prediction: the search is centred on it (level 1, prediction = -v + (2, 1) -> still -v).
    for (u32 i = 0; i < kFw * kFh; ++i) {
        pred[i * 2u] = static_cast<std::int16_t>(-vx + 2);
        pred[i * 2u + 1u] = static_cast<std::int16_t>(-vy + 1);
    }
    fg_of_search_cpu(cur.data(), prev.data(), kW, kH, 1u, true, pred.data(), false, flow.data());
    expect(interiorExact(-vx, -vy, 2u), "search: prediction + local search = -v");
    // A displacement outside prediction + [-8, 7] is not found (search window).
    fg_of_search_cpu(cur.data(), prev.data(), kW, kH, 1u, true, std::vector<std::int16_t>(kFw * kFh * 2u, 12).data(), false, flow.data());
    expect(!interiorExact(-vx, -vy, 2u), "search: the window is prediction + [-8, 7]");
    // No motion -> 0; flat image -> all SADs equal, the top-left-bias fix picks the centre (0, 0).
    fg_of_search_cpu(prev.data(), prev.data(), kW, kH, 0u, false, pred.data(), false, flow.data());
    expect(interiorExact(0, 0, 0u), "search: static image -> (0, 0)");
    fg_of_search_cpu(flat.data(), flat.data(), kW, kH, 1u, false, pred.data(), false, flow.data());
    expect(interiorExact(0, 0, 0u), "search: flat image -> centre (top-left-bias fix)");
    // Level-0 local-search fallback: the zero-offset SAD wins ties even against a prediction.
    fg_of_search_cpu(flat.data(), flat.data(), kW, kH, 0u, true, std::vector<std::int16_t>(kFw * kFh * 2u, 5).data(), false, flow.data());
    expect(interiorExact(0, 0, 0u), "search: level-0 fallback to (0, 0) on ties");
    fg_of_search_cpu(cur.data(), prev.data(), kW, kH, 0u, false, pred.data(), true, flow.data());
    expect(interiorExact(0, 0, 0u), "search: scene change -> (0, 0)");
    std::printf("of_search: CPU twin recovers v = (%d, %d); window, prediction, top-left bias, fallback and scene change hold\n", vx, vy);
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
    } else if (suite == "plan") {
        rc = runPlan();
    } else if (suite == "composite") {
        rc = runComposite();
    } else if (suite == "of_search") {
        rc = runOfSearch();
    } else {
        std::fprintf(stderr, "unknown suite %s\n", suite.c_str());
        return 2;
    }
    if (rc == kSkip) {
        return kSkip;
    }
    if (rc != 0 || g_failures != 0) {
        std::fprintf(stderr, "FAIL %s: %d failure(s)\n", suite.c_str(), g_failures + rc);
        return 1;
    }
    std::printf("PASS %s\n", suite.c_str());
    return 0;
}
