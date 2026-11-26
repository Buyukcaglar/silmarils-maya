"""Render matching images from the Amiga and PC ports side by side for the fidelity check."""
import sys, os
sys.path.insert(0, os.path.dirname(__file__))
from alis_res import *

OUT = os.path.join(os.path.dirname(__file__), '..', 'docs', 'fidelity')
os.makedirs(OUT, exist_ok=True)

def load(port, name):
    d, ext = (AMIGA_DIR, '.CO') if port == 'amiga' else (DOS_DIR, '.IO')
    f = next(f for f in os.listdir(d) if f.upper() == name + ext)
    return Script(os.path.join(d, f))

def pal(s, idx):
    return palette_rgb(s, dict(s.resources())[idx])

def render(s, idx, palette, scale=2):
    t, w, h, bpp, rows = decode_bitmap(s, dict(s.resources())[idx])
    out = []
    for r in rows:
        line = [palette[v % len(palette)] for v in r for _ in range(scale)]
        out += [line] * scale
    return out

def side_by_side(name, idx, pal_name, pal_idx, scale=2, gap=8):
    A, P = load('amiga', name), load('pc', name)
    PA, PP = load('amiga', pal_name), load('pc', pal_name)
    ia = render(A, idx, pal(PA, pal_idx), scale); ip = render(P, idx, pal(PP, pal_idx), scale)
    h = max(len(ia), len(ip)); bg = (40, 40, 40)
    rows = []
    for y in range(h):
        ra = ia[y] if y < len(ia) else [bg] * len(ia[0])
        rp = ip[y] if y < len(ip) else [bg] * len(ip[0])
        rows.append(ra + [bg] * gap + rp)
    fn = os.path.join(OUT, f'{name.lower()}_{idx}.png')
    write_png(fn, rows)
    return fn

if __name__ == '__main__':
    jobs = [('PRESENTE', 1, 'PRESENTE', 0), ('RANGE', 1, 'RANGE', 0), ('DECOR', 14, 'DECOR', 17),
            ('COPAN', 12, 'COPAN', 0), ('CHAMBRE', 0, 'CHAMBRE', 9), ('LOGO', 0, 'LOGO', 6),
            ('IGAGNE', 1, 'IGAGNE', 0), ('VILLAGE', 3, 'VILLAGE', 0)]
    for j in jobs:
        print(side_by_side(*j))
