"""ChipJuke - generator znaku i logotypu (SVG + PNG + ICO) z jednej definicji geometrii.

Uzycie:  python -I tools/brand/make_brand.py <katalog_wyjsciowy>
Wymaga:  Pillow.  Kroj logotypu to wlasna siatka pikseli 5x7 (bez zewnetrznych licencji).
Zrodlo prawdy dla kolorow: docs/brand/brand-guidelines.md (te same wartosci co src/ui/theme.cpp).
"""
import math
import os
import sys

from PIL import Image, ImageDraw

C = {
    "ink": "#0B0D12", "panel": "#12151C", "line": "#2A3040", "row": "#252C3B",
    "text": "#E3E8F0", "dim": "#8C96A8", "mint": "#69F0AE", "mintDeep": "#1B9E63",
    "amber": "#FFB74D", "sky": "#4FC3F7", "paper": "#F4F6FA",
}


def hexrgb(h):
    h = h.lstrip("#")
    return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))


# ---- geometria znaku na plotnie 1024 x 1024 ----
CX, ARCY = 512, 430
R_OUT, R_IN = 270, 228
X0O, X1O, YBO = CX - R_OUT, CX + R_OUT, 884
X0I, X1I, YBI = CX - R_IN, CX + R_IN, 842
PINS_Y = [560, 650, 740]
PIN_H, PIN_W = 40, 62
ROWS = [(330, 640, 694, 676, "row"), (330, 700, 694, 742, "amber"), (330, 766, 694, 802, "row")]
SMALL_INSET = 30          # wariant <= 32 px: grubszy luk, bez pinow i szarych wierszy


def wave_points(n=64):
    pts = []
    for i in range(n + 1):
        t = i / n
        pts.append((330 + t * 364, 470 - 92 * math.sin(t * 3 * math.pi)))
    return pts


def arch_path(x0, x1, yb, r):
    return f"M{x0},{yb} L{x0},{ARCY} A{r},{r} 0 0 1 {x1},{ARCY} L{x1},{yb} Z"


def svg_icon(mono=None, bg=True):
    m = lambda k: mono if mono else C[k]
    s = ['<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1024 1024" width="1024" height="1024">']
    if bg and not mono:
        s.append(f'<rect width="1024" height="1024" rx="210" fill="{C["panel"]}"/>')
    for y in PINS_Y:
        s.append(f'<rect x="{X0O - PIN_W + 8}" y="{y}" width="{PIN_W}" height="{PIN_H}" rx="8" fill="{m("dim")}"/>')
        s.append(f'<rect x="{X1O - 8}" y="{y}" width="{PIN_W}" height="{PIN_H}" rx="8" fill="{m("dim")}"/>')
    outer = arch_path(X0O, X1O, YBO, R_OUT)
    inner = arch_path(X0I, X1I, YBI, R_IN)
    if mono:
        s.append(f'<path d="{outer} {inner}" fill-rule="evenodd" fill="{mono}"/>')
    else:
        s.append(f'<path d="{outer}" fill="{C["mint"]}"/>')
        s.append(f'<path d="{inner}" fill="{C["ink"]}"/>')
    pts = " ".join(f"{x:.1f},{y:.1f}" for x, y in wave_points())
    s.append(f'<polyline points="{pts}" fill="none" stroke="{m("mint")}" stroke-width="30" '
             'stroke-linecap="round" stroke-linejoin="round"/>')
    for x0, y0, x1, y1, col in ROWS:
        s.append(f'<rect x="{x0}" y="{y0}" width="{x1 - x0}" height="{y1 - y0}" rx="10" fill="{m(col)}"/>')
    s.append("</svg>")
    return "\n".join(s)


def png_icon(px, bg=True, small=None):
    if small is None:
        small = px <= 32
    S = 4096
    k = S / 1024
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    if bg:
        d.rounded_rectangle((0, 0, S - 1, S - 1), radius=int(210 * k), fill=hexrgb(C["panel"]))
    for y in ([] if small else PINS_Y):
        d.rounded_rectangle(((X0O - PIN_W + 8) * k, y * k, (X0O + 8) * k, (y + PIN_H) * k), radius=8 * k, fill=hexrgb(C["dim"]))
        d.rounded_rectangle(((X1O - 8) * k, y * k, (X1O - 8 + PIN_W) * k, (y + PIN_H) * k), radius=8 * k, fill=hexrgb(C["dim"]))

    def arch(x0, x1, yb, r, col):
        d.pieslice(((CX - r) * k, (ARCY - r) * k, (CX + r) * k, (ARCY + r) * k), 180, 360, fill=hexrgb(col))
        d.rectangle((x0 * k, ARCY * k, x1 * k, yb * k), fill=hexrgb(col))

    ins = SMALL_INSET if small else 0
    arch(X0O, X1O, YBO, R_OUT, C["mint"])
    arch(X0I + ins, X1I - ins, YBI - ins, R_IN - ins, C["ink"])
    pts = [(x * k, y * k) for x, y in wave_points(256)]
    w = int((52 if small else 30) * k)
    d.line(pts, fill=hexrgb(C["mint"]), width=w, joint="curve")
    for p in (pts[0], pts[-1]):
        d.ellipse((p[0] - w / 2, p[1] - w / 2, p[0] + w / 2, p[1] + w / 2), fill=hexrgb(C["mint"]))
    for x0, y0, x1, y1, col in ROWS:
        if small and col != "amber":
            continue
        if small:
            y0, y1 = 690, 760
        d.rounded_rectangle((x0 * k, y0 * k, x1 * k, y1 * k), radius=10 * k, fill=hexrgb(C[col]))
    return im.resize((px, px), Image.LANCZOS)


