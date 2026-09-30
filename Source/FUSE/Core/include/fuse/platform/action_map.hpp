#pragma once

// UNI-INPUT-1: named game actions over keyboard, mouse and gamepads.
//
// An ActionMap holds named *button* actions ("Jump": pressed / held / released) and *axis*
// actions ("MoveX": a value) bound to keys, mouse buttons, mouse motion, gamepad buttons and
// gamepad axes. `update()` evaluates every action from an InputState + GamepadState once per
// frame. Bindings can be changed at run time (rebinding UI: `captureBinding` reports the first
// input the player touches) and saved to / loaded from a JSON file in the project
// (e.g. `<project>/config/input.json`).
//
// Axis value = clamp(sum of key / button / gamepad contributions, -1, 1) + mouse contributions
// (mouse deltas are not clamped: they are pixels x scale x mouse sensitivity). A button action
// is held while any binding is active; a gamepad axis bound to a button action is active when
// axis x scale >= threshold.
//
// PlayerController bundles one player's InputState, GamepadState (fed by a GamepadBackend) and
// ActionMap with the `in.mouseSensitivity` cvar — the object the runtime and PIE drive.

#include <fuse/config/cvar.hpp>
#include <fuse/platform/gamepad.hpp>
#include <fuse/platform/input.hpp>
#include <fuse/types.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace fuse::platform {

class EventPump;
struct PlatformEvent;

inline constexpr u8 kAnyGamepad = 0xFF;

enum class InputSourceKind : u8 { Key, MouseButton, MouseAxis, GamepadButton, GamepadAxis };
enum class MouseAxisCode : u8 { X, Y };

struct InputBinding {
    InputSourceKind kind = InputSourceKind::Key;
    u16 code = 0;
    /// Contribution when active (digital) or multiplier (analog); negative inverts.
    f32 scale = 1.f;
    /// Analog source bound to a button action: active when value x scale >= threshold.
    f32 threshold = 0.5f;
    /// Gamepad slot, or kAnyGamepad (the controller's pad / any connected pad).
    u8 gamepad = kAnyGamepad;

    static InputBinding key(Key k, f32 scale = 1.f) {
        return {InputSourceKind::Key, static_cast<u16>(k), scale, 0.5f, kAnyGamepad};
    }
    static InputBinding mouseButton(MouseButton b, f32 scale = 1.f) {
        return {InputSourceKind::MouseButton, static_cast<u16>(b), scale, 0.5f, kAnyGamepad};
    }
    static InputBinding mouseAxis(MouseAxisCode a, f32 scale = 1.f) {
        return {InputSourceKind::MouseAxis, static_cast<u16>(a), scale, 0.5f, kAnyGamepad};
    }
    static InputBinding gamepadButton(GamepadButton b, f32 scale = 1.f, u8 pad = kAnyGamepad) {
        return {InputSourceKind::GamepadButton, static_cast<u16>(b), scale, 0.5f, pad};
    }
    static InputBinding gamepadAxis(GamepadAxis a, f32 scale = 1.f, u8 pad = kAnyGamepad, f32 threshold = 0.5f) {
        return {InputSourceKind::GamepadAxis, static_cast<u16>(a), scale, threshold, pad};
    }

    bool operator==(const InputBinding& o) const {
        return kind == o.kind && code == o.code && scale == o.scale && threshold == o.threshold &&
               gamepad == o.gamepad;
    }
};

/// "Key:W", "Mouse:Left", "MouseAxis:X", "Pad:South", "PadAxis:LeftX" (source only; scale,
/// threshold and pad are separate JSON fields).
std::string bindingSourceName(const InputBinding& binding);
/// Parses the source part into `binding.kind` / `binding.code`; false when unknown.
bool parseBindingSource(std::string_view text, InputBinding& binding);

enum class ActionKind : u8 { Button, Axis };

class ActionMap {
public:
    using ActionId = u32;
    static constexpr ActionId kInvalidAction = ~0u;

    /// Adds (or returns the existing) action; kInvalidAction when the name is empty or exists
    /// with the other kind.
    ActionId addButton(std::string_view name);
    ActionId addAxis(std::string_view name);
    [[nodiscard]] ActionId find(std::string_view name) const;
    bool remove(std::string_view name);

    /// Appends a binding (duplicates ignored). False when the action is unknown.
    bool bind(std::string_view action, const InputBinding& binding);
    bool unbind(std::string_view action, const InputBinding& binding);
    /// Replace binding `slot` of `action` (slot == count appends). False when out of range.
    bool rebind(std::string_view action, u32 slot, const InputBinding& binding);
    bool clearBindings(std::string_view action);
    [[nodiscard]] const std::vector<InputBinding>* bindings(std::string_view action) const;

    [[nodiscard]] usize actionCount() const { return m_actions.size(); }
    [[nodiscard]] const std::string& actionName(ActionId id) const;
    [[nodiscard]] ActionKind actionKind(ActionId id) const;

    void setMouseSensitivity(f32 sensitivity) { m_mouseSensitivity = sensitivity; }
    [[nodiscard]] f32 mouseSensitivity() const { return m_mouseSensitivity; }

    /// Evaluate every action. `gamepads` may be null. Bindings with kAnyGamepad read pad
    /// `gamepadSlot`, or every connected pad when that is kAnyGamepad too (strongest wins).
    void update(const InputState& input, const GamepadState* gamepads, u8 gamepadSlot = kAnyGamepad);
    /// Release every action (focus loss / PIE stop): held actions report released next query.
    void resetState();

