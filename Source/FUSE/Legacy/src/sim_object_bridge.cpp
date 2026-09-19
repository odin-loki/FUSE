#include <fuse/legacy/sim_object_bridge.hpp>

#include <fuse/object.hpp>
#include <fuse/platform/thread.hpp>

namespace fuse::legacy {

bool bridgeToObject(const LegacySimObjectStub& legacy, fuse::Object& out) {
    if (!fuse::platform::isMainThread()) {
        return false;
    }

    const u32 id = legacy.legacyId != 0 ? legacy.legacyId : legacy.simObjectId;
    out.setLegacyId(id);

    if (!legacy.name.empty()) {
        out.setName(legacy.name);
    } else if (!legacy.internalName.empty()) {
        out.setName(legacy.internalName);
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
    if (out.internalName.empty()) {
        out.internalName = src.name();
    }
    return true;
}

} // namespace fuse::legacy
