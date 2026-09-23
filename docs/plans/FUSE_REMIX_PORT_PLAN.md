# FUSE Remix Port Plan: FUSE Relight, a FUSE-native D3D8/D3D9 remastering runtime

Status: plan, not started. Written 2026-09-23 on branch `claude/port-execution-plan-bi45jj`.
Product name: **FUSE Relight**, the runtime half of the FUSE Remaster stack. Remaster track A2 is now fully planned in this document.

Companion documents:
- [`FUSE_REMASTER_PLAN.md`](FUSE_REMASTER_PLAN.md): POCO assets, the replacement DB and the legal/IP policy. It is being revised in parallel. This plan references its sections and does not edit it.
- [`FUSE_RENDERER_PLAN.md`](FUSE_RENDERER_PLAN.md) and [`../unification/RENDERER-EXECUTION.md`](../unification/RENDERER-EXECUTION.md): RG v2, tiers T0 to T3, Slang, bindless, the WP-x.y packages.
- [`FUSE_ASSET_PLAN.md`](FUSE_ASSET_PLAN.md): cook formats and asset gates.
- [`../compute-kernels.md`](../compute-kernels.md): single-source CPU/CUDA kernels and the CPU-first reference rule.
- [`../nvidia-plugin.md`](../nvidia-plugin.md): the runtime-plugin rule for NVIDIA proprietary SDKs.

**Upstream snapshot surveyed** (shallow clones in the session scratchpad, never copied into the repo):

| Repo | Commit / version | Date |
|---|---|---|
| `NVIDIAGameWorks/dxvk-remix` | `0867d3c748a7` (`remix-main`) | 2026-09-23 |
| `doitsujin/dxvk` | `25ca63f17f34`, version 3.1.1 | 2026-09-23 |
| `doitsujin/dxbc-spirv` | submodule of DXVK 3.1.1 | – |
| `NVIDIAGameWorks/rtx-remix` | umbrella repo (submodules only) | – |

Submodule licences were checked by fetching each LICENSE file. File paths in this document are relative to those repos unless they start with `Source/`, `Engine/`, `Tools/`, `Tests/`, `cmake/` or `docs/`, which are FUSE paths.

**The plan in one paragraph.** FUSE Relight is a drop-in `d3d9.dll` / `d3d8.dll`.
- **Front end.** It vendors **upstream DXVK 3.x** (zlib) for D3D8/D3D9 → Vulkan translation. It does not use Remix's 2021-era DXVK fork, and it does not rewrite D3D9 from scratch.
- **Tap.** A small, clearly marked patch set adds a *draw tap* and lets **FUSE own the `VkDevice`**, which DXVK imports.
- **Scene side.** On the tap, FUSE-owned code does the rest:
  - classifies draws;
  - computes **bit-exact Remix-compatible hashes**, so existing community Remix mods and captures work;
  - builds a scene;
  - applies replacements from the Remaster **replacement DB and POCO assets**, including an importer for Remix USD mods.
- **Rendering.** The scene is rendered by the **FUSE renderer**:
  - T3: a path tracer with FUSE's own ReSTIR DI/GI/PT, radiance cache and denoiser, all written from the papers;
  - T0 to T2: a raster remaster path, so it also runs on Lavapipe and non-RT GPUs.
- **32-bit games.** They go through a revamped port of the Remix bridge.
- **NVIDIA proprietary pieces** (DLSS/RR, NRD, NRC, RTXDI, RTXCR, rtxio, MDL materials) are never vendored. The DLSS/NRD paths are optional runtime plugins through the existing `plugins/nvidia` seam.
- **Testing.** Everything is testable here: MinGW-built D3D9/D3D8 test apps run under Wine + Xvfb through our DLLs, onto Lavapipe. §6.1 records the verification done during this survey.

---

## 0. Goals, non-goals, licences and attribution

### 0.1 Goals

| # | Goal | Measured by |
|---|---|---|
| G1 | **Full D3D9 and D3D8 coverage.** Any game that runs on DXVK runs on Relight with the tap on and no remaster, with identical output (passthrough). | Test-app goldens bit-identical tap-off vs tap-on (§6); DXVK's own app compatibility is inherited. |
| G2 | **Remix mod compatibility.** Existing Remix mods (`rtx-remix/mods/*/mod.usda` + `rtx.conf`) load and replace the same meshes, materials and lights as on Remix. | Hash KATs (§4.1); fixture mods; manual parity against a real Remix capture on the RTX 3090 (§6.6). |
| G3 | **Remix capture interop.** Relight writes FUSE captures (POCO + `hash_key` rows) *and* Remix-compatible USD captures, so FUSE authoring and the Remix Toolkit can be used on either side. | Round trip: capture → import → re-hash is lossless on keys. |
| G4 | **Best-in-class path tracer** on T3, grounded in the literature: ReSTIR DI (with GRIS pairwise MIS), ReSTIR GI, then ReSTIR PT, plus a light tree, radiance cache and in-tree spatiotemporal denoiser. | Unbiasedness and variance gates against CPU references (§5.8); on the 3090, quality and frame time at least match upstream Remix on the same scene and settings (§6.6). |
| G5 | **Scales down.** T0 to T2 raster remaster (clustered lights, VSM/CSM, DDGI, ray-query shadows and reflections on T2) with the same replacements. A passthrough mode exists for any Vulkan 1.3 device. | Tier-masked goldens on Lavapipe. |
| G6 | **32-bit games.** An x86 client proxies the D3D9/D3D8 API to an x64 host. | The x86 builds of all test apps produce the same goldens and captures as the x64 builds (Wine32 CI). |
| G7 | **One binary for Windows and Linux (Wine/Proton).** PE DLLs built with MinGW. | CI builds PE on Linux; the Wine runs are the gate. |
| G8 | **Licence-clean.** No NVIDIA proprietary file, no leaked code, no LGPL/GPL file in the tree. Attribution is complete. | Licence gates (RL-0.1). |

### 0.2 Non-goals

- D3D10/11/12 or OpenGL games. DXVK's `d3d10`, `d3d11` and `dxgi` front ends are **not** vendored. Remaster track B covers those games.
- Reconstructing geometry from arbitrary pixel shaders. As in Remix, shader-based draws are supported through **vertex capture** and texture hashes. Complex shader materials fall back to the raster passthrough.
- Shipping NVIDIA binaries, MDL material files, or the Remix Toolkit (Omniverse Kit).
- Anti-cheat circumvention, DRM, or injection into online multiplayer titles. The launcher refuses known anti-cheat processes.
- Using Remix's `RtxdiApplicationBridge.slangh`, `rtxcr*.slangh`, `NRC.h` or `NRD.slangh` in any form (§0.3).

### 0.3 Licence findings, per component

"Verdict" means:
- **Vendor**: copy verbatim, pinned, notices kept.
- **Port**: re-home MIT/zlib code into FUSE with its notice kept, modified.
- **Reimplement**: FUSE's own code, from papers or specs.
- **Plugin**: optional runtime-loaded module; the binary is never committed.
- **Drop**: not needed.

| Component (upstream path) | Licence (evidence) | Verdict for FUSE |
|---|---|---|
| dxvk-remix runtime code: `src/dxvk/rtx_render/**`, `src/d3d9/d3d9_rtx*`, `src/lssusd/**`, `src/usd-plugins/**`, `src/ogn/**`, `src/dxvk/imgui/dxvk_imgui*.cpp`, `rtx_user_menu.cpp`, `public/include/remix/*.h` | MIT, NVIDIA 2021–2026 (`LICENSE-MIT`, per-file headers) | **Port** selectively (§3). Keep the NVIDIA MIT notice in every ported file and add a FUSE modification line. |
| dxvk-remix shaders `src/dxvk/shaders/rtx/**` (54.6k lines) | MIT, **except the five files below** | **Port** the MIT parts: surfaces, materials, lights, geometry resolver, PSR, volumes, particles, post. |
| `shaders/rtx/algorithm/rtxdi/RtxdiApplicationBridge.slangh` (1,023 lines), `algorithm/rtxcr/rtxcr.slangh`, `rtxcr_material.slangh` (490), `external/NRC.h`, `external/NRD.slangh` | **NVIDIA proprietary** ("NVIDIA CORPORATION and its licensors retain all intellectual property…"). They sit inside the MIT repo but are not MIT. | **Never read or copy (clean room).** Reimplement ReSTIR DI/GI, SSS/hair materials, radiance cache and denoiser from papers (§5). A text-scan gate enforces this (RL-0.1). |
| `src/mdl/**` (`AperturePBR_*.mdl`, `nvidia/core_definitions.mdl`…) | **NVIDIA proprietary** MDL EULA (`src/mdl/PACKAGE-LICENSES/LICENSE.md`, file headers) | **Drop.** FUSE never compiles MDL. The Remix material *parameter names* (interface facts) are mapped from the MIT table in `rtx_material_data.h` (§4.5). |
| DXVK core in the Remix fork (`src/d3d9`, `src/dxso`, `src/dxvk` non-rtx, `src/util`) | zlib (`LICENSE`, Rebohle/Ashton) + NV-DXVK MIT modifications | **Do not use the fork** (§2.2). Vendor **upstream DXVK 3.1.1** instead. |
| Upstream DXVK 3.1.1 `src/{d3d8,d3d9,dxvk,util,wsi,spirv,vulkan}` | zlib (`doitsujin/dxvk/LICENSE`) | **Vendor** into `Engine/lib/dxvk`, marking the altered source (zlib clause 2) and keeping the notice. |
| `dxbc-spirv` (DXVK 3.x shader front end, including SM1–3 `sm3/`) | MIT (Rebohle 2025) | **Vendor** into `Engine/lib/dxbc-spirv`. |
| DXVK `include/native/directx` (mingw-directx-headers) | **LGPL-2.1+** (README of `Joshua-Ashton/mingw-directx-headers`; Wine-derived headers) | **Not vendored.** PE builds use the MinGW toolchain's system headers. Native-Linux unit builds fetch the pinned headers at configure time into `build/`, never into git. |
| Vulkan-Headers, SPIRV-Headers (submodules) | Apache-2.0 / MIT | Use FUSE's existing Vulkan headers; vendor SPIRV-Headers (MIT) if not already present. |
| `bridge/**` (formerly bridge-remix) | MIT (`bridge/LICENSE-MIT`) | **Port and revamp** (§3, RL-2.x). |
| `submodules/rtxdi` (RTXDI, `remix` branch) | **NVIDIA RTX SDKs License** (`LICENSE.txt`) | **Reimplement** ReSTIR from papers. No plugin needed. |
| `submodules/rtxcr` (RTXCR Material Library) | **NVIDIA RTX SDKs License** | **Reimplement** (Burley/Christensen SSS, Chiang hair). |
| `submodules/nrc` (Neural Radiance Cache; ships `NRC_Vulkan.dll`, `cudart64_13.dll`) | **NVIDIA RTX SDKs License** | **Reimplement** (hash-grid radiance cache now, own neural cache later). NRC may be offered as a plugin; not planned. |
| NRD (packman `rtx-remix-nrd` 4.13; `NVIDIA-RTX/NRD`) | **NVIDIA RTX SDKs License** | **Plugin** (renderer WP-6.4b). The in-tree denoiser is FUSE's own. |
| `submodules/dlss` (DLSS SR/RR/FG, NGX) | **NVIDIA RTX SDKs License** | **Plugin**, already built as `Renderer/plugins/nvidia` (`dlss_sr`, `dlss_rr`). |
| Streamline | MIT headers, proprietary runtime DLLs (`license.txt`) | Existing `Engine/lib/streamline` headers plus the plugin. Unchanged. |
| `submodules/nvapi` | MIT for headers and libs (`License.txt`, 2024) | **Drop.** Not needed with Vulkan (it was used for Reflex/driver queries). |
| Reflex, rtxio, aftermath, Nsight SDK, `ngx_sdk_dlnr` (packman) | NVIDIA proprietary | **Drop.** Reflex goes through the Streamline plugin. rtxio is replaced by the GDeflate reference below. |
| `submodules/xess` | Intel Simplified Software License (binary, no modification) | **Plugin** (renderer WP-4.3). |
| `submodules/Detours` | MIT | **Drop** for now. The launcher uses `CreateProcess` + `CREATE_SUSPENDED` + a remote LoadLibrary; add Detours only if a game needs API hooks. |
| `include/MathLib/External/sse2neon` | MIT | **Drop** (x86-64 only for now). |
| `src/util/xxHash` | BSD-2 (Yann Collet), v0.8.x | **Vendor** xxHash ≥ 0.8.0 into `Engine/lib/xxhash`. The XXH3 output has been stable since 0.8.0, which hash compatibility needs. |
| Dear ImGui, ImPlot (`src/dxvk/imgui/imgui*.cpp`, `implot*`) | MIT | **Vendor** upstream Dear ImGui + ImPlot pinned in `Engine/lib/imgui`, not Remix's copy. |
| Tracy (`src/tracy`, `bridge/src/tracy`) | BSD-3 | **Drop** here. Renderer WP-0.6 owns Tracy. |
| gli / glm (`include/gli`, `include/glm`) | MIT | **Drop.** FUSE's own DDS reader and math. |
| OpenUSD (packman `open_usd` 25.11) | Tomorrow Open Source Technology License (Apache-2.0 variant) | **Optional offline tool only** (`usd-core` wheel for flattening and CI cross-check). The runtime uses TinyUSDZ (Apache-2.0), shared with Remaster W2.2. |
| Slang (packman) | Apache-2.0 WITH LLVM-exception | Pinned binary per renderer WP-0.5. |
| GDeflate (RTX IO `.pkg` compression, `RTXIO_COMPRESSION_GDEFLATE_1_0` in `rtx_io.cpp:316`) | Reference codec `microsoft/DirectStorage/GDeflate`: Apache-2.0 | **Vendor** the CPU codec into `Engine/lib/gdeflate` for `.pkg`/`.rtxio` mod packages. |
| d3d8to9 (crosire; used by Remix packaging) | BSD-2 | **Drop.** Upstream DXVK 3.x has a native `src/d3d8` front end (5.8k lines, zlib; D8VK was merged). |
| tiny-cuda-nn (NRC research reference only) | BSD-3 | Not in the runtime. It may be used as a research oracle in `Tools/` (§5.4). |
| Remix Toolkit (`toolkit-remix`) | Apache-2.0 (Kit runtime is separate and proprietary) | Read only for the mod and USD conventions. Nothing vendored. |
| RTXGI, RTXPT, SHaRC (inside the NRC repo) | NVIDIA RTX SDKs License | Not used. FUSE's DDGI and radiance cache are its own. |

### 0.4 Attribution plan

1. **Third-party notice file.** Add `Source/FUSE/Relight/THIRD_PARTY.md` with one entry per vendored or ported upstream, giving its licence text or pointer and its pin:
   - DXVK (zlib);
   - dxbc-spirv (MIT);
   - dxvk-remix (MIT, NVIDIA);
   - xxHash (BSD-2);
   - Dear ImGui and ImPlot (MIT);
   - TinyUSDZ (Apache-2.0, notice);
   - GDeflate (Apache-2.0, NOTICE);
   - SPIRV-Headers (MIT).

   The packager (RL-6.4) generates `THIRD_PARTY_NOTICES.txt` next to the shipped DLLs from this file and each `Engine/lib/*/LICENSE*`.
2. **zlib clause 2 ("altered source versions must be plainly marked").** Mark every edit to vendored DXVK in two ways:
   - wrap the edit in `// FUSE-DXVK begin: <reason>` / `// FUSE-DXVK end`;
   - list the patch in `Engine/lib/dxvk/PATCHES.md` with its purpose and the RL work package.

   `Engine/lib/dxvk/VERSION` records the upstream tag, commit and a sha256 per file (the VMA pin convention). A lint recomputes the hashes of **unmarked** regions to prove that no unmarked edits exist.
3. **MIT notices.** Every file ported from dxvk-remix keeps the NVIDIA MIT header verbatim and adds `Modifications Copyright (c) 2026 FUSE contributors (MIT)`. Ported files carry an `// Ported from dxvk-remix <path>@0867d3c` line, so a rebase can diff against upstream.
4. **Proprietary-text scan (clean-room enforcement).** A CMake-script gate (the pattern of `plugins/nvidia/cmake/nvidia_binary_gate.cmake`) fails if a tracked file contains NVIDIA's proprietary header phrases, or the "NVIDIA RTX SDKs LICENSE" title, outside an allow-list (`docs/**`). The patterns are assembled from fragments so the gate file does not match itself. It also extends the binary gate with these patterns:
   - `NRC_*.dll`, `cudart64_*.dll`, `nvrtc*.dll`;
   - `rtxio*.dll`, `NRD.dll`, `NRI.dll`;
   - `*.mdl`;
   - `NvRemix*.exe`, `.trex/`, `remixapi.dll`.
