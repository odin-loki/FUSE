#!/usr/bin/env python3
"""Builds colrv1_test.ttf, a tiny COLRv1 + CPAL font for the fuse_text_colrv1 gates (needs fontTools).

1000 units per em; "box" is the rect (100, 0)-(900, 800), "inner" (300, 200)-(700, 600), "disc" a circle of radius 350
around (500, 400). Palette 0: 0 red, 1 blue, 2 white, 3 green, 4 yellow, 5 cyan.

Glyphs (cmap):
  U+0041  box: plain outline, no colour record.
  U+E000  two solid layers (PaintColrLayers): red box, half-transparent white inner square.
  U+E001  a linear gradient red (x = 100) -> blue (x = 900) over the box.
  U+E002  a translated PaintColrGlyph of U+E000.
  U+E003  both a v0 record (green) and a v1 graph (red): the v0 record must win.
  U+E004  radial gradient over the box: circles (400, 400) r 50 -> (500, 400) r 300, red -> blue, reflect (rings
          red -> blue -> red ... outwards, the inner circle and the ring centres shifted right).
  U+E005  sweep gradient over the box: centre (500, 400), 0 -> 90 degrees, red -> blue, repeat (four red -> blue fans,
          each starting red at east / north / west / south and turning counter-clockwise to blue).
  U+E006  PaintRotateAroundCenter 45 degrees (counter-clockwise) about (500, 400) of the inner square filled with a
          linear gradient red (left) -> blue (right): a diamond, red at the lower-left, blue at the upper-right.
  U+E007  PaintScaleAroundCenter (1.5, 0.5) about (500, 400) of PaintSkewAroundCenter (x skew 30 degrees) of the inner
          square in green: a flat green parallelogram 600 wide and 200 high centred on (500, 400), its top edge
          shifted left (y axis leaning counter-clockwise).
  U+E008  PaintTransform (xx 1, yx 0.5, xy 0, yy 1, dx 0, dy -250) of the inner square in blue: a parallelogram
          (300, 200) (700, 400) (700, 800) (300, 600) rising to the right.
  U+E009  PaintComposite multiply: yellow rect (100, 0)-(600, 800) backdrop, cyan rect (400, 0)-(900, 800) source:
          yellow | green overlap (400..600) | cyan.
  U+E00A  PaintComposite dest_out (source: green inner square) of PaintComposite src_in (source: red box, backdrop:
          blue disc): a red disc with a transparent square hole.
  U+E00B  red box with a ClipBox (300, 200)-(700, 600): only a red 400 x 400 square.
  U+E00C  box filled with the foreground colour (palette index 0xFFFF) at alpha 0.5.
Usage: python make_colrv1_test_font.py <out.ttf>
"""
import math
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


def circle(cx, cy, r):
    # 8 quadratic arcs (clockwise, like the rects)
    pen = TTGlyphPen(None)
    k = r / math.cos(math.pi / 8)
    pt = lambda rad, a: (round(cx + rad * math.cos(a)), round(cy + rad * math.sin(a)))
    pen.moveTo(pt(r, 0))
    for i in range(8):
        a = -(i + 0.5) * math.pi / 4
        pen.qCurveTo(pt(k, a), pt(r, -(i + 1) * math.pi / 4))
    pen.closePath()
    return pen.glyph()


