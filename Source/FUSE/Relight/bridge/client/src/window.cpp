/*
 * Copyright (c) 2022-2025, NVIDIA CORPORATION. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */
// Modifications Copyright (c) 2026 FUSE contributors (MIT)
// Ported from dxvk-remix bridge/src/client/{window.cpp,di_hook.cpp}@0867d3c

// FUSE Relight RL-2.2: window subclassing and input hooks (see window.hpp).
#include "window.hpp"

#include "connection.hpp"

#include <fuse/relight/bridge/client/input_translate.hpp>

#include <windowsx.h>

#include <atomic>
#include <cstring>
#include <map>
#include <mutex>

namespace fuse::relight::bridge::client {

namespace {

// ---- state -----------------------------------------------------------------------------------------
std::mutex g_mutex;  // window + hook bookkeeping (not the API lock: WndProcs run re-entrantly)
HWND g_hwnd = nullptr;
WNDPROC g_gameWndProc = nullptr;
D3DPRESENT_PARAMETERS g_pp {};
D3DDEVICE_CREATION_PARAMETERS g_cp {};
bool g_activateProcessed = false;
std::atomic<bool> g_uiActive {false};
std::atomic<uint32_t> g_forwarded {0}, g_swallowed {0}, g_diMessages {0}, g_diReads {0};
MessageSink g_sink = nullptr;
bool g_hooksInstalled = false;

// The real user32 functions, resolved by name: calls from this module must not go through an import
// table the hooks patched (the unit test links the client into the hooked executable).
struct User32 {
    decltype(&::SetWindowLongPtrA) setWindowLongPtrA;
    decltype(&::SetWindowLongPtrW) setWindowLongPtrW;
    decltype(&::GetWindowLongPtrA) getWindowLongPtrA;
    decltype(&::GetWindowLongPtrW) getWindowLongPtrW;
    decltype(&::GetCursorPos) getCursorPos;
    decltype(&::SetCursorPos) setCursorPos;
    decltype(&::GetAsyncKeyState) getAsyncKeyState;
    decltype(&::GetKeyState) getKeyState;
    decltype(&::GetKeyboardState) getKeyboardState;
    decltype(&::GetRawInputData) getRawInputData;
    decltype(&::GetRawInputBuffer) getRawInputBuffer;
};

template <class F>
F resolve(HMODULE m, const char* name) {
    return reinterpret_cast<F>(reinterpret_cast<void*>(::GetProcAddress(m, name)));
}

const User32& user32() {
    static const User32 u = [] {
        HMODULE m = ::GetModuleHandleA("user32.dll");
        User32 r;
#ifdef _WIN64
        r.setWindowLongPtrA = resolve<decltype(r.setWindowLongPtrA)>(m, "SetWindowLongPtrA");
        r.setWindowLongPtrW = resolve<decltype(r.setWindowLongPtrW)>(m, "SetWindowLongPtrW");
        r.getWindowLongPtrA = resolve<decltype(r.getWindowLongPtrA)>(m, "GetWindowLongPtrA");
        r.getWindowLongPtrW = resolve<decltype(r.getWindowLongPtrW)>(m, "GetWindowLongPtrW");
#else
        r.setWindowLongPtrA = resolve<decltype(r.setWindowLongPtrA)>(m, "SetWindowLongA");
        r.setWindowLongPtrW = resolve<decltype(r.setWindowLongPtrW)>(m, "SetWindowLongW");
        r.getWindowLongPtrA = resolve<decltype(r.getWindowLongPtrA)>(m, "GetWindowLongA");
        r.getWindowLongPtrW = resolve<decltype(r.getWindowLongPtrW)>(m, "GetWindowLongW");
#endif
        r.getCursorPos = resolve<decltype(r.getCursorPos)>(m, "GetCursorPos");
        r.setCursorPos = resolve<decltype(r.setCursorPos)>(m, "SetCursorPos");
        r.getAsyncKeyState = resolve<decltype(r.getAsyncKeyState)>(m, "GetAsyncKeyState");
        r.getKeyState = resolve<decltype(r.getKeyState)>(m, "GetKeyState");
        r.getKeyboardState = resolve<decltype(r.getKeyboardState)>(m, "GetKeyboardState");
        r.getRawInputData = resolve<decltype(r.getRawInputData)>(m, "GetRawInputData");
        r.getRawInputBuffer = resolve<decltype(r.getRawInputBuffer)>(m, "GetRawInputBuffer");
        return r;
    }();
    return u;
}

bool envFlag(const char* name) {
    char buf[16];
    const DWORD n = ::GetEnvironmentVariableA(name, buf, sizeof(buf));
    return n > 0 && n < sizeof(buf) && buf[0] == '1';
}

bool isInputMessage(UINT msg) {
    switch (msg) {
    case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP: case WM_SYSCHAR:
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: case WM_LBUTTONUP:
    case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: case WM_MBUTTONUP:
    case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: case WM_RBUTTONUP:
    case WM_MOUSEWHEEL: case WM_MOUSEMOVE: case WM_CHAR: case WM_UNICHAR:
    case WM_MOUSELEAVE: case WM_MOUSEHOVER: case WM_INPUT:
        return true;
    default:
        return false;
    }
}

void sendToHost(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    ++g_forwarded;
    if (g_sink != nullptr) {
        g_sink(hwnd, msg, wParam, lParam);
        return;
    }
    if (!bridge().alive()) {
        return;
    }
    FUSE_BRIDGE_LOCK();
    schema::cmd::Bridge_WindowMessage c;
    c.hwnd = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(hwnd));
    c.msg = msg;
    c.wParam = static_cast<uint64_t>(wParam);
    c.lParam = static_cast<int64_t>(lParam);
    bridge().send(c, 0, 0);
}

// Upstream window.cpp windowMsg: fullscreen window management.
void windowMsg(HWND hwnd, UINT msg, WPARAM wParam) {
    if (hwnd != g_hwnd || g_pp.Windowed || (g_cp.BehaviorFlags & D3DCREATE_NOWINDOWCHANGES)) {
        return;
    }
    if (msg == WM_ACTIVATEAPP) {
        if (wParam && !g_activateProcessed) {
            MONITORINFO mi {};
            mi.cbSize = sizeof(mi);
            ::GetMonitorInfoA(::MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
            ::SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, int(g_pp.BackBufferWidth),
                           int(g_pp.BackBufferHeight), SWP_NOACTIVATE | SWP_NOZORDER | SWP_ASYNCWINDOWPOS);
            g_activateProcessed = true;
        } else if (!wParam) {
            if (::IsWindowVisible(hwnd)) {
                ::ShowWindowAsync(hwnd, SW_MINIMIZE);
            }
            g_activateProcessed = false;
        }
    } else if (msg == WM_SIZE && !::IsIconic(hwnd)) {
        ::PostMessageW(hwnd, WM_ACTIVATEAPP, 1, LPARAM(::GetCurrentThreadId()));
    }
}

// Upstream window.cpp remixMsg: true = swallow.
bool bridgeMsg(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    const bool ui = g_uiActive.load(std::memory_order_acquire);
    if (ui) {
        if (msg == WM_MOUSELEAVE) {
            return true;
        }
        if (msg == WM_SYSCOMMAND &&
            (wParam == SC_MOVE || wParam == SC_SIZE || wParam == 0xF012 || wParam == SC_MINIMIZE || wParam == SC_MAXIMIZE)) {
            return true;
        }
        if ((msg & 0xFFA0) == 0x00A0 && wParam != HTCLOSE) {  // WM_NCMOUSEMOVE .. WM_NCXBUTTONDBLCLK
            return true;
        }
    }
    if (msg != WM_INPUT && (isInputMessage(msg) || msg == WM_ACTIVATEAPP || msg == WM_SIZE || msg == WM_KILLFOCUS ||
                            msg == WM_SETFOCUS)) {
        sendToHost(hwnd, msg, wParam, lParam);
    }
    if (ui && isInputMessage(msg) && !(msg == WM_KEYUP && wParam == VK_MENU)) {
        return true;
    }
    return false;
}

LRESULT CALLBACK bridgeWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    const bool unicode = ::IsWindowUnicode(hwnd);
    WNDPROC game;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        game = g_gameWndProc;
    }
    if (msg == WM_ACTIVATEAPP || msg == WM_SIZE) {
        windowMsg(hwnd, msg, wParam);
    }
    if (bridgeMsg(hwnd, msg, wParam, lParam)) {
        ++g_swallowed;
        return unicode ? ::DefWindowProcW(hwnd, msg, wParam, lParam) : ::DefWindowProcA(hwnd, msg, wParam, lParam);
    }
    if (msg == WM_DESTROY && hwnd == g_hwnd) {
        windowDetach();
    }
    if (game == nullptr) {
        return unicode ? ::DefWindowProcW(hwnd, msg, wParam, lParam) : ::DefWindowProcA(hwnd, msg, wParam, lParam);
    }
    return unicode ? ::CallWindowProcW(game, hwnd, msg, wParam, lParam) : ::CallWindowProcA(game, hwnd, msg, wParam, lParam);
}

