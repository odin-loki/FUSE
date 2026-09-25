// WP-2.3 forward transparency CPU gates (stub-safe): record layouts, the transparent-set collection and
// back-to-front sort (forward_reference.hpp) against a brute-force f64 evaluation, the premultiplied
// blend against the closed-form composite, zero allocations, and the API without a device.
//
//   fuse_rp_forward_cpu layout | collect | sort | blend | api
#include <fuse/renderer/culling/instance_cull_kernel.hpp>
#include <fuse/renderer/forward/forward_reference.hpp>
#include <fuse/renderer/forward/forward_transparency.hpp>
#include <fuse/renderer/forward/forward_types.hpp>
#include <fuse/renderer/gpu_scene/gpu_scene_types.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <new>
#include <random>
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
using namespace fuse::renderer::forward;
using namespace fuse::renderer::gpu_scene;
using fuse::f32;
using fuse::f64;
using fuse::u32;
using fuse::u64;
using fuse::math::Vec3;
using fuse::math::Vec4;

int g_failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

// --- camera (column-major, Vulkan clip space, forward depth) ---------------------------------------
struct Mat4 {
    f32 m[16] = {};
};

Mat4 mul(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (u32 c = 0; c < 4; ++c) {
        for (u32 row = 0; row < 4; ++row) {
            f32 s = 0.f;
            for (u32 k = 0; k < 4; ++k) {
                s += a.m[k * 4 + row] * b.m[c * 4 + k];
            }
            r.m[c * 4 + row] = s;
        }
    }
    return r;
}

Mat4 perspective(f32 fovY, f32 aspect, f32 zNear, f32 zFar) {
    const f32 f = 1.f / std::tan(fovY * 0.5f);
    Mat4 p{};
    p.m[0] = f / aspect;
    p.m[5] = -f;
    p.m[10] = zFar / (zNear - zFar);
    p.m[11] = -1.f;
    p.m[14] = zNear * zFar / (zNear - zFar);
    return p;
}

Mat4 orthographic(f32 halfW, f32 halfH, f32 zNear, f32 zFar) {
    Mat4 p{};
    p.m[0] = 1.f / halfW;
    p.m[5] = -1.f / halfH;
    p.m[10] = 1.f / (zNear - zFar);
    p.m[14] = zNear / (zNear - zFar);
    p.m[15] = 1.f;
    return p;
}

Mat4 lookAt(const Vec3& eye, const Vec3& at) {
    Vec3 f = (at - eye).normalized();
    const Vec3 s = fuse::math::cross(f, Vec3{0.f, 1.f, 0.f}).normalized();
    const Vec3 u = fuse::math::cross(s, f);
    Mat4 v{};
    v.m[0] = s.x;
    v.m[4] = s.y;
    v.m[8] = s.z;
    v.m[1] = u.x;
    v.m[5] = u.y;
    v.m[9] = u.z;
    v.m[2] = -f.x;
    v.m[6] = -f.y;
    v.m[10] = -f.z;
    v.m[12] = -s.dot(eye);
    v.m[13] = -u.dot(eye);
    v.m[14] = f.dot(eye);
    v.m[15] = 1.f;
    return v;
}

// --- random scene ---------------------------------------------------------------------------------
struct CpuScene {
    std::vector<GpuInstance> instances;
    std::vector<GpuTransform> transforms;
    std::vector<GpuMesh> meshes;
    std::vector<OpacityEntry> opacity;
};

