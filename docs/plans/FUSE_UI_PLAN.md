# FUSE UI — GUI and Application Framework Plan

Author: Odin Loch
Status: Draft for decision (nothing in this plan is in tree yet)
Scope: A GUI framework inside FUSE that covers what Qt gives application developers, and more, while staying a game-engine subsystem: same loop, same job spine, same renderer, same ownership rules. One toolkit for game HUDs and menus, world-space UI, tools, the editor, and standalone applications on Windows, Linux, macOS, Android and iOS.

Related: [`FUSE_MASTER_PLAN.md`](FUSE_MASTER_PLAN.md) §A3 (Qt editor policy, "runtime_ui/" slot), [`FUSE_RENDERER_PLAN.md`](FUSE_RENDERER_PLAN.md) §5.3 (UI frame budget), [`../unification/U4-HYBRID-FRAME.md`](../unification/U4-HYBRID-FRAME.md) (hybrid UI layer), [`../editor.md`](../editor.md).

---

## 0. Summary

FUSE already has most of what a GUI toolkit stands on: a fiber job spine, generation-checked handles, frame and pool allocators, a Vulkan render graph with a dedicated UI stage (`frame.ui_composite`, kept out of upscaler / frame-generation history), an action map, input replay, a VFS, a string table, Lua, and a deterministic immediate-mode overlay in Relight. What it lacks is the application half: text input and IME, clipboard, cursors, touch, multi-window, a real text stack, layout, styling, widgets, model/view, and present paths on macOS / iOS / Android.

The plan adds one new layer, **FUSE UI** (`fuse_ui_*` libraries, namespace `fuse::ui`), built on those engine primitives rather than beside them:

- **UI is a dimension.** A project can enable 3D, 2D and UI. A game turns all three on. An application is a project with only the UI dimension, and it idles when nothing changes. An editor is an application that embeds worlds through viewport widgets.
- **One core, two front-ends.** A retained widget tree (Qt Widgets / Qt Quick style) and an immediate-mode API (debug tools, quick HUDs) lower into the same layout, style, event, text and draw-list core.
- **One renderer.** The UI draws through the existing render graph. In a game it is the `FrameUiSource` of the `FrameComposer`. In an app it draws straight to the swapchain with damage tracking. World-space panels render to textures.
- **Engine rules apply.** Handles, not owning pointers. `std::expected`, not exceptions. Jobs, not threads. Zero heap allocations in steady-state frames. A CPU reference for every GPU kernel, with a FLIP gate between them. Deterministic under `FUSE_JOBS_SINGLE_THREAD`.

Work is split into phases UI0–UI10 (Section 9). Of the seven decisions in Section 11, D5 is decided (Qt first for the editor); the other six need your call before UI1 starts.

---

## 1. Goals

1. **Qt-class application capability.** Windows, dialogs, menus, docking, item views with millions of rows, rich text editing, IME, clipboard, drag and drop, accessibility, theming, i18n, declarative UI and a designer.
2. **Game-class performance.** GPU-first rendering, a handful of draw calls per frame, zero steady-state allocations, damage-only redraw, idle at zero CPU / GPU when an app has nothing to do.
3. **One toolkit everywhere.** The same widgets in a game HUD, a pause menu, a 3D in-world terminal, the editor and a standalone tool, on all five platforms.
4. **Lightweight and strippable.** A shipping game that only uses a HUD pays only for the HUD. Every heavy piece (complex-script shaping, accessibility, the designer) is an option.
5. **Testable by construction.** Headless contexts, a CPU reference rasteriser, scripted input by widget id, layout and accessibility tree dumps, golden images — the house testing style applied to UI.
6. **Replace Qt in the editor** when parity is proven (decision D5), so FUSE ships one toolkit and the editor can run on tablets.

### Beyond Qt ("and more")

| Capability | Qt | FUSE UI |
|---|---|---|
| Same widgets inside a running 3D game, composited before present | Separate (QML over GL, or overlay hacks) | Native: the HUD is the `FrameComposer` UI stage |
| World-space / diegetic UI with raycast input | No | `UiSurface` ECS component (UI7) |
| Gamepad and spatial focus navigation | Minimal | First-class, driven by `ActionMap` (UI7) |
| Embedding a live 3D viewport in a panel | `QWindow` container + surface handshake | Zero-copy viewport widget on the same device (UI4) |
| Code generator | `moc`, `uic`, `rcc` | None — C++23 templates and concepts |
| Deterministic replay of UI sessions | No | `InputReplay` + single-thread job mode |
| Per-frame allocation budget | Not measured | Gated at zero in steady state |
| Strip unused widget classes from the binary | Manual | Cooker emits the registration list from the project's `.fuseui` files; the linker drops the rest |
| UI devtools on a running build | Limited (GammaRay, external) | Built-in inspector overlay (UI8) |

### Non-goals

| Not doing | Why |
|---|---|
| A web engine (Qt WebEngine equivalent) | Hundreds of MB; contradicts "lightweight". An optional system web-view widget (WebView2, WKWebView, Android WebView) is listed in UI10 |
| Print support | Rarely needed; PDF export can come later |
| Positioning, Bluetooth, NFC, serial, sensors | Not GUI. Add as platform services only if a product needs them |
| SQL module | `vendor/sqlite` exists; an item-model adapter over a query is enough |
| A second GPU API (OpenGL, Metal, D3D) | Vulkan everywhere (MoltenVK on Apple). The CPU rasteriser is the fallback |
| Subpixel (LCD) text antialiasing | Breaks under transforms, HDR and composition; grayscale AA with good hinting instead |

---

## 2. Where FUSE is today

What the UI layer builds on, and the gaps it has to close. Paths are relative to `Source/FUSE/`.

