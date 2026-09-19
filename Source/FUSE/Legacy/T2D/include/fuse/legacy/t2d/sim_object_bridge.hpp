#pragma once

#include <fuse/legacy/sim_object_bridge.hpp>

namespace fuse {
class SceneObject2D;
}

namespace fuse::legacy::t2d {

/// Import quarantined SimObject stub into a greenfield 2D node (game thread only).
bool importSimObject(const legacy::LegacySimObjectStub& legacy, SceneObject2D& out);

/// Export greenfield 2D node into a stub record (game thread only).
bool exportSimObject(const SceneObject2D& src, legacy::LegacySimObjectStub& out);

} // namespace fuse::legacy::t2d
