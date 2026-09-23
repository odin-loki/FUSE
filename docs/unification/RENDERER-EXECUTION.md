# FUSE Renderer — Execution Tracking

**Plan:** [FUSE_RENDERER_PLAN.md](../plans/FUSE_RENDERER_PLAN.md) (Phases 0–9, tiers T0–T3)
**Related:** [FUSE_MASTER_PLAN.md](../plans/FUSE_MASTER_PLAN.md) §B2 / §B5 · [TRACK-B-VULKAN.md](./TRACK-B-VULKAN.md) · [TRACK-B-RENDER-B5.md](./TRACK-B-RENDER-B5.md) · [B5.7-SCREEN-SPACE-EFFECTS.md](./B5.7-SCREEN-SPACE-EFFECTS.md) · [compute-kernels.md](../compute-kernels.md) · [upscaling research](../research/upscaling-framegen-and-post-injectors.md)
**Snapshot:** 2026-09-23, working tree (including uncommitted work by other agents). Re-survey before starting any package.

This file maps every deliverable in the renderer plan to the code that exists today. For each item it gives a status, the evidence, the gap, and what can be tested on this machine. It then breaks the remaining work into packages that parallel agents can pick up without touching the same files. The packages are ordered by the dependency graph in plan §7.

---

## 0. Legend and ground rules

| Status | Meaning |
|---|---|
| **Done** | The deliverable exists as the plan describes it, runs on a real Vulkan device (Lavapipe) and has a gate test. |
| **Partial** | Some of it exists. Typical cases are a CPU reference or CUDA-only version with no Vulkan GPU path, or the right API with the wrong mechanism underneath. |
| **In progress** | Another agent is adding it now (untracked or modified files in the working tree). Do not edit those files. Coordinate or wait. |
| **Missing** | Nothing usable exists yet. |

| Testability tag | Meaning |
|---|---|
| **LVP** | Can be tested end to end on Lavapipe here and on the GitHub Linux runner (functional and image output, not performance). |
| **LVP-func** | The feature runs on Lavapipe, but the plan's exit criterion is a performance or budget number, which needs real hardware. |
| **CPU** | Unit or parity test only (CPU reference). |
| **HW** | Needs NVIDIA, AMD or Intel hardware or a closed runtime. Manual or nightly run on a dedicated machine. |

Ground rules follow [EXECUTION-PLAN.md](./EXECUTION-PLAN.md) §0: the full ctest suite passes, `fuse_vulkan_validation_gate` reports zero messages, the stub backend (`-DCMAKE_DISABLE_FIND_PACKAGE_Vulkan=ON`) still builds, and every package lands with its gate test.

---

## 1. Executive summary

1. **The renderer that exists is a B2 RHI plus a B5 deferred feature set that runs mostly on the CPU.** The Vulkan layer is real and gated on Lavapipe:
   - device and queue selection
   - swapchain and frame slots with timestamp pools
   - VMA 3.4.0
   - a bindless set
   - an async upload queue
   - `VkPipelineCache` on disk
   - GLSL hot reload
   - a G-buffer raster pass
   - a GPU radix sort in compute
   - a composite pass

   Almost every B5 lighting feature (clustered shading, CSM, DDGI, SSAO/SSR/SSGI, TAA, post stack, atmosphere, fog) is a **CPU reference, a CUDA kernel, or a render-graph schedule stub**. No Vulkan compute or graphics shaders exist for them. The plan's GPU-driven, Vulkan-native frame (§5.1) is therefore mostly **new work**, not a port.
2. **The render graph cannot yet meet the Phase 0 exit criterion**: "a pass can be added without writing any manual barrier". Barriers are planned per resource, but they are:
   - all emitted up front, before any pass runs;
   - recorded with synchronization1 (`vkCmdPipelineBarrier`);
   - applied to one of three fixed images from `VkFrameEncodeContext`, not to the resource the pass declared.

   Transient "alias groups" are computed but no memory backs them. "Async compute" is a flag that runs the pass inline. This is the highest-risk item (plan §9, risk 1), and every later phase depends on it.
3. **Lavapipe here (Mesa 25.2.8, Vulkan 1.4.318) can functionally test T0, T1 and T2, and the T3 RT pipeline.** It reports mesh and task shaders, ray query, acceleration structures, the RT pipeline, 64-bit buffer and image atomics, descriptor buffer, device-generated commands, shader objects, sparse residency and sync2. It has **one queue family with queueCount = 1**, so async-compute overlap cannot be observed; only correctness of the cross-queue path can be tested, through a spoofing layer. It has **no cooperative matrix or vector**, so Phase 9 neural work can only get a CPU reference here. No `slangc` is installed; `libslang2` on this machine is S-Lang, an unrelated library.
4. **Toolchain mismatch.** The plan specifies Slang, volk and Tracy. The repo uses GLSL with `glslangValidator` (and checked-in `.spv` fixtures), links the Vulkan loader directly, and has its own profiler. The single-source compute-kernel framework (C++ bodies for CPU and CUDA, being added now) has only a *seam* for Vulkan compute (`Backend::VulkanCompute`). It cannot generate SPIR-V. The plan's GPU-driven passes must be written as shaders, and the kernel ports stay the CPU and CUDA reference used for parity tests.
5. **Other agents are working in Phase 4 right now** (TAAU, image metrics, UpscaleInputs, the IUpscaler interface with FSR1/NIS/CAS, the Look system, the DLSS/Streamline plugin ABI) and on the CUDA kernel ports for Phases 2, 3 and 6. The packages below are scoped to avoid their files.

---

## 2. Environment and tier testability

Probed with `VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json vulkaninfo` (llvmpipe, LLVM 20.1.2, Mesa 25.2.8-0ubuntu0.24.04.2):

| Capability | Lavapipe | Plan tier | Notes |
|---|---|---|---|
| apiVersion 1.4.318, sync2, dynamic rendering, timeline, BDA, descriptor indexing | yes | T0 | Plan requires 1.3. The FUSE instance currently asks for 1.2 (`src/vk/instance.cpp:128`). |
| `drawIndirectCount`, `multiDrawIndirect`, `shaderDrawParameters` | yes | T0 | |
| `shaderInt64`, `shaderBufferInt64Atomics`, `shaderImageInt64Atomics` (`VK_EXT_shader_image_atomic_int64`) | yes | T0 | Visibility buffer and SW raster can be tested. |
| `sparseResidencyImage2D` | yes | – | Hardware-sparse VSM is an option; the plan's software page table is still recommended. |
| `VK_EXT_mesh_shader` (task + mesh, 1024 invocations, queries) | yes | T1 | `multiviewMeshShader` = false. |
| `VK_KHR_acceleration_structure`, `VK_KHR_ray_query`, `VK_KHR_ray_tracing_pipeline` (recursion 31), `ray_tracing_position_fetch` | yes | T2 / T3 | Functional only; very slow on CPU. Keep scenes tiny. |
| `VK_EXT_descriptor_buffer`, `VK_EXT_device_generated_commands`, `VK_EXT_shader_object`, `VK_EXT_graphics_pipeline_library` | yes | opt | |
| `VK_KHR_cooperative_matrix`, `VK_NV_cooperative_vector` | **no** | T3 | Phase 9 neural work needs hardware. Only a CPU reference is possible here. |
| Queue families | **1 family, queueCount 1** (G/C/T/sparse) | – | Async compute cannot overlap. Test the cross-queue path by extending the existing spoof layers (`tests/vk_layer_split_transfer_family.cpp`). |
| Validation layer (`VkLayer_khronos_validation`) incl. sync validation | yes | – | Already used by `test_gpu_radix_sort_gates.cpp`, `test_b2_upload_queue_family.cpp`. |
| `glslangValidator` 15.1 / `spirv-val` | yes | – | GLSL covers mesh, ray query, RT and int64 atomics, so it is usable as the interim shader language. |
| `slangc` | **no** | – | Must be fetched as a pinned release binary (see WP-0.5). |
| CUDA `nvcc` | compile only | – | CUDA kernels compile; they cannot run here. |
| Qt6, MinGW + Wine; GitHub Linux + Windows runners | yes | – | Windows native MSVC CI is **in progress** (`.github/workflows/fuse-windows-native.yml`, Vulkan compile-only there). |

The GitHub Ubuntu 24.04 runner gets the same `mesa-vulkan-drivers` from noble-updates (`fuse-umbrella-linux.yml` installs it). Every T1+ test must still **detect the capability and skip** (ctest `SKIP_RETURN_CODE`) instead of failing, so older Mesa builds and the stub backend stay green.

---

## 3. Architectural mismatches (plan vs. code)

