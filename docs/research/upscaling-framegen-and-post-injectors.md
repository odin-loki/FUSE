# Upscaling, frame generation and post-process injectors: research and a plan for FUSE

*Research date: 2026-09-23. Every source was accessed on 2026-09-23. Citations such as [S12] point to the
numbered list in the [Sources](#sources) section at the end.*

> Scope: NVIDIA DLSS 5 and the DLSS/Streamline stack, ENBSeries, related upscalers, frame generation,
> latency, denoisers, injectors, middleware and neural-rendering trends. It ends with a recommended
> architecture and a prioritised plan for FUSE. FUSE is AGPL-3.0-licensed (`LICENSE.md`). Its renderer already
> has a same-resolution CPU-reference TAA (`Source/FUSE/Renderer/include/fuse/renderer/taa/`) and a CPU
> reference post chain: bloom, DoF, motion blur, tone map, grade and grain
> (`.../postprocess/post_stack.hpp`). Compute work goes through the single-source kernel model
> (`docs/compute-kernels.md`).

---

## 0. Executive summary

1. **"DLSS 5" is real, and it is not an upscaler.** NVIDIA announced it at GTC on **2026-03-16** as a
   "real-time neural rendering model that infuses pixels with photoreal lighting and materials" [S1][S2].
   It shipped on **2026-09-03** in NBA 2K27, on RTX 50-series GPUs only [S3][S5]. NVIDIA markets it as
   **"3D-Guided Neural Rendering"**. Its inputs are the frame colour and motion vectors, it is strictly
   one frame in, one frame out, and developers control it through model selection, *Structure
   Intensity*, *Tone Intensity*, semantic AI masks and engine masks [S5][S6]. It sits *alongside* DLSS
   Super Resolution, Ray Reconstruction and (Multi) Frame Generation and does not replace them [S5][S7].
   The previous generation, **DLSS 4.5** (CES, January 2026), brought a second-generation transformer for
   Super Resolution and **6X Dynamic Multi Frame Generation** on RTX 50 [S8][S9].
2. **Integration path for DLSS is Streamline.** The Streamline framework is **MIT**-licensed [S11], but the
   DLSS/NGX runtime binaries and the DLSS-G plugin are closed. They are governed by the **NVIDIA RTX SDKs
   License**, which allows redistribution in object form inside an application with "material additional
   functionality". The DLSS SDK may only be used "for systems with NVIDIA GPUs", and the license forbids
   any use that would make the SDK "subject to an open source software license" [S13]. **Consequence for
   FUSE:** DLSS must be an optional backend that is loaded at runtime, kept out of the MIT tree and out of
   CI. That is feasible, and it is what the licence allows.
3. **ENBSeries is a closed, hook-based D3D post/lighting injector** by Boris Vorontsov [S30][S31]. Its
   **2026-08-06 EULA** explicitly forbids third-party software from parsing, loading, converting or mapping
   ENB presets, parameter names or shaders, and from using the "ENB" name to market compatibility [S32].
   **Blocker: FUSE must not ship an ENB preset importer, reuse ENB parameter names, or advertise "ENB
   compatibility".** The *ideas* (per-time-of-day, per-weather and per-interior parameter blending, a
   data-driven effect stack, user-editable shader files) are generic and can be reimplemented cleanly.
4. **ReShade is BSD-3-Clause**, and so is its FX compiler (`source/effect_*.cpp`, SPDX
   `BSD-3-Clause`). Its add-on API header is dual `BSD-3-Clause OR MIT` [S35][S36][S37]. An optional
   **ReShade-FX compatibility layer is legally viable** for an MIT engine, with attribution. Individual
   community shaders keep their own licences. For example, iMMERSE is "All rights reserved" [S40], while
   SweetFX is MIT [S41].
5. **Vendorable upscalers:** FSR 1/2/3.1 (**MIT**, DX12 and Vulkan, FidelityFX SDK v1.1.4) [S15][S16], Arm
   ASR (MIT, Vulkan, derived from FSR 2) [S24], Snapdragon GSR 1/2 (BSD-3, GLES and Vulkan) [S25], and NVIDIA
   NIS (MIT) [S27]. **Not vendorable:** FSR 4 / "Redstone" ML (signed DLLs, **DX12 only**, RDNA 3/4)
   [S17][S18][S19], XeSS (Intel Simplified Software License: binary only, no modification) [S21], and
   DLSS (RTX SDKs License) [S13].
6. **Recommended FUSE order:**
   - (1) Split render resolution from display resolution and turn the existing TAA into a **native TAAU**
     CPU-reference kernel.
   - (2) Build a **`TemporalUpscaler` abstraction** with a common input contract: jitter, motion vectors
     including dynamic objects, depth, exposure, reactive and transparency masks, and a HUD-less or UI
     split.
   - (3) Vendor **FSR 3.1 (Vulkan)** as the first GPU backend.
   - (4) Add a **data-driven "Look" system**: effect graph, presets and a blend-space over time of day,
     weather and location volumes, a 3D LUT, CAS, lens effects and HDR output.
   - (5) Add optional **DLSS SR/RR (Streamline on Windows, NGX on Linux)**, then frame generation.
   - (6) Optionally add ReShade-FX import.

   Build CPU-provable metrics harnesses (PSNR, SSIM, FLIP, temporal flicker, ghosting, latency
   accounting) before any hardware backend.

---

## 1. NVIDIA DLSS: the 2026 state of the stack

### 1.1 What "DLSS 5" is

| Item | Finding | Source |
|---|---|---|
| Announcement | GTC, **16 March 2026**. Jensen Huang: "DLSS 5 is the GPT moment for graphics". | [S1][S2][S4] |
| What it does | "real-time neural rendering model that infuses pixels with photoreal lighting and materials". It "takes a game's color and motion vectors for each frame as input". The model is trained end to end to understand scene semantics such as characters, hair, fabric, translucent skin and lighting conditions. | [S1][S2] |
| Marketing name | **3D-Guided Neural Rendering**. It uses "the game engine's rendered frame, including its artist-authored geometry, textures, and lighting buffers as an unyielding foundation" and works on "a strict one-frame-in, one-frame-out model". | [S5][S6] |
| Developer controls | Model selection (several models), **Structure Intensity** (high-frequency detail), **Tone Intensity** (low-frequency lighting and colour), semantic AI masking, and engine-level masking of assets such as glassware or foliage. The March announcement mentioned "intensity, color grading and masking". | [S1][S5][S6] |
| Relation to other DLSS parts | An "independent, optional feature that sits alongside Super Resolution, Multi Frame Generation and Ray Reconstruction". NVIDIA describes it as "the final rendering stage". | [S5][S7] |
| Launch | **3 September 2026**, NBA 2K27, all RTX 50 desktop and laptop GPUs and GeForce NOW Ultimate. | [S3][S5] |
| Hardware | NVIDIA's DLSS compatibility table lists "3D-Guided Neural Rendering" as **RTX 50 only**. Secondary press reports RTX 40 support "planned for later this fall"; NVIDIA's own launch post does not confirm this. | [S10][S5][S34] |
| Performance history | NVIDIA says performance improved "5X since March 2026, enabling execution on single GPUs". | [S5] |
| Integration | "using the same NVIDIA Streamline framework used by existing DLSS and NVIDIA Reflex technologies". An Unreal Engine 5 plugin is also mentioned. | [S2][S7] |
| Reception | Mixed. Criticism centres on how it alters faces and on generative artefacts. | [S3][S4] |

**Takeaway for FUSE.** DLSS 5 is a post-render, generative "look" transform. It overlaps conceptually
with an ENB-style look system, but it is driven by an ML model. Integrating it would use Streamline, like
the other DLSS features. It needs the same motion vectors, and FUSE would have to expose engine-side masks
(a per-object "exclude from neural enhancement" stencil or ID mask) [S6]. It is RTX 50 only [S10], so for
FUSE it is a very late, optional item.

### 1.2 DLSS 4.5 and the rest of the DLSS family

| Feature | What it is | GPUs | Source |
|---|---|---|---|
| Super Resolution (SR) + DLAA | Temporal ML upscaler. DLSS 4.5 adds a **2nd-generation transformer** ("5x the compute of the original transformer", trained in linear space). RTX 20/30 lack FP8, so models "M and L" cost more there. | RTX 20+ | [S8][S10] |
| Ray Reconstruction (RR) | ML denoiser and upscaler that replaces hand-tuned RT denoisers. | RTX 20+ | [S10][S14] |
| Frame Generation (FG) | Interpolates generated frames between rendered frames. Requires Reflex. | RTX 40+ | [S10][S12] |
| (Dynamic) Multi Frame Generation | Up to 5 generated frames per rendered frame (**6X**). The dynamic mode picks the multiplier to hit a target refresh rate. | RTX 50 | [S8][S9][S12] |
| 3D-Guided Neural Rendering (DLSS 5) | See §1.1. | RTX 50 | [S10] |

### 1.3 Streamline: integration contract

