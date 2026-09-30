// MP-B7.3-SCRIPT-COMPONENT / UNI-U3-SCRIPT-1 gates (CPU):
//  - ecs::Script is a registered, trivially copyable component (module path + exposed properties)
//  - an entity with a Script component moves during ScriptSystem ticks; exposed properties reach `self`
//  - component add / remove / disable / module change / entity destroy attach and detach behaviours,
//    running on_start / on_destroy exactly once each
//  - enterPlay / exitPlay (PIE lifecycle) and physics-style contact dispatch
//  - scene save / load round trip re-attaches behaviours with their properties; runtime state is not
//    serialised; corrupt Script rows are rejected
//  - Script cook: Lua syntax check + bytecode `.fusescript`, legacy `.cs` tagged `t3d:` and passed
//    through, syntax errors rejected with file:line
//  - zero heap allocations in ScriptSystem::update at steady state
#include <fuse/ecs/component_types.hpp>
#include <fuse/ecs/components/script.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/registry.hpp>
#include <fuse/ecs/registry_serialiser.hpp>
#include <fuse/script/script_cook.hpp>
#include <fuse/script/script_host_service.hpp>
#include <fuse/script/script_runtime.hpp>
#include <fuse/script/script_system.hpp>
#include <fuse/script/script_vm.hpp>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <new>
#include <string>
#include <type_traits>
#include <vector>

// ---- allocation counter (plain new/delete; aligned forms keep the library defaults) -------------
namespace {
std::atomic<unsigned long long> g_newCount{0};
}

void* operator new(std::size_t size) {
    g_newCount.fetch_add(1u, std::memory_order_relaxed);
    if (void* p = std::malloc(size == 0 ? 1 : size)) {
        return p;
    }
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) {
    g_newCount.fetch_add(1u, std::memory_order_relaxed);
    if (void* p = std::malloc(size == 0 ? 1 : size)) {
        return p;
    }
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {

int g_failures = 0;

using fuse::f32;
using fuse::f64;
using fuse::u32;
using fuse::u64;
using fuse::usize;
using fuse::ecs::EntityID;
using fuse::ecs::Registry;
using fuse::ecs::Script;
using fuse::ecs::ScriptProperty;
using fuse::ecs::Transform;
using fuse::script::ScriptEngineBindings;
using fuse::script::ScriptRuntime;
using fuse::script::ScriptSystem;
using fuse::script::ScriptVM;
using fuse::script::ScriptVMDesc;
namespace bind = fuse::script::bind;
namespace fs = std::filesystem;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

constexpr f32 kDt = 1.f / 60.f;

static_assert(std::is_trivially_copyable_v<Script>, "Script is stored and serialised as bytes");
static_assert(std::is_trivially_destructible_v<Script>, "ECS components are trivially destructible");

// Moves along `dir * speed`; counts lifecycle calls in globals (read back with `counter(...)`).
constexpr const char* kMover = R"lua(
function on_start(self)
    self.ticks = 0
    _G.started = (_G.started or 0) + 1
end
function on_update(self, dt)
    self.ticks = self.ticks + 1
    local p = Entity.get_position(self)
    Entity.set_position(self, {x = p.x + self.dir.x * self.speed * dt,
                               y = p.y + self.dir.y * self.speed * dt,
                               z = p.z + self.dir.z * self.speed * dt})
end
function on_collision(self, other, point)
    self.hits = (self.hits or 0) + 1
    self.last_other = other
    self.last_y = point.y
end
function on_trigger_enter(self, other)
    self.triggers = (self.triggers or 0) + 1
end
function on_destroy(self)
    _G.destroyed = (_G.destroyed or 0) + 1
    if Entity.alive(self) then _G.destroyed_alive = (_G.destroyed_alive or 0) + 1 end
end
)lua";

constexpr const char* kSpinner = R"lua(
function on_start(self) _G.spinner_started = (_G.spinner_started or 0) + 1 end
function on_update(self, dt) self.spun = (self.spun or 0) + 1 end
)lua";

constexpr const char* kCounters = R"lua(
function counter(name) return _G[name] or 0 end
)lua";

