"""ChipJuke - banner README i podglad spolecznosciowy (tlo z comfy_banner_bg.py + logotyp z make_brand.py).

Uzycie:  python -I tools/brand/make_banner.py <tlo.png> <katalog_assets_brand> <katalog_wyjsciowy>
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

FONT = os.path.join(os.path.dirname(__file__), "..", "..", "assets", "fonts", "BIZUDGothic-Regular.ttf")
TAGLINE = "tracker-style VGM jukebox for Windows  ·  real notes  ·  per-channel scopes  ·  gamepad"


def make(bg_path, logo, W, H, out):
    bg = Image.open(bg_path).convert("RGB")
    s = max(W / bg.width, H / bg.height)
    bg = bg.resize((int(bg.width * s) + 1, int(bg.height * s) + 1), Image.LANCZOS)
    left, top = (bg.width - W) // 2, (bg.height - H) // 2
    bg = bg.crop((left, top, left + W, top + H)).convert("RGBA")
    bg.alpha_composite(Image.new("RGBA", (W, H), (11, 13, 18, 150)))
    lh = int(H * 0.36)
    lg = logo.resize((logo.width * lh // logo.height, lh), Image.LANCZOS)
    y = int(H * 0.22)
    bg.alpha_composite(lg, ((W - lg.width) // 2, y))
    d = ImageDraw.Draw(bg)
    f = ImageFont.truetype(FONT, int(H * 0.05))
    tw = d.textlength(TAGLINE, font=f)
    ty = y + lh + int(H * 0.08)
    d.rectangle(((W - tw) / 2, ty - int(H * 0.03), (W + tw) / 2, ty - int(H * 0.03) + 4), fill=(255, 183, 77, 255))
    d.text(((W - tw) / 2, ty), TAGLINE, font=f, fill=(227, 232, 240, 255))
    bg.convert("RGB").save(out, optimize=True)


def main():
    bg_path, brand_dir, out_dir = sys.argv[1:4]
    os.makedirs(out_dir, exist_ok=True)
    logo = Image.open(os.path.join(brand_dir, "chipjuke-logo-horizontal.png"))
    make(bg_path, logo, 1280, 533, os.path.join(out_dir, "banner.png"))
    make(bg_path, logo, 1280, 640, os.path.join(out_dir, "social-preview.png"))
    print("ok")


if __name__ == "__main__":
    main()