| Area | In tree | Gap for a GUI framework |
|---|---|---|
| Windows | `fuse::platform::Window` — Win32 HWND, X11 (Xlib), optional GLFW; `WindowWsiKind` = Null / Glfw / Win32 / X11 | One window per process in practice; no Wayland-native, Cocoa, UIKit or Android window |
| Mobile / macOS present | CI compiles `fuse_core` + `fuse_hybrid` with the Android NDK and `fuse_core` for the iOS simulator (both with Vulkan off). `mobile_vulkan_stub.hpp` reports Android `ANativeWindow` WSI and MoltenVK as deferred | No surface, no swapchain, no input on device yet. Required before any app ships on mobile |
| Events | `EventPump`, fixed ring of 32 `PlatformEvent`s; key, mouse, raw delta, resize, focus, close, capture | No text input, no IME composition, no touch / pen. Wheel is encoded as `MouseMove` with Y = wheel delta. No DPI-change, move, minimise, drop or theme events |
| Keys | `Key` enum: letters, digits, F1–F12, arrows, editing keys, one each of Ctrl / Shift / Alt / Super | No punctuation, numpad, media keys, left/right modifiers, scancodes or repeat flag |
| Clipboard, cursors, dialogs | None in product code. `vendor/nativeFileDialogs` exists (Win32 / Cocoa / GTK / Zenity) but is not wired | All needed |
| DPI | Per-monitor-v2 awareness on Windows (`dpi.hpp`), `windowDpi()` | No scale-change notification; other platforms not wired |
| Lifecycle | `lifecycle.hpp` (foreground / background), `surface_loss.hpp` (lost / restored) — stubs called manually | Hook up to real OS callbacks on mobile |
| Input mapping | `ActionMap`, `PlayerController`, gamepad (XInput, Linux evdev), `InputReplay` | Good base for UI shortcuts and gamepad navigation |
| Renderer UI hook | `FrameComposer::setUiSource(FrameUiSource)` — a render-graph callback returning a premultiplied RGBA image, composited after post by `frame.ui_composite`, kept out of upscaler and frame-generation history | This is the game-mode integration point. No UI renderer behind it yet |
| Hybrid UI layer | `hybrid.sprite_layer` kernel: flat-colour rotated quads (48-byte `SpriteQuad`), binary coverage, CPU reference | Placeholder. No text, images, AA or clipping |
| Existing UI code | Relight `overlay::Ui`: deterministic immediate-mode UI, FNV-1a ids, scripted clicks, 5×7 bitmap font, allocation-free steady state | Proves the house style. Becomes the first client of the new immediate front-end |
| Fonts | `vendor/fonts/opensans/OpenSans.ttf` | No rasteriser (no FreeType, no `stb_truetype`), no shaper, no bidi, no line breaking |
| Images | `vendor/stb` (`stb_image`, resize, write); cooked `.fusetex` BCn path | Enough for UI images; add icon atlasing and SVG |
| i18n | `fuse::locale::StringTable` — key lookup with locale → "en" → key fallback, `{0}` formatting | No plurals, number / date formatting or RTL |
| Editor | Qt 6 Widgets shell: `QMainWindow`, `QDockWidget`, menus, trees, spin boxes, line edits, combos, wizards, file dialogs, message boxes. The viewport is a `QVulkanInstance` / `QWindow` child adopted through a 5-step handshake. Measured: changed-region repaint median < 1 ms, full 9-dock repaint 4.1–4.4 ms (`fuse_editor_qt_ui_frame`) | Defines the parity target for UI9 and the performance bar to beat |
| Project format | `project.json` schema 1 with `dimensions.enableUI` | The flag exists with nothing behind it. FUSE UI is what it turns on |

---

## 3. Design principles

1. **One loop, one renderer, one job spine.** No second event loop thread, no second GPU path, no private thread pool. Apps run the engine frame; they just skip frames when nothing is dirty.
2. **Data-oriented retained core.** Nodes live in pooled storage addressed by `fuse::Handle`. Widget behaviour is a registered function table per widget class, not a virtual per node. Per-node draw commands are cached; a frame with no changes copies cached ranges or does nothing.
3. **Separate dirty bits, separate passes.** Style, layout, paint and accessibility each have their own dirty flag and their own pass, so a colour change never re-runs layout and a hover never re-shapes text.
4. **The game thread owns mutation** (as with `fuse::Object`). Workers shape text, decode images, tessellate paths and lay out large independent subtrees, then hand results back through handles and snapshots. Each `UiContext` is bound to one thread; the editor's context can live on the editor UI thread while the game HUD's lives on the game thread.
5. **CPU reference first, GPU second.** Every primitive gets a CPU rasteriser path that is the specification; the Vulkan kernel is gated against it with FLIP (`vendor/nvidia-flip`), the way `hybrid.sprite_layer` and `frame.ui_composite` are today.
6. **Optional and strippable.** Separate libraries, CMake options and build profiles (`hud`, `app`, `editor`). Widget registration is explicit, so unused classes do not link.
7. **Honest stubs.** Any backend that is a placeholder says so in its header and its gate name, per `coding-standards.md`.

---

## 4. Architecture

### 4.1 Libraries and layers

```
L4  Tools / editor    fuse_editor (Qt today → FUSE UI in UI9), fuse_ui_designer, fuse_ui_inspector
L3  Widgets           fuse_ui_widgets (app + editor widgets), fuse_ui_game (HUD / world-space / nav)
L2  Dimensions        Hybrid composer: 3D + 2D + UI;  fuse_app (application shell, UI-only projects)
L1  Shared services   fuse_ui_core (tree, layout, style, events, anim, draw list, CPU raster)
                      fuse_text (fonts, shaping, bidi, line break, glyph cache, text editing)
                      fuse_ui_vk (GPU backend on the render graph)    fuse_ui_script (Lua bindings)
L0  Core              fuse::platform v2 (text input, IME, clipboard, cursors, touch, windows, dialogs)
```

| Library | Depends on | Vulkan? | Notes |
|---|---|---|---|
| `fuse_text` | `fuse_core` | No | Also used for 3D world text and subtitles, so it is not under UI |
| `fuse_ui_core` | `fuse_core`, `fuse_text` | No | Fully headless; CPU raster lives here |
| `fuse_ui_vk` | `fuse_ui_core`, `fuse_renderer` | Yes | Draw list → render-graph passes |
| `fuse_ui_widgets` | `fuse_ui_core` | No | Paints into draw lists only |
| `fuse_ui_game` | `fuse_ui_widgets`, `fuse_ecs`, `fuse_audio` | No | `UiSurface` component, HUD widgets, navigation |
| `fuse_app` | `fuse_ui_vk`, `fuse_ui_widgets`, `fuse_project` | Yes | `Application`, `AppWindow`, on-demand loop |
| `fuse_ui_script` | `fuse_ui_core`, `fuse_script` | No | Lua bindings and binding expressions |
| `fuse_ui_designer` | `fuse_app` | Yes | Desktop tool, built with FUSE UI |

### 4.2 Two run modes, one frame

**Game mode (continuous).** Runs every frame, inside the existing tick from `architecture.md`:

```
1. Pump OS events → UI hit-tests against last frame's layout first
   → events the UI consumes stop there; the rest go to ActionMap / PlayerController
2. Tick worlds and modules; UI tick: timers, animations, bindings, script callbacks
3. UI update: style (dirty nodes) → layout (dirty subtrees; large independent subtrees on jobs)
   → text shaping on jobs, joined at the UI barrier → paint (dirty nodes re-record draw commands)
4. UI snapshot: immutable draw list + damage rects + atlas uploads
5. Render: FrameComposer … post → FrameUiSource records UI passes → frame.ui_composite → frame generation
6. Present
```

