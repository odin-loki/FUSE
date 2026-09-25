// FUSE Relight RL-6.1: the tap's window hook (WndProc subclass of the D3D9 device window).
//
// install() replaces the window's WndProc (SetWindowLongPtr, the A or W flavour the window was created with) by one
// that asks InputHookCore::handle() first and calls the game's WndProc (CallWindowProc) for every message the core
// does not consume (input.hpp). remove() restores the game's WndProc when ours is still the installed one; when the
// game (or another hook) subclassed the window after us, ours stays in the chain as a pure forwarder. One window per
// process (the first device that asks; D3D9 games render into one window).
//
// Not Windows (Linux-native unit tests): install() fails and send() hands messages straight to the core.
#pragma once

#include <fuse/relight/overlay/input.hpp>

#include <cstdint>

namespace fuse::relight::overlay {

class WindowHook {
public:
    WindowHook() = default;
    ~WindowHook() { remove(); }
    WindowHook(const WindowHook&) = delete;
    WindowHook& operator=(const WindowHook&) = delete;

    /// Subclasses `hwnd` (HWND value). False when there is no window, it is not a window of this process, or
    /// another WindowHook owns the process's hook.
    bool install(std::uint64_t hwnd, InputHookCore* core);
    void remove();
    bool installed() const { return m_installed; }
    std::uint64_t window() const { return m_window; }
    /// The window's client size (GetClientRect). False without a hooked window.
    bool clientSize(std::int32_t& width, std::int32_t& height) const;
    /// Delivers a message the way the game's message loop does (SendMessage: synchronously through the subclassed
    /// WndProc, so an unconsumed one reaches the game's WndProc). Without a hooked window: straight to `core`.
    void send(InputHookCore& core, std::uint32_t msg, std::uint64_t wParam, std::int64_t lParam);

private:
    std::uint64_t m_window = 0;
    bool m_installed = false;
};

} // namespace fuse::relight::overlay
