# Port list: `claude/magical-bassi-815825` onto main's FUSE UI

Written 2026-11-12 with the merge commit that brought the branch into main. It is the work list for finishing that
merge: every feature of the branch that is **not** live in the merged build, where its code is, what blocks it and how
to check it once it is ported. Background and the survey of all branches: [`BRANCH_MERGE_INVENTORY.md`](BRANCH_MERGE_INVENTORY.md).

Names used below:

- **base** = `5dcee19e` (common ancestor), **branch** = `7178393b4fea` (tip of `claude/magical-bassi-815825`, now an
  ancestor of main), **main** = the first parent of the merge commit.
- `U/` = `Source/FUSE/UI/`.
- "In the tree" = the file is checked in on main at that path with the branch's content. "Unbuilt" = no CMake target
  lists it.
- "Branch delta" = a change the branch made to a file main also has, which the merge did **not** take. It is not in
  the working tree. Read it with `git diff 5dcee19e 7178393b4fea -- <path>` (what the branch changed) or
  `git show 7178393b4fea:<path>` (the branch's whole file).

## 1. How the merge was resolved

| Area | Rule applied | Result |
|---|---|---|
| Everything outside `U/`, `docs/ui`, `docs/plans/FUSE_UI_PLAN.md` | Branch taken whole (clean merge) | 714 files (322 added, 392 modified): Renderer, `Source/FUSE/Remaster`, `Tools/FUSE`, EditorShell, Project, Animation, Apps, vendor (nanosvg, Lucide, Noto Thai + CJK subset), Content, Tests, cmake, docs. Nothing main had was deleted. |
| `Source/FUSE/Editor/src/editor_host.cpp` | Both sides combined | Both includes kept (`t3d_asset_vfs.hpp` from main, `world_converter.hpp` from the branch). |
| 79 existing files under `U/` the branch changed (41 with conflicts, 17 merged cleanly by git, 21 only the branch changed) | Main's version, except the hunks listed in §2 | A textual merge of these would have produced duplicate `UiContext` members and two implementations of scale, touch, RTL, icons and cursors. See §4 for the per-file list of branch deltas left out. |
| 185 new files under `U/` | All kept | 16 are built (§2); 169 are unbuilt and listed in §3. |
| `docs/ui/README.md`, `FUSE_UI_PLAN.md` | Main's text plus sections for what is live | The branch's README sections for unbuilt features are not in the README; read them with `git show 7178393b4fea:docs/ui/README.md`. |

No golden image changed. `U/tests/test_text_styles.cpp` now expects 10 scanned faces and 8 families instead of 8 and 6,
because the branch vendors `NotoSansThai-Regular.ttf` and `NotoSansCJKsc-Subset.otf` under `vendor/fonts/noto`.

## 2. Live now (built and gated in the merged tree)

| Feature | Files | Gate |
|---|---|---|
| `fuse_ui_l10n_lib`: CLDR plurals, ICU-style messages, number / date / time / currency formatting, calendar names, string-table format | `U/L10n/**` (new). Changed for main: `std::from_chars` on doubles goes through `fuse/text/from_chars.hpp` (libc++ on the NDK / FreeBSD lacks it); `detail::toCharsFixed` in `l10n_internal.hpp` replaces `std::to_chars(..., fixed)` and uses printf on Apple (not compiled on Apple here; the printf path passes the gate on Linux with `-DFUSE_TEXT_FROM_CHARS_FALLBACK`) | `fuse_ui_l10n` |
| **Ported 2026-11-13 (item 3.12, the editor API):** `DockEdge`, `dockSplit`, `dockOpen`, `dockRemove`, `dockSideOf`, `dockSplitOf`, dock layout format 2 (`tree` lines); `treeLineAt`, `treeLineRect`; `TimelineClipDrag`, `timelineClipDrag`, `timelineSelectClip` (+ `timelineSelectedClip`), clip-drag events 5 / 6 / 7, Ctrl-key clip selection and retiming (event 2), Shift = free move | by hand on main's `U/Widgets/src/{widgets_tier_b2.cpp,widgets_tier_b.cpp,widgets_tier_d2.cpp}` and `widgets.hpp`: the splits are built on main's dock model, so they work with its tab groups, floating docks and dock drags | `fuse_ui_dock_split_{split,split_groups,tree_line,timeline_clip}` (`U/tests/test_dock_split.cpp`: the branch's `test_ui6.cpp` split checks plus the interplay with tabs / floating / drags), golden `fuse_ui_port_golden_dock_split`; main's `fuse_ui_dock_layout_roundtrip`, `fuse_ui_widget_dock_*`, `fuse_ui_dock_drag_*` unchanged |
| `LocaleHooks`, `UiContext::setLocale / locale / localeHooks`; localised calendar (month header, weekday names, first day of week) | ported by hand into main's `U/Core/include/fuse/ui/ui.hpp` and `U/Widgets/src/widgets_tier_cd.cpp` (`CalendarW`) | `fuse_ui_l10n` |
| `StringTable`: fallback locale, `has`, `tr(key, args)`, `bind(..., args)`, replace-on-re-add, `l10n::parseTable` format. Main's `trPlural` kept | `U/Game/include/fuse/ui/game/game_ui.hpp`, `U/Game/src/game_ui.cpp`; `fuse_ui_game` now links `fuse_ui_l10n_lib` | `fuse_ui_l10n`, `fuse_text_i18n`, `fuse_ui_pause_time_domain` |
| `doc::Translator`: load-time translation, `Document::retranslate`, `setTranslator`, `translatableCount` (JSON and cooked loads) | `U/Doc/include/fuse/ui/doc/document.hpp`, `U/Doc/src/document.cpp`, `U/Doc/src/cook.cpp` (the branch's `"icon"` document key was left out: §3.3) | `fuse_ui_l10n` |
| `fuse_ui_extract_strings` CLI | `U/Doc/tools/fuse_ui_extract_strings.cpp` (new) | `fuse_ui_l10n` |
| Lua handler lifetime (`LuaUi::handlerCount`, sweep on `update` / `ui.on` / `ui.destroy`) | `U/Script/include/fuse/ui/script/lua_ui.hpp`, `U/Script/src/lua_ui.cpp` | `fuse_ui_script_bind` (new, `U/tests/test_script_bind.cpp`), `fuse_ui_lua_bindings` |
| `JsonValue` out-of-line special members (Clang < 19 with libstdc++ 14) | `U/Doc/include/fuse/ui/doc/json.hpp`, `U/Doc/src/json.cpp` | every `fuse_ui_doc_*` gate |
| `FUSE_INSTRUMENTED_RUN` switch for wall-clock budgets (`timingBudgetsEnforced`) | `U/tests/test_env.hpp` (new), used by `test_app.cpp`, `test_bench_10k.cpp` | `fuse_app_hello_startup`, `fuse_ui_bench_10k` |
| Wayland backend of `fuse_app_native` (was 3.6): xdg-shell toplevels, `wl_shm` and Vulkan present, pointer / keyboard / touch, integer and fractional scale, cursor shapes, text clipboard, drops, text-input-v3, xdg-decoration, `osPopups` as xdg_popups, icons. Backend choice: `NativeOptions::backend`, `$FUSE_UI_BACKEND` (the branch's `$FUSE_APP_BACKEND` is gone), then `$WAYLAND_DISPLAY` | `U/App/src/native_wayland.cpp` + `wayland_internal.hpp` (the branch's protocol layer, adapted: main's `Cursor`, `WindowBackend` / `AppWindow::notify*` instead of the branch's chrome hooks and `platform.hpp`; added `wl_touch`, wheel steps / smooth axis, the frame-callback throttle, hide / show, size limits, restore size, `FreeBSD` shm file, connection loss); `U/App/src/native_wayland_platform.{hpp,cpp}` (new: the `NativePlatform` side); dispatch seam in `U/App/src/native.cpp` (`FUSE_APP_WL`), `native_services.inl` (2 lines); `SurfaceSource::Wayland` in `native_internal.hpp` / `vk_present.cpp`; `NativeBackend` in `native.hpp` | `fuse_app_wayland` (`U/tests/test_app_wayland.cpp`, rewritten for main: headless weston started by the gate), `fuse_app_wayland_seat` (`U/tests/test_app_wayland_seat.cpp`, new: in-process compositor) |
| HUD benchmark executable | `U/tests/test_bench_hud.cpp`, `U/Game/samples/hud_bench_scene.hpp` (new) | `fuse_ui_bench_hud` (registered since 2026-11-13, see 3.14) |
| **Ported 2026-11-13.** Themes (3.13): `ThemeVariant { Dark, Light, HighContrast }`, `ThemeDesc`, `themeCss`, `lightThemeCss`, `highContrastThemeCss(HighContrastPalette)`, `themeTokensCss`, `setTheme`; every colour of the theme rules and of the widget paint code is a `:root` token; focus rings; `UiContext::setThemeStyleSheet` (the theme slot) on sheets by handle | `U/Widgets/src/theme.cpp` (rewritten on main: token table with dark / light / high-contrast values), `widgets_internal.hpp` (`tok::`, `themed`), the paint functions of `U/Widgets/src/*.cpp`, `U/Core/src/style_sheets.cpp` (new), `U/App/src/services.cpp` (`applyTheme` on one replaced sheet) | `fuse_ui_theme_{sheet_handles,token_only,contrast,focus_ring}`, `fuse_ui_theme_golden_{light,contrast}_<scene>`, `fuse_ui_widget_golden_{controls,editors}`, `fuse_app_theme_sheets` |
| **Ported 2026-11-13.** ProjectHost (3.11): `fuse::app::ProjectHost`, targets `fuse_app_project`, `fuse_app_host`, and with `fuse_hybrid` also `fuse_app_project_hybrid`, `fuse_hybrid_host` (in tree only) | `U/App/include/fuse/app/project_host*.hpp`, `U/App/src/project_host*.cpp`, `U/App/samples/app_host.cpp`, `U/App/CMakeLists.txt` | `fuse_project_schema2`, `fuse_project_schema2_native` (see 3.11 for what was run) |
| **Ported 2026-11-13.** Performance gates (3.14): `fuse_ui_bench_hud` registered (label `perf`) after the style-hash fix in `StyleEngine` (0.72 -> 0.105 ms); `fuse_app_binary_size` with main's own baseline | `U/Core/src/style.cpp` (`hashStyle`), `U/tests/CMakeLists.txt`, `U/tests/test_app_binary_size.cpp` | `fuse_ui_bench_hud`, `fuse_app_binary_size` |

| **§3.1 text stack (ported 2026-11-13)**: `FUSE_UI_TEXT=COOKED` (`.fusefont`, `fuse_text_cook`, `cookFont`), MSDF, exact COLRv1, CBDT / sbix, variable fonts, `Font::withFeatures`, generated UAX #29 tables, caches, the catalogue | `U/Text/**` (merged by hand with main's `text.hpp` / `text.cpp` / `text_full.cpp`: main's `Font::synthesized`, `FontFamily`, `int weight()`, `int faceIndex`, decoration metrics, letter spacing and line height kept; the branch's `emboldened()` runs on main's embolden), `U/Core` (`ComputedStyle` + `font-stretch` / `font-variation-settings` / `font-feature-settings`, `UiContext::styledFont`, `setMsdfThreshold`, glyph modes `kGlyphColor` / `kGlyphMsdf` in `paintTextLayout`, `DrawList::scale` and the CPU rasteriser), `U/Render/src/gpu_draw_data.cpp` (skips the two new glyph modes), `U/Doc/src/cook.cpp` (`PropType::FontSettings`) | `fuse_text_{grapheme,linebreak,bidi}_conformance`, `fuse_text_{shaping_scripts,system_fonts,cache_zero_alloc,colrv1,dpi_1,dpi_1_25,dpi_1_5,dpi_2,bitmap_emoji,msdf,cooked,features,variable,bench_layout}`; the whole suite in MINIMAL, FULL and COOKED |
| **§3.3 icons (ported 2026-11-13, redesigned onto main's vector path)**: `fuse_ui_icon_cook`, `IconSet`, `.fuseicons` (version 3: parsed vector shapes), `UiContext::setIcons`, `icon` / `icon-size`, `widgets::icon`, `widgets::setIcon(ui, node, name)`, `"icon"` in `.fuseui`, `CookInfo::icons`, Lucide starter pack | `U/Core/include/fuse/ui/icon_set.hpp`, `U/Core/src/icon_set.cpp`, `U/Icons/**` (rewritten: no nanosvg), `U/Widgets/src/widgets_svg.cpp`, `U/Doc/src/doc_classes.cpp`, style engine (`Prop::Icon`, `Prop::IconSize`, `PropType::Ident`) | `fuse_ui_icon_cook`, `fuse_ui_icon_render` (`U/tests/test_icons.cpp`, rewritten; goldens `icons.png`, `icons_color.png` regenerated) |
| **§3.16 i18n fold (2026-11-13)**: one plural-rule table and one number-symbol table; `fuse/text/i18n.hpp` is a front end compiled into `fuse_ui_l10n_lib` | `U/L10n/src/{plural,format,text_i18n}.cpp`, `U/L10n/src/l10n_internal.hpp`, `U/Text/src/i18n.cpp` (pseudo-localisation only), `U/Text/include/fuse/text/i18n.hpp` | `fuse_text_i18n`, `fuse_ui_l10n` |

Standalone UI build on Linux after the merge: 180 ctest gates (the 178 main had, `fuse_ui_l10n`, `fuse_ui_script_bind`).
With the Wayland port: +2 (`fuse_app_wayland`, `fuse_app_wayland_seat`).

The platform-layer rows of §2.1 and the Wayland backend were ported side by side and then made consistent
(2026-11-13): every `NativePlatform` call of §2.1 goes through the Wayland dispatch seam. On Wayland the clipboard
formats, the primary selection, synthetic input, `cursorThemeSize()`, frame pacing and window chrome work (and a
window the compositor does not decorate gets the chrome as client-side decoration:
`NativeOptions::clientDecorations`); OS tool windows, drag out of the app and XSETTINGS do not exist there and answer
"nothing". Details in 3.6 / 3.7 and `docs/ui/KNOWN_GAPS.md`.

The theme port (3.13), the widget port (3.12 here, 3.7 / 3.12 of `PORT_gui_framework.md`) and the platform layer were
ported side by side and then made consistent (2026-11-13):
- Every colour of the ported widgets is a theme token: their rules in `theme.cpp` use `var(--token)` only and their
  paint code reads `tok::` tokens with the dark value it had (the dark goldens `port_*.png` did not change). New
  tokens: `--lcd-text`, `--lcd-bg`, `--workspace`, `--scene-bg`, `--item-focus`, `--placeholder`,
  `--text-disabled`, `--focus-ring`, `--window-close` (the table `kTokens` has 90 entries). High contrast outlines them;
  the editable combo box draws a focus ring around its field.
- `fuse_ui_theme_contrast` measures them (gallery + token pairs), `fuse_ui_theme_focus_ring` probes the composites
  and option-driven paint paths, `fuse_ui_theme_paint_literals` also catches `Color::rgb(cond ? 0x.. : 0x..)`,
  scans `App/src` and checks the `tok::` list against the theme table; the three port scenes have light and
  high-contrast goldens (`fuse_ui_theme_golden_<light|contrast>_port_<scene>`, MINIMAL).
- The client-side title bar (`App/src/chrome.cpp`, also the Wayland client-side decoration) is styled from tokens
  only, so `AppWindow::applyTheme` restyles it; X11 sets `SystemTheme::highContrast` from the theme name (XSETTINGS
  live, `$GTK_THEME` at start), gated in `fuse_app_native_services` and `fuse_app_theme_sheets`.
- Dock splits and OS tool windows (`NativeOptions::osWindows`, class `os-window`): `fuse_ui_dock_split_tool_windows`
  and the tool-window part of `fuse_app_native_services` float docks out of splits and tab groups, dock tool windows
  into splits, tear off / Escape / drop on a zone, and round-trip layout format 2 with floating docks.
- `StyleEngine`'s token colour cache has 512 slots (was 128): with the 90 tokens of the table four found no slot
  within its 8 probes and were resolved from the sheet text on every paint. `fuse_ui_theme_token_only` asks for
  every token and checks it is cached (`StyleEngine::tokenColorCached`).
- `fuse_app_binary_size` re-measured: 2,683,504 B, budget 2,951,000 (3.14, `docs/ui/BASELINES.md`).
Standalone UI build on Linux after this: 292 ctest entries.

Non-UI code that includes UI headers and compiles against main's UI (syntax-checked, not linked: the full engine does
not build on the merge machine): `Source/FUSE/Apps/HybridHud/hud_ui.cpp`, `Source/FUSE/Renderer/tests/test_scene_renderer_vk.cpp`
(its `--mode fuse_ui` part uses only `UiFrameSource` and `DrawList`).

### 2.1 Live since the app-platform port (2026-11-13)

Ported onto main's `services.hpp` / `NativePlatform` API (one platform layer; nothing of the branch's `native.cpp`
structure was taken). X11 parts run under Xvfb without a window manager; Win32 parts are in
`U/App/src/native_platform_win32.inl`, compiled and linked with MinGW-w64, **never run**.

| Feature (item) | Where on main | Gate |
|---|---|---|
| Window-less dialogs: open (filters, multiple) / save / folder / colour / message box, `revealInFileManager`, `PlatformError`, `isOpenableUrl` (3.5) | types in `fuse/app/services.hpp`, functions in `fuse/app/native.hpp`; X11 / BSD through zenity / kdialog (`native_platform.inl`), Win32 `IFileDialog` / `ChooseColorW` / `MessageBoxW`; `Unavailable` on macOS / iOS / Android (`native_common.cpp`). `fuse/app/platform.hpp` is now a compatibility header (differences: `openUrl` returns `bool`, `DisplayInfo::workArea` instead of `work`) | `fuse_app_dialog_helpers`, `fuse_app_native_services` (fake helpers) |
| Clipboard images, MIME types, lazy items, primary selection (3.5) | `ClipboardItem`, `kClipboardText / kClipboardImage`, `encodePng / decodePng` in `services.hpp`; `NativePlatform::setClipboard / setClipboardImage / clipboardImage / clipboardData / clipboardFormats / clipboardOwned / setPrimarySelectionText / primarySelectionText`. X11: on the platform's own connection, `TARGETS`, INCR both ways. Wayland: `wl_data_source` with every item's MIME type, `zwp_primary_selection_v1` (`native_wayland_platform.cpp`). `fuse/app/clipboard.hpp` is a compatibility header (the branch's free functions became methods of the platform that owns the display) | `fuse_app_clipboard_png`, `fuse_app_native_services`, `fuse_app_wayland_seat` |
| X11 XSETTINGS: `Xft/DPI` live, theme name / prefer-dark live, cursor theme and size (3.5) | `XSetting`, `parseXSettings`, `buildXSettings` in `services.hpp`; `NativePlatform::Impl::readXSettings`, `cursorThemeSize()` | `fuse_app_dialog_helpers` (parser), `fuse_app_native_services` (the gate is the settings manager) |
| OS tool windows for floating docks (3.5; `PORT_gui_framework.md` 3.5) | `UiContext::setOsWindows / osWindows / windowSurfaces`, class `os-window` (set by `widgets::dockFloat`); `NativeOptions::osWindows` (default **off** on main, the branch had on), `NativePlatform::toolWindowCount / toolWindow / toolWindowScreenRect / captureToolWindow`. X11 / Win32 only: ignored on Wayland (0 / false) | `fuse_app_tool_surfaces`, `fuse_app_native_services` |
| Synthetic input `postPointerMove`, `postPointerButton`, `postClose` (3.5) | `NativePlatform` (Wayland: delivered in-process, `wl::injectPointer` / `injectClose`) | used by the XDND / chrome / tool-window sections of `fuse_app_native_services`; `fuse_app_wayland`, `fuse_app_wayland_seat` |
| Saved state across process death (3.5) | `Settings::items / toText / fromText`, `AppWindow::saveUiState / restoreUiState / saveWindowState / restoreWindowState`, `Application::saveState / restoreState / saveStateTo / restoreStateFrom / restored / restoredState`, `StateHooks` (function table instead of the branch's `InlineFn` callbacks); Android `APP_CMD_SAVE_STATE` hook in `native_android.cpp` (cross-compiled only) | `fuse_app_saved_state`, `fuse_app_native_services` |
| Window chrome (3.7) | `WindowFrame`, `WindowChrome`, `WindowCommand`, `WindowDesc::frame / resizeBorder`, `AppWindow::chrome / command`; `fuse/app/chrome.hpp`, `U/App/src/chrome.cpp` (built into `fuse_app`). The branch's backend command hook is gone: the buttons call main's window management (`minimize`, `setMode`, `tryClose`). X11 without a window manager moves / resizes in the backend. Wayland: caption / edge presses become `xdg_toplevel.move` / `resize` (`chromePress` in `native_wayland.cpp`), and the chrome is the client-side decoration of windows the compositor does not decorate | `fuse_app_window_chrome`, `fuse_app_native_services`, `fuse_app_wayland`, `fuse_app_wayland_seat` |
| Notifications (3.8) | in the live Extras desktop integration, not in `fuse_app`: `fuse::ui::desktop::Notifier` (`org.freedesktop.Notifications` on `extras/dbus.hpp`: actions, `onAction`, `onClosed`, replace, close), `NotifyResult`, `notifyNative`, `notificationsSupported`, `notify()` native first; `TrayIcon::showMessage / onMessageClicked` (D-Bus; Win32 `NIF_INFO` balloon, not run) | `fuse_ui_extras_notify` |

Parked files deleted because they are superseded: `U/App/src/platform_posix.cpp`, `chrome_internal.hpp`,
`chrome_win32.cpp`, `U/App/include/fuse/app/shell.hpp`, `U/App/src/shell_unix.cpp`, `shell_win32.cpp`,
`U/tests/test_app_tray.cpp`, `test_app_tray_host.cpp` (the tray itself is `gui-framework`'s `Extras/tray`, see
`PORT_gui_framework.md` §6: no second tray was ported).

## 3. Not live: port items

Each item: what it is, where the branch code is, what it overlaps on main, what blocks it, size (S: under a day,
M: days, L: a week or more), and how to verify. "Gates" are the branch's ctest names; their registrations are in the
branch's `U/tests/CMakeLists.txt` (`git show 7178393b4fea:Source/FUSE/UI/tests/CMakeLists.txt`), which main did not take.

Order that unblocks the most: 3.1 (text) and 3.2 (core deltas) first; 3.3, 3.4, 3.6 and 3.9 depend on them.
Items tagged **editor** are needed before `-DFUSE_EDITOR_SHELL=fuse` compiles (§5).

### 3.1 Text: COOKED profile, MSDF, colour glyphs, variable fonts, OpenType features, font catalogue — **ported 2026-11-13**

Live (§2). Decisions taken and what is left:

- **Catalogue decision**: main's `font_family.hpp` stays the one public API. The branch's `FontCatalog` class was
  folded into it and `U/Text/src/font_catalog.cpp` deleted: its script tables, coverage check, fallback chains,
  generic families and named-instance listing are in `U/Text/src/font_scripts.cpp` over `SystemFontInfo` /
  `systemFonts()`; file helpers (`readFile`, `MappedFile`, name strings) in `U/Text/src/text_io.cpp`.
- **Synthetic bold**: main's (`Font::synthesized`, coverage sum-smear, `FontFamily` strengths) is the only
  implementation. `Font::emboldened()` (em / 24, used for bold fallbacks) is main's embolden on a shared state.
  `Font::weight()` stays `int`. The UI's synthesised bold does not restyle fallbacks (main's documented behaviour;
  the branch's did): `tests/test_text_fallback.cpp` asserts main's.
- **FULL layout**: the branch's `fullLayout` (fallback runs shaped once inside the bidi pass, bidi level cache,
  4-way run cache) replaced main's `applyFallbackPerLine`; main's letter spacing, line height and oblique shear were
  re-applied on it. All FULL goldens are unchanged.
- **Not ported** (branch deltas that remain, all in files other agents own or that need a GPU):
  - GPU: the shader / packer support for `kGlyphColor` and `kGlyphMsdf` (`U/Render/**`, §3.4). The packer skips
    those commands; `test_text_color.cpp`'s `FUSE_TEXT_COLOR_HAVE_VK` parity block is compiled out.
  - `UiContext::animate(..., AnimProp::FontWeight)` (the branch's animation enum); `font-stretch` is animatable
    through main's transitions instead, `font-weight` is not.
  - The branch's paint-time glyph quad cache (`m_quads`) and the CPU rasteriser rewrite (§3.2).
  - `-ffunction-sections` / `--gc-sections` for the UI targets and the `hud` binary-size gate (§3.14).
  - `tests/golden/rtl_layout*.png`, `test_keyboard_only.cpp`, `cpu_raster_*.hpp` (§3.2).
- **Not verified here**: Windows (registry font list, `MappedFile`, Segoe UI Variable / Bahnschrift checks), Apple;
  COOKED was built and run on Linux only (cross builds need `-DFUSE_TEXT_COOK_HOST`).

### 3.2 Core deltas: device scale, zoom, cursor, reduced motion, touch, keyboard-only, hover fix (L, decisions)

- **What**: the branch's own versions of features main also built after base, plus smaller additions.
- **Branch deltas not taken**: `U/Core/include/fuse/ui/ui.hpp` (+188), `U/Core/src/ui.cpp` (+757 / -52),
  `style.hpp` (+57), `style.cpp` (+253), `platform_bridge.hpp`, `draw_list.hpp` (+25), `draw_list.cpp` (+909 / -133).
- **Duplicates of main (take main's, port only what main lacks)**: `UiContext::setScale` (main: render scale, commit
  `a925efbd1`), `cursor`, `reducedMotion / setReducedMotion`, `touchCancel`, `touchPanning`, `flip`, `markAllDirty`,
  `nodeFont`, `paintIcon`; `CursorShape` (branch: `fuse/ui/style.hpp`; main: `fuse/app/services.hpp`); RTL
  (`direction`, `mirrorX`, `baseDirection`; main: commit `37c62f78d`, gate `fuse_ui_rtl_layout`).
- **Missing on main (port)**: `zoom` property and `kineticZoom` / `setKineticZoom` (`platform_feel.hpp` / `.cpp`
  deltas, pinch and Ctrl+wheel); `Key::Alt` release toggling the menu bar; focus rings and scroll-into-view fixes
  from the keyboard-only pass; `m_msdfThreshold`; OS text scale; `PointerKind` / pressure on pointer events (check
  against main's pen / touch events first); the CPU rasteriser work in `draw_list.cpp` (span runs, banded worker
  threads, SSE2 / NEON edges, `-ffp-contract=off`, paint-time text cache).
- **In the tree, unbuilt**: `U/tests/test_keyboard_only.cpp` (907 lines), `U/tests/test_cpu_raster_bench.cpp`,
  `cpu_raster_fuzz.hpp`, `cpu_raster_reference.hpp`. `U/tests/test_rtl_layout.cpp` exists on both sides with different
  content: main's file is in the tree, the branch's 725-line version is a branch delta, with goldens
  `U/tests/golden/rtl_layout.png` and `rtl_layout_full.png` in the tree (unused).
- **Blocks**: each sub-feature is interleaved with main's edits in `ui.cpp` (13 conflict hunks). Port one sub-feature
  at a time; do not re-run the textual merge.
- **Verify**: `fuse_ui_keyboard_only`, `fuse_ui_cpu_raster_bench` (perf), the branch's RTL assertions folded into
  `fuse_ui_rtl_layout`, `fuse_ui_platform_feel` with the branch's zoom cases (`test_platform_feel.cpp` delta, +63);
  the CPU rasteriser must stay bit-exact: `fuse_ui_cpu_raster_golden`, `fuse_ui_single_thread_parity`, every golden.

### 3.3 Icons: SVG cooker, `IconSet`, `.fuseicons`, `icon` style property — **ported 2026-11-13** **editor**

Live (§2). The decision asked for here was taken as suggested, and further: main's `icons.hpp` API and `SvgImage`
renderer stay; `IconSet` lives in `fuse/ui/icon_set.hpp` (included by `icons.hpp`, so the editor shell's
`#include "fuse/ui/icons.hpp"` finds it); and the cooker does **not** use nanosvg. Main's `SvgImage` parses the whole
Lucide subset and the colour test set, so a pack is the parsed vector shapes in binary form (version 3) and icons
draw through `Painter` like every other vector image. `vendor/nanosvg` is no longer referenced by any UI target.

What the FUSE-UI editor shell uses is there: `IconSet::fromFile`, `IconSet::find`, `UiContext::setIcons`,
`widgets::setIcon(ui, node, std::string_view)`.

Left out, with the branch code that has it:

- `:name:` in menu item text (`widgets.cpp` delta) and the icon column of menus.
- `kDisabledIconAlpha` greying of fixed-colour layers on disabled nodes.
- `fuse_ui_cook --icons=` / `--icon-pack=` (`U/Doc/tools/fuse_ui_cook.cpp` delta); `CookInfo::icons` exists.
- `notes_app` toolbar icons, `hello.fuseui`, the designer's icon picker (§3.15).
- The branch's coverage-atlas design itself (per-size hinted rasters, icons as Glyph commands on the GPU path):
  superseded; its `icon_cook.cpp`, `test_icons.cpp` and goldens were replaced, not kept.

### 3.4 GPU: backdrop blur, damage present, SDF-quad backend, custom effects, pipeline cache (L)

- **What**: `ui.draw_list` kernel additions (GPU blur, MSDF glyphs), damage-driven incremental present, a raster
  SDF-quad backend (`ui_sdf_quad.vert / .frag`), custom shader effects (`effects.hpp`, `EffectRegistry`, `effect:`
  style, built-ins desaturate / ripple / grain), persistent pipeline cache.
- **In the tree, unbuilt**: `U/Core/include/fuse/ui/effects.hpp`, `U/Core/src/effects.cpp` (compiles against main's
  headers as is); `U/Render/src/{ui_vk_effects,ui_vk_sdf}.cpp`, `U/Render/cmake/{fuse_ui_effects,make_effect}.cmake`,
  `U/Render/src/shaders/{ui_effect.comp.in,ui_glyph.glsl,ui_math.glsl,ui_sdf_quad.vert,ui_sdf_quad.frag}`,
  `shaders/effects/{desaturate,grain,ripple}.glsl`; tests `U/tests/test_vk_{budget,custom_effect,damage,gates,sdf_quads,validation_clean}.cpp`,
  `test_ui_baselines.cpp`, `U/tests/data/fx_vignette_image.glsl`.
- **Branch deltas not taken**: `U/Render/include/fuse/ui/render/{gpu_draw_data,ui_vk,ui_layer_gpu}.hpp`,
  `U/Render/src/{gpu_draw_data,ui_vk,ui_layer_gpu}.cpp` (`ui_vk.cpp` +459), `shaders/ui_draw_list.comp` (+215) and
  its fixture `.spv`, `U/tests/{test_vk_parity.cpp,vk_headless.hpp}`, `DrawList::effect` and `kGlyphMsdf` in
  `draw_list.hpp / .cpp`, `effect` in `style.cpp`; in `U/CMakeLists.txt`: `fuse_ui_vulkan_headers` (headers only when
  `fuse_rhi` supplies the loader; main has its own fix for the same MSVC LNK2005, compare before porting), `-g0` for
  glslang, `FUSE_UI_LVP_ENV` (system ICD when lavapipe is absent, for native Windows runs).
- **Overlap on main**: main changed `gpu_draw_data.hpp / .cpp` for vector paths (pre-rasterised Image commands,
  gate `fuse_ui_gpu_pack_vector`); the two `GpuDrawData` layouts differ. KNOWN_GAPS lists GPU blur, damage present
  and custom effects as missing.
- **Blocks**: merge the two `GpuDrawData` layouts and kernel command sets (main's Image op + the branch's blur /
  effect / MSDF ops), then the shader and the fixture `.spv` have to be regenerated together. Needs a Vulkan loader
  and lavapipe; the merge machine had neither.
- **Verify**: `fuse_ui_vk_parity` (bit-exact against the CPU rasteriser on lavapipe), `fuse_ui_vk_{damage,validation_clean,budget,custom_effect,sdf_quads}`,
  `fuse_ui_vk_{golden,tier0,zero_alloc}`, `fuse_ui_baselines`, main's `fuse_ui_gpu_pack_vector`; in tree
  `fuse_scene_renderer_vk_fuse_ui_t0`, `fuse_ui_hudless_history_invariant`, `fuse_demo_hybrid_hud_ui`.

### 3.5 App platform layer: dialogs, clipboard formats, X11 parity, saved state, presenter work (L, decision) **editor**

- **What**: `fuse/app/platform.hpp` (open / save / folder / colour dialogs, `revealInFileManager`, `PlatformError`),
  `fuse/app/clipboard.hpp` (images, MIME, lazy formats, primary selection), X11 RandR / XSETTINGS / themed cursor
  sizes / XIM pre-edit, OS tool windows for floating docks (`NativeOptions::osWindows`, `NativePlatform::toolWindow*`,
  `UiContext::osWindows / windowSurfaces`), synthetic input (`postPointerMove`, `postPointerButton`), saved state
  across process death, async Vulkan presenter init, software presenter dirty-rect copy, MAILBOX pacing,
  `presentTiming`, `hello_app --startup-probe`.
- **In the tree, unbuilt**: `U/App/include/fuse/app/{platform,clipboard}.hpp`,
  `U/App/src/{clipboard,platform_posix,platform_win32}.cpp`, `{platform_internal,x11_internal,dbus_internal}.hpp`;
  tests `U/tests/test_platform_{desktop,services,text,x11}.cpp`, `test_x11_wm.cpp`,
  `test_app_{idle_native,input_latency,native_startup,binary_size}.cpp`.
- **Branch deltas not taken**: `U/App/src/native.cpp` (+1665 / -212, 33 conflict hunks), `native_internal.hpp`,
  `vk_present.cpp` (+232), `app.cpp` (+195), `U/App/include/fuse/app/{app,native}.hpp`, `hello_app.cpp`,
  `U/App/CMakeLists.txt`, tests `test_app_native.cpp`, `test_app_os_popups.cpp`.
- **Overlap on main**: `fuse/app/services.hpp` + `native_services.inl` already have cursors, system theme, displays,
  file dialogs, clipboard text, `openUrl`, window state (gates `fuse_app_*` services, `fuse_app_native_services`).
  Including `platform.hpp` next to `services.hpp` fails today: `DisplayInfo` is defined twice and `openUrl` has two
  signatures.
- **Blocks**: decision: main's `services.hpp` stays the public API; fold in what it lacks (colour dialog,
  `revealInFileManager`, clipboard images / MIME / primary selection, RandR, XSETTINGS) and keep `platform.hpp` only
  as a thin compatibility header if the editor shell is to compile unchanged. The X11 internals
  (`x11_internal.hpp`) assume the branch's `native.cpp` structure.
- **Status (2026-11-13)**: dialogs, clipboard formats, XSETTINGS, tool windows, synthetic pointer input and saved
  state are live (§2.1). **Still not ported**, with the parked file that holds it:
  - XIM pre-edit and the IMM32 text-input session (`textInputActive`, `textInputArea`, `inputMethodAvailable`):
    `x11_internal.hpp`, branch delta of `native.cpp`; tests `test_platform_text.cpp`. Main's X11 text input is still
    `XLookupString`.
  - RandR change notification and per-window `setWindowScale`: `x11_internal.hpp`; `test_platform_x11.cpp`
    (`fuse_x11_randr`). `displays()` on main already lists RandR monitors.
  - A settings manager that starts after the app (the `MANAGER` client message on the root window) is not watched.
  - Win32: delayed clipboard rendering (`WM_RENDERFORMAT`), `WM_POINTER` touch / pen, the theme fields main's
    `SystemTheme` lacks (high contrast palette, reduced motion, text scale), dark title bar, `MessageHook`:
    `clipboard.cpp` (Win32 half), `platform_win32.cpp`, `platform_internal.hpp`; `test_platform_desktop.cpp`,
    `test_platform_services.cpp`.
  - Wayland routing of the clipboard: done on main's API (`WaylandPlatform::setClipboard` ... in
    `native_wayland_platform.cpp`); the parked `clipboard.cpp` (Wayland half) is not needed for it any more.
  - Presenter work: async Vulkan start (`asyncVulkan`, `presenterPending`, `waitForPresenter`), software-presenter
    dirty-rect copy (`AppWindow::rasterized`), MAILBOX pacing, `presentTiming`, pipeline-cache directory,
    `hello_app --startup-probe`, `NativePlatform::wake`: branch deltas of `vk_present.cpp` / `native.cpp` / `app.cpp`;
    `test_app_{idle_native,input_latency,native_startup,binary_size}.cpp`. Not attempted: no Vulkan loader on the port
    machine.
  - Gates that need a window manager (`test_x11_wm.cpp`: `fuse_x11_wm_{chrome,popups,multiwindow}`) were not
    brought over; main's gate covers the no-window-manager paths only.
  - `dbus_internal.hpp` stays because the parked `test_a11y_linux.cpp` (3.9) includes it.
- **Verify**: `fuse_platform_{dpi_change,displays,cursors,system_theme,high_contrast,dialogs,touch_synthetic,pen_synthetic,multiwindow}`,
  `fuse_platform_{text_input,ime_composition,clipboard_roundtrip}`, `fuse_x11_{randr,xsettings,wake,cursor_size}`,
  `fuse_x11_wm_{chrome,popups,multiwindow}`, `fuse_ui_platform_services` (in tree: needs `fuse/platform/event_pump.hpp`),
  `fuse_app_idle_native`, `fuse_app_input_latency`, `fuse_app_native_startup`, `fuse_app_binary_size`; main's
  `fuse_app_native_services` and `fuse_app_native_window` unchanged. All need Xvfb.

### 3.6 Wayland backend: ported (2026-11-13), remainder (S-M)

The backend is live: see §2. `native_wayland.cpp`, `wayland_internal.hpp` and `tests/test_app_wayland.cpp` are built
(adapted in place). What of the branch is **not** ported:

- **Still parked, unbuilt**: `U/tests/test_app_wayland_parity.cpp` (841 lines) and `U/tests/wayland_peer.hpp` (658
  lines, a second Wayland client on its own thread). They are the branch's `fuse_app_wayland_popups` and
  `fuse_app_wayland_seat` gates. Their popup checks (xdg_popup vs the CPU raster, a click on an item outside the
  window, a child tool toplevel) are now part of `fuse_app_wayland`; real seat events are covered by main's new
  `fuse_app_wayland_seat` against an in-process compositor. Not covered anywhere, and the reason the two files stay:
  a headless **sway** run with the second client driving `zwlr_virtual_pointer_v1` / `zwp_virtual_keyboard_v1`
  (grabbed popups ended by an outside click, keys driving a grabbed menu), live per-output scale changes
  (`swaymsg output scale`), an input method client (`zwp_input_method_v2`), clipboard formats beyond text
  (image/png, custom MIME, lazy items) and the primary selection end to end between two clients (main's seat gate
  plays the other client from inside the compositor), a drag from the second client. They need
  `NativePlatform::windowFocused`, which main does not have (`clipboard.hpp`, `postPointerMove / postPointerButton /
  postClose` now exist and work on Wayland), and a sway binary (not installed on the port machine).
- **Wired after the platform layer (§2.1) landed**: the primary selection (`setPrimarySelectionText /
  primarySelectionText`), clipboard formats, interactive move / resize for window chrome (`chromeHitTest` is back
  in the `wl_pointer` press path: `wl::beginMove` / `beginResize`), client-side decoration fallback.
- **Still in the protocol layer only**: child tool toplevels (`wl::createTool`, `moveChild`, `resizeChild`).
  `NativeOptions::osWindows` is ignored on Wayland and `NativePlatform::toolWindow*` answer 0 / false. What wiring
  needs: a `syncTools` in `native_wayland_platform.cpp` like the X11 one (create / present / resize a child per
  `UiContext::windowSurfaces()` entry), and, because a client cannot place a toplevel, `widgets::dockFloat`'s title
  drag handed to `wl::moveChild` instead of moving the node, with drop-zone detection that does not rely on screen
  coordinates. `fuse_app_wayland` exercises `createTool` directly.
- **Dropped**: the branch's `asyncVulkan` start (main's presenter creation is synchronous), tablet tool cursor
  shapes (the interface table is there, nothing binds a tablet seat), `$FUSE_APP_BACKEND` (now `$FUSE_UI_BACKEND`),
  the rule "X11 when `$DISPLAY` is set" (now Wayland first when `$WAYLAND_DISPLAY` is set).
- **Verify**: `fuse_app_wayland` (headless weston 13: passes), `fuse_app_wayland_seat` (in-process compositor:
  passes); the Vulkan part of `fuse_app_wayland` needs a build with a Vulkan loader and an ICD with
  `VK_KHR_wayland_surface` (run once with lavapipe: GPU draw, 0 pixels off the CPU raster).

### 3.7 Window chrome (M)

- **What**: client-side window frame: `WindowChrome`, `WindowFrame`, hit testing (`WM_NCHITTEST` on Win32,
  `_NET_WM_MOVERESIZE` on X11).
- **In the tree, unbuilt**: `U/App/include/fuse/app/chrome.hpp`, `U/App/src/{chrome.cpp,chrome_internal.hpp,chrome_win32.cpp}`,
  `U/tests/test_app_chrome.cpp`.
- **Blocks**: `chrome.cpp` needs `WindowChrome` in the branch's `app.hpp` / `native.hpp` and `x11_internal.hpp`
  (3.5). Main's `mainWindow` widget and window-state service do not cover it.
- **Status (2026-11-13)**: live (§2.1). Left parked: `U/tests/test_app_chrome.cpp`, for its two parts main has no
  gate for: the Win32 real-window checks (`WM_NCHITTEST` answers, non-client button clicks; needs Windows or Wine)
  and the X11 check that a caption / edge press sends `_NET_WM_MOVERESIZE` with the right direction (the test acts
  as a window manager through `x11_internal.hpp`). On main that hand-over is written but was never observed.
  Wayland (after 3.6): the hand-over is `xdg_toplevel.move` / `resize`; `fuse_app_wayland_seat` observes both
  requests with the press serial on its in-process compositor, and the fallback title bar for undecorated windows
  is checked there and under weston. No compositor has moved a window by it (weston headless has no pointer).
- **Verify**: `fuse_app_window_chrome`, `fuse_x11_wm_chrome`.

### 3.8 Tray icon and notifications (M)

- **What**: `fuse/app/shell.hpp`: StatusNotifierItem + dbusmenu over a small D-Bus client on Linux,
  `Shell_NotifyIcon` and toast notifications on Win32.
- **In the tree, unbuilt**: `U/App/include/fuse/app/shell.hpp`, `U/App/src/{shell_unix,shell_win32}.cpp`,
  `dbus_internal.hpp`; `U/tests/test_app_tray.cpp`, `test_app_tray_host.cpp`.
- **Blocks**: `shell_unix.cpp` fails only on `fuse::ui::CursorShape` pulled in through `platform.hpp` (3.5);
  otherwise self-contained. `gui-framework` has another tray (`Extras/tray`): pick one when that branch is merged.
- **Status (2026-11-13)**: closed. The tray was not ported (decision in `PORT_gui_framework.md` §6: the live
  `Extras/tray` stays); the notification API has its equivalent in `fuse::ui::desktop` (§2.1) and the parked tray
  files are deleted. Not carried over: `TrayIcon::screenRect` (Win32 `Shell_NotifyIconGetRect`), `onDoubleClick`,
  and Win32 notifications without a tray icon (the branch added a transient icon; main's `desktop::notify` uses the
  PowerShell toast helper on Windows).
- **Verify**: `fuse_app_tray` (with `fuse_app_tray_host`, under `dbus-run-session`).

### 3.9 Accessibility on a native window: `WindowAccessibility`, UIA and AT-SPI adapters (M) **editor**

- **What**: `fuse_app_a11y`: `WindowAccessibility` binds an `a11y::Bridge` to a native window; AccessKit
  `WindowsAdapter` (UI Automation) and `UnixAdapter` (AT-SPI); `FUSE_UI_ACCESSKIT_BUILD` fetches accesskit-c 0.22.3
  and builds it with cargo (Rust >= 1.85).
- **In the tree, unbuilt**: `U/App/include/fuse/app/accessibility.hpp`, `U/App/src/accessibility.cpp`;
  `U/tests/test_a11y_linux.cpp` (real AT-SPI client over D-Bus), `test_a11y_windows.cpp` (real UIA client).
- **Branch deltas not taken**: `U/A11y/include/fuse/ui/a11y/a11y.hpp` (+65), `U/A11y/src/a11y.cpp` (+99),
  `a11y_accesskit.cpp` (+157), `test_a11y_bridge.cpp` (+31), `test_a11y_tree_dump.cpp` (scroll bars focusable), the
  AccessKit block of `U/CMakeLists.txt`.
- **Overlap on main**: main extended the same three files for grid / tree roles (commit `941737b9c`, gates
  `fuse_ui_a11y_family_*`).
- **Blocks**: `accessibility.cpp` needs `Bridge::setScale` and `Bridge::setWakeHandler` plus the adapter classes
  from the branch's `a11y.hpp`; merge them with main's role additions. A Rust toolchain is needed to build AccessKit.
- **Verify**: `fuse_ui_a11y_linux` (under `dbus-run-session` + Xvfb), `fuse_ui_a11y_windows` (native Windows),
  main's `fuse_ui_a11y_*` gates, in tree `fuse_editor_ui_a11y*`.

### 3.10 Android: branch backend, JNI IME, APK packer, emulator gates (M, decision)

- **What**: the branch's `native_android.cpp` (1258 lines: JNI IME composition, async Vulkan, lifecycle,
  `safeAreaInsets`), `fuse_app_add_android_app` / `fuse_app_add_android_gate` (APK without Gradle), emulator gate
  script, `android_demo`, `hud_bench` app.
