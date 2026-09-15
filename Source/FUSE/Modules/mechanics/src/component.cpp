#include <fuse/mechanics/component.hpp>

namespace fuse::mechanics {

Component::Component(Handle<Object> owner)
    : m_owner(owner) {}

} // namespace fuse::mechanics
