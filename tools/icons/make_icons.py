"""SteamVR status icons of the PSVR (headset) and the PS Move (controllers / trackers).

Writes pc-setup/icons/, which the PC setup installs into ALVR-PS4_PC-Streamer/resources/icons
(the ALVR driver's resource folder, "{alvr_server}/icons/..." in the device properties).

SteamVR's icon conventions, taken from its own drivers (drivers/htc/resources/icons):
- one flat colour, SteamVR green #85B717 (133, 183, 23), antialiased through the alpha only:
  SteamVR derives its green/blue gradient variants (name.b4bfb144.png) and its grey variants
  (name.6e6c89c9.png) from that exact green, in the driver's icon folder;
- off: grey #4E4D52; standby: outline only; searching: animated GIF (29 frames fading
  linearly to about 25 % and back); alert / low battery / error: a round badge at the
  bottom right (green with an "i", orange with a lightning bolt, pink with an "!"), cut out
  of the icon by a transparent ring, with the symbol cut out of the badge;
- headsets 50x32, controllers and trackers 32x32, plus @2x versions at twice the size.

PSVR: the icon drawn by leonmc330 (github.com/leonmc330), tools/icons/psvr_leonmc330.png
(512x512): its shape, in SteamVR's green and grey, with the error badge in its red. PS Move:
drawn here, at the angle of SteamVR's Vive wand icon (-46 degrees), after the PS Move icon of
PSMoveSteamVRBridge (Apache-2.0, github.com/HipsterSloth).

Usage: python tools/icons/make_icons.py
"""
import math
import os

from PIL import Image, ImageChops, ImageDraw, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, "pc-setup", "icons")
SS = 16  # supersampling: shapes are built at 16x the 32 px design grid

GREEN = (133, 183, 23)
GREY = (78, 77, 82)
BADGE = {"alert": GREEN, "low": (238, 64, 38), "error": (255, 0, 0)}  # error: the red of leonmc330's icons


def blank(w, h):
    return Image.new("L", (w * SS, h * SS), 0)


def smooth(mask, radius):
    """Round off a high resolution mask (blur, then threshold)."""
    return mask.filter(ImageFilter.GaussianBlur(radius * SS)).point(lambda v: 255 if v >= 128 else 0)


def erode(mask, px):
    return mask.filter(ImageFilter.GaussianBlur(px * SS * 0.75)).point(lambda v: 255 if v >= 250 else 0)


def dilate(mask, px):
    return mask.filter(ImageFilter.GaussianBlur(px * SS * 0.75)).point(lambda v: 255 if v >= 5 else 0)


# --- shapes (design grid units: 1 = one pixel of the 32 px icon) --------------------------

def psvr_shape(w, h):
    """leonmc330's PSVR (its alpha), fitted into the icon with a 1 px margin, centred."""
    src = Image.open(os.path.join(ROOT, "tools", "icons", "psvr_leonmc330.png")).convert("RGBA").getchannel("A")
    src = src.crop(src.getbbox())
    scale = min((w - 2) * SS / src.width, (h - 2) * SS / src.height)
    big = src.resize((round(src.width * scale), round(src.height * scale)), Image.LANCZOS)
    m = blank(w, h)
    m.paste(big, ((w * SS - big.width) // 2, (h * SS - big.height) // 2))
    return m


def psmove_shape(w, h):
    """PS Move side view along an axis at -46 degrees: handle bottom left, sphere top right."""
    m = blank(w, h)
    d = ImageDraw.Draw(m)
    ang = math.radians(-46.0)
    ux, uy = math.cos(ang), math.sin(ang)          # along the axis, towards the sphere
    vx, vy = -uy, ux                               # across
    ox, oy = 3.4, 28.6                             # handle end (design units)

    def pt(t, s):
        return ((ox + ux * t + vx * s) * SS, (oy + uy * t + vy * s) * SS)

    def capsule(t0, t1, r0, r1, fill=255):
        # tapered body with round ends
        poly = [pt(t0, -r0), pt(t1, -r1), pt(t1, r1), pt(t0, r0)]
        d.polygon(poly, fill=fill)
        for t, r in ((t0, r0), (t1, r1)):
            cx, cy = pt(t, 0)
            d.ellipse((cx - r * SS, cy - r * SS, cx + r * SS, cy + r * SS), fill=fill)

    # proportions of the real controller: 44 mm sphere, handle about as wide at the top
    sphere_r = 4.9
    sphere_t = 29.4
    capsule(1.9, 21.6, 2.9, 3.7)                   # handle, wider towards the top
    # transparent ring between the neck and the sphere, then the sphere
    cx, cy = pt(sphere_t, 0)
    r = (sphere_r + 1.0) * SS
    d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=0)
    r = sphere_r * SS
    d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=255)
    # Move button (a hole near the top of the handle), like the hole of the Vive icon
    bx, by = pt(19.0, 0)
    r = 1.35 * SS
    d.ellipse((bx - r, by - r, bx + r, by + r), fill=0)
    return m


