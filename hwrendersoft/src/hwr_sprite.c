#include "hwr_api.h"
#include "hwr_gl.h"
#include "hwr_internal.h"
#include "hwr_lights.h"
#include "hwr_scene_source.h"
#include "hwr_sprite.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* =========================================================================
 * RLE decoder — game-agnostic TbSprite RLE → 8-bit indexed buffer
 * =========================================================================
 * RLE format (row-major):
 *   signed byte count:
 *     >0 : literal run of `count` opaque palette indices follow
 *     <0 : transparent run of `-count` pixels (write index 0)
 *     =0 : end of row
 * ========================================================================*/

/* Consume remaining RLE data for this row up to and including the 0x00 EOL marker.
 * Must be called after EVERY row regardless of how many pixels were output, because
 * the BF RLE format always ends each row with 0x00, and if the run-length sum
 * exactly equals w then cnt==0 hasn't been consumed yet. */
static void skip_to_eol(const uint8_t **rle)
{
    int8_t cnt;
    do {
        cnt = (int8_t)*(*rle)++;
        if (cnt > 0)
            (*rle) += cnt;
    } while (cnt != 0);
}

int hwr_rle_decode(const uint8_t *rle, uint8_t *out, int w, int h)
{
    int row;
    if (rle == NULL || out == NULL || w <= 0 || h <= 0)
        return -1;
    for (row = 0; row < h; row++) {
        uint8_t *row_out = out + row * w;
        int col = 0;
        while (col < w) {
            int8_t cnt = (int8_t)*rle++;
            if (cnt == 0)
                break;
            if (cnt > 0) {
                int copy = cnt;
                if (col + copy > w) copy = w - col;
                memcpy(row_out + col, rle, (size_t)copy);
                rle += cnt;
                col += copy;
            } else {
                int skip = -cnt;
                if (col + skip > w) skip = w - col;
                memset(row_out + col, 0, (size_t)skip);
                col += skip;
            }
        }
        /* Fill remaining columns with transparent if row was short */
        if (col < w) {
            memset(row_out + col, 0, (size_t)(w - col));
        } else {
            /* Row was fully consumed (or overflowed) — 0x00 EOL marker hasn't
             * been read yet. Skip remaining RLE data up to and including it. */
            skip_to_eol(&rle);
        }
    }
    return 0;
}

int hwr_rle_decode_opaque(const uint8_t *rle, uint8_t *out, uint8_t *opq, int w, int h)
{
    int row;
    if (rle == NULL || out == NULL || w <= 0 || h <= 0)
        return -1;
    for (row = 0; row < h; row++) {
        uint8_t *row_out = out + row * w;
        uint8_t *row_opq = opq + row * w;
        int col = 0;
        while (col < w) {
            int8_t cnt = (int8_t)*rle++;
            if (cnt == 0)
                break;
            if (cnt > 0) {
                int copy = cnt;
                if (col + copy > w) copy = w - col;
                memcpy(row_out + col, rle, (size_t)copy);
                memset(row_opq + col, 255, (size_t)copy);
                rle += cnt;
                col += copy;
            } else {
                int skip = -cnt;
                if (col + skip > w) skip = w - col;
                memset(row_out + col, 0, (size_t)skip);
                memset(row_opq + col, 0, (size_t)skip);
                col += skip;
            }
        }
        /* Mark remaining as transparent */
        if (col < w) {
            memset(row_out + col, 0, (size_t)(w - col));
            memset(row_opq + col, 0, (size_t)(w - col));
        } else {
            /* Row was fully consumed (or overflowed) — 0x00 EOL marker hasn't
             * been read yet. Skip remaining RLE data up to and including it. */
            skip_to_eol(&rle);
        }
    }
    return 0;
}

/* =========================================================================
 * Atlas — lazy 8192×8192 GL_RGBA8 shelf packer + hash table
 * =========================================================================
 * The atlas packs decoded sprite pixel data as RGBA into a GL_RGBA8 texture.
 * xBR-upscaled full-colour sprite composites are stored here.
 * The key is a 32-bit hash of (frame, frv_pack, angle) set by the source.
 * ========================================================================*/

/* Hash table entry */
typedef struct {
    uint32_t key;
    int      slot;       /* -1 = empty */
} AtlasEntry;

/* Shelf: horizontal strip where sprites are placed left-to-right */
typedef struct Shelf {
    int x, y, w, h;          /* remaining rect in the shelf */
    struct Shelf *next;
} Shelf;

static struct {
    GLuint    tex;             /* GL_RGBA8 HWR_ATLAS_W x HWR_ATLAS_H texture */
    int       ready;           /* GL texture created by renderer thread */
    int       hash_ready;      /* hash table initialized (safe from main thread) */
    Shelf     *shelves;        /* linked list of shelves */
    int       next_shelf_y;    /* y for the next new shelf */

    AtlasEntry hash[HWR_ATLAS_MAX_SLOTS];
    int       slot_count;      /* total unique sprites registered */
    int       slot_w[HWR_ATLAS_MAX_SLOTS];
    int       slot_h[HWR_ATLAS_MAX_SLOTS];
    float     slot_u0[HWR_ATLAS_MAX_SLOTS];
    float     slot_v0[HWR_ATLAS_MAX_SLOTS];
    float     slot_u1[HWR_ATLAS_MAX_SLOTS];
    float     slot_v1[HWR_ATLAS_MAX_SLOTS];

    /* Padded box actually reserved/uploaded in the atlas (sprite rect + 1px
     * border on each side, see ATLAS_PAD). UVs above stay pointed at the
     * inner (unpadded) sprite rect; the border exists purely so GL_LINEAR
     * sampling near the edge blends with a clamped copy of the sprite's own
     * border pixel instead of bleeding into whatever was packed next door. */
    int       slot_box_x[HWR_ATLAS_MAX_SLOTS];
    int       slot_box_y[HWR_ATLAS_MAX_SLOTS];
    int       slot_box_w[HWR_ATLAS_MAX_SLOTS];
    int       slot_box_h[HWR_ATLAS_MAX_SLOTS];

    /* Deferred GL upload: pixels stored here by the main thread, uploaded by the
     * renderer thread via hwr_atlas_upload_pending(). NULL = no pending upload. */
    uint8_t   *pending[HWR_ATLAS_MAX_SLOTS];

    /* Blacklist: keys that failed to register (atlas full). Entries are hashed
     * with linear probing and a sentinel key of 0xFFFFFFFF = empty. */
#define HWR_ATLAS_BL_SIZE  512
    uint32_t  blacklist[HWR_ATLAS_BL_SIZE];
} at;

/* FNV-1a hash for the 32-bit key */
static uint32_t atlas_hash(uint32_t key)
{
    uint32_t h = 2166136261u;
    h = (h ^ (uint8_t)(key >> 0))  * 16777619u;
    h = (h ^ (uint8_t)(key >> 8))  * 16777619u;
    h = (h ^ (uint8_t)(key >> 16)) * 16777619u;
    h = (h ^ (uint8_t)(key >> 24)) * 16777619u;
    return h;
}

/* Initialise the hash table to all-empty (-1). Safe to call from any thread
 * because it does NO GL work — only memset. */
static void atlas_init_hash(void)
{
    if (at.hash_ready)
        return;
    memset(at.hash, 0xFF, sizeof(at.hash));
    memset(at.pending, 0, sizeof(at.pending));
    memset(at.blacklist, 0xFF, sizeof(at.blacklist));
    at.slot_count = 0;
    at.hash_ready = 1;
}

/* Blacklist helpers: 512-slot linear probe with sentinel 0xFFFFFFFF = empty. */
static int atlas_blacklisted(uint32_t key)
{
    uint32_t idx = key % HWR_ATLAS_BL_SIZE;
    for (uint32_t probe = 0; probe < HWR_ATLAS_BL_SIZE; probe++) {
        uint32_t k = at.blacklist[idx];
        if (k == key) return 1;
        if (k == 0xFFFFFFFF) return 0;
        idx = (idx + 1) % HWR_ATLAS_BL_SIZE;
    }
    return 0;
}
static void atlas_blacklist_add(uint32_t key)
{
    uint32_t idx = key % HWR_ATLAS_BL_SIZE;
    for (uint32_t probe = 0; probe < HWR_ATLAS_BL_SIZE; probe++) {
        if (at.blacklist[idx] == 0xFFFFFFFF) {
            at.blacklist[idx] = key;
            return;
        }
        idx = (idx + 1) % HWR_ATLAS_BL_SIZE;
    }
}

/* Create the GL texture for the atlas. Must be called from the renderer thread
 * (GL context must be current). */
static void atlas_init_gl(void)
{
    if (at.ready)
        return;
    atlas_init_hash();  /* ensure hash is ready too */

    {
        GLint max_tex = 0;
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_tex);
        if (max_tex < HWR_ATLAS_W || max_tex < HWR_ATLAS_H) {
            fprintf(stderr,
                "hwr_sprite: GL_MAX_TEXTURE_SIZE=%d is smaller than the "
                "%dx%d sprite atlas; sprites will fail to register and "
                "disappear. Rebuild with a smaller HWR_ATLAS_W/H.\n",
                (int)max_tex, HWR_ATLAS_W, HWR_ATLAS_H);
        }
    }

    glGenTextures(1, &at.tex);
    glBindTexture(GL_TEXTURE_2D, at.tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    {
        /* RGBA8: full-colour sprite pixels */
        size_t sz = (size_t)HWR_ATLAS_W * HWR_ATLAS_H * 4;
        uint8_t *zeros = (uint8_t *)calloc(1, sz);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, HWR_ATLAS_W, HWR_ATLAS_H, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, zeros);
        free(zeros);
    }
    at.ready = 1;
}

static void atlas_drop_shelves(void)
{
    int i;
    Shelf *s = at.shelves;
    while (s) {
        Shelf *next = s->next;
        free(s);
        s = next;
    }
    at.shelves = NULL;
    at.next_shelf_y = 0;
    for (i = 0; i < HWR_ATLAS_MAX_SLOTS; i++) {
        free(at.pending[i]);
        at.pending[i] = NULL;
    }
}

/* Pure lookup — returns slot if key exists, -1 if not found,
 * -2 if the key was blacklisted (atlas full, don't retry). */
int hwr_atlas_find(uint32_t key)
{
    uint32_t idx;
    atlas_init_hash();
    if (atlas_blacklisted(key))
        return -2;
    idx = atlas_hash(key) % HWR_ATLAS_MAX_SLOTS;
    {
        uint32_t probe = 0;
        while (at.hash[idx].slot >= 0) {
            if (at.hash[idx].key == key)
                return at.hash[idx].slot;
            probe++;
            idx = (idx + 1) % HWR_ATLAS_MAX_SLOTS;
            if (probe >= HWR_ATLAS_MAX_SLOTS)
                return -1;
        }
    }
    return -1;
}

/* Border width reserved around every packed sprite to stop GL_LINEAR
 * filtering from sampling a neighbouring sprite's texels at the seam. */
#define ATLAS_PAD 1

/* Build a (w+2*PAD) x (h+2*PAD) RGBA copy of pixels with the outer ring
 * clamped from the sprite's own edge pixels, for a bleed-proof upload. */
static uint8_t *atlas_build_padded(const uint8_t *pixels, int w, int h)
{
    int box_w = w + 2 * ATLAS_PAD, box_h = h + 2 * ATLAS_PAD;
    uint8_t *dst = (uint8_t *)malloc((size_t)box_w * box_h * 4);
    int x, y;
    if (!dst)
        return NULL;
    for (y = 0; y < box_h; y++) {
        int sy = y - ATLAS_PAD;
        if (sy < 0) sy = 0;
        if (sy >= h) sy = h - 1;
        for (x = 0; x < box_w; x++) {
            int sx = x - ATLAS_PAD;
            if (sx < 0) sx = 0;
            if (sx >= w) sx = w - 1;
            memcpy(dst + ((size_t)y * box_w + x) * 4,
                   pixels + ((size_t)sy * w + sx) * 4, 4);
        }
    }
    return dst;
}

