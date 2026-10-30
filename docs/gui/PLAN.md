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
| Qt SVG (QSvgRenderer / QSvgWidget: shapes, paths incl. arcs, groups, transforms, use, CSS style, multi-stop linear/radial gradients (units, gradientTransform, spreadMethod), `<text>`/`<tspan>` via font outlines, clipPath, mask, pattern) | `fuse::ui::svg::Document`, `widgets::svgWidget`; `DrawList::pushClipPath` (CPU + GPU, parity exact), `Font::glyphOutline` | done; gate fuse_ui_extras_svg. Masks are per-pixel (luminance x alpha) and clipPath unions exact via `DrawList::pushClipLayers` (CPU + GPU, vk_parity "clip_layers" exact); radial focal point (fx/fy) on CPU + GPU |
| Qt Virtual Keyboard | `widgets::virtualKeyboard` (shift / caps / symbols, never steals focus; Qt click-focus semantics in core) | done; gate fuse_ui_extras_vkeyboard |
| Qt Charts | `widgets::chart` (line, bar, area, scatter, pie, donut), axis customisation, legends, tooltips, wheel/drag zoom | done; gate fuse_ui_extras_charts |
| Qt Multimedia widgets | `fuse_ui_video` (Theora), `media::SystemVideo` (system decoder: GStreamer loaded at runtime, decodes into `VideoSource`), `widgets::audioLevelMeter` (peak/RMS dBFS, hold, clip lamp) | done (gate fuse_ui_extras_media): streaming decode + audio playback (Pulse / ALSA / WASAPI); Media Foundation backend passes under Wine; AVFoundation backend written, not built or tested (no Apple SDK) |
| Qt Data Visualization (3D) | `widgets::dataViz3d` (surface, scatter, bars; camera drag/wheel/keys, picking, tooltips) projected on the CPU into DrawList paths, so it runs on the Vulkan draw-list path and the CPU raster alike | done (basic); gate fuse_ui_extras_dataviz3d, vk_parity "svg+dataviz3d" 0 differing pixels. Surface cells and bar faces are ordered by a BSP tree (`dv3d::bspOrder`: splits straddling faces, splitter = fewest cuts of 16 candidates), so interpenetrating and cyclic overlaps paint correctly; points / grid / markers keep the depth sort. Gate fuse_ui_extras_dataviz3d_bsp. Split pieces are filled without the wire outline and can show faint AA seams |
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
- Done: GPU text runs. Compact glyph records are now 7 words: origin, uv, size+flags, a fill offset shared by consecutive glyphs, the clipped box, and a clip carrier only for rounded clips. Unclipped, pixel-aligned glyphs take a fast path. Each tile turns runs of 3+ glyphs with non-decreasing x into one run entry, with a y union and per-column ranges [lo,hi), so a thread only visits glyphs that cover its column. Lavapipe text_page went from 69 to 20.6 ms against 13.9 ms on the CPU, so it still does not beat the CPU there. The base dispatch alone is about 9-10 ms on llvmpipe, and on a real GPU text is no longer the bottleneck.
- Done: GPU HDR encode. `UiVkEncoder` (ui_encode_pq / ui_encode_scrgb) converts the RGBA8 canvas over the damage box into an rgb10a2 (BT.2020 + PQ) or rgba16f (scRGB, linear x sdrWhite/80) output image, which is then blitted to the swapchain. GPU draw stays on in HDR. `OutputColorSpace::ScRgb` (EXTENDED_SRGB_LINEAR + R16G16B16A16_SFLOAT) is offered only with the encoder. Without the encoder, HDR10 falls back to the CPU Hdr10Packer. Gate: fuse_ui_vk_encode (hdr10 1 code, scrgb 0.09%).
- Done: Win32 HDR check. `fuse::ui::os::displayHdr(hwnd)` calls FORGE `forge::os::display_hdr`, which uses DXGI IDXGIOutput6::GetDesc1 on the window's monitor, with dxgi.dll loaded at run time. When the OS reports SDR, vk_present refuses Hdr10/ScRgb, and it clamps the ST 2086 peak to the panel's max nits. The FORGE side is a LOCAL change to vendor/forge os.hpp and is not pushed. The FUSE code is guarded by `FORGE_OS_HAS_DISPLAY_HDR` and reports "unknown" until FORGE ships it.
- Done: HDR content. `UiVkRenderer::setTargetHdr` switches to ui_draw_list_hdr and ui_backdrop_hdr, which are the same .glsl bodies built with `UI_HDR`. They draw into an RGBA16F canvas of extended-range sRGB (1.0 = SDR white), blending in float without 8-bit rounding. The encoders gain float-source variants (`UiVkEncoder::init(..., hdrSource)`).
  - App API: `color::hdr(c, brightness)` scales c's linear light, `color::hdrNits(c, nits, sdrWhite)` sets an absolute brightness, and `color::hdrBrightness(c)` reads it back. `NativeOptions::hdrContent` (default on) uses the float canvas whenever Hdr10/ScRgb is granted with GPU draw and the encoder; `NativePlatform::hdrContent(w)` / `WaylandPlatform::hdrContent(w)` report it.
  - The CPU raster and 8-bit targets clip at 1.
  - Gate: fuse_ui_vk_hdr. SDR scenes are within 1 code of the CPU; 4x white stays at 1.824; the PQ code goes 594 to 746 (SDR white vs 4x).
