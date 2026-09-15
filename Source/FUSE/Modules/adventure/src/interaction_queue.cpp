#include <fuse/adventure/interaction_queue.hpp>

#include <fuse/mechanics/interact_action.hpp>

namespace fuse::adventure {

void InteractionQueue::enqueue(QueuedInteraction entry) {
    if (entry.target == nullptr) {
        return;
    }

    m_pending.push_back(std::move(entry));
}

void InteractionQueue::clear() {
    m_pending.clear();
}

InteractResult InteractionQueue::dispatchEntry(mechanics::MechanicsRegistry& registry,
                                             MechanicsBridge& bridge,
                                             QueuedInteraction& entry) {
    const auto kind = mechanics::actionKindForVerb(entry.ctx.verb);

    switch (kind) {
    case mechanics::InteractActionKind::Use:
        return bridge.use(registry, entry.target, entry.instigator, entry.ctx);
    case mechanics::InteractActionKind::Pickup:
        return bridge.pickup(registry, entry.target, entry.instigator, entry.ctx);
    case mechanics::InteractActionKind::Examine:
        if (registry.interact(entry.target, entry.ctx)) {
            return InteractResult::Used;
        }
        return InteractResult::Failed;
    }

    return InteractResult::Failed;
}

u32 InteractionQueue::drain(mechanics::MechanicsRegistry& registry, MechanicsBridge& bridge) {
    const u32 batchSize = static_cast<u32>(m_pending.size());

    for (QueuedInteraction& entry : m_pending) {
        const InteractResult result = dispatchEntry(registry, bridge, entry);
        if (result == InteractResult::Failed || result == InteractResult::Ignored) {
            ++m_failed;
        } else {
            ++m_processed;
        }
    }

    m_pending.clear();
    return batchSize;
}

} // namespace fuse::adventure
