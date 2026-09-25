// WP-4.5 GPU post stack: CPU gates (stub-safe). The Lavapipe gates are test_rp_post_gpu.cpp.
//
//   layout     PostFrameConstants / PostExposureState / PostPush sizes; the GLSL and Slang mirrors of
//              PostFrameConstants (shaders/post/pp_common.{glsl,slang}) declare the same fields in the
//              same order at the same offsets (parsed from the sources); PostBufferLayout sections are
//              256-aligned, disjoint and sized for the extent; the pyramid matches bloom_level_count.
//   agx        the AgX operator added to the CPU oracle: black -> 0, bounded [0, 1], monotone on a grey
//              ramp and on each primary, near-neutral greys, saturates toward white, a mid-grey
//              calibration exists and lands scene 0.18 on display 0.18, apply_tone_map / name dispatch,
//              PostStack::processFrame runs with it.
//   settings   settings_from_post_stack mirrors what PostStack::processFrame applies (bloom, DoF, motion
//              blur, exposure + calibration, auto exposure, curve, tone mapper, non-neutral grade stages,
//              vignette, grain); settings_from_look walks the graph (order, unsupported nodes, LUT,
//              look vignette / grain); a no-op Look resolves to exactly the constants (bytes) of the
//              PostStack it feeds through apply_look_to_post_stack - the CPU half of "no-op Look is
//              bit-identical to PostStack"; the neutral default look likewise for every shared field.
//   reference  display_reference == PostStack::processFrame per pixel (the spatial chain off) within
//              1e-6 for every tone mapper incl. AgX, with the curve, a full grade, vignette and grain;
//              spatial_reference == bloom_composite / dof_pass / motion_blur_pass; histogram_reference
//              == LuminanceHistogram; the LUT branch == look::kernels::GradeLutKernel.
//   api        PostStackGpu without a device fails cleanly (init false, no passes, no refs).
#include <fuse/renderer/look/effect_graph.hpp>
#include <fuse/renderer/look/look_kernels.hpp>
#include <fuse/renderer/look/look_params.hpp>
#include <fuse/renderer/look/look_post_chain.hpp>
#include <fuse/renderer/look/lut3d.hpp>
#include <fuse/renderer/postprocess/gpu/post_gpu_reference.hpp>
#include <fuse/renderer/postprocess/gpu/post_gpu_types.hpp>
#include <fuse/renderer/postprocess/gpu/post_stack_gpu.hpp>
#include <fuse/renderer/postprocess/post_stack.hpp>
#include <fuse/renderer/rg/graph.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace fuse::renderer;
using namespace fuse::renderer::post_gpu;
using fuse::f32;
using fuse::f64;
using fuse::u32;
using fuse::u64;
using fuse::math::Vec2;
using fuse::math::Vec3;

int g_failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

f32 maxAbs(const Vec3& a, const Vec3& b) {
    return std::max({std::fabs(a.x - b.x), std::fabs(a.y - b.y), std::fabs(a.z - b.z)});
}

u32 hashU32(u32 x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
f32 rnd(u32 seed) { return static_cast<f32>(hashU32(seed) & 0xFFFFFFu) / 16777216.f; }

/// Deterministic HDR frame: gradient + bright spots (bloom), dark corner (exposure range).
std::vector<Vec3> makeHdr(u32 w, u32 h) {
    std::vector<Vec3> img(static_cast<size_t>(w) * h);
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            const u32 i = y * w + x;
            const f32 fx = static_cast<f32>(x) / static_cast<f32>(w);
            const f32 fy = static_cast<f32>(y) / static_cast<f32>(h);
            Vec3 c{0.05f + 0.6f * fx, 0.03f + 0.4f * fy, 0.2f * (1.f - fx) + 0.02f};
            c = c * (0.5f + rnd(i * 3u + 1u));
            if ((x / 7u + y / 5u) % 9u == 0u) {
                c = c * 12.f;
            }
            img[i] = c;
        }
    }
    return img;
}

