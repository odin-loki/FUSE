# fuse_script — B7.3 Script Host (deepen follow-up)

Lua-ready script host for Track B7.3. Uses a null backend when Lua is unavailable; when `FUSE_SCRIPT_LUA=1`, `ScriptVM` compiles and runs chunks via system Lua and `script_bind_lua.hpp` bridges tagged values to the Lua stack.

## Layout

| Header | Role |
|--------|------|
| `script_host.hpp` | Game-thread facade — VM lifecycle, callback registry, `dispatch` / `dispatch_update` / `tick_update_scripts` |
| `script_update.hpp` | Per-script OnUpdate registry — dt accumulation, enable/disable, error isolation |
| `script_vm.hpp` | Null or Lua VM with `load_string` / `load_file` |
| `script_console.hpp` | Headless REPL — built-in command stubs, history buffer, dispatch to `ScriptHost` |
| `script_console_command_registry.hpp` | Named built-in/custom command registry with `lookup_kind`, prefix match, `longest_common_prefix`, `unique_prefix_match`, `suggest_commands`, name validation, shadowing + sorted `help`/`list` |
| `script_console_history.hpp` | Fixed-capacity history ring buffer with recall navigation, `newest`/`oldest`, `is_empty`/`is_valid_index`, and `clear` |
| `script_bind.hpp` | Tagged `ScriptValue` helpers (primitives + ECS types) + `values_equal` + `PropertyStore` / `MethodTable` |
| `script_bind_lua.hpp` | Lua stack push/pop when `FUSE_SCRIPT_LUA=1` |
| `script_callback.hpp` | Script event kinds and callback context |
| `script_result.hpp` | Load status/results |

## Tests

`fuse_script_tests` (`ctest` name `fuse_script_b73`) covers host init, load stubs (and Lua parse errors when linked), callback register/dispatch/unregister, multi-frame and edge-case `OnUpdate` dt propagation, per-script tick order/disable/error isolation, `PropertyStore` and `MethodTable` bind stubs, primitive and ECS bind round-trips, `values_equal`, Lua stack round-trip when linked, and `ScriptConsole` command dispatch with `describe`/`complete`/`suggest`/`repeat` lookup stubs, prefix/LCP/unique-match completion helpers, unknown-command suggestions, history guard accessors, and history buffer navigation — without editor or renderer dependencies.