- **What it is:** "An open-sourced cross-IHV solution that simplifies integration of the latest NVIDIA
  and other independent hardware vendors' super resolution technologies". The current version is
  **2.14.1** [S11]. Everything builds from source except the **DLSS-G plugin**, which ships only as
  prebuilt DLLs. NVIDIA recommends shipping signed production DLLs or checking signatures yourself [S11].
- **Licence:** the Streamline repository is **MIT**. The `sl_nvperf` component is under a separate
  proprietary Nsight Perf SDK licence [S11a].
- **Platforms and APIs:** needs a "GPU supporting DirectX 11 and Vulkan 1.2 or higher" and "Win10 20H1
  or newer". The README documents only Windows builds [S11]. **Treat Streamline as Windows-only.** The
  DLSS SDK ships `Linux_x86_64` and `Linux_aarch64` libraries [S12a], and NVIDIA's Linux driver guide says
  native Linux games can bundle DLSS libraries. On Linux, Reflex is exposed natively through
  `VK_NV_low_latency2` [S12b].
- **DLSS SR inputs** [S12c]:
  - Required: render-resolution colour, output colour, depth, motion vectors.
  - Optional: exposure, and a reactive or transparency mask.
  - Constants: `mvecScale`, so vectors normalise to [-1, 1]; camera matrices without jitter; jitter
    offset in pixels; the `depthInverted` flag.
  - Modes: Off, DLAA, Quality, Balanced, Performance, Ultra Performance.
  - Presets: E to K; A to D and G to I are deprecated.
- **DLSS Ray Reconstruction inputs** [S14]:
  - Required: linear diffuse albedo, specular albedo, normals (F16 or F32), linear roughness, noisy HDR
    colour, dense motion vectors, depth.
  - Required for reflections: either specular motion vectors or specular hit distance plus the view and
    projection matrices.
  - Optional: transparency layer, colour before transparency, SSS guide, DoF guide.
  - RR accepts HDR input only and does not support dynamic resolution scaling.
- **DLSS Frame Generation (DLSS-G) inputs** [S12]:
  - Depth and dense motion vectors, including dynamic objects.
  - The backbuffer, taken through Streamline's swap-chain proxy.
  - Recommended: **HUD-less colour** plus **UI alpha** (or UI colour and alpha), and optionally a
    bidirectional distortion field.
  - Per-frame constants and a frame index matching the Reflex present markers.
  - "It is required for sl.reflex to be integrated". `numFramesToGenerate` goes up to 5.
  - Vulkan needs driver 527.64+ on Windows or 525.72+ on Linux for native optical flow.
  - Windows 10 20H1+ with hardware-accelerated GPU scheduling.
- **Licence of the DLSS runtime** [S13] (NVIDIA RTX SDKs License):
  - Redistribution is allowed "as incorporated in object code format into a software application".
  - Attribution notice is required.
  - End-user terms must be "at least as protective".
  - No stand-alone distribution.
  - **No use that would put the SDK under an open-source licence.**
  - The supplement limits the DLSS/NGX SDK to "systems with NVIDIA GPUs".
  - The same licence covers RTXDI, RTXPT, NRD, RTXGI and RTXNTC [S13][S52][S53][S54].

**FUSE implication.** FUSE can ship an **MIT-licensed adapter** (`fuse_upscaler_streamline`) that calls
Streamline's MIT headers and loads `sl.interposer.dll` and `nvngx_dlss*.dll` at runtime. The NVIDIA
binaries are **not committed** to the repository. Packagers fetch them per platform, and the licence notice
goes into the shipped product's credits.

---

## 2. ENBSeries (Boris Vorontsov)

### 2.1 What it is

- "ENBSeries is 3d graphic modification for games like TES Skyrim, TES Oblivion, Fallout, GTA, Deus Ex,
  and others. It work by modifying render functions calls of the games and applying additional effects."
  There are generic builds (Direct3D proxy DLLs that run on many games with partial effect support) and
  per-game exclusive builds [S30].
- History: first released 3 December 2007 as an SSAO experiment on GTA San Andreas [S31].
- It can chain with other D3D9 and D3D11 mods through a "proxy" feature [S30]. ENBoost, its memory
  manager, lives in `enblocal.ini` [S30].
- Latest build: **v0.505 for Skyrim SE**, 5 August 2026 [S33][S33a]. Active per-game branches include
  Skyrim SE 0.505, Fallout 4 0.501, Skyrim VR 0.495 and Skyrim LE 0.479 [S33].

### 2.2 Effect set (as documented)

- **Generic list:** "SSAO, SSIL, Depth Of Field, Lens FX, Bloom, HDR, Tone Mapping, Sharpening, Sun Rays,
  Shadows, Detailed Shadows, Reflection and others" [S30].
- **Skyrim SE `[EFFECT]` toggles** [S38]: ambient occlusion and indirect lighting, depth of field, ENB
  bloom, lens, adaptation (eye adaptation), reflection, subsurface scattering, skylighting, directional
  skylighting, cloud shadows, detailed shadows, image-based lighting, water, underwater, wet and rain-wet
  surfaces, procedural sun, sun glare, sun rays, volumetric rays, complex particle and fire lights, and
  original post-processing overrides.
- **Added in the Skyrim SE branch** over time [S33a]:
  - complex parallax (two-pass POM) with shadows, terrain parallax, terrain blending
  - complex materials (dielectric and metallic)
  - complex grass with SSS, wind collisions
  - screen-space reflections (water only, for stability)
  - dynamic cubemaps, IBL (with aurora)
  - LOD shadows (v0.504), sky scattering and cloud scattering (v0.502), moon rays
  - underwater caustics, water tessellation, water TAA
  - per-colour-filter `HDRWeighting` (v0.503)
- **User-editable shader files** (HLSL `.fx`) [S38][S39][S33a]: `enbeffect.fx` (tonemap and colour
  grading, multipass since v0.445), `enbbloom.fx`, `enbadaptation.fx`, `enblens.fx`, `enbdepthoffield.fx`,
  `enbeffectprepass.fx` (runs before DoF), `enbeffectpostpass.fx`, `enbsunsprite.fx` and `enbunderwater.fx`.
  Shader parameters use annotations such as `UIHidden` and drop-down boxes, and an "ENB SDK" exposes
  callbacks and parameters to plugins [S33a].
- **Legacy `effect.txt`:** HLSL post effects applied "at the end of frame rendering, after it only game
  hud elements and mouse cursor drawed", with up to eight chained `PostProcess` techniques [S39].
- **Configuration** [S42][S43]: `enbseries.ini` with categories per effect. **Time-of-day (ToD)** variants
  interpolate between Dawn, Sunrise, Day, Sunset, Dusk and Night keyframes (defaults `SunriseTime=7`,
  `DayTime=13`, `SunsetTime=19`, and so on), with separate *Interior* values. Per-weather INIs live in
  `enbseries/Weather/*.ini` and are mapped to weathers by FormID in `_weatherlist.ini`. They are driven by
  an SKSE helper that "queries current weather information from the game and applies effect settings
  overriding those of enbseries.ini".

### 2.3 Licensing and status: a hard constraint

Section numbers below are the EULA's own [S32]:
- **Closed source.** Decompilation, reverse engineering and debugging are prohibited (§4).
- **DonateWare.** Commercial distribution as part of a game requires the author's permission (§2, §5).
- **Section X, X.1 and X.2 (revised 2026-08-06):** third parties may not build "automated software,
  scripts, parsers, or conversion utilities designed to programmatically map, extract, or translate these
  proprietary parameter names". They may not use "wrappers, hooks, compatibility layers, scripts, AI, or
  tools to load, execute, parse, read, interpret, or convert presets, shaders ... created for ENBSeries".
  Runtime interception of ENB state to calibrate other engines is also prohibited.
- **Section Y:** "ENB" and "ENBSeries" may not be used to market the compatibility of alternative
  renderers.
- **Section Z:** preset authors own their presets and may port them manually themselves. Third-party
  automated migration is prohibited.

**Status for FUSE:**
- **Do not** implement an ENB `.ini` or `.fx` importer.
- **Do not** mirror ENB parameter identifiers such as `AmbientLightingIntensityNight`.
- **Do not** mention ENB compatibility in product text.

The enforceability of the ENB EULA against clean-room engines is not assessed here. The conservative
reading is simply to avoid it.

### 2.4 What is transferable as an engine-native feature

These are generic rendering and tooling concepts, long used in engines, that FUSE can implement with its
own names and data format:

| ENB concept | Engine-native equivalent in FUSE |
|---|---|
| Per-ToD, per-weather and interior parameter sets | `LookProfile` assets blended by a **blend-space**: ToD curve × weather weights × location/post volumes. See §4.2. |
| User-editable `.fx` post shaders | A data-driven **effect graph** of kernels, plus an optional ReShade-FX importer (BSD-3). See §4.2. |
| SSAO + SSIL | FUSE already has HBAO, SSGI and DDGI. Expose an "indirect lighting" look control and optionally add a GTAO/visibility-bitmask path (XeGTAO is MIT [S47]). |
| Bokeh DoF, lens (dirt, flare, chromatic aberration), bloom, adaptation | Existing DoF, bloom, lens flare and auto exposure. Add lens dirt, anamorphic streaks, chromatic aberration and a physically based bokeh shape. |
| Colour grading in `enbeffect.fx` | 3D-LUT grading (a 32³ or 64³ LUT in log or PQ space), plus the existing lift/gamma/gain. |
| Sun rays, volumetric rays, sky and cloud scattering | Existing atmosphere and volumetrics. Expose them as look parameters. |
| Wet surfaces, rain, snow | Material-layer parameters driven by the weather system (not post). |
| Live editor with instant preview | Editor "Look" panel with hot reload and an A/B split view. |

