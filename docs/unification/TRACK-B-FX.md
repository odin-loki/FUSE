# Track B — FX / AFX Module (U5 `fuse_fx`)

**Status:** U5 follow-up — effect graph tick stubs, parameter bind, cast/timeline vertical slice  
**Work package:** [U5-MODULES.md](./U5-MODULES.md) §3  
**Threading:** [architecture-parallel.md](./architecture-parallel.md) §7

---

## Scope

| Component | Location | Notes |
|-----------|----------|-------|
| `EffectDescriptor` / `SpellDescriptor` | `Modules/fx/include/fuse/fx/effect_descriptor.hpp` | `afxEffectronData` / `afxMagicSpellData` ore |
| `EffectTimeline` | `Modules/fx/include/fuse/fx/effect_timeline.hpp` | Socket-bound phrase playback |
| `EffectGraph` | `Modules/fx/include/fuse/fx/effect_graph.hpp` | Parent/child group tick stub (`afxEffectGroup`) |
| `CastPipeline` | `Modules/fx/include/fuse/fx/cast_pipeline.hpp` | Spell phase state machine |
| `ResidualEffectQueue` | `Modules/fx/include/fuse/fx/residual_effects.hpp` | World decoration lifetime |
| `FxComposer` | `Modules/fx/include/fuse/fx/fx_composer.hpp` | Registry + tick facade |
| `bind::ParameterBinder` | `Modules/fx/include/fuse/fx/parameter_bind.hpp` | Runtime cast slot substitution |

**Ore sources (read-only):** `Engine/source/afx/`, `third_party/addons/AFX-Template/game/` — see [Modules/fx/README.md](../../Source/FUSE/Modules/fx/README.md).

**Not in scope (deferred):** GPU particle pools (`afxParticlePool`), constraint remapping (`afxConstraint`), Torque choreographer bridge, hybrid demo FX drive.

---

## Effect graph tick

`EffectGraph` models hierarchical effect groups:

1. `addNode(effectId, parentId)` — register a node; link children via parent id (`kInvalidNode` for roots).
2. `activate()` — mark all root nodes `Active`; children stay `Pending`.
3. `tick(dt, registry)` — advance active nodes against `EffectDescriptor::duration`; on completion, activate pending children.

`FxComposer::tick()` forwards the effect registry to `EffectGraph::tick()` after `EffectTimeline`.

---

## Parameter bind

`fuse::fx::bind::ParameterBinder` holds named runtime slots for cast substitution:

| Slot | Kind | Resolves to |
|------|------|-------------|
| `caster` | `Handle<Object>` | `CastBinding::caster` |
| `target` | `Handle<Object>` | `CastBinding::target` |
| arbitrary | `Float` / `Int` / `Bool` | Future effect/spell field drive |

`FxComposer::parameters()` exposes the composer-owned binder. `resolveCastBinding()` maps well-known handle slots into `CastBinding` for `beginCast`.

---

## Verification

```bash
cmake -B build-fuse -G Ninja -DFUSE_UMBRELLA=ON -DFUSE_BUILD_MODULES=ON -DFUSE_BUILD_CORE_TESTS=ON
cmake --build build-fuse --target fuse_fx_tests
./build-fuse/Source/FUSE/Modules/fx/tests/fuse_fx_tests
```

Tests cover descriptor registry, socket attach rejection, effect timeline completion, effect graph parent/child and parallel-root tick, parameter bind cast resolution, fireball phase progression, and cast→residual path.
