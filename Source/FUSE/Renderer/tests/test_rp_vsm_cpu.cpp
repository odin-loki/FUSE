// WP-3.1 virtual shadow map CPU gates (stub-safe): `fuse_rp_vsm_cpu <suite>`.
//
//   layout      VsmFrameConstants / level / bounds records: C++ sizes and offsets, and the GLSL and Slang
//               twins (vsm_common.{glsl,slang}) declare the same fields in the same order; work layout
//               sections 256-aligned and disjoint
//   clipmap     VsmClipmap placement: page size doubles per level, window origins = the camera's page,
//               the camera page sits inside every window, first-frame / scroll / depth-key / rotation
//               history, depthToLight == [R 0; 0 1] * invViewProj (double), argument validation
//   mark        the mark kernel on an analytic depth image (ground plane + spheres, 320 x 180):
//               CpuReference == CpuParallel bit for bit; every pixel's page equals a double-precision
//               brute-force oracle (level = finest window that contains the point with a page of margin,
//               page = floor) except on pixels within 1e-4 page / level-boundary distance (counted, < 0.5%);
//               marked pages lie in their windows; the radius footprint covers the centre page;
//               kernel stats
//   invalidate  VsmInvalidationReference: first frame marks every tracked footprint, a static frame marks
//               nothing, a moved / removed / flag-changed instance marks exactly its old + new footprint
//               (brute-force 8-corner light-space box, double), untracked instances never mark
//   frame       the whole CPU frame (placement + mark + invalidation + page model) over a static scene:
//               frame 2 renders 0 pages, a moving object re-renders only pages under its footprint, a
//               camera move re-renders only the pages that scrolled in, a light rotation re-renders all
//   api         VirtualShadowMap without a device: init fails cleanly, calls are no-ops; capabilities
#include <bit>
#include <fuse/compute_kernel/launch.hpp>
#include <fuse/compute_kernel/stats.hpp>
#include <fuse/renderer/shadow/vsm/virtual_shadow_map.hpp>
#include <fuse/renderer/shadow/vsm/vsm_clipmap.hpp>
#include <fuse/renderer/shadow/vsm/vsm_kernel.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace fuse::renderer;
using namespace fuse::renderer::vsm;
namespace core_logic = fuse::core_logic;
using fuse::f32;
using fuse::f64;
using fuse::s32;
using fuse::s64;
using fuse::u32;
using fuse::u64;
using fuse::usize;

int g_failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

// --- math (column-major, Vulkan clip space, forward depth) -----------------------------------------
struct M4 {
    f64 m[16] = {};
};

M4 mul(const M4& a, const M4& b) {
    M4 r{};
    for (u32 c = 0; c < 4; ++c) {
        for (u32 row = 0; row < 4; ++row) {
            f64 s = 0.0;
            for (u32 k = 0; k < 4; ++k) {
                s += a.m[k * 4 + row] * b.m[c * 4 + k];
            }
            r.m[c * 4 + row] = s;
        }
    }
    return r;
}

M4 perspective(f64 fovY, f64 aspect, f64 n, f64 f) {
    const f64 t = 1.0 / std::tan(fovY * 0.5);
    M4 p{};
    p.m[0] = t / aspect;
    p.m[5] = -t;
    p.m[10] = f / (n - f);
    p.m[11] = -1.0;
    p.m[14] = n * f / (n - f);
    return p;
}

M4 lookAt(const f64 eye[3], const f64 at[3]) {
    f64 fw[3] = {at[0] - eye[0], at[1] - eye[1], at[2] - eye[2]};
    const f64 fl = std::sqrt(fw[0] * fw[0] + fw[1] * fw[1] + fw[2] * fw[2]);
    for (f64& v : fw) {
        v /= fl;
    }
    f64 s[3] = {fw[1] * 0.0 - fw[2] * 1.0, fw[2] * 0.0 - fw[0] * 0.0, fw[0] * 1.0 - fw[1] * 0.0};
    const f64 sl = std::sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    for (f64& v : s) {
        v /= sl;
    }
    const f64 u[3] = {s[1] * fw[2] - s[2] * fw[1], s[2] * fw[0] - s[0] * fw[2], s[0] * fw[1] - s[1] * fw[0]};
    M4 v{};
    v.m[0] = s[0];
    v.m[4] = s[1];
    v.m[8] = s[2];
    v.m[1] = u[0];
    v.m[5] = u[1];
    v.m[9] = u[2];
    v.m[2] = -fw[0];
    v.m[6] = -fw[1];
    v.m[10] = -fw[2];
    v.m[12] = -(s[0] * eye[0] + s[1] * eye[1] + s[2] * eye[2]);
    v.m[13] = -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]);
    v.m[14] = fw[0] * eye[0] + fw[1] * eye[1] + fw[2] * eye[2];
    v.m[15] = 1.0;
    return v;
}

bool invert(const M4& in, M4& out) {
    f64 a[4][8];
    for (u32 r = 0; r < 4; ++r) {
        for (u32 c = 0; c < 4; ++c) {
            a[r][c] = in.m[c * 4 + r];
            a[r][c + 4] = r == c ? 1.0 : 0.0;
        }
    }
    for (u32 c = 0; c < 4; ++c) {
        u32 piv = c;
        for (u32 r = c + 1; r < 4; ++r) {
            if (std::fabs(a[r][c]) > std::fabs(a[piv][c])) {
                piv = r;
            }
        }
        if (std::fabs(a[piv][c]) < 1e-300) {
            return false;
        }
        for (u32 k = 0; k < 8; ++k) {
            std::swap(a[c][k], a[piv][k]);
        }
        const f64 d = a[c][c];
        for (u32 k = 0; k < 8; ++k) {
            a[c][k] /= d;
        }
        for (u32 r = 0; r < 4; ++r) {
            if (r != c) {
                const f64 f = a[r][c];
                for (u32 k = 0; k < 8; ++k) {
                    a[r][k] -= f * a[c][k];
                }
            }
        }
    }
    for (u32 r = 0; r < 4; ++r) {
        for (u32 c = 0; c < 4; ++c) {
            out.m[c * 4 + r] = a[r][c + 4];
        }
    }
    return true;
}

