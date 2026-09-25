# FUSE Renderer — Implementation Plan

Author: Odin Loch
Target API: Vulkan 1.3 minimum, 1.4 preferred
Scope: Core software rendering technology for the FUSE game engine: geometry, visibility, lighting, shadows, global illumination, ray tracing, upscaling and post-processing.

Execution tracking: [../unification/RENDERER-EXECUTION.md](../unification/RENDERER-EXECUTION.md)

## 1. Goals

- A GPU-driven renderer in which the CPU submits a near-constant number of commands regardless of scene complexity.
- Bindless resource access throughout, with no per-draw descriptor churn.
- Scalable lighting, from thousands of dynamic lights to full real-time global illumination.
- Virtualized geometry that supports film-density meshes with automatic LOD.
- Vendor-neutral upscaling: DLSS, FSR and XeSS behind one interface.
- A tiered feature set, so the engine runs on non-RT hardware and scales up where RT cores and mesh shaders exist.

### Non-goals (initial release)

- Mobile or tile-based GPU optimisation.
- A DX12 or Metal backend.
- Offline or production path tracing. Real-time path tracing is a late, optional phase.

## 2. Hardware Tiers

| Tier | Minimum hardware | Features enabled |
|---|---|---|
| T0 – Baseline | Vulkan 1.3, descriptor indexing, buffer device address | Visibility buffer, clustered lighting, virtual shadow maps, DDGI (compute-traced against SDFs), FSR/XeSS |
| T1 – Mesh | T0 + VK_EXT_mesh_shader | Meshlet pipeline, virtual geometry |
| T2 – RT | T1 + VK_KHR_ray_query / VK_KHR_acceleration_structure | RT shadows and reflections, hardware-traced DDGI, ReSTIR DI |
| T3 – Full | T2 + ray tracing pipeline, tensor/matrix units | ReSTIR GI or path tracing, DLSS Ray Reconstruction, neural techniques |

Each feature must declare the tier it needs and its fallback. No feature may hard-require a higher tier than T0 unless it is explicitly optional.

## 3. Vulkan Extensions

### Required (T0)

| Extension / feature | Purpose |
|---|---|
| Dynamic rendering (core 1.3) | Removes VkRenderPass / VkFramebuffer boilerplate |
| Synchronization2 (core 1.3) | Clearer barriers; required by the render graph |
| Timeline semaphores (core 1.2) | Async compute and transfer coordination |
| Descriptor indexing (core 1.2) | Bindless textures and buffers |
| Buffer device address (core 1.2) | Pointer-style GPU scene data |
| Draw indirect count (core 1.2) | GPU-driven submission |
| Shader draw parameters, 64-bit atomics | Visibility buffer writes, instance IDs |

### Optional (T1–T3)

| Extension | Tier | Purpose |
|---|---|---|
| VK_EXT_mesh_shader | T1 | Meshlet culling and rendering |
| VK_EXT_descriptor_buffer | T0+ | Lower-overhead bindless |
| VK_EXT_device_generated_commands | T0+ | GPU-generated pipeline and state changes |
| VK_KHR_acceleration_structure | T2 | BLAS/TLAS |
| VK_KHR_ray_query | T2 | Inline tracing from compute and fragment shaders |
| VK_KHR_ray_tracing_pipeline | T3 | Shader binding table workloads |
| VK_KHR_cooperative_matrix, VK_NV_cooperative_vector | T3 | Neural texture compression, neural caches |
| VK_EXT_shader_object | Optional | Reduces pipeline-permutation cost |

VK_AMDX_shader_enqueue (work graphs) is experimental. Monitor it; do not build on it.

## 4. Third-Party Dependencies

| Library | Role | Phase |
|---|---|---|
| VMA (Vulkan Memory Allocator) | Allocation, defragmentation, budget tracking | 0 |
| Slang | Shader language compiling to SPIR-V; modules and generics | 0 |
| volk | Vulkan function loader | 0 |
| meshoptimizer | Meshlet build, cluster simplification, vertex compression | 1, 5 |
| NVIDIA Streamline | DLSS Super Resolution, Frame Generation, Ray Reconstruction, Reflex | 4 |
| AMD FidelityFX SDK | FSR upscaling, frame interpolation, other FX passes | 4 |
| Intel XeSS SDK | XeSS upscaling | 4 |
| NVIDIA NRD (via NRI) | Denoisers for RT shadows, reflections and GI | 6 |
| Tracy | CPU/GPU profiling | 0 |
| RenderDoc / Nsight Graphics | Frame capture and debugging | 0 |

Licences must be checked per library before integration. Keep third-party code behind FUSE-owned interfaces.

## 5. Architecture

### 5.1 Frame Structure

```
CPU: scene update → upload deltas to GPU scene buffers → build render graph → submit
GPU: instance cull → meshlet cull → visibility buffer → material resolve
     → lighting (clustered + shadows + GI) → transparency → post-process
     → upscale → UI → present
```

