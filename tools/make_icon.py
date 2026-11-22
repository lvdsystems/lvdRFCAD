"""Builds the lvd RF cad application icon from the red chip mark of the LVD Systems logo."""
import os
import shutil
import sys

from PIL import Image, ImageDraw

out = sys.argv[1]
logo = sys.argv[2]
os.makedirs(out, exist_ok=True)

RED = (187, 27, 30, 255)
DARK = (140, 18, 22, 255)
WHITE = (250, 252, 252, 255)

S = 1024  # drawing size, downsampled for anti-aliasing


def draw(size):
    big = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(big)
    m = S * 0.04
    d.rounded_rectangle((m, m, S - m, S - m), radius=S * 0.16, fill=DARK)
    d.rounded_rectangle((m, m, S - m, S - m - S * 0.025), radius=S * 0.16, fill=RED)
    # staggered white bars as in the logo mark (5 columns on a 30-unit grid)
    u = (S - 2 * m) / 30.0
    bars = [  # (column x, y0, y1) in grid units
        (3, 5, 16), (3, 20, 25),
        (8.5, 11, 22),
        (14, 3, 8), (14, 13, 27),
        (19.5, 8, 19),
        (25, 4, 15), (25, 19, 25),
    ]
    w = 3.2
    for x, y0, y1 in bars:
        d.rounded_rectangle((m + x * u, m + y0 * u, m + (x + w) * u, m + y1 * u), radius=u * 0.8, fill=WHITE)
    return big.resize((size, size), Image.LANCZOS)


sizes = [16, 24, 32, 48, 64, 128, 256]
images = {s: draw(s) for s in sizes}
for s, img in images.items():
    img.save(os.path.join(out, f"lvdrfcad-{s}.png"))
images[256].save(os.path.join(out, "lvdrfcad.ico"), sizes=[(s, s) for s in sizes])
shutil.copyfile(logo, os.path.join(out, "lvd-systems-logo.png"))
print("ok")