struct World {
    Registry registry;
    ScriptVM vm;
    ScriptRuntime runtime;
    ScriptSystem system;

    bool init(usize heapReserve = 0) {
        registry.init(1024);
        ScriptVMDesc desc;
        desc.memory_limit_bytes = 32u * 1024u * 1024u;
        desc.instruction_budget = 5'000'000u;
        desc.heap_reserve_bytes = heapReserve;
        if (!vm.init(desc) || !vm.has_lua_backend()) {
            return false;
        }
        ScriptEngineBindings bindings;
        bindings.registry = &registry;
        return runtime.init(vm, bindings) && runtime.load_module_source("mover", kMover).ok() &&
               runtime.load_module_source("spinner", kSpinner).ok() && vm.load_string(kCounters, "counters").ok() &&
               system.init(registry, runtime);
    }

    ~World() {
        system.shutdown();
        runtime.shutdown();
    }

    f64 counter(const char* name) {
        const bind::ScriptValue arg = bind::push_string(name);
        bind::ScriptValue out;
        if (!vm.call_global("counter", &arg, 1, &out).ok() || !bind::is_number(out)) {
            return -1.0;
        }
        return bind::to_number(out);
    }

    f64 field(EntityID id, const char* name) {
        bind::ScriptValue value;
        if (!runtime.get_instance_field(id, name, value) || !bind::is_number(value)) {
            return -1.0e30;
        }
        return bind::to_number(value);
    }

    EntityID spawn(const char* module, f32 x, f64 speed, f32 dx = 1.f, f32 dy = 0.f, f32 dz = 0.f) {
        const EntityID id = registry.create();
        Transform t{};
        t.position = {x, 0.f, 0.f, 1.f};
        registry.add(id, t);
        Script s{};
        (void)s.set_path(module);
        (void)s.set_number("speed", speed);
        (void)s.set_vec3("dir", dx, dy, dz);
        registry.add(id, s);
        return id;
    }
};

f32 posX(Registry& registry, EntityID id) {
    const Transform* t = registry.get<Transform>(id);
    return t != nullptr ? t->position.x : -1.0e30f;
}

bool approx(f64 a, f64 b, f64 eps = 1e-3) { return std::fabs(a - b) <= eps; }

// ---------------------------------------------------------------------------------------------
void testComponentBasics() {
    fuse::ecs::register_builtin_components();
    const fuse::ecs::ComponentTypeInfo* info = fuse::ecs::ComponentTypes::find("Script");
    expectTrue(info != nullptr && info->size == sizeof(Script), "Script registered for serialisation");

    Script s{};
    expectTrue(s.enabled && !s.started && s.lua_ref == Script::kNoRef && s.property_count == 0u,
               "Script defaults: enabled, not started, no lua_ref, no properties");
    expectTrue(s.set_path("scripts/door.lua") && s.path() == "scripts/door.lua", "set_path round trip");
    expectTrue(!s.set_path(std::string(Script::kPathCapacity, 'x')), "overlong path rejected (not truncated)");
    expectTrue(s.path() == "scripts/door.lua", "rejected path leaves the old one");
    expectTrue(s.set_number("speed", 3.5) && s.set_bool("open", true) && s.set_string("label", "north") &&
                   s.set_vec3("axis", 0.f, 1.f, 0.f),
               "number / bool / string / vec3 properties set");
    expectTrue(s.property_count == 4u, "four properties stored");
    expectTrue(s.set_number("speed", 4.0) && s.property_count == 4u && s.find_property("speed")->number == 4.0,
               "setting an existing key overwrites it");
    const ScriptProperty* label = s.find_property("label");
    expectTrue(label != nullptr && label->type == ScriptProperty::Type::String && label->string_value() == "north",
               "string property value");
    for (int i = 0; i < 4; ++i) {
        const std::string key = "k" + std::to_string(i);
        expectTrue(s.set_bool(key, true), "fills the property table");
    }
    expectTrue(!s.set_bool("overflow", true), "property table is bounded");
    expectTrue(!s.set_string("long", std::string(ScriptProperty::kTextCapacity, 'y')), "overlong string rejected");
}

