#pragma once

#include <fuse/handle.hpp>
#include <fuse/object.hpp>
#include <fuse/types.hpp>

#include <string>

namespace fuse::mechanics {

/// Interactable stub — game-thread mutation only.
/// TODO(U5 extract): GMK component patterns + physics rebind to FUSE composition boundary.
class Interactable {
public:
    Interactable(Handle<Object> owner, std::string prompt);

    Handle<Object> owner() const { return m_owner; }
    const std::string& prompt() const { return m_prompt; }
    bool isEnabled() const { return m_enabled; }
    void setEnabled(bool enabled) { m_enabled = enabled; }

private:
    Handle<Object> m_owner;
    std::string m_prompt;
    bool m_enabled = true;
};

} // namespace fuse::mechanics
