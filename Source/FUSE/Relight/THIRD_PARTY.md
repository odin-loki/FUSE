# FUSE Relight: third-party components

This file lists every upstream component that FUSE Relight vendors, ports, loads as a plugin or has decided to drop. It follows the licence findings in [`docs/plans/FUSE_REMIX_PORT_PLAN.md`](../../../docs/plans/FUSE_REMIX_PORT_PLAN.md) §0.3 and the attribution plan in §0.4. The packager (RL-6.4) builds `THIRD_PARTY_NOTICES.txt` from this file and from each `Engine/lib/*/LICENSE*`, and ships it next to the DLLs.

FUSE Relight itself is MIT.

**Usage values:**

| Usage | Meaning |
|---|---|
| **vendored** | Copied verbatim into `Engine/lib/<name>`, pinned in `VERSION` (tag, commit, sha256 per file), licence file kept next to it. |
| **ported** | MIT or zlib code moved into `Source/FUSE/Relight` and modified. The upstream notice is kept in every file. |
| **reimplemented** | FUSE's own code, written from papers or specifications. Nothing upstream is read or copied (clean room). |
| **plugin** | An optional module loaded at runtime. The user installs the binary. It is never committed. |
| **tool-only** | Used by an offline tool or in CI. Not linked into the runtime and not shipped. |
| **dropped** | Not used. |

A pin marked *(set by RL-x.y)* is recorded in that package's `VERSION` file when it lands. Until then, the version shown is the planned one.

## 1. Components that ship (vendored or ported)

