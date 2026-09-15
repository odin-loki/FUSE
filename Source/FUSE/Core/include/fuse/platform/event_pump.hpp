#pragma once

#include <fuse/types.hpp>

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

    /// Drain the OS event queue — no-op in the B1.7 stub.
    void processOsEvents();

    /// Pump OS events then poll until the queue is empty. Returns false when quit was requested.
    bool pumpOnce();

    /// Test / headless hook — enqueue a synthetic event.
    void pushSyntheticEvent(const PlatformEvent& event);

    void requestQuit();
    bool quitRequested() const;
    void resetQuit();

    void clearSyntheticEvents();

private:
    bool m_quitRequested = false;
    u32 m_syntheticHead = 0;
    u32 m_syntheticTail = 0;
    static constexpr u32 kMaxSyntheticEvents = 32;
    PlatformEvent m_syntheticEvents[kMaxSyntheticEvents]{};
};

} // namespace fuse::platform
