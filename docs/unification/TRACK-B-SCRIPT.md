# Track B — Script Host (B7.3 deepen follow-up)

**Status:** B7.3 deepen follow-up — Lua stack bridge, `dispatch_update`, bind equality landed  
**Master plan:** [FUSE_MASTER_PLAN.md](../plans/FUSE_MASTER_PLAN.md) §B7.3  
**Threading:** Game-thread facade; hot-reload and ECS `Script` component deferred

---

## Scope

| Component | Location | Notes |
|-----------|----------|-------|
| `ScriptHost` | `Source/FUSE/Script/include/fuse/script/script_host.hpp` | Init/shutdown, callback registry, dispatch, `dispatch_update` |
| `ScriptVM` | `Source/FUSE/Script/include/fuse/script/script_vm.hpp` | Null backend or optional Lua compile/run |
| Bind helpers | `Source/FUSE/Script/include/fuse/script/script_bind.hpp` | Tagged `ScriptValue` carriers + `values_equal` |
| Lua stack bridge | `Source/FUSE/Script/include/fuse/script/script_bind_lua.hpp` | `push_to_stack` / `read_from_stack` when `FUSE_SCRIPT_LUA=1` |
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

`ScriptHost::register_callback` returns a `ScriptCallbackId`. Handlers run in registration order via `dispatch(event, ctx)`. `OnUpdate` handlers receive `ScriptCallbackContext::dt` each frame. Use `dispatch_update(dt, entity)` for the common per-frame path. `unregister_callback` and `clear_callbacks` remove entries without leaking raw function pointers in the public API.

Dispatch edge cases covered by tests:

- Zero `dt` still invokes `OnUpdate` handlers.
- `dispatch_update` on an uninitialized host is a no-op.
- Event kind isolation — `OnStart` dispatch does not invoke `OnUpdate` handlers (and vice versa).

### Bind helpers

`fuse::script::bind::ScriptValue` is a tagged POD-like carrier for script-facing values:

| Kind | Helpers |
|------|---------|
| `Nil` | `push_nil`, `is_nil` |
| `Bool` | `push_bool`, `to_bool`, `is_bool` |
| `Number` | `push_number`, `to_number`, `is_number` |
| `String` | `push_string`, `to_string`, `is_string` |
| `EntityId` | `push_entity_id`, `to_entity_id`, `is_entity_id` |
| `Transform` | `push_transform`, `to_transform`, `is_transform` |

`kind_name` returns a stable diagnostic label. `values_equal` compares kind + payload. Coercion helpers return defaults when the tag does not match.

When `FUSE_SCRIPT_LUA=1`, `fuse::script::bind::lua::push_to_stack` / `read_from_stack` round-trip tagged values through the Lua stack (primitives as native Lua types; ECS types as structured tables).

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
| `FUSE_SCRIPT_ENABLE_LUA=ON` + system Lua | `FUSE_SCRIPT_LUA=1`; VM uses Lua compile/run backend; stack bridge active |
| `FUSE_SCRIPT_ENABLE_LUA=OFF` or Lua missing | Null backend; loads recorded only; stack bridge compiled out |
| `FUSE_BUILD_CORE_TESTS=ON` | `fuse_script_b73` CTest target |

---

## Tests

| Target | Validates |
|--------|-----------|
| `fuse_script_b73` | Host init (null or Lua backend), load stubs / parse errors, callback register/dispatch/unregister, multi-frame `OnUpdate` dt propagation, zero-dt and event-isolation edge cases, `dispatch_update`, primitive + ECS bind round-trips, `values_equal`, Lua hello-world and stack round-trip when linked |

Run:

```bash
ctest --test-dir build --output-on-failure -R fuse_script
```

---

## Gates (B7.3 deepen follow-up)

- [x] `ScriptHost` / `ScriptVM` on FUSE APIs (null + optional Lua backend)
- [x] Register/unregister script callbacks
- [x] `OnUpdate` dispatch passes per-frame `dt` to all handlers
- [x] `dispatch_update` convenience + zero-dt / uninitialized / event-isolation edge cases
- [x] `load_string` / `load_file` with parse-error reporting when Lua linked
- [x] Bind helpers for nil/bool/number/string + `EntityID` / `Transform`
- [x] `values_equal` + Lua stack push/pop bridge when linked
- [x] CTest target green in umbrella CI
- [ ] ECS `Script` component + per-entity `lua_ref` (follow-up)
- [ ] Hot-reload watcher (follow-up)

---

## Next

- [ ] `ecs/components/script.hpp` + per-entity `lua_ref`
- [ ] `ScriptHotReload` file watcher (master plan sketch)
- [ ] Engine API surface (`Entity.*`, `Physics.*`, `Input.*`) as Lua bindings
- [ ] Wire stack bridge into callback dispatch (pass `ctx` fields to Lua handlers)

---

## Related docs

- [FUSE_MASTER_PLAN.md](../plans/FUSE_MASTER_PLAN.md) §B7.3
- [TRACK-B-ECS.md](./TRACK-B-ECS.md) — `EntityID` / `Transform` types consumed by bind helpers
