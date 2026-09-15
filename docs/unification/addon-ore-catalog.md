# FUSE U0 — Addon Ore Catalog

**Phase:** U0 Inventory & collision map  
**Date:** 2026-09-15  
**Scope:** All seven addons under `third_party/addons/` — engine patches vs scripts vs art vs tools; licenses; extractable kernel vs content pack; kernel entry points.

**Policy:** Addons are **ore**, not drop-in plugins. Do not merge addon `Engine/` trees into FUSE `Engine/` (prestarter §2.2, §16). FUSE root already contains pre-integrated `afx/` and `Verve/` — treat as prior partial merges to be refactored into U5 modules.

---

## Summary table

| Addon | Submodule path | Shape | Engine patches | Scripts | Art assets | Tools | License | Kernel estimate | Content pack estimate |
|-------|---------------|-------|----------------|---------|------------|-------|---------|-----------------|----------------------|
| **GMK** | `third_party/addons/GMK` | Full T3D tree + Qt4 PM | 26 files in `component/`, `unit/` | ~1,052 | ~4,057 | Qt4 DLLs, PM.exe | MIT (T3D) | **Small** (~30 C++ files) | **Large** (templates, art, docs) |
| **Verve** | `third_party/addons/Verve` | Engine patch + templates | 163 C++ in `Engine/source/Verve/` | ~104 | ~64 | — | MIT | **Medium** (~163 C++) | Small (template missions) |
| **BadBehaviour** | `third_party/addons/BadBehaviour` | Full T3D tree | 74 C++ in `BadBehavior/` | ~988 | ~2,191 | project gen | MIT (T3D) | **Medium** (~74 C++) | Large (BehaviorTestbed, art) |
| **GuideBot** | `third_party/addons/GuideBot` | Module + docs | 96 C++ in `guideBotT3D/engine/` | ~44 | ~145 | PDF guides | Custom (see `guidebot_license_agreement.txt`) | **Small–medium** (~96 C++) | Medium (demo level, models) |
| **UAISK** | `third_party/addons/UAISK` | Kit folder only | **0** (no engine) | ~44 | ~1 | HTML docs | MIT (`LICENSE`) | **Scripts only** (templates) | Small |
| **AFX** | `third_party/addons/AFX-Template` | Project template | 0 unique engine (see FUSE `Engine/source/afx/`) | ~640 | ~2,099 | shader scripts | MIT (`LICENSE`) | **Already in FUSE root** (218 files) | Large (demo levels, FX art) |
| **3DAAK** | `third_party/addons/3DAAK` | Full T3D tree | 0 addon-only dirs (full engine fork) | ~1,014 | ~2,179 | project gen | MIT (`LICENSE.md`) | **Scripts/systems** (inventory, interaction) | Large (Outpost template) |

---

## 1. GMK — Game Mechanics Kit

**Upstream:** `TorqueGameEngines/Addon-GMK` @ `e322f148`  
**License:** MIT (inherits T3D `LICENSE.md` pattern)

### Tree breakdown

| Layer | Path | Count / notes |
|-------|------|---------------|
| Engine patches | `Engine/source/component/`, `Engine/source/unit/` | **26** C++ source files (addon-only dirs vs FUSE root) |
| Bundled engine | `Engine/` (full tree) | ~1,938 C++ files — **discard after extraction** |
| Scripts | `Templates/`, `My Projects/` | ~1,052 `.cs`/`.gui`/`.mis` |
| Art | templates, projects | ~4,057 image/audio/model files |
| Tools | `Project Manager.exe`, `QtCore4.dll`, `QtGui4.dll`, `QtNetwork4.dll`, `QtXml4.dll` | **Discard** — replace with FUSE Qt 6 editor |

### Extractable kernel → `fuse_mechanics`

