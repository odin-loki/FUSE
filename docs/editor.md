# Editor

The editor is a **desktop** Qt 6 application. Mobile builds ship `fuse_runtime` only.

## Pieces

| Target | Role |
|--------|------|
| `fuse_editor_api` | Qt-free boundary: `CommandQueue`, `EditorHost` |
| `fuse_editor` | Qt 6 Widgets shell (optional binary) |

**Locked rule:** the UI thread **posts** `EditorCommand` envelopes; the game thread **drains** them in `EditorHost::gameTick()`. No raw scene pointers cross that boundary — only `fuse::Handle<T>` in payloads.

Engine libraries must not `#include` Qt headers.

## Platform policy

| Platform | Ships |
|----------|-------|
| Windows, Linux, macOS | Runtime + optional editor |
| iOS, Android | Runtime only |

`FUSE_BUILD_EDITOR` is ignored (CMake warning) when `FUSE_PLATFORM_MOBILE` is set.

## Build with Qt 6

Requires Qt 6.5+ Widgets.

### Linux

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

### Windows / macOS

Install Qt 6 Widgets. Pass `CMAKE_PREFIX_PATH` or `Qt6_DIR` if CMake cannot find it, then use the same flags.

When Qt is absent, sources still ship. CI runs the headless host tests only (`FUSE_BUILD_EDITOR=OFF`).

## Headless tests (no Qt)

| CTest | Binary | Purpose |
|-------|--------|---------|
| `fuse_editor_command_queue` | `fuse_editor_api_tests` | Post / drain |
| `fuse_editor_host` | `fuse_editor_host_tests` | `EditorHost` game-tick without Qt |

## Intended chrome

Track B panels (in progress): project hub, viewport, gizmos, hierarchy, inspector, material editor, SDF sculpt, asset browser, profiler, console, play mode. Viewport embeds the renderer via a native window container; gizmos draw into the viewport, not a third-party immediate-mode UI.

Current tree: a project hub, viewport placeholder, and command drain — enough to prove the thread split.
