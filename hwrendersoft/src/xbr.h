#ifndef XBR_H
#define XBR_H

#include <stdint.h>

/* Scale an RGBA image by factor 2, 3, or 4 using the xBR algorithm.
 * src  – input RGBA pixels (R at byte 0, G at 1, B at 2, A at 3)
 * dst  – output buffer, must hold w * factor * h * factor * 4 bytes
 * w, h – input dimensions (> 0)
 * Returns 0 on success, -1 on invalid factor.
 */
int xbr_scale(const uint8_t *src, uint8_t *dst, int w, int h, int factor);

#endif
