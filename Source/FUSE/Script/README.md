# fuse_script — B7.3 Script Host (deepen follow-up)

Lua-ready script host for Track B7.3. Uses a null backend when Lua is unavailable; when `FUSE_SCRIPT_LUA=1`, `ScriptVM` compiles and runs chunks via system Lua and `script_bind_lua.hpp` bridges tagged values to the Lua stack.

## Layout

| Header | Role |
|--------|------|
| `script_host.hpp` | Game-thread facade — VM lifecycle, callback registry, `dispatch` / `dispatch_update` |
| `script_vm.hpp` | Null or Lua VM with `load_string` / `load_file` |
| `script_bind.hpp` | Tagged `ScriptValue` helpers (primitives + ECS types) + `values_equal` |
| `script_bind_lua.hpp` | Lua stack push/pop when `FUSE_SCRIPT_LUA=1` |
| `script_callback.hpp` | Script event kinds and callback context |
| `script_result.hpp` | Load status/results |

## Tests

`fuse_script_tests` (`ctest` name `fuse_script_b73`) covers host init, load stubs (and Lua parse errors when linked), callback register/dispatch/unregister, multi-frame and edge-case `OnUpdate` dt propagation, primitive and ECS bind round-trips, `values_equal`, and Lua stack round-trip when linked — without editor or renderer dependencies.