| # | Plan says | Code today | Consequence and recommendation |
|---|---|---|---|
| **A1** | Slang shaders, modules and generics; precompiled permutations (§4, §5.2) | GLSL (`Renderer/shaders/**`) compiled by `glslangValidator` at configure time (`cmake/FuseShaderSpirvRegen.cmake`, `Renderer/cmake/b5_rhi_rows.cmake`, `gpu_radix_sort.cmake`). The runtime `ShaderCompiler` is "offline-first", with glslang behind `FUSE_SHADER_GLSLANG` (`shader/shader_compiler.hpp`). Checked-in `.spv` fixtures. Vendored FidelityFX and NIS ship GLSL. | Add Slang **alongside** glslang behind one `ShaderCompiler` front end (WP-0.5). Keep glslang for vendored GLSL and as the fallback. Write new Phase 1+ shaders in Slang only once the Slang gate is green. Until then write them in GLSL so work is not blocked. `slangc` is Apache-2.0 with LLVM exception; pin a release binary. |
| **A2** | Render graph derives sync2 barriers, aliases transient memory and schedules async compute (§5.2, Phase 0) | `RenderGraph` (`render_graph.hpp/.cpp`, 820 lines) orders passes, culls unused ones, plans per-resource layout transitions and lifetimes, and computes `aliasGroup`. `execute()` then emits **all** barriers before the first pass (`render_graph.cpp:629-640`). `CommandBufferRecorder::encodeVulkanPipelineBarrier` maps each barrier onto `barrierImage`, `depthImage` or `presentBarrierImage` from `VkFrameEncodeContext`, and buffer barriers onto one `barrierBuffer`, using sync1 `vkCmdPipelineBarrier`. `createTransient()` ignores its `TextureDesc`. `isCompute` passes run inline. `kMaxPassesPerFrame = 32` silently drops extra passes. Pass callbacks are raw function pointers. | Needs **RG v2** (WP-0.3): a per-resource physical binding table, per-pass `vkCmdPipelineBarrier2` batches with stage and access masks derived from declared usage, transient heaps placed with VMA (`vmaCreateAliasingImage`) from the existing lifetime and alias data, queue assignment with timeline-semaphore edges, and debug labels per pass. Keep the current API as a thin facade so B5 schedules, the deferred pipeline and CUDA passes keep compiling. |
| **A3** | Visibility buffer plus material resolve; GPU-driven (Phase 1) | A 6-attachment G-buffer deferred pipeline (`deferred/gbuffer*.{hpp,cpp}`, `frame_pipeline.hpp` 19-pass schedule). The only real raster pass is `GBufferRasterPass`: a **legacy `VkRenderPass`** with per-draw `vkCmdDraw` plus push constants (`src/deferred/gbuffer_raster_pass.cpp:184-209`). CPU-built `DrawList`. `encodeDrawIndexedIndirect` exists; there is no `Count` variant and `drawIndirectCount` is not enabled. | Build the visibility-buffer path as a **new** path (WP-1.x) with dynamic rendering. The deferred G-buffer stays as the reference and the fallback until VB image parity passes. Material resolve can write the same G-buffer layout so clustered shading, SSFX, TAA and post keep working unchanged. |
| **A4** | Virtual shadow maps: 16k clipmaps, page table, physical pool, caching (Phase 3) | 4-cascade CSM with stabilisation and an R32F atlas (`shadow/csm.hpp`, `shadow_atlas.hpp`, `directional_shadow.hpp`, `shadow_pass.hpp`). CPU math only; no shadow raster shaders. SDF soft shadows (CUDA port in progress). No PCF or PCSS. | VSM is a **replacement** (WP-3.x). Keep CSM as the low-end fallback, and reuse the stabilised light basis for clipmap snapping. Page allocation and the page table are pure CPU logic, so they are a PRISM candidate (§8). |
| **A5** | GPU compute for lighting, GI, SSFX and post | The single-source kernel framework (`Core/include/fuse/compute_kernel/*`, **in progress**) runs C++ bodies on CpuReference, CpuParallel and CUDA. `Backend::VulkanCompute` is a seam: "The GLSL/SPIR-V side of `VulkanCompute` is not generated" (`docs/compute-kernels.md`). | Plan-tier passes must be SPIR-V shaders dispatched by the render graph. Use the kernel ports as the **parity oracle**: render on Lavapipe, read back, compare with `run_parity`/`compare_floats` against the CpuReference. Do not attempt C++ to SPIR-V translation. Slang could later share code between kernels and shaders; treat that as research. |
| **A6** | Hybrid CUDA passes | `RGPassDesc::isCuda` plus `cuda/vk_sync.*` interop; seven CUDA passes in the deferred schedule | Keep CUDA passes as optional nodes on hardware builds. The plan's tiers are Vulkan-only, so every CUDA pass needs a Vulkan or CPU fallback to keep T0 honest. |
| **A7** | Vulkan 1.3 minimum with tier detection (§2) | Device selection rejects below 1.2 (`src/vk/device.cpp:200`). It enables Vulkan 1.2 features plus `VK_KHR_dynamic_rendering`. It does **not** enable sync2, `drawIndirectCount`, `shaderInt64` or int64 atomics, `shaderDrawParameters`, or any T1–T3 extension. There is no tier enum. | WP-0.1. Raising the minimum to 1.3 is fine for Lavapipe, MoltenVK is out of scope, and all tier devices are 1.3+. Keep a `FUSE_VK_ALLOW_1_2` escape for one release if anything else relies on it. |
| **A8** | IUpscaler with DLSS, FSR and XeSS backends | **In progress** by other agents: an IUpscaler-style interface with FSR1/NIS/CAS, TAAU, UpscaleInputs, and the NV plugin ABI (`plugins/nvidia/include/fuse/renderer/nvidia/fuse_nv_plugin_abi.h`). Existing TAA is same-resolution and CPU-only. | Plan Phase 4 names **FSR (temporal) via FidelityFX** and **XeSS**. Neither is covered by the in-progress work, which vendors FSR1 and CAS only. Add them as later packages behind the same interface. |
| **A9** | Tracy GPU zones and RenderDoc (Phase 0) | Own `Core/profiler`; per-frame timestamp pool in `src/vk/frame.cpp`; kernel stats registry. No Tracy, no RenderDoc in-app API, no `vkCmdBeginDebugUtilsLabelEXT`. Object names exist (`test_b5_rhi_object_names`). | WP-0.6: a Tracy adapter behind `fuse::profiler`, default OFF, and a RenderDoc in-app capture hook. RG v2 emits per-pass labels and timestamps. |
| **A10** | Many-light and RT denoising via NRD (Phase 6b, 7) | Nothing yet | NRD and the RTXDI family are under the **NVIDIA RTX SDKs License**. Keep them as an optional out-of-tree plugin like the DLSS one. The in-tree default denoiser should be MIT or self-written: FidelityFX Denoiser (MIT), or SVGF/A-SVGF from the papers. |

### 3.1 Third-party licence notes (plan §4, with [research §6](../research/upscaling-framegen-and-post-injectors.md#6-licensing-matrix-and-blockers))

| Library | Licence | In repo | Rule for FUSE (MIT tree) |
|---|---|---|---|
| VMA | MIT | **Vendored** `Engine/lib/vma` v3.4.0, pinned plus sha256 (`fuse_lint_vendored_pins_vma`) | OK |
| Slang | Apache-2.0 WITH LLVM-exception | absent | OK to fetch or pin a binary. Keep the NOTICE. Do not vendor the source tree (large). |
| volk | MIT | absent | OK to vendor (header plus one `.c`) |
| meshoptimizer | MIT | absent | OK to vendor |
| Tracy | BSD-3-Clause | absent | OK. Optional, default OFF, notice in credits. |
| RenderDoc in-app API (`renderdoc_app.h`) | MIT | absent | OK. Header only; the capture tool is not shipped. |
| NVIDIA Streamline | MIT headers; `sl_nvperf` is proprietary | **In progress**: `Engine/lib/streamline` v2.14.1 headers subset, pinned | Headers and adapter OK. `sl.interposer.dll`, `nvngx_*.dll` and DLSS-G are **never committed** (RTX SDKs License). Windows-only; Linux uses an NGX bridge. |
| DLSS / NGX / DLSS-G / Reflex runtime, **NRD**, RTXDI, RTXPT, RTXNTC | NVIDIA RTX SDKs License | – | **Not in the MIT tree.** Out-of-tree plugin loaded at runtime, NVIDIA GPUs only, attribution notice, no CI. Must never be made subject to an OSS licence. |
| NRI | MIT | – | Only needed if NRD is used. It then lives inside the NRD plugin. |
| AMD FidelityFX SDK v1.1.4 (FSR 1/2/3.1, CAS, Denoiser, SPD, LPM, …) | MIT | **In progress**: FSR1 plus CAS subset vendored | OK. FSR 3.1 temporal and FG supports Vulkan. **FSR 4 / Redstone is DX12-only**; do not use the leaked FSR 4 source. |
| Intel XeSS SDK | Intel Simplified Software License (binary only, no modification) | – | Optional runtime plugin. Cannot vendor source. The Vulkan path exists (1.1+); no Linux build is documented. |
| NVIDIA NIS | MIT | **In progress** (`Engine/lib/nvidia-nis` v1.0.3) | OK |
| NVlabs FLIP | BSD-3 | **In progress?** (image-metrics agent) | OK, with notice |
| ENB, iMMERSE, Special K | proprietary / all rights reserved / GPL-3 | – | **Do not** import, name or claim compatibility (plan §Phase 4 note; research §2.3). |

---

## 4. Deliverable status

### Phase 0 — Foundation

