# Track B — Vulkan Bootstrap (B2.1–B2.4)

**Status:** B2.1 bootstrap + B2.2 swapchain/frame ring + B2.3 resource/bindless scaffolding + B2.4 shader scaffold  
**Master plan:** [FUSE_MASTER_PLAN.md](../plans/FUSE_MASTER_PLAN.md) §B2.1–B2.4  
**Threading:** [architecture-parallel.md](./architecture-parallel.md) §4.2, §4.4, §5.3  
**Hybrid integration:** [U4-HYBRID-FRAME.md](./U4-HYBRID-FRAME.md)

---

## Scope

| Component | Location | Notes |
|-----------|----------|-------|
| `RenderCommandList` | `Source/FUSE/Renderer/` | Per-frame draws/clears merged on render thread |
| `VulkanInstance` / `VulkanDevice` | `Source/FUSE/Renderer/include/fuse/renderer/vk/` | Headless bootstrap; optional validation layers |
| `VulkanSurface` | same | Headless vs external `VkSurfaceKHR` abstraction |
| `VulkanSwapchain` | same | Real `VkSwapchainKHR` when External surface + WSI; headless stub otherwise |
| `FrameManager` | same | Triple-buffered fence/semaphore ring aligned with `FrameBarrier` |
| `ResourceManager` / `GpuAllocator` | `Source/FUSE/Renderer/` | Handle-based buffers/images; VMA when vendored, stub otherwise |
| `BindlessDescriptors` | `Source/FUSE/Renderer/include/fuse/renderer/vk/` | Index table scaffolding (descriptor pool deferred to B2.4 follow-up) |
| `HandleMap<T>` | `Source/FUSE/Core/include/fuse/` | Generation-checked slots for GPU resources |
| `ShaderCompiler` / `ShaderModule` | `Source/FUSE/Renderer/include/fuse/renderer/shader/` | Offline-first — loads checked-in `.spv` fixtures |
| `PipelineLayout` | `Source/FUSE/Renderer/include/fuse/renderer/vk/pipeline_layout.hpp` | Placeholder layout (push constants only; bindless sets deferred) |
| `RhiContext` | `Source/FUSE/Renderer/` | `beginFrame` / `submitFrame` on `renderThread()` |
| `fuse::platform::gl_context.hpp` | `Source/FUSE/Core/` | Portable “may touch GPU” guard |
| `HybridComposer` wiring | `Source/FUSE/Hybrid/` | Dual path: software `PlaceholderRenderer` **and** RHI command mirror |

**Not in scope:** Engine marriage, real present in CI (no window surface), MoltenVK/Android surface wiring, full bindless descriptor pool, graphics pipeline cache, hot-reload watchers.

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
- Call `RhiContext::beginFrame()` / `submitFrame()`

Workers produce snapshot SOA / staging data only. Job code never includes `<vulkan/vulkan.h>`.

`HybridComposer::render()`:

1. Assert render thread (`platform::isRenderThread()`)
2. Execute software placeholder (existing U4 tests)
3. When `FUSE_HAS_VULKAN_RHI`: reset `RenderCommandList`, mirror clears/sprites, `beginFrame(ctx.frameIndex)`, `submitFrame()`

**Frame barrier alignment:** `FrameBarrier` at end of tick ensures jobs for frame `N` complete before render record. `FrameManager::signalTickComplete()` + `beginFrame(frameIndex)` mirror that sync point on the GPU path.

---

## Backend modes

```cpp
enum class VulkanBackendMode : u8 { Stub, Headless };
```

| Mode | When | Instance | Device | Swapchain |
|------|------|----------|--------|-----------|
| **Stub** | No loader / no ICD | — | — | — |
| **Headless** | Loader + ICD (incl. Lavapipe), no surface | `VkInstance` | `VkDevice` + queues | stub (records desc, no `VkSwapchainKHR`) |
| **Presentable** | External `VkSurfaceKHR` + `VK_KHR_swapchain` | `VkInstance` + WSI ext* | `VkDevice` | real `VkSwapchainKHR` |

\*WSI instance extensions (`VK_KHR_surface`, platform surface) must be enabled by the platform module that creates the surface — not yet wired in CI.

`GpuAllocator` creates a real `VmaAllocator` when `third_party/VulkanMemoryAllocator/include/vk_mem_alloc.h` is present; otherwise stub bookkeeping handles are issued and `VulkanDeviceInfo::vmaAllocator` stays null until VMA is vendored.

---

## Surface abstraction (B2.2)

```cpp
enum class SurfaceKind : u8 { Headless, External };
```

| Kind | `nativeSurface` | Swapchain behaviour |
|------|-----------------|---------------------|
| **Headless** | `nullptr` | Records width/height/imageCount; **no** `VkSwapchainKHR` (CI / Lavapipe default) |
| **External** | opaque `VkSurfaceKHR*` | Creates real swapchain when device has `VK_KHR_swapchain` and queue supports present |

