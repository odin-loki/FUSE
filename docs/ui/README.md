# FUSE UI

FUSE's own GUI framework ([plan](../plans/FUSE_UI_PLAN.md)). One retained core, styled with a CSS subset, drawn
through a backend-neutral draw list, plus an immediate-mode front-end on the same primitives. Engine rules apply:
handles, not pointers; `std::expected`; one owner thread (other threads `post()`); no steady-state allocation.

Status (2026-10-29): headless core (UI2) with all nine gates green, `.fuseui` documents and Lua bindings, MINIMAL
text plus an opt-in FULL profile (HarfBuzz + FreeType, SheenBidi bidi, libunibreak line breaking, shaped-run cache),
Tier A and Tier B widgets including splitters, dock area, item models and a table view (UI6), a Vulkan compute
backend for the draw list feeding both the runtime UI stage (`UiFrameSource`) and app windows (UI4), desktop text
input / IME / wheel / clipboard on Win32 and X11 (UI1), the `fuse_app` application shell with real OS windows and a
Vulkan swapchain presenter (UI5), and the game UI library `fuse_ui_game` (UI7). Relight's dev menu runs on the
immediate-mode front-end. See the plan's status table for what is still open.

## Layout of the code

| Path | Target | What |
|---|---|---|
| `Source/FUSE/UI/Text` | `fuse_text` | Fonts (stb_truetype; FreeType in FULL), bundled Open Sans, UTF-8, `ITextShaper` (`MinimalShaper`, `HarfBuzzShaper` in FULL), wrap/ellipsis layout, glyph atlas, `TextEditBuffer` |
| `Source/FUSE/UI/Core` | `fuse_ui_core` | `UiContext` (tree, style, layout, paint, input, focus, signals, timers, animation, a11y), `DrawList` + `CpuRasterizer`, `StyleEngine`, `immediate::ImmediateUi`, `platform_bridge.hpp` |
| `Source/FUSE/UI/Widgets` | `fuse_ui_widgets` | Tier A + Tier B widgets + default dark theme (`widgets::install`) |
| `Source/FUSE/UI/Doc` | `fuse_ui_doc` | `.fuseui` JSON documents, binding model, expression bytecode |
| `Source/FUSE/UI/Script` | `fuse_ui_script` | Lua `ui` table (vendored Lua 5.2) |
| `Source/FUSE/UI/Render` | `fuse_ui_render`, `fuse_ui_vk` | GPU draw data packing, `ui.draw_list` compute kernel, `UiLayerGpu` render-graph pass and `UiFrameSource` (in tree with the Vulkan RHI) |
| `Source/FUSE/UI/App` | `fuse_app`, `fuse_app_native` | `Application` / `AppWindow` on-demand loop, settings, command line, single instance; native X11 / Win32 windows and the Vulkan swapchain presenter; samples `hello_app`, `notes_app` |
| `Source/FUSE/UI/Game` | `fuse_ui_game` | HUD widgets, world markers, `UiSurface` world panels, gamepad navigation, input arbitration, prompts, time domains, locale table, subtitles, dialogue; samples `hud_demo`, `world_panel_demo` |
| `Source/FUSE/UI/Sql` | `fuse_ui_sql` | `SqliteModel`: an `ItemModel` over a SQLite query (option `FUSE_BUILD_UI_SQL`) |
| `Source/FUSE/UI/A11y` | `fuse_ui_a11y` | Accessibility bridge (snapshot diff, action queue); AccessKit adapter when `FUSE_UI_ACCESSKIT_DIR` is set |
| `Source/FUSE/UI/Designer` | `fuse_ui_designer_lib`, `fuse_ui_designer` | `.fuseui` designer prototype (UI8) |
| `Source/FUSE/Core/include/fuse/platform/text_services.hpp` | `fuse_core` | clipboard, text-input begin/end, IME candidate placement |
| `Source/FUSE/UI/tests` | gates | `fuse_ui_*` ctest executables and `fuse_ui_sample` |
| `Source/FUSE/Core/include/fuse/platform/events_v2.hpp` | header | `KeyV2` / `EventV2` with lossless v1 mapping |