5. **Trademarks.** "RTX", "Remix" and "NVIDIA" appear only in **nominative** use: "compatible with RTX Remix mods", and the `rtx.*` config keys and `remixapi_*` ABI symbols that interoperability needs. Product, binary and module names are FUSE-native: FUSE Relight, `fuse_relight_host.exe`, `Source/FUSE/Relight`. No NVIDIA logos, no "powered by RTX".

### 0.5 How this plan relates to the other plans

- **Remaster plan** ([`FUSE_REMASTER_PLAN.md`](FUSE_REMASTER_PLAN.md)):
  - §1.1 A2, "FUSE D3D8/9 capture proxy [SPECULATIVE]", becomes this plan, with capture *and* runtime replacement and rendering.
  - Its Wave 7 is superseded by Waves R0 to R3 here.
  - Its decision D2 (Remix stays external) still holds for track A1, where users keep running NVIDIA's Remix.
  - Relight writes into the same replacement DB (§1.3 there) and POCO store (§2 there). It adds `hash_key` algorithms (§4.1 here) and one POCO extension record (§4.4).
- **Renderer plan.** Relight is the renderer's first large client:
  - It consumes RG v2 (WP-0.3), bindless (WP-0.4), Slang (WP-0.5), the GPU scene (WP-1.1), acceleration structures (WP-6.0), the denoiser (WP-6.4), the light tree (WP-7.1), ReSTIR (WP-7.2) and path tracing (WP-7.3).
  - §5 gives the **Relight specification** that WP-6.4 and WP-7.x must meet. If those packages are unclaimed when Relight reaches them, Relight agents execute them under their original WP ids and owned paths.
- **Asset plan.** Imported mod textures and meshes go through the same cook and gates when cooked for track B. At runtime Relight streams DDS/BCn directly (§4.6).

---

## 1. Upstream architecture map (survey)

### 1.1 Size map (lines of C++/Slang/HLSL/Python, `wc -l`, dxvk-remix @0867d3c)

| Area | Path | Lines |
|---|---|---|
| D3D9 front end (DXVK fork) | `src/d3d9` | 28,223 (of which Remix hooks: `d3d9_rtx.cpp` 1,294, `d3d9_rtx.h` 294, `d3d9_rtx_geometry.cpp` 288, `d3d9_rtx_utils.cpp` 246) |
| SM1–3 → SPIR-V (fork) | `src/dxso` | 7,303 (vertex capture injected in `dxso_compiler.cpp:483+`) |
| DXVK backend core (non-RTX) | `src/dxvk/*.cpp,*.h` | 37,874 |
| Remix runtime | `src/dxvk/rtx_render` | 97,336 (breakdown below) |
| Remix shaders | `src/dxvk/shaders/rtx` | 54,592: `pass/` 22,680, `algorithm/` 13,345, `concept/` 10,542, `utility/` 6,119, `external/` 2,347 |
| ImGui + dev menu | `src/dxvk/imgui` | 73,177 (about 62k is vendored Dear ImGui/ImPlot. Remix UI: `dxvk_imgui.cpp` 4,590, `rtx_user_menu.cpp` 687, about/splash/capture/first-use ≈ 1k) |
| USD export helpers | `src/lssusd` | 3,907 (`game_exporter.cpp`, `usd_mesh_importer.cpp`, path/prefix constants) |
| Utilities | `src/util` | 21,351 (includes xxHash) |
| Bridge | `bridge/src/{client,server,util,launcher}` | 13,845 / 4,132 / 9,382 / 275 (+ 24k vendored Tracy) |
| Public API | `public/include/remix/{remix.h,remix_c.h}`, `public/include/remixapi/bridge_remix_api.h` | 2,205 (API 0.6) |
| Tests | `tests/rtx` (unit + API apps), `tests/d3d9` | 15,409 / 15,247 |
| Options | `RtxOptions.md` | about 1,000 `rtx.*` options (346 in `rtx.`, 49 `rtx.volumetrics`, 40 `rtx.restirGI`, 37 `rtx.neuralRadianceCache`…) |

`rtx_render` by subsystem (file groups summed):

| Subsystem | Main files | Lines |
|---|---|---|
| Scene capture and management | `rtx_scene_manager` (2,743+453), `rtx_instance_manager` (2,429+506), `rtx_draw_call_tracker`, `rtx_draw_call_cache`, `rtx_types` (1,093+610), `rtx_camera(_manager)`, `rtx_geometry_utils`, `rtx_hashing`, `rtx_point_instancer_system` | 13,021 |
| Acceleration structures, OMM, bindless | `rtx_accel_manager` (2,122), `rtx_opacity_micromap_manager` (2,596+635), `rtx_bindless_resource_manager` | 5,990 |
| Materials and textures | `rtx_materials.h` (2,102), `rtx_material_data.h`, `rtx_texture_manager` (1,647) | 5,077 |
| Lights and portals | `rtx_lights`, `rtx_light_manager`, `rtx_lights_data`, `rtx_ray_portal_manager` | 5,499 |
| Mods and replacement | `rtx_mod_usd.cpp` (2,567), `rtx_mod_manager`, `rtx_asset_replacer`, `rtx_asset_data_manager`, `rtx_asset_package.h`, `rtx_io`, `rtx_file_watch` | 6,526 |
| Capture (USD export) | `rtx_game_capturer*`, `rtx_asset_exporter` | 2,105 |
| Options system | `rtx_option*`, `rtx_options.*` | 9,153 |
| Logic graphs | `rtx_render/graph/**` (≈ 60 components) | 10,792 |
| C API | `rtx_remix_api.cpp` (1,801), `rtx_remix_pnext.h`, `rtx_remix_specialization.inl` | 2,113 |
| Path tracer host side | `rtx_context.cpp` (3,063), `rtx_pathtracer_*`, `rtx_rtxdi_rayquery`, `rtx_restir_gi_rayquery`, `rtx_nee_cache`, `rtx_resources`, `rtx_shader_manager`, `rtx_global_volumetrics`, `rtx_composite`, `rtx_demodulate`, `rtx_accumulation`, `rtx_sparse_rendering` | 13,324 |
| NVIDIA SDK hosts (all proprietary-SDK glue) | `rtx_dlss*`, `rtx_ngx_wrapper`, `rtx_ray_reconstruction`, `rtx_nrd_*`, `rtx_neural_radiance_cache`, `rtx_nrc_context`, `rtx_dlfg`, `rtx_reflex`, `rtx_nis`, `rtx_xess`, `rtx_nsight_capture`, `rtx_gpu_crash*` | 12,617 |
| Post | tone mapping, local tone mapping, bloom, postFx, auto exposure, TAA, dither, debug view | 4,697 |
| Particles and terrain | `rtx_particle_system`, `rtx_dust_particles`, `rtx_terrain_baker` | 3,326 |
| In-runtime GUI helpers | `rtx_imgui`, `rtx_gui_widgets`, `rtx_overlay_window`, object picking | 2,731 |

### 1.2 D3D9 front end

- Remix is a **fork of DXVK from the 1.x era**. It still has `src/d3d10`, `src/dxso`, `dxvk_state_cache`, and fixed-function SPIR-V generation in `d3d9_fixed_function.cpp` (2,903 lines).
- Its NV edits are marked `// NV-DXVK start/end`. There are **539 marked blocks across about 94 core files**, outside `rtx_render`:
  - `src/d3d9`: 165 blocks in 25 files. The largest are `d3d9_device.cpp` (41) and `d3d9_swapchain.cpp` (41).
  - `src/dxvk`: 300 blocks in 50 files.
  - `src/util`: 51 blocks.
  - `src/dxso`: 23 blocks.
- `D3D9Rtx` (`d3d9_rtx.h/.cpp`) is owned by `D3D9DeviceEx`. On every draw it runs `PrepareDrawGeometryForRT` → `internalPrepareDraw`:
  1. classifies the draw (`makeDrawCallType`, `d3d9_rtx.cpp:406`);
  2. rebases and copies the indices (`processIndexBuffer`, with memoization per IBO);
  3. slices the vertex streams by declaration usage (`processVertices`);
  4. launches async hashing and AABB jobs (`computeHash`, `computeAxisAlignedBoundingBox` in `d3d9_rtx_geometry.cpp`);
  5. collects the fixed-function state (`setLegacyMaterialState`, `setFogState` in `d3d9_rtx_utils.cpp`; transforms, textures and samplers in `processRenderState`/`processTextures`);
  6. runs vertex capture for programmable VS;
  7. sends a `DrawCallState` over the CS thread to `RtxContext`.
- At the **injection point**, the first UI draw (`triggerInjectRTX`), Remix path traces the scene and writes it into the back buffer. Later draws are rasterized on top.
- **Upstream DXVK 3.1.1 has moved a long way:**
  - SM1–3 now goes through **dxbc-spirv** (`d3d9_shader.cpp` includes `sm3/sm3_converter.h`);
  - fixed function is a GLSL **uber-shader** (`d3d9_fixed_function.cpp` is now 149 lines plus `shaders/d3d9_fixed_function_{vert,frag}`);
  - there is a native `src/d3d8`;
  - it offers **instance and device import** (`DxvkInstanceImportInfo` in `dxvk_instance.h:17`, `DxvkDeviceImportInfo` + `DxvkAdapter::importDevice` in `dxvk_adapter.h:98/244`);
  - it has a D3D9 interop interface (`ID3D9VkInteropDevice::GetVulkanHandles/LockSubmissionQueue`, `d3d9_interfaces.h:130`);
  - it builds for Linux natively (`src/wsi/{sdl2,sdl3,glfw}`).

### 1.3 D3D8

Remix itself has no D3D8 code. Its releases pair it with crosire's `d3d8to9`, and `rtx.preTransformedVerticesIsUI` mentions D3D8. Upstream DXVK 3.x ships `src/d3d8` (5,815 lines), which wraps its own `d3d9` in-process. So **D3D8 support comes for free** once upstream DXVK is vendored, and the tap sees D3D8 draws as D3D9 draws.

### 1.4 Bridge (32-bit client ↔ 64-bit server)

- **Client.** `bridge/src/client` is an x86 `d3d9.dll` that implements the whole D3D9 COM surface: `d3d9_device.cpp` 3,929, plus textures, surfaces, volumes, state blocks, swap chain and shaders. It also hooks input (`di_hook.cpp` 1,315) and windows (`window.cpp`). It serializes every call.
- **Server.** `bridge/src/server/main.cpp` (3,626) is an x64 process. It loads the x64 Remix `d3d9.dll` from `.trex/` and replays the calls.
- **IPC** (`bridge/src/util`):
  - named shared memory with circular command queues (`util_blockingcircularqueue.h`, `util_atomiccircularqueue.h`) and named semaphores;
  - a shared heap for resource data (`util_sharedheap.cpp`);
  - 370 command ids in `util_commands.h` (`D3D9Command`: device and resource calls plus `RemixApi_*`, `Bridge_SharedHeap_*`).
- `NvRemixLauncher32.exe` injects the client into games that do not load a local `d3d9.dll`.
- It is built with MSVC only; the x86 client comes from a separate solution.

### 1.5 Scene capture: classification, geometry, hashing, materials, instances

- **Draw classification** (`makeDrawCallType`, in order):
  1. the draw-call range;
  2. programmable VS without vertex capture → ignore;
  3. zero primitives or an unsupported topology → ignore (points and lines are unsupported);
  4. alpha-test and alpha-blend enable options;
  5. occlusion query active → raster;
  6. no colour RT, or RGB write disabled → ignore;
  7. **shadow-mask detection**: a non-textured flood fill into a small square RT with an incompatible format;
  8. raytraced render targets (`rtx.raytracedRenderTargetTextures`, keyed by *descriptor hash*);
  9. non-primary RT → raster;
  10. **stencil shadow volumes**: stencil ALWAYS, INCR/DECR on z-fail, no z-write → ignore;
  11. **UI**: orthographic projection (`proj[3][3] == 1`) and no z-write, or a UI texture bound; this triggers injection;
  12. `POSITIONT` vertices → UI or raster.
- **Categories.** `DrawCallState::setupCategoriesForTexture` (`rtx_types.cpp:375`) maps the colour-texture hash to 25 `InstanceCategories` through `rtx.*Textures` hash lists: WorldUI, WorldMatte, Sky, Ignore, IgnoreLights, IgnoreAntiCulling, IgnoreMotionBlur, IgnoreOpacityMicromap, IgnoreAlphaChannel, Hidden, Particle, Beam, four decal classes, AlphaBlendToCutout, Terrain, AnimatedWater, ThirdPersonPlayerModel/Body, IgnoreBakedLighting, ParticleEmitter, SmoothNormals, HairCards.
- **Sky** has three sources:
  - explicit: `skyMinZThreshold`, sky texture or geometry lists, or `skyDrawcallIdThreshold` for untextured draws;
  - auto-detected: camera-position clustering in `checkSkyAutoDetect`.
  Terrain is baked when the texture is in `terrainTextures`.
- **Geometry.** Indices are copied with `minIndex` subtracted, `vertexCount = max − min + 1`, and the vertex base moves by `minIndex`. Streams are taken by D3D declaration usage:
  - POSITION/POSITIONT[0];
  - NORMAL[0];
  - TEXCOORD[`m_texcoordIndex`];
  - COLOR[0], unless baked lighting is ignored;
  - BLENDWEIGHT and BLENDINDICES[0].
- **Hashing** (normative spec in §4.1) lives in `rtx_hashing.{h,cpp}` and `d3d9_rtx_geometry.cpp`. Geometry has nine hash components, combined by configurable **rules** (`rtx.geometryGenerationHashRuleString`, default `positions,indices,texcoords,geometrydescriptor,vertexlayout,vertexshader`; `rtx.geometryAssetHashRuleString`, default `positions,indices,geometrydescriptor`). Textures use XXH3 over mip 0 (`d3d9_common_texture.cpp:652–690`). The material hash is the stage-0 colour-texture hash (`rtx_materials.h:1903`).
- **Fixed-function → material** (`setLegacyMaterialState`, `d3d9_rtx_utils.cpp:149`):
  - diffuse and specular colour sources (`D3DRS_*MATERIALSOURCE`, `COLORVERTEX`);
  - alpha test op and reference;
  - `TEXTUREFACTOR`;
  - blend state (with separate alpha);
  - texture stage ops;
  - `LegacyMaterialData` defaults via `rtx.legacyMaterial.*` (13 options).

  D3D lights become `RtLight`s (`rtx_light_manager.cpp:636`; the sphere-light hash is at `rtx_lights.cpp:296`). Fog becomes volumetric or fog state.
- **Instances.** `InstanceManager::processSceneObject` (`rtx_instance_manager.cpp:669`) matches a draw to last frame's instance of the same BLAS if it moved less than `rtx.uniqueObjectDistance` (300 game units). This keeps IDs stable for motion vectors, anti-culling and replacements.
- **Camera.** `rtx_camera_manager` classifies main, sky, portal and view-model cameras from the view/projection transforms, with a free cam.

### 1.6 Game setup heuristics and options

- `RTX_OPTION("rtx.cat", type, name, default, "doc")` macros declare every option (`rtx_option.h`).
- Options resolve through **sparse layers**, highest priority first (`documentation/RemixConfig.md`):
  1. quality presets;
  2. `user.conf`;
  3. dynamic layers (for example from Logic graphs);
  4. `rtx.conf`, from the game directory or the mod directory (`baseGameModPath`);
  5. defaults.
- Hash-list options (`rtx.uiTextures = 0x…, 0x…`) support `-` removal in higher layers.
- The developer menu edits layers and writes `rtx.conf`. Texture categorization ("tag this texture as UI or sky") is the main game-setup workflow.

### 1.7 Replacement and asset system

- **Discovery** (`rtx_mod_manager.cpp:109–194`). Mods are read from `<modsDir>/*/mod.{usda,usdc,usd}` (the stem `kDefaultModFileName`). The mods directory is `rtx-remix/mods/` next to the game, or a base-game mod path extracted from the command line by regex.
- **Prim layout** (`lssusd/game_exporter_common.h:45–50`, `game_exporter_paths.h:61–65`):
  - `/RootNode/meshes/mesh_<16 uppercase hex>`;
  - `/RootNode/Looks/mat_<hex>` (a material may also be keyed by the "strongest opinion" path hash when it is not named `mat_…`);
  - `/RootNode/lights/light_<hex>` (legacy `sphereLight_<hex>`);
  - `/RootNode/instances`, `/RootNode/cameras`.

  Hash names are parsed with `strtoull(…, 16)` (`rtx_mod_usd.cpp:333`) and written as uppercase, zero-padded to 16 digits (`hashToString`, `rtx_utils.h:85`).
