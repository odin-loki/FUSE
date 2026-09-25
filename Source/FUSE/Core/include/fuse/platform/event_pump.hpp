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
    KeyDown,
    KeyUp,
    MouseMove,
    MouseButtonDown,
    MouseButtonUp,
    RawMouseDelta,
    /// A window's `InputCaptureMode` changed; `inputCaptured` holds the new state. Raw mouse
    /// input (WM_INPUT / XI_RawMotion) only flows while captured, so `InputState` uses this to
    /// switch its delta source back to `MouseMove` on release.
    InputCaptureChanged,
};

struct PlatformEvent {
    PlatformEventType type = PlatformEventType::None;
    Window* window = nullptr;
    u32 width = 0;
    u32 height = 0;
    u32 keyCode = 0;  // virtual key / USB-ish; 0 means unused
    i32 mouseX = 0;  // client X, or signed raw delta when type is RawMouseDelta
    i32 mouseY = 0;  // client Y, or signed raw delta when type is RawMouseDelta
    u8 mouseButton = 0;  // 1=left 2=right 3=middle
    bool inputCaptured = false;  // InputCaptureChanged: true = Captured, false = Released
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
    bool hasPendingResizeEvent = false;
    bool lastCoalescedResizeValid = false;
    PlatformEventType frontEventType = PlatformEventType::None;
};

