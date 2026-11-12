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

### The rest of the catalogue

`groupBox` (optionally checkable), `stack`, `toolBar` / `toolBarAction`, `statusBar` / `statusMessage`, `mainWindow`
(menu bar + tool bar + dock area + status bar), `setContextMenu`, `messageBox`, `wizard`, `popover`, `toast`,
`headerView`, `textEdit` (word wrapped, undo, clipboard, IME), `plainTextEdit`, `richText` (inline markup with
links) and `doubleSpinBox`. Gate: `fuse_ui_tier_b3`, one `fuse_ui_widget_<name>` ctest per widget.

The last four pieces of the catalogue (gates `fuse_ui_widget_chart_flame`, `_dock_tabs`, `_dock_float`,
`_dock_layout_tabs_float`, `_minimap`, `_ammo_counter`):

- **Flame graph**: `chart(ui, parent, ChartKind::Flame)` + `chartSetFlame(ui, chart, spans)` with
  `FlameSpan {name, start, duration, depth, color}`. Bars are stacked by depth (0 at the top) and scaled to the total
  time range; a label is cut to its bar. Hover sets `value()` to the span index (-1 outside), a click emits
  `Submitted {value = index, text = name}`. `chartFlameSpanRect` / `chartFlameSpanAt` give the geometry.
- **Tabbed docks**: `dockTabify(ui, dock, withDock)` puts `dock` in `withDock`'s region as a tab. The group shows a
  tab strip instead of title bars and only the current dock's body; `dockRaise` (or a click on a tab, Left / Right on
  the strip) changes it, `dockCurrent` / `dockTabBar` / `dockTabRect` query it. `dockTo` takes a dock out of its group.
- **Floating docks**: `dockFloat(ui, dock, rect)` moves the dock into the overlay layer as a panel that is dragged by
  its title bar; `dockTo` docks it again; `dockFloating` / `dockFloatRect` query it. There are no drop zones (a
  floating dock is docked through `dockTo`, not by dragging it onto the area), a floating dock holds no tabs, and it
  is a panel inside the window, not an OS window.
- `dockSaveLayout` adds `tab <id> <group> <index> current|back` and `float <id> <x> <y> <w> <h> open|closed` lines
  only when docks are tabbed or floating; layouts written before still restore, and restoring one un-tabs and docks
  the docks it names.
- **Minimap** (`fuse/ui/game/game_extras.hpp`): `minimap(ui, parent, id)`, `setMinimapRange` (world units that reach
  the frame), `setMinimapHeading(ui, map, degrees, rotateMap)` (heading-up or north-up), `setMinimapShape` (circle /
  square), `setMinimapImage` (rasteriser image index, drawn north-up) and `minimapSetBlips` (`MinimapBlip {pos, color,
  kind}`, position relative to the player, +x east, +y north). Blips beyond the range sit on the frame edge;
  `minimapBlipPos` returns where a blip is drawn.
- **Ammo counter**: `ammoCounter(ui, parent, id)`, `setAmmo(ui, node, inMagazine, magazineSize, reserve)` shows
  "12 / 90" and one pip per round (a bar above `kAmmoMaxPips` = 40), turns to the danger colour at or below 25 % of
  the magazine (`setAmmoLowFraction`), `setAmmoReload(ui, node, progress01)` shows a reload bar. A count change emits
  `ValueChanged`.

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

### Platforms: Android, macOS, iOS, FreeBSD

`fuse_app_native` has one window backend per platform; `NativePlatform` is the same API on all of them.

| Platform | Backend | Present | Entry |
|---|---|---|---|
| Linux (X11), Windows | `App/src/native.cpp` | Vulkan swapchain; GDI on Win32 without Vulkan | `main` |
| FreeBSD (X11) | `App/src/native.cpp` (same backend as Linux; `libX11.so.6` is loaded at run time) | Vulkan swapchain (`VK_KHR_xlib_surface`) when the build finds a Vulkan loader, else the CPU raster presenters only | `main` |
| macOS | `App/src/native_apple.mm` (Cocoa `NSWindow`, `NSTextInputClient` IME, `NSPasteboard`) | Vulkan over a `CAMetalLayer` (MoltenVK) when the build finds a Vulkan loader, else the CPU raster as the layer contents | `main` |
| iOS | `App/src/native_apple.mm` (UIKit `UIWindow`, touch, software keyboard through `UIKeyInput`) + `entry_ios.mm` | same as macOS | `UIApplicationMain`, which runs your `main` |
| Android | `App/src/native_android.cpp` (NativeActivity + NDK app glue, touch / mouse / keys, soft keyboard, clipboard over JNI) + `entry_android.cpp` | Vulkan (`VK_KHR_android_surface`), else the CPU raster through `ANativeWindow_lock` | `android_main`, which runs your `main` |

