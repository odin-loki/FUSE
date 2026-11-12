#!/usr/bin/env python3
"""Builds variable_test.ttf, a small variable TrueType font for the fuse_text_variable gate (needs fontTools).

Axes: wght 100..900 (default 400) with an avar map (user 600 -> design 700, so normalised 0.4 -> 0.6), and
wdth 50..150 (default 100). Masters at wght 100 / 400 / 900 (wdth 100) and wdth 50 / 150 (wght 400) are merged by
fontTools.varLib, which writes fvar, avar, gvar (IUP-optimised: untouched points must be interpolated), HVAR, MVAR and a
variable COLR (Var* paints + ItemVariationStore).

1000 units per em; at design weight w the stem is s(w) = 40 (w 100), 100 (w 400), 300 (w 900), linear in between per
segment; width w scales every x by wdth / 100.
  U+0041 'A'  a rectangle x 100 .. 100 + s, y 0 .. 700 drawn with 8 on-curve points (corners and edge midpoints) and
              advance 300 + s; its x extent is the stem.
  U+0042 'B'  composite: 'A' at (0, 0) and 'C' at (s + 150, 0); advance 600 + s.
  U+0043 'C'  square 0 .. 100 + s/2 on both axes with a quadratic bump on top (off-curve points); advance 200 + s.
  U+E000      COLRv1: PaintColrLayers [box filled with PaintVarSolid red at alpha 0.2 (w 100) / 0.6 (w 400) / 1.0
              (w 900); box filled with a linear gradient blue -> green whose x1 runs 300 / 600 / 900, translated by
              dx 0 / 50 / 100 (PaintVarTranslate)]. Palette 0: 0 red, 1 blue, 2 green.
Metrics: hhea / OS/2 typo ascender 800 at w 100 and 400, 900 at w 900 (MVAR hasc); descender -200.
Named instances: Thin (100, 100), Regular (400, 100), SemiBold (600, 100), Black (900, 100), Condensed (400, 50),
Bold Wide (700, 150).
Usage: python make_variable_test_font.py <out.ttf>
"""
import sys

from fontTools import varLib
from fontTools.colorLib.builder import buildCOLR, buildCPAL
from fontTools.designspaceLib import AxisDescriptor, DesignSpaceDocument, InstanceDescriptor, SourceDescriptor
from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.ttLib.tables import otTables as ot

NAMES = [".notdef", "space", "A", "B", "C", "colr", "box"]


def stem(w):
    return 40 + (w - 100) * 60 / 300 if w <= 400 else 100 + (w - 400) * 200 / 500


