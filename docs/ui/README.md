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
| `Source/FUSE/UI/Text` | `fuse_text` | Fonts (stb_truetype; FreeType in FULL), bundled Open Sans, UTF-8, `ITextShaper` (`MinimalShaper`, `HarfBuzzShaper` in FULL), wrap/ellipsis layout, glyph atlas, `TextEditBuffer`, UAX #29 / #14 / #9 segmentation, fallback chains, `FontCatalog` (system fonts), variable fonts (fvar / avar / gvar / HVAR / MVAR), OpenType feature variants |
| `Source/FUSE/UI/Core` | `fuse_ui_core` | `UiContext` (tree, style, layout, paint, input, focus, signals, timers, animation, a11y), `DrawList` + `CpuRasterizer`, `StyleEngine`, `immediate::ImmediateUi`, `platform_bridge.hpp` |
| `Source/FUSE/UI/Widgets` | `fuse_ui_widgets` | Tier A + Tier B widgets + default dark theme (`widgets::install`) |
| `Source/FUSE/UI/Doc` | `fuse_ui_doc`, `fuse_ui_cook`, `fuse_ui_extract_strings` | `.fuseui` JSON documents, binding model, expression bytecode; the cooker and the string-extraction CLI |
| `Source/FUSE/UI/L10n` | `fuse_ui_l10n_lib` | CLDR plural rules, ICU-style message formatting, locale number / date formatting, string-table file format (standard library only, no ICU) |
| `Source/FUSE/UI/Script` | `fuse_ui_script` | Lua `ui` table (vendored Lua 5.2) |
| `Source/FUSE/UI/Render` | `fuse_ui_render`, `fuse_ui_vk` | GPU draw data packing, `ui.draw_list` compute kernel, `UiLayerGpu` render-graph pass and `UiFrameSource` (in tree with the Vulkan RHI) |
| `Source/FUSE/UI/App` | `fuse_app`, `fuse_app_native`, `fuse_app_a11y` | `Application` / `AppWindow` on-demand loop, settings, command line, single instance; native X11 / Win32 / Android (NativeActivity) windows and the Vulkan swapchain presenter; custom window chrome; tray icon / notifications; `WindowAccessibility` (screen readers on a native window); samples `hello_app`, `notes_app`, `game_menu`, `fuse_ui_android_demo`; `App/android/` APK packer and adb gate driver |
| `Source/FUSE/UI/Game` | `fuse_ui_game` | HUD widgets, world markers, `UiSurface` world panels, gamepad navigation, input arbitration, prompts, time domains, locale table, subtitles, dialogue; samples `hud_demo`, `world_panel_demo` |
| `Source/FUSE/UI/Sql` | `fuse_ui_sql` | `SqliteModel`: an `ItemModel` over a SQLite query (option `FUSE_BUILD_UI_SQL`) |
| `Source/FUSE/UI/A11y` | `fuse_ui_a11y` | Accessibility bridge (snapshot diff, action queue); AccessKit adapters (`UnixAdapter` AT-SPI, `WindowsAdapter` UI Automation) when AccessKit is linked |
| `Source/FUSE/UI/Designer` | `fuse_ui_designer_lib`, `fuse_ui_designer` | `.fuseui` designer prototype (UI8) |
| `Source/FUSE/UI/Icons` | `fuse_ui_icon_cook_lib`, `fuse_ui_icon_cook` | SVG -> `.fuseicons` cooker (vendored nanosvg); the loader `IconSet` is in `fuse_ui_core` (`fuse/ui/icons.hpp`) |
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
network or a populated `FETCHCONTENT_BASE_DIR`; `COOKED` loads only `.fusefont` packs, the bundled fonts cooked at build time), `FUSE_UI_ACCESSIBILITY`
(`FUSE_UI_ACCESSKIT_DIR` = prebuilt accesskit-c prefix, or `FUSE_UI_ACCESSKIT_BUILD=ON` to fetch accesskit-c
`FUSE_UI_ACCESSKIT_TAG` (0.22.3) and build it with cargo at build time; needs Rust >= 1.85 and network),
`FUSE_UI_PROFILE`, `FUSE_BUILD_UI_APP`, `FUSE_BUILD_UI_GAME` (both ON by default); `FUSE_BUILD_APP` (UI9) and
`FUSE_BUILD_UI_DESIGNER` are reserved. The HUD golden is regenerated the same way as the raster golden with
`fuse_ui_game_hud_golden` (`tests/golden/hud.png`, `hud_full.png` in FULL).

FULL text: `cmake -S Source/FUSE/UI -B build-ui-full -G Ninja -DFUSE_UI_TEXT=FULL -DCMAKE_BUILD_TYPE=Release`. It has
its own raster golden (`tests/golden/sample_full.png`). GPU parity runs on any Vulkan 1.2 device; for software use
`VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json` (lavapipe). The kernel is bit-exact with the CPU raster on
hardware too: it rounds division / sqrt to the IEEE result (Vulkan allows 2.5 ulp), including backdrop blur.
FULL builds natively with MSVC as well.

GPU backends (`UiVkDesc`): the compute tile kernel (default, bit-exact) or SDF quads — `useSdfQuads(desc)` selects
an instanced quad pipeline (vertex + fragment, the same coverage arithmetic, fixed-function premultiplied-over
blending) drawing each draw-list segment in a render pass on the same GENERAL-layout image (it then needs
`VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT`); backdrops and effects still run as compute passes between the draws. It is
within ±2 / 255 of the reference (hardware blending rounds translucent overs differently) and on an RTX 3090
1.3–3× faster (`fuse_ui_vk_sdf_quads`, `fuse_ui_baselines`). Compute stays the default: exact on every device (the
parity / golden gates rely on it), no attachment usage, and every frame is already far inside the §6 budgets.

### Custom shader effects

An effect post-processes a node's rect (or any draw-list rect) after everything under it is drawn: a GLSL compute
snippet plus a C++ twin with the same arithmetic (`fuse/ui/effects.hpp`). Built-ins: `desaturate(amount, r, g, b)`,
`ripple(amplitude, wavelength, speed)` (animated), `grain(amount, fps)` (animated).

```css
.card.disabled { effect: desaturate(0.8); }
.water { effect: ripple(6px, 24, 40); }        /* missing parameters take the registered defaults */
```

```cpp
fuse::ui::EffectRegistry fx;
fx.addBuiltins();
fx.add({.name = "vignette", .cpu = &vignetteCpu, .spirv = kMyVignette, .defaults = {2.f}, .exact = false});
ui.setEffects(&fx);           // style hookup; animated effects repaint every update (ui.elapsed() is their time)
renderer.setEffects(fx);      // fuse_ui_vk: one compute pipeline per effect
cpuRaster.setEffects(&fx);    // the reference path
dl.effect(rect, id, params, time, margin, radii, imageIndex);   // draw lists directly
```

The snippet defines `vec4 fx_main(ivec2 p)` (premultiplied RGBA, 0..255) and reads `fx_src(p)` (the target before
the effect, edge-clamped to the source region), `fx_param(i)`, `fx_time`, `fx_rect`, `fx_imageTexel` /
`fx_imageSize` (an optional image or external texture), with CPU-identical helpers `divRn`, `sqrtRn`, `fx_sin`,
`fx_cos`, `fx_hash`, `fx_hash01`; `fuse_ui_add_effect(<target> NAME kMyVignette SOURCE vignette.glsl)`
(`Render/cmake/fuse_ui_effects.cmake`) splices it into `Render/src/shaders/ui_effect.comp.in` and embeds the SPIR-V.
Effects made of add / mul / floor / `divRn` / `sqrtRn` and the `fx_*` helpers are bit-exact on the GPU; ones that
call native `exp` / `pow` / `sin` register `exact = false` (compared within ±2). An Effect command ends a GPU segment
like a backdrop: the source region (rect + margin) is copied to the backdrop scratch and the effect pipeline runs over
the rect, masked by its rounded corners and the clip. `DrawList::redrawRegion` grows damage over an effect's source
region.

Text (`fuse/text/text.hpp`):

```cpp
using namespace fuse::text;
auto font = Font::bundled().value();
auto chain = Font::bundledFallbacks();              // Hebrew, Arabic, Devanagari, Twemoji, Symbols 2, Thai, CJK subset
std::vector<const Font*> ptrs;
for (const Font& f : chain) ptrs.push_back(&f);
font.setFallbacks(ptrs);

const FontCatalog& fonts = FontCatalog::system();  // installed fonts, scanned once (no fontconfig)
if (const FontFaceInfo* f = fonts.match("Segoe UI", 600)) auto semibold = FontCatalog::load(*f);
auto han = fonts.systemFallbackChain(Script::Han); // e.g. Microsoft YaHei UI, Yu Gothic UI, ...

std::vector<LineBreak> breaks;
lineBreaks(utf8, breaks);                           // UAX #14 (FULL), spaces / newlines (MINIMAL)
std::vector<std::uint8_t> levels; std::uint8_t para;
bidiLevels(utf8, BaseDirection::Auto, levels, para); // UAX #9 levels after L1 (FULL)
```

