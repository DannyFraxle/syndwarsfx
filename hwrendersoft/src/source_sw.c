/******************************************************************************/
// Syndicate Wars FX3D - OpenGL hardware renderer for Bullfrog titles.
/******************************************************************************/
/**                        2026 danny@fraxle.net                             **/
/******************************************************************************/
/** @file source_sw.c
 *     Syndicate Wars implementation of the HwrSceneSource interface.
 * @par Purpose:
 *     Bridges the game's world data to the engine-agnostic scene source the GL
 *     backend pulls from. This is the only file in libhwrender that knows about
 *     Syndicate Wars globals; other Bullfrog titles supply their own source_*.c.
 *
 *     The needed globals are declared here directly (with link-compatible
 *     types) rather than by including the game's deeply-tangled headers, so the
 *     library stays buildable on its own. They resolve at the final link of the
 *     game executable.
 * @par  Copying and copyrights:
 *     This program is free software; you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of the License, or
 *     (at your option) any later version.
 */
/******************************************************************************/
#include "hwr_scene_source.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* --- Game globals (resolved at the executable's link step) --- */
/* Camera centre, from engincam.h (s32). */
extern int32_t        engn_xc, engn_yc, engn_zc, engn_anglexz;
extern unsigned short overall_scale;
/* Affine-projection factors, from engintrns.h, set every frame by the SW camera
 * setup. transform_shpoint() uses exactly these. */
extern int32_t        dword_176D10, dword_176D14;   /* sin, cos of XZ rotation */
extern int32_t        dword_176D18, dword_176D1C;   /* sin, cos of view pitch  */
extern int32_t        dword_176D3C, dword_176D40;   /* screen centre x, y      */
/* Visible window radius in tiles, from enginzoom.h. */
extern unsigned short render_area_a, render_area_b;
/* Projection mode (5 = the game's default fake-perspective). */
extern int32_t        game_perspective;
/* Current 6-bit-per-channel palette (256 RGB triplets). */
extern unsigned char  display_palette[768];

/* Map grid and floor textures. Layouts mirror src/bigmap.h and
 * swrendersoft/include/enginsngtxtr.h exactly (sizeof 18 each, packed). */
#define HWR_MAP_TILE_WIDTH  128
#define HWR_TMAP_PAGES      18
#define HWR_TMAP_DIM        256

#pragma pack(push, 1)
struct HwrMapEl {           /* == struct MyMapElement */
    uint16_t Texture;
    uint16_t Shade;
    uint8_t  ShadeR;
    uint8_t  Flags;
    int16_t  Alt;
    int16_t  Child;
    uint16_t ColHead;
    uint16_t Ambient;
    uint8_t  Zip;
    uint8_t  Flags2;
    uint16_t ColumnHead;
};
struct HwrFloorTex {        /* == struct SingleFloorTexture (face4 quads use this) */
    uint8_t TMapX1, TMapY1, TMapX2, TMapY2;
    uint8_t TMapX3, TMapY3, TMapX4, TMapY4;
    uint8_t Page;
    uint8_t field_9[3];
    uint8_t field_C[5];
    uint8_t field_11;
};
struct HwrFaceTex {         /* == struct SingleTexture, sizeof 16 (face3 tris use this) */
    uint8_t TMapX1, TMapY1, TMapX2, TMapY2, TMapX3, TMapY3;
    uint8_t Page;
    uint8_t padding1;
    int16_t pal;
    uint8_t field_A[6];
};
/* Object geometry mirrors (enginsngobjs.h). Buildings/props are SingleObjects,
 * each referencing a run of face3/face4 that index a shared SinglePoint pool. */
