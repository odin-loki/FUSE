#pragma once

// Asset plan §5.1 / 4b: animation clip cook. assimp (FBX / glTF / COLLADA / ...) aiAnimation -> uniform resample ->
// quantised `.fuseanim` (format: Source/FUSE/Animation/include/fuse/animation/cooked_anim.hpp).
//
// Import: the joints are the nodes the chosen aiAnimation animates, in depth-first pre-order of the scene's node tree
// (channels whose node is not in the tree follow in channel order); a joint's parent is its nearest animated
// ancestor. Each channel is sampled at frame i = min(i / sample_rate, duration) with linear (vectors) / slerp
// (rotations) interpolation between the source keys and clamping outside them (assimp's own key semantics), giving
// absolute local TRS. Root motion (optional) records the root joint's translation and yaw (rotation of +Z about +Y)
// relative to frame 0; the root track itself is kept as authored. Strict like the mesh cook: unsorted key times,
// non-finite values, duplicate channels or a missing clip fail; nothing is repaired.

#include <fuse/animation/cooked_anim.hpp>
#include <fuse/cook/cook_stub_writer.hpp>
#include <fuse/types.hpp>

#include <string>
#include <vector>

namespace fuse::cook {

struct AnimCookEvent {
    std::string name;
    f32 time = 0.f; ///< seconds, clamped to nothing: outside [0, duration] fails the cook
};

struct AnimCookOptions {
    f32 sample_rate = 30.f;
    u32 clip_index = 0;      ///< aiScene::mAnimations index (ignored when clip_name is set)
    std::string clip_name;   ///< select the clip by name
    bool looping = true;
    bool root_motion = true;
    std::string root_joint;  ///< empty: the first joint (a root of the joint hierarchy)
    std::vector<AnimCookEvent> events; ///< sorted by (time, name) by the cook
};

/// Number of animation clips in a source (0 when it has none or cannot be read / assimp is absent).
[[nodiscard]] u32 count_anim_clips(const std::string& input_path);

/// Import + resample into `out` (float samples, not yet quantised). Failure classes like import_mesh_file:
/// ImporterUnavailable, MalformedSource (unreadable, no such clip, bad keys), InvalidArgument (bad options).
bool import_anim_file(const std::string& input_path, const AnimCookOptions& options, animation::CookedAnim& out,
                      std::string* error = nullptr, CookFailure* failure = nullptr);

/// Import + encode_cooked_anim + write. Deterministic: same source + options -> same bytes.
CookStubWriteResult cook_anim_file(const std::string& input_path, const std::string& output_path,
                                   const AnimCookOptions& options = {});

} // namespace fuse::cook
