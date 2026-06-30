/******************************************************************************/
// Syndicate Wars Fan Expansion, source port of the classic game from Bullfrog.
/******************************************************************************/
/** @file engindrwlstx.c
 *     Drawlists execution for the 3D engine.
 * @par Purpose:
 *     Implements functions for executing previously made drawlists,
 *     meaning the actual drawing based on primitives in the list.
 * @par Comment:
 *     None.
 * @author   Tomasz Lis
 * @date     22 Apr 2024 - 12 May 2024
 * @par  Copying and copyrights:
 *     This program is free software; you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of the License, or
 *     (at your option) any later version.
 */
/******************************************************************************/
#include "engindrwlstx.h"

#include <assert.h>
#include <string.h>

#include "enginbckt.h"
#include "enginfloor.h"
#include "enginshadws.h"
#include "enginsngobjs.h"
#include "enginsngtxtr.h"
/******************************************************************************/

ushort tnext_draw_item = 0;

ushort tnext_sort_sprite = 0;
//extern ushort tnext_sort_line; -- no such var?
//extern ushort tnext_special_obj_face3;
ushort tnext_special_obj_face4 = 1;

ushort tnext_screen_point = 0;

/* When nonzero, the opaque object/building face draw-item types are skipped
 * during drawlist execution. Set by the FX3D hardware renderer (which draws
 * those faces as 3D geometry instead); the keyed gaps let the GL scene show
 * through. Reflective/transparent faces, sprites, objects and HUD still draw
 * in software. Always 0 in a software-only build. */
int engine_hwr_suppress_faces = 0;

/* When nonzero, Thing-based sprite draw items are skipped during drawlist
 * execution so the FX3D renderer can draw them as camera-facing billboards. */
int engine_hwr_suppress_sprites = 0;
/* 512 bytes = 4096 bits: game_sort_sprites holds up to 4001 entries, so a
 * 256-byte (2048-bit) mask overflowed when smoke pushed the sort-sprite count
 * past 2048 — high-index sprites couldn't be suppressed (software smoke drew on
 * top of the GL billboards) and the collection wrote out of bounds. */
unsigned char hwr_sprite_skip_mask[512] = {0};

/* Per-effect skip masks for the dedicated fire/phwoar arrays (separate index
 * namespaces from the Thing sort-sprites). A set bit means the FX3D renderer
 * collected that effect as a translucent billboard, so the SW draw is skipped.
 * 512 flames -> 64 bytes, 1024 phwoar -> 128 bytes. Set by the FX3D scene
 * source, cleared each frame alongside hwr_sprite_skip_mask. */
unsigned char hwr_fire_skip_mask[64] = {0};
unsigned char hwr_phwoar_skip_mask[128] = {0};

/* FX3D Phase 8: bitset of object indices the SW engine decided are
 * semi-transparent this frame (deep-radar see-through buildings). Set in
 * draw_object() during drawlist build (which always runs, even when face draw
 * is suppressed) and read by the FX3D scene source to route those objects'
 * faces into the blended transparent pass. Cleared each frame in
 * reset_drawlist(). 8192 bytes covers all 16-bit object indices. */
unsigned char hwr_obj_transp_mask[8192] = {0};

/* FX3D: bitset of object indices the SW drawlist build actually drew this frame
 * (draw_object set the bit). The GL face emitter brute-forces every entry in
 * game_objects[], which would keep drawing buildings the SW engine has stopped
 * traversing (destroyed/collapsed buildings still linger in the array). Gating
 * the GL static-object faces on this mask makes GL match SW exactly. Set in
 * draw_object during the build; cleared once per frame in process_engine_unk3
 * (game.c) alongside hwr_obj_transp_mask. Keyed by game_objects[] index. */
unsigned char hwr_obj_live_mask[8192] = {0};

/* FX3D: world-space positions of the light "glares" (car headlights / street
 * lamps) enlisted this frame by build_glare(). These are screen-space additive
 * quads in SW (special-obj-face4 mode 9), which the GL emitter can't reuse, so
 * build_glare records the lamp world pos + radius here and the FX3D scene source
 * draws them as additive glow billboards instead. Cleared at frame start in
 * process_engine_unk3 (game.c); the SW draw of these glare faces is suppressed
 * under FX3D (see drawitem_is_suppressed_glare). */
struct HwrGlare hwr_glare_list[HWR_GLARE_MAX];
int hwr_glare_count = 0;

