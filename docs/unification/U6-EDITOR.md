# U6 — Qt 6 Editor Shell (WP-08)

**Phase:** U6 / WP-08 vertical slice  
**Date:** 2026-09-15  
**Status:** Minimal desktop shell landed — project hub, viewport placeholder, game-thread command drain

---

## 1. Scope

| Target | Role |
|--------|------|
| `fuse_editor_api` | Qt-free boundary: `CommandQueue`, `EditorHost` |
| `fuse_editor` | Qt 6 desktop executable (optional; gated on `find_package(Qt6)`) |

**Architecture rule (locked):** UI thread **posts** `EditorCommand` envelopes; game thread **drains** via `EditorHost::gameTick()`. No raw `SceneObject*` (or other scene pointers) cross the UI boundary — only `fuse::Handle<T>` in command payloads.

See [architecture-parallel.md](./architecture-parallel.md) §2.1–§3.1 for process/thread model.

---

## 2. Desktop vs mobile policy

| Platform | Ships |
|----------|-------|
| **Desktop** (Windows, Linux, macOS) | `fuse_runtime` + optional `fuse_editor` |
| **Mobile** (iOS, Android) | **`fuse_runtime` only** — no editor binary in product builds |

`FUSE_BUILD_EDITOR` is ignored (with a CMake warning) when `FUSE_PLATFORM_MOBILE` is set. Mobile CI and store builds should leave `FUSE_BUILD_EDITOR=OFF`.

---

## 3. CMake options

| Option | Default | Description |
|--------|---------|-------------|
| `FUSE_BUILD_EDITOR_API` | `ON` | Build `fuse_editor_api` + headless tests |
| `FUSE_BUILD_EDITOR` | `OFF` | Build `fuse_editor` Qt shell when Qt 6 Widgets is found |
| `FUSE_BUILD_CORE_TESTS` | `ON` | Registers `fuse_editor_command_queue` and `fuse_editor_host` CTest entries |

`fuse_editor` uses `find_package(Qt6 COMPONENTS Widgets QUIET)`. When Qt is absent, **sources are still shipped** and CI runs the headless host tests only.

---

## 4. Build locally (with Qt 6)

### Linux (Ubuntu/Debian)

```bash
sudo apt-get install -y qt6-base-dev

cmake -B build-editor -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DFUSE_UMBRELLA=ON \
  -DFUSE_BUILD_CORE=ON \
  -DFUSE_BUILD_EDITOR_API=ON \
  -DFUSE_BUILD_EDITOR=ON \
  -DFUSE_BUILD_T3D=OFF \
  -DFUSE_BUILD_T2D=OFF

cmake --build build-editor --target fuse_editor
./build-editor/Source/FUSE/Editor/fuse_editor --samples Samples/unification
```

### macOS / Windows

Install Qt 6.5+ (Widgets module). Point CMake at your Qt installation (`CMAKE_PREFIX_PATH` or `Qt6_DIR`) and use the same flags as above.

---

## 5. Headless CI path (no Qt)

Linux umbrella CI configures with `FUSE_BUILD_EDITOR=OFF` (default). Tests:

| CTest name | Binary | Purpose |
|------------|--------|---------|
| `fuse_editor_command_queue` | `fuse_editor_api_tests` | Command queue post/drain |
| `fuse_editor_host` | `fuse_editor_host_tests` | `EditorHost` game-tick drain without Qt |

```bash
cmake -B build-fuse -G Ninja \
  -DFUSE_UMBRELLA=ON \
  -DFUSE_BUILD_CORE=ON \
  -DFUSE_BUILD_CORE_TESTS=ON \
  -DFUSE_BUILD_EDITOR_API=ON \
  -DFUSE_BUILD_EDITOR=OFF \
  -DFUSE_BUILD_T3D=OFF \
  -DFUSE_BUILD_T2D=OFF

cmake --build build-fuse
ctest --test-dir build-fuse -R fuse_editor --output-on-failure
```

---

## 6. Runtime layout (this slice)

```
┌─────────────────────────────────────────────────────────────┐
│ fuse_editor (single process)                                 │
│  ┌──────────────────┐   ┌───────────────────────────────┐ │
│  │ Qt UI thread     │   │ Game thread (QThread + QTimer) │ │
│  │  ProjectHub      │   │  EditorHost::gameTick()        │ │
│  │  Viewport stub   │──▶│  CommandQueue::drain()         │ │
│  │  postFromUi()    │   │                                │ │
│  └──────────────────┘   └───────────────────────────────┘ │
└─────────────────────────────────────────────────────────────┘
```

- **Project hub** — lists `Samples/unification/*/project.json` directories.
- **Viewport placeholder** — painted panel until embedded GPU viewport lands.
- **Open project** — posts `SetProperty { project = <name> }` to the queue (handle-only command envelope).

---

## 7. Public API headers

| Header | Notes |
|--------|-------|
| `fuse/editor/command_queue.hpp` | `EditorCommand`, `CommandQueue` |
| `fuse/editor/editor_host.hpp` | `EditorHost` — `postFromUi()` / `gameTick()` |

---

## 8. Follow-ups (out of WP-08 slice)

- Embedded `fuse_runtime` + real viewport (Vulkan/Metal/GLES)
- Inspectors, timelines, addon feature panes
- PIE (play-in-editor) wiring through `fuse_project` loader
- Thread-safe command queue storage (mutex + deque) once commands carry payloads beyond counters

---

## 9. Related docs

- [work-plan.md](./work-plan.md) — WP-08 status
- [architecture-parallel.md](./architecture-parallel.md) — editor threading
- [U7-PROJECT-FORMAT.md](./U7-PROJECT-FORMAT.md) — `project.json` consumed by hub
- [unified-layout.md](./unified-layout.md) — `Source/FUSE/Editor/` layout
