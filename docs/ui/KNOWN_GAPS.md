# FUSE UI — not done and not verified

Status as of 2026-11-12, branch `ccr-111d9b83-2iujo8`. This lists what is missing or has never been exercised, so
nobody mistakes "compiles in CI" for "works on a device". Phase-by-phase detail is in
[`../plans/FUSE_UI_PLAN.md`](../plans/FUSE_UI_PLAN.md) §9; usage is in [`README.md`](README.md).

Several gaps below (cooked / MSDF / variable fonts, the icon cooker, GPU blur, damage present, custom
shaders, `ProjectHost`) have an implementation in the tree that no target builds yet: the UI code of the merged
branch `claude/magical-bassi-815825`. They stay gaps until ported; [`archive/PORT_magical_bassi.md`](archive/PORT_magical_bassi.md)
says where each one is and what blocks it.

The merged branch `gui-framework` is in the same position: part of its `Extras/` module is live (`fuse_ui_os`,
`fuse_ui_extras`: undo, declarative bindings, PDF, desktop helpers, tray, web view, charts, 3D data visualisation);
everything else of it (a second Wayland backend whose drag-out and subsurface tool windows are not ported, OS drag and drop, `@keyframes` and the other CSS engine work, GPU
paths / filters / HDR, more widgets, rich text, SVG documents, media) is in the tree unbuilt and listed in
[`archive/PORT_gui_framework.md`](archive/PORT_gui_framework.md), which also says which of the two branches'
implementations to port where both have one. Its FORGE submodule was not taken.

## What has been verified

| Platform | How | Result |
|---|---|---|
| Linux (UI standalone) | Local build + ctest | 208/208 (MINIMAL text) after the `gui-framework` merge; before the two branch merges 178/178 (MINIMAL), 177/177 (FULL). FULL not re-run since |
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
  The same holds for everything in `App/src/native_platform_win32.inl` (MinGW-w64 13 compile and link only): OS tool
  windows, custom chrome (`WM_NCCALCSIZE` / `WM_NCHITTEST`), OLE drop target and `DoDragDrop`, clipboard images /
  registered formats, `IFileDialog` / `ChooseColorW` / `MessageBoxW`, and the tray balloon of `TrayIcon::showMessage`.
- **Apple platform code has never executed**: Cocoa and UIKit windows, `NSTextInputClient` IME, native menu bar,
  cursors (diagonal-resize and busy cursors use private `NSCursor` selectors behind `respondsToSelector`), window
  management, theme observer, file panels, HiDPI layout, touch path on iOS.
- **Android platform code has never executed**: NativeActivity lifecycle, surface loss / restore, touch, JNI
  clipboard and key-character lookups, soft keyboard, HiDPI buffer geometry.