// --- layout ----------------------------------------------------------------------------------------
struct Field {
    const char* name;
    size_t offset;
};
#define PP_FIELD(n) Field{#n, offsetof(PostFrameConstants, n)}
const Field kFields[] = {
    PP_FIELD(histPartials), PP_FIELD(exposureState), PP_FIELD(lut), PP_FIELD(dump), PP_FIELD(cocs), PP_FIELD(tileMax),
    PP_FIELD(neighborMax), PP_FIELD(reserved0), PP_FIELD(width), PP_FIELD(height), PP_FIELD(inputHdr),
    PP_FIELD(inputDepth), PP_FIELD(inputVelocity), Field{"output_", offsetof(PostFrameConstants, output)},
    PP_FIELD(flags), PP_FIELD(toneMapper), PP_FIELD(exposureScale), PP_FIELD(exposureEv), PP_FIELD(histMinLog),
    PP_FIELD(histMaxLog), PP_FIELD(histBins), PP_FIELD(histGroupsX), PP_FIELD(histGroups), PP_FIELD(percentile),
    PP_FIELD(minEv), PP_FIELD(maxEv), PP_FIELD(targetLuminance), PP_FIELD(meteringBias), PP_FIELD(speedUp),
    PP_FIELD(speedDown), PP_FIELD(emaUp), PP_FIELD(emaDown), PP_FIELD(deltaSeconds), PP_FIELD(resetEv),
    PP_FIELD(adaptValid), PP_FIELD(histValid), PP_FIELD(bloomThreshold), PP_FIELD(bloomKnee), PP_FIELD(bloomIntensity),
    PP_FIELD(bloomScatter), PP_FIELD(bloomTint), PP_FIELD(dofFocalDistance), PP_FIELD(dofFocalLength),
    PP_FIELD(dofFStop), PP_FIELD(dofSensorWidth), PP_FIELD(dofMaxRadius), PP_FIELD(dofReach), PP_FIELD(reserved1),
    PP_FIELD(reserved2), PP_FIELD(mbSamples), PP_FIELD(mbShutter), PP_FIELD(mbMaxBlur), PP_FIELD(mbSoftDepth),
    PP_FIELD(mbTile), PP_FIELD(mbTilesX), PP_FIELD(mbTilesY), PP_FIELD(reserved3), PP_FIELD(curveKind),
    PP_FIELD(filmicA), PP_FIELD(filmicB), PP_FIELD(filmicC), PP_FIELD(filmicD), PP_FIELD(filmicE), PP_FIELD(filmicF),
    PP_FIELD(filmicWhiteScale), PP_FIELD(curveGamma), PP_FIELD(reinhardWhite), PP_FIELD(reinhardScale),
    PP_FIELD(acesContrast), PP_FIELD(acesShoulder), PP_FIELD(reserved4), PP_FIELD(reserved5), PP_FIELD(lutSize),
    PP_FIELD(lift), PP_FIELD(invGamma), PP_FIELD(gain), PP_FIELD(vignetteTint), PP_FIELD(contrast),
    PP_FIELD(saturation), PP_FIELD(vignette), PP_FIELD(vignetteFalloff), PP_FIELD(vignetteRoundness), PP_FIELD(grain),
    PP_FIELD(grainResponse), PP_FIELD(seedLo), PP_FIELD(seedHi), PP_FIELD(reserved6), PP_FIELD(reserved7),
    PP_FIELD(reserved8),
};
#undef PP_FIELD

/// Parses `struct PpFrame { ... };` of a shader source: (name, scalar-layout offset) per field.
bool parseShaderStruct(const std::string& path, std::vector<Field>& out, size_t& size, std::vector<std::string>& names) {
    std::ifstream file(path);
    if (!file) {
        return false;
    }
    std::stringstream ss;
    ss << file.rdbuf();
    const std::string text = ss.str();
    const size_t begin = text.find("struct PpFrame {");
    const size_t end = text.find("};", begin);
    if (begin == std::string::npos || end == std::string::npos) {
        return false;
    }
    std::istringstream body(text.substr(begin + 16, end - begin - 16));
    std::string line;
    size_t offset = 0;
    while (std::getline(body, line)) {
        std::istringstream ls(line);
        std::string type, decl;
        if (!(ls >> type >> decl) || decl.back() != ';') {
            continue;
        }
        decl.pop_back();
        size_t count = 1;
        const size_t bracket = decl.find('[');
        if (bracket != std::string::npos) {
            count = static_cast<size_t>(std::stoul(decl.substr(bracket + 1)));
            decl = decl.substr(0, bracket);
        }
        const size_t bytes = type == "uint64_t" ? 8u : 4u;
        offset = (offset + bytes - 1u) / bytes * bytes;
        names.push_back(decl);
        out.push_back(Field{nullptr, offset});
        offset += bytes * count;
    }
    size = offset;
    return true;
}

