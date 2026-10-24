# FUSE: third-party components

This file lists upstream libraries that the FUSE product graph vendors under [`vendor/`](vendor/) and consumes through [`cmake/FuseVendor.cmake`](cmake/FuseVendor.cmake) (`FUSE_VENDOR_DIR`, `fuse_vendor_path`, `fuse_stb_include`). Version pins live in each tree's `VERSION` file where present; `fuse_lint vendored-pins` gates enforce them. CPack installs a subset of licence texts next to the shipped tools (see [`cmake/FusePackaging.cmake`](cmake/FusePackaging.cmake)).

FUSE itself is AGPL-3.0 (see [`LICENSE.md`](LICENSE.md)).

**Relight-specific vendoring** (DXVK, dxbc-spirv, xxHash, GDeflate, TinyUSDZ, nested Khronos/OpenVR notices, plugins, and clean-room exclusions) is documented in [`Source/FUSE/Relight/THIRD_PARTY.md`](Source/FUSE/Relight/THIRD_PARTY.md). That file is the authority for the optional Relight stack; this file covers the rest of the product graph and summarises shared trees.

**Usage values:**

| Usage | Meaning |
|---|---|
| **vendored** | Copied into `vendor/<name>`, pinned in `VERSION` where applicable, licence file kept next to it. |
| **headers-only** | Include path or API header only; no upstream library object code linked from this tree. |
| **tool-only** | Build-time download or compiler; not linked into the default runtime. |
| **legacy** | Still present under `vendor/` for Torque-era or future work; not referenced by the FUSE umbrella `CMakeLists.txt`. |

## 1. Core product graph (vendored or headers-only)