# ---- logotyp: pikselowy kroj 5x7 ----
GLYPHS = {
    "C": [".###.", "#...#", "#....", "#....", "#....", "#...#", ".###."],
    "H": ["#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"],
    "I": [".###.", "..#..", "..#..", "..#..", "..#..", "..#..", ".###."],
    "P": ["####.", "#...#", "#...#", "####.", "#....", "#....", "#...."],
    "J": ["..###", "...#.", "...#.", "...#.", "...#.", "#..#.", ".##.."],
    "U": ["#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."],
    "K": ["#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#"],
    "E": ["#####", "#....", "#....", "####.", "#....", "#....", "#####"],
}


def word_cells(word="CHIPJUKE", split=4):
    cells, x = [], 0
    for i, ch in enumerate(word):
        for r, line in enumerate(GLYPHS[ch]):
            for c, v in enumerate(line):
                if v == "#":
                    cells.append((x + c, r, "text" if i < split else "mint"))
        x += 6
    return cells, x - 1


def cell_color(col, light):
    if col == "mint":
        return C["mintDeep"] if light else C["mint"]
    return C["ink"] if light else C["text"]


def svg_wordmark(p=16, light=False, mono=None):
    cells, w = word_cells()
    s = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w * p} {7 * p}" width="{w * p}" height="{7 * p}">']
    for x, y, col in cells:
        s.append(f'<rect x="{x * p}" y="{y * p}" width="{p - 1}" height="{p - 1}" fill="{mono or cell_color(col, light)}"/>')
    s.append("</svg>")
    return "\n".join(s)


def png_wordmark(p, light=False):
    cells, w = word_cells()
    im = Image.new("RGBA", (w * p, 7 * p), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    gap = max(1, p // 12)
    for x, y, col in cells:
        d.rectangle((x * p, y * p, x * p + p - 1 - gap, y * p + p - 1 - gap), fill=hexrgb(cell_color(col, light)))
    return im


def png_horizontal(height=256, light=False, bg=None):
    icon = png_icon(height)
    wm = png_wordmark(max(2, int(height * 0.62 / 7)), light)
    gap = int(height * 0.28)
    W = height + gap + wm.width + int(height * 0.12)
    im = Image.new("RGBA", (W, height), hexrgb(bg) + (255,) if bg else (0, 0, 0, 0))
    im.alpha_composite(icon, (0, 0))
    im.alpha_composite(wm, (height + gap, (height - wm.height) // 2))
    return im


def svg_horizontal(light=False):
    icon = svg_icon().split("\n")[1:-1]
    p = 22
    cells, w = word_cells()
    W = 256 + 72 + w * p + 30
    s = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} 256" width="{W}" height="256">',
         '<g transform="scale(0.25)">'] + icon + ["</g>", f'<g transform="translate(328,{(256 - 7 * p) // 2})">']
    for x, y, col in cells:
        s.append(f'<rect x="{x * p}" y="{y * p}" width="{p - 2}" height="{p - 2}" fill="{cell_color(col, light)}"/>')
    s += ["</g>", "</svg>"]
    return "\n".join(s)


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)

    def w(name, txt):
        with open(os.path.join(out, name), "w", encoding="utf-8", newline="\n") as f:
            f.write(txt + "\n")

    w("chipjuke-icon.svg", svg_icon())
    w("chipjuke-icon-mono-light.svg", svg_icon(mono=C["text"]))
    w("chipjuke-icon-mono-dark.svg", svg_icon(mono=C["ink"]))
    w("chipjuke-wordmark.svg", svg_wordmark())
    w("chipjuke-wordmark-on-light.svg", svg_wordmark(light=True))
    w("chipjuke-logo-horizontal.svg", svg_horizontal())
    w("chipjuke-logo-horizontal-on-light.svg", svg_horizontal(light=True))
    for n in (16, 24, 32, 48, 64, 128, 256, 512, 1024):
        png_icon(n).save(os.path.join(out, f"chipjuke-icon-{n}.png"))
    sizes = (256, 128, 64, 48, 32, 24, 16)
    ico = [png_icon(n) for n in sizes]
    ico[0].save(os.path.join(out, "chipjuke.ico"), sizes=[(n, n) for n in sizes], append_images=ico[1:])
    png_horizontal(256).save(os.path.join(out, "chipjuke-logo-horizontal.png"))
    png_horizontal(256, bg=C["ink"]).save(os.path.join(out, "chipjuke-logo-horizontal-dark.png"))
    png_horizontal(256, light=True, bg=C["paper"]).save(os.path.join(out, "chipjuke-logo-horizontal-light.png"))
    png_wordmark(24).save(os.path.join(out, "chipjuke-wordmark.png"))
    print("ok", out)


if __name__ == "__main__":
    main()
