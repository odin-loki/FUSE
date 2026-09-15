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

    /// Poll one queued event. Returns false when the queue is empty.
    bool pollEvent(PlatformEvent& outEvent);

    /// True when the synthetic queue holds at least one event.
    bool hasPendingEvents() const;

    /// Number of events waiting in the synthetic queue (0 when empty).
    u32 pendingEventCount() const;

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

private:
    bool tryCoalescePendingResize_(const PlatformEvent& event);
    void enqueueSyntheticEvent_(const PlatformEvent& event);
    bool m_quitRequested = false;
    u32 m_syntheticHead = 0;
    u32 m_syntheticTail = 0;
    static constexpr u32 kMaxSyntheticEvents = 32;
    PlatformEvent m_syntheticEvents[kMaxSyntheticEvents]{};
};

} // namespace fuse::platform
