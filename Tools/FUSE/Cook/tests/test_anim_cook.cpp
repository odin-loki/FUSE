// fuse_asset_anim_cook (asset plan §5.1 `.fuseanim`, task 4b). CPU gate, labels asset;gate;cook.
//
//   import     the glTF fixture (anim_fixture.hpp) through assimp: joints root -> arm (non-animated Armature
//              skipped), 31 frames at 30 Hz, every sample equal to the analytic curve (|dT| <= 1e-5, angle <= 1e-5 rad)
//   round trip cook -> read_cooked_anim: positions / scales within half a unorm16 step of the track range
//              (+1e-6), rotations within 1e-5 rad (smallest-three 20-bit), constant tracks exact, events sorted,
//              root motion = (3, 0, 1.5, pi/2) at the end, skeleton hash = hash of ("root", "arm" <- root), the
//              same value as fuse_assetcheck's skin-gate rig hash
//   clip       cooked_anim_to_clip without and with a skeleton (non-identity bind pose + an extra bone): world
//              matrices of root / arm at frame and mid-frame times match the analytic pose within 2e-4
//   determinism re-cooking (same and different output path) gives byte-identical files
//   refusals   corrupt trailer / truncated bytes / bad magic; no-animation source and unreadable source
//              (MalformedSource); event outside the clip (InvalidArgument)
// Exit 77 (skip) when assimp is not linked.

#include "anim_fixture.hpp"

#include <fuse/animation/cooked_anim.hpp>
#include <fuse/animation/math.hpp>
#include <fuse/assetcheck/asset_rules.hpp>
#include <fuse/cook/anim_cook.hpp>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

namespace an = fuse::animation;
namespace fs = std::filesystem;
using fuse::f32;
using fuse::u32;
using fuse::u8;

int g_failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        ++g_failures;
    }
}

std::vector<u8> read_bytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<u8>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

an::quat axis_quat(int axis, double degrees) {
    const double h = degrees * anim_fixture::kPi / 360.0;
    an::quat q{0.f, 0.f, 0.f, static_cast<f32>(std::cos(h))};
    (&q.x)[axis] = static_cast<f32>(std::sin(h));
    return q;
}

/// Rotation angle between two unit quaternions, from r = conj(a) * b in double (atan2 form: acos(|dot|) loses all
/// precision near 1 for float inputs).
double quat_angle(const an::quat& a, const an::quat& b) {
    const double ax = -a.x, ay = -a.y, az = -a.z, aw = a.w;
    const double rw = aw * b.w - ax * b.x - ay * b.y - az * b.z;
    const double rx = aw * b.x + ax * b.w + ay * b.z - az * b.y;
    const double ry = aw * b.y - ax * b.z + ay * b.w + az * b.x;
    const double rz = aw * b.z + ax * b.y - ay * b.x + az * b.w;
    return 2.0 * std::atan2(std::sqrt(rx * rx + ry * ry + rz * rz), std::fabs(rw));
}

double max_diff(const an::vec3& a, const an::vec3& b) {
    return std::fmax(std::fabs(a.x - b.x), std::fmax(std::fabs(a.y - b.y), std::fabs(a.z - b.z)));
}

/// Analytic world matrices (root, arm) at time t.
void analytic_world(double t, an::mat4& root, an::mat4& arm) {
    const anim_fixture::Expected e = anim_fixture::expected(t);
    const an::vec3 rt{static_cast<f32>(e.rootT[0]), static_cast<f32>(e.rootT[1]), static_cast<f32>(e.rootT[2]), 0.f};
    root = an::mat4_from_trs(rt, axis_quat(1, e.rootYawDeg), an::vec3{1.f, 1.f, 1.f, 0.f});
    const f32 s = static_cast<f32>(e.armScale);
    const an::mat4 armLocal = an::mat4_from_trs(an::vec3{0.f, 1.f, 0.f, 0.f}, axis_quat(0, e.armXDeg), an::vec3{s, s, s, 0.f});
    arm = an::mat4_multiply(root, armLocal);
}

double mat_diff(const an::mat4& a, const an::mat4& b) {
    double m = 0.0;
    for (u32 i = 0; i < 16; ++i) {
        m = std::fmax(m, std::fabs(static_cast<double>(a.data[i]) - b.data[i]));
    }
    return m;
}