`fuse_ui_core` does not depend on `fuse_core`; the platform bridge is header-only.

## Build and test

In tree it builds with `FUSE_BUILD_UI=ON` (default). Standalone, which is quickest:

```sh
cmake -S Source/FUSE/UI -B build-ui -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build-ui && ctest --test-dir build-ui
build-ui/tests/fuse_ui_sample out.png          # renders the Tier A sample via the CPU rasteriser
```

Regenerate the raster golden after an intended visual change:
`cd Source/FUSE/UI/tests && FUSE_UI_UPDATE_GOLDEN=1 ../../../../build-ui/tests/fuse_ui_cpu_raster_golden`.

Options: `FUSE_BUILD_UI_WIDGETS`, `FUSE_BUILD_UI_DOC`, `FUSE_BUILD_UI_SCRIPT`, `FUSE_BUILD_UI_VK`, `FUSE_BUILD_UI_TESTS`,
`FUSE_UI_TEXT` (`MINIMAL` default; `FULL` fetches HarfBuzz 10.1.0 + FreeType 2.13.3 at configure time, so it needs
network or a populated `FETCHCONTENT_BASE_DIR`; `COOKED` falls back to MINIMAL), `FUSE_UI_ACCESSIBILITY`,
`FUSE_UI_PROFILE`, `FUSE_BUILD_UI_APP`, `FUSE_BUILD_UI_GAME` (both ON by default); `FUSE_BUILD_APP` (UI9) and
`FUSE_BUILD_UI_DESIGNER` are reserved. The HUD golden is regenerated the same way as the raster golden with
`fuse_ui_game_hud_golden` (`tests/golden/hud.png`, `hud_full.png` in FULL).

FULL text: `cmake -S Source/FUSE/UI -B build-ui-full -G Ninja -DFUSE_UI_TEXT=FULL -DCMAKE_BUILD_TYPE=Release`. It has
its own raster golden (`tests/golden/sample_full.png`). GPU parity runs on any Vulkan 1.2 device; for software use
`VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json` (lavapipe).

## Retained use

```cpp
#include <fuse/ui/ui.hpp>
#include <fuse/ui/widgets/widgets.hpp>
using namespace fuse::ui;

auto font = fuse::text::Font::bundled().value();
UiContext ui({.viewport = {800, 600}, .font = &font});
widgets::install(ui);                                  // registers classes + default theme
ui.addStyleSheet(".card { padding: 16px; } :root { --accent: #4c8dff; }");

NodeHandle card = widgets::panel(ui, ui.root(), "card");
ui.addClass(card, "card");
NodeHandle ok = widgets::button(ui, card, "Apply", "apply");
ui.connect(ok, SignalKind::Clicked, [](UiContext&, NodeHandle, const SignalArgs&) { /* ... */ });

// per frame
ui.pointerMove({x, y}); ui.pointerDown({x, y}); ui.keyDown(Key::Tab); ui.textInput("é");
ui.update(dt);                                         // commands → timers → anims → style → layout → paint
if (ui.drawList().size()) { /* hand ui.drawList() to a backend, or CpuRasterizer for tests */ }
```

From another thread: `ui.post(fn, node, a, b)` (bounded queue, applied at the next `update()`).

### Platform input (UI1)

```cpp
#include <fuse/ui/platform_bridge.hpp>
fuse::ui::usePlatformClipboard(ui);                    // Ctrl+C/X/V in line edits use the OS clipboard
PlatformEvent ev;
while (pump.pollEvent(ev)) fuse::ui::feed(ui, ev, pump, dpiScale);   // keys, text, IME pre-edit, wheel, pointer
fuse::ui::syncTextInput(ui, window, dpiScale);         // starts/stops IME and places the candidate window
```

