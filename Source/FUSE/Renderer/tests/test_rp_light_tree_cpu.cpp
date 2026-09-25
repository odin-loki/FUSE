// WP-7.1 light tree CPU gates (stub-safe; the Lavapipe gates are test_rp_light_tree.cpp).
//
//   layout      record sizes / offsets; the GLSL and Slang mirrors (lt_common.{glsl,slang}) of every record
//               declare the same fields in the same order at the same offsets
//   pmf_sum     the selection pmf is a distribution at every shading point: sum over all lights == 1 (mixed
//               kinds incl. directional, 10.7k emissive triangles), pmf(sample.light) == sample.pmf bit for bit,
//               the pmf equals an independent double-precision enumeration of the tree, structural checks
//               (bit trails, depth cap, conservative bounds)
//   area_pdf    the continuous density pmf x pdfArea over emissive surfaces (triangles, rectangles, disks)
//               integrates to 1 (Monte Carlo over uniform surface points), E_tree[1 / p] == visible area and
//               E_tree[g / p] == the integral of g (the returned points follow the returned density), the point
//               samplers are uniform (chi-square) and land on the light
//   histogram   sampling histograms vs the brute-force pmf (chi-square), and vs the flat brute-force
//               importance (power x bound-based, per light): exact for two lights (chi-square), total-variation
//               bound for the mixed (<= 0.6) and clustered (<= 0.35) scenes (the tree's pmf is a product of
//               cluster-level approximations, not the per-light importance)
//   refit       refit of unchanged lights reproduces the build bit for bit; after moves / power changes the
//               refitted bounds stay conservative and the pmf still sums to 1; rebuild after adds / removes;
//               refit refuses a different light set; versions change
//   adapter     WP-1.1 GpuScene (CPU-only mode): analytic + WP-2.2 area lights, emissive meshlet triangles
//   zero_alloc  steady-state build / refit / sample / pmf make no heap allocation (replaced operator new)
#include <fuse/renderer/geometry/meshlet_builder.hpp>
#include <fuse/renderer/gpu_scene/gpu_scene.hpp>
#include <fuse/renderer/light_tree/light_tree.hpp>
#include <fuse/renderer/light_tree/light_tree_gpu.hpp>
#include <fuse/renderer/light_tree/light_tree_kernel.hpp>
#include <fuse/renderer/lighting/ltc/ltc_lut.hpp>
#include <fuse/renderer/material/material.hpp>

#include "test_rp_light_tree_scene.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <new>
#include <sstream>
#include <string>
#include <vector>

// --- allocation counter (operator new) -----------------------------------------------------------
namespace {
thread_local bool t_count = false;
thread_local unsigned long long t_allocations = 0;
} // namespace

// GCC may inline the replacement operators into callers and then report a false
// -Wmismatched-new-delete at -O2; replacement functions must not be inline anyway.
#if defined(__GNUC__)
#define FUSE_TEST_REPLACEMENT_NOINLINE __attribute__((noinline))
#else
#define FUSE_TEST_REPLACEMENT_NOINLINE
#endif

