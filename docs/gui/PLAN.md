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
| Linux Wayland | SDL3 per D1(c), native later | text-input-v3 | Vulkan | AT-SPI | planned (G2) |
| Windows | native Win32 | IMM32 | Vulkan / GDI fallback | UIA via AccessKit | **done**, gated under Wine |
| FreeBSD | X11 backend (same code) | XIM | Vulkan / CPU | AT-SPI | builds expected; CI cross-compile in G1 |
| macOS | Cocoa via SDL3 → native NSView | NSTextInputClient | MoltenVK / CAMetalLayer | NSAccessibility | scaffold (G3) |
| iOS | UIKit via SDL3 | UITextInput | MoltenVK | UIAccessibility | scaffold (G4) |
| Android | ANativeActivity via SDL3 | InputConnection | Vulkan `ANativeWindow` | AccessibilityNodeProvider | scaffold (G4) |

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
| QtGui windowing, QWindow, multi-window, popups | `fuse_app_native` | done (X11, Win32) |
| QPainter / 2D raster, paths, gradients, blur | draw list + CPU raster + Vulkan | done |
| High DPI, per-monitor scale | `dpiScale` plumbing | done desktop; mobile G4 |
| QtWidgets basic (button…lineedit) | Tier A | done |
| Containers, MainWindow, docks, menus, toolbars, dialogs | Tier B | done |
| Model/view, delegates, proxy sort/filter, 1M rows | Tier B/C/D | done |
| QSS styling, themes, palettes | `StyleSheet`, themes | done |
| Text: shaping, bidi, fallback, emoji, grapheme editing | `fuse_text` FULL | done |
| IME pre-edit, candidate window placement | `syncTextInput` | done desktop |
| Animation framework, easing, springs, kinetic scroll | core + platform feel | done |
| Accessibility | a11y tree + AccessKit (Linux) | partial: Win/mac/mobile adapters |
| Drag and drop (in-app) | core | done; OS-level DnD G2 |
| Clipboard text | window-system + FORGE fallback | done; rich MIME G2 |
| QML / declarative, hot reload, scripting | `.fuseui`, Lua | done |
| Designer | `fuse_ui_designer` | prototype |
| Native file dialogs | `vendor/nativeFileDialogs` | G2 |
| System tray, notifications, global menu bar | — | G5 |
| Printing / PDF output | — | G5 |
| Web view | system web view widget | G5 (optional) |
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
  bit-exact vs scalar, gated by `fuse_ui_simd_blur`).
- **G2**: Wayland (SDL3 path per D1), OS-level drag and drop, rich clipboard MIME, native file dialogs, hot reload on
  `forge::os::file_watch`.
  *Status:* hot reload done: `fuse::ui::doc::HotReload` (styles replaced in place via `StyleEngine::replaceSheet`,
  documents via `Document::reload`, last-good kept on bad edits; test `fuse_ui_hot_reload`).
  Native file dialogs done on the seam: `os::nativeFileDialog` (zenity / kdialog on Linux+BSD, osascript on macOS,
  spawned via `forge::os::spawn_wait`; `unavailable` -> in-UI FileDialog fallback; Win32 IFileDialog with G3 backend).
  Rich clipboard done on the seam: `os::clipboardTypes/clipboardData/setClipboardData` by MIME (wl-clipboard /
  xclip via FORGE process; text/plain falls back to `forge::os::clipboard`).
  System drag and drop (receive) done on X11: XDND v5 drop target in `native.cpp` (XdndAware/Enter/Position/Status/
  Drop/Finished, uri-list preferred over text), surfaced as `AppWindow::onExternalDrop` with decoded file paths.
  Left: XDND drag *source*, Win32 OLE IDropTarget, Wayland (wl_data_device) - with the Wayland backend.
- **G3**: macOS: Cocoa window, NSTextInputClient IME, MoltenVK present, NSAccessibility.
- **G4**: iOS + Android: surfaces, touch/pen, lifecycle, virtual keyboard, mobile a11y; emulator/simulator gates.
- **G5**: tray/notifications/global menu, printing, web view, designer to product.

Every phase: commits on `gui-framework`, pushed after each milestone; never to `main`; no force pushes.
