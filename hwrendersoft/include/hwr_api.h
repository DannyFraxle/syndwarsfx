/******************************************************************************/
// Syndicate Wars FX3D - OpenGL hardware renderer for Bullfrog titles.
/******************************************************************************/
/** @file hwr_api.h
 *     Public entry points of the hardware (OpenGL) renderer.
 * @par Purpose:
 *     The small surface the host game calls: initialise a GL context on the
 *     existing SDL window, draw a frame by pulling from a HwrSceneSource, and
 *     shut down. Everything else is internal to libhwrender.
 * @par  Copying and copyrights:
 *     This program is free software; you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of the License, or
 *     (at your option) any later version.
 */
/******************************************************************************/
#ifndef HWR_API_H
#define HWR_API_H

#include "hwr_scene_source.h"

#ifdef __cplusplus
extern "C" {
#endif
/******************************************************************************/

/** Result codes. */
#define HWR_OK     0
#define HWR_ERROR (-1)

/** Renderer configuration, supplied by the host (from rules.ini / CLI) before
 *  hwr_init(). aa_samples is the MSAA sample count (0 = off; otherwise 2/4/8).
 *  The filter_* flags select GL_LINEAR (1) vs GL_NEAREST (0) for each texture
 *  category; they take effect as the corresponding phases create textures. */
typedef struct {
    int aa_samples;
    int filter_ground;
    int filter_objects;
    int filter_sprites;
} HwrConfig;

/** Store the renderer configuration. Call before hwr_init() so the MSAA sample
 *  count is known when the GL context is created. Safe to call again later to
 *  update the texture-filter flags. */
void hwr_set_config(const HwrConfig *cfg);

/** Read-only access to the current configuration (never NULL). */
const HwrConfig *hwr_config(void);

/** Create a GL 3.3 core context on the given SDL_Window and load GL.
 *  win is an SDL_Window* (void* to keep SDL out of this header). The window
 *  must have been created with the SDL_WINDOW_OPENGL flag. Returns HWR_OK or
 *  HWR_ERROR; on error hwr_last_error() describes the failure. */
int hwr_init(void *sdl_window);

/** True once hwr_init() has succeeded and the backend is usable. */
int hwr_is_ready(void);

/** Bind the scene source the renderer pulls from each frame. */
void hwr_set_source(const HwrSceneSource *src);

/** Render one frame: clear, then (as phases land) floor, faces, lights,
 *  sprites and the translucent pass. Does NOT swap buffers - the host owns
 *  the window flip (call hwr_present() or SDL_GL_SwapWindow). */
void hwr_draw_frame(void);

/** Present an 8-bit indexed framebuffer (the game's WScreen) as a depalettised
 *  fullscreen quad. px points to w*h index bytes, tightly packed at `pitch`
 *  bytes per row; pal is 256*3 bytes of the active full-range 8-bit RGB
 *  palette. Clears and draws into the current GL back buffer but does NOT swap;
 *  call hwr_present() afterwards. This is the bridge that lets the existing
 *  software-rendered frame display through OpenGL, and the basis for the
 *  depalettising shader the 3D phases reuse. */
void hwr_present_indexed(const uint8_t *px, int w, int h, int pitch,
    const uint8_t *pal6);

/** Begin a 3D scene frame: sync the viewport and clear colour + depth. Call
 *  before the floor/face/sprite passes, then composite the keyed overlay. */
void hwr_scene_begin(void);

/** Render the level floor as 3D geometry for this frame, pulling camera, floor
 *  geometry and texture pages from the bound scene source. pal8 is the active
 *  256*3 8-bit palette; filter_linear selects smooth vs crisp sampling. Returns
 *  nonzero if anything was drawn. Does not clear or swap. */
int hwr_floor_render(const uint8_t *pal8, int filter_linear);

/** Drop cached floor GPU art (texture pages); call on level change. */
void hwr_floor_reset(void);

/** Present the 8-bit framebuffer over the current GL scene, discarding pixels
 *  whose palette index equals key_index (so the 3D scene shows through). Same as
 *  hwr_present_indexed but with the compositing key; key_index < 0 disables it. */
void hwr_present_indexed_keyed(const uint8_t *px, int w, int h, int pitch,
    const uint8_t *pal, int key_index);

/** Swap the GL back buffer to the screen. */
void hwr_present(void);

/** Tear down GL resources and the context. Safe to call when not ready. */
void hwr_shutdown(void);

/** Human-readable description of the most recent failure (never NULL). */
const char *hwr_last_error(void);

/******************************************************************************/
#ifdef __cplusplus
}
#endif
#endif