void testLayout() {
    expect(sizeof(PostFrameConstants) == 448u, "PostFrameConstants is 448 bytes");
    expect(sizeof(PostExposureState) == 32u + 4u * kPostHistMaxBins, "PostExposureState layout");
    expect(sizeof(PostPush) == 64u, "PostPush is 64 bytes (<= 128 guaranteed push-constant bytes)");
    const size_t fieldCount = sizeof(kFields) / sizeof(kFields[0]);
    for (const char* lang : {"glsl", "slang"}) {
        const std::string path = std::string(FUSE_RP_POST_SHADER_DIR) + "/pp_common." + lang;
        std::vector<Field> fields;
        std::vector<std::string> names;
        size_t size = 0;
        const bool parsed = parseShaderStruct(path, fields, size, names);
        expect(parsed, "pp_common struct PpFrame parsed");
        if (!parsed) {
            continue;
        }
        bool same = fields.size() == fieldCount && size == sizeof(PostFrameConstants);
        for (size_t i = 0; same && i < fieldCount; ++i) {
            same = names[i] == kFields[i].name && fields[i].offset == kFields[i].offset;
            if (!same) {
                std::fprintf(stderr, "  %s field %zu: shader %s @%zu vs C++ %s @%zu\n", lang, i, names[i].c_str(),
                             fields[i].offset, kFields[i].name, kFields[i].offset);
            }
        }
        std::printf("layout: pp_common.%s PpFrame %zu fields, %zu bytes\n", lang, fields.size(), size);
        expect(same, "shader PpFrame == PostFrameConstants (names, order, offsets, size)");
    }

    for (const auto& extent : {std::pair<u32, u32>{1u, 1u}, {61u, 37u}, {256u, 192u}, {1920u, 1080u}}) {
        const u32 w = extent.first;
        const u32 h = extent.second;
        const PostBufferLayout l = PostBufferLayout::compute(w, h);
        BloomParams deep{};
        deep.mip_levels = 64;
        expect(l.levels == bloom_level_count(w, h, deep), "pyramid reaches 1 x 1 like bloom_level_count");
        expect(l.levelW[l.levels - 1u] == 1u && l.levelH[l.levels - 1u] == 1u, "last level is 1 x 1");
        struct Section {
            u64 at, bytes;
        };
        std::vector<Section> sections;
        const u64 n = static_cast<u64>(w) * h;
        sections.push_back({l.hdr[0], n * 16});
        sections.push_back({l.hdr[1], n * 16});
        for (u32 i = 0; i < l.levels; ++i) {
            sections.push_back({l.down[i], static_cast<u64>(l.levelW[i]) * l.levelH[i] * 16});
        }
        sections.push_back({l.tmp, std::max<u64>(static_cast<u64>((w + 1) / 2) * h, static_cast<u64>(w) * ((h + 1) / 2)) * 16});
        sections.push_back({l.bloom, n * 16});
        sections.push_back({l.cocs, n * 8});
        sections.push_back({l.tileMax, n * 8});
        sections.push_back({l.neighborMax, n * 8});
        sections.push_back({l.histPartials, static_cast<u64>(l.histGroups) * kPostHistMaxBins * 4});
        bool ok = true;
        for (size_t i = 0; i < sections.size(); ++i) {
            ok = ok && sections[i].at % 256u == 0u && sections[i].at + sections[i].bytes <= l.workBytes;
            for (size_t j = i + 1; j < sections.size(); ++j) {
                ok = ok && (sections[i].at + sections[i].bytes <= sections[j].at || sections[j].at + sections[j].bytes <= sections[i].at);
            }
        }
        expect(ok, "work sections are 256-aligned, disjoint and inside the buffer");
        expect(l.histGroups == ((w + 63u) / 64u) * ((h + 63u) / 64u), "one histogram row per 64 x 64 tile");
        std::printf("layout: %ux%u work buffer %.2f MiB (%u pyramid levels, %u histogram tiles)\n", w, h,
                    static_cast<f64>(l.workBytes) / (1024.0 * 1024.0), l.levels, l.histGroups);
    }
}