```cpp
#include "fuse/app/app.hpp"
#include "fuse/app/native.hpp"
#include "fuse/app/main.hpp"   // last: on Android / iOS the platform entry point calls this main

int main(int argc, char** argv) {
    fuse::app::Application app{{.name = "hello", .argc = argc, .argv = argv}};
    fuse::app::NativePlatform native(app);
    auto& window = app.createWindow({.title = "Hello"});
    native.show(window);
    return app.run();
}
```

```cmake
fuse_app_add_executable(hello hello.cpp)   # executable; .app bundle on iOS; libhello.so for NativeActivity on Android
```

```bash
# Android (NDK r26+): libraries, gates and the sample apps as lib<name>.so
cmake -S Source/FUSE/UI -B build-ui-android -G Ninja -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
      -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24 && cmake --build build-ui-android
# macOS (optional Vulkan presenter: brew install molten-vk vulkan-headers vulkan-loader)
cmake -S Source/FUSE/UI -B build-ui-macos -G Ninja && cmake --build build-ui-macos
# iOS simulator
cmake -S Source/FUSE/UI -B build-ui-ios -G Ninja -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphonesimulator \
      -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0 && cmake --build build-ui-ios
# FreeBSD 14 (base clang / libc++; optional Vulkan presenter: pkg install vulkan-headers vulkan-loader)
pkg install cmake ninja && cmake -S Source/FUSE/UI -B build-ui-freebsd -G Ninja && cmake --build build-ui-freebsd
```

Things that differ on these platforms:

- **One OS window** on Android and iOS: the first window shown gets the surface, later ones stay headless.
- **Scale (HiDPI).** The UI works in logical units (dp / points): style, layout, events, hit testing, `rect()` and
  `AppWindow::size()` never see pixels. The frame is rasterised at the display's physical pixel size:
  `UiContext::setRenderScale(s)` makes `drawList()` (and popup surfaces) device pixels = logical x `s`, with glyphs
  shaped at the logical font size and rasterised at font size x `s`, so text is as sharp as the screen. Widgets and
  custom `paint` callbacks keep drawing in logical units. `AppWindow::setScale` / `WindowDesc::dpiScale` set it for a
  window; `image()`, `writePng()` and the presenters then work at `pixelSize()`. On macOS / iOS / Android the backend
  sets the display's scale (`NativeOptions::scale` overrides the factor); X11 / Win32 windows run at scale 1.
  `NativePlatform::scale()` and `safeArea()` report the scale and the area clear of notches and system bars.
- **Resources.** `AppWindow::load("ui/main.fuseui")` falls back to `fuse::app::readResource`, which reads APK assets
  on Android and the bundle's `Resources` on iOS / macOS. Settings live under `fuse::app::dataRoot()` on mobile.
- **Fonts.** Open Sans is embedded in the binary on Android / iOS (`FUSE_TEXT_EMBED_FONT`). The fallback faces
  (Noto, Twemoji) load from `fonts/<vendor path>` in the assets / bundle when you ship them.
- **Lifecycle.** Without a surface (Android `TERM_WINDOW`) the presenter goes headless and comes back with the next
  surface; a paused / inactive app blocks in the event pump and renders nothing.
