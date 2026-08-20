# Third-party material

## Summary

**This repository contains no third-party assets.** Every texture, the bitmap
font and every line of rendering code are original work. The only external
dependency is SDL2, which is linked, not vendored.

That is deliberate. The renderer generates its textures procedurally at startup
(`src/texture.c`) and embeds its font in source (`src/overlay.c`) precisely so
the project ships as source alone.

## Dependencies

| Component | Licence | How it is used |
|---|---|---|
| SDL2 | zlib licence | Linked at build time. Not redistributed here. |
| ImageMagick | ImageMagick licence (Apache-2.0 style) | Optional, used only by `scripts/make_texture_atlas.py` at author time. Not required to build or run. |

The C standard library and libm are used as provided by the toolchain.

## Optional texture packs

`--textures` can load a Minecraft-format resource pack converted by
`scripts/make_texture_atlas.py`. **No pack is included, and none may be.**

Resource packs are third-party artwork with their own licences, and most of them
forbid redistribution. The build is arranged so that a pack can never end up in
version control by accident:

- `.gitignore` blocks `*.zip`, `*.vxtx`, `docs/*.zip`, `textures/` and `packs/`.
- The generated `.vxtx` atlas is a **derivative work** containing the pack's
  pixels. It is subject to the pack's licence exactly as the original files are,
  and it is gitignored for that reason, not merely for tidiness.
- The renderer falls back to its own procedural textures whenever no pack is
  given, so nothing about the project depends on one.

**Before using any pack, read its licence.** If it forbids redistribution, use it
locally only: do not commit it, do not commit the atlas built from it, and do
not attach either to a report.

### The pack used while developing

Development and the demo used **Soartex Grove 1.1.2**
(<https://gitlab.com/GroveGraphics/soartex-grove>), © AVPSoftworks. It is *not*
included in this repository and *not* redistributed in any form.

Its terms are restrictive and worth knowing:

- The Grove licence requires prior permission for public distribution.
- Grove inherits textures from **Soartex Fanver**, whose licence prohibits public
  redistribution "in whole or in part", prohibits public distribution of modified
  packs containing its textures "under any circumstances", and permits **private
  distribution and personal use**.
- The original **Soartex** by Soar49 was released to the public domain in 2011,
  but Fanver and Invictus are explicitly *not* public domain and carry their own
  terms.
- Part of the pack is offered under **CC BY-NC-SA 4.0**, which requires
  attribution and forbids commercial use.

Local use for coursework falls under personal use. Publishing the pack, the
atlas built from it, or a modified pack containing its textures does not.

## A note for the report

Figures in the written report should use the **procedural** textures, not a pack.
They are the project's own work, they need no attribution or permission, and
they demonstrate what was actually built. If a pack appears in a screenshot,
credit it in the caption and link to its source.