CpuScene randomScene(std::mt19937& rng, u32 count) {
    std::uniform_real_distribution<f32> u(-1.f, 1.f);
    CpuScene s;
    for (u32 m = 0; m < 5u; ++m) {
        GpuMesh mesh{};
        mesh.boundsCenter[0] = 0.2f * u(rng);
        mesh.boundsCenter[1] = 0.2f * u(rng);
        mesh.boundsCenter[2] = 0.2f * u(rng);
        mesh.boundsRadius = m == 4u ? 0.f : 0.5f + 0.5f * (u(rng) * 0.5f + 0.5f); // mesh 4: no bounds
        mesh.indexCount = m == 3u ? 0u : 36u * (m + 1u);                             // mesh 3: no draw range
        mesh.firstIndex = 100u * m;
        s.meshes.push_back(mesh);
    }
    for (u32 i = 0; i < count; ++i) {
        GpuInstance inst{};
        inst.mesh = static_cast<u32>(rng() % 6u); // 5 = out of range
        const u32 pick = static_cast<u32>(rng() % 8u);
        inst.flags = pick == 0u ? 0u
                     : pick == 1u ? (kInstanceValid | kInstanceTransparent)
                     : pick == 2u ? (kInstanceValid | kInstanceVisible)
                                  : kTransparentFlags;
        inst.generation = static_cast<u32>(rng() % 3u);
        s.instances.push_back(inst);
        GpuTransform t{};
        const f32 sc = 0.3f + 1.2f * (u(rng) * 0.5f + 0.5f);
        const f32 yaw = 3.f * u(rng);
        t.rows[0][0] = std::cos(yaw) * sc;
        t.rows[0][2] = std::sin(yaw) * sc;
        t.rows[1][1] = (rng() % 5u == 0u ? -1.f : 1.f) * sc; // some mirrored
        t.rows[2][0] = -std::sin(yaw) * sc;
        t.rows[2][2] = std::cos(yaw) * sc;
        t.rows[0][3] = 30.f * u(rng);
        t.rows[1][3] = 10.f * u(rng);
        t.rows[2][3] = -25.f + 30.f * u(rng);
        s.transforms.push_back(t);
        OpacityEntry e{};
        const u32 o = static_cast<u32>(rng() % 5u);
        e.set = o != 0u;
        e.generation = o == 4u ? inst.generation + 1u : inst.generation; // 4: stale generation
        e.opacity = o == 3u ? 1.7f : (o == 2u ? -0.3f : 0.05f + 0.9f * (u(rng) * 0.5f + 0.5f));
        s.opacity.push_back(e);
    }
    return s;
}

CollectParams params(const CpuScene& s, const f32* viewProj, bool frustum) {
    CollectParams p{};
    p.instances = s.instances.data();
    p.transforms = s.transforms.data();
    p.instanceCount = static_cast<u32>(s.instances.size());
    p.meshes = s.meshes.data();
    p.meshCount = static_cast<u32>(s.meshes.size());
    p.viewProj = viewProj;
    p.opacity = s.opacity.data();
    p.opacityCount = static_cast<u32>(s.opacity.size());
    p.frustumCull = frustum;
    return p;
}

// Brute force in f64: the expected draws (slot order back to front) with their opacity.
struct Expected {
    u32 slot;
    f64 key;
    f32 opacity;
    bool mirrored;
    f64 margin; ///< smallest (signed plane distance + radius) over the six planes; < 0 = outside
};

/// f32 rounding of the key / plane expressions (a few ulp of values up to ~10^2): decisions closer than
/// this to a boundary may go either way and are accepted both ways.
constexpr f64 kBorderline = 1e-3;

