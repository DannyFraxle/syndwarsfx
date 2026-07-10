/******************************************************************************/
// Syndicate Wars Fan Expansion, source port of the classic game from Bullfrog.
/******************************************************************************/
/** @file engindrwlstx.h
 *     Header file for engindrwlstx.c.
 * @par Purpose:
 *     Drawlists execution for the 3D engine.
 * @par Comment:
 *     Just a header file - #defines, typedefs, function prototypes etc.
 * @author   Tomasz Lis
 * @date     22 Apr 2024 - 12 May 2024
 * @par  Copying and copyrights:
 *     This program is free software; you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of the License, or
 *     (at your option) any later version.
 */
/******************************************************************************/
#ifndef ENGINDRWLSTX_H
#define ENGINDRWLSTX_H

#include "bftypes.h"

#ifdef __cplusplus
extern "C" {
#endif
/******************************************************************************/

#pragma pack(1)

struct DrawItem {
    ubyte Type;
    ushort Offset;
    ushort Child;
};

struct SpecialPoint {
    short X;
    short Y;
    short Z;
    short PadTo8;
};

struct SortSprite {
    short X;
    short Y;
    short Z;
    ushort Frame;
    intptr_t SrcItem;
    ubyte Brightness;
    ubyte Angle;
    short Scale;
};

struct SortLine {
    short X1;
    short Y1;
    short X2;
    short Y2;
    ubyte Col;
    ubyte Shade;
    ubyte Flags;
};

/** Map coordinates storage used when handling drawlists.
 *
 * The game should have its own types for map coordinates, this one is for
 * the rendering only.
 */
struct SortMapPoint {
    s32 X;
    s32 Y;
    s32 Z;
};

struct TbSprite;
struct PolyPoint;

typedef void (*ScreenTriangleRenderCallback)(
  struct PolyPoint *p_pt1,
  struct PolyPoint *p_pt2,
  struct PolyPoint *p_pt3,
  ushort face, ubyte type);
typedef void (*ScreenSortSpriteRenderCallback)(ushort sspr);

#pragma pack()
/******************************************************************************/
extern struct DrawItem *game_draw_list;
extern struct DrawItem *p_current_draw_item;
extern ushort next_draw_item;

/** Array of triangular faces valid only as part of drawlist for a single frame.
 */
extern struct SingleObjectFace3 *game_special_obj_faces3;
extern ushort next_special_obj_face3;

/** Array of rectangular faces valid only as part of drawlist for a single frame.
 */
extern struct SingleObjectFace4 *game_special_obj_faces4;
extern ushort next_special_obj_face4;

/** Array of points with items valid only as part of drawlist for a single frame.
 */
extern struct SpecialPoint *game_screen_point_pool;
extern ushort next_screen_point;

extern struct SortSprite *game_sort_sprites;
extern struct SortSprite *p_current_sort_sprite;
extern ushort next_sort_sprite;

extern struct SortLine *game_sort_lines;
extern struct SortLine *p_current_sort_line;
extern ushort next_sort_line;

extern TbPixel face_transp_tinted_surface_col;
extern TbPixel face_transp_tinted_line_col;

extern ubyte engine_render_lights;

/* When nonzero, opaque object/building faces are skipped during drawlist
 * execution so the FX3D hardware renderer can draw them as 3D geometry. */
extern int engine_hwr_suppress_faces;

/* When nonzero, Thing-based sprite draw items (persons, statics, vehicles)
 * are skipped during drawlist execution so the FX3D hardware renderer can
 * draw them as camera-facing billboards. The hwr_sprite_skip_mask bitset
 * identifies which SortSprite indices were collected. */
extern int engine_hwr_suppress_sprites;
extern unsigned char hwr_sprite_skip_mask[512];

/* When nonzero, the SW pixel-block rain/snow post-effect (enginpeff.c) is
 * skipped so the FX3D hardware renderer can draw weather as its own
 * alpha-blended GL overlay instead of opaque WScreen pixels. */
extern int engine_hwr_suppress_rain;

/* FX3D Phase 8: bitset of object indices flagged semi-transparent (deep-radar
 * see-through) this frame. Set during drawlist build in draw_object(); read by
 * the FX3D scene source to route those objects' faces into the blended
 * transparent pass. Indexed by object index (game_objects[]). */
extern unsigned char hwr_obj_transp_mask[8192];

/* FX3D: bitset of object indices the SW build actually drew this frame (set in
 * draw_object). The FX3D scene source gates its static-object faces on this so
 * GL stops drawing buildings the SW engine no longer traverses (destroyed). */
extern unsigned char hwr_obj_live_mask[8192];

/* FX3D: light-glare (headlight / lamp) world positions enlisted this frame by
 * build_glare(), drawn by the FX3D renderer as additive glow billboards. */
#define HWR_GLARE_MAX 512
/* siren: CarGlare.Flag of this glare — 0 = plain (headlight / lamp), 1 and 2 =
 * the two police roof siren lights (drawn red / blue and flashed by FX3D). */
struct HwrGlare { int x, y, z, r, siren; };
extern struct HwrGlare hwr_glare_list[HWR_GLARE_MAX];
extern int hwr_glare_count;

/* Per-vehicle CarGlare.Flag sequence for the glares about to be enlisted by the
 * next do_car_glare() call. build_vehicle() fills this from the vehicle's
 * car_glare range; build_glare() consumes one flag per call (in order) so each
 * recorded glare carries its Flag. flag_n = 0 means "no capture" (plain glares,
 * e.g. street lamps). */
#define HWR_GLARE_FLAG_MAX 64
extern int hwr_glare_flag_seq[HWR_GLARE_FLAG_MAX];
extern int hwr_glare_flag_n;
extern int hwr_glare_flag_pos;

/* FX3D: object-model ground-shadow decals (draw_object_model_shadow - the
 * angled silhouette shadows cast by matrix'd objects: buildings/temples,
 * vehicles). Captured during the drawlist build as world-space quads with
 * their page-4 shadow-texture rect; the FX3D renderer draws them as blended
 * dark decals on the floor. Corners are a ring (1,2,3,4). */
#define HWR_MODEL_SHADOW_MAX 128
struct HwrModelShadow {
    int x[4], y[4], z[4];          /* absolute world corners (y = 8*alt space) */
    unsigned char u1, v1, u2, v2;  /* page-4 texture rect (X1,Y1)-(X2,Y2) */
};
extern struct HwrModelShadow hwr_model_shadow_list[HWR_MODEL_SHADOW_MAX];
extern int hwr_model_shadow_count;

extern short word_1A5834;
extern short word_1A5836;

extern ScreenTriangleRenderCallback screen_position_face_render_cb;
extern ScreenSortSpriteRenderCallback screen_sorted_sprite_statc_render_cb;
extern ScreenSortSpriteRenderCallback screen_sorted_sprite_persn_render_cb;

/** FX3D: run a sprite's mouse-pick callback without drawing it (used when the
 * SW sprite draw is suppressed because the sprite is rendered as a HW billboard). */
void hwr_run_sprite_pick(ushort sspr, ubyte ditype);
/******************************************************************************/

void draw_frame_scaled_alpha(int scr_x, int scr_y, ushort frm,
  ushort scale, ushort alpha);
void draw_sorted_sprite1a(ushort frm, short x, short y, ubyte csel);
void draw_sort_sprite1a(ushort sspr);

void draw_floor_tile1a(ushort tl);
void draw_floor_tile1b(ushort tl);

void set_nuclear_shade_point(s32 x, s32 y, s32 z);
void set_nuclear_shade_timer(ulong tmval);

void draw_drawitem_1(ushort dihead);
void draw_drawitem_2(ushort dihead);

/******************************************************************************/
#ifdef __cplusplus
}
#endif
#endif
