// FUSE platform events and key set v2 (FUSE UI plan UI1, docs/plans/FUSE_UI_PLAN.md §4.1). Header-only.
//
// v2 adds what a GUI needs without breaking v1 consumers:
//   keys    punctuation, numpad, left/right modifiers, F13–F24, lock / system and media keys. KeyV2 starts with the
//           v1 `Key` values in the same order, so `fromLegacy` is a cast and `toLegacy` folds the new keys back
//           (LeftCtrl → Ctrl, Numpad5 → Num5, NumpadEnter → Enter; keys with no v1 equivalent → Key::COUNT).
//   events  text input (UTF-8, inline), a real wheel event (v1 encoded wheel as MouseMove with Y = delta), scancode,
//           repeat flag and modifier mask on keys, touch, DPI change, move, minimise / restore, theme change.
// `fromLegacy(PlatformEvent)` / `toLegacy(EventV2)` convert between the two so EventPump and InputState stay v1 until
// the native backends emit v2 directly.
#pragma once

#include <fuse/platform/event_pump.hpp>
#include <fuse/platform/input.hpp>

#include <array>
#include <cstring>
#include <optional>
#include <string_view>

namespace fuse::platform {

enum class KeyV2 : u16 {
    // ---- v1 keys, same values as fuse::platform::Key ----
    A = 0, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    Num0, Num1, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9,
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    Space, Enter, Escape, Tab, Backspace, Delete, Insert, Left, Right, Up, Down, PageUp, PageDown, Home, End,
    Ctrl, Shift, Alt, Super, CapsLock,
    // ---- v2 additions ----
    Minus, Equal, LeftBracket, RightBracket, Backslash, Semicolon, Apostrophe, Grave, Comma, Period, Slash,
    Numpad0, Numpad1, Numpad2, Numpad3, Numpad4, Numpad5, Numpad6, Numpad7, Numpad8, Numpad9,
    NumpadAdd, NumpadSubtract, NumpadMultiply, NumpadDivide, NumpadDecimal, NumpadEnter,
    LeftCtrl, RightCtrl, LeftShift, RightShift, LeftAlt, RightAlt, LeftSuper, RightSuper,
    F13, F14, F15, F16, F17, F18, F19, F20, F21, F22, F23, F24,
    PrintScreen, ScrollLock, Pause, NumLock, Menu,
    MediaPlayPause, MediaNext, MediaPrevious, MediaStop, VolumeUp, VolumeDown, VolumeMute,
    COUNT
};
static_assert(u16(KeyV2::CapsLock) == u16(Key::CapsLock), "KeyV2 must start with the v1 Key values");
static_assert(u16(KeyV2::Minus) == u16(Key::COUNT));

enum KeyMods : u8 { kKeyModShift = 1, kKeyModCtrl = 2, kKeyModAlt = 4, kKeyModSuper = 8, kKeyModCaps = 16, kKeyModNum = 32 };

constexpr KeyV2 fromLegacy(Key k) { return k == Key::COUNT ? KeyV2::COUNT : KeyV2(u16(k)); }

constexpr Key toLegacy(KeyV2 k) {
    const u16 v = u16(k);
    if (v < u16(Key::COUNT)) return Key(v);
    if (k >= KeyV2::Numpad0 && k <= KeyV2::Numpad9) return Key(u16(Key::Num0) + (v - u16(KeyV2::Numpad0)));
    switch (k) {
    case KeyV2::NumpadEnter: return Key::Enter;
    case KeyV2::LeftCtrl: case KeyV2::RightCtrl: return Key::Ctrl;
    case KeyV2::LeftShift: case KeyV2::RightShift: return Key::Shift;
    case KeyV2::LeftAlt: case KeyV2::RightAlt: return Key::Alt;
    case KeyV2::LeftSuper: case KeyV2::RightSuper: return Key::Super;
    default: return Key::COUNT;
    }
}

namespace detail {
struct VkPair { u16 vk; KeyV2 key; };
// Win32 virtual-key codes (also used by the X11 backend's translation table and by input replays).
inline constexpr VkPair kVkTable[] = {
    {0x08, KeyV2::Backspace}, {0x09, KeyV2::Tab}, {0x0D, KeyV2::Enter}, {0x10, KeyV2::Shift}, {0x11, KeyV2::Ctrl},
    {0x12, KeyV2::Alt}, {0x13, KeyV2::Pause}, {0x14, KeyV2::CapsLock}, {0x1B, KeyV2::Escape}, {0x20, KeyV2::Space},
    {0x21, KeyV2::PageUp}, {0x22, KeyV2::PageDown}, {0x23, KeyV2::End}, {0x24, KeyV2::Home}, {0x25, KeyV2::Left},
    {0x26, KeyV2::Up}, {0x27, KeyV2::Right}, {0x28, KeyV2::Down}, {0x2C, KeyV2::PrintScreen}, {0x2D, KeyV2::Insert},
    {0x2E, KeyV2::Delete}, {0x5B, KeyV2::LeftSuper}, {0x5C, KeyV2::RightSuper}, {0x5D, KeyV2::Menu},
    {0x6A, KeyV2::NumpadMultiply}, {0x6B, KeyV2::NumpadAdd}, {0x6D, KeyV2::NumpadSubtract},
    {0x6E, KeyV2::NumpadDecimal}, {0x6F, KeyV2::NumpadDivide}, {0x90, KeyV2::NumLock}, {0x91, KeyV2::ScrollLock},
    {0xA0, KeyV2::LeftShift}, {0xA1, KeyV2::RightShift}, {0xA2, KeyV2::LeftCtrl}, {0xA3, KeyV2::RightCtrl},
    {0xA4, KeyV2::LeftAlt}, {0xA5, KeyV2::RightAlt}, {0xAD, KeyV2::VolumeMute}, {0xAE, KeyV2::VolumeDown},
    {0xAF, KeyV2::VolumeUp}, {0xB0, KeyV2::MediaNext}, {0xB1, KeyV2::MediaPrevious}, {0xB2, KeyV2::MediaStop},
    {0xB3, KeyV2::MediaPlayPause}, {0xBA, KeyV2::Semicolon}, {0xBB, KeyV2::Equal}, {0xBC, KeyV2::Comma},
    {0xBD, KeyV2::Minus}, {0xBE, KeyV2::Period}, {0xBF, KeyV2::Slash}, {0xC0, KeyV2::Grave},
    {0xDB, KeyV2::LeftBracket}, {0xDC, KeyV2::Backslash}, {0xDD, KeyV2::RightBracket}, {0xDE, KeyV2::Apostrophe},
};
inline constexpr std::string_view kV2Names[] = {
    "Minus", "Equal", "LeftBracket", "RightBracket", "Backslash", "Semicolon", "Apostrophe", "Grave", "Comma",
    "Period", "Slash", "Numpad0", "Numpad1", "Numpad2", "Numpad3", "Numpad4", "Numpad5", "Numpad6", "Numpad7",
    "Numpad8", "Numpad9", "NumpadAdd", "NumpadSubtract", "NumpadMultiply", "NumpadDivide", "NumpadDecimal",
    "NumpadEnter", "LeftCtrl", "RightCtrl", "LeftShift", "RightShift", "LeftAlt", "RightAlt", "LeftSuper",
    "RightSuper", "F13", "F14", "F15", "F16", "F17", "F18", "F19", "F20", "F21", "F22", "F23", "F24", "PrintScreen",
    "ScrollLock", "Pause", "NumLock", "Menu", "MediaPlayPause", "MediaNext", "MediaPrevious", "MediaStop", "VolumeUp",
    "VolumeDown", "VolumeMute"};
static_assert(sizeof(kV2Names) / sizeof(kV2Names[0]) == u16(KeyV2::COUNT) - u16(KeyV2::Minus));
constexpr char lower(char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }
constexpr bool ieq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (lower(a[i]) != lower(b[i])) return false;
    return true;
}
} // namespace detail

