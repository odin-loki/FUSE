# U8 exit — stakeholder sign-off checklist

**Purpose:** Single “one program” gate for unification exit (prestarter §13.2). Engineering artifacts below are tracked in CI; product sign-off is explicit.

**Last updated:** 2026-10-25

---

## 1. Engineering evidence (CI / local)

| Row | Evidence | Status |
|-----|----------|--------|
| Seven parity demos run headless | `ctest -R '^fuse_u8_demo_'` (six sample-backed + `demo_hybrid_hud` gates) | ✅ Umbrella `fuse-core-linux` |
| Real swapchain present (Linux/Xvfb) | `ctest -R 'fuse_u8_.*_present'` + `--present` / `demo_present.hpp` | ✅ When display wrapper configured |
| Editor embed PIE (seven projects) | `fuse_u8_parity_embed_pie_smoke` | ✅ Lavapipe ICD lock |
| ASan+UBSan on U8 + PIE | `fuse-asan` job: `ctest -L u8` + full unit suite | ✅ `fuse-umbrella-linux.yml` |
| Guides | [player-guide.md](../player-guide.md), [programmer-guide.md](../programmer-guide.md) | ✅ |
| Parity matrix | [demo-corpus-parity-targets.md](./demo-corpus-parity-targets.md) | ✅ Maintained |

---

## 2. Honest limits (documented, not blockers for engineering sign-off)

| Topic | Note |
|-------|------|
| Animated sprites | Atlas **frame index** + per-frame GPU tint; bindless **texture atlas sampling** still Track B |
| Golden ore | Bundled `Samples/unification/*/worlds/` stubs; optional local ore mirror |
| Windows present | GameWindow present path is X11-first; native WSI extension optional |
| Qt live present | `fuse_editor_qt_live_present` on `fuse-editor-qt` job — see [U6-EDITOR.md](./U6-EDITOR.md) |

---

## 3. Stakeholder sign-off (product)

| Question | Owner | Date | OK |
|----------|-------|------|-----|
| The seven U8 demos prove one runtime (2D, 3D, hybrid, modules) | _TBD_ | | ☐ |
| Editor PIE + headless embed match demo projects | _TBD_ | | ☐ |
| Documented limits (sprites, content corpus) acceptable for milestone | _TBD_ | | ☐ |
| **“This is one program”** for the agreed milestone | _TBD_ | | ☐ |

---

## 4. Reproduce locally

```bash
# Headless parity (from build tree with tests enabled)
ctest -R '^fuse_u8_demo_' --output-on-failure

# ASan U8 slice (after cmake --preset fuse-asan)
ctest -L u8 --output-on-failure -j1

# Present (Linux + Xvfb + Lavapipe)
ctest -R 'fuse_u8_.*_present' -j1
```

See [building.md](../building.md) and [U6-EDITOR.md](./U6-EDITOR.md) for editor/Qt present playbooks.