// ---- import-table hooks: Set/GetWindowLong(Ptr) --------------------------------------------------------
template <bool Unicode>
LONG_PTR WINAPI hookSetWindowLongPtr(HWND hwnd, int index, LONG_PTR value) {
    if (index == GWLP_WNDPROC) {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_hwnd != nullptr && hwnd == g_hwnd && g_gameWndProc != nullptr) {
            const LONG_PTR old = reinterpret_cast<LONG_PTR>(g_gameWndProc);
            g_gameWndProc = reinterpret_cast<WNDPROC>(value);
            return old;
        }
    }
    return Unicode ? user32().setWindowLongPtrW(hwnd, index, value) : user32().setWindowLongPtrA(hwnd, index, value);
}
template <bool Unicode>
LONG_PTR WINAPI hookGetWindowLongPtr(HWND hwnd, int index) {
    if (index == GWLP_WNDPROC) {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_hwnd != nullptr && hwnd == g_hwnd && g_gameWndProc != nullptr) {
            return reinterpret_cast<LONG_PTR>(g_gameWndProc);
        }
    }
    return Unicode ? user32().getWindowLongPtrW(hwnd, index) : user32().getWindowLongPtrA(hwnd, index);
}
#ifndef _WIN64
// On x86 SetWindowLongPtr is SetWindowLong; the LONG variants are the same functions.
template <bool Unicode>
LONG WINAPI hookSetWindowLong(HWND hwnd, int index, LONG value) {
    return static_cast<LONG>(hookSetWindowLongPtr<Unicode>(hwnd, index, value));
}
template <bool Unicode>
LONG WINAPI hookGetWindowLong(HWND hwnd, int index) {
    return static_cast<LONG>(hookGetWindowLongPtr<Unicode>(hwnd, index));
}
#endif

