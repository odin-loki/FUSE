# FUSE UI — not done and not verified

Status as of 2026-11-12, branch `ccr-111d9b83-2iujo8`. This lists what is missing or has never been exercised, so
nobody mistakes "compiles in CI" for "works on a device". Phase-by-phase detail is in
[`../plans/FUSE_UI_PLAN.md`](../plans/FUSE_UI_PLAN.md) §9; usage is in [`README.md`](README.md).

## What has been verified

| Platform | How | Result |
|---|---|---|
| Linux (UI standalone) | Local build + ctest | 178/178 (MINIMAL text), 177/177 (FULL text) |
| Linux (whole engine, in-tree) | `fuse-umbrella-linux.yml` | 1245 of 1247; the two failures also fail on `main` (see below) |
| Android arm64 (NDK r26d) | `fuse-core-android.yml` + local cross-build | Builds and links, apps as `lib<name>.so` |
| macOS | `fuse-core-ios.yml` (macOS job, MoltenVK headers) | Builds; headless gates pass |
| iOS simulator | `fuse-core-ios.yml` (iOS job) | Builds and links |
| FreeBSD 14 x86-64 | `fuse-ui-freebsd.yml` in a FreeBSD VM | Builds; headless gates pass |
| Windows (MSVC, clang-cl) | `fuse-windows-native.yml` | No UI build or link errors; jobs still fail on non-UI code (see below) |

The last commit before this file (a11y bridge gate ordering fix for MSVC, FreeBSD workflow trigger restored) was
pushed after those CI runs and re-checked on Linux only.

## Never run

- **No real device or desktop session.** Nothing has run on a phone, tablet, Mac, Windows desktop, or an iOS /
  Android simulator. Every mobile and Apple path is compile-checked plus headless tests.
- **Win32 platform code has never executed** (only compiled by MSVC / clang-cl / MinGW): per-monitor DPI and
  `WM_DPICHANGED`, clipboard, cursors, fullscreen / min-max handling, theme registry watch, native file dialogs.
- **Apple platform code has never executed**: Cocoa and UIKit windows, `NSTextInputClient` IME, native menu bar,
  cursors (diagonal-resize and busy cursors use private `NSCursor` selectors behind `respondsToSelector`), window
  management, theme observer, file panels, HiDPI layout, touch path on iOS.
- **Android platform code has never executed**: NativeActivity lifecycle, surface loss / restore, touch, JNI
  clipboard and key-character lookups, soft keyboard, HiDPI buffer geometry.
- **X11 window-manager features** (maximise, fullscreen, minimise, activate) were only exercised under Xvfb with no
  window manager, i.e. as "state stays Normal".
- **GPU output for the new work is untested** (no Vulkan device where this was developed): vector paths, gradients,
  9-slice, clip-path and layer groups are pre-rasterised on the CPU and packed as images, checked only against a C++
  model of the kernel's image op; render scale other than 1 on the GPU path; synthetic bold / italic glyphs.
  `fuse_ui_vk_parity` has not been run with any of these.
- **Installed-font scanning** ran on Linux only; the macOS, iOS, Android and Windows directory lists are untested,
  and Windows font paths go through narrow `fopen` (non-ANSI paths may fail).
- **Packaging was never built**: the Android Gradle template (`App/android/`), the Apple bundle rules and
  `Info.plist` templates, and whether bundle resources land where `readResource("ui/...")` looks on iOS under a
  non-Xcode generator.
- **AccessKit backend** (`A11y/src/a11y_accesskit.cpp`) with the extended accessibility data was not compiled (needs
  accesskit-c / a Rust toolchain).
- **Sanitizers**: one ASan + UBSan pass over the UI suite was done before the last fixes; not repeated on the final
  tree. Not built at all: `-DFUSE_UI_TEXT=FULL` on any platform other than Linux.

## Known failures

- `fuse_core_ui1_text_input` fails on Windows (MSVC shipping job): `IMM32 composition string -> TextComposition`.
  It is an engine-core test (`Source/FUSE/Core/tests`), not the UI module; not checked against `main`.
- Failing on `main` as well and left alone (outside the UI module):
  - Linux: `fuse_vulkan_validation_gate` (VUID-VkImageViewCreateInfo-image-04441), `fuse_runtime_steady_state_alloc`
    (editor phase allocates), Qt gates `fuse_editor_qt_idle_frames`, `fuse_editor_qt_lighting`,
    `fuse_editor_qt_shell_assets`.
  - Windows: `EditorShell/SceneHost/src/scene_viewport_host.cpp` cannot find
    `fuse/renderer/geometry/meshlet_builder.hpp`; `EditorShell/Controller/src/editor_panel_models.cpp` uses the
    deprecated `std::filesystem::u8path`; `Editor/tests/qt/test_editor_qt_gates.cpp` (a variable named `far`
    collides with `windows.h`, discarded `[[nodiscard]]`) and `test_editor_qt_shell.cpp`.