| Deliverable | Status | Evidence | Gap | Testable |
|---|---|---|---|---|
| Vulkan 1.3 device creation with feature and **tier** detection | **Partial** | `src/vk/device.cpp` (selection plus features, lines 180-560), `src/vk/instance.cpp:128` (`VK_API_VERSION_1_2`). Tests: `test_b2_physical_device_selection`, `test_b2_device_queue_families`, `test_vulkan_bootstrap`, `vk_layer_spoof_multi_gpu` | Instance and device are 1.2. No `VkPhysicalDeviceVulkan13Features` chain (sync2 and dynamic rendering come from the KHR extension only). `drawIndirectCount`, `shaderInt64`, int64 atomics and `shaderDrawParameters` are not enabled. No `RenderTier` (T0–T3) or per-feature caps struct. No mesh, RT, descriptor-buffer, DGC or shader-object probing. | LVP (all tiers detectable). A spoof layer can mask features to force T0 or T1. |
| VMA integration | **Done** (budget and defrag **Partial**) | `Engine/lib/vma` 3.4.0; `src/vk/allocator.cpp` (1133 lines, pools, dedicated allocs, `vmaCalculateStatistics`), `gpu_alloc_stats.cpp`. Tests: `test_b5_rhi_memory_types`, `test_b2_frame_alloc_budget`, `test_rhi_resource_destroy_order` | No `vmaGetHeapBudgets` (with `VK_EXT_memory_budget`), no defragmentation, no aliasing allocations for RG transients | LVP |
| volk loader | **Missing** | Links `Vulkan::Vulkan` (`cmake/FuseFindVulkan.cmake`, which another agent has **modified**) | Vendor volk; `VMA_DYNAMIC_VULKAN_FUNCTIONS`; one loader init in `instance.cpp` and `device.cpp` | LVP |
| Validation layers | **Done** | `instance.cpp:165`; `fuse_vulkan_validation_gate` (EXECUTION-PLAN §2: 28 VUIDs down to 0); sync validation opted in by `test_gpu_radix_sort_gates`, `test_b2_upload_queue_family` | Sync validation is not on for the whole suite (plan §8) | LVP |
| Debug labels | **Partial** | Object names: `src/vk/debug_utils.cpp`, `test_b5_rhi_object_names` | No `vkCmdBegin/EndDebugUtilsLabelEXT` per RG pass | LVP |
| Render graph: pass declaration | **Done** (v1) | `render_graph.hpp`; `test_render_graph`, `test_b2_render_graph_budget` (perf gate), `test_resource_layout_tracking`, `test_frame_barrier_integration` (Hybrid) | 32-pass cap. Function-pointer callbacks. No per-pass queue, subresource ranges or buffer ranges. | LVP |
| Render graph: automatic barriers (sync2) | **Partial** | `planBarriersForPass`, `render_graph.cpp:629-640`, `command_buffer.cpp:228` | See **A2**: barriers are batched before every pass, are sync1, and target fixed images, not the declared resource. Manual barriers remain in `gpu_radix_sort.cpp:108,118`, `resource_manager.cpp`, `upload_queue.cpp`, and the legacy render pass in `gbuffer_raster_pass.cpp`. | LVP (sync validation proves it) |
| Render graph: transient aliasing | **Partial** | `assignTransientAliasGroups`, `RGResourceLifetime::aliasGroup` | `createTransient` drops the desc; no physical memory, so nothing is aliased | LVP (VMA aliasing plus readback) |
| Render graph: async compute queue | **Missing** | `RGPassDesc::isCompute` runs inline (`render_graph.cpp:666`). The device creates compute and transfer families when present (`device.cpp:430-440`). Timeline semaphores exist (`test_b5_rhi_frame_timelines`). | Queue assignment, cross-queue ownership transfer and timeline waits | LVP-func via a spoof layer (single real queue) |
| Bindless descriptor registry | **Done** (descriptor-buffer variant **Missing**) | `src/vk/bindless.cpp` (891 lines, generation-checked slots, device-derived array sizes, update-after-bind). Tests: `test_bindless_descriptors`, `test_b2_bindless_churn` | Shader-side 32-bit handle packing convention to document. Optional `VK_EXT_descriptor_buffer` backend. | LVP |
| Slang toolchain | **Missing** | GLSL only (see **A1**) | Pinned `slangc`, CMake rule, SPIR-V validation, reflection → pipeline layout | LVP once `slangc` is fetched |
| Shader hot reload | **Done** (GLSL) | `shader/shader_watch.cpp`, `ShaderCompiler::pollHotReload`; `test_shader_watch`, `test_b5_rhi_hot_reload` | Extend to Slang modules and include dependency tracking | LVP |
| Pipeline cache | **Partial** | `src/vk/pipeline_cache.cpp` (on-disk, per-SPIR-V-hash file); `test_pipeline_cache`, `test_shader_pipeline`, `test_graphics_pipeline` | No permutation keying, no background compile job, no `VK_EXT_graphics_pipeline_library` or shader-object fast path | LVP |
| Tracy GPU zones | **Missing** | `Core/profiler`, `frame.cpp` timestamp pool | Tracy adapter, per-pass GPU zones | LVP (zones recorded; timings meaningless) |
| RenderDoc integration | **Missing** | – | In-app API hook (`renderdoc_app.h`), capture-frame hotkey or CLI flag | Manual |
| **Exit:** zero validation errors on test scenes | **Partial** | Current suite is clean | No plan reference scenes exist yet (§8) | LVP |
| **Exit:** pass added without a manual barrier | **Missing** | – | Blocked on RG v2 | LVP |

### Phase 1 — GPU-driven visibility buffer

| Deliverable | Status | Evidence | Gap | Testable |
|---|---|---|---|---|
| GPU scene buffers with delta uploads | **Partial** | Upload infrastructure: `src/vk/upload_queue.cpp` (1008 lines, staging ring, transfer-family ownership), `test_b2_async_upload`, `test_b2_staging_wrap`, `test_b2_upload_queue_family`. Material SSBO table: `material/material_system.*`. Bone buffer: `skinning/bone_buffer.*`. | No persistent instance, transform, mesh, meshlet or light buffers addressed by BDA. No dirty-range tracking from ECS. No previous-frame transforms (needed for per-object MVs). | LVP |
| Compute instance culling (frustum plus two-phase Hi-Z) | **Missing** | CPU frustum culling only (`Hybrid` `test_parallel_cull_edge`). A Hi-Z exists only for SSR (`Compute/.../screen_space_effects.hpp:93`). | Hi-Z build (SPD-style single-pass downsample; FidelityFX SPD is MIT). Phase-1 and phase-2 cull shaders. | LVP (Hi-Z pyramid and cull lists read back) |
| 64-bit visibility buffer (depth plus instance and triangle ID) | **Missing** | – | R64_UINT atomics path, or raster to R32G32_UINT plus depth | LVP (`shaderImageInt64Atomics` = true) |
| Material resolve (classification plus indirect dispatch) | **Missing** | G-buffer layout to target: `deferred/gbuffer.hpp`, `shaders/common/gbuffer.glsl` | Attribute reconstruction (barycentrics from VB), material tile classification, per-material indirect dispatch | LVP (image parity against the existing G-buffer raster) |
| `vkCmdDrawIndexedIndirectCount` submission | **Partial** | `CommandBufferRecorder::encodeDrawIndexedIndirect` (`command_buffer.cpp:588-630`) | No Count variant; feature not enabled; no GPU-written args | LVP |
| Meshlets built offline with meshoptimizer | **Missing** | No meshoptimizer in `Engine/lib`. Asset cooking exists in `Hybrid/cooked_asset_bindings.*`. | Vendor meshoptimizer, meshlet cook step, meshlet asset format | CPU plus LVP |
| **Exit:** CPU cost constant from 1k to 100k instances | **Missing** | Comparable CPU gate: `fuse_b2_render_graph_budget` | Needs the GPU-driven path. CPU recording time *is* measurable on Lavapipe and host-independent. | LVP (CPU side only) |
| **Exit:** two-phase occlusion, no popping on camera cuts | **Missing** | – | Deterministic camera-cut sequence plus image diff | LVP |

### Phase 2 — Clustered lighting

| Deliverable | Status | Evidence | Gap | Testable |
|---|---|---|---|---|
| Froxel/cluster grid 16×9×24, tunable, with a compute light-assignment pass | **Partial** / **In progress** | `lighting/clustered.hpp` (`ClusterDesc` 16×9×24, max 32×18×64), `clustered_light_culler.cpp`. CUDA and single-source port **in progress**: `lighting/clustered_kernel.hpp`, `kernels/clustered_lighting.cu`, `test_clustered_kernel_parity`. Tests: `test_clustered_light_culler`, `test_b5_clustered_gates`. | No Vulkan compute assignment or shading shader. Grid lives in `std::vector`, not GPU buffers. | LVP (with CPU parity) |
| Point, spot, area (LTC) and directional lights | **Partial** | Point: CPU shade. Spot: culled but "not handled by this pass" (`clustered_kernel.hpp:308`). Directional: CSM sun. | Spot shading, LTC area lights (LUT textures, MIT reference: Heitz et al.), unified GPU light buffer | LVP |
| PBR BRDF with multi-scatter energy compensation | **Partial** | `material/brdf.{hpp,cpp}`, `shaders/common/brdf.glsl`, `test_material_system` | No energy compensation (Kulla-Conty or Fdez-Agüera). No DFG LUT. | CPU plus LVP (white-furnace test) |
| Clustered data shared with forward transparency | **Missing** | `DeferredPassId::TransparentPass` is a schedule stub | Forward pass reading the cluster lists; OIT is out of scope | LVP |
| **Exit:** 4,096 lights within budget | **Missing** | – | Correctness at 4,096 lights on LVP; the budget number needs HW | LVP-func / HW |

