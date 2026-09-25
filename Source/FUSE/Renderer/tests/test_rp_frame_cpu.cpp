// Frame composer: CPU gates (no device; also run in the stub tree).
//
//   layout     FrameConstants / FramePush sizes; the GLSL and Slang mirrors of FrameConstants (struct FcFrame in
//              shaders/frame/fc_common.*) have the same fields, order and offsets; the push block mirrors FramePush.
//   reference  the CPU twins of the composer kernel: frame_unproject inverts a view-projection (world point of a
//              pixel at its projected depth within 1e-4 relative), frame_view_distance (sky pixels take
//              skyDistance, geometry pixels the eye distance), frame_resolve_texel (splats off: bits kept; on:
//              rgb x T + c; alpha 1), frame_pack_visibility (clamped .x), frame_restir_texel (background bits
//              kept, geometry + albedo x DI), frame_oct_decode (inverse of the G-buffer octahedral encode),
//              frame_reflect_texel (background kept; F = F0 at normal incidence, -> 1 at grazing; metal F0 =
//              albedo); the SSFX jitter convention (the composer's proj[8] += 2 jx / w, proj[9] -= 2 jy / h on
//              the unflipped SSFX projection puts every view point on the same pixel as the jittered
//              visibility-buffer projection, temporal::jitter_view_proj).
//   api        a FrameComposer without a device / with an invalid description fails init with a reason and
//              records nothing; default FrameSettings (every stage on except splats, ReSTIR and frame
//              generation); stage bits are disjoint.
#include <fuse/renderer/frame/frame_composer.hpp>
#include <fuse/renderer/frame/frame_types.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/ssfx_gpu/ssfx_gpu_reference.hpp>
#include <fuse/renderer/temporal/temporal_types.hpp>

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

using namespace fuse::renderer::frame;
using fuse::f32;
using fuse::f64;
using fuse::u32;
using fuse::u64;

int g_failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

// --- layout ----------------------------------------------------------------------------------------
struct Field {
    const char* name;
    size_t offset;
};
#define FC_FIELD(n) Field{#n, offsetof(FrameConstants, n)}
const Field kFields[] = {
    FC_FIELD(atmosphere),   FC_FIELD(background), FC_FIELD(distance),      FC_FIELD(clouds),        FC_FIELD(splats),
    FC_FIELD(denoised),     FC_FIELD(visibility), FC_FIELD(restirDi),      FC_FIELD(restirAlbedo),  FC_FIELD(reflection),
    FC_FIELD(width),        FC_FIELD(height),     FC_FIELD(inColor),       FC_FIELD(inDepth),       FC_FIELD(outColor),
    FC_FIELD(outResolve),   FC_FIELD(flags),      FC_FIELD(gbufferNormal), FC_FIELD(gbufferAlbedo), FC_FIELD(gbufferRoughMetal),
    FC_FIELD(reserved1),    FC_FIELD(invViewProj), FC_FIELD(cameraPos),    FC_FIELD(skyDistance),   FC_FIELD(reserved2),
};
#undef FC_FIELD

bool readText(const std::string& path, std::string& text) {
    std::ifstream file(path);
    if (!file) {
        return false;
    }
    std::stringstream ss;
    ss << file.rdbuf();
    text = ss.str();
    return true;
}

/// Parses `struct <name> { ... };` of a shader source: (field name, scalar-layout offset) per field.
bool parseShaderStruct(const std::string& text, const char* header, std::vector<size_t>& offsets, size_t& size,
                       std::vector<std::string>& names) {
    const size_t begin = text.find(header);
    const size_t end = text.find('}', begin);
    if (begin == std::string::npos || end == std::string::npos) {
        return false;
    }
    const size_t start = begin + std::strlen(header);
    std::istringstream body(text.substr(start, end - start));
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
        offsets.push_back(offset);
        offset += bytes * count;
    }
    size = offset;
    return true;
}

