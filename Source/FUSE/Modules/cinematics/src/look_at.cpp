#include <fuse/cinematics/look_at.hpp>

namespace fuse::cinematics {

Vec3 resolve_look_at_world(const CameraKeyframe& keyframe, const LookAtResolver& resolver) {
    if (keyframe.look_at_mode == CameraLookAtMode::TargetEntity && !keyframe.look_at_target_id.empty()
        && resolver.can_resolve()) {
        return resolver.resolve(keyframe.look_at_target_id);
    }

    return keyframe.look_at;
}

} // namespace fuse::cinematics
