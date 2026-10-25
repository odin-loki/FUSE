# FUSE samples

Headless demos that prove one runtime: 2D, 3D, hybrid, and feature modules.

Product guides: [`docs/player-guide.md`](../../docs/player-guide.md) (run demos), [`docs/programmer-guide.md`](../../docs/programmer-guide.md) (API wiring). Also [`docs/samples.md`](../../docs/samples.md), [`docs/projects.md`](../../docs/projects.md).

Enable binaries with `FUSE_BUILD_PARITY_DEMOS=ON` (default).

| Directory | Binary | Proves | Wiring depth |
|-----------|--------|--------|--------------|
| `demo_3d_empty/` | `demo_3d_empty` | 3D dimension path | ✅ `project.json` + VFS mount + `.mis`→`.fuselevel` convert/load |
| `demo_2d_sprites/` | `demo_2d_sprites` | 2D dimension path | ✅ SpriteToy-inspired `.cs` module bridge + Box2D tick |
| `demo_hybrid_hud/` | `demo_hybrid_hud` | Hybrid compositor | ✅ Full U5 module gates (ai/cinematics/fx/mechanics/adventure) |
| `demo_ai_bt/` | `demo_ai_bt` | `fuse_ai` | ✅ Hybrid 2D/3D agents + UAISK patrol profile + mission convert |
| `demo_timeline/` | `demo_timeline` | `fuse_cinematics` | ✅ Outpost intro asset + VActor + hybrid timeline drive |
| `demo_fx/` | `demo_fx` | `fuse_fx` | ✅ AFX template pack + mission VM + GPU particle pool |
| `demo_adventure_stub/` | `demo_adventure_stub` | `fuse_adventure` | ✅ Outpost JSON spawn + mechanics registry + weapon grant |

```bash
ctest --test-dir build -R fuse_u8_ --output-on-failure
./build/Source/FUSE/Apps/Demo3DEmpty/demo_3d_empty Samples/unification/demo_3d_empty
./build/Source/FUSE/Apps/HybridHud/demo_hybrid_hud
```

**Still stub / deferred**

- Bindless `imageMap` **PNG** sampling (animated frames use hybrid sprite-layer atlas strips + tint today)
- Bit-perfect legacy mission replay — converters produce wiring stubs, not full gameplay
- Live upstream addon trees — golden paths point at `Samples/unification/*/worlds/`; bundled `.mis`/`.cs` stubs used when a golden file is missing

**Present (U8)**

- All seven `demo_*` binaries: `--present` / `FUSE_DEMO_PRESENT=1` → GameWindow + `vkQueuePresentKHR` (Linux/Xvfb CI: `ctest -R 'fuse_u8_.*_present'`)

**Wave 5 wiring**

- `ensureDefault2DWorldReady` — symmetric 2D `.cs`→`.fuselevel` prepare (used by `bridge2DWorldFromProject`)
- Parity fallback tests for `demo_fx`, `demo_ai_bt`, `demo_timeline` golden paths (`ctest -R fuse_parity_legacy_sources`)

**Wave 4 wiring**

- T2D module bridge → `SceneObject2D` animation metadata; GPU hybrid path tints per atlas frame
- `classifySubmoduleDirectory` — local-only reference-tree probe (no network fetch)
- `fuse_u8_parity_embed_pie_smoke` — editor embed + PIE across **seven** projects
- `fuse-asan` CI: `ctest -L u8` (six `fuse_u8_*` sample demos + `demo_hybrid_hud` gates + PIE smoke)

**Wave 3 wiring**
- `demo_fx` bridges `defaultWorld2D` sprite sockets alongside 3D AFX mission VM
- `resolveParityLegacySource` prefers bundled `Samples/unification/*/worlds/` stubs (no upstream addon trees in-repo)

`demo_hybrid_hud` uses the hybrid GPU path when Vulkan is available; `--present` exercises the same GameWindow path as other demos.
