# FUSE patches to vendored DXVK

This tree is upstream DXVK v3.1.1 (`b1a1c99ab52b687cf950d62c88bc2fa316b41663`), pinned in `VERSION`. DXVK is under the zlib licence (`LICENSE`), whose clause 2 requires altered source versions to be plainly marked. FUSE marks every edit in two ways:

1. **In the source.** Each edit is wrapped in `// FUSE-DXVK begin: <reason>` / `// FUSE-DXVK end`.
2. **In this list.** Each edit has an entry below, giving the files, its purpose and the Relight work package ([`docs/plans/FUSE_REMIX_PORT_PLAN.md`](../../../docs/plans/FUSE_REMIX_PORT_PLAN.md) §0.4, §2.4).

The plan's patch budget is at most 30 marked blocks (§2.4). An edited file gets its sha256 re-pinned in `VERSION` in the same change.

## Patch list

Each patch has an ID (`RL-x.y-NN`), which is the first token after `FUSE-DXVK begin:`. Block count: 26 of 30 (RL-0.2-01, RL-1.1-01 to -25; one block each). `VERSION` pins every patched file twice:
- `sha256:` is the file as vendored;
- `upstream_sha256:` is the file with its marked blocks removed, which must equal the upstream file.

`rl_dxvk_pins` (`Tests/relight/smoke/check_pins.cmake`) checks both, so an edit outside the markers fails the gate.