// ---- import-table hooks: Win32 input (upstream di_hook.cpp AttachConventionalInput) ------------------------
POINT g_uiCursor {};
bool g_uiCursorValid = false;
POINT g_lastCursor {};

BOOL WINAPI hookGetCursorPos(LPPOINT p) {
    if (p == nullptr) {
        ::SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (g_uiActive.load()) {
        *p = g_uiCursorValid ? g_uiCursor : g_lastCursor;
        return TRUE;
    }
    if (user32().getCursorPos(p)) {
        g_lastCursor = *p;
        return TRUE;
    }
    return FALSE;
}
BOOL WINAPI hookSetCursorPos(int x, int y) {
    if (g_uiActive.load()) {
        g_uiCursor = POINT {x, y};
        g_uiCursorValid = true;
        return TRUE;
    }
    const BOOL r = user32().setCursorPos(x, y);
    if (r) {
        g_lastCursor = POINT {x, y};
    }
    return r;
}
SHORT WINAPI hookGetAsyncKeyState(int vk) { return g_uiActive.load() ? 0 : user32().getAsyncKeyState(vk); }
SHORT WINAPI hookGetKeyState(int vk) { return g_uiActive.load() ? 0 : user32().getKeyState(vk); }
BOOL WINAPI hookGetKeyboardState(PBYTE state) {
    if (g_uiActive.load() && state != nullptr) {
        std::memset(state, 0, 256);
        return TRUE;
    }
    return user32().getKeyboardState(state);
}
std::atomic<bool> g_gameUsesDirectInput {false};
UINT WINAPI hookGetRawInputData(HRAWINPUT raw, UINT command, LPVOID data, PUINT size, UINT header) {
    static RAWKEYBOARD lastKeyboard {};
    const UINT res = user32().getRawInputData(raw, command, data, size, header);
    if (data == nullptr || size == nullptr || command != RID_INPUT || res == 0 || res == UINT(-1)) {
        return res;
    }
    auto* r = static_cast<RAWINPUT*>(data);
    const size_t mouseEnd = offsetof(RAWINPUT, data) + sizeof(RAWMOUSE);
    const size_t keyboardEnd = offsetof(RAWINPUT, data) + sizeof(RAWKEYBOARD);
    if ((r->header.dwType == RIM_TYPEMOUSE && res < mouseEnd) || (r->header.dwType == RIM_TYPEKEYBOARD && res < keyboardEnd)) {
        return res;
    }
    if (g_uiActive.load()) {
        if (r->header.dwType == RIM_TYPEMOUSE) {
            if (!(r->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
                r->data.mouse.lLastX = 0;
                r->data.mouse.lLastY = 0;
            }
            r->data.mouse.usButtonFlags = 0;
            r->data.mouse.usButtonData = 0;
            r->data.mouse.ulRawButtons = 0;
        } else if (r->header.dwType == RIM_TYPEKEYBOARD && !g_gameUsesDirectInput.load()) {
            r->data.keyboard = lastKeyboard;
        }
        return res;
    }
    if (!g_gameUsesDirectInput.load() && r->header.dwType == RIM_TYPEKEYBOARD) {
        lastKeyboard = r->data.keyboard;
    }
    return res;
}
UINT WINAPI hookGetRawInputBuffer(PRAWINPUT data, PUINT size, UINT header) {
    if (data == nullptr || !g_uiActive.load()) {
        return user32().getRawInputBuffer(data, size, header);
    }
    UINT res = user32().getRawInputBuffer(data, size, header);
    while (res != 0 && res != UINT(-1)) {
        res = user32().getRawInputBuffer(data, size, header);
    }
    return res == UINT(-1) ? res : 0;
}

// ---- DirectInput (upstream di_hook.cpp DirectInputHookBase / DirectInput{7,8}Hook) ---------------------------
using PfnAcquire = HRESULT(STDMETHODCALLTYPE*)(void*);
using PfnGetDeviceState = HRESULT(STDMETHODCALLTYPE*)(void*, DWORD, LPVOID);
using PfnGetDeviceData = HRESULT(STDMETHODCALLTYPE*)(void*, DWORD, void*, LPDWORD, DWORD);
using PfnSetCooperativeLevel = HRESULT(STDMETHODCALLTYPE*)(void*, HWND, DWORD);
using PfnSetProperty = HRESULT(STDMETHODCALLTYPE*)(void*, REFGUID, const void*);
using PfnGetCapabilities = HRESULT(STDMETHODCALLTYPE*)(void*, void*);
using PfnCreateDevice = HRESULT(STDMETHODCALLTYPE*)(void*, REFGUID, void**, IUnknown*);

// Device vtable indices shared by IDirectInputDevice{,2,7,8}{A,W}.
constexpr int kVtGetCapabilities = 3, kVtSetProperty = 6, kVtAcquire = 7, kVtUnacquire = 8, kVtGetDeviceState = 9,
              kVtGetDeviceData = 10, kVtSetCooperativeLevel = 13;
constexpr int kVtCreateDevice = 3;  // IDirectInput{,2,7,8}{A,W}::CreateDevice

struct DeviceVtable {
    PfnGetCapabilities getCapabilities = nullptr;
    PfnSetProperty setProperty = nullptr;
    PfnAcquire acquire = nullptr;
    PfnAcquire unacquire = nullptr;
    PfnGetDeviceState getDeviceState = nullptr;
    PfnGetDeviceData getDeviceData = nullptr;
    PfnSetCooperativeLevel setCooperativeLevel = nullptr;
};
std::map<void**, DeviceVtable> g_deviceVtables;
std::map<void**, PfnCreateDevice> g_factoryVtables;
enum class DiKind { Unknown, Keyboard, Mouse };
std::map<void*, DiKind> g_devices;
std::map<void*, bool> g_mouseAbsolute;
std::map<void*, bool> g_mouseStateUsed;  // upstream MouseDeviceStateUsed: buffered path only without it
std::map<void*, bool> g_keyboardStateUsed;

DirectInputTranslator& translator() {
    static DirectInputTranslator t(
        [](const WindowMessage& m) {
            ++g_diMessages;
            sendToHost(g_hwnd, m.msg, static_cast<WPARAM>(m.wParam), static_cast<LPARAM>(m.lParam));
        },
        InputTranslation {
            [](uint32_t scan) { return static_cast<uint32_t>(::MapVirtualKeyExA(scan, MAPVK_VSC_TO_VK, nullptr)); },
            [](uint32_t vk, uint32_t scan, const uint8_t* ks) -> int32_t {
                WORD ch[2] = {0, 0};
                return ::ToAscii(vk, scan, ks, ch, 0) == 1 ? int32_t(ch[0]) : -1;
            }});
    return t;
}

const DeviceVtable* vtableOf(void* dev) {
    auto it = g_deviceVtables.find(*static_cast<void***>(dev));
    return it == g_deviceVtables.end() ? nullptr : &it->second;
}

DiKind kindOf(void* dev) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(dev);
    if (it != g_devices.end()) {
        return it->second;
    }
    DiKind k = DiKind::Unknown;
    const DeviceVtable* vt = vtableOf(dev);
    if (vt != nullptr && vt->getCapabilities != nullptr) {
        struct {
            DWORD dwSize, dwFlags, dwDevType, dwAxes, dwButtons, dwPOVs, dwFFSamplePeriod, dwFFMinTimeResolution,
                dwFirmwareRevision, dwHardwareRevision, dwFFDriverVersion;
        } caps {};
        caps.dwSize = sizeof(caps);  // DIDEVCAPS
        if (SUCCEEDED(vt->getCapabilities(dev, &caps))) {
            const DWORD type = caps.dwDevType & 0xFF;
            // DI8DEVTYPE_MOUSE 0x12 / KEYBOARD 0x13; DIDEVTYPE_MOUSE 2 / KEYBOARD 3 (DirectInput 7)
            if (type == 0x13 || type == 3) {
                k = DiKind::Keyboard;
            } else if (type == 0x12 || type == 2) {
                k = DiKind::Mouse;
            }
        }
    }
    g_devices[dev] = k;
    return k;
}

HRESULT STDMETHODCALLTYPE hookDiSetProperty(void* dev, REFGUID prop, const void* header) {
    const HRESULT hr = vtableOf(dev)->setProperty(dev, prop, header);
    // DIPROP_AXISMODE is MAKEDIPROP(2): the GUID reference is the integer 2.
    if (SUCCEEDED(hr) && reinterpret_cast<uintptr_t>(&prop) == 2 && header != nullptr) {
        const DWORD data = static_cast<const DWORD*>(header)[4];  // DIPROPDWORD::dwData
        std::lock_guard<std::mutex> lock(g_mutex);
        g_mouseAbsolute[dev] = data == 1;  // DIPROPAXISMODE_ABS
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hookDiAcquire(void* dev) {
    g_gameUsesDirectInput = true;
    kindOf(dev);
    return vtableOf(dev)->acquire(dev);
}

HRESULT STDMETHODCALLTYPE hookDiUnacquire(void* dev) { return vtableOf(dev)->unacquire(dev); }

HRESULT STDMETHODCALLTYPE hookDiSetCooperativeLevel(void* dev, HWND hwnd, DWORD flags) {
    if (envFlag("FUSE_RELIGHT_BRIDGE_DI_NONEXCLUSIVE")) {  // upstream client.DirectInput.disableExclusiveInput
        flags = (flags & ~DWORD(0x1)) | 0x2;               // DISCL_NONEXCLUSIVE
    }
    const bool exclusive = (flags & 0x1) != 0;  // DISCL_EXCLUSIVE
    const DiKind k = kindOf(dev);
    if (k == DiKind::Keyboard) {
        translator().setExclusive(InputDevice::Keyboard, exclusive);
    } else if (k == DiKind::Mouse) {
        translator().setExclusive(InputDevice::Mouse, exclusive);
    }
    return vtableOf(dev)->setCooperativeLevel(dev, hwnd, flags);
}

bool absoluteMouse(void* dev) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_mouseAbsolute.find(dev);
    return it != g_mouseAbsolute.end() && it->second;
}

HRESULT STDMETHODCALLTYPE hookDiGetDeviceState(void* dev, DWORD size, LPVOID data) {
    const HRESULT hr = vtableOf(dev)->getDeviceState(dev, size, data);
    ++g_diReads;
    if (hr != S_OK || data == nullptr) {
        return hr;
    }
    const DiKind k = kindOf(dev);
    if (k == DiKind::Keyboard && size == 256) {
        g_keyboardStateUsed[dev] = true;
        translator().keyboardState(static_cast<const uint8_t*>(data));
    } else if (k == DiKind::Mouse && (size == 16 || size == 20)) {  // DIMOUSESTATE / DIMOUSESTATE2
        g_mouseStateUsed[dev] = true;
        MouseInput m;
        const auto* p = static_cast<const uint8_t*>(data);
        std::memcpy(&m.x, p, 4);
        std::memcpy(&m.y, p + 4, 4);
        std::memcpy(&m.z, p + 8, 4);
        std::memcpy(m.buttons.data(), p + 12, size - 12);
        translator().mouseState(m, absoluteMouse(dev));
    }
    if (g_uiActive.load()) {
        std::memset(data, 0, size);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hookDiGetDeviceData(void* dev, DWORD objectSize, void* rgdod, LPDWORD inOut, DWORD flags) {
    const HRESULT hr = vtableOf(dev)->getDeviceData(dev, objectSize, rgdod, inOut, flags);
    ++g_diReads;
    if (rgdod == nullptr || inOut == nullptr) {
        return hr;
    }
    if (hr == S_OK) {
        const DiKind k = kindOf(dev);
        for (DWORD i = 0; i < *inOut; ++i) {
            const auto* o = static_cast<const uint8_t*>(rgdod) + size_t(i) * objectSize;
            DWORD ofs, value;
            std::memcpy(&ofs, o, 4);  // DIDEVICEOBJECTDATA::dwOfs, dwData
            std::memcpy(&value, o + 4, 4);
            if (k == DiKind::Mouse && !g_mouseStateUsed[dev]) {
                translator().mouseEvent(ofs, value, absoluteMouse(dev));
            } else if (k == DiKind::Keyboard && !g_keyboardStateUsed[dev]) {
                translator().keyboardEvent(ofs, value);
            }
        }
    }
    if (g_uiActive.load()) {
        std::memset(rgdod, 0, size_t(*inOut) * objectSize);
        *inOut = 0;
    }
    return hr;
}

bool writeVtable(void** slot, void* value) {
    DWORD old;
    if (!::VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
        return false;
    }
    *slot = value;
    ::VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}

HRESULT STDMETHODCALLTYPE hookDiCreateDevice(void* di, REFGUID guid, void** device, IUnknown* outer) {
    PfnCreateDevice orig;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        orig = g_factoryVtables[*static_cast<void***>(di)];
    }
    const HRESULT hr = orig(di, guid, device, outer);
    if (SUCCEEDED(hr) && device != nullptr && *device != nullptr) {
        hookDirectInputDevice(*device);
    }
    return hr;
}

void hookFactory(void* di) {
    void** vt = *static_cast<void***>(di);
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_factoryVtables.count(vt) != 0 || vt[kVtCreateDevice] == reinterpret_cast<void*>(&hookDiCreateDevice)) {
        return;
    }
    g_factoryVtables[vt] = reinterpret_cast<PfnCreateDevice>(vt[kVtCreateDevice]);
    writeVtable(&vt[kVtCreateDevice], reinterpret_cast<void*>(&hookDiCreateDevice));
}

using PfnDirectInput8Create = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
using PfnDirectInputCreate = HRESULT(WINAPI*)(HINSTANCE, DWORD, void**, LPUNKNOWN);

HRESULT WINAPI hookDirectInput8Create(HINSTANCE inst, DWORD version, REFIID riid, LPVOID* out, LPUNKNOWN outer) {
    static auto orig = reinterpret_cast<PfnDirectInput8Create>(
        reinterpret_cast<void*>(::GetProcAddress(::LoadLibraryA("dinput8.dll"), "DirectInput8Create")));
    g_gameUsesDirectInput = true;
    const HRESULT hr = orig ? orig(inst, version, riid, out, outer) : E_FAIL;
    if (SUCCEEDED(hr) && out != nullptr && *out != nullptr) {
        hookFactory(*out);
    }
    return hr;
}

template <bool Unicode>
HRESULT WINAPI hookDirectInputCreate(HINSTANCE inst, DWORD version, void** out, LPUNKNOWN outer) {
    static auto orig = reinterpret_cast<PfnDirectInputCreate>(reinterpret_cast<void*>(
        ::GetProcAddress(::LoadLibraryA("dinput.dll"), Unicode ? "DirectInputCreateW" : "DirectInputCreateA")));
    g_gameUsesDirectInput = true;
    const HRESULT hr = orig ? orig(inst, version, out, outer) : E_FAIL;
    if (SUCCEEDED(hr) && out != nullptr && *out != nullptr) {
        hookFactory(*out);
    }
    return hr;
}

template <class F>
void* fp(F f) {
    return reinterpret_cast<void*>(f);
}

}  // namespace

// ---- public ---------------------------------------------------------------------------------------------------

bool patchImport(HMODULE module, const char* dll, const char* func, void* hook) {
    if (module == nullptr) {
        return false;
    }
    auto* base = reinterpret_cast<uint8_t*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.VirtualAddress == 0) {
        return false;
    }
    bool patched = false;
    for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name != 0; ++imp) {
        if (::lstrcmpiA(reinterpret_cast<const char*>(base + imp->Name), dll) != 0) {
            continue;
        }
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        auto* funcs = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
        for (; names->u1.AddressOfData != 0; ++names, ++funcs) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) {
                continue;
            }
            auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(byName->Name), func) != 0) {
                continue;
            }
            patched = writeVtable(reinterpret_cast<void**>(&funcs->u1.Function), hook) || patched;
        }
    }
    return patched;
}