// ---------------------------------------------------------------------------------------------
void testMovesDuringTicks() {
    World w;
    expectTrue(w.init(), "world init");
    const EntityID a = w.spawn("mover", 0.f, 2.0);
    const EntityID b = w.spawn("mover", 10.f, 1.0, -1.f, 0.f, 0.f);
    const EntityID plain = w.registry.create();
    w.registry.add(plain, Transform{});

    w.system.update(kDt);
    expectTrue(w.system.attached_count() == 0u && posX(w.registry, a) == 0.f,
               "outside play nothing attaches and nothing ticks");

    w.system.enterPlay();
    expectTrue(w.system.playing() && w.system.attached_count() == 2u, "enterPlay attaches both Script entities");
    const Script* sa = w.registry.get<Script>(a);
    expectTrue(sa->lua_ref != Script::kNoRef && !sa->started, "attach sets lua_ref; started after the first tick");
    for (int f = 0; f < 60; ++f) {
        w.system.update(kDt);
    }
    expectTrue(w.registry.get<Script>(a)->started && w.registry.get<Script>(b)->started, "started written back");
    expectTrue(approx(posX(w.registry, a), 2.0, 1e-3), "entity A moved 2 units in 1 s (speed property 2)");
    expectTrue(approx(posX(w.registry, b), 9.0, 1e-3), "entity B moved -1 unit in 1 s (dir property -x)");
    expectTrue(w.field(a, "ticks") == 60.0, "on_update ran every tick");
    expectTrue(w.counter("started") == 2.0, "on_start ran once per behaviour");
    expectTrue(!w.system.is_attached(plain) && w.runtime.error_count() == 0u, "no behaviour without a Script");
    if (w.runtime.error_count() != 0u) {
        std::fprintf(stderr, "script error: %s\n", w.runtime.last_error().c_str());
    }

    // String / bool properties reach `self` too.
    Script* script = w.registry.get<Script>(a);
    (void)script->set_string("label", "hero");
    (void)script->set_bool("armed", true);
    w.system.exitPlay();
    expectTrue(w.counter("destroyed") == 2.0, "exitPlay runs on_destroy for every started behaviour");
    expectTrue(w.registry.get<Script>(a)->lua_ref == Script::kNoRef && !w.registry.get<Script>(a)->started,
               "exitPlay clears lua_ref / started");
    w.system.enterPlay();
    bind::ScriptValue label;
    bind::ScriptValue armed;
    expectTrue(w.runtime.get_instance_field(a, "label", label) && bind::is_string(label) &&
                   bind::to_string(label) == "hero",
               "string property copied into self");
    expectTrue(w.runtime.get_instance_field(a, "armed", armed) && bind::is_bool(armed) && bind::to_bool(armed),
               "bool property copied into self");
}

