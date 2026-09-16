# Scripting

Gameplay scripting is a **game-thread** host with a Lua-ready VM. Hot-reload and a per-entity ECS `Script` component are not in this milestone.

## Components

| Type | Header | Role |
|------|--------|------|
| `ScriptHost` | `fuse/script/script_host.hpp` | Lifecycle, callbacks, load, tick |
| `ScriptVM` | `fuse/script/script_vm.hpp` | Null backend or optional Lua |
| `ScriptConsole` | `fuse/script/script_console.hpp` | Headless REPL, history, prefix completion |
| Bind helpers | `fuse/script/script_bind.hpp` | `ScriptValue`, property store, method table |

Library: `fuse_script` (`FUSE_BUILD_SCRIPT=ON`).

## Callbacks

Register C++ handlers for `OnStart`, `OnUpdate`, `OnDestroy`, plus collision / trigger hooks:

```cpp
#include <fuse/script/script_host.hpp>

fuse::script::ScriptHost host;
host.init();

host.register_callback(fuse::script::ScriptEventKind::OnUpdate,
    [](const fuse::script::ScriptCallbackContext& ctx) {
        (void)ctx;
    });

host.tick_update_scripts(dt);
host.shutdown();
```

`tick_update_scripts` isolates errors per registration so one failing script does not kill the rest.

## VM backends

`ScriptVM` defaults to `ScriptBackendKind::Null` when Lua is not found. Loads are still accepted and recorded for tests; no bytecode runs.

When system Lua is available and `FUSE_SCRIPT_ENABLE_LUA=ON`, CMake defines `FUSE_SCRIPT_LUA=1`. The VM then:

1. Creates a `lua_State` and opens standard libraries
2. Reports `has_lua_backend() == true`
3. Compiles `load_string` / `load_file` chunks (`ParseError` on failure)

Lua stack helpers live in `fuse/script/script_bind_lua.hpp`.

## Console

`ScriptConsole` is a headless REPL for tests and a future editor panel: built-in command stubs, a fixed-capacity history ring, and dispatch by name. Qt console chrome is not wired yet.

## Rules

- Script that mutates the scene runs on the game thread.
- Do not call the host from job workers.
- Prefer C++23 for new systems. Script is glue, not the kernel.
