# Player guide

How to run FUSE today: headless demos, optional editor embed, and what to expect.

This is the **unified product** path — one build from the FUSE repo root. You do not need separate Torque2D or Torque3D installs.

## What you can run now

| What | Binary | What you see |
|------|--------|--------------|
| Hybrid compositor | `demo_hybrid_hud` | 60-frame software render (no window by default) |
| 3D empty world | `demo_3d_empty` | Mission convert + 3D clear colour in software buffer |
| 2D sprites | `demo_2d_sprites` | SpriteToy-inspired module bridge + optional Box2D tick |
| AI behavior trees | `demo_ai_bt` | Hybrid 2D/3D BT agents + UAISK patrol profile |
| Cinematics timeline | `demo_timeline` | Verve intro mission + timeline host stub |
| Spell / FX | `demo_fx` | AFX mission VM + 2D sprite sockets + particle pool |
| Adventure stub | `demo_adventure_stub` | Outpost inventory, interactions, weapon grant |

All seven parity demos live under `Samples/unification/`. Each folder is a `project.json` plus bundled `worlds/` sources.

**Honest limits today**

- No real GPU window present in parity demos — software placeholder renderer only (Track B).
- Converters produce wiring stubs, not full legacy gameplay replay.
- Golden addon/T2D assets are used when submodules are initialized; otherwise bundled `.mis` / `.cs` stubs are used automatically.

## Build (first time)

```bash
git clone --recurse-submodules https://github.com/odin-loki/FUSE.git
cd FUSE

cmake -B build -G Ninja \
  -DFUSE_UMBRELLA=ON \
  -DFUSE_BUILD_CORE=ON \
  -DFUSE_BUILD_CORE_TESTS=ON \
  -DFUSE_BUILD_T3D=OFF \
  -DFUSE_BUILD_T2D=OFF

cmake --build build
```

Full option matrix: [building.md](building.md). Faster clone without submodules still works — demos fall back to bundled stubs.

## Run demos

From the repo root (working directory matters for relative paths):

```bash
# Hybrid — exercises all five fuse_* modules
./build/Source/FUSE/Apps/HybridHud/demo_hybrid_hud

# Per-demo (pass project path as argv[1])
./build/Source/FUSE/Apps/Demo3DEmpty/demo_3d_empty Samples/unification/demo_3d_empty
./build/Source/FUSE/Apps/Demo2DSprites/demo_2d_sprites Samples/unification/demo_2d_sprites
./build/Source/FUSE/Apps/DemoAiBt/demo_ai_bt Samples/unification/demo_ai_bt
./build/Source/FUSE/Apps/DemoTimeline/demo_timeline Samples/unification/demo_timeline
./build/Source/FUSE/Apps/DemoFx/demo_fx Samples/unification/demo_fx
./build/Source/FUSE/Apps/DemoAdventureStub/demo_adventure_stub Samples/unification/demo_adventure_stub
```

On Windows, binaries are under `build\Source\FUSE\Apps\<Demo>\Release\` (or `Debug\`).

Success prints a line like `demo_3d_empty: PASS` and exits 0. Failures print `FAIL:` and exit non-zero.

## Automated smoke (recommended)

```bash
ctest --test-dir build -R fuse_u8_ --output-on-failure
```

This runs all seven parity demo binaries plus editor embed PIE smoke (`fuse_u8_parity_embed_pie_smoke` when the editor API is built).

## Editor (optional, desktop)

With Qt 6 installed:

```bash
cmake -B build -G Ninja \
  -DFUSE_BUILD_EDITOR=ON \
  -DFUSE_BUILD_T3D=OFF -DFUSE_BUILD_T2D=OFF
cmake --build build --target fuse_editor
```

See [editor.md](editor.md). Editor embed converts bundled/golden worlds before load; with a display the viewport presents through an embedded Vulkan window (editor-scoped Track B present unlock; software placeholder when headless).

## Enable 2D / 3D / modules in a project

Edit `project.json` in your game folder — no second repo:

```json
{
  "dimensions": { "enable3D": true, "enable2D": true },
  "modules": { "ai": true, "fx": false, "cinematics": false, "mechanics": true, "adventure": false },
  "defaultWorld3D": "worlds/main.fuselevel",
  "defaultWorld2D": "worlds/hud.fuselevel"
}
```

Schema details: [projects.md](projects.md).

## What is not ready yet

- Shipping `fuse_runtime` player with installer — demos are the player surface today.
- Animated sprite GPU texture atlas playback — metadata and wiring stubs only.
- Full golden submodule corpus without bundled fallbacks — requires `git submodule update --init`.
- Stakeholder sign-off on “one program” — engineering gates are tracked in [demo-corpus-parity-targets.md](unification/demo-corpus-parity-targets.md).

## Next reads

- [samples.md](samples.md) — demo table and ctest names
- [getting-started.md](getting-started.md) — clone and first build
- [programmer-guide.md](programmer-guide.md) — APIs and extension points
