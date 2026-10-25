# U8 exit — sign-off checklist

**Purpose:** Single “one program” gate for unification exit (prestarter §13.2).

**Engineering exit:** 2026-10-25 (CI-backed)  
**Product exit:** milestone owner signs §3 when ready

---

## 1. Engineering evidence (CI / local)

| Row | Evidence | Status |
|-----|----------|--------|
| Seven parity demos run headless | `ctest -R '^fuse_u8_demo_'` + umbrella manual demo steps | ✅ `fuse-core-linux` |
| Real swapchain present (Linux/Xvfb) | `ctest -L 'u8;present'` / `fuse_u8_<demo>_present` | ✅ `fuse-u8-present` job |
| Editor embed PIE (seven projects) | `fuse_u8_parity_embed_pie_smoke` | ✅ Lavapipe ICD lock |
| ASan+UBSan on U8 + PIE | `fuse-asan` → `ctest -L u8` | ✅ `fuse-smoke-asan` |
| Animated sprites + `imageMap` | PNG strip sample → `SpriteDrawCmd` + hybrid atlas strip | ✅ `fuse_world2d_sprite_image_map_tests`, `demo_2d_sprites` |
| Editor PIE transport (Qt UI) | Toolbar Play/Pause/Stop | ✅ `fuse_editor_qt_e15_transport` (`fuse-editor-qt`) |
| Guides | [player-guide.md](../player-guide.md), [programmer-guide.md](../programmer-guide.md) | ✅ |
| Parity matrix | [demo-corpus-parity-targets.md](./demo-corpus-parity-targets.md) | ✅ |

---

## 2. Documented follow-ups (post-U8 engineering)

| Topic | Note |
|-------|------|
| Bindless GPU texture fetch | `imageMap` colors sampled on CPU from PNG; hybrid still uses sprite-layer quads |
| Golden ore | `third_party/Torque2D` SpriteToy when present; else bundled stubs |
| macOS present | Win32 + X11 GameWindow; Cocoa WSI not in parity CI |
| Qt live present | `fuse_editor_qt_live_present` on `fuse-editor-qt` — [U6-EDITOR.md](./U6-EDITOR.md) |

---

## 3. Stakeholder sign-off (product)

| Question | Owner | Date | OK |
|----------|-------|------|-----|
| The seven U8 demos prove one runtime (2D, 3D, hybrid, modules) | Engineering (CI) | 2026-10-25 | ☑ |
| Editor PIE + headless embed match demo projects | Engineering (CI) | 2026-10-25 | ☑ |
| Documented limits (sprites, content corpus) acceptable for milestone | Program / leads | _pending_ | ☐ |
| **“This is one program”** for the agreed milestone | Program sponsor | _pending_ | ☐ |

---

## 4. Reproduce locally

```bash
ctest -R '^fuse_u8_demo_' --output-on-failure
ctest -L u8 --output-on-failure -j1          # after cmake --preset fuse-asan
ctest -R 'fuse_u8_.*_present' -j1              # Linux + Xvfb + Lavapipe
ctest -R fuse_hybrid_sprite_layer_cpu -j1
```

See [building.md](../building.md) and [U6-EDITOR.md](./U6-EDITOR.md).
