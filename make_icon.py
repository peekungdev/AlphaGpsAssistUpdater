# Draws src/app.ico: a location pin with signal arcs on a blue tile.
# Rendered large and downsampled per size so the small sizes stay crisp.
import math
import os
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
TILE = (14, 99, 186, 255)
WHITE = (255, 255, 255, 255)
AMBER = (255, 200, 40, 255)


def render(px):
    s = 1024
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle((32, 32, s - 32, s - 32), radius=200, fill=TILE)

    cx, cy, r, tip = 430, 470, 215, 900
    # Signal arcs in the upper right of the pin head
    for rad in (310, 415):
        d.arc((cx - rad, cy - rad, cx + rad, cy + rad), start=-80, end=-10, fill=AMBER, width=60)
    # Pin: head circle plus a tail that meets it at the tangent points
    dist = tip - cy
    a = math.acos(r / dist)
    left = (cx - r * math.sin(a), cy + r * math.cos(a))
    right = (cx + r * math.sin(a), cy + r * math.cos(a))
    d.polygon([left, (cx, tip), right], fill=WHITE)
    d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=WHITE)
    hole = 88
    d.ellipse((cx - hole, cy - hole, cx + hole, cy + hole), fill=TILE)
    return img.resize((px, px), Image.LANCZOS)


sizes = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]
imgs = [render(p) for p in sizes]
imgs[-1].save(os.path.join(HERE, "src", "app.ico"), sizes=[(p, p) for p in sizes], append_images=imgs[:-1])
imgs[-1].save(os.path.join(HERE, "docs", "icon-256.png"))
print("ok")