---

## 3. Landscape: related technologies

### 3.1 Upscalers

| Tech | Kind | Inputs | APIs | Licence and distribution | Integration effort for FUSE | Source |
|---|---|---|---|---|---|---|
| **DLSS SR / DLAA** | Temporal, ML (transformer) | Colour, depth, MV, jitter, optional exposure and reactive masks | DX11, DX12, Vulkan (Streamline on Windows; DLSS SDK also ships Linux libraries) | RTX SDKs License, closed binaries, NVIDIA GPUs only | Medium (runtime plugin, no CI) | [S12a][S12c][S13] |
| **FSR 1** | Spatial (EASU + RCAS) | Anti-aliased, tone-mapped colour. Place it after tonemap and before grain/UI. | DX11, DX12, Vulkan | **MIT** | Low (pure shaders; CPU-portable) | [S20] |
| **FSR 2.3 / FSR 3.1** | Temporal, analytic | Colour, depth, MV, exposure, **reactive mask**, **transparency and composition mask**, jitter | DX12, **Vulkan** (SDK v1.1.4) | **MIT**, source | Medium (vendor shaders; CPU port feasible) | [S15][S16] |
| **FSR 4 / FSR Upscaling 4.1 ("Redstone")** | Temporal, ML | Like FSR 3.1 | **DX12 only** in FSR SDK 2.x ("Vulkan is currently not supported in SDK") | Signed prebuilt DLLs, "binaries and limited source". 4.1.1 runs on RX 9000 and RX 7000 (SM 6.6). FSR 4 source was briefly published under MIT by mistake in August 2025; AMD called it an error. | High / not applicable for Vulkan. **Do not vendor the leaked code.** | [S17][S18][S19][S22] |
| **XeSS SR (1.x to 3)** | Temporal, ML (XMX, or DP4a on SM 6.4 GPUs) | Colour, depth, MV, jitter, exposure, responsive mask | DX12; **Vulkan 1.1**; DX11 on Arc only | **Intel Simplified Software License**: binary redistribution only, **no modification or reverse engineering**. Intel's open-sourcing is still unfulfilled. | Medium (runtime DLL); no Linux build documented | [S21][S21a][S21b][S23] |
| **MetalFX** | Spatial, temporal, **denoised upscaler** (Metal 4), frame interpolation | Temporal: colour, depth, MV, jitter, exposure, optional reactive mask. Denoised: adds normals, diffuse and specular albedo, roughness, optional hit distance. | Metal (Apple silicon) | Apple SDK | Only if FUSE gains a Metal RHI | [S26] |
| **DirectSR** | Vendor-neutral D3D12 API over DLSS, FSR and XeSS variants | Colour, depth, MV (with scale), jitter, optional exposure, reactive and ignore-history masks, sharpness | **D3D12 only** (`directsr.dll`, Agility SDK) | Part of Windows / Agility SDK | Not applicable (FUSE is Vulkan) | [S28][S28a] |
| **Unreal TSR** | Temporal, analytic, Epic-only | Depth, velocity, colour. History stored at 200% on Epic/Cinematic tiers; anti-flicker and thin-geometry heuristics | D3D11/12, Vulkan, Metal, consoles | UE EULA (not reusable) | Design reference only | [S29] |
| **Sony PSSR** | ML (PS5 Pro). The 2026 upgrade uses the FSR 4 algorithm (Project Amethyst). | Not public | PS5 Pro | Platform NDA | Not applicable | [S44] |
| **Arm ASR** | Temporal (FSR 2.2.2 derivative, mobile-tuned, fp16) | Colour, depth, MV, optional reactive mask and exposure | **Vulkan** (custom backends possible) | **MIT** | Low to medium (good mobile/low-power reference) | [S24] |
| **Snapdragon GSR 1/2** | SGSR1 spatial (12-tap Lanczos-like + sharpen); SGSR2 temporal | SGSR2: depth, MV, jitter | GLES, Vulkan | **BSD-3** | Low (small shaders; CPU-portable) | [S25] |
| **NVIDIA NIS** | Spatial (6-tap + directional adaptive sharpening), NVScaler / NVSharpen | Colour | HLSL/GLSL compute (DX11, DX12, Vulkan) | **MIT** | Low | [S27] |
| **Checkerboard rendering** | Half-rate shading with an ID buffer and a TAA-integrated resolve (Frostbite) | Object ID, depth, MV, MSAA sample positions | Any | Technique (GDC 2017) | High (deep pipeline change); lower priority than TAAU | [S45] |
| **TAAU (generic)** | Temporal accumulation into display-resolution history. The main parts are sample accumulation and history validation. | Colour, depth, MV, jitter | Any | Technique | **Native baseline for FUSE** | [S46] |

### 3.2 Frame generation and latency

| Tech | How it works | Requirements | Licence | Source |
|---|---|---|---|---|
| **DLSS FG / MFG / Dynamic MFG** | Optical flow plus ML interpolation. Up to 5 generated frames; the dynamic mode targets the refresh rate. | RTX 40 (FG) or RTX 50 (MFG). Reflex mandatory. HUD-less colour and UI alpha. HAGS on Windows. | RTX SDKs License | [S8][S10][S12] |
| **FSR 3.1 Frame Generation** | Optical flow (derived from AFMF) plus interpolation. Optional async compute. UI by (1) a separate UI texture, (2) a callback, or (3) HUD-less vs. with-UI comparison. Recommend ≥ 60 fps before interpolation. | DX12 and Vulkan; any vendor. Anti-Lag 2 from FSR 3.1.1. | **MIT** | [S16] |
| **FSR Frame Generation 4 (ML)** | ML interpolation | RX 9000+; DX12 SDK only | Signed DLLs | [S17][S18] |
| **XeSS-FG (+ MFG 3x/4x in XeSS 3)** | ML interpolation. **XeLL is required.** | Arc and SM 6.4 GPUs (MFG on Arc). DX12. | Intel Simplified Software License | [S21][S21a][S23] |
| **MetalFX Frame Interpolation** | One interpolated frame per two inputs. UI composited, offscreen, or rendered every frame. | Apple silicon | Apple SDK | [S26] |
| **NVIDIA Reflex / Reflex 2 Frame Warp** | Low-latency mode syncs CPU and GPU; markers and PCL stats. Frame Warp reprojects the frame to the latest input. | Vulkan through `VK_NV_low_latency2`. **Reflex 2 was still unreleased as of July 2026.** | RTX SDKs License / Streamline | [S12b][S48][S49] |
| **AMD Anti-Lag 2** | In-game latency SDK; Vulkan through `VK_AMD_anti_lag` | Adrenalin 24.9.1+ for Vulkan | **MIT** | [S50] |
| **Vulkan present timing** | `VK_EXT_present_timing` (published December 2025): present scheduling and feedback timestamps | Driver support varies (Mesa 26.2 X11) | Khronos | [S51] |
| **AFMF 2 / 2.1** | Driver-level interpolation, no engine integration, no engine motion vectors | RX 6000+; DX11, DX12, Vulkan, OpenGL | AMD driver | [S55] |
| **Lossless Scaling (LSFG 3.x)** | App-level: captures the window and interpolates like an overlay, with no injection. X2 to X20. | Any GPU; windowed or borderless | Commercial (Steam) | [S56] |
| **NVIDIA Smooth Motion** | Driver-level FG. NVIDIA warns not to combine it with native DLSS FG. | NVIDIA driver; Linux env var | NVIDIA driver | [S12b] |

### 3.3 Denoisers and ray reconstruction

| Tech | Summary | Licence | Source |
|---|---|---|---|
| **DLSS Ray Reconstruction** | ML joint denoise and upscale (inputs in §1.3) | RTX SDKs License | [S14] |
| **NVIDIA NRD** (REBLUR, RELAX, SIGMA, REFERENCE) | Spatio-temporal, API-agnostic; needs diffuse and specular split, hit distance, normal and roughness, viewZ, MV. D3D11, D3D12 and Vulkan through NRI. v4.18.0. | RTX SDKs License | [S52][S13] |
| **AMD FidelityFX Denoiser** | Reflection and shadow denoisers (v1.3), DX12 and Vulkan | **MIT** | [S15] |
| **AMD FSR Ray Regeneration** | ML denoiser (Redstone), RX 9000+, DX12 | Signed DLLs | [S17][S19] |
| **Intel OIDN 2.5.1** | CNN denoiser (colour + albedo + normal) with fast, balanced and high modes. Runs on CPU, SYCL, CUDA, HIP and Metal. | **Apache-2.0** | [S57] |
| **SVGF** (HPG 2017) / **A-SVGF** (HPG 2018) | Variance-guided à-trous wavelet filter with temporal accumulation. A-SVGF adds temporal gradients to cut lag and ghosting. | Papers (implement freely) | [S58][S59] |
| **MetalFX denoised upscaler** | ML joint denoise and upscale on Apple | Apple SDK | [S26] |

