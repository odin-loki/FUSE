# FUSE UI — not done and not verified

Status as of 2026-11-13, branch `ccr-111d9b83-2iujo8`. This lists what is missing or has never been exercised, so
nobody mistakes "compiles in CI" for "works on a device". Phase-by-phase detail is in
[`../plans/FUSE_UI_PLAN.md`](../plans/FUSE_UI_PLAN.md) §9; usage is in [`README.md`](README.md).

Several gaps below (GPU blur, damage present, custom shaders) have an implementation in the
tree that no target builds yet: the UI code of the merged
branch `claude/magical-bassi-815825`. They stay gaps until ported; [`archive/PORT_magical_bassi.md`](archive/PORT_magical_bassi.md)
says where each one is and what blocks it.

The merged branch `gui-framework` is in the same position: part of its `Extras/` module is live (`fuse_ui_os`,
`fuse_ui_extras`: undo, declarative bindings, PDF, desktop helpers, tray, web view, charts, 3D data visualisation);
everything else of it (a second Wayland backend whose drag-out and subsurface tool windows are not ported, `@keyframes` and the other CSS engine work, GPU
paths / filters / HDR, more widgets, rich text, SVG documents, media) is in the tree unbuilt and listed in
[`archive/PORT_gui_framework.md`](archive/PORT_gui_framework.md), which also says which of the two branches'
implementations to port where both have one. Its FORGE submodule was not taken.

## What has been verified

| Platform | How | Result |
|---|---|---|
| Linux (UI standalone) | Local build + ctest | After all ports (2026-11-13): MINIMAL 327/327 (4 skipped: no Vulkan device, FULL-only text gates), FULL 298/298 (2 skipped), COOKED 326/326 (7 skipped: real-font gates, FULL-only gates). Before the two branch merges 178/178 (MINIMAL), 177/177 (FULL) |
| Linux (FUSE-UI editor shell, in tree, `-DFUSE_EDITOR_SHELL=fuse -DFUSE_BUILD_VULKAN=OFF`) | Local build + `ctest -L fuse_editor_ui` under Xvfb | Builds and links; 78 entries: 72 passed, 4 skipped (Vulkan), 2 failed (one gate under two names, `ui_frame`: its 8 ms full-window CPU raster budget measured 7.5 to 9.0 ms here). The rest of the in-tree suite was not run in that configuration |
| Linux (whole engine, in-tree) | `fuse-umbrella-linux.yml` | 1245 of 1247; the two failures also fail on `main` (see below) |
| Android arm64 (NDK r26d) | `fuse-core-android.yml` + local cross-build | Builds and links, apps as `lib<name>.so` |
| macOS | `fuse-core-ios.yml` (macOS job, MoltenVK headers) | Builds; headless gates pass |
| iOS simulator | `fuse-core-ios.yml` (iOS job) | Builds and links |
| FreeBSD 14 x86-64 | `fuse-ui-freebsd.yml` in a FreeBSD VM | Builds; headless gates pass |
| Windows (MSVC release / profile / shipping / debug, clang-cl) | `fuse-windows-native.yml`, runs 43370404838 and 43377707488 | All five build with warnings as errors and run ctest (747 tests; shipping 670). Failing: `fuse_core_ui1_text_input` (all five) and `fuse_ui_bench_10k` (all but debug); see "Known failures" |
| Windows + Qt 6 editor | same run | Builds for the first time (it does not on `main`); `ctest -R editor`: 76 of 81, see "Known failures" |
| Windows + Vulkan (build only) | run 43377707488 | Builds |


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
  9-slice, clip-path, layer, transform, filter and mask groups are pre-rasterised on the CPU and packed as images
  (translation groups as moved kernel commands), checked only against a C++
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

- `fuse_core_ui1_text_input` fails on Windows (every MSVC job): `IMM32 composition string -> TextComposition`
  (`ImmSetCompositionStringW` succeeds on the hosted runner, the composition read back is empty). It is an engine-core
  test (`Source/FUSE/Core/tests`), not the UI module, and fails the same way on `main` (run 43323981954).
