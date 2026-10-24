# Building FUSE

Root `CMakeLists.txt` configures the FUSE product graph: `fuse_core`, modules, Vulkan/CUDA, and native world/scene APIs under `Source/FUSE/`. Vendored deps live in `vendor/`. See [repository-layout.md](repository-layout.md).

## Clone

```bash
git clone https://github.com/odin-loki/FUSE.git
cd FUSE
```

All third-party libraries are committed under `vendor/`; clone and configure — no extra dependency checkout.

## Prerequisites

| Need | Minimum |
|------|---------|
| CMake | 3.21 |
| Language | C++23 (`Source/FUSE/`) |
| Generators | Ninja, Visual Studio, Xcode, Unix Makefiles |

### Linux system packages (Ubuntu 24.04)

Vendored libraries live under `vendor/`; you only need a C++23 toolchain plus optional GPU/Qt packages for the CMake options you enable.

**Toolchain** (`FUSE_BUILD_CORE` only, no Vulkan tests):

```bash
sudo apt-get update
sudo apt-get install -y ninja-build cmake g++-13
```

**`FUSE_BUILD_CORE_TESTS` with Vulkan** (`FUSE_BUILD_VULKAN=ON`, shader SPIR-V regen, Lavapipe gates — matches [`.github/workflows/fuse-umbrella-linux.yml`](../.github/workflows/fuse-umbrella-linux.yml), the **full CI gate** on `main`, typically **~3 h** on GitHub Actions):

```bash
sudo apt-get install -y ninja-build cmake g++-13 \
  libvulkan-dev mesa-vulkan-drivers vulkan-tools \
  vulkan-validationlayers glslang-tools spirv-tools
```

Point the loader at Mesa Lavapipe for headless runs (CI sets this automatically):

```bash
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json
```

| Package | Role |
|---------|------|
| `libvulkan-dev` | Vulkan loader + headers (`find_package(Vulkan)`) |
| `mesa-vulkan-drivers` | Lavapipe software ICD |
| `vulkan-tools` | `vulkaninfo` and loader diagnostics |
| `vulkan-validationlayers` | Khronos validation layer (`fuse_vulkan_validation_gate`) |
| `glslang-tools` | `glslangValidator` (configure-time SPIR-V regen and cook gates) |
| `spirv-tools` | `spirv-val` (validates regenerated SPIR-V) |

**Qt 6 editor** (`FUSE_BUILD_EDITOR=ON`, optional): `qt6-base-dev` plus the Xvfb/xcb packages listed in the Qt editor CI job — see [editor.md](editor.md).

**CUDA** (`FUSE_BUILD_CUDA=ON`, optional): install the distro CUDA toolkit when you want real device code; without it, configure continues with CUDA stubs. The compile-only CUDA workflow installs `nvidia-cuda-toolkit` on `ubuntu-24.04`.

**Slang** (`FUSE_SLANG=AUTO`, default): no apt package; CMake may download a pinned `slangc` when enabled.

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

Legacy Torque3D/Torque2D application targets are not part of this graph. World conversion and module bridges use native `fuse::world2d` / `fuse::scene` APIs.

## Optional CI (GitHub Actions)

The default **`main`** gate is [`.github/workflows/fuse-umbrella-linux.yml`](../.github/workflows/fuse-umbrella-linux.yml) (see Linux packages above). Neither optional workflow below runs on push or PR.

| Workflow | File | Actions status | Role |
|----------|------|----------------|------|
| FUSE fuse_core (iOS simulator) | [fuse-core-ios.yml](../.github/workflows/fuse-core-ios.yml) | **Active** — `workflow_dispatch` only | macOS + Xcode; `fuse_core` iphonesimulator compile only. **Actions → FUSE fuse_core (iOS simulator) → Run workflow**. |
| FUSE fuse_core (TSan nightly) | [fuse-tsan-nightly.yml](../.github/workflows/fuse-tsan-nightly.yml) | **Active** — `workflow_dispatch` + optional `03:00` UTC schedule | `FUSE_CORE_ENABLE_TSAN=ON`; builds `Source/FUSE/Core/tests/all` + asset runtime tests; `ctest -L tsan` (not every `fuse_core_*` name — Tracy nm probes stay on the umbrella job). **Actions → FUSE fuse_core (TSan nightly) → Run workflow** for manual runs. |

Local equivalents: set `FUSE_CORE_ENABLE_TSAN=ON` for ThreadSanitizer (see table above), or follow the configure commands in the iOS workflow header / [unification/BUILD.md](unification/BUILD.md) for mobile stubs.

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

[`CMakePresets.json`](../CMakePresets.json) (CMake 3.25+) lists all configure presets. Short reference: [`cmake/README.md`](../cmake/README.md).

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
