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
// Ported from dxvk-remix bridge/src/client/di_hook.cpp@0867d3c (class DirectInputForwarder)

// FUSE Relight RL-2.2: DirectInput -> window-message translation (the di_hook semantics).
//
// Semantics kept from upstream DirectInputForwarder: a game that reads the keyboard and mouse
// through DirectInput never sends the renderer window messages, so the client diffs every
// GetDeviceState / GetDeviceData result against the last known state and synthesizes
// WM_KEYDOWN/WM_KEYUP (+ WM_CHAR for keys with a one-character translation), WM_MOUSEMOVE with the
// accumulated (relative mode) or absolute cursor clamped to the window, WM_[LR]BUTTON{DOWN,UP} and
// WM_MOUSEWHEEL. Forwarding needs the device to be in exclusive mode (assumed until the game calls
// SetCooperativeLevel) and the per-device policy (Never / while the renderer UI is inactive / while
// it is active / Always) to allow it; mouse state is not tracked while the UI is active; DIK_INSERT
// (scan code 210) maps to VK_INSERT without the OS table; window extents under 16 pixels are
// ignored (game overlays with zero-sized DirectInput windows).
//
// Changes (revamp):
// - Pure logic with no Windows or DirectInput headers: the scan-code and character translation are
//   injected (MapVirtualKeyEx / ToAscii in the client, tables in the unit test), messages go to a
//   sink, and state is per instance instead of class statics, so it is unit tested on Linux too.
// - The buffered path (GetDeviceData) feeds per-object events instead of rebuilding a partial
//   DIMOUSESTATE / a static 256-byte array per call.
#pragma once

#include <array>
#include <cstdint>
#include <functional>

namespace fuse::relight::bridge::client {

struct WindowMessage {
    uint32_t msg = 0;
    uint64_t wParam = 0;
    int64_t lParam = 0;
};

enum class InputDevice : uint32_t { Mouse = 0, Keyboard = 1 };

// Upstream DI::ForwardPolicy (client option forwardDirectInput{Mouse,Keyboard}Policy).
enum class ForwardPolicy : uint32_t { Never = 0, UiInactive = 1, UiActive = 2, Always = 3 };

struct MouseInput {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    std::array<uint8_t, 8> buttons {};
};

struct InputTranslation {
    // DirectInput scan code -> virtual key (0: no translation). Client: MapVirtualKeyExA(MAPVK_VSC_TO_VK).
    std::function<uint32_t(uint32_t scanCode)> scanToVk;
    // Character of a key-down, or -1 without a one-character translation. Client: ToAscii.
    std::function<int32_t(uint32_t vk, uint32_t scanCode, const uint8_t* keyState)> toChar;
};

class DirectInputTranslator {
public:
    using Sink = std::function<void(const WindowMessage&)>;

    static constexpr uint32_t kWM_KEYDOWN = 0x0100;
    static constexpr uint32_t kWM_KEYUP = 0x0101;
    static constexpr uint32_t kWM_CHAR = 0x0102;
    static constexpr uint32_t kWM_MOUSEMOVE = 0x0200;
    static constexpr uint32_t kWM_LBUTTONDOWN = 0x0201;
    static constexpr uint32_t kWM_LBUTTONUP = 0x0202;
    static constexpr uint32_t kWM_RBUTTONDOWN = 0x0204;
    static constexpr uint32_t kWM_RBUTTONUP = 0x0205;
    static constexpr uint32_t kWM_MOUSEWHEEL = 0x020A;
    // DIMOFS_* offsets of the buffered mouse path
    static constexpr uint32_t kOfsX = 0, kOfsY = 4, kOfsZ = 8, kOfsButton0 = 12, kOfsButton1 = 13;

    DirectInputTranslator(Sink sink, InputTranslation translation);

    void setPolicy(InputDevice device, ForwardPolicy policy) noexcept { policy_[idx(device)] = policy; }
    void setExclusive(InputDevice device, bool exclusive) noexcept { exclusive_[idx(device)] = exclusive; }
    bool exclusive(InputDevice device) const noexcept { return exclusive_[idx(device)]; }
    void setUiActive(bool active) noexcept { uiActive_ = active; }
    bool uiActive() const noexcept { return uiActive_; }
    // Window extent used to clamp the synthesized cursor; values <= 16 are ignored.
    void setWindowSize(int32_t width, int32_t height) noexcept;
    int32_t windowWidth() const noexcept { return windowWidth_; }
    int32_t windowHeight() const noexcept { return windowHeight_; }

    // GetDeviceState(256 bytes) of a keyboard.
    void keyboardState(const uint8_t* keyState256);
    // GetDeviceData of a keyboard: one DIDEVICEOBJECTDATA (dwOfs = scan code, dwData = state byte).
    void keyboardEvent(uint32_t scanCode, uint32_t data);
    // GetDeviceState(DIMOUSESTATE / DIMOUSESTATE2) of a mouse.
    void mouseState(const MouseInput& state, bool absoluteAxes);
    // GetDeviceData of a mouse: one DIDEVICEOBJECTDATA (DIMOFS_* offset, value).
    void mouseEvent(uint32_t offset, uint32_t data, bool absoluteAxes);
    // Upstream resetMouseState (called when the renderer UI closes).
    void resetMouse() noexcept;

    int32_t cursorX() const noexcept { return mouseX_; }
    int32_t cursorY() const noexcept { return mouseY_; }

private:
    static constexpr size_t idx(InputDevice d) noexcept { return static_cast<size_t>(d); }
    bool allowed(InputDevice device) const noexcept;
    void forward(const WindowMessage& m);

    Sink sink_;
    InputTranslation translation_;
    std::array<ForwardPolicy, 2> policy_ {ForwardPolicy::UiActive, ForwardPolicy::UiActive};
    std::array<bool, 2> exclusive_ {true, true};
    bool uiActive_ = false;
    std::array<uint8_t, 256> keys_ {};
    std::array<uint8_t, 8> mouseButtons_ {};
    int32_t mouseX_ = 0;
    int32_t mouseY_ = 0;
    int32_t windowWidth_ = 3840;
    int32_t windowHeight_ = 2160;
    WindowMessage lastMove_ {};
    WindowMessage lastLButton_ {};
    WindowMessage lastRButton_ {};
    WindowMessage lastWheel_ {};
};

}  // namespace fuse::relight::bridge::client
