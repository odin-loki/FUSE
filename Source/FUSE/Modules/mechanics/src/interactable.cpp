#include <fuse/mechanics/interactable.hpp>

namespace fuse::mechanics {

Interactable::Interactable(Handle<Object> owner, std::string prompt)
    : m_owner(owner)
    , m_prompt(std::move(prompt)) {}

} // namespace fuse::mechanics
