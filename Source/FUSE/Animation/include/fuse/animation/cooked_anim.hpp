#pragma once

// Asset plan §5.1 / 4b: the cooked animation clip `.fuseanim` (writer used by Tools/FUSE/Cook anim_cook, reader used
// at runtime). Dependency-free (no assimp): the cook imports through assimp into a CookedAnim (float samples) and
// calls encode_cooked_anim; the runtime reads the bytes back with read_cooked_anim and turns them into an
// AnimationClip with cooked_anim_to_clip.
//
// Byte layout (little endian, every section 4-byte aligned):
//
//   u32 magic "FANM" | u32 version (2; 1 still read) | u32 flags (bit 0 looping, bit 1 root motion)
//   u32 frame_count  | f32 sample_rate (Hz) | f32 duration (s) | u64 skeleton_hash
//   u32 joint_count  | u32 event_count | str clip name
//   joint_count x { str name | s32 parent (earlier joint or -1) | u8 t_mode, r_mode, s_mode, 0
//                   | translation track | rotation track | scale track }      (in that order, absent = 0 bytes)
//       vec3 track  mode 1 constant : f32 x, y, z
//                   mode 2 quantised: f32 min[3], f32 max[3], frame_count x unorm16 x 3 (padded to 4)
//       quat track  mode 1 constant : f32 x, y, z, w (normalised)
//                   mode 2 quantised: frame_count x u64 smallest-three: bits 0-1 index of the dropped (largest)
//                                     component (made positive), bits 2-21 / 22-41 / 42-61 the other three in
//                                     index order as unorm20 over [-1/sqrt2, 1/sqrt2], bits 62-63 zero
//   root motion (flag bit 1): u32 root joint | frame_count x f32 dx, dy, dz, dyaw (cumulative from frame 0, in the
//                             root's parent space, Y-up; yaw = rotation of +Z about +Y, unwrapped)
//   event_count x { f32 time | str name }   (sorted by time, then name; 0 <= time <= duration)
//   u64 FNV-1a 64 of every preceding byte
//
//   str = u16 byte length + UTF-8 bytes, zero padded to 4.
//
// Frames are uniform: frame i is at min(i / sample_rate, duration); frame_count = ceil(duration * rate) + 1 (1 for a
// zero-length clip). Tracks hold the joint's *local* TRS as authored (absolute, not relative to the bind pose).
// skeleton_hash (v2) = FNV-1a 64 over, per joint in file order, the name bytes, a 0 byte and the parent index as s32
// little endian (skeleton_hash_of_joints): the same identity fuse_assetcheck's `skin` gate computes for an FSKL rig
// (asset_rules.hpp skeletonHash), so a clip and a rig can be matched by hash. v1 files (names only,
// skeleton_hash_of_names) still read; their hash is checked with the v1 rule and restated with the v2 one.
// Output depends only on the CookedAnim contents (no paths, time or host).

#include <fuse/animation/clip.hpp>
#include <fuse/animation/skeleton.hpp>
#include <fuse/types.hpp>

#include <string>
#include <vector>

