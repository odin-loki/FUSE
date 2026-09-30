// UNI-INPUT-1: ActionMap (named actions / axes over keyboard, mouse, gamepads; JSON persistence)
// and PlayerController.

#include <fuse/platform/action_map.hpp>

#include <fuse/config/json.hpp>
#include <fuse/platform/event_pump.hpp>

#include <cmath>
#include <fstream>
#include <sstream>
#include <system_error>

namespace fuse::platform {

namespace {

f32 clampSigned(f32 v) { return v < -1.f ? -1.f : (v > 1.f ? 1.f : v); }

f32 absf(f32 v) { return v < 0.f ? -v : v; }

std::string_view mouseAxisName(u16 code) {
    return code == static_cast<u16>(MouseAxisCode::X) ? "X" : code == static_cast<u16>(MouseAxisCode::Y) ? "Y" : "";
}

/// Value of a gamepad axis for a binding: its pad, else the strongest over every connected pad.
f32 gamepadAxisValue(const GamepadState& pads, u8 pad, GamepadAxis a) {
    if (pad != kAnyGamepad) {
        return pads.axis(pad, a);
    }
    f32 best = 0.f;
    for (u32 p = 0; p < kMaxGamepads; ++p) {
        if (!pads.connected(p)) {
            continue;
        }
        const f32 v = pads.axis(p, a);
        if (absf(v) > absf(best)) {
            best = v;
        }
    }
    return best;
}

bool gamepadButtonValue(const GamepadState& pads, u8 pad, GamepadButton b) {
    if (pad != kAnyGamepad) {
        return pads.buttonDown(pad, b);
    }
    for (u32 p = 0; p < kMaxGamepads; ++p) {
        if (pads.buttonDown(p, b)) {
            return true;
        }
    }
    return false;
}

} // namespace

std::string bindingSourceName(const InputBinding& binding) {
    switch (binding.kind) {
    case InputSourceKind::Key:
        return "Key:" + std::string(keyName(static_cast<Key>(binding.code)));
    case InputSourceKind::MouseButton:
        return "Mouse:" + std::string(mouseButtonName(static_cast<MouseButton>(binding.code)));
    case InputSourceKind::MouseAxis:
        return "MouseAxis:" + std::string(mouseAxisName(binding.code));
    case InputSourceKind::GamepadButton:
        return "Pad:" + std::string(gamepadButtonName(static_cast<GamepadButton>(binding.code)));
    case InputSourceKind::GamepadAxis:
        return "PadAxis:" + std::string(gamepadAxisName(static_cast<GamepadAxis>(binding.code)));
    }
    return {};
}

bool parseBindingSource(std::string_view text, InputBinding& binding) {
    const usize colon = text.find(':');
    if (colon == std::string_view::npos) {
        return false;
    }
    const std::string_view kind = text.substr(0, colon);
    const std::string_view name = text.substr(colon + 1);
    if (kind == "Key") {
        const Key k = keyFromName(name);
        if (k == Key::COUNT) {
            return false;
        }
        binding.kind = InputSourceKind::Key;
        binding.code = static_cast<u16>(k);
        return true;
    }
    if (kind == "Mouse") {
        const MouseButton b = mouseButtonFromName(name);
        if (b == MouseButton::COUNT) {
            return false;
        }
        binding.kind = InputSourceKind::MouseButton;
        binding.code = static_cast<u16>(b);
        return true;
    }
    if (kind == "MouseAxis") {
        if (name == "X" || name == "x") {
            binding.code = static_cast<u16>(MouseAxisCode::X);
        } else if (name == "Y" || name == "y") {
            binding.code = static_cast<u16>(MouseAxisCode::Y);
        } else {
            return false;
        }
        binding.kind = InputSourceKind::MouseAxis;
        return true;
    }
    if (kind == "Pad") {
        const GamepadButton b = gamepadButtonFromName(name);
        if (b == GamepadButton::COUNT) {
            return false;
        }
        binding.kind = InputSourceKind::GamepadButton;
        binding.code = static_cast<u16>(b);
        return true;
    }
    if (kind == "PadAxis") {
        const GamepadAxis a = gamepadAxisFromName(name);
        if (a == GamepadAxis::COUNT) {
            return false;
        }
        binding.kind = InputSourceKind::GamepadAxis;
        binding.code = static_cast<u16>(a);
        return true;
    }
    return false;
}

// ---- ActionMap --------------------------------------------------------------------------------

ActionMap::ActionId ActionMap::add(std::string_view name, ActionKind kind) {
    if (name.empty()) {
        return kInvalidAction;
    }
    const ActionId existing = find(name);
    if (existing != kInvalidAction) {
        return m_actions[existing].kind == kind ? existing : kInvalidAction;
    }
    Action a;
    a.name = std::string(name);
    a.kind = kind;
    m_actions.push_back(std::move(a));
    return static_cast<ActionId>(m_actions.size() - 1);
}

ActionMap::ActionId ActionMap::addButton(std::string_view name) { return add(name, ActionKind::Button); }
ActionMap::ActionId ActionMap::addAxis(std::string_view name) { return add(name, ActionKind::Axis); }

ActionMap::ActionId ActionMap::find(std::string_view name) const {
    for (usize i = 0; i < m_actions.size(); ++i) {
        if (m_actions[i].name == name) {
            return static_cast<ActionId>(i);
        }
    }
    return kInvalidAction;
}

bool ActionMap::remove(std::string_view name) {
    const ActionId id = find(name);
    if (id == kInvalidAction) {
        return false;
    }
    m_actions.erase(m_actions.begin() + static_cast<std::ptrdiff_t>(id));
    return true;
}

ActionMap::Action* ActionMap::findAction(std::string_view name) {
    const ActionId id = find(name);
    return id == kInvalidAction ? nullptr : &m_actions[id];
}

const ActionMap::Action* ActionMap::findAction(std::string_view name) const {
    const ActionId id = find(name);
    return id == kInvalidAction ? nullptr : &m_actions[id];
}

bool ActionMap::bind(std::string_view action, const InputBinding& binding) {
    Action* a = findAction(action);
    if (a == nullptr) {
        return false;
    }
    for (const InputBinding& b : a->bindings) {
        if (b == binding) {
            return true;
        }
    }
    a->bindings.push_back(binding);
    return true;
}

bool ActionMap::unbind(std::string_view action, const InputBinding& binding) {
    Action* a = findAction(action);
    if (a == nullptr) {
        return false;
    }
    for (auto it = a->bindings.begin(); it != a->bindings.end(); ++it) {
        if (*it == binding) {
            a->bindings.erase(it);
            return true;
        }
    }
    return false;
}

bool ActionMap::rebind(std::string_view action, u32 slot, const InputBinding& binding) {
    Action* a = findAction(action);
    if (a == nullptr || slot > a->bindings.size()) {
        return false;
    }
    if (slot == a->bindings.size()) {
        a->bindings.push_back(binding);
    } else {
        a->bindings[slot] = binding;
    }
    return true;
}

bool ActionMap::clearBindings(std::string_view action) {
    Action* a = findAction(action);
    if (a == nullptr) {
        return false;
    }
    a->bindings.clear();
    return true;
}

const std::vector<InputBinding>* ActionMap::bindings(std::string_view action) const {
    const Action* a = findAction(action);
    return a != nullptr ? &a->bindings : nullptr;
}

const std::string& ActionMap::actionName(ActionId id) const {
    static const std::string kEmpty;
    return id < m_actions.size() ? m_actions[id].name : kEmpty;
}

ActionKind ActionMap::actionKind(ActionId id) const {
    return id < m_actions.size() ? m_actions[id].kind : ActionKind::Button;
}

void ActionMap::update(const InputState& input, const GamepadState* gamepads, u8 gamepadSlot) {
    for (Action& a : m_actions) {
        a.prevHeld = a.held;
        f32 clamped = 0.f; // keys / buttons / gamepad
        f32 mouse = 0.f;   // unclamped pixels
        bool active = false;
        for (const InputBinding& b : a.bindings) {
            const u8 pad = b.gamepad != kAnyGamepad ? b.gamepad : gamepadSlot;
            switch (b.kind) {
            case InputSourceKind::Key:
                if (input.keyDown(static_cast<Key>(b.code))) {
                    clamped += b.scale;
                    active = true;
                }
                break;
            case InputSourceKind::MouseButton:
                if (input.mouseDown(static_cast<MouseButton>(b.code))) {
                    clamped += b.scale;
                    active = true;
                }
                break;
            case InputSourceKind::MouseAxis: {
                const i32 delta = b.code == static_cast<u16>(MouseAxisCode::X) ? input.mouseDeltaX() : input.mouseDeltaY();
                const f32 v = static_cast<f32>(delta) * b.scale * m_mouseSensitivity;
                mouse += v;
                if (a.kind == ActionKind::Button ? v >= b.threshold : v != 0.f) {
                    active = true;
                }
                break;
            }
            case InputSourceKind::GamepadButton:
                if (gamepads != nullptr && gamepadButtonValue(*gamepads, pad, static_cast<GamepadButton>(b.code))) {
                    clamped += b.scale;
                    active = true;
                }
                break;
            case InputSourceKind::GamepadAxis: {
                if (gamepads == nullptr) {
                    break;
                }
                const f32 v = gamepadAxisValue(*gamepads, pad, static_cast<GamepadAxis>(b.code)) * b.scale;
                if (a.kind == ActionKind::Button) {
                    active = active || v >= b.threshold;
                } else {
                    clamped += v;
                    active = active || v != 0.f;
                }
                break;
            }
            }
        }
        if (a.kind == ActionKind::Axis) {
            a.value = clampSigned(clamped) + mouse;
            a.held = a.value != 0.f;
        } else {
            a.held = active;
            a.value = active ? 1.f : 0.f;
        }
    }
}

void ActionMap::resetState() {
    for (Action& a : m_actions) {
        a.prevHeld = a.held;
        a.held = false;
        a.value = 0.f;
    }
}

bool ActionMap::pressed(ActionId id) const {
    return id < m_actions.size() && m_actions[id].held && !m_actions[id].prevHeld;
}

bool ActionMap::held(ActionId id) const { return id < m_actions.size() && m_actions[id].held; }

bool ActionMap::released(ActionId id) const {
    return id < m_actions.size() && !m_actions[id].held && m_actions[id].prevHeld;
}

f32 ActionMap::axis(ActionId id) const { return id < m_actions.size() ? m_actions[id].value : 0.f; }

bool ActionMap::captureBinding(const InputState& input, const GamepadState* gamepads, InputBinding& out) {
    for (u32 k = 0; k < static_cast<u32>(Key::COUNT); ++k) {
        if (input.keyPressed(static_cast<Key>(k))) {
            out = InputBinding::key(static_cast<Key>(k));
            return true;
        }
    }
    for (u32 b = 0; b < static_cast<u32>(MouseButton::COUNT); ++b) {
        if (input.mousePressed(static_cast<MouseButton>(b))) {
            out = InputBinding::mouseButton(static_cast<MouseButton>(b));
            return true;
        }
    }
    if (gamepads == nullptr) {
        return false;
    }
    for (u32 p = 0; p < kMaxGamepads; ++p) {
        if (!gamepads->connected(p)) {
            continue;
        }
        for (u32 b = 0; b < static_cast<u32>(GamepadButton::COUNT); ++b) {
            if (gamepads->buttonPressed(p, static_cast<GamepadButton>(b))) {
                out = InputBinding::gamepadButton(static_cast<GamepadButton>(b));
                return true;
            }
        }
        for (u32 a = 0; a < static_cast<u32>(GamepadAxis::COUNT); ++a) {
            const f32 v = gamepads->axis(p, static_cast<GamepadAxis>(a));
            if (absf(v) >= 0.5f) {
                out = InputBinding::gamepadAxis(static_cast<GamepadAxis>(a), v < 0.f ? -1.f : 1.f);
                return true;
            }
        }
    }
    return false;
}

std::string ActionMap::toJson() const {
    using config::json::Value;
    Value root = Value::object();
    root.set("version", Value::number(1));
    Value& list = root.set("actions", Value::array());
    for (const Action& a : m_actions) {
        Value action = Value::object();
        action.set("name", Value::string(a.name));
        action.set("kind", Value::string(a.kind == ActionKind::Axis ? "axis" : "button"));
        Value& bindingList = action.set("bindings", Value::array());
        for (const InputBinding& b : a.bindings) {
            Value binding = Value::object();
            binding.set("source", Value::string(bindingSourceName(b)));
            binding.set("scale", Value::number(b.scale));
            if (b.kind == InputSourceKind::GamepadAxis || b.kind == InputSourceKind::MouseAxis) {
                binding.set("threshold", Value::number(b.threshold));
            }
            if (b.gamepad != kAnyGamepad) {
                binding.set("pad", Value::number(b.gamepad));
            }
            bindingList.push(std::move(binding));
        }
        list.push(std::move(action));
    }
    return config::json::write(root, true);
}

bool ActionMap::fromJson(std::string_view json, std::string* error) {
    using config::json::Type;
    using config::json::Value;
    const auto fail = [error](std::string message) {
        if (error != nullptr) {
            *error = std::move(message);
        }
        return false;
    };
    Value root;
    std::string parseError;
    if (!config::json::parse(json, root, &parseError)) {
        return fail("input map JSON: " + parseError);
    }
    if (!root.is_object()) {
        return fail("input map JSON: root must be an object");
    }
    if (const Value* version = root.find("version"); version != nullptr && version->as_number(0.0) != 1.0) {
        return fail("input map JSON: unsupported version");
    }
    const Value* list = root.find("actions");
    if (list == nullptr || !list->is_array()) {
        return fail("input map JSON: 'actions' array missing");
    }
    struct Parsed {
        std::string name;
        ActionKind kind;
        std::vector<InputBinding> bindings;
    };
    std::vector<Parsed> parsed;
    for (const Value& item : list->items()) {
        const Value* name = item.find("name");
        const Value* kind = item.find("kind");
        if (name == nullptr || !name->is_string() || name->as_string().empty()) {
            return fail("input map JSON: action without a name");
        }
        Parsed p;
        p.name = name->as_string();
        const std::string kindText = kind != nullptr && kind->is_string() ? kind->as_string() : "button";
        if (kindText == "button") {
            p.kind = ActionKind::Button;
        } else if (kindText == "axis") {
            p.kind = ActionKind::Axis;
        } else {
            return fail("input map JSON: action '" + p.name + "' has unknown kind '" + kindText + "'");
        }
        const ActionId existing = find(p.name);
        if (existing != kInvalidAction && m_actions[existing].kind != p.kind) {
            return fail("input map JSON: action '" + p.name + "' changes kind");
        }
        if (const Value* bindingList = item.find("bindings"); bindingList != nullptr) {
            if (!bindingList->is_array()) {
                return fail("input map JSON: action '" + p.name + "': 'bindings' must be an array");
            }
            for (const Value& b : bindingList->items()) {
                const Value* source = b.find("source");
                InputBinding binding;
                if (source == nullptr || !source->is_string() || !parseBindingSource(source->as_string(), binding)) {
                    return fail("input map JSON: action '" + p.name + "': unknown binding source '" +
                                (source != nullptr && source->is_string() ? source->as_string() : std::string()) + "'");
                }
                if (const Value* scale = b.find("scale")) {
                    if (!scale->is_number()) {
                        return fail("input map JSON: action '" + p.name + "': 'scale' must be a number");
                    }
                    binding.scale = static_cast<f32>(scale->as_number());
                }
                if (const Value* threshold = b.find("threshold")) {
                    if (!threshold->is_number()) {
                        return fail("input map JSON: action '" + p.name + "': 'threshold' must be a number");
                    }
                    binding.threshold = static_cast<f32>(threshold->as_number());
                }
                if (const Value* pad = b.find("pad")) {
                    const f64 v = pad->as_number(-1.0);
                    if (!pad->is_number() || v < 0.0 || v >= static_cast<f64>(kMaxGamepads) || v != std::floor(v)) {
                        return fail("input map JSON: action '" + p.name + "': 'pad' must be 0.." +
                                    std::to_string(kMaxGamepads - 1));
                    }
                    binding.gamepad = static_cast<u8>(v);
                }
                p.bindings.push_back(binding);
            }
        }
        parsed.push_back(std::move(p));
    }
    for (Parsed& p : parsed) {
        const ActionId id = add(p.name, p.kind);
        m_actions[id].bindings = std::move(p.bindings);
    }
    return true;
}

bool ActionMap::saveFile(const std::filesystem::path& path) const {
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    std::filesystem::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
        const std::string text = toJson();
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!out) {
            return false;
        }
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::error_code rm;
        std::filesystem::remove(path, rm);
        ec.clear();
        std::filesystem::rename(tmp, path, ec);
    }
    return !ec;
}

