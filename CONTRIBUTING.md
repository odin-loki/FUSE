# Contributing to FUSE

Thank you for helping. This document is the process for FUSE itself — product code under `Source/FUSE/`, `fuse::` APIs, docs, samples, and tools.

## Repository layout

| Path | Contribute here? |
|------|------------------|
| `Source/FUSE/` | **Yes** — product C++23 (`fuse::` APIs) |
| `Samples/unification/` | **Yes** — U8 parity `project.json` demos and bundled worlds |
| `Tools/FUSE/` | **Yes** — CLI importers and cookers |
| `docs/` | **Yes** — user and engineering documentation |
| `vendor/` | **Rarely** — pinned third-party; bump deliberately and preserve licence files |
| `Templates/` | Torque-era reference content only — do not add legacy engine CMake targets |

Details: [`docs/repository-layout.md`](docs/repository-layout.md). Parity demo index: [`Samples/unification/README.md`](Samples/unification/README.md).

## Dependencies and vendoring

FUSE does **not** use git submodules. After clone, third-party libraries are already under `vendor/` — no extra checkout step.

Torque addon **ore** (AFX, Verve, BadBehaviour, GMK, 3DAAK, and similar) is **not** vendored in this repo. Extract kernels into `Source/FUSE/Modules/` per [`docs/unification/addon-ore-catalog.md`](docs/unification/addon-ore-catalog.md); parity content belongs in `Samples/unification/`.

When you change `vendor/`:

- Keep upstream `LICENSE` / notice files intact.
- Prefer minimal, auditable diffs and state the upstream version in the PR.
- Discuss new copyleft dependencies with maintainers first (product licence: AGPL-3.0 in [`LICENSE.md`](LICENSE.md); provenance: [`docs/heritage.md`](docs/heritage.md)).

## Build

Configure with CMake 3.21+ (presets require 3.25+). Presets live in [`CMakePresets.json`](CMakePresets.json); see [`docs/building.md`](docs/building.md) and [`cmake/README.md`](cmake/README.md).

## Report an issue

Search [existing issues](https://github.com/odin-loki/FUSE/issues) first. When you open a new one, include:

- Platform, compiler, and CMake options you used
- What happens, and what should happen
- Steps to reproduce
- GPU and driver details if it is a graphics issue
- Sanitizer output (`ASan` / `UBSan` / `TSan`) when you have it

## Pull requests

Work happens on **`main`**. Open PRs against `main`.

### Rules

- Code must be legally compatible with the AGPL-3.0 license in [`LICENSE.md`](LICENSE.md).
- Follow [`docs/coding-standards.md`](docs/coding-standards.md).
- Keep each PR focused. One intent, a small set of files, a clear test plan.
- Do not leak Qt types into engine libraries. Editor chrome is Qt; core is Qt-free.
- Scene mutation stays on the game thread. Cross-thread traffic uses `fuse::Handle<T>` and command buffers, not raw object pointers.
- Public APIs live in `fuse::` with `FUSE_*` macros. Do not introduce a second product namespace.

### What we look for

- Tests for the change (`ctest` names under `fuse_*`)
- Behaviour that matches single-thread and multi-thread job profiles unless the PR is specifically about timing
- Honest stubs: if a renderer or GPU path is not real yet, say so in the PR and in comments
- Docs updates when you change a public header, CMake option, or project schema

### How to send a PR

1. Fork and branch from up-to-date `main`.
2. Build the umbrella with core tests (see [`docs/building.md`](docs/building.md)).
3. Run `ctest --test-dir build --output-on-failure` for the targets you touched.
4. Write a short PR body: why, what, how to test.

## Feature requests

Open an issue with the problem, the user, and a proposed module or API. Even better: implement a vertical slice behind `fuse::` with tests.

## Questions

Architecture and module guides: [`docs/README.md`](docs/README.md).