std::vector<Expected> bruteForce(const CpuScene& s, const f32* m, bool frustum, u32* culledOut) {
    std::vector<Expected> out;
    const bool ortho = m[3] == 0.f && m[7] == 0.f && m[11] == 0.f;
    u32 culled = 0;
    for (u32 i = 0; i < s.instances.size(); ++i) {
        const GpuInstance& inst = s.instances[i];
        const bool flags = (inst.flags & kInstanceValid) && (inst.flags & kInstanceVisible) && (inst.flags & kInstanceTransparent);
        if (!flags || inst.mesh >= s.meshes.size()) {
            continue;
        }
        const GpuMesh& mesh = s.meshes[inst.mesh];
        if (mesh.indexCount == 0u || mesh.boundsRadius <= 0.f) {
            continue;
        }
        const GpuTransform& t = s.transforms[i];
        f64 c[3];
        for (u32 r = 0; r < 3u; ++r) {
            c[r] = static_cast<f64>(t.rows[r][3]);
            for (u32 k = 0; k < 3u; ++k) {
                c[r] += static_cast<f64>(t.rows[r][k]) * mesh.boundsCenter[k];
            }
        }
        f64 scale = 0.0;
        for (u32 k = 0; k < 3u; ++k) {
            scale = std::max(scale, std::sqrt(static_cast<f64>(t.rows[0][k]) * t.rows[0][k] + static_cast<f64>(t.rows[1][k]) * t.rows[1][k] +
                                              static_cast<f64>(t.rows[2][k]) * t.rows[2][k]));
        }
        const f64 radius = mesh.boundsRadius * scale;
        f64 clip[4];
        for (u32 r = 0; r < 4u; ++r) {
            clip[r] = static_cast<f64>(m[12 + r]) + m[r] * c[0] + m[4 + r] * c[1] + m[8 + r] * c[2];
        }
        f64 margin = 1e30;
        for (u32 k = 0; k < 6u; ++k) {
            f64 pl[4];
            for (u32 j = 0; j < 4u; ++j) {
                const f64 w = m[j * 4 + 3];
                const f64 x = m[j * 4 + 0];
                const f64 y = m[j * 4 + 1];
                const f64 z = m[j * 4 + 2];
                const f64 v[6] = {w + x, w - x, w + y, w - y, z, w - z};
                pl[j] = v[k];
            }
            const f64 n = std::sqrt(pl[0] * pl[0] + pl[1] * pl[1] + pl[2] * pl[2]);
            margin = std::min(margin, (pl[0] * c[0] + pl[1] * c[1] + pl[2] * c[2] + pl[3]) / n + radius);
        }
        if (frustum && margin < 0.0) {
            ++culled;
            if (margin > -kBorderline) {
                out.push_back({i, 0.0, 0.f, false, margin}); // borderline: either decision accepted
            }
            continue;
        }
        f32 opacity = 1.f;
        const OpacityEntry& e = s.opacity[i];
        if (e.set && e.generation == inst.generation) {
            opacity = std::min(std::max(e.opacity, 0.f), 1.f);
        }
        const auto& r = t.rows;
        const f64 det = static_cast<f64>(r[0][0]) * (static_cast<f64>(r[1][1]) * r[2][2] - static_cast<f64>(r[1][2]) * r[2][1]) -
                        static_cast<f64>(r[0][1]) * (static_cast<f64>(r[1][0]) * r[2][2] - static_cast<f64>(r[1][2]) * r[2][0]) +
                        static_cast<f64>(r[0][2]) * (static_cast<f64>(r[1][0]) * r[2][1] - static_cast<f64>(r[1][1]) * r[2][0]);
        out.push_back({i, ortho ? clip[2] : clip[3], opacity, det < 0.0, frustum ? margin : 1e30});
    }
    std::sort(out.begin(), out.end(), [](const Expected& a, const Expected& b) {
        return a.key != b.key ? a.key > b.key : a.slot < b.slot;
    });
    if (culledOut != nullptr) {
        *culledOut = culled;
    }
    return out;
}

Mat4 camera(u32 k, bool ortho) {
    const f32 t = static_cast<f32>(k);
    const Vec3 eye{-3.f + 1.1f * t, 1.f + 0.3f * t, 6.f - 0.5f * t};
    const Vec3 at{0.4f * t - 1.f, -0.5f, -20.f};
    return mul(ortho ? orthographic(12.f, 8.f, 0.3f, 80.f) : perspective(1.1f, 4.f / 3.f, 0.3f, 90.f), lookAt(eye, at));
}