// --- agx -------------------------------------------------------------------------------------------
void testAgx() {
    const Vec3 black = agx_tonemap({0.f, 0.f, 0.f});
    expect(black.x == 0.f && black.y == 0.f && black.z == 0.f, "AgX maps black to 0");
    bool bounded = true;
    bool monotone = true;
    f32 prev[4] = {-1.f, -1.f, -1.f, -1.f};
    f32 greyDrift = 0.f;
    for (u32 i = 0; i <= 400u; ++i) {
        const f32 v = std::exp2(-14.f + 0.05f * static_cast<f32>(i));
        const Vec3 g = agx_tonemap({v, v, v});
        const Vec3 r = agx_tonemap({v, 0.f, 0.f});
        const f32 vals[4] = {g.y, r.x, agx_tonemap({0.f, v, 0.f}).y, agx_tonemap({0.f, 0.f, v}).z};
        for (u32 k = 0; k < 4u; ++k) {
            monotone = monotone && vals[k] >= prev[k] - 1e-6f;
            prev[k] = vals[k];
        }
        for (const Vec3& c : {g, r}) {
            bounded = bounded && c.x >= 0.f && c.x <= 1.f && c.y >= 0.f && c.y <= 1.f && c.z >= 0.f && c.z <= 1.f;
        }
        greyDrift = std::max(greyDrift, std::max(std::fabs(g.x - g.y), std::fabs(g.z - g.y)));
    }
    expect(bounded, "AgX output in [0, 1]");
    expect(monotone, "AgX monotone on the grey ramp and on each primary");
    expect(greyDrift < 2e-3f, "AgX keeps greys near-neutral (inset rows sum to 1 within 1.4e-4)");
    std::printf("agx: grey drift %.2e\n", static_cast<f64>(greyDrift));
    const Vec3 white = agx_tonemap({1e4f, 1e4f, 1e4f});
    expect(white.x > 0.98f && white.y > 0.98f && white.z > 0.98f, "AgX saturates toward white");
    const Vec3 hot = agx_tonemap({1e4f, 0.f, 0.f});
    expect(hot.y > 0.01f && hot.z > 0.01f, "AgX desaturates a very bright primary (channel crosstalk)");
    const f32 ev = tone_mapper_mid_grey_calibration_ev(ToneMapper::AgX);
    const f32 grey = kSceneMidGrey * std::exp2(ev);
    const Vec3 mid = apply_tone_map({grey, grey, grey}, ToneMapper::AgX);
    std::printf("agx: mid-grey calibration %.4f EV -> display %.5f\n", static_cast<f64>(ev), static_cast<f64>(mid.y));
    expect(ev != 0.f && std::fabs(mid.y - kDisplayMidGrey) < 1e-3f, "AgX mid-grey calibration lands 0.18 on 0.18");
    const Vec3 x{0.3f, 1.7f, 0.05f};
    const Vec3 a = apply_tone_map(x, ToneMapper::AgX);
    const Vec3 b = agx_tonemap(x);
    expect(a.x == b.x && a.y == b.y && a.z == b.z, "apply_tone_map dispatches AgX");
    expect(std::strcmp(tone_mapper_name(ToneMapper::AgX), "agx") == 0, "tone_mapper_name(AgX)");

    PostStack stack;
    stack.init({16, 8});
    ColorGradeParams grade = stack.colorGrade().params();
    grade.tone_mapper = ToneMapper::AgX;
    stack.setColorGradeParams(grade);
    const std::vector<Vec3> hdr = makeHdr(16, 8);
    PostFrameInput in{};
    in.hdr = hdr.data();
    in.width = 16;
    in.height = 8;
    std::vector<Vec3> out;
    expect(stack.processFrame(in, out) && stack.toneMap().mapper() == ToneMapper::AgX, "PostStack runs AgX");
}

// --- settings --------------------------------------------------------------------------------------
look::LookResolved noopLook() {
    look::LookParamBlock b = look::look_default_params();
    using P = look::LookParam;
    for (P p : {P::AoEnabled, P::DofEnabled, P::MbEnabled, P::BloomEnabled, P::DirtEnabled, P::FlareEnabled,
                P::ExpAutoEnabled, P::GradeEnabled, P::SharpenEnabled, P::CaEnabled, P::VignetteEnabled,
                P::GrainEnabled}) {
        b.set(p, 0.f);
    }
    return look::look_resolve(b);
}