**App mode (on demand).** Same steps, but the loop blocks in the platform wait until there is input, a timer or animation deadline, a posted command, or a window event. With nothing dirty, steps 3–6 are skipped: zero frames, zero GPU work. The UI draws directly to the swapchain (no `FrameComposer`, no GPU scene) with damage scissoring, and uses `VK_KHR_incremental_present` where the driver has it. A viewport widget switches its window to continuous mode only while visible and animating.

A per-window `RedrawPolicy { OnDemand, Continuous }` lets a game run its pause menu on demand, or a tool run a live preview continuously.

### 4.3 Threading

| Role | Does | Must not |
|---|---|---|
| Owning thread of a `UiContext` (game thread for HUDs, editor UI thread for the editor) | Tree mutation, events, script callbacks, style, layout commit, paint | Block on workers outside the UI barrier |
| Job workers | Text shaping, glyph rasterisation, image decode, SVG / path tessellation, layout of large independent subtrees, async model fetches | Touch nodes except through handles and the commit queue |
| Render thread (or game thread in v1) | Records `fuse_ui_vk` passes from the immutable UI snapshot | Read the live tree |
| Other threads | Post `UiCommand`s to a context's lock-free MPSC queue (same pattern as `asset::RenderUploadQueue` and `editor::CommandQueue`), drained at the start of the context's next update | Call widget APIs directly |

### 4.4 Data model

- **`UiContext`** — owns one UI tree, its style sheet set, focus state, timers, animations and command queue. Many per process (each window, each world-space panel, the HUD, the editor).
- **Nodes** — pooled storage; hierarchy as parent / first-child / next-sibling handles; a flattened traversal order cached and rebuilt only on structural change. Hot per-node data in SoA arrays (rect, dirty bits, style index, class id, flags).
- **`WidgetClass`** — a registered table of functions (`measure`, `arrange`, `paint`, `onEvent`, `a11y`) plus a typed property block. C++23 concepts turn a plain struct with static functions into a class table; no inheritance, no `moc`.
- **Properties** — one registry (`PropertyId` → type, default, inherited?, animatable?) shared by styles, bindings, animations, the inspector, the designer and Lua.
- **Signals** — `Signal<Args...>` with small inline slot storage; connections are handles and disconnect automatically when the receiving node's generation dies. Cross-thread delivery goes through the command queue (Qt's queued connection).
- **Widget wrappers** — value types holding `{UiContext*, NodeHandle}`, for an ergonomic API without ownership.

Proposed API shape (illustrative, not final):

```cpp
// Retained
fuse::ui::UiContext ui{desc};
auto form = ui.root().add<Column>({.gap = 8, .padding = 12});
auto name = form.add<LineEdit>({.placeholder = ui.str("form.name")});
auto ok   = form.add<Button>({.text = ui.str("common.ok")});
ok.clicked().connect([name](Button&) { submit(name.text()); });

// Immediate (same core, nodes reconciled by stable id)
if (im::window(ui, "Debug")) {
    im::text("{} fps", fps);
    if (im::button("Reload shaders")) reloadShaders();
    im::slider("Exposure", exposure, -4.f, 4.f);
}

// Custom widget: a plain struct, checked by a concept
struct Gauge {
    struct Props { f32 value = 0.f; f32 max = 1.f; };
    static Size measure(const Props&, const Constraints&);
    static void paint(const Props&, const PaintContext&, DrawList&);
    static EventResult onEvent(Props&, const Event&, EventContext&);
};
ui.registry().registerWidget<Gauge>("game.gauge");
```

### 4.5 Layout

| Engine | Covers (Qt equivalent) | Notes |
|---|---|---|
| Box / flex | `QHBoxLayout`, `QVBoxLayout`, Qt Quick `Row` / `Column` | Grow, shrink, basis, gap, align, justify, wrap |
| Grid | `QGridLayout`, `QFormLayout` | Fixed, auto, fractional tracks; spans; form preset |
| Stack | `QStackedLayout` | One visible child, or all overlaid |
| Anchor | Qt Quick anchors, game HUD anchoring | Edge / centre anchors, margins, safe-area insets |
| Absolute | `move()` / `setGeometry()` | Canvas-style placement |
| Flow | `QListView` icon mode, wrap panels | Asset-browser thumbnails |

Rules: measure results cached per (node, constraints); only dirty subtrees re-layout; a subtree with a fixed size from its parent is a layout boundary, so changes inside it never propagate up. Units are logical pixels (dp); snapping to device pixels happens once, at paint. RTL mirrors box and anchor layouts automatically.

### 4.6 Style and theme

