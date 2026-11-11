#ifndef REALTIME_OVERLAY_DRAW_H
#define REALTIME_OVERLAY_DRAW_H

#include "overlay_canvas.h"

namespace draw {

void line(Canvas &c, int x0, int y0, int x1, int y1, uint32_t color);
void rect(Canvas &c, int x, int y, int w, int h, uint32_t color);
void fillRect(Canvas &c, int x, int y, int w, int h, uint32_t color);
void circle(Canvas &c, int cx, int cy, int r, uint32_t color);
void fillCircle(Canvas &c, int cx, int cy, int r, uint32_t color);
void triangle(Canvas &c, int x0, int y0, int x1, int y1, int x2, int y2, uint32_t color);
void fillTriangle(Canvas &c, int x0, int y0, int x1, int y1, int x2, int y2, uint32_t color);
void roundRect(Canvas &c, int x, int y, int w, int h, int r, uint32_t color);
void fillRoundRect(Canvas &c, int x, int y, int w, int h, int r, uint32_t color);

/* Built-in 8x8 bitmap font for ASCII debug text (no file dependency) */
void text(Canvas &c, int x, int y, const char *str, uint32_t color, int scale = 1);

} // namespace draw

#endif  // REALTIME_OVERLAY_DRAW_H