/// Win32 virtual-key → KeyV2 (letters are the upper-case VKs; 0x61.. are numpad keys); unknown → COUNT.
constexpr KeyV2 keyV2FromVirtualKey(u32 vk) {
    if (vk >= 'A' && vk <= 'Z') return KeyV2(u16(KeyV2::A) + (vk - 'A'));
    if (vk >= '0' && vk <= '9') return KeyV2(u16(KeyV2::Num0) + (vk - '0'));
    if (vk >= 0x60 && vk <= 0x69) return KeyV2(u16(KeyV2::Numpad0) + (vk - 0x60));
    if (vk >= 0x70 && vk <= 0x7B) return KeyV2(u16(KeyV2::F1) + (vk - 0x70));
    if (vk >= 0x7C && vk <= 0x87) return KeyV2(u16(KeyV2::F13) + (vk - 0x7C));
    for (const auto& p : detail::kVkTable)
        if (p.vk == vk) return p.key;
    return KeyV2::COUNT;
}
/// Inverse of keyV2FromVirtualKey (0 when the key has no virtual-key code; NumpadEnter → VK_RETURN).
constexpr u32 virtualKeyFromKeyV2(KeyV2 k) {
    const u16 v = u16(k);
    if (k <= KeyV2::Z) return 'A' + (v - u16(KeyV2::A));
    if (k >= KeyV2::Num0 && k <= KeyV2::Num9) return '0' + (v - u16(KeyV2::Num0));
    if (k >= KeyV2::Numpad0 && k <= KeyV2::Numpad9) return 0x60 + (v - u16(KeyV2::Numpad0));
    if (k >= KeyV2::F1 && k <= KeyV2::F12) return 0x70 + (v - u16(KeyV2::F1));
    if (k >= KeyV2::F13 && k <= KeyV2::F24) return 0x7C + (v - u16(KeyV2::F13));
    if (k == KeyV2::NumpadEnter) return 0x0D;
    if (k == KeyV2::Super) return 0x5B;
    for (const auto& p : detail::kVkTable)
        if (p.key == k) return p.vk;
    return 0;
}

