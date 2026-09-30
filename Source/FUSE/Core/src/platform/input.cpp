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
    for (u32 i = 0; i < kMouseButtonCount; ++i) {
        m_mousePressed[i] = false;
        m_mouseReleased[i] = false;
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
        // Consecutive client positions (OS cursor path, acceleration applied). Only a fallback
        // delta source: with raw input active the delta comes from RawMouseDelta alone.
        if (m_haveMousePosition && !m_rawMouseActive) {
            m_mouseDeltaX += event.mouseX - m_mouseX;
            m_mouseDeltaY += event.mouseY - m_mouseY;
        }
        m_mouseX = event.mouseX;
        m_mouseY = event.mouseY;
        m_haveMousePosition = true;
        break;
    }
    case PlatformEventType::InputCaptureChanged:
        onInputCaptureChanged(event.inputCaptured);
        break;
    case PlatformEventType::RawMouseDelta: {
        m_rawMouseActive = true;
        m_mouseDeltaX += event.mouseX;
        m_mouseDeltaY += event.mouseY;
        break;
    }
    case PlatformEventType::MouseButtonDown: {
        const MouseButton button = mouseButtonFromPlatformCode(event.mouseButton);
        if (!validMouseButton(button)) {
            return;
        }
        if (!m_mouseDown[static_cast<u32>(button)]) {
            m_mousePressed[static_cast<u32>(button)] = true;
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
        if (m_mouseDown[static_cast<u32>(button)]) {
            m_mouseReleased[static_cast<u32>(button)] = true;
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

void InputState::onInputCaptureChanged(bool captured) {
    if (captured) {
        // Raw mode starts with the first RawMouseDelta (raw input may be unavailable).
        return;
    }
    // Raw input stops with capture: deltas come from MouseMove again. The cursor was hidden /
    // clipped while captured, so the next MouseMove re-establishes the baseline instead of
    // producing a jump delta.
    m_rawMouseActive = false;
    m_haveMousePosition = false;
}

u32 InputState::applyPump(EventPump& pump) {
    u32 applied = 0;
    PlatformEvent event;
    while (pump.pollEvent(event)) {
        apply(event);
        ++applied;
    }
    return applied;
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

bool InputState::mousePressed(MouseButton button) const {
    if (!validMouseButton(button)) {
        return false;
    }
    return m_mousePressed[static_cast<u32>(button)];
}

bool InputState::mouseReleased(MouseButton button) const {
    if (!validMouseButton(button)) {
        return false;
    }
    return m_mouseReleased[static_cast<u32>(button)];
}

void InputState::releaseAll() {
    for (u32 i = 0; i < kKeyCount; ++i) {
        if (m_keyDown[i]) {
            m_keyReleased[i] = true;
        }
        m_keyDown[i] = false;
    }
    for (u32 i = 0; i < kMouseButtonCount; ++i) {
        if (m_mouseDown[i]) {
            m_mouseReleased[i] = true;
        }
        m_mouseDown[i] = false;
    }
}

namespace {

constexpr std::string_view kKeyNames[] = {
    "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O", "P", "Q", "R", "S",
    "T", "U", "V", "W", "X", "Y", "Z", "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "F1", "F2",
    "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12", "Space", "Enter", "Escape", "Tab",
    "Backspace", "Delete", "Insert", "Left", "Right", "Up", "Down", "PageUp", "PageDown", "Home",
    "End", "Ctrl", "Shift", "Alt", "Super", "CapsLock",
};
static_assert(sizeof(kKeyNames) / sizeof(kKeyNames[0]) == static_cast<usize>(Key::COUNT),
              "kKeyNames must list every Key");

constexpr std::string_view kMouseButtonNames[] = {"Left", "Right", "Middle", "X1", "X2"};
static_assert(sizeof(kMouseButtonNames) / sizeof(kMouseButtonNames[0]) == static_cast<usize>(MouseButton::COUNT),
              "kMouseButtonNames must list every MouseButton");

bool equalsNoCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (usize i = 0; i < a.size(); ++i) {
        char x = a[i];
        char y = b[i];
        x = (x >= 'A' && x <= 'Z') ? static_cast<char>(x - 'A' + 'a') : x;
        y = (y >= 'A' && y <= 'Z') ? static_cast<char>(y - 'A' + 'a') : y;
        if (x != y) {
            return false;
        }
    }
    return true;
}

} // namespace

std::string_view keyName(Key key) {
    return validKey(key) ? kKeyNames[static_cast<u32>(key)] : std::string_view();
}

Key keyFromName(std::string_view name) {
    for (u32 i = 0; i < static_cast<u32>(Key::COUNT); ++i) {
        if (equalsNoCase(kKeyNames[i], name)) {
            return static_cast<Key>(i);
        }
    }
    // Common aliases.
    if (equalsNoCase(name, "Return")) {
        return Key::Enter;
    }
    if (equalsNoCase(name, "Esc")) {
        return Key::Escape;
    }
    if (equalsNoCase(name, "Control")) {
        return Key::Ctrl;
    }
    return Key::COUNT;
}

std::string_view mouseButtonName(MouseButton button) {
    return validMouseButton(button) ? kMouseButtonNames[static_cast<u32>(button)] : std::string_view();
}

MouseButton mouseButtonFromName(std::string_view name) {
    for (u32 i = 0; i < static_cast<u32>(MouseButton::COUNT); ++i) {
        if (equalsNoCase(kMouseButtonNames[i], name)) {
            return static_cast<MouseButton>(i);
        }
    }
    return MouseButton::COUNT;
}

} // namespace fuse::platform
