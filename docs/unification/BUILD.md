# FUSE Umbrella Build (U1)

**Work package:** WP-01  
**Status:** Initial umbrella CMake + `fuse_core` stub  
**Architecture:** [architecture-parallel.md](./architecture-parallel.md) (platform + worker policy)

The root `CMakeLists.txt` is the **FUSE umbrella** entry point. It can build:

| Option | Default | Target |
|--------|---------|--------|
| `FUSE_BUILD_CORE` | ON | `fuse_core` static library + optional `fuse_core_tests` |
| `FUSE_BUILD_T3D` | ON | Legacy Torque3D app (`TORQUE_APP_NAME`, default `Torque3D`) |
| `FUSE_BUILD_T2D` | ON | Torque2D engine via `fuse_torque2d` external project (if submodule present) |
| `FUSE_BUILD_CORE_TESTS` | ON | CTest targets `fuse_core_worker_count`, `fuse_core_jobs` |
| `FUSE_BUILD_LEGACY` | ON | `fuse_t3d_legacy`, `fuse_t2d_legacy` quarantine static libs |
| `FUSE_BUILD_SMOKE` | ON | `fuse_runtime_smoke` one-process test binary |
| `FUSE_BUILD_HYBRID_DEMO` | ON | `demo_hybrid_hud` U4 hybrid frame demo (software renderer) |
| `FUSE_BUILD_MODULES` | ON | L3 feature modules (`fuse_cinematics`, …) |
| `FUSE_SMOKE_ENABLE_ASAN` | OFF | AddressSanitizer for smoke target |

Legacy Torque3D-only workflow is **unchanged**:

```bash
cmake -B build -DTORQUE_APP_NAME=Torque3D
cmake --build build
```

When `TORQUE_APP_NAME` is set and `FUSE_UMBRELLA` is not forced ON, the root CMake delegates to `cmake/LegacyTorque3D.cmake` (same behaviour as pre-U1).

---

## Prerequisites

```bash
git submodule update --init --recursive
```

Minimum for `fuse_core` only:

- CMake ≥ 3.21
- C++17 compiler (GCC 11+, Clang 14+, MSVC 2019+)

Full T3D/T2D builds need the dependencies documented in upstream Torque READMEs (vcpkg, Ninja, platform SDKs).

---

## Configure — desktop (Linux example)

### fuse_core + smoke (U2 fast CI path)

```bash
cmake -B build -G Ninja \
  -DCMAKE_CXX_COMPILER=g++-13 \
  -DFUSE_UMBRELLA=ON \
  -DFUSE_BUILD_CORE=ON \
  -DFUSE_BUILD_CORE_TESTS=ON \
  -DFUSE_BUILD_LEGACY=ON \
  -DFUSE_BUILD_SMOKE=ON \
  -DFUSE_BUILD_T3D=OFF \
  -DFUSE_BUILD_T2D=OFF

cmake --build build
ctest --test-dir build --output-on-failure
./build/Source/FUSE/Apps/RuntimeSmoke/fuse_runtime_smoke
./build/Source/FUSE/Apps/HybridHud/demo_hybrid_hud
```

See [U2-SMOKE.md](./U2-SMOKE.md) for quarantine strategy and blockers. U4 hybrid notes: [U4-HYBRID-FRAME.md](./U4-HYBRID-FRAME.md).

### fuse_core + tests only (fastest CI path)

```bash
cmake -B build -G Ninja \
  -DCMAKE_CXX_COMPILER=g++-13 \
  -DFUSE_UMBRELLA=ON \
  -DFUSE_BUILD_CORE=ON \
  -DFUSE_BUILD_CORE_TESTS=ON \
  -DFUSE_BUILD_T3D=OFF \
  -DFUSE_BUILD_T2D=OFF

cmake --build build
ctest --test-dir build --output-on-failure
```

### Full umbrella (T3D + T2D + fuse_core)

```bash
cmake -B build -G Ninja \
  -DFUSE_UMBRELLA=ON \
  -DTORQUE_APP_NAME=Torque3D \
  -DFUSE_BUILD_CORE=ON \
  -DFUSE_BUILD_T3D=ON \
  -DFUSE_BUILD_T2D=ON

cmake --build build
```

