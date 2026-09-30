#pragma once
// E03 (RE-FI-1 / RE-RUNTIME-3D-RENDER / UNI-U4-1): what World3D hands its GPU renderer. World3D stays renderer-agnostic
// (it links no RHI): the Hybrid layer implements IWorld3DRenderer with the E02 SceneRenderer, and World3D::render calls
// it with the world's ECS registry (Transform + Mesh + lights) and camera. Without a renderer attached (no Vulkan
// device, stub backend) World3D::render records nothing and the PlaceholderRenderer fallback draws the frame.
#include <fuse/types.hpp>

namespace fuse::frame {
struct FrameCtx;
}

namespace fuse::world3d {

class World3D;

/// Engine mesh ids (ecs::Mesh::vertex_buffer.index()) the runtime renderer pre-registers as procedural meshes, so
/// scenes without cooked assets render. Unit sized: the cube spans [-1, 1]^3, the sphere has radius 1, the plane is
/// the 2 x 2 xz quad at y = 0 (normal +y). Scale them with the entity transform.
enum class BuiltinMesh : u32 {
    Cube = 0,
    Sphere = 1,
    Plane = 2,
};
inline constexpr u32 kBuiltinMeshCount = 3u;

/// Camera of the world (eye, look-at target, vertical field of view in radians). When `valid` is false the renderer
/// uses the registry's active ecs::Camera instead (scene_renderer::cameraFromRegistry).
struct RenderCamera3D {
    f32 eye[3] = {0.f, 1.5f, 4.f};
    f32 target[3] = {0.f, 0.5f, 0.f};
    f32 fovY = 1.f;
    f32 nearPlane = 0.1f;
    f32 farPlane = 200.f;
    bool valid = false;
};

/// A material row of the world (ecs::Mesh::material_id indexes it). Linear base colour, metallic / roughness,
/// emissive colour x intensity.
struct RenderMaterial3D {
    f32 baseColor[3] = {0.8f, 0.8f, 0.8f};
    f32 metallic = 0.f;
    f32 roughness = 0.5f;
    f32 emissive[3] = {0.f, 0.f, 0.f};
    f32 emissiveIntensity = 0.f;
};

/// GPU renderer of a World3D (implemented by fuse::hybrid::HybridSceneRenderer). renderWorld3D runs on the render
/// thread inside World3D::render and records the world's 3D frame (it does not submit: the frame compositor submits
/// 3D + sprites + UI together).
class IWorld3DRenderer {
public:
    virtual ~IWorld3DRenderer() = default;
    virtual bool renderWorld3D(World3D& world, frame::FrameCtx& ctx) = 0;
};

} // namespace fuse::world3d
