#include <fuse/core/init.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/script/script_bind.hpp>
#include <fuse/script/script_host.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void testHostInitializes() {
    fuse::script::ScriptHost host;
    expectTrue(host.init(), "script host initializes");
    expectTrue(host.is_initialized(), "script host reports initialized");
    expectTrue(host.vm().is_initialized(), "script VM initializes with host");
    expectTrue(host.vm().backend_kind() == fuse::script::ScriptBackendKind::Null,
               "default backend is null stub");
    host.shutdown();
    expectTrue(!host.is_initialized(), "script host shuts down");
}

void testLoadStringAndFileStubs() {
    fuse::script::ScriptHost host;
    host.init();

    const auto string_result = host.load_string("return 1", "bootstrap");
    expectTrue(string_result.ok(), "load_string succeeds on null backend");
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

void testBindHelpersEntityAndTransform() {
    const fuse::ecs::EntityID entity{9u, 2u};
    const fuse::script::bind::ScriptValue entity_value =
        fuse::script::bind::push_entity_id(entity);
    expectTrue(fuse::script::bind::is_entity_id(entity_value), "entity value tagged");
    const fuse::ecs::EntityID round_trip = fuse::script::bind::to_entity_id(entity_value);
    expectTrue(round_trip == entity, "entity id round-trips through bind helper");

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

} // namespace

int main() {
    fuse::core::initialize();

    testHostInitializes();
    testLoadStringAndFileStubs();
    testCallbackRegisterDispatchUnregister();
    testBindHelpersEntityAndTransform();

    if (g_failures != 0) {
        std::fprintf(stderr, "%d test failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }

    std::fprintf(stdout, "fuse_script_b73: all tests passed\n");
    return EXIT_SUCCESS;
}
