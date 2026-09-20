#include <fuse/platform/input.hpp>

#include <fuse/platform/event_pump.hpp>

namespace fuse::platform {

namespace {

MouseButton mouseButtonFromPlatformCode(u8 mouseButton) {
    switch (mouseButton) {
    case 1:
        return MouseButton::Left;
    case 2:
        return MouseButton::Right;
    case 3:
        return MouseButton::Middle;
    case 4:
        return MouseButton::X1;
    case 5:
        return MouseButton::X2;
    default:
        return MouseButton::COUNT;
    }
}

bool validKey(Key key) {
    return static_cast<u32>(key) < static_cast<u32>(Key::COUNT);
}

bool validMouseButton(MouseButton button) {
    return static_cast<u32>(button) < static_cast<u32>(MouseButton::COUNT);
}

} // namespace

Key keyFromPlatformCode(u32 keyCode) {
    // Win32 VK_A is 'A' (65). Tests also pass USB-ish 65 for A.
    if (keyCode >= static_cast<u32>('A') && keyCode <= static_cast<u32>('Z')) {
        return static_cast<Key>(static_cast<u16>(keyCode - static_cast<u32>('A')) +
                                static_cast<u16>(Key::A));
    }
    if (keyCode >= static_cast<u32>('a') && keyCode <= static_cast<u32>('z')) {
        return static_cast<Key>(static_cast<u16>(keyCode - static_cast<u32>('a')) +
                                static_cast<u16>(Key::A));
    }
    if (keyCode >= static_cast<u32>('0') && keyCode <= static_cast<u32>('9')) {
        return static_cast<Key>(static_cast<u16>(keyCode - static_cast<u32>('0')) +
                                static_cast<u16>(Key::Num0));
    }

    switch (keyCode) {
    case 0x70:
        return Key::F1;
    case 0x71:
        return Key::F2;
    case 0x72:
        return Key::F3;
    case 0x73:
        return Key::F4;
    case 0x74:
        return Key::F5;
    case 0x75:
        return Key::F6;
    case 0x76:
        return Key::F7;
    case 0x77:
        return Key::F8;
    case 0x78:
        return Key::F9;
    case 0x79:
        return Key::F10;
    case 0x7A:
        return Key::F11;
    case 0x7B:
        return Key::F12;
    case 0x20:
        return Key::Space;
    case 0x0D:
        return Key::Enter;
    case 0x1B:
        return Key::Escape;
    case 0x09:
        return Key::Tab;
    case 0x08:
        return Key::Backspace;
    case 0x2E:
        return Key::Delete;
    case 0x2D:
        return Key::Insert;
    case 0x25:
        return Key::Left;
    case 0x27:
        return Key::Right;
    case 0x26:
        return Key::Up;
    case 0x28:
        return Key::Down;
    case 0x21:
        return Key::PageUp;
    case 0x22:
        return Key::PageDown;
    case 0x24:
        return Key::Home;
    case 0x23:
        return Key::End;
    case 0x11:
    case 0xA2:
    case 0xA3:
        return Key::Ctrl;
    case 0x10:
    case 0xA0:
    case 0xA1:
        return Key::Shift;
    case 0x12:
    case 0xA4:
    case 0xA5:
        return Key::Alt;
    case 0x5B:
    case 0x5C:
        return Key::Super;
    case 0x14:
        return Key::CapsLock;
    default:
        return Key::COUNT;
    }
}

void InputState::beginFrame() {
    for (u32 i = 0; i < kKeyCount; ++i) {
        m_keyPressed[i] = false;
        m_keyReleased[i] = false;
    }
    m_mouseDeltaX = 0;
    m_mouseDeltaY = 0;
}

void InputState::apply(const PlatformEvent& event) {
    switch (event.type) {
    case PlatformEventType::KeyDown: {
        const Key key = keyFromPlatformCode(event.keyCode);
        if (!validKey(key)) {
            return;
        }
        const u32 index = static_cast<u32>(key);
        if (!m_keyDown[index]) {
            m_keyPressed[index] = true;
        }
        m_keyDown[index] = true;
        break;
    }
    case PlatformEventType::KeyUp: {
        const Key key = keyFromPlatformCode(event.keyCode);
        if (!validKey(key)) {
            return;
        }
        const u32 index = static_cast<u32>(key);
        m_keyDown[index] = false;
        m_keyReleased[index] = true;
        break;
    }
    case PlatformEventType::MouseMove: {
        // Consecutive client positions — not OS cursor-acceleration velocity.
        if (m_haveMousePosition) {
            m_mouseDeltaX += event.mouseX - m_mouseX;
            m_mouseDeltaY += event.mouseY - m_mouseY;
        }
        m_mouseX = event.mouseX;
        m_mouseY = event.mouseY;
        m_haveMousePosition = true;
        break;
    }
    case PlatformEventType::MouseButtonDown: {
        const MouseButton button = mouseButtonFromPlatformCode(event.mouseButton);
        if (!validMouseButton(button)) {
            return;
        }
        m_mouseDown[static_cast<u32>(button)] = true;
        m_mouseX = event.mouseX;
        m_mouseY = event.mouseY;
        m_haveMousePosition = true;
        break;
    }
    case PlatformEventType::MouseButtonUp: {
        const MouseButton button = mouseButtonFromPlatformCode(event.mouseButton);
        if (!validMouseButton(button)) {
            return;
        }
        m_mouseDown[static_cast<u32>(button)] = false;
        m_mouseX = event.mouseX;
        m_mouseY = event.mouseY;
        m_haveMousePosition = true;
        break;
    }
    default:
        break;
    }
}

bool InputState::keyDown(Key key) const {
    if (!validKey(key)) {
        return false;
    }
    return m_keyDown[static_cast<u32>(key)];
}

bool InputState::keyPressed(Key key) const {
    if (!validKey(key)) {
        return false;
    }
    return m_keyPressed[static_cast<u32>(key)];
}

bool InputState::keyReleased(Key key) const {
    if (!validKey(key)) {
        return false;
    }
    return m_keyReleased[static_cast<u32>(key)];
}

bool InputState::mouseDown(MouseButton button) const {
    if (!validMouseButton(button)) {
        return false;
    }
    return m_mouseDown[static_cast<u32>(button)];
}

} // namespace fuse::platform
