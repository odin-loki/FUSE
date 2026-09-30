// UNI-INPUT-1 gates: synthetic key / mouse / gamepad events -> action states and axes, gamepad
// deadzones and connect / disconnect, XInput snapshot diff and evdev mapping (the backends'
// translation layers), the evdev read loop on a pipe (Linux), rebinding + JSON persistence, and
// PlayerController with the in.mouseSensitivity cvar.

#include <fuse/config/cvar.hpp>
#include <fuse/core/temp_path.hpp>
#include <fuse/platform/action_map.hpp>
#include <fuse/platform/event_pump.hpp>
#include <fuse/platform/gamepad.hpp>
#include <fuse/platform/input.hpp>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#if defined(FUSE_PLATFORM_HAS_EVDEV_GAMEPAD)
#include <fcntl.h>
#include <linux/input.h>
#include <unistd.h>
#endif

using namespace fuse;
using namespace fuse::platform;

namespace {

int g_failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

bool approx(f32 a, f32 b, f32 eps = 1e-4f) { return std::fabs(a - b) <= eps; }

PlatformEvent keyEvent(bool down, u32 code) {
    PlatformEvent e{};
    e.type = down ? PlatformEventType::KeyDown : PlatformEventType::KeyUp;
    e.keyCode = code;
    return e;
}

PlatformEvent mouseButtonEvent(bool down, u8 button) {
    PlatformEvent e{};
    e.type = down ? PlatformEventType::MouseButtonDown : PlatformEventType::MouseButtonUp;
    e.mouseButton = button;
    return e;
}

PlatformEvent rawMouse(i32 dx, i32 dy) {
    PlatformEvent e{};
    e.type = PlatformEventType::RawMouseDelta;
    e.mouseX = dx;
    e.mouseY = dy;
    return e;
}

void testKeyNamesAndMouseEdges() {
    expect(keyFromName("space") == Key::Space && keyFromName("W") == Key::W && keyFromName("f12") == Key::F12,
           "key names (case-insensitive)");
    expect(keyFromName("Esc") == Key::Escape && keyFromName("nope") == Key::COUNT, "key aliases / unknown");
    for (u32 k = 0; k < static_cast<u32>(Key::COUNT); ++k) {
        if (keyFromName(keyName(static_cast<Key>(k))) != static_cast<Key>(k)) {
            expect(false, "every key name round-trips");
            break;
        }
    }
    expect(mouseButtonFromName("left") == MouseButton::Left && mouseButtonName(MouseButton::X2) == "X2",
           "mouse button names");

    InputState in;
    in.beginFrame();
    in.apply(mouseButtonEvent(true, 1));
    expect(in.mousePressed(MouseButton::Left) && in.mouseDown(MouseButton::Left), "mouse pressed this frame");
    in.beginFrame();
    expect(!in.mousePressed(MouseButton::Left) && in.mouseDown(MouseButton::Left), "pressed clears next frame");
    in.apply(mouseButtonEvent(false, 1));
    expect(in.mouseReleased(MouseButton::Left) && !in.mouseDown(MouseButton::Left), "mouse released");
    in.beginFrame();
    in.apply(keyEvent(true, 'W'));
    in.beginFrame();
    in.releaseAll();
    expect(!in.keyDown(Key::W) && in.keyReleased(Key::W), "releaseAll releases held keys");
}

void testDeadzones() {
    f32 x = 0.f;
    f32 y = 0.f;
    applyStickDeadzone(0.1f, 0.1f, 0.25f, x, y);
    expect(x == 0.f && y == 0.f, "inside radial deadzone -> 0");
    applyStickDeadzone(1.f, 0.f, 0.25f, x, y);
    expect(approx(x, 1.f) && y == 0.f, "full deflection -> 1");
    applyStickDeadzone(0.625f, 0.f, 0.25f, x, y);
    expect(approx(x, 0.5f), "rescaled from [dz,1] to [0,1]");
    applyStickDeadzone(0.6f, 0.8f, 0.25f, x, y); // magnitude 1, direction (0.6, 0.8)
    expect(approx(x, 0.6f) && approx(y, 0.8f), "diagonal keeps direction");
    applyStickDeadzone(1.f, 1.f, 0.25f, x, y);
    expect(approx(std::sqrt(x * x + y * y), 1.f), "corner clamped to unit magnitude");
    expect(applyTriggerDeadzone(0.05f, 0.1f) == 0.f && approx(applyTriggerDeadzone(0.55f, 0.1f), 0.5f) &&
               approx(applyTriggerDeadzone(1.f, 0.1f), 1.f),
           "trigger deadzone");
}

void testGamepadState() {
    GamepadState pads;
    pads.beginFrame();
    pads.apply(GamepadEvent::connected(1));
    expect(pads.connected(1) && pads.connectedThisFrame(1) && pads.connectedCount() == 1 && pads.firstConnected() == 1,
           "connect");
    pads.apply(GamepadEvent::button(1, GamepadButton::South, true));
    pads.apply(GamepadEvent::axis(1, GamepadAxis::LeftX, 0.1f));
    pads.apply(GamepadEvent::axis(1, GamepadAxis::RightTrigger, 2.f));
    expect(pads.buttonPressed(1, GamepadButton::South) && pads.buttonDown(1, GamepadButton::South), "button press");
    expect(pads.rawAxis(1, GamepadAxis::LeftX) == 0.1f && pads.axis(1, GamepadAxis::LeftX) == 0.f,
           "small stick input eaten by the deadzone");
    expect(pads.rawAxis(1, GamepadAxis::RightTrigger) == 1.f && pads.axis(1, GamepadAxis::RightTrigger) == 1.f,
           "trigger clamped to 1");
    pads.beginFrame();
    expect(!pads.buttonPressed(1, GamepadButton::South) && pads.buttonDown(1, GamepadButton::South), "held");
    pads.apply(GamepadEvent::disconnected(1));
    expect(!pads.connected(1) && pads.disconnectedThisFrame(1) && pads.buttonReleased(1, GamepadButton::South) &&
               !pads.buttonDown(1, GamepadButton::South) && pads.rawAxis(1, GamepadAxis::RightTrigger) == 0.f,
           "disconnect releases buttons and zeroes axes");
    pads.apply(GamepadEvent::button(9, GamepadButton::South, true));
    expect(pads.connectedCount() == 0, "out-of-range pad ignored");
    expect(gamepadButtonFromName("a") == GamepadButton::South && gamepadButtonFromName("RB") == GamepadButton::RightShoulder &&
               gamepadAxisFromName("rt") == GamepadAxis::RightTrigger,
           "gamepad name aliases");
}

void testActionsFromSyntheticEvents() {
    ActionMap map = PlayerController::defaultActionMap();
    InputState in;
    GamepadState pads;

    in.beginFrame();
    pads.beginFrame();
    in.apply(keyEvent(true, ' '));
    in.apply(keyEvent(true, 'D'));
    map.update(in, &pads);
    expect(map.pressed("Jump") && map.held("Jump") && !map.released("Jump"), "Space -> Jump pressed");
    expect(map.axis("MoveX") == 1.f && map.axis("MoveY") == 0.f, "D -> MoveX +1");

    in.beginFrame();
    in.apply(keyEvent(true, 'A'));
    map.update(in, &pads);
    expect(!map.pressed("Jump") && map.held("Jump"), "Jump held on the next frame");
    expect(map.axis("MoveX") == 0.f, "D + A cancel");

    in.beginFrame();
    in.apply(keyEvent(false, ' '));
    in.apply(keyEvent(false, 'D'));
    in.apply(keyEvent(true, 0x27)); // VK_RIGHT: ignored? no - Right arrow +1, A -1 -> 0
    map.update(in, &pads);
    expect(map.released("Jump") && !map.held("Jump"), "Jump released");
    expect(map.axis("MoveX") == 0.f, "Right arrow + A cancel");

    in.beginFrame();
    in.apply(keyEvent(false, 'A'));
    in.apply(keyEvent(true, 'D'));
    map.update(in, &pads);
    expect(map.axis("MoveX") == 1.f, "D + Right arrow clamp to 1");

    // Mouse look: raw deltas x sensitivity, not clamped; Y inverted (screen down = look down).
    in.beginFrame();
    in.apply(rawMouse(12, -4));
    map.setMouseSensitivity(0.5f);
    map.update(in, &pads);
    expect(map.axis("LookX") == 6.f && map.axis("LookY") == 2.f, "mouse look deltas x sensitivity");
    in.beginFrame();
    map.update(in, &pads);
    expect(map.axis("LookX") == 0.f, "no motion -> 0");

    // Mouse button -> Fire.
    in.apply(mouseButtonEvent(true, 1));
    map.update(in, &pads);
    expect(map.pressed("Fire"), "left mouse -> Fire");
    in.apply(mouseButtonEvent(false, 1));

    // Gamepad: left stick -> MoveX/MoveY (deadzone applied), trigger as a button (threshold),
    // face button -> Jump.
    in.beginFrame();
    in.apply(keyEvent(false, 'D'));
    in.apply(keyEvent(false, 0x27));
    pads.beginFrame();
    pads.apply(GamepadEvent::connected(0));
    pads.apply(GamepadEvent::axis(0, GamepadAxis::LeftX, 1.f));
    pads.apply(GamepadEvent::axis(0, GamepadAxis::LeftY, 0.1f));
    pads.apply(GamepadEvent::axis(0, GamepadAxis::RightTrigger, 0.4f));
    pads.apply(GamepadEvent::button(0, GamepadButton::South, true));
    map.update(in, &pads);
    expect(approx(map.axis("MoveX"), 0.9950f, 1e-3f) || approx(map.axis("MoveX"), 1.f, 5e-3f), "stick X -> MoveX");
    expect(map.axis("MoveY") > 0.f && map.axis("MoveY") < 0.1f, "small Y on a deflected stick survives (radial)");
    expect(map.pressed("Jump"), "South -> Jump");
    expect(!map.held("Fire"), "trigger below threshold (0.4 raw -> 0.32) is not Fire");
    pads.beginFrame();
    pads.apply(GamepadEvent::axis(0, GamepadAxis::RightTrigger, 0.9f));
    map.update(in, &pads);
    expect(map.pressed("Fire"), "trigger above threshold -> Fire pressed");

    // Pad slot filter: a controller bound to pad 1 ignores pad 0.
    pads.beginFrame();
    map.update(in, &pads, 1);
    expect(!map.held("Jump") && map.axis("MoveX") == 0.f, "slot 1 ignores pad 0");
    map.update(in, &pads, 0);
    expect(map.held("Jump"), "slot 0 sees pad 0");

    // Disconnect while held -> action released.
    pads.beginFrame();
    pads.apply(GamepadEvent::disconnected(0));
    map.update(in, &pads);
    expect(map.released("Jump") && map.axis("MoveX") == 0.f, "disconnect releases actions");

    map.resetState();
    expect(!map.held("Jump") && map.axis("LookX") == 0.f, "resetState");
    expect(map.find("Nope") == ActionMap::kInvalidAction && !map.pressed("Nope") && map.axis("Nope") == 0.f,
           "unknown action reads as idle");
    expect(map.addButton("MoveX") == ActionMap::kInvalidAction, "kind clash refused");
}

void testXInputDiff() {
    std::vector<GamepadEvent> ev;
    XInputPadSnapshot off{};
    XInputPadSnapshot on{};
    on.connected = true;
    on.buttons = xinput_bits::A | xinput_bits::DPadLeft;
    on.thumbLX = -32768;
    on.thumbRY = 32767;
    on.rightTrigger = 255;
    diffXInputSnapshot(2, off, on, ev);
    GamepadState pads;
    pads.beginFrame();
    pads.applyAll(ev);
    expect(!ev.empty() && ev[0].type == GamepadEventType::Connected && ev[0].pad == 2, "connect first");
    expect(pads.buttonDown(2, GamepadButton::South) && pads.buttonDown(2, GamepadButton::DPadLeft), "buttons mapped");
    expect(pads.rawAxis(2, GamepadAxis::LeftX) == -1.f && pads.rawAxis(2, GamepadAxis::RightY) == 1.f &&
               pads.rawAxis(2, GamepadAxis::RightTrigger) == 1.f,
           "axes normalised");
    ev.clear();
    diffXInputSnapshot(2, on, on, ev);
    expect(ev.empty(), "no change -> no events");
    XInputPadSnapshot next = on;
    next.buttons = xinput_bits::B | xinput_bits::Guide;
    next.thumbLX = 0;
    diffXInputSnapshot(2, on, next, ev);
    pads.beginFrame();
    pads.applyAll(ev);
    expect(ev.size() == 5, "A up, DPadLeft up, B down, Guide down, LeftX");
    expect(pads.buttonReleased(2, GamepadButton::South) && pads.buttonPressed(2, GamepadButton::East) &&
               pads.buttonDown(2, GamepadButton::Guide) && pads.rawAxis(2, GamepadAxis::LeftX) == 0.f,
           "state follows");
    ev.clear();
    diffXInputSnapshot(2, next, off, ev);
    pads.beginFrame();
    pads.applyAll(ev);
    expect(ev.size() == 1 && ev[0].type == GamepadEventType::Disconnected && !pads.connected(2) &&
               pads.buttonReleased(2, GamepadButton::East),
           "disconnect");
}

void testEvdevMapper() {
    using namespace evdev_codes;
    EvdevGamepadMapper m;
    m.setAbsRange(AbsX, -32768, 32767);
    m.setAbsRange(AbsY, -32768, 32767);
    m.setAbsRange(AbsRZ, 0, 1023);
    m.setAbsRange(AbsHat0X, -1, 1);
    m.setAbsRange(AbsHat0Y, -1, 1);
    std::vector<GamepadEvent> ev;
    m.translate(0, EvKey, BtnSouth, 1, ev);
    m.translate(0, EvKey, BtnSouth, 2, ev); // autorepeat ignored
    m.translate(0, EvKey, BtnMode, 1, ev);
    m.translate(0, EvAbs, AbsX, 32767, ev);
    m.translate(0, EvAbs, AbsY, -32768, ev); // up on evdev -> +1
    m.translate(0, EvAbs, AbsRZ, 1023, ev);
    m.translate(0, EvAbs, AbsZ, 100, ev); // undeclared axis ignored
    m.translate(0, EvKey, BtnTL2, 1, ev); // no analog left trigger declared -> digital
    m.translate(0, EvKey, BtnTR2, 1, ev); // analog RZ exists -> ignored
    m.translate(0, EvAbs, AbsHat0X, -1, ev);
    m.translate(0, EvAbs, AbsHat0X, 1, ev);
    m.translate(0, EvAbs, AbsHat0Y, 1, ev);
    GamepadState pads;
    pads.beginFrame();
    pads.applyAll(ev);
    expect(pads.buttonDown(0, GamepadButton::South) && pads.buttonDown(0, GamepadButton::Guide), "evdev buttons");
    expect(pads.rawAxis(0, GamepadAxis::LeftX) == 1.f && pads.rawAxis(0, GamepadAxis::LeftY) == 1.f,
           "evdev stick normalised with Y up");
    expect(pads.rawAxis(0, GamepadAxis::RightTrigger) == 1.f && pads.rawAxis(0, GamepadAxis::LeftTrigger) == 1.f,
           "analog RZ trigger and digital TL2 trigger");
    expect(!pads.buttonDown(0, GamepadButton::DPadLeft) && pads.buttonDown(0, GamepadButton::DPadRight) &&
               pads.buttonDown(0, GamepadButton::DPadDown),
           "hat -> d-pad with release of the opposite direction");
    expect(approx(m.normalise(AbsX, 0), 0.f, 1e-4f), "centre ~ 0");
}

#if defined(FUSE_PLATFORM_HAS_EVDEV_GAMEPAD)
void writeEvent(int fd, u16 type, u16 code, i32 value) {
    input_event e{};
    e.type = type;
    e.code = code;
    e.value = value;
    const ssize_t n = write(fd, &e, sizeof(e));
    (void)n;
}

void testEvdevStream() {
    using namespace evdev_codes;
    int fds[2] = {-1, -1};
    if (pipe(fds) != 0) {
        expect(false, "pipe");
        return;
    }
    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);
    EvdevGamepadBackend backend("", false); // no directory: only the adopted stream
    EvdevDeviceCaps caps;
    caps.name = "Synthetic Pad";
    caps.absAxes = {{AbsX, -128, 127}, {AbsY, -128, 127}, {AbsZ, 0, 255}};
    const int slot = backend.adoptStream(fds[0], caps);
    expect(slot == 0 && backend.deviceCount() == 1 && backend.deviceName(0) == "Synthetic Pad", "stream adopted");

