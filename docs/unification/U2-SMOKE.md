# FUSE U2 — One-Process Smoke & Legacy Quarantine

**Work package:** WP-02  
**Date:** 2026-09-15  
**Binary:** `fuse_runtime_smoke`  
**Build:** [BUILD.md](./BUILD.md)

---

## 1. What U2 proves today

`fuse_runtime_smoke` links in **one OS process**:

| Component | Target | Role |
|-----------|--------|------|
| FUSE core | `fuse_core` | `fuse::core::initialize()`, adaptive job pool |
| T3D quarantine | `fuse_t3d_legacy` | Prefixed `fuse_t3d_Con_*` (19 APIs), `fuse_t3d_StringTable_*`, optional `bitmapUtils` probe |
| T2D quarantine | `fuse_t2d_legacy` | Prefixed `fuse_t2d_Con_*` (19 APIs), `fuse_t2d_StringTable_*` |

Both legacy dimensions call `initialize()` / `shutdown()` sequentially on the **game thread** (main), exercising explicit init order without static-init collisions on shared `Con::` / `StringTable` globals.

```bash
cmake -B build -G Ninja -DCMAKE_CXX_COMPILER=g++-13 \
  -DFUSE_UMBRELLA=ON \
  -DFUSE_BUILD_CORE=ON \
  -DFUSE_BUILD_LEGACY=ON \
  -DFUSE_BUILD_SMOKE=ON \
  -DFUSE_BUILD_T3D=OFF \
  -DFUSE_BUILD_T2D=OFF

cmake --build build --target fuse_runtime_smoke
./build/Source/FUSE/Apps/RuntimeSmoke/fuse_runtime_smoke
```

ASan build:

```bash
cmake -B build-asan ... -DFUSE_SMOKE_ENABLE_ASAN=ON
```

---

## 2. Quarantine strategy (not a full Engine rewrite)

### 2.1 Static libraries

```
Source/FUSE/Legacy/T3D/  → fuse_t3d_legacy
Source/FUSE/Legacy/T2D/  → fuse_t2d_legacy
```

Each lib wraps **collision-surface shims** that model the highest-severity symbols from [symbol-collision-report.md](./symbol-collision-report.md):

- `Con::init`, `Con::execute`, `Con::executef`, `Con::printf`, `Con::errorf`, `Con::warnf`, `Con::getVariable`, `Con::setVariable`, `Con::getIntVariable`, `Con::setIntVariable`, `Con::getBoolVariable`, `Con::setBoolVariable`, `Con::addVariable`, `Con::getData`, `Con::setData`, `Con::isFunction`, `Con::threadSafeExecute`, `Con::addPathExpando`, `Con::expandPath`, `Con::collapsePath` → prefixed `fuse_t3d_Con_*` / `fuse_t2d_Con_*`
- `StringTable` singleton → `fuse_t3d_StringTable_intern`, `fuse_t2d_StringTable_intern`
- `ImageUtil::ddsCompress` mip loop (Tier A) → `fuse::legacy::t3d::image::compressMipsParallel` via `parallel_for_indices` (WP-11 P1 quarantine route; squish linked, no Engine `.cpp`)

Public C++ API: `fuse/legacy/t3d/api.hpp`, `fuse/legacy/t2d/api.hpp`.

### 2.2 Prefix tooling

| Tool | Purpose |
|------|---------|
| `Tools/FUSE/prefix_legacy_symbols.py --plan` | Scan raw Engine trees; emit JSON rename plan |
| `Tools/FUSE/prefix_legacy_symbols.py --verify-shims` | Confirm U2 shims export required prefixed symbols |

Full Engine source is **not** compiled into these libs yet — that is incremental work tracked below.

### 2.3 Init order (explicit, single-threaded)

1. `fuse::core::initialize()` — job scheduler with `computeWorkerCount()`
2. `fuse::legacy::t3d::initialize()` — string table, then console
3. `fuse::legacy::t2d::initialize()` — string table, then console
4. Reverse on shutdown

No parallel tick; no cross-thread legacy calls (per [architecture-parallel.md](./architecture-parallel.md) §9).

---

## 3. Honest blockers — full dual-legacy Engine init

