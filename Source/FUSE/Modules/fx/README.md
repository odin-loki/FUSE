# fuse_fx — AFX ore extraction (U5)

Thin game-thread FX runtime shaped by Arcane FX concepts already in FUSE root `Engine/source/afx/`.

## Ore map

| FUSE type | AFX ore source |
|-----------|----------------|
| `EffectDescriptor` | `afxEffectronData` (`afxEffectron.h`) |
| `SpellDescriptor` / `SpellPhase` | `afxMagicSpellData` / `afxMagicSpellDefs` (`afxMagicSpell.h`) |
| `CastPipeline` | `afxMagicSpell` cast state machine (`afxMagicSpell.h`) |
| `ResidualEffectQueue` | `afxResidueMgr` (`afxResidueMgr.h`) |
| `FxComposer` | `afxChoreographer` orchestration (`afxChoreographer.h`) |
| `EffectEntry` / `EffectTiming` | `afxEffectWrapperData` / `afxEffectTimingData` (`afxEffectWrapper.h`) |

Sample content reference only (no Engine merge): `third_party/addons/AFX-Template/game/levels/AFXDemo_Minimal.mis`.

## Threading

- Descriptor registration, cast begin, socket attach: **game thread**
- `tick()` advances cast phases and residual lifetimes; particle sim jobification deferred

## Tests

`fuse_fx_tests` (`ctest` name `fuse_fx_runtime`) registers descriptors and ticks cast/residual paths without GUI.
