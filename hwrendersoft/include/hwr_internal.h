/******************************************************************************/
// Syndicate Wars FX3D - OpenGL hardware renderer for Bullfrog titles.
/******************************************************************************/
/** @file hwr_internal.h
 *     Declarations shared between libhwrender translation units.
 * @par  Copying and copyrights:
 *     This program is free software; you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of the License, or
 *     (at your option) any later version.
 */
/******************************************************************************/
#ifndef HWR_INTERNAL_H
#define HWR_INTERNAL_H

#include "hwr_scene_source.h"

#ifdef __cplusplus
extern "C" {
#endif
/******************************************************************************/

/** Maximum point lights uploaded to the shader per frame (matches the [64] uniform size). */
#define HWR_MAX_LIGHTS 64

/** Record a printf-style formatted message as the last error. */
void hwr_set_error(const char *fmt, ...);

/** Set the GL viewport to the window's current drawable size. The window is
 *  resized as the game changes video mode, but GL does not track that, so this
 *  must be called before drawing each frame. */
void hwr_sync_viewport(void);

/** The scene source currently bound (may be NULL before hwr_set_source). */
extern const HwrSceneSource *hwr_source;

/* ---- Debug overlay resources (shared with hwr_tuning.c) ----------------- */
extern unsigned int dbg_prog, dbg_vao, dbg_vbo, dbg_font_tex;
extern int    dbg_ready;

/** Project a world-space coordinate (PRCCOORD) to screen pixel-space using the
 *  last captured camera snapshot.  Returns 0 if no snapshot is available. */
int dbg_project(float wx, float wy, float wz, float *sx, float *sy);

/** Emit a single digit quad into the vertex buffer (x,y, u,v) for the debug
 *  font shader.  vw/vh are the viewport dimensions in pixels; sx,sy is the
 *  top-left corner of the digit in screen pixels; glyph is a font index (0-9
 *  for digits, 10='-', 11='.', 12-37 for A-Z). */
extern void dbg_emit_digit(float *vbuf, int *nv,
    float sx, float sy, int glyph, float vw, float vh);

/** Emit a text string using the debug font. Accepts 0-9, A-Z, '-', '.'. */
extern void dbg_emit_text(float *vbuf, int *nv,
    float sx, float sy, const char *text, float vw, float vh);

/******************************************************************************/
#ifdef __cplusplus
}
#endif
#endif