    PlayerController player;
    player.setGamepadBackend(&backend);
    player.beginFrame();
    player.pollGamepads();
    expect(player.gamepads().connected(0) && player.gamepads().connectedThisFrame(0), "Connected reported on poll");

    writeEvent(fds[1], EvKey, BtnSouth, 1);
    writeEvent(fds[1], EvAbs, AbsX, 127);
    writeEvent(fds[1], EvSyn, SynReport, 0);
    player.tick(nullptr);
    expect(player.actions().pressed("Jump") && player.moveX() > 0.99f, "evdev stream drives actions");

    // SYN_DROPPED: everything up to the next SYN_REPORT is discarded.
    writeEvent(fds[1], EvSyn, SynDropped, 0);
    writeEvent(fds[1], EvKey, BtnEast, 1);
    writeEvent(fds[1], EvSyn, SynReport, 0);
    writeEvent(fds[1], EvKey, BtnNorth, 1);
    writeEvent(fds[1], EvSyn, SynReport, 0);
    player.tick(nullptr);
    expect(!player.gamepads().buttonDown(0, GamepadButton::East) && player.gamepads().buttonDown(0, GamepadButton::North),
           "events inside a dropped window are skipped");

    close(fds[1]); // unplug
    player.tick(nullptr);
    expect(!player.gamepads().connected(0) && player.gamepads().disconnectedThisFrame(0) && backend.deviceCount() == 0,
           "EOF -> Disconnected");
    expect(player.actions().released("Jump"), "Jump released on disconnect");