- **In the tree, unbuilt**: `U/App/android/{fuse_apk.py,android_gate.py}`, `U/App/cmake/FuseAndroidApp.cmake`,
  `U/App/samples/android_demo.cpp`, `U/Game/samples/hud_bench.cpp`. Outside the UI: `Source/FUSE/Renderer/android/`
  (`fuse_runtime_android`, gate `fuse_runtime_present_android`); `Source/FUSE/CMakeLists.txt` adds it only when the
  command `fuse_app_add_android_app` exists, so it is off until `FuseAndroidApp.cmake` is included.
- **Branch delta not taken**: `U/App/src/native_android.cpp` (add / add conflict; main's 739-line file kept).
- **Overlap on main**: main's Android backend, `entry_android.cpp`, `fuse/app/main.hpp` (`FUSE_APP_MAIN`),
  `fuse_app_add_executable`, packaging helpers in `U/cmake`; CI `fuse-core-android.yml` builds it.
- **Blocks**: decision on the backend: keep main's and port JNI IME / lifecycle / safe-area from the branch, then
  adapt `FuseAndroidApp.cmake` to main's `fuse_app_add_executable` and entry point. `android_demo.cpp` needs
  `NativePlatform::lifecycle` and `safeAreaInsets`; `present_android.cpp` needs `NativePlatform::androidApp()`.
- **Verify**: Android cross-build of the UI tree; `fuse_ui_bench_hud_mobile` and the `android_gate.py` gates on an
  emulator; `fuse_runtime_present_android`.

### 3.11 ProjectHost and `project.json` schema 2 (M)

**Ported 2026-11-13** (listed in §2). What was done and what was not:

- `project_host.cpp` needed one change for main: the branch's `Application::setFollowSystemTheme` does not exist, so
  `ui.theme: "system"` calls `AppWindow::setFollowSystemTheme(true)` per window, and `dark` / `light` go through
  `widgets::setTheme` (the theme slot from 3.13). `AppWindow::followsSystemTheme()` was added for the gate.
- `U/App/CMakeLists.txt`: `fuse_app_project` + `fuse_app_host` when `fuse_project` and `fuse_core` exist;
  `fuse_app_project_hybrid` + `fuse_hybrid_host` when `fuse_hybrid` exists.
- `U/tests/test_project_schema2.cpp`: the hybrid game-side parts are inside `#if FUSE_PROJECT_SCHEMA2_HYBRID` (set when
  `fuse_app_project_hybrid` is linked); without it the gate checks that an app-only host refuses a hybrid project.
  The system-theme check now also changes the system theme and expects the window to restyle.
- The native part of the gate waited for the branch's asynchronous Vulkan start-up
  (`NativePlatform::presenterPending`); main installs the presenter in `show()`, so that wait is gone.
- **Verified 2026-11-13** in the engine tree configured with `-DFUSE_BUILD_VULKAN=OFF` (no Vulkan loader on the port
  machine): `fuse_app_host`, `fuse_app_project_hybrid` and `fuse_project_schema2` build and link (GCC 13.3);
  `fuse_project_schema2` passes: 7 schema 1 sample projects unchanged, 24 invalid manifests rejected, themes, an app
  project up with only `AppPresent` (first frame 1.3 ms headless), the hybrid game side deferred until the viewport
  shows (software frames). It printed "hybrid GPU (headless): no Vulkan device, software viewport (pixel checks
  skipped)", and `--native` skipped (no display). `fuse_app_host Templates/FuseUIApp --headless --first-frame`
  runs. The rest of the engine tree was not built.
