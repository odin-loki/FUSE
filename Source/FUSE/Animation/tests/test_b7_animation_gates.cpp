// B7.1 / B7.10 animation gate proofs. Every check compares the engine against an independent
// double-precision reference (hand-rolled quaternions, forward kinematics, law of cosines) or a
// brute-force property check, never against the engine's own helpers.

#include <fuse/animation/animator.hpp>
#include <fuse/animation/blend_tree.hpp>
#include <fuse/animation/clip.hpp>
#include <fuse/animation/ik_solver.hpp>
#include <fuse/animation/skeleton.hpp>
#include <fuse/animation/skinning.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <new>
#include <random>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------------------------
// Allocation accounting (live-block count) for the Animator leak gate.
// ---------------------------------------------------------------------------------------------

namespace {
std::atomic<long long> g_liveAllocations{0};
} // namespace

void* operator new(std::size_t size) {
    void* p = std::malloc(size == 0 ? 1 : size);
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    g_liveAllocations.fetch_add(1, std::memory_order_relaxed);
    return p;
}

void operator delete(void* p) noexcept {
    if (p != nullptr) {
        g_liveAllocations.fetch_sub(1, std::memory_order_relaxed);
        std::free(p);
    }
}

void operator delete(void* p, std::size_t) noexcept {
    operator delete(p);
}