- **Materials.** A USD `Material` has a `Shader` whose `info:mdl:sourceAsset` is `AperturePBR_Opacity.mdl`, `AperturePBR_Translucent.mdl` or `AperturePBR_Portal.mdl`. The runtime *never runs MDL*. It reads `inputs:<name>` attributes by the **MIT X-macro tables** `LIST_OPAQUE_MATERIAL_*` / `LIST_TRANSLUCENT_MATERIAL_*` in `rtx_material_data.h` (names, types, ranges, defaults: `diffuse_texture`, `normalmap_texture`, `reflectionroughness_texture`, `metallic_texture`, `emissive_mask_texture`, `height_texture`, `subsurface_*`, `blend_type`, `alpha_test_type`, `sprite_sheet_*`, `filter_mode`, `wrap_mode_u/v`…).
- **Textures.** `.dds` through gli (`rtx_asset_data_manager.cpp:70`), and packaged `.pkg`/`.rtxio` files (`AssetPackage`, magic `0xbaadd00d`, v1) with GDeflate-compressed blobs streamed by the proprietary rtxio library.
- **Meshes** are `UsdGeomMesh` (points, faceVertexIndices, `primvars:st`, normals, subsets, skinning through `usd_mesh_importer.cpp`). **Lights** are UsdLux Sphere, Rect, Disk, Cylinder and Distant, plus Remix shaping.
- **Other features:**
  - `preserveOriginalDrawCall`;
  - point instancers;
  - `RemixInstanceCategoryAPI` and `ParticleSystemAPI` schemas (`src/usd-plugins`);
  - OmniGraph Logic graphs (`graph/rtx_graph_usd_parser.cpp`);
  - hot reload through USD change notices and a file watcher (`rtx_mod_usd.cpp:440–560`).

### 1.8 Path tracer (frame pipeline in `RtxContext::dispatch…`, `rtx_context.cpp`)

1. **Scene preparation.** BLAS per geometry (with optional **opacity micromaps**), TLAS (primary and unordered), bindless tables, surface and material buffers.
2. **Primary pass** (`pass/gbuffer/*`: ray query or RT pipeline). G-buffer, **PSR (primary and secondary surface replacement)** for mirrors, glass and ray portals, and decals resolved with up to `primaryRayMaxInteractions` interactions.
3. **Direct lighting.** RTXDI (ReSTIR DI) initial, temporal and spatial passes (`pass/rtxdi/*`), gradients and confidence for the denoiser, then `integrate_direct`.
4. **Indirect lighting.** `integrate_indirect` path continuation with an **NEE cache** (`pass/nee_cache`), ReSTIR GI temporal, spatial and final shading (`restir_gi_*`), and optional **NRC** (`pass/nrc`).
5. **Volumetrics.** A froxel radiance cache with light sampling (`rtx_global_volumetrics`, `pass/volumetrics`).
6. **Particles.** A GPU particle system (`pass/particles`), dust, and ray-traced billboards.
7. **Denoise.** Demodulation, then NRD (ReBLUR/ReLAX) or DLSS-RR, then remodulation and composite.
8. **Upscale.** DLSS, XeSS, NIS or TAA-U.
9. **Post.** Auto exposure, global and local tone mapping, bloom, post FX, dithering.
10. **Other.** Terrain baking (rasterizes terrain draws and decals into a baked texture, `rtx_terrain_baker.cpp`), anti-culling and foliage systems (`documentation/{AntiCullingSystem,FoliageSystem,TerrainSystem}.md`), and sparse rendering.

### 1.9 Capture tool

`GameCapturer` (`rtx_game_capturer.cpp`, 1,180 lines) plus `lssusd/game_exporter.cpp` write a USD stage through OpenUSD:
- per-hash meshes (`meshes/mesh_*.usd`), materials (`materials/`), lights, instances with per-frame transforms, and cameras;
- textures exported as DDS (`textures/`) and thumbnails;
- `meta.geometryHashRule` records the asset hash rule (`rtx_game_capturer.cpp:1059`).

### 1.10 C API

`remix_c.h` is API 0.6. It exposes startup and shutdown, material, mesh and light creation, instance drawing, camera setup, config variables, present, object picking and `dxvk_*` interop (CreateD3D9, RegisterD3D9Device, external swapchain, VkImage access). It is implemented in `rtx_remix_api.cpp` and forwarded over the bridge (`RemixApi_*` commands). There is **no REST server in the runtime**; the REST API belongs to the Toolkit.

### 1.11 Developer menu

The in-game overlay is ImGui (`dxvk_imgui.cpp` 4,590 lines, `rtx_user_menu.cpp`). It has:
- user and developer tabs;
- texture categorization with hover and click selection;
- debug views (`rtx_debug_view.cpp` 1,780 lines);
- the capture UI;
- option layers;
- graph editing and inspection;
- a memory profiler.

### 1.12 Build system and platform limits

- **Build.** Meson + Ninja driven by PowerShell scripts. Dependencies come from packman (`packman-external.xml`): USD, Slang, NRD, NRC, rtxio, Reflex, aftermath, fonts, sentry. `meson.build:72` has an MSVC path that most targets use. The code uses MSVC intrinsics (`m128_f32`, `_stricmp`, `<intrin.h>`) in `rtx_render`, `d3d9`, `util` and `lssusd` (18 sites) and the SSE/NEON paths in `d3d9_rtx_geometry.cpp`.
- **Targets.** x64 and ARM64/ARM64EC runtime; x86 only for the bridge client. Windows only: WSI, the packman binaries and NVIDIA SDK DLLs are Windows builds, and OpenUSD is Windows-packaged.
- **Linux.** Users run the Windows DLL under Wine/Proton. The *proprietary dependencies*, not DXVK, are what stop a Linux-native build. Upstream DXVK builds natively on Linux (dxvk-native) and cross-builds with MinGW (`build-win64.txt`).

---

## 2. Target architecture: FUSE Relight

### 2.1 Overview

```
 game.exe (x86 or x64)                                                      FUSE-owned, MIT
 ─────────────────────────────────────────────────────────────────────────────────────────────
 x86 game: Relight bridge client d3d9.dll / d3d8.dll  ──shared-memory IPC──┐   (RL-2.x)
 x64 game: loads Relight d3d9.dll / d3d8.dll directly ───────────────────┐ │
                                                                         ▼ ▼
 ┌───────────────────────── x64 process: game itself, or fuse_relight_host.exe ─────────────────────┐
 │ Engine/lib/dxvk (vendored upstream DXVK 3.x, zlib; FUSE-DXVK patches marked)                     │
 │   d3d8 → d3d9 front end → DXVK backend (raster executor for the game's own draws, UI, passthrough)│
 │        │ tap: device/resource/draw/present events (IRelightTap)   ▲ inject/composite             │
 │        ▼                                                          │                              │
 │ Source/FUSE/Relight                                               │                              │
 │   capture: geometry + textures + FF state + vertex capture ─► hash (Remix-compatible)            │
 │   scene: classify (UI/sky/shadow/...), instances, lights, camera, categories                     │
 │   replace: replacement DB (remaster.db) + POCO store + Remix mod importer (USD)                  │
 │   logic graphs, particles, terrain baking, options (rtx.conf-compatible), overlay, C API         │
 │        │ GPU scene deltas, bindless handles, game VkImages (imported)                            │
 │        ▼                                                                                         │
 │ Source/FUSE/Renderer (RG v2) on the FUSE-owned VkDevice (DXVK imports it)                        │
 │   T3 path tracer: primary+PSR → ReSTIR DI → ReSTIR GI/PT + radiance cache → denoise → upscale    │
 │   T0–T2 raster remaster: VB/G-buffer → clustered → VSM/CSM → DDGI (+RT shadows/refl on T2)       │
 │   upscalers: FUSE TAAU / FSR1 / FSR3.1 in-tree; DLSS SR/RR, NRD via plugins/nvidia               │
 └──────────────────────────────────────────────────────────────────────────────────────────────────┘
```

### 2.2 Decision AD-1: vendor upstream DXVK 3.x for D3D8/D3D9; do not reuse Remix's fork; do not rewrite

| Option | For | Against |
|---|---|---|
| **(a) Vendor upstream DXVK 3.1.x (zlib) + dxbc-spirv (MIT)** | Mature: years of game-compatibility fixes and app profiles (`util/config`). Native D3D8. Modern SM1–3 translator. Actively maintained. Supports device import. Builds with MinGW. Permissive licence. | Carries its own Vulkan backend (≈ 74k lines) next to FUSE's RHI. We must patch in a tap. Upstream churn. |
| (b) Reuse the Remix fork (DXVK 1.x era + NV-DXVK edits) | Hash behaviour is exact "by construction" | 539 NV edit blocks tangled with proprietary-SDK glue. Old DXVK (dxso, state cache). MSVC-isms. Rebasing it on upstream is harder than re-deriving the few hooks we need. |
| (c) Write our own D3D9 → FUSE RHI translator | One Vulkan backend. Full control. | D3D9 is huge: 30k lines in DXVK alone, plus the fixed-function uber-shader, format conversion, SWVP emulation, state blocks, query semantics and hundreds of game quirks. That is years of compatibility work for no user-visible gain. |

**Adopted: (a).** Hash compatibility does not need the fork. §4.1 re-derives every hash from D3D-level data with the fork's exact layout rules, so it does not depend on DXVK internals.

**Adopted with it (AD-2).** DXVK's backend stays the **raster executor** for the game's own draws. FUSE's RHI does the remaster rendering. Both share **one `VkInstance`/`VkDevice` created by FUSE**, which enables the T2/T3 feature chain from `RendererCaps`. The d3d9 front end creates its `DxvkInstance`/`DxvkDevice` through `DxvkInstanceImportInfo` and `DxvkAdapter::importDevice(DxvkDeviceImportInfo)`, as `d3d11_main.cpp:370–426` already does upstream. The `queueCallback` hook serializes queue access with FUSE's submission lock.

Retargeting d3d9 onto the FUSE RHI is **not planned**. We revisit it only if 3090 profiling shows the dual-backend cost above 0.3 ms/frame (§6.6).

### 2.3 Frame flow

1. **Game thread.** A D3D9 call reaches `D3D9DeviceEx`. The tap copies what it needs (§2.4) and returns: hashing runs asynchronously on the FUSE `JobScheduler`. DXVK records its usual raster work on its CS thread. Draws the classifier tags as *ray-traced* are **skipped** by DXVK (`Ignore`). Draws tagged *raster* or *UI* are kept.
2. **Injection point.** This is the first UI draw of the frame (Remix semantics), or `Present` if there is no UI. The tap asks DXVK to flush and signal a timeline semaphore. Relight finalizes the scene (instances, replacements, lights) and records the FUSE frame graph (RG v2): path trace or raster remaster, then denoise, upscale and post. The result goes into a FUSE-owned image.
3. **Composite.** A patched DXVK context command copies or blits the FUSE image into the current back buffer (layout transitions through `ID3D9VkInteropDevice`-style helpers), waiting on FUSE's timeline value. DXVK then continues with the UI draws on top.
4. **Present.** DXVK presents as usual (its WSI and swapchain). The frame index, jitter and previous-frame data advance.

Game textures stay DXVK `DxvkImage`s. Relight registers their `VkImage`/`VkImageView` handles in the FUSE bindless registry as *external, non-owned* resources, with generation checks and release on DXVK image destruction through a tap event. Replacement textures are FUSE-owned.

### 2.4 The tap: what the vendored d3d9 exposes (patch budget: at most 30 marked blocks)

`Source/FUSE/Relight/tap/relight_tap.hpp` is a C++ interface. The implementation lives in Relight, and d3d9 holds a pointer to it:

| Event | Data | Needed for |
|---|---|---|
| `onDeviceCreate/Reset/Destroy` | present params, adapter, focus window, back-buffer format | swapchain size, primary RT detection |
| `onTextureUpload(subresource 0)` | D3D format, dims, locked rows (pointer + pitch), usage, pool, "full update" flag; `UpdateTexture`/`UpdateSurface` source→dest | texture hash (§4.1.3) |
| `onTextureWriteLock(mip 0)` | handle | `rtx.recomputeTextureHashOnWrite` |
| `onImageDestroy` | handle | bindless release |
| `onDraw(DrawContext, IndexContext, VertexContext[])` | full `Direct3DState9` snapshot: decl, streams, IB, transforms, render states, texture stage states, samplers, lights, material, viewport, RTs, VS/PS bytecode, constants | classification, geometry, material, lights |
| `onDrawDecision` (return value) | `Ignore` / `Raster` / `RayTraced+PreserveRaster` | skip or keep DXVK raster |
| `substituteVertexShader(module)` | SPIR-V of a VS | vertex capture (§2.5) |
| `onQueryBegin/End`, `onClear`, `onSetRenderTarget` | – | occlusion-query rule, primary-RT tracking |
| `onInjectPoint`, `onPresent` | back buffer `VkImage` | render and composite |

The patch set also adds:
- device import in `d3d9_main.cpp` / `d3d9_interface.cpp`;
- a composite hook in `d3d9_swapchain.cpp`;
- a skip path in `D3D9DeviceEx::PrepareDraw`.

When the tap is disabled (env `FUSE_RELIGHT=0`), every hook is a null-pointer check. The gate is bit-identical goldens (§6.3).

### 2.5 Vertex capture (programmable VS), revamped

Remix injects SSBO writes into its dxso compiler (`dxso_compiler.cpp:483+`, 48-byte `CapturedVertex`: position, texcoord0, normal0, color0; back-transformed with `invProj`, `viewToWorld` and `worldToObject`).

Relight does this as a **FUSE-owned SPIR-V transform** (`Source/FUSE/Relight/capture/vertex_capture/`). It:
- runs on the *final* SPIR-V of any VS, whether dxbc-spirv SM1–3 or DXVK's fixed-function uber-shader when needed;
- locates the `Position`, `TEXCOORD0`, `NORMAL` and `COLOR0` output variables;
- appends at every `OpReturn` of the entry point a store to a bound SSBO indexed by `gl_VertexIndex − baseVertex`.

This survives upstream DXVK changes because it touches only standard SPIR-V. The CPU side reproduces Remix's back-transform exactly. It is used only when `rtx.useVertexCapture` is enabled (default true upstream, `d3d9_rtx.h:43`), as in Remix.

### 2.6 Process model and 32-bit games

- **x64 games** load Relight in-process.
- **x86 games** load the **Relight bridge client** (x86 PE, D3D9/D3D8 COM proxies). It serializes to `fuse_relight_host.exe` (x64), which hosts vendored DXVK and Relight and presents into the game's window (HWND shared across processes, as Remix does).

The bridge is a port and revamp of `bridge/` (MIT), with these changes:
- a **generated** command schema: one table produces the client encoders, host decoders and tests, instead of 370 hand-written cases;
- a versioned handshake;
- a single shared-heap implementation with a POSIX backend for Linux-native unit tests;
- hang and crash detection with a clean fallback.

### 2.7 FUSE module layout (new)

```
Engine/lib/dxvk/            vendored upstream DXVK 3.1.x subset (d3d8,d3d9,dxvk,util,wsi,spirv,vulkan) + VERSION + LICENSE + PATCHES.md
Engine/lib/dxbc-spirv/      vendored (MIT) + VERSION
Engine/lib/xxhash/          vendored xxHash 0.8.x (BSD-2) + VERSION
Engine/lib/imgui/           vendored Dear ImGui + ImPlot (MIT) + VERSION
Engine/lib/gdeflate/        vendored GDeflate CPU codec (Apache-2.0) + NOTICE + VERSION
Source/FUSE/Relight/
  THIRD_PARTY.md
  cmake/                    relight_*.cmake (one per work package), licence gates
  tap/                      IRelightTap, tap dispatcher
  hash/                     Remix-compatible hashing (pure logic, no Vulkan)
  options/                  layered options, rtx.conf/user.conf/dxvk.conf reader, relight.* ↔ rtx.* aliases
  capture/{geometry,texture,vertex_capture,export}/
  scene/{classify,translate,instances,lights,camera,categories}/
  replace/                  runtime replacement engine, mod stacking, hot reload
  mods/{usd,import,assets}/ Remix mod reader, importer to POCO + DB, DDS/pkg/GDeflate
  logic/                    graph runtime + components + USD parser
  particles/ terrain/       ported systems
  render/{frame,raster,material,lights,pathtrace,volumetrics,post}/   Relight-specific rendering on the FUSE renderer
  shaders/                  Slang (GLSL until WP-0.5 lands)
  kernels/                  single-source CPU references (BSDFs, light sampling, reservoirs, filters)
  bridge/{ipc,client,host,launcher}/
  api/                      fuse_relight C API + remixapi 0.6 compat exports
  overlay/                  in-game ImGui developer menu
  shell/                    d3d9.dll / d3d8.dll entry points, .def files
  tests/
Tests/relight/{apps,fixtures,golden}/     test apps (C, MinGW x64/x86), fixture mods/captures (ours), goldens
Tools/FUSE/Relight/        fuse_relight_import, remix_hash_ref.py, capture diff tools
```