### Phase 3 — Virtual shadow maps

| Deliverable | Status | Evidence | Gap | Testable |
|---|---|---|---|---|
| 16k² VSM per directional light with clipmap levels | **Missing** (CSM exists) | `shadow/csm.*`, `shadow_atlas.*`, `directional_shadow.*`, `shadow_pass.*`, `test_shadow_system`, `test_csm_guards`, `test_b5_shadows_gates` | Everything. Reuse the stabilised light basis from `CascadeLightSpaceLayout`. | LVP |
| Page marking, page table, physical page pool | **Missing** | – | Pure logic suits a CPU model plus PRISM. GPU marking shader. | CPU plus LVP |
| Static page caching and invalidation on movement | **Missing** | – | Needs GPU-scene previous/current bounds | LVP |
| PCF and optional PCSS | **Missing** | Nothing in `src/shadow` | Filtering in the shading shader | LVP |
| Point and spot lights on cube or single virtual pages | **Missing** | – | | LVP |
| SDF soft shadows (not in plan; existing T0 extra) | **In progress** | `shadow/sdf_shadows.hpp`, `sdf_shadow_kernel.hpp`, `kernels/sdf_shadows.cu`, `test_sdf_shadows_kernel_parity` | Keep as an optional contact or area term | CPU |
| **Exit:** no cascade seams near to far | **Missing** | CSM seam test exists (`fuse_b5_shadows_gates`, 0 mismatches), reusable | Port the same seam test to VSM | LVP |
| **Exit:** cached-frame shadow cost under 1 ms | **Missing** | – | Page-render counts are measurable on LVP; timing needs HW | LVP-func / HW |

### Phase 4 — Upscaling and temporal pipeline

| Deliverable | Status | Evidence | Gap | Testable |
|---|---|---|---|---|
| Motion vectors (camera and per-object) | **Partial** | G-buffer velocity attachment (`deferred/gbuffer.hpp`); TAA consumes pixel velocity (`taa/taa_cpu_resolve.hpp:27-50`) | No GPU MV generation with previous transforms; no skinned or SDF MVs | LVP (analytic reprojection test) |
| Jitter sequence | **Done** | `taa/taa_jitter.*` (Halton), `test_taa_pass` | Phase count from the upscale ratio (in progress with TAAU) | CPU |
| Reactive and transparency masks | **In progress** | UpscaleInputs agent | – | CPU / LVP |
| IUpscaler interface | **In progress** | IUpscaler, FSR1, NIS and CAS agent; `Engine/lib/{fidelityfx,nvidia-nis}` vendored; planned `include/fuse/renderer/upscale/` | – | CPU plus LVP (GPU reference gate on Lavapipe) |
| DLSS via Streamline | **In progress** | `plugins/nvidia/.../fuse_nv_plugin_abi.h`, `Engine/lib/streamline` headers | Runtime: HW only (Windows plus RTX) | CPU (mock provider) / HW |
| FSR via FidelityFX (temporal 2.x/3.1) | **Missing** | Only FSR1 (spatial) in progress | Vendor FSR 3.1 Vulkan backend (MIT); `IUpscaler` backend | LVP (runs, slow) |
| XeSS | **Missing** | – | Binary-only runtime plugin (Intel licence) | HW |
| Native TAA fallback | **Done** (same-res, CPU) / TAAU **In progress** | `taa/taa_cpu_resolve.*`, `taa_resolve.*`, `taa_history.*`; `test_taa_cpu_resolve_wiring`, `test_b5_taa_ssfx_gates`; `taa/taa_kernel_common.hpp` (new, in progress) | Vulkan compute TAAU shader | LVP |
| Frame generation (Streamline, FSR) and Reflex | **Missing** (Reflex headers vendored) | `Engine/lib/streamline/include/sl_reflex.h`, `sl_dlss_g.h`, `sl_pcl.h` | FSR 3.1 FG (MIT, Vulkan), DLSS-G plugin, `ILatencyProvider`, present timing | CPU (timeline model) / HW |
| Post stack: exposure, bloom, tonemap (AgX/ACES), grading LUT, DoF, motion blur | **Partial** (CPU) / Look **In progress** | `postprocess/{auto_exposure,bloom,tonemap,tonemap_curve,color_grade,dof,motion_blur,lens_flare,post_stack}.*`; `test_post_process_stack`, `test_b5_post_gates`; ACES (Narkowicz), Filmic, Reinhard, Neutral (`tonemap.hpp:8-14`); `look/look_params.hpp` (new) | **No AgX**, no 3D LUT (Look agent), **no GPU shaders** for any post pass | CPU plus LVP |
| **Exit:** runtime-switchable upscalers with no missing-input artefacts | **Missing** | – | Switch test across NativeTaau, FSR1, NIS and FSR3 on LVP; DLSS and XeSS on HW | LVP / HW |

### Phase 5 — Mesh shaders and virtual geometry (T1)

| Deliverable | Status | Evidence | Gap | Testable |
|---|---|---|---|---|
| Task/mesh pipeline: frustum, cone and Hi-Z culling per meshlet | **Missing** | `graphics_pipeline.*` has no mesh stages | Pipeline plus shaders | LVP (`meshShader` = true) |
| Cluster DAG via meshoptimizer simplification, screen-space-error LOD | **Missing** | – | DAG builder, error metric, cooked format | CPU plus LVP |
| Cluster page streaming and residency manager | **Missing** | Streaming infrastructure elsewhere: `WorldPartition`, VFS (`docs/unification/vfs-mount-plan.md`), `upload_queue` | Page file format, prioritised residency, coarse-LOD guarantee | CPU plus LVP |
| Compute software rasteriser for micro-triangles | **Missing** | – | 64-bit atomic VB writes (shares WP-1.4 format) | LVP |
| Fallback to the Phase 1 indirect path on T0 | **Missing** | – | Tier switch in the VB front end | LVP (mask mesh shaders) |
| **Exit:** over 100M source triangles within budget; no LOD cracks | **Missing** | – | Crack test (watertight DAG boundary check) on LVP; budget needs HW | LVP-func / HW |

### Phase 6 — Global illumination

| Deliverable | Status | Evidence | Gap | Testable |
|---|---|---|---|---|
| 6a Irradiance probes with visibility (depth moments) | **Partial** / **In progress** | `gi/ddgi.hpp` (probe grid, bordered depth-moment atlas layout, lines 367-376), `ddgi_cpu.*`, `ddgi_kernels.hpp`; CUDA port **in progress** (`ddgi_probe_kernel.hpp`, `kernels/ddgi_probe_update.cu`, `test_ddgi_kernel_parity`); `test_ddgi`, `test_ddgi_atlas_upload`, `test_b5_ddgi_gates` | Vulkan compute update and blend; Chebyshev visibility in shading | LVP |
| 6a Probe rays by ray query (T2) or against a global SDF or voxels (T0) | **Partial** (T0 CPU and CUDA) | SDF ray march `Compute/.../ray_march_kernel.hpp` (in progress); SVO `Scene/svo*`, `svo_ray_kernel.hpp` (in progress) | No BLAS/TLAS builder, no ray query shader, no GPU global SDF | LVP (both paths) |
| 6a Probe relocation and classification (inside geometry) | **Partial** | Border-shell classification only (`ddgi.hpp:96-140`) | Relocation offsets; inside, backface or inactive classification | CPU plus LVP |
| 6b RT soft shadows (area lights) and reflections, ray query | **Missing** | – | Needs the AS builder (shared with 6a T2) | LVP (tiny scenes) |
| 6b Denoising (NRD SIGMA, REBLUR/RELAX) | **Missing** | – | NRD is RTX-SDK-licensed (plugin only). In-tree default: SVGF/A-SVGF or FidelityFX Denoiser (MIT). | LVP (in-tree) / HW (NRD) |
| 6c SSR and SSAO/GTAO | **Partial** / **In progress** | `ScreenSpace/…/{hbao,ssr,ssgi}.hpp` plus `*_kernel.hpp` (in progress); `Compute/…/screen_space_kernels.hpp`; B5.7 doc; `test_screen_space_kernel_parity` | HBAO, not GTAO; no Vulkan shaders | CPU plus LVP |
| 6d Radiance cascades (research) | **Missing** | – | Optional | LVP |
| **Exit:** dynamic time of day, stable GI, no leaks, no probe grid | **Missing** | – | Leak test scene (thin wall) plus temporal-stability metric | LVP |

### Phase 7 — ReSTIR and path tracing

