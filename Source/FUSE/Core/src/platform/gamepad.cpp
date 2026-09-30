// UNI-INPUT-1: portable gamepad state, deadzones and backend translation (XInput snapshot diff,
// evdev event mapping). The device I/O lives in gamepad_linux.cpp / win/gamepad_xinput.cpp.

#include <fuse/platform/gamepad.hpp>

#include <cmath>

namespace fuse::platform {

namespace {

constexpr std::string_view kButtonNames[] = {
    "South", "East", "West", "North", "LeftShoulder", "RightShoulder", "Back", "Start",
    "Guide", "LeftStick", "RightStick", "DPadUp", "DPadDown", "DPadLeft", "DPadRight",
};
static_assert(sizeof(kButtonNames) / sizeof(kButtonNames[0]) == static_cast<usize>(GamepadButton::COUNT),
              "kButtonNames must list every GamepadButton");

constexpr std::string_view kAxisNames[] = {"LeftX", "LeftY", "RightX", "RightY", "LeftTrigger", "RightTrigger"};
static_assert(sizeof(kAxisNames) / sizeof(kAxisNames[0]) == static_cast<usize>(GamepadAxis::COUNT),
              "kAxisNames must list every GamepadAxis");

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

f32 clampUnit(f32 v, f32 lo) {
    return v < lo ? lo : (v > 1.f ? 1.f : v);
}

bool validPad(u32 pad) { return pad < kMaxGamepads; }

f32 thumbToUnit(std::int16_t v) {
    // -32768 .. 32767 -> -1 .. 1 (asymmetric range: scale each side separately).
    return v < 0 ? static_cast<f32>(v) / 32768.f : static_cast<f32>(v) / 32767.f;
}

f32 triggerToUnit(u8 v) { return static_cast<f32>(v) / 255.f; }

} // namespace

std::string_view gamepadButtonName(GamepadButton button) {
    const u32 i = static_cast<u32>(button);
    return i < static_cast<u32>(GamepadButton::COUNT) ? kButtonNames[i] : std::string_view();
}

GamepadButton gamepadButtonFromName(std::string_view name) {
    for (u32 i = 0; i < static_cast<u32>(GamepadButton::COUNT); ++i) {
        if (equalsNoCase(kButtonNames[i], name)) {
            return static_cast<GamepadButton>(i);
        }
    }
    struct Alias {
        std::string_view name;
        GamepadButton button;
    };
    static constexpr Alias kAliases[] = {
        {"A", GamepadButton::South},         {"B", GamepadButton::East},
        {"X", GamepadButton::West},          {"Y", GamepadButton::North},
        {"LB", GamepadButton::LeftShoulder}, {"RB", GamepadButton::RightShoulder},
        {"Select", GamepadButton::Back},     {"LS", GamepadButton::LeftStick},
        {"RS", GamepadButton::RightStick},
    };
    for (const Alias& a : kAliases) {
        if (equalsNoCase(a.name, name)) {
            return a.button;
        }
    }
    return GamepadButton::COUNT;
}

std::string_view gamepadAxisName(GamepadAxis axis) {
    const u32 i = static_cast<u32>(axis);
    return i < static_cast<u32>(GamepadAxis::COUNT) ? kAxisNames[i] : std::string_view();
}

GamepadAxis gamepadAxisFromName(std::string_view name) {
    for (u32 i = 0; i < static_cast<u32>(GamepadAxis::COUNT); ++i) {
        if (equalsNoCase(kAxisNames[i], name)) {
            return static_cast<GamepadAxis>(i);
        }
    }
    if (equalsNoCase(name, "LT")) {
        return GamepadAxis::LeftTrigger;
    }
    if (equalsNoCase(name, "RT")) {
        return GamepadAxis::RightTrigger;
    }
    return GamepadAxis::COUNT;
}

void applyStickDeadzone(f32 rawX, f32 rawY, f32 deadzone, f32& outX, f32& outY) {
    const f32 magnitude = std::sqrt(rawX * rawX + rawY * rawY);
    if (!(magnitude > deadzone) || magnitude <= 0.f) {
        outX = 0.f;
        outY = 0.f;
        return;
    }
    const f32 span = 1.f - deadzone;
    f32 scaled = span > 0.f ? (magnitude - deadzone) / span : 1.f;
    scaled = scaled > 1.f ? 1.f : scaled;
    const f32 k = scaled / magnitude;
    outX = rawX * k;
    outY = rawY * k;
}

f32 applyTriggerDeadzone(f32 raw, f32 deadzone) {
    if (!(raw > deadzone)) {
        return 0.f;
    }
    const f32 span = 1.f - deadzone;
    const f32 v = span > 0.f ? (raw - deadzone) / span : 1.f;
    return v > 1.f ? 1.f : v;
}

// ---- GamepadState -----------------------------------------------------------------------------

void GamepadState::clearPad(Pad& pad) {
    for (u32 b = 0; b < kButtonCount; ++b) {
        pad.down[b] = false;
    }
    for (u32 a = 0; a < kAxisCount; ++a) {
        pad.raw[a] = 0.f;
    }
}

void GamepadState::beginFrame() {
    for (Pad& pad : m_pads) {
        pad.connectedThisFrame = false;
        pad.disconnectedThisFrame = false;
        for (u32 b = 0; b < kButtonCount; ++b) {
            pad.pressed[b] = false;
            pad.released[b] = false;
        }
    }
}

void GamepadState::apply(const GamepadEvent& event) {
    if (!validPad(event.pad)) {
        return;
    }
    Pad& pad = m_pads[event.pad];
    switch (event.type) {
    case GamepadEventType::Connected:
        if (!pad.connected) {
            clearPad(pad);
            pad.connected = true;
            pad.connectedThisFrame = true;
        }
        break;
    case GamepadEventType::Disconnected:
        if (pad.connected) {
            for (u32 b = 0; b < kButtonCount; ++b) {
                if (pad.down[b]) {
                    pad.released[b] = true;
                }
            }
            clearPad(pad);
            pad.connected = false;
            pad.disconnectedThisFrame = true;
        }
        break;
    case GamepadEventType::Button: {
        if (event.code >= kButtonCount) {
            return;
        }
        if (!pad.connected) {
            // A backend that never sent Connected (e.g. synthetic input) still counts as a pad.
            pad.connected = true;
            pad.connectedThisFrame = true;
        }
        if (event.down && !pad.down[event.code]) {
            pad.pressed[event.code] = true;
        } else if (!event.down && pad.down[event.code]) {
            pad.released[event.code] = true;
        }
        pad.down[event.code] = event.down;
        break;
    }
    case GamepadEventType::Axis: {
        if (event.code >= kAxisCount) {
            return;
        }
        if (!pad.connected) {
            pad.connected = true;
            pad.connectedThisFrame = true;
        }
        const bool trigger = event.code == static_cast<u8>(GamepadAxis::LeftTrigger) ||
                             event.code == static_cast<u8>(GamepadAxis::RightTrigger);
        f32 v = std::isfinite(event.value) ? event.value : 0.f;
        v = clampUnit(v, trigger ? 0.f : -1.f);
        pad.raw[event.code] = v;
        break;
    }
    }
}

u32 GamepadState::applyAll(const std::vector<GamepadEvent>& events) {
    for (const GamepadEvent& e : events) {
        apply(e);
    }
    return static_cast<u32>(events.size());
}

bool GamepadState::connected(u32 pad) const { return validPad(pad) && m_pads[pad].connected; }
bool GamepadState::connectedThisFrame(u32 pad) const { return validPad(pad) && m_pads[pad].connectedThisFrame; }
bool GamepadState::disconnectedThisFrame(u32 pad) const {
    return validPad(pad) && m_pads[pad].disconnectedThisFrame;
}

u32 GamepadState::connectedCount() const {
    u32 n = 0;
    for (const Pad& pad : m_pads) {
        n += pad.connected ? 1u : 0u;
    }
    return n;
}

u32 GamepadState::firstConnected() const {
    for (u32 i = 0; i < kMaxGamepads; ++i) {
        if (m_pads[i].connected) {
            return i;
        }
    }
    return kMaxGamepads;
}

bool GamepadState::buttonDown(u32 pad, GamepadButton button) const {
    const u32 b = static_cast<u32>(button);
    return validPad(pad) && b < kButtonCount && m_pads[pad].down[b];
}

bool GamepadState::buttonPressed(u32 pad, GamepadButton button) const {
    const u32 b = static_cast<u32>(button);
    return validPad(pad) && b < kButtonCount && m_pads[pad].pressed[b];
}

bool GamepadState::buttonReleased(u32 pad, GamepadButton button) const {
    const u32 b = static_cast<u32>(button);
    return validPad(pad) && b < kButtonCount && m_pads[pad].released[b];
}

f32 GamepadState::rawAxis(u32 pad, GamepadAxis axis) const {
    const u32 a = static_cast<u32>(axis);
    return validPad(pad) && a < kAxisCount ? m_pads[pad].raw[a] : 0.f;
}

f32 GamepadState::axis(u32 pad, GamepadAxis axis) const {
    if (!validPad(pad)) {
        return 0.f;
    }
    const Pad& p = m_pads[pad];
    f32 x = 0.f;
    f32 y = 0.f;
    switch (axis) {
    case GamepadAxis::LeftX:
    case GamepadAxis::LeftY:
        applyStickDeadzone(p.raw[static_cast<u32>(GamepadAxis::LeftX)], p.raw[static_cast<u32>(GamepadAxis::LeftY)],
                           m_deadzones.leftStick, x, y);
        return axis == GamepadAxis::LeftX ? x : y;
    case GamepadAxis::RightX:
    case GamepadAxis::RightY:
        applyStickDeadzone(p.raw[static_cast<u32>(GamepadAxis::RightX)], p.raw[static_cast<u32>(GamepadAxis::RightY)],
                           m_deadzones.rightStick, x, y);
        return axis == GamepadAxis::RightX ? x : y;
    case GamepadAxis::LeftTrigger:
    case GamepadAxis::RightTrigger:
        return applyTriggerDeadzone(p.raw[static_cast<u32>(axis)], m_deadzones.trigger);
    case GamepadAxis::COUNT:
        break;
    }
    return 0.f;
}

// ---- XInput snapshot diff ---------------------------------------------------------------------

void diffXInputSnapshot(u8 pad, const XInputPadSnapshot& prev, const XInputPadSnapshot& next,
                        std::vector<GamepadEvent>& out) {
    struct BitMap {
        u16 bit;
        GamepadButton button;
    };
    static constexpr BitMap kBits[] = {
        {xinput_bits::A, GamepadButton::South},
        {xinput_bits::B, GamepadButton::East},
        {xinput_bits::X, GamepadButton::West},
        {xinput_bits::Y, GamepadButton::North},
        {xinput_bits::LeftShoulder, GamepadButton::LeftShoulder},
        {xinput_bits::RightShoulder, GamepadButton::RightShoulder},
        {xinput_bits::Back, GamepadButton::Back},
        {xinput_bits::Start, GamepadButton::Start},
        {xinput_bits::Guide, GamepadButton::Guide},
        {xinput_bits::LeftThumb, GamepadButton::LeftStick},
        {xinput_bits::RightThumb, GamepadButton::RightStick},
        {xinput_bits::DPadUp, GamepadButton::DPadUp},
        {xinput_bits::DPadDown, GamepadButton::DPadDown},
        {xinput_bits::DPadLeft, GamepadButton::DPadLeft},
        {xinput_bits::DPadRight, GamepadButton::DPadRight},
    };

    if (!next.connected) {
        if (prev.connected) {
            out.push_back(GamepadEvent::disconnected(pad));
        }
        return;
    }
    // A fresh connection diffs against an all-zero pad so the full state is reported.
    const XInputPadSnapshot base = prev.connected ? prev : XInputPadSnapshot{};
    if (!prev.connected) {
        out.push_back(GamepadEvent::connected(pad));
    }
    for (const BitMap& m : kBits) {
        const bool was = (base.buttons & m.bit) != 0;
        const bool now = (next.buttons & m.bit) != 0;
        if (was != now) {
            out.push_back(GamepadEvent::button(pad, m.button, now));
        }
    }
    const auto axisIf = [&](bool changed, GamepadAxis a, f32 v) {
        if (changed) {
            out.push_back(GamepadEvent::axis(pad, a, v));
        }
    };
    axisIf(base.thumbLX != next.thumbLX, GamepadAxis::LeftX, thumbToUnit(next.thumbLX));
    axisIf(base.thumbLY != next.thumbLY, GamepadAxis::LeftY, thumbToUnit(next.thumbLY));
    axisIf(base.thumbRX != next.thumbRX, GamepadAxis::RightX, thumbToUnit(next.thumbRX));
    axisIf(base.thumbRY != next.thumbRY, GamepadAxis::RightY, thumbToUnit(next.thumbRY));
    axisIf(base.leftTrigger != next.leftTrigger, GamepadAxis::LeftTrigger, triggerToUnit(next.leftTrigger));
    axisIf(base.rightTrigger != next.rightTrigger, GamepadAxis::RightTrigger, triggerToUnit(next.rightTrigger));
}

// ---- evdev mapper -----------------------------------------------------------------------------

void EvdevGamepadMapper::setAbsRange(u16 absCode, i32 minimum, i32 maximum) {
    if (absCode >= evdev_codes::AbsCount || maximum <= minimum) {
        return;
    }
    m_abs[absCode] = {true, minimum, maximum};
}

bool EvdevGamepadMapper::hasAbs(u16 absCode) const {
    return absCode < evdev_codes::AbsCount && m_abs[absCode].present;
}

f32 EvdevGamepadMapper::normalise(u16 absCode, i32 value) const {
    if (!hasAbs(absCode)) {
        return 0.f;
    }
    const AbsRange& r = m_abs[absCode];
    const f64 t = (static_cast<f64>(value) - r.lo) / (static_cast<f64>(r.hi) - r.lo);
    const f64 clamped = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
    switch (absCode) {
    case evdev_codes::AbsZ:
    case evdev_codes::AbsRZ:
    case evdev_codes::AbsGas:
    case evdev_codes::AbsBrake:
        return static_cast<f32>(clamped); // triggers 0..1
    default:
        return static_cast<f32>(clamped * 2.0 - 1.0);
    }
}

void EvdevGamepadMapper::resetHats() {
    m_hatX = 0;
    m_hatY = 0;
}

u32 EvdevGamepadMapper::translate(u8 pad, u16 type, u16 code, i32 value, std::vector<GamepadEvent>& out) {
    using namespace evdev_codes;
    const usize before = out.size();
    if (type == EvKey) {
        if (value == 2) {
            return 0; // autorepeat
        }
        const bool down = value != 0;
        GamepadButton button = GamepadButton::COUNT;
        switch (code) {
        case BtnSouth:
            button = GamepadButton::South;
            break;
        case BtnEast:
            button = GamepadButton::East;
            break;
        case BtnNorth:
            button = GamepadButton::North;
            break;
        case BtnWest:
            button = GamepadButton::West;
            break;
        case BtnTL:
            button = GamepadButton::LeftShoulder;
            break;
        case BtnTR:
            button = GamepadButton::RightShoulder;
            break;
        case BtnSelect:
            button = GamepadButton::Back;
            break;
        case BtnStart:
            button = GamepadButton::Start;
            break;
        case BtnMode:
            button = GamepadButton::Guide;
            break;
        case BtnThumbL:
            button = GamepadButton::LeftStick;
            break;
        case BtnThumbR:
            button = GamepadButton::RightStick;
            break;
        case BtnDPadUp:
            button = GamepadButton::DPadUp;
            break;
        case BtnDPadDown:
            button = GamepadButton::DPadDown;
            break;
        case BtnDPadLeft:
            button = GamepadButton::DPadLeft;
            break;
        case BtnDPadRight:
            button = GamepadButton::DPadRight;
            break;
        case BtnTL2:
            if (!hasAbs(AbsZ) && !hasAbs(AbsBrake)) {
                out.push_back(GamepadEvent::axis(pad, GamepadAxis::LeftTrigger, down ? 1.f : 0.f));
            }
            break;
        case BtnTR2:
            if (!hasAbs(AbsRZ) && !hasAbs(AbsGas)) {
                out.push_back(GamepadEvent::axis(pad, GamepadAxis::RightTrigger, down ? 1.f : 0.f));
            }
            break;
        default:
            break;
        }
        if (button != GamepadButton::COUNT) {
            out.push_back(GamepadEvent::button(pad, button, down));
        }
        return static_cast<u32>(out.size() - before);
    }
    if (type != EvAbs || !hasAbs(code)) {
        return 0;
    }
    switch (code) {
    case AbsX:
        out.push_back(GamepadEvent::axis(pad, GamepadAxis::LeftX, normalise(code, value)));
        break;
    case AbsY:
        out.push_back(GamepadEvent::axis(pad, GamepadAxis::LeftY, -normalise(code, value)));
        break;
    case AbsRX:
        out.push_back(GamepadEvent::axis(pad, GamepadAxis::RightX, normalise(code, value)));
        break;
    case AbsRY:
        out.push_back(GamepadEvent::axis(pad, GamepadAxis::RightY, -normalise(code, value)));
        break;
    case AbsZ:
    case AbsBrake:
        out.push_back(GamepadEvent::axis(pad, GamepadAxis::LeftTrigger, normalise(code, value)));
        break;
    case AbsRZ:
    case AbsGas:
        out.push_back(GamepadEvent::axis(pad, GamepadAxis::RightTrigger, normalise(code, value)));
        break;
    case AbsHat0X:
    case AbsHat0Y: {
        const i32 dir = value < 0 ? -1 : (value > 0 ? 1 : 0);
        i32& state = code == AbsHat0X ? m_hatX : m_hatY;
        if (dir == state) {
            break;
        }
        const GamepadButton negative = code == AbsHat0X ? GamepadButton::DPadLeft : GamepadButton::DPadUp;
        const GamepadButton positive = code == AbsHat0X ? GamepadButton::DPadRight : GamepadButton::DPadDown;
        if (state < 0) {
            out.push_back(GamepadEvent::button(pad, negative, false));
        } else if (state > 0) {
            out.push_back(GamepadEvent::button(pad, positive, false));
        }
        if (dir < 0) {
            out.push_back(GamepadEvent::button(pad, negative, true));
        } else if (dir > 0) {
            out.push_back(GamepadEvent::button(pad, positive, true));
        }
        state = dir;
        break;
    }
    default:
        break;
    }
    return static_cast<u32>(out.size() - before);
}

// gamepad_linux.cpp / win/gamepad_xinput.cpp define the factory when CMake compiles them.
#if !defined(FUSE_GAMEPAD_EVDEV) && !defined(FUSE_GAMEPAD_XINPUT)
std::unique_ptr<GamepadBackend> createPlatformGamepadBackend() {
    return std::make_unique<NullGamepadBackend>();
}
#endif

} // namespace fuse::platform
