#include <fuse/ecs/components/camera.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/systems/camera_system.hpp>
#include <fuse/ecs/systems/culling_system.hpp>

namespace fuse::ecs {

void CameraSystem::update(Registry& reg) {
    reg.each<Camera, Transform>([&](EntityID, Camera& camera, Transform& transform) {
        if (!camera.is_active) {
            return;
        }

        const vec3 eye = {transform.local_to_world.data[12], transform.local_to_world.data[13],
                          transform.local_to_world.data[14], 1.f};
        const vec3 forward = {transform.local_to_world.data[8], transform.local_to_world.data[9],
                            transform.local_to_world.data[10], 0.f};
        const vec3 target = {eye.x + forward.x, eye.y + forward.y, eye.z + forward.z, 1.f};
        const vec3 up = {0.f, 1.f, 0.f, 0.f};

        camera.projection = perspective(camera.fov_deg, camera.aspect_ratio, camera.near_plane,
                                        camera.far_plane);
        camera.view = look_at(eye, target, up);
        camera.view_projection = camera.projection * camera.view;
        camera.frustum = CullingSystem::extract_frustum(camera.view_projection);
    });
}

} // namespace fuse::ecs
