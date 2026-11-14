# Editor

The editor is a **desktop** application. Its chrome is built with FUSE UI (`Source/FUSE/EditorShell`). Mobile builds ship `fuse_runtime` only.

> **2026-11-14: Qt removed.** The editor used a Qt 6 Widgets shell until this date. On the owner's decision the Qt shell, its tests and the `fuse_editor_qt_*` gates were deleted, and the FUSE UI shell became the only editor. See [FUSE_EDITOR_QT_FIRST_PLAN.md](plans/FUSE_EDITOR_QT_FIRST_PLAN.md) §12 and the historical gate record in [editor/QT_PARITY.md](editor/QT_PARITY.md).

## Pieces

| Target | Role |
|--------|------|
| `fuse_editor_api` | Editor boundary library: `CommandQueue`, `EditorHost`, undo, scene files, PIE, gizmos, panel models |
| `fuse_editor_controller` | `EditorController`, the action table and theme tokens (`EditorShell/Controller`, links only `fuse_editor_api`) |
| `fuse_editor_scene_host` | `SceneViewportHost`: renders the `EditorHost` scene for the viewport (`EditorShell/SceneHost`) |
| `fuse_editor_fuse` | The FUSE UI shell library and the editor binary (`EditorShell/Fuse`) |
| `fuse_editor_ui_gates` | Gate harness for `ctest -L fuse_editor_ui` |

**Locked rule:** the UI thread **posts** `EditorCommand` envelopes; the game thread **drains** them in `EditorHost::gameTick()`. No raw scene pointers cross that boundary — only `fuse::Handle<T>` in payloads. The shell talks only to `EditorController`.

The editor-shell lint `fuse_lint_editor_shell_fuse_ui` checks that no editor target links or includes Qt (or Dear ImGui).

## Platform policy

| Platform | Ships |
|----------|-------|
| Windows, Linux | Runtime + optional editor |
| macOS | Runtime + editor, not verified. `fuse_app_native` has a Cocoa backend (`native_apple.mm`), but it has never run ([ui/KNOWN_GAPS.md](ui/KNOWN_GAPS.md)); there is no NSAccessibility gate |
| iOS, Android | Runtime only |

`FUSE_BUILD_EDITOR` is ignored (CMake warning) when `FUSE_PLATFORM_MOBILE` is set.

## Build

No third-party UI toolkit is needed. Turn the editor on with `FUSE_BUILD_EDITOR=ON`. It needs `FUSE_BUILD_UI` (ON by default):

```bash
cmake -B build-editor -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DFUSE_BUILD_CORE=ON \
  -DFUSE_BUILD_EDITOR_API=ON \
  -DFUSE_BUILD_EDITOR=ON

cmake --build build-editor --target fuse_editor_fuse_app fuse_editor_ui_gates
./build-editor/Source/FUSE/EditorShell/fuse_editor_fuse
```

The binary is named `fuse_editor_fuse` (CMake target `fuse_editor_fuse_app`). Useful flags: `--gpu-viewport` (live zero-copy viewport), `--font-scale`, `--screenshot`.

On Linux, `fuse_app_native` loads libX11, libXrandr, libXcursor, libxkbcommon and libdbus-1 at run time; no X11 headers are needed to build. The gates run under Xvfb (`xvfb`, `xauth`) when there is no display. To run every `fuse_editor_ui` test, build the whole tree: the tests span several executables. CI: the `fuse-editor-ui` job in `fuse-umbrella-linux.yml` — see [building.md](building.md#editor-gates-local-playbook).

## Tests

| CTest | Purpose |
|-------|---------|
| `fuse_editor_command_queue` | Post / drain (`fuse_editor_api_tests`) |
| `fuse_editor_host` | `EditorHost` game tick, no window |
| `fuse_editor_controller_api` | `EditorController` on scripted services |
| `fuse_editor_panel_models` | `EditorPanels` models |
| `fuse_editor_ui_<gate>` | Shell gates: startup, layout, theme, fonts, panels, viewport, PIE, frame budgets, accessibility, … |

Run all of them with `ctest -L fuse_editor_ui`. The GPU gates (`live_present`, `scene_viewport`, `gpu_viewport`, `present_adopt`) exit 77 (skipped) without a Vulkan device or display. `ui_frame` and `idle_frames` enforce budgets only in Release.

## Chrome

Menus and a toolbar come from the action table. Docks hold tabs and can split into several tab stacks per side. Panels: Hierarchy, Inspector, Console, Project Hub, Asset Browser, Material Editor, SDF Sculpt, Profiler, Sequencer, Timeline, Lighting. The New Project wizard, project settings, preferences and Build / Package dialogs are in the shell. Gizmos draw into the viewport as a vector overlay in the UI draw list.

## Viewport

`SceneViewportHost` creates the engine Vulkan instance and device and a `HybridSceneRenderer`. It renders the scene from the viewport camera into a headless RGBA8 target, only when the camera, size or scene changes (every frame while playing).

With `--gpu-viewport`, the FUSE UI presenter adopts that device and samples the frame in place as an external texture (zero-copy). Without a GPU-drawn window (`--screenshot`, CPU presenter), the host reads the frame back into the viewport image. Without a device, the shell shows a software grid viewport.

Gates: `scene_viewport` (readback path, re-render on change, idle when unchanged, 0 validation errors) and `gpu_viewport` (adopted device, window pixels match the engine frame, 0 validation errors).

