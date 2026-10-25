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

The default **`main`** gate is [`.github/workflows/fuse-umbrella-linux.yml`](../.github/workflows/fuse-umbrella-linux.yml) (see Linux packages above).

| Workflow | File | Actions status | Role |
|----------|------|----------------|------|
| FUSE CUDA compile-only (Linux) | [fuse-cuda-compile.yml](../.github/workflows/fuse-cuda-compile.yml) | **Active** — push/PR to `main` | Installs `nvidia-cuda-toolkit`; cubin/header + host-path CUDA gates (no NVIDIA GPU required). |
| FUSE fuse_core (iOS simulator) | [fuse-core-ios.yml](../.github/workflows/fuse-core-ios.yml) | **Active** — `workflow_dispatch` only; verified green at `204a5f8e6` | macOS + Xcode; `fuse_core` iphonesimulator compile only. **Actions → FUSE fuse_core (iOS simulator) → Run workflow**. |
| FUSE fuse_core (TSan nightly) | [fuse-tsan-nightly.yml](../.github/workflows/fuse-tsan-nightly.yml) | **Active** — `workflow_dispatch` + optional `03:00` UTC schedule (since `75db736a6`) | `FUSE_CORE_ENABLE_TSAN=ON`; builds `Source/FUSE/Core/tests/all` + asset runtime tests; `ctest -L tsan` (not every `fuse_core_*` name — Tracy nm probes stay on the umbrella job). **Actions → FUSE fuse_core (TSan nightly) → Run workflow** for manual runs. |

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

