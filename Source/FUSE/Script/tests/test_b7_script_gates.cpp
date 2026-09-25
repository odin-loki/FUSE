// B7.3 Scripting Layer gate rows (master plan B7.3 + B7.10 "Scripting" carry-forward):
//  - Lua state initialises and executes a hello-world script without error
//  - Entity.get_position / set_position round-trip correctly through Lua
//  - Physics.ray_cast returns correct hit from Lua — verified against C++ ray_cast result
//  - on_update called every frame with correct dt — verified by accumulating dt over 60 frames
//  - on_collision fires on first frame of contact — verified with controlled rigid body test
//  - Hot-reload replaces script function mid-run — new behaviour active within 1 frame
//  - B7.3 implemented on FUSE APIs: Entity/Physics tables reach the ECS registry and the FUSE
//    PhysicsManager only through fuse_script's public binding layer
// Plus the host-safety rows: script errors are isolated (engine keeps running), sandboxing,
// instruction budget for runaway scripts, memory cap + bounded GC, coroutine semantics.
#include <fuse/core/init.hpp>
#include <fuse/ecs/components/collider.hpp>
#include <fuse/ecs/components/rigidbody.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/registry.hpp>
#include <fuse/physics/physics_manager.hpp>
#include <fuse/script/script_engine_api.hpp>
#include <fuse/script/script_host.hpp>
#include <fuse/script/script_hot_reload.hpp>
#include <fuse/script/script_physics_bridge.hpp>
#include <fuse/script/script_runtime.hpp>
#include <fuse/script/script_vm.hpp>
#include <fuse/types.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <type_traits>
#include <vector>