struct HwrSinglePoint {     /* == struct SinglePoint, sizeof 10 */
    uint16_t PointOffset;
    int16_t  X, Y, Z;
    uint8_t  Pad1;
    uint8_t  Flags;
};
struct HwrObjFace3 {        /* == struct SingleObjectFace3, sizeof 32 */
    int16_t  PointNo[3];
    uint16_t Texture;
    uint8_t  GFlags;
    uint8_t  Flags;
    uint16_t ExCol;
    uint16_t Object;
    int16_t  Shade0, Shade1, Shade2;
    uint16_t Light0, Light1, Light2;
    uint16_t FaceNormal;
    uint16_t WalkHeader;
    uint16_t UnknTringl;
};
struct HwrObjFace4 {        /* == struct SingleObjectFace4, sizeof 40 */
    int16_t  PointNo[4];
    uint16_t Texture;
    uint8_t  GFlags;
    uint8_t  Flags;
    uint16_t ExCol;
    uint16_t Object;
    int16_t  Shade0, Shade1, Shade2, Shade3;
    int16_t  Light0, Light1, Light2, Light3;
    uint16_t FaceNormal;
    uint16_t WalkHeader;
    uint16_t UnknTringl1, UnknTringl2;
};
struct HwrObject {          /* == struct SingleObject, sizeof 36 */
    uint16_t StartFace;
    uint16_t NumbFaces;
    uint16_t NextObject;
    uint16_t StartFace4;
    uint16_t NumbFaces4;
    int16_t  ThingNo;
    int16_t  OffsetX, OffsetY, OffsetZ;
    int16_t  ObjectNo;
    int16_t  MapX, MapZ;
    uint16_t StartPoint, EndPoint;
    uint16_t field_1C, field_1E;
    uint8_t  field_20[3];
    uint8_t  field_23;
};
#pragma pack(pop)

extern struct HwrMapEl    *game_my_big_map;     /* == game_my_big_map */
extern struct HwrFloorTex *game_textures;       /* == game_textures (floor + face4) */
extern int32_t             game_textures_limit;
extern struct HwrFaceTex  *game_face_textures;  /* == game_face_textures (face3) */
extern int32_t             face_textures_limit;
extern unsigned char      *vec_tmap[HWR_TMAP_PAGES]; /* 256x256 indexed pages */

extern struct HwrObject     *game_objects;       /* == game_objects        */
extern unsigned short        next_object;        /* count of objects        */
extern struct HwrSinglePoint *game_object_points;/* == game_object_points   */
extern struct HwrObjFace3   *game_object_faces3; /* == game_object_faces3   */
extern struct HwrObjFace4   *game_object_faces4; /* == game_object_faces4   */

/* The palette index reserved as the composite key (set by the host glue). */
int hwr_sw_key_index = 0;

/* Camera snapshot, captured at floor-draw time (when the projection globals hold
 * the engine-view values). Reading the live globals at present time is unsafe -
 * later sub-renders (BAT/billboard) overwrite them. */
static struct {
    int32_t xc, yc, zc;
    int32_t D10, D14, D18, D1C, D3C, D40;
    int32_t scale;
    int     ra, rb;
    int     persp;
    int     valid;
} snap;

void hwr_sw_capture(void)
{
    snap.xc = engn_xc; snap.yc = engn_yc; snap.zc = engn_zc;
    snap.D10 = dword_176D10; snap.D14 = dword_176D14;
    snap.D18 = dword_176D18; snap.D1C = dword_176D1C;
    snap.D3C = dword_176D3C; snap.D40 = dword_176D40;
    snap.scale = overall_scale;
    snap.ra = render_area_a; snap.rb = render_area_b;
    snap.persp = game_perspective;
    snap.valid = 1;
}

/* Viewport, supplied by the host at creation time. */
static int sw_view_w = 0;
static int sw_view_h = 0;

/* --- Floor geometry buffers (rebuilt each frame) --- */
#define HWR_FLOOR_MAX_TILES 16384       /* up to 127x127 visible tiles (full map) */
static HwrVertex floor_verts[HWR_FLOOR_MAX_TILES * 4];
static uint32_t  floor_index[HWR_FLOOR_MAX_TILES * 6];
static int       floor_vert_count = 0;
static int       floor_index_count = 0;