bool ActionMap::loadFile(const std::filesystem::path& path, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error != nullptr) {
            *error = "cannot open " + path.string();
        }
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return fromJson(ss.str(), error);
}

// ---- PlayerController -------------------------------------------------------------------------

PlayerController::PlayerController(u8 gamepadSlot) : m_actions(defaultActionMap()), m_slot(gamepadSlot) {
    m_events.reserve(64);
}

void PlayerController::setEnabled(bool enabled) {
    if (m_enabled && !enabled) {
        m_input.releaseAll();
        m_actions.resetState();
    }
    m_enabled = enabled;
}

void PlayerController::beginFrame() {
    m_input.beginFrame();
    m_gamepads.beginFrame();
}

u32 PlayerController::pollGamepads() {
    if (m_backend == nullptr) {
        return 0;
    }
    m_events.clear();
    const u32 n = m_backend->poll(m_events);
    for (const GamepadEvent& e : m_events) {
        m_gamepads.apply(e);
    }
    return n;
}

void PlayerController::update() {
    if (!m_enabled) {
        m_actions.resetState();
        return;
    }
    m_actions.setMouseSensitivity(m_sensitivity != nullptr ? m_sensitivity->get() : 1.f);
    m_actions.update(m_input, &m_gamepads, m_slot);
}