// --- suites ---------------------------------------------------------------------------------------
void suiteLayout() {
    expect(sizeof(ForwardFrameConstants) == 128u, "ForwardFrameConstants 128 bytes");
    expect(sizeof(ForwardDraw) == 16u, "ForwardDraw 16 bytes");
    expect(sizeof(ForwardPush) == 16u, "ForwardPush 16 bytes");
    expect(sizeof(ForwardDumpTexel) == 96u, "ForwardDumpTexel 96 bytes");
    expect(kInstanceTransparent == 32u, "kInstanceTransparent == 32 (gpu_scene.glsl FUSE_INSTANCE_TRANSPARENT)");
    const u32 others = kInstanceValid | kInstanceVisible | kInstanceCastShadow | kInstanceReceiveShadow | kInstanceStatic;
    expect((kInstanceTransparent & others) == 0u, "kInstanceTransparent is a new bit");
    // The opaque culler never sees transparent instances.
    GpuInstance inst{};
    inst.mesh = 0;
    inst.flags = kInstanceValid | kInstanceVisible;
    expect(culling::cull_kernel::eligible(inst, 1u), "opaque instance eligible for the culler");
    inst.flags |= kInstanceTransparent;
    expect(!culling::cull_kernel::eligible(inst, 1u), "transparent instance not eligible for the culler");
    expect(kForwardColorFormat == static_cast<u32>(GpuFormat::R16G16B16A16Sfloat), "colour target RGBA16F");
#if defined(FUSE_RP_FORWARD_SHADER_DIR)
    // The media fields (kForwardFlagAerial / kForwardFlagFog) close the record in both shader languages, in C++ order.
    expect(offsetof(ForwardFrameConstants, flags) == 108u && offsetof(ForwardFrameConstants, atmosphere) == 112u &&
               offsetof(ForwardFrameConstants, fog) == 120u && kForwardFlagAerial == 1u && kForwardFlagFog == 2u,
           "ForwardFrameConstants: flags 108, atmosphere 112, fog 120; ForwardFlag bits");
    for (const char* file : {"/fw_common.glsl", "/fw_common.slang"}) {
        std::ifstream in(std::string(FUSE_RP_FORWARD_SHADER_DIR) + file);
        std::stringstream ss;
        ss << in.rdbuf();
        const std::string text = ss.str();
        const bool glsl = std::string(file).find("glsl") != std::string::npos;
        const size_t begin = text.find(glsl ? "struct FuseFwFrame {" : "struct FwFrame {");
        const size_t end = text.find("};", begin);
        const std::string body = begin != std::string::npos && end != std::string::npos ? text.substr(begin, end - begin) : "";
        const size_t f = body.find("uint flags;");
        const size_t a = body.find("uint64_t atmosphere;");
        const size_t g = body.find("uint64_t fog;");
        expect(f != std::string::npos && a != std::string::npos && g != std::string::npos && f < a && a < g &&
                   body.find("reserved") == std::string::npos,
               "fw_common FwFrame: flags, atmosphere, fog in C++ order (no reserved words left)");
        expect(glsl ? (text.find("#define FUSE_FW_FLAG_AERIAL 1u") != std::string::npos &&
                       text.find("#define FUSE_FW_FLAG_FOG 2u") != std::string::npos)
                    : (text.find("kFuseFwFlagAerial = 1u") != std::string::npos && text.find("kFuseFwFlagFog = 2u") != std::string::npos),
               "fw_common: the ForwardFlag bits");
    }
#endif
}

void suiteCollect() {
    std::mt19937 rng(20260924);
    u32 total = 0;
    u32 totalCulled = 0;
    u32 mirroredSeen = 0;
    u32 staleSeen = 0;
    std::vector<SortedDraw> out(4096);
    for (u32 trial = 0; trial < 12u; ++trial) {
        const CpuScene s = randomScene(rng, 1500u);
        const bool ortho = trial % 4u == 3u;
        const Mat4 vp = camera(trial, ortho);
        for (const bool frustum : {true, false}) {
            u32 culled = 0;
            const std::vector<Expected> expected = bruteForce(s, vp.m, frustum, &culled);
            const CollectResult r = collect_transparent(params(s, vp.m, frustum), out.data(), static_cast<u32>(out.size()));
            // Set: every clearly-inside candidate drawn, nothing clearly outside drawn.
            std::vector<const Expected*> bySlot(s.instances.size(), nullptr);
            for (const Expected& e : expected) {
                bySlot[e.slot] = &e;
            }
            std::vector<char> drawn(s.instances.size(), 0);
            bool same = r.dropped == 0u;
            for (u32 i = 0; same && i < r.count; ++i) {
                const Expected* e = bySlot[out[i].slot];
                same = e != nullptr && e->margin > -kBorderline && out[i].opacity == e->opacity && out[i].mirrored == e->mirrored &&
                       out[i].mesh == s.instances[out[i].slot].mesh;
                drawn[out[i].slot] = 1;
                mirroredSeen += out[i].mirrored ? 1u : 0u;
            }
            for (const Expected& e : expected) {
                same = same && (drawn[e.slot] != 0 || e.margin < kBorderline);
            }
            // Order: back to front up to the f32 key rounding; exact key ties by ascending slot.
            for (u32 i = 1; same && i < r.count; ++i) {
                const f64 ka = bySlot[out[i - 1].slot]->key;
                const f64 kb = bySlot[out[i].slot]->key;
                same = ka >= kb - kBorderline && out[i - 1].key >= out[i].key &&
                       (out[i - 1].key != out[i].key || out[i - 1].slot < out[i].slot);
            }
            if (!same) {
                std::fprintf(stderr, "  trial %u frustum %d: count %u vs %zu, culled %u vs %u\n", trial, frustum ? 1 : 0, r.count,
                             expected.size(), r.culled, culled);
            }
            expect(same, "collect_transparent == brute force (set, order, opacity, mirroring)");
            total += r.count;
            totalCulled += r.culled;
        }
        for (u32 i = 0; i < s.instances.size(); ++i) {
            staleSeen += (s.opacity[i].set && s.opacity[i].generation != s.instances[i].generation) ? 1u : 0u;
        }
    }
    std::printf("  collect: %u draws over 24 cases, %u frustum-culled, %u mirrored, %u stale opacity entries ignored\n", total,
                totalCulled, mirroredSeen, staleSeen);
    expect(totalCulled > 0u && mirroredSeen > 0u && staleSeen > 0u, "the random scenes exercise culling, mirroring, stale opacity");

    // Capacity: the nearest `capacity` are kept, still back to front; zero allocations.
    const CpuScene s = randomScene(rng, 3000u);
    const Mat4 vp = camera(1u, false);
    std::vector<Expected> expected = bruteForce(s, vp.m, true, nullptr);
    expected.erase(std::remove_if(expected.begin(), expected.end(), [](const Expected& e) { return e.margin < 0.0; }), expected.end());
    const u32 capacity = static_cast<u32>(expected.size() / 3u);
    t_allocations = 0;
    t_count = true;
    const CollectResult r = collect_transparent(params(s, vp.m, true), out.data(), capacity);
    t_count = false;
    // Kept = the `capacity` smallest keys (up to f32 rounding at the cut), still back to front.
    const f64 cut = expected[expected.size() - capacity].key;
    bool nearest = r.count == capacity;
    for (u32 i = 0; nearest && i < capacity; ++i) {
        f64 key = 0.0;
        for (const Expected& e : expected) {
            key = e.slot == out[i].slot ? e.key : key;
        }
        nearest = key <= cut + kBorderline && (i == 0u || out[i - 1].key >= out[i].key);
    }
    std::printf("  capacity %u of %zu: dropped %u, allocations %llu\n", capacity, expected.size(), r.dropped, t_allocations);
    expect(nearest, "over capacity: the nearest instances are kept, back to front");
    expect(t_allocations == 0u, "collect_transparent allocates nothing");
}

