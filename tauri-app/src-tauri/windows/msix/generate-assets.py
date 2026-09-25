#!/usr/bin/env python3
"""Generate the Store tile assets an MSIX AppxManifest.xml requires, from
NetPulse's existing app icon.

Why this exists: the Microsoft Store / Desktop Bridge packaging format needs
a fixed set of PNG tile images at fixed pixel sizes (Square44x44Logo,
Square150x150Logo, Wide310x150Logo, StoreLogo) referenced by name from
AppxManifest.xml -- these are NOT the same files Tauri's own bundler icon
pipeline produces (that one targets .ico/.icns + a handful of PNG sizes for
NSIS/AppImage/dmg, none of which match MSIX's required set or aspect
ratios), so they need to be generated separately for this package.

Source: tauri-app/src-tauri/icons/icon.icns, the highest-resolution source
icon already committed to the repo (1024x1024) -- reused rather than adding
a new source asset, since Pillow can read .icns directly.

Only the mandatory baseline sizes are generated (unscaled, scale-100).
Windows will upscale/downscale these for other DPI settings, which is
visibly softer than shipping real scale-125/150/200/400 variants -- adding
those is a reasonable follow-up polish item, deliberately left out here to
keep the first working version of this pipeline simple to reason about and
verify.

Usage:
    python3 generate-assets.py <output-dir>
"""
import sys
from pathlib import Path

from PIL import Image

SOURCE_ICON = (
    Path(__file__).resolve().parents[2] / "icons" / "icon.icns"
)

# (filename, width, height) -- exact pixel sizes required by the MSIX/UWP
# manifest schema for a Desktop Bridge (Win32) app's mandatory tile set.
# https://learn.microsoft.com/en-us/windows/uwp/design/style/app-icons-and-logos
TILE_SPECS = [
    ("Square44x44Logo.png", 44, 44),
    ("Square150x150Logo.png", 150, 150),
    ("Wide310x150Logo.png", 310, 150),
    ("StoreLogo.png", 50, 50),
]


def make_square_tile(source: Image.Image, size: int) -> Image.Image:
    """Resize the (square) source icon down/up to an exact size x size tile."""
    return source.resize((size, size), Image.LANCZOS)


def make_wide_tile(source: Image.Image, width: int, height: int) -> Image.Image:
    """Build a non-square wide tile by centering the square icon, scaled to
    fit the tile's height, on a transparent canvas of the target dimensions
    -- there's no wide source art, so this is the standard, simplest correct
    way to derive a wide tile from a square icon without distorting it."""
    canvas = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    icon = source.resize((height, height), Image.LANCZOS)
    offset_x = (width - height) // 2
    canvas.paste(icon, (offset_x, 0), icon)
    return canvas


def main() -> None:
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <output-dir>", file=sys.stderr)
        sys.exit(1)

    out_dir = Path(sys.argv[1])
    out_dir.mkdir(parents=True, exist_ok=True)

    if not SOURCE_ICON.exists():
        print(f"Source icon not found: {SOURCE_ICON}", file=sys.stderr)
        sys.exit(1)

    source = Image.open(SOURCE_ICON).convert("RGBA")

    for filename, width, height in TILE_SPECS:
        if width == height:
            tile = make_square_tile(source, width)
        else:
            tile = make_wide_tile(source, width, height)
        dest = out_dir / filename
        tile.save(dest, format="PNG")
        print(f"wrote {dest} ({width}x{height})")


if __name__ == "__main__":
    main()
