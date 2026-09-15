# FUSE Unification Demos (U8 targets)

**Status:** `demo_hybrid_hud` scaffold lives in `Source/FUSE/Apps/HybridHud/` (U4/WP-06). U8 parity demos remain placeholders.  
**Spec:** [docs/unification/demo-corpus-parity-targets.md](../../docs/unification/demo-corpus-parity-targets.md)  
**U4 notes:** [docs/unification/U4-HYBRID-FRAME.md](../../docs/unification/U4-HYBRID-FRAME.md)

Minimum frozen demo set (do not shrink):

| Directory (planned) | Proves | Legacy golden source |
|---------------------|--------|----------------------|
| `demo_3d_empty/` | 3D dimension path | `Templates/BaseGame/game/data/ExampleModule/levels/ExampleLevel.mis` |
| `demo_2d_sprites/` | 2D dimension path | `third_party/Torque2D/toybox/SpriteToy/1/main.cs` |
| `demo_hybrid_hud/` | Hybrid compositor | ExampleLevel + 2D HUD — **U4 scaffold** (`Source/FUSE/Apps/HybridHud/`) |
| `demo_ai_bt/` | `fuse_ai` | `BadBehaviour/.../BehaviorTestbed.mis` |
| `demo_timeline/` | `fuse_cinematics` | Verve template |
| `demo_fx/` | `fuse_fx` | `AFX-Template/game/levels/AFXDemo_Minimal.mis` |
| `demo_adventure_stub/` | `fuse_adventure` | `3DAAK/.../Outpost.mis` |

Each subdirectory will contain a `README.md` + minimal FUSE project stub when U7/U8 converters exist.
