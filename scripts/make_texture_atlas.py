#!/usr/bin/env python3
"""Pack a Minecraft-style texture pack into the atlas the renderer reads.

The program has no PNG decoder on purpose: adding one would mean either a new
build dependency or several thousand lines of third-party code in a project
whose brief asks for our own. Decoding happens here instead, offline, and the
renderer just fread()s a raw block of pixels.

Usage:
    scripts/make_texture_atlas.py <pack-directory> <output.vxtx> [--size N]

Requires ImageMagick (`magick`) on PATH.

Two details that matter for the result to look right:

* Grass and leaf textures ship GREYSCALE in Minecraft packs. The game tints
  them per biome at runtime. Loaded as-is they come out white, so this script
  applies a fixed tint.
* Some textures (water, fire) are ANIMATION STRIPS: a tall image holding N
  square frames stacked vertically. Only the first frame is taken.
"""

import argparse
import struct
import subprocess
import sys
from pathlib import Path

# Order must match the TEX_* enum in src/texture.h.
# Each entry: (slot name, [candidate file names], tint or None)
# Candidates cover both the modern (1.13+) and legacy naming schemes.
TEXTURES = [
    ("stone",       ["stone.png"],                                  None),
    ("cobble",      ["cobblestone.png", "cobble.png"],              None),
    ("dirt",        ["dirt.png", "coarse_dirt.png"],                None),
    ("grass_top",   ["grass_block_top.png", "grass_top.png"],       (0x6A, 0xA8, 0x4A)),
    ("grass_side",  ["grass_block_side.png", "grass_side.png"],     None),
    ("gold",        ["gold_block.png"],                             None),
    ("diamond_ore", ["diamond_ore.png"],                            None),
    ("log_side",    ["oak_log.png", "log_oak.png"],                 None),
    ("log_top",     ["oak_log_top.png", "log_oak_top.png"],         None),
    ("brick",       ["bricks.png", "brick.png"],                    None),
    ("sand",        ["sand.png"],                                   None),
    ("snow",        ["snow_block.png", "snow.png"],                 None),
    ("leaves",      ["oak_leaves.png", "leaves_oak.png"],           (0x5A, 0x9B, 0x3C)),
    ("water",       ["water_still.png", "water.png"],               None),
    ("gravel",      ["gravel.png"],                                 None),
    ("dry_grass",   ["grass_block_top.png", "grass_top.png"],       (0xB4, 0xB4, 0x6E)),
]

MAGIC = b"VXTX"
VERSION = 1


def find_texture(pack_root: Path, candidates):
    """First candidate that exists anywhere under the pack, by search order."""
    for name in candidates:
        matches = sorted(pack_root.rglob(name))
        if matches:
            # Prefer a path under textures/block(s)/ over a random match.
            for match in matches:
                if "block" in match.parent.name:
                    return match
            return matches[0]
    return None


def decode(path: Path, size: int):
    """PNG -> list of (r, g, b, a), cropped to its first square frame."""
    dims = subprocess.run(["magick", "identify", "-format", "%w %h", str(path)],
                          capture_output=True, text=True, check=True).stdout.split()
    width, height = int(dims[0]), int(dims[1])

    # An animation strip is taller than it is wide; keep only the top frame.
    crop = f"{width}x{width}+0+0" if height > width else f"{width}x{height}+0+0"

    raw = subprocess.run(
        ["magick", str(path), "-crop", crop, "+repage",
         "-resize", f"{size}x{size}!", "-depth", "8", "rgba:-"],
        capture_output=True, check=True).stdout

    expected = size * size * 4
    if len(raw) != expected:
        raise RuntimeError(f"{path.name}: got {len(raw)} bytes, expected {expected}")
    return raw


def to_pixels(raw, size, tint):
    """Raw RGBA bytes -> packed 0x00RRGGBB, tinted and flattened over grey."""
    out = []
    for i in range(size * size):
        r, g, b, a = raw[i * 4: i * 4 + 4]
        if tint:
            # Minecraft tints by multiplying, so a greyscale source keeps its
            # luminance detail and takes the hue from the tint.
            r = r * tint[0] // 255
            g = g * tint[1] // 255
            b = b * tint[2] // 255
        if a < 250:
            # The renderer is fully opaque; composite transparent pixels over a
            # neutral grey so cut-outs do not read as black holes.
            f = a / 255.0
            r = int(r * f + 0x6E * (1 - f))
            g = int(g * f + 0x6E * (1 - f))
            b = int(b * f + 0x6E * (1 - f))
        out.append((r << 16) | (g << 8) | b)
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("pack", type=Path, help="texture pack directory")
    parser.add_argument("output", type=Path, help="atlas file to write")
    parser.add_argument("--size", type=int, default=16,
                        help="edge length, power of two (default 16)")
    args = parser.parse_args()

    size = args.size
    if size < 8 or size > 512 or (size & (size - 1)) != 0:
        sys.exit(f"error: --size must be a power of two between 8 and 512, got {size}")
    if not args.pack.is_dir():
        sys.exit(f"error: '{args.pack}' is not a directory")

    try:
        subprocess.run(["magick", "-version"], capture_output=True, check=True)
    except (FileNotFoundError, subprocess.CalledProcessError):
        sys.exit("error: ImageMagick ('magick') is required and was not found")

    pixels = []
    missing = []
    for slot, candidates, tint in TEXTURES:
        path = find_texture(args.pack, candidates)
        if path is None:
            missing.append(f"{slot} (looked for {', '.join(candidates)})")
            pixels.extend([0x6E6E6E] * (size * size))  # flat grey placeholder
            continue
        pixels.extend(to_pixels(decode(path, size), size, tint))
        print(f"  {slot:<12} {path.relative_to(args.pack)}")

    with open(args.output, "wb") as f:
        f.write(MAGIC)
        f.write(struct.pack("<III", VERSION, len(TEXTURES), size))
        f.write(struct.pack(f"<{len(pixels)}I", *pixels))

    print(f"\nWrote {args.output} ({len(TEXTURES)} textures at {size}x{size})")
    if missing:
        print("\nNot found in this pack, filled with grey:")
        for entry in missing:
            print(f"  - {entry}")
        print("Add the names to TEXTURES in this script if the pack calls them "
              "something else.")


if __name__ == "__main__":
    main()
