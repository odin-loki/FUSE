# Samples

Headless demos live under `Samples/unification/`. Each folder is a FUSE project (`project.json`) plus a matching binary when `FUSE_BUILD_PARITY_DEMOS=ON`.

| Directory | Binary | Proves |
|-----------|--------|--------|
| `demo_3d_empty/` | `demo_3d_empty` | 3D dimension path |
| `demo_2d_sprites/` | `demo_2d_sprites` | 2D dimension path |
| `demo_hybrid_hud/` | `demo_hybrid_hud` | Hybrid compositor (3D + 2D HUD) |
| `demo_ai_bt/` | `demo_ai_bt` | `fuse_ai` behavior trees |
| `demo_timeline/` | `demo_timeline` | `fuse_cinematics` |
| `demo_fx/` | `demo_fx` | `fuse_fx` |
| `demo_adventure_stub/` | `demo_adventure_stub` | `fuse_adventure` |

`demo_hybrid_hud` uses a **software placeholder renderer**. Real GL/Vulkan present is Track B.

## Run

From the repo root after an umbrella build:

```bash
ctest --test-dir build --output-on-failure

./build/Source/FUSE/Apps/Demo3DEmpty/demo_3d_empty Samples/unification/demo_3d_empty
./build/Source/FUSE/Apps/HybridHud/demo_hybrid_hud
```

Windows: add the config directory (`Release` / `Debug`) to the binary path.

AI templates (script-only, not compiled): `Samples/Modules/ai/uaisk-templates/`.
