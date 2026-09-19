#pragma once

#include <fuse/types.hpp>

#include <string>

namespace fuse {
class Object;
}

namespace fuse::legacy {

/// Distilled SimObject identity + transform fields — no Engine `SimObject*` crosses this boundary.
struct LegacySimObjectStub {
    u32 simObjectId = 0;
    u32 legacyId = 0;
    std::string internalName;
    std::string className;
    std::string parentName;
    std::string name;
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;
    s32 layer = 0;
};

/// Copy quarantined SimObject fields onto a greenfield `fuse::Object` (game thread only).
bool bridgeToObject(const LegacySimObjectStub& legacy, fuse::Object& out);

/// Read greenfield identity fields into a stub record (game thread only).
bool bridgeFromObject(const fuse::Object& src, LegacySimObjectStub& out);

} // namespace fuse::legacy
