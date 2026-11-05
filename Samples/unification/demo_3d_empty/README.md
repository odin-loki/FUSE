# demo_3d_empty

**Proves:** 3D dimension path (World3D + software placeholder renderer)  
**Golden source:** `Templates/BaseGame/game/data/ExampleModule/levels/ExampleLevel.mis`  
**Sample world:** `worlds/example.mis` (hierarchy stub) → cook with `fuse_convert` / `fuse_cook --fuselevel`  
**Binary:** `demo_3d_empty` (built when `FUSE_BUILD_PARITY_DEMOS=ON`)

`worlds/example.fuselevel` is gitignored. `demo_3d_empty` cooks it on first use from the golden
mission when present, else from the bundled stub; the ctest fixture `fuse_sample_level_demo_3d_empty` does the same
before the gates that open this sample. By hand:

```bash
./build-fuse/Tools/FUSE/fuse_convert \
  --mis Templates/BaseGame/game/data/ExampleModule/levels/ExampleLevel.mis \
  --output Samples/unification/demo_3d_empty/worlds/example.fuselevel
# or, without the golden mission: --mis Samples/unification/demo_3d_empty/worlds/example.mis
```

Run from repo root:

```bash
./build-fuse/Source/FUSE/Apps/Demo3DEmpty/demo_3d_empty Samples/unification/demo_3d_empty
```
