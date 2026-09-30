# Track B — Script Host (B7.3 deepen follow-up)

**Status:** B7.3 deepen follow-up — script console REPL prefix completion, suggest stub, history guards  
**Master plan:** [FUSE_MASTER_PLAN.md](../plans/FUSE_MASTER_PLAN.md) §B7.3  
**Threading:** Game-thread facade; hot-reload and ECS `Script` component deferred

---

## Scope

| Component | Location | Notes |
|-----------|----------|-------|
| `ScriptHost` | `Source/FUSE/Script/include/fuse/script/script_host.hpp` | Init/shutdown, callback registry, dispatch, `dispatch_update`, `tick_update_scripts` |
| `ScriptUpdateRegistry` | `Source/FUSE/Script/include/fuse/script/script_update.hpp` | Per-script OnUpdate tick, dt accumulation, enable/disable, error isolation |
| `ScriptVM` | `Source/FUSE/Script/include/fuse/script/script_vm.hpp` | Null backend or optional Lua compile/run |
| `ScriptConsole` | `Source/FUSE/Script/include/fuse/script/script_console.hpp` | Headless REPL — built-in command stubs, history buffer, host dispatch |
| `ScriptConsoleCommandRegistry` | `Source/FUSE/Script/include/fuse/script/script_console_command_registry.hpp` | Named built-in/custom command registry + dispatch-by-name |
| `ScriptConsoleHistoryBuffer` | `Source/FUSE/Script/include/fuse/script/script_console_history.hpp` | Fixed-capacity history ring buffer with recall navigation |
| Bind helpers | `Source/FUSE/Script/include/fuse/script/script_bind.hpp` | Tagged `ScriptValue` carriers + `values_equal` + `PropertyStore` / `MethodTable` |
| Lua stack bridge | `Source/FUSE/Script/include/fuse/script/script_bind_lua.hpp` | `push_to_stack` / `read_from_stack` when `FUSE_SCRIPT_LUA=1` |
| Callbacks | `Source/FUSE/Script/include/fuse/script/script_callback.hpp` | `OnStart`, `OnUpdate`, `OnDestroy`, collision/trigger hooks |
| Engine API | `Source/FUSE/Script/include/fuse/script/script_engine_api.hpp` | `bind_engine_api`: `Entity` `Physics` `Input` `Audio` `Scene` `SDF` `CVar` tables; one backend pointer per service in `ScriptEngineBindings` |
| API backends | `Source/FUSE/Script/include/fuse/script/script_engine_backends.hpp` | `InputScriptBackend` (InputState + ActionMap + GamepadState / PlayerController), `RegistrySceneBackend` (names, sphere query via BVH or scan, active camera) |
| Audio bridge | `Source/FUSE/Script/include/fuse/script/script_audio_bridge.hpp` | `AudioEngineScriptBackend` in `fuse_script_audio` (links fuse_audio; fuse_script stays audio-free) |

**Not in scope:** hot-reload watcher, Qt editor console chrome. (The ECS `Script` component and the engine API tables have since landed — see below.)

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

### Per-script OnUpdate registry

`ScriptUpdateRegistry` (owned by `ScriptHost::update_registry()`) manages named script instances with per-frame `tick(dt, entity)`:

- Scripts tick in **registration order** (multi-script order tests).
- Each instance tracks **accumulated `dt`** across frames.
- **`set_enabled` / `is_enabled`** — disabled scripts are skipped on tick; accumulated dt freezes at last active frame.
- **Error isolation** — a throwing script is caught, recorded via `error_count()` / `last_error()`, and remaining scripts continue.

`ScriptHost::tick_update_scripts(dt, entity)` delegates to the registry when initialized.

### Engine API tables (MP-B7.3-LUA-API)

`bind_engine_api(vm, bindings)` registers these globals. Each table reads its service from
`ScriptEngineBindings` (`registry`, `physics`, `input`, `audio`, `scene`, `cvars`); a call whose
service is null, or with a malformed argument / unknown name, raises a Lua error that the VM's
protected call reports. Entity ids are numbers (`generation * 2^32 + index`); a behaviour's `self`
table is accepted wherever an id is.

