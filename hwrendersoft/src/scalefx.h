#ifndef SCALEFX_H
#define SCALEFX_H

#include <stdint.h>

/* ScaleFX edge-directed pixel-art upscaler (pure C).
 * Ported from Sp00kyFox's RetroArch/libretro slang shader
 * (edge-smoothing/scalefx/shaders/scalefx-pass{0..4}.slang,
 * https://github.com/libretro/slang-shaders), released under the MIT
 * licence reproduced in scalefx.c.
 *
 * Unlike xBR, ScaleFX never blends colours: every output subpixel is a
 * verbatim copy of one of the 9 candidate input texels chosen by the
 * edge-classification passes, so hard sprite-art edges and binary alpha
 * stay exact. The algorithm is intrinsically a fixed 3x scaler (the final
 * pass tiles each source pixel into a 3x3 block of picks) - there is no
 * 2x/4x variant.
 *
 * src  - input RGBA pixels (R at byte 0, G at 1, B at 2, A at 3)
 * dst  - output buffer, must hold (w*3) * (h*3) * 4 bytes
 * w, h - input dimensions (> 0)
 * Returns 0 on success, -1 on failure (invalid size or allocation failure).
 */
int scalefx_scale(const uint8_t *src, uint8_t *dst, int w, int h);

#endif