| Component | Licence | Version pin | Usage | FUSE location | Licence file(s) | Notice obligations |
|---|---|---|---|---|---|---|
| stb (`stb_image`, `stb_image_write`, `stb_image_resize2`) | MIT / public domain (Sean Barrett) | Informal header tags in each `.h` | headers-only | `vendor/stb` | Licence text at top of each header under `vendor/stb/` | Keep the notice in source distributions that include the headers; stb is compiled into FUSE objects where `STB_*_IMPLEMENTATION` is defined. |
| ENet | MIT (Lee Salzman) | 1.3.7 *(CMake comment in `Source/FUSE/Net`)* | vendored | `vendor/enet` | [`vendor/enet/LICENSE.txt`](vendor/enet/LICENSE.txt) | Keep the copyright and permission notice. Shipped in CPack under `share/doc/fuse/vendor/enet/`. |
| Lua | MIT (PUC-Rio) | 5.2.3 | vendored | `vendor/lua` | [`vendor/lua/LICENSE.txt`](vendor/lua/LICENSE.txt) | Keep the copyright and permission notice. Shipped in CPack under `share/doc/fuse/vendor/lua/`. |
| Bullet Physics | zlib (Erwin Coumans et al.) | 3.25 (`vendor/bullet/VERSION`) | vendored | `vendor/bullet` | [`vendor/bullet/LICENSE.txt`](vendor/bullet/LICENSE.txt) | zlib clause 2: mark altered sources. `Extras/` and `examples/ThirdPartyLibs/` use other licences if ever enabled. |
| Open Asset Import Library (assimp) | BSD-3-Clause (assimp team); bundled deps listed in the same file | Upstream tree under `vendor/assimp` | vendored | `vendor/assimp` | [`vendor/assimp/LICENSE`](vendor/assimp/LICENSE) | Keep the BSD-3 notice and follow assimp's bundled-component list when redistributing binaries that link assimp (e.g. `fuse_cook`). |
| OpenAL Soft | LGPL-2.0+ | Upstream tree under `vendor/openal-soft` | vendored | `vendor/openal-soft` | [`vendor/openal-soft/COPYING`](vendor/openal-soft/COPYING) | LGPL: provide licence text and offer corresponding source for the linked library when distributing binaries that embed or link it (`fuse_audio` backend). |
| Xiph libogg | BSD-3-Clause | 1.3.6 — archive `vendor/xiph-ogg-v1.3.6.tar.gz`, pin [`vendor/xiph/ogg/VERSION`](vendor/xiph/ogg/VERSION) | vendored (extract-at-configure) | `vendor/xiph-ogg-v1.3.6.tar.gz`, [`vendor/xiph/ogg/COPYING`](vendor/xiph/ogg/COPYING) | [`vendor/xiph/ogg/COPYING`](vendor/xiph/ogg/COPYING) | Keep the copyright and conditions in source and binary distributions of the static libraries built by [`cmake/FuseXiph.cmake`](cmake/FuseXiph.cmake). |
| Xiph libvorbis (+ enc/file) | BSD-3-Clause | 1.3.7 — `vendor/xiph-vorbis-v1.3.7.tar.gz`, pin [`vendor/xiph/vorbis/VERSION`](vendor/xiph/vorbis/VERSION) | vendored (extract-at-configure) | `vendor/xiph-vorbis-v1.3.7.tar.gz`, [`vendor/xiph/vorbis/COPYING`](vendor/xiph/vorbis/COPYING) | [`vendor/xiph/vorbis/COPYING`](vendor/xiph/vorbis/COPYING) | Same as libogg. |
| Xiph libFLAC | BSD-3-Clause | 1.5.0 — `vendor/xiph-flac-1.5.0.tar.gz`, pin [`vendor/xiph/flac/VERSION`](vendor/xiph/flac/VERSION) | vendored (extract-at-configure) | `vendor/xiph-flac-1.5.0.tar.gz`, [`vendor/xiph/flac/COPYING.Xiph`](vendor/xiph/flac/COPYING.Xiph) | [`vendor/xiph/flac/COPYING.Xiph`](vendor/xiph/flac/COPYING.Xiph) | Same as libogg. |
| Vulkan Memory Allocator (VMA) | MIT (AMD) | Pin in [`vendor/vma/VERSION`](vendor/vma/VERSION) | headers-only | `vendor/vma` | [`vendor/vma/LICENSE.txt`](vendor/vma/LICENSE.txt) | Keep the copyright and permission notice. Shipped in CPack under `share/doc/fuse/vendor/vma/`. |
| volk | MIT (Arseny Kapoulkine) | Pin in [`vendor/volk/VERSION`](vendor/volk/VERSION) | headers-only | `vendor/volk` | [`vendor/volk/LICENSE.md`](vendor/volk/LICENSE.md) | Keep the copyright and permission notice. |
| meshoptimizer | MIT (Arseny Kapoulkine) | Pin in [`vendor/meshoptimizer/VERSION`](vendor/meshoptimizer/VERSION) | vendored | `vendor/meshoptimizer` | [`vendor/meshoptimizer/LICENSE.md`](vendor/meshoptimizer/LICENSE.md) | Keep the copyright and permission notice. |
| AMD FidelityFX SDK (subset) | MIT (Advanced Micro Devices, Inc.) | v1.1.4 — pin in [`vendor/fidelityfx/VERSION`](vendor/fidelityfx/VERSION) | vendored (headers + GLSL compiled to SPIR-V) | `vendor/fidelityfx` | [`vendor/fidelityfx/LICENSE.txt`](vendor/fidelityfx/LICENSE.txt) | Keep the copyright and permission notice; FSR/CAS passes are built verbatim from the vendored GLSL. |
| NVIDIA Image Scaling (NIS) | MIT (NVIDIA) | v1.0.3 — pin in [`vendor/nvidia-nis/VERSION`](vendor/nvidia-nis/VERSION) | headers-only (+ GLSL) | `vendor/nvidia-nis` | [`vendor/nvidia-nis/licence.txt`](vendor/nvidia-nis/licence.txt) | Keep the copyright and permission notice. |
| NVIDIA FLIP (LDR metric) | BSD-3-Clause (NVIDIA) | Pin in [`vendor/nvidia-flip/VERSION`](vendor/nvidia-flip/VERSION) | headers-only | `vendor/nvidia-flip` | [`vendor/nvidia-flip/LICENSE`](vendor/nvidia-flip/LICENSE) | Keep the copyright and conditions; used for content golden tests, not the shipping game runtime. |
| RenderDoc in-app API | MIT (Baldur Karlsson) | Pin in [`vendor/renderdoc/VERSION`](vendor/renderdoc/VERSION) | headers-only | `vendor/renderdoc/include/renderdoc_app.h` | MIT text in [`vendor/renderdoc/include/renderdoc_app.h`](vendor/renderdoc/include/renderdoc_app.h) | Keep the copyright and permission notice in the header and in shipped notices if the API surface is redistributed. |
| NVIDIA Streamline (public headers) | MIT (NVIDIA) | Pin in [`vendor/streamline/VERSION`](vendor/streamline/VERSION) | headers-only | `vendor/streamline` | [`vendor/streamline/LICENSE.txt`](vendor/streamline/LICENSE.txt) | Keep the MIT notice for vendored headers. Runtime DLLs are user-installed plugins (`docs/nvidia-plugin.md`); never committed. |
| Tracy client | BSD-3-Clause (Bartosz Taudul) | Pin in [`vendor/tracy/VERSION`](vendor/tracy/VERSION) | vendored (optional, `-DFUSE_TRACY=ON`) | `vendor/tracy` | [`vendor/tracy/LICENSE`](vendor/tracy/LICENSE) | Keep the copyright and conditions when linking the Tracy client into profiler builds. |
| Open Sans (font) | Apache-2.0 (Google) | Pin in [`vendor/fonts/opensans/VERSION`](vendor/fonts/opensans/VERSION) | vendored | `vendor/fonts/opensans` | [`vendor/fonts/opensans/LICENSE.txt`](vendor/fonts/opensans/LICENSE.txt) | Apache-2.0: ship licence text with font binaries (editor Qt resource). Shipped in CPack under `share/doc/fuse/vendor/opensans/`. |
| Khronos Vulkan headers (from SDL tree path) | Apache-2.0 OR MIT (Khronos) | Bundled under vendored SDL sources | headers-only | `vendor/sdl/src/video/khronos` | Copyright block in headers (e.g. `vulkan.h`); SDL itself is not built by FUSE | Used when no Vulkan SDK is found ([`cmake/FuseFindVulkan.cmake`](cmake/FuseFindVulkan.cmake), Relight MinGW). Keep Khronos licence terms if redistributing the headers. |

