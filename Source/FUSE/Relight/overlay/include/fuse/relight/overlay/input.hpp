// FUSE Relight RL-6.1: the developer overlay's input path.
//
// The tap subclasses the device window's WndProc (win32_hook.hpp). Every window message goes through
// InputHookCore::handle() first, on whichever thread the game pumps messages:
//   * Alt+X (WM_SYSKEYDOWN 'X' with the context bit, not a repeat) queues a Toggle; that chord's WM_SYSCHAR /
//     WM_SYSKEYUP are swallowed too (Remix's menu key behaves the same);
//   * overlay hidden: every other message is forwarded to the game's WndProc unchanged (counted);
//   * overlay shown: keyboard (WM_KEYFIRST..WM_KEYLAST), mouse (WM_MOUSEFIRST..WM_MOUSELAST) and WM_INPUT messages
//     are consumed (the game does not see them) and the ones the menu uses become InputEvents, mouse positions
//     mapped from client to back-buffer pixels; everything else is forwarded.
// Events wait in a fixed-capacity queue (no allocation) until the tap drains it at Present.
//
// translateMessage() is the pure part (no <windows.h>; the Win32 values are spelled out below) so the rules are
// tested on every platform.
#pragma once

#include <fuse/relight/overlay/ui.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace fuse::relight::overlay {

namespace win32 {
inline constexpr std::uint32_t kWmKillFocus = 0x0008;
inline constexpr std::uint32_t kWmInput = 0x00FF;
inline constexpr std::uint32_t kWmKeyDown = 0x0100;
inline constexpr std::uint32_t kWmKeyUp = 0x0101;
inline constexpr std::uint32_t kWmChar = 0x0102;
inline constexpr std::uint32_t kWmSysKeyDown = 0x0104;
inline constexpr std::uint32_t kWmSysKeyUp = 0x0105;
inline constexpr std::uint32_t kWmSysChar = 0x0106;
inline constexpr std::uint32_t kWmKeyLast = 0x0109;
inline constexpr std::uint32_t kWmMouseMove = 0x0200;
inline constexpr std::uint32_t kWmLButtonDown = 0x0201;
inline constexpr std::uint32_t kWmLButtonUp = 0x0202;
inline constexpr std::uint32_t kWmLButtonDblClk = 0x0203;
inline constexpr std::uint32_t kWmMouseWheel = 0x020A;
inline constexpr std::uint32_t kWmMouseLast = 0x020E;
inline constexpr std::int64_t kAltContextBit = 1ll << 29; ///< lParam: ALT held (WM_SYSKEY*)
inline constexpr std::int64_t kRepeatBit = 1ll << 30;     ///< lParam: key was down before
inline constexpr std::uint64_t kVkX = 'X';

inline constexpr std::int64_t makeLParam(std::int32_t x, std::int32_t y) {
    return static_cast<std::int64_t>((static_cast<std::uint32_t>(x) & 0xffffu) |
                                     ((static_cast<std::uint32_t>(y) & 0xffffu) << 16));
}
} // namespace win32

/// Client-area pixels -> back-buffer pixels.
struct CoordMap {
    std::int32_t clientW = 0, clientH = 0; ///< 0: identity
    std::int32_t frameW = 0, frameH = 0;
    std::int32_t x(std::int32_t cx) const { return clientW > 0 && frameW > 0 ? cx * frameW / clientW : cx; }
    std::int32_t y(std::int32_t cy) const { return clientH > 0 && frameH > 0 ? cy * frameH / clientH : cy; }
    /// The inverse (scripts address back-buffer pixels).
    std::int32_t toClientX(std::int32_t fx) const { return clientW > 0 && frameW > 0 ? fx * clientW / frameW : fx; }
    std::int32_t toClientY(std::int32_t fy) const { return clientH > 0 && frameH > 0 ? fy * clientH / frameH : fy; }
};

struct Translation {
    bool consume = false; ///< the game's WndProc does not see the message
    InputEvent event;     ///< type None: nothing for the menu
};

/// What the hook does with one message (see the header comment).
Translation translateMessage(std::uint32_t msg, std::uint64_t wParam, std::int64_t lParam, bool visible,
                             const CoordMap& map);

/// Thread-safe front end of the hook: counters, visibility and the event queue.
class InputHookCore {
public:
    static constexpr std::size_t kQueueCapacity = 256;

    /// A window message (any thread). True: consumed (do not call the game's WndProc).
    bool handle(std::uint32_t msg, std::uint64_t wParam, std::int64_t lParam);

    void setVisible(bool visible) { m_visible.store(visible, std::memory_order_release); }
    bool visible() const { return m_visible.load(std::memory_order_acquire); }
    void setCoordMap(const CoordMap& map);
    CoordMap coordMap() const;

    /// Moves queued events to `out` (at most `capacity`), oldest first; returns the count.
    std::size_t drain(InputEvent* out, std::size_t capacity);

    std::uint64_t forwarded() const { return m_forwarded.load(std::memory_order_relaxed); }
    std::uint64_t consumed() const { return m_consumed.load(std::memory_order_relaxed); }
    std::uint64_t dropped() const { return m_dropped.load(std::memory_order_relaxed); }
    std::uint64_t toggles() const { return m_toggles.load(std::memory_order_relaxed); }

private:
    mutable std::mutex m_mutex;
    std::array<InputEvent, kQueueCapacity> m_queue{};
    std::size_t m_head = 0, m_count = 0;
    CoordMap m_map;
    std::atomic<bool> m_visible{false};
    std::atomic<std::uint64_t> m_forwarded{0}, m_consumed{0}, m_dropped{0}, m_toggles{0};
};

} // namespace fuse::relight::overlay
