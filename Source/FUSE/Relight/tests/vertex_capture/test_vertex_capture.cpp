// FUSE Relight RL-1.6 tests: vertex capture (capture/vertex_capture). See CMakeLists.txt.
//
//   fuse_relight_vertex_capture_tests                    the unit tests (rl_vertex_capture_unit)
//   fuse_relight_vertex_capture_tests --emit OUT [SPV..] transforms the given SPIR-V files (glslang
//                                                        output of the test shaders) and the built-in
//                                                        variants into OUT/<name>.spv for spirv-val
//   fuse_relight_vertex_capture_tests --analyze FILE     one line per shader: "<swvp 0|1> <hex tokens..>";
//                                                        prints "<valid> <F> <I> <B>" per line (the
//                                                        Python twin rl_vs_analysis.py checks it)
#include <fuse/relight/capture/vertex_capture/back_transform.hpp>
#include <fuse/relight/capture/vertex_capture/capture_layout.hpp>
#include <fuse/relight/capture/vertex_capture/capture_ring.hpp>
#include <fuse/relight/capture/vertex_capture/draw_capture.hpp>
#include <fuse/relight/capture/vertex_capture/dxvk_hook.hpp>
#include <fuse/relight/capture/vertex_capture/spirv_vertex_capture.hpp>
#include <fuse/relight/capture/vertex_capture/vs_hash.hpp>
#include <fuse/relight/hash/geometry_hash.hpp>

#include <spirv/unified1/spirv.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>

// The hand-over points the FUSE-DXVK patches call (capture/vertex_capture/src/dxvk_hook.cpp).
namespace dxvk {
bool fuseRelightVertexCaptureBinding(const char* shaderName, std::uint32_t* set, std::uint32_t* binding,
                                     std::uint32_t* slot);
} // namespace dxvk

namespace {

using namespace fuse::relight;
using namespace fuse::relight::capture::vertex_capture;

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const std::string& what, int line) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL (line %d): %s\n", line, what.c_str());
    }
}
#define VC_CHECK(cond) check((cond), #cond, __LINE__)
#define VC_CHECK_MSG(cond, msg) check((cond), (msg), __LINE__)

// ---- D3D9 token builder -----------------------------------------------------------------------------

namespace dxso {
constexpr std::uint32_t kTemp = 0, kInput = 1, kConst = 2, kAddr = 3, kRastOut = 4, kOutput = 6, kConstInt = 7,
                        kSampler = 10, kConst2 = 11, kConstBool = 14, kRegLoop = 15, kLabel = 18, kPredicate = 19;

std::uint32_t reg(std::uint32_t type, std::uint32_t num) {
    return 0x80000000u | ((type & 7u) << 28) | ((type & 0x18u) << 8) | (num & 0x7FFu);
}
std::uint32_t dst(std::uint32_t type, std::uint32_t num, std::uint32_t mask = 0xF) { return reg(type, num) | (mask << 16); }
std::uint32_t src(std::uint32_t type, std::uint32_t num, bool relative = false) {
    return reg(type, num) | (0xE4u << 16) | (relative ? (1u << 13) : 0u);
}

struct Program {
    std::vector<std::uint32_t> t;
    std::uint32_t major = 2;
    Program(std::uint32_t maj, std::uint32_t minor) : major(maj) { t.push_back(0xFFFE0000u | (maj << 8) | minor); }
    Program& op(std::uint32_t opcode, std::vector<std::uint32_t> params, bool predicated = false) {
        std::uint32_t token = opcode | (predicated ? (1u << 28) : 0u);
        if (major >= 2) {
            token |= std::uint32_t(params.size()) << 24;
        }
        t.push_back(token);
        t.insert(t.end(), params.begin(), params.end());
        return *this;
    }
    Program& def(std::uint32_t num, float a, float b, float c, float d) {
        std::uint32_t bits[4];
        const float v[4] = {a, b, c, d};
        std::memcpy(bits, v, sizeof bits);
        return op(81, {dst(kConst, num), bits[0], bits[1], bits[2], bits[3]});
    }
    Program& defi(std::uint32_t num, std::int32_t a) {
        return op(48, {dst(kConstInt, num), std::uint32_t(a), 0, 1, 0});
    }
    Program& defb(std::uint32_t num, bool v) { return op(47, {dst(kConstBool, num, 0), v ? 1u : 0u}); }
    Program& comment(std::vector<std::uint32_t> body) {
        t.push_back(0xFFFEu | (std::uint32_t(body.size()) << 16));
        t.insert(t.end(), body.begin(), body.end());
        return *this;
    }
    std::vector<std::uint32_t> end() {
        std::vector<std::uint32_t> out = t;
        out.push_back(0x0000FFFFu);
        return out;
    }
};

// Opcodes.
constexpr std::uint32_t kMov = 1, kAdd = 2, kMad = 4, kDp3 = 8, kDp4 = 9, kMax = 11, kM4x4 = 20, kM3x2 = 24,
                        kCallNz = 26, kLoop = 27, kEndLoop = 29, kLabelOp = 30, kDcl = 31, kSinCos = 37, kRep = 38,
                        kEndRep = 39, kIf = 40, kEndIf = 43, kMova = 46, kTexLdl = 95;
} // namespace dxso

ShaderConstantRanges ranges(std::uint32_t f, std::uint32_t i, std::uint32_t b) { return {f, i, b, true}; }

// The RL-0.4 vs_sm2 / vs_sm3 vertex shaders exactly as the apps create them (Tests/relight/apps/scenes,
// the bytecode the apps' sidecars record).
std::vector<std::uint32_t> vsSm2() {
    return {
        0xfffe0200, 0x0200001f, 0x80000000, 0x900f0000, 0x0200001f, 0x80000003, 0x900f0001, 0x0200001f,
        0x80000005, 0x900f0002, 0x0200001f, 0x8000000a, 0x900f0003, 0x05000051, 0xa00f0007, 0x00000000,
        0x00000000, 0x00000000, 0x00000000, 0x03000014, 0xc00f0000, 0x90e40000, 0xa0e40000, 0x03000008,
        0x80010000, 0x90e40001, 0xa0e40004, 0x0300000b, 0x80010000, 0x80000000, 0xa0000007, 0x04000004,
        0xd00f0000, 0x90e40003, 0x80000000, 0xa0e40005, 0x02000001, 0xe0030000, 0x90e40002, 0x0000ffff,
    };
}

