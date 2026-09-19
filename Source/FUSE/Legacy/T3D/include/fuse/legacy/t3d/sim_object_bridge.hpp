#pragma once

#include <fuse/legacy/sim_object_bridge.hpp>

namespace fuse {
class SceneObject3D;
}

namespace fuse::legacy::t3d {

/// Import quarantined SimObject stub into a greenfield 3D node (game thread only).
bool importSimObject(const legacy::LegacySimObjectStub& legacy, SceneObject3D& out);

/// Export greenfield 3D node into a stub record (game thread only).
bool exportSimObject(const SceneObject3D& src, legacy::LegacySimObjectStub& out);

} // namespace fuse::legacy::t3d