/* --- Object/building face geometry buffers (rebuilt each frame, Phase 4) --- */
#define HWR_FACE_MAX_VERTS  (256 * 1024)
#define HWR_FACE_MAX_INDEX  (384 * 1024)
static HwrVertex face_verts[HWR_FACE_MAX_VERTS];
static uint32_t  face_index[HWR_FACE_MAX_INDEX];
static int       face_vert_count = 0;
static int       face_index_count = 0;
/* One bit per object: marks objects already emitted this frame (an object can be
 * referenced by several map columns). */
static uint8_t   face_obj_seen[65536 / 8];

/* Texture pages packed contiguously (18 * 256 * 256) for the GL texture array. */
static uint8_t   floor_pages[HWR_TMAP_PAGES * HWR_TMAP_DIM * HWR_TMAP_DIM];
static int       floor_pages_ready = 0;

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static int16_t tile_alt(int gx, int gz)
{
    if (game_my_big_map == NULL)
        return 0;
    gx = clampi(gx, 0, HWR_MAP_TILE_WIDTH - 1);
    gz = clampi(gz, 0, HWR_MAP_TILE_WIDTH - 1);
    return game_my_big_map[HWR_MAP_TILE_WIDTH * gz + gx].Alt;
}

/* Return the Alt for a tile corner, but if the corner cell is a wall/column cell
 * (Texture==0, no floor surface), use the centre tile's Alt instead.
 * This prevents ledge-edge tiles from creating ramps down to ground level when
 * one pair of corners is adjacent to a building. */
static int16_t corner_alt(int cx, int cz, int corner_gx, int corner_gz)
{
    int cgx = clampi(corner_gx, 0, HWR_MAP_TILE_WIDTH - 1);
    int cgz = clampi(corner_gz, 0, HWR_MAP_TILE_WIDTH - 1);
    if (game_my_big_map[HWR_MAP_TILE_WIDTH * cgz + cgx].Texture == 0)
        return tile_alt(cx, cz);
    return game_my_big_map[HWR_MAP_TILE_WIDTH * cgz + cgx].Alt;
}

/* For column/building cells (Texture==0), find the nearest valid floor tile
 * (8-connected) and copy its texture index and ShadeR. Returns 1 on success.
 * Column cells never have ShadeR set by the SW renderer, so inheriting prevents
 * them rendering as pitch-black even when geometry is correct. */
static int nearest_floor_neighbour(int gx, int gz, int *out_texidx, uint8_t *out_shade)
{
    static const int offs[8][2] = {
        {-1,0},{1,0},{0,-1},{0,1},{-1,-1},{-1,1},{1,-1},{1,1}
    };
    int i;
    for (i = 0; i < 8; i++) {
        int nx = clampi(gx + offs[i][0], 0, HWR_MAP_TILE_WIDTH - 1);
        int nz = clampi(gz + offs[i][1], 0, HWR_MAP_TILE_WIDTH - 1);
        struct HwrMapEl *nm = &game_my_big_map[HWR_MAP_TILE_WIDTH * nz + nx];
        int ni = nm->Texture & 0x3FFF;
        if (nm->Texture != 0 && !(nm->Texture & 0x8000) && ni < game_textures_limit) {
            *out_texidx = ni;
            *out_shade  = nm->ShadeR;
            return 1;
        }
    }
    return 0;
}

static int sw_get_camera(void *ctx, HwrCamera *out)
{
    /* Hand the shader the raw projection factors so it can reproduce
     * transform_shpoint() exactly, including the mode-5 perspective. */
    (void)ctx;
    if (out == NULL || !snap.valid || snap.D3C == 0 || snap.D40 == 0)
        return -1;
    out->d10 = (float)snap.D10; out->d14 = (float)snap.D14;
    out->d18 = (float)snap.D18; out->d1c = (float)snap.D1C;
    out->scale    = (float)snap.scale;
    out->centre_x = (float)snap.D3C;
    out->centre_y = (float)snap.D40;
    out->cx  = (float)snap.xc;
    out->cy8 = (float)(8 * snap.yc);
    out->cz  = (float)snap.zc;
    out->perspective = snap.persp;
    out->view_w = sw_view_w;
    out->view_h = sw_view_h;
    return 0;
}