```lua
-- Entity
Entity.create([name]) -> id            -- a name is registered with the scene backend
Entity.destroy(id) / Entity.alive(id)
Entity.get_position(id) -> {x,y,z}     Entity.set_position(id, {x,y,z})
Entity.get_rotation(id) -> {x,y,z,w}   Entity.set_rotation_euler(id, {x,y,z})  -- degrees
-- Physics
Physics.ray_cast(origin, dir[, max]) -> {entity, point, normal, distance} | nil
Physics.apply_impulse(id, v)  Physics.set_velocity(id, v)  Physics.get_velocity(id) -> v | nil
-- Input (keys: "Space", "W", "F1", "Left", ...; mouse buttons: "Left", "Right", "Middle", "X1", "X2")
Input.key_pressed(k) / key_held(k) / key_released(k) -> bool
Input.mouse_pressed(b) / mouse_held(b) / mouse_released(b) -> bool
Input.mouse_delta() -> dx, dy          Input.mouse_position() -> x, y
Input.pressed(action) / held(action) / released(action) -> bool   Input.axis(action) -> number
Input.gamepad_connected(pad) -> bool   -- pad 0..3
-- Audio
Audio.load(path) -> clip | nil
Audio.play_at(clip, {x,y,z}[, volume[, pitch]]) -> bool   Audio.play_2d(clip[, volume]) -> bool
-- Scene
Scene.find_entity(name) -> id | nil    Scene.query_sphere({x,y,z}, radius) -> {id, ...}
Scene.set_active_camera(id) -> bool    Scene.active_camera() -> id | nil
-- SDF (SDFObject component)
SDF.set_radius(id, r)  SDF.set_alpha(id, a)  SDF.set_primitive(id, "Sphere"|"Box"|"Capsule"|"Torus"|"Cylinder"|"Custom")
SDF.get(id) -> {primitive, radius, alpha, params = {x,y,z}} | nil
-- CVar (fuse::config, runtime precedence)
CVar.get(name) -> bool | number | string | nil    CVar.set(name, value) -> true | false, reason
```

Gate: `fuse_script_lua_api` (Input over synthetic key / mouse / gamepad events and the default
ActionMap, Audio through the Null backend with audible capture, Scene names / sphere query (scan
and BVH agree) / active camera, SDF radius / alpha / primitive edits, CVar get / set, errors for
missing services and bad names).

### Script console / REPL

`ScriptConsole` is a headless REPL facade for editor and tooling integration (distinct from `fuse::editor::ConsolePanel` log chrome).

Built-in command stubs:

| Command | Behaviour |
|---------|-----------|
| `help` | Lists built-in and registered custom commands (single-line `commands:` prefix) |
| `list` | Lists built-in and registered custom commands (one name per line) |
| `echo <text>` | Returns argument text as output |
| `clear` | Clears accumulated output lines |
| `history` | Prints indexed command history |
| `backend` | Reports attached `ScriptHost` VM backend + loaded chunk count |
| `load <path>` | Dispatches `ScriptHost::load_file` |
| `run <lua>` | Dispatches `ScriptHost::load_string` with chunk name `repl` |
| `describe <name>` | Reports `built-in`, `custom`, or `unknown` for a command name |
| `complete <prefix>` | Returns shared prefix + space-separated matches; sole match returns the command name |
| `suggest <partial>` | Returns up to three `suggest_commands` matches (fuzzy/prefix hints) |
| `repeat` | Re-dispatches the last non-`repeat` command line |

Custom commands register via `register_command` / `unregister_command` and participate in `help` / `list` output (sorted alphabetically). Custom handlers may **shadow** built-in names — dispatch checks the custom map first, then built-ins. `has_command` / `is_built_in_command` / `is_custom_command` / `command_kind` expose lookup for tooling. Unknown commands return `UnknownCommand` with an `unknown command: <name>` error string and up to three `did you mean:` suggestions from `suggest_commands`.

`ScriptConsoleCommandRegistry` owns built-in and custom handler maps; `ScriptConsole::dispatch_` delegates by name (custom overrides are checked before built-ins). `lookup_kind`, `commands_with_prefix`, `longest_common_prefix`, `unique_prefix_match`, and `suggest_commands` support `describe` / `complete` / `suggest` stubs and unknown-command hints. `ScriptConsole` forwards `command_names`, prefix lookup, and completion helpers for editor wiring. `is_valid_command_name` rejects empty or whitespace-containing names on register. Registry unit tests cover register/dispatch failure paths, duplicate built-in rejection, invalid name rejection, unregister failures, prefix/LCP/unique-match lookup, suggestion lookup, and unknown-command errors directly (not only via `ScriptConsole::execute`).

**History buffer** — `ScriptConsoleHistoryBuffer` ring buffer (default 64 entries, configurable via `setHistoryCapacity`):

- Skips consecutive duplicate lines (same as editor console coalescing).
- Evicts oldest entries on overflow (ring wrap covered by direct buffer tests).
- `newest()` / `oldest()` expose the surviving range after wrap.
- `is_empty()` / `is_valid_index()` guard empty and out-of-range history access (`at` returns empty for invalid indices).
- `recallHistory(previous)` supports up/down navigation for REPL recall; `resetHistoryNavigation` returns to the live input position.
- `execute(nullptr)` returns `InvalidArgument` without recording history.
- `history clear` built-in stub (and `ScriptConsoleHistoryBuffer::clear`) empties the buffer without touching output lines.

