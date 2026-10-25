"""PitchRide's installer pictures (WiX): dialog.bmp 493x312 (left panel
# Run from anywhere: python3 packaging/wix/images/pitchride_art.py <b2b-desktop folder>
# (needs cairosvg and Pillow).
164 px) and banner.bmp 493x58. WiX writes its own black text on the white
parts, so those stay white."""
import io, sys
import cairosvg
from PIL import Image, ImageDraw, ImageFont

ROOT = sys.argv[1]
BRAND = f"{ROOT}/res/b2b/brand"
FONTS = f"{ROOT}/res/fonts"
OUT = f"{ROOT}/packaging/wix/images"

def svg(name, px):
    png = cairosvg.svg2png(url=f"{BRAND}/{name}", output_width=px, output_height=px)
    return Image.open(io.BytesIO(png)).convert("RGBA")

def grad(w, h, stops):
    im = Image.new("RGB", (w, h))
    px = im.load()
    for y in range(h):
        t = y / (h - 1)
        for i in range(len(stops) - 1):
            (t0, c0), (t1, c1) = stops[i], stops[i + 1]
            if t0 <= t <= t1:
                f = (t - t0) / (t1 - t0)
                c = tuple(round(a + (b - a) * f) for a, b in zip(c0, c1))
                break
        for x in range(w):
            px[x, y] = c
    return im

hex_ = lambda h: tuple(int(h[i:i + 2], 16) for i in (1, 3, 5))
S = 4  # draw big, then shrink: clean edges

# Dialog (welcome / finish pages): the blue panel on the left.
W, H, P = 493, 312, 164
d = Image.new("RGB", (W * S, H * S), "white")
panel = grad(P * S, H * S, [(0, hex_("#1B35A6")), (0.45, hex_("#112283")), (1, hex_("#0A1555"))])
d.paste(panel, (0, 0))
logo = svg("logo.svg", 112 * S)
d.paste(logo, ((P * S - logo.width) // 2, 62 * S), logo)
draw = ImageDraw.Draw(d)
word = ImageFont.truetype(f"{FONTS}/HankenGrotesk-ExtraBold.ttf", 25 * S)
tw = draw.textlength("pitchride", font=word)
draw.text(((P * S - tw) / 2, 196 * S), "pitchride", font=word, fill=hex_("#F7D6DC"))
mono = ImageFont.truetype(f"{FONTS}/DMMono-Regular.ttf", 8 * S)
tag = "BACK TO BACK, ONLINE"
draw.text(((P * S - draw.textlength(tag, font=mono)) / 2, 234 * S), tag, font=mono, fill=hex_("#AAB7F2"))
draw.line([(P * S - S, 0), (P * S - S, H * S)], fill=hex_("#E8483C"), width=S)  # the keyline
d.resize((W, H), Image.LANCZOS).save(f"{OUT}/dialog.bmp")

# Banner (the other pages' top strip): WiX's title on the left, ours on the right.
W, H = 493, 58
b = Image.new("RGB", (W * S, H * S), "white")
walls = svg("logo.svg", 40 * S)
draw = ImageDraw.Draw(b)
word = ImageFont.truetype(f"{FONTS}/HankenGrotesk-ExtraBold.ttf", 20 * S)
tw = draw.textlength("pitchride", font=word)
x = W * S - 12 * S - tw
draw.text((x, 15 * S), "pitchride", font=word, fill=hex_("#112283"))
b.paste(walls, (int(x - 8 * S - walls.width), 9 * S), walls)
draw.line([(0, H * S - S), (W * S, H * S - S)], fill=hex_("#E8483C"), width=S)
b.resize((W, H), Image.LANCZOS).save(f"{OUT}/banner.bmp")
print("ok")
