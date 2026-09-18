#pragma once

#include <fuse/types.hpp>

#include <vector>

namespace fuse::platform {

class Window;

enum class PlatformEventType : u8 {
    None = 0,
    Quit,
    WindowCloseRequested,
    WindowResized,
    WindowFocusGained,
    WindowFocusLost,
};

struct PlatformEvent {
    PlatformEventType type = PlatformEventType::None;
    Window* window = nullptr;
    u32 width = 0;
    u32 height = 0;
};

/// Snapshot of the most recent in-place resize coalesce (diagnostic only).
struct ResizeCoalesceRecord {
    Window* window = nullptr;
    u32 width = 0;
    u32 height = 0;
    bool valid = false;
};

/// Pending resize dimensions for a window when a `WindowResized` event is queued.
struct PendingResizeExtent {
    u32 width = 0;
    u32 height = 0;
    bool pending = false;
};

/// Aggregate EventPump counters for headless diagnostics and tests.
struct EventPumpStats {
    u32 pendingEventCount = 0;
    u32 droppedEventCount = 0;
    u32 coalescedResizeCount = 0;
    bool quitRequested = false;
    bool hasPendingQuitEvent = false;
};

/// OS event pump — B1.7 stub drains a synthetic queue only (desktop + mobile no-op).
///
/// Platform backends will override `processOsEvents()` behaviour by replacing this
/// translation unit or routing through a backend registry in a follow-up PR.
class EventPump {
public:
    EventPump();
    ~EventPump();

    EventPump(const EventPump&) = delete;
    EventPump& operator=(const EventPump&) = delete;

    /// Poll one queued event. Returns false when the queue is empty (outEvent is reset to None).
    bool pollEvent(PlatformEvent& outEvent);

    /// Inspect the front queued event without removing it. Returns false when empty.
    bool peekEvent(PlatformEvent& outEvent) const;

    /// Inspect only the front event type without removing it. Returns false when empty.
    bool peekEventType(PlatformEventType& outType) const;

    /// True when the synthetic queue holds at least one event.
    bool hasPendingEvents() const;

    /// Number of events waiting in the synthetic queue (0 when empty).
    u32 pendingEventCount() const;

    /// True when a `WindowResized` event for `window` is still queued.
    bool hasPendingResizeFor(const Window& window) const;

    /// True when at least one queued event matches `type`.
    bool hasPendingEventOfType(PlatformEventType type) const;

    /// Number of queued events whose `window` pointer matches `window`.
    u32 countPendingEventsFor(const Window& window) const;

    /// Pending resize dimensions for `window`, or `pending == false` when none queued.
    ///
    /// Zero width or height on a queued resize is treated as invalid and ignored.
    PendingResizeExtent pendingResizeExtentFor(const Window& window) const;

    /// Most recent in-place resize coalesce (invalid when none have occurred).
    const ResizeCoalesceRecord& lastCoalescedResize() const;

    /// Aggregate queue + overflow/coalesce counters.
    EventPumpStats stats() const;

    /// Number of synthetic events dropped because the ring buffer was full.
    u32 droppedEventCount() const;

    /// Number of `WindowResized` events merged in-place via coalescing.
    u32 coalescedResizeCount() const;

    /// Drain the OS event queue — no-op in the B1.7 stub.
    void processOsEvents();

    /// Pump OS events then poll until the queue is empty. Returns false when quit was requested.
    bool pumpOnce();

    /// Move all queued synthetic events into `out` (FIFO order). Returns count moved.
    u32 drainEvents(std::vector<PlatformEvent>& out);

    /// Test / headless hook — enqueue a synthetic event.
    ///
    /// Pending `WindowResized` events for the same `window` pointer are coalesced
    /// in-place (latest width/height wins) instead of enqueueing duplicates.
    void pushSyntheticEvent(const PlatformEvent& event);

    /// Stub helpers — enqueue window lifecycle events for tests and headless runners.
    void pushWindowResized(Window& window);
    void pushWindowFocusGained(Window& window);
    void pushWindowFocusLost(Window& window);
    void pushWindowCloseRequested(Window& window);

    void requestQuit();
    bool quitRequested() const;
    void resetQuit();

    void clearSyntheticEvents();

    /// Reset overflow/coalesce counters without touching the queued events.
    void resetEventStats();

private:
    bool tryCoalescePendingResize_(const PlatformEvent& event);
    void enqueueSyntheticEvent_(const PlatformEvent& event);
    bool m_quitRequested = false;
    u32 m_syntheticHead = 0;
    u32 m_syntheticTail = 0;
    u32 m_droppedEventCount = 0;
    u32 m_coalescedResizeCount = 0;
    ResizeCoalesceRecord m_lastCoalescedResize{};
    static constexpr u32 kMaxSyntheticEvents = 32;
    PlatformEvent m_syntheticEvents[kMaxSyntheticEvents]{};
};

} // namespace fuse::platform
