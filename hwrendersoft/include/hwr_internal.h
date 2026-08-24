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

#include "hwr_gl.h"
#include "hwr_scene_source.h"

#ifdef __cplusplus
extern "C" {
#endif
/******************************************************************************/

/** GLSL helper shared by every program that samples atlas/baked-texture RGBA
 *  data recolour-mapped through the frozen bake palette back to the live
 *  palette (see hwr_sprite.c's "recolour" comment block for the full
 *  rationale). Paste into a fragment shader string, then call
 *  atlas_recolour(rgb) on the sampled colour. */
#define HWR_RECOLOUR_GLSL \
    "uniform sampler3D uInvPal;\n" \
    "uniform sampler2D uPalLive;\n" \
    "uniform int uRecolour;\n" \
    "vec3 atlas_recolour(vec3 c) {\n" \
    "    if (uRecolour == 0) return c;\n" \
    "    vec3 v8 = floor(clamp(c, 0.0, 1.0) * 255.0 + 0.5);\n" \
    "    vec3 cell = (floor(v8 * 0.25) + 0.5) / 64.0;\n" \
    "    float idx = floor(texture(uInvPal, cell).r * 255.0 + 0.5);\n" \
    "    return texture(uPalLive, vec2((idx + 0.5) / 256.0, 0.5)).rgb;\n" \
    "}\n"

/** Bind the recolour uniforms/textures (uInvPal @ GL_TEXTURE6, uPalLive @
 *  GL_TEXTURE7) for a program using HWR_RECOLOUR_GLSL. loc_invpal/loc_pallive/
 *  loc_on are that program's uniform locations for uInvPal/uPalLive/uRecolour
 *  (pass -1 for any not present). Returns nonzero if recolour is active this
 *  frame (live palette differs from the frozen bake palette). */
int hwr_recolour_bind(GLint loc_invpal, GLint loc_pallive, GLint loc_on);

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
