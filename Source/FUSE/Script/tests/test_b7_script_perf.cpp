// B7.3 script call-overhead budgets (perf gate; budgets enforced in optimised builds only):
//  - engine -> script: per-entity on_update dispatch  < 2 us per callback (1000 entities)
//  - script -> engine: Entity.get_position + set_position pair < 1 us per pair
#include <fuse/core/init.hpp>
#include <fuse/core/sanitizer.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/registry.hpp>
#include <fuse/script/script_budget_us.hpp>
#include <fuse/script/script_engine_api.hpp>
#include <fuse/script/script_indent.hpp>
#include <fuse/script/script_runtime.hpp>
#include <fuse/script/script_vm.hpp>
#include <fuse/script/time_budget.hpp>
#include <fuse/types.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits>
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

// Indent spaces are the closed range 0..32. No allocation.
void testScriptIndentOk() {
    expectTrue(fuse::script::script_indent_ok(0), "script_indent_ok(0) is true");
    expectTrue(fuse::script::script_indent_ok(32), "script_indent_ok(32) is true");
    expectTrue(!fuse::script::script_indent_ok(-1), "script_indent_ok(-1) is false");
    expectTrue(!fuse::script::script_indent_ok(33), "script_indent_ok(33) is false");
}

void testScriptBudgetUsOk() {
    expectTrue(fuse::script::script_budget_us_ok(1), "1 us budget is accepted");
    expectTrue(fuse::script::script_budget_us_ok(100000), "100000 us budget is accepted");
    expectTrue(!fuse::script::script_budget_us_ok(0), "0 us budget is rejected");
    expectTrue(!fuse::script::script_budget_us_ok(100001), "100001 us budget is rejected");
}

void testOverBudget() {
    expectTrue(fuse::script::over_budget(2.5f, 2.f), "2.5 ms exceeds a 2 ms budget");
    expectTrue(!fuse::script::over_budget(2.f, 2.f), "2 ms meets a 2 ms budget");
    expectTrue(!fuse::script::over_budget(0.f, 1.f), "0 ms meets a 1 ms budget");
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    expectTrue(fuse::script::over_budget(nan, 1.f), "non-finite milliseconds is over budget");
    expectTrue(fuse::script::over_budget(inf, 1.f), "infinite milliseconds is over budget");
    expectTrue(fuse::script::over_budget(-inf, 1.f), "negative infinite milliseconds is over budget");
    expectTrue(fuse::script::over_budget(1.f, 0.f), "positive time with a zero budget is over budget");
    expectTrue(fuse::script::over_budget(1.f, -2.f), "positive time with a negative budget is over budget");
    expectTrue(!fuse::script::over_budget(0.f, 0.f), "zero time with a non-positive budget is within budget");
    expectTrue(!fuse::script::over_budget(-1.f, -2.f), "negative time with a non-positive budget is within budget");
    expectTrue(fuse::script::over_budget(1.f, nan), "positive time with a non-finite budget is over budget");
    expectTrue(fuse::script::over_budget(1.f, inf), "positive time with an infinite budget is over budget");
    expectTrue(!fuse::script::over_budget(0.f, nan), "zero time with a non-finite budget is within budget");
    expectTrue(!fuse::script::over_budget(-1.f, inf), "negative time with a non-finite budget is within budget");
    expectTrue(!fuse::script::over_budget(-0.5f, 1.f), "negative time under a positive budget is within budget");
}

} // namespace

int main() {
    testScriptIndentOk();
    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_script_b7_perf: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    testScriptBudgetUsOk();
    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_script_b7_perf: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    testOverBudget();
    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_script_b7_perf: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
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