static int sw_get_floor(void *ctx, HwrGeometryBatch *out)
{
    int cx, cz, ra, rb, gx, gz, x0, x1, z0, z1;
    (void)ctx;
    floor_vert_count = 0;
    floor_index_count = 0;
    if (out != NULL) {
        out->verts = NULL; out->vert_count = 0;
        out->indices = NULL; out->index_count = 0;
    }
    if (game_my_big_map == NULL || game_textures == NULL || out == NULL || !snap.valid)
        return 0;

    cx = snap.xc >> 8;
    cz = snap.zc >> 8;
    ra = snap.ra ? snap.ra + 2 : 24;
    rb = snap.rb ? snap.rb + 2 : 24;
    x0 = clampi(cx - ra, 0, HWR_MAP_TILE_WIDTH - 2);
    x1 = clampi(cx + ra, 0, HWR_MAP_TILE_WIDTH - 2);
    z0 = clampi(cz - rb, 0, HWR_MAP_TILE_WIDTH - 2);
    z1 = clampi(cz + rb, 0, HWR_MAP_TILE_WIDTH - 2);

    for (gz = z0; gz <= z1; gz++) {
        for (gx = x0; gx <= x1; gx++) {
            struct HwrMapEl *me = &game_my_big_map[HWR_MAP_TILE_WIDTH * gz + gx];
            struct HwrFloorTex *tx;
            int texidx = me->Texture & 0x3FFF;   /* low bits = index, high = flags */
            uint8_t inherited_shade = me->ShadeR;
            HwrVertex *v;
            int base;

            if (texidx >= game_textures_limit)
                continue;
            /* True water/reflective tiles have both the 0x8000 transparent flag
             * AND Flags&0x10 (the waft/wobble flag). Skip those — the SW blit
             * renders water animation. Non-wobbling 0x8000 tiles (e.g. special
             * ledge cells) fall through and render normally. */
            if ((me->Texture & 0x8000) && (me->Flags & 0x10))
                continue;
            if (floor_vert_count + 4 > HWR_FLOOR_MAX_TILES * 4)
                break;

            /* Column/building cells have Texture==0 (no floor surface). Borrow
             * texture AND shade from the nearest valid floor neighbour so the
             * cell fills correctly (ShadeR is never set for column cells). */
            if (me->Texture == 0) {
                int ni;
                if (!nearest_floor_neighbour(gx, gz, &ni, &inherited_shade)) continue;
                texidx = ni;
            }
            tx = &game_textures[texidx];

            /* Per-tile depth: scrd at the tile centre, with perspective mod.
             * All 4 vertices share the same depth so tiles sort as flat units,
             * matching the SW bucket sort and preventing ground tiles that are
             * closer horizontally from hiding elevated ledge tiles above them. */
            {
                float tdx = (float)(((gx << 8) + 128) - snap.xc);
                float tdz = (float)(((gz << 8) + 128) - snap.zc);
                float talt = (float)(8 * tile_alt(gx, gz));
                float tdy = talt - (float)(8 * snap.yc);
                float tfb = ((float)snap.D10 * tdx + (float)snap.D14 * tdz) / 65536.0f;
                /* Linear depth (no perspective clamp) so floor and faces share a
                 * monotonic z-buffer scale; see face_scrd(). */
                float td  = ((float)snap.D18 * tdy + (float)snap.D1C * tfb) / 65536.0f;
                floor_verts[floor_vert_count + 0].tile_depth =
                floor_verts[floor_vert_count + 1].tile_depth =
                floor_verts[floor_vert_count + 2].tile_depth =
                floor_verts[floor_vert_count + 3].tile_depth = td;
            }

            base = floor_vert_count;
            v = &floor_verts[base];
            /* Corner positions: v[0]=(gx,gz), v[1]=(gx+1,gz),
             *                   v[2]=(gx+1,gz+1), v[3]=(gx,gz+1) */
            v[0].x = (float)(gx << 8);     v[0].z = (float)(gz << 8);     v[0].y = (float)(8 * corner_alt(gx, gz, gx,   gz));
            v[1].x = (float)((gx+1) << 8); v[1].z = (float)(gz << 8);     v[1].y = (float)(8 * corner_alt(gx, gz, gx+1, gz));
            v[2].x = (float)((gx+1) << 8); v[2].z = (float)((gz+1) << 8); v[2].y = (float)(8 * corner_alt(gx, gz, gx+1, gz+1));
            v[3].x = (float)(gx << 8);     v[3].z = (float)((gz+1) << 8); v[3].y = (float)(8 * corner_alt(gx, gz, gx,   gz+1));
            /* UV mapping derived from draw_floor_tile1a / set_floor_texture_uv:
             *   v[0](gx,gz)     → TMap4
             *   v[1](gx+1,gz)   → TMap3
             *   v[2](gx+1,gz+1) → TMap2
             *   v[3](gx,gz+1)   → TMap1 */
            v[0].u = tx->TMapX4; v[0].v = tx->TMapY4;
            v[1].u = tx->TMapX3; v[1].v = tx->TMapY3;
            v[2].u = tx->TMapX2; v[2].v = tx->TMapY2;
            v[3].u = tx->TMapX1; v[3].v = tx->TMapY1;
            v[0].page = v[1].page = v[2].page = v[3].page = tx->Page;
            {
                /* ShadeR (0..127). For Texture==0 cells, inherited_shade comes
                 * from the nearest valid neighbour (ShadeR is never set for
                 * column cells). Scale to 0..255 for the light byte. */
                int sh = (int)inherited_shade * 2;
                if (sh > 255) sh = 255;
                if (sh < 0)   sh = 0;
                v[0].light = v[1].light = v[2].light = v[3].light = (uint8_t)sh;
            }
            floor_vert_count += 4;

            /* Triangle split matches SW draw_floor_tile1a: diagonal (v[3]→v[1])
             * i.e. (v0,v3,v1) + (v1,v3,v2). */
            floor_index[floor_index_count++] = base + 0;
            floor_index[floor_index_count++] = base + 3;
            floor_index[floor_index_count++] = base + 1;
            floor_index[floor_index_count++] = base + 1;
            floor_index[floor_index_count++] = base + 3;
            floor_index[floor_index_count++] = base + 2;
        }
    }

    out->verts = floor_verts;
    out->vert_count = floor_vert_count;
    out->indices = floor_index;
    out->index_count = floor_index_count;
    return floor_index_count;
}

