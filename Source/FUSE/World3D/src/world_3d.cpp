#include <fuse/world3d/world_3d.hpp>

#include <fuse/ecs/components/light.hpp>
#include <fuse/ecs/components/mesh.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/log/logger.hpp>
#include <fuse/platform/thread.hpp>

#include <cmath>

namespace fuse::world3d {

World3D::World3D() : m_root(std::make_unique<SceneObject3D>("World3DRoot")) {
    m_physics.init();
}

World3D::~World3D() {
    if (m_registryReady) {
        m_registry.destroy();
    }
}

ecs::Registry& World3D::registry() {
    if (m_externalRegistry != nullptr) {
        return *m_externalRegistry;
    }
    if (!m_registryReady) {
        m_registry.init(kRegistryCapacity);
        m_registryReady = true;
    }
    return m_registry;
}

void World3D::setMaterial(u32 id, const RenderMaterial3D& material) {
    if (id >= m_materials.size()) {
        m_materials.resize(static_cast<usize>(id) + 1u);
    }
    m_materials[id] = material;
    ++m_materialVersion;
}

namespace {

void setTranslation(ecs::Transform& t, const f32 p[3]) {
    t.position = ecs::vec3{p[0], p[1], p[2], 1.f};
    t.local_to_world.data[12] = p[0];
    t.local_to_world.data[13] = p[1];
    t.local_to_world.data[14] = p[2];
}

} // namespace

ecs::EntityID World3D::spawnMesh(BuiltinMesh mesh, u32 materialId, const f32 center[3], const f32 halfExtent[3]) {
    ecs::Registry& r = registry();
    const ecs::EntityID e = r.create();
    ecs::Transform t{};
    t.scale = ecs::vec3{halfExtent[0], halfExtent[1], halfExtent[2], 0.f};
    t.local_to_world = ecs::mat4::identity();
    t.local_to_world.data[0] = halfExtent[0];
    t.local_to_world.data[5] = halfExtent[1];
    t.local_to_world.data[10] = halfExtent[2];
    setTranslation(t, center);
    r.add<ecs::Transform>(e, t);
    ecs::Mesh m{};
    m.vertex_buffer = ecs::MeshVertexBufferHandle(static_cast<u32>(mesh), 1);
    m.material_id = materialId;
    // Unit builtin meshes: the plane is flat in y (its AABB keeps a zero-height slab).
    const bool plane = mesh == BuiltinMesh::Plane;
    m.aabb_min = ecs::vec3{-1.f, plane ? 0.f : -1.f, -1.f, 0.f};
    m.aabb_max = ecs::vec3{1.f, plane ? 0.f : 1.f, 1.f, 0.f};
    r.add<ecs::Mesh>(e, m);
    return e;
}

ecs::EntityID World3D::spawnDirectionalLight(const f32 toSun[3], const f32 color[3], f32 intensity) {
    ecs::Registry& r = registry();
    const ecs::EntityID e = r.create();
    // Lights shine down their local -Z: +Z (third column) = the direction toward the sun.
    f32 z[3] = {toSun[0], toSun[1], toSun[2]};
    const f32 len = std::sqrt(z[0] * z[0] + z[1] * z[1] + z[2] * z[2]);
    if (len > 0.f) {
        for (f32& v : z) {
            v /= len;
        }
    } else {
        z[0] = 0.f;
        z[1] = 1.f;
        z[2] = 0.f;
    }
    // Any orthonormal x / y completing the frame.
    f32 x[3] = {z[2], 0.f, -z[0]};
    f32 xl = std::sqrt(x[0] * x[0] + x[2] * x[2]);
    if (xl < 1e-4f) {
        x[0] = 1.f;
        x[1] = 0.f;
        x[2] = 0.f;
        xl = 1.f;
    }
    x[0] /= xl;
    x[2] /= xl;
    const f32 y[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]};
    ecs::Transform t{};
    t.local_to_world = ecs::mat4::identity();
    for (u32 a = 0; a < 3u; ++a) {
        t.local_to_world.data[0 + a] = x[a];
        t.local_to_world.data[4 + a] = y[a];
        t.local_to_world.data[8 + a] = z[a];
    }
    const f32 origin[3] = {0.f, 10.f, 0.f};
    setTranslation(t, origin);
    r.add<ecs::Transform>(e, t);
    ecs::DirectionalLight d{};
    d.color = ecs::vec3{color[0], color[1], color[2], 0.f};
    d.intensity = intensity;
    r.add<ecs::DirectionalLight>(e, d);
    return e;
}

