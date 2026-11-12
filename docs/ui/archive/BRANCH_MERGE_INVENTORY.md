# FUSE branch inventory and merge plan

Read-only analysis, 2026-11-12. `origin/main` = `fda0cb0b0`; old fork point = `5dcee19e` ("plan: UI4 backdrop blur
status", 2026-10-30). git 2.43. Nothing in the worktree, index or refs was changed.

## 0. Headline findings

1. **The local clone is shallow** (`.git/shallow` = `d7ac91d31`, main shows 88 commits). "No common history" for
   `docs/fuse-ui-plan` and `wip/fuse-ci-backup-20261031` is an artefact of that. GitHub's compare API says:
   - `docs/fuse-ui-plan` vs `main`: **ahead 0, behind 247** -> it is already a strict ancestor of main. Nothing to merge.
   - `wip/fuse-ci-backup-20261031` vs `main`: **ahead 3, behind 222**, merge base `1792f270e`.
   - `wip/fuse-misc-backup-20261031`: genuinely no common ancestor (1 orphan commit).
   Before any real merge of `wip/fuse-ci-backup`, run `git fetch --unshallow origin` (or use
   `--allow-unrelated-histories` with `-s ours`, which gives the right parents anyway). Merges of the
   `5dcee19e`-based branches work in the shallow clone as is.
2. **There are three parallel, independent developments of `Source/FUSE/UI` from `5dcee19e`**, with zero
   patch-equivalent commits between them (`git cherry`: all `+`):
   - `main`: 38 commits (Nov 6-12), UI code 29.6k -> 64.3k lines.
   - `gui-framework`: 127 commits (Oct 30-31), UI code -> 84.8k lines. UI only, plus a FORGE submodule.
   - `claude/magical-bassi-815825`: 334 commits (273 non-merge, Nov 4-5), UI code -> 73.7k lines, plus about 60k lines
     outside the UI (Renderer, Remaster, EditorShell, Tools, Project, assets).
3. Everything else is redundant with one of those: `wip/fuse-backup` is an ancestor of `gui-framework`;
   `gui-features` and `gui-render` are patch-identical subsets of `gui-framework`; the six `/workspace/FUSE-*`
   snapshots are states along `gui-framework`'s history (only `charts` carries 83 uncommitted lines);
   `wip/fuse-lower` is a 29-file skeleton of a different checkout; `wip/fuse-ci-backup`'s 3 commits are a superseded
   early UI core.
4. No secrets were found (pattern scan of added lines and of the loose JSON/MD files), no file over 1 MB is added by
   any branch, and there are no build outputs or editor junk.

## 1. Relationships

### 1.1 Ancestry (`git rev-list --count A..B`, all from merge base `5dcee19e` unless stated)

| Branch | Tip | Commits not in main | Relation |
|---|---|---|---|
| claude/magical-bassi-815825 | 7178393b4 | 334 (273 non-merge, 61 merges) | independent line; shares nothing with gui-* beyond 5dcee19e |
| gui-framework | 363ecf87d | 127 | = wip/fuse-backup + 2 commits |
| wip/fuse-backup-20261031 | b3a50c90b | 125 | **ancestor of gui-framework** (gf..backup = 0, backup..gf = 2) |
| gui-features | 737e82137 | 20 | shares first 18 commits with gf (up to 4d8b87acb), +2 own |
| gui-render | 0c32be51c | 20 | shares the same 18, +2 own |
| 7 x wip/fuse-*-backup snapshots | - | 1 each | single commit on 5dcee19e |
| docs/fuse-ui-plan | 02ae97bda | 0 (server) | **already an ancestor of main** |
| wip/fuse-ci-backup-20261031 | 3486ce2d8 | 3 (server) | docs/fuse-ui-plan + 28 commits; 25 of those are in main |
| wip/fuse-misc-backup-20261031 | e4bb99886 | 1 | orphan root commit |

Also present and fully merged (0 commits not in main): `fuse/local-u6-u7`, `fuse/local-finish-u6-u7`,
`ccr-111d9b83-2iujo8`.

### 1.2 Patch equivalence

- `git cherry origin/gui-framework origin/gui-features` and `... origin/gui-render`: all four unique commits are `-`
  (already in gf). `git range-diff` shows `=` (identical patches):
  - gui-features 2a9e0129b = gf 93193fbc1 (fuse_ui_extras: UndoStack, gestures, i18n, desktop, PDF)
  - gui-features 737e82137 = gf c79b70dca (Qt widget parity)
  - gui-render 7facb6aa2 = gf a313f0c59 (Wayland Vulkan present)
  - gui-render 0c32be51c = gf 0bbeff2d1 (Android Vulkan present)
- The only files gui-features / gui-render have that gf lacks are `Script/.../lua_ui.hpp`, `Script/src/lua_ui.cpp`,
  `tests/test_lua.cpp` (base files that gf later deleted; main still has them).
- `git cherry origin/main <branch>`: gf 127 `+`, magical-bassi 273 `+`, 0 `-`. gf vs magical-bassi: 0 equivalent.

### 1.3 Snapshot branches: closest commit on gui-framework

Closest commit found by minimising `git diff --numstat` over all 127 gf commits (paths `Source/FUSE docs scripts cmake
.github Tools Tests`). Every snapshot also deletes 4 `Source/FUSE/Relight/package/dist/*` files and about 80 vendor
files (`vendor/sdl/build-scripts` 35, `vendor/bullet/build3` 24, `vendor/pcre/lib` 8, ...): copy artefacts, about
27-28k deleted lines each.

| Snapshot | Closest gf commit | Residual beyond it | Unique content |
|---|---|---|---|
| wip/fuse-script | #125 b3a50c90b (= wip/fuse-backup tip) | none (only the deletions) | none |
| wip/fuse-scripts | #76 565090da6 | none | none |
| wip/fuse-gui | #48 ee9318434 | 3 files; blobs equal gf 1bce01281, `path.cpp` 2 lines all in gf tip | none |
| wip/fuse-gpu | #120 6895c9470 | 15 files; 13 blobs equal gf 2eb2892ff, `ui.hpp`/`ui.cpp` 23 lines all in gf tip | none (work-in-progress of 2eb2892ff) |
| wip/fuse-features | #121 f5635e067 | 6 files; in-progress draft of 363ecf87d. 48 of 328 added lines in `ui.cpp` differ in form from the committed version | none worth keeping (earlier draft of committed z-index / flex-wrap code) |
| wip/fuse-charts | #122 4ad23f7aa | **`Extras/include/fuse/ui/extras/charts.hpp` +83 lines, 73 not in gf tip** | declarations only, no implementation |

The charts delta declares: `GeoRegion`, `GeoProjection`, `AnnotationKind` / `Annotation`, `Chart::annotations`,
`Chart::crosshair`, `forceLayout(Series&)`, `VectorText`, `drawListToSvg`, `renderSvg`, `renderPdf`, `exportSvg`,
`exportPdf`, `linkCharts` / `unlinkChart` / `chartCrosshair`, `chartAppend`. No `.cpp` changed, so these are
unimplemented API sketches.

### 1.4 Unrelated-history branches

- Top-level tree hashes of `docs/fuse-ui-plan` and `wip/fuse-ci-backup` match main's for 16 of 24 entries
  (`.clang-tidy`, `.cursor`, `Content`, `Samples`, `Templates`, `scripts`, `CMakeLists.txt`, ...). They are the same
  project's earlier history, not a pre-rewrite copy.
- `wip/fuse-ci-backup` 3 unique commits (`8d6597675`, `1ed8ec5fd`, `3486ce2d8`; 29 files, +3166/-8):
  - Editor QSettings geometry + dock-layout persistence: main has it (`main_window.cpp`: `kSettingsGeometry`,
    `kSettingsLayout`, `saveSettingsLayout`, `restoreSettingsLayout`).
  - "FUSE UI core (UI2 slice)": 13 files that exist nowhere in main (`UI/Core/include/fuse/ui/{context,cpu_raster,
    font5x7,geometry,test_driver}.hpp`, `UI/Core/src/{context,cpu_raster,font5x7,style_sheet,test_driver,widgets}.cpp`,
    `UI/samples/hello_app.cpp`, `UI/tests/test_ui_core.cpp`). Main took the competing implementation committed ten
    minutes earlier (`a44ec545c` "fuse_ui_core headless retained core ..."). Superseded.
  - Doc edits to `docs/unification/*`, `docs/ui/README.md`, `FUSE_UI_PLAN.md`: superseded by main's versions.
- `wip/fuse-misc`: 62 files, 8.45 MiB raw (3.95 MiB packed). 48 PNG screenshots (4.2 MiB; 26 under `fuse-ui-shots/`),
  one 12 KB `.ogv`, 10 `.md` under `fuse-docs-backup/`, 3 JSON dumps of PR lists (`fuse_scan.json` 1.49 MB,
  `fuse_open_prs.json` 1.40 MB, `fuse-open-prs.json` 0.86 MB; arrays of PR number / head ref / title / check status,
  one with an `author` field). The MD files: `docs/sources/P2..P7.md` are byte-identical to main; `FUSE_MASTER_PLAN.md`,
  `_TOC.md` and `P1.md` are the 2026-09-14 originals already in main's history (blob `0caa7cee1` found at `2a158eefd`).
  Nothing here exists in main's tree, and nothing is needed by it.
- `wip/fuse-lower`: 29 files. 17 empty-directory `.gitkeep`s (`assets/ audio/ compat/ core/ ecs/ ...`), a 337-byte
  `LICENSE`, small `README.md` / `.gitignore` / `.gitattributes`, the same 2026-09-14 master plan and P1..P7, and
  **`docs/plans/MASTER_CONDENSED.md` (583 lines, 30 KB), which exists nowhere in main** (not found in history either;
  the history search may have timed out, so treat as unique).

## 2. Per-branch contribution relative to current main

### 2.1 gui-framework (and its ancestor wip/fuse-backup-20261031)

Diff vs base: 250 files, +58,606 / -1,713. 184 files are new relative to main.

| Area | Files | + lines | Notes |
|---|---|---|---|
| UI/Extras (new module) | 53 | 13,667 | charts, datavis3d, richtext, svg, pdf, media, tray, webview, dbus, undo, gesture, i18n, declarative, widgets_extra |
| UI/Core | 18 | 10,769 | CSS engine, filters, masks, colour space, mesh3d, SIMD blur |
| UI/Widgets | 11 | 6,956 | completer, line regex, item selection, in-app DnD |
| UI/App | 24 | 4,798 | wayland, cocoa, uikit, android (+ Java activity), win32_dnd |
| UI/Render | 33 | 4,071 (11 `.spv`) | HDR, mesh, backdrop, encoders |
| UI/Script | 5 | 1,242 / -327 | adds ForgeUi, **deletes lua_ui** |
| UI/Os (new), UI/Public (new) | 6 | 1,094 | OS seam, `<fuse/gui.hpp>` |
| UI/tests | 60 | 13,662 | 1 binary (`media_clip.avi`, 64 KB) |
| docs/gui, scripts/gui, cmake | 18 | 1,018 | PLAN, TESTING, staged CI yml, per-platform scripts, FuseForge.cmake + patch |
| .gitmodules, vendor/forge | 2 | - | submodule gitlink `944b053d` -> `github.com/odin-loki/FORGE.git` |

Commit themes: FORGE OS seam (G1/G2); platform backends (Wayland, Cocoa, UIKit, Android, Win32 OLE); HiDPI; Vulkan
present per WSI; Extras (undo, gestures, i18n, PDF, tray, web view, rich text, media); Qt widget parity (line edit,
combo, spin, date/time, calendar, button box, tabs, docks, headers, item-view DnD); CSS engine (transitions,
keyframes, selectors, transforms 2D/3D, filters, masks, at-rules, positioning); GPU renderer (paths, gradients, clips,
filters, HDR, incremental pack); charts and 3D dataviz; FORGE scripting; test scripts and staged CI.

**(a) Already in main in equivalent or different form**

| gf feature | main counterpart |
|---|---|
| group box, status bar, message box, wizard, tool bar | `Widgets/src/widgets_tier_b3.cpp`, `widgets.hpp` (`groupBox`, `statusBar`, `messageBox`, `wizard`, `toolBar`, plus `mainWindow`, `popover`, `toast`, `headerView`) |
| stacked widget | `stack` / `stackAddPage` / `stackPage` |
| Android / Cocoa / UIKit backends | `App/src/native_android.cpp`, `native_apple.mm`, `entry_android.cpp`, `entry_ios.mm` |
| Android / Metal Vulkan surface kinds in `vk_present.cpp` | main's `vk_present.cpp` (CAMetalLayer, Android) |
| HiDPI (`setScale`, `setPixelScale`, WM_DPICHANGED) | render scale commit a925efbd1, `native_services.inl` |
| FreeBSD build, `fromChars` fallback | `.github/workflows/fuse-ui-freebsd.yml`, `Text/include/fuse/text/from_chars.hpp` |
| style / `.fuseui` hot reload | `Doc/include/fuse/ui/doc/hot_reload.hpp` (add/add conflict) |
| in-app drag and drop, gestures (tap, pan, pinch) | `Core/src/interaction.cpp`, `ui.hpp` (`DropAction`, `GestureConfig`), `widgets/interaction.hpp` |
| i18n (.po, plurals, RTL direction) | `Text/include/fuse/text/i18n.hpp`, RTL in core |
| vector paths, multi-stop gradients, SVG subset | `Core/{path,painter,svg}.hpp`, `draw_vector.cpp` (CPU only) |
| CSS transitions, `linear-gradient`, flex-wrap, grid templates | `Core/src/style.cpp` |
| tabbed / floating docks, dock drop zones | commits 1d72473de, d73a3472a |
| selection modes, proxy models, header view | `widgets/modelview.hpp` |
| native file dialogs, clipboard, cursors, `openUrl`, single instance, settings | `App/include/fuse/app/services.hpp`, `native_services.inl`, `app.hpp` |
| a11y grid / tree roles | commit 941737b9c |
| `<fuse/gui.hpp>` public target | `cmake/fuse_ui_install.cmake` + FuseUI CMake package |

**(b) Genuinely missing from main (must be ported)**

Platform / app:
- Native **Wayland backend** (xdg-shell, wl_shm, seat, data-device DnD in/out, text-input-v3 IME, xdg_popup, key
  repeat, fractional-scale + viewporter, cursor-shape, Vulkan `VK_KHR_wayland_surface`) and its in-process compositor
  test.
- **OS drag and drop**: X11 XDND v5 target and source, multi-format; Win32 OLE `IDropTarget` / `DoDragDrop` with drag
  image helpers; Cocoa drop-in; `AppWindow::onExternalDrop`. KNOWN_GAPS lists this as missing.
- **OS tool windows for floating docks / toolbars** (X11 / Win32 / Wayland).
- Android **Java `FuseActivity`**: IME via `InputConnection`, TalkBack via `AccessibilityNodeProvider`; Gradle-free APK
  script and emulator smoke script.
- iOS `UITextInput` marked text; Cocoa `CVDisplayLink` pacing; frame pacing API (`Presenter::vsynced`, max frame rate,
  Wayland frame throttle); live window retitle.
- a11y `PlatformAdapter` (UIA / NSAccessibility / AT-SPI through AccessKit adapters), `A11yState`, auto-complete state.
- `fuse::ui::os` seam (`Os/include/fuse/ui/os.hpp`): file watcher, process runner, shared buffers, audio output,
  library loading, rich MIME clipboard, HDR display query. FORGE-backed.

Extras module (all absent in main):
- `UndoStack` / `UndoGroup` / `UndoCommand`.
- `PdfDocument`, print preview, printing and printer listing, desktop notifications.
- `TrayIcon`, `GlobalMenuBar` (StatusNotifierItem + dbusmenu, Shell_NotifyIcon, NSStatusItem), minimal D-Bus client.
- `WebView` (WebKitGTK, WebView2, WKWebView).
- `RichDocument` / `RichTextEditor` (char and block formats, lists, HTML + Markdown import/export, justify, oblique).
- `SystemVideo` decode (GStreamer, Media Foundation, AVFoundation), audio output, audio level meter widget.
- Declarative `Property` bindings, `Effect`, `StateMachine`.
- SVG documents / `svgWidget` with `<text>`, `clipPath`, `mask`, patterns, focal radial gradients, filters.
- **Charts library** (`fuse::gui::charts`): line/area/bar/scatter, pie/donut, candlestick, box, waterfall, contour,
  radar, treemap (nested), sankey, gauge, funnel; axes, legends, tooltips, zoom/pan, model mapping, LTTB / min-max
  decimation, animated transitions, colour maps, image export. Main has only `ChartKind { Line, Bar, Area, Scatter,
  Flame }`.
- **3D data visualisation** (surface / scatter / bars, volume slices, BSP ordering, picking, GPU mesh path
  `ui_vk_mesh`, `mesh3d.hpp`).
- Widgets: `toolBox`, `inputDialog`, `dialogButtonBox` (platform button orders, default / reject buttons),
  `lcdNumber`, `keySequenceEdit`, `mdiArea`, `graphicsView` / `GraphicsScene`, `virtualKeyboard`, wizard validators.

Core widgets:
- Line edit: echo modes, max length, placeholder, validators (int / double / regex), input mask, fixup,
  `editingFinished`; `QCompleter`-style completer.
- Editable combo box (insert policies, item data / icons, model-backed).
- Spin box options (prefix / suffix, wrapping, special value text, adaptive step, selection, auto-repeat).
- `dateEdit` / time edit with format sections and calendar popup; calendar options (first weekday, ISO weeks, range).
- Tab bar: closable, movable, disabled, scroll buttons, position, elide, icons, tooltips, add / remove page.
- Item views: tree view DnD and scrolling, rubber-band selection, cross-view drops, multi-row drags, auto-scroll /
  auto-expand, header section move / hide / resize modes / menu, list icon mode, splitter sizes / collapsible,
  toolbar overflow, movable / floating toolbars.

Style engine:
- `@keyframes` + CSS animations, Web-Animations-style API, scroll / view timelines, animation ranges.
- Selectors: attribute, `:nth-*`, sibling combinators, `:not/:is/:where/:has`, `:empty`; invalidation sets.
- 2D affine transforms (rotate / skew / matrix, `transform-origin`) and 3D transforms (perspective, preserve-3d,
  backface, BSP splitting) with transformed hit testing.
- CSS filters and `backdrop-filter` functions, `drop-shadow`, SVG `<filter>` programs.
- `clip-path` shapes / `path()` / `url()`, full `mask-*`, `background-clip` (incl. `text`), multi-stop
  `linear-gradient` angles.
- `@media`, `@supports`, `@container`, `@layer`, `@scope`, nesting, `style()` queries, container / viewport units.
- `position` + insets, `z-index` stacking contexts, `align-content`, gaps, `aspect-ratio`, grid masonry / subgrid.

Rendering:
- GPU vector paths, gradients, clip paths, layered clips / masks, filter layers, GPU text runs (main re-bakes paths on
  the CPU per pack).
- GPU backdrop blur kernel, damage-box partial repaint, persistent / incremental GPU buffers.
- Colour management and HDR: `color_space.hpp`, HDR10 / scRGB swapchains and encoders, RGBA16F targets, float CPU
  raster (`HdrImage`).
- GLSL -> SPIR-V build step with precompiled fallback; SIMD blur / span fills; render benchmarks.

Tooling: `scripts/gui/*` per-platform test scripts, FreeBSD cross + qemu VM runner, `docs/gui/PLAN.md`,
`docs/gui/TESTING.md`, staged CI `docs/gui/ci/fuse-gui.yml`; Designer product pass (clipboard, shortcuts, validation,
preview, device frames).

**(c) Conflicting designs**

- **Same names, same namespace, different signatures** in different files (git will not flag these; they fail at
  compile or link time): `groupBox`, `groupBoxBody`, `statusBar`, `statusBarPermanent`, `statusMessage`,
  `messageBox`, `MessageIcon`, `wizard`, `wizardPage` (gf `extras/widgets_extra.hpp` vs main `widgets.hpp`, both
  `fuse::ui::widgets`); `toolBar`, `toolBarSeparator`, `dockFloat`, `dockRaise`, `dockTabify`, `setTableDragDrop`,
  `SelectionMode`, `DropAction`, `GestureConfig`, `Path`, `Gradient*`, `Easing`, `fromChars`, `openUrl`, `keyName`,
  `MenuItem`.
- **Scripting**: gf deletes the Lua binding and replaces it with FORGE script. Main keeps and extends Lua (builds for
  iOS). Do not take the deletion.
- **OS layer**: gf routes file I/O, settings, clipboard, dialogs and SIMD through the FORGE submodule
  (`FUSE_UI_OS_BACKEND=forge`, default; configure fails without the submodule). Main has its own
  `services.hpp` / `native_services.inl`.
- **Backends**: gf has per-platform classes (`android.cpp`, `cocoa.mm`, `uikit.mm`, `wayland.cpp`); main has
  `native_android.cpp`, `native_apple.mm`, `native_common.cpp`. Two HiDPI APIs, two hot-reload classes, two path /
  gradient types in `draw_list.hpp`, two `GpuDrawData` layouts.
- gf's Cocoa, UIKit, Metal and AVFoundation code is marked UNVERIFIED / never compiled in its own commit messages.

### 2.2 claude/magical-bassi-815825

Diff vs base: 978 files, +128,609 / -3,861. 507 files are new relative to main. Largest areas:

| Area | Files | + lines |
|---|---|---|
| Source/FUSE/UI (all) | ~264 | ~57,000 (tests 27,833 incl. 48 binaries; App 11,856; Text 7,828; Core 2,661; Render 1,611; Widgets 1,566; L10n 1,223; Icons 490) |
| Tools/FUSE | 116 | 21,911 |
| Source/FUSE/Renderer | 269 | 14,142 / -1,391 |
| Source/FUSE/Remaster (new) | 60 | 13,220 |
| Source/FUSE/EditorShell | 22 | 7,113 |
| Source/FUSE/Project | 21 | 2,520 |
| Animation, Apps, Scene, Hybrid, World3D, Relight, Core | ~86 | ~2,800 |
| vendor (nanosvg, Lucide icons, Noto fonts) | 54 | 5,560 |
| Content (schemas, recipes), Tests (fixtures, goldens) | 51 | ~1,740 |
| docs (ui, editor, remaster, unification, plans) | 16 | ~2,200 |
| cmake, scripts, Templates, root files | ~12 | ~400 |

Commit themes: native Windows / NVIDIA bring-up (correctly rounded shader maths, gate fixes across the renderer);
UI Vulkan (GPU backdrop blur, damage present, SDF quads, custom effects, pipeline cache, budget / golden / validation
gates); text (COOKED fonts, MSDF, COLRv1, CBDT / sbix, variable fonts, OpenType features, conformance data); icons
(nanosvg cooker, `.fuseicons`, Lucide subset); app platform (window chrome, tray, notifications, clipboard, IME,
X11 parity, Wayland, Android, saved state); a11y (UIA, AT-SPI end to end); RTL, keyboard-only, high contrast; l10n
library + string extraction; `project.json` schema 2 / ProjectHost; FUSE-UI editor (docking, panels, Qt parity
gates); renderer (GPU skinning, Hi-Z SSR, path tracer stage, HW bench); assets (DTS / DSQ readers, `.fuseanim`);
Remaster waves 0-5 (`fuse_poco`, store / db, `fuse_ai`, TorqueScript parser, DIF reader, Remix ingest / export).

**(a) Already in main**

| mb feature | main counterpart |
|---|---|
| device-pixel scale (`UiContext::setScale`) | render scale commit a925efbd1 |
| touch pan, kinetic scrolling | 6b3e2384f, `platform_feel.hpp` |
| RTL layout and widget mirroring | 37c62f78d (mb's goes further: Tier B2/C/D, bidi caret) |
| Android NativeActivity backend | `native_android.cpp` (add/add conflict; mb's is 1,258 lines vs 739 and has JNI IME, async Vulkan, APK packer) |
| `game_menu` sample | `App/samples/game_menu.cpp` (add/add conflict) |
| built-in icons (`icons.hpp`, `icons.cpp`) | main's vector icons (add/add conflict, different design) |
| cursors, system theme, displays, dialogs, clipboard text | `services.hpp` |
| tabbed / floating docks | main's dock commits |
| system font families, synthetic bold / italic | `font_family.hpp` |
| number formatting, plurals | `Text/i18n.hpp` |
| flame chart, minimap | main has them; mb does not |

**(b) Genuinely missing from main (must be ported)**

UI:
- **Wayland backend** (`native_wayland.cpp`: xdg-shell, popups, child tool windows, fractional scale, clipboard /
  primary selection / drops, text input) and X11 parity (RandR, XSETTINGS, themed cursor sizes, real XIM pre-edit).
- Custom **window chrome** (`chrome.hpp`, `WindowFrame`, `WM_NCHITTEST`), **tray icon + notifications**
  (`shell.hpp`, SNI + dbusmenu, Win32).
- Clipboard images / MIME / lazy formats / primary selection (`clipboard.hpp`); platform dialogs API
  (`platform.hpp`: open / save / folder / colour dialogs, `revealInFileManager`).
- `WindowAccessibility` + AccessKit **UIA adapter** and **AT-SPI** end to end, with real-client gates.
- Saved state across process death; async Vulkan presenter init; software presenter dirty-rect copy; MAILBOX pacing.
- Android: IME composition through JNI, async Vulkan, APK packer (`fuse_apk.py`), emulator gate script.
- **GPU**: backdrop blur, damage-driven incremental present, SDF-quad raster backend, custom shader effects
  (`effects.hpp`, `EffectRegistry`, `effect:` style), persistent pipeline cache. All listed as missing in KNOWN_GAPS.
- **Text**: COOKED profile (`.fusefont`, `fuse_text_cook`), MSDF glyphs, exact COLRv1, CBDT / sbix, variable fonts
  (fvar / avar / gvar / HVAR / MVAR), OpenType features, `font-weight` / `font-stretch` / `font-variation-settings` /
  `font-feature-settings`, per-node fonts, PNG codec, layout caches, Unicode conformance gates. All listed as missing
  in KNOWN_GAPS.
- **Icons**: SVG icon cooker (`Icons/`, nanosvg), `IconSet`, `.fuseicons` v2 multi-colour, `icon` / `icon-size` style.
- **L10n library** (`L10n/`): CLDR plural rules, ICU-style messages, date / time / currency formatting, string tables,
  `fuse_ui_extract_strings` CLI, `.fuseui` load-time translation.
- High-contrast theme, OS text scale, `zoom` property, light theme / `ThemeVariant`, pinch / Ctrl+wheel zoom.
- Widgets: `dockSplit` nested splitters (layout format 2), `dockOpen` / `dockRemove`, timeline clip drag,
  `treeLineAt` / `treeLineRect`, keyboard-only operation fixes and focus rings, per-class widget gates.
- CPU rasteriser runs / banded threads / SSE2 + NEON edges; paint-time text cache; hover fix.
- **ProjectHost** (`project_host.hpp`, `fuse_app_host`, hybrid game side), `project.json` **schema 2** /
  `AppPresent`, `demo_hybrid_hud` on a UI context. Listed as untouched plan items in KNOWN_GAPS.
- Designer: icon picker, effect picker, registry-driven property grid.
- Docs: `docs/ui/BASELINES.md`, `docs/editor/QT_PARITY.md`, `scripts/ui_baselines.py`, `Templates/FuseUIApp`.

Outside the UI (none of it is in main; main touched none of these files except `Editor/src/editor_host.cpp`):
- Renderer: correctly rounded `exact_math` shader library and NVIDIA bit-parity fixes across about 269 files, GPU
  skinning + skinned motion vectors, Hi-Z SSR, AO denoise, path tracer FrameComposer stage, Android surface, present
  semaphore fix, render-graph barrier fixes, HW bench harness, `RENDERER-HW-RUN.md`.
- EditorShell: FUSE-UI editor (per-panel docking, OS tool windows, Lighting / Project Settings / Build dialogs,
  profiler flame graph, timeline retime, hierarchy DnD) with `fuse_editor_ui_*` gates.
- Remaster: `Source/FUSE/Remaster` + `Tools/FUSE` (POCO schemas, blob store, SQLite db, `fuse_ai` providers, Torque
  DTS / DSQ / DIF / TorqueScript readers, Remix ingest / export, parity harness).
- Assets / Animation: `.fuseanim` cook + loader, DTS reader, asset-check gates, licence lint policy.
- Build: MSVC / MinGW / Clang fixes, Vulkan test-env helper, `.gitattributes` LF rules, Relight native Windows build.

**(c) Conflicting designs**

- Three-way collisions with main and gf on: Android backend, render scale, touch, RTL, icons, cursor shape
  (`CursorShape` in mb `style.hpp`, gf `ui.hpp`, main `services.hpp`), dialogs / `messageBox` / `MessageIcon`,
  `openUrl`, `DisplayInfo`, `SystemTheme`, `formatNumber`, `systemFontFamilies`, `setIcon`.
- The textual merge leaves duplicate `UiContext` members outside conflict markers: `cursor`, `flip`, `markAllDirty`,
  `nodeFont`, `paintIcon`, `reducedMotion`, `setReducedMotion`, `touchCancel`, `touchPanning` (main + mb);
  `animate`, `animating`, `animSetKeyframes`, `defineClipPath`, `invalidate`, `scriptAnim`, `timelineProgress`
  (main + gf).
- mb vs gf overlap on Wayland (two backends), tray (App/shell vs Extras/tray), GPU backdrop blur, damage present,
  a11y adapters, i18n (three libraries counting main's), SVG (nanosvg cooker vs Extras/svg vs main `Core/svg`).
- mb's EditorShell and Apps code uses mb-only UI API: `DockEdge` / `dockSplit` / `dockSplitOf` / `dockRemove` /
  `dockOpen` / `dockSideOf`, `treeLineAt` / `treeLineRect`, `timelineClipDrag`, `WindowAccessibility`,
  `fuse/app/platform.hpp` dialogs, `IconSet`. These must exist in the merged UI or the editor will not compile.

### 2.3 gui-features, gui-render

Fully contained in gui-framework (section 1.2). No unique content. Their unique-commit diffs: gui-features 20 files
+3,101; gui-render 10 files +296 / -43.

### 2.4 Snapshots, wip/fuse-lower, wip/fuse-misc, wip/fuse-ci-backup, docs/fuse-ui-plan

See 1.3 and 1.4. Unique content worth a decision: `charts.hpp` +83 lines (wip/fuse-charts),
`docs/plans/MASTER_CONDENSED.md` (wip/fuse-lower), screenshots and PR-scan JSON (wip/fuse-misc).

## 3. Mergeability (`git merge-tree --write-tree --name-only`, no worktree touched)

| Merge | Conflicted files | Conflict hunks / lines (approx.) |
|---|---|---|
| main <- gui-framework | 39 | ~165 hunks, ~9,000 lines |
| main <- wip/fuse-backup | 39 | same set |
| main <- gui-features | 16 | - |
| main <- gui-render | 18 | - |
| main <- magical-bassi | 44 | ~210 hunks, ~9,700 lines |
| gui-framework <- magical-bassi | 44 | ~215 hunks, ~13,800 lines |
| gui-framework <- gui-features / gui-render | 13 / 5 | irrelevant: patch-identical content already in gf |
| gui-features <- gui-render | 0 | clean |
| main <- each snapshot | 27-39 | same as gf at that stage, plus the vendor deletions |

All conflicts are inside `Source/FUSE/UI`, except for magical-bassi: `docs/ui/README.md` (7 hunks),
`docs/plans/FUSE_UI_PLAN.md` (1), `Source/FUSE/Editor/src/editor_host.cpp` (1).

Worst files, main <- gui-framework (hunks / conflict lines): `App/src/native.cpp` 21 / 438;
`Widgets/src/widgets_tier_b2.cpp` 16 / 1,380; `Core/src/ui.cpp` 15 / 382; `Core/src/style.cpp` 13 / 583;
`Core/src/draw_list.cpp` 10 / 567; `Widgets/src/widgets_tier_cd.cpp` 9 / 243; `Core/include/fuse/ui/ui.hpp` 8 / 282;
`App/src/vk_present.cpp` 7 / 112; `Core/src/path.cpp` 3 / 1,323 (add/add); `Render/src/gpu_draw_data.cpp` 2 / 1,117;
`Core/include/fuse/ui/draw_list.hpp` 4 / 398. Modify/delete: `Script/src/lua_ui.cpp`.

Worst files, main <- magical-bassi: `App/src/native.cpp` 33 / 848; `App/src/native_android.cpp` 9 / 1,930 (add/add);
`Core/src/ui.cpp` 13 / 541; `Widgets/src/widgets_tier_b2.cpp` 9 / 536; `Text/src/text.cpp` 9 / 382;
`App/src/vk_present.cpp` 9 / 111; `Core/include/fuse/ui/ui.hpp` 8 / 239; `docs/ui/README.md` 7 / 373;
`Core/src/style.cpp` 7 / 258; `tests/test_rtl_layout.cpp` 1 / 895 (add/add); `App/samples/game_menu.cpp` 2 / 701
(add/add); `Core/src/draw_list.cpp` 3 / 353.

Clean parts: gf's 184 new files (all of `Extras/`, `Os/`, `Public/`, new backends, shaders, scripts, docs);
magical-bassi's 507 new files and every non-UI path. "Clean" is textual only: the new files compile against their
own branch's versions of `ui.hpp`, `style.hpp`, `draw_list.hpp`, `widgets.hpp` and `gpu_draw_data.hpp`, which are
exactly the conflicted files.

Files both sides modified: main & gf 50; main & mb 61 (58 in UI); gf & mb 56.

## 4. Risks

- **Never merge as a tree**: `wip/fuse-lower` (23,471 files / 7.2 M lines deleted vs base) and the six snapshots
  (each deletes about 84 files: 4 Relight dist files incl. `licenses/dxvk-remix-MIT.txt`, and vendor SDL / Bullet /
  PCRE files). A real merge of a snapshot would also revert gf-era code to older states.
- **gui-framework deletes Lua scripting** (`lua_ui.hpp`, `lua_ui.cpp`, `test_lua.cpp`); main modified `lua_ui.cpp`.
- **FORGE submodule** (`vendor/forge` -> `https://github.com/odin-loki/FORGE.git` @ `944b053d`): a second repository
  becomes a hard build dependency of the UI (default backend; `FATAL_ERROR` without it), plus a patch applied to a
  build-dir copy (`cmake/forge-patches/0001-script-host-bindings.patch`). I could not read that repository from this
  session, so its licence, visibility and size are unknown. gf adds no THIRD_PARTY.md entry for it.
- **Third-party code added**:
  - magical-bassi: `vendor/nanosvg` (zlib, `LICENSE.txt` present), `vendor/icons/lucide` 43 SVGs (ISC + MIT,
    `LICENSE` present), Noto Sans Thai + CJK SC subset (OFL, `OFL.txt` present, subset recipe included), Unicode test
    data under `UI/tests/data/unicode` (`LICENSE.txt` present). All four are recorded in THIRD_PARTY.md.
  - gui-framework: no vendored sources. It loads system libraries at run time (libwayland, libdbus, WebKitGTK,
    GStreamer, WebView2 loader). `Extras/src/compat/mingw/EventToken.h` is a 6-line own shim;
    `App/src/wl_protocol.hpp` is hand-written protocol tables. The WebView2 SDK header is expected from the SDK, not
    checked in. No licence headers in `Extras/` or `Os/`.
- **Large files**: none over 1 MB in any code branch. Largest: `vendor/fonts/noto/NotoSansCJKsc-Subset.otf` 627 KB,
  `RENDERER-EXECUTION.md` 387 KB (text), `BidiCharacterTest.txt` 340 KB (mb); `ui_backdrop*.comp.spv` about 130 KB
  each (gf). Binary totals: mb 62 files / 1.29 MB, gf 12 files / 0.49 MB. In `wip/fuse-misc`: three JSON files of
  0.86 / 1.40 / 1.49 MB and 48 PNGs.
- **Secrets / personal data**: pattern scan (GitHub / OpenAI / AWS / Google / Slack tokens, private-key headers) over
  all added lines of gf and mb and over the misc JSON / MD files: 0 hits. The misc JSON files are PR-list dumps (one
  includes PR `author` fields: public GitHub data, but not something main needs). `fuse_ai` has cloud providers behind
  an opt-in guard; no keys are checked in.
- **Junk**: none. The `.obj` / `.log` matches in mb are Wavefront and expected-output test fixtures.
- **Unverified code**: gf's Apple code was never compiled; main's own mobile / Apple / Win32 paths were never run
  (KNOWN_GAPS). mb's code was run natively on Windows (RTX 3090) and Linux per its plan notes; I did not re-run
  anything.
- **Hidden semantic conflicts** after a textual merge: duplicate declarations and same-name-different-signature
  functions listed in 2.1(c) and 2.2(c); two or three implementations of the same subsystem linked into one target;
  `tests/CMakeLists.txt` test-name collisions.

## 5. Recommended plan

Constraint: only commits pushed to a branch and merged by PR can make a branch deletable, so every branch must end up
as an ancestor of main through a merge commit.

| # | Branch | Method | Size | Why |
|---|---|---|---|---|
| 0 | docs/fuse-ui-plan | nothing; delete | - | already an ancestor of main on the server (ahead 0) |
| 1 | wip/fuse-ci-backup-20261031 | `git merge -s ours` (after `git fetch --unshallow`, or with `--allow-unrelated-histories`) | S | 3 commits; QSettings layout is in main, UI core slice superseded |
| 2 | claude/magical-bassi-815825 | true `git merge --no-ff` on an integration branch | XL | 44 conflicted files, ~9.7k conflict lines, 3 add/add files, duplicate members; but ~60k lines of non-UI work merge cleanly and are only obtainable this way |
| 3 | gui-framework | true merge, or port by subsystem then `-s ours` | XL | 39 conflicted files, ~9k lines, FORGE dependency, Lua deletion, name collisions with main's widgets; after step 2 the conflict set grows towards the gf <- mb figure (44 files, ~13.8k lines) |
| 4 | wip/fuse-backup-20261031 | nothing | - | ancestor of gf; becomes an ancestor of main with step 3 (only if step 3 is a true merge or `-s ours` of gf) |
| 5 | gui-features, gui-render | `git merge -s ours` each | S | patch-identical to gf commits (range-diff `=`), different SHAs |
| 6 | wip/fuse-{script,scripts,gui,gpu,features}-backup | `git merge -s ours` each | S | no content beyond gf (section 1.3) |
| 7 | wip/fuse-charts-backup | save the 83-line `charts.hpp` delta as a patch or TODO alongside the charts port, then `-s ours` | S | declarations only |
| 8 | wip/fuse-lower-backup | optionally copy `docs/plans/MASTER_CONDENSED.md` into main, then `-s ours` | S | must never be merged as a tree |
| 9 | wip/fuse-misc-backup | `git merge -s ours --allow-unrelated-histories`, or delete without merging if the owner does not want the screenshots | S | orphan; nothing main needs |

Steps 1 and 5-9 can be one octopus-free series of `-s ours` merges in a single PR; none changes main's tree.

**Order of the two big ones: magical-bassi first, then gui-framework.**
- magical-bassi overlaps main in the same subsystems main just rewrote (Android, scale, touch, RTL, services, icons),
  so those design decisions should be made once, before gf's additions land on top.
- Its non-UI work is independent of the UI decisions except for the editor's use of the UI API listed in 2.2(c).
- gf is mostly additive (`Extras/`, `Os/`, CSS engine, GPU features) and is the older line; it is easier to port onto
  a settled core.

**What each integration must port** is the (b) list of its section. Decisions to make first:
- magical-bassi: which Android backend, icon system and i18n library survive (main's or mb's); mb's text stack
  (COOKED / MSDF / variable fonts) and GPU features have no main counterpart and can be taken whole; the editor needs
  `dockSplit` and friends, `platform.hpp`, `accessibility.hpp`, `IconSet`.
- gui-framework: whether FORGE becomes a dependency. If not, `Os/src/os_forge.cpp`, the SIMD lanes and `ForgeUi` need
  a non-FORGE backend (`FUSE_UI_OS_BACKEND=custom` exists as an option but I did not find an implementation). Keep
  Lua. Rename or fold gf's `groupBox` / `statusBar` / `messageBox` / `wizard` / `toolBar` into main's. Choose one
  Wayland backend (gf's or mb's) and one tray implementation.

**If a true merge of gui-framework is judged too costly**, port the (b) items in PRs and finish with
`git merge -s ours origin/gui-framework`; that also makes `wip/fuse-backup` an ancestor. The same fallback works for
magical-bassi's UI part, but its non-UI paths should still come in by a real merge (resolve every UI conflict to
main's side only if the editor code that needs mb's UI API is held back or ported in the same PR).

**Repository size impact**
- `docs/fuse-ui-plan` / `wip/fuse-ci-backup`: their 12.7k commits are already in main's history on the server. The
  ci-backup merge adds 3 commits, 57 objects, 0.04 MiB packed.
- `wip/fuse-misc`: 65 objects, 3.95 MiB packed (8.2 MiB raw), permanent once merged.
- `wip/fuse-lower`: 15 objects, 0.02 MiB. Six snapshots: 713 objects, 1.2 MiB. gui-features + gui-render: 89
  objects, 0.03 MiB.
- gui-framework: 2,991 objects, 1.9 MiB. magical-bassi: 5,685 objects, 3.6 MiB.
- Total about 11 MiB against a repository GitHub reports at about 840 MB. All of these objects are already stored on
  the remote (they are reachable from the branches today); merging only makes them permanent.

## 6. Not verified

- Nothing was built or tested; (a)/(b) judgements come from headers, symbol sets, commit subjects and greps.
- The public-symbol comparison is name-based over `Source/FUSE/UI/*/include`; class members were only spot-checked.
- The FORGE repository could not be inspected.
- A sequential merge (main + magical-bassi, then gui-framework) cannot be trial-run without resolving the first; the
  step-3 estimate is extrapolated from the pairwise gf <- mb result.
- History searches for `MASTER_CONDENSED.md` ran under a timeout; "exists nowhere in main's history" is unconfirmed.
- Local ancestry checks for main are limited by the shallow clone; server-side figures come from the GitHub compare
  API.