/* CarGlare.Flag sequence for the current do_car_glare() (see header). */
int hwr_glare_flag_seq[HWR_GLARE_FLAG_MAX];
int hwr_glare_flag_n = 0;
int hwr_glare_flag_pos = 0;

/* True for the opaque face draw-item types the FX3D renderer takes over. */
static TbBool drawitem_is_suppressed_face(ubyte type)
{
    if (!engine_hwr_suppress_faces)
        return false;
    switch (type)
    {
    /* Standard object/building faces, drawn as 3D geometry by FX3D (they live
     * in game_object_faces3/4). Special faces (DrIT_SpObFace4) come from a
     * separate runtime-built array the GL emitter does not read, so they are
     * left to the software renderer to avoid leaving holes. */
    case DrIT_ObFace3Txtr:
    case DrIT_Unkn10:
    case DrIT_ObFace4Txtr:
    case DrIT_ObFace3G:
    case DrIT_ObFace4G:
    case DrIT_ObFacePole:
    /* Reflective ("chameleon" paint) faces are drawn by the FX3D chameleon pass
     * as proper depth-tested geometry, so suppress the SW projected reflective
     * draw to avoid double-drawing them over the 3D scene. */
    case DrIT_ObFace3Refl:
    case DrIT_ObFace4Refl:
    /* Semi-transparent (deep-radar see-through) faces are drawn by the FX3D
     * blended transparent pass; suppress the SW tinted draw so it doesn't
     * paint over the 3D scene. */
    case DrIT_ObFace3Tran:
    case DrIT_ObFace4Tran:
        return true;
    default:
        return false;
    }
}

/* Returns true when the draw item is a Thing-based sprite that the FX3D
 * hardware renderer has collected for billboard rendering. The check uses
 * the hwr_sprite_skip_mask bitset indexed by the SortSprite offset. */
static TbBool drawitem_is_suppressed_sprite(const struct DrawItem *itm)
{
    if (!engine_hwr_suppress_sprites)
        return false;
    ushort ss_idx = itm->Offset;
    if (hwr_sprite_skip_mask[ss_idx >> 3] & (1 << (ss_idx & 7)))
        return true;
    return false;
}

/* Returns true when the draw item is a light "glare" (headlight / lamp) special
 * face the FX3D renderer draws as an additive glow billboard instead. Glares are
 * special-obj-face4 entries with mode (Flags) 9 or 10 (set by build_glare). */
static TbBool drawitem_is_suppressed_glare(const struct DrawItem *itm)
{
    if (!engine_hwr_suppress_faces)
        return false;
    if (itm->Type != DrIT_SpObFace4 || game_special_obj_faces4 == NULL)
        return false;
    {
        ubyte fl = game_special_obj_faces4[itm->Offset].Flags;
        /* 9/10 = light glares (GL additive billboards); 15 = tinted slices
         * (laser/lightning) and 17 = shaded circle fans (shield-hit / blast /
         * recoil / nuclear discs) -> GL overlay quads. */
        return (fl == 9 || fl == 10 || fl == 15 || fl == 17);
    }
}

/* Returns true when the draw item is a fire/phwoar effect the FX3D renderer has
 * collected as a translucent billboard (per-effect skip masks, separate index
 * namespaces from the Thing sort-sprites). */
static TbBool drawitem_is_suppressed_effect(const struct DrawItem *itm)
{
    ushort idx;
    if (!engine_hwr_suppress_sprites)
        return false;
    idx = itm->Offset;
    if (itm->Type == DrIT_SFireFlame && idx < 512)
        return (hwr_fire_skip_mask[idx >> 3] & (1 << (idx & 7))) != 0;
    if (itm->Type == DrIT_SFrmPhwoar && idx < 1024)
        return (hwr_phwoar_skip_mask[idx >> 3] & (1 << (idx & 7))) != 0;
    return false;
}

/******************************************************************************/
// from engindrwlstx_spr
void draw_sort_line1a(ushort sln);
void draw_sort_sprite1c(ushort sspr);
void draw_hud_frame_on_screen_unscaled_but_scale_pos(short scr_x, short scr_y, ushort frm, int sscale);
void draw_hud_frame_on_screen(short scr_x, short scr_y, ushort frm, int sscale);
void draw_sort_sprite_frame_pers_v(int sspr);
void draw_sort_sprite_frame_pers_b(int sspr);
void draw_sort_sprite_frame_efct_v(int sspr);
void draw_phwoar(ushort ph);
void draw_sort_sprite_long_prop_bar(short sspr);
void draw_sort_sprite_number(ushort sspr);
void draw_sort_sprite_short_text(ushort sspr);
void draw_fire_flame(ushort flm);
// from engindrwlstx_fac
void set_face_texture_uv(ushort stex, struct PolyPoint *p_pt1,
  struct PolyPoint *p_pt2, struct PolyPoint *p_pt3, ubyte gflags);
