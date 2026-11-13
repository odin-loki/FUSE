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
| `LocaleHooks`, `UiContext::setLocale / locale / localeHooks`; localised calendar (month header, weekday names, first day of week) | ported by hand into main's `U/Core/include/fuse/ui/ui.hpp` and `U/Widgets/src/widgets_tier_cd.cpp` (`CalendarW`) | `fuse_ui_l10n` |
| `StringTable`: fallback locale, `has`, `tr(key, args)`, `bind(..., args)`, replace-on-re-add, `l10n::parseTable` format. Main's `trPlural` kept | `U/Game/include/fuse/ui/game/game_ui.hpp`, `U/Game/src/game_ui.cpp`; `fuse_ui_game` now links `fuse_ui_l10n_lib` | `fuse_ui_l10n`, `fuse_text_i18n`, `fuse_ui_pause_time_domain` |
| `doc::Translator`: load-time translation, `Document::retranslate`, `setTranslator`, `translatableCount` (JSON and cooked loads) | `U/Doc/include/fuse/ui/doc/document.hpp`, `U/Doc/src/document.cpp`, `U/Doc/src/cook.cpp` (the branch's `"icon"` document key was left out: §3.3) | `fuse_ui_l10n` |
| `fuse_ui_extract_strings` CLI | `U/Doc/tools/fuse_ui_extract_strings.cpp` (new) | `fuse_ui_l10n` |
| Lua handler lifetime (`LuaUi::handlerCount`, sweep on `update` / `ui.on` / `ui.destroy`) | `U/Script/include/fuse/ui/script/lua_ui.hpp`, `U/Script/src/lua_ui.cpp` | `fuse_ui_script_bind` (new, `U/tests/test_script_bind.cpp`), `fuse_ui_lua_bindings` |
| `JsonValue` out-of-line special members (Clang < 19 with libstdc++ 14) | `U/Doc/include/fuse/ui/doc/json.hpp`, `U/Doc/src/json.cpp` | every `fuse_ui_doc_*` gate |
| `FUSE_INSTRUMENTED_RUN` switch for wall-clock budgets (`timingBudgetsEnforced`) | `U/tests/test_env.hpp` (new), used by `test_app.cpp`, `test_bench_10k.cpp` | `fuse_app_hello_startup`, `fuse_ui_bench_10k` |
| HUD benchmark executable | `U/tests/test_bench_hud.cpp`, `U/Game/samples/hud_bench_scene.hpp` (new). Built always; the ctest gate is registered only with `-DFUSE_UI_PORTING_BENCH_HUD=ON` (§3.12) | (not registered by default) |

Standalone UI build on Linux after the merge: 180 ctest gates (the 178 main had, `fuse_ui_l10n`, `fuse_ui_script_bind`).

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
| Clipboard images, MIME types, lazy items, primary selection (3.5) | `ClipboardItem`, `kClipboardText / kClipboardImage`, `encodePng / decodePng` in `services.hpp`; `NativePlatform::setClipboard / setClipboardImage / clipboardImage / clipboardData / clipboardFormats / clipboardOwned / setPrimarySelectionText / primarySelectionText`. X11: on the platform's own connection, `TARGETS`, INCR both ways. `fuse/app/clipboard.hpp` is a compatibility header (the branch's free functions became methods of the platform that owns the display) | `fuse_app_clipboard_png`, `fuse_app_native_services` |
| X11 XSETTINGS: `Xft/DPI` live, theme name / prefer-dark live, cursor theme and size (3.5) | `XSetting`, `parseXSettings`, `buildXSettings` in `services.hpp`; `NativePlatform::Impl::readXSettings`, `cursorThemeSize()` | `fuse_app_dialog_helpers` (parser), `fuse_app_native_services` (the gate is the settings manager) |
| OS tool windows for floating docks (3.5; `PORT_gui_framework.md` 3.5) | `UiContext::setOsWindows / osWindows / windowSurfaces`, class `os-window` (set by `widgets::dockFloat`); `NativeOptions::osWindows` (default **off** on main, the branch had on), `NativePlatform::toolWindowCount / toolWindow / toolWindowScreenRect / captureToolWindow` | `fuse_app_tool_surfaces`, `fuse_app_native_services` |
| Synthetic input `postPointerMove`, `postPointerButton`, `postClose` (3.5) | `NativePlatform` | used by the XDND / chrome / tool-window sections of `fuse_app_native_services` |
| Saved state across process death (3.5) | `Settings::items / toText / fromText`, `AppWindow::saveUiState / restoreUiState / saveWindowState / restoreWindowState`, `Application::saveState / restoreState / saveStateTo / restoreStateFrom / restored / restoredState`, `StateHooks` (function table instead of the branch's `InlineFn` callbacks); Android `APP_CMD_SAVE_STATE` hook in `native_android.cpp` (cross-compiled only) | `fuse_app_saved_state`, `fuse_app_native_services` |
| Window chrome (3.7) | `WindowFrame`, `WindowChrome`, `WindowCommand`, `WindowDesc::frame / resizeBorder`, `AppWindow::chrome / command`; `fuse/app/chrome.hpp`, `U/App/src/chrome.cpp` (built into `fuse_app`). The branch's backend command hook is gone: the buttons call main's window management (`minimize`, `setMode`, `tryClose`). X11 without a window manager moves / resizes in the backend | `fuse_app_window_chrome`, `fuse_app_native_services` |
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

### 3.1 Text: COOKED profile, MSDF, colour glyphs, variable fonts, OpenType features, font catalogue (L)

- **What**: `FUSE_UI_TEXT=COOKED` (`.fusefont` packs, `fuse_text_cook`), MSDF glyphs, exact COLRv1, CBDT / sbix
  bitmaps, `fvar / avar / gvar / HVAR / MVAR`, `font-feature-settings`, `FontCatalog` (system fonts), UAX #29 / #14 / #9
  tables in MINIMAL, layout caches, shared PNG codec.
- **In the tree, unbuilt** (24 files): `U/Text/include/fuse/text/{font_cook,png_codec}.hpp`;
  `U/Text/src/{color_bitmap,colr_paint,font_catalog,font_cook,font_var,msdf,paint_enable,path_raster,png_codec,text_cooked}.cpp`,
  `{font_var,fusefont,glyph_paint,text_cooked,text_internal}.hpp`, `unicode_tables.inc`;
  `U/Text/tools/{fuse_text_cook.cpp,cook_reference.hpp,gen_unicode_tables.py}`.
  Tests: `U/tests/test_text_{bench_layout,color,conformance,cooked,features,ui3,variable}.cpp`; data
  `U/tests/data/{bitmap_cbdt_test,bitmap_sbix_test,colrv1_test,features_test,variable_test,variable_avar2_test}.ttf`,
  the `make_*_test_font.py` scripts, `U/tests/data/unicode/*`; goldens `U/tests/golden/{colrv1_exact,shaping_scripts_full}.png`.
- **Branch deltas not taken**: `U/Text/include/fuse/text/text.hpp` (+273), `U/Text/src/text.cpp` (+1087 / -262),
  `text_full.cpp` (+284), `text_full.hpp`; tests `test_text_basics.cpp`, `test_text_full.cpp`, `test_text_fallback.cpp`
  (+107: Thai and CJK fallback), `test_util.hpp` (`testFontPath`, `setEffects`); the text part of `U/CMakeLists.txt`
  (`fuse_text_cook_lib`, cooked bundled fonts, `-ffunction-sections` / `--gc-sections`).
- **Overlap on main**: `U/Text/src/font_family.cpp` + `font_family.hpp` (system font scan, families, synthetic
  bold / italic) against the branch's `FontCatalog`; `U/Text/src/i18n.cpp`; main's own edits to `text.cpp` (9 conflict
  hunks, 382 lines).
- **Blocks**: every new file needs types that exist only in the branch's `text.hpp` (`ColorImage`, `ColorBitmap`,
  `FontVariation`, `FontFeature`, `Script`, `LineBreak`, `TextProfile::Cooked`, `unicodeVersion`). **Decision needed**:
  keep main's `FontFamily` API as the public one and put `FontCatalog` behind it, or the reverse.
  `png_codec.cpp` compiles alone, but it defines the stb_image / stb_image_write implementations inside `fuse_text`;
  check for duplicate definitions against the other UI users of stb before adding it.
