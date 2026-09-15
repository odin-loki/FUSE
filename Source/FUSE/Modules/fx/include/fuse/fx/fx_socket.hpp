#pragma once

#include <fuse/fx/fx_defs.hpp>
#include <fuse/handle.hpp>
#include <fuse/object.hpp>

#include <string>

namespace fuse::fx {

/// FX attachment point — ore analogue: AFX effectron/choreographer sockets.
struct FxSocket {
    Handle<Object> owner = Handle<Object>::invalid();
    FxSocketKind kind = FxSocketKind::Shape3D;
    std::string effectId;
};

} // namespace fuse::fx
