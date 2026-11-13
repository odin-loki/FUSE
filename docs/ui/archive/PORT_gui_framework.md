# Port list: `gui-framework` onto main's FUSE UI

Written 2026-11-13 with the merge commit that brought the branch into main. It is the work list for finishing that
merge: what of the branch is live in the merged build, and for every feature that is **not** live, where its code is,
what blocks it and how to check it once it is ported. Background and the survey of all branches:
[`BRANCH_MERGE_INVENTORY.md`](BRANCH_MERGE_INVENTORY.md). The same list for the other big branch:
[`PORT_magical_bassi.md`](PORT_magical_bassi.md) ("mb" below); §6 compares the features both branches implement.

Names used below:

- **base** = `5dcee19e` (common ancestor), **branch** = `363ecf87d` (tip of `gui-framework`, now an ancestor of main;
  `wip/fuse-backup-20261031` = `b3a50c90b` is an ancestor of it), **main** = the first parent of the merge commit.
- `U/` = `Source/FUSE/UI/`.
- "In the tree" = the file is checked in on main at that path with the branch's content. "Unbuilt" = no CMake target
  lists it.
- "Branch delta" = a change the branch made to a file main also has, which the merge did **not** take. It is not in
  the working tree. Read it with `git diff 5dcee19e 363ecf87d -- <path>` (what the branch changed) or
  `git show 363ecf87d:<path>` (the branch's whole file).
- `U/_port/gui-framework/<relative path>` = the branch's version of a file whose path `U/<relative path>` was already
  taken on main by a different file (§1.1). Nothing builds from `U/_port/`.

## 1. How the merge was resolved

| Area | Rule applied | Result |
|---|---|---|
| 61 existing files under `U/` the branch changed (36 with content conflicts, 22 that git merged cleanly or only the branch changed, 2 modify / delete, 1 delete) | Main's version, unchanged | A textual merge would have left duplicate `UiContext` members (`animate`, `animating`, `animSetKeyframes`, `defineClipPath`, `invalidate`, `scriptAnim`, `timelineProgress`), two path / gradient types in `draw_list.hpp`, two `GpuDrawData` layouts and the Lua binding deleted. §4 lists the branch deltas left out, per file. |
| 5 add / add paths | Main's file stays; the branch's is copied to `U/_port/gui-framework/` | §1.1 |
| 163 new files under `U/` | All kept | 39 are built on at least one platform (§2); 124 are unbuilt and listed in §3. Five of them were edited so that they compile on main (§2.1). |
| Lua binding (`U/Script/include/fuse/ui/script/lua_ui.hpp`, `U/Script/src/lua_ui.cpp`, `U/tests/test_lua.cpp`) | The branch deleted them; **the deletion was not taken** | Main's files and the gates `fuse_ui_lua_bindings`, `fuse_ui_script_bind` are unchanged. |
| `.gitmodules`, gitlink `vendor/forge` (FORGE submodule) | **Not taken** (repository policy: no new git submodules) | §5 |
| `cmake/FuseForge.cmake`, `cmake/forge-patches/0001-script-host-bindings.patch` | Kept; `FuseForge.cmake` edited | No default path, nothing fetched; used only with `-DFUSE_UI_OS_BACKEND=forge -DFUSE_FORGE_DIR=<checkout>`. §5 |
| `docs/gui/{PLAN.md,TESTING.md,ci/fuse-gui.yml,ci/freebsd-cross.cmake}`, `scripts/gui/*` (11 files) | Kept as an archive | A note at the top of `PLAN.md`, `TESTING.md`, `scripts/gui/README.md` and `fuse-gui.yml` says they describe the branch's tree. The scripts were not run against main. |
| `docs/ui/README.md` | Main's text plus sections for what is live | The branch's README: `git show 363ecf87d:docs/ui/README.md`. |

No golden image changed. Nothing main had was deleted.

### 1.1 Same-path files

| Path under `U/` | On main at that path | Branch version kept at | Port item |
|---|---|---|---|
| `Core/src/path.cpp` | main's `Path` / flatten / stroke / coverage (838 lines, live) | `U/_port/gui-framework/Core/src/path.cpp` (500 lines: the branch's `Path`, `DrawList::path`, gradients, `Gradient::at`) | 3.9 |
| `Doc/include/fuse/ui/doc/hot_reload.hpp`, `Doc/src/hot_reload.cpp` | main's `HotReload` (live, gates `fuse_ui_hot_reload_identity`, `fuse_ui_hot_reload_file`) | deleted 2026-11-13 (ported: `doc::StyleSheetFileWatcher` in main's files) | 3.14 |
| `App/android/AndroidManifest.xml` | main's Gradle template manifest (live packaging helper) | `U/_port/gui-framework/App/android/AndroidManifest.xml` (for `org.fuse.ui.FuseActivity`) | 3.4 |
| `tests/test_app_wayland.cpp` | mb's parked test (311 lines, unbuilt; goes with `native_wayland.cpp`) | `U/_port/gui-framework/tests/test_app_wayland.cpp` (583 lines: in-process Wayland compositor driving the branch's `wayland.cpp`) | 3.2 |

## 2. Live now (built and gated in the merged tree)