bool sameConstants(const PostGpuSettings& a, const PostGpuSettings& b) {
    PostFrameConstants ca{};
    PostFrameConstants cb{};
    resolve_constants(a, ca);
    resolve_constants(b, cb);
    return std::memcmp(&ca, &cb, sizeof(ca)) == 0;
}

void testSettings() {
    PostStack stack;
    stack.init({64, 40});
    PostGpuSettings s = settings_from_post_stack(stack, 7u, 0.02f);
    expect(s.bloom && s.bloomParams.intensity == stack.bloom().params().intensity, "stack: bloom (intensity != 0)");
    expect(s.dof == stack.dofParams().enabled && s.motionBlur == stack.motionBlurParams().enabled, "stack: DoF / MB flags");
    expect(s.spatialCount == 3u && s.spatialOrder[0] == PostSpatialPass::Bloom &&
               s.spatialOrder[1] == PostSpatialPass::DepthOfField && s.spatialOrder[2] == PostSpatialPass::MotionBlur,
           "stack: processFrame's spatial order");
    expect(std::fabs(s.exposureEv - (stack.totalExposureEv() + (stack.autoExposure().params().enabled ? stack.autoExposure().currentEv() : 0.f))) < 1e-6f,
           "stack: exposure = manual + calibration (auto subtracted on the GPU)");
    expect(s.autoExposure == stack.autoExposure().params().enabled, "stack: auto exposure");
    expect(!s.anyGrade() && s.vignetteMode == PostVignetteMode::Stack && s.grainMode == PostGrainMode::Stack,
           "stack defaults: neutral grade stages skipped, vignette + grain on");
    ColorGradeParams g = stack.colorGrade().params();
    g.lift = {0.02f, 0.f, -0.01f};
    g.saturation = 1.2f;
    stack.setColorGradeParams(g);
    s = settings_from_post_stack(stack, 0u);
    expect(s.gradeLiftContrast && s.gradeSaturation && !s.gradeGammaGain && s.grainMode == PostGrainMode::None,
           "stack: only non-neutral grade stages; seed 0 disables grain");

    // Look front end: graph order, unsupported nodes.
    look::LookEffectGraph graph = look::LookEffectGraph::makeDefault();
    look::LookParamBlock b = look::look_default_params();
    b.set(look::LookParam::DofEnabled, 1.f);
    b.set(look::LookParam::SharpenEnabled, 1.f);
    b.set(look::LookParam::CaEnabled, 1.f);
    const look::LookResolved look = look::look_resolve(b);
    PostGpuSettings ls{};
    u32 unsupported = 0;
    expect(settings_from_look(graph, look, 5u, 0.02f, ls, &unsupported), "look: default graph resolves");
    expect(ls.spatialCount == 3u && ls.spatialOrder[0] == PostSpatialPass::DepthOfField &&
               ls.spatialOrder[1] == PostSpatialPass::MotionBlur && ls.spatialOrder[2] == PostSpatialPass::Bloom,
           "look: spatial passes in graph order (DoF, motion blur, bloom)");
    expect(ls.lut && ls.vignetteMode == PostVignetteMode::Look && ls.grainMode == PostGrainMode::Look &&
               ls.autoExposure,
           "look: grade -> LUT, look vignette / grain, auto exposure");
    expect(unsupported == ((1u << static_cast<u32>(look::LookEffect::Sharpen)) |
                           (1u << static_cast<u32>(look::LookEffect::ChromaticAberration))),
           "look: sharpen + chromatic aberration reported unsupported");
    std::printf("settings: look unsupported mask 0x%x\n", unsupported);
    look::LookEffectGraph bad;
    bad.push(look::LookEffect::Bloom);
    expect(!settings_from_look(bad, look, 5u, 0.02f, ls, nullptr), "look: invalid graph rejected");

    // No-op Look == the PostStack it feeds (constants byte for byte, same passes).
    for (const ToneMapper op : {ToneMapper::ACES, ToneMapper::Filmic, ToneMapper::Reinhard, ToneMapper::Neutral}) {
        look::LookResolved noop = noopLook();
        noop.tonemap.op = static_cast<look::LookToneMapOperator>(op);
        PostStack fed;
        fed.init({64, 40});
        look::apply_look_to_post_stack(noop, fed);
        PostGpuSettings fromLook{};
        expect(settings_from_look(look::LookEffectGraph::makeDefault(), noop, 99u, 0.02f, fromLook, nullptr),
               "no-op look resolves");
        const PostGpuSettings fromStack = settings_from_post_stack(fed, 99u, 0.02f);
        const bool passes = fromLook.bloom == fromStack.bloom && fromLook.dof == fromStack.dof &&
                            fromLook.motionBlur == fromStack.motionBlur && fromLook.autoExposure == fromStack.autoExposure &&
                            fromLook.lut == false && !fromStack.anyGrade();
        expect(passes, "no-op Look and its PostStack enable the same passes (none spatial, no grade)");
        expect(sameConstants(fromLook, fromStack), "no-op Look constants == PostStack constants (bytes)");
    }
}