std::vector<std::uint32_t> vsSm3() {
    return {
        0xfffe0300, 0x0200001f, 0x80000000, 0x900f0000, 0x0200001f, 0x8000000a, 0x900f0001, 0x0200001f,
        0x80000005, 0x900f0002, 0x0200001f, 0x80000000, 0xe00f0000, 0x0200001f, 0x80000005, 0xe0030001,
        0x0200001f, 0x8000000a, 0xe00f0002, 0x05000051, 0xa00f000a, 0x00000000, 0x00000000, 0x00000000,
        0x00000000, 0x05000030, 0xf00f0000, 0x00000001, 0x00000000, 0x00000001, 0x00000000, 0x03000014,
        0xe00f0000, 0x90e40000, 0xa0e40000, 0x02000001, 0x800f0001, 0xa0e4000a, 0x0200001b, 0xf0e40800,
        0xf0e40001, 0x03000002, 0x800f0001, 0x80e40001, 0xa0e40008, 0x0000001d, 0x0200001b, 0xf0e40800,
        0xf0e40000, 0x03000002, 0x800f0001, 0x80e40001, 0xa0e4000a, 0x0000001d, 0x01000028, 0xe0e40800,
        0x03000002, 0x800f0001, 0x80e40001, 0xa0e40009, 0x0000002b, 0x03000002, 0xe00f0002, 0x90e40001,
        0x80e40001, 0x02000001, 0xe0030001, 0x90e40002, 0x0000ffff,
    };
}

void testAnalysis() {
    using namespace dxso;
    const ConstantLayout hw = ConstantLayout::forVertexShaders(false);
    const ConstantLayout sw = ConstantLayout::forVertexShaders(true);

    VC_CHECK(analyzeVertexShader(vsSm2(), hw) == ranges(6, 0, 0));
    VC_CHECK(analyzeVertexShader(vsSm3(), hw) == ranges(10, 2, 1));

    { // use before def counts, def before use does not
        Program p(2, 0);
        p.op(kMov, {dst(kTemp, 0), src(kConst, 9)});
        p.def(9, 1, 2, 3, 4);
        p.def(20, 1, 2, 3, 4);
        p.op(kMov, {dst(kTemp, 0), src(kConst, 20)});
        VC_CHECK(analyzeVertexShader(p.end(), hw) == ranges(10, 0, 0));
    }
    { // relative addressing: the whole layout (vs_2_0 carries a relative-address token)
        Program p(2, 0);
        p.op(kMova, {dst(kAddr, 0, 1), src(kConst, 1)});
        p.op(kMov, {dst(kTemp, 0), src(kConst, 3, true), src(kAddr, 0)});
        VC_CHECK(analyzeVertexShader(p.end(), hw) == ranges(256, 0, 0));
        VC_CHECK(analyzeVertexShader(p.end(), sw) == ranges(8192, 0, 0));
    }
    { // relative addressing of a defined constant still counts
        Program p(2, 0);
        p.def(3, 1, 1, 1, 1);
        p.op(kMov, {dst(kTemp, 0), src(kConst, 3, true), src(kAddr, 0)});
        VC_CHECK(analyzeVertexShader(p.end(), hw).maxConstIndexF == 256);
    }
    { // sincos (vs_2_0: three sources) loads src0 only
        Program p(2, 0);
        p.op(kSinCos, {dst(kTemp, 0, 3), src(kConst, 2), src(kConst, 10), src(kConst, 11)});
        VC_CHECK(analyzeVertexShader(p.end(), hw) == ranges(3, 0, 0));
    }
    { // matrix rows
        Program p(2, 0);
        p.op(kM3x2, {dst(kTemp, 0, 3), src(kInput, 0), src(kConst, 40)});
        VC_CHECK(analyzeVertexShader(p.end(), hw) == ranges(42, 0, 0));
    }
    { // integer and bool registers; defi hides; callnz / label read nothing
        Program p(2, 0);
        p.defi(5, 3);
        p.op(kLoop, {src(kRegLoop, 0), src(kConstInt, 3)});
        p.op(kEndLoop, {});
        p.op(kRep, {src(kConstInt, 5)});
        p.op(kEndRep, {});
        p.op(kIf, {src(kConstBool, 2)});
        p.op(kEndIf, {});
        p.op(kCallNz, {src(kLabel, 0), src(kConstBool, 7)});
        p.op(kLabelOp, {src(kLabel, 0)});
        VC_CHECK(analyzeVertexShader(p.end(), hw) == ranges(0, 4, 3));
    }
    { // defb hides a bool
        Program p(2, 0);
        p.defb(4, true);
        p.op(kIf, {src(kConstBool, 4)});
        p.op(kEndIf, {});
        VC_CHECK(analyzeVertexShader(p.end(), hw) == ranges(0, 0, 0));
    }
    { // clamps to the layout; c2048+ (CONST2) is float 2048 + n
        Program p(3, 0);
        p.op(kMov, {dst(kTemp, 0), src(kConst, 300)});
        VC_CHECK(analyzeVertexShader(p.end(), hw) == ranges(256, 0, 0));
        Program q(3, 0);
        q.op(kMov, {dst(kTemp, 0), src(kConst2, 5)});
        VC_CHECK(analyzeVertexShader(q.end(), sw) == ranges(2054, 0, 0));
        Program r(3, 0);
        r.op(kLoop, {src(kRegLoop, 0), src(kConstInt, 40)});
        VC_CHECK(analyzeVertexShader(r.end(), hw).maxConstIndexI == 16);
        VC_CHECK(analyzeVertexShader(r.end(), sw).maxConstIndexI == 41);
    }
    { // predicated instruction: the predicate token sits between dst and the sources
        Program p(3, 0);
        p.op(kAdd, {dst(kTemp, 0), src(kPredicate, 0), src(kTemp, 1), src(kConst, 12)}, true);
        VC_CHECK(analyzeVertexShader(p.end(), hw) == ranges(13, 0, 0));
    }
    { // vs_3_0 indexed output: dst relative token
        Program p(3, 0);
        p.op(kMov, {dst(kOutput, 1) | (1u << 13), src(kRegLoop, 0), src(kConst, 5)});
        VC_CHECK(analyzeVertexShader(p.end(), hw) == ranges(6, 0, 0));
    }
    { // texldl reads its coordinate
        Program p(3, 0);
        p.op(kTexLdl, {dst(kTemp, 0), src(kConst, 2), src(kSampler, 0)});
        VC_CHECK(analyzeVertexShader(p.end(), hw) == ranges(3, 0, 0));
    }
    { // comments are skipped even when their body looks like instructions
        Program p(2, 0);
        p.comment({kMov | (2u << 24), dst(kTemp, 0), src(kConst, 99)});
        p.op(kMov, {dst(kTemp, 0), src(kConst, 1)});
        VC_CHECK(analyzeVertexShader(p.end(), hw) == ranges(2, 0, 0));
    }
    { // vs_1_1: no length field; def literals with the sign bit set
        Program p(1, 1);
        p.def(1, -1.0f, -2.0f, 3.0f, -0.0f);
        p.op(kDp4, {dst(kRastOut, 0, 1), src(kInput, 0), src(kConst, 20)});
        p.op(kMov, {dst(kTemp, 0), src(kConst, 1)});
        p.op(kMov, {dst(kTemp, 1), src(kConst, 1, true)}); // vs_1_1 relative: no extra token
        p.op(kAdd, {dst(kTemp, 2), src(kTemp, 0), src(kConst, 30)});
        VC_CHECK(analyzeVertexShader(p.end(), hw) == ranges(256, 0, 0));
        Program q(1, 1);
        q.def(1, -1.0f, -2.0f, 3.0f, -0.0f);
        q.op(kDp4, {dst(kRastOut, 0, 1), src(kInput, 0), src(kConst, 20)});
        q.op(kMov, {dst(kTemp, 0), src(kConst, 1)});
        VC_CHECK(analyzeVertexShader(q.end(), hw) == ranges(21, 0, 0));
    }
    { // invalid: pixel shader, no END, empty
        std::vector<std::uint32_t> ps = {0xFFFF0200u, 0x0000FFFFu};
        VC_CHECK(!analyzeVertexShader(ps, hw).valid);
        Program p(2, 0);
        p.op(kMov, {dst(kTemp, 0), src(kConst, 1)});
        VC_CHECK(!analyzeVertexShader(p.t, hw).valid);
        VC_CHECK(!analyzeVertexShader({}, hw).valid);
    }
}