void suiteSort() {
    // Keys and ties: equal keys order by slot; the key is clip w (perspective) / clip z (orthographic).
    CpuScene s;
    GpuMesh mesh{};
    mesh.boundsRadius = 1.f;
    mesh.indexCount = 3u;
    s.meshes.push_back(mesh);
    const f32 zs[6] = {-5.f, -9.f, -5.f, -2.f, -9.f, -14.f};
    for (u32 i = 0; i < 6u; ++i) {
        GpuInstance inst{};
        inst.mesh = 0;
        inst.flags = kTransparentFlags;
        s.instances.push_back(inst);
        GpuTransform t{};
        t.rows[2][3] = zs[i];
        s.transforms.push_back(t);
        s.opacity.push_back(OpacityEntry{});
    }
    const u32 expectedOrder[6] = {5u, 1u, 4u, 0u, 2u, 3u};
    for (const bool ortho : {false, true}) {
        const Mat4 vp = mul(ortho ? orthographic(10.f, 10.f, 0.1f, 50.f) : perspective(1.2f, 1.f, 0.1f, 50.f),
                            lookAt(Vec3{0.f, 0.f, 0.f}, Vec3{0.f, 0.f, -1.f}));
        SortedDraw out[6];
        const CollectResult r = collect_transparent(params(s, vp.m, true), out, 6u);
        bool ok = r.count == 6u;
        for (u32 i = 0; ok && i < 6u; ++i) {
            ok = out[i].slot == expectedOrder[i] && out[i].opacity == 1.f;
        }
        for (u32 i = 1; ok && i < 6u; ++i) {
            ok = out[i - 1].key >= out[i].key;
        }
        expect(ok, ortho ? "orthographic: back to front by clip z, ties by slot" : "perspective: back to front by clip w, ties by slot");
    }
    // A sphere straddling the near plane is kept; one fully behind the camera is culled.
    s.transforms[0].rows[2][3] = 0.5f;
    s.transforms[1].rows[2][3] = 5.f;
    const Mat4 vp = mul(perspective(1.2f, 1.f, 0.1f, 50.f), lookAt(Vec3{0.f, 0.f, 0.f}, Vec3{0.f, 0.f, -1.f}));
    SortedDraw out[6];
    const CollectResult r = collect_transparent(params(s, vp.m, true), out, 6u);
    bool has0 = false;
    bool has1 = false;
    for (u32 i = 0; i < r.count; ++i) {
        has0 = has0 || out[i].slot == 0u;
        has1 = has1 || out[i].slot == 1u;
    }
    expect(has0 && !has1 && r.culled == 1u, "near-plane straddler kept, sphere behind the camera culled");
}

