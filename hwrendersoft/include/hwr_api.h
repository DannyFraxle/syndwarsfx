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
#include <stdint.h>

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
    int vsync;          /* 1 = swap synced to refresh, 0 = off */
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

/** Render object/building faces (Phase 4). Reuses the floor program, texture
 *  pages and palette; call after hwr_floor_render so the pages are uploaded.
 *  Returns nonzero if anything was drawn. Does not clear or swap. */
int hwr_faces_render(const uint8_t *pal8, int filter_linear);

/** Render reflective "chameleon" paint faces (Phase 7). Pulls the reflective
 *  batch from the bound source's get_reflect_faces and shades it procedurally
 *  (view-angle hue shift + faked sheen). Call after hwr_faces_render so the
 *  depth buffer holds the opaque scene. Returns nonzero if anything drew. */
int hwr_reflect_render(const uint8_t *pal8);

/** Render sprite billboards for this frame (Phase 6). Pulls billboard data
 *  from the bound scene source's get_sprites callback. Call after the floor
 *  and face passes, before SSAO resolve. Returns nonzero if anything drew. */
int hwr_sprites_render(const uint8_t *pal8, int filter_linear);

/** Render semi-transparent faces (Phase 8): deep-radar see-through buildings
 *  and static glass/fence faces, pulled from get_transparent_faces and drawn
 *  alpha-blended over the opaque scene (depth-tested, not depth-writing). Call
 *  after hwr_faces_render. Returns nonzero if anything drew. */
int hwr_transparent_render(const uint8_t *pal8, int filter_linear);

/** Configure the transparent face pass (from fx3d.ini [transparency]):
 *  enable toggles it; alpha is the blended fragment opacity (0..1);
 *  deepradar_idx is the palette index used to flat-tint deep-radar see-through
 *  buildings (the syndicate purple, default 216). */
void hwr_transparent_config(int enable, float alpha, int deepradar_idx);

/** Render translucent sprite billboards (Phase 8): fire/flame/smoke/glow
 *  effects flagged HWR_BILLBOARD_TRANSLUCENT, drawn blended (additive for
 *  emissive, alpha otherwise) after the opaque sprite pass. Returns nonzero if
 *  anything drew. */
int hwr_sprites_trans_render(const uint8_t *pal8, int filter_linear);

/** Configure the translucent sprite pass: enable toggles it; alpha scales the
 *  per-sprite opacity (0..1). */
void hwr_sprites_trans_config(int enable, float alpha);

/** Drop cached sprite atlas and state; call on level change. */
void hwr_sprites_reset(void);

/** Drop cached floor GPU art (texture pages); call on level change. */
void hwr_floor_reset(void);

/** Present the 8-bit framebuffer over the current GL scene, discarding pixels
 *  whose palette index equals key_index (so the 3D scene shows through). Same as
 *  hwr_present_indexed but with the compositing key; key_index < 0 disables it. */
void hwr_present_indexed_keyed(const uint8_t *px, int w, int h, int pitch,
    const uint8_t *pal, int key_index);

/** As hwr_present_indexed_keyed, but blends the (non-key) overlay over the 3D
 *  scene at `alpha` (0..1) — used for the transparent HUD panel option. */
void hwr_present_indexed_keyed_alpha(const uint8_t *px, int w, int h, int pitch,
    const uint8_t *pal, int key_index, float alpha);

/** As hwr_present_indexed_keyed_alpha but applies alpha only to pixels whose
 *  palette colour has luminance below bg_luma (the panel background fill);
 *  brighter pixels (outlines, numbers, map) are fully opaque. */
void hwr_present_indexed_keyed_luma(const uint8_t *px, int w, int h, int pitch,
    const uint8_t *pal, int key_index, float bg_alpha, float bg_luma);

/** Configure the procedural rain overlay (from fx3d.ini [rain]).
 *  enable toggles the pass; alpha is the streak opacity (0..1); density is the
 *  streak-column count across one screen-height of width; speed is the fall
 *  speed in screen-heights/second; width is streak thickness in pixels;
 *  length is streak length as a fraction of screen height; angle is a wind
 *  slant in radians (0 = straight down). */
void hwr_rain_config(int enable, float alpha, float density, float speed,
    float width, float length, float angle);

/** Draw the procedural rain overlay, alpha-blended over the already-rendered
 *  3D scene. Call after the opaque/translucent 3D passes, before the keyed
 *  WScreen (HUD) composite. No-op when disabled or not ready. */
void hwr_rain_render(void);

