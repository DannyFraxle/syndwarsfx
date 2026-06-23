/*
 * xBR high-quality pixel-art upscaler (pure C)
 * --------------------------------------------
 * Based on Hyllian's xBR algorithm (FFmpeg / libxbr-standalone by Treeki).
 * Licensed under LGPL 2.1.
 */
#include "xbr.h"
#include <string.h>
#include <stdlib.h>

/* ----------------------------------------------------------------- */
/*  xBR algorithm core (adapted from FFmpeg/libxbr-standalone)       */
/* ----------------------------------------------------------------- */

/* YUV tables: 16M-entry RGB->YUV lookup (built once) */
static uint32_t *g_r2y = NULL;

/* Pixel diff in RGBA: alpha diff + YUV diff of RGB channels.
   Note: our RGBA pixel has byte order R=0,G=1,B=2,A=3.
   We convert to ARGB (B=0,G=1,R=2,A=3) by swapping R/B. */
static uint32_t argb_pixel(uint32_t rgba)
{
    return (rgba & 0xFF00FF00) | ((rgba & 0x00FF0000) >> 16) | ((rgba & 0x000000FF) << 16);
}

static uint32_t rgba_pixel(uint32_t argb)
{
    return argb_pixel(argb);
}

static uint32_t pixel_diff(uint32_t x, uint32_t y, const uint32_t *r2y)
{
    uint32_t yuv1 = r2y[x & 0xffffff];
    uint32_t yuv2 = r2y[y & 0xffffff];
    return (abs((int)((x >> 24) & 0xFF) - (int)((y >> 24) & 0xFF))) +
           (abs((int)((yuv1 >> 16) & 0xFF) - (int)((yuv2 >> 16) & 0xFF))) +
           (abs((int)((yuv1 >>  8) & 0xFF) - (int)((yuv2 >>  8) & 0xFF))) +
           abs((int)(yuv1 & 0xFF) - (int)(yuv2 & 0xFF));
}

#define ALPHA_BLEND_BASE(a, b, m, s) (  (0x00FF00FF & (((a) & 0x00FF00FF) + (((((b) & 0x00FF00FF) - ((a) & 0x00FF00FF)) * (m)) >> (s)))) \
                                      | ((0x00FF00FF & ((((a) >> 8) & 0x00FF00FF) + ((((((b) >> 8) & 0x00FF00FF) - (((a) >> 8) & 0x00FF00FF)) * (m)) >> (s)))) << 8))

#define ALPHA_BLEND_32_W(a, b)  ALPHA_BLEND_BASE(a, b, 1, 3)
#define ALPHA_BLEND_64_W(a, b)  ALPHA_BLEND_BASE(a, b, 1, 2)
#define ALPHA_BLEND_128_W(a, b) ALPHA_BLEND_BASE(a, b, 1, 1)
#define ALPHA_BLEND_192_W(a, b) ALPHA_BLEND_BASE(a, b, 3, 2)
#define ALPHA_BLEND_224_W(a, b) ALPHA_BLEND_BASE(a, b, 7, 3)

#define df(A, B) pixel_diff(A, B, r2y)
#define eq(A, B) (df(A, B) < 155)

#define FILT2(PE, PI, PH, PF, PG, PC, PD, PB, PA, G5, C4, G0, D0, C1, B1, F4, I4, H5, I5, A0, A1,   \
              N0, N1, N2, N3) do {                                                                  \
    if (PE != PH && PE != PF) {                                                                     \
        const unsigned e = df(PE,PC) + df(PE,PG) + df(PI,H5) + df(PI,F4) + (df(PH,PF)<<2);          \
        const unsigned i = df(PH,PD) + df(PH,I5) + df(PF,I4) + df(PF,PB) + (df(PE,PI)<<2);          \
        if (e <= i) {                                                                               \
            const unsigned px = df(PE,PF) <= df(PE,PH) ? PF : PH;                                   \
            if (e < i && (!eq(PF,PB) && !eq(PH,PD) || eq(PE,PI)                                     \
                          && (!eq(PF,I4) && !eq(PH,I5))                                             \
                          || eq(PE,PG) || eq(PE,PC))) {                                             \
                const unsigned ke = df(PF,PG);                                                      \
                const unsigned ki = df(PH,PC);                                                      \
                const int left    = ke<<1 <= ki && PE != PG && PD != PG;                            \
                const int up      = ke >= ki<<1 && PE != PC && PB != PC;                            \
                if (left && up) {                                                                   \
                    E[N3] = ALPHA_BLEND_224_W(E[N3], px);                                           \
                    E[N2] = ALPHA_BLEND_64_W( E[N2], px);                                           \
                    E[N1] = E[N2];                                                                  \
                } else if (left) {                                                                  \
                    E[N3] = ALPHA_BLEND_192_W(E[N3], px);                                           \
                    E[N2] = ALPHA_BLEND_64_W( E[N2], px);                                           \
                } else if (up) {                                                                    \
                    E[N3] = ALPHA_BLEND_192_W(E[N3], px);                                           \
                    E[N1] = ALPHA_BLEND_64_W( E[N1], px);                                           \
                } else { /* diagonal */                                                             \
                    E[N3] = ALPHA_BLEND_128_W(E[N3], px);                                           \
                }                                                                                   \
            } else {                                                                                \
                E[N3] = ALPHA_BLEND_128_W(E[N3], px);                                               \
            }                                                                                       \
        }                                                                                           \
    }                                                                                               \
} while (0)

