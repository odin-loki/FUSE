# FUSE patches to vendored DXVK

This tree is upstream DXVK v3.1.1 (`b1a1c99ab52b687cf950d62c88bc2fa316b41663`), pinned in `VERSION`. DXVK is under the zlib licence (`LICENSE`), whose clause 2 requires altered source versions to be plainly marked. FUSE marks every edit in two ways:

1. **In the source.** Each edit is wrapped in `// FUSE-DXVK begin: <reason>` / `// FUSE-DXVK end`.
2. **In this list.** Each edit has an entry below, giving the files, its purpose and the Relight work package ([`docs/plans/FUSE_REMIX_PORT_PLAN.md`](../../../docs/plans/FUSE_REMIX_PORT_PLAN.md) §0.4, §2.4).

The plan's patch budget is at most 30 marked blocks (§2.4). An edited file gets its sha256 re-pinned in `VERSION` in the same change.

## Patch list

Each patch has an ID (`RL-x.y-NN`), which is the first token after `FUSE-DXVK begin:`. `VERSION` pins every patched file twice:
- `sha256:` is the file as vendored;
- `upstream_sha256:` is the file with its marked blocks removed, which must equal the upstream file.

`rl_dxvk_pins` (`Tests/relight/smoke/check_pins.cmake`) checks both, so an edit outside the markers fails the gate.

| ID | Files | Purpose | Work package | Upstream status |
|---|---|---|---|---|
| RL-0.2-01 | `src/dxvk/dxvk_device_info.cpp` (`DxvkDeviceCapabilities::initSupportedExtensions`, 1 block) | Accept `VK_EXT_load_store_op_none` as DXVK's required `VK_KHR_load_store_op_none`. The two are aliases with identical enums and semantics. When the device exposes only the EXT name, the KHR entry is pointed at it, so it is detected and enabled at device creation. Why it is needed: Wine 9.0's winevulkan (Ubuntu 24.04, the CI image) hides the KHR name even though Lavapipe has it, so DXVK 3.1.1 rejected every device and `rl_dxvk_smoke` could not run. | RL-0.2 | FUSE-only, not submitted. Upstream DXVK targets newer Wine and requires the KHR name on purpose (it is core in Vulkan 1.4). Drop the patch when CI's Wine exposes the KHR name. |

## Build notes (no source edits)

- **Build system.** Meson is not used. `Source/FUSE/Relight/cmake/relight_dxvk.cmake` rebuilds the D3D9 and D3D8 DLLs from the vendored `meson.build` source lists, which it reads at configure time. It also generates `version.h`, `buildenv.h`, the GLSL → SPIR-V headers and libdisplay-info's `pnp-id-table.c`. The version string is `v3.1.1-fuse`.
- **DirectX headers.** PE builds use the MinGW-w64 toolchain's DirectX headers. DXVK's `include/native/directx` is LGPL-2.1+ and is not vendored.
- **Linux native build (dxvk-native).** Not ported. It would need SDL2, SDL3 or GLFW for WSI, plus the LGPL DirectX headers fetched into `build/` at configure time. Relight runs its PE DLLs under Wine/Proton on Linux instead.
- **Not built:** `d3d10`, `d3d11` and `dxgi`. None of their sources are vendored. The exception is five interface headers (`src/d3d11/d3d11_{interfaces,include}.h`, `src/dxgi/dxgi_{interfaces,format,include}.h`), which are kept because `src/util/com/com_guid.cpp` includes them.
