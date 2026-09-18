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

} // namespace fuse::cinematics