- Qt editor gates on Windows (never run there before; the Qt editor is outside the UI module):
  `fuse_editor_qt_context_menu_dpr1_5` (QMenu placed elsewhere than computed), `fuse_editor_qt_idle_frames` and
  `fuse_editor_qt_shell_assets` (both also fail on Linux on `main`), `fuse_editor_qt_e20_inspector`,
  `fuse_editor_qt_e20_reparent`. Not investigated.
- `fuse_ui_bench_10k` misses its wall-clock budget on the hosted Windows runners (0.5 ms for one dirty panel in a
  10k-node tree; measured 0.50 to 0.94 ms there, 0.38 to 0.43 ms on the Linux development machine). It fails on `main`
  too. The budget is enforced there on purpose; nothing was relaxed.
- MSVC Debug only: the "zero heap allocations in steady frames" checks of `fuse_ui_zero_alloc_steady`,
  `fuse_ui_style_motion`, `fuse_ui_mv_selection_1m`, `fuse_ui_mv_async`, `fuse_ui_vector_steady`,
  `fuse_ui_port_zero_alloc`, `fuse_ui_css_animations` (`UI_CHECK_NO_ALLOC`) and of `fuse_editor_b6_idle_frame_gates`
  print the count and do not fail: MSVC's debug STL allocates a `_Container_proxy` per container object. This is the
  engine's existing rule (`Core/tests/fuse_test_msvc_debug_stl.hpp`); MSVC Release / Profile / Shipping and all other
  compilers enforce them. Whether every one of the counted allocations is such a proxy was not checked one by one.
- Failing on `main` as well and left alone (outside the UI module):
  - Linux: `fuse_vulkan_validation_gate` (VUID-VkImageViewCreateInfo-image-04441), `fuse_runtime_steady_state_alloc`
    (editor phase allocates), Qt gates `fuse_editor_qt_idle_frames`, `fuse_editor_qt_lighting`,
    `fuse_editor_qt_shell_assets`.
  - Windows: the build errors `main` has in `EditorShell` (`meshlet_builder.hpp` not found, deprecated
    `std::filesystem::u8path`) and in the Qt editor tests (`far`, discarded `[[nodiscard]]`) are fixed on this
    branch; see "What has been verified" for what the Windows jobs do now.
- `fuse_world_extract_synthetic` (Tools/FUSE/WorldExtract) failed once on this branch and passed on the next run;
  cause not investigated.
- Wall-clock gates (label `perf`: `fuse_ui_bench_10k`, `fuse_ui_modelview_1m_rows`, `fuse_ui_mv_selection_1m`,
  `fuse_ui_mv_tree_1m`, `fuse_ui_vector_steady`, `fuse_app_hello_startup`) are excluded on the FreeBSD and macOS CI
  VMs, so their non-timing checks do not run there either. `fuse_ui_bench_10k` can also miss its budget on a loaded
  Linux machine.

## Not implemented

### Platform