- **X11 window-manager features** (maximise, fullscreen, minimise, activate) were only exercised under Xvfb with no
  window manager, i.e. as "state stays Normal". The same for custom chrome: the `_NET_WM_MOVERESIZE` hand-over and
  `_MOTIF_WM_HINTS` were never seen by a window manager (the gate runs the backend's own move / resize), and OS tool
  windows were never stacked or focused by one. XDND and the clipboard formats were tested between two connections
  of the same process, not against GTK / Qt applications.
- **Android saved state** (`APP_CMD_SAVE_STATE` -> `Application::saveState`, restored by `show()`) is cross-compiled
  only. **`Presenter::vsynced`** is set for a FIFO Vulkan swapchain, which no gate here can create.
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
- **`fuse_ui_os` / `fuse_ui_extras`** (from `gui-framework`): off by default on Windows and Apple platforms, where
  they were never built (MinGW-w64 syntax check only). On Android and FreeBSD they are cross-built, no gate run. The
  web view gate passed without a web engine installed (it skips inside the test); URL opening and printing were
  checked as command lines only; the tray and the native notifications (`desktop::Notifier`,
  `TrayIcon::showMessage`) only against a private `dbus-daemon` with an in-process stand-in for the tray host /
  notification service, never against a desktop's own. The charts
  were not compared image by image with the branch's renderer. Nothing GPU-side of that branch is built.
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

- Backends do not feed the newer input APIs: pinch / rotate (only the primary touch is forwarded on Android and
  iOS), pen pressure / tilt. OS file / text drops and drags out of the app exist on X11 (XDND) and Win32 (OLE,
  not run) only: not on macOS, iOS, Android or Wayland. Out of the app only `kDragText` / `kDragFiles` travel (no
  custom MIME types, no drag image for the other application); drops larger than one X property (INCR) are refused;
  the other application is always answered "copy".
- Android: no IME string commits (needs a Java `InputConnection`), no pointer icons, no system file picker, no
  refresh-rate query; theme-change notification needs `android:configChanges="uiMode"`.
- No gamepad backend in `fuse_app` (the `game_menu` sample takes pad input from a script or the host).
- X11: one scale per connection. XSETTINGS (DPI, theme, cursor theme) is read from the settings
  manager that owns the selection when the platform starts; one that starts later is not noticed. No XIM pre-edit
  (text input is `XLookupString`: Latin-1, no input methods), no RandR change notification, no clipboard `MULTIPLE`
  target, no hand-over to a clipboard manager at exit.
- Wayland (`App/src/native_wayland.cpp`, `native_wayland_platform.cpp`; gates `fuse_app_wayland`,
  `fuse_app_wayland_seat`):
  - Run against headless weston 13 (desktop shell, output scale 1 and 2) and against the test's own in-process
    compositor. It has not run on GNOME (mutter), KDE (kwin), sway / wlroots or on real hardware; the seat paths
    (pointer, keyboard, touch, clipboard, drops, IME, fractional scale, cursor shapes) have only met the in-process
    compositor, which checks the wire protocol and not a desktop's behaviour.
  - Decorations: server side through `xdg-decoration` only. GNOME does not offer it, so windows there have no title
    bar or borders (no client-side frame is drawn and libdecor is not used). `wl::beginMove` / `beginResize` exist for
    a client-side frame; nothing calls them yet.
  - No drag from the app out to other clients (`wl_data_device.start_drag`), no primary selection on
    `NativePlatform` (the protocol layer has it), clipboard is text only, no tablet tools, no `xdg-activation`
    (`AppWindow::activate()` reports false), no `xdg_popup.reposition` (a popup that moves is made again), no
    tool-window API on `NativePlatform` (the protocol layer has child toplevels).
  - By protocol: no window position (`setPosition` false, `position()` and `screenOrigin` stay 0), minimise has no
    state (`mode()` never reports `Minimized`), `postClick` is delivered in-process, `capture()` reads the last
    buffer the app committed, not the screen.
  - The Vulkan presenter on `VK_KHR_wayland_surface` was run with lavapipe only (a private build against an unpacked
    Mesa; the default build on the gate machine has no Vulkan loader, so the gate's Vulkan part does not run there).
    No frame-callback throttle on the Vulkan path (the swapchain's FIFO mode paces it).
  - Key repeat runs on the event pump's timeout; keys held while the loop is stalled repeat at most 8 times per
    wake-up.
- Popups as separate OS windows exist on X11 / Win32 / Wayland (`xdg_popup`). One OS window per process on Android /
  iOS.
- macOS: moving a window to a screen with a different backing scale is picked up only on the next layout.
- Native dialogs block the app's windows while open. No custom image cursors. Multi-file selection, colour and
  message dialogs exist only as the window-less functions (zenity / kdialog on Linux / BSD, Win32 not run, nothing
  on macOS / iOS / Android). `fuse_app` itself has no tray icon, notification or web view API; those exist in
  `fuse_ui_extras` (verified on Linux only) and are not tied to `fuse_app` windows. Native notifications
  (`desktop::Notifier`) are D-Bus only; Windows and macOS go through the helper programs, without click callbacks
  (except the Win32 tray balloon, not run).
- OS tool windows for floating docks: X11 and Win32 only, off by default. They are not hidden when the main window
  is minimised or hidden, have no task-bar / pager presence of their own, and on X11 are override-redirect (no
  window-manager stacking; they stay above other applications' windows).
- Custom window chrome: X11 and Win32 only (the frame kind is ignored elsewhere). No snap-layout flyout
  integration beyond what `HTMAXBUTTON` gives, no system menu, no shadow for `Borderless` windows on X11.
- Saved state covers nodes with an id only and the widget kinds listed in `AppWindow::saveUiState`; model / view
  selections, tree expansion, dock layouts (`dockSaveLayout`) and splitter positions are the app's to add through
  `StateHooks`. `restoreWindowState` after `show()` does not resize the OS window (restore before showing).
- Clipboard: Win32 has no delayed rendering (lazy items are made when the clipboard is set); images on macOS / iOS /
  Android are not supported.
- `applyTheme` adds a style sheet per call (the style engine has no sheet removal).
- In-window menus built from `MenuDesc` show `[x]` text for checkable items and do not grey disabled ones.
- These are native backends, not the SDL3 route decision D1 recommended for macOS / iOS / Android.

### Text

- Cooked fonts (`FUSE_UI_TEXT=COOKED`, `.fusefont`), MSDF glyphs, CBDT / sbix and COLRv1 colour fonts.
- Variable-font axes (default instance only), CFF2.
- Fallback-chain faces (other scripts, emoji) are not restyled under bold / italic.
- `letter-spacing` / `line-height` are ignored by the text edit widgets; no `font` shorthand.
- i18n: dates, times, currencies and the string-extraction CLI exist since the `claude/magical-bassi-815825` merge
  (`fuse_ui_l10n_lib`, `fuse_ui_extract_strings`, gate `fuse_ui_l10n`). Still missing: native digit sets, date
  skeletons, accounting formats; plural tables are hand-written, not generated from CLDR, and exist twice
  (`fuse/text/i18n.hpp` and `fuse_ui_l10n_lib`).
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
- Docks: no tab groups inside a floating dock; floating docks are panels inside the window unless the host turns on
  OS tool windows (`NativeOptions::osWindows`, X11 / Win32); a destroyed dock area does not destroy its floating docks.
- Model / view: rows are single-line and fixed-height; a filtered or sorted proxy rebuilds fully on any source
  change; tree selection is by visible row (collapsing deselects); `SqliteModel` is not async; the combo-box cell
  editor commits on Enter / Tab / click-away, not on picking from the popup.
- The `chart` widget has line, bar, area, scatter and flame kinds; the richer charts are a separate library
  (`fuse::ui::charts` in `fuse_ui_extras`) with its own `chartView` widget, not reachable from `.fuseui` documents.
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