// ---- hash ---------------------------------------------------------------------------------------------

std::string hex64(std::uint64_t v) {
    char b[20];
    std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(v));
    return b;
}

void testHash() {
    const std::vector<std::uint32_t> tokens = vsSm3();
    std::vector<float> f(256 * 4, 0.0f);
    std::vector<std::int32_t> i(16 * 4, 0);
    std::vector<std::uint32_t> b(1, 0);
    // vs_sm3's state (Tests/relight/apps/scenes/vs_sm3.cpp): c0..c3 = transpose(W V P), c8, c9, i1, b0.
    const float wvp[16] = {1.29903817f, 0, 0, 0, 0, 1.7320509f, 0, 0, 0, 0, 1.00502515f, 3.01507568f, 0, 0, 1, 3.5f};
    std::memcpy(f.data(), wvp, sizeof wvp);
    f[8 * 4 + 0] = 0.25f;
    f[9 * 4 + 1] = 0.25f;
    i[1 * 4 + 0] = 2;
    i[1 * 4 + 2] = 1;

    tap::DrawState s;
    s.vertexShader.id = 1;
    s.vertexShader.tokens = tokens.data();
    s.vertexShader.byteSize = std::uint32_t(tokens.size() * 4);
    s.vsConstF = reinterpret_cast<const float(*)[4]>(f.data());
    s.vsConstFCount = 256;
    s.vsConstI = reinterpret_cast<const std::int32_t(*)[4]>(i.data());
    s.vsConstICount = 16;
    s.vsConstB = b.data();
    s.vsConstBCount = 16;

    const std::optional<hash::Hash64> h = vertexShaderHash(s);
    VC_CHECK(h.has_value());
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(tokens.data());
    const hash::Hash64 expected =
        hash::hashVertexShader(std::span(bytes, tokens.size() * 4), f.data(), 10, i.data(), 2, b.data(), 1);
    VC_CHECK(h && *h == expected);
    // Remix-compatible KAT (Tools/FUSE/Relight/remix_hash_ref.py vertex_shader on the same bytes).
    VC_CHECK_MSG(h && hex64(*h) == "a030a7641e6f0b1c", "vs_sm3 KAT: got " + (h ? hex64(*h) : std::string("none")));
    // Constants outside the ranges do not change the hash; inside they do.
    f[20 * 4] = 7.0f;
    VC_CHECK(vertexShaderHash(s) == h);
    f[9 * 4 + 3] = 1.0f;
    VC_CHECK(vertexShaderHash(s) != h);
    // The bool quirk: 1 bool register hashes 4 / 32 = 0 bytes, so b0 does not matter.
    f[9 * 4 + 3] = 0.0f;
    b[0] = 1;
    VC_CHECK(vertexShaderHash(s) == h);

    // Without the bytecode or the constants the ranges need: no component.
    tap::DrawState t = s;
    t.vsConstF = nullptr;
    VC_CHECK(!vertexShaderHash(t).has_value());
    t = s;
    t.vertexShader.tokens = nullptr;
    VC_CHECK(!vertexShaderHash(t).has_value());
    t = s;
    t.vertexShader.id = tap::kNoResource;
    VC_CHECK(!vertexShaderHash(t).has_value());
    VC_CHECK(vertexShaderHashHook()(s) == vertexShaderHash(s));

    // vs_sm2 KAT (c0..c3, c4, c5 set as the app sets them).
    const std::vector<std::uint32_t> t2 = vsSm2();
    std::vector<float> f2(256 * 4, 0.0f);
    std::memcpy(f2.data(), wvp, sizeof wvp);
    f2[4 * 4 + 2] = -1.0f;
    tap::DrawState s2 = s;
    s2.vertexShader.tokens = t2.data();
    s2.vertexShader.byteSize = std::uint32_t(t2.size() * 4);
    s2.vsConstF = reinterpret_cast<const float(*)[4]>(f2.data());
    const std::optional<hash::Hash64> h2 = vertexShaderHash(s2);
    VC_CHECK_MSG(h2 && hex64(*h2) == "e806ad604ee2d7b9", "vs_sm2 KAT: got " + (h2 ? hex64(*h2) : std::string("none")));
}

// ---- back-transform -----------------------------------------------------------------------------------

using M = Matrix4;

M mul(const M& a, const M& b) { // D3D row-vector product a * b
    M r{};
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            double s = 0;
            for (int k = 0; k < 4; ++k) {
                s += double(a[i * 4 + k]) * double(b[k * 4 + j]);
            }
            r[i * 4 + j] = float(s);
        }
    }
    return r;
}

M perspectiveLH(float fovY, float aspect, float zn, float zf) {
    const float ys = 1.0f / std::tan(fovY * 0.5f), xs = ys / aspect;
    return {xs, 0, 0, 0, 0, ys, 0, 0, 0, 0, zf / (zf - zn), 1, 0, 0, -zn * zf / (zf - zn), 0};
}

M rotation(float yaw, float pitch, float roll) {
    const float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch), cr = std::cos(roll),
                sr = std::sin(roll);
    const M ry{cy, 0, -sy, 0, 0, 1, 0, 0, sy, 0, cy, 0, 0, 0, 0, 1};
    const M rx{1, 0, 0, 0, 0, cp, sp, 0, 0, -sp, cp, 0, 0, 0, 0, 1};
    const M rz{cr, sr, 0, 0, -sr, cr, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    return mul(mul(rz, rx), ry);
}