void xform(const M4& m, const f64 p[4], f64 out[4]) {
    for (u32 r = 0; r < 4; ++r) {
        out[r] = m.m[r] * p[0] + m.m[4 + r] * p[1] + m.m[8 + r] * p[2] + m.m[12 + r] * p[3];
    }
}

// --- analytic scene --------------------------------------------------------------------------------
constexpr u32 kW = 320;
constexpr u32 kH = 180;

struct Camera {
    M4 viewProj{};
    M4 invViewProj{};
    f64 eye[3] = {};
};

Camera makeCamera(const f64 eye[3], const f64 at[3]) {
    Camera c{};
    c.viewProj = mul(perspective(1.0, static_cast<f64>(kW) / kH, 0.2, 300.0), lookAt(eye, at));
    invert(c.viewProj, c.invViewProj);
    std::memcpy(c.eye, eye, sizeof(c.eye));
    return c;
}

/// Forward depth of the nearest hit of a ground plane (y = 0) and a few spheres.
std::vector<f32> renderDepth(const Camera& cam) {
    std::vector<f32> depth(static_cast<usize>(kW) * kH, 1.f);
    const f64 spheres[4][4] = {{0.0, 1.0, -8.0, 1.0}, {3.0, 2.0, -20.0, 2.0}, {-6.0, 0.5, -3.0, 0.5}, {10.0, 4.0, -60.0, 4.0}};
    for (u32 y = 0; y < kH; ++y) {
        for (u32 x = 0; x < kW; ++x) {
            const f64 nx = (x + 0.5) * 2.0 / kW - 1.0;
            const f64 ny = (y + 0.5) * 2.0 / kH - 1.0;
            const f64 pn[4] = {nx, ny, 0.0, 1.0};
            const f64 pf[4] = {nx, ny, 1.0, 1.0};
            f64 a[4], b[4];
            xform(cam.invViewProj, pn, a);
            xform(cam.invViewProj, pf, b);
            f64 o[3], d[3];
            for (u32 k = 0; k < 3; ++k) {
                o[k] = a[k] / a[3];
                d[k] = b[k] / b[3] - o[k];
            }
            f64 best = 1e300;
            if (d[1] < 0.0) {
                best = -o[1] / d[1];
            }
            for (const auto& s : spheres) {
                const f64 oc[3] = {o[0] - s[0], o[1] - s[1], o[2] - s[2]};
                const f64 A = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
                const f64 B = 2.0 * (oc[0] * d[0] + oc[1] * d[1] + oc[2] * d[2]);
                const f64 C = oc[0] * oc[0] + oc[1] * oc[1] + oc[2] * oc[2] - s[3] * s[3];
                const f64 disc = B * B - 4.0 * A * C;
                if (disc >= 0.0) {
                    const f64 t = (-B - std::sqrt(disc)) / (2.0 * A);
                    if (t > 0.0 && t < best) {
                        best = t;
                    }
                }
            }
            if (best > 1.0) {
                continue; // beyond the far plane
            }
            const f64 hit[4] = {o[0] + d[0] * best, o[1] + d[1] * best, o[2] + d[2] * best, 1.0};
            f64 clip[4];
            xform(cam.viewProj, hit, clip);
            depth[y * kW + x] = static_cast<f32>(clip[2] / clip[3]);
        }
    }
    return depth;
}

VsmViewDesc makeView(const Camera& cam, const f32 lightDir[3]) {
    VsmViewDesc v{};
    std::memcpy(v.lightDirection, lightDir, sizeof(v.lightDirection));
    for (u32 k = 0; k < 3; ++k) {
        v.cameraPosition[k] = static_cast<f32>(cam.eye[k]);
    }
    for (u32 i = 0; i < 16; ++i) {
        v.invViewProj[i] = static_cast<f32>(cam.invViewProj.m[i]);
    }
    v.depthWidth = kW;
    v.depthHeight = kH;
    v.pixelSpread = static_cast<f32>(2.0 * std::tan(0.5) / kH); // makeCamera: fovY 1.0
    return v;
}

const f32 kSun[3] = {0.35f, -1.f, -0.25f};