// ---------------------------------------------------------------------------------------------
void testAttachDetachLifecycle() {
    World w;
    expectTrue(w.init(), "world init");
    w.system.enterPlay();
    const EntityID a = w.spawn("mover", 0.f, 1.0);
    w.system.update(kDt);
    expectTrue(w.system.is_attached(a) && w.counter("started") == 1.0, "component added in play attaches next tick");

    // Remove the component: detach + on_destroy.
    w.registry.remove<Script>(a);
    w.system.update(kDt);
    expectTrue(!w.system.is_attached(a) && !w.runtime.is_attached(a) && w.counter("destroyed") == 1.0,
               "component removal detaches and runs on_destroy");

    // Disabled component: no behaviour; enabling attaches a fresh one.
    Script s{};
    (void)s.set_path("mover");
    (void)s.set_number("speed", 1.0);
    (void)s.set_vec3("dir", 1.f, 0.f, 0.f);
    s.enabled = false;
    w.registry.add(a, s);
    w.system.update(kDt);
    expectTrue(!w.system.is_attached(a), "disabled Script does not attach");
    w.registry.get<Script>(a)->enabled = true;
    w.system.update(kDt);
    expectTrue(w.system.is_attached(a) && w.counter("started") == 2.0, "enabling attaches (on_start again)");
    w.registry.get<Script>(a)->enabled = false;
    w.system.update(kDt);
    expectTrue(!w.system.is_attached(a) && w.counter("destroyed") == 2.0, "disabling detaches (on_destroy)");
    w.registry.get<Script>(a)->enabled = true;
    w.system.update(kDt);

    // Module change: old behaviour destroyed, new one started.
    (void)w.registry.get<Script>(a)->set_path("spinner");
    w.system.update(kDt);
    expectTrue(w.system.is_attached(a) && w.counter("destroyed") == 3.0 && w.counter("spinner_started") == 1.0,
               "module change re-attaches the new module");
    expectTrue(w.field(a, "spun") == 1.0, "new module ticks");

    // ScriptSystem::destroy_entity: on_destroy runs while the entity is still alive.
    const EntityID b = w.spawn("mover", 0.f, 1.0);
    w.system.update(kDt);
    const f64 aliveBefore = w.counter("destroyed_alive");
    w.system.destroy_entity(b);
    expectTrue(!w.registry.alive(b) && !w.system.is_attached(b) && w.counter("destroyed") == 4.0 &&
                   w.counter("destroyed_alive") == aliveBefore + 1.0,
               "destroy_entity runs on_destroy on a live entity, then destroys it");

    // Registry destroy behind the system's back: detached on the next tick.
    const EntityID c = w.spawn("mover", 0.f, 1.0);
    w.system.update(kDt);
    w.registry.destroy_entity(c);
    w.system.update(kDt);
    expectTrue(!w.system.is_attached(c) && !w.runtime.is_attached(c) && w.counter("destroyed") == 5.0,
               "entity destroyed in the registry: behaviour detached with on_destroy");
    expectTrue(w.system.attached_count() == 1u, "only the spinner is left");

    // Missing module: remembered as failed (no retry spam), retried when the path changes.
    const EntityID d = w.spawn("does/not/exist.lua", 0.f, 1.0);
    w.system.update(kDt);
    w.system.update(kDt);
    expectTrue(!w.system.is_attached(d) && w.system.stats().load_failures == 1u && !w.system.last_error().empty(),
               "unloadable module fails once with an error");
    (void)w.registry.get<Script>(d)->set_path("mover");
    w.system.update(kDt);
    expectTrue(w.system.is_attached(d), "fixing the path attaches");

    // Module asset id resolved through the resolver hook.
    const EntityID e = w.registry.create();
    w.registry.add(e, Transform{});
    Script byId{};
    byId.module = fuse::asset::AssetId::fromValue(0x1234u);
    w.registry.add(e, byId);
    w.system.set_module_resolver(
        [](void*, fuse::asset::AssetId id, std::string& out) {
            if (id.value != 0x1234u) {
                return false;
            }
            out = "spinner";
            return true;
        },
        nullptr);
    w.system.enterPlay(); // retries failed attaches
    expectTrue(w.system.is_attached(e), "module asset id resolved to a module path");
    expectTrue(w.runtime.error_count() == 0u || w.runtime.last_error().find("does/not/exist") != std::string::npos,
               "only the deliberate missing-module error was recorded");
}

// ---------------------------------------------------------------------------------------------
void testContactDispatch() {
    World w;
    expectTrue(w.init(), "world init");
    const EntityID a = w.spawn("mover", 0.f, 0.0);
    const EntityID other = w.registry.create();
    fuse::script::ScriptContactEvent events[2];
    events[0].kind = fuse::script::ScriptContactEvent::Kind::Collision;
    events[0].a = other;
    events[0].b = a;
    events[0].point = {0.f, 1.5f, 0.f, 0.f};
    events[1].kind = fuse::script::ScriptContactEvent::Kind::TriggerEnter;
    events[1].a = a;
    events[1].b = other;
    expectTrue(w.system.dispatch_contacts(events) == 0u, "no contact callbacks outside play");
    w.system.enterPlay();
    w.system.update(kDt);
    expectTrue(w.system.dispatch_contacts(events) == 2u, "one collision + one trigger callback on the scripted side");
    expectTrue(w.field(a, "hits") == 1.0 && w.field(a, "triggers") == 1.0 && approx(w.field(a, "last_y"), 1.5),
               "on_collision / on_trigger_enter received the contact");
    expectTrue(w.field(a, "last_other") == fuse::script::encode_entity_id(other), "other entity id passed");
}