void testLayout() {
    expect(sizeof(FrameConstants) == 224u, "FrameConstants is 224 bytes");
    expect(sizeof(FramePush) == 16u, "FramePush is 16 bytes");
    expect(offsetof(FramePush, mode) == 8u, "FramePush::mode at 8");
    const size_t fieldCount = sizeof(kFields) / sizeof(kFields[0]);
    for (const char* lang : {"glsl", "slang"}) {
        const std::string path = std::string(FUSE_RP_FRAME_SHADER_DIR) + "/fc_common." + lang;
        std::string text;
        expect(readText(path, text), "fc_common source readable");
        std::vector<size_t> offsets;
        std::vector<std::string> names;
        size_t size = 0;
        const bool parsed = parseShaderStruct(text, "struct FcFrame {", offsets, size, names);
        expect(parsed, "fc_common struct FcFrame parsed");
        if (!parsed) {
            continue;
        }
        bool same = offsets.size() == fieldCount && size == sizeof(FrameConstants);
        for (size_t i = 0; same && i < fieldCount; ++i) {
            same = names[i] == kFields[i].name && offsets[i] == kFields[i].offset;
            if (!same) {
                std::fprintf(stderr, "  %s field %zu: shader %s @%zu vs C++ %s @%zu\n", lang, i, names[i].c_str(), offsets[i],
                             kFields[i].name, kFields[i].offset);
            }
        }
        std::printf("layout: fc_common.%s FcFrame %zu fields, %zu bytes\n", lang, offsets.size(), size);
        expect(same, "shader FcFrame == FrameConstants (names, order, offsets, size)");
        std::vector<size_t> pushOffsets;
        std::vector<std::string> pushNames;
        size_t pushSize = 0;
        const char* pushHeader = std::strcmp(lang, "glsl") == 0 ? "uniform FcPush {" : "struct FcPush {";
        const bool pushParsed = parseShaderStruct(text, pushHeader, pushOffsets, pushSize, pushNames);
        expect(pushParsed && pushSize == sizeof(FramePush) && pushNames.size() == 3u && pushNames[0] == "frame" &&
                   pushNames[1] == "mode" && pushOffsets[1] == offsetof(FramePush, mode),
               "shader FcPush == FramePush");
        // Mode / flag constants.
        const bool glsl = std::strcmp(lang, "glsl") == 0;
        const char* modes[6][2] = {{"FC_MODE_SKY 0u", "kFcModeSky = 0u"},
                                   {"FC_MODE_GATHER 1u", "kFcModeGather = 1u"},
                                   {"FC_MODE_RESOLVE 2u", "kFcModeResolve = 2u"},
                                   {"FC_MODE_SHADOW_PACK 3u", "kFcModeShadowPack = 3u"},
                                   {"FC_MODE_RESTIR 4u", "kFcModeRestir = 4u"},
                                   {"FC_MODE_REFLECT 5u", "kFcModeReflect = 5u"}};
        for (const auto& m : modes) {
            expect(text.find(glsl ? m[0] : m[1]) != std::string::npos, "shader mode constant == FrameKernelMode");
        }
        const char* flags[5][2] = {{"FC_FLAG_SKY (1u << 0)", "kFcFlagSky = 1u << 0"},
                                   {"FC_FLAG_AERIAL (1u << 1)", "kFcFlagAerial = 1u << 1"},
                                   {"FC_FLAG_SUN_DISK (1u << 2)", "kFcFlagSunDisk = 1u << 2"},
                                   {"FC_FLAG_CLOUDS (1u << 3)", "kFcFlagClouds = 1u << 3"},
                                   {"FC_FLAG_SPLATS (1u << 4)", "kFcFlagSplats = 1u << 4"}};
        for (const auto& f : flags) {
            expect(text.find(glsl ? f[0] : f[1]) != std::string::npos, "shader flag constant == FrameFlag");
        }
    }
    expect(kFrameFlagSky == 1u && kFrameFlagAerial == 2u && kFrameFlagSunDisk == 4u && kFrameFlagClouds == 8u &&
               kFrameFlagSplats == 16u,
           "FrameFlag values");
}