- **Verify**: gates `fuse_text_{grapheme,linebreak,bidi}_conformance`, `fuse_text_{shaping_scripts,system_fonts,cache_zero_alloc,colrv1,dpi_1,dpi_1_25,dpi_1_5,dpi_2}`,
  `fuse_text_{bitmap_emoji,msdf}`, `fuse_text_cooked`, `fuse_text_features`, `fuse_text_variable`,
  `fuse_text_bench_layout`; a second configure with `-DFUSE_UI_TEXT=COOKED` running the whole suite; main's
  `fuse_text_font_styles`, `fuse_text_fallback`, `fuse_text_i18n` and every golden unchanged in MINIMAL and FULL.

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

### 3.3 Icons: SVG cooker, `IconSet`, `.fuseicons`, `icon` style property (M, decision) **editor**

- **What**: `fuse_ui_icon_cook` (nanosvg at tool time) cooks SVGs into a `.fuseicons` pack (v2: multi-colour);
  `IconSet::fromFile`, `UiContext::setIcons`, `icon` / `icon-size` style properties, `:name:` in menu item text,
  `widgets::setIcon(ui, node, "name")`, `"icon"` key in `.fuseui`, the Lucide starter pack cooked at build time.
- **In the tree, unbuilt**: `U/Icons/include/fuse/ui/icons/icon_cook.hpp`, `U/Icons/src/icon_cook.cpp`,
  `U/Icons/tools/fuse_ui_icon_cook.cpp`, `U/tests/test_icons.cpp`, `U/tests/data/icons_color/*.svg`, goldens
  `U/tests/golden/{icons,icons_color}.png`. Vendored and used only by this: `vendor/nanosvg`, `vendor/icons/lucide`
  (both in THIRD_PARTY.md).
