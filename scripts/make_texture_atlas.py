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
* In modern packs `grass_block_side.png` is plain dirt. The green fringe is a
  separate greyscale OVERLAY that the game tints and composites on top, so it
  has to be composited here or grass blocks come out with bare dirt sides.

Candidates may include a directory, like `oak_log/1.png`. Packs that ship random
variants keep them in per-block folders, and a bare `1.png` would match dozens
of unrelated files.
"""

import argparse
import struct
import subprocess
import sys
from pathlib import Path

# Order must match the TEX_* enum in src/texture.h.
# Each entry: (slot name, [candidate file names], tint or None)
# Candidates cover both the modern (1.13+) and legacy naming schemes.
# Minecraft tints grass, leaves AND water by biome, so all three ship
# desaturated and come out grey unless tinted here. Grove's water_still.png
# averages a flat (200, 200, 200).
GRASS_TINT = (0x6A, 0xA8, 0x4A)
WATER_TINT = (0x3F, 0x76, 0xE4)
LEAF_TINT = (0x5A, 0x9B, 0x3C)
DRY_TINT = (0xB4, 0xB4, 0x6E)

# Order must match the TEX_* enum in src/texture.h.
# (slot, candidate paths, tint or None, overlay or None)
# Candidates are tried in order and cover the modern naming, the legacy naming
# and the per-block variant folders that CTM packs use.
TEXTURES = [
    ("stone",       ["stone.png", "stone/1.png"],                   None, None),
    ("cobble",      ["cobblestone.png", "cobblestone/1.png",
                     "cobble.png"],                                 None, None),
    ("dirt",        ["dirt.png", "dirt/1.png", "coarse_dirt.png"],  None, None),
    ("grass_top",   ["grass_block_top.png", "grass_top.png"],       GRASS_TINT, None),
    ("grass_side",  ["grass_block_side.png", "grass_side.png"],     None,
                    (["grass_block_side_overlay.png",
                      "grass_side_overlay.png"], GRASS_TINT)),
    ("gold",        ["gold_block.png"],                             None, None),
    ("diamond_ore", ["diamond_ore.png"],                            None, None),
    ("log_side",    ["oak_log.png", "oak_log/1.png", "oak_bark.png",
                     "log_oak.png"],                                None, None),
    ("log_top",     ["oak_log_top.png", "oak_log/top.png",
                     "log_oak_top.png"],                            None, None),
    ("brick",       ["bricks.png", "brick.png"],                    None, None),
    ("sand",        ["sand.png", "sand/1.png"],                     None, None),
    ("snow",        ["snow_block.png", "snow.png"],                 None, None),
    # `_opaque` variants exist precisely for renderers without transparency.
    ("leaves",      ["leaves/oak_opaque.png", "oak_leaves.png",
                     "leaves/oak.png", "leaves_oak.png"],           LEAF_TINT, None),
    ("water",       ["water_still.png", "water.png"],               WATER_TINT, None),
    ("gravel",      ["gravel.png", "gravel/1.png"],                 None, None),
    ("dry_grass",   ["grass_block_top.png", "grass_top.png"],       DRY_TINT, None),
]

MAGIC = b"VXTX"
VERSION = 1


def find_texture(pack_root: Path, candidates):
    """First candidate that exists under the pack, in the order given.

    A candidate matches only on a whole path segment boundary, so `stone.png`
    never matches `blackstone.png` and `oak_log/1.png` never matches
    `stone/1.png`.

    Matches are tried SHALLOWEST FIRST. Packs carry the same leaf name in many
    subfolders -- `block/button/stone.png` is the stone button, not the stone
    block -- and alphabetical order picks the wrong one.
    """
    for candidate in candidates:
        leaf = candidate.rsplit("/", 1)[-1]
        found = sorted(pack_root.rglob(leaf),
                       key=lambda m: (len(m.relative_to(pack_root).parts), str(m)))
        bare = "/" not in candidate
        for match in found:
            rel = match.relative_to(pack_root).as_posix()
            if rel != candidate and not rel.endswith("/" + candidate):
                continue
            # A candidate without a directory means "the block texture itself".
            # Requiring it to sit directly in block/ keeps `stone.png` from
            # resolving to the stone BUTTON, which lives in block/button/.
            if bare and match.parent.name not in ("block", "blocks"):
                continue
            return match
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


def tint_channel(value, tint_value):
    """Minecraft tints by multiplying, so a greyscale source keeps its luminance
    detail and takes its hue from the tint."""
    return value * tint_value // 255


def composite(base, raw_overlay, size, tint):
    """Alpha-composites a tinted overlay over already-packed base pixels."""
    out = list(base)
    for i in range(size * size):
        r, g, b, a = raw_overlay[i * 4: i * 4 + 4]
        if a == 0:
            continue
        if tint:
            r, g, b = (tint_channel(r, tint[0]),
                       tint_channel(g, tint[1]),
                       tint_channel(b, tint[2]))
        f = a / 255.0
        br = (out[i] >> 16) & 0xFF
        bg = (out[i] >> 8) & 0xFF
        bb = out[i] & 0xFF
        out[i] = ((int(r * f + br * (1 - f)) << 16)
                  | (int(g * f + bg * (1 - f)) << 8)
                  | int(b * f + bb * (1 - f)))
    return out


def to_pixels(raw, size, tint):
    """Raw RGBA bytes -> packed 0x00RRGGBB, tinted and flattened over grey."""
    out = []
    for i in range(size * size):
        r, g, b, a = raw[i * 4: i * 4 + 4]
        if tint:
            r, g, b = (tint_channel(r, tint[0]),
                       tint_channel(g, tint[1]),
                       tint_channel(b, tint[2]))
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
    for slot, candidates, tint, overlay in TEXTURES:
        path = find_texture(args.pack, candidates)
        if path is None:
            missing.append(f"{slot} (looked for {', '.join(candidates)})")
            pixels.extend([0x6E6E6E] * (size * size))  # flat grey placeholder
            continue

        packed = to_pixels(decode(path, size), size, tint)
        note = ""
        if overlay:
            overlay_candidates, overlay_tint = overlay
            overlay_path = find_texture(args.pack, overlay_candidates)
            if overlay_path:
                packed = composite(packed, decode(overlay_path, size),
                                   size, overlay_tint)
                note = f"  + {overlay_path.name}"

        pixels.extend(packed)
        print(f"  {slot:<12} {path.relative_to(args.pack)}{note}")

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