- **Remains**: run `fuse_project_schema2` with Vulkan (the hybrid viewport renders on the game side's device) and
  `--native` on a display; the Android `fuse_ui_bench_hud_mobile` / APK side of the branch is 3.10.

### 3.12 Widgets: per-class gates; what is left (S-M)

**Ported 2026-11-13** (see §2): dock splits and layout format 2, tree line geometry, timeline clip drag and selection.
Signatures are the branch's (the editor shell's call sites in `Source/FUSE/EditorShell/Fuse/src/editor_shell*.cpp`
were read against them). Semantics that differ from the branch, because main's dock model is richer:

- `DockEdge::Center` makes the dock a **tab** of the target's group and raises it (`dockTabify` + `dockRaise`); the
  branch stacked the two docks in one pane. The editor shell does not call `dockSplit` with `Center` (it keeps its own
  tab stacks), and its `dropEdge() == Center` is its own enum use.
- A split beside a tabbed dock moves the whole tab group (strip and members) into the pane.
- `dockSaveLayout` writes `fusedock 2` only when the layout has splits; a layout without splits is still
  `fusedock 1`, byte-identical to before (main's layout gates depend on that). `tab` and `float` lines are valid in
  both versions. `dockRestoreLayout` flattens existing splits before applying either version.
- `dockOpen` on a tabbed dock brings its tab back without changing the current tab.
- Main's dock drag (title-bar tear-off, drop zones) still applies to docks in splits. The editor shell implements
  its own panel drag; whether both should be active on its stacks is a shell decision (not built here).

- **Still not live**:
  - `setListRowFont` (`widgets_internal.hpp` delta; per-row fonts of the font picker): belongs with 3.1 (font
    catalogue).
  - `setIcon(ui, node, name)` / `icon(...)` by `IconSet` name: 3.3.
  - Per-widget-class gates and goldens: `U/tests/test_widget_gates.cpp` (924 lines), `U/tests/golden/widgets/*.png`
    (36 images) stay parked. The images were rendered with the branch's theme and text stack and cannot be reused;
    the gate file needs the branch's RTL helpers and icon API (3.2, 3.3) to compile.
  - The RTL parts of the branch's widget deltas (mirrored hit testing in tree / tab bar / dock title): 3.2.
  - Keyboard additions in the branch's deltas that main lacks: F10 focuses the menu bar; tooltips on keyboard focus;
    node-graph and curve-editor keys (PageUp / PageDown / arrows); timeline PageUp / PageDown scrub by a second.
  - The editor shell itself (`-DFUSE_EDITOR_SHELL=fuse`) was not built: it also needs 3.3, 3.5 and 3.9 (§5).
    `g++ -fsyntax-only` over `EditorShell/Fuse/src/editor_shell.cpp` and `editor_shell_panels.cpp` against the
    ported headers (2026-11-13) reports no error for the dock, tree-line or timeline API any more; what is left there
    is `fuse::ui::IconSet`, `UiContext::setIcons`, `setIcon(ui, node, name)` (3.3) and `UiContext::osWindows` (3.5).
- **Verify**: the gates in §2; `fuse_editor_ui_*` once the shell builds.

### 3.13 Themes: light, high contrast, token-only colours, focus rings (M)

**Ported 2026-11-13** (listed in §2), on main's theme rather than as the branch delta:

- The branch moved the theme rules' literals into tokens; on main the widget paint code is tokenised too (106
  `Color::rgb(0x...)` sites in `U/Widgets/src/*.cpp` now read `tok::` tokens through `UiContext::themeColor`), so the
  light and high-contrast sets restyle sliders, scroll bars, code editor, node graph, timeline and the rest.
