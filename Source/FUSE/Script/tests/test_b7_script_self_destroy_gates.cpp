// B7.10 integration gate row (master plan): "Script-controlled entity destroys itself on
// collision — no dangling entity handles".
//
// A Lua behaviour calls `Entity.destroy(self)` from `on_collision`. The doomed ball is dropped
// symmetrically between two static pillars so it raises two Enter events on the same physics
// step (the second dispatch arrives after the entity already died), and it carries a joint to a
// survivor. After the destroy, nothing may still hand out the dead handle:
//  - scripts: on_collision runs once; Entity.alive(self) is false; the instance is detached
//    (on_destroy runs) on the next update; no later callback receives the dead entity as `self`,
//  - physics queries: Physics.ray_cast / PhysicsManager::rayCast / querySphere never return it,
//  - physics state: its body leaves the solver on the next step (bodyIndex, entityOf), the joint
//    to it is released,
//  - events: no collision event raised after the destroy step names it,
//  - handle reuse: a new entity recycling the slot (next generation) is a distinct body.
#include <fuse/core/init.hpp>
#include <fuse/ecs/components/collider.hpp>
#include <fuse/ecs/components/rigidbody.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/registry.hpp>
#include <fuse/physics/physics_manager.hpp>
#include <fuse/script/script_engine_api.hpp>
#include <fuse/script/script_physics_bridge.hpp>
#include <fuse/script/script_runtime.hpp>
#include <fuse/script/script_vm.hpp>
#include <fuse/types.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

using fuse::f32;
using fuse::f64;
using fuse::u32;
using fuse::ecs::Collider;
using fuse::ecs::EntityID;
using fuse::ecs::Registry;
using fuse::ecs::Transform;
using fuse::script::ScriptEngineBindings;
using fuse::script::ScriptRuntime;
using fuse::script::ScriptVM;
namespace bind = fuse::script::bind;

constexpr f32 kDt = 1.f / 60.f;
constexpr u32 kNoBody = ~0u;

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

f64 globalNumber(ScriptVM& vm, const char* name) {
    bind::ScriptValue v;
    if (!vm.call_global(name, nullptr, 0, &v).ok() || !bind::is_number(v)) {
        return -1.0e30;
    }
    return bind::to_number(v);
}

bool eventNames(const fuse::physics::CollisionEvent& e, EntityID id) { return e.entityA == id || e.entityB == id; }

