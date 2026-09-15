#include <fuse/core/init.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/script/script_bind.hpp>
#if defined(FUSE_SCRIPT_LUA) && FUSE_SCRIPT_LUA
#include <fuse/script/script_bind_lua.hpp>
extern "C" {
#include <lauxlib.h>
#include <lua.h>
}
#endif
#include <fuse/script/script_host.hpp>
#include <fuse/types.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

int g_failures = 0;

void run_script_console_tests();

namespace {

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void expectNear(fuse::f32 actual, fuse::f32 expected, fuse::f32 epsilon, const char* message) {
    if (std::fabs(actual - expected) > epsilon) {
        std::fprintf(stderr, "FAIL: %s (got %f expected %f)\n", message, actual, expected);
        ++g_failures;
    }
}

void testHostInitializes() {
    fuse::script::ScriptHost host;
    expectTrue(host.init(), "script host initializes");
    expectTrue(host.is_initialized(), "script host reports initialized");
    expectTrue(host.vm().is_initialized(), "script VM initializes with host");
#if defined(FUSE_SCRIPT_LUA) && FUSE_SCRIPT_LUA
    expectTrue(host.vm().has_lua_backend(), "lua backend active when FUSE_SCRIPT_LUA=1");
    expectTrue(host.vm().backend_kind() == fuse::script::ScriptBackendKind::Lua,
               "backend kind is Lua when linked");
#else
    expectTrue(!host.vm().has_lua_backend(), "null backend has no lua state");
    expectTrue(host.vm().backend_kind() == fuse::script::ScriptBackendKind::Null,
               "default backend is null stub");
#endif
    host.shutdown();
    expectTrue(!host.is_initialized(), "script host shuts down");
}

void testLoadStringAndFileStubs() {
    fuse::script::ScriptHost host;
    host.init();

    const auto string_result = host.load_string("return 1", "bootstrap");
    expectTrue(string_result.ok(), "load_string succeeds");
    expectTrue(host.vm().loaded_chunk_count() == 1u, "load_string records one chunk");

    const auto missing_file = host.load_file("/tmp/fuse_script_missing_b73.lua");
    expectTrue(!missing_file.ok(), "missing file load fails");
    expectTrue(missing_file.status == fuse::script::ScriptLoadStatus::FileNotFound,
               "missing file reports FileNotFound");

    const std::filesystem::path temp_dir =
        std::filesystem::temp_directory_path() / "fuse_script_b73";
    std::filesystem::create_directories(temp_dir);
    const std::filesystem::path script_path = temp_dir / "hello.lua";
    {
        std::ofstream out(script_path);
        out << "function on_start() end\n";
    }

    const auto file_result = host.load_file(script_path.string().c_str());
    expectTrue(file_result.ok(), "existing file load succeeds");
    expectTrue(host.vm().loaded_chunk_count() == 2u, "load_file records chunk after load_string");

#if defined(FUSE_SCRIPT_LUA) && FUSE_SCRIPT_LUA
    const auto parse_error = host.load_string("function bad(", "syntax_error");
    expectTrue(!parse_error.ok(), "invalid lua source fails load");
    expectTrue(parse_error.status == fuse::script::ScriptLoadStatus::ParseError,
               "invalid lua reports ParseError");
#endif

    host.shutdown();
}

void testCallbackRegisterDispatchUnregister() {
    fuse::script::ScriptHost host;
    host.init();

    fuse::ecs::EntityID entity{4u, 1u};
    int on_start_count = 0;
    int on_update_count = 0;

    const fuse::script::ScriptCallbackId on_start_id =
        host.register_callback(fuse::script::ScriptEventKind::OnStart,
                               [&](const fuse::script::ScriptCallbackContext& ctx) {
                                   ++on_start_count;
                                   expectTrue(ctx.entity.index == entity.index, "on_start entity index");
                                   expectTrue(ctx.entity.generation == entity.generation,
                                              "on_start entity generation");
                               });

    const fuse::script::ScriptCallbackId on_update_id =
        host.register_callback(fuse::script::ScriptEventKind::OnUpdate,
                               [&](const fuse::script::ScriptCallbackContext& ctx) {
                                   ++on_update_count;
                                   expectTrue(ctx.dt == 0.016f, "on_update delta time");
                               });

    expectTrue(on_start_id != fuse::script::kInvalidScriptCallback, "on_start callback registered");
    expectTrue(on_update_id != fuse::script::kInvalidScriptCallback, "on_update callback registered");
    expectTrue(host.callback_count() == 2u, "host tracks two callbacks");
    expectTrue(host.callback_count(fuse::script::ScriptEventKind::OnStart) == 1u,
               "one on_start callback");

    fuse::script::ScriptCallbackContext ctx;
    ctx.entity = entity;
    ctx.dt = 0.016f;
    host.dispatch(fuse::script::ScriptEventKind::OnStart, ctx);
    host.dispatch(fuse::script::ScriptEventKind::OnUpdate, ctx);
    expectTrue(on_start_count == 1, "on_start dispatched once");
    expectTrue(on_update_count == 1, "on_update dispatched once");

    expectTrue(host.unregister_callback(on_start_id), "unregister on_start callback");
    expectTrue(host.callback_count() == 1u, "one callback remains after unregister");
    host.dispatch(fuse::script::ScriptEventKind::OnStart, ctx);
    expectTrue(on_start_count == 1, "unregistered on_start callback not invoked again");

    host.clear_callbacks();
    expectTrue(host.callback_count() == 0u, "clear_callbacks removes all handlers");
    host.shutdown();
}

void testOnUpdateDispatchWithDeltaTime() {
    fuse::script::ScriptHost host;
    host.init();

    fuse::ecs::EntityID entity{7u, 3u};
    std::vector<fuse::f32> received_dt;
    int dispatch_count = 0;

    host.register_callback(fuse::script::ScriptEventKind::OnUpdate,
                           [&](const fuse::script::ScriptCallbackContext& ctx) {
                               ++dispatch_count;
                               received_dt.push_back(ctx.dt);
                               expectTrue(ctx.entity == entity, "on_update entity preserved");
                           });

    host.register_callback(fuse::script::ScriptEventKind::OnUpdate,
                           [&](const fuse::script::ScriptCallbackContext& ctx) {
                               received_dt.push_back(ctx.dt);
                           });

    fuse::script::ScriptCallbackContext ctx;
    ctx.entity = entity;

    ctx.dt = 0.016f;
    host.dispatch(fuse::script::ScriptEventKind::OnUpdate, ctx);
    ctx.dt = 0.033f;
    host.dispatch(fuse::script::ScriptEventKind::OnUpdate, ctx);
    ctx.dt = 0.008f;
    host.dispatch(fuse::script::ScriptEventKind::OnUpdate, ctx);

    expectTrue(dispatch_count == 3, "primary on_update handler invoked each frame");
    expectTrue(received_dt.size() == 6u, "both on_update handlers receive each dispatch");
    expectTrue(received_dt[0] == 0.016f && received_dt[1] == 0.016f, "frame 1 dt propagated");
    expectTrue(received_dt[2] == 0.033f && received_dt[3] == 0.033f, "frame 2 dt propagated");
    expectTrue(received_dt[4] == 0.008f && received_dt[5] == 0.008f, "frame 3 dt propagated");

    fuse::f32 accumulated = 0.f;
    for (const fuse::f32 dt : received_dt) {
        accumulated += dt;
    }
    expectNear(accumulated, 0.114f, 1e-5f, "on_update handlers observe summed frame dt");

    host.shutdown();
}

void testOnUpdateDispatchEdgeCases() {
    fuse::script::ScriptHost host;
    host.init();

    fuse::ecs::EntityID entity{11u, 4u};
    int update_count = 0;
    fuse::f32 last_dt = -1.f;

    host.register_callback(fuse::script::ScriptEventKind::OnUpdate,
                           [&](const fuse::script::ScriptCallbackContext& ctx) {
                               ++update_count;
                               last_dt = ctx.dt;
                               expectTrue(ctx.entity == entity, "dispatch_update entity");
                           });

    host.dispatch_update(0.f, entity);
    expectTrue(update_count == 1, "zero dt still dispatches on_update");
    expectTrue(last_dt == 0.f, "zero dt preserved");

    host.dispatch_update(0.05f, entity);
    expectTrue(update_count == 2, "dispatch_update increments handler count");
    expectNear(last_dt, 0.05f, 1e-6f, "dispatch_update dt");

    int on_start_count = 0;
    host.register_callback(fuse::script::ScriptEventKind::OnStart,
                           [&](const fuse::script::ScriptCallbackContext& ctx) {
                               ++on_start_count;
                               expectTrue(ctx.entity == entity, "on_start entity preserved");
                           });

    fuse::script::ScriptCallbackContext start_ctx;
    start_ctx.entity = entity;
    start_ctx.dt = 99.f;
    host.dispatch(fuse::script::ScriptEventKind::OnStart, start_ctx);
    expectTrue(on_start_count == 1, "on_start dispatched once");
    expectTrue(update_count == 2, "on_start dispatch does not invoke on_update");

    host.dispatch_update(0.01f, entity);
    expectTrue(on_start_count == 1, "dispatch_update does not invoke on_start");
    expectTrue(update_count == 3, "dispatch_update still reaches on_update handlers");

    fuse::script::ScriptHost uninitialized_host;
    uninitialized_host.dispatch_update(0.016f, entity);
    expectTrue(uninitialized_host.callback_count() == 0u,
               "dispatch_update on uninitialized host is no-op");

    host.shutdown();
}

void testBindHelpersValuesEqual() {
    expectTrue(fuse::script::bind::values_equal(fuse::script::bind::push_nil(),
                                                fuse::script::bind::push_nil()),
               "nil values equal");
    expectTrue(!fuse::script::bind::values_equal(fuse::script::bind::push_nil(),
                                                 fuse::script::bind::push_bool(false)),
               "nil and bool differ");
    expectTrue(fuse::script::bind::values_equal(fuse::script::bind::push_number(2.0),
                                                fuse::script::bind::push_number(2.0)),
               "number values equal");
    expectTrue(fuse::script::bind::values_equal(fuse::script::bind::push_string("abc"),
                                                fuse::script::bind::push_string("abc")),
               "string values equal");
}

void testBindHelpersPrimitives() {
    const fuse::script::bind::ScriptValue nil_value = fuse::script::bind::push_nil();
    expectTrue(fuse::script::bind::is_nil(nil_value), "nil value tagged");
    expectTrue(fuse::script::bind::kind_name(nil_value.kind) == std::string("nil"),
               "nil kind name");

    const fuse::script::bind::ScriptValue bool_value = fuse::script::bind::push_bool(true);
    expectTrue(fuse::script::bind::is_bool(bool_value), "bool value tagged");
    expectTrue(fuse::script::bind::to_bool(bool_value), "bool round-trips");
    expectTrue(fuse::script::bind::to_bool(fuse::script::bind::push_nil(), true),
               "nil coerces to default bool");

    const fuse::script::bind::ScriptValue number_value = fuse::script::bind::push_number(3.5);
    expectTrue(fuse::script::bind::is_number(number_value), "number value tagged");
    expectTrue(fuse::script::bind::to_number(number_value) == 3.5, "number round-trips");
    expectTrue(fuse::script::bind::to_number(fuse::script::bind::push_nil(), 9.0) == 9.0,
               "nil coerces to default number");

    const fuse::script::bind::ScriptValue string_value =
        fuse::script::bind::push_string("fuse_script");
    expectTrue(fuse::script::bind::is_string(string_value), "string value tagged");
    expectTrue(fuse::script::bind::to_string(string_value) == "fuse_script", "string round-trips");
    expectTrue(fuse::script::bind::to_string(fuse::script::bind::push_nil()).empty(),
               "nil coerces to empty string");
}

void testBindHelpersEntityAndTransform() {
    const fuse::ecs::EntityID entity{9u, 2u};
    const fuse::script::bind::ScriptValue entity_value =
        fuse::script::bind::push_entity_id(entity);
    expectTrue(fuse::script::bind::is_entity_id(entity_value), "entity value tagged");
    const fuse::ecs::EntityID round_trip = fuse::script::bind::to_entity_id(entity_value);
    expectTrue(round_trip == entity, "entity id round-trips through bind helper");
    expectTrue(fuse::script::bind::kind_name(entity_value.kind) == std::string("entity_id"),
               "entity kind name");

    fuse::ecs::Transform transform;
    transform.position = {1.f, 2.f, 3.f, 1.f};
    transform.scale = {2.f, 2.f, 2.f, 0.f};
    transform.dirty = true;

    const fuse::script::bind::ScriptValue transform_value =
        fuse::script::bind::push_transform(transform);
    expectTrue(fuse::script::bind::is_transform(transform_value), "transform value tagged");
    const fuse::ecs::Transform restored = fuse::script::bind::to_transform(transform_value);
    expectTrue(restored.position.x == 1.f && restored.position.y == 2.f && restored.position.z == 3.f,
               "transform position round-trips");
    expectTrue(restored.scale.x == 2.f, "transform scale round-trips");
    expectTrue(restored.dirty, "transform dirty flag preserved");
}

#if defined(FUSE_SCRIPT_LUA) && FUSE_SCRIPT_LUA
void testLuaLoadsHelloWorld() {
    fuse::script::ScriptHost host;
    host.init();

    const auto result = host.load_string("print('fuse_script_lua')", "hello");
    expectTrue(result.ok(), "lua hello-world chunk loads and runs");
    expectTrue(host.vm().loaded_chunk_count() == 1u, "lua chunk recorded");

    host.shutdown();
}

void testLuaBindStackRoundTrip() {
    lua_State* L = luaL_newstate();
    expectTrue(L != nullptr, "lua state for bind round-trip");

    const fuse::script::bind::ScriptValue samples[] = {
        fuse::script::bind::push_nil(),
        fuse::script::bind::push_bool(true),
        fuse::script::bind::push_number(42.5),
        fuse::script::bind::push_string("fuse"),
        fuse::script::bind::push_entity_id(fuse::ecs::EntityID{3u, 1u}),
    };

    fuse::ecs::Transform transform;
    transform.position = {4.f, 5.f, 6.f, 1.f};
    transform.rotation = {0.f, 0.707f, 0.f, 0.707f};
    transform.scale = {1.f, 2.f, 3.f, 0.f};
    transform.dirty = true;
    transform.parent = fuse::ecs::EntityID{8u, 2u};

    for (const fuse::script::bind::ScriptValue& sample : samples) {
        fuse::script::bind::lua::push_to_stack(L, sample);
        const fuse::script::bind::ScriptValue round_trip =
            fuse::script::bind::lua::read_from_stack(L, -1);
        expectTrue(fuse::script::bind::values_equal(sample, round_trip),
                   "lua stack round-trip preserves tagged value");
        lua_pop(L, 1);
    }

    const fuse::script::bind::ScriptValue transform_value =
        fuse::script::bind::push_transform(transform);
    fuse::script::bind::lua::push_to_stack(L, transform_value);
    const fuse::script::bind::ScriptValue restored =
        fuse::script::bind::lua::read_from_stack(L, -1);
    expectTrue(fuse::script::bind::values_equal(transform_value, restored),
               "transform round-trips through lua stack");
    lua_pop(L, 1);

    lua_close(L);
}
#endif

} // namespace

int main() {
    fuse::core::initialize();

    testHostInitializes();
    testLoadStringAndFileStubs();
    testCallbackRegisterDispatchUnregister();
    testOnUpdateDispatchWithDeltaTime();
    testOnUpdateDispatchEdgeCases();
    testBindHelpersPrimitives();
    testBindHelpersValuesEqual();
    testBindHelpersEntityAndTransform();
    run_script_console_tests();
#if defined(FUSE_SCRIPT_LUA) && FUSE_SCRIPT_LUA
    testLuaLoadsHelloWorld();
    testLuaBindStackRoundTrip();
#endif

    if (g_failures != 0) {
        std::fprintf(stderr, "%d test failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }

    std::fprintf(stdout, "fuse_script_b73: all tests passed\n");
    return EXIT_SUCCESS;
}