    [[nodiscard]] bool pressed(ActionId id) const;
    [[nodiscard]] bool held(ActionId id) const;
    [[nodiscard]] bool released(ActionId id) const;
    [[nodiscard]] f32 axis(ActionId id) const;
    [[nodiscard]] bool pressed(std::string_view name) const { return pressed(find(name)); }
    [[nodiscard]] bool held(std::string_view name) const { return held(find(name)); }
    [[nodiscard]] bool released(std::string_view name) const { return released(find(name)); }
    [[nodiscard]] f32 axis(std::string_view name) const { return axis(find(name)); }

    /// First input the player started this frame: a key / mouse button / gamepad button pressed,
    /// or a gamepad axis pushed past 0.5 (scale = its sign). For "press a key to rebind" UIs.
    static bool captureBinding(const InputState& input, const GamepadState* gamepads, InputBinding& out);

    // ---- persistence (JSON) ------------------------------------------------------------------
    /// {"version":1,"actions":[{"name","kind":"button"|"axis","bindings":[{"source","scale",
    /// "threshold","pad"}]}]}. Pad is omitted for kAnyGamepad.
    [[nodiscard]] std::string toJson() const;
    /// Replaces the bindings of every action in the document (creating missing actions); actions
    /// not in the document are kept. All-or-nothing: on error nothing changes.
    bool fromJson(std::string_view json, std::string* error = nullptr);
    bool saveFile(const std::filesystem::path& path) const;
    bool loadFile(const std::filesystem::path& path, std::string* error = nullptr);

private:
    struct Action {
        std::string name;
        ActionKind kind = ActionKind::Button;
        std::vector<InputBinding> bindings;
        bool held = false;
        bool prevHeld = false;
        f32 value = 0.f;
    };

    ActionId add(std::string_view name, ActionKind kind);
    Action* findAction(std::string_view name);
    const Action* findAction(std::string_view name) const;

    std::vector<Action> m_actions;
    f32 m_mouseSensitivity = 1.f;
};

/// Names used by PlayerController::defaultActionMap and its helpers.
namespace action_names {
inline constexpr std::string_view MoveX = "MoveX";
inline constexpr std::string_view MoveY = "MoveY";
inline constexpr std::string_view LookX = "LookX";
inline constexpr std::string_view LookY = "LookY";
inline constexpr std::string_view Jump = "Jump";
inline constexpr std::string_view Fire = "Fire";
inline constexpr std::string_view Interact = "Interact";
inline constexpr std::string_view Sprint = "Sprint";
inline constexpr std::string_view Pause = "Pause";
} // namespace action_names

/// One local player's input: keyboard / mouse state, gamepads and the action map.
class PlayerController {
public:
    /// `gamepadSlot`: the pad this player uses (kAnyGamepad = any connected pad).
    explicit PlayerController(u8 gamepadSlot = kAnyGamepad);

    [[nodiscard]] ActionMap& actions() { return m_actions; }
    [[nodiscard]] const ActionMap& actions() const { return m_actions; }
    [[nodiscard]] InputState& input() { return m_input; }
    [[nodiscard]] const InputState& input() const { return m_input; }
    [[nodiscard]] GamepadState& gamepads() { return m_gamepads; }
    [[nodiscard]] const GamepadState& gamepads() const { return m_gamepads; }

    /// Non-owning; null disables gamepad polling (events can still be applied directly).
    void setGamepadBackend(GamepadBackend* backend) { m_backend = backend; }
    [[nodiscard]] GamepadBackend* gamepadBackend() const { return m_backend; }
    /// Mouse sensitivity source (typically engine_cvars().mouseSensitivity); null = 1.
    void setMouseSensitivityCVar(const config::CVar<f32>* cvar) { m_sensitivity = cvar; }
    void setGamepadSlot(u8 slot) { m_slot = slot; }
    [[nodiscard]] u8 gamepadSlot() const { return m_slot; }

    /// Disabled (window unfocused, PIE paused): update() releases every action.
    void setEnabled(bool enabled);
    [[nodiscard]] bool enabled() const { return m_enabled; }

    void beginFrame();
    void apply(const PlatformEvent& event) { m_input.apply(event); }
    void apply(const GamepadEvent& event) { m_gamepads.apply(event); }
    u32 pumpEvents(EventPump& pump) { return m_input.applyPump(pump); }
    /// Polls the backend and applies its events; returns the number of events.
    u32 pollGamepads();
    void update();
    /// beginFrame + pump (when given) + pollGamepads + update.
    void tick(EventPump* pump);

    // ---- default-map helpers -----------------------------------------------------------------
    [[nodiscard]] f32 moveX() const { return m_actions.axis(action_names::MoveX); }
    [[nodiscard]] f32 moveY() const { return m_actions.axis(action_names::MoveY); }
    [[nodiscard]] f32 lookX() const { return m_actions.axis(action_names::LookX); }
    [[nodiscard]] f32 lookY() const { return m_actions.axis(action_names::LookY); }

    /// MoveX/MoveY (WASD, arrows, left stick), LookX/LookY (mouse, right stick), Jump (Space,
    /// South), Fire (left mouse, right trigger), Interact (E, West), Sprint (Shift, left stick
    /// click), Pause (Escape, Start).
    static ActionMap defaultActionMap();

private:
    ActionMap m_actions;
    InputState m_input;
    GamepadState m_gamepads;
    GamepadBackend* m_backend = nullptr;
    const config::CVar<f32>* m_sensitivity = nullptr;
    std::vector<GamepadEvent> m_events;
    u8 m_slot = kAnyGamepad;
    bool m_enabled = true;
};

} // namespace fuse::platform
