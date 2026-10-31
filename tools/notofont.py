#!/usr/bin/env python3
"""Make a Noto Sans width style selectable on its own: Slint picks fonts by family and weight but
not by width, and Noto's condensed styles share the family "Noto Sans". Each static font given is
cut down to Latin, Greek and Cyrillic without hinting, which the renderer does not use, renamed
to the family "Noto Sans <style>" and written to ui/fonts. Needs fontTools.

  tools/notofont.py NotoSans-SemiCondensedLight.ttf...
"""
import os, sys
from fontTools import subset
from fontTools.ttLib import TTFont

ROOT = os.path.join(os.path.dirname(__file__), '..')
RANGES = [(0x20, 0x24f), (0x370, 0x3ff), (0x400, 0x4ff), (0x2000, 0x206f), (0x20ac, 0x20ac), (0x2122, 0x2122)]

for path in sys.argv[1:]:
    font = TTFont(path)
    names = font['name']
    style = names.getDebugName(17) or names.getDebugName(2)
    family = 'Noto Sans ' + style
    opts = subset.Options()
    opts.name_IDs = [0, 1, 2, 3, 4, 5, 6, 13, 14]
    opts.layout_features = ['*']
    opts.hinting = False
    sub = subset.Subsetter(opts)
    sub.populate(unicodes=[c for a, b in RANGES for c in range(a, b + 1)])
    sub.subset(font)
    for rec in names.names:
        if rec.nameID == 1 or rec.nameID == 4:
            rec.string = family
        elif rec.nameID == 2:
            rec.string = 'Regular'
    out = os.path.join(ROOT, 'ui/fonts', os.path.basename(path))
    font.save(out)
    print(f'{out}: {family}, {os.path.getsize(out)} bytes')