// --- reference -------------------------------------------------------------------------------------
void mul4(const f64 a[16], const f64 b[16], f64 out[16]) {
    for (u32 c = 0; c < 4u; ++c) {
        for (u32 r = 0; r < 4u; ++r) {
            f64 s = 0.0;
            for (u32 k = 0; k < 4u; ++k) {
                s += a[k * 4u + r] * b[c * 4u + k];
            }
            out[c * 4u + r] = s;
        }
    }
}

void testReference() {
    // Camera at (1, 2, 3) looking down -z, fov 60 degrees, 64 x 48, near 0.1, far 100: viewProj and its inverse.
    const u32 w = 64, h = 48;
    const f64 f = 1.0 / std::tan(0.5 * 1.0471975511965976);
    const f64 aspect = static_cast<f64>(w) / h;
    const f64 zn = 0.1, zf = 100.0;
    f64 proj[16] = {};
    proj[0] = f / aspect;
    proj[5] = -f;
    proj[10] = zf / (zn - zf);
    proj[11] = -1.0;
    proj[14] = zn * zf / (zn - zf);
    f64 view[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -1, -2, -3, 1};
    f64 vp[16];
    mul4(proj, view, vp);
    // Inverse: translate(1, 2, 3) x inverse(proj).
    f64 invProj[16] = {};
    invProj[0] = aspect / f;
    invProj[5] = -1.0 / f;
    invProj[11] = 1.0 / proj[14];
    invProj[14] = -1.0;
    invProj[15] = proj[10] / proj[14];
    const f64 invView[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 1, 2, 3, 1};
    f64 inv[16];
    mul4(invView, invProj, inv);
    FrameConstants c{};
    c.width = w;
    c.height = h;
    for (u32 i = 0; i < 16u; ++i) {
        c.invViewProj[i] = static_cast<f32>(inv[i]);
    }
    c.cameraPos[0] = 1.f;
    c.cameraPos[1] = 2.f;
    c.cameraPos[2] = 3.f;
    c.skyDistance = 1.0e30f;
    f64 worst = 0.0;
    u32 checked = 0;
    for (u32 y = 0; y < h; y += 5u) {
        for (u32 x = 0; x < w; x += 7u) {
            for (const f64 dist : {0.5, 3.0, 40.0}) {
                // World point on the pixel ray at view depth `dist`.
                const f64 nx = (x + 0.5) / w * 2.0 - 1.0;
                const f64 ny = (y + 0.5) / h * 2.0 - 1.0;
                const f64 vx = nx * aspect / f * dist;
                const f64 vy = -ny / f * dist;
                const f64 world[3] = {vx + 1.0, vy + 2.0, -dist + 3.0};
                const f64 clipZ = vp[2] * world[0] + vp[6] * world[1] + vp[10] * world[2] + vp[14];
                const f64 clipW = vp[3] * world[0] + vp[7] * world[1] + vp[11] * world[2] + vp[15];
                const f32 depth = static_cast<f32>(clipZ / clipW);
                f32 p[3];
                frame_unproject(c, x, y, depth, p);
                const f64 len = std::sqrt(vx * vx + vy * vy + dist * dist);
                for (u32 k = 0; k < 3u; ++k) {
                    worst = std::max(worst, std::fabs(p[k] - world[k]) / len);
                }
                const f32 d = frame_view_distance(c, x, y, depth);
                worst = std::max(worst, std::fabs(d - len) / len);
                ++checked;
            }
        }
    }
    std::printf("reference: frame_unproject / frame_view_distance: %u samples, worst relative error %.3g\n", checked, worst);
    expect(worst < 1e-3, "unproject / distance within 1e-3 relative (f32 depth quantisation at 40 m)");
    expect(frame_view_distance(c, 3, 4, 1.f) == 1.0e30f, "sky pixels take skyDistance");
    const f32 base[4] = {0.25f, 1.5f, 3.f, 0.5f};
    const f32 splat[4] = {0.1f, 0.2f, 0.3f, 0.5f};
    f32 out[4];
    c.flags = 0u;
    frame_resolve_texel(c, base, splat, out);
    expect(out[0] == base[0] && out[1] == base[1] && out[2] == base[2] && out[3] == 1.f, "resolve without splats keeps rgb");
    c.flags = kFrameFlagSplats;
    frame_resolve_texel(c, base, splat, out);
    expect(out[0] == 0.25f * 0.5f + 0.1f && out[1] == 1.5f * 0.5f + 0.2f && out[2] == 3.f * 0.5f + 0.3f && out[3] == 1.f,
           "resolve with splats: rgb x T + c");
    const f32 transparent[4] = {0.f, 0.f, 0.f, 1.f};
    frame_resolve_texel(c, base, transparent, out);
    expect(out[0] == base[0] && out[1] == base[1] && out[2] == base[2], "an empty splat texel (T = 1, c = 0) keeps the bits");
    const f32 d0[4] = {-0.5f, 0.f, 0.f, 0.f};
    const f32 d1[4] = {0.25f, 9.f, 9.f, 9.f};
    const f32 d2[4] = {3.f, 0.f, 0.f, 0.f};
    expect(frame_pack_visibility(d0) == 0.f && frame_pack_visibility(d1) == 0.25f && frame_pack_visibility(d2) == 1.f,
           "shadow pack clamps .x to [0, 1]");

    // Restir: background bits kept, geometry + albedo x DI, alpha kept.
    {
        const f32 in[4] = {0.5f, 0.25f, 1.f, 0.75f};
        const f32 albedo[4] = {0.5f, 1.f, 0.25f, 1.f};
        const f32 di[4] = {2.f, 0.5f, 4.f, 0.f};
        f32 o[4];
        frame_restir_texel(in, 1.f, albedo, di, o);
        expect(std::memcmp(o, in, sizeof(o)) == 0, "restir keeps background pixels");
        frame_restir_texel(in, 0.5f, albedo, di, o);
        expect(o[0] == 1.5f && o[1] == 0.75f && o[2] == 2.f && o[3] == 0.75f, "restir adds albedo x DI on geometry");
    }
    // Octahedral decode: inverse of the signed octahedral encode on a set of directions.
    {
        f64 worst = 0.0;
        for (u32 i = 0; i < 64u; ++i) {
            const f32 th = 0.1f + 3.0f * static_cast<f32>(i) / 64.f;
            const f32 ph = 0.37f * static_cast<f32>(i);
            f32 n[3] = {std::sin(th) * std::cos(ph), std::sin(th) * std::sin(ph), std::cos(th)};
            const f32 s = std::fabs(n[0]) + std::fabs(n[1]) + std::fabs(n[2]);
            f32 ox = n[0] / s;
            f32 oy = n[1] / s;
            if (n[2] < 0.f) {
                const f32 x = ox;
                const f32 y = oy;
                ox = (1.f - std::fabs(y)) * (x >= 0.f ? 1.f : -1.f);
                oy = (1.f - std::fabs(x)) * (y >= 0.f ? 1.f : -1.f);
            }
            f32 d[3];
            frame_oct_decode(ox, oy, d);
            for (u32 k = 0; k < 3u; ++k) {
                worst = std::max(worst, static_cast<f64>(std::fabs(d[k] - n[k])));
            }
        }
        std::printf("reference: frame_oct_decode worst component error %.3g over 64 directions\n", worst);
        expect(worst < 1e-5, "octahedral decode inverts the G-buffer encode");
    }
    // Reflect: background kept; normal incidence -> F0; grazing -> ~1; metal F0 = albedo.
    {
        FrameConstants r = c; // camera at (1, 2, 3) looking down -z
        const f32 in[4] = {0.1f, 0.2f, 0.3f, 1.f};
        const f32 refl[4] = {2.f, 2.f, 2.f, 5.f};
        const f32 rt0[4] = {0.f, 0.f, 0.f, 1.f};   // +z normal (towards the camera)
        const f32 rt1[4] = {0.5f, 0.25f, 1.f, 1.f};
        const f32 dielectric[4] = {0.5f, 0.f, 0.f, 0.f};
        const f32 metal[4] = {0.5f, 1.f, 0.f, 0.f};
        f32 o[4];
        frame_reflect_texel(r, 32, 24, 1.f, in, rt0, rt1, dielectric, refl, o);
        expect(std::memcmp(o, in, sizeof(o)) == 0, "reflect keeps background pixels");
        // Pixel near the centre at 3 m: N.V ~ 1 -> F ~ F0.
        const f64 zc = 3.0;
        const f32 dc = static_cast<f32>((vp[10] * (3.0 - zc) + vp[14]) / (vp[11] * (3.0 - zc) + vp[15]));
        frame_reflect_texel(r, 32, 24, dc, in, rt0, rt1, dielectric, refl, o);
        const bool dielectricOk = std::fabs(o[0] - (0.1f + 0.04f * 2.f)) < 1e-4f && std::fabs(o[1] - (0.2f + 0.04f * 2.f)) < 1e-4f;
        frame_reflect_texel(r, 32, 24, dc, in, rt0, rt1, metal, refl, o);
        const bool metalOk = std::fabs(o[0] - (0.1f + 0.5f * 2.f)) < 1e-3f && std::fabs(o[2] - (0.3f + 1.f * 2.f)) < 1e-3f &&
                             o[3] == 1.f;
        const f32 grazing[4] = {1.f, 0.f, 0.f, 1.f}; // +x normal, perpendicular to the view axis
        frame_reflect_texel(r, 32, 24, dc, in, grazing, rt1, dielectric, refl, o);
        const bool grazingOk = o[0] > 0.1f + 0.9f * 2.f;
        std::printf("reference: frame_reflect_texel dielectric %s, metal %s, grazing %s\n", dielectricOk ? "ok" : "BAD",
                    metalOk ? "ok" : "BAD", grazingOk ? "ok" : "BAD");
        expect(dielectricOk && metalOk && grazingOk, "reflect weight is Schlick F(F0, N.V)");
    }
    // SSFX jitter convention (FrameComposer::beginFrame): the unflipped SSFX projection with proj[8] += 2 jx / w and
    // proj[9] -= 2 jy / h projects view points onto the same pixels as the jittered (y-flipped) G-buffer projection.
    {
        const f32 jx = 0.3125f, jy = -0.4375f;
        f32 flipped[16] = {};
        f32 ssfx[16] = {};
        for (u32 i = 0; i < 16u; ++i) {
            flipped[i] = static_cast<f32>(proj[i]);
            ssfx[i] = static_cast<f32>(proj[i]);
        }
        ssfx[5] = -ssfx[5];
        f32 jittered[16];
        fuse::renderer::temporal::jitter_view_proj(flipped, jx, jy, w, h, jittered);
        ssfx[8] += 2.f * jx / static_cast<f32>(w);
        ssfx[9] -= 2.f * jy / static_cast<f32>(h);
        fuse::ssfx::SsfxCamera cam{};
        const bool camOk = fuse::renderer::ssfx_gpu::camera_from_projection(ssfx, w, h, cam);
        f64 worstPx = 0.0;
        const f32 pts[4][3] = {{0.3f, 0.2f, -2.f}, {-1.1f, 0.7f, -5.f}, {2.f, -1.5f, -9.f}, {0.f, 0.f, -1.f}};
        for (const auto& p : pts) {
            const f32 cx = jittered[0] * p[0] + jittered[4] * p[1] + jittered[8] * p[2] + jittered[12];
            const f32 cy = jittered[1] * p[0] + jittered[5] * p[1] + jittered[9] * p[2] + jittered[13];
            const f32 cw = jittered[3] * p[0] + jittered[7] * p[1] + jittered[11] * p[2] + jittered[15];
            const f64 px = (cx / cw * 0.5 + 0.5) * w;
            const f64 py = (cy / cw * 0.5 + 0.5) * h;
            f32 sx = 0.f, sy = 0.f;
            // SSFX view space: x right, y down, z forward.
            cam.project(fuse::math::Vec3{p[0], -p[1], -p[2]}, sx, sy);
            worstPx = std::max(worstPx, std::max(std::fabs(sx - px), std::fabs(sy - py)));
        }
        std::printf("reference: SSFX jitter convention, worst pixel mismatch %.3g\n", worstPx);
        expect(camOk && worstPx < 1e-3, "the jittered SSFX projection matches the jittered G-buffer pixels");
    }
}

