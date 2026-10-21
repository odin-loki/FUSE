## Summary

<!-- What problem does this solve? Link issues if applicable. -->

## Test plan

- [ ] Configured with a relevant preset (for example `fuse-debug`) or documented manual CMake flags
- [ ] `ctest --test-dir build/<preset-binary-dir> --output-on-failure` for targets you touched
- [ ] Parity demos under `Samples/unification/` exercised when project wiring or converters change

## Checklist

- [ ] Changes are compatible with AGPL-3.0 ([`LICENSE.md`](../LICENSE.md)); licence notices preserved for vendored or adapted code
- [ ] No Qt types in engine libraries under `Source/FUSE/` (Qt is editor-only)
- [ ] Scene mutation stays on the game thread; cross-thread handles use `fuse::Handle<T>` / command buffers
- [ ] Docs updated for public headers, CMake options, or `project.json` schema changes
- [ ] No new git submodules; third-party code stays under `vendor/` unless maintainers agree otherwise