| Entry point | File | Role |
|-------------|------|------|
| `SimComponent` | `Engine/source/component/simComponent.h` | Component base (`NetObject` subclass) |
| `ComponentInterface` | `Engine/source/component/componentInterface.h` | Property/query interface |
| `DynamicConsoleMethodComponent` | `Engine/source/component/dynamicConsoleMethodComponent.h` | Script-callable components |
| `SimpleComponent` / `MoreAdvancedComponent` | `component/simpleComponent.h`, `moreAdvancedComponent.h` | Reference patterns |
| Unit test harness | `Engine/source/unit/` | Test infra only — not product |

### Kernel entry points (TorqueScript exposure)

Components register via `IMPLEMENT_CONOBJECT` on `SimComponent` derivatives — exact registrations in `component/*.cpp`.

### Content pack

- `Templates/Full/`, `Templates/Empty/` — starter projects  
- `Documentation/` — mechanics editor concepts  
- `My Projects/` — sample games  

**Overlap with Verve:** cutscene/mechanics editor ideas — merge into single `fuse_cinematics` + `fuse_mechanics` boundary during U5 (prestarter §10.2).

---

## 2. Verve — Cinematics / timeline

**Upstream:** `TorqueGameEngines/Addon-Verve` @ `0ea77b28`  
**License:** MIT (`LICENSE`)

### Tree breakdown

| Layer | Path | Count |
|-------|------|-------|
| Engine patches | `Engine/source/Verve/` | **163** C++ files |
| Templates | `Templates/` | missions + scripts |
| Tools | `Tools/VerveEditor/` (Gui scripts) | editor scripts |

### FUSE root status

**`Engine/source/Verve/` already exists in FUSE root** (163 files) — submodule copy is reference ore; do not double-merge.

### Extractable kernel → `fuse_cinematics`

| Class | Header | Role |
|-------|--------|------|
| `VController` | `Verve/Core/VController.h` | Timeline director |
| `VTrack` | `Verve/Core/VTrack.h` | Track container |
| `VEvent` | `Verve/Core/VEvent.h` | Keyframe/event |
| `VGroup` | `Verve/Core/VGroup.h` | Grouping |
| `VObject` | `Verve/Core/VObject.h` | Base verve object |
| `VDataTable` | `Verve/Core/VDataTable.h` | Data persistence |
| `VPath` | `Verve/VPath/VPath.h` | Path/cinematic rail (`SceneObject`) |
| `VActor` | `Verve/VActor/VActor.h` | Animated actor (`ShapeBase`) |
| `VMotionTrack` / `VMotionEvent` | `Verve/Extension/Motion/` | Motion tracks |
| `VTimeLineControl` | `Verve/GUI/VTimeLineControl.h` | Legacy Gui timeline (replace with Qt pane U6) |

### `IMPLEMENT_CONOBJECT` kernel registrations (sample)

`VController`, `VTrack`, `VEvent`, `VGroup`, `VObject`, `VMotionTrack`, `VMotionEvent`, `VPath`, `VPathEditor`, `VTimeLineControl`, `VFadeControl`, `VEditorWindow`, plus Torque3D bridge types (`VCamera`, `VSoundEffect`, `VPostEffect`, …).

### Content pack

- `Templates/Full/` — Verve-enabled project  
- Online docs: http://www.violent-tulip.com/?doc-type=verve-docs  

---

## 3. BadBehaviour — Behavior trees

**Upstream:** `TorqueGameEngines/Addon-BadBehaviour` @ `9fd48731`  
**License:** MIT (T3D)

### Tree breakdown

| Layer | Path | Count |
|-------|------|-------|
| Engine patches | `Engine/source/BadBehavior/` | **74** C++ files |
| Also | `Engine/source/component/` | shared GMK-style components |
| Scripts | templates | ~988 |
| Art | templates | ~2,191 |

### Extractable kernel → `fuse_ai`