### Tier B widgets

Popups live in an overlay layer (`widgets::overlayLayer`), the root's last child, outside layout. Each popup is a
viewport-sized scope (the scrim) that traps focus; outside click or Esc closes it and focus returns to the owner.

```cpp
NodeHandle bar = widgets::menuBar(ui, ui.root());
NodeHandle file = widgets::menuBarMenu(ui, bar, "File", "New\nOpen\n-\nQuit");
ui.connect(file, SignalKind::Submitted, [](UiContext&, NodeHandle, const SignalArgs& a) { /* a.value = index, a.text */ });
NodeHandle list = widgets::listView(ui, parent, items);           // virtualised: paints visible rows only
NodeHandle tree = widgets::treeView(ui, parent, "Scene\n\tCamera\n\tLights");  // tabs = depth
NodeHandle tabs = widgets::tabWidget(ui, parent, "General\nRender");
widgets::label(ui, widgets::tabPage(ui, tabs, 0), "...");
NodeHandle dlg = widgets::dialog(ui, "Delete asset?");
widgets::button(ui, widgets::dialogBody(ui, dlg), "Delete");
widgets::openDialog(ui, dlg);                                     // modal; Esc or closeDialog hides it
widgets::setTooltip(ui, someButton, "Explains the button");
```

With Tier B installed (`widgets::install` does both tiers), a combo box opens a popup list instead of cycling.

### Documents and Lua

`.fuseui` files are JSON trees with `{expr}` and `{=two-way}` bindings (see `Doc/include/fuse/ui/doc/document.hpp` for
the format and `tests/data/settings.fuseui` for an example); `fuse_ui_script` exposes the same context and model to Lua
as a global `ui` table.

## Application shell (UI5)

```cpp
#include <fuse/app/app.hpp>
#include <fuse/app/native.hpp>
fuse::app::Application app{{.name = "Hello", .argc = argc, .argv = argv}};
fuse::app::NativePlatform native(app);                      // declare after the Application; installs the OS event source
auto& w = app.createWindow({.title = "Hello", .width = 800, .height = 600});
if (auto r = w.load("ui/main.fuseui", &model); !r) return 1;
native.show(w);                                             // X11 / Win32 window + Vulkan swapchain (GDI on Win32 without Vulkan)
return app.run();                                           // sleeps until input, a timer or an animation needs a frame
```

Without a native platform the app runs headless (tests, `--screenshot=`). `PresenterKind` reports what was picked
(`Vulkan` draws the draw list on the GPU with `UiVkRenderer`, or uploads CPU-rasterised damage with
`VkDrawMode::CpuUpload`; `Software` blits). `NativePlatform::capture` reads back what the OS shows (used by `fuse_app_native_window`).
Under Xvfb set `VK_ICD_FILENAMES` to lavapipe; under Wine the Win32 path and the Vulkan presenter both run.

## Game UI (UI7)

```cpp
#include <fuse/ui/game/game_ui.hpp>
namespace g = fuse::ui::game;
widgets::install(ui); g::install(ui);
ui.setCss(ui.root(), "layout", "anchor");                  // game screens position by anchors
g::GameUi hud(ui);                                          // HUD layer: safe area, kill feed, subtitles, dialogue, pools
hud.setSafeArea({48, 27, 48, 27});
NodeHandle hp = g::hudBar(ui, hud.safeArea(), 1.f, "hp");  // damage trail drains in game time
g::setHudBar(ui, hp, 0.55f);
hud.setCamera(viewProj, eye);  u32 m = hud.addMarker({8, 2, -30}, "Beacon");  // edge-clamped, distance-faded
hud.floatText({x, y}, "-42", Color::rgb(0xFF5C5C));  hud.feed("Odin > Raider");  hud.say("Mira", "Keep low.");
hud.clock().paused = true;                                  // game-time HUD freezes; menus and subtitles keep running
// per frame
hud.update(realDt); nav.update(padState, realDt); ui.update(realDt);
```

