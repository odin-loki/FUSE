# fuse_script — B7.3 Script Host (deepen)

Lua-ready script host for Track B7.3. Uses a null backend when Lua is unavailable; when `FUSE_SCRIPT_LUA=1`, `ScriptVM` compiles and runs chunks via system Lua.

## Layout

| Header | Role |
|--------|------|
| `script_host.hpp` | Game-thread facade — VM lifecycle, callback registry, dispatch |
| `script_vm.hpp` | Null or Lua VM with `load_string` / `load_file` |
| `script_bind.hpp` | Tagged `ScriptValue` helpers (primitives + ECS types) |
| `script_callback.hpp` | Script event kinds and callback context |
| `script_result.hpp` | Load status/results |

## Tests

`fuse_script_tests` (`ctest` name `fuse_script_b73`) covers host init, load stubs (and Lua parse errors when linked), callback register/dispatch/unregister, multi-frame `OnUpdate` dt propagation, primitive and ECS bind round-trips without editor or renderer dependencies.