# --- badges ------------------------------------------------------------------------------

def badge_geometry(w, h):
    r = 5.6
    return (w - r - 2.4) * SS, (h - r - 2.4) * SS, r * SS


def badge(w, h, kind):
    """(ring cut out of the icon, badge disc, symbol cut out of the disc)"""
    cx, cy, r = badge_geometry(w, h)
    ring, disc, sym = blank(w, h), blank(w, h), blank(w, h)
    g = 1.6 * SS
    ImageDraw.Draw(ring).ellipse((cx - r - g, cy - r - g, cx + r + g, cy + r + g), fill=255)
    ImageDraw.Draw(disc).ellipse((cx - r, cy - r, cx + r, cy + r), fill=255)
    s = ImageDraw.Draw(sym)
    u = SS
    if kind == "alert":      # "i"
        s.ellipse((cx - 0.95 * u, cy - 3.6 * u, cx + 0.95 * u, cy - 1.7 * u), fill=255)
        s.rounded_rectangle((cx - 0.9 * u, cy - 0.9 * u, cx + 0.9 * u, cy + 3.5 * u), radius=0.5 * u, fill=255)
    elif kind == "error":    # "!"
        s.polygon([(cx - 1.1 * u, cy - 3.6 * u), (cx + 1.1 * u, cy - 3.6 * u), (cx + 0.6 * u, cy + 1.0 * u),
                   (cx - 0.6 * u, cy + 1.0 * u)], fill=255)
        s.ellipse((cx - 0.9 * u, cy + 1.9 * u, cx + 0.9 * u, cy + 3.7 * u), fill=255)
    elif kind == "low":      # lightning bolt
        s.polygon([(cx + 0.9 * u, cy - 4.0 * u), (cx - 2.2 * u, cy + 0.6 * u), (cx - 0.1 * u, cy + 0.6 * u),
                   (cx - 1.0 * u, cy + 4.0 * u), (cx + 2.2 * u, cy - 0.7 * u), (cx + 0.1 * u, cy - 0.7 * u)],
                  fill=255)
    return ring, disc, sym


# --- composition -------------------------------------------------------------------------

def compose(layers, w, h, scale):
    """layers: [(mask, rgb)] from bottom to top, high resolution. Returns RGBA at scale x."""
    W, H = w * scale, h * scale
    acc_a = Image.new("L", (w * SS, h * SS), 0)
    acc = Image.new("RGB", (w * SS, h * SS), (0, 0, 0))
    for mask, rgb in layers:
        acc.paste(rgb, (0, 0), mask)
        acc_a = ImageChops.lighter(acc_a, mask)
    # premultiplied downscale, so edges keep the colour instead of fading to black
    pre = Image.composite(acc, Image.new("RGB", acc.size, (0, 0, 0)), acc_a)
    small_rgb = pre.resize((W, H), Image.BOX)
    small_a = acc_a.resize((W, H), Image.BOX)
    out = Image.new("RGBA", (W, H))
    px, pa, po = small_rgb.load(), small_a.load(), out.load()
    # single colour regions: restore the exact colour of the nearest layer where covered
    colours = [rgb for _, rgb in layers]
    for y in range(H):
        for x in range(W):
            a = pa[x, y]
            if a == 0:
                po[x, y] = (0, 0, 0, 0)
                continue
            r, g, b = (min(255, round(c * 255 / a)) for c in px[x, y])
            best = min(colours, key=lambda c: (c[0] - r) ** 2 + (c[1] - g) ** 2 + (c[2] - b) ** 2)
            dist = (best[0] - r) ** 2 + (best[1] - g) ** 2 + (best[2] - b) ** 2
            po[x, y] = (best + (a,)) if dist < 900 else (r, g, b, a)
    return out