| ID | Files | Purpose | Work package | Upstream status |
|---|---|---|---|---|
| RL-0.2-01 | `src/dxvk/dxvk_device_info.cpp` (`DxvkDeviceCapabilities::initSupportedExtensions`, 1 block) | Accept `VK_EXT_load_store_op_none` as DXVK's required `VK_KHR_load_store_op_none`. The two are aliases with identical enums and semantics. When the device exposes only the EXT name, the KHR entry is pointed at it, so it is detected and enabled at device creation. Why it is needed: Wine 9.0's winevulkan (Ubuntu 24.04, the CI image) hides the KHR name even though Lavapipe has it, so DXVK 3.1.1 rejected every device and `rl_dxvk_smoke` could not run. | RL-0.2 | FUSE-only, not submitted. Upstream DXVK targets newer Wine and requires the KHR name on purpose (it is core in Vulkan 1.4). Drop the patch when CI's Wine exposes the KHR name. |
| RL-1.1-01 | `src/dxvk/dxvk_instance.cpp` (`DxvkInstance::DxvkInstance(const DxvkInstanceImportInfo&, DxvkInstanceFlags)`, 1 block) | Import the FUSE-created `VkInstance` (plan §2.2 AD-2). When the constructor gets no instance (the `DxvkInstance(flags)` path used by d3d9, whose argument is a non-const temporary), it asks FUSE Relight (`dxvk::fuseRelightImportInstance`, `Source/FUSE/Relight/tap/src/vk_bootstrap.cpp`) to fill in the loader, instance and enabled extensions; upstream's own import path then runs unchanged. FUSE declines with `FUSE_RELIGHT=0`, `relight.device.import = false` or without a loader, and DXVK creates its instance as upstream. | RL-1.1 | FUSE-only. |
| RL-1.1-02 | `src/dxvk/dxvk_adapter.cpp` (`DxvkAdapter::createDevice()`, 1 block) | Import the FUSE-created `VkDevice` through upstream's `DxvkAdapter::importDevice`, with a `queueCallback` that takes FUSE's queue (submission) lock. Only when the adapter's instance is FUSE's; otherwise upstream device creation. | RL-1.1 | FUSE-only. |
| RL-1.1-03 | `src/d3d9/d3d9_device.h` (includes, 1 block) | Include the tap entry points (`Source/FUSE/Relight/tap/dxvk/fuse_tap_dxvk.h`, declarations only). | RL-1.1 | FUSE-only. |
| RL-1.1-04 | `src/d3d9/d3d9_device.h` (`D3D9DeviceEx`, 1 block) | Per-device tap dispatcher pointer `m_fuseTap` (null when the tap is off) and friend access for the dispatcher (`FuseTap`, `FuseTapContext`). | RL-1.1 | FUSE-only. |
| RL-1.1-05 | `src/d3d9/d3d9_device.cpp` (`~D3D9DeviceEx`) | `onDeviceDestroy`; frees the dispatcher. | RL-1.1 | FUSE-only. |
| RL-1.1-06 | `src/d3d9/d3d9_device.cpp` (`ResetSwapChain`, end) | Attaches the tap on the first successful reset (`InitialReset`) according to `relight.tap.mode` (`onDeviceCreate`); `onDeviceReset` afterwards. The dispatcher builds the tap with `createTapForDevice`, which also covers the `capture` mode (live in-process capture, see below). | RL-1.1 | FUSE-only. |
| RL-1.1-07 | `src/d3d9/d3d9_device.cpp` (`UpdateSurface`, end) | `onTextureCopy` (UpdateSurface), with the copied extent (the source rect, or the whole source level) and the destination point (tap interface 2), computed by the dispatcher from the hook's existing arguments. | RL-1.1 | FUSE-only. |
| RL-1.1-08 | `src/d3d9/d3d9_device.cpp` (`UpdateTexture`, end) | `onTextureCopy` (UpdateTexture). | RL-1.1 | FUSE-only. |
| RL-1.1-09 | `src/d3d9/d3d9_device.cpp` (`SetRenderTarget`) | With the tap on: runs `SetRenderTargetInternal` itself and reports `onSetRenderTarget` on success. | RL-1.1 | FUSE-only. |
| RL-1.1-10 | `src/d3d9/d3d9_device.cpp` (`Clear`, after validation) | `onClear`. | RL-1.1 | FUSE-only. |
| RL-1.1-11 | `src/d3d9/d3d9_device.cpp` (`DrawPrimitive`) | `onDraw` with the call and a view of `Direct3DState9`; decision `Ignore` returns `D3D_OK` without drawing (the §2.4 skip path). | RL-1.1 | FUSE-only. |
| RL-1.1-12 | `src/d3d9/d3d9_device.cpp` (`DrawIndexedPrimitive`) | As RL-1.1-11. | RL-1.1 | FUSE-only. |
| RL-1.1-13 | `src/d3d9/d3d9_device.cpp` (`DrawPrimitiveUP`) | As RL-1.1-11, with the application's vertex pointer. | RL-1.1 | FUSE-only. |
| RL-1.1-14 | `src/d3d9/d3d9_device.cpp` (`DrawIndexedPrimitiveUP`) | As RL-1.1-11, with the application's vertex and index pointers. | RL-1.1 | FUSE-only. |
| RL-1.1-15 | `src/d3d9/d3d9_device.cpp` (`LockImage`, end) | Remembers the lock (pointer, pitches, box, flags) for the upload event; `onTextureWriteLock` for write locks. | RL-1.1 | FUSE-only. |
| RL-1.1-16 | `src/d3d9/d3d9_device.cpp` (`UnlockImage`) | `onTextureUpload` with the bytes the application wrote. | RL-1.1 | FUSE-only. |
| RL-1.1-17 | `src/d3d9/d3d9_device.cpp` (`LockBuffer`, start) | Remembers the write range and the application's lock flags (before DXVK adjusts them). | RL-1.1 | FUSE-only. |
| RL-1.1-18 | `src/d3d9/d3d9_device.cpp` (`UnlockBuffer`) | `onBufferWrite` for each remembered range once the last lock is released. | RL-1.1 | FUSE-only. |
| RL-1.1-19 | `src/d3d9/d3d9_common_texture.cpp` (`D3D9CommonTexture` constructor, end) | `onTextureCreate`. | RL-1.1 | FUSE-only. |
| RL-1.1-20 | `src/d3d9/d3d9_common_texture.cpp` (`~D3D9CommonTexture`) | `onImageDestroy` (bindless release, plan §2.3). | RL-1.1 | FUSE-only. |
| RL-1.1-21 | `src/d3d9/d3d9_common_buffer.cpp` (`D3D9CommonBuffer` constructor, end) | `onBufferCreate`. There is no destructor hook (patch budget): a buffer created at a known address reports `onBufferDestroy` for the old id first. | RL-1.1 | FUSE-only. |
| RL-1.1-22 | `src/d3d9/d3d9_swapchain.cpp` (`D3D9SwapChainEx::Present`) | `onInjectPoint` (Present until RL-1.2's classifier finds the first UI draw) and `onPresent`; frame boundary. | RL-1.1 | FUSE-only. |
| RL-1.1-23 | `src/d3d9/d3d9_query.cpp` (`D3D9Query::Issue`) | `onQueryBegin` / `onQueryEnd`. | RL-1.1 | FUSE-only. |
| RL-1.1-24 | `src/d3d9/d3d9_device.cpp` (`SetLight`, end) | Lights changed (`FuseTap::LightsChanged`) when the light set is enabled: advances `DrawState::lightsVersion` (tap interface 3), where dxvk-remix sets `D3D9RtxFlag::DirtyLights`, so the translation re-sends the game lights exactly when upstream does rather than on every frame. | RL-1.5 follow-up (RL-1.1 tap) | FUSE-only (dxvk-remix has the equivalent `m_rtx.SetDirty(DirtyLights)` edit). |
| RL-1.1-25 | `src/d3d9/d3d9_device.cpp` (`LightEnable`, after the enable bit changes) | As RL-1.1-24, when `LightEnable` flips a light's enable bit (the early return for an unchanged bit comes first, as upstream). | RL-1.5 follow-up (RL-1.1 tap) | FUSE-only (as RL-1.1-24). |

## Capture tap mode (no source edits)

`relight.tap.mode = capture` (RL-1.1, `Source/FUSE/Relight/tap/capture`) runs the RL-1.2 classifier, RL-1.3 geometry capture, RL-1.4 texture tracking and the RL-1.5 fixed-function translation (TranslateTap: material, fog, transforms / clip plane, game lights, camera) inside d3d9.dll on the events of the hooks above, and writes a per-frame capture record. It adds no marked block: the tap is chosen once, at the RL-1.1-06 attach, so a device whose mode is not `capture` runs no capture code, and with the tap off every hook stays the one null-pointer test. The UpdateSurface extent comes from the arguments RL-1.1-07 already passes. `fuse_relight_tap_capture` is linked into `relight_d3d9` link-only, like `fuse_relight_tap`. The capture mode itself adds no block; the translation's change tracking below adds two (RL-1.1-24/25).

## Draw-state change tracking (tap interface 3)

`DrawState` carries two change counters and the render-target alpha-swizzle mask, so the RL-1.5 translation follows processRenderState's `DirtyLights` / `DirtyClipPlanes` flags and `setLegacyMaterialState`'s `m_alphaSwizzleRTs` instead of re-deriving them every frame:

- `lightsVersion`: RL-1.1-24 / RL-1.1-25 above, plus device creation and `Reset` (the RL-1.1-06 hook; upstream's `ResetState` dirties the lights). Two marked blocks; DXVK tracks nothing light-specific that the draw hooks could read.
- `clipPlanesVersion`: no source edit. DXVK sets its own `D3D9DeviceDirtyFlag::ClipPlanes` in exactly the places dxvk-remix sets `DirtyClipPlanes` (`SetClipPlane` changing an enabled plane, `SetRenderState(D3DRS_CLIPPLANEENABLE)`, `ResetState`) and clears it in the `PrepareDraw` after the draw hooks (RL-1.1-11 to -14). The dispatcher reads the flag in the draw hook, so a set flag means "changed since the last draw".
- `alphaSwizzleRenderTargets`: no source edit. The draw hooks read `m_rtSlotTracking.hasAlphaSwizzle` (DXVK's mask of render targets whose view maps alpha to ONE), through the RL-1.1-04 friend access.

## Build notes (no source edits)

- **Build system.** Meson is not used. `Source/FUSE/Relight/cmake/relight_dxvk.cmake` rebuilds the D3D9 and D3D8 DLLs from the vendored `meson.build` source lists, which it reads at configure time. It also generates `version.h`, `buildenv.h`, the GLSL → SPIR-V headers and libdisplay-info's `pnp-id-table.c`. The version string is `v3.1.1-fuse`.
- **DirectX headers.** PE builds use the MinGW-w64 toolchain's DirectX headers. DXVK's `include/native/directx` is LGPL-2.1+ and is not vendored.
- **Linux native build (dxvk-native).** Not ported. It would need SDL2, SDL3 or GLFW for WSI, plus the LGPL DirectX headers fetched into `build/` at configure time. Relight runs its PE DLLs under Wine/Proton on Linux instead.
- **Not built:** `d3d10`, `d3d11` and `dxgi`. None of their sources are vendored. The exception is five interface headers (`src/d3d11/d3d11_{interfaces,include}.h`, `src/dxgi/dxgi_{interfaces,format,include}.h`), which are kept because `src/util/com/com_guid.cpp` includes them.