### 2.8 Tier behaviour

| Mode | Device need | What renders | Default on |
|---|---|---|---|
| **Passthrough** | Vulkan 1.3 (DXVK's own minimum) | Original game via DXVK. Replacement textures swapped in place (sampler-compatible). Captures work. | any device without T0 |
| **T0 raster remaster** | renderer T0 | Captured scene + replacements through FUSE deferred/VB, clustered lights, CSM→VSM, SDF DDGI, SSAO/SSR, TAAU/FSR | Lavapipe masked T0, iGPUs |
| **T1** | T0 + mesh shaders | Same, meshlet path | – |
| **T2** | T1 + ray query/AS | T0 + ray-query shadows and reflections, hardware DDGI, ReSTIR DI | Lavapipe (functional), RTX 20/30 "balanced" |
| **T3 path tracing** | T2 + RT pipeline | Full path tracer (§5) | RTX 3090 and up |

Tier selection is automatic from `RendererCaps`, with an override (`relight.tier`, alias `rtx.enableRaytracing=False` → Passthrough). Replacement assets are shared by all tiers. POCO LOD and cook tiers apply (Remaster §2.4).

---

## 3. Component-by-component port table

Action key: **vendor**, **port** (MIT/zlib code re-homed and modified), **rewrite** (new FUSE code guided by upstream behaviour, where upstream code is MIT but unsuitable), **reimplement** (clean room, from papers or specs; upstream not permissive), **replace** (an existing FUSE system takes over), **drop**.

| # | Upstream component | Lines | Action | FUSE destination | Depends on | Test strategy |
|---|---|---|---|---|---|---|
| 1 | DXVK `d3d9` (upstream 3.1.1) | 30,657 | vendor + ≤ 30 marked patches | `Engine/lib/dxvk/src/d3d9` | – | Test apps tap-off/tap-on goldens; Wine-builtin d3d9 cross-reference |
| 2 | DXVK `d3d8` (upstream) | 5,815 | vendor | `Engine/lib/dxvk/src/d3d8` | 1 | D3D8 twin apps equal D3D9 captures |
| 3 | DXVK backend `dxvk`, `util`, `wsi`, `spirv`, `vulkan` (upstream) | ≈ 89,900 | vendor + import patch | `Engine/lib/dxvk/src/*` | – | FUSE device import under validation |
| 4 | dxbc-spirv (SM1–3) | (pinned) | vendor | `Engine/lib/dxbc-spirv` | – | SM2/SM3 apps |
| 5 | Remix `d3d9_rtx*` draw path (classification, index/vertex processing, state capture) | 2,122 | port (logic onto the tap) | `Relight/capture/*`, `Relight/scene/classify` | 1, tap | Rule unit tests + app captures |
| 6 | `rtx_hashing`, `d3d9_rtx_geometry` hashing, texture hash, `rtx_utils` hash helpers | ≈ 700 | port (bit-exact) | `Relight/hash` | xxHash | KATs + Python reference + 3090 Remix parity |
| 7 | Vertex capture in `dxso_compiler.cpp` | ≈ 250 | rewrite as a SPIR-V pass | `Relight/capture/vertex_capture` | 4 | SM2/SM3 apps vs CPU transform |
| 8 | `rtx_types` DrawCallState, categories, sky/terrain heuristics | 1,703 | port | `Relight/scene/{classify,categories}` | 5 | Unit tests per category |
| 9 | Scene manager, instance manager, draw-call tracker/cache | ≈ 7,100 | port + revamp onto the FUSE GPU scene | `Relight/scene/instances`, `Relight/render/frame` | WP-1.1 | multi_instance app, ID stability, MV parity |
| 10 | Camera + camera manager | 2,000 | port | `Relight/scene/camera` | 5 | Projection decomposition KATs |
| 11 | Accel manager | 2,437 | replace (renderer WP-6.0 AS builder) + port the policies (instance masks, alpha categories, unordered TLAS) | `Relight/render/pathtrace/accel_policy.*` | WP-6.0 | Ray-query hit IDs vs CPU BVH |
| 12 | Opacity micromaps | 3,231 | port (policy) + new AS glue; T3 HW only | `Relight/render/pathtrace/omm/*` | 11, `VK_EXT_opacity_micromap` | HW checklist; CPU OMM baker parity |
| 13 | Materials (`rtx_materials.h`, `rtx_material_data.h`), legacy material | 2,643 | port | `Relight/scene/translate`, `Relight/render/material` | 5 | Parameter-table round trip; FF state tests |
| 14 | Texture manager (streaming, mip bias, VRAM budget) | 1,853 | replace (FUSE upload queue + residency) + port the policies | `Relight/replace/texture_residency.*` | WP-0.4 | Budget tests |
| 15 | Lights, light manager, light data, D3D light conversion | 4,796 | port | `Relight/scene/lights`, `Relight/render/lights` | 5 | lit_ff app; light-hash KATs |
| 16 | Ray portals | 852 | port | `Relight/render/pathtrace/portals.*` | 21 | Fixture scene; CPU reference PT |
| 17 | Mod manager + USD mod reader (`rtx_mod_usd.cpp`) | 2,834 | rewrite on TinyUSDZ (OpenUSD API is not used) | `Relight/mods/usd` | TinyUSDZ | Fixture mods (ours) + OpenUSD-flatten cross-check |
| 18 | Asset replacer, asset data manager, packages, rtxio | 1,851 | port (replacer, package format) + reimplement (GDeflate path with the Apache-2.0 reference) | `Relight/replace`, `Relight/mods/assets` | 17 | Decode round trips |
| 19 | USD plugins (categories, particle schema) | 139 + schema `.usda` | port (schema facts only) | `Relight/mods/usd/schemas` | 17 | Schema fixtures |
| 20 | Game capturer + `lssusd` exporter | 6,012 | rewrite (USDA text writer, no OpenUSD) + FUSE capture (POCO) | `Relight/capture/export` | 9, Remaster W0.1–0.3 | Re-ingest round trip; Toolkit opens it (manual) |
| 21 | RT shaders: concept/surface, surface_material, lights, geometry resolver, PSR, portals, decals, alpha blend, eye, view distance, visibility | ≈ 25,000 | port to FUSE Slang (MIT) | `Relight/shaders/{material,lights,pathtrace}` | WP-0.5 | CPU reference kernels + white furnace |
| 22 | Integrator (`algorithm/integrator*`, `pass/integrate/*`), NEE cache | ≈ 5,500 | rewrite on the FUSE PT core (WP-7.3); NEE cache **replaced** by the light tree + ReSTIR | `Renderer` WP-7.3 + `Relight/render/pathtrace` | 21 | CPU reference PT parity |
| 23 | RTXDI passes (`pass/rtxdi/*`, `rtx_rtxdi_rayquery`) + `RtxdiApplicationBridge` | ≈ 3,700 | **reimplement** (clean room) | Renderer WP-7.2 per §5.3 | WP-7.1 | Unbiasedness and variance gates |
| 24 | ReSTIR GI passes (`restir_gi_*`, `rtx_restir_gi_rayquery`) | ≈ 1,500 | reimplement from Ouyang et al. 2021 (the upstream code is MIT but coupled to RTXDI headers; a clean design is simpler) | WP-7.2 + `Relight/render/pathtrace/restir_gi*` | 23 | Same gates |
| 25 | NRC | 1,614 + 1,178 shader | reimplement (hash-grid radiance cache; own neural cache later) | `Relight/render/pathtrace/radiance_cache*` | 22 | Bias and variance vs reference |
| 26 | NRD host + DLSS-RR + DLSS + DLFG + Reflex + XeSS + NIS hosts | 12,617 | replace: in-tree denoiser (WP-6.4 per §5.5), IUpscaler registry, `plugins/nvidia` (DLSS SR/RR), WP-4.3 (XeSS), NIS already vendored | `Renderer/…` | WP-6.4, 4.x | Existing plugin mocks + denoiser gates |
| 27 | Global volumetrics | 1,132 + 382 shader | port (MIT) onto FUSE froxels (WP-8.1) with ReSTIR-style light sampling | `Relight/render/volumetrics` | WP-8.1 | Convergence and ghosting |
| 28 | Particle system + dust | 1,545 + 1,525 shader | port | `Relight/particles` | 21 | Deterministic sim KATs (CPU kernel) |
| 29 | Terrain baker | 1,466 + 133 shader | port | `Relight/terrain` | 9, raster RG | Baked-texture golden |
| 30 | Anti-culling, foliage, point instancer | (in scene/instance mgr) | port | `Relight/scene/instances` | 9 | Fixture tests |
| 31 | Post: tone mapping, local TM, bloom, postFx, auto exposure, TAA, dither, composite, demodulate | ≈ 6,700 | replace with FUSE post (WP-4.5) + Look; port the local tone mapper and demodulate/composite | `Relight/render/post` | WP-4.5 | CPU-reference parity |
| 32 | Debug views | 2,039 + 1,456 shader | port a subset (albedo, normals, motion, hit distance, reservoirs, heat maps) | `Relight/render/debug` | – | Goldens |
| 33 | Options system | 9,153 | port (MIT) | `Relight/options` | – | Ported semantics tests (`test_rtx_option.cpp`, `test_option_layer_export.cpp`) |
| 34 | Logic graphs | 10,792 | port | `Relight/logic` | 17, 33 | Ported `tests/rtx/unit/graph` semantics |
| 35 | C API (`rtx_remix_api.cpp`, headers) | 4,318 | port; FUSE-native API + compat exports | `Relight/api` | 9 | Upstream `tests/rtx/apps/RemixAPI_C` behaviour replicated in our own app |
| 36 | Developer menu (`dxvk_imgui.cpp`, `rtx_user_menu.cpp`, GUI widgets) | ≈ 8,000 | port (MIT) onto vendored Dear ImGui | `Relight/overlay` | Engine/lib/imgui | Scripted UI tests (ImGui test engine is not needed; state-machine unit tests) |
| 37 | Bridge client/server/util/launcher | 27,634 | port + revamp (generated schema) | `Relight/bridge` | 1 | Wine32 app parity; IPC fuzz |
| 38 | GPU crash recorder, Sentry, Nsight, aftermath | ≈ 1,600 | drop (FUSE crash handler exists) | – | – | – |
| 39 | Sparse rendering, DLSS neural rendering | ≈ 900 | drop (DLSS 5 look is in `plugins/nvidia`) | – | – | – |
| 40 | MDL files, RTXCR, RTXDI, NRC, NRD, rtxio, nvapi, Detours, sse2neon, gli/glm, Tracy | – | drop / reimplement (§0.3) | – | – | Licence gates |

---

## 4. The replacement asset format and Remix mod compatibility

### 4.1 Hash specification (normative; `Source/FUSE/Relight/hash`)

Everything below reproduces dxvk-remix @0867d3c exactly. Every function has a known-answer test (KAT) and a Python reference in `Tools/FUSE/Relight/remix_hash_ref.py`, using the `xxhash` wheel (BSD-2).

#### 4.1.1 Geometry components

The `HashComponents` enum is ordered: `positions`=0, `legacypositions0`, `legacypositions1`, `texcoords`, `indices`, `legacyindices`, `geometrydescriptor`, `vertexlayout`, `vertexshader`.

**Preprocessing:**
- Indexed draws:
  - indices are rebased: `idx − minIndex`, keeping the original width (u16/u32);
  - `vertexCount = maxIndex − minIndex + 1`;
  - the vertex region starts at `streamOffset + stride × (BaseVertexIndex + minIndex)`;
  - draws with `maxIndex == minIndex` are skipped.
- Non-indexed draws: `vertexCount = GetVertexCount(primType, primCount)`.

**Components:**
- **positions / texcoords.** `elementSize` is the byte size of the declaration element's format (FLOAT3 = 12, FLOAT4/POSITIONT = 16, and so on, per `DecodeDecltype` → VkFormat element size).
  - Indexed: take the *sorted unique* rebased indices (bin table). For each index `i`: `h = XXH3_64bits_withSeed(base + i×stride, elementSize, h)`, starting from `h = 0`.
  - Non-indexed: every vertex in order.
  - texcoords use the stream of `TEXCOORD[usageIndex]` chosen by Remix's `m_texcoordIndex`.
- **indices.** `XXH3_64bits(rebasedIndices, indexCount × sizeof(T))`.
- **geometrydescriptor.** `h = XXH3_withSeed(&indexCount,4,0)`, then `h = XXH3_withSeed(&vertexCount,4,h)`, then `&topology` (**VkPrimitiveTopology**: list = 3, strip = 4, fan = 5), then `&indexType` (**VkIndexType**: UINT16 = 0, UINT32 = 1, NONE_KHR = 1000165000). Each value is a u32.
- **vertexlayout.** `XXH3_64bits(&stride, 8)`, where `stride` is a `size_t`. It is the position stream stride if the data is interleaved and GPU-friendly, else `computeOptimalVertexStride`, which is ported.
- **vertexshader** (programmable VS with capture only).
  1. `h = XXH3_64bits(bytecode)`.
  2. Seed with the float constants `[0, maxConstIndexF)` × 16 bytes.
  3. Then the int constants × 16 bytes.
  4. Then the bool constants, over `maxConstIndexB × 4 / 32` bytes. This integer quirk is kept.

  `maxConstIndex*` comes from shader analysis. Parity for VS draws is **best-effort** (risk R3).
- **legacyindices.**
  - If `indexCount × sizeof(T) ≤ 1024`: `XXH3_64bits(all)`.
  - Else `step = indexCount × sizeof(T) / 512`, and for `i = 0; i < indexCount; i += step`: `h = XXH3_withSeed(&idx[i], sizeof(T), h)`.
- **legacypositions0/1.**
  - `step = 0.01 × meterToWorldUnitScale`, where `meterToWorldUnitScale = 100 × rtx.sceneScale` (`rtx_options.h:1464`).
  - For **every** vertex (not only unique ones): `v = floor(p × (1/step)) × step`. This is the **SSE4.1 path**: multiply by the reciprocal, `_mm_round_ps` with FLOOR. It is what runs on every x86-64 CPU Remix supports. The scalar fallback divides and differs in the last bit, so do **not** use it.
  - Hash 12 bytes per vertex with `h1 = XXH3_withSeed(&v, 12, h1)`.
  - `h0` is `h1` captured when the byte offset equals `min(size, 20 × stride)`. Meshes with fewer than 20 vertices therefore keep `h0 = 0` (edge case kept).
  - A NEON variant is not needed (x86-64 only).

#### 4.1.2 Rules and combination (`GeometryHashes::getHashForRuleImpl`)

- Walk the components in enum order. The first selected component's value is used as-is. Each later one gives `h = XXH64(&field, 8, h)`.
- Rule strings are comma lists of the component names (`createRule`, spaces ignored).
- The defaults:
  - `rtx.geometryAssetHashRuleString = positions,indices,geometrydescriptor`, the **mesh replacement key**;
  - `rtx.geometryGenerationHashRuleString = positions,indices,texcoords,geometrydescriptor,vertexlayout,vertexshader`, which controls which components are computed at all.
- Per-game `rtx.conf` overrides are honoured. Mods made with legacy rules (`legacypositions0,legacyindices`) work as long as the game's `rtx.conf` says so.

#### 4.1.3 Texture hash

- Computed only for `D3DRTYPE_TEXTURE`, not depth-stencil. It is set once, at the **first upload of subresource 0** (`d3d9_device.cpp:4912`). A full-size `UpdateTexture` or `UpdateSurface` inherits the source hash (`:849`, `:900`).
- The hash is `XXH3_64bits(buffer, size)` over mip 0 in the **fork's packed layout**:
  - `rows = blocksHigh × depth`;
  - `rowBytes = align(elementSize × blocksWide, 4)`;
  - `× min(planeCount, 2)`.

  `elementSize` and `blockSize` come from the fork's D3DFORMAT → VkFormat mapping (`d3d9_format.cpp`, `GetMipSize` at `d3d9_common_texture.cpp:214`). The mapping table is ported as hash-only data with one KAT per D3DFORMAT.
- Relight re-packs the app's locked rows into this canonical layout, so the hash does not depend on the vendored DXVK's pitch or allocator.
- Options:
  - `rtx.useObsoleteHashOnTextureUpload` uses `XXH64(data, size, 0)` instead;
  - `rtx.recomputeTextureHashOnWrite` clears the hash on a mip-0 write lock unless the texture is listed as terrain, lightmap, ignore or ignore-baked-lighting (`d3d9_device.cpp:4631`).
- Render targets also get a **descriptor hash** from `D3DSURFACE_DESC` (`m_desc.CalculateHash()`, ported) for the raytraced-RT feature.

#### 4.1.4 Material and light hashes

- **Legacy material hash** = the stage-0 colour-texture hash (`colorTextures[0].getImageHash()`). Untextured draws get 0 and cannot be material-replaced, as upstream.
- **Light hash** (sphere light, `rtx_lights.cpp:296`):
  1. `h = (u64)RtLightType::Sphere`;
  2. `h = XXH64(position, 12, h)`;
  3. `h = XXH64(&radius, 4, h)`;
  4. `h = XXH64(&h, 8, shapingHash)`, where the shaping hash chains direction, cosConeAngle, coneSoftness and focusExponent (`rtx_lights.cpp:116–119`).

  The inputs come from the ported D3DLIGHT9 → `RtLight` conversion, with the same float operation order. Distant, rect and disk lights follow the same pattern (`:478`, `:653`).

#### 4.1.5 Key namespaces in the replacement DB (Remaster §1.3 `hash_key.algo`)

| algo | Value | Notes |
|---|---|---|
| `remix.geom.asset` | 64-bit rule hash | Qualified by `rule_id = XXH3(rule string)` stored with the key |
| `remix.geom.legacy0` / `remix.geom.legacy1` | legacy rule hashes | Older mods |
| `remix.tex` | XXH3 mip 0 | Also the material hash |
| `remix.tex.obsolete` | XXH64 | When the game's `rtx.conf` sets obsolete hashing |
| `remix.rtdesc` | RT descriptor hash | Raytraced render targets |
| `remix.light` | light hash | |
| `fuse.capture.sha256` | canonical sha256 (Remaster §1.3) | Links a Remix key to the canonical `oaid` when pixels or vertices are available |

### 4.2 Mod discovery and stacking

- **Search path.** Same as upstream: `<game>/rtx-remix/mods/<Mod>/mod.{usda,usdc,usd}`, plus the base-game-mod regex path. Relight also scans `<game>/fuse-relight/mods/` for FUSE-native mods.
- **Priority.** Order is by the mod's layer priority, then name (upstream `Mod::m_priority`). FUSE-native mods sit above Remix mods by default. Per-game `relight.modOrder` (alias `rtx.*`) can reorder.
- **Per-mod `rtx.conf`** loads as the "Remix Config" layer (§1.6).
- **Hot reload.** A file watch plus a USD re-read. Invalidation is per hash through the same prim-path classification as `classifyChangedPath` (`rtx_mod_usd.cpp:466`).

### 4.3 USD subset and reader

- **Runtime reader:** TinyUSDZ, vendored by Remaster W2.2 and consumed here. It reads USDA, USDC (crate) and USDZ.
- **Composition implemented by Relight** (the "Remix profile"):
  - sublayers (with offsets ignored);
  - references and payloads to external layers, meshes and material libraries;
  - `over` specifiers merged by strongest opinion;
  - variant sets (default selection);
  - `instanceable` prims flattened;
  - asset-path resolution relative to the defining layer.

  Anything outside the profile (inherits, specializes, relocates, value clips) produces a clear warning with the prim path.
- **Cross-check.** `Tools/FUSE/Relight/usd_flatten_check.py` uses the `usd-core` wheel (TOST/Apache) to flatten a mod. CI compares our composed result on every fixture, and users can run the same check on real mods.

### 4.4 Mapping Remix mods to POCO assets

| Remix mod element | POCO record (Remaster §2.3) | Relight extension |
|---|---|---|
| `/RootNode/meshes/mesh_<H>` with child mesh prims / references | `Mesh` (streams: Position, Normal, Uv0, Color0, Joints/Weights; `indices32`; submeshes per `GeomSubset`) + `replacement(oaid(remix.geom.asset=H), variant=default, tier=remix)` | `relight_replacement` record: `preserveOriginalDrawCall`, local transform, instance categories, attached lights, particle system, graph refs |
| Material bound to the replacement, or `/RootNode/Looks/mat_<H>` | `Material` + `TextureSet` | `relight_material` record (below) |
| `/RootNode/lights/light_<H>` and lights under meshes | `Light` (Point/Spot/Rect/Disk/Directional/Dome) | shaping (cone, softness, focus), volumetric scale, cylinder type |
| `RemixInstanceCategoryAPI` | – | categories bitmask |
| `ParticleSystemAPI` | – | `relight_particles` record |
| OmniGraph prims | – | `relight_graph` record (component graph JSON) |
| Per-mod `rtx.conf` | – | stored as the mod's option layer |

`relight_material` is a POCO extension kind (`fuse.poco/1`, kind `material_ext`, same id as the base `Material`). It holds every field of `LIST_OPAQUE_MATERIAL_*` / `LIST_TRANSLUCENT_MATERIAL_*` / portal that the base POCO `Material` lacks:
- thin film and its thickness;
- anisotropy;
- sprite sheet;
- blend type and inverted blend;
- alpha-test type and reference;
- legacy-alpha-state use;
- displacement in/out (POM);
- SSS transmittance, measurement distance, single-scatter albedo, anisotropy, diffusion-profile radius and scale;
- filter and wrap modes;
- translucent IoR, transmittance measurement distance, thin-walled flag and diffuse layer;
- the portal index.

This keeps the Remaster header untouched. The Remaster owner can later fold these fields into `Material` v2 without breaking readers.

### 4.5 Material model mapping (MDL name → FUSE)

| `info:mdl:sourceAsset` | FUSE `ShadingModel` | Notes |
|---|---|---|
| `AperturePBR_Opacity.mdl` | `Opaque` (masked or blended per `blend_enabled`, `alpha_test_type`) | albedo/opacity, roughness, metallic, normal (OpenGL convention in Remix textures; verify per texture with the asset plan's normal gate), emissive mask × `emissive_intensity`, height → POM, SSS → `SubsurfaceSSS` when `subsurface_*` is set |
| `AperturePBR_Translucent.mdl` | `Translucent` (FUSE translucent BSDF, §5.2) | IoR, transmittance colour and measurement distance (Beer–Lambert), thin-walled, diffuse layer |
| `AperturePBR_Portal.mdl` | Relight portal surface | `rayPortalIndex` pairs portals |
| Unknown MDL or no shader | `Opaque` defaults (MIT defaults table) + warning | – |

Ranges and defaults come from the MIT tables verbatim, for example `emissive_intensity` in [0, 65504] with default 40 and `diffuse_color_constant` default 0.2. The shader-side evaluation is FUSE's own port of the MIT `opaque_surface_material*.slangh` / `translucent_*` logic.

### 4.6 Textures and packages

- **DDS reader** (`Relight/mods/assets/dds.*`): DX9 and DX10 headers, BC1–BC7 including BC6H, R8G8B8A8/B8G8R8A8/R16F/R32F, mips, arrays and cubes. The sRGB flag follows the USD `inputs:*:colorSpace` hint or the parameter's colour space (Remix: `diffuse_texture` and `emissive_mask_texture` are sRGB, data maps are linear). BCn blocks upload without re-encoding.
- **`.pkg` / `.rtxio` packages:** the header and dictionary as in `rtx_asset_package.h` (MIT facts: magic `0xbaadd00d`, version 1, `AssetDesc`). GDeflate blobs are decoded on the CPU with the vendored Apache-2.0 reference; GPU decode is a later option. A missing codec falls back cleanly.
- **Streaming and residency:** FUSE `UploadQueue` plus a VRAM budget (policies ported from `rtx_texture_manager`: mip bias from upscaling, preload flags, `preload_textures`).

### 4.7 Runtime replacement semantics (port of `SceneManager::submitDrawState` / `drawReplacements`)

- **Mesh replacement.** Keyed by `remix.geom.asset` under the active rule. Replacement prims are drawn with the original instance's transform. The original draw is hidden unless `preserveOriginalDrawCall`. Categories come from USD or the texture lists.
- **Material replacement.** Keyed by the draw's legacy material hash, the stage-0 texture. It applies to original draws and to mesh replacements with no bound material.
- **Light replacement.** A game light hash can be replaced or deleted. Lights attached to mesh replacements follow their instances.
- **Instance identity** (§1.5) drives replacement stability, motion vectors and anti-culling.
- **Variants.** POCO replacement `variant` (Remaster §1.3) selects `default`, `lowspec`, `stylised` and so on per tier.

### 4.8 Round trip and export

- `fuse_relight_import <mod dir> --game <id>` writes POCO + DB rows (derived-from-mod provenance; licence from the mod's own licence file, or `LicenseRef-ThirdPartyMod-<name>` marked **never shipped by us**).
- Export POCO → Remix mod is owned by Remaster W2.4. Relight's KATs make sure exported names match runtime hashes.
- Relight captures are written in two forms:
  - a **FUSE capture** (POCO + keys);
  - a **Remix-compatible capture USDA** (§1.9 layout; meshes and materials as `mesh_<H>` / `mat_<H>`; DDS textures written by our encoder, or kept as in-game BC blocks).

  Either toolchain can author against them. The capture folder stays in `build/remaster-cache/<game>/capture/` (Remaster §0.1.2).

---

## 5. The path tracer revamp

The goal is best-in-class quality for DX8/9-era scenes on an RTX 3090 at 1440p output. Everything here is FUSE's own code, from the cited papers. The generic parts are specified as **requirements for renderer WP-6.0, 6.4, 7.1, 7.2 and 7.3**. The Relight-specific parts are RL work packages.

### 5.1 Frame graph (T3)

```
GPU scene update (instances, prev transforms, materials, lights, emissive tris)
 → AS build/refit (WP-6.0; OMM on HW)
 → Primary: ray-query/RT G-buffer + PSR (mirrors, glass, portals, decals, alpha-blended layers)
 → Light tree build/refit (WP-7.1) + presampled light tiles
 → ReSTIR DI: initial RIS → temporal → spatial (GRIS pairwise MIS) → visibility → shade
 → Indirect: BSDF-sampled path (1 bounce reservoir) → ReSTIR GI temporal/spatial → final shade
      └ radiance cache (hash grid) for path termination and far-field
      └ [Ultra] ReSTIR PT (hybrid reconnection shift) replaces GI reuse
 → Volumetrics (froxel, RIS light sampling), particles
 → Demodulate → denoiser (diffuse, specular, DI/shadow signals) or DLSS-RR (plugin)
 → Remodulate + composite (+ raster UI, from DXVK)
 → Upscale: FUSE TAAU / FSR 3.1 / DLSS SR (plugin) → Look/post (exposure, TM, bloom, grading)
```

### 5.2 Surfaces, materials and lights (Relight, port of the MIT code)

- **BSDFs.** Opaque: GGX with height-correlated Smith, **multiple-scattering energy compensation** (Kulla–Conty 2017; shared with WP-2.2's DFG LUT), diffuse (Lambert, with a Burley option), thin film (Belcour–Barla 2017), anisotropy. Translucent: dielectric with Beer–Lambert, thin-walled, diffuse layer.
- **SSS.** Burley normalized diffusion (Christensen–Burley 2015), sampled with Golubev's disk sampling, plus single scattering. This replaces the proprietary RTXCR path.
- **Hair cards.** Chiang et al. 2016 (d'Eon 2011 lobes), used when `HairCards` is set.
- **Legacy compatibility.** Alpha-test types, all D3D blend modes as ray-traced blended layers (`alpha_blend_surface.slangh` semantics), vertex colour, `TFACTOR`, texture transforms, sprite sheets, decals with the four offset modes, and ray portals with PSR.
- **Lights.** Sphere, rect, disk, cylinder, distant and dome, with shaping (cone, softness, focus). Emissive triangles come from materials and feed the light tree. Game D3D lights convert per §4.1.4.
- **CPU references** (`Relight/kernels/bsdf_*`, single-source) for every `eval`, `sample` and `pdf`, plus light `sample`/`pdf`. They are the parity oracles.

### 5.3 Direct lighting: light tree + ReSTIR DI (renderer WP-7.1/7.2 requirements)

- **Light tree.** Estevez & Kulla 2018 / Conty & Kulla 2018 many-light importance tree with orientation cones, built on the CPU and refitted on the GPU. Emissive triangles are bucketed by instance.
- **ReSTIR DI:**
  - Bitterli et al. 2020 reservoirs;
  - **presampled light tiles** (Wyman & Panteleev 2021, "Rearchitecting spatiotemporal resampling");
  - the **GRIS** formulation with pairwise MIS and correct shift Jacobians (Lin et al. 2022);
  - M-capping, visibility reuse, a "boiling filter" and temporal gradients for denoiser anti-lag (Schied 2018 A-SVGF gradients).
  - Two modes: *unbiased* (the gate reference) and *fast* (biased reuse with capped M).
- **Clean-room rule.** Implementers do not open `RtxdiApplicationBridge.slangh` or RTXDI SDK sources. They work from the papers above and the Remix *host-side* MIT code for integration only.

### 5.4 Indirect lighting and the radiance cache

- **ReSTIR GI** (Ouyang et al. 2021): one-bounce sample reservoirs, temporal and spatial reuse with reconnection Jacobians, and a validation ray budget.
- **ReSTIR PT** (Lin et al. 2022, hybrid reconnection shift) as the "Ultra" preset once GI parity passes. It is gated by variance-per-ms against GI on the 3090.
- **Radiance cache (NRC replacement, default).** A world-space **hash-grid radiance cache** (spatial hashing after Binder et al. 2019 path-space filtering and Gautron 2020), updated by short training paths and queried at path termination and for far-field bounces. It is deterministic and cheap, runs on T2/T3 including Lavapipe, and has a CPU reference.
- **Neural cache (research, T3 HW).** Our own real-time neural radiance cache per Müller et al. 2021: a fully fused MLP on `VK_KHR_cooperative_matrix` / `VK_NV_cooperative_vector`, with a CPU reference MLP. tiny-cuda-nn (BSD-3) is an offline oracle only. It ships only if it beats the hash grid on the 3090 (renderer plan Phase 9 rule).
- The NEE cache (Remix `pass/nee_cache`) is **not ported**. The light tree + ReSTIR DI covers it.

### 5.5 Denoiser (renderer WP-6.4 requirements; in-tree default)

The denoiser is FUSE's own "recurrent spatiotemporal" design, from the published literature.

- **Inputs:** demodulated diffuse and specular radiance, hit distance, DI signal and shadow visibility, normal/roughness, depth, motion (FUSE GPU-scene previous transforms), and instance IDs.
- **Temporal:** reprojection with disocclusion tests (depth, normal, instance), **specular reprojection by virtual motion / hit distance** (parallax-correct), history clamping, and accumulation-speed tracking.
- **Anti-lag:** A-SVGF temporal gradients (Schied et al. 2018) fed by the ReSTIR gradient pass.
- **Spatial:**
  - SVGF variance-guided à-trous, 5 levels (Schied et al. 2017);
  - a pre-blur with hit-distance-scaled radii, as in the recurrent-blur approach of *Ray Tracing Gems II* ch. 49;
  - **history fix** from mip pyramids for disocclusions;
  - firefly suppression.
- **Separate lightweight paths** for shadows (ray-traced soft shadow denoise, SIGMA-like from first principles) and volumetrics.
- **Plugins:** NRD (ReBLUR/ReLAX, WP-6.4b) and **DLSS Ray Reconstruction** (`dlss_rr`, which replaces the denoiser and upscaler together) through `plugins/nvidia`. They are selected at runtime and never required.

### 5.6 Upscaling and post

- **Upscaling.** In-tree FUSE TAAU (WP-4.1), FSR1/NIS/CAS (vendored) and FSR 3.1 (WP-4.2, MIT). **DLSS SR** comes through the plugin; on the 3090, SR and RR work, but DLSS FG needs RTX 40. The mip bias follows the upscale ratio (ported policy).
- **Post.** FUSE post (WP-4.5: exposure histogram, bloom, tone mapping including AgX/ACES, LUT) and the **Look** system. Remix's local tone mapper is ported as a Look node. The "Classic" Look reproduces original captures (Remaster §5.1).

### 5.7 Volumetrics, particles, decals, terrain, OMM

- **Volumetrics.** Froxel media (WP-8.1) extended for T3. In-scattering uses RIS over the light tree plus reservoir reuse (the ported Remix concept), a temporal filter, and fog parameters from the D3D fog state.
- **Particles.** The ported GPU particle system (USD `ParticleSystemAPI`) and ray-traced billboards (`concept/billboard.h`). The simulation has a CPU kernel reference.
- **Decals and terrain baking.** Ported. Terrain decals bake into a cached texture through RG raster passes.
- **Opacity micromaps.** `VK_EXT_opacity_micromap` when present. Lavapipe does not expose it, so it is HW-only with an any-hit fallback.

### 5.8 CPU reference implementations and parity (FUSE CPU-first rule)

Every GPU algorithm has a single-source kernel (`compute_kernel`, `CpuReference` and `CpuParallel`) used as its oracle on tiny scenes:

| Kernel (`Relight/kernels/…`) | Oracle for | Gate |
|---|---|---|
| `bsdf_eval/sample/pdf` | material shaders | White furnace (albedo 1 → 1 ± 1%), reciprocity, χ² sample-vs-pdf test, GPU/CPU ≤ 1e-4 |
| `light_sample/pdf`, `light_tree_*` | light sampling | pdf integrates to 1 ± 1e-3; tree vs brute force |
| `ris_reservoir` (stream, merge, GRIS pairwise) | ReSTIR DI/GI shaders | Merge associativity; unbiased-mode mean within 3σ of the reference |
| `pt_reference` (brute-force path tracer, MIS, NEE) | the GPU PT | 4k-spp reference images for 32×32 to 64×64 scenes |
| `radiance_cache_*` | hash grid | Same hash cell ids; cache query vs reference |
| `svgf_*`, `atrous_*`, `temporal_*` | denoiser passes | Per-pass parity ≤ 1/1024 |
| `froxel_*`, `particle_sim` | volumetrics, particles | Convergence, deterministic sim |
| `taau` (existing TAAU kernel) | TAAU shader | Existing gate |

Scenes come from the Relight **test-app captures** themselves (Cornell-style fixed-function app, lit app, alpha app), so parity also covers capture → scene → render.

### 5.9 Lower tiers (raster remaster)

- The same captured scene and replacements feed FUSE's raster path: legacy G-buffer now, VB + material resolve after WP-1.5.
- Lighting and effects: clustered lights (WP-2.1), CSM/VSM (WP-3.x), DDGI (WP-6.1, SDF on T0 and ray query on T2), RT shadows and reflections on T2 (WP-6.2), SSAO/SSR (WP-6.3), TAAU/FSR.
- Translucency and legacy blending use the forward pass (WP-2.3).
- The raster path uses the same material parameters, so replacements look consistent across tiers.

---

## 6. Testability here, and CI gates

### 6.1 Environment facts (verified in this session)

| Fact | Evidence |
|---|---|
| MinGW x64 posix toolchain present; **no i686 MinGW** | `/usr/bin/x86_64-w64-mingw32-g++-posix`; no `i686-w64-mingw32-*` |
| Wine 9.0 (Ubuntu) x64 present; **wine32 missing** | `wine --version` asks for wine32; `/usr/lib/x86_64-linux-gnu/wine/i386-windows` holds 1 file |
| **A Windows x64 PE calling `vulkan-1.dll` under Wine + Xvfb reaches Lavapipe** | A test exe built with MinGW: `vkCreateInstance` = 0, 1 device "llvmpipe (LLVM 20.1.2, 256 bits) api 1.4.318" |
| Through winevulkan, the device exposes `VK_KHR_acceleration_structure`, `VK_KHR_ray_query`, `VK_KHR_ray_tracing_pipeline`, `VK_KHR_ray_tracing_position_fetch`, `VK_EXT_mesh_shader`, `VK_KHR_swapchain` (140 extensions) | Same test exe enumerating device extensions |
| With the **null** Wine display driver (the current `fuse-wine-run.sh` default), winevulkan fails: "Failed to load Wine graphics driver supporting Vulkan", `VK_ERROR_INITIALIZATION_FAILED` | Same exe without `DISPLAY` → the Relight runner must use Xvfb (`xvfb-run` is installed) |
| Lavapipe lacks `VK_EXT_opacity_micromap`, cooperative matrix/vector and SER; it has `VK_EXT_headless_surface`, `shader_object`, 16-bit storage | `vulkaninfo` (native) |
| No `slangc`; `glslangValidator` present | renderer doc §2, re-checked |

### 6.2 Test apps (`Tests/relight/apps/`, C99, MinGW x64 + i686, no third-party code)

Each app is deterministic: fixed resolution 128×96, fixed frame count, no timing dependence. It renders N frames and on the last frame does `GetRenderTargetData` → raw RGBA dump, plus a JSON sidecar with its own known inputs (vertex data, texture bytes), which the hash KATs use.

| App | Covers |
|---|---|
| `ff_triangle` / `ff_textured` | FF pipeline, texture hash, geometry hash (indexed u16, u32, non-indexed) |
| `ff_lit` | D3D lights (point, spot, directional), material colour sources, specular |
| `ff_alpha` | alpha test ops, all blend modes, separate alpha |
| `ff_skinned` | indexed vertex blending (BLENDWEIGHT/INDICES), world matrices |
| `ff_multi_instance` | same mesh, many transforms, moving objects, a teleport past `uniqueObjectDistance` |
| `sky_ui_hud` | skybox (low minZ and texture lists), ortho HUD, `POSITIONT` HUD, injection point |
| `stencil_shadow` / `shadow_mask` | the two ignore heuristics |
| `rt_target` | render-to-texture, non-primary RT, raytraced RT |
| `dynamic_buffers` | `D3DLOCK_DISCARD`/`NOOVERWRITE`, `DrawPrimitiveUP`, `DrawIndexedPrimitiveUP`, negative BaseVertexIndex |
| `texture_formats` | every common D3DFORMAT (DXT1–5, A8R8G8B8, X8R8G8B8, R5G6B5, X1R5G5B5, A1R5G5B5, A4R4G4B4, L8, A8L8, A8, V8U8, L6V5U5, Q8W8V8U8, R16F/R32F…) plus `UpdateTexture`/`UpdateSurface` |
| `vs_sm2` / `vs_sm3` | programmable VS + vertex capture; VS constants in the hash |
| `ff_fog` | vertex and table fog |
| `d3d8_*` twins | the same scenes through D3D8 |
| `remixapi_c` | C API usage (our own app, modelled on the API's documented contract) |

**Independent reference renderer.** Wine's builtin `d3d9.dll` (wined3d on OpenGL/llvmpipe under Xvfb) runs the same apps. Its images are a *second opinion* for the passthrough goldens, compared by FLIP threshold rather than bit-exactly. This catches cases where the FUSE-patched DXVK and plain D3D9 semantics diverge.

### 6.3 CI gates (ctest labels `relight;…`; workflow `.github/workflows/fuse-relight-wine.yml`)

| Gate | Label | What it checks | Where |
|---|---|---|---|
| `rl_licence_text_scan`, `rl_binary_gate` | `gate;licence` | No proprietary NVIDIA text or binaries, and no LGPL headers, in the tree; seeded bad fixtures are caught | Linux, CPU |
| `rl_dxvk_pins` | `gate` | Vendored DXVK/dxbc-spirv/xxhash/imgui/gdeflate sha256 pins; no unmarked edits | CPU |
| `rl_hash_kat_*` | `gate;hash` | Every §4.1 function vs KAT tables and the Python reference (10k random meshes, all formats, all rules) | CPU (Linux + MinGW/Wine) |
| `rl_passthrough_golden_*` | `golden;wine` | Each app through Relight's `d3d9.dll`, tap **off** vs tap **on** → bit-identical; FLIP ≤ 0.02 vs the Wine-builtin reference | Wine + Xvfb + Lavapipe |
| `rl_capture_*` | `wine;capture` | Captured scene (draw classes, categories, instance ids, lights, camera, material records, hashes) equals the expected JSON per app | Wine |
| `rl_d3d8_twin_*` | `wine` | D3D8 twin captures equal the D3D9 captures | Wine |
| `rl_bridge_*` | `wine32` | i686 apps via bridge produce the same goldens and captures as x64; IPC fuzz 1M commands; host-crash recovery | Wine32 (CI installs `wine32` + `g++-mingw-w64-i686-posix`; skip 77 locally) |
| `rl_mod_*` | `mods` | Fixture mods (ours: USDA/USDC, sublayers, references, variants, DDS, `.pkg` + GDeflate) compose as expected; OpenUSD-flatten cross-check when `usd-core` is available | CPU |
| `rl_replace_golden_*` | `golden;wine` | Apps + fixture mods → replaced meshes, materials and lights visible in captures and images; hot reload within one frame | Wine |
| `rl_raster_tier_*` | `golden;wine;tier_t0/t1/t2` | Raster remaster goldens with tier masks (`FUSE_RENDER_TIER_MAX`) | Wine + Lavapipe |
| `rl_pt_parity_*` | `wine;pt` | GPU PT (64×64, N spp) vs `pt_reference`: per-8×8-block mean within 3σ; converged FLIP ≤ 0.05 | Wine + Lavapipe (RT pipeline) |
| `rl_restir_unbiased`, `rl_denoise_*`, `rl_radiance_cache_*` | `pt` | §5.8 gates | Lavapipe + CPU |
| `rl_validation_clean` | `gate;vulkan` | Every Wine GPU test runs with `VK_LAYER_KHRONOS_validation` + sync validation (Windows validation layer staged into the prefix) → 0 messages | Wine |

Budget: the Wine/Lavapipe jobs are small (≤ 128×96, ≤ 64 spp). The whole `relight` label targets under 25 minutes on the GitHub runner, split into 3 jobs.

### 6.4 Hash compatibility: three levels of proof

1. **KATs + Python reference (CI).** These prove the implementation matches the algorithm as written in §4.1, which in turn is derived line by line from the upstream MIT source.
2. **Real Remix parity on the RTX 3090 (nightly or manual, `HW` checklist).** Install the official Remix runtime release (user-installed, never committed). Run the same test apps (x64 and x86 via Remix's bridge). Capture with Remix and with Relight. `Tools/FUSE/Relight/capture_diff` compares the `mesh_*`, `mat_*` and `light_*` name sets and requires 100% equality for FF apps. VS apps are reported separately.
3. **Community mods.** On the user's own games: the replacement hit rate (replacement prims matched per frame) must equal Remix's count shown in its dev menu on the same save.

### 6.5 Path-tracer parity

- **Tiny scenes** captured from the test apps (Cornell-style `ff_lit`, `ff_alpha`) at 32×32 to 64×64.
- **CPU reference:** `pt_reference`, 4k spp; takes about minutes on `CpuParallel`, and the result is cached in `build/` and keyed by the scene hash.
- **GPU under test:** the Relight PT on Lavapipe via Wine at 16–64 spp. Gates:
  - unbiased mode: per-block mean within 3σ;
  - ReSTIR fast mode: bias bound plus variance lower than RIS-only;
  - the denoiser: error vs the reference drops at least 4× over the raw 1-spp input.

### 6.6 Real-GPU validation (user's RTX 3090; manual or nightly; not a CI blocker)

| Check | Target |
|---|---|
| Hash parity vs official Remix (§6.4.2) | 100% FF name-set equality |
| Frame time at 1440p output, DLSS SR Quality or FSR 3.1 Quality, on test scenes and one owned game | ≤ upstream Remix frame time at matched settings; the dual-backend overhead (DXVK + FUSE) ≤ 0.3 ms |
| Quality | FLIP vs a 4k-spp offline Relight reference, compared against Remix's output at the same settings: Relight ≤ Remix |
| Plugins | DLSS SR and DLSS RR (Ampere OK); NRD via WP-6.4b; FG is n/a on Ampere |
| Features | RT pipeline and ray query yes; OMM, cooperative-vector and SER availability on Ampere recorded (driver-dependent) |
| Stability | 2-hour soak in an owned DX9 game: no device loss, bounded VRAM |

---

## 7. Execution waves and work packages

### 7.0 Rules

- **Ownership.** Each work package owns only the paths listed. Shared registration files get **one include line** per package:
  - `Source/FUSE/CMakeLists.txt` → `add_subdirectory(Relight)` once, in RL-0.2;
  - `Source/FUSE/Relight/CMakeLists.txt` → one `include(cmake/relight_<wp>.cmake)` per package.
- **Do not touch** the renderer or remaster files listed in RENDERER-EXECUTION §5.0 or claimed by their packages. Files marked **⚠ coordinate** below are shared and need a hand-off.
- **Landing.** Every package lands with its gate test green, the full ctest suite green, `fuse_vulkan_validation_gate` at 0, and the stub backend still building (EXECUTION-PLAN §0).
- **Estimates** are in agent-days (one PR ≈ 1–3 days).

### Wave R0: foundations (all parallel, no renderer dependency) → **ready to hand out now**

| WP | Title | Owns | Depends | Deliverables | Exit criteria | Est. |
|---|---|---|---|---|---|---|
| **RL-0.1** | Licence and attribution gates | new `Source/FUSE/Relight/THIRD_PARTY.md`, `Source/FUSE/Relight/cmake/relight_licence_gates.cmake`, `Source/FUSE/Relight/cmake/relight_proprietary_scan.cmake`, `Source/FUSE/Relight/tests/licence/**`; ⚠ coordinate a pattern addition in `plugins/nvidia/cmake/nvidia_binary_gate.cmake` (or a separate `relight_binary_gate.cmake`; preferred, so no shared edit) | – | Text scan (patterns assembled from fragments); binary patterns (§0.4.4); LGPL-header scan (`GNU Lesser General Public` in `*.h` outside `build/`); THIRD_PARTY skeleton with all planned entries | Seeded bad and good fixtures (generated at test time) behave as expected; the current tree passes; ctest `rl_licence_text_scan`, `rl_binary_gate` (`gate;licence`) | 1.5 |
| **RL-0.2** | Vendor upstream DXVK 3.1.1 subset + dxbc-spirv, with a CMake build | new `Engine/lib/dxvk/**` (`src/{d3d8,d3d9,dxvk,util,wsi,spirv,vulkan}`, the DXVK shader GLSL, `LICENSE`, `VERSION` with per-file sha256, `PATCHES.md` (empty)), new `Engine/lib/dxbc-spirv/**`, new `Source/FUSE/Relight/CMakeLists.txt`, `Source/FUSE/Relight/cmake/relight_dxvk.cmake`, `Tests/relight/smoke/create_device.c`; ⚠ one line in `Source/FUSE/CMakeLists.txt` | – | CMake port of DXVK's meson (generated `version.h`/`config.h`, glslang shader-to-header rules, `.def` exports, per-target warning suppressions, `FUSE_BUILD_RELIGHT` option default ON only for the MinGW preset); x64 `d3d9.dll`, `d3d8.dll`; no d3d10/11/dxgi | PE32+ exports check (`Direct3DCreate9`, `Direct3DCreate9Ex`, `Direct3DCreate8`); `create_device.c` under `xvfb-run` + Wine + Lavapipe creates a device, clears, presents and reads back the expected colour (ctest `rl_dxvk_smoke`, skip 77 without Xvfb/Wine); pin lint `rl_dxvk_pins` | 4 |
| **RL-0.3** | Wine/Xvfb runner, i686 toolchain, CI workflow | new `cmake/toolchains/fuse-wine-xvfb-run.sh`, new `cmake/toolchains/mingw-w64-i686.cmake`, new `.github/workflows/fuse-relight-wine.yml`, new `Source/FUSE/Relight/tests/env/probe_vulkan.c` | – | Runner: one Xvfb per ctest job (flock), `VK_ICD_FILENAMES` → lvp, validation layer staged into the prefix, exit codes passed through; i686 toolchain; workflow installs `wine32` (multiarch), `g++-mingw-w64-i686-posix`, `xvfb`, `mesa-vulkan-drivers` | `rl_env_probe_x64`: the Windows PE sees llvmpipe with the RT extensions (as verified in §6.1); `rl_env_probe_x86` passes in CI, skips 77 here; the workflow is green on a PR | 2 |
| **RL-0.4** | D3D9/D3D8 test-app kit | new `Tests/relight/apps/**`, `Tests/relight/apps/common/{rl_app.h,rl_dump.c}`, `Source/FUSE/Relight/cmake/relight_apps.cmake` | RL-0.3 runner (can use `xvfb-run` directly until it lands) | The §6.2 app list (x64; i686 when the toolchain exists); a raw RGBA + JSON sidecar dump; reference runs with Wine's builtin d3d9 produce reference images (FLIP metric from `renderer/quality`) | All apps run on Wine-builtin d3d9 and produce stable dumps (two runs byte-identical); sidecars validate against a schema | 4 |
| **RL-0.5** | Remix-compatible hash library | new `Engine/lib/xxhash/**` (0.8.x pinned), new `Source/FUSE/Relight/hash/**` (pure logic, no Vulkan or D3D headers; its own `D3DFORMAT` enum values), new `Tools/FUSE/Relight/remix_hash_ref.py`, new `Source/FUSE/Relight/tests/hash/**` | – | Every §4.1 function: components, rules, legacy (SSE semantics with an exact scalar emulation of `_mm_round_ps` FLOOR after multiplying by the reciprocal), the texture layout table for every D3DFORMAT, descriptor hash, light hash, hex formatting and parsing | `rl_hash_kat_*`: xxHash official vectors; KAT tables; C++ vs Python on 10k random cases per function (Python skipped if the wheel is unavailable); property tests (rule order, empty components); MinGW/Wine run gives identical results | 3 |
| **RL-0.6** | Options system (rtx.conf-compatible) | new `Source/FUSE/Relight/options/**`, `Source/FUSE/Relight/tests/options/**` | – | Port of `rtx_option*` (MIT): typed options, sparse layers (defaults, rtx.conf, dynamic, user.conf, presets), hash lists with `-` removal, env vars, `relight.*` ↔ `rtx.*` alias table, docs generator (`docs/relight-options.md` generated at build, not committed) | Ported semantics of upstream `test_rtx_option.cpp` / `test_option_layer_export.cpp` (rewritten as FUSE gtests); round-trip write of rtx.conf is byte-stable | 3 |
| RL-0.7 | FUSE renderer on MinGW with Vulkan | new `Source/FUSE/Relight/cmake/relight_renderer_mingw.cmake`; ⚠ coordinate `CMakePresets.json` (new `fuse-mingw-relight-*` presets) and `cmake/FuseFindVulkan.cmake` (volk) | renderer WP-0.2 (volk) | `fuse_renderer` builds as PE with volk, no import lib needed; shaders compiled on the host | `rp_device_tiers` and `rp_golden_gbuffer_roundtrip` run as PE under the Xvfb runner on Lavapipe (T2 reported) | 3 |

### Wave R1: interception and capture (needs RL-0.2, 0.4, 0.5; RL-1.6 also needs RL-0.7)

| WP | Title | Owns | Depends | Deliverables | Exit criteria | Est. |
|---|---|---|---|---|---|---|
| RL-1.1 | Tap seam + device import | `Engine/lib/dxvk/src/d3d9/**` and `src/dxvk/dxvk_{instance,adapter}.*` (patches only, recorded in `PATCHES.md`; ownership of `Engine/lib/dxvk` passes from RL-0.2 to RL-1.1), new `Source/FUSE/Relight/tap/**` | 0.2, 0.4 | `IRelightTap` (§2.4), a null tap, a recording mock tap, and device import from a FUSE-created VkDevice (plain Vulkan bootstrap until 0.7) | Tap off: `rl_passthrough_golden_*` bit-identical to RL-0.2; recording tap: the event stream per app equals the expected JSON; validation clean with an imported device; ≤ 30 marked blocks (lint counts them) | 5 |
| RL-1.2 | Draw classifier and heuristics | `Relight/scene/classify/**` | 0.6, 1.1 | Port of `makeDrawCallType`, `isRenderingUI`, sky (explicit + auto), terrain, stencil shadow, shadow mask, raytraced-RT, `POSITIONT`, draw-call range, and categories from texture lists (`setupCategoriesForTexture`) | Unit tests per rule on a synthetic `D3DStateModel`; `sky_ui_hud`, `stencil_shadow`, `shadow_mask`, `rt_target` capture expectations | 3 |
| RL-1.3 | Geometry extraction + hashing + skinning | `Relight/capture/geometry/**` | 0.5, 1.1 | Index rebase with memoization, stream slicing by usage, `texcoordIndex` selection, async hash/AABB jobs (JobScheduler), FF skinning data, UP draws | `rl_capture_*` geometry hashes equal `remix_hash_ref.py` on every app (u16/u32/non-indexed, negative base vertex, UP) | 4 |
| RL-1.4 | Texture tracking + hash on upload | `Relight/capture/texture/**` | 0.5, 1.1 | Canonical repack, first-upload hashing, Update* inheritance, recompute-on-write, RT descriptor hash, external image registry (pre-bindless) | `texture_formats` app: every format's hash equals the reference; inheritance cases correct | 3 |
| RL-1.5 | FF state translation: material, lights, fog, camera | `Relight/scene/{translate,lights,camera}/**` | 1.2 | `setLegacyMaterialState`/`setFogState` semantics, D3DLIGHT9 → light records + light hash, camera decomposition (FOV, near/far, jitter detection), camera classification | `ff_lit`, `ff_alpha`, `ff_fog` capture expectations; camera KATs ≤ 1e-5 | 4 |
| RL-1.6 | Vertex capture SPIR-V pass | `Relight/capture/vertex_capture/**` | 1.1, 0.7 | SPIR-V transform (SPIRV-Headers only), 48-byte capture layout, CPU back-transform, VS-constant hash component | `vs_sm2`/`vs_sm3`: object-space positions within 1e-4 of the CPU transform; `spirv-val` clean; the VS hash matches the reference | 5 |
| RL-1.7 | Instance tracking and scene model | `Relight/scene/instances/**` | 1.3, 1.5 | `processSceneObject` semantics (same-BLAS nearest within `uniqueObjectDistance`), stable ids, previous transforms, anti-culling and foliage bookkeeping, point instancer | `ff_multi_instance`: ids stable over 60 frames; teleport → new id; per-instance motion equals analytic motion | 4 |
| RL-1.8 | Capture writers (FUSE POCO + Remix-compatible USDA) | `Relight/capture/export/**`, `Tools/FUSE/Relight/capture_diff.*` | 1.7; Remaster W0.1–W0.3 (else a JSON store shim inside this path) | POCO + `hash_key` rows (§4.1.5); USDA writer (§1.9 layout); DDS writer for captured textures (BC passthrough) | Every app → capture → re-ingest → identical key set; USDA parses with TinyUSDZ (or our parser if W2.2 is late); `capture_diff` self-test | 4 |
| RL-1.9 | D3D8 coverage | `Tests/relight/apps/d3d8_*`, `Relight/tests/d3d8/**` | 1.3–1.5 | D3D8 twin apps; tap coverage checks | `rl_d3d8_twin_*` captures equal the D3D9 twins | 2 |

### Wave R2: bridge for 32-bit games (after RL-1.1; parallel with R1 and R3)

| WP | Title | Owns | Depends | Deliverables | Exit criteria | Est. |
|---|---|---|---|---|---|---|
| RL-2.1 | IPC core | `Relight/bridge/ipc/**`, `Relight/bridge/schema/**` (command table + generator) | – | Shared-memory rings, blocking and atomic queues, shared heap; Win32 and POSIX backends; versioned handshake; generated encoders and decoders | Linux-native and Wine tests: 1M-command fuzz round trip; back-pressure; peer-death detection < 1 s | 4 |
| RL-2.2 | Client proxies (x86 `d3d9.dll`/`d3d8.dll`) | `Relight/bridge/client/**` | 2.1 | Full COM surface ported from `bridge/src/client` onto the generated schema; window and input handling (`di_hook` semantics) | All i686 apps through the bridge: goldens and captures equal the x64 builds (`rl_bridge_*`, Wine32) | 8 |
| RL-2.3 | Host + launcher | `Relight/bridge/{host,launcher}/**` | 2.1, 1.1 | `fuse_relight_host.exe` (x64) hosting DXVK + Relight; launcher (suspended process + LoadLibrary injection; refuses known anti-cheat processes); crash fallback to passthrough | Host-crash test: the game keeps running in plain DXVK mode (x86 build of vendored DXVK as a fallback); launcher self-test | 4 |

### Wave R3: replacements and mods (after RL-0.5, 0.6; Remaster W0.1–0.3 and W2.2)

| WP | Title | Owns | Depends | Deliverables | Exit criteria | Est. |
|---|---|---|---|---|---|---|
| RL-3.1 | USD reader (Remix profile) | `Relight/mods/usd/**`, `Tests/relight/fixtures/mods/**` (ours), `Tools/FUSE/Relight/usd_flatten_check.py` | Remaster W2.2 (TinyUSDZ) | Composition subset (§4.3), schemas (categories, particles), prim classification (`mesh_`/`mat_`/`light_`) | `rl_mod_*` on 20+ handcrafted fixtures (USDA and USDC); cross-check with `usd-core` flattening where installed | 5 |
| RL-3.2 | Mod importer → POCO + DB | `Relight/mods/import/**`, `Tools/FUSE/Relight/fuse_relight_import.cpp` | 3.1, 3.3 | Mesh, material (§4.5 table) and light mapping; `material_ext` records; licence and provenance per §4.8 | Fixture mods → expected POCO JSON; the material table round-trips every parameter; DB export is deterministic | 5 |
| RL-3.3 | DDS, `.pkg` and GDeflate | `Relight/mods/assets/**`, new `Engine/lib/gdeflate/**` | – | DDS reader (§4.6), package reader, GDeflate CPU decode | Decode vs expected for all formats; GDeflate reference round trip; malformed-file fuzz (1k cases) | 3 |
| RL-3.4 | Runtime replacement engine | `Relight/replace/**` | 1.7, 3.2 | Per-draw lookup, `preserveOriginalDrawCall`, material by texture hash, light replace/delete/attach, mod stacking, per-mod rtx.conf layers, hot reload, texture residency policy | `rl_replace_golden_*`: fixture mods on the apps change the captured scene as expected; hot-reload latency ≤ 1 frame after the notification | 5 |
| RL-3.5 | Logic graphs | `Relight/logic/**` | 3.1, 0.6 | Component runtime (~60 components: sense, transform, action, const), USD graph parser, option-layer actions | Semantics tests ported from `tests/rtx/unit/graph` and `test_transform_components.cpp` (rewritten); a fixture graph toggles an option layer | 5 |
| RL-3.6 | Particle system | `Relight/particles/**` | 3.1 | USD `ParticleSystemAPI`, GPU sim + CPU kernel reference, billboard generation | CPU/GPU sim parity (bit-exact CPU backends; ≤ 1e-4 GPU) | 4 |

### Wave R4: rendering bring-up (needs renderer WP-0.3, 0.4, 1.1; T2+ also needs WP-6.0)

| WP | Title | Owns | Depends | Deliverables | Exit criteria | Est. |
|---|---|---|---|---|---|---|
| RL-4.1 | Frame orchestration, GPU-scene adapter, composite | `Relight/render/frame/**` | 0.7, 1.7, WP-0.3/0.4/1.1 | External-image bindless registration, injection and composite (§2.3), timeline sync with DXVK's queue callback, passthrough texture swap | Passthrough composite bit-identical to tap-off; composite of a solid FUSE image visible below the HUD; sync validation clean | 5 |
| RL-4.2 | Raster remaster (T0–T2) | `Relight/render/raster/**`, `Relight/shaders/raster/**` | 4.1, 4.3, WP-2.1/3.x/6.1 (degrades gracefully if absent: G-buffer + clustered only) | Legacy-material shading in the FUSE deferred path, forward blended pass, decals, fog | `rl_raster_tier_*` goldens at T0/T1/T2 masks for `ff_lit`, `ff_alpha`, `sky_ui_hud` | 6 |
| RL-4.3 | Relight material and BSDF model | `Relight/render/material/**`, `Relight/shaders/material/**`, `Relight/kernels/bsdf_*` | WP-0.5 (Slang; GLSL fallback) | Port of the MIT surface-material shaders (§5.2), including SSS, hair and thin film from papers | White furnace, reciprocity, χ², GPU/CPU parity (§5.8) | 6 |
| RL-4.4 | Light model + light-tree integration | `Relight/render/lights/**`, `Relight/kernels/light_*` | WP-7.1 | Light types, shaping, emissive triangles, D3D light conversion on the GPU | pdf integrates to 1; tree sampling vs brute force; GPU/CPU parity | 4 |

### Wave R5: path tracer (T3). Implements or extends renderer WP-6.4, 7.2 and 7.3 to the §5 specification

| WP | Title | Owns | Depends | Deliverables | Exit criteria | Est. |
|---|---|---|---|---|---|---|
| RL-5.1 | PT core + primary/PSR + CPU reference PT | `Relight/render/pathtrace/**`, `Relight/kernels/pt_reference*` (the generic integrator stays in WP-7.3's `src/pathtrace/**` if claimed) | 4.3, 4.4, WP-6.0 | RT-pipeline and ray-query paths, PSR, portals, alpha semantics, view model, accumulation reference mode | `rl_pt_parity_*` (unbiased, 3σ) on 3 captured scenes | 8 |
| RL-5.2 | ReSTIR DI (§5.3) | WP-7.2 paths (`src/restir/**`, `shaders/restir/**`) or `Relight/render/pathtrace/restir_di*` if WP-7.2 is claimed with a different scope | 5.1, 4.4 | Light tiles, GRIS pairwise MIS, visibility reuse, gradients, unbiased and fast modes | `rl_restir_unbiased` (mean within 3σ); variance < RIS-only at equal samples | 7 |
| RL-5.3 | ReSTIR GI → ReSTIR PT | as 5.2 | 5.1, 5.2 | GI reservoirs and reuse; PT hybrid shift ("Ultra") | Unbiased mean within 3σ; variance-per-ms tracked (HW) | 13 |
| RL-5.4 | Radiance cache (+ neural research track) | `Relight/render/pathtrace/radiance_cache*`, `Relight/kernels/radiance_cache*`, `research/relight_nrc/**` | 5.1 | Hash grid (default); neural cache prototype with a CPU MLP reference | Bias bound vs reference; cache hit-rate metrics; neural track reports only (HW) | 5 + 6 |
| RL-5.5 | Denoiser to the §5.5 spec | WP-6.4 paths (`src/denoise/**`, `shaders/denoise/**`) | 5.1, WP-4.1 | Temporal, specular virtual motion, A-SVGF gradients, à-trous, history fix, shadow and volumetric paths; plugin switch to NRD and DLSS-RR | `rl_denoise_*`: error reduction ≥ 4×; per-pass CPU parity; plugin mocks | 8 |
| RL-5.6 | Volumetrics, particles render, terrain baking, OMM | `Relight/render/volumetrics/**`, `Relight/terrain/**`, `Relight/render/pathtrace/omm/**` | 5.1, WP-8.1, 3.6 | §5.7 | Convergence and ghosting metrics; terrain bake golden; OMM on the HW checklist only | 6 |
| RL-5.7 | Post and upscale wiring | `Relight/render/post/**` | WP-4.1/4.2/4.5, `plugins/nvidia` | Demodulate/composite, local TM Look node, upscaler selection, mip bias | Upscaler switch test (no NaNs, validation clean); post parity | 3 |

### Wave R6: tools, UX and API

| WP | Title | Owns | Depends | Deliverables | Exit criteria | Est. |
|---|---|---|---|---|---|---|
| RL-6.1 | In-game developer overlay | new `Engine/lib/imgui/**`, `Relight/overlay/**` | 4.1, 0.6 | Port of the dev menu: texture tagging to rtx.conf layers, option layers, debug views, capture button, replacement stats, graph inspector | UI state-machine tests; the overlay draws on the Wine golden without disturbing passthrough when hidden | 6 |
| RL-6.2 | C API (FUSE-native + Remix API 0.6 compat) | `Relight/api/**`, `Tests/relight/apps/remixapi_c` | 1.7, 3.4, 2.1 | `fuse_relight_*` API; compat exports (`remixapi_InitializeLibrary` and the function table per the MIT `remix_c.h`); bridge forwarding | The `remixapi_c` app renders the expected scene (x64 direct, x86 via bridge) | 5 |
| RL-6.3 | Game-setup assistant and per-game profiles | `Relight/scene/setup/**`, `Content/relight/profiles/**` (small JSON, ours) | 1.2, 6.1 | Suggestions: UI/sky/particle texture candidates, camera sanity, hash-rule choice; profile DB (hash lists and options, no game data) | Suggestions reproduce the known categories on the apps | 3 |
| RL-6.4 | Packaging, docs, plugin discovery | `Relight/cmake/relight_package.cmake`, new `docs/relight.md` | all | Dist layout (`d3d9.dll`, `d3d8.dll`, `fuse_relight_host.exe`, bridge client, `THIRD_PARTY_NOTICES.txt`); `FUSE_NVIDIA_SDK_DIR` plugin discovery | Package manifest test; binary and licence gates on the package tree | 2 |

### Wave R7: hardware validation (RTX 3090; manual or nightly; `HW` checklist)

| WP | Title | Exit |
|---|---|---|
| RL-7.1 | Hash parity vs official Remix (§6.4.2) | 100% FF name-set equality; VS report |
| RL-7.2 | Performance and quality (§6.6) | Budgets met or tracked with profiles |
| RL-7.3 | Real games + community mods (user-owned) | Replacement hit rate equals Remix's; 2-hour soak |

### 7.1 Critical path and schedule

```
R0: 0.1 ∥ 0.2 ∥ 0.3 ∥ 0.4 ∥ 0.5 ∥ 0.6        (+0.7 after renderer WP-0.2)
R1: 1.1 → {1.2, 1.3, 1.4} → 1.5 → 1.7 → 1.8 ; 1.6 (needs 0.7) ; 1.9
R2: 2.1 → 2.2 ∥ 2.3                          (parallel with R1 after 1.1)
R3: 3.3 ∥ 3.1 → 3.2 → 3.4 ; 3.5, 3.6          (parallel with R1/R2)
R4: 4.1 → 4.2 ; 4.3 ∥ 4.4                     (needs renderer Wave 0/1)
R5: 5.1 → 5.2 → 5.3 ; 5.4, 5.5, 5.6, 5.7
R6: 6.1–6.4 ; R7 on HW whenever the user runs it
```

Totals: R0 ≈ 20.5, R1 ≈ 34, R2 ≈ 16, R3 ≈ 27, R4 ≈ 21, R5 ≈ 56, R6 ≈ 16 agent-days, so about **190 agent-days**. With 5–6 parallel agents that is roughly **10–12 calendar weeks**. The path tracer (R5) is paced by renderer Waves 0–1 and WP-6.0.

**Milestones:**
- **M-R1 "Passthrough + capture"** (end of R1): any D3D8/9 game runs, and FUSE captures with Remix hashes.
- **M-R2 "Mods"** (R3 + R4.1): Remix mods load and render in passthrough/raster.
- **M-R3 "32-bit"** (R2).
- **M-R4 "Traced"** (R5): the T3 path tracer.
- **M-R5 "Ship"** (R6 + R7).

### 7.2 First packages ready to hand to agents

These have no mutual file overlap and none depends on another's code to start:

1. **RL-0.1 Licence gates.**
   - Owns: `Source/FUSE/Relight/THIRD_PARTY.md`, `Source/FUSE/Relight/cmake/relight_licence_gates.cmake`, `relight_proprietary_scan.cmake`, `Source/FUSE/Relight/tests/licence/**`.
   - Exit: seeded fixtures fail and pass as expected; the tree is clean; ctest `rl_licence_text_scan`, `rl_binary_gate`.
2. **RL-0.2 Vendor DXVK 3.1.1 + dxbc-spirv (CMake build).**
   - Owns: `Engine/lib/dxvk/**`, `Engine/lib/dxbc-spirv/**`, `Source/FUSE/Relight/CMakeLists.txt`, `cmake/relight_dxvk.cmake`, `Tests/relight/smoke/**`, plus one line in `Source/FUSE/CMakeLists.txt`.
   - Exit: PE exports present; `rl_dxvk_smoke` (device + clear + readback under Xvfb/Wine/Lavapipe); pin lint.
3. **RL-0.3 Wine/Xvfb runner, i686 toolchain, CI.**
   - Owns: `cmake/toolchains/fuse-wine-xvfb-run.sh`, `cmake/toolchains/mingw-w64-i686.cmake`, `.github/workflows/fuse-relight-wine.yml`, `Source/FUSE/Relight/tests/env/**`.
   - Exit: `rl_env_probe_x64` sees llvmpipe with the RT extensions; the x86 probe is green in CI and skips 77 locally.
4. **RL-0.4 Test-app kit.**
   - Owns: `Tests/relight/apps/**`, `Source/FUSE/Relight/cmake/relight_apps.cmake`.
   - Exit: all §6.2 apps run on Wine-builtin d3d9 with byte-stable dumps and schema-valid sidecars.
5. **RL-0.5 Hash library.**
   - Owns: `Engine/lib/xxhash/**`, `Source/FUSE/Relight/hash/**`, `Tools/FUSE/Relight/remix_hash_ref.py`, `Source/FUSE/Relight/tests/hash/**`.
   - Exit: `rl_hash_kat_*` (official xxHash vectors, KATs, 10k-case C++ vs Python per function, identical under Wine).
6. **RL-0.6 Options system.**
   - Owns: `Source/FUSE/Relight/options/**`, `Source/FUSE/Relight/tests/options/**`.
   - Exit: layering semantics tests; byte-stable rtx.conf round trip.

`Source/FUSE/Relight/CMakeLists.txt` is created by RL-0.2. Packages that land before it add their `cmake/relight_*.cmake` file and a temporary `include()` guarded by `if(EXISTS)`. RL-0.2 folds the guards in.

---

## 8. Risks and adopted decisions

### 8.1 Adopted decisions (made here; no user decision needed)

| # | Decision | Rationale |
|---|---|---|
| AD-1 | Vendor **upstream DXVK 3.1.x** d3d8/d3d9 + backend (zlib) and dxbc-spirv (MIT). Do not use the Remix fork. Do not rewrite. | §2.2 |
| AD-2 | FUSE creates the Vulkan instance and device; DXVK imports them | One device, with the RT feature chain from `RendererCaps`; upstream supports import |
| AD-3 | DXVK's backend stays the raster executor for game and UI draws; FUSE's RHI renders the remaster | Avoids rewriting d3d9 on the FUSE RHI; revisit only if the measured overhead is > 0.3 ms |
| AD-4 | 32-bit games through a revamped port of the Remix bridge with a generated schema | 32-bit address space cannot hold a path tracer and HD assets; generation removes 370 hand-written cases |
| AD-5 | Bit-exact Remix hashes, including legacy rules, SSE rounding semantics and per-game rule strings | Community mod compatibility (G2) |
| AD-6 | TinyUSDZ at runtime (shared with Remaster W2.2) with a Remix-profile composition; OpenUSD only as an optional offline flatten and CI cross-check | Small, permissive, no heavy dependency; the cross-check covers composition gaps |
| AD-7 | MDL is never compiled or vendored; parameters are mapped from the MIT tables | MDL files are proprietary; the runtime never needed MDL semantics |
| AD-8 | ReSTIR DI/GI/PT, light tree, radiance cache, denoiser and SSS/hair are FUSE's own, from papers; NRD, DLSS-RR and DLSS SR are plugins only | Licence rules plus best quality; plugins stay optional |
| AD-9 | Clean-room rule for the five proprietary shader files and the RTXDI/RTXCR/NRC/NRD SDKs, enforced by the text-scan gate and a PR checklist line | Keeps FUSE free of licence contamination |
| AD-10 | Relight shaders in Slang once WP-0.5 lands; GLSL until then | Renderer decision; the Remix shaders are Slang, so porting is closer |
| AD-11 | Read `rtx.conf`, `user.conf` and `dxvk.conf` with `rtx.*` keys; the native namespace is `relight.*` with an alias table | Compatibility without adopting NVIDIA naming for our own options |
| AD-12 | FUSE-native C API plus Remix API 0.6 compat exports | Engines and source ports already written against `remix_c.h` work unchanged |
| AD-13 | Product and binary names are FUSE-native ("FUSE Relight", `fuse_relight_host.exe`); only the system DLL names `d3d9.dll`/`d3d8.dll` are fixed | Trademark hygiene (§0.4.5) |
| AD-14 | Dear ImGui (vendored upstream) for the in-game overlay; the Qt editor (Remaster workspace) for authoring | An in-game overlay cannot be Qt; ImGui is the Remix UX users know |
| AD-15 | D3D8 via DXVK's native d3d8 (not d3d8to9) | Maintained, same licence family, one code path |
| AD-16 | Compatibility baseline dxvk-remix @0867d3c / API 0.6. Rebase review every quarter: re-run the hash KAT generator against upstream `rtx_hashing.cpp` / `d3d9_rtx_geometry.cpp` diffs | Remix changes fast (Remaster risk table) |
| AD-17 | GDeflate CPU decode from the Apache-2.0 reference for `.pkg`/`.rtxio` | rtxio is proprietary |
| AD-18 | No CUDA in the Relight runtime (Vulkan only). CUDA stays in FUSE's kernel framework for other modules; tiny-cuda-nn only as an offline research oracle | One GPU API in a DLL injected into games |
| AD-19 | Remaster track A2 becomes this plan; Remaster Wave 7 is superseded; Remaster D2 (Remix external) still holds for users who prefer NVIDIA's runtime | Keeps the plans consistent without editing the Remaster doc |
| AD-20 | Test apps are our own C code; no game content in CI; fixture mods are handcrafted by us | Remaster §0.1 IP policy |

### 8.2 Risks

| # | Risk | Impact | Mitigation |
|---|---|---|---|
| R1 | Upstream DXVK churn breaks the patch set | Rebase cost | ≤ 30 marked blocks, pinned tag, `PATCHES.md`, and a quarterly rebase with the passthrough goldens as the gate |
| R2 | Remix changes its hashing | Mods built for new Remix versions miss | AD-16 review; KAT generator; multi-key DB (Remaster §1.3); per-game Remix-version pin |
| R3 | VS-draw hashes depend on shader-analysis details (`maxConstIndex*`) that differ between the fork's dxso and dxbc-spirv | VS-heavy games miss some mesh replacements | Port the fork's constant-range analysis as a pure function over DXSO bytecode, independent of the translator; report VS parity separately (§6.4) |
| R4 | winevulkan or Wine differences from Windows (window, present, timing) | CI false positives or negatives | 3090 checklist on Windows; the Wine-builtin d3d9 reference; keep app timing-free |
| R5 | Lavapipe RT is very slow | CI time | ≤ 64×64 PT scenes, spp caps, cached CPU references |
| R6 | Dual Vulkan backends in one process (memory, queue contention) | Performance | Shared device and queue lock; VMA budget includes DXVK usage via `VK_EXT_memory_budget`; measured on the 3090 (AD-3 threshold) |
| R7 | Bridge complexity (input, windows, timing) | 32-bit instability | Generated schema, fuzzing, crash fallback to plain DXVK |
| R8 | USD composition gaps in the TinyUSDZ-based reader | Some mods load wrong | Remix-profile tests; `usd-core` flatten cross-check tool for users |
| R9 | Clean-room contamination (someone pastes RTXDI code) | Licence breach | Text-scan gate; PR checklist; algorithm notes cite papers only |
| R10 | Anti-cheat or online games | Bans | Launcher refusal list; docs; no injection into flagged processes |
| R11 | Path-tracer quality falls short of Remix at first | User expectations | Unbiased reference gates first; DLSS-RR plugin available from day one of R5; HW comparison in RL-7.2 |
| R12 | Scope: about 190 agent-days | Schedule | Milestones are independently useful (M-R1 capture-only already beats Remaster W7) |

### 8.3 External blockers (outside this repo; each has a CI-safe fallback)

1. **RTX 3090 access** for the `HW` checklist (§6.6, Wave R7). The user has one. It needs Windows, or Linux with Proton/Wine, plus a current NVIDIA driver.
2. **The official Remix runtime installed by the user** on that machine, for hash parity (§6.4.2). It is never committed.
3. **User-owned games and community mods** for real-world validation (RL-7.3), per the Remaster IP policy.
4. **NVIDIA SDK runtimes** (DLSS/Streamline, NRD), downloaded by the user into `FUSE_NVIDIA_SDK_DIR` for the plugin checks.
5. **CI runner packages**: `wine32` (multiarch), `g++-mingw-w64-i686-posix`, `xvfb`. These are available on `ubuntu-24.04` runners and installed by RL-0.3's workflow.

---

## 9. Sources

**Upstream code** (commits in the header):
- `dxvk-remix`: `LICENSE`, `LICENSE-MIT`, `ThirdPartyLicenses.txt`, `.gitmodules`, `packman-external.xml`, `meson.build`, `AGENTS.md`, `documentation/{RemixConfig,RemixLogic,TerrainSystem,AntiCullingSystem,FoliageSystem}.md`, `RtxOptions.md`;
- `src/d3d9/{d3d9_rtx.cpp,d3d9_rtx_geometry.cpp,d3d9_rtx_utils.cpp,d3d9_common_texture.cpp,d3d9_device.cpp}`;
- `src/dxvk/rtx_render/{rtx_hashing.*,rtx_types.*,rtx_options.h,rtx_mod_usd.cpp,rtx_mod_manager.cpp,rtx_material_data.h,rtx_materials.h,rtx_lights.cpp,rtx_light_manager.cpp,rtx_asset_package.h,rtx_asset_data_manager.cpp,rtx_io.cpp,rtx_game_capturer.cpp,rtx_utils.h,rtx_instance_manager.cpp,rtx_scene_manager.*}`;
- `src/lssusd/{game_exporter_common.h,game_exporter_paths.h}`, `src/mdl/**` (headers only, for licence), `src/dxvk/shaders/rtx/**` (tree and headers), `bridge/{README.md,src/util/util_commands.h}`, `public/include/remix/remix_c.h`;
- `doitsujin/dxvk` 3.1.1: `LICENSE`, `.gitmodules`, `meson.build`, `src/d3d8/**`, `src/d3d9/{d3d9_shader.cpp,d3d9_fixed_function.cpp,d3d9_interfaces.h}`, `src/dxvk/{dxvk_instance.h,dxvk_adapter.h}`, `src/d3d11/d3d11_main.cpp`, `src/wsi/**`;
- `doitsujin/dxbc-spirv` `LICENSE` (MIT);
- `Joshua-Ashton/mingw-directx-headers` README (LGPL-2.1+).

**Licences fetched:**
- RTXDI (`remix` and `main`), NRD, DLSS, RTXCR, Neural-Radiance-Cache, RTXGI, RTXPT: NVIDIA RTX SDKs License;
- nvapi: MIT; Detours: MIT; XeSS: Intel Simplified Software License; sse2neon: MIT;
- Streamline: MIT headers; d3d8to9: BSD-2; MDL-SDK: BSD-3 (the SDK; the Remix `.mdl` files are separately proprietary);
- Slang: Apache-2.0 WITH LLVM-exception; OpenUSD: TOST; `rtx-remix` umbrella: MIT; `toolkit-remix`: Apache-2.0;
- Donut: MIT; xxHash: BSD-2; tiny-cuda-nn: BSD-3; TinyUSDZ: Apache-2.0; Dear ImGui: MIT;
- DirectStorage: MIT, with its GDeflate codec under Apache-2.0.

**FUSE documents:**
- `docs/plans/{FUSE_RENDERER_PLAN,FUSE_REMASTER_PLAN,FUSE_ASSET_PLAN}.md`, `docs/unification/RENDERER-EXECUTION.md`, `docs/compute-kernels.md`, `docs/nvidia-plugin.md`;
- `cmake/toolchains/{mingw-w64-x86_64.cmake,fuse-wine-run.sh}`, `.github/workflows/fuse-windows-cross.yml`;
- `Source/FUSE/Renderer/{include/fuse/renderer/rg/graph.hpp,include/fuse/renderer/upscale/*,plugins/nvidia/cmake/nvidia_binary_gate.cmake}`, `Engine/lib/vma/VERSION`.

**Papers** (the implementation basis for §5):
- Bitterli et al., "Spatiotemporal reservoir resampling for real-time ray tracing with dynamic direct lighting", SIGGRAPH 2020.
- Wyman & Panteleev, "Rearchitecting spatiotemporal resampling for production", HPG 2021.
- Ouyang et al., "ReSTIR GI: Path resampling for real-time path tracing", HPG 2021.
- Lin et al., "Generalized resampled importance sampling: foundations of ReSTIR", SIGGRAPH 2022.
- Estevez & Kulla, "Importance sampling of many lights with adaptive tree splitting", HPG 2018.
- Conty Estevez & Kulla, "Importance sampling of many lights" (light trees), 2018.
- Müller et al., "Real-time neural radiance caching for path tracing", SIGGRAPH 2021.
- Binder et al., "Massively parallel path space filtering", 2019.
- Gautron, "Real-time ray-traced ambient occlusion of complex scenes using spatial hashing", 2020.
- Schied et al., "Spatiotemporal variance-guided filtering", HPG 2017.
- Schied et al., "Gradient estimation for real-time adaptive temporal filtering", 2018.
- Zhdan, "ReBLUR: a hierarchical recurrent denoiser", *Ray Tracing Gems II* ch. 49, 2021.
- Kulla & Conty, "Revisiting physically based shading at Imageworks", 2017.
- Belcour & Barla, "A practical extension to microfacet theory for the modeling of varying iridescence", 2017.
- Christensen & Burley, "Approximate reflectance profiles for efficient subsurface scattering", 2015.
- Chiang et al., "A practical and controllable hair and fur model for production path tracing", 2016.