void set_floor_texture_uv(ushort sftex, struct PolyPoint *p_pt1, struct PolyPoint *p_pt2,
  struct PolyPoint *p_pt3, struct PolyPoint *p_pt4, ubyte gflags);
void set_floor_texture_uv_damaged_ground(struct PolyPoint *p_pt1,
  struct PolyPoint *p_pt2, struct PolyPoint *p_pt3, struct PolyPoint *p_pt4, ubyte neighbrs);
void draw_object_face4g_textrd(ushort face4);
void draw_object_face3_reflect(ushort face3);
void draw_object_face4_reflect(ushort face4);
void draw_object_face3g_textrd(ushort face);
void draw_object_face4d_textrd_dk(ushort face4);
void draw_ex_face(ushort exface);
void draw_special_object_face4(ushort face4);
void draw_object_face4_pole(ushort face4);
void draw_object_face4d_textrd(ushort face4);
void draw_object_face3d_textrd(ushort face);
void draw_object_face3d_textrd_dk(ushort face);
void draw_object_face3_tran_tint(ushort face);
void draw_object_face4_tran_tint(ushort face4);
void draw_shrapnel(ushort shrap);

void reset_drawlist(void)
{
    tnext_screen_point = next_screen_point;
    next_screen_point = 0;

    next_sort_line = 0;
    p_current_sort_line = &game_sort_lines[next_sort_line];

    tnext_draw_item = next_draw_item;
    next_draw_item = 1;
    p_current_draw_item = &game_draw_list[next_draw_item];

    tnext_sort_sprite = next_sort_sprite;
    next_sort_sprite = 0;
    p_current_sort_sprite = &game_sort_sprites[next_sort_sprite];

    next_special_obj_face3 = 1;

    tnext_special_obj_face4 = next_special_obj_face4;
    next_special_obj_face4 = 1;

    tnext_floor_texture = next_floor_texture;

    next_floor_tile = 1;
    /* NOTE: the FX3D deep-radar transparent-object mask (hwr_obj_transp_mask) is
     * NOT cleared here. reset_drawlist() can run multiple times per frame (e.g.
     * game.c process_engine_unk3 and lvdraw3d func_2e440), which would wipe the
     * bits draw_object() set during the build before the GL present consumes
     * them. Instead the mask is cleared at the end of sw_get_faces() once the
     * present has read it. */
}

// Special non-textured draw; used during nuclear explosions?
void draw_drawitem_1(ushort dihead)
{
    struct DrawItem *itm;
    ushort iidx;

    for (iidx = dihead; iidx != 0; iidx = itm->Child)
    {
      itm = &game_draw_list[iidx];
      if (drawitem_is_suppressed_face(itm->Type))
          continue;
      if (drawitem_is_suppressed_sprite(itm)) {
          /* Sprite is drawn as a HW billboard instead of by SW, but its
           * mouse-pick (targeting/selection) is normally a side effect of the
           * SW draw — run it here so the cursor still registers the thing. */
          hwr_run_sprite_pick(itm->Offset, itm->Type);
          continue;
      }
      if (drawitem_is_suppressed_effect(itm))
          continue;
      if (drawitem_is_suppressed_glare(itm))
          continue;
      switch (itm->Type)
      {
      case DrIT_ObFace3Txtr:
      case DrIT_Unkn10:
          draw_object_face3d_textrd_dk(itm->Offset);
          break;
      case DrIT_Unkn2:
      case DrIT_Unkn8:
          break;
      case DrIT_SFrmStatc:
          draw_sort_sprite1a(itm->Offset);
          break;
      case DrIT_Unkn4:
          draw_floor_tile1a(itm->Offset);
          break;
      case DrIT_Unkn5:
          draw_ex_face(itm->Offset);
          break;
      case DrIT_Unkn6:
          draw_floor_tile1b(itm->Offset);
          break;
      case DrIT_ObFace3G:
          draw_object_face3g_textrd(itm->Offset);
          break;
      case DrIT_ObFace4Txtr:
          draw_object_face4d_textrd_dk(itm->Offset);
          break;
      case DrIT_Unkn11:
          draw_sort_line1a(itm->Offset);
          break;
      case DrIT_SpObFace4:
          draw_special_object_face4(itm->Offset);
          break;
      case DrIT_SFrmPersV:
          draw_sort_sprite_frame_pers_v(itm->Offset);
          break;
      case DrIT_SFrmPersB:
          draw_sort_sprite_frame_pers_b(itm->Offset);
          break;
      case DrIT_SFrmEfctV:
          draw_sort_sprite_frame_efct_v(itm->Offset);
          break;
      case DrIT_ObFacePole:
          draw_object_face4_pole(itm->Offset);
          break;
      case DrIT_Unkn15:
          draw_sort_sprite1c(itm->Offset);
          break;
      }
    }
}