// ---------------------------------------------------------------------------------------------
void testSaveLoadRoundTrip(const fs::path& dir) {
    const std::string scene = (dir / "scripted.fecs").string();
    EntityID ids[3];
    f32 savedX[3] = {};
    f32 savedZ1 = 0.f;
    {
        World w;
        expectTrue(w.init(), "world init");
        ids[0] = w.spawn("mover", 0.f, 1.0);
        ids[1] = w.spawn("mover", 5.f, 3.0, 0.f, 0.f, 1.f);
        ids[2] = w.spawn("spinner", 0.f, 0.0);
        (void)w.registry.get<Script>(ids[1])->set_string("label", "saved");
        const EntityID disabled = w.spawn("mover", 0.f, 1.0);
        w.registry.get<Script>(disabled)->enabled = false;
        w.system.enterPlay();
        for (int f = 0; f < 30; ++f) {
            w.system.update(kDt);
        }
        expectTrue(w.registry.get<Script>(ids[0])->lua_ref != Script::kNoRef, "live component carries lua_ref");
        for (int i = 0; i < 3; ++i) {
            savedX[i] = posX(w.registry, ids[i]);
        }
        savedZ1 = w.registry.get<Transform>(ids[1])->position.z;
        const fuse::ecs::RegistrySerialiseResult saved = fuse::ecs::RegistrySerialiser::save(w.registry, scene);
        expectTrue(saved.ok, "scene with Script components saves");
        if (!saved.ok) {
            std::fprintf(stderr, "save error: %s\n", saved.error.c_str());
        }

        // Same system: load_scene destroys the old behaviours and attaches the loaded ones.
        const f64 destroyedBefore = w.counter("destroyed");
        const fuse::ecs::RegistrySerialiseResult reloaded = w.system.load_scene(scene);
        expectTrue(reloaded.ok && w.system.attached_count() == 3u, "load_scene re-attaches the 3 enabled behaviours");
        expectTrue(w.counter("destroyed") == destroyedBefore + 2.0, "load_scene ran on_destroy for the old movers");
    }

    // Fresh process: new registry, VM, runtime and system.
    World w;
    expectTrue(w.init(), "world init");
    const fuse::ecs::RegistrySerialiseResult loaded = fuse::ecs::RegistrySerialiser::load(scene, w.registry);
    expectTrue(loaded.ok, "scene loads");
    const Script* s1 = w.registry.get<Script>(ids[1]);
    expectTrue(s1 != nullptr && s1->path() == "mover" && s1->lua_ref == Script::kNoRef && !s1->started,
               "loaded Script keeps its module, runtime state is cleared");
    expectTrue(s1 != nullptr && s1->find_property("speed") != nullptr && s1->find_property("speed")->number == 3.0 &&
                   s1->find_property("label") != nullptr && s1->find_property("label")->string_value() == "saved",
               "exposed properties survive the round trip");
    w.system.on_scene_loaded(); // not playing yet: no-op
    expectTrue(w.system.attached_count() == 0u, "nothing attaches before play");
    w.system.enterPlay();
    expectTrue(w.system.attached_count() == 3u, "loaded behaviours attach on play (disabled one stays off)");
    for (int f = 0; f < 60; ++f) {
        w.system.update(kDt);
    }
    expectTrue(approx(posX(w.registry, ids[0]), savedX[0] + 1.0, 1e-3), "re-attached mover 0 continues from its saved position");
    const Transform* t1 = w.registry.get<Transform>(ids[1]);
    expectTrue(t1 != nullptr && approx(t1->position.z, savedZ1 + 3.0, 1e-3) && approx(t1->position.x, savedX[1], 1e-5),
               "re-attached mover 1 uses its saved speed / dir properties");
    bind::ScriptValue label;
    expectTrue(w.runtime.get_instance_field(ids[1], "label", label) && bind::to_string(label) == "saved",
               "saved string property reaches self after load");
    expectTrue(w.field(ids[2], "spun") == 60.0, "spinner re-attached");

    // Corrupt Script rows are rejected on load.
    const EntityID bad = w.registry.create();
    Script corrupt{};
    (void)corrupt.set_path("mover");
    corrupt.property_count = 99u;
    w.registry.add(bad, corrupt);
    const std::string badScene = (dir / "corrupt.fecs").string();
    expectTrue(fuse::ecs::RegistrySerialiser::save(w.registry, badScene).ok, "corrupt scene still saves raw bytes");
    Registry other;
    const fuse::ecs::RegistrySerialiseResult rejected = fuse::ecs::RegistrySerialiser::load(badScene, other);
    expectTrue(!rejected.ok && rejected.error.find("corrupt Script component") != std::string::npos,
               "load rejects a Script row with a bad property table");
}