| Blocker | Severity | Notes |
|---------|----------|-------|
| **311+ class ODR collisions** (`SimObject`, `GuiCanvas`, …) | Critical | Cannot `add_subdirectory` raw `Engine/source` + T2D `engine/source` into one link unit |
| **33 `Con::` API overlaps** | Critical | Requires prefix or namespace wrap across thousands of call sites |
| **237 basename header collisions** | High | Include-path isolation + prefixed libs mandatory |
| **Dual Gui\* stacks** | High | Only one Gui stack per process until Qt editor (U6) |
| **Dual StringTable / allocators** | High | R14 — singleton init order; FUSE core must own tables long-term (U3) |
| **Dual TorqueScript VMs** | High | Quarantine `compat/ts_t3d`, `compat/ts_t2d` (U3+) |
| **Platform thread trees** | Medium | T3D `platformWin32` vs T2D `platformAndroid` — compose via `fuse::platform` only |

### What still blocks “real” smoke (empty 3D world + Scene2D)

1. **Incremental source migration** — wrap T3D/T2D `.cpp` subsets into prefixed static libs using `prefix_legacy_symbols.py` plans; estimated multi-week, not U2 scope.
2. **Gui / render contexts** — cannot init both full gfx stacks; hybrid demo (U4) uses FUSE compositor, not dual legacy presenters.
3. **vcpkg / link closure** — full T3D link pulls 100+ deps; smoke intentionally avoids that until umbrella CI stabilises.

**U2 exit interpretation:** one-process link of **prefixed quarantine libs** + core init is proven; full legacy engine tick is **not** claimed.

### 3.1 Incremental quarantine progress (2026-09-19)

| Surface | Status | Next step |
|---------|--------|-----------|
| `Con::` logging + variables + paths + data | ✅ **33/33** APIs per dimension | class/command registration (`addCommand`, …) deferred to U3 |
| `StringTable` | ✅ shim singleton per dimension | Route to FUSE core table (U3 / R14) |
| Scene adapter stub | ✅ `LegacySceneObjectStub` ↔ `SceneObject2D/3D` | U3 `sim_object_bridge` slice landed (below) |
| SimObject bridge (U3 slice) | ✅ `LegacySimObjectStub` ↔ `fuse::Object` + scene nodes | `className` / `internalName` / `parentName` / `layer` round-trip; first real `SimObject` `.cpp` batch still blocked |
| StringTable quarantine | ✅ owned-copy `StringInternTable` per dimension | Route to FUSE core intern (U3 / R14) |
| Image mip compress | ✅ `compressMipsParallel` (squish + `parallel_for`) | `imageUtils.cpp` call-site swap deferred — Engine batch blocked (below) |
| Engine `.cpp` probe | ✅ **opt-in** `-DFUSE_T3D_LEGACY_ENGINE_PROBE=ON` | Batch 1–12 (see table); batch 13: `BitStream::setStringBuffer` dedup smoke, deepen `gbitmap_probe_stub` `copyRect` RGBA8 |
| Full `SimObject` | ⏳ **blocked** | 122-header closure, `IMPLEMENT_CONOBJECT`, Gui/Con/sim pulls, T2D `SimObject` ODR — **not** force-linking `simObject.cpp` |
| Gui / script VM | ⏳ blocked | See §3 table |

**SimObject blocker detail (unchanged):** T3D `simObject.cpp` (3,550 LOC) transitively pulls `console.cpp`, `simManager.cpp`, `guiInspector.h`, TorqueScript registration, and `sim/netObject.h`. T2D declares an identical `class SimObject : public ConsoleObject` — linking both trees exports colliding vtables. **Unblock path:** U3 `fuse::Object` extraction + single-console-host or curated adapter slice, not raw `simObject.cpp` drop-in.

**Engine probe detail:** Curated T3D `.cpp` batches compile under `-DFUSE_T3D_LEGACY_ENGINE_PROBE=ON` with quarantine stubs — no `simObject.cpp`, no full `platform/` tree.

