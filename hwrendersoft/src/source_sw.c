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
#include "hwr_lights.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "hwr_api.h"

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
struct HwrFullLight {       /* == struct FullLight, sizeof 32 */
    int16_t  Intensity, TrueIntensity, Command, NextFull;
    int16_t  X, Y, Z;
    int16_t  lgtfld_E, lgtfld_10, lgtfld_12;
    uint8_t  lgtfld_14[10];
    uint16_t Flags;
};
struct HwrQuickLight {      /* == struct QuickLight, sizeof 6 */
    uint16_t Ratio, Light, NextQuick;
};
struct HwrThingMini {       /* first bytes of struct Thing, sizeof 168 */
    int16_t  Parent, Next, LinkParent, LinkChild;
    uint8_t  SubType, Type;
    int16_t  State;
    uint32_t Flag;
    int16_t  LinkSame, LinkSameGroup, Radius, ThingOffset;
    int32_t  X, Y, Z;
};
struct HwrSimpleThingMini { /* == struct SimpleThing, sizeof 60 */
    int16_t  Parent, Next, LinkParent, LinkChild;
    uint8_t  SubType, Type;
    int16_t  State;
    uint32_t Flag;
    int16_t  LinkSame, Object, Radius, ThingOffset;
    int32_t  X, Y, Z;
    int16_t  Frame, StartFrame, Timer1, StartTimer1;
    int16_t  U_Frame, U_StartFrame, U_LightHead, U_LightDie, U_LightAnim, U_Health;
    int16_t  Owner2;
    uint16_t UniqueID;
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

extern struct HwrFullLight  *game_full_lights;   /* == game_full_lights     */
extern uint16_t              next_full_light;     /* active count            */

extern struct HwrQuickLight *game_quick_lights;  /* == game_quick_lights    */
extern uint16_t              next_quick_light;

extern uint16_t              next_object_face3;   /* count of Face3 entries  */
extern uint16_t              next_object_face4;   /* count of Face4 entries  */

extern char                 *things;              /* == struct Thing array   */

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

int hwr_sw_camera_snapshot(int32_t *xc, int32_t *yc, int32_t *zc,
    int32_t *d10, int32_t *d14, int32_t *d18, int32_t *d1c,
    int32_t *d3c, int32_t *d40, int32_t *scale, int32_t *persp)
{
    if (!snap.valid) return 0;
    *xc = snap.xc; *yc = snap.yc; *zc = snap.zc;
    *d10 = snap.D10; *d14 = snap.D14; *d18 = snap.D18; *d1c = snap.D1C;
    *d3c = snap.D3C; *d40 = snap.D40;
    *scale = snap.scale;
    *persp = snap.persp;
    return 1;
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

/* Geometric ambient occlusion for a floor corner. The grid corner (cgx,cgz) is
 * shared by up to 4 cells; each one that is a building/column footprint
 * (Texture==0) is a vertical occluder. Returns an "openness" byte (255 = fully
 * open, lower = more occluded) baked into the vertex so the shader darkens the
 * ambient fill at the base of walls and in corners. */
static uint8_t corner_ao(int cgx, int cgz)
{
    int dx, dz, occ = 0;
    for (dz = -1; dz <= 0; dz++) {
        for (dx = -1; dx <= 0; dx++) {
            int nx = clampi(cgx + dx, 0, HWR_MAP_TILE_WIDTH - 1);
            int nz = clampi(cgz + dz, 0, HWR_MAP_TILE_WIDTH - 1);
            if (game_my_big_map[HWR_MAP_TILE_WIDTH * nz + nx].Texture == 0)
                occ++;        /* building/column cell touching this corner */
        }
    }
    /* Each occluding cell darkens this corner; clamp so a corner boxed in by
     * buildings stays dim rather than wrapping past zero, and never fully black. */
    {
        int open = 255 - occ * 56;
        if (open < 31) open = 31;
        return (uint8_t)open;
    }
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
            /* Per-corner geometric AO: darkens the ambient fill where the floor
             * meets buildings/columns. Replaces the SW per-tile ShadeR (which is
             * near-flat on open ground and reads as no occlusion). */
            (void)inherited_shade;
            v[0].light = corner_ao(gx,     gz);
            v[1].light = corner_ao(gx + 1, gz);
            v[2].light = corner_ao(gx + 1, gz + 1);
            v[3].light = corner_ao(gx,     gz + 1);
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

/* Local cap matching HWR_MAX_LIGHTS in hwr_floor.c — keep in sync. */
#define SW_LIGHTS_MAX 64

static int sw_get_lights(void *ctx, HwrLight *out, int max)
{
    /* Pick the nearest `max` lights to the camera (XZ plane).
     * Keeps a fixed-size set of closest candidates: O(N*max) per frame,
     * fine for N<=4000 and max<=SW_LIGHTS_MAX. */
    struct { int idx; int dist2; } nearest[SW_LIGHTS_MAX];
    int nnearest = 0;
    int cx, cz, i, j;
    float rmul, sstr;
    (void)ctx;

    if (game_full_lights == NULL || out == NULL || max <= 0)
        return 0;
    if (max > SW_LIGHTS_MAX)
        max = SW_LIGHTS_MAX;

    /* --- Debug: dump all light IDs when light_debug is enabled --- */
    if (hwr_lights_defaults().light_debug) {
        static int dumped = 0;
        if (!dumped) {
            FILE *dbg = fopen("fx3d_light_ids.txt", "w");
            if (dbg) {
                int nlights = (int)next_full_light - 1;
                int hist[55] = {0};
                fprintf(dbg, "Light intensity histogram (%d lights, bucket = 25 units)\n", nlights);
                fprintf(dbg, "========================================================\n");
                fprintf(dbg, "Categories from LightHead owner's Thing (Type,SubType).\n\n");
                for (i = 1; i < (int)next_full_light; i++) {
                    int intens = (int)game_full_lights[i].Intensity;
                    int bucket = intens / 25;
                    if (bucket < 0) bucket = 0;
                    if (bucket > 54) bucket = 54;
                    hist[bucket]++;
                }
                for (i = 0; i < 55; i++) {
                    if (hist[i] > 0) {
                        int lo = i * 25;
                        int hi = (i + 1) * 25 - 1;
                        int bars = hist[i] / 5;
                        if (bars < 1 && hist[i] > 0) bars = 1;
                        fprintf(dbg, "Int %3d-%-3d: %4d ", lo, hi, hist[i]);
                        while (bars--) putc('#', dbg);
                        putc('\n', dbg);
                    }
                }
                fprintf(dbg, "\nSample config for [defaultlighting]:\n");
                fprintf(dbg, "filler_brightness   = 0.2   (dim fillers)\n");
                fprintf(dbg, "building_brightness  = 0.8   (subtle buildings)\n");
                fprintf(dbg, "street_brightness    = 1.5   (vivid streetlamps)\n");
                fclose(dbg);
            }
            dumped = 1;
        }
    }

    /* --- Per-(Type,SubType) category cache for all lights ----------------- */
    #define HWR_THING_CACHE_LEN 4096
    static int cached_type[HWR_THING_CACHE_LEN];
    static int cached_sub[HWR_THING_CACHE_LEN];
    static int cached_valid = 0;
    {
        int nfl = (int)next_full_light;
        if (nfl > HWR_THING_CACHE_LEN) nfl = HWR_THING_CACHE_LEN;
        memset(cached_type, 0, sizeof(cached_type));
        memset(cached_sub, 0, sizeof(cached_sub));
        /* Only LightHead ownership determines a light's type for category
         * purposes.  We DO NOT traverse faces here: a face references a
         * light for illumination, not ownership.  Unconnected lights (no
         * SimpleThing LightHead chain) fall back to intensity-based
         * category in the output loop below. */
        /* Override with LightHead ownership: the SimpleThing that OWNS
         * a FullLight (via LightHead→NextFull chain) determines its type,
         * NOT the objects whose faces reference it for illumination.
         * STHINGS_LIMIT = 1500 — do NOT go past this or garbage data can
         * overwrite valid cached_type entries for real lights. */
        {
            extern char *things;
            int max_si = 1500;
            for (int si = 1; si < max_si; si++) {
                struct HwrSimpleThingMini *st = (struct HwrSimpleThingMini *)((char *)things - si * 60);
                if (st->Type == 0 || st->U_LightHead == 0) continue;
                int fidx = st->U_LightHead;
                int visited = 0;
                while (fidx > 0 && fidx < (uint16_t)nfl) {
                    if (st->Type > 0) {
                        cached_type[fidx] = st->Type;
                        cached_sub[fidx]  = st->SubType;
                    }
                    fidx = game_full_lights[fidx].NextFull;
                    if (++visited > 100) break;
                }
            }
        }
        cached_valid = 1;
    }

    {
        HwrLightDefaults ld = hwr_lights_defaults();
        rmul = ld.radius;
        sstr = ld.shadow_strength;
    }
    cx = snap.xc;
    cz = snap.zc;

    for (i = 1; i < (int)next_full_light; i++) {
        struct HwrFullLight *fl = &game_full_lights[i];
        int dx, dz, d2, worst;
        /* Intensity is signed: negative lights are "anti-lights" the original
         * used to subtract light and fake shadows. shadow_strength <= 0 disables
         * them so they do not even consume light-selection slots. */
        if (fl->Intensity == 0)
            continue;
        if (fl->Intensity < 0 && sstr <= 0.0f)
            continue;
        dx = (int)fl->X - cx;
        dz = (int)fl->Z - cz;
        d2 = dx*dx + dz*dz;

        if (nnearest < max) {
            nearest[nnearest].idx   = i;
            nearest[nnearest].dist2 = d2;
            nnearest++;
        } else {
            /* Replace the furthest candidate if this one is closer. */
            worst = 0;
            for (j = 1; j < nnearest; j++)
                if (nearest[j].dist2 > nearest[worst].dist2)
                    worst = j;
            if (d2 < nearest[worst].dist2) {
                nearest[worst].idx   = i;
                nearest[worst].dist2 = d2;
            }
        }
    }

    for (i = 0; i < nnearest; i++) {
        struct HwrFullLight *fl = &game_full_lights[nearest[i].idx];
        HwrLightColor col = hwr_lights_lookup((int)fl->Command);
        out[i].x = (float)fl->X + 70.0f;    /* 70 PRC east */
        out[i].y = (float)fl->Y;
        out[i].z = (float)fl->Z + 50.0f;    /* 50 PRC south */
        if (fl->Intensity < 0) {
            /* Anti-light: shader reads .r as the darkening amount and the
             * negative radius as the flag. Scale by |Intensity| vs the ~64 the
             * level's lamps use, times the global shadow strength. */
            int ai = -(int)fl->Intensity;
            out[i].r = out[i].g = out[i].b = sstr * ((float)ai / 64.0f);
            /* Inverse-square attenuation constant: Intensity * 34019 (= 1088608/32)
             * matching the SW super-quick-light formula. rmul/21 normalises so
             * ini radius=21 gives exact SW behaviour. */
            out[i].radius = -((float)ai * 34019.0f * (rmul / 21.0f) * col.intensity_scale);
        } else {
            /* Category from LightHead owner's Thing (Type,SubType). */
            HwrLightDefaults ld = hwr_lights_defaults();
            int intens = (int)fl->Intensity;
            int lidx = nearest[i].idx;
            int cat = 0; /* 0=auto, 1=filler, 2=building, 3=street */
            float cat_bright = ld.filler_brightness;
            if (cached_valid && lidx > 0 && lidx < HWR_THING_CACHE_LEN) {
                int tt = cached_type[lidx], ts = cached_sub[lidx];
                if (tt > 0) {
                    int oc = hwr_thing_category_get(tt, ts);
                    if (oc >= 1 && oc <= 3) { cat = oc; }
                    if (oc == 1) cat_bright = ld.filler_brightness;
                    else if (oc == 2) cat_bright = ld.building_brightness;
                    else if (oc == 3) cat_bright = ld.street_brightness;
                }
            }
            /* Per-category radius factor.
             * cat_radius / 21 normalises to the SW standard (21 = exact original). */
            float cat_radius = ld.filler_radius;
            if (cat == 1) cat_radius = ld.filler_radius;
            else if (cat == 2) cat_radius = ld.building_radius;
            else if (cat == 3) cat_radius = ld.street_radius;
            float radius_scale = cat_radius / 21.0f;
            out[i].r = col.r * col.brightness * cat_bright;
            out[i].g = col.g * col.brightness * cat_bright;
            out[i].b = col.b * col.brightness * cat_bright;
            out[i].radius = (float)fl->Intensity * 34019.0f * radius_scale * col.intensity_scale;
            /* Per-light distance cull scales with the per-category radius.
             * 4194304 = (8 tiles * 256 PRC/tile)² at default radius_scale=1. */
            out[i].max_dist2 = 4194304.0f * radius_scale;
        }
    }
    return nnearest;
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