def master(wght, wdth):
    s = stem(wght)
    k = wdth / 100.0
    X = lambda x: round(x * k)
    pen = TTGlyphPen(None)  # 'A': 8 on-curve points, midpoints included (their deltas are left to IUP)
    x0, x1 = X(100), X(100 + s)
    pen.moveTo((x0, 0))
    pen.lineTo((x0, 350))
    pen.lineTo((x0, 700))
    pen.lineTo((round((x0 + x1) / 2), 700))
    pen.lineTo((x1, 700))
    pen.lineTo((x1, 350))
    pen.lineTo((x1, 0))
    pen.lineTo((round((x0 + x1) / 2), 0))
    pen.closePath()
    a = pen.glyph()
    c_side = 100 + s / 2
    pen = TTGlyphPen(None)  # 'C': square with a quadratic bump on top
    pen.moveTo((0, 0))
    pen.lineTo((0, round(c_side)))
    pen.qCurveTo((X(c_side / 4), round(c_side + 60)), (X(c_side * 3 / 4), round(c_side + 60)), (X(c_side), round(c_side)))
    pen.lineTo((X(c_side), 0))
    pen.closePath()
    c = pen.glyph()
    pen = TTGlyphPen({"A": a, "C": c})
    pen.addComponent("A", (1, 0, 0, 1, 0, 0))
    pen.addComponent("C", (1, 0, 0, 1, X(s + 150), 0))
    b = pen.glyph()

    def rect(x0, y0, x1, y1):
        p = TTGlyphPen(None)
        p.moveTo((x0, y0))
        p.lineTo((x0, y1))
        p.lineTo((x1, y1))
        p.lineTo((x1, y0))
        p.closePath()
        return p.glyph()

    glyphs = {".notdef": rect(50, 0, 450, 700), "space": TTGlyphPen(None).glyph(), "A": a, "B": b, "C": c,
              "colr": rect(100, 0, 900, 800), "box": rect(100, 0, 900, 800)}
    fb = FontBuilder(1000, isTTF=True)
    fb.setupGlyphOrder(NAMES)
    fb.setupCharacterMap({0x20: "space", 0x41: "A", 0x42: "B", 0x43: "C", 0xE000: "colr"})
    fb.setupGlyf(glyphs)
    glyf = fb.font["glyf"]
    adv = {".notdef": 500, "space": 250, "A": X(300 + s), "B": X(600 + s), "C": X(200 + s), "colr": 1000, "box": 1000}
    metrics = {}
    for n in NAMES:
        g = glyf[n]
        g.recalcBounds(glyf)
        metrics[n] = (adv[n], getattr(g, "xMin", 0) if g.numberOfContours else 0)
    fb.setupHorizontalMetrics(metrics)
    asc = 900 if wght >= 900 else 800
    fb.setupHorizontalHeader(ascent=800, descent=-200)
    fb.setupNameTable({"familyName": "FUSE Variable Test", "styleName": "Regular"})
    fb.setupOS2(sTypoAscender=asc, sTypoDescender=-200, sTypoLineGap=0, usWinAscent=900, usWinDescent=200,
                usWeightClass=400)
    fb.setupPost()
    fb.font["CPAL"] = buildCPAL([[(1, 0, 0, 1), (0, 0, 1, 1), (0, 1, 0, 1)]])
    P = ot.PaintFormat
    alpha = {100: 0.2, 400: 0.6, 900: 1.0}.get(wght, 0.6)
    x1 = {100: 300, 400: 600, 900: 900}.get(wght, 600)
    dx = {100: 0, 400: 50, 900: 100}.get(wght, 50)
    grad = {"Format": P.PaintLinearGradient,
            "ColorLine": {"Extend": "pad", "ColorStop": [{"StopOffset": 0.0, "PaletteIndex": 1, "Alpha": 1.0},
                                                         {"StopOffset": 1.0, "PaletteIndex": 2, "Alpha": 1.0}]},
            "x0": 100, "y0": 0, "x1": x1, "y1": 0, "x2": 100, "y2": 800}
    color_glyphs = {"colr": {"Format": P.PaintColrLayers, "Layers": [
        {"Format": P.PaintGlyph, "Glyph": "box", "Paint": {"Format": P.PaintSolid, "PaletteIndex": 0, "Alpha": alpha}},
        {"Format": P.PaintTranslate, "dx": dx, "dy": 0,
         "Paint": {"Format": P.PaintGlyph, "Glyph": "box", "Paint": grad}}]}}
    fb.font["COLR"] = buildCOLR(color_glyphs, version=1, glyphMap=fb.font.getReverseGlyphMap())
    return fb.font


def main():
    out = sys.argv[1]
    ds = DesignSpaceDocument()
    wght = AxisDescriptor()
    wght.tag, wght.name, wght.minimum, wght.default, wght.maximum = "wght", "Weight", 100, 400, 900
    wght.map = [(100, 100), (400, 400), (600, 700), (900, 900)]
    wdth = AxisDescriptor()
    wdth.tag, wdth.name, wdth.minimum, wdth.default, wdth.maximum = "wdth", "Width", 50, 100, 150
    ds.addAxis(wght)
    ds.addAxis(wdth)
    for w, d in [(400, 100), (100, 100), (900, 100), (400, 50), (400, 150)]:
        src = SourceDescriptor()
        src.font = master(w, d)
        src.location = {"Weight": w, "Width": d}
        src.styleName = f"w{w} d{d}"
        ds.addSource(src)
    for name, w, d in [("Thin", 100, 100), ("Regular", 400, 100), ("SemiBold", 600, 100), ("Black", 900, 100),
                       ("Condensed", 400, 50), ("Bold Wide", 700, 150)]:
        inst = InstanceDescriptor()
        inst.familyName = "FUSE Variable Test"
        inst.styleName = name
        inst.userLocation = {"Weight": w, "Width": d}
        ds.addInstance(inst)
    font, _, _ = varLib.build(ds, exclude=["STAT"])
    font["head"].created = font["head"].modified = 3849984000  # fixed (2026-01-01): reproducible output
    font.save(out)


if __name__ == "__main__":
    main()
