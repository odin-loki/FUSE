# Parity sprite map stubs

Horizontal PNG strips referenced by T2D `imageMap` fields in parity demos (e.g. `HeroSheet.png` in `demo_2d_sprites`).

Regenerate `HeroSheet.png`:

```bash
python Tools/FUSE/gen_hero_sheet_png.py
```

Search order: `<project>/assets/`, `<project>/worlds/`, then `Samples/unification/common/sprite_maps/`.
