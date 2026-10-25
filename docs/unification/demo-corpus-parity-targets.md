# FUSE U0 — Demo Corpus & Parity Targets

**Phase:** U0 Inventory & collision map  
**Date:** 2026-09-15 (updated 2026-10-25 — U8 real present)  
**Purpose:** Freeze the minimum demo set for unification exit (U8) and map each demo to existing T3D missions, T2D scenes, and addon content.

**Policy:** List may grow later; **must not shrink** below this minimum set (prestarter §13, gate U0).

---

## 1. U8 required demos (frozen minimum)

Aligned with [FUSE_UNIFIED_PRESTARTER.md](../plans/FUSE_UNIFIED_PRESTARTER.md) §13.1:

| FUSE demo ID | Proves | Status |
|--------------|--------|--------|
| `demo_3d_empty` | 3D dimension path | ✅ Headless binary + `project.json` + `.mis` convert/load via `fuse_demo_wiring` |
| `demo_2d_sprites` | 2D dimension path | ✅ SpriteToy-inspired `.cs` module bridge + Box2D tick |
| `demo_hybrid_hud` | Shared frame / compositor | ✅ U4/U5 full module gates (`hybrid_module_gates`) |
| `demo_ai_bt` | `fuse_ai` on 3D + 2D agents | ✅ Hybrid BT tick + UAISK patrol profile + mission convert |
| `demo_timeline` | `fuse_cinematics` | ✅ Outpost intro asset/stub + VActor + hybrid timeline drive |
| `demo_fx` | `fuse_fx` | ✅ AFX mission VM + `defaultWorld2D` sprite socket bridge |
| `demo_adventure_stub` | `fuse_adventure` interactions | ✅ Outpost JSON spawn + mechanics + weapon grant + mission convert |

Future location: `Samples/unification/<demo_id>/` (see [Samples/unification/README.md](../../Samples/unification/README.md)).

**Honest gaps toward full U8 exit (prestarter §13.2):**

- ✅ Real window present — all seven `demo_*` binaries accept `--present` / `FUSE_DEMO_PRESENT=game|1`; Xvfb + Lavapipe gates `ctest -R fuse_u8_.*_present` (GameWindow + `vkQueuePresentKHR`, shared `demo_present.hpp`)
- ✅ Legacy import — bundled stubs under `Samples/unification/*/worlds/` are the default; a local ore mirror may be preferred when present (CI uses stubs only; no network fetch)
- ✅ PIE ASan smoke — `fuse-asan` preset instruments all FUSE targets; `fuse_u8_parity_embed_pie_smoke` + `ctest -L u8` in umbrella `fuse-smoke-asan` job (seven demos + headless `fuse_u8_*`)
- ✅ Animated sprite playback — T2D bridge → `SpriteDrawCmd` atlas frame; hybrid `sprite_layer` horizontal atlas strip + tint (`fuse_hybrid_sprite_layer_cpu`); bindless **imageMap** texture sampling tracked post-U8
- ✅ Player + programmer guides — [`docs/player-guide.md`](../player-guide.md), [`docs/programmer-guide.md`](../programmer-guide.md) (honest limits, real demo binaries + ctest names)
- ✅ U8 engineering exit — CI matrix in [U8-EXIT-SIGNOFF.md](./U8-EXIT-SIGNOFF.md); product sponsor row optional for milestone

---

## 2. Golden 3D missions (T3D lineage)

### 2.1 FUSE root templates

| Mission | Path | Use |
|---------|------|-----|
| Example level | `Templates/BaseGame/game/data/ExampleModule/levels/ExampleLevel.mis` | Baseline 3D load/play |
| Editor template | `Templates/BaseGame/game/tools/levels/EditorTemplateLevel.mis` | Editor parity (legacy Gui) |
| Default editor | `Templates/BaseGame/game/tools/levels/DefaultEditorLevel.mis` | Editor smoke |

### 2.2 Recommended `demo_3d_empty` golden path

**Primary:** `Templates/BaseGame/game/data/ExampleModule/levels/ExampleLevel.mis`  
**Stretch:** `Templates/BaseGame` full template — module + datablocks + empty terrain.

**Bundled stub:** `Samples/unification/demo_3d_empty/worlds/example.mis` (SpawnSphere + GroundPlane wiring stubs).

**Acceptance (U8):** FUSE runtime loads converted FUSE world3d, clears colour or minimal scene, no crash under ASan.

---

## 3. Golden T2D scenes (historical upstream Torque2D)

Historical upstream paths below are **not in the FUSE repo** (clone without submodules). Parity demos use bundled stubs under `Samples/unification/`. T2D used `main.cs` module entry points (not `.mis` missions).

### 3.1 Core tutorials

| Scene | Path | Use |
|-------|------|-----|
| Fish tutorial | `third_party/Torque2D/tutorials/fishTutorialBase/main.cs` | End-to-end 2D gameplay tutorial |
| Blank game | `third_party/Torque2D/library/BlankGame/` | Minimal module shell |
| App core | `third_party/Torque2D/library/AppCore/` | App bootstrap pattern |

### 3.2 Toybox — feature slices (recommended parity corpus)

| Toy | Path | Feature exercised |
|-----|------|-------------------|
| **SpriteToy** | `toybox/SpriteToy/1/main.cs` | **Primary `demo_2d_sprites` golden** — sprites, layers |
| CompositeSpriteToy | `toybox/CompositeSpriteToy/1/main.cs` | Composite sprites |
| SceneLayerToy | `toybox/SceneLayerToy/1/main.cs` | Layer sorting |
| CollisionToy | `toybox/CollisionToy/1/main.cs` | 2D collision |
| BridgeToy | `toybox/BridgeToy/1/main.cs` | Box2D joints |
| ChainToy | `toybox/ChainToy/1/main.cs` | Chain physics |
| SoftbodyToy | `toybox/SoftbodyToy/1/main.cs` | Soft bodies |
| AudioToy | `toybox/AudioToy/1/module.cs` | 2D audio |
| PickingToy | `toybox/PickingToy/1/main.cs` | Input picking |
| TextSpriteToy | `toybox/TextSpriteToy/1/main.cs` | Bitmap text |
| Tilemaps | (via tutorials / modules) | Tilemap — add when importer exists (U7) |