// --- reference -------------------------------------------------------------------------------------
void testReference() {
    const u32 w = 48;
    const u32 h = 27;
    const std::vector<Vec3> hdr = makeHdr(w, h);
    // display_reference vs PostStack::processFrame (no spatial passes, manual exposure).
    f32 worst = 0.f;
    for (const ToneMapper op : {ToneMapper::ACES, ToneMapper::Filmic, ToneMapper::Reinhard, ToneMapper::Neutral, ToneMapper::AgX}) {
        for (u32 variant = 0; variant < 3u; ++variant) {
            PostStack stack;
            stack.init({w, h});
            BloomParams bloom = stack.bloom().params();
            bloom.intensity = 0.f;
            stack.setBloomParams(bloom);
            AutoExposureParams ae = stack.autoExposure().params();
            ae.enabled = false;
            stack.setAutoExposureParams(ae);
            ColorGradeParams g = stack.colorGrade().params();
            g.tone_mapper = op;
            g.exposure = 0.4f;
            if (variant >= 1u) {
                g.lift = {0.01f, -0.02f, 0.03f};
                g.contrast = 1.15f;
                g.saturation = 0.8f;
                g.gamma = {1.1f, 0.95f, 1.f};
                g.gain = {1.02f, 1.f, 0.97f};
                g.vignette = 0.35f;
                g.film_grain = 0.03f;
            }
            stack.setColorGradeParams(g);
            if (variant == 2u) {
                stack.setTonemapCurveParams(make_filmic_curve_params());
            }
            PostFrameInput in{};
            in.hdr = hdr.data();
            in.width = w;
            in.height = h;
            in.frame_seed = variant == 0u ? 0u : 1234u;
            std::vector<Vec3> oracle;
            stack.processFrame(in, oracle);
            const PostGpuSettings s = settings_from_post_stack(stack, in.frame_seed);
            const f32 scale = std::pow(2.f, s.exposureEv);
            for (u32 y = 0; y < h; ++y) {
                for (u32 x = 0; x < w; ++x) {
                    const Vec3 r = display_reference(s, hdr[y * w + x], scale, x, y, w, h, nullptr);
                    worst = std::max(worst, maxAbs(r, oracle[y * w + x]));
                }
            }
        }
    }
    std::printf("reference: display_reference vs PostStack::processFrame max |diff| %.3g\n", static_cast<f64>(worst));
    expect(worst < 1e-6f, "display_reference == PostStack::processFrame (all operators, curve, grade, vignette, grain)");

    // spatial_reference == the oracle passes.
    std::vector<f32> depth(static_cast<size_t>(w) * h);
    std::vector<Vec2> velocity(static_cast<size_t>(w) * h);
    for (u32 i = 0; i < w * h; ++i) {
        depth[i] = 2.f + 20.f * rnd(i + 77u);
        velocity[i] = Vec2{(rnd(i + 5u) - 0.5f) * 12.f, (rnd(i + 9u) - 0.5f) * 6.f};
    }
    PostGpuSettings s{};
    s.bloom = true;
    s.dof = true;
    s.dofParams.focal_distance = 6.f;
    s.dofParams.max_coc_radius_px = 4.f;
    s.motionBlur = true;
    std::vector<Vec3> ref;
    spatial_reference(s, hdr.data(), depth.data(), velocity.data(), w, h, ref);
    std::vector<Vec3> a, b2, c;
    bloom_composite(hdr.data(), w, h, s.bloomParams, a);
    dof_pass(a.data(), depth.data(), w, h, s.dofParams, b2);
    motion_blur_pass(b2.data(), velocity.data(), depth.data(), w, h, s.motionBlurParams, c);
    bool same = ref.size() == c.size();
    for (size_t i = 0; same && i < c.size(); ++i) {
        same = maxAbs(ref[i], c[i]) == 0.f;
    }
    expect(same, "spatial_reference == bloom_composite -> dof_pass -> motion_blur_pass (bit for bit)");

    // histogram_reference == LuminanceHistogram.
    LuminanceHistogramParams hp{};
    std::vector<u32> bins;
    histogram_reference(hdr.data(), w * h, hp, bins);
    LuminanceHistogram hist;
    hist.init(hp);
    for (const Vec3& v : hdr) {
        hist.accumulate(v);
    }
    u32 total = 0;
    for (u32 v : bins) {
        total += v;
    }
    expect(total == w * h && total == hist.sampleCount(), "histogram_reference counts every pixel");
    u32 cumulative = 0;
    u32 median = 0;
    for (u32 i = 0; i < bins.size(); ++i) {
        cumulative += bins[i];
        if (cumulative >= (total + 1u) / 2u) {
            median = i;
            break;
        }
    }
    expect(std::fabs(LuminanceHistogram::binCenterLuminance(median, hp) - hist.percentileLuminance(0.5f)) < 1e-6f,
           "histogram_reference median bin == LuminanceHistogram::percentileLuminance(0.5)");

    // LUT branch == look::kernels::GradeLutKernel.
    look::LookParamBlock lb = look::look_default_params();
    lb.set(look::LookParam::GradeSaturation, 1.3f);
    lb.set(look::LookParam::GradeTemperatureK, 5200.f);
    const look::LookResolved look = look::look_resolve(lb);
    look::Lut3D lut;
    look::lut_generate_grade(look, 32, lut);
    PostGpuSettings ls{};
    ls.lut = true;
    ls.toneMapper = ToneMapper::Neutral;
    f32 lutWorst = 0.f;
    for (u32 i = 0; i < w * h; ++i) {
        const Vec3 v = apply_tone_map(hdr[i], ToneMapper::Neutral);
        const Vec3 base = look::kernels::min3(look::kernels::max3(v, 0.f), 1.f);
        const Vec3 graded = look::kernels::srgb_decode3(look::kernels::saturate3(
            look::kernels::lut_sample_tetrahedral(lut.data.data(), lut.size, look::kernels::srgb_encode3(base))));
        const Vec3 expected = finalize_display(graded + (look::kernels::max3(v, 0.f) - base), true);
        lutWorst = std::max(lutWorst, maxAbs(display_reference(ls, hdr[i], 1.f, i % w, i / w, w, h, &lut), expected));
    }
    expect(lutWorst == 0.f, "display_reference LUT stage == look GradeLutKernel");
}

