/******************************************************************************/
// Syndicate Wars FX3D - OpenGL hardware renderer for Bullfrog titles.
/******************************************************************************/
/** @file hwr_source_sw.h
 *     Accessor for the Syndicate Wars scene source.
 * @par Purpose:
 *     The one game-specific entry point of libhwrender: hands the host the
 *     HwrSceneSource that reads Syndicate Wars world data. Other Bullfrog
 *     titles provide their own equivalent.
 * @par  Copying and copyrights:
 *     This program is free software; you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of the License, or
 *     (at your option) any later version.
 */
/******************************************************************************/
#ifndef HWR_SOURCE_SW_H
#define HWR_SOURCE_SW_H

#include "hwr_scene_source.h"

#ifdef __cplusplus
extern "C" {
#endif
/******************************************************************************/

/** Configure (for the given viewport) and return the Syndicate Wars scene
 *  source. Pass the result to hwr_set_source(). */
const HwrSceneSource *hwr_sw_source(int view_w, int view_h);

/** Collect Thing-based sprites from the current frame's sort-sprite and
 *  draw-list arrays into an internal billboard buffer, and set bits in
 *  hwr_sprite_skip_mask so the SW drawlist executor skips them.
 *  Must be called after engine_draw_things() and before draw_screen()
 *  (before reset_drawlist() clears the arrays). */
void hwr_sw_collect_sprites(void);

/** Bitmask indexed by SortSprite index: bit set = this sprite was collected
 *  by the HW renderer and should be skipped by the SW drawlist. Defined in
 *  engindrwlstx.c (libswrender) so both libraries share one copy. */
extern unsigned char hwr_sprite_skip_mask[512];

/** Snapshot the engine-view camera (projection factors + centre) for this
 *  frame. Call at floor-draw time, while the projection globals still hold the
 *  engine view (before BAT/billboard sub-renders overwrite them). */
void hwr_sw_capture(void);

/** Snapshot ex_faces (explosion/collapse fragments) for interpolation. Call
 *  AFTER process_explode() has advanced them for this turn - see the doc
 *  comment on the definition in source_sw.c for why this must NOT be folded
 *  into hwr_sw_capture(), which runs earlier in the turn. */
void hwr_sw_capture_explode(void);

/** Result of the per-level sun-direction scan of the baked SW floor shading.
 *  The scan measures the darkness-weighted brightness gradient of the floor
 *  shading (dark/shadowed tiles point toward the lit sun side), independently
 *  for the per-tile Ambient and Shade fields, and picks whichever shows a real,
 *  coherent direction. */
typedef struct {
    float azimuth;      /**< Chosen sun bearing (deg), or <0 if no clear direction. */
    float coherence;    /**< 0..1 directional agreement of the chosen field.        */
    int   field;        /**< Field used: 0=none, 1=Ambient, 2=Shade.                */
    /* Per-field diagnostics (so we can see which field carries the signal). */
    float amb_std, amb_az, amb_coh;   /**< Ambient: std-dev, gradient bearing, coherence. */
    float shd_std, shd_az, shd_coh;   /**< Shade:   std-dev, gradient bearing, coherence. */
    long  nfloor;       /**< Floor cells scanned (Texture!=0).                      */
    int   map_null;     /**< 1 = game_my_big_map was NULL when scanned (no level).  */
    int   fresh;        /**< 1 = recomputed this call (map/level changed).          */
} HwrSunHint;

/** Estimate the per-level sun azimuth from the level's baked floor shading.
 *  Recomputed only when the map (level) changes; fills *out with the result and
 *  full diagnostics. azimuth < 0 means the caller should keep its configured
 *  bearing. */
void hwr_sw_sun_hint(HwrSunHint *out);

/******************************************************************************/
#ifdef __cplusplus
}
#endif
#endif