- **Branch deltas not taken**: the branch's `U/Core/include/fuse/ui/icons.hpp` (93 lines) and `U/Core/src/icons.cpp`
  (154): **same paths as main's files, different content** (add / add conflict, main's kept); icon parts of
  `ui.hpp`, `style.cpp`, `widgets.cpp`; `U/Doc/src/cook.cpp` + `cook.hpp` (`CookInfo::icons`, `kCookedVersion`),
  `U/Doc/tools/fuse_ui_cook.cpp` (`--icons=`), `U/App/samples/notes_app.cpp` (toolbar icons),
  `U/App/samples/ui/hello.fuseui`, `U/Designer/**` (icon picker), the `icon` shorthand in `document.cpp`.
- **Overlap on main**: built-in vector icons drawn through `Path` / `SvgImage` (`icons.hpp`, `svg.hpp`,
  `widgets_svg.cpp`; `"icon": "folder"` on `svgimage`, button icons; gates `fuse_ui_svg`, `fuse_ui_vector_golden_icons`).
- **Blocks**: decision on one icon system. Suggested: keep main's `icons.hpp` API and `SvgImage` renderer, add the
  cooker as a second source of icons (pack -> registered named icons), put the branch's `IconSet` in a new header
  (`fuse/ui/icon_set.hpp`) instead of replacing `icons.hpp`. `icon_cook.cpp` needs `kIconPackMagic`,
  `kIconPackVersion` and `IconSet` from the branch's `icons.hpp`.
- **Verify**: `fuse_ui_icon_cook_gate`, `fuse_ui_icon_render`, `fuse_ui_icons` (`test_icons.cpp`), the icon picker
  cases of `fuse_ui_designer` (`test_designer.cpp` delta, +104), main's `fuse_ui_svg` and vector goldens unchanged.

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
  - Wayland routing of the clipboard: `clipboard.cpp` (Wayland half), with item 3.6.
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

### 3.6 Wayland backend (L)