- **macOS modifiers.** Command reports as `kModCtrl` (Cmd+C / V / Z drive the widgets' shortcuts), Control as `kModSuper`.
- **Touch.** Fingers go through `UiContext::touchDown / touchMove / touchUp / touchCancel` (a mouse on Android keeps
  `pointerDown / Move / Up` and the wheel). A tap clicks. A drag past the slop (8 px, `setTouchScroll`) cancels the
  press (`PointerUp` with `Event::cancel`, no `Click`) and pans the nearest ancestor that can scroll along the drag
  axis 1:1: a scroll container (`overflow: scroll`), or a list / table / grid / text edit / code edit, which receive
  Wheel events in whole rows. A container at its end in the drag direction hands the gesture to its ancestor.
  Releasing in motion flings (velocity from the last 100 ms, exponential decay, stops at the extents, no
  overscroll); `needsUpdate()` / `nextWakeSeconds()` keep the on-demand loop running until it stops, and a new touch
  stops it without clicking. Widgets that own their drags keep them: `WidgetClass::capturesDrag` (`kCapturesDrag`:
  slider, dial, scroll bar, splitter handle, dock title, colour picker, node graph, curve editor, timeline,
  viewport, `kineticScrollArea`, the game sticks / slots) or a per-press `touchDrag` hook (table scroll bar and
  column edges, header sections, video seek bar); `setCapturesDrag(node, true)` does it for one node. On touch a
  drag over a text / code edit scrolls it; selecting by dragging is a mouse feature. Lists and tables select the row
  on the press, so a pan that starts on a row selects it. One finger drives the UI; there is no pinch.
- Popups are overlays inside the window (`osPopups` is X11 / Win32 only). `AndroidManifest.xml` template:
  `Source/FUSE/UI/App/android/`.

These backends are compiled in CI (`fuse-core-android.yml`, `fuse-core-ios.yml`) and have not been run on a device.

### Platform services (plan §4.13)

One platform-neutral API (`fuse/app/services.hpp`, on `AppWindow` / `NativePlatform`), one implementation per
backend, and an honest `false` / no-op where a platform cannot do it. Everything on `AppWindow` also works headless
(the window is then its own window system), which is what the gates drive.

```cpp
fuse::app::NativePlatform native(app);            // scale 0 = the display's (see below)
auto& w = app.createWindow({.title = "Editor"});
w.setMinimumSize({480, 320});
w.setEventHandler({&state, [](void* u, fuse::app::AppWindow& w, const fuse::app::WindowEvent& e) { /* FocusLost, Moved ... */ }});
w.setCloseGuard({&state, [](void* u, fuse::app::AppWindow&) { return !static_cast<State*>(u)->unsaved; }});
w.setFollowSystemTheme(true);                     // light / dark + accent, now and on change
w.setMenu(menuDesc, {&state, &onMenu});           // macOS: the menu bar; elsewhere: an in-window widgets::menuBar
native.show(w);
w.setFullscreen(true);  w.maximize();  w.minimize();  w.restore();  w.setPosition({100, 80});  w.activate();
native.openFileDialog(w, {.title = "Open", .filter = "*.fuseui"}, {&state, &onPath}); // native, else widgets::fileDialog
fuse::app::openUrl("https://example.org");
```

- **Mouse cursors.** A node asks for a cursor with the `cursor` style property (`auto | default | text | pointer |
  ew-resize | col-resize | ns-resize | row-resize | nwse-resize | nesw-resize | move | crosshair | not-allowed | wait |
  none`; `fuse::ui::Cursor`). It is not inherited: `UiContext::cursor()` walks up from the pressed node (a drag keeps
  its cursor) or the hovered one and takes the first `WidgetClass::cursor(ui, node, pos)` hook or `cursor` style that
  is not `auto`. The default theme sets `text` on line / text / code edits, `pointer` on links and `not-allowed` on
  anything `:disabled`; splitter handles, header section edges and table column edges answer through the hook.
  `setCursorOverride(Cursor::Wait)` is the busy cursor. The backends apply it after each event pump: X11 (themed
  cursors through an optional `libXcursor`, else the core cursor font), Win32 (`WM_SETCURSOR`), macOS (`NSCursor`);
  iOS / Android have none.
- **Clipboard.** `show()` installs the system clipboard as the window's `ClipboardHooks`; also
  `NativePlatform::setClipboardText / clipboardText`. X11: the `CLIPBOARD` selection as `UTF8_STRING` (no INCR, and no
  clipboard-manager hand-over when the app exits); Win32: `CF_UNICODETEXT`.
- **HiDPI.** `NativeOptions::scale`: 0 = the display's scale, 1 = physical pixels, anything else forces it. X11:
  `$FUSE_UI_SCALE`, else `Xft.dpi / 96`, else `$GDK_SCALE`, else 1 (one scale for the connection). Win32: per-monitor
  DPI awareness, `GetDpiForWindow`, `WM_DPICHANGED`. The OS window is logical size x scale pixels, the frame is
  rendered at that size (`AppWindow::resizePixels`), pointer positions are divided by the scale and OS popup windows
  use it. `NativePlatform::postClick` and `screenOrigin` stay in window / screen pixels.
- **Window management** is on `AppWindow`: `setTitle`, `setMinimumSize / setMaximumSize`, `setMode` (`maximize`,
  `minimize`, `restore`, `setFullscreen`), `setVisible`, `setPosition`, `activate`, the queries `mode()`, `visible()`,
  `position()`, `active()`, and `WindowEvent`s (focus gained / lost, moved, mode changed, shown / hidden, close
  requested / vetoed, theme changed, scale changed). `tryClose()` is the user's close and asks the close guard;
  `requestClose()` stays unconditional. X11: maximise / fullscreen / minimise / activate are EWMH requests, so without
  a window manager the mode stays as it is. iOS / Android: one fullscreen surface, every request answers "cannot".
- **System theme and displays.** `Application::systemTheme()` holds the light / dark preference and accent the backend
  reports (`NativePlatform::systemTheme()` queries now). Win32: registry + change messages; macOS: effective
  appearance + accent, with notification; iOS: trait collection; Android: night mode (the activity needs
  `android:configChanges="uiMode"`); X11: `$GTK_THEME`, else `gsettings` once, else Unknown, and no change
  notification. `AppWindow::applyTheme` / `setFollowSystemTheme` re-colour the default theme by adding a token sheet
  (`themeTokensCss`). `NativePlatform::displays()` lists bounds, work area, scale and refresh rate where known.
- **File dialogs.** `NativePlatform::fileDialog` is the modal native one: Win32 `GetOpenFileNameW` /
  `GetSaveFileNameW` / `SHBrowseForFolderW`, macOS `NSOpenPanel` / `NSSavePanel`, Linux / BSD `zenity` or `kdialog`
  when installed; otherwise `Unavailable`. `openFileDialog` tries it and falls back to `widgets::fileDialog`.
- **Menus.** `MenuDesc` (titles, items, shortcuts, submenus, separators, enabled, checkable / checked) builds an
  `NSMenu` tree on macOS and the in-window `widgets::menuBar` elsewhere; activation calls the `MenuHandler` with the
  item id either way, and `AppWindow::menuShortcut` handles shortcuts the UI did not consume. The in-window bar shows
  checkable items as `[x]` / `[ ]` and does not grey out disabled items (they do not fire).
- **Shell.** `openUrl` (xdg-open / `ShellExecuteW` / `NSWorkspace` / `UIApplication` / an `ACTION_VIEW` Intent) and
  `revealFile` (Explorer / Finder select the file; Linux / BSD open its folder; false on mobile).
- Without Vulkan an X11 window now shows the CPU raster (`XPutImage`, `PresenterKind::Software`).

The macOS / iOS parts are compiled in CI only; the Win32 parts are cross-compiled with MinGW and have not been run.

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
`:active`, `:selected`, `:drag-over`. Motion and direction properties (`transition`, `enter-animation`,
`exit-animation`, `enter-duration`, `exit-duration`, `direction`, `padding-start`, `padding-end`) are described under
"Interaction" below. `cursor` (see Platform services) is the mouse cursor a node asks for.

### Fonts, text styles and i18n

Text properties: `font-weight` (`normal` / `bold` / 1..1000), `font-style` (`normal` / `italic`), `font-family` (comma
list), `letter-spacing`, `line-height` (`normal`, a multiplier, px or %) are inherited; `text-decoration` (`underline`,
`line-through`, both) is not. Labels, buttons and every widget that paints through `paintString` /
`paintTextLayout` / `measureString` follow them.

```cpp
#include <fuse/text/font_family.hpp>
#include <fuse/text/i18n.hpp>

ui.setCss(title, "font-weight", "bold");                  // synthesised from the context's regular face
ui.setCss(note, "font-family", "\"DejaVu Sans\", sans-serif"); // an installed family, else the context's font
fuse::text::FontFamily brand("Brand");                    // an application family with real faces
brand.add(*fuse::text::Font::fromFile("Brand-Regular.ttf"));
brand.add(*fuse::text::Font::fromFile("Brand-Bold.ttf")); // weight / slant read from the file (OS/2, head)
ui.addFontFamily(brand);                                  // font-family: Brand now uses the real bold
for (const auto& f : fuse::text::systemFonts()) { /* family, style, path, faceIndex, weight, italic, monospace */ }
auto mono = fuse::text::Font::fromSystem("DejaVu Sans Mono", 700, false);

fuse::text::formatPlural("one {# file} other {# files}", "ru", 5); // CLDR category of the locale, '#' = the number
fuse::text::formatNumber("de", 1234567.891, 2);            // "1.234.567,89"
fuse::text::pseudoLocalize("Save {name}");                 // "[Šávé {name} ··]"
```

- A family answers a (weight, italic) request with its nearest real face (CSS weight matching). Without one it
  synthesises: bold smears the rasterised coverage sideways by a fraction of the em (and widens advances), italic shears
  the outline before rasterisation. Both happen where glyphs enter the atlas, so the CPU rasteriser and the GPU glyph
  path draw the same texels, at the render scale. Fallback-chain faces (other scripts, emoji) are not restyled.
- `font-family` names are matched case-insensitively: families registered with `addFontFamily`, then installed
  fonts, then the context's font. `sans-serif` / `system-ui` are the context's font; `serif` / `monospace` pick a
  well-known installed family. `setSystemFontLookup(false)` keeps a context to its own and registered fonts.
- `systemFonts()` scans the platform font directories once (Linux / BSD: XDG data dirs, `~/.fonts`,
  `/usr/share/fonts`, `/usr/local/share/fonts`; macOS / iOS: `/System/Library/Fonts`, `/Library/Fonts`,
  `~/Library/Fonts`; Android: `/system/fonts`; Windows: `%WINDIR%\Fonts`) and reads each file's name / OS/2 tables
  itself: `.ttf`, `.otf` (glyf or CFF) and `.ttc` collections. It is empty, not an error, on a machine without fonts.
  `setSystemFontDirectories` points it elsewhere (a font folder shipped with the app, tests).
- `fontPicker` lists the bundled and generic families followed by the installed ones and previews the chosen face
  with its Bold / Italic boxes. `richText` draws `<b>` / `<i>` in the family's bold / italic face; `markdownView` does
  the same for `**bold**` / `*italic*`, and its headings are bold.
- `letter-spacing` and `line-height` apply to a node's own text (labels, text blocks); `letter-spacing` also to
  strings painted with `paintString`. Text edits ignore both.
- Plural rules cover the common CLDR families (one/other, French, East and South Slavic, Polish, Czech, Slovenian,
  Baltic, Romanian, Arabic, Hebrew, Welsh, Irish, and the no-plural languages); number formatting has a built-in
  separator table and always writes Western digits. `StringTable::trPlural(key, n)` applies both to an entry.
- Not there: variable-font axes (a variable font is its default instance), CFF2 and bitmap-only fonts, dates and
  currencies, a string-extraction tool, cooked `.fusefont` files (`FUSE_UI_TEXT=COOKED` still builds MINIMAL).

## Interaction: drag and drop, gestures, actions, motion, RTL

All of this is in `fuse_ui_core` (`ui.hpp`, `actions.hpp`, `style.hpp`); the widget glue is
`fuse/ui/widgets/interaction.hpp` (included by `widgets.hpp`).

**Drag and drop.** A drag carries a `DragData` (`type` = `dragType("application/x-thing")`, two inline `u64`, an
optional pointer and text). Sources: `ui.setDragSource(node, fn)` (or `WidgetClass::dragStart`) is asked when a press
moves past `GestureConfig::dragThreshold`, or call `ui.beginDrag(source, data, image)` yourself. Targets:
`ui.setDropTypes(node, {types...})`, `ui.setDropFilter(node, fn)` or `WidgetClass::acceptsDrop`. The target gets
`DragEnter / DragMove / DragLeave / Drop` events, has the `:drag-over` style state, and may call
`ui.setDropAction(DropAction::None)` to refuse or `ui.setDropIndicator(rect)` to show where the data lands; the
source gets `DragEnd` with the result (`Move`, `Copy` with Ctrl, `None`). Escape cancels. The ghost (a chip with the
text, a translucent copy of a node, or your draw callback) is painted over everything in the draw list. Touch: a
swipe on a drag source inside something scrollable still pans; a long press picks it up. Platform layers inject OS
file drops with `ui.dropExternal(pos, data)` (`kDragFiles`, paths in `text`), `dragExternalMove` / `dragExternalLeave`
for hover feedback; no backend calls them yet.

```cpp
w::setListDragDrop(ui, list, w::kViewDragDrop);       // reorder rows, drag them to other views, accept theirs
w::setTableDragDrop(ui, table, w::kViewDragReorder);  // model views report: RowMoved {value = from, value2 = to}
ui.connect(table, SignalKind::RowMoved, [](UiContext&, NodeHandle, const SignalArgs& a) { /* move the model row */ });
```

A string `listView` edits its own items; `tableView` / `modelListView` / `gridView` emit `RowMoved`, `Dropped` and
`DragEnded` and leave the model to the application.

**Gestures.** `Event::clicks` counts presses; the second click of a double click sends `DoubleClick` after its
`Click` (unconsumed: `SignalKind::DoubleClicked`), and list / table / grid views treat it as Enter (`Submitted`).
Touch: `GestureTap`, `GestureDoubleTap`, `GestureLongPress` (0.5 s within the slop; timer in the context's time, so
call `update(dt)`); with pointer ids (`touchDown(id, p)` ...) two fingers send `GesturePinch` (`scale`, centre in
`pos`) and `GestureRotate` (`rotation` in radians) with `gesturePhase` Begin / Update / End. The single-finger pan /
fling is unchanged. Listen with a `SignalKind::Event` slot or in a widget class's `onEvent`. No backend feeds pointer
ids yet (Android and iOS pass the primary touch).

**Actions and shortcuts.** `ActionRegistry reg(ui);` then
`reg.add({.id = "file.save", .text = "Save", .shortcut = primaryShortcut(Key::S)}, callback)`. Shortcuts fire on a
KeyDown the focused widget did not consume: Subtree-scoped actions while the focus is inside their node, Window
actions otherwise, none of the window's actions while a modal scope is open, and plain typing keys never while a
text field has the focus. `reg.bind(action, button)` keeps text / enabled / checked in sync and triggers on click;
`w::actionMenuBarMenu(ui, reg, bar, "File", reg.addList({...}))` and `w::actionPopupMenu` build menus that show the
shortcut right-aligned. `parseShortcut("Ctrl+Shift+S")` / `shortcutToString`, `reg.rebind(a, s)` (returns the
conflicting action unless told to steal), `saveShortcuts()` / `loadShortcuts()`. `primaryModifier()` is Cmd on Apple
platforms and Ctrl elsewhere. The registry does not use fuse_core's `ActionMap`; ids and shortcut strings are what an
adapter needs.

**Motion.** `transition: background 0.15s ease-out, border-radius 0.1s` (or `all`) fades animatable properties when
a node's style changes; `enter-animation` / `exit-animation: fade | slide-up | slide-down` with `enter-duration` /
`exit-duration` play when a node first shows / is hidden (popups stay alive for their exit time).
`ui.animateProperty(node, Prop::Width, PropValue::ofLength(Length::px(240)), AnimSpec::springy())` tweens or springs
a style property and leaves the end value inline; `ui.animateKeyframes(...)` runs up to 6 keys. Animated nodes use a
scratch slot of the style table, so nothing is interned or allocated per frame and an idle context sleeps.
`ui.setReducedMotion(true)` turns all of it (and `animate()`) into jumps. The default theme declares no motion; opt
in per node with the classes `.fx` (hover / focus colours fade), `.fx-fade`, `.fx-slide`, or for every popup with a
rule such as `popup { enter-animation: fade; exit-animation: fade; }`. Descendants inherit the cascade's final value
of `color`, not the in-flight one: give them their own `transition` if they should fade too.

**RTL.** `direction: rtl` (inherited; `ltr` switches a subtree back) mirrors Box layouts along x, swaps `left` /
`right` of Anchor children, makes `padding-start` / `padding-end` the right / left padding and flips `text-align:
start / end` (`left` / `right` are aliases of start / end and flip too). Overlay popups keep their screen position.
Widgets that paint their own content (lists, sliders, check marks) are not mirrored inside.

**Hot reload.** `doc::DocumentFileWatcher watch(path);` then `watch.poll(document)` each frame (one `stat`) reloads a
`.fuseui` file when it changes; nodes with an id keep their handle, focus, scroll, value and edit text
(`Document::reload`).

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
| `fuse_text_font_styles` | face description, synthetic bold / oblique at pixel level, atlas keys, `FontFamily` matching (real face preferred), the font scanner over a temp directory (collection, corrupt files, no fonts), CSS font / decoration / spacing properties, render scale 2, `tests/golden/text_styles.png` (`text_styles_full.png` in FULL) |
| `fuse_text_i18n` | CLDR plural categories per rule family, plural patterns, number formatting per locale, pseudo-localisation, `StringTable::trPlural` |
| `fuse_ui_drag_drop` | drag threshold, target by type / filter / modal scope, event order, `:drag-over`, Move / Copy / refuse / Escape, ghost, external drops, touch long-press drag vs pan, list reorder and list / table / grid row drags |
| `fuse_ui_gestures` | double click (time, distance, third click), list activation, tap / double tap / long press, multi-touch ids, pinch and rotate phases |
| `fuse_ui_actions` | shortcut text round trip, scope rules (window, subtree, modal, text fields, disabled), bound widgets, action menus, rebinding conflicts, save / load |
| `fuse_ui_style_motion` | `transition` parsing, fades in context time, idle afterwards, zero allocation / no style-table growth, tween / spring / keyframes, popup enter / exit, reduced motion, static default theme |
| `fuse_ui_rtl_layout` | layout dumps of one tree in `ltr` and `rtl`, logical padding, text alignment, popups unmoved |
| `fuse_ui_hot_reload_file` | file-time poll reloads a `.fuseui` file keeping handles, focus, edit text, value and scroll; broken / missing file |
| `fuse_ui_interaction_golden` | action menu with shortcut texts and a list row drag (ghost, insertion line) against `tests/golden/interaction.png` (MINIMAL profile only) |

| `fuse_app_idle_zero_frames`, `fuse_app_multiwindow`, `fuse_app_hello_startup`, `fuse_app_settings` | UI5 loop, windows, startup ≤ 150 ms, settings / command line / single instance |
| `fuse_app_native_window` | real X11 / Win32 window, Vulkan GPU-draw and CPU-upload presenters vs the CPU raster (skips 77 without a display or device) |
| `fuse_app_cursor_resolution`, `fuse_app_menu_description`, `fuse_app_dpi_scale`, `fuse_app_theme_restyle`, `fuse_app_window_state`, `fuse_app_file_dialog_fallback` | platform services, headless: cursor from styles / hooks / hover / drag, `MenuDesc` -> in-window menu bar, scale detection maths and pixel mapping, theme change -> restyle, window state API + events + close guard, toolkit file-dialog fallback and helper command lines |
| `fuse_app_native_services` | the same on real X11 windows under Xvfb: scale-2 window and popup vs the CPU raster, clicks in window pixels, applied cursors, CLIPBOARD between two connections, position / show / hide / focus / size limits, displays, `zenity` / `xdg-open` helpers (skips 77 without a display) |
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

## Editor shell (UI9)

Configure with `-DFUSE_EDITOR_SHELL=fuse` to build `fuse_editor_fuse`, which is the editor chrome in FUSE UI on the shared Qt-free `EditorController`. Run `ctest -L fuse_editor_ui`. All ten editor panels and the New Project wizard are ported (45 gates, including the Top / Front / Side orthographic views with View > Viewport Layout single / two / quad on Ctrl+Alt+1..3 (`multi_viewport`), the play-mode frame / badge and Maximize Viewport on Play (`play_frame`), the asset browser grid / type filter / breadcrumb / context menu (`asset_browser_polish`), timeline zoom / pan / loop / snap / key jumps (`timeline_nav`), Preferences shortcut rebinding (`shortcut_rebind`), File > Recent Projects (`recent_projects`), Group / Ungroup / Duplicate subtree / Frame Selected (`entity_group`), collider wireframes and drag handles (`colliders`), Entity > Open Script (`open_script`), the Terrain panel (`terrain`), the Navigation panel (`navigation`), Edit > Preferences (`preferences`) and view modes Lit / Unlit / Wireframe / Lit Wireframe and mesh bounds: `view_modes`). `--gpu-viewport` shows the live 3D scene in the viewport, zero-copy through a `ui_vk` external-texture slot (gates `scene_viewport`, `gpu_viewport`), with a Move/Rotate/Scale gizmo drawn as a viewport overlay (`viewport_gizmo`). Status is in [FUSE_EDITOR_QT_FIRST_PLAN.md §11](../plans/FUSE_EDITOR_QT_FIRST_PLAN.md).
