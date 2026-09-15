#include <fuse/adventure/interactable.hpp>

namespace fuse::adventure {

InteractResult InteractableStub::onUse(InteractContext& /*ctx*/, ItemId item) {
    ++m_useCount;
    m_lastItem = item;
    return InteractResult::Used;
}

InteractResult InteractableStub::onPickup(InteractContext& /*ctx*/, ItemId item, u32 /*amount*/) {
    ++m_pickupCount;
    m_lastItem = item;
    return InteractResult::PickedUp;
}

} // namespace fuse::adventure
