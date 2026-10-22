# The start splash (500x268, 24-bit BMP, drawn at 2x and scaled down): background.jpg (a picture of the Chornobyl NPP
# and its sarcophagus, kept at the 2x size only), the S.T.A.L.K.E.R. Anomaly logo bottom left, a worn white neXt-Ray
# bottom right over "x64 / DX12" (or DX11), an optional version before it. The window's corners are rounded by the
# engine (SetWindowRgn in x_ray.cpp); the README's docs/splash.png gets them as transparent corners. Fonts: macOS's
# DIN Condensed / DIN Alternate.
#   python3 tools/splash/make_splash.py                     both exes' splashes into src/xrEngine, the DX12 one to docs
#   python3 tools/splash/make_splash.py --version 1.0       with "v1.0  /  x64  /  DX12"
#   python3 tools/splash/make_splash.py --api DX12 --out x.bmp
import argparse, math, os, random
from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.join(HERE, "..", "..", "src", "xrEngine")
DOC = os.path.join(HERE, "..", "..", "docs", "splash.png")
OUTPUTS = {"DX12": "splash_xrcs_en.bmp", "DX11": "splash_xrcs_dx11.bmp"}
W, H, K = 500, 268, 2
FS = "/System/Library/Fonts/Supplemental/"
DIN = FS + "DIN Condensed Bold.ttf"
DINA = FS + "DIN Alternate Bold.ttf"
WEAR = 0.85  # the wordmark's grit: holes, chips and scratches


def font(name, size):
    return ImageFont.truetype(name, int(size * K))


def background(shift_y=0.35):
    bg = Image.open(os.path.join(HERE, "background.jpg")).convert("RGB")
    w = bg.width
    h = int(round(w * H / W))
    y0 = int((bg.height - h) * shift_y)
    return bg.crop((0, y0, w, y0 + h)).resize((W * K, H * K), Image.LANCZOS)


def paste_logo(img, x, y, width, shadow=0.85):
    lg = Image.open(os.path.join(HERE, "anomaly_logo.png")).convert("LA")
    lg = lg.resize((int(width * K), int(lg.height * width * K / lg.width)), Image.LANCZOS)
    rgba = Image.merge("RGBA", [lg.getchannel("L")] * 3 + [lg.getchannel("A")])
    a = rgba.getchannel("A")
    sh = Image.new("RGBA", rgba.size, (0, 0, 0, 0))
    sh.putalpha(a.point(lambda v: int(v * shadow)).filter(ImageFilter.GaussianBlur(3 * K)))
    img.alpha_composite(sh, (int(x * K + 2 * K), int(y * K + 2 * K)))
    img.alpha_composite(rgba, (int(x * K), int(y * K)))


def shade(img, box, alpha, blur=40):
    # A soft dark patch behind the logo: box in 1x units
    m = Image.new("L", img.size, 0)
    ImageDraw.Draw(m).rounded_rectangle([v * K for v in box], radius=20 * K, fill=alpha)
    dark = Image.new("RGBA", img.size, (0, 0, 0, 255))
    dark.putalpha(m.filter(ImageFilter.GaussianBlur(blur * K)))
    img.alpha_composite(dark)


def gradient(img, top_alpha, bottom_alpha, start):
    g = Image.linear_gradient("L").resize(img.size)
    g = g.point(lambda v: int(top_alpha + (bottom_alpha - top_alpha) * (v / 255 - start) / (1 - start))
                if v / 255 >= start else top_alpha)
    dark = Image.new("RGBA", img.size, (0, 0, 0, 255))
    dark.putalpha(g)
    img.alpha_composite(dark)


def spaced(d, x, y, text, size, fill=(200, 196, 184), spacing=3):
    # Letter-spaced, right-aligned at x
    f = font(DINA, size)
    ws = [d.textlength(ch, font=f) for ch in text]
    cx = x * K - (sum(ws) + spacing * K * (len(text) - 1))
    for ch, w in zip(text, ws):
        d.text((cx + K, y * K + K), ch, font=f, fill=(0, 0, 0, 220), anchor="ls")
        d.text((cx, y * K), ch, font=f, fill=fill, anchor="ls")
        cx += w + spacing * K


