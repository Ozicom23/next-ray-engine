# Gamepad button glyphs and the controller pictures of Options > Gamepad > Buttons overview, as DDS (32-bit BGRA with
# mipmaps) into gamedata/textures/ui:
#   ui_gamepad_glyphs.dds   512x512, cells of 64: Xbox in cells 0-17 (the buttons in GP_A order: A B X Y LB RB LT RT
#                           View Menu L3 R3 D-pad up down left right, then the left and right stick), PlayStation
#                           from cell 32 (rows 4-6). The font draws them inline in text (xrEngine/gamepad_glyphs.h)
#   ui_gamepad_fill.dds     8x8 white: the overview's background, tinted by its xml
#   ui_gamepad_xbox.dds, ui_gamepad_ps.dds   1024x512: the controllers, seen from above-front; the button centres
#                           are in CONTROLLER and CONTROLLER_PS (ui_gamepad_overview.script has them too)
# Fonts: macOS's DIN Alternate Bold. python3 tools/gamepad/make_glyphs.py
import math, os, struct
from PIL import Image, ImageDraw, ImageFont, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "..", "gamedata", "textures", "ui")
DINA = "/System/Library/Fonts/Supplemental/DIN Alternate Bold.ttf"
SS = 4  # supersampling
CELL = 64

BODY = (34, 34, 36, 240)
RING = (200, 200, 200, 255)
WHITE = (238, 238, 238, 255)
DIM = (110, 110, 110, 255)
XBOX = {"A": (108, 192, 74), "B": (224, 74, 63), "X": (63, 143, 224), "Y": (240, 195, 48)}
PS = {"cross": (127, 168, 232), "circle": (232, 96, 106), "square": (224, 127, 192), "triangle": (79, 192, 160)}


def font(px):
    return ImageFont.truetype(DINA, int(px))


def cell():
    s = CELL * SS
    return Image.new("RGBA", (s, s), (0, 0, 0, 0)), s


def text_center(d, cx, cy, txt, px, fill):
    f = font(px)
    d.text((cx, cy), txt, font=f, fill=fill, anchor="mm")


def disc(d, s, ring=True):
    m = s * 0.06
    d.ellipse([m, m, s - m, s - m], fill=BODY, outline=RING if ring else None, width=int(s * 0.035))


def face_xbox(letter):
    im, s = cell()
    d = ImageDraw.Draw(im)
    disc(d, s)
    text_center(d, s / 2, s / 2 + s * 0.03, letter, s * 0.56, XBOX[letter] + (255,))
    return im


def face_ps(shape):
    im, s = cell()
    d = ImageDraw.Draw(im)
    disc(d, s)
    c = PS[shape] + (255,)
    w = int(s * 0.07)
    r = s * 0.22
    cx = cy = s / 2
    if shape == "cross":
        d.line([cx - r, cy - r, cx + r, cy + r], fill=c, width=w)
        d.line([cx - r, cy + r, cx + r, cy - r], fill=c, width=w)
    elif shape == "circle":
        d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=c, width=w)
    elif shape == "square":
        r *= 0.9
        d.rectangle([cx - r, cy - r, cx + r, cy + r], outline=c, width=w)
    else:
        r *= 1.1
        pts = [(cx, cy - r), (cx + r * 0.95, cy + r * 0.62), (cx - r * 0.95, cy + r * 0.62)]
        d.polygon(pts, outline=c, width=w)
    return im


def shoulder(label, trigger):
    im, s = cell()
    d = ImageDraw.Draw(im)
    m = s * 0.06
    if trigger:
        # A trigger: rounded top, square-ish bottom
        d.rounded_rectangle([m * 1.6, m, s - m * 1.6, s - m], radius=s * 0.28, fill=BODY, outline=RING, width=int(s * 0.035))
    else:
        d.rounded_rectangle([m, s * 0.2, s - m, s * 0.8], radius=s * 0.14, fill=BODY, outline=RING, width=int(s * 0.035))
    text_center(d, s / 2, s / 2 + s * 0.02, label, s * (0.36 if len(label) > 1 else 0.5), WHITE)
    return im


def small_button(kind):
    im, s = cell()
    d = ImageDraw.Draw(im)
    disc(d, s)
    cx = cy = s / 2
    w = int(s * 0.05)
    if kind == "view":  # two overlapping windows
        a = s * 0.15
        d.rectangle([cx - a - s * 0.06, cy - a + s * 0.06, cx + a - s * 0.06, cy + a + s * 0.06], outline=WHITE, width=w)
        d.rectangle([cx - a + s * 0.06, cy - a - s * 0.06, cx + a + s * 0.06, cy + a - s * 0.06], fill=BODY, outline=WHITE, width=w)
    elif kind == "menu" or kind == "options":  # three lines
        for k in (-1, 0, 1):
            y = cy + k * s * 0.13
            d.line([cx - s * 0.2, y, cx + s * 0.2, y], fill=WHITE, width=w)
    elif kind == "create":  # a share arrow out of a tray
        d.line([cx, cy + s * 0.12, cx, cy - s * 0.2], fill=WHITE, width=w)
        d.line([cx - s * 0.11, cy - s * 0.09, cx, cy - s * 0.2, cx + s * 0.11, cy - s * 0.09], fill=WHITE, width=w)
        d.line([cx - s * 0.2, cy, cx - s * 0.2, cy + s * 0.2, cx + s * 0.2, cy + s * 0.2, cx + s * 0.2, cy], fill=WHITE, width=w)
    return im