void PlayerController::tick(EventPump* pump) {
    beginFrame();
    if (pump != nullptr) {
        (void)pumpEvents(*pump);
    }
    (void)pollGamepads();
    update();
}

ActionMap PlayerController::defaultActionMap() {
    ActionMap map;
    map.addAxis(action_names::MoveX);
    map.bind(action_names::MoveX, InputBinding::key(Key::D, 1.f));
    map.bind(action_names::MoveX, InputBinding::key(Key::A, -1.f));
    map.bind(action_names::MoveX, InputBinding::key(Key::Right, 1.f));
    map.bind(action_names::MoveX, InputBinding::key(Key::Left, -1.f));
    map.bind(action_names::MoveX, InputBinding::gamepadAxis(GamepadAxis::LeftX));

    map.addAxis(action_names::MoveY);
    map.bind(action_names::MoveY, InputBinding::key(Key::W, 1.f));
    map.bind(action_names::MoveY, InputBinding::key(Key::S, -1.f));
    map.bind(action_names::MoveY, InputBinding::key(Key::Up, 1.f));
    map.bind(action_names::MoveY, InputBinding::key(Key::Down, -1.f));
    map.bind(action_names::MoveY, InputBinding::gamepadAxis(GamepadAxis::LeftY));

    map.addAxis(action_names::LookX);
    map.bind(action_names::LookX, InputBinding::mouseAxis(MouseAxisCode::X, 1.f));
    map.bind(action_names::LookX, InputBinding::gamepadAxis(GamepadAxis::RightX));

    // Screen Y grows downwards: moving the mouse up looks up (+).
    map.addAxis(action_names::LookY);
    map.bind(action_names::LookY, InputBinding::mouseAxis(MouseAxisCode::Y, -1.f));
    map.bind(action_names::LookY, InputBinding::gamepadAxis(GamepadAxis::RightY));

    map.addButton(action_names::Jump);
    map.bind(action_names::Jump, InputBinding::key(Key::Space));
    map.bind(action_names::Jump, InputBinding::gamepadButton(GamepadButton::South));

    map.addButton(action_names::Fire);
    map.bind(action_names::Fire, InputBinding::mouseButton(MouseButton::Left));
    map.bind(action_names::Fire, InputBinding::gamepadAxis(GamepadAxis::RightTrigger));

    map.addButton(action_names::Interact);
    map.bind(action_names::Interact, InputBinding::key(Key::E));
    map.bind(action_names::Interact, InputBinding::gamepadButton(GamepadButton::West));

    map.addButton(action_names::Sprint);
    map.bind(action_names::Sprint, InputBinding::key(Key::Shift));
    map.bind(action_names::Sprint, InputBinding::gamepadButton(GamepadButton::LeftStick));

    map.addButton(action_names::Pause);
    map.bind(action_names::Pause, InputBinding::key(Key::Escape));
    map.bind(action_names::Pause, InputBinding::gamepadButton(GamepadButton::Start));
    return map;
}

} // namespace fuse::platform
