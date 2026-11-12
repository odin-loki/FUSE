# Roadmap

FUSE ships as **one program**. Work is sequenced: harden the host, then grow features on that host. Do not invert the tracks.

## Track A — Port and harden

Make the product language, ownership, editor chrome, and CI gates real.

| Goal | Bar |
|------|-----|
| ISO C++23 | Host libraries on `CMAKE_CXX_STANDARD 23`; CUDA device dialect may stay C++20 |
| Memory safety | Handles + allocators in public APIs; ASan/UBSan clean on smoke and unit suites |
| Qt GUI | Editor chrome is Qt 6; no Qt includes in engine core |
| Behavioural parity | Golden scenes load within agreed tolerances before features land on that subsystem |

Principle: wrap, then replace. Compat loaders keep old content runnable until each subsystem’s gate is green.

## Track B — Features

Each item is a modification of the ported engine, not a side project.

| Phase | Focus |
|-------|--------|
| B1 | Foundation — types, allocators, math, jobs, log, platform |
| B2 | Vulkan bootstrap, swapchain, bindless resources, shaders, render graph, CUDA interop |
| B3 | ECS, spatial structures (BVH, SVO), scene, camera |
| B4 | Physics — broad/narrow phase, PBD, destruction, soft body |
| B5 | Deferred PBR, clustered lighting, shadows, GI, post, atmosphere |
| B6 | Qt editor panels, gizmos, play mode |
| B7 | Animation, audio, script, net, terrain, world partition, VFX, platforms, cookers |

Heading list: [`plans/FUSE_MASTER_PLAN_TOC.md`](plans/FUSE_MASTER_PLAN_TOC.md).

## Unification gates (in tree)

These prove “one program” while Track A/B land:

| Gate | Meaning | Status |
|------|---------|--------|
| U1 | Umbrella CMake | Landed |
| U2 | One-process smoke + quarantine libs | Landed |
| U3 | Shared services, jobs/fibers | Landed (cooperative fiber waits on POSIX ucontext and Win32 fibers; Android/iOS use the condition-variable fallback, Emscripten the single-thread stub — [wp03-fiber-remaining](unification/wp03-fiber-remaining.md)) |
| U4 | Dimension APIs + hybrid frame | World2D/3D `findSpawnPoint` + `enterPlayMode`; SSRC/DDGI/fog + optional flatland RC (`rcFlatland`) in `FrameComposer`; `DdgiGpu::importCpuVolumeAtlases` for CPU-volume staging |
| U5 | Feature modules | Vertical slices landed |
| U6 | Qt editor shell | Landed editor API + project VFS remount on project open (`mountOrRemap` for `/game/`, `/t3d/`, `/t2d/`) + PIE `SpawnMarker` snap + headless in-viewport gizmo raster (translate/rotate/scale). Live Qt present remains optional (`fuse_editor` when Qt 6 is found). |
| U7 | `project.json` + importers | Landed through wave 18: loader/writer, CLI dry-run (VFS mount/remap + cook-cache report), T3D/T2D importers, async material/shader cook cache persisted to `.fuse/cook_cache.json`. `remapLegacyAssetPath` normalizes `\` and `./` and matches prefixes case-insensitively. Deferred: real ispc_texcomp / vorbisenc CI packages. |
| U8 | Parity demo corpus | Seven headless demos |

## What is real today vs later

**In tree and testable:** `fuse_core` (jobs, handles, allocators, math, VFS, platform), World2D/World3D, hybrid composer (software present), L3 modules, project loader, editor API, script host stubs, terrain/VFX/world-partition CPU scaffolding.

**Vulkan on Lavapipe (gated in CI):** Vulkan RHI with render graph v2, GPU scene / visibility buffer, clustered lighting, VSM, DDGI, SSFX, post, fog, atmosphere, and the `FrameComposer` frame (SSRC and flatland RC behind settings, off by default) — status per deliverable in [`unification/RENDERER-EXECUTION.md`](unification/RENDERER-EXECUTION.md).

**Still scaffolding / later:** CUDA kernels on real hardware, vendor plugins (DLSS, XeSS, NRD runtimes), native world files, Lua as the default shipped VM.

Engineering notes: [`unification/README.md`](unification/README.md), [`plans/FUSE_MASTER_PLAN.md`](plans/FUSE_MASTER_PLAN.md).