Grapheme tables come from the UCD: `python Source/FUSE/UI/Text/tools/gen_unicode_tables.py <ucd dir>` rewrites
`Text/src/unicode_tables.inc`. The CJK subset covers kana, CJK punctuation, fullwidth forms, ~1400 frequent Han
characters and Hangul (the 2350 KS X 1001 syllables + jamo; `vendor/fonts/noto/make_cjk_subset.py`).

Colour glyphs: COLRv0 layers always. `enableColorGlyphs()` (call it before loading bitmap fonts) adds exact COLRv1
(linear / radial / sweep gradients, every transform, clip boxes, all composite modes, rendered to RGBA when a glyph is
first drawn) and CBDT / sbix bitmap strikes (best strike for the size, scaled); without it COLRv1 is flattened to solid
layers and bitmap-only glyphs draw nothing. These are `ColorLayer::image` layers: `GlyphAtlas::getColor` puts them in
the atlas colour page (premultiplied RGBA, side `size() / 2`, after the coverage page in `pixels()`) and the UI draws
them as `kGlyphColor` glyph commands. The rasterisers are linked only into binaries that enable them.

Variable fonts: `enableVariableFonts()` (opt-in like the colour rasterisers; without it a variable font draws its
default instance) gives `Font::axes()`, `namedInstances()`, `instance({{fontTag("wght"), 650}})`, `namedInstance(i)`
and `setVariation(...)`. An instance is another `Font` over the same bytes (owned by the font that loaded them, cached by
normalised coordinates, found without allocation): fvar + avar normalisation, HVAR advances (else the gvar phantom
points), MVAR ascender / descender / line gap, gvar outlines (MINIMAL: FUSE's own glyf + gvar decoder with IUP and
composites, rasterised exactly; FULL: FreeType at `FT_Set_Var_Blend_Coordinates`, shaped by HarfBuzz at the same
normalised coordinates, sharing the run cache) and COLRv1 `Var*` paints from COLR's item variation store. CFF2 outlines
vary only in FULL. `FontCatalog` lists each named instance of a variable face as its own face, named like DirectWrite
(STAT values other than weight / width / slant join the family: "Segoe UI Variable Display" + "Semibold", weight 600);
`FontCatalog::load` pins it. In the UI a variable font follows `font-weight`, `font-stretch` (wdth),
`font-variation-settings: "wght" 650, "opsz" 12` (over the first two) and font-size (opsz, CSS `font-optical-sizing:
auto`); `UiContext::animate(n, AnimProp::FontWeight, 700, 0.2f)` tweens the weight. COOKED packs are cooked at one
instance: `fuse_text_cook --variation=wght:600,wdth:80` or `--instance="SemiBold"`.

OpenType features: `font.withFeatures({{fontTag("tnum")}, {fontTag("liga"), 0}})` (CSS `font-feature-settings:
"tnum", "liga" off, "ss01" 2`) is a variant of the font that HarfBuzz shapes with those features (FULL; MINIMAL /
COOKED return the font itself). Variants are cached per feature list and key the run cache. `UiContext::setFont(node,
&font)` draws one node's text in another face (the font picker previews every family in its own face).

MSDF: `ui.setMsdfThreshold(48)` draws text of at least 48 device px from multi-channel signed distance fields (one per
glyph at `GlyphAtlas::kMsdfPixelSize`, range `kMsdfRange`, `kGlyphMsdf` commands stretched to any size; for zoomed and
world-space UI). The CPU rasteriser, the compute kernel and the SDF-quad shader sample them identically.

COOKED profile (`-DFUSE_UI_TEXT=COOKED`): fonts are `.fusefont` packs and nothing is parsed at run time (no
stb_truetype in the binary). `fuse_text_cook --out=x.fusefont --sizes=8-40,17.5 [--color-sizes=..] [--msdf]
[--chars=20-7E,A0-17F] font.ttf` (or `fuse::text::cookFont`, `fuse/text/font_cook.hpp`) stores the cmap subset,
advances, kerning pairs, COLR layers and glyphs pre-rasterised by the MINIMAL code at the listed sizes (byte-identical
to MINIMAL there), colour images, and optionally MSDFs; other sizes come from the MSDF (else the nearest strike,
resampled). Packs are memory-mapped (`Font::fromFile`). The build cooks the bundled fonts into `<build>/fonts`; shaping
is MINIMAL's (cmap + kerning), so complex scripts and bidi behave as in MINIMAL.

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

### Keyboard model

Every interactive widget works without a pointer (gate `fuse_ui_keyboard_only`). Keys go to the focused node first;
what it does not take falls through to the core.

- **Focus**: Tab / Shift+Tab walk the tab order of the current scope (wrapping); Ctrl+Tab / Ctrl+Shift+Tab always move
  focus, also out of widgets that take Tab (the code editor indents on Tab). Keyboard focus sets `:focus-visible`, which
  the default themes (dark and light) show as an accent ring or highlight on every focusable widget, and scrolls the
  node into view inside scroll containers. Tooltips also show on keyboard focus.
- **Activation**: Enter / Space click buttons, tool buttons, links, check boxes, switches, menu titles and items.
- **Disabled** widgets refuse focus: Tab, radio arrows and menu bar arrows skip them.
- **Choices and ranges**: radio buttons move and check with the arrows (wrapping within the siblings); sliders, dials,
  spin boxes and scroll bars take the arrows, PageUp / PageDown (a page) and Home / End (the ends).
- **Combo box**: Up / Down / Home / End pick in place; Alt+Down, F4, Space or Enter open the list, arrows + Enter
  pick, Escape closes it. The date-time edit opens its calendar the same way (Left / Right pick the field, Up / Down
  step it, digits type it).
- **Menus**: F10 (when no widget takes it) moves focus to the first menu bar and back; in the bar Left / Right / Home /
  End move between titles, Down / Enter / Space open; in a menu Up / Down / Home / End move, Enter picks, Right / Left
  open / close submenus or switch to the neighbouring bar menu, Escape closes (and in the bar returns to where F10
  came from). Unconsumed keys reach the root's `SignalKind::Event` signal for app-wide shortcuts.
- **Views**: list, tree, table and grid views take arrows, Home / End, PageUp / PageDown and Enter (Submitted); tree
  views Left / Right collapse / expand; tab bars Left / Right / Home / End.
- **Containers**: splitter handles are focusable (arrows resize by 16 px); a dock title closes its dock on Enter,
  Space or Delete; dialogs trap focus and Escape closes them (a file dialog reports Submitted with an empty path).
- **Pro widgets**: colour picker arrows (saturation / value), Shift+Left / Right (alpha), Page keys (hue); calendar
  arrows (day / week), Page keys (month), Home / End (month start / end); chart Left / Right / Home / End step the
  readout; node graph PageUp / PageDown select a node, arrows move it (Shift: fine) or pan with none selected, Delete
  removes, Escape deselects; curve editor PageUp / PageDown / Home / End select a key, arrows move it (Shift: x10),
  Delete removes; timeline arrows (a frame), Page keys (a second), Home / End; video Space plays / pauses, Left /
  Right step a frame, Home / End; the viewport forwards keys but not Tab.

### Documents and Lua

`.fuseui` files are JSON trees with `{expr}` and `{=two-way}` bindings (see `Doc/include/fuse/ui/doc/document.hpp` for
the format and `tests/data/settings.fuseui` for an example); `fuse_ui_script` exposes the same context and model to Lua
as a global `ui` table.

### Icons

Vector icons are cooked, not parsed at run time. `fuse_ui_icon_cook --out=x.fuseicons [--sizes=16,24,...]
<svg files or dirs>` rasterises an SVG subset (paths incl. arcs, rect / circle / ellipse / line / polyline / polygon,
`g` transforms, fill / stroke / stroke-width / opacity, fill-rule, currentColor, viewBox) into a coverage atlas at
16 / 20 / 24 / 32 / 48 px by default plus a name index; output is deterministic. The build cooks the starter set
(43 Lucide icons, `vendor/icons/lucide`, ISC) into `<build>/icons/fuse_ui_icons.fuseicons` (target `fuse_ui_icons`);
`fuse_ui_cook --icons=<pack> doc.fuseui <svg dir>` cooks a pack along with the documents and fails when a document
names an icon the pack lacks; the pack is only written when nothing failed (an existing pack stays untouched).

```cpp
auto icons = fuse::ui::IconSet::fromFile("fuse_ui_icons.fuseicons");
ui.setIcons(&*icons);                                        // non-owning
w::icon(ui, row, "search", 24.f);                            // an icon by itself
w::setIcon(ui, w::toolButton(ui, bar, "", "save"), "save");  // any widget: icon before its text
w::popupMenu(ui, owner, ":file-plus: New\n:x: Close", at);   // menu items: a leading ":name: "
```

