# FUSE U3 — Script Host Unification (prestarter §3.4)

**Phase:** U3+ shared services  
**Decision (interim):** **Lua-primary** `fuse::script::ScriptHost` — dual TorqueScript VMs stay quarantined; legacy `.cs` content routes through compat stubs until cooked forward.

---

## 1. What landed

| Component | Location | Role |
|-----------|----------|------|
| `ScriptHost` | `Source/FUSE/Script/include/fuse/script/script_host.hpp` | Game-thread VM + callback registry (B7.3) |
| `ScriptHostService` | `Source/FUSE/Script/include/fuse/script/script_host_service.hpp` | Singleton facade toward one process-wide host |
| `legacy_script_route` | `Source/FUSE/Script/include/fuse/script/legacy_script_route.hpp` | Chunk-name prefix routing (`uaisk:`, `t3d:`, `t2d:`, `fuse:`) |
| `ScriptHostBridge` (UAISK) | `Source/FUSE/Modules/ai/include/fuse/ai/uaisk_script_host_bridge.hpp` | Module-level import of `.cs` BT assets via host |

**Stakeholder gate (prestarter §18.1):** Lua vs single TorqueScript vs other — **interim lock: Lua-primary host + compat routing stubs**. No second VM linked in the umbrella binary.

---

## 2. Chunk routing

| Prefix | Dialect | Behaviour today |
|--------|---------|-------------------|
| *(none)* / `fuse:` | `Fuse` | `ScriptHost::load_string` / Lua backend when linked |
| `uaisk:` | `UaiskCompat` | Loads through host; `uaisk_script_host_bridge` registers BT profiles |
| `t3d:` | `T3dTorqueScript` | **Compat stub** — route recorded, dual VM **not** invoked |
| `t2d:` | `T2dTorqueScript` | **Compat stub** — route recorded, dual VM **not** invoked |

`ScriptHostService::load_chunk` is the single entry point for new code. `compat_route_count()` tracks `t3d:`/`t2d:` stubs; `fuse_route_count()` tracks host-backed loads.

---

## 3. Honest blockers

| Blocker | Notes |
|---------|-------|
| Dual TorqueScript VMs | Still quarantined U0–U2; `compat/ts_t3d` / `compat/ts_t2d` not linked |
| ECS `Script` component | Per-entity `lua_ref` deferred — see [TRACK-B-SCRIPT.md](./TRACK-B-SCRIPT.md) |
| Hot-reload watcher | Deferred |
| Editor REPL wiring | `ScriptConsole` headless stubs exist; Qt chrome deferred (U6) |

---

## 4. Tests

```bash
ctest --test-dir build --output-on-failure -R fuse_script_b73
```

`fuse_script_b73` covers `parse_legacy_chunk_route`, `ScriptHostService` fuse/uaisk/t3d/t2d routing, and existing host/console/update/bind suites.

---

## 5. Next

1. Wire `ScriptHostService` into runtime smoke / hybrid gates (optional follow-up).
2. `ecs/components/script.hpp` + per-entity instance table.
3. Lua bindings for `Entity.*` / `Transform` via existing `script_bind` helpers.
4. Cook pipeline tags for legacy `.cs` → FUSE script chunks in U7.
