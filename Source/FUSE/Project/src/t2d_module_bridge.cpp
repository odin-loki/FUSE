#include <fuse/project/t2d_module_bridge.hpp>

#include <fuse/legacy/t2d/scene_adapter.hpp>
#include <fuse/log/logger.hpp>
#include <fuse/scene/scene.hpp>
#include <fuse/world2d/scene_object_2d.hpp>
#include <fuse/world2d/world_2d.hpp>

#include <cctype>
#include <fstream>
#include <sstream>

namespace fuse::project {

namespace {

std::string readFileToString(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

bool parseFloatPair(const std::string& text, float& x, float& y) {
    std::istringstream stream(text);
    return static_cast<bool>(stream >> x >> y);
}

bool isSpriteLikeClass(const std::string& className) {
    if (className.find("Sprite") != std::string::npos) {
        return true;
    }
    if (className.find("SceneObject") != std::string::npos) {
        return true;
    }
    if (className.find("Player") != std::string::npos) {
        return true;
    }
    return false;
}

fuse::scene::SceneEntityTransform makeSceneTransform(const T2DSceneNodeStub& node) {
    fuse::scene::SceneEntityTransform transform{};
    float x = 0.f;
    float y = 0.f;
    if (!node.position.empty()) {
        parseFloatPair(node.position, x, y);
    }
    transform.positionX = x;
    transform.positionY = y;
    transform.positionZ = 0.f;
    transform.rotationW = 1.f;
    transform.scaleX = 1.f;
    transform.scaleY = 1.f;
    transform.scaleZ = 1.f;
    return transform;
}

legacy::t2d::LegacySceneObjectStub makeLegacyStub(const T2DSceneNodeStub& node, u32 spriteIndex) {
    legacy::t2d::LegacySceneObjectStub legacy{};
    legacy.legacyId = spriteIndex + 1u;
    legacy.name = node.objectName.empty() ? node.className : node.objectName;
    legacy.layer = node.layer;
    legacy.sortKey = node.sortKey;
    legacy.compositeSprite = node.isCompositeSprite;
    legacy.physicsEnabled = node.physicsEnabled;
    legacy.collisionLayer = node.collisionLayer;
    legacy.collisionMask = node.collisionMask;
    legacy.physicsRadius = node.physicsRadius;
    legacy.boxHalfWidth = node.boxHalfWidth;
    legacy.boxHalfHeight = node.boxHalfHeight;
    switch (node.physicsShape) {
    case T2DPhysicsShape::Circle:
        legacy.physicsShape = legacy::t2d::LegacyPhysicsShape::Circle;
        break;
    case T2DPhysicsShape::Box:
        legacy.physicsShape = legacy::t2d::LegacyPhysicsShape::Box;
        break;
    default:
        legacy.physicsShape = legacy::t2d::LegacyPhysicsShape::None;
        break;
    }
    if (!node.position.empty()) {
        parseFloatPair(node.position, legacy.x, legacy.y);
    }
    return legacy;
}

} // namespace

u32 populateSceneFromModuleExtract(fuse::scene::Scene& scene, const T2DModuleExtract& extract) {
    scene.clearEntities();

    std::vector<s32> parentAtDepth;
    u32 entityCount = 0;

    for (const T2DSceneNodeStub& node : extract.sceneNodes) {
        s32 parentIndex = -1;
        if (node.depth > 0 && static_cast<std::size_t>(node.depth - 1) < parentAtDepth.size()) {
            parentIndex = parentAtDepth[static_cast<std::size_t>(node.depth - 1)];
        }

        const std::string entityName =
            node.objectName.empty() ? node.className : node.objectName;
        scene.addEntity(entityName, makeSceneTransform(node), parentIndex);
        const s32 entityIndex = static_cast<s32>(scene.entityCount() - 1u);

        if (static_cast<std::size_t>(node.depth) >= parentAtDepth.size()) {
            parentAtDepth.resize(static_cast<std::size_t>(node.depth) + 1u, -1);
        }
        parentAtDepth[static_cast<std::size_t>(node.depth)] = entityIndex;
        ++entityCount;
    }

    if (entityCount == 0) {
        scene.addEntity("ModuleRoot");
        entityCount = 1u;
    }

    return entityCount;
}

T2DRuntimeBridgeResult populateWorld2DFromModuleExtract(fuse::world2d::World2D& world,
                                                        const T2DModuleExtract& extract) {
    T2DRuntimeBridgeResult result;
    result.nodeCount = static_cast<u32>(extract.sceneNodes.size());

    bool anyPhysics = false;
    for (const T2DSceneNodeStub& node : extract.sceneNodes) {
        if (node.physicsEnabled) {
            anyPhysics = true;
            break;
        }
    }
    if (anyPhysics) {
        world.setPhysicsEnabled(true);
    }

    u32 spriteIndex = 0;
    std::vector<fuse::SceneObject2D*> depthParents;
    depthParents.resize(32u, nullptr);

    for (const T2DSceneNodeStub& node : extract.sceneNodes) {
        if (!isSpriteLikeClass(node.className) && !node.isCompositeSprite) {
            if (static_cast<std::size_t>(node.depth) < depthParents.size()) {
                depthParents[static_cast<std::size_t>(node.depth)] = nullptr;
            }
            continue;
        }

        auto sprite = std::make_unique<fuse::SceneObject2D>(
            node.objectName.empty() ? node.className : node.objectName);

        const legacy::t2d::LegacySceneObjectStub legacy = makeLegacyStub(node, spriteIndex);
        if (!legacy::t2d::importSceneObject(legacy, *sprite)) {
            continue;
        }

        if (node.physicsEnabled && sprite->physicsShape() == fuse::PhysicsShape2D::None) {
            sprite->setPhysicsShape(fuse::PhysicsShape2D::Circle);
        }

        fuse::SceneObject2D* parent = nullptr;
        if (node.depth > 0 && static_cast<std::size_t>(node.depth - 1) < depthParents.size()) {
            parent = depthParents[static_cast<std::size_t>(node.depth - 1)];
        }

        if (parent != nullptr) {
            parent->addChild(sprite.get());
            world.addSprite(sprite.get());
        } else {
            world.addSprite(sprite.get());
        }

        if (static_cast<std::size_t>(node.depth) < depthParents.size()) {
            depthParents[static_cast<std::size_t>(node.depth)] =
                node.isCompositeSprite ? sprite.get() : nullptr;
        }

        if (node.physicsEnabled) {
            ++result.physicsBodyCount;
            if (node.physicsShape == T2DPhysicsShape::Box) {
                ++result.boxBodyCount;
            } else {
                ++result.circleBodyCount;
            }
            if (node.collisionLayer != 0) {
                ++result.collisionLayerCount;
            }
        }

        world.adoptOwnedSprite(std::move(sprite));
        ++spriteIndex;
    }

    result.spriteCount = spriteIndex;
    result.ok = true;
    result.note = "bridged " + std::to_string(result.spriteCount) +
                  " sprites from T2D module extract (layers/physics shapes/collision)";
    return result;
}

T2DRuntimeBridgeResult bridgeT2DModuleToRuntime(fuse::world2d::World2D& world,
                                              const std::string& modulePath) {
    T2DRuntimeBridgeResult result;

    const std::string text = readFileToString(modulePath);
    if (text.empty()) {
        result.note = "unable to read module file";
        return result;
    }

    const T2DModuleExtract extract = extractT2DModuleFields(text, modulePath);
    result = populateWorld2DFromModuleExtract(world, extract);
    fuse::log::info("bridgeT2DModuleToRuntime: %s -> %u sprites", modulePath.c_str(), result.spriteCount);
    return result;
}

} // namespace fuse::project