#define FILT3(PE, PI, PH, PF, PG, PC, PD, PB, PA, G5, C4, G0, D0, C1, B1, F4, I4, H5, I5, A0, A1,   \
              N0, N1, N2, N3, N4, N5, N6, N7, N8) do {                                              \
    if (PE != PH && PE != PF) {                                                                     \
        const unsigned e = df(PE,PC) + df(PE,PG) + df(PI,H5) + df(PI,F4) + (df(PH,PF)<<2);          \
        const unsigned i = df(PH,PD) + df(PH,I5) + df(PF,I4) + df(PF,PB) + (df(PE,PI)<<2);          \
        if (e <= i) {                                                                               \
            const unsigned px = df(PE,PF) <= df(PE,PH) ? PF : PH;                                   \
            if (e < i && (!eq(PF,PB) && !eq(PF,PC) || !eq(PH,PD) && !eq(PH,PG) || eq(PE,PI)         \
                          && (!eq(PF,F4) && !eq(PF,I4) || !eq(PH,H5) && !eq(PH,I5))                 \
                          || eq(PE,PG) || eq(PE,PC))) {                                             \
                const unsigned ke = df(PF,PG);                                                      \
                const unsigned ki = df(PH,PC);                                                      \
                const int left    = ke<<1 <= ki && PE != PG && PD != PG;                            \
                const int up      = ke >= ki<<1 && PE != PC && PB != PC;                            \
                if (left && up) {                                                                   \
                    E[N7] = ALPHA_BLEND_192_W(E[N7], px);                                           \
                    E[N6] = ALPHA_BLEND_64_W( E[N6], px);                                           \
                    E[N5] = E[N7];                                                                  \
                    E[N2] = E[N6];                                                                  \
                    E[N8] = px;                                                                     \
                } else if (left) {                                                                  \
                    E[N7] = ALPHA_BLEND_192_W(E[N7], px);                                           \
                    E[N5] = ALPHA_BLEND_64_W( E[N5], px);                                           \
                    E[N6] = ALPHA_BLEND_64_W( E[N6], px);                                           \
                    E[N8] = px;                                                                     \
                } else if (up) {                                                                    \
                    E[N5] = ALPHA_BLEND_192_W(E[N5], px);                                           \
                    E[N7] = ALPHA_BLEND_64_W( E[N7], px);                                           \
                    E[N2] = ALPHA_BLEND_64_W( E[N2], px);                                           \
                    E[N8] = px;                                                                     \
                } else { /* diagonal */                                                             \
                    E[N8] = ALPHA_BLEND_224_W(E[N8], px);                                           \
                    E[N5] = ALPHA_BLEND_32_W( E[N5], px);                                           \
                    E[N7] = ALPHA_BLEND_32_W( E[N7], px);                                           \
                }                                                                                   \
            } else {                                                                                \
                E[N8] = ALPHA_BLEND_128_W(E[N8], px);                                               \
            }                                                                                       \
        }                                                                                           \
    }                                                                                               \
} while (0)

