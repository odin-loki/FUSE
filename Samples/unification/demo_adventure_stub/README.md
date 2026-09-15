# demo_adventure_stub

**Status:** Scaffold — `fuse_adventure` APIs live in `Source/FUSE/Modules/adventure/`.  
**Proves:** Inventory add/remove, use/pickup interaction, and puzzle-gate scaffolding.  
**Legacy golden source:** `third_party/addons/3DAAK/Templates/Full/game/levels/Outpost.mis`

## 3DAAK ore mapping

| 3DAAK script API | `fuse::adventure` API |
|------------------|----------------------|
| `ShapeBase::incInventory` / `decInventory` | `Inventory::incInventory` / `decInventory` |
| `ShapeBase::hasInventory` / `getInventory` | `Inventory::hasInventory` / `getInventory` |
| `ShapeBase::use` / `serverCmdUse` | `InteractionSystem::use` |
| `ShapeBase::pickup` / `onPickup` | `InteractionSystem::pickup` / `IInteractable::onPickup` |
| Key / door gating (template gameplay) | `PuzzleGate` |

## Build

The module is built with the FUSE umbrella (`cmake -B build -DFUSE_UMBRELLA=ON`).
Run tests: `ctest -R fuse_adventure`.

Full demo parity (Outpost mission import) is a U8 target — see [demo-corpus-parity-targets.md](../../docs/unification/demo-corpus-parity-targets.md).
