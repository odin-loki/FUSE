// WP-9.2 3D Gaussian splatting CPU gates (stub-safe). Lavapipe gates: test_rp_gsplat.cpp.
//
//   layout     GsFrameConstants / GsPush / GsProjected / GsSplat sizes; the GLSL and Slang mirrors of
//              GsFrameConstants (struct GsFrame in shaders/gsplat/gs_common.*): names, order, offsets, size
//   ply        .ply loader: binary LE round trip at SH degrees 0..3 (activations inverted by the writer), an
//              ascii file with extra properties / elements and free property order, big endian, double / uchar
//              types, and the error paths (missing property, bad f_rest count, truncated body)
//   reference  CPU reference invariants: an isotropic splat on the axis (analytic mean, conic, radius, centre
//              pixel colour and transmittance), SH degree 0 == DC x C0 + 0.5, the whole pipeline on a random
//              scene (offsets = prefix sum, sorted keys, padding, ranges cover exactly the emitted entries, the
//              image equals a brute-force per-pixel depth sort), capacity overflow, depth composite (an occluder
//              in front of everything gives (0, T = 1); one behind everything changes nothing), early termination
//   api        sort key bits (even radix pass count), constant resolution errors, GsplatRenderer::init without a
//              device fails
#include <fuse/renderer/gsplat/gsplat.hpp>
#include <fuse/renderer/gsplat/gsplat_ply.hpp>
#include <fuse/renderer/gsplat/gsplat_reference.hpp>

#include "test_rp_gsplat_scene.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

using namespace fuse::renderer::gsplat;
using fuse::f32;
using fuse::f64;
using fuse::u32;
using fuse::u64;
using fuse::u8;
using fuse::usize;

