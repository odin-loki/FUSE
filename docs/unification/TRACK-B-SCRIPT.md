# Track B — Script Host (B7.3 deepen)

**Status:** B7.3 deepen — bind primitives, OnUpdate dt dispatch, optional Lua backend landed  
**Master plan:** [FUSE_MASTER_PLAN.md](../plans/FUSE_MASTER_PLAN.md) §B7.3  
**Threading:** Game-thread facade; hot-reload and ECS `Script` component deferred

---

## Scope

| Component | Location | Notes |
|-----------|----------|-------|
| `ScriptHost` | `Source/FUSE/Script/include/fuse/script/script_host.hpp` | Init/shutdown, callback registry, dispatch |
| `ScriptVM` | `Source/FUSE/Script/include/fuse/script/script_vm.hpp` | Null backend or optional Lua compile/run |
| Bind helpers | `Source/FUSE/Script/include/fuse/script/script_bind.hpp` | Tagged `ScriptValue` carriers for nil/bool/number/string + ECS types |
| Callbacks | `Source/FUSE/Script/include/fuse/script/script_callback.hpp` | `OnStart`, `OnUpdate`, `OnDestroy`, collision/trigger hooks |

**Not in scope:** ECS `Script` component, hot-reload watcher, physics/input API bindings, per-entity `lua_ref`.

---

## Design

### Backend selection

`ScriptVM` defaults to `ScriptBackendKind::Null` when Lua is unavailable. When system Lua is found (`FUSE_SCRIPT_ENABLE_LUA=ON`), CMake defines `FUSE_SCRIPT_LUA=1` and the VM:

1. Creates a `lua_State` and opens standard libraries.
2. Reports `ScriptBackendKind::Lua` and `has_lua_backend() == true`.
3. Compiles and executes chunks via `load_string` / `load_file`, returning `ParseError` on syntax/runtime failures.

Without Lua, loads are still accepted when the source/path is valid and recorded for tests; no bytecode is executed.

### Callback registry

`ScriptHost::register_callback` returns a `ScriptCallbackId`. Handlers run in registration order via `dispatch(event, ctx)`. `OnUpdate` handlers receive `ScriptCallbackContext::dt` each frame. `unregister_callback` and `clear_callbacks` remove entries without leaking raw function pointers in the public API.

### Bind helpers

`fuse::script::bind::ScriptValue` is a tagged POD-like carrier for script-facing values until a real Lua stack lands:

| Kind | Helpers |
|------|---------|
| `Nil` | `push_nil`, `is_nil` |
| `Bool` | `push_bool`, `to_bool`, `is_bool` |
| `Number` | `push_number`, `to_number`, `is_number` |
| `String` | `push_string`, `to_string`, `is_string` |
| `EntityId` | `push_entity_id`, `to_entity_id`, `is_entity_id` |
| `Transform` | `push_transform`, `to_transform`, `is_transform` |

`kind_name` returns a stable diagnostic label. Coercion helpers return defaults when the tag does not match.

---

## Build

`fuse_script` builds when `FUSE_BUILD_SCRIPT=ON` (default).

```bash
cmake -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DFUSE_UMBRELLA=ON \
  -DFUSE_BUILD_CORE=ON \
  -DFUSE_BUILD_CORE_TESTS=ON \
  -DFUSE_BUILD_SCRIPT=ON

cmake --build build
ctest --test-dir build --output-on-failure -R fuse_script
```

| Condition | Behaviour |
|-----------|-----------|
| `FUSE_BUILD_SCRIPT=OFF` | Script module omitted from umbrella |
| `FUSE_SCRIPT_ENABLE_LUA=ON` + system Lua | `FUSE_SCRIPT_LUA=1`; VM uses Lua compile/run backend |
| `FUSE_SCRIPT_ENABLE_LUA=OFF` or Lua missing | Null backend; loads recorded only |
| `FUSE_BUILD_CORE_TESTS=ON` | `fuse_script_b73` CTest target |

---

## Tests

| Target | Validates |
|--------|-----------|
| `fuse_script_b73` | Host init (null or Lua backend), load stubs / parse errors, callback register/dispatch/unregister, multi-frame `OnUpdate` dt propagation, primitive + ECS bind round-trips, Lua hello-world when linked |

Run:

```bash
ctest --test-dir build --output-on-failure -R fuse_script
```

---

## Gates (B7.3 deepen)

- [x] `ScriptHost` / `ScriptVM` on FUSE APIs (null + optional Lua backend)
- [x] Register/unregister script callbacks
- [x] `OnUpdate` dispatch passes per-frame `dt` to all handlers
- [x] `load_string` / `load_file` with parse-error reporting when Lua linked
- [x] Bind helpers for nil/bool/number/string + `EntityID` / `Transform`
- [x] CTest target green in umbrella CI
- [ ] ECS `Script` component + per-entity `lua_ref` (follow-up)
- [ ] Hot-reload watcher (follow-up)

---

## Next

- [ ] `ecs/components/script.hpp` + per-entity `lua_ref`
- [ ] `ScriptHotReload` file watcher (master plan sketch)
- [ ] Engine API surface (`Entity.*`, `Physics.*`, `Input.*`) as Lua bindings
- [ ] Bridge `ScriptValue` helpers to real Lua stack push/pop

---

## Related docs

- [FUSE_MASTER_PLAN.md](../plans/FUSE_MASTER_PLAN.md) §B7.3
- [TRACK-B-ECS.md](./TRACK-B-ECS.md) — `EntityID` / `Transform` types consumed by bind helpers