M translation(float x, float y, float z) { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, y, z, 1}; }
M scale(float x, float y, float z) { return {x, 0, 0, 0, 0, y, 0, 0, 0, 0, z, 0, 0, 0, 0, 1}; }

void testBackTransform() {
    // packColor (Remix's COLOR0 packing).
    const float c1[4] = {1.0f, 0.5f, 0.25f, 1.0f};
    VC_CHECK(packColor(c1) == 0xFFFF8040u);
    const float c2[4] = {-1.0f, 2.0f, 0.0f, 0.0f};
    VC_CHECK(packColor(c2) == 0x0000FF00u);

    // inverse / inverseAffine
    const M a = mul(mul(scale(2, 3, 0.5f), rotation(0.3f, -0.7f, 1.1f)), translation(4, -5, 6));
    const M ia = inverseAffine(a), ig = inverse(a);
    for (std::size_t k = 0; k < 16; ++k) {
        VC_CHECK(std::fabs(ia[k] - ig[k]) < 1e-5f);
    }
    const M id = mul(a, ig);
    for (std::size_t k = 0; k < 16; ++k) {
        VC_CHECK(std::fabs(id[k] - identityMatrix()[k]) < 1e-5f);
    }
    const M p = perspectiveLH(1.0471976f, 4.0f / 3.0f, 0.5f, 100.0f);
    const M ip = mul(p, inverse(p));
    for (std::size_t k = 0; k < 16; ++k) {
        VC_CHECK(std::fabs(ip[k] - identityMatrix()[k]) < 1e-5f);
    }
    // A singular 3x3 falls back to the general inverse (which is then infinite / NaN, as upstream).
    const M singular = scale(0, 1, 1);
    const M is = inverseAffine(singular);
    VC_CHECK(!std::isfinite(is[0]) || std::isnan(is[0]));

    // processRenderState
    const M w = translation(1, 2, 3);
    const DrawTransforms t0 = drawTransforms(w.data(), identityMatrix().data(), p.data(), false);
    VC_CHECK(t0.objectToWorld == identityMatrix());
    M w0 = w;
    w0[15] = 0.0f;
    VC_CHECK(drawTransforms(w0.data(), w0.data(), p.data(), true).objectToWorld[15] == 1.0f);
    VC_CHECK(drawTransforms(w0.data(), w0.data(), p.data(), true).worldToView[15] == 1.0f);
    VC_CHECK(drawTransforms(w0.data(), w0.data(), w0.data(), true).viewToProjection[15] == 0.0f);

    // clip -> object, 1000 random draws: within 1e-4 of the object-space position.
    std::mt19937 rng(1616);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    float worst = 0.0f;
    for (int n = 0; n < 1000; ++n) {
        const M world = mul(mul(scale(0.5f + std::fabs(u(rng)), 0.5f + std::fabs(u(rng)), 0.5f + std::fabs(u(rng))),
                                rotation(u(rng) * 3, u(rng) * 1.5f, u(rng) * 3)),
                            translation(u(rng) * 5, u(rng) * 5, u(rng) * 5));
        const M camera = mul(rotation(u(rng) * 3, u(rng) * 1.4f, 0), translation(u(rng) * 5, u(rng) * 5, -20 + u(rng) * 3));
        const M view = inverse(camera);
        const M proj = perspectiveLH(0.6f + std::fabs(u(rng)), 1.0f + std::fabs(u(rng)), 0.1f + std::fabs(u(rng)), 500.0f);
        const BackTransform bt = backTransformFor(drawTransforms(world.data(), view.data(), proj.data(), true));
        const M wvp = mul(mul(world, view), proj);
        for (int v = 0; v < 8; ++v) {
            const float obj[3] = {u(rng) * 2, u(rng) * 2, u(rng) * 2};
            float clip[4];
            for (int c = 0; c < 4; ++c) {
                clip[c] = float(double(obj[0]) * wvp[0 * 4 + c] + double(obj[1]) * wvp[1 * 4 + c] +
                                double(obj[2]) * wvp[2 * 4 + c] + double(wvp[3 * 4 + c]));
            }
            const std::array<float, 3> back = clipToObject(bt, clip);
            for (int c = 0; c < 3; ++c) {
                worst = std::max(worst, std::fabs(back[std::size_t(c)] - obj[c]));
            }
        }
    }
    VC_CHECK_MSG(worst < 1e-4f, "clip -> object worst error " + std::to_string(worst));

    // normalTransform: the upper 3x3 applied as M * n (Remix reads the row-major UBO column by column).
    const M rz90 = rotation(0, 0, 1.5707963f);
    const BackTransform bn = backTransformFor(drawTransforms(rz90.data(), identityMatrix().data(), p.data(), true));
    const float n[3] = {1, 0, 0};
    const std::array<float, 3> tn = transformNormal(bn, n);
    VC_CHECK(std::fabs(tn[0]) < 1e-6f && std::fabs(tn[1] + 1.0f) < 1e-6f && std::fabs(tn[2]) < 1e-6f);

    // backTransform of a slot keeps Remix's defaults for members the shader did not write.
    RawCapturedVertex raw;
    raw.clip[3] = 1.0f;
    raw.fields = fields::kWritten;
    raw.texcoord0[0] = 9;
    raw.color0 = 0x12345678u;
    const CapturedVertex cv = backTransform(bn, raw);
    VC_CHECK(cv.texcoord0[0] == 0.0f && cv.color0 == 0xFFFFFFFFu && cv.normal0[0] == 0.0f);
    raw.fields |= fields::kTexcoord | fields::kColor;
    const CapturedVertex cv2 = backTransform(bn, raw);
    VC_CHECK(cv2.texcoord0[0] == 9.0f && cv2.color0 == 0x12345678u);
}

// ---- capture ring ---------------------------------------------------------------------------------------