### 5.2 Core Subsystems

- **Render graph.** Declares passes and resources, derives barriers, aliases transient memory and schedules async compute. Every later phase plugs into it.
- **GPU scene.** Persistent buffers holding instances, transforms, materials, meshlets and lights, addressed by buffer device address. The CPU uploads only deltas.
- **Bindless registry.** A single global descriptor heap. Resources are referenced by 32-bit handles in shader data.
- **Pipeline cache.** Precompiled Slang permutations with an on-disk VkPipelineCache and background compilation.
- **Upscaler abstraction.** One interface (IUpscaler) with DLSS, FSR and XeSS backends selected at runtime.

### 5.3 Frame Budget Targets (1440p, 60 fps, mid-high T2 GPU)

| Stage | Budget (ms) |
|---|---|
| Culling and visibility buffer | 2.0 |
| Material resolve | 1.5 |
| Direct lighting and shadows | 3.0 |
| Global illumination | 3.0 |
| Volumetrics and transparency | 1.5 |
| Post-process and upscale | 2.0 |
| UI, headroom | 3.6 |
| **Total** | **16.6** |

These are initial targets and will be revised once Phase 2 profiling data exists.

## 6. Phased Implementation

Each phase has deliverables and exit criteria. A phase is not closed until its exit criteria pass on the reference scenes (Section 8).

### Phase 0 — Foundation

Deliverables
- Vulkan 1.3 device creation with feature and tier detection.
- VMA integration, volk loader, validation layers and debug labels.
- Render graph: pass declaration, automatic barriers (synchronization2), transient resource aliasing, async compute queue.
- Bindless descriptor registry.
- Slang toolchain, shader hot reload, pipeline cache.
- Tracy GPU zones and RenderDoc integration.

Exit criteria
- Zero validation errors on the test scenes.
- A pass can be added without writing any manual barrier.

### Phase 1 — GPU-Driven Visibility Buffer

Deliverables
- GPU scene buffers with delta uploads.
- Compute instance culling (frustum plus two-phase Hi-Z occlusion).
- Visibility buffer: a 64-bit target storing depth plus instance and triangle ID, written with atomics or a raster pass.
- Material resolve pass reconstructing attributes from the visibility buffer and shading per material tile (material classification plus indirect dispatch).
- vkCmdDrawIndexedIndirectCount submission.
- Meshlet data built offline with meshoptimizer, even before mesh shaders are enabled.

Exit criteria
- CPU submission cost is roughly constant from 1k to 100k instances.
- Two-phase occlusion culling shows no popping during camera cuts.

### Phase 2 — Clustered Lighting

Deliverables
- Froxel/cluster grid (for example 16×9×24, tunable), with a compute light-assignment pass.
- Point, spot, area (LTC) and directional lights.
- A PBR BRDF with multi-scatter energy compensation.
- Clustered data shared with the forward transparency pass.

Exit criteria
- 4,096 dynamic lights within the lighting budget on the reference GPU.

### Phase 3 — Virtual Shadow Maps

Deliverables
- A 16k×16k virtual shadow map per directional light, with clipmap levels.
- Page marking from visible pixels, a page table, and a physical page pool.
- Caching of static pages, with invalidation when objects move.
- Filtering (PCF, with optional contact-hardening PCSS).
- Point and spot lights using cube or single virtual pages.

Exit criteria
- Sharp shadows from near to far without cascade seams.
- Cached-frame shadow cost stays under 1 ms.

### Phase 4 — Upscaling and Temporal Pipeline

Deliverables
- Motion vectors (camera and per-object), jitter sequence, reactive and transparency masks.
- IUpscaler backends:
  - DLSS via Streamline.
  - FSR via the FidelityFX SDK.
  - XeSS.
  - Native TAA as the fallback.
- Frame generation via Streamline and FSR where supported. Reflex/latency integration.
- Post-process stack: exposure, bloom, tonemapping (AgX or ACES), colour grading LUT, depth of field, motion blur.

The post-process stack gives FUSE native control over what ENB provides in modded games. ENB itself is a DirectX injector and does not apply to a Vulkan engine.

Exit criteria
- Each upscaler is switchable at runtime without artefacts from missing inputs.

### Phase 5 — Mesh Shaders and Virtual Geometry (T1)

Deliverables
- Task/mesh shader pipeline performing per-meshlet frustum, backface-cone and Hi-Z culling.
- Cluster hierarchy (DAG) built with meshoptimizer's simplification, with LOD selected by a screen-space error metric.
- Streaming of cluster pages from disk, with a residency manager.
- A software rasteriser (compute) for micro-triangle clusters.
- Fallback to the Phase 1 indirect path on T0 hardware.

Exit criteria
- A reference scene of more than 100M source triangles renders within budget.
- No visible LOD cracks.

### Phase 6 — Global Illumination

Build in order; each step works standalone.