#define FILT4(PE, PI, PH, PF, PG, PC, PD, PB, PA, G5, C4, G0, D0, C1, B1, F4, I4, H5, I5, A0, A1,   \
              N15, N14, N11, N3, N7, N10, N13, N12, N9, N6, N2, N1, N5, N8, N4, N0) do {            \
    if (PE != PH && PE != PF) {                                                                     \
        const unsigned e = df(PE,PC) + df(PE,PG) + df(PI,H5) + df(PI,F4) + (df(PH,PF)<<2);          \
        const unsigned i = df(PH,PD) + df(PH,I5) + df(PF,I4) + df(PF,PB) + (df(PE,PI)<<2);          \
        if (e <= i) {                                                                               \
            const unsigned px = df(PE,PF) <= df(PE,PH) ? PF : PH;                                   \
            if (e < i && (!eq(PF,PB) && !eq(PH,PD) || eq(PE,PI)                                     \
                          && (!eq(PF,I4) && !eq(PH,I5))                                             \
                          || eq(PE,PG) || eq(PE,PC))) {                                             \
                const unsigned ke = df(PF,PG);                                                      \
                const unsigned ki = df(PH,PC);                                                      \
                const int left    = ke<<1 <= ki && PE != PG && PD != PG;                            \
                const int up      = ke >= ki<<1 && PE != PC && PB != PC;                            \
                if (left && up) {                                                                   \
                    E[N13] = ALPHA_BLEND_192_W(E[N13], px);                                         \
                    E[N12] = ALPHA_BLEND_64_W( E[N12], px);                                         \
                    E[N15] = E[N14] = E[N11] = px;                                                  \
                    E[N10] = E[N3]  = E[N12];                                                       \
                    E[N7]  = E[N13];                                                                \
                } else if (left) {                                                                  \
                    E[N11] = ALPHA_BLEND_192_W(E[N11], px);                                         \
                    E[N13] = ALPHA_BLEND_192_W(E[N13], px);                                         \
                    E[N10] = ALPHA_BLEND_64_W( E[N10], px);                                         \
                    E[N12] = ALPHA_BLEND_64_W( E[N12], px);                                         \
                    E[N14] = px;                                                                    \
                    E[N15] = px;                                                                    \
                } else if (up) {                                                                    \
                    E[N14] = ALPHA_BLEND_192_W(E[N14], px);                                         \
                    E[N7 ] = ALPHA_BLEND_192_W(E[N7 ], px);                                         \
                    E[N10] = ALPHA_BLEND_64_W( E[N10], px);                                         \
                    E[N3 ] = ALPHA_BLEND_64_W( E[N3 ], px);                                         \
                    E[N11] = px;                                                                    \
                    E[N15] = px;                                                                    \
                } else { /* diagonal */                                                             \
                    E[N11] = ALPHA_BLEND_128_W(E[N11], px);                                         \
                    E[N14] = ALPHA_BLEND_128_W(E[N14], px);                                         \
                    E[N15] = px;                                                                    \
                }                                                                                   \
            } else {                                                                                \
                E[N15] = ALPHA_BLEND_128_W(E[N15], px);                                             \
            }                                                                                       \
        }                                                                                           \
    }                                                                                               \
} while (0)

