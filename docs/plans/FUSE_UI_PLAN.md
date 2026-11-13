# FUSE UI — GUI and Application Framework Plan

Author: Odin Loch
Status: In progress — D1–D7 recorded (§11); UI0 skeleton + HarfBuzz/FreeType spike, UI1 desktop input (text input, IME pre-edit, wheel, clipboard; Win32 + X11), UI2 headless core + `.fuseui` documents + Lua bindings, UI3 MINIMAL text + FULL profile (HarfBuzz/FreeType, bidi, UAX #14, shaped-run cache), UI4 GPU draw-list backend (Vulkan compute, lavapipe parity, `FrameUiSource`), UI5 `fuse_app` shell with native X11/Win32 windows and a Vulkan swapchain presenter, UI6 Tier A/B/C widgets plus Tier D grid, property grid, colour picker, calendar, chart and Markdown view, delegates, sort/filter proxy, dock save/restore and the 1M-row gate, UI7 game UI (`fuse_ui_game`), the Relight dev menu on FUSE UI, the rest of UI6 Tier D, the SQL model, the a11y bridge with optional AccessKit, UI10 platform feel and a UI8 designer prototype landed 2026-10-29. Code: `Source/FUSE/UI/`; usage: [`../ui/README.md`](../ui/README.md).
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
| Qt Widgets — model/view | `QAbstractItemModel`, views, proxies, delegates | `ItemModel` (roles, hierarchy, notifications), virtualised table / list / tree / grid views, `SelectionModel`, editor delegates, sort / filter / tree-filter proxies, `AsyncModel` | UI6 |
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

### Status (2026-11-12)

| Phase | State | Notes |
|---|---|---|
| UI0 | **Partly done** | D1–D7 recorded (§11). Skeleton `Source/FUSE/UI/{Text,Core,Widgets,Doc,Script,Render,tests}` with the options from §7. Spike (a) done on Linux x86-64: HarfBuzz 10.1.0 + FreeType 2.13.3 via FetchContent build with the conda GCC toolchain; stripped `fuse_ui_sample` grows from 0.44 MB (MINIMAL) to 2.46 MB (FULL), static libs 1.86 MB (HarfBuzz) + 1.20 MB (FreeType). Not done: HarfBuzz + FreeType on the other four targets (only Linux was built and measured), spike (c) SDF-quad timing, §6 baselines. Spike (b) (mobile surfaces through the D1 candidate) was not run: native Android / Apple window backends were written instead (see the Mobile / macOS row; compiled in CI, not run on a device). |
| UI1 | **Desktop input done (Win32 + X11)** | `events_v2.hpp` (KeyV2/EventV2, lossless v1 mapping). v1 `PlatformEvent` gains real `MouseWheel` (1/120 notches, both axes), `TextInput` (UTF-8 per code point) and `TextComposition` (IME pre-edit + caret); the wheel-as-mousemove tag is retired (still decoded for old recordings). Win32: `WM_MOUSEWHEEL/HWHEEL`, `WM_CHAR` with surrogate pairing, IMM32 pre-edit on owned windows. X11: `XLookupString` text, buttons 4–7 as wheel. `fuse/platform/text_services.hpp`: clipboard (Win32 `CF_UNICODETEXT`, process-local elsewhere), begin/end text input and candidate-window placement. `InputState` exposes `wheelX/Y` and per-frame `textInput()`. UI side: `UiContext::textComposition`, clipboard hooks, `wantsTextInput`/`textCaret`; LineEdit draws the pre-edit inline and does Ctrl+C/X/V; `platform_bridge.hpp` feeds v1 or v2 events and syncs IME placement. Gates: `fuse_core_ui1_text_input`, `fuse_ui_text_input`, X11 window gate under Xvfb. X11 XIM pre-edit + CLIPBOARD selection (`fuse_core_ui1_x11_text_services`) and the Win32 paths verified under Wine. Left open by this phase and done later: DPI detection on X11 / Win32 and window management (Platform services row: gates `fuse_app_dpi_scale`, `fuse_app_window_state`, `fuse_app_native_services`), touch input and the mobile backends (Mobile / macOS row: `UiContext::touchDown / Move / Up`, Android / iOS backends; compiled, not run on a device). Not done: a gate with several native windows at different scales (per-monitor scale is Win32 only, see Platform services). |
| UI2 | **Gates green** | `fuse_ui_core`: handle-based retained tree, CSS-subset cascade with `:root` tokens, Box/Grid/Stack/Anchor/Absolute/Flow layout with measure cache + layout boundaries, draw list + CPU reference rasteriser, capture/target/bubble events, focus/tab/spatial nav/modal, signals, timers, animations, `post()` command queue, accessibility tree dump, an immediate-mode front-end (`immediate.hpp`), and an overlay `layer` class for popups. `fuse_ui_doc`: `.fuseui` JSON documents with `{expr}` / `{=two-way}` bindings compiled to stack bytecode (D4). `fuse_ui_script`: Lua `ui` table over a context and model (D6). Gates: the nine core gates (`fuse_ui_bench_10k` median ≈0.30 ms, budget 0.5 ms), `fuse_ui_doc_load`, `fuse_ui_lua_bindings`. |
| UI3 | **MINIMAL + FULL** | `fuse_text` behind `ITextShaper`: stb_truetype, bundled Open Sans, UTF-8, kerning, wrap/ellipsis, glyph atlas, `TextEditBuffer`. `FUSE_UI_TEXT=FULL` fetches HarfBuzz + FreeType: `HarfBuzzShaper` (ligatures, GPOS, marks, complex scripts), SheenBidi levels with per-direction runs and L1/L2 reordering per line, UAX #14 line breaking via libunibreak, and a fixed-capacity shaped-run cache. UAX #29 extended grapheme clusters (`prevBoundary`/`nextBoundary`/`graphemeCount`: combining marks, CR LF, Hangul, Prepend/SpacingMark, RI flags, emoji modifiers, ZWJ sequences, GB9c Indic conjuncts) drive caret moves, selection, Backspace and Delete in `TextEditBuffer` and the text widgets (gate `fuse_text_edit_ops`). Font fallback chains (`Font::setFallbacks`, per-face runs at cluster granularity, em-matched sizes; FULL re-shapes fallback runs per line with HarfBuzz) with bundled Noto Sans Hebrew / Arabic / Devanagari / Symbols 2 and Twemoji COLRv0 + CPAL colour emoji painted as palette-coloured layers through the ordinary Glyph op, so the CPU and GPU backends are unchanged; `fuse_app`'s default font carries the chain (gate `fuse_text_fallback`); all UI gates pass in both profiles (`golden/sample_full.png`, `golden/hud_full.png`). Gate: `fuse_ui_text_full`. Font families and styles: `FontFamily` (real faces by CSS weight / slant matching, registered per context with `UiContext::addFontFamily`), synthetic bold (coverage emboldening with wider advances) and synthetic oblique (outline shear) made in the glyph rasterisation path of both profiles, so the atlas the CPU and GPU glyph paths sample carries them, at the render scale; CSS `font-weight`, `font-style`, `font-family`, `text-decoration` (underline / line-through from the post and OS/2 metrics), `letter-spacing`, `line-height`; `richText` and `markdownView` draw bold / italic in those faces. Installed fonts: `systemFonts()` scans the platform font directories of Linux / BSD, macOS / iOS, Android and Windows and reads the sfnt name / OS/2 / head / post tables itself (`.ttf`, `.otf`, `.ttc`; no fontconfig / CoreText / DirectWrite), `Font::fromSystem`, `FontFamily::fromSystem`, `font-family` resolution with fallback to the context font, and the font picker lists them. i18n (`fuse/text/i18n.hpp`): CLDR cardinal plural categories for the common rule families, ICU-style plural patterns, number formatting from a built-in separator table, pseudo-localisation, `StringTable::trPlural`. Gates: `fuse_text_font_styles` (with `golden/text_styles[_full].png`), `fuse_text_i18n`. **Text port (2026-11-13, from `claude/magical-bassi-815825`):** `FUSE_UI_TEXT=COOKED` is real (`.fusefont` packs, `fuse_text_cook` / `cookFont`, a `fuse_text` without font parser or rasteriser; the build cooks the bundled fonts; the whole suite runs in that profile); MSDF glyphs (`GlyphAtlas::getMsdf`, `UiContext::setMsdfThreshold`); exact COLRv1 and CBDT / sbix colour glyphs (`enableColorGlyphs`, `GlyphAtlas::getColor`); variable fonts (`enableVariableFonts`: fvar / avar / gvar / HVAR / MVAR, `Font::instance`, named instances) with CSS `font-stretch` and `font-variation-settings`; `font-feature-settings` (FULL); the branch's `FontCatalog` folded into `font_family.hpp` (`Script`, `SystemFontInfo::covers`, `systemFallbackChain`, generic families, named-instance faces, `loadSystemFont`; Windows registry list); generated UAX #29 tables (Unicode 15.1), cmap / kerning caches, FULL fallback runs shaped once inside the bidi pass; Thai and a CJK subset in the bundled fallbacks; `fuse/text/i18n.hpp` now a front end to the `fuse_ui_l10n_lib` tables. Gates `fuse_text_{grapheme,linebreak,bidi}_conformance`, `_shaping_scripts`, `_system_fonts`, `_cache_zero_alloc`, `_colrv1`, `_bitmap_emoji`, `_msdf`, `_dpi_{1,1_25,1_5,2}`, `_variable`, `_features`, `_cooked`, `_bench_layout`. Not done: the GPU kernel / packer for colour-image and MSDF glyphs (the packer skips them; CPU rasteriser only), CFF2, `ital` / `slnt` axes for italic, animatable `font-weight`, restyling of fallback faces under a synthesised bold, shaping beyond cmap + kerning and bidi in COOKED, a host cooker for cross-compiled COOKED builds, the macOS / iOS / Android / Windows font directories and registry list exercised on a device. MINIMAL stays the default. |
| UI6 | **Tier A + Tier B** | Tier A: button, toolbutton, link, checkbox, radio, switch, slider, scrollbar, dial, progressbar, spinbox, combobox, lineedit, image, separator, label; default dark theme. Tier B: overlay layer, popup menu with cascading submenus and screen clamp, combo popup list, menu bar, modal dialog, tooltip, virtualised list view, tree view, tab widget, scroll area with scrollbars, splitters, dock area, item models and table view. Tests: `fuse_ui_widget_interaction`, `fuse_ui_tier_b`, `fuse_ui_tier_b2`. Then: cell delegates, `SortFilterProxy`, `gridView`, `propertyGrid`, `colorPicker`, `calendar`, `chart` (line/bar/area/scatter), `markdownView`, `dockSaveLayout`/`dockRestoreLayout`. Gates: `fuse_ui_dock_layout_roundtrip`, `fuse_ui_modelview_1m_rows` (1M rows: median 0.04 ms/frame, ~116 cell reads/frame, constant node count), `fuse_ui_tier_c`, `fuse_ui_tier_d`. Tier D completed 2026-10-29: `fontPicker`, `fileDialog`, `nodeGraph`, `curveEditor`, `timeline`, `viewportWidget`, `codeEdit`, `dateTimeEdit`, `videoWidget` (frame-source stub; no Theora decode yet), gate `fuse_ui_tier_d2` with one ctest per widget. `fuse_ui_sql`: `SqliteModel` (`ItemModel` over a query on vendored SQLite, windowed LIMIT/OFFSET cache; gate `fuse_ui_sql_model`, 200k rows at 0.04 ms/frame). `fuse_ui_a11y`: snapshot diff bridge plus optional AccessKit (AT-SPI `UnixAdapter`) when `FUSE_UI_ACCESSKIT_DIR` points at a prebuilt accesskit-c (0.22.3 was used: its HEAD needs Rust 1.87); gate `fuse_ui_a11y_bridge`. Not done: Windows / macOS / Android AccessKit adapters wired to native windows (they need a Rust toolchain and the devices). Theora decode has since landed (`fuse_ui_video`: `video::TheoraVideo`, vendored libtheora + libogg, gate `fuse_ui_theora_video`); the accessibility tree itself was completed later (Finishing row). **Model / view (§4.11, `fuse/ui/widgets/modelview.hpp`)**: `ItemModel` grew optional typed roles, flags, `setData`, a hierarchy over opaque ids, lazy children and a `ModelNotifier` (rows inserted / removed / moved, data changed, reset: views keep selection and scroll on the same rows); `tableView` / `modelListView` / `treeModelView` are one virtualised view class (`treeModelView` tracks only expanded nodes; tree table with more columns; the string `treeView` lost its 64-row collapse limit); `SelectionModel` (range list; single / multi / extended, rows or cells, shareable, current vs selected) for table, list, grid and tree; in-place editing (line edit / spin box / combo box delegates, check boxes in place, Enter / Escape / Tab, one editor node); horizontal scrolling, frozen / hidden / reorderable columns, resize to contents, row height, elided-cell tooltips, context-menu hook; `SortFilterProxy` multi-key stable sort, all-column and wildcard filters, proxy over proxy; `TreeFilterProxy`; `AsyncModel` (placeholder rows, pages delivered from any thread through `UiContext::post`, committed on the owner thread). Gates: `fuse_ui_mv_notify`, `_selection`, `_selection_1m` (Shift+End over 1M rows: 1 range, 0 allocations), `_tree`, `_tree_1m` (> 1M-node lazy tree: constant node count, ~58 cell reads / frame, median ~0.04 ms), `_editing`, `_view`, `_proxy`, `_async`, `_dir_model` (lazy `std::filesystem` model, tests only), goldens `fuse_ui_mv_golden_tree` / `_table_edit`. Not done (model/view): `SqliteModel` behind `AsyncModel` (needs a worker-owned connection), `fileDialog` on the directory model, ECS / VFS / profiler engine adapters in the library, word-wrapped or variable-height rows, row drag-and-drop on `treeModelView` (tables and model lists have it through `setTableDragDrop`; a drag carries the pressed row, not a multi-selection), persistent indexes (a tree selection is in visible rows), incremental proxy updates (a filtered / sorted proxy rebuilds on a source change). |
| Relight port | **Done** | `dev_menu.cpp` runs on `FuseMenuUi` (FUSE UI immediate mode) with glyph emission into the overlay rasteriser; `fuse_ui_relight_equivalence` and `rl_overlay_glyphs` gate it. The Wine golden `rl_overlay_shown_*` may need a refresh on Windows (not verifiable here). |
| UI4 | **Draw-list backend + presents** | `fuse_ui_vk`: `ui.draw_list` compute kernel (rects, AA, gradients, shadows, glyph coverage, scissor) and `UiLayerGpu` render-graph pass; `fuse_ui_vk_parity` matches the CPU rasteriser bit-exactly on lavapipe natively and under Wine with zero steady-state allocation. Game path: `UiFrameSource` feeds draw lists to the runtime UI stage as a `frame::FrameUiSource` (`fuse_scene_renderer_vk_fuse_ui_t0`). App path: the UI5 swapchain presenter. Backdrop blur: `DrawOp::Backdrop` / `backdrop-blur` (3× box ≈ Gaussian, rounded + clipped, optional tint) in the CPU reference, gate `fuse_ui_backdrop_blur`; the GPU tile kernel skips it for now (panel drawn unblurred; needs a blur pass between kernel segments). Not done: SDF-quad raster path, GPU backdrop-blur pass, custom shaders, damage-driven incremental GPU present, `demo_hybrid_hud` on a UI context, SVG/icon cooker. |
| UI5 | **Desktop shell done** | `fuse_app`: `Application` / `AppWindow`, on-demand loop (sleeps until input, timer or animation), redraw policies, settings store, command line, single instance, headless presenter; `fuse_app_native`: real OS windows (X11 via runtime-loaded libX11, Win32) with a Vulkan swapchain presenter that either draws the draw list on the GPU or uploads the CPU raster (GDI fallback on Win32), resize / out-of-date handling. Samples `hello_app`, `notes_app`. Gates: `fuse_app_idle_zero_frames`, `fuse_app_multiwindow`, `fuse_app_hello_startup`, `fuse_app_settings`, `fuse_app_native_window` (GPU and CPU-upload presents match the CPU raster with max error 0 on Xvfb + lavapipe and under Wine). OS-window popups (`NativeOptions::osPopups`, `UiContext::setOsPopups` / `popupSurfaces`): menus, submenus and combo lists become borderless OS windows (X11 override-redirect, Win32 `WS_POPUP` + `WS_EX_NOACTIVATE`) that can extend past the main window and clamp to the screen work area; gate `fuse_app_os_popups` (popup pixels match the CPU raster with max error 0, and a real click posted to the popup window picks an item outside the main window; Xvfb and Wine); a click outside every app window closes popups (X11 `XGrabPointer` with owner events, Win32 deactivation / out-of-bounds capture). Not done: `project.json` schema 2 / `AppPresent`, the binary-size gate, Android / macOS gates that run on a device (`fuse_app_lifecycle_android`). Since then: the `game_menu` and `widget_gallery` samples (Finishing row), and Android / macOS / iOS window backends with surface-loss / pause handling (Mobile / macOS row; compiled in CI, not run on a device). |
| UI7 | **Landed (core)** | `fuse_ui_game`: HUD bar with damage trail, cooldown dial, compass, radial menu, kill feed, pooled floating text, world markers (edge clamp, off-screen arrows, distance fade), `UiSurface` world panels with raycast input and damage-driven re-render, `GamepadNav` (delay + repeat, A/B), `InputArbiter`, device-aware prompts, safe areas, `GameClock` time domains, `StringTable` locale switching, subtitles and typewriter dialogue, UI sound hook. Samples `hud_demo`, `world_panel_demo`. Gates: `fuse_ui_game_hud_golden`, `fuse_ui_gamepad_nav`, `fuse_ui_world_panel_raycast`, `fuse_ui_input_arbitration`, `fuse_ui_pause_time_domain` (native, FULL and Wine). The extras first deferred here have landed in `game_extras.cpp` and the bridges: inventory drag and drop (`fuse_ui_inventory_dnd`), touch virtual sticks / buttons (`fuse_ui_touch_controls`), loading / transition screens (`fuse_ui_loading_screen`), rich dialogue with tags and voice-line timing (`fuse_ui_rich_dialogue`), sound themes (`fuse_ui_sound_theme`; `fuse_audio` playback through `fuse_ui_game_audio`, in tree only: `fuse_ui_ui_audio`), `UiSurface` as an ECS component with a depth-test flag (`fuse_ui_game_ecs`, in tree only: `fuse_ui_ecs_surface`), minimap and ammo counter. Not done: the HUD-less `frame.ui_composite` invariant gate; the in-tree bridges are not built by the standalone UI project. (The rich dialogue's body class is `dialoguetext`: it was registered as `richtext`, the name of the widget library's rich text view, and whichever was registered last replaced the other in a context that had both.) |
| UI8 | **Designer (interactive)** | `fuse_ui_designer_lib` + app `fuse_ui_designer`: `DesignDocument` (lossless `.fuseui` load/save fixed point, keeps bindings, actions and unknown `x-*` editor keys, insert/remove/move/set-property with undo/redo) and a `Designer` UI (palette, outline, design-mode canvas that selects instead of operating widgets, property grid, live preview rebuild). Drag-to-place from the palette and within the canvas (drop target = container + child slot, one undo step), Ctrl/Shift multi-select with group delete / move, a Style sheet panel (document `style`, Apply = one undo step) and per-node style rows in the property grid. Gate `fuse_ui_designer_roundtrip` (`--out` writes `designer.png`). The UI inspector existed already. The cooker's widget-registration list exists (`fuse_ui_cook`, `registrationList` / `registrationSource`, `widgets::installClasses`, gate `fuse_ui_cooked_registration`) and now covers document classes (Finishing row). Not done: the designer palette lists a fixed set of classes, not every document class. |
| UI10 | **Partly done** | `platform_feel.hpp`: haptics hook, per-platform scroll profiles, `KineticScroller` (fling, rubber band, spring back) and `kineticScrollArea`; gate `fuse_ui_platform_feel`. The macOS menu bar is built from `MenuDesc` (see Platform services; compiled in CI only). Not possible here: system web view, mobile pickers. |
| UI9 | **In progress (FUSE shell beside Qt)** | `-DFUSE_EDITOR_SHELL=fuse` builds `fuse_editor_fuse` on the Qt-free `fuse_editor_controller` / `EditorPanels`: menus, toolbar and per-action shortcuts from the action table, three tabbed docks, all ten panels plus Navigation and Terrain, New Project wizard, Preferences with shortcut rebinding, view modes, transform gizmo, play-mode frame and Maximize on Play, Single/Two/Quad viewport layouts with ortho framing, and the zero-copy live 3D viewport (`--gpu-viewport`). 45 twin gates (`ctest -L fuse_editor_ui`) pass on X11 and under Wine. Qt stays the default shell until sign-off. Details: [`FUSE_EDITOR_QT_FIRST_PLAN.md`](FUSE_EDITOR_QT_FIRST_PLAN.md) §11. |
| Mobile / macOS (2026-11-12) | **Compiles; not run on devices** | `fuse_app_native` window backends for Android (`native_android.cpp`: NativeActivity + NDK app glue, `VK_KHR_android_surface` or `ANativeWindow_lock`, touch / mouse / keys, soft keyboard, JNI clipboard), macOS (`native_apple.mm`: Cocoa, `NSTextInputClient` IME, `CAMetalLayer` + MoltenVK or layer contents) and iOS (same file: UIKit, `UIKeyInput`, `UIApplicationMain` entry). These are native backends, not the SDL3 route D1 recommended for these platforms: no new dependency, and the vendored SDL is 2.x. `fuse/app/main.hpp` gives one entry function everywhere (`int FUSE_APP_MAIN(int, char**)`: `main` on desktop, `fuse_app_main` under the platform entry point on Android / iOS); `fuse_app_add_executable` builds the app target per platform. Portable `fuse::text::fromChars` / `toChars` (libc++ lacks / gates the floating-point `<charconv>` overloads), embedded default font, asset / bundle resource and font readers, `dataRoot`. CI: `fuse-core-android.yml` and `fuse-core-ios.yml` build `Source/FUSE/UI` for Android arm64, macOS (also runs the headless gates) and the iOS simulator. Render scale (HiDPI): `UiContext::setRenderScale` keeps layout / input in logical units and emits the draw list in device pixels with glyphs rasterised at font size x scale; `AppWindow::setScale` / `pixelSize()`; the Android, macOS and iOS backends render at the display's pixel size (gates `fuse_ui_render_scale`, `fuse_app_render_scale`). Touch input: `UiContext::touchDown/Move/Up` pan scroll containers and wheel-scrolled widgets with a kinetic fling (gate `fuse_ui_touch_scroll`); the Android and iOS backends feed it. Not done: running on a device or simulator (the HiDPI path of the Apple and Android backends is compiled / reviewed only), mobile IME composition, AccessKit adapters, an APK or an Xcode-built bundle (the Gradle template, plists and bundle rules of the Finishing row exist but were never run). |
| Platform services (2026-11-12) | **Done on X11 (run under Xvfb); Win32 cross-compiled; macOS / iOS / Android compiled or reviewed only** | §4.13 services behind one API (`fuse/app/services.hpp`, `AppWindow`, `NativePlatform`). Mouse cursors: `cursor` style property + `WidgetClass::cursor` hook resolved by `UiContext::cursor()`, theme rules, backends X11 (optional `libXcursor`, else cursor font) / Win32 / macOS. Clipboard in `fuse_app_native`: X11 `CLIPBOARD` + `UTF8_STRING`, Win32 `CF_UNICODETEXT`. Desktop HiDPI: X11 `Xft.dpi` / env, Win32 per-monitor DPI + `WM_DPICHANGED`, scaled OS popups. Window management on `AppWindow` (title, size limits, mode, show / hide, position, activate, events, close guard) with X11 (ICCCM / EWMH), Win32 and macOS backends. System theme + accent (Win32, macOS, iOS, Android; X11 best effort) with `setFollowSystemTheme`, display list, `openUrl` / `revealFile`, native file dialogs (Win32, macOS, `zenity` / `kdialog`) with toolkit fallback, `MenuDesc` -> `NSMenu` on macOS / in-window menu bar elsewhere, Android dead keys and repeated keys. X11 without Vulkan shows the CPU raster (`XPutImage`). Gates: `fuse_app_cursor_resolution`, `fuse_app_menu_description`, `fuse_app_dpi_scale`, `fuse_app_theme_restyle`, `fuse_app_window_state`, `fuse_app_file_dialog_fallback`, `fuse_app_native_services` (Xvfb). Not done: Wayland; clipboard-manager hand-over, per-monitor X11 scale, EWMH state without a window manager; Android pointer icons, IME string commits (`ACTION_MULTIPLE` + `KEYCODE_UNKNOWN`), Storage Access Framework picker (all need Java); iOS document picker; custom image cursors; a tray icon in `fuse_app` itself (it is in `fuse_ui_extras`); disabled / checked item visuals in the in-window menu. (XSETTINGS, INCR and clipboard formats, multi-file / colour / message dialogs and native notifications came with the platform-layer port, next row.) The Win32 code has not been run (no Wine here); the Objective-C++ code has not been compiled here. |
| Platform layer port (2026-11-13) | **Done on X11 (run under Xvfb, no window manager); on Wayland the clipboard formats, primary selection, window chrome, synthetic input and frame pacing work (headless weston / in-process compositor), tool windows, drag out and XSETTINGS do not; Win32 compiled and linked with MinGW-w64, never run; macOS / iOS / Android: the API answers "cannot"** | The app platform layer of the two merged branches on main's services API (`docs/ui/archive/PORT_magical_bassi.md` 3.5 / 3.7 / 3.8, `PORT_gui_framework.md` 3.1 / 3.5); one API, no parallel layer: `fuse/app/platform.hpp` and `clipboard.hpp` are compatibility headers over `services.hpp` / `native.hpp`. **Clipboard formats**: `ClipboardItem` (text, `image/png` as a straight-alpha `Image`, any MIME type, lazy `provide`), `NativePlatform::setClipboard / setClipboardImage / clipboardImage / clipboardData / clipboardFormats / clipboardOwned`, PRIMARY text; X11 selections with `TARGETS`, INCR both ways; Win32 `CF_DIBV5` + `PNG` + registered formats. **Window-less dialogs** with `std::expected<..., PlatformError>`: `openFileDialog` (filters, multiple), `saveFileDialog`, `pickFolderDialog`, `colorDialog`, `messageBox`, `revealInFileManager` (zenity / kdialog; `IFileDialog`, `ChooseColorW`, `MessageBoxW`). **OS drag and drop** (`NativeOptions::dragAndDrop`): XDND v5 target and source, Win32 `IDropTarget` / `DoDragDrop`, on main's core API (`dragExternalMove`, `dropExternal`, new `UiContext::dragFinishExternal`); files and text in, `kDragText` / `kDragFiles` out. **OS tool windows** (`NativeOptions::osWindows`, default off): `UiContext::setOsWindows / windowSurfaces`, class `os-window` (set by `dockFloat`), `NativePlatform::toolWindow*`; floating docks become borderless OS windows on X11 / Win32 and stay in-window panels otherwise (Wayland included). **Window chrome**: `WindowFrame` / `WindowChrome` / `AppWindow::command`, `chrome.hpp` (`titleBar`, `chromeHitTest`); X11 `_MOTIF_WM_HINTS` + `_NET_WM_MOVERESIZE` or a backend move / resize without a window manager; Win32 `WM_NCCALCSIZE` / `WM_NCHITTEST`. **Saved state**: `AppWindow::saveUiState / restoreUiState / saveWindowState / restoreWindowState`, `Application::saveState / restoreState / saveStateTo / restoreStateFrom`, `StateHooks`, `Settings::toText / fromText`; a position set before `show()` places the OS window; Android keeps it in the saved-instance state. **X11 XSETTINGS**: `Xft/DPI` (live re-scale of every window), theme name / prefer-dark (live `ThemeChanged`), cursor theme and size. **Frame pacing**: `AppWindow::setMaxFrameRate`, `Presenter::vsynced`. **Synthetic input**: `postPointerMove / postPointerButton / postClose`. **Notifications** (`fuse_ui_extras`): `desktop::Notifier` on `org.freedesktop.Notifications` (actions, callbacks, replace, close), `notify()` native first, `TrayIcon::showMessage`. Gates: `fuse_app_{saved_state,window_chrome,dialog_helpers,clipboard_png,frame_pacing,tool_surfaces}` (headless), new sections of `fuse_app_native_services` (Xvfb: clipboard image / MIME / lazy / 600 kB INCR / PRIMARY between two connections, XDND text and files between two connections with refusal / leave / in-app-only types, a floating dock in its own OS window read back against the CPU raster and dragged by its title, caption drag / edge resize / size limits / close on a Custom-frame window, saved-state restore of a real window, XSETTINGS with the gate as settings manager, fake-helper dialogs), `fuse_ui_extras_notify` (private `dbus-daemon`). Not done: XIM pre-edit, RandR change notification, async Vulkan presenter start, software-presenter dirty-rect copy, `presentTiming`, `NativeOptions::colorSpace`, a settings manager that starts after the app, Win32 delayed clipboard rendering, drag images / custom MIME types across applications, Win32 `WM_POINTER` touch / pen and high-contrast / text-scale theme fields, Cocoa drag and drop (parked, never compiled), Wayland (separate port item). Not verified: everything Win32 (not run), every X11 path that needs a window manager, the Android saved-state hook, `Presenter::vsynced` with a real FIFO swapchain. |
| UI6 catalogue (2026-11-12) | **Tier A–D complete** | `widgets_tier_b3.cpp`: group box, stack, tool bar, status bar, main window, context menu, message box, wizard, popover, toast, standalone header, word-wrapped text edit, plain text edit, rich text view, double spin box. Gate `fuse_ui_tier_b3` + `fuse_ui_widget_<name>`. Added after: flame-graph chart kind (`ChartKind::Flame`, `chartSetFlame`), tabbed docks (`dockTabify` / `dockRaise`) and floating docks (`dockFloat`, a panel in the overlay layer dragged by its title bar), both round-tripped by `dockSaveLayout` / `dockRestoreLayout` (extra `tab` / `float` lines, older blobs still restore), game minimap frame (`minimap`, `minimapSetBlips`) and ammo counter (`ammoCounter`, `setAmmo`); gates `fuse_ui_widget_chart_flame`, `_dock_tabs`, `_dock_float`, `_dock_layout_tabs_float`, `_minimap`, `_ammo_counter`. Goldens: `test_widget_golden.cpp`, one gate per scene, `fuse_ui_widget_golden_{containers,text,dialogs,wizard,mainwindow,hud}` against `tests/golden/widgets_<scene>.png` (MINIMAL) / `widgets_<scene>_full.png` (FULL), same tolerance and `FUSE_UI_UPDATE_GOLDEN=1` regenerate path as `fuse_ui_cpu_raster_golden`; they covered every widget of this row except the context menu and the double spin box, which got the `spinmenu` scene later (Finishing row). The goldens found the wizard painting each page's title over the page content (fixed: `wizardpage` class that paints no text). Not done: tab groups inside a floating dock, and floating docks as separate OS windows (drop zones came with the Finishing row). |
| Drawing (2026-11-12) | **Paths, Painter, SVG, icons: CPU reference done; GPU by pre-rasterisation** | `fuse_ui_core`: `Path` (lines, quadratic / cubic Béziers, SVG arcs, shape helpers, SVG path data), flattening in device pixels, exact-area coverage rasteriser with a per-shape mask cache, stroker (caps, joins, miter limit, dashes; pieces cut at the inner miter so coverage is the exact union). Draw-list ops `Path` (fill non-zero / even-odd or stroke; solid or N-stop linear / radial / conic gradient with pad / repeat / reflect), `NinePatch` (9-slice and sub-rect images), `PushClipPath` (also per-corner rounded clips) and `PushLayer` (group opacity); `translate` / `scale` carry them, a 2× scaled list renders bit-identically to the shapes recorded at double size. `Painter` (state stack, affine transforms, brush / pen, clips, layers, text, images, 9-slice). `SvgImage` (icon-level subset, own XML scanner, strict on malformed input) and 19 built-in icons; widgets `svgimage`, `setIcon` on button / tool button, `iconButton`. GPU: no kernel change; `packDrawList` pre-rasterises each vector op / group on the CPU and emits a kernel Image command (the reference composites these ops as RGBA8 texels with the Image blend, so the result is identical by construction). Gates `fuse_ui_paths`, `fuse_ui_painter`, `fuse_ui_svg`, `fuse_ui_vector_steady`, `fuse_ui_vector_golden_{paths,icons}`, `fuse_ui_gpu_pack_vector`. Not done: a native GPU path / gradient op (pre-rasterisation costs CPU time and upload bytes per covered pixel, which is why chart series, node-graph links, the curve editor and dial arcs still draw `line()` segments); `fuse_ui_vk_parity` has not been run with vector ops (no Vulkan device here); rotated / skewed text and images; SVG `text`, `use`, `clipPath`, `mask`, filters, CSS sheets, radial focal point, true group opacity inside SVG; path tessellation on job workers; MSDF icons. **Icon port (2026-11-13):** `fuse_ui_icon_cook` / `fuse_ui_icon_cook_lib` cook SVG files with `SvgImage` (no nanosvg) into `.fuseicons` packs of parsed vector shapes; `IconSet` (`fuse/ui/icon_set.hpp`) loads them without XML parsing; `UiContext::setIcons`; `icon` / `icon-size` style properties (pack, then built-in icons) honoured by `button`, `toolbutton` and the new `icon` widget; `widgets::setIcon(ui, node, name)`; `"icon"` in `.fuseui` resolves through the pack; the Lucide starter pack is cooked at build time. Gates `fuse_ui_icon_cook`, `fuse_ui_icon_render` (goldens `icons.png`, `icons_color.png`). Not done: `:name:` markup in menu text, `:disabled` dimming of fixed-colour paints, the designer icon picker, `fuse_ui_cook --icons`. |
| Style engine + drawing core port (2026-11-13) | **CPU reference done, gates green on Linux; GPU by pre-rasterisation, not run on a device** | The `gui-framework` style / drawing features ported onto this tree's engine ([`PORT_gui_framework.md`](../ui/archive/PORT_gui_framework.md) §3.8 / §3.9). **Style**: attribute selectors (`setAttribute`), `:nth-child` family, `:empty`, `:not / :is / :where / :has`, `+` / `~`, nesting, with selector-driven invalidation (`StyleEngine::features` / `classInvalidation`); `@media` (re-evaluated on viewport / render scale / preferences), `@container` (settled inside `update()`), `@layer`, `@scope`, `@supports`, `@keyframes`; `animation-*` and `UiContext::animate` / `animateCss` with `AnimationHandle` control (pause / play / reverse / finish / cancel / time / rate / `commitAnimationStyles`), held animations idle; `position: relative / absolute / fixed / sticky`, `z-index`, `aspect-ratio`, subgrid; `transform` (2D, hit testing through the inverse, `toLocal`), `filter`, `clip-path`, `mask-image`, multi-stop `background-image`. Cooked style format 4. **Drawing**: `DrawList::pushTransform / pushFilter / pushMask` on the CPU rasteriser (translation exact and in place, other matrices resampled; Gaussian blur, Filter Effects colour matrices, drop-shadow; alpha / luminance masks), `DrawList::path`, `gradientStopsAt`; the Extras path adapter is gone; `packDrawList` bakes the groups (translation groups packed as moved commands). Gates `fuse_ui_css_selectors4`, `fuse_ui_style_invalidation`, `fuse_ui_css_cascade`, `fuse_ui_css_animations`, `fuse_ui_web_animations`, `fuse_ui_css_position`, `fuse_ui_transform_affine`, `fuse_ui_css_masking`, `fuse_ui_css_gradient`, `fuse_ui_draw_fx_groups`, `fuse_ui_style_fuzz`, `fuse_ui_vector_golden_effects`, additions to `fuse_ui_gpu_pack_vector`. **Not done**: `!important`, `cubic-bezier()` / `steps()`, 3D transforms, `url()` filters / masks / clip paths (the branch's `filter_program.cpp` and `css_masking.cpp` stay parked), scroll / view timelines, `animation-composition`, document `"animate"` / `"order"` keys, stacking contexts across parents, native GPU ops for the groups; full list in `docs/ui/KNOWN_GAPS.md`. |
| UI11 | **Wayland backend live (2026-11-13); run under headless weston 13 and an in-process test compositor only** | `fuse_app_native` has a Wayland backend beside X11 on Linux / FreeBSD, chosen at run time (`NativeOptions::backend`, `$FUSE_UI_BACKEND`, else `$WAYLAND_DISPLAY`), with `libwayland-client` / `libxkbcommon` / `libwayland-cursor` loaded by `dlopen` and hand-written protocol tables (no Wayland headers, no wayland-scanner). Ported from the `claude/magical-bassi-815825` implementation onto this tree's `NativePlatform` (`App/src/native_wayland.cpp` = protocol layer, `native_wayland_platform.cpp` = the `NativePlatform` side, a one-line dispatch per public call in `native.cpp`), plus from `gui-framework`: `SurfaceSource::Wayland` / `VK_KHR_wayland_surface` in `vk_present.cpp`, the `wl_surface.frame` throttle and the in-process-compositor test idea. Done: xdg_toplevel windows (title, app id, size limits, maximise / fullscreen / restore / minimise, hide / show, close guard, mode and activation reported back), `wl_shm` software present and the Vulkan swapchain, pointer / keyboard (xkb keymap, compositor repeat rate) / touch (pointer-id path) / wheel steps and smooth scrolling, integer and fractional scale (`wp_fractional_scale_v1` + `wp_viewporter`), `wp_cursor_shape_v1` or theme cursors, clipboard (text and, with the platform layer, `setClipboard` formats: images, MIME types, lazy items) and the primary selection (`zwp_primary_selection_v1`), drops from other clients into `dropExternal`, `zwp_text_input_v3`, server-side decorations through `xdg-decoration` and, where the compositor has none, the toolkit's window chrome as client-side decoration (`NativeOptions::clientDecorations`; caption / edge presses handed over with `xdg_toplevel.move` / `resize`, also for `WindowFrame::Custom` / `Borderless` windows), synthetic input in-process, `osPopups` as `xdg_popup`s, window icons (`xdg_toplevel_icon_v1`), displays. Gates: `fuse_app_wayland` (headless weston, started by the gate), `fuse_app_wayland_seat` (in-process compositor on libwayland-server); both skip (77) where the libraries or weston are missing. Not done: never run on mutter / kwin / wlroots or real hardware; client-side decorations are the toolkit's own title bar (no libdecor, no desktop theme, no window menu) and no compositor has yet moved or resized a window by them (the requests were seen by the in-process compositor only); no drag out to other clients; no OS tool windows (`osWindows` is ignored, child toplevels exist in the protocol layer only); no tablet API; no XSETTINGS equivalent; no `xdg-activation`; the Vulkan path was run with lavapipe in a private build only (the default gate machine has no Vulkan loader); the branch's sway gates with a second client (`tests/test_app_wayland_parity.cpp`, `wayland_peer.hpp`) are still parked. |
| Core interaction (2026-11-12) | **Landed; gates green on Linux** | Drag and drop in `fuse_ui_core` (`DragData`, `beginDrag`, drag sources / drop type lists / filters, `DragEnter/Move/Leave/Drop/DragEnd`, `:drag-over`, Move / Copy, Escape, overlay ghost and drop indicator, `dropExternal` hook for OS drops, touch long-press pick-up that coexists with pan) and row drag in list / table / model-list / grid views (`setListDragDrop` etc.; string lists edit themselves, model views emit `RowMoved` / `Dropped` / `DragEnded`). Gestures: click counting with `DoubleClick` (list / table / grid activate on it, which is what the file dialog's list uses), tap / double tap / long press, pointer-id multi-touch with pinch and two-finger rotate. `ActionRegistry` (`actions.hpp`): actions with shortcuts, window / subtree / modal scope rules, text-field typing rule, bound buttons / check boxes, action menus with right-aligned shortcut text, shortcut parse / print, rebinding with conflict detection, save / load. Style motion: CSS `transition`, `enter-animation` / `exit-animation`, `animateProperty` (tween, spring), `animateKeyframes`, reduced-motion switch; animated nodes use scratch style slots (no interning, no per-frame allocation); the default theme stays static (opt-in classes `.fx`, `.fx-fade`, `.fx-slide`). `direction: rtl` with `padding-start/end`. `doc::DocumentFileWatcher` (file-time poll over the existing `Document::reload`). Gates: `fuse_ui_drag_drop`, `fuse_ui_gestures`, `fuse_ui_actions`, `fuse_ui_style_motion`, `fuse_ui_rtl_layout`, `fuse_ui_hot_reload_file`, `fuse_ui_interaction_golden`. Not done: no platform backend calls `dropExternal` or the pointer-id touch calls yet (OS file drops and pinch / rotate are reachable only through the API); no drag out of the application to the OS; drag auto-scroll is one step per pointer move, not timed; tree view and the game inventory are not on the core drag (the inventory keeps its own); action menus are flat (no submenus from action lists) and there is no engine `ActionMap` adapter; transitions do not follow inherited values into descendants, there is no `@keyframes` in style sheets and no transition of `display` other than enter / exit; RTL does not mirror the inside of self-painting widgets, column-wrap or scroll origins, and `text-align: left / right` are aliases of start / end; pen input (pressure, tilt) is not modelled; the interaction golden exists for the MINIMAL text profile only; the cooked format version went to 2, and to 3 when the feature sets were merged (Finishing row); cooked files must be re-cooked. |
| Finishing (2026-11-12) | **Done in the CPU / headless paths, merged with the text / platform / interaction / model-view / drawing work; device and macOS parts unbuilt** | **Dock drag and drop**: a docked dock's title bar or its tab tears off past 8 px into a floating dock under the pointer; drop-zone indicators (Left / Right / Bottom / Center of the area, Tab over a docked dock) with a translucent landing preview; release docks / tabifies, elsewhere it stays floating; Escape, `dockCancelDrag` or a cancelled touch restores region, order, tab group / index / current tab or floating rect; mouse and touch (`dockDragged`, `dockDropZone`, `dockDropTarget`, `dockDropPreview`, `dockDropIndicator`). Gates `fuse_ui_dock_drag_{tear_off,zones,tabs,escape,touch}`. **Coverage**: an audit of `widgets.hpp` / `platform_feel.hpp` against the tests found public functions no gate called; `fuse_ui_gap_*` gates them (double spin box, standalone tab bar, `closePopup` / `closeDialog` / `closePopover`, `timelineSetView`, viewport image / overlay, `nodeGraphDisconnect`, haptics, theme text, and the touch-drag hooks of grid view / header / video); golden `fuse_ui_widget_golden_spinmenu` (double spin boxes + an open context menu, MINIMAL and FULL). **Accessibility** (§4.14): `A11yRole` gained Menu, MenuItem, MenuBar, Dialog, Tree, TreeItem, Table, Row, Cell, ColumnHeader, TabList, TabPanel, ToolBar, StatusBar, Tooltip, Alert; `A11ySnapshotNode` carries value text, expanded / selected / modal, live region, level, position in set and the accepted actions; virtualised views (list, tree, table, model list, grid) and tab bars, dock tab strips, headers and rich-text links expose their VISIBLE items as virtual children with stable ids (`WidgetClass::a11y`); `UiContext::performA11yAction(node, action, value, child, text)` (Focus, Click, Increment / Decrement, SetValue incl. text, Expand / Collapse, Select, Scroll*, ScrollIntoView; `WidgetClass::a11yAction`) is what the bridge and tests call; `UiContext::setA11yLive` + `Bridge::takeAnnouncements` for toasts / status messages; the bridge diffs the new fields and gives virtual children 64-bit ids. On the merged tree the table / model-list hooks sit on the one item view class (`widgets_modelview.cpp`): header row and column headers (Click sorts), visible rows and cells in visual column order (hidden and scrolled-out columns left out), selection from the `SelectionModel` (multi-selection; cells when the unit is Cells; Select is idempotent, Click follows the mode's pointer rules), check cells (`checked`, Click toggles through `setData`), editable cells (SetValue = an edit through `setData`), disabled cells, sideways scrolling (ScrollLeft / Right, ScrollIntoView of a column); `treeModelView` is Tree / TreeItem with level, expanded / collapsed and Expand / Collapse, its rows keyed by item id so ids survive expanding above them (item ids >= ~32M fall back to the visible row); the in-place editor is a real child named after its column; grid tiles report multi-selection; menu items are named by their label alone (no shortcut text, no state bytes) with disabled / checked states. Gates `fuse_ui_a11y_family_{controls,menus,dialogs_live,list,tree,table,modelview,grid,tabs,text,pro,bridge}`. **Documents**: every catalogue widget is creatable from a `.fuseui` by class name through document classes (`Doc/src/doc_classes.cpp`: group box, tab widget, stack, splitter, dock area / dock, dialog, main window, scroll area, tool / status bar, property grid, markdown, font picker, table / grid / model list over inline data, `treemodelview` over inline `"items"` (a `StringTreeModel`; `"editable"`, `"expanded"`), `svgimage` (`"icon"`: a built-in icon name, or inline `"svg"`), `"icon"` on `button` / `toolbutton`, chart, node graph, curve editor, timeline, ...; `registerDocumentClass` for app / game classes); compound widgets are rebuilt on hot reload; the cooker's registration list resolves document classes to the widget classes they build (and adds what item views create later: the cell tooltip, a tree's editor); `kCookedVersion` is 3 (the merged property set; document-class properties are stored as data, never as bindings). Gates `fuse_ui_doc_{all_classes,every_class,compound_reload,cooked_catalogue,merged_classes}`. **Samples**: `widget_gallery` (every widget on seven tabbed pages, `ui/gallery.fuseui` + the call-only widgets and the game HUD page; also icon buttons and SVG images, a table with extended selection next to an editable tree table, rich text in real bold / italic, an Edit menu built from an `ActionRegistry` with shortcut texts) and `game_menu` (main / pause / settings on `ui/game_menu.fuseui`: bindings, tabs, sliders, key rebinding, confirm dialog; keyboard, gamepad navigation, mouse, touch), both `fuse_app_add_executable` targets; gates `fuse_app_widget_gallery_page0..6` and `fuse_app_game_menu_sample` (a scripted session over all four input paths that checks each step). **Packaging**: `cmake --install` + `FuseUIConfig.cmake` (`find_package(FuseUI)`, imported `FuseUI::<lib>` targets; every public header directory is installed, which the gate checks for the headers of the later work; gate `fuse_ui_install_consumer` builds and runs an out-of-tree consumer against the install tree, using model / view, actions, icons, font families, plural rules and the app services); APK asset staging (`fuse_app_stage_assets`, gate `fuse_app_stage_assets`); Apple bundles from `fuse_app_add_executable` (`Info.plist.ios.in` with `UILaunchScreen` and orientations, optional macOS `.app` with `FUSE_APP_MACOS_BUNDLE`, icon from `Branding/fuse_256.png`, `fuse_app_bundle_resources`); an Android Gradle project template (`App/android/build.gradle`). **Fixed while merging** (each with a gate): `setContextMenu` opened nothing on widgets whose class consumed the right press (list, tree, table, grid, model views, line edit, slider, spin box): a consumed right `PointerDown` is now still offered to the node's event slots unless the class showed its own menu (`EventContext::contextMenuHandled`, set by the item view's `setViewContextMenu` hook), and a right press no longer steps a spin box, moves a slider / scroll bar / dial, switches a tab or folds a tree row; a context-menu pick on a combo box no longer changed its selection (`fuse_ui_gap_context_menu_views`). The default theme dims disabled inputs (`:disabled` rules for tool button, link, check box, radio, switch, slider, scroll bar, dial, spin box, combo box, line edit, menu-bar button; `button` already had one), and a disabled widget no longer reacts to the mouse wheel (`fuse_ui_gap_disabled_theme`; golden `widgets_spinmenu` regenerated for the dimmed spin box). The game dialogue body and the rich text view were both registered as `richtext` (UI7 row). Not done / not verified: the Gradle template never built an APK (no Android SDK on the build machine) and the Apple bundle rules and plists were never run through Xcode / macOS CMake; the AccessKit backend's new roles, states and actions (`FUSE_UI_ACCESSKIT_EXTENDED`) were not compiled (no accesskit-c here) and no native adapter beyond the existing AT-SPI one is wired; game HUD widgets keep their earlier roles (no virtual children for the radial menu / inventory); a tree item's current-row (as opposed to selected) state and a menu item's shortcut are not in the snapshot; the in-window menu bar built from `MenuDesc` (Platform services) writes check marks and shortcuts into the item text ("[x] Grid    Ctrl+G"), so that is what its items are named; tab groups inside a floating dock; the gamepad in `game_menu` is fed by a script or by the host (there is no gamepad backend in `fuse_app`). |
| Branch merge (2026-11-12) | **Structural merge done; UI port list open** | `claude/magical-bassi-815825` (334 commits from the same base) merged with this tree's UI architecture and public API kept. Everything outside `Source/FUSE/UI` came in whole (Renderer, Remaster, `Tools/FUSE`, EditorShell, asset / animation cooking, vendored nanosvg / Lucide icons / Noto Thai + CJK fonts). Live from the branch's UI: `fuse_ui_l10n_lib` (CLDR plurals, ICU-style messages, dates, times, currencies) with `StringTable` message formatting, `LocaleHooks` for the calendar, load-time document translation and `fuse_ui_extract_strings` (gate `fuse_ui_l10n`); Lua handler lifetime (gate `fuse_ui_script_bind`). Since ported: the text stack (COOKED, MSDF, colour glyphs, variable fonts, features, catalogue: UI3 row), the icon cooker / `IconSet` / `icon` property (Drawing row) and the `i18n.hpp` fold. Also ported since (rows below): the Wayland backend, window chrome, notifications, the app platform layer, `ProjectHost`, themes, dock splits and the other API the FUSE-UI editor shell needs. Still in the tree as unbuilt source with its tests and goldens: AccessKit window adapters, GPU blur / damage / SDF quads / effects, the rest of the core deltas, the Android JNI / APK work, the Designer additions; [`docs/ui/archive/PORT_magical_bassi.md`](../ui/archive/PORT_magical_bassi.md) §0 is the split. `-DFUSE_EDITOR_SHELL=fuse` builds since 2026-11-13 (last row). |
| Port: themes, documents, project host (2026-11-13) | **Live on Linux (245 standalone ctest entries, 100% serial); Android / FreeBSD cross-built; in-tree ProjectHost compiled, see the port lists** | Themes: `Widgets/src/theme.cpp` is one rule body over a `:root` token table with dark / light / high-contrast values (`ThemeVariant`, `ThemeDesc`, `themeCss`, `themeTokensCss`, `setTheme`, `HighContrastPalette`); every colour of the rules and of the widget paint code (106 sites) is a token read through `UiContext::themeColor` (cached, no allocation); keyboard focus rings on all 32 focusable classes; style sheets by handle (`StyleEngine::addSheetTracked / replaceSheet / removeSheet / tokenColor`, `UiContext::setThemeStyleSheet`), `AppWindow::applyTheme` keeps one override sheet; dark goldens unchanged, light + high-contrast goldens of eight scenes, contrast gate (4.5 / 7). Extras: rich text editor on main's text stack (real bold / italic faces, `UndoStack` undo, view-markup bridge to the `richText` view), PDF path output, audio level meter, stand-alone gesture recogniser, `<fuse/gui.hpp>`. `doc::StyleSheetFileWatcher` (style sheet hot reload). `l10n::parsePo` (gettext `.po` catalogues into `StringTable`). `fuse_app_project` / `fuse_app_host` (`ProjectHost` for `kind: app`). `fuse_ui_bench_hud` registered after a style-hash fix (0.72 -> 0.105 ms), `fuse_app_binary_size`. **Not done:** SVG documents into `SvgImage` (`<text>`, `clipPath`, `mask`, patterns, focal gradients, filters), system video / audio output, Designer additions (need the icon and GPU effect items), `fuse_ui_hud_binary_size` (needs the COOKED profile); no backend queries high contrast, no forced-colours mode for application sheets; five dark-theme text pairs are below 4.5:1 (frozen by the dark goldens, pinned in the gate); light / high-contrast goldens exist for MINIMAL text only; the hybrid GPU viewport of `ProjectHost` and `--native` need Vulkan and were not run. |
| Widget port (2026-11-13) | **Live on Linux (CPU / headless); Android + FreeBSD cross-built** | From the merged branches (`docs/ui/archive/PORT_magical_bassi.md` §3.12, `PORT_gui_framework.md` §3.7 / §3.12), on main's architecture. **Editor API** (`widgets_tier_b2.cpp`, `widgets_tier_b.cpp`, `widgets_tier_d2.cpp`): `DockEdge`, `dockSplit`, `dockOpen`, `dockRemove`, `dockSideOf`, `dockSplitOf` on main's dock model (nested `docksplit` splitters next to tab groups, floating docks and dock drags; layout format `fusedock 2` with `tree` lines, written only when splits exist, format 1 still read and written); `treeLineAt` / `treeLineRect`; `TimelineClipDrag`, `timelineClipDrag`, `timelineSelectClip`, clip selection and Ctrl-key retiming. Gates `fuse_ui_dock_split_{split,split_groups,tree_line,timeline_clip}`. **Options** (`widgets/edit_options.hpp`; new files `widgets_lineedit.cpp`, `line_regex.cpp`, `completer.cpp`, `widgets_spinbox.cpp`, `widgets_tabs.cpp`; a widget without options keeps its old code path and pixels): line edit echo modes / max length / placeholder / read-only / validators / fixup / input masks / `EditingFinished`, completer, editable combo box, spin box options with typed entry, tab bar closable / movable / disabled tabs / scroll buttons / add-remove pages. Gates `fuse_ui_lineedit`, `fuse_ui_completer`, `fuse_ui_editcombo`, `fuse_ui_spinbox` (the branch's gates, adapted to main's accessibility snapshot), `fuse_ui_tab_options_{closable,movable,scroll,pages}`. **Extras** (`widgets/extra.hpp`, `widgets_extra.cpp`, `widgets_buttonbox.cpp`, moved from `fuse_ui_extras` into `fuse_ui_widgets`): dialog button box, dialog default / reject keys, message box escape button, wizard validator, input dialog, tool box, LCD number, key sequence edit, MDI area, graphics scene / view with rubber-band selection, on-screen keyboard; accessibility hooks, document classes (`editcombo`, `lcdnumber`, `keysequenceedit`, `toolbox`, `mdiarea` / `mdiwindow`, `graphicsview`, `vkeyboard`, `dialogbuttonbox`, and the option properties of `lineedit` / `spinbox` / `tabwidget`), default-theme rules, `installClasses`, a "More" page in `widget_gallery`. Gates `fuse_ui_extra_{buttonbox,dialog_keys,input_dialog,toolbox,lcd,keysequence,mdi,graphics,vkeyboard,classes}`; goldens `fuse_ui_port_golden_{edit_options,extra,dock_split}` (MINIMAL text profile only); `fuse_ui_port_doc` (the document classes and properties); `fuse_ui_port_zero_alloc` (50 steady frames over every ported widget: 0 allocations); `fuse_app_widget_gallery_page7`. Core: `SignalKind::EditingFinished / Highlighted / Activated` appended. Not done: split zones in the dock drag; `-DFUSE_EDITOR_SHELL=fuse` was not built (it also needs the icon, platform and accessibility port items); tab position / elide / tab icons / tab tooltips; date / time edit sections; calendar options; tree-view drag and drop, scrolling and multi-selection; table multi-row / cross-view drags; header resize modes and menu; list icon mode; splitter sizes; tool bar overflow and movable tool bars; MIME drag payloads; the per-class gates and goldens of `magical-bassi` (`tests/test_widget_gates.cpp`, rendered with that branch's theme and text). GPU output of the new widgets is unverified (no Vulkan loader); they use only draw-list primitives the packer already has. |
| Ports merged: themes x widgets x platform (2026-11-13) | **Live on Linux (292 standalone ctest entries); Android / FreeBSD cross-built** | The theme port, the widget port and the app platform layer + Wayland backend were made consistent. The ported widgets (`widgets/extra.hpp`, `widgets/edit_options.hpp`, dock splits, timeline clip selection) take every colour from theme tokens (nine new: `--lcd-text`, `--lcd-bg`, `--workspace`, `--scene-bg`, `--item-focus`, `--placeholder`, `--text-disabled`, `--focus-ring`, `--window-close`), have light / high-contrast rules and goldens and show keyboard focus (the editable combo box draws a ring around its field); the theme gates measure them (`fuse_ui_theme_contrast`, `_focus_ring`, `_token_only`, `_paint_literals`, which now also scans `App/src`). The client-side title bar is token-styled and follows `AppWindow::applyTheme`; X11 sets `SystemTheme::highContrast` from the desktop theme name (XSETTINGS, live). Dock splits work with OS tool windows (`fuse_ui_dock_split_tool_windows`, `fuse_app_native_services`). `fuse_app_binary_size` re-measured at 2,683,504 B (budget 2,951,000; steps in `docs/ui/BASELINES.md`). Open: see `docs/ui/KNOWN_GAPS.md` (themes). |
| Ports merged: style engine x text / icons x themes x widgets (2026-11-13) | **Live on Linux (standalone ctest: MINIMAL 327, FULL 298, COOKED 326, all passing); Android / FreeBSD cross-built; FUSE-UI editor shell builds** | The style engine + drawing core port and the text / icon / i18n port were merged onto the tree with themes, widgets and the platform layer, and made consistent. **One property registry**: 97 properties (main's 71, the style engine's 21, then `font-stretch`, `font-variation-settings`, `font-feature-settings`, `icon`, `icon-size`), value types `Raw`, `FontSettings`, `Ident`; `kCookedVersion` 5 (Raw payloads, tags, and the `@media` block of each token are stored; 3 / 4 are refused). **Themes under the at-rules**: `UiContext::setMediaTheme(dark, moreContrast)`, driven by `widgets::install` / `setTheme` and `AppWindow::applyTheme`, so `@media (prefers-color-scheme)` and the new `(prefers-contrast: more)` follow the theme shown; a `:root` token defined inside an `@media` block applies only while the block does (rules, `var()` and paint-code `themeColor` agree); layered application rules lose to the unlayered theme, as CSS says. **Text / icons under the style engine**: `icon`, `icon-size`, `font-stretch` in `@media`, `@container`, `@layer`, nested rules, `@keyframes`, scripted animations and transitions (`font-stretch` / `icon-size` interpolate, `icon` switches half-way); icons in `currentColor` follow the theme. **Glyph modes under effects**: MSDF and colour-image glyphs are drawn inside transform / filter / mask groups by the CPU rasteriser and are part of the image the GPU packer bakes for such a group; loose ones are still skipped on the GPU path and now counted (`GpuDrawData::skippedGlyphs`). **Editor shell**: `-DFUSE_EDITOR_SHELL=fuse -DFUSE_BUILD_VULKAN=OFF` builds `fuse_editor_fuse`, the app and its gates; `ctest -L fuse_editor_ui` under Xvfb: 78 entries, 72 pass, 4 skip without a Vulkan device, and `ui_frame` (registered under two names) misses one wall-clock budget on the merge machine (a full 1600x900 CPU raster in 8 ms; 7.5 to 9.0 ms measured, passes with `FUSE_UI_PERF_REPORT_ONLY=1`). Three core behaviours it needs were ported from the `magical-bassi` core deltas: a hover change keeps `:hover` on ancestors shared by the old and new chain (a pointer moving between two buttons no longer restyles and damages the window: 1.35 -> 0.09 ms per hover frame in the shell), unconsumed keys reach the root's `Event` signal, the viewport widget forwards Tab without trapping focus; the shell's flame-graph bars no longer set `position: absolute` (now real CSS positioning). COOKED: the pack of the UI font gained the 12.5 px strike (calendar day numbers, code editor line numbers) so the `editors` goldens of the theme port hold in that profile; the rich text golden is MINIMAL only. Gates `fuse_ui_merge_{registry,style_rules,glyph_fx,shell_core}`, additions to `fuse_app_theme_sheets`, `fuse_ui_gpu_pack_vector`, `fuse_ui_doc_merged_classes`. `fuse_app_binary_size` re-measured at 3,424,968 B (budget 3,767,000; steps in `docs/ui/BASELINES.md`); `fuse_ui_bench_10k` unchanged (0.44 -> 0.42 ms median, interleaved). No golden changed. Not done: MSDF is not used to sharpen text under a scaling transform (the group layer is resampled); tokens are not layered or scoped; colour / MSDF glyphs on the GPU path; nothing of this ran on a GPU, Windows or Apple. Open items: `docs/ui/KNOWN_GAPS.md`, port lists §0. |

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

### UI11 — Wayland and platform backlog (M, after UI5)

Native Wayland for `fuse_app_native` beside X11, then the platform items no earlier phase owns: xdg_popup popups and child-toplevel tool windows, fractional scale, text-input-v3, the Wayland clipboard / primary selection / drops, window icons. Gates: `fuse_app_wayland` (headless weston; includes the popup checks) and `fuse_app_wayland_seat` (an in-process compositor on libwayland-server driving the seat). The backend is live (see the status table); still open from this phase: client-side decorations, drag out, primary selection and tool windows on `NativePlatform`, and a gate against a wlroots compositor with a second client.

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
| D1 | Platform backend for windows, input, IME, clipboard and dialogs | (a) Native per OS: extend Win32 / X11, write Wayland, Cocoa, UIKit, Android. (b) SDL3 everywhere. (c) Keep native Win32 / X11; use SDL3 for macOS, iOS, Android and Wayland; replace with native only where SDL3 falls short | **(c)**. Fastest route to all five platforms with IME, touch, pen, file dialogs and mobile lifecycle; keeps the proven native raw-input game path; SDL3 is zlib-licensed *Adopted per recommendation on 2026-10-29, revisitable by Odin.* |
| D2 | Text stack | (a) HarfBuzz + FreeType + bidi + segmentation. (b) Minimal shaping only. (c) In-house shaper | **(a) as `FULL`**, with `MINIMAL` and `COOKED` profiles for small games. Correct Arabic / Indic / Thai is table stakes for an application framework *Adopted per recommendation on 2026-10-29, revisitable by Odin.* |
| D3 | Programming model | Retained only / immediate only / both on one core | **Both on one core** (Section 4.4) *Adopted per recommendation on 2026-10-29, revisitable by Odin.* |
| D4 | Declarative authoring format | (a) JSON for structure + CSS subset for style. (b) A QML-like DSL | **(a) first.** Reuses the existing JSON parser and tooling; the designer writes it; the cooker binarises it. Revisit a DSL only if hand-authoring becomes the bottleneck *Adopted per recommendation on 2026-10-29, revisitable by Odin.* |
| D5 | Move the editor off Qt | This reverses master plan §A3 ("Qt replaces GUI") | **Decided (2026-10-29): Qt first.** The editor stays on Qt while the engine and FUSE UI are built, then moves at UI9 behind `FUSE_EDITOR_SHELL` with gate-for-gate parity — [`FUSE_EDITOR_QT_FIRST_PLAN.md`](FUSE_EDITOR_QT_FIRST_PLAN.md) |
| D6 | UI logic language | Lua / another VM / C++ only | **Lua** (already the engine VM) plus binding bytecode for simple expressions *Adopted per recommendation on 2026-10-29, revisitable by Odin.* |
| D7 | Accessibility | (a) AccessKit (all five platforms; adds a Rust toolchain to builds that enable it). (b) Hand-written native adapters. (c) Defer | **Accessibility tree in UI2 regardless; adapters via (a) as an optional module**, off for `hud` builds, so most builds need no Rust *Adopted per recommendation on 2026-10-29, revisitable by Odin.* |

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