FUSE_TEST_REPLACEMENT_NOINLINE void* operator new(std::size_t size) {
    if (t_count) {
        ++t_allocations;
    }
    void* p = std::malloc(size == 0 ? 1 : size);
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    return p;
}
FUSE_TEST_REPLACEMENT_NOINLINE void* operator new[](std::size_t size) { return ::operator new(size); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete(void* p) noexcept { std::free(p); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete[](void* p) noexcept { std::free(p); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete(void* p, std::size_t) noexcept { std::free(p); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {

using namespace fuse::renderer;
using namespace fuse::renderer::light_tree;
using fuse::f32;
using fuse::f64;
using fuse::u32;
using fuse::u64;
using fuse::u8;
using fuse::usize;
using lt_test::Rng;
using lt_test::ShadingPoint;

int g_failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

u32 bits(f32 v) {
    u32 b = 0;
    std::memcpy(&b, &v, 4);
    return b;
}

// --- layout ----------------------------------------------------------------------------------------
struct Field {
    const char* name;
    size_t offset;
};
#define LT_FIELD(T, n) Field{#n, offsetof(T, n)}
const Field kNodeFields[] = {LT_FIELD(LightTreeNode, boundsMin), LT_FIELD(LightTreeNode, phi),
                             LT_FIELD(LightTreeNode, boundsMax), LT_FIELD(LightTreeNode, childOrEmitter),
                             LT_FIELD(LightTreeNode, axis),      LT_FIELD(LightTreeNode, cosThetaO),
                             LT_FIELD(LightTreeNode, cosThetaE), LT_FIELD(LightTreeNode, sinThetaO),
                             LT_FIELD(LightTreeNode, flags),     LT_FIELD(LightTreeNode, depth)};
const Field kEmitterFields[] = {LT_FIELD(LightTreeEmitter, p0),       LT_FIELD(LightTreeEmitter, kind),
                                LT_FIELD(LightTreeEmitter, e1),       LT_FIELD(LightTreeEmitter, area),
                                LT_FIELD(LightTreeEmitter, e2),       LT_FIELD(LightTreeEmitter, source),
                                LT_FIELD(LightTreeEmitter, normal),   LT_FIELD(LightTreeEmitter, flags),
                                LT_FIELD(LightTreeEmitter, leafNode), LT_FIELD(LightTreeEmitter, bitTrail),
                                LT_FIELD(LightTreeEmitter, depth),    LT_FIELD(LightTreeEmitter, power)};
const Field kHeaderFields[] = {LT_FIELD(LightTreeHeader, nodes),        LT_FIELD(LightTreeHeader, emitters),
                               LT_FIELD(LightTreeHeader, directional),  LT_FIELD(LightTreeHeader, nodeCount),
                               LT_FIELD(LightTreeHeader, emitterCount), LT_FIELD(LightTreeHeader, directionalCount),
                               LT_FIELD(LightTreeHeader, version),      LT_FIELD(LightTreeHeader, reserved)};
const Field kQueryFields[] = {LT_FIELD(LightTreeQuery, position), LT_FIELD(LightTreeQuery, u0),
                              LT_FIELD(LightTreeQuery, normal),   LT_FIELD(LightTreeQuery, u1),
                              LT_FIELD(LightTreeQuery, u2),       LT_FIELD(LightTreeQuery, evalLight),
                              LT_FIELD(LightTreeQuery, reserved)};
const Field kSampleFields[] = {LT_FIELD(LightTreeSample, light),   LT_FIELD(LightTreeSample, pmf),
                               LT_FIELD(LightTreeSample, pdfArea), LT_FIELD(LightTreeSample, kind),
                               LT_FIELD(LightTreeSample, position), LT_FIELD(LightTreeSample, evalPmf)};
const Field kPushFields[] = {LT_FIELD(LightTreePush, header), LT_FIELD(LightTreePush, queries),
                             LT_FIELD(LightTreePush, results), LT_FIELD(LightTreePush, count),
                             LT_FIELD(LightTreePush, reserved)};
#undef LT_FIELD

std::string readText(const std::string& path) {
    std::ifstream file(path);
    std::stringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

/// Parses `struct <name> {...};` of a shader source: (name, std430 / scalar offset) per field.
bool parseShaderStruct(const std::string& text, const std::string& name, std::vector<size_t>& offsets, size_t& size,
                       std::vector<std::string>& names) {
    const std::string key = "struct " + name + " {";
    const size_t begin = text.find(key);
    const size_t end = text.find("};", begin);
    if (begin == std::string::npos || end == std::string::npos) {
        return false;
    }
    std::istringstream body(text.substr(begin + key.size(), end - begin - key.size()));
    std::string line;
    size_t offset = 0;
    size_t align = 4;
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
        align = std::max(align, bytes);
        offset = (offset + bytes - 1u) / bytes * bytes;
        names.push_back(decl);
        offsets.push_back(offset);
        offset += bytes * count;
    }
    size = (offset + align - 1u) / align * align;
    return true;
}

template <size_t N>
void checkStruct(const std::string& text, const char* lang, const char* shaderName, const Field (&fields)[N],
                 size_t cppSize) {
    std::vector<size_t> offsets;
    std::vector<std::string> names;
    size_t size = 0;
    const bool parsed = parseShaderStruct(text, shaderName, offsets, size, names);
    expect(parsed, "shader struct parsed");
    if (!parsed) {
        std::fprintf(stderr, "  %s: struct %s missing\n", lang, shaderName);
        return;
    }
    bool same = offsets.size() == N && size == cppSize;
    for (size_t i = 0; same && i < N; ++i) {
        same = names[i] == fields[i].name && offsets[i] == fields[i].offset;
        if (!same) {
            std::fprintf(stderr, "  %s %s field %zu: shader %s @%zu vs C++ %s @%zu\n", lang, shaderName, i,
                         names[i].c_str(), offsets[i], fields[i].name, fields[i].offset);
        }
    }
    if (offsets.size() != N || size != cppSize) {
        std::fprintf(stderr, "  %s %s: %zu fields / %zu bytes vs C++ %zu / %zu\n", lang, shaderName, offsets.size(), size,
                     N, cppSize);
    }
    std::printf("layout: %s %s %zu fields, %zu bytes\n", lang, shaderName, offsets.size(), size);
    expect(same, "shader struct == C++ record (names, order, offsets, size)");
}

void testLayout() {
    expect(sizeof(LightTreeNode) == 64u && sizeof(LightTreeEmitter) == 80u && sizeof(LightTreeHeader) == 48u &&
               sizeof(LightTreeQuery) == 48u && sizeof(LightTreeSample) == 32u && sizeof(LightTreePush) == 32u,
           "record sizes");
    const std::string dir = FUSE_RP_LIGHT_TREE_SHADER_DIR;
    for (const char* lang : {"glsl", "slang"}) {
        const std::string text = readText(dir + "/lt_common." + lang);
        expect(!text.empty(), "lt_common source readable");
        checkStruct(text, lang, "LtNode", kNodeFields, sizeof(LightTreeNode));
        checkStruct(text, lang, "LtEmitter", kEmitterFields, sizeof(LightTreeEmitter));
        checkStruct(text, lang, "LtHeader", kHeaderFields, sizeof(LightTreeHeader));
        checkStruct(text, lang, "LtQuery", kQueryFields, sizeof(LightTreeQuery));
        checkStruct(text, lang, "LtSampleResult", kSampleFields, sizeof(LightTreeSample));
        const std::string kernel = readText(dir + "/lt_sample." + std::string(std::strcmp(lang, "glsl") == 0 ? "comp" : "slang"));
        if (std::strcmp(lang, "slang") == 0) {
            checkStruct(kernel, lang, "LtPush", kPushFields, sizeof(LightTreePush));
        } else {
            // GLSL push block: `uniform LtPush {` ... `} pc;`
            std::string block = kernel;
            const size_t at = block.find("uniform LtPush {");
            expect(at != std::string::npos, "GLSL push block present");
            if (at != std::string::npos) {
                block.replace(at, std::strlen("uniform LtPush {"), "struct LtPush {");
                const size_t close = block.find("} pc;", at);
                if (close != std::string::npos) {
                    block.replace(close, 5, "};");
                }
                checkStruct(block.substr(at), lang, "LtPush", kPushFields, sizeof(LightTreePush));
            }
        }
    }
    const LightTreeSlotLayout l = LightTreeSlotLayout::compute(19u, 10u, 3u);
    expect(l.header == 0u && l.nodes == 256u && l.emitters % 256u == 0u && l.directional % 256u == 0u &&
               l.bytes >= l.directional + 12u && l.nodes >= sizeof(LightTreeHeader) && l.emitters >= l.nodes + 19u * 64u,
           "slot layout sections 256-aligned, disjoint");
    expect(!queryLightTreeCapabilities(nullptr).gpu, "no device: not capable");
    LightTreeGpu gpu;
    expect(!gpu.init(LightTreeGpuDesc{}) && !gpu.valid(), "init without a device fails cleanly");
}

// --- double-precision reference ---------------------------------------------------------------------

f64 refImportance(const LightTreeNode& node, const f32 (&p)[3], const f32 (&n)[3]) {
    f64 c[3], d[3], g[3];
    for (u32 i = 0; i < 3u; ++i) {
        c[i] = (static_cast<f64>(node.boundsMin[i]) + node.boundsMax[i]) * 0.5;
        d[i] = p[i] - c[i];
        g[i] = static_cast<f64>(node.boundsMax[i]) - node.boundsMin[i];
    }
    const f64 dist2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
    const f64 radius2 = (g[0] * g[0] + g[1] * g[1] + g[2] * g[2]) * 0.25;
    const f64 d2 = std::max({dist2, radius2, 1e-12});
    f64 w[3] = {0.0, 0.0, 0.0};
    if (dist2 > 0.0) {
        const f64 len = std::sqrt(dist2);
        for (u32 i = 0; i < 3u; ++i) {
            w[i] = d[i] / len;
        }
    }
    f64 cosW = node.axis[0] * w[0] + node.axis[1] * w[1] + node.axis[2] * w[2];
    if ((node.flags & kLtFlagTwoSided) != 0u) {
        cosW = std::fabs(cosW);
    }
    auto angle = [](f64 c) { return std::acos(std::clamp(c, -1.0, 1.0)); };
    const f64 thetaW = angle(cosW);
    const f64 thetaO = angle(node.cosThetaO);
    const f64 thetaB = dist2 > radius2 ? std::asin(std::sqrt(radius2 / dist2)) : 3.14159265358979323846;
    const f64 thetaP = std::max(0.0, thetaW - thetaO - thetaB);
    if (std::cos(thetaP) <= node.cosThetaE) {
        return 0.0;
    }
    f64 imp = node.phi * std::cos(thetaP) / d2;
    if (lt_has_normal(n)) {
        const f64 cosI = std::fabs(w[0] * n[0] + w[1] * n[1] + w[2] * n[2]);
        const f64 thetaI = std::max(0.0, angle(cosI) - thetaB);
        imp *= std::cos(thetaI);
    }
    return std::max(imp, 0.0);
}

/// pmf of every light by enumerating the tree (independent of lt_sample / lt_pmf).
void refPmf(const LightTree& tree, const f32 (&p)[3], const f32 (&n)[3], std::vector<f64>& out) {
    const LightTreeView v = tree.view();
    out.assign(v.emitterCount, 0.0);
    const u32 total = v.directionalCount + (v.nodeCount > 0u ? 1u : 0u);
    if (total == 0u) {
        return;
    }
    const f64 pDir = static_cast<f64>(v.directionalCount) / total;
    for (u32 d = 0; d < v.directionalCount; ++d) {
        out[v.directional[d]] = pDir / v.directionalCount;
    }
    if (v.nodeCount == 0u) {
        return;
    }
    struct Item {
        u32 node;
        f64 pmf;
    };
    std::vector<Item> stack{{0u, 1.0 - pDir}};
    while (!stack.empty()) {
        const Item it = stack.back();
        stack.pop_back();
        const LightTreeNode& node = v.nodes[it.node];
        if ((node.flags & kLtFlagLeaf) != 0u) {
            out[node.childOrEmitter] = it.pmf;
            continue;
        }
        const f64 i0 = refImportance(v.nodes[it.node + 1u], p, n);
        const f64 i1 = refImportance(v.nodes[node.childOrEmitter], p, n);
        const f64 p0 = i0 + i1 > 0.0 ? i0 / (i0 + i1) : 0.5;
        stack.push_back({it.node + 1u, it.pmf * p0});
        stack.push_back({node.childOrEmitter, it.pmf * (1.0 - p0)});
    }
}

// --- structural checks ------------------------------------------------------------------------------

/// Angle between two (nearly unit) float vectors, robust for tiny angles.
f64 angleBetween(const f32* a, const f32* b) {
    const f64 x[3] = {a[0], a[1], a[2]};
    const f64 y[3] = {b[0], b[1], b[2]};
    const f64 c[3] = {x[1] * y[2] - x[2] * y[1], x[2] * y[0] - x[0] * y[2], x[0] * y[1] - x[1] * y[0]};
    const f64 s = std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
    const f64 d = x[0] * y[0] + x[1] * y[1] + x[2] * y[2];
    return std::atan2(s, d);
}

/// Every node contains its children (box, cone, phi, cosThetaE), bit trails lead to the leaves, depth <= cap.
bool checkStructure(const LightTree& tree, const char* what) {
    const LightTreeView v = tree.view();
    bool ok = true;
    u32 leaves = 0;
    for (u32 i = 0; i < v.nodeCount; ++i) {
        const LightTreeNode& node = v.nodes[i];
        ok = ok && node.depth <= kLtMaxDepth;
        if ((node.flags & kLtFlagLeaf) != 0u) {
            ++leaves;
            ok = ok && node.childOrEmitter < v.emitterCount && v.emitters[node.childOrEmitter].leafNode == i;
            continue;
        }
        const u32 kids[2] = {i + 1u, node.childOrEmitter};
        f64 phi = 0.0;
        for (u32 k : kids) {
            if (k >= v.nodeCount) {
                return false;
            }
            const LightTreeNode& c = v.nodes[k];
            ok = ok && c.depth == node.depth + 1u;
            for (u32 a = 0; a < 3u; ++a) {
                ok = ok && c.boundsMin[a] >= node.boundsMin[a] && c.boundsMax[a] <= node.boundsMax[a];
            }
            phi += c.phi;
            ok = ok && c.cosThetaE >= node.cosThetaE;
            const f64 thetaParent = std::acos(std::clamp(static_cast<f64>(node.cosThetaO), -1.0, 1.0));
            const f64 thetaChild = std::acos(std::clamp(static_cast<f64>(c.cosThetaO), -1.0, 1.0));
            if (node.cosThetaO > -1.f) {
                // Stored cones: float cos near 1 resolves angles only to ~3.5e-4 rad, so a child's stored cone can
                // be that much wider than its content (the emitter check below is the exact one).
                const bool contained = angleBetween(node.axis, c.axis) + thetaChild <= thetaParent + 5e-4;
                if (!contained && ok) {
                    std::fprintf(stderr, "  %s: node %u cone does not contain child %u (%g + %g > %g)\n", what, i, k,
                                 angleBetween(node.axis, c.axis), thetaChild, thetaParent);
                }
                ok = ok && contained;
            }
        }
        ok = ok && std::fabs(phi - node.phi) <= 1e-5 * std::max(1.0, phi);
    }
    ok = ok && leaves == tree.stats().treeLights;
    for (u32 e = 0; e < v.emitterCount; ++e) {
        const LightTreeEmitter& em = v.emitters[e];
        if (em.kind == kLtKindDirectional) {
            ok = ok && em.leafNode == kLtInvalid && em.depth < v.directionalCount && v.directional[em.depth] == e;
            continue;
        }
        // The emitter's own orientation cone (area: its normal; spot: axis + inner angle; point: everything) lies
        // in every ancestor's stored cone, and its box in every ancestor's box.
        f64 thetaEmitter = 0.0;
        if (em.kind == kLtKindPoint) {
            thetaEmitter = 3.14159265358979323846;
        } else if (em.kind == kLtKindSpot) {
            thetaEmitter = std::acos(std::clamp(static_cast<f64>(v.nodes[em.leafNode].cosThetaO), -1.0, 1.0));
        }
        u32 node = 0;
        for (u32 d = 0; d <= em.depth && node < v.nodeCount; ++d) {
            const LightTreeNode& anc = v.nodes[node];
            if (anc.cosThetaO > -1.f && (anc.flags & kLtFlagLeaf) == 0u) {
                const f64 thetaAnc = std::acos(std::clamp(static_cast<f64>(anc.cosThetaO), -1.0, 1.0));
                const f32* axis = em.kind == kLtKindSpot ? v.nodes[em.leafNode].axis : em.normal;
                const bool inCone = angleBetween(anc.axis, axis) + thetaEmitter <= thetaAnc + 1e-6;
                if (!inCone && ok) {
                    std::fprintf(stderr, "  %s: emitter %u (kind %u) outside the cone of ancestor %u\n", what, e, em.kind,
                                 node);
                }
                ok = ok && inCone;
            }
            for (u32 a = 0; a < 3u; ++a) {
                ok = ok && em.p0[a] >= anc.boundsMin[a] && em.p0[a] <= anc.boundsMax[a];
            }
            if (d == em.depth) {
                break;
            }
            node = ((em.bitTrail >> d) & 1u) == 0u ? node + 1u : v.nodes[node].childOrEmitter;
        }
        ok = ok && node == em.leafNode && v.nodes[node].depth == em.depth;
    }
    if (!ok) {
        std::fprintf(stderr, "  %s: structure check failed\n", what);
    }
    return ok;
}

// --- pmf_sum -------------------------------------------------------------------------------------------

void checkPmfSum(const LightTree& tree, const std::vector<ShadingPoint>& points, const char* what) {
    Rng rng(77);
    const LightTreeView v = tree.view();
    f64 worstSum = 0.0;
    f64 worstTv = 0.0;
    u32 bitMismatch = 0;
    u32 samples = 0;
    u32 invalid = 0;
    std::vector<f64> ref;
    for (const ShadingPoint& s : points) {
        f64 sum = 0.0;
        for (u32 e = 0; e < v.emitterCount; ++e) {
            sum += lt_pmf(v, s.p, s.n, e);
        }
        worstSum = std::max(worstSum, std::fabs(sum - 1.0));
        refPmf(tree, s.p, s.n, ref);
        f64 tv = 0.0;
        for (u32 e = 0; e < v.emitterCount; ++e) {
            tv += std::fabs(ref[e] - lt_pmf(v, s.p, s.n, e));
        }
        worstTv = std::max(worstTv, 0.5 * tv);
        for (u32 k = 0; k < 64u; ++k) {
            const LightTreeSample smp = tree.sample(s.p, s.n, rng.uniform(), rng.uniform(), rng.uniform());
            ++samples;
            if (smp.light == kLtInvalid || !(smp.pmf > 0.f)) {
                ++invalid;
                continue;
            }
            if (bits(tree.pmf(s.p, s.n, smp.light)) != bits(smp.pmf)) {
                ++bitMismatch;
            }
        }
    }
    std::printf("pmf_sum %s: %u lights (%u nodes, depth %u, %u median splits), %zu points: max |sum - 1| = %.3g, "
                "max TV(pmf, double reference) = %.3g; %u samples, %u pmf bit mismatches, %u invalid\n",
                what, v.emitterCount, v.nodeCount, tree.stats().maxDepth, tree.stats().medianSplits, points.size(),
                worstSum, worstTv, samples, bitMismatch, invalid);
    expect(worstSum < 2e-5, "sum over lights of the selection pmf == 1 at every shading point");
    expect(worstTv < 1e-3, "pmf == the double-precision enumeration of the tree");
    expect(bitMismatch == 0u, "pmf(sample.light) == sample.pmf bit for bit");
    expect(invalid == 0u, "every sample returns a light with pmf > 0");
}

void testPmfSum() {
    {
        const std::vector<LightTreeLight> lights = lt_test::mixedScene(1);
        LightTree tree;
        expect(tree.build(lights), "mixed build");
        expect(checkStructure(tree, "mixed"), "mixed: conservative, consistent structure");
        expect(tree.stats().directional == 2u && tree.stats().treeLights + 2u == lights.size(), "directional split");
        checkPmfSum(tree, lt_test::shadingPoints(5, 64, 12.f), "mixed");
    }
    {
        const std::vector<LightTreeLight> lights = lt_test::triangleScene(2, 71);
        LightTree tree;
        expect(tree.build(lights), "triangles build");
        expect(lights.size() > 10000u, "10k+ emissive triangles");
        expect(checkStructure(tree, "triangles"), "triangles: conservative, consistent structure");
        checkPmfSum(tree, lt_test::shadingPoints(6, 24, 8.f), "10k triangles");
    }
    {
        // Degenerate: every light at the same point (no SAH split possible -> median), a single light, only
        // directional lights, empty.
        std::vector<LightTreeLight> same;
        for (u32 i = 0; i < 37u; ++i) {
            const f32 p[3] = {1.f, 2.f, 3.f};
            same.push_back(makePointLight(p, 1.f + static_cast<f32>(i), i));
        }
        LightTree tree;
        expect(tree.build(same), "coincident build");
        expect(checkStructure(tree, "coincident") && tree.stats().medianSplits > 0u, "coincident lights: median splits");
        checkPmfSum(tree, lt_test::shadingPoints(7, 8, 5.f), "coincident");
        LightTree one;
        const f32 p[3] = {0.f, 1.f, 0.f};
        one.build(std::vector<LightTreeLight>{makePointLight(p, 2.f)});
        const f32 q[3] = {0.f, 0.f, 0.f};
        const f32 none[3] = {0.f, 0.f, 0.f};
        const LightTreeSample s = one.sample(q, none, 0.5f, 0.5f, 0.5f);
        expect(s.light == 0u && s.pmf == 1.f && s.kind == kLtKindPoint && s.pdfArea == 0.f && s.position[1] == 1.f,
               "single light: pmf 1");
        LightTree dirOnly;
        const f32 d[3] = {0.f, -1.f, 0.f};
        dirOnly.build(std::vector<LightTreeLight>{makeDirectionalLight(d, 1.f), makeDirectionalLight(d, 2.f)});
        const LightTreeSample ds = dirOnly.sample(q, none, 0.75f, 0.f, 0.f);
        expect(ds.light == 1u && ds.pmf == 0.5f && ds.kind == kLtKindDirectional && ds.position[1] == -1.f,
               "directional only: uniform");
        LightTree empty;
        empty.build(nullptr, 0u);
        expect(empty.sample(q, none, 0.5f, 0.5f, 0.5f).light == kLtInvalid && empty.pmf(q, none, 0u) == 0.f,
               "empty tree: no light");
    }
    {
        // Depth cap: a chain-prone distribution (geometric spacing and powers along a line) at 5000 lights.
        std::vector<LightTreeLight> chain;
        for (u32 i = 0; i < 5000u; ++i) {
            const f32 p[3] = {std::pow(1.002f, static_cast<f32>(i)), 0.f, 0.f};
            chain.push_back(makePointLight(p, std::pow(1.003f, static_cast<f32>(i)), i));
        }
        LightTree tree;
        tree.build(chain);
        expect(checkStructure(tree, "chain") && tree.stats().maxDepth <= kLtMaxDepth, "depth cap holds");
        checkPmfSum(tree, lt_test::shadingPoints(8, 8, 200.f), "chain");
    }
}

// --- area_pdf ------------------------------------------------------------------------------------------

/// Chi-square statistic of `counts` against `probs` (N samples; bins with expected < 5 merged).
f64 chiSquare(const std::vector<u64>& counts, const std::vector<f64>& probs, u64 n, u32& dof) {
    f64 chi = 0.0;
    f64 restE = 0.0;
    f64 restO = 0.0;
    u32 bins = 0;
    for (usize i = 0; i < counts.size(); ++i) {
        const f64 e = probs[i] * static_cast<f64>(n);
        if (e < 5.0) {
            restE += e;
            restO += static_cast<f64>(counts[i]);
            continue;
        }
        const f64 d = static_cast<f64>(counts[i]) - e;
        chi += d * d / e;
        ++bins;
    }
    if (restE >= 5.0) {
        const f64 d = restO - restE;
        chi += d * d / restE;
        ++bins;
    } else if (restO > 20.0) {
        chi += 1e9; // samples where the reference expects (almost) none
    }
    dof = bins > 0u ? bins - 1u : 0u;
    return chi;
}

bool chiOk(f64 chi, u32 dof) { return chi <= static_cast<f64>(dof) + 6.0 * std::sqrt(2.0 * std::max(dof, 1u)) + 10.0; }

void testPointSamplers() {
    Rng rng(99);
    const u64 n = 200000;
    // Triangle: 4 midpoint sub-triangles of equal area.
    {
        const f32 a[3] = {0.f, 0.f, 0.f}, b[3] = {2.f, 0.f, 0.f}, c[3] = {0.5f, 1.5f, 0.f};
        const LightTreeEmitter e = makeEmitter(makeTriangleLight(a, b, c, 1.f));
        std::vector<u64> counts(4, 0);
        bool onLight = true;
        for (u64 i = 0; i < n; ++i) {
            f32 p[3];
            f32 pdf = 0.f;
            lt_sample_point(e, rng.uniform(), rng.uniform(), p, pdf);
            // Barycentrics via the 2D edge solve (z = 0).
            const f64 det = static_cast<f64>(e.e1[0]) * e.e2[1] - static_cast<f64>(e.e1[1]) * e.e2[0];
            const f64 x = p[0] - a[0], y = p[1] - a[1];
            const f64 s = (x * e.e2[1] - y * e.e2[0]) / det;
            const f64 t = (e.e1[0] * y - e.e1[1] * x) / det;
            onLight = onLight && s >= -1e-6 && t >= -1e-6 && s + t <= 1.0 + 1e-6 && p[2] == 0.f && pdf == 1.f / e.area;
            const u32 bin = s > 0.5 ? 0u : (t > 0.5 ? 1u : (s + t < 0.5 ? 2u : 3u));
            ++counts[bin];
        }
        u32 dof = 0;
        const f64 chi = chiSquare(counts, std::vector<f64>(4, 0.25), n, dof);
        std::printf("area_pdf: triangle point sampler chi2 %.2f (dof %u)\n", chi, dof);
        expect(onLight && chiOk(chi, dof), "triangle points: on the triangle, uniform");
        expect(std::fabs(e.area - 1.5f) < 1e-6f, "triangle area");
    }
    // Rectangle 4 x 4 grid, disk 4 equal-area rings x 8 sectors.
    for (u32 kind : {kLtKindRect, kLtKindDisk}) {
        const f32 c[3] = {1.f, 2.f, 3.f}, u[3] = {2.f, 0.f, 0.f}, v[3] = {0.f, 0.5f, 0.f};
        const LightTreeEmitter e = makeEmitter(kind == kLtKindRect ? makeRectLight(c, u, v, 1.f) : makeDiskLight(c, u, v, 1.f));
        std::vector<u64> counts(kind == kLtKindRect ? 16u : 32u, 0);
        bool onLight = true;
        for (u64 i = 0; i < n; ++i) {
            f32 p[3];
            f32 pdf = 0.f;
            lt_sample_point(e, rng.uniform(), rng.uniform(), p, pdf);
            const f64 x = (p[0] - c[0]) / 2.0;
            const f64 y = (p[1] - c[1]) / 0.5;
            onLight = onLight && p[2] == 3.f && pdf == 1.f / e.area;
            if (kind == kLtKindRect) {
                onLight = onLight && std::fabs(x) <= 1.0 + 1e-6 && std::fabs(y) <= 1.0 + 1e-6;
                const u32 bx = std::min(3u, static_cast<u32>((x + 1.0) * 2.0));
                const u32 by = std::min(3u, static_cast<u32>((y + 1.0) * 2.0));
                ++counts[by * 4u + bx];
            } else {
                const f64 r2 = x * x + y * y;
                onLight = onLight && r2 <= 1.0 + 1e-5;
                const u32 ring = std::min(3u, static_cast<u32>(r2 * 4.0));
                const f64 phi = std::atan2(y, x) + 3.14159265358979323846;
                const u32 sector = std::min(7u, static_cast<u32>(phi / (2.0 * 3.14159265358979323846) * 8.0));
                ++counts[ring * 8u + sector];
            }
        }
        u32 dof = 0;
        const f64 chi = chiSquare(counts, std::vector<f64>(counts.size(), 1.0 / static_cast<f64>(counts.size())), n, dof);
        std::printf("area_pdf: %s point sampler chi2 %.2f (dof %u)\n", kind == kLtKindRect ? "rectangle" : "disk", chi, dof);
        expect(onLight && chiOk(chi, dof), "rectangle / disk points: on the light, uniform");
        const f32 area = kind == kLtKindRect ? 4.f : 3.14159265f;
        expect(std::fabs(e.area - area) < 1e-5f, "rectangle / disk area");
    }
    // The polynomial sin / cos.
    f64 worst = 0.0;
    for (u32 i = 0; i <= 1000u; ++i) {
        const f32 t = -0.78539816f + 1.57079632f * static_cast<f32>(i) / 1000.f;
        worst = std::max(worst, std::fabs(lt_sin_poly(t) - std::sin(static_cast<f64>(t))));
        worst = std::max(worst, std::fabs(lt_cos_poly(t) - std::cos(static_cast<f64>(t))));
    }
    std::printf("area_pdf: polynomial sin / cos max error %.3g on [-pi/4, pi/4]\n", worst);
    expect(worst < 1e-6, "polynomial sin / cos accuracy");
}

void testAreaPdf() {
    testPointSamplers();
    // Area emitters only (the delta lights have no surface density).
    std::vector<LightTreeLight> lights = lt_test::triangleScene(3, 24);
    {
        Rng rng(4);
        for (u32 i = 0; i < 40u; ++i) {
            f32 c[3] = {rng.range(-6.f, 6.f), rng.range(-2.f, 3.f), rng.range(-6.f, 6.f)};
            f32 n[3], u[3], v[3];
            lt_test::unitVector(rng, n);
            lt_test::planeAxes(n, rng.range(0.1f, 0.6f), rng.range(0.1f, 0.6f), u, v);
            lights.push_back((i % 2u) == 0u ? makeRectLight(c, u, v, 3.f, (i % 4u) == 0u)
                                            : makeDiskLight(c, u, v, 3.f, (i % 4u) == 1u));
        }
    }
    LightTree tree;
    tree.build(lights);
    const LightTreeView v = tree.view();
    f64 totalArea = 0.0;
    std::vector<f64> cdf;
    for (u32 e = 0; e < v.emitterCount; ++e) {
        totalArea += v.emitters[e].area;
        cdf.push_back(totalArea);
    }
    const std::vector<ShadingPoint> points = lt_test::shadingPoints(10, 6, 5.f);
    Rng rng(11);
    const u32 n = 200000;
    for (u32 k = 0; k < points.size(); ++k) {
        const ShadingPoint& s = points[k];
        // (1) Uniform points over the total emitter area: E[p(X) x A] == integral of p == 1.
        f64 mean = 0.0, m2 = 0.0;
        for (u32 i = 0; i < n; ++i) {
            const f64 r = static_cast<f64>(rng.uniform()) * totalArea;
            const u32 e = static_cast<u32>(std::min<usize>(std::upper_bound(cdf.begin(), cdf.end(), r) - cdf.begin(),
                                                           cdf.size() - 1u));
            f32 x[3];
            f32 pdfArea = 0.f;
            lt_sample_point(v.emitters[e], rng.uniform(), rng.uniform(), x, pdfArea);
            const f64 value = static_cast<f64>(tree.pmf(s.p, s.n, e)) * pdfArea * totalArea;
            const f64 d = value - mean;
            mean += d / (i + 1.0);
            m2 += d * (value - mean);
        }
        const f64 se = std::sqrt(m2 / (n - 1.0) / n);
        // (2) Tree samples: E[1 / p] == area with p > 0, E[g / p] == integral of g, g = |x - s|^2 / (1 + |x|^2).
        f64 visibleArea = 0.0;
        f64 gIntegral = 0.0;
        {
            // Reference integral of g over the emitters with pmf > 0 (uniform points, 4x the samples).
            f64 gMean = 0.0;
            u32 kept = 0;
            for (u32 e = 0; e < v.emitterCount; ++e) {
                if (tree.pmf(s.p, s.n, e) > 0.f) {
                    visibleArea += v.emitters[e].area;
                }
            }
            for (u32 i = 0; i < 4u * n; ++i) {
                const f64 r = static_cast<f64>(rng.uniform()) * totalArea;
                const u32 e = static_cast<u32>(std::min<usize>(std::upper_bound(cdf.begin(), cdf.end(), r) - cdf.begin(),
                                                               cdf.size() - 1u));
                f32 x[3];
                f32 pdfArea = 0.f;
                lt_sample_point(v.emitters[e], rng.uniform(), rng.uniform(), x, pdfArea);
                const f64 dx = x[0] - s.p[0], dy = x[1] - s.p[1], dz = x[2] - s.p[2];
                const f64 g = (dx * dx + dy * dy + dz * dz) / (1.0 + x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
                gMean += tree.pmf(s.p, s.n, e) > 0.f ? g : 0.0;
                ++kept;
            }
            gIntegral = gMean / kept * totalArea;
        }
        f64 invMean = 0.0, invM2 = 0.0, gEst = 0.0, gM2 = 0.0;
        for (u32 i = 0; i < n; ++i) {
            const LightTreeSample smp = tree.sample(s.p, s.n, rng.uniform(), rng.uniform(), rng.uniform());
            const f64 density = static_cast<f64>(smp.pmf) * smp.pdfArea;
            const f64 inv = density > 0.0 ? 1.0 / density : 0.0;
            const f64 dx = smp.position[0] - s.p[0], dy = smp.position[1] - s.p[1], dz = smp.position[2] - s.p[2];
            const f64 g = (dx * dx + dy * dy + dz * dz) /
                          (1.0 + static_cast<f64>(smp.position[0]) * smp.position[0] +
                           static_cast<f64>(smp.position[1]) * smp.position[1] +
                           static_cast<f64>(smp.position[2]) * smp.position[2]);
            const f64 di = inv - invMean;
            invMean += di / (i + 1.0);
            invM2 += di * (inv - invMean);
            const f64 dg = g * inv - gEst;
            gEst += dg / (i + 1.0);
            gM2 += dg * (g * inv - gEst);
        }
        const f64 invSe = std::sqrt(invM2 / (n - 1.0) / n);
        const f64 gSe = std::sqrt(gM2 / (n - 1.0) / n);
        std::printf("area_pdf point %u (%s): integral of p = %.5f +- %.5f; E[1/p] = %.3f +- %.3f (visible area %.3f of "
                    "%.3f); E[g/p] = %.3f +- %.3f (reference %.3f)\n",
                    k, lt_has_normal(s.n) ? "surface" : "volume", mean, se, invMean, invSe, visibleArea, totalArea, gEst,
                    gSe, gIntegral);
        expect(std::fabs(mean - 1.0) <= 5.0 * se + 1e-4, "the continuous pdf over the emitter area integrates to 1");
        expect(std::fabs(invMean - visibleArea) <= 5.0 * invSe + 1e-3 * visibleArea, "E_tree[1 / p] == visible area");
        expect(std::fabs(gEst - gIntegral) <= 5.0 * gSe + 0.01 * std::fabs(gIntegral), "E_tree[g / p] == integral of g");
    }
}

// --- histogram ------------------------------------------------------------------------------------

/// Flat brute-force importance of every light at (p, n): the importance of its leaf (power x bound-based).
void flatImportance(const LightTree& tree, const f32 (&p)[3], const f32 (&n)[3], std::vector<f64>& out) {
    const LightTreeView v = tree.view();
    out.assign(v.emitterCount, 0.0);
    f64 sum = 0.0;
    for (u32 e = 0; e < v.emitterCount; ++e) {
        if (v.emitters[e].leafNode != kLtInvalid) {
            out[e] = refImportance(v.nodes[v.emitters[e].leafNode], p, n);
            sum += out[e];
        }
    }
    for (f64& x : out) {
        x = sum > 0.0 ? x / sum : 0.0;
    }
}

void histogramCase(const LightTree& tree, const ShadingPoint& s, u64 n, const char* what, f64 maxTvFlat) {
    const LightTreeView v = tree.view();
    std::vector<u64> counts(v.emitterCount, 0);
    Rng rng(1234);
    for (u64 i = 0; i < n; ++i) {
        const LightTreeSample smp = tree.sample(s.p, s.n, rng.uniform(), rng.uniform(), rng.uniform());
        if (smp.light < v.emitterCount) {
            ++counts[smp.light];
        }
    }
    std::vector<f64> ref;
    refPmf(tree, s.p, s.n, ref);
    u32 dof = 0;
    const f64 chi = chiSquare(counts, ref, n, dof);
    std::vector<f64> flat;
    flatImportance(tree, s.p, s.n, flat);
    f64 tv = 0.0;
    f64 treeMass = 1.0;
    if (v.directionalCount > 0u) {
        treeMass -= static_cast<f64>(v.directionalCount) / (v.directionalCount + 1.0);
    }
    for (u32 e = 0; e < v.emitterCount; ++e) {
        if (v.emitters[e].leafNode != kLtInvalid) {
            tv += std::fabs(ref[e] / treeMass - flat[e]);
        }
    }
    tv *= 0.5;
    std::printf("histogram %s: %llu samples over %u lights: chi2 vs brute-force tree pmf %.1f (dof %u); "
                "TV(tree pmf, flat importance) = %.4f\n",
                what, static_cast<unsigned long long>(n), v.emitterCount, chi, dof, tv);
    expect(chiOk(chi, dof), "sampling histogram == brute-force pmf (chi-square)");
    expect(tv <= maxTvFlat, "tree pmf close to the flat brute-force importance");
    if (maxTvFlat <= 1e-6) {
        std::vector<u64> c2 = counts;
        u32 dof2 = 0;
        const f64 chi2 = chiSquare(c2, flat, n, dof2);
        std::printf("histogram %s: chi2 vs flat brute-force importance %.1f (dof %u)\n", what, chi2, dof2);
        expect(chiOk(chi2, dof2), "sampling histogram == flat brute-force importance (chi-square)");
    }
}

void testHistogram() {
    // Two lights: the tree's only split is the flat importance (exact).
    {
        const f32 a[3] = {-1.f, 0.f, 0.f}, b[3] = {3.f, 1.f, 0.f}, axis[3] = {-1.f, 0.f, 0.f};
        LightTree tree;
        tree.build(std::vector<LightTreeLight>{makePointLight(a, 1.f), makeSpotLight(b, axis, 0.9f, 0.5f, 4.f)});
        ShadingPoint s{};
        s.p[0] = 0.5f;
        s.p[1] = 0.5f;
        s.n[1] = 1.f;
        histogramCase(tree, s, 400000, "two lights", 1e-6);
    }
    // Mixed scene: chi-square vs the tree pmf, TV bound vs the flat importance.
    {
        LightTree tree;
        tree.build(lt_test::mixedScene(21));
        const std::vector<ShadingPoint> pts = lt_test::shadingPoints(22, 3, 8.f);
        for (const ShadingPoint& s : pts) {
            histogramCase(tree, s, 1000000, "mixed", 0.6);
        }
    }
    // Clustered point lights: the tree tracks the flat importance closely.
    {
        Rng rng(31);
        std::vector<LightTreeLight> lights;
        for (u32 c = 0; c < 10u; ++c) {
            const f32 center[3] = {rng.range(-20.f, 20.f), rng.range(-20.f, 20.f), rng.range(-20.f, 20.f)};
            for (u32 i = 0; i < 50u; ++i) {
                const f32 p[3] = {center[0] + rng.range(-0.5f, 0.5f), center[1] + rng.range(-0.5f, 0.5f),
                                  center[2] + rng.range(-0.5f, 0.5f)};
                lights.push_back(makePointLight(p, rng.range(0.5f, 2.f)));
            }
        }
        LightTree tree;
        tree.build(lights);
        const std::vector<ShadingPoint> pts = lt_test::shadingPoints(32, 3, 25.f);
        for (const ShadingPoint& s : pts) {
            histogramCase(tree, s, 1000000, "clusters", 0.35);
        }
    }
}

// --- refit ------------------------------------------------------------------------------------------

void testRefit() {
    std::vector<LightTreeLight> lights = lt_test::mixedScene(41);
    LightTree tree;
    tree.build(lights);
    const std::vector<LightTreeNode> nodes = tree.nodes();
    const std::vector<LightTreeEmitter> emitters = tree.emitters();
    const u64 v0 = tree.version();
    expect(tree.refit(lights), "refit accepts the same set");
    expect(tree.version() != v0, "refit bumps the version");
    expect(std::memcmp(nodes.data(), tree.nodes().data(), nodes.size() * sizeof(LightTreeNode)) == 0 &&
               std::memcmp(emitters.data(), tree.emitters().data(), emitters.size() * sizeof(LightTreeEmitter)) == 0,
           "refit of unchanged lights == build, bit for bit");
    // Move every light, change powers, turn some off.
    Rng rng(42);
    for (u32 frame = 0; frame < 8u; ++frame) {
        for (LightTreeLight& l : lights) {
            const f32 d[3] = {rng.range(-0.5f, 0.5f), rng.range(-0.5f, 0.5f), rng.range(-0.5f, 0.5f)};
            for (u32 i = 0; i < 3u; ++i) {
                l.position[i] += d[i];
                if (l.kind == kLtKindTriangle) {
                    l.u[i] += d[i];
                    l.v[i] += d[i];
                }
            }
            l.intensity = (rng.next() % 10u) == 0u ? 0.f : l.intensity * rng.range(0.8f, 1.25f);
            if (l.kind == kLtKindSpot || l.kind == kLtKindDirectional) {
                f32 nd[3];
                lt_test::unitVector(rng, nd);
                std::memcpy(l.direction, nd, sizeof(nd));
            }
        }
        expect(tree.refit(lights), "refit after moves");
        expect(checkStructure(tree, "refit"), "refit bounds conservative");
        // Every emitter lies in its leaf's box, which lies in every ancestor's box (checkStructure).
        bool inside = true;
        for (const LightTreeEmitter& e : tree.emitters()) {
            if (e.leafNode == kLtInvalid) {
                continue;
            }
            const LightTreeNode& leaf = tree.nodes()[e.leafNode];
            for (u32 i = 0; i < 3u; ++i) {
                inside = inside && e.p0[i] >= leaf.boundsMin[i] && e.p0[i] <= leaf.boundsMax[i];
            }
        }
        expect(inside, "emitters inside their refitted leaves");
    }
    checkPmfSum(tree, lt_test::shadingPoints(43, 16, 12.f), "after 8 refits");
    // A fresh build of the moved lights vs the refitted tree: same distribution family, both sum to 1.
    LightTree fresh;
    fresh.build(lights);
    checkPmfSum(fresh, lt_test::shadingPoints(43, 16, 12.f), "rebuilt");
    // Add / remove lights: refit refuses, build accepts.
    std::vector<LightTreeLight> more = lights;
    const f32 p[3] = {0.f, 0.f, 0.f};
    more.push_back(makePointLight(p, 1.f));
    expect(!tree.refit(more), "refit refuses a different count");
    std::vector<LightTreeLight> swapped = lights;
    for (LightTreeLight& l : swapped) {
        if (l.kind == kLtKindPoint) {
            l.kind = kLtKindDirectional;
            break;
        }
    }
    expect(!tree.refit(swapped), "refit refuses a light moving between the tree and the directional table");
    const u64 before = tree.version();
    expect(tree.build(more) && tree.version() != before && tree.emitters().size() == more.size(), "rebuild after add");
    expect(checkStructure(tree, "rebuild add"), "rebuild after add: structure");
    more.erase(more.begin() + 10, more.begin() + 60);
    expect(tree.build(more) && tree.emitters().size() == more.size(), "rebuild after remove");
    expect(checkStructure(tree, "rebuild remove"), "rebuild after remove: structure");
    checkPmfSum(tree, lt_test::shadingPoints(44, 8, 12.f), "after remove");
    const u32 builds = tree.stats().builds;
    const u32 refits = tree.stats().refits;
    std::printf("refit: %u builds, %u refits on one tree\n", builds, refits);
}

// --- adapter ----------------------------------------------------------------------------------------

bool buildQuadGrid(geometry::MeshletMesh& out) {
    // Two submeshes: a 6 x 6 quad grid at y = 0 (material 0, facing +y) and a 3 x 3 grid at y = 1 (material 1).
    std::vector<f32> pos;
    std::vector<u32> idx;
    std::vector<geometry::MeshletSourceSubmesh> subs;
    auto grid = [&](u32 cells, f32 y, u32 material) {
        const u32 base = static_cast<u32>(pos.size() / 3u);
        const u32 first = static_cast<u32>(idx.size());
        for (u32 j = 0; j <= cells; ++j) {
            for (u32 i = 0; i <= cells; ++i) {
                pos.insert(pos.end(), {static_cast<f32>(i) * 0.5f, y, static_cast<f32>(j) * 0.5f});
            }
        }
        for (u32 j = 0; j < cells; ++j) {
            for (u32 i = 0; i < cells; ++i) {
                const u32 a = base + j * (cells + 1u) + i, b = a + 1u, c = a + cells + 1u, d = c + 1u;
                idx.insert(idx.end(), {a, c, b, b, c, d});
            }
        }
        subs.push_back({first, static_cast<u32>(idx.size()) - first, material});
    };
    grid(6, 0.f, 0);
    grid(3, 1.f, 1);
    geometry::MeshletSource src{};
    src.positions = pos.data();
    src.vertex_count = static_cast<u32>(pos.size() / 3u);
    src.indices = idx.data();
    src.index_count = static_cast<u32>(idx.size());
    src.submeshes = subs;
    std::string error;
    return geometry::build_meshlets(src, geometry::MeshletBuildOptions{}, out, &error);
}

void testAdapter() {
    gpu_scene::GpuScene scene;
    expect(scene.init(gpu_scene::GpuSceneDesc{}), "GpuScene CPU-only init");
    gpu_scene::GpuLight point{};
    point.type = static_cast<u32>(gpu_scene::GpuLightType::Point);
    point.position[0] = 1.f;
    point.color[0] = 0.5f;
    point.color[1] = 2.f;
    point.intensity = 3.f;
    gpu_scene::GpuLight spot = point;
    spot.type = static_cast<u32>(gpu_scene::GpuLightType::Spot);
    spot.direction[0] = 0.f;
    spot.direction[1] = -1.f;
    spot.direction[2] = 0.f;
    spot.cosInner = 0.9f;
    spot.cosOuter = 0.7f;
    gpu_scene::GpuLight sun = point;
    sun.type = static_cast<u32>(gpu_scene::GpuLightType::Directional);
    ltc::AreaLightDesc area{};
    area.center = {0.f, 3.f, 0.f};
    area.normal = {0.f, -1.f, 0.f};
    area.tangent = {1.f, 0.f, 0.f};
    area.halfWidth = 0.5f;
    area.halfHeight = 0.25f;
    area.intensity = 4.f;
    const gpu_scene::LightHandle hp = scene.addLight(point);
    const gpu_scene::LightHandle hs = scene.addLight(spot);
    const gpu_scene::LightHandle hd = scene.addLight(sun);
    const gpu_scene::LightHandle hr = scene.addLight(ltc::makeRectLight(area));
    area.halfHeight = 0.5f;
    const gpu_scene::LightHandle hk = scene.addLight(ltc::makeDiskLight(area));
    const gpu_scene::LightHandle hx = scene.addLight(point);
    scene.removeLight(hx);
    (void)hp;
    (void)hs;
    (void)hd;
    std::vector<LightTreeLight> lights;
    const u32 appended = appendSceneLights(scene, lights);
    expect(appended == 5u && lights.size() == 5u, "5 live scene lights (the removed slot is skipped)");
    bool kinds = lights.size() == 5u && lights[0].kind == kLtKindPoint && lights[1].kind == kLtKindSpot &&
                 lights[2].kind == kLtKindDirectional && lights[3].kind == kLtKindRect && lights[4].kind == kLtKindDisk;
    expect(kinds, "kinds converted");
    if (kinds) {
        expect(lights[0].intensity == 6.f && lights[0].source == hp.slot && lights[0].position[0] == 1.f,
               "point: max colour x intensity, source slot");
        expect(lights[1].cosInner == 0.9f && lights[1].cosOuter == 0.7f && lights[1].direction[1] == -1.f, "spot cone");
        const LightTreeEmitter rect = makeEmitter(lights[3]);
        const LightTreeEmitter disk = makeEmitter(lights[4]);
        expect(std::fabs(rect.area - 0.5f) < 1e-5f && std::fabs(rect.normal[1] + 1.f) < 1e-5f && rect.source == hr.slot,
               "rectangle: area 4 x hw x hh, normal = the lit side");
        expect(std::fabs(disk.area - 3.14159265f * 0.25f) < 1e-5f && std::fabs(disk.normal[1] + 1.f) < 1e-5f &&
                   disk.source == hk.slot,
               "disk: area pi r1 r2");
    }
    // Emissive triangles: submesh 0 of the grid is emissive (material row 4), submesh 1 is not (row 5).
    geometry::MeshletMesh mesh;
    expect(buildQuadGrid(mesh), "meshlet build");
    const u32 meshIndex = scene.addMeshletMesh(mesh);
    Material emissive{};
    emissive.emissiveColor = {1.f, 2.f, 0.5f};
    emissive.emissiveIntensity = 3.f;
    Material dull{};
    for (u32 m = 0; m < 4u; ++m) {
        scene.setMaterial(m, dull);
    }
    scene.setMaterial(4, emissive);
    scene.setMaterial(5, dull);
    gpu_scene::InstanceDesc inst{};
    inst.mesh = meshIndex;
    inst.material = 4;
    inst.transform.rows[0][3] = 10.f; // translate x by 10
    inst.transform.rows[1][3] = 2.f;
    scene.addInstance(inst);
    gpu_scene::InstanceDesc hidden = inst;
    hidden.flags = 0u; // not visible
    scene.addInstance(hidden);
    const geometry::MeshletMesh* table[1] = {&mesh};
    std::vector<EmissiveTriangleRef> refs;
    const usize before = lights.size();
    const u32 tris = appendSceneEmissiveTriangles(scene, table, 1u, lights, &refs);
    expect(tris == 72u && lights.size() == before + 72u && refs.size() == 72u, "72 emissive triangles (6 x 6 quads)");
    geometry::DecodedVertices decoded;
    geometry::decode_vertices(mesh, decoded);
    bool placed = true;
    for (u32 i = 0; i < tris && placed; ++i) {
        const LightTreeLight& l = lights[before + i];
        const EmissiveTriangleRef& r = refs[l.source];
        // Find the MTRI entry of r.triangle.
        u32 t = 0;
        for (const geometry::MeshletRecord& m : mesh.meshlets) {
            if (r.triangle < t + m.triangle_count) {
                const u32 packed = mesh.meshlet_triangles[m.triangle_offset + (r.triangle - t)];
                const f32* verts[3] = {l.position, l.u, l.v};
                for (u32 c = 0; c < 3u; ++c) {
                    const u32 vtx = mesh.meshlet_vertices[m.vertex_offset + geometry::triangle_index(packed, c)];
                    placed = placed && std::fabs(verts[c][0] - (decoded.positions[3u * vtx] + 10.f)) < 1e-5f &&
                             std::fabs(verts[c][1] - (decoded.positions[3u * vtx + 1u] + 2.f)) < 1e-5f &&
                             std::fabs(verts[c][2] - decoded.positions[3u * vtx + 2u]) < 1e-5f;
                }
                placed = placed && m.submesh == 0u;
                break;
            }
            t += m.triangle_count;
        }
        placed = placed && l.kind == kLtKindTriangle && l.intensity == 6.f && r.instance == 0u;
    }
    expect(placed, "emissive triangles: decoded positions, instance transform, radiance max(colour x intensity)");
    f64 emissiveArea = 0.0;
    for (u32 i = 0; i < tris; ++i) {
        emissiveArea += makeEmitter(lights[before + i]).area;
    }
    expect(std::fabs(emissiveArea - 9.0) < 1e-3, "emissive area == 3 m x 3 m");
    LightTree tree;
    expect(tree.build(lights) && checkStructure(tree, "adapter"), "scene tree builds");
    std::printf("adapter: %u scene lights + %u emissive triangles (area %.3f), %u nodes\n", appended, tris, emissiveArea,
                tree.stats().nodes);
    checkPmfSum(tree, lt_test::shadingPoints(51, 8, 6.f), "adapter scene");
}

// --- zero_alloc --------------------------------------------------------------------------------------

void testZeroAlloc() {
    std::vector<LightTreeLight> lights = lt_test::mixedScene(61, 2);
    std::vector<LightTreeLight> fewer(lights.begin(), lights.begin() + 500);
    const std::vector<ShadingPoint> points = lt_test::shadingPoints(62, 32, 12.f);
    LightTree tree;
    tree.reserve(static_cast<u32>(lights.size()));
    tree.build(lights); // warm-up
    Rng rng(63);
    unsigned long long total = 0;
    f64 sink = 0.0;
    for (u32 frame = 0; frame < 32u; ++frame) {
        for (LightTreeLight& l : lights) {
            l.position[0] += 0.01f;
            if (l.kind == kLtKindTriangle) {
                l.u[0] += 0.01f;
                l.v[0] += 0.01f;
            }
        }
        t_allocations = 0;
        t_count = true;
        const bool ok = (frame % 4u) == 3u ? tree.build((frame % 8u) == 7u ? fewer : lights) : tree.refit(lights);
        for (const ShadingPoint& s : points) {
            for (u32 k = 0; k < 16u; ++k) {
                const LightTreeSample smp = tree.sample(s.p, s.n, rng.uniform(), rng.uniform(), rng.uniform());
                sink += smp.pmf + tree.pmf(s.p, s.n, smp.light);
            }
        }
        t_count = false;
        total += t_allocations;
        if ((frame % 8u) == 7u) {
            tree.build(lights); // back to the full set (refit needs the same count)
        }
        expect(ok, "steady-state build / refit ok");
    }
    std::printf("zero_alloc: 32 frames (refit x 24, rebuild x 8 incl. a smaller set), %u lights, 512 samples + pmf "
                "per frame: %llu operator-new calls (checksum %.3f)\n",
                static_cast<u32>(lights.size()), total, sink);
    expect(total == 0u, "build / refit / sample / pmf make no steady-state heap allocation");
}

} // namespace

int main(int argc, char** argv) {
    const std::string suite = argc > 1 ? argv[1] : "all";
    const bool all = suite == "all";
    if (all || suite == "layout") {
        testLayout();
    }
    if (all || suite == "pmf_sum") {
        testPmfSum();
    }
    if (all || suite == "area_pdf") {
        testAreaPdf();
    }
    if (all || suite == "histogram") {
        testHistogram();
    }
    if (all || suite == "refit") {
        testRefit();
    }
    if (all || suite == "adapter") {
        testAdapter();
    }
    if (all || suite == "zero_alloc") {
        testZeroAlloc();
    }
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("PASS %s\n", suite.c_str());
    return 0;
}