def stick(label, click):
    im, s = cell()
    d = ImageDraw.Draw(im)
    disc(d, s)
    cx = cy = s / 2
    r = s * 0.27
    d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=DIM, width=int(s * 0.03))
    if click:
        text_center(d, cx, cy + s * 0.02, label + "3", s * 0.32, WHITE)
    else:
        text_center(d, cx, cy + s * 0.02, label, s * 0.36, WHITE)
        # Four ticks: it moves
        t = s * 0.36
        for a in range(4):
            ang = a * math.pi / 2
            x, y = cx + math.cos(ang) * t, cy + math.sin(ang) * t
            k = s * 0.045
            px, py = -math.sin(ang), math.cos(ang)
            d.polygon([(x + math.cos(ang) * k, y + math.sin(ang) * k), (x + px * k, y + py * k), (x - px * k, y - py * k)],
                      fill=WHITE)
    return im


def dpad(arm):
    im, s = cell()
    d = ImageDraw.Draw(im)
    cx = cy = s / 2
    a, b = s * 0.15, s * 0.44
    arms = {"up": [cx - a, cy - b, cx + a, cy - a], "down": [cx - a, cy + a, cx + a, cy + b],
            "left": [cx - b, cy - a, cx - a, cy + a], "right": [cx + a, cy - a, cx + b, cy + a]}
    # The cross outlined, then the arms; the pressed one white
    o = s * 0.035
    d.rounded_rectangle([cx - a - o, cy - b - o, cx + a + o, cy + b + o], radius=s * 0.06, fill=RING)
    d.rounded_rectangle([cx - b - o, cy - a - o, cx + b + o, cy + a + o], radius=s * 0.06, fill=RING)
    d.rectangle([cx - a, cy - a, cx + a, cy + a], fill=BODY)
    for k, box in arms.items():
        d.rounded_rectangle(box, radius=s * 0.04, fill=WHITE if k == arm else BODY)
    return im


def glyph_set(playstation):
    if playstation:
        faces = [face_ps(k) for k in ("cross", "circle", "square", "triangle")]
        sh = [shoulder(l, t) for l, t in (("L1", False), ("R1", False), ("L2", True), ("R2", True))]
        mid = [small_button("create"), small_button("options")]
    else:
        faces = [face_xbox(k) for k in "ABXY"]
        sh = [shoulder(l, t) for l, t in (("LB", False), ("RB", False), ("LT", True), ("RT", True))]
        mid = [small_button("view"), small_button("menu")]
    clicks = [stick("L", True), stick("R", True)]
    pads = [dpad(k) for k in ("up", "down", "left", "right")]
    sticks = [stick("L", False), stick("R", False)]
    return faces + sh + mid + clicks + pads + sticks


