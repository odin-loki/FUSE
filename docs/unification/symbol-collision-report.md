# FUSE U0 — Symbol Collision Report

**Phase:** U0 Inventory & collision map  
**Date:** 2026-09-15  
**Evidence base:** FUSE `main` at commit inventory time; submodules initialised (`third_party/Torque2D` @ `e7b0011`, addons @ pinned SHAs in `.gitmodules`)  
**Scope:** Duplicate globals, classes, filenames, and linker-visible symbols across T3D (`Engine/source`) and T2D (`third_party/Torque2D/engine/source`)

---

## 1. Executive summary

Linking raw T3D and T2D engine sources into **one process without renaming or static-library quarantine will fail**. Both trees inherit GarageGames-era Torque DNA and share hundreds of colliding identifiers at the C++ type, filename, and `Con::` API layers.

| Metric | T3D (`Engine/source`) | T2D (`third_party/Torque2D/engine/source`) | Overlap |
|--------|----------------------|---------------------------------------------|---------|
| Source files (`.cpp`/`.h`/`.cc`/`.c`) | 2,513 | 1,281 | — |
| Basename file collisions | — | — | **237** |
| Class name collisions (line-start `class` decl, filtered) | ~1,900+ unique | ~870+ unique | **311** |
| Class name collisions (whole-file regex, unfiltered) | 2,417 | 1,180 | 445 |
| Struct name collisions (regex) | 958 | 527 | 195 |
| `namespace` name collisions | 134 | 97 | 39 |
| `Con::` function name collisions | 88 unique | 40 unique | **33** |
| `IMPLEMENT_CONOBJECT` registered name collisions | 411 registrations | 8 registrations | **1** (`SimXMLDocument`) |

**Conclusion for U2:** Compile each legacy dimension as a **prefixed static library** (`fuse_t3d_legacy`, `fuse_t2d_legacy`) with symbol renaming for C-linkage and shared Torque globals. Do **not** `add_subdirectory` both raw trees into one link unit.

---

## 2. Methodology

1. **Submodule init:** `git submodule update --init --recursive` — required; T2D and addons are empty without it.
2. **File inventory:** `find` over both `engine/source` trees; basename intersection.
3. **Class/struct extraction:** Python scan of `class Foo` at line start (filters comment false-positives like `class is designed for`); supplemental regex pass for totals.
4. **Console collision:** `Con::identifier(` pattern across both trees.
5. **Runtime registration:** `IMPLEMENT_CONOBJECT(ClassName)` scan — measures TorqueScript-exposed type name collisions (distinct from C++ link symbols).
6. **Manual spot-check:** High-value types (`SimObject`, `GuiCanvas`, `BitStream`, `GameConnection`) confirmed in both trees via ripgrep.

Scripts are reproducible from the repo roots; counts may drift slightly if upstream submodules move.

---

## 3. Subsystem collision map

### 3.1 Console & simulation (highest severity)

| Collision domain | Representative symbols | T3D path | T2D path | Link risk |
|------------------|------------------------|----------|----------|-----------|
| Core object model | `SimObject`, `SimGroup`, `SimSet`, `SimEvent`, `SimFieldDictionary` | `Engine/source/console/simObject.h` | `third_party/Torque2D/engine/source/sim/simObject.h` | **ODR / vtable** |
| Console machinery | `ConsoleObject`, `AbstractClassRep`, `ConcreteClassRep`, `CodeBlock`, `Namespace`, `Dictionary` | `Engine/source/console/` | `third_party/Torque2D/engine/source/console/` | **ODR** |
| Console logging | `ConsoleLogger` | `console/consoleLogger.h` | `console/consoleLogger.h` | ODR |
| Script AST | `ast.h` (basename collision) | `console/torquescript/ast.h` | `console/ast.h` | Include ambiguity |
| Networking events | `BitStream`, `GameConnection` | `core/stream/bitStream.h`, `T3D/gameBase/gameConnection.h` | `io/bitStream.h`, `game/gameConnection.h` | **ODR** |

**`Con::` API overlap (33 functions):** `execute`, `executef`, `printf`, `errorf`, `warnf`, `getVariable`, `setVariable`, `getIntVariable`, `setIntVariable`, `getBoolVariable`, `setBoolVariable`, `addVariable`, `init`, `expandPath`, `collapsePath`, `getData`, `setData`, `isFunction`, `threadSafeExecute`, and others. Both define `namespace Con { ... }` with overlapping free functions — **guaranteed link failure** if both translation units export the same symbols.

**Evidence — identical class declaration pattern:**

```233:233:third_party/Torque2D/engine/source/sim/simObject.h
class SimObject: public ConsoleObject, public TamlCallbacks
```

```240:240:Engine/source/console/simObject.h
class SimObject: public ConsoleObject, public TamlCallbacks
```