### 3.4 Post-process injectors and frameworks

| Tool | What it is | Licence | Notes | Source |
|---|---|---|---|---|
| **ReShade 6.8.0** (2 August 2026) | Hooking post-process injector for D3D9/10/11/12, OpenGL and Vulkan. Own **ReShade FX** language (HLSL-like) compiled once for every API. Add-on DLL API. Depth access. | **BSD-3-Clause** (FX compiler files `BSD-3-Clause`; add-on API header `BSD-3-Clause OR MIT`) | Depth is disabled in multiplayer; full add-on support needs the unsigned build | [S35][S36][S37] |
| **iMMERSE / iMMERSE Pro** (Marty McFly / Pascal Gilcher) | MXAO (GTAO-based), SMAA, Launchpad (normals and optical flow), Sharpen. Pro adds RTGI (screen-space ray-traced GI) and ReGrade. | "Copyright (c) Pascal Gilcher. **All rights reserved.**" Pro is Patreon-only. | Not reusable; ideas only | [S40][S40a] |
| **SweetFX** (CeeJay.dk) | Classic shader suite, now distributed as ReShade FX | **MIT** | Reusable with notice | [S41] |
| **ENBSeries** | See §2 | Proprietary, restrictive EULA | Avoid any compatibility | [S30][S32] |
| **GShade** | ReShade fork for FFXIV and others. Shut down February 2023 after its developer shipped forced-reboot "anti-tamper" code. Mirrors exist. | Mixed or unclear | Cautionary tale about injector trust | [S60][S60a] |
| **RTX Remix** | Omniverse-based remastering platform for **DX8/DX9 fixed-function** games. Captures the scene and reinjects assets with path tracing, DLSS and Reflex. | Runtime **MIT** (open source) | Scene-capture approach, not a post filter | [S61] |
| **NVIDIA Freestyle / RTX HDR / RTX Dynamic Vibrance** | Driver-level filters in the NVIDIA App. RTX HDR is AI SDR-to-HDR; Dynamic Vibrance is AI saturation. More than 1,200 games. | NVIDIA | End-user only | [S62] |
| **Special K** | Injection "Swiss Army knife": HDR retrofit, frame pacing and latency limiter, texture mods. D3D9/11/12 and OpenGL. | **GPL-3.0** | Do not copy code into an MIT engine | [S63] |
| **Windows Auto HDR** | OS SDR-to-HDR for DX11 and DX12 games, with a per-game intensity slider | OS feature | Native HDR output makes it unnecessary | [S64] |

### 3.5 Middleware and open libraries

| Library | Contents | Licence | APIs | Source |
|---|---|---|---|---|
| **AMD FidelityFX SDK v1.1.4** | CACAO (AO), CAS, Denoiser, Classifier, LPM (HDR tonemap and gamut map), Parallel Sort, SPD, SSSR, FSR 1/2/3.1, Variable Shading, Blur, DoF (bokeh), Lens (CA, grain, vignette), Hybrid Shadows and Reflections, Breadcrumbs, Brixelizer, Brixelizer GI | **MIT** | DX12, **Vulkan** | [S15] |
| **AMD FSR SDK 2.x "Redstone"** (2.3, June 2026) | FSR Upscaling 4.1 (ML), FG 4 (ML), Ray Regeneration, Radiance Caching (preview) | Binaries and limited source | **DX12 only** | [S17][S18][S19] |
| **NVIDIA RTX Kit** | DLSS, RTXNS (neural shading), RTXNTC, RTXTF, RTXTS, RTX Mega Geometry, RTXCR, RTXGI, RTXDI, RTXPT, Streamline, NRD, OMM, RTXMU, STBN | Mostly RTX SDKs License; Streamline MIT | DX12, Vulkan | [S65][S13] |
| **RTXDI 3.1** | ReSTIR DI, GI and PT | RTX SDKs License | D3D12, Vulkan | [S53] |
| **RTXGI 2.x** | Neural Radiance Cache (NRC, tensor cores, Turing+) and SHaRC (shader-only) | See repository | DX12, Vulkan | [S66] |
| **RTXPT** | Reference real-time path tracer: RTXDI, NRD, DLSS RR/SR/FG/MFG, SER | RTX SDKs License | DX12, Vulkan | [S54] |
| **Intel XeGTAO** | GTAO with bent normals and a denoise pass. About 1.4 ms at 4K on an RTX 3070 (High). **Archived April 2024.** | **MIT** | DX/HLSL | [S47] |
| **Unreal Lumen, Unity HDRP** | Engine-internal, not reusable | EULA | – | – |

### 3.6 Neural rendering trends (2025–2026)

- **DirectX.** SM 6.9's **Cooperative Vector** is being **deprecated** in favour of a unified **DirectX
  Linear Algebra** (matrix-matrix and vector-matrix) in **SM 6.10** [S67][S68]. At GDC 2026 (12 March 2026)
  Microsoft announced LinAlg public preview for April 2026 and a **DirectX Compute Graph Compiler** in
  private preview for summer 2026, "with AMD, Intel, NVIDIA, and Qualcomm" support on day one [S69].
- **Vulkan.**
  - `VK_KHR_cooperative_matrix` is the cross-vendor cooperative GEMM [S70].
  - `VK_NV_cooperative_matrix2` adds conversions, reductions, per-element ops, tensor addressing and
    workgroup-scope matrices [S70a].
  - `VK_NV_cooperative_vector` (SPV_NV_cooperative_vector) adds per-invocation vectors that cooperate
    on matrix-vector multiplies without uniform control flow [S71].
- **Neural texture compression.** RTXNTC works on Vulkan and D3D12 through cooperative vectors, claiming a
  "2-4x improvement in inference throughput" on Ada and Blackwell. It is RTX SDKs licensed, and v0.10
  beta adds Agility SDK support [S72][S6].
- **Neural shaders and materials.** RTXNS is a Slang-based sample for inference and training on Windows
  (DX12 preview) and Linux (Vulkan cooperative vector, R570+) [S73].
- **Neural radiance caching.** NVIDIA NRC in RTXGI 2.x [S66]; AMD FSR Radiance Caching (preview) [S17].
- **Generative post (DLSS 5).** See §1.1.

---

## 4. Recommended architecture for FUSE

### 4.0 Where FUSE is today (from the repository)

- The TAA is a **same-resolution** resolve. `TaaCpuResolver` does Halton jitter, velocity reprojection,
  Catmull-Rom history, YCoCg variance clipping, and depth and velocity rejection. Inputs are the jittered
  current colour, velocity in pixels, and linear depth
  (`Source/FUSE/Renderer/include/fuse/renderer/taa/taa_cpu_resolve.hpp`, `taa_types.hpp`).
- The post chain is a fixed order: Bloom, DoF, MotionBlur, ToneMap, ColorGrade, FilmGrain
  (`postprocess/post_stack.hpp`). Grading is ASC-style lift/gamma/gain plus vignette and grain; there is
  **no 3D LUT, no sharpening, no reactive masks and no render/display resolution split**
  (`postprocess/color_grade.hpp`).
- Kernels follow the single-source model: `CpuReference`, `CpuParallel` and `Cuda`, with a
  `VulkanCompute` seam and `run_parity` for parity tests (`docs/compute-kernels.md`).

### 4.1 (a) `TemporalUpscaler` / `FrameGenerator` abstraction

**Interface sketch.** Illustrative only; the names follow FUSE conventions.

```cpp
namespace fuse::renderer::upscale {

enum class UpscalerBackend : u8 { None, NativeTaau, Fsr1, Fsr3, ArmAsr, Sgsr2, XeSS, DlssSR, DlssRR };
enum class QualityMode : u8 { NativeAA, Quality /*1.5x*/, Balanced /*1.7x*/, Performance /*2x*/, UltraPerf /*3x*/ };

struct UpscalerCaps {           // queried at runtime; CI only ever sees NativeTaau/Fsr1/Fsr3 (CPU ports)
    bool temporal, needs_reactive, supports_rr_guides, supports_dynamic_res, hdr_input, ml;
    f32  min_scale, max_scale;
};

struct UpscaleInputs {          // one contract that is a superset of DLSS/FSR/XeSS/ASR/DirectSR
    TextureRef color;           // render-res, jittered, pre-tonemap HDR (FSR1: post-tonemap LDR)
    TextureRef depth;           // render-res; + flag: reversed-Z / linear
    TextureRef motion;          // render-res, pixels or NDC; includes skinned/dynamic objects & camera
    TextureRef exposure;        // 1x1 (or scalar) — from AutoExposure; must match what tonemap uses
    TextureRef reactive;        // optional R8: particles/alpha-blended/animated textures
    TextureRef transparency;    // optional R8: transparency & composition (FSR) / ignore-history (DirectSR)
    // Ray-reconstruction guides (optional; DLSS-RR, MetalFX denoised, FUSE native RR later)
    TextureRef diffuse_albedo, specular_albedo, normals, roughness, spec_hit_dist;
    Vec2 jitter_px;             // this frame's sub-pixel offset (pixel units, +x right/+y down doc'd)
    Vec2 mv_scale;              // converts `motion` into the backend's convention
    Mat4 view, proj_nojitter, prev_view_proj;   // row/col-major documented
    f32  near_z, far_z, vfov, frame_dt_ms;
    bool reset_history;         // camera cut / teleport / resolution change
    u32  frame_index;
};
struct UpscaleOutputs { TextureRef color_display_res; };

class ITemporalUpscaler {
public:
    virtual UpscalerCaps caps() const = 0;
    virtual Vec2u render_size(Vec2u display, QualityMode) const = 0;
    virtual u32  jitter_phase_count(Vec2u render, Vec2u display) const = 0; // e.g. 8*(display/render)^2 (FSR)
    virtual bool evaluate(CommandContext&, const UpscaleInputs&, UpscaleOutputs&) = 0;
};

struct FrameGenInputs { TextureRef hudless_color, ui_color_alpha, depth, motion; /* + camera, frame_index */ };
class IFrameGenerator { /* present-side: owns swapchain proxy / pacing; requires LatencyProvider */ };
class ILatencyProvider { /* markers: sim start/end, render submit, present; backends: None, Reflex(VK_NV_low_latency2), AntiLag2(VK_AMD_anti_lag) */ };
}
```