namespace {
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
#define GS_FIELD(n) Field{#n, offsetof(GsFrameConstants, n)}
const Field kFields[] = {
    GS_FIELD(splats), GS_FIELD(projected), GS_FIELD(offsets), GS_FIELD(keys), GS_FIELD(values), GS_FIELD(ranges),
    GS_FIELD(counters), Field{"output_", offsetof(GsFrameConstants, output)}, GS_FIELD(splatCount), GS_FIELD(capacity),
    GS_FIELD(width), GS_FIELD(height), GS_FIELD(tilesX), GS_FIELD(tilesY), GS_FIELD(tileCount), GS_FIELD(depthHandle),
    GS_FIELD(flags), GS_FIELD(shDegree), GS_FIELD(reserved0), GS_FIELD(reserved1), GS_FIELD(view), GS_FIELD(camPos),
    GS_FIELD(fx), GS_FIELD(fy), GS_FIELD(cx), GS_FIELD(cy), GS_FIELD(nearZ), GS_FIELD(tanFovX), GS_FIELD(tanFovY),
    GS_FIELD(lowPass), GS_FIELD(depthA), GS_FIELD(depthB), GS_FIELD(alphaMin), GS_FIELD(transmittanceMin),
};
#undef GS_FIELD

bool parseShaderStruct(const std::string& path, const char* header, std::vector<size_t>& offsets, size_t& size,
                       std::vector<std::string>& names) {
    std::ifstream file(path);
    if (!file) {
        return false;
    }
    std::stringstream ss;
    ss << file.rdbuf();
    const std::string text = ss.str();
    const size_t begin = text.find(header);
    const size_t end = text.find("};", begin);
    if (begin == std::string::npos || end == std::string::npos) {
        return false;
    }
    const size_t skip = std::strlen(header);
    std::istringstream body(text.substr(begin + skip, end - begin - skip));
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
    expect(sizeof(GsFrameConstants) == 224u, "GsFrameConstants is 224 bytes");
    expect(sizeof(GsPush) == 16u, "GsPush is 16 bytes");
    expect(sizeof(GsProjected) == 64u && offsetof(GsProjected, conic) == 16u && offsetof(GsProjected, color) == 32u &&
               offsetof(GsProjected, radius) == 44u && offsetof(GsProjected, rect) == 48u,
           "GsProjected == the kernels' 4 x uvec4 record");
    expect(sizeof(GsSplat) == 240u && offsetof(GsSplat, scale) == 16u && offsetof(GsSplat, rotation) == 32u &&
               offsetof(GsSplat, sh) == 48u,
           "GsSplat == the kernels' 60-float record (pos+opacity, scale, rotation, sh at 12)");
    const size_t fieldCount = sizeof(kFields) / sizeof(kFields[0]);
    for (const char* lang : {"glsl", "slang"}) {
        const std::string path = std::string(FUSE_RP_GSPLAT_SHADER_DIR) + "/gs_common." + lang;
        std::vector<size_t> offsets;
        std::vector<std::string> names;
        size_t size = 0;
        const bool parsed = parseShaderStruct(path, "struct GsFrame {", offsets, size, names);
        expect(parsed, "gs_common struct GsFrame parsed");
        if (!parsed) {
            continue;
        }
        bool same = offsets.size() == fieldCount && size == sizeof(GsFrameConstants);
        for (size_t i = 0; same && i < fieldCount; ++i) {
            same = names[i] == kFields[i].name && offsets[i] == kFields[i].offset;
            if (!same) {
                std::fprintf(stderr, "  %s field %zu: shader %s @%zu vs C++ %s @%zu\n", lang, i, names[i].c_str(),
                             offsets[i], kFields[i].name, kFields[i].offset);
            }
        }
        std::printf("layout: gs_common.%s GsFrame %zu fields, %zu bytes\n", lang, offsets.size(), size);
        expect(same, "shader GsFrame == GsFrameConstants (names, order, offsets, size)");
    }
}

// --- ply -------------------------------------------------------------------------------------------
bool near(f32 a, f32 b, f32 rel) { return std::fabs(a - b) <= rel * std::max(1.f, std::max(std::fabs(a), std::fabs(b))); }

void testPly() {
    for (u32 degree = 0; degree <= 3u; ++degree) {
        const GsAsset src = gs_test::makeScene(64, degree, 11u + degree);
        const std::vector<u8> bytes = gs_write_ply(src);
        GsAsset back;
        const GsPlyResult r = gs_load_ply(bytes.data(), bytes.size(), back);
        expect(r.ok, "binary LE round trip loads");
        expect(back.shDegree == degree && back.splats.size() == src.splats.size(), "degree / count survive");
        bool same = r.ok && back.splats.size() == src.splats.size();
        const u32 coeffs = (degree + 1u) * (degree + 1u);
        for (usize i = 0; same && i < src.splats.size(); ++i) {
            const GsSplat& a = src.splats[i];
            const GsSplat& b = back.splats[i];
            for (u32 k = 0; k < 3u; ++k) {
                same = same && a.position[k] == b.position[k] && near(a.scale[k], b.scale[k], 1e-5f);
            }
            for (u32 k = 0; k < 4u; ++k) {
                same = same && near(a.rotation[k], b.rotation[k], 1e-5f);
            }
            same = same && near(a.opacity, b.opacity, 1e-5f);
            for (u32 k = 0; k < kGsShCoeffs * 3u; ++k) {
                same = same && (k < coeffs * 3u ? a.sh[k] == b.sh[k] : b.sh[k] == 0.f);
            }
        }
        std::printf("ply: degree %u round trip (%zu bytes) %s\n", degree, bytes.size(), same ? "ok" : "MISMATCH");
        expect(same, "round trip preserves the splats (activations within 1e-5)");
    }
    // ASCII: free property order, extra vertex properties, a face element with a list, degree 0, doubles.
    const char* ascii =
        "ply\nformat ascii 1.0\ncomment test\nelement vertex 2\n"
        "property double opacity\nproperty float x\nproperty float y\nproperty float z\nproperty uchar red\n"
        "property float rot_1\nproperty float rot_0\nproperty float rot_2\nproperty float rot_3\n"
        "property float scale_0\nproperty float scale_1\nproperty float scale_2\n"
        "property float f_dc_2\nproperty float f_dc_1\nproperty float f_dc_0\n"
        "element face 1\nproperty list uchar int vertex_indices\nend_header\n"
        "0 1 2 3 255 0 2 0 0 0 -1 -2 0.3 0.2 0.1\n"
        "2.5 -1 -2 -3 7 0 0 0 0 0 0 0 1 1 1\n"
        "3 0 1 1\n";
    GsAsset a;
    GsPlyResult r = gs_load_ply(reinterpret_cast<const u8*>(ascii), std::strlen(ascii), a);
    expect(r.ok && a.splats.size() == 2u && a.shDegree == 0u, "ascii file loads (degree 0)");
    if (r.ok && a.splats.size() == 2u) {
        const GsSplat& s = a.splats[0];
        expect(s.position[0] == 1.f && s.position[1] == 2.f && s.position[2] == 3.f, "ascii position");
        expect(near(s.opacity, 0.5f, 1e-6f), "sigmoid(0) = 0.5");
        expect(near(s.scale[0], 1.f, 1e-6f) && near(s.scale[1], std::exp(-1.f), 1e-6f) &&
                   near(s.scale[2], std::exp(-2.f), 1e-6f),
               "scale = exp(scale_i)");
        expect(s.rotation[0] == 1.f && s.rotation[1] == 0.f, "rotation (2, 0, 0, 0) normalised to identity");
        expect(s.sh[0] == 0.1f && s.sh[1] == 0.2f && s.sh[2] == 0.3f, "f_dc by name, not by order");
        expect(a.splats[1].rotation[0] == 1.f && near(a.splats[1].opacity, 1.f / (1.f + std::exp(-2.5f)), 1e-6f),
               "zero quaternion -> identity; double opacity");
    }
    // Big endian, one splat, degree 1.
    {
        GsAsset one = gs_test::makeScene(1, 1, 5u);
        std::vector<u8> le = gs_write_ply(one);
        const std::string le_tag = "binary_little_endian";
        std::string text(le.begin(), le.end());
        const size_t at = text.find(le_tag);
        const size_t headerEnd = text.find("end_header\n") + 11u;
        std::vector<u8> be(le.begin(), le.begin() + static_cast<std::ptrdiff_t>(headerEnd));
        std::string header(be.begin(), be.end());
        header.replace(at, le_tag.size(), "binary_big_endian");
        be.assign(header.begin(), header.end());
        for (size_t i = headerEnd; i + 4u <= le.size(); i += 4u) {
            be.insert(be.end(), {le[i + 3u], le[i + 2u], le[i + 1u], le[i]});
        }
        GsAsset back;
        r = gs_load_ply(be.data(), be.size(), back);
        expect(r.ok && back.splats.size() == 1u && back.shDegree == 1u && back.splats[0].position[0] == one.splats[0].position[0] &&
                   back.splats[0].sh[5] == one.splats[0].sh[5],
               "big-endian file loads");
    }
    // Errors.
    const char* missing = "ply\nformat ascii 1.0\nelement vertex 1\nproperty float x\nproperty float y\nend_header\n1 2\n";
    r = gs_load_ply(reinterpret_cast<const u8*>(missing), std::strlen(missing), a);
    expect(!r.ok && !r.error.empty(), "missing 3DGS properties rejected");
    std::string badRest = "ply\nformat binary_little_endian 1.0\nelement vertex 1\n";
    for (const char* n : {"x", "y", "z", "f_dc_0", "f_dc_1", "f_dc_2", "f_rest_0", "f_rest_1", "opacity", "scale_0",
                          "scale_1", "scale_2", "rot_0", "rot_1", "rot_2", "rot_3"}) {
        badRest += std::string("property float ") + n + "\n";
    }
    badRest += "end_header\n" + std::string(16u * 4u, '\0');
    r = gs_load_ply(reinterpret_cast<const u8*>(badRest.data()), badRest.size(), a);
    expect(!r.ok, "f_rest count 2 rejected");
    std::vector<u8> truncated = gs_write_ply(gs_test::makeScene(4, 3, 3u));
    truncated.resize(truncated.size() - 7u);
    r = gs_load_ply(truncated.data(), truncated.size(), a);
    expect(!r.ok, "truncated binary body rejected");
    r = gs_load_ply_file("/nonexistent/fuse_gsplat.ply", a);
    expect(!r.ok, "missing file rejected");
}

// --- reference -------------------------------------------------------------------------------------
GsFrameConstants frameFor(const GsCamera& cam, u32 w, u32 h, u32 splats, u32 degree, u32 capacity, bool depth) {
    GsFrameConstants f{};
    const bool ok = gs_resolve_constants(cam, GsSettings{}, w, h, splats, degree, capacity, depth, f);
    expect(ok, "constants resolve");
    return f;
}

/// Brute-force oracle of one pixel: every splat whose rect covers the pixel's tile, sorted by (view z, index).
void bruteForcePixel(const std::vector<GsProjected>& proj, u32 x, u32 y, f32 scene, const GsFrameConstants& f,
                     f32 (&out)[4]) {
    const u32 tx = x / kGsTile;
    const u32 ty = y / kGsTile;
    std::vector<u32> ids;
    for (u32 i = 0; i < proj.size(); ++i) {
        const GsProjected& p = proj[i];
        if (p.radius > 0u && tx >= p.rect[0] && tx < p.rect[2] && ty >= p.rect[1] && ty < p.rect[3]) {
            ids.push_back(i);
        }
    }
    std::stable_sort(ids.begin(), ids.end(), [&](u32 a, u32 b) { return proj[a].viewZ < proj[b].viewZ; });
    gs_raster_pixel(proj.data(), ids.data(), 0u, static_cast<u32>(ids.size()), x, y, scene, f, out);
}

void testReference() {
    // Isotropic splat on the optical axis: sigma 0.1 at z = 4, focal 200 -> 5 px std. dev.
    {
        GsCamera cam{};
        cam.fx = cam.fy = 200.f;
        cam.cx = 32.f;
        cam.cy = 32.f;
        cam.view[11] = 4.f; // camera at z = -4 looking down +Z
        cam.depthA = 1.f;
        cam.depthB = -0.1f;
        GsSplat s{};
        s.scale[0] = s.scale[1] = s.scale[2] = 0.1f;
        s.opacity = 0.8f;
        s.sh[0] = 1.f;
        s.sh[1] = 0.f;
        s.sh[2] = -1.f;
        const GsFrameConstants f = frameFor(cam, 64, 64, 1, 0, 64, false);
        const GsProjected p = gs_preprocess_splat(s, f);
        const f32 var = 25.f + 0.3f;
        expect(p.mean[0] == 32.f && p.mean[1] == 32.f && p.viewZ == 4.f, "on-axis splat projects to the centre");
        expect(near(p.conic[0], 1.f / var, 1e-5f) && std::fabs(p.conic[1]) < 1e-9f && near(p.conic[2], 1.f / var, 1e-5f),
               "conic = 1 / (sigma_px^2 + 0.3)");
        expect(p.radius == static_cast<u32>(std::ceil(3.f * std::sqrt(var))), "radius = ceil(3 sqrt(lambda_max))");
        expect(p.rect[0] == 1u && p.rect[1] == 1u && p.rect[2] == 3u && p.rect[3] == 3u, "tile rect of a 16 px radius");
        const f32 c0 = 0.28209479177387814f;
        expect(near(p.color[0], c0 + 0.5f, 1e-6f) && near(p.color[1], 0.5f, 1e-6f) &&
                   near(p.color[2], std::max(0.f, -c0 + 0.5f), 1e-6f),
               "SH degree 0 = C0 dc + 0.5");
        GsReferenceFrame ref;
        gs_render_reference(&s, f, nullptr, ref);
        // Pixel (31, 31) is centred at (31.5, 31.5): d = (0.5, 0.5).
        const usize pix = (31u * 64u + 31u) * 4u;
        const f32 alpha = 0.8f * std::exp(-0.5f * (0.25f + 0.25f) / var);
        expect(near(ref.image[pix + 3u], 1.f - alpha, 1e-5f) && near(ref.image[pix], p.color[0] * alpha, 1e-5f),
               "centre pixel: T = 1 - alpha, C = c alpha");
        const f32 far = ref.image[(0u * 64u + 0u) * 4u + 3u];
        expect(far == 1.f, "pixel outside the rect untouched (T = 1)");
        expect(ref.entries == 4u && ref.ranges[(1u * 4u + 1u) * 2u + 1u] == 1u, "4 tile entries, one per tile");
    }
    // Random scene: the whole pipeline's invariants and a brute-force image.
    const u32 w = 93;
    const u32 h = 61;
    const GsAsset scene = gs_test::makeScene(600, 3, 7u);
    const GsCamera cam = gs_test::orbitCamera(w, h, 0.4f);
    const std::vector<f32> depth = gs_test::occluderDepth(cam, w, h, gs_test::kOrbit, 0.5f);
    const GsFrameConstants f = frameFor(cam, w, h, 600, 3, 1u << 16, true);
    GsReferenceFrame ref;
    gs_render_reference(scene.splats.data(), f, depth.data(), ref);
    u32 prefix = 0;
    bool offsetsOk = true;
    u32 visible = 0;
    for (u32 i = 0; i < 600u; ++i) {
        offsetsOk = offsetsOk && ref.offsets[i] == prefix;
        prefix += gs_tiles_touched(ref.projected[i]);
        visible += ref.projected[i].radius > 0u ? 1u : 0u;
    }
    expect(offsetsOk && prefix == ref.entries && ref.dropped == 0u, "offsets = exclusive prefix sum of tiles touched");
    expect(std::is_sorted(ref.keys.begin(), ref.keys.end()), "keys sorted");
    bool padOk = true;
    for (u32 i = ref.entries; i < f.capacity; ++i) {
        padOk = padOk && ref.keys[i] == gs_pad_key(f.tileCount) && ref.values[i] == kGsPadValue;
    }
    expect(padOk, "entries past the emitted ones are padding");
    u32 covered = 0;
    bool rangesOk = true;
    for (u32 t = 0; t < f.tileCount; ++t) {
        const u32 b = ref.ranges[t * 2u];
        const u32 e = ref.ranges[t * 2u + 1u];
        covered += e - b;
        for (u32 i = b; i < e; ++i) {
            rangesOk = rangesOk && static_cast<u32>(ref.keys[i] >> 32) == t;
        }
    }
    expect(rangesOk && covered == ref.entries, "tile ranges partition the emitted entries");
    f64 maxDiff = 0.0;
    u32 occluded = 0;
    bool occludedEmpty = true;
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            const usize p = static_cast<usize>(y) * w + x;
            f32 bf[4];
            bruteForcePixel(ref.projected, x, y, depth[p], f, bf);
            for (u32 c = 0; c < 4u; ++c) {
                maxDiff = std::max(maxDiff, f64(std::fabs(bf[c] - ref.image[p * 4u + c])));
            }
            if (depth[p] < 1.f) {
                ++occluded;
            }
        }
    }
    std::printf("reference: %u / 600 splats visible, %u entries, %u tiles, image vs brute force max |diff| %.3g\n",
                visible, ref.entries, f.tileCount, maxDiff);
    expect(visible > 400u, "most splats visible");
    expect(maxDiff == 0.0, "tile pipeline == brute-force per-pixel depth sort");
    // Occluder at the orbit distance hides the back half: compare against an occluder in front of everything.
    {
        const std::vector<f32> front = gs_test::occluderDepth(cam, w, h, 0.5f, 1.f);
        GsReferenceFrame hidden;
        gs_render_reference(scene.splats.data(), f, front.data(), hidden);
        for (usize p = 0; p < static_cast<usize>(w) * h; ++p) {
            occludedEmpty = occludedEmpty && hidden.image[p * 4u] == 0.f && hidden.image[p * 4u + 1u] == 0.f &&
                            hidden.image[p * 4u + 2u] == 0.f && hidden.image[p * 4u + 3u] == 1.f;
        }
        expect(occludedEmpty, "occluder in front of every splat -> (0, 0, 0, T = 1) everywhere");
        const std::vector<f32> behind = gs_test::occluderDepth(cam, w, h, 50.f, 1.f);
        GsReferenceFrame shown;
        gs_render_reference(scene.splats.data(), f, behind.data(), shown);
        const GsFrameConstants noDepth = frameFor(cam, w, h, 600, 3, 1u << 16, false);
        GsReferenceFrame free;
        gs_render_reference(scene.splats.data(), noDepth, nullptr, free);
        expect(shown.image == free.image, "occluder behind every splat == no depth test");
        u32 changed = 0;
        for (usize p = 0; p < static_cast<usize>(w) * h; ++p) {
            if (depth[p] < 1.f && ref.image[p * 4u + 3u] != free.image[p * 4u + 3u]) {
                ++changed;
            }
            if (depth[p] == 1.f) {
                for (u32 c = 0; c < 4u; ++c) {
                    expect(ref.image[p * 4u + c] == free.image[p * 4u + c], "unoccluded half unchanged");
                }
            }
        }
        std::printf("reference: occluder at z = %.1f changes %u / %u covered pixels\n", f64(gs_test::kOrbit), changed, occluded);
        expect(changed > occluded / 4u, "the mid-scene occluder hides splats behind it");
    }
    // Early termination: 40 opaque splats stacked on the axis keep T >= transmittanceMin.
    {
        GsCamera c{};
        c.fx = c.fy = 100.f;
        c.cx = c.cy = 8.f;
        c.view[11] = 4.f;
        std::vector<GsSplat> stack(40);
        for (u32 i = 0; i < 40u; ++i) {
            stack[i].position[2] = -1.f + 0.05f * static_cast<f32>(i);
            stack[i].scale[0] = stack[i].scale[1] = stack[i].scale[2] = 0.5f;
            stack[i].opacity = 0.99f;
            stack[i].sh[0] = 1.f;
        }
        GsFrameConstants sf = frameFor(c, 16, 16, 40, 0, 4096, false);
        GsReferenceFrame sr;
        gs_render_reference(stack.data(), sf, nullptr, sr);
        const f32 t = sr.image[(8u * 16u + 8u) * 4u + 3u];
        expect(t >= sf.transmittanceMin && t < 1e-3f, "early termination keeps T >= 1e-4 (and stops blending)");
        // Capacity overflow: 40 entries into a capacity of 16.
        sf.capacity = 16u;
        gs_render_reference(stack.data(), sf, nullptr, sr);
        expect(sr.entries == 40u && sr.dropped == 24u, "overflow counted");
    }
}