- `GamepadNav` maps d-pad / stick to spatial `navigate()` with a 0.4 s delay and 0.12 s repeat, A to Enter and B to
  Escape. `InputArbiter` decides per device whether the UI or the game gets input (interactive HUD nodes and
  `.ui-blocker` panels take the pointer, a held press stays with the UI, focused text fields take the keyboard, menu
  mode takes everything). Device prompts (`GameUi::prompt`) swap glyphs on `noteDevice`.
- `UiSurface` is a UI context placed in the world (centre, right / up axes, size). `surfaceRaycast` maps a ray to
  panel pixels, `surfacePointer` feeds it as pointer input and `surfaceRender` re-rasterises on damage.
- `StringTable` holds per-locale `key=value` tables; bound nodes are re-texted on `setLocale` + `refresh`.
- The sound hook (`setSoundHook`) receives Move / Accept / Back / Open / Close; wiring it to `fuse_audio` and theme
  sound names is the game's job for now.

## Data and editor widgets (UI6 Tier C/D)

- `setTableDelegate(table, col, CellDelegate)` paints a column with your own callback; `SortFilterProxy` wraps an `ItemModel` with a filter predicate and a sort column (header clicks sort).
- `gridView(parent, model, cellW, cellH)` is a virtualised icon grid; `gridColumns`, `gridFirstVisibleRow`.
- `propertyGrid(parent, std::span<const PropertyDesc>)` builds grouped rows (bool, int, float, string, enum, colour); read with `propertyValue`, collapse with `setPropertyGroupOpen`.
- `colorPicker`, `colorPickerColor`, `setColorPickerColor`; `calendar`, `calendarDate` (`daysFromCivil`/`civilFromDays` helpers); `chart(parent, ChartKind)` + `chartSetSeries`; `markdownView` + `setMarkdown` (headings, paragraphs, lists, code, quotes).
- Tier D editor widgets: `fontPicker`, `fileDialog`, `nodeGraph`, `curveEditor`, `timeline`, `viewportWidget` (forwards input to your callback, paints your image), `codeEdit` (line numbers, highlighting), `dateTimeEdit` (`value()` = days since 1970, `dateTimeEditMinutes`), `videoWidget` (frame-source stub).
- `fuse_ui_sql::SqliteModel`: `open`/`attach`, `setQuery`, `sortBy`, `refresh`; pass `model()` to `tableView`. Only the visible window of rows is fetched.
- Platform feel (`platform_feel.hpp`): `setHapticsHandler`, `ScrollProfile::desktop()/ios()/android()`, `kineticScrollArea` (drag/fling with rubber band; a drag never clicks).
- Accessibility (`fuse/ui/a11y/a11y.hpp`): `Bridge::sync` diffs `UiContext::snapshotA11y`, `perform`/`enqueue`/`drain` run assistive-tech actions on the UI thread. AccessKit needs a Rust toolchain and a prebuilt accesskit-c (tested with 0.22.3); without it the bridge still builds.
- Designer: `fuse_ui_designer [file.fuseui] [--save=out] [--screenshot=png]`. `DesignDocument` saves exactly what it loaded; keys starting with `x-` are kept for tools and stripped from the live preview.
- `dockSaveLayout(dock)` returns a text layout (`fusedock 1`); `dockRestoreLayout(dock, text)` checks the whole text before applying it and ignores unknown panel ids.

## Styling (CSS subset, D4)

