# FUSE UI

FUSE's own GUI framework ([plan](../plans/FUSE_UI_PLAN.md)). One retained core, styled with a CSS subset, drawn
through a backend-neutral draw list, plus an immediate-mode front-end on the same primitives. Engine rules apply:
handles, not pointers; `std::expected`; one owner thread (other threads `post()`); no steady-state allocation.

Status (2026-10-29): headless core (UI2) with all nine gates green, `.fuseui` documents and Lua bindings, MINIMAL
text plus an opt-in FULL profile (HarfBuzz + FreeType), Tier A and Tier B widgets (UI6), a Vulkan compute backend
for the draw list (UI4), and desktop text input / IME / wheel / clipboard on Win32 and X11 (UI1). Relight's dev menu
runs on the immediate-mode front-end. See the plan's status table for what is still open.

## Layout of the code

| Path | Target | What |
|---|---|---|
| `Source/FUSE/UI/Text` | `fuse_text` | Fonts (stb_truetype; FreeType in FULL), bundled Open Sans, UTF-8, `ITextShaper` (`MinimalShaper`, `HarfBuzzShaper` in FULL), wrap/ellipsis layout, glyph atlas, `TextEditBuffer` |
| `Source/FUSE/UI/Core` | `fuse_ui_core` | `UiContext` (tree, style, layout, paint, input, focus, signals, timers, animation, a11y), `DrawList` + `CpuRasterizer`, `StyleEngine`, `immediate::ImmediateUi`, `platform_bridge.hpp` |
| `Source/FUSE/UI/Widgets` | `fuse_ui_widgets` | Tier A + Tier B widgets + default dark theme (`widgets::install`) |
| `Source/FUSE/UI/Doc` | `fuse_ui_doc` | `.fuseui` JSON documents, binding model, expression bytecode |
| `Source/FUSE/UI/Script` | `fuse_ui_script` | Lua `ui` table (vendored Lua 5.2) |
| `Source/FUSE/UI/Render` | `fuse_ui_render`, `fuse_ui_vk` | GPU draw data packing, `ui.draw_list` compute kernel, `UiLayerGpu` render-graph pass (in tree with the Vulkan RHI) |
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
`FUSE_UI_PROFILE`; `FUSE_BUILD_UI_GAME`, `FUSE_BUILD_APP`, `FUSE_BUILD_UI_DESIGNER` are reserved.

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

Extra: `fuse_ui_text_basics`, `fuse_ui_text_full`, `fuse_ui_widget_interaction`, `fuse_ui_tier_b`,
`fuse_ui_relight_equivalence`, `fuse_ui_platform_keys_v2`, `fuse_ui_text_input`, `fuse_ui_doc_load`,
`fuse_ui_lua_bindings`, `fuse_ui_vk_parity`; in tree also `fuse_core_ui1_text_input` and `rl_overlay_glyphs`.
