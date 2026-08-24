/*
 * ScaleFX edge-directed pixel-art upscaler (pure C port)
 * -------------------------------------------------------
 * Ported line-for-line from Sp00kyFox's 5-pass RetroArch/libretro slang
 * shader (edge-smoothing/scalefx/shaders/scalefx-pass{0..4}.slang,
 * https://github.com/libretro/slang-shaders, preset scalefx.slangp).
 *
 * Original shader copyright notice (MIT):
 *
 * Copyright (c) 2016 Sp00kyFox - ScaleFX@web.de
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * ---------------------------------------------------------------------------
 * Porting notes
 * ---------------------------------------------------------------------------
 * The shader pipeline is 5 GPU passes, each a full-screen pass reading the
 * previous pass's output (plus, for pass 4, the untouched original via the
 * "refpass" alias). Ported here as 5 CPU passes over flat buffers:
 *
 *   pass0  -> metric[]     per-pixel perceptual colour distance to 4 neighbours
 *   pass1  -> strength[]   per-pixel corner-interpolation candidate strength
 *   pass2  -> junction[]   per-pixel boolean tags (res/hori/vert/orien) per
 *                          one of the pixel's 4 corner junctions
 *   pass3  -> tag[]        per-pixel final corner/mid subpixel picks (0-8)
 *   pass4  -> output       3x output image: each subpixel copies one of the
 *                          9 candidate texels (E/D/D0/F/F0/B/B0/H/H0) - no
 *                          blending, unlike xBR.
 *
 * The shader packs pass2's 4 booleans per junction into a single float
 * channel (`(res + 2*hori + 4*vert + 8*orien) / 15`) purely because GPU
 * textures only have 4 colour channels to carry 4 junctions x 4 booleans.
 * Since this port keeps the values as plain C structs there is no need to
 * pack/unpack bits - `junction[]` stores the 4 booleans directly per corner.
 * All arithmetic below is otherwise a direct transcription (GLSL vec4
 * swizzles yzwx/zwxy/wxyz are index rotations `[(k+1)%4]` etc; the
 * step()-based LE/GE/LEQ/GEQ/NOT macros are reproduced exactly, including
 * their (non-obvious) strict/non-strict semantics).
 *
 * Required sampling border: pass3 reads pass2 data up to 3 texels away,
 * pass2 reads pass1 (+1) and pass0 (+1), pass1 reads pass0 (+1), pass0
 * reads the source image (+1). Total: a 6-pixel edge-replicated border
 * around the source image is enough to compute pass3's tags validly for
 * every visible pixel (see the BORDER constant below).
 */
#include "scalefx.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef struct { float x, y, z, w; } V4;

/* --- step()-based helpers, exactly as used by the GLSL source ------------ */
/* #define LE(x, y)  (1 - step(y, x))   -> 1 iff x <  y (strict) */
/* #define GE(x, y)  (1 - step(x, y))   -> 1 iff x >  y (strict) */
/* #define LEQ(x, y) step(x, y)         -> 1 iff x <= y           */
/* #define GEQ(x, y) step(y, x)         -> 1 iff x >= y           */
static float sfxLE (float x, float y) { return (x < y)  ? 1.0f : 0.0f; }
static float sfxGE (float x, float y) { return (x > y)  ? 1.0f : 0.0f; }
static float sfxLEQ(float x, float y) { return (x <= y) ? 1.0f : 0.0f; }
static float sfxNOT(float x) { return 1.0f - x; }

/* Default shader parameter values (scalefx.slangp does not override the
 * #pragma parameter defaults declared in the shaders). */
#define SFX_CLR 0.50f   /* pass1: corner-strength threshold */
#define SFX_SAA 1.00f   /* pass1: filter anti-aliased (soft) corners */
#define SFX_SCN 1.00f   /* pass3: allow level-1 corners without a "screen" neighbour */

