/* SPDX-License-Identifier: GPL-3.0-or-later
 * RGB565 event-picture effects. Tone compatibility follows EasyRPG Player's
 * mathematical hard-light/saturation transform; see RENDER_FX.md for provenance.
 * Inputs to tone are 0..255 with 128 neutral. Alpha is 0..256 inclusive. */
#ifndef BBVM_FX_H
#define BBVM_FX_H
#include "bbvm.h"

static inline bb_u32 bbvm_fx_clamp(int n) {
    return n < 0 ? 0u : n > 255 ? 255u : (bb_u32)n;
}

static inline bb_u32 bbvm_fx_light(bb_u32 tone, bb_u32 value) {
    bb_u32 n = tone <= 128u ? 2u * tone * value / 255u
        : 255u - 2u * (255u - tone) * (255u - value) / 255u;
    return n > 255u ? 255u : n;
}

static inline bb_u16 bbvm_fx_tone(bb_u16 pixel, bb_u32 red, bb_u32 green,
                                 bb_u32 blue, bb_u32 saturation) {
    bb_u32 r = (pixel >> 11) & 31u, g = (pixel >> 5) & 63u, b = pixel & 31u;
    if (red == 128u && green == 128u && blue == 128u && saturation == 128u)
        return pixel;
    r = (r << 3) | (r >> 2); g = (g << 2) | (g >> 4); b = (b << 3) | (b >> 2);
    if (saturation != 128u) {
        int lum = (int)((19595u*r + 38470u*g + 7471u*b) >> 16);
        int sat = saturation > 128u ? 1024 + (int)(saturation-128u)*16 : (int)saturation*8;
        r = bbvm_fx_clamp((lum*1024+((int)r-lum)*sat)/1024);
        g = bbvm_fx_clamp((lum*1024+((int)g-lum)*sat)/1024);
        b = bbvm_fx_clamp((lum*1024+((int)b-lum)*sat)/1024);
    }
    r = bbvm_fx_light(red,r); g = bbvm_fx_light(green,g); b = bbvm_fx_light(blue,b);
    return (bb_u16)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static inline bb_u16 bbvm_fx_blend(bb_u16 source, bb_u16 dest, bb_u32 alpha) {
    bb_u32 inverse = 256u-alpha;
    bb_u32 r = (((source>>11)*alpha + (dest>>11)*inverse + 128u)>>8);
    bb_u32 g = ((((source>>5)&63u)*alpha + ((dest>>5)&63u)*inverse + 128u)>>8);
    bb_u32 b = (((source&31u)*alpha + (dest&31u)*inverse + 128u)>>8);
    return (bb_u16)((r<<11)|(g<<5)|b);
}
#endif
