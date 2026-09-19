#pragma once

#include <fuse/handle.hpp>
#include <fuse/object.hpp>
#include <fuse/types.hpp>

#include <functional>

namespace fuse::ai {

/// Resolves agent world positions from entity handles (picker / scene bind ore).
using AgentPositionProvider = std::function<bool(Handle<Object> entity, float& outX, float& outY)>;

} // namespace fuse::ai
