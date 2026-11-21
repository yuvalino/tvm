#!/usr/bin/env python3
"""App Store marketing screenshots: rsc/<device>-<screen>.png inside a drawn
device frame under a headline, on one background image that runs across
all the screenshots (rsc/appstore-background.png).

python3 scripts/appstore_shots.py  ->  rsc/appstore/<size>-<n>-<screen>.png
                                     rsc/frames/iphone-<n>-<screen>.png

The frames are the iPhone alone, at the shot's own resolution, with a
transparent margin around it and no background or headline.
"""
import os
from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RSC = os.path.join(ROOT, "rsc")
OUT = os.path.join(RSC, "appstore")
FRAMES = os.path.join(RSC, "frames")
FRAME_MARGIN = 0.08  # the frames' transparent margin, of the device's width
FONTS = os.path.join(ROOT, "flutter", "assets", "fonts")

# tvm_colors.dart
BG = (0x1F, 0x1F, 0x1E)
BRIGHT = (0xF2, 0xEC, 0xE0)

BACKGROUND = os.path.join(RSC, "appstore-background.jpg")
REACH = 1.35       # where the fade reaches black, in half-widths/heights from the centre
BRIGHTNESS = 0.55  # the background's brightness at the centre
# the neofetch apple on the MacBook in the photo (centre, as fractions of the
# photo): placed in the last screenshot's left margin, beside the device
LOGO = (0.753, 0.704)
LOGO_Y = 0.70      # its height in the screenshots, of the height
# the device's width, of the screenshot's; the same margin at the sides and bottom
DEVICE_W = {"iphone": 0.70, "ipad": 0.66}
BEZEL = {"iphone": 0.032, "ipad": 0.030}

# (screen, headline), in App Store order; "tvm" is the log tab
SCREENS = [
    ("vim", "your pocket shell"),
    ("neofetch", "on device SSH server"),
    ("tvm", "access reinvented"),
]
# the headline: "// " and the title, JetBrains Mono Bold, the same size in
# every screenshot, at most TITLE_W of the width
PREFIX = "// "
TITLE_PX = 92      # at 1320 wide (iPhone) / 1600 (iPad)
TITLE_W = 0.88

# App Store Connect sizes (portrait) -> the device whose shots go in them
SIZES = {
    "iphone-6.9": ("iphone", 1320, 2868),
    "iphone-6.1": ("iphone", 1179, 2556),
    "ipad-13": ("ipad", 2064, 2752),
}

SS = 4  # supersampling for the frame's shapes

# The tvm tab's network line (tvm_tab.dart: JetBrains Mono 12, TvmColors.dim)
# redrawn with another hostname: the shots were taken on the simulator, which
# shows the Mac's. The size and origin are fitted to the shots' own text.
NETWORK = "192.168.68.135 · {}"
HOST = {"iphone": "pan1c.local", "ipad": "ab0rt.local"}
NETWORK_TEXT = {  # (font px, x, y, the line's ink box to clear)
    "iphone": (37.25, 222.25, 289, (220, 296, 962, 338)),
    "ipad": (24.75, 148.75, 122, (146, 127, 642, 156)),
}
SCREEN_BG = (0x1F, 0x1F, 0x1E)
DIM = (0x8A, 0x83, 0x76)


def rename_host(shot, kind):
    shot = shot.convert("RGB")
    px, x, y, box = NETWORK_TEXT[kind]
    d = ImageDraw.Draw(shot)
    d.rectangle(box, fill=SCREEN_BG)
    d.text((x, y), NETWORK.format(HOST[kind]), font=font("Regular", px), fill=DIM)
    return shot


def font(weight, size):
    return ImageFont.truetype(os.path.join(FONTS, f"JetBrainsMono-{weight}.ttf"), size)


def rounded_mask(size, box, radius):
    """An antialiased mask of a rounded rectangle."""
    w, h = size
    m = Image.new("L", (w * SS, h * SS), 0)
    x0, y0, x1, y1 = box
    ImageDraw.Draw(m).rounded_rectangle(
        (x0 * SS, y0 * SS, x1 * SS - 1, y1 * SS - 1), radius * SS, fill=255)
    return m.resize(size, Image.LANCZOS)