- **What**: plan phase UI11. xdg-shell, xdg_popup, child tool windows, fractional scale, clipboard / primary
  selection / drops, text-input-v3; libwayland-client, xkbcommon and wayland-cursor loaded at run time, protocol
  tables written out (no wayland-scanner).
- **In the tree, unbuilt**: `U/App/src/native_wayland.cpp` (2093 lines), `wayland_internal.hpp`;
  `U/tests/test_app_wayland.cpp`, `test_app_wayland_parity.cpp`, `wayland_peer.hpp`.
- **Blocks**: depends on 3.5 (`platform.hpp` types, `clipboard.hpp`, the branch's `native_internal.hpp` backend
  seam, `CursorShape` in `fuse::ui`). Backend choice is by `FUSE_APP_BACKEND` and `WAYLAND_DISPLAY`. `gui-framework`
  has a second Wayland backend (`App/src/wayland.cpp`): pick one when that branch is merged.
- **Verify**: `fuse_app_wayland`, `fuse_app_wayland_popups` (headless weston), `fuse_app_wayland_seat` (headless
  sway with a second client driving the seat); the gates skip (77) without a compositor.

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

- **What**: `fuse::app::ProjectHost` (`fuse_app_host`): run an app / hybrid project from `project.json` schema 2
  (`AppPresent`), hybrid game side on a UI context.
- **In the tree, unbuilt**: `U/App/include/fuse/app/{project_host,project_host_hybrid}.hpp`,
  `U/App/src/{project_host,project_host_hybrid}.cpp`, `U/App/samples/app_host.cpp`, `U/tests/test_project_schema2.cpp`.
  The non-UI half is live: `Source/FUSE/Project` (manifest schema 2), `Source/FUSE/Core/include/fuse/core/track_b.hpp`,
  `Templates/FuseUIApp`.
- **Blocks**: in tree only (`fuse/project/manifest.hpp`). Needs the `U/App/CMakeLists.txt` delta (targets
  `fuse_app_host`, `fuse_app_host_hybrid`) and whatever of 3.5 `project_host.cpp` uses from the branch's `app.hpp`.
- **Verify**: `fuse_project_schema2`, `fuse_project_schema2_native` (in-tree build with Vulkan).

### 3.12 Widgets: dock splits, tree line geometry, timeline clip drag, per-class gates (M) **editor**

- **What**: `DockEdge`, `dockSplit`, `dockSplitOf`, `dockSideOf`, `dockOpen`, `dockRemove` (nested splitters, dock
  layout format 2); `treeLineAt`, `treeLineRect`; `TimelineClipDrag`, `timelineSelectClip`; `setListRowFont`;
  `setIcon(ui, node, name)`; per-widget-class gates and goldens.
- **Branch deltas not taken**: `U/Widgets/include/fuse/ui/widgets/widgets.hpp` (+75),
  `U/Widgets/src/{widgets,widgets_tier_b,widgets_tier_b2,widgets_tier_cd,widgets_tier_d2}.cpp`,
  `widgets_internal.hpp`, tests `test_tier_d2.cpp` (+47), `test_ui6.cpp` (+109).
- **In the tree, unbuilt**: `U/tests/test_widget_gates.cpp` (924 lines), `U/tests/golden/widgets/*.png` (36 images).
- **Overlap on main**: tabbed / floating docks with drag and drop and drop zones (commits `1d72473de`, `d73a3472a`;
  `dockFloat`, `dockRaise`, `dockTabify`; gates `fuse_ui_dock_drag_*`, `fuse_ui_dock_layout_roundtrip`); main's
  timeline and tree view.
- **Blocks**: `dockSplit` has to be built on main's dock model (the branch's lives in its own `widgets_tier_b2.cpp`,
  9 conflict hunks); the saved layout format needs a version bump that still reads main's. The per-class goldens
  were rendered with the branch's theme and text, so they must be re-rendered and reviewed, not reused.