int hwr_atlas_register(uint32_t key, const uint8_t *pixels, int w, int h)
{
    uint32_t idx;
    int slot;
    Shelf *s, *best;
    int box_w = w + 2 * ATLAS_PAD;
    int box_h = h + 2 * ATLAS_PAD;

    if (w <= 0 || h <= 0 || box_w > HWR_ATLAS_W || box_h > HWR_ATLAS_H) {
        atlas_blacklist_add(key);
        return -1;
    }

    /* Ensure hash table is initialised (safe, no GL calls) */
    atlas_init_hash();

    /* Check hash table for existing entry */
    idx = atlas_hash(key) % HWR_ATLAS_MAX_SLOTS;
    {
        uint32_t probe = 0;
        while (at.hash[idx].slot >= 0) {
            if (at.hash[idx].key == key)
                return at.hash[idx].slot;
            probe++;
            idx = (idx + 1) % HWR_ATLAS_MAX_SLOTS;
            if (probe >= HWR_ATLAS_MAX_SLOTS) {
                atlas_blacklist_add(key);
                return -1;
            }
        }
    }

    if (at.slot_count >= HWR_ATLAS_MAX_SLOTS) {
        atlas_blacklist_add(key);
        return -1;
    }

    /* Find best-fit shelf (first-fit with smallest remainder), sized against
     * the padded box so the border never overlaps a neighbouring sprite. */
    best = NULL;
    for (s = at.shelves; s; s = s->next) {
        if (s->h >= box_h && s->w >= box_w) {
            if (!best || s->h < best->h || (s->h == best->h && s->w < best->w))
                best = s;
        }
    }

    if (best) {
        int bx = best->x, by = best->y;
        slot = at.slot_count;
        at.slot_u0[slot] = (float)(bx + ATLAS_PAD) / (float)HWR_ATLAS_W;
        at.slot_v0[slot] = (float)(by + ATLAS_PAD) / (float)HWR_ATLAS_H;
        at.slot_u1[slot] = (float)(bx + ATLAS_PAD + w) / (float)HWR_ATLAS_W;
        at.slot_v1[slot] = (float)(by + ATLAS_PAD + h) / (float)HWR_ATLAS_H;
        at.slot_w[slot] = w;
        at.slot_h[slot] = h;
        at.slot_box_x[slot] = bx;
        at.slot_box_y[slot] = by;
        at.slot_box_w[slot] = box_w;
        at.slot_box_h[slot] = box_h;

        /* Stash padded RGBA pixels for deferred GL upload by the renderer thread */
        if (pixels != NULL) {
            free(at.pending[slot]);
            at.pending[slot] = atlas_build_padded(pixels, w, h);
        }

        /* Shrink the shelf */
        best->x += box_w;
        best->w -= box_w;

        at.hash[idx].key = key;
        at.hash[idx].slot = slot;
        at.slot_count++;
        return slot;
    }

    /* No shelf fits: start a new shelf at next_shelf_y */
    if (at.next_shelf_y + box_h > HWR_ATLAS_H) {
        atlas_blacklist_add(key);
        return -1;
    }

    s = (Shelf *)malloc(sizeof(Shelf));
    s->x = box_w;
    s->y = at.next_shelf_y;
    s->w = HWR_ATLAS_W - box_w;
    s->h = box_h;
    s->next = at.shelves;
    at.shelves = s;

    slot = at.slot_count;
    at.slot_u0[slot] = (float)ATLAS_PAD / (float)HWR_ATLAS_W;
    at.slot_v0[slot] = (float)(s->y + ATLAS_PAD) / (float)HWR_ATLAS_H;
    at.slot_u1[slot] = (float)(ATLAS_PAD + w) / (float)HWR_ATLAS_W;
    at.slot_v1[slot] = (float)(s->y + ATLAS_PAD + h) / (float)HWR_ATLAS_H;
    at.slot_w[slot] = w;
    at.slot_h[slot] = h;
    at.slot_box_x[slot] = 0;
    at.slot_box_y[slot] = s->y;
    at.slot_box_w[slot] = box_w;
    at.slot_box_h[slot] = box_h;

    /* Stash padded RGBA pixels for deferred GL upload by the renderer thread */
        if (pixels != NULL) {
            free(at.pending[slot]);
            at.pending[slot] = atlas_build_padded(pixels, w, h);
        }

        at.next_shelf_y += box_h;

    at.hash[idx].key = key;
    at.hash[idx].slot = slot;
    at.slot_count++;
    return slot;
}

void hwr_atlas_uv(int slot, float *u0, float *v0, float *u1, float *v1)
{
    if (slot < 0 || slot >= at.slot_count) {
        *u0 = *v0 = 0.0f; *u1 = *v1 = 1.0f;
        return;
    }
    *u0 = at.slot_u0[slot]; *v0 = at.slot_v0[slot];
    *u1 = at.slot_u1[slot]; *v1 = at.slot_v1[slot];
}

void hwr_atlas_size(int slot, int *w, int *h)
{
    if (slot < 0 || slot >= at.slot_count) {
        *w = *h = 0;
        return;
    }
    *w = at.slot_w[slot];
    *h = at.slot_h[slot];
}

void hwr_atlas_upload_pending(void)
{
    int i;
    if (!at.ready)
        atlas_init_gl();
    glBindTexture(GL_TEXTURE_2D, at.tex);
    for (i = 0; i < at.slot_count; i++) {
        if (at.pending[i] != NULL) {
            /* Upload the padded box (sprite + 1px clamped border), not just
             * the inner sprite rect the UVs point at. */
            glTexSubImage2D(GL_TEXTURE_2D, 0, at.slot_box_x[i], at.slot_box_y[i],
                            at.slot_box_w[i], at.slot_box_h[i],
                            GL_RGBA, GL_UNSIGNED_BYTE, at.pending[i]);
            free(at.pending[i]);
            at.pending[i] = NULL;
        }
    }
}

void hwr_atlas_bind(int unit)
{
    if (!at.ready)
        atlas_init_gl();
    glActiveTexture((GLenum)((int)GL_TEXTURE0 + unit));
    glBindTexture(GL_TEXTURE_2D, at.tex);
}

/* Atlas slots of the procedural glow tiles (white / red / blue), see
 * hwr_atlas_glow_slot. Cached so spr_build can inset their UVs and avoid
 * atlas-neighbour bleed (these are the only full-alpha tiles, so unlike the
 * alpha-keyed sprites their edges would otherwise sample adjacent atlas content
 * and fringe with stray colour). */
static int spr_glow_slot[3] = { -1, -1, -1 };

static int spr_is_glow_slot(int slot)
{
    return slot >= 0 && (slot == spr_glow_slot[0] || slot == spr_glow_slot[1]
                      || slot == spr_glow_slot[2]);
}

int hwr_atlas_glow_slot(int variant)
{
    /* Reserved atlas keys for the procedural glow tiles (0xFFFFFFFF is the
     * blacklist sentinel). variant: 0 = white, 1 = red, 2 = blue. */
    static const uint32_t GLOW_KEY[3] =
        { 0xFFFFFFFEu, 0xFFFFFFFDu, 0xFFFFFFFCu };
    int slot;
    if (variant < 0 || variant > 2) variant = 0;
    slot = hwr_atlas_find(GLOW_KEY[variant]);
    if (slot >= 0)
        return (spr_glow_slot[variant] = slot);
    if (slot == -2)
        return -1;   /* blacklisted (atlas full) */
    {
        enum { GW = 64 };
        uint8_t *px = (uint8_t *)malloc((size_t)GW * GW * 4);
        int x, y;
        /* RGB channel scale for white/red/blue. */
        float cr = (variant == 1) ? 1.0f : (variant == 2 ? 0.15f : 1.0f);
        float cg = (variant == 0) ? 1.0f : 0.15f;
        float cb = (variant == 2) ? 1.0f : (variant == 1 ? 0.15f : 1.0f);
        /* Siren glows brighter than the plain white glow; blue is pushed harder
         * than red since it reads perceptually dimmer and washes out additively. */
        HwrLightDefaults gld = hwr_lights_defaults();
        float vi = (variant == 0) ? gld.glare_headlamp_alpha
                 : (variant == 2) ? gld.glare_blue_alpha
                 : gld.glare_red_alpha;
        if (px == NULL)
            return -1;
        for (y = 0; y < GW; y++) {
            for (x = 0; x < GW; x++) {
                float dx = ((float)x + 0.5f) / GW * 2.0f - 1.0f;
                float dy = ((float)y + 0.5f) / GW * 2.0f - 1.0f;
                float d = sqrtf(dx * dx + dy * dy);          /* 0 centre .. ~1.41 corner */
                float f = 1.0f - d;
                if (f < 0.0f) f = 0.0f;
                f = f * f;                                    /* soft falloff */
                f *= 0.075f * vi;                             /* glow intensity */
                {
                    uint8_t *p = &px[(y * GW + x) * 4];
                    /* Additive blend means the dark edges add nothing. Full alpha
                     * so the alpha-test (atex.a < 0.5) never discards. */
                    p[0] = (uint8_t)(f * cr * 255.0f + 0.5f);
                    p[1] = (uint8_t)(f * cg * 255.0f + 0.5f);
                    p[2] = (uint8_t)(f * cb * 255.0f + 0.5f);
                    p[3] = 255;
                }
            }
        }
        slot = hwr_atlas_register(GLOW_KEY[variant], px, GW, GW);
        free(px);
        spr_glow_slot[variant] = slot;
        return slot;
    }
}

void hwr_atlas_reset(void)
{
    atlas_drop_shelves();
    memset(at.hash, 0xFF, sizeof(at.hash));
    at.slot_count = 0;
    at.hash_ready = 1;   /* hash table is valid (memset above) */
    if (at.tex)
        glDeleteTextures(1, &at.tex);
    at.tex = 0;
    at.ready = 0;
}

/* =========================================================================
 * Palette recolour (infrared/thermal view, brightness changes)
 * =========================================================================
 * The atlas is a cache keyed by sprite identity, so a tile's pixels are frozen
 * at whatever palette was live when it was first baked. The game does swap the
 * palette mid-mission — thermal/infrared view loads pal3, brightness changes
 * reload it — and the floor/face passes follow instantly because they store
 * palette INDICES and depalettise in the shader. Cached sprite tiles cannot:
 * they hold RGB, so they kept the colours of whichever palette happened to be
 * live when they were baked (only sprites seen for the very first time during
 * thermal came out thermal-coloured — the "odd frame that changes and then
 * lingers").
 *
 * Re-baking the whole atlas on every palette swap would stall, so instead
 * tiles are always baked against ONE frozen palette (the bake palette, see
 * sw_bake_palette() in source_sw.c) and the shader maps their colours back
 * through it at draw time:
 *
 *     baked RGB --uInvPal--> palette index --uPalLive--> live palette RGB
 *
 * uInvPal is a 64^3 lookup of "nearest bake-palette index" for a colour. The
 * game palette is 6 bits per channel, so every exact palette colour lands in
 * its own cell and the round trip is lossless; the cells in between (reached
 * only by xBR-blended edge pixels, which the software renderer never had) are
 * filled by a flood fill outwards from the seeded cells. */

#define INVPAL_DIM 64

static uint8_t at_bake_pal[768];    /* palette the atlas tiles were baked with */
static uint8_t at_live_pal[768];    /* palette the game is displaying now */
static int     at_bake_pal_ready = 0;
static int     at_recolour = 0;     /* live palette differs from the bake one */
static GLuint  at_invpal_tex = 0;   /* 64^3 R8: colour -> bake palette index */
static GLuint  at_pallive_tex = 0;  /* 256x1 RGB8: the live palette */
static int     at_invpal_dirty = 1;
static int     at_pallive_dirty = 1;

void hwr_atlas_set_palettes(const uint8_t *bake_pal, const uint8_t *live_pal)
{
    if (bake_pal == NULL || live_pal == NULL)
        return;
    if (!at_bake_pal_ready || memcmp(at_bake_pal, bake_pal, 768) != 0) {
        memcpy(at_bake_pal, bake_pal, 768);
        at_bake_pal_ready = 1;
        at_invpal_dirty = 1;
    }
    if (memcmp(at_live_pal, live_pal, 768) != 0) {
        memcpy(at_live_pal, live_pal, 768);
        at_pallive_dirty = 1;
    }
    at_recolour = (memcmp(at_bake_pal, at_live_pal, 768) != 0);
}

/* Build the inverse-palette volume from the bake palette. Renderer thread. */
static void invpal_build(void)
{
    const int D = INVPAL_DIM;
    const int N = D * D * D;
    static const int nb[6][3] = {
        {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1}
    };
    uint8_t  *lut    = (uint8_t *)malloc((size_t)N);
    uint8_t  *filled = (uint8_t *)calloc(1, (size_t)N);
    uint32_t *queue  = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)N);
    int head = 0, tail = 0, i;

    if (lut == NULL || filled == NULL || queue == NULL) {
        free(lut); free(filled); free(queue);
        return;
    }
    memset(lut, 0, (size_t)N);

    /* Seed one cell per palette entry. Duplicate colours: first index wins. */
    for (i = 0; i < 256; i++) {
        int r = at_bake_pal[i * 3 + 0] >> 2;
        int g = at_bake_pal[i * 3 + 1] >> 2;
        int b = at_bake_pal[i * 3 + 2] >> 2;
        int c = (b * D + g) * D + r;
        if (filled[c])
            continue;
        filled[c] = 1;
        lut[c] = (uint8_t)i;
        queue[tail++] = (uint32_t)c;
    }
    /* Flood the rest: 6-neighbour BFS, so each cell inherits the index of its
     * nearest seed by city-block distance. */
    while (head < tail) {
        int c = (int)queue[head++];
        int r = c % D, g = (c / D) % D, b = c / (D * D);
        int k;
        for (k = 0; k < 6; k++) {
            int nr = r + nb[k][0], ng = g + nb[k][1], nbz = b + nb[k][2];
            int nc;
            if (nr < 0 || nr >= D || ng < 0 || ng >= D || nbz < 0 || nbz >= D)
                continue;
            nc = (nbz * D + ng) * D + nr;
            if (filled[nc])
                continue;
            filled[nc] = 1;
            lut[nc] = lut[c];
            queue[tail++] = (uint32_t)nc;
        }
    }

    if (at_invpal_tex == 0)
        glGenTextures(1, &at_invpal_tex);
    glActiveTexture(GL_TEXTURE6);
    glBindTexture(GL_TEXTURE_3D, at_invpal_tex);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage3D(GL_TEXTURE_3D, 0, GL_R8, D, D, D, 0, GL_RED,
                 GL_UNSIGNED_BYTE, lut);
    glActiveTexture(GL_TEXTURE0);

    free(lut); free(filled); free(queue);
    at_invpal_dirty = 0;
}

/* Bind the recolour uniforms/textures for a program that samples the atlas.
 * Returns nonzero if the recolour is active this frame. */