// --- api -------------------------------------------------------------------------------------------
void testApi() {
    PostStackGpu post;
    PostStackGpuDesc desc{};
    expect(!post.init(desc), "init without a device fails");
    expect(!post.valid(), "not valid");
    rg::Graph graph;
    const PostGraphRefs refs = post.importInto(graph);
    expect(!refs.work.valid() && !refs.output.valid(), "no refs without init");
    post.addPasses(graph, refs, {});
    expect(graph.passCount() == 0u, "no passes without init");
    PostGpuSettings s{};
    expect(!post.beginFrame(1u, s, {}), "beginFrame without init fails");
    const PostCapabilities caps = queryPostCapabilities(nullptr);
    expect(!caps.post, "no capabilities without a device");
    std::printf("api: capabilities without a device: %s\n", caps.reason);
}

} // namespace

int main(int argc, char** argv) {
    const std::string suite = argc > 1 ? argv[1] : "all";
    const bool all = suite == "all";
    if (all || suite == "layout") {
        testLayout();
    }
    if (all || suite == "agx") {
        testAgx();
    }
    if (all || suite == "settings") {
        testSettings();
    }
    if (all || suite == "reference") {
        testReference();
    }
    if (all || suite == "api") {
        testApi();
    }
    if (g_failures != 0) {
        std::fprintf(stderr, "FAIL: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("PASS %s\n", suite.c_str());
    return 0;
}