void check_clip_pose(const an::AnimationClip& clip, const an::Skeleton& skel, int rootBone, int armBone,
                     const std::string& label) {
    double worst = 0.0;
    for (u32 i = 0; i <= 60; ++i) {
        const double t = i / 60.0; // frame and mid-frame times of the 30 Hz clip
        an::PoseSoA pose = an::PoseSoA::from_bind_pose(skel);
        clip.evaluate(static_cast<f32>(t) * 0.999999f, skel, pose);
        an::mat4 root, arm;
        analytic_world(t * 0.999999, root, arm);
        worst = std::fmax(worst, mat_diff(pose.bone_world_transforms[static_cast<size_t>(rootBone)], root));
        worst = std::fmax(worst, mat_diff(pose.bone_world_transforms[static_cast<size_t>(armBone)], arm));
    }
    std::printf("%s: max world-matrix error %.3g\n", label.c_str(), worst);
    check(worst <= 2e-4, label + ": world matrices within 2e-4 of the analytic pose");
}

} // namespace

int main() {
    const fs::path scratch = fs::temp_directory_path() / "fuse_asset_anim_cook";
    std::error_code ec;
    fs::remove_all(scratch, ec);
    fs::create_directories(scratch, ec);
    const std::string dir = scratch.generic_string();
    const std::string gltf = anim_fixture::write_gltf(dir, "walk");

    if (fuse::cook::count_anim_clips(gltf) == 0u) {
        an::CookedAnim probe;
        fuse::cook::CookFailure failure = fuse::cook::CookFailure::None;
        std::string error;
        (void)fuse::cook::import_anim_file(gltf, {}, probe, &error, &failure);
        if (failure == fuse::cook::CookFailure::ImporterUnavailable) {
            std::printf("SKIP: assimp not linked (%s)\n", error.c_str());
            return 77;
        }
        check(false, "fixture has one clip (" + error + ")");
        return 1;
    }
    check(fuse::cook::count_anim_clips(gltf) == 1u, "fixture has one clip");

    fuse::cook::AnimCookOptions options;
    options.events = {{"footstep_r", 0.75f}, {"footstep_l", 0.25f}};

    // --- import against the analytic curve ------------------------------------------------------------------
    an::CookedAnim imported;
    std::string error;
    fuse::cook::CookFailure failure = fuse::cook::CookFailure::None;
    check(fuse::cook::import_anim_file(gltf, options, imported, &error, &failure), "import: " + error);
    check(imported.name == "walk", "clip name");
    check(imported.joints.size() == 2u && imported.joints[0].name == "root" && imported.joints[1].name == "arm",
          "joints root, arm in hierarchy order (non-animated Armature skipped)");
    if (imported.joints.size() != 2u) {
        return 1;
    }
    check(imported.joints[0].parent == -1 && imported.joints[1].parent == 0, "arm's parent is root");
    check(std::fabs(imported.duration - 1.f) < 1e-6f && imported.frame_count == 31u, "1 s, 31 frames at 30 Hz");
    // assimp's glTF importer gives every channel all three key lists (unkeyed paths hold the node's rest TRS), so the
    // root's scale is a constant (1, 1, 1) track rather than absent.
    check(imported.joints[1].translation.size() == 31u &&
              (imported.joints[0].scale.empty() || imported.joints[0].scale.size() == 31u),
          "track presence");
    double importT = 0.0, importR = 0.0;
    for (u32 f = 0; f < imported.frame_count; ++f) {
        const anim_fixture::Expected e = anim_fixture::expected(imported.frame_time(f));
        const an::vec3 rt{static_cast<f32>(e.rootT[0]), static_cast<f32>(e.rootT[1]), static_cast<f32>(e.rootT[2]), 0.f};
        importT = std::fmax(importT, max_diff(imported.joints[0].translation[f], rt));
        importT = std::fmax(importT, max_diff(imported.joints[1].translation[f], an::vec3{0.f, 1.f, 0.f, 0.f}));
        const f32 s = static_cast<f32>(e.armScale);
        importT = std::fmax(importT, max_diff(imported.joints[1].scale[f], an::vec3{s, s, s, 0.f}));
        importR = std::fmax(importR, quat_angle(imported.joints[0].rotation[f], axis_quat(1, e.rootYawDeg)));
        importR = std::fmax(importR, quat_angle(imported.joints[1].rotation[f], axis_quat(0, e.armXDeg)));
    }
    std::printf("import vs analytic: translation/scale %.3g, rotation %.3g rad\n", importT, importR);
    check(importT <= 1e-5 && importR <= 1e-5, "imported samples match the analytic curve");

    // --- cook + read back --------------------------------------------------------------------------------------
    const std::string out1 = dir + "/walk.fuseanim";
    const std::string out2 = dir + "/again/walk_copy.fuseanim";
    const fuse::cook::CookStubWriteResult cooked = fuse::cook::cook_anim_file(gltf, out1, options);
    check(cooked.ok, "cook: " + cooked.note);
    std::printf("%s (%u bytes)\n", cooked.note.c_str(), cooked.byteCount);
    const std::vector<u8> bytes1 = read_bytes(out1);
    check(fuse::cook::cook_anim_file(gltf, out1, options).ok && read_bytes(out1) == bytes1,
          "re-cook to the same path is byte-identical");
    check(fuse::cook::cook_anim_file(gltf, out2, options).ok && read_bytes(out2) == bytes1,
          "re-cook to another path is byte-identical (no paths in the output)");

    an::CookedAnim loaded;
    check(an::read_cooked_anim_file(out1, loaded, &error), "read .fuseanim: " + error);
    if (loaded.joints.size() != 2u) {
        return 1;
    }
    check(loaded.skeleton_hash == an::skeleton_hash_of_joints({"root", "arm"}, {-1, 0}),
          "skeleton hash of (root, arm <- root)");
    // One identity for clips and rigs: the .fuseanim hash is fuse_assetcheck's skin-gate rig hash.
    check(loaded.skeleton_hash == fuse::assetcheck::skeletonHash({"root", "arm"}, {-1, 0}),
          "skeleton hash equals the assetcheck skin-gate hash (names + parents)");
    check(loaded.joints[1].translation_mode == an::CookedTrackMode::Constant &&
              loaded.joints[0].translation_mode == an::CookedTrackMode::Quantized &&
              loaded.joints[0].rotation_mode == an::CookedTrackMode::Quantized &&
              loaded.joints[0].scale_mode != an::CookedTrackMode::Quantized,
          "track modes (constant arm translation, quantised root tracks, root scale absent or constant)");
    double errT = 0.0, errR = 0.0;
    bool withinStep = true;
    for (u32 j = 0; j < 2; ++j) {
        const auto& src = imported.joints[j];
        const auto& dst = loaded.joints[j];
        for (const auto* pair : {&src.translation, &src.scale}) {
            const std::vector<an::vec3>& a = *pair;
            const std::vector<an::vec3>& b = pair == &src.translation ? dst.translation : dst.scale;
            if (a.empty()) continue;
            f32 lo[3] = {a[0].x, a[0].y, a[0].z}, hi[3] = {lo[0], lo[1], lo[2]};
            for (const an::vec3& v : a) {
                const f32 c[3] = {v.x, v.y, v.z};
                for (u32 i = 0; i < 3; ++i) { lo[i] = std::fmin(lo[i], c[i]); hi[i] = std::fmax(hi[i], c[i]); }
            }
            for (size_t f = 0; f < a.size(); ++f) {
                const f32 ca[3] = {a[f].x, a[f].y, a[f].z}, cb[3] = {b[f].x, b[f].y, b[f].z};
                for (u32 i = 0; i < 3; ++i) {
                    const double d = std::fabs(static_cast<double>(ca[i]) - cb[i]);
                    errT = std::fmax(errT, d);
                    if (d > 0.5 * (hi[i] - lo[i]) / 65535.0 + 1e-6) withinStep = false;
                }
            }
        }
        for (size_t f = 0; f < src.rotation.size(); ++f) {
            errR = std::fmax(errR, quat_angle(src.rotation[f], dst.rotation[f]));
        }
    }
    std::printf("round trip: translation/scale %.3g, rotation %.3g rad\n", errT, errR);
    check(withinStep, "quantised vec3 tracks within half a unorm16 step of their range");
    check(errR <= 1e-5, "rotations within 1e-5 rad");
    check(loaded.joints[1].translation[7].y == 1.f, "constant track exact");
    check(loaded.events.size() == 2u && loaded.events[0].name == "footstep_l" && loaded.events[1].name == "footstep_r" &&
              loaded.events[0].time == 0.25f,
          "events sorted by time");
    check(loaded.has_root_motion && loaded.root_joint == 0u && loaded.root_motion.size() == 31u, "root motion present");
    if (loaded.root_motion.size() == 31u) {
        const an::CookedAnimRootSample& last = loaded.root_motion.back();
        check(std::fabs(last.dx - 3.f) < 1e-5f && std::fabs(last.dy) < 1e-5f && std::fabs(last.dz - 1.5f) < 1e-5f &&
                  std::fabs(last.dyaw - static_cast<f32>(anim_fixture::kPi / 2.0)) < 1e-4f,
              "root motion at the end = (3, 0, 1.5) and pi/2 yaw");
        check(loaded.root_motion.front().dx == 0.f && loaded.root_motion.front().dyaw == 0.f, "root motion starts at 0");
    }

    // --- runtime clip ------------------------------------------------------------------------------------------
    an::AnimationClip clip;
    check(an::cooked_anim_to_clip(loaded, nullptr, clip, &error), "clip without skeleton: " + error);
    const an::Skeleton own = an::cooked_anim_skeleton(loaded);
    check(own.has_valid_hierarchy() && an::skeleton_hash_of(own) == loaded.skeleton_hash, "clip skeleton + hash");
    check_clip_pose(clip, own, 0, 1, "clip (own skeleton)");

    an::Skeleton rig;
    rig.bones.resize(3);
    std::snprintf(rig.bones[0].name, sizeof(rig.bones[0].name), "extra");
    std::snprintf(rig.bones[1].name, sizeof(rig.bones[1].name), "root");
    std::snprintf(rig.bones[2].name, sizeof(rig.bones[2].name), "arm");
    rig.bones[2].parent_index = 1;
    rig.bones[2].local_transform =
        an::mat4_from_trs(an::vec3{0.2f, 1.f, 0.f, 0.f}, axis_quat(2, 10.0), an::vec3{1.f, 1.f, 1.f, 0.f});
    rig.bone_count = 3;
    an::AnimationClip rigClip;
    u32 unmatched = 99;
    check(an::cooked_anim_to_clip(loaded, &rig, rigClip, &error, &unmatched) && unmatched == 0u,
          "clip onto a rig with a bind pose: " + error);
    check_clip_pose(rigClip, rig, 1, 2, "clip (rig, rebased bind pose)");
    an::AnimationClip viaFile;
    check(an::load_cooked_anim_clip(out1, &rig, viaFile, nullptr, &error) &&
              viaFile.bone_channels.size() == rigClip.bone_channels.size(),
          "load_cooked_anim_clip");

    // --- refusals ----------------------------------------------------------------------------------------------
    {
        std::vector<u8> bad = bytes1;
        bad[bad.size() / 2] ^= 0x5Au;
        an::CookedAnim tmp;
        check(!an::read_cooked_anim(bad.data(), bad.size(), tmp, &error), "flipped byte refused");
        check(!an::read_cooked_anim(bytes1.data(), bytes1.size() - 9u, tmp, &error), "truncated refused");
        bad = bytes1;
        bad[0] = 'X';
        check(!an::read_cooked_anim(bad.data(), bad.size(), tmp, &error), "bad magic refused");
    }
    {
        const std::string obj = dir + "/tri.obj";
        std::ofstream(obj) << "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
        an::CookedAnim tmp;
        check(!fuse::cook::import_anim_file(obj, {}, tmp, &error, &failure) &&
                  failure == fuse::cook::CookFailure::MalformedSource,
              "mesh without a clip -> MalformedSource");
        const std::string junk = dir + "/junk.gltf";
        std::ofstream(junk) << "{ this is not gltf";
        check(!fuse::cook::import_anim_file(junk, {}, tmp, &error, &failure) &&
                  failure == fuse::cook::CookFailure::MalformedSource,
              "unreadable source -> MalformedSource");
        fuse::cook::AnimCookOptions late = options;
        late.events.push_back({"too_late", 2.f});
        check(!fuse::cook::import_anim_file(gltf, late, tmp, &error, &failure) &&
                  failure == fuse::cook::CookFailure::InvalidArgument,
              "event outside the clip -> InvalidArgument");
        fuse::cook::AnimCookOptions badClip;
        badClip.clip_name = "run";
        check(!fuse::cook::import_anim_file(gltf, badClip, tmp, &error, &failure), "missing clip name refused");
    }

    fs::remove_all(scratch, ec);
    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_asset_anim_cook: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("fuse_asset_anim_cook: OK\n");
    return 0;
}
