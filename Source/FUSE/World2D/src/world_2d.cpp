#include <fuse/world2d/world_2d.hpp>

#include <fuse/jobs/parallel_for.hpp>
#include <fuse/log/logger.hpp>
#include <fuse/platform/thread.hpp>
#include <fuse/world2d/fuselevel_bridge.hpp>

#include <filesystem>

#include <cmath>

namespace fuse::world2d {

World2D::World2D() : m_root(std::make_unique<SceneObject2D>("World2DRoot")) {
    m_physics.init();
}

World2D::~World2D() {
    clearLoadedSprites_();
}

void World2D::clearLoadedSprites_() {
    if (m_root) {
        for (SceneObject2D* sprite : m_sprites) {
            m_root->removeChild(sprite);
        }
    }
    m_sprites.clear();
    while (!m_ownedSprites.empty()) {
        SceneObject2D* sprite = m_ownedSprites.back().get();
        if (sprite != nullptr && sprite->parent() != nullptr) {
            sprite->parent()->removeChild(sprite);
        }
        m_ownedSprites.pop_back();
    }
    m_physics.reset();
    m_physics.init();
    m_physicsBodyIndices.clear();
    m_physicsEnabled = false;
    m_snapshot.clear();
    m_transformSoA.clear();
    m_cullVisible.clear();
}

FuselevelLoadResult World2D::loadWorldFromFuselevel(const std::string& fuselevelPath) {
    clearLoadedSprites_();
    m_lastFuselevelLoad = populateWorld2DFromFuselevel(*this, fuselevelPath);
    return m_lastFuselevelLoad;
}

void World2D::setProjectWorldSource(std::string projectRoot, std::string defaultWorld2DRel) {
    m_projectRoot = std::move(projectRoot);
    m_defaultWorld2DRel = std::move(defaultWorld2DRel);
}

void World2D::loadWorld(dimension::WorldHandle world) {
    m_activeWorld = world;
    if (!m_pendingFuselevelPath.empty()) {
        loadWorldFromFuselevel(m_pendingFuselevelPath);
        return;
    }

    if (!m_projectRoot.empty() && !m_defaultWorld2DRel.empty()) {
        std::filesystem::path worldPath = std::filesystem::path(m_projectRoot) / m_defaultWorld2DRel;
        loadWorldFromFuselevel(worldPath.lexically_normal().string());
        return;
    }

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
    attachPhysicsBodyForSprite_(sprite);
}

void World2D::attachPhysicsBodyForSprite_(SceneObject2D* sprite) {
    if (!m_physicsEnabled || sprite == nullptr || !sprite->physicsEnabled()) {
        m_physicsBodyIndices.push_back(kNoPhysicsBody);
        return;
    }

    u32 bodyIndex = kNoPhysicsBody;
    if (sprite->physicsShape() == PhysicsShape2D::Box) {
        bodyIndex = m_physics.addBoxBody(sprite->x(), sprite->y(), sprite->boxHalfWidth(),
                                         sprite->boxHalfHeight(), 1.f);
    } else {
        const f32 radius =
            sprite->physicsShape() == PhysicsShape2D::Circle ? sprite->physicsRadius() : 0.5f;
        bodyIndex = m_physics.addCircleBody(sprite->x(), sprite->y(), radius, 1.f);
    }
    m_physicsBodyIndices.push_back(bodyIndex);
}

void World2D::rebuildPhysicsBodies_() {
    m_physics.reset();
    m_physics.init();
    m_physicsBodyIndices.clear();
    if (!m_physicsEnabled) {
        return;
    }

    for (SceneObject2D* sprite : m_sprites) {
        attachPhysicsBodyForSprite_(sprite);
    }
}

void World2D::setPhysicsEnabled(bool enabled) {
    if (m_physicsEnabled == enabled) {
        return;
    }
    m_physicsEnabled = enabled;
    rebuildPhysicsBodies_();
}

void World2D::adoptOwnedSprite(std::unique_ptr<SceneObject2D> sprite) {
    if (sprite == nullptr) {
        return;
    }
    m_ownedSprites.push_back(std::move(sprite));
}

void World2D::syncPhysicsFromScene() {
    for (usize i = 0; i < m_sprites.size() && i < m_physicsBodyIndices.size(); ++i) {
        const u32 bodyIndex = m_physicsBodyIndices[i];
        if (bodyIndex == kNoPhysicsBody) {
            continue;
        }
        const SceneObject2D* sprite = m_sprites[i];
        if (sprite == nullptr) {
            continue;
        }
        m_physics.setBodyPosition(bodyIndex, sprite->x(), sprite->y());
    }
}

void World2D::syncSceneFromPhysics() {
    for (usize i = 0; i < m_sprites.size() && i < m_physicsBodyIndices.size(); ++i) {
        const u32 bodyIndex = m_physicsBodyIndices[i];
        if (bodyIndex == kNoPhysicsBody) {
            continue;
        }
        SceneObject2D* sprite = m_sprites[i];
        if (sprite == nullptr) {
            continue;
        }
        float x = 0.f;
        float y = 0.f;
        m_physics.getBodyPosition(bodyIndex, x, y);
        sprite->setPosition(x, y);
    }
}

void World2D::buildSnapshot(frame::FrameCtx& ctx) {
    if (!m_root) {
        m_snapshot.clear();
        m_transformSoA.clear();
        return;
    }

    m_snapshot.reserve(static_cast<u32>(m_sprites.size()));
    m_transformSoA.reserve(static_cast<u32>(m_sprites.size()));
    fillSnapshotSoA(*m_root, m_snapshot, m_transformSoA, false);
    m_snapshot.setSpriteRotations(ctx.time * 1.5f);
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
        const float x = m_transformSoA.worldX[i];
        const float y = m_transformSoA.worldY[i];
        const bool inView = (x > -10000.f && x < 10000.f && y > -10000.f && y < 10000.f);
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

void World2D::tickGameThread(frame::FrameCtx& ctx) {
    if (!m_enabled) {
        return;
    }

    if (m_physicsEnabled) {
        syncPhysicsFromScene();
        m_physics.step(ctx.dt);
        syncSceneFromPhysics();
    }

    buildSnapshot(ctx);
}

void World2D::tick(frame::FrameCtx& ctx) {
    tickGameThread(ctx);
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