| Component | Licence | Version pin | Usage | FUSE location | Notice obligations |
|---|---|---|---|---|---|
| DXVK (`doitsujin/dxvk`), subset `src/{d3d8,d3d9,dxvk,util,wsi,spirv,vulkan}` plus shader GLSL | zlib (Philip Rebohle, Joshua Ashton) | 3.1.1, tag `v3.1.1` = `b1a1c99ab52b` (sha256 per file in `Engine/lib/dxvk/VERSION`) | vendored, with a patch set | `Engine/lib/dxvk` | Keep `LICENSE`. **zlib clause 2:** mark every altered source. Each edit sits inside `// FUSE-DXVK begin: <reason>` / `// FUSE-DXVK end` and is listed in `Engine/lib/dxvk/PATCHES.md`. `rl_dxvk_pins` proves no unmarked edits exist. Do not misrepresent the origin. |
| Build inputs bundled with DXVK, kept by RL-0.2 as the minimum the unpatched d3d9/d3d8 build needs: Vulkan-Headers (`include/vulkan`: `vk_platform.h`, `vulkan.h`, `vulkan_core.h`, `vulkan_win32.h`, `vk_video/*.h`), SPIRV-Headers (`include/spirv`: `spirv.hpp`, `GLSL.std.450.h`), OpenVR (`include/openvr/openvr.hpp`), libdisplay-info (`subprojects/libdisplay-info`, DXVK's `windows` branch) | Apache-2.0 OR MIT (Khronos); MIT (Khronos); BSD-3 (Valve); MIT (libdisplay-info contributors) | DXVK v3.1.1 gitlinks: Vulkan-Headers `8864cdc896bb`, SPIRV-Headers `04f10f650d51`, libdisplay-info `275e6459c7ab`; OpenVR is in the DXVK tree itself. sha256 per file in `Engine/lib/dxvk/VERSION`. | vendored. Headers are build-only. libdisplay-info is compiled into the DLLs. | `Engine/lib/dxvk/include/{vulkan,spirv,openvr}`, `Engine/lib/dxvk/subprojects/libdisplay-info` | Keep each `LICENSE`. Ship the libdisplay-info MIT notice and the OpenVR BSD-3 notice, since both are compiled into the DLLs. For Apache-2.0, keep the notice text in the shipped notices file. Why FUSE's own Vulkan headers are not used: FUSE's are 1.3.275, and DXVK 3.1.1 needs about 100 newer symbols (descriptor heap, maintenance7–11, device fault). OpenVR stays because `dxvk_instance.cpp` always registers the OpenVR provider; dropping it would need a FUSE-DXVK patch. |
| dxbc-spirv (`doitsujin/dxbc-spirv`), including the SM1–3 front end `sm3/` | MIT (Philip Rebohle 2025) | DXVK v3.1.1 gitlink `bf14419e5fa7` (no upstream tags; `VERSION` records meson's `0.1.0`) | vendored | `Engine/lib/dxbc-spirv` | Keep the copyright and permission notice in the sources and in the shipped notices. |
| SPIRV-Headers (submodule of dxbc-spirv) | MIT (Khronos) | dxbc-spirv gitlink `c8ad050fcb29` (`spirv.hpp`, `GLSL.std.450.h` only) | vendored | `Engine/lib/dxbc-spirv/submodules/spirv_headers` | Keep `LICENSE`. |
| dxvk-remix runtime code (`src/dxvk/rtx_render/**`, `src/d3d9/d3d9_rtx*`, `src/lssusd/**`, `src/usd-plugins/**`, `src/ogn/**`, `dxvk_imgui*.cpp`, `rtx_user_menu.cpp`, `public/include/remix/*.h`) | MIT, NVIDIA 2021–2026 (`LICENSE-MIT`, per-file headers) | `NVIDIAGameWorks/dxvk-remix` `0867d3c748a7` (`remix-main`) | ported (selectively, §3 of the plan) | `Source/FUSE/Relight/**` | Keep the NVIDIA MIT header verbatim in every ported file. Add `Modifications Copyright (c) 2026 FUSE contributors (MIT)` and `// Ported from dxvk-remix <path>@0867d3c`. |
| dxvk-remix MIT shaders (`src/dxvk/shaders/rtx/**`, **except** the five proprietary files in §2) | MIT, NVIDIA | `0867d3c748a7` | ported to FUSE Slang/GLSL | `Source/FUSE/Relight/shaders/**` | Same as the row above. |
| Remix bridge (`bridge/**`, formerly bridge-remix) | MIT, NVIDIA (`bridge/LICENSE-MIT`) | `0867d3c748a7` | ported and revamped (generated command schema) | `Source/FUSE/Relight/bridge/**` | Same as the dxvk-remix runtime row. |
| xxHash (Yann Collet) | BSD-2-Clause | ≥ 0.8.0; XXH3 output is stable from 0.8.0 on *(exact tag set by RL-0.5)* | vendored | `Engine/lib/xxhash` | Keep the copyright notice, conditions and disclaimer in the sources, and reproduce them in the shipped notices (binary redistribution). |
| Dear ImGui + ImPlot (upstream, not Remix's copy) | MIT | *(set by the overlay package, RL-6.x)* | vendored | `Engine/lib/imgui` | Keep the copyright and permission notice. |
| TinyUSDZ (shared with Remaster W2.2) | Apache-2.0 | *(set by Remaster W2.2 / RL-3.x)* | vendored | shared location per Remaster W2.2 | Ship the licence text. Keep any upstream `NOTICE` in the shipped notices. State modifications in the modified files. |
| GDeflate reference CPU codec (`microsoft/DirectStorage/GDeflate`) | Apache-2.0 (the DirectStorage repo itself is MIT) | *(set by RL-3.x)* | vendored (decode for `.pkg` / `.rtxio` mod packages; replaces rtxio) | `Engine/lib/gdeflate` | Keep `LICENSE` and `NOTICE`, carry the NOTICE into the shipped notices, and mark modified files. |
| Slang compiler (build tool) | Apache-2.0 WITH LLVM-exception | pinned binary per renderer WP-0.5 | tool-only (shader build) | renderer toolchain | No runtime notice unless the Slang runtime library is shipped. If it is, ship the licence and NOTICE. |

## 2. NVIDIA proprietary material: never vendored, never read (clean room)

These files sit inside the MIT dxvk-remix repository but are **not** MIT. FUSE agents never open or copy them. Their functionality is reimplemented from the papers listed in plan §9. `rl_licence_text_scan` enforces this: it fails the build if their licence headers, their file names or their public API prefixes (`RTXDI_*`, `RAB_*`, `RTXCR_*`, `*_FrontEnd_*`) show up in the Relight or vendored trees. This file is the only place allowed to name them.

| Upstream file or SDK | Licence | Usage | FUSE replacement |
|---|---|---|---|
| `shaders/rtx/algorithm/rtxdi/RtxdiApplicationBridge.slangh` | NVIDIA proprietary | dropped (clean room) | ReSTIR DI with GRIS pairwise MIS, light tree (renderer WP-7.1/7.2, plan §5.3) |
| `shaders/rtx/algorithm/rtxcr/rtxcr.slangh`, `rtxcr_material.slangh` | NVIDIA proprietary | dropped (clean room) | Burley/Christensen SSS and Chiang hair (plan §5.2) |
| `shaders/rtx/external/NRC.h` | NVIDIA proprietary | dropped (clean room) | hash-grid radiance cache, with FUSE's own neural cache later (plan §5.4) |
| `shaders/rtx/external/NRD.slangh` | NVIDIA proprietary | dropped (clean room) | in-tree spatiotemporal denoiser (renderer WP-6.4, plan §5.5) |
| `src/mdl/**` (`AperturePBR_*.mdl`, `nvidia/core_definitions.mdl`, …) | NVIDIA proprietary MDL EULA (`src/mdl/PACKAGE-LICENSES`) | dropped | FUSE never compiles MDL (AD-7). Only the Remix material *parameter names*, which are interface facts, are mapped, and they come from the MIT `rtx_material_data.h` (plan §4.5). |
| RTXDI SDK (`submodules/rtxdi`, `remix` branch) | NVIDIA RTX SDKs License | reimplemented | see the RtxdiApplicationBridge row |
| RTXCR Material Library (`submodules/rtxcr`) | NVIDIA RTX SDKs License | reimplemented | see the rtxcr row |
| Neural Radiance Cache (`submodules/nrc`, ships `NRC_Vulkan.dll`, `cudart64_13.dll`) | NVIDIA RTX SDKs License | reimplemented. A plugin is possible but not planned. | see the NRC.h row |
| RTXGI, RTXPT, SHaRC | NVIDIA RTX SDKs License | dropped | FUSE's own DDGI and radiance cache |
| Reflex, RTX IO (`rtxio`), Nsight Aftermath, Nsight SDK, `ngx_sdk_dlnr` (packman) | NVIDIA proprietary | dropped | Reflex goes through the Streamline plugin. GDeflate replaces rtxio. |

## 3. Plugins (binary installed by the user, never committed)

`rl_binary_gate` and `fuse_nvidia_no_committed_binaries` fail if any of these runtimes is tracked by git, or could be added to it. They cover `nvngx*`, `sl.*`, `NRC_*`, `cudart*`, `nvrtc*`, `rtxio*`, `NRD`/`NRI`, Remix `*.mdl` modules, `NvRemix*.exe`, `.trex/`, `remixapi.dll`, `libxess*` and `libxell*`.

| Component | Licence | Version pin | Usage | Notice obligations |
|---|---|---|---|---|
| DLSS SR / RR / FG, NGX (`submodules/dlss`) | NVIDIA RTX SDKs License | whatever the user installs in `FUSE_NVIDIA_SDK_DIR` | plugin (`Source/FUSE/Renderer/plugins/nvidia`: `dlss_sr`, `dlss_rr`) | Redistributable only in object form, for NVIDIA GPUs. FUSE ships no binary, so it carries no notice. See `docs/nvidia-plugin.md`. |
| NRD (packman `rtx-remix-nrd` 4.13; `NVIDIA-RTX/NRD`) | NVIDIA RTX SDKs License | user-installed | plugin (renderer WP-6.4b). The default denoiser is FUSE's own. | As for DLSS. |
| Streamline | MIT headers (vendored); proprietary runtime DLLs | headers per `Engine/lib/streamline/VERSION` | headers vendored (existing); runtime is a plugin | Keep the MIT notice for the headers. The runtime DLLs are never shipped. |
| XeSS (`submodules/xess`) | Intel Simplified Software License (binary only, no modification) | user-installed | plugin (renderer WP-4.3) | The binary is never committed or modified. If a future package ships it, include Intel's licence text. |

## 4. Not vendored: build or tool inputs only

| Component | Licence | Version pin | Usage | Notice obligations |
|---|---|---|---|---|
| DXVK `include/native/directx` (`Joshua-Ashton/mingw-directx-headers`, Wine-derived) | LGPL-2.1+ | DXVK 3.1.1 submodule `9df86f234161` | **not vendored**. PE builds use the MinGW toolchain's system headers. Native-Linux unit builds fetch the pinned headers into `build/` at configure time, never into git. `rl_licence_text_scan` fails on the path or on LGPL text. | None, as long as nothing is vendored or shipped. |
| OpenUSD (packman `open_usd` 25.11; `usd-core` wheel) | Tomorrow Open Source Technology License (Apache-2.0 variant) | wheel version of the tool's environment | tool-only (optional flatten and CI cross-check) | Not shipped. |
| tiny-cuda-nn | BSD-3-Clause | – | tool-only research oracle in `Tools/` (plan §5.4). Not in the runtime. | Not shipped. If it is ever redistributed, keep the BSD-3 notice. |
| Remix Toolkit (`toolkit-remix`) | Apache-2.0 (the Kit runtime is separate and proprietary) | – | reference only: mod and USD conventions. Nothing vendored. | None. |
| `rtx-remix` umbrella repo | MIT | – | reference only (submodule layout) | None. |

## 5. Dropped

| Component | Licence | Reason |
|---|---|---|
| Remix's DXVK fork (`src/d3d9`, `src/dxso`, non-rtx `src/dxvk`, `src/util`) | zlib + NV-DXVK MIT modifications | Upstream DXVK 3.1.1 is vendored instead (AD-1). |
| nvapi (`submodules/nvapi`) | MIT (headers and libs) | Not needed with Vulkan. |
| Detours (`submodules/Detours`) | MIT | The launcher uses `CreateProcess` + `CREATE_SUSPENDED` + a remote `LoadLibrary`. Detours may be added later for API hooks. |
| sse2neon (`include/MathLib/External/sse2neon`) | MIT | x86-64 only for now. |
| Tracy (`src/tracy`, `bridge/src/tracy`) | BSD-3-Clause | Renderer WP-0.6 owns Tracy. |
| gli / glm | MIT | FUSE has its own DDS reader and math. |
| d3d8to9 (crosire) | BSD-2-Clause | DXVK's native `src/d3d8` is used instead (AD-15). |
| Remix's copies of Dear ImGui / ImPlot | MIT | Upstream is vendored instead (§1). |
| GPU crash recorder, Sentry, Nsight, Aftermath integrations | mixed / proprietary | The FUSE crash handler already exists. |

## 6. Trademarks

"NVIDIA", "RTX" and "Remix" are used only nominatively. Examples are "compatible with RTX Remix mods", the `rtx.*` config keys and the `remixapi_*` ABI symbols, all of which interoperability needs. Product, binary and module names are FUSE-native: FUSE Relight, `fuse_relight_host.exe`, `Source/FUSE/Relight`. There are no NVIDIA logos and no "powered by RTX" (plan §0.4.5, AD-13).
