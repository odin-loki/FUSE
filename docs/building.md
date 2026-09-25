# Building FUSE

Root `CMakeLists.txt` configures the FUSE product graph: `fuse_core`, modules, Vulkan/CUDA, and native world/scene APIs under `Source/FUSE/`. Vendored deps live in `third_party/vendor/`. See [repository-layout.md](repository-layout.md).

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
| `FUSE_JOBS_SINGLE_THREAD` | OFF | Inline jobs on the caller thread |
| `FUSE_SANITIZE` | "" | `address,undefined`: ASan+UBSan on FUSE targets (`fuse-asan` preset) |
| `FUSE_SMOKE_ENABLE_ASAN` | OFF | AddressSanitizer on smoke (legacy; superseded by `FUSE_SANITIZE`) |
| `FUSE_CORE_ENABLE_TSAN` | OFF | ThreadSanitizer on `fuse_core` tests |

The full Torque3D/Torque2D **application** targets (`Engine/` exe, upstream `third_party/Torque2D`) are not part of this graph. World conversion and module bridges use native `fuse::world2d` / `fuse::scene` APIs.

## Desktop (Linux example)

```bash
cmake -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DFUSE_BUILD_CORE=ON \
  -DFUSE_BUILD_CORE_TESTS=ON

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
cmake -B build `
  -DFUSE_BUILD_CORE=ON `
  -DFUSE_BUILD_CORE_TESTS=ON

cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Ninja + MSVC from a developer prompt is the same as the Linux command set.

### Windows x64 from Linux (MinGW-w64 cross + Wine)

See `CMakePresets.json` presets `fuse-mingw-*`.

## Presets

```bash
cmake --preset fuse-debug
cmake --build --preset fuse-debug
ctest --test-dir build/fuse-debug --output-on-failure
```

| Preset | Use |
|--------|-----|
| `fuse-debug` | Daily development |
| `fuse-release` | Optimized product build |
| `fuse-asan` | Sanitizer sweep |
| `fuse-shipping` | Game runtime without editor |
| `fuse-track-b-unlock` | Local production-present defaults (still locked in CI) |
| `fuse-editor-debug` | Qt editor shell |

## CUDA (optional)

```bash
cmake -B build-cuda -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DFUSE_BUILD_CUDA=ON \
  -DFUSE_BUILD_VULKAN=ON
```

On Windows use MSVC + `vcvars64` and set `CMAKE_CUDA_ARCHITECTURES=86` for RTX 30xx.