static int spr_bind_recolour(GLint loc_invpal, GLint loc_pallive, GLint loc_on)
{
    int on = at_recolour && at_bake_pal_ready;

    if (loc_on >= 0)
        glUniform1i(loc_on, on);
    if (!on)
        return 0;

    if (at_invpal_dirty)
        invpal_build();
    if (at_invpal_tex == 0)
        return 0;

    glActiveTexture(GL_TEXTURE6);
    glBindTexture(GL_TEXTURE_3D, at_invpal_tex);

    if (at_pallive_tex == 0) {
        glGenTextures(1, &at_pallive_tex);
        at_pallive_dirty = 1;
    }
    glActiveTexture(GL_TEXTURE7);
    glBindTexture(GL_TEXTURE_2D, at_pallive_tex);
    if (at_pallive_dirty) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, 256, 1, 0, GL_RGB,
                     GL_UNSIGNED_BYTE, at_live_pal);
        at_pallive_dirty = 0;
    }
    glActiveTexture(GL_TEXTURE0);

    if (loc_invpal >= 0)
        glUniform1i(loc_invpal, 6);
    if (loc_pallive >= 0)
        glUniform1i(loc_pallive, 7);
    return 1;
}

/* GLSL helper shared by every program that samples the atlas. */
#define RECOLOUR_GLSL \
    "uniform sampler3D uInvPal;\n" \
    "uniform sampler2D uPalLive;\n" \
    "uniform int uRecolour;\n" \
    "vec3 atlas_recolour(vec3 c) {\n" \
    "    if (uRecolour == 0) return c;\n" \
    /* Round to the 8-bit value first: c*255 alone lands just under the integer
     * for some texels and would drop a whole cell (252 -> 62 instead of 63). */ \
    "    vec3 v8 = floor(clamp(c, 0.0, 1.0) * 255.0 + 0.5);\n" \
    "    vec3 cell = (floor(v8 * 0.25) + 0.5) / 64.0;\n" \
    "    float idx = floor(texture(uInvPal, cell).r * 255.0 + 0.5);\n" \
    "    return texture(uPalLive, vec2((idx + 0.5) / 256.0, 0.5)).rgb;\n" \
    "}\n"

/* =========================================================================
 * Billboard shaders
 * =========================================================================
 * Vertex shader reproduces transform_shpoint() exactly (same as floor_vert_src)
 * on pre-computed world-space corner positions. Fragment shader samples the
 * atlas, alpha-tests index 0, depalettises, and evaluates point lights + sun
 * shadow (same lighting code as floor_frag_src).
 * ========================================================================*/

static const char *spr_vert_src =
    "#version 330 core\n"
    "layout(location=0) in vec3 aPos;\n"        /* world corner */
    "layout(location=1) in vec2 aUV;\n"          /* atlas UV */
    "layout(location=2) in float aShade;\n"      /* brightness 0..1 */
    "layout(location=3) in float aDepth;\n"      /* centre scrd (uniform across quad) */
    "uniform float uD10, uD14, uD18, uD1C;\n"
    "uniform float uScale;\n"
    "uniform vec2 uCentre;\n"
    "uniform vec3 uCtr;\n"
    "uniform int  uPersp;\n"
    "out vec2 vUV;\n"
    "out vec3 vWorldPos;\n"
    "out float vShade;\n"
    "out float vScrd;\n"
    "void main(){\n"
    "    float dx = aPos.x - uCtr.x;\n"
    "    float dy = aPos.y - uCtr.y;\n"
    "    float dz = aPos.z - uCtr.z;\n"
    "    float fa = (uD14*dx - uD10*dz) / 65536.0;\n"
    "    float fb = (uD10*dx + uD14*dz) / 65536.0;\n"
    "    float fc = (uD1C*dy - uD18*fb) / 65536.0;\n"
    "    float scrd = (uD18*dy + uD1C*fb) / 65536.0;\n"
    "    if (uPersp == 5 && scrd > 1024.0)\n"
    "        scrd = 16384.0*scrd/(scrd + 16384.0);\n"
    "    float shx = uScale*fa / 2048.0;\n"
    "    float shy = uScale*fc / 2048.0;\n"
    "    if (uPersp == 5) {\n"
    "        shx = shx*(16384.0 - scrd) / 16384.0;\n"
    "        shy = shy*(16384.0 - scrd) / 16384.0;\n"
    "    }\n"
    "    float sx = uCentre.x + shx;\n"
    "    float sy = uCentre.y - shy;\n"
    "    vUV = aUV;\n"
    "    vWorldPos = aPos;\n"
    "    vShade = aShade;\n"
    "    vScrd = scrd;\n"
    "    /* Depth uses centre scrd (aDepth, uniform across quad) to prevent\n"
    "     * floor from clipping one half of the sprite.  Only a SMALL forward\n"
    "     * epsilon here: a large one (this was 512) makes a person or crate\n"
    "     * standing just behind a wall or vehicle draw in front of it, since\n"
    "     * faces sit at their true depth. The headroom that flat-lying sprites\n"
    "     * (corpses, dropped items) need against the floor comes from pushing\n"
    "     * the FLOOR pass away instead - see HWR_FLOOR_DEPTH_PUSHBACK in\n"
    "     * hwr_floor.c; the two sum to the old 512-unit floor margin. */\n"
    "    float ndc_z = clamp((aDepth - 64.0) / 65536.0, -1.0, 1.0);\n"
    "    gl_Position = vec4(sx/uCentre.x - 1.0, 1.0 - sy/uCentre.y, ndc_z, 1.0);\n"
    "}\n";

static const char *spr_frag_src =
    "#version 330 core\n"
    "in vec2 vUV;\n"
    "in vec3 vWorldPos;\n"
    "in float vShade;\n"
    "in float vScrd;\n"
    "layout(location=0) out vec4 frag;\n"
    "layout(location=1) out vec4 fragPos;\n"
    "uniform sampler2D uAtlas;\n"       /* RGBA8 sprite pixels (unit 4) */
    "uniform sampler2D uShadowMap;\n"   /* depth from sun (unit 3) */
    "uniform vec3  uLightPos[64];\n"
    "uniform vec3  uLightRgb[64];\n"
    "uniform float uLightRadius[64];\n"
    "uniform int   uNumLights;\n"
    "uniform float uAmbient;\n"
    "uniform float uGain;\n"
    "uniform vec3  uTint;\n"
    "uniform float uAO;\n"
    "uniform float uLightMaxDist2[64];\n"
    "uniform mat4  uSunMVP;\n"
    "uniform float uSunBright;\n"
    "uniform float uSunAmbient;\n"
    "uniform float uSunBias;\n"
    "uniform int   uSunEnable;\n"
    "uniform int   uSunPCF;\n"
    "uniform int   uSunDebug;\n"
    "uniform float uSunHaze;\n"
    "uniform float uAlpha;\n"            /* output alpha (1 = opaque; <1 = translucent) */
    "uniform int  uUnlit;\n"             /* 1 = self-lit (ignore scene lights) — effects */
    RECOLOUR_GLSL
    "void main(){\n"
    "    fragPos = vec4(vWorldPos, -1.0);\n"  /* w: 1=water, 0=3D geom, -1=sprite/billboard */
    "    vec4 atex = texture(uAtlas, vUV);\n"
    "    if (atex.a < 0.5) discard;\n"
    /* Remap the tile's frozen bake-palette colours to the live palette, so
     * infrared/thermal view (and brightness changes) recolour cached sprites
     * exactly like the software renderer's palette swap. */
    "    vec3 c = atlas_recolour(atex.rgb);\n"
    "    /* Effects (smoke/fire/glow) are self-lit like the software renderer: the\n"
    "     * baked colour at full brightness, with the per-sprite shade used as an\n"
    "     * ALPHA multiplier so particles can fade out over their life. */\n"
    "    if (uUnlit == 1) { frag = vec4(c, uAlpha * clamp(vShade, 0.0, 1.0)); return; }\n"
    "    vec3 light_col = vec3(0.0);\n"
    "    float shadow = 0.0;\n"
    "    for (int i = 0; i < uNumLights; i++) {\n"
    "        float r = uLightRadius[i];\n"
    "        if (r == 0.0) continue;\n"
    "        vec3 delta = vWorldPos - uLightPos[i];\n"
    "        float dist2 = delta.x*delta.x + delta.z*delta.z + delta.y*delta.y;\n"
    "        float nd = dist2 / uLightMaxDist2[i];\n"
    "        if (nd >= 1.0) continue;\n"
    "        float brightness = 1.0 - sqrt(nd);\n"
    "        if (r > 0.0)\n"
    "            light_col += uLightRgb[i] * brightness;\n"
    "        else\n"
    "            shadow += uLightRgb[i].x * brightness;\n"
    "    }\n"
    "    float base = uAmbient;\n"
    "    if (uSunEnable == 1) {\n"
    "        vec4 sc = uSunMVP * vec4(vWorldPos, 1.0);\n"
    "        vec3 p = sc.xyz / sc.w * 0.5 + 0.5;\n"
    "        float lit = 1.0;\n"
    "        if (p.x >= 0.0 && p.x <= 1.0 && p.y >= 0.0 && p.y <= 1.0 && p.z >= 0.0 && p.z <= 1.0) {\n"
    "            float texel = 1.0 / 2048.0;\n"
    "            float halfK = float(uSunPCF);\n"
    "            float sigma = halfK * 0.5 + 1.0;\n"
    "            float sum_w = 0.0, sum_lit = 0.0;\n"
    "            for (int sx = -uSunPCF; sx <= uSunPCF; sx++) {\n"
    "                for (int sy = -uSunPCF; sy <= uSunPCF; sy++) {\n"
    "                    float dsq = float(sx*sx + sy*sy);\n"
    "                    float w = exp(-dsq / (2.0 * sigma * sigma));\n"
    "                    float closest = texture(uShadowMap, p.xy + vec2(float(sx),float(sy))*texel).r;\n"
    "                    sum_lit += w * ((p.z - uSunBias > closest) ? 0.0 : 1.0);\n"
    "                    sum_w += w;\n"
    "                }\n"
    "            }\n"
    "            lit = sum_lit / sum_w;\n"
    "            lit = mix(lit, 1.0, uSunHaze);\n"
    "        }\n"
    "        if (uSunDebug == 1) { frag = vec4(vec3(lit) * vShade, uAlpha); return; }\n"
    "        base += uSunAmbient + uSunBright * lit;\n"
    "    }\n"
    "    light_col = light_col * uGain * uTint + base;\n"
    "    light_col *= clamp(1.0 - shadow, 0.0, 1.0);\n"
    "    /* SW parity: the per-thing Brightness (vShade) ALREADY contains the\n"
    "     * scene lighting - SW computes it per turn from nearby lights\n"
    "     * (process_lighting) and draws sprites at exactly c * Brightness.\n"
    "     * Adding scene lamps/ambient on top double-counts and channel-clamps\n"
    "     * bright texels (washed-out trees/props). Pin light_col to 1.0. */\n"
    "    light_col = vec3(1.0);\n"
    "    frag = vec4(c * light_col * vShade, uAlpha);\n"
    "}\n";

/* Shadow blob shader: simple radial gradient on the ground */
static const char *shadow_vert_src =
    "#version 330 core\n"
    "layout(location=0) in vec3 aPos;\n"
    "layout(location=1) in vec2 aUV;\n"
    "uniform float uD10, uD14, uD18, uD1C;\n"
    "uniform float uScale;\n"
    "uniform vec2 uCentre;\n"
    "uniform vec3 uCtr;\n"
    "uniform int  uPersp;\n"
    "out vec2 vUV;\n"
    "void main(){\n"
    "    float dx = aPos.x - uCtr.x;\n"
    "    float dy = aPos.y - uCtr.y;\n"
    "    float dz = aPos.z - uCtr.z;\n"
    "    float fa = (uD14*dx - uD10*dz) / 65536.0;\n"
    "    float fb = (uD10*dx + uD14*dz) / 65536.0;\n"
    "    float fc = (uD1C*dy - uD18*fb) / 65536.0;\n"
    "    float scrd = (uD18*dy + uD1C*fb) / 65536.0;\n"
    "    float scrd_raw = scrd;\n"  /* raw depth, matches CPU face_scrd() */
    "    if (uPersp == 5 && scrd > 1024.0)\n"
    "        scrd = 16384.0*scrd/(scrd + 16384.0);\n"
    "    float shx = uScale*fa / 2048.0;\n"
    "    float shy = uScale*fc / 2048.0;\n"
    "    if (uPersp == 5) {\n"
    "        shx = shx*(16384.0 - scrd) / 16384.0;\n"
    "        shy = shy*(16384.0 - scrd) / 16384.0;\n"
    "    }\n"
    "    float ndc_z = clamp(scrd_raw / 65536.0 - 0.003, -1.0, 1.0);\n"  /* bias so it
                                        reliably beats the floor's own depth despite
                                        per-frame animation jitter in the sprite's
                                        reported y/height (ground_y wobbles a little
                                        each walk-cycle frame even with a still camera).
                                        Most of the margin over the floor now comes
                                        from HWR_FLOOR_DEPTH_PUSHBACK (hwr_floor.c) pushing
                                        the floor pass away instead, so this stays small and
                                        the shadow does not bleed onto walls standing on the
                                        same tile; the two still sum to the ~655-unit floor
                                        separation that stopped the 60fps flicker. */
    "    gl_Position = vec4((uCentre.x + shx)/uCentre.x - 1.0, 1.0 - (uCentre.y - shy)/uCentre.y, ndc_z, 1.0);\n"
    "    vUV = aUV;\n"
    "}\n";

static const char *shadow_frag_src =
    "#version 330 core\n"
    "in vec2 vUV;\n"
    "out vec4 frag;\n"
    "void main(){\n"
    "    float d = length(vUV - 0.5) * 2.0;\n"
    "    float a = clamp(1.0 - d, 0.0, 1.0);\n"
    "    a = a * a * 0.25;\n"
    "    frag = vec4(0.0, 0.0, 0.0, a);\n"
    "}\n";

/* Projected shape-shadow shader: draws the sprite's silhouette on the ground
 * offset in the light direction. Opacity per-quad for distance fade. */
