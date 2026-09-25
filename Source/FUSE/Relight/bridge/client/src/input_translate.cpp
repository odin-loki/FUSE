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

// FUSE Relight RL-2.2: DirectInput -> window-message translation. See input_translate.hpp.
#include <fuse/relight/bridge/client/input_translate.hpp>

#include <cstring>
#include <utility>

namespace fuse::relight::bridge::client {

namespace {
constexpr uint32_t kMK_LBUTTON = 0x0001;
constexpr uint32_t kMK_RBUTTON = 0x0002;
constexpr uint32_t kMK_SHIFT = 0x0004;
constexpr uint32_t kMK_CONTROL = 0x0008;
constexpr uint32_t kVK_INSERT = 0x2D;
constexpr uint32_t kDIK_INSERT = 210;
constexpr uint32_t kDIK_LCONTROL = 0x1D, kDIK_RCONTROL = 0x9D, kDIK_LSHIFT = 0x2A, kDIK_RSHIFT = 0x36;
constexpr uint32_t kWM_MOUSEFIRST = 0x0200, kWM_MOUSELAST = 0x020E;

bool sameMessage(const WindowMessage& a, const WindowMessage& b) {
    return a.msg == b.msg && a.wParam == b.wParam && a.lParam == b.lParam;
}

int16_t wheelDelta(uint64_t wParam) { return static_cast<int16_t>((wParam >> 16) & 0xFFFF); }
}  // namespace

DirectInputTranslator::DirectInputTranslator(Sink sink, InputTranslation translation)
    : sink_(std::move(sink)), translation_(std::move(translation)) {}

void DirectInputTranslator::setWindowSize(int32_t width, int32_t height) noexcept {
    // Upstream updateWindowSize: only accept reasonable extents.
    if (width > 16) {
        windowWidth_ = width;
    }
    if (height > 16) {
        windowHeight_ = height;
    }
}

bool DirectInputTranslator::allowed(InputDevice device) const noexcept {
    if (!exclusive_[idx(device)]) {
        return false;
    }
    const ForwardPolicy p = policy_[idx(device)];
    if (p == ForwardPolicy::Never) {
        return false;
    }
    if (p == ForwardPolicy::Always) {
        return true;
    }
    return (p == ForwardPolicy::UiActive) == uiActive_;
}

void DirectInputTranslator::forward(const WindowMessage& m) {
    const InputDevice dev = (m.msg >= kWM_MOUSEFIRST && m.msg <= kWM_MOUSELAST) ? InputDevice::Mouse : InputDevice::Keyboard;
    if (allowed(dev) && sink_) {
        sink_(m);
    }
}

void DirectInputTranslator::keyboardState(const uint8_t* ks) {
    for (uint32_t vsc = 0; vsc < 256; ++vsc) {
        if (keys_[vsc] == ks[vsc]) {
            continue;
        }
        const uint32_t vk = vsc == kDIK_INSERT ? kVK_INSERT : (translation_.scanToVk ? translation_.scanToVk(vsc) : 0);
        if (vk == 0) {
            continue;  // upstream: "unable to translate VSC" (the state byte is not recorded either)
        }
        WindowMessage m;
        m.msg = (ks[vsc] & 0x80) ? kWM_KEYDOWN : kWM_KEYUP;
        m.wParam = vk;
        forward(m);
        if (m.msg == kWM_KEYDOWN && translation_.toChar) {
            const int32_t ch = translation_.toChar(vk, vsc, ks);
            if (ch >= 0) {
                m.msg = kWM_CHAR;
                m.wParam = static_cast<uint32_t>(ch);
                forward(m);
            }
        }
        keys_[vsc] = ks[vsc];
    }
}

void DirectInputTranslator::keyboardEvent(uint32_t scanCode, uint32_t data) {
    if (scanCode >= 256) {
        return;
    }
    std::array<uint8_t, 256> next = keys_;
    next[scanCode] = static_cast<uint8_t>(data);
    keyboardState(next.data());
}

void DirectInputTranslator::mouseState(const MouseInput& s, bool absolute) {
    if (uiActive_) {
        return;  // upstream: the renderer UI owns the cursor while it is open
    }
    if (absolute) {
        mouseX_ = s.x;
        mouseY_ = s.y;
    } else {
        mouseX_ += s.x;
        mouseY_ += s.y;
    }
    if (mouseX_ < 0) mouseX_ = 0;
    if (mouseY_ < 0) mouseY_ = 0;
    if (mouseX_ > windowWidth_) mouseX_ = windowWidth_;
    if (mouseY_ > windowHeight_) mouseY_ = windowHeight_;

    WindowMessage m;
    m.msg = kWM_MOUSEMOVE;
    m.lParam = int64_t(uint32_t(mouseX_) | (uint32_t(mouseY_) << 16));
    uint32_t w = (s.buttons[0] & 0x80) ? kMK_LBUTTON : 0;
    w += (s.buttons[1] & 0x80) ? kMK_RBUTTON : 0;
    w += ((keys_[kDIK_LCONTROL] & 0x80) || (keys_[kDIK_RCONTROL] & 0x80)) ? kMK_CONTROL : 0;
    w += ((keys_[kDIK_LSHIFT] & 0x80) || (keys_[kDIK_RSHIFT] & 0x80)) ? kMK_SHIFT : 0;
    m.wParam = w;

    if (!sameMessage(m, lastMove_)) {
        forward(m);
        lastMove_ = m;
    }
    if (mouseButtons_[0] != s.buttons[0]) {
        m.msg = (s.buttons[0] & 0x80) ? kWM_LBUTTONDOWN : kWM_LBUTTONUP;
        mouseButtons_[0] = s.buttons[0];
        if (!sameMessage(m, lastLButton_)) {
            forward(m);
            lastLButton_ = m;
        }
    }
    if (mouseButtons_[1] != s.buttons[1]) {
        m.msg = (s.buttons[1] & 0x80) ? kWM_RBUTTONDOWN : kWM_RBUTTONUP;
        mouseButtons_[1] = s.buttons[1];
        if (!sameMessage(m, lastRButton_)) {
            forward(m);
            lastRButton_ = m;
        }
    }
    if (wheelDelta(lastWheel_.wParam) != static_cast<int16_t>(s.z)) {
        const uint32_t buttons = static_cast<uint32_t>(m.wParam & 0xFFFF);
        m.msg = kWM_MOUSEWHEEL;
        m.wParam = buttons | (uint32_t(uint16_t(int16_t(s.z))) << 16);
        forward(m);
        lastWheel_ = m;
    }
}

void DirectInputTranslator::mouseEvent(uint32_t offset, uint32_t data, bool absolute) {
    MouseInput s;
    s.buttons = mouseButtons_;
    if (absolute) {
        s.x = mouseX_;
        s.y = mouseY_;
    }
    switch (offset) {
    case kOfsX: s.x = static_cast<int32_t>(data); break;
    case kOfsY: s.y = static_cast<int32_t>(data); break;
    case kOfsZ: s.z = static_cast<int32_t>(data); break;
    case kOfsButton0: s.buttons[0] = static_cast<uint8_t>(data); break;
    case kOfsButton1: s.buttons[1] = static_cast<uint8_t>(data); break;
    default: return;
    }
    mouseState(s, absolute);
}

void DirectInputTranslator::resetMouse() noexcept {
    mouseX_ = 0;
    mouseY_ = 0;
    mouseButtons_ = {};
    lastMove_ = {};
    lastLButton_ = {};
    lastRButton_ = {};
    lastWheel_ = {};
}

}  // namespace fuse::relight::bridge::client