ecs::EntityID World3D::spawnPointLight(const f32 position[3], const f32 color[3], f32 intensity, f32 radius) {
    ecs::Registry& r = registry();
    const ecs::EntityID e = r.create();
    ecs::Transform t{};
    setTranslation(t, position);
    r.add<ecs::Transform>(e, t);
    ecs::PointLight p{};
    p.color = ecs::vec3{color[0], color[1], color[2], 0.f};
    p.intensity = intensity;
    p.radius = radius;
    r.add<ecs::PointLight>(e, p);
    return e;
}

void World3D::setClearColor(float r, float g, float b) {
    m_clearR = r;
    m_clearG = g;
    m_clearB = b;
}

void World3D::loadWorld(dimension::WorldHandle world) {
    m_activeWorld = world;
    log::info("World3D: load world handle index=%u gen=%u", world.index(), world.generation());
}

void World3D::addObject(SceneObject3D* object) {
    if (object == nullptr) {
        return;
    }
    m_objects.push_back(object);
    if (m_root) {
        m_root->addChild(object);
    }
    if (m_physicsEnabled) {
        const u32 bodyIndex = m_physics.addSphereBody(object->x(), object->y(), object->z(), 0.5f, 1.f);
        m_physicsBodyIndices.push_back(bodyIndex);
    }
}

void World3D::clearDynamicObjects() {
    if (m_root) {
        for (SceneObject3D* object : m_objects) {
            m_root->removeChild(object);
        }
    }
    m_objects.clear();
    m_physicsBodyIndices.clear();
    m_snapshot.clear();
    m_transformSoA.clear();
    m_cullVisible.clear();
}

void World3D::syncPhysicsFromScene() {
    for (usize i = 0; i < m_objects.size() && i < m_physicsBodyIndices.size(); ++i) {
        const SceneObject3D* object = m_objects[i];
        if (object == nullptr) {
            continue;
        }
        m_physics.setBodyPosition(m_physicsBodyIndices[i], object->x(), object->y(), object->z());
    }
}

void World3D::syncSceneFromPhysics() {
    for (usize i = 0; i < m_objects.size() && i < m_physicsBodyIndices.size(); ++i) {
        SceneObject3D* object = m_objects[i];
        if (object == nullptr) {
            continue;
        }
        float x = 0.f;
        float y = 0.f;
        float z = 0.f;
        m_physics.getBodyPosition(m_physicsBodyIndices[i], x, y, z);
        object->setPosition(x, y);
        object->setZ(z);
    }
}

void World3D::buildSnapshot() {
    if (!m_root) {
        m_snapshot.clear();
        m_transformSoA.clear();
        return;
    }

    m_snapshot.reserve(static_cast<u32>(m_objects.size()));
    m_transformSoA.reserve(static_cast<u32>(m_objects.size()));
    fillSnapshotSoA(*m_root, m_snapshot, m_transformSoA, false);
}

void World3D::runParallelCull() {
    const u32 count = static_cast<u32>(m_snapshot.objects().size());
    m_cullVisible.assign(count, 0u);

    if (count == 0) {
        m_snapshot.setVisibleCount(0);
        return;
    }

    m_cullJobs.run(count, 4u, [this](u32 i) {
        const ObjectDrawCmd3D& cmd = m_snapshot.objects()[i];
        const float z = m_transformSoA.worldZ[i];
        const bool inFrustum = (z > -500.f && z < 500.f);
        m_cullVisible[i] = (cmd.visible && inFrustum) ? 1u : 0u;
    });

    u32 visible = 0;
    for (u8 v : m_cullVisible) {
        if (v != 0u) {
            ++visible;
        }
    }
    m_snapshot.setVisibleCount(visible);
}

void World3D::tickGameThread(frame::FrameCtx& ctx) {
    if (!m_enabled) {
        return;
    }

    if (m_physicsEnabled) {
        syncPhysicsFromScene();
        m_physics.step(ctx.dt);
        syncSceneFromPhysics();
    }

    buildSnapshot();
}

void World3D::tick(frame::FrameCtx& ctx) {
    tickGameThread(ctx);
    runParallelCull();
}

void World3D::render(frame::FrameCtx& ctx) {
    if (!m_enabled) {
        return;
    }

    if (!platform::isRenderThread()) {
        log::warn("World3D::render called off render thread — GPU touch forbidden in v1");
        return;
    }

    // E03: the GPU renderer (hybrid SceneRenderer) records the world's registry + camera. Without one (no Vulkan
    // device) nothing is recorded here and HybridComposer's PlaceholderRenderer fallback draws the frame.
    if (m_renderer != nullptr) {
        if (m_renderer->renderWorld3D(*this, ctx)) {
            ++m_gpuFrames;
        } else {
            ++m_gpuFailures;
        }
    }
}

} // namespace fuse::world3d
