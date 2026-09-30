// MP-B7.3-LUA-API gates: a Lua script reads Input.pressed / axis over the ActionMap (synthetic key
// and gamepad events), plays audio through the Null backend, finds an entity by name, queries a
// sphere, switches the active camera, edits an SDF radius / alpha / primitive, and reads / sets
// cvars. Missing services and bad arguments raise Lua errors (caught by the VM).

#include <fuse/config/cvar.hpp>
#include <fuse/config/engine_cvars.hpp>
#include <fuse/core/temp_path.hpp>
#include <fuse/ecs/components/camera.hpp>
#include <fuse/ecs/components/sdf_object.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/registry.hpp>
#include <fuse/ecs/systems/culling_system.hpp>
#include <fuse/ecs/systems/transform_system.hpp>
#include <fuse/platform/action_map.hpp>
#include <fuse/platform/event_pump.hpp>
#include <fuse/script/script_engine_api.hpp>
#include <fuse/script/script_engine_backends.hpp>
#include <fuse/script/script_vm.hpp>
#include <fuse/spatial/bvh.hpp>

#if defined(FUSE_TEST_HAS_SCRIPT_AUDIO)
#include <fuse/audio/audio_engine.hpp>
#include <fuse/script/script_audio_bridge.hpp>
#endif

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace fuse;
using fuse::ecs::EntityID;
using fuse::script::ScriptEngineBindings;
using fuse::script::ScriptVM;

namespace {

int g_failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

platform::PlatformEvent keyEvent(bool down, u32 code) {
    platform::PlatformEvent e{};
    e.type = down ? platform::PlatformEventType::KeyDown : platform::PlatformEventType::KeyUp;
    e.keyCode = code;
    return e;
}

/// Runs `source` and returns its print output (empty + failure on a load / runtime error).
std::string run(ScriptVM& vm, const char* source, const char* label) {
    vm.clear_output();
    const auto result = vm.load_string(source, label);
    if (!result.ok()) {
        std::fprintf(stderr, "FAIL: %s: %s\n", label, vm.last_error().c_str());
        ++g_failures;
        return {};
    }
    return vm.output();
}

/// True when `source` raises, and the error text contains `needle`.
bool raises(ScriptVM& vm, const char* source, const char* needle) {
    const auto result = vm.load_string(source, "raises");
    return !result.ok() && vm.last_error().find(needle) != std::string::npos;
}

void testInput() {
    ScriptVM vm;
    if (!vm.init() || !vm.has_lua_backend()) {
        expect(false, "Lua VM");
        return;
    }
    platform::PlayerController player;
    script::InputScriptBackend input(player);
    ScriptEngineBindings bindings;
    bindings.input = &input;
    expect(script::bind_engine_api(vm, bindings), "bind Input");

    const char* frameScript = R"lua(
        print(tostring(Input.pressed("Jump")) .. " " .. tostring(Input.held("Jump")) .. " " ..
              string.format("%.2f", Input.axis("MoveX")) .. " " .. tostring(Input.key_pressed("Space")) .. " " ..
              tostring(Input.key_held("W")) .. " " .. tostring(Input.mouse_pressed("Left")) .. " " ..
              tostring(Input.gamepad_connected(0)))
        local dx, dy = Input.mouse_delta()
        print(dx .. " " .. dy)
    )lua";

    // Frame 1: Space + D down, mouse moved, left button pressed.
    platform::EventPump pump;
    pump.pushSyntheticEvent(keyEvent(true, ' '));
    pump.pushSyntheticEvent(keyEvent(true, 'D'));
    platform::PlatformEvent motion{};
    motion.type = platform::PlatformEventType::RawMouseDelta;
    motion.mouseX = 7;
    motion.mouseY = -3;
    pump.pushSyntheticEvent(motion);
    platform::PlatformEvent click{};
    click.type = platform::PlatformEventType::MouseButtonDown;
    click.mouseButton = 1;
    pump.pushSyntheticEvent(click);
    player.tick(&pump);
    std::string out = run(vm, frameScript, "input frame 1");
    expect(out == "true true 1.00 true false true false\n7 -3\n", "frame 1: Jump pressed, MoveX 1, key + mouse");
    if (out != "true true 1.00 true false true false\n7 -3\n") {
        std::fprintf(stderr, "  got: %s", out.c_str());
    }

