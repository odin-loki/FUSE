#include <fuse/cinematics/look_at.hpp>

#include <cmath>
#include <unordered_set>

namespace fuse::cinematics {

namespace {

constexpr float kLookAtCoincidentEpsilon = 1e-6f;

} // namespace

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

Vec3 resolve_look_at_or_fallback(const CameraKeyframe& keyframe,
                                 const LookAtResolver& resolver,
                                 const Vec3& fallback) {
    if (keyframe.look_at_mode == CameraLookAtMode::TargetEntity && !keyframe.look_at_target_id.empty()
        && resolver.can_resolve()) {
        return resolver.resolve_or(keyframe.look_at_target_id, fallback);
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

std::vector<std::string> collect_camera_look_at_target_ids(
    const std::vector<CameraKeyframe>& keyframes) {
    std::unordered_set<std::string> seen;
    std::vector<std::string> ids;
    ids.reserve(keyframes.size());

    for (const CameraKeyframe& keyframe : keyframes) {
        if (!camera_keyframe_uses_entity_look_at(keyframe)) {
            continue;
        }

        if (seen.insert(keyframe.look_at_target_id).second) {
            ids.push_back(keyframe.look_at_target_id);
        }
    }

    return ids;
}

std::size_t camera_keyframe_entity_look_at_count(const std::vector<CameraKeyframe>& keyframes) {
    std::size_t count = 0;
    for (const CameraKeyframe& keyframe : keyframes) {
        if (camera_keyframe_uses_entity_look_at(keyframe)) {
            ++count;
        }
    }
    return count;
}

} // namespace fuse::cinematics
