# FUSE patches to vendored DXVK

This tree is upstream DXVK v3.1.1 (`b1a1c99ab52b687cf950d62c88bc2fa316b41663`), pinned in `VERSION`. DXVK is under the zlib licence (`LICENSE`), whose clause 2 requires altered source versions to be plainly marked. FUSE marks every edit in two ways:

1. **In the source.** Each edit is wrapped in `// FUSE-DXVK begin: <reason>` / `// FUSE-DXVK end`.
2. **In this list.** Each edit has an entry below, giving the files, its purpose and the Relight work package ([`docs/plans/FUSE_REMIX_PORT_PLAN.md`](../../../docs/plans/FUSE_REMIX_PORT_PLAN.md) §0.4, §2.4).

The plan's patch budget is at most 30 marked blocks (§2.4). An edited file gets its sha256 re-pinned in `VERSION` in the same change.

## Patch list

Each patch has an ID (`RL-x.y-NN`), which is the first token after `FUSE-DXVK begin:`. Block count: 29 of 30 (RL-0.2-01, RL-1.1-01 to -25, RL-1.6-01 and -02, RL-4.1-01; one block each). `VERSION` pins every patched file twice:
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
| RL-1.6-01 | `src/dxvk/dxvk_shader_ir.cpp` (`DxvkIrShader::getLayout()`, 1 block) | Vertex capture (plan §2.5): once FUSE Relight enabled it (the d3d9 dispatcher, for a tap that wants vertex capture on a device with `vertexPipelineStoresAndAtomics`), every D3D9 vertex shader (`vs.` name) gets one extra binding: a storage buffer in the constant-buffer set (set 1, binding 120) sourced from uniform-buffer slot 120, which the dispatcher binds per programmable-VS draw (`Source/FUSE/Relight/capture/vertex_capture/dxvk`). The answer is latched per shader name (`dxvk::fuseRelightVertexCaptureBinding`, `capture/vertex_capture/src/dxvk_hook.cpp`), so layout and code agree; a layout that came from DXVK's shader cache with the binding is not extended twice. | RL-1.6 | FUSE-only. dxvk-remix emits its capture buffer from its dxso compiler; upstream DXVK compiles SM1-3 through dxbc-spirv, so the binding is added to the pipeline layout here. |
| RL-1.6-02 | `src/dxvk/dxvk_shader_ir.cpp` (`DxvkIrShader::getCode()`, 1 block) | For a shader RL-1.6-01 gave the binding: offers the finished SPIR-V, with the set and binding this pipeline's binding map assigns the capture buffer, to FUSE Relight (`dxvk::fuseRelightVertexCaptureCode` -> the tap's `substituteVertexShader`), which appends the capture stores as a SPIR-V pass (`capture/vertex_capture/src/spirv_vertex_capture.cpp`). Declined: the upstream code. | RL-1.6 | FUSE-only (as RL-1.6-01). |
| RL-4.1-01 | `src/d3d9/d3d9_device.cpp` (`D3D9DeviceEx::BindTexture`, before the bind) | Passthrough texture swap (plan §2.3, RL-4.1): with the tap on, `FuseTap::BindTexture` binds the FUSE-owned twin of the sampler's texture when FUSE swapped it (relight.frame.textureSwap), with the same image-view key as the original's sample view; the dispatcher first copies the texture's content into the twin whenever it changed (uploads, `UpdateTexture` / `UpdateSurface`), in DXVK's command stream after the managed upload and mip generation that `PrepareDraw` records before the bind. Returns false for textures that are not swapped, and the upstream bind runs. The rest of RL-4.1 (injection at the first UI draw, the composite, the timeline semaphores, image import) goes through the existing hooks and DXVK's own API from the dispatcher (see "Frame orchestration" below). | RL-4.1 | FUSE-only. |

## Frame orchestration (RL-4.1; one source edit, RL-4.1-01)

`relight.frame.mode` / `relight.frame.textureSwap` (Source/FUSE/Relight/render/frame) wrap the capture tap in a
RenderTap; the dispatcher is the device's frame host (`Source/FUSE/Relight/tap/include/fuse/relight/tap/frame_host.hpp`,
`DeviceEvent::host`, tap interface 4). Everything except the texture-swap bind uses hooks that already exist:

- **Injection point.** The first draw the RL-1.2 classifier marks as the RTX injection point (the first UI draw) injects
  from inside its draw hook (RL-1.1-11 to -14), which runs before DXVK records the draw; without a UI draw, Present
  (RL-1.1-22).
- **Timeline sync.** Two DXVK fences (`DxvkDevice::createFence`, timeline semaphores): at the injection point the dispatcher
  records `signalFence(acquire, A)` and flushes with `FlushAndSync9On12` (the submission reached the queue); FUSE then
  submits its frame (wait acquire >= A, signal release = R) under `lockSubmission` / `unlockSubmission`, which also takes
  FUSE's queue lock through the RL-1.1-02 `queueCallback`; the composite is recorded as `waitFence(release, R)` +
  `copyImage`. On the one shared queue FUSE's batch always follows the signal it waits for.
- **Composite.** FUSE's images are imported with `DxvkDevice::importImage` (non-owning: `DxvkAllocationFlag::Imported`),
  shared, in GENERAL at every hand-over; DXVK copies FUSE's image over the back buffer with `copyImage`, and the UI draws
  that follow land on top.
- **Texture swap.** The twins are imported the same way from the original's `DxvkImageCreateInfo` (UNDEFINED, DXVK-laid-out);
  the dispatcher bumps a content version in the upload / copy hooks (RL-1.1-07, -08, -16) and marks the bound slots dirty,
  so RL-4.1-01 re-copies before the next bind.

