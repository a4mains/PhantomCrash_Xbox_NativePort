"""Builds the port's executable icon from original procedural geometry."""

from __future__ import annotations

import argparse
import math
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT = ROOT / "port" / "res"

SIZES = (16, 20, 24, 32, 40, 48, 64, 96, 128, 256)
MONOGRAM_ONLY_BELOW = 24
MASTER = 1024

TILE_TOP = (38, 92, 98)
TILE_BOTTOM = (10, 30, 34)
TILE_RADIUS = 0.18
ACCENT_CYAN = (108, 214, 218)
FRAME_STEEL = (206, 224, 228)
FRAME_SHADOW = (14, 24, 28)
VISOR_AMBER = (240, 164, 52)
VISOR_BRIGHT = (255, 214, 118)
TEXT_OUTLINE = (12, 16, 18)


def tile_mask(size: int) -> Image.Image:
    mask = Image.new("L", (size, size), 0)
    ImageDraw.Draw(mask).rounded_rectangle(
        [0, 0, size - 1, size - 1], radius=round(size * TILE_RADIUS), fill=255
    )
    return mask


def tile(size: int) -> Image.Image:
    gradient = Image.new("RGBA", (size, size))
    draw = ImageDraw.Draw(gradient)
    for y in range(size):
        t = y / max(1, size - 1)
        colour = tuple(round(a + (b - a) * t) for a, b in zip(TILE_TOP, TILE_BOTTOM))
        draw.line([(0, y), (size, y)], fill=colour + (255,))
    gradient.putalpha(tile_mask(size))
    return gradient


def clip_to_tile(image: Image.Image) -> Image.Image:
    image.putalpha(ImageChops.darker(image.getchannel("A"), tile_mask(image.width)))
    return image


def draw_monogram(
    draw: ImageDraw.ImageDraw,
    x: float,
    y: float,
    h: float,
    thickness: float,
    outline: float,
    fill: tuple[int, int, int],
) -> None:
    """Draws bold geometric block letters 'PC' with a dark outline."""
    w = h * 0.56
    gap = h * 0.14
    t = thickness

    def rect(x0: float, y0: float, x1: float, y1: float, col: tuple[int, int, int, int]) -> None:
        draw.rectangle([x0, y0, x1, y1], fill=col)

    def draw_glyphs(pad: float, col: tuple[int, int, int, int]) -> None:
        # Letter P
        px0, py0 = x, y
        rect(px0 - pad, py0 - pad, px0 + t + pad, py0 + h + pad, col)
        rect(px0 - pad, py0 - pad, px0 + w + pad, py0 + t + pad, col)
        rect(px0 + w - t - pad, py0 - pad, px0 + w + pad, py0 + h * 0.54 + pad, col)
        rect(px0 - pad, py0 + h * 0.54 - t - pad, px0 + w + pad, py0 + h * 0.54 + pad, col)
        # Letter C
        cx0 = px0 + w + gap
        rect(cx0 - pad, py0 - pad, cx0 + t + pad, py0 + h + pad, col)
        rect(cx0 - pad, py0 - pad, cx0 + w + pad, py0 + t + pad, col)
        rect(cx0 - pad, py0 + h - t - pad, cx0 + w + pad, py0 + h + pad, col)

    draw_glyphs(outline, TEXT_OUTLINE + (255,))
    draw_glyphs(0.0, fill + (255,))