    // The real directory scan must not crash when /dev/input is missing or unreadable.
    EvdevGamepadBackend real;
    std::vector<GamepadEvent> ev;
    real.poll(ev);
    expect(real.deviceCount() <= kMaxGamepads, "real evdev scan runs");
}
#endif

void testRebindingPersistence() {
    const std::filesystem::path dir = test::makeUniqueTempDir("fuse_action_map");
    const std::filesystem::path file = dir / "config" / "input.json";

    ActionMap map = PlayerController::defaultActionMap();
    // Rebind Jump's keyboard slot to J, add a pad-specific binding, make an inverted axis.
    expect(map.rebind("Jump", 0, InputBinding::key(Key::J)), "rebind slot 0");
    expect(map.bind("Jump", InputBinding::gamepadButton(GamepadButton::RightShoulder, 1.f, 2)), "bind pad 2");
    expect(map.addAxis("Zoom") != ActionMap::kInvalidAction, "new axis");
    expect(map.bind("Zoom", InputBinding::gamepadAxis(GamepadAxis::RightY, -0.5f, kAnyGamepad, 0.25f)), "bind zoom");
    expect(!map.rebind("Jump", 9, InputBinding::key(Key::K)), "rebind out of range refused");
    expect(map.unbind("Interact", InputBinding::key(Key::E)), "unbind E");
    expect(map.saveFile(file), "saveFile");

    ActionMap loaded = PlayerController::defaultActionMap();
    std::string error;
    expect(loaded.loadFile(file, &error), "loadFile");
    expect(loaded.toJson() == map.toJson(), "save -> load -> save is identical");
    const std::vector<InputBinding>* jump = loaded.bindings("Jump");
    expect(jump != nullptr && jump->size() == 3 && (*jump)[0] == InputBinding::key(Key::J) && (*jump)[2].gamepad == 2,
           "rebinding persisted");
    const std::vector<InputBinding>* zoom = loaded.bindings("Zoom");
    expect(zoom != nullptr && zoom->size() == 1 && (*zoom)[0].scale == -0.5f && (*zoom)[0].threshold == 0.25f,
           "new axis persisted with scale / threshold");

    // The loaded map behaves like the saved one: J jumps, Space no longer does.
    InputState in;
    in.beginFrame();
    in.apply(keyEvent(true, ' '));
    loaded.update(in, nullptr);
    expect(!loaded.held("Jump"), "Space unbound after load");
    in.apply(keyEvent(true, 'J'));
    loaded.update(in, nullptr);
    expect(loaded.pressed("Jump"), "J jumps after load");

    // Errors are all-or-nothing.
    const std::string before = loaded.toJson();
    expect(!loaded.fromJson("{\"version\":1,\"actions\":[{\"name\":\"Jump\",\"bindings\":[{\"source\":\"Key:Nope\"}]}]}",
                            &error) &&
               error.find("Key:Nope") != std::string::npos,
           "unknown key rejected with its name");
    expect(!loaded.fromJson("{\"actions\":[{\"name\":\"MoveX\",\"kind\":\"button\"}]}", &error), "kind change rejected");
    expect(!loaded.fromJson("{\"actions\":[{\"name\":\"Jump\",\"bindings\":[{\"source\":\"Pad:South\",\"pad\":7}]}]}",
                            &error),
           "pad out of range rejected");
    expect(!loaded.fromJson("{\"version\":2,\"actions\":[]}", &error), "future version rejected");
    expect(!loaded.fromJson("{\"actions\": [", &error) && error.find("1:") != std::string::npos, "syntax error located");
    expect(loaded.toJson() == before, "failed loads change nothing");
    expect(!loaded.loadFile(dir / "missing.json", &error), "missing file reported");

    // Capture for a rebinding UI.
    InputState cap;
    GamepadState pads;
    cap.beginFrame();
    pads.beginFrame();
    InputBinding captured;
    expect(!ActionMap::captureBinding(cap, &pads, captured), "nothing to capture");
    pads.apply(GamepadEvent::axis(1, GamepadAxis::LeftY, -0.9f));
    expect(ActionMap::captureBinding(cap, &pads, captured) && captured.kind == InputSourceKind::GamepadAxis &&
               captured.code == static_cast<u16>(GamepadAxis::LeftY) && captured.scale == -1.f,
           "stick push captured with its sign");
    cap.apply(keyEvent(true, 'Q'));
    expect(ActionMap::captureBinding(cap, &pads, captured) && captured == InputBinding::key(Key::Q),
           "key press captured first");
    InputBinding parsed;
    expect(parseBindingSource(bindingSourceName(InputBinding::mouseAxis(MouseAxisCode::Y)), parsed) &&
               parsed.kind == InputSourceKind::MouseAxis && parsed.code == 1,
           "binding source names round-trip");
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

class ScriptedBackend final : public GamepadBackend {
public:
    const char* name() const override { return "scripted"; }
    u32 poll(std::vector<GamepadEvent>& out) override {
        const u32 n = static_cast<u32>(queued.size());
        out.insert(out.end(), queued.begin(), queued.end());
        queued.clear();
        return n;
    }
    std::vector<GamepadEvent> queued;
};

void testPlayerController() {
    config::CVarRegistry reg;
    config::CVar<f32> sensitivity("in.mouseSensitivity", 1.f, 0.01f, 20.f, "", config::CVarFlags::Archive, reg);
    ScriptedBackend backend;
    PlayerController player(1);
    player.setGamepadBackend(&backend);
    player.setMouseSensitivityCVar(&sensitivity);

    EventPump pump;
    PlatformEvent e = rawMouse(10, 0);
    pump.pushSyntheticEvent(e);
    backend.queued = {GamepadEvent::connected(0), GamepadEvent::button(0, GamepadButton::South, true),
                      GamepadEvent::connected(1), GamepadEvent::axis(1, GamepadAxis::LeftY, -1.f)};
    sensitivity.set(2.f);
    player.tick(&pump);
    expect(player.lookX() == 20.f, "look uses the sensitivity cvar");
    expect(!player.actions().held("Jump"), "player 1 ignores pad 0's Jump");
    expect(player.moveY() == -1.f, "player 1 reads pad 1's stick");

    player.setEnabled(false);
    player.tick(&pump);
    expect(player.moveY() == 0.f && !player.actions().held("Jump"), "disabled controller reports nothing");
    player.setEnabled(true);
    player.tick(&pump);
    expect(player.moveY() == -1.f, "re-enabled controller resumes (pad state kept)");
}

} // namespace

int main() {
    testKeyNamesAndMouseEdges();
    testDeadzones();
    testGamepadState();
    testActionsFromSyntheticEvents();
    testXInputDiff();
    testEvdevMapper();
#if defined(FUSE_PLATFORM_HAS_EVDEV_GAMEPAD)
    testEvdevStream();
#endif
    testRebindingPersistence();
    testPlayerController();
    std::unique_ptr<GamepadBackend> platform = createPlatformGamepadBackend();
    expect(platform != nullptr && platform->name() != nullptr, "platform backend factory");
    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_core_action_map: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("fuse_core_action_map: all checks passed\n");
    return 0;
}