static const char *psh_vert_src =
    "#version 330 core\n"
    "layout(location=0) in vec3 aPos;\n"
    "layout(location=1) in vec2 aUV;\n"
    "layout(location=2) in float aOpacity;\n"
    "uniform float uD10, uD14, uD18, uD1C;\n"
    "uniform float uScale;\n"
    "uniform vec2 uCentre;\n"
    "uniform vec3 uCtr;\n"
    "uniform int  uPersp;\n"
    "out vec2 vUV;\n"
    "out float vOpacity;\n"
    "void main(){\n"
    "    float dx = aPos.x - uCtr.x;\n"
    "    float dy = aPos.y - uCtr.y;\n"
    "    float dz = aPos.z - uCtr.z;\n"
    "    float fa = (uD14*dx - uD10*dz) / 65536.0;\n"
    "    float fb = (uD10*dx + uD14*dz) / 65536.0;\n"
    "    float fc = (uD1C*dy - uD18*fb) / 65536.0;\n"
    "    float scrd = (uD18*dy + uD1C*fb) / 65536.0;\n"
    "    float scrd_raw = scrd;\n"  /* raw depth, matches CPU face_scrd() */
    "    if (uPersp == 5 && scrd > 1024.0)\n"
    "        scrd = 16384.0*scrd/(scrd + 16384.0);\n"
    "    float shx = uScale*fa / 2048.0;\n"
    "    float shy = uScale*fc / 2048.0;\n"
    "    if (uPersp == 5) {\n"
    "        shx = shx*(16384.0 - scrd) / 16384.0;\n"
    "        shy = shy*(16384.0 - scrd) / 16384.0;\n"
    "    }\n"
    "    float ndc_z = clamp(scrd_raw / 65536.0 - 0.003, -1.0, 1.0);\n"  /* bias so it
                                        reliably beats the floor's own depth despite
                                        per-frame animation jitter in the sprite's
                                        reported y/height (ground_y wobbles a little
                                        each walk-cycle frame even with a still camera).
                                        Most of the margin over the floor now comes
                                        from HWR_FLOOR_DEPTH_PUSHBACK (hwr_floor.c) pushing
                                        the floor pass away instead, so this stays small and
                                        the shadow does not bleed onto walls standing on the
                                        same tile; the two still sum to the ~655-unit floor
                                        separation that stopped the 60fps flicker. */
    "    gl_Position = vec4((uCentre.x + shx)/uCentre.x - 1.0, 1.0 - (uCentre.y - shy)/uCentre.y, ndc_z, 1.0);\n"
    "    vUV = aUV;\n"
    "    vOpacity = aOpacity;\n"
    "}\n";

static const char *psh_frag_src =
    "#version 330 core\n"
    "in vec2 vUV;\n"
    "in float vOpacity;\n"
    "out vec4 frag;\n"
    "uniform sampler2D uAtlas;\n"
    "void main(){\n"
    "    vec4 t = texture(uAtlas, vUV);\n"
    "    float a = t.a > 0.3 ? vOpacity : 0.0;\n"
    "    frag = vec4(0.0, 0.0, 0.0, a);\n"
    "}\n";

/* =========================================================================
 *  Shader compilation / program creation
 * ========================================================================= */

static GLuint spr_prog = 0, spr_vao = 0, spr_vbo = 0, spr_ebo = 0;
static GLint spr_loc_atlas = -1, spr_loc_shadowmap = -1;
static GLint spr_loc_d10 = -1, spr_loc_d14 = -1, spr_loc_d18 = -1, spr_loc_d1c = -1;
static GLint spr_loc_scale = -1, spr_loc_centre = -1, spr_loc_ctr = -1, spr_loc_persp = -1;
static GLint spr_loc_lpos_base = -1, spr_loc_lrgb_base = -1, spr_loc_lrad_base = -1;
static GLint spr_loc_nlights = -1, spr_loc_ambient = -1, spr_loc_gain = -1;
static GLint spr_loc_tint = -1, spr_loc_ao = -1, spr_loc_maxdist2 = -1;
static GLint spr_loc_sun_mvp = -1, spr_loc_sun_bright = -1, spr_loc_sun_ambient = -1;
static GLint spr_loc_sun_bias = -1, spr_loc_sun_enable = -1, spr_loc_sun_pcf = -1;
static GLint spr_loc_sun_debug = -1, spr_loc_sun_haze = -1;
static GLint spr_loc_alpha = -1;
static GLint spr_loc_invpal = -1, spr_loc_pallive = -1, spr_loc_recolour = -1;
static GLint spr_loc_unlit = -1;
static int   spr_ready = 0;

/* Translucent sprite pass config (Phase 8). */
static int   spr_tr_enable = 1;
static float spr_tr_alpha  = 1.0f;

static GLuint shd_prog = 0, shd_vao = 0, shd_vbo = 0, shd_ebo = 0;
static GLint shd_loc_d10 = -1, shd_loc_d14 = -1, shd_loc_d18 = -1, shd_loc_d1c = -1;
static GLint shd_loc_scale = -1, shd_loc_centre = -1, shd_loc_ctr = -1, shd_loc_persp = -1;
static int   shd_ready = 0;

/* Projected shape-shadow program (atlas-textured shadows on the ground). */
static GLuint psh_prog = 0, psh_vao = 0, psh_vbo = 0, psh_ebo = 0;
static GLint psh_loc_d10 = -1, psh_loc_d14 = -1, psh_loc_d18 = -1, psh_loc_d1c = -1;
static GLint psh_loc_scale = -1, psh_loc_centre = -1, psh_loc_ctr = -1, psh_loc_persp = -1;
static GLint psh_loc_atlas = -1;
static int   psh_ready = 0;

static GLuint spr_compile(GLenum type, const char *src)
{
    GLuint sh = glCreateShader(type);
    GLint ok = 0;
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(sh, sizeof(log), NULL, log);
        hwr_set_error("sprite shader compile failed: %s", log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static int spr_init(void)
{
    GLuint vs, fs;
    GLint ok = 0;

    vs = spr_compile(GL_VERTEX_SHADER, spr_vert_src);
    if (!vs) return HWR_ERROR;
    fs = spr_compile(GL_FRAGMENT_SHADER, spr_frag_src);
    if (!fs) { glDeleteShader(vs); return HWR_ERROR; }
    spr_prog = glCreateProgram();
    glAttachShader(spr_prog, vs);
    glAttachShader(spr_prog, fs);
    glLinkProgram(spr_prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    glGetProgramiv(spr_prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(spr_prog, sizeof(log), NULL, log);
        hwr_set_error("sprite program link failed: %s", log);
        return HWR_ERROR;
    }

    spr_loc_atlas      = glGetUniformLocation(spr_prog, "uAtlas");
    spr_loc_shadowmap  = glGetUniformLocation(spr_prog, "uShadowMap");
    spr_loc_d10        = glGetUniformLocation(spr_prog, "uD10");
    spr_loc_d14        = glGetUniformLocation(spr_prog, "uD14");
    spr_loc_d18        = glGetUniformLocation(spr_prog, "uD18");
    spr_loc_d1c        = glGetUniformLocation(spr_prog, "uD1C");
    spr_loc_scale      = glGetUniformLocation(spr_prog, "uScale");
    spr_loc_centre     = glGetUniformLocation(spr_prog, "uCentre");
    spr_loc_ctr        = glGetUniformLocation(spr_prog, "uCtr");
    spr_loc_persp      = glGetUniformLocation(spr_prog, "uPersp");
    spr_loc_lpos_base  = glGetUniformLocation(spr_prog, "uLightPos");
    spr_loc_lrgb_base  = glGetUniformLocation(spr_prog, "uLightRgb");
    spr_loc_lrad_base  = glGetUniformLocation(spr_prog, "uLightRadius");
    spr_loc_nlights    = glGetUniformLocation(spr_prog, "uNumLights");
    spr_loc_ambient    = glGetUniformLocation(spr_prog, "uAmbient");
    spr_loc_gain       = glGetUniformLocation(spr_prog, "uGain");
    spr_loc_tint       = glGetUniformLocation(spr_prog, "uTint");
    spr_loc_ao         = glGetUniformLocation(spr_prog, "uAO");
    spr_loc_maxdist2   = glGetUniformLocation(spr_prog, "uLightMaxDist2");
    spr_loc_sun_mvp    = glGetUniformLocation(spr_prog, "uSunMVP");
    spr_loc_sun_bright = glGetUniformLocation(spr_prog, "uSunBright");
    spr_loc_sun_ambient= glGetUniformLocation(spr_prog, "uSunAmbient");
    spr_loc_sun_bias   = glGetUniformLocation(spr_prog, "uSunBias");
    spr_loc_sun_enable = glGetUniformLocation(spr_prog, "uSunEnable");
    spr_loc_sun_pcf    = glGetUniformLocation(spr_prog, "uSunPCF");
    spr_loc_sun_debug  = glGetUniformLocation(spr_prog, "uSunDebug");
    spr_loc_sun_haze   = glGetUniformLocation(spr_prog, "uSunHaze");
    spr_loc_alpha      = glGetUniformLocation(spr_prog, "uAlpha");
    spr_loc_unlit      = glGetUniformLocation(spr_prog, "uUnlit");
    spr_loc_invpal     = glGetUniformLocation(spr_prog, "uInvPal");
    spr_loc_pallive    = glGetUniformLocation(spr_prog, "uPalLive");
    spr_loc_recolour   = glGetUniformLocation(spr_prog, "uRecolour");

    glGenVertexArrays(1, &spr_vao);
    glBindVertexArray(spr_vao);
    glGenBuffers(1, &spr_vbo);
    glGenBuffers(1, &spr_ebo);
    glBindBuffer(GL_ARRAY_BUFFER, spr_vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, spr_ebo);
    {
        GLsizei stride = 28;  /* 3 floats pos + 2 floats UV + 1 float shade + 1 float depth */
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void *)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, (void *)12);
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, stride, (void *)20);
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, stride, (void *)24);
    }
    glBindVertexArray(0);

    /* Shadow shader */
    vs = spr_compile(GL_VERTEX_SHADER, shadow_vert_src);
    if (!vs) return HWR_ERROR;
    fs = spr_compile(GL_FRAGMENT_SHADER, shadow_frag_src);
    if (!fs) { glDeleteShader(vs); return HWR_ERROR; }
    shd_prog = glCreateProgram();
    glAttachShader(shd_prog, vs);
    glAttachShader(shd_prog, fs);
    glLinkProgram(shd_prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    glGetProgramiv(shd_prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(shd_prog, sizeof(log), NULL, log);
        hwr_set_error("shadow shader link failed: %s", log);
        return HWR_ERROR;
    }
    shd_loc_d10   = glGetUniformLocation(shd_prog, "uD10");
    shd_loc_d14   = glGetUniformLocation(shd_prog, "uD14");
    shd_loc_d18   = glGetUniformLocation(shd_prog, "uD18");
    shd_loc_d1c   = glGetUniformLocation(shd_prog, "uD1C");
    shd_loc_scale = glGetUniformLocation(shd_prog, "uScale");
    shd_loc_centre= glGetUniformLocation(shd_prog, "uCentre");
    shd_loc_ctr   = glGetUniformLocation(shd_prog, "uCtr");
    shd_loc_persp = glGetUniformLocation(shd_prog, "uPersp");
    glGenVertexArrays(1, &shd_vao);
    glBindVertexArray(shd_vao);
    glGenBuffers(1, &shd_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, shd_vbo);
    glGenBuffers(1, &shd_ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, shd_ebo);
    {
        GLsizei stride = 20;
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void *)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, (void *)12);
    }
    glBindVertexArray(0);

    /* ---- Projected shape-shadow program ---- */
    vs = spr_compile(GL_VERTEX_SHADER, psh_vert_src);
    if (!vs) return HWR_ERROR;
    fs = spr_compile(GL_FRAGMENT_SHADER, psh_frag_src);
    if (!fs) { glDeleteShader(vs); return HWR_ERROR; }
    psh_prog = glCreateProgram();
    glAttachShader(psh_prog, vs);
    glAttachShader(psh_prog, fs);
    glLinkProgram(psh_prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    glGetProgramiv(psh_prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(psh_prog, sizeof(log), NULL, log);
        hwr_set_error("psh program link failed: %s", log);
        return HWR_ERROR;
    }
    psh_loc_d10   = glGetUniformLocation(psh_prog, "uD10");
    psh_loc_d14   = glGetUniformLocation(psh_prog, "uD14");
    psh_loc_d18   = glGetUniformLocation(psh_prog, "uD18");
    psh_loc_d1c   = glGetUniformLocation(psh_prog, "uD1C");
    psh_loc_scale = glGetUniformLocation(psh_prog, "uScale");
    psh_loc_centre= glGetUniformLocation(psh_prog, "uCentre");
    psh_loc_ctr   = glGetUniformLocation(psh_prog, "uCtr");
    psh_loc_persp = glGetUniformLocation(psh_prog, "uPersp");
    psh_loc_atlas = glGetUniformLocation(psh_prog, "uAtlas");
    glGenVertexArrays(1, &psh_vao);
    glBindVertexArray(psh_vao);
    glGenBuffers(1, &psh_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, psh_vbo);
    glGenBuffers(1, &psh_ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, psh_ebo);
    {
        GLsizei stride = 24;
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void *)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, (void *)12);
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, stride, (void *)20);
    }
    glBindVertexArray(0);
    psh_ready = 1;

    if (hwr_gl_check("spr_init"))
        return HWR_ERROR;
    spr_ready = 1;
    shd_ready = 1;
    return HWR_OK;
}

/* =========================================================================
 * Per-frame rendering
 * =========================================================================
 * Vertex format: 3 floats pos + 2 floats UV + 1 float shade + 1 float depth = 28 bytes.
 * Per billboard: 4 vertices + 6 indices (two triangles).
 * ========================================================================*/