- `UiContext::setThemeStyleSheet` exists as the branch had it, built on `StyleEngine::replaceSheet` by handle. The
  first call adds the sheet where it stands (as `install` always did), later calls replace it in place.
- The dark rendering did not change: all existing goldens compare with 0 differing pixels. The focus-ring rules do
  change a focused widget, so the dark dialogs / wizard scenes are captured with the focus parked (their FULL-profile
  goldens cannot be re-rendered without the fetched text stack); the rings are covered by the new goldens.
- **Not ported**: `StyleEngine::setForcedColors` (drop blur / shadows / gradients of application sheets in high
  contrast): the high-contrast token set only clears the theme's own shadows. `SystemTheme` has `highContrast`; only
  X11 sets it (from the theme name in XSETTINGS / `$GTK_THEME`, since the merge with the platform layer) and it
  carries no palette (the branch's `SystemTheme::contrast`, `reducedMotion`, `textScale` are
  3.5). The branch gates `fuse_platform_high_contrast` / `fuse_platform_system_theme` (`test_platform_desktop.cpp`)
  stay parked with 3.5; `fuse_app_theme_sheets` covers the headless part.

### 3.14 Performance gates and baselines (S)

**Ported 2026-11-13** (listed in §2).

- `fuse_ui_bench_hud`: main really was slower than the branch on this scene, and not because of the missing
  paint-time text cache. `StyleEngine::intern` hashed a handful of style fields; the styles a HUD animates differ in
  fields that were not hashed (anchored / absolute offsets, the other colour channels), so they shared one probe
  chain and every lookup compared `ComputedStyle`s along it (callgrind: 80.7 % of the bench in `intern`). With
  placement, margins and all colour channels in the hash: 0.65-0.77 ms -> 0.104-0.109 ms on the port machine (4
  cores, load average 16 to 36, six runs each). The gate is registered with labels `gate;perf`;
  `FUSE_UI_PORTING_BENCH_HUD` is gone. Numbers: `docs/ui/BASELINES.md`.