def noise(scale, seed):
    random.seed(seed)
    n = Image.effect_noise((max(2, W * K // scale), max(2, H * K // scale)), 110)
    return n.resize((W * K, H * K), Image.BICUBIC)


def worn(mask, wear, seed=3):
    # Small holes and edge chips scattered over the letters, a few scratches
    rnd = random.Random(seed)
    bbox = mask.getbbox()
    holes = Image.new("L", mask.size, 255)
    hd = ImageDraw.Draw(holes)
    area = (bbox[2] - bbox[0]) * (bbox[3] - bbox[1])
    for _ in range(int(area / (K * K) * (0.006 + 0.022 * wear))):
        x = rnd.uniform(bbox[0], bbox[2])
        y = rnd.uniform(bbox[1], bbox[3])
        r = rnd.choice([0.4, 0.5, 0.6, 0.8, 1.0 + 0.8 * wear]) * K
        rx = r * rnd.uniform(0.7, 1.4)
        hd.ellipse([x - rx, y - r, x + rx, y + r], fill=0)
    for _ in range(int(1 + 4 * wear)):
        x = rnd.uniform(bbox[0], bbox[2])
        y = rnd.uniform(bbox[1], bbox[3])
        l = rnd.uniform(4, 14) * K
        ang = rnd.uniform(-0.6, 0.6)
        hd.line([x, y, x + l * math.cos(ang), y + l * math.sin(ang)], fill=0, width=max(1, int(0.6 * K)))
    return ImageChops.multiply(mask, holes).filter(ImageFilter.GaussianBlur(0.3 * K))


def mottled_white(strength):
    n = ImageChops.add(noise(4, 11), noise(30, 12), scale=2.0)
    return n.point(lambda v: int(255 - strength * (255 - v) / 255 * 60))


def render(api, version):
    img = background().convert("RGBA")
    gradient(img, 0, 170, 0.45)
    shade(img, (0, 110, 240, 268), 120)
    paste_logo(img, 18, 128, 200)
    m = Image.new("L", (W * K, H * K), 0)
    ImageDraw.Draw(m).text((482 * K, 229 * K), "neXt-Ray", font=font(DIN, 40), fill=255, anchor="rs")
    dark = Image.new("RGBA", img.size, (0, 0, 0, 255))
    dark.putalpha(m.filter(ImageFilter.GaussianBlur(1.5 * K)).point(lambda v: int(v * 0.85)))
    img.alpha_composite(dark, (int(1.5 * K), int(1.5 * K)))
    fill = mottled_white(0.5 + WEAR * 0.5)
    img.alpha_composite(Image.merge("RGBA", [fill, fill, fill, worn(m, WEAR)]))
    line = f"x64  /  {api}"
    if version:
        line = f"v{version.lstrip('vV')}  /  " + line
    spaced(ImageDraw.Draw(img), 482, 254, line, 11)
    return img.convert("RGB").resize((W, H), Image.LANCZOS)


def rounded(im, radius=12):
    # The window's region (CreateRoundRectRgn, 24 px ellipses), antialiased: transparent corners
    m = Image.new("L", (W * 4, H * 4), 0)
    ImageDraw.Draw(m).rounded_rectangle([0, 0, W * 4 - 1, H * 4 - 1], radius=radius * 4, fill=255)
    out = im.convert("RGBA")
    out.putalpha(m.resize((W, H), Image.LANCZOS))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--api", choices=sorted(OUTPUTS), help="one exe's splash (default: both)")
    ap.add_argument("--version", help="printed before x64, e.g. 1.0")
    ap.add_argument("--out", help="the BMP to write (with --api; default: the engine's for that exe)")
    a = ap.parse_args()
    for api in [a.api] if a.api else sorted(OUTPUTS):
        out = a.out if a.out and a.api else os.path.join(ENGINE, OUTPUTS[api])
        img = render(api, a.version)
        img.save(out)
        print(os.path.normpath(out))
        if api == "DX12" and not a.out:
            rounded(img).save(DOC)
            print(os.path.normpath(DOC))


if __name__ == "__main__":
    main()
