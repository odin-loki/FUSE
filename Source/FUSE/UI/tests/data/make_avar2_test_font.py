#!/usr/bin/env python3
"""Builds variable_avar2_test.ttf for the fuse_text_variable gate (needs fontTools >= 4.40): variable_test.ttf with its
avar table rewritten as version 2.

The segment maps stay (wght: user 600 -> design 700, normalised 0.4 -> 0.6); the avar2 ItemVariationStore adds a
cross-axis mapping: normalised wdth += 0.5 * normalised wght for wght above its default (one region: wght 0 .. 1 peak 1;
item 1 = wdth delta 8192 F2DOT14). There is no axisIndexMap, so axis i reads item (0, i); item 0 (wght) has no delta.
So wght 900 at the default wdth draws as wght 900 / wdth 125 of variable_test.ttf, and wght 600 as wdth 115
(normalised wght 0.6 after the segment map: wdth 0.3).
Usage: python make_avar2_test_font.py <variable_test.ttf> <out.ttf>
"""
import sys

from fontTools.ttLib import TTFont
from fontTools.ttLib.tables import otTables as ot
from fontTools.varLib import builder


def main():
    font = TTFont(sys.argv[1])
    avar = font["avar"]
    axes = [a.axisTag for a in font["fvar"].axes]
    assert axes == ["wght", "wdth"], axes
    regions = builder.buildVarRegionList([{"wght": (0.0, 1.0, 1.0)}], axes)
    data = builder.buildVarData([0], [[0], [8192]], optimize=False)
    store = builder.buildVarStore(regions, [data])
    avar.majorVersion = 2
    avar.minorVersion = 0
    if getattr(avar, "table", None) is None:
        avar.table = ot.avar()
    avar.table.VarIdxMap = None
    avar.table.VarStore = store
    font.save(sys.argv[2])


if __name__ == "__main__":
    main()