/// Stable names: v1 names (keyName) for v1 keys, then "Minus", "Numpad7", "LeftCtrl", "F13", "VolumeUp", ...
inline std::string_view keyV2Name(KeyV2 k) {
    if (u16(k) < u16(Key::COUNT)) return keyName(Key(u16(k)));
    if (k >= KeyV2::COUNT) return {};
    return detail::kV2Names[u16(k) - u16(KeyV2::Minus)];
}
inline KeyV2 keyV2FromName(std::string_view n) {
    const Key k = keyFromName(n);
    if (k != Key::COUNT) return fromLegacy(k);
    for (u16 i = 0; i < u16(KeyV2::COUNT) - u16(KeyV2::Minus); ++i)
        if (detail::ieq(detail::kV2Names[i], n)) return KeyV2(u16(KeyV2::Minus) + i);
    return KeyV2::COUNT;
}

// ---- Events --------------------------------------------------------------------------------------------------------
enum class EventV2Type : u8 {
    None = 0, Quit, WindowCloseRequested, WindowResized, WindowFocusGained, WindowFocusLost, KeyDown, KeyUp,
    MouseMove, MouseButtonDown, MouseButtonUp, RawMouseDelta, InputCaptureChanged,
    // v2
    TextInput, Wheel, TouchDown, TouchMove, TouchUp, DpiChanged, WindowMoved, WindowMinimized, WindowRestored,
    ThemeChanged,
};

struct EventV2 {
    EventV2Type type = EventV2Type::None;
    Window* window = nullptr;
    u32 width = 0, height = 0;          ///< resize
    KeyV2 key = KeyV2::COUNT;
    u32 virtualKey = 0;                 ///< original platform code
    u32 scancode = 0;                   ///< physical key (0 = unknown)
    bool repeat = false;
    u8 mods = 0;                        ///< KeyMods
    float x = 0.f, y = 0.f;             ///< client pixels (float for touch / high-precision pointers); raw delta
    float wheelX = 0.f, wheelY = 0.f;   ///< notches (y > 0 = away from the user)
    u8 mouseButton = 0;                 ///< 1 left, 2 right, 3 middle
    u32 pointerId = 0;                  ///< touch
    float dpiScale = 1.f;               ///< DpiChanged
    bool inputCaptured = false;
    bool darkTheme = false;             ///< ThemeChanged
    std::array<char, 16> text{};        ///< TextInput: UTF-8, NUL-padded (one event per committed code point / cluster)
    std::string_view textView() const { return {text.data(), ::strnlen(text.data(), text.size())}; }
    void setText(std::string_view s) {
        text.fill(0);
        std::memcpy(text.data(), s.data(), s.size() < text.size() ? s.size() : text.size());
    }
};

/// v1 → v2. When `wheelInMouseMove` is set, a v1 MouseMove with keyCode == 0xFFFF (the legacy wheel tag used by the
/// Win32 backend) becomes a Wheel event with wheelY = mouseY / 120.
inline EventV2 fromLegacy(const PlatformEvent& e, bool wheelInMouseMove = true) {
    EventV2 v;
    v.type = EventV2Type(u8(e.type));
    v.window = e.window;
    v.width = e.width;
    v.height = e.height;
    v.virtualKey = e.keyCode;
    v.x = float(e.mouseX);
    v.y = float(e.mouseY);
    v.mouseButton = e.mouseButton;
    v.inputCaptured = e.inputCaptured;
    if (e.type == PlatformEventType::KeyDown || e.type == PlatformEventType::KeyUp) {
        const KeyV2 k = keyV2FromVirtualKey(e.keyCode);
        v.key = k != KeyV2::COUNT ? k : fromLegacy(keyFromPlatformCode(e.keyCode));
    }
    if (wheelInMouseMove && e.type == PlatformEventType::MouseMove && e.keyCode == 0xFFFFu) {
        v.type = EventV2Type::Wheel;
        v.wheelY = float(e.mouseY) / 120.f;
        v.x = v.y = 0.f;
    }
    return v;
}

/// v2 → v1 for consumers that still take PlatformEvent; nullopt for v2-only events (text, touch, ...).
inline std::optional<PlatformEvent> toLegacy(const EventV2& v) {
    if (u8(v.type) > u8(EventV2Type::InputCaptureChanged)) {
        if (v.type != EventV2Type::Wheel) return std::nullopt;
        PlatformEvent e;
        e.type = PlatformEventType::MouseMove;
        e.window = v.window;
        e.keyCode = 0xFFFFu;
        e.mouseY = i32(v.wheelY * 120.f);
        return e;
    }
    PlatformEvent e;
    e.type = PlatformEventType(u8(v.type));
    e.window = v.window;
    e.width = v.width;
    e.height = v.height;
    e.keyCode = v.virtualKey ? v.virtualKey : virtualKeyFromKeyV2(v.key);
    e.mouseX = i32(v.x);
    e.mouseY = i32(v.y);
    e.mouseButton = v.mouseButton;
    e.inputCaptured = v.inputCaptured;
    return e;
}

} // namespace fuse::platform
