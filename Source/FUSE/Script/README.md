# fuse_script — B7.3 Scripting Layer

Lua scripting for Track B7.3. CMake links a system Lua >= 5.2 when found, otherwise builds the
in-tree Lua 5.2.3 sources (`FUSE_SCRIPT_BUNDLED_LUA`, default ON) and defines `FUSE_SCRIPT_LUA=1`;
with neither, `ScriptVM` falls back to a null backend that only records chunk names.

Every Lua entry point runs inside `ScriptVM::run_protected` (`lua_pcall` + traceback): script
errors, runaway loops (`ScriptVMDesc::instruction_budget`) and heap exhaustion
(`ScriptVMDesc::memory_limit_bytes`) come back as `ScriptLoadStatus::RuntimeError` and never take
the engine down. VMs are sandboxed by default (no io/os.execute/package/debug/load*/string.dump).

## Layout

| Header | Role |
|--------|------|
| `script_host.hpp` | Game-thread facade — VM lifecycle, callback registry, `dispatch` / `dispatch_update` / `tick_update_scripts` |
| `script_host_service.hpp` | Singleton toward U3+ single process-wide host — `load_chunk` with legacy prefix routing |
| `legacy_script_route.hpp` | `uaisk:` / `t3d:` / `t2d:` / `fuse:` chunk-name dialect parsing |
| `script_update.hpp` | Per-script OnUpdate registry — dt accumulation, enable/disable, error isolation |
| `script_vm.hpp` | Null or Lua VM: `load_string` / `load_file` / `call_global` / `run_protected`, sandbox, instruction budget, memory cap, captured `print` |
| `script_engine_api.hpp` | `bind_engine_api` — `Entity.*` (ECS registry) and `Physics.*` (`ScriptPhysicsBackend`) tables |
| `script_runtime.hpp` | Per-entity Lua behaviours: `on_start` / `on_update(dt)` / `on_collision` / `on_trigger_enter` / `on_destroy`, in-place hot reload |
| `script_hot_reload.hpp` | File/directory watcher that reloads changed modules into a `ScriptRuntime` |
| `script_physics_bridge.hpp` | `fuse_script_physics`: `PhysicsManagerScriptBackend` + `dispatch_physics_events` on the FUSE `PhysicsManager` |
| `script_console.hpp` | Headless REPL — built-in command stubs, history buffer, dispatch to `ScriptHost` |
| `script_console_command_registry.hpp` | Named built-in/custom command registry with `lookup_kind`, prefix match, `longest_common_prefix`, `unique_prefix_match`, `suggest_commands`, name validation, shadowing + sorted `help`/`list` |
| `script_console_history.hpp` | Fixed-capacity history ring buffer with recall navigation, `newest`/`oldest`, `is_empty`/`is_valid_index`, and `clear` |
| `script_bind.hpp` | Tagged `ScriptValue` helpers (primitives + ECS types) + `values_equal` + `PropertyStore` / `MethodTable` |
| `script_bind_lua.hpp` | Lua stack push/pop when `FUSE_SCRIPT_LUA=1` |
| `script_callback.hpp` | Script event kinds and callback context |
| `script_result.hpp` | Load status/results |

## Tests

`fuse_script_b7_gate_tests` (`ctest -R fuse_script_b7_gates`) proves the B7.3 / B7.10 scripting rows
against independent references (hello world, Entity position round-trip, `Physics.ray_cast` vs C++
and analytic ray/sphere, 60-frame dt accumulation, first-contact `on_collision`, hot reload within
one frame, error isolation, sandbox, instruction budget, memory cap/GC, coroutines).
`fuse_script_b7_perf_tests` (`fuse_script_b7_perf`, labels `perf;gate`, RUN_SERIAL) enforces call
overhead budgets in optimised builds.

`fuse_script_tests` (`ctest` name `fuse_script_b73`) covers host init, load stubs (and Lua parse errors when linked), callback register/dispatch/unregister, multi-frame and edge-case `OnUpdate` dt propagation, per-script tick order/disable/error isolation, `PropertyStore` and `MethodTable` bind stubs, primitive and ECS bind round-trips, `values_equal`, Lua stack round-trip when linked, and `ScriptConsole` command dispatch with `describe`/`complete`/`suggest`/`repeat` lookup stubs, prefix/LCP/unique-match completion helpers, unknown-command suggestions, history guard accessors, and history buffer navigation — without editor or renderer dependencies.