// --- api -------------------------------------------------------------------------------------------
void testApi() {
    for (u32 tiles : {1u, 2u, 255u, 256u, 8160u, 65535u, 1u << 20}) {
        const u32 bits = gs_sort_key_bits(tiles);
        const u32 passes = (bits + 7u) / 8u;
        expect((passes & 1u) == 0u && bits >= 32u + static_cast<u32>(std::bit_width(tiles)) && bits <= 64u,
               "sort key bits: even pass count covering the padding tile");
    }
    GsFrameConstants f{};
    GsCamera cam = gs_test::orbitCamera(64, 64, 0.f);
    expect(!gs_resolve_constants(cam, GsSettings{}, 0, 64, 1, 3, 16, false, f), "empty extent rejected");
    GsCamera bad = cam;
    bad.fx = 0.f;
    expect(!gs_resolve_constants(bad, GsSettings{}, 64, 64, 1, 3, 16, false, f), "zero focal length rejected");
    GsSettings s{};
    s.shDegree = 2;
    expect(gs_resolve_constants(cam, s, 64, 48, 1, 3, 16, true, f) && f.shDegree == 2u && f.tilesX == 4u && f.tilesY == 3u &&
               (f.flags & kGsFlagDepthTest) != 0u,
           "SH degree clamp, tile grid, depth flag");
    expect(gs_resolve_constants(cam, GsSettings{}, 64, 48, 1, 1, 16, true, f) && f.shDegree == 1u, "asset degree clamp");
    GsplatRenderer r;
    expect(!r.init(GsplatDesc{}), "init without a device fails");
    expect(!r.valid() && !r.setSplats(nullptr, 0, 0), "invalid renderer rejects calls");
}

} // namespace

int main(int argc, char** argv) {
    const std::string suite = argc > 1 ? argv[1] : "all";
    const bool all = suite == "all";
    if (all || suite == "layout") {
        testLayout();
    }
    if (all || suite == "ply") {
        testPly();
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
    std::printf("PASS %s\n", suite.c_str());
    return 0;
}