**Backends and priority:**

| # | Backend | Why | Build and CI policy |
|---|---|---|---|
| 1 | **NativeTaau** | Extends the existing `TaaCpuResolver` to display-resolution history. The main parts are accumulation and validation, as in the survey [S46]; TSR's 200% history and anti-flicker ideas [S29]. Written as a single-source kernel: CpuReference is ground truth, CpuParallel ships, CUDA and Vulkan come later. | Always built; parity-tested in CI |
| 2 | **Fsr1** (EASU + RCAS) | MIT, trivial, runs after tonemap [S20]. Useful fallback and a "spatial" test point. | Port to a single-source kernel; CI |
| 3 | **Fsr3** (FSR 3.1 upscaler, FidelityFX SDK v1.1.4) | MIT, Vulkan, vendorable [S15][S16]. Industry-standard input contract (reactive and T&C masks). | Vendor under `third_party/` with the MIT notice. The GPU path needs the Vulkan RHI. A CPU port of the shader logic is possible for parity. |
| 4 | **DlssSR / DlssRR** | Best quality on NVIDIA [S8]. RR could replace hand-written denoisers for the RT/SDF paths [S14]. | Optional `FUSE_WITH_STREAMLINE` (Windows) or `FUSE_WITH_NGX` (Linux). Runtime-detected. **Not built in CI**; binaries fetched by packaging, never committed [S11][S13]. |
| 5 | **XeSS** | Covers Arc and DP4a GPUs [S21]. | Optional runtime DLL. Licence allows binary redistribution only [S21b]. |
| 6 | **ArmAsr / Sgsr2** | Mobile and low-power targets; MIT or BSD-3 [S24][S25]. | Optional; low effort once the FSR 2/3 contract exists |
| – | FSR 4 / Redstone | DX12-only SDK [S18]; FUSE is Vulkan | Revisit if AMD ships Vulkan |
| – | DirectSR | D3D12-only [S28a] | Not applicable |

**Frame generation (later, hardware-only):** `IFrameGenerator` backends are DLSS-G (Streamline, Reflex
mandatory) [S12] and FSR 3.1 FG (MIT, Vulkan) [S16]. FUSE-native interpolation would be a research item.
`ILatencyProvider` backends are Reflex through `VK_NV_low_latency2` [S12b], Anti-Lag 2 through
`VK_AMD_anti_lag` (MIT SDK) [S50], and `VK_EXT_present_timing` for pacing [S51].

**Required render-graph changes. These are the real work, and they are needed by every backend:**

1. **Render and display resolution split.** Every pass before the upscaler runs at render resolution and
   every pass after it at display resolution. Add dynamic resolution support.
2. **Jittered projection.** Take the Halton sequence from `TaaJitter`, but set the phase count from the
   upscale ratio: FSR recommends about 8×(display/render)² phases. Apply a **mip LOD bias** of
   `log2(render/display) - 1` (the common FSR and DLSS guidance) to material sampling.
3. **Motion vectors for everything:**
   - camera plus per-object previous transforms
   - skinned meshes (previous-frame skinning)
   - particles and vertex animation, where possible
   - **SDF/raymarched surfaces:** compute motion from reprojected hit position and object motion
   - **sky/atmosphere:** camera-only motion at infinite depth
4. **Exposure.** Pass the *same* exposure value that tonemapping uses (`PostStack::totalExposureEv`) to
   the upscaler as a 1×1 texture [S12c][S16].
5. **Reactive and transparency masks.** Particles, alpha-blended passes, UV-scrolling and emissive
   animated materials write to R8 masks. Volumetrics and atmosphere may also contribute.
6. **Pass ordering.** The upscaler replaces TAA and runs **before** bloom, DoF, motion blur, tonemap,
   grade and grain. This is required for DLSS/FSR 2+/XeSS, which want HDR pre-post colour. DoF and motion
   blur at display resolution cost more, but they can run on the upscaled image. FSR 1 is the exception:
   it runs after tonemap and before grain [S20].
7. **UI separation.** Render UI into its own RGBA target. Composite it after upscaling for SR. For frame
   generation, supply **HUD-less colour plus UI alpha** [S12][S16].
8. **History reset.** Camera cuts, teleports and resolution changes set `reset_history`.
9. **RR guides** (later): diffuse and specular albedo, normals, roughness, specular hit distance (or
   specular motion vectors) from the G-buffer and RT/SDF passes [S14].

### 4.2 (b) Engine-native "Look" system, inspired by ENB and ReShade but independent of both

**Data model** (FUSE-owned names and schema, stored as JSON or TOML assets):

- **`EffectNode`**: a post kernel with typed inputs and outputs (HDR, LDR, depth, normals, velocity,
  masks), a parameter block schema (float, vec, colour, curve, LUT, texture), and a stage tag:
  - `PreUpscale` (render-res HDR): SSAO/SSIL, SSR, volumetric composite
  - `PostUpscaleHDR` (display-res HDR): bloom, DoF, motion blur, lens
  - `Display` (after tonemap): grade LUT, CAS, grain, vignette
  - `Output`: HDR encode
- **`EffectGraph`**: a DAG compiled to a linear schedule inside the render graph. Default order follows
  common practice and the FSR 1 note on placing noise after upscaling [S20]:
  1. SSAO/SSIL
  2. SSR
  3. volumetrics composite
  4. **upscaler**
  5. DoF
  6. motion blur
  7. bloom + lens dirt + flare
  8. auto exposure / tonemap (or LPM for HDR [S15])
  9. 3D LUT grade
  10. CAS sharpen [S74]
  11. chromatic aberration and vignette
  12. film grain
  13. UI composite
  14. output transform (sRGB, PQ or scRGB)
- **`LookProfile`**: parameter overrides for a subset of nodes.
- **`LookBlendSpace`**: evaluates the final parameters each frame:
  - **Time of day:** keyframes on a 24 h curve. You are not limited to six fixed slots; ENB's
    dawn/sunrise/day/sunset/dusk/night split is one choice [S42]. Interpolation is Catmull-Rom or linear,
    per parameter.
  - **Weather:** weights from the weather system, cross-fading during transitions.
  - **Location:** post volumes with priority, blend radius and an interior flag, as UE PostProcessVolume
    does.
  - **Gameplay overrides:** damage, underwater, photo mode.
  - Composition: `final = lerp_chain(base, ToD(t), Σ weather_i·w_i, volumes by priority, overrides)`.
    Parameters declare a blend mode (lerp, log-lerp for exposure and intensities, or LUT blend in a LUT
    atlas).
- **Editor.** A Look panel with live edit, hot reload of kernel parameters and shader files, an A/B split,
  a scopes view (waveform, vectorscope, histogram) and a false-colour exposure view. MetalFX has a similar
  exposure debug checkerboard [S26].

**Effects to add or upgrade** (FUSE already has bloom, DoF, motion blur, lens flare, auto exposure,
tonemap curves, grade and grain):

| Effect | Approach and reference | Licence note |
|---|---|---|
| 3D LUT grading | 32³ or 64³ LUT in a log or PQ shaper space; import `.cube`; LUT blending for the blend-space | Own code |
| CAS sharpening (+ optional scale) | Port AMD CAS [S74] or RCAS [S20] | MIT (keep the notice) |
| SSIL / GI boost | Reuse SSGI/DDGI; optional GTAO-style AO with bent normals, as in XeGTAO [S47] | XeGTAO MIT |
| Bokeh DoF upgrade | Scatter/gather with shaped bokeh; FidelityFX DoF as reference [S15] | MIT |
| Lens dirt, chromatic aberration, anamorphic streaks | Extend `lens_flare`; FidelityFX Lens as reference [S15] | MIT |
| HDR output | scRGB or HDR10 (PQ) swapchain; tonemap to display max nits; FidelityFX LPM [S15] | MIT |
| Film grain | Existing hash grain; add a luminance-dependent response and per-channel size | Own |
| Sub-surface, wetness, snow | Material layers driven by weather, not post | Own |

