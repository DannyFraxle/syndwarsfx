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
#include <stdlib.h>
#include <SDL.h>

#include "hwr_api.h"
#include "xbr.h"

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
/* Sprite-suppression gate, set by hwrender_glue.c. */
extern int            engine_hwr_suppress_sprites;
/* Current 6-bit-per-channel palette (256 RGB triplets). */
extern unsigned char  display_palette[768];
/* 8-bit-per-channel palette (SDL_Color equivalent) for xBR RGBA conversion. */
extern struct { unsigned char r, g, b, a; } lbPaletteColors[256];

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
struct HwrNormal {          /* == struct Normal, sizeof 16 (object-space normal) */
    int32_t NX, NY, NZ, LightRatio;
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
extern struct HwrNormal     *game_normals;       /* == game_normals         */
extern uint16_t              next_normal;        /* count of normals        */

extern struct HwrFullLight  *game_full_lights;   /* == game_full_lights     */
extern uint16_t              next_full_light;     /* active count            */

extern struct HwrQuickLight *game_quick_lights;  /* == game_quick_lights    */
extern uint16_t              next_quick_light;

extern uint16_t              next_object_face3;   /* count of Face3 entries  */
extern uint16_t              next_object_face4;   /* count of Face4 entries  */

extern char                 *things;              /* == struct Thing array   */
extern short                 current_level;        /* level token for cache invalidation */
extern unsigned short        current_map;

/* Vehicle rotation matrices. HwrM33 mirrors struct M33 (3x3 int32, sizeof=36).
 * local_mats[] and next_local_mat are resolved at the final executable link.  */
typedef struct { int32_t R[3][3]; } HwrM33;
extern HwrM33    local_mats[100];
extern uint16_t  next_local_mat;
/* Byte-offset constants derived from struct Thing (sizeof=168, #pragma pack(1)).
 * Union U starts at byte 76; MatrixIndex (int16) is at union+8 = byte 84.   */
#define HWR_THING_SIZEOF  168
#define HWR_THING_MATX    84

/* The palette index reserved as the composite key (set by the host glue). */
int hwr_sw_key_index = 0;

/* Number of vehicle (headlight/tail) lights appended at the END of the array
 * returned by sw_get_lights this frame. The reflective-paint pass reads this to
 * exclude vehicle lights so a car's own lamps don't self-illuminate its paint. */
int hwr_sw_vehicle_lights = 0;

/* --- Sprite billboard collection (Phase 6) --- */

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

/* Per-object snapshot of moving-Thing state (position + rotation matrix index),
 * captured at floor-draw time together with the camera so that vehicle faces
 * render on the SAME sim-turn time base as the camera and the sprites.
 *
 * Vehicle face geometry is built in sw_get_faces() at PRESENT time, which is one
 * process_things() tick ahead of the camera snapshot the present uses. Reading
 * live things[]/local_mats[] there drew the body one turn ahead of the camera
 * frame, so it swam against the (static, camera-consistent) road as the camera
 * moved. Sprites never had this because they are collected here at gate time.
 * Indexed by object index (matches face_obj_seen). */
#define HWR_MAX_SNAP_OBJS 65536
#define HWR_TT_VEHICLE          0x2    /* enum ThingType TT_VEHICLE  */
#define HWR_TT_BUILDING         0x9    /* enum ThingType TT_BUILDING */
#define HWR_SubTT_BLD_MGUN      0x20   /* stationary turret (mounted gun) */
#define HWR_SubTT_BLD_MOVN_ROTOR 0x36  /* rotating machinery part         */
static struct {
    int32_t  tx, ty, tz;   /* world position (X>>8, Y>>5 or >>8, Z>>8) at capture */
    int16_t  matx;         /* MatrixIndex, or <=0 for none                        */
    uint8_t  is_dynamic;   /* 1 = position from Thing + matrix (vehicle/turret/rotor) */
    uint8_t  is_vehicle;   /* 1 = TT_VEHICLE — also skip SW-drawn reflective faces    */
} obj_snap[HWR_MAX_SNAP_OBJS];
static unsigned obj_snap_count = 0;   /* objects captured this frame */
static int      obj_snap_valid = 0;
static HwrM33   snap_local_mats[100]; /* local_mats copy at capture time */

/* Manual struct definitions matching the game's SortSprite / DrawItem / Frame /
 * Element / TbSprite layouts (packed 1-byte, matching the game's headers).
 * We can't include the game headers directly because libhwrender is meant to
 * be buildable standalone. The tags match the extern declarations below. */
#pragma pack(push, 1)
struct DrawItem {
    uint8_t  Type;
    uint16_t Offset;
    uint16_t Child;
};
struct SortSprite {
    int16_t  X, Y, Z;
    uint16_t Frame;
    intptr_t SrcItem;
    uint8_t  Brightness;
    uint8_t  Angle;
    int16_t  Scale;
};
struct Frame {
    uint16_t FirstElement;
    uint8_t  SWidth, SHeight;
    uint8_t  FX, Flags;
    uint16_t Next;
};
struct Element {
    uint16_t ToSprite;
    int16_t  X, Y;
    uint16_t Flags;
    uint16_t Next;
};
struct TbSprite {
    uint8_t *Data;
    uint8_t  SWidth, SHeight;
};
#pragma pack(pop)

#include "hwr_sprite.h"

/* Draw item types from enginbckt.h (raw hex values to avoid cross-library include). */
#define HWR_DI_SFrmStatc  0x03
#define HWR_DI_SFrmPersV  0x0D
#define HWR_DI_Unkn15     0x0F
#define HWR_DI_SFrmPersB  0x1C
#define HWR_DI_SFrmEfctV  0x1D
#define HWR_SMTT_DROPPED_ITEM 0x19   /* SimpleThing Type for a dropped (collectable) item */

/* The SW sort-sprite, draw-list and frame/sprite arrays. */
extern struct SortSprite *game_sort_sprites;
extern unsigned short     next_sort_sprite;
extern struct DrawItem   *game_draw_list;
extern unsigned short     next_draw_item;

extern struct Frame      *frame, *frame_end;
extern struct Element    *melement_ani, *mele_ani_end;
extern struct TbSprite   *m_sprites, *m_sprites_end;

/* Pre-collected billboard storage (filled by hwr_sw_collect_sprites,
 * consumed by sw_get_sprites). */
#define HWR_MAX_COLLECTED 2048
static HwrBillboard hwr_collected_billboards[HWR_MAX_COLLECTED];
static int          hwr_collected_count = 0;
static int          hwr_xbr_count = 0;

/* Viewport, supplied by the host at creation time. */
static int sw_view_w = 0;
static int sw_view_h = 0;

/* The skip mask the drawlist executor checks — storage defined in
 * engindrwlstx.c (libswrender), linked at the final executable. */
extern unsigned char hwr_sprite_skip_mask[256];

/* render_ghost is a ghosting lookup table used by SW sprite drawing functions.
 * It must be set before ANY sprite drawing; normally it's set inside
 * draw_frame_scaled_alpha(), but the first sprite draw can be a frame that goes
 * through draw_frame_scaled_alpha_frv() which does NOT set it. We initialise it
 * here so all code paths have a valid table. */
extern unsigned char *render_ghost;
extern struct {
    unsigned char fade_table[64 * 256];
    unsigned char ghost_table[256 * 256];
} pixmap;

void hwr_sw_collect_sprites(void)
{
    unsigned short i;
    hwr_collected_count = 0;
    hwr_xbr_count = 0;
    int hwr_eligible_count = 0;
    int hwr_passed_count = 0;
    memset(hwr_sprite_skip_mask, 0, sizeof(hwr_sprite_skip_mask));
    render_ghost = &pixmap.ghost_table[0];

    if (!snap.valid || game_draw_list == NULL || game_sort_sprites == NULL)
        return;
    if (frame == NULL || m_sprites == NULL || melement_ani == NULL)
        return;

    for (i = 1; i < next_draw_item && hwr_collected_count < HWR_MAX_COLLECTED; i++) {
        struct DrawItem *itm = &game_draw_list[i];
        int eligible = 0;

        switch (itm->Type) {
        case HWR_DI_SFrmStatc:
        case HWR_DI_SFrmPersV:
        case HWR_DI_Unkn15:
        case HWR_DI_SFrmPersB:
        case HWR_DI_SFrmEfctV:
            eligible = 1;
            break;
        default:
            break;
        }
        if (!eligible) continue;
        hwr_eligible_count++;

        unsigned short ss_idx = itm->Offset;
        if (ss_idx >= next_sort_sprite) continue;

        struct SortSprite *ss = &game_sort_sprites[ss_idx];
        if (ss->SrcItem == 0) continue;

        struct HwrSimpleThingMini *thing;
        thing = (struct HwrSimpleThingMini *)ss->SrcItem;
        if (thing->Type == 0) continue;
        hwr_passed_count++;

        {
            unsigned short frm_idx = ss->Frame;
            if (frm_idx >= (unsigned short)(frame_end - frame))
                continue;
            struct Frame *frm = &frame[frm_idx];

            /* Atlas key: (frame_index, frv_pack, xbr_scale).
             * NEITHER Angle NOR per-sprite brightness is in the key.  The
             * composited pixels depend only on the frame, the frv version bits
             * (packed in Scale) and the xBR scale; element selection is driven by
             * the frv bits, not Angle.  Brightness (and the angle-gated +15 bonus)
             * is applied at DRAW time via bb->shade, not baked, so identical
             * sprites at different brightness share one slot instead of colliding
             * (all showing whichever brightness baked first) or re-baking as the
             * sprite turns — the light/dark "blink".  This also keeps the
             * non-reclaiming atlas from exhausting (which blacklisted keys and
             * SKIPPED sprites — the old "randomly darkening" symptom). */
            uint16_t frv_pack = ss->Scale;
            uint8_t angle = ss->Angle;
            int xbr_key = hwr_lights_defaults().xbr_scale;
            uint32_t key = ((uint32_t)frm_idx << 18)
                         | ((uint32_t)(frv_pack & 0x3FFF) << 4)
                         | ((uint32_t)(xbr_key & 0x03) << 2);

            /* FRV version unpack helper */
            int frv_arr[5];
            frv_arr[0] = (frv_pack >> 0) & 0x07;
            frv_arr[1] = (frv_pack >> 3) & 0x07;
            frv_arr[2] = (frv_pack >> 6) & 0x07;
            frv_arr[3] = (frv_pack >> 9) & 0x07;
            frv_arr[4] = (frv_pack >> 12) & 0x07;

            /* Compute element bounding box (version check always — same as SW) */
            unsigned short el_idx;
            int off_x, off_y, max_x, max_y;
            off_x = 0x7FFFFFFF; off_y = 0x7FFFFFFF;
            max_x = -0x7FFFFFFF; max_y = -0x7FFFFFFF;
            for (el_idx = frm->FirstElement; el_idx > 0; ) {
                struct Element *el;
                if (el_idx >= (unsigned short)(mele_ani_end - melement_ani))
                    break;
                el = &melement_ani[el_idx];
                if (el->ToSprite > 0) {
                    struct TbSprite *spr;
                    spr = (struct TbSprite *)((uint8_t *)m_sprites + el->ToSprite);
                    if (spr > m_sprites && spr < m_sprites_end) {
                        int frv_idx = (el->Flags >> 4) & 0x1F;
                        if (frv_idx >= 5) { el_idx = el->Next; continue; }
                        int frv_ver = (el->Flags >> 9) & 0x07;
                        if (frv_arr[frv_idx] != frv_ver) { el_idx = el->Next; continue; }
                        int ex = (int)(el->X) >> 1;
                        int ey = (int)(el->Y) >> 1;
                        int sw = spr->SWidth;
                        int sh = spr->SHeight;
                        if (ex < off_x) off_x = ex;
                        if (ey < off_y) off_y = ey;
                        if (ex + sw > max_x) max_x = ex + sw;
                        if (ey + sh > max_y) max_y = ey + sh;
                    }
                }
                el_idx = el->Next;
            }
            if (off_x == 0x7FFFFFFF) off_x = 0;
            if (off_y == 0x7FFFFFFF) off_y = 0;
            int fw = max_x - off_x;
            int fh = max_y - off_y;
            if (fw <= 0 || fh <= 0 || fw > 256 || fh > 256)
                continue;

            /* Check atlas cache BEFORE expensive composite + xBRZ.
             * -2 = blacklisted (atlas full, never retry). */
            int slot = hwr_atlas_find(key);
            if (slot == -1) {
                /* Not cached — composite, upscale, register */
                int row, col;
                uint8_t comp[256 * 256 * 4];
                memset(comp, 0, (size_t)fw * fh * 4);

                for (el_idx = frm->FirstElement; el_idx > 0; ) {
                    struct Element *el;
                    if (el_idx >= (unsigned short)(mele_ani_end - melement_ani))
                        break;
                    el = &melement_ani[el_idx];
                    if (el->ToSprite > 0) {
                        struct TbSprite *spr;
                        spr = (struct TbSprite *)((uint8_t *)m_sprites + el->ToSprite);
                        if (spr > m_sprites && spr < m_sprites_end) {
                            int frv_idx = (el->Flags >> 4) & 0x1F;
                            if (frv_idx >= 5) { el_idx = el->Next; continue; }
                            int frv_ver = (el->Flags >> 9) & 0x07;
                            if (frv_arr[frv_idx] != frv_ver) { el_idx = el->Next; continue; }

                            int spr_w = spr->SWidth;
                            int spr_h = spr->SHeight;
                            int el_x = ((int)(el->X) >> 1) - off_x;
                            int el_y = ((int)(el->Y) >> 1) - off_y;
                            int flip_h = (el->Flags & 0x0001) != 0;

                            if (spr_w > 0 && spr_h > 0 && spr_w <= 256 && spr_h <= 256) {
                                uint8_t temp[256 * 256];
                                uint8_t opq[256 * 256];
                                memset(temp, 0, sizeof(temp));
                                memset(opq, 0, sizeof(opq));
                                if (hwr_rle_decode_opaque(spr->Data, temp, opq, spr_w, spr_h) == 0) {
                                    /* Bake EVERY sprite at a fixed full brightness so that all
                                     * instances of a sprite share one atlas slot regardless of
                                     * their per-instance Brightness.  Brightness (and the
                                     * angle-gated +15 bonus) is applied at draw time via
                                     * bb->shade — see the fill block below.  Baking per-instance
                                     * brightness here made identical sprites collide on one slot
                                     * and all show whichever brightness baked first, and the
                                     * angle-gated bonus re-baked them as they turned: the
                                     * light/dark blink on walking/running characters. */
                                    int bri = 60;
                                    int use_remap = frv_idx != 4;
                                    for (row = 0; row < spr_h && el_y + row < fh; row++) {
                                        for (col = 0; col < spr_w && el_x + col < fw; col++) {
                                            int idx = row * spr_w + col;
                                            int dst_x = flip_h ? (el_x + spr_w - 1 - col) : (el_x + col);
                                            int dst = ((el_y + row) * fw + dst_x) * 4;
                                            int pixel = temp[idx];
                                            if (use_remap)
                                                pixel = pixmap.fade_table[bri * 256 + pixel];
                                            if (opq[idx]) {
                                                comp[dst + 0] = lbPaletteColors[pixel].r;
                                                comp[dst + 1] = lbPaletteColors[pixel].g;
                                                comp[dst + 2] = lbPaletteColors[pixel].b;
                                                comp[dst + 3] = 255;
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                    el_idx = el->Next;
                }

                {
                    int sf = hwr_lights_defaults().xbr_scale;
                    uint8_t *reg_pixels = comp;
                    int reg_w = fw, reg_h = fh;
                    uint8_t *scaled = NULL;
                    if (sf >= 2 && sf <= 4
                        && fw * sf <= 4096 && fh * sf <= 4096)
                    {
                        int sw = fw * sf, sh = fh * sf;
                        scaled = (uint8_t *)malloc((size_t)sw * sh * 4);
                        if (scaled) {
                        if (xbr_scale(comp, scaled, fw, fh, sf) == 0) {
                            reg_pixels = scaled;
                            reg_w = sw; reg_h = sh;
                            hwr_xbr_count++;
                        } else {
                                free(scaled);
                                scaled = NULL;
                            }
                        }
                    }
                    slot = hwr_atlas_register(key, reg_pixels, reg_w, reg_h);
                    if (slot < 0 && reg_pixels != comp) {
                        fprintf(stderr, "xbr FALLBACK: key=%08x 4x(%dx%d) failed, trying 1x(%dx%d)\n",
                            key, reg_w, reg_h, fw, fh);
                        slot = hwr_atlas_register(key, comp, fw, fh);
                    }
                    if (slot < 0)
                        fprintf(stderr, "xbr FAIL: key=%08x slot<0 after fallback\n", key);
                    if (scaled) free(scaled);
                }
                if (slot < 0)
                    continue;
            }

            /* Fill billboard */
            {
                HwrBillboard *bb = &hwr_collected_billboards[hwr_collected_count];
                float wx = (float)(thing->X >> 8);
                float wy = (float)(thing->Y >> 5);
                float wz = (float)(thing->Z >> 8);
                bb->x = wx;
                bb->y = wy;
                bb->z = wz;
                bb->sprite = (uint16_t)slot;
                /* U_LightHead is only valid for SimpleThing pointers (static/effect
                 * draw items).  Person draw items (PersV/PersB) have a struct Thing
                 * as SrcItem where offset 48 is Timer1, not LightHead — reading it
                 * would incorrectly set NOSHADOW and suppress blob shadows. */
                int is_person = (itm->Type == HWR_DI_SFrmPersV
                              || itm->Type == HWR_DI_SFrmPersB);
                int is_emitter = (!is_person && thing && thing->U_LightHead > 0);
                /* Sprites bake at a fixed brightness (above); apply the per-instance
                 * Brightness here as the draw-time shade, including the angle-gated
                 * +15 bonus.  Because this is per frame (not baked), identical
                 * sprites no longer collide on one baked brightness and turning no
                 * longer re-bakes/blinks. */
                {
                    int bonus = (frv_arr[4] != 0 && angle > 1 && angle < 7) ? 15 : 0;
                    int sh = (int)ss->Brightness + bonus;
                    if (sh < 10) sh = 10;
                    if (sh > 75) sh = 75;
                    bb->shade = (uint8_t)sh;
                }
                /* NOSHADOW (light emitters don't cast blob shadows) is a separate
                 * shadow-casting concern, unrelated to brightness. */
                bb->flags = is_emitter ? HWR_BILLBOARD_NOSHADOW : 0;
                /* Dropped items sit at the same spot as the dead body that
                 * dropped them; bias them toward the camera so they always draw
                 * on top and stay easy to click. */
                if (itm->Type == HWR_DI_SFrmStatc && thing->Type == HWR_SMTT_DROPPED_ITEM)
                    bb->flags |= HWR_BILLBOARD_ONTOP;
                {
                float sc = (float)snap.scale;
                if (sc <= 0.0f) sc = 256.0f;
                float rnorm = sqrtf((float)snap.D14 * snap.D14 + (float)snap.D10 * snap.D10);
                float res_scale = (sw_view_h > 0) ? (float)sw_view_h / 480.0f : 1.0f;
                if (rnorm > 0.001f && snap.D1C != 0) {
                    bb->half_size_x = (float)fw * 100663296.0f / (sc * rnorm) * 0.85f * res_scale;
                    bb->half_size_y = (float)fh * 100663296.0f / (sc * (float)snap.D1C) * 0.85f * res_scale;
                } else {
                    bb->half_size_x = (float)fw * 18.0f * 0.85f * res_scale;
                    bb->half_size_y = (float)fh * 18.0f * 0.85f * res_scale;
                }
                /* Shift billboard up by half-height so feet (at comp bottom) align
                 * with the thing's world Y (feet position), not the quad centre. */
                bb->y += bb->half_size_y;
            }
            hwr_collected_count++;
                hwr_sprite_skip_mask[ss_idx >> 3] |= (uint8_t)(1 << (ss_idx & 7));
            }
        }
    }

    /* ---- KP-7 one-shot debug dump (xBR/billboard stats) ---- */
    {
        static int prev_kp7 = 0;
        const Uint8 *keys = SDL_GetKeyboardState(NULL);
        int kp7 = keys ? keys[SDL_SCANCODE_KP_7] : 0;
        if (kp7 && !prev_kp7) {
            FILE *df = fopen("fx3d_sprites_debug.txt", "w");
            if (df) {
                int di;
                int xbr_sf = hwr_lights_defaults().xbr_scale;
                fprintf(df, "xbr_scale=%d  xbr_active=%s\n", xbr_sf, xbr_sf > 0 ? "YES" : "NO");
                fprintf(df, "=== One-shot frame ===  xbr_done=%d\n", hwr_xbr_count);
                fprintf(df, "cam: xc=%d yc=%d zc=%d D14=%d D1C=%d D10=%d D18=%d D3C=%d D40=%d scale=%d persp=%d\n",
                    snap.xc, snap.yc, snap.zc, snap.D14, snap.D1C, snap.D10, snap.D18,
                    snap.D3C, snap.D40, snap.scale, snap.persp);
                fprintf(df, "collected: %d   eligible=%d passed=%d next_draw_item=%d next_sort_sprite=%d\n",
                    hwr_collected_count, hwr_eligible_count, hwr_passed_count, next_draw_item, next_sort_sprite);
                for (di = 0; di < hwr_collected_count && di < 10; di++) {
                    fprintf(df, " [%d]: pos=(%.0f,%.0f,%.0f) hw=%.0f hh=%.0f slot=%d shade=%d\n",
                        di, hwr_collected_billboards[di].x, hwr_collected_billboards[di].y,
                        hwr_collected_billboards[di].z,
                        hwr_collected_billboards[di].half_size_x,
                        hwr_collected_billboards[di].half_size_y,
                        (int)hwr_collected_billboards[di].sprite,
                        (int)hwr_collected_billboards[di].shade);
                }
                fprintf(df, "skip_mask[0..7]:");
                for (di = 0; di < 8 && di < (int)((next_sort_sprite + 7) / 8); di++)
                    fprintf(df, " %02x", hwr_sprite_skip_mask[di]);
                fprintf(df, "\n  suppress=%d\n", (int)engine_hwr_suppress_sprites);
                fclose(df);
            }
        }
        prev_kp7 = kp7;
    }
}

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

    /* Snapshot moving-Thing object state on the same tick as the camera, so the
     * vehicle faces built later (at present time) match this camera frame. */
    obj_snap_valid = 0;
    obj_snap_count = 0;
    if (game_objects != NULL && things != NULL) {
        unsigned o;
        memcpy(snap_local_mats, local_mats, sizeof(snap_local_mats));
        for (o = 1; o < next_object && o < HWR_MAX_SNAP_OBJS; o++) {
            struct HwrObject *obj = &game_objects[o];
            const struct HwrThingMini *th = NULL;
            int dynamic = 0, y_mul8 = 1, is_veh = 0;

            if (obj->ThingNo > 0) {
                th = (const struct HwrThingMini *)(things +
                    (int)(uint16_t)obj->ThingNo * HWR_THING_SIZEOF);
            }
            /* Decide which objects are positioned dynamically (Thing pos +
             * rotation matrix) vs the cached MapX/OffsetY/MapZ path. The engine
             * draws these via draw_rot_object/2 from the live Thing position:
             *   - TT_VEHICLE                         (Y>>5)
             *   - TT_BUILDING / SubTT_BLD_MGUN       stationary turret (Y>>5)
             *   - TT_BUILDING / SubTT_BLD_MOVN_ROTOR rotating part     (Y>>8)
             * matching thing_position_uses_y_mul_8(). Everything else (regular
             * buildings, gates, statics) keeps the cached path. */
            if (th != NULL) {
                if (th->Type == HWR_TT_VEHICLE) {
                    dynamic = 1; y_mul8 = 1; is_veh = 1;
                } else if (th->Type == HWR_TT_BUILDING &&
                           th->SubType == HWR_SubTT_BLD_MGUN) {
                    dynamic = 1; y_mul8 = 1;
                } else if (th->Type == HWR_TT_BUILDING &&
                           th->SubType == HWR_SubTT_BLD_MOVN_ROTOR) {
                    dynamic = 1; y_mul8 = 0;
                }
            }
            if (dynamic) {
                obj_snap[o].tx = (int32_t)th->X >> 8;   /* PRCCOORD_TO_MAPCOORD */
                obj_snap[o].ty = y_mul8 ? ((int32_t)th->Y >> 5)   /* PRCCOORD_TO_YCOORD */
                                        : ((int32_t)th->Y >> 8);  /* PRCCOORD_TO_MAPCOORD */
                obj_snap[o].tz = (int32_t)th->Z >> 8;
                obj_snap[o].matx = *(const int16_t *)((const char *)th + HWR_THING_MATX);
                obj_snap[o].is_dynamic = 1;
                obj_snap[o].is_vehicle = (uint8_t)is_veh;
            } else {
                obj_snap[o].is_dynamic = 0;
                obj_snap[o].is_vehicle = 0;
            }
        }
        obj_snap_count = o;
        obj_snap_valid = 1;
    }
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

/* --- Reflective (chameleon paint) face buffers, filled alongside sw_get_faces.
 * GFlags&0x80 faces are diverted here instead of into the opaque face batch. */
#define HWR_REFL_MAX_VERTS  (64 * 1024)
#define HWR_REFL_MAX_INDEX  (96 * 1024)
static HwrReflectVertex refl_verts[HWR_REFL_MAX_VERTS];
static uint32_t         refl_index[HWR_REFL_MAX_INDEX];
static int              refl_vert_count = 0;
static int              refl_index_count = 0;

/* Texture pages packed contiguously (18 * 256 * 256) for the GL texture array. */
static uint8_t   floor_pages[HWR_TMAP_PAGES * HWR_TMAP_DIM * HWR_TMAP_DIM];
static int       floor_pages_ready = 0;

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Linear (monotonic) screen depth for a world point; defined below, used by the
 * floor builder so floor corners share the faces' z-buffer scale. */
static float face_scrd(float wx, float wy, float wz);

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

            base = floor_vert_count;
            v = &floor_verts[base];
            /* Corner positions: v[0]=(gx,gz), v[1]=(gx+1,gz),
             *                   v[2]=(gx+1,gz+1), v[3]=(gx,gz+1) */
            v[0].x = (float)(gx << 8);     v[0].z = (float)(gz << 8);     v[0].y = (float)(8 * corner_alt(gx, gz, gx,   gz));
            v[1].x = (float)((gx+1) << 8); v[1].z = (float)(gz << 8);     v[1].y = (float)(8 * corner_alt(gx, gz, gx+1, gz));
            v[2].x = (float)((gx+1) << 8); v[2].z = (float)((gz+1) << 8); v[2].y = (float)(8 * corner_alt(gx, gz, gx+1, gz+1));
            v[3].x = (float)(gx << 8);     v[3].z = (float)((gz+1) << 8); v[3].y = (float)(8 * corner_alt(gx, gz, gx,   gz+1));
            /* Per-vertex linear depth (no perspective clamp), matching the face
             * pass so floor and buildings share one monotonic z-buffer scale.
             * Per-corner (not per-tile-centre) so a tile's far edge reports its
             * true depth and no longer pokes through walls standing on it. */
            v[0].tile_depth = face_scrd(v[0].x, v[0].y, v[0].z);
            v[1].tile_depth = face_scrd(v[1].x, v[1].y, v[1].z);
            v[2].tile_depth = face_scrd(v[2].x, v[2].y, v[2].z);
            v[3].tile_depth = face_scrd(v[3].x, v[3].y, v[3].z);
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

/* Rotate an object-space normal by the object matrix (or identity) and return a
 * unit world-space vector. Mirrors compute_normals_light_ratio's matrix_transform
 * step; the chameleon shader then projects this against the camera factors. */
static void hwr_world_normal(const HwrM33 *m, int nx, int ny, int nz, float out[3])
{
    double wx, wy, wz, len;
    if (m != NULL) {
        wx = (double)m->R[0][0]*nx + (double)m->R[0][1]*ny + (double)m->R[0][2]*nz;
        wy = (double)m->R[1][0]*nx + (double)m->R[1][1]*ny + (double)m->R[1][2]*nz;
        wz = (double)m->R[2][0]*nx + (double)m->R[2][1]*ny + (double)m->R[2][2]*nz;
    } else {
        wx = nx; wy = ny; wz = nz;
    }
    len = wx*wx + wy*wy + wz*wz;
    if (len > 1e-9) {
        len = 1.0 / sqrt(len);
        out[0] = (float)(wx*len); out[1] = (float)(wy*len); out[2] = (float)(wz*len);
    } else {
        out[0] = 0.0f; out[1] = 1.0f; out[2] = 0.0f;
    }
}

/* Emit one reflective (chameleon paint) vertex. */
static void refl_emit_vert(int wx, int wy, int wz, const float n[3],
    float base, float depth)
{
    HwrReflectVertex *o = &refl_verts[refl_vert_count++];
    o->x = (float)wx; o->y = (float)wy; o->z = (float)wz;
    o->nx = n[0]; o->ny = n[1]; o->nz = n[2];
    o->depth = depth;
    o->base = base;
}

/* Rotate a vehicle object point by its M33 matrix.
 * Replicates transform_rot_object_shpoint: inputs scaled ×2, result >>15.
 * Inputs are raw SinglePoint X/Y/Z (int16); output is the rotated world-space
 * offset to add to the vehicle's tile position. */
static void hwr_rotate_point(const HwrM33 *m,
    int px, int py, int pz, int *rx, int *ry, int *rz)
{
    int64_t ix = (int64_t)px * 2;
    int64_t iy = (int64_t)py * 2;
    int64_t iz = (int64_t)pz * 2;
    *rx = (int)((m->R[0][0]*ix + m->R[0][1]*iy + m->R[0][2]*iz) >> 15);
    *ry = (int)((m->R[1][0]*ix + m->R[1][1]*iy + m->R[1][2]*iz) >> 15);
    *rz = (int)((m->R[2][0]*ix + m->R[2][1]*iy + m->R[2][2]*iz) >> 15);
}

/* Reduce a vehicle's cornering lean. The matrix columns are the object's basis
 * vectors in world space (col0=right, col1=up, col2=forward), magnitude 16384
 * for unit. We blend the up axis a fraction of the way back toward world-up
 * (factor 1.0 = no change/full lean, 0.0 = upright), re-orthonormalise (keeping
 * the heading), and write the basis back. factor 0.5 = half the tilt. */
static void hwr_reduce_tilt(const HwrM33 *in, HwrM33 *out, float factor)
{
    double rx,ry,rz, ux,uy,uz, fxx,fxy,fxz, l, d, ty;
    /* Decode columns. */
    rx=in->R[0][0]; ry=in->R[1][0]; rz=in->R[2][0];
    ux=in->R[0][1]; uy=in->R[1][1]; uz=in->R[2][1];
    fxx=in->R[0][2]; fxy=in->R[1][2]; fxz=in->R[2][2];
    l = sqrt(ux*ux+uy*uy+uz*uz);
    if (l < 1e-6) { *out = *in; return; }
    ux/=l; uy/=l; uz/=l;
    /* Target up = world up, sign matching current up so we don't flip. */
    ty = (uy < 0.0) ? -1.0 : 1.0;
    /* new_up = lerp(target, up, factor): factor 0 -> upright, 1 -> unchanged. */
    ux = ux*factor;
    uy = uy*factor + ty*(1.0-factor);
    uz = uz*factor;
    l = sqrt(ux*ux+uy*uy+uz*uz);
    if (l < 1e-6) { *out = *in; return; }
    ux/=l; uy/=l; uz/=l;
    /* Gram-Schmidt forward and right against new up (keeps heading + handedness). */
    l = sqrt(fxx*fxx+fxy*fxy+fxz*fxz); if (l>1e-6){fxx/=l;fxy/=l;fxz/=l;}
    d = fxx*ux+fxy*uy+fxz*uz; fxx-=d*ux; fxy-=d*uy; fxz-=d*uz;
    l = sqrt(fxx*fxx+fxy*fxy+fxz*fxz); if (l<1e-6){*out=*in;return;} fxx/=l;fxy/=l;fxz/=l;
    l = sqrt(rx*rx+ry*ry+rz*rz); if (l>1e-6){rx/=l;ry/=l;rz/=l;}
    d = rx*ux+ry*uy+rz*uz; rx-=d*ux; ry-=d*uy; rz-=d*uz;
    d = rx*fxx+ry*fxy+rz*fxz; rx-=d*fxx; ry-=d*fxy; rz-=d*fxz;
    l = sqrt(rx*rx+ry*ry+rz*rz); if (l<1e-6){*out=*in;return;} rx/=l;ry/=l;rz/=l;
    /* Write basis back at unit magnitude 16384. */
    out->R[0][0]=(int32_t)(rx*16384.0); out->R[1][0]=(int32_t)(ry*16384.0); out->R[2][0]=(int32_t)(rz*16384.0);
    out->R[0][1]=(int32_t)(ux*16384.0); out->R[1][1]=(int32_t)(uy*16384.0); out->R[2][1]=(int32_t)(uz*16384.0);
    out->R[0][2]=(int32_t)(fxx*16384.0); out->R[1][2]=(int32_t)(fxy*16384.0); out->R[2][2]=(int32_t)(fxz*16384.0);
}

/* Build the world position of a static building object point. Buildings are
 * axis-aligned, so MapX/MapZ/OffsetY (cached at load) suffice directly.
 * For TT_BUILDING, thing_position_uses_y_mul_8 is false, so the render path uses
 * cor_dy = thing.Y>>8 = OffsetY directly (already in the floor's 8*Alt scale -
 * NO extra 8x, or buildings fly). Using 8*tile_alt instead sank some buildings.
 * Vehicles use hwr_rotate_point() + live Thing position instead of these macros. */
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
    refl_vert_count = 0;
    refl_index_count = 0;
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
        int f;
        /* World-space origin and optional rotation matrix for this object.
         * Only TT_VEHICLE objects use the dynamic Thing position + rotation
         * matrix (captured at gate time). Buildings, gates and other static
         * objects keep the cached MapX/OffsetY/MapZ path with no rotation. */
        int obj_tx, obj_ty, obj_tz;
        const HwrM33 *obj_mat = NULL;
        HwrM33 obj_mat_lvl;        /* tilt-reduced copy for vehicles */
        int snapped    = (obj_snap_valid && o < obj_snap_count);
        int is_dynamic = (snapped && obj_snap[o].is_dynamic);

        if (is_dynamic) {
            /* Dynamic object (vehicle / turret / rotor): position + matrix
             * captured at floor-gate time (see obj_snap), so it is consistent
             * with the camera snapshot this present uses. Reading live things[]
             * here would draw it one sim-turn ahead of the camera -> swims
             * against the road. */
            int16_t matx_idx = obj_snap[o].matx;
            obj_tx = obj_snap[o].tx;
            obj_ty = obj_snap[o].ty;
            obj_tz = obj_snap[o].tz;
            /* Cull by captured tile position. */
            if ((obj_tx >> 8) < x0 || (obj_tx >> 8) > x1 ||
                (obj_tz >> 8) < z0 || (obj_tz >> 8) > z1)
                continue;
            if (matx_idx > 0 && matx_idx < (int16_t)next_local_mat) {
                obj_mat = &snap_local_mats[matx_idx];
                /* Halve the cornering lean for actual vehicles (not turrets/
                 * rotors, which don't bank). */
                if (obj_snap[o].is_vehicle) {
                    hwr_reduce_tilt(obj_mat, &obj_mat_lvl, 0.5f);
                    obj_mat = &obj_mat_lvl;
                }
            }
        } else {
            /* Static building: cached world-unit position, no rotation. */
            int mtx = (uint16_t)obj->MapX >> 8;
            int mtz = (uint16_t)obj->MapZ >> 8;
            if (mtx < x0 || mtx > x1 || mtz < z0 || mtz > z1)
                continue;
            obj_tx = (int)(uint16_t)obj->MapX;
            obj_ty = (int)obj->OffsetY;
            obj_tz = (int)(uint16_t)obj->MapZ;
        }

        /* One object can be referenced by several map columns; emit once. */
        if (face_obj_seen[o >> 3] & (1 << (o & 7)))
            continue;
        face_obj_seen[o >> 3] |= (uint8_t)(1 << (o & 7));

        /* --- Quads (face4) --- */
        for (f = 0; f < obj->NumbFaces4; f++) {
            struct HwrObjFace4 *fc = &game_object_faces4[obj->StartFace4 + f];
            /* Reflective ("chameleon") paint faces (FGFlg_Unkn80) are diverted to
             * the reflective batch and drawn by the chameleon pass (view-angle hue
             * shift + sheen) instead of as plain textured geometry. SW is told to
             * skip them too (DrIT_ObFace*Refl suppression), so no double-draw.
             * If normals are unavailable, fall through to the textured path so the
             * face still renders (no holes). */
            if ((fc->GFlags & 0x80) && game_normals != NULL && next_normal > 0) {
                struct HwrSinglePoint *rp[4];
                int rwx[4], rwy[4], rwz[4], rk, rbase;
                float rsd, rn[4][3];
                int16_t rsh[4];
                if (refl_vert_count + 4 > HWR_REFL_MAX_VERTS ||
                    refl_index_count + 6 > HWR_REFL_MAX_INDEX)
                    continue;
                rsh[0]=fc->Shade0; rsh[1]=fc->Shade1; rsh[2]=fc->Shade2; rsh[3]=fc->Shade3;
                for (rk = 0; rk < 4; rk++) {
                    rp[rk] = &game_object_points[fc->PointNo[rk]];
                    if (obj_mat != NULL) {
                        int dx, dy, dz;
                        hwr_rotate_point(obj_mat, rp[rk]->X, rp[rk]->Y, rp[rk]->Z,
                            &dx, &dy, &dz);
                        rwx[rk] = obj_tx + dx; rwy[rk] = obj_ty + dy; rwz[rk] = obj_tz + dz;
                    } else {
                        rwx[rk] = obj_tx + (int)rp[rk]->X;
                        rwy[rk] = obj_ty + (int)rp[rk]->Y;
                        rwz[rk] = obj_tz + (int)rp[rk]->Z;
                    }
                    if (rsh[rk] >= 0 && rsh[rk] < (int16_t)next_normal) {
                        struct HwrNormal *nn = &game_normals[rsh[rk]];
                        hwr_world_normal(obj_mat, nn->NX, nn->NY, nn->NZ, rn[rk]);
                    } else {
                        rn[rk][0]=0.0f; rn[rk][1]=1.0f; rn[rk][2]=0.0f;
                    }
                }
                rbase = refl_vert_count;
                for (rk = 0; rk < 4; rk++) {
                    rsd = face_scrd((float)rwx[rk], (float)rwy[rk], (float)rwz[rk]);
                    refl_emit_vert(rwx[rk], rwy[rk], rwz[rk], rn[rk],
                        (float)fc->ExCol, rsd);
                }
                /* Same diagonal as the textured quad: (0,2,1)+(3,1,2). */
                refl_index[refl_index_count++] = rbase + 0;
                refl_index[refl_index_count++] = rbase + 2;
                refl_index[refl_index_count++] = rbase + 1;
                refl_index[refl_index_count++] = rbase + 3;
                refl_index[refl_index_count++] = rbase + 1;
                refl_index[refl_index_count++] = rbase + 2;
                continue;
            }
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
                if (obj_mat != NULL) {
                    int dx, dy, dz;
                    hwr_rotate_point(obj_mat, p[k]->X, p[k]->Y, p[k]->Z,
                        &dx, &dy, &dz);
                    wx[k] = obj_tx + dx;
                    wy[k] = obj_ty + dy;
                    wz[k] = obj_tz + dz;
                } else {
                    wx[k] = obj_tx + (int)p[k]->X;
                    wy[k] = obj_ty + (int)p[k]->Y;
                    wz[k] = obj_tz + (int)p[k]->Z;
                }
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
            /* Reflective ("chameleon") paint faces -> reflective batch (see face4). */
            if ((fc->GFlags & 0x80) && game_normals != NULL && next_normal > 0) {
                struct HwrSinglePoint *rp[3];
                int rwx[3], rwy[3], rwz[3], rk, rbase;
                float rsd, rn[3][3];
                int16_t rsh[3];
                if (refl_vert_count + 3 > HWR_REFL_MAX_VERTS ||
                    refl_index_count + 3 > HWR_REFL_MAX_INDEX)
                    continue;
                rsh[0]=fc->Shade0; rsh[1]=fc->Shade1; rsh[2]=fc->Shade2;
                for (rk = 0; rk < 3; rk++) {
                    rp[rk] = &game_object_points[fc->PointNo[rk]];
                    if (obj_mat != NULL) {
                        int dx, dy, dz;
                        hwr_rotate_point(obj_mat, rp[rk]->X, rp[rk]->Y, rp[rk]->Z,
                            &dx, &dy, &dz);
                        rwx[rk] = obj_tx + dx; rwy[rk] = obj_ty + dy; rwz[rk] = obj_tz + dz;
                    } else {
                        rwx[rk] = obj_tx + (int)rp[rk]->X;
                        rwy[rk] = obj_ty + (int)rp[rk]->Y;
                        rwz[rk] = obj_tz + (int)rp[rk]->Z;
                    }
                    if (rsh[rk] >= 0 && rsh[rk] < (int16_t)next_normal) {
                        struct HwrNormal *nn = &game_normals[rsh[rk]];
                        hwr_world_normal(obj_mat, nn->NX, nn->NY, nn->NZ, rn[rk]);
                    } else {
                        rn[rk][0]=0.0f; rn[rk][1]=1.0f; rn[rk][2]=0.0f;
                    }
                }
                rbase = refl_vert_count;
                for (rk = 0; rk < 3; rk++) {
                    rsd = face_scrd((float)rwx[rk], (float)rwy[rk], (float)rwz[rk]);
                    refl_emit_vert(rwx[rk], rwy[rk], rwz[rk], rn[rk],
                        (float)fc->ExCol, rsd);
                }
                refl_index[refl_index_count++] = rbase + 0;
                refl_index[refl_index_count++] = rbase + 1;
                refl_index[refl_index_count++] = rbase + 2;
                continue;
            }
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
                if (obj_mat != NULL) {
                    int dx, dy, dz;
                    hwr_rotate_point(obj_mat, p[k]->X, p[k]->Y, p[k]->Z,
                        &dx, &dy, &dz);
                    wx[k] = obj_tx + dx;
                    wy[k] = obj_ty + dy;
                    wz[k] = obj_tz + dz;
                } else {
                    wx[k] = obj_tx + (int)p[k]->X;
                    wy[k] = obj_ty + (int)p[k]->Y;
                    wz[k] = obj_tz + (int)p[k]->Z;
                }
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

/* Hand back the reflective (chameleon paint) batch collected by sw_get_faces.
 * Must be called after sw_get_faces() each frame (the floor pass calls faces
 * before this). */
static int sw_get_reflect_faces(void *ctx, HwrReflectBatch *out)
{
    (void)ctx;
    if (out == NULL)
        return 0;
    out->verts = refl_verts;
    out->vert_count = refl_vert_count;
    out->indices = refl_index;
    out->index_count = refl_index_count;
    return refl_index_count;
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

    /* Reserve light slots for nearby vehicle headlights/tails so the map lights
     * (a city is densely lit) don't fill all 64 and starve them. Count vehicles
     * within range of the camera; each contributes 2 lights.
     *
     * The cull disc is centred not on the camera look-point but SHIFTED forward
     * along the view direction (the camera is always angled), so its near edge
     * sits further from the viewer and its outer edge reaches well into the
     * scene. Used by both the reserve count and the placement loop below. */
    long veh_cull_cx, veh_cull_cz;
    const long VEH_CULL_RANGE2 = 5120L * 5120L;   /* ~20 tiles outer radius */
    {
        float vfx = (float)snap.D10, vfz = (float)snap.D14;   /* into-screen XZ dir */
        float vfl = (float)sqrt(vfx*vfx + vfz*vfz);
        const float VEH_CULL_SHIFT = 1536.0f;                 /* push ~6 tiles into scene */
        if (vfl > 1e-3f) { vfx /= vfl; vfz /= vfl; }
        veh_cull_cx = snap.xc + (long)(vfx * VEH_CULL_SHIFT);
        veh_cull_cz = snap.zc + (long)(vfz * VEH_CULL_SHIFT);
    }
    int veh_reserve = 0;
    {
        if (obj_snap_valid) {
            unsigned o; int nv = 0;
            for (o = 1; o < obj_snap_count; o++) {
                long ddx, ddz;
                if (!obj_snap[o].is_vehicle) continue;
                ddx = (long)obj_snap[o].tx - veh_cull_cx;
                ddz = (long)obj_snap[o].tz - veh_cull_cz;
                if (ddx*ddx + ddz*ddz <= VEH_CULL_RANGE2) nv++;
            }
            veh_reserve = nv * 4;                      /* 2 headlights + 2 tails per car */
            if (veh_reserve > 32) veh_reserve = 32;    /* cap: ~8 nearest cars */
        }
    }

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
    static short cached_level = -1;
    static unsigned short cached_map = 0xFFFF;
    {
        int nfl = (int)next_full_light;
        if (nfl > HWR_THING_CACHE_LEN) nfl = HWR_THING_CACHE_LEN;
        /* The cache is keyed by light array index, which is NOT stable within a
         * frame's worth of work: process_temp_light() appends randomized flicker
         * lights every tick, growing/shrinking next_full_light and mutating the
         * NextFull chains, so the SimpleThing→light traversal resolves a given
         * static lamp's index only INTERMITTENTLY.  If we cleared and rebuilt the
         * cache every frame, a streetlamp would flip between its owner-derived
         * category (when the traversal happened to reach it) and the intensity
         * fallback (when it didn't) — visible as cyclic brightness flashing.
         *
         * Fix: PERSIST the cache across frames.  Clear it only on level change;
         * otherwise just overwrite the entries the traversal positively resolves
         * this frame and leave previously-resolved entries intact.  Once a lamp
         * is classified by ownership it stays classified, killing the flicker. */
        if (cached_level != current_level || cached_map != current_map) {
            memset(cached_type, 0, sizeof(cached_type));
            memset(cached_sub, 0, sizeof(cached_sub));
            cached_level = current_level;
            cached_map   = current_map;
        }
        /* Only LightHead ownership determines a light's type for category
         * purposes.  We DO NOT traverse faces here: a face references a
         * light for illumination, not ownership.  Unconnected lights (no
         * SimpleThing LightHead chain) fall back to intensity-based
         * category in the output loop below.
         * The SimpleThing that OWNS a FullLight (via LightHead→NextFull chain)
         * determines its type.  STHINGS_LIMIT = 1500 — do NOT go past this or
         * garbage data can overwrite valid cached_type entries for real lights. */
        {
            extern char *things;
            int max_si = 1500;
            for (int si = 1; si < max_si; si++) {
                struct HwrSimpleThingMini *st = (struct HwrSimpleThingMini *)((char *)things - si * 60);
                if (st->Type == 0 || st->U_LightHead == 0) continue;
                int new_has_cat = (hwr_thing_category_get(st->Type, st->SubType) >= 1);
                int fidx = st->U_LightHead;
                int visited = 0;
                while (fidx > 0 && fidx < (uint16_t)nfl) {
                    if (st->Type > 0) {
                        /* Multiple SimpleThings can chain to the same light index
                         * (a real lamp Thing AND e.g. a passing unit whose
                         * U_LightHead garbage-chains into it).  If we let the last
                         * writer win every frame, the resolved category oscillates
                         * → visible cyclic flashing.  Resolution priority, applied
                         * stably so a given light locks to one owner:
                         *   1. an owner whose (Type,SubType) has a CONFIGURED
                         *      category ([thing_categories]) always wins and sticks;
                         *   2. otherwise first writer wins (never overwritten). */
                        int cur = cached_type[fidx];
                        int cur_has_cat = cur
                            ? (hwr_thing_category_get(cur, cached_sub[fidx]) >= 1)
                            : 0;
                        if (cur == 0 || (new_has_cat && !cur_has_cat)) {
                            cached_type[fidx] = st->Type;
                            cached_sub[fidx]  = st->SubType;
                        }
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
        /* Use TrueIntensity (stable, pre-animation) for selection so lights
         * don't pop in/out of the 64-slot uniform when ASM_unkn_update_lights
         * oscillates their animated Intensity.  The output loop below sets
         * radius=0 when Intensity==0 so the shader silently skips dimmed lights. */
        if (fl->TrueIntensity == 0)
            continue;
        if (fl->TrueIntensity < 0 && sstr <= 0.0f)
            continue;
        dx = (int)fl->X - cx;
        dz = (int)fl->Z - cz;
        d2 = dx*dx + dz*dz;

        if (nnearest < max - veh_reserve) {
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
        out[i].fdx = 0.0f; out[i].fdz = 0.0f;   /* map lights are round */
        if (fl->TrueIntensity < 0) {
            /* Anti-light: shader reads .r as the darkening amount and the
             * negative radius as the flag. Use TrueIntensity (stable,
             * pre-animation value) to avoid flickering from ASM_unkn_update_lights.
             * Check TrueIntensity (not Intensity) so animated oscillation can't
             * flip a positive light into anti-light mode mid-cycle. */
            int ai = -(int)fl->TrueIntensity;
            out[i].r = out[i].g = out[i].b = sstr * ((float)ai / 64.0f);
            /* Inverse-square attenuation constant: TrueIntensity * 34019
             * matching the SW super-quick-light formula. rmul/21 normalises so
             * ini radius=21 gives exact SW behaviour. */
            out[i].radius = -((float)ai * 34019.0f * (rmul / 21.0f) * col.intensity_scale);
            out[i].max_dist2 = 4194304.0f * (rmul / 21.0f);
        } else {
            /* Category from LightHead owner's Thing (Type,SubType).
             * When ownership cannot be determined (no SimpleThing traces to
             * this light), fall back to intensity-based classification matching
             * the SW engine: filler/intensity ≤ 50, building ≤ 200, street > 200. */
            HwrLightDefaults ld = hwr_lights_defaults();
            int lidx = nearest[i].idx;
            int cat = 0; /* 1=filler, 2=building, 3=street */
            /* Intensity-based classification is the DETERMINISTIC default,
             * matching the SW engine. We must NOT depend on whether the
             * per-frame SimpleThing→LightHead traversal happened to tag this
             * light index this frame: process_temp_light() appends randomized
             * flicker lights every tick, so next_full_light and the tail of
             * game_full_lights shuffle, making cached_type[lidx] unstable.
             * Keying brightness off it caused lights (e.g. streetlamps) to
             * cycle on/off as the traversal hit or missed their index. */
            {
                int intens = (int)fl->TrueIntensity;
                if (intens <= ld.filler_maxint)        cat = 1;
                else if (intens <= ld.building_maxint)  cat = 2;
                else                                    cat = 3;
            }
            /* An explicit (Type,SubType) category override REPLACES the
             * intensity default, but only when one is actually set (oc>=1).
             * oc==0 means "no override" and must leave the intensity result
             * intact — never force the light to filler/0.0. */
            {
                int tt = (cached_valid && lidx > 0 && lidx < HWR_THING_CACHE_LEN)
                         ? cached_type[lidx] : 0;
                if (tt > 0) {
                    int oc = hwr_thing_category_get(tt, cached_sub[lidx]);
                    if (oc >= 1 && oc <= 3) cat = oc;
                }
            }
            float cat_bright = (cat == 1) ? ld.filler_brightness
                             : (cat == 2) ? ld.building_brightness
                                          : ld.street_brightness;
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
            /* Use TrueIntensity (stable, pre-animation) for the radius so the
             * light pool size doesn't oscillate with ASM_unkn_update_lights(). */
            out[i].radius = (float)fl->TrueIntensity * 34019.0f * radius_scale * col.intensity_scale;
            /* Per-light distance cull scales with the per-category radius.
             * 4194304 = (8 tiles * 256 PRC/tile)² at default radius_scale=1. */
            out[i].max_dist2 = 4194304.0f * radius_scale;
        }
    }

    /* --- Vehicle headlights + tail lights ---------------------------------
     * Each vehicle gets a white point light a short way IN FRONT (a round pool
     * on the road/buildings, exactly like the original) and a red point light
     * behind. These reuse the normal point-light path (radial falloff), so no
     * separate pass is needed. Positions are MAPCOORD, matching out[].x/z and
     * the floor/face world coords. Forward comes from the vehicle matrix. */
    hwr_sw_vehicle_lights = 0;
    if (obj_snap_valid) {
        int veh_light_start = nnearest;
        /* Tunables (MAPCOORD; 256 = one tile). */
        const float HL_FRONT_OFF = 420.0f;   /* lamp position ahead of centre (egg extends further forward) */
        const float HL_REAR_OFF  = 340.0f;   /* tail position behind centre */
        const float HL_SIDE      = 70.0f;    /* L/R lamp offset from the centreline */
        const int   HL_FWD_SIGN  = -1;        /* flip if pools land at wrong end */
        const float HL_REACH     = 900.0f;   /* headlight forward reach (egg length) */
        const float TL_POOL2     = 100.0f*100.0f*2.0f;  /* tail cull radius² */
        const float HL_BRIGHT    = 3.0f;
        const float TL_BRIGHT    = 2.4f;
        unsigned o;
        for (o = 1; o < obj_snap_count && nnearest + 4 <= max; o++) {
            const HwrM33 *m;
            float fx, fz, rx, rz, fl;
            float tx, ty, tz;
            long ddx, ddz;
            int s;
            if (!obj_snap[o].is_vehicle)
                continue;
            ddx = (long)obj_snap[o].tx - veh_cull_cx;
            ddz = (long)obj_snap[o].tz - veh_cull_cz;
            if (ddx*ddx + ddz*ddz > VEH_CULL_RANGE2)
                continue;   /* only cars within the (shifted) cull disc get lights */
            tx = (float)obj_snap[o].tx;
            ty = (float)obj_snap[o].ty;
            tz = (float)obj_snap[o].tz;
            /* Forward and right (XZ) from the vehicle matrix; fall back to axes. */
            m = (obj_snap[o].matx > 0 && obj_snap[o].matx < (int16_t)next_local_mat)
                ? &snap_local_mats[obj_snap[o].matx] : NULL;
            if (m != NULL) {
                int dx, dy, dz;
                hwr_rotate_point(m, 0, 0, 256, &dx, &dy, &dz);
                fx = (float)dx; fz = (float)dz;
                hwr_rotate_point(m, 256, 0, 0, &dx, &dy, &dz);
                rx = (float)dx; rz = (float)dz;
            } else {
                fx = 0.0f; fz = 256.0f; rx = 256.0f; rz = 0.0f;
            }
            fl = (float)sqrt(fx*fx + fz*fz);
            if (fl > 1e-3f) { fx /= fl; fz /= fl; }
            fl = (float)sqrt(rx*rx + rz*rz);
            if (fl > 1e-3f) { rx /= fl; rz /= fl; }
            fx *= (float)HL_FWD_SIGN; fz *= (float)HL_FWD_SIGN;

            for (s = -1; s <= 1; s += 2) {
                float ox = rx * (HL_SIDE * s), oz = rz * (HL_SIDE * s);
                /* Headlight (shaped/egg): warm-white, projecting forward. */
                out[nnearest].x = tx + fx*HL_FRONT_OFF + ox;
                out[nnearest].y = ty;
                out[nnearest].z = tz + fz*HL_FRONT_OFF + oz;
                out[nnearest].r = 1.00f * HL_BRIGHT;
                out[nnearest].g = 0.93f * HL_BRIGHT;
                out[nnearest].b = 0.78f * HL_BRIGHT;
                out[nnearest].radius = 1.0f;          /* >0 = positive light */
                out[nnearest].max_dist2 = HL_REACH * HL_REACH;
                out[nnearest].fdx = fx;               /* shaped: teardrop along forward */
                out[nnearest].fdz = fz;
                nnearest++;
            }
            for (s = -1; s <= 1; s += 2) {
                float ox = rx * (HL_SIDE * s), oz = rz * (HL_SIDE * s);
                /* Tail (round): red pool behind. */
                out[nnearest].x = tx - fx*HL_REAR_OFF + ox;
                out[nnearest].y = ty;
                out[nnearest].z = tz - fz*HL_REAR_OFF + oz;
                out[nnearest].r = 1.00f * TL_BRIGHT;
                out[nnearest].g = 0.05f * TL_BRIGHT;
                out[nnearest].b = 0.00f;
                out[nnearest].radius = 1.0f;
                out[nnearest].max_dist2 = TL_POOL2;
                out[nnearest].fdx = 0.0f; out[nnearest].fdz = 0.0f;
                nnearest++;
            }
        }
        hwr_sw_vehicle_lights = nnearest - veh_light_start;
    }

    return nnearest;
}

static int sw_get_sprites(void *ctx, HwrBillboard *out, int max)
{
    int n = hwr_collected_count;
    if (n > max) n = max;
    if (n > 0 && out != NULL)
        memcpy(out, hwr_collected_billboards, (size_t)n * sizeof(HwrBillboard));
    return n;
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
    sw_get_reflect_faces,
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