def main():
    out = sys.argv[1]
    names = [".notdef", "box", "inner", "solid", "grad", "ref", "both",
             "radial", "sweep", "rotate", "scaleskew", "affine", "multiply", "srcin", "clipped", "fg",
             "disc", "left", "right"]
    fb = FontBuilder(1000, isTTF=True)
    fb.setupGlyphOrder(names)
    fb.setupCharacterMap({0xE000: "solid", 0xE001: "grad", 0xE002: "ref", 0xE003: "both", 0x41: "box",
                          0xE004: "radial", 0xE005: "sweep", 0xE006: "rotate", 0xE007: "scaleskew",
                          0xE008: "affine", 0xE009: "multiply", 0xE00A: "srcin", 0xE00B: "clipped", 0xE00C: "fg"})
    glyphs = {n: rect(100, 0, 900, 800) for n in names}
    glyphs["inner"] = rect(300, 200, 700, 600)
    glyphs[".notdef"] = rect(50, 0, 450, 700)
    glyphs["disc"] = circle(500, 400, 350)
    glyphs["left"] = rect(100, 0, 600, 800)
    glyphs["right"] = rect(400, 0, 900, 800)
    fb.setupGlyf(glyphs)
    fb.setupHorizontalMetrics({n: (1000, 100) for n in names})
    fb.setupHorizontalHeader(ascent=900, descent=-200)
    fb.setupNameTable({"familyName": "FUSE COLRv1 Test", "styleName": "Regular"})
    fb.setupOS2(sTypoAscender=900, sTypoDescender=-200, usWinAscent=900, usWinDescent=200)
    fb.setupPost()
    # palette 0: red, blue, white, green, yellow, cyan
    fb.font["CPAL"] = buildCPAL([[(1, 0, 0, 1), (0, 0, 1, 1), (1, 1, 1, 1), (0, 1, 0, 1), (1, 1, 0, 1), (0, 1, 1, 1)]])
    P = ot.PaintFormat
    solid = lambda i, a=1.0: {"Format": P.PaintSolid, "PaletteIndex": i, "Alpha": a}
    glyph = lambda g, p: {"Format": P.PaintGlyph, "Glyph": g, "Paint": p}
    red_blue = lambda extend: {"Extend": extend, "ColorStop": [
        {"StopOffset": 0.0, "PaletteIndex": 0, "Alpha": 1.0},
        {"StopOffset": 1.0, "PaletteIndex": 1, "Alpha": 1.0}]}
    composite = lambda mode, src, backdrop: {"Format": P.PaintComposite, "CompositeMode": mode,
                                             "SourcePaint": src, "BackdropPaint": backdrop}
    color_glyphs = {
        "solid": {"Format": P.PaintColrLayers,
                  "Layers": [glyph("box", solid(0)), glyph("inner", solid(2, 0.5))]},
        "grad": glyph("box", {"Format": P.PaintLinearGradient, "ColorLine": red_blue("pad"),
                              "x0": 100, "y0": 0, "x1": 900, "y1": 0, "x2": 100, "y2": 800}),
        "ref": {"Format": P.PaintTranslate, "dx": 10, "dy": 0,
                "Paint": {"Format": P.PaintColrGlyph, "Glyph": "solid"}},
        "both": glyph("box", solid(0)),
        "radial": glyph("box", {"Format": P.PaintRadialGradient, "ColorLine": red_blue("reflect"),
                                "x0": 400, "y0": 400, "r0": 50, "x1": 500, "y1": 400, "r1": 300}),
        "sweep": glyph("box", {"Format": P.PaintSweepGradient, "ColorLine": red_blue("repeat"),
                               "centerX": 500, "centerY": 400, "startAngle": 0.0, "endAngle": 90.0}),
        "rotate": {"Format": P.PaintRotateAroundCenter, "angle": 45.0, "centerX": 500, "centerY": 400,
                   "Paint": glyph("inner", {"Format": P.PaintLinearGradient, "ColorLine": red_blue("pad"),
                                            "x0": 300, "y0": 400, "x1": 700, "y1": 400, "x2": 300, "y2": 600})},
        "scaleskew": {"Format": P.PaintScaleAroundCenter, "scaleX": 1.5, "scaleY": 0.5,
                      "centerX": 500, "centerY": 400,
                      "Paint": {"Format": P.PaintSkewAroundCenter, "xSkewAngle": 30.0, "ySkewAngle": 0.0,
                                "centerX": 500, "centerY": 400, "Paint": glyph("inner", solid(3))}},
        "affine": {"Format": P.PaintTransform,
                   "Transform": {"xx": 1.0, "yx": 0.5, "xy": 0.0, "yy": 1.0, "dx": 0.0, "dy": -250.0},
                   "Paint": glyph("inner", solid(1))},
        "multiply": composite("multiply", glyph("right", solid(5)), glyph("left", solid(4))),
        "srcin": composite("dest_out", glyph("inner", solid(3)),
                           composite("src_in", glyph("box", solid(0)), glyph("disc", solid(1)))),
        "clipped": glyph("box", solid(0)),
        "fg": glyph("box", solid(0xFFFF, 0.5)),
    }
    colr = buildCOLR(color_glyphs, version=1, glyphMap=fb.font.getReverseGlyphMap(),
                     clipBoxes={"clipped": (300, 200, 700, 600)})
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