void installInputHooks(HMODULE m) {
    patchImport(m, "user32.dll", "GetCursorPos", fp(&hookGetCursorPos));
    patchImport(m, "user32.dll", "SetCursorPos", fp(&hookSetCursorPos));
    patchImport(m, "user32.dll", "GetAsyncKeyState", fp(&hookGetAsyncKeyState));
    patchImport(m, "user32.dll", "GetKeyState", fp(&hookGetKeyState));
    patchImport(m, "user32.dll", "GetKeyboardState", fp(&hookGetKeyboardState));
    patchImport(m, "user32.dll", "GetRawInputData", fp(&hookGetRawInputData));
    patchImport(m, "user32.dll", "GetRawInputBuffer", fp(&hookGetRawInputBuffer));
#ifdef _WIN64
    patchImport(m, "user32.dll", "SetWindowLongPtrA", fp(&hookSetWindowLongPtr<false>));
    patchImport(m, "user32.dll", "SetWindowLongPtrW", fp(&hookSetWindowLongPtr<true>));
    patchImport(m, "user32.dll", "GetWindowLongPtrA", fp(&hookGetWindowLongPtr<false>));
    patchImport(m, "user32.dll", "GetWindowLongPtrW", fp(&hookGetWindowLongPtr<true>));
#else
    patchImport(m, "user32.dll", "SetWindowLongA", fp(&hookSetWindowLong<false>));
    patchImport(m, "user32.dll", "SetWindowLongW", fp(&hookSetWindowLong<true>));
    patchImport(m, "user32.dll", "GetWindowLongA", fp(&hookGetWindowLong<false>));
    patchImport(m, "user32.dll", "GetWindowLongW", fp(&hookGetWindowLong<true>));
#endif
    patchImport(m, "dinput8.dll", "DirectInput8Create", fp(&hookDirectInput8Create));
    patchImport(m, "dinput.dll", "DirectInputCreateA", fp(&hookDirectInputCreate<false>));
    patchImport(m, "dinput.dll", "DirectInputCreateW", fp(&hookDirectInputCreate<true>));
}