/// OS event pump — synthetic queue plus optional native drain.
///
/// `processOsEvents()` is a no-op until a `Window` registers a non-null native
/// handle (HWND on Win32 via `Window::setNativeHandleForPump`). Headless tests
/// that never attach a native window keep a pure synthetic queue.
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

    /// Peek the front event only when its type matches `type`. Returns false when empty or
    /// the front event is a different type (outEvent is reset to None).
    bool tryPeekEventOfType(PlatformEventType type, PlatformEvent& outEvent) const;

    /// Peek the front event only when its `window` matches. Returns false when empty or the
    /// front event targets a different window (outEvent is reset to None).
    bool tryPeekEventFor(const Window& window, PlatformEvent& outEvent) const;

    /// Peek the front event only when its type and `window` match. Returns false when empty or
    /// the front event does not match (outEvent is reset to None).
    bool tryPeekEventOfTypeFor(const Window& window, PlatformEventType type,
                               PlatformEvent& outEvent) const;

    /// Poll the front event only when its type matches `type`. Returns false when empty or
    /// the front event is a different type (outEvent is reset to None).
    bool tryPollEventOfType(PlatformEventType type, PlatformEvent& outEvent);

    /// Poll the front event only when its type and `window` match. Returns false when empty or
    /// the front event does not match (outEvent is reset to None).
    bool tryPollEventOfTypeFor(const Window& window, PlatformEventType type,
                               PlatformEvent& outEvent);

    /// True when the front queued event matches `type` (false when empty).
    bool frontEventTypeIs(PlatformEventType type) const;

    /// True when the front queued event matches both `window` and `type` (false when empty).
    bool frontEventIsFor(const Window& window, PlatformEventType type) const;

    /// True when the synthetic queue holds at least one event.
    bool hasPendingEvents() const;

    /// Number of events waiting in the synthetic queue (0 when empty).
    u32 pendingEventCount() const;

    /// True when a `WindowResized` event for `window` is still queued.
    bool hasPendingResizeFor(const Window& window) const;

    /// True when at least one queued event matches `type`.
    bool hasPendingEventOfType(PlatformEventType type) const;

    /// True when a queued event matches both `window` and `type`.
    bool hasPendingEventOfTypeFor(const Window& window, PlatformEventType type) const;

    /// True when at least one queued event targets `window`.
    bool hasPendingEventsFor(const Window& window) const;

    /// Number of queued events whose `window` pointer matches `window`.
    u32 countPendingEventsFor(const Window& window) const;

    /// Number of queued events whose `type` matches `type`.
    u32 countPendingEventsOfType(PlatformEventType type) const;

    /// Number of queued events matching both `window` and `type`.
    u32 countPendingEventsOfTypeFor(const Window& window, PlatformEventType type) const;

    /// True when `pushSyntheticEvent(event)` would coalesce an existing resize instead of enqueueing.
    bool wouldCoalesceResize(const PlatformEvent& event) const;

    /// True when `pushWindowResized(window)` would coalesce instead of enqueueing.
    bool wouldCoalesceResizeFor(const Window& window) const;

    /// Pending resize dimensions for `window`, or `pending == false` when none queued.
    ///
    /// Zero width or height on a queued resize is treated as invalid and ignored.
    PendingResizeExtent pendingResizeExtentFor(const Window& window) const;

    /// Most recent in-place resize coalesce (invalid when none have occurred).
    const ResizeCoalesceRecord& lastCoalescedResize() const;

    /// True when the most recent in-place resize coalesce targeted `window`.
    bool hasCoalescedResizeFor(const Window& window) const;

    /// Aggregate queue + overflow/coalesce counters.
    EventPumpStats stats() const;

    /// Number of synthetic events dropped because the ring buffer was full.
    u32 droppedEventCount() const;

    /// Number of `WindowResized` events merged in-place via coalescing.
    u32 coalescedResizeCount() const;

    /// Drain native OS messages into the synthetic queue.
    ///
    /// Win32: PeekMessage loop for registered HWND pumps (KeyDown/Up, MouseMove,
    /// MouseButton, RawMouseDelta from WM_INPUT, WindowCloseRequested, WindowResized,
    /// Quit). X11: Xlib events for registered windows, plus RawMouseDelta from XInput2
    /// XI_RawMotion (pre-acceleration device counts) while a focused window is captured.
    /// GLFW: glfwPollEvents when WSI is available. No-op when no native window is registered.
    ///
    /// Capture changes made with `Window::setInputCapture` without a pump are reported here as
    /// `InputCaptureChanged` (before the OS messages) for every registered native window.
    ///
    /// Key/Mouse mapping in `enqueueMappedOsMessage` is dropped when
    /// `requireCaptureForInput()` is true and the target window is
    /// `InputCaptureMode::Released`. Window lifecycle / Quit are not filtered.
    void processOsEvents();

    /// Process-wide policy: OS-pumped Key/Mouse events require `Window` capture.
    /// Default false so headless tests keep enqueueing regardless of capture mode.
    void setRequireCaptureForInput(bool require);
    bool requireCaptureForInput() const;

    /// Pump OS events then poll until the queue is empty. Returns false when quit was requested.
    bool pumpOnce();

    /// Move all queued synthetic events into `out` (FIFO order). Returns count moved.
    u32 drainEvents(std::vector<PlatformEvent>& out);

    /// Test / headless hook — enqueue a synthetic event.
    ///
    /// Bypasses input-capture filtering. Capture only applies to OS-pumped Key/Mouse
    /// mapping inside `processOsEvents` (`enqueueMappedOsMessage`).
    ///
    /// Pending `WindowResized` events for the same `window` pointer are coalesced
    /// in-place (latest width/height wins) instead of enqueueing duplicates.
    void pushSyntheticEvent(const PlatformEvent& event);

    /// Stub helpers — enqueue window lifecycle events for tests and headless runners.
    void pushWindowResized(Window& window);
    void pushWindowFocusGained(Window& window);
    void pushWindowFocusLost(Window& window);
    void pushWindowCloseRequested(Window& window);
    /// Enqueue `InputCaptureChanged` for `window`'s current capture mode.
    void pushInputCaptureChanged(Window& window);

    /// Headless input helpers — wrap `pushSyntheticEvent` for key/mouse smoke.
    void pushKeyDown(u32 keyCode);
    void pushKeyUp(u32 keyCode);
    void pushMouseMove(i32 x, i32 y);
    void pushMouseButton(u8 button, bool down, i32 x, i32 y);

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