void testRing() {
    tap::DrawCall d;
    d.call = tap::DrawCallType::DrawIndexedPrimitive;
    d.baseVertex = -4;
    d.minIndex = 10;
    d.numVertices = 6;
    RegionRequest r = regionForDraw(d);
    VC_CHECK(r.baseVertex == 6 && r.vertexOffset == -4 && r.vertexCount == 6);
    d.call = tap::DrawCallType::DrawIndexedPrimitiveUP;
    r = regionForDraw(d);
    VC_CHECK(r.baseVertex == 10 && r.vertexOffset == 0 && r.vertexCount == 6);
    d.call = tap::DrawCallType::DrawPrimitive;
    d.startVertex = 7;
    d.vertexCount = 3;
    r = regionForDraw(d);
    VC_CHECK(r.baseVertex == 7 && r.vertexOffset == 0 && r.vertexCount == 3);
    d.call = tap::DrawCallType::DrawPrimitiveUP;
    r = regionForDraw(d);
    VC_CHECK(r.baseVertex == 0 && r.vertexCount == 3);
    d.vertexCount = 5000000;
    VC_CHECK(regionForDraw(d, 100).vertexCount == 100);

    CaptureRing ring(4096);
    std::vector<std::uint8_t> mem(4096, 0xCD);
    CaptureRing::writeEmptyRegion(mem.data());
    RegionHeader empty;
    std::memcpy(&empty, mem.data(), sizeof empty);
    VC_CHECK(empty.vertexCount == 0);

    const auto a = ring.allocate(5, RegionRequest{100, 0, 10});
    VC_CHECK(a && a->offset == CaptureRing::kEmptyRegionBytes && a->size == regionBytes(10));
    CaptureRing::initRegion(mem.data(), *a);
    RegionHeader h;
    std::memcpy(&h, mem.data() + a->offset, sizeof h);
    VC_CHECK(h.baseVertex == 100 && h.vertexCount == 10 && h.drawLow == 5 && h.drawHigh == 0);
    VC_CHECK(mem[a->offset + kRegionHeaderSize + 7] == 0);
    const auto b = ring.allocate(6, RegionRequest{0, 0, 3});
    VC_CHECK(b && b->offset % CaptureRing::kAlignment == 0 && b->offset >= a->offset + a->size);
    VC_CHECK(!ring.allocate(7, RegionRequest{0, 0, 0}).has_value()); // nothing to capture: not dropped
    VC_CHECK(ring.dropped() == 0);
    const auto c = ring.allocate(8, RegionRequest{0, 0, 1000}); // does not fit
    VC_CHECK(!c.has_value() && ring.dropped() == 1 && ring.wantedCapacity() > ring.capacity());
    const auto d2 = ring.allocate(9, RegionRequest{0, 0, 2}); // a small one still fits after a drop
    VC_CHECK(d2.has_value());
    const std::vector<tap::VertexCaptureDraw> back = ring.readBack(mem.data());
    VC_CHECK(back.size() == 3 && back[0].draw == 5 && back[1].draw == 6 && back[2].draw == 9);
    VC_CHECK(back[0].data == mem.data() + a->offset + kRegionHeaderSize && back[0].vertexCount == 10);
    ring.reset(ring.wantedCapacity());
    VC_CHECK(!ring.pending() && ring.dropped() == 0 && ring.capacity() >= regionBytes(1000));
    VC_CHECK(ring.allocate(10, RegionRequest{0, 0, 1000}).has_value());
}

// ---- draw capture -------------------------------------------------------------------------------------------

void testDrawCapture() {
    const M world = translation(1, 0, 0);
    const M view = translation(0, 0, 3.5f);
    const M proj = perspectiveLH(1.0471976f, 4.0f / 3.0f, 0.5f, 100.0f);
    float transforms[tap::kTransformCount][16] = {};
    std::memcpy(transforms[tap::kTransformWorld0], world.data(), 64);
    std::memcpy(transforms[tap::kTransformView], view.data(), 64);
    std::memcpy(transforms[tap::kTransformProjection], proj.data(), 64);
    tap::DrawState s;
    s.transforms = transforms;
    DrawVertexCapture c = beginDrawCapture(s, VertexCaptureOptions{});

    const M wvp = mul(mul(world, view), proj);
    std::vector<RawCapturedVertex> slots(3);
    const float objs[2][3] = {{0.5f, -0.25f, 0.0f}, {-1.0f, 1.0f, 0.5f}};
    for (int v = 0; v < 2; ++v) {
        for (int k = 0; k < 4; ++k) {
            slots[std::size_t(v)].clip[k] = objs[v][0] * wvp[0 * 4 + k] + objs[v][1] * wvp[1 * 4 + k] +
                                            objs[v][2] * wvp[2 * 4 + k] + wvp[3 * 4 + k];
        }
        slots[std::size_t(v)].fields = fields::kWritten | fields::kColor;
        slots[std::size_t(v)].color0 = 0xFF00FF00u;
    }
    tap::VertexCaptureDraw region;
    region.draw = 3;
    region.baseVertex = 4;
    region.vertexOffset = 1;
    region.vertexCount = 3;
    region.data = slots.data();
    completeDrawCapture(c, region);
    VC_CHECK(c.captured && c.written == 2 && c.fields == (fields::kWritten | fields::kColor));
    VC_CHECK(c.baseVertex == 4 && c.vertexOffset == 1 && c.raw.size() == 3 && c.vertices.size() == 3);
    for (int v = 0; v < 2; ++v) {
        for (int k = 0; k < 3; ++k) {
            VC_CHECK(std::fabs(c.vertices[std::size_t(v)].position[k] - objs[v][k]) < 1e-4f);
        }
        VC_CHECK(c.vertices[std::size_t(v)].color0 == 0xFF00FF00u);
    }
    // Without rtx.useWorldMatricesForShaders the world matrix is identity: the result is world space.
    VertexCaptureOptions noWorld;
    noWorld.useWorldMatricesForShaders = false;
    DrawVertexCapture w = beginDrawCapture(s, noWorld);
    completeDrawCapture(w, region);
    VC_CHECK(std::fabs(w.vertices[0].position[0] - (objs[0][0] + 1.0f)) < 1e-4f);
}

// ---- SPIR-V -----------------------------------------------------------------------------------------------

/// A minimal SPIR-V module builder for the pass's edge cases.
class Spv {
public:
    std::vector<std::uint32_t> words;
    std::uint32_t bound = 1;
    std::uint32_t version = 0x00010600;

    std::uint32_t id() { return bound++; }
    void op(spv::Op o, std::vector<std::uint32_t> operands) {
        words.push_back((std::uint32_t(operands.size() + 1) << 16) | std::uint32_t(o));
        words.insert(words.end(), operands.begin(), operands.end());
    }
    static std::vector<std::uint32_t> str(const std::string& s) {
        std::vector<std::uint32_t> w(s.size() / 4 + 1, 0);
        std::memcpy(w.data(), s.data(), s.size());
        return w;
    }
    std::vector<std::uint32_t> finish() const {
        std::vector<std::uint32_t> out = {spv::MagicNumber, version, 0, bound, 0};
        out.insert(out.end(), words.begin(), words.end());
        return out;
    }
};

