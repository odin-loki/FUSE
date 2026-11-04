#!/usr/bin/env python3
"""Builds colrv1_test.ttf, a tiny COLRv1 + CPAL font for the fuse_text_colrv1 gate (needs fontTools).

Glyphs (cmap): U+E000 two solid layers (PaintColrLayers), U+E001 a linear gradient red -> blue, U+E002 a translated
PaintColrGlyph of U+E000, U+E003 both a v0 record (green) and a v1 graph (red): the v0 record must win.
Usage: python make_colrv1_test_font.py <out.ttf>
"""
import sys

from fontTools.colorLib.builder import buildCOLR, buildCPAL
from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.ttLib.tables import otTables as ot


def rect(x0, y0, x1, y1):
    pen = TTGlyphPen(None)
    pen.moveTo((x0, y0))
    pen.lineTo((x0, y1))
    pen.lineTo((x1, y1))
    pen.lineTo((x1, y0))
    pen.closePath()
    return pen.glyph()


def main():
    out = sys.argv[1]
    names = [".notdef", "box", "inner", "solid", "grad", "ref", "both"]
    fb = FontBuilder(1000, isTTF=True)
    fb.setupGlyphOrder(names)
    fb.setupCharacterMap({0xE000: "solid", 0xE001: "grad", 0xE002: "ref", 0xE003: "both", 0x41: "box"})
    glyphs = {n: rect(100, 0, 900, 800) for n in names}
    glyphs["inner"] = rect(300, 200, 700, 600)
    glyphs[".notdef"] = rect(50, 0, 450, 700)
    fb.setupGlyf(glyphs)
    fb.setupHorizontalMetrics({n: (1000, 100) for n in names})
    fb.setupHorizontalHeader(ascent=900, descent=-200)
    fb.setupNameTable({"familyName": "FUSE COLRv1 Test", "styleName": "Regular"})
    fb.setupOS2(sTypoAscender=900, sTypoDescender=-200, usWinAscent=900, usWinDescent=200)
    fb.setupPost()
    # palette 0: red, blue, white, green
    fb.font["CPAL"] = buildCPAL([[(1, 0, 0, 1), (0, 0, 1, 1), (1, 1, 1, 1), (0, 1, 0, 1)]])
    solid = lambda i, a=1.0: {"Format": ot.PaintFormat.PaintSolid, "PaletteIndex": i, "Alpha": a}
    glyph = lambda g, p: {"Format": ot.PaintFormat.PaintGlyph, "Glyph": g, "Paint": p}
    color_glyphs = {
        "solid": {"Format": ot.PaintFormat.PaintColrLayers,
                  "Layers": [glyph("box", solid(0)), glyph("inner", solid(2, 0.5))]},
        "grad": glyph("box", {"Format": ot.PaintFormat.PaintLinearGradient,
                              "ColorLine": {"Extend": "pad", "ColorStop": [
                                  {"StopOffset": 0.0, "PaletteIndex": 0, "Alpha": 1.0},
                                  {"StopOffset": 1.0, "PaletteIndex": 1, "Alpha": 1.0}]},
                              "x0": 100, "y0": 0, "x1": 900, "y1": 0, "x2": 100, "y2": 800}),
        "ref": {"Format": ot.PaintFormat.PaintTranslate, "dx": 10, "dy": 0,
                "Paint": {"Format": ot.PaintFormat.PaintColrGlyph, "Glyph": "solid"}},
        "both": glyph("box", solid(0)),
    }
    colr = buildCOLR(color_glyphs, version=1, glyphMap=fb.font.getReverseGlyphMap())
    # a v0 record for "both": one green layer
    rec = ot.BaseGlyphRecord()
    rec.BaseGlyph, rec.FirstLayerIndex, rec.NumLayers = "both", 0, 1
    layer = ot.LayerRecord()
    layer.LayerGlyph, layer.PaletteIndex = "box", 3
    colr.table.BaseGlyphRecordArray = ot.BaseGlyphRecordArray()
    colr.table.BaseGlyphRecordArray.BaseGlyphRecord = [rec]
    colr.table.BaseGlyphRecordCount = 1
    colr.table.LayerRecordArray = ot.LayerRecordArray()
    colr.table.LayerRecordArray.LayerRecord = [layer]
    colr.table.LayerRecordCount = 1
    fb.font["COLR"] = colr
    fb.save(out)


if __name__ == "__main__":
    main()