    // Frame 2: Space still held (not pressed), A added -> MoveX 0; gamepad connects with the stick left.
    pump.pushSyntheticEvent(keyEvent(true, 'A'));
    player.beginFrame();
    player.pumpEvents(pump);
    player.apply(platform::GamepadEvent::connected(0));
    player.apply(platform::GamepadEvent::axis(0, platform::GamepadAxis::LeftX, -1.f));
    player.update();
    out = run(vm, frameScript, "input frame 2");
    // Keys: D - A = 0, stick -1 -> -1.
    expect(out == "false true -1.00 false false false true\n0 0\n", "frame 2: held, stick drives MoveX, pad connected");
    if (out != "false true -1.00 false false false true\n0 0\n") {
        std::fprintf(stderr, "  got: %s", out.c_str());
    }

    // A behaviour-style script: jump when the action fires.
    pump.pushSyntheticEvent(keyEvent(false, ' '));
    player.tick(&pump);
    out = run(vm, "print(tostring(Input.released('Jump')))", "released");
    expect(out == "true\n", "Input.released after key up");

    expect(raises(vm, "Input.pressed('NoSuchAction')", "unknown action"), "unknown action raises");
    expect(raises(vm, "Input.key_held('NotAKey')", "unknown key"), "unknown key raises");
    expect(raises(vm, "Input.mouse_held('Thumb')", "unknown mouse button"), "unknown mouse button raises");

    ScriptVM bare;
    bare.init();
    ScriptEngineBindings none;
    script::bind_engine_api(bare, none);
    expect(raises(bare, "Input.held('Jump')", "no input backend"), "Input without backend raises");
    expect(raises(bare, "Audio.play_2d(1)", "no audio backend"), "Audio without backend raises");
    expect(raises(bare, "Scene.find_entity('x')", "no scene backend"), "Scene without backend raises");
    expect(raises(bare, "SDF.set_radius(1, 2)", "no registry"), "SDF without registry raises");
    expect(raises(bare, "CVar.get('r.vsync')", "no cvar registry"), "CVar without registry raises");
}

EntityID spawnSdf(ecs::Registry& reg, f32 x, f32 radius) {
    const EntityID id = reg.create();
    ecs::Transform t{};
    t.position = {x, 0.f, 0.f, 1.f};
    reg.add<ecs::Transform>(id, t);
    ecs::SDFObject sdf{};
    sdf.params = {radius, 0.f, 0.f, 0.f};
    reg.add<ecs::SDFObject>(id, sdf);
    return id;
}

