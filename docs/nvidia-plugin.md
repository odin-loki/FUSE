# NVIDIA DLSS plugin (optional)

FUSE can use NVIDIA DLSS Super Resolution, Ray Reconstruction, Frame Generation and DLSS 5 ("3D-Guided
Neural Rendering") on machines that have an RTX GPU and the NVIDIA runtime. The support is an optional
plugin. FUSE never links NVIDIA code at build time and never ships NVIDIA binaries. A developer points FUSE
at their own copy of the SDK, and FUSE loads it at runtime. Without that copy, FUSE reports the plugin as
"unavailable" and registers no NVIDIA backend. CI builds and tests only the MIT seam and the in-tree mocks.

Design background: [research notes](research/upscaling-framegen-and-post-injectors.md) §1, §4.1 and §6
(recommendation 6.5). The upscaler abstraction the backends plug into is described in
[upscalers.md](upscalers.md).

## At a glance

| Name | What it is | GPU | Where it registers | Provider support |
|---|---|---|---|---|
| `dlss_sr` | DLSS Super Resolution / DLAA (temporal upscaler) | RTX 20+ | `upscale::UpscalerRegistry` | Streamline, NGX bridge, mock |
| `dlss_rr` | DLSS Ray Reconstruction (denoise + upscale) | RTX 20+ | `upscale::UpscalerRegistry` | Streamline, mock |
| `dlss_fg` | DLSS Frame Generation (needs Reflex); multi-frame up to 6x on RTX 50 | RTX 40+ | `nvidia::NvExtensionRegistry` (frame generation) | Streamline (options only), mock |
| `dlss5_look` | DLSS 5 neural "look" pass, one frame in and one frame out | RTX 50 only | `nvidia::NvExtensionRegistry` (look node) | Streamline, mock |

A backend is registered only when the loaded provider reports the feature as supported. Each backend is
also flagged with the lowest RTX generation it needs. The NVIDIA upscalers declare
`UpscalerCaps::apis = Vulkan`, so `UpscalerRegistry::select()` never picks them for CPU dispatch.

## Architecture

```
FUSE (MIT)                                            developer's machine only
------------------------------------------------      ------------------------------------------
nv_backends      dlss_sr / dlss_rr / dlss_fg / dlss5_look
   |  UpscaleInputs + NvGpuFrame -> NvFrameInputs -> validate_inputs()
nv_plugin_loader dlopen / LoadLibraryExW(<FUSE_NVIDIA_SDK_DIR>/<provider>)
   |  fuseNvPluginGetApi(ABI 1.x)  (fuse_nv_plugin_abi.h, plain C)
   +-- fuse_nvplugin_streamline (MIT) --LoadLibrary--> sl.interposer.dll -> sl.dlss*.dll, nvngx_*.dll
   +-- fuse_nvplugin_ngx  (MIT source; binary links libnvsdk_ngx.a) ---> libnvidia-ngx-dlss.so.*
   +-- fuse_nvplugin_mock (MIT, CI)
```

- **Provider ABI**, in `Source/FUSE/Renderer/plugins/nvidia/include/fuse/renderer/nvidia/fuse_nv_plugin_abi.h`.
  It is FUSE's own versioned C ABI. It uses fixed-width integers and no enums. Every struct starts with
  `struct_size`, and there is one exported entry point, `fuseNvPluginGetApi`. The loader rejects a
  provider in these cases:
  - Its ABI major version differs from the loader's. The loader checks `api.abi_version` itself, even when
    the provider returns OK.
  - Its function table is truncated.
  - It has no entry point.
- **Loader**, in `nv_plugin_loader.hpp`. Every failure is a `PluginStatus` with a readable `detail`, never
  a crash or an exception:
  - `NotConfigured`, `LibraryNotFound`, `LoadFailed`, `EntryPointMissing`, `AbiMismatch`: the provider
    library could not be found, loaded or accepted.
  - `RuntimeMissing`, `NoNvidiaGpu`, `DriverTooOld`, `InitFailed`: the provider loaded but could not
    start.
  - `NoFeatures`: the provider started but supports no feature.
  - `DisabledAtBuild`: probing is compiled out.
  - `Available`: the provider is ready.
- **Input contract**, in `nv_input_mapping.hpp`. `NvFrameInputs` holds one resource tag per buffer kind
  plus the constants. `validate_inputs()` rejects a frame with a specific status before the frame reaches
  the provider.
- **Backends**, in `nv_backends.hpp`:
  - `NvTemporalUpscaler` implements `upscale::ITemporalUpscaler`.
  - `NvFrameGenerator` handles frame generation.
  - `NvNeuralLookPass` runs the DLSS 5 pass.
  - `plan_dlss5_look_placement()` chooses where DLSS 5 goes in the look graph.
  - `register_nvidia_backends()` and `probe_and_register_nvidia_backends()` register the backends.

## Enabling it on a developer machine with an RTX GPU

1. Configure with `-DFUSE_ENABLE_NVIDIA_PLUGIN=ON`. The option is OFF by default, and no CI preset turns it
   on. It controls only two things:
   - whether `NvPluginLoader::probe()` and `probe_and_register_nvidia_backends()` look for a runtime at
     start-up;
   - whether the Linux NGX bridge is built.
2. Put the provider and NVIDIA's runtime in one directory, outside the repository, for example
   `~/nvidia-sdk/`:
   - **Windows (Streamline).** Build FUSE; the provider is written to
     `build/<preset>/fuse_nvidia_plugin/streamline/fuse_nvplugin_streamline.dll`. Copy it next to
     `sl.interposer.dll`, `sl.common.dll`, `sl.dlss.dll`, `sl.dlss_d.dll`, `sl.dlss_g.dll`,
     `sl.reflex.dll`, `sl.pcl.dll` (and the DLSS 5 `sl.dlss_nr.dll` plugin once you have it) and the
     `nvngx_dlss*.dll` files from the Streamline 2.14.1 release. Use NVIDIA's signed production DLLs.
   - **Linux (NGX bridge).** Also configure with `-DFUSE_NVIDIA_DLSS_SDK_DIR=<your clone of
     github.com/NVIDIA/DLSS>`. This builds `fuse_nvidia_plugin/ngx/libfuse_nvplugin_ngx.so` against the
     SDK's own headers and statically links its `libnvsdk_ngx.a`. Copy the `.so` next to
     `lib/Linux_x86_64/rel/libnvidia-ngx-dlss.so.<version>`. Streamline documents only Windows builds, so
     this bridge is the Linux path.
3. Point FUSE at the directory:
   - Set the environment variable `FUSE_NVIDIA_SDK_DIR=<dir>`, or set `PluginConfig::sdk_dir` from the
     project settings. A project value wins over the environment, field by field.
   - `FUSE_NVIDIA_PLUGIN_LIB` overrides the provider file name. The default is
     `fuse_nvplugin_streamline.dll` on Windows and `libfuse_nvplugin_ngx.so` on Linux.
   - `FUSE_NVIDIA_PLUGIN_OPTIONS="k=v;k=v"` passes options to the provider. The Streamline provider
     accepts `interposer=<file>` and `ota=1` (NVIDIA over-the-air updates, off by default).
   - Pass the Vulkan (or D3D12) handles in `PluginConfig::{instance, physical_device, device}` with
     `graphics_api`, and your NVIDIA-issued application id in `application_id`.
4. At renderer start-up, call `nvidia::probe_and_register_nvidia_backends(project_config)`. `dlss_sr` and
   `dlss_rr` then appear in `UpscalerRegistry::instance()`. Before each `evaluate()`, bind the frame's native
   images with `NvTemporalUpscaler::bind_gpu_frame(NvGpuFrame)`. Without a bound frame the backend returns
   `BackendUnavailable`, which is what happens today until the render graph hands out texture handles.

## Input mapping

The canonical `fuse::renderer::UpscaleInputs` (`upscale/upscale_inputs.hpp`) plus the frame's native
images (`NvGpuFrame`) map as follows. The last two columns come from the vendored Streamline 2.14.1 headers
and the NGX bridge.

| FUSE | ABI kind / constant | Streamline | NGX bridge |
|---|---|---|---|
| jittered render colour | `COLOR_IN` | `kBufferTypeScalingInputColor` (DLSS 5: `kBufferTypeUpliftInputColor`) | `Feature.pInColor` |
| display output | `COLOR_OUT` | `kBufferTypeScalingOutputColor` (DLSS 5: `kBufferTypeUpliftOutputColor`, may alias the input) | `Feature.pInOutput` |
| depth | `DEPTH` + `DEPTH_INVERTED` flag | `kBufferTypeDepth`, `depthInverted` | `pInDepth`, `DepthInverted` create flag |
| motion (UV, current - previous) | `MOTION_VECTORS`, `mvec_scale = (-1, -1)` | `kBufferTypeMotionVectors`, `mvecScale` (previous - current, [-1, 1]) | `InMVScale = mvec_scale * render size` (pixels) |
| exposure | `EXPOSURE` texture, or `exposure_scale` | `kBufferTypeExposure`; `useAutoExposure` when there is no texture | `pInExposureTexture` / `AutoExposure` flag |
| reactive mask | `REACTIVE_MASK` | `kBufferTypeBiasCurrentColorHint` (DLSS's reactive input) | `pInBiasCurrentColorMask` |
| transparency / composition | `TRANSPARENCY_MASK` | `kBufferTypeTransparencyHint` | n/a |
| HUD-less colour, UI | `HUDLESS_COLOR`, `UI_COLOR_ALPHA` | `kBufferTypeHUDLessColor`, `kBufferTypeUIColorAndAlpha` | n/a |
| RR guides | `DIFFUSE_ALBEDO`, `SPECULAR_ALBEDO`, `NORMALS`, `ROUGHNESS`, `SPECULAR_HIT_DISTANCE` | `kBufferTypeAlbedo`, `...SpecularAlbedo`, `...Normals`, `...Roughness`, `...SpecularHitDistance` | n/a |
| DLSS 5 engine mask | `NEURAL_CONTROL_MASK` | `kBufferTypeUpliftControlMask` | n/a |
| `jitter_px` | `jitter_offset_px` (render pixels, [-0.5, 0.5]) | `jitterOffset` | `InJitterOffset` |
| `camera.projection` / `.view` | `camera_view_to_clip`, `world_to_camera_view` (+ inverses) | `cameraViewToClip`, ...; RR `worldToCameraView` | n/a |
| `camera` vs `previous_camera` | `clip_to_prev_clip = prevVP * inverse(VP)` | `clipToPrevClip` / `prevClipToClip` | n/a |
| `reset_history`, invalidation | `RESET` flag | `reset` | `InReset` |

FUSE matrices are column-major and use column vectors. Streamline wants row-major matrices applied to row
vectors. The two layouts have the same 16 floats in memory, so the mapping copies them unchanged. A gate
reprojects a world point through `clip_to_prev_clip` to check this.

**Validation statuses** (`nvidia::InputStatus`), checked before anything reaches the provider:

- Missing required inputs: `MissingColorInput`, `MissingColorOutput`, `MissingDepth`,
  `MissingMotionVectors`, `MissingDiffuseAlbedo`, `MissingSpecularAlbedo`, `MissingNormals`,
  `MissingRoughness`, `MissingSpecularHitDistance`, `MissingHudlessColor`.
- Sizes: `ZeroRenderSize`, `ZeroOutputSize`, `OutputSmallerThanRender`, `RenderOutputSizeMismatch`
  (DLSS 5 is 1:1), `ResourceExtentMismatch`.
- Constants: `InvalidMotionVectorScale`, `JitterOutOfRange`, `MissingProjection` (RR and frame
  generation), `NonFiniteConstants`, `InvalidExposure`, `HdrRequired` (RR).
- Other: `UnknownFeature`.
- Two warnings do not fail validation: `WarnMissingExposure` and `WarnMissingUiColorAlpha`.

`to_plugin_status()` maps each status to the provider codes `FUSE_NV_ERR_MISSING_INPUT`,
`INVALID_ARGUMENT` and `INVALID_CONSTANTS`.

**DLSS 5 placement.** `plan_dlss5_look_placement(graph)` puts `dlss5_look` right after the temporal
upscaler and before the first display-resolution HDR node of the look graph (DoF in the default graph).
Its input and output are scene-referred HDR at display resolution. This follows NVIDIA's public
description: "the final rendering stage", "one-frame-in, one-frame-out", driven by colour and motion
vectors. The exact colour space DLSS 5 expects is not public, so this placement is an assumption to check
on hardware.

## Licensing

These are the licence decisions for the plugin. Each one follows from the licence texts, which were read in
full on 2026-09-23.

| Component | Licence | Decision |
|---|---|---|
| Streamline public headers (`Engine/lib/streamline/include`, v2.14.1, commit `2122257e`) | MIT (`licence.txt`, "Copyright (c) 2023 NVIDIA CORPORATION") | **Vendored.** Only the 13 headers FUSE compiles against are included, verbatim. The pin is in `VERSION` and checked by ctest `fuse_lint_vendored_pins_streamline` plus a compile-time `static_assert` on `SL_VERSION_*`. |
| `sl_nvperf.h` / `sl_nvperf.dll` | NVIDIA Nsight Perf SDK License (proprietary) | **Not vendored.** |
| `sl_reflex.h`, `sl_pcl.h` | MIT | Not vendored yet (needed for hardware frame generation). v2.14.1's `sl_pcl.h` is ill-formed under Clang in C++23 (`using to_underlying = std::to_underlying;`). |
| DLSS SDK headers (`nvsdk_ngx*.h`) | `SPDX-License-Identifier: LicenseRef-NvidiaProprietary`, NVIDIA RTX SDKs License | **Not vendored.** FUSE does not re-declare NGX's ABI either: on Linux the NGX entry points live in the static `libnvsdk_ngx.a`, so dlopen has nothing to load, and re-creating the snippet interface would breach §4(a) ("may not reverse engineer"). The Linux path is `fuse_nvplugin_ngx.cpp`. It is FUSE's own MIT source, compiled only against the developer's own SDK checkout. |
| DLSS / NGX / Streamline plugin binaries (`nvngx_*.dll`, `libnvidia-ngx-*.so*`, `sl.*.dll`, `libnvsdk_ngx.a`, and the built `libfuse_nvplugin_ngx.so`) | NVIDIA RTX SDKs License | **Never committed.** `.gitignore` covers them. ctest `fuse_nvidia_no_committed_binaries` fails if any is tracked by git, or if `.gitignore` stops ignoring the canonical names. |

**Obligations when you ship a product with DLSS enabled.** These come from the NVIDIA RTX SDKs License
(v. March 14, 2024) and its RTX supplement. Read the licence that comes with your SDK. It governs, not this
summary.

- **§1(c):** distribute the NVIDIA runtime only "as incorporated in object code format into a software
  application". Ship the DLLs or `.so` files next to your game. Never ship them on their own, and never in
  the FUSE source tree (§4(b): "you may not distribute or sublicense the SDK as a stand-alone product").
- **§2(a):** your application "must have material additional functionality, beyond the included portions
  of the SDK".
- **§2(b):** if you distribute modified NVIDIA source code, include the notice "This software contains
  source code provided by NVIDIA Corporation." FUSE ships no NVIDIA source code (it ships only the MIT
  Streamline headers). The notice applies if you add NVIDIA sample code.
- **§2(c):** your end-user terms must be "at least as protective as the terms of this license".
- **§4(e):** you "may not use the SDK in any manner that would cause it to become subject to an open source
  software license". This is why the runtime is kept out of the MIT tree. List it in your third-party
  notices under NVIDIA's terms, not under FUSE's MIT licence.
- **Supplement §1:** DLSS and NGX are licensed "only for their use in systems with NVIDIA GPUs". The loader
  registers nothing without an NVIDIA RTX adapter.
- **Supplement §4:** notify NVIDIA "prior to commercial release" at `https://developer.nvidia.com/sw-notification`.
- **Supplement §7.1(b):** "attribute the use of the applicable SDK and include the NVIDIA Marks on splash
  screens, in the about box of the application (if present), and in credits for game applications". NVIDIA
  must approve the use of its marks beforehand (§7.2(c)).
- **Streamline (MIT):** the provider compiles the Streamline headers in. Include Streamline's copyright
  notice and permission notice (`Engine/lib/streamline/LICENSE.txt`) in your third-party notices.

Suggested credits and notice text (this is not legal advice):

> NVIDIA DLSS: Portions of this software use NVIDIA DLSS, NVIDIA Streamline and NVIDIA NGX technology,
> © NVIDIA Corporation. NVIDIA, the NVIDIA logo, NVIDIA RTX and DLSS are trademarks and/or registered
> trademarks of NVIDIA Corporation. The NVIDIA DLSS runtime is distributed under the NVIDIA RTX SDKs License
> and is not covered by the FUSE MIT licence.
> NVIDIA Streamline headers: Copyright (c) 2022-2025 NVIDIA CORPORATION, MIT licence (full text in
> third-party notices).

## What is verified here and what needs hardware

**Verified in CI.** All of this runs on Linux GCC and Clang with no NVIDIA SDK, GPU or binary. It uses the
MIT mocks in `plugins/nvidia/mock/`.

- `fuse_nvidia_plugin_gates` (154 checks) covers the loader, the failure paths, the Streamline mapping and
  the Streamline provider:
  - The loader finds the mock through `FUSE_NVIDIA_SDK_DIR`, `FUSE_NVIDIA_PLUGIN_LIB` and
    `FUSE_NVIDIA_PLUGIN_OPTIONS`, and project settings override the environment.
  - Failures come back as clean statuses: a missing library gives `LibraryNotFound`, a corrupt file
    `LoadFailed`, a missing export `EntryPointMissing`, an ABI 2.0 provider (even one that claims success)
    or a truncated table `AbiMismatch`. Mock options also produce no GPU, missing runtime, old driver and no
    features. Failed initialisations leak no provider context.
  - Capability flags are correct for RTX 20, 40 and 50 and when Reflex is disabled.
  - Every validation status is covered. Dropping any required buffer yields that buffer's own status.
  - Provider-side rejections work (missing input or constants, unsupported feature, multi-frame generation
    on RTX 40), and an echo round trip shows that handles, jitter, motion-vector scale, flags, quality and
    DLSS 5 intensities reach the provider.
  - Every Streamline buffer type, constant, mode and result mapping is checked.
  - The **Streamline provider runs against a mock `sl.interposer`**. Its `slInit` preferences are checked
    (pinned `kSDKVersion`, manual hooking, frame-based tagging, over-the-air updates off). So are
    `slSetTagForFrame` types and handles, `slSetConstants` booleans (never `eInvalid`),
    `slDLSSSetOptions` and `slDLSSDSetOptions`, `slEvaluateFeature` for SR, RR and DLSS 5 (local uplift
    tags), and `slDLSSGSetOptions` for frame generation.
- `fuse_nvidia_backend_gates` (102 checks) covers registration and the upscaler path:
  - `dlss_sr`, `dlss_rr`, `dlss_fg` and `dlss5_look` appear only when detected, and unregistration removes
    them all. CPU selection never picks a DLSS backend.
  - `dlss_sr` and `dlss_rr` work through the registry with canonical `UpscaleInputs`. The gates cover the
    render size, reset and camera-cut handling, resolution-change invalidation, the specific statuses for a
    missing depth or normals texture, and the `clip_to_prev_clip` reprojection.
  - Frame generation is limited to 2x on RTX 40 and up to 6x on RTX 50. DLSS 5 runs 1:1 at display
    resolution, and its look-graph placement is valid.
  - A default build (`OFF`) probes nothing.
- `fuse_nvidia_no_committed_binaries`: the self-tested matcher, the git-tracked file scan and the
  `.gitignore` coverage.
- `fuse_lint_vendored_pins_streamline`: the vendored bytes, version and licence match the pin.
- The Linux NGX bridge was compile-checked with `-Wall -Wextra -Werror` (GCC 13 and Clang 18) against the
  DLSS SDK v310.9.1 headers, outside the repository. It was not linked or run.

**Hardware checklist.** None of this has been verified. Each item needs an RTX GPU, the NVIDIA driver and
the SDK binaries:

1. Windows plus Streamline 2.14.1 signed DLLs:
   - `slInit` with manual hooking and `slSetVulkanInfo` on FUSE's own device;
   - SR output quality compared against native TAAU, using the metrics harness;
   - RR with real G-buffer guides.
2. Verify the interposer's signature before loading it (Streamline `sl_security.h`, not vendored yet).
3. Frame generation needs Streamline's swap-chain and present hooks. Vendor `sl_reflex.h` and `sl_pcl.h`
   (with a fix for the C++23 `to_underlying` issue) and add Reflex markers. Frame pacing and latency need
   hardware to measure.
4. DLSS 5 needs the `sl.dlss_nr` plugin and its options API, which is not in Streamline 2.14.1's public
   headers. The provider forwards no structure or tone intensity yet; only the mock receives them. The
   colour space and placement also need checking.
5. Linux NGX bridge: link `libnvsdk_ngx.a` into a shared module (check it is PIC), run `NVSDK_NGX_VULKAN_Init`
   and `CreateFeature`/`Evaluate` on a real device, and add RR through `nvsdk_ngx_helpers_dlssd_vk.h`.
6. Every `nvngx_*`/`sl.*` binary stays outside the repository (the gate enforces this in CI).

## Open coordination items

- There is no `IFrameGenerator` registry and no external-node hook in the Look system yet. `dlss_fg` and
  `dlss5_look` live in `nvidia::NvExtensionRegistry` until those exist. When the Look system gains a hook
  (such as the existing `LookPostChain::setSharpenHook`), it can place `dlss5_look` at
  `plan_dlss5_look_placement()`.
- The render graph does not hand out native texture handles yet. `NvGpuFrame` is the binding point.