// --- api -------------------------------------------------------------------------------------------
void testApi() {
    FrameComposer composer;
    FrameComposerDesc desc{};
    expect(!composer.init(desc), "init without a device fails");
    expect(!composer.valid() && std::strlen(composer.reason()) > 0u, "a failed init names a reason");
    expect(composer.available() == 0u, "nothing is available after a failed init");
    fuse::renderer::rg::Graph graph;
    FrameDesc fd{};
    FrameSettings fs{};
    expect(!composer.beginFrame(fd, fs), "beginFrame on an uninitialised composer fails");
    const FrameGraphOutputs outs = composer.addFrame(graph);
    expect(!outs.output.valid() && graph.passCount() == 0u, "addFrame without beginFrame records nothing");
    expect(composer.commitScene(), "commitScene is a no-op without a TLAS");
    composer.collectRetired(~0ull);
    expect(fs.vsm && fs.ddgi && fs.ddgiAtmosphereSky && fs.rtShadows && fs.denoise && fs.ssfx && fs.sky && fs.aerialPerspective &&
               fs.fog && fs.clouds && !fs.splats && fs.post && fs.upscaler == FrameUpscaler::Taau && fs.rtReflections &&
               fs.forward && !fs.restir && !fs.frameGen,
           "default FrameSettings: every stage on except splats, ReSTIR and frame generation, TAAU");
    FrameComposer probe;
    expect(!probe.setRestirLights(nullptr, nullptr, 0u) && probe.restirDiOffset() == 0u && probe.reflectionOffset() == 0u,
           "ReSTIR / reflection inspection without a device");
    const u32 stages[] = {kStageScene, kStageVsm,  kStageTlas,  kStageRtShadows, kStageDenoise, kStageDdgi,
                          kStageLighting, kStageSsfx, kStageAtmosphere, kStageSky, kStageFog, kStageClouds,
                          kStageSplats, kStageResolve, kStageTaau, kStageFsr3, kStagePost, kStageAerial,
                          kStageRestir, kStageRtReflections, kStageForward, kStageFrameGen};
    u32 all = 0;
    bool disjoint = true;
    for (const u32 s : stages) {
        disjoint = disjoint && (all & s) == 0u && s != 0u;
        all |= s;
    }
    expect(disjoint, "FrameStage bits are distinct");
    std::printf("api: init failure reason \"%s\"\n", composer.reason());
}

} // namespace

int main(int argc, char** argv) {
    const std::string suite = argc > 1 ? argv[1] : "all";
    const bool all = suite == "all";
    if (all || suite == "layout") {
        testLayout();
    }
    if (all || suite == "reference") {
        testReference();
    }
    if (all || suite == "api") {
        testApi();
    }
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