| Batch | Engine sources | Smoke |
|-------|----------------|-------|
| 1 | `gfx/bitmap/bitmapUtils.cpp` | extrude5551, RGB→5551, half-float |
| 2 | `gfx/bitmap/loaders/ies/ies_loader.cpp`, `core/util/md5.cpp` | empty IES reject, MD5 digest |
| 3 | `core/util/hashFunction.cpp`, `core/util/commonSwizzles.cpp` | hash32/hash64, `getStringHash64`, `Swizzles::bgra` |
| 4 | `core/stream/stream.cpp`, `memStream.cpp`, `fileStream.cpp`, `core/util/path.cpp`, `byteBuffer.cpp`, `refBase.cpp`, `stringFunctions.cpp` | `MakeFourCC`, MemStream round-trip, FileStream `/tmp` write/read |
| 5 | `gfx/bitmap/loaders/bitmapSTB.cpp`, `core/util/tVector.cpp` | STB `readStreamFunc` loads 1×1 BMP from MemStream; `bitmapStbRegisterAnchor()` defeats static-lib GC of format registration |
| 6 | `gbitmap_probe_stub.cpp` (`readBitmap`/`readBitmapStream`), `core/util/timeClass.cpp`, `core/util/tSignal.cpp` | `readBitmapStream`/`readBitmap` path dispatch (no `Resource<GBitmap>`); unknown format rejected; `Torque::Time` date round-trip; `Signal<void>` notify/trigger |
| 7 | `gbitmap_probe_stub.cpp` (`writeBitmap`/`writeBitmapStream`, `Face::getAddress`), `gfx/bitmap/loaders/bitmapPng.cpp` (opt-in), `core/crc.cpp`, `core/bitVector.cpp`, `core/idGenerator.cpp`, `core/util/tDictionary.cpp` | `writeBitmapStream` STB TGA encode; `writeBitmap` PNG path round-trip (when libpng); `writeBitmapStream` PNG MemStream round-trip; unknown write format rejected; `CRC::calculateCRC`, `IdGenerator` alloc/free, `BitVector` set/test |
| 8 | `core/color.cpp`, `core/dataChunker.cpp` | `ColorI`/`LinearColorF` static constants; `StockColor::create`/`isColor`/`colorI` round-trip; `DataChunker` alloc/`isManagedByChunker`/`countUsedBytes` |
| 9 | `core/filterStream.cpp`, `core/resizeStream.cpp`, `core/tagDictionary.cpp` | `ResizeFilterStream` offset-window read; `TagDictionary` `addEntry`/`defineToId`/`idToDefine`; STB `writeBitmap` TGA path round-trip via `gbitmap_probe_stub` (PNG path unchanged when libpng present) |
| 10 | `core/strings/findMatch.cpp`, `core/tokenizer.cpp`, `rgb2xyz_probe_stub.cpp`, deepen `gbitmap_probe_stub` (`fillWhite`, `getSurfaceSize`, `sGetExtensionList`) | `FindMatch` wildcard `isMatch`/`findMatch`; `Tokenizer` `setBuffer`/`getNextToken`; `ConvertRGB::toXYZ`/`fromXYZ` round-trip (no `math_backend`); `GBitmap::checkForTransparency`, `fillWhite`, `getSurfaceSize`, `sGetExtensionList` |
| 11 | `core/stream/bitStream.cpp`, `core/stringBuffer.cpp`, `core/strings/unicode.cpp`, `rgb2luv_probe_stub.cpp`, deepen `gbitmap_probe_stub` (`getColor`/`setColor` RGBA8) | `BitStream` U32 write/read round-trip; `StringBuffer` UTF-8 `set`/`append`/`getPtr8`; `ConvertRGB::toLUV`/`toLUVScaled` (no `math_backend`); `GBitmap::getColor`/`setColor` RGBA8 round-trip |
| 12 | deepen `bitStream` smoke (Huffman `writeString`/`readString`, `writeClassId`/`readClassId`), `uuid_probe_stub.cpp`, `engineTypeInfo.h` shadow, deepen `gbitmap_probe_stub` (`getColor`/`setColor` RGB8, `allocateBitmap` `extrudeMipLevels`), extend `string_stub.cpp` (`startsWith`/`endsWith`) | `BitStream` class-id + Huffman string round-trip; `Torque::UUID` generate/`toString`/`fromString`; `String` `startsWith`/`endsWith`; `GBitmap` RGB8 color round-trip + pow2 mip chain (`getNumMipLevels` 3 for 4×4) |
| 13 | fix `BitStream::setStringBuffer` (re-enable shared-buffer pointer), deepen `gbitmap_probe_stub` (`copyRect` RGB/RGBA8 same-format blit) | `BitStream` `setStringBuffer` + Huffman delta dedup (`writeString` prefix reuse); `GBitmap::copyRect` RGBA8 1×1 blit |
| 14 | deepen `gbitmap_probe_stub` (`chopTopMips`, `getSurfaceSize` BC1/BC3 + mip-level edge cases) | `GBitmap::getSurfaceSize` RGBA8 mip0/1/2 sizes; BC1 8×8 + BC3 4×4 block layout; `chopTopMips(1)` on 4×4 RGBA8 mip chain → 2×2 base + preserved mip1 pixel |
| 15 | `core/threadStatic.cpp`, `core/bitRender.cpp` | `DITTS`/`ATTS` counter round-trip; `BitRender::render` triangle sets 1bpp bits in 8×8 buffer |
| 16 | deepen `filterStream`/`stream` smoke (`FilterStream` delegate, typed `Stream` read/write), `core/strings/stringUnit.cpp`, deepen `gbitmap_probe_stub` (`copyRect` RGB8) | `FilterStream` `getStreamSize`/`getPosition`/`setPosition`/`hasCapability`/`detachStream` via `ResizeFilterStream`; `Stream` U32/F32 typed round-trip on `MemStream`; `ByteBuffer` append/ref-count/`getCopy`; `StringUnit::getUnit`/`getUnitCount`/`getUnits`; `GBitmap::copyRect` RGB8 1×1 blit |