void testSceneAndSdf() {
    ecs::Registry reg;
    reg.init(256);
    ScriptVM vm;
    if (!vm.init() || !vm.has_lua_backend()) {
        expect(false, "Lua VM");
        return;
    }
    script::RegistrySceneBackend scene(reg);
    ScriptEngineBindings bindings;
    bindings.registry = &reg;
    bindings.scene = &scene;
    expect(script::bind_engine_api(vm, bindings), "bind Scene / SDF");

    const EntityID ball = spawnSdf(reg, 0.f, 1.f);
    const EntityID distant = spawnSdf(reg, 50.f, 1.f);
    (void)distant;
    expect(scene.set_name(ball, "Ball"), "name the ball");
    ecs::Transform camPose{};
    camPose.position = {0.f, 100.f, 0.f, 1.f}; // out of the sphere queries below
    const EntityID camA = reg.create();
    reg.add<ecs::Transform>(camA, camPose);
    ecs::Camera camera{};
    camera.is_active = true;
    reg.add<ecs::Camera>(camA, camera);
    const EntityID camB = reg.create();
    reg.add<ecs::Transform>(camB, camPose);
    reg.add<ecs::Camera>(camB);
    EntityID notified = EntityID::null();
    scene.set_camera_changed_callback([&](EntityID id) { notified = id; });

    std::string out = run(vm, R"lua(
        local ball = Scene.find_entity("Ball")
        print(ball ~= nil)
        print(Scene.find_entity("Nobody") == nil)
        -- SDF edits
        print(SDF.set_radius(ball, 2.5))
        print(SDF.set_alpha(ball, 1.5))
        print(SDF.set_primitive(ball, "box"))
        local s = SDF.get(ball)
        print(s.primitive .. " " .. s.radius .. " " .. s.alpha .. " " .. s.params.y)
        -- Entity.create(name) registers the name with the scene
        local marker = Entity.create("Marker")
        Entity.set_position(marker, {x = 3, y = 0, z = 0})
        print(Scene.find_entity("Marker") == marker)
        -- sphere query: the ball (box half-extent 2.5 at the origin) and the marker at x=3
        local hits = Scene.query_sphere({x = 4, y = 0, z = 0}, 1.6)
        print(#hits)
        local near_ball = Scene.query_sphere({x = 0, y = 0, z = 0}, 0.1)
        print(#near_ball == 1 and near_ball[1] == ball)
        print(SDF.set_radius(marker, 1) == false)
        print(SDF.get(marker) == nil)
    )lua", "scene script");
    expect(out == "true\ntrue\ntrue\ntrue\ntrue\nBox 2.5 1 2.5\ntrue\n2\ntrue\ntrue\ntrue\n", "scene / SDF script output");
    if (out != "true\ntrue\ntrue\ntrue\ntrue\nBox 2.5 1 2.5\ntrue\n2\ntrue\ntrue\ntrue\n") {
        std::fprintf(stderr, "  got: %s", out.c_str());
    }
    const ecs::SDFObject* sdf = reg.get<ecs::SDFObject>(ball);
    expect(sdf != nullptr && sdf->type == ecs::SDFPrimitive::Box && sdf->params.x == 2.5f && sdf->params.z == 2.5f &&
               sdf->blend_alpha == 1.f,
           "SDF component edited from Lua");

    // Active camera.
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "print(Scene.active_camera() == %.0f) print(Scene.set_active_camera(%.0f)) "
                  "print(Scene.active_camera() == %.0f) print(Scene.set_active_camera(%.0f))",
                  script::encode_entity_id(camA), script::encode_entity_id(camB), script::encode_entity_id(camB),
                  script::encode_entity_id(ball));
    out = run(vm, buf, "camera script");
    expect(out == "true\ntrue\ntrue\nfalse\n", "set_active_camera switches and rejects non-cameras");
    expect(!reg.get<ecs::Camera>(camA)->is_active && reg.get<ecs::Camera>(camB)->is_active && notified == camB,
           "exactly one camera active; host notified");

    // Destroyed names are not handed out.
    reg.destroy_entity(ball);
    out = run(vm, "print(Scene.find_entity('Ball') == nil)", "stale name");
    expect(out == "true\n", "destroyed entity's name resolves to nil");
    expect(raises(vm, "SDF.set_primitive(1, 'Blob')", "unknown primitive"), "unknown primitive raises");
    expect(raises(vm, "SDF.set_radius(1, -1)", "radius"), "negative radius raises");

    // BVH path gives the same answer as the scan.
    const EntityID a = spawnSdf(reg, 10.f, 1.f);
    const EntityID b = spawnSdf(reg, 13.f, 1.f);
    std::vector<spatial::BVHLeaf> leaves;
    reg.each<ecs::SDFObject, ecs::Transform>([&](EntityID id, ecs::SDFObject& s, ecs::Transform& t) {
        ecs::Transform w = t;
        ecs::TransformSystem::recompute_world_matrix(w, ecs::mat4::identity());
        spatial::BVHLeaf leaf{};
        leaf.type = spatial::BVHLeafType::SDF;
        leaf.entity = id;
        leaf.aabb = ecs::CullingSystem::world_bounds(w, s);
        leaves.push_back(leaf);
    });
    spatial::BVH bvh;
    bvh.build(leaves);
    std::vector<EntityID> scanHits;
    std::vector<EntityID> bvhHits;
    scene.query_sphere({11.5f, 0.f, 0.f, 0.f}, 1.f, scanHits);
    scene.set_bvh(&bvh);
    scene.query_sphere({11.5f, 0.f, 0.f, 0.f}, 1.f, bvhHits);
    expect(scanHits.size() == 2 && scanHits == bvhHits && scanHits[0] == a && scanHits[1] == b,
           "BVH query matches the scan");
}

void testCVars() {
    ScriptVM vm;
    if (!vm.init() || !vm.has_lua_backend()) {
        expect(false, "Lua VM");
        return;
    }
    config::CVarRegistry reg;
    const config::EngineCVars cvars = config::register_engine_cvars(reg);
    ScriptEngineBindings bindings;
    bindings.cvars = &reg;
    script::bind_engine_api(vm, bindings);
    const std::string out = run(vm, R"lua(
        print(CVar.get("r.vsync"), CVar.get("r.renderScale"), CVar.get("r.tier"), CVar.get("nope"))
        print(CVar.set("r.renderScale", 0.5))
        print(CVar.set("r.renderScale", 9))
        print(CVar.set("r.vsync", false))
        print(CVar.set("r.upscaler", "fsr1"))
    )lua", "cvar script");
    expect(out == "true\t1\tauto\tnil\ntrue\nfalse\tvalue out of range\ntrue\ntrue\n", "CVar.get / CVar.set");
    if (out != "true\t1\tauto\tnil\ntrue\nfalse\tvalue out of range\ntrue\ntrue\n") {
        std::fprintf(stderr, "  got: %s", out.c_str());
    }
    expect(cvars.renderScale.get() == 0.5f && !cvars.vsync.get() && cvars.upscaler.get() == "fsr1" &&
               cvars.renderScale.entry().source() == config::CVarSource::Runtime,
           "Lua sets land in the registry at runtime precedence");
}

#if defined(FUSE_TEST_HAS_SCRIPT_AUDIO)
std::string writeWav(const std::filesystem::path& path, u32 frames) {
    const u32 rate = 48000;
    const u32 dataBytes = frames * 2;
    std::ofstream out(path, std::ios::binary);
    const auto u32le = [&](u32 v) {
        const unsigned char b[4] = {static_cast<unsigned char>(v), static_cast<unsigned char>(v >> 8),
                                    static_cast<unsigned char>(v >> 16), static_cast<unsigned char>(v >> 24)};
        out.write(reinterpret_cast<const char*>(b), 4);
    };
    const auto u16le = [&](u16 v) {
        const unsigned char b[2] = {static_cast<unsigned char>(v), static_cast<unsigned char>(v >> 8)};
        out.write(reinterpret_cast<const char*>(b), 2);
    };
    out.write("RIFF", 4);
    u32le(36 + dataBytes);
    out.write("WAVEfmt ", 8);
    u32le(16);
    u16le(1);        // PCM
    u16le(1);        // mono
    u32le(rate);
    u32le(rate * 2); // byte rate
    u16le(2);        // block align
    u16le(16);       // bits
    out.write("data", 4);
    u32le(dataBytes);
    for (u32 i = 0; i < frames; ++i) {
        const f32 s = 0.5f * std::sin(2.f * 3.14159265f * static_cast<f32>(i) / 48.f);
        u16le(static_cast<u16>(static_cast<i32>(s * 32767.f)));
    }
    return path.string();
}

void testAudio() {
    ScriptVM vm;
    if (!vm.init() || !vm.has_lua_backend()) {
        expect(false, "Lua VM");
        return;
    }
    const std::filesystem::path dir = test::makeUniqueTempDir("fuse_script_audio");
    const std::string wav = writeWav(dir / "blip.wav", 4800);

    audio::AudioEngine engine;
    audio::AudioDesc desc;
    desc.output = audio::AudioOutputRequest::Null;
    desc.frames_per_buf = 256;
    desc.capture_frames = 4096;
    desc.cuda_reverb = false;
    engine.init(desc);
    expect(engine.is_initialized() && engine.backend_kind() == audio::AudioBackendKind::Null, "Null audio backend");
    audio::AudioRegistry audioRegistry;
    const audio::EntityId listenerId = audioRegistry.create_entity();
    audio::AudioListener* listener = audioRegistry.set_listener(listenerId);
    listener->forward = {0.f, 0.f, -1.f};
    listener->up = {0.f, 1.f, 0.f};

    script::AudioEngineScriptBackend backend(engine);
    ScriptEngineBindings bindings;
    bindings.audio = &backend;
    script::bind_engine_api(vm, bindings);

    std::string source = "clip = Audio.load('" + wav + "')\n" + R"lua(
        print(clip ~= nil)
        print(Audio.play_2d(clip, 0.8))
        print(Audio.play_at(clip, {x = 1, y = 0, z = -2}, 1.0, 1.0))
        print(Audio.load("does/not/exist.wav") == nil)
        print(Audio.play_2d(12345))
    )lua";
    std::string out = run(vm, source.c_str(), "audio script");
    expect(out == "true\ntrue\ntrue\ntrue\nfalse\n", "Audio.load / play_2d / play_at");
    if (out != "true\ntrue\ntrue\ntrue\nfalse\n") {
        std::fprintf(stderr, "  got: %s", out.c_str());
    }
    expect(engine.spatial_mixer().one_shots().size() == 2, "two one-shot voices queued");
    engine.clear_output_capture();
    engine.update(audioRegistry, 256.f / 48000.f);
    f32 peak = 0.f;
    for (const f32 s : engine.output_capture()) {
        peak = std::fabs(s) > peak ? std::fabs(s) : peak;
    }
    expect(engine.output_capture().size() >= 512 && peak > 0.05f, "the Null backend captured audible output");
    expect(raises(vm, "Audio.play_2d(-1)", "clip handle"), "bad clip handle raises");
    engine.destroy();
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}
#endif

} // namespace

int main() {
    {
        ScriptVM probe;
        if (!probe.init() || !probe.has_lua_backend()) {
            std::printf("SKIP: fuse_script built without a Lua backend\n");
            return 77;
        }
    }
    testInput();
    testSceneAndSdf();
    testCVars();
#if defined(FUSE_TEST_HAS_SCRIPT_AUDIO)
    testAudio();
#else
    std::printf("note: fuse_script_audio not built; Audio.* checked only for the missing-backend error\n");
#endif
    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_script_lua_api: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("fuse_script_lua_api: all checks passed\n");
    return 0;
}