`attach(ScriptHost*)` wires `load`, `run`, and `backend` to the game-thread host. Detached consoles still run local stubs (`help`, `echo`, `history`, `clear`).

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

**Property store** — `PropertyStore` holds named `ScriptValue` properties (`set_property`, `get_property`, `get_property_or`, `remove_property`). Intended as a stub for per-entity script instance data until ECS `Script` lands.

**Method table** — `MethodTable` registers optional named `ScriptMethodFn` handlers (`register_method`, `invoke`, `unregister_method`). Missing methods return `nil`.

When `FUSE_SCRIPT_LUA=1`, `fuse::script::bind::lua::push_to_stack` / `read_from_stack` round-trip tagged values through the Lua stack (primitives as native Lua types; ECS types as structured tables).

---

## Build

`fuse_script` builds when `FUSE_BUILD_SCRIPT=ON` (default).

```bash
cmake -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
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
| `fuse_script_b73` | Host init (null or Lua backend), load stubs / parse errors, callback register/dispatch/unregister, multi-frame `OnUpdate` dt propagation, zero-dt and event-isolation edge cases, `dispatch_update`, per-script tick order / disable / error isolation, `PropertyStore` + `MethodTable` bind stubs, primitive + ECS bind round-trips, `values_equal`, Lua hello-world and stack round-trip when linked, `ScriptConsole` built-in/custom command dispatch (including custom shadow of built-ins), `describe` / `complete` / `suggest` / `repeat` lookup stubs, prefix/LCP/unique-match completion helpers, unknown-command `did you mean` suggestions, `ScriptConsoleCommandRegistry` register/dispatch/lookup_kind/invalid-name/unknown-command paths, `ScriptConsoleHistoryBuffer` ring wrap + `newest`/`oldest`/`is_empty`/`is_valid_index` + clear + whitespace/null rejection, null-line execute guard, history buffer eviction/navigation, host `load`/`run`/`backend` wiring |

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
- [x] Per-script `ScriptUpdateRegistry` — tick order, dt accumulation, enable/disable, error isolation
- [x] `PropertyStore` + `MethodTable` bind stubs
- [x] `load_string` / `load_file` with parse-error reporting when Lua linked
- [x] Bind helpers for nil/bool/number/string + `EntityID` / `Transform`
- [x] `values_equal` + Lua stack push/pop bridge when linked
- [x] `ScriptConsole` REPL stubs + history buffer + command dispatch tests
- [x] CTest target green in umbrella CI
- [x] ECS `Script` component + per-entity `lua_ref` — `ecs/components/script.hpp` (module path / AssetId, enabled, started, lua_ref, 8-entry number/bool/string/vec3 property table), registered + serialised (runtime fields scrubbed, corrupt rows rejected), `ScriptSystem` attach/detach on add/remove/disable/module change/destroy/scene load, `enterPlay`/`exitPlay`, contact dispatch; gate `fuse_script_component` (MP-B7.3-SCRIPT-COMPONENT, CPU)
- [x] Engine API tables `Input` / `Audio` / `Scene` / `SDF` / `CVar` next to `Entity` / `Physics`, each with a backend pointer in `ScriptEngineBindings`; gate `fuse_script_lua_api` (MP-B7.3-LUA-API, CPU)
- [ ] Hot-reload watcher (follow-up)

---

## Next

- [x] `ecs/components/script.hpp` + per-entity `lua_ref` (see gates above; `CookAssetKind::Script` cooks `.lua` → bytecode `.fusescript`, gate `fuse_script_cook`)
- [x] `ScriptHotReload` file watcher — polled by the editor's PIE session (module edits reload into the live runtime and are logged on the console; E15, `fuse_editor_e15_console_pie`)
- [x] Engine API surface (`Entity.*`, `Physics.*`, `Input.*`, `Audio.*`, `Scene.*`, `SDF.*`, `CVar.*`) as Lua bindings (MP-B7.3-LUA-API; runtime / PIE hosts fill `ScriptEngineBindings` in their own packages)
- [ ] Wire stack bridge into callback dispatch (pass `ctx` fields to Lua handlers)
- [x] Wire `ScriptConsole` into `fuse::editor::ConsolePanel` command line (E15: non-engine lines go to the game thread's ScriptConsole / Lua REPL via the CommandQueue; Qt console dock included)

---

## Related docs

- [FUSE_MASTER_PLAN.md](../plans/FUSE_MASTER_PLAN.md) §B7.3
- [TRACK-B-ECS.md](./TRACK-B-ECS.md) — `EntityID` / `Transform` types consumed by bind helpers
- [TRACK-B-EDITOR.md](./TRACK-B-EDITOR.md) — `ConsolePanel` log buffer (B6.11); REPL wiring deferred
