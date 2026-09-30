#include <fuse/world3d/world_3d.hpp>

#include <fuse/ecs/components/collider.hpp>
#include <fuse/ecs/components/light.hpp>
#include <fuse/ecs/components/mesh.hpp>
#include <fuse/ecs/components/rigidbody.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/world2d/scene_transform.hpp>
#include <fuse/log/logger.hpp>
#include <fuse/platform/thread.hpp>

#include <cmath>

namespace fuse::world3d {

World3D::World3D() : m_root(std::make_unique<SceneObject3D>("World3DRoot")) {}

World3D::~World3D() {
    m_schedule.shutdown();
    for (const ObjectEntry& entry : m_objects) {
        if (SceneObject2D* node = m_handles.resolve(entry.handle)) {
            m_handles.unpublish(*node);
        }
    }
    m_objects.clear();
    if (m_registryReady) {
        m_registry.destroy();
    }
}

ecs::Registry& World3D::ownRegistry_() {
    if (!m_registryReady) {
        m_registry.init(kRegistryCapacity);
        m_registryReady = true;
    }
    return m_registry;
}

RuntimeSchedule& World3D::schedule() {
    if (!m_schedule.initialized()) {
        // The schedule simulates the world's own registry; an external render registry (the editor's
        // edited scene) is only drawn, never simulated here.
        m_schedule.init(ownRegistry_(), m_scheduleDesc);
        m_schedule.setStageEnabled(RuntimeStage::Physics, m_physicsEnabled);
        m_schedule.setHook(RuntimeStage::RenderExtract, &World3D::renderExtractHook_, this);
    }
    return m_schedule;
}

ecs::Registry& World3D::registry() {
    if (m_externalRegistry != nullptr) {
        return *m_externalRegistry;
    }
    return ownRegistry_();
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
    t.dirty = false; // TRS and local_to_world already agree
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
    // Keep the TRS consistent with the basis so TransformSystem (runtime schedule) rebuilds the same frame.
    const math::Quat q = scene_math::quatFromBasis({x[0], x[1], x[2]}, {y[0], y[1], y[2]}, {z[0], z[1], z[2]});
    t.rotation = ecs::quat{q.x, q.y, q.z, q.w};
    t.dirty = false;
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
    t.dirty = false;
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

Handle<Object> World3D::addObject(SceneObject3D* object) {
    if (object == nullptr) {
        return Handle<Object>::invalid();
    }
    if (m_handles.valid(object->handle()) && m_handles.resolve(object->handle()) == object) {
        return object->handle(); // already in this world
    }
    ObjectEntry entry;
    entry.handle = m_handles.publish(*object);
    if (m_root) {
        m_root->addChild(object);
    }
    if (m_physicsEnabled) {
        createBody_(entry, *object);
    }
    m_objects.push_back(entry);
    return entry.handle;
}

void World3D::removeObject(SceneObject3D* object) {
    if (object == nullptr || m_handles.resolve(object->handle()) != object) {
        return;
    }
    const Handle<Object> handle = object->handle();
    for (usize i = 0; i < m_objects.size(); ++i) {
        if (m_objects[i].handle == handle) {
            destroyBody_(m_objects[i]);
            m_objects.erase(m_objects.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
    }
    m_handles.unpublish(*object);
    if (m_root && object->parent() == m_root.get()) {
        m_root->removeChild(object);
    }
}

SceneObject3D* World3D::resolve(Handle<Object> handle) const {
    return asSceneObject3D(m_handles.resolve(handle));
}

ecs::EntityID World3D::bodyOf(Handle<Object> handle) const {
    for (const ObjectEntry& entry : m_objects) {
        if (entry.handle == handle) {
            return entry.body;
        }
    }
    return ecs::EntityID::null();
}

void World3D::clearDynamicObjects() {
    for (ObjectEntry& entry : m_objects) {
        destroyBody_(entry);
        if (SceneObject2D* node = m_handles.resolve(entry.handle)) {
            if (m_root && node->parent() == m_root.get()) {
                m_root->removeChild(node);
            }
            m_handles.unpublish(*node);
        }
    }
    m_objects.clear();
    m_snapshot.clear();
    m_transformSoA.clear();
    m_cullVisible.clear();
}

void World3D::createBody_(ObjectEntry& entry, SceneObject3D& object) {
    if (entry.body.valid()) {
        return;
    }
    ecs::Registry& r = ownRegistry_();
    const math::Mat4& world = object.worldMatrix();
    math::Vec3 t;
    math::Quat q;
    math::Vec3 scale;
    scene_math::decomposeTRS(world, t, q, scale);

    const ecs::EntityID e = r.create();
    ecs::Transform transform{};
    transform.position = ecs::vec3{t.x, t.y, t.z, 1.f};
    transform.rotation = ecs::quat{q.x, q.y, q.z, q.w};
    r.add<ecs::Transform>(e, transform);

    ecs::RigidBody body{};
    body.mass = 1.f;
    body.inv_mass = 1.f;
    r.add<ecs::RigidBody>(e, body);

    // The object's physics shape: Box -> box (half width, half height, half width as depth);
    // Circle -> sphere of physicsRadius; None -> the historical 0.5 m sphere.
    ecs::Collider collider{};
    if (object.physicsShape() == PhysicsShape2D::Box) {
        collider.shape = ecs::Collider::Box;
        collider.params = ecs::vec3{object.boxHalfWidth(), object.boxHalfHeight(), object.boxHalfWidth(), 0.f};
    } else {
        collider.shape = ecs::Collider::Sphere;
        const f32 radius = object.physicsShape() == PhysicsShape2D::Circle ? object.physicsRadius() : 0.5f;
        collider.params = ecs::vec3{radius, 0.f, 0.f, 0.f};
    }
    if (object.collisionLayer() != 0) {
        collider.layer = static_cast<u32>(object.collisionLayer());
    }
    collider.mask = object.collisionMask();
    r.add<ecs::Collider>(e, collider);

    entry.body = e;
    entry.syncedTranslation = t;
    entry.syncedRotation = q;
    entry.syncedVersion = object.worldVersion();
    entry.synced = true;
}

void World3D::destroyBody_(ObjectEntry& entry) {
    if (entry.body.valid() && m_registryReady && m_registry.alive(entry.body)) {
        m_registry.destroy_entity(entry.body);
    }
    entry.body = ecs::EntityID::null();
    entry.synced = false;
}

void World3D::pruneDeadObjects_() {
    for (usize i = 0; i < m_objects.size();) {
        if (!m_handles.valid(m_objects[i].handle)) {
            destroyBody_(m_objects[i]);
            m_objects.erase(m_objects.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
}

ecs::EntityID World3DPhysics::addStaticPlane(const math::Vec3& normal, f32 distance) {
    return m_world->addStaticPlane(normal, distance);
}

u32 World3DPhysics::bodyCount() const {
    return m_world->physicsManager().bodies().count();
}

u32 World3DPhysics::contactCount() const {
    return m_world->physicsManager().solver().contactCount();
}

physics::PhysicsManager& World3DPhysics::manager() const {
    return m_world->physicsManager();
}

ecs::EntityID World3D::addStaticPlane(const math::Vec3& normal, f32 distance) {
    ecs::Registry& r = ownRegistry_();
    const ecs::EntityID e = r.create();
    r.add<ecs::Transform>(e, ecs::Transform{});
    ecs::RigidBody body{};
    body.is_static = true;
    body.mass = 0.f;
    body.inv_mass = 0.f;
    r.add<ecs::RigidBody>(e, body);
    ecs::Collider collider{};
    collider.shape = ecs::Collider::Plane;
    const math::Vec3 n = normal.normalized();
    collider.params = ecs::vec3{n.x, n.y, n.z, 0.f};
    collider.scalar = distance;
    r.add<ecs::Collider>(e, collider);
    return e;
}

void World3D::setPhysicsEnabled(bool enabled) {
    if (m_physicsEnabled == enabled) {
        return;
    }
    m_physicsEnabled = enabled;
    if (m_schedule.initialized()) {
        m_schedule.setStageEnabled(RuntimeStage::Physics, enabled);
    }
    for (ObjectEntry& entry : m_objects) {
        if (!enabled) {
            destroyBody_(entry);
        } else if (SceneObject3D* object = resolve(entry.handle)) {
            createBody_(entry, *object);
        }
    }
}

void World3D::syncPhysicsFromScene() {
    // A scene object whose world matrix changed since the last exchange was moved by game code: its
    // body is teleported (PhysicsManager treats a Transform that differs from what it last wrote as a
    // teleport / rotation edit and wakes the body).
    ecs::Registry& r = ownRegistry_();
    for (ObjectEntry& entry : m_objects) {
        if (!entry.body.valid()) {
            continue;
        }
        const SceneObject3D* object = resolve(entry.handle);
        ecs::Transform* transform = r.get<ecs::Transform>(entry.body);
        if (object == nullptr || transform == nullptr) {
            continue;
        }
        const u64 version = object->worldVersion();
        if (entry.synced && version == entry.syncedVersion) {
            continue;
        }
        math::Vec3 t;
        math::Quat q;
        math::Vec3 scale;
        scene_math::decomposeTRS(object->worldMatrix(), t, q, scale);
        transform->position = ecs::vec3{t.x, t.y, t.z, 1.f};
        transform->rotation = ecs::quat{q.x, q.y, q.z, q.w};
        transform->dirty = true;
        entry.syncedTranslation = t;
        entry.syncedRotation = q;
        entry.syncedVersion = version;
        entry.synced = true;
    }
}

void World3D::syncSceneFromPhysics() {
    ecs::Registry& r = ownRegistry_();
    for (ObjectEntry& entry : m_objects) {
        if (!entry.body.valid()) {
            continue;
        }
        SceneObject3D* object = resolve(entry.handle);
        const ecs::Transform* transform = r.get<ecs::Transform>(entry.body);
        if (object == nullptr || transform == nullptr) {
            continue;
        }
        const math::Vec3 t{transform->position.x, transform->position.y, transform->position.z};
        const math::Quat q{transform->rotation.x, transform->rotation.y, transform->rotation.z, transform->rotation.w};
        if (entry.synced && t.x == entry.syncedTranslation.x && t.y == entry.syncedTranslation.y &&
            t.z == entry.syncedTranslation.z && q.x == entry.syncedRotation.x && q.y == entry.syncedRotation.y &&
            q.z == entry.syncedRotation.z && q.w == entry.syncedRotation.w) {
            continue; // body did not move (asleep / static)
        }
        const SceneObject2D* parentNode = object->sceneParent();
        const bool identityParent =
            parentNode == nullptr || parentNode->worldMatrix().data == math::Mat4::identity().data;
        if (identityParent) {
            object->setLocalTranslation(t);
            object->setLocalRotation(q);
        } else {
            object->setWorldPose(t, q);
        }
        // Remember the exchanged pose and the resulting world version, so the next pre-physics sync
        // does not mistake this write-back for a game-side move.
        entry.syncedTranslation = t;
        entry.syncedRotation = q;
        entry.syncedVersion = object->worldVersion();
        entry.synced = true;
    }
}

void World3D::renderExtractHook_(void* user, ecs::Registry& /*registry*/, f32 /*dt*/) {
    World3D& world = *static_cast<World3D*>(user);
    if (world.m_physicsEnabled) {
        world.syncSceneFromPhysics();
    }
    world.buildSnapshot();
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

    pruneDeadObjects_();
    RuntimeSchedule& frameSchedule = schedule();
    if (m_physicsEnabled) {
        syncPhysicsFromScene();
    }
    // Fixed-step schedule; its RenderExtract stage (renderExtractHook_) writes the bodies back into
    // the scene objects and builds the snapshot once per frame.
    frameSchedule.advance(ctx.dt);
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