- `fuse_app_binary_size`: registered with main's own baseline (stripped `hello_app`, Linux x86-64 GCC 13.3 Release
  MINIMAL standalone: 1,936,432 B, budget 2,130,000). Other toolchains report and skip (77) until
  `-DFUSE_APP_BINARY_BUDGET` is set. The branch's byte counts were not reused. Re-measured when the theme, widget
  and platform ports met (2026-11-13): 2,683,504 B (platform layer + Wayland +345,632, widget port +393,248, their
  theme tokens +8,192), budget 2,951,000; the steps are in `docs/ui/BASELINES.md`.
- **Remains**: `fuse_ui_hud_binary_size` (needs the COOKED text profile, 3.1); `scripts/ui_baselines.py` was not run
  against main; the baselines of the other toolchains (MSVC, Clang) are not measured.

### 3.15 Designer additions (S, after 3.3 and 3.4)

*Still parked (checked 2026-11-13): the icon picker needs the `IconSet` / `icon` style property of 3.3 and the effect
picker the custom effects of 3.4; neither was live when this item was looked at. The registry-driven property grid
has no such prerequisite and can be ported alone.*

- **What**: icon picker, effect picker, registry-driven property grid.
- **Branch deltas not taken**: `U/Designer/include/fuse/ui/designer/designer.hpp` (+25), `U/Designer/src/designer.cpp`
  (+178), `U/Designer/app/designer_main.cpp`, `U/tests/test_designer.cpp` (+104).
