#include <fuse/legacy/t3d/sim_object_bridge.hpp>

#include <fuse/legacy/t3d/scene_adapter.hpp>
#include <fuse/platform/thread.hpp>
#include <fuse/world3d/scene_object_3d.hpp>

namespace fuse::legacy::t3d {

bool importSimObject(const legacy::LegacySimObjectStub& legacy, SceneObject3D& out) {
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
    scene.z = legacy.z;
    return importSceneObject(scene, out);
}

bool exportSimObject(const SceneObject3D& src, legacy::LegacySimObjectStub& out) {
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
    out.z = scene.z;
    if (out.name.empty()) {
        out.name = scene.name;
    }
    if (out.internalName.empty()) {
        out.internalName = scene.name;
    }
    return true;
}

} // namespace fuse::legacy::t3d