### 3.2 Platform (high severity)

| Symbols | Notes |
|---------|-------|
| `PlatformAssert`, `PlatformFont`, `PlatformThreadStorage` | Both trees |
| `Thread`, `Mutex`, `MutexHandle`, `Semaphore` | Threading primitives |
| `Input`, `InputManager`, `InputDevice` | T3D under `platform/input/`; T2D under `platform/` + `input/` |
| `FileStream`, `FileDialog`, `OpenFileDialog`, `SaveFileDialog` | I/O and native dialogs |
| `NetAsync`, `NetSocket`, `StdConsole` | Platform networking helpers |

**36 basename collisions** in platform-related files (e.g. `platform.h`, `platformAssert.h`, `mutex.h`, `platformInput.h`, `fileStream.h`).

**Platform coverage divergence (not a symbol issue, but affects merge):**

| Platform | T3D (`Engine/source`) | T2D (`engine/source`) |
|----------|----------------------|------------------------|
| Desktop Win/Linux/macOS | `platformWin32`, `platformPOSIX`, `platformMac`, `platformX11`, `platformSDL`, `platformX86UNIX` | `platformWin32`, `platformX86UNIX`, `platformOSX` |
| Mobile / web | — | `platformAndroid`, `platformiOS`, `platformEmscripten` |

### 3.3 Math (medium–high severity)

**19 filtered class collisions**, including: `Point2F`, `Point2I`, `Point3F`, `Point3I`, `Point4F`, `MatrixF`, `Box3F`, `PlaneF`, `QuatF`, `RectF`, `RectI`, `SphereF`, `AngAxisF`, `PlaneTransformer`, `RectClipper`.

Basename collisions: `mPoint.cpp`, `mMatrix.h`, `mBox.h`, `mPlane.h`, `mQuat.h`, `mRect.h`, `mSphere.h`, `bitMatrix.h`.

Math types are header-heavy and often inlined — duplicate definitions cause **ODR violations** even when behaviour is identical.

### 3.4 GUI (high severity for editor unification)

**45+ Gui* class collisions**, including: `GuiCanvas`, `GuiControl`, `GuiControlProfile`, `GuiButtonCtrl`, `GuiEditCtrl`, `GuiScrollCtrl`, `GuiInspector` (+ type variants), `GuiConsole`, `GuiListBoxCtrl`, `GuiTabBookCtrl`, and many more.

**28 basename collisions** under `gui*`.

Both engines ship a full retained-mode Gui* stack. FUSE plan (U6) replaces product UI with Qt 6; until then, **only one Gui* stack may link per process**.

### 3.5 Utilities, streams, strings (high severity)

| Area | Colliding basenames (sample) |
|------|------------------------------|
| Streams | `stream.h`, `bitStream.h`, `fileStream.h`, `filterStream.h`, `resizeStream.h`, `zipSubStream.h` |
| Strings | `stringTable.h`, `stringBuffer.h`, `stringStack.h`, `stringUnit.h` |
| Containers | `bitVector.h`, `bitVectorW.h`, `dataChunker.h` |
| Delegates | `FastDelegate.h` (both trees) |

**111 util-related class collisions** in the broad util bucket (includes `DataChunker`, `BitVector`, `EventManager`, `FactoryCache`, delegate templates).

### 3.6 Assets & persistence (medium severity)

**33 Taml/Asset class collisions:** `AssetBase`, `AssetManager`, `AssetPtr`, `Taml`, `TamlXmlParser`, `TamlJsonParser`, `ModuleDefinition`, `ModuleManager`, and visitor/update types.

T2D's asset/module story and T3D's newer AssetPtr/Taml paths share naming but have diverged implementation details.

### 3.7 Graphics (partial overlap)

| Symbol | T3D | T2D |
|--------|-----|-----|
| `GBitmap`, `GFont` | `gfx/bitmap/gBitmap.h` | `graphics/gBitmap.h` |
| `ColorI`, `MatrixF`, `Point2F`, `RectI`, `Stream` | Used across gfx | Used across graphics |

T3D gfx is **417 files** across `gfx/`, `renderInstance/`, `shaderGen/`, etc. T2D graphics is **35 files** in `graphics/` — largely different architecture; fewer symbol collisions but incompatible render paths.

### 3.8 Audio (low symbol overlap)

| T3D | T2D |
|-----|-----|
| `sfx/` — 65 files | `audio/` — 19 files |

Only **1 filtered class collision** (`AudioDescription`). Backend and API shapes differ; collision risk is lower than console/platform.

### 3.9 Physics (low symbol overlap, high semantic divergence)

| T3D | T2D |
|-----|-----|
| `collision/` — 27 files (internal T3D collision) | `Box2D/` — 113 files (in-tree Box2D) |