/* Reference: http://www.compuphase.com/cmetric.htm */
static float sfx_dist(V4 A, V4 B)
{
    float r = 0.5f * (A.x + B.x);
    float dr = A.x - B.x, dg = A.y - B.y, db = A.z - B.z;
    float cr = 2.0f + r, cg = 4.0f, cb = 3.0f - r;
    return sqrtf(cr * dr * dr + cg * dg * dg + cb * db * db) / 3.0f;
}

/* pass1: corner strength */
static float sfx_str(float d, float ax, float ay, float bx, float by)
{
    float diff  = ax - ay;
    float wght1 = fmaxf(SFX_CLR - d, 0.0f) / SFX_CLR;
    float cond  = (fminf(ax, bx) + ax) > (fminf(ay, by) + ay);
    float wght2 = (1.0f - d) + (cond ? diff : -diff);
    if (wght2 < 0.0f) wght2 = 0.0f;
    if (wght2 > 1.0f) wght2 = 1.0f;
    if (SFX_SAA == 1.0f || 2.0f * d < ax + ay)
        return (wght1 * wght2) * (ax * ay);
    return 0.0f;
}

/* pass2: dom(x,y,z,w) where x/y/z/w are vec3 args; x0..x2 = x.x,x.y,x.z etc. */
static V4 sfx_dom(float x0, float x1, float x2,
                   float y0, float y1, float y2,
                   float z0, float z1, float z2,
                   float w0, float w1, float w2)
{
    V4 r;
    r.x = 2.0f * x1 - (x0 + x2);
    r.y = 2.0f * y1 - (y0 + y2);
    r.z = 2.0f * z1 - (z0 + z2);
    r.w = 2.0f * w1 - (w0 + w2);
    return r;
}

/* pass2: majority vote for ambiguous dominance junctions, applied to jDx/y/z/w. */
static void sfx_majority(const float jD[4], float outJ[4])
{
    int k;
    for (k = 0; k < 4; k++) {
        float a = jD[k], b = jD[(k + 1) % 4], c = jD[(k + 2) % 4], d = jD[(k + 3) % 4];
        float t1 = sfxGE(a, 0.0f) * (sfxLEQ(b, 0.0f) * sfxLEQ(d, 0.0f) + sfxGE(a + c, b + d));
        outJ[k] = fminf(t1, 1.0f);
    }
}

/* pass2: necessary-but-not-sufficient orthogonal-edge junction condition. */
static float sfx_clear(float crnx, float crny, float ax, float ay, float bx, float by)
{
    float t1 = crnx >= fmaxf(fminf(ax, ay), fminf(bx, by));
    float t2 = crny >= fmaxf(fminf(ax, by), fminf(bx, ay));
    return (t1 && t2) ? 1.0f : 0.0f;
}

/* Per-pixel boolean tags for the 4 corner junctions (x=NW,y=NE,z=SE,w=SW),
 * i.e. pass2's packed (res + 2*hori + 4*vert + 8*orien)/15 output kept
 * unpacked since this is plain C, not a 4-channel GPU texture. */
typedef struct {
    int res[4];
    int hori[4];
    int vert[4];
    int orien[4];
} Junction;

/* Border needed around the source image so every visible pixel's pass3
 * tags can be computed validly (see file header). */
#define BORDER 6

static V4      *g_raw = NULL;      /* (w+2B) x (h+2B), edge-replicated source */
static V4      *g_metric = NULL;   /* pass0 output, same dims as g_raw */
static V4      *g_strength = NULL; /* pass1 output, same dims as g_raw */
static Junction *g_junction = NULL; /* pass2 output, same dims as g_raw */
static int      *g_crn = NULL;     /* pass3 output, w x h, 4 ints per pixel */
static int      *g_mid = NULL;     /* pass3 output, w x h, 4 ints per pixel */
static int g_buf_w = 0, g_buf_h = 0; /* padded dims the above are sized for */
static int g_tag_w = 0, g_tag_h = 0; /* visible dims g_crn/g_mid are sized for */