**Use WSL2 for day-to-day builds and gates on Windows.** Do **not** point CMake at the [Strawberry Perl](https://strawberryperl.com/) toolchain (`C:\Strawberry\c\bin\cmake.exe`, `gcc.exe`, and so on) for FUSE work: that path is unsupported here, often leaves `FUSE_BUILD_VULKAN=OFF`, breaks or skips renderer Lavapipe tests (`fuse_rp_frame_*`, `fuse_scene_renderer_vk_*`), and Slang/Vulkan configure differently from CI.

| Goal | Where to build |
|------|----------------|
| Full umbrella gates, Lavapipe, parallel `ctest -j` | **WSL2** (Ubuntu 24.04) — same as [fuse-umbrella-linux.yml](../.github/workflows/fuse-umbrella-linux.yml) |
| Native Windows PE / MSVC smoke | Optional: Visual Studio or `fuse-msvc-*` presets (see below) |
| Cross PE + Wine | Linux host with `fuse-mingw-*` presets (CI), not Strawberry on the desktop |

### WSL2 (recommended on Windows)

1. Install [WSL2](https://learn.microsoft.com/en-us/windows/wsl/install) with **Ubuntu 24.04**.
2. In the WSL shell, install dependencies: run **`bash scripts/wsl-bootstrap.sh`** (needs `sudo` once), then **re-configure** `build-wsl` so CMake picks up `glslangValidator` and embedded shaders (`cmake -B build-wsl …` as below). Or install the same packages as [Linux system packages](#linux-system-packages-ubuntu-2404) manually.
3. Clone or use the repo from the Linux filesystem when you can (`~/FUSE`). A tree on `/mnt/c/...` (OneDrive/Desktop, **drvfs**) is fine for core/Vulkan work but I/O is slower; prefer `~/FUSE` for large `ctest -j` sweeps. Drvfs is case-insensitive, so `vendor/volk/VERSION` can shadow the libc++ `<version>` header (see `cmake/FuseVolk.cmake`) and break **Qt editor** configures — build with `FUSE_BUILD_EDITOR=ON` only under `~/FUSE` (or another Linux-native path).

```bash
# Inside WSL (Ubuntu 24.04)
sudo apt-get update
sudo apt-get install -y ninja-build cmake g++-13 \
  libvulkan-dev mesa-vulkan-drivers vulkan-tools \
  vulkan-validationlayers glslang-tools spirv-tools

cd ~/FUSE   # or: cd /mnt/c/Users/<you>/OneDrive/Desktop/FUSE

export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json

cmake -B build-wsl -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DFUSE_BUILD_CORE=ON \
  -DFUSE_BUILD_CORE_TESTS=ON \
  -DFUSE_BUILD_VULKAN=ON

cmake --build build-wsl
ctest --test-dir build-wsl -L gate -j"$(nproc)" --output-on-failure

# Frame integration only (after wsl-bootstrap.sh):
bash scripts/wsl-run-frame-gates.sh
# On drvfs (/mnt/c/...), run that script alone: overlapping `ctest -R fuse_rp_frame_vk -j`
# or other parallel jobs that execute `fuse_rp_frame` often fails with flock "Text file busy".

# Or one-shot configure + full gate sweep (editor off):
bash scripts/wsl-build-local.sh
```

Renderer frame integration (T0 Lavapipe) lives under the `gate;renderer;frame` labels, for example `fuse_rp_frame_vk_full_t0` and `fuse_scene_renderer_vk_golden_t0`. Those Vulkan tests **skip (exit 77)** if `VK_LAYER_KHRONOS_validation` is missing — install `vulkan-validationlayers` (included in `scripts/wsl-bootstrap.sh`) and confirm `/usr/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json` exists. A ctest run where frame VK tests only skip is **not** the same as passing the frame gates (`scripts/wsl-run-frame-gates.sh`).

Windowed core/editor tests run through `build-wsl/fuse_run_with_display.sh` (generated at configure). It may use `xvfb-run` when `DISPLAY` is unset or is WSLg’s `:0`; the wrapper **unsets `WAYLAND_DISPLAY`** and defaults `QT_QPA_PLATFORM=xcb` so Qt does not select Wayland under Xvfb (teardown aborts). Leave **`FUSE_FORCE_XVFB` unset** unless you need to force Xvfb for every wrapped test.

`glslangValidator` must be on `PATH` **when you configure** CMake: it embeds the GPU radix-sort shaders (WP-9.2 splats, `hybrid.sprite_layer`, and several other packages). If you configured before installing `glslang-tools`, re-run `cmake -B build-wsl …` after `wsl-bootstrap.sh` and rebuild (`fuse_gsplat`, `fuse_hybrid`, `fuse_compute`).

Presets work inside WSL if CMake 3.25+ is installed:

```bash
cmake --preset fuse-debug
cmake --build --preset fuse-debug
ctest --test-dir build/fuse-debug --output-on-failure
```

Open the repo in Cursor/VS Code with the [WSL extension](https://code.visualstudio.com/docs/remote/wsl) so terminals and builds run in Linux, not PowerShell + Strawberry.

### Native MSVC (optional)

For Windows-specific debugging without Lavapipe, use a **Visual Studio** or **Ninja + MSVC** developer prompt (Vulkan SDK on the host). This matches [fuse-windows-native.yml](../.github/workflows/fuse-windows-native.yml), not the full Linux umbrella gate.

```powershell
cmake -B build `
  -DFUSE_BUILD_CORE=ON `
  -DFUSE_BUILD_CORE_TESTS=ON `
  -DFUSE_BUILD_VULKAN=ON

cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

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
  -DCMAKE_CUDA_ARCHITECTURES=86 \
  -DFUSE_BUILD_CORE=ON -DFUSE_BUILD_CORE_TESTS=ON \
  -DFUSE_BUILD_CUDA=ON \
  -DFUSE_BUILD_VULKAN=ON
cmake --build build-cuda
ctest --test-dir build-cuda -R '^(fuse_cuda_compile_gate|fuse_cuda_shared_headers_gate|fuse_compute_kernel_cuda_gate)$' --output-on-failure
```

On Windows use MSVC + `vcvars64` and set `CMAKE_CUDA_ARCHITECTURES=86` for RTX 30xx.

**Device gates (RTX 3090 / sm_86):** with the toolkit and an NVIDIA GPU, run `bash scripts/wsl-run-cuda-device-gates.sh` (optional build dir, default `build-cuda`). CUDA–Vulkan interop tests need the **system NVIDIA Vulkan ICD** (do not set `VK_ICD_FILENAMES` to Lavapipe). Umbrella Linux configures `FUSE_BUILD_CUDA=ON` without a toolkit, so device tests register and **SKIP (77)**.
