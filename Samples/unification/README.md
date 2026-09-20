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
- Live addon submodule assets — golden paths tried first; bundled `.mis`/`.cs` stubs used when submodules absent

**Wave 4 wiring**

- `__fuse.wire|animated_sprite|*` stubs from T2D module convert + runtime bridge metadata counts
- `classifySubmoduleDirectory` — local-only golden submodule probe (no network init)
- `fuse_u8_parity_embed_pie_smoke` — editor embed + PIE start/stop across **seven** parity demos (ASan when `FUSE_SMOKE_ENABLE_ASAN`)
- All six `fuse_u8_*` parity demo binaries + `demo_hybrid_hud` ASan-linked when `FUSE_SMOKE_ENABLE_ASAN=ON`

**Wave 3 wiring**

- `fuse_u8_parity_embed_pie_smoke` — editor embed + PIE start/stop across six parity demos (ASan when `FUSE_SMOKE_ENABLE_ASAN`)
- `demo_fx` bridges `defaultWorld2D` sprite sockets alongside 3D AFX mission VM
- `resolveParityLegacySource` prefers `third_party/` golden paths when submodules are initialized

`demo_hybrid_hud` uses a software placeholder renderer. Real present is Track B.