In style sheets and markup: `icon: save; icon-size: 20px;` (`none` clears; the size defaults to font-size + 2) and the
`.fuseui` key `"icon": "save"`. Icons are tinted with the node's `color` (SVG currentColor), so they follow theme,
`:hover` and `:disabled` colours. They are copied into the context's glyph atlas on first use and drawn as `Glyph`
commands, so the CPU rasteriser and the GPU kernel draw them with no new ops; sizes that were not cooked are
area-resampled from the nearest larger cooked size. MSDF is not supported.

Multi-colour icons: an SVG with colours other than currentColor / black is cooked as a layered palette (COLR-style):
its paints, in paint order, grouped into layers of consecutive equal colour; currentColor layers stay tintable, the
others keep their colour (black included; gradients become their mean stop colour). Each layer is a coverage bitmap in
the same atlas (pack format version 2; version 1 packs still load), drawn as one tinted `Glyph` per layer, so the GPU
backends need nothing new. A fixed layer's alpha follows the tint's alpha (fades); on a `:disabled` node fixed layers are
greyed to their luminance at `kDisabledIconAlpha` (0.45), so multi-colour icons dim like monochrome ones; edges shared by two layers blend
like independently anti-aliased shapes. `fuse_ui_icon_cook --list` prints each icon's layer colours.

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
Under Xvfb set `VK_ICD_FILENAMES` to lavapipe; under Wine the Win32 path and the Vulkan presenter both run. The
real-window gates start Xvfb with `+bs` (backing store): without a window manager windows open at 0,0 and overlap,
and `capture()` (XGetImage) reads a window's own pixels only where the server keeps them.

Custom window chrome (`fuse/app/chrome.hpp`): `WindowDesc::frame = WindowFrame::Custom` (resizable, OS frame removed,
snap / maximise kept) or `Borderless` (no frame, no resize edges), plus a FUSE UI title bar:

```cpp
#include <fuse/app/chrome.hpp>
auto& w = app.createWindow({.title = "Tool", .frame = fuse::app::WindowFrame::Custom, .resizeBorder = 6.f});
fuse::app::titleBar(w, w.ui().root(), "Tool");              // drag area + min / max / close (ids titlebar.*)
```

`chromeHitTest(w, p)` says what a point is (caption, edge / corner, min / max / close, client); interactive widgets
inside the caption stay clickable. Win32 answers `WM_NCHITTEST` from it (so Win11 snap layouts appear on the maximise
button) and removes the frame in `WM_NCCALCSIZE`; `AppWindow::command()` minimises / maximises / closes. X11 asks the
window manager for no decorations (`_MOTIF_WM_HINTS`), hands caption / edge presses to it with `_NET_WM_MOVERESIZE`
(same hit test, resize cursors on the edges), maximises through `_NET_WM_STATE` (also on a caption double click),
minimises with `WM_CHANGE_STATE` and follows the window's `_NET_WM_STATE` for `chrome().maximized`.