namespace {

namespace anim = fuse::animation;
using fuse::f32;
using fuse::s32;
using fuse::u32;
using fuse::u64;

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

// ---------------------------------------------------------------------------------------------
// Independent double-precision reference math.
// ---------------------------------------------------------------------------------------------

struct D3 {
    double x = 0, y = 0, z = 0;
};
struct DQ {
    double x = 0, y = 0, z = 0, w = 1;
};
struct DM {
    // Column-major 4x4, same layout as fuse::ecs::mat4.
    std::array<double, 16> m{};
};

D3 d3(const anim::vec3& v) {
    return {v.x, v.y, v.z};
}
DQ dq(const anim::quat& q) {
    return {q.x, q.y, q.z, q.w};
}
D3 add(D3 a, D3 b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
D3 sub(D3 a, D3 b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
D3 scale(D3 a, double s) {
    return {a.x * s, a.y * s, a.z * s};
}
double dot(D3 a, D3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
double len(D3 a) {
    return std::sqrt(dot(a, a));
}
D3 norm(D3 a) {
    return scale(a, 1.0 / len(a));
}
D3 lerp3(D3 a, D3 b, double t) {
    return add(a, scale(sub(b, a), t));
}

DQ qmul(DQ a, DQ b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
DQ qnorm(DQ q) {
    const double l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return {q.x / l, q.y / l, q.z / l, q.w / l};
}
DQ qaxis(D3 axis, double radians) {
    const D3 a = norm(axis);
    const double s = std::sin(radians * 0.5);
    return {a.x * s, a.y * s, a.z * s, std::cos(radians * 0.5)};
}
D3 qrot(DQ q, D3 v) {
    // Explicit q * (v,0) * conj(q).
    const DQ p{v.x, v.y, v.z, 0.0};
    const DQ r = qmul(qmul(q, p), DQ{-q.x, -q.y, -q.z, q.w});
    return {r.x, r.y, r.z};
}
/// Geodesic slerp along the shortest arc, via the angle/axis of the relative rotation.
DQ slerp(DQ a, DQ b, double t) {
    double d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (d < 0) {
        b = {-b.x, -b.y, -b.z, -b.w};
        d = -d;
    }
    // rel = conj(a) * b ; result = a * rel^t
    const DQ rel = qmul(DQ{-a.x, -a.y, -a.z, a.w}, b);
    const double vlen = std::sqrt(rel.x * rel.x + rel.y * rel.y + rel.z * rel.z);
    if (vlen < 1e-12) {
        return a;
    }
    const double half = std::atan2(vlen, rel.w);
    const double s = std::sin(half * t) / vlen;
    const DQ relT{rel.x * s, rel.y * s, rel.z * s, std::cos(half * t)};
    return qnorm(qmul(a, relT));
}
/// Rotation angle (radians) between two unit quaternions, sign-agnostic. Uses the chord form
/// 4 * atan2(|a - b|, |a + b|), which stays accurate for tiny angles (acos of the dot does not).
double qangle(DQ a, DQ b) {
    if (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0) {
        b = {-b.x, -b.y, -b.z, -b.w};
    }
    const double dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z, dw = a.w - b.w;
    const double sx = a.x + b.x, sy = a.y + b.y, sz = a.z + b.z, sw = a.w + b.w;
    return 4.0 * std::atan2(std::sqrt(dx * dx + dy * dy + dz * dz + dw * dw),
                            std::sqrt(sx * sx + sy * sy + sz * sz + sw * sw));
}

DM dm_trs(D3 t, DQ q, D3 s) {
    // Built column by column from rotated basis vectors (independent of mat4_from_trs).
    DM r;
    const D3 cx = scale(qrot(q, {1, 0, 0}), s.x);
    const D3 cy = scale(qrot(q, {0, 1, 0}), s.y);
    const D3 cz = scale(qrot(q, {0, 0, 1}), s.z);
    r.m = {cx.x, cx.y, cx.z, 0, cy.x, cy.y, cy.z, 0, cz.x, cz.y, cz.z, 0, t.x, t.y, t.z, 1};
    return r;
}
DM dm_mul(const DM& a, const DM& b) {
    DM r;
    for (int c = 0; c < 4; ++c) {
        for (int row = 0; row < 4; ++row) {
            double sum = 0;
            for (int k = 0; k < 4; ++k) {
                sum += a.m[k * 4 + row] * b.m[c * 4 + k];
            }
            r.m[c * 4 + row] = sum;
        }
    }
    return r;
}
D3 dm_point(const DM& a, D3 p) {
    return {a.m[0] * p.x + a.m[4] * p.y + a.m[8] * p.z + a.m[12],
            a.m[1] * p.x + a.m[5] * p.y + a.m[9] * p.z + a.m[13],
            a.m[2] * p.x + a.m[6] * p.y + a.m[10] * p.z + a.m[14]};
}
D3 dm_translation(const DM& a) {
    return {a.m[12], a.m[13], a.m[14]};
}
anim::mat4 to_mat4(const DM& a) {
    anim::mat4 r{};
    for (int i = 0; i < 16; ++i) {
        r.data[i] = static_cast<f32>(a.m[i]);
    }
    return r;
}
DM from_mat4(const anim::mat4& a) {
    DM r;
    for (int i = 0; i < 16; ++i) {
        r.m[i] = a.data[i];
    }
    return r;
}
double max_abs_diff(const anim::mat4& a, const DM& b) {
    double worst = 0;
    for (int i = 0; i < 16; ++i) {
        worst = std::max(worst, std::fabs(static_cast<double>(a.data[i]) - b.m[i]));
    }
    return worst;
}
D3 world_pos(const anim::mat4& m) {
    return {m.data[12], m.data[13], m.data[14]};
}

/// Local TRS for the reference solver: world = parent_world * T * R * S.
struct RefTRS {
    D3 t;
    DQ r;
    D3 s{1, 1, 1};
};

std::vector<DM> reference_fk(const anim::Skeleton& skel, const std::vector<RefTRS>& locals) {
    std::vector<DM> world(skel.bones.size());
    for (size_t i = 0; i < skel.bones.size(); ++i) {
        const DM local = dm_trs(locals[i].t, locals[i].r, locals[i].s);
        const s32 parent = skel.bones[i].parent_index;
        world[i] = parent >= 0 ? dm_mul(world[static_cast<size_t>(parent)], local) : local;
    }
    return world;
}

anim::vec3 v3(double x, double y, double z) {
    return {static_cast<f32>(x), static_cast<f32>(y), static_cast<f32>(z), 0.f};
}
anim::quat q4(DQ q) {
    return {static_cast<f32>(q.x), static_cast<f32>(q.y), static_cast<f32>(q.z), static_cast<f32>(q.w)};
}

DQ random_quat(std::mt19937& rng) {
    // Shoemake uniform random rotation.
    std::uniform_real_distribution<double> u(0.0, 1.0);
    const double u1 = u(rng), u2 = u(rng), u3 = u(rng);
    const double a = std::sqrt(1 - u1), b = std::sqrt(u1);
    const double pi2 = 2.0 * 3.14159265358979323846;
    return {a * std::sin(pi2 * u2), a * std::cos(pi2 * u2), b * std::sin(pi2 * u3), b * std::cos(pi2 * u3)};
}

D3 random_unit(std::mt19937& rng) {
    std::normal_distribution<double> n(0.0, 1.0);
    D3 v{n(rng), n(rng), n(rng)};
    while (len(v) < 1e-6) {
        v = {n(rng), n(rng), n(rng)};
    }
    return norm(v);
}

anim::Bone make_bone(const char* name, s32 parent, const RefTRS& bind) {
    anim::Bone bone{};
    std::strncpy(bone.name, name, sizeof(bone.name) - 1);
    bone.parent_index = parent;
    bone.local_transform = to_mat4(dm_trs(bind.t, bind.r, bind.s));
    return bone;
}

std::string scratch_path(const char* leaf) {
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
    if (ec) {
        dir = ".";
    }
    return (dir / (std::string("fuse_b7_anim_") + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" + leaf)).string();
}

// ---------------------------------------------------------------------------------------------
// Gate: Skeleton loads from binary format with correct hierarchy — parent/child chain verified.
// ---------------------------------------------------------------------------------------------

void gateSkeletonBinaryLoad() {
    std::mt19937 rng(0xB71u);
    std::uniform_real_distribution<double> off(-1.0, 1.0);

    // 40-bone branching skeleton: bone i's parent is a random earlier bone.
    anim::Skeleton skel;
    std::vector<RefTRS> binds;
    for (u32 i = 0; i < 40; ++i) {
        RefTRS bind{{off(rng), off(rng) + 1.0, off(rng)}, random_quat(rng), {1, 1, 1}};
        const s32 parent = i == 0 ? -1 : static_cast<s32>(std::uniform_int_distribution<u32>(0, i - 1)(rng));
        const std::string name = "bone_" + std::to_string(i);
        skel.bones.push_back(make_bone(name.c_str(), parent, bind));
        binds.push_back(bind);
    }
    skel.bone_count = static_cast<u32>(skel.bones.size());
    // Inverse bind from the reference FK so the round trip covers both matrices per bone.
    const std::vector<DM> refBindWorld = reference_fk(skel, binds);
    for (u32 i = 0; i < skel.bone_count; ++i) {
        skel.bones[i].inverse_bind = anim::mat4_inverse_affine(to_mat4(refBindWorld[i]));
    }

    const std::string path = scratch_path("skeleton.fskl");
    expectTrue(skel.save(path.c_str()), "skeleton binary save succeeds");

    anim::Skeleton loaded;
    expectTrue(loaded.load(path.c_str()), "skeleton binary load succeeds");
    expectTrue(loaded.bone_count == 40u && loaded.bones.size() == 40u, "loaded skeleton bone count");
    expectTrue(loaded.has_valid_hierarchy(), "loaded skeleton hierarchy valid");

    bool namesMatch = true;
    bool parentsMatch = true;
    bool matricesMatch = true;
    for (u32 i = 0; i < loaded.bones.size() && i < skel.bones.size(); ++i) {
        namesMatch = namesMatch && std::strcmp(loaded.bones[i].name, skel.bones[i].name) == 0 &&
                     loaded.find_bone(skel.bones[i].name) == static_cast<s32>(i);
        parentsMatch = parentsMatch && loaded.bones[i].parent_index == skel.bones[i].parent_index;
        matricesMatch = matricesMatch &&
                        std::memcmp(loaded.bones[i].local_transform.data.data(),
                                    skel.bones[i].local_transform.data.data(), sizeof(f32) * 16) == 0 &&
                        std::memcmp(loaded.bones[i].inverse_bind.data.data(),
                                    skel.bones[i].inverse_bind.data.data(), sizeof(f32) * 16) == 0;
    }
    expectTrue(namesMatch, "loaded bone names and lookups match");
    expectTrue(parentsMatch, "loaded parent indices match");
    expectTrue(matricesMatch, "loaded bind/inverse-bind matrices bit-identical");

    // Parent/child chain: walking parents from every bone reaches the single root, and world
    // transforms (engine chain walk and SoA FK) match the double-precision reference.
    bool chainsReachRoot = true;
    double worstWorld = 0.0;
    double worstBindIdentity = 0.0;
    const anim::PoseSoA bindPose = anim::PoseSoA::from_bind_pose(loaded);
    for (u32 i = 0; i < loaded.bone_count; ++i) {
        s32 cursor = static_cast<s32>(i);
        u32 steps = 0;
        while (loaded.bones[static_cast<u32>(cursor)].parent_index >= 0 && steps <= loaded.bone_count) {
            cursor = loaded.bones[static_cast<u32>(cursor)].parent_index;
            ++steps;
        }
        chainsReachRoot = chainsReachRoot && cursor == 0 && steps <= loaded.bone_count;
        worstWorld = std::max(worstWorld, max_abs_diff(loaded.compute_world_transform(i), refBindWorld[i]));
        worstWorld = std::max(worstWorld, max_abs_diff(bindPose.bone_world_transforms[i], refBindWorld[i]));
        const DM skinning = dm_mul(from_mat4(bindPose.bone_world_transforms[i]), from_mat4(loaded.bones[i].inverse_bind));
        worstBindIdentity = std::max(worstBindIdentity, max_abs_diff(anim::mat4::identity(), skinning));
    }
    expectTrue(chainsReachRoot, "every loaded bone's parent chain terminates at the root");
    expectTrue(worstWorld < 1e-4, "loaded skeleton world transforms match reference FK");
    expectTrue(worstBindIdentity < 1e-4, "bind world * inverse bind is identity after load");
    std::printf("[gate] skeleton binary load: 40 bones, world err %.2e, bind*invbind err %.2e\n", worstWorld,
                worstBindIdentity);

    // Corrupt inputs are rejected and leave the destination untouched.
    {
        std::FILE* f = std::fopen(path.c_str(), "r+b");
        expectTrue(f != nullptr, "reopen skeleton file");
        if (f != nullptr) {
            std::fputc('X', f);
            std::fclose(f);
        }
        anim::Skeleton probe = loaded;
        expectTrue(!probe.load(path.c_str()), "bad magic rejected");
        expectTrue(probe.bone_count == 40u, "failed load leaves skeleton untouched");
    }
    {
        anim::Skeleton cyclic = skel;
        cyclic.bones[3].parent_index = 7; // forward reference (cycle-capable) is not topological
        expectTrue(!cyclic.has_valid_hierarchy(), "forward parent reference detected");
        expectTrue(!cyclic.save(path.c_str()), "invalid hierarchy refuses to save");
    }
    {
        expectTrue(skel.save(path.c_str()), "re-save skeleton");
        std::filesystem::resize_file(path, std::filesystem::file_size(path) - 10);
        anim::Skeleton probe;
        expectTrue(!probe.load(path.c_str()), "truncated skeleton rejected");
    }
    std::filesystem::remove(path);

    // Clip binary round trip (clips previously had stub save/load that reported success).
    anim::AnimationClip clip{};
    std::strncpy(clip.name, "walk", sizeof(clip.name) - 1);
    clip.duration = 1.25f;
    clip.looping = false;
    anim::AnimationClip::BoneChannels ch{};
    ch.bone_index = 2;
    ch.position.times = {0.f, 0.5f, 1.25f};
    ch.position.values_vec3 = {v3(0, 1, 0), v3(1, 2, 3), v3(-1, 0.5, 2)};
    ch.rotation.times = {0.f, 1.25f};
    ch.rotation.values_quat = {q4(random_quat(rng)), q4(random_quat(rng))};
    clip.bone_channels.push_back(ch);
    const std::string clipPath = scratch_path("clip.facl");
    expectTrue(clip.save(clipPath.c_str()), "clip save succeeds");
    anim::AnimationClip clipLoaded{};
    expectTrue(clipLoaded.load(clipPath.c_str()), "clip load succeeds");
    expectTrue(std::strcmp(clipLoaded.name, "walk") == 0 && clipLoaded.duration == 1.25f && !clipLoaded.looping &&
                   clipLoaded.bone_channels.size() == 1u && clipLoaded.bone_channels[0].bone_index == 2u &&
                   clipLoaded.bone_channels[0].position.values_vec3.size() == 3u &&
                   clipLoaded.bone_channels[0].position.values_vec3[1].z == 3.f &&
                   clipLoaded.bone_channels[0].rotation.values_quat[1].w == ch.rotation.values_quat[1].w,
               "clip round trip preserves name, flags and keys");
    anim::AnimationClip missing{};
    expectTrue(!missing.load((clipPath + ".missing").c_str()), "clip load of missing file fails");
    std::filesystem::remove(clipPath);
}

// ---------------------------------------------------------------------------------------------
// Shared fixture: 4-bone chain with non-trivial bind rotations.
// ---------------------------------------------------------------------------------------------

struct ChainFixture {
    anim::Skeleton skel;
    std::vector<RefTRS> binds;
};

ChainFixture make_chain_fixture() {
    ChainFixture fx;
    fx.binds = {
        {{0.5, 0.0, -0.25}, qaxis({0, 0, 1}, 0.5), {1, 1, 1}},
        {{0.0, 1.0, 0.0}, qaxis({1, 0, 0}, 0.8), {1, 1, 1}},
        {{0.0, 0.8, 0.1}, qaxis({1, 1, 0}, -0.6), {1, 1, 1}},
        {{0.2, 0.6, 0.0}, qaxis({0, 1, 1}, 1.1), {1, 1, 1}},
    };
    const char* names[] = {"pelvis", "spine", "chest", "neck"};
    for (u32 i = 0; i < fx.binds.size(); ++i) {
        fx.skel.bones.push_back(make_bone(names[i], static_cast<s32>(i) - 1, fx.binds[i]));
    }
    fx.skel.bone_count = static_cast<u32>(fx.skel.bones.size());
    return fx;
}

/// Reference channel sampling: linear position/scale, geodesic slerp rotation, clamped ends.
template <typename T, typename Interp>
T ref_sample(const std::vector<f32>& times, const std::vector<T>& values, double t, Interp interp) {
    if (values.size() == 1 || t <= times.front()) {
        return values.front();
    }
    if (t >= times.back()) {
        return values.back();
    }
    size_t k = 0;
    while (k + 1 < times.size() && !(t < times[k + 1])) {
        ++k;
    }
    const double a = (t - times[k]) / (static_cast<double>(times[k + 1]) - times[k]);
    return interp(values[k], values[k + 1], a);
}

/// Reference local TRS for a bone channel composed onto its bind local: bind * T_k * R_k * S_k.
RefTRS ref_channel_local(const RefTRS& bind, const anim::AnimationClip::BoneChannels& ch, double t) {
    std::vector<D3> pos, scl;
    std::vector<DQ> rot;
    for (const auto& v : ch.position.values_vec3) pos.push_back(d3(v));
    for (const auto& v : ch.scale.values_vec3) scl.push_back(d3(v));
    for (const auto& q : ch.rotation.values_quat) rot.push_back(dq(q));
    const D3 p = pos.empty() ? D3{} : ref_sample(ch.position.times, pos, t, lerp3);
    const DQ r = rot.empty() ? DQ{} : ref_sample(ch.rotation.times, rot, t, slerp);
    const D3 s = scl.empty() ? D3{1, 1, 1} : ref_sample(ch.scale.times, scl, t, lerp3);
    // bind (uniform unit scale) * TRS: translation tb + Rb p, rotation Rb*R, scale s.
    return {add(bind.t, qrot(bind.r, p)), qmul(bind.r, qnorm(r)), s};
}

// ---------------------------------------------------------------------------------------------
// Gate: ClipNode samples position and rotation channels within 0.001f of reference at all keyframes.
// ---------------------------------------------------------------------------------------------

void gateClipNodeKeyframes() {
    const ChainFixture fx = make_chain_fixture();
    std::mt19937 rng(0xC11Bu);
    std::uniform_real_distribution<double> off(-2.0, 2.0);
    std::uniform_real_distribution<double> scl(0.5, 1.8);

    anim::AnimationClip clip{};
    clip.duration = 1.2f;
    clip.looping = false;
    const std::vector<f32> keyTimes = {0.f, 0.13f, 0.4f, 0.41f, 0.9f, 1.2f};
    for (u32 bone = 0; bone < fx.skel.bone_count; ++bone) {
        anim::AnimationClip::BoneChannels ch{};
        ch.bone_index = bone;
        ch.position.times = keyTimes;
        ch.rotation.times = keyTimes;
        DQ prev = random_quat(rng);
        for (size_t k = 0; k < keyTimes.size(); ++k) {
            ch.position.values_vec3.push_back(v3(off(rng), off(rng), off(rng)));
            // Large inter-key arcs, with alternating hemisphere sign to exercise shortest-path.
            DQ q = qmul(qaxis(random_unit(rng), 2.6 * (k % 2 == 0 ? 1.0 : -0.7)), prev);
            if (k % 3 == 1) {
                q = {-q.x, -q.y, -q.z, -q.w};
            }
            ch.rotation.values_quat.push_back(q4(qnorm(q)));
            prev = q;
        }
        if (bone == 2) {
            // Non-uniform scale on one bone checks T*R*S ordering.
            ch.scale.times = keyTimes;
            for (size_t k = 0; k < keyTimes.size(); ++k) {
                ch.scale.values_vec3.push_back(v3(scl(rng), scl(rng), scl(rng)));
            }
        }
        clip.bone_channels.push_back(ch);
    }

    auto reference_world = [&](double t) {
        std::vector<RefTRS> locals = fx.binds;
        for (const auto& ch : clip.bone_channels) {
            locals[ch.bone_index] = ref_channel_local(fx.binds[ch.bone_index], ch, t);
        }
        return reference_fk(fx.skel, locals);
    };

    double worstKeyChannel = 0.0;
    double worstKeyWorld = 0.0;
    anim::ClipNode node;
    node.clip = &clip;
    node.looping = false;
    for (f32 key : keyTimes) {
        // Raw channel sampling at the key returns the key exactly.
        for (const auto& ch : clip.bone_channels) {
            for (size_t k = 0; k < keyTimes.size(); ++k) {
                if (keyTimes[k] != key) continue;
                const anim::vec3 p = ch.position.sample_vec3(key);
                worstKeyChannel = std::max(worstKeyChannel, len(sub(d3(p), d3(ch.position.values_vec3[k]))));
                const anim::quat q = ch.rotation.sample_quat(key);
                worstKeyChannel = std::max(worstKeyChannel, qangle(dq(q), dq(ch.rotation.values_quat[k])));
            }
        }

        const std::vector<DM> ref = reference_world(key);
        node.time = key;
        anim::PoseSoA soa;
        node.evaluate_soa(0.f, fx.skel, soa);
        node.time = key;
        anim::Pose aos;
        node.evaluate(0.f, fx.skel, aos);
        for (u32 i = 0; i < fx.skel.bone_count; ++i) {
            worstKeyWorld = std::max(worstKeyWorld, max_abs_diff(soa.bone_world_transforms[i], ref[i]));
            worstKeyWorld = std::max(worstKeyWorld, max_abs_diff(aos.bone_world_transforms[i], ref[i]));
        }
    }
    expectTrue(worstKeyChannel < 1e-3, "channel samples equal keys within 0.001 at every keyframe");
    expectTrue(worstKeyWorld < 1e-3, "ClipNode pose matches reference within 0.001 at every keyframe");

    // Between keys: linear position + geodesic slerp rotation, checked on a dense grid.
    double worstBetween = 0.0;
    for (int step = 0; step <= 240; ++step) {
        const double t = 1.2 * step / 240.0;
        const std::vector<DM> ref = reference_world(t);
        anim::PoseSoA soa;
        clip.evaluate(static_cast<f32>(t), fx.skel, soa);
        for (u32 i = 0; i < fx.skel.bone_count; ++i) {
            worstBetween = std::max(worstBetween, max_abs_diff(soa.bone_world_transforms[i], ref[i]));
        }
    }
    expectTrue(worstBetween < 2e-3, "clip interpolation between keys matches slerp/lerp reference");

    // Looping: ClipNode time advanced by dt wraps modulo duration.
    anim::AnimationClip loopClip = clip;
    loopClip.looping = true;
    anim::ClipNode loopNode;
    loopNode.clip = &loopClip;
    loopNode.looping = true;
    double worstLoop = 0.0;
    double elapsed = 0.0;
    const f32 dt = 0.07f;
    for (int frame = 0; frame < 60; ++frame) {
        elapsed += dt;
        anim::PoseSoA soa;
        loopNode.evaluate_soa(dt, fx.skel, soa);
        const double wrapped = std::fmod(static_cast<double>(loopNode.time), 1.2);
        const double expected = std::fmod(elapsed, 1.2);
        worstLoop = std::max(worstLoop, std::min(std::fabs(wrapped - expected), 1.2 - std::fabs(wrapped - expected)));
        const std::vector<DM> ref = reference_world(loopNode.time);
        for (u32 i = 0; i < fx.skel.bone_count; ++i) {
            worstLoop = std::max(worstLoop, max_abs_diff(soa.bone_world_transforms[i], ref[i]));
        }
    }
    expectTrue(worstLoop < 2e-3, "looping ClipNode wraps time and matches reference");
    std::printf("[gate] clip sampling: key channel err %.2e, key world err %.2e, between-key err %.2e, loop err %.2e\n",
                worstKeyChannel, worstKeyWorld, worstBetween, worstLoop);
}

// ---------------------------------------------------------------------------------------------
// Gate: BlendNode2 interpolates pose correctly at blend values 0.0, 0.5 and 1.0.
// ---------------------------------------------------------------------------------------------

struct ConstantPoseClip {
    anim::AnimationClip clip;
    std::vector<RefTRS> locals; // reference local TRS (bind composed)
};

ConstantPoseClip make_constant_clip(const ChainFixture& fx, std::mt19937& rng) {
    std::uniform_real_distribution<double> off(-0.5, 0.5);
    ConstantPoseClip out;
    out.clip.duration = 1.f;
    out.locals = fx.binds;
    for (u32 bone = 0; bone < fx.skel.bone_count; ++bone) {
        anim::AnimationClip::BoneChannels ch{};
        ch.bone_index = bone;
        ch.position.times = {0.f};
        ch.position.values_vec3 = {v3(off(rng), off(rng), off(rng))};
        ch.rotation.times = {0.f};
        ch.rotation.values_quat = {q4(qaxis(random_unit(rng), 1.4 + off(rng)))};
        out.clip.bone_channels.push_back(ch);
        out.locals[bone] = ref_channel_local(fx.binds[bone], ch, 0.0);
    }
    return out;
}

std::vector<DM> reference_blend(const ChainFixture& fx, const std::vector<RefTRS>& a, const std::vector<RefTRS>& b,
                                double w) {
    std::vector<RefTRS> blended(a.size());
    for (size_t i = 0; i < a.size(); ++i) {
        blended[i] = {lerp3(a[i].t, b[i].t, w), slerp(a[i].r, b[i].r, w), lerp3(a[i].s, b[i].s, w)};
    }
    return reference_fk(fx.skel, blended);
}

void gateBlendNode2() {
    const ChainFixture fx = make_chain_fixture();
    std::mt19937 rng(0xB1E2u);
    const ConstantPoseClip clipA = make_constant_clip(fx, rng);
    const ConstantPoseClip clipB = make_constant_clip(fx, rng);

    f32 blend = 0.f;
    anim::BlendNode2 node;
    auto a = std::make_unique<anim::ClipNode>();
    a->clip = &clipA.clip;
    auto b = std::make_unique<anim::ClipNode>();
    b->clip = &clipB.clip;
    node.a = std::move(a);
    node.b = std::move(b);
    node.blend_param = &blend;

    double worst[3] = {0, 0, 0};
    const f32 weights[3] = {0.f, 0.5f, 1.f};
    for (int wi = 0; wi < 3; ++wi) {
        blend = weights[wi];
        const std::vector<DM> ref = reference_blend(fx, clipA.locals, clipB.locals, weights[wi]);
        anim::PoseSoA soa;
        node.evaluate_soa(0.f, fx.skel, soa);
        anim::Pose aos;
        node.evaluate(0.f, fx.skel, aos);
        for (u32 i = 0; i < fx.skel.bone_count; ++i) {
            worst[wi] = std::max(worst[wi], max_abs_diff(soa.bone_world_transforms[i], ref[i]));
            worst[wi] = std::max(worst[wi], max_abs_diff(aos.bone_world_transforms[i], ref[i]));
        }
    }
    expectTrue(worst[0] < 1e-4, "BlendNode2 at 0.0 equals pose A");
    expectTrue(worst[1] < 1e-4, "BlendNode2 at 0.5 equals local slerp/lerp midpoint");
    expectTrue(worst[2] < 1e-4, "BlendNode2 at 1.0 equals pose B (rotations included)");
    std::printf("[gate] BlendNode2: err @0.0 %.2e, @0.5 %.2e, @1.0 %.2e\n", worst[0], worst[1], worst[2]);
}

// ---------------------------------------------------------------------------------------------
// Gate: AnimStateMachine transitions between two states — blend completes in specified duration.
// ---------------------------------------------------------------------------------------------

void gateStateMachineTransition() {
    const ChainFixture fx = make_chain_fixture();
    std::mt19937 rng(0x57A7u);
    const ConstantPoseClip idle = make_constant_clip(fx, rng);
    const ConstantPoseClip run = make_constant_clip(fx, rng);

    const double durations[] = {0.1, 0.2, 0.25, 0.3, 0.5, 1.0};
    const double dts[] = {1.0 / 30.0, 1.0 / 60.0, 1.0 / 144.0};
    int configs = 0;
    int exact = 0;
    double worstPose = 0.0;
    double worstTimingError = 0.0;

    for (double duration : durations) {
        for (double dtD : dts) {
            const f32 dt = static_cast<f32>(dtD);
            bool go = false;
            anim::AnimStateMachine sm;
            auto idleNode = std::make_unique<anim::ClipNode>();
            idleNode->clip = &idle.clip;
            auto runNode = std::make_unique<anim::ClipNode>();
            runNode->clip = &run.clip;
            sm.add_state("idle", std::move(idleNode));
            sm.add_state("run", std::move(runNode));
            u32 enters = 0;
            sm.states[1].on_enter = [&enters] { ++enters; };
            sm.add_transition("idle", "run", static_cast<f32>(duration), [&go] { return go; });

            anim::Pose pose;
            sm.evaluate(dt, fx.skel, pose); // settle in idle
            const std::vector<DM> idleRef = reference_fk(fx.skel, idle.locals);
            for (u32 i = 0; i < fx.skel.bone_count; ++i) {
                worstPose = std::max(worstPose, max_abs_diff(pose.bone_world_transforms[i], idleRef[i]));
            }

            go = true;
            // Expected frame count: first frame whose elapsed time (frames * dt) reaches duration.
            const double ratio = duration / static_cast<double>(dt);
            const int expectedFrames = static_cast<int>(std::ceil(ratio - 1e-4));
            int frames = 0;
            while (frames < 10000) {
                sm.evaluate(dt, fx.skel, pose);
                ++frames;
                const double alpha = std::min(1.0, frames * static_cast<double>(dt) / duration);
                const std::vector<DM> ref = reference_blend(fx, idle.locals, run.locals, alpha);
                for (u32 i = 0; i < fx.skel.bone_count; ++i) {
                    worstPose = std::max(worstPose, max_abs_diff(pose.bone_world_transforms[i], ref[i]));
                }
                if (!sm.is_transitioning) {
                    break;
                }
            }
            ++configs;
            const double completedAt = frames * static_cast<double>(dt);
            worstTimingError = std::max(worstTimingError, std::fabs(completedAt - expectedFrames * static_cast<double>(dt)));
            if (frames == expectedFrames && sm.active_state == 1u && enters == 1u) {
                ++exact;
            } else {
                std::fprintf(stderr, "  duration %.3f dt %.5f: completed after %d frames (expected %d), state %u\n",
                             duration, dtD, frames, expectedFrames, sm.active_state);
            }
        }
    }
    expectTrue(exact == configs, "crossfade completes on the frame elapsed time reaches blend duration");
    expectTrue(worstPose < 1e-4, "crossfade pose equals reference blend at alpha = elapsed / duration");
    std::printf("[gate] state machine: %d/%d duration/dt configs complete on schedule, pose err %.2e\n", exact,
                configs, worstPose);
}

// ---------------------------------------------------------------------------------------------
// Gate: FABRIK converges within tolerance in < 10 iterations for 95% of random target positions.
// ---------------------------------------------------------------------------------------------

struct RandomChain {
    anim::Skeleton skel;
    std::vector<double> lengths;
};

RandomChain make_random_chain(std::mt19937& rng, u32 bones) {
    std::uniform_real_distribution<double> segLen(0.4, 1.2);
    RandomChain rc;
    for (u32 i = 0; i < bones; ++i) {
        RefTRS bind;
        if (i > 0) {
            const double l = segLen(rng);
            rc.lengths.push_back(l);
            // Mildly bent rest pose (joints within ~35 degrees of straight).
            bind.t = {0.0, l, 0.0};
            bind.r = qaxis(random_unit(rng), std::uniform_real_distribution<double>(0.0, 0.6)(rng));
        } else {
            bind.t = {0.3, -0.2, 0.1};
            bind.r = random_quat(rng);
        }
        const std::string name = "j" + std::to_string(i);
        rc.skel.bones.push_back(make_bone(name.c_str(), static_cast<s32>(i) - 1, bind));
    }
    rc.skel.bone_count = bones;
    return rc;
}

void gateFabrik() {
    std::mt19937 rng(0xFAB1u);
    const int trials = 2000;
    int convergedFast = 0;
    int converged = 0;
    int plainFast = 0; // passes only, no reshape warm start (reported, not gated)
    double worstLengthDrift = 0.0;
    double worstRootDrift = 0.0;
    double worstRigidity = 0.0;
    u64 iterationSum = 0;

    for (int trial = 0; trial < trials; ++trial) {
        const u32 bones = 3 + static_cast<u32>(trial % 5); // 3..7 joints
        const RandomChain rc = make_random_chain(rng, bones);
        anim::Pose pose = anim::Pose::make_bind_pose(rc.skel);
        const anim::Pose bind = pose;

        // Target uniform over the reachable workspace: the ball of radius L (total chain length)
        // around the root, minus the inner hole of radius max(0, 2 * longest - L).
        const D3 rootPos = world_pos(bind.bone_world_transforms[0]);
        double total = 0.0, longest = 0.0;
        for (double l : rc.lengths) {
            total += l;
            longest = std::max(longest, l);
        }
        const double inner = std::max(0.0, 2.0 * longest - total);
        std::uniform_real_distribution<double> u01(0.0, 1.0);
        double radius = 0.0;
        do {
            radius = total * std::cbrt(u01(rng));
        } while (radius <= inner);
        const D3 p = add(rootPos, scale(random_unit(rng), radius));

        anim::FABRIKChain chain;
        for (u32 i = 0; i < bones; ++i) chain.bone_indices.push_back(i);
        chain.target = v3(p.x, p.y, p.z);
        chain.tolerance = 0.001f;
        chain.max_iterations = 10;
        anim::Pose plainPose = pose;
        anim::FABRIKChain plain = chain;
        plain.reshape_warm_start = false;
        (void)plain.solve(plainPose, rc.skel);
        if (plain.converged() && plain.last_iterations < 10) {
            ++plainFast;
        }

        expectTrue(chain.solve(pose, rc.skel), "FABRIK solve accepts valid chain");

        const double err = len(sub(world_pos(pose.bone_world_transforms[bones - 1]), p));
        iterationSum += chain.last_iterations;
        if (err <= 0.001 + 1e-6) {
            ++converged;
            if (chain.last_iterations < 10) {
                ++convergedFast;
            }
        }
        worstRootDrift = std::max(worstRootDrift, len(sub(world_pos(pose.bone_world_transforms[0]),
                                                          world_pos(bind.bone_world_transforms[0]))));
        for (u32 i = 1; i < bones; ++i) {
            const double l = len(sub(world_pos(pose.bone_world_transforms[i]), world_pos(pose.bone_world_transforms[i - 1])));
            worstLengthDrift = std::max(worstLengthDrift, std::fabs(l - rc.lengths[i - 1]));
            // Rigid consistency: child world = parent world * original bind local (FABRIK only rotates).
            const DM expectChild = dm_mul(from_mat4(pose.bone_world_transforms[i - 1]),
                                          from_mat4(rc.skel.bones[i].local_transform));
            worstRigidity = std::max(worstRigidity, len(sub(dm_translation(expectChild),
                                                            world_pos(pose.bone_world_transforms[i]))));
        }
    }

    // Unreachable target straightens the chain toward it (closest achievable configuration).
    {
        std::mt19937 rng2(7);
        const RandomChain rc = make_random_chain(rng2, 4);
        anim::Pose pose = anim::Pose::make_bind_pose(rc.skel);
        const D3 root = world_pos(pose.bone_world_transforms[0]);
        const double total = rc.lengths[0] + rc.lengths[1] + rc.lengths[2];
        const D3 far = add(root, scale(norm({1, 2, 0.5}), total * 3.0));
        anim::FABRIKChain chain;
        chain.bone_indices = {0, 1, 2, 3};
        chain.target = v3(far.x, far.y, far.z);
        expectTrue(chain.solve(pose, rc.skel), "FABRIK unreachable solve");
        const D3 expectedEnd = add(root, scale(norm(sub(far, root)), total));
        expectTrue(len(sub(world_pos(pose.bone_world_transforms[3]), expectedEnd)) < 1e-3,
                   "FABRIK unreachable target fully extends chain toward target");
        expectTrue(!chain.converged(), "FABRIK reports non-convergence for unreachable target");
    }

    const double pct = 100.0 * convergedFast / trials;
    expectTrue(pct >= 95.0, "FABRIK converges within tolerance in < 10 iterations for >= 95% of targets");
    expectTrue(worstRootDrift < 1e-5, "FABRIK keeps chain root pinned");
    expectTrue(worstLengthDrift < 1e-4, "FABRIK preserves segment lengths");
    expectTrue(worstRigidity < 1e-3, "FABRIK world matrices stay rigidly consistent with bind locals");
    std::printf("[gate] FABRIK: %.2f%% converged in <10 iters (%d/%d; %d within tol overall), mean iters %.2f, "
                "len drift %.2e, root drift %.2e, rigidity %.2e; passes-only (no warm start) %.2f%%\n",
                pct, convergedFast, trials, converged, static_cast<double>(iterationSum) / trials, worstLengthDrift,
                worstRootDrift, worstRigidity, 100.0 * plainFast / trials);
}

// ---------------------------------------------------------------------------------------------
// Gate: TwoBoneIK produces analytically correct limb pose — verified against reference solver.
// ---------------------------------------------------------------------------------------------

/// Reference solver: law of cosines in double. The mid joint lies at angle A from the root→target
/// axis, in the half-plane spanned by that axis and the pole's perpendicular component.
bool reference_two_bone(D3 root, double a, double b, D3 target, D3 pole, D3& outMid, D3& outEnd) {
    const D3 toTarget = sub(target, root);
    const double d = len(toTarget);
    if (d > a + b || d < std::fabs(a - b) || d < 1e-9) {
        return false;
    }
    const D3 axis = scale(toTarget, 1.0 / d);
    const D3 perpRaw = sub(pole, scale(axis, dot(pole, axis)));
    if (len(perpRaw) < 1e-6) {
        return false;
    }
    const D3 perp = norm(perpRaw);
    const double cosA = (a * a + d * d - b * b) / (2.0 * a * d);
    const double sinA = std::sqrt(std::max(0.0, 1.0 - cosA * cosA));
    outMid = add(root, add(scale(axis, a * cosA), scale(perp, a * sinA)));
    outEnd = target;
    return true;
}

void gateTwoBoneIK() {
    std::mt19937 rng(0x2B0Eu);
    std::uniform_real_distribution<double> segLen(0.3, 1.5);
    std::uniform_real_distribution<double> u01(0.0, 1.0);
    const int trials = 1000;
    int solved = 0;
    double worstMid = 0.0;
    double worstEnd = 0.0;
    double worstSoA = 0.0;
    double worstRigid = 0.0;

    for (int trial = 0; trial < trials; ++trial) {
        const double a = segLen(rng);
        const double b = segLen(rng);
        anim::Skeleton skel;
        skel.bones.push_back(make_bone("hip", -1, {{0.4, 1.0, -0.3}, random_quat(rng), {1, 1, 1}}));
        skel.bones.push_back(make_bone("knee", 0, {scale(random_unit(rng), a), random_quat(rng), {1, 1, 1}}));
        skel.bones.push_back(make_bone("ankle", 1, {scale(random_unit(rng), b), random_quat(rng), {1, 1, 1}}));
        skel.bones.push_back(make_bone("toe", 2, {{0.0, 0.0, 0.2}, DQ{}, {1, 1, 1}}));
        skel.bone_count = 4;

        anim::Pose pose = anim::Pose::make_bind_pose(skel);
        const D3 root = world_pos(pose.bone_world_transforms[0]);

        // Target strictly inside the reachable shell |a-b| < d < a+b.
        const double lo = std::fabs(a - b) + 0.02 * (a + b);
        const double hi = (a + b) * 0.98;
        const double dist = lo + (hi - lo) * u01(rng);
        const D3 target = add(root, scale(random_unit(rng), dist));
        const D3 pole = random_unit(rng);

        D3 refMid, refEnd;
        if (!reference_two_bone(root, a, b, target, pole, refMid, refEnd)) {
            continue;
        }

        anim::TwoBoneIK ik;
        ik.root_bone = 0;
        ik.mid_bone = 1;
        ik.end_bone = 2;
        ik.target = v3(target.x, target.y, target.z);
        ik.pole_vector = v3(pole.x, pole.y, pole.z);
        if (!ik.solve(pose, skel)) {
            continue;
        }
        ++solved;
        worstMid = std::max(worstMid, len(sub(world_pos(pose.bone_world_transforms[1]), refMid)));
        worstEnd = std::max(worstEnd, len(sub(world_pos(pose.bone_world_transforms[2]), refEnd)));
        // Descendant (toe) rides rigidly with the ankle.
        const DM toe = dm_mul(from_mat4(pose.bone_world_transforms[2]), from_mat4(skel.bones[3].local_transform));
        worstRigid = std::max(worstRigid, max_abs_diff(pose.bone_world_transforms[3], toe));

        anim::PoseSoA soa = anim::PoseSoA::from_bind_pose(skel);
        if (ik.solve(soa, skel)) {
            worstSoA = std::max(worstSoA, len(sub(world_pos(soa.bone_world_transforms[1]), refMid)));
            worstSoA = std::max(worstSoA, len(sub(world_pos(soa.bone_world_transforms[2]), refEnd)));
            // SoA solve only rewrites rotations: child local offsets keep the limb lengths.
            worstSoA = std::max(worstSoA, len(sub(d3(soa.local_positions[1]),
                                                  dm_translation(from_mat4(skel.bones[1].local_transform)))));
        } else {
            worstSoA = 1e9;
        }
    }

    expectTrue(solved == trials, "TwoBoneIK solves every reachable random limb");
    expectTrue(worstMid < 1e-3, "TwoBoneIK mid joint matches law-of-cosines reference");
    expectTrue(worstEnd < 1e-3, "TwoBoneIK end effector reaches target");
    expectTrue(worstSoA < 1e-3, "TwoBoneIK SoA path matches reference");
    expectTrue(worstRigid < 1e-4, "TwoBoneIK carries descendants rigidly");
    std::printf("[gate] TwoBoneIK: %d/%d solved, mid err %.2e, end err %.2e, SoA err %.2e, child rigidity %.2e\n",
                solved, trials, worstMid, worstEnd, worstSoA, worstRigid);
}

// ---------------------------------------------------------------------------------------------
// Supporting: CPU reference LBS vs brute force (the GPU/CUDA budget row is hardware-only).
// ---------------------------------------------------------------------------------------------

void supportCpuSkinningReference() {
    std::mt19937 rng(0x5C1Eu);
    std::uniform_real_distribution<double> off(-1.0, 1.0);
    std::vector<DM> bones;
    anim::SkinningInput input;
    for (int i = 0; i < 16; ++i) {
        bones.push_back(dm_trs({off(rng), off(rng), off(rng)}, random_quat(rng), {1, 1, 1}));
        input.bone_transforms.push_back(to_mat4(bones.back()));
    }
    const u32 vertexCount = 2048;
    for (u32 v = 0; v < vertexCount; ++v) {
        input.rest_positions.push_back(v3(off(rng), off(rng), off(rng)));
        const D3 n = random_unit(rng);
        input.rest_normals.push_back(v3(n.x, n.y, n.z));
        anim::SkinningWeights w{};
        double sum = 0;
        double raw[4];
        for (int k = 0; k < 4; ++k) {
            raw[k] = std::uniform_real_distribution<double>(0.0, 1.0)(rng);
            sum += raw[k];
            w.bone_indices[k] = std::uniform_int_distribution<u32>(0, 15)(rng);
        }
        for (int k = 0; k < 4; ++k) w.bone_weights[k] = static_cast<f32>(raw[k] / sum);
        input.weights.push_back(w);
    }
    anim::SkinningOutput out;
    expectTrue(anim::skin_vertices(input, out), "CPU skinning runs");
    double worstPos = 0, worstNormal = 0;
    for (u32 v = 0; v < vertexCount; ++v) {
        D3 pos{}, nrm{};
        for (int k = 0; k < 4; ++k) {
            const double w = input.weights[v].bone_weights[k];
            const DM& m = bones[input.weights[v].bone_indices[k]];
            pos = add(pos, scale(dm_point(m, d3(input.rest_positions[v])), w));
            DM lin = m;
            lin.m[12] = lin.m[13] = lin.m[14] = 0;
            nrm = add(nrm, scale(dm_point(lin, d3(input.rest_normals[v])), w));
        }
        worstPos = std::max(worstPos, len(sub(d3(out.positions[v]), pos)));
        worstNormal = std::max(worstNormal, len(sub(d3(out.normals[v]), norm(nrm))));
    }
    expectTrue(worstPos < 1e-4, "CPU LBS positions match brute-force reference");
    expectTrue(worstNormal < 1e-4, "CPU LBS normals match renormalized reference");

    // Mismatched normal array is rejected instead of read out of bounds.
    anim::SkinningInput bad = input;
    bad.rest_normals.resize(3);
    expectTrue(!anim::skin_vertices(bad, out), "skinning rejects short normal array");
    std::printf("[gate] CPU LBS reference (%u verts): pos err %.2e, normal err %.2e\n", vertexCount, worstPos,
                worstNormal);
}

// ---------------------------------------------------------------------------------------------
// Gate: Animator updates pose and uploads bone buffer every frame without memory leak.
// ---------------------------------------------------------------------------------------------

void gateAnimatorFrameLoop() {
    const ChainFixture fx = make_chain_fixture();
    std::mt19937 rng(0xA417u);
    anim::Skeleton skel = fx.skel;
    const std::vector<DM> bindWorld = reference_fk(skel, fx.binds);
    for (u32 i = 0; i < skel.bone_count; ++i) {
        skel.bones[i].inverse_bind = anim::mat4_inverse_affine(to_mat4(bindWorld[i]));
    }

    anim::AnimationClip clip{};
    clip.duration = 2.f;
    clip.looping = true;
    for (u32 bone = 0; bone < skel.bone_count; ++bone) {
        anim::AnimationClip::BoneChannels ch{};
        ch.bone_index = bone;
        ch.rotation.times = {0.f, 1.f, 2.f};
        const DQ q0 = random_quat(rng);
        ch.rotation.values_quat = {q4(q0), q4(random_quat(rng)), q4(q0)};
        clip.bone_channels.push_back(ch);
    }

    anim::Animator animator;
    animator.state_machine = std::make_unique<anim::AnimStateMachine>();
    auto node = std::make_unique<anim::ClipNode>();
    node->clip = &clip;
    animator.state_machine->add_state("loop", std::move(node));

    fuse::frame::FrameCtx ctx{};
    ctx.dt = 1.f / 60.f;
    const int warmup = 30;
    const int frames = 1200;
    long long liveAfterWarmup = 0;
    const anim::mat4* paletteData = nullptr;
    bool paletteStable = true;
    bool palettesCorrect = true;
    bool posesAdvance = true;
    double worstPalette = 0.0;
    double worstPose = 0.0;
    double clipTime = 0.0;
    anim::mat4 previousPalette1{};

    for (int frame = 0; frame < warmup + frames; ++frame) {
        animator.tick(skel, ctx);
        clipTime = std::fmod(clipTime + static_cast<double>(ctx.dt), 2.0);
        if (frame == warmup - 1) {
            liveAfterWarmup = g_liveAllocations.load();
            paletteData = animator.bone_palette.data();
        }
        if (frame >= warmup) {
            paletteStable = paletteStable && animator.bone_palette.data() == paletteData &&
                            animator.bone_palette.size() == skel.bone_count;
        }
        // Pose equals direct clip evaluation at the accumulated time; palette = world * inverse bind.
        std::vector<RefTRS> locals = fx.binds;
        for (const auto& ch : clip.bone_channels) {
            locals[ch.bone_index] = ref_channel_local(fx.binds[ch.bone_index], ch, clipTime);
        }
        const std::vector<DM> ref = reference_fk(skel, locals);
        for (u32 i = 0; i < skel.bone_count && i < animator.current_pose.bone_world_transforms.size(); ++i) {
            worstPose = std::max(worstPose, max_abs_diff(animator.current_pose.bone_world_transforms[i], ref[i]));
            const DM expectPalette = dm_mul(ref[i], from_mat4(skel.bones[i].inverse_bind));
            if (i < animator.bone_palette.size()) {
                worstPalette = std::max(worstPalette, max_abs_diff(animator.bone_palette[i], expectPalette));
            } else {
                palettesCorrect = false;
            }
        }
        if (frame > 0 && animator.bone_palette.size() > 1 &&
            std::memcmp(previousPalette1.data.data(), animator.bone_palette[1].data.data(), sizeof(f32) * 16) == 0) {
            posesAdvance = false;
        }
        if (animator.bone_palette.size() > 1) {
            previousPalette1 = animator.bone_palette[1];
        }
    }
    const long long liveAfterRun = g_liveAllocations.load();

    expectTrue(animator.tick_count == static_cast<u32>(warmup + frames), "Animator ticks every frame");
    expectTrue(posesAdvance, "Animator bone buffer changes every frame while the clip plays");
    expectTrue(palettesCorrect && worstPalette < 1e-3, "Animator bone buffer = world * inverse bind each frame");
    expectTrue(worstPose < 1e-3, "Animator pose tracks clip reference each frame");
    expectTrue(paletteStable, "Animator bone buffer storage is reused (no per-frame reallocation)");
    expectTrue(liveAfterRun == liveAfterWarmup, "Animator frame loop leaves no live allocations behind");

    // Paused animators skip evaluation and leave the bone buffer untouched.
    animator.paused = true;
    const std::vector<anim::mat4> frozen = animator.bone_palette;
    animator.tick(skel, ctx);
    expectTrue(animator.tick_count == static_cast<u32>(warmup + frames) &&
                   std::memcmp(frozen.data(), animator.bone_palette.data(), frozen.size() * sizeof(anim::mat4)) == 0,
               "paused Animator does not tick");

    std::printf("[gate] Animator: %d frames, pose err %.2e, palette err %.2e, live allocs delta %lld\n", frames,
                worstPose, worstPalette, liveAfterRun - liveAfterWarmup);
}

// ---------------------------------------------------------------------------------------------
// Supporting: TRS decomposition regression (rotations near 180 degrees were returned as identity).
// ---------------------------------------------------------------------------------------------

void supportDecomposeTrs() {
    std::mt19937 rng(0xDEC0u);
    std::uniform_real_distribution<double> off(-3.0, 3.0);
    std::uniform_real_distribution<double> scl(0.3, 2.5);
    double worstRot = 0, worstScale = 0, worstTrans = 0, worstInverse = 0;
    for (int i = 0; i < 5000; ++i) {
        DQ q = random_quat(rng);
        if (i % 4 == 0) {
            q = qaxis(random_unit(rng), 3.14159265358979 - 1e-3 * (i % 7)); // trace < 0 regime
        }
        const D3 t{off(rng), off(rng), off(rng)};
        const D3 s{scl(rng), scl(rng), scl(rng)};
        const DM m = dm_trs(t, q, s);
        anim::vec3 outT, outS;
        anim::quat outR;
        anim::decompose_trs(to_mat4(m), outT, outR, outS);
        worstRot = std::max(worstRot, qangle(dq(outR), q));
        worstScale = std::max(worstScale, len(sub(d3(outS), s)));
        worstTrans = std::max(worstTrans, len(sub(d3(outT), t)));
        const DM prod = dm_mul(m, from_mat4(anim::mat4_inverse_affine(to_mat4(m))));
        worstInverse = std::max(worstInverse, max_abs_diff(anim::mat4::identity(), prod));
    }
    expectTrue(worstRot < 2e-3, "decompose_trs recovers rotation (incl. near-180-degree)");
    expectTrue(worstScale < 1e-4 && worstTrans < 1e-5, "decompose_trs recovers non-uniform scale/translation");
    expectTrue(worstInverse < 1e-4, "mat4_inverse_affine is a true inverse");

    // Slerp vs geodesic reference, including antipodal-sign inputs and the near-parallel branch.
    double worstSlerp = 0.0;
    for (int i = 0; i < 5000; ++i) {
        const DQ a = random_quat(rng);
        DQ b = i % 5 == 0 ? qnorm(qmul(qaxis(random_unit(rng), 1e-3), a)) : random_quat(rng);
        if (i % 2 == 0) b = {-b.x, -b.y, -b.z, -b.w};
        const double t = std::uniform_real_distribution<double>(0.0, 1.0)(rng);
        const anim::quat r = anim::lerp(q4(a), q4(b), static_cast<f32>(t));
        const double l = std::sqrt(double(r.x) * r.x + double(r.y) * r.y + double(r.z) * r.z + double(r.w) * r.w);
        worstSlerp = std::max(worstSlerp, std::max(qangle(dq(r), slerp(a, b, t)), std::fabs(l - 1.0)));
    }
    expectTrue(worstSlerp < 2e-3, "quaternion slerp matches geodesic reference and stays unit length");
    std::printf("[gate] TRS decompose: rot %.2e rad, scale %.2e, inverse %.2e; slerp err %.2e\n", worstRot,
                worstScale, worstInverse, worstSlerp);
}

} // namespace

int main() {
    gateSkeletonBinaryLoad();
    gateClipNodeKeyframes();
    gateBlendNode2();
    gateStateMachineTransition();
    gateFabrik();
    gateTwoBoneIK();
    supportCpuSkinningReference();
    gateAnimatorFrameLoop();
    supportDecomposeTrs();

    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_b7_animation_gates: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("fuse_b7_animation_gates: all gates passed\n");
    return EXIT_SUCCESS;
}
