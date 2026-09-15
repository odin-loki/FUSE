#pragma once

#include <fuse/adventure/interaction.hpp>
#include <fuse/adventure/mechanics_bridge.hpp>
#include <fuse/mechanics/interactable.hpp>
#include <fuse/mechanics/registry.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::adventure {

/// One deferred interaction — posted from input/UI, drained on the game thread.
struct QueuedInteraction {
    mechanics::Component* target = nullptr;
    mechanics::Component* instigator = nullptr;
    mechanics::InteractionContext ctx;
};

/// Game-thread interaction queue (mirrors `fuse::editor::CommandQueue` pattern).
class InteractionQueue {
public:
    void enqueue(QueuedInteraction entry);
    void clear();

    /// Drain all pending entries through `MechanicsBridge` + `MechanicsRegistry`.
    u32 drain(mechanics::MechanicsRegistry& registry, MechanicsBridge& bridge);

    u32 pendingCount() const { return static_cast<u32>(m_pending.size()); }
    u32 processedCount() const { return m_processed; }
    u32 failedCount() const { return m_failed; }

private:
    InteractResult dispatchEntry(mechanics::MechanicsRegistry& registry,
                                 MechanicsBridge& bridge,
                                 QueuedInteraction& entry);

    std::vector<QueuedInteraction> m_pending;
    u32 m_processed = 0;
    u32 m_failed = 0;
};

} // namespace fuse::adventure