| Feature | Files | Gates |
|---|---|---|
| `fuse_ui_os`: the `fuse::ui::os` seam on a standard-library backend | `U/Os/include/fuse/ui/os.hpp` (branch); **`U/Os/src/os_std.cpp` (new, written for the merge)**: environment, config / runtime dirs, `readFile`, `writeFileAtomic`, directory listing, polling `FileWatcher`, clocks, `InstanceLock` (flock / named mutex), `Library` (dlopen / LoadLibrary), `runProcess` (fork + exec, POSIX desktops only), `findExecutable`, helper-based clipboard and file dialogs (wl-clipboard / xclip, zenity / kdialog / osascript), POSIX shared buffers and pipes; `U/Os/src/file_dialog_win32.cpp` (branch, Windows only) | `fuse_ui_os_seam` (`U/tests/test_os_seam.cpp`; the process, pipe and shared-buffer checks were added on main) |
| Undo framework: `UndoCommand`, `UndoStack`, `UndoGroup` | `U/Extras/include/fuse/ui/extras/undo.hpp`, `U/Extras/src/undo.cpp` | `fuse_ui_extras_undo` |
| Declarative `Property<T>` bindings, `Effect`, `bindText / bindValue / bindCss / bindVisible`, `StateMachine` | `extras/declarative.hpp`, `Extras/src/declarative.cpp` | `fuse_ui_extras_declarative` |
| `PdfDocument`, `renderDrawList` (draw list -> vector PDF page) | `extras/pdf.hpp`, `Extras/src/pdf.cpp` | `fuse_ui_extras_pdf` |
| Desktop integration: notifications, `openUrl`, printer list, print jobs (argv builders + helper processes) | `extras/desktop.hpp`, `Extras/src/desktop.cpp` | `fuse_ui_extras_desktop` (argv only; no helper is run) |
| `TrayIcon`, `GlobalMenuBar`, minimal D-Bus client | `extras/{tray,dbus}.hpp`, `Extras/src/{tray,tray_sni,tray_none,tray_win32,dbus}.cpp`, `tray_internal.hpp` | `fuse_ui_extras_tray` (StatusNotifierItem + dbusmenu against a private `dbus-daemon`, checked with `busctl`); `fuse_ui_tray_win32` is registered on Windows only and was not run |
| `WebView` on the system engine | `extras/webview.hpp`, `Extras/src/{webview,webview_gtk,webview_none,webview_win32}.cpp`, `webview_internal.hpp` | `fuse_ui_extras_webview` under `U/tests/run_isolated_x.sh` (own Xvfb + session bus). **On the merge machine no WebKitGTK is installed, so the gate printed "no system engine, skipped" and passed without loading a page.** |
| Charts library `fuse::ui::charts` (line, area, bar, scatter, pie / donut, candlestick, box, waterfall, contour, radar, treemap, sankey, gauge, funnel; axes, legend, tooltips, zoom / pan, model binding, min-max / LTTB decimation, transitions, PNG export, `chartView`, `Chart3D`, `chart3dView`) | `extras/charts.hpp`, `Extras/src/{charts,charts_simd}.cpp`, `charts_simd.hpp`, `charts_simd_kernel.hpp` | `fuse_ui_charts_{series,axes,decimation,ring,hit,widget,model,export,bench,demo,3d,3d_large,demo3d,mesh3d,mesh3d_large,beyond,transition,demo_beyond}` (`U/tests/test_charts.cpp`) |
| 3D data visualisation (`fuse::ui::dv3d`: surface, scatter, bars, volume slices, BSP ordering, picking) and the CPU mesh rasteriser | `extras/datavis3d.hpp`, `Extras/src/datavis3d.cpp`, `U/Core/include/fuse/ui/mesh3d.hpp` (header only) | `fuse_ui_extras_dataviz3d`, `fuse_ui_extras_dataviz3d_bsp`, the `3d` / `mesh3d` chart gates |
| **Ported 2026-11-13 (item 3.7, part):** line edit options (echo modes, max length, placeholder, read-only, validators + fixup, input masks, `EditingFinished`), `intValidator` / `doubleValidator` / `regexValidator`, `QCompleter`-style completer, editable combo box, spin box options (typed entry, prefix / suffix, special value, wrapping, adaptive step, correction, auto-repeat), tab bar options (closable, movable, disabled tabs, scroll buttons / shrink to fit, add / remove pages) | `U/Widgets/include/fuse/ui/widgets/edit_options.hpp` (new), `U/Widgets/src/{widgets_lineedit.cpp,widgets_spinbox.cpp,widgets_tabs.cpp}` (new, the branch code moved onto per-node option records), `U/Widgets/src/{line_regex.cpp,completer.cpp}` (the branch's, adapted); hooks in `widgets.cpp` / `widgets_tier_b.cpp`; `SignalKind::EditingFinished / Highlighted / Activated` | `fuse_ui_lineedit`, `fuse_ui_completer`, `fuse_ui_editcombo`, `fuse_ui_spinbox` (the branch's tests, accessibility parts rewritten for main's snapshot), `fuse_ui_tab_options_{closable,movable,scroll,pages}` (new), golden `fuse_ui_port_golden_edit_options` |
| **Ported 2026-11-13 (item 3.12):** `dialogButtonBox`, `setDialogDefaultButton` / `setDialogRejectButton`, `inputDialog`, `toolBox`, `lcdNumber`, `keySequenceEdit`, `mdiArea` / `mdiSubWindow`, `GraphicsScene` / `graphicsView`, `virtualKeyboard`, `setWizardValidator` (on main's wizard) | `U/Widgets/include/fuse/ui/widgets/extra.hpp`, `U/Widgets/src/{widgets_extra.cpp,widgets_buttonbox.cpp}` (in `fuse_ui_widgets`, not `fuse_ui_extras`: document classes and `installClasses` need them there); document classes in `U/Doc/src/doc_classes.cpp`; theme rules in `U/Widgets/src/theme.cpp` | `fuse_ui_extra_{buttonbox,dialog_keys,input_dialog,toolbox,lcd,keysequence,mdi,graphics,vkeyboard,classes}` (`U/tests/test_widgets_extra.cpp`), golden `fuse_ui_port_golden_extra`, `fuse_ui_doc_every_class` |
| Key-code / modifier / role tables of the branch's Cocoa backend | `U/App/src/cocoa_map.hpp` (plain header) | `fuse_app_cocoa_map` (`U/tests/test_cocoa_map.cpp`). The backend that uses the table is parked (3.3). |
| From the branch's Wayland backend (3.2), onto the backend ported from `claude/magical-bassi-815825`: the Vulkan Wayland surface (`SurfaceSource::Wayland`, `VK_KHR_wayland_surface` resolved through `vkGetInstanceProcAddr`, no `vulkan_wayland.h` needed), the `wl_surface.frame` throttle for `wl_shm` presents, and the in-process compositor as a gate that needs no installed compositor | `U/App/src/vk_present.cpp`, `native_internal.hpp`, `native_wayland.cpp` (`frameReady`), `U/tests/test_app_wayland_seat.cpp` (new, written for main's backend; the branch's test stays under `_port/`) | `fuse_app_wayland`, `fuse_app_wayland_seat` |
| **Ported 2026-11-13.** Rich text: `rich::RichDocument`, `RichTextEditor`, HTML + Markdown import / export, `richtextedit` widget, on main's text stack (real bold / italic faces, font decoration metrics), undo on `UndoStack`, `toRichTextMarkup / fromRichTextMarkup` bridge to main's `richText` view | `extras/richtext.hpp`, `Extras/src/richtext.cpp` | `fuse_ui_extras_richtext_{model,undo,widget,view,golden}` (`U/tests/test_richtext.cpp`) |
| **Ported 2026-11-13.** `PdfDocument::renderDrawList` writes main's `Path` commands (fill, even-odd, stroke, dashes) and path clips | `Extras/src/pdf.cpp` | `fuse_ui_extras_pdf_paths` (`U/tests/test_pdf_paths.cpp`) |
| **Ported 2026-11-13.** Audio level meter widget, split out of the media module | `extras/audio_meter.hpp`, `Extras/src/audio_meter.cpp` | `fuse_ui_extras_audio_meter` (`U/tests/test_extras_small.cpp`) |
| **Ported 2026-11-13.** Gesture recogniser as `fuse::ui::gesture::GestureRecognizer` (recogniser helpers next to the core's gestures) | `extras/gesture.hpp`, `Extras/src/gesture.cpp` | `fuse_ui_extras_gesture` |
| **Ported 2026-11-13.** Style sheet hot reload as `doc::StyleSheetFileWatcher` on main's file poll (`StyleEngine::replaceSheet` by handle) | `U/Doc/include/fuse/ui/doc/hot_reload.hpp`, `U/Doc/src/hot_reload.cpp`, `U/Core/src/style_sheets.cpp` | `fuse_ui_hot_reload_style` (`U/tests/test_hot_reload.cpp`, rewritten) |
| **Ported 2026-11-13.** gettext `.po` catalogues as string-table entries: `l10n::parsePo`, `poKey`, `poToTable` (the reader of `extras/i18n.hpp`, folded into `fuse_ui_l10n_lib`) | `U/L10n/include/fuse/ui/l10n/l10n.hpp`, `U/L10n/src/po.cpp` (new) | `fuse_ui_l10n_po` (`U/tests/test_l10n_po.cpp`) |
| **Ported 2026-11-13.** `<fuse/gui.hpp>` umbrella header and target `fuse::gui`, over the live modules | `U/Public/include/fuse/gui.hpp`, `U/Public/examples/gui_quickstart.cpp` | `fuse_gui_quickstart` |

Standalone UI build on Linux after the merge: 208 ctest gates (the 180 main had + 28). After the theme, widget and
platform ports were merged and made consistent (2026-11-13; the list is in `PORT_magical_bassi.md` §2: the ported
widgets of 3.7 / 3.12 read theme tokens, have light / high-contrast goldens and focus rings, and are in the theme
gates): 292 ctest entries. `fuse_ui_os` and
`fuse_ui_extras` are in the FuseUI install set (`U/cmake/fuse_ui_install.cmake`; `fuse_ui_install_consumer` passes).

Option: `FUSE_BUILD_UI_EXTRAS` (builds both targets). Default ON on Linux, FreeBSD and Android (all three built here;
gates run on Linux). Default **OFF on Windows and Apple platforms**: every source of the two targets compiles with
MinGW-w64 13 (`-fsyntax-only`), but nothing was built with MSVC or clang-cl, nothing was linked or run on Windows, and
no Apple build was possible. Turning it on there is the first step of 3.16.

### 2.0 Live since the app-platform port (2026-11-13)

| Feature (item) | Where on main | Gates |
|---|---|---|
| OS drag and drop, X11 XDND v5 target and source (3.1) | `U/App/src/native_platform.inl` (`dndClient`, `dndSelectionNotify`, `dndOutMotion`, `dndOutRelease`), hooked into main's X11 pump; routed through main's core API instead of the branch's `AppWindow::onExternalDrop / beginExternalDrag`: drops call `UiContext::dragExternalMove / dropExternal` (`kDragFiles` with LF-separated paths, `kDragText`), a UI drag (`beginDrag`) with text / files goes out by itself when the pointer leaves the app's windows and ends with the new `UiContext::dragFinishExternal`. `NativeOptions::dragAndDrop`, `NativePlatform::externalDragActive`, `pathsFromUriList` / `uriListFromPaths` in `services.hpp` | `fuse_app_native_services` (two X connections in one process: text, files, refusal, leave and return, in-app-only type, a window that is no drop target), `fuse_app_tool_surfaces` (`dragFinishExternal`), `fuse_app_dialog_helpers` (uri lists) |
| OS drag and drop, Win32 OLE (3.1) | `U/App/src/native_platform_win32.inl` (`DropTarget`, `DataObject`, `DropSource`, `dragOutCheck`): CF_HDROP and CF_UNICODETEXT both ways. **Compiled and linked with MinGW-w64, never run** | none run |
| OS tool windows for floating docks (3.5) | bassi's API as §6 recommends: see `PORT_magical_bassi.md` §2.1 | `fuse_app_tool_surfaces`, `fuse_app_native_services` |
| Frame pacing (3.5) | `AppWindow::setMaxFrameRate / maxFrameRate` (default 0 = unlimited, the branch had 60), `Presenter::vsynced` (set by the X11 / Win32 backends for a FIFO Vulkan swapchain) in `U/App/src/app.cpp` | `fuse_app_frame_pacing` (virtual clock) |
| Live retitle, `WindowDesc::dpiScale` (3.5) | already on main (`AppWindow::setTitle` -> `WindowRequest::Title`, `WindowDesc::dpiScale`) | `fuse_app_window_state`, `fuse_app_dpi_scale` |
| Native notifications (§6) | `fuse::ui::desktop::Notifier` on `extras/dbus.hpp`, `notify()` native first with `notifyCommand` as the fallback, `TrayIcon::showMessage` | `fuse_ui_extras_notify` |

Parked files deleted because they are superseded: `U/tests/test_app_xdnd.cpp`, `U/tests/test_app_x11_widget_dnd.cpp`
(both drove the branch's callback API with `xdotool`).

### 2.1 Changes made to branch files so they build on main

| File | Change |
|---|---|
| `U/Extras/src/charts.cpp`, `U/Extras/src/datavis3d.cpp` | The branch's path API (`Path::circle / append`, `Path::arcTo(Vec2, degrees, ...)`, `DrawList::path(path, fill, evenOdd, stroke, width)`, `DrawList::gradientPath`, `Gradient::at`) is called through `U/Extras/src/path_compat.hpp` (new), which maps it onto main's `Path::addCircle / addPath / arcTo`, `DrawList::fillPath / strokePath` and `Brush`. About 45 call sites; no logic changed. Strokes use round joins and caps. |
| `U/Extras/src/pdf.cpp` | `renderDrawList` skips main's `Path`, `NinePatch`, `PushClipPath` and `PushLayer` ops and keeps its clip stack balanced across them (it does not draw them: 3.13). |
| `U/tests/test_extras.cpp` | The gates of parked modules are inside `#if FUSE_UI_EXTRAS_ALL` (default 0); an unknown `--gate=` now fails with exit code 2 instead of passing; one `dl.path` call became `fillPath`. |
| `U/tests/test_charts.cpp` | No `extras.hpp` umbrella include and no `registerExtraWidgets` (both belong to parked modules). |
| `U/tests/test_os_seam.cpp` | The `fuse_app` parts (Settings, single instance, `pathsFromUriList` through the seam) are inside `#if FUSE_UI_OS_SEAM_APP`; process / pipe / shared-buffer checks added. |
| `U/Extras/include/fuse/ui/extras/{charts,desktop}.hpp` | Header comments corrected (namespace is `fuse::ui::charts`; no FORGE on main). |
| `U/Extras/src/compat/mingw/EventToken.h`, `U/Script/src/forge_remote_off.cpp` | `fuse-lint-allow(namespace)` markers (both unbuilt; the repository lint scans every file under `Source/FUSE`). |
| `cmake/FuseForge.cmake` | §5 |

## 3. Not live: port items

Each item: what it is, where the branch code is, what it overlaps on main, what blocks it, size (S: under a day,
M: days, L: a week or more), and how to verify. "Gates" are the branch's ctest names; their registrations are in the
branch's `U/tests/CMakeLists.txt` (`git show 363ecf87d:Source/FUSE/UI/tests/CMakeLists.txt`), which main did not take.

Order that unblocks the most: 3.9 (one path / gradient API) and 3.8 (style engine) first: 3.10, 3.11, 3.12 and 3.13
build on them. 3.1, 3.5, 3.6 and 3.15 are independent of those.

### 3.1 OS drag and drop: X11 XDND, Win32 OLE, Cocoa; drag out of the app (M)

- **What**: XDND v5 target and source with several formats, Win32 `IDropTarget` / `DoDragDrop` with drag images,
  Cocoa drop-in; `AppWindow::onExternalDrop / onExternalDragEnd / beginExternalDrag`, `ExternalDrop`,
  `AppWindow::pathsFromUriList`, `deliverExternalDrop`.
- **In the tree, unbuilt**: `U/App/src/win32_dnd.cpp` (422 lines; compiles alone, nothing calls it);
  `U/tests/test_app_xdnd.cpp`, `test_app_x11_widget_dnd.cpp`, `test_app_win32_ole.cpp`.
- **Branch deltas not taken**: the XDND code is inside `U/App/src/native.cpp` (+535 / -43, 21 conflict hunks);
  `U/App/include/fuse/app/app.hpp` (+105), `native.hpp` (+42), `U/App/src/app.cpp` (+174 / -86),
  `native_internal.hpp`, `U/App/CMakeLists.txt`.
- **Overlap on main**: `UiContext::dropExternal` and in-app drag and drop exist (`Core/src/interaction.cpp`,
  `widgets/interaction.hpp`); KNOWN_GAPS lists "backends do not feed OS file drops" and "no drag from the app out to
  the OS". Main's `native.cpp` was restructured (`native_common.cpp`, `native_services.inl`), so the XDND hunks do not
  apply.
- **Blocks**: re-apply the XDND state machine to main's X11 event loop by hand and route drops into
  `UiContext::dropExternal` (main's API) instead of the branch's `onExternalDrop` callback, or add the callback on top.
- **Status (2026-11-13)**: X11 and Win32 are live on main's core API (§2.0); the Win32 half has not been run.
  **Still not ported**: what `U/App/src/win32_dnd.cpp` has beyond files and text, which is why that file stays
  parked: registered MIME clipboard formats both ways, the shell drag image (`IDragSourceHelper` /
  `IDropTargetHelper`), copy / link drag cursors; on X11 the same limits (only `text/uri-list` and text types are
  taken or offered, no INCR drops, no drag cursor over other applications, the proposed action is always copy).
  `U/tests/test_app_win32_ole.cpp` stays parked (written against the branch's API; needs Windows or Wine). The
  Cocoa part is untouched (3.3, never compiled).
- **Verify**: `fuse_app_xdnd`, `fuse_app_x11_widget_dnd` (Xvfb, a second X client as the peer), `fuse_app_win32_ole`
  (Windows / Wine); main's `fuse_app_native_window`, `fuse_ui_drag_drop`, `fuse_ui_inventory_dnd` gates unchanged.

### 3.2 Wayland backend (L, decision: see §6): **mb's backend is live (2026-11-13); remainder of this one below**

Status after the port: main has a live Wayland backend (`U/App/src/native_wayland.cpp`, mb 3.6, see
[PORT_magical_bassi.md](PORT_magical_bassi.md) §2). From this branch it took the Vulkan Wayland surface, the frame
throttle and the in-process compositor test approach (§2 above). **Not ported from this branch, which is why its files
stay parked** (`U/App/src/wayland.cpp`, `U/App/include/fuse/app/wayland.hpp`, `U/App/src/wl_protocol.hpp`,
`U/tests/test_app_weston.cpp`, `U/_port/gui-framework/tests/test_app_wayland.cpp`):

- drag out of the app (`wl_data_device.start_drag` with several formats and a pixmap icon surface, `dnd_finished`).
  Main's OS drag-out now exists for X11 / Win32 on the core drag API (3.1: `UiContext::beginDrag` leaving the
  window, `dragFinishExternal`, `NativePlatform::externalDragActive`); the Wayland side would start the
  `wl_data_device` drag from the same place (`externalDragActive()` is false on Wayland until then);
- tool windows as `wl_subsurface`s that follow a title drag (the live backend has child `xdg_toplevel`s in its
  protocol layer instead; `NativeOptions::osWindows` / `NativePlatform::toolWindow*` are X11 / Win32 only and answer
  0 / false on Wayland: 3.5 and `PORT_magical_bassi.md` 3.6 say what wiring needs);
- the in-app-drag cursor feedback and the `fuse_app_weston` gate that drives a nested weston with xdotool;
- the `--vulkan` / `--vulkan-gpu` variants of the in-process test (the WSI's committed buffers vs the CPU raster).

Do not build `wayland.cpp` beside the live backend: take the pieces above into `native_wayland.cpp` and delete the
five files when done. The original description follows.

- **What**: native libwayland-client backend loaded at run time: xdg-shell toplevels and popups, `wl_shm`
  presentation, seat (pointer, keyboard with xkbcommon, client-side key repeat), data-device drag and drop in and
  out, text-input-v3, fractional-scale + viewporter, cursor-shape, subsurfaces for tool windows, frame-callback
  throttling, Vulkan `VK_KHR_wayland_surface`.
- **In the tree, unbuilt**: `U/App/include/fuse/app/wayland.hpp`, `U/App/src/wayland.cpp` (1182 lines),
  `U/App/src/wl_protocol.hpp` (hand-written protocol tables); tests `U/tests/test_app_weston.cpp` and
  `U/_port/gui-framework/tests/test_app_wayland.cpp` (in-process compositor).
- **Branch deltas not taken**: `native_internal.hpp` (`SurfaceSource::Wayland`, cursor image fields),
  `vk_present.cpp` (+326: Wayland / Android / Metal surfaces, HDR), `native.cpp` (backend selection).
- **Blocks**: 47 compile errors against main, all from the branch's `native_internal.hpp` seam (`SurfaceSource::Wayland`,
  custom cursor images) and `NativeOptions` fields. It uses `fuse::ui::os` (`loadLibrary`, `SharedBuffer`, pipes),
  which `os_std.cpp` now provides.
- **Verify**: `fuse_app_wayland` (in-process compositor, no system compositor needed), `fuse_app_wayland_vk`,
  `fuse_app_wayland_vk_gpu`, `fuse_app_weston` (headless weston).

### 3.3 Apple backends: Cocoa, UIKit, Metal present, AVFoundation, NSStatusItem, WKWebView (L, **never compiled**)

- **What**: the branch's own macOS and iOS backends. Its commit messages mark all of it UNVERIFIED (`03606b74a`,
  `d9fd0381d`, `e21adbb26`, `9a0caf26f`, `d81ac868c`): no Apple SDK was available on the branch either.
- **In the tree, unbuilt**: `U/App/include/fuse/app/{cocoa,uikit}.hpp`, `U/App/src/{cocoa.mm,uikit.mm}`,
  `U/Extras/src/{tray_cocoa.mm,webview_cocoa.mm,media_avf.mm}`; test `U/tests/test_mobile_map.cpp` (needs
  `A11yState` from 3.6).
- **Overlap on main**: `U/App/src/native_apple.mm` and `entry_ios.mm` are main's Cocoa / UIKit backends; they build in
  CI (`fuse-core-ios.yml`). The merge did not touch them.
- **Blocks**: do not replace main's files. Port single features into them after reading the branch code as a sketch:
  NSAccessibility / VoiceOver elements from `snapshotA11y`, `CVDisplayLink` pacing, `UITextInput` marked text, Cocoa
  drag-in; then `tray_cocoa.mm` and `webview_cocoa.mm` can be added to `fuse_ui_extras` on macOS.
- **Verify**: only on a Mac / iOS simulator. `fuse_app_cocoa_map` (live) and `fuse_app_mobile_map` cover the tables.

### 3.4 Android: branch backend, Java `FuseActivity`, APK script (M, decision: see §6)

- **What**: `android_native_app_glue` backend; `org.fuse.ui.FuseActivity` (Java): IME through an `InputConnection`
  (commit and composing text), TalkBack through an `AccessibilityNodeProvider`; Gradle-free APK build and an emulator
  smoke script.
- **In the tree, unbuilt**: `U/App/include/fuse/app/android.hpp`, `U/App/src/{android.cpp,android_map.hpp}`,
  `U/App/android/java/org/fuse/ui/FuseActivity.java`, `U/App/android/{build_apk.sh,run_emulator_smoke.sh}`,
  `U/App/samples/android_hello.cpp`, `U/_port/gui-framework/App/android/AndroidManifest.xml`.
- **Overlap on main**: `U/App/src/native_android.cpp` + `entry_android.cpp` (NativeActivity, JNI clipboard / key
  characters), the Gradle template under `U/App/android/`. KNOWN_GAPS: "no IME string commits (needs a Java
  `InputConnection`)". mb has a third backend with JNI IME (mb 3.10).
- **Blocks**: keep main's backend; add the Java activity and the JNI entry points it calls
  (`FuseActivity.java` names them) to `native_android.cpp`; `android_map.hpp` needs `A11yState` (3.6).
- **Verify**: Android cross-build; `run_emulator_smoke.sh` on an emulator; `fuse_app_mobile_map`.

### 3.5 OS tool windows, frame pacing, live retitle, window options (M, decision: see §6)

- **What**: floating docks and toolbars as OS windows (X11 / Win32 / Wayland), `Presenter::vsynced`, max frame rate,
  `WindowDesc::dpiScale`, `NativeOptions::colorSpace`, live window retitle.
- **Branch deltas not taken**: `U/App/src/native.cpp`, `app.cpp`, `vk_present.cpp`, `U/App/include/fuse/app/{app,native}.hpp`;
  tests `test_app.cpp` (+114: `hidpi`, `frame_pacing` cases), `test_app_native.cpp` (+22), `test_app_os_popups.cpp` (+77).
- **Overlap on main**: popups as OS windows on X11 / Win32 (`fuse_app_os_popups`), render scale (`a925efbd1`),
  window-state service; floating docks are in-window panels (KNOWN_GAPS).
- **Blocks**: same files as 3.1; port after it.
- **Status (2026-11-13)**: tool windows (X11 / Win32; not on Wayland, see 3.2) and frame pacing are live (§2.0).
  On Wayland `Presenter::vsynced` is set for the Vulkan FIFO swapchain; `wl_shm` windows are held back by the
  `wl_surface.frame` throttle of the live backend (`wl::frameReady`, which replaces the branch's
  `setFramePending`) and `setMaxFrameRate` applies to them as well.
  **Not ported**: `NativeOptions::colorSpace` (GPU, with 3.11).
- **Verify**: `fuse_app_hidpi`, `fuse_app_frame_pacing`, the tool-window cases of `fuse_app_os_popups`.

### 3.6 Accessibility: `PlatformAdapter`, `A11yState`, auto-complete state (M, decision: see §6)

- **What**: `a11y::PlatformAdapter(Bridge&, nativeWindow)` choosing UIA / NSAccessibility / AT-SPI through AccessKit;
  `A11yState` (expanded, autoComplete, controls, activeDescendant), `A11yAutoComplete`.
- **Branch deltas not taken**: `U/A11y/include/fuse/ui/a11y/a11y.hpp` (+29), `U/A11y/src/a11y.cpp` (+16),
  `a11y_accesskit.cpp` (+102), `U/tests/test_a11y_bridge.cpp` (+143), the a11y parts of `ui.hpp`.
- **Overlap on main**: grid / tree roles (`941737b9c`, gates `fuse_ui_a11y_family_*`); mb's `WindowAccessibility` (mb 3.9).
- **Blocks**: three-way merge of `a11y.hpp` (main, mb, this branch). AccessKit needs a Rust toolchain.
- **Verify**: `fuse_ui_a11y_bridge` with the branch's cases; `fuse_app_mobile_map`.

### 3.7 Core widgets: date / time, calendar, item views, toolbars; what is left of tabs (M-L)

**Ported 2026-11-13** (see §2): line edit options, validators, completer, editable combo box, spin box options, tab
bar closable / movable / disabled / scroll buttons / pages. How it was done, for the rest: the branch's behaviour
lives in new files and is reached through a per-node option record (`detail::lineEditHasState` and friends), so a
widget nobody set options on keeps main's code path and pixels. The same pattern fits what remains.

- **Still not live** (branch code: `git show 363ecf87d:Source/FUSE/UI/Widgets/src/<file>`):
  - Tab bar: `TabPosition` (South / West / East, vertical bars), `ElideMode`, per-tab icons (`setTabIcon`) and
    tooltips (`setTabToolTip`). `widgets_tabs.cpp` has the state struct to extend; vertical bars need `measure` and
    `tabRect` along the other axis. (`widgets_tier_b.cpp` delta, `TabBarState`.)
  - `dateEdit` / `timeEdit` / `DateTimeEditOptions` (display-format sections, locale names, min / max, calendar
    popup): `widgets_tier_d2.cpp` delta (+540 / -106); test `U/tests/test_datetimeedit.cpp` (parked). Main's
    `dateTimeEdit` has fixed fields and a different signature (`dateTimeEditValue`): add the branch's as
    `dateTimeEditOptions` on main's widget, do not replace it.
  - `CalendarOptions` (first day of week, ISO week numbers), `setCalendarRange`, `calendarShownPage`: `widgets_tier_cd.cpp`
    delta; test `U/tests/test_calendar.cpp` (parked). Main's calendar takes the first day from the locale hooks.
  - Tree view: scrolling (`treeFirstVisible`), `SelectionMode` on the string tree, `setTreeDragDrop` (internal move,
    cross-view drops, auto-scroll, auto-expand): `widgets_tier_b.cpp` delta + `U/Widgets/src/item_selection.hpp`
    (parked). Main's answer for trees is `treeModelView`; the string `treeView` got `treeLineAt` / `treeLineRect`
    (from magical-bassi) for application-side drops.
  - Table: `TableDragDrop` (multi-row drags, cross-view drops), `HeaderResizeMode`, header menu, `tableMoveColumn`:
    `widgets_tier_b2.cpp` delta. Main has `setTableDragDrop(ui, table, flags)` (same name, other signature), frozen /
    hidden / reorderable columns and resize-to-contents in `modelview.hpp`.
  - `ListViewMode::Icon`, `setListItemIcon`; `splitterSizes` / `setSplitterSizes` / collapsible panes; tool bar
    overflow button, `toolBarArea`, movable / floating tool bars: `widgets_tier_b.cpp` / `_b2.cpp` deltas.
  - MIME-typed in-app drag and drop (`DragPayload`, `DropPhase`, `setDropTarget`, `setDragSource`):
    `U/Widgets/src/widgets_dnd.cpp` (parked) is a second drag-and-drop model; main's is in `fuse_ui_core`
    (`DragData`). Port the MIME payload idea into main's, do not build both.
  - The rubber-band selection of the branch's item views. (The graphics view has rubber-band selection: §3.12.)
- **Parked tests for these**: `U/tests/test_{datetimeedit,calendar,dnd,item_views,widget_gaps}.cpp`.
- **Differences from the branch in what was ported**: options are opt-in per node (the branch changed every spin box
  / line edit); `regexValidator` has no `std::regex` fallback (main builds without exceptions in mind: an
  unsupported pattern accepts everything and says so through `regexPartialMatch`); accessibility is main's snapshot
  (no `A11yState::autoComplete / controls / activeDescendant`, no `protectedText`: the node text of a password
  field is the mask); a press on the completion list or the combo's drop button takes the focus off the field until
  the release (main moves the focus on every press), which the field treats as "still editing".
- **Verify**: the gates in §2; main's `fuse_ui_tier_*`, `fuse_ui_widget_*`, `fuse_ui_mv_*`, `fuse_ui_dock_*` and all
  goldens unchanged.

### 3.8 Style engine: `@keyframes`, Web Animations, selectors, at-rules, positioning (L)

- **What**: CSS animations and a Web-Animations-style API (`UiContext::animate`, `AnimationHandle`, scroll / view
  timelines, ranges); selectors (attribute, `:nth-*`, sibling combinators, `:not / :is / :where / :has`, `:empty`)
  with invalidation sets; `@media`, `@supports`, `@container`, `@layer`, `@scope`, nesting, `style()` queries,
  container / viewport units; `position` + insets, `z-index` stacking contexts, `align-content`, gaps,
  `aspect-ratio`, grid masonry / subgrid; `"animate"` and `"order"` keys in `.fuseui`.
- **In the tree, unbuilt**: tests `U/tests/test_css_{animations,cascade,position,selectors4,transitions}.cpp`,
  `test_web_animations.cpp`, `test_style_invalidation.cpp`, `test_transform_timelines.cpp`, `test_doc_animate.cpp`.
- **Branch deltas not taken**: `U/Core/src/style.cpp` (+3078 / -120), `style.hpp` (+525), `ui.cpp` (+2507 / -124),
  `ui.hpp` (+354), `handle.hpp` (`AnimationHandle`), `U/Doc/src/{document,cook}.cpp`, `document.hpp`, `cook.hpp`.
- **Overlap on main**: transitions, `linear-gradient`, flex-wrap, grid templates in main's `style.cpp`; main's
  animation members on `UiContext` (the duplicates named in §1). KNOWN_GAPS: "no `@keyframes` in style sheets".
- **Blocks**: main's and the branch's `style.cpp` both grew from base in the same functions (13 conflict hunks, 583
  lines). Port by feature in this order: selectors + invalidation, at-rules, `@keyframes`, the scripted animation API
  (reconcile with main's `animate`), positioning / stacking.
- **Verify**: the nine gates above (`fuse_ui_css_*`, `fuse_ui_web_animations`, `fuse_ui_style_invalidation`,
  `fuse_ui_transform_timelines`, `fuse_ui_doc_animate`); main's `fuse_ui_style_cascade`,
  `fuse_ui_layout_conformance`, `fuse_ui_bench_10k` and all goldens unchanged.

### 3.9 Drawing core: one path / gradient API, transforms, filters, masks (L, decision)

- **What**: the branch's `Path` (edge list), `DrawList::path / gradientPath / pushClipLayers / pushProjective`,
  `Gradient` with focal point and `toUnit`, `ClipLayer`; 2D affine and 3D transforms (perspective, preserve-3d,
  backface, BSP splitting) with transformed hit testing; CSS filters and `backdrop-filter` functions, `drop-shadow`,
  SVG `<filter>` programs (`FilterProgram`, `FeOp`); `clip-path` shapes, `mask-*`, `background-clip`.
- **In the tree, unbuilt**: `U/Core/include/fuse/ui/css_masking.hpp`, `U/Core/src/css_masking.cpp` (657 lines),
  `U/Core/src/filter_program.cpp` (1136), `U/_port/gui-framework/Core/src/path.cpp`; tests
  `U/tests/test_{transform_affine,transform_3d,css_masking,css_gradient,color_path}.cpp`.
- **Branch deltas not taken**: `U/Core/include/fuse/ui/draw_list.hpp` (+360), `U/Core/src/draw_list.cpp` (+999 / -66),
  `test_backdrop_blur.cpp` (+330).
- **Overlap on main**: `fuse/ui/path.hpp` (`Path`, `Affine`, `StrokeStyle`, flatten / stroke / coverage),
  `DrawList::fillPath / strokePath / pushClipPath / pushLayer`, `Brush`, N-stop linear / radial / conic `Gradient`,
  `Painter`, `SvgImage`. Same names, different types: `Path`, `Gradient`, `GradientStop`, `GradientKind`.
- **Blocks**: **decision**: main's path API is the public one (the live Extras already run on it through
  `path_compat.hpp`). Port onto it what it lacks: focal radial gradients, layered clips / masks, projective
  transforms, filter programs. `css_masking.cpp` and `filter_program.cpp` then need their `Path` / `ClipLayer` uses
  rewritten and the style properties from 3.8 (`maskImage`, `maskSize`, interned source text).
- **Verify**: `fuse_ui_transform_affine`, `fuse_ui_transform_3d`, `fuse_ui_css_masking`, `fuse_ui_css_gradient`,
  `fuse_ui_color_path`, `fuse_ui_backdrop_blur` with the branch's cases; main's `fuse_ui_vector_*`, `fuse_ui_svg`,
  `fuse_ui_cpu_raster_golden` and every golden unchanged.

### 3.10 GPU renderer: paths, gradients, clips, filters, backdrop kernel, incremental pack, mesh pass (L; see §6)

- **What**: native GPU path / gradient / clip-layer / filter-layer ops and GPU text runs in `ui.draw_list`, a
  separate `ui.backdrop` blur kernel, damage-box partial repaint, persistent / incremental GPU buffers, GLSL ->
  SPIR-V build step with checked-in fallbacks, the `ui.mesh3d` pass for charts.
- **In the tree, unbuilt**: `U/Render/include/fuse/ui/render/ui_vk_mesh.hpp`, `U/Render/src/ui_vk_mesh.cpp`;
  shaders `U/Render/src/shaders/{ui_backdrop.comp,ui_backdrop.glsl,ui_backdrop_hdr.comp,ui_clip.glsl,ui_draw_list.glsl,ui_draw_list_hdr.comp,ui_mesh3d.vert,ui_mesh3d.frag,ui_mesh3d_exact.frag}`
  and `fixtures/{ui_backdrop,ui_backdrop_hdr,ui_draw_list_hdr,ui_mesh3d.vert,ui_mesh3d.frag,ui_mesh3d_exact.frag}*.spv`;
  `U/tests/bench_render.cpp`, `U/Core/src/simd/*` (3.15).
- **Branch deltas not taken**: `U/Render/include/fuse/ui/render/{gpu_draw_data,ui_vk}.hpp` (+175, +72),
  `U/Render/src/gpu_draw_data.cpp` (+1386 / -77), `ui_vk.cpp` (+520), `shaders/ui_draw_list.comp` (the branch moved
  its body to `ui_draw_list.glsl`) and its fixture `.spv`, `U/tests/test_vk_parity.cpp` (+1045), `vk_headless.hpp`.
- **Overlap on main**: main packs vector ops as pre-rasterised Image commands (`fuse_ui_gpu_pack_vector`); mb has
  GPU blur, damage present, SDF quads and effects against a third `GpuDrawData` layout (mb 3.4).
- **Blocks**: 3.9 first (the kernel ops mirror the branch's draw-list ops). Then one `GpuDrawData` layout for main +
  mb + this branch; shader and fixtures regenerated together. `ui_vk_mesh.cpp` is independent of that: it needs only
  a Vulkan loader and `mesh3d.hpp` (live), and can be added to `fuse_ui_vk` on its own (S-M).
- **Verify**: `fuse_ui_vk_parity` (with the branch's cases incl. "charts", "dataviz3d"), `fuse_ui_bench_render`
  (trend tool, no thresholds); needs lavapipe. Nothing here was run for the merge (no Vulkan loader).

### 3.11 Colour management and HDR (M, after 3.10)

- **What**: sRGB / P3 / Rec.2020 conversion and PQ / HLG transfer functions, HDR10 / scRGB swapchains and encode
  kernels, RGBA16F targets, float CPU raster (`HdrImage`), `os::displayHdr`.
- **In the tree, unbuilt**: `U/Core/include/fuse/ui/color_space.hpp` (header only, standard library only; unused);
  shaders `ui_encode_{pq,scrgb}{,_hdr}.comp`, `ui_encode_{pq,scrgb}.glsl` and their four fixtures;
  `U/tests/{test_cpu_hdr.cpp,test_vk_hdr.cpp,test_vk_encode.cpp,hdr_scene.hpp}`.
- **Branch deltas not taken**: HDR parts of `draw_list.hpp / .cpp`, `ui_vk.cpp`, `vk_present.cpp`.
- **Blocks**: `HdrImage` and `color::hdr` live in the branch's `draw_list.hpp`; `displayHdr` is FORGE-only
  (`os_std.cpp` reports "unknown").
- **Verify**: `fuse_ui_cpu_hdr`, `fuse_ui_vk_hdr`, `fuse_ui_vk_encode`.

### 3.12 Extras widgets: done

**Ported 2026-11-13** (see §2). `U/Extras/include/fuse/ui/extras/widgets_extra.hpp`, `U/Extras/src/{widgets_extra.cpp,
buttonbox.cpp}` and `U/tests/test_buttonbox.cpp` were deleted; the gates `containers dialogs lcd_keyseq mdi completer
graphics vkeyboard` were removed from `U/tests/test_extras.cpp` (their checks are in `U/tests/test_widgets_extra.cpp`
and `test_completer.cpp`). `extras/extras.hpp` (umbrella, still parked for `gesture.hpp` / `i18n.hpp` / `richtext.hpp`)
includes `widgets/widgets.hpp` instead.

- Not taken, because main has its own: the branch's `groupBox`, `stackedWidget`, `statusBar`, `messageBox`,
  `wizard`, `MessageIcon`, `keyName`, and `attachCompleter` / `completions` (superseded by `setCompleter`, §3.7).
- Changed while porting: the key sequence edit works on main's `Shortcut` / `shortcutToString` (no second key-name
  table); the graphics view's double click uses the core's click count instead of an OS clock; MDI sub-windows clamp
  to their area, report activation and handle a cancelled touch; the on-screen keyboard keeps its own target (main
  moves the focus on a press); Enter reaches a dialog's default button only when nothing inside consumed it.
- Left in `U/tests/test_extras.cpp` behind `FUSE_UI_EXTRAS_ALL`: `widget_pdf` (needs `renderDrawList` to draw
  main's path ops: 3.13).

### 3.13 Extras documents and media: rich text, SVG documents, system video, audio (M each)

- **Rich text**: **ported 2026-11-13** (§2). How the two rich-text implementations were reconciled: there is one
  model, `rich::RichDocument`. The Extras `richtextedit` widget edits it; main's read-only `richText` view
  (`widgets_tier_b3.cpp`) is its renderer through the view's own markup (`RichDocument::toRichTextMarkup /
  fromRichTextMarkup`, `widgets::richTextMarkup(ui, editor)`). Both resolve a run's face the same way
  (`UiContext::fontFor(family, bold ? 700 : weight, italic)`) and draw underline / strike-through from the font's
  decoration metrics, and the gate checks that a line lays out to the same width in both. The branch's
  double-strike bold and sheared oblique (`rich::obliqueGlyphs`) are gone; `rich::layout` takes a `Measure(text, px,
  format)` because a real bold face has its own advances. Undo is the Extras `UndoStack` (own, or shared through
  `setUndoStack`). What the view cannot show: block formats (alignment, lists, headings, indent) are flattened in
  view markup. The view was not rebuilt on `RichDocument` because `fuse_ui_widgets` cannot depend on `fuse_ui_extras`;
  moving `RichDocument` into the widget library would remove that limit.
- **SVG documents** (`fuse::ui::svg`, `svgWidget`): **still parked.** `extras/svg.hpp`, `Extras/src/svg.cpp` (993
  lines) were not ported and not deleted. Target: main's `SvgImage` (`U/Core/src/svg.cpp`), which stores a flat list of
  shapes. To take `clipPath`, `mask`, group `opacity` and filters it needs a group record in that list (begin / end
  with a clip path or a layer: `Painter::pushClipPath / pushLayer` exist); `<use>` needs elements kept by id;
  `<text>` needs a font at render time (`SvgImage::render` takes only a `Painter`) or `Font::glyphOutline`, a branch
  delta in `U/Text` that is not on main; patterns need a tiled paint the draw list does not have; a focal radial
  gradient needs `Gradient` to carry the focal point. Gate to bring along: `fuse_ui_extras_svg`.
- **Media**: the audio level meter is **ported** (§2, `extras/audio_meter.hpp`). `SystemVideo` (GStreamer / Media
  Foundation / AVFoundation) and the audio output (`U/Os/src/audio_out.cpp`) are **still parked**: `media.cpp` needs
  `VideoSource::playback` and `::clock` (branch delta in the video widget), and no portable part is left once the
  meter is out. `media.hpp` now includes `audio_meter.hpp`.
- **PDF of vector content**: **ported** (§2). Paths, even-odd fills, strokes with dashes and path clips are written;
  a gradient paint is written as the mean colour of its stops (no PDF shading), `NinePatch` and `PushLayer` effects
  are still skipped. The branch gate `fuse_ui_extras_widget_pdf` (in `test_extras.cpp`, needs `widgets_extra`) is
  still parked.
- **Charts through the widgets API** (`ChartKind::Pie / Donut`, `chartSetAxis`, ...): not looked at in this round.

### 3.14 FORGE script bindings, hot reload on the seam, Designer pass, public header (S-M each)

- **Script**: `U/Script/include/fuse/ui/script/forge_ui.hpp`, `U/Script/src/{forge_ui.cpp,forge_remote_off.cpp}`,
  `U/tests/test_script.cpp`: the `ui::` functions for FORGE script. Needs FORGE (§5). Main keeps Lua. Port only if
  FORGE script becomes a supported language; the binding surface (`ui::find`, charts calls) can be read as a list of
  what the Lua binding lacks.
- **Hot reload**: **ported 2026-11-13** as `doc::StyleSheetFileWatcher` (§2). Main keeps its poll design
  (`DocumentFileWatcher`); the style-sheet part of the branch's `HotReload` was the missing piece and is built on
  `UiContext::replaceStyleSheet`. `os::FileWatcher` is not used: on main it is itself a polling watcher
  (`os_std.cpp`) and `fuse_ui_doc` does not depend on `fuse_ui_os`. The `_port/gui-framework/Doc` copies are deleted
  and `U/tests/test_hot_reload.cpp` is rewritten for the watcher.
- **Designer product pass** (clipboard, shortcuts, validation, preview, device frames): branch deltas
  `U/Designer/include/fuse/ui/designer/designer.hpp` (+53), `U/Designer/src/designer.cpp` (+297),
  `designer_main.cpp`, `U/tests/{test_designer.cpp (+123),test_ui8.cpp (+59)}`. mb changed the same files (mb 3.15).
- **`<fuse/gui.hpp>`**: **ported 2026-11-13** (§2). The header includes the live modules only (the Extras when
  the tree was built with them: `FUSE_GUI_HAS_EXTRAS`); `fuse::gui` is an interface target inside the UI build, it
  is not exported by the FuseUI package.

### 3.15 SIMD: box blur and chart kernels (S, FORGE or rewrite)

- `U/Core/src/simd/{box_blur.cpp,box_blur.hpp,box_blur_avx2.cpp,box_blur_kernel.hpp}`, `U/tests/test_simd_blur.cpp`,
  `U/Extras/src/{charts_simd_avx2.cpp,charts_simd_neon.cpp}`: kernels written on `forge::lane::vec` with FORGE's CPU
  detection. Charts fall back to the scalar kernel on main. To use them without FORGE, replace `forge/lane/vec.hpp`
  by intrinsics or `std::experimental::simd` and CPU detection by `__builtin_cpu_supports` / HWCAP. Gate
  `fuse_ui_simd_blur`; `fuse_ui_charts_decimation` already checks the scalar path.

### 3.16 Remaining Extras modules and platforms (S each)

- Gesture recogniser: **ported 2026-11-13** (§2) as `fuse::ui::gesture` (its `GestureConfig` / `TouchPhase` no longer
  collide with main's). Compared with main's core gestures it adds pan with offset and release velocity, swipe
  with a direction and a finger count, for raw points that do not go through `UiContext::touch`. Backends still do
  not feed pinch / rotate from OS gesture events (KNOWN_GAPS); that is platform work, not this module.
- `.po` catalogues: **ported 2026-11-13** into `fuse_ui_l10n_lib` as `l10n::parsePo / poKey / poToTable`
  (`U/L10n/src/po.cpp`, gate `fuse_ui_l10n_po`): msgctxt, multi-line strings, fuzzy and untranslated entries, and
  plural entries turned into ICU plural messages on the CLDR categories by evaluating the catalogue's own
  Plural-Forms expression, so a `.po` file feeds `StringTable` like a `key=value` table. What is left in the parked
  `extras/i18n.hpp` / `Extras/src/i18n.cpp` and is covered elsewhere on main: number formatting (`l10n::formatNumber`),
  layout direction (the core's `direction` / RTL), plural indices (CLDR rules in `l10n`). Not covered: `%1`..`%9`
  positional formatting (`i18n::format`; ICU messages use `{0}`) and the process-wide `installTranslator`. The two
  files are still in the tree because the parked `extras.hpp` umbrella and `gateI18n` in `test_extras.cpp` name them;
  delete them with the `widgets_extra` port (3.12).
- Windows: turn `FUSE_BUILD_UI_EXTRAS` on in `fuse-windows-native.yml`, fix what MSVC reports, run
  `fuse_ui_tray_win32`; implement `os::runProcess` with `CreateProcessW`; WebView2 needs `FUSE_UI_WEBVIEW2_DIR`
  (`Extras/src/compat/mingw/EventToken.h` is the MinGW shim).
- Apple: turn it on in `fuse-core-ios.yml` (macOS job first). `os_std.cpp` has an untested Apple path
  (`TargetConditionals.h`; no helper processes on iOS).
- Scripts and CI: `scripts/gui/*` and `docs/gui/ci/fuse-gui.yml` assume the branch's tree (FORGE submodule, branch
  gates). Either adapt the per-platform scripts to main's gates or delete them once main's workflows cover the same.

## 4. Existing UI files: what the merge kept

All of these are main's content, byte for byte, except the three build files in the last row.

| File (under `U/`) | Branch delta (+ / -) | Belongs to |
|---|---|---|
| `A11y/include/fuse/ui/a11y/a11y.hpp`, `A11y/src/a11y.cpp`, `A11y/src/a11y_accesskit.cpp`, `tests/test_a11y_bridge.cpp` | +29, +16, +102, +143 | 3.6 |
| `App/CMakeLists.txt`, `App/include/fuse/app/{app,native}.hpp`, `App/src/{app,native}.cpp`, `App/src/native_internal.hpp` | +29, +105, +42, +174 / -86, +535 / -43, +9 | 3.1, 3.2, 3.5 |
| `App/src/vk_present.cpp` | +326 / -45 | 3.2, 3.5, 3.11 |
| `tests/{test_app,test_app_native,test_app_os_popups}.cpp` | +114, +22, +77 | 3.5 |
| `Core/include/fuse/ui/{draw_list,style,ui,handle}.hpp` | +360, +525, +354, +9 | 3.8, 3.9, 3.11 |
| `Core/src/{draw_list,style,ui}.cpp` | +999 / -66, +3078 / -120, +2507 / -124 | 3.8, 3.9 |
| `Designer/**`, `tests/{test_designer,test_ui8}.cpp` | +351, +182 | 3.14 |
| `Doc/include/fuse/ui/doc/{cook,document}.hpp`, `Doc/src/{cook,document}.cpp` | +1, +6, +251, +184 | 3.8 (`"animate"`, `"order"`) |
| `Doc/include/fuse/ui/doc/json.hpp`, `Doc/src/json.cpp` | +10, +2 (`JsonMember` struct instead of `std::pair`) | none: main solved the same Clang problem with out-of-line members (mb merge) |
| `Game/src/game_extras.cpp` | +1 (`<cstdlib>`) | none |
| `Render/include/fuse/ui/render/{gpu_draw_data,ui_vk}.hpp`, `Render/src/{gpu_draw_data,ui_vk}.cpp`, `shaders/ui_draw_list.comp` + fixture, `tests/{test_vk_parity.cpp,vk_headless.hpp}` | +175, +72, +1386, +520, -187, +1045, +22 | 3.10, 3.11 |
| `tests/test_backdrop_blur.cpp` | +330 | 3.9, 3.10 |
| `Script/include/fuse/ui/script/lua_ui.hpp`, `Script/src/lua_ui.cpp`, `tests/test_lua.cpp` | deleted by the branch | none: kept |
| `tests/data/settings.fuseui` | comment only | none |
| `Text/include/fuse/text/text.hpp`, `Text/src/text.cpp` | +12, +36 (`Font::glyphOutline`, file reads through the seam) | 3.13 (SVG text) |
| `Video/src/theora_video.cpp` | +4 (file read through the seam) | none |
| `Widgets/include/fuse/ui/widgets/widgets.hpp`, `Widgets/src/{widgets,widgets_tier_b,widgets_tier_b2,widgets_tier_cd,widgets_tier_d2}.cpp`, `widgets_internal.hpp`, `tests/{test_ui6,test_tier_d2}.cpp` | +564, +894, +1560, +1207, +584, +540, +13, +16, +4 | 3.7, 3.12, 3.13 |
| `tests/test_bench_10k.cpp` | +22 (`FUSE_UI_PERF_SCALE` budget multiplier, best-of-batches median) | none: main has `tests/test_env.hpp` from the mb merge; take the multiplier if emulated CI hosts need it |
| `CMakeLists.txt`, `tests/CMakeLists.txt`, `cmake/fuse_ui_install.cmake` | main + `fuse_ui_os`, `fuse_ui_extras` and their gates (§2) | every item (targets and gate registrations) |

## 5. FORGE

`gui-framework` added a git submodule `vendor/forge` -> `https://github.com/odin-loki/FORGE.git` at `944b053d` and
made it a hard dependency (`FUSE_UI_OS_BACKEND=forge` was the default and configure failed without it). The merge
did not take `.gitmodules` or the gitlink. The FORGE repository could not be read from the merge session, so its
licence, size and contents are known only from how the branch uses it. THIRD_PARTY.md has no entry for it.

What the branch used it for, and the state on main:

| Use | Branch code | On main |
|---|---|---|
| OS layer `forge::os` (files, env, clocks, locks, dynamic libraries, file watch, clipboard, process spawn, pipes, `display_hdr`) behind `fuse::ui::os` | `U/Os/src/os_forge.cpp` (434 lines) | Unbuilt. Replaced by `U/Os/src/os_std.cpp`. `-DFUSE_UI_OS_BACKEND=forge -DFUSE_FORGE_DIR=<checkout>` builds it instead (not tried: no checkout). |
| The seam used by core code: font and video file reads, settings, single instance, hot reload, Wayland fds | branch deltas in `Text/src/text.cpp`, `Video/src/theora_video.cpp`, `App/src/app.cpp`, `Doc/src/hot_reload.cpp`, `App/src/wayland.cpp` | Not taken: main's core does not use the seam. Only `fuse_ui_extras` links `fuse_ui_os`. |
| SIMD `forge::lane` (vector type, CPU detection, `fuse_forge_lane` library built from FORGE's `lane/src`) | `U/Core/src/simd/*`, `U/Extras/src/charts_simd_{avx2,neon}.cpp` | Unbuilt; charts use the scalar kernel (3.15). With the forge backend the AVX2 chart kernel is added on x86-64. |
| Script runtime `forge::script` (built from FORGE's `script/src`, patched in the build dir with `cmake/forge-patches/0001-script-host-bindings.patch` when the checkout lacks `host_fn`), `forge::json` | `U/Script/src/{forge_ui.cpp,forge_remote_off.cpp}`, `forge_ui.hpp`, `U/tests/test_script.cpp`; CMake function `fuse_add_forge_script()` in `cmake/FuseForge.cmake` | Unbuilt and not wired: main's `fuse_ui_script` is the Lua binding. `fuse_add_forge_script()` is kept for the port (3.14) and is called by nothing. |

`cmake/FuseForge.cmake` on main: `FUSE_FORGE_DIR` defaults to empty; `fuse_add_forge()` stops with a message that
names the two ways out (pass a checkout, or use the default std backend). It is included only when
`FUSE_UI_OS_BACKEND=forge`.

## 6. Features both branches implement: which one to port

| Feature | `gui-framework` (this list) | `claude/magical-bassi-815825` ([mb list](PORT_magical_bassi.md)) | Recommendation |
|---|---|---|---|
| Wayland backend (**done 2026-11-13 as recommended; drag out remains, see 3.2**) | 3.2: `wayland.cpp`, 1182 lines + `wl_protocol.hpp`; uses the `fuse::ui::os` seam for libraries and fds; has data-device drag out, Vulkan `VK_KHR_wayland_surface`, frame-callback throttle; gate with an in-process compositor (no weston needed) plus a weston gate | mb 3.6: `native_wayland.cpp`, 2093 lines, self-contained (dlopen); also primary selection, xdg-decoration, tablet tool, positioner-based popups, child tool windows, window chrome hooks; gates against headless weston and sway with a second client | **Port mb's** as the backend (wider protocol coverage, no seam dependency, tested against real compositors, and the editor shell expects its tool windows). Then take from this branch: the Vulkan Wayland surface in `vk_present.cpp`, frame throttling, drag out, and the in-process compositor test as a gate that needs no installed compositor. Both depend on their own branch's `native_internal.hpp`; mb's also on mb 3.5. |
| Tray icon | **Live** (§2): `Extras/tray` + `dbus.cpp`, StatusNotifierItem + dbusmenu + AppMenu global menu, Win32, Cocoa (parked) | mb 3.8: `fuse/app/shell.hpp`, SNI + dbusmenu, Win32, toast notifications; blocked on mb 3.5 | **Keep this branch's** (already live and gated). From mb take what it adds: Win32 toast notifications and the `fuse_app_tray_host` test host; do not build a second D-Bus client (`dbus_internal.hpp` vs `extras/dbus.hpp`). |
| Notifications | **Live**: `desktop::notify` through `notify-send` / PowerShell / osascript helper processes (on main only the POSIX-desktop helpers can run) | mb 3.8: native D-Bus `org.freedesktop.Notifications` and Win32 toasts | mb's native path is the better end state (no helper process, action callbacks); implement it on `extras/dbus.hpp` and keep `notifyCommand` as the fallback. |
| Accessibility adapters | 3.6: `a11y::PlatformAdapter` in `fuse_ui_a11y` (UIA / NSAccessibility / AT-SPI through AccessKit), `A11yState`; no real-client gate | mb 3.9: `fuse_app_a11y` `WindowAccessibility` + AccessKit Windows / Unix adapters, `Bridge::setScale`, wake handler, gates with a real AT-SPI client and a real UIA client; needed by the editor shell | **Port mb's** adapter and gates. Take this branch's `A11yState` / auto-complete additions and its macOS adapter selection as extensions of it. |
| GPU backdrop blur | 3.10: separate `ui.backdrop` compute kernel (+ HDR variant), SIMD CPU reference | mb 3.4: blur inside the `ui.draw_list` kernel, parity gate against the CPU rasteriser on lavapipe, validation and budget gates | **Port mb's first** (it comes with bit-exact parity, validation-clean and budget gates and was run on real hardware per its notes). Compare this branch's separable-kernel cost in `bench_render.cpp` afterwards. Neither was run for either merge. |
| Damage / incremental GPU present | 3.10: damage-box repaint, persistent buffers, incremental pack | mb 3.4: damage-driven incremental present, gate `fuse_ui_vk_damage` | **mb's** (gated); this branch's incremental pack is complementary (CPU-side packing) and can follow. |
| GPU vector paths / gradients | 3.10: native kernel ops | none | This branch's, after 3.9. |
| OS tool windows for floating docks | 3.5: X11 / Win32 / Wayland, inside the `native.cpp` delta | mb 3.5: `NativeOptions::osWindows`, `NativePlatform::toolWindow*`, `UiContext::osWindows / windowSurfaces`; the editor shell calls this API | **mb's API** (the editor shell needs exactly it; mb §5). Use this branch's code as a second reference for the Win32 and X11 calls. |
| Android backend | 3.4: Java `FuseActivity` with `InputConnection` and TalkBack provider | mb 3.10: JNI IME on NativeActivity, async Vulkan, APK packer, emulator gates | Keep **main's** backend. IME: this branch's `InputConnection` is the standard Android route and also gives TalkBack; mb's APK packer and emulator gates are the better tooling. Port both onto main's backend. |
| i18n | 3.16: `extras/i18n` (`.po`) | live: `fuse_ui_l10n_lib`; mb 3.16 | Main's two libraries; nothing to port unless the `.po` reader is wanted. |
| SVG | 3.13: `Extras/svg` document renderer | mb 3.3: nanosvg icon cooker (build-time) | Different jobs. Extend main's `SvgImage` with this branch's features; mb's cooker stays a build tool. |
| Clipboard formats | `os::clipboardData` through helpers (live, POSIX desktops) | mb 3.5: `fuse/app/clipboard.hpp` native images / MIME / primary selection | **mb's** (native); the helper path is a fallback. |
| Hot reload, Designer, `test_app.cpp`, `test_bench_10k.cpp`, `json.hpp` | deltas on the same files | deltas on the same files (mb's `json`, `test_app`, `test_bench_10k` are live) | Port this branch's on top of what is live. |

## 7. Not verified by the merge

- Windows: `fuse_ui_os` / `fuse_ui_extras` are off by default there. MinGW-w64 `-fsyntax-only` passed for every
  source; no MSVC / clang-cl compile, no link, no run (`fuse_ui_tray_win32`, WebView2, `file_dialog_win32.cpp`).
- Apple: nothing of the branch is built on Apple platforms; no `.mm` file was compiled. Main's Apple files are
  untouched.
- Android, FreeBSD: cross-built (libraries and test executables link); no gate was run on either.
- `fuse_ui_extras_webview` passed without a web engine (it skips inside the test). `desktop::notify / openUrl / print`
  were checked as argv only. Tray: against a private `dbus-daemon`, not a desktop shell.
- Vulkan: no loader on the merge machine; nothing GPU-side of the branch is built or run.
- FORGE backend (`-DFUSE_UI_OS_BACKEND=forge`): not configured (no checkout).
- The live charts and 3D graphs passed the branch's own gates through `path_compat.hpp`; those gates check geometry,
  hit testing, counts and rendered output with tolerances, not images against the branch's renderer. Stroke joins and anti-aliasing
  differ from the branch's rasteriser, and nobody compared images side by side.
- `os_std.cpp` is new code with one gate. `runProcess` forks from a possibly multi-threaded process and execs
  immediately; the polling `FileWatcher` compares modification time and size, so two writes of equal size within the
  timestamp granularity are missed.
- `scripts/gui/*` and `docs/gui/ci/fuse-gui.yml` were not run against main.