Torque2D is built in `${CMAKE_BINARY_DIR}/third_party/Torque2D` because its CMake assumes a top-level source tree. If `third_party/Torque2D` is empty, configure continues with a status message.

---

## Platform options (`FUSE_PLATFORM_*`)

`cmake/FusePlatforms.cmake` sets cache options aligned with T2D’s platform matrix:

| Cache option | When set |
|--------------|----------|
| `FUSE_PLATFORM_WINDOWS` | Win32 desktop |
| `FUSE_PLATFORM_LINUX` | Linux desktop |
| `FUSE_PLATFORM_MACOS` | macOS desktop |
| `FUSE_PLATFORM_IOS` | iOS / simulator toolchain |
| `FUSE_PLATFORM_ANDROID` | Android NDK toolchain |

These are auto-detected from `CMAKE_SYSTEM_NAME` / `ANDROID` / `IOS`. They drive `fuse_core` compile definitions (`FUSE_PLATFORM_MOBILE`, fiber stack defaults, worker profile selection).

---

## Mobile — Android NDK (`fuse_core` stub)

Cross-compile **only** `fuse_core` (no T3D/T2D link required for U1):

```bash
export ANDROID_NDK_HOME=/path/to/ndk
cmake -B build-android -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-24 \
  -DFUSE_UMBRELLA=ON \
  -DFUSE_BUILD_CORE=ON \
  -DFUSE_BUILD_CORE_TESTS=OFF \
  -DFUSE_BUILD_T3D=OFF \
  -DFUSE_BUILD_T2D=OFF

cmake --build build-android --target fuse_core
```

See `.github/workflows/fuse-core-android.yml` for CI.

---

## Mobile — iOS simulator (`fuse_core` stub)

Requires **macOS** + Xcode. Workflow stub: `.github/workflows/fuse-core-ios.yml`.

```bash
cmake -B build-ios -G Xcode \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_SYSROOT=iphonesimulator \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DFUSE_UMBRELLA=ON \
  -DFUSE_BUILD_CORE=ON \
  -DFUSE_BUILD_T3D=OFF \
  -DFUSE_BUILD_T2D=OFF

cmake --build build-ios --target fuse_core
```

---

## Jobs / platform stubs (WP-03)

| Header | Purpose |
|--------|---------|
| `fuse/jobs/worker_count.hpp` | `computeWorkerCount()` — locked formula §3.1.1 |
| `fuse/jobs/job_scheduler.hpp` | Scheduler stub |
| `fuse/jobs/job_counter.hpp` | Counter stub |
| `fuse/jobs/parallel_for.hpp` | Serial fallback `parallel_for` |
| `fuse/platform/thread.hpp` | `getCoreCount`, `renderThread`, … |
| `fuse/platform/power.hpp` | `getPowerState()` |

Single-thread fallback: configure with `-DFUSE_JOBS_SINGLE_THREAD=ON` or define `FUSE_JOBS_SINGLE_THREAD=1` at compile time. When set, `computeWorkerCount()` returns `0`.

---

## WP-02 prep — prefix rename script

Evidence-driven planner (dry-run only):

```bash
python3 Tools/FUSE/prefix_legacy_symbols.py --list-con
python3 Tools/FUSE/prefix_legacy_symbols.py --plan --dimension t3d --output /tmp/t3d-prefix-plan.json
python3 Tools/FUSE/prefix_legacy_symbols.py --plan --dimension t2d --output /tmp/t2d-prefix-plan.json
```

U2 (`fuse_t3d_legacy` / `fuse_t2d_legacy`) will consume the JSON plans to apply `fuse_t3d_` / `fuse_t2d_` prefixes inside quarantined static libraries — see [symbol-collision-report.md](./symbol-collision-report.md).

---

## Directory map (U1)

```
CMakeLists.txt              # FUSE umbrella entry
cmake/
  LegacyTorque3D.cmake      # Preserved T3D root logic
  FusePlatforms.cmake       # FUSE_PLATFORM_* detection
  FuseTorque2D.cmake        # ExternalProject wrapper for T2D
Source/FUSE/Core/           # fuse_core library + tests
Tools/FUSE/prefix_legacy_symbols.py
```
