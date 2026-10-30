# FUSE GUI framework — execution plan (gui-framework branch)

Status: active, 2026-10-30. Owner: Odin Loch.
G0 progress: FORGE submodule + `fuse_forge`, `fuse::ui::os` seam, `fuse_app` / `fuse_text` / X11 loader moved onto
it, `<fuse/gui.hpp>` + `fuse::gui`, `gui_quickstart` example; standalone UI build 69/69 ctest green on Linux (clang 18).
Companion to the long-form design in [`../plans/FUSE_UI_PLAN.md`](../plans/FUSE_UI_PLAN.md) (architecture, decisions D1–D7,
phase history) and the usage guide [`../ui/README.md`](../ui/README.md). This file is the short, binding plan for
finishing the GUI framework as a product every one of Odin's packages can depend on.

## 1. Scope rule (binding)

FUSE GUI implements **GUI concerns only**: windowing, surfaces and present, rendering (CPU raster + GPU draw list),
the widget tree, layout, input events (pointer, keyboard, touch, pen, IME), focus and navigation, styling and
theming, text shaping and rendering, accessibility, animation, model/view, dialogs, menus, drag and drop, high DPI.

It does **not** implement anything the OS layer or the C++ standard library already provides. Those come from
**FORGE** (`https://github.com/odin-loki/FORGE`, Odin's library) or from `std::`:

| Need inside the GUI | Provided by | Not written in FUSE GUI |
|---|---|---|
| Files, atomic writes, directories, file locks | `forge::os` (`os.hpp`, `fs.hpp`) | no `fopen`/`mkdir`/`flock`/`MoveFileEx` in GUI code |
| Environment, home / temp dirs, platform identity | `forge::os::getenv`, `home_dir`, `temp_dir`, `current()` | |
| Monotonic clock, sleep | `forge::os::monotonic_ns`, `sleep_ns` | |
| Threads, mutexes, condvars | `forge::os::thread/mutex/condvar` (or `std::`) | no GUI thread pool |
| Dynamic loading (libX11, libwayland, libvulkan, AccessKit…) | `forge::os::library` | no raw `dlopen`/`LoadLibrary` |
| Plain-text clipboard fallback (no window system bound) | `forge::os::clipboard` | |
| Local IPC (single-instance activation) | `forge::os::local_ipc`, `fs::file_lock` | |
| File watching (style / `.fuseui` hot reload) | `forge::os::file_watch` | |
| Process spawn (open URL, file manager) | `forge::os::process` | |
| SIMD for the CPU rasteriser / blur | `forge::lane` (runtime-dispatched intrinsics) | no hand-rolled per-ISA kernels |
| Logging | `forge::log` | |
| Containers, strings, formatting, `expected`, chrono | C++23 standard library | |

Removed from the GUI parity matrix (owned by FORGE / std): QtCore threads, QFile/QDir, QSettings storage backend,
QtNetwork, QTimer's OS part, QProcess, QString/containers, QJson parsing beyond the `.fuseui` document format.

Where GUI code needs one of these, it goes through **one thin seam**: `fuse/ui/os.hpp` (`fuse::ui::os` namespace)
— a handful of functions the GUI calls, implemented on FORGE by default. A package that embeds FUSE GUI with a
different OS layer replaces that one translation unit (`FUSE_UI_OS_BACKEND=custom`); nothing else in the GUI
touches the OS directly except the window-system backends themselves (Win32, X11, Wayland, Cocoa, UIKit,
Android), which are GUI concerns.

### FORGE integration

- Git submodule `vendor/forge` pinned to a FORGE commit (read-only from FUSE's point of view; FUSE never pushes to it).
- FUSE does **not** `add_subdirectory` FORGE's full build (AI runtime, GPU compute, generators). `cmake/FuseForge.cmake`
  defines `fuse_forge` (INTERFACE) with FORGE's header-only `core`, `os` and `lane` include roots plus the OS link libs
  FORGE's own `forge_os` target uses (`ws2_32 bcrypt` on Windows, `dl` on Unix).
- `-DFUSE_FORGE_DIR=/path/to/FORGE` overrides the submodule (use an existing checkout or a system package).
- FORGE is C++23; FUSE GUI is already C++23, so no dialect bridge is needed. On clang + libstdc++ 14 builds,
  add `-D__cpp_concepts=202002L` (same as FORGE's dev preset) so `std::expected` is visible.

## 2. Architecture (as built)

```
fuse_ui_public  (fuse/gui.hpp — single stable include, versioned)        <- what packages consume
  ├─ fuse_app / fuse_app_native   Application, AppWindow, loop, settings, native windows, present
  ├─ fuse_ui_widgets              ~90 widgets (Tier A–D), themes, platform feel, inspector
  ├─ fuse_ui_doc                  .fuseui declarative documents, bindings, cook
  ├─ fuse_ui_core                 UiContext: node tree, layout (flex/grid/stack/abs), style sheets, events,
  │                               focus, animation, draw list, CPU raster, damage tracking
  ├─ fuse_text                    font loading, shaping (MINIMAL stb / FULL HarfBuzz+FreeType), bidi, UAX#14/#29
  ├─ fuse_ui_render / ui_vk       GPU draw-list backend (Vulkan compute; lavapipe parity)
  ├─ fuse_ui_a11y                 accessibility tree + AccessKit bridge
  └─ fuse::ui::os seam ──────────► FORGE (forge::os, forge::lane, forge::log)  /  std::
```

Window-system backends (one per platform family) sit under `fuse_app_native` behind `NativeOptions` / `Presenter`.

## 3. Platform matrix (target vs today)

| Platform | Window + input | IME | Present | A11y | State |
|---|---|---|---|---|---|
| Linux X11 | native (runtime-loaded libX11) | XIM | Vulkan swapchain / CPU upload | AT-SPI via AccessKit | **done**, gated on Xvfb + lavapipe |
| Linux Wayland | native libwayland-client (runtime-loaded), xdg-shell, wl_shm | wl_keyboard+xkbcommon, client key repeat, text-input-v3 | Vulkan swapchain (`VK_KHR_wayland_surface`, GPU draw or CPU upload; gates `fuse_app_wayland_vk[_gpu]` on lavapipe), wl_shm CPU fallback | AT-SPI | **done (G2)**: window, input, DnD in+out, xdg_popup (`showPopup`), IME, key repeat; gated by in-process libwayland-server compositor test |
| Windows | native Win32 | IMM32 | Vulkan / GDI fallback | UIA via AccessKit | **done**, gated under Wine |
| FreeBSD | X11 backend (same code) | XIM | Vulkan / CPU | AT-SPI | builds expected; CI cross-compile in G1 |
| macOS | native Cocoa NSWindow + layer-backed NSView (`fuse/app/cocoa.hpp`) | NSTextInputClient | layer-hosting CAMetalLayer + Vulkan-on-Metal (`VK_EXT_metal_surface`, portability enumeration/subset; UNVERIFIED), CGImage CPU fallback | NSAccessibilityElement tree | **written, UNVERIFIED (G3)**: never compiled (no Apple SDK on Linux host); only `cocoa_map.hpp` tables tested |
| iOS | native UIKit UIScene + FuseUIView (`fuse/app/uikit.hpp`) | UITextInput with marked text (`uikit::MarkedText` -> textComposition/textInput, tested; UIKit glue UNVERIFIED) | CAMetalLayer sublayer + Vulkan-on-Metal (UNVERIFIED), CGImage CPU fallback | UIAccessibilityElement | **written, UNVERIFIED (G4)**: never compiled (no iOS SDK); `uikit::traits` tested |
| Android | NDK NativeActivity + `org.fuse.ui.FuseActivity` (`fuse/app/android.hpp`) | InputConnection (commit + composing) | Vulkan swapchain (`VK_KHR_android_surface`, GPU draw; emulator smoke `FUSE_EXPECT_VULKAN=1` passes on SwiftShader), ANativeWindow_lock CPU fallback | AccessibilityNodeProvider | **done (G4)**: cross-built with NDK r27c (arm64 + x86_64), Gradle-free APK, `App/android/run_emulator_smoke.sh` passes on an android-34 emulator (frame, tap, IME, a11y tree) |

Headless (offscreen CPU raster) works everywhere and is what the test suite uses.

## 4. Public API contract

- One umbrella header `#include <fuse/gui.hpp>`, one CMake target `fuse::gui`.
- `FUSE_GUI_VERSION_{MAJOR,MINOR,PATCH}`; within a major version, source compatibility is kept.
- Everything under `fuse::ui`, `fuse::app`, `fuse::text` reachable from that header is public; `*_internal.hpp` and
  `src/` are not.
- Value types and handles (`NodeHandle`, generation-checked) instead of raw owning pointers; no exceptions across the
  API (`std::expected` for fallible calls).

## 5. Qt parity matrix (GUI only)

| Qt module / feature | FUSE GUI | Status |
|---|---|---|
| QtGui windowing, QWindow, multi-window, popups | `fuse_app_native` | done (X11, Win32); live setTitle on X11/Win32/Wayland/Cocoa with `osTitle` readback |
| QPainter / 2D raster, paths, gradients, blur | draw list + CPU raster + Vulkan | done |
| High DPI, per-monitor scale | `dpiScale` plumbing | done desktop; mobile G4 |
| QtWidgets basic (button…lineedit) | Tier A | done |
| Containers, MainWindow, docks, menus, toolbars, dialogs | Tier B | done |
| Model/view, delegates, proxy sort/filter, 1M rows | Tier B/C/D | done |
| QSS styling, themes, palettes | `StyleSheet`, themes | done |
| Text: shaping, bidi, fallback, emoji, grapheme editing | `fuse_text` FULL | done |
| IME pre-edit, candidate window placement | `syncTextInput` | done desktop |
| Animation framework, easing, springs, kinetic scroll | core + platform feel | done |
| Accessibility | a11y tree + AccessKit; `PlatformAdapter` (UIA on Windows, NSAccessibility on macOS, AT-SPI on Linux); Android AccessibilityNodeProvider, iOS UIAccessibilityElement | done: Linux + Android verified; Windows UIA built (llvm-mingw); macOS + iOS UNVERIFIED (no Apple SDK) |
| Drag and drop (in-app) | core | done; OS-level DnD G2 |
| Clipboard text | window-system + FORGE fallback | done; rich MIME G2 |
| QML / declarative, hot reload, scripting | `.fuseui`, Lua | done |
| QProperty / QML property bindings (auto dependency tracking, binding loops, two-way UI binding) | `fuse::ui::decl::Property`, `Effect`, `bindText/Value/Css/Visible`, `bind*TwoWay` | done; gate fuse_ui_extras_declarative |
| Qt State Machine (hierarchical, parallel, history, guards, eventless, assignProperty, done.state) | `fuse::ui::decl::StateMachine` | done; gate fuse_ui_extras_declarative |
| Qt SVG (QSvgRenderer / QSvgWidget, static Tiny subset: shapes, paths incl. arcs, groups, transforms, use, gradients, CSS style) | `fuse::ui::svg::Document`, `widgets::svgWidget`; affine `Path::transform`, `Path::ellipse`; full CSS named colours | done; gate fuse_ui_extras_svg |
| Qt Virtual Keyboard | `widgets::virtualKeyboard` (shift / caps / symbols, never steals focus; Qt click-focus semantics in core) | done; gate fuse_ui_extras_vkeyboard |
| Qt Charts | `widgets::chart` (line, bar, area, scatter) | done (basic); no axes customisation / pie |
| Qt Multimedia widgets | `fuse_ui_video` | partial |
| Qt Data Visualization (3D) | none | gap |
| Designer | `fuse_ui_designer`: palette/outline/canvas/properties, drag-drop, multi-select, style sheet, undo, copy/cut/paste/duplicate (clipboard JSON, id renumbering), shortcuts, dirty title (live OS window retitle), validation, live preview mode, device-size frames, Open / Save As via native or built-in file dialog | done (product pass); gate fuse_ui_designer_roundtrip |
| Native file dialogs | `os::nativeFileDialog` (portal / zenity / kdialog, IFileDialog, NSOpenPanel) + built-in `widgets::fileDialog` fallback | done |
| Desktop notifications, open URL | `fuse::ui::desktop` (notify-send / osascript / PowerShell toast, via FORGE process) | done (G5) |
| System tray, global menu bar | `fuse::ui::tray` (StatusNotifierItem + com.canonical.dbusmenu over runtime-loaded libdbus, Shell_NotifyIcon, NSStatusItem/NSMenu) | done: Linux D-Bus gate + Win32 under Wine; macOS UNVERIFIED |
| Printing / PDF output, print preview | `fuse::ui::pdf` (vector PDF writer, DrawList to PDF, `PrintPreview::paginate`) + `desktop::print` (lp / Start-Process / open) | done (G5); anti-aliased glyphs (8-bit soft mask) |
| QUndoStack / QUndoGroup | `fuse::ui::UndoStack`, merge, macros, clean state, limit | done (G5) |
| QGesture (tap, long press, pan, pinch, swipe) | `GestureRecognizer` | done (G5) |
| i18n: tr/trn, .po catalogs, plurals, locale numbers, RTL direction | `fuse::ui::i18n` | done (G5) |
| QGroupBox, QStackedWidget, QToolBox, QStatusBar, QMessageBox, QInputDialog, QWizard, QLCDNumber, QKeySequenceEdit, QCompleter | `widgets_extra.hpp` | done (G5) |
| QMdiArea (tile, cascade, drag, resize, close) | `mdiArea` / `mdiSubWindow` | done (G5) |
| QGraphicsScene / QGraphicsView (grid index, z order, hit testing, selection, zoom, pan, rubber band) | `GraphicsScene` / `graphicsView` | done (G5) |
| Web view | `fuse::ui::web::WebView` over the runtime-loaded system engine (WebKitGTK, WebView2, WKWebView) + `fuse.postMessage` bridge | done: WebKitGTK verified (gate fuse_ui_extras_webview); WebView2 compiled, not run (no runtime under Wine); WKWebView UNVERIFIED |
| Beyond Qt: game HUD/world-space UI, GPU draw-list compute, deterministic replay, immediate + retained in one tree | `fuse_ui_game`, `immediate.hpp` | done |

## 6. Phases for this branch

- **G0 (this round)**: PLAN.md; toolchain fix (clang + libstdc++ 14); FORGE submodule + `fuse_forge`; `fuse::ui::os`
  seam on FORGE; move `fuse_app`'s hand-written OS code (settings files, dirs, single-instance lock, clock, sleep,
  env, libX11 loading) onto it; `fuse/gui.hpp` + `fuse::gui` target; example app; tests green.
- **G1**: Route the remaining direct OS calls (font file read, file dialog directory listing, AccessKit loading) through
  the seam; FreeBSD + macOS cross-compile jobs; CPU raster inner loops on `forge::lane`.
  *Status: done.* OS calls on seam (42870318b); CI jobs staged in `docs/gui/ci/fuse-gui.yml` (needs moving to
  `.github/workflows`, token lacks `workflow` scope; unverified); backdrop-blur vertical pass on `forge::lane`
  (`Core/src/simd/`, AVX2 picked via `forge::lane::detect()`, built from FORGE sources as `fuse_forge_lane`;
  bit-exact vs scalar, gated by `fuse_ui_simd_blur`). Horizontal pass (pixel transpose + lane sweep) and solid
  span fills (`simd::fillSpan`) also on `forge::lane`.
- **G2**: Wayland (SDL3 path per D1), OS-level drag and drop, rich clipboard MIME, native file dialogs, hot reload on
  Wayland done natively (not SDL3: fits the runtime-loaded X11 architecture): `fuse/app/wayland.hpp`, gate `fuse_app_wayland`. X11 XDND drag-out done (`fuse_app_xdnd`). SIMD horizontal blur + span fills on forge::lane.
  `forge::os::file_watch`.
  *Status:* hot reload done: `fuse::ui::doc::HotReload` (styles replaced in place via `StyleEngine::replaceSheet`,
  documents via `Document::reload`, last-good kept on bad edits; test `fuse_ui_hot_reload`).
  Native file dialogs done on the seam: `os::nativeFileDialog` (zenity / kdialog on Linux+BSD, osascript on macOS,
  spawned via `forge::os::spawn_wait`; `unavailable` -> in-UI FileDialog fallback; Win32 IFileDialog with G3 backend).
  Rich clipboard done on the seam: `os::clipboardTypes/clipboardData/setClipboardData` by MIME (wl-clipboard /
  xclip via FORGE process; text/plain falls back to `forge::os::clipboard`).
  System drag and drop (receive) done on X11: XDND v5 drop target in `native.cpp` (XdndAware/Enter/Position/Status/
  Drop/Finished, uri-list preferred over text), surfaced as `AppWindow::onExternalDrop` with decoded file paths.
  XDND drag source done: `AppWindow::beginExternalDrag` (uri-list or MIME data; SelectionRequest served,
  Finished -> `onExternalDragEnd`); `fuse_app_xdnd` drives a real drag+drop round trip under Xvfb with xdotool.
  Win32 OLE IDropTarget/IDropSource + DoDragDrop and IFileOpenDialog/IFileSaveDialog done (`fuse_app_win32_ole`, llvm-mingw build run under Wine).
- **G3**: macOS: Cocoa window, NSTextInputClient IME, MoltenVK present, NSAccessibility.
- **G4**: iOS + Android: surfaces, touch/pen, lifecycle, virtual keyboard, mobile a11y; emulator/simulator gates.
- **G5**: tray/notifications/global menu, printing, web view, designer to product.
  *Status:* `fuse_ui_extras` (Extras/), gated by `fuse_ui_extras_<gate>` tests (12 gates). Done: notifications,
  open URL, PDF writer + DrawList printing + print preview pagination, system printing, undo, gestures, i18n,
  extra widgets, MDI, graphics view, system tray + global menu, rich text editor (`richtextedit`, HTML/Markdown), web view,
  a11y platform adapters, anti-aliased PDF text, designer product pass. Left: run the Apple + WebView2 paths on real hosts;
  rich text italic is drawn upright and Justify falls back to left; reparenting WebKitGTK into a FUSE X11 window is experimental.
  Known FORGE bug (reported, not fixed here): `spawn_wait` leaks pipe fds into children.

Every phase: commits on `gui-framework`, pushed after each milestone; never to `main`; no force pushes.

### Rendering track: HiDPI / shaders / DnD (gui-render)
- HiDPI core: AppWindow::setScale/pixelSize/pixelDrawList, crisp glyphs via UiContext::setPixelScale; WindowDesc::dpiScale seeds it. Gate: fuse_app_hidpi.
- Wayland: wp_fractional_scale_v1 + wp_viewporter, preferred_buffer_scale fallback (tested shm + Vulkan).
- Win32: per-monitor DPI v2, WM_DPICHANGED, DPI-scaled input/popups (built with llvm-mingw, run under wine).
- Cocoa: backingScaleFactor -> contentsScale/drawableSize/setScale, viewDidChangeBackingProperties; NSDraggingDestination drop-in (files + text). UNVERIFIED (no macOS host).
- UIKit: screen.scale -> contentScaleFactor/CAMetalLayer drawableSize/setScale. UNVERIFIED.
- Shaders: configure-time GLSL->SPIR-V (glslangValidator/glslang/glslc) with precompiled fallback; target fuse_ui_spv_fixtures.

### Rendering track: GPU effects, damage repaint, pacing, benchmarks (gui-render)
- Done: `ui.backdrop` compute kernel. Backdrop blur runs on the GPU between draw-list segments and is bit-exact with the CPU `backdropBlur` (vk_parity: 0 differing pixels). The draw kernel now dispatches only over the damage box, with a clear-to-black flag. vk_present uses this for partial frames and no longer clears or redraws the whole canvas.
- Done: the glyph atlas uploads only when `GlyphAtlas::version()` changes. `version()` is new; it is bumped whenever a glyph is rasterised or the atlas is reset.
- Done: frame pacing.
  - `Presenter::vsynced` presenters are not paced; this covers Vulkan FIFO, Wayland with vsync, UIKit CADisplayLink and headless.
  - Other presenters (X11/GDI/Cocoa CPU paths, and Vulkan without vsync) are paced by `AppWindow::setMaxFrameRate`, default 60, on the app clock. Skipped time is carried into the next `update`.
  - Wayland SHM throttles on `wl_surface.frame` through `AppWindow::setFramePending`.
  - Gate: fuse_app_frame_pacing.
- Done: fuse_ui_bench_render (label `bench`) measures CPU vs GPU for a full frame, a damage box, backdrop blur and a text page. It writes bench_render.json.
- Done: shadows are drawn on the GPU by the draw kernel (analytic SDF identical to the CPU; vk_parity "primitives"); no separate pass needed.
- Done: the bench uses GPU timestamp queries (device time) with wall-minus-baseline as the fallback; CPU columns are thread CPU time, minimum over iterations (the box is shared). It is registered in every build; without fuse_ui_vk (Windows cross build) only the CPU and pack columns run.

### Rendering track: vector paths, colour, perf (gui-render)
- Done: `fuse::ui::Path` (moveTo/lineTo/quadTo/cubicTo/arcTo/close, rect, circle, transform) and SVG path data (M L H V C S Q T A Z, relative forms, true elliptical arcs per SVG 1.1 F.6.5). `DrawList::path` (non-zero / even-odd fill + stroke) and `gradientPath` (vertical / horizontal / radial). CPU and the ui.draw_list kernel are bit-exact (vk_parity "paths": 0 differing pixels).
- Done: CPU perf: per-scanline path edge lists (bit-exact), a glyph fast path, and one transpose per backdrop blur with row-major box passes. Release lavapipe box: vector_paths 252 -> 40 ms, text_page 26 -> 14 ms, backdrop_blur 28 -> 23.5 ms.
- Done: colour management basics in `fuse/ui/color_space.hpp`: sRGB / Display P3 / Rec.2020 transfer curves and gamut matrices, PQ and HLG, `toHdr10`, luminance and WCAG contrast. `NativeOptions::colorSpace` (Srgb / DisplayP3 / Hdr10) picks a matching swapchain format through VK_EXT_swapchain_colorspace, falling back to sRGB; `NativePlatform::outputColorSpace(w)` reports what was granted. Content must be encoded by the app (the canvas stays RGBA8).
- Done (UNVERIFIED, no macOS host): Cocoa CPU presenter paced by CVDisplayLink (vsynced presenter + `setFramePending`, cleared on the main queue each refresh).
- Done: HDR10 output. With `colorSpace = Hdr10` granted, the canvas is A2B10G10R10 and each upload converts the sRGB raster with `color::Hdr10Packer` (Rec.2020 + PQ, SDR white = `NativeOptions::sdrWhiteNits`, default 203; LUT-driven, within 2/1023 of `toHdr10`). VK_EXT_hdr_metadata is enabled when present and the swapchain gets ST 2086 metadata (Rec.2020, D65, `hdrMaxNits`). HDR10 presents through CPU upload (the draw kernel writes RGBA8).
- Done: Windows AVX2. `fuse_target_is_x86_64` probes the compiler, since CMAKE_SYSTEM_PROCESSOR is empty in cross builds, and `fuse_simd_flags` picks /arch:AVX2 (MSVC) or -mavx2. The lane library now builds for MSVC too. Wine backdrop_blur went from 40.8 to 24 ms; MSVC is unverified (no MSVC on the box).
- Done: GPU path culling. Each path packs per-16px-row edge lists (an edge's y-range grown by the stroke reach), and the kernel walks only its row's list. Bit-exact.
- Done: compact glyph records. Glyphs pack as 11-word records in the images buffer (tile id `kCompactGlyph | offset`), and clip carriers are shared per clip run. text_page went from 63k GpuCmds of 128 B to about 1 cmd plus 2.8 MB, and it now fits the default upload limits. Before this change, text_page (63k cmds) exceeded `maxCmds = 16384`, so its GPU bench numbers timed a rejected upload. The bench now flags such scenes.
- Left:
  - HDR on the GPU draw path (rgb10a2 storage plus PQ in the kernel), plus extended-range scRGB.
  - Wayland: when the Vulkan WSI is given an HDR10 colour space, it attaches wp_color_management_v1 to the surface itself, and a second app-side object would be a protocol error. Tagging SHM (CPU) surfaces only matters for non-sRGB content, which the 8-bit raster cannot carry.
  - Win32: hide Hdr10 unless the display's DXGI output reports HDR (IDXGIOutput6). This needs a FORGE DXGI query.
  - GPU text: the kernel's glyph loop is the text_page cost on lavapipe; batch several glyphs per tile entry.
  - The Apple paths (CVDisplayLink, Metal surface) are unverified on hardware.