static int sfx_ensure_buffers(int w, int h)
{
    int pw = w + 2 * BORDER, ph = h + 2 * BORDER;
    if (pw * ph > g_buf_w * g_buf_h || g_raw == NULL) {
        size_t n = (size_t)pw * (size_t)ph;
        V4 *raw, *metric, *strength;
        Junction *junction;
        raw = (V4 *)realloc(g_raw, n * sizeof(V4));
        if (!raw) return -1;
        g_raw = raw;
        metric = (V4 *)realloc(g_metric, n * sizeof(V4));
        if (!metric) return -1;
        g_metric = metric;
        strength = (V4 *)realloc(g_strength, n * sizeof(V4));
        if (!strength) return -1;
        g_strength = strength;
        junction = (Junction *)realloc(g_junction, n * sizeof(Junction));
        if (!junction) return -1;
        g_junction = junction;
        g_buf_w = pw; g_buf_h = ph;
    }
    if (w * h > g_tag_w * g_tag_h || g_crn == NULL) {
        size_t n = (size_t)w * (size_t)h * 4;
        int *crn = (int *)realloc(g_crn, n * sizeof(int));
        if (!crn) return -1;
        g_crn = crn;
        int *mid = (int *)realloc(g_mid, n * sizeof(int));
        if (!mid) return -1;
        g_mid = mid;
        g_tag_w = w; g_tag_h = h;
    }
    return 0;
}