- `fuse_world_extract_synthetic` (Tools/FUSE/WorldExtract) failed once on this branch and passed on the next run;
  cause not investigated.
- Wall-clock gates (label `perf`: `fuse_ui_bench_10k`, `fuse_ui_modelview_1m_rows`, `fuse_ui_mv_selection_1m`,
  `fuse_ui_mv_tree_1m`, `fuse_ui_vector_steady`, `fuse_app_hello_startup`) are excluded on the FreeBSD and macOS CI
  VMs, so their non-timing checks do not run there either. `fuse_ui_bench_10k` can also miss its budget on a loaded
  Linux machine.

## Not implemented

### Platform

- Backends do not feed the newer input APIs: OS file drops (`UiContext::dropExternal`), pinch / rotate (only the
  primary touch is forwarded on Android and iOS), pen pressure / tilt. No drag from the app out to the OS.
- Android: no IME string commits (needs a Java `InputConnection`), no pointer icons, no system file picker, no
  refresh-rate query; theme-change notification needs `android:configChanges="uiMode"`.
- No gamepad backend in `fuse_app` (the `game_menu` sample takes pad input from a script or the host).
- X11: one scale per connection, no INCR clipboard transfers, no theme-change notification, no Wayland backend.
- Popups as separate OS windows exist on X11 / Win32 only. One OS window per process on Android / iOS.
- macOS: moving a window to a screen with a different backing scale is picked up only on the next layout.
- Native dialogs block the app's windows while open. No tray icons, notifications, custom image cursors,
  multi-file selection, or system web view.
- `applyTheme` adds a style sheet per call (the style engine has no sheet removal).
- In-window menus built from `MenuDesc` show `[x]` text for checkable items and do not grey disabled ones.
- These are native backends, not the SDL3 route decision D1 recommended for macOS / iOS / Android.

### Text

- Cooked fonts (`FUSE_UI_TEXT=COOKED`, `.fusefont`), MSDF glyphs, CBDT / sbix and COLRv1 colour fonts.
- Variable-font axes (default instance only), CFF2.
- Fallback-chain faces (other scripts, emoji) are not restyled under bold / italic.
- `letter-spacing` / `line-height` are ignored by the text edit widgets; no `font` shorthand.
- i18n: no date or currency formatting, no native digit sets, no string-extraction CLI; plural tables are
  hand-written, not generated from CLDR.
- Bold / italic frames have no zero-allocation gate.
- The glyph atlas is fixed at 1024x1024; very large text at scale 3 or more can overflow it.

### Drawing

- No native GPU path or gradient op: every path op is re-baked on the CPU per pack. Existing widgets that draw
  curves from line segments (chart series, node-graph links, curve editor, dial arcs) were not moved to paths.
- Built-in icons are only exposed; no hand-drawn widget mark was replaced.
- SVG: `text`, `use`, `image`, `clipPath`, `mask`, filters and `<style>` sheets are not drawn; group `opacity`
  multiplies fill and stroke alpha; radial focal point ignored; about 65 colour names.
- `Painter`: images under rotation / skew / mirroring are rejected; text under rotation or skew is drawn upright.
- No SVG / icon cooker, no backdrop-blur GPU pass, no custom shaders, no damage-driven incremental GPU present.
- The game module rasterises its draw list at scale 1 only.

### Interaction and widgets

- Touch: one-finger pan only; a pan that starts on a list or table row selects it; a slider inside a scroll area
  always wins the drag. Drag auto-scroll in views is one step per pointer move.
- Tree views have no drag and drop; a row drag carries only the pressed row, not the multi-selection.
- Transitions do not follow inherited values into descendants; no `@keyframes` in style sheets.
- RTL does not mirror the inside of self-painting widgets or scroll origins.
- Docks: no tab groups inside a floating dock; floating docks are panels inside the window, not OS windows; a
  destroyed dock area does not destroy its floating docks.
- Model / view: rows are single-line and fixed-height; a filtered or sorted proxy rebuilds fully on any source
  change; tree selection is by visible row (collapsing deselects); `SqliteModel` is not async; the combo-box cell
  editor commits on Enter / Tab / click-away, not on picking from the popup.
- A chart flame graph exists; a dedicated profiler model does not. Message box, wizard, popover, toast and file
  dialog cannot be created from a `.fuseui` document.
- The Designer and the Lua bindings were not extended for the new style properties.
- Side tables `TreeFold`, `DockDrag` and the game `richBindings` still key on the `UiContext` address (compared,
  never dereferenced): a new context at the same address can match a stale entry.
- Accessibility: no native adapters; tree item ids above about 32 million fall back to visible-row keys; menu
  shortcuts are not in the snapshot.
- The interaction golden exists for the MINIMAL text profile only. `widget_gallery` page gates check layout, not
  that each widget paints.

### Engine integration (plan items untouched)

- `project.json` schema 2 / `AppPresent`, `demo_hybrid_hud` on a UI context, `UiSurface` depth-tested rendering,
  the Qt editor replacement (UI9) beyond what was already in the tree, binary-size and Android startup gates.
