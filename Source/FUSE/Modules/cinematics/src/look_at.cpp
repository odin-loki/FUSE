#include <fuse/cinematics/look_at.hpp>

#include <cmath>

namespace fuse::cinematics {

namespace {

constexpr float kLookAtCoincidentEpsilon = 1e-6f;

} // namespace

bool look_at_resolver_available(const LookAtResolver* resolver) {
    return resolver != nullptr && resolver->can_resolve();
}

bool look_at_resolver_can_resolve_target(const LookAtResolver& resolver, const std::string& target_id) {
    Vec3 out{};
    return resolver.try_resolve(target_id, out);
}

Vec3 fallback_camera_look_at(const CameraKeyframe& keyframe,
                             const Vec3& camera_position,
                             float default_look_distance) {
    if (camera_look_distance(camera_position, keyframe.look_at) <= kLookAtCoincidentEpsilon) {
        return default_camera_look_at_for_position(camera_position, default_look_distance);
    }
    return keyframe.look_at;
}

Vec3 resolve_look_at_world_with_fallback(const CameraKeyframe& keyframe,
                                         const LookAtResolver& resolver,
                                         const Vec3& camera_position,
                                         float default_look_distance) {
    const Vec3 resolved = resolve_look_at_world(keyframe, resolver);
    if (camera_look_distance(camera_position, resolved) <= kLookAtCoincidentEpsilon) {
        return default_camera_look_at_for_position(camera_position, default_look_distance);
    }
    return resolved;
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

} // namespace fuse::cinematics
