"""Make the save data icon (Settings > Application Saved Data Management): the app icon
centred on white, 228x128.

Usage: python tools/logo/make_save_icon.py
Reads client/icons/icon0.png, writes client/icons/save_data.png.
"""
import os

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(ROOT, "client", "icons", "icon0.png")
OUT = os.path.join(ROOT, "client", "icons", "save_data.png")
W, H = 228, 128


def main():
    icon = Image.open(SRC).convert("RGB").resize((H, H), Image.LANCZOS)
    im = Image.new("RGB", (W, H), (255, 255, 255))
    im.paste(icon, ((W - H) // 2, 0))
    im.save(OUT, optimize=True)
    print(f"{os.path.relpath(OUT, ROOT)}: {os.path.getsize(OUT)} bytes")


main()