#define SPR_VERT_STRIDE 28
/* Raised from 2048: heavy combat (many agents + a big explosion's ~1000 phwoar
 * smoke puffs + fire + glares) overflowed the old cap, so the tail of the smoke
 * fell back to the software renderer and flickered. Must match HWR_MAX_COLLECTED
 * in source_sw.c. */
#define SPR_MAX_BILLBOARDS 4096
#define SPR_MAX_VERTS (SPR_MAX_BILLBOARDS * 4)
#define SPR_MAX_INDEX (SPR_MAX_BILLBOARDS * 6)

static float   spr_vbuf[SPR_MAX_VERTS * (SPR_VERT_STRIDE / 4)];
static uint32_t spr_ibuf[SPR_MAX_INDEX];
static int      spr_vcount, spr_icount;
/* Shared scratch for get_sprites — file-scope (not stack) because the cap is now
 * large; the three sprite/shadow passes run sequentially, never reentrantly. */
static HwrBillboard spr_billboards[SPR_MAX_BILLBOARDS];

/* Emit a ground shadow quad — inline in hwr_sprites_render, not this helper */

/* Upload lights (same as fl_upload_lights) */
static void spr_upload_lights(const HwrLight *lights, int n)
{
    float pos_buf[64 * 3], rgb_buf[64 * 3], rad_buf[64], maxd2_buf[64];
    int i;
    if (n > 64) n = 64;
    {
        HwrLightDefaults d = hwr_lights_defaults();
        for (i = 0; i < n; i++) {
            pos_buf[i*3+0] = lights[i].x;
            pos_buf[i*3+1] = lights[i].y;
            pos_buf[i*3+2] = lights[i].z;
            rgb_buf[i*3+0] = lights[i].r;
            rgb_buf[i*3+1] = lights[i].g;
            rgb_buf[i*3+2] = lights[i].b;
            rad_buf[i]     = lights[i].radius;
            if (lights[i].max_dist2 > 0.0f)
                maxd2_buf[i] = lights[i].max_dist2;
            else {
                float yabs = (lights[i].y < 0.0f) ? -lights[i].y : lights[i].y;
                maxd2_buf[i] = d.max_light_dist2 + yabs * yabs;
            }
        }
        glUniform1f(spr_loc_ambient, d.ambient);
        glUniform1f(spr_loc_gain, d.intensity);
        glUniform3f(spr_loc_tint, d.tint_r, d.tint_g, d.tint_b);
        glUniform1f(spr_loc_ao, d.ao);
    }
    if (n > 0) {
        glUniform3fv(spr_loc_lpos_base, n, pos_buf);
        glUniform3fv(spr_loc_lrgb_base, n, rgb_buf);
        glUniform1fv(spr_loc_lrad_base, n, rad_buf);
        glUniform1fv(spr_loc_maxdist2, n, maxd2_buf);
    }
    glUniform1i(spr_loc_nlights, n);
}

/* Set the sprite program camera/lighting uniforms.
 * cam and source are from the current frame. */
static void spr_setup_program(const HwrCamera *cam,
    const HwrSceneSource *source)
{
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(0x0203); /* GL_LEQUAL */
    glDisable(GL_BLEND);

    glUseProgram(spr_prog);
    glUniform1f(spr_loc_alpha, 1.0f);   /* opaque; translucent pass overrides */
    glUniform1i(spr_loc_unlit, 0);      /* opaque sprites are scene-lit */
    glUniform1f(spr_loc_d10, cam->d10);
    glUniform1f(spr_loc_d14, cam->d14);
    glUniform1f(spr_loc_d18, cam->d18);
    glUniform1f(spr_loc_d1c, cam->d1c);
    glUniform1f(spr_loc_scale, cam->scale);
    glUniform2f(spr_loc_centre, cam->centre_x, cam->centre_y);
    glUniform3f(spr_loc_ctr, cam->cx, cam->cy8, cam->cz);
    glUniform1i(spr_loc_persp, cam->perspective);

    /* Atlas on unit 4 */
    hwr_atlas_bind(4);
    glUniform1i(spr_loc_atlas, 4);

    /* Live-palette remap of the atlas colours (units 6/7) — see
     * hwr_atlas_set_palettes(). No-op unless the palette has been swapped. */
    spr_bind_recolour(spr_loc_invpal, spr_loc_pallive, spr_loc_recolour);

    /* Shadow map on unit 3 */
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, (GLuint)hwr_sun_texture());
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(spr_loc_shadowmap, 3);

    /* Sun uniforms */
    {
        int sun_on = hwr_sun_enabled();
        glUniform1i(spr_loc_sun_enable, sun_on);
        if (sun_on) {
            glUniformMatrix4fv(spr_loc_sun_mvp, 1, GL_FALSE, hwr_sun_mvp());
            glUniform1f(spr_loc_sun_bright,  hwr_sun_bright());
            glUniform1f(spr_loc_sun_ambient, hwr_sun_ambient());
            glUniform1f(spr_loc_sun_bias,    hwr_sun_bias());
            glUniform1i(spr_loc_sun_pcf,     hwr_sun_pcf());
            glUniform1i(spr_loc_sun_debug,   hwr_sun_debug());
            glUniform1f(spr_loc_sun_haze,    hwr_sun_haze());
        }
    }

    /* Lights */
    {
        HwrLight lights[64];
        int nlight = (source && source->get_lights)
            ? source->get_lights(source->ctx, lights, 64) : 0;
        spr_upload_lights(lights, nlight < 0 ? 0 : nlight);
    }

    hwr_gl_check("spr_setup_program");
}

/* Billboard corners go through the exact same per-vertex "true 3D" correction
 * the floor/face vertex shaders apply — spr_vert_src's
 *   if (uPersp==5) shx = shx*(16384-scrd)/16384;
 * (scrd computed per-vertex from aPos, i.e. from each billboard corner's own
 * actual world position). That's correct and unavoidable geometry, and it's
 * why floor/faces (built from real, correctly-sized world positions) look
 * right: there's no separate CPU size calibration to get wrong. But sprite
 * billboards ALSO have a CPU-computed WORLD HALF-SIZE (see hwr_sw_collect_
 * sprites in source_sw.c) derived from camera zoom/orientation only — a value
 * that's meant to be depth-INDEPENDENT (a fixed "reference" world size), and
 * this SAME per-vertex shader term then multiplies it by up to ~1.75x or down
 * to ~0.49x depending on the sprite's own depth (measured: raw view-depth
 * spans roughly -12000..+8000 in a typical street scene) — a MUCH bigger
 * swing than intended, since the base size was never meant to be hit by that
 * factor at all. A flat CPU-side size correction can't fix this: it multiplies
 * both extremes equally, so making far-oversized sprites correct always makes
 * near-undersized ones (or vice versa) worse.
 *
 * Fix: cancel the shader's own upcoming multiplier here, then re-apply a
 * DAMPENED copy of the identical curve — sprite_persp_strength (0 = flat with
 * DISTANCE; 1 = full uncancelled 3D perspective) lets this be tuned to taste
 * instead of guessing a made-up falloff shape (that was the earlier attempt).
 *
 * ZOOM (separate axis): the CPU base half-size (hwr_sw_collect_sprites) is
 * ∝ 1/scale, which the shader's ×uScale (=scale) exactly cancels — so sprite
 * on-screen size was zoom-INDEPENDENT (never shrank when zooming out). The SW
 * original sized sprites ∝ overall_scale (zoom). Restore that here with a
 * scale/persp_zoom_ref multiplier applied UNCONDITIONALLY (lockstep with the
 * floor/world); persp_zoom_ref is the zoom at which the factor is 1.0 (nominal
 * calibrated size). This is orthogonal to the strength dampening above.
 *
 * cam->scale is overall_scale, which get_scaled_zoom() (enginzoom.c) already
 * multiplies by screen_height/240 so 2D SW blits stay the same *relative*
 * screen size at any resolution. The 3D billboards here are already sized
 * correctly by the projection as resolution changes, so dividing the raw
 * (resolution-scaled) cam->scale by a fixed zoom_ref would double-apply that
 * factor - e.g. sprites end up ~1.5x too big after 720p->1080p. Strip the
 * same height/240 factor back out (mirroring get_unscaled_zoom) before
 * comparing to zoom_ref, so zoom_ref is a resolution-independent constant. */
static float hwr_billboard_dist_scale(const HwrCamera *cam, float bx, float by, float bz)
{
    float strength = hwr_lights_defaults().sprite_persp_strength;
    float zoom_ref = hwr_lights_defaults().sprite_persp_zoom_ref;
    float unscaled_scale = cam->scale;
    float h = (cam->view_h < cam->view_w) ? cam->view_h : cam->view_w;
    if (h >= 400.0f)
        unscaled_scale = cam->scale * 240.0f / h;
    float zoom = (zoom_ref > 0.0f) ? unscaled_scale / zoom_ref : 1.0f;
    float cdx, cdy, cdz, cfb, s, mult;
    /* Zoom factor always applies (even at full strength / non-persp mode);
     * only the perspective cancel/dampen below is gated. */
    if (cam->perspective != 5 || strength >= 0.999f)
        return zoom;   /* nothing to cancel: flat mode or full strength requested */
    cdx = bx - cam->cx;
    cdy = by - cam->cy8;
    cdz = bz - cam->cz;
    cfb = (cam->d10 * cdx + cam->d14 * cdz) / 65536.0f;
    s = (cam->d18 * cdy + cam->d1c * cfb) / 65536.0f;
    if (s > 1024.0f)
        s = 16384.0f * s / (s + 16384.0f);   /* mirror the shader's own scrd warp */
    mult = (16384.0f - s) / 16384.0f;
    if (mult < 0.05f) mult = 0.05f;          /* guard against near-zero/negative divide */
    /* effective_mult = 1 + strength*(mult-1); dscale is what the CPU applies
     * now so that (dscale * mult), which is what actually reaches the screen
     * after the shader's own multiply, equals effective_mult. Times zoom. */
    {
        float dscale = zoom * (1.0f + strength * (mult - 1.0f)) / mult;
        /* The 0.05 floor above only guards the divide; it does NOT bound the
         * result. As s approaches 16384 (far/edge sprites, and continuously
         * now that positions are interpolated) the denominator collapses toward
         * 0.05 while the numerator stays near zoom*0.715, so dscale can spike to
         * ~14x zoom - the intermittent "giant sprite". Cap the final value to a
         * sane multiple of the zoom base. */
        float max_scale = hwr_lights_defaults().sprite_persp_max_scale;
        if (max_scale > 0.0f && dscale > zoom * max_scale)
            dscale = zoom * max_scale;
        return dscale;
    }
}

/* =========================================================================
 * Public entry points
 * ========================================================================= */

/* Build the billboard quad buffers (spr_vbuf/spr_ibuf, spr_vcount/spr_icount)
 * for the subset matching want_translucent (0 = opaque billboards, 1 = those
 * flagged HWR_BILLBOARD_TRANSLUCENT). Shared by the opaque and translucent
 * sprite passes so they stay in sync. */