void hookDirectInputDevice(void* device) {
    void** vt = *static_cast<void***>(device);
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_deviceVtables.count(vt) != 0) {
        return;
    }
    DeviceVtable o;
    o.getCapabilities = reinterpret_cast<PfnGetCapabilities>(vt[kVtGetCapabilities]);
    o.setProperty = reinterpret_cast<PfnSetProperty>(vt[kVtSetProperty]);
    o.acquire = reinterpret_cast<PfnAcquire>(vt[kVtAcquire]);
    o.unacquire = reinterpret_cast<PfnAcquire>(vt[kVtUnacquire]);
    o.getDeviceState = reinterpret_cast<PfnGetDeviceState>(vt[kVtGetDeviceState]);
    o.getDeviceData = reinterpret_cast<PfnGetDeviceData>(vt[kVtGetDeviceData]);
    o.setCooperativeLevel = reinterpret_cast<PfnSetCooperativeLevel>(vt[kVtSetCooperativeLevel]);
    g_deviceVtables[vt] = o;
    writeVtable(&vt[kVtSetProperty], fp(&hookDiSetProperty));
    writeVtable(&vt[kVtAcquire], fp(&hookDiAcquire));
    writeVtable(&vt[kVtUnacquire], fp(&hookDiUnacquire));
    writeVtable(&vt[kVtGetDeviceState], fp(&hookDiGetDeviceState));
    writeVtable(&vt[kVtGetDeviceData], fp(&hookDiGetDeviceData));
    writeVtable(&vt[kVtSetCooperativeLevel], fp(&hookDiSetCooperativeLevel));
}

