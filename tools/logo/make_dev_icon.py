"""Make the icon of the "ALVR PS4 (Dev)" package: the app icon with a large black "DEV".

Usage: python tools/logo/make_dev_icon.py
Reads client/icons/icon0.png, writes client/icons/icon0_dev.png (512x512).
"""
import os

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(ROOT, "client", "icons", "icon0.png")
OUT = os.path.join(ROOT, "client", "icons", "icon0_dev.png")
FONTS = [r"C:\Windows\Fonts\ariblk.ttf", r"C:\Windows\Fonts\arialbd.ttf"]


def main():
    im = Image.open(SRC).convert("RGB")
    draw = ImageDraw.Draw(im)
    font_path = next(p for p in FONTS if os.path.exists(p))
    text = "DEV"
    # Largest size that keeps the text within 80 % of the icon width.
    size = 40
    while True:
        font = ImageFont.truetype(font_path, size + 4)
        l, t, r, b = draw.textbbox((0, 0), text, font=font)
        if r - l > im.width * 0.80:
            break
        size += 4
    font = ImageFont.truetype(font_path, size)
    l, t, r, b = draw.textbbox((0, 0), text, font=font)
    x = (im.width - (r - l)) / 2 - l
    y = (im.height - (b - t)) / 2 - t
    draw.text((x, y), text, font=font, fill=(0, 0, 0))
    im.save(OUT)
    print(f"{os.path.relpath(OUT, ROOT)}: {font_path}, size {size}")


main()