static void spr_build(HwrBillboard *billboards, int nbill, const HwrCamera *camp,
    int want_translucent, int want_additive, int want_unlit)
{
    HwrCamera cam = *camp;
    int i;
    spr_vcount = 0;
    spr_icount = 0;
    for (i = 0; i < nbill && spr_vcount + 4 <= SPR_MAX_VERTS; i++) {
        HwrBillboard *bb = &billboards[i];
        float u0, v0, u1, v1;
        int sw, sh;
        float hw, hh;
        float shade;

        /* want_translucent: 0 = opaque only, 1 = translucent only, -1 = all
         * (used by the opaque pass when the translucent pass is disabled, so
         * flagged sprites still draw instead of vanishing). */
        if (want_translucent >= 0 &&
            ((bb->flags & HWR_BILLBOARD_TRANSLUCENT) != 0) != want_translucent)
            continue;
        /* want_additive: -1 = don't care, else split the translucent subset by
         * blend mode so fire/glow draw additively and smoke draws alpha-over. */
        if (want_additive >= 0 &&
            ((bb->flags & HWR_BILLBOARD_ADDITIVE) != 0) != want_additive)
            continue;
        /* want_unlit: -1 = don't care, 0 = scene-lit only, 1 = unlit only.
         * Used to split the opaque pass so firing sprites bypass lighting. */
        if (want_unlit >= 0 &&
            ((bb->flags & HWR_BILLBOARD_UNLIT) != 0) != want_unlit)
            continue;

        hwr_atlas_uv(bb->sprite, &u0, &v0, &u1, &v1);
        hwr_atlas_size(bb->sprite, &sw, &sh);
        if (sw <= 0 || sh <= 0)
            continue;
        /* The full-alpha glow tile would otherwise bleed neighbouring atlas
         * pixels at its edges (alpha-keyed sprites don't, their edges discard);
         * inset its UVs ~2 texels so sampling stays inside the tile. */
        if (spr_is_glow_slot(bb->sprite)) {
            float ix = 2.0f / (float)HWR_ATLAS_W, iy = 2.0f / (float)HWR_ATLAS_H;
            u0 += ix; u1 -= ix; v0 += iy; v1 -= iy;
        }

        hw = bb->half_size_x;
        hh = bb->half_size_y;
        if (hw <= 0.0f) hw = 8.0f;
        if (hh <= 0.0f) hh = 8.0f;

        /* SW parity: the fade-table rows scale colours by bri/32 (32 = identity,
         * texture as-is). The atlas is baked at identity, so the draw-time shade
         * is bri/32, clamped to SW's draw range [10..48] = [0.31 .. 1.5x]
         * (draw_sorted_sprite1a). Translucent effects may still fade to 0. */
        shade = (float)bb->shade / 32.0f;
        if (shade > (48.0f / 32.0f)) shade = 48.0f / 32.0f;
        if (shade < (10.0f / 32.0f) && !(bb->flags & HWR_BILLBOARD_TRANSLUCENT))
            shade = 10.0f / 32.0f;
        if (shade < 0.0f) shade = 0.0f;

        {
            float right_norm = sqrtf(cam.d14 * cam.d14 + cam.d10 * cam.d10);
            float rx, rz;
            if (right_norm > 0.0001f) {
                rx = cam.d14 / right_norm;
                rz = -cam.d10 / right_norm;
            } else {
                rx = 1.0f; rz = 0.0f;
            }
            /* bb->x/y/z is the TRUE anchor (Thing's real ground/feet position,
             * or emitter centre for effects) — NOT the quad's geometric centre.
             * cx/cy/cz below is that quad centre, derived from the anchor. */
            float ax = bb->x, ay = bb->y, az = bb->z;
            float cdx = ax - cam.cx;
            float cdy = ay - cam.cy8;
            float cdz = az - cam.cz;
            float cfb = (cam.d10 * cdx + cam.d14 * cdz) / 65536.0f;
            /* Raw (pre-perspective-clamp) view depth — kept for centre_scrd
             * (z-fighting bias) below. Size correction is the cancel-and-
             * dampen scheme in hwr_billboard_dist_scale(); see its comment. */
            float raw_scrd = (cam.d18 * cdy + cam.d1c * cfb) / 65536.0f;
            float cx, cy, cz;
            {
                float dscale = hwr_billboard_dist_scale(&cam, ax, ay, az);
                hw *= dscale;
                hh *= dscale;
                /* Apply the anchor-to-centre offset using the FINAL (post-
                 * dampening) half-size, not the size baked in at capture time
                 * — see anchor_ratio_x/y's doc comment (hwr_scene_source.h)
                 * for why: doing it at capture time desynced from dscale and
                 * made asymmetric poses visibly twitch as distance changed. */
                cx = ax + bb->anchor_ratio_x * hw * rx;
                cz = az + bb->anchor_ratio_x * hw * rz;
                cy = ay + bb->anchor_ratio_y * hh;
            }
            float verts[4][3] = {
                {cx - rx * hw, cy - hh, cz - rz * hw},
                {cx + rx * hw, cy - hh, cz + rz * hw},
                {cx + rx * hw, cy + hh, cz + rz * hw},
                {cx - rx * hw, cy + hh, cz - rz * hw},
            };
            float uvs[4][2] = {{u0,v1}, {u1,v1}, {u1,v0}, {u0,v0}};
            float centre_scrd = raw_scrd;
            if (cam.perspective == 5 && centre_scrd > 1024.0f)
                centre_scrd = 16384.0f * centre_scrd / (centre_scrd + 16384.0f);
            if (bb->flags & HWR_BILLBOARD_ONTOP)
                centre_scrd -= 768.0f;
            int base = spr_vcount;
            int k;
            for (k = 0; k < 4; k++) {
                float *v = &spr_vbuf[(spr_vcount + k) * (SPR_VERT_STRIDE / 4)];
                v[0] = verts[k][0]; v[1] = verts[k][1]; v[2] = verts[k][2];
                v[3] = uvs[k][0];   v[4] = uvs[k][1];
                v[5] = shade;
                v[6] = centre_scrd;
            }
            spr_ibuf[spr_icount++] = base + 0;
            spr_ibuf[spr_icount++] = base + 1;
            spr_ibuf[spr_icount++] = base + 2;
            spr_ibuf[spr_icount++] = base + 0;
            spr_ibuf[spr_icount++] = base + 2;
            spr_ibuf[spr_icount++] = base + 3;
            spr_vcount += 4;
        }
    }
}

int hwr_sprites_render(const unsigned char *pal8, int filter_linear)
{
    HwrCamera cam;
    HwrBillboard *billboards = spr_billboards;
    const HwrSceneSource *s = hwr_source;
    int nbill, i;

    if (!hwr_is_ready() || s == NULL)
        return 0;
    if (!spr_ready && spr_init() != HWR_OK)
        return 0;

    /* Upload any pending sprite pixel data to the GL atlas texture.
     * Pixel data is stashed by the main thread in hwr_atlas_register(). */
    hwr_atlas_upload_pending();

    if (s->get_camera == NULL || s->get_camera(s->ctx, &cam) != 0)
        return 0;
    if (s->get_sprites == NULL)
        return 0;

    nbill = s->get_sprites(s->ctx, billboards, SPR_MAX_BILLBOARDS);
    if (nbill <= 0) {
        /* Silently return — sprites may already be SW-suppressed. */
        return 0;
    }

    /* Opaque pass — two sub-passes so scene lighting doesn't darken sprites
     * with HWR_BILLBOARD_UNLIT (e.g. character firing frames with muzzle flash).
     * When the translucent pass is disabled, draw everything in one call (-1). */
    (void)i;
    spr_setup_program(&cam, s);
    glBindVertexArray(spr_vao);

    if (!spr_tr_enable) {
        /* All sprites, unlit=0 (legacy path — translucent pass disabled). */
        spr_build(billboards, nbill, &cam, -1, -1, -1);
        if (spr_vcount > 0) {
            glBindBuffer(GL_ARRAY_BUFFER, spr_vbo);
            glBufferData(GL_ARRAY_BUFFER,
                (GLsizeiptr)spr_vcount * SPR_VERT_STRIDE, spr_vbuf, GL_STREAM_DRAW);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, spr_ebo);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                (GLsizeiptr)spr_icount * sizeof(uint32_t), spr_ibuf, GL_STREAM_DRAW);
            glDrawElements(GL_TRIANGLES, spr_icount, GL_UNSIGNED_INT, (void *)0);
        }
    } else {
        int sub;
        for (sub = 0; sub < 2; sub++) {
            /* sub 0: scene-lit (unlit=0), sub 1: self-lit (unlit=1, firing/muzzle) */
            spr_build(billboards, nbill, &cam, 0, -1, sub);
            if (spr_vcount <= 0) continue;
            /* sub 0 → scene-lit (uUnlit=0); sub 1 → firing full-bright (uUnlit=2).
             * uUnlit=1 is reserved for translucent effects (shade drives alpha). */
            glUniform1i(spr_loc_unlit, sub == 1 ? 2 : 0);
            glBindBuffer(GL_ARRAY_BUFFER, spr_vbo);
            glBufferData(GL_ARRAY_BUFFER,
                (GLsizeiptr)spr_vcount * SPR_VERT_STRIDE, spr_vbuf, GL_STREAM_DRAW);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, spr_ebo);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                (GLsizeiptr)spr_icount * sizeof(uint32_t), spr_ibuf, GL_STREAM_DRAW);
            glDrawElements(GL_TRIANGLES, spr_icount, GL_UNSIGNED_INT, (void *)0);
        }
        glUniform1i(spr_loc_unlit, 0);   /* restore for safety */
    }

    glBindVertexArray(0);
    hwr_gl_check("hwr_sprites_render");
    return 1;
}

void hwr_sprites_trans_config(int enable, float alpha)
{
    spr_tr_enable = enable;
    if (alpha < 0.0f) alpha = 0.0f;
    if (alpha > 1.0f) alpha = 1.0f;
    spr_tr_alpha = alpha;
}

/* Translucent billboard pass (Phase 8): draws sprites flagged
 * HWR_BILLBOARD_TRANSLUCENT (fire/smoke/glow) blended over the opaque scene.
 * Depth-tested but not depth-writing; uses the opaque sprite program with a
 * sub-1 output alpha. Call after hwr_sprites_render, before SSAO resolve. */
int hwr_sprites_trans_render(const unsigned char *pal8, int filter_linear)
{
    HwrCamera cam;
    HwrBillboard *billboards = spr_billboards;
    const HwrSceneSource *s = hwr_source;
    int nbill;
    (void)pal8; (void)filter_linear;

    if (!spr_tr_enable)
        return 0;
    if (!hwr_is_ready() || s == NULL)
        return 0;
    if (!spr_ready && spr_init() != HWR_OK)
        return 0;
    hwr_atlas_upload_pending();
    if (s->get_camera == NULL || s->get_camera(s->ctx, &cam) != 0)
        return 0;
    if (s->get_sprites == NULL)
        return 0;
    nbill = s->get_sprites(s->ctx, billboards, SPR_MAX_BILLBOARDS);
    if (nbill <= 0)
        return 0;

    /* The translucent pass shares the SSAO G-buffer (colour @0 + world-position
     * @1).  The blend MUST NOT touch the world-position attachment: a small
     * blended sprite over the floor would blend its world position with the
     * floor's, and the SSAO pass then reads a large height discontinuity at the
     * sprite's pixels (sprite-pos vs neighbouring floor-pos) and darkens them to
     * near-black — the sprite "vanishes".  Large glass faces don't hit this
     * because every neighbour samples the same surface, so the blended position
     * stays locally smooth.  Restrict the blended draw to colour attachment 0,
     * then restore the MRT bindings.  Only when the SSAO G-buffer is the bound
     * target; glDrawBuffers on the default framebuffer expects GL_BACK, not an
     * attachment enum. */
    {
        int gbuf = hwr_ssao_active();
        int pass;

        spr_setup_program(&cam, s);
        glUniform1f(spr_loc_alpha, spr_tr_alpha);
        glUniform1i(spr_loc_unlit, 1);   /* effects (smoke/fire/glow) are self-lit */

        if (gbuf) {
            static const GLenum draw1[1] = { GL_COLOR_ATTACHMENT0 };
            glDrawBuffers(1, draw1);
        }
        glEnable(GL_BLEND);
        glDepthMask(GL_FALSE);

        /* Two sub-passes over the translucent subset: alpha-over (smoke) first,
         * then additive (fire/explosions/glow). Each rebuilds the quad buffers
         * for its blend-mode subset. */
        for (pass = 0; pass < 2; pass++) {
            int want_additive = pass;   /* 0 = alpha-over, 1 = additive */
            spr_build(billboards, nbill, &cam, 1, want_additive, -1);
            if (spr_vcount <= 0)
                continue;

            if (want_additive)
                glBlendFunc(GL_SRC_ALPHA, GL_ONE);
            else
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

            glBindVertexArray(spr_vao);
            glBindBuffer(GL_ARRAY_BUFFER, spr_vbo);
            glBufferData(GL_ARRAY_BUFFER,
                (GLsizeiptr)spr_vcount * SPR_VERT_STRIDE, spr_vbuf, GL_STREAM_DRAW);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, spr_ebo);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                (GLsizeiptr)spr_icount * sizeof(uint32_t), spr_ibuf, GL_STREAM_DRAW);
            glDrawElements(GL_TRIANGLES, spr_icount, GL_UNSIGNED_INT, (void *)0);
        }

        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
        if (gbuf) {
            static const GLenum draw2[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
            glDrawBuffers(2, draw2);
        }
    }
    glBindVertexArray(0);

    hwr_gl_check("hwr_sprites_trans_render");
    return 1;
}

/* =========================================================================
 * Shadow passes (blob + projected) — rendered AFTER floor but BEFORE faces
 * so that buildings correctly occlude shadows.
 * ========================================================================= */
