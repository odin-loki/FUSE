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
};

struct T3DMaterialRefStub {
    std::string assetPath;
};

struct T3DDatablockRefStub {
    std::string datablockName;
};

struct T3DMissionExtract {
    std::string missionName;
    std::vector<T3DSimObjectStub> simObjects;
    std::vector<T3DMaterialRefStub> materials;
    std::vector<T3DDatablockRefStub> datablocks;
};

struct T2DSceneNodeStub {
    std::string className;
    std::string objectName;
    s32 depth = 0;
    std::string position;
};

struct T2DModuleExtract {
    std::string moduleName;
    std::vector<T2DSceneNodeStub> sceneNodes;
};

T3DMissionExtract extractT3DMissionFields(const std::string& missionText);
T2DModuleExtract extractT2DModuleFields(const std::string& moduleText, const std::string& fallbackPath);

} // namespace fuse::project