**TagDictionary flake fix (batch 15):** `TagDictionary::hashDefine` kept modulo in `dsize_t` — old `(S32)((dsize_t)define >> 2) % tsize` truncated on 64-bit ASLR (~50% `defineToId` miss); probe `string_table_stub` uses `unordered_set` stable intern; `tagDictionarySmoke` uses string-literal entries; probe skips unused global `tagDictionary`; `avar` is `thread_local`.

Under `-std=c++17` GCC drops the legacy `linux` macro; probe compile adds `-DLINUX=1` so `types.gcc.h` pulls `types.posix.h` (`dsize_t`, `FileTime`). Quarantine stubs in `engine_probe/`: `platform_stub.cpp` (`dMem*`, `dMalloc`, `dMalloc_r`), `platform_assert_stub.cpp` (`avar` **`thread_local`**, `PlatformAssert::processAssert`, `Platform::debugBreak` — for `bitmapPng.cpp` AssertISV paths), extended `string_stub.cpp` (path + hash + **`startsWith`/`endsWith`** String surface — **not** full `str.cpp`), **`string_table_stub.cpp`** (owned-copy intern set — **stable `StringTableEntry` pointers**), `frame_allocator_stub.cpp`, shadow headers (`console/console.h`, `console/consoleTypes.h`, `console/consoleInternal.h`, `console/consoleObject.h` — `AbstractClassRep` static tables for `BitStream::writeClassId`, `console/engineTypeInfo.h` — minimal `EngineFieldTable` for `uuid.h`, `console/engineAPI.h`, `console/enginePrimitives.h`, `core/color.h` shim for non-color TUs, `core/resource.h`, `core/stringTable.h`, `gfx/bitmap/imageUtils.h`, `platform/profiler.h`, `platform/platformNet.h` + `Net::sendto` stub, `platform/threads/threadPool.h`), `color_cpp_prelude.h` + `color_real_wrapper.h` (force-include on `color.cpp` / `color_probe_smoke.cpp` only — real `core/color.h` + `DefineEngineFunction` stub + MODULE macro no-op), `fs_volume_stub.cpp` (POSIX `Torque::FS::OpenFile`/`CreatePath`), `thread_pool_stub.cpp` (sync `WorkItem` dispatch), `console_stub.cpp` (`Con::printf`/`errorf`/`warnf`), `console_object_stub.cpp` (`AbstractClassRep::NetClassCount`/`NetClassBitSize`), `math_backend_probe_stub.cpp` (scalar `float3`/`float4`/`mat44` dispatch for `bitStream.cpp` — not full `math_backend.cpp` SIMD paths), `bitstream_prelude.h` (force-include `tVector.h` for Huffman `Vector<>` members), `gbitmap_probe_stub.cpp` (minimal `GBitmap` alloc/register/transparency/**read/writeBitmap dispatch** + `Face::getAddress` + **`copyRect` RGB/RGBA8** + **`fillWhite`/`getSurfaceSize`/`sGetExtensionList`/`getColor`/`setColor` RGB/RGBA8** + **`allocateBitmap` `extrudeMipLevels` mip chain** + **`chopTopMips`** + **`getSurfaceSize` BC1/BC3 block layout** — **not** full `gBitmap.cpp` / `resourceManager.cpp`), `rgb2xyz_probe_stub.cpp` (sRGB↔XYZ matrix multiply — **not** full `rgb2xyz.cpp` / `math_backend`), `rgb2luv_probe_stub.cpp` (`toLUV`/`toLUVScaled` — **not** full `rgb2luv.cpp` / `mPoint2`/`mPoint3`), **`uuid_probe_stub.cpp`** (v1 `UUID::generate`/`toString`/`fromString`/`getHash` — **not** full `uuid.cpp` / `UUIDEngineExport`). **`fourcc.h` is header-only** (no `fourcc.cpp` in Engine tree) — batch 4 smoke exercises `MakeFourCC` directly. **`PROFILE_START_IF` shadow** expands to `if (false) { (void)0; }` so bitmapSTB's if/else profiler chain parses with profiler disabled. **`dataChunker.cpp`** is an include anchor — implementation is header-only templates in `dataChunker.h`.

**`bitmapPng.cpp` gate (batch 7):** CMake `find_package(PNG)` + `find_package(ZLIB)`; when both found, links `bitmapPng.cpp` with bundled `Engine/lib/lpng` headers + system libs, defines `FUSE_T3D_LEGACY_ENGINE_PROBE_PNG=1` on `fuse_t3d_legacy` and `fuse_runtime_smoke`. `bitmapPngRegisterAnchor()` defeats static-lib GC of PNG format registration. Skipped on hosts without dev packages.

**Still blocked:** **`gBitmap.cpp` / `ddsFile.cpp` / `resourceManager.cpp`** — full 3k+ LOC bitmap I/O, `Resource<GBitmap>` load path, DDS encode/decode, `engineAPI` registration; probe uses **gBitmap subset stub** with registration dispatch + batch-10/16 pixel helpers (`fillWhite`, `getSurfaceSize` **BC1/BC3 block layout + mip-level sizes**, `sGetExtensionList`, **`copyRect` RGB/RGBA8** (batch 16 deepens RGB8), **`getColor`/`setColor` RGB/RGBA8**, **`allocateBitmap` `extrudeMipLevels`**, **`chopTopMips`**) — still no `extrudeMipLevels()` filter pass, cubemap face paths, or compressed-bitmap pixel I/O. **`core/util/str.cpp`** — pulls `console/engineAPI.h`, `math/mMathFn.h`, mutex/profiler/unicode/`StrFormat` closure; probe keeps **`string_stub.cpp`** (batch 12 adds `startsWith`/`endsWith`; not full `str.cpp` / `StringBuilder`). **`core/util/uuid.cpp`** — full TU pulls `UUIDEngineExport` / `engineTypeInfo.cpp`; probe uses **`uuid_probe_stub.cpp`** (v1 generate/parse only). **`rgb2xyz.cpp` / `rgb2luv.cpp`** — full Engine TUs pull `math/mMatrix.h` / `math_backend`; probe uses **`rgb2xyz_probe_stub.cpp`** for `toXYZ`/`fromXYZ` and **`rgb2luv_probe_stub.cpp`** for `toLUV`/`toLUVScaled` only (no `fromLUV`). **`bitmapSTB.cpp` IES branch** compiles but is not smoke-tested (needs FS + IES sidecar PNG). **STB `writeStreamFunc`** prefixes chunks via `stbiWriteFunc` — not raw image bytes; stream round-trip smoke uses PNG (`pngWriteDataFn`) when libpng present, else TGA encode-only via `writeBitmapStreamRoundTripSmoke`. **STB JPG/HDR file-path write** not smoke-tested (BMP path round-trip covered in batch 9). **`stringBuffer.cpp`** debug `DefineEngineFunction` paths skipped (no `TORQUE_DEBUG`).

**SimObject bridge detail (U3 slice, no `simObject.cpp`):** `fuse/legacy/sim_object_bridge.hpp` defines `LegacySimObjectStub` (identity + transform distilled from Torque `SimObject`). `bridgeToObject` / `bridgeFromObject` propagate `legacyClassName`, `legacyInternalName`, `legacyParentName` on `fuse::Object` plus `layer` on `SceneObject2D`. Per-dimension `importSimObject` / `exportSimObject` compose with scene adapters — reduces ODR pressure vs linking dual Engine `SimObject` trees. Smoke exercises T2D/T3D round-trip on main thread.

---

## 4. Remaining symbol conflicts (trend tracking)

| Metric (U0 baseline) | U2 status (2026-09-19) |
|----------------------|------------------------|
| `Con::` collisions (33) | ✅ **33/33 shimmed** per dimension — `init`, `execute`, `executef`, `executeArgv`, `executefArgv`, `evaluate`, `evaluatef`, `printf`, `errorf`, `warnf`, `getVariable`, `setVariable`, `getIntVariable`, `setIntVariable`, `getBoolVariable`, `setBoolVariable`, `getFloatVariable`, `setFloatVariable`, `addVariable`, `addConstant`, `removeVariable`, `addVariableNotify`, `removeVariableNotify`, `getData`, `setData`, `isFunction`, `threadSafeExecute`, `tabComplete`, `addPathExpando`, `removePathExpando`, `isPathExpando`, `getPathExpandoCount`, `expandPath`, `collapsePath` + `StringTable_intern` — **0 open** |
| Class collisions (311) | **0 merged** — adapters deferred to U3–U5 |
| Basename collisions (237) | **0 merged** — include isolation via separate libs |
| IMPLEMENT_CONOBJECT dupes (1) | **Unchanged** (`SimXMLDocument`) |
| ThreadPool Tier A (image compress) | **1 route** — `image_compress_route.cpp` replaces `CompressJob`/`ThreadPool` pattern in quarantine |

Re-run collision inventory at U3 when first real Engine `.cpp` batches land in quarantine libs.

---

## 5. WP-03 job spine progress

| Item | U1 stub | U2 | WP-03 (follow-up PR) |
|------|---------|-----|----------------------|
| `computeWorkerCount()` | ✅ | ✅ | ✅ |
| `JobScheduler` | no-op | work-stealing thread pool | + cooperative job fiber per worker |
| `JobCounter::wait()` | no-op | blocking wait (CV) | **worker yield via POSIX fibers** |
| `parallel_for` | serial only | parallel when workers > 0 | ✅ unchanged API |
| Cooperative fibers | — | deferred | ✅ Linux/macOS ucontext — [wp03-fiber-remaining.md](./wp03-fiber-remaining.md) |

**U2 blockers unchanged:** full Engine source in quarantine libs (§3), symbol prefix at scale, dual script VMs.

**New since U2 (does not unblock §3):** FUSE logger + VFS stub wired in smoke; greenfield `fuse::Object` → `SceneObject2D` → `SceneObject3D` hierarchy tests.

---

## 6. Gate U2 checklist

| Item | Status |
|------|--------|
| `fuse_t3d_legacy` + `fuse_t2d_legacy` CMake targets | ✅ |
| `fuse_runtime_smoke` one-process binary | ✅ |
| Prefix strategy documented + tooling | ✅ |
| ASan-friendly smoke target | ✅ `FUSE_SMOKE_ENABLE_ASAN` |
| Explicit init order documented | ✅ §2.3 |
| Remaining conflicts listed | ✅ §4 |
| Full Engine dual-init | ⏳ **Blocked** — see §3 |

---

## 7. Smoke reliability (batch tracking)

| Metric | Value |
|--------|-------|
| **HEAD** | `21925931d` (batch 16) |
| **Consecutive smoke** | 10/10 Release + 10/10 ASan (`fuse_runtime_smoke` loop) |
| **verify-shims** | `python3 Tools/FUSE/prefix_legacy_symbols.py --verify-shims --dimension t3d/t2d` |
| **broadphase** | `ctest -R fuse_physics_broadphase_tests` |

**Remaining Engine probe batches (post-16):** `memVolume.cpp` / `volume.cpp` (FS/platform closure), `stringUnit` set/remove paths (covered: get/getCount/getUnits), `streamObject.cpp` (SimObject), full `gBitmap.cpp` I/O, `str.cpp`/`uuid.cpp` real TUs, zip archive subtree, Gui/script VM stacks.
