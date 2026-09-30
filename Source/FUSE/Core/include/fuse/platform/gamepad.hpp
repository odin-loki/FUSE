#pragma once

// UNI-INPUT-1: gamepad state, events and platform backends.
//
// Backends turn device input into `GamepadEvent`s (connect / disconnect / button / axis):
//   Win32   XInput (xinput1_4 / xinput1_3 / xinput9_1_0 loaded at run time), 4 user slots.
//   Linux   evdev (/dev/input/event*): devices with gamepad buttons (BTN_SOUTH) or a joystick
//           (BTN_JOYSTICK + ABS_X), hot-plug through inotify, SYN_DROPPED re-sync.
//   other   null backend (no pads).
// `GamepadState` applies those events once per frame like `InputState` does for keyboard and
// mouse: held / pressed / released buttons and axes with deadzones applied.
//
// Conventions (both backends): sticks are -1..1 with +X right and +Y up; triggers are 0..1;
// buttons use positional names (South = Xbox A / PlayStation Cross).

#include <fuse/types.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fuse::platform {

inline constexpr u32 kMaxGamepads = 4;

enum class GamepadButton : u8 {
    South,
    East,
    West,
    North,
    LeftShoulder,
    RightShoulder,
    Back,
    Start,
    Guide,
    LeftStick,
    RightStick,
    DPadUp,
    DPadDown,
    DPadLeft,
    DPadRight,
    COUNT
};

enum class GamepadAxis : u8 { LeftX, LeftY, RightX, RightY, LeftTrigger, RightTrigger, COUNT };

/// "South", "East", "West", "North", "LeftShoulder", "RightShoulder", "Back", "Start", "Guide",
/// "LeftStick", "RightStick", "DPadUp", "DPadDown", "DPadLeft", "DPadRight" (aliases "A", "B",
/// "X", "Y", "LB", "RB"). Case-insensitive; unknown -> COUNT.
std::string_view gamepadButtonName(GamepadButton button);
GamepadButton gamepadButtonFromName(std::string_view name);
/// "LeftX", "LeftY", "RightX", "RightY", "LeftTrigger", "RightTrigger" (aliases "LT", "RT").
std::string_view gamepadAxisName(GamepadAxis axis);
GamepadAxis gamepadAxisFromName(std::string_view name);

enum class GamepadEventType : u8 { Connected, Disconnected, Button, Axis };

struct GamepadEvent {
    GamepadEventType type = GamepadEventType::Connected;
    u8 pad = 0;       ///< 0 .. kMaxGamepads-1
    u8 code = 0;      ///< GamepadButton / GamepadAxis
    bool down = false; ///< Button
    f32 value = 0.f;   ///< Axis: raw normalised value (sticks -1..1, triggers 0..1), no deadzone

    static GamepadEvent connected(u8 pad) { return {GamepadEventType::Connected, pad, 0, false, 0.f}; }
    static GamepadEvent disconnected(u8 pad) { return {GamepadEventType::Disconnected, pad, 0, false, 0.f}; }
    static GamepadEvent button(u8 pad, GamepadButton b, bool isDown) {
        return {GamepadEventType::Button, pad, static_cast<u8>(b), isDown, 0.f};
    }
    static GamepadEvent axis(u8 pad, GamepadAxis a, f32 v) {
        return {GamepadEventType::Axis, pad, static_cast<u8>(a), false, v};
    }
};

/// Deadzones (fractions of full deflection). Defaults are XInput's recommended values
/// (XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE 7849, RIGHT 8689 of 32767; TRIGGER_THRESHOLD 30 of 255).
struct GamepadDeadzones {
    f32 leftStick = 7849.f / 32767.f;
    f32 rightStick = 8689.f / 32767.f;
    f32 trigger = 30.f / 255.f;
};

