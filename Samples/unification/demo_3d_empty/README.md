# demo_3d_empty

**Proves:** 3D dimension path (World3D + software placeholder renderer)  
**Golden source:** `Templates/BaseGame/game/data/ExampleModule/levels/ExampleLevel.mis`  
**Sample world:** `worlds/example.mis` (hierarchy stub) → cook with `fuse_convert` / `fuse_cook --fuselevel`  
**Binary:** `demo_3d_empty` (built when `FUSE_BUILD_PARITY_DEMOS=ON`)

Cook the bundled mission to the manifest path:

```bash
./build-fuse/Tools/FUSE/fuse_convert \
  --mis Samples/unification/demo_3d_empty/worlds/example.mis \
  --output Samples/unification/demo_3d_empty/worlds/example.fuselevel
```

Run from repo root:

```bash
./build-fuse/Source/FUSE/Apps/Demo3DEmpty/demo_3d_empty Samples/unification/demo_3d_empty
```

**Bundled shapes:** `shapes/Core_GameObjects/unit_cube.dts` and `shapes/Prototyping/carwheel.dae` (copies of the
Templates/BaseGame ShapeAssets, in the Torque `<Module>.module` + `*.asset.taml` layout) are the sources of the
TSStatic stage (`Core_GameObjects:unit_cube`, `Prototyping:carwheel_shape`). They resolve through the project root,
so a packaged or relocated player (`bin/demo_3d_empty project`, any working directory) cooks them without the source
tree.