Selectors: type, `.class`, `#id`, `:hover :pressed :focus :focus-visible :checked :disabled`, descendant and `>`
combinators. Tokens are `--name` in `:root` and used with `var(--name[, fallback])`; they are global (scoped tokens
are deferred). Inline (`setCss`) beats everything; later sheets win ties. Layout properties: `layout`
(box/grid/stack/anchor/absolute/flow), `flex-direction`, `flex-wrap`, `gap`, `padding*`, `margin*`,
`width/height/min-*/max-*`, `flex-grow/shrink/basis`, `align-items/self`, `justify-content`,
`grid-template-columns`, `grid-row/column(-span)`, `left/top/right/bottom/center-x/center-y`, `overflow`
(visible/clip/scroll), `display`, `pointer-events`. Paint: `background` (colour or `linear-gradient`),
`background-end` (gradient end; widgets use it as the accent), `border`, `border-width/color`, `border-*-radius`,
`box-shadow`, `shadow-x/y/blur/color`, `opacity`, `color`, `font-size`, `text-align`. Selectors also accept
`:active`, `:selected`.

## Immediate mode

`fuse::ui::immediate::ImmediateUi` has the same API and layout rules as Relight's `overlay::Ui` (`begin`, `text`,
`button`, `textField`, `sameLine`, …) but draws real glyphs into a FUSE UI draw list. `fuse_ui_relight_equivalence`
checks it against the overlay under one scripted event stream.

## Gates

| Gate | Checks |
|---|---|
| `fuse_ui_layout_conformance` | every layout model, scrolling, `display:none`, measure cache and layout boundaries |
| `fuse_ui_style_cascade` | specificity, order, inline, inheritance, pseudo-classes, combinators, tokens, diagnostics |
| `fuse_ui_event_routing` | capture/target/bubble, stop, click semantics, pointer capture, hover, disabled, modal, `post()` |
| `fuse_ui_focus_order` | tab order, tab index, skipping, wrap, focus-visible, keyboard activation, spatial nav, modal trap |
| `fuse_ui_cpu_raster_golden` | rasteriser primitives and the sample tree against `tests/golden/sample.png` (`sample_full.png` in FULL) |
| `fuse_ui_zero_alloc_steady` | zero heap allocations over 50 interactive frames after warm-up |
| `fuse_ui_bench_10k` | 10k nodes, one dirty label: ≤ 0.5 ms median (Release; 1.0 ms in FULL) |
| `fuse_ui_single_thread_parity` | direct vs posted-from-threads mutations give identical layout, draw list and pixels |
| `fuse_ui_a11y_tree_dump` | roles, names, states, values |

| `fuse_app_idle_zero_frames`, `fuse_app_multiwindow`, `fuse_app_hello_startup`, `fuse_app_settings` | UI5 loop, windows, startup ≤ 150 ms, settings / command line / single instance |
| `fuse_app_native_window` | real X11 / Win32 window, Vulkan GPU-draw and CPU-upload presenters vs the CPU raster (skips 77 without a display or device) |
| `fuse_ui_game_hud_golden` | HUD demo scene against `tests/golden/hud.png` plus trail / marker / pool state |
| `fuse_ui_gamepad_nav` | navigation with delay + repeat, A / B, radial menu by stick, prompt glyph swap, focus sounds |
| `fuse_ui_world_panel_raycast` | ray → panel pixel mapping, misses and back faces, ray clicks, idle surface does not re-render |
| `fuse_ui_input_arbitration` | pointer / keyboard / gamepad ownership between UI and game |
| `fuse_ui_pause_time_domain` | pause and slow motion affect game-time HUD only; locale switching |

Extra: `fuse_ui_text_basics`, `fuse_ui_text_full`, `fuse_ui_widget_interaction`, `fuse_ui_tier_b`, `fuse_ui_tier_b2`,
`fuse_ui_relight_equivalence`, `fuse_ui_platform_keys_v2`, `fuse_ui_text_input`, `fuse_ui_doc_load`,
`fuse_ui_lua_bindings`, `fuse_ui_vk_parity`, the sample runs `fuse_app_hello_sample`, `fuse_app_notes_sample`,
`fuse_ui_game_hud_demo`, `fuse_ui_game_world_panel_demo`; in tree also `fuse_core_ui1_text_input`,
`fuse_scene_renderer_vk_fuse_ui_t0` and `rl_overlay_glyphs`.