def mips(im):
    out = [im]
    while min(out[-1].size) > 1:
        w, h = out[-1].size
        out.append(out[-1].resize((max(1, w // 2), max(1, h // 2)), Image.LANCZOS))
    return out


def save_dds(im, path):
    # Uncompressed 32-bit BGRA with every mip level (A8R8G8B8)
    levels = mips(im.convert("RGBA"))
    w, h = im.size
    flags = 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x8  # caps, height, width, pixelformat, mipmapcount, pitch
    header = struct.pack("<4sIIIIIII44x", b"DDS ", 124, flags, h, w, w * 4, 0, len(levels))
    pf = struct.pack("<II4sIIIII", 32, 0x41, b"\0\0\0\0", 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000)
    caps = struct.pack("<IIIII", 0x1000 | 0x8 | 0x400000, 0, 0, 0, 0)
    with open(path, "wb") as f:
        f.write(header + pf + caps)
        for lv in levels:
            r, g, b, a = lv.split()
            f.write(Image.merge("RGBA", (b, g, r, a)).tobytes())
    print(os.path.normpath(path))


def atlas():
    im = Image.new("RGBA", (512, 512), (0, 0, 0, 0))
    for base, ps in ((0, False), (32, True)):
        for i, g in enumerate(glyph_set(ps)):
            k = base + i
            g = g.resize((CELL, CELL), Image.LANCZOS)
            im.alpha_composite(g, ((k % 8) * CELL, (k // 8) * CELL))
    return im


# The controller pictures: 1024x512, the button centres in pixels (the overview's labels point at them)
CONTROLLER = {
    "LT": (300, 62), "RT": (724, 62), "LB": (300, 118), "RB": (724, 118),
    "BACK": (440, 205), "START": (584, 205),
    "LS": (330, 226), "Y": (700, 180), "X": (648, 228), "B": (752, 228), "A": (700, 276),
    "DPAD": (418, 318), "RS": (606, 318),
}
# A DualSense: the D-pad up left, both sticks below
CONTROLLER_PS = dict(CONTROLLER, LS=CONTROLLER["DPAD"], DPAD=CONTROLLER["LS"])


def controller(playstation):
    pos = CONTROLLER_PS if playstation else CONTROLLER
    W, H = 1024 * 2, 512 * 2
    im = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    k = 2
    body = (52, 52, 56, 255)
    edge = (95, 95, 100, 255)
    # The body: two grips and the bridge between them
    shape = Image.new("L", (W, H), 0)
    sd = ImageDraw.Draw(shape)
    sd.rounded_rectangle([240 * k, 140 * k, 784 * k, 330 * k], radius=80 * k, fill=255)
    sd.ellipse([222 * k, 160 * k, 442 * k, 452 * k], fill=255)
    sd.ellipse([582 * k, 160 * k, 802 * k, 452 * k], fill=255)
    shape = shape.filter(ImageFilter.GaussianBlur(6 * k)).point(lambda v: 255 if v > 128 else 0)
    shadow = Image.new("RGBA", (W, H), (0, 0, 0, 160))
    shadow.putalpha(shape.filter(ImageFilter.GaussianBlur(14 * k)).point(lambda v: int(v * 0.6)))
    im.alpha_composite(shadow, (0, 10 * k))
    fill = Image.new("RGBA", (W, H), body)
    fill.putalpha(shape)
    im.alpha_composite(fill)
    outline = shape.filter(ImageFilter.FIND_EDGES).filter(ImageFilter.MaxFilter(5))
    ol = Image.new("RGBA", (W, H), edge)
    ol.putalpha(outline)
    im.alpha_composite(ol)
    # Shoulders and triggers behind the top edge
    for name in ("LT", "RT"):
        x, y = pos[name]
        d.rounded_rectangle([(x - 62) * k, (y - 34) * k, (x + 62) * k, (y + 30) * k], radius=26 * k, fill=(44, 44, 48, 255), outline=edge, width=3 * k)
    for name in ("LB", "RB"):
        x, y = pos[name]
        d.rounded_rectangle([(x - 78) * k, (y - 16) * k, (x + 78) * k, (y + 16) * k], radius=14 * k, fill=(60, 60, 64, 255), outline=edge, width=3 * k)
    # Buttons from the glyphs, drawn larger
    g = glyph_set(playstation)
    def put(idx, name, size):
        x, y = pos[name]
        im.alpha_composite(g[idx].resize((size * k, size * k), Image.LANCZOS), ((x - size // 2) * k, (y - size // 2) * k))
    for i, name in enumerate(("A", "B", "X", "Y")):
        put(i, name, 58)
    put(8, "BACK", 40)
    put(9, "START", 40)
    put(16, "LS", 92)
    put(17, "RS", 92)
    # The D-pad: the cross alone
    x, y = pos["DPAD"]
    cross = dpad(None).resize((96 * k, 96 * k), Image.LANCZOS)
    im.alpha_composite(cross, ((x - 48) * k, (y - 48) * k))
    # Shoulder labels
    for i, name in ((4, "LB"), (5, "RB"), (6, "LT"), (7, "RT")):
        x, y = pos[name]
        lab = ("L1", "R1", "L2", "R2")[i - 4] if playstation else name
        text_center(d, x * k, (y + 2) * k, lab, 26 * k, WHITE)
    return im.resize((1024, 512), Image.LANCZOS)


def main():
    os.makedirs(OUT, exist_ok=True)
    save_dds(atlas(), os.path.join(OUT, "ui_gamepad_glyphs.dds"))
    save_dds(Image.new("RGBA", (8, 8), (255, 255, 255, 255)), os.path.join(OUT, "ui_gamepad_fill.dds"))
    save_dds(controller(False), os.path.join(OUT, "ui_gamepad_xbox.dds"))
    save_dds(controller(True), os.path.join(OUT, "ui_gamepad_ps.dds"))
    if os.environ.get("PREVIEW"):
        atlas().save(os.environ["PREVIEW"] + "_glyphs.png")
        controller(False).save(os.environ["PREVIEW"] + "_xbox.png")
        controller(True).save(os.environ["PREVIEW"] + "_ps.png")


if __name__ == "__main__":
    main()