- Done: glyph ink culling. Each glyph record shrinks to the ink box of its atlas bitmap: origin, uv, size and pixel boxes, computed once per pack per unique glyph in a reused open-addressed table. Blank bitmaps are dropped, and tiles that only the padding touches no longer list the glyph. It is bit-exact. FUSE's atlas bitmaps are already tight, so text_page does not change (same tile bytes); it helps padded or SDF-style atlases.
- Done: persistent GPU buffers. `packDrawList(..., atlasGeneration)` keeps the previous pack when the inputs are unchanged: list contents, size, scissor, image pixels and a known atlas generation. It sets `GpuDrawData::reused` and keeps `contentVersion`, which is process-unique. `UiVkRenderer::upload` then writes nothing. A changed frame writes only the 4 KB pages that differ from what the slot holds, tracked against a per-slot CPU shadow; `uploadedBytes()` counts them. vk_present passes the atlas version.
  - Release lavapipe text_page: repack 0.51 ms instead of 3.8 ms, 0 bytes uploaded for an unchanged frame, and 1.5 of 3.1 MB after one glyph colour change. Inserted fill words shift the later records and runs, so those pages differ.
  - Gate: fuse_ui_vk_parity "incremental-upload".
- Done: colour-only patching. When a frame differs from the previous pack only in DrawCmd::fill (hover, selection, caret or theme tint), packDrawList patches in place and sets `GpuDrawData::patched`. A GpuCmd gets its fill rewritten; a glyph record's fill word is pointed at deduplicated words appended after the packed data, using a per-command slot map (`cmdSlot`). Nothing else moves, so the page diff uploads only the touched pages.
  - Release text_page: patch 0.56 ms instead of a 4 ms repack, and 7 KB uploaded instead of 1.5 MB.
  - Geometry, order or count changes still repack fully.
  - Gates: fuse_ui_vk_parity "incremental-upload", "patch-rect" and "moved-glyph".
- Done: HDR CPU images. `Image::hdrPixels` holds premultiplied RGBA16F texels, built with `color::packHdrPixel`, plus an 8-bit `pixels` fallback. The GPU path ships them as half words (image table flag `kImageHalf`) and keeps values above 1 on the HDR target. Gate: fuse_ui_vk_hdr "img-hdr" (2.5 stays 2.5).
- Done: HDR external textures. On the HDR target, float external textures keep values above 1 (rgb >= 0, alpha 0..1). Gate: fuse_ui_vk_hdr "ext-hdr" (3.0 stays 3.0).
- Shaders: variants are `#include`s of shared `.glsl` bodies (GL_GOOGLE_include_directive); CMake rebuilds on body changes.
- Left:
  - Upstream the FORGE `display_hdr` (os.hpp) and bump the submodule. Until then the Win32 check is compiled out (local change only, awaiting the owner).
  - HDR on the CPU-upload path: CpuRasterizer is 8-bit. A float raster means a second pixel pipeline (blend, AA, blur, images); not started.
  - Structural changes (moved, added or removed commands) still repack fully (about 4 ms on text_page), and an insertion shifts every later images word. Stable record slots keyed by DrawCmd::node would fix that.
  - Wayland: when the Vulkan WSI is given an HDR10 colour space, it attaches wp_color_management_v1 to the surface itself, and a second app-side object would be a protocol error.
  - GPU text on lavapipe is still slower than the CPU (about 21 vs 14-16 ms). The cost is per-pixel glyph loops on a software rasteriser; real GPUs are not measured here.
  - The Apple paths (CVDisplayLink, Metal surface) are unverified on hardware.
- Done: node-keyed glyph slot arena in packDrawList — glyph records live in per-node slots with slack (grow-in-place, holes compacted on reset); inserting/removing/moving glyphs in one node rewrites only that slot plus cmds/tiles (text_page insert: 70 KB of 3.6 MB uploaded). Parity tests inserted-glyph / removed-glyph.
- Done: node-keyed slots for GpuCmds and per-(pass, tile) slots for tile lists (kernel tile header is now begin|end per tile; glyph runs live in the tile slot). Incremental structural pack: one changed range of plain draw commands re-emits only that range into free slot spots and rebuilds only the tiles it touches (text_page insert: 0.75 ms pack, 20 KB upload, was 14.8 ms / 70 KB). Parity: inserted-rect, rect+removed; incremental asserted.
- Done: fuse_ui_bench_10k gate is RUN_SERIAL and uses the best median of 10 batches (was flaky under Wine with ctest -j load).

## Features track (gui-features), latest
- Done: backdrop blur honours clip paths. `DrawOp::Backdrop` coverage is multiplied by the active `pushClipPath` / `pushClipLayers` entries on the CPU (`backdropBlur`) and GPU (ui.backdrop binds the images buffer; clip coverage shared with ui.draw_list via `ui_clip.glsl`). vk_parity "backdrop_clip": 0 differing pixels.
- Done: CSS `background-clip: border-box | padding-box | content-box` (`ComputedStyle::backgroundClip`). Background, gradients and the backdrop paint in the chosen box with radii shrunk by the inset; the border still strokes the border box. Gate fuse_ui_backdrop_blur.
- Done: 3D depth ordering by BSP in datavis3d (see the Data Visualization row).
- Left: `background-clip: text`; backdrop-filter functions other than blur (brightness, saturate, ...); wire outlines on BSP-split pieces; AVFoundation build/test on a Mac.
