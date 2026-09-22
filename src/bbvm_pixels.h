#ifndef BBVM_PIXELS_H
#define BBVM_PIXELS_H

#include "bbvm.h"

/* Rectangles are clipped by the caller. Reverse traversal preserves the
 * original source for overlapping copies, including color-keyed pixels. */
static void bbvm_blit_pixels(bb_u16 *dst, const bb_u16 *src, int stride,
                            int sx, int sy, int width, int height,
                            int dx, int dy, bb_u16 key) {
    int row, column;
    int reverse = src == dst && (dy > sy || (dy == sy && dx > sx));
    for (row = 0; row < height; ++row) {
        int y = reverse ? height - 1 - row : row;
        const bb_u16 *s = src + (sy + y) * stride + sx;
        bb_u16 *d = dst + (dy + y) * stride + dx;
        if (reverse) {
            for (column = width; column-- > 0;) {
                bb_u16 pixel = s[column];
                if (pixel != key) d[column] = pixel;
            }
        } else {
            for (column = 0; column < width; ++column) {
                bb_u16 pixel = s[column];
                if (pixel != key) d[column] = pixel;
            }
        }
    }
}

#endif