// ---------------------------------------------------------------------------------------------
void testCook(const fs::path& dir) {
    namespace script = fuse::script;
    const std::string lua = (dir / "patrol.lua").string();
    const std::string cooked = (dir / "patrol.fusescript").string();
    std::ofstream(lua) << kMover;
    const script::ScriptCookResult ok = script::cook_script_file(lua, cooked);
    expectTrue(ok.ok && ok.kind == script::CookedScriptKind::LuaBytecode && ok.byte_count > 0u,
               "Lua source cooks to bytecode");
    script::CookedScript loaded;
    expectTrue(script::load_cooked_script(cooked, loaded) && loaded.payload.size() > 4u && loaded.payload[0] == 0x1b &&
                   loaded.payload[1] == 'L',
               ".fusescript holds a Lua binary chunk (ESC 'Lua' header)");

    // A Script component pointing at the cooked file runs it.
    {
        World w;
        expectTrue(w.init(), "world init");
        const EntityID id = w.spawn(cooked.c_str(), 0.f, 1.0);
        w.system.enterPlay();
        for (int f = 0; f < 60; ++f) {
            w.system.update(kDt);
        }
        expectTrue(w.system.is_attached(id) && approx(posX(w.registry, id), 1.0, 1e-3),
                   "Script component runs a cooked .fusescript module");
    }

    // Syntax error: rejected with a clear message and nothing written.
    const std::string broken = (dir / "broken.lua").string();
    const std::string brokenOut = (dir / "broken.fusescript").string();
    std::ofstream(broken) << "function on_update(self, dt)\n  self.x = = 1\nend\n";
    const script::ScriptCookResult bad = script::cook_script_file(broken, brokenOut);
    expectTrue(!bad.ok && bad.failure == script::ScriptCookFailure::SyntaxError, "syntax error fails the cook");
    expectTrue(bad.message.find("Lua syntax error") != std::string::npos &&
                   bad.message.find("broken.lua:2:") != std::string::npos,
               "syntax error message names file and line");
    expectTrue(!fs::exists(brokenOut), "no output written for a rejected script");
    std::printf("script cook syntax error: %s\n", bad.message.c_str());

    // Legacy TorqueScript: tagged t3d: and passed through untouched.
    const std::string cs = (dir / "Game.cs").string();
    const std::string csOut = (dir / "Game.fusescript").string();
    const std::string csText = "function onStart() { echo(\"legacy\"); }\n";
    std::ofstream(cs) << csText;
    const script::ScriptCookResult legacy = script::cook_script_file(cs, csOut);
    script::CookedScript legacyLoaded;
    expectTrue(legacy.ok && legacy.kind == script::CookedScriptKind::LegacyTorqueScript &&
                   script::load_cooked_script(csOut, legacyLoaded) && legacyLoaded.chunk_name == "t3d:Game" &&
                   std::string(legacyLoaded.payload.begin(), legacyLoaded.payload.end()) == csText,
               "legacy .cs passes through as a t3d: chunk");
    {
        World w;
        expectTrue(w.init(), "world init");
        expectTrue(!w.runtime.load_module_file(csOut.c_str()).ok(), "legacy chunk is not a Lua behaviour module");
    }

    // ScriptHostService runs both cooked kinds on their routes.
    fuse::script::ScriptHostService& service = fuse::script::ScriptHostService::instance();
    const std::string hostLua = (dir / "host.lua").string();
    const std::string hostOut = (dir / "host.fusescript").string();
    std::ofstream(hostLua) << "function cooked_answer() return 6 * 7 end\n";
    expectTrue(script::cook_script_file(hostLua, hostOut).ok, "host chunk cooks");
    expectTrue(service.load_cooked(hostOut.c_str()).ok(), "ScriptHostService runs cooked Lua bytecode");
    bind::ScriptValue answer;
    expectTrue(service.host().vm().call_global("cooked_answer", nullptr, 0, &answer).ok() &&
                   bind::to_number(answer) == 42.0,
               "cooked chunk defined its global");
    const fuse::script::ScriptLoadResult routed = service.load_cooked(csOut.c_str());
    expectTrue(service.last_loaded_dialect() == fuse::script::LegacyScriptDialect::T3dTorqueScript &&
                   (routed.ok() || routed.status == fuse::script::ScriptLoadStatus::BackendUnavailable),
               "cooked legacy chunk takes the t3d: Compat route");
    service.shutdown();

    // Container integrity.
    std::vector<fuse::u8> bytes = script::encode_cooked_script(loaded);
    bytes.back() ^= 0xFFu;
    script::CookedScript tampered;
    std::string error;
    expectTrue(!script::decode_cooked_script(bytes.data(), bytes.size(), tampered, &error) &&
                   error.find("hash") != std::string::npos,
               "corrupt .fusescript payload rejected");
    expectTrue(!script::decode_cooked_script(bytes.data(), 10u, tampered, &error), "truncated .fusescript rejected");
}

