# FUSE Unification Demos (U8 targets)

**Status:** All seven minimum demos have `project.json` stubs + runnable/stubbed binaries (`FUSE_BUILD_PARITY_DEMOS=ON`).  
**Spec:** [docs/unification/demo-corpus-parity-targets.md](../../docs/unification/demo-corpus-parity-targets.md)  
**U7 format:** [docs/unification/U7-PROJECT-FORMAT.md](../../docs/unification/U7-PROJECT-FORMAT.md)  
**U4 notes:** [docs/unification/U4-HYBRID-FRAME.md](../../docs/unification/U4-HYBRID-FRAME.md)

Minimum frozen demo set (do not shrink):

| Directory | Binary | Proves | Legacy golden source |
|-----------|--------|--------|----------------------|
| `demo_3d_empty/` | `demo_3d_empty` | 3D dimension path | `Templates/BaseGame/.../ExampleLevel.mis` |
| `demo_2d_sprites/` | `demo_2d_sprites` | 2D dimension path | `third_party/Torque2D/toybox/SpriteToy/1/main.cs` |
| `demo_hybrid_hud/` | `demo_hybrid_hud` | Hybrid compositor | ExampleLevel + 2D HUD (`Source/FUSE/Apps/HybridHud/`) |
| `demo_ai_bt/` | `demo_ai_bt` | `fuse_ai` | `BadBehaviour/.../BehaviorTestbed.mis` |
| `demo_timeline/` | `demo_timeline` | `fuse_cinematics` | Verve template |
| `demo_fx/` | `demo_fx` | `fuse_fx` | `AFX-Template/game/levels/AFXDemo_Minimal.mis` |
| `demo_adventure_stub/` | `demo_adventure_stub` | `fuse_adventure` | `3DAAK/.../Outpost.mis` |

Run from repo root after umbrella build:

```bash
ctest --test-dir build-fuse
./build-fuse/Source/FUSE/Apps/Demo3DEmpty/demo_3d_empty Samples/unification/demo_3d_empty
```