## 2. Shared with FUSE Relight (see Relight doc for detail)

These trees sit under `vendor/` and are also consumed by the optional Relight module. Pin sets, patch markers, nested licences (Vulkan-Headers, SPIRV-Headers, OpenVR, libdisplay-info), and packaging rules are spelled out in [`Source/FUSE/Relight/THIRD_PARTY.md`](Source/FUSE/Relight/THIRD_PARTY.md).

| Component | Licence (summary) | Tree | Primary licence file(s) |
|---|---|---|---|
| DXVK (d3d8/d3d9 subset) | zlib | `vendor/dxvk` | [`vendor/dxvk/LICENSE`](vendor/dxvk/LICENSE) (+ nested files listed in Relight doc) |
| dxbc-spirv | MIT | `vendor/dxbc-spirv` | [`vendor/dxbc-spirv/LICENSE`](vendor/dxbc-spirv/LICENSE) |
| xxHash | BSD-2-Clause | `vendor/xxhash` | [`vendor/xxhash/LICENSE`](vendor/xxhash/LICENSE) |
| GDeflate CPU codec | MIT (libdeflate) + Apache-2.0 (DirectStorage GDeflate) | `vendor/gdeflate` | [`vendor/gdeflate/libdeflate/COPYING`](vendor/gdeflate/libdeflate/COPYING), [`vendor/gdeflate/GDeflate/LICENSE`](vendor/gdeflate/GDeflate/LICENSE) |
| TinyUSDZ (subset) | Apache-2.0 (+ bundled licences in tree) | `vendor/tinyusdz` | [`vendor/tinyusdz/LICENSE`](vendor/tinyusdz/LICENSE) and paths in Relight doc §1 |

## 3. Build and tool inputs (not vendored source in git)

| Component | Licence | Version pin | Usage | FUSE location | Notice obligations |
|---|---|---|---|---|---|
| Slang compiler (`slangc`) | Apache-2.0 WITH LLVM-exception | [`vendor/slang/VERSION`](vendor/slang/VERSION) — download pin only | tool-only | Downloaded to the cache by [`cmake/FuseSlang.cmake`](cmake/FuseSlang.cmake); no Slang source in `vendor/slang/` | No runtime notice unless a Slang runtime library is shipped; then ship licence and NOTICE from the downloaded release. |

## 4. Trees under `vendor/` not used by the FUSE product graph

The following directories remain in the repository (mostly Torque-era ore or future ports) but are **not** referenced from the root FUSE umbrella [`CMakeLists.txt`](CMakeLists.txt) / `Source/FUSE/**` product CMake. They are omitted from packaging and from `fuse_lint vendored-pins` unless noted otherwise.

| Path | Typical upstream licence | Notes |
|---|---|---|
| `vendor/Torque3D`, `vendor/nativeFileDialogs`, `vendor/CMakeLists.txt` | mixed | Legacy Torque vendor layout; [`Templates/BaseGame/CMakeLists.txt`](Templates/BaseGame/CMakeLists.txt) still points at `Tools/CMake/`, not FUSE. |
| `vendor/gtest` | BSD-3-Clause | GoogleTest; FUSE tests use the in-tree harness, not this copy. |
| `vendor/sqlite`, `vendor/zlib`, `vendor/lpng`, `vendor/squish`, `vendor/tinyxml2`, `vendor/pcre` | per upstream | Not wired into `Source/FUSE/` today. |
| `vendor/recast`, `vendor/opcode`, `vendor/convexMath`, `vendor/collada`, `vendor/glad` | per upstream | Not wired into `Source/FUSE/` today. |
| `vendor/sdl` (full library) | zlib (SDL) | Only the Khronos header path in §1 is used; SDL is not built as a FUSE target. |
| `vendor/ispc_texcomp/ispc_texcomp.h` | — | FUSE-authored API stub for cook hooks when Intel's library is absent; not third-party code. |

Planned Relight overlay dependencies (for example upstream Dear ImGui in `vendor/imgui`) are listed in [`Source/FUSE/Relight/THIRD_PARTY.md`](Source/FUSE/Relight/THIRD_PARTY.md) when they land.
