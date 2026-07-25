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
#include "hwr_source_sw.h"
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
/* Current game tick, advanced once per sim step; drives the water wobble
 * animation phase identically to shpoint_compute_coord_y (lvdraw3d.c). Set
 * from gameturn every frame (game.c). */
extern uint32_t       render_anim_turn;
/* Sine-like wobble lookup tables (32 entries each), from enginpeff.c.
 * waft_table:  small flat per-frame offset (Flags&0x40, e.g. rope sway).
 * waft_table2: spatial water wave terms (Flags&0x10). */
extern const int16_t  waft_table[32];
extern const int16_t  waft_table2[32];
/* Render floor flags (enginprops.c); bit 0x02 = RendFlrF_WobblyTerrain: the SW
 * engine adds waft_table[render_anim_turn&0x1F]>>3 to every vertex Y (whole level
 * bobs). Mirrored in the HW camera below. */
extern uint32_t       render_floor_flags;
#define HWR_RENDFLR_WOBBLY 0x02
/* 32-bit bit-rotate helpers (bflibrary/bfendian.h), used by the water wobble
 * per-tile phase offset (dvfactor), exactly matching shpoint_compute_coord_y. */
extern uint32_t       bw_rotl32(uint32_t n, uint8_t c);
extern uint32_t       bw_rotr32(uint32_t n, uint8_t c);
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
/* SW's flat floor colour for cells with no ground surface (Texture==0). */
extern unsigned char       colour_grey2;
/* Sentinel page for the floor shader's LIT solid-palette-colour path (the
 * vUV.z 251.5..252.5 branch); distinct from HWR_SOLID_PAGE 253, which is
 * unlit and used by shrapnel. */
#define HWR_FLATCOL_PAGE 252

/* Explosion face fragments (== ex_faces[], swrendersoft/enginfexpl.h
 * struct ExplodeFace3, sizeof 46, packed): flying object debris plus the
 * recursively-subdividing floor-crater shards. The game animates them each
 * turn (position/rotation baked into the stored coords), so GL reads the array
 * live and re-projects. Active entries have Timer != 0. */
#pragma pack(push, 1)
struct HwrExplodeFace {
    uint16_t Texture;
    uint16_t Flags;
    uint8_t  Type;          /* 1/3/5 = textured tri, 2/4/6 = textured quad */
    uint8_t  Col;
    int16_t  X0, Y0, Z0, X1, Y1, Z1, X2, Y2, Z2, X3, Y3, Z3;
    int16_t  PointOffset, Timer, X, Y, Z;
    int8_t   DX, DY, DZ, AngleDX, AngleDY, AngleDZ;
};
#pragma pack(pop)
#define HWR_EXPLODE_FACES_COUNT 1024
extern struct HwrExplodeFace ex_faces[HWR_EXPLODE_FACES_COUNT];
extern uint32_t              dont_bother_with_explode_faces;
/* Snapshots of ex_faces[] for interpolating flying debris / collapse shards to
 * the display rate: _cap = last captured turn, _prev = the turn before it. */
static struct HwrExplodeFace ex_faces_cap[HWR_EXPLODE_FACES_COUNT];
static struct HwrExplodeFace ex_faces_prev[HWR_EXPLODE_FACES_COUNT];
static int ex_faces_cap_valid = 0;
static int ex_faces_prev_valid = 0;

/* Explosion shrapnel chips (== shrapnel[], swrendersoft/enginshrapn.h struct
 * Shrapnel, sizeof 24, packed): small spinning flat-coloured triangles flung by
 * explosions. Enumerated per frame from the DrIT_SharpnlPoly draw items; each
 * carries a world position (x/z in >>8 units, y in >>5) plus yaw/pitch. The SW
 * path draws them as flat colour_lookup[8]/[9] tris with no depth test; the GL
 * path re-derives world-space corners and draws them as real depth-tested
 * geometry, interpolated to the display rate against the snapshots below. */
#pragma pack(push, 1)
struct HwrShrapnel {
    int32_t  x, y, z;
    int8_t   vx, vy, vz;
    uint8_t  yaw, pitch;
    int8_t   vyaw, vpitch;
    uint8_t  type;
    uint16_t PointOffset;
    uint16_t child;
};
#pragma pack(pop)
#define HWR_SHRAPNEL_COUNT 512
extern struct HwrShrapnel shrapnel[HWR_SHRAPNEL_COUNT];
extern long    lbSinTable[];     /* bfmath.h: fixed-point sine LUT */
extern uint8_t colour_lookup[];  /* engincolour.h: TbPixel colour_lookup[] */
extern uint32_t bw_rotl32(uint32_t n, uint8_t c);  /* bfendian.h */
#define HWR_LBFPMATH_PI 1024     /* bfmath.h LbFPMath_PI */
/* Snapshots of shrapnel[] for display-rate interpolation (same scheme as
 * ex_faces): _cap = last captured turn, _prev = the turn before it. */
static struct HwrShrapnel shrapnel_cap[HWR_SHRAPNEL_COUNT];
static struct HwrShrapnel shrapnel_prev[HWR_SHRAPNEL_COUNT];
static int shrapnel_cap_valid = 0;
static int shrapnel_prev_valid = 0;
/* Active shrapnel indices captured from the draw list at collect time. The draw
 * list is rebuilt/reset several times before and during the GL present, so it is
 * NOT safe to walk from emit_shrapnels() (that dropped a timing-dependent subset
 * of chips); we snapshot the active set once, alongside the overlay collector. */
static uint16_t hwr_shrapnel_active[HWR_SHRAPNEL_COUNT];
static int      hwr_shrapnel_active_count = 0;

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
 * Union U starts at byte 76; MatrixIndex (int16) is at union+8 = byte 84.
 * PassengerHead (int16) is at union+18 = byte 94 (follows MaxSpeed at pos=92). */
#define HWR_THING_SIZEOF    168
#define HWR_THING_MATX       84
#define HWR_THING_PASSHEAD   94

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
static struct HwrCamSnap {
    int32_t xc, yc, zc;
    int32_t D10, D14, D18, D1C, D3C, D40;
    int32_t scale;
    int     ra, rb;
    int     persp;
    int     valid;
} snap, snap_prev;   /* snap = latest turn, snap_prev = the turn before it */

/* Renderer interpolation factor [0..1] from game_speed.c: fraction of the way
 * from snap_prev to snap for the frame being presented. */
extern float g_interp_alpha;

/* Floor/face tile culling window: sw_get_floor/sw_get_faces/emit_floor_damage_
 * decals build their tile batch once per capture (16Hz), sized ra+2/rb+2 tiles
 * around the camera. But the RENDER camera (sw_get_camera) is interpolated
 * smoothly between snap_prev and snap across the several present frames before
 * the next capture. A window sized only around the latest snap position covers
 * the interpolation's END point but not points nearer snap_prev — for the early
 * frames of a turn (small alpha, camera still close to the old position), a
 * fast scroll can reveal ground/faces just outside that window before the next
 * capture regenerates it: a trailing-edge flicker whose visibility scales with
 * per-turn scroll speed (worse in whichever direction happens to move fastest).
 * Fix: size the window to cover BOTH snapshots' tile-ranges (their union), which
 * covers every point on the straight-line interpolation between them too. */
static void hwr_floor_window_bounds(int *out_x0, int *out_x1, int *out_z0, int *out_z1)
{
    int cx = snap.xc >> 8, cz = snap.zc >> 8;
    int ra = snap.ra ? snap.ra + 2 : 24;
    int rb = snap.rb ? snap.rb + 2 : 24;
    int x0 = cx - ra, x1 = cx + ra;
    int z0 = cz - rb, z1 = cz + rb;
    if (snap_prev.valid) {
        int pcx = snap_prev.xc >> 8, pcz = snap_prev.zc >> 8;
        int pra = snap_prev.ra ? snap_prev.ra + 2 : 24;
        int prb = snap_prev.rb ? snap_prev.rb + 2 : 24;
        int px0 = pcx - pra, px1 = pcx + pra;
        int pz0 = pcz - prb, pz1 = pcz + prb;
        if (px0 < x0) x0 = px0;
        if (px1 > x1) x1 = px1;
        if (pz0 < z0) z0 = pz0;
        if (pz1 > z1) z1 = pz1;
    }
    *out_x0 = x0; *out_x1 = x1; *out_z0 = z0; *out_z1 = z1;
}

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
struct HwrObjSnap {
    int32_t  tx, ty, tz;   /* world position (X>>8, Y>>5 or >>8, Z>>8) at capture */
    int16_t  matx;         /* MatrixIndex, or <=0 for none                        */
    uint8_t  is_dynamic;   /* 1 = position from Thing + matrix (vehicle/turret/rotor) */
    uint8_t  is_vehicle;   /* 1 = TT_VEHICLE — also skip SW-drawn reflective faces    */
    uint8_t  has_passengers; /* 1 = PassengerHead != 0 (vehicle is occupied)         */
};
static struct HwrObjSnap obj_snap[HWR_MAX_SNAP_OBJS];
/* Previous turn's object snapshot, for interpolating dynamic-object (vehicle/
 * turret/rotor) positions to the display rate. Indexed by object id like above. */
static struct HwrObjSnap obj_snap_prev[HWR_MAX_SNAP_OBJS];
static unsigned obj_snap_count = 0;   /* objects captured this frame */
static unsigned obj_snap_prev_count = 0;
static int      obj_snap_valid = 0;
static int      obj_snap_prev_valid = 0;
static HwrM33   snap_local_mats[100]; /* local_mats copy at capture time */
static HwrM33   snap_local_mats_prev[100]; /* previous turn, for rotation interp */

/* Debug: detect buildings whose Thing Y changes per turn (hovering/animating or
 * collapsing) and report the most recent one, so we can identify the subtype
 * that needs the dynamic (Thing-tracked) render path. Shown by draw_fps_counter. */
int hwr_dbg_hover_sub = -1;
int hwr_dbg_hover_state = -1;
int hwr_dbg_hover_y = 0;
static int32_t hover_lasty[HWR_MAX_SNAP_OBJS];
static int hover_lasty_valid = 0;

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
#define HWR_DI_SpObFace4   0x0C
#define HWR_DI_Unkn11      0x0B
#define HWR_DI_SharpnlPoly 0x14
#define HWR_DI_SFrmPhwoar  0x15
#define HWR_DI_SFireFlame  0x19

/* Special object face4 + screen point pool (for flat-tinted 2D overlay effects:
 * shield-hit spheres, blast rings, lightning). Layouts mirror enginsngobjs.h /
 * engindrwlstx.h. These special faces store already-projected screen points and
 * a palette colour (ExCol); render mode (Flags) 15 = 50% tint blend. */
#pragma pack(push, 1)
struct HwrSpObFace4 {       /* == SingleObjectFace4, sizeof 40 */
    int16_t  PointNo[4];
    uint16_t Texture;
    uint8_t  GFlags, Flags;
    uint16_t ExCol;
    uint16_t Object;
    int16_t  Shade0, Shade1, Shade2, Shade3;
    int16_t  Light0, Light1, Light2, Light3;
    uint16_t FaceNormal, WalkHeader, UnknTringl1, UnknTringl2;
};
struct HwrSpecialPoint { int16_t X, Y, Z, PadTo8; };
struct HwrSortLine {        /* == struct SortLine */
    int16_t X1, Y1, X2, Y2;
    uint8_t Col, Shade, Flags;
};
#pragma pack(pop)
extern struct HwrSpObFace4    *game_special_obj_faces4;
extern struct HwrSpecialPoint *game_screen_point_pool;
extern struct HwrSortLine     *game_sort_lines;

#define HWR_SMTT_DROPPED_ITEM 0x19   /* SimpleThing Type for a dropped (collectable) item */

/* Effect arrays from enginshrapn.h (fire flames / phwoar smoke+explosion clouds).
 * Layouts mirror that header exactly (packed); the symbols resolve at the final
 * executable link. The draw list carries one DrIT_SFireFlame / DrIT_SFrmPhwoar
 * item per visible effect with Offset = index into these arrays. */
#pragma pack(push, 1)
struct HwrFireFlame {       /* == struct FireFlame, sizeof 20 */
    uint8_t  type, count;
    int8_t   fvel, fcount, big, dbig, ddbig;
    uint8_t  life;
    uint16_t frame;
    int16_t  x, y, z;
    uint16_t PointOffset;
    uint16_t next;
};
struct HwrPhwoar {          /* == struct Phwoar */
    int32_t  x, y, z;
    int8_t   vx, vy, vz;
    uint8_t  type, rabbit, gestation;
    int8_t   die;
    uint8_t  vf;
    uint16_t f;
    uint16_t PointOffset;
    uint16_t child;
    uint8_t  fc, shit;
};
#pragma pack(pop)
extern struct HwrFireFlame FIRE_flame[512];
extern struct HwrPhwoar    phwoar[1024];

/* Target boxes recorded by the HUD (hud_target.c): screen centre + half-extent +
 * bracket-set variant (0 person, 1 vehicle). Drawn as a translucent box tile. */
struct HwrTgtBoxRec { int16_t cx, cy, half; uint8_t variant; };
extern struct HwrTgtBoxRec hwr_tgtbox_list[64];
extern int hwr_tgtbox_count;
/* Pause popup box fills (fepause.c): each entry is a screen rect + palette
 * colour to draw as a semi-transparent GL overlay quad (purple tint over 3D). */
struct HwrPauseBoxRec { short x0, y0, x1, y1; uint8_t colr; };
extern struct HwrPauseBoxRec hwr_pause_box_list[8];
extern int hwr_pause_box_count;
/* HUD sprite bank (pop1_sprites): same TbSprite layout as m_sprites. */
extern struct TbSprite *pop1_sprites;

/* Per-effect skip masks (defined in engindrwlstx.c, libswrender). A set bit
 * means the FX3D renderer collected that effect as a billboard, so the SW
 * drawlist executor must skip its software draw. 512 flames -> 64 bytes,
 * 1024 phwoar -> 128 bytes. */
extern unsigned char hwr_fire_skip_mask[64];
extern unsigned char hwr_phwoar_skip_mask[128];

/* Light-glare (headlight / lamp) world positions enlisted by build_glare this
 * frame (see engindrwlstx.h struct HwrGlare). Drawn as additive glow billboards;
 * their SW screen-space faces are suppressed under FX3D. Layout must match. */
struct HwrGlareRec { int x, y, z, r, siren; };
#define HWR_GLARE_MAX 512
extern struct HwrGlareRec hwr_glare_list[HWR_GLARE_MAX];
extern int hwr_glare_count;

/* World-anchored 2D HUD overlays (numbers over heads, short tags, vehicle health
 * bars) captured during the SW build by the enlist_draw_* routines. Layout must
 * match struct HwrOverlayReq in engindrwlstx.h exactly. Re-projected here every
 * present frame with the interpolated camera; bars additionally interpolate their
 * world anchor between the previous and current sim turn (matched by `ident`, the
 * source Thing pointer) so they track a moving vehicle smoothly. */
enum { HwrOvReq_Number = 0, HwrOvReq_Text = 1, HwrOvReq_Bar = 2, HwrOvReq_Frame = 3 };
#define HWR_OVREQ_MAX 512
struct HwrOverlayReq {
    unsigned char kind;
    unsigned char col, col2;
    short scr_dx, scr_dy;
    int   ax, dyc, az;
    intptr_t ident;
    int   ival, ival2;
    char  text[8];
};
extern struct HwrOverlayReq hwr_overlay_req[HWR_OVREQ_MAX];
extern int hwr_overlay_req_count;
/* Previous sim-turn snapshot of the overlay list, for anchor interpolation. The
 * "current" list is hwr_overlay_req itself, read live (fully populated — including
 * the agent numbers added late in draw_hud — by present time). The snapshot is
 * taken in the always-compiled swrender lib (engindrwlstx.c hwr_overlay_snapshot_
 * prev, called from game.c just before the per-turn reset) so it holds the
 * previous turn's COMPLETE list; we only read it here. */
extern struct HwrOverlayReq hwr_ovreq_prev[HWR_OVREQ_MAX];
extern int hwr_ovreq_prev_count;

/* Software proportional font (bflibrary rom.c): `font` holds 8 bytes per glyph
 * (6 bitmap rows + advance width at offset 6); prop_text rasterises a string
 * into a palette-index buffer. Used to bake per-glyph atlas tiles for the GL
 * HUD-number/text overlay. */
extern const unsigned char font[];
extern void prop_text(const char *text, unsigned char *out, long scanline, unsigned char colour);

/* Weapon beam segments (electric zap / laser) captured during the SW build.
 * Layout must match struct HwrBeamSeg in engindrwlstx.h. world==1: a/b are
 * (mapX, transform Y arg, mapZ) absolute world coords re-projected each frame;
 * world==0: a/b are (screen x, screen y, scrd depth) fixed at the 16Hz rate. */
#define HWR_BEAM_MAX 4096
struct HwrBeamSeg {
    unsigned char world;
    unsigned char col;
    short thick;
    int a[3], b[3];
};
extern struct HwrBeamSeg hwr_beam_list[HWR_BEAM_MAX];
extern int hwr_beam_count;

/* The SW sort-sprite, draw-list and frame/sprite arrays. */
extern struct SortSprite *game_sort_sprites;
extern unsigned short     next_sort_sprite;
extern struct DrawItem   *game_draw_list;
extern unsigned short     next_draw_item;

extern struct Frame      *frame, *frame_end;
extern struct Element    *melement_ani, *mele_ani_end;
extern struct TbSprite   *m_sprites, *m_sprites_end;

/* Pre-collected billboard storage (filled by hwr_sw_collect_sprites,
 * consumed by sw_get_sprites). Must match SPR_MAX_BILLBOARDS in hwr_sprite.c;
 * raised from 2048 so heavy combat (agents + a big explosion's smoke puffs +
 * fire + glares) no longer overflows and drops the tail of the smoke to the
 * flickering software fallback. */
#define HWR_MAX_COLLECTED 4096
static HwrBillboard hwr_collected_billboards[HWR_MAX_COLLECTED];
static int          hwr_collected_count = 0;
static int          hwr_xbr_count = 0;

/* Sprite position interpolation (Thing-anchored billboards only). Collection
 * happens once per 16Hz turn (hwrender_floor_gate -> hwr_sw_collect_sprites),
 * but get_sprites is pulled every present frame at up to 60fps+, so without
 * this the sprite (person/vehicle-thing/dropped-item) billboards visibly
 * shimmer against the now-smoothly-interpolated floor/camera as they snap in
 * 16Hz steps while scrolling. hwr_collected_key[i] is the persistent identity
 * (the Thing pointer) for entry i, or NULL if this billboard shouldn't be
 * interpolated (ephemeral effects: fire/phwoar/glares). hwr_collected_prev_idx[i]
 * is resolved once per capture (matching this turn's keys against last turn's)
 * so the per-frame render path is a cheap array lookup, not a search. */
static const void  *hwr_collected_key[HWR_MAX_COLLECTED];
static HwrBillboard hwr_collected_billboards_prev[HWR_MAX_COLLECTED];
static const void  *hwr_collected_key_prev[HWR_MAX_COLLECTED];
static int          hwr_collected_count_prev = 0;
static int          hwr_collected_prev_idx[HWR_MAX_COLLECTED];

/* Small open-addressing hash of the previous turn's keys -> prev index, used
 * to resolve hwr_collected_prev_idx in one pass. Sized well above
 * HWR_MAX_COLLECTED to keep load factor low. */
#define HWR_SPRITE_MATCH_HASH 8192
static const void *spr_match_key[HWR_SPRITE_MATCH_HASH];
static int         spr_match_idx[HWR_SPRITE_MATCH_HASH];

static uint32_t hwr_ptr_hash(const void *p)
{
    /* This project builds -m32 (uintptr_t is 32-bit); do the mix as 32-bit
     * only (a >>32 shift on a 32-bit value is undefined behaviour). */
    uint32_t h = (uint32_t)(uintptr_t)p;
    h ^= h >> 16; h *= 0x85ebca6bu;
    h ^= h >> 13; h *= 0xc2b2ae35u;
    h ^= h >> 16;
    return h;
}

/* Snapshot the just-finished turn's collected billboards as "previous" (for
 * interpolation), then build the key->index match table for it. Must run
 * BEFORE hwr_collected_count/keys are overwritten for the new turn. */
static void hwr_sw_snapshot_prev_sprites(void)
{
    int i;
    memcpy(hwr_collected_billboards_prev, hwr_collected_billboards,
        (size_t)hwr_collected_count * sizeof(hwr_collected_billboards[0]));
    memcpy(hwr_collected_key_prev, hwr_collected_key,
        (size_t)hwr_collected_count * sizeof(hwr_collected_key[0]));
    hwr_collected_count_prev = hwr_collected_count;

    for (i = 0; i < HWR_SPRITE_MATCH_HASH; i++)
        spr_match_key[i] = NULL;
    for (i = 0; i < hwr_collected_count_prev; i++) {
        const void *k = hwr_collected_key_prev[i];
        uint32_t idx;
        if (k == NULL) continue;
        idx = hwr_ptr_hash(k) & (HWR_SPRITE_MATCH_HASH - 1);
        while (spr_match_key[idx] != NULL)
            idx = (idx + 1) & (HWR_SPRITE_MATCH_HASH - 1);
        spr_match_key[idx] = k;
        spr_match_idx[idx] = i;
    }
}

/* Resolve hwr_collected_prev_idx[] for the current turn's billboards against
 * the match table built by hwr_sw_snapshot_prev_sprites(). Must run AFTER the
 * current turn's hwr_collected_key[] is fully populated (i.e. at the end of
 * hwr_sw_collect_sprites, after effects/glares are appended). */
static void hwr_sw_resolve_sprite_interp(void)
{
    int i;
    for (i = 0; i < hwr_collected_count; i++) {
        const void *k = hwr_collected_key[i];
        uint32_t idx;
        hwr_collected_prev_idx[i] = -1;
        if (k == NULL) continue;
        idx = hwr_ptr_hash(k) & (HWR_SPRITE_MATCH_HASH - 1);
        while (spr_match_key[idx] != NULL) {
            if (spr_match_key[idx] == k) {
                hwr_collected_prev_idx[i] = spr_match_idx[idx];
                break;
            }
            idx = (idx + 1) & (HWR_SPRITE_MATCH_HASH - 1);
        }
    }
}

/* Effect-collection counts (fire/phwoar), surfaced in the KP-7 dump. */
static int          hwr_fire_seen = 0, hwr_fire_coll = 0;
static int          hwr_phwoar_seen = 0, hwr_phwoar_coll = 0;
static int          hwr_effect_badslot = 0;

/* --- Fire dynamic lights ------------------------------------------------
 * The SW renderer lights the ground/objects around a fire via apply_full_light
 * (see ASM_process_napalm_flame / process_temp_light), a per-frame dynamic
 * light path the GL renderer's sw_get_lights() (which only reads the static
 * game_full_lights array) never sees. To reproduce it we accumulate the fire
 * flames collected each frame — bucketed per map tile so a fire's ~20 flames
 * collapse into one warm point light — and hand the result to sw_get_lights().
 * Fire flame x/y/z are already in the same raw world space as the floor verts
 * and game_full_lights (the billboard path uses them directly). */
#define HWR_FIRELIGHT_MAX 48
struct HwrFireLightAcc { long sx, sy, sz; int n; };
static struct HwrFireLightAcc hwr_firelight_acc[HWR_FIRELIGHT_MAX];
static int hwr_firelight_acc_count = 0;
struct HwrFireLight { float x, y, z, strength; };
static struct HwrFireLight hwr_firelights[HWR_FIRELIGHT_MAX];
static int hwr_firelight_count = 0;
/* Squared flame->cluster merge radius (world units²), refreshed each frame from
 * the [firelight] cluster tunable at the top of hwr_sw_collect_effects. */
static long hwr_firelight_merge2 = 768L * 768L;

/* Cheap per-light flicker PRNG (LCG). Independent from the game RNG so it
 * cannot perturb simulation; reseeds itself, one draw per light per frame. */
static unsigned hwr_flick_seed = 0x1234567u;
static float hwr_flick_rand(void)
{
    hwr_flick_seed = hwr_flick_seed * 1664525u + 1013904223u;
    return (float)((hwr_flick_seed >> 8) & 0xFFFFu) / 65535.0f;
}

/* Burning people cast fire light too, but their flames are DrIT_Unkn15 scale
 * sprites (not DrIT_SFireFlame), so they never reach the flame loop that feeds
 * the ground-light accumulator. The sprite collector records each burning
 * person's world position here; hwr_sw_collect_effects() feeds them into the
 * same firelight clusters (with a per-person flame weight) before finalize. */
#define HWR_BURNING_PPL_MAX     64
#define HWR_BURNING_PERSON_FLAMES 8   /* cluster weight ~= a decent fire pool */
static float hwr_burning_ppl[HWR_BURNING_PPL_MAX][3];
static int   hwr_burning_ppl_count = 0;

/* Persuaded people get their own cold turquoise pool, on the same clustering
 * machinery but a separate accumulator so the colours never mix. They used to
 * fall into the fire path because set_person_persuaded() (src/people.c) raises
 * TngF_Unkn40000000 — the same flag a burning person carries — so a persuaded
 * civilian lit the street orange as if alight. */