**Optional ReShade-FX compatibility (Phase 6).**
- ReShade's FX lexer, preprocessor, parser and SPIR-V codegen are **BSD-3-Clause**
  (`source/effect_*.cpp` carry `SPDX-License-Identifier: BSD-3-Clause`) [S36][S37]. They can be vendored
  into MIT FUSE, provided the BSD notice is reproduced in the binary distribution and no endorsement is
  implied [S36].
- An importer would compile `.fx` to SPIR-V for the Vulkan path. It would map ReShade's standard
  semantics (`COLOR`, `DEPTH`, uniforms such as `timer` and `framecount`) to FUSE graph resources, and
  wrap each technique as an `EffectNode` in the `Display` stage.
- CPU-reference execution of arbitrary FX is **not** practical. FX-imported nodes are GPU-only.
- **Shader licences vary.** Ship none by default. Document that users bring their own, and flag
  "All rights reserved" collections such as iMMERSE [S40].
- **Do not** do the same for ENB `.fx` or `.ini` (see §2.3) [S32].

### 4.3 (c) What can be proven on CPU and what needs hardware

| Item | CPU-provable in FUSE CI? | Notes |
|---|---|---|
| Native TAAU (single-source kernel) | **Yes.** Bit-exact CpuReference vs. CpuParallel; CUDA within tolerance. | Primary quality baseline |
| FSR 1 (EASU/RCAS), CAS, NIS, SGSR1 | **Yes.** Straightforward per-pixel math. | Port shaders to kernels |
| FSR 2/3.1 upscaler, Arm ASR, SGSR2 | **Mostly.** A CPU port of the algorithm is feasible for parity and metrics; the production path is GPU (Vulkan). | Large shader bodies; start with GPU vendoring |
| Motion-vector correctness (camera, skinned, SDF) | **Yes.** Analytic reprojection tests. | Prerequisite for everything |
| Reactive and transparency mask generation | **Yes** | |
| Look blend-space, ToD/weather/volume evaluation, LUT bake and apply | **Yes.** Deterministic unit tests. | |
| Effect-graph scheduling and validation | **Yes** | |
| ReShade-FX parse and codegen to SPIR-V | **Parse and codegen: yes** (compile-only tests). **Execution: needs GPU** | |
| Frame-pacing and latency *model* (timeline simulation) | **Yes.** Simulated timelines. | Real measurement needs a GPU and display |
| DLSS SR/RR/FG, DLSS 5, XeSS, FSR 4 | **No.** Hardware and closed runtime. | Manual or nightly runs on dedicated machines |
| FSR 3.1 FG, Reflex, Anti-Lag 2, present timing | **No** (GPU and swapchain) | |
| Neural (cooperative vector/matrix) | **Partially.** A CPU reference MLP for parity. Performance needs hardware. | [S70][S71] |

---

## 5. Prioritised implementation plan with verification

| Phase | Deliverable | Verification |
|---|---|---|
| **P0: Metrics harness** | `fuse_imgmetrics` (CPU): PSNR, SSIM (and MS-SSIM), **FLIP** (LDR and HDR; port or vendor NVlabs FLIP, BSD-3 [S75][S76]), temporal metrics. Deterministic **reference scenes**: thin geometry fence, sub-pixel wires, high-frequency texture, particles over a moving background, skinned character crossing a textured wall, SDF scene, camera cut. Ground truth is 16–64 spp supersampled native. | Metrics unit tests against known image pairs; golden values stored in the repository |
| **P1: Render/display split + MV completeness** | Resolution split in the render graph; jitter phase count from the ratio; mip bias; skinned and SDF motion vectors; exposure texture; reactive and T&C masks; UI target. | Analytic MV tests: reproject frame N-1 into N and require error below 1e-3 px for static geometry. Mask coverage tests. |
| **P2: NativeTaau kernel** | Upgrade `TaaCpuResolver` to display-resolution history (Lanczos or Catmull-Rom resample), luminance-weighted accumulation, reactive-mask-driven alpha, anti-flicker (history luminance variance). | **Quality:** PSNR, SSIM and FLIP vs. ground truth at 1.5×, 1.7×, 2× and 3×. Beat bilinear and FSR 1; track against FSR 3.1 once available. **Stability:** temporal flicker metric (below). **Ghosting:** trail test (below). **Parity:** `run_parity` CpuReference vs. CpuParallel bitwise. |
| **P3: Upscaler abstraction + FSR 1 + FSR 3.1 (Vulkan)** | `ITemporalUpscaler`, backend registry, runtime caps. FSR 3.1 vendored (MIT) [S15]. | Same metric suite per backend; the backend matrix is recorded in `KernelStats`-style reports |
| **P4: Look system v1** | EffectGraph, LookProfile, LookBlendSpace (ToD, weather, volumes), 3D LUT, CAS, lens dirt and CA, editor panel | Blend-space determinism tests; LUT identity round-trip below 1/1024; CAS vs. reference port; "no-op look = bit-identical to current PostStack" regression |
| **P5: HDR output** | PQ and scRGB swapchain, display-referred tonemap (LPM or own) | Colour-bar and ramp tests; PQ encode/decode round-trip; mid-grey calibration consistent with existing `calibrate_mid_grey` |
| **P6: DLSS SR/RR (optional)** | Streamline (Windows) and NGX (Linux) adapter behind a flag; runtime detection; graceful fallback to NativeTaau | Manual or nightly GPU run; same metric suite; screenshot diffs; licence notice check in the packaging step |
| **P7: Frame generation + latency** | `ILatencyProvider` (Reflex, Anti-Lag 2, none), present-timing pacing, then FSR 3.1 FG and DLSS-G | Latency accounting (below); frame-pacing histogram (MetalFX guidance targets ≤ 2 histogram buckets [S26]); FG UI-artefact test with a HUD-heavy scene |
| **P8: ReShade-FX import (optional)** | Vendor the BSD-3 compiler; FX → SPIR-V → EffectNode | Compile-only CI for a corpus of MIT-licensed FX (for example SweetFX [S41]); GPU smoke test offline |
| **P9: Neural (research)** | CPU-reference MLP kernel; later `VK_KHR_cooperative_matrix` and `VK_NV_cooperative_vector` backends; NTC-style texture decode experiment | Parity CPU vs. GPU within tolerance; performance on hardware |

**Metric definitions to implement:**
- **Quality vs. native.** Per-frame PSNR, SSIM and FLIP between the upscaled output and the supersampled
  native reference, in display space after an identical tonemap. Report the mean and the 95th percentile
  over 120-frame sequences. FLIP is designed for "differences between rendered images and corresponding
  ground truths" as perceived when alternating images [S75].
- **Temporal stability (flicker).** For a static camera and scene, compute the per-pixel temporal standard
  deviation of luminance across N frames, minus the same for the reference. Also compute
  **tPSNR/tFLIP**: the metric on the frame difference `(out[t] - out[t-1])` vs. `(ref[t] - ref[t-1])`.
  This catches shimmer that per-frame metrics miss.
- **Ghosting.** Move a high-contrast object across a textured background at known px/frame. Measure the
  energy of `out - ref` in the trail region behind the object, and the number of frames until residual
  error drops below a threshold after a disocclusion. The same rig with particles tests reactive-mask
  effectiveness.
- **Disocclusion recovery.** Frames until SSIM in the revealed region reaches 0.95 of steady state.
- **Latency accounting.** Instrument markers for input sample, sim start/end, render submit, GPU
  start/end and present. With present timing [S51], also record actual display time.
  - Compute click-to-photon estimates per frame.
  - With frame generation, report both **displayed fps** and **render fps**, plus added latency: about
    one rendered frame plus FG cost for interpolation, which is why DLSS-G requires Reflex [S12] and FSR 3
    recommends ≥ 60 fps base [S16].
  - The CPU-only timeline simulator lets CI verify the accounting logic.

---

## 6. Licensing matrix and blockers