- **Style sheets** in a CSS subset (familiar from Qt's QSS): type, `.class`, `#id`, `:hover :pressed :focus :disabled :checked :selected`, descendant and child combinators, custom properties (`--accent`), transitions.
- **Resolution** happens per node on state change, not per frame; the result is an index into a deduplicated table of computed styles.
- **Themes** are sets of design tokens (colour, typography, spacing, radii, elevation, motion). Light / dark follow the OS unless the project pins one. Games ship their own skin; the editor ships a default.
- **Platform look** is a theme, not a native widget. No native-widget wrapping (that is what makes Qt heavy and inconsistent).

### 4.7 Events, focus, navigation, input arbitration

- **Routing:** capture (root → target), target, bubble (target → root), with consume / pass-through. Pointer capture for drags. Hover tracking with enter / leave.
- **Hit testing** against the last committed layout, respecting clips, transforms and `pointerEvents: none` regions.
- **Focus:** focus scopes (dialogs, popups), tab order (explicit or geometric), focus-visible only for keyboard / gamepad.
- **Shortcuts and actions:** `QAction` / `QShortcut` equivalents built on `fuse::platform::ActionMap`, so game actions, UI shortcuts and rebinding share one table and one file format.
- **Gamepad and remote navigation:** spatial navigation (nearest focusable in the pressed direction, with explicit overrides), `ui.accept` / `ui.back` / `ui.tab_next` actions.
- **Touch and pen:** gesture recognisers (tap, double tap, long press, pan, pinch, rotate, fling with kinetic scrolling); pressure and tilt for pen.
- **Drag and drop:** internal (MIME-typed payloads) and OS (files, text, images) where the platform supports it.
- **Game vs UI arbitration:** the UI sees input first; anything it consumes never reaches `PlayerController`. Modal UI can block game input completely, or let selected actions through (e.g. camera look under a radial menu).

### 4.8 Text

The largest single piece of work. Split into `fuse_text` so 3D world text, subtitles and the console use the same stack.

| Part | Approach |
|---|---|
| Font loading | TTF / OTF / collections / variable fonts through `fuse::io` VFS; bundled fonts plus system fonts on desktop |
| Shaping | `ITextShaper` interface. Full profile: HarfBuzz. Minimal profile: cmap + kerning only (Latin, Greek, Cyrillic, CJK; no complex scripts) |
| Bidi | Unicode Bidirectional Algorithm (UAX #9) — SheenBidi, or an in-house implementation gated against the Unicode conformance files |
| Segmentation | Line breaking (UAX #14) and graphemes / words (UAX #29) — libunibreak, or in-house against the conformance files |
| Rasterisation | Small sizes: hinted grayscale coverage bitmaps with subpixel positioning (FreeType). Large, animated-scale or 3D text: MSDF glyphs (msdfgen-style generation at cook or load time) |
| Glyph cache | Atlas pages (bindless when available), LRU eviction, uploads batched through `UploadQueue` |
| Colour emoji | COLR / CPAL (v0, v1) and bitmap emoji tables |
| Fallback | Per-script fallback chains; missing glyphs report once, never per frame |
| Rich text | Spans (font, size, weight, colour, decorations, links, inline icons / images); a small markup for game text (`<b>`, `<color=…>`, `<icon=…>`, typewriter pauses) |
| Layout object | Shaped, line-broken, cached; re-shaped only when text, font or width class changes; eliding |
| Editing | Piece-table buffer, cursors and selections, grapheme- and bidi-aware movement, word / line navigation, undo / redo, IME pre-edit rendering, password and input masks, validators (`QValidator`) |
| Cooked fonts | `.fusefont`: subset + MSDF atlas + metrics, so a shipping game can render text with no runtime rasteriser at all |
| i18n | `StringTable` grows CLDR plural rules, number / date / currency formatting, pseudo-localisation, and a string-extraction CLI (Qt `lupdate` equivalent) |

### 4.9 Rendering

**Draw list** (CPU, no Vulkan types): rounded rects with per-corner radii and borders, linear / radial / conic gradients, analytic box shadows, images and 9-slice images, glyph runs, tessellated paths (fill rules, strokes, joins, caps, dashes), clip rects and rounded clips, opacity / transform groups, backdrop blur, custom-shader quads, and **embedded render-graph textures** (a 3D viewport, a video frame, another UI context).

**`QPainter` equivalent:** a `Painter` that records into a draw list — paths, transforms, text, images, gradients — used by custom widgets, charts and the node-graph editor. Paths are tessellated on job workers and cached per path.

**CPU reference rasteriser** in `fuse_ui_core`: the specification for every primitive; the headless fallback when there is no GPU (replacing `PlaceholderRenderer` for UI); used by every golden test.

**GPU backend `fuse_ui_vk`:**

- One instanced uber-pipeline for shapes (SDF analytic antialiasing in the fragment shader), one for glyphs (coverage and MSDF), one for paths, plus user custom shaders. Instances come from a storage buffer, as `hybrid.sprite_layer` does today.
- Batches break only on clip-layer changes or custom shaders, not on textures (bindless). A typical HUD is a few draw calls.
- **Damage tracking:** the retained tree knows which nodes changed; their old and new bounds become damage rects. The persistent UI image is re-rendered only inside them.
- **Layer caching:** subtrees marked `cache` (or detected as static and expensive) render to an offscreen texture until dirty.
- **Tiers:** UI-T0 = Vulkan 1.1 with classic descriptor sets and texture arrays (older Android devices); UI-T1 = descriptor indexing / bindless. The renderer plan's own T0 requires Vulkan 1.3 with descriptor indexing and buffer device address, and lists mobile optimisation as a non-goal, so UI-T0 deliberately sits below it: a UI-only app must run on devices the 3D renderer does not target. `VulkanDevice` already detects dynamic rendering and descriptor indexing per device, so the tier is chosen at runtime.
- **Colour:** premultiplied RGBA in display space, matching `frame.ui_composite`; a paper-white level for HDR outputs.

**Three integration paths:**

| Path | Used by | How |
|---|---|---|
| Game HUD | Games | `FrameComposer::setUiSource(uiRenderer.frameUiSource(hudContext))`; the HUD-less output stays clean for TAAU / FSR and frame generation. `HybridComposer`'s UI quads are replaced by the UI context |
| App window | Apps, tools, editor | Draw to the acquired swapchain image with damage scissors; no `FrameComposer`, no `GpuScene` unless a viewport widget needs one |
| World-space panel | Games | Render a `UiContext` to a texture; a material on a mesh displays it; input arrives by raycast → UV → UI coordinates |

### 4.10 Animation

Tweens, springs and keyframe tracks on any animatable property; style transitions; enter / exit animations for popups and list items. Each context chooses a time domain: game time (pauses with the game) or real time (pause menus keep animating). Authored sequences can use `fuse_cinematics` timelines through a property track.

### 4.11 Model / view

- **`ItemModel`** — rows, columns, tree parents, roles, and change notifications (inserted, removed, moved, data changed, reset): the `QAbstractItemModel` contract without the `QVariant` cost (typed role accessors).
- **Proxy models** — sort, filter, flatten, group; incremental where possible.
- **Views** — `ListView`, `TreeView`, `TableView`, `GridView`, all virtualised: only visible rows exist as nodes, recycled from a pool. Target: scroll a 1,000,000-row table inside budget.
- **Delegates** — per-column / per-role cell widgets, editors on demand.
- **Async models** — rows fetched on job workers, committed on the owning thread.
- **Engine adapters** — ECS query → model (hierarchy, inspector), VFS directory → model (asset browser), profiler capture → model.

### 4.12 Declarative UI, binding, scripting, hot reload

- **`.fuseui`** — declarative tree (structure, properties, bindings, event → action names). Authoring format per decision D4; cooked to a binary blob so mobile loads with no parsing.
- **`.fusestyle`** — the CSS-subset style sheets.
- **Bindings** — `"{player.health / player.maxHealth}"` compiled to a small expression bytecode (no Lua needed for simple bindings); Lua for anything larger through `fuse_ui_script` on top of `script_bind`.
- **Hot reload** — file-time watching through `fuse::io`; trees are rebuilt with node identity preserved by id, so focus, scroll and text survive a reload.

### 4.13 Platform services (L0 additions)

These go into `fuse::platform`, behind the existing `Window` / `EventPump` design, and benefit games as much as apps.

| Service | Content |
|---|---|
| Events v2 | `TextInput` (UTF-8), `TextComposition` (IME pre-edit, cursor, selection), `MouseWheel` (x / y, high-precision, momentum phase), `Touch`, `Pen`, `KeyDown/Up` with scancode, key, modifiers and repeat, `DpiChanged`, `WindowMoved`, `WindowMinimised/Restored`, `Drop`, `ThemeChanged` |
| Queue | Ring size set at init (default larger than 32) plus a text byte arena; still no heap in steady state; drop counters kept |
| Windows | Multiple windows, each with its own surface and swapchain; parenting and transient popups; borderless / custom-chrome windows with hit-test regions for drag and resize |
| Text input session | Begin / end, caret rectangle for the IME candidate window, virtual keyboard show / hide and type on mobile |
| Clipboard | Text, images, custom MIME types, lazy provision |
| Cursors | System shapes and custom images |
| Displays | Monitors, scale, refresh rate, work area, safe-area insets (notches, TV overscan) |
| Dialogs | Native file open / save / folder (vendored `nativeFileDialogs` on desktop; system pickers on mobile), native message box as an option |
| Shell | Open URL, reveal in file manager, notifications, tray icon (desktop) |
| Theme | Dark / light, accent colour, reduced motion, high contrast, text scale preference |

### 4.14 Accessibility

Each node carries role, name, description, value, states and actions from day one (UI2), maintained as an incremental accessibility tree. Platform adapters (UI Automation, NSAccessibility, AT-SPI, Android accessibility, UIAccessibility) come in UI6 per decision D7. Gates dump the tree as text and compare it, the same way layout dumps are compared.

### 4.15 Game UI

- HUD widgets: bars, radial menus, minimap frame, compass, ammo / cooldown dials, kill feed, pooled floating combat text.
- World-anchored markers that follow entities, clamp to screen edges with off-screen arrows, and fade by distance.
- `UiSurface` ECS component for diegetic and world-space panels, with raycast input and optional depth-tested or always-on-top rendering.
- Dialogue boxes with typewriter reveal, rich tags and voice-line sync; subtitles with speaker colour and background plates.
- Inventory grids with drag and drop between containers; tooltips that follow the gamepad focus.
- Input-device-aware button prompts (keyboard, Xbox, PlayStation, Switch, touch) that swap when the last-used device changes.
- Virtual sticks and buttons for touch.
- Loading and transition screens that keep animating while the world streams in.
- UI sounds through `fuse_audio` (hover, accept, back, error) defined in the theme.

### 4.16 Testing and tooling

| Tool | Purpose |
|---|---|
| `UiTestDriver` | Headless context: click / type / drag / navigate by widget id or path, advance time, dump layout tree, style, accessibility tree |
| Golden images | CPU raster goldens; GPU vs CPU compared with FLIP |
| `InputReplay` extension | Records text, wheel, touch and UI events; replays deterministically under `FUSE_JOBS_SINGLE_THREAD` |
| Budget gates | Allocation counters, CPU / GPU timing, binary size, startup and idle checks in CI |
| Fuzzing | `.fuseui` / `.fusestyle` parsers, text shaping and bidi input, under ASan / UBSan |
| UI inspector | In-process devtools overlay: tree, computed style, layout boxes, damage rects, event log, timings |
| UI designer | WYSIWYG editing of `.fuseui`, built on FUSE UI itself (the Qt Designer equivalent) |

---

## 5. Qt parity matrix

| Qt module | What apps use it for | FUSE equivalent | Phase |
|---|---|---|---|
| Qt Core — event loop, timers | Main loop, `QTimer` | Engine frame + `fuse_app` on-demand loop; UI timers | UI2, UI5 |
| Qt Core — objects, signals, properties | `QObject`, `connect`, `Q_PROPERTY` | `Signal<>`, property registry, handles (no `moc`) | UI2 |
| Qt Core — threads, concurrency | `QThread`, `QtConcurrent`, `QFuture` | Job spine, `JobCounter`, command queues | In tree |
| Qt Core — files, settings, JSON | `QFile`, `QSettings`, `QJsonDocument` | `fuse::io` VFS, cvars, `config/json.hpp`; settings store in `fuse_app` | In tree, UI5 |
| Qt Core — i18n | `tr()`, Linguist | `StringTable` + plurals, formatting, extraction CLI | UI3 |
| Qt GUI — windows, input | `QWindow`, events | `fuse::platform` v2 | UI1 |
| Qt GUI — painting | `QPainter`, `QImage`, `QPainterPath` | `Painter` + draw list + CPU raster + `fuse_ui_vk` | UI2, UI4 |
| Qt GUI — fonts, text | `QFont`, `QTextLayout`, `QTextDocument` | `fuse_text` | UI3 |
| Qt GUI — Vulkan | `QVulkanInstance`, `QVulkanWindow` | Native (FUSE is Vulkan) | In tree |
| Qt Widgets | Every classic control, `QMainWindow`, docks, dialogs | `fuse_ui_widgets` | UI6 |
| Qt Widgets — model/view | `QAbstractItemModel`, views, proxies, delegates | `ItemModel`, virtualised views | UI6 |
| Qt Widgets — style sheets | QSS | `.fusestyle` CSS subset + theme tokens | UI2 |
| Qt Quick / QML | Declarative UI, bindings, animation | `.fuseui`, binding bytecode, Lua, animation core | UI2, UI8 |
| Qt Quick Controls | Touch-friendly themed controls | Same widget set; touch metrics come from the theme | UI6 |
| Qt SVG | Icons, vector art | SVG subset → cooked paths / MSDF icons | UI4 |
| Qt Multimedia | Video playback in UI | Video widget on vendored Theora + `fuse_audio` | UI10 |
| Qt Charts | Plots | `Chart` widget (line, bar, area, scatter, flame graph) | UI6 |
| Qt 3D / Quick 3D | 3D in UI | The engine itself, through viewport widgets and world-space UI | UI4, UI7 |
| Qt Test | Widget tests | `UiTestDriver`, goldens, replay | UI2 |
| Qt Designer / uic | Visual layout | `fuse_ui_designer` | UI8 |
| Qt Accessibility | Screen readers | Accessibility tree + platform adapters | UI2, UI6 |
| Qt Network | HTTP, sockets | `fuse_net` (outside UI scope) | — |
| Qt SQL | Databases | Item-model adapter over `vendor/sqlite` (optional) | UI6 |
| Qt WebEngine | Embedded browser | Non-goal; optional system web view | UI10 |
| Qt Print Support | Printing | Non-goal | — |

### Widget catalogue

| Tier | Widgets (Qt name → FUSE) |
|---|---|
| A — basic | Label, Button (`QPushButton`), ToolButton, CheckBox, RadioButton, Switch, Slider, ScrollBar, SpinBox / DoubleSpinBox, ComboBox, LineEdit, ProgressBar, Dial, Image, Separator, Link |
| B — containers and app chrome | Panel / Frame, GroupBox, ScrollArea, TabView / TabBar, Splitter, Stack, MainWindow, Dock system (dock areas, floating, tabbed docks, saved layouts), MenuBar, Menu, ContextMenu, ToolBar, StatusBar, Dialog, MessageBox, Wizard, Tooltip, Popover, Toast |
| C — model / view | ListView, TreeView, TableView / DataGrid, GridView, Header, delegates, proxies |
| D — rich and pro | TextEdit / PlainTextEdit / CodeEdit, RichText / Markdown view, PropertyGrid (reflection-driven inspector), ColorPicker, FontPicker, DateTimeEdit / Calendar, FileDialog (fallback when no native one), Chart, NodeGraph editor, Curve / Timeline editor, Viewport (embedded world), Video |
| G — game | HUD set, world markers, radial menu, dialogue box, subtitles, inventory grid, prompts, virtual sticks, loading screen |

Every widget ships with: mouse, keyboard, gamepad and touch behaviour; an accessibility role; RTL layout; theme states; a golden; a behaviour test.

---

## 6. Performance budgets (the "lightweight" contract)

Initial targets, revised after UI0 measures baselines. Each one becomes a CI gate.

| Measure | Target | Gate |
|---|---|---|
| Steady-state heap allocations per frame (no structural change) | 0 | `fuse_ui_zero_alloc_steady`, `fuse_ui_vk_zero_alloc` |
| Idle app: frames rendered with no input / timers / animation for 10 s | 0 | `fuse_app_idle_zero_frames` |
| Typical game HUD (~300 nodes), UI CPU per frame, mid desktop | ≤ 0.5 ms | `fuse_ui_bench_hud` |
| Same HUD, mid Android | ≤ 2 ms | `fuse_ui_bench_hud_mobile` |
| HUD GPU time at 1440p | ≤ 0.5 ms (inside the renderer plan's 3.6 ms "UI, headroom") | `fuse_ui_vk_budget` |
| Editor-size tree (~10k nodes): re-layout + repaint of one dirty panel | ≤ 0.5 ms | `fuse_ui_bench_10k` |
| Editor full 9-dock repaint | ≤ 2 ms (Qt baseline measured at 4.1–4.4 ms) | `fuse_editor_ui_frame` |
| 1,000,000-row table, continuous scroll | Frame ≤ 4 ms CPU, constant memory | `fuse_ui_modelview_1m_rows` |
| Hello-world app, launch to first presented frame, desktop | ≤ 150 ms | `fuse_app_hello_startup` |
| Same, mid Android | ≤ 400 ms | `fuse_app_hello_startup_android` |
| Hello-world app, stripped binary, `app` profile, full text | Measure in UI0, then hold within +10 % | `fuse_app_binary_size` |
| `hud` profile with cooked `.fusefont` (no runtime rasteriser or shaper) | Measure in UI0, then hold within +10 % | `fuse_ui_hud_binary_size` |
| Input to presented frame | ≤ 1 frame + present queue | Latency probe on the existing `latency` path |

---

## 7. Repository changes

```
Source/FUSE/
  Text/                         fuse_text           (L1)
  UI/
    Core/                       fuse_ui_core        tree, layout, style, events, anim, draw list, CPU raster, immediate
    Render/                     fuse_ui_vk          GPU backend + Slang shaders (+ GLSL twins, as elsewhere)
    Widgets/                    fuse_ui_widgets
    Game/                       fuse_ui_game
    App/                        fuse_app
    Script/                     fuse_ui_script
    Designer/                   fuse_ui_designer    (desktop tool)
    tests/
Tools/FUSE/Cook/                .fusefont, .fuseui, .fusestyle, SVG → path / MSDF icon cookers
Samples/ui/                     hello_app, notes_app, game_menu, hud_demo, world_panel_demo
docs/ui/                        user guide, widget reference, style reference, porting from Qt
```

CMake options (proposed):

| Option | Default | Meaning |
|---|---|---|
| `FUSE_BUILD_UI` | ON | `fuse_text`, `fuse_ui_core`, `fuse_ui_vk` (with Vulkan) |
| `FUSE_BUILD_UI_WIDGETS` | ON | Widget library |
| `FUSE_BUILD_UI_GAME` | ON | Game UI |
| `FUSE_BUILD_APP` | ON | Application shell and UI samples |
| `FUSE_UI_TEXT` | `FULL` | `FULL` (HarfBuzz, bidi, segmentation, FreeType) / `MINIMAL` / `COOKED` (cooked fonts only) |
| `FUSE_UI_ACCESSIBILITY` | ON (desktop), per D7 | Platform accessibility adapters |
| `FUSE_UI_PROFILE` | `app` | `hud` / `app` / `editor` — selects libraries and defaults |
| `FUSE_PLATFORM_BACKEND` | per D1 | `NATIVE` / `SDL3` |
| `FUSE_BUILD_UI_DESIGNER` | OFF | Designer tool |
| `FUSE_EDITOR_SHELL` | `qt` | `qt` / `fuse` (UI9) |

`project.json` schema 2 (loader keeps accepting schema 1):

```json
{
  "schemaVersion": 2,
  "name": "my_tool",
  "kind": "app",
  "dimensions": { "enable3D": false, "enable2D": false, "enableUI": true },
  "ui": {
    "root": "ui/main.fuseui",
    "styles": ["ui/theme.fusestyle"],
    "theme": "system",
    "redraw": "onDemand"
  }
}
```

`kind` is `game` (default, today's behaviour) or `app`. An `app` project does not create a `GpuScene`, `FrameComposer` or worlds until a viewport widget asks for one.

Present gating: apps get a host-scoped `TrackBHostFeature::AppPresent`, mirroring `EditorViewportPresent`, so they can present without flipping the global Track B unlock.

---

## 8. Third-party candidates

All must pass the licence check in `THIRD_PARTY.md` before vendoring. Keeping every new dependency permissive (MIT, zlib, BSD, Apache-2.0, FTL) keeps a future commercial-licence option open (Section 12).

| Library | Role | Licence (to verify in UI0) | Profile |
|---|---|---|---|
| HarfBuzz | Shaping | MIT ("Old MIT") | `FULL` |
| FreeType | Rasterisation, font tables, colour fonts | FTL (BSD-style) or GPLv2 | `FULL`, `MINIMAL` |
| SheenBidi | UAX #9 bidi | Apache-2.0 | `FULL` (or in-house) |
| libunibreak | UAX #14 / #29 | zlib | `FULL` (or in-house) |
| msdfgen | MSDF glyph and icon generation (cook time; optionally runtime) | MIT | Cooker; optional runtime |
| nanosvg (or similar) | SVG subset import | zlib | Cooker |
| SDL3 | Platform backend (if D1 chooses it) | zlib | Per D1 |
| AccessKit | Accessibility adapters for all five platforms (Rust core, C API) | MIT / Apache-2.0 | Per D7 |
| `vendor/nativeFileDialogs` | Desktop file dialogs | Already vendored | Desktop |
| `vendor/stb`, `vendor/xiph` | Images, video | Already vendored | — |

---

## 9. Phases

Sizes are relative (S < M < L < XL). XL phases are split into sub-milestones when they start. A phase closes only when its gates are green, including ASan / UBSan on touched paths.

### UI0 — Decisions, spikes, baselines (S)

- Record decisions D1–D7 in this file.
- Spikes: (a) HarfBuzz + FreeType building for all five targets, size measured; (b) window + Vulkan surface + present on Android, iOS simulator and macOS through the D1 candidate; (c) an instanced SDF-quad UI pipeline next to `hybrid.sprite_layer`, timed.
- Baselines: stripped sizes and startup of today's `fuse_runtime`; rerun `fuse_editor_qt_ui_frame` for the comparison numbers.
- Skeleton directories and CMake options; `docs/ui/README.md` stub.

**Exit:** decisions recorded; Section 6 targets filled in with measured baselines.

### UI1 — Platform services v2 (L) — benefits games too

- Events v2, key set v2, queue v2 (Section 4.13); wheel gets its own event (the `MouseMove` encoding is mapped for back-compat, then retired).
- Clipboard, cursors, text-input sessions, display info, system theme, native file dialogs, open-URL.
- Multiple windows with their own surfaces and swapchains.
- Backends per D1. Whatever the choice, this phase delivers real surfaces and input on **Android** (`ANativeWindow` → `VkSurfaceKHR`, replacing `androidVulkanWsiStubStatus`), **iOS** and **macOS** (`CAMetalLayer` through MoltenVK), and wires `lifecycle.hpp` / `surface_loss.hpp` to the real OS callbacks.

**Gates:** `fuse_platform_text_input`, `fuse_platform_ime_composition`, `fuse_platform_clipboard_roundtrip`, `fuse_platform_multiwindow`, `fuse_platform_touch_synthetic`, `fuse_platform_dpi_change`, `fuse_platform_present_android` (emulator), `fuse_platform_present_ios` (simulator), `fuse_platform_present_macos`.

### UI2 — UI core, headless (L)

- `UiContext`, node storage, `WidgetClass` registry, property registry, signals, command queue, dirty tracking.
- Layout engines (Section 4.5), scrolling and clipping, hit testing, event routing, focus, shortcuts on `ActionMap`, timers, animation core.
- Style engine and themes; draw list; CPU reference rasteriser for every non-text primitive.
- Immediate-mode front-end; port Relight's `overlay::Ui` onto it as the first client (or prove equivalence and port when stable).
- Accessibility tree data (roles, names, states) — adapters come later.
- `UiTestDriver`.

**Gates:** `fuse_ui_layout_conformance`, `fuse_ui_style_cascade`, `fuse_ui_event_routing`, `fuse_ui_focus_order`, `fuse_ui_cpu_raster_golden`, `fuse_ui_zero_alloc_steady`, `fuse_ui_bench_10k`, `fuse_ui_single_thread_parity`, `fuse_ui_a11y_tree_dump`.

### UI3 — Text (XL) — can run alongside UI2

- `fuse_text` per Section 4.8: loading, shaping, bidi, segmentation, rasterisation (coverage + MSDF), atlas cache, emoji, fallback, rich text, layout object, editing engine.
- `.fusefont` cooker; `StringTable` plurals and formatting; string-extraction CLI.

**Gates:** `fuse_text_shaping_scripts` (Latin, Cyrillic, Greek, Arabic, Hebrew, Devanagari, Thai, CJK, emoji goldens), `fuse_text_bidi_conformance`, `fuse_text_linebreak_conformance`, `fuse_text_edit_ops`, `fuse_text_cache_zero_alloc`, `fuse_text_dpi_{1,1_25,1_5,2}` (twins of `fuse_editor_qt_font_*`).

### UI4 — GPU renderer (L)

- `fuse_ui_vk` per Section 4.9: pipelines, atlases, clips, layers, backdrop blur, custom shaders, embedded render-graph textures, damage tracking, UI-T0 / UI-T1.
- Game path: `FrameUiSource` for `FrameComposer`; `HybridComposer` UI layer moved onto a UI context; `demo_hybrid_hud` shows real text and widgets.
- App path: direct-to-swapchain with damage and incremental present.
- World-space path: UI context → texture.
- SVG / icon cooker.

**Gates:** `fuse_ui_vk_golden` (GPU vs CPU, FLIP), `fuse_ui_vk_validation_clean`, `fuse_ui_vk_zero_alloc`, `fuse_ui_vk_damage`, `fuse_ui_vk_budget`, `fuse_ui_vk_tier0` (no descriptor indexing), updated `fuse_hybrid_frame_vk_golden`.

### UI5 — Application shell (M)

- `fuse_app`: `Application`, `AppWindow`, on-demand loop, redraw policies, popups (OS windows on desktop, overlays on mobile and in fullscreen games), mobile lifecycle (release and rebuild surfaces), settings store, command-line parsing, single instance.
- `project.json` schema 2; `AppPresent` host feature; minimal init path.
- Samples: `hello_app`, `notes_app`, `game_menu`.

```cpp
int main(int argc, char** argv) {
    fuse::app::Application app{{.name = "Hello", .argc = argc, .argv = argv}};
    auto window = app.createWindow({.title = "Hello", .width = 800, .height = 600});
    if (auto loaded = window.ui().load("ui/main.fuseui"); !loaded) {
        return app.fail(loaded.error());
    }
    return app.run();  // sleeps until input, a timer or an animation needs a frame
}
```

**Gates:** `fuse_app_hello_startup`, `fuse_app_idle_zero_frames`, `fuse_app_binary_size`, `fuse_app_multiwindow`, `fuse_app_lifecycle_android`, `fuse_project_schema2`.

### UI6 — Widget library and accessibility adapters (XL)

- Tiers A → B → C → D (Section 5), in that order. Tier A can start on the CPU rasteriser before UI4 lands.
- Dock system with save / restore; model/view with proxies and delegates; property grid driven by the ECS / editor component schema.
- Accessibility adapters per D7.

**Gates:** `fuse_ui_widget_<name>` for every widget, `fuse_ui_modelview_1m_rows`, `fuse_ui_dock_layout_roundtrip`, `fuse_ui_a11y_<platform>`.

### UI7 — Game UI (L)

- Section 4.15 in full: HUD set, world markers, `UiSurface`, gamepad navigation, input arbitration, device prompts, safe areas, time domains, UI sounds, runtime locale switching, subtitles.
- Sample: `hud_demo`, `world_panel_demo`.

**Gates:** `fuse_ui_game_hud_golden`, `fuse_ui_gamepad_nav`, `fuse_ui_world_panel_raycast`, `fuse_ui_input_arbitration`, `fuse_ui_pause_time_domain`, and the HUD-less invariant (`frame.ui_composite` keeps upscaler / FG history clean).

### UI8 — Declarative UI, scripting, tools (L)

- `.fuseui` / `.fusestyle` loaders and cookers; binding bytecode; Lua bindings; hot reload with identity preservation.
- Runtime UI inspector; UI designer; widget-registration list generated by the cooker.

**Gates:** `fuse_ui_markup_roundtrip`, `fuse_ui_markup_fuzz`, `fuse_ui_hot_reload_identity`, `fuse_ui_script_bind`, `fuse_ui_cooked_registration`.

### UI9 — Editor on FUSE UI (XL) — D5 decided: Qt first

Detailed in [`FUSE_EDITOR_QT_FIRST_PLAN.md`](FUSE_EDITOR_QT_FIRST_PLAN.md): the Qt shell is thinned now (items Q1–Q12, on Qt), and this phase is its Stage 3.

- `FUSE_EDITOR_SHELL=fuse` builds the editor chrome with FUSE UI beside the Qt shell; both call the same Qt-free `EditorController`.
- The viewport becomes a zero-copy viewport widget; the `QVulkanInstance` adoption handshake and `QWindow` container go away.
- The editor's `UiContext` runs on the editor UI thread and talks only to `EditorController`; the "UI posts, game thread drains" rule is unchanged.
- Qt is removed only when every `fuse_editor_qt_*` gate has a green `fuse_editor_ui_*` twin, desktop accessibility adapters are in, and you sign off.
- New capability: the editor can run on tablets.

**Gates:** `fuse_editor_ui_*` parity set, `fuse_editor_ui_frame` (≤ 2 ms full repaint), `fuse_editor_ui_live_present`.

### UI10 — Platform extras (M, any time after UI5)

Native macOS global menu bar, tray icons and notifications, optional system web view widget, video widget, IME and accessibility polish on mobile, platform-specific feel (scroll physics, haptics).

### Order

```
UI0 ──┬── UI1 ───────────────┐
      ├── UI2 ──┬── UI4 ──┬──┴── UI5 ──┬── UI6 (B, C, D) ──┬── UI9
      └── UI3 ──┘         │            │                   │
                 UI6 (A) ─┘            ├── UI7             │
                                       ├── UI8 ────────────┘
                                       └── UI10
```

UI1, UI2 and UI3 run in parallel. UI6 Tier A starts on the CPU rasteriser before UI4. UI9 is desktop-only: it needs the desktop parts of UI1, UI4 and UI5, widget tiers A–C plus part of D, and desktop accessibility adapters, but not mobile, UI7 or the designer (see [`FUSE_EDITOR_QT_FIRST_PLAN.md`](FUSE_EDITOR_QT_FIRST_PLAN.md) §7).

---

## 10. How this keeps FUSE a game engine

| Rule | Consequence |
|---|---|
| No second loop | `fuse_app` is the engine frame with on-demand scheduling, not `QApplication::exec` |
| No second renderer | UI draws through the render graph; apps simply skip the scene stages |
| No private threads | Shaping, decode, tessellation and big layouts are jobs |
| Same ownership rules | Handles, allocator domains, `std::expected`, no exceptions, game-thread mutation |
| Same data paths | VFS, cooker, `StringTable`, `ActionMap`, `InputReplay`, Tracy zones |
| Same tests | CPU reference + GPU gate, goldens, sanitizers, single-thread parity |
| Pay for what you use | `hud` profile for games, `app` for tools, `editor` for the full set |

---

## 11. Decisions needed

| # | Decision | Options | Recommendation |
|---|---|---|---|
| D1 | Platform backend for windows, input, IME, clipboard and dialogs | (a) Native per OS: extend Win32 / X11, write Wayland, Cocoa, UIKit, Android. (b) SDL3 everywhere. (c) Keep native Win32 / X11; use SDL3 for macOS, iOS, Android and Wayland; replace with native only where SDL3 falls short | **(c)**. Fastest route to all five platforms with IME, touch, pen, file dialogs and mobile lifecycle; keeps the proven native raw-input game path; SDL3 is zlib-licensed |
| D2 | Text stack | (a) HarfBuzz + FreeType + bidi + segmentation. (b) Minimal shaping only. (c) In-house shaper | **(a) as `FULL`**, with `MINIMAL` and `COOKED` profiles for small games. Correct Arabic / Indic / Thai is table stakes for an application framework |
| D3 | Programming model | Retained only / immediate only / both on one core | **Both on one core** (Section 4.4) |
| D4 | Declarative authoring format | (a) JSON for structure + CSS subset for style. (b) A QML-like DSL | **(a) first.** Reuses the existing JSON parser and tooling; the designer writes it; the cooker binarises it. Revisit a DSL only if hand-authoring becomes the bottleneck |
| D5 | Move the editor off Qt | This reverses master plan §A3 ("Qt replaces GUI") | **Decided (2026-10-29): Qt first.** The editor stays on Qt while the engine and FUSE UI are built, then moves at UI9 behind `FUSE_EDITOR_SHELL` with gate-for-gate parity — [`FUSE_EDITOR_QT_FIRST_PLAN.md`](FUSE_EDITOR_QT_FIRST_PLAN.md) |
| D6 | UI logic language | Lua / another VM / C++ only | **Lua** (already the engine VM) plus binding bytecode for simple expressions |
| D7 | Accessibility | (a) AccessKit (all five platforms; adds a Rust toolchain to builds that enable it). (b) Hand-written native adapters. (c) Defer | **Accessibility tree in UI2 regardless; adapters via (a) as an optional module**, off for `hud` builds, so most builds need no Rust |

---

## 12. Risks

| Risk | Impact | Mitigation |
|---|---|---|
| Scope: Qt is decades of work | Never "done" | Parity tiers ordered by what the editor and a typical tool need; non-goals listed; each tier ships usable |
| Text and IME correctness | Broken input for many languages | Off-the-shelf shaper; Unicode conformance files as gates; IME tested on each platform in UI1 |
| Mobile platforms not presenting yet | Apps cannot ship on Android / iOS | UI1 makes real surfaces and lifecycle a hard exit criterion |
| Two editor shells during UI9 | Double maintenance | Shared `fuse_editor_api`; panel-by-panel switch; Qt removed promptly after sign-off |
| Dependency and size creep | "Lightweight" erodes | Size and startup gates from UI0; profiles; cooker-driven registration |
| Android GPU driver variance | Rendering bugs on some devices | UI-T0 path with classic descriptors; CPU fallback; device matrix in CI once UI1 lands |
| Accessibility adds a Rust toolchain | Build complexity | Optional module; off by default for game profiles |
| Performance regressions | Slower than Qt in places | Every budget in Section 6 is a CI gate |
| Licensing of apps built on FUSE | FUSE is AGPL-3.0, so apps linking `fuse_ui` are covered by it | Fine for your own products; if third parties will build closed apps, decide on a dual-licence or exception before UI5. Permissive-only dependencies keep that open |

---

## 13. First steps

1. Decide D1–D7 (Section 11).
2. UI0 spikes: HarfBuzz + FreeType size on all targets; Android / iOS / macOS present through the chosen backend; SDF-quad pipeline timing.
3. Land the UI1 event and key-set changes behind compatibility mapping (wheel, text input, scancodes) — small, useful to games immediately, and unblocks everything else.
4. Start UI2 headless with layout conformance tests and the `UiTestDriver`, and port Relight's overlay as the first client.