**1 trivial class collision** (`is` false-positive filtered out in practice). No shared physics API — separate worlds required (see risk register).

### 3.10 Script VM (high architectural collision)

| | T3D | T2D |
|---|-----|-----|
| Script tree | `ts/`, `cinterface/` — 81 files | Script integrated in `console/` (no separate `ts/`) |
| Parser | Bison/flex under `console/torquescript/` | Bison/flex under `console/` |

Dual TorqueScript dialects and duplicate `CodeBlock` / compiler infrastructure. U0–U2 plan: quarantine `compat/ts_t3d` and `compat/ts_t2d`.

---

## 4. Filename collision catalogue (237 basenames)

Full list available via:

```bash
comm -12 \
  <(find Engine/source -name '*.h' -o -name '*.cpp' | xargs -I{} basename {} | sort -u) \
  <(find third_party/Torque2D/engine/source -name '*.h' -o -name '*.cpp' | xargs -I{} basename {} | sort -u)
```

**Critical basename collisions (link/include hazard):**

| Category | Count | Examples |
|----------|-------|----------|
| Console / sim | 17 | `simObject.h`, `codeBlock.h`, `consoleObject.h`, `simDictionary.h` |
| Platform | 36 | `platform.h`, `mutex.h`, `thread.h`, `fileStream.h`, `netConnection.h` |
| Math | 17 | `mMatrix.h`, `mPoint.cpp`, `mBox.h` |
| Gui | 28 | `guiCanvas.h`, `guiControl.h`, `guiButtonCtrl.h` |
| Util / I/O | 25 | `stream.h`, `bitStream.h`, `stringTable.h`, `dataChunker.h` |
| Sim / game | 12 | `gameConnection.h`, `bitStream.h`, `simBase.h` |
| Gfx | 2 | `gBitmap.h`, `gFont.h` |

---

## 5. Filtered class collision groups (311 total)

| Group | Count | Sample identifiers |
|-------|-------|-------------------|
| Gui* | 45 | `GuiCanvas`, `GuiControl`, `GuiInspector`, … |
| Other (streams, delegates, misc) | 169 | `ActionMap`, `DataChunker`, `FastDelegate`, `Stream`, … |
| Assets / Taml | 33 | `Taml`, `AssetManager`, `ModuleDefinition`, … |
| Sim* | 19 | `SimObject`, `SimGroup`, `SimSet`, `SimDataBlock`, … |
| Math* | 19 | `MatrixF`, `Point3F`, `QuatF`, `Box3F`, … |
| Console* | 10 | `ConsoleObject`, `CodeBlock`, `Namespace`, `Dictionary` |
| Net/Sim | 5 | `BitStream`, `GameConnection`, `ConnectionProtocol` |
| Platform* | 8 | `Input`, `InputManager`, `Mutex`, `Thread` |
| Gfx* | 2 | `GBitmap`, `GFont` |
| Audio* | 1 | `AudioDescription` |

---

## 6. FUSE root pre-merged addon symbols (inventory note)

The FUSE root `Engine/source/` already contains **partially integrated** addon engine code (ore merged upstream of U5 extraction):

| Directory | Source files | Origin |
|-----------|-------------|--------|
| `Engine/source/afx/` | 218 | AFX (Arcane FX) |
| `Engine/source/Verve/` | 163 | Verve cinematic core |

These do **not** collide with T2D (T3D-only addons) but reinforce that addon symbols already live in the T3D link unit. U5 extraction should move these to `fuse_fx` / `fuse_cinematics` modules without duplicating into a second Engine tree.

---

## 7. Recommended mitigations (for U1–U2)

| Priority | Tactic | Applies to |
|----------|--------|------------|
| P0 | Separate static libs `fuse_t3d_legacy`, `fuse_t2d_legacy` | All |
| P0 | C symbol prefix script (`fuse_t3d_`, `fuse_t2d_`) for `extern "C"` and global singletons | Platform, console |
| P0 | Rename or namespace-wrap `Con::` in one tree | Console |
| P1 | Header include path isolation — no shared `-I` for both `console/` trees | Build |
| P1 | Ban single link of both `Gui*` stacks; Qt 6 for editor (U6) | Gui |
| P2 | Shared math via FUSE core (`fuse::math`) replacing both copies (Track A P2) | Math |
| P2 | Single `fuse::BitStream` facade (U3) | Net serialization |

---

## 8. Gate U0 checklist item

- [x] Collision report published under `docs/unification/`
- [ ] Trend tracking begins at U2 (remaining conflicts list)

**Next consumer:** Phase U1 umbrella CMake and Phase U2 one-process smoke tests use this report to choose prefix vs multiprocess fallback (multiprocess is **temporary scaffold only** per prestarter §7).
