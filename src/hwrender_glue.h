/******************************************************************************/
// Syndicate Wars Fan Expansion, source port of the classic game from Bullfrog.
/******************************************************************************/
/** @file hwrender_glue.h
 *     Host-side glue to the FX3D OpenGL hardware renderer.
 * @par Purpose:
 *     Thin shim the game calls to drive libhwrender: read the command-line
 *     toggle, request a GL-capable window, initialise on the SDL window, and
 *     render+present each frame. The hardware renderer is always built and
 *     enabled by default in this build.
 * @par  Copying and copyrights:
 *     This program is free software; you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of the License, or
 *     (at your option) any later version.
 */
/******************************************************************************/
#ifndef HWRENDER_GLUE_H
#define HWRENDER_GLUE_H

#include "bftypes.h"

#ifdef __cplusplus
extern "C" {
#endif
/******************************************************************************/

/* --- FX3D configuration (from rules.ini [fx3d] and/or command line) --- */

/** MSAA sample count: 0 = none, otherwise 2/4/8. */
extern int fx3d_aa_samples;
/** Texture filtering per category: 0 = nearest (crisp), 1 = linear (smooth). */
extern int fx3d_filter_ground;
extern int fx3d_filter_objects;
extern int fx3d_filter_sprites;

/** Display frame-rate target (0 = uncapped), vsync on/off, and the FPS overlay
 *  toggle. Applied by fx3d_config_finalize(). */
extern int fx3d_target_fps;
extern int fx3d_vsync;
extern int fx3d_show_fps;
extern int fx3d_debug_things;

/** Command-line overrides; -1 means "not set on the command line", so rules.ini
 *  provides the value. The CLI parser sets these; fx3d_config_finalize() applies
 *  them over whatever rules.ini set. */
extern int fx3d_cli_aa;
extern int fx3d_cli_filter_ground;
extern int fx3d_cli_filter_objects;
extern int fx3d_cli_filter_sprites;

/** Apply command-line overrides over the rules.ini values and publish the MSAA
 *  sample count to the screen backend. Call once, right after rules.ini is read
 *  (and before the video mode / GL window is set up). */
void fx3d_config_finalize(void);

/** True when the hardware renderer is active (always true by default in
 *  this build, unless explicitly disabled via rules.ini). */
TbBool hwrender_requested(void);

/** Record that the user asked for the hardware renderer (CLI parse). */
void hwrender_set_requested(TbBool on);

/** True once the GL backend has been initialised and is drawing. */
TbBool hwrender_active(void);

/** 1 while the current frame is floor-gated for FX3D (set by
 *  hwrender_floor_gate() at frame start). Test THIS from draw/build code;
 *  never call hwrender_floor_gate() itself as a query - it PERFORMS the
 *  gating (framebuffer key-fill, camera snapshot, sprite collection). */
extern int hwr_floor_gated_frame;

/** Palette index used as the transparent composite key: where the software
 *  engine view is gated for 3D, WScreen is filled with this index, and the GL
 *  overlay discards it so the 3D scene shows through. */
#define HWR_KEY_INDEX 255

/** Called in place of the software floor draw. When the hardware renderer is
 *  active and drawing the engine view, this fills the engine framebuffer with
 *  the composite key (so the 3D floor shows through) and returns true, meaning
 *  the caller must skip the software floor. Returns false otherwise (caller
 *  draws the software floor as normal). */
TbBool hwrender_floor_gate(void);

/** Initialise the GL backend on the current SDL window. Call after the video
 *  mode is set. No-op (returns false) in a software-only build or when not
 *  requested. */
TbBool hwrender_startup(int view_w, int view_h);

/** Render the current frame with the hardware path and present it. Returns
 *  true if it handled the present (so the caller must skip LbScreenSwap).
 *  Returns false when the hardware path is inactive. */
TbBool hwrender_present_frame(void);

/** Release the GL backend. Safe to call unconditionally. */
void hwrender_shutdown(void);

/** Flip between the hardware and software renderers at runtime. The GL
 * context is kept alive either way (only used for cheap 2D presentation of
 * the software-rendered frame in software mode); no-op if the hardware
 * renderer was never brought up. */
void hwrender_toggle(void);

/** Pass 1 before drawing popup screens (e.g. pause) to force full-opacity
 *  WScreen compositing so controls stay solid; pass 0 when done. */
void hwrender_set_opaque_present(int on);


/******************************************************************************/
#ifdef __cplusplus
}
#endif
#endif
