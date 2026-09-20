#pragma once

#include <fuse/types.hpp>

#include <string>
#include <vector>

namespace fuse::project {

struct T3DSimObjectStub {
    std::string className;
    std::string objectName;
    std::string position;
    std::string rotation;
    std::string scale;
    std::string datablockRef;
    std::string materialAsset;
    std::string shaderAsset;
};

struct T3DMaterialRefStub {
    std::string assetPath;
};

struct T3DDatablockRefStub {
    std::string datablockName;
};

struct T3DShaderRefStub {
    std::string shaderRef;
};

struct T3DMissionExtract {
    std::string missionName;
    std::vector<T3DSimObjectStub> simObjects;
    std::vector<T3DMaterialRefStub> materials;
    std::vector<T3DDatablockRefStub> datablocks;
    std::vector<T3DShaderRefStub> shaders;
};

enum class T2DPhysicsShape : u8 {
    None = 0,
    Circle = 1,
    Box = 2,
};

struct T2DSceneNodeStub {
    std::string className;
    std::string objectName;
    std::string position;
    s32 depth = 0;
    s32 layer = 0;
    u32 sortKey = 0;
    bool physicsEnabled = false;
    bool isCompositeSprite = false;
    s32 collisionLayer = 0;
    u32 collisionMask = 0xFFFFFFFFu;
    T2DPhysicsShape physicsShape = T2DPhysicsShape::None;
    float physicsRadius = 0.5f;
    float boxHalfWidth = 0.5f;
    float boxHalfHeight = 0.5f;
    std::string imageMap;
    std::string animationName;
    u32 frameCount = 0;
    float animationFps = 0.f;

    [[nodiscard]] bool hasAnimatedSpriteFields() const {
        return !imageMap.empty() || !animationName.empty() || frameCount > 0u || animationFps > 0.f;
    }
};

struct T2DModuleExtract {
    std::string moduleName;
    std::vector<T2DSceneNodeStub> sceneNodes;
};

T3DMissionExtract extractT3DMissionFields(const std::string& missionText);
T2DModuleExtract extractT2DModuleFields(const std::string& moduleText, const std::string& fallbackPath);

} // namespace fuse::project
