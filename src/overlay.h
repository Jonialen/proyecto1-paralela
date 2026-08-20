#ifndef OVERLAY_H
#define OVERLAY_H

#include <stdint.h>

/* On-screen HUD drawn with an embedded 5x7 bitmap font.
 *
 * It writes into the RESOLVED buffer, at native window resolution, not into the
 * supersampled framebuffer: text drawn before the box filter would come out
 * blurred, and drawing it after costs the same at any --ssaa setting. */

#define OVERLAY_GLYPH_W 5
#define OVERLAY_GLYPH_H 7

/* Width in pixels that `text` will occupy at the given scale. */
int overlay_text_width(const char *text, int scale);

/* Blends a solid rectangle over the image. `alpha` is 0-255. Used as a backing
 * panel so the HUD stays readable over bright terrain. */
void overlay_panel(uint32_t *pixels, int width, int height,
                   int x, int y, int w, int h, uint32_t color, int alpha);

/* Draws `text` with its top-left corner at (x, y). Lowercase is folded to
 * uppercase; unsupported characters render as blanks. */
void overlay_text(uint32_t *pixels, int width, int height,
                  int x, int y, int scale, uint32_t color, const char *text);

#endif /* OVERLAY_H */