namespace {

int g_failures = 0;

using fuse::f32;
using fuse::f64;
using fuse::u32;
using fuse::u64;
using fuse::usize;
using fuse::ecs::Collider;
using fuse::ecs::EntityID;
using fuse::ecs::Registry;
using fuse::ecs::Transform;
using fuse::script::ScriptEngineBindings;
using fuse::script::ScriptHotReload;
using fuse::script::ScriptLoadStatus;
using fuse::script::ScriptRuntime;
using fuse::script::ScriptVM;
using fuse::script::ScriptVMDesc;
namespace bind = fuse::script::bind;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

constexpr f32 kDt = 1.f / 60.f;

// No owning raw pointers in the public API: the VM owns its Lua state through unique_ptr and is
// move-only; the runtime holds non-owning references and is pinned.
static_assert(!std::is_copy_constructible_v<ScriptVM>, "ScriptVM owns a Lua state: not copyable");
static_assert(std::is_nothrow_move_constructible_v<ScriptVM>, "ScriptVM moves its Lua state");
static_assert(!std::is_copy_constructible_v<ScriptRuntime>, "ScriptRuntime holds Lua refs");

f64 numberField(ScriptRuntime& runtime, EntityID entity, const char* field) {
    bind::ScriptValue value;
    if (!runtime.get_instance_field(entity, field, value) || !bind::is_number(value)) {
        return -1.0e30;
    }
    return bind::to_number(value);
}

bool initLuaVm(ScriptVM& vm, const ScriptVMDesc& desc = {}) {
    if (!vm.init(desc)) {
        return false;
    }
    return vm.has_lua_backend();
}

EntityID spawnBody(Registry& reg, f32 x, f32 y, f32 z, u32 shape, f32 radius, bool isStatic) {
    const EntityID id = reg.create();
    Transform t{};
    t.position = {x, y, z, 1.f};
    reg.add(id, t);
    fuse::ecs::RigidBody rb{};
    rb.is_static = isStatic;
    rb.restitution = 0.f;
    reg.add(id, rb);
    Collider c{};
    c.shape = shape;
    c.params = shape == Collider::Plane ? fuse::ecs::vec3{0.f, 1.f, 0.f, 0.f}
                                        : fuse::ecs::vec3{radius, 0.f, 0.f, 0.f};
    reg.add(id, c);
    return id;
}

// ---------------------------------------------------------------------------------------------
// Row: Lua state initialises and executes a hello-world script without error.
void testHelloWorld() {
    ScriptVM vm;
    expectTrue(initLuaVm(vm), "Lua VM initialises with a real Lua backend");
    expectTrue(vm.backend_kind() == fuse::script::ScriptBackendKind::Lua, "backend kind is Lua");

    const auto hello = vm.load_string("print('hello world')", "hello");
    expectTrue(hello.ok(), "hello-world chunk executes without error");
    expectTrue(vm.output() == "hello world\n", "print output captured verbatim");
    expectTrue(vm.error_count() == 0, "no script errors after hello world");

    // The VM really evaluates Lua: compare script arithmetic with the C++ result.
    expectTrue(vm.load_string("function mix(a, b) return a * 3 + b / 4 end", "mix").ok(),
               "function definition chunk loads");
    const bind::ScriptValue args[] = {bind::push_number(7.25), bind::push_number(-10.0)};
    bind::ScriptValue out;
    const auto called = vm.call_global("mix", args, 2, &out);
    expectTrue(called.ok() && bind::is_number(out) && bind::to_number(out) == 7.25 * 3 + (-10.0) / 4,
               "Lua arithmetic matches the C++ reference");

    const auto syntax = vm.load_string("function broken(", "broken");
    expectTrue(syntax.status == ScriptLoadStatus::ParseError, "syntax error reports ParseError");
    const auto runtime = vm.load_string("local t = nil; return t.field", "nilindex");
    expectTrue(runtime.status == ScriptLoadStatus::RuntimeError, "runtime error reports RuntimeError");
    expectTrue(runtime.message != nullptr && std::strstr(runtime.message, "stack traceback") != nullptr,
               "runtime error carries a traceback");
    expectTrue(vm.load_string("print('still alive')", "after").ok() &&
                   vm.output().find("still alive") != std::string::npos,
               "VM stays usable after errors");

    // ScriptHost facade runs the same backend.
    fuse::script::ScriptHost host;
    expectTrue(host.init() && host.vm().has_lua_backend(), "ScriptHost boots the Lua backend");
    expectTrue(host.load_string("print('hello world')", "hello").ok() && host.vm().output() == "hello world\n",
               "ScriptHost executes hello world");
    expectTrue(host.load_string("bb.sequence children=0\n", "uaisk:tree.cs").ok(),
               "UAISK data chunks are recorded, not executed as Lua");
    host.shutdown();
}

// ---------------------------------------------------------------------------------------------
// Row: Entity.get_position / set_position round-trip correctly through Lua.
void testEntityPositionRoundTrip() {
    Registry reg;
    reg.init(64);
    ScriptVM vm;
    expectTrue(initLuaVm(vm), "VM for Entity API");
    ScriptEngineBindings bindings;
    bindings.registry = &reg;
    expectTrue(fuse::script::bind_engine_api(vm, bindings), "Entity API binds on the VM");

    expectTrue(vm.load_string(R"lua(
        function make() return Entity.create("probe") end
        function set(id, x, y, z) return Entity.set_position(id, {x = x, y = y, z = z}) end
        function roundtrip(id, x, y, z)
            if not Entity.set_position(id, {x = x, y = y, z = z}) then return false end
            local p = Entity.get_position(id)
            return p ~= nil and p.x == x and p.y == y and p.z == z
        end
        function get_axis(id, axis)
            local p = Entity.get_position(id)
            if p == nil then return "nil" end
            return p[axis]
        end
        function alive(id) return Entity.alive(id) end
        function destroy(id) return Entity.destroy(id) end
        function euler(id, x, y, z) return Entity.set_rotation_euler(id, {x = x, y = y, z = z}) end
    )lua", "entity_api").ok(), "entity API script loads");

    bind::ScriptValue created;
    expectTrue(vm.call_global("make", nullptr, 0, &created).ok() && bind::is_number(created),
               "Entity.create returns an id");
    const EntityID id = fuse::script::decode_entity_id(bind::to_number(created));
    expectTrue(reg.alive(id) && reg.has<Transform>(id), "script-created entity lives in the registry");

    // Lua -> C++: values written by script land bit-exactly in the Transform component, and
    // read back through get_position unchanged. Inputs are f32 values (the component type).
    std::mt19937 rng(0xB73u);
    std::uniform_real_distribution<f32> dist(-10000.f, 10000.f);
    int exact = 0;
    constexpr int kSamples = 256;
    for (int i = 0; i < kSamples; ++i) {
        const f32 x = dist(rng), y = dist(rng), z = dist(rng);
        const bind::ScriptValue args[] = {created, bind::push_number(x), bind::push_number(y), bind::push_number(z)};
        bind::ScriptValue ok;
        const bool called = vm.call_global("roundtrip", args, 4, &ok).ok();
        const Transform* t = reg.get<Transform>(id);
        if (called && bind::to_bool(ok) && t->position.x == x && t->position.y == y && t->position.z == z &&
            t->dirty) {
            ++exact;
        }
    }
    std::printf("entity position round-trip: %d/%d bit-exact\n", exact, kSamples);
    expectTrue(exact == kSamples, "set_position -> Transform -> get_position is bit-exact");

    // C++ -> Lua: engine-side writes are what the script reads.
    Transform* t = reg.get<Transform>(id);
    t->position = {1.5f, -2.25f, 1.0e-3f, 1.f};
    const char* axes[] = {"x", "y", "z"};
    const f32 expected[] = {1.5f, -2.25f, 1.0e-3f};
    for (int axis = 0; axis < 3; ++axis) {
        const bind::ScriptValue args[] = {created, bind::push_string(axes[axis])};
        bind::ScriptValue value;
        expectTrue(vm.call_global("get_axis", args, 2, &value).ok() && bind::is_number(value) &&
                       bind::to_number(value) == static_cast<f64>(expected[axis]),
                   "get_position reads the engine-side Transform");
    }

    // Double inputs are stored at component (f32) precision.
    {
        const bind::ScriptValue args[] = {created, bind::push_number(0.1), bind::push_number(1.0 / 3.0),
                                          bind::push_number(1.0e10)};
        vm.call_global("set", args, 4, nullptr);
        const Transform* after = reg.get<Transform>(id);
        expectTrue(after->position.x == 0.1f && after->position.y == static_cast<f32>(1.0 / 3.0) &&
                       after->position.z == 1.0e10f,
                   "double inputs round to the f32 component value");
    }

    // set_rotation_euler vs an independent rotation-matrix reference (R = Ry * Rx * Rz).
    std::uniform_real_distribution<f32> angle(-180.f, 180.f);
    f64 worst = 0.0;
    for (int i = 0; i < 64; ++i) {
        const f32 ex = angle(rng), ey = angle(rng), ez = angle(rng);
        const bind::ScriptValue args[] = {created, bind::push_number(ex), bind::push_number(ey), bind::push_number(ez)};
        vm.call_global("euler", args, 4, nullptr);
        const fuse::ecs::quat q = reg.get<Transform>(id)->rotation;
        const f64 d2r = 3.14159265358979323846 / 180.0;
        const f64 cx = std::cos(ex * d2r), sx = std::sin(ex * d2r);
        const f64 cy = std::cos(ey * d2r), sy = std::sin(ey * d2r);
        const f64 cz = std::cos(ez * d2r), sz = std::sin(ez * d2r);
        const f64 Rx[3][3] = {{1, 0, 0}, {0, cx, -sx}, {0, sx, cx}};
        const f64 Ry[3][3] = {{cy, 0, sy}, {0, 1, 0}, {-sy, 0, cy}};
        const f64 Rz[3][3] = {{cz, -sz, 0}, {sz, cz, 0}, {0, 0, 1}};
        f64 yx[3][3] = {};
        f64 R[3][3] = {};
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                for (int k = 0; k < 3; ++k) yx[r][c] += Ry[r][k] * Rx[k][c];
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                for (int k = 0; k < 3; ++k) R[r][c] += yx[r][k] * Rz[k][c];
        const f64 w = q.w, x = q.x, y = q.y, z = q.z;
        const f64 Q[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
                             {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
                             {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)}};
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) worst = std::fmax(worst, std::fabs(Q[r][c] - R[r][c]));
    }
    std::printf("set_rotation_euler vs matrix reference: max |dR| = %.3g\n", worst);
    expectTrue(worst < 1e-5, "set_rotation_euler matches the Ry*Rx*Rz reference");

    // Stale handles: destroyed entities read nil and refuse writes; stale generations too.
    {
        const bind::ScriptValue args[] = {created};
        bind::ScriptValue destroyed;
        expectTrue(vm.call_global("destroy", args, 1, &destroyed).ok() && bind::to_bool(destroyed),
                   "Entity.destroy removes the entity");
        expectTrue(!reg.alive(id), "registry sees the script destroy");
        bind::ScriptValue alive;
        vm.call_global("alive", args, 1, &alive);
        expectTrue(bind::is_bool(alive) && !bind::to_bool(alive), "Entity.alive is false after destroy");
        const bind::ScriptValue getArgs[] = {created, bind::push_string("x")};
        bind::ScriptValue value;
        vm.call_global("get_axis", getArgs, 2, &value);
        expectTrue(bind::is_string(value) && bind::to_string(value) == "nil", "get_position of a dead id is nil");
        const bind::ScriptValue setArgs[] = {created, bind::push_number(1), bind::push_number(2), bind::push_number(3)};
        bind::ScriptValue ok;
        vm.call_global("set", setArgs, 4, &ok);
        expectTrue(bind::is_bool(ok) && !bind::to_bool(ok), "set_position on a dead id returns false");
        const EntityID reused = reg.create();
        reg.add<Transform>(reused);
        vm.call_global("alive", args, 1, &alive);
        expectTrue(reused.index == id.index ? !bind::to_bool(alive) : true,
                   "stale generation is not alive after slot reuse");
    }

    // Malformed arguments raise inside the protected call instead of crashing.
    expectTrue(vm.load_string("Entity.set_position('not an id', 5)", "bad_args").status == ScriptLoadStatus::RuntimeError,
               "bad Entity arguments raise a script error");
    expectTrue(vm.load_string("print(Entity.get_position(0))", "zero_id").ok(), "id 0 (null) reads nil");
}

// ---------------------------------------------------------------------------------------------
// Row: Physics.ray_cast returns correct hit from Lua — verified against C++ ray_cast result.
void testPhysicsRayCast() {
    Registry reg;
    reg.init(128);
    std::mt19937 rng(0xCA57u);
    std::uniform_real_distribution<f32> place(-8.f, 8.f);
    std::uniform_real_distribution<f32> radius(0.3f, 1.2f);
    struct Sphere {
        EntityID id;
        f64 x, y, z, r;
    };
    std::vector<Sphere> spheres;
    for (int i = 0; i < 24; ++i) {
        const f32 x = place(rng), y = place(rng) + 10.f, z = place(rng), r = radius(rng);
        spheres.push_back({spawnBody(reg, x, y, z, Collider::Sphere, r, true), x, y, z, r});
    }

    fuse::physics::PhysicsManager manager;
    manager.init({});
    fuse::physics::PhysicsStreamManager streams{};
    manager.step(reg, kDt, streams); // registers the (static) bodies

    fuse::script::PhysicsManagerScriptBackend backend(manager, reg);
    ScriptVM vm;
    expectTrue(initLuaVm(vm), "VM for Physics API");
    ScriptEngineBindings bindings;
    bindings.registry = &reg;
    bindings.physics = &backend;
    expectTrue(fuse::script::bind_engine_api(vm, bindings), "Physics API binds");
    expectTrue(vm.load_string(R"lua(
        function cast(ox, oy, oz, dx, dy, dz, maxd)
            local hit = Physics.ray_cast({x = ox, y = oy, z = oz}, {x = dx, y = dy, z = dz}, maxd)
            if hit == nil then return "miss" end
            return string.format("%.17g %.9g %.9g %.9g %.9g %.9g %.9g %.9g", hit.entity, hit.distance,
                hit.point.x, hit.point.y, hit.point.z, hit.normal.x, hit.normal.y, hit.normal.z)
        end
    )lua", "ray").ok(), "ray script loads");

    std::uniform_real_distribution<f32> unit(-1.f, 1.f);
    int rays = 0, hits = 0, matchCpp = 0, matchAnalytic = 0;
    f64 worstCpp = 0.0, worstAnalytic = 0.0;
    for (int i = 0; i < 400; ++i) {
        // Aim near a random sphere so roughly half the rays hit.
        const Sphere& target = spheres[static_cast<usize>(i) % spheres.size()];
        const f32 ox = place(rng) * 2.f, oy = 30.f + place(rng), oz = place(rng) * 2.f;
        f32 dx = static_cast<f32>(target.x) + unit(rng) * 1.5f - ox;
        f32 dy = static_cast<f32>(target.y) + unit(rng) * 1.5f - oy;
        f32 dz = static_cast<f32>(target.z) + unit(rng) * 1.5f - oz;
        const f32 maxd = 200.f;
        ++rays;

        const bind::ScriptValue args[] = {bind::push_number(ox), bind::push_number(oy), bind::push_number(oz),
                                          bind::push_number(dx), bind::push_number(dy), bind::push_number(dz),
                                          bind::push_number(maxd)};
        bind::ScriptValue result;
        if (!vm.call_global("cast", args, 7, &result).ok() || !bind::is_string(result)) {
            continue;
        }

        // C++ reference: PhysicsManager::rayCast directly.
        EntityID cppHit;
        fuse::physics::vec3 cppNormal{};
        f32 cppT = 0.f;
        const bool cppFound = manager.rayCast({ox, oy, oz}, {dx, dy, dz}, maxd, cppHit, cppNormal, cppT);

        // Independent analytic reference: nearest ray/sphere root in double precision.
        const f64 len = std::sqrt(f64(dx) * dx + f64(dy) * dy + f64(dz) * dz);
        const f64 ux = dx / len, uy = dy / len, uz = dz / len;
        f64 bestT = maxd;
        EntityID bestId;
        for (const Sphere& s : spheres) {
            const f64 lx = ox - s.x, ly = oy - s.y, lz = oz - s.z;
            const f64 b = lx * ux + ly * uy + lz * uz;
            const f64 c = lx * lx + ly * ly + lz * lz - s.r * s.r;
            const f64 disc = b * b - c;
            if (disc < 0.0) continue;
            const f64 t = -b - std::sqrt(disc);
            if (t >= 0.0 && t < bestT) {
                bestT = t;
                bestId = s.id;
            }
        }
        const bool analyticFound = bestId.valid();

        const std::string& text = bind::to_string(result);
        if (text == "miss") {
            matchCpp += cppFound ? 0 : 1;
            matchAnalytic += analyticFound ? 0 : 1;
            continue;
        }
        ++hits;
        f64 packed = 0;
        f64 dist = 0, px = 0, py = 0, pz = 0, nx = 0, ny = 0, nz = 0;
        std::sscanf(text.c_str(), "%lf %lf %lf %lf %lf %lf %lf %lf", &packed, &dist, &px, &py, &pz, &nx, &ny, &nz);
        const EntityID luaHit = fuse::script::decode_entity_id(packed);
        if (cppFound && luaHit == cppHit && static_cast<f32>(dist) == cppT &&
            std::fabs(nx - cppNormal.x) < 1e-6 && std::fabs(ny - cppNormal.y) < 1e-6 &&
            std::fabs(nz - cppNormal.z) < 1e-6) {
            ++matchCpp;
        }
        worstCpp = std::fmax(worstCpp, std::fabs(dist - cppT));
        if (analyticFound && luaHit == bestId) {
            const f64 ex = ox + ux * bestT, ey = oy + uy * bestT, ez = oz + uz * bestT;
            const f64 err = std::fmax(std::fabs(dist - bestT),
                                      std::fmax(std::fabs(px - ex), std::fmax(std::fabs(py - ey), std::fabs(pz - ez))));
            worstAnalytic = std::fmax(worstAnalytic, err);
            if (err < 2e-3) {
                ++matchAnalytic;
            }
        }
    }
    std::printf("Physics.ray_cast from Lua: %d rays, %d hits; matches C++ %d/%d (max |dt| %.3g), "
                "analytic %d/%d (max err %.3g)\n",
                rays, hits, matchCpp, rays, worstCpp, matchAnalytic, rays, worstAnalytic);
    expectTrue(hits > rays / 4 && hits < rays, "ray set mixes hits and misses");
    expectTrue(matchCpp == rays, "every Lua ray_cast equals the C++ PhysicsManager::rayCast result");
    expectTrue(matchAnalytic == rays, "every Lua ray_cast matches the analytic ray/sphere reference");

    // Velocity round-trip through the Physics table on a dynamic body.
    const EntityID ball = spawnBody(reg, 50.f, 5.f, 50.f, Collider::Sphere, 0.5f, false);
    manager.step(reg, kDt, streams);
    const std::string script = "local id = " + std::to_string(fuse::script::encode_entity_id(ball)) +
                               "\nPhysics.set_velocity(id, {x = 3, y = 0, z = -1})\n"
                               "Physics.apply_impulse(id, {x = 1, y = 2, z = 0})\n"
                               "local v = Physics.get_velocity(id)\n"
                               "vx, vy, vz = v.x, v.y, v.z\n";
    expectTrue(vm.load_string(script.c_str(), "velocity").ok(), "velocity script runs");
    const u32 body = manager.bodyIndex(ball);
    const fuse::physics::vec3 v = manager.bodies().linearVelocities[body];
    expectTrue(v.x == 4.f && v.y == 2.f && v.z == -1.f, "set_velocity + unit-mass impulse reach the solver");

    // Physics without a backend is a script error, not a crash.
    ScriptVM bare;
    initLuaVm(bare);
    ScriptEngineBindings noPhysics;
    noPhysics.registry = &reg;
    fuse::script::bind_engine_api(bare, noPhysics);
    const auto unbound = bare.load_string("Physics.ray_cast({x=0,y=0,z=0},{x=0,y=1,z=0},1)", "unbound");
    expectTrue(unbound.status == ScriptLoadStatus::RuntimeError && unbound.message != nullptr &&
                   std::strstr(unbound.message, "no physics backend") != nullptr,
               "unbound Physics API raises a script error");
}

// ---------------------------------------------------------------------------------------------
// Row: on_update called every frame with correct dt — accumulate dt over 60 frames.
void testOnUpdateDt() {
    Registry reg;
    reg.init(16);
    ScriptVM vm;
    initLuaVm(vm);
    ScriptRuntime runtime;
    ScriptEngineBindings bindings;
    bindings.registry = &reg;
    expectTrue(runtime.init(vm, bindings), "runtime initialises on the Lua VM");
    expectTrue(runtime.load_module_source("ticker", R"lua(
        function on_start(self)
            self.started = (self.started or 0) + 1
            self.frames_at_start = self.frames or 0
        end
        function on_update(self, dt)
            self.frames = (self.frames or 0) + 1
            self.acc = (self.acc or 0) + dt
            self.last_dt = dt
        end
    )lua").ok(), "ticker module loads");

    const EntityID a = reg.create();
    const EntityID b = reg.create();
    expectTrue(runtime.attach(a, "ticker") && runtime.attach(b, "ticker"), "two instances attach");
    expectTrue(!runtime.attach(a, "ticker"), "double attach refused");
    expectTrue(!runtime.attach(reg.create(), "missing"), "unknown module refused");

    std::mt19937 rng(60u);
    std::uniform_real_distribution<f32> jitter(0.5f, 1.5f);
    f64 reference = 0.0;
    int perFrameExact = 0;
    for (int frame = 0; frame < 60; ++frame) {
        const f32 dt = kDt * jitter(rng); // variable frame time
        reference += static_cast<f64>(dt);
        runtime.update(dt);
        if (numberField(runtime, a, "last_dt") == static_cast<f64>(dt) &&
            numberField(runtime, a, "frames") == frame + 1) {
            ++perFrameExact;
        }
    }
    const f64 accA = numberField(runtime, a, "acc");
    const f64 accB = numberField(runtime, b, "acc");
    std::printf("on_update over 60 frames: lua acc %.12f, C++ acc %.12f, per-frame exact %d/60\n", accA, reference,
                perFrameExact);
    expectTrue(perFrameExact == 60, "on_update runs once per frame with that frame's dt");
    expectTrue(accA == reference && accB == reference, "accumulated dt equals the C++ sum exactly");
    expectTrue(numberField(runtime, a, "frames") == 60.0, "60 frames -> 60 on_update calls");
    expectTrue(numberField(runtime, a, "started") == 1.0 && numberField(runtime, a, "frames_at_start") == 0.0,
               "on_start runs once, before the first on_update");
    expectTrue(runtime.error_count() == 0, "no script errors");

    // Destroyed entities are detached (on_destroy) on the next frame.
    expectTrue(runtime.load_module_source("reporter", R"lua(
        destroyed = 0
        function on_destroy(self) destroyed = destroyed + 1 end
    )lua").ok(), "reporter loads");
    const EntityID c = reg.create();
    runtime.attach(c, "reporter");
    runtime.update(kDt);
    reg.destroy_entity(c);
    runtime.update(kDt);
    expectTrue(!runtime.is_attached(c) && runtime.instance_count() == 2, "dead entity instance detached");
    runtime.shutdown();
}

// ---------------------------------------------------------------------------------------------
// Row: on_collision fires on first frame of contact — controlled rigid body test.
void testOnCollisionFirstContact() {
    Registry reg;
    reg.init(32);
    const EntityID ground = spawnBody(reg, 0.f, 0.f, 0.f, Collider::Plane, 0.f, true);
    const f32 radius = 0.5f;
    const EntityID ball = spawnBody(reg, 0.f, 3.f, 0.f, Collider::Sphere, radius, false);
    const EntityID zoneBall = spawnBody(reg, 5.f, 6.f, 0.f, Collider::Sphere, radius, false);
    const EntityID zone = spawnBody(reg, 5.f, 3.f, 0.f, Collider::Sphere, 0.6f, true);
    reg.get<Collider>(zone)->is_trigger = true;

    fuse::physics::PhysicsManager manager;
    manager.init({});
    fuse::physics::PhysicsStreamManager streams{};
    fuse::script::PhysicsManagerScriptBackend backend(manager, reg);

    ScriptVM vm;
    initLuaVm(vm);
    ScriptRuntime runtime;
    ScriptEngineBindings bindings;
    bindings.registry = &reg;
    bindings.physics = &backend;
    runtime.init(vm, bindings);
    expectTrue(runtime.load_module_source("ball", R"lua(
        function on_update(self, dt) self.frame = (self.frame or 0) + 1 end
        function on_collision(self, other, point)
            self.hits = (self.hits or 0) + 1
            if self.hits == 1 then
                self.hit_frame = (self.frame or 0) + 1   -- dispatched before this frame's on_update
                self.other = other
                self.py = point.y
            end
        end
        function on_trigger_enter(self, other)
            self.triggers = (self.triggers or 0) + 1
            self.trigger_frame = (self.frame or 0) + 1
            self.trigger_other = other
        end
    )lua").ok(), "ball module loads");
    runtime.attach(ball, "ball");
    runtime.attach(zoneBall, "ball");

    int engineEnterFrame = -1;
    int engineTriggerFrame = -1;
    f32 prevY = 3.f;
    f32 yAtEnter = 0.f;
    f32 yBeforeEnter = 0.f;
    for (int frame = 1; frame <= 180; ++frame) {
        manager.step(reg, kDt, streams);
        for (const fuse::physics::CollisionEvent& e : manager.lastEvents()) {
            const bool involvesBall = e.entityA == ball || e.entityB == ball;
            if (e.type == fuse::physics::CollisionEventType::Enter && involvesBall && engineEnterFrame < 0) {
                engineEnterFrame = frame;
                yAtEnter = reg.get<Transform>(ball)->position.y;
                yBeforeEnter = prevY;
            }
            const bool involvesZone = e.entityA == zoneBall || e.entityB == zoneBall;
            if (e.type == fuse::physics::CollisionEventType::Trigger && involvesZone && engineTriggerFrame < 0) {
                engineTriggerFrame = frame;
            }
        }
        fuse::script::dispatch_physics_events(manager.lastEvents(), runtime);
        runtime.update(kDt);
        prevY = reg.get<Transform>(ball)->position.y;
    }

    const f64 hitFrame = numberField(runtime, ball, "hit_frame");
    const f64 hits = numberField(runtime, ball, "hits");
    std::printf("on_collision: engine Enter frame %d, script hit frame %.0f, hits %.0f; ball y before %.4f at %.4f "
                "(r %.2f); trigger engine %d script %.0f\n",
                engineEnterFrame, hitFrame, hits, yBeforeEnter, yAtEnter, radius, engineTriggerFrame,
                numberField(runtime, zoneBall, "trigger_frame"));
    expectTrue(engineEnterFrame > 1, "ball lands after falling (controlled drop)");
    expectTrue(hitFrame == engineEnterFrame, "on_collision fires on the physics step that raised Enter");
    expectTrue(yBeforeEnter - radius > 0.f && yAtEnter - radius < 0.02f,
               "that step is the first contact: separated before, touching after");
    expectTrue(hits == 1.0, "resting contact (Stay) does not re-fire on_collision");
    expectTrue(fuse::script::decode_entity_id(numberField(runtime, ball, "other")) == ground,
               "on_collision receives the other entity");
    expectTrue(std::fabs(numberField(runtime, ball, "py")) < 0.05, "contact point lies on the ground plane");
    expectTrue(numberField(runtime, zoneBall, "trigger_frame") == engineTriggerFrame &&
                   numberField(runtime, zoneBall, "triggers") == 1.0 &&
                   fuse::script::decode_entity_id(numberField(runtime, zoneBall, "trigger_other")) == zone,
               "on_trigger_enter fires once on the trigger step");
    expectTrue(runtime.error_count() == 0, "no script errors in collision test");
}

// ---------------------------------------------------------------------------------------------
// Row: Hot-reload replaces script function mid-run — new behaviour active within 1 frame.
void writeFile(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::file_time_type previous{};
    std::error_code ec;
    const bool existed = std::filesystem::exists(path, ec);
    if (existed) {
        previous = std::filesystem::last_write_time(path, ec);
    }
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << text;
    }
    // Coarse-mtime filesystems: make sure the stamp moves.
    if (existed && std::filesystem::last_write_time(path, ec) <= previous) {
        std::filesystem::last_write_time(path, previous + std::chrono::seconds(1), ec);
    }
}

void testHotReload() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "fuse_b7_script_hot_reload";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const std::filesystem::path file = dir / "mover.lua";
    writeFile(file, R"lua(
        local M = {}
        function M.on_update(self, dt)
            self.count = (self.count or 0) + 1
            self.version = 1
        end
        return M
    )lua");

    Registry reg;
    reg.init(16);
    ScriptVM vm;
    initLuaVm(vm);
    ScriptRuntime runtime;
    ScriptEngineBindings bindings;
    bindings.registry = &reg;
    runtime.init(vm, bindings);
    ScriptHotReload watcher;
    expectTrue(watcher.watch_directory(dir.string().c_str()) == 1, "watcher finds the script");
    expectTrue(watcher.poll(runtime) == 1, "first poll loads the module");
    const std::string key = file.string();
    const EntityID e = reg.create();
    expectTrue(runtime.attach(e, key.c_str()), "instance attaches to the file module");

    for (int i = 0; i < 10; ++i) {
        expectTrue(watcher.poll(runtime) == 0, "no reload without changes");
        runtime.update(kDt);
    }
    expectTrue(numberField(runtime, e, "count") == 10.0 && numberField(runtime, e, "version") == 1.0,
               "v1 behaviour for 10 frames");

    // Edit mid-run: +100 per frame.
    writeFile(file, R"lua(
        local M = {}
        function M.on_update(self, dt)
            self.count = (self.count or 0) + 100
            self.version = 2
        end
        return M
    )lua");
    const usize reloaded = watcher.poll(runtime); // same frame as the edit is observed
    runtime.update(kDt);
    std::printf("hot reload: reloaded %zu, count after first frame %.0f (expect 110), version %.0f\n", reloaded,
                numberField(runtime, e, "count"), numberField(runtime, e, "version"));
    expectTrue(reloaded == 1 && runtime.module_version(key.c_str()) == 2, "edit is reloaded on the next poll");
    expectTrue(numberField(runtime, e, "version") == 2.0, "new function runs on the first frame after the edit");
    expectTrue(numberField(runtime, e, "count") == 110.0, "instance state survives the reload (10 + 100)");

    // A broken edit keeps the last good version running and is reported, not fatal.
    writeFile(file, "local M = {}\nfunction M.on_update(self, dt\n  self.count = -1\nend\nreturn M\n");
    expectTrue(watcher.poll(runtime) == 0 && watcher.failure_count() == 1, "syntax error reload rejected");
    runtime.update(kDt);
    expectTrue(numberField(runtime, e, "count") == 210.0 && numberField(runtime, e, "version") == 2.0,
               "previous version keeps running after a failed reload");
    expectTrue(watcher.last_error().find("mover.lua") != std::string::npos, "reload error names the file");

    // Removing a callback removes the behaviour.
    writeFile(file, "local M = {}\nfunction M.on_start(self) end\nreturn M\n");
    expectTrue(watcher.poll(runtime) == 1, "fixed edit reloads");
    runtime.update(kDt);
    expectTrue(numberField(runtime, e, "count") == 210.0, "deleted on_update no longer runs");

    // Global-function style modules reload the same way.
    expectTrue(runtime.load_module_source("globals", "function on_update(self) self.v = 1 end").ok(),
               "global-style module loads");
    const EntityID g = reg.create();
    runtime.attach(g, "globals");
    runtime.update(kDt);
    expectTrue(runtime.load_module_source("globals", "function on_update(self) self.v = 2 end").ok(),
               "global-style module reloads");
    runtime.update(kDt);
    expectTrue(numberField(runtime, g, "v") == 2.0, "global-style reload active next frame");
    runtime.shutdown();
    std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------------------------------------
// Error isolation: a failing script never stops the engine or the other scripts.
void testErrorIsolation() {
    Registry reg;
    reg.init(16);
    ScriptVM vm;
    initLuaVm(vm);
    ScriptRuntime runtime;
    ScriptEngineBindings bindings;
    bindings.registry = &reg;
    runtime.init(vm, bindings);
    runtime.load_module_source("bad", R"lua(
        function on_start(self) error("start failed") end
        function on_update(self, dt) local t = nil; t.x = 1 end
    )lua");
    runtime.load_module_source("thrower", "function on_update(self) error({code = 7}) end");
    runtime.load_module_source("good", "function on_update(self) self.n = (self.n or 0) + 1 end");
    const EntityID bad = reg.create();
    const EntityID thrower = reg.create();
    const EntityID good = reg.create();
    runtime.attach(bad, "bad");
    runtime.attach(thrower, "thrower");
    runtime.attach(good, "good");
    for (int i = 0; i < 30; ++i) {
        runtime.update(kDt);
    }
    std::printf("error isolation: %zu errors, last: %.60s\n", runtime.error_count(), runtime.last_error().c_str());
    expectTrue(numberField(runtime, good, "n") == 30.0, "healthy script runs every frame beside failing ones");
    expectTrue(runtime.error_count() == 61, "every failure is recorded (1 on_start + 30 + 30 on_update)");
    expectTrue(runtime.last_error().find("thrower:on_update") != std::string::npos, "error names module:callback");

    const auto parse = runtime.load_module_source("broken", "function (");
    expectTrue(parse.status == ScriptLoadStatus::ParseError && !runtime.has_module("broken"),
               "module with a syntax error is rejected");
    expectTrue(!runtime.attach(reg.create(), "broken"), "rejected module cannot be attached");
}

// ---------------------------------------------------------------------------------------------
// Sandboxing: only the FUSE API reaches the host.
void testSandbox() {
    ScriptVM sandboxed;
    initLuaVm(sandboxed);
    const auto probe = sandboxed.load_string(R"lua(
        escaped = {}
        local names = {"io", "debug", "package", "require", "dofile", "loadfile", "load", "loadstring",
                       "collectgarbage"}
        for _, n in ipairs(names) do if _G[n] ~= nil then escaped[#escaped + 1] = n end end
        if os.execute ~= nil or os.remove ~= nil or os.getenv ~= nil or os.exit ~= nil then
            escaped[#escaped + 1] = "os"
        end
        if string.dump ~= nil then escaped[#escaped + 1] = "string.dump" end
        escaped_count = #escaped
        clock_ok = type(os.clock()) == "number"
    )lua", "sandbox_probe");
    expectTrue(probe.ok(), "sandbox probe runs");
    bind::ScriptValue count;
    sandboxed.load_string("function count() return escaped_count end", "c");
    sandboxed.call_global("count", nullptr, 0, &count);
    expectTrue(bind::to_number(count, -1.0) == 0.0, "no host-escaping globals in the sandbox");
    expectTrue(sandboxed.load_string("assert(clock_ok)", "clock").ok(), "os.clock stays available");
    expectTrue(sandboxed.load_string("io.open('/tmp/x', 'w')", "io").status == ScriptLoadStatus::RuntimeError,
               "file IO attempt is a script error");

    ScriptVMDesc open;
    open.sandboxed = false;
    ScriptVM full;
    initLuaVm(full, open);
    expectTrue(full.load_string("assert(io ~= nil and debug ~= nil and require ~= nil)", "full").ok(),
               "unsandboxed VM keeps the standard libraries");
}

// ---------------------------------------------------------------------------------------------
// Instruction budget: runaway scripts are aborted, the frame continues.
void testInstructionBudget() {
    Registry reg;
    reg.init(8);
    ScriptVMDesc desc;
    desc.instruction_budget = 2'000'000;
    ScriptVM vm;
    initLuaVm(vm, desc);
    ScriptRuntime runtime;
    ScriptEngineBindings bindings;
    bindings.registry = &reg;
    runtime.init(vm, bindings);
    runtime.load_module_source("spin", "function on_update(self) while true do end end");
    runtime.load_module_source("spin_co", R"lua(
        function on_update(self)
            local co = coroutine.create(function() while true do end end)
            local ok, err = coroutine.resume(co)
            while true do end   -- keeps spinning even after the coroutine aborted
        end
    )lua");
    runtime.load_module_source("ok", "function on_update(self) self.n = (self.n or 0) + 1 end");
    const EntityID spin = reg.create();
    const EntityID spinCo = reg.create();
    const EntityID ok = reg.create();
    runtime.attach(spin, "spin");
    runtime.attach(spinCo, "spin_co");
    runtime.attach(ok, "ok");

    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 5; ++i) {
        runtime.update(kDt);
    }
    const f64 ms = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::printf("instruction budget: 5 frames with 2 infinite loops took %.1f ms, errors %zu\n", ms,
                runtime.error_count());
    std::printf("budget last error: %s\n", runtime.last_error().c_str());
    expectTrue(runtime.error_count() == 10, "each runaway callback aborted every frame");
    expectTrue(runtime.last_error().find("instruction budget exceeded") != std::string::npos,
               "abort reason reported");
    expectTrue(numberField(runtime, ok, "n") == 5.0, "other scripts keep running");
    expectTrue(ms < 5000.0, "runaway scripts do not hang the game thread");

    // Legit work under the budget is untouched.
    expectTrue(vm.load_string("local s = 0 for i = 1, 100000 do s = s + i end assert(s == 5000050000)", "sum").ok(),
               "bounded loop under the budget completes");
}

// ---------------------------------------------------------------------------------------------
// Memory cap and GC bounds.
void testMemoryBounds() {
    ScriptVMDesc desc;
    desc.memory_limit_bytes = 8u << 20;
    ScriptVM vm;
    initLuaVm(vm, desc);
    const usize baseline = vm.memory_bytes();
    const auto hog = vm.load_string("local t = {} for i = 1, 1e8 do t[i] = {i} end", "hog");
    std::printf("memory: baseline %zu B, peak %zu B (cap %zu B), hog -> %s\n", baseline, vm.peak_memory_bytes(),
                desc.memory_limit_bytes, hog.message != nullptr ? hog.message : "ok");
    expectTrue(hog.status == ScriptLoadStatus::RuntimeError, "allocation past the cap is a script error");
    expectTrue(vm.peak_memory_bytes() <= desc.memory_limit_bytes, "Lua heap never exceeds the cap");
    vm.collect_garbage();
    expectTrue(vm.memory_bytes() < baseline + (256u << 10), "hog garbage is reclaimed");
    expectTrue(vm.load_string("x = {1, 2, 3}", "after_oom").ok(), "VM usable after out-of-memory");

    // Steady per-frame garbage stays bounded by the incremental GC.
    Registry reg;
    reg.init(8);
    ScriptRuntime runtime;
    ScriptEngineBindings bindings;
    bindings.registry = &reg;
    runtime.init(vm, bindings);
    runtime.load_module_source("churn", R"lua(
        function on_update(self, dt)
            local garbage = {}
            for i = 1, 200 do garbage[i] = {x = i, y = dt, s = "tmp" .. i} end
            self.last = #garbage
        end
    )lua");
    const EntityID e = reg.create();
    runtime.attach(e, "churn");
    usize worst = 0;
    for (int i = 0; i < 2000; ++i) {
        runtime.update(kDt);
        worst = vm.memory_bytes() > worst ? vm.memory_bytes() : worst;
    }
    std::printf("GC churn: 2000 frames x 200 tables, worst heap %zu B\n", worst);
    expectTrue(runtime.error_count() == 0 && worst < (4u << 20), "heap stays bounded under per-frame garbage");
}

// ---------------------------------------------------------------------------------------------
// Coroutine / yield semantics across frames.
void testCoroutines() {
    Registry reg;
    reg.init(8);
    ScriptVM vm;
    initLuaVm(vm);
    ScriptRuntime runtime;
    ScriptEngineBindings bindings;
    bindings.registry = &reg;
    runtime.init(vm, bindings);
    runtime.load_module_source("sequence", R"lua(
        function on_start(self)
            self.log = ""
            self.co = coroutine.create(function(me)
                for step = 1, 3 do
                    me.log = me.log .. step
                    coroutine.yield()            -- wait one frame
                end
                me.done = 1
            end)
        end
        function on_update(self, dt)
            if coroutine.status(self.co) ~= "dead" then
                assert(coroutine.resume(self.co, self))
            end
        end
    )lua");
    const EntityID e = reg.create();
    runtime.attach(e, "sequence");
    std::string logs;
    for (int frame = 0; frame < 5; ++frame) {
        runtime.update(kDt);
        bind::ScriptValue log;
        runtime.get_instance_field(e, "log", log);
        logs += bind::to_string(log) + "|";
    }
    expectTrue(logs == "1|12|123|123|123|", "coroutine advances one step per frame and stops");
    expectTrue(numberField(runtime, e, "done") == 1.0, "coroutine completes");

    // Generator semantics against a C++ reference.
    ScriptVM plain;
    initLuaVm(plain);
    plain.load_string(R"lua(
        function fib_list(n)
            local gen = coroutine.wrap(function()
                local a, b = 0, 1
                while true do coroutine.yield(a); a, b = b, a + b end
            end)
            local out = {}
            for i = 1, n do out[i] = string.format("%d", gen()) end
            return table.concat(out, ",")
        end
    )lua", "fib");
    const bind::ScriptValue n = bind::push_number(15);
    bind::ScriptValue fib;
    plain.call_global("fib_list", &n, 1, &fib);
    std::string reference;
    u64 a = 0, b = 1;
    for (int i = 0; i < 15; ++i) {
        reference += (i ? "," : "") + std::to_string(a);
        const u64 next = a + b;
        a = b;
        b = next;
    }
    expectTrue(bind::to_string(fib) == reference, "coroutine.wrap generator matches the C++ Fibonacci reference");
    expectTrue(runtime.error_count() == 0, "no coroutine errors");
}

} // namespace

int main() {
    fuse::core::initialize();

#if defined(FUSE_SCRIPT_LUA) && FUSE_SCRIPT_LUA
    testHelloWorld();
    testEntityPositionRoundTrip();
    testPhysicsRayCast();
    testOnUpdateDt();
    testOnCollisionFirstContact();
    testHotReload();
    testErrorIsolation();
    testSandbox();
    testInstructionBudget();
    testMemoryBounds();
    testCoroutines();
#else
    std::fprintf(stderr, "FAIL: fuse_script built without a Lua backend (FUSE_SCRIPT_LUA=0)\n");
    ++g_failures;
#endif

    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_script_b7_gates: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("fuse_script_b7_gates: all gates passed\n");
    return EXIT_SUCCESS;
}