/* Linear (monotonic) screen depth for a world point, used as the z-buffer value.
 * NOTE: deliberately omits the mode-5 perspective clamp (16384*td/(td+16384)).
 * That clamp is non-monotonic around td=1024 (a farther point can get a smaller
 * value), which is fine for the SW's coarse bucket sort but inverts occlusion in
 * a per-pixel z-buffer and punches gaps in large faces. The clamp still lives in
 * the vertex shader for the X/Y projection; depth must stay linear. */
static float face_scrd(float wx, float wy, float wz)
{
    float dx = wx - (float)snap.xc;
    float dy = wy - (float)(8 * snap.yc);
    float dz = wz - (float)snap.zc;
    float fb = ((float)snap.D10 * dx + (float)snap.D14 * dz) / 65536.0f;
    float td = ((float)snap.D18 * dy + (float)snap.D1C * fb) / 65536.0f;
    return td;
}

/* Emit one face vertex. */
static void face_emit_vert(int wx, int wy, int wz, uint8_t u, uint8_t v,
    uint8_t page, uint8_t light, float depth)
{
    HwrVertex *o = &face_verts[face_vert_count++];
    o->x = (float)wx; o->y = (float)wy; o->z = (float)wz;
    o->u = u; o->v = v;
    o->page = page; o->light = light;
    o->tile_depth = depth;
}