void windowAttach(HWND hwnd, const D3DPRESENT_PARAMETERS& pp, const D3DDEVICE_CREATION_PARAMETERS& cp) {
    if (!g_hooksInstalled) {
        g_hooksInstalled = true;
        if (!envFlag("FUSE_RELIGHT_BRIDGE_NO_INPUT_HOOKS")) {
            installInputHooks(::GetModuleHandleA(nullptr));
        }
        translator().setPolicy(InputDevice::Mouse, ForwardPolicy::UiActive);
        translator().setPolicy(InputDevice::Keyboard, ForwardPolicy::UiActive);
    }
    if (hwnd == nullptr || !::IsWindow(hwnd) || envFlag("FUSE_RELIGHT_BRIDGE_NO_WNDPROC")) {
        return;
    }
    windowDetach();
    std::lock_guard<std::mutex> lock(g_mutex);
    g_pp = pp;
    g_cp = cp;
    g_hwnd = hwnd;
    g_activateProcessed = false;
    // The real SetWindowLongPtr (not our import hook): our procedure goes on top.
    g_gameWndProc = reinterpret_cast<WNDPROC>(
        user32().setWindowLongPtrA(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&bridgeWndProc)));
    // Upstream DInputSetDefaultWindow: assume exclusive input until SetCooperativeLevel says otherwise.
    translator().setExclusive(InputDevice::Keyboard, true);
    translator().setExclusive(InputDevice::Mouse, true);
    RECT r {};
    ::GetWindowRect(hwnd, &r);
    translator().setWindowSize(r.right - r.left, r.bottom - r.top);
}

