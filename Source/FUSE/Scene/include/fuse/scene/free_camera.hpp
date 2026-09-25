#pragma once

#include <fuse/ecs/entity.hpp>
#include <fuse/ecs/math/vec.hpp>
#include <fuse/ecs/registry.hpp>
#include <fuse/types.hpp>

namespace fuse::scene {

/// One frame of free-camera ("fly") input. Axes are in [-1, 1]; look deltas are in degrees.
struct FreeCameraInput {
    f32 moveForward = 0.f; ///< +1 along the view direction
    f32 moveRight = 0.f;   ///< +1 along the camera's right vector
    f32 moveUp = 0.f;      ///< +1 along world +Y
    f32 yawDeltaDeg = 0.f;   ///< + turns towards +X (from a +Z view)
    f32 pitchDeltaDeg = 0.f; ///< + looks up
    bool boost = false;      ///< multiplies speed by `boostMultiplier`
};

/// Free-fly camera controller (B3.8) driving an ECS camera entity's Transform. Yaw is about
/// world +Y, pitch about the camera's right axis (clamped short of the poles so the look-at
/// basis stays defined). Identity orientation looks down +Z, matching CameraSystem, which reads
/// the view direction from the Transform's local-to-world Z column.
class FreeCameraController {
public:
    f32 moveSpeed = 10.f;       ///< world units per second
    f32 boostMultiplier = 4.f;
    f32 maxPitchDeg = 89.f;

    /// Adopts the entity's current position (orientation resets to the given yaw/pitch).
    void attach(fuse::ecs::Registry& registry, fuse::ecs::EntityID camera, f32 yawDeg = 0.f, f32 pitchDeg = 0.f);

    /// Applies look then move for `dt` seconds and writes position + rotation to the Transform
    /// (marked dirty). Returns false when the camera entity is dead or has no Transform.
    bool update(fuse::ecs::Registry& registry, const FreeCameraInput& input, f32 dt);

    /// Teleport / aim (e.g. to follow a scripted path). Writes the Transform like `update`.
    bool setPose(fuse::ecs::Registry& registry, fuse::ecs::vec3 position, f32 yawDeg, f32 pitchDeg);

    [[nodiscard]] fuse::ecs::EntityID camera() const { return m_camera; }
    [[nodiscard]] fuse::ecs::vec3 position() const { return m_position; }
    [[nodiscard]] f32 yawDeg() const { return m_yawDeg; }
    [[nodiscard]] f32 pitchDeg() const { return m_pitchDeg; }
    /// Unit view direction for the current yaw/pitch.
    [[nodiscard]] fuse::ecs::vec3 forward() const;
    /// Unit screen-right vector (horizontal; `forward x worldUp`, as CameraSystem's look_at).
    [[nodiscard]] fuse::ecs::vec3 right() const;
    /// Rotation quaternion written to the Transform (yaw * pitch).
    [[nodiscard]] fuse::ecs::quat orientation() const;

private:
    bool writeTransform_(fuse::ecs::Registry& registry) const;

    fuse::ecs::EntityID m_camera{};
    fuse::ecs::vec3 m_position{0.f, 0.f, 0.f, 1.f};
    f32 m_yawDeg = 0.f;
    f32 m_pitchDeg = 0.f;
};

} // namespace fuse::scene
