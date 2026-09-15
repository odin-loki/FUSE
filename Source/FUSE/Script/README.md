# fuse_script — B7.3 Script Host (stub)

Lua-ready script host scaffolding for Track B7.3. The null backend records loads and dispatches registered callbacks without linking Lua.

## Layout

| Header | Role |
|--------|------|
| `script_host.hpp` | Game-thread facade — VM lifecycle, callback registry, dispatch |
| `script_vm.hpp` | Null/stub VM with `load_string` / `load_file` |
| `script_bind.hpp` | Bind helpers for `EntityID` and `Transform` |
| `script_callback.hpp` | Script event kinds and callback context |
| `script_result.hpp` | Load status/results |

## Tests

`fuse_script_tests` (`ctest` name `fuse_script_b73`) covers host init, load stubs, callback register/dispatch/unregister, and bind helper round-trips without editor or renderer dependencies.
