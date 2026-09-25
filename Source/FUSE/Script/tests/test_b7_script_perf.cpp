// B7.3 script call-overhead budgets (perf gate; budgets enforced in optimised builds only):
//  - engine -> script: per-entity on_update dispatch  < 2 us per callback (1000 entities)
//  - script -> engine: Entity.get_position + set_position pair < 1 us per pair
#include <fuse/core/init.hpp>
#include <fuse/core/sanitizer.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/registry.hpp>
#include <fuse/script/script_engine_api.hpp>
#include <fuse/script/script_runtime.hpp>
#include <fuse/script/script_vm.hpp>
#include <fuse/types.hpp>

#include <algorithm>
#include <chrono>
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

#if defined(NDEBUG)
constexpr bool kEnforceBudgets = true;
#else
constexpr bool kEnforceBudgets = false;
#endif

constexpr double kUpdateBudgetUs = 2.0;
constexpr double kApiPairBudgetUs = 1.0;

double medianOf(std::vector<double> samples) {
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
}

#if defined(FUSE_SCRIPT_LUA) && FUSE_SCRIPT_LUA
void testUpdateDispatchOverhead() {
    fuse::ecs::Registry reg;
    reg.init(4096);
    fuse::script::ScriptVM vm;
    vm.init();
    fuse::script::ScriptRuntime runtime;
    fuse::script::ScriptEngineBindings bindings;
    bindings.registry = &reg;
    expectTrue(runtime.init(vm, bindings), "runtime initialises");
    runtime.load_module_source("tick", "function on_update(self, dt) self.t = (self.t or 0) + dt end");
    constexpr int kEntities = 1000;
    for (int i = 0; i < kEntities; ++i) {
        const fuse::ecs::EntityID e = reg.create();
        runtime.attach(e, "tick");
    }
    runtime.update(1.f / 60.f); // on_start + warm-up

    std::vector<double> perCallUs;
    for (int round = 0; round < 15; ++round) {
        constexpr int kFrames = 20;
        const auto start = std::chrono::steady_clock::now();
        for (int f = 0; f < kFrames; ++f) {
            runtime.update(1.f / 60.f);
        }
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
        perCallUs.push_back(us / (kFrames * kEntities));
    }
    const double median = medianOf(perCallUs);
    std::printf("on_update dispatch: %.3f us per callback (median of 15 x 20 frames x %d entities), budget %.1f us%s\n",
                median, kEntities, kUpdateBudgetUs, kEnforceBudgets ? "" : " [not enforced: debug build]");
    expectTrue(runtime.error_count() == 0, "no script errors");
    if (kEnforceBudgets && fuse::core::timingBudgetsEnforcedNoted()) {
        expectTrue(median < kUpdateBudgetUs, "on_update dispatch within budget");
    }
}

void testEngineApiOverhead() {
    fuse::ecs::Registry reg;
    reg.init(64);
    fuse::script::ScriptVM vm;
    vm.init();
    fuse::script::ScriptEngineBindings bindings;
    bindings.registry = &reg;
    fuse::script::bind_engine_api(vm, bindings);
    const fuse::ecs::EntityID e = reg.create();
    reg.add<fuse::ecs::Transform>(e);
    const std::string setup = "id = " + std::to_string(fuse::script::encode_entity_id(e)) + R"lua(
        function bench(n)
            local get, set = Entity.get_position, Entity.set_position
            for i = 1, n do
                local p = get(id)
                p.x = p.x + 1
                set(id, p)
            end
        end
    )lua";
    expectTrue(vm.load_string(setup.c_str(), "bench").ok(), "bench script loads");
    const fuse::script::bind::ScriptValue warm = fuse::script::bind::push_number(1000);
    vm.call_global("bench", &warm, 1);

    constexpr int kCalls = 100000;
    std::vector<double> perPairUs;
    for (int round = 0; round < 9; ++round) {
        const fuse::script::bind::ScriptValue n = fuse::script::bind::push_number(kCalls);
        const auto start = std::chrono::steady_clock::now();
        vm.call_global("bench", &n, 1);
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
        perPairUs.push_back(us / kCalls);
    }
    const double median = medianOf(perPairUs);
    const float x = reg.get<fuse::ecs::Transform>(e)->position.x;
    std::printf("Entity get+set from Lua: %.3f us per pair (median of 9 x %d), budget %.1f us%s\n", median, kCalls,
                kApiPairBudgetUs, kEnforceBudgets ? "" : " [not enforced: debug build]");
    expectTrue(x == 1000.f + 9.f * kCalls, "every scripted write reached the Transform");
    if (kEnforceBudgets && fuse::core::timingBudgetsEnforcedNoted()) {
        expectTrue(median < kApiPairBudgetUs, "script -> engine API call within budget");
    }
}
#endif

} // namespace

int main() {
    fuse::core::initialize();
#if defined(FUSE_SCRIPT_LUA) && FUSE_SCRIPT_LUA
    testUpdateDispatchOverhead();
    testEngineApiOverhead();
#else
    std::fprintf(stderr, "FAIL: fuse_script built without a Lua backend\n");
    ++g_failures;
#endif
    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_script_b7_perf: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("fuse_script_b7_perf: all budgets met\n");
    return EXIT_SUCCESS;
}