namespace fuse::animation {

inline constexpr u32 kCookedAnimMagic = 0x4D4E4146u; ///< "FANM" little endian
inline constexpr u32 kCookedAnimVersion = 2u;   ///< written (skeleton hash over names + parents)
inline constexpr u32 kCookedAnimVersionV1 = 1u; ///< still read (skeleton hash over names only)
inline constexpr u32 kCookedAnimFlagLooping = 1u << 0;
inline constexpr u32 kCookedAnimFlagRootMotion = 1u << 1;

enum class CookedTrackMode : u8 { Absent = 0, Constant = 1, Quantized = 2 };

struct CookedAnimJoint {
    std::string name;
    s32 parent = -1;
    /// Per-frame samples (frame_count entries) or empty when the channel is not animated. The encoder stores a track
    /// whose samples are all equal as Constant; the reader expands Constant tracks back to frame_count samples.
    std::vector<vec3> translation;
    std::vector<quat> rotation;
    std::vector<vec3> scale;
    /// Modes as read (informational; the encoder decides from the samples).
    CookedTrackMode translation_mode = CookedTrackMode::Absent;
    CookedTrackMode rotation_mode = CookedTrackMode::Absent;
    CookedTrackMode scale_mode = CookedTrackMode::Absent;
};

struct CookedAnimEvent {
    std::string name;
    f32 time = 0.f;
};

struct CookedAnimRootSample {
    f32 dx = 0.f, dy = 0.f, dz = 0.f, dyaw = 0.f;
};

struct CookedAnim {
    std::string name;
    f32 sample_rate = 30.f;
    f32 duration = 0.f;
    u32 frame_count = 1;
    bool looping = true;
    u64 skeleton_hash = 0; ///< filled by encode / read (skeleton_hash_of_joints of the joints)
    std::vector<CookedAnimJoint> joints;
    std::vector<CookedAnimEvent> events;
    bool has_root_motion = false;
    u32 root_joint = 0;
    std::vector<CookedAnimRootSample> root_motion; ///< frame_count samples when has_root_motion

    [[nodiscard]] f32 frame_time(u32 frame) const;
};

/// v1 rule: FNV-1a 64 over the names in order, each followed by a 0 byte.
[[nodiscard]] u64 skeleton_hash_of_names(const std::vector<std::string>& names);
/// v2 rule (written): FNV-1a 64 over, per joint, the name bytes, a 0 byte and the parent as s32 little endian
/// (a missing parent counts as -1). Equal to fuse_assetcheck's skeletonHash(names, parents).
[[nodiscard]] u64 skeleton_hash_of_joints(const std::vector<std::string>& names, const std::vector<s32>& parents);
[[nodiscard]] u64 skeleton_hash_of(const CookedAnim& anim);
[[nodiscard]] u64 skeleton_hash_of(const Skeleton& skeleton);

/// Frame count for a duration at a rate (ceil(duration * rate - 1e-4) + 1, at least 1).
[[nodiscard]] u32 cooked_anim_frame_count(f32 duration, f32 sample_rate);

/// Validate (frame counts, parents, finite values, event order) and serialize. Empty result + `error` on failure.
std::vector<u8> encode_cooked_anim(const CookedAnim& anim, std::string* error = nullptr);

/// Parse + validate `.fuseanim` bytes. On failure `out` is reset and `error` says why.
bool read_cooked_anim(const u8* data, usize size, CookedAnim& out, std::string* error = nullptr);
bool read_cooked_anim_file(const std::string& path, CookedAnim& out, std::string* error = nullptr);

/// Skeleton of the clip's joints (names, parents) with identity bind / inverse bind matrices: clips converted
/// without a skeleton evaluate against it.
[[nodiscard]] Skeleton cooked_anim_skeleton(const CookedAnim& anim);

/// CookedAnim -> AnimationClip (one key per frame).
///   skeleton == nullptr: bone_index = joint index (pair with cooked_anim_skeleton); values as stored.
///   skeleton != nullptr: joints map to bones by name (unmatched joints are skipped and counted in `unmatched`);
///   because AnimationClip::evaluate composes bind_local * channel, each frame is rebased to
///   inverse(bind_local) * TRS(stored) so the evaluated local transform equals the cooked one. Channels a joint does
///   not animate take the bind value.
/// Events and root motion stay on the CookedAnim (AnimationClip has no slots for them).
bool cooked_anim_to_clip(const CookedAnim& anim, const Skeleton* skeleton, AnimationClip& out,
                         std::string* error = nullptr, u32* unmatched = nullptr);

/// read_cooked_anim_file + cooked_anim_to_clip.
bool load_cooked_anim_clip(const std::string& path, const Skeleton* skeleton, AnimationClip& out,
                           CookedAnim* anim_out = nullptr, std::string* error = nullptr);

} // namespace fuse::animation
