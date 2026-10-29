# FUSE UI

FUSE UI is the engine's own GUI and application framework. The full plan is in [`../plans/FUSE_UI_PLAN.md`](../plans/FUSE_UI_PLAN.md).

## What is in tree (UI2 slice)

`Source/FUSE/UI/` builds `fuse_ui_core` (CMake option `FUSE_BUILD_UI`, ON by default). It is headless: no Vulkan and no Qt.

| Piece | Header | Notes |
|---|---|---|
| Retained tree | `fuse/ui/context.hpp` | `UiContext`, generation-checked `NodeId`s, pooled nodes, `WidgetClass` function tables |
| Style | `context.hpp` (`StyleSheet`) | QSS-like subset: `Type.class#name:state { ... }`, selector lists, specificity, per-node overrides, built-in dark theme |
| Layout | `context.hpp` (`LayoutProps`) | Column / Row (gap, padding, grow, align, justify), Stack, Absolute, anchors with margins, vertical scrolling with clipping and culling |
| Events | `context.hpp` | Hit testing, capture → target → bubble with event filters, pointer capture, hover, modal scopes, consumed / pass-through result for game input |
| Focus | `context.hpp` | Tab / Shift+Tab order, skips disabled, stays inside modal scopes, `:focus` styling, shortcuts |
| Threads, time | `context.hpp` | `post()` from any thread, drained at `update()`; timers; value and colour tweens |
| Widgets | `src/widgets.cpp` | Panel, Label, Button, CheckBox, Slider, LineEdit, ScrollView, Spacer, ProgressBar |
| Immediate mode | `fuse/ui/immediate.hpp` | `im::ImmediateUi` (`label`, `button`, `checkbox`, `slider`, `inputText`, `progress`) reconciled onto retained nodes by id (`"label##id"`) |
| Drawing | `fuse/ui/draw_list.hpp`, `fuse/ui/cpu_raster.hpp` | Backend-neutral draw list; CPU reference rasteriser (rounded rects with analytic AA, strokes, clips, damage rects, text) |
| Testing | `fuse/ui/test_driver.hpp` | `UiTestDriver` (click / drag / wheel / key / type by widget name), `dumpTree()`, `dumpAccessibility()` |

`update()` runs only the dirty passes. An idle frame does no work and allocates nothing. A paint-only change, such as a slider value, re-records the draw list without layout, and its damage rect covers only that widget.

## Example

```cpp
#include <fuse/ui/context.hpp>
#include <fuse/ui/immediate.hpp>

fuse::ui::UiContext ui;
ui.setViewport({640, 400});
auto form = ui.create("Panel", ui.root(), "form");
ui.layout(form).gap = 8;
auto name = ui.create("LineEdit", form, "name");
auto ok = ui.create("Button", form, "ok");
ui.setText(ok, "OK");
ui.onClicked(ok, [&](fuse::ui::UiContext& c, fuse::ui::NodeId) { submit(c.text(name)); });

fuse::ui::im::ImmediateUi debug(ui);   // same core, immediate API
debug.begin();
if (debug.button("Reload shaders")) reload();
debug.slider("Exposure", exposure, -4.f, 4.f);
debug.end();

ui.update(dt);                        // style → layout → paint, only what is dirty
// ui.drawList() → fuse::ui::rasterize(...) today; fuse_ui_vk (UI4) later
```

The sample `Source/FUSE/UI/samples/hello_app.cpp` (`fuse_ui_hello_app out.png`) builds a form and an immediate-mode panel, drives them with scripted input and writes a PNG.

## Gates

`fuse_ui_core_tests <gate>` provides these ctests (label `ui`):

- `fuse_ui_layout_conformance`
- `fuse_ui_style_cascade`
- `fuse_ui_event_routing`
- `fuse_ui_focus_order`
- `fuse_ui_cpu_raster_golden`
- `fuse_ui_zero_alloc_steady`
- `fuse_ui_bench_10k`
- `fuse_ui_a11y_tree_dump`
- `fuse_ui_immediate`
- `fuse_ui_timers_commands`

## Not yet (by phase)

- **UI1:** platform v2 (IME, clipboard, multi-window).
- **UI2 remainder:**
  - Grid and flow layouts.
  - Measure caching and layout boundaries.
  - Descendant selectors and transitions in style sheets.
  - The property registry and `Signal<>` with automatic disconnect.
  - Gamepad spatial navigation.
  - Porting Relight's `overlay::Ui` onto the immediate front-end.
- **UI3:** `fuse_text`. The built-in 5x7 bitmap font stands in until it lands, and LineEdit is ASCII only.
- **UI4:** `fuse_ui_vk` GPU backend and `FrameUiSource` integration.
- **UI5:** `fuse_app`.
- **UI6:** widget tiers.
- **UI8:** `.fuseui` / `.fusestyle` files and Lua.