| Component | Licence | Can FUSE (MIT) vendor source? | Can FUSE redistribute binaries? | Blocker? |
|---|---|---|---|---|
| FidelityFX SDK v1.1.4 (FSR 1/2/3.1, CAS, LPM, CACAO, SSSR, DoF, Lens, Brixelizer GI…) | MIT [S15] | Yes | Yes | No |
| FSR SDK 2.x (FSR 4 / Redstone) | Signed DLLs, limited source; DX12 only [S18][S19] | No | Per AMD terms | **Yes for Vulkan (unsupported)**. Avoid the leaked FSR 4 source [S22]. |
| Streamline | MIT (+ Nsight Perf proprietary part) [S11a] | Yes (headers/adapter) | – | No |
| DLSS / NGX / DLSS-G / Reflex / NRD / RTXDI / RTXPT / RTXNTC | NVIDIA RTX SDKs License [S13] | **No** (keep outside the MIT tree) | Yes, in object form inside the app, with notice and EULA; NVIDIA GPUs only | **Conditional.** Optional plugin; not in CI; must not be made subject to an OSS licence. |
| XeSS SDK | Intel Simplified Software License [S21b] | No (binary only) | Yes, with notice; no modification | Conditional (optional plugin) |
| Arm ASR | MIT [S24] | Yes | Yes | No |
| Snapdragon GSR | BSD-3 [S25] | Yes | Yes | No |
| NVIDIA NIS | MIT [S27] | Yes | Yes | No |
| ReShade (incl. FX compiler) | BSD-3 (API header BSD-3 OR MIT) [S36][S37] | Yes | Yes (notice) | No |
| iMMERSE shaders | All rights reserved [S40] | No | No | **Yes**, do not bundle |
| SweetFX | MIT [S41] | Yes | Yes | No |
| Special K | GPL-3.0 [S63] | **No** (would force GPL) | – | **Yes** for code reuse |
| ENBSeries (binaries, presets, param names) | Proprietary EULA forbidding parsing and conversion [S32] | **No** | **No** | **Yes**, no ENB import, naming or compatibility claims |
| Intel OIDN | Apache-2.0 [S57] | Yes | Yes | No (patent clause fine) |
| XeGTAO | MIT, archived [S47] | Yes | Yes | No |
| NVlabs FLIP | BSD-3 [S76] | Yes | Yes | No |

---

## Sources

All accessed 2026-09-23.

