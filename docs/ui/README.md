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
| `Source/FUSE/UI/Text` | `fuse_text` | Fonts (stb_truetype; FreeType in FULL), bundled Open Sans, UTF-8, `ITextShaper` (`MinimalShaper`, `HarfBuzzShaper` in FULL), wrap/ellipsis layout, glyph atlas, `TextEditBuffer`, UAX #29 / #14 / #9 segmentation, fallback chains, `FontCatalog` (system fonts) |
| `Source/FUSE/UI/Core` | `fuse_ui_core` | `UiContext` (tree, style, layout, paint, input, focus, signals, timers, animation, a11y), `DrawList` + `CpuRasterizer`, `StyleEngine`, `immediate::ImmediateUi`, `platform_bridge.hpp` |
| `Source/FUSE/UI/Widgets` | `fuse_ui_widgets` | Tier A + Tier B widgets + default dark theme (`widgets::install`) |
| `Source/FUSE/UI/Doc` | `fuse_ui_doc` | `.fuseui` JSON documents, binding model, expression bytecode |
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
  not-allowed crosshair`, inherited) picks the OS cursor; line edits, links and splitter handles set theirs.
- Theme: `Application::systemTheme()` (dark, accent, high contrast, reduced motion, text scale), `onThemeChanged`,
  live on `WM_SETTINGCHANGE` (X11: XSETTINGS `Net/ThemeName`, dark when the name says so; `$FUSE_UI_THEME` /
  `$GTK_THEME` override); `setFollowSystemTheme(true)` switches the default theme dark / light
  (`widgets::themeCss(ThemeVariant)`, `UiContext::setThemeStyleSheet`) with the system accent. Reduced motion always
  makes `animate()` jump; the title bar follows via `DWMWA_USE_IMMERSIVE_DARK_MODE`.
- Touch and pen (Win32 `WM_POINTER` → events_v2 `Touch*` / `Pen*` → `UiContext::touchDown/Move/Up`): the first
  finger is the pointer, every contact is in `touches()`, `pinchScale()` for two fingers, `pressure()` for the pen.
- `openFileDialog` / `saveFileDialog` / `pickFolderDialog` (IFileDialog; zenity on X11), `messageBox`, `openUrl`
  (http, https, mailto, file only), `revealInFileManager`.

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
- Accessibility (`fuse/ui/a11y/a11y.hpp`): `Bridge::sync` diffs `UiContext::snapshotA11y` (plus `extras()`: menu / menu bar / tree kinds from the widget class, text-field value, expanded, selected), `perform`/`enqueue`/`drain` run assistive-tech actions on the UI thread (focus, click, increment / decrement, set value / text, expand / collapse, scroll into view). AccessKit needs a Rust toolchain: `FUSE_UI_ACCESSKIT_BUILD=ON` builds accesskit-c 0.22.3 with cargo, or point `FUSE_UI_ACCESSKIT_DIR` at a prebuilt one; without it the bridge still builds.
- Screen readers on a native window (`fuse/app/accessibility.hpp`, `fuse_app_a11y`): after `native.show(win)`, keep a `fuse::app::WindowAccessibility a11y(native, win);` (declared after the `NativePlatform`). On Windows it answers `WM_GETOBJECT` through `NativePlatform::setMessageHook` with AccessKit's UI Automation provider (Narrator, NVDA, Inspect); UIA actions are queued and run on the UI thread from the event pump. Bounds are client px (logical px x client width / viewport). The Rust library links the release CRT (Debug MSVC builds may warn LNK4098).
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
`icon-size`. Selectors also accept
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

| `fuse_app_idle_zero_frames`, `fuse_app_multiwindow`, `fuse_app_hello_startup`, `fuse_app_settings` | UI5 loop, windows, startup ≤ 150 ms, settings / command line / single instance |
| `fuse_app_native_window` | real X11 / Win32 window, Vulkan GPU-draw and CPU-upload presenters vs the CPU raster, async Vulkan handover (software frame, resize during the handover, first Vulkan frame) (skips 77 without a display or device) |
| `fuse_ui_a11y_windows` | UIA client (IUIAutomation) on a real Win32 window: control types and names, screen bounds, Invoke / Toggle / Value / RangeValue / ExpandCollapse, SetFocus + focus-changed event (Windows; skips 77 without AccessKit) |
| `fuse_ui_vk_damage` | incremental GPU redraw of `DrawList::redrawRegion(damage)` (grown around backdrops): only touched tiles dispatched, result equals a full render |
| `fuse_ui_vk_validation_clean` | `fuse_ui_vk_parity` + `fuse_ui_vk_damage` under `VK_LAYER_KHRONOS_validation` with sync validation: no warnings / errors (77 without the layer) |
| `fuse_ui_vk_custom_effect` | built-in effects (desaturate, ripple, grain: plain, rounded, in a rounded clip, at the edge, scissored) bit-exact against their CPU twins; a user effect via `fuse_ui_add_effect` (image + native exp / sin) within ±2; `effect:` style hookup and animated repaint; validation clean; no steady-state allocation |
| `fuse_ui_vk_sdf_quads` | SDF-quad backend vs the CPU reference (sample, primitives, blur + effects between draws, scissored redraw): max error ≤ 2, mean ≤ 0.1 / 255; validation clean; reports compute vs SDF timings at 1080p and 4K |
| `fuse_ui_vk_budget` | GPU time (timestamp queries) of 1080p frames: full, backdrop blur, incremental, a ~270-node HUD ≤ 0.5 ms (software devices report only) |
| `fuse_app_native_startup` | launches `hello_app` 5× per configuration: default (async Vulkan) and software-only first frame median ≤ 150 ms; every default run switches to Vulkan (median ≤ 2 s) with the switched frame equal to the CPU raster; synchronous Vulkan init ≤ 2 s (skips 77 without a display) |
| `fuse_app_binary_size` | `hello_app` file size ≤ measured baseline + 10 % (MSVC x64 Release, MINIMAL text: standalone 870,912 B → 958,000 B, in the engine tree 904,192 B → 994,600 B; other toolchains report and skip until a baseline is set with `-DFUSE_APP_BINARY_BUDGET`). Effect and SDF-quad SPIR-V live in their own translation units, so apps that do not use them do not carry it |
| `fuse_app_window_chrome` | custom-chrome window: `chromeHitTest` probes, then `WM_NCHITTEST` on a real Win32 window, frame removed, max / restore / min / close through non-client clicks; X11 (Xvfb, the gate watching the root like a WM): `_MOTIF_WM_HINTS`, `_NET_WM_MOVERESIZE` direction / position per probe, edge cursors, `_NET_WM_STATE` maximise / restore applied by the gate, `WM_CHANGE_STATE`, close (77 without a display) |
| `fuse_app_tray` | Win32: tray icon added and found (`Shell_NotifyIconGetRect` / `NIM_MODIFY`), simulated click / double-click callbacks, a real `TrackPopupMenu` context menu picked by posted keys, a notification, removal. Unix (under `dbus-run-session`, the gate plays watcher, notification daemon and tray host): registration, `IconPixmap` / `ToolTip` properties and change signals, `Activate`, dbusmenu layout + click, notification + `ActionInvoked`, name released on removal, `Unsupported` without a daemon (77 without a tray / private bus) |
| `fuse_x11_randr`, `fuse_x11_xsettings`, `fuse_x11_wake` | X11 under Xvfb against the gate's own connection: two RandR monitors + `_NET_WORKAREA` in `displays()` and a monitor removed live; an XSETTINGS manager's `Xft/DPI` / `Net/ThemeName` at start and live (every window re-scales, `onDpiChanged`, `onThemeChanged`), manager exit → `RESOURCE_MANAGER`, a new manager; `wake()` from another thread ends a blocked pump and an accessibility action queued from an AT thread runs at once |
| `fuse_ui_platform_services` | headless: DPI scale (layout fixed, device-pixel draw list, re-rasterised glyphs), `cursor:` style, reduced motion, touch / pinch / pen through `platform_bridge`, dark → light theme slot |
| `fuse_platform_dpi_change`, `fuse_platform_displays`, `fuse_platform_cursors`, `fuse_platform_system_theme`, `fuse_platform_dialogs`, `fuse_platform_touch_synthetic`, `fuse_platform_multiwindow` | real windows: `WM_DPICHANGED` re-scale and back, monitors, OS cursor per hovered widget, theme change via `WM_SETTINGCHANGE` (faked query, user settings untouched), dialogs dismissed by a helper thread, injected touch tap + pinch, two swapchain windows with input routing / close / per-window DPI |
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
| `fuse_demo_hybrid_hud_ui` | `demo_hybrid_hud` HUD on a UI context in the hybrid frame (`HybridComposer::setUiOverlay` → `UiFrameSource::recordOver`, over the sprite layer): every opaque HUD pixel of the GPU readback equals the CPU raster (≤ 1/255), glyphs visible |

Extra: `fuse_ui_text_basics`, `fuse_ui_text_full`, `fuse_ui_widget_interaction`, `fuse_ui_tier_b`, `fuse_ui_tier_b2`,
`fuse_ui_relight_equivalence`, `fuse_ui_platform_keys_v2`, `fuse_ui_text_input`, `fuse_ui_doc_load`,
`fuse_ui_lua_bindings`, `fuse_ui_vk_parity` (also multi-colour icons), `fuse_ui_baselines` (report: §6 baselines — CPU
raster and GPU time of the sample, 1080p cards and the 1440p HUD, atlas and GPU memory), the sample runs `fuse_app_hello_sample`, `fuse_app_notes_sample`, `fuse_app_game_menu_sample`,
`fuse_ui_game_hud_demo`, `fuse_ui_game_world_panel_demo`; in tree also `fuse_core_ui1_text_input`,
`fuse_scene_renderer_vk_fuse_ui_t0` and `rl_overlay_glyphs`.

## Editor shell (UI9)

Configure with `-DFUSE_EDITOR_SHELL=fuse` to build `fuse_editor_fuse`, which is the editor chrome in FUSE UI on the shared Qt-free `EditorController`. Run `ctest -L fuse_editor_ui`. All ten editor panels and the New Project wizard are ported (45 gates, including the Top / Front / Side orthographic views with View > Viewport Layout single / two / quad on Ctrl+Alt+1..3 (`multi_viewport`), the play-mode frame / badge and Maximize Viewport on Play (`play_frame`), the asset browser grid / type filter / breadcrumb / context menu (`asset_browser_polish`), timeline zoom / pan / loop / snap / key jumps (`timeline_nav`), Preferences shortcut rebinding (`shortcut_rebind`), File > Recent Projects (`recent_projects`), Group / Ungroup / Duplicate subtree / Frame Selected (`entity_group`), collider wireframes and drag handles (`colliders`), Entity > Open Script (`open_script`), the Terrain panel (`terrain`), the Navigation panel (`navigation`), Edit > Preferences (`preferences`) and view modes Lit / Unlit / Wireframe / Lit Wireframe and mesh bounds: `view_modes`). `--gpu-viewport` shows the live 3D scene in the viewport, zero-copy through a `ui_vk` external-texture slot (gates `scene_viewport`, `gpu_viewport`), with a Move/Rotate/Scale gizmo drawn as a viewport overlay (`viewport_gizmo`). Status is in [FUSE_EDITOR_QT_FIRST_PLAN.md §11](../plans/FUSE_EDITOR_QT_FIRST_PLAN.md).