/** Configure the distance fog overlay (from fx3d.ini [fog]). enable
 *  toggles the pass; r/g/b is the haze tint; density is the maximum opacity at
 *  full distance (0..1); start/end are the world-unit distance ramp used when
 *  the SSAO/water G-buffer is available; scr_start/scr_end are the screen-Y
 *  ramp (0 = top of screen) used as the geometry-free fallback. */
void hwr_fog_config(int enable, float r, float g, float b, float density,
    float start, float end, float scr_start, float scr_end);

/** Draw the distance fog, alpha-blended over the already-rendered 3D scene.
 *  Call after the 3D passes, before hwr_rain_render(). */
void hwr_fog_render(void);

/** Configure the bullet-time radial-blur + motion-trail effect (from
 *  fx3d.ini [bullettime]). enable toggles the pass; blur_strength is
 *  the max radial-blur reach at the screen edge (UV units) at full dip;
 *  trail is how much of the previous frame persists into this one at full
 *  dip (0..1, motion-trail strength). */
void hwr_bullettime_config(int enable, float blur_strength, float trail);

/** Draw the bullet-time radial-blur + motion-trail effect over the already-
 *  rendered 3D scene (captures and replaces the current back buffer content
 *  - genuinely samples the rendered scene, unlike the flat-quad rain/HUD
 *  overlays). intensity is game_speed.c's bullettime_intensity() (0 = normal
 *  speed, no-op; up to 1 = deepest slow-motion dip). Call after the
 *  opaque/translucent 3D passes, before debug overlays and the keyed
 *  WScreen composite. */
void hwr_bullettime_render(float intensity);

/** Configure screen-space ambient occlusion (from fx3d.ini). enable
 *  toggles the whole G-buffer path; world is the AO reach in WORLD units (the
 *  sample radius, converted to pixels per-pixel so the effect keeps a constant
 *  physical size under zoom and the mode-5 perspective warp); max_px caps that
 *  screen radius in pixels; strength scales the darkening; bias rejects
 *  self-occlusion; debug selects a stage view (0=final, 1=world-pos, 2=raw AO,
 *  3=blurred AO). */
void hwr_ssao_config(int enable, float max_px, float world, float strength,
    float bias, int debug);

/** Set the camera's world-space view direction (pointing into the screen /
 *  increasing depth), used by SSAO to orient reconstructed surface normals
 *  toward the camera. Call once per frame before hwr_ssao_resolve. */
void hwr_ssao_set_viewdir(float x, float y, float z);

/** Configure water screen-space reflection (from fx3d.ini [water]).
 *  When enabled, the composite pass ray-marches reflections of the scene off
 *  water pixels (tagged in the G-buffer's world-position .w channel), falling
 *  back to the sky colour where a ray leaves the screen or hits nothing.
 *  strength is the reflection blend (0..1, mild); sky_* is the dark blue/grey
 *  reflected when there is no geometry. Enabling this forces the G-buffer /
 *  composite path on even when SSAO itself is off (AO is then held at 1.0). */
void hwr_ssao_reflect_config(int enable, float strength,
    float sky_r, float sky_g, float sky_b, int debug, float blur);

/** Feed the current camera's projection factors to the SSAO composite so the
 *  water-reflection ray-march can project world points back to screen UVs
 *  (reproduces transform_shpoint). Call once per frame before hwr_ssao_resolve;
 *  hwr_floor's fl_setup_program already does this. */
void hwr_ssao_set_camera(float d10, float d14, float d18, float d1c,
    float scale, float centre_x, float centre_y,
    float cx, float cy8, float cz, int perspective);

/** Begin the 3D geometry pass. When SSAO is enabled this binds the G-buffer
 *  (creating/resizing it to w*h) and clears it; otherwise it is a no-op and
 *  geometry draws straight to the back buffer. Call before the floor/face passes. */
void hwr_ssao_begin(int w, int h);

/** Resolve SSAO: run the occlusion + blur passes and composite the scene colour
 *  multiplied by AO into the back buffer. No-op when SSAO is disabled (the scene
 *  is already in the back buffer). Call after the floor/face passes, before the
 *  keyed HUD overlay. */
void hwr_ssao_resolve(void);

/** Blit the scene depth from the SSAO G-buffer to the back buffer so a following
 *  depth-tested pass (weapon beams) is occluded by the 3D scene. No-op when SSAO
 *  is inactive (the back buffer already holds the scene depth). Call after
 *  hwr_ssao_resolve(), before hwr_beams_render(). */
void hwr_ssao_blit_depth(void);

