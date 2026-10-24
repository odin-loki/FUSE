# Getting started

Build FUSE from source, run tests, and launch a hybrid demo.

## Prerequisites

- Git
- CMake 3.21 or newer
- A C++23 compiler (GCC 13+, Clang 16+, MSVC 2022 17.8+).
- Ninja recommended
- **Windows:** use **WSL2 (Ubuntu 24.04)** for builds and tests — do not use the Strawberry Perl CMake/GCC bundle. See [building.md — Windows (WSL)](building.md#wsl2-recommended-on-windows).

Optional:

- Qt 6 Widgets — desktop editor (`fuse_editor`)
- Vulkan SDK — real RHI path (`fuse_rhi`); otherwise a stub backend is used
- CUDA toolkit — compute / interop (`FUSE_BUILD_CUDA=ON`)
- Lua — script VM backend (`FUSE_SCRIPT_ENABLE_LUA`)

## Clone

```bash
git clone https://github.com/odin-loki/FUSE.git
cd FUSE
```

Vendored dependencies are committed under `vendor/`; no extra dependency checkout is required.

## Configure and build (core + tests)

This is the fast path: `fuse_core`, worlds, modules, smoke, and demos — no legacy application targets.

### Linux / macOS

```bash
cmake -B build -G Ninja \
  -DFUSE_BUILD_CORE=ON \
  -DFUSE_BUILD_CORE_TESTS=ON

cmake --build build
ctest --test-dir build --output-on-failure
```

### Windows (WSL2 — recommended)

Open an **Ubuntu** shell in WSL, not PowerShell with Strawberry Perl:

```bash
sudo apt-get update
sudo apt-get install -y ninja-build cmake g++-13

cd ~/FUSE   # clone here, or cd to your checkout under /mnt/c/...

cmake -B build-wsl -G Ninja \
  -DFUSE_BUILD_CORE=ON \
  -DFUSE_BUILD_CORE_TESTS=ON

cmake --build build-wsl
ctest --test-dir build-wsl --output-on-failure
```

Full Vulkan / Lavapipe packages and `VK_ICD_FILENAMES`: [building.md](building.md#wsl2-recommended-on-windows).

### Windows (native MSVC, optional)

Only if you need a Windows-hosted binary without the Linux gate layout:

```powershell
cmake -B build -DFUSE_BUILD_CORE=ON -DFUSE_BUILD_CORE_TESTS=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

## Run a demo

After a successful build:

```bash
# Hybrid 3D clear + spinning 2D sprite (software placeholder renderer)
./build/Source/FUSE/Apps/HybridHud/demo_hybrid_hud

# Empty 3D project
./build/Source/FUSE/Apps/Demo3DEmpty/demo_3d_empty Samples/unification/demo_3d_empty
```

On Windows, binaries live under the chosen config folder (for example `build\Source\FUSE\Apps\HybridHud\Release\`).

More demos: [samples.md](samples.md) and the [player guide](player-guide.md). API wiring: [programmer guide](programmer-guide.md). Full CMake matrix: [building.md](building.md).

## What you just built

| Piece | Location |
|-------|----------|
| Core library | `Source/FUSE/Core` → `fuse_core` |
| 2D / 3D worlds | `Source/FUSE/World2D`, `World3D` |
| Hybrid compositor | `Source/FUSE/Hybrid` |
| Feature modules | `Source/FUSE/Modules` |
| Headless apps | `Source/FUSE/Apps` |

The editor is off by default. Enable it with `-DFUSE_BUILD_EDITOR=ON` when Qt 6 is installed — see [editor.md](editor.md).