- [S1] NVIDIA Newsroom, "NVIDIA DLSS 5 Delivers AI-Powered Breakthrough in Visual Fidelity for Games" (2026-03-16). https://nvidianews.nvidia.com/news/nvidia-dlss-5-delivers-ai-powered-breakthrough-in-visual-fidelity-for-games
- [S2] NVIDIA GeForce News, "NVIDIA DLSS 5 Delivers AI-Powered Breakthrough In Visual Fidelity For Games". https://www.nvidia.com/en-us/geforce/news/dlss5-breakthrough-in-visual-fidelity-for-games/
- [S3] Tom's Hardware, "Nvidia's controversial DLSS 5 will launch September 3 with NBA2K27…". https://www.tomshardware.com/pc-components/gpus/nvidias-controversial-dlss-5-will-launch-september-3-with-nba2k27-available-on-all-rtx-50-series-gpus-laptops-and-geforce-now
- [S4] NVIDIA GeForce News, "New DLSS 4 Games, Plus DLSS 5 Announced At GTC 2026". https://www.nvidia.com/en-eu/geforce/news/death-stranding-2-crimson-desert-dlss-4-multi-frame-gen
- [S5] NVIDIA GeForce News, "DLSS 5 3D-Guided Neural Rendering Debuts in NBA 2K27" (2026-09-01). https://www.nvidia.com/en-us/geforce/news/dlss-5-3d-guided-neural-rendering/
- [S6] NVIDIA Technical Blog, "What's New for Game Developers: DLSS 5 with 3D-Guided Neural Rendering, NVIDIA ACE Updates, and New RTX Kit Capabilities" (2026-09-22). https://developer.nvidia.com/blog/whats-new-for-game-developers-dlss-5-with-3d-guided-neural-rendering-nvidia-ace-updates-and-new-rtx-kit-capabilities/
- [S7] GameDev.net mirror of [S6] (search summary: Streamline and UE5 plugin; "independent, optional feature…"). https://gamedev.net/news/whats-new-for-game-developers-dlss-5-with-3d-guided-neural-rendering-nvidia-ace-updates-and-new-rtx-kit-capabilities-r5948/
- [S8] NVIDIA GeForce News, "NVIDIA DLSS 4.5 Delivers Major Upgrade With 2nd Gen Transformer Model For Super Resolution & 6X Dynamic Multi Frame Generation". https://www.nvidia.com/en-us/geforce/news/dlss-4-5-dynamic-multi-frame-gen-6x-2nd-gen-transformer-super-res/
- [S9] Tom's Hardware, "Nvidia introduces DLSS 4.5 and Multi Frame Generation 6X at CES 2026". https://www.tomshardware.com/pc-components/cpus/nvidia-introduces-dlss-4-5-and-multi-frame-generation-6x-at-ces-2026-updated-models-can-generate-higher-quality-upscaled-frames-and-more-of-them-dynamically
- [S10] NVIDIA, DLSS technology page (feature/GPU compatibility table). https://www.nvidia.com/en-us/geforce/technologies/dlss/
- [S11] NVIDIA-RTX/Streamline repository and README (v2.14.1). https://github.com/NVIDIA-RTX/Streamline and https://raw.githubusercontent.com/NVIDIA-RTX/Streamline/main/README.md
- [S11a] Streamline licence file. https://raw.githubusercontent.com/NVIDIA-RTX/Streamline/main/license.txt
- [S12] Streamline, "Programming Guide – DLSS-G". https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS_G.md
- [S12a] NVIDIA/DLSS repository, `lib/` directory (Linux_x86_64, Linux_aarch64, Windows_*). https://github.com/NVIDIA/DLSS/tree/main/lib
- [S12b] NVIDIA Driver Installation Guide, "DLSS / Smooth Motion / Reflex" (Linux). https://docs.nvidia.com/datacenter/tesla/driver-installation-guide/gaming.html
- [S12c] Streamline, "Programming Guide – DLSS". https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS.md
- [S13] NVIDIA RTX SDKs License (DLSS repo LICENSE.txt). https://raw.githubusercontent.com/NVIDIA/DLSS/main/LICENSE.txt
- [S14] Streamline, "Programming Guide – DLSS-RR". https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS_RR.md
- [S15] AMD GPUOpen, "Welcome to the AMD FidelityFX SDK 1.1.4" (manual, effect list, licences, APIs). https://gpuopen.com/manuals/fidelityfx_sdk/
- [S16] AMD GPUOpen, "AMD FidelityFX Super Resolution 3 (FSR 3)". https://gpuopen.com/fidelityfx-super-resolution-3/
- [S17] AMD GPUOpen, "AMD FidelityFX SDK 2.0 launches our neural rendering technologies for developers" (2025-08-20). https://gpuopen.com/learn/amd-fidelityfx-sdk-2-0/
- [S18] GPUOpen-LibrariesAndSDKs/FidelityFX-SDK repository (FSR SDK 2.3.0; "Vulkan is currently not supported in SDK"). https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK
- [S19] AMD GPUOpen, "AMD FSR Upscaling 4.1 RDNA 3 Support Now Available in FSR SDK 2.3 Update" (2026-06-24). https://gpuopen.com/learn/amd-fsr-sdk-2-3-blog/
- [S20] AMD GPUOpen, "AMD FidelityFX Super Resolution 1". https://gpuopen.com/fidelityfx-superresolution/
- [S21] intel/xess repository. https://github.com/intel/xess
- [S21a] intel/xess release SDK 2.0.1. https://github.com/intel/xess/releases/tag/v2.0.1 and releases list https://github.com/intel/xess/releases
- [S21b] Intel Simplified Software License (XeSS LICENSE.txt). https://raw.githubusercontent.com/intel/xess/main/LICENSE.txt
- [S22] VideoCardz, "AMD confirms FSR4 'open-source' release was a mistake…". https://videocardz.com/newz/amd-confirms-fsr4-open-source-release-was-a-mistake-but-the-mit-license-may-be-hard-to-undo ; Tom's Hardware. https://www.tomshardware.com/pc-components/gpus/amd-accidentally-marks-fsr-4-open-source-source-code-reveals-potential-support-for-older-radeon-gpus
- [S23] Tom's Hardware, "Intel shares XeSS 3.0 SDK for game devs with 3x and 4x MFG modes — but it still hasn't followed through on its open-source promise". https://www.tomshardware.com/pc-components/gpu-drivers/intel-shares-xess-3-0-sdk-for-game-devs-with-3x-and-4x-mfg-modes-but-it-still-hasnt-followed-through-on-its-open-source-promise
- [S24] arm/accuracy-super-resolution-generic-library. https://github.com/arm/accuracy-super-resolution-generic-library
- [S25] SnapdragonGameStudios/snapdragon-gsr. https://github.com/SnapdragonGameStudios/snapdragon-gsr
- [S26] Apple WWDC25 session 211, "Go further with Metal 4 games". https://developer.apple.com/videos/play/wwdc2025/211/
- [S27] NVIDIAGameWorks/NVIDIAImageScaling. https://github.com/NVIDIAGameWorks/NVIDIAImageScaling
- [S28] Microsoft DirectX Developer Blog, "DirectSR Preview Available Now". https://devblogs.microsoft.com/directx/directsr-preview/
- [S28a] DirectX-Specs, "DirectSR". https://microsoft.github.io/DirectX-Specs/DirectSR/DirectSR.html
- [S29] Epic Games, "Temporal Super Resolution in Unreal Engine". https://dev.epicgames.com/documentation/en-us/unreal-engine/temporal-super-resolution-in-unreal-engine
- [S30] ENBSeries home page. http://enbdev.com/index.html
- [S31] ENBSeries, "Description". http://enbdev.com/description_en.htm
- [S32] ENBSeries, "License" (EULA revised 2026-08-06). http://enbdev.com/license_en.htm
- [S33] ENBSeries, "News". http://enbdev.com/news.html
- [S33a] ENBSeries v0.505 for TES Skyrim SE (changelog). http://enbdev.com/mod_tesskyrimse_v0505.htm
- [S34] Tech Insider, "Nvidia DLSS 5 Launches September 3, RTX 40 Waits" (secondary source; RTX 40 claim unconfirmed by NVIDIA). https://tech-insider.org/nvidia-dlss-5-siggraph-2026/
- [S35] ReShade home page (v6.8.0, 2026-08-02). https://reshade.me/
- [S36] ReShade LICENSE.md (BSD-3-Clause). https://raw.githubusercontent.com/crosire/reshade/main/LICENSE.md
- [S37] ReShade source headers: `source/effect_parser_stmt.cpp`, `source/effect_lexer.cpp`, `source/effect_codegen_spirv.cpp` (SPDX BSD-3-Clause); `include/reshade_api.hpp` (SPDX BSD-3-Clause OR MIT). https://github.com/crosire/reshade/tree/main/source
- [S38] STEP Modifications wiki, "SkyrimSE: ENBSeries INI Reference / Effect". https://stepmodifications.org/wiki/SkyrimSE:ENBSeries_INI_Reference/Effect
- [S39] ENBSeries, "Effect" (effect.txt). http://enbdev.com/effect_en.htm
- [S40] martymcmodding/iMMERSE repository and LICENSE. https://github.com/martymcmodding/iMMERSE
- [S40a] Marty's Mods. https://www.martysmods.com/home/
- [S41] CeeJayDK/SweetFX LICENSE (MIT). https://github.com/CeeJayDK/SweetFX
- [S42] STEP wiki, "SkyrimSE: ENBSeries INI Reference / Timeofday". https://stepmodifications.org/wiki/SkyrimSE:ENBSeries_INI_Reference/Timeofday
- [S43] STEP wiki, "ENBSeries – SSE Weather System". https://stepmodifications.org/wiki/SkyrimSE:ENBSeries_INI_Reference/Weather
- [S44] PlayStation.Blog, "Upgraded PSSR upscaler is coming to PS5 Pro" (2026-02-27). https://blog.playstation.com/2026/02/27/upgraded-pssr-upscaler-is-coming-to-ps5-pro/
- [S45] G. Wihlidal, "4K Checkerboard in Battlefield 1 and Mass Effect Andromeda", GDC 2017. https://www.ea.com/frostbite/news/4k-checkerboard-in-battlefield-1-and-mass-effect-andromeda
- [S46] L. Yang, S. Liu, M. Salvi, "A Survey of Temporal Antialiasing Techniques", CGF / Eurographics 2020. http://behindthepixels.io/assets/files/TemporalAA.pdf
- [S47] GameTechDev/XeGTAO. https://github.com/GameTechDev/XeGTAO
- [S48] NVIDIA Developer, "Reflex SDK". https://developer.nvidia.com/performance-rendering-tools/reflex
- [S49] VideoCardz, "NVIDIA Reflex 2 remains unreleased 18 months after announcement". https://videocardz.com/newz/nvidia-reflex-2-remains-unreleased-18-months-after-announcement-rtx-owners-still-waiting
- [S50] GPUOpen-LibrariesAndSDKs/AntiLag2-SDK; GPUOpen integration guide. https://github.com/GPUOpen-LibrariesAndSDKs/AntiLag2-SDK ; https://gpuopen.com/learn/integrating-amd-radeon-anti-lag-2-sdk-in-your-game/
- [S51] Khronos Blog, "VK_EXT_present_timing: the Journey to State-of-the-Art Frame Pacing in Vulkan". https://www.khronos.org/blog/vk-ext-present-timing-the-journey-to-state-of-the-art-frame-pacing-in-vulkan
- [S52] NVIDIA-RTX/NRD repository and LICENSE.txt. https://github.com/NVIDIA-RTX/NRD
- [S53] NVIDIA-RTX/RTXDI repository and LICENSE.txt. https://github.com/NVIDIA-RTX/RTXDI
- [S54] NVIDIA-RTX/RTXPT repository and LICENSE.txt. https://github.com/NVIDIA-RTX/RTXPT
- [S55] AMD, AFMF product page (search summary; page returned 503 on direct fetch) and VideoCardz AFMF 2 coverage. https://amd.com/en/products/software/adrenalin/afmf.html ; https://videocardz.com/newz/amd-introduces-fluid-motion-frames-2-better-frame-generation-with-lower-latency
- [S56] Lossless Scaling home page and coverage. https://losslessscaling.com/ ; https://videocardz.com/newz/lossless-scaling-3-released-with-frame-generation-up-to-x20-no-shrooms-required
- [S57] RenderKit/oidn. https://github.com/RenderKit/oidn
- [S58] C. Schied et al., "Spatiotemporal Variance-Guided Filtering", HPG 2017. https://research.nvidia.com/publication/2017-07_spatiotemporal-variance-guided-filtering-real-time-reconstruction-path-traced
- [S59] C. Schied, C. Peters, C. Dachsbacher, "Gradient Estimation for Real-Time Adaptive Temporal Filtering", HPG 2018. https://cg.ivd.kit.edu/publications/2018/adaptive_temporal_filtering/adaptive_temporal_filtering.pdf
- [S60] PC Gamer, "Final Fantasy 14 dispute ends in the death of its most popular mod". https://www.pcgamer.com/final-fantasy-14-dispute-ends-in-the-death-of-its-most-popular-mod/
- [S60a] Mortalitas/GShade (mirror). https://github.com/Mortalitas/GShade
- [S61] NVIDIAGameWorks/rtx-remix. https://github.com/NVIDIAGameWorks/rtx-remix
- [S62] NVIDIA, "Test Drive The New NVIDIA App Beta" (RTX HDR, RTX Dynamic Vibrance, Freestyle). https://www.nvidia.com/en-us/geforce/news/nvidia-app-beta-download/
- [S63] SpecialKO/SpecialK. https://github.com/SpecialKO/SpecialK
- [S64] Microsoft Support, "Use Auto HDR for better gaming in Windows". https://support.microsoft.com/en-us/windows/hardware/display-graphics/use-auto-hdr-for-better-gaming-in-windows
- [S65] NVIDIA-RTX/RTX-Kit. https://github.com/NVIDIA-RTX/RTX-Kit
- [S66] NVIDIA-RTX/RTXGI (v2.x with NRC and SHaRC). https://github.com/NVIDIA-RTX/RTXGI
- [S67] DirectX Developer Blog, "Shader Model 6.9 and The Future of Cooperative Vector" (2025-09-11). https://devblogs.microsoft.com/directx/shader-model-6-9-and-the-future-of-cooperative-vector/
- [S68] DirectX Developer Blog, "D3D12 LinAlg Matrix Preview" and "Announcing Shader Model 6.10 Preview and AgilitySDK 720 Preview". https://devblogs.microsoft.com/directx/d3d12-linalg-preview/ ; https://devblogs.microsoft.com/directx/shader-model-6-10-agilitysdk-720-preview/
- [S69] DirectX Developer Blog, "Evolving DirectX for the ML Era on Windows" (2026-03-12). https://devblogs.microsoft.com/directx/evolving-directx-for-the-ml-era-on-windows/
- [S70] Vulkan Documentation, "VK_KHR_cooperative_matrix" proposal. https://docs.vulkan.org/features/latest/features/proposals/VK_KHR_cooperative_matrix.html
- [S70a] Vulkan Documentation, "VK_NV_cooperative_matrix2" proposal. https://docs.vulkan.org/features/latest/features/proposals/VK_NV_cooperative_matrix2.html
- [S71] Khronos Vulkan-Docs, `appendices/VK_NV_cooperative_vector.adoc`. https://raw.githubusercontent.com/KhronosGroup/Vulkan-Docs/main/appendices/VK_NV_cooperative_vector.adoc
- [S72] NVIDIA-RTX/RTXNTC. https://github.com/NVIDIA-RTX/RTXNTC
- [S73] NVIDIA-RTX/RTXNS. https://github.com/NVIDIA-RTX/RTXNS
- [S74] AMD GPUOpen, "AMD FidelityFX Contrast Adaptive Sharpening (CAS)". https://gpuopen.com/fidelityfx-cas/
- [S75] P. Andersson et al., "FLIP: A Difference Evaluator for Alternating Images", PACMCGIT 3(2), 2020. https://dl.acm.org/doi/10.1145/3406183
- [S76] NVlabs/flip (LDR-FLIP and HDR-FLIP; BSD-3). https://github.com/NVlabs/flip

*Caveats:*
- Some details come from secondary press ([S3], [S9], [S22], [S23], [S34], [S49], [S55], [S56]) where
  primary pages were truncated or unavailable. They are marked where they matter; RTX 40 support for
  DLSS 5 in particular is unconfirmed.
- XeSS release dates were not reliably extracted from the GitHub releases page and are omitted.
- Streamline Linux support is inferred from the absence of Linux build and requirements documentation in
  the README.
