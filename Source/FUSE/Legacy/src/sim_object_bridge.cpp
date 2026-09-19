#include <fuse/legacy/sim_object_bridge.hpp>

#include <fuse/object.hpp>
#include <fuse/platform/thread.hpp>
#include <fuse/world2d/scene_object_2d.hpp>

namespace fuse::legacy {

bool bridgeToObject(const LegacySimObjectStub& legacy, fuse::Object& out) {
    if (!fuse::platform::isMainThread()) {
        return false;
    }

    const u32 id = legacy.legacyId != 0 ? legacy.legacyId : legacy.simObjectId;
    out.setLegacyId(id);

    if (!legacy.className.empty()) {
        out.setLegacyClassName(legacy.className);
    }
    if (!legacy.parentName.empty()) {
        out.setLegacyParentName(legacy.parentName);
    }
    if (!legacy.internalName.empty()) {
        out.setLegacyInternalName(legacy.internalName);
    }

    if (!legacy.name.empty()) {
        out.setName(legacy.name);
    } else if (!legacy.internalName.empty()) {
        out.setName(legacy.internalName);
    }

    if (auto* scene2d = dynamic_cast<fuse::SceneObject2D*>(&out)) {
        scene2d->setLayer(legacy.layer);
    }

    return true;
}

bool bridgeFromObject(const fuse::Object& src, LegacySimObjectStub& out) {
    if (!fuse::platform::isMainThread()) {
        return false;
    }

    out.legacyId = src.legacyId();
    out.simObjectId = src.legacyId();
    out.name = src.name();
    out.className = src.legacyClassName();
    out.parentName = src.legacyParentName();
    out.internalName = src.legacyInternalName();
    if (out.internalName.empty()) {
        out.internalName = src.name();
    }

    if (const auto* scene2d = dynamic_cast<const fuse::SceneObject2D*>(&src)) {
        out.layer = scene2d->layer();
    }

    return true;
}

} // namespace fuse::legacy