int hwr_shadows_render(void)
{
    HwrCamera cam;
    HwrBillboard *billboards = spr_billboards;
    const HwrSceneSource *s = hwr_source;
    int nbill, i;

    if (!hwr_is_ready() || s == NULL)
        return 0;
    /* Ensure shaders are initialised (spr_init sets both shd_prog & psh_prog). */
    if (!spr_ready && spr_init() != HWR_OK)
        return 0;

    if (s->get_camera == NULL || s->get_camera(s->ctx, &cam) != 0)
        return 0;
    if (s->get_sprites == NULL)
        return 0;

    nbill = s->get_sprites(s->ctx, billboards, SPR_MAX_BILLBOARDS);
    if (nbill <= 0)
        return 0;

    /* Sun direction for shadow offset */
    float sun_dir_x = 0.0f, sun_dir_y = 1.0f, sun_dir_z = 0.0f;
    int    sun_active = hwr_sun_enabled();
    int    gbuf = hwr_ssao_active();
    if (sun_active)
        hwr_sun_get_direction(&sun_dir_x, &sun_dir_y, &sun_dir_z);

    /* Both shadow shaders declare only `out vec4 frag` (location 0) - they never
     * write the world-position attachment. Drawing them into the MRT G-buffer
     * therefore leaves attachment 1 UNDEFINED at every shadow pixel, stamping
     * garbage world positions in a ring at each sprite's base. SSAO mostly
     * tolerated it, but water SSR reads those positions and turned each ring into
     * a chain of false reflection hits receding to infinity. Shadows are decals:
     * the real surface is the floor underneath, whose position must survive. So
     * restrict the draw to colour attachment 0 and restore MRT afterwards (same
     * pattern as the blended sprite pass above). */
    if (gbuf) {
        static const GLenum draw1[1] = { GL_COLOR_ATTACHMENT0 };
        glDrawBuffers(1, draw1);
    }

    /* ---- Pass 1: radial-gradient blob shadows ---- */
    {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        /* GL_LEQUAL (not GL_ALWAYS): the floor pass already wrote depth for every
         * elevation it drew, including upper floors/roofs. Without testing here a
         * ground-level shadow blob draws through any higher floor slab nearer the
         * camera at that screen pixel, since nothing stops it from painting over
         * already-resolved closer geometry. depthMask stays off so the shadow
         * itself still doesn't occlude anything drawn after it. */
        glDepthFunc(0x0203); /* GL_LEQUAL */
        glDepthMask(GL_FALSE);
        glUseProgram(shd_prog);
        glUniform1f(shd_loc_d10, cam.d10);
        glUniform1f(shd_loc_d14, cam.d14);
        glUniform1f(shd_loc_d18, cam.d18);
        glUniform1f(shd_loc_d1c, cam.d1c);
        glUniform1f(shd_loc_scale, cam.scale);
        glUniform2f(shd_loc_centre, cam.centre_x, cam.centre_y);
        glUniform3f(shd_loc_ctr, cam.cx, cam.cy8, cam.cz);
        glUniform1i(shd_loc_persp, cam.perspective);

        static float shd_vbuf[SPR_MAX_BILLBOARDS * 4 * 5];
        static uint32_t shd_ibuf[SPR_MAX_BILLBOARDS * 6];
        int shd_vc = 0, shd_ic = 0;

        for (i = 0; i < nbill; i++) {
            HwrBillboard *bb = &billboards[i];
            if (bb->flags & HWR_BILLBOARD_NOSHADOW) continue;
            float sx = bb->x, sy = bb->y, sz = bb->z;
            /* bb->y is now the TRUE anchor (the Thing's actual floor-contact
             * height — see anchor_ratio_y's doc comment), so ground_y is just
             * sy directly; no half_size subtraction needed or wanted (scaling
             * it by dscale, which drifts continuously with camera distance,
             * would make the shadow float above/sink below the floor while
             * scrolling — the same bug the sprite quad itself had). Match the
             * sprite quad's own distance falloff for the FOOTPRINT SIZE only. */
            float dscale = hwr_billboard_dist_scale(&cam, sx, sy, sz);
            float hh = bb->half_size_y * dscale;
            float ground_y = sy;

            /* Stable height basis (≈ old half_size_x*3) so the blob doesn't pop with
             * per-frame sprite width. */
            float sr = hh * 1.15f;
            float sd = hh * 1.15f;
            if (sr < 16.0f) sr = 16.0f;
            if (sd < 16.0f) sd = 16.0f;

            if (shd_vc + 4 > SPR_MAX_BILLBOARDS * 4) break;
            float verts[4][3] = {
                {sx - sr, ground_y, sz - sd},
                {sx + sr, ground_y, sz - sd},
                {sx + sr, ground_y, sz + sd},
                {sx - sr, ground_y, sz + sd},
            };
            float uvs[4][2] = {{0,1},{1,1},{1,0},{0,0}};
            int base = shd_vc;
            int k;
            for (k = 0; k < 4; k++) {
                float *v = &shd_vbuf[shd_vc * 5];
                v[0] = verts[k][0]; v[1] = verts[k][1]; v[2] = verts[k][2];
                v[3] = uvs[k][0];   v[4] = uvs[k][1];
                shd_vc++;
            }
            shd_ibuf[shd_ic++] = base + 0;
            shd_ibuf[shd_ic++] = base + 1;
            shd_ibuf[shd_ic++] = base + 2;
            shd_ibuf[shd_ic++] = base + 0;
            shd_ibuf[shd_ic++] = base + 2;
            shd_ibuf[shd_ic++] = base + 3;
        }

        if (shd_vc > 0) {
            glBindVertexArray(shd_vao);
            glBindBuffer(GL_ARRAY_BUFFER, shd_vbo);
            glBufferData(GL_ARRAY_BUFFER,
                (GLsizeiptr)shd_vc * 5 * sizeof(float), shd_vbuf, GL_STREAM_DRAW);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, shd_ebo);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                (GLsizeiptr)shd_ic * sizeof(uint32_t), shd_ibuf, GL_STREAM_DRAW);
            glDrawElements(GL_TRIANGLES, shd_ic, GL_UNSIGNED_INT, (void *)0);
            glBindVertexArray(0);
        }
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
    }

    /* ---- Pass 2: projected shape shadows (sprite silhouettes on ground) ---- */
    if (psh_ready) {
#define PSH_MAX_QUADS 8192
#define PSH_MAX_VC (PSH_MAX_QUADS * 4)
#define PSH_MAX_IC (PSH_MAX_QUADS * 6)
        float    psh_vbuf[PSH_MAX_VC * 6];
        uint32_t psh_ibuf[PSH_MAX_IC];
        int      psh_vc = 0, psh_ic = 0;

        HwrLight psh_lights[64];
        int npsh_lights = (s && s->get_lights)
            ? s->get_lights(s->ctx, psh_lights, 64) : 0;
        if (npsh_lights < 0) npsh_lights = 0;

        for (i = 0; i < nbill && psh_vc + 4 <= PSH_MAX_VC; i++) {
            HwrBillboard *bb = &billboards[i];
            if (bb->flags & HWR_BILLBOARD_NOSHADOW) continue;
            float u0, v0, u1, v1;
            int sw, sh;
            hwr_atlas_uv(bb->sprite, &u0, &v0, &u1, &v1);
            hwr_atlas_size(bb->sprite, &sw, &sh);
            if (sw <= 0 || sh <= 0) continue;

            /* Size the shadow footprint from the STABLE character height, not the
             * per-frame sprite width (bb->half_size_x ∝ fw).  fw spikes when limbs
             * extend — mostly in profile views — which made the shadow "pop" bigger
             * for one frame then snap back.  half_size_y (∝ fh) is stable across the
             * walk cycle and direction-independent.  0.38 ≈ a typical person's
             * hw/hh ratio, so footprint magnitude is preserved (tunable). */
            float sx = bb->x, sy = bb->y, sz = bb->z;
            /* bb->y is the TRUE anchor (floor-contact height) — ground_y is
             * just sy directly. Match the sprite quad's own distance falloff
             * for the footprint size only; see the blob-shadow pass above. */
            float dscale = hwr_billboard_dist_scale(&cam, sx, sy, sz);
            float hh_true = bb->half_size_y;
            if (hh_true <= 0.0f) hh_true = 8.0f;
            float hh = hh_true * dscale;
            float hw = hh * 0.38f;
            if (hw <= 0.0f) hw = 8.0f;

            float ground_y = sy;

            /* ---- Sun projected shadow ---- */
            if (sun_active && sun_dir_y > 0.001f && psh_vc + 4 <= PSH_MAX_VC) {
                float off_x = -hh * 0.4f * sun_dir_x / sun_dir_y;
                float off_z = -hh * 0.4f * sun_dir_z / sun_dir_y;
                {
                    float max_o = 4096.0f;
                    if (off_x > max_o) off_x = max_o;
                    if (off_x < -max_o) off_x = -max_o;
                    if (off_z > max_o) off_z = max_o;
                    if (off_z < -max_o) off_z = -max_o;
                }

                float shw = hw * 0.8f;
                float shz = hw * 0.8f;
                float opacity = 0.15f;
                float verts[4][3] = {
                    {sx + off_x - shw, ground_y, sz + off_z - shz},
                    {sx + off_x + shw, ground_y, sz + off_z - shz},
                    {sx + off_x + shw, ground_y, sz + off_z + shz},
                    {sx + off_x - shw, ground_y, sz + off_z + shz},
                };
                float uvs[4][2] = {{u0,v1},{u1,v1},{u1,v0},{u0,v0}};
                int base = psh_vc;
                int k;
                for (k = 0; k < 4; k++) {
                    float *v = &psh_vbuf[psh_vc * 6];
                    v[0] = verts[k][0]; v[1] = verts[k][1]; v[2] = verts[k][2];
                    v[3] = uvs[k][0];   v[4] = uvs[k][1];
                    v[5] = opacity;
                    psh_vc++;
                }
                psh_ibuf[psh_ic++] = base + 0;
                psh_ibuf[psh_ic++] = base + 1;
                psh_ibuf[psh_ic++] = base + 2;
                psh_ibuf[psh_ic++] = base + 0;
                psh_ibuf[psh_ic++] = base + 2;
                psh_ibuf[psh_ic++] = base + 3;
            }

            /* ---- Point-light projected shadows (up to 4 per sprite) ---- */
            if (npsh_lights > 0) {
                int li, n_this = 0;
                /* This is a ray/plane intersection: cast a ray from the light
                 * through the character's BODY (an elevated point) down onto
                 * the floor (ground_y) and see where it lands. It needs a
                 * point ABOVE the floor to define that ray — sy USED to be
                 * that point (the billboard's quad centre, feet+hh, before
                 * the anchor-offset refactor made bb->y the true floor-contact
                 * height directly). Reconstruct it here instead of using sy
                 * (now == ground_y, which degenerated the math: t collapsed
                 * to exactly 1.0 every time, so `t <= 1.0f` skipped ALL
                 * point-light shadows unconditionally — the "shadows gone"
                 * regression).
                 *
                 * Use hh_true (the REAL, undampened body half-height), not hh
                 * (the distance/zoom-dampened display size): this needs the
                 * character's actual physical height for the light-angle
                 * geometry to come out right. Using the dampened hh made the
                 * character "shorter" than reality whenever dscale<1 (i.e.
                 * almost always, away from the one calibrated reference zoom/
                 * distance), pulling centre_y right down toward ground_y and
                 * making t barely move off 1.0 for ANY light position — the
                 * shadows reappeared but never visibly stretched. */
                float center_y = sy + hh_true;

                for (li = 0; li < npsh_lights && n_this < 4 && psh_vc + 4 <= PSH_MAX_VC; li++) {
                    HwrLight *lt = &psh_lights[li];
                    float lx = lt->x, ly = lt->y, lz = lt->z;
                    float dx = sx - lx, dy = center_y - ly, dz = sz - lz;
                    float dist2 = dx*dx + dy*dy + dz*dz;
                    if (dist2 <= 0.0f) continue;

                    float maxd2 = lt->max_dist2;
                    if (maxd2 <= 0.0f) maxd2 = 4194304.0f;
                    if (dist2 > maxd2 * 1.5f) continue;

                    /* Horizontal offset from the light to the character — this
                     * is the direction the shadow is cast in (away from the
                     * light) and, for an elevated light, the quantity that
                     * decides how COMPRESSED the shadow is. */
                    float hdx = sx - lx, hdz = sz - lz;
                    float hlen = sqrtf(hdx*hdx + hdz*hdz);
                    float lh = ly - ground_y;   /* light height above the floor */
                    float shadow_len, stretch;

                    if (lh >= hh_true * 0.5f) {
                        /* Elevated light (streetlamp, window, etc.) — true
                         * perspective projection of a pole of height 2*hh_true
                         * standing at the character's feet:
                         *
                         *     shadow_len = body_height * (horiz_dist / light_height)
                         *
                         * so the shadow length scales with how far OFF-AXIS the
                         * character is from the lamp.  Standing right under the
                         * lamp the ratio → 0 and the shadow compresses to a small
                         * pool at the feet (light nearly overhead); walking away
                         * stretches it out.  The old ray/plane form used only the
                         * light/body height ratio for `stretch` and then clamped
                         * the cast distance to hw*3, so the shadow saturated at a
                         * fixed length and never compressed near the lamp. */
                        if (hlen < 0.001f) hlen = 0.001f;
                        stretch = hlen / lh;
                        if (stretch < 0.12f) { stretch = 0.12f; }
                        if (stretch > 3.0f) { stretch = 3.0f; }
                        /* Body half-height, not full height: projecting the
                         * full 2*hh_true pole read as roughly double the
                         * length it should be on screen. */
                        shadow_len = hh_true * stretch;
                    } else {
                        /* Light at/near floor height (fire; vehicle headlights —
                         * sw_get_lights sets their Y to the vehicle's own body Y,
                         * i.e. ~ground level).  There is no meaningful "overhead"
                         * angle for these, so they keep a fixed-length shadow cast
                         * directly away from the light along the ground. */
                        if (hlen < hw * 0.25f) continue;   /* light ~on top of the character */
                        shadow_len = hh_true * 2.0f;
                        stretch = 1.4f;
                    }

                    if (hlen < 0.001f) continue;
                    float nx = hdx / hlen, nz = hdz / hlen;
                    float px = -nz, pz = nx;

                    /* Match the sprite's own distance/zoom falloff, as the
                     * widths (hw/hh) already do. */
                    float ndl = shadow_len * dscale;

                    float nd = dist2 / maxd2;
                    float opacity = (1.0f - nd) * 0.20f;
                    if (opacity < 0.01f) continue;

                    /* Stretch — trapezoid: bottom at sprite feet, top projected away
                     * (stretch itself computed above, per which cast path applied). */
                    float bottom_w = hw * 0.7f;
                    /* Width flares only mildly with the cast angle — the
                     * compression must show up in the LENGTH, not the width,
                     * otherwise an overhead light produces a thin sliver
                     * instead of a compact pool. */
                    float top_w   = bottom_w * (0.85f + stretch * 0.25f);
                    float length  = ndl;   /* stretch is already baked into ndl */
                    float verts[4][3] = {
                        {sx - px*bottom_w, ground_y, sz - pz*bottom_w},
                        {sx + px*bottom_w, ground_y, sz + pz*bottom_w},
                        {sx + nx*length + px*top_w, ground_y, sz + nz*length + pz*top_w},
                        {sx + nx*length - px*top_w, ground_y, sz + nz*length - pz*top_w},
                    };
                    float uvs[4][2] = {{u0,v1},{u1,v1},{u1,v0},{u0,v0}};
                    int base = psh_vc;
                    int k;
                    for (k = 0; k < 4; k++) {
                        float *v = &psh_vbuf[psh_vc * 6];
                        v[0] = verts[k][0]; v[1] = verts[k][1]; v[2] = verts[k][2];
                        v[3] = uvs[k][0];   v[4] = uvs[k][1];
                        v[5] = opacity;
                        psh_vc++;
                    }
                    psh_ibuf[psh_ic++] = base + 0;
                    psh_ibuf[psh_ic++] = base + 1;
                    psh_ibuf[psh_ic++] = base + 2;
                    psh_ibuf[psh_ic++] = base + 0;
                    psh_ibuf[psh_ic++] = base + 2;
                    psh_ibuf[psh_ic++] = base + 3;
                    n_this++;
                }
            }
        }

        if (psh_vc > 0) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthFunc(0x0203); /* GL_LEQUAL — see blob shadow pass above */
            glDepthMask(GL_FALSE);
            glUseProgram(psh_prog);
            glUniform1f(psh_loc_d10, cam.d10);
            glUniform1f(psh_loc_d14, cam.d14);
            glUniform1f(psh_loc_d18, cam.d18);
            glUniform1f(psh_loc_d1c, cam.d1c);
            glUniform1f(psh_loc_scale, cam.scale);
            glUniform2f(psh_loc_centre, cam.centre_x, cam.centre_y);
            glUniform3f(psh_loc_ctr, cam.cx, cam.cy8, cam.cz);
            glUniform1i(psh_loc_persp, cam.perspective);
            hwr_atlas_bind(4);
            glUniform1i(psh_loc_atlas, 4);

            glBindVertexArray(psh_vao);
            glBindBuffer(GL_ARRAY_BUFFER, psh_vbo);
            glBufferData(GL_ARRAY_BUFFER,
                (GLsizeiptr)psh_vc * 6 * sizeof(float), psh_vbuf, GL_STREAM_DRAW);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, psh_ebo);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                (GLsizeiptr)psh_ic * sizeof(uint32_t), psh_ibuf, GL_STREAM_DRAW);
            glDrawElements(GL_TRIANGLES, psh_ic, GL_UNSIGNED_INT, (void *)0);
            glBindVertexArray(0);
            glDepthMask(GL_TRUE);
            glDisable(GL_BLEND);
        }
    }

    glDepthFunc(0x0203); /* GL_LEQUAL — restore for subsequent passes */
    if (gbuf) {          /* restore MRT for the passes that follow */
        static const GLenum draw2[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
        glDrawBuffers(2, draw2);
    }
    hwr_gl_check("hwr_shadows_render");
    return 1;
}