/// Radial stick deadzone: magnitude below `deadzone` -> (0,0); above, the magnitude is rescaled
/// from [deadzone, 1] to [0, 1] (clamped) keeping the direction, so there is no jump at the edge.
void applyStickDeadzone(f32 rawX, f32 rawY, f32 deadzone, f32& outX, f32& outY);
/// Trigger deadzone: below `deadzone` -> 0, else rescaled from [deadzone, 1] to [0, 1].
f32 applyTriggerDeadzone(f32 raw, f32 deadzone);

/// Frame snapshot of up to kMaxGamepads pads. No heap. Call `beginFrame()` once per tick, then
/// `apply()` every event of the tick.
class GamepadState {
public:
    void beginFrame();
    void apply(const GamepadEvent& event);
    u32 applyAll(const std::vector<GamepadEvent>& events);

    [[nodiscard]] bool connected(u32 pad) const;
    [[nodiscard]] bool connectedThisFrame(u32 pad) const;
    [[nodiscard]] bool disconnectedThisFrame(u32 pad) const;
    [[nodiscard]] u32 connectedCount() const;
    /// Lowest connected pad index, or kMaxGamepads when none.
    [[nodiscard]] u32 firstConnected() const;

    [[nodiscard]] bool buttonDown(u32 pad, GamepadButton button) const;
    [[nodiscard]] bool buttonPressed(u32 pad, GamepadButton button) const;
    [[nodiscard]] bool buttonReleased(u32 pad, GamepadButton button) const;
    /// Deadzone-applied value (sticks radial per stick, triggers per trigger).
    [[nodiscard]] f32 axis(u32 pad, GamepadAxis axis) const;
    [[nodiscard]] f32 rawAxis(u32 pad, GamepadAxis axis) const;

    void setDeadzones(const GamepadDeadzones& deadzones) { m_deadzones = deadzones; }
    [[nodiscard]] const GamepadDeadzones& deadzones() const { return m_deadzones; }

private:
    static constexpr u32 kButtonCount = static_cast<u32>(GamepadButton::COUNT);
    static constexpr u32 kAxisCount = static_cast<u32>(GamepadAxis::COUNT);

    struct Pad {
        bool connected = false;
        bool connectedThisFrame = false;
        bool disconnectedThisFrame = false;
        bool down[kButtonCount]{};
        bool pressed[kButtonCount]{};
        bool released[kButtonCount]{};
        f32 raw[kAxisCount]{};
    };

    void clearPad(Pad& pad);

    Pad m_pads[kMaxGamepads]{};
    GamepadDeadzones m_deadzones{};
};

/// A source of gamepad events. `poll` is called on the game thread once per frame.
class GamepadBackend {
public:
    virtual ~GamepadBackend() = default;
    [[nodiscard]] virtual const char* name() const = 0;
    /// Appends every event since the last poll (hot-plug included); returns how many.
    virtual u32 poll(std::vector<GamepadEvent>& out) = 0;
    /// Rumble (0..1 low / high frequency motor). False when unsupported or the pad is absent.
    virtual bool setRumble(u32 pad, f32 lowFrequency, f32 highFrequency) {
        (void)pad;
        (void)lowFrequency;
        (void)highFrequency;
        return false;
    }
};

class NullGamepadBackend final : public GamepadBackend {
public:
    [[nodiscard]] const char* name() const override { return "null"; }
    u32 poll(std::vector<GamepadEvent>& out) override {
        (void)out;
        return 0;
    }
};

/// XInput on Win32, evdev on Linux, null elsewhere (or when the platform API is unavailable,
/// e.g. no XInput DLL). Never returns null.
std::unique_ptr<GamepadBackend> createPlatformGamepadBackend();

// ---- portable translation (used by the backends; testable on every host) ------------------

/// Mirror of XINPUT_GAMEPAD plus the connection state of one user slot.
struct XInputPadSnapshot {
    bool connected = false;
    u16 buttons = 0; ///< XINPUT_GAMEPAD_* bits
    u8 leftTrigger = 0;
    u8 rightTrigger = 0;
    std::int16_t thumbLX = 0;
    std::int16_t thumbLY = 0;
    std::int16_t thumbRX = 0;
    std::int16_t thumbRY = 0;
};