void testSelfDestroyOnCollision() {
    Registry reg;
    reg.init(64);
    const EntityID ground = spawnBody(reg, 0.f, 0.f, 0.f, Collider::Plane, 0.f, true);
    // Two static pillars; the doomed ball drops exactly between them and touches both at once.
    const EntityID pillarL = spawnBody(reg, -0.8f, 0.5f, 0.f, Collider::Sphere, 0.5f, true);
    const EntityID pillarR = spawnBody(reg, 0.8f, 0.5f, 0.f, Collider::Sphere, 0.5f, true);
    const EntityID doomed = spawnBody(reg, 0.f, 3.f, 0.f, Collider::Sphere, 0.5f, false);
    // A survivor falls onto the ground later and is jointed (rope) to the doomed ball.
    const EntityID survivor = spawnBody(reg, 4.f, 6.f, 0.f, Collider::Sphere, 0.5f, false);

    fuse::physics::PhysicsManager manager;
    manager.init({});
    fuse::physics::PhysicsStreamManager streams{};
    fuse::script::PhysicsManagerScriptBackend backend(manager, reg);
    const fuse::physics::JointHandle rope =
        manager.createJoint(reg, fuse::physics::JointDesc::rope(doomed, survivor, {0.f, 3.f, 0.f},
                                                               {4.f, 6.f, 0.f}, 50.f));
    expectTrue(rope.valid(), "rope joint between doomed and survivor created");

    ScriptVM vm;
    expectTrue(vm.init({}) && vm.has_lua_backend(), "Lua VM initialises");
    ScriptRuntime runtime;
    ScriptEngineBindings bindings;
    bindings.registry = &reg;
    bindings.physics = &backend;
    runtime.init(vm, bindings);
    // Counters live in _G (module chunks run in their own environment); host reads them back.
    expectTrue(vm.load_string(R"lua(
        collisions = 0          -- every on_collision call on a doomed instance
        destroyed_ok = 0        -- Entity.destroy(self) returned true
        alive_after = 0         -- Entity.alive(self) still true right after the destroy
        destroy_calls = 0       -- on_destroy
        seen = {}               -- `other` of every survivor on_collision
        function get_collisions() return collisions end
        function get_destroyed_ok() return destroyed_ok end
        function get_alive_after() return alive_after end
        function get_destroy_calls() return destroy_calls end
        function seen_count() return #seen end
        function seen_at(i) return seen[i] end
        function probe(x, y, z)
            local hit = Physics.ray_cast({x = x, y = y, z = z}, {x = 0, y = -1, z = 0}, 20)
            if hit == nil then return -1 end
            return hit.entity
        end
    )lua", "self_destroy_host").ok(), "host helpers load");
    expectTrue(runtime.load_module_source("self_destruct", R"lua(
        function on_collision(self, other, point)
            _G.collisions = _G.collisions + 1
            if Entity.destroy(self) then _G.destroyed_ok = _G.destroyed_ok + 1 end
            if Entity.alive(self) then _G.alive_after = _G.alive_after + 1 end
        end
        function on_destroy(self) _G.destroy_calls = _G.destroy_calls + 1 end
    )lua").ok(), "self_destruct module loads");
    expectTrue(runtime.load_module_source("watcher", R"lua(
        function on_collision(self, other, point) _G.seen[#_G.seen + 1] = other end
    )lua").ok(), "watcher module loads");
    expectTrue(runtime.attach(doomed, "self_destruct"), "doomed ball runs the self-destruct script");
    expectTrue(runtime.attach(survivor, "watcher"), "survivor runs the watcher script");
    runtime.update(kDt);

    int destroyFrame = -1;
    int enterEventsOnDestroyStep = 0;
    int eventsNamingDeadAfter = 0;
    int deadBodySteps = 0;
    bool rayHitDeadCpp = false;
    bool sphereHitDead = false;
    f64 scriptRayHit = 0.0;
    const u32 bodiesBefore = 5;

    for (int frame = 1; frame <= 150; ++frame) {
        manager.step(reg, kDt, streams);
        if (destroyFrame >= 0) {
            for (const fuse::physics::CollisionEvent& e : manager.lastEvents()) {
                if (eventNames(e, doomed)) {
                    ++eventsNamingDeadAfter;
                    if (eventsNamingDeadAfter <= 3) {
                        std::fprintf(stderr, "  step %d: event type %d names destroyed entity\n", frame,
                                     static_cast<int>(e.type));
                    }
                }
            }
            if (manager.bodyIndex(doomed) != kNoBody) {
                ++deadBodySteps;
            }
            for (u32 b = 0; b < manager.bodies().count(); ++b) {
                if (manager.entityOf(b) == doomed) {
                    ++deadBodySteps;
                }
            }
        }
        const bool destroyStep = destroyFrame < 0;
        if (destroyStep) {
            for (const fuse::physics::CollisionEvent& e : manager.lastEvents()) {
                if (e.type == fuse::physics::CollisionEventType::Enter && eventNames(e, doomed)) {
                    ++enterEventsOnDestroyStep;
                }
            }
        }
        fuse::script::dispatch_physics_events(manager.lastEvents(), runtime);
        if (destroyFrame < 0 && !reg.alive(doomed)) {
            destroyFrame = frame;
            // Same frame, after the script destroyed it and before the next physics step: queries
            // must not return the dead handle.
            EntityID hit{};
            fuse::physics::vec3 normal{};
            f32 t = 0.f;
            if (manager.rayCast({0.f, 10.f, 0.f}, {0.f, -1.f, 0.f}, 20.f, hit, normal, t, &reg)) {
                rayHitDeadCpp = hit == doomed;
            }
            std::vector<EntityID> near;
            manager.querySphere({0.f, 1.f, 0.f}, 0.3f, near, &reg);
            for (const EntityID id : near) {
                sphereHitDead = sphereHitDead || id == doomed;
            }
            bind::ScriptValue args[3] = {bind::push_number(0.0), bind::push_number(10.0),
                                         bind::push_number(0.0)};
            bind::ScriptValue out;
            vm.call_global("probe", args, 3, &out);
            scriptRayHit = bind::is_number(out) ? bind::to_number(out) : -2.0;
        }
        runtime.update(kDt);
    }

    const f64 collisions = globalNumber(vm, "get_collisions");
    const f64 destroyedOk = globalNumber(vm, "get_destroyed_ok");
    const f64 aliveAfter = globalNumber(vm, "get_alive_after");
    const f64 destroyCalls = globalNumber(vm, "get_destroy_calls");
    const f64 seenCount = globalNumber(vm, "seen_count");
    bool survivorSawDead = false;
    for (int i = 1; i <= static_cast<int>(seenCount); ++i) {
        bind::ScriptValue args[1] = {bind::push_number(i)};
        bind::ScriptValue out;
        vm.call_global("seen_at", args, 1, &out);
        survivorSawDead = survivorSawDead ||
                          (bind::is_number(out) && fuse::script::decode_entity_id(bind::to_number(out)) == doomed);
    }
    const f64 deadEncoded = fuse::script::encode_entity_id(doomed);

    std::printf("self-destroy: destroyed on step %d with %d Enter events that step; on_collision calls %.0f, "
                "destroy ok %.0f, on_destroy %.0f; bodies %u -> %u; joints %u; post-destroy events naming it %d; "
                "ray (C++) dead=%d, sphere dead=%d, script ray entity %s; survivor collisions %.0f\n",
                destroyFrame, enterEventsOnDestroyStep, collisions, destroyedOk, destroyCalls, bodiesBefore,
                manager.bodies().count(), manager.jointCount(), eventsNamingDeadAfter, rayHitDeadCpp ? 1 : 0,
                sphereHitDead ? 1 : 0, scriptRayHit == deadEncoded ? "DEAD" : "ok", seenCount);

    expectTrue(destroyFrame > 1, "doomed ball falls, collides and destroys itself");
    expectTrue(enterEventsOnDestroyStep >= 2, "the fatal step raised two Enter events for the doomed ball");
    expectTrue(destroyedOk == 1.0 && aliveAfter == 0.0, "Entity.destroy(self) destroys it; Entity.alive(self) is false");
    expectTrue(collisions == 1.0, "no on_collision is delivered to an already destroyed entity");
    expectTrue(destroyCalls == 1.0 && !runtime.is_attached(doomed) && runtime.instance_count() == 1,
               "script instance detached (on_destroy once) on the next update");
    expectTrue(!rayHitDeadCpp, "PhysicsManager::rayCast(aliveIn) never returns the destroyed entity");
    expectTrue(!sphereHitDead, "PhysicsManager::querySphere(aliveIn) never returns the destroyed entity");
    expectTrue(scriptRayHit != deadEncoded, "Physics.ray_cast (Lua) never returns the destroyed entity");
    expectTrue(deadBodySteps == 0, "destroyed entity has no solver body after the next step");
    expectTrue(manager.bodies().count() == bodiesBefore - 1u, "exactly one body left the simulation");
    expectTrue(!manager.isJointValid(rope) && manager.jointCount() == 0u, "joint to the destroyed entity released");
    expectTrue(eventsNamingDeadAfter == 0, "no collision event after the destroy step names the dead entity");
    expectTrue(seenCount >= 1.0 && !survivorSawDead, "survivor collided later and never saw the dead entity");
    expectTrue(reg.alive(survivor) && reg.alive(ground) && reg.alive(pillarL) && reg.alive(pillarR),
               "other entities unaffected");
    expectTrue(runtime.error_count() == 0, "no script errors");
    if (runtime.error_count() != 0) {
        std::fprintf(stderr, "  last script error: %s\n", runtime.last_error().c_str());
    }

    // Handle reuse: a new body recycling the slot is a distinct entity and a distinct solver body.
    const EntityID reborn = spawnBody(reg, 0.f, 3.f, 0.f, Collider::Sphere, 0.5f, false);
    manager.step(reg, kDt, streams);
    expectTrue(reborn.index != doomed.index || reborn.generation != doomed.generation, "reused slot gets a new handle");
    expectTrue(manager.bodyIndex(reborn) != kNoBody && manager.bodyIndex(doomed) == kNoBody,
               "old handle stays dead while the recycled slot simulates");
    expectTrue(!runtime.is_attached(reborn) && !runtime.is_attached(doomed), "no script instance leaks onto the reused slot");

    runtime.shutdown();
    manager.destroy();
}

} // namespace

int main() {
    fuse::core::initialize();
    testSelfDestroyOnCollision();
    fuse::core::shutdown();
    if (g_failures == 0) {
        std::printf("fuse_script_b7_self_destroy_gates: all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "fuse_script_b7_self_destroy_gates: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
