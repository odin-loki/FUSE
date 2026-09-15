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
| T3D quarantine | `fuse_t3d_legacy` | Prefixed `fuse_t3d_Con_*`, `fuse_t3d_StringTable_*` |
| T2D quarantine | `fuse_t2d_legacy` | Prefixed `fuse_t2d_Con_*`, `fuse_t2d_StringTable_*` |

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

- `Con::execute`, `Con::printf` → `fuse_t3d_Con_execute`, `fuse_t2d_Con_execute`, …
- `StringTable` singleton → `fuse_t3d_StringTable_intern`, `fuse_t2d_StringTable_intern`

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

---

## 4. Remaining symbol conflicts (trend tracking)

| Metric (U0 baseline) | U2 status |
|----------------------|-----------|
| `Con::` collisions (33) | **3 shimmed** per dimension (`execute`, `printf` + intern) — **30 open** |
| Class collisions (311) | **0 merged** — adapters deferred to U3–U5 |
| Basename collisions (237) | **0 merged** — include isolation via separate libs |
| IMPLEMENT_CONOBJECT dupes (1) | **Unchanged** (`SimXMLDocument`) |

Re-run collision inventory at U3 when first real Engine `.cpp` batches land in quarantine libs.

---

## 5. WP-03 job spine progress (same PR)

| Item | U1 stub | U2 |
|------|---------|-----|
| `computeWorkerCount()` | ✅ | ✅ |
| `JobScheduler` | no-op | **work-stealing thread pool** |
| `JobCounter::wait()` | no-op | **blocking wait** |
| `parallel_for` | serial only | **parallel when workers > 0** |
| Cooperative fibers | — | **deferred** — API surface kept; OS threads back workers |

Remaining fiber work: stack switching (`fuse::platform::fiber`), `JobCounter::wait()` yield on worker threads without blocking OS threads, 32/64 KiB stacks from `recommendedFiberStackBytes()`.

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
