# CMake helpers

FUSE’s configure entry point is the root [`CMakeLists.txt`](../CMakeLists.txt). This directory holds platform helpers, sanitizers, vendor wiring (`FuseVendor.cmake`), and cross-compilation toolchains.

## Presets

[`CMakePresets.json`](../CMakePresets.json) is the supported way to pick generators, binary dirs, and common cache variables. CMake **3.25+** is required for presets (3.21+ for manual `-B` configures).

Typical workflow:

```bash
cmake --list-presets
cmake --preset fuse-debug
cmake --build --preset fuse-debug
ctest --test-dir build/fuse-debug --output-on-failure
```

| Preset family | Purpose |
|---------------|---------|
| `fuse-debug`, `fuse-release`, `fuse-profile` | Daily product builds |
| `fuse-asan` | Address + undefined sanitizers on FUSE targets |
| `fuse-shipping` | Runtime without editor |
| `fuse-editor-debug` | Qt 6 `fuse_editor` |
| `fuse-track-b-unlock` | Local production-present defaults |
| `fuse-werror-*` | `-Werror` / `/WX` on FUSE targets |
| `fuse-mingw-*` | Windows x64 cross from Linux (Wine ctest) |
| `fuse-msvc-*`, `fuse-clangcl-*` | Native Windows toolchains |
| `fuse-mingw-relight-*` | Optional Relight / DXVK stack (MinGW) |

Full option matrix and platform notes: [`docs/building.md`](../docs/building.md).

**Windows hosts:** run CMake/Ninja/ctest inside **WSL2**, not the Strawberry Perl toolchain. Use `build-wsl/` (or preset dirs under `build/`) from a Linux shell; see [building.md — WSL2](../docs/building.md#wsl2-recommended-on-windows).