// --- layout ----------------------------------------------------------------------------------------
std::string readFile(const std::string& path) {
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

/// Field names of the block that starts at `open` (up to the next "};"), in order.
std::vector<std::string> fieldNames(const std::string& text, const std::string& open) {
    std::vector<std::string> names;
    const usize at = text.find(open);
    if (at == std::string::npos) {
        return names;
    }
    const usize end = text.find("};", at);
    std::istringstream lines(text.substr(at + open.size(), end - at - open.size()));
    std::string line;
    while (std::getline(lines, line)) {
        const usize semi = line.find(';');
        if (semi == std::string::npos) {
            continue;
        }
        std::string decl = line.substr(0, semi);
        const usize br = decl.find('[');
        if (br != std::string::npos) {
            decl = decl.substr(0, br);
        }
        const usize sp = decl.find_last_of(" \t");
        names.push_back(sp == std::string::npos ? decl : decl.substr(sp + 1));
    }
    return names;
}

void testLayout() {
    expect(sizeof(VsmFrameConstants) == 1072u && sizeof(VsmLevelConstants) == 48u && sizeof(VsmBoundsRecord) == 32u,
           "record sizes");
    const std::string dir = FUSE_VSM_INCLUDE_DIR;
    const std::string glsl = readFile(dir + "/vsm_common.glsl");
    const std::string slang = readFile(dir + "/vsm_common.slang");
    const auto gc = fieldNames(glsl, "readonly buffer FuseVsmConstantsRef {");
    const auto sc = fieldNames(slang, "struct VsmConstants {");
    const auto gl = fieldNames(glsl, "struct FuseVsmLevel {");
    const auto sl = fieldNames(slang, "struct VsmLevel {");
    std::printf("  constants fields: glsl %zu, slang %zu; level fields: glsl %zu, slang %zu\n", gc.size(), sc.size(), gl.size(),
                sl.size());
    expect(gc.size() == 44u && gc == sc, "GLSL and Slang VsmFrameConstants declare the same fields in order");
    expect(gl.size() == 12u && gl == sl, "GLSL and Slang level records declare the same fields in order");
    // Spot-check against the C++ names.
    expect(!gc.empty() && gc.front() == "pageTable" && gc[4] == "depthToLight" && gc.back() == "level", "C++ field order");
    for (const u32 phys : {1u, 256u, 4096u}) {
        for (const u32 levels : {1u, 7u, 16u}) {
            const VsmWorkLayout l = VsmWorkLayout::compute(levels, phys);
            const u64 words = (static_cast<u64>(levels) * kPagesPerLevel + 31u) / 32u;
            const bool aligned = l.request % 256u == 0u && l.need % 256u == 0u && l.renderList % 256u == 0u &&
                                 l.candidates % 256u == 0u && l.counters == 0u;
            const bool disjoint = l.request >= kCounterCount * 4u && l.need >= l.request + words * 4u &&
                                  l.renderList >= l.need + words * 4u && l.candidates >= l.renderList + phys * 8u &&
                                  l.bytes >= l.candidates + phys * 4u && l.request + l.resetBytes <= l.renderList &&
                                  l.request + l.resetBytes >= l.need + words * 4u;
            expect(aligned && disjoint, "work layout aligned and disjoint");
        }
    }
}

// --- clipmap ---------------------------------------------------------------------------------------
void testClipmap() {
    VsmClipmap cm;
    VsmClipmapDesc d{};
    expect(!cm.init(VsmClipmapDesc{0u}) && !cm.init(VsmClipmapDesc{17u}), "level count validated");
    d.firstLevelExtent = 0.f;
    expect(!cm.init(d), "extent validated");
    d = VsmClipmapDesc{};
    d.markRadiusTexels = 65.f;
    expect(!cm.init(d), "radius validated");
    d = VsmClipmapDesc{};
    d.lodBias = 40;
    expect(!cm.init(d), "LOD bias validated");
    d = VsmClipmapDesc{};
    VsmFrameConstants c{};
    const f64 eye0[3] = {1.3, 3.0, 4.0};
    const f64 at0[3] = {0.0, 0.0, -20.0};
    const Camera cam = makeCamera(eye0, at0);
    VsmViewDesc view = makeView(cam, kSun);
    expect(!cm.build(view, c), "build before init fails");
    expect(cm.init(d), "init");
    VsmViewDesc bad = view;
    bad.depthWidth = 0;
    expect(!cm.build(bad, c), "empty depth extent rejected");
    bad = view;
    bad.lightDirection[0] = bad.lightDirection[1] = bad.lightDirection[2] = 0.f;
    expect(!cm.build(bad, c), "degenerate light rejected");
    bad = view;
    bad.cameraPosition[0] = 1e12f;
    expect(!cm.build(bad, c), "camera past the origin range rejected");
    expect(cm.frame() == 0u, "failed builds do not advance the frame");
    expect(cm.build(view, c) && c.frame == 1u && c.levels == 16u && c.flags == 0u, "first frame");
    bool ok = true;
    for (u32 l = 0; l < 16u; ++l) {
        const VsmLevelConstants& L = c.level[l];
        ok = ok && L.prevOriginX == L.originX && L.prevOriginY == L.originY && L.prevDepthKey == L.depthKey && L.flags == 0u;
        ok = ok && L.pageWorld == std::ldexp(d.firstLevelExtent / 128.f, static_cast<int>(l)) && L.pageWorld * L.invPageWorld == 1.f;
        ok = ok && L.originX == static_cast<s32>(std::floor(c.cameraLight[0] * L.invPageWorld)) &&
             L.originY == static_cast<s32>(std::floor(c.cameraLight[1] * L.invPageWorld));
        ok = ok && core_logic::vsm_in_window(L.originX, L.originX, 128u) && L.depthStep == L.pageWorld * 32.f;
    }
    expect(ok, "level placement: page size doubles, origin = the camera's page, first frame has no history");
    // depthToLight == [R 0; 0 1] * invViewProj.
    f64 maxErr = 0.0;
    for (u32 col = 0; col < 4; ++col) {
        for (u32 row = 0; row < 4; ++row) {
            f64 s = 0.0;
            for (u32 k = 0; k < 4; ++k) {
                const f64 r = row < 3 && k < 3 ? c.lightRotation[row * 4 + k] : (row == 3 && k == 3 ? 1.0 : 0.0);
                s += r * static_cast<f32>(cam.invViewProj.m[col * 4 + k]);
            }
            maxErr = std::max(maxErr, std::fabs(s - c.depthToLight[col * 4 + row]) / std::max(1.0, std::fabs(s)));
        }
    }
    expect(maxErr < 1e-6, "depthToLight = rotation * invViewProj");
    // Light basis is orthonormal and z runs against the light.
    f64 dot = 0.0, len = 0.0;
    const f64 ln = std::sqrt(0.35 * 0.35 + 1.0 + 0.25 * 0.25);
    for (u32 k = 0; k < 3; ++k) {
        dot += c.lightRotation[8 + k] * kSun[k] / ln;
        len += c.lightRotation[k] * c.lightRotation[k];
    }
    expect(std::fabs(dot + 1.0) < 1e-5 && std::fabs(len - 1.0) < 1e-5, "stabilised light basis (z = -light direction)");
    // Scroll: move the camera by 3 level-0 pages along light x.
    const VsmFrameConstants first = c;
    VsmViewDesc moved = view;
    for (u32 k = 0; k < 3; ++k) {
        moved.cameraPosition[k] += 3.f * first.level[0].pageWorld * c.lightRotation[k];
    }
    expect(cm.build(moved, c) && c.frame == 2u && c.flags == 0u, "second frame");
    expect(c.level[0].prevOriginX == first.level[0].originX && std::abs(c.level[0].originX - first.level[0].originX - 3) <= 1 &&
               c.level[0].prevOriginY == first.level[0].originY,
           "scroll history: prev = last frame's origin");
    // Depth key: move along the light by more than one depth step of level 0.
    VsmViewDesc deeper = moved;
    for (u32 k = 0; k < 3; ++k) {
        deeper.cameraPosition[k] += 1.5f * c.level[0].depthStep * c.lightRotation[8 + k];
    }
    expect(cm.build(deeper, c) && c.level[0].depthKey != c.level[0].prevDepthKey && (c.level[0].flags & 1u) != 0u &&
               (c.level[15].flags & 1u) == 0u,
           "a depth-key step invalidates that level only");
    // Rotation.
    VsmViewDesc rotated = deeper;
    rotated.lightDirection[0] = 0.4f;
    expect(cm.build(rotated, c) && (c.flags & kFrameInvalidateAll) != 0u && (c.level[3].flags & 1u) != 0u,
           "a light rotation invalidates everything");
    expect(cm.build(rotated, c) && c.flags == 0u, "same rotation next frame: no invalidation");
    VsmViewDesc forced = rotated;
    forced.invalidateAll = true;
    expect(cm.build(forced, c) && (c.flags & kFrameInvalidateAll) != 0u, "forced invalidate-all");
    cm.reset();
    expect(cm.build(view, c) && c.frame == 1u && c.level[0].prevOriginX == c.level[0].originX, "reset restarts the history");
    const core_logic::VsmFrameInput in = frameInput(c);
    expect(in.frame == 1u && in.levels == 16u && in.level[5].originX == c.level[5].originX &&
               in.level[5].depthKey == c.level[5].depthKey && in.invalidateAll == 0u,
           "frameInput mirrors the constants");
}

// --- mark ------------------------------------------------------------------------------------------
struct OraclePage {
    s32 level = -1;
    s64 ax = 0, ay = 0;
    bool robust = true;
};

OraclePage oraclePage(const VsmFrameConstants& c, const Camera& cam, u32 x, u32 y, f32 depth) {
    OraclePage o{};
    if (!(depth < 1.f)) {
        o.level = -2; // background
        return o;
    }
    const f64 nx = (x + 0.5) * 2.0 / kW - 1.0;
    const f64 ny = (y + 0.5) * 2.0 / kH - 1.0;
    const f64 p[4] = {nx, ny, static_cast<f64>(depth), 1.0};
    f64 w[4];
    xform(cam.invViewProj, p, w);
    f64 l[3] = {};
    for (u32 r = 0; r < 3; ++r) {
        for (u32 k = 0; k < 3; ++k) {
            l[r] += static_cast<f64>(c.lightRotation[r * 4 + k]) * (w[k] / w[3]);
        }
    }
    f64 d = 0.0;
    for (u32 k = 0; k < 3; ++k) {
        d = std::max(d, std::fabs(l[k] - static_cast<f64>(c.cameraLight[k])));
    }
    // Containment: the finest level whose (kPagesPerAxis / 2 - 1) pages of margin contain the point;
    // density: the first level whose texel (pageWorld / 128) covers d * pixelSpread / texelsPerPixel.
    s32 containment = 0;
    while (containment < 64 && d >= 63.0 * std::ldexp(static_cast<f64>(c.level[0].pageWorld), containment)) {
        ++containment;
    }
    const f64 texel0 = static_cast<f64>(c.level[0].pageWorld) / 128.0;
    const f64 footprint = d * static_cast<f64>(c.densityScale) * texel0; // = d * pixelSpread / texelsPerPixel
    s32 density = 0;
    while (density < 64 && footprint >= std::ldexp(texel0, density)) {
        ++density;
    }
    const s32 level = std::max(containment, density + c.lodBias);
    auto fragileRatio = [](f64 q) { return q >= 0.5 && std::fabs(q / std::exp2(std::round(std::log2(q))) - 1.0) < 1e-4; };
    if (fragileRatio(d / (63.0 * c.level[0].pageWorld)) || fragileRatio(footprint / texel0)) {
        o.robust = false;
    }
    if (level >= static_cast<s32>(c.levels)) {
        o.level = -1;
        return o;
    }
    o.level = level;
    const f64 u = l[0] * c.level[level].invPageWorld;
    const f64 v = l[1] * c.level[level].invPageWorld;
    o.ax = static_cast<s64>(std::floor(u));
    o.ay = static_cast<s64>(std::floor(v));
    if (std::fabs(u - std::round(u)) < 1e-4 * std::max(1.0, std::fabs(u)) ||
        std::fabs(v - std::round(v)) < 1e-4 * std::max(1.0, std::fabs(v))) {
        o.robust = false;
    }
    return o;
}

void testMark() {
    VsmClipmap cm;
    VsmClipmapDesc d{};
    d.firstLevelExtent = 4.f;  // level 0 window 4 m
    d.texelsPerPixel = 8.f;    // finer than the screen: more pages, more page / level boundaries to test
    expect(cm.init(d), "init");
    const f64 eye[3] = {0.7, 2.2, 3.0};
    const f64 at[3] = {0.5, 0.0, -25.0};
    const Camera cam = makeCamera(eye, at);
    const std::vector<f32> depth = renderDepth(cam);
    VsmFrameConstants c{};
    expect(cm.build(makeView(cam, kSun), c), "build");
    std::vector<u32> ref(c.requestWords), par(c.requestWords), scratch, scratchPar;
    const u32 marked = markReference(c, depth.data(), kW, kH, ref.data(), fuse::kernel::Backend::CpuReference, scratch);
    const u32 markedPar = markReference(c, depth.data(), kW, kH, par.data(), fuse::kernel::Backend::CpuParallel, scratchPar);
    expect(marked == markedPar && ref == par && scratch == scratchPar, "CpuReference == CpuParallel (bit for bit)");
    const fuse::kernel::LaunchRecord last = fuse::kernel::last_launch();
    expect(std::strcmp(last.name, "vsm.mark") == 0 && last.items == static_cast<u64>(kW) * kH, "kernel stats name and items");
    // Per pixel against the double oracle.
    u32 covered = 0, robustBad = 0, fragile = 0, fragileBad = 0, beyond = 0;
    u32 levelsUsed[16] = {};
    for (u32 y = 0; y < kH; ++y) {
        for (u32 x = 0; x < kW; ++x) {
            const u32 i = y * kW + x;
            const OraclePage o = oraclePage(c, cam, x, y, depth[i]);
            const u32 got = scratch[i * 4u];
            if (o.level == -2) {
                expect(got == kPageNone, "background pixels mark nothing");
                continue;
            }
            ++covered;
            u32 expected = kPageNone;
            if (o.level >= 0) {
                ++levelsUsed[o.level];
                expected = static_cast<u32>(o.level) * kPagesPerLevel + kernel_math::slot_of(static_cast<s32>(o.ay)) * 128u +
                           kernel_math::slot_of(static_cast<s32>(o.ax));
                const VsmLevelConstants& L = c.level[o.level];
                expect(core_logic::vsm_in_window(static_cast<s32>(o.ax), L.originX, 128u) &&
                           core_logic::vsm_in_window(static_cast<s32>(o.ay), L.originY, 128u),
                       "the selected level's window contains the point");
            } else {
                ++beyond;
            }
            if (!o.robust) {
                ++fragile;
                fragileBad += got != expected ? 1u : 0u;
            } else if (got != expected) {
                ++robustBad;
            }
        }
    }
    std::printf("  mark: %u covered pixels, %u pages marked, levels used:", covered, marked);
    for (u32 l = 0; l < 16u; ++l) {
        if (levelsUsed[l] != 0u) {
            std::printf(" L%u=%u", l, levelsUsed[l]);
        }
    }
    std::printf("; beyond the last level %u; boundary pixels %u (%u differ from the double oracle)\n", beyond, fragile,
                fragileBad);
    expect(covered > kW * kH / 2u && marked > 20u, "the scene covers the image and marks pages");
    u32 distinctLevels = 0;
    for (const u32 n : levelsUsed) {
        distinctLevels += n > 0u ? 1u : 0u;
    }
    expect(distinctLevels >= 4u, "several clipmap levels are used");
    expect(robustBad == 0u, "every robust pixel marks the oracle's page");
    expect(fragile * 200u < covered, "boundary pixels < 0.5%");
    // Mark radius: 64 texels = half a page; the footprint covers the centre page and stays <= 2 x 2.
    VsmClipmap wide;
    VsmClipmapDesc wd = d;
    wd.markRadiusTexels = 64.f;
    expect(wide.init(wd), "init radius");
    VsmFrameConstants cw{};
    expect(wide.build(makeView(cam, kSun), cw), "build radius");
    std::vector<u32> wideWords(cw.requestWords), wideScratch;
    const u32 wideMarked = markReference(cw, depth.data(), kW, kH, wideWords.data(), fuse::kernel::Backend::CpuReference, wideScratch);
    bool covers = true;
    u32 multi = 0;
    for (u32 i = 0; i < kW * kH; ++i) {
        const u32 centre = scratch[i * 4u];
        u32 n = 0;
        bool found = centre == kPageNone;
        for (u32 k = 0; k < 4u; ++k) {
            const u32 v = wideScratch[i * 4u + k];
            n += v != kPageNone ? 1u : 0u;
            found = found || v == centre;
        }
        covers = covers && found && (centre == kPageNone) == (n == 0u);
        multi += n > 1u ? 1u : 0u;
    }
    expect(covers && multi > 0u && wideMarked > marked, "radius footprint: centre page included, neighbours added");
    // Every marked bit belongs to an active level; ref bits == union of per-pixel pages.
    bool inRange = true;
    for (u32 w = c.virtualPages / 32u; w < c.requestWords; ++w) {
        inRange = inRange && ref[w] == 0u;
    }
    expect(inRange, "no bits past the active levels");
}

// --- invalidate ------------------------------------------------------------------------------------
struct MiniScene {
    std::vector<gpu_scene::GpuInstance> inst;
    std::vector<gpu_scene::GpuTransform> xf;
    std::vector<gpu_scene::GpuMesh> meshes;
};

gpu_scene::GpuTransform place(f32 x, f32 y, f32 z, f32 s, f32 yaw) {
    gpu_scene::GpuTransform t{};
    t.rows[0][0] = std::cos(yaw) * s;
    t.rows[0][2] = std::sin(yaw) * s;
    t.rows[1][1] = s;
    t.rows[2][0] = -std::sin(yaw) * s;
    t.rows[2][2] = std::cos(yaw) * s;
    t.rows[0][3] = x;
    t.rows[1][3] = y;
    t.rows[2][3] = z;
    return t;
}

MiniScene makeMiniScene() {
    MiniScene s;
    gpu_scene::GpuMesh m{};
    m.boundsCenter[1] = 0.5f;
    m.boundsRadius = 0.9f;
    s.meshes.push_back(m);
    m.boundsCenter[1] = 0.f;
    m.boundsRadius = 3.f;
    s.meshes.push_back(m);
    std::mt19937 rng(7);
    std::uniform_real_distribution<f32> u(-1.f, 1.f);
    for (u32 i = 0; i < 40; ++i) {
        gpu_scene::GpuInstance in{};
        in.mesh = i % 7u == 0u ? 1u : 0u;
        in.flags = gpu_scene::kInstanceValid | gpu_scene::kInstanceVisible | gpu_scene::kInstanceCastShadow;
        s.inst.push_back(in);
        s.xf.push_back(place(u(rng) * 12.f, 0.f, -4.f - 30.f * (u(rng) * 0.5f + 0.5f), 0.5f + 0.5f * (u(rng) * 0.5f + 0.5f),
                             u(rng) * 3.f));
    }
    s.inst[5].flags = 0u;                          // free slot
    s.inst[6].flags &= ~gpu_scene::kInstanceCastShadow; // not a caster
    s.inst[7].mesh = 99u;                          // no mesh
    return s;
}

/// Brute-force footprint (double, 8 corners of the world box) of one instance on one level, or false.
bool bruteRect(const VsmFrameConstants& c, const gpu_scene::GpuInstance& in, const gpu_scene::GpuTransform& t,
               const std::vector<gpu_scene::GpuMesh>& meshes, u32 level, s64 rect[4], bool& robust) {
    const u32 need = gpu_scene::kInstanceValid | gpu_scene::kInstanceCastShadow;
    if ((in.flags & need) != need || in.mesh >= meshes.size()) {
        return false;
    }
    const gpu_scene::GpuMesh& m = meshes[in.mesh];
    f64 center[3], extent[3];
    for (u32 k = 0; k < 3; ++k) {
        center[k] = t.rows[k][0] * static_cast<f64>(m.boundsCenter[0]) + t.rows[k][1] * static_cast<f64>(m.boundsCenter[1]) +
                    t.rows[k][2] * static_cast<f64>(m.boundsCenter[2]) + t.rows[k][3];
        extent[k] = m.boundsRadius * (std::fabs(t.rows[k][0]) + std::fabs(t.rows[k][1]) + std::fabs(t.rows[k][2]));
    }
    f64 lo[2] = {1e300, 1e300}, hi[2] = {-1e300, -1e300};
    for (u32 corner = 0; corner < 8; ++corner) {
        f64 p[3];
        for (u32 k = 0; k < 3; ++k) {
            p[k] = center[k] + ((corner >> k) & 1u ? extent[k] : -extent[k]);
        }
        for (u32 r = 0; r < 2; ++r) {
            const f64 v = c.lightRotation[r * 4] * p[0] + c.lightRotation[r * 4 + 1] * p[1] + c.lightRotation[r * 4 + 2] * p[2];
            lo[r] = std::min(lo[r], v);
            hi[r] = std::max(hi[r], v);
        }
    }
    const VsmLevelConstants& L = c.level[level];
    robust = true;
    s64 r[4];
    const f64 f[4] = {lo[0] * L.invPageWorld, lo[1] * L.invPageWorld, hi[0] * L.invPageWorld, hi[1] * L.invPageWorld};
    for (u32 k = 0; k < 4; ++k) {
        r[k] = static_cast<s64>(std::floor(f[k]));
        if (std::fabs(f[k] - std::round(f[k])) < 1e-4) {
            robust = false;
        }
    }
    r[0] = std::max<s64>(r[0], L.originX - 64);
    r[1] = std::max<s64>(r[1], L.originY - 64);
    r[2] = std::min<s64>(r[2], L.originX + 63);
    r[3] = std::min<s64>(r[3], L.originY + 63);
    if (r[0] > r[2] || r[1] > r[3]) {
        return false;
    }
    std::memcpy(rect, r, sizeof(r));
    return true;
}

/// Kernel footprint bits of one instance state (every level) + per-level comparison against the brute
/// force (counted: robust comparisons and robust mismatches).
struct RectCheck {
    u32 robust = 0;
    u32 mismatches = 0;
};

void kernelBits(const VsmFrameConstants& c, const MiniScene& s, const gpu_scene::GpuInstance& in, const gpu_scene::GpuTransform& t,
                std::vector<u32>& words, RectCheck& check) {
    const gpu_scene::GpuMesh* mesh = in.mesh < s.meshes.size() ? &s.meshes[in.mesh] : nullptr;
    const VsmBoundsRecord rec = kernel_math::make_bounds_record(in, mesh, t);
    for (u32 l = 0; l < c.levels; ++l) {
        s32 rect[4];
        const bool got = kernel_math::bounds_rect(c, rec, l, rect);
        if (got) {
            setRectBits(l, rect, words.data());
        }
        s64 brute[4] = {0, 0, 0, 0};
        bool rob = true;
        const bool want = bruteRect(c, in, t, s.meshes, l, brute, rob);
        if (!rob) {
            continue;
        }
        ++check.robust;
        const bool same = got == want && (!got || (rect[0] == brute[0] && rect[1] == brute[1] && rect[2] == brute[2] &&
                                                   rect[3] == brute[3]));
        check.mismatches += same ? 0u : 1u;
    }
}

u32 popcount(const std::vector<u32>& w) {
    u32 n = 0;
    for (const u32 v : w) {
        n += static_cast<u32>(std::popcount(v));
    }
    return n;
}

void testInvalidate() {
    VsmClipmap cm;
    VsmClipmapDesc d{};
    d.firstLevelExtent = 4.f;
    d.levels = 10;
    expect(cm.init(d), "init");
    const f64 eye[3] = {0.0, 2.0, 2.0};
    const f64 at[3] = {0.0, 0.0, -20.0};
    const Camera cam = makeCamera(eye, at);
    VsmFrameConstants c{};
    expect(cm.build(makeView(cam, kSun), c), "build");
    MiniScene s = makeMiniScene();
    VsmInvalidationReference ref;
    std::vector<u32> words(c.requestWords), expected(c.requestWords);
    const u32 n = static_cast<u32>(s.inst.size());
    // Frame 1: every tracked instance appears.
    u32 changed = ref.run(c, s.inst.data(), s.xf.data(), n, s.meshes.data(), static_cast<u32>(s.meshes.size()), words.data());
    std::fill(expected.begin(), expected.end(), 0u);
    RectCheck check{};
    for (u32 i = 0; i < n; ++i) {
        kernelBits(c, s, s.inst[i], s.xf[i], expected, check);
    }
    expect(changed == n - 3u, "first frame: every tracked slot changed (3 untracked)");
    expect(words == expected, "first frame: the mask is the union of every tracked footprint");
    std::printf("  invalidate: first frame %u pages over %u levels; %u robust footprints vs the 8-corner brute force, "
                "%u differ\n",
                popcount(words), c.levels, check.robust, check.mismatches);
    expect(check.mismatches == 0u && check.robust > n, "footprints == brute force (double, 8 corners)");
    // Frame 2: nothing changed.
    changed = ref.run(c, s.inst.data(), s.xf.data(), n, s.meshes.data(), static_cast<u32>(s.meshes.size()), words.data());
    expect(changed == 0u && popcount(words) == 0u, "static frame: no invalidation");
    // Frame 3: move one instance, remove one, disable casting on one, change an untracked one.
    const gpu_scene::GpuTransform oldXf = s.xf[3];
    const gpu_scene::GpuInstance oldInst9 = s.inst[9], oldInst11 = s.inst[11];
    s.xf[3].rows[0][3] += 0.6f;
    s.inst[9].flags = 0u;
    s.inst[11].flags &= ~gpu_scene::kInstanceCastShadow;
    s.xf[6].rows[1][3] += 5.f; // not a caster: moves freely
    changed = ref.run(c, s.inst.data(), s.xf.data(), n, s.meshes.data(), static_cast<u32>(s.meshes.size()), words.data());
    std::fill(expected.begin(), expected.end(), 0u);
    check = RectCheck{};
    kernelBits(c, s, s.inst[3], oldXf, expected, check);
    kernelBits(c, s, s.inst[3], s.xf[3], expected, check);
    kernelBits(c, s, oldInst9, s.xf[9], expected, check);
    kernelBits(c, s, oldInst11, s.xf[11], expected, check);
    kernelBits(c, s, s.inst[9], s.xf[9], expected, check);   // removed: no footprint
    kernelBits(c, s, s.inst[11], s.xf[11], expected, check); // not a caster any more: no footprint
    expect(changed == 3u, "three slots changed");
    expect(words == expected, "moved / removed / non-casting: exactly the old + new footprints");
    expect(check.mismatches == 0u, "those footprints == brute force");
    std::printf("  invalidate: move + remove + flag change -> %u pages\n", popcount(words));
    // Growth: new slots start tracked from nothing.
    s.inst.push_back(s.inst[0]);
    s.xf.push_back(place(2.f, 0.f, -9.f, 1.f, 0.f));
    changed = ref.run(c, s.inst.data(), s.xf.data(), n + 1u, s.meshes.data(), static_cast<u32>(s.meshes.size()), words.data());
    expect(changed == 1u && popcount(words) > 0u, "a new instance invalidates its footprint");
    ref.reset();
    changed = ref.run(c, s.inst.data(), s.xf.data(), n + 1u, s.meshes.data(), static_cast<u32>(s.meshes.size()), words.data());
    expect(changed == n + 1u - 5u && ref.records().size() == n + 1u, "reset: every tracked slot again");
    // Record math: untracked records are all zero, tracked ones carry the flag.
    const VsmBoundsRecord r0 = kernel_math::make_bounds_record(s.inst[5], &s.meshes[0], s.xf[5]);
    const VsmBoundsRecord r1 = kernel_math::make_bounds_record(s.inst[0], &s.meshes[s.inst[0].mesh], s.xf[0]);
    expect(r0.tracked == 0u && r0.center[0] == 0.f && r0.extent[2] == 0.f && r1.tracked == 1u && r1.extent[0] > 0.f,
           "bounds records");
    expect(kernel_math::same_record(r1, r1) && !kernel_math::same_record(r0, r1), "record comparison");
    s32 rect[4];
    expect(!kernel_math::bounds_rect(c, r0, 0, rect) && !kernel_math::bounds_rect(c, r1, 99u, rect), "untracked / bad level");
    VsmBoundsRecord far = r1;
    far.center[0] = 1e7f;
    expect(!kernel_math::bounds_rect(c, far, 0, rect), "outside the window");
    VsmBoundsRecord huge = r1;
    huge.extent[0] = huge.extent[1] = huge.extent[2] = 1e20f;
    expect(kernel_math::bounds_rect(c, huge, 0, rect) && rect[2] - rect[0] == 127 && rect[3] - rect[1] == 127,
           "a huge object covers the whole window");
    VsmBoundsRecord nan = r1;
    nan.extent[0] = std::nanf("");
    expect(!kernel_math::bounds_rect(c, nan, 0, rect), "NaN bounds are ignored");
}

// --- frame -----------------------------------------------------------------------------------------
struct CpuFrame {
    VsmClipmap clipmap;
    VsmInvalidationReference inval;
    std::unique_ptr<VsmPageModel> model{new VsmPageModel()};
    std::vector<u32> req, inv, scratch;
    core_logic::VsmPageStats stats{};
    VsmFrameConstants c{};

    bool run(const Camera& cam, const f32 light[3], MiniScene& s) {
        if (!clipmap.build(makeView(cam, light), c)) {
            return false;
        }
        const std::vector<f32> depth = renderDepth(cam);
        req.assign(c.requestWords, 0u);
        inv.assign(c.requestWords, 0u);
        markReference(c, depth.data(), kW, kH, req.data(), fuse::kernel::Backend::CpuParallel, scratch);
        inval.run(c, s.inst.data(), s.xf.data(), static_cast<u32>(s.inst.size()), s.meshes.data(),
                  static_cast<u32>(s.meshes.size()), inv.data());
        return model->update(frameInput(c), req.data(), inv.data(), stats) == core_logic::ClStatus::Ok;
    }
};

void testFrame() {
    CpuFrame f;
    VsmClipmapDesc d{};
    d.firstLevelExtent = 4.f;
    d.levels = 12;
    d.texelsPerPixel = 4.f;
    expect(f.clipmap.init(d), "init");
    expect(f.model->reset(12, 1024) == core_logic::ClStatus::Ok, "model reset");
    MiniScene s = makeMiniScene();
    const f64 eye[3] = {0.7, 2.2, 3.0};
    const f64 at[3] = {0.5, 0.0, -25.0};
    const Camera cam = makeCamera(eye, at);
    expect(f.run(cam, kSun, s), "frame 1");
    const u32 firstRender = f.stats.toRender;
    expect(firstRender == f.stats.requested && f.stats.failed == 0u && firstRender > 20u, "frame 1 renders every requested page");
    expect(f.run(cam, kSun, s), "frame 2");
    expect(f.stats.toRender == 0u && f.stats.alreadyMapped == f.stats.requested, "static frame 2: 0 pages rendered");
    // Move one object: only pages under its old / new footprint re-render.
    s.xf[3].rows[0][3] += 0.4f;
    expect(f.run(cam, kSun, s), "frame 3");
    bool subset = true;
    for (u32 i = 0; i < f.model->render_count(); ++i) {
        const u32 v = f.model->render_page(i);
        subset = subset && ((f.inv[v / 32u] >> (v % 32u)) & 1u) != 0u;
    }
    std::printf("  frame: %u pages first frame, moving object re-renders %u (invalidated %u)\n", firstRender, f.stats.toRender,
                f.stats.invalidated);
    expect(subset && f.stats.toRender > 0u && f.stats.toRender < firstRender / 2u, "a moving object re-renders only its pages");
    expect(f.run(cam, kSun, s) && f.stats.toRender == 0u, "then static again");
    // Camera move by ~one level-0 page: only pages that scrolled in (or newly requested) render.
    Camera moved = cam;
    f64 eye2[3] = {eye[0] + 0.6, eye[1], eye[2]};
    moved = makeCamera(eye2, at);
    expect(f.run(moved, kSun, s), "frame 5");
    std::printf("  frame: camera move -> %u rendered, %u scrolled, %u newly allocated\n", f.stats.toRender, f.stats.scrolled,
                f.stats.allocated);
    // (Windows are ~63 pages wide around the camera while density keeps the visible pages near it, so a
    // small move rarely scrolls a requested page: new pages are what re-renders.)
    expect(f.stats.toRender <= f.stats.allocated + f.stats.scrolled && f.stats.toRender < firstRender / 2u,
           "a camera move re-renders only new / scrolled pages");
    // Light rotation: everything.
    const f32 sun2[3] = {0.3f, -1.f, -0.2f};
    expect(f.run(moved, sun2, s) && f.stats.toRender == f.stats.requested, "light rotation re-renders every requested page");
    expect(f.model->check_invariants(), "model invariants");
}

// --- api -------------------------------------------------------------------------------------------
void testApi() {
    const VsmCapabilities caps = queryVsmCapabilities(nullptr);
    expect(!caps.vsm && caps.reason != nullptr, "no device: not capable");
    VirtualShadowMap vsm;
    VirtualShadowMapDesc d{};
    expect(!vsm.init(d) && !vsm.valid(), "init without a device fails");
    VsmFrameDesc f{};
    expect(!vsm.beginFrame(1, f), "beginFrame on an invalid VSM fails");
    rg::Graph graph;
    const VsmGraphRefs refs = vsm.importInto(graph);
    expect(!refs.pageTable.valid() && !refs.pool.valid(), "no imports");
    vsm.addFrame(graph, refs, rg::TextureRef{}, gpu_scene::GpuSceneGraphRefs{});
    expect(vsm.stats().passes == 0u && vsm.collectRetired(~0ull) == 0u, "no passes");
    expect(std::strcmp(vsm.kernelLanguage(), "none") == 0, "no kernels");
    vsm.destroy();
}

} // namespace

int main(int argc, char** argv) {
    const std::string suite = argc > 1 ? argv[1] : "all";
    struct Suite {
        const char* name;
        void (*fn)();
    } suites[] = {{"layout", testLayout},         {"clipmap", testClipmap}, {"mark", testMark},
                  {"invalidate", testInvalidate}, {"frame", testFrame},     {"api", testApi}};
    bool ran = false;
    for (const Suite& s : suites) {
        if (suite == "all" || suite == s.name) {
            ran = true;
            std::printf("[%s]\n", s.name);
            s.fn();
        }
    }
    if (!ran) {
        std::fprintf(stderr, "unknown suite %s\n", suite.c_str());
        return 2;
    }
    std::printf("%s (%d failures)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