Headless is intentional for CI: Lavapipe provides an ICD but umbrella tests run without a window. Editor Qt viewport (`U6`) will pass `SurfaceKind::External`.

---

## Frame ring (B2.2 sketch)

`kFramesInFlight = 3` — triple-buffered slot ring:

| Per-slot sync | Role |
|---------------|------|
| `image_available` | swapchain acquire signal (used when presentable) |
| `render_finished` | present wait |
| `in_flight_fence` | CPU wait before reusing slot |

Lifecycle per frame:

1. Tick completes → `FrameBarrier::signalTickJobsComplete()` (game thread)
2. Render thread → `FrameManager::signalTickComplete()` + `beginFrame(frameIndex)` (waits prior fence)
3. Record `RenderCommandList` + placeholder software path
4. `endFrame()` advances ring index

Command pools, descriptor pools, and scratch allocators deferred to B2.3+.

---

## B2.3 — Resources & bindless scaffolding

| Piece | Path | Behaviour |
|-------|------|-----------|
| Typed handles | `resources.hpp` | `TextureHandle`, `BufferHandle`, `SamplerHandle` |
| `GpuAllocator` | `vk/allocator.hpp` | VMA path when header vendored; stub IDs otherwise |
| `ResourceManager` | `resource_manager.hpp` | Create/destroy + bindless index assignment; 64 MiB staging ring |
| `BindlessDescriptors` | `vk/bindless.hpp` | Free-list indices only until descriptor pool lands |

Optional VMA: place [VulkanMemoryAllocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator) at `third_party/VulkanMemoryAllocator/include/vk_mem_alloc.h` and reconfigure.

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

---

## Desktop vs mobile (design notes)

| Platform | B2.2 stance | Later |
|----------|-------------|-------|
| Linux/Windows desktop | Headless bootstrap + Lavapipe/discrete ICD; real swapchain via External surface | Editor Qt viewport |
| macOS | Same API surface; MoltenVK behind platform module | Dedicated `FUSE_PLATFORM_MACOS` RHI backend |
| iOS / Android | Stub compile (`FUSE_BUILD_VULKAN=OFF` in mobile CI) | GLES/Metal/Vulkan queue ownership per §4.4 |

Portable invariant unchanged: job code emits `RenderCommandList`; platform module selects Vulkan/Metal/GLES backend and supplies `SurfaceDesc`.

---

## Tests

| Target | Validates |
|--------|-----------|
| `fuse_vulkan_bootstrap` | Instance/device or stub path; surface abstraction; render-thread submit |
| `fuse_vulkan_swapchain` | Headless swapchain desc; frame ring advance; external surface graceful failure |
| `fuse_vulkan_resources` | `HandleMap`, bindless index recycle, buffer/texture create/destroy |
| `fuse_shader_pipeline` | SPIR-V I/O, offline compiler, shader module + pipeline layout (stub or Vulkan) |
| `fuse_render_command_list` | Hybrid mirrors commands without breaking placeholder pixels |
| `fuse_hybrid_tests` | Existing U4 software renderer regressions |

Run:

```bash
ctest --test-dir build --output-on-failure -R 'fuse_vulkan|fuse_shader_pipeline|fuse_render_command|fuse_hybrid'
```

---

## CI story (honest)

1. **Linux umbrella** — `FUSE_BUILD_VULKAN=ON`, Mesa Lavapipe for headless ICD; Khronos validation layers used when installed, otherwise stub message (non-fatal). Swapchain stays **headless** (no `VkSurfaceKHR`); frame ring exercises real fences/semaphores.
2. **Android NDK** — `FUSE_BUILD_VULKAN=OFF`; `fuse_core` + `fuse_hybrid` unchanged.
3. **iOS stub workflow** — unchanged; Vulkan deferred.

No GPU window on runner is OK: stub backend keeps configure/build green; when Lavapipe is present, tests exercise real instance/device + frame sync objects.

---

## Next

- [x] `VkSwapchainKHR` path behind surface abstraction (External surface; headless stub documented)
- [x] Triple-buffered frame ring sketch aligned with `FrameBarrier`
- [x] B2.3 resource handles + stub/VMA allocator + bindless index table
- [x] B2.4 shader scaffold — offline SPIR-V, shader module, pipeline layout placeholder
- [ ] B2.4 follow-up: bindless descriptor pool + graphics pipeline cache
- [ ] Replace `PlaceholderRenderer` present path incrementally — keep software fallback for headless CI
- [ ] Editor Qt native surface (`U6` viewport) → `SwapchainDesc.surface`
- [ ] Android Vulkan WSI + MoltenVK macOS module

---

## Related docs

- [work-plan.md](./work-plan.md) — Track B kickoff entry
- [BUILD.md](./BUILD.md) — umbrella CMake options
- [risk-register.md](./risk-register.md) R21 — render thread invariant
