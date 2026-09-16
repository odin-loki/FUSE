# Getting started

Build FUSE from source, run tests, and launch a hybrid demo.

## Prerequisites

- Git with submodule support
- CMake 3.21 or newer
- A C++17 compiler (GCC 11+, Clang 14+, MSVC 2019+). Product code is moving to ISO C++23.
- Ninja recommended (optional on Windows if you use Visual Studio generators)

Optional:

- Qt 6 Widgets — desktop editor (`fuse_editor`)
- Vulkan SDK — real RHI path (`fuse_rhi`); otherwise a stub backend is used
- CUDA toolkit — compute / interop (`FUSE_BUILD_CUDA=ON`)
- Lua — script VM backend (`FUSE_SCRIPT_ENABLE_LUA`)

## Clone

```bash
git clone --recurse-submodules https://github.com/odin-loki/FUSE.git
cd FUSE
```

If you already cloned without submodules:

```bash
git submodule update --init --recursive
```

## Configure and build (core + tests)

This is the fast path: `fuse_core`, worlds, modules, smoke, and demos — no legacy application targets.

### Linux / macOS

```bash
cmake -B build -G Ninja \
  -DFUSE_UMBRELLA=ON \
  -DFUSE_BUILD_CORE=ON \
  -DFUSE_BUILD_CORE_TESTS=ON \
  -DFUSE_BUILD_T3D=OFF \
  -DFUSE_BUILD_T2D=OFF

cmake --build build
ctest --test-dir build --output-on-failure
```

### Windows (PowerShell)

```powershell
cmake -B build -DFUSE_UMBRELLA=ON `
  -DFUSE_BUILD_CORE=ON `
  -DFUSE_BUILD_CORE_TESTS=ON `
  -DFUSE_BUILD_T3D=OFF `
  -DFUSE_BUILD_T2D=OFF

cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

## Run a demo

After a successful umbrella build:

```bash
# Hybrid 3D clear + spinning 2D sprite (software placeholder renderer)
./build/Source/FUSE/Apps/HybridHud/demo_hybrid_hud

# Empty 3D project
./build/Source/FUSE/Apps/Demo3DEmpty/demo_3d_empty Samples/unification/demo_3d_empty
```

On Windows, binaries live under the chosen config folder (for example `build\Source\FUSE\Apps\HybridHud\Release\`).

More demos: [samples.md](samples.md). Full CMake matrix: [building.md](building.md).

## What you just built

| Piece | Location |
|-------|----------|
| Core library | `Source/FUSE/Core` → `fuse_core` |
| 2D / 3D worlds | `Source/FUSE/World2D`, `World3D` |
| Hybrid compositor | `Source/FUSE/Hybrid` |
| Feature modules | `Source/FUSE/Modules` |
| Headless apps | `Source/FUSE/Apps` |

The editor is off by default. Enable it with `-DFUSE_BUILD_EDITOR=ON` when Qt 6 is installed — see [editor.md](editor.md).