void windowDetach() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_hwnd != nullptr && g_gameWndProc != nullptr && ::IsWindow(g_hwnd)) {
        const auto current = reinterpret_cast<WNDPROC>(user32().getWindowLongPtrA(g_hwnd, GWLP_WNDPROC));
        if (current == &bridgeWndProc) {
            user32().setWindowLongPtrA(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_gameWndProc));
        }
    }
    g_hwnd = nullptr;
    g_gameWndProc = nullptr;
}

void windowPump() {
    HWND hwnd;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        hwnd = g_hwnd;
    }
    if (hwnd != nullptr) {
        RECT r {};
        if (::GetWindowRect(hwnd, &r)) {
            translator().setWindowSize(r.right - r.left, r.bottom - r.top);
        }
    }
}

void inputSetUiActive(bool active) {
    const bool was = g_uiActive.exchange(active);
    translator().setUiActive(active);
    if (was == active) {
        return;
    }
    if (active) {
        // Upstream onRemixUIActivated: the game's cursor freezes where it was.
        g_uiCursor = g_lastCursor;
        g_uiCursorValid = true;
        HWND hwnd = g_hwnd;
        WNDPROC game = g_gameWndProc;
        if (hwnd != nullptr && game != nullptr) {
            // Unstick modifier keys on the game side (upstream remixMsg).
            for (UINT vk : {UINT(VK_CONTROL), UINT(VK_SHIFT), UINT(VK_INSERT)}) {
                ::CallWindowProcA(game, hwnd, WM_KEYUP, vk,
                                  LPARAM(((KF_REPEAT + KF_UP + ::MapVirtualKeyA(vk, MAPVK_VK_TO_VSC)) << 16) + 1));
            }
        }
    } else {
        g_uiCursorValid = false;
        translator().resetMouse();
    }
}

bool inputUiActive() { return g_uiActive.load(); }

WindowStats windowStats() {
    WindowStats s;
    s.forwarded = g_forwarded.load();
    s.swallowed = g_swallowed.load();
    s.directInputMessages = g_diMessages.load();
    s.directInputReads = g_diReads.load();
    return s;
}

void setMessageSinkForTest(MessageSink sink) { g_sink = sink; }

}  // namespace fuse::relight::bridge::client
