#include <fuse/legacy/t2d/scene_adapter.hpp>

#include <fuse/platform/thread.hpp>
#include <fuse/world2d/scene_object_2d.hpp>

namespace fuse::legacy::t2d {

bool importSceneObject(const LegacySceneObjectStub& legacy, SceneObject2D& out) {
    if (!fuse::platform::isMainThread()) {
        return false;
    }

    out.setLegacyId(legacy.legacyId);
    if (!legacy.name.empty()) {
        out.setName(legacy.name);
    }
    out.setPosition(legacy.x, legacy.y);
    out.setLayer(legacy.layer);
    return true;
}

bool exportSceneObject(const SceneObject2D& src, LegacySceneObjectStub& out) {
    if (!fuse::platform::isMainThread()) {
        return false;
    }

    out.legacyId = src.legacyId();
    out.name = src.name();
    out.x = src.x();
    out.y = src.y();
    out.layer = src.layer();
    return true;
}

} // namespace fuse::legacy::t2d
