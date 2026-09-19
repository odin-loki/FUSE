#include <fuse/legacy/t2d/sim_object_bridge.hpp>

#include <fuse/legacy/t2d/scene_adapter.hpp>
#include <fuse/platform/thread.hpp>
#include <fuse/world2d/scene_object_2d.hpp>

namespace fuse::legacy::t2d {

bool importSimObject(const legacy::LegacySimObjectStub& legacy, SceneObject2D& out) {
    if (!fuse::platform::isMainThread()) {
        return false;
    }
    if (!fuse::legacy::bridgeToObject(legacy, out)) {
        return false;
    }

    LegacySceneObjectStub scene{};
    scene.legacyId = legacy.legacyId != 0 ? legacy.legacyId : legacy.simObjectId;
    scene.name = legacy.name.empty() ? legacy.internalName : legacy.name;
    scene.x = legacy.x;
    scene.y = legacy.y;
    scene.layer = legacy.layer;
    return importSceneObject(scene, out);
}

bool exportSimObject(const SceneObject2D& src, legacy::LegacySimObjectStub& out) {
    if (!fuse::platform::isMainThread()) {
        return false;
    }
    if (!fuse::legacy::bridgeFromObject(src, out)) {
        return false;
    }

    LegacySceneObjectStub scene{};
    if (!exportSceneObject(src, scene)) {
        return false;
    }

    out.x = scene.x;
    out.y = scene.y;
    out.layer = scene.layer;
    if (out.name.empty()) {
        out.name = scene.name;
    }
    if (out.internalName.empty()) {
        out.internalName = scene.name;
    }
    return true;
}

} // namespace fuse::legacy::t2d
