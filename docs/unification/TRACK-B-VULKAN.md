# Track B — Vulkan Bootstrap (B2.1 kickoff)

**Status:** Scaffolding landed — headless instance/device + command-list path; swapchain/present deferred to B2.2  
**Master plan:** [FUSE_MASTER_PLAN.md](../plans/FUSE_MASTER_PLAN.md) §B2.1  
**Threading:** [architecture-parallel.md](./architecture-parallel.md) §4.4, §5.3  
**Hybrid integration:** [U4-HYBRID-FRAME.md](./U4-HYBRID-FRAME.md)

---

## Scope (this PR)

| Component | Location | Notes |
|-----------|----------|-------|
| `RenderCommandList` | `Source/FUSE/Renderer/` | Per-frame draws/clears merged on render thread |
| `VulkanInstance` / `VulkanDevice` | `Source/FUSE/Renderer/include/fuse/renderer/vk/` | Headless bootstrap; optional validation layers |
| `VulkanSwapchain` | same | **Placeholder** — records desc, no `VkSwapchainKHR` yet |
| `RhiContext` | `Source/FUSE/Renderer/` | Accepts command lists on `renderThread()` |
| `fuse::platform::gl_context.hpp` | `Source/FUSE/Core/` | Portable “may touch GPU” guard |
| `HybridComposer` wiring | `Source/FUSE/Hybrid/` | Dual path: software `PlaceholderRenderer` **and** RHI command mirror |

**Not in scope:** Engine marriage, VMA integration, real swapchain/present, MoltenVK/Android surface wiring (stubs documented below).

---

## Build flag — `FUSE_BUILD_VULKAN`

```bash
cmake -B build -DFUSE_UMBRELLA=ON -DFUSE_BUILD_VULKAN=ON
```

| Condition | Behaviour |
|-----------|-----------|
| `FUSE_BUILD_VULKAN=OFF` | No `fuse_rhi` target; Hybrid stays software-only (U4 path unchanged) |
| `FUSE_BUILD_VULKAN=ON`, loader found | `FUSE_VULKAN_BACKEND=1` — real `VkInstance` / `VkDevice` when ICD present |
| `FUSE_BUILD_VULKAN=ON`, loader missing | Stub backend — tests assert graceful degradation |
| CI Linux | Installs `libvulkan-dev` + Mesa Lavapipe; validation layers optional |
| CI Android | `FUSE_BUILD_VULKAN=OFF` until NDK surface bootstrap lands |

CMake discovers Vulkan quietly in `Source/FUSE/CMakeLists.txt`; `fuse_rhi` always builds when the option is ON.

---

## Thread ownership (locked)

Only `fuse::platform::renderThread()` may:

- Create/destroy GPU contexts (v1 bootstrap on same thread as Hybrid `render()`)
- Submit `RenderCommandList` to `RhiContext`

Workers produce snapshot SOA / staging data only. Job code never includes `<vulkan/vulkan.h>`.

`HybridComposer::render()`:

1. Assert render thread (`platform::isRenderThread()`)
2. Execute software placeholder (existing U4 tests)
3. When `FUSE_HAS_VULKAN_RHI`: reset `RenderCommandList`, mirror clears/sprites, `RhiContext::submitFrame()`

---

## Backend modes

```cpp
enum class VulkanBackendMode : u8 { Stub, Headless };
```

| Mode | When | Instance | Device | Swapchain |
|------|------|----------|--------|-----------|
| **Stub** | No loader / no ICD | — | — | placeholder message only |
| **Headless** | Loader + ICD (incl. Lavapipe) | `VkInstance` | `VkDevice` + queues | placeholder (no surface) |

Device extensions from master plan B2.1 are **requested when supported**; missing extensions do not fail headless bootstrap.

VMA (`VmaAllocator`) field exists as `nullptr` — wired in a follow-up once third-party policy lands.

---

## Desktop vs mobile (design notes)

| Platform | B2.1 stance | Later |
|----------|-------------|-------|
| Linux/Windows desktop | Headless bootstrap + Lavapipe/discrete ICD | B2.2 swapchain + Qt viewport |
| macOS | Same API surface; MoltenVK behind platform module | Dedicated `FUSE_PLATFORM_MACOS` RHI backend |
| iOS / Android | Stub compile (`FUSE_BUILD_VULKAN=OFF` in mobile CI) | GLES/Metal/Vulkan queue ownership per §4.4 |

