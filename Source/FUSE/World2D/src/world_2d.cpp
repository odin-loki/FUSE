#include <fuse/world2d/world_2d.hpp>

#include <fuse/log/logger.hpp>
#include <fuse/platform/thread.hpp>
#include <fuse/world2d/fuselevel_bridge.hpp>

#include <filesystem>

#include <cmath>

namespace fuse::world2d {

#if !(defined(FUSE_WORLD2D_HAS_FUSELEVEL) && FUSE_WORLD2D_HAS_FUSELEVEL)
// fuselevel_bridge.cpp needs fuse_scene (FUSE_BUILD_PROJECT); report the load as unavailable instead.
FuselevelLoadResult populateWorld2DFromFuselevel(World2D&, const std::string& fuselevelPath) {
    FuselevelLoadResult result;
    result.note = "fuselevel loading unavailable (built without FUSE_BUILD_PROJECT)";
    log::warn("World2D: cannot load '%s' — fuselevel loading requires FUSE_BUILD_PROJECT", fuselevelPath.c_str());
    return result;
}
#endif

World2D::World2D() : m_root(std::make_unique<SceneObject2D>("World2DRoot")) {
    m_physics.init();
}

World2D::~World2D() {
    clearLoadedSprites_();
}

void World2D::clearLoadedSprites_() {
    for (const Handle<Object> handle : m_sprites) {
        if (SceneObject2D* sprite = m_handles.resolve(handle)) {
            if (m_root && sprite->parent() == m_root.get()) {
                m_root->removeChild(sprite);
            }
            m_handles.unpublish(*sprite);
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

Handle<Object> World2D::addSprite(SceneObject2D* sprite) {
    if (sprite == nullptr) {
        return Handle<Object>::invalid();
    }
    if (m_handles.valid(sprite->handle()) && m_handles.resolve(sprite->handle()) == sprite) {
        return sprite->handle(); // already in this world
    }
    const Handle<Object> handle = m_handles.publish(*sprite);
    m_sprites.push_back(handle);
    if (m_root) {
        m_root->addChild(sprite);
    }
    attachPhysicsBodyForSprite_(sprite);
    return handle;
}

void World2D::removeSprite(SceneObject2D* sprite) {
    if (sprite == nullptr || !m_handles.valid(sprite->handle()) || m_handles.resolve(sprite->handle()) != sprite) {
        return;
    }
    const Handle<Object> handle = sprite->handle();
    for (usize i = 0; i < m_sprites.size(); ++i) {
        if (m_sprites[i] == handle) {
            m_sprites.erase(m_sprites.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
    }
    m_handles.unpublish(*sprite);
    if (m_root && sprite->parent() == m_root.get()) {
        m_root->removeChild(sprite);
    }
    for (usize i = 0; i < m_ownedSprites.size(); ++i) {
        if (m_ownedSprites[i].get() == sprite) {
            m_ownedSprites.erase(m_ownedSprites.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
    }
    rebuildPhysicsBodies_();
}

bool World2D::destroySprite(Handle<Object> handle) {
    SceneObject2D* sprite = m_handles.resolve(handle);
    if (sprite == nullptr) {
        return false;
    }
    removeSprite(sprite);
    return true;
}

void World2D::pruneDeadSprites_() {
    bool pruned = false;
    for (usize i = 0; i < m_sprites.size();) {
        if (!m_handles.valid(m_sprites[i])) {
            m_sprites.erase(m_sprites.begin() + static_cast<std::ptrdiff_t>(i));
            pruned = true;
        } else {
            ++i;
        }
    }
    if (pruned) {
        rebuildPhysicsBodies_();
    }
}

void World2D::attachPhysicsBodyForSprite_(SceneObject2D* sprite) {
    if (!m_physicsEnabled || sprite == nullptr || !sprite->physicsEnabled()) {
        m_physicsBodyIndices.push_back(kNoPhysicsBody);
        return;
    }

    const u32 collisionLayer = static_cast<u32>(sprite->collisionLayer());
    const u32 collisionMask = sprite->collisionMask();

    u32 bodyIndex = kNoPhysicsBody;
    if (sprite->physicsShape() == PhysicsShape2D::Box) {
        const math::Vec3 world = sprite->worldTranslation();
        bodyIndex = m_physics.addBoxBody(world.x, world.y, sprite->boxHalfWidth(),
                                         sprite->boxHalfHeight(), 1.f, collisionLayer, collisionMask);
    } else {
        const f32 radius =
            sprite->physicsShape() == PhysicsShape2D::Circle ? sprite->physicsRadius() : 0.5f;
        const math::Vec3 world = sprite->worldTranslation();
        bodyIndex = m_physics.addCircleBody(world.x, world.y, radius, 1.f, collisionLayer,
                                            collisionMask);
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

    for (const Handle<Object> handle : m_sprites) {
        attachPhysicsBodyForSprite_(m_handles.resolve(handle));
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
        const SceneObject2D* sprite = m_handles.resolve(m_sprites[i]);
        if (sprite == nullptr) {
            continue;
        }
        const math::Vec3 world = sprite->worldTranslation();
        m_physics.setBodyPosition(bodyIndex, world.x, world.y);
    }
}

void World2D::syncSceneFromPhysics() {
    for (usize i = 0; i < m_sprites.size() && i < m_physicsBodyIndices.size(); ++i) {
        const u32 bodyIndex = m_physicsBodyIndices[i];
        if (bodyIndex == kNoPhysicsBody) {
            continue;
        }
        SceneObject2D* sprite = m_handles.resolve(m_sprites[i]);
        if (sprite == nullptr) {
            continue;
        }
        float x = 0.f;
        float y = 0.f;
        m_physics.getBodyPosition(bodyIndex, x, y);
        const SceneObject2D* parentNode = sprite->sceneParent();
        if (parentNode == nullptr || parentNode == m_root.get()) {
            // Root children (the common case): the root is only ever translated, so offset by it.
            const math::Vec3 rootT = parentNode != nullptr ? parentNode->worldTranslation() : math::Vec3{};
            sprite->setPosition(x - rootT.x, y - rootT.y);
        } else {
            // Nested sprite: convert the world position back through the parent chain.
            math::Vec3 t;
            math::Quat r;
            math::Vec3 sc;
            scene_math::decomposeTRS(sprite->worldMatrix(), t, r, sc);
            sprite->setWorldPose({x, y, t.z}, r);
        }
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
    m_cullVisible.assign(count, 0u);

    if (count == 0) {
        m_snapshot.setVisibleCount(0);
        return;
    }

    m_cullJobs.run(count, 8u, [this](u32 i) {
        const SpriteDrawCmd& cmd = m_snapshot.sprites()[i];
        const float x = m_transformSoA.worldX[i];
        const float y = m_transformSoA.worldY[i];
        const bool inView = (x > -10000.f && x < 10000.f && y > -10000.f && y < 10000.f);
        m_cullVisible[i] = (cmd.visible && inView) ? 1u : 0u;
    });

    u32 visible = 0;
    for (u8 v : m_cullVisible) {
        if (v != 0u) {
            ++visible;
        }
    }
    m_snapshot.setVisibleCount(visible);
}

void World2D::tickGameThread(frame::FrameCtx& ctx) {
    if (!m_enabled) {
        return;
    }

    pruneDeadSprites_();

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
