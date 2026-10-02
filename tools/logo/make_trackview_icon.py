"""Icon of ALVR PS4 Tracking Viewer (companion/trackview.ico): the ALVR logo above the lobby's
floor grid, on a dark rounded square. Run from the repo root: python tools/logo/make_trackview_icon.py"""
from PIL import Image, ImageDraw

S = 1024  # drawn large, then scaled down
img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
d = ImageDraw.Draw(img)
d.rounded_rectangle((32, 32, S - 32, S - 32), radius=180, fill=(8, 11, 16, 255))

# Floor grid in perspective, vanishing above the middle, fading with distance.
horizon, bottom, vx = 470, S - 60, S / 2
for i in range(-6, 7):
    x0 = vx + i * 170
    d.line([(vx + i * 22, horizon + 40), (x0, bottom)], fill=(150, 160, 170, 255), width=10)
for k in range(1, 7):
    t = (k / 6) ** 1.9
    y = horizon + 40 + t * (bottom - horizon - 40)
    half = 140 + t * 900
    c = int(80 + 110 * t)
    d.line([(vx - half, y), (vx + half, y)], fill=(c, c + 8, c + 16, 255), width=10)
mask = Image.new("L", (S, S), 0)
ImageDraw.Draw(mask).rounded_rectangle((32, 32, S - 32, S - 32), radius=180, fill=255)
img.putalpha(Image.composite(img.getchannel("A"), mask, mask))

logo = Image.open("tools/logo/alvr_icon.png").convert("RGBA").resize((520, 520), Image.LANCZOS)
img.alpha_composite(logo, ((S - 520) // 2, 70))

img = img.resize((256, 256), Image.LANCZOS)
img.save("companion/trackview.ico", sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
print("companion/trackview.ico written")
