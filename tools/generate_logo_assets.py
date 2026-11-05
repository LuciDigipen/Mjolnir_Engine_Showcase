"""Regenerates the derived logo files in branding/ from branding/mjolnir-logo.png.

Run by hand when the logo changes and commit the output, so builds need no image tool.
Requires Pillow.
"""
from pathlib import Path

from PIL import Image, ImageFilter

BRANDING = Path(__file__).resolve().parent.parent / "branding"

ICON_SIZES = [16, 24, 32, 48, 64, 128, 256]
ICON_OUTLINE_COLOR = (18, 18, 22)
SPLASH_SIZE = 512
SPLASH_LOGO_FRACTION = 0.84


def white_with_alpha(alpha):
    # white under every pixel, so bilinear sampling can't pull in a dark fringe
    image = Image.new("RGBA", alpha.size, (255, 255, 255, 0))
    image.putalpha(alpha)
    return image


def centered_alpha(cropped_alpha, size, inner):
    # Fits the logo in inner pixels, keeping its aspect ratio, centered on a size square.
    scale = inner / max(cropped_alpha.size)
    glyph_size = (max(1, round(cropped_alpha.width * scale)), max(1, round(cropped_alpha.height * scale)))
    glyph = cropped_alpha.resize(glyph_size, Image.LANCZOS)

    canvas = Image.new("L", (size, size), 0)
    canvas.paste(glyph, ((size - glyph.width) // 2, (size - glyph.height) // 2))
    return canvas


def make_splash_logo(cropped_alpha):
    # Plain white: the splash draws its own hard drop shadow behind it.
    glyph_alpha = centered_alpha(cropped_alpha, SPLASH_SIZE, round(SPLASH_SIZE * SPLASH_LOGO_FRACTION))
    return white_with_alpha(glyph_alpha)


def make_icon_frame(cropped_alpha, size):
    # The logo is white: the outline keeps it visible on light Explorer backgrounds.
    outline = max(1, round(size / 32))
    glyph_alpha = centered_alpha(cropped_alpha, size, size - 2 * outline)
    outline_alpha = glyph_alpha.filter(ImageFilter.MaxFilter(2 * outline + 1))

    frame = Image.new("RGBA", (size, size), ICON_OUTLINE_COLOR + (0,))
    frame.putalpha(outline_alpha)
    frame.alpha_composite(white_with_alpha(glyph_alpha))
    return frame


def main():
    alpha = Image.open(BRANDING / "mjolnir-logo.png").convert("RGBA").getchannel("A")
    cropped = alpha.crop(alpha.getbbox())

    make_splash_logo(cropped).save(BRANDING / "mjolnir-logo-splash.png")

    frames = [make_icon_frame(cropped, size) for size in ICON_SIZES]
    frames[-1].save(BRANDING / "mjolnir.ico", format="ICO", append_images=frames[:-1],
                    sizes=[(size, size) for size in ICON_SIZES])


if __name__ == "__main__":
    main()
