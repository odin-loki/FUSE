#include <fuse/world3d/world_3d.hpp>

#include <fuse/jobs/parallel_for.hpp>
#include <fuse/log/logger.hpp>
#include <fuse/platform/thread.hpp>

namespace fuse::world3d {

World3D::World3D() : m_root(std::make_unique<SceneObject3D>("World3DRoot")) {
    m_physics.init();
}

World3D::~World3D() = default;

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
    m_snapshot.clear();
    m_snapshot.reserve(static_cast<u32>(m_objects.size()));

    for (SceneObject3D* object : m_objects) {
        if (object == nullptr) {
            continue;
        }

        ObjectDrawCmd3D cmd;
        cmd.object = object->handle();
        cmd.x = object->x();
        cmd.y = object->y();
        cmd.z = object->z();
        cmd.visible = true;
        m_snapshot.addObject(cmd);
    }
}

void World3D::runParallelCull() {
    const u32 count = static_cast<u32>(m_snapshot.objects().size());
    m_cullVisible.assign(count, false);

    if (count == 0) {
        m_snapshot.setVisibleCount(0);
        return;
    }

    jobs::parallel_for(0u, count, 4u, [this](u32 i) {
        const ObjectDrawCmd3D& cmd = m_snapshot.objects()[i];
        const bool inFrustum = (cmd.z > -500.f && cmd.z < 500.f);
        m_cullVisible[i] = cmd.visible && inFrustum;
    });

    u32 visible = 0;
    for (bool v : m_cullVisible) {
        if (v) {
            ++visible;
        }
    }
    m_snapshot.setVisibleCount(visible);
}

void World3D::tick(frame::FrameCtx& ctx) {
    if (!m_enabled) {
        return;
    }

    if (m_physicsEnabled) {
        syncPhysicsFromScene();
        m_physics.step(ctx.dt);
        syncSceneFromPhysics();
    }

    buildSnapshot();
    runParallelCull();
}

void World3D::render(frame::FrameCtx& /*ctx*/) {
    if (!m_enabled) {
        return;
    }

    if (!platform::isRenderThread()) {
        log::warn("World3D::render called off render thread — GPU touch forbidden in v1");
        return;
    }

    // Placeholder path — HybridComposer records clear via PlaceholderRenderer.
}

} // namespace fuse::world3d
