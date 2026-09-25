// FUSE Relight RL-6.1: the tap's window hook (see win32_hook.hpp).
#include <fuse/relight/overlay/win32_hook.hpp>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <mutex>

namespace fuse::relight::overlay {

namespace {

struct HookState {
    std::mutex mutex;
    HWND window = nullptr;
    WNDPROC original = nullptr;
    bool unicode = false;
    const WindowHook* owner = nullptr;
    std::atomic<InputHookCore*> core{nullptr};
};

HookState& state() {
    static HookState s;
    return s;
}

LRESULT CALLBACK overlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    HookState& s = state();
    InputHookCore* core = s.core.load(std::memory_order_acquire);
    if (core && core->handle(static_cast<std::uint32_t>(msg), static_cast<std::uint64_t>(wParam),
                             static_cast<std::int64_t>(lParam))) {
        return 0;
    }
    WNDPROC original = s.original;
    const bool unicode = s.unicode;
    if (msg == WM_NCDESTROY) {
        // The window goes: nothing may call into it (or restore its WndProc) afterwards.
        std::lock_guard<std::mutex> lock(s.mutex);
        s.core.store(nullptr, std::memory_order_release);
        s.window = nullptr;
    }
    if (!original) {
        return unicode ? DefWindowProcW(hwnd, msg, wParam, lParam) : DefWindowProcA(hwnd, msg, wParam, lParam);
    }
    return unicode ? CallWindowProcW(original, hwnd, msg, wParam, lParam)
                   : CallWindowProcA(original, hwnd, msg, wParam, lParam);
}

} // namespace

bool WindowHook::install(std::uint64_t hwndValue, InputHookCore* core) {
    remove();
    HWND hwnd = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(hwndValue));
    if (!hwnd || !IsWindow(hwnd) || !core) {
        return false;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId()) {
        return false;
    }
    HookState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.owner && s.owner != this) {
        return false;
    }
    if (s.window == hwnd && s.original) {
        // Our WndProc is still in this window's chain (an earlier device): reuse it.
        s.core.store(core, std::memory_order_release);
        s.owner = this;
        m_window = hwndValue;
        m_installed = true;
        return true;
    }
    const bool unicode = IsWindowUnicode(hwnd) != FALSE;
    s.unicode = unicode;
    s.core.store(core, std::memory_order_release);
    const LONG_PTR ours = reinterpret_cast<LONG_PTR>(&overlayWndProc);
    const LONG_PTR previous =
        unicode ? SetWindowLongPtrW(hwnd, GWLP_WNDPROC, ours) : SetWindowLongPtrA(hwnd, GWLP_WNDPROC, ours);
    if (previous == 0) {
        s.core.store(nullptr, std::memory_order_release);
        return false;
    }
    s.original = reinterpret_cast<WNDPROC>(previous);
    s.window = hwnd;
    s.owner = this;
    m_window = hwndValue;
    m_installed = true;
    return true;
}

void WindowHook::remove() {
    if (!m_installed) {
        return;
    }
    HookState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    m_installed = false;
    if (s.owner != this) {
        return;
    }
    s.owner = nullptr;
    s.core.store(nullptr, std::memory_order_release);
    if (!s.window || !IsWindow(s.window)) {
        s.window = nullptr;
        s.original = nullptr;
        return;
    }
    const LONG_PTR ours = reinterpret_cast<LONG_PTR>(&overlayWndProc);
    const LONG_PTR current =
        s.unicode ? GetWindowLongPtrW(s.window, GWLP_WNDPROC) : GetWindowLongPtrA(s.window, GWLP_WNDPROC);
    if (current == ours) {
        const LONG_PTR original = reinterpret_cast<LONG_PTR>(s.original);
        if (s.unicode) {
            SetWindowLongPtrW(s.window, GWLP_WNDPROC, original);
        } else {
            SetWindowLongPtrA(s.window, GWLP_WNDPROC, original);
        }
        s.window = nullptr;
        s.original = nullptr;
    }
    // Else another subclass sits on top of ours: ours stays as a pure forwarder (core null) for this window.
}

bool WindowHook::clientSize(std::int32_t& width, std::int32_t& height) const {
    if (!m_installed) {
        return false;
    }
    RECT r{};
    if (!GetClientRect(reinterpret_cast<HWND>(static_cast<std::uintptr_t>(m_window)), &r)) {
        return false;
    }
    width = static_cast<std::int32_t>(r.right - r.left);
    height = static_cast<std::int32_t>(r.bottom - r.top);
    return width > 0 && height > 0;
}

void WindowHook::send(InputHookCore& core, std::uint32_t msg, std::uint64_t wParam, std::int64_t lParam) {
    if (!m_installed) {
        core.handle(msg, wParam, lParam);
        return;
    }
    HWND hwnd = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(m_window));
    if (state().unicode) {
        SendMessageW(hwnd, msg, static_cast<WPARAM>(wParam), static_cast<LPARAM>(lParam));
    } else {
        SendMessageA(hwnd, msg, static_cast<WPARAM>(wParam), static_cast<LPARAM>(lParam));
    }
}

} // namespace fuse::relight::overlay

#else // !_WIN32

namespace fuse::relight::overlay {

bool WindowHook::install(std::uint64_t, InputHookCore*) {
    m_installed = false;
    return false;
}
void WindowHook::remove() { m_installed = false; }
bool WindowHook::clientSize(std::int32_t&, std::int32_t&) const { return false; }
void WindowHook::send(InputHookCore& core, std::uint32_t msg, std::uint64_t wParam, std::int64_t lParam) {
    core.handle(msg, wParam, lParam);
}

} // namespace fuse::relight::overlay

#endif
