#include <fuse/cinematics/look_at.hpp>

namespace fuse::cinematics {

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
        return resolver.resolve_or(keyframe.look_at_target_id,
                                   camera_keyframe_look_at_fallback(keyframe, default_distance));
    }

    return camera_keyframe_look_at_fallback(keyframe, default_distance);
}

bool look_at_resolver_has_target(const LookAtResolver& resolver, const std::string& target_id) {
    if (!resolver.can_resolve() || target_id.empty()) {
        return false;
    }

    Vec3 out{};
    return resolver.try_resolve(target_id, out);
}

} // namespace fuse::cinematics
