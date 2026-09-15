# Track B — Script Host (B7.3)

**Status:** B7.3 Lua-ready host stubs landed  
**Master plan:** [FUSE_MASTER_PLAN.md](../plans/FUSE_MASTER_PLAN.md) §B7.3  
**Threading:** Game-thread facade; hot-reload and Lua stack deferred

---

## Scope

| Component | Location | Notes |
|-----------|----------|-------|
| `ScriptHost` | `Source/FUSE/Script/include/fuse/script/script_host.hpp` | Init/shutdown, callback registry, dispatch |
| `ScriptVM` | `Source/FUSE/Script/include/fuse/script/script_vm.hpp` | Null backend; `load_string` / `load_file` stubs |
| Bind helpers | `Source/FUSE/Script/include/fuse/script/script_bind.hpp` | `EntityID` / `Transform` tagged values for future Lua stack |
| Callbacks | `Source/FUSE/Script/include/fuse/script/script_callback.hpp` | `OnStart`, `OnUpdate`, `OnDestroy`, collision/trigger hooks |

**Not in scope:** Real Lua VM integration, `Script` ECS component, hot-reload watcher, physics/input API bindings.

---

## Design (B7.3 stub)

### Null backend first

`ScriptVM` defaults to `ScriptBackendKind::Null`. Loads are accepted when the source/path is valid and recorded for tests; no bytecode is executed. When system Lua is found (`FUSE_SCRIPT_ENABLE_LUA=ON`), the target defines `FUSE_SCRIPT_LUA=1` for follow-up wiring without changing public headers.

### Callback registry

`ScriptHost::register_callback` returns a `ScriptCallbackId`. Handlers run in registration order via `dispatch(event, ctx)`. `unregister_callback` and `clear_callbacks` remove entries without leaking raw function pointers in the public API.

### Bind helpers

`fuse::script::bind::ScriptValue` is a tagged POD-like carrier for `EntityID` and `Transform` until a real Lua stack lands. Round-trip helpers keep ECS types out of script-facing headers that will later include `lua.hpp`.

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
| `FUSE_SCRIPT_ENABLE_LUA=ON` + system Lua | `FUSE_SCRIPT_LUA=1` defined; backend remains null until Lua backend is implemented |
| `FUSE_BUILD_CORE_TESTS=ON` | `fuse_script_b73` CTest target |

---

## Tests

| Target | Validates |
|--------|-----------|
| `fuse_script_b73` | Host init, load stubs, callback register/dispatch/unregister, `EntityID`/`Transform` bind round-trips |

Run:

```bash
ctest --test-dir build --output-on-failure -R fuse_script
```

---

## Gates (B7.3 stub)

- [x] `ScriptHost` / `ScriptVM` on FUSE APIs (null backend)
- [x] Register/unregister script callbacks
- [x] `load_string` / `load_file` stubs
- [x] Bind helpers for `EntityID` and `Transform`
- [x] CTest target green in umbrella CI
- [ ] Real Lua backend + `Script` component (follow-up)
- [ ] Hot-reload watcher (follow-up)

---

## Next

- [ ] Wire `ScriptVM` to Lua when `FUSE_SCRIPT_LUA=1`
- [ ] `ecs/components/script.hpp` + per-entity `lua_ref`
- [ ] `ScriptHotReload` file watcher (master plan sketch)
- [ ] Engine API surface (`Entity.*`, `Physics.*`, `Input.*`) as Lua bindings

---

## Related docs

- [FUSE_MASTER_PLAN.md](../plans/FUSE_MASTER_PLAN.md) §B7.3
- [TRACK-B-ECS.md](./TRACK-B-ECS.md) — `EntityID` / `Transform` types consumed by bind helpers