| Deliverable | Status | Evidence | Gap | Testable |
|---|---|---|---|---|
| ReSTIR DI (spatiotemporal reservoirs) | **Missing** | GPU radix sort is reusable (`compute/gpu_radix_sort.*`) | All | LVP (tiny) |
| ReSTIR GI | **Missing** | – | All | LVP (tiny) |
| Real-time path tracing mode with DLSS RR or NRD | **Missing** | – | PT on LVP (RT pipeline present); RR and NRD on HW | LVP / HW |
| Light BVH or light tree | **Missing** | `Scene/bvh_stub.*` is a stub | Build (CPU), GPU traversal | CPU plus LVP |
| **Exit:** 10k+ emissive lights, stable and low-noise | **Missing** | – | Variance metric against a 4k-spp reference on a tiny scene | LVP-func |

### Phase 8 — Volumetrics and atmosphere

| Deliverable | Status | Evidence | Gap | Testable |
|---|---|---|---|---|
| Froxel fog with temporal reprojection, lit by clustered lights, shadows and GI | **Partial** | `volumetric/volumetric_fog.hpp` (`FroxelGridDesc`, froxel camera mapping), `light_shafts.*`; `test_volumetric_lighting` | No reprojection, no GPU injection or integration, no shadow or GI lookup | LVP |
| Physically based sky (Hillaire LUTs) | **Partial** | `atmosphere/*` single-scattering plus transmittance and sky LUTs (nearest sampling) and sun disk; `test_atmosphere_sky`, `test_b5_atmosphere_gates` | Multi-scattering LUT, sky-view LUT, aerial-perspective froxels, bilinear sampling, GPU passes | CPU plus LVP |
| Volumetric clouds (raymarched, temporally amortised) | **Missing** | – | All | LVP |
| **Exit:** 1.5 ms, no ghosting | **Missing** | – | Ghosting metric on LVP; time needs HW | LVP-func / HW |

### Phase 9 — Neural and advanced (T3, optional)

| Deliverable | Status | Evidence | Gap | Testable |
|---|---|---|---|---|
| Neural texture compression (cooperative matrix or vector) | **Missing** | – | CPU reference MLP first | CPU / HW |
| Neural radiance cache | **Missing** | – | | CPU / HW |
| 3D Gaussian splatting composited with the VB | **Missing** | GPU radix sort exists and is gated on Lavapipe (`test_gpu_radix_sort_gates`) | Splat raster, depth composite with the VB | LVP |
| Device-generated commands for GPU pipeline selection | **Missing** | – | `VK_EXT_device_generated_commands` is on LVP | LVP |
| **Exit:** measured against conventional path | **Missing** | – | Harness equals §8 metrics plus memory accounting | CPU / HW |

### §3 Vulkan extensions and features

| Extension / feature | Tier | Enabled today | Status | Package |
|---|---|---|---|---|
| Dynamic rendering | T0 | Via `VK_KHR_dynamic_rendering` when listed (`device.cpp:490`). Raster still uses legacy `VkRenderPass` (`render_pass.cpp`, `gbuffer_raster_pass.cpp`, `command_buffer.cpp:411,473`). | Partial | WP-0.1, WP-0.3 |
| Synchronization2 | T0 | No | Missing | WP-0.1, WP-0.3 |
| Timeline semaphores | T0 | Yes (required) | Done | – |
| Descriptor indexing | T0 | Yes, plus UAB and non-uniform flags | Done | – |
| Buffer device address | T0 | Enabled if supported; not used by any shader | Partial | WP-1.1 |
| Draw indirect count | T0 | No | Missing | WP-0.1, WP-1.3 |
| Shader draw parameters, 64-bit atomics | T0 | No | Missing | WP-0.1 |
| VK_EXT_mesh_shader | T1 | No | Missing | WP-0.1, WP-5.1 |
| VK_EXT_descriptor_buffer | T0+ | No | Missing (optional) | WP-0.4b |
| VK_EXT_device_generated_commands | T0+ | No | Missing (optional) | WP-9.3 |
| VK_KHR_acceleration_structure / ray_query | T2 | No | Missing | WP-0.1, WP-6.2 |
| VK_KHR_ray_tracing_pipeline | T3 | No | Missing | WP-7.3 |
| VK_KHR_cooperative_matrix, VK_NV_cooperative_vector | T3 | No (not on LVP) | Missing | WP-9.1 (HW) |
| VK_EXT_shader_object | opt | No | Missing (optional) | WP-0.5b |
| VK_AMDX_shader_enqueue | monitor | – | Do not build | – |

### §4 Third-party dependencies

| Library | Phase | Status | Evidence / action |
|---|---|---|---|
| VMA | 0 | **Done** | `Engine/lib/vma` 3.4.0, pin lint plus `static_assert` |
| Slang | 0 | **Missing** | WP-0.5 (pinned binary download, `cmake/FuseSlang.cmake`) |
| volk | 0 | **Missing** | WP-0.2 (`Engine/lib/volk`, pin lint) |
| meshoptimizer | 1, 5 | **Missing** | WP-1.2 (`Engine/lib/meshoptimizer`, pin lint) |
| Streamline | 4 | **In progress** | `Engine/lib/streamline` headers plus `plugins/nvidia` (other agent) |
| FidelityFX SDK | 4 | **In progress** (FSR1 and CAS) / FSR3.1 **Missing** | Extend `Engine/lib/fidelityfx` pin with the FSR3.1 Vulkan backend (WP-4.2) |
| XeSS SDK | 4 | **Missing** | WP-4.3 (plugin, binary fetched by the developer) |
| NRD (via NRI) | 6 | **Missing** | WP-6.4b (plugin, out-of-tree binary) |
| Tracy | 0 | **Missing** | WP-0.6 |
| RenderDoc / Nsight | 0 | **Missing** | WP-0.6 (RenderDoc in-app API); Nsight is manual |

### §8 Validation and testing

| Item | Status | Evidence | Gap / plan |
|---|---|---|---|
| Reference scenes: Sponza, Bistro, high-density, open-world | **Missing** | No scene assets (only `Engine/lib/bullet/data/sponza.mtl`) | Download at test time (not vendored; Sponza is CC-BY, Bistro is CC-BY 4.0; verify). Add a procedural **mini** scene per phase for Lavapipe (small, deterministic). WP-0.7. |
| Image regression (FLIP or SSIM per phase, in CI) | **Partial** / **In progress** | Pixel readback: `test_b2_triangle_readback`, `vk/image_readback.*`; image-metrics agent (FLIP, SSIM, PSNR) | Golden store plus `fuse_renderer_golden` ctest label; Lavapipe renders are deterministic enough for FLIP thresholds | 
| Perf regression (Tracy captures against §5.3 budgets, fail above 5%) | **Missing** | CPU-side perf gates exist (`fuse_b2_render_graph_budget`, label `perf`) | Needs HW runners; on LVP gate only **CPU submission cost** and **GPU work counters** (draws, dispatches, pages rendered) |
| Validation layers mandatory in debug, sync validation for RG | **Partial** | `fuse_vulkan_validation_gate`; sync validation in 2 tests | Turn on sync validation suite-wide for `rg`/`renderer` labels (WP-0.7) |
| Tier matrix (≥ 1 T0 and 1 T2 device per phase) | **Missing** | – | LVP stands in as both "T0 (masked)" and "T2" functionally; real T2 device needed for perf and vendor quirks |
| PRISM integration (render graph, GPU scene, residency; coverage, sanitisers, BMC) | **Missing** | ASan/TSan presets exist (`cmake/FuseSanitizers.cmake`, `fuse-tsan-nightly.yml`); no PRISM tooling in repo | Isolate pure-logic cores (RG compiler, page table, residency LRU, VMA-like pools) as header-light libraries with no Vulkan types so a BMC tool (e.g. CBMC/ESBMC) can take them. WP-0.8. |

---

## 5. Work packages

Packages are sized for one agent each, with **disjoint file ownership**. "Owns" lists the directories and files the package may create or edit. Everything else is read-only for it. Shared registration files (`Source/FUSE/Renderer/CMakeLists.txt`, `Source/FUSE/Renderer/tests/CMakeLists.txt`) are touched through **one include line per package**: each package adds its own `Source/FUSE/Renderer/cmake/rp_<wp>.cmake` and a single `include()` line, as the existing `cmake/b5_*.cmake` files do.

### 5.0 Do-not-touch (in progress by other agents at snapshot time)

- `Source/FUSE/Core/include/fuse/compute_kernel/**`, `Core/src/compute_kernel/**`, `Core/tests/*compute_kernel*`
- `Source/FUSE/Renderer/include/fuse/renderer/{gi/ddgi_probe_kernel.hpp, lighting/clustered_kernel.hpp, shadow/sdf_shadow_kernel.hpp, shadow/sdf_shadows.hpp, taa/taa_kernel_common.hpp, look/**}`, `Renderer/kernels/**`, `Renderer/src/shadow/sdf_shadows.cpp`, `Renderer/tests/test_*_kernel_parity.cpp`, and the modified `Renderer/cmake/b5_{clustered,ddgi,shadows}.cmake`, `src/gi/ddgi*.cpp`, `src/lighting/clustered_*.cpp`
- `Source/FUSE/Renderer/include/fuse/renderer/upscale/**` (planned by the IUpscaler/FSR1/NIS/CAS agent), image-metrics and TAAU files, `Renderer/plugins/nvidia/**`
- `Engine/lib/{fidelityfx,nvidia-nis,streamline}/**`
- `Source/FUSE/{ScreenSpace,Compute,Scene,VFX,Physics,Animation,Audio,Modules/fx}/**` kernel files
- `.github/workflows/fuse-windows-native.yml`, `CMakePresets.json`, `cmake/{FuseFindVulkan,FuseWarnings,FuseShipping,FuseLintGates}.cmake`, `Tools/FUSE/Lint/fuse_lint.cpp`