#define HWR_PERSUADED_PPL_MAX   64
#define HWR_PERSUADELIGHT_MAX   24
static float hwr_persuaded_ppl[HWR_PERSUADED_PPL_MAX][3];
static int   hwr_persuaded_ppl_count = 0;
static struct HwrFireLightAcc hwr_persuadelight_acc[HWR_PERSUADELIGHT_MAX];
static int hwr_persuadelight_acc_count = 0;
static struct HwrFireLight hwr_persuadelights[HWR_PERSUADELIGHT_MAX];
static int hwr_persuadelight_count = 0;
static long hwr_persuadelight_merge2 = 768L * 768L;

/* Proximity-merge one emitter into a cluster accumulator: it joins the nearest
 * existing cluster within the merge radius, otherwise starts a new one. Grid
 * bucketing split a single fire (whose ~20 flames spread up to ~2 tiles) across
 * several tile-lights; proximity merging keeps one blaze — and neighbouring
 * fires within the radius — as a single light. */
static void hwr_cluster_add(struct HwrFireLightAcc *acc, int *acc_count,
                            int acc_max, long merge2, int x, int y, int z)
{
    int i, best = -1;
    long best_d2 = 0;
    for (i = 0; i < *acc_count; i++) {
        struct HwrFireLightAcc *a = &acc[i];
        long cx = a->sx / a->n, cz = a->sz / a->n;   /* running centroid */
        long dx = (long)x - cx, dz = (long)z - cz;
        long d2 = dx*dx + dz*dz;
        if (d2 <= merge2 && (best < 0 || d2 < best_d2)) {
            best = i;
            best_d2 = d2;
        }
    }
    if (best >= 0) {
        acc[best].sx += x;
        acc[best].sy += y;
        acc[best].sz += z;
        acc[best].n++;
        return;
    }
    if (*acc_count >= acc_max)
        return;
    i = (*acc_count)++;
    acc[i].sx = x;
    acc[i].sy = y;
    acc[i].sz = z;
    acc[i].n  = 1;
}

/* Collapse clusters into finalized point lights: centroid position, strength
 * that grows with member count and saturates. Clusters with fewer than min_n
 * members are dropped. `per_unit` is the member count that reaches strength
 * 1.0 (fires: ~10 flames; persuaded crowds: ~3 people). */
static int hwr_cluster_finalize(const struct HwrFireLightAcc *acc, int acc_count,
                                struct HwrFireLight *out, int min_n, float per_unit)
{
    int i, n = 0;
    for (i = 0; i < acc_count; i++) {
        const struct HwrFireLightAcc *a = &acc[i];
        struct HwrFireLight *fo;
        float strength;
        if (a->n < min_n)
            continue;
        fo = &out[n++];
        fo->x = (float)(a->sx / a->n);
        fo->y = (float)(a->sy / a->n);
        fo->z = (float)(a->sz / a->n);
        strength = (float)a->n / per_unit;
        if (strength < 0.35f) strength = 0.35f;
        if (strength > 1.25f) strength = 1.25f;
        fo->strength = strength;
    }
    return n;
}

static void hwr_firelight_add(int x, int y, int z)
{
    hwr_cluster_add(hwr_firelight_acc, &hwr_firelight_acc_count,
                    HWR_FIRELIGHT_MAX, hwr_firelight_merge2, x, y, z);
}

static void hwr_persuadelight_add(int x, int y, int z)
{
    hwr_cluster_add(hwr_persuadelight_acc, &hwr_persuadelight_acc_count,
                    HWR_PERSUADELIGHT_MAX, hwr_persuadelight_merge2, x, y, z);
}

/* Clusters with fewer than firelight_min_flames flames are dropped
 * (suppresses small/lone fires). */
static void hwr_firelight_finalize(void)
{
    int min_flames = hwr_lights_defaults().firelight_min_flames;
    if (min_flames < 1) min_flames = 1;
    hwr_firelight_count = hwr_cluster_finalize(hwr_firelight_acc,
        hwr_firelight_acc_count, hwr_firelights, min_flames, 10.0f);
}

static void hwr_persuadelight_finalize(void)
{
    hwr_persuadelight_count = hwr_cluster_finalize(hwr_persuadelight_acc,
        hwr_persuadelight_acc_count, hwr_persuadelights, 1, 3.0f);
}

/* Screen-space overlay quads (shield/blast/lightning special faces), snapshotted
 * at gate time — the draw list and screen-point pool are reset/overwritten
 * before the GL present, so they must be captured here, not read live. */
#define HWR_MAX_OVERLAYS 4096
static HwrOverlayQuad hwr_collected_overlays[HWR_MAX_OVERLAYS];
static int            hwr_collected_overlay_count = 0;

/* Diagnostic: histogram of draw-item types (DrIT_*) in the draw list this frame,
 * dumped on KP-7 — to find which type the chimney/building smoke uses. */
static int            hwr_di_hist[64];

/* Viewport, supplied by the host at creation time. */
static int sw_view_w = 0;
static int sw_view_h = 0;

/* The skip mask the drawlist executor checks — storage defined in
 * engindrwlstx.c (libswrender), linked at the final executable. 512 bytes =
 * 4096 bits to cover game_sort_sprites (up to 4001 entries). */
extern unsigned char hwr_sprite_skip_mask[512];

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

/* Composite a sprite frame's version-0 elements into the atlas (with the same
 * xBR upscale as the Thing path) and return its atlas slot, or <0 on failure.
 * Used for the effect arrays (fire/phwoar) which draw a plain frame with no FRV
 * versioning — the SW drawers (draw_frame_on_screen / draw_frame_scaled_alpha)
 * only emit elements whose version bits (Flags & 0xFE00) are zero, so we match
 * that here. *out_fw/*out_fh receive the composited (pre-xBR) pixel size. */
