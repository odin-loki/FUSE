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
| U3 | Shared services, jobs/fibers | In progress |
| U4 | Dimension APIs + hybrid frame | Scaffolding (`demo_hybrid_hud`) |
| U5 | Feature modules | Vertical slices landed |
| U6 | Qt editor shell | Minimal desktop shell |
| U7 | `project.json` + importers | Minimal loader + CLI dry-run |
| U8 | Parity demo corpus | Seven headless demos |

## What is real today vs later

**In tree and testable:** `fuse_core` (jobs, handles, allocators, math, VFS, platform), World2D/World3D, hybrid composer (software present), L3 modules, project loader, editor API, script host stubs, terrain/VFX/world-partition CPU scaffolding.

**Scaffolding / Track B:** Vulkan RHI, CUDA kernels, full PBR frame, in-viewport gizmos, native world files, Lua as the default shipped VM.

Engineering notes: [`unification/README.md`](unification/README.md), [`plans/FUSE_MASTER_PLAN.md`](plans/FUSE_MASTER_PLAN.md).
