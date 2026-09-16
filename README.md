# FUSE

**Fast Unified Simulation Engine**

One program for 2D, 3D, and hybrid worlds. ISO C++ host, handle-based ownership, a fiber job spine, Vulkan and optional CUDA, and a Qt 6 editor.

*Exact where it matters. Parallel everywhere else.*

- Product docs: [`docs/README.md`](docs/README.md)
- Getting started: [`docs/getting-started.md`](docs/getting-started.md)
- Architecture: [`docs/architecture.md`](docs/architecture.md)
- Build: [`docs/building.md`](docs/building.md)

Work happens on **`main`**.

---

## What it is

FUSE is a single runtime and a single editor:

| Target | Role |
|--------|------|
| `fuse_runtime` | Game / player process — 2D, 3D, or both in one frame |
| `fuse_editor` | Qt 6 desktop shell — project hub, viewports, inspectors |
| `fuse_tools` | CLI cookers and importers |

Gameplay, AI, FX, cinematics, mechanics, and adventure systems are **engine modules**, not bolt-on kits. 2D and 3D share object identity, assets, input, audio, and networking.

Current public APIs live under `fuse::` in [`Source/FUSE/`](Source/FUSE/).

---

## Status

FUSE is under active construction. The umbrella CMake graph, `fuse_core`, dimension worlds, hybrid compositor, feature modules, project format, and headless demos are in tree. The Vulkan renderer, CUDA compute path, and Qt editor are scaffolding toward Track B.

See [`docs/roadmap.md`](docs/roadmap.md) for the phase map.

---

## Quick start

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
ctest --test-dir build --output-on-failure
```

Run a hybrid demo (software placeholder renderer):

```bash
./build/Source/FUSE/Apps/HybridHud/demo_hybrid_hud
```

On Windows without Ninja, omit `-G Ninja` and pass `--config Release` to the build step. Full options: [`docs/building.md`](docs/building.md).

---

## Repository map

```
Source/FUSE/     Product engine — core, worlds, renderer, modules, editor, apps
Samples/         Demo projects (`project.json`)
Tools/FUSE/      Importers and cookers
docs/            User and engineering documentation
cmake/           Umbrella platform and target helpers
```

---

## License

MIT. See [`LICENSE.md`](LICENSE.md). Provenance notes: [`docs/heritage.md`](docs/heritage.md).
