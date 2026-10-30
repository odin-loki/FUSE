# FUSE

**F**ast **U**nified **S**imulation **E**ngine

Torque3D-lineage engine modernised to ISO C++23, memory-safe ownership (handles + allocators), Vulkan/CUDA compute, and a **Qt 6** editor.

## Status

Planning complete. Implementation starts at Track A **P0** (repo skeleton, CMake, Qt hello).

See [`docs/plans/FUSE_MASTER_PLAN.md`](docs/plans/FUSE_MASTER_PLAN.md) (full fidelity) and [`docs/plans/FUSE_MASTER_PLAN_TOC.md`](docs/plans/FUSE_MASTER_PLAN_TOC.md).

## Layout

```
fuse/
├── core/ platform/ renderer/ compute/ physics/ ecs/
├── assets/ audio/ editor/ runtime_ui/ game/ compat/ tools/
├── tests/ third_party/ docs/
└── README.md
```

## License

Engine code will ship under terms documented in `THIRD_PARTY.md` / `LICENSE` (Torque3D MIT notices preserved in `compat/` and attributions).
