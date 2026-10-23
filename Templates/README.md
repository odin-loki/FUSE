# Torque-era templates (reference only)

`Templates/` holds legacy Torque 3D project trees (`BaseGame/`, `Modules/`, …). They are kept for import/converter golden sources and historical comparison—not as shippable FUSE projects.

**Not in FUSE product CI.** The umbrella build and `ctest` gates do not compile or test these trees. A few workflows may still call `fuse_import` against a single `.mis` under `BaseGame/` as a smoke import; that is not full template validation.

**Parity demos:** For maintained, headless projects that exercise the FUSE runtime and CI, use [`Samples/unification/`](../Samples/unification/README.md) (`demo_3d_empty`, `demo_hybrid_hud`, …). See also [`docs/repository-layout.md`](../docs/repository-layout.md).
