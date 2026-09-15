# Track B — Editor Panels (B6.6–B6.8)

**Status:** B6.6 Property Inspector + B6.7 Material Editor + B6.8 SDF Sculpt panel API stubs landed  
**Master plan:** [FUSE_MASTER_PLAN.md](../plans/FUSE_MASTER_PLAN.md) §B6.6–B6.8  
**Threading:** [architecture-parallel.md](./architecture-parallel.md) §4 (Qt chrome on UI thread; game thread drains commands)

---

## Scope

| Component | Location | Notes |
|-----------|----------|-------|
| `EditorState` | `Source/FUSE/Editor/include/fuse/editor/editor_state.hpp` | Selection + play/edit flags shared by panels |
| `EditorScene` | `Source/FUSE/Editor/include/fuse/editor/editor_scene.hpp` | Owns `fuse::ecs::Registry` for headless panel tests |
| `CommandStack` | `Source/FUSE/Editor/include/fuse/editor/command_stack.hpp` | Minimal undo/redo stub posting to `CommandQueue` |
| `PropertyInspector` | `Source/FUSE/Editor/include/fuse/editor/property_inspector.hpp` | B6.6 — component section model + live ECS edits |
| `MaterialEditorPanel` | `Source/FUSE/Editor/include/fuse/editor/material_editor_panel.hpp` | B6.7 — catalog/selection + `MaterialEditState` |
| `SdfSculptPanel` | `Source/FUSE/Editor/include/fuse/editor/sdf_sculpt_panel.hpp` | B6.8 — brush state + stroke spacing / symmetry |

**Not in scope:** Qt dock widgets, ImGui, live material preview RT, full undo command objects (B6.2), viewport picking wiring (B6.3–B6.4).

---

## Design

Panels are **Qt-free** and live in `fuse_editor_api` so CI and headless hosts can exercise editor logic without linking `fuse_editor`.

- `PropertyInspector::sync()` introspects ECS components on `EditorState::primarySelection` and exposes section metadata for future Qt property widgets.
- `MaterialEditorPanel` keeps a POD `MaterialEditState`; when `FUSE_VULKAN_BACKEND=1`, `syncFromMaterialSystem()` / `pushToMaterialSystem()` bridge to `fuse::renderer::MaterialSystem`.
- `SdfSculptPanel::handleBrushStroke()` enforces minimum world-space spacing and optional X symmetry, posting `EditorCommand` envelopes for the game thread.

Existing WP-08 pieces remain unchanged:

- `EditorHost` + `CommandQueue` — UI posts, game thread drains (`editor_host.hpp`).
- Optional Qt 6 shell — `FUSE_BUILD_EDITOR=ON` (`fuse_editor` binary).

---

## Build

`fuse_editor_api` builds with the umbrella when `FUSE_BUILD_EDITOR_API=ON` (default in CI).

```bash
cmake -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DFUSE_UMBRELLA=ON \
  -DFUSE_BUILD_CORE=ON \
  -DFUSE_BUILD_CORE_TESTS=ON \
  -DFUSE_BUILD_EDITOR_API=ON \
  -DFUSE_BUILD_EDITOR=OFF \
  -DFUSE_BUILD_VULKAN=ON

cmake --build build
ctest --test-dir build --output-on-failure -R fuse_editor
```

| Condition | Behaviour |
|-----------|-----------|
| `FUSE_BUILD_EDITOR_API=OFF` | No `fuse_editor_api` target |
| `FUSE_BUILD_VULKAN=OFF` | Material panel RHI bridge methods no-op; ECS/sculpt tests still run |
| `FUSE_BUILD_EDITOR=ON` + Qt6 | Desktop shell links `fuse_editor_api` (panels not yet wired into docks) |

---

## Tests

| Target | Validates |
|--------|-----------|
| `fuse_editor_command_queue` | UI/game command envelope posting |
| `fuse_editor_host` | `EditorHost::gameTick()` drain semantics |
| `fuse_editor_panels` | B6.6–B6.8 inspector/material/sculpt headless API |

Run:

```bash
ctest --test-dir build --output-on-failure -R fuse_editor
```

---

## Gates (B6.6–B6.8)

- [x] **B6.6** `PropertyInspector` on FUSE ECS APIs (Qt-free)
- [x] **B6.7** `MaterialEditorPanel` stub with optional RHI bridge
- [x] **B6.8** `SdfSculptPanel` brush + stroke command stub
- [x] CTest target `fuse_editor_panels` green with `FUSE_BUILD_VULKAN=ON`
- [x] No owning raw pointers in public editor panel APIs
- [ ] Qt 6 dock wiring for the three panels (follow-up under `FUSE_BUILD_EDITOR`)
- [ ] ASan/UBSan smoke dedicated to editor panels (umbrella ASan job covers runtime smoke)

---

## Next

- [ ] B6.2 — typed undo commands replacing stringly `SetProperty` envelopes
- [ ] B6.3–B6.5 — viewport/hierarchy integration with `EditorState` selection
- [ ] Wire panels into `fuse_editor` Qt dock layout
- [ ] Live material preview render target (B6.7 follow-up)

---

## Related docs

- [FUSE_MASTER_PLAN.md](../plans/FUSE_MASTER_PLAN.md) §B6
- [TRACK-B-ECS.md](./TRACK-B-ECS.md) — component types inspected by B6.6
- [TRACK-B-VULKAN.md](./TRACK-B-VULKAN.md) — `MaterialSystem` bridge for B6.7