static int hwr_effect_frame_slot(unsigned short frm_idx, int *out_fw, int *out_fh,
    int *out_off_x, int *out_off_y)
{
    struct Frame *frm;
    unsigned short el_idx;
    int off_x, off_y, max_x, max_y, fw, fh, slot, xbr_key;
    uint32_t key;

    if (frm_idx == 0 || frm_idx >= (unsigned short)(frame_end - frame))
        return -1;
    frm = &frame[frm_idx];
    xbr_key = hwr_lights_defaults().xbr_scale;
    key = ((uint32_t)frm_idx << 18) | ((uint32_t)(xbr_key & 0x03) << 2);

    /* Bounding box of the version-0 elements. */
    off_x = 0x7FFFFFFF; off_y = 0x7FFFFFFF;
    max_x = -0x7FFFFFFF; max_y = -0x7FFFFFFF;
    for (el_idx = frm->FirstElement; el_idx > 0; ) {
        struct Element *el;
        if (el_idx >= (unsigned short)(mele_ani_end - melement_ani))
            break;
        el = &melement_ani[el_idx];
        if (el->ToSprite > 0 && (el->Flags & 0xFE00) == 0) {
            struct TbSprite *spr = (struct TbSprite *)((uint8_t *)m_sprites + el->ToSprite);
            if (spr > m_sprites && spr < m_sprites_end) {
                int ex = (int)(el->X) >> 1;
                int ey = (int)(el->Y) >> 1;
                int sw = spr->SWidth, sh = spr->SHeight;
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
    fw = max_x - off_x; fh = max_y - off_y;
    if (fw <= 0 || fh <= 0 || fw > 256 || fh > 256)
        return -1;
    if (out_fw) *out_fw = fw;
    if (out_fh) *out_fh = fh;
    if (out_off_x) *out_off_x = off_x;
    if (out_off_y) *out_off_y = off_y;

    slot = hwr_atlas_find(key);
    if (slot != -1)
        return slot;   /* cached (>=0) or blacklisted (-2) */

    {
        uint8_t comp[256 * 256 * 4];
        memset(comp, 0, (size_t)fw * fh * 4);
        for (el_idx = frm->FirstElement; el_idx > 0; ) {
            struct Element *el;
            if (el_idx >= (unsigned short)(mele_ani_end - melement_ani))
                break;
            el = &melement_ani[el_idx];
            if (el->ToSprite > 0 && (el->Flags & 0xFE00) == 0) {
                struct TbSprite *spr = (struct TbSprite *)((uint8_t *)m_sprites + el->ToSprite);
                if (spr > m_sprites && spr < m_sprites_end) {
                    int spr_w = spr->SWidth, spr_h = spr->SHeight;
                    int el_x = ((int)(el->X) >> 1) - off_x;
                    int el_y = ((int)(el->Y) >> 1) - off_y;
                    int flip_h = (el->Flags & 0x0001) != 0;
                    if (spr_w > 0 && spr_h > 0 && spr_w <= 256 && spr_h <= 256) {
                        uint8_t temp[256 * 256], opq[256 * 256];
                        int row, col;
                        memset(temp, 0, sizeof(temp));
                        memset(opq, 0, sizeof(opq));
                        if (hwr_rle_decode_opaque(spr->Data, temp, opq, spr_w, spr_h) == 0) {
                            /* Effects are self-lit — bake the raw full-bright palette
                             * colour (no fade-table dim) so fire stays bright; the
                             * billboard shade is set to full at draw time. */
                            for (row = 0; row < spr_h && el_y + row < fh; row++) {
                                for (col = 0; col < spr_w && el_x + col < fw; col++) {
                                    int idx = row * spr_w + col;
                                    int dst_x = flip_h ? (el_x + spr_w - 1 - col) : (el_x + col);
                                    int dst = ((el_y + row) * fw + dst_x) * 4;
                                    int pixel = temp[idx];
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
            if (sf >= 2 && sf <= 4 && fw * sf <= 4096 && fh * sf <= 4096) {
                int sw = fw * sf, sh = fh * sf;
                scaled = (uint8_t *)malloc((size_t)sw * sh * 4);
                if (scaled) {
                    if (xbr_scale(comp, scaled, fw, fh, sf) == 0) {
                        reg_pixels = scaled; reg_w = sw; reg_h = sh;
                        hwr_xbr_count++;
                    } else {
                        free(scaled); scaled = NULL;
                    }
                }
            }
            slot = hwr_atlas_register(key, reg_pixels, reg_w, reg_h);
            if (slot < 0 && reg_pixels != comp)
                slot = hwr_atlas_register(key, comp, fw, fh);
            if (scaled) free(scaled);
        }
    }
    return slot;
}

/* Collect fire flames + phwoar (smoke/explosion) clouds from the SW draw list as
 * translucent billboards. Fire/explosions blend additively (glow); smoke blends
 * alpha-over. Items we successfully collect are marked in the per-effect skip
 * masks so the SW drawlist executor skips their software draw; items we cannot
 * atlas are left for the software renderer (no skip bit set). */
static void hwr_sw_collect_effects(void)
{
    unsigned short i;

    hwr_fire_seen = hwr_fire_coll = 0;
    hwr_phwoar_seen = hwr_phwoar_coll = 0;
    hwr_effect_badslot = 0;
    hwr_firelight_acc_count = 0;
    hwr_persuadelight_acc_count = 0;
    {
        /* Flame->cluster merge radius, from the [firelight] cluster tunable
         * (tiles -> world units, 256/tile). */
        float ctiles = hwr_lights_defaults().firelight_cluster;
        long merge = (long)(ctiles * 256.0f);
        if (merge < 128) merge = 128;
        hwr_firelight_merge2 = merge * merge;
        ctiles = hwr_lights_defaults().persuadelight_cluster;
        merge = (long)(ctiles * 256.0f);
        if (merge < 128) merge = 128;
        hwr_persuadelight_merge2 = merge * merge;
    }

    for (i = 1; i < next_draw_item && hwr_collected_count < HWR_MAX_COLLECTED; i++) {
        struct DrawItem *itm = &game_draw_list[i];
        int is_fire   = (itm->Type == HWR_DI_SFireFlame);
        int is_phwoar = (itm->Type == HWR_DI_SFrmPhwoar);
        unsigned short off = itm->Offset;
        unsigned short frm_idx;
        float wx, wy, wz, bigf = 1.0f;
        int fw = 0, fh = 0, slot;
        const void *eff_key;

        if (!is_fire && !is_phwoar)
            continue;
        if (is_fire) hwr_fire_seen++; else hwr_phwoar_seen++;

        /* World position. The GL billboard vertex shader subtracts the camera
         * centre uCtr = (xc, 8*yc, zc); the SW enlist of these effects feeds
         * transform_shpoint a vertical delta of (worldY - yc) - 8*yc (one extra
         * -yc vs the Thing path), so the matching billboard Y carries that -yc.
         * Fire stores world units directly (short x/y/z); phwoar stores fixed
         * point like a Thing (X>>8, Y>>5, Z>>8). */
        if (is_fire) {
            struct HwrFireFlame *fl;
            if (off >= 512) continue;
            fl = &FIRE_flame[off];
            frm_idx = fl->frame;
            wx = (float)fl->x;
            wy = (float)fl->y - (float)snap.yc;
            wz = (float)fl->z;
            /* draw_fire_flame scales by (overall_scale*(big+128))>>7 when big!=0;
             * (big+128)/128 is 1.0 at big==0 so it works for the unscaled case too. */
            bigf = (float)((int)fl->big + 128) / 128.0f;
            if (bigf < 0.25f) bigf = 0.25f;
            /* Feed the ground-light accumulator with the flame's raw world
             * position (same space as the floor verts). Done before the atlas
             * attempt so a fire still lights the ground even when its billboard
             * falls back to the SW renderer. */
            hwr_firelight_add((int)fl->x, (int)fl->y, (int)fl->z);
            eff_key = (const void *)fl;
        } else {
            struct HwrPhwoar *ph;
            if (off >= 1024) continue;
            ph = &phwoar[off];
            frm_idx = ph->f;
            wx = (float)(ph->x >> 8);
            wy = (float)(ph->y >> 5) - (float)snap.yc;
            wz = (float)(ph->z >> 8);
            eff_key = (const void *)ph;
        }

        slot = hwr_effect_frame_slot(frm_idx, &fw, &fh, NULL, NULL);
        if (slot < 0) {
            hwr_effect_badslot++;
            continue;   /* leave to the SW renderer (don't set the skip bit) */
        }
        if (is_fire) hwr_fire_coll++; else hwr_phwoar_coll++;

        {
            HwrBillboard *bb = &hwr_collected_billboards[hwr_collected_count];
            float sc = (float)snap.scale;
            float rnorm = sqrtf((float)snap.D14 * snap.D14 + (float)snap.D10 * snap.D10);
            float res_scale = (sw_view_h > 0) ? (float)sw_view_h / 480.0f : 1.0f;
            bb->x = wx; bb->y = wy; bb->z = wz;
            bb->sprite = (uint16_t)slot;
            bb->shade = 48;   /* full brightness — effects are self-lit */
            bb->flags = HWR_BILLBOARD_TRANSLUCENT | HWR_BILLBOARD_NOSHADOW
                      | (is_fire ? HWR_BILLBOARD_ADDITIVE : 0);
            /* Anchor already IS the quad centre for effects (no feet/anchor
             * offset like Thing sprites) — must explicitly zero since this
             * array slot is reused frame-to-frame and may hold a stale
             * nonzero ratio from a previous frame's Thing-sprite entry. */
            bb->anchor_ratio_x = 0.0f;
            bb->anchor_ratio_y = 0.0f;
            if (sc <= 0.0f) sc = 256.0f;
            /* Fire flames render about 20% too small versus the original SW
             * proportions; boost them to match. */
            if (is_fire) bigf *= 1.2f;
            if (rnorm > 0.001f && snap.D1C != 0) {
                bb->half_size_x = (float)fw * 100663296.0f / (sc * rnorm) * 0.85f * res_scale * bigf;
                bb->half_size_y = (float)fh * 100663296.0f / (sc * (float)snap.D1C) * 0.85f * res_scale * bigf;
            } else {
                bb->half_size_x = (float)fw * 18.0f * 0.85f * res_scale * bigf;
                bb->half_size_y = (float)fh * 18.0f * 0.85f * res_scale * bigf;
            }
            /* Effects anchor at the emitter centre (the projected x/y/z point),
             * unlike Thing sprites which anchor at the feet. Keyed by the
             * FIRE_flame/phwoar array slot address (fixed arrays; a reused slot
             * jump is caught by sw_get_sprites' teleport guard). */
            hwr_collected_key[hwr_collected_count] = eff_key;
            hwr_collected_count++;
        }

        if (is_fire)
            hwr_fire_skip_mask[off >> 3] |= (uint8_t)(1 << (off & 7));
        else
            hwr_phwoar_skip_mask[off >> 3] |= (uint8_t)(1 << (off & 7));
    }

    /* Burning people cast fire light too. Their flames are DrIT_Unkn15 scale
     * sprites (not DrIT_SFireFlame), so they miss the flame loop above; the
     * sprite collector recorded their positions in hwr_burning_ppl[]. Add each
     * with a small flame weight so it forms (or joins) a firelight cluster. Runs
     * after the flame loop (which reset the accumulator) and before finalize.
     * NOTE hwr_sw_collect_effects() is called from hwr_sw_collect_sprites()
     * AFTER its person loop, so hwr_burning_ppl[] is fully populated here. */
    {
        int b, w;
        for (b = 0; b < hwr_burning_ppl_count; b++)
            for (w = 0; w < HWR_BURNING_PERSON_FLAMES; w++)
                hwr_firelight_add((int)hwr_burning_ppl[b][0],
                                  (int)hwr_burning_ppl[b][1],
                                  (int)hwr_burning_ppl[b][2]);
    }

    /* Persuaded people, same deal but into the turquoise accumulator (one
     * emitter each — a knot of followers merges into one bigger pool). */
    {
        int b;
        for (b = 0; b < hwr_persuaded_ppl_count; b++)
            hwr_persuadelight_add((int)hwr_persuaded_ppl[b][0],
                                  (int)hwr_persuaded_ppl[b][1],
                                  (int)hwr_persuaded_ppl[b][2]);
    }

    hwr_firelight_finalize();
    hwr_persuadelight_finalize();
}

/* Emit the light glares (car headlights / street lamps) recorded by build_glare
 * as additive glow billboards. build_glare uses transform_point, which equals
 * transform_shpoint with a vertical delta of (y - (yc>>3)) - 8*yc, so the
 * matching billboard Y is y - (yc>>3). Their SW screen-space faces are
 * suppressed under FX3D (drawitem_is_suppressed_glare). */
static void hwr_sw_collect_glares(void)
{
    int i;
    int slot_white, slot_red, slot_blue;
    HwrLightDefaults gld = hwr_lights_defaults();
    /* Police siren flash phase: alternate red/blue every 250 ms (each colour
     * pulses twice a second). 0 = red side lit, 1 = blue side lit. */
    int phase = (int)((SDL_GetTicks() / 250u) & 1u);
    if (hwr_glare_count <= 0)
        return;
    slot_white = hwr_atlas_glow_slot(0);
    slot_red   = hwr_atlas_glow_slot(1);
    slot_blue  = hwr_atlas_glow_slot(2);
    if (slot_white < 0)
        return;
    for (i = 0; i < hwr_glare_count && hwr_collected_count < HWR_MAX_COLLECTED; i++) {
        int siren = hwr_glare_list[i].siren;
        int slot;
        HwrBillboard *bb;
        float r = (float)hwr_glare_list[i].r;
        if (siren == 1) {
            if (phase != 0) continue;          /* red shows on phase 0 only */
            slot = (slot_red >= 0) ? slot_red : slot_white;
        } else if (siren == 2) {
            if (phase != 1) continue;          /* blue shows on phase 1 only */
            slot = (slot_blue >= 0) ? slot_blue : slot_white;
        } else {
            slot = slot_white;                 /* plain headlight / lamp */
        }
        bb = &hwr_collected_billboards[hwr_collected_count];
        bb->x = (float)hwr_glare_list[i].x;
        bb->y = (float)hwr_glare_list[i].y - (float)(snap.yc >> 3);
        bb->z = (float)hwr_glare_list[i].z;
        bb->sprite = (uint16_t)slot;
        bb->shade = 48;   /* full brightness — self-lit glow */
        bb->flags = HWR_BILLBOARD_TRANSLUCENT | HWR_BILLBOARD_NOSHADOW
                  | HWR_BILLBOARD_ADDITIVE;
        /* Anchor already IS the quad centre; must be explicit (reused array
         * slot), see the fire/phwoar site above for why. */
        bb->anchor_ratio_x = 0.0f;
        bb->anchor_ratio_y = 0.0f;
        {
            float w = (siren == 1) ? gld.glare_red_width
                    : (siren == 2) ? gld.glare_blue_width
                    : gld.glare_headlamp_width;
            bb->half_size_x = r * w;
            bb->half_size_y = r * w;
        }
        hwr_collected_key[hwr_collected_count] = NULL;  /* ephemeral, not interpolated */
        hwr_collected_count++;
    }
}

/* Snapshot the flat-tinted special-face overlay quads (shield-hit / blast /
 * lightning) at gate time. The special faces (DrIT_SpObFace4 modes 15/17) carry
 * already-projected screen points + a palette colour (ExCol); we capture them
 * now because the draw list and screen-point pool are reset before the GL
 * present. Their SW draw is suppressed under FX3D. */
static void hwr_sw_collect_overlays(void)
{
    unsigned short i;
    hwr_collected_overlay_count = 0;
    if (game_draw_list == NULL || game_special_obj_faces4 == NULL ||
        game_screen_point_pool == NULL)
        return;
    for (i = 1; i < next_draw_item && hwr_collected_overlay_count < HWR_MAX_OVERLAYS; i++) {
        struct DrawItem *itm = &game_draw_list[i];
        struct HwrSpObFace4 *fc;
        HwrOverlayQuad *q;
        int k, col;
        /* Spark/weapon lines (DrIT_Unkn11/SortLine) are now drawn by the
         * depth-tested beam pass (hwr_beams_render) using captured world/screen
         * endpoints WITH depth, so they are occluded by 3D geometry instead of
         * painting on top as a flat screen overlay. Skip them here to avoid
         * double-drawing. */
        if (itm->Type == HWR_DI_Unkn11)
            continue;
        if (itm->Type != HWR_DI_SpObFace4)
            continue;
        fc = &game_special_obj_faces4[itm->Offset];
        /* 15 = tinted slices (laser/lightning), 17 = shaded circle fans
         * (shield-hit / blast / recoil / nuclear discs). */
        if (fc->Flags != 15 && fc->Flags != 17)
            continue;
        q = &hwr_collected_overlays[hwr_collected_overlay_count];
        if (fc->Flags == 15) {
            /* Laser/lightning slices (build_polygon_slice, engindrwlstm_3d.c):
             * PointNo is a "ladder" order {start+, start-, end+, end-}, not a
             * perimeter walk. hwr_overlay_render's fixed {0,1,2,0,2,3} split
             * assumes consecutive points trace the quad's perimeter (true for
             * mode 17's fan order); applied to the ladder order it mixes
             * triangles from both diagonals into a self-intersecting bowtie
             * (the "shaded triangles" corruption). Remap to perimeter order
             * start+ -> end+ -> end- -> start- here instead. */
            static const int perim[4] = { 0, 2, 3, 1 };
            for (k = 0; k < 4; k++) {
                struct HwrSpecialPoint *sp = &game_screen_point_pool[(uint16_t)fc->PointNo[perim[k]]];
                q->x[k] = (float)sp->X;
                q->y[k] = (float)sp->Y;
            }
        } else {
            for (k = 0; k < 4; k++) {
                struct HwrSpecialPoint *sp = &game_screen_point_pool[(uint16_t)fc->PointNo[k]];
                q->x[k] = (float)sp->X;
                q->y[k] = (float)sp->Y;
            }
        }
        col = fc->ExCol & 0xFF;
        q->r = (float)lbPaletteColors[col].r / 255.0f;
        q->g = (float)lbPaletteColors[col].g / 255.0f;
        q->b = (float)lbPaletteColors[col].b / 255.0f;
        q->a = 0.5f;   /* mode 15/17 = 50% ghost blend */
        q->slot = -1;  /* flat-coloured quad */
        hwr_collected_overlay_count++;
    }
}

/* Snapshot the active explosion-shrapnel indices from the draw list at collect
 * time (DrIT_SharpnlPoly items, added by draw_bang during the SW build). Read
 * back by emit_shrapnels() during the geometry build. Captured here rather than
 * walked live because the draw list is reset/rebuilt before/within the present. */
static void hwr_sw_collect_shrapnels(void)
{
    unsigned short i;
    hwr_shrapnel_active_count = 0;
    if (game_draw_list == NULL)
        return;
    for (i = 1; i < next_draw_item &&
         hwr_shrapnel_active_count < HWR_SHRAPNEL_COUNT; i++) {
        struct DrawItem *itm = &game_draw_list[i];
        if (itm->Type != HWR_DI_SharpnlPoly)
            continue;
        if (itm->Offset < 1 || itm->Offset >= HWR_SHRAPNEL_COUNT)
            continue;
        hwr_shrapnel_active[hwr_shrapnel_active_count++] = itm->Offset;
    }
}

void hwr_sw_collect_sprites(void)
{
    unsigned short i;
    hwr_sw_snapshot_prev_sprites();
    hwr_collected_count = 0;
    hwr_burning_ppl_count = 0;
    hwr_persuaded_ppl_count = 0;
    hwr_xbr_count = 0;
    int hwr_eligible_count = 0;
    int hwr_passed_count = 0;
    memset(hwr_sprite_skip_mask, 0, sizeof(hwr_sprite_skip_mask));
    memset(hwr_fire_skip_mask, 0, sizeof(hwr_fire_skip_mask));
    memset(hwr_phwoar_skip_mask, 0, sizeof(hwr_phwoar_skip_mask));
    render_ghost = &pixmap.ghost_table[0];

    if (!snap.valid || game_draw_list == NULL || game_sort_sprites == NULL)
        return;
    if (frame == NULL || m_sprites == NULL || melement_ani == NULL)
        return;

    /* Diagnostic: count every draw-item type present this frame. */
    memset(hwr_di_hist, 0, sizeof(hwr_di_hist));
    for (i = 1; i < next_draw_item; i++)
        hwr_di_hist[game_draw_list[i].Type & 63]++;

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

        /* Scale-effect sprites (DrIT_Unkn15): drift smoke (chimneys, burning
         * buildings), explosion clouds, flames, splashes. Here SortSprite.Scale
         * is a SCALE FACTOR, not an FRV version pack, and the SW draws the whole
         * frame alpha-blended & scaled (draw_sort_sprite1c -> draw_frame_scaled_
         * alpha). Treating Scale as FRV (as the normal path does) composites a
         * garbage frame that gets skipped or thrashes the atlas -> the smoke fell
         * back to the flickering software bitmap. Composite without FRV (effect
         * compositor) and size by the scale, as a translucent billboard. */
        if (itm->Type == HWR_DI_Unkn15) {
            int efw = 0, efh = 0;
            int eslot = hwr_effect_frame_slot(ss->Frame, &efw, &efh, NULL, NULL);
            if (eslot >= 0) {
                HwrBillboard *bb = &hwr_collected_billboards[hwr_collected_count];
                float sc = (float)snap.scale;
                float rnorm = sqrtf((float)snap.D14 * snap.D14 + (float)snap.D10 * snap.D10);
                float res_scale = (sw_view_h > 0) ? (float)sw_view_h / 480.0f : 1.0f;
                float scl = (float)ss->Scale / 256.0f;   /* scale-effect enlargement */
                int st = thing->SubType;
                int is_flame = (st == 46 || st == 52 || st == 56 || st == 57);
                /* Fade the particle out over its remaining life. shade now drives
                 * the effect ALPHA (see the self-lit shader path), so newest
                 * smoke is opaque and it dissolves as Timer1 counts down to 0.
                 * Use the full life if StartTimer1 is set, else fade over the
                 * last ~64 ticks. */
                float life = (thing->StartTimer1 > 0) ? (float)thing->StartTimer1 : 64.0f;
                float fade = (float)thing->Timer1 / life;
                if (fade < 0.0f) fade = 0.0f;
                if (fade > 1.0f) fade = 1.0f;
                if (sc <= 0.0f) sc = 256.0f;
                if (scl <= 0.05f) scl = 1.0f;
                bb->x = (float)(thing->X >> 8);
                bb->y = (float)(thing->Y >> 5);
                bb->z = (float)(thing->Z >> 8);
                bb->sprite = (uint16_t)eslot;
                /* 32 = full (identity) on the draw-time bri/32 scale; this is an
                 * alpha ramp for the fade-out, so keep it topping out at 1.0. */
                bb->shade = (uint8_t)(fade * 32.0f + 0.5f);
                bb->flags = HWR_BILLBOARD_TRANSLUCENT | HWR_BILLBOARD_NOSHADOW
                          | (is_flame ? HWR_BILLBOARD_ADDITIVE : 0);
                /* Anchor already IS the quad centre; must be explicit (reused
                 * array slot), see the fire/phwoar site for why. */
                bb->anchor_ratio_x = 0.0f;
                bb->anchor_ratio_y = 0.0f;
                if (rnorm > 0.001f && snap.D1C != 0) {
                    bb->half_size_x = (float)efw * 100663296.0f / (sc * rnorm) * 0.85f * res_scale * scl;
                    bb->half_size_y = (float)efh * 100663296.0f / (sc * (float)snap.D1C) * 0.85f * res_scale * scl;
                } else {
                    bb->half_size_x = (float)efw * 18.0f * 0.85f * res_scale * scl;
                    bb->half_size_y = (float)efh * 18.0f * 0.85f * res_scale * scl;
                }
                hwr_collected_key[hwr_collected_count] = (const void *)thing;
                hwr_collected_count++;
                hwr_sprite_skip_mask[ss_idx >> 3] |= (uint8_t)(1 << (ss_idx & 7));
            }
            continue;
        }

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

            /* Compute element bounding box (version check always — same as SW).
             * Also detect whether any gun-overlay elements (frv_idx==4) are
             * visible in this frame — those are baked at full brightness and must
             * bypass scene lighting (see SW LbSpriteDraw path for frv_idx==4). */
            unsigned short el_idx;
            int off_x, off_y, max_x, max_y;
            int has_gun_overlay = 0;
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
                        if (frv_idx == 4) has_gun_overlay = 1;
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
                                     * light/dark blink on walking/running characters.
                                     * Bake at IDENTITY (fade row 32 = 1.0x): the fade rows
                                     * scale colours by i/32 through LbPaletteFindColour, so
                                     * baking overbright (the old 60 = 1.875x) channel-clamped
                                     * and hue-shifted colours INTO the atlas - dark dithered
                                     * art (trees) washed out to pink. Identity keeps exact
                                     * palette colours; brightness is applied at draw time. */
                                    int bri = 32;
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
            } else if (slot < 0) {
                /* Blacklisted (-2): atlas full, never retry this key. */
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
                /* State sits at the same offset in struct Thing and SimpleThing
                 * (shared prefix), so it's safe to read here even for persons
                 * (where `thing` is really a struct Thing). PerSt_DEAD == 0xD
                 * (people.h) — corpses shouldn't cast a standing blob shadow. */
                int is_dead_body = (is_person && thing && thing->State == 0xD);
                /* Sprites bake at a fixed brightness (above); apply the per-instance
                 * Brightness here as the draw-time shade, including the angle-gated
                 * +15 bonus.  Because this is per frame (not baked), identical
                 * sprites no longer collide on one baked brightness and turning no
                 * longer re-bakes/blinks. */
                {
                    int bonus = (frv_arr[4] != 0 && angle > 1 && angle < 7) ? 15 : 0;
                    int sh = (int)ss->Brightness + bonus;
                    /* Static map sprites: SW passes the tile's cached ShadeR as
                     * Brightness (build_static). Diagnostics showed those values
                     * are valid under GL (Ambient/4 + the tile's lamp light), so
                     * use them as-is - they already carry the SW look. */
                    if (sh < 10) sh = 10;
                    if (sh > 75) sh = 75;
                    bb->shade = (uint8_t)sh;
                }
                /* A person on fire (PerSt_PERSON_BURNING == 0x24, or the burning
                 * flag TngF_Unkn40000000) is engulfed in self-lit flame
                 * billboards, but their body sprite otherwise keeps its dark
                 * scene shade and reads as an unlit silhouette inside the flames.
                 * Brighten the body so it glows as if lit by the fire (48 =
                 * identity/full-bright; push overbright so it clearly reads as
                 * lit). The flame billboards still draw on top. */
                /* Dying/dead on the ground (PerSt_DIE/DIEING/DEAD == 0xB..0xD):
                 * the burning flag lingers on the corpse, but the glow + fire
                 * light should go out once the body hits the floor. */
                int dying_or_dead = (thing && thing->State >= 0xB
                                            && thing->State <= 0xD);
                /* TngF_Unkn40000000 is NOT burning-exclusive: set_person_persuaded()
                 * raises it too (and it stays up on a persuaded follower), so it
                 * alone made every persuadertron convert glow and light the street
                 * like a bonfire. Persuasion is identified by
                 * PerSt_BEING_PERSUADED (0x3B) or TngF_Persuaded (0x00080000);
                 * only an explicit PerSt_PERSON_BURNING, or the flag on someone
                 * who is neither, counts as on fire. TngF_Persuaded stays up for
                 * the rest of the mission, so it has to keep suppressing the
                 * fire path — but the turquoise glow is only for the conversion
                 * itself (state 0x3B), not for the follower afterwards. */
                int is_persuaded = (is_person && thing && !dying_or_dead
                                    && (thing->State == 0x3B
                                        || (thing->Flag & 0x00080000u)));
                int is_being_persuaded = (is_person && thing && !dying_or_dead
                                          && thing->State == 0x3B);
                int is_burning = (is_person && thing && !dying_or_dead
                                  && (thing->State == 0x24
                                      || ((thing->Flag & 0x40000000u) && !is_persuaded)));
                if (is_burning) {
                    if (bb->shade < 68) bb->shade = 68;
                    /* Also let them cast fire light on their surroundings (fed
                     * into the firelight clusters in hwr_sw_collect_effects). */
                    if (hwr_burning_ppl_count < HWR_BURNING_PPL_MAX) {
                        hwr_burning_ppl[hwr_burning_ppl_count][0] = wx;
                        hwr_burning_ppl[hwr_burning_ppl_count][1] = wy;
                        hwr_burning_ppl[hwr_burning_ppl_count][2] = wz;
                        hwr_burning_ppl_count++;
                    }
                } else if (is_being_persuaded) {
                    /* No body overbright — they are not alight, and the sprite
                     * shader cannot tint, so a white-hot body would just look
                     * like fire without flames. Only the ground pool. */
                    if (hwr_persuaded_ppl_count < HWR_PERSUADED_PPL_MAX) {
                        hwr_persuaded_ppl[hwr_persuaded_ppl_count][0] = wx;
                        hwr_persuaded_ppl[hwr_persuaded_ppl_count][1] = wy;
                        hwr_persuaded_ppl[hwr_persuaded_ppl_count][2] = wz;
                        hwr_persuaded_ppl_count++;
                    }
                }
                /* NOSHADOW (light emitters don't cast blob shadows) is a separate
                 * shadow-casting concern, unrelated to brightness. */
                bb->flags = (is_emitter || is_dead_body) ? HWR_BILLBOARD_NOSHADOW : 0;
                /* Gun/weapon overlay (frv_idx==4) elements are baked at full
                 * brightness; set shade=48 and UNLIT so the shader floors lighting
                 * at 1.0 — character is always full bright, external lights overbright.
                 * Must be set AFTER the bb->flags= assignment above or it is wiped. */
                if (has_gun_overlay) {
                    bb->shade = 48;
                    bb->flags |= HWR_BILLBOARD_UNLIT;
                }
                /* The effect-versioned frames (DrIT_SFrmEfctV) are the person
                 * shield bubble — a translucent energy overlay, not a solid
                 * sprite. Route it through the blended pass (alpha) and don't let
                 * it cast a blob shadow. */
                if (itm->Type == HWR_DI_SFrmEfctV)
                    bb->flags |= HWR_BILLBOARD_TRANSLUCENT | HWR_BILLBOARD_NOSHADOW;
                /* Scale-effect sprites (DrIT_Unkn15) are smoke / flames /
                 * splashes — translucent, not solid. Drift smoke (chimney smoke,
                 * burning/destroyed buildings) and explosion clouds blend alpha;
                 * the flame subtypes glow additively. Without this they render as
                 * opaque billboards (the "solid software-looking" smoke). */
                if (itm->Type == HWR_DI_Unkn15) {
                    int st = thing ? thing->SubType : 0;
                    int is_flame = (st == 46 || st == 52 || st == 56 || st == 57);
                    bb->flags |= HWR_BILLBOARD_TRANSLUCENT | HWR_BILLBOARD_NOSHADOW
                              | (is_flame ? HWR_BILLBOARD_ADDITIVE : 0);
                }
                /* Dropped items sit at the same spot as the dead body that
                 * dropped them; bias them toward the camera so they always draw
                 * on top and stay easy to click. Full brightness and unlit so
                 * they stay clearly visible/readable regardless of scene shade
                 * (dark alleys, building shadows) — matches the gun-overlay
                 * treatment above. */
                if (itm->Type == HWR_DI_SFrmStatc && thing->Type == HWR_SMTT_DROPPED_ITEM) {
                    bb->flags |= HWR_BILLBOARD_ONTOP | HWR_BILLBOARD_UNLIT;
                    bb->shade = 48;
                }
                {
                float sc = (float)snap.scale;
                if (sc <= 0.0f) sc = 256.0f;
                float rnorm = sqrtf((float)snap.D14 * snap.D14 + (float)snap.D10 * snap.D10);
                float res_scale = (sw_view_h > 0) ? (float)sw_view_h / 480.0f : 1.0f;
                /* Streetlamp fixture props (thing_categories "street" owners) render
                 * about 10% oversized versus the original SW proportions; scale them
                 * down to match. (The earlier is_person flat corrections of 0.8 and
                 * 0.6 were chasing the wrong bug — see hwr_billboard_dist_scale in
                 * hwr_sprite.c for the real cause/fix; reset to neutral here until
                 * that's confirmed and re-tuned if still needed.) */
                float size_corr = (itm->Type == HWR_DI_SFrmStatc
                    && hwr_thing_category_get(thing->Type, thing->SubType) == 3) ? 0.909f : 1.0f;
                if (rnorm > 0.001f && snap.D1C != 0) {
                    bb->half_size_x = (float)fw * 100663296.0f / (sc * rnorm) * 0.85f * res_scale * size_corr;
                    bb->half_size_y = (float)fh * 100663296.0f / (sc * (float)snap.D1C) * 0.85f * res_scale * size_corr;
                } else {
                    bb->half_size_x = (float)fw * 18.0f * 0.85f * res_scale * size_corr;
                    bb->half_size_y = (float)fh * 18.0f * 0.85f * res_scale * size_corr;
                }
                /* Anchor offset: element X=0/Y=0 (the Thing's true world
                 * anchor, bb->x/y/z as already set above) must appear at the
                 * atlas pixel (-off_x, feet-relative -off_y), not at the quad's
                 * geometric centre. Store this as a ratio of half_size (NOT a
                 * baked world-unit shift) — the shift must be reapplied at
                 * render time using whatever half_size is ACTUALLY used for the
                 * quad (post distance-dampening, hwr_billboard_dist_scale in
                 * hwr_sprite.c); baking it in here at capture-time size caused
                 * the shift to desync from the later-scaled quad and made
                 * asymmetric poses (arm/leg extended, bigger off_x/off_y) visibly
                 * twitch as the dampening factor drifted with camera distance. */
                bb->anchor_ratio_x = (fw > 0)
                    ? ((float)off_x + (float)fw * 0.5f) * 2.0f / (float)fw : 0.0f;
                bb->anchor_ratio_y = (fh > 0)
                    ? (1.0f - 2.0f * (float)(fh + off_y) / (float)fh) : 1.0f;
            }
            hwr_collected_key[hwr_collected_count] = (const void *)thing;
            hwr_collected_count++;
                hwr_sprite_skip_mask[ss_idx >> 3] |= (uint8_t)(1 << (ss_idx & 7));
            }
        }
    }

    /* Append translucent effect billboards (fire/smoke/explosions). */
    hwr_sw_collect_effects();
    hwr_sw_collect_glares();
    hwr_sw_collect_overlays();
    hwr_sw_collect_shrapnels();
    hwr_sw_resolve_sprite_interp();

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
                fprintf(df, "effects: fire seen=%d coll=%d  phwoar seen=%d coll=%d  badslot=%d\n",
                    hwr_fire_seen, hwr_fire_coll, hwr_phwoar_seen, hwr_phwoar_coll, hwr_effect_badslot);
                fprintf(df, "overlays (shield/blast mode15/17): %d  glares=%d\n",
                    hwr_collected_overlay_count, hwr_glare_count);
                fprintf(df, "draw-item types (DrIT:count):");
                for (di = 0; di < 64; di++)
                    if (hwr_di_hist[di])
                        fprintf(df, " %d:%d", di, hwr_di_hist[di]);
                fprintf(df, "\n");
                /* raw_scrd here, for calibrating sprite_dist_falloff: same formula
                 * as hwr_billboard_dist_scale()/spr_build() in hwr_sprite.c, using
                 * the captured (not render-interpolated) camera — close enough for
                 * a stationary calibration screenshot. Also dumps min/max/avg over
                 * ALL collected billboards so we can see the real depth range a
                 * street view actually spans, instead of guessing a reference. */
                {
                    float scrd_min = 1e30f, scrd_max = -1e30f, scrd_sum = 0.0f;
                    int scrd_n = 0;
                    for (di = 0; di < hwr_collected_count; di++) {
                        HwrBillboard *b = &hwr_collected_billboards[di];
                        float cdx = b->x - (float)snap.xc;
                        float cdy = b->y - 8.0f * (float)snap.yc;
                        float cdz = b->z - (float)snap.zc;
                        float cfb = ((float)snap.D10 * cdx + (float)snap.D14 * cdz) / 65536.0f;
                        float raw_scrd = ((float)snap.D18 * cdy + (float)snap.D1C * cfb) / 65536.0f;
                        if (raw_scrd < scrd_min) scrd_min = raw_scrd;
                        if (raw_scrd > scrd_max) scrd_max = raw_scrd;
                        scrd_sum += raw_scrd;
                        scrd_n++;
                        if (di < 40)
                            fprintf(df, " [%d]: pos=(%.0f,%.0f,%.0f) hw=%.0f hh=%.0f slot=%d shade=%d raw_scrd=%.0f\n",
                                di, b->x, b->y, b->z, b->half_size_x, b->half_size_y,
                                (int)b->sprite, (int)b->shade, raw_scrd);
                    }
                    if (scrd_n > 0)
                        fprintf(df, "raw_scrd range: min=%.0f max=%.0f avg=%.0f (n=%d)\n",
                            scrd_min, scrd_max, scrd_sum / scrd_n, scrd_n);
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

void hwr_quicklight_cache_flush(void);   /* defined below (quicklight memo) */

void hwr_sw_capture(void)
{
    /* New present: invalidate the per-frame quicklight sum memo. */
    hwr_quicklight_cache_flush();

    /* Keep the previous turn's camera so the present path can interpolate the
     * view toward the current one across the frames until the next capture. On
     * the very first capture there is no prior turn, so mirror the current one. */
    if (snap.valid)
        snap_prev = snap;

    snap.xc = engn_xc; snap.yc = engn_yc; snap.zc = engn_zc;
    snap.D10 = dword_176D10; snap.D14 = dword_176D14;
    snap.D18 = dword_176D18; snap.D1C = dword_176D1C;
    snap.D3C = dword_176D3C; snap.D40 = dword_176D40;
    snap.scale = overall_scale;
    snap.ra = render_area_a; snap.rb = render_area_b;
    snap.persp = game_perspective;
    snap.valid = 1;
    if (!snap_prev.valid)
        snap_prev = snap;   /* first turn: no motion to interpolate */

    /* Snapshot moving-Thing object state on the same tick as the camera, so the
     * vehicle faces built later (at present time) match this camera frame. */
    /* Shift the last snapshot to prev first, so dynamic-object positions can be
     * interpolated to the display rate between the two most recent turns. */
    if (obj_snap_valid) {
        memcpy(obj_snap_prev, obj_snap,
            (size_t)obj_snap_count * sizeof(obj_snap[0]));
        memcpy(snap_local_mats_prev, snap_local_mats, sizeof(snap_local_mats));
        obj_snap_prev_count = obj_snap_count;
        obj_snap_prev_valid = 1;
    }
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
            /* Debug: flag any building whose Thing Y moved since last turn. */
            if (th != NULL && th->Type == HWR_TT_BUILDING) {
                if (hover_lasty_valid && hover_lasty[o] != th->Y) {
                    hwr_dbg_hover_sub = th->SubType;
                    hwr_dbg_hover_state = th->State;
                    hwr_dbg_hover_y = th->Y >> 8;
                }
                hover_lasty[o] = th->Y;
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
                obj_snap[o].has_passengers = is_veh
                    ? (*(const int16_t *)((const char *)th + HWR_THING_PASSHEAD) != 0 ? 1 : 0)
                    : 0;
            } else {
                /* Static object (building/gate/etc.): capture the cached
                 * MapX/OffsetY/MapZ too so collapsing/animating buildings can be
                 * interpolated to the display rate the same way. */
                obj_snap[o].tx = (int32_t)(uint16_t)obj->MapX;
                obj_snap[o].ty = (int32_t)obj->OffsetY;
                obj_snap[o].tz = (int32_t)(uint16_t)obj->MapZ;
                obj_snap[o].matx = 0;
                obj_snap[o].is_dynamic = 0;
                obj_snap[o].is_vehicle = 0;
                obj_snap[o].has_passengers = 0;
            }
        }
        obj_snap_count = o;
        obj_snap_valid = 1;
        hover_lasty_valid = 1;
    }

}

/** Snapshot explosion/collapse fragments so emit_explode_faces() can
 *  interpolate them to the display rate. Shift the last capture to prev, then
 *  copy the live array into cap.
 *
 *  Deliberately separate from hwr_sw_capture(): that runs at floor-gate time,
 *  BEFORE process_explode() advances ex_faces for this turn (see game.c, the
 *  gameturn_animation_advance block calls hwrender_floor_gate() then
 *  process_explode() a few lines later). Capturing there would grab the
 *  fragments' PRE-update state as "current", making every render frame lerp
 *  against a snapshot that's already a turn stale relative to what vehicles
 *  and the camera use (their Thing/view state is refreshed by process_things()
 *  earlier in the main loop, before draw_game() runs) - fragments always end
 *  up one full turn behind, which reads as the whole cascade being locked to
 *  the 16Hz sim rate no matter how smooth everything else is. Call this AFTER
 *  process_explode() instead, so "current" is this turn's freshly-simulated
 *  state, same as the other interpolated paths. */
void hwr_sw_capture_explode(void)
{
    if (ex_faces_cap_valid) {
        memcpy(ex_faces_prev, ex_faces_cap, sizeof(ex_faces_prev));
        ex_faces_prev_valid = 1;
    }
    memcpy(ex_faces_cap, ex_faces, sizeof(ex_faces_cap));
    ex_faces_cap_valid = 1;

    /* Shrapnel advance in the sim step (process_things_bang) before draw_game(),
     * so at this point (post-advance) shrapnel[] is this turn's fresh state -
     * the same "current" the other interpolated paths capture. */
    if (shrapnel_cap_valid) {
        memcpy(shrapnel_prev, shrapnel_cap, sizeof(shrapnel_prev));
        shrapnel_prev_valid = 1;
    }
    memcpy(shrapnel_cap, shrapnel, sizeof(shrapnel_cap));
    shrapnel_cap_valid = 1;
}

int hwr_sw_camera_snapshot(int32_t *xc, int32_t *yc, int32_t *zc,
    int32_t *d10, int32_t *d14, int32_t *d18, int32_t *d1c,
    int32_t *d3c, int32_t *d40, int32_t *scale, int32_t *persp)
{
    if (!snap.valid) return 0;
    {
        /* Interpolate the view from the previous turn's camera to the current
         * one by the fraction of the turn elapsed, so scrolling/rotating/zooming
         * is smooth at the display rate even though the sim updates at 16Hz.
         * The camera is view-only, so this cannot affect gameplay/determinism. */
        float a = g_interp_alpha;
        const struct HwrCamSnap *p = &snap_prev;
        const struct HwrCamSnap *c = &snap;
        if (a < 0.0f) a = 0.0f;
        if (a > 1.0f) a = 1.0f;
        if (!p->valid) a = 1.0f;
#define HWR_LERP_I(pv, cv) ((int32_t)((pv) + (int32_t)(((cv) - (pv)) * a)))
        *xc = HWR_LERP_I(p->xc, c->xc);
        *yc = HWR_LERP_I(p->yc, c->yc);
        *zc = HWR_LERP_I(p->zc, c->zc);
        *d10 = HWR_LERP_I(p->D10, c->D10);
        *d14 = HWR_LERP_I(p->D14, c->D14);
        *d18 = HWR_LERP_I(p->D18, c->D18);
        *d1c = HWR_LERP_I(p->D1C, c->D1C);
        *d3c = HWR_LERP_I(p->D3C, c->D3C);
        *d40 = HWR_LERP_I(p->D40, c->D40);
        *scale = HWR_LERP_I(p->scale, c->scale);
#undef HWR_LERP_I
        *persp = snap.persp;
    }
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

/* --- Transparent (blended) face buffers, filled alongside sw_get_faces.
 * Faces flagged see-through are diverted here (Phase 8): the deep-radar
 * mask (whole objects the SW engine made semi-transparent) and static
 * transparent-mode faces (SW vec_mode 6 = wire fence / glass). Drawn by the
 * blended transparent pass, back-to-front sorted in sw_get_transparent_faces. */
#define HWR_TRANS_MAX_VERTS  (64 * 1024)
#define HWR_TRANS_MAX_INDEX  (96 * 1024)
static HwrVertex trans_verts[HWR_TRANS_MAX_VERTS];
static uint32_t  trans_index[HWR_TRANS_MAX_INDEX];
static uint32_t  trans_index_sorted[HWR_TRANS_MAX_INDEX];
static int       trans_vert_count = 0;
static int       trans_index_count = 0;

/* The deep-radar transparent-object bitset, populated in draw_object()
 * (libswrender) during the SW drawlist build; read here to route those
 * objects' faces into the blended pass. */
extern unsigned char hwr_obj_transp_mask[8192];
/* Objects the SW build actually drew this frame (set in draw_object). Static
 * building faces are gated on this so GL stops drawing destroyed/collapsed
 * buildings that still linger in game_objects[] but are no longer traversed. */
extern unsigned char hwr_obj_live_mask[8192];

/* Object-model ground-shadow decals captured by draw_object_model_shadow
 * (tngobjdrw.c) during the SW build - the angled silhouette shadows matrix'd
 * objects (buildings/temples, vehicles) cast on the ground. World-space quads
 * with a page-4 shadow-texture rect; emitted into the blended transparent
 * batch below. Mirrors struct HwrModelShadow (engindrwlstx.h). */
struct HwrModelShadowMirror {
    int32_t x[4], y[4], z[4];
    uint8_t u1, v1, u2, v2;
    uint16_t obj_idx;
};
extern struct HwrModelShadowMirror hwr_model_shadow_list[];
extern int hwr_model_shadow_count;


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

/* Resolve which map element supplies a grid corner's height/wave state: the
 * corner's OWN cell, always. This mirrors SW exactly - shpoint_compute_coord_y
 * (lvdraw3d.c) is called once per grid point with game_my_big_map[gz][gx] of
 * that point, with no Texture test and no substitution; wall/column cells carry
 * a perfectly valid Alt, they simply have no floor texture of their own.
 *
 * Substituting a neighbour's Alt for Texture==0 corners (as this used to do)
 * cannot be made symmetric. A grid point is shared by four tiles but indexes
 * only one cell - the tile it is the *minimum* corner of. So on a wall
 * footprint exactly one of the four corners takes the substitute and the other
 * three do not, tilting the quad along a diagonal: the "two opposite corners
 * inverted" look on high walls. Reading each point's own Alt is both SW-exact
 * and inherently shared between all tiles touching the point. */
static struct HwrMapEl *corner_mapel(int corner_gx, int corner_gz)
{
    int cgx = clampi(corner_gx, 0, HWR_MAP_TILE_WIDTH - 1);
    int cgz = clampi(corner_gz, 0, HWR_MAP_TILE_WIDTH - 1);
    return &game_my_big_map[HWR_MAP_TILE_WIDTH * cgz + cgx];
}

static int16_t corner_alt(int corner_gx, int corner_gz)
{
    return corner_mapel(corner_gx, corner_gz)->Alt;
}

/* Per-vertex water wobble, mirroring shpoint_compute_coord_y's Flags&0x10
 * branch (lvdraw3d.c) exactly, mag=8 to match the floor tile scale (8*Alt).
 * elcr_x/elcr_z are world units (tile<<8), same basis as the floor verts. */
/* Raw water wobble at a world point (shpoint_compute_coord_y's Flags&0x10 branch,
 * lvdraw3d.c). Drives BOTH the vertical displacement (mag*wobble) and the SW
 * water "shine" (ReflShade = (wobble+32)<<9, folded into the vertex shade - the
 * smooth brightness blobs that drift across the surface). */
static float water_wobble_sum(uint32_t turn, int elcr_x, int elcr_z, int dvfactor)
{
    return (float)(waft_table2[(turn + (uint32_t)(elcr_x >> 7)) & 0x1F]
                 + waft_table2[(turn + (uint32_t)(elcr_z >> 7)) & 0x1F]
                 + waft_table2[(32 * turn / (uint32_t)dvfactor) & 0x1F]);
}

/* SW advances this once per 16Hz turn, so the waves (and the shine riding on
 * them) visibly stepped while the screen runs at 60fps+. Interpolate between the
 * previous and current turn by g_interp_alpha - the same treatment the wobbly-
 * terrain camera waft already gets in sw_get_camera. Returns float so the
 * displacement stays smooth instead of quantising to whole wobble units.
 * The third table term advances only every ~4-8 turns (32/dvfactor per turn), so
 * it is usually identical between the two turns and contributes nothing to the
 * lerp; the first two terms are what actually animate. */
static float water_wobble_raw(int elcr_x, int elcr_z)
{
    uint32_t turn = render_anim_turn;
    int dvfactor = 140 + ((bw_rotl32(0x5D3BA6C3, (uint8_t)(elcr_z >> 8)) ^
                            bw_rotr32(0xA7B4D8AC, (uint8_t)(elcr_x >> 8))) & 0x7F);
    float sc = water_wobble_sum(turn,          elcr_x, elcr_z, dvfactor);
    float sp = water_wobble_sum(turn - 1u,     elcr_x, elcr_z, dvfactor);
    return (sp + (sc - sp) * g_interp_alpha) / 8.0f;   /* SW's >> 3 */
}

static float water_wobble_y(int elcr_x, int elcr_z)
{
    return 8.0f * water_wobble_raw(elcr_x, elcr_z);   /* mag = 8, matches 8*Alt */
}

/* Y offset to add on top of 8*Alt for a grid corner, replicating
 * shpoint_compute_coord_y's Flags branches. Uses the CORNER's own map element
 * (with corner_mapel's neighbour substitution for column/wall cells), since SW
 * evaluates each grid point against its own map element - and, like the Alt,
 * it must not depend on which tile is asking or adjacent tiles would wobble
 * their shared corner by different amounts. */
static float corner_wave_y(int corner_gx, int corner_gz, int wx, int wz)
{
    struct HwrMapEl *cme = corner_mapel(corner_gx, corner_gz);
    if (cme->Flags & 0x10)
        return water_wobble_y(wx, wz);
    if (cme->Flags & 0x40) {
        /* Interpolated between turns for the same reason as the water wobble. */
        float wc = (float)waft_table[render_anim_turn & 0x1F];
        float wp = (float)waft_table[(render_anim_turn - 1u) & 0x1F];
        return wp + (wc - wp) * g_interp_alpha;
    }
    return 0.0f;
}

/* Geometric ambient occlusion for a floor corner. The grid corner (cgx,cgz) is
 * shared by up to 4 cells; each one that is a building/column footprint
 * (Texture==0) is a vertical occluder. Returns an "openness" byte (255 = fully
 * open, lower = more occluded) baked into the vertex so the shader darkens the
 * ambient fill at the base of walls and in corners. */
__attribute__((unused))   /* superseded by the SW-exact corner_baked_shade */
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

/* --- Baked SW floor shade (default shadow system) -----------------------------
 * The SW engine bakes each level's floor lighting - including building shadows -
 * into the per-tile Ambient field. We render that directly (smoothed) so the GL
 * floor reproduces the software look, instead of casting a dynamic sun shadow.
 * Normalised against the level's mean Ambient so open ground reads ~full bright
 * and baked-dark tiles fall off; box-blurred over a few tiles to remove the
 * per-tile blockiness. STRENGTH deepens the contrast; both are ini-tunable via
 * the ao slider (uAO) which scales the whole term in the shader. */
static float    sn_amb_mean = 128.0f;   /* mean floor Ambient for normalisation */
static uint32_t sn_amb_sig  = 0;
static int      sn_amb_done = 0;
static uint32_t sn_map_signature(void);   /* fwd (defined with the sun hint below) */

/* Per-level blurred Ambient field. A small per-corner window (the old approach)
 * only spans ~1 tile, so sharp shadow edges stayed blocky. Instead we blur the
 * whole Ambient map once per level with a wide multi-pass box blur (separable,
 * a few iterations ~ Gaussian) into this buffer, then just sample it per corner.
 * Cheap (once per level) and genuinely smooth. */
static float    sn_amb_blur[HWR_MAP_TILE_WIDTH * HWR_MAP_TILE_WIDTH];
static float    sn_amb_tmp [HWR_MAP_TILE_WIDTH * HWR_MAP_TILE_WIDTH];

/* Tunables for the baked-shade look (exposed as constants for now). */
#define SN_BAKE_BLUR_R    1      /* box-blur radius in tiles (gentle edge smoothing) */
#define SN_BAKE_BLUR_ITER 2      /* blur passes (more = smoother, softer) */
#define SN_BAKE_STRENGTH  1.6f   /* >1 deepens shadows below the mean */
#define SN_BAKE_MINBRIGHT 0.10f  /* deepest shadow (never pure black) */

/* One separable box-blur pass (horizontal then vertical) with a running sum. */
static void sn_box_blur_pass(float *buf, float *tmp, int R)
{
    const int W = HWR_MAP_TILE_WIDTH;
    int x, y, i;
    float inv = 1.0f / (float)(2 * R + 1);
    /* Horizontal: buf -> tmp */
    for (y = 0; y < W; y++) {
        float *row = &buf[y * W];
        float *orow = &tmp[y * W];
        double run = 0.0;
        for (i = -R; i <= R; i++) run += row[clampi(i, 0, W - 1)];
        for (x = 0; x < W; x++) {
            orow[x] = (float)(run * inv);
            run -= row[clampi(x - R, 0, W - 1)];
            run += row[clampi(x + R + 1, 0, W - 1)];
        }
    }
    /* Vertical: tmp -> buf */
    for (x = 0; x < W; x++) {
        double run = 0.0;
        for (i = -R; i <= R; i++) run += tmp[clampi(i, 0, W - 1) * W + x];
        for (y = 0; y < W; y++) {
            buf[y * W + x] = (float)(run * inv);
            run -= tmp[clampi(y - R, 0, W - 1) * W + x];
            run += tmp[clampi(y + R + 1, 0, W - 1) * W + x];
        }
    }
}

static void sn_update_amb_mean(void)
{
    uint32_t sig = sn_map_signature();
    const int W = HWR_MAP_TILE_WIDTH;
    double sum = 0.0; long n = 0; int i, tot = W * W, pass;
    if (sn_amb_done && sig == sn_amb_sig)
        return;
    sn_amb_sig = sig; sn_amb_done = 1;
    if (game_my_big_map == NULL) {
        sn_amb_mean = 128.0f;
        for (i = 0; i < tot; i++) sn_amb_blur[i] = 128.0f;
        return;
    }
    /* Seed the blur buffer with raw Ambient and accumulate the mean. The rare
     * non-floor cells (Texture==0) have no ambient; seed them with the running
     * mean estimate so they don't punch dark holes into the blur. */
    for (i = 0; i < tot; i++) {
        struct HwrMapEl *m = &game_my_big_map[i];
        if (m->Texture != 0) { sn_amb_blur[i] = (float)m->Ambient; sum += (double)m->Ambient; n++; }
        else                 { sn_amb_blur[i] = -1.0f; }
    }
    sn_amb_mean = n ? (float)(sum / (double)n) : 128.0f;
    if (sn_amb_mean < 1.0f) sn_amb_mean = 1.0f;
    for (i = 0; i < tot; i++)
        if (sn_amb_blur[i] < 0.0f) sn_amb_blur[i] = sn_amb_mean;
    /* Wide multi-pass blur. */
    for (pass = 0; pass < SN_BAKE_BLUR_ITER; pass++)
        sn_box_blur_pass(sn_amb_blur, sn_amb_tmp, SN_BAKE_BLUR_R);
}

/* Per-vertex face shade, replicating the software renderer's matcap lighting
 * (compute_normals_light_ratio in engindrwlstm_wrp.c). SW rotates each vertex's
 * object-space normal (game_normals[Shade0..3]) into world space, then projects
 * it through the SAME camera-rotation factors used to project points (D10/D14 =
 * XZ rotation, D18/D1C = pitch/depth). The projected normal's screen-space X/Y
 * pick a texel in a pre-lit "shading sphere" (matcap). We approximate that matcap
 * with a directional light in that same screen space: faces whose normal points
 * toward SN_FACE_L* (screen up-left, matching SW's baked light) are bright, the
 * far side shaded. Camera-relative, so buildings re-shade as the view rotates —
 * exactly how SW behaves. FLOOR keeps the dark side from going black. */
/* Per-vertex face brightness, replicating the software renderer's face shading
 * exactly (draw_object_face4d_textrd, engindrwlstx_fac.c):
 *   mode 2 faces:  fixed S = 0x200000            -> shade index 32 = identity
 *   other modes:   S = (Shade0<<7 + lights) << 7 -> shade index Shade0>>2,
 *                  where index 32 = identity (texture as-is), 63 = ~2x bright.
 * So Shade0..3 ARE the baked per-vertex brightnesses with 128 = identity.
 * Map to a GL light byte with 255 = identity: light = Shade * 255/128 = ~Shade*2.
 * SW's ushort wrap makes negative Shade values overbright -> clamp to 255.
 * (Dynamic quicklights from Light0..3 are covered by GL's own light pass.) */
/* SW-EXACT static face vertex shade (draw_object_face4d_textrd/face3d):
 *   shd = Shade<<7 + SUM(quicklights via Light chain), idx = shd>>9,
 * identity 32, overbright to 63. Same unified byte scale as the floor
 * (128 = identity), so the faces pass reconstructs the level as vAO*2 and
 * static lamps light walls from the SW data - not the GL radial pools. */
static int sw_quicklight_sum(uint16_t light_first);   /* fwd (defined below) */
static uint8_t sw_shade_to_byte(int shd);             /* fwd (defined below) */

static uint8_t sw_face_shade(int16_t s, uint16_t light_first)
{
    int shd;
    if (s < 0)                          /* SW ushort wrap -> clamped overbright */
        return 252;
    shd = ((int)s << 7) + sw_quicklight_sum(light_first);
    return sw_shade_to_byte(shd);
}

static void hwr_world_normal(const HwrM33 *m, int nx, int ny, int nz, float out[3]); /* fwd */

/* Face shade for ROTATED (dynamic) objects - vehicles, turrets, rotors.
 * SW draws these through draw_object_face4g_textrd, where Shade0..3 are indices
 * into game_normals[] and the brightness is recomputed each frame from the
 * rotated normal (compute_normals_light_ratio): the normal is rotated by the
 * object matrix, projected through the camera factors, and its view-depth
 * component becomes the shade. Replicated here in float with a unit normal:
 * s in [-1..1], negative clamped to 0 exactly as SW does. */
static uint8_t sw_face_shade_dynamic(int16_t nidx, const HwrM33 *m)
{
    float n[3], fp, s;
    int v;
    if (nidx <= 0 || nidx >= (int16_t)next_normal || game_normals == NULL)
        return 200;                              /* no normal -> neutral */
    {
        struct HwrNormal *nn = &game_normals[nidx];
        hwr_world_normal(m, nn->NX, nn->NY, nn->NZ, n);   /* unit, world space */
    }
    fp = ((float)snap.D10 * n[0] + (float)snap.D14 * n[2]) / 65536.0f;
    s  = ((float)snap.D18 * n[1] + (float)snap.D1C * fp)   / 65536.0f;
    if (s < 0.0f) s = -s;   /* mesh winding is inconsistent; use magnitude */
    if (s > 1.0f) s = 1.0f;
    /* Unified scale: 128 = identity (the faces pass reconstructs level as
     * vAO*2), so a fully camera-facing normal (s=1) = identity brightness. */
    v = (int)(s * 128.0f + 0.5f);
    if (v < 12) v = 12;     /* keep grazing faces from pure black */
    return (uint8_t)v;
}

/* Floor-corner brightness, replicating the software renderer exactly
 * (shpoint_compute_shade, lvdraw3d.c): each corner point's shade starts from the
 * corner cell's baked Ambient, where shade index 32 = identity (texture as-is)
 * -> Ambient 128 = identity. Dynamic lamp light is added by the GL light pass
 * (SW adds sqlight/quicklights the same way). Gouraud interpolation across the
 * tile comes free from the per-corner vertices - same smoothing as SW. */
/* SW-EXACT floor lighting for a grid corner, replicating shpoint_compute_shade
 * (lvdraw3d.c) with the same data the software renderer uses:
 *   shd = (Ambient << 7) + 256 + SUM(FullLight.Intensity * QuickLight.Ratio)
 *   shade index = shd >> 9, identity 32, overbright to 63.
 * The per-tile QuickLight list (mapel->Shade -> game_quick_lights) is the
 * engine's precomputed static lighting: every map lamp's pool AND the
 * negative-intensity anti-lights (building shadows) with per-corner falloff
 * ratios. So this one value IS the complete static SW floor light - lamps,
 * shadows, ambient - at SW's own numbers. (ReflShade and the transient person
 * super_quick_light term are omitted: minor, and dynamic GL lights cover the
 * latter.) Returned as a light byte with 128 = identity (idx*4), so the shader
 * reconstructs the level as vAO*2 (up to ~2x overbright at 252). */
/* SUM(FullLight.Intensity * QuickLight.Ratio) over a quicklight chain - the
 * engine's precomputed static lighting term (cummulate_shade_from_quick_lights):
 * every map lamp's pool and the negative-intensity anti-light shadows, with
 * per-point falloff ratios. Shared by the floor corners (mapel->Shade chain)
 * and the object faces (face->Light0..3 chains). */
/* Memoised per frame: the sums are per chain HEAD, and the chains only change
 * per game turn, but this gets called for every face vertex and floor corner
 * every present (tens of thousands of walks of randomly-scattered nodes) -
 * uncached it dropped the frame rate to turn rate. */
static int32_t  ql_sum_cache[65536];
static uint32_t ql_sum_stamp[65536];
static uint32_t ql_sum_frame = 0;

void hwr_quicklight_cache_flush(void)
{
    ql_sum_frame++;    /* invalidates every stamp lazily */
}

static int sw_quicklight_sum(uint16_t light_first)
{
    int shd = 0, i;
    uint16_t light;
    if (light_first == 0 || game_quick_lights == NULL || game_full_lights == NULL)
        return 0;
    if (ql_sum_stamp[light_first] == ql_sum_frame)
        return ql_sum_cache[light_first];
    for (light = light_first, i = 0;
         light != 0 && light < next_quick_light && i <= 100; i++) {
        struct HwrQuickLight *ql = &game_quick_lights[light];
        if (ql->Light < next_full_light)
            shd += (int)game_full_lights[ql->Light].Intensity * (int)ql->Ratio;
        light = ql->NextQuick;
    }
    ql_sum_cache[light_first] = shd;
    ql_sum_stamp[light_first] = ql_sum_frame;
    return shd;
}

/* SW shade word -> light byte on the unified scale: index = shd>>9 (identity
 * 32, overbright caps at 63 ~= 2x), byte = index*4 so 128 = identity. */
static uint8_t sw_shade_to_byte(int shd)
{
    int v;
    if (shd < 0)      shd = 0;
    if (shd > 0x7E00) shd = 0x7F00;   /* SW clamp */
    v = (shd >> 9) * 4;
    if (v > 255) v = 255;
    if (v < 4)   v = 4;               /* never pure black */
    return (uint8_t)v;
}

static uint8_t corner_baked_shade(int cgx, int cgz)
{
    const int W = HWR_MAP_TILE_WIDTH;
    struct HwrMapEl *m =
        &game_my_big_map[W * clampi(cgz, 0, W-1) + clampi(cgx, 0, W-1)];
    /* Water (Flags&0x10): SW computes the shade LIVE every turn because the
     * wobble-driven ReflShade term changes with gameturn - it never uses the
     * static baked ShadeR. Reproduce shpoint_compute_shade including ReflShade =
     * (wobble+32)<<9: a per-corner brightness that follows the wave, Gouraud-
     * interpolated into the smooth "shine" blobs that drift across the water.
     * water_shine_strength scales the moving component (1.0 = SW-exact); enable=0
     * drops the term so water is flat. */
    if (m->Flags & 0x10) {
        HwrLightDefaults wd = hwr_lights_defaults();
        int refl = 0;
        if (wd.water_shine_enable) {
            float wob = water_wobble_raw(cgx << 8, cgz << 8);
            refl = ((int)(wob * wd.water_shine_strength) + 32) << 9;
        }
        return sw_shade_to_byte(((int)m->Ambient << 7) + refl + 256
                                + sw_quicklight_sum(m->Shade));
    }
    /* Prefer the map's baked ShadeR (the complete static shade the level was
     * authored with, shade-index scale, 32 = identity): under FX3D the SW floor
     * pass that would recompute it is skipped, so it survives from load - and
     * it carries the properly SHAPED (diagonal, sun-angled) building shadows
     * that the runtime Ambient+quicklight reconstruction was rendering as
     * axis-aligned squares. Cells without a baked value (never floor-shaded,
     * e.g. column cells) fall back to the runtime formula. */
    if (m->ShadeR != 0) {
        int v = (int)m->ShadeR * 4;
        if (v > 255) v = 255;
        return (uint8_t)v;
    }
    return sw_shade_to_byte(((int)m->Ambient << 7) + 256
                            + sw_quicklight_sum(m->Shade));
}

/* For column/building cells (Texture==0), find the nearest valid floor tile
 * (8-connected) and copy its texture index and ShadeR. Returns 1 on success.
 * Column cells never have ShadeR set by the SW renderer, so inheriting prevents
 * them rendering as pitch-black even when geometry is correct. */
static int nearest_floor_neighbour(int gx, int gz, int *out_texidx, uint8_t *out_shade,
    uint8_t *out_flags, uint16_t *out_ambient)
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
            *out_flags  = nm->Flags;
            *out_ambient = nm->Ambient;
            return 1;
        }
    }
    return 0;
}

/* ---- Auto-derived sun azimuth from the level's baked floor lighting --------
 * SW stores no sun angle; each level's directional look is baked into the
 * per-tile ambient brightness. A building footprint (Texture==0) leaves the
 * floor tile on its anti-sun side darker than the level average, so the darker
 * neighbours of buildings reveal the shadow direction. We compare each building
 * cell's floor neighbours against the mean floor ambient, accumulate a net
 * "toward the shadow" vector, and take the sun as its opposite. Cached per level
 * (keyed on a cheap layout signature) and logged once so the result is visible.
 * Returns azimuth in degrees, or a negative sentinel when the baked data carries
 * no usable directional signal (flat/symmetric ambient) -> caller keeps the ini
 * azimuth. */
static uint32_t sn_hint_sig  = 0;
static int      sn_hint_done = 0;
static HwrSunHint sn_hint;                  /* last computed result + diagnostics */

static uint32_t sn_map_signature(void)
{
    /* FNV-1a over a sparse Alt sample. Alt is static per level, unlike Texture
     * (whose animation flag bits toggle per frame) or Shade (live lighting), so
     * the scan recomputes once per level instead of every frame. */
    uint32_t sig = 2166136261u;
    int i, n = HWR_MAP_TILE_WIDTH * HWR_MAP_TILE_WIDTH;
    if (game_my_big_map == NULL)
        return 0;
    for (i = 0; i < n; i += 37)
        sig = (sig ^ (uint32_t)(uint16_t)game_my_big_map[i].Alt) * 16777619u;
    return sig;
}

/* Scan one baked floor field (0 = Ambient, 1 = Shade) for a directional sun
 * signal. Buildings here are objects/columns, not floor-texture holes, so we do
 * not look for footprints: instead we take the darkness-weighted brightness
 * gradient over every floor tile. A tile sitting in a baked shadow is darker
 * than its lit neighbours, so its local gradient points toward the lit (sun)
 * side; weighting each tile's gradient by how far below the mean it sits and
 * summing gives a net "toward the sun" vector. Fills *az (deg, <0 if none),
 * *coh (0..1 agreement) and *std (field spread; ~0 means a flat field with no
 * baked shading at all). */
static void sn_scan_field(int use_shade,
    double *az, double *coh, double *std, long *nfloor_out)
{
    const int W = HWR_MAP_TILE_WIDTH;
    double sum = 0.0, sum2 = 0.0, mean, variance;
    double sx = 0.0, sz = 0.0, wacc = 0.0, mag;
    long   n = 0;
    int gx, gz;

    *az = -1.0; *coh = 0.0; *std = 0.0;

    for (gz = 0; gz < W; gz++)
        for (gx = 0; gx < W; gx++) {
            struct HwrMapEl *me = &game_my_big_map[W*gz+gx];
            double v;
            if (me->Texture == 0) continue;         /* skip the rare non-floor cells */
            v = use_shade ? (double)me->Shade : (double)me->Ambient;
            sum += v; sum2 += v*v; n++;
        }
    if (nfloor_out) *nfloor_out = n;
    if (n < 64) return;
    mean = sum / (double)n;
    variance = sum2/(double)n - mean*mean;
    *std = variance > 0.0 ? sqrt(variance) : 0.0;

    /* Local high-pass: weight each tile by how far it sits BELOW its own local
     * neighbourhood mean (a real cast shadow is a local dip), not the global
     * mean - that removes broad surface-type brightness (roads vs pavement) and
     * lets the directional shadow signal survive. R sets the neighbourhood. */
    #define FLD(m) (use_shade ? (double)(m)->Shade : (double)(m)->Ambient)
    {
        const int R = 3;
        for (gz = 1; gz < W-1; gz++)
            for (gx = 1; gx < W-1; gx++) {
                struct HwrMapEl *me = &game_my_big_map[W*gz+gx];
                struct HwrMapEl *l = &game_my_big_map[W*gz+(gx-1)];
                struct HwrMapEl *r = &game_my_big_map[W*gz+(gx+1)];
                struct HwrMapEl *u = &game_my_big_map[W*(gz-1)+gx];
                struct HwrMapEl *dn= &game_my_big_map[W*(gz+1)+gx];
                double fc, ggx, ggz, w, lm; long lc;
                int wx, wz;
                if (me->Texture==0 || l->Texture==0 || r->Texture==0 ||
                    u->Texture==0 || dn->Texture==0)
                    continue;                        /* need 4 floor neighbours */
                fc = FLD(me);
                /* Local neighbourhood mean (floor tiles only). */
                lm = 0.0; lc = 0;
                for (wz = -R; wz <= R; wz++)
                    for (wx = -R; wx <= R; wx++) {
                        int nx = gx+wx, nz = gz+wz;
                        struct HwrMapEl *nm;
                        if (nx<0||nx>=W||nz<0||nz>=W) continue;
                        nm = &game_my_big_map[W*nz+nx];
                        if (nm->Texture==0) continue;
                        lm += FLD(nm); lc++;
                    }
                if (lc < 4) continue;
                lm /= (double)lc;
                w = lm - fc;                          /* >0 => locally darker (shadow) */
                if (w <= 0.0) continue;              /* only local dips carry direction */
                ggx = FLD(r) - FLD(l);               /* toward brighter (+x) */
                ggz = FLD(dn) - FLD(u);              /* toward brighter (+z) */
                sx += w * ggx;
                sz += w * ggz;
                wacc += w * (fabs(ggx) + fabs(ggz));
            }
    }
    #undef FLD

    mag = sqrt(sx*sx + sz*sz);
    *coh = (wacc > 1e-6) ? mag / wacc : 0.0;
    if (mag < 1e-6) return;
    {
        /* (sx,sz) points toward the sun; azimuth in hwr_sun's convention
         * (dx=sin(az), dz=cos(az)). */
        double a = atan2(sx, sz) * 180.0 / 3.14159265358979;
        if (a < 0.0) a += 360.0;
        *az = a;
    }
}

static void sn_derive_sun_azimuth(void)
{
    double a_az, a_coh, a_std, s_az, s_coh, s_std;
    long   nfloor = 0;

    memset(&sn_hint, 0, sizeof(sn_hint));
    sn_hint.azimuth = -1.0f;
    if (game_my_big_map == NULL) {
        sn_hint.map_null = 1;
        return;
    }

    sn_scan_field(0, &a_az, &a_coh, &a_std, &nfloor);  /* Ambient */
    sn_scan_field(1, &s_az, &s_coh, &s_std, NULL);     /* Shade   */

    sn_hint.nfloor = nfloor;
    sn_hint.amb_az = (float)a_az; sn_hint.amb_coh = (float)a_coh; sn_hint.amb_std = (float)a_std;
    sn_hint.shd_az = (float)s_az; sn_hint.shd_coh = (float)s_coh; sn_hint.shd_std = (float)s_std;

    /* Pick the field that has real spread AND a coherent direction; prefer the
     * more coherent one. Thresholds are deliberately loose - the per-field
     * diagnostics are logged so they can be tightened once we see real levels. */
    {
        /* Only Ambient is baked; Shade is the live quick-light mask (ignored).
         * Coherence stays low because most tiles are open ground with no shadow,
         * so we accept a weak-but-consistent direction and validate visually. */
        (void)s_az; (void)s_coh; (void)s_std;
        if (a_std >= 2.0 && a_coh >= 0.03 && a_az >= 0.0) {
            sn_hint.field = 1;
            sn_hint.azimuth = (float)a_az;
            sn_hint.coherence = (float)a_coh;
        }
    }
}

void hwr_sw_sun_hint(HwrSunHint *out)
{
    uint32_t sig = sn_map_signature();
    int recomputed = 0;
    if (!sn_hint_done || sig != sn_hint_sig) {
        sn_hint_sig  = sig;
        sn_hint_done = 1;
        sn_derive_sun_azimuth();
        recomputed = 1;         /* map (level) changed this call */
    }
    if (out != NULL) {
        *out = sn_hint;
        out->fresh = recomputed;
    }
}

static int sw_get_camera(void *ctx, HwrCamera *out)
{
    /* Hand the shader the raw projection factors so it can reproduce
     * transform_shpoint() exactly, including the mode-5 perspective. */
    (void)ctx;
    if (out == NULL || !snap.valid || snap.D3C == 0 || snap.D40 == 0)
        return -1;
    {
        /* Interpolate the view between the previous turn's camera and the current
         * one by the fraction of the turn elapsed (g_interp_alpha), so the whole
         * scene scrolls/rotates/zooms smoothly at the display rate while the sim
         * stays at 16Hz. The shader transforms world-space floor/face/sprite
         * geometry by this camera, so smoothing it here smooths everything. The
         * camera is view-only: no gameplay/determinism impact. */
        float a = g_interp_alpha;
        const struct HwrCamSnap *p = snap_prev.valid ? &snap_prev : &snap;
        const struct HwrCamSnap *c = &snap;
        if (a < 0.0f) a = 0.0f;
        if (a > 1.0f) a = 1.0f;
#define HWR_LERP_F(pv, cv) ((float)(pv) + ((float)(cv) - (float)(pv)) * a)
        out->d10 = HWR_LERP_F(p->D10, c->D10); out->d14 = HWR_LERP_F(p->D14, c->D14);
        out->d18 = HWR_LERP_F(p->D18, c->D18); out->d1c = HWR_LERP_F(p->D1C, c->D1C);
        out->scale    = HWR_LERP_F(p->scale, c->scale);
        out->centre_x = HWR_LERP_F(p->D3C, c->D3C);
        out->centre_y = HWR_LERP_F(p->D40, c->D40);
        out->cx  = HWR_LERP_F(p->xc, c->xc);
        out->cy8 = 8.0f * HWR_LERP_F(p->yc, c->yc);
        out->cz  = HWR_LERP_F(p->zc, c->zc);
#undef HWR_LERP_F
        /* Wobbly-terrain levels: the SW engine bobs the whole scene by
         * waft_table[render_anim_turn&0x1F]>>3 in world Y. Reproduce it here as a
         * camera Y shift (moves floor+faces+sprites together) and interpolate it
         * between turns so it's smooth. Scale is tunable. */
        if (render_floor_flags & HWR_RENDFLR_WOBBLY) {
            int ic = (int)(render_anim_turn & 0x1F);
            int ip = (int)((render_anim_turn - 1) & 0x1F);
            float wc = (float)(waft_table[ic] >> 3);
            float wp = (float)(waft_table[ip] >> 3);
            float wob = wp + (wc - wp) * a;
            out->cy8 -= 8.0f * wob;
        }
    }
    out->perspective = snap.persp;
    out->view_w = sw_view_w;
    out->view_h = sw_view_h;
    /* World-space half-extent of the visible floor (sw_get_floor builds tiles out
     * to render_area+2 around the camera). The sun shadow map uses this to size
     * its ortho frustum to the actual zoom instead of a fixed 32-tile box, so
     * shadows cover the whole view and stay proportionally sharp. */
    {
        int r = (snap.ra > snap.rb ? snap.ra : snap.rb);
        r = (r ? r + 2 : 24);
        out->world_half = (float)(r << 8);   /* tiles -> world units (tile<<8) */
    }
    return 0;
}

static int sw_get_floor(void *ctx, HwrGeometryBatch *out)
{
    int gx, gz, x0, x1, z0, z1;
    (void)ctx;
    floor_vert_count = 0;
    floor_index_count = 0;
    if (out != NULL) {
        out->verts = NULL; out->vert_count = 0;
        out->indices = NULL; out->index_count = 0;
    }
    if (game_my_big_map == NULL || game_textures == NULL || out == NULL || !snap.valid)
        return 0;

    sn_update_amb_mean();   /* refresh the baked-shade normalisation for this level */

    hwr_floor_window_bounds(&x0, &x1, &z0, &z1);
    x0 = clampi(x0, 0, HWR_MAP_TILE_WIDTH - 2);
    x1 = clampi(x1, 0, HWR_MAP_TILE_WIDTH - 2);
    z0 = clampi(z0, 0, HWR_MAP_TILE_WIDTH - 2);
    z1 = clampi(z1, 0, HWR_MAP_TILE_WIDTH - 2);

    for (gz = z0; gz <= z1; gz++) {
        for (gx = x0; gx <= x1; gx++) {
            struct HwrMapEl *me = &game_my_big_map[HWR_MAP_TILE_WIDTH * gz + gx];
            struct HwrFloorTex *tx;
            int texidx = me->Texture & 0x3FFF;   /* low bits = index, high = flags */
            uint8_t inherited_shade = me->ShadeR;
            uint8_t tile_flags = me->Flags;
            uint16_t tile_ambient = me->Ambient;
            HwrVertex *v;
            int base;
            int flat_colour;

            if (texidx >= game_textures_limit)
                continue;
            /* SW-exact "no floor surface here" skip (lvdraw3d.c lvdraw_do_floor:
             * (game_perspective != 2) && (Flags & 0x80); game_perspective is 5
             * in normal play, so this always applies). The map marks cells where
             * a wall stands rather than ground. Emitting them meant a tile whose
             * four grid corners straddle the top and bottom of a ledge became a
             * big diagonal ramp wearing a borrowed wall texture - the wall
             * texture smeared across the floor instead of a 90-degree drop.
             * Only two of a ledge corner's four orientations showed it, because
             * a grid point indexes the cell it is the minimum corner of. */
            if (me->Flags & 0x80)
                continue;
            if (floor_vert_count + 4 > HWR_FLOOR_MAX_TILES * 4)
                break;

            /* Cells with Texture==0 have no ground surface - a wall or ledge
             * stands there. Their four grid corners straddle the top and bottom
             * of the drop, so the quad is a steep ramp. SW has that ramp too (a
             * continuous height field; alt_at_point interpolates the same slope)
             * but never lets you see it: the tile is pushed to the back of the
             * painter's-order bucket (lvdraw3d.c, dpthalt 2500/3500 + BUCKET_MID)
             * so the wall FACES paint over it. A z-buffer has no equivalent - the
             * ramp intersects the wall geometrically and pokes out, chamfering
             * ledge corners that should be square. So by default don't emit it at
             * all; the wall faces already cover the ground there.
             * [floor] no_surface_tiles in fx3d_lights.ini switches behaviour:
             *   0 = skip (default), 1 = SW's flat colour_grey2 polygon,
             *   2 = nearest floor neighbour's texture (the old behaviour). */
            flat_colour = 0;
            if (me->Texture == 0) {
                int mode = hwr_lights_defaults().floor_no_surface_mode;
                if (mode == 0)
                    continue;
                if (mode == 1) {
                    flat_colour = 1;
                } else {
                    int ni;
                    if (!nearest_floor_neighbour(gx, gz, &ni, &inherited_shade,
                            &tile_flags, &tile_ambient))
                        continue;
                    texidx = ni;
                }
            }
            (void)tile_ambient;   /* Ambient now read per corner in corner_baked_shade */
            tx = &game_textures[texidx];

            base = floor_vert_count;
            v = &floor_verts[base];
            /* Corner positions: v[0]=(gx,gz), v[1]=(gx+1,gz),
             *                   v[2]=(gx+1,gz+1), v[3]=(gx,gz+1) */
            v[0].x = (float)(gx << 8);     v[0].z = (float)(gz << 8);
            v[1].x = (float)((gx+1) << 8); v[1].z = (float)(gz << 8);
            v[2].x = (float)((gx+1) << 8); v[2].z = (float)((gz+1) << 8);
            v[3].x = (float)(gx << 8);     v[3].z = (float)((gz+1) << 8);
            v[0].y = (float)(8 * corner_alt(gx,   gz)
                + corner_wave_y(gx,   gz,   (int)v[0].x, (int)v[0].z));
            v[1].y = (float)(8 * corner_alt(gx+1, gz)
                + corner_wave_y(gx+1, gz,   (int)v[1].x, (int)v[1].z));
            v[2].y = (float)(8 * corner_alt(gx+1, gz+1)
                + corner_wave_y(gx+1, gz+1, (int)v[2].x, (int)v[2].z));
            v[3].y = (float)(8 * corner_alt(gx,   gz+1)
                + corner_wave_y(gx,   gz+1, (int)v[3].x, (int)v[3].z));
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
            /* Water (Flags&0x10): switch to continuous, world-derived UV so the
             * texture tiles seamlessly across tile boundaries instead of each
             * tile independently clamping its own sub-rect (the visible "seams").
             * Collapse all 4 corners to the sub-rect origin (min texel) and pass
             * the span in uv_w/uv_h; the floor vertex shader rebuilds the UV from
             * fract(worldXZ/256). Non-water tiles keep uv_w/uv_h = 0 and the exact
             * per-corner mapping above. The non-zero span also flags the specular
             * shine in the fragment shader. */
            if (tile_flags & 0x10) {
                uint8_t ux0 = tx->TMapX1, ux1 = tx->TMapX1;
                uint8_t uy0 = tx->TMapY1, uy1 = tx->TMapY1;
                uint8_t xs[4], ys[4];
                int k;
                xs[0] = tx->TMapX1; xs[1] = tx->TMapX2; xs[2] = tx->TMapX3; xs[3] = tx->TMapX4;
                ys[0] = tx->TMapY1; ys[1] = tx->TMapY2; ys[2] = tx->TMapY3; ys[3] = tx->TMapY4;
                for (k = 0; k < 4; k++) {
                    if (xs[k] < ux0) ux0 = xs[k];
                    if (xs[k] > ux1) ux1 = xs[k];
                    if (ys[k] < uy0) uy0 = ys[k];
                    if (ys[k] > uy1) uy1 = ys[k];
                }
                v[0].u = v[1].u = v[2].u = v[3].u = ux0;
                v[0].v = v[1].v = v[2].v = v[3].v = uy0;
                v[0].uv_w = v[1].uv_w = v[2].uv_w = v[3].uv_w = (uint8_t)(ux1 - ux0);
                v[0].uv_h = v[1].uv_h = v[2].uv_h = v[3].uv_h = (uint8_t)(uy1 - uy0);
            } else {
                v[0].uv_w = v[1].uv_w = v[2].uv_w = v[3].uv_w = 0;
                v[0].uv_h = v[1].uv_h = v[2].uv_h = v[3].uv_h = 0;
            }
            /* SW mode-04 flat colour: sentinel page carries the palette index in
             * u, and the floor shader still runs the full lighting path on it. */
            if (flat_colour) {
                int k;
                for (k = 0; k < 4; k++) {
                    v[k].page  = HWR_FLATCOL_PAGE;
                    v[k].u     = colour_grey2;
                    v[k].v     = 0;
                    v[k].uv_w  = 0;
                    v[k].uv_h  = 0;
                }
            }
            /* Per-corner geometric AO: darkens the ambient fill where the floor
             * meets buildings/columns. Replaces the SW per-tile ShadeR (which is
             * near-flat on open ground and reads as no occlusion). */
            (void)inherited_shade;
            /* Baked SW floor shade (smoothed): reproduces the software floor's
             * lighting/building shadows. Replaces the geometric corner AO, which
             * did almost nothing here (buildings are objects, not Texture==0
             * floor cells). The GL sun shadow map is the alternative (sun_enable). */
            v[0].light = corner_baked_shade(gx,     gz);
            v[1].light = corner_baked_shade(gx + 1, gz);
            v[2].light = corner_baked_shade(gx + 1, gz + 1);
            v[3].light = corner_baked_shade(gx,     gz + 1);
            /* Mirrors lvdraw3d.c's floor mode pick: Flags&0x20 selects glass
             * mode 21 (unshaded, like object window glass) over the normal
             * dynamically-shaded mode 5; Flags&0x01 separately forces the SW
             * tile to max brightness (Shade=0x3F00) regardless of mode.
             * Ambient (shpoint_compute_shade: (Ambient<<7) baked into the SW
             * tile's shade ahead of dynamic lights/AO) is map-authored, e.g.
             * road markings/crossings painted brighter than the surrounding
             * asphalt - fold it in the same way so it isn't lost to GL's
             * geometric AO + dynamic sun/shadow. */
            {
                /* Flags 0x20 (glass mode 21) / 0x01 (forced max shade) stay
                 * emissive as in SW. The old "emissive = tile Ambient" hack is
                 * gone: Ambient now feeds the per-corner light directly (see
                 * corner_baked_shade), Gouraud-smooth like SW, instead of a
                 * flat per-tile floor that showed as square patches. */
                uint8_t floor_em = 0;
                if (tile_flags & 0x20)
                    floor_em = 255;
                if (tile_flags & 0x01)
                    floor_em = 255;
                v[0].emissive = v[1].emissive = v[2].emissive = v[3].emissive = floor_em;
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
    uint8_t page, uint8_t light, float depth, uint8_t emissive)
{
    HwrVertex *o = &face_verts[face_vert_count++];
    o->x = (float)wx; o->y = (float)wy; o->z = (float)wz;
    o->u = u; o->v = v;
    o->page = page; o->light = light;
    o->tile_depth = depth;
    o->emissive = emissive;
    o->uv_w = o->uv_h = 0;   /* faces never use the water continuous-UV path */
}

static void emit_model_shadows(void);   /* fwd (defined with the trans sort below) */

/* Emit one transparent (blended) face vertex into the transparent batch. */
static void trans_emit_vert(int wx, int wy, int wz, uint8_t u, uint8_t v,
    uint8_t page, uint8_t light, float depth, uint8_t emissive)
{
    HwrVertex *o = &trans_verts[trans_vert_count++];
    o->x = (float)wx; o->y = (float)wy; o->z = (float)wz;
    o->u = u; o->v = v;
    o->page = page; o->light = light;
    o->tile_depth = depth;
    o->emissive = emissive;
    o->uv_w = o->uv_h = 0;   /* transparent faces never use the water UV path */
}

/* Face/tile Flags double as the SW rasterizer's RendVec_mode (vec_mode = ...
 * ->Flags directly in engindrwlstx_fac.c). Modes 2/3 are a raw texture blit
 * with no fade/shade term of any kind, and 21/25 are the window-glass modes
 * (fade_table + ghost_table blend) - all four render at a fixed brightness in
 * SW, completely untouched by scene lighting, so mirror that with full
 * emissive. Other unshaded-looking modes (7-13/18/19/22/23) still run their
 * texture through SW's fade_table, which reacts to the level's overall
 * lighting/palette state, so they must NOT be bypassed here or normal walls
 * lose their shading (regressed to fully lit in testing - only the truly
 * lighting-independent modes belong in this set). */
static int hwr_mode_is_emissive(int mode)
{
    switch (mode) {
        case 2: case 3: case 21: case 25:
            return 1;
        default:
            return 0;
    }
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

/* Emit one reflective (chameleon paint) vertex. lx/ly/lz are the point's
 * object-local coordinates (pre-rotation), so dirt/grime noise can be anchored
 * to the body panel instead of sliding as the vehicle moves/turns. */
static void refl_emit_vert(int wx, int wy, int wz, const float n[3],
    float base, float depth, float lx, float ly, float lz)
{
    HwrReflectVertex *o = &refl_verts[refl_vert_count++];
    o->x = (float)wx; o->y = (float)wy; o->z = (float)wz;
    o->nx = n[0]; o->ny = n[1]; o->nz = n[2];
    o->depth = depth;
    o->base = base;
    o->lx = lx; o->ly = ly; o->lz = lz;
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

/* --- Explosion crater decals + shatter fragments ---------------------------
 * Both are appended to the opaque face batch (face_verts/face_index) at the end
 * of sw_get_faces, so they share the face pass's index-0 cutout, depth-write and
 * scene lighting. Sources: set_floor_texture_uv_damaged_ground (crater decals)
 * and enginfexpl.c / draw_ex_face (shatter fragments). */

#define HWR_DAMAGE_PAGE 4          /* == HWR_TMAP_ANIM_PAGE0: animated/decal page */
/* Depth bias (scrd units) nudging the flat crater decal a hair toward the camera
 * so it wins GL_LESS against the coincident opaque floor tile beneath it, without
 * poking through walls that stand on the same tile. */
#define HWR_DECAL_DEPTH_BIAS 48.0f

/* Damaged-ground decal UVs on page 4, transcribed verbatim from
 * set_floor_texture_uv_damaged_ground (engindrwlstx_fac.c): one 32x32 sub-tile
 * per neighbour code 1..12 (directional edge/corner/centre crater pieces), in
 * the SW point1..point4 order. Returns 1 if nb is a valid damage code. */
static int hwr_damaged_ground_uv(int nb, uint8_t u[4], uint8_t v[4])
{
    static const uint8_t tbl[12][8] = {
        /* {u1,v1, u2,v2, u3,v3, u4,v4} */
        {160,64, 160,95, 191,64, 191,95}, /* 1  */
        {159,64, 159,95, 128,64, 128,95}, /* 2  */
        {223,64, 223,95, 192,64, 192,95}, /* 3  */
        {160,95, 191,95, 160,64, 191,64}, /* 4  */
        {159,95, 159,64, 128,95, 128,64}, /* 5  */
        {223,95, 223,64, 192,95, 192,64}, /* 6  */
        {191,95, 191,64, 160,95, 160,64}, /* 7  */
        {128,95, 128,64, 159,95, 159,64}, /* 8  */
        {192,95, 192,64, 223,95, 223,64}, /* 9  */
        {191,64, 160,64, 191,95, 160,95}, /* 10 */
        {128,64, 128,95, 159,64, 159,95}, /* 11 */
        {192,64, 192,95, 223,64, 223,95}, /* 12 */
    };
    const uint8_t *t;
    if (nb < 1 || nb > 12)
        return 0;
    t = tbl[nb - 1];
    u[0]=t[0]; v[0]=t[1]; u[1]=t[2]; v[1]=t[3];
    u[2]=t[4]; v[2]=t[5]; u[3]=t[6]; v[3]=t[7];
    return 1;
}

/* Walk the camera window (same bounds/geometry as sw_get_floor) and append a
 * blended crater-decal overlay quad for every tile whose damage code (top nibble
 * of ColumnHead, 1..12) is set - mirrors draw_floor_tile1a's second "damage
 * overlays" pass (engindrwlstx_fac.c:1164). Corner positions match sw_get_floor
 * exactly so the decal lands flush on its floor tile. */
static void emit_floor_damage_decals(void)
{
    int x0, x1, z0, z1, gx, gz;

    if (game_my_big_map == NULL || !snap.valid)
        return;

    hwr_floor_window_bounds(&x0, &x1, &z0, &z1);
    x0 = clampi(x0, 0, HWR_MAP_TILE_WIDTH - 2);
    x1 = clampi(x1, 0, HWR_MAP_TILE_WIDTH - 2);
    z0 = clampi(z0, 0, HWR_MAP_TILE_WIDTH - 2);
    z1 = clampi(z1, 0, HWR_MAP_TILE_WIDTH - 2);

    for (gz = z0; gz <= z1; gz++) {
        for (gx = x0; gx <= x1; gx++) {
            struct HwrMapEl *me = &game_my_big_map[HWR_MAP_TILE_WIDTH * gz + gx];
            int nb = (me->ColumnHead >> 12) & 0xF;
            uint8_t du[4], dv[4], lt[4];
            float wx[4], wy[4], wz[4], dep[4];
            int base, c;

            if (!hwr_damaged_ground_uv(nb, du, dv))
                continue;
            if (face_vert_count + 4 > HWR_FACE_MAX_VERTS ||
                face_index_count + 6 > HWR_FACE_MAX_INDEX)
                return;

            /* Corners v0=(gx,gz) v1=(gx+1,gz) v2=(gx+1,gz+1) v3=(gx,gz+1). */
            wx[0]=(float)(gx<<8);     wz[0]=(float)(gz<<8);
            wx[1]=(float)((gx+1)<<8); wz[1]=(float)(gz<<8);
            wx[2]=(float)((gx+1)<<8); wz[2]=(float)((gz+1)<<8);
            wx[3]=(float)(gx<<8);     wz[3]=(float)((gz+1)<<8);
            wy[0]=(float)(8*corner_alt(gx,   gz)   + corner_wave_y(gx,   gz,   (int)wx[0],(int)wz[0]));
            wy[1]=(float)(8*corner_alt(gx+1, gz)   + corner_wave_y(gx+1, gz,   (int)wx[1],(int)wz[1]));
            wy[2]=(float)(8*corner_alt(gx+1, gz+1) + corner_wave_y(gx+1, gz+1, (int)wx[2],(int)wz[2]));
            wy[3]=(float)(8*corner_alt(gx,   gz+1) + corner_wave_y(gx,   gz+1, (int)wx[3],(int)wz[3]));
            /* Damage decals lie flat on the floor and ride the faces pass, which
             * now uses the unified 128-identity scale - light them with the same
             * SW per-corner floor shade so they match the ground they overlay. */
            lt[0]=corner_baked_shade(gx,gz);     lt[1]=corner_baked_shade(gx+1,gz);
            lt[2]=corner_baked_shade(gx+1,gz+1); lt[3]=corner_baked_shade(gx,gz+1);
            for (c = 0; c < 4; c++)
                dep[c] = face_scrd(wx[c], wy[c], wz[c]) - HWR_DECAL_DEPTH_BIAS;

            /* SW damaged-ground point order maps to grid corners as
             *   point1=(gx,gz) point2=(gx,gz+1) point3=(gx+1,gz) point4=(gx+1,gz+1)
             * -> GL corner->UV: v0=UV1, v1=UV3, v2=UV4, v3=UV2. */
            base = face_vert_count;
            face_emit_vert((int)wx[0],(int)wy[0],(int)wz[0], du[0],dv[0], HWR_DAMAGE_PAGE, lt[0], dep[0], 0);
            face_emit_vert((int)wx[1],(int)wy[1],(int)wz[1], du[2],dv[2], HWR_DAMAGE_PAGE, lt[1], dep[1], 0);
            face_emit_vert((int)wx[2],(int)wy[2],(int)wz[2], du[3],dv[3], HWR_DAMAGE_PAGE, lt[2], dep[2], 0);
            face_emit_vert((int)wx[3],(int)wy[3],(int)wz[3], du[1],dv[1], HWR_DAMAGE_PAGE, lt[3], dep[3], 0);
            /* triangles (v0,v3,v1)+(v1,v3,v2), same split as the floor tile. */
            face_index[face_index_count++] = base + 0;
            face_index[face_index_count++] = base + 3;
            face_index[face_index_count++] = base + 1;
            face_index[face_index_count++] = base + 1;
            face_index[face_index_count++] = base + 3;
            face_index[face_index_count++] = base + 2;
        }
    }
}

/* Palette-colour sentinel page for the face shader's unlit solid-colour path
 * (the vUV.z 252.5..253.5 branch in the floor/face fragment shader). */
#define HWR_SOLID_PAGE 253

/* Append active explosion shrapnel (shrapnel[], DrIT_SharpnlPoly / draw_shrapnel)
 * to the opaque face batch as flat palette-coloured triangles. Corner geometry
 * mirrors enlist_draw_bang_shrapnels (engindrwlstm_3d.c) but kept in world space
 * (x/z in >>8 units, y in >>5); the face pass transforms X/Z itself, so only Y
 * carries the extra -snap.yc quirk shared with the ex_faces / fire collectors.
 * SW picks the tri colour by winding (draw_trigpoly back-face cull): one side
 * colour_lookup[8], the other [9]; GL culling is disabled, so we choose per-chip
 * from the camera-facing sign instead. Positions + spin interpolate to the
 * display rate against the previous turn's snapshot. */
static void emit_shrapnels(void)
{
    int n;

    if (!snap.valid)
        return;

    for (n = 0; n < hwr_shrapnel_active_count; n++) {
        struct HwrShrapnel *sh, *pv;
        int idx = hwr_shrapnel_active[n];
        int yaw, pitch;
        int bx, by, bz;
        int cwx[3], cwy[3], cwz[3];
        int sh_cc, sh_cs, sh_sc, sh_ss, sh_z;
        int col, base, k;

        if (idx < 1 || idx >= HWR_SHRAPNEL_COUNT)
            continue;
        sh = &shrapnel[idx];
        if (sh->type < 1 || sh->type > 3)
            continue;
        if (face_vert_count + 3 > HWR_FACE_MAX_VERTS ||
            face_index_count + 3 > HWR_FACE_MAX_INDEX)
            return;

        /* Interpolate position + spin against last turn (same slot + type). */
        bx = sh->x; by = sh->y; bz = sh->z;
        yaw = sh->yaw; pitch = sh->pitch;
        pv = &shrapnel_prev[idx];
        if (shrapnel_prev_valid && g_interp_alpha < 1.0f &&
            pv->type == sh->type) {
            float a = g_interp_alpha;
            int dyaw   = (int)(int8_t)(sh->yaw   - pv->yaw);   /* wrap-safe */
            int dpitch = (int)(int8_t)(sh->pitch - pv->pitch);
            bx = pv->x + (int)((float)(sh->x - pv->x) * a);
            by = pv->y + (int)((float)(sh->y - pv->y) * a);
            bz = pv->z + (int)((float)(sh->z - pv->z) * a);
            yaw   = (pv->yaw   + (int)((float)dyaw   * a)) & 0xFF;
            pitch = (pv->pitch + (int)((float)dpitch * a)) & 0xFF;
        }

        /* Rotated triangle corner offsets (verbatim from
         * enlist_draw_bang_shrapnels' fixed-point yaw/pitch rotation). */
        {
            int cos_yaw, cos_pitch, sin_yaw, sin_pitch, tmp;
            short shrap_yaw   = (short)(8 * yaw);
            short shrap_pitch = (short)(8 * pitch);
            cos_yaw   = lbSinTable[shrap_yaw   + HWR_LBFPMATH_PI/2];
            cos_pitch = lbSinTable[shrap_pitch + HWR_LBFPMATH_PI/2];
            sin_pitch = lbSinTable[shrap_pitch];
            sin_yaw   = lbSinTable[shrap_yaw];

            tmp = (cos_pitch * cos_yaw) & 0xFFFF0000;
            tmp |= (int)(((uint64_t)(cos_pitch * (int64_t)cos_yaw) >> 32) & 0xFFFF);
            sh_cc = (int)bw_rotl32((uint32_t)tmp, 16) >> 10;

            tmp = (cos_pitch * sin_yaw) & 0xFFFF0000;
            tmp |= (int)(((uint64_t)(cos_pitch * (int64_t)sin_yaw) >> 32) & 0xFFFF);
            sh_cs = (int)bw_rotl32((uint32_t)tmp, 16) >> 10;

            tmp = (sin_pitch * cos_yaw) & 0xFFFF0000;
            tmp |= (int)(((uint64_t)(sin_pitch * (int64_t)cos_yaw) >> 32) & 0xFFFF);
            sh_sc = (int)bw_rotl32((uint32_t)tmp, 16) >> 10;

            tmp = (sin_pitch * sin_yaw) & 0xFFFF0000;
            tmp |= (int)(((uint64_t)(sin_pitch * (int64_t)sin_yaw) >> 32) & 0xFFFF);
            sh_ss = (int)bw_rotl32((uint32_t)tmp, 16) >> 10;

            sh_z = sin_yaw >> 10;
        }

        bx = bx >> 8;   /* SW cor_dx uses x>>8 */
        by = by >> 5;   /* SW cor_dy uses y>>5 */
        bz = bz >> 8;   /* SW cor_dz uses z>>8 */

        /* World-space corners (== SW sp1/sp2/sp3 before camera subtraction). */
        cwx[0] = bx + sh_cc;         cwy[0] = by - sh_sc;         cwz[0] = bz - sh_z;
        cwx[1] = bx + sh_cs;         cwy[1] = by - sh_ss;         cwz[1] = bz;
        cwx[2] = bx - sh_cc - sh_cs; cwy[2] = by + sh_sc + sh_ss; cwz[2] = bz + sh_z;

        /* Pick the two-tone colour by which side faces the camera (snap.xc/yc/zc
         * = world camera centre, same per-axis units as the corners). */
        {
            int e1x = cwx[1]-cwx[0], e1y = cwy[1]-cwy[0], e1z = cwz[1]-cwz[0];
            int e2x = cwx[2]-cwx[0], e2y = cwy[2]-cwy[0], e2z = cwz[2]-cwz[0];
            long nx = (long)e1y*e2z - (long)e1z*e2y;
            long ny = (long)e1z*e2x - (long)e1x*e2z;
            long nz = (long)e1x*e2y - (long)e1y*e2x;
            long vx = (long)snap.xc - cwx[0];
            long vy = (long)snap.yc - cwy[0];
            long vz = (long)snap.zc - cwz[0];
            long facing = nx*vx + ny*vy + nz*vz;
            col = colour_lookup[(facing >= 0) ? 8 : 9];
        }

        base = face_vert_count;
        for (k = 0; k < 3; k++) {
            int wx = cwx[k];
            int wy = cwy[k] - snap.yc;   /* extra -yc quirk (see ex_faces note) */
            int wz = cwz[k];
            face_emit_vert(wx, wy, wz, (uint8_t)col, 0, HWR_SOLID_PAGE, 128,
                face_scrd((float)wx, (float)wy, (float)wz), 0);
        }
        face_index[face_index_count++] = base + 0;
        face_index[face_index_count++] = base + 1;
        face_index[face_index_count++] = base + 2;
    }
}

/* Append active explosion fragments (ex_faces[], DrIT_Unkn5 / draw_ex_face) to
 * the opaque face batch. Types 3/4 store absolute world coords; 1/2/5/6 store a
 * base (X,Y,Z) plus per-corner offsets. Odd types are textured tris indexing
 * game_face_textures; even types are textured quads indexing game_textures. The
 * SW draw feeds transform_shpoint a vertical delta of (worldY - yc) - 8*yc, so
 * the GL world Y carries the extra -snap.yc (same quirk as the fire collector,
 * see the effects loop above). Fragments are drawn full-bright (SW fixes their
 * point shade to 0x100000). */
static void emit_explode_faces(void)
{
    int i;

    if (dont_bother_with_explode_faces || !snap.valid ||
        game_textures == NULL || game_face_textures == NULL)
        return;

    for (i = 1; i < HWR_EXPLODE_FACES_COUNT; i++) {
        struct HwrExplodeFace *ef = &ex_faces[i];
        int type = ef->Type;
        int quad = (type == 2 || type == 4 || type == 6);
        int absolute = (type == 3 || type == 4);
        int npt = quad ? 4 : 3;
        int cxs[4], cys[4], czs[4], k, base;
        uint8_t cu[4], cv[4], page = 0;

        if (ef->Timer == 0 || type < 1 || type > 6)
            continue;
        if (face_vert_count + npt > HWR_FACE_MAX_VERTS ||
            face_index_count + (quad ? 6 : 3) > HWR_FACE_MAX_INDEX)
            return;

        {
            int ox[4] = { ef->X0, ef->X1, ef->X2, ef->X3 };
            int oy[4] = { ef->Y0, ef->Y1, ef->Y2, ef->Y3 };
            int oz[4] = { ef->Z0, ef->Z1, ef->Z2, ef->Z3 };
            int bx = absolute ? 0 : ef->X;
            int by = absolute ? 0 : ef->Y;
            int bz = absolute ? 0 : ef->Z;
            /* Interpolate the fragment's corners between the previous turn and
             * this one so flying debris / collapsing building shards move
             * smoothly at the display rate. Same slot must hold the same
             * fragment (matched by Type + Timer); a reused slot or a fresh
             * fragment snaps.
             *
             * Types 1/2/5/6 (thing_expld.c: animate_explode_face1/face5)
             * decrement Timer every turn while alive, so "still the same
             * fragment" means Timer strictly decreased. Types 3/4 (absolute
             * world-coord tri/quad shatter pieces, animate_explode_face3_tri/
             * _quad) are different: their Timer is a constant set at spawn and
             * is only ever changed to 0 at the instant they die or subdivide
             * (explode_face3_move_above_ground never touches it) - requiring
             * "decreased" there can never be true, so these NEVER interpolated
             * before, only snapped every tick for their whole (often brief,
             * subdivision-heavy) life. For them "still alive" instead means
             * Timer is unchanged from last turn. */
            struct HwrExplodeFace *pf = &ex_faces_prev[i];
            int interp = (ex_faces_prev_valid && g_interp_alpha < 1.0f &&
                pf->Type == ef->Type && pf->Timer != 0 &&
                (absolute ? (pf->Timer == ef->Timer) : (pf->Timer > ef->Timer)));
            int pbx = 0, pby = 0, pbz = 0;
            int pox[4], poy[4], poz[4];
            float a = g_interp_alpha;
            if (interp) {
                pbx = absolute ? 0 : pf->X;
                pby = absolute ? 0 : pf->Y;
                pbz = absolute ? 0 : pf->Z;
                pox[0]=pf->X0; pox[1]=pf->X1; pox[2]=pf->X2; pox[3]=pf->X3;
                poy[0]=pf->Y0; poy[1]=pf->Y1; poy[2]=pf->Y2; poy[3]=pf->Y3;
                poz[0]=pf->Z0; poz[1]=pf->Z1; poz[2]=pf->Z2; poz[3]=pf->Z3;
            }
            for (k = 0; k < npt; k++) {
                int ccx = bx + ox[k];
                int ccy = by + oy[k];
                int ccz = bz + oz[k];
                if (interp) {
                    int pcx = pbx + pox[k];
                    int pcy = pby + poy[k];
                    int pcz = pbz + poz[k];
                    ccx = pcx + (int)((float)(ccx - pcx) * a);
                    ccy = pcy + (int)((float)(ccy - pcy) * a);
                    ccz = pcz + (int)((float)(ccz - pcz) * a);
                }
                cxs[k] = ccx;
                cys[k] = ccy - snap.yc;   /* extra -yc: see note above */
                czs[k] = ccz;
            }
        }

        if (quad) {
            int idx = ef->Texture;
            struct HwrFloorTex *tx;
            if (idx < 0 || idx >= game_textures_limit) idx = 0;
            tx = &game_textures[idx];
            page = tx->Page;
            /* set_floor_texture_uv: corner c_n <- TMap(n+1). */
            cu[0]=tx->TMapX1; cv[0]=tx->TMapY1;
            cu[1]=tx->TMapX2; cv[1]=tx->TMapY2;
            cu[2]=tx->TMapX3; cv[2]=tx->TMapY3;
            cu[3]=tx->TMapX4; cv[3]=tx->TMapY4;
        } else {
            int idx = ef->Texture;
            struct HwrFaceTex *tx;
            if (idx < 0 || idx >= face_textures_limit) idx = 0;
            tx = &game_face_textures[idx];
            page = tx->Page;
            /* draw_ex_face odd types: c0<-TMap1, c1<-TMap2, c2<-TMap3. */
            cu[0]=tx->TMapX1; cv[0]=tx->TMapY1;
            cu[1]=tx->TMapX2; cv[1]=tx->TMapY2;
            cu[2]=tx->TMapX3; cv[2]=tx->TMapY3;
        }

        base = face_vert_count;
        /* 128 = identity on the unified faces-pass scale (was 255 when the
         * pass treated 255 as identity) - fragments stay full-bright. */
        for (k = 0; k < npt; k++)
            face_emit_vert(cxs[k], cys[k], czs[k], cu[k], cv[k], page, 128,
                face_scrd((float)cxs[k], (float)cys[k], (float)czs[k]), 0);

        if (quad) {
            /* two tris (c0,c1,c2)+(c1,c2,c3), matching draw_ex_face's quad. */
            face_index[face_index_count++] = base + 0;
            face_index[face_index_count++] = base + 1;
            face_index[face_index_count++] = base + 2;
            face_index[face_index_count++] = base + 1;
            face_index[face_index_count++] = base + 2;
            face_index[face_index_count++] = base + 3;
        } else {
            face_index[face_index_count++] = base + 0;
            face_index[face_index_count++] = base + 1;
            face_index[face_index_count++] = base + 2;
        }
    }
}

static int sw_get_faces(void *ctx, HwrGeometryBatch *out)
{
    int x0, x1, z0, z1;
    unsigned o;
    (void)ctx;
    face_vert_count = 0;
    face_index_count = 0;
    refl_vert_count = 0;
    refl_index_count = 0;
    trans_vert_count = 0;
    trans_index_count = 0;
    int transp_debug = hwr_lights_defaults().transp_debug;
    if (out != NULL) {
        out->verts = NULL; out->vert_count = 0;
        out->indices = NULL; out->index_count = 0;
    }
    if (game_objects == NULL || game_object_points == NULL ||
        game_object_faces4 == NULL || game_object_faces3 == NULL ||
        game_textures == NULL || out == NULL || !snap.valid)
        return 0;

    hwr_floor_window_bounds(&x0, &x1, &z0, &z1);

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
            /* Interpolate the position between the previous turn and this one so
             * vehicles/turrets/rotors move smoothly at the display rate. Rotation
             * still steps at 16Hz (kept at the current matrix). Skip when there is
             * no prior snapshot for this object (new/respawned) or when the jump is
             * too large to be real motion (teleport) - snap in those cases. */
            if (obj_snap_prev_valid && o < obj_snap_prev_count &&
                obj_snap_prev[o].is_dynamic && g_interp_alpha < 1.0f) {
                int dtx = obj_tx - obj_snap_prev[o].tx;
                int dty = obj_ty - obj_snap_prev[o].ty;
                int dtz = obj_tz - obj_snap_prev[o].tz;
                if (abs(dtx) < (2 << 8) && abs(dty) < (2 << 8) && abs(dtz) < (2 << 8)) {
                    float a = g_interp_alpha;
                    obj_tx = obj_snap_prev[o].tx + (int)((float)dtx * a);
                    obj_ty = obj_snap_prev[o].ty + (int)((float)dty * a);
                    obj_tz = obj_snap_prev[o].tz + (int)((float)dtz * a);
                }
            }
            /* Cull by captured tile position. */
            if ((obj_tx >> 8) < x0 || (obj_tx >> 8) > x1 ||
                (obj_tz >> 8) < z0 || (obj_tz >> 8) > z1)
                continue;
            if (matx_idx > 0 && matx_idx < (int16_t)next_local_mat) {
                const HwrM33 *src = &snap_local_mats[matx_idx];
                HwrM33 mlerp;
                int did_lerp = 0;
                /* Interpolate the rotation between the previous turn's matrix and
                 * this one so cornering is smooth at the display rate. Element-wise
                 * lerp of the two basis matrices (small per-turn angles) followed by
                 * the Gram-Schmidt re-orthonormalise inside hwr_reduce_tilt keeps it
                 * a clean rotation. */
                if (obj_snap_prev_valid && o < obj_snap_prev_count &&
                    obj_snap_prev[o].is_dynamic && g_interp_alpha < 1.0f) {
                    int16_t pmatx = obj_snap_prev[o].matx;
                    if (pmatx > 0 && pmatx < 100) {
                        const HwrM33 *pm = &snap_local_mats_prev[pmatx];
                        float a = g_interp_alpha;
                        int r, c;
                        for (r = 0; r < 3; r++)
                            for (c = 0; c < 3; c++)
                                mlerp.R[r][c] = (int32_t)((float)pm->R[r][c] +
                                    ((float)src->R[r][c] - (float)pm->R[r][c]) * a);
                        src = &mlerp;
                        did_lerp = 1;
                    }
                }
                obj_mat = src;
                /* Halve the cornering lean for actual vehicles (not turrets/
                 * rotors, which don't bank); this also re-orthonormalises. For
                 * turrets/rotors, re-orthonormalise the interpolated matrix too. */
                if (obj_snap[o].is_vehicle) {
                    hwr_reduce_tilt(src, &obj_mat_lvl, 0.5f);
                    obj_mat = &obj_mat_lvl;
                } else if (did_lerp) {
                    hwr_reduce_tilt(src, &obj_mat_lvl, 1.0f);
                    obj_mat = &obj_mat_lvl;
                }
            }
        } else {
            /* Static building: cached world-unit position, no rotation. */
            int mtx = (uint16_t)obj->MapX >> 8;
            int mtz = (uint16_t)obj->MapZ >> 8;
            /* Only emit static objects the SW build actually drew this frame.
             * Destroyed/collapsed buildings stay in game_objects[] but are no
             * longer traversed by the SW build (draw_object), so their live bit
             * is clear — skip them so GL doesn't render a building SW removed. */
            if (!((hwr_obj_live_mask[o >> 3] >> (o & 7)) & 1))
                continue;
            if (mtx < x0 || mtx > x1 || mtz < z0 || mtz > z1)
                continue;
            obj_tx = (int)(uint16_t)obj->MapX;
            obj_ty = (int)obj->OffsetY;
            obj_tz = (int)(uint16_t)obj->MapZ;
            /* Interpolate static-object position too, so collapsing/hovering
             * buildings move smoothly at the display rate. Only when the prior
             * snapshot for this object was also static; a large jump snaps. */
            if (obj_snap_prev_valid && o < obj_snap_prev_count &&
                !obj_snap_prev[o].is_dynamic && g_interp_alpha < 1.0f) {
                int dtx = obj_tx - obj_snap_prev[o].tx;
                int dty = obj_ty - obj_snap_prev[o].ty;
                int dtz = obj_tz - obj_snap_prev[o].tz;
                if (abs(dtx) < (2 << 8) && abs(dty) < (2 << 8) && abs(dtz) < (2 << 8)) {
                    float a = g_interp_alpha;
                    obj_tx = obj_snap_prev[o].tx + (int)((float)dtx * a);
                    obj_ty = obj_snap_prev[o].ty + (int)((float)dty * a);
                    obj_tz = obj_snap_prev[o].tz + (int)((float)dtz * a);
                }
            }
        }

        /* Per-object floating bob: SingleObject.field_1C & 0x0100 makes the SW
         * engine add waft_table[gameturn&0x1F] to the object Y (draw_object_faces).
         * Reproduce it here (interpolated) so flagged objects (e.g. floating
         * buildings) hover smoothly. Full waft value, matching the object path. */
        if ((obj->field_1C & 0x0100) != 0) {
            int ic = (int)(render_anim_turn & 0x1F);
            int ip = (int)((render_anim_turn - 1) & 0x1F);
            float a2 = g_interp_alpha;
            float wob;
            if (a2 < 0.0f) a2 = 0.0f;
            if (a2 > 1.0f) a2 = 1.0f;
            wob = (float)waft_table[ip] + ((float)waft_table[ic] - (float)waft_table[ip]) * a2;
            obj_ty += (int)wob;
        }

        /* One object can be referenced by several map columns; emit once. */
        if (face_obj_seen[o >> 3] & (1 << (o & 7)))
            continue;
        face_obj_seen[o >> 3] |= (uint8_t)(1 << (o & 7));


        /* Deep-radar see-through: whole object flagged semi-transparent by the
         * SW engine this frame -> all its (non-reflective) faces blend.
         * [transparency] debug=1 forces every object transparent (diagnostic to
         * separate "blended pass works" from "deep-radar mask is being set"). */
        int obj_transp = transp_debug || ((hwr_obj_transp_mask[o >> 3] >> (o & 7)) & 1);

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
                        (float)fc->ExCol, rsd,
                        (float)rp[rk]->X, (float)rp[rk]->Y, (float)rp[rk]->Z);
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
            /* Transparent (blended) face: whole object is deep-radar see-through,
             * or this face uses the SW transparent-textured mode (wire fence /
             * glass). Reflective faces handled above; everything else opaque. */
            /* Blended (see-through) faces: deep-radar see-through objects, or the
             * mode-6 "transparent textured" faces (window glass / fences). Both
             * the opaque and blended passes discard the index-0 texture key, but
             * mode-6 glass has solid (non-key) glass texels that must blend so
             * you can see the building interior through bank windows — hence the
             * blended pass, not opaque. */
            /* Blended (see-through) faces: building WINDOW GLASS (modes 21/25 —
             * bank fronts etc.) so you can see the interior, plus deep-radar
             * see-through objects (obj_transp). Mode 6 (fences/scaffolding) is
             * NOT here: it goes to the opaque pass, which discards the index-0
             * texture key, giving solid bars with see-through gaps. */
            int is_transp = obj_transp || fc->Flags == 21 || fc->Flags == 25;
            struct HwrFloorTex *tx = NULL;
            uint8_t pg, u0,v0c,u1,v1c,u2,v2c,u3,v3c;
            struct HwrSinglePoint *p[4];
            int wx[4], wy[4], wz[4], k, base;
            float sd[4];

            if (!flat && texidx >= game_textures_limit)
                texidx = 0;   /* clamp like set_floor_texture_uv, still render */
            if (is_transp) {
                if (trans_vert_count + 4 > HWR_TRANS_MAX_VERTS ||
                    trans_index_count + 6 > HWR_TRANS_MAX_INDEX)
                    continue;
            } else if (face_vert_count + 4 > HWR_FACE_MAX_VERTS ||
                       face_index_count + 6 > HWR_FACE_MAX_INDEX) {
                break;
            }
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

            /* Full emissive for face modes SW renders at fixed brightness
             * regardless of scene lighting (window glass + raw texture blits -
             * see hwr_mode_is_emissive). ONLY for non-rotated objects: SW's
             * fixed-identity mode-2 special case lives in draw_object_face4d;
             * rotated objects (matrix set - vehicles, swaying trees) draw via
             * draw_object_face4g, which normal-shades EVERY mode per frame.
             * Treating a rotated object's mode-2 faces as emissive rendered
             * them full-bright (washed-out pink park trees). */
            uint8_t em[4] = {0, 0, 0, 0};
            if (obj_mat == NULL && hwr_mode_is_emissive(fc->Flags))
                em[0] = em[1] = em[2] = em[3] = 255;

            /* Quad diagonal is PN1-PN2, matching draw_object_face4d_textrd:
             * triangles (PN0,PN2,PN1) + (PN3,PN1,PN2). A naive (0,1,2)+(0,2,3)
             * fan splits the wrong diagonal and leaves triangular gaps. */
            if (is_transp) {
                /* Deep-radar see-through faces lose their texture and render as a
                 * flat syndicate tint (page sentinel 254); glass/fence (mode 6)
                 * stay textured. */
                uint8_t epg = obj_transp ? (uint8_t)254 : pg;
                base = trans_vert_count;
                trans_emit_vert(wx[0], wy[0], wz[0], u0, v0c, epg, 200, sd[0], em[0]);
                trans_emit_vert(wx[1], wy[1], wz[1], u1, v1c, epg, 200, sd[1], em[1]);
                trans_emit_vert(wx[2], wy[2], wz[2], u2, v2c, epg, 200, sd[2], em[2]);
                trans_emit_vert(wx[3], wy[3], wz[3], u3, v3c, epg, 200, sd[3], em[3]);
                trans_index[trans_index_count++] = base + 0;
                trans_index[trans_index_count++] = base + 2;
                trans_index[trans_index_count++] = base + 1;
                trans_index[trans_index_count++] = base + 3;
                trans_index[trans_index_count++] = base + 1;
                trans_index[trans_index_count++] = base + 2;
            } else {
                /* SW per-vertex face shading. Static objects (buildings) bake the
                 * brightness in Shade0..3 (128 = identity, draw_object_face4d);
                 * rotated objects (vehicles - obj_mat set) use Shade0..3 as
                 * normal indices and recompute from the rotated normal each
                 * frame (draw_object_face4g). Mode-2 faces are identity-flat in
                 * SW and ride the emissive path (em=255) instead. */
                uint8_t fl0, fl1, fl2, fl3;
                if (obj_mat != NULL) {
                    fl0 = sw_face_shade_dynamic(fc->Shade0, obj_mat);
                    fl1 = sw_face_shade_dynamic(fc->Shade1, obj_mat);
                    fl2 = sw_face_shade_dynamic(fc->Shade2, obj_mat);
                    fl3 = sw_face_shade_dynamic(fc->Shade3, obj_mat);
                } else {
                    fl0 = sw_face_shade(fc->Shade0, (uint16_t)fc->Light0);
                    fl1 = sw_face_shade(fc->Shade1, (uint16_t)fc->Light1);
                    fl2 = sw_face_shade(fc->Shade2, (uint16_t)fc->Light2);
                    fl3 = sw_face_shade(fc->Shade3, (uint16_t)fc->Light3);
                }
                base = face_vert_count;
                face_emit_vert(wx[0], wy[0], wz[0], u0, v0c, pg, fl0, sd[0], em[0]);
                face_emit_vert(wx[1], wy[1], wz[1], u1, v1c, pg, fl1, sd[1], em[1]);
                face_emit_vert(wx[2], wy[2], wz[2], u2, v2c, pg, fl2, sd[2], em[2]);
                face_emit_vert(wx[3], wy[3], wz[3], u3, v3c, pg, fl3, sd[3], em[3]);
                face_index[face_index_count++] = base + 0;
                face_index[face_index_count++] = base + 2;
                face_index[face_index_count++] = base + 1;
                face_index[face_index_count++] = base + 3;
                face_index[face_index_count++] = base + 1;
                face_index[face_index_count++] = base + 2;
            }
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
                        (float)fc->ExCol, rsd,
                        (float)rp[rk]->X, (float)rp[rk]->Y, (float)rp[rk]->Z);
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
            /* Blended (see-through) faces: deep-radar see-through objects, or the
             * mode-6 "transparent textured" faces (window glass / fences). Both
             * the opaque and blended passes discard the index-0 texture key, but
             * mode-6 glass has solid (non-key) glass texels that must blend so
             * you can see the building interior through bank windows — hence the
             * blended pass, not opaque. */
            /* Blended (see-through) faces: building WINDOW GLASS (modes 21/25 —
             * bank fronts etc.) so you can see the interior, plus deep-radar
             * see-through objects (obj_transp). Mode 6 (fences/scaffolding) is
             * NOT here: it goes to the opaque pass, which discards the index-0
             * texture key, giving solid bars with see-through gaps. */
            int is_transp = obj_transp || fc->Flags == 21 || fc->Flags == 25;
            struct HwrFaceTex *tx = NULL;
            uint8_t pg, u0,v0c,u1,v1c,u2,v2c;
            struct HwrSinglePoint *p[3];
            int wx[3], wy[3], wz[3], k, base;
            float sd[3];

            if (!flat && texidx >= face_textures_limit)
                texidx = 0;   /* clamp like set_face_texture_uv, still render */
            if (is_transp) {
                if (trans_vert_count + 3 > HWR_TRANS_MAX_VERTS ||
                    trans_index_count + 3 > HWR_TRANS_MAX_INDEX)
                    continue;
            } else if (face_vert_count + 3 > HWR_FACE_MAX_VERTS ||
                       face_index_count + 3 > HWR_FACE_MAX_INDEX) {
                break;
            }
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

            /* See the face4 loop above / hwr_mode_is_emissive: emissive modes
             * only apply to non-rotated objects (rotated = normal-shaded). */
            uint8_t em3[3] = {0, 0, 0};
            if (obj_mat == NULL && hwr_mode_is_emissive(fc->Flags))
                em3[0] = em3[1] = em3[2] = 255;

            if (is_transp) {
                uint8_t epg = obj_transp ? (uint8_t)254 : pg;
                base = trans_vert_count;
                trans_emit_vert(wx[0], wy[0], wz[0], u0, v0c, epg, 200, sd[0], em3[0]);
                trans_emit_vert(wx[1], wy[1], wz[1], u1, v1c, epg, 200, sd[1], em3[1]);
                trans_emit_vert(wx[2], wy[2], wz[2], u2, v2c, epg, 200, sd[2], em3[2]);
                trans_index[trans_index_count++] = base + 0;
                trans_index[trans_index_count++] = base + 1;
                trans_index[trans_index_count++] = base + 2;
            } else {
                /* SW per-vertex shading. Rotated objects (matrix set) draw via
                 * the "g" variant: normal-shaded per frame for EVERY mode.
                 * Non-rotated (draw_object_face3d): modes 0/2 identity-flat,
                 * otherwise Shade0..2 = baked brightness. */
                uint8_t fl0, fl1, fl2;
                if (obj_mat != NULL) {
                    fl0 = sw_face_shade_dynamic(fc->Shade0, obj_mat);
                    fl1 = sw_face_shade_dynamic(fc->Shade1, obj_mat);
                    fl2 = sw_face_shade_dynamic(fc->Shade2, obj_mat);
                } else if (fc->Flags == 0 || fc->Flags == 2) {
                    fl0 = fl1 = fl2 = 128;   /* SW identity-flat (unified scale) */
                } else {
                    fl0 = sw_face_shade(fc->Shade0, fc->Light0);
                    fl1 = sw_face_shade(fc->Shade1, fc->Light1);
                    fl2 = sw_face_shade(fc->Shade2, fc->Light2);
                }
                base = face_vert_count;
                face_emit_vert(wx[0], wy[0], wz[0], u0, v0c, pg, fl0, sd[0], em3[0]);
                face_emit_vert(wx[1], wy[1], wz[1], u1, v1c, pg, fl1, sd[1], em3[1]);
                face_emit_vert(wx[2], wy[2], wz[2], u2, v2c, pg, fl2, sd[2], em3[2]);
                face_index[face_index_count++] = base + 0;
                face_index[face_index_count++] = base + 1;
                face_index[face_index_count++] = base + 2;
            }
        }
    }

    /* NOTE: the deep-radar mask is NOT cleared here. sw_get_faces runs several
     * times per frame (sun shadow pass + main face pass), so clearing here would
     * leave later calls with an empty mask -> flicker. It is cleared once per
     * frame at the top of process_engine_unk3() (game.c), before the build. */

    /* Explosion crater decals + shatter fragments ride the opaque face batch
     * (index-0 cutout, depth-write, scene lighting). Appended last so they
     * overlay the base floor/faces already emitted this pass. */
    emit_floor_damage_decals();
    emit_explode_faces();
    emit_shrapnels();
    emit_model_shadows();

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

/* Emit the object-model ground-shadow decals captured during the SW build
 * (draw_object_model_shadow: the angled silhouette shadows matrix'd objects -
 * buildings/temples, vehicles - cast on the ground) into the blended
 * transparent batch. Each is a world-space quad textured from the page-4
 * shadow art; SW draws them ghosted (mode 10, ~50% darken), which the blended
 * pass's alpha reproduces. Texel 0 (outside the silhouette) is the transparent
 * key, discarded by the pass. Depth-biased toward the camera like the other
 * floor decals so they sit on top of the ground. */
static void emit_model_shadows(void)
{
    int i;
    if (!snap.valid)
        return;
    for (i = 0; i < hwr_model_shadow_count; i++) {
        struct HwrModelShadowMirror *ms = &hwr_model_shadow_list[i];
        uint8_t cu[4], cv[4];
        int base, k;
        if (trans_vert_count + 4 > HWR_TRANS_MAX_VERTS ||
            trans_index_count + 6 > HWR_TRANS_MAX_INDEX)
            return;
        /* Corner ring 1..4 maps the texture rect (X1,Y1)-(X2,Y2):
         * cor1=(-w,-l)->(u1,v1)  cor2=(+w,-l)->(u2,v1)
         * cor3=(+w,+l)->(u2,v2)  cor4=(-w,+l)->(u1,v2). */
        cu[0]=ms->u1; cv[0]=ms->v1;
        cu[1]=ms->u2; cv[1]=ms->v1;
        cu[2]=ms->u2; cv[2]=ms->v2;
        cu[3]=ms->u1; cv[3]=ms->v2;
        /* The corners were captured at the vehicle's 16Hz position, but the
         * vehicle BODY is drawn at the interpolated display position (see the
         * obj_snap lerp in the object loop). Shift the shadow by the same
         * horizontal delta so it tracks the body instead of stuttering a turn
         * behind - and so its depth-bias is computed from the SAME position the
         * body uses, which stops the decal intermittently sinking into the floor.
         * Horizontal (X/Z) only: Y stays the captured ground height under the
         * shadow. Teleport-guarded and snapped when there's no prior snapshot. */
        {
            int sx = 0, sz = 0;
            unsigned oi = ms->obj_idx;
            if (obj_snap_valid && oi < obj_snap_count && obj_snap[oi].is_dynamic &&
                obj_snap_prev_valid && oi < obj_snap_prev_count &&
                obj_snap_prev[oi].is_dynamic && g_interp_alpha < 1.0f) {
                int dtx = obj_snap[oi].tx - obj_snap_prev[oi].tx;
                int dtz = obj_snap[oi].tz - obj_snap_prev[oi].tz;
                if (abs(dtx) < (2 << 8) && abs(dtz) < (2 << 8)) {
                    float a1 = g_interp_alpha - 1.0f;   /* interp - current */
                    sx = (int)((float)dtx * a1);
                    sz = (int)((float)dtz * a1);
                }
            }
            base = trans_vert_count;
            for (k = 0; k < 4; k++) {
                int cx = ms->x[k] + sx;
                int cz = ms->z[k] + sz;
                float dep = face_scrd((float)cx, (float)ms->y[k],
                    (float)cz) - HWR_DECAL_DEPTH_BIAS;
                trans_emit_vert(cx, ms->y[k], cz, cu[k], cv[k],
                    4 /* shadow art page */, 128 /* identity */, dep, 0);
            }
        }
        trans_index[trans_index_count++] = base + 0;
        trans_index[trans_index_count++] = base + 1;
        trans_index[trans_index_count++] = base + 2;
        trans_index[trans_index_count++] = base + 0;
        trans_index[trans_index_count++] = base + 2;
        trans_index[trans_index_count++] = base + 3;
    }
}

/* One transparent triangle keyed by centroid depth for back-to-front sorting. */
struct HwrTransTri { float d; uint32_t i0, i1, i2; };

/* Sort farther triangles first (larger face_scrd depth = farther into screen),
 * so alpha blending composites back-to-front. */
static int trans_tri_cmp(const void *pa, const void *pb)
{
    float da = ((const struct HwrTransTri *)pa)->d;
    float db = ((const struct HwrTransTri *)pb)->d;
    if (da < db) return  1;
    if (da > db) return -1;
    return 0;
}

/* Hand back the transparent (blended) face batch collected by sw_get_faces,
 * triangle-sorted back-to-front. Must be called after sw_get_faces() each frame. */
static int sw_get_transparent_faces(void *ctx, HwrGeometryBatch *out)
{
    static struct HwrTransTri tri[HWR_TRANS_MAX_INDEX / 3];
    int ntri, t;
    (void)ctx;
    if (out == NULL)
        return 0;
    out->verts = NULL; out->vert_count = 0;
    out->indices = NULL; out->index_count = 0;
    if (trans_index_count < 3)
        return 0;

    ntri = trans_index_count / 3;
    for (t = 0; t < ntri; t++) {
        uint32_t a = trans_index[t * 3 + 0];
        uint32_t b = trans_index[t * 3 + 1];
        uint32_t c = trans_index[t * 3 + 2];
        tri[t].d = (trans_verts[a].tile_depth + trans_verts[b].tile_depth +
                    trans_verts[c].tile_depth) * (1.0f / 3.0f);
        tri[t].i0 = a; tri[t].i1 = b; tri[t].i2 = c;
    }
    qsort(tri, (size_t)ntri, sizeof(tri[0]), trans_tri_cmp);
    for (t = 0; t < ntri; t++) {
        trans_index_sorted[t * 3 + 0] = tri[t].i0;
        trans_index_sorted[t * 3 + 1] = tri[t].i1;
        trans_index_sorted[t * 3 + 2] = tri[t].i2;
    }

    out->verts = trans_verts;
    out->vert_count = trans_vert_count;
    out->indices = trans_index_sorted;
    out->index_count = ntri * 3;
    return ntri * 3;
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
                if (!obj_snap[o].has_passengers) continue;  /* empty cars get no lights */
                ddx = (long)obj_snap[o].tx - veh_cull_cx;
                ddz = (long)obj_snap[o].tz - veh_cull_cz;
                if (ddx*ddx + ddz*ddz <= VEH_CULL_RANGE2) nv++;
            }
            veh_reserve = nv * 4;                      /* 2 headlights + 2 tails per car */
            if (veh_reserve > 32) veh_reserve = 32;    /* cap: ~8 nearest cars */
        }
    }

    /* Reserve slots for nearby fire lights so the dense map lights don't fill
     * all 64 and starve them. Same idea as veh_reserve.
     *
     * Use the SAME forward-shifted cull disc as the vehicles (veh_cull_cx/cz,
     * VEH_CULL_RANGE2), NOT a small disc around snap.xc/zc: the camera is
     * angled and looks forward, so the visible ground extends far beyond a few
     * tiles from the eye. A tight disc centred on the eye culls the fires in
     * the far half of the screen — they'd pop in only as you moved toward them.
     * The shifted disc covers the whole visible play area (near edge at the
     * viewer, reaching well into the scene). */
    int fire_reserve = 0;
    {
        int fi;
        for (fi = 0; fi < hwr_firelight_count; fi++) {
            long ddx = (long)hwr_firelights[fi].x - veh_cull_cx;
            long ddz = (long)hwr_firelights[fi].z - veh_cull_cz;
            if (ddx*ddx + ddz*ddz <= VEH_CULL_RANGE2) fire_reserve++;
        }
        if (fire_reserve > 16) fire_reserve = 16;      /* cap */
        for (fi = 0; fi < hwr_persuadelight_count; fi++) {
            long ddx = (long)hwr_persuadelights[fi].x - veh_cull_cx;
            long ddz = (long)hwr_persuadelights[fi].z - veh_cull_cz;
            if (ddx*ddx + ddz*ddz <= VEH_CULL_RANGE2) fire_reserve++;
        }
        if (fire_reserve > 24) fire_reserve = 24;      /* cap incl. persuade pools */
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
        /* Intensity==0 is the engine's live on/off signal (a destroyed lamp's
         * Command sets it to 0 immediately; see hwr_debug.c's light-label
         * overlay, which uses the same check). TrueIntensity is a separate
         * stable pre-animation baseline used below for radius/brightness so
         * flicker doesn't pop lights in/out of the 64-slot uniform frame to
         * frame — but it is NOT necessarily zeroed by destruction, so relying
         * on it alone let dead lamps keep rendering at full baseline
         * brightness forever. Check both. */
        if (fl->Intensity == 0)
            continue;
        if (fl->TrueIntensity == 0)
            continue;
        if (fl->TrueIntensity < 0 && sstr <= 0.0f)
            continue;

        /* Some lamp fixtures chain two FullLight records at (almost) the exact
         * same position (see the LightHead->NextFull traversal above — a lamp
         * Thing can own more than one light in its chain). Rendering both as
         * separate point lights doubles the brightness at one spot, which
         * reads as "two overlapping lights" on a single lamp — most visible
         * now that building_radius is tight enough to show the doubled core
         * distinctly instead of blending into a wide pool. Skip a light if an
         * already-selected one sits within ~1/8 tile (32 PRC units) of it in
         * all three axes; only the first (closer-processed) one is kept. */
        {
            int dup = 0;
            for (j = 0; j < nnearest; j++) {
                struct HwrFullLight *ofl = &game_full_lights[nearest[j].idx];
                int odx = (int)fl->X - (int)ofl->X;
                int ody = (int)fl->Y - (int)ofl->Y;
                int odz = (int)fl->Z - (int)ofl->Z;
                if (odx > -32 && odx < 32 && ody > -32 && ody < 32 &&
                    odz > -32 && odz < 32) {
                    dup = 1;
                    break;
                }
            }
            if (dup) continue;
        }

        dx = (int)fl->X - cx;
        dz = (int)fl->Z - cz;
        d2 = dx*dx + dz*dz;

        if (nnearest < max - veh_reserve - fire_reserve) {
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
        out[i].dynamic = 0;                     /* static map lamp / anti-light */
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
            HwrM33 mlerp;
            float fx, fz, rx, rz, fl;
            int obj_tx, obj_ty, obj_tz;
            float tx, ty, tz;
            long ddx, ddz;
            int s;
            if (!obj_snap[o].is_vehicle || !obj_snap[o].has_passengers)
                continue;   /* no lights for non-vehicles or empty vehicles */
            ddx = (long)obj_snap[o].tx - veh_cull_cx;
            ddz = (long)obj_snap[o].tz - veh_cull_cz;
            if (ddx*ddx + ddz*ddz > VEH_CULL_RANGE2)
                continue;   /* only cars within the (shifted) cull disc get lights */
            /* Interpolate position + rotation the SAME way sw_get_faces does for
             * the vehicle body (see obj_tx/obj_mat above): obj_snap is captured
             * once per 16Hz sim turn but sw_get_lights runs every present frame,
             * so without this the headlight/tail pools snapped to the new spot
             * once a turn while the smoothly-interpolated car body and camera
             * glided past them - the light visibly detaching from the car each
             * turn read as a flicker. */
            obj_tx = obj_snap[o].tx;
            obj_ty = obj_snap[o].ty;
            obj_tz = obj_snap[o].tz;
            if (obj_snap_prev_valid && o < obj_snap_prev_count &&
                obj_snap_prev[o].is_dynamic && g_interp_alpha < 1.0f) {
                int dtx = obj_tx - obj_snap_prev[o].tx;
                int dty = obj_ty - obj_snap_prev[o].ty;
                int dtz = obj_tz - obj_snap_prev[o].tz;
                if (abs(dtx) < (2 << 8) && abs(dty) < (2 << 8) && abs(dtz) < (2 << 8)) {
                    float a = g_interp_alpha;
                    obj_tx = obj_snap_prev[o].tx + (int)((float)dtx * a);
                    obj_ty = obj_snap_prev[o].ty + (int)((float)dty * a);
                    obj_tz = obj_snap_prev[o].tz + (int)((float)dtz * a);
                }
            }
            tx = (float)obj_tx;
            ty = (float)obj_ty;
            tz = (float)obj_tz;
            /* Forward and right (XZ) from the vehicle matrix; fall back to axes. */
            m = (obj_snap[o].matx > 0 && obj_snap[o].matx < (int16_t)next_local_mat)
                ? &snap_local_mats[obj_snap[o].matx] : NULL;
            if (m != NULL && obj_snap_prev_valid && o < obj_snap_prev_count &&
                obj_snap_prev[o].is_dynamic && g_interp_alpha < 1.0f) {
                int16_t pmatx = obj_snap_prev[o].matx;
                if (pmatx > 0 && pmatx < 100) {
                    const HwrM33 *pm = &snap_local_mats_prev[pmatx];
                    float a = g_interp_alpha;
                    int r, c;
                    for (r = 0; r < 3; r++)
                        for (c = 0; c < 3; c++)
                            mlerp.R[r][c] = (int32_t)((float)pm->R[r][c] +
                                ((float)m->R[r][c] - (float)pm->R[r][c]) * a);
                    m = &mlerp;
                }
            }
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
                out[nnearest].dynamic = 1;
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
                out[nnearest].dynamic = 1;
                nnearest++;
            }
        }
        hwr_sw_vehicle_lights = nnearest - veh_light_start;
    }

    /* --- Fire dynamic lights ----------------------------------------------
     * A warm, flickering round pool per burning tile (see hwr_firelight_*).
     * Reproduces the SW apply_full_light ground glow that the static
     * game_full_lights scan above cannot see. Distance-culled to the same
     * forward-shifted disc used for the reserve count (covers the visible
     * play area — see the reserve comment above). */
    {
        HwrLightDefaults fld = hwr_lights_defaults();
        if (fld.firelight_enable) {
            float radius_scale = fld.firelight_radius / 21.0f;
            float base_dist2   = 4194304.0f * radius_scale;
            int fi;
            for (fi = 0; fi < hwr_firelight_count && nnearest < max; fi++) {
                struct HwrFireLight *fli = &hwr_firelights[fi];
                long ddx = (long)fli->x - veh_cull_cx;
                long ddz = (long)fli->z - veh_cull_cz;
                float flick, b;
                if (ddx*ddx + ddz*ddz > VEH_CULL_RANGE2)
                    continue;
                /* Per-light flicker: independent random dim each frame. */
                flick = 1.0f - fld.firelight_flicker * hwr_flick_rand();
                b = fld.firelight_brightness * fli->strength * flick;
                out[nnearest].x = fli->x;
                out[nnearest].y = fli->y;
                out[nnearest].z = fli->z;
                out[nnearest].r = 1.00f * b;   /* warm orange */
                out[nnearest].g = 0.55f * b;
                out[nnearest].b = 0.18f * b;
                out[nnearest].radius = 1.0f;   /* >0 = positive round light */
                out[nnearest].fdx = 0.0f; out[nnearest].fdz = 0.0f;
                /* Bigger fires reach a little further; small ones stay tight. */
                out[nnearest].max_dist2 = base_dist2 * (0.65f + 0.35f * fli->strength);
                out[nnearest].dynamic = 1;
                nnearest++;
            }
        }
    }

    /* --- Persuaded-person lights ------------------------------------------
     * Cold turquoise pool under people converted by the persuadertron. They
     * share the burning flag (TngF_Unkn40000000) with people who are alight, so
     * before this they emitted orange fire light; the sprite collector now
     * routes them into their own cluster set. Steady by default apart from a
     * slow breath — this is a control glow, not a fire. */
    {
        HwrLightDefaults pld = hwr_lights_defaults();
        if (pld.persuadelight_enable) {
            float radius_scale = pld.persuadelight_radius / 21.0f;
            float base_dist2   = 4194304.0f * radius_scale;
            int pi;
            for (pi = 0; pi < hwr_persuadelight_count && nnearest < max; pi++) {
                struct HwrFireLight *pli = &hwr_persuadelights[pi];
                long ddx = (long)pli->x - veh_cull_cx;
                long ddz = (long)pli->z - veh_cull_cz;
                float pulse, b;
                if (ddx*ddx + ddz*ddz > VEH_CULL_RANGE2)
                    continue;
                pulse = 1.0f - pld.persuadelight_pulse * hwr_flick_rand();
                b = pld.persuadelight_brightness * pli->strength * pulse;
                out[nnearest].x = pli->x;
                out[nnearest].y = pli->y;
                out[nnearest].z = pli->z;
                out[nnearest].r = pld.persuadelight_r * b;
                out[nnearest].g = pld.persuadelight_g * b;
                out[nnearest].b = pld.persuadelight_b * b;
                out[nnearest].radius = 1.0f;   /* >0 = positive round light */
                out[nnearest].fdx = 0.0f; out[nnearest].fdz = 0.0f;
                out[nnearest].max_dist2 = base_dist2 * (0.65f + 0.35f * pli->strength);
                out[nnearest].dynamic = 1;
                nnearest++;
            }
        }
    }

    return nnearest;
}

static int sw_get_sprites(void *ctx, HwrBillboard *out, int max)
{
    int n = hwr_collected_count;
    int i;
    if (n > max) n = max;
    if (n <= 0 || out == NULL)
        return n;
    memcpy(out, hwr_collected_billboards, (size_t)n * sizeof(HwrBillboard));
    if (g_interp_alpha >= 1.0f)
        return n;
    /* Lerp Thing-anchored billboards toward their previous-turn position so
     * they scroll smoothly with the now-interpolated floor/camera instead of
     * snapping once per 16Hz turn (see hwr_collected_key / prev_idx above). */
    for (i = 0; i < n; i++) {
        int p = hwr_collected_prev_idx[i];
        const HwrBillboard *pb;
        float dx, dy, dz;
        if (p < 0)
            continue;
        pb = &hwr_collected_billboards_prev[p];
        dx = out[i].x - pb->x;
        dy = out[i].y - pb->y;
        dz = out[i].z - pb->z;
        /* Teleport guard (2 map tiles = 512 world units): a respawn/warp
         * shouldn't visibly slide from the old spot. */
        if (dx*dx + dy*dy + dz*dz > 512.0f*512.0f)
            continue;
        out[i].x = pb->x + dx * g_interp_alpha;
        out[i].y = pb->y + dy * g_interp_alpha;
        out[i].z = pb->z + dz * g_interp_alpha;
        /* half_size_x/y are NOT lerped (used to be, see git history): they're
         * driven by the current animation frame's atlas bounding box, a
         * discrete per-pose value, not a continuous physical quantity — poses
         * with a big frame-to-frame bbox swing (e.g. arms out vs in) smoothly
         * MORPHED between the two sizes every turn transition, visible as a
         * breathing/bounce artifact. The original reason for lerping this
         * (shadow ground_y/footprint derived from half_size_y) no longer
         * applies now that ground_y uses the true anchor Y directly (see
         * anchor_ratio_y in hwr_scene_source.h) instead of half_size_y. */
    }
    return n;
}

static const uint8_t *sw_get_palette(void *ctx)
{
    (void)ctx;
    return (const uint8_t *)display_palette;
}

/* Pages 4 and 5 host FLIC-animated content (billboard / equipment / cyborg
 * playback, see anim_type_get_output_buffer in game.c) and are redecoded into
 * vec_tmap[] every game tick, unlike the other 16 pages of static art. */
#define HWR_TMAP_ANIM_PAGE0 4
#define HWR_TMAP_ANIM_PAGE1 5
/* Page 0 is also mutated every tick while raining: water_droplets_on_floor
 * (enginpeff.c) paints ripple/splash pixels straight into vec_tmap[0] as a
 * floor-texture animation, the same trick the original SW renderer used for
 * puddle ripples. It needs the same per-frame refresh as the FLIC pages or
 * the GPU copy stays frozen at its initial load and the floor never shows
 * rain splashes. */
#define HWR_TMAP_RAIN_PAGE 0

static int sw_get_texture_pages(void *ctx, HwrTexturePages *out)
{
    int p;
    (void)ctx;
    if (out == NULL)
        return -1;
    /* Pack the 18 indexed pages contiguously once; most are static art. */
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
    } else {
        /* Refresh the animated pages every frame so FLIC playback and the
         * rain-ripple page reach the GPU texture array (see fl_upload_pages'
         * sub-image refresh). */
        if (vec_tmap[HWR_TMAP_ANIM_PAGE0] != NULL)
            memcpy(floor_pages + HWR_TMAP_ANIM_PAGE0 * (HWR_TMAP_DIM * HWR_TMAP_DIM),
                vec_tmap[HWR_TMAP_ANIM_PAGE0], HWR_TMAP_DIM * HWR_TMAP_DIM);
        if (vec_tmap[HWR_TMAP_ANIM_PAGE1] != NULL)
            memcpy(floor_pages + HWR_TMAP_ANIM_PAGE1 * (HWR_TMAP_DIM * HWR_TMAP_DIM),
                vec_tmap[HWR_TMAP_ANIM_PAGE1], HWR_TMAP_DIM * HWR_TMAP_DIM);
        if (vec_tmap[HWR_TMAP_RAIN_PAGE] != NULL)
            memcpy(floor_pages + HWR_TMAP_RAIN_PAGE * (HWR_TMAP_DIM * HWR_TMAP_DIM),
                vec_tmap[HWR_TMAP_RAIN_PAGE], HWR_TMAP_DIM * HWR_TMAP_DIM);
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

/* Collect the flat-tinted special-face overlay quads (shield-hit spheres, blast
 * rings, lightning slices) the SW build enlisted as DrIT_SpObFace4 with render
 * mode (Flags) 15. They carry already-projected screen points and a palette
 * colour (ExCol); we hand them to the GL overlay pass as screen-space quads. The
 * matching SW draw is suppressed under FX3D (drawitem_is_suppressed_glare). */
/* Composite the 4 corner bracket sprites (pop1_sprites) into one box tile in the
 * atlas, on a transparent background, and return its slot. variant 0 = person
 * (sprites 78/79/80/81), 1 = vehicle (84/85/86/87); corner order TL,TR,BL,BR.
 * Cached by a reserved key. *out_dim = the (square) tile size. */
static int hwr_targetbox_slot(int variant, int *out_dim)
{
    enum { TILE_MAX = 256 };
    static uint8_t tile[TILE_MAX * TILE_MAX * 4];
    static const uint16_t pv[2][4] = { {78, 79, 80, 81}, {84, 85, 86, 87} };
    uint32_t key = 0xFFE00000u | (uint32_t)variant;
    int slot, k, maxd = 0, TILE;
    *out_dim = 0;
    if (pop1_sprites == NULL || variant < 0 || variant > 1)
        return -1;
    slot = hwr_atlas_find(key);
    if (slot != -1) {
        int w, h; hwr_atlas_size(slot, &w, &h); *out_dim = w;
        return slot;   /* cached (>=0) or blacklisted (-2) */
    }
    /* Decode all four corners once, recording each one's tight *opaque* bounding
     * box (the visible bracket art may sit inset within the sprite's bounding box
     * with transparent padding — that padding is what was clipping/centring the
     * left column). We align the opaque bbox's OUTER corner to the tile's outer
     * corner, so the placement is immune to in-sprite padding. */
    {
        static uint8_t dec[4][64 * 64];     /* decoded indices per corner */
        static uint8_t dop[4][64 * 64];     /* decoded opacity per corner */
        int cw[4], ch[4];                   /* sprite bbox size */
        int bx0[4], by0[4], bw[4], bh[4];   /* tight opaque bbox */
        int ok[4] = {0,0,0,0};
        int maxbw = 0, gap;

        for (k = 0; k < 4; k++) {
            struct TbSprite *p = &pop1_sprites[pv[variant][k]];
            int w = p->SWidth, h = p->SHeight, row, col;
            int mnx = w, mny = h, mxx = -1, mxy = -1;
            cw[k] = w; ch[k] = h;
            if (w <= 0 || h <= 0 || w > 64 || h > 64)
                continue;
            memset(dec[k], 0, (size_t)w * h);
            memset(dop[k], 0, (size_t)w * h);
            if (hwr_rle_decode_opaque(p->Data, dec[k], dop[k], w, h) != 0)
                continue;
            for (row = 0; row < h; row++)
                for (col = 0; col < w; col++)
                    if (dop[k][row * w + col]) {
                        if (col < mnx) mnx = col;
                        if (col > mxx) mxx = col;
                        if (row < mny) mny = row;
                        if (row > mxy) mxy = row;
                    }
            if (mxx < 0) continue;          /* fully transparent */
            bx0[k] = mnx; by0[k] = mny;
            bw[k] = mxx - mnx + 1; bh[k] = mxy - mny + 1;
            if (bw[k] > maxbw) maxbw = bw[k];
            if (bh[k] > maxbw) maxbw = bh[k];
            ok[k] = 1;
        }
        if (maxbw < 2) return -1;

        /* Tile = two corner widths + a modest gap, mirroring the software box
         * whose middle gap is small relative to the brackets. A transparent
         * MARGIN around the content keeps the thin bracket arms off the atlas
         * tile boundary, so bilinear magnification doesn't bleed them into the
         * neighbouring atlas tile (which was clipping the left/top arms). */
        enum { MARGIN = 3 };
        gap = maxbw / 2; if (gap < 2) gap = 2;
        TILE = 2 * maxbw + gap + 2 * MARGIN;
        if (TILE > TILE_MAX) TILE = TILE_MAX;
        *out_dim = TILE;
        memset(tile, 0, (size_t)TILE * TILE * 4);

        for (k = 0; k < 4; k++) {
            int w = cw[k], row, col, ox, oy;
            if (!ok[k]) continue;
            /* Outer corner of the opaque bbox -> tile corner, inset by MARGIN. */
            ox = (k == 1 || k == 3) ? (TILE - MARGIN - bw[k]) : MARGIN;
            oy = (k == 2 || k == 3) ? (TILE - MARGIN - bh[k]) : MARGIN;
            for (row = 0; row < bh[k]; row++) {
                for (col = 0; col < bw[k]; col++) {
                    int si = (by0[k] + row) * w + (bx0[k] + col);
                    int dx = ox + col, dy = oy + row, di;
                    if (dx < 0 || dy < 0 || dx >= TILE || dy >= TILE) continue;
                    if (!dop[k][si]) continue;
                    di = (dy * TILE + dx) * 4;
                    tile[di + 0] = lbPaletteColors[dec[k][si]].r;
                    tile[di + 1] = lbPaletteColors[dec[k][si]].g;
                    tile[di + 2] = lbPaletteColors[dec[k][si]].b;
                    tile[di + 3] = 255;
                }
            }
        }
    }
    (void)maxd;
    return hwr_atlas_register(key, tile, TILE, TILE);
}

/* Reproduce transform_shpoint() (engintrns.c) in float using the interpolated
 * camera factors, giving the on-screen pixel position of a world anchor. dxc/dzc
 * are camera-relative (world minus the interpolated camera centre); dyc is the
 * frozen Y argument the software path used. No clamping — GL clips off-screen. */
static void hwr_ovreq_project(const HwrCamera *cam, int dxc, int dyc, int dzc,
    float *out_x, float *out_y, float *out_scrd)
{
    float fctr_a = (cam->d14 * dxc - cam->d10 * dzc) / 65536.0f;
    float fctr_b = (cam->d10 * dxc + cam->d14 * dzc) / 65536.0f;
    float fctr_c = (cam->d1c * dyc - cam->d18 * fctr_b) / 65536.0f;
    float scr_d  = (cam->d18 * dyc + cam->d1c * fctr_b) / 65536.0f;
    float sca_x = cam->scale * fctr_a;
    float sca_y = cam->scale * fctr_c;
    float scr_shx, scr_shy;
    if (cam->perspective == 5 && scr_d > (float)(0x4000 / 16))
        scr_d = 16384.0f * scr_d / (scr_d + 16384.0f);
    scr_shx = sca_x / 2048.0f;
    scr_shy = sca_y / 2048.0f;
    if (cam->perspective == 5) {
        scr_shx = scr_shx * (16384.0f - scr_d) / 16384.0f;
        scr_shy = scr_shy * (16384.0f - scr_d) / 16384.0f;
    }
    *out_x = cam->centre_x + scr_shx;
    *out_y = cam->centre_y - scr_shy;
    if (out_scrd)
        *out_scrd = scr_d;
}

/* Interpolate an overlay's world anchor (ax/az) between the previous and current
 * sim turn by g_interp_alpha, matched on `ident` (the source Thing), so a bar
 * tracks its moving vehicle smoothly rather than snapping once per 16Hz turn.
 * Falls back to the current anchor when there is no identity or no prev match. */
static void hwr_ovreq_interp_anchor(const struct HwrOverlayReq *r,
    float *out_ax, float *out_az)
{
    float a = g_interp_alpha;
    *out_ax = (float)r->ax;
    *out_az = (float)r->az;
    if (a >= 1.0f || r->ident == 0)
        return;
    if (a < 0.0f) a = 0.0f;
    {
        int i;
        for (i = 0; i < hwr_ovreq_prev_count; i++) {
            const struct HwrOverlayReq *p = &hwr_ovreq_prev[i];
            if (p->ident != r->ident || p->kind != r->kind)
                continue;
            /* Teleport guard: don't interpolate across a large jump (same 512²
             * world-unit threshold the sprite/vehicle interpolation uses). */
            {
                int ddx = r->ax - p->ax, ddz = r->az - p->az;
                if (ddx * ddx + ddz * ddz > 512 * 512)
                    return;
            }
            *out_ax = (float)p->ax + ((float)r->ax - (float)p->ax) * a;
            *out_az = (float)p->az + ((float)r->az - (float)p->az) * a;
            return;
        }
    }
}

/* Register (or fetch cached) an 8x6 RGBA atlas tile for one glyph `ch` baked in
 * palette colour `col`, using the software proportional font. Index 0 (unset
 * font pixels) becomes fully transparent; the ovt shader alpha-tests it out. */
static int hwr_glyph_slot(unsigned char ch, unsigned char col)
{
    enum { GW = 8, GH = 6 };
    uint32_t key = 0xFFD00000u | ((uint32_t)col << 8) | (uint32_t)ch;
    int slot = hwr_atlas_find(key);
    unsigned char idx[GW * GH];
    unsigned char tile[GW * GH * 4];
    char s[2];
    int i;
    if (slot != -1)
        return slot;   /* cached (>=0) or blacklisted (-2) */
    if (ch < 32)
        return -1;
    memset(idx, 0, sizeof(idx));
    s[0] = (char)ch; s[1] = '\0';
    prop_text(s, idx, GW, col);
    for (i = 0; i < GW * GH; i++) {
        unsigned char p = idx[i];
        if (p != 0) {
            tile[i * 4 + 0] = lbPaletteColors[p].r;
            tile[i * 4 + 1] = lbPaletteColors[p].g;
            tile[i * 4 + 2] = lbPaletteColors[p].b;
            tile[i * 4 + 3] = 255;
        } else {
            tile[i * 4 + 0] = 0; tile[i * 4 + 1] = 0;
            tile[i * 4 + 2] = 0; tile[i * 4 + 3] = 0;
        }
    }
    return hwr_atlas_register(key, tile, GW, GH);
}

/* Emit a string as a run of per-glyph textured overlay quads starting at screen
 * (px,py), advancing by each glyph's proportional width. Returns the new quad
 * count. Glyphs are drawn at the software font's native pixel size (unscaled),
 * matching the SW draw_text look. */
static int hwr_emit_text_quads(HwrOverlayQuad *out, int n, int max,
    const char *s, unsigned char col, float px, float py)
{
    enum { GW = 8, GH = 6 };
    const char *c;
    for (c = s; *c != '\0'; c++) {
        unsigned char ch = (unsigned char)*c;
        int slot;
        if (ch >= 32) {
            slot = hwr_glyph_slot(ch, col);
            if (slot >= 0 && n < max) {
                float u0, v0, u1, v1;
                HwrOverlayQuad *q = &out[n++];
                hwr_atlas_uv(slot, &u0, &v0, &u1, &v1);
                q->x[0] = px;        q->y[0] = py;        /* TL */
                q->x[1] = px + GW;   q->y[1] = py;        /* TR */
                q->x[2] = px + GW;   q->y[2] = py + GH;   /* BR */
                q->x[3] = px;        q->y[3] = py + GH;   /* BL */
                q->u0 = u0; q->v0 = v0; q->u1 = u1; q->v1 = v1;
                q->slot = slot;
                q->r = q->g = q->b = 1.0f;
                q->a = 1.0f;
            }
            px += (float)font[8 * ((ch - 32) & 0xFF) + 6];
        } else {
            px += 4.0f;   /* space/control: nominal advance */
        }
    }
    return n;
}

/* Emit one flat coloured overlay quad (screen rect x,y,w,h in pixels). */
static int hwr_emit_box_quad(HwrOverlayQuad *out, int n, int max,
    float x, float y, float w, float h, unsigned char col)
{
    HwrOverlayQuad *q;
    if (w <= 0.0f || h <= 0.0f || n >= max)
        return n;
    q = &out[n++];
    q->x[0] = x;     q->y[0] = y;
    q->x[1] = x + w; q->y[1] = y;
    q->x[2] = x + w; q->y[2] = y + h;
    q->x[3] = x;     q->y[3] = y + h;
    q->r = lbPaletteColors[col].r / 255.0f;
    q->g = lbPaletteColors[col].g / 255.0f;
    q->b = lbPaletteColors[col].b / 255.0f;
    q->a = 1.0f;
    q->slot = -1;
    return n;
}

/* Re-project and emit the world-anchored HUD overlays (numbers/tags/bars). */
static int hwr_emit_overlay_reqs(HwrOverlayQuad *out, int n, int max)
{
    HwrCamera cam;
    int i;
    if (sw_get_camera(NULL, &cam) != 0)
        return n;
    for (i = 0; i < hwr_overlay_req_count && n < max; i++) {
        const struct HwrOverlayReq *r = &hwr_overlay_req[i];
        float ax, az, sx, sy;
        hwr_ovreq_interp_anchor(r, &ax, &az);
        hwr_ovreq_project(&cam, (int)(ax - cam.cx), r->dyc, (int)(az - cam.cz),
            &sx, &sy, NULL);
        sx += r->scr_dx;
        sy += r->scr_dy;
        if (r->kind == HwrOvReq_Bar) {
            /* Reproduce draw_horiz_level_bar (engindrwlstx_spr.c): a background
             * box plus an inset level box, sized by the live zoom (cam.scale). */
            int scale = (int)cam.scale;
            int max_lvl = r->ival2 <= 0 ? 1 : r->ival2;
            int lvl = r->ival;
            int bar_w = (44 * scale) >> 8;
            int bar_h = (5 * scale) >> 8;
            int range_w = ((44 * 15 / 16) * scale) >> 8;
            int range_h = ((5 * 12 / 16) * scale) >> 8;
            int level_x;
            if (lvl < 0) lvl = 0;
            else if (lvl > max_lvl) lvl = max_lvl + 1;
            if (bar_w & 1) range_w |= 1; else range_w &= ~1;
            if (bar_h & 1) range_h |= 1; else range_h &= ~1;
            n = hwr_emit_box_quad(out, n, max, sx - bar_w / 2.0f, sy,
                (float)bar_w, (float)bar_h, r->col2);
            level_x = range_w * lvl / max_lvl;
            n = hwr_emit_box_quad(out, n, max, sx - bar_w / 2.0f,
                sy + (bar_h - range_h) / 2.0f, (float)level_x, (float)range_h,
                r->col);
        } else if (r->kind == HwrOvReq_Number) {
            char locstr[16];
            sprintf(locstr, "%d", r->ival);
            n = hwr_emit_text_quads(out, n, max, locstr, r->col, sx, sy);
        } else if (r->kind == HwrOvReq_Frame) {
            /* Agent selection number (number_player): a HUD sprite frame drawn at
             * the agent's re-projected screen position. Composite the frame into
             * one atlas tile and emit a single textured quad. Element positions
             * scale by cam.scale/256 (matching draw_frame_on_map_coords' >>9 on
             * el.X); sprite size is native when the SW path was unscaled, else
             * scaled by cam.scale/512 (matching draw_hud_frame_on_screen's >>9). */
            int fw = 0, fh = 0, offx = 0, offy = 0;
            int slot = hwr_effect_frame_slot((unsigned short)r->ival, &fw, &fh,
                &offx, &offy);
            if (slot >= 0 && n < max) {
                float pscale = cam.scale / 256.0f;
                float sf = r->ival2 ? 1.0f : cam.scale / 512.0f;
                float x0 = sx + offx * pscale;
                float y0 = sy + offy * pscale;
                float w = fw * sf, h = fh * sf;
                float u0, v0, u1, v1;
                HwrOverlayQuad *q = &out[n++];
                hwr_atlas_uv(slot, &u0, &v0, &u1, &v1);
                q->x[0] = x0;     q->y[0] = y0;
                q->x[1] = x0 + w; q->y[1] = y0;
                q->x[2] = x0 + w; q->y[2] = y0 + h;
                q->x[3] = x0;     q->y[3] = y0 + h;
                q->u0 = u0; q->v0 = v0; q->u1 = u1; q->v1 = v1;
                q->slot = slot;
                q->r = q->g = q->b = 1.0f;
                q->a = 1.0f;
            }
        } else { /* HwrOvReq_Text */
            char locstr[9];
            memcpy(locstr, r->text, 8);
            locstr[8] = '\0';
            n = hwr_emit_text_quads(out, n, max, locstr, r->col, sx, sy);
        }
    }
    return n;
}

/* Build the depth-tested weapon beams as a clip-space triangle list. Each segment
 * becomes a thick screen-space quad (2 triangles) with per-endpoint depth so it is
 * occluded by 3D geometry. World beams (laser) re-project with the live camera
 * every frame (60fps); screen beams (electric zag) use their captured 16Hz screen
 * position + depth. */
static int sw_get_beams(void *ctx, HwrBeamVertex *out, int max)
{
    HwrCamera cam;
    int i, n = 0;
    float cx, cy;
    (void)ctx;
    if (out == NULL || max < 6 || hwr_beam_count <= 0)
        return 0;
    if (sw_get_camera(NULL, &cam) != 0)
        return 0;
    cx = cam.centre_x; cy = cam.centre_y;
    if (cx <= 0.0f || cy <= 0.0f)
        return 0;
    for (i = 0; i < hwr_beam_count && n + 6 <= max; i++) {
        const struct HwrBeamSeg *b = &hwr_beam_list[i];
        float x1, y1, d1, x2, y2, d2, dx, dy, len, px, py, hw;
        float r, g, bl;
        float p1ax, p1ay, p1bx, p1by, p2ax, p2ay, p2bx, p2by;
        float z1, z2;
        if (b->world) {
            hwr_ovreq_project(&cam, b->a[0] - (int)cam.cx, b->a[1],
                b->a[2] - (int)cam.cz, &x1, &y1, &d1);
            hwr_ovreq_project(&cam, b->b[0] - (int)cam.cx, b->b[1],
                b->b[2] - (int)cam.cz, &x2, &y2, &d2);
            hw = (float)b->thick * (cam.scale / 256.0f);
        } else {
            x1 = (float)b->a[0]; y1 = (float)b->a[1]; d1 = (float)b->a[2];
            x2 = (float)b->b[0]; y2 = (float)b->b[1]; d2 = (float)b->b[2];
            hw = (float)b->thick;
        }
        if (hw < 1.0f) hw = 1.0f;
        dx = x2 - x1; dy = y2 - y1;
        len = sqrtf(dx * dx + dy * dy);
        if (len < 0.001f)
            continue;
        px = -dy / len * hw; py = dx / len * hw;
        r = (float)lbPaletteColors[b->col].r / 255.0f;
        g = (float)lbPaletteColors[b->col].g / 255.0f;
        bl = (float)lbPaletteColors[b->col].b / 255.0f;
        /* /65536: shared linear-depth -> NDC scale (see floor_vert_src ndc_z).
         * Must match every other pass writing this depth buffer. */
        z1 = d1 / 65536.0f; if (z1 < -1.0f) z1 = -1.0f; if (z1 > 1.0f) z1 = 1.0f;
        z2 = d2 / 65536.0f; if (z2 < -1.0f) z2 = -1.0f; if (z2 > 1.0f) z2 = 1.0f;
        /* Corner screen positions -> NDC. */
        p1ax = (x1 + px) / cx - 1.0f; p1ay = 1.0f - (y1 + py) / cy;
        p1bx = (x1 - px) / cx - 1.0f; p1by = 1.0f - (y1 - py) / cy;
        p2ax = (x2 + px) / cx - 1.0f; p2ay = 1.0f - (y2 + py) / cy;
        p2bx = (x2 - px) / cx - 1.0f; p2by = 1.0f - (y2 - py) / cy;
#define HWR_BEAM_V(vx, vy, vz) do { \
            HwrBeamVertex *v = &out[n++]; \
            v->x = (vx); v->y = (vy); v->z = (vz); \
            v->r = r; v->g = g; v->b = bl; v->a = 1.0f; } while (0)
        HWR_BEAM_V(p1ax, p1ay, z1);
        HWR_BEAM_V(p2ax, p2ay, z2);
        HWR_BEAM_V(p2bx, p2by, z2);
        HWR_BEAM_V(p1ax, p1ay, z1);
        HWR_BEAM_V(p2bx, p2by, z2);
        HWR_BEAM_V(p1bx, p1by, z1);
#undef HWR_BEAM_V
    }
    return n;
}

static int sw_get_overlays(void *ctx, HwrOverlayQuad *out, int max)
{
    int n = hwr_collected_overlay_count;
    int i;
    (void)ctx;
    if (out == NULL || max <= 0)
        return 0;
    if (n > max) n = max;
    memcpy(out, hwr_collected_overlays, (size_t)n * sizeof(HwrOverlayQuad));

    /* Pause popup box fills: semi-transparent purple rects over the 3D scene.
     * The SW fill is replaced by key (transparent) in draw_box_cutedge; these
     * GL quads provide the tinted background while controls stay solid. */
    for (i = 0; i < hwr_pause_box_count && n + 1 <= max; i++) {
        HwrOverlayQuad *q = &out[n++];
        float x0 = (float)hwr_pause_box_list[i].x0;
        float y0 = (float)hwr_pause_box_list[i].y0;
        float x1 = (float)hwr_pause_box_list[i].x1;
        float y1 = (float)hwr_pause_box_list[i].y1;
        uint8_t ci = hwr_pause_box_list[i].colr;
        q->x[0] = x0; q->y[0] = y0;
        q->x[1] = x1; q->y[1] = y0;
        q->x[2] = x1; q->y[2] = y1;
        q->x[3] = x0; q->y[3] = y1;
        q->r = lbPaletteColors[ci].r / 255.0f;
        q->g = lbPaletteColors[ci].g / 255.0f;
        q->b = lbPaletteColors[ci].b / 255.0f;
        q->a = 0.55f;
        q->slot = -1;
    }

    /* Append the target boxes as a single translucent textured quad each (the
     * pre-composited bracket box tile), scaled to the box size and centred on the
     * target. One tile drawn as one quad — no per-corner alignment/bleed. */
    for (i = 0; i < hwr_tgtbox_count && n + 1 <= max; i++) {
        int dim = 64;
        int slot = hwr_targetbox_slot(hwr_tgtbox_list[i].variant, &dim);
        float cx, cy, h, x0, y0, x1, y1, u0, v0, u1, v1;
        HwrOverlayQuad *q;
        if (slot < 0)
            continue;
        cx = (float)hwr_tgtbox_list[i].cx;
        cy = (float)hwr_tgtbox_list[i].cy;
        h  = (float)hwr_tgtbox_list[i].half;
        x0 = cx - h; y0 = cy - h; x1 = cx + h; y1 = cy + h;
        hwr_atlas_uv(slot, &u0, &v0, &u1, &v1);
        /* Inset by half a texel so bilinear magnification never samples across
         * the tile's atlas sub-rect edge into a neighbouring tile. */
        if (dim > 0) {
            float ht = 0.5f * (u1 - u0) / (float)dim;
            u0 += ht; u1 -= ht; v0 += ht; v1 -= ht;
        }
        q = &out[n++];
        q->x[0] = x0; q->y[0] = y0;   /* TL */
        q->x[1] = x1; q->y[1] = y0;   /* TR */
        q->x[2] = x1; q->y[2] = y1;   /* BR */
        q->x[3] = x0; q->y[3] = y1;   /* BL */
        q->u0 = u0; q->v0 = v0; q->u1 = u1; q->v1 = v1;
        q->slot = slot;
        q->r = q->g = q->b = 1.0f;
        q->a = 0.5f;   /* 50% transparent */
    }

    /* World-anchored HUD overlays (numbers over heads, short tags, vehicle health
     * bars), re-projected with the interpolated camera for full-frame-rate motion. */
    n = hwr_emit_overlay_reqs(out, n, max);
    return n;
}

static HwrSceneSource sw_source = {
    NULL,           /* ctx */
    NULL,           /* begin_frame */
    sw_get_camera,
    sw_get_floor,
    sw_get_faces,
    sw_get_reflect_faces,
    sw_get_transparent_faces,
    sw_get_lights,
    sw_get_sprites,
    sw_get_palette,
    sw_get_texture_pages,
    sw_get_key_index,
    sw_get_overlays,
    sw_get_beams,
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
