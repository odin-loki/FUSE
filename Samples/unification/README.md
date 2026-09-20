# FUSE samples

Headless demos that prove one runtime: 2D, 3D, hybrid, and feature modules.

Product guide: [`docs/samples.md`](../../docs/samples.md). Project format: [`docs/projects.md`](../../docs/projects.md).

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

- Real GLES/Vulkan present (Track B) — software placeholder renderer only
- Bit-perfect legacy mission replay — converters produce wiring stubs, not full gameplay
- `demo_fx` 2D sprite socket world (`defaultWorld2D`) — 3D AFX mission path only today
- Live addon submodule assets — bundled `.mis`/`.cs` stubs under each `worlds/` dir

`demo_hybrid_hud` uses a software placeholder renderer. Real present is Track B.
