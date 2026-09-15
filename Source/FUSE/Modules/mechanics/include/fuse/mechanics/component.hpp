#pragma once

#include <fuse/handle.hpp>
#include <fuse/object.hpp>
#include <fuse/types.hpp>

namespace fuse::mechanics {

/// Component base stub — ore analogue: GMK SimComponent.
/// TODO(U5 extract): third_party/addons/GMK/Engine/source/component/simComponent.h
class Component {
public:
    explicit Component(Handle<Object> owner);
    Handle<Object> owner() const { return m_owner; }

private:
    Handle<Object> m_owner;
};

} // namespace fuse::mechanics