/// XINPUT_GAMEPAD_* button bits (xinput.h values).
namespace xinput_bits {
inline constexpr u16 DPadUp = 0x0001;
inline constexpr u16 DPadDown = 0x0002;
inline constexpr u16 DPadLeft = 0x0004;
inline constexpr u16 DPadRight = 0x0008;
inline constexpr u16 Start = 0x0010;
inline constexpr u16 Back = 0x0020;
inline constexpr u16 LeftThumb = 0x0040;
inline constexpr u16 RightThumb = 0x0080;
inline constexpr u16 LeftShoulder = 0x0100;
inline constexpr u16 RightShoulder = 0x0200;
inline constexpr u16 Guide = 0x0400; ///< only reported by the undocumented XInputGetStateEx
inline constexpr u16 A = 0x1000;
inline constexpr u16 B = 0x2000;
inline constexpr u16 X = 0x4000;
inline constexpr u16 Y = 0x8000;
} // namespace xinput_bits

/// Events that turn `prev` into `next` for user slot `pad`: Connected (followed by the full
/// state), Disconnected (after releasing held buttons and zeroing axes), button and axis
/// changes. Thumb values map to -1..1 (-32768 -> -1), triggers to 0..1.
void diffXInputSnapshot(u8 pad, const XInputPadSnapshot& prev, const XInputPadSnapshot& next,
                        std::vector<GamepadEvent>& out);

/// Linux evdev codes used by the mapper (linux/input-event-codes.h values).
namespace evdev_codes {
inline constexpr u16 EvSyn = 0x00;
inline constexpr u16 EvKey = 0x01;
inline constexpr u16 EvAbs = 0x03;
inline constexpr u16 SynReport = 0;
inline constexpr u16 SynDropped = 3;
inline constexpr u16 BtnJoystick = 0x120;
inline constexpr u16 BtnSouth = 0x130;
inline constexpr u16 BtnEast = 0x131;
inline constexpr u16 BtnC = 0x132;
inline constexpr u16 BtnNorth = 0x133;
inline constexpr u16 BtnWest = 0x134;
inline constexpr u16 BtnZ = 0x135;
inline constexpr u16 BtnTL = 0x136;
inline constexpr u16 BtnTR = 0x137;
inline constexpr u16 BtnTL2 = 0x138;
inline constexpr u16 BtnTR2 = 0x139;
inline constexpr u16 BtnSelect = 0x13a;
inline constexpr u16 BtnStart = 0x13b;
inline constexpr u16 BtnMode = 0x13c;
inline constexpr u16 BtnThumbL = 0x13d;
inline constexpr u16 BtnThumbR = 0x13e;
inline constexpr u16 BtnDPadUp = 0x220;
inline constexpr u16 BtnDPadDown = 0x221;
inline constexpr u16 BtnDPadLeft = 0x222;
inline constexpr u16 BtnDPadRight = 0x223;
inline constexpr u16 AbsX = 0x00;
inline constexpr u16 AbsY = 0x01;
inline constexpr u16 AbsZ = 0x02;
inline constexpr u16 AbsRX = 0x03;
inline constexpr u16 AbsRY = 0x04;
inline constexpr u16 AbsRZ = 0x05;
inline constexpr u16 AbsGas = 0x09;
inline constexpr u16 AbsBrake = 0x0a;
inline constexpr u16 AbsHat0X = 0x10;
inline constexpr u16 AbsHat0Y = 0x11;
inline constexpr u16 AbsCount = 0x40;
} // namespace evdev_codes