| Class | Header | Role |
|-------|--------|------|
| `BehaviorTreeRunner` | `BadBehavior/core/Runner.h` | Tick scheduler (`SimObject`) |
| `Behavior` | `BadBehavior/core/behavior.h` | Leaf behavior node |
| `BehaviorTreeBranch` | `BadBehavior/core/Branch.h` | Composite branch |
| `BehaviorTreeStepper` | `BadBehavior/core/Stepper.h` | Step/debug |
| `ScriptedBehavior` | `BadBehavior/leaf/ScriptedBehavior.h` | TS-scripted leaf |
| `FollowBehaviorAction` | `BadBehavior/leaf/compiled/followBehaviorAction.h` | Compiled leaf example |
| Decorators | `BadBehavior/decorator/` | `Loop`, `Inverter`, `Root`, `Monitor`, `SucceedAlways` |
| Editor | `BadBehavior/tools/guiBTViewCtrl.h` | `GuiBehaviorTreeViewCtrl` — replace with Qt BT pane |

### Parity demo mission

`Templates/Full/game/levels/BehaviorTestbed.mis` — primary BT parity target.

### Content pack

- `Templates/Full/game/levels/Outpost.mis`  
- Full T3D template tree (~1,963 engine files) — **discard after kernel extract**

---

## 4. GuideBot — Action / navigation AI

**Upstream:** `TorqueGameEngines/Addon-GuideBot` @ `0e1e4230`  
**License:** **Custom** — `guidebot_license_agreement.txt` (not plain MIT; review before redistribution)

### Tree breakdown

| Layer | Path | Count |
|-------|------|-------|
| Engine | `guideBotT3D/engine/source/T3D/logickingMechanics/guideBot/` | **96** C++ files |
| Game | `guideBotT3D/game/` | scripts + levels |
| Docs | `guideBotUsersGuide.pdf`, `guideBotInstallationGuide.pdf` | |
| Models | `modelsSources/` | source art |

### Extractable kernel → `fuse_ai` (navigation helpers)

| Class | Header | Role |
|-------|--------|------|
| `EnhancedPlayer` | `guideBot/sceneWorldObject.h` / `enhancedPlayer.h` | `AIPlayer` + `GuideBot::Actor` |
| `ScriptAction` | `guideBot/scriptAction.h` | `GuideBot::Action` script binding |
| `SceneWorldObject` | `guideBot/sceneWorldObject.h` | `GuideBot::WorldObject` |
| `VisualDataBlock` | `enhancedPlayer.h` | Visual config |

### Parity demo

`guideBotT3D/game/levels/guideBotDemo.mis`

### Content pack

- `guideBotT3D/game/` assets and scripts  
- PDF documentation  

---

## 5. UAISK — Universal AI Starter Kit

**Upstream:** `TorqueGameEngines/UAISK` @ `c8224f76`  
**License:** MIT (`LICENSE`)

### Tree breakdown

| Layer | Path | Count |
|-------|------|-------|
| Engine patches | **None** | 0 C++ |
| Kit | `The_Universal_AI_Starter_Kit/` | scripts + HTML docs |
| Scripts | `Templates/Full/game/scripts/server/UAISK/` | ~17 `ai*.cs` modules |
| Instructions | `Instructions/UAISK_AFX/` | reference copies of AI scripts |

### Extractable kernel → `fuse_ai` templates

**No C++ kernel** — harvest as **FUSE AI template pack** (TorqueScript patterns):

| Script module | Role |
|---------------|------|
| `aiBehaviors.cs` | Behavior definitions |
| `aiMovement.cs` | Movement helpers |
| `aiTargeting.cs` | Target selection |
| `aiActions.cs` | Action execution |
| `aiNPC.cs` | NPC setup |
| `aiSpawning.cs` | Spawn logic |
| `aiGroups.cs` | Squad/group AI |
| `aiPathed.cs` | Path following |
| `aiWeapons.cs` | Weapon AI |
| `aiGlobals.cs`, `aiDatablocks.cs`, `aiFunctions.cs`, `aiExecutes.cs`, `aiThought.cs`, `aiTraits.cs`, `aiLoading.cs` | Support |

### Content pack

- HTML tutorials (`Getting_Started_Guide.html`, `Pet_Tutorial.html`, `Team_Tutorial.html`)  
- `Templates/Full/` weapon datablocks + game template  

**Note:** Targets T3D 3.10 per README; script porting required for FUSE APIs.

---

## 6. AFX — Arcane FX (spell / residual FX)

