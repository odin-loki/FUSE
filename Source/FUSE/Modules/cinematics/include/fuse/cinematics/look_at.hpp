#pragma once

// Ore: Engine/source/T3D/camera.cpp (setTrackObject / look-at modes)
//      Verve VCameraTrack scene-object binding (entity target without Torque bridge)

#include <fuse/cinematics/camera_track.hpp>

#include <functional>
#include <string>

namespace fuse::cinematics {

/// Stub resolver for entity-bound look-at (game thread provides world positions).
class LookAtResolver {
public:
    using ResolveFn = std::function<Vec3(const std::string& target_id)>;
    using TryResolveFn = std::function<bool(const std::string& target_id, Vec3& out)>;

    void set_resolve_fn(ResolveFn fn) {
        resolve_fn_ = std::move(fn);
        try_resolve_fn_ = nullptr;
    }

    void set_try_resolve_fn(TryResolveFn fn) {
        try_resolve_fn_ = std::move(fn);
        resolve_fn_ = nullptr;
    }

    bool can_resolve() const { return static_cast<bool>(resolve_fn_ || try_resolve_fn_); }

    Vec3 resolve(const std::string& target_id) const {
        Vec3 out{};
        if (try_resolve(target_id, out)) {
            return out;
        }
        return {};
    }

    /// Returns true when the target was found. Legacy `resolve_fn` always succeeds.
    bool try_resolve(const std::string& target_id, Vec3& out) const {
        if (try_resolve_fn_) {
            return try_resolve_fn_(target_id, out);
        }
        if (resolve_fn_) {
            out = resolve_fn_(target_id);
            return true;
        }
        return false;
    }

    /// Resolve entity world position, or return `fallback` when lookup fails.
    Vec3 resolve_or(const std::string& target_id, const Vec3& fallback) const {
        Vec3 out{};
        if (try_resolve(target_id, out)) {
            return out;
        }
        return fallback;
    }

private:
    ResolveFn resolve_fn_;
    TryResolveFn try_resolve_fn_;
};

/// Resolve a keyframe's look-at to world space (fixed point or entity stub).
/// Falls back to `keyframe.look_at` when entity mode is unset or the resolver cannot resolve.
Vec3 resolve_look_at_world(const CameraKeyframe& keyframe, const LookAtResolver& resolver);

/// Resolve entity look-at, or return `fallback` when entity mode is inactive or unresolved.
Vec3 resolve_look_at_or_fallback(const CameraKeyframe& keyframe,
                                 const LookAtResolver& resolver,
                                 const Vec3& fallback);

/// Unique non-empty entity ids referenced by `keyframes` (editor resolver wiring stub).
std::vector<std::string> collect_camera_look_at_target_ids(
    const std::vector<CameraKeyframe>& keyframes);

/// Count keyframes that bind look-at to an entity id.
std::size_t camera_keyframe_entity_look_at_count(const std::vector<CameraKeyframe>& keyframes);

} // namespace fuse::cinematics