void draw_drawitem_2(ushort dihead)
{
    struct DrawItem *itm;
    ushort iidx;
    ushort i;

    assert(screen_position_face_render_cb != NULL);
    assert(screen_sorted_sprite_statc_render_cb != NULL);
    assert(screen_sorted_sprite_persn_render_cb != NULL);

    i = 0;
    for (iidx = dihead; iidx != 0; iidx = itm->Child)
    {
      i++;
      if (i > BUCKET_ITEMS_MAX)
          break;
      itm = &game_draw_list[iidx];
      if (drawitem_is_suppressed_face(itm->Type))
          continue;
      if (drawitem_is_suppressed_sprite(itm)) {
          /* Sprite is drawn as a HW billboard instead of by SW, but its
           * mouse-pick (targeting/selection) is normally a side effect of the
           * SW draw — run it here so the cursor still registers the thing. */
          hwr_run_sprite_pick(itm->Offset, itm->Type);
          continue;
      }
      if (drawitem_is_suppressed_effect(itm))
          continue;
      if (drawitem_is_suppressed_glare(itm))
          continue;
      switch (itm->Type)
      {
      case DrIT_ObFace3Txtr:
      case DrIT_Unkn10:
          draw_object_face3d_textrd(itm->Offset);
          break;
      case DrIT_SFrmStatc:
          draw_sort_sprite1a(itm->Offset);
          break;
      case DrIT_Unkn4:
          draw_floor_tile1a(itm->Offset);
          break;
      case DrIT_Unkn5:
          draw_ex_face(itm->Offset);
          break;
      case DrIT_Unkn6:
          draw_floor_tile1b(itm->Offset);
          break;
      case DrIT_ObFace3G:
          draw_object_face3g_textrd(itm->Offset);
          break;
      case DrIT_ObFace4Txtr:
          draw_object_face4d_textrd(itm->Offset);
          break;
      case DrIT_Unkn11:
          draw_sort_line1a(itm->Offset);
          break;
      case DrIT_SpObFace4:
          draw_special_object_face4(itm->Offset);
          break;
      case DrIT_SFrmPersV:
          draw_sort_sprite_frame_pers_v(itm->Offset);
          break;
      case DrIT_SFrmPersB:
          draw_sort_sprite_frame_pers_b(itm->Offset);
          break;
      case DrIT_SFrmEfctV:
          draw_sort_sprite_frame_efct_v(itm->Offset);
          break;
      case DrIT_ObFacePole:
          draw_object_face4_pole(itm->Offset);
          break;
      case DrIT_Unkn15:
          draw_sort_sprite1c(itm->Offset);
          break;
      case DrIT_ObFace4G:
          draw_object_face4g_textrd(itm->Offset);
          break;
      case DrIT_ObFace3Refl:
          draw_object_face3_reflect(itm->Offset);
          break;
      case DrIT_ObFace4Refl:
          draw_object_face4_reflect(itm->Offset);
          break;
      case DrIT_SPersShdw:
          draw_sort_sprite_person_shadow(itm->Offset);
          break;
      case DrIT_SharpnlPoly:
          draw_shrapnel(itm->Offset);
          break;
      case DrIT_SFrmPhwoar:
          draw_phwoar(itm->Offset);
          break;
      case DrIT_LongPropBar:
          draw_sort_sprite_long_prop_bar(itm->Offset);
          break;
      case DrIT_ObFace4Tran:
          draw_object_face4_tran_tint(itm->Offset);
          break;
      case DrIT_ObFace3Tran:
          draw_object_face3_tran_tint(itm->Offset);
          break;
      case DrIT_SFireFlame:
          draw_fire_flame(itm->Offset);
          break;
      case DrIT_Number:
          draw_sort_sprite_number(itm->Offset);
          break;
      case DrIT_ShortText:
          draw_sort_sprite_short_text(itm->Offset);
          break;
      default:
          break;
      }
    }
}
/******************************************************************************/