struct ModuleOptions {
    bool perVertexBlock = false;   // gl_Position as a member of a block
    int returns = 1;               // OpReturn sites (2: an early return behind a branch)
    bool vertexIndex = false;      // the shader declares gl_VertexIndex (as uint)
    bool color = true;
    bool writeColor = true;        // false: the COLOR0 output is declared but never stored (SM1/2 style)
    bool splitTexcoord = false;    // TEXCOORD0 as two float outputs with Component 0 / 1
    bool inputNormal = true;       // an input named "v1_normal0"
    bool position = true;
    bool returnValue = false;      // an OpReturnValue in the entry point (rejected)
    std::uint32_t version = 0x00010600;
};

std::vector<std::uint32_t> buildModule(const ModuleOptions& o) {
    Spv m;
    m.version = o.version;
    const std::uint32_t glsl = m.id(), main = m.id();
    const std::uint32_t tVoid = m.id(), tFn = m.id(), tF = m.id(), tV4 = m.id(), tV2 = m.id(), tU = m.id(), tBool = m.id();
    const std::uint32_t tPtrOutV4 = m.id(), tPtrInV4 = m.id(), tPtrOutF = m.id(), tPtrInU = m.id();
    const std::uint32_t pos = m.id(), tex = m.id(), tex1 = m.id(), col = m.id(), nrmIn = m.id(), inPos = m.id(), vid = m.id();
    const std::uint32_t tBlock = m.id(), tPtrOutBlock = m.id(), c0 = m.id(), cU0 = m.id(), cTrue = m.id(), cF1 = m.id(),
                        cF0 = m.id(), cZero4 = m.id();
    const std::uint32_t lEntry = m.id(), lA = m.id(), lB = m.id(), lMerge = m.id(), ld = m.id(), ld2 = m.id(), ptrPos = m.id();

    m.op(spv::OpCapability, {spv::CapabilityShader});
    m.op(spv::OpExtInstImport, [&] { auto w = Spv::str("GLSL.std.450"); w.insert(w.begin(), glsl); return w; }());
    m.op(spv::OpMemoryModel, {spv::AddressingModelLogical, spv::MemoryModelGLSL450});
    std::vector<std::uint32_t> ep = {spv::ExecutionModelVertex, main};
    for (std::uint32_t w : Spv::str("main")) {
        ep.push_back(w);
    }
    if (o.position) {
        ep.push_back(pos);
    }
    for (std::uint32_t v : {tex, col, nrmIn, inPos}) {
        ep.push_back(v);
    }
    if (o.splitTexcoord) {
        ep.push_back(tex1);
    }
    if (o.vertexIndex) {
        ep.push_back(vid);
    }
    m.op(spv::OpEntryPoint, ep);
    auto name = [&](std::uint32_t id, const std::string& n) {
        auto w = Spv::str(n);
        w.insert(w.begin(), id);
        m.op(spv::OpName, w);
    };
    name(main, "main");
    name(nrmIn, o.inputNormal ? "v1_normal0" : "v1_color");
    if (o.position) {
        if (o.perVertexBlock) {
            m.op(spv::OpMemberDecorate, {tBlock, 0, spv::DecorationBuiltIn, spv::BuiltInPosition});
            m.op(spv::OpMemberDecorate, {tBlock, 1, spv::DecorationBuiltIn, spv::BuiltInPointSize});
            m.op(spv::OpDecorate, {tBlock, spv::DecorationBlock});
        } else {
            m.op(spv::OpDecorate, {pos, spv::DecorationBuiltIn, spv::BuiltInPosition});
        }
    }
    m.op(spv::OpDecorate, {tex, spv::DecorationLocation, 1});
    if (o.splitTexcoord) {
        m.op(spv::OpDecorate, {tex1, spv::DecorationLocation, 1});
        m.op(spv::OpDecorate, {tex1, spv::DecorationComponent, 1});
    }
    m.op(spv::OpDecorate, {col, spv::DecorationLocation, o.color ? 9u : 7u});
    m.op(spv::OpDecorate, {nrmIn, spv::DecorationLocation, 3});
    m.op(spv::OpDecorate, {inPos, spv::DecorationLocation, 0});
    if (o.vertexIndex) {
        m.op(spv::OpDecorate, {vid, spv::DecorationBuiltIn, spv::BuiltInVertexIndex});
    }
    m.op(spv::OpTypeVoid, {tVoid});
    m.op(spv::OpTypeFunction, {tFn, tVoid});
    m.op(spv::OpTypeFloat, {tF, 32});
    m.op(spv::OpTypeVector, {tV4, tF, 4});
    m.op(spv::OpTypeVector, {tV2, tF, 2});
    m.op(spv::OpTypeInt, {tU, 32, 0});
    m.op(spv::OpTypeBool, {tBool});
    m.op(spv::OpTypePointer, {tPtrOutV4, spv::StorageClassOutput, tV4});
    m.op(spv::OpTypePointer, {tPtrInV4, spv::StorageClassInput, tV4});
    m.op(spv::OpTypePointer, {tPtrOutF, spv::StorageClassOutput, tF});
    m.op(spv::OpTypePointer, {tPtrInU, spv::StorageClassInput, tU});
    m.op(spv::OpTypeStruct, {tBlock, tV4, tF});
    m.op(spv::OpTypePointer, {tPtrOutBlock, spv::StorageClassOutput, tBlock});
    m.op(spv::OpConstant, {tU, c0, 0});
    m.op(spv::OpConstant, {tU, cU0, 0});
    m.op(spv::OpConstantTrue, {tBool, cTrue});
    m.op(spv::OpConstant, {tF, cF1, 0x3F800000u});
    m.op(spv::OpConstant, {tF, cF0, 0});
    m.op(spv::OpConstantComposite, {tV4, cZero4, cF0, cF0, cF0, cF0});
    if (o.position) {
        m.op(spv::OpVariable, {o.perVertexBlock ? tPtrOutBlock : tPtrOutV4, pos, spv::StorageClassOutput});
    }
    m.op(spv::OpVariable, {o.splitTexcoord ? tPtrOutF : tPtrOutV4, tex, spv::StorageClassOutput});
    if (o.splitTexcoord) {
        m.op(spv::OpVariable, {tPtrOutF, tex1, spv::StorageClassOutput});
    }
    m.op(spv::OpVariable, {tPtrOutV4, col, spv::StorageClassOutput});
    m.op(spv::OpVariable, {tPtrInV4, nrmIn, spv::StorageClassInput});
    m.op(spv::OpVariable, {tPtrInV4, inPos, spv::StorageClassInput});
    if (o.vertexIndex) {
        m.op(spv::OpVariable, {tPtrInU, vid, spv::StorageClassInput});
    }
    m.op(spv::OpFunction, {tVoid, main, 0, tFn});
    m.op(spv::OpLabel, {lEntry});
    m.op(spv::OpLoad, {tV4, ld, inPos});
    if (o.position) {
        if (o.perVertexBlock) {
            m.op(spv::OpAccessChain, {tPtrOutV4, ptrPos, pos, c0});
            m.op(spv::OpStore, {ptrPos, ld});
        } else {
            m.op(spv::OpStore, {pos, ld});
        }
    }
    m.op(spv::OpLoad, {tV4, ld2, nrmIn});
    if (o.splitTexcoord) {
        m.op(spv::OpStore, {tex, cF1});
        m.op(spv::OpStore, {tex1, cF0});
    } else {
        m.op(spv::OpStore, {tex, ld2});
    }
    // An unwritten output only gets the zero fill (dxbc-spirv, SM1/2).
    m.op(spv::OpStore, {col, o.writeColor ? ld : cZero4});
    if (o.returns >= 2) {
        m.op(spv::OpSelectionMerge, {lMerge, spv::SelectionControlMaskNone});
        m.op(spv::OpBranchConditional, {cTrue, lA, lB});
        m.op(spv::OpLabel, {lA});
        m.op(spv::OpReturn, {});
        m.op(spv::OpLabel, {lB});
        m.op(spv::OpBranch, {lMerge});
        m.op(spv::OpLabel, {lMerge});
    }
    if (o.returnValue) {
        m.op(spv::OpReturnValue, {cF0});
    } else {
        m.op(spv::OpReturn, {});
    }
    m.op(spv::OpFunctionEnd, {});
    return m.finish();
}