// ---------------------------------------------------------------------------------------------
void testZeroSteadyStateAllocations() {
    World w;
    expectTrue(w.init(512u * 1024u), "world init with a reserved Lua heap");
    std::vector<EntityID> ids;
    for (int i = 0; i < 64; ++i) {
        ids.push_back(w.spawn(i % 4 == 0 ? "spinner" : "mover", static_cast<f32>(i), 0.5));
    }
    w.system.enterPlay();
    for (int f = 0; f < 120; ++f) { // warm-up: first ticks, GC pacing, pool classes
        w.system.update(kDt);
    }
    const unsigned long long newsBefore = g_newCount.load();
    expectTrue(newsBefore > 0u, "allocation counter is live (setup allocated)");
    const u64 poolBefore = w.vm.heap_system_allocations();
    for (int f = 0; f < 600; ++f) {
        w.system.update(kDt);
    }
    const unsigned long long news = g_newCount.load() - newsBefore;
    const u64 pool = w.vm.heap_system_allocations() - poolBefore;
    std::printf("ScriptSystem::update steady state: %llu operator new, %llu Lua pool refills over 600 frames "
                "(64 behaviours)\n",
                news, static_cast<unsigned long long>(pool));
    expectTrue(news == 0u, "zero operator new calls in ScriptSystem::update at steady state");
    expectTrue(pool == 0u, "zero Lua heap system allocations at steady state");
    expectTrue(w.system.attached_count() == 64u && w.runtime.error_count() == 0u, "64 behaviours ran cleanly");
}

} // namespace

int main() {
    {
        ScriptVM probe;
        if (!probe.init() || !probe.has_lua_backend()) {
            std::printf("fuse_script_component: SKIP (no Lua backend)\n");
            return 77;
        }
    }
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / "fuse_script_component_test";
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    testComponentBasics();
    testMovesDuringTicks();
    testAttachDetachLifecycle();
    testContactDispatch();
    testSaveLoadRoundTrip(dir);
    testCook(dir);
    testZeroSteadyStateAllocations();

    fs::remove_all(dir, ec);
    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_script_component: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("fuse_script_component: PASS\n");
    return 0;
}