**Upstream:** `TorqueGameEngines/AFX-Template` @ `1907e55b`  
**License:** MIT (`LICENSE`)

### Tree breakdown

| Layer | Path | Count |
|-------|------|-------|
| FUSE root engine | `Engine/source/afx/` | **218** C++ files (**already merged**) |
| Template project | `AFX-Template/game/` | scripts, levels, art |
| Template `source/` | `AFX-Template/source/` | `torqueConfig.h` only |

### Extractable kernel → `fuse_fx`

Already in FUSE root. Key entry points:

| Class | Header | Role |
|-------|--------|------|
| `afxChoreographer` | `afx/afxChoreographer.h` | Effect orchestration |
| `afxEffectron` | `afx/afxEffectron.h` | Effect instance |
| `afxEffectGroup` | `afx/afxEffectGroup.h` | Grouped effects |
| `afxEffectWrapper` | `afx/afxEffectWrapper.h` | Wrapper/adapter |
| `afxMagicSpell` | `afx/afxMagicSpell.h` | Spell casting |
| `afxMagicMissile` | `afx/afxMagicMissile.h` | Projectile FX |
| `afxConstraint` | `afx/afxConstraint.h` | Spatial constraints |
| `afxCamera` | `afx/afxCamera.h` | FX camera |
| `afxXM_*` | `afx/xm/` | Transform modifiers (spin, wave colour, height sample, …) |

### Parity demo missions

| Mission | Path |
|---------|------|
| Minimal | `game/levels/AFXDemo_Minimal.mis` |
| Day / Night / Fog | `AFXDemo_Day.mis`, `AFXDemo_Night.mis`, `AFXDemo_Fog.mis` |
| Wasteland / Orc army | `AFXDemo_Wasteland.mis`, `AFXDemo_OrcArmy.mis` |

### Content pack

~2,099 art assets, shaders under `game/`, spell datablocks in scripts.

---

## 7. 3DAAK — 3D Action Adventure Kit

**Upstream:** `TorqueGameEngines/Addon-3DAAK` @ `8684cd1a`  
**License:** MIT (`LICENSE.md`)

### Tree breakdown

| Layer | Path | Count |
|-------|------|-------|
| Engine | `Engine/` full T3D fork | ~2,069 C++ — **no addon-only dirs** vs standard T3D layout |
| Scripts | `Templates/Full/game/scripts/` | ~1,014 (inventory, weapons, items) |
| Art | templates | ~2,179 |

### Extractable kernel → `fuse_adventure`

**Primarily script-level systems** (no isolated C++ adventure module):

| System | Key scripts | Entry functions |
|--------|-------------|-----------------|
| Inventory | `scripts/server/inventory.cs` | `ShapeBase::pickup`, `incInventory`, `decInventory`, `hasInventory` |
| Items | `scripts/server/item.cs` | `ItemData::onPickup` |
| Weapons | `scripts/server/weapon.cs` | `Weapon::onPickup`, `Ammo::onInventory` |
| Interactions | various gameplay scripts | template-specific |

### Parity demo missions

| Mission | Path |
|---------|------|
| Outpost | `Templates/Full/game/levels/Outpost.mis` |
| Empty / terrain templates | `Empty Room.mis`, `Empty Terrain.mis` |

### Content pack

Full adventure template (FPS-derived), art, datablocks — **large content pack, small novel C++ kernel**.

---

## Cross-addon dependencies

```
BadBehaviour ──component──► GMK (shared SimComponent pattern)
UAISK ──scripts──► AFX (aiTargeting integrates with AFX spells)
Verve ──tracks──► AFX / T3D objects (VPostEffect, VSoundEffect, VCamera)
GuideBot ──AIPlayer──► T3D base + custom GuideBot:: namespace
```

U5 module boundaries should cut on **FUSE APIs**, not on legacy addon folder names.

---

## Gate U0 checklist item

- [x] Addon ore catalog lists kernel entry points for all seven addons
- [ ] Extraction PRs (U5) not started — ore remains read-only in submodules