std::size_t countOp(const std::vector<std::uint32_t>& w, spv::Op op) {
    std::size_t n = 0;
    for (std::size_t i = 5; i < w.size();) {
        if ((w[i] & 0xFFFF) == std::uint32_t(op)) {
            ++n;
        }
        const std::uint32_t len = w[i] >> 16;
        if (len == 0) {
            break;
        }
        i += len;
    }
    return n;
}

bool hasDecoration(const std::vector<std::uint32_t>& w, std::uint32_t decoration, std::uint32_t value) {
    for (std::size_t i = 5; i < w.size();) {
        const std::uint32_t len = w[i] >> 16;
        if ((w[i] & 0xFFFF) == spv::OpDecorate && len >= 4 && w[i + 2] == decoration && w[i + 3] == value) {
            return true;
        }
        if (len == 0) {
            break;
        }
        i += len;
    }
    return false;
}

std::vector<std::pair<std::string, std::vector<std::uint32_t>>> builtinVariants() {
    std::vector<std::pair<std::string, std::vector<std::uint32_t>>> out;
    ModuleOptions o;
    out.emplace_back("builtin_plain", buildModule(o));
    o.perVertexBlock = true;
    out.emplace_back("builtin_pervertex", buildModule(o));
    o.perVertexBlock = false;
    o.returns = 2;
    out.emplace_back("builtin_two_returns", buildModule(o));
    o.returns = 1;
    o.vertexIndex = true;
    out.emplace_back("builtin_uint_vertex_index", buildModule(o));
    o.vertexIndex = false;
    o.splitTexcoord = true;
    out.emplace_back("builtin_split_texcoord", buildModule(o));
    o.splitTexcoord = false;
    o.color = false;
    o.inputNormal = false;
    out.emplace_back("builtin_position_only", buildModule(o));
    ModuleOptions old;
    old.version = 0x00010000;
    out.emplace_back("builtin_spirv10", buildModule(old));
    return out;
}

void testSpirv() {
    SpirvCaptureOptions options;
    options.descriptorSet = 3;
    options.binding = 17;

    const SpirvCaptureResult plain = addVertexCapture(buildModule({}), options);
    VC_CHECK_MSG(plain.transformed(), "plain: " + plain.detail);
    VC_CHECK(plain.returns == 1);
    VC_CHECK(plain.fields == (fields::kWritten | fields::kTexcoord | fields::kColor | fields::kNormalInput));
    VC_CHECK(hasDecoration(plain.words, spv::DecorationDescriptorSet, 3));
    VC_CHECK(hasDecoration(plain.words, spv::DecorationBinding, 17));
    VC_CHECK(hasDecoration(plain.words, spv::DecorationBuiltIn, spv::BuiltInVertexIndex));
    VC_CHECK(countOp(plain.words, spv::OpSelectionMerge) == 1 && countOp(plain.words, spv::OpReturn) == 1);
    VC_CHECK(plain.words[3] > buildModule({})[3]); // bound grew
    // Transforming twice is refused.
    VC_CHECK(addVertexCapture(plain.words, options).status == SpirvCaptureStatus::AlreadyTransformed);

    ModuleOptions two;
    two.returns = 2;
    const SpirvCaptureResult r2 = addVertexCapture(buildModule(two), options);
    VC_CHECK(r2.transformed() && r2.returns == 2 && countOp(r2.words, spv::OpSelectionMerge) == 3);

    ModuleOptions block;
    block.perVertexBlock = true;
    VC_CHECK(addVertexCapture(buildModule(block), options).transformed());

    ModuleOptions vid;
    vid.vertexIndex = true;
    const SpirvCaptureResult rv = addVertexCapture(buildModule(vid), options);
    VC_CHECK(rv.transformed() && countOp(rv.words, spv::OpBitcast) == 2); // uint -> int, int -> uint

    ModuleOptions none;
    none.color = false;
    none.inputNormal = false;
    const SpirvCaptureResult rn = addVertexCapture(buildModule(none), options);
    VC_CHECK(rn.transformed() && rn.fields == (fields::kWritten | fields::kTexcoord));
    ModuleOptions unwritten;
    unwritten.writeColor = false;
    VC_CHECK(addVertexCapture(buildModule(unwritten), options).fields ==
             (fields::kWritten | fields::kTexcoord | fields::kNormalInput));
    SpirvCaptureOptions noNormal = options;
    noNormal.captureInputNormal = false;
    VC_CHECK(addVertexCapture(buildModule({}), noNormal).fields == (fields::kWritten | fields::kTexcoord | fields::kColor));
    SpirvCaptureOptions byLocation = options;
    byLocation.inputNormalLocation = 3;
    VC_CHECK((addVertexCapture(buildModule(none), byLocation).fields & fields::kNormalInput) != 0);

    ModuleOptions old;
    old.version = 0x00010000;
    const SpirvCaptureResult ro = addVertexCapture(buildModule(old), options);
    VC_CHECK(ro.transformed() && countOp(ro.words, spv::OpExtension) == 1);

    ModuleOptions noPos;
    noPos.position = false;
    VC_CHECK(addVertexCapture(buildModule(noPos), options).status == SpirvCaptureStatus::NoPosition);
    ModuleOptions rv2;
    rv2.returnValue = true;
    VC_CHECK(addVertexCapture(buildModule(rv2), options).status == SpirvCaptureStatus::Unsupported);
    std::vector<std::uint32_t> broken = buildModule({});
    broken.resize(broken.size() - 1);
    VC_CHECK(addVertexCapture(broken, options).status == SpirvCaptureStatus::Malformed);
    VC_CHECK(addVertexCapture({}, options).status == SpirvCaptureStatus::Malformed);
    std::vector<std::uint32_t> fragment = buildModule({});
    for (std::size_t i = 5; i < fragment.size(); i += fragment[i] >> 16) {
        if ((fragment[i] & 0xFFFF) == spv::OpEntryPoint) {
            fragment[i + 1] = spv::ExecutionModelFragment;
        }
    }
    VC_CHECK(addVertexCapture(fragment, options).status == SpirvCaptureStatus::NoVertexEntryPoint);
    VC_CHECK(spirvCaptureStatusName(SpirvCaptureStatus::Transformed) == "transformed");
}