Portable invariant unchanged: job code emits `RenderCommandList`; platform module selects Vulkan/Metal/GLES backend.

---

## Tests

| Target | Validates |
|--------|-----------|
| `fuse_vulkan_bootstrap` | Instance/device or stub path; render-thread submit |
| `fuse_render_command_list` | Hybrid mirrors commands without breaking placeholder pixels |
| `fuse_hybrid_tests` | Existing U4 software renderer regressions |

Run:

```bash
ctest --test-dir build --output-on-failure -R 'fuse_vulkan|fuse_render_command|fuse_hybrid'
```

---

## CI story (honest)

1. **Linux umbrella** — `FUSE_BUILD_VULKAN=ON`, Mesa Lavapipe for headless ICD; Khronos validation layers used when installed, otherwise stub message (non-fatal).
2. **Android NDK** — `FUSE_BUILD_VULKAN=OFF`; `fuse_core` + `fuse_hybrid` unchanged.
3. **iOS stub workflow** — unchanged; Vulkan deferred.

No GPU on runner is OK: stub backend keeps configure/build green; when Lavapipe is present, B2.1 tests exercise real instance/device creation.

---

## B2.4 — Shader system & pipeline compiler (scaffold)

**Status:** Offline SPIR-V load + `VkShaderModule` stub + pipeline layout placeholders landed.

| Component | Location | Notes |
|-----------|----------|-------|
| `ShaderCompiler` | `Source/FUSE/Renderer/include/fuse/renderer/shader/` | Offline-first — loads checked-in `.spv` fixtures |
| `ShaderModule` | same | Creates `VkShaderModule` when `FUSE_VULKAN_BACKEND=1` and device ready |
| `PipelineLayout` | `Source/FUSE/Renderer/include/fuse/renderer/vk/pipeline_layout.hpp` | Placeholder layout (push constants only; bindless sets deferred) |
| Fixtures | `Source/FUSE/Renderer/shaders/fixtures/` | `minimal.vert` / `minimal.frag` + precompiled `.spv` for CI |

### Offline SPIR-V path (CI default)

CI does **not** require glslang. Tests load `minimal.vert.spv` / `minimal.frag.spv` checked into the repo (sibling naming: `source.glsl` → `source.glsl.spv`).

```bash
# Regenerate fixtures locally when GLSL changes (developer machine only):
glslangValidator -V Source/FUSE/Renderer/shaders/fixtures/minimal.vert \
  -o Source/FUSE/Renderer/shaders/fixtures/minimal.vert.spv
spirv-val Source/FUSE/Renderer/shaders/fixtures/minimal.vert.spv
```

Optional runtime glslang (off by default):

```bash
cmake -B build -DFUSE_UMBRELLA=ON -DFUSE_BUILD_VULKAN=ON -DFUSE_SHADER_GLSLANG=ON
```

When `FUSE_SHADER_GLSLANG=ON` but glslang is missing, configure continues with offline SPIR-V only.

### Tests

| Target | Validates |
|--------|-----------|
| `fuse_shader_pipeline` | SPIR-V I/O, offline compiler, shader module + pipeline layout (stub or Vulkan) |

```bash
ctest --test-dir build --output-on-failure -R fuse_shader_pipeline
```

**Not in scope (other agents / later milestones):** swapchain present, VMA integration, bindless descriptor sets, graphics pipeline cache, hot-reload watchers.

---

## Next (B2.2+)

- [ ] `VkSwapchainKHR` + triple-buffered frame ring
- [ ] VMA + bindless descriptor scaffolding (B2.3)
- [ ] Graphics pipeline builder + content-hashed cache (B2.4 follow-up)
- [ ] Replace `PlaceholderRenderer` present path incrementally — keep software fallback for headless CI
- [ ] Editor Qt native surface (`U6` viewport) → `SwapchainDesc.surface`
- [ ] Android Vulkan WSI + MoltenVK macOS module

---

## Related docs

- [work-plan.md](./work-plan.md) — Track B kickoff entry
- [BUILD.md](./BUILD.md) — umbrella CMake options
- [risk-register.md](./risk-register.md) R21 — render thread invariant
