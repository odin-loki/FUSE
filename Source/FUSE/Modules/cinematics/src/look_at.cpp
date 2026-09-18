#include <fuse/cinematics/look_at.hpp>

#include <unordered_set>

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

Vec3 resolve_look_at_or_fallback(const CameraKeyframe& keyframe,
                                 const LookAtResolver& resolver,
                                 const Vec3& fallback) {
    if (keyframe.look_at_mode == CameraLookAtMode::TargetEntity && !keyframe.look_at_target_id.empty()
        && resolver.can_resolve()) {
        return resolver.resolve_or(keyframe.look_at_target_id, fallback);
    }

    return keyframe.look_at;
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
