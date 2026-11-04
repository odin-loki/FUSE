#!/usr/bin/env python3
"""Builds features_test.ttf, a small TrueType font with OpenType features for the fuse_text_features gate (needs
fontTools).

1000 units per em, every glyph a rectangle; advances tell the substitutions apart:
  f 400, i 300, f_i 500 (liga: "fi" -> f_i), one 300 / one.tnum 600 (tnum), a 500 / a.sc 450 (smcp) / a.ss01 550
  (ss01), A 600, space 250. GPOS: kern A a -100 (kern, on by default).
Usage: python make_features_test_font.py <out.ttf>
"""
import sys

from fontTools.feaLib.builder import addOpenTypeFeaturesFromString
from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen

ADV = {".notdef": 500, "space": 250, "f": 400, "i": 300, "f_i": 500, "one": 300, "one.tnum": 600, "a": 500,
       "a.sc": 450, "a.ss01": 550, "A": 600}

FEA = """
languagesystem DFLT dflt;
languagesystem latn dflt;
feature liga { sub f i by f_i; } liga;
feature tnum { sub one by one.tnum; } tnum;
feature smcp { sub a by a.sc; } smcp;
feature ss01 { sub a by a.ss01; } ss01;
feature kern { pos A a -100; } kern;
"""


def rect(x0, y0, x1, y1):
    pen = TTGlyphPen(None)
    pen.moveTo((x0, y0))
    pen.lineTo((x0, y1))
    pen.lineTo((x1, y1))
    pen.lineTo((x1, y0))
    pen.closePath()
    return pen.glyph()


def main():
    names = list(ADV)
    fb = FontBuilder(1000, isTTF=True)
    fb.setupGlyphOrder(names)
    fb.setupCharacterMap({0x20: "space", 0x66: "f", 0x69: "i", 0x31: "one", 0x61: "a", 0x41: "A"})
    glyphs = {n: rect(50, 0, ADV[n] - 50, 700) for n in names}
    glyphs["space"] = TTGlyphPen(None).glyph()
    fb.setupGlyf(glyphs)
    fb.setupHorizontalMetrics({n: (ADV[n], 0 if n == "space" else 50) for n in names})
    fb.setupHorizontalHeader(ascent=800, descent=-200)
    fb.setupNameTable({"familyName": "FUSE Features Test", "styleName": "Regular"})
    fb.setupOS2(sTypoAscender=800, sTypoDescender=-200, usWinAscent=800, usWinDescent=200)
    fb.setupPost()
    addOpenTypeFeaturesFromString(fb.font, FEA)
    fb.font["head"].created = fb.font["head"].modified = 3849984000  # fixed (2026-01-01): reproducible output
    fb.save(sys.argv[1])


if __name__ == "__main__":
    main()