/// Maps one evdev device's EV_KEY / EV_ABS events to GamepadEvents using the Linux gamepad
/// layout (Documentation/input/gamepad.rst): BTN_SOUTH/EAST/NORTH/WEST face buttons, BTN_TL/TR
/// shoulders, ABS_X/Y left stick, ABS_RX/RY right stick, ABS_Z/RZ (or ABS_BRAKE/GAS) analog
/// triggers, BTN_TL2/TR2 as digital triggers when the device has no analog ones, ABS_HAT0X/Y
/// or BTN_DPAD_* for the d-pad. Stick Y is flipped to +up.
class EvdevGamepadMapper {
public:
    /// Declare an absolute axis the device reports (from EVIOCGABS). Axes never declared are
    /// ignored.
    void setAbsRange(u16 absCode, i32 minimum, i32 maximum);
    [[nodiscard]] bool hasAbs(u16 absCode) const;
    /// Translate one input_event; returns the number of events appended.
    u32 translate(u8 pad, u16 type, u16 code, i32 value, std::vector<GamepadEvent>& out);
    /// Normalised value of an absolute axis (sticks -1..1, triggers / hats as reported).
    [[nodiscard]] f32 normalise(u16 absCode, i32 value) const;
    /// Forget d-pad hat state (after a disconnect / re-sync).
    void resetHats();

private:
    struct AbsRange {
        bool present = false;
        i32 lo = 0;
        i32 hi = 0;
    };
    AbsRange m_abs[evdev_codes::AbsCount]{};
    i32 m_hatX = 0;
    i32 m_hatY = 0;
};

// Linux desktop only (CMake compiles gamepad_linux.cpp for Linux, not Android).
#if defined(__linux__) && !defined(__ANDROID__)
#define FUSE_PLATFORM_HAS_EVDEV_GAMEPAD 1
/// Capabilities of an adopted evdev stream (test hook; real devices are probed by ioctl).
struct EvdevDeviceCaps {
    std::string name;
    struct Abs {
        u16 code = 0;
        i32 minimum = 0;
        i32 maximum = 0;
    };
    std::vector<Abs> absAxes;
};

/// Linux evdev backend (see the file comment). One `/dev/input/event*` node per pad slot.
class EvdevGamepadBackend final : public GamepadBackend {
public:
    /// `inputDir` is scanned for event* nodes; `watchHotplug` adds an inotify watch on it.
    explicit EvdevGamepadBackend(std::string inputDir = "/dev/input", bool watchHotplug = true);
    ~EvdevGamepadBackend() override;
    EvdevGamepadBackend(const EvdevGamepadBackend&) = delete;
    EvdevGamepadBackend& operator=(const EvdevGamepadBackend&) = delete;

    [[nodiscard]] const char* name() const override { return "evdev"; }
    u32 poll(std::vector<GamepadEvent>& out) override;
    bool setRumble(u32 pad, f32 lowFrequency, f32 highFrequency) override;

    /// Adopt an open non-blocking fd that delivers `struct input_event` records (a pipe in tests)
    /// as a pad with `caps`. The backend owns the fd. Returns the pad slot or -1 when full.
    /// Connected is reported by the next poll; EOF is a disconnect.
    int adoptStream(int fd, const EvdevDeviceCaps& caps);
    /// Pads currently open.
    [[nodiscard]] u32 deviceCount() const;
    /// Device name of a pad slot (EVIOCGNAME), empty when free.
    [[nodiscard]] std::string deviceName(u32 pad) const;
    /// Request a directory rescan on the next poll (also triggered by inotify).
    void requestRescan() { m_scanPending = true; }

private:
    struct Device {
        int fd = -1;
        std::string path;
        std::string name;
        EvdevGamepadMapper mapper;
        bool announced = false;
        bool dropped = false;
        bool stream = false;
        i32 rumbleEffect = -1;
    };

    void rescan(std::vector<GamepadEvent>& out);
    bool openDevice(const std::string& path, std::vector<GamepadEvent>& out);
    void resync(u8 pad, std::vector<GamepadEvent>& out);
    void closeDevice(u8 pad, std::vector<GamepadEvent>& out);

    Device m_devices[kMaxGamepads];
    std::string m_dir;
    int m_inotify = -1;
    bool m_scanPending = true;
};
#endif

} // namespace fuse::platform
