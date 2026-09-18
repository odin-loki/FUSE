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

private:
    ResolveFn resolve_fn_;
    TryResolveFn try_resolve_fn_;
};

/// Resolve a keyframe's look-at to world space (fixed point or entity stub).
/// Falls back to `keyframe.look_at` when entity mode is unset or the resolver cannot resolve.
Vec3 resolve_look_at_world(const CameraKeyframe& keyframe, const LookAtResolver& resolver);

/// True when `resolver` is non-null and has a resolve callback installed.
bool look_at_resolver_available(const LookAtResolver* resolver);

/// Probe whether `resolver` can resolve `target_id` without mutating world state.
bool look_at_resolver_can_resolve_target(const LookAtResolver& resolver, const std::string& target_id);

/// Use `keyframe.look_at` when distinct from `camera_position`; otherwise offset along -Z.
Vec3 fallback_camera_look_at(const CameraKeyframe& keyframe,
                              const Vec3& camera_position,
                              float default_look_distance = 10.f);

/// `resolve_look_at_world` plus coincident-position fallback via `fallback_camera_look_at`.
Vec3 resolve_look_at_world_with_fallback(const CameraKeyframe& keyframe,
                                         const LookAtResolver& resolver,
                                         const Vec3& camera_position,
                                         float default_look_distance = 10.f);

} // namespace fuse::cinematics