static void xbr_filter(const uint32_t *src, uint32_t *dst,
                       int w, int h, int pitch, int out_pitch, int factor)
{
    const uint32_t *r2y = g_r2y;
    const int nl  = out_pitch >> 2;
    const int nl1 = nl + nl;
    const int nl2 = nl1 + nl;

    for (int y = 0; y < h; y++) {
        uint32_t *E = dst + y * out_pitch / 4 * factor;
        const uint32_t *sa2 = src + y * pitch / 4 - 2;
        const uint32_t *sa1 = sa2 - pitch / 4;
        const uint32_t *sa0 = sa1 - pitch / 4;
        const uint32_t *sa3 = sa2 + pitch / 4;
        const uint32_t *sa4 = sa3 + pitch / 4;

        if (y <= 1) { sa0 = sa1; if (y == 0) sa0 = sa1 = sa2; }
        if (y >= h - 2) { sa4 = sa3; if (y == h - 1) sa4 = sa3 = sa2; }

        for (int x = 0; x < w; x++) {
            const uint32_t B1 = sa0[2], PB = sa1[2], PE = sa2[2], PH = sa3[2], H5 = sa4[2];

            const int pprev = 2 - (x > 0);
            const uint32_t A1 = sa0[pprev], PA = sa1[pprev], PD = sa2[pprev], PG = sa3[pprev], G5 = sa4[pprev];

            const int pprev2 = pprev - (x > 1);
            const uint32_t A0 = sa1[pprev2], D0 = sa2[pprev2], G0 = sa3[pprev2];

            const int pnext = 3 - (x == w - 1);
            const uint32_t C1 = sa0[pnext], PC = sa1[pnext], PF = sa2[pnext], PI = sa3[pnext], I5 = sa4[pnext];

            const int pnext2 = pnext + 1 - (x >= w - 2);
            const uint32_t C4 = sa1[pnext2], F4 = sa2[pnext2], I4 = sa3[pnext2];

            if (factor == 2) {
                E[0] = E[1] = E[nl] = E[nl + 1] = PE;
                FILT2(PE, PI, PH, PF, PG, PC, PD, PB, PA, G5, C4, G0, D0, C1, B1, F4, I4, H5, I5, A0, A1, 0, 1, nl, nl+1);
                FILT2(PE, PC, PF, PB, PI, PA, PH, PD, PG, I4, A1, I5, H5, A0, D0, B1, C1, F4, C4, G5, G0, nl, 0, nl+1, 1);
                FILT2(PE, PA, PB, PD, PC, PG, PF, PH, PI, C1, G0, C4, F4, G5, H5, D0, A0, B1, A1, I4, I5, nl+1, nl, 1, 0);
                FILT2(PE, PG, PD, PH, PA, PI, PB, PF, PC, A0, I5, A1, B1, I4, F4, H5, G5, D0, G0, C1, C4, 1, nl+1, 0, nl);
            } else if (factor == 3) {
                E[0] = E[1] = E[2] = E[nl] = E[nl+1] = E[nl+2] = E[nl1] = E[nl1+1] = E[nl1+2] = PE;
                FILT3(PE, PI, PH, PF, PG, PC, PD, PB, PA, G5, C4, G0, D0, C1, B1, F4, I4, H5, I5, A0, A1, 0, 1, 2, nl, nl+1, nl+2, nl1, nl1+1, nl1+2);
                FILT3(PE, PC, PF, PB, PI, PA, PH, PD, PG, I4, A1, I5, H5, A0, D0, B1, C1, F4, C4, G5, G0, nl1, nl, 0, nl1+1, nl+1, 1, nl1+2, nl+2, 2);
                FILT3(PE, PA, PB, PD, PC, PG, PF, PH, PI, C1, G0, C4, F4, G5, H5, D0, A0, B1, A1, I4, I5, nl1+2, nl1+1, nl1, nl+2, nl+1, nl, 2, 1, 0);
                FILT3(PE, PG, PD, PH, PA, PI, PB, PF, PC, A0, I5, A1, B1, I4, F4, H5, G5, D0, G0, C1, C4, 2, nl+2, nl1+2, 1, nl+1, nl1+1, 0, nl, nl1);
            } else if (factor == 4) {
                E[0] = E[1] = E[2] = E[3] = E[nl] = E[nl+1] = E[nl+2] = E[nl+3] =
                E[nl1] = E[nl1+1] = E[nl1+2] = E[nl1+3] = E[nl2] = E[nl2+1] = E[nl2+2] = E[nl2+3] = PE;
                FILT4(PE, PI, PH, PF, PG, PC, PD, PB, PA, G5, C4, G0, D0, C1, B1, F4, I4, H5, I5, A0, A1, nl2+3, nl2+2, nl1+3, 3, nl+3, nl1+2, nl2+1, nl2, nl1+1, nl+2, 2, 1, nl+1, nl1, nl, 0);
                FILT4(PE, PC, PF, PB, PI, PA, PH, PD, PG, I4, A1, I5, H5, A0, D0, B1, C1, F4, C4, G5, G0, 3, nl+3, 2, 0, 1, nl+2, nl1+3, nl2+3, nl1+2, nl+1, nl, nl1, nl1+1, nl2+2, nl2+1, nl2);
                FILT4(PE, PA, PB, PD, PC, PG, PF, PH, PI, C1, G0, C4, F4, G5, H5, D0, A0, B1, A1, I4, I5, 0, 1, nl, nl2, nl1, nl+1, 2, 3, nl+2, nl1+1, nl2+1, nl2+2, nl1+2, nl+3, nl1+3, nl2+3);
                FILT4(PE, PG, PD, PH, PA, PI, PB, PF, PC, A0, I5, A1, B1, I4, F4, H5, G5, D0, G0, C1, C4, nl2, nl1, nl2+1, nl2+3, nl2+2, nl1+1, nl, 0, nl+1, nl1+2, nl1+3, nl+3, nl+2, 1, 2, 3);
            }

            sa0++; sa1++; sa2++; sa3++; sa4++;
            E += factor;
        }
    }
}

