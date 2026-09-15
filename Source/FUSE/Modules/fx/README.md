# fuse_fx — AFX ore extraction (U5)

Game-thread FX runtime shaped by Arcane FX concepts already in FUSE root `Engine/source/afx/`.

## Ore map

| FUSE type | AFX ore source |
|-----------|----------------|
| `EffectDescriptor` | `afxEffectronData` (`afxEffectron.h`) |
| `EffectTimeline` | `afxPhrase` / `afxEffectron` playback (`afxPhrase.h`, `afxEffectron.h`) |
| `SpellDescriptor` / `SpellPhase` | `afxMagicSpellData` / `afxMagicSpellDefs` (`afxMagicSpell.h`) |
| `CastPipeline` | `afxMagicSpell` cast state machine (`afxMagicSpell.h`) |
| `ResidualEffectQueue` | `afxResidueMgr` (`afxResidueMgr.h`) |
| `FxComposer` | `afxChoreographer` orchestration (`afxChoreographer.h`) |
| `FxSocket` | AFX constraint / effectron host attachment |
| `EffectEntry` / `EffectTiming` | `afxEffectWrapperData` / `afxEffectTimingData` (`afxEffectWrapper.h`) |

Sample content reference only (no Engine merge): `third_party/addons/AFX-Template/game/levels/AFXDemo_Minimal.mis`.

## Vertical slice (U5)

- Descriptor registry (`EffectDescriptor`, `SpellDescriptor`) with `makeSparkBurst`, `makeMuzzleFlash`, `makeFireball`
- Socket attach validates registered effects and starts `EffectTimeline` playback
- `CastPipeline` drives spell phases (casting → launch → delivery → impact → linger)
- Impact phase auto-enqueues residuals from spell impact entries (fireball → scorch zodiac)
- `FxComposer::tick()` advances effect timeline, casts, and residual lifetimes

## Threading

- Descriptor registration, cast begin, socket attach: **game thread**
- `tick()` advances effect phrases, cast phases, and residual lifetimes; particle sim jobification deferred

## Tests

`fuse_fx_tests` (`ctest` name `fuse_fx_runtime`) covers descriptor registry, socket attach rejection, effect timeline completion, fireball phase progression, and cast-to-residual path.