def full_icon() -> Image.Image:
    s = MASTER
    image = tile(s)
    layer = Image.new("RGBA", (s, s))
    draw = ImageDraw.Draw(layer)

    # Subtle HUD targeting rings and crosshairs
    cx, cy = s * 0.50, s * 0.48
    for radius, width, alpha in ((s * 0.39, round(s * 0.012), 70), (s * 0.30, round(s * 0.008), 110)):
        draw.arc(
            [cx - radius, cy - radius, cx + radius, cy + radius],
            start=25,
            end=335,
            fill=ACCENT_CYAN + (alpha,),
            width=width,
        )
    for angle_deg in (45, 135, 225, 315):
        ang = math.radians(angle_deg)
        r0, r1 = s * 0.33, s * 0.42
        draw.line(
            [
                (cx + math.cos(ang) * r0, cy + math.sin(ang) * r0),
                (cx + math.cos(ang) * r1, cy + math.sin(ang) * r1),
            ],
            fill=ACCENT_CYAN + (135,),
            width=round(s * 0.012),
        )

    # Original angular geometric mecha head / visor silhouette (pure geometry)
    crest = [
        (s * 0.50, s * 0.14),
        (s * 0.64, s * 0.24),
        (s * 0.78, s * 0.19),
        (s * 0.73, s * 0.41),
        (s * 0.80, s * 0.54),
        (s * 0.68, s * 0.72),
        (s * 0.50, s * 0.78),
        (s * 0.32, s * 0.72),
        (s * 0.20, s * 0.54),
        (s * 0.27, s * 0.41),
        (s * 0.22, s * 0.19),
        (s * 0.36, s * 0.24),
    ]
    draw.polygon(crest, fill=FRAME_SHADOW + (245,), outline=ACCENT_CYAN + (210,), width=round(s * 0.014))

    inner_plate = [
        (s * 0.50, s * 0.22),
        (s * 0.65, s * 0.32),
        (s * 0.69, s * 0.53),
        (s * 0.61, s * 0.67),
        (s * 0.50, s * 0.71),
        (s * 0.39, s * 0.67),
        (s * 0.31, s * 0.53),
        (s * 0.35, s * 0.32),
    ]
    draw.polygon(inner_plate, fill=FRAME_STEEL + (255,), outline=FRAME_SHADOW + (255,), width=round(s * 0.01))

    # Angular amber optic visor + highlight band
    visor = [
        (s * 0.32, s * 0.43),
        (s * 0.50, s * 0.38),
        (s * 0.68, s * 0.43),
        (s * 0.64, s * 0.55),
        (s * 0.50, s * 0.52),
        (s * 0.36, s * 0.55),
    ]
    draw.polygon(visor, fill=VISOR_AMBER + (255,), outline=FRAME_SHADOW + (255,), width=round(s * 0.01))
    highlight = [
        (s * 0.37, s * 0.44),
        (s * 0.50, s * 0.41),
        (s * 0.63, s * 0.44),
        (s * 0.61, s * 0.47),
        (s * 0.50, s * 0.45),
        (s * 0.39, s * 0.47),
    ]
    draw.polygon(highlight, fill=VISOR_BRIGHT + (255,))

    # Subtle lower monogram "PC"
    draw_monogram(
        draw,
        x=s * 0.63,
        y=s * 0.67,
        h=s * 0.25,
        thickness=s * 0.052,
        outline=s * 0.018,
        fill=VISOR_AMBER,
    )
    image.alpha_composite(layer)
    return clip_to_tile(image)


def monogram_icon() -> Image.Image:
    s = MASTER
    image = tile(s)
    layer = Image.new("RGBA", (s, s))
    draw = ImageDraw.Draw(layer)
    draw_monogram(
        draw,
        x=s * 0.14,
        y=s * 0.22,
        h=s * 0.56,
        thickness=s * 0.12,
        outline=s * 0.036,
        fill=VISOR_AMBER,
    )
    image.alpha_composite(layer)
    return clip_to_tile(image)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()

    full = full_icon()
    small = monogram_icon()
    images = [
        (small if size < MONOGRAM_ONLY_BELOW else full).resize((size, size), Image.LANCZOS)
        for size in SIZES
    ]
    args.output.mkdir(parents=True, exist_ok=True)
    largest = images[-1]
    largest.save(args.output / "phantomcrash.png")
    largest.save(
        args.output / "phantomcrash.ico",
        sizes=[(size, size) for size in SIZES],
        append_images=images[:-1],
    )
    print(f"wrote {args.output / 'phantomcrash.ico'} ({len(SIZES)} sizes)")


if __name__ == "__main__":
    main()