// ---- DXVK hand-over -------------------------------------------------------------------------------------

int g_offers = 0;
bool fakeSubstitute(void*, const tap::ShaderModule& m, std::vector<std::uint32_t>& out) {
    ++g_offers;
    SpirvCaptureOptions o;
    o.descriptorSet = m.captureSet;
    o.binding = m.captureBinding;
    SpirvCaptureResult r = addVertexCapture(std::span(m.spirv, m.wordCount), o);
    if (!r.transformed()) {
        return false;
    }
    out = std::move(r.words);
    return true;
}

void testHook() {
    std::uint32_t set = 0, binding = 0, slot = 0;
    VC_CHECK(!dxvk_hook::layoutBinding("vs.aaaa", &set, &binding, &slot)); // not enabled yet
    dxvk_hook::enable();
    VC_CHECK(dxvk_hook::enabled());
    VC_CHECK(dxvk_hook::layoutBinding("vs.aaaa", &set, &binding, &slot));
    VC_CHECK(set == dxvk_hook::kDescriptorSet && binding == dxvk_hook::kBinding && slot == dxvk_hook::kResourceSlot);
    VC_CHECK(!dxvk_hook::layoutBinding("fs.aaaa", &set, &binding, &slot));
    VC_CHECK(!dxvk_hook::layoutBinding(nullptr, &set, &binding, &slot));

    const std::vector<std::uint32_t> code = buildModule({});
    std::vector<std::uint32_t> out;
    VC_CHECK(!dxvk_hook::substituteCode("vs.aaaa", code.data(), code.size(), 1, 5, &out)); // no substitutor
    int owner = 0;
    dxvk_hook::setSubstitutor(&owner, &fakeSubstitute);
    VC_CHECK(dxvk_hook::substituteCode("vs.aaaa", code.data(), code.size(), 1, 5, &out) && !out.empty());
    VC_CHECK(hasDecoration(out, spv::DecorationBinding, 5));
    VC_CHECK(!dxvk_hook::substituteCode("vs.bbbb", code.data(), code.size(), 1, 5, &out)); // no layout latched
    int other = 0;
    dxvk_hook::clearSubstitutor(&other); // not the owner: kept
    VC_CHECK(dxvk_hook::substituteCode("vs.aaaa", code.data(), code.size(), 1, 5, &out));
    dxvk_hook::clearSubstitutor(&owner);
    VC_CHECK(!dxvk_hook::substituteCode("vs.aaaa", code.data(), code.size(), 1, 5, &out));
    VC_CHECK(g_offers == 2);
    const dxvk_hook::Stats st = dxvk_hook::stats();
    VC_CHECK(st.layouts == 1 && st.substituted == 2);
    // The names the DXVK patches declare resolve to the same registry.
    VC_CHECK(dxvk::fuseRelightVertexCaptureBinding("vs.cccc", &set, &binding, &slot));
}

// ---- tool modes -----------------------------------------------------------------------------------------

std::vector<std::uint32_t> readWords(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::vector<std::uint32_t> w(bytes.size() / 4);
    if (!w.empty()) {
        std::memcpy(w.data(), bytes.data(), w.size() * 4);
    }
    return w;
}

bool writeWords(const std::string& path, const std::vector<std::uint32_t>& w) {
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(w.data()), std::streamsize(w.size() * 4));
    return bool(f);
}

std::string baseName(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    std::string b = slash == std::string::npos ? path : path.substr(slash + 1);
    const std::size_t dot = b.rfind('.');
    return dot == std::string::npos ? b : b.substr(0, dot);
}

int emit(int argc, char** argv) {
    const std::string out = argv[2];
    SpirvCaptureOptions options;
    options.descriptorSet = 1;
    options.binding = 120;
    int failed = 0;
    auto one = [&](const std::string& name, const std::vector<std::uint32_t>& in) {
        const SpirvCaptureResult r = addVertexCapture(in, options);
        if (!r.transformed()) {
            std::printf("FAIL %s: %s (%s)\n", name.c_str(), std::string(spirvCaptureStatusName(r.status)).c_str(),
                        r.detail.c_str());
            ++failed;
            return;
        }
        const std::string path = out + "/" + name + ".spv";
        if (!writeWords(path, r.words)) {
            std::printf("FAIL %s: cannot write %s\n", name.c_str(), path.c_str());
            ++failed;
            return;
        }
        std::printf("emitted %s (fields 0x%x, %u returns)\n", path.c_str(), r.fields, r.returns);
    };
    for (const auto& [name, words] : builtinVariants()) {
        one(name, words);
    }
    for (int i = 3; i < argc; ++i) {
        const std::vector<std::uint32_t> words = readWords(argv[i]);
        if (words.empty()) {
            std::printf("FAIL %s: cannot read (build the rl_vertex_capture_test_shaders target)\n", argv[i]);
            ++failed;
            continue;
        }
        one(baseName(argv[i]), words);
    }
    return failed ? 1 : 0;
}

int analyze(const char* path) {
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream in(line);
        int swvp = 0;
        in >> swvp;
        std::vector<std::uint32_t> tokens;
        std::string tok;
        while (in >> tok) {
            tokens.push_back(std::uint32_t(std::strtoul(tok.c_str(), nullptr, 16)));
        }
        const ShaderConstantRanges r = analyzeVertexShader(tokens, ConstantLayout::forVertexShaders(swvp != 0));
        std::printf("%d %u %u %u\n", r.valid ? 1 : 0, r.maxConstIndexF, r.maxConstIndexI, r.maxConstIndexB);
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[1]) == "--emit") {
        return emit(argc, argv);
    }
    if (argc == 3 && std::string(argv[1]) == "--analyze") {
        return analyze(argv[2]);
    }
    testAnalysis();
    testHash();
    testBackTransform();
    testRing();
    testDrawCapture();
    testSpirv();
    testHook();
    std::printf("rl_vertex_capture_unit: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