### 3.3 Recommended `demo_2d_sprites` golden path

**Primary:** `third_party/Torque2D/toybox/SpriteToy/1/main.cs`  
**Secondary:** `tutorials/fishTutorialBase/main.cs` (broader gameplay)

**Bundled stub:** `Samples/unification/demo_2d_sprites/worlds/sprite_toy_stub.cs`

**Acceptance (U8):** FUSE runtime loads converted FUSE world2d, animated sprites visible, Box2D tick optional.

---

## 4. One demo per addon feature

| Addon | FUSE module | Golden demo | Path |
|-------|-------------|-------------|------|
| **BadBehaviour** | `fuse_ai` | Behavior testbed | `Samples/unification/demo_ai_bt/worlds/behavior_testbed.mis` |
| **GuideBot** | `fuse_ai` | GuideBot demo | `demo_hybrid_hud` move-toward gate (upstream `guideBotDemo.mis` ore) |
| **UAISK** | `fuse_ai` templates | Full template + UAISK scripts | `Samples/Modules/ai/uaisk-templates/` |
| **Verve** | `fuse_cinematics` | Verve template mission | `Samples/unification/demo_timeline/worlds/verve_intro.mis` |
| **GMK** | `fuse_mechanics` | Full template mechanics | `demo_hybrid_hud` mechanics gates (upstream GMK template ore) |
| **AFX** | `fuse_fx` | AFX minimal demo | `Samples/unification/demo_fx/worlds/afx_minimal.mis` |
| **3DAAK** | `fuse_adventure` | Outpost adventure | `Samples/unification/demo_adventure_stub/worlds/outpost.mis` |

### AFX extended corpus (FX regression)

| Mission | Path |
|---------|------|
| Day | `AFX-Template/game/levels/AFXDemo_Day.mis` |
| Night | `AFX-Template/game/levels/AFXDemo_Night.mis` |
| Fog | `AFX-Template/game/levels/AFXDemo_Fog.mis` |
| Wasteland | `AFX-Template/game/levels/AFXDemo_Wasteland.mis` |
| Orc army | `AFX-Template/game/levels/AFXDemo_OrcArmy.mis` |

---

## 5. Hybrid demo (`demo_hybrid_hud`)

**No single legacy demo exists.** Composite target:

| Layer | Source inspiration |
|-------|-------------------|
| 3D world | `Templates/BaseGame/.../ExampleLevel.mis` (minimal zone) |
| 2D HUD overlay | `SpriteToy` or `AppCore` Gui-style 2D layer |
| Compositor | New FUSE U4 `HybridComposer` |

**Acceptance (U8):** One window, 3D scene + 2D HUD sprites, shared input, single process.

**Status:** Headless `demo_hybrid_hud` exercises all five `fuse_*` modules via `hybrid_module_gates` (60-frame software render). `--present` runs 3 frames through `HybridRendererBootstrap::runFrame` with GameWindow swapchain present (same helpers as `demo_3d_empty`).

---

## 6. Cross-demo mapping to U8 acceptance

| U8 demo | 3D golden | 2D golden | Addon golden |
|---------|-----------|-----------|--------------|
| `demo_3d_empty` | ExampleLevel.mis | — | — |
| `demo_2d_sprites` | — | SpriteToy | — |
| `demo_hybrid_hud` | ExampleLevel (minimal) | SpriteToy HUD layer | — |
| `demo_ai_bt` | BehaviorTestbed.mis | SpriteToy + BT on 2D agent (new) | UAISK scripts |
| `demo_timeline` | Verve template | — | Verve |
| `demo_fx` | AFXDemo_Minimal.mis | AFX on 2D sprite (new) | AFX |
| `demo_adventure_stub` | Outpost.mis (pickup/use) | 2D interact stub (new) | 3DAAK inventory.cs |

---

## 7. Parity tolerance (explicit)

Per prestarter §1.3 and §17:

- **Not** bit-identical replay of every legacy demo on day one  
- FUSE demos are **primary** acceptance; legacy missions are **conversion sources**  
- Visual/audio variance acceptable if behaviour and feature coverage match  

---

## 8. Gate U0 checklist item

- [x] Parity demo list frozen (minimum set documented)
- [x] `Samples/unification/` stubs created (see README)
- [x] Converters (U7) can import golden paths (stub importers + `fuse_import` dry-run)
- [x] U8 wiring wave 2: `fuse_demo_wiring` + bundled `worlds/` sources + `ctest -R fuse_u8_`
- [x] U8 wiring wave 3: golden-path resolver + editor embed convert-before-load + `fuse_u8_parity_embed_pie_smoke`
- [x] U8 wiring wave 4: animated-sprite wiring stubs + golden-path directory classification + ASan on all parity demos + PIE embed covers `demo_hybrid_hud`
- [x] U8 wiring wave 5: `ensureDefault2DWorldReady` (symmetric to 3D) + parity fallback tests for `demo_fx` / `demo_ai_bt` / `demo_timeline` + player/programmer guides
- [x] U8 real present: `fuse_demo_wiring` / `demo_present.*` + `--present` on all seven demos + `fuse_u8_<demo>_present` ctests (`fuse_run_with_display.sh` + Lavapipe ICD lock)