int scalefx_scale(const uint8_t *src, uint8_t *dst, int w, int h)
{
    int pw, ph, x, y;

    if (w <= 0 || h <= 0) return -1;
    if (sfx_ensure_buffers(w, h) != 0) return -1;

    pw = w + 2 * BORDER;
    ph = h + 2 * BORDER;

#define RAWXY(px, py) g_raw[(py) * pw + (px)]
#define METXY(px, py) g_metric[(py) * pw + (px)]
#define STRXY(px, py) g_strength[(py) * pw + (px)]
#define JUNXY(px, py) g_junction[(py) * pw + (px)]

    /* ---- Load source into the padded RGBA buffer (float 0..1), with the
     * outer BORDER edge-replicated so every pass can sample past the
     * image edge without a special case. ---- */
    for (y = 0; y < h; y++) {
        const uint8_t *srow = src + (size_t)y * w * 4;
        for (x = 0; x < w; x++) {
            V4 *p = &RAWXY(x + BORDER, y + BORDER);
            p->x = srow[x * 4 + 0] / 255.0f;
            p->y = srow[x * 4 + 1] / 255.0f;
            p->z = srow[x * 4 + 2] / 255.0f;
            p->w = srow[x * 4 + 3] / 255.0f;
        }
    }
    for (y = 0; y < ph; y++) {
        int sy = y - BORDER; if (sy < 0) sy = 0; if (sy > h - 1) sy = h - 1;
        for (x = 0; x < pw; x++) {
            int sx = x - BORDER; if (sx < 0) sx = 0; if (sx > w - 1) sx = w - 1;
            if (x >= BORDER && x < BORDER + w && y >= BORDER && y < BORDER + h)
                continue; /* already filled with real data above */
            RAWXY(x, y) = RAWXY(sx + BORDER, sy + BORDER);
        }
    }

    /* ---- Pass 0: per-pixel distance metric to NW/N/NE/E neighbours. ---- */
    for (y = 1; y < ph - 1; y++) {
        for (x = 1; x < pw - 1; x++) {
            V4 A = RAWXY(x - 1, y - 1), B = RAWXY(x, y - 1), C = RAWXY(x + 1, y - 1);
            V4 E = RAWXY(x, y), F = RAWXY(x + 1, y);
            V4 *m = &METXY(x, y);
            m->x = sfx_dist(E, A);
            m->y = sfx_dist(E, B);
            m->z = sfx_dist(E, C);
            m->w = sfx_dist(E, F);
        }
    }

    /* ---- Pass 1: corner-interpolation candidate strength. ---- */
    for (y = 2; y < ph - 2; y++) {
        for (x = 2; x < pw - 2; x++) {
            V4 A = METXY(x - 1, y - 1), B = METXY(x, y - 1);
            V4 D = METXY(x - 1, y),     E = METXY(x, y),     F = METXY(x + 1, y);
            V4 G = METXY(x - 1, y + 1), H = METXY(x, y + 1), I = METXY(x + 1, y + 1);
            V4 *s = &STRXY(x, y);
            s->x = sfx_str(D.z, D.w, E.y, A.w, D.y);
            s->y = sfx_str(F.x, E.w, E.y, B.w, F.y);
            s->z = sfx_str(H.z, E.w, H.y, H.w, I.y);
            s->w = sfx_str(H.x, D.w, H.y, G.w, G.y);
        }
    }

    /* ---- Pass 2: resolve ambiguous junction dominance and orthogonal
     * edges into 4 boolean tags (res/hori/vert/orien) per corner. ---- */
    for (y = 3; y < ph - 3; y++) {
        for (x = 3; x < pw - 3; x++) {
            V4 A = METXY(x - 1, y - 1), B = METXY(x, y - 1);
            V4 D = METXY(x - 1, y),     E = METXY(x, y),     F = METXY(x + 1, y);
            V4 G = METXY(x - 1, y + 1), H = METXY(x, y + 1), I = METXY(x + 1, y + 1);

            V4 As = STRXY(x - 1, y - 1), Bs = STRXY(x, y - 1), Cs = STRXY(x + 1, y - 1);
            V4 Ds = STRXY(x - 1, y),     Es = STRXY(x, y),     Fs = STRXY(x + 1, y);
            V4 Gs = STRXY(x - 1, y + 1), Hs = STRXY(x, y + 1), Is = STRXY(x + 1, y + 1);

            V4 jDx = sfx_dom(As.y, As.z, As.w,  Bs.z, Bs.w, Bs.x,  Es.w, Es.x, Es.y,  Ds.x, Ds.y, Ds.z);
            V4 jDy = sfx_dom(Bs.y, Bs.z, Bs.w,  Cs.z, Cs.w, Cs.x,  Fs.w, Fs.x, Fs.y,  Es.x, Es.y, Es.z);
            V4 jDz = sfx_dom(Es.y, Es.z, Es.w,  Fs.z, Fs.w, Fs.x,  Is.w, Is.x, Is.y,  Hs.x, Hs.y, Hs.z);
            V4 jDw = sfx_dom(Ds.y, Ds.z, Ds.w,  Es.z, Es.w, Es.x,  Hs.w, Hs.x, Hs.y,  Gs.x, Gs.y, Gs.z);

            float jSx[4] = { As.z, Bs.w, Es.x, Ds.y };
            float jSy[4] = { Bs.z, Cs.w, Fs.x, Es.y };
            float jSz[4] = { Es.z, Fs.w, Is.x, Hs.y };
            float jSw[4] = { Ds.z, Es.w, Hs.x, Gs.y };

            float jx[4], jy[4], jz[4], jw[4];
            { float a[4] = { jDx.x, jDx.y, jDx.z, jDx.w }; sfx_majority(a, jx); }
            { float a[4] = { jDy.x, jDy.y, jDy.z, jDy.w }; sfx_majority(a, jy); }
            { float a[4] = { jDz.x, jDz.y, jDz.z, jDz.w }; sfx_majority(a, jz); }
            { float a[4] = { jDw.x, jDw.y, jDw.z, jDw.w }; sfx_majority(a, jw); }

            float res[4];
            res[0] = fminf(jx[2] + sfxNOT(jx[1]) * sfxNOT(jx[3]) * sfxGE(jSx[2], 0.0f)
                            * (jx[0] + sfxGE(jSx[0] + jSx[2], jSx[1] + jSx[3])), 1.0f);
            res[1] = fminf(jy[3] + sfxNOT(jy[2]) * sfxNOT(jy[0]) * sfxGE(jSy[3], 0.0f)
                            * (jy[1] + sfxGE(jSy[1] + jSy[3], jSy[0] + jSy[2])), 1.0f);
            res[2] = fminf(jz[0] + sfxNOT(jz[3]) * sfxNOT(jz[1]) * sfxGE(jSz[0], 0.0f)
                            * (jz[2] + sfxGE(jSz[0] + jSz[2], jSz[1] + jSz[3])), 1.0f);
            res[3] = fminf(jw[1] + sfxNOT(jw[0]) * sfxNOT(jw[2]) * sfxGE(jSw[1], 0.0f)
                            * (jw[3] + sfxGE(jSw[1] + jSw[3], jSw[0] + jSw[2])), 1.0f);

            {
                float base[4] = { jx[2], jy[3], jz[0], jw[1] };
                float newres[4];
                int k;
                for (k = 0; k < 4; k++) {
                    float rprev = res[(k + 3) % 4], rnext = res[(k + 1) % 4];
                    newres[k] = fminf(res[k] * (base[k] + sfxNOT(rprev * rnext)), 1.0f);
                }
                memcpy(res, newres, sizeof(res));
            }

            float clr[4];
            clr[0] = sfx_clear(D.z, E.x,  D.w, E.y,  A.w, D.y);
            clr[1] = sfx_clear(F.x, E.z,  E.w, E.y,  B.w, F.y);
            clr[2] = sfx_clear(H.z, I.x,  E.w, H.y,  H.w, I.y);
            clr[3] = sfx_clear(H.x, G.z,  D.w, H.y,  G.w, G.y);

            {
                float hh[4] = { fminf(D.w, A.w), fminf(E.w, B.w), fminf(E.w, H.w), fminf(D.w, G.w) };
                float vv[4] = { fminf(E.y, D.y), fminf(E.y, F.y), fminf(H.y, I.y), fminf(H.y, G.y) };
                float hadd[4] = { D.w, E.w, E.w, D.w };
                float vadd[4] = { E.y, E.y, H.y, H.y };
                Junction *jn = &JUNXY(x, y);
                int k;
                for (k = 0; k < 4; k++) {
                    int orien = sfxGE(hh[k] + hadd[k], vv[k] + vadd[k]) != 0.0f;
                    int hori  = (sfxLE(hh[k], vv[k]) != 0.0f) && (clr[k] != 0.0f);
                    int vert  = (sfxGE(hh[k], vv[k]) != 0.0f) && (clr[k] != 0.0f);
                    jn->res[k]   = (res[k] != 0.0f);
                    jn->hori[k]  = hori;
                    jn->vert[k]  = vert;
                    jn->orien[k] = orien;
                }
            }
        }
    }

    /* ---- Pass 3: classify each corner/mid subpixel candidate (0-8), for
     * every VISIBLE pixel only (this needs pass2 data up to 3 texels away,
     * which BORDER=6 guarantees is valid). ---- */
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            int px = x + BORDER, py = y + BORDER;
            Junction E  = JUNXY(px, py);
            Junction D  = JUNXY(px - 1, py), D0 = JUNXY(px - 2, py), D1 = JUNXY(px - 3, py);
            Junction F  = JUNXY(px + 1, py), F0 = JUNXY(px + 2, py), F1 = JUNXY(px + 3, py);
            Junction B  = JUNXY(px, py - 1), B0 = JUNXY(px, py - 2), B1 = JUNXY(px, py - 3);
            Junction H  = JUNXY(px, py + 1), H0 = JUNXY(px, py + 2), H1 = JUNXY(px, py + 3);

            const int *Ec = E.res, *Eh = E.hori, *Ev = E.vert, *Eo = E.orien;
            const int *Dc = D.res, *Dh = D.hori, *Do = D.orien;
            const int *D0c = D0.res, *D0h = D0.hori, *D1h = D1.hori;
            const int *Fc = F.res, *Fh = F.hori, *Fo = F.orien;
            const int *F0c = F0.res, *F0h = F0.hori, *F1h = F1.hori;
            const int *Bc = B.res, *Bv = B.vert, *Bo = B.orien;
            const int *B0c = B0.res, *B0v = B0.vert, *B1v = B1.vert;
            const int *Hc = H.res, *Hv = H.vert, *Ho = H.orien;
            const int *H0c = H0.res, *H0v = H0.vert, *H1v = H1.vert;

            int scn = (SFX_SCN == 1.0f);

            int lvl1x = Ec[0] && (Dc[2] || Bc[2] || scn);
            int lvl1y = Ec[1] && (Fc[3] || Bc[3] || scn);
            int lvl1z = Ec[2] && (Fc[0] || Hc[0] || scn);
            int lvl1w = Ec[3] && (Dc[1] || Hc[1] || scn);

            int lvl2x[2] = { (Ec[0] && Eh[1]) && Dc[2], (Ec[1] && Eh[0]) && Fc[3] };
            int lvl2y[2] = { (Ec[1] && Ev[2]) && Bc[3], (Ec[2] && Ev[1]) && Hc[0] };
            int lvl2z[2] = { (Ec[3] && Eh[2]) && Dc[1], (Ec[2] && Eh[3]) && Fc[0] };
            int lvl2w[2] = { (Ec[0] && Ev[3]) && Bc[2], (Ec[3] && Ev[0]) && Hc[1] };

            int lvl3x[2] = { lvl2x[1] && (Dh[1] && Dh[0]) && Fh[2], lvl2w[1] && (Bv[3] && Bv[0]) && Hv[2] };
            int lvl3y[2] = { lvl2x[0] && (Fh[0] && Fh[1]) && Dh[3], lvl2y[1] && (Bv[2] && Bv[1]) && Hv[3] };
            int lvl3z[2] = { lvl2z[0] && (Fh[3] && Fh[2]) && Dh[0], lvl2y[0] && (Hv[1] && Hv[2]) && Bv[0] };
            int lvl3w[2] = { lvl2z[1] && (Dh[2] && Dh[3]) && Fh[1], lvl2w[0] && (Hv[0] && Hv[3]) && Bv[1] };

            int lvl4x[2] = {
                (Dc[0] && Dh[1] && Eh[0] && Eh[1] && Fh[0] && Fh[1]) && (D0c[2] && D0h[3]),
                (Bc[0] && Bv[3] && Ev[0] && Ev[3] && Hv[0] && Hv[3]) && (B0c[2] && B0v[1]) };
            int lvl4y[2] = {
                (Fc[1] && Fh[0] && Eh[1] && Eh[0] && Dh[1] && Dh[0]) && (F0c[3] && F0h[2]),
                (Bc[1] && Bv[2] && Ev[1] && Ev[2] && Hv[1] && Hv[2]) && (B0c[3] && B0v[0]) };
            int lvl4z[2] = {
                (Fc[2] && Fh[3] && Eh[2] && Eh[3] && Dh[2] && Dh[3]) && (F0c[0] && F0h[1]),
                (Hc[2] && Hv[1] && Ev[2] && Ev[1] && Bv[2] && Bv[1]) && (H0c[0] && H0v[3]) };
            int lvl4w[2] = {
                (Dc[3] && Dh[2] && Eh[3] && Eh[2] && Fh[3] && Fh[2]) && (D0c[1] && D0h[0]),
                (Hc[3] && Hv[0] && Ev[3] && Ev[0] && Bv[3] && Bv[0]) && (H0c[1] && H0v[2]) };

            int lvl5x[2] = { lvl4x[0] && (F0h[0] && F0h[1]) && (D1h[2] && D1h[3]),
                              lvl4y[0] && (D0h[1] && D0h[0]) && (F1h[3] && F1h[2]) };
            int lvl5y[2] = { lvl4y[1] && (H0v[1] && H0v[2]) && (B1v[3] && B1v[0]),
                              lvl4z[1] && (B0v[2] && B0v[1]) && (H1v[0] && H1v[3]) };
            int lvl5z[2] = { lvl4w[0] && (F0h[3] && F0h[2]) && (D1h[1] && D1h[0]),
                              lvl4z[0] && (D0h[2] && D0h[3]) && (F1h[0] && F1h[1]) };
            int lvl5w[2] = { lvl4x[1] && (H0v[0] && H0v[3]) && (B1v[2] && B1v[1]),
                              lvl4w[1] && (B0v[3] && B0v[0]) && (H1v[1] && H1v[2]) };

            int lvl6x[2] = { lvl5x[1] && (D1h[1] && D1h[0]), lvl5w[1] && (B1v[3] && B1v[0]) };
            int lvl6y[2] = { lvl5x[0] && (F1h[0] && F1h[1]), lvl5y[1] && (B1v[2] && B1v[1]) };
            int lvl6z[2] = { lvl5z[0] && (F1h[3] && F1h[2]), lvl5y[0] && (H1v[1] && H1v[2]) };
            int lvl6w[2] = { lvl5z[1] && (D1h[2] && D1h[3]), lvl5w[0] && (H1v[0] && H1v[3]) };

            int crn[4], mid[4];

            crn[0] = (lvl1x && Eo[0]) || (lvl3x[0] && Eo[1]) || (lvl4x[0] && Do[0]) || (lvl6x[0] && Fo[1]) ? 5
                   : (lvl1x || (lvl3x[1] && !Eo[3]) || (lvl4x[1] && !Bo[0]) || (lvl6x[1] && !Ho[3])) ? 1
                   : lvl3x[0] ? 3 : lvl3x[1] ? 7 : lvl4x[0] ? 2 : lvl4x[1] ? 6 : lvl6x[0] ? 4 : lvl6x[1] ? 8 : 0;
            crn[1] = (lvl1y && Eo[1]) || (lvl3y[0] && Eo[0]) || (lvl4y[0] && Fo[1]) || (lvl6y[0] && Do[0]) ? 5
                   : (lvl1y || (lvl3y[1] && !Eo[2]) || (lvl4y[1] && !Bo[1]) || (lvl6y[1] && !Ho[2])) ? 3
                   : lvl3y[0] ? 1 : lvl3y[1] ? 7 : lvl4y[0] ? 4 : lvl4y[1] ? 6 : lvl6y[0] ? 2 : lvl6y[1] ? 8 : 0;
            crn[2] = (lvl1z && Eo[2]) || (lvl3z[0] && Eo[3]) || (lvl4z[0] && Fo[2]) || (lvl6z[0] && Do[3]) ? 7
                   : (lvl1z || (lvl3z[1] && !Eo[1]) || (lvl4z[1] && !Ho[2]) || (lvl6z[1] && !Bo[1])) ? 3
                   : lvl3z[0] ? 1 : lvl3z[1] ? 5 : lvl4z[0] ? 4 : lvl4z[1] ? 8 : lvl6z[0] ? 2 : lvl6z[1] ? 6 : 0;
            crn[3] = (lvl1w && Eo[3]) || (lvl3w[0] && Eo[2]) || (lvl4w[0] && Do[3]) || (lvl6w[0] && Fo[2]) ? 7
                   : (lvl1w || (lvl3w[1] && !Eo[0]) || (lvl4w[1] && !Ho[3]) || (lvl6w[1] && !Bo[0])) ? 1
                   : lvl3w[0] ? 3 : lvl3w[1] ? 5 : lvl4w[0] ? 2 : lvl4w[1] ? 8 : lvl6w[0] ? 4 : lvl6w[1] ? 6 : 0;

            mid[0] = (lvl2x[0] && Eo[0]) || (lvl2x[1] && Eo[1]) || (lvl5x[0] && Do[0]) || (lvl5x[1] && Fo[1]) ? 5
                   : lvl2x[0] ? 1 : lvl2x[1] ? 3 : lvl5x[0] ? 2 : lvl5x[1] ? 4
                   : (Ec[0] && Dc[2] && Ec[1] && Fc[3]) ? (Eo[0] ? (Eo[1] ? 5 : 3) : 1) : 0;
            mid[1] = (lvl2y[0] && !Eo[1]) || (lvl2y[1] && !Eo[2]) || (lvl5y[0] && !Bo[1]) || (lvl5y[1] && !Ho[2]) ? 3
                   : lvl2y[0] ? 5 : lvl2y[1] ? 7 : lvl5y[0] ? 6 : lvl5y[1] ? 8
                   : (Ec[1] && Bc[3] && Ec[2] && Hc[0]) ? (!Eo[1] ? (!Eo[2] ? 3 : 7) : 5) : 0;
            mid[2] = (lvl2z[0] && Eo[3]) || (lvl2z[1] && Eo[2]) || (lvl5z[0] && Do[3]) || (lvl5z[1] && Fo[2]) ? 7
                   : lvl2z[0] ? 1 : lvl2z[1] ? 3 : lvl5z[0] ? 2 : lvl5z[1] ? 4
                   : (Ec[2] && Fc[0] && Ec[3] && Dc[1]) ? (Eo[2] ? (Eo[3] ? 7 : 1) : 3) : 0;
            mid[3] = (lvl2w[0] && !Eo[0]) || (lvl2w[1] && !Eo[3]) || (lvl5w[0] && !Bo[0]) || (lvl5w[1] && !Ho[3]) ? 1
                   : lvl2w[0] ? 5 : lvl2w[1] ? 7 : lvl5w[0] ? 6 : lvl5w[1] ? 8
                   : (Ec[3] && Hc[1] && Ec[0] && Bc[2]) ? (!Eo[3] ? (!Eo[0] ? 1 : 5) : 7) : 0;

            memcpy(&g_crn[(y * w + x) * 4], crn, sizeof(crn));
            memcpy(&g_mid[(y * w + x) * 4], mid, sizeof(mid));
        }
    }

    /* ---- Pass 4: emit the 3x output. Each of the 9 subpixels of a source
     * pixel picks one of 9 candidates (E/D/D0/F/F0/B/B0/H/H0) verbatim from
     * the ORIGINAL (unfiltered) image - never a blend. ---- */
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            const int *crn = &g_crn[(y * w + x) * 4];
            const int *mid = &g_mid[(y * w + x) * 4];
            /*  grid (subpixel layout, row major, 0=top):
             *    crn.x  mid.x  crn.y
             *    mid.w    0    mid.y
             *    crn.w  mid.z  crn.z
             */
            int sp[9] = {
                crn[0], mid[0], crn[1],
                mid[3], 0,      mid[1],
                crn[3], mid[2], crn[2]
            };
            int sub;
            for (sub = 0; sub < 9; sub++) {
                int ox, oy;
                switch (sp[sub]) {
                    case 1: ox = -1; oy =  0; break;
                    case 2: ox = -2; oy =  0; break;
                    case 3: ox =  1; oy =  0; break;
                    case 4: ox =  2; oy =  0; break;
                    case 5: ox =  0; oy = -1; break;
                    case 6: ox =  0; oy = -2; break;
                    case 7: ox =  0; oy =  1; break;
                    case 8: ox =  0; oy =  2; break;
                    default: ox = 0; oy = 0; break;
                }
                {
                    V4 c = RAWXY(x + BORDER + ox, y + BORDER + oy);
                    int dx = x * 3 + (sub % 3), dy = y * 3 + (sub / 3);
                    uint8_t *d = dst + ((size_t)dy * (w * 3) + dx) * 4;
                    d[0] = (uint8_t)(c.x * 255.0f + 0.5f);
                    d[1] = (uint8_t)(c.y * 255.0f + 0.5f);
                    d[2] = (uint8_t)(c.z * 255.0f + 0.5f);
                    d[3] = (uint8_t)(c.w * 255.0f + 0.5f);
                }
            }
        }
    }

#undef RAWXY
#undef METXY
#undef STRXY
#undef JUNXY

    return 0;
}