def variant(shape, w, h, state):
    """High resolution layers of one status icon."""
    kind = {"ready_alert": "alert", "searching_alert": "alert", "standby_alert": "alert",
            "ready_low": "low", "error": "error"}.get(state)
    body = shape
    if state.startswith("standby"):
        body = ImageChops.subtract(shape, erode(shape, 1.5))
    colour = GREY if state == "off" else GREEN
    layers = []
    if kind:
        ring, disc, sym = badge(w, h, kind)
        body = ImageChops.subtract(body, ring)
        layers.append((body, colour))
        layers.append((ImageChops.subtract(disc, sym), BADGE[kind]))
    else:
        layers.append((body, colour))
    return layers


def save_png(layers, w, h, name):
    for scale, suffix in ((1, ""), (2, "@2x")):
        compose(layers, w, h, scale).save(os.path.join(OUT, f"{name}{suffix}.png"), optimize=True)


def save_gif(layers, w, h, name):
    """Searching, as SteamVR's own GIFs: 29 frames fading the whole icon linearly down to about
    25 % and back (the darkest frame is shown once), 120 / 80 ms alternately. GIFs have no
    partial transparency: antialiased edges are blended over SteamVR's dark background
    (37, 40, 40) and kept opaque, like SteamVR's. One palette for all frames, with a fixed
    transparent index (a per frame palette moved the transparent colour between frames)."""
    bg = (37, 40, 40)
    key = (255, 0, 255)
    fades = [1.0 - 0.75 * (min(i, 28 - i) / 14.0) for i in range(29)]
    durations = [120 if i % 2 == 0 else 80 for i in range(29)]
    durations[-1] = 80
    for scale, suffix in ((1, ""), (2, "@2x")):
        base = compose(layers, w, h, scale)
        src = base.load()
        rgb_frames = []
        for k in fades:
            f = Image.new("RGB", base.size, key)
            dst = f.load()
            for y in range(base.height):
                for x in range(base.width):
                    r, g, b, a = src[x, y]
                    if a < 64:
                        continue
                    t = k * a / 255.0
                    dst[x, y] = tuple(round(c0 + (c - c0) * t) for c, c0 in zip((r, g, b), bg))
            rgb_frames.append(f)
        # one palette from all frames stacked
        strip = Image.new("RGB", (base.width, base.height * len(rgb_frames)))
        for i, f in enumerate(rgb_frames):
            strip.paste(f, (0, i * base.height))
        pal_img = strip.quantize(colors=64, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
        pal = pal_img.getpalette()
        tidx = min(range(len(pal) // 3),
                   key=lambda j: sum((pal[j * 3 + c] - key[c]) ** 2 for c in range(3)))
        frames = []
        for f in rgb_frames:
            q = f.quantize(palette=pal_img, dither=Image.Dither.NONE)
            fa, qa = f.load(), q.load()
            for y in range(f.height):
                for x in range(f.width):
                    if fa[x, y] == key:
                        qa[x, y] = tidx
            frames.append(q)
        frames[0].save(os.path.join(OUT, f"{name}{suffix}.gif"), save_all=True, append_images=frames[1:],
                       duration=durations, loop=0, transparency=tidx, disposal=2, optimize=False)


STATES = ["off", "ready", "ready_alert", "ready_low", "standby", "standby_alert", "error"]
GIF_STATES = ["searching", "searching_alert"]


def main():
    os.makedirs(OUT, exist_ok=True)
    for name, fn, w, h in (("psvr_status", psvr_shape, 50, 32), ("psmove_status", psmove_shape, 32, 32)):
        shape = fn(w, h)
        for st in STATES:
            save_png(variant(shape, w, h, st), w, h, f"{name}_{st}")
        for st in GIF_STATES:
            base_state = "ready_alert" if st.endswith("alert") else "ready"
            save_gif(variant(shape, w, h, base_state), w, h, f"{name}_{st}")
    print("icons written to", OUT, len(os.listdir(OUT)), "files")


if __name__ == "__main__":
    main()