**6a — DDGI (T0/T2)**
- Irradiance probe volumes with a visibility (depth moments) term.
- Probe rays traced by ray query on T2, or against a global SDF / voxel scene on T0.
- Probe relocation and classification to handle probes inside geometry.

**6b — RT effects (T2)**
- RT shadows (soft, from area lights) and RT reflections using ray query.
- Denoising with NRD (SIGMA for shadows, REBLUR or RELAX for reflections).

**6c — Screen-space fallback**
- SSR and SSAO/GTAO for T0, and as a supplement on higher tiers.

**6d — Radiance cascades (optional research track)**
- Evaluate as a noise-free alternative or supplement to DDGI for T0.

Exit criteria
- Dynamic time of day with stable GI: no light leaks and no visible probe grid.

### Phase 7 — ReSTIR and Path Tracing (T2–T3)

Deliverables
- ReSTIR DI for many-light direct illumination: spatiotemporal reservoir reuse.
- ReSTIR GI for indirect illumination.
- An optional real-time path tracing mode (T3) with DLSS Ray Reconstruction or NRD.
- A light BVH or light tree for importance sampling.

Exit criteria
- 10k+ emissive lights with stable, low-noise output after denoising.

### Phase 8 — Volumetrics and Atmosphere

Deliverables
- Froxel volumetric fog with temporal reprojection, lit by clustered lights, shadows and GI.
- Physically based sky and atmosphere (Hillaire-style LUTs).
- Volumetric clouds (raymarched, temporally amortised).

Exit criteria
- Volumetrics fit inside the 1.5 ms budget with no temporal ghosting.

### Phase 9 — Neural and Advanced Techniques (T3, optional)

Deliverables
- Neural texture compression using cooperative matrix/vector.
- Neural radiance cache evaluation, as a supplement to ReSTIR GI.
- 3D Gaussian splatting renderer for scanned assets, composited with the visibility buffer.
- Device-generated commands for full GPU-side pipeline selection.

Exit criteria
- Each technique is measured against the conventional path for memory, quality and cost. Techniques that do not win are not shipped.

## 7. Dependency Graph

```
P0 Foundation
 └─ P1 Visibility Buffer
     ├─ P2 Clustered Lighting ─┬─ P3 Virtual Shadow Maps
     │                          └─ P8 Volumetrics
     ├─ P4 Upscaling/Temporal (needed by P6b, P7 denoising)
     └─ P5 Mesh Shaders / Virtual Geometry
P2 + P3 + P4 ─ P6 Global Illumination ─ P7 ReSTIR / Path Tracing
P7 ─ P9 Neural / Advanced
```

P4 can run in parallel with P2 and P3. P5 can run in parallel with P6.

## 8. Validation and Testing

- Reference scenes:
  - Sponza (lighting).
  - Bistro (lights and transparency).
  - A high-density geometry scene (virtual geometry).
  - An open-world stress test (streaming, shadows, GI).
- Image regression: a golden-image comparison per phase using FLIP or SSIM, run in CI on every commit that touches the renderer.
- Performance regression: Tracy captures of per-pass GPU time, compared against the Phase 5.3 budgets. Fail on a regression above 5%.
- Validation layers: mandatory in debug builds, with synchronization validation enabled for render graph testing.
- Tier matrix: every phase is tested on at least one T0 and one T2 device.
- PRISM integration: the C/C++ core (render graph, GPU scene, residency manager) is candidate code for the PRISM verification pipeline, covering coverage, sanitisers and bounded model checking of allocator and page-table logic.

## 9. Risks

| Risk | Impact | Mitigation |
|---|---|---|
| Render graph design is wrong early | Every later phase is reworked | Prototype P1–P3 against it before freezing the API |
| Mesh shader performance varies by vendor | T1 regressions on some GPUs | Keep the P1 indirect path as a first-class fallback |
| Virtual geometry streaming stalls | Visible pop-in, hitches | Prioritised residency, conservative prefetch, a coarse-LOD guarantee |
| GI light leaks | Visual artefacts | Probe visibility terms, relocation, thin-wall authoring rules |
| Upscaler SDK licence or API churn | Integration breakage | Isolate behind IUpscaler; pin SDK versions |
| Shader permutation explosion | Long compile times, hitches | Slang generics, uber-material via the visibility buffer, background compile |
| Solo scope | Schedule overrun | Phases are independently shippable; P7–P9 are optional |

## 10. Milestones

| Milestone | Phases | Outcome |
|---|---|---|
| M1 — Renders | P0, P1 | GPU-driven scene on screen |
| M2 — Lit | P2, P3, P4 | Shipping-quality raster renderer with upscaling |
| M3 — Dense | P5 | Virtualized geometry |
| M4 — Global | P6, P8 | Dynamic GI, sky and volumetrics |
| M5 — Traced | P7, P9 | ReSTIR / path tracing and neural techniques |

M2 is the first point at which FUSE is usable for a game. Everything after M2 is an incremental upgrade.