Packages that must eventually edit one of these (e.g. WP-0.2 volk into `FuseFindVulkan.cmake`) are marked **⚠ coordinate**. They should land after the owning agent merges.

### Wave 0 — foundation (P0). Parallel: 0.1, 0.2, 0.4, 0.5, 0.6, 0.7 and 0.8 in parallel; 0.3 starts at once but its execute half depends on 0.1.

| WP | Title | Owns | Depends | Deliverables | Exit test (Lavapipe) |
|---|---|---|---|---|---|
| **WP-0.1** | Device 1.3 plus tier detection | `src/vk/device.cpp`, `src/vk/instance.cpp`, `include/fuse/renderer/vk/{device,instance}.hpp`, new `include/fuse/renderer/vk/render_tier.hpp` + `src/vk/render_tier.cpp`, new `tests/test_rp_device_tiers.cpp`, new `tests/vk_layer_mask_features.cpp` | – | Instance and device 1.3. `Vulkan13Features` chain (sync2, dynamicRendering, maintenance4). Enable `drawIndirectCount`, `shaderInt64`, int64 atomics, `shaderDrawParameters`, `multiDrawIndirect`. Optional probe and enable for mesh, AS, ray query, RT pipeline, descriptor buffer, DGC, shader object, coop matrix. `RenderTier` T0–T3 plus `RendererCaps` (per-feature bools and fallback names). `FUSE_RENDER_TIER_MAX` env override. | On LVP reports T2 (RT pipeline yes, no tensor, so not T3). With a masking layer or env override it reports T0 and T1. Zero VUIDs. Stub backend still builds. |
| **WP-0.2** | volk loader | new `Engine/lib/volk/**` (pinned plus `VERSION`), new `cmake/FuseVolk.cmake`; ⚠ coordinate `cmake/FuseFindVulkan.cmake`, `src/vk/allocator.cpp` (VMA dynamic functions) | 0.1 merged | volk init in instance and device; `fuse_lint_vendored_pins_volk` | Full Vulkan suite green with volk; `nm` shows no direct `vk*` imports except `vkGetInstanceProcAddr` |
| **WP-0.3** | **Render graph v2** | `include/fuse/renderer/render_graph.hpp`, `src/render_graph.cpp`, `src/command_buffer.cpp` + `include/.../command_buffer.hpp`, new `include/fuse/renderer/rg/**` + `src/rg/**` (sync2 barrier builder, transient allocator, queue scheduler), tests `test_render_graph*.cpp`, new `tests/test_rp_rg_*.cpp` | 0.1 (sync2) | Per-pass `VkDependencyInfo` batches from declared access (stage and access masks, subresource ranges, buffer ranges). Resource table mapping `RGTextureRef` and `RGBufferRef` to real `VkImage`/`VkBuffer`. Transient heap with VMA aliasing from existing lifetimes. Queue classes (graphics, async compute, transfer) with timeline semaphores and queue-family ownership transfer. Debug labels per pass. Raise or remove the 32-pass cap (fixed-capacity pools keep B2.11's zero-alloc rule). Keep the v1 API as a facade. Migrate `gpu_radix_sort` to RG passes as the proof. | (a) Sync-validation clean on a 12-pass synthetic graph with RAW, WAR and WAW hazards on images and buffers. (b) **"No manual barrier" gate**: a lint ctest greps `src/**` for `vkCmdPipelineBarrier` outside `src/rg/` and `upload_queue.cpp` (allow-list). (c) Aliasing: two non-overlapping transients share memory (VMA stats) and readbacks stay correct. (d) Async path through the spoof layer runs clean under sync validation. |
| **WP-0.4** | Bindless registry hardening | `src/vk/bindless.cpp`, `include/.../vk/bindless.hpp`, new `shaders/common/bindless.glsl` (+ `.slang` later), `tests/test_bindless_descriptors.cpp` | 0.1 | Documented 32-bit handle packing (index plus type bits) shared by C++ and shaders; BDA buffer table; sampler heap | Shader indexes 10k textures and a BDA buffer; readback matches |
| WP-0.4b | Descriptor-buffer backend (optional) | new `src/vk/bindless_descriptor_buffer.cpp` | 0.4 | `VK_EXT_descriptor_buffer` path selected by caps | Same test on both backends |
| **WP-0.5** | Slang toolchain plus pipeline cache v2 | `src/shader/**`, `include/.../shader/**`, new `cmake/FuseSlang.cmake`, new `Engine/lib/slang/VERSION` (download pin only), `src/vk/pipeline_cache.cpp`, `src/vk/{graphics,compute}_pipeline.cpp`, `src/vk/pipeline_layout.cpp`, `tests/test_{shader_pipeline,pipeline_cache,shader_watch}.cpp` | 0.1 | `slangc` fetched by hash (Linux and Windows). `ShaderCompiler` front end dispatching on `.slang`/`.glsl`. Reflection to pipeline layout. Permutation key (defines plus tier). Background compile on `JobScheduler`. Graphics pipeline library fast-link when available. Hot reload for Slang modules. | A `.slang` compute shader compiles, passes `spirv-val`, runs on LVP and matches the GLSL twin bit-exactly. Hot reload of a Slang include triggers recompilation. Warm-start cache hit rate is 100% on the second run. |
| WP-0.5b | Shader-object path (optional) | new `src/vk/shader_object.cpp` | 0.5 | `VK_EXT_shader_object` for permutation-heavy passes | Same output as pipelines |
| **WP-0.6** | Profiling and capture | new `Engine/lib/tracy/**` (pinned, optional), new `Core/src/profiler/tracy_adapter.cpp` (⚠ coordinate with Core owner), new `Renderer/src/vk/gpu_profiler.cpp` + header, new `src/vk/renderdoc_capture.cpp` + header | 0.3 for per-pass zones (can stub) | `FUSE_WITH_TRACY` (default OFF). GPU zones from timestamp queries per RG pass. RenderDoc in-app API load (dlopen) plus `--capture-frame N`. | GPU zone count equals executed-pass count; Tracy build compiles on Linux and MinGW; RenderDoc hook is a no-op without the library |
| **WP-0.7** | Renderer test harness and goldens | new `Source/FUSE/Renderer/tests/harness/**` (headless frame runner, readback-to-EXR/PNG, scene builders), new `Tests/golden/renderer/**`, new `Renderer/cmake/rp_harness.cmake` | image-metrics agent (**In progress**) | Deterministic mini scenes (Cornell box, sphere field, thin-wall, 1k to 100k instance grid, HUD overlay). Golden comparison through FLIP and SSIM once metrics land (PSNR fallback until then). ctest labels `renderer;golden`, `tier_t0`/`t1`/`t2`. Sync validation env on by default for these labels. | Golden round trip on the existing G-buffer raster path; deliberate one-pixel change fails the gate |
| **WP-0.8** | Pure-logic cores for PRISM | new `Source/FUSE/Renderer/core_logic/**` (no Vulkan includes): RG compiler model, page-table and page-pool allocator, residency LRU | – (0.3, 3.1 and 5.3 consume it) | Header-light C++ with bounded loops and no heap in hot paths; ASan, UBSan and coverage targets; harness entry points for a BMC tool | 100% branch coverage; ASan and UBSan clean; property tests (no double-map, free-list conservation) |

### Wave 1 — GPU-driven visibility (P1). Needs 0.1 and 0.3 (and 0.5 for Slang, else GLSL).

| WP | Title | Owns | Depends | Deliverables | Exit test |
|---|---|---|---|---|---|
| **WP-1.1** | GPU scene | new `include/fuse/renderer/gpu_scene/**`, `src/gpu_scene/**`, `tests/test_rp_gpu_scene*.cpp` | 0.3, 0.4 | Persistent BDA buffers (instances, prev/cur transforms, meshes, materials, lights) with dirty-range delta upload through `UploadQueue`. ECS extraction adapter (read-only on ECS). | Delta upload bytes scale with changed instances, not total; GPU readback equals CPU mirror |
| **WP-1.2** | Meshlet cook | new `Engine/lib/meshoptimizer/**` (pinned), new `Source/FUSE/Renderer/geometry/**` (meshlet builder, cooked format), cook hook ⚠ coordinate `Hybrid/cooked_asset_bindings.*` | – | Offline meshlets (64 verts, 124 tris), cone and sphere bounds, vertex compression, versioned binary format | Round trip; every triangle is in exactly one meshlet; bounds contain vertices |
| **WP-1.3** | Instance cull plus Hi-Z plus indirect-count | new `src/culling/**`, `shaders/culling/**`, `tests/test_rp_culling*.cpp` | 1.1 | SPD-style Hi-Z build; phase-1 (last-frame Hi-Z) and phase-2 (new Hi-Z, disoccluded) cull; `vkCmdDrawIndexedIndirectCount` args | Visible set equals CPU brute-force reference; camera-cut test (teleport) shows 0 missing objects after phase 2; CPU record time flat from 1k to 100k (±10%) |
| **WP-1.4** | Visibility buffer | new `src/visbuffer/**`, `shaders/visbuffer/**` | 1.3 | Raster to `R32G32_UINT` plus depth (T0 default) and 64-bit atomic path (shared with SW raster). Dynamic rendering. | Instance and triangle IDs decode to the expected triangle for sampled pixels |
| **WP-1.5** | Material resolve | new `src/material_resolve/**`, `shaders/material_resolve/**` | 1.4 | Barycentric attribute reconstruction; material-tile classification; indirect dispatch per material; writes the existing G-buffer layout | FLIP against the legacy G-buffer raster on Sponza-mini ≤ threshold; `test_deferred_pipeline` still green |

### Wave 2 — lighting, temporal, geometry (P2, P4, P5 run in parallel after Wave 1)

| WP | Title | Owns | Depends | Deliverables | Exit test |
|---|---|---|---|---|---|
| **WP-2.1** | GPU clustered assignment plus shading | new `src/lighting/gpu/**`, `shaders/lighting/**`, `tests/test_rp_clustered_gpu.cpp` (do not edit the in-progress `clustered_*` files; read the kernel as the oracle) | 1.1, clustered kernel agent | Compute assignment to GPU buffers; deferred shade compute; spot shading | Light lists identical to `ClusteredLightCuller` CPU reference; 4,096-light scene parity with the CPU shade kernel within tolerance |
| **WP-2.2** | Light types plus BRDF energy compensation | `material/brdf.*`, `shaders/common/brdf.glsl`, new `src/lighting/ltc/**` (+ LUT data) | 2.1 | LTC area lights, directional, multi-scatter compensation, DFG LUT bake | White-furnace test: albedo 1, any roughness returns 1 ± 1%; LTC against a Monte Carlo reference |
| **WP-2.3** | Forward transparency | new `src/forward/**`, `shaders/forward/**` | 2.1 | Forward pass reading cluster lists | Transparent sphere lit by 3 lights matches the deferred result on the opaque twin |
| **WP-4.1** | GPU motion vectors plus TAAU shader | new `shaders/temporal/**`, new `src/temporal/**` (the TAAU **kernel** belongs to the TAAU agent; this is the Vulkan dispatch) | 1.1, TAAU and UpscaleInputs agents | Camera plus per-object MVs from prev transforms; skinned MVs; TAAU compute matching the CPU kernel | Analytic reprojection error < 1e-3 px on static geometry (research §5 P1); TAAU LVP output equals the CPU kernel within tolerance |
| **WP-4.2** | FSR 3.1 temporal backend | extend `Engine/lib/fidelityfx` pin (**after** the FSR1 agent merges), new `src/upscale_backends/fsr3/**` | IUpscaler agent | FSR 3.1 Vulkan backend behind IUpscaler; FSR 3.1 FG optional | Runtime switch NativeTaau ↔ FSR1 ↔ NIS ↔ FSR3 for 60 frames with no validation errors and no NaNs; FLIP against the 16-spp reference |
| WP-4.3 | XeSS plugin | new `Renderer/plugins/intel_xess/**` | IUpscaler agent | Runtime-loaded binary, mock provider for CI | Mock-provider gate (CPU); HW manual |
| WP-4.4 | Frame generation plus latency | new `src/present/latency/**` | 4.2, NV plugin agent | `ILatencyProvider` (none, Reflex via plugin, Anti-Lag 2, `VK_NV_low_latency2`); FSR3 FG path; present-timing model | CPU timeline simulator gate (research §5 latency accounting) |
| **WP-4.5** | GPU post stack | new `shaders/post/**`, new `src/postprocess/gpu/**` (Look agent owns `look/**`; coordinate the node API) | 0.3, Look agent | Compute shaders for exposure histogram, bloom, DoF, motion blur, tonemap (**add AgX**), LUT apply; CPU `postprocess/*` becomes the oracle | Each pass equals its CPU reference within 1/1024; "no-op Look is bit-identical to PostStack" |
| **WP-5.1** | Mesh-shader path (T1) | new `src/meshlet/**`, `shaders/meshlet/**` | 1.2, 1.3, 1.4 | Task/mesh pipeline: frustum, cone and Hi-Z per meshlet; writes the VB; T0 fallback switch | VB IDs identical to the 1.4 indirect path on the same scene; masked-T0 run uses the fallback |
| **WP-5.2** | Cluster DAG plus LOD | `Source/FUSE/Renderer/geometry/dag/**` (sub-directory of 1.2's tree, handed off) | 1.2 | meshoptimizer simplification, group partition, error metric, cooked DAG | **Crack test**: shared group boundaries are vertex-identical across LOD cuts; monotonic error |
| **WP-5.3** | Cluster streaming and residency | new `src/geometry_streaming/**`, logic in `core_logic/residency/**` (from 0.8) | 5.2, 0.8 | Page file, prioritised requests, coarse-LOD guarantee, feedback buffer | Scripted fly-through: 0 frames with missing coarse LOD; residency never exceeds budget |
| **WP-5.4** | Compute software rasteriser | new `shaders/swraster/**`, `src/swraster/**` | 1.4 | 64-bit atomic depth plus ID writes for micro-triangle clusters | SW and HW raster of the same cluster set agree on ≥ 99.9% of pixels (edge rules documented) |

### Wave 3 — shadows, volumetrics, RT infrastructure (P3 and P8 after P2; RT infra can start after Wave 1)

| WP | Title | Owns | Depends | Deliverables | Exit test |
|---|---|---|---|---|---|
| **WP-3.1** | VSM core | new `src/shadow/vsm/**`, `include/.../shadow/vsm/**`, logic in `core_logic/vsm_pages/**` | 2.1, 0.8, 1.3 | Clipmap levels (16k² virtual), page marking from depth, page table, physical pool, static caching with invalidation from GPU-scene bounds | Page-table model property tests; marked pages equal the CPU reference |
| **WP-3.2** | VSM raster, filtering, local lights | new `shaders/shadow_vsm/**` | 3.1, 1.4 | Page rendering (indirect or meshlet), PCF and PCSS, spot single-page, point cube pages | **Seam test** ported from `fuse_b5_shadows_gates` (0 mismatches across clipmap boundaries); cached frame renders 0 static pages when nothing moves |
| **WP-8.1** | Froxel fog GPU | new `shaders/volumetric/**`, `src/volumetric/gpu/**` | 2.1, 3.2 (shadow lookup), 6.1 (GI, optional) | Inject, reproject and integrate; clustered lights | Static camera: fog converges and temporal variance falls below ε; moving camera: ghosting metric below threshold |
| **WP-8.2** | Hillaire atmosphere | `src/atmosphere/**`, `include/.../atmosphere/**`, new `shaders/atmosphere/**` | 0.3 | Transmittance, multi-scatter, sky-view LUTs; aerial perspective; bilinear sampling | Against the existing brute-force single-scatter reference (±6%) plus the multi-scatter energy check |
| WP-8.3 | Volumetric clouds | new `src/clouds/**`, `shaders/clouds/**` | 8.2 | Raymarched, temporally amortised | Convergence and ghosting metrics |
| **WP-6.0** | Acceleration structures | new `src/rt/**`, `include/.../rt/**` | 1.1, 0.1 | BLAS per mesh, TLAS from GPU scene, compaction, refit; T2 caps gate | TLAS hit IDs from a ray-query compute probe equal the CPU BVH reference on a tiny scene (LVP) |

### Wave 4 — GI (P6). Needs P2, P3 and P4 (plan §7).

| WP | Title | Owns | Depends | Deliverables | Exit test |
|---|---|---|---|---|---|
| **WP-6.1** | DDGI Vulkan (T0 SDF and T2 ray query) | new `src/gi/gpu/**`, `shaders/ddgi/**` (the DDGI kernel files belong to the kernel agent and are the oracle) | 6.0 (T2 path), DDGI kernel agent, global SDF (Compute/Scene agents) | Probe trace (SDF on T0, ray query on T2), irradiance and depth-moment blend, relocation, inside/backface/inactive classification, Chebyshev visibility in shading | GPU atlas against the CPU DDGI within tolerance; **thin-wall leak test** (light on one side, luminance on the other below ε); time-of-day sweep with temporal-stability metric |
| **WP-6.2** | RT shadows plus reflections (T2) | new `shaders/rt_effects/**`, `src/rt_effects/**` | 6.0, 4.1 | Ray-query area-light soft shadows, reflections, hit distance for the denoiser | Converged (many-frame) result matches an offline reference |
| **WP-6.3** | Screen-space fallback on Vulkan | new `shaders/ssfx/**`, `src/ssfx_gpu/**` (the SSFX kernels in `ScreenSpace/` belong to the kernel agent) | 0.3, SSFX kernel agent | SSR, GTAO (new; HBAO stays as oracle), SSGI dispatch | Parity with the CPU kernels |
| **WP-6.4** | In-tree denoiser | new `src/denoise/**`, `shaders/denoise/**` | 4.1 | SVGF/A-SVGF (or FidelityFX Denoiser, MIT) for shadows, reflections and GI | Variance reduction factor and bias against the converged reference |
| WP-6.4b | NRD plugin | new `Renderer/plugins/nvidia_nrd/**` (MIT adapter; NRD binary never committed) | 6.4, NV plugin ABI | SIGMA, REBLUR and RELAX behind the denoiser interface | Mock provider on CI; HW manual |
| WP-6.5 | Radiance cascades (research) | new `research/radiance_cascades/**` | 6.1 | Prototype plus report | Metrics only |

### Wave 5 — ReSTIR and PT (P7), then P9

| WP | Title | Owns | Depends | Deliverables | Exit test |
|---|---|---|---|---|---|
| WP-7.1 | Light tree | new `src/light_tree/**` | 1.1 | CPU build plus GPU traversal | PDF integrates to 1; sampling against brute force |
| WP-7.2 | ReSTIR DI and GI | new `src/restir/**`, `shaders/restir/**` | 7.1, 6.0, 6.4 | Reservoirs, temporal and spatial reuse, unbiased mode | Tiny scene with 10k emissive triangles: mean equals the reference within confidence bounds; variance below the RIS-only baseline |
| WP-7.3 | Path-tracing mode (T3 pipeline) | new `src/pathtrace/**`, `shaders/pathtrace/**` | 7.2, 0.1 (RT pipeline) | SBT, PT integrator; RR/NRD via plugins | 4k-spp convergence against the reference (LVP, very small resolution) |
| WP-9.1 | Neural (coop matrix and vector) | new `src/neural/**` | 7.x | CPU reference MLP first; GPU on HW | CPU parity only here |
| WP-9.2 | 3D Gaussian splatting | new `src/gsplat/**`, `shaders/gsplat/**` | 1.4, `gpu_radix_sort` | Sort (existing radix sort), splat raster, depth composite with VB | Splat scene against the reference image (FLIP) |
| WP-9.3 | Device-generated commands | new `src/vk/dgc/**` | 1.3, 0.5 | `VK_EXT_device_generated_commands` pipeline selection | Same image as the indirect-count path; CPU command count drops |

---

## 6. Recommended execution order

```
Wave 0  (parallel)  WP-0.1 → {0.2, 0.3, 0.4, 0.5, 0.6}   +   0.7, 0.8 independent
        gate: RG v2 sync-validation clean + "no manual barrier" lint + Slang twin test   → Phase 0 closed
Wave 1              WP-1.1 ∥ 1.2 → 1.3 → 1.4 → 1.5
        gate: VB+resolve FLIP vs legacy G-buffer; CPU record flat 1k→100k               → M1 "Renders"
Wave 2  (parallel)  P2: 2.1 → 2.2, 2.3      P4: 4.1, 4.5 (then 4.2, 4.3, 4.4)      P5: 5.1, 5.2 → 5.3, 5.4
        + WP-6.0 (AS) can start here (only needs 1.1)
Wave 3              P3: 3.1 → 3.2      P8: 8.2 → 8.1 → 8.3
        gate: VSM seam + cache test; switchable upscalers                                → M2 "Lit" (first usable)
Wave 4              P6: 6.1, 6.3 ∥ 6.2 → 6.4 (6.4b, 6.5 optional)                        → M4 "Global" (with P8)
Wave 5              P7: 7.1 → 7.2 → 7.3;   P9: 9.2, 9.3 (LVP), 9.1 (HW)                   → M5 "Traced"
```

Rules:
- **Freeze the RG v2 API only after WP-1.3, 1.4 and 2.1 have used it** (plan §9, risk 1). Until then, WP-0.3's owner reviews every RG-facing change.
- **Do not retire legacy paths** (G-buffer raster, CSM, CPU post) until the replacement's golden test is green. They are the fallbacks and the oracles.
- **Every CPU or CUDA kernel port now in progress becomes a parity oracle** for the Vulkan shader that replaces it on T0–T2.
- Hardware-only exits (budgets in §5.3, DLSS, XeSS, NRD, coop matrix, FG and latency) are tracked in a separate `HW` checklist per phase and run nightly or manually on a real T2 device. The plan's §8 tier matrix needs **at least one real T2 GPU runner**. Lavapipe does not replace it for performance or vendor quirks.

---

## 7. Exit-criteria tests runnable on Lavapipe

All of these are deterministic, headless and small (≤ 256×256 unless stated). They carry ctest labels `vulkan;renderer`, run with `VK_LAYER_KHRONOS_validation` plus sync validation, and skip (not fail) when a capability is missing.

| Phase | Test idea | Measures |
|---|---|---|
| P0 | `rp_rg_hazard_matrix`: generated graphs (random DAG, 4–24 passes, image and buffer RAW/WAR/WAW, mip and layer subranges) | 0 sync-validation messages; barrier count ≤ an optimal bound; readback equals a CPU simulation |
| P0 | `rp_rg_no_manual_barrier_lint` | No `vkCmdPipelineBarrier*` outside the allow-list |
| P0 | `rp_rg_transient_alias` | Aliased memory (VMA stats) plus correct readback; alias disabled produces the same image |
| P0 | `rp_rg_async_spoof` | Compute pass on a spoofed compute family; ownership-transfer VUIDs 0; result equals single-queue |
| P0 | `rp_device_tiers` | LVP gives T2; masked gives T0 and T1; caps JSON dump stable |
| P0 | `rp_slang_twin` | Slang and GLSL compute produce bit-identical buffers |
| P1 | `rp_cull_reference` | GPU visible set equals the CPU frustum and occlusion reference for 10k instances |
| P1 | `rp_cull_camera_cut` | Teleport camera; after phase 2, 0 visible objects missing (compared with no-occlusion) |
| P1 | `rp_submit_flat` | CPU record µs at 1k, 10k and 100k instances within ±10% (CPU-only, host-stable) |
| P1 | `rp_vb_resolve_golden` | VB plus resolve against legacy G-buffer, FLIP mean ≤ 0.01 |
| P2 | `rp_clustered_parity_4096` | GPU light lists equal the CPU culler; shaded image against the CPU kernel within 1e-3 |
| P2 | `rp_brdf_white_furnace` | Energy 1 ± 1% across roughness |
| P3 | `rp_vsm_seams` | 0 depth-test mismatches across clipmap level boundaries against a brute-force 16k reference in a tiny scene |
| P3 | `rp_vsm_cache` | Static scene frame 2: 0 pages rendered; moving one object invalidates only overlapping pages |
| P4 | `rp_mv_analytic` | Reprojection error < 1e-3 px (camera, rigid, skinned) |
| P4 | `rp_upscaler_switch` | Cycle backends every 10 frames for 120 frames: no NaN or Inf, no validation errors, FLIP bounded after 8 frames of settle |
| P4 | `rp_post_parity` | Each GPU post pass against its CPU reference within 1/1024 |
| P5 | `rp_mesh_vs_indirect` | Mesh-shader VB IDs equal indirect VB IDs |
| P5 | `rp_dag_crack` | Boundary vertex identity across all LOD cuts on a sphere-of-spheres |
| P5 | `rp_swraster_agree` | SW against HW raster ≥ 99.9% of pixels |
| P6 | `rp_ddgi_leak` | Thin-wall luminance on the dark side below ε; probe relocation moves inside probes out |
| P6 | `rp_ddgi_tod_stability` | 60-step sun sweep: temporal flicker metric below threshold |
| P6 | `rp_rq_tlas_probe` | Ray-query hit IDs equal the CPU BVH |
| P7 | `rp_restir_unbiased` | Mean within 3σ of a 4k-spp reference; variance below RIS-only |
| P8 | `rp_fog_ghosting` | Moving-camera trail energy below threshold; static convergence |
| P8 | `rp_atmos_reference` | Sky LUT output within 6% of the brute-force integral (existing reference) |
| P9 | `rp_dgc_equivalence` | DGC image equals the indirect-count image |
| P9 | `rp_gsplat_golden` | FLIP against the reference render |

Hardware-only (not on Lavapipe): §5.3 budgets and the 5% regression gate, 4,096-light timing, VSM under 1 ms cached, 100M-triangle budget, volumetrics at 1.5 ms, DLSS SR/RR/FG, XeSS, NRD, Reflex/Anti-Lag latency, cooperative matrix and vector performance, async-compute overlap gains.

---

## 8. Open decisions

1. **Slang adoption scope.** Options: (a) all new shaders in Slang after WP-0.5, or (b) GLSL now and a Slang migration later. Recommended: (a), with glslang kept for vendored GLSL.
2. **Minimum API.** Raise to Vulkan 1.3 in WP-0.1 (the plan requires it). Confirm no user of the 1.2 path remains (Editor under Xvfb uses Lavapipe 1.4).
3. **Reference scene assets.** Download at test time with a pinned hash (licence notices), or commit reduced "mini" versions. Recommended: procedural mini scenes in CI, full Sponza and Bistro only on HW nightlies.
4. **Default denoiser.** SVGF/A-SVGF, self-written from the papers, or FidelityFX Denoiser (MIT). NRD stays a plugin.
5. **PRISM.** The tool is not in the repo. WP-0.8 prepares the code shape (pure logic, no Vulkan types) so it can plug in later.