/* Build the world position of an object point. The object is placed at its
 * Thing position, cached at load: MapX/OffsetX = thing.X>>8, MapZ/OffsetZ =
 * thing.Z>>8 (world units, tile<<8) so they add directly; OffsetY = thing.Y>>8,
 * For TT_BUILDING, thing_position_uses_y_mul_8 is false, so the render path uses
 * cor_dy = thing.Y>>8 = OffsetY directly (already in the floor's 8*Alt scale -
 * NO extra 8x, or buildings fly). Using 8*tile_alt instead sank some buildings. */
#define FACE_OBJ_WORLDX(obj, pt) ((int)(uint16_t)(obj)->MapX + (int)(pt)->X)
#define FACE_OBJ_WORLDZ(obj, pt) ((int)(uint16_t)(obj)->MapZ + (int)(pt)->Z)
#define FACE_OBJ_WORLDY(obj, pt) ((int)(obj)->OffsetY + (int)(pt)->Y)

static int sw_get_faces(void *ctx, HwrGeometryBatch *out)
{
    int cx, cz, ra, rb, x0, x1, z0, z1;
    unsigned o;
    (void)ctx;
    face_vert_count = 0;
    face_index_count = 0;
    if (out != NULL) {
        out->verts = NULL; out->vert_count = 0;
        out->indices = NULL; out->index_count = 0;
    }
    if (game_objects == NULL || game_object_points == NULL ||
        game_object_faces4 == NULL || game_object_faces3 == NULL ||
        game_textures == NULL || out == NULL || !snap.valid)
        return 0;

    cx = snap.xc >> 8;
    cz = snap.zc >> 8;
    ra = (snap.ra ? snap.ra + 2 : 24);
    rb = (snap.rb ? snap.rb + 2 : 24);
    x0 = cx - ra; x1 = cx + ra;
    z0 = cz - rb; z1 = cz + rb;

    memset(face_obj_seen, 0, sizeof(face_obj_seen));

    for (o = 1; o < next_object; o++) {
        struct HwrObject *obj = &game_objects[o];
        int mtx = (uint16_t)obj->MapX >> 8;   /* MapX/Z are world units (tile<<8) */
        int mtz = (uint16_t)obj->MapZ >> 8;
        int f;

        /* One object can be referenced by several map columns; emit once. */
        if (face_obj_seen[o >> 3] & (1 << (o & 7)))
            continue;
        face_obj_seen[o >> 3] |= (uint8_t)(1 << (o & 7));

        /* Cull by map placement against the visible tile window. */
        if (mtx < x0 || mtx > x1 || mtz < z0 || mtz > z1)
            continue;

        /* --- Quads (face4) --- */
        for (f = 0; f < obj->NumbFaces4; f++) {
            struct HwrObjFace4 *fc = &game_object_faces4[obj->StartFace4 + f];
            /* Object faces use the RAW Texture value as the index (no 0x3FFF
             * mask, no 0x8000 flag - those are floor-tile semantics). Mirror
             * set_floor_texture_uv: clamp an out-of-range index to 0 and still
             * render, never drop the face. */
            int texidx = fc->Texture;
            int flat = (texidx == 0);   /* untextured -> flat-shaded face */
            struct HwrFloorTex *tx = NULL;
            uint8_t pg, u0,v0c,u1,v1c,u2,v2c,u3,v3c;
            struct HwrSinglePoint *p[4];
            int wx[4], wy[4], wz[4], k, base;
            float sd[4];

            if (!flat && texidx >= game_textures_limit)
                texidx = 0;   /* clamp like set_floor_texture_uv, still render */
            if (face_vert_count + 4 > HWR_FACE_MAX_VERTS ||
                face_index_count + 6 > HWR_FACE_MAX_INDEX)
                break;
            if (flat) {
                pg = 255;   /* shader sentinel: flat-shaded, no texture sample */
                u0=v0c=u1=v1c=u2=v2c=u3=v3c=0;
            } else {
                tx = &game_textures[texidx];
                pg = tx->Page;
                /* UV-to-point mapping from draw_object_face4d_textrd's
                 * set_floor_texture_uv call: PN0->TMap1, PN1->TMap2, and then
                 * normally PN2->TMap3, PN3->TMap4. With FGFlg_Unkn20 the last two
                 * are swapped (PN2->TMap4, PN3->TMap3). */
                u0=tx->TMapX1; v0c=tx->TMapY1; u1=tx->TMapX2; v1c=tx->TMapY2;
                if (fc->GFlags & 0x20) {   /* FGFlg_Unkn20 */
                    u2=tx->TMapX4; v2c=tx->TMapY4; u3=tx->TMapX3; v3c=tx->TMapY3;
                } else {
                    u2=tx->TMapX3; v2c=tx->TMapY3; u3=tx->TMapX4; v3c=tx->TMapY4;
                }
            }

            for (k = 0; k < 4; k++) {
                p[k] = &game_object_points[fc->PointNo[k]];
                wx[k] = FACE_OBJ_WORLDX(obj, p[k]);
                wy[k] = FACE_OBJ_WORLDY(obj, p[k]);
                wz[k] = FACE_OBJ_WORLDZ(obj, p[k]);
                /* Per-vertex depth: faces are real 3D surfaces, so a single
                 * per-face depth makes overlapping/curved faces z-fight. */
                sd[k] = face_scrd((float)wx[k], (float)wy[k], (float)wz[k]);
            }

            base = face_vert_count;
            face_emit_vert(wx[0], wy[0], wz[0], u0, v0c, pg, 200, sd[0]);
            face_emit_vert(wx[1], wy[1], wz[1], u1, v1c, pg, 200, sd[1]);
            face_emit_vert(wx[2], wy[2], wz[2], u2, v2c, pg, 200, sd[2]);
            face_emit_vert(wx[3], wy[3], wz[3], u3, v3c, pg, 200, sd[3]);
            /* Quad diagonal is PN1-PN2, matching draw_object_face4d_textrd:
             * triangles (PN0,PN2,PN1) + (PN3,PN1,PN2). A naive (0,1,2)+(0,2,3)
             * fan splits the wrong diagonal and leaves triangular gaps. */
            face_index[face_index_count++] = base + 0;
            face_index[face_index_count++] = base + 2;
            face_index[face_index_count++] = base + 1;
            face_index[face_index_count++] = base + 3;
            face_index[face_index_count++] = base + 1;
            face_index[face_index_count++] = base + 2;
        }

        /* --- Triangles (face3) --- */
        for (f = 0; f < obj->NumbFaces; f++) {
            struct HwrObjFace3 *fc = &game_object_faces3[obj->StartFace + f];
            /* Triangles index game_face_textures (struct SingleTexture, 3 UVs)
             * via set_face_texture_uv - NOT game_textures (the floor/quad array).
             * Using the wrong array sampled empty texels -> magenta/gaps. */
            int texidx = fc->Texture;
            int flat = (texidx == 0);
            struct HwrFaceTex *tx = NULL;
            uint8_t pg, u0,v0c,u1,v1c,u2,v2c;
            struct HwrSinglePoint *p[3];
            int wx[3], wy[3], wz[3], k, base;
            float sd[3];

            if (!flat && texidx >= face_textures_limit)
                texidx = 0;   /* clamp like set_face_texture_uv, still render */
            if (face_vert_count + 3 > HWR_FACE_MAX_VERTS ||
                face_index_count + 3 > HWR_FACE_MAX_INDEX)
                break;
            if (flat) {
                pg = 255;
                u0=v0c=u1=v1c=u2=v2c=0;
            } else {
                tx = &game_face_textures[texidx];
                pg = tx->Page;
                u0=tx->TMapX1; v0c=tx->TMapY1; u1=tx->TMapX2; v1c=tx->TMapY2;
                u2=tx->TMapX3; v2c=tx->TMapY3;
            }

            for (k = 0; k < 3; k++) {
                p[k] = &game_object_points[fc->PointNo[k]];
                wx[k] = FACE_OBJ_WORLDX(obj, p[k]);
                wy[k] = FACE_OBJ_WORLDY(obj, p[k]);
                wz[k] = FACE_OBJ_WORLDZ(obj, p[k]);
                sd[k] = face_scrd((float)wx[k], (float)wy[k], (float)wz[k]);
            }

            base = face_vert_count;
            face_emit_vert(wx[0], wy[0], wz[0], u0, v0c, pg, 200, sd[0]);
            face_emit_vert(wx[1], wy[1], wz[1], u1, v1c, pg, 200, sd[1]);
            face_emit_vert(wx[2], wy[2], wz[2], u2, v2c, pg, 200, sd[2]);
            face_index[face_index_count++] = base + 0;
            face_index[face_index_count++] = base + 1;
            face_index[face_index_count++] = base + 2;
        }
    }

    out->verts = face_verts;
    out->vert_count = face_vert_count;
    out->indices = face_index;
    out->index_count = face_index_count;
    return face_index_count;
}