- Backends do not feed the newer input APIs: pinch / rotate (only the primary touch is forwarded on Android and
  iOS), pen pressure / tilt. OS file / text drops exist on X11 (XDND), Wayland (`wl_data_device`) and Win32 (OLE,
  not run); drags out of the app on X11 and Win32 only. Neither on macOS, iOS or Android. Out of the app only `kDragText` / `kDragFiles` travel (no
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
  - Decorations: server side through `xdg-decoration` where offered. Elsewhere (GNOME, weston) a System-frame
    window gets the toolkit's client-side chrome when shown (`NativeOptions::clientDecorations`, on by default):
    `titleBar()` as the root's first child plus 6 px resize edges. That bar is a widget row, not a desktop title
    bar: no shadow or rounded corners, no window menu (`xdg_toplevel.show_window_menu`), no desktop theme or button
    layout, it takes its 32 px out of the window's size, it assumes the root lays its children out in a column, and
    it is not added when the app already set `AppWindow::chrome().caption`. libdecor is not used. The compositor's
    answer is read once, at `show()`; a compositor that changes the decoration mode later is not followed.
  - Custom chrome (`WindowFrame::Custom` / `Borderless`, and the fallback above): caption and edge presses are
    handed to the compositor (`xdg_toplevel.move` / `resize`) from `wl_pointer` presses only: a touch on the caption
    does not drag the window. The requests with a real press serial were checked against the in-process compositor
    (it records them); no compositor has actually moved or resized a window by them, since weston's headless
    backend has no pointer and injected presses carry no serial.
  - Clipboard formats (`setClipboard`, images, custom MIME types, lazy items) and the primary selection
    (`zwp_primary_selection_v1`) were exchanged with the in-process compositor only, not with another client
    through a desktop compositor. Setting a selection needs an input serial: a window that never had input may be
    ignored by the compositor (the call still returns true). A lazy item that refuses stays in the offer and
    answers with no data. weston has no primary selection: `setPrimarySelectionText` returns false there.
  - No drag from the app out to other clients (`wl_data_device.start_drag`; `externalDragActive()` is always
    false). No OS tool windows: `NativeOptions::osWindows` is ignored, floating docks stay in-window panels and
    `toolWindowCount / toolWindow / toolWindowScreenRect / captureToolWindow` answer 0 / false (the protocol layer
    can make child toplevels, `wl::createTool`, but a client cannot place them, so a floating dock could not follow
    its title drag or the main window; wiring that needs the dock widget to hand its drag to `wl::moveChild`).
  - No XSETTINGS equivalent (the settings portal is not read): the theme comes from `$GTK_THEME` / `gsettings`
    once, the scale from the compositor, `cursorThemeSize()` is `$XCURSOR_SIZE` or 24, and 0 when the compositor
    draws the cursors (`wp_cursor_shape_v1`). Saved window state restores size and mode, not the position.
  - No tablet tools, no `xdg-activation` (`AppWindow::activate()` reports false), no `xdg_popup.reposition` (a
    popup that moves is made again).
  - By protocol: no window position (`setPosition` false, `position()` and `screenOrigin` stay 0), minimise has no
    state (`mode()` never reports `Minimized`), `postClick / postPointerMove / postPointerButton / postClose` are
    delivered in-process, `capture()` reads the last buffer the app committed, not the screen.
  - The Vulkan presenter on `VK_KHR_wayland_surface` was run with lavapipe only (a private build against an unpacked
    Mesa; the default build on the gate machine has no Vulkan loader, so the gate's Vulkan part does not run there).
    No frame-callback throttle on the Vulkan path (the swapchain's FIFO mode paces it; `Presenter::vsynced` is set
    for it). `wl_shm` windows are not `vsynced`: `setMaxFrameRate` applies to them, on top of the frame-callback
    throttle (that combination has only the headless `fuse_app_frame_pacing` gate behind it).
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
- OS tool windows for floating docks: X11 and Win32 only (not Wayland, see above), off by default. They are not hidden when the main window
  is minimised or hidden, have no task-bar / pager presence of their own, and on X11 are override-redirect (no
  window-manager stacking; they stay above other applications' windows).
- Custom window chrome: X11, Wayland and Win32 (the frame kind is ignored on macOS / iOS / Android). No snap-layout flyout
  integration beyond what `HTMAXBUTTON` gives, no system menu, no shadow for `Borderless` windows on X11.
- Saved state covers nodes with an id only and the widget kinds listed in `AppWindow::saveUiState`; model / view
  selections, tree expansion, dock layouts (`dockSaveLayout`) and splitter positions are the app's to add through
  `StateHooks`. `restoreWindowState` after `show()` does not resize the OS window (restore before showing).
- Clipboard: formats beyond text on X11, Wayland and Win32. Win32 has no delayed rendering (lazy items are made when the clipboard is set); images on macOS / iOS /
  Android are not supported.
- Themes: the dark theme has five text / background pairs below WCAG 4.5:1, frozen by its goldens and pinned in
  `fuse_ui_theme_contrast` (white on `--accent` 3.33: primary buttons, check marks, the selected calendar day; on
  `--danger` 3.91 and `--warning` 2.27: message box badges; code editor line numbers 3.00). Light and high contrast
  have none (the ported widgets add two more dark pairs: placeholder text, the text colour at half alpha, is 4.47 in
  a field and 3.71 in an editable combo box). `SystemTheme::highContrast` is set on X11 only, from the theme's name
  (XSETTINGS `Net/ThemeName` live, `$GTK_THEME` at start: a name containing "HighContrast" / "high-contrast"; not
  from the `gsettings` query, and nothing on Wayland beyond `$GTK_THEME`); Win32 `SPI_GETHIGHCONTRAST` and the scheme
  colours and macOS increase-contrast are not queried: hosts set it through
  `Application::setSystemTheme`, and the palette is the default one unless the app calls `widgets::setTheme` with its
  own. High contrast does not strip shadows / blur / gradients of application sheets (the branch's
  `StyleEngine::setForcedColors` is not ported); it only clears the theme's own. The light and high-contrast goldens
  exist for the MINIMAL text profile only (also those of the ported widgets, `port_<scene>_<light|contrast>.png`).
  Data colours are not themed (colour picker, flame / pin / clip palettes, the items of a `GraphicsScene`: the
  defaults of `GraphicsItem::fill / stroke` are a blue fill and a white outline, made for the dark theme), and neither
  are the game HUD widgets of `fuse_ui_game` (they draw over a game frame in their own colours). In high contrast
  docked docks have no outline (only floating ones and the dock tab underline), and a floating dock's title strip
  covers its top border; the resize grip and close glyph of an MDI sub-window stay the text colour at 45 % / 90 %.
  The editable combo box draws its focus ring 1px wide in every theme (high contrast: in the hot-light colour).
  `widget_gallery` has no theme switch (it starts in the dark theme; `AppWindow::setFollowSystemTheme` is not on).
  The existing dark dialogs / wizard goldens are captured with the focus parked, because they predate focus rings
  and their FULL-profile twins cannot be re-rendered without the fetched text stack; the light / high-contrast
  goldens and `fuse_ui_widget_golden_controls` show the rings.
- Rich text: one model (`rich::RichDocument`), two widgets: the Extras `richtextedit` edits it, the widgets `richText`
  view shows it through markup that flattens block formats (no alignment, lists as text, headings as size + bold).
  The editor has no images, tables, per-run font family, line spacing or BiDi caret movement; the clipboard carries
  plain text; undo keeps whole-document snapshots per command (fine for pages, not for books).
- Extras still parked: SVG documents (`extras/svg.hpp`: `<text>`, `clipPath`, `mask`, patterns, focal radial gradients,
  filters; to be folded into main's `SvgImage`), system video and audio output (`extras/media.hpp`), the Designer
  additions of both branches. `l10n::parsePo` does not rewrite `%1`-style positional arguments of singular strings. `PdfDocument::renderDrawList` writes a gradient as the
  mean of its stops and skips nine-patches and layer effects.
- `fuse_ui_vector_steady` (label `perf`) has a wall-clock budget (a 2000-point stroked series at 1080p < 8 ms) that the
  port machine misses under load: it measures 3.3 to 7.8 ms alone, the same on the base commit, and fails in a parallel
  `ctest` run with a load average above 30. It passes alone and in a serial run.
- `fuse_app_binary_size` has a budget only for Linux x86-64 GCC Release MINIMAL in the standalone tree; the HUD binary
  size gate of the branch needs the COOKED text profile and is not registered.
- In-window menus built from `MenuDesc` show `[x]` text for checkable items and do not grey disabled ones.
- These are native backends, not the SDL3 route decision D1 recommended for macOS / iOS / Android.

### Text

- Cooked fonts, MSDF glyphs, CBDT / sbix and COLRv1 colour fonts and variable fonts are live (ported from the
  `claude/magical-bassi-815825` branch). What is missing around them:
  - GPU: `packDrawList` skips loose colour-image (`kGlyphColor`) and MSDF (`kGlyphMsdf`) glyph commands (counted in
    `GpuDrawData::skippedGlyphs`); only the CPU rasteriser draws them. Inside a group the packer bakes on the CPU
    (layer, clip path, non-translation transform, filter, mask) they are in the baked image, so the same text can be
    visible inside such a group and missing next to it. The branch's shader work for them was not ported (no Vulkan
    device to verify on); `tests/test_text_color.cpp` still carries the branch's GPU parity code behind
    `FUSE_TEXT_COLOR_HAVE_VK`, which nothing defines and which does not match this tree's `GpuDrawData`.
  - MSDF under a CSS `transform`: the group's layer is rasterised 1:1 in the node's own space and then resampled, so
    an MSDF glyph is as soft as a coverage glyph when a transform scales it up. `setMsdfThreshold` looks at
    `font-size` x render scale only, not at the transform. (Rendering the layer at the transform's scale would fix
    both; not done.)
  - `FUSE_UI_TEXT=COOKED`: MINIMAL shaping only (no GSUB / GPOS, no bidi), one face and one instance per pack, no
    synthetic oblique, no face names / weight in a pack, no font-feature-settings. A cross-compiled COOKED build
    needs a host `fuse_text_cook` (`FUSE_TEXT_COOK_HOST`); none was built for Android or FreeBSD.
  - Variable fonts: CFF2 outlines (variable `.otf`) are not supported; `font-weight` is not animatable
    (`font-stretch` is); italic uses the synthetic oblique, not the `ital` / `slnt` axis.
  - The Windows registry font list and the `.ttc` named-instance listing were compiled nowhere here (Windows) or
    not exercised against Segoe UI Variable / Bahnschrift (those checks only run where the fonts exist).
- Fallback-chain faces are restyled only under a real bold primary (bold face, variable instance,
  `Font::emboldened()`); the UI's synthesised bold of a static family leaves them regular, and nothing restyles them
  for italic.
- `letter-spacing` / `line-height` are ignored by the text edit widgets; no `font` shorthand.
- i18n: dates, times, currencies and the string-extraction CLI exist since the `claude/magical-bassi-815825` merge
  (`fuse_ui_l10n_lib`, `fuse_ui_extract_strings`, gate `fuse_ui_l10n`). Still missing: native digit sets, date
  skeletons, accounting formats; plural tables are hand-written, not generated from CLDR (one table since
  `fuse/text/i18n.hpp` was folded into `fuse_ui_l10n_lib`). The number rows that came from `i18n.hpp` (bn, th, ca,
  gl, id, da, el, hr, sl, sr, ro, vi, nb, nn, no, sk, be, bg, hu, lt, lv, et, kk and some regions) carry a plain
  `-` / `%`: their percent and minus forms were not checked against CLDR.
- Bold / italic frames have no zero-allocation gate.
- The glyph atlas is fixed at 1024x1024; very large text at scale 3 or more can overflow it.

### Drawing

- No native GPU path or gradient op: every path op is re-baked on the CPU per pack. Existing widgets that draw
  curves from line segments (chart series, node-graph links, curve editor, dial arcs) were not moved to paths.
- Built-in icons are only exposed; no hand-drawn widget mark was replaced.
- SVG: `text`, `use`, `image`, `clipPath`, `mask`, filters and `<style>` sheets are not drawn; group `opacity`
  multiplies fill and stroke alpha; radial focal point ignored; about 65 colour names.
- `Painter`: images under rotation / skew / mirroring are rejected; text under rotation or skew is drawn upright.
- Icon packs (`.fuseicons`, `fuse_ui_icon_cook`, `IconSet`, the `icon` style property) are live. Missing: `:name:`
  icon markup in menu item text, dimming of fixed-colour paints on `:disabled` nodes, the designer's icon picker,
  `fuse_ui_cook --icons`; `icon` is honoured by `button`, `toolbutton` and the `icon` widget only (menu items, tabs,
  tool-box pages, list / combo items and the ported `widgets/extra.hpp` widgets take no icon name: combo and list
  items take image indices, the others have no icon slot); packs of the merged branch (coverage atlases, versions
  1 / 2) do not load.
- No backdrop-blur GPU pass, no custom shaders, no damage-driven incremental GPU present.
- The game module rasterises its draw list at scale 1 only.

### Style engine port (selectors, at-rules, positioning, effects, animations)

Ported from `gui-framework` onto this tree's engine as a subset; the branch's own implementation is not in the build.

- Cascade: no `!important` (parsed, reported, applied as a normal declaration); `@scope` proximity is not a cascade
  criterion; `@supports` is decided when the sheet is parsed; `@import`, `@font-face`, `@property` are skipped with a
  diagnostic. `background: <colour>` does not reset an earlier `background-image`.
- Selectors: attribute selectors see explicit attributes, `id` and (`~=`) classes, not widget values or text; no
  pseudo-elements, `:nth-child(... of S)`, `:nth-of-type`, `:enabled`. `:has()` on a compound that is not the subject
  (`.a:has(.b) .c`) restyles the whole tree on every relevant change; `:has()` matching walks the anchor's subtree.
- Queries: `@container` answers size features only (no `style()` queries, no container / viewport units); a container
  must be an ancestor. `@media` has no `print` output, `update`, `color-gamut` or `forced-colors`.
- Easing: `cubic-bezier()` and `steps()` are not parsed (the named curves only).
- Positioning: `z-index` orders siblings under their parent; it does not lift a node over its parent's siblings
  (every node is a stacking context). An absolute node is still clipped by `overflow` ancestors between it and its
  containing block, and without insets sits at the start of its parent's content box (not its flow position). A fixed
  node ignores transformed ancestors and their opacity. `sticky` needs an `overflow: scroll` ancestor. `relative` /
  `sticky` insets are ignored under `anchor` / `absolute` layouts (the insets place the child there). Subgrid shares
  columns only (this grid has no row tracks) and one level deep. No flex `order`, `align-content`, row / column gap
  pair, masonry.
- Transforms: 2D only (no `perspective`, `rotateX/Y`, `translateZ`, `preserve-3d`, `backface-visibility`). Other than
  translations they are drawn by resampling a layer: text softens when scaled up, scales well below 1 alias, and each
  such node costs a layer per repaint. The overlay scrollbars and touch panning of a scroll container inside a rotated
  or scaled node are hit in untransformed space. `rect()`, the layout dump and accessibility bounds stay the
  untransformed layout boxes. Individual `translate` / `rotate` / `scale` properties are not parsed.
- Filters: up to 4 functions and one `drop-shadow` per list; no `url()` (the branch's SVG filter programs,
  `Core/src/filter_program.cpp`, stay parked); `backdrop-filter` is still blur only. Mismatched lists switch
  half-way instead of interpolating.
- Clip paths: basic shapes only (`inset`, `circle`, `ellipse`, `polygon` up to 8 points); no `path()`, `url()`,
  reference boxes (the branch's `Core/src/css_masking.cpp` stays parked). Masks: one gradient layer over the border
  box; no images, `mask-size / position / repeat / composite`.
- Gradients (`background-image`, `mask-image`): up to 4 stops, percentages only; corner keywords use the 45 degree
  diagonals; radial is always the centred farthest-corner ellipse; no conic, repeating or layered images.
- Animations: up to 4 CSS animations per node; no `animation-composition`, `animation-timeline` (scroll / view
  timelines), `iterationStart`, per-animation events other than `AnimationFinished`; a finished scripted animation
  without a forwards fill releases its handle. Keyframe `var()` reads `:root` tokens only. Documents (`.fuseui`) have
  no `"animate"` / `"order"` keys yet.
- GPU: every non-translation effect group is rasterised on the CPU each time it repaints (a large blurred area is
  slow), and none of it has run on a device (see above).
- Themes and the style engine: the widget theme's rules are outside every `@layer`, so layered application rules
  lose to them (as CSS says; there is no way to put the theme into a layer). Tokens (`:root { --x }`) are not
  layered and not scoped: the last sheet that defines one under `@media` conditions that hold wins; a token inside
  `@container` or `@scope` is treated as unconditional. `prefers-contrast` knows `more` and `no-preference` only
  (`less` / `custom` never match); `forced-colors` is not a feature. `setMediaTheme` is driven by
  `widgets::install` / `setTheme` and `AppWindow::applyTheme`; an application that loads its own theme sheet with
  `setThemeStyleSheet` has to call it itself. A high-contrast `SystemTheme` always reports `prefers-color-scheme:
  dark` (the default palette is the dark one).
- `font-variation-settings` / `font-feature-settings` and `icon` do not interpolate (they switch half-way in
  keyframes and at once in transitions); `font-weight` is not animatable.

### Interaction and widgets

- Touch: one-finger pan only; a pan that starts on a list or table row selects it; a slider inside a scroll area
  always wins the drag. Drag auto-scroll in views is one step per pointer move.
- Tree views have no drag and drop; a row drag carries only the pressed row, not the multi-selection.
- Transitions do not follow inherited values into descendants.
- RTL does not mirror the inside of self-painting widgets or scroll origins.
- Docks: no tab groups inside a floating dock; floating docks are panels inside the window unless the host turns on
  OS tool windows (`NativeOptions::osWindows`, X11 / Win32, not Wayland); a destroyed dock area does not destroy its floating docks.
- Model / view: rows are single-line and fixed-height; a filtered or sorted proxy rebuilds fully on any source
  change; tree selection is by visible row (collapsing deselects); `SqliteModel` is not async; the combo-box cell
  editor commits on Enter / Tab / click-away, not on picking from the popup.
- The `chart` widget has line, bar, area, scatter and flame kinds; the richer charts are a separate library
  (`fuse::ui::charts` in `fuse_ui_extras`) with its own `chartView` widget, not reachable from `.fuseui` documents.
- A chart flame graph exists; a dedicated profiler model does not. Message box, input dialog, wizard, popover, toast
  and file dialog cannot be created from a `.fuseui` document.
- Dock splits (`dockSplit`) are made from code: the dock drag still offers only the Left / Right / Bottom / Center /
  Tab zones, there is no "drop on a dock's edge to split" zone. Cancelling a drag restores a dock that was alone in
  a split pane; a tab group inside a pane is restored to its group and pane, not to a pane that collapsed meanwhile.
- Qt-parity options ported from `gui-framework` (line edit, completer, editable combo, spin box, tab bar): a widget
  gets the Qt behaviour only once an option is set on it (a plain spin box still takes no typed entry and does not
  auto-repeat). `regexValidator` has no fallback for backreferences / lookaround: such a pattern accepts everything.
  Not ported: tab position / elide / per-tab icons and tooltips, date / time edit sections, calendar options, tree
  drag and drop and multi-selection, table multi-row and cross-view drags, header resize modes and menu, list icon
  mode, splitter sizes, tool bar overflow / movable tool bars, MIME drag payloads (the branch code stays parked:
  `docs/ui/archive/PORT_gui_framework.md` §3.7).
- Extra widgets (`widgets/extra.hpp`): MDI sub-windows are panels inside their area (no maximise / minimise / OS
  windows, no keyboard window switching); the graphics view has no item rotation / grouping / per-item transforms and
  repaints the visible items every frame it is dirty (ellipses and polygons as vector paths, which the GPU packer
  pre-rasterises); the on-screen keyboard has one English QWERTY layout, learns its target while it paints (or
  from `virtualKeyboardAutoShow` / `setVirtualKeyboardTarget`) and a key press takes the focus off the field until
  the click puts it back (the field sees a focus-out / focus-in pair); the key sequence edit records one chord, not
  a sequence of up to four as QKeySequenceEdit does; the editable combo box's inline completion and the completer
  have no accessibility `autocomplete` / `controls` relations (main's snapshot has no such fields).
- In a dialog with a default button, Enter on a focused check box / radio / switch presses the default button
  instead of toggling it (Space still toggles), as in Qt; dialogs without a default button are unchanged.
- The Designer and the Lua bindings were not extended for the new style properties.
- Side tables `TreeFold`, `DockDrag` and the game `richBindings` still key on the `UiContext` address (compared,
  never dereferenced): a new context at the same address can match a stale entry.
- Accessibility: no native adapters; tree item ids above about 32 million fall back to visible-row keys; menu
  shortcuts are not in the snapshot.
- The interaction golden and the `fuse_ui_port_golden_*` goldens exist for the MINIMAL text profile only. `widget_gallery` page gates check layout, not
  that each widget paints.

### Engine integration (plan items untouched)

- `project.json` schema 2 / `AppPresent`, `demo_hybrid_hud` on a UI context, `UiSurface` depth-tested rendering,
  the Qt editor replacement (UI9) beyond what was already in the tree, binary-size and Android startup gates.