/** Nonzero when the SSAO G-buffer (MRT: colour @0 + world-position @1) is the
 *  bound draw target between hwr_ssao_begin() and hwr_ssao_resolve(). Blended
 *  passes that share the G-buffer use this to restrict their draw to the colour
 *  attachment so they don't corrupt the world-position attachment. */
int hwr_ssao_active(void);

/** The G-buffer world-position texture (xyz world pos, w = water mask), still
 *  valid after hwr_ssao_resolve(). Returns 0 when the G-buffer path is off.
 *  Used by the distance fog to get true per-pixel world positions. */
unsigned int hwr_ssao_position_texture(void);

/** The camera centre (look-at point) and unit view direction in world space.
 *  dot(P - ctr, dir) is the view depth: 0 at the screen-centre look-at point,
 *  growing into the distance. This is what distance fog ramps on. */
void hwr_ssao_get_view(float ctr[3], float dir[3]);

/** Configure the directional sun and shadow map (from fx3d.ini [sun]).
 *  enable toggles the whole shadow-map path; brightness is the lit-ground
 *  level (replaces ambient when enabled); ambient is the floor brightness
 *  inside shadow; azimuth/elevation set the sun direction; pcf is the PCF
 *  kernel half-radius in texels (0=hard, 1=3×3, ...); bias is the small
 *  constant shader bias (post-offset); slope/units are the glPolygonOffset
 *  factors for slope-proportional acne suppression; debug=1 shows the
 *  greyscale lit factor for calibration; haze blends the shadow edge toward
 *  lit (0..1, simulating atmospheric scatter). */
void hwr_sun_config(int enable, float brightness, float ambient,
    float azimuth, float elevation, int pcf,
    float bias, float slope, float units, int debug, float haze);

/** Render the scene depth from the sun's viewpoint into the shadow map.
 *  Must be called after hwr_scene_begin() and before hwr_ssao_begin() /
 *  hwr_floor_render(). Restores FBO 0 and viewport on exit. No-op when
 *  the sun is disabled. */
void hwr_sun_shadow_pass(void);

/** Return a pointer to the current 4×4 column-major sun MVP matrix
 *  (valid after hwr_sun_shadow_pass, NULL-safe to read even before init). */
const float *hwr_sun_mvp(void);

/** Return the GL name of the 2048² shadow depth texture (0 if not ready). */
unsigned int hwr_sun_texture(void);

/** 1 if the sun path is both enabled and successfully initialised. */
int hwr_sun_enabled(void);

/** Accessors for the per-frame sun shader parameters.  These read the same
 *  values set by hwr_sun_config so fl_setup_program can query them without a
 *  separate copy of the config. */
float hwr_sun_bright(void);
float hwr_sun_ambient(void);
int   hwr_sun_pcf(void);
float hwr_sun_bias(void);
int   hwr_sun_debug(void);
float hwr_sun_haze(void);

/** Return the world-space direction vector toward the sun (normalised).
 *  Used by the sprite shadow system for sun-projected shadows. */
void hwr_sun_get_direction(float *dx, float *dy, float *dz);

/** Report the GL drawable size in pixels (what the scene renders into). Writes
 *  0,0 if unavailable. Used to size the SSAO G-buffer to match the viewport. */
void hwr_drawable_size(int *w, int *h);

/** Retrieve the camera snapshot captured by hwr_sw_capture() at floor-draw time.
 *  The projection globals (d10/d14/d18/d1c/etc.) are only valid at that point;
 *  later sub-renders overwrite them.  Returns 1 if a snapshot is available, 0 if
 *  hwr_sw_capture() has never been called. */
int hwr_sw_camera_snapshot(int32_t *xc, int32_t *yc, int32_t *zc,
    int32_t *d10, int32_t *d14, int32_t *d18, int32_t *d1c,
    int32_t *d3c, int32_t *d40, int32_t *scale, int32_t *persp);

/* ---- ThingNo debug overlay --------------------------------------------- */
/** Enable/disable the ThingNo debug overlay (0=off, 1=on).  When enabled,
 *  every in-game Thing has its index number drawn at its screen position. */
void hwr_thingno_debug(int enable);

/** Render the ThingNo overlay. Call after the 3D passes (floor + faces) but
 *  before the HUD / present, so text appears on top of the scene. */
void hwr_thingno_render(void);

/* ---- Sprite debug overlay ------------------------------------------------ */
/** Enable/disable the sprite debug overlay (0=off, 1=on). */
void hwr_sprites_debug(int enable);

/** Render the sprite debug overlay. Call after hwr_sprites_render. */
void hwr_sprites_debug_render(void);

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