void suiteBlend() {
    std::mt19937 rng(77);
    std::uniform_real_distribution<f32> u(0.f, 1.f);
    f64 maxErr = 0.0;
    for (u32 trial = 0; trial < 2000u; ++trial) {
        const u32 layers = 1u + trial % 5u;
        const Vec4 dst0{4.f * u(rng), 4.f * u(rng), 4.f * u(rng), trial % 3u == 0u ? 0.f : 1.f};
        Vec4 dst = dst0;
        Vec3 c[5];
        f32 a[5];
        for (u32 i = 0; i < layers; ++i) {
            c[i] = {8.f * u(rng), 8.f * u(rng), 8.f * u(rng)};
            a[i] = trial % 7u == 0u ? (i % 2u == 0u ? 0.f : 1.f) : u(rng);
            dst = blend_over(c[i], a[i], dst);
        }
        // Closed form: sum_i c_i a_i prod_{j > i} (1 - a_j) + dst0 prod_j (1 - a_j).
        f64 closed[3] = {0.0, 0.0, 0.0};
        f64 transmit = 1.0;
        for (u32 i = layers; i-- > 0;) {
            closed[0] += static_cast<f64>(c[i].x) * a[i] * transmit;
            closed[1] += static_cast<f64>(c[i].y) * a[i] * transmit;
            closed[2] += static_cast<f64>(c[i].z) * a[i] * transmit;
            transmit *= 1.0 - a[i];
        }
        closed[0] += dst0.x * transmit;
        closed[1] += dst0.y * transmit;
        closed[2] += dst0.z * transmit;
        const f64 alpha = 1.0 - (1.0 - dst0.w) * transmit;
        const f64 e = std::max({std::fabs(closed[0] - dst.x), std::fabs(closed[1] - dst.y), std::fabs(closed[2] - dst.z),
                                std::fabs(alpha - dst.w)});
        maxErr = std::max(maxErr, e);
    }
    std::printf("  blend: sequential premultiplied over vs closed form: max error %.3g\n", maxErr);
    expect(maxErr < 1e-5, "blend_over composes to the closed-form composite");
    const Vec4 dst{0.3f, 0.2f, 0.1f, 1.f};
    const Vec4 one = blend_over(Vec3{1.5f, 2.5f, 3.5f}, 1.f, dst);
    expect(one.x == 1.5f && one.y == 2.5f && one.z == 3.5f && one.w == 1.f, "alpha 1 replaces the destination exactly");
    const Vec4 zero = blend_over(Vec3{1.5f, 2.5f, 3.5f}, 0.f, dst);
    expect(zero.x == dst.x && zero.y == dst.y && zero.z == dst.z && zero.w == dst.w, "alpha 0 keeps the destination exactly");
}

void suiteApi() {
    const ForwardCapabilities caps = queryForwardCapabilities(nullptr);
    expect(!caps.forward, "no device: not usable");
    std::printf("  capabilities without a device: %s\n", caps.reason);
    ForwardTransparency f;
    ForwardTransparencyDesc d{};
    expect(!f.init(d), "init without a device fails");
    expect(!f.valid(), "not valid after a failed init");
    expect(!f.setOpacity(InstanceHandle{0u, 0u}, 0.5f), "setOpacity before init fails");
    ForwardFrameDesc fd{};
    expect(!f.beginFrame(1u, fd), "beginFrame before init fails");
    expect(f.drawCount() == 0u, "no draws");
    f.destroy();
}

} // namespace

int main(int argc, char** argv) {
    const std::string suite = argc > 1 ? argv[1] : "all";
    const bool all = suite == "all";
    if (all || suite == "layout") {
        suiteLayout();
    }
    if (all || suite == "collect") {
        suiteCollect();
    }
    if (all || suite == "sort") {
        suiteSort();
    }
    if (all || suite == "blend") {
        suiteBlend();
    }
    if (all || suite == "api") {
        suiteApi();
    }
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("PASS (%s)\n", suite.c_str());
    return 0;
}