/* =========================================================================
 * Screen-space coloured overlay quads (Phase 8.x): flat-tinted 2D special-face
 * effects — shield-hit spheres, blast rings, lightning slices. The source hands
 * them as screen-pixel quads + RGBA; we convert to NDC and alpha-blend on top of
 * the resolved scene (no depth test — brief overlay effects draw over the 3D).
 * ========================================================================= */

static const char *ov_vert_src =
    "#version 330 core\n"
    "layout(location=0) in vec2 aPos;\n"   /* NDC */
    "layout(location=1) in vec4 aCol;\n"
    "out vec4 vCol;\n"
    "void main(){ vCol = aCol; gl_Position = vec4(aPos, 0.0, 1.0); }\n";

static const char *ov_frag_src =
    "#version 330 core\n"
    "in vec4 vCol;\n"
    "out vec4 frag;\n"
    "void main(){ frag = vCol; }\n";

/* Textured overlay (HUD sprite tiles from the atlas, e.g. the target box). */
static const char *ovt_vert_src =
    "#version 330 core\n"
    "layout(location=0) in vec2 aPos;\n"
    "layout(location=1) in vec2 aUV;\n"
    "layout(location=2) in float aAlpha;\n"
    "out vec2 vUV; out float vA;\n"
    "void main(){ vUV = aUV; vA = aAlpha; gl_Position = vec4(aPos, 0.0, 1.0); }\n";

static const char *ovt_frag_src =
    "#version 330 core\n"
    "in vec2 vUV; in float vA;\n"
    "out vec4 frag;\n"
    "uniform sampler2D uAtlas;\n"
    RECOLOUR_GLSL
    "void main(){ vec4 t = texture(uAtlas, vUV); if (t.a < 0.5) discard;\n"
    "              frag = vec4(atlas_recolour(t.rgb), vA); }\n";

static GLuint ov_prog = 0, ov_vao = 0, ov_vbo = 0;
static GLuint ovt_prog = 0, ovt_vao = 0, ovt_vbo = 0;
static GLint  ovt_loc_atlas = -1;
static GLint  ovt_loc_invpal = -1, ovt_loc_pallive = -1, ovt_loc_recolour = -1;
static int    ov_ready = 0;

#define OV_MAX_QUADS 4096
static HwrOverlayQuad ov_quads[OV_MAX_QUADS];
static float ov_vbuf[OV_MAX_QUADS * 6 * 6];   /* flat: xy + rgba */
static float ovt_vbuf[OV_MAX_QUADS * 6 * 5];  /* textured: xy + uv + a */

static int ov_init(void)
{
    GLuint vs, fs;
    GLint ok = 0;
    vs = spr_compile(GL_VERTEX_SHADER, ov_vert_src);
    if (!vs) return HWR_ERROR;
    fs = spr_compile(GL_FRAGMENT_SHADER, ov_frag_src);
    if (!fs) { glDeleteShader(vs); return HWR_ERROR; }
    ov_prog = glCreateProgram();
    glAttachShader(ov_prog, vs);
    glAttachShader(ov_prog, fs);
    glLinkProgram(ov_prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    glGetProgramiv(ov_prog, GL_LINK_STATUS, &ok);
    if (!ok) { hwr_set_error("overlay program link failed"); return HWR_ERROR; }
    glGenVertexArrays(1, &ov_vao);
    glBindVertexArray(ov_vao);
    glGenBuffers(1, &ov_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, ov_vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 24, (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 24, (void *)8);
    glBindVertexArray(0);

    vs = spr_compile(GL_VERTEX_SHADER, ovt_vert_src);
    if (!vs) return HWR_ERROR;
    fs = spr_compile(GL_FRAGMENT_SHADER, ovt_frag_src);
    if (!fs) { glDeleteShader(vs); return HWR_ERROR; }
    ovt_prog = glCreateProgram();
    glAttachShader(ovt_prog, vs);
    glAttachShader(ovt_prog, fs);
    glLinkProgram(ovt_prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    glGetProgramiv(ovt_prog, GL_LINK_STATUS, &ok);
    if (!ok) { hwr_set_error("overlay-tex program link failed"); return HWR_ERROR; }
    ovt_loc_atlas = glGetUniformLocation(ovt_prog, "uAtlas");
    ovt_loc_invpal   = glGetUniformLocation(ovt_prog, "uInvPal");
    ovt_loc_pallive  = glGetUniformLocation(ovt_prog, "uPalLive");
    ovt_loc_recolour = glGetUniformLocation(ovt_prog, "uRecolour");
    glGenVertexArrays(1, &ovt_vao);
    glBindVertexArray(ovt_vao);
    glGenBuffers(1, &ovt_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, ovt_vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 20, (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 20, (void *)8);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, 20, (void *)16);
    glBindVertexArray(0);

    ov_ready = 1;
    return HWR_OK;
}

int hwr_overlay_render(void)
{
    HwrCamera cam;
    const HwrSceneSource *s = hwr_source;
    int nq, i, vc = 0, tvc = 0;
    float cx, cy;
    /* Two triangles sharing the TL->BR diagonal: {TL,TR,BR} + {TL,BR,BL}.
     * (A split using two different diagonals leaves a triangular gap on one
     * side — that was clipping the box's left arms at an angle.) */
    static const int order[6] = { 0, 1, 2, 0, 2, 3 };
    static const float uvx[4] = { 0, 1, 1, 0 }, uvy[4] = { 0, 0, 1, 1 };

    if (!hwr_is_ready() || s == NULL || s->get_overlays == NULL)
        return 0;
    if (s->get_camera == NULL || s->get_camera(s->ctx, &cam) != 0)
        return 0;
    if (cam.centre_x <= 0.0f || cam.centre_y <= 0.0f)
        return 0;
    nq = s->get_overlays(s->ctx, ov_quads, OV_MAX_QUADS);
    if (nq <= 0)
        return 0;
    if (!ov_ready && ov_init() != HWR_OK)
        return 0;
    hwr_atlas_upload_pending();   /* box tile may have been registered in get_overlays */

    cx = cam.centre_x; cy = cam.centre_y;
    for (i = 0; i < nq; i++) {
        HwrOverlayQuad *q = &ov_quads[i];
        int j;
        if (q->slot < 0) {
            for (j = 0; j < 6; j++) {
                int c = order[j];
                float *v = &ov_vbuf[vc * 6];
                v[0] = q->x[c] / cx - 1.0f;
                v[1] = 1.0f - q->y[c] / cy;
                v[2] = q->r; v[3] = q->g; v[4] = q->b; v[5] = q->a;
                vc++;
            }
        } else {
            for (j = 0; j < 6; j++) {
                int c = order[j];
                float *v = &ovt_vbuf[tvc * 5];
                v[0] = q->x[c] / cx - 1.0f;
                v[1] = 1.0f - q->y[c] / cy;
                v[2] = q->u0 + uvx[c] * (q->u1 - q->u0);
                v[3] = q->v0 + uvy[c] * (q->v1 - q->v0);
                v[4] = q->a;
                tvc++;
            }
        }
    }

    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    if (vc > 0) {
        glUseProgram(ov_prog);
        glBindVertexArray(ov_vao);
        glBindBuffer(GL_ARRAY_BUFFER, ov_vbo);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)vc * 6 * sizeof(float), ov_vbuf, GL_STREAM_DRAW);
        glDrawArrays(GL_TRIANGLES, 0, vc);
    }
    if (tvc > 0) {
        glUseProgram(ovt_prog);
        hwr_atlas_bind(4);
        glUniform1i(ovt_loc_atlas, 4);
        spr_bind_recolour(ovt_loc_invpal, ovt_loc_pallive, ovt_loc_recolour);
        glBindVertexArray(ovt_vao);
        glBindBuffer(GL_ARRAY_BUFFER, ovt_vbo);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)tvc * 5 * sizeof(float), ovt_vbuf, GL_STREAM_DRAW);
        glDrawArrays(GL_TRIANGLES, 0, tvc);
    }
    glBindVertexArray(0);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);

    hwr_gl_check("hwr_overlay_render");
    return 1;
}

/* --- Depth-tested weapon beams (electric zap / laser) ---------------------- */
static const char *beam_vert_src =
    "#version 330 core\n"
    "layout(location=0) in vec3 aPos;\n"   /* clip/NDC (xy screen, z = scrd/16384) */
    "layout(location=1) in vec4 aCol;\n"
    "out vec4 vCol;\n"
    "void main(){ vCol = aCol; gl_Position = vec4(aPos, 1.0); }\n";

static const char *beam_frag_src =
    "#version 330 core\n"
    "in vec4 vCol;\n"
    "out vec4 frag;\n"
    "void main(){ frag = vCol; }\n";

static GLuint beam_prog = 0, beam_vao = 0, beam_vbo = 0;
static int    beam_ready = 0;
#define BEAM_MAX_VERTS (4096 * 6)
static HwrBeamVertex beam_verts[BEAM_MAX_VERTS];

static int beam_init(void)
{
    GLuint vs, fs;
    GLint ok = 0;
    vs = spr_compile(GL_VERTEX_SHADER, beam_vert_src);
    if (!vs) return HWR_ERROR;
    fs = spr_compile(GL_FRAGMENT_SHADER, beam_frag_src);
    if (!fs) { glDeleteShader(vs); return HWR_ERROR; }
    beam_prog = glCreateProgram();
    glAttachShader(beam_prog, vs);
    glAttachShader(beam_prog, fs);
    glLinkProgram(beam_prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    glGetProgramiv(beam_prog, GL_LINK_STATUS, &ok);
    if (!ok) { hwr_set_error("beam program link failed"); return HWR_ERROR; }
    glGenVertexArrays(1, &beam_vao);
    glBindVertexArray(beam_vao);
    glGenBuffers(1, &beam_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, beam_vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 28, (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 28, (void *)12);
    glBindVertexArray(0);
    beam_ready = 1;
    return HWR_OK;
}

/* Draw weapon beams (electric zap / laser) as depth-tested triangles so 3D
 * geometry occludes them. Must run while the scene depth buffer is still intact
 * (after the opaque pass, before the depth-less HUD overlay pass). */
int hwr_beams_render(void)
{
    const HwrSceneSource *s = hwr_source;
    int nv;
    if (!hwr_is_ready() || s == NULL || s->get_beams == NULL)
        return 0;
    nv = s->get_beams(s->ctx, beam_verts, BEAM_MAX_VERTS);
    if (nv < 3)
        return 0;
    if (!beam_ready && beam_init() != HWR_OK)
        return 0;
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_FALSE);          /* beams don't write depth */
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(beam_prog);
    glBindVertexArray(beam_vao);
    glBindBuffer(GL_ARRAY_BUFFER, beam_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)nv * 7 * sizeof(float),
        beam_verts, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, nv);
    glBindVertexArray(0);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    hwr_gl_check("hwr_beams_render");
    return 1;
}

void hwr_sprites_reset(void)
{
    hwr_atlas_reset();
    if (beam_prog) { glDeleteProgram(beam_prog); beam_prog = 0; }
    if (beam_vao)  { glDeleteVertexArrays(1, &beam_vao); beam_vao = 0; }
    if (beam_vbo)  { glDeleteBuffers(1, &beam_vbo); beam_vbo = 0; }
    beam_ready = 0;
    if (psh_prog) { glDeleteProgram(psh_prog); psh_prog = 0; }
    if (psh_vao)  { glDeleteVertexArrays(1, &psh_vao); psh_vao = 0; }
    if (psh_vbo)  { glDeleteBuffers(1, &psh_vbo); psh_vbo = 0; }
    if (psh_ebo)  { glDeleteBuffers(1, &psh_ebo); psh_ebo = 0; }
    psh_ready = 0;
}