- **Verify**: `fuse_ui_designer`.

### 3.16 Fold `fuse/text/i18n.hpp` into `fuse_ui_l10n_lib` — **done 2026-11-13**

One implementation: `l10n::pluralCardinal` (extended with the families only `i18n.hpp` had: be, sr / hr / bs, sl,
lt, lv, ro, cy, ga, hy and the n = 1 / i = 0 or n = 1 lists; `da` corrected to the CLDR rule) and one number-symbol
table (`format.cpp`, extended with `i18n.hpp`'s rows). `fuse/text/i18n.hpp` keeps its API (source compatible) and is
compiled into `fuse_ui_l10n_lib` (`text_i18n.cpp`), which `fuse_text` links; pseudo-localisation stays in
`fuse_text`. `StringTable::trPlural` and `tr(key, args)` therefore use the same rules. Two differences are kept and
documented in both headers: the English fallback of `fuse::text::pluralCategory` for a language without rules
(`l10n::hasPluralRules`), and the rounding / minus sign of `fuse::text::formatNumber`.

## 4. Existing UI files: what the merge kept

Main's content, with the branch hunks of §2 added where marked `+`. Everything else the branch changed in these
files is a branch delta assigned to the item in the last column.

| File (under `U/`) | Kept | Branch delta belongs to |
|---|---|---|
| `A11y/include/fuse/ui/a11y/a11y.hpp`, `A11y/src/a11y.cpp`, `A11y/src/a11y_accesskit.cpp` | main | 3.9 |
| `App/CMakeLists.txt`, `App/include/fuse/app/{app,native}.hpp`, `App/src/{app,native,vk_present}.cpp`, `App/src/native_internal.hpp`, `App/samples/hello_app.cpp` | main | 3.5, 3.6, 3.7, 3.11 |
| `App/src/native_android.cpp` (add / add) | main | 3.10 |
| `App/samples/game_menu.cpp` (add / add) | main | none: same sample written twice |
| `App/samples/notes_app.cpp`, `App/samples/ui/hello.fuseui` | main | 3.3 |
| `CMakeLists.txt`, `tests/CMakeLists.txt` | main + L10n / extract / script_bind / bench_hud | every item (targets and gate registrations) |
| `Core/include/fuse/ui/{draw_list,platform_bridge,style}.hpp`, `Core/src/{draw_list,style}.cpp`, `Core/src/ui.cpp` | main | 3.1, 3.2, 3.3, 3.4 |
| `Core/include/fuse/ui/ui.hpp` | main + `LocaleHooks`, `setLocale` | 3.2, 3.3, 3.5 |
| `Core/include/fuse/ui/icons.hpp`, `Core/src/icons.cpp` (add / add) | main | 3.3 |
| `Designer/**`, `tests/test_designer.cpp` | main | 3.15 |
| `Doc/include/fuse/ui/doc/cook.hpp` | main | 3.3 (`kCookedVersion`, `CookInfo::icons`) |
| `Doc/src/cook.cpp` | main + translator arguments | 3.1 (`PropValue::tags`), 3.3 |
| `Doc/include/fuse/ui/doc/document.hpp`, `Doc/src/document.cpp` | main + translator | 3.3 (`"icon"` key) |
| `Doc/include/fuse/ui/doc/json.hpp`, `Doc/src/json.cpp` | main + branch (complete) | none |
| `Doc/tools/fuse_ui_cook.cpp` | main | 3.3 |
| `Game/include/fuse/ui/game/game_ui.hpp`, `Game/src/game_ui.cpp` | main + `StringTable` (complete) | none |
| `Render/**` (headers, sources, shader, fixture `.spv`) | main | 3.4 |
| `Script/**` | main + branch (complete) | none |
| `Text/include/fuse/text/text.hpp`, `Text/src/{text,text_full}.cpp`, `Text/src/text_full.hpp` | main | 3.1 |
| `Widgets/include/fuse/ui/widgets/widgets.hpp`, `Widgets/src/widgets*.cpp`, `widgets_internal.hpp` | main; `widgets_tier_cd.cpp` + calendar locale | 3.2 (RTL helpers), 3.12 |
| `Widgets/include/fuse/ui/widgets/platform_feel.hpp`, `Widgets/src/platform_feel.cpp`, `tests/test_platform_feel.cpp` | main | 3.2 (zoom) |
| `Widgets/src/theme.cpp` | main | 3.13 |
| `tests/test_app.cpp`, `tests/test_bench_10k.cpp` | branch (complete) | none |
| `tests/test_text_styles.cpp` | main, font counts 8 -> 10 | none |
| `tests/test_rtl_layout.cpp` (add / add) | main | 3.2 |
| `tests/{test_a11y_bridge,test_a11y_tree_dump}.cpp` | main | 3.9, 3.2 |
| `tests/{test_app_native,test_app_os_popups}.cpp` | main | 3.5 |
| `tests/{test_text_basics,test_text_full,test_text_fallback}.cpp`, `tests/test_util.hpp` | main | 3.1, 3.4 |
| `tests/{test_tier_d2,test_ui6}.cpp` | main | 3.12 |
| `tests/{test_vk_parity.cpp,vk_headless.hpp}` | main | 3.4 |

## 5. Non-UI code blocked on UI API

| Target | Enabled by | State | Needs |
|---|---|---|---|
| `fuse_editor_fuse`, `fuse_editor_fuse_app`, gates `fuse_editor_ui_*` (`Source/FUSE/EditorShell/Fuse`, `EditorShell/tests/test_editor_ui_*.cpp`) | `-DFUSE_EDITOR_SHELL=fuse` (default `qt`) | **Does not compile.** Not part of the default build, so nothing was gated off. | ~~3.12 (`DockEdge`, `dockSplit`, `dockSplitOf`, `dockSideOf`, `dockOpen`, `dockRemove`, `treeLineAt`, `treeLineRect`, `TimelineClipDrag`, `timelineSelectClip`)~~ (live since 2026-11-13), 3.3 (`IconSet`, `UiContext::setIcons`, `setIcon` by name), 3.5 (`fuse/app/platform.hpp` without the `DisplayInfo` / `openUrl` clash, `NativeOptions::osWindows`, `NativePlatform::toolWindow / toolWindowCount / toolWindowScreenRect / captureToolWindow / postPointerMove / postPointerButton`, `UiContext::osWindows / windowSurfaces`), 3.9 (`fuse/app/accessibility.hpp`) |
| `fuse_editor_controller`, `fuse_editor_scene_host`, `fuse_editor_controller_api`, `fuse_editor_panel_models` | `FUSE_BUILD_EDITOR_API` / `FUSE_BUILD_EDITOR` | Qt-free, no UI dependency; taken from the branch | nothing |
| `fuse_runtime_android` (`Source/FUSE/Renderer/android`) | Android + command `fuse_app_add_android_app` | Off (the command is not defined) | 3.10 |
| `demo_hybrid_hud` with `hud_ui.cpp` | `fuse_ui_game` + `fuse_ui_vk` + Vulkan | Compiles against main's UI (syntax check) | nothing; gate `fuse_demo_hybrid_hud_ui` not run |
| `fuse_app_host` | (target not defined on main) | Unbuilt | 3.11 |

The error list for the editor shell came from compiling `Source/FUSE/EditorShell/Fuse/src/*.cpp`, `Fuse/app/main.cpp`
and `EditorShell/tests/test_editor_ui_*.cpp` with `-fsyntax-only` against main's UI headers. Main's own UI changed
since base as well, so the shell may also need adapting to main-side API changes that those errors hide.

## 6. Not verified by the merge

- The full engine was configured and built in tree on Linux without a Vulkan loader (`cmake -S . -DFUSE_BUILD_CORE=ON
  -DFUSE_BUILD_CORE_TESTS=ON -DFUSE_WARNINGS_AS_ERRORS=ON`, GCC 13.3; `fuse_ui_vk`, the Vulkan RHI paths, Qt and CUDA
  targets were not configured). Everything compiled and linked after two fixes to branch code that only showed
  without the Vulkan backend (`renderer/vk/raster_path.hpp`: `colorImageLayout()` used a member declared under
  `FUSE_VULKAN_BACKEND`; `skinning_gpu.cpp`: helpers unused without it), except two files of main's own UI that GCC
  13 rejects under `-Werror` and the merge did not touch: `U/Widgets/src/widgets.cpp:535` (`-Wdangling-reference`,
  suppressed for the check) and `U/tests/test_modelview.cpp:51` (`-Wmismatched-new-delete`; the `fuse_ui_modelview`
  executable and its 12 `fuse_ui_mv_*` gates were therefore not built in that tree; they pass in the standalone
  build, which has no `-Werror`).
- The in-tree ctest run was cut short at test 967 of 1322 when the disk filled up: 914 passed, 36 skipped;
  `fuse_b5_rhi_spirv_val`, `fuse_package_gate`, `fuse_core_io_lane` and `fuse_b7_streaming_gates` failed and were
  not diagnosed (the last three ran while the disk was full). The remaining 355 tests did not run. Treat the
  branch's Renderer / Remaster / Tools gates as not verified on main.
- Apple: no `.mm` file was added or changed. `U/L10n` and the `StringTable` / calendar changes are new code in the
  macOS and iOS builds and were not compiled for Apple.
- Windows: not compiled here. In tree, `cmake/FuseWarnings.cmake` gives every target `/utf-8`, which the L10n
  sources need (UTF-8 string literals). The branch also added those MSVC options (`/utf-8`, `/bigobj`,
  `/permissive-`, `/Zc:__cplusplus`) to the standalone UI `CMakeLists.txt`; main did not take that hunk, so a
  standalone MSVC build of the UI would compile L10n in the system code page.
- Vulkan gates (`fuse_ui_vk_parity`, `fuse_app_native_window`) did not run: no loader / display.