With `relight.frame.*` off (the default) the RenderTap is not created, no fence or image is created, and RL-4.1-01 finds no
swap: the passthrough goldens stay bit-identical.

## Capture tap mode (no source edits)

`relight.tap.mode = capture` (RL-1.1, `Source/FUSE/Relight/tap/capture`) runs the RL-1.2 classifier, RL-1.3 geometry capture, RL-1.4 texture tracking and the RL-1.5 fixed-function translation (TranslateTap: material, fog, transforms / clip plane, game lights, camera) inside d3d9.dll on the events of the hooks above, and writes a per-frame capture record. It adds no marked block: the tap is chosen once, at the RL-1.1-06 attach, so a device whose mode is not `capture` runs no capture code, and with the tap off every hook stays the one null-pointer test. The UpdateSurface extent comes from the arguments RL-1.1-07 already passes. `fuse_relight_tap_capture` is linked into `relight_d3d9` link-only, like `fuse_relight_tap`. The capture mode itself adds no block; the translation's change tracking below adds two (RL-1.1-24/25).

## Draw-state change tracking (tap interface 3)

`DrawState` carries two change counters and the render-target alpha-swizzle mask, so the RL-1.5 translation follows processRenderState's `DirtyLights` / `DirtyClipPlanes` flags and `setLegacyMaterialState`'s `m_alphaSwizzleRTs` instead of re-deriving them every frame:

- `lightsVersion`: RL-1.1-24 / RL-1.1-25 above, plus device creation and `Reset` (the RL-1.1-06 hook; upstream's `ResetState` dirties the lights). Two marked blocks; DXVK tracks nothing light-specific that the draw hooks could read.
- `clipPlanesVersion`: no source edit. DXVK sets its own `D3D9DeviceDirtyFlag::ClipPlanes` in exactly the places dxvk-remix sets `DirtyClipPlanes` (`SetClipPlane` changing an enabled plane, `SetRenderState(D3DRS_CLIPPLANEENABLE)`, `ResetState`) and clears it in the `PrepareDraw` after the draw hooks (RL-1.1-11 to -14). The dispatcher reads the flag in the draw hook, so a set flag means "changed since the last draw".
- `alphaSwizzleRenderTargets`: no source edit. The draw hooks read `m_rtSlotTracking.hasAlphaSwizzle` (DXVK's mask of render targets whose view maps alpha to ONE), through the RL-1.1-04 friend access.

## Vertex capture (RL-1.6; two source edits, RL-1.6-01 and -02)

Remix emits vertex capture from its dxso compiler; DXVK 3.1.1 compiles SM1-3 through dxbc-spirv, so Relight adds it to the
finished SPIR-V of each D3D9 vertex shader (`Source/FUSE/Relight/capture/vertex_capture`, plan §2.5). Only the pipeline
layout (RL-1.6-01) and the SPIR-V hand-off (RL-1.6-02) touch DXVK; the rest uses hooks that already exist:

- **Enable.** At the RL-1.1-06 attach the dispatcher asks the tap (`IRelightTap::wantsVertexCapture`, tap interface 5; the
  capture tap answers `rtx.useVertexCapture`). With `vertexPipelineStoresAndAtomics` it turns capture on for the process
  (`dxvk_hook::enable`, sticky) and registers the tap as the SPIR-V substitutor until the device is destroyed.
- **Per draw.** In the draw hooks (RL-1.1-11 to -14), after `onDraw`, a programmable-VS draw that DXVK will draw gets a
  48-byte-slot region of a host-visible buffer (header: base vertex, vertex count), bound with
  `DxvkContext::bindUniformBuffer` at slot 120 through the CS stream; other programmable-VS draws bind an empty region.
- **Readback.** In the Present hook (RL-1.1-22), when the frame had regions, the dispatcher waits for the buffer
  (`D3D9DeviceEx::WaitForResource`) and reports the regions (`IRelightTap::onVertexCapture`) before `onInjectPoint` /
  `onPresent`.
- **Recording tap.** The event stream carries each shader's bytecode once (`"ev":"shader"`), so the replay tools can
  compute the vertexshader hash component (a D3D8 app's shaders reach the tap translated).

Without a tap that wants capture nothing is enabled: RL-1.6-01 / -02 answer "no binding" and DXVK's layout and SPIR-V are
unchanged. The passthrough goldens stay bit-identical with capture on (the pass only reads the shader's outputs).

## Build notes (no source edits)

- **Build system.** Meson is not used. `Source/FUSE/Relight/cmake/relight_dxvk.cmake` rebuilds the D3D9 and D3D8 DLLs from the vendored `meson.build` source lists, which it reads at configure time. It also generates `version.h`, `buildenv.h`, the GLSL → SPIR-V headers and libdisplay-info's `pnp-id-table.c`. The version string is `v3.1.1-fuse`.
- **DirectX headers.** PE builds use the MinGW-w64 toolchain's DirectX headers. DXVK's `include/native/directx` is LGPL-2.1+ and is not vendored.
- **Linux native build (dxvk-native).** Not ported. It would need SDL2, SDL3 or GLFW for WSI, plus the LGPL DirectX headers fetched into `build/` at configure time. Relight runs its PE DLLs under Wine/Proton on Linux instead.
- **Not built:** `d3d10`, `d3d11` and `dxgi`. None of their sources are vendored. The exception is five interface headers (`src/d3d11/d3d11_{interfaces,include}.h`, `src/dxgi/dxgi_{interfaces,format,include}.h`), which are kept because `src/util/com/com_guid.cpp` includes them.
