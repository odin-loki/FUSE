#include <fuse/adventure/interaction.hpp>

namespace fuse::adventure {

bool InteractionSystem::tryInteract(const InteractionRequest& request) {
    if (request.actor.isValid() && request.target.isValid()) {
        ++m_successCount;
        return true;
    }
    return false;
}

} // namespace fuse::adventure
