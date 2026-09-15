#include <fuse/world2d/world_2d.hpp>

#include <fuse/jobs/parallel_for.hpp>
#include <fuse/log/logger.hpp>
#include <fuse/platform/thread.hpp>

#include <cmath>

namespace fuse::world2d {

World2D::World2D() : m_root(std::make_unique<SceneObject2D>("World2DRoot")) {
    m_physics.init();
}

World2D::~World2D() = default;

void World2D::loadWorld(dimension::WorldHandle world) {
    m_activeWorld = world;
    log::info("World2D: load world handle index=%u gen=%u", world.index(), world.generation());
}

void World2D::addSprite(SceneObject2D* sprite) {
    if (sprite == nullptr) {
        return;
    }
    m_sprites.push_back(sprite);
    if (m_root) {
        m_root->addChild(sprite);
    }
    if (m_physicsEnabled) {
        const u32 bodyIndex = m_physics.addCircleBody(sprite->x(), sprite->y(), 0.5f, 1.f);
        m_physicsBodyIndices.push_back(bodyIndex);
    }
}

void World2D::syncPhysicsFromScene() {
    for (usize i = 0; i < m_sprites.size() && i < m_physicsBodyIndices.size(); ++i) {
        const SceneObject2D* sprite = m_sprites[i];
        if (sprite == nullptr) {
            continue;
        }
        m_physics.setBodyPosition(m_physicsBodyIndices[i], sprite->x(), sprite->y());
    }
}

void World2D::syncSceneFromPhysics() {
    for (usize i = 0; i < m_sprites.size() && i < m_physicsBodyIndices.size(); ++i) {
        SceneObject2D* sprite = m_sprites[i];
        if (sprite == nullptr) {
            continue;
        }
        float x = 0.f;
        float y = 0.f;
        m_physics.getBodyPosition(m_physicsBodyIndices[i], x, y);
        sprite->setPosition(x, y);
    }
}

void World2D::buildSnapshot(frame::FrameCtx& ctx) {
    m_snapshot.clear();
    m_snapshot.reserve(static_cast<u32>(m_sprites.size()));

    for (SceneObject2D* sprite : m_sprites) {
        if (sprite == nullptr) {
            continue;
        }

        SpriteDrawCmd cmd;
        cmd.object = sprite->handle();
        cmd.x = sprite->x();
        cmd.y = sprite->y();
        cmd.rotation = ctx.time * 1.5f;
        cmd.layer = static_cast<u32>(sprite->layer());
        cmd.visible = true;
        m_snapshot.addSprite(cmd);
    }
}

void World2D::runParallelCull() {
    const u32 count = static_cast<u32>(m_snapshot.sprites().size());
    m_cullVisible.assign(count, false);

    if (count == 0) {
        m_snapshot.setVisibleCount(0);
        return;
    }

    jobs::parallel_for(0u, count, 8u, [this](u32 i) {
        const SpriteDrawCmd& cmd = m_snapshot.sprites()[i];
        const bool inView = (cmd.x > -10000.f && cmd.x < 10000.f && cmd.y > -10000.f && cmd.y < 10000.f);
        m_cullVisible[i] = cmd.visible && inView;
    });

    u32 visible = 0;
    for (bool v : m_cullVisible) {
        if (v) {
            ++visible;
        }
    }
    m_snapshot.setVisibleCount(visible);
}

void World2D::tick(frame::FrameCtx& ctx) {
    if (!m_enabled) {
        return;
    }

    if (m_physicsEnabled) {
        syncPhysicsFromScene();
        m_physics.step(ctx.dt);
        syncSceneFromPhysics();
    }

    buildSnapshot(ctx);
    runParallelCull();
}

void World2D::render(frame::FrameCtx& /*ctx*/) {
    if (!m_enabled) {
        return;
    }

    if (!platform::isRenderThread()) {
        log::warn("World2D::render called off render thread — GPU touch forbidden in v1");
        return;
    }

    // Placeholder path — HybridComposer records actual draws via PlaceholderRenderer.
}

} // namespace fuse::world2d
