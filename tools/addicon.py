#!/usr/bin/env python3
"""Copy Ionicons from third_party/asteroid-icons-ion into ui/icons, or resize those already
there, giving each a 160 px intrinsic size with a viewBox over its drawing. Slint renders an
icon at build time at the size it is drawn when it can tell, otherwise at this intrinsic size,
and the icons come at 512 px; 160 px is the largest any screen draws one.

  tools/addicon.py NAME...   copy and resize the named icons
  tools/addicon.py           resize every icon in ui/icons
"""
import os, re, sys

ROOT = os.path.join(os.path.dirname(__file__), '..')
SRC = os.path.join(ROOT, 'third_party/asteroid-icons-ion/src')
DST = os.path.join(ROOT, 'ui/icons')
SIZE = 160


def resize(svg):
    tag = re.search(r'<svg\b[^>]*>', svg)
    head = tag.group(0)
    w = re.search(r'\swidth="([0-9.]+)', head)
    h = re.search(r'\sheight="([0-9.]+)', head)
    if not re.search(r'\sviewBox=', head):
        if not (w and h):
            return svg
        head = head.replace('<svg', f'<svg viewBox="0 0 {w.group(1)} {h.group(1)}"', 1)
    head = re.sub(r'\swidth="[^"]*"', '', head)
    head = re.sub(r'\sheight="[^"]*"', '', head)
    head = head.replace('<svg', f'<svg width="{SIZE}" height="{SIZE}"', 1)
    return svg[:tag.start()] + head + svg[tag.end():]


names = sys.argv[1:]
paths = [os.path.join(SRC, n + '.svg') for n in names] if names else \
        [os.path.join(DST, f) for f in sorted(os.listdir(DST)) if f.endswith('.svg')]
for p in paths:
    svg = open(p).read()
    out = os.path.join(DST, os.path.basename(p))
    new = resize(svg)
    if new != svg or out != p:
        open(out, 'w').write(new)
        print(os.path.basename(out))
