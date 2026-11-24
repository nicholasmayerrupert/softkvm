#!/usr/bin/env python3
"""Generates the SoftKVM app icon: assets/icon-1024.png, assets/softkvm.ico (Windows),
and assets/AppIcon.iconset/ (turned into AppIcon.icns with `iconutil` on a Mac)."""
import os
from PIL import Image, ImageDraw, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
S = 1024


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(len(a)))


def draw(size=S):
    k = size / S
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    # Rounded-square background with a vertical blue gradient (macOS-style margins).
    m, r = int(100 * k), int(185 * k)
    grad = Image.new("RGBA", (size, size))
    gd = ImageDraw.Draw(grad)
    top, bottom = (59, 130, 246, 255), (29, 64, 175, 255)
    for y in range(size):
        gd.line([(0, y), (size, y)], fill=lerp(top, bottom, y / size))
    mask = Image.new("L", (size, size), 0)
    ImageDraw.Draw(mask).rounded_rectangle([m, m, size - m, size - m], r, fill=255)
    shadow = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    ImageDraw.Draw(shadow).rounded_rectangle([m, m + int(14 * k), size - m, size - m + int(14 * k)], r,
                                            fill=(0, 0, 0, 90))
    img.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(int(18 * k) or 1)))
    img.paste(grad, (0, 0), mask)

    def monitor(layer, x, y, w, h, screen=None):
        d = ImageDraw.Draw(layer)
        white = (255, 255, 255, 255)
        t = int(34 * k)
        if screen:  # fill the screen so this monitor hides whatever is behind it
            d.rounded_rectangle([x, y, x + w, y + h], int(30 * k), fill=screen)
        d.rounded_rectangle([x, y, x + w, y + h], int(30 * k), outline=white, width=t)
        sx = x + w // 2
        d.rectangle([sx - int(22 * k), y + h, sx + int(22 * k), y + h + int(60 * k)], fill=white)
        d.rounded_rectangle([sx - int(110 * k), y + h + int(52 * k), sx + int(110 * k), y + h + int(86 * k)],
                            int(16 * k), fill=white)

    # The monitor behind is faded; the one in front is solid and covers it.
    back = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    monitor(back, int(215 * k), int(250 * k), int(420 * k), int(290 * k))
    back.putalpha(back.getchannel("A").point(lambda a: a * 55 // 100))
    img.alpha_composite(back)
    front = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    monitor(front, int(390 * k), int(390 * k), int(420 * k), int(290 * k), screen=(52, 114, 232, 255))
    img.alpha_composite(front)
    return img


def main():
    big = draw(S)
    big.save(os.path.join(HERE, "icon-1024.png"))
    # Windows .ico (render each size directly so small sizes stay crisp)
    sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256]
    frames = [draw(s) if s >= 64 else draw(256).resize((s, s), Image.LANCZOS) for s in sizes]
    frames[-1].save(os.path.join(HERE, "softkvm.ico"), sizes=[(s, s) for s in sizes], append_images=frames[:-1])
    # macOS iconset
    iconset = os.path.join(HERE, "AppIcon.iconset")
    os.makedirs(iconset, exist_ok=True)
    for base in [16, 32, 128, 256, 512]:
        for scale in (1, 2):
            px = base * scale
            im = big.resize((px, px), Image.LANCZOS)
            im.save(os.path.join(iconset, f"icon_{base}x{base}{'@2x' if scale == 2 else ''}.png"))
    print("icons written to", HERE)


if __name__ == "__main__":
    main()
