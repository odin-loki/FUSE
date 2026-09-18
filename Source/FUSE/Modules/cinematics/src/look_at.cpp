#include <fuse/cinematics/look_at.hpp>

namespace fuse::cinematics {

bool LookAtResolver::has_target(const std::string& target_id) const {
    return look_at_resolver_has_target(*this, target_id);
}

bool look_at_resolver_has_target(const LookAtResolver& resolver, const std::string& target_id) {
    if (target_id.empty() || !resolver.can_resolve()) {
        return false;
    }

    Vec3 out{};
    return resolver.try_resolve(target_id, out);
}

Vec3 resolve_look_at_world(const CameraKeyframe& keyframe, const LookAtResolver& resolver) {
    if (keyframe.look_at_mode == CameraLookAtMode::TargetEntity && !keyframe.look_at_target_id.empty()
        && resolver.can_resolve()) {
        Vec3 resolved{};
        if (resolver.try_resolve(keyframe.look_at_target_id, resolved)) {
            return resolved;
        }
    }

    return keyframe.look_at;
}

Vec3 resolve_look_at_world_or_default(const CameraKeyframe& keyframe,
                                      const LookAtResolver& resolver,
                                      float default_distance) {
    if (keyframe.look_at_mode == CameraLookAtMode::TargetEntity && !keyframe.look_at_target_id.empty()
        && resolver.can_resolve()) {
        const Vec3 fallback = camera_keyframe_look_at_unset(keyframe)
                                  ? default_camera_look_at_for_position(keyframe.position, default_distance)
                                  : keyframe.look_at;
        return resolver.resolve_or(keyframe.look_at_target_id, fallback);
    }

    if (camera_keyframe_look_at_unset(keyframe)) {
        return default_camera_look_at_for_position(keyframe.position, default_distance);
    }

    return keyframe.look_at;
}

} // namespace fuse::cinematics