Shell services (`fuse/app/shell.hpp`, UI10): `TrayIcon` (icon from an RGBA `Image`, tooltip, `onClick` /
`onDoubleClick`, a context menu in `popupMenu` item format shown as an OS popup menu, `notify()` balloons / toasts)
and a free `notify(title, body)`. Win32: `Shell_NotifyIconW` (re-added when Explorer restarts). Unix: a
StatusNotifierItem over a runtime-loaded libdbus-1 (KDE, GNOME with the AppIndicator extension, waybar ...): icon
pixmap, tooltip, `Activate` → `onClick`, the menu exported as `com.canonical.dbusmenu` (the tray host draws it, so
`openMenu()`, double clicks and `screenRect()` do nothing there); its D-Bus connection is one of the X11 pump's watches.
`notify()` uses `org.freedesktop.Notifications` (a tray icon's notifications report clicks via `ActionInvoked`), else
returns `Unsupported`.

Samples: `hello_app` (`--startup-probe` prints launch → first presented frame), `notes_app`, `game_menu` (title,
Play / Options / Quit over an animated background on a backdrop-blurred panel, options page with resolution, volume
sliders and key bindings, animated page transitions, `GamepadNav`; `--script --screenshot=out.png` plays a simulated
gamepad session and checks it).

Fast first frame (`NativeOptions::asyncVulkan`, default on): the first window shows the CPU raster through the
software presenter (Win32 GDI, X11 `XPutImage`) immediately while a worker thread creates the Vulkan instance,
device and swapchain (X11: on its own X connection); the event pump then switches the window to Vulkan and the switch
frame redraws the whole current UI, so nothing flickers and a resize in between is followed. `presenterPending(w)` /
`waitForPresenter(w)` expose the handover; `asyncVulkan = false` makes `show()` block as before (adopted devices and
later windows of the platform always start on Vulkan directly). Measured with `fuse_app_native_startup` (RTX 3090,
Windows 11): launch → first frame ≈ 60–90 ms with the default configuration (budget 150 ms), switch to Vulkan
≈ 0.8–0.9 s after launch (ceiling 2 s), switched frame equal to the CPU raster; synchronous Vulkan init ≈ 0.85 s to
the first frame (instance + device ≈ 0.4–0.7 s, swapchain + first present ≈ 0.35 s).

Desktop platform services (UI1, `fuse/app/native.hpp`, `fuse/app/platform.hpp`):

- DPI: the process is per-monitor-v2 aware; each window takes its monitor's scale and follows `WM_DPICHANGED`
  (X11: the XSETTINGS `Xft/DPI`, followed live for every window, else `RESOURCE_MANAGER` `Xft.dpi`; `$FUSE_UI_SCALE`
  overrides; `NativePlatform::setWindowScale` forces one). `AppWindow::scale()` /
  `pixelSize()` / `onDpiChanged`; `UiContext::setScale` keeps layout and input in UI units and emits the draw list in
  device pixels with text rasterised at the new size.
- `NativePlatform::displays()` (bounds, work area, scale, refresh rate, primary). X11: RandR 1.5 monitors through a
  runtime-loaded libXrandr (refresh from the output's CRTC mode), work area from `_NET_WORKAREA`.
- Cursors: the `cursor:` style (`default text pointer ns-resize ew-resize nwse-resize nesw-resize move wait
  not-allowed crosshair`, inherited) picks the OS cursor; line edits, links and splitter handles set theirs. X11:
  themed cursors through a runtime-loaded libXcursor from XSETTINGS `Gtk/CursorThemeName` at `Gtk/CursorThemeSize`
  (else `$XCURSOR_SIZE`, else 24) times the DPI scale, re-applied when either or `Xft/DPI` changes; font cursors
  without libXcursor / a theme.
- Theme: `Application::systemTheme()` (dark, accent, high contrast, reduced motion, text scale), `onThemeChanged`,
  live on `WM_SETTINGCHANGE` (X11: XSETTINGS `Net/ThemeName`, dark when the name says so; `$FUSE_UI_THEME` /
  `$GTK_THEME` override); `setFollowSystemTheme(true)` switches the default theme dark / light
  (`widgets::themeCss(ThemeVariant)`, `UiContext::setThemeStyleSheet`) with the system accent. Reduced motion always
  makes `animate()` jump; the title bar follows via `DWMWA_USE_IMMERSIVE_DARK_MODE`. While following, the OS text
  scale multiplies every font size (`StyleEngine::setTextScale`, app sheets and explicit sizes included), and high
  contrast switches to `widgets::highContrastThemeCss(palette)` in the scheme's colours (Win32 `GetSysColor`:
  window, text, highlight, button face, gray text, hot light; `SystemTheme::contrast`) with forced colours
  (`StyleEngine::setForcedColors`, like CSS `forced-colors`: no backdrop blur, shadows, gradients or effects even from
  app sheets; every control outlined; keyboard focus gets a 2px border in the hot-light colour).
- Touch and pen (Win32 `WM_POINTER` → events_v2 `Touch*` / `Pen*` → `UiContext::touchDown/Move/Up`): the first
  finger is the pointer, every contact is in `touches()`, `pinchScale()` for two fingers (and `EventType::Pinch` to
  the node under their midpoint), `pressure()` for the pen. Ctrl+wheel carries the modifier (`Event::mods`).
- `openFileDialog` / `saveFileDialog` / `pickFolderDialog` (IFileDialog; zenity on X11), `messageBox`, `openUrl`
  (http, https, mailto, file only), `revealInFileManager`.
- Text input and IME: committed text (Win32 `WM_CHAR` with surrogate pairs; X11 `Xutf8LookupString` through XIM) and
  the IME pre-edit (`WM_IME_COMPOSITION` → `UiContext::textComposition`, drawn inline by the focused field; X11 XIM
  pre-edit callbacks when the IM offers them). The input method is on only while a text field has focus
  (`textInputActive`: Win32 associates the IMM32 context, X11 focuses the XIM input context), and the composition /
  candidate windows are placed at `UiContext::textCaret()` when a composition starts (`textInputArea`). X11 opens the
  IM with a UTF-8 `LC_CTYPE` (restored afterwards): `$XMODIFIERS` (ibus, fcitx), else libX11's compose IM (dead keys).
- Clipboard (`fuse/app/clipboard.hpp`): `setClipboardText` / `setClipboardImage` / `setClipboard({items})` with text,
  straight-RGBA images, any MIME type and lazy items (`provide` runs once, when a paste first asks); `clipboardText`,
  `clipboardImage`, `clipboardData(mime)`, `clipboardFormats()`, `clipboardOwned()`. Win32: `CF_UNICODETEXT`,
  `CF_DIBV5` (reads `CF_DIB` too), `RegisterClipboardFormat(mime)`, delayed rendering for lazy items. X11: the
  `CLIPBOARD` selection on a private connection served from the pump (`TARGETS`, `UTF8_STRING` / `STRING` /
  `text/plain;charset=utf-8`, `image/png`, MIME atoms, INCR both ways). Shown windows route their `UiContext`
  clipboard (line edit Ctrl+C / X / V) to it. Gates: `fuse_platform_text_input`, `fuse_platform_ime_composition`,
  `fuse_platform_clipboard_roundtrip`.

### Android

The same `main()` runs as a NativeActivity (NDK `android_native_app_glue`, no Java code and no Gradle).
`fuse_app_add_android_app(<target> SOURCES ... PACKAGE org.x.y LABEL "Name" [ASSETS prefix=dir])`
(`App/cmake/FuseAndroidApp.cmake`) builds `lib<target>.so` with `main` renamed to `fuse_app_main`, strips it and packs
`<build>/<target>.apk` with `App/android/fuse_apk.py`. The packer writes the binary manifest itself, stores the library
uncompressed and 16 KB aligned (`extractNativeLibs=false`), adds `vendor/fonts` and the app's assets, and signs the APK
with APK Signature Scheme v2 using a debug key it creates in the build dir (`fuse_debug_key.pem`, not committed). It
needs Python 3 with `cryptography` and no SDK build-tools. At start, `android_main` copies the assets to the app's files
dir, makes that the working directory (relative paths such as `ui/hello.fuseui` work), redirects stdout / stderr to
logcat (tag `fuse_app`) and passes the intent extra `args` as extra argv words.

```sh
# NDK r26+ (tested with 30.0.16248370); ABI arm64-v8a or x86_64 (emulator)
cmake -S Source/FUSE/UI -B build-android -G Ninja -DCMAKE_BUILD_TYPE=Release -DFUSE_BUILD_UI_TESTS=OFF   -DCMAKE_TOOLCHAIN_FILE=$ANDROID_HOME/ndk/<ver>/build/cmake/android.toolchain.cmake   -DANDROID_ABI=x86_64 -DANDROID_PLATFORM=android-26
ninja -C build-android                                   # libraries + hello_app.apk + fuse_ui_android_demo.apk
adb install -r -t build-android/fuse_ui_android_demo.apk
adb shell am start -n org.fuse.uidemo/android.app.NativeActivity [--es args "--draw=cpu"]
adb logcat -s fuse_app
ANDROID_SERIAL=emulator-5554 ctest --test-dir build-android -L android   # the four gates below
```

The gates run on the host (`App/android/android_gate.py`: install, drive with `am` / `input` / `settings`, read the
state file the demo keeps with `run-as`) and skip with 77 when adb or a device with the APK's ABI is missing.
Two §6 budget gates sit beside them: `fuse_ui_bench_hud_mobile` runs the `hud_bench` APK (the ~300-node HUD of
`Game/samples/hud_bench_scene.hpp`, the same workload as the desktop `fuse_ui_bench_hud`) and gates the median UI CPU
per frame at 2 ms; `fuse_app_hello_startup_android` cold-launches `hello_app --startup-probe` six times (the first is a
warm-up) and gates the median process start → first presented frame at 400 ms. On an emulator the startup gate only
reports and skips (77): the emulator's own process / activity start takes ~0.4–0.5 s before `main()` and its Vulkan
is SwiftShader; `android_gate.py --strict` enforces it there too.
Emulator: an x86_64 Google APIs image (API 36 was used) with `emulator -avd <name> -no-window -gpu host` (or
`-gpu swiftshader_indirect`) has Vulkan (`adb shell pm has-feature android.hardware.vulkan.version`).

Behaviour on Android:
- `NativePlatform::show` waits for the activity window and returns `Vulkan` (`VK_KHR_android_surface`, the same
  swapchain presenter, GPU draw or CPU upload) or `Software` (`ANativeWindow_lock` of the CPU raster;
  `NativeOptions::vulkan = false`). Vulkan starts synchronously (`asyncVulkan` is ignored: a window that
  `ANativeWindow_lock` connected as a CPU producer cannot be handed to a swapchain). The window is the whole activity,
  sized by the OS; `AppWindow::setScale` gets the screen density / 160 unless `NativeOptions::scale` is set.
  `displays()` reports the screen with the safe area as the work area; `querySystemTheme()` follows night mode.
- Lifecycle: `APP_CMD_TERM_WINDOW` releases the swapchain and surface (the device stays), `APP_CMD_INIT_WINDOW`
  rebuilds them and repaints; the UI tree is never touched. Pause / resume, config changes (rotation, density;
  the manifest keeps the activity alive) and low memory are counted (`NativePlatform::lifecycle()`) and reported
  through `NativeHooks`; with `fuse_core` (or `Core/src/platform/platform_hardening.cpp` standalone)
  `fuse::platform::notifyAppVisibility` and `notifySurfaceLost/Restored` fire. Back sends Escape (closes a popup or
  dialog) and finishes the activity when nothing used it. Process death loses the UI state (no saved-state hook yet).
- Touch: every `AMotionEvent` contact goes to `UiContext::touchDown/Move/Up` (the same path as Win32 `WM_POINTER`):
  the first finger is the pointer (tap = click, no hover after lift), all are in `touches()` / `pinchScale()`; a
  vertical drag past `UiContext::kTouchSlop` over a scroll container pans it (the press is released without a click).
  Mouse wheel and hover work too. Popups are in-window overlays.
- Safe area: `NativePlatform::safeAreaInsets` (system bars + display cutout through `WindowInsets`, UI units) and
  `NativeHooks::insetsChanged`; pass them to `GameUi::setSafeArea` or pad the root (the demo does).
- Text: the soft keyboard is shown / hidden as text fields gain / lose focus (`InputMethodManager` on the decor view;
  `NativeOptions::autoSoftKeyboard`, `setSoftKeyboard`). Key events become `keyDown` / `keyUp` plus committed text
  through `KeyCharacterMap`; there is no IME composition (pre-edit) with NativeActivity.
### App projects (`project.json` schema 2)

A project declares that it is an application with `"schemaVersion": 2` and `"kind": "app"` (or `"hybrid"`: an app
whose viewport widgets may create game worlds on demand; `"game"` is today's behaviour and the default):

```json
{
  "schemaVersion": 2, "name": "my_tool", "kind": "app",
  "dimensions": { "enable3D": false, "enable2D": false, "enableUI": true },
  "requiredFeatures": [],
  "ui": { "root": "ui/main.fuseui", "styles": ["ui/theme.fusestyle"], "theme": "system",
          "redraw": "onDemand", "profile": "app", "text": "minimal" },
  "windows": [ { "id": "main", "title": "My Tool", "width": 800, "height": 600, "root": "", "redraw": "onDemand" } ]
}
```

- `requiredFeatures`: host features beyond what the kind implies — `AppPresent`, `GameFrame`, `Renderer3D`,
  `Renderer2D`, `Physics`, `Audio`, `Script`, `Jobs`, `Viewport`. `app` implies `AppPresent`, `hybrid` adds `Viewport`,
  `game` implies `GameFrame` + `Jobs` + renderers / physics per dimension (`fuse::project::impliedHostFeatures`).
- `windows[].root` empty = `ui.root`; no `windows` = one window titled after the project. Paths stay inside the project.
- Schema 2 is validated strictly (`fuse::project::validateManifest`): unknown keys are errors except `x-*`, errors name
  the field (`$.windows[0].width: ...`). An `app` has no 3D / 2D worlds and no game-side features (use `hybrid`).
- Schema 1 files load unchanged (kind `game` in memory) and are written back as schema 1. Migrate with
  `fuse_project_tool --migrate <project> [--dry-run]` (or `fuse::project::migrateProjectFile`).
- New app projects: the editor's New Project wizard (Type "Application (FUSE UI)"), `fuse_project_tool --new-app <dir>`
  or `fuse::project::createAppProject`; the files match [`Templates/FuseUIApp`](../../Templates/FuseUIApp/README.md).

The minimal init path: `fuse::project::planInit` says what a host brings up; `fuse::app::ProjectHost`
(`fuse/app/project_host.hpp`, in tree) loads the project, enables `TrackBHostFeature::AppPresent` (the host-scoped
present unlock for apps, honoured in shipping builds), starts the job scheduler only when `Jobs` is declared, opens the
windows with their documents and style sheets and presents the first frame — no worlds, `GpuScene`, `FrameComposer` or
physics. Run a project with `fuse_app_host <project> [--headless] [--first-frame]`. The first frame is software while
the Vulkan presenter starts on a worker (`NativeOptions::asyncVulkan`).

`ui.theme`: `system` follows the OS colour scheme live (`Application::setFollowSystemTheme`), `dark` / `light` pin the
widget theme; other names are refused at start. A `hybrid` project's game side (job scheduler, `HybridComposer` with
the project's dimension flags, the default worlds, physics) stays down until one of its `viewport` widgets is visible
(polled 4× a second); then it renders into every visible viewport at 60 Hz and drops back to polling when they hide.
`ProjectHost::ensureGameSide()` brings it up on request.

The `ui.draw_list` kernel keeps a persistent `VkPipelineCache` in the per-user cache dir (`%LOCALAPPDATA%\FUSE\cache`,
`$XDG_CACHE_HOME/fuse` or `~/.cache/fuse`; `NativeOptions::pipelineCacheDir`), validated by the kernel's SPIR-V hash and
the driver's vendor / device / `pipelineCacheUUID`. `FUSE_APP_VK_TRACE=1` prints the Vulkan presenter start-up phases.

Baseline measurements for §6 of the plan (sizes, startup, idle, memory, frame times) are in [BASELINES.md](BASELINES.md).

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
- `StringTable` holds per-locale `key=value` tables; bound nodes are re-texted on `setLocale` + `refresh`. See
  [Localisation](#localisation) for messages with arguments, plurals and the extraction CLI.
- The sound hook (`setSoundHook`) receives Move / Accept / Back / Open / Close; wiring it to `fuse_audio` and theme
  sound names is the game's job for now.

## Localisation

`fuse_ui_l10n_lib` (`fuse/ui/l10n/l10n.hpp`, namespace `fuse::ui::l10n`) has no ICU dependency. Locale tags are
matched case-insensitively, region first (`de-CH`, `pt_PT`, `en-IN`), then the language; digits are always Latin.

```cpp
namespace l10n = fuse::ui::l10n;
l10n::formatMessage("en", "{n, plural, =0 {No items} one {# item} other {# items}}", {{"n", 1000}}); // "1,000 items"
l10n::formatMessage("ru", "{n, plural, one {# файл} few {# файла} many {# файлов} other {# файла}}", {{"n", 22}});
l10n::formatMessage("en", "{g, select, female {She} other {They}} left on {d, date, long}",
                    {{"g", "female"}, {"d", l10n::Date{2024, 3, 5}}});                        // "She left on March 5, 2024"
l10n::formatNumber("de", 1234.5);       // "1.234,5"     formatNumber("hi", 1234567) -> "12,34,567"
l10n::formatPercent("tr", 0.5);         // "%50"
l10n::formatDate("ja", {2024, 3, 5}, l10n::DateStyle::Long); // "2024年3月5日"
l10n::pluralCardinal("ar", l10n::PluralOperands::fromDouble(3)); // Plural::Few
```

- Plural rules (CLDR 44, cardinal and ordinal) with the operands n, i, v, f, t, so `"1.0"` and `1.5` pick the decimal
  categories: en, de, nl, sv, fi, da, nb, et, gl, fr, es, it, pt, pt-PT, ru, uk, pl, cs, sk, ar, he, tr, el, hu, bg,
  hi, bn, fa, ja, zh, ko (and the root rule, other only, for anything else). Ordinals: en, fr, it, sv, uk, hi, bn;
  others are other only. The compact-exponent operands (c / e) are not supported.
- Messages follow ICU MessageFormat: `{name}`, `{n, number[, integer|percent]}`, `{d, date[, short|medium|long]}`,
  `{n, plural, [offset:k] =0 {..} one {..} other {..}}`, `{n, selectordinal, ..}`, `{g, select, .. other {..}}`,
  nesting, `#` (the plural number minus the offset, locale-formatted; literal inside a nested select, as in ICU),
  ICU apostrophe quoting (`''` is `'`; `'{...}'` is literal; a lone `'` is literal). Arguments are text, numbers
  or `Date`; a decimal string (`"1.50"`) keeps its visible fraction digits for plural selection and `#`. A missing
  argument prints as `{name}`; a malformed pattern is an error (`formatMessageOr` returns the pattern instead).
- Numbers: decimal / grouping separators, minimum grouping digits (es, pl, pt-PT: `1234` stays ungrouped), Indian
  grouping (hi, en-IN), minus signs (U+2212 for sv, fi), percent placement; half-even rounding. Dates (gregorian
  short / medium / long and month names): en, en-GB, en-IN, de, fr, es, it, pt, nl, sv, ru, pl, tr, ja, zh, ko, ar;
  other locales use en. Time formatting, currencies, skeletons and non-Latin digits are not done.

`StringTable` (in `fuse_ui_game`) on top of it:

```cpp
StringTable st;
st.add("en", "items={n, plural, one {# item} other {# items}}\ngreet=Hello, {name}!");
st.add("de", "items={n, plural, one {# Gegenstand} other {# Gegenstände}}");
st.setLocale("de");
st.tr("items", {{"n", 3}});        // "3 Gegenstände", formatted with the locale the string came from
st.tr("greet", {{"name", "Ann"}}); // "Hello, Ann!" (fallback locale: the first added, or setFallbackLocale)
st.tr("greet");                    // raw value, as before
st.bind(ui, label, "items", {{"n", 3}}); // re-texted on refresh()
```

Table files are one `key=value` per line, `#` comment lines, escapes `\\ \n \t` (and `\=`, a leading `\#` in keys).

### `fuse_ui_extract_strings`

```
fuse_ui_extract_strings --out=de.strings [--locale=de] [--source] [--fn=tr,...] [--no-locations] [--drop-obsolete]
                        <files or directories>...
```

Scans `.fuseui` documents (constant `text` except line / text edits, `items`, `a11yName`; strings with a `{`
binding are skipped) and C++ sources (`.cpp .h .hpp ...`) for `tr("...")` calls whose first argument is a string
literal or adjacent literals (escapes, `u8` and raw literals decoded; comments and other strings ignored), and writes
the table sorted by key with `#: file:line` (or `file:node id`) location comments. An existing output file is merged:
translations are kept, untranslated keys are written as `#? key` (ignored by `StringTable`, so lookups fall back),
keys new since the last run are flagged `#, new`, and translations whose key left the sources move to
`#~ key=value` at the end (restored if the key comes back; `--drop-obsolete` removes them). `--source` writes new
keys as `key=key` for the source-language table. A summary and the new / obsolete keys are printed; exit code 1 on
an unreadable input or a broken document. The output is deterministic for the same inputs.

## Data and editor widgets (UI6 Tier C/D)

- `setTableDelegate(table, col, CellDelegate)` paints a column with your own callback; `SortFilterProxy` wraps an `ItemModel` with a filter predicate and a sort column (header clicks sort).
- `gridView(parent, model, cellW, cellH)` is a virtualised icon grid; `gridColumns`, `gridFirstVisibleRow`.
- `propertyGrid(parent, std::span<const PropertyDesc>)` builds grouped rows (bool, int, float, string, enum, colour); read with `propertyValue`, collapse with `setPropertyGroupOpen`.
- `colorPicker`, `colorPickerColor`, `setColorPickerColor`; `calendar`, `calendarDate` (`daysFromCivil`/`civilFromDays` helpers); `chart(parent, ChartKind)` + `chartSetSeries`; `markdownView` + `setMarkdown` (headings, paragraphs, lists, code, quotes).
- Tier D editor widgets: `fontPicker`, `fileDialog`, `nodeGraph`, `curveEditor`, `timeline`, `viewportWidget` (forwards input to your callback, paints your image), `codeEdit` (line numbers, highlighting), `dateTimeEdit` (`value()` = days since 1970, `dateTimeEditMinutes`), `videoWidget` (frame-source stub).
- `fuse_ui_sql::SqliteModel`: `open`/`attach`, `setQuery`, `sortBy`, `refresh`; pass `model()` to `tableView`. Only the visible window of rows is fetched.
- Platform feel (`platform_feel.hpp`): `setHapticsHandler`, `ScrollProfile::desktop()/ios()/android()`, `kineticScrollArea` (drag/fling with rubber band; a drag never clicks; with `ScrollProfile::minZoom < maxZoom` a
  pinch or Ctrl+wheel zooms the content about the gesture point — `kineticZoom` / `setKineticZoom`).
- Accessibility (`fuse/ui/a11y/a11y.hpp`): `Bridge::sync` diffs `UiContext::snapshotA11y` (plus `extras()`: menu / menu bar / tree kinds from the widget class, text-field value, expanded, selected), `perform`/`enqueue`/`drain` run assistive-tech actions on the UI thread (focus, click, increment / decrement, set value / text, expand / collapse, scroll into view). AccessKit needs a Rust toolchain: `FUSE_UI_ACCESSKIT_BUILD=ON` builds accesskit-c 0.22.3 with cargo, or point `FUSE_UI_ACCESSKIT_DIR` at a prebuilt one; without it the bridge still builds.
- Screen readers on a native window (`fuse/app/accessibility.hpp`, `fuse_app_a11y`): after `native.show(win)`, keep a `fuse::app::WindowAccessibility a11y(native, win);` (declared after the `NativePlatform`). On Windows it answers `WM_GETOBJECT` through `NativePlatform::setMessageHook` with AccessKit's UI Automation provider (Narrator, NVDA, Inspect); UIA actions are queued and run on the UI thread from the event pump. Bounds are client px (logical px x client width / viewport). The Rust library links the release CRT (Debug MSVC builds may warn LNK4098). On Linux it is AccessKit's AT-SPI adapter (Orca, Accerciser): the adapter registers once AT-SPI is enabled (`org.a11y.Status.IsEnabled`, which screen readers set), the window's client rect is reported in screen px each frame (AT-SPI extents are screen coordinates), text fields carry a text run (AT-SPI `Text`), and disabled widgets offer no focus / click action. AccessKit 0.22's AT-SPI side has no `EditableText` and no expanded state: an AT reads text but cannot write it. The distro cargo of Ubuntu 24.04 (1.75) is too old; a per-user rustup (`~/.cargo/bin`) is picked up first.
- Designer: `fuse_ui_designer [file.fuseui] [--icons=pack.fuseicons] [--save=out] [--screenshot=png]`. `DesignDocument` saves exactly what it loaded; keys starting with `x-` are kept for tools and stripped from the live preview. The property grid has curated rows, an `icon` picker (the context's `IconSet`; the app loads the starter pack) and an `effect` picker (the `EffectRegistry`, each effect with its parameter defaults, e.g. `ripple(4, 24, 40)`; `effect-params` edits the full text), then every other style property from `propInfo()` under "All style" (folded), so new style properties (variable-font axes, font features, ...) appear without designer changes. Under the grid an icon palette shows every pack icon rendered; clicking one sets the selection's `icon` (`Designer::iconNames`, `effectChoices`, `propertyNames`).
- `dockSaveLayout(dock)` returns a text layout (`fusedock 1`); `dockRestoreLayout(dock, text)` checks the whole text before applying it and ignores unknown panel ids.

## Styling (CSS subset, D4)

Selectors: type, `.class`, `#id`, `:hover :pressed :focus :focus-visible :checked :disabled`, descendant and `>`
combinators. Tokens are `--name` in `:root` and used with `var(--name[, fallback])`; they are global (scoped tokens
are deferred). Inline (`setCss`) beats everything; later sheets win ties. Layout properties: `layout`
(box/grid/stack/anchor/absolute/flow), `flex-direction`, `flex-wrap`, `gap`, `padding*`, `margin*`,
`width/height/min-*/max-*`, `flex-grow/shrink/basis`, `align-items/self`, `justify-content`,
`grid-template-columns`, `grid-row/column(-span)`, `left/top/right/bottom/center-x/center-y`, `overflow`
(visible/clip/scroll), `display`, `pointer-events`, `cursor`. Paint: `background` (colour or `linear-gradient`),
`background-end` (gradient end; widgets use it as the accent), `border`, `border-width/color`, `border-*-radius`,
`box-shadow`, `shadow-x/y/blur/color`, `backdrop-blur`, `effect`, `opacity`, `color`, `font-size`, `text-align`, `icon`,
`icon-size`, `zoom` (a number: scales every px length and the font size of the node and its subtree, multiplying down
the tree; text is re-rasterised at the new size). Selectors also accept
`:active`, `:selected`.

Right-to-left: `direction: rtl` (inherited, `ltr` default; e.g. `:root { direction: rtl; }`) mirrors box rows and
column cross-axis alignment, `justify-content` / `align-*` start and end, grid columns and flow lines; margins and
padding stay physical, stack / anchor / absolute placement is not mirrored. `text-align: start` / `end` follow the
direction (`left` / `right` are physical). Overflow scrolling starts at the right (`scroll().x` counts from the start
edge) with the vertical bar on the left. Widgets mirror: indicators of check box / radio / switch, slider / progress /
scroll bar (minimum on the right; Left increases a slider), spin / combo arrows, line edit alignment, tab bar and menu
bar order, tree indentation (Left expands), list scroll indicator. Popup menus of an RTL owner inherit its direction and
take `at` as their top-right corner (aligned to the right edge of the menu button / combo box); submenus open to the
left and Left / Right open / close them. Not mirrored yet: the Tier B2 / C / D widgets (table, splitter, dock, grid
view, calendar, chart, colour picker, node graph, curve editor, timeline, code edit, date-time edit), `popupPanel`,
tooltips, and the line edit caret over bidi text (paragraph direction still comes from the text's first strong
character).

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
| `fuse_ui_rtl_layout` | `direction: rtl`: mirrored box rows / justify / align / grid / flow with physical margins, inheritance, `text-align`, slider pointer + keys, tab bar, tree keys, scroll bar side and horizontal start, right-aligned menus and submenus to the left; text-free widgets render as the mirror image of LTR; RTL sample against `tests/golden/rtl_layout.png` (`rtl_layout_full.png` in FULL) |
| `fuse_ui_keyboard_only` | a form with every interactive Tier A–D widget driven only by keyDown / keyUp / textInput: Tab and Shift+Tab reach every widget in one order, each is operated by its keys (see "Keyboard model"), menus by F10 / arrows / Escape, the file dialog traps focus and Escape cancels, keyboard focus scrolls into view; focused vs unfocused paint differs for every focusable widget in the dark and light themes |
| `fuse_ui_zero_alloc_steady` | zero heap allocations over 50 interactive frames after warm-up |
| `fuse_ui_bench_10k` | 10k nodes, one dirty label: ≤ 0.5 ms median (Release; 1.0 ms in FULL) |
| `fuse_ui_single_thread_parity` | direct vs posted-from-threads mutations give identical layout, draw list and pixels |
| `fuse_ui_a11y_tree_dump` | roles, names, states, values |
| `fuse_ui_icon_cook` | icon cooker determinism (any input order; tool = library), index round trip against the rasterised SVGs, SVG subset, corrupt-pack fuzz, resampled sizes; `fuse_ui_cook --icons` writes no pack when a document icon is missing |
| `fuse_ui_icon_render` | icons at 16 / 24 / 32 / 40 / 48 px in two tints and disabled against `tests/golden/icons.png`; hover tint, button / menu / `.fuseui` / cooked-style icons; multi-colour icons (layers, tint vs fixed colours) against `tests/golden/icons_color.png` |
| `fuse_text_grapheme_conformance` | `GraphemeBreakTest.txt` 15.1 against fuse_text's UAX #29: 1187/1187 (both profiles) |
| `fuse_text_linebreak_conformance` | `LineBreakTest.txt` 15.0 (trimmed) against `lineBreaks(Strict)`: 2389/2406 + 17 listed libunibreak exceptions (FULL); MINIMAL runs its letters / space / newline subset |
| `fuse_text_bidi_conformance` | `BidiCharacterTest.txt` 15.1 (trimmed): paragraph level, levels after L1, visual order: 4726/4726 (FULL; 77 in MINIMAL) |
| `fuse_text_shaping_scripts` | Latin, Cyrillic, Greek, Arabic, Hebrew, Devanagari, Thai, CJK, emoji through the bundled chain: per-script shaping checks + `tests/golden/shaping_scripts_full.png` (FULL; 77 in MINIMAL) |
| `fuse_text_system_fonts` | `FontCatalog::system()`: Segoe UI / Arial, weight / italic / generic matching, per-script chains, `fontPicker("@system")` (77 with no installed fonts) |
| `fuse_text_cache_zero_alloc` | layout / measure / atlas / segmentation of already-seen strings (LTR, RTL, fallback, wrapped, elided): zero allocations |
| `fuse_text_dpi_1`, `_1_25`, `_1_5`, `_2` | bundled font + chain at 14 px × scale: metrics, widths, glyph boxes and ink scale; same line breaks; no missing glyphs |
| `fuse_text_colrv1` | `tests/data/colrv1_test.ttf`: flattened solid layers without `enableColorGlyphs`; with it exact rendering against `tests/golden/colrv1_exact.png` (max channel difference 0; COOKED <= 1: foreground glyphs rebuilt from black / white renders) |
| `fuse_text_bitmap_emoji` | `tests/data/bitmap_{cbdt,sbix}_test.ttf`: strike choice per size, placement, sbix dupe, premultiplication, the atlas colour page, colour glyphs through the UI |
| `fuse_text_msdf` | MSDF vs coverage at 64 / 128 / 256 px (mean error < 0.06, < 0.6 % of pixels off by more than half), determinism, `setMsdfThreshold` with DPI scale, GPU parity of colour + MSDF glyphs (compute kernel exact, SDF quads within their blend tolerance) |
| `fuse_text_cooked` | `fuse_text_cook` determinism (every profile); COOKED: build-time pack = fresh cook, `.ttf` rejected, layout of sample strings equal to the MINIMAL reference (ids, pen positions within 1e-3 px) and renders byte-identical at every cooked size, MSDF for other sizes |
| `fuse_text_variable` | `tests/data/variable_test.ttf` (`make_variable_test_font.py`: wght with avar + wdth, IUP-optimised gvar, composite, HVAR, MVAR, variable COLR): axes / named instances, MVAR ascender, stem width and HVAR advance at wght 100 .. 900 and wdth 50 / 150 against the font's analytic values, phantom-point advances = HVAR, Var* paint colours at four weights, CSS weight / stretch / variation settings and an `AnimProp::FontWeight` tween, zero-alloc steady state; Segoe UI Variable / Bahnschrift when installed: catalog WWS faces, our glyf decoder vs stb_truetype outlines (MSDF max difference 1), HarfBuzz advances (FULL); COOKED: a pack cooked at wght 700 |
| `fuse_text_features` | `tests/data/features_test.ttf` (`make_features_test_font.py`): `liga` / `kern` off, `tnum`, `smcp`, `ss01` through `Font::withFeatures` and CSS `font-feature-settings`, Open Sans `pnum`, zero-alloc steady state (FULL; 77 in MINIMAL / COOKED) |
| `fuse_text_bench_layout` | 1 MB mixed-script document: paragraphs cold / warm, codeEdit frames, one-block layout, markdownView first update / resize within budgets (~4x measured), zero allocations and (FULL) zero run-cache misses relaying out a screen |

| `fuse_app_idle_zero_frames`, `fuse_app_multiwindow`, `fuse_app_hello_startup`, `fuse_app_settings` | UI5 loop, windows, startup ≤ 150 ms, settings / command line / single instance |
| `fuse_app_native_window` | real X11 / Win32 window, Vulkan GPU-draw and CPU-upload presenters vs the CPU raster, async Vulkan handover (software frame, resize during the handover, first Vulkan frame) (skips 77 without a display or device) |
| `fuse_ui_a11y_windows` | UIA client (IUIAutomation) on a real Win32 window: control types and names, screen bounds, Invoke / Toggle / Value / RangeValue / ExpandCollapse, SetFocus + focus-changed event (Windows; skips 77 without AccessKit) |
| `fuse_ui_vk_damage` | incremental GPU redraw of `DrawList::redrawRegion(damage)` (grown around backdrops): only touched tiles dispatched, result equals a full render |
| `fuse_ui_vk_validation_clean` | `fuse_ui_vk_parity` + `fuse_ui_vk_damage` under `VK_LAYER_KHRONOS_validation` with sync validation: no warnings / errors (77 without the layer) |
| `fuse_ui_vk_custom_effect` | built-in effects (desaturate, ripple, grain: plain, rounded, in a rounded clip, at the edge, scissored) bit-exact against their CPU twins; a user effect via `fuse_ui_add_effect` (image + native exp / sin) within ±2; `effect:` style hookup and animated repaint; validation clean; no steady-state allocation |
| `fuse_ui_vk_sdf_quads` | SDF-quad backend vs the CPU reference (sample, primitives, blur + effects between draws, scissored redraw): max error ≤ 2, mean ≤ 0.1 / 255; validation clean; reports compute vs SDF timings at 1080p and 4K |
| `fuse_ui_vk_budget` | GPU time (timestamp queries) of 1080p frames: full, backdrop blur, incremental, a ~270-node HUD ≤ 0.5 ms (software devices report only) |
| `fuse_app_native_startup` | launches `hello_app` 5× per configuration: default (async Vulkan) and software-only first frame median ≤ 150 ms; every default run switches to Vulkan (median ≤ 2 s) with the switched frame equal to the CPU raster; synchronous Vulkan init ≤ 2 s (skips 77 without a display) |
| `fuse_app_binary_size` | `hello_app` file size ≤ measured baseline + 10 % (MSVC x64 Release, MINIMAL text: standalone 870,912 B → 958,000 B, in the engine tree 904,192 B → 994,600 B; Linux x86-64 Release, MINIMAL, stripped: GCC 13 standalone 1,530,488 B → 1,683,600 B, engine tree 1,591,928 B → 1,751,200 B, Clang 23 standalone 1,179,616 B → 1,297,600 B; other toolchains report and skip until a baseline is set with `-DFUSE_APP_BINARY_BUDGET`). Effect and SDF-quad SPIR-V live in their own translation units, so apps that do not use them do not carry it |
| `fuse_app_window_chrome` | custom-chrome window: `chromeHitTest` probes, then `WM_NCHITTEST` on a real Win32 window, frame removed, max / restore / min / close through non-client clicks; X11 (Xvfb, the gate watching the root like a WM): `_MOTIF_WM_HINTS`, `_NET_WM_MOVERESIZE` direction / position per probe, edge cursors, `_NET_WM_STATE` maximise / restore applied by the gate, `WM_CHANGE_STATE`, close (77 without a display) |
| `fuse_app_tray` | Win32: tray icon added and found (`Shell_NotifyIconGetRect` / `NIM_MODIFY`), simulated click / double-click callbacks, a real `TrackPopupMenu` context menu picked by posted keys, a notification, removal. Unix (under `dbus-run-session`, the gate plays watcher, notification daemon and tray host): registration, `IconPixmap` / `ToolTip` properties and change signals, `Activate`, dbusmenu layout + click, notification + `ActionInvoked`, name released on removal, `Unsupported` without a daemon (77 without a tray / private bus) |
| `fuse_ui_a11y_linux` | AT-SPI client (libatspi, loaded at runtime) on a real X11 window under `dbus-run-session` + Xvfb; the gate starts the accessibility bus and enables AT-SPI: the app in the registry, roles and names (push button, check box, slider, entry, list, tree, menu bar / item), states (enabled, focusable, checked; disabled), screen extents of a button, `click` on a button / check box, `Value` on the slider, `grab_focus` + focus event, `Text` read-back and a text change, File menu opened / closed by `click` (skips 77 without AccessKit / libatspi / a session bus) |
| `fuse_x11_wm_chrome`, `fuse_x11_wm_popups`, `fuse_x11_wm_multiwindow` | under a real window manager the gate starts on Xvfb (openbox / xfwm4 / metacity ..., also `~/opt/<pkg>/usr/bin` for one unpacked without root, `$FUSE_TEST_WM`), real pointer through XTest: custom chrome undecorated, caption drag moves and corner drag resizes (`_NET_WM_MOVERESIZE`), max button / caption double click maximise + restore, min iconifies and `_NET_ACTIVE_WINDOW` restores, close; OS popups not reparented and placed below the WM title bar, real clicks pick / dismiss; two managed windows, focus by `_NET_ACTIVE_WINDOW`, clicks to the window under the pointer, `resizeWindow` through the WM, `_NET_CLOSE_WINDOW` closes one (77 without a WM) |
| `fuse_app_tray_host` | the tray icon under a real StatusNotifierItem host: xfce4-panel's status tray (started with xfconfd on a private bus + Xvfb, `$FUSE_TEST_SNI_HOST`), registered and shown, real left click activates, real right click opens the host's menu built from the dbusmenu export and keys pick an item (`EventGroup`), removal unregisters (77 without the panel) |
| `fuse_x11_cursor_size` | XSETTINGS `Gtk/CursorThemeName` / `Gtk/CursorThemeSize` with a private Xcursor theme (`XCURSOR_PATH`): the server shows the cursor at 24 px at 96 dpi, 48 px at 192 dpi, then 32 / 64 px as the size changes (XFixes cursor image) |
| `fuse_x11_randr`, `fuse_x11_xsettings`, `fuse_x11_wake` | X11 under Xvfb against the gate's own connection: two RandR monitors + `_NET_WORKAREA` in `displays()` and a monitor removed live; an XSETTINGS manager's `Xft/DPI` / `Net/ThemeName` at start and live (every window re-scales, `onDpiChanged`, `onThemeChanged`), manager exit → `RESOURCE_MANAGER`, a new manager; `wake()` from another thread ends a blocked pump and an accessibility action queued from an AT thread runs at once |
| `fuse_ui_platform_services` | headless: DPI scale (layout fixed, device-pixel draw list, re-rasterised glyphs), `cursor:` style, reduced motion, touch / pinch / pen through `platform_bridge`, dark → light theme slot |
| `fuse_platform_dpi_change`, `fuse_platform_displays`, `fuse_platform_cursors`, `fuse_platform_system_theme`, `fuse_platform_high_contrast`, `fuse_platform_dialogs`, `fuse_platform_touch_synthetic`, `fuse_platform_pen_synthetic`, `fuse_platform_multiwindow` | real windows: `WM_DPICHANGED` re-scale and back, monitors, OS cursor per hovered widget, theme change via `WM_SETTINGCHANGE` (faked query, user settings untouched), high contrast (faked query: scheme colours, forced colours over app sheets, 2px focus border, text scale on every font size, back to normal), dialogs dismissed by a helper thread, injected touch tap + pinch, synthetic pen (`InjectSyntheticPointerInput` PT_PEN: pressure, tap; 77 without synthetic pointers or when another window covers the test window), two swapchain windows with input routing / close / per-window DPI |
| `fuse_platform_text_input`, `fuse_platform_ime_composition`, `fuse_platform_clipboard_roundtrip` | real windows (Win32; X11 under Xvfb): typed text incl. a surrogate pair / remapped keysyms into a line edit, Ctrl+C / V / X against another clipboard client, IME off outside text fields; an IMM32 composition (`ImmSetCompositionStringW`: inline pre-edit, commit, cancel, composition / candidate window at the caret; X11: a dead-key compose through XIM, pre-edit only with a server IM); clipboard text / RGBA image / custom MIME / several formats / lazy items / INCR against another thread (Win32) or X connection |
| `fuse_platform_present_android` | Android build tree, adb: the demo presents on Vulkan (GPU draw and CPU upload) and in software; each surface's first frame read back in-process matches the CPU raster; density scale applied |
| `fuse_app_lifecycle_android` | HOME releases the surface, `am start` rebuilds it with a correct first frame and the same UI state; rotation resizes to landscape and back |
| `fuse_platform_touch_android` | `input tap` clicks the button; `input swipe` over the list scrolls it both ways without clicking |
| `fuse_platform_ime_android` | tapping the line edit focuses it and shows the soft keyboard; `input text` and backspace edit it |
| `fuse_app_idle_native` | real idle window for 3 s: zero presents, ≤ 10 loop wakeups / s (OS messages only), CPU ≤ 2 % |
| `fuse_project_schema2`, `fuse_project_schema2_native` | project.json schema 1 load / write / migrate, schema 2 round trip, 24 rejected files, the app template, `ProjectHost` brings up only `AppPresent` and presents the `.fuseui` (headless ≤ 150 ms; `_native` on a real window vs the CPU raster) |
| `fuse_ui_game_hud_golden` | HUD demo scene against `tests/golden/hud.png` plus trail / marker / pool state |
| `fuse_ui_gamepad_nav` | navigation with delay + repeat, A / B, radial menu by stick, prompt glyph swap, focus sounds |
| `fuse_ui_world_panel_raycast` | ray → panel pixel mapping, misses and back faces, ray clicks, idle surface does not re-render |
| `fuse_ui_input_arbitration` | pointer / keyboard / gamepad ownership between UI and game |
| `fuse_ui_pause_time_domain` | pause and slow motion affect game-time HUD only; locale switching |
| `fuse_ui_l10n` | plural categories against the CLDR sample values (cardinal + ordinal, 20+ locales), message formatting (plural, selectordinal, select, nesting, offset, quoting, number / date arguments, errors), number / percent / date formatting per locale, `StringTable::tr` with arguments and fallback, bound nodes; `fuse_ui_extract_strings` on `tests/data/l10n` (exact output, merge keeping translations, idempotent re-run, `--source`, `--fn`) |
| `fuse_demo_hybrid_hud_ui` | `demo_hybrid_hud` HUD on a UI context in the hybrid frame (`HybridComposer::setUiOverlay` → `UiFrameSource::recordOver`, over the sprite layer): every opaque HUD pixel of the GPU readback equals the CPU raster (≤ 1/255), glyphs visible |
| `fuse_ui_vk_golden` | GPU vs CPU reference with NVIDIA FLIP (`vendor/nvidia-flip`) over the Tier A sample, the game HUD and an editor-like scene: compute kernel max channel error ≤ 2 and mean FLIP ≤ 0.001 (0 today), SDF quads ≤ 4 / ≤ 0.005; heat maps written on failure |
| `fuse_ui_vk_tier0` | UI-T0 (§4.9): Vulkan 1.1 instance, a device with no extensions and no features, validation on; every shader's SPIR-V ≤ 1.3 without descriptor-indexing / BDA capabilities; scenes, backdrop and an external texture exact against the CPU |
| `fuse_ui_vk_zero_alloc` | the whole steady-state GPU frame loop (update, pack, upload, record into pre-made command buffers, submit, wait), full and damage-scissored frames: 0 heap allocations over 120 frames, result equal to a full CPU raster |
| `fuse_ui_bench_hud` | ~300-node game HUD animated as in play: UI CPU per frame (update + layout + paint) median ≤ 0.5 ms (Release; scene shared with `fuse_ui_bench_hud_mobile`) |
| `fuse_ui_hud_binary_size` | `hud` profile: the `hud_demo` sample with cooked `.fusefont` text ≤ baseline + 10 % (MSVC x64 Release, standalone, `FUSE_UI_TEXT=COOKED`: 596,480 B → 656,200 B; other builds report and skip, `-DFUSE_UI_HUD_BINARY_BUDGET` sets one) |
| `fuse_ui_script_bind` | the Lua `ui` table as a whole: every function known and exercised (a new binding without coverage fails), pcall-able argument errors, every signal kind with its arguments, a failing handler does not stop the next, a handler destroying its own node, handlers released with their nodes (2000 create / on / destroy cycles: handler count and Lua heap flat), no Lua garbage over steady frames |
| `fuse_ui_widget_<class>` (36 classes) | one gate per registered widget class outside Tier D (`test_widget_gates.cpp`): node of the class with its accessibility role in the snapshot, laid out and painting, deterministic CPU raster against `tests/golden/widgets/<class>.png` (MINIMAL; `FUSE_UI_UPDATE_GOLDEN=1` rewrites), Tab reaches focusable ones, disabled ones look different and take no pointer / key / text input or focus, and the widget's own behaviour (clicks, keys, drags, wheel / fling, popups, selection, sorting, ...) |
| `fuse_ui_widget_catalogue` | every class in the widget registry has a `fuse_ui_widget_` gate (the list above or the Tier D ones of `fuse_ui_tier_d2`) |

Extra: `fuse_ui_text_basics`, `fuse_ui_text_full`, `fuse_ui_widget_interaction`, `fuse_ui_tier_b`, `fuse_ui_tier_b2`,
`fuse_ui_relight_equivalence`, `fuse_ui_platform_keys_v2`, `fuse_ui_text_input`, `fuse_ui_doc_load`,
`fuse_ui_lua_bindings`, `fuse_ui_vk_parity` (also multi-colour icons), `fuse_ui_baselines` (report: §6 baselines — CPU
raster and GPU time of the sample, 1080p cards and the 1440p HUD, atlas and GPU memory), the sample runs `fuse_app_hello_sample`, `fuse_app_notes_sample`, `fuse_app_game_menu_sample`,
`fuse_ui_game_hud_demo`, `fuse_ui_game_world_panel_demo`; in tree also `fuse_core_ui1_text_input`,
`fuse_scene_renderer_vk_fuse_ui_t0` and `rl_overlay_glyphs`.

## Editor shell (UI9)

Configure with `-DFUSE_EDITOR_SHELL=fuse` to build `fuse_editor_fuse`, which is the editor chrome in FUSE UI on the shared Qt-free `EditorController`. Run `ctest -L fuse_editor_ui`. All ten editor panels and the New Project wizard are ported (45 gates, including the Top / Front / Side orthographic views with View > Viewport Layout single / two / quad on Ctrl+Alt+1..3 (`multi_viewport`), the play-mode frame / badge and Maximize Viewport on Play (`play_frame`), the asset browser grid / type filter / breadcrumb / context menu (`asset_browser_polish`), timeline zoom / pan / loop / snap / key jumps (`timeline_nav`), Preferences shortcut rebinding (`shortcut_rebind`), File > Recent Projects (`recent_projects`), Group / Ungroup / Duplicate subtree / Frame Selected (`entity_group`), collider wireframes and drag handles (`colliders`), Entity > Open Script (`open_script`), the Terrain panel (`terrain`), the Navigation panel (`navigation`), Edit > Preferences (`preferences`) and view modes Lit / Unlit / Wireframe / Lit Wireframe and mesh bounds: `view_modes`). `--gpu-viewport` shows the live 3D scene in the viewport, zero-copy through a `ui_vk` external-texture slot (gates `scene_viewport`, `gpu_viewport`), with a Move/Rotate/Scale gizmo drawn as a viewport overlay (`viewport_gizmo`). Status is in [FUSE_EDITOR_QT_FIRST_PLAN.md §11](../plans/FUSE_EDITOR_QT_FIRST_PLAN.md). `fuse_editor_ui_frame` is the plan §6 name of the `ui_frame` gate, run on its own (RUN_SERIAL): the full 9-dock repaint (restyle + relayout + draw list of every node, median of 21) must stay ≤ 2 ms in optimised builds.
