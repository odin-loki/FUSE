# Building FUSE

Root `CMakeLists.txt` is the **umbrella**. Default configure is `FUSE_UMBRELLA=ON` unless you force a legacy-only application name.

## Prerequisites

```bash
git submodule update --init --recursive
```

| Need | Minimum |
|------|---------|
| CMake | 3.21 |
| Language | C++17 compiler today; C++23 is the product target |
| Generators | Ninja, Visual Studio, Xcode, Unix Makefiles |

## Common CMake options

| Option | Default | Target |
|--------|---------|--------|
| `FUSE_UMBRELLA` | ON | Product graph (`fuse_core` and modules) |
| `FUSE_BUILD_CORE` | ON | `fuse_core` |
| `FUSE_BUILD_CORE_TESTS` | ON | `fuse_core_*` CTest binaries |
| `FUSE_BUILD_SMOKE` | ON | `fuse_runtime_smoke` |
| `FUSE_BUILD_HYBRID_DEMO` | ON | `demo_hybrid_hud` |
| `FUSE_BUILD_PROJECT` | ON | `fuse_project` + `project.json` loader |
| `FUSE_BUILD_PARITY_DEMOS` | ON | Headless demos under `Source/FUSE/Apps` |
| `FUSE_BUILD_TOOLS` | ON | `fuse_import` (dry-run) and related CLI |
| `FUSE_BUILD_AUDIO` | ON | `fuse_audio` |
| `FUSE_BUILD_SCRIPT` | ON | `fuse_script` |
| `FUSE_BUILD_MODULES` | ON | `fuse_ai`, `fuse_fx`, `fuse_cinematics`, `fuse_mechanics`, `fuse_adventure` |
| `FUSE_BUILD_EDITOR_API` | ON | Qt-free `fuse_editor_api` |
| `FUSE_BUILD_EDITOR` | OFF | Qt 6 `fuse_editor` (desktop only) |
| `FUSE_BUILD_VULKAN` | ON | `fuse_rhi` (stub if the loader is missing) |
| `FUSE_BUILD_CUDA` | OFF | CUDA job lane / `fuse_compute` |
| `FUSE_BUILD_LEGACY` | ON | Quarantine static libs used by smoke |
| `FUSE_BUILD_T3D` | ON | Optional legacy 3D application target |
| `FUSE_BUILD_T2D` | ON | Optional legacy 2D application target |
| `FUSE_JOBS_SINGLE_THREAD` | OFF | Inline jobs on the caller thread |
| `FUSE_SMOKE_ENABLE_ASAN` | OFF | AddressSanitizer on smoke |
| `FUSE_CORE_ENABLE_TSAN` | OFF | ThreadSanitizer on `fuse_core` tests |

For product development, turn the optional legacy application targets **off**:

```bash
-DFUSE_BUILD_T3D=OFF -DFUSE_BUILD_T2D=OFF
```

## Desktop (Linux example)

```bash
cmake -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DFUSE_UMBRELLA=ON \
  -DFUSE_BUILD_CORE=ON \
  -DFUSE_BUILD_CORE_TESTS=ON \
  -DFUSE_BUILD_T3D=OFF \
  -DFUSE_BUILD_T2D=OFF

cmake --build build
ctest --test-dir build --output-on-failure
```

Smoke and hybrid demo:

```bash
./build/Source/FUSE/Apps/RuntimeSmoke/fuse_runtime_smoke
./build/Source/FUSE/Apps/HybridHud/demo_hybrid_hud
```

## Windows

Visual Studio generator:

```powershell
cmake -B build -DFUSE_UMBRELLA=ON `
  -DFUSE_BUILD_CORE=ON `
  -DFUSE_BUILD_CORE_TESTS=ON `
  -DFUSE_BUILD_T3D=OFF `
  -DFUSE_BUILD_T2D=OFF

cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Ninja + MSVC from a developer prompt is the same as the Linux command set.

## Editor (Qt 6)

Desktop only. Ignored on mobile toolchains.

```bash
cmake -B build-editor -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DFUSE_UMBRELLA=ON \
  -DFUSE_BUILD_EDITOR_API=ON \
  -DFUSE_BUILD_EDITOR=ON \
  -DFUSE_BUILD_T3D=OFF \
  -DFUSE_BUILD_T2D=OFF

cmake --build build-editor --target fuse_editor
```

Point CMake at Qt with `CMAKE_PREFIX_PATH` or `Qt6_DIR` if `find_package(Qt6)` fails. Details: [editor.md](editor.md).

## Sanitizers

AddressSanitizer on smoke:

```bash
cmake -B build-asan -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DFUSE_UMBRELLA=ON \
  -DFUSE_BUILD_SMOKE=ON \
  -DFUSE_SMOKE_ENABLE_ASAN=ON \
  -DFUSE_BUILD_T3D=OFF \
  -DFUSE_BUILD_T2D=OFF
```

ThreadSanitizer on core tests (Linux nightly CI uses this):

```bash
cmake -B build-tsan -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DFUSE_UMBRELLA=ON \
  -DFUSE_BUILD_CORE=ON \
  -DFUSE_BUILD_CORE_TESTS=ON \
  -DFUSE_CORE_ENABLE_TSAN=ON \
  -DFUSE_BUILD_T3D=OFF \
  -DFUSE_BUILD_T2D=OFF

cmake --build build-tsan --target fuse_core_tests
ctest --test-dir build-tsan -R '^fuse_core_' --output-on-failure
```

## Single-thread jobs

Use for replay, golden tests, and bisect. `computeWorkerCount()` returns `0`; `submit()` and `parallel_for` run on the caller.

```bash
cmake -B build-st -G Ninja \
  -DFUSE_UMBRELLA=ON \
  -DFUSE_BUILD_CORE=ON \
  -DFUSE_BUILD_CORE_TESTS=ON \
  -DFUSE_JOBS_SINGLE_THREAD=ON \
  -DFUSE_BUILD_T3D=OFF \
  -DFUSE_BUILD_T2D=OFF
```

Behaviour must match the parallel scheduler modulo timing.

## Platforms

`cmake/FusePlatforms.cmake` sets `FUSE_PLATFORM_*` from the toolchain:

| Cache option | When |
|--------------|------|
| `FUSE_PLATFORM_WINDOWS` | Win32 desktop |
| `FUSE_PLATFORM_LINUX` | Linux desktop |
| `FUSE_PLATFORM_MACOS` | macOS desktop |
| `FUSE_PLATFORM_IOS` | iOS / simulator |
| `FUSE_PLATFORM_ANDROID` | Android NDK |

These drive `FUSE_PLATFORM_MOBILE`, fiber stack defaults, and worker profiles. Gameplay code should not `#ifdef` on them; platform differences live in `fuse::platform`.

### Android (`fuse_core`)

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

### iOS simulator (`fuse_core`)

Requires macOS and Xcode.

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

Emscripten is a separate future profile (`FUSE_PLATFORM_EMSCRIPTEN`). It does not gate desktop or mobile native work. When it lands, jobs default to `FUSE_JOBS_SINGLE_THREAD`.

## Directory map

```
CMakeLists.txt                 Umbrella entry
cmake/FusePlatforms.cmake      FUSE_PLATFORM_* detection
Source/FUSE/                   Product libraries and apps
Tools/FUSE/                    CLI utilities
```