/* ----------------------------------------------------------------- */
/*  YUV lookup table initialisation                                  */
/* ----------------------------------------------------------------- */
static void ensure_lut(void)
{
    if (g_r2y) return;
    g_r2y = (uint32_t *)malloc(sizeof(uint32_t) * (1 << 24));
    if (!g_r2y) return;
    for (int bg = -255; bg < 256; bg++) {
        for (int rg = -255; rg < 256; rg++) {
            const uint32_t u = (uint32_t)((-169 * rg + 500 * bg) / 1000) + 128;
            const uint32_t v = (uint32_t)(( 500 * rg -  81 * bg) / 1000) + 128;
            int startg = bg > -bg ? bg : -bg;
            int tmp = rg > -rg ? rg : -rg;
            if (tmp > startg) startg = tmp;
            if (startg < 0) startg = 0;
            int endg = 255;
            tmp = 255 - bg; if (tmp < endg) endg = tmp;
            tmp = 255 - rg; if (tmp < endg) endg = tmp;
            if (startg > endg) continue;
            uint32_t y = (uint32_t)((299 * rg + 1000 * startg + 114 * bg) / 1000);
            uint32_t c = (uint32_t)(bg + (rg << 16) + 0x010101 * startg);
            for (int g = startg; g <= endg; g++) {
                g_r2y[c] = ((y++) << 16) + (u << 8) + v;
                c += 0x010101;
            }
        }
    }
}

/* ----------------------------------------------------------------- */
/*  Public API — wraps the xBR algorithm for RGBA + edge padding    */
/* ----------------------------------------------------------------- */

/* Reusable work buffers — max sprite is 256×256, so padded is 260×260,
   and max output at 4× is 1040×1040 in uint32_t.  These avoid
   per-invocation malloc/free churn when processing many unique frames. */
static uint32_t *g_padded     = NULL;
static int       g_padded_sz  = 0;
static uint32_t *g_outpadded  = NULL;
static int       g_outpadded_sz = 0;

int xbr_scale(const uint8_t *src, uint8_t *dst, int w, int h, int factor)
{
    if (factor < 2 || factor > 4) return -1;
    ensure_lut();
    if (!g_r2y) return -1;

    const int pw = w + 4, ph = h + 4;
    const int padded_req  = pw * ph;
    const int outpad_req  = pw * ph * factor * factor;
    if (padded_req > g_padded_sz) {
        free(g_padded);
        g_padded = (uint32_t *)malloc((size_t)padded_req * sizeof(uint32_t));
        if (!g_padded) return -1;
        g_padded_sz = padded_req;
    }
    if (outpad_req > g_outpadded_sz) {
        free(g_outpadded);
        g_outpadded = (uint32_t *)malloc((size_t)outpad_req * sizeof(uint32_t));
        if (!g_outpadded) return -1;
        g_outpadded_sz = outpad_req;
    }
    uint32_t *padded = g_padded;
    uint32_t *out_padded = g_outpadded;

    /* Place source (converted to ARGB) into the padded centre, leaving
       a 2-pixel border for the algorithm to read without bounds checks. */
    for (int y = 0; y < h; y++) {
        const uint32_t *src_row = (const uint32_t *)(src + (size_t)y * w * 4);
        uint32_t *dst_row = padded + (y + 2) * pw + 2;
        for (int x = 0; x < w; x++)
            dst_row[x] = argb_pixel(src_row[x]);
    }

    /* Extend all borders by repeating the edge row/col values. */
    for (int y = 0; y < ph; y++) {
        uint32_t left_pix = padded[y * pw + 2];
        padded[y * pw + 0] = left_pix;
        padded[y * pw + 1] = left_pix;
        uint32_t right_pix = padded[y * pw + 2 + w - 1];
        padded[y * pw + 2 + w + 0] = right_pix;
        padded[y * pw + 2 + w + 1] = right_pix;
    }
    for (int x = 0; x < pw; x++) {
        padded[0 * pw + x] = padded[2 * pw + x];
        padded[1 * pw + x] = padded[2 * pw + x];
        padded[(ph - 1) * pw + x] = padded[(h + 1) * pw + x];
        padded[(ph - 2) * pw + x] = padded[(h + 1) * pw + x];
    }

    const int out_pw = pw * factor, out_ph = ph * factor;

    xbr_filter(padded, out_padded, pw, ph, pw * 4, out_pw * 4, factor);

    const int border = 2 * factor;
    uint32_t *dst32 = (uint32_t *)dst;
    for (int y = 0; y < h * factor; y++) {
        const uint32_t *src_row = out_padded + (y + border) * out_pw + border;
        for (int x = 0; x < w * factor; x++)
            dst32[y * w * factor + x] = rgba_pixel(src_row[x]);
    }
    return 0;
}
