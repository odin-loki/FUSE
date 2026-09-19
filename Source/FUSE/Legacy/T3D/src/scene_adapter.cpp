#include <fuse/legacy/t3d/scene_adapter.hpp>

#include <fuse/platform/thread.hpp>
#include <fuse/world3d/scene_object_3d.hpp>

namespace fuse::legacy::t3d {

bool importSceneObject(const LegacySceneObjectStub& legacy, SceneObject3D& out) {
    if (!fuse::platform::isMainThread()) {
        return false;
    }

    out.setLegacyId(legacy.legacyId);
    if (!legacy.name.empty()) {
        out.setName(legacy.name);
    }
    out.setPosition(legacy.x, legacy.y);
    out.setZ(legacy.z);
    return true;
}

bool exportSceneObject(const SceneObject3D& src, LegacySceneObjectStub& out) {
    if (!fuse::platform::isMainThread()) {
        return false;
    }

    out.legacyId = src.legacyId();
    out.name = src.name();
    out.x = src.x();
    out.y = src.y();
    out.z = src.z();
    return true;
}

} // namespace fuse::legacy::t3d