static int sw_get_lights(void *ctx, HwrLight *out, int max)
{
    (void)ctx; (void)out; (void)max;
    return 0;   /* Phase 5 */
}

static int sw_get_sprites(void *ctx, HwrBillboard *out, int max)
{
    (void)ctx; (void)out; (void)max;
    return 0;   /* Phase 6 */
}

static const uint8_t *sw_get_palette(void *ctx)
{
    (void)ctx;
    return (const uint8_t *)display_palette;
}

static int sw_get_texture_pages(void *ctx, HwrTexturePages *out)
{
    int p;
    (void)ctx;
    if (out == NULL)
        return -1;
    /* Pack the 18 indexed pages contiguously once; they are static art. */
    if (!floor_pages_ready) {
        int any = 0;
        for (p = 0; p < HWR_TMAP_PAGES; p++) {
            uint8_t *dst = floor_pages + p * (HWR_TMAP_DIM * HWR_TMAP_DIM);
            if (vec_tmap[p] != NULL) {
                memcpy(dst, vec_tmap[p], HWR_TMAP_DIM * HWR_TMAP_DIM);
                any = 1;
            } else {
                memset(dst, 0, HWR_TMAP_DIM * HWR_TMAP_DIM);
            }
        }
        if (any)
            floor_pages_ready = 1;
    }
    if (!floor_pages_ready)
        return -1;       /* art not loaded yet; try again next frame */
    out->texels = floor_pages;
    out->width  = HWR_TMAP_DIM;
    out->height = HWR_TMAP_DIM;
    out->count  = HWR_TMAP_PAGES;
    return 0;
}

static int sw_get_key_index(void *ctx)
{
    (void)ctx;
    return hwr_sw_key_index;
}

static HwrSceneSource sw_source = {
    NULL,           /* ctx */
    NULL,           /* begin_frame */
    sw_get_camera,
    sw_get_floor,
    sw_get_faces,
    sw_get_lights,
    sw_get_sprites,
    sw_get_palette,
    sw_get_texture_pages,
    sw_get_key_index,
};

/** Return the Syndicate Wars scene source, configured for the given viewport. */
const HwrSceneSource *hwr_sw_source(int view_w, int view_h)
{
    sw_view_w = view_w;
    sw_view_h = view_h;
    return &sw_source;
}

/** Invalidate cached static art (e.g. on level change) so texture pages and the
 *  key index are refreshed. */
void hwr_sw_source_reset(void)
{
    floor_pages_ready = 0;
}
