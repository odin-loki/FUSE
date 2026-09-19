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
| `Con::` logging + variables + paths + data | ✅ **19/33** APIs per dimension | remaining doc-listed overlaps (`executef` variants, notify APIs, …) |
| `StringTable` | ✅ shim singleton per dimension | Route to FUSE core table (U3 / R14) |
| Scene adapter stub | ✅ `LegacySceneObjectStub` ↔ `SceneObject3D` | Wire first real `SimObject` batch (U3) |
| Image mip compress | ✅ `compressMipsParallel` (squish + `parallel_for`) | `imageUtils.cpp` call-site swap deferred — Engine batch blocked (below) |
| Engine `.cpp` probe | ✅ **opt-in** `-DFUSE_T3D_LEGACY_ENGINE_PROBE=ON` | `bitmapUtils.cpp` + `platform_stub.cpp`; needs `LINUX=1` under C++17 for `types.posix.h` |
| Full `SimObject` | ⏳ **blocked** | 122-header closure, `IMPLEMENT_CONOBJECT`, Gui/Con/sim pulls, T2D `SimObject` ODR — **not** force-linking `simObject.cpp` |
| Gui / script VM | ⏳ blocked | See §3 table |

**SimObject blocker detail (unchanged):** T3D `simObject.cpp` (3,550 LOC) transitively pulls `console.cpp`, `simManager.cpp`, `guiInspector.h`, TorqueScript registration, and `sim/netObject.h`. T2D declares an identical `class SimObject : public ConsoleObject` — linking both trees exports colliding vtables. **Unblock path:** U3 `fuse::Object` extraction + single-console-host or curated adapter slice, not raw `simObject.cpp` drop-in.

**Engine probe detail:** Smallest collision-free T3D candidate is `gfx/bitmap/bitmapUtils.cpp`. Under `-std=c++17` GCC drops the legacy `linux` macro; probe compile adds `-DLINUX=1` so `types.gcc.h` pulls `types.posix.h` (`dsize_t`, `FileTime`). Quarantine `platform_stub.cpp` supplies `Float_Inf` + `dMem*`. Smoke calls `engineProbe::bitmapExtrude5551Smoke()` when probe flag is ON.

---

## 4. Remaining symbol conflicts (trend tracking)

| Metric (U0 baseline) | U2 status (2026-09-19) |
|----------------------|------------------------|
| `Con::` collisions (33) | **19 shimmed** per dimension (`init`, `execute`, `executef`, `printf`, `errorf`, `warnf`, `getVariable`, `setVariable`, `getIntVariable`, `setIntVariable`, `getBoolVariable`, `setBoolVariable`, `addVariable`, `getData`, `setData`, `isFunction`, `threadSafeExecute`, `expandPath`, `collapsePath`) + `addPathExpando` + `StringTable_intern` — **14 open** |
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