- **Verify**: `fuse_ui_widget_<class>` gates and `fuse_ui_widget_catalogue` (`FUSE_UI_WIDGET_GATES` list in the
  branch's tests CMake), `fuse_ui_dock_layout_roundtrip` with format 2, main's `fuse_ui_dock_drag_*`.

### 3.13 Themes: light, high contrast, token-only colours, focus rings (M)

- **What**: `ThemeVariant`, `themeCss`, `lightThemeCss`, `highContrastThemeCss(HighContrastPalette)`; every literal
  colour of the default theme moved to `:root` tokens; `:focus-visible` rings and `:disabled` opacity for more classes.
- **Branch delta not taken**: `U/Widgets/src/theme.cpp` (+112), the declarations in `widgets.hpp`.
- **Overlap on main**: `defaultThemeCss`, system theme restyle (gate `fuse_app_theme_restyle`).
- **Blocks**: nothing structural. The focus-ring and disabled rules change most widget goldens: port the token
  refactor first (no pixel change expected), then the new rules with a reviewed golden update.
- **Verify**: `fuse_platform_high_contrast`, `fuse_platform_system_theme`, all goldens.

### 3.14 Performance gates and baselines (S)

- **What**: `fuse_ui_bench_hud` (293-node HUD, UI CPU per frame median <= 0.5 ms), `fuse_app_binary_size`,
  `fuse_ui_hud_binary_size`, `scripts/ui_baselines.py`, `docs/ui/BASELINES.md`.
- **State**: `fuse_ui_bench_hud` builds; on the merge machine (4 cores, load average 3.5) main's core measured
  0.57 to 0.68 ms, so the gate is registered only with `-DFUSE_UI_PORTING_BENCH_HUD=ON`. Not measured on a quiet
  machine. `test_app_binary_size.cpp` is unbuilt: its budgets are byte counts of the branch's `hello_app`.
- **Blocks**: 3.2 (paint-time text cache) for the HUD budget; new baselines measured on main for the size gates.
- **Verify**: `ctest -R fuse_ui_bench_hud` alone on a quiet machine; re-measure and set `FUSE_APP_BINARY_BUDGET`.

### 3.15 Designer additions (S, after 3.3 and 3.4)

- **What**: icon picker, effect picker, registry-driven property grid.
- **Branch deltas not taken**: `U/Designer/include/fuse/ui/designer/designer.hpp` (+25), `U/Designer/src/designer.cpp`
  (+178), `U/Designer/app/designer_main.cpp`, `U/tests/test_designer.cpp` (+104).
- **Verify**: `fuse_ui_designer`.

### 3.16 Fold `fuse/text/i18n.hpp` into `fuse_ui_l10n_lib` (S, decision)

Both are live. `fuse::text::formatPlural / formatNumber / pluralCategory` (main) and `fuse::ui::l10n` (branch)
implement CLDR plural rules twice. `StringTable::trPlural` uses the first, `tr(key, args)` the second. Decide
whether `fuse_text` keeps a minimal copy (it must not depend on the UI) or `trPlural` forwards to `l10n`.
Gates: `fuse_text_i18n`, `fuse_ui_l10n`.

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
| `fuse_editor_fuse`, `fuse_editor_fuse_app`, gates `fuse_editor_ui_*` (`Source/FUSE/EditorShell/Fuse`, `EditorShell/tests/test_editor_ui_*.cpp`) | `-DFUSE_EDITOR_SHELL=fuse` (default `qt`) | **Does not compile.** Not part of the default build, so nothing was gated off. | 3.12 (`DockEdge`, `dockSplit`, `dockSplitOf`, `dockSideOf`, `dockOpen`, `dockRemove`, `treeLineAt`, `treeLineRect`, `TimelineClipDrag`, `timelineSelectClip`), 3.3 (`IconSet`, `UiContext::setIcons`, `setIcon` by name), 3.5 (`fuse/app/platform.hpp` without the `DisplayInfo` / `openUrl` clash, `NativeOptions::osWindows`, `NativePlatform::toolWindow / toolWindowCount / toolWindowScreenRect / captureToolWindow / postPointerMove / postPointerButton`, `UiContext::osWindows / windowSurfaces`), 3.9 (`fuse/app/accessibility.hpp`) |
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
