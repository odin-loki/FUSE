# FUSE samples

Headless demos that prove one runtime: 2D, 3D, hybrid, and feature modules.

Product guide: [`docs/samples.md`](../../docs/samples.md). Project format: [`docs/projects.md`](../../docs/projects.md).

Enable binaries with `FUSE_BUILD_PARITY_DEMOS=ON` (default).

| Directory | Binary | Proves |
|-----------|--------|--------|
| `demo_3d_empty/` | `demo_3d_empty` | 3D dimension path |
| `demo_2d_sprites/` | `demo_2d_sprites` | 2D dimension path |
| `demo_hybrid_hud/` | `demo_hybrid_hud` | Hybrid compositor |
| `demo_ai_bt/` | `demo_ai_bt` | `fuse_ai` |
| `demo_timeline/` | `demo_timeline` | `fuse_cinematics` |
| `demo_fx/` | `demo_fx` | `fuse_fx` |
| `demo_adventure_stub/` | `demo_adventure_stub` | `fuse_adventure` |

```bash
ctest --test-dir build --output-on-failure
./build/Source/FUSE/Apps/Demo3DEmpty/demo_3d_empty Samples/unification/demo_3d_empty
./build/Source/FUSE/Apps/HybridHud/demo_hybrid_hud
```

`demo_hybrid_hud` uses a software placeholder renderer. Real present is Track B.