def panorama(w, h, panels, tx, ty):
    """rsc/appstore-background.jpg across `panels` screenshots side by side,
    scaled and placed so its LOGO lands at (tx, ty) of the strip, at the
    smallest scale that still covers it; dimmed and faded to black from the
    centre out."""
    W = w * panels
    src = Image.open(BACKGROUND).convert("RGB")
    lx, ly = LOGO
    aspect = src.height / src.width
    iw = max(W, h / aspect, tx / lx, (W - tx) / (1 - lx),
             ty / ly / aspect, (h - ty) / (1 - ly) / aspect)
    ih = iw * aspect
    ox, oy = round(tx - lx * iw), round(ty - ly * ih)
    src = src.resize((round(iw), round(ih)), Image.LANCZOS).crop((-ox, -oy, -ox + W, -oy + h))
    ph = h
    # the fade: elliptical, from the centre out, black past REACH of the
    # half-width / half-height (so the corners), never a rectangular frame
    sw, sh = max(1, W // 16), max(1, h // 16)  # smooth: computed small, scaled up
    small = Image.new("L", (sw, sh))
    small.putdata([
        round(255 * BRIGHTNESS * (1 - (lambda t: t * t * (3 - 2 * t))(min(1.0, (
            ((x + 0.5) / sw * 2 - 1) ** 2 + ((y + 0.5) / sh * 2 - 1) ** 2) ** 0.5 / REACH))))
        for y in range(sh) for x in range(sw)])
    mask = small.resize((W, h), Image.BICUBIC)
    img = Image.new("RGB", (W, h), (0, 0, 0))
    img.paste(src, (0, 0), mask)
    return img


def title_size(w, scale):
    """TITLE_PX, shrunk until every headline fits TITLE_W of the width."""
    size = int(TITLE_PX * scale)
    while max(font("Bold", size).getlength(PREFIX + t) for _, t in SCREENS) > w * TITLE_W:
        size -= 1
    return size


def headline(img, title, centre, size):
    """PREFIX and the title, all bright, centred on y = centre."""
    d = ImageDraw.Draw(img)
    f = font("Bold", size)
    title = PREFIX + title
    x0, y0, x1, y1 = d.textbbox((0, 0), title, font=f)
    x, top = (img.width - (x1 - x0)) / 2 - x0, centre - (y0 + y1) / 2
    shadow = Image.new("L", img.size, 0)
    ImageDraw.Draw(shadow).text((x, top), title, font=f, fill=255)
    shadow = shadow.filter(ImageFilter.MaxFilter(9)).filter(ImageFilter.GaussianBlur(size / 4))
    img.paste((0, 0, 0), (0, 0), shadow.point(lambda v: v * 0.8))
    d.text((x, top), title, font=f, fill=BRIGHT)


def device(img, shot, kind, box):
    """The screenshot in a device frame, filling box = (x, y, width)."""
    x, y, dw = box
    if kind == "iphone":
        bezel = dw * BEZEL["iphone"]
        pt = (dw - 2 * bezel) / 393          # iPhone 16: 393 pt wide
        screen_r = 55 * pt
    else:
        bezel = dw * BEZEL["ipad"]
        pt = (dw - 2 * bezel) / 1032         # iPad Pro 13": 1032 pt wide
        screen_r = 18 * pt
    sw = round(dw - 2 * bezel)
    sh = round(sw * shot.height / shot.width)
    dh = sh + 2 * bezel
    x, y, bezel, dw, dh = round(x), round(y), round(bezel), round(dw), round(dh)

    # soft shadow
    shadow = Image.new("L", img.size, 0)
    ImageDraw.Draw(shadow).rounded_rectangle(
        (x, y + dw * 0.02, x + dw, y + dh + dw * 0.02), screen_r + bezel, fill=150)
    shadow = shadow.filter(ImageFilter.GaussianBlur(dw * 0.03))
    img.paste((0, 0, 0), (0, 0), shadow)

    # side buttons (iPhone: action + volume left, power right)
    d = ImageDraw.Draw(img)
    if kind == "iphone":
        bw = max(4, round(dw * 0.008))
        for side, ys in ((-1, [(0.17, 0.21), (0.25, 0.31), (0.33, 0.39)]), (1, [(0.27, 0.36)])):
            for a, b in ys:
                bx = x - bw if side < 0 else x + dw
                d.rounded_rectangle((bx, y + dh * a, bx + bw, y + dh * b), bw // 2, fill=(0x3A, 0x38, 0x34))

    # body: an edge ring, then the black bezel
    ring = max(3, round(dw * 0.004))
    outer_r = screen_r + bezel
    img.paste((0x4A, 0x46, 0x40), (0, 0), rounded_mask(img.size, (x, y, x + dw, y + dh), outer_r))
    img.paste((0x0A, 0x0A, 0x0A), (0, 0),
              rounded_mask(img.size, (x + ring, y + ring, x + dw - ring, y + dh - ring), outer_r - ring))

    # screen
    sx, sy = x + bezel, y + bezel
    screen = shot.convert("RGB").resize((sw, sh), Image.LANCZOS)
    mask = rounded_mask((sw, sh), (0, 0, sw, sh), screen_r)
    img.paste(screen, (sx, sy), mask)

    # Dynamic Island (iPhone 16: 126 x 37 pt, 11 pt from the top)
    if kind == "iphone":
        iw, ih, it = 126 * pt, 37 * pt, 11 * pt
        ix = sx + (sw - iw) / 2
        isl = rounded_mask(img.size, (round(ix), round(sy + it), round(ix + iw), round(sy + it + ih)), ih / 2)
        img.paste((0, 0, 0), (0, 0), isl)


def layout(kind, w, h, shot):
    """The device's (x, y, width): DEVICE_W of the width, with the side margin
    below it too."""
    dw = w * DEVICE_W[kind]
    m = (w - dw) / 2
    b = BEZEL[kind]
    dh = dw * ((1 - 2 * b) * shot.height / shot.width + 2 * b)
    return m, h - m - dh, dw


def compose(size_name, n, screen, title, back):
    kind, w, h = SIZES[size_name]
    shot = Image.open(os.path.join(RSC, f"{kind}-{screen}.png"))
    if screen == "tvm":
        shot = rename_host(shot, kind)
    img = back.crop(((n - 1) * w, 0, n * w, h))
    scale = w / 1320 if kind == "iphone" else w / 1600
    x, y, dw = layout(kind, w, h, shot)
    headline(img, title, y / 2, title_size(w, scale))
    device(img, shot, kind, (x, y, dw))
    os.makedirs(OUT, exist_ok=True)
    path = os.path.join(OUT, f"{size_name}-{n}-{screen}.png")
    img.convert("RGB").save(path, optimize=True)
    print("wrote", os.path.relpath(path, ROOT), img.size, img.mode)


def frame(n, screen, kind="iphone"):
    """The device alone on transparency, its screen at the shot's resolution."""
    shot = Image.open(os.path.join(RSC, f"{kind}-{screen}.png"))
    if screen == "tvm":
        shot = rename_host(shot, kind)
    b = BEZEL[kind]
    dw = shot.width / (1 - 2 * b)
    dh = dw * ((1 - 2 * b) * shot.height / shot.width + 2 * b)
    m = round(dw * FRAME_MARGIN)
    img = Image.new("RGBA", (round(dw) + 2 * m, round(dh) + 2 * m), (0, 0, 0, 0))
    device(img, shot, kind, (m, m, dw))
    os.makedirs(FRAMES, exist_ok=True)
    path = os.path.join(FRAMES, f"{kind}-{n}-{screen}.png")
    img.save(path, optimize=True)
    print("wrote", os.path.relpath(path, ROOT), img.size, img.mode)


if __name__ == "__main__":
    for n, (screen, _) in enumerate(SCREENS, 1):
        frame(n, screen)
    for size_name, (kind, w, h) in SIZES.items():
        # the logo: in the last screenshot's left margin
        m = layout(kind, w, h, Image.open(os.path.join(RSC, f"{kind}-{SCREENS[-1][0]}.png")))[0]
        back = panorama(w, h, len(SCREENS), (len(SCREENS) - 1) * w + m / 2, h * LOGO_Y)
        for n, (screen, title) in enumerate(SCREENS, 1):
            compose(size_name, n, screen, title, back)
