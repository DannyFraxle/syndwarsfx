/******************************************************************************/
// Syndicate Wars FX3D - OpenGL hardware renderer for Bullfrog titles.
/******************************************************************************/
/** @file hwr_scene_source.h
 *     Engine-agnostic scene source interface.
 * @par Purpose:
 *     Defines the pull interface the GL backend renders from. The backend
 *     never touches game-specific types; a per-title implementation (e.g.
 *     source_sw.c for Syndicate Wars) fills these structures by walking that
 *     game's world data. This is the seam that lets other Bullfrog decomps
 *     plug in later.
 * @par Comment:
 *     Just a header file - typedefs and function-pointer table.
 * @par  Copying and copyrights:
 *     This program is free software; you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of the License, or
 *     (at your option) any later version.
 */
/******************************************************************************/
#ifndef HWR_SCENE_SOURCE_H
#define HWR_SCENE_SOURCE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
/******************************************************************************/

/** A single textured, lit vertex in world space.
 *  Coordinates are world units in the same frame as HwrCamera.mvp.
 *  u,v are texels (0..255) into the texture page; page selects the layer of the
 *  indexed texture-array. light is a 0..255 per-vertex shade applied on top of
 *  point lighting. */
typedef struct {
    float x, y, z;
    float u, v;
    float tile_depth;  /* per-tile constant scrd, matches SW bucket sort depth */
    uint8_t page;
    uint8_t light;
    uint8_t emissive; /* 255 for face modes SW never scene-shades (window glass,
                        * unshaded texture/flat-fill modes - see
                        * hwr_mode_is_scene_shaded in source_sw.c), 0 otherwise */
} HwrVertex;

/** The indexed texture pages backing the geometry: count layers of
 *  width*height 8-bit (palette-index) texels, layer-major. Supplied by the
 *  source so the backend can upload a GL_R8 texture array. */
typedef struct {
    const uint8_t *texels;   /**< count * width * height bytes, or NULL. */
    int width, height, count;
} HwrTexturePages;

/** Raw factors of the game's isometric projection (transform_shpoint), so the
 *  vertex shader can reproduce it exactly - including the mode-5 perspective
 *  foreshortening, which no single matrix can express. d10/d14 are sin/cos of
 *  the XZ rotation and d18/d1c sin/cos of the view tilt (all *65536); scale is
 *  overall_scale; cx/cy8/cz are the camera centre (cy8 = 8*engn_yc); centre_x/y
 *  are the screen centre (D3C/D40); perspective is game_perspective. */
typedef struct {
    float d10, d14, d18, d1c;
    float scale;
    float centre_x, centre_y;
    float cx, cy8, cz;
    int   perspective;
    int   view_w, view_h;
} HwrCamera;

/** A batch of geometry: indexed triangles over a shared vertex array.
 *  Used for both floor tiles (Phase 3) and object/building faces (Phase 4). */
typedef struct {
    const HwrVertex *verts;
    int              vert_count;
    const uint32_t  *indices;
    int              index_count;
} HwrGeometryBatch;

/** A reflective ("chameleon"/spectraflair paint) vertex. Carries the unit
 *  world-space normal and a base palette colour, so the chameleon shader can
 *  compute a view-angle hue shift plus a faked fresnel sheen procedurally.
 *  depth is the same per-vertex scrd as HwrVertex.tile_depth (shared z-buffer). */
typedef struct {
    float x, y, z;
    float nx, ny, nz;
    float depth;
    float base;            /* ExCol palette index (0..255) as float */
} HwrReflectVertex;

/** A batch of reflective faces: indexed triangles over a shared vertex array. */
typedef struct {
    const HwrReflectVertex *verts;
    int                     vert_count;
    const uint32_t         *indices;
    int                     index_count;
} HwrReflectBatch;


/** A point light. r,g,b are linear 0..1 (already 6-bit-expanded by the
 *  source). radius is the inverse-square attenuation constant; max_dist2 is the
 *  per-pixel distance-cull radius squared (PRCCOORD²), set per-light by the
 *  scene source (can widen for elevated lights so their pool reaches the ground). */
typedef struct {
    float x, y, z;
    float r, g, b;
    float radius;
    float max_dist2;       /* per-light distance-cull threshold, default = global_base */
    float fdx, fdz;        /* shaped (headlight) forward dir in XZ; (0,0) = round light.
                            * When set, the light uses a teardrop falloff: narrow/bright
                            * near the lamp, widening and fading along the forward dir. */
} HwrLight;

/** A camera-facing billboard sprite (a Thing). pos is the world anchor;
 *  sprite indexes into the atlas; shade is a 0..255 brightness; flags carries
 *  per-sprite hints (bit0 = translucent -> deferred to the Phase 7 pass).
 *  half_size_x/y are the world-space half-extents of the billboard quad
 *  (the quad corners are at center ± half_size in the camera-facing plane). */
typedef struct {
    float    x, y, z;
    uint16_t sprite;
    uint8_t  shade;
    uint8_t  flags;
    float    half_size_x, half_size_y;
} HwrBillboard;

#define HWR_BILLBOARD_TRANSLUCENT 0x01
#define HWR_BILLBOARD_NOSHADOW   0x02   /* light sources — skip shadow casting */
#define HWR_BILLBOARD_ONTOP      0x04   /* depth-bias toward camera (e.g. dropped items over bodies) */
#define HWR_BILLBOARD_ADDITIVE   0x08   /* translucent blend hint: additive (fire/explosions/glow)
                                         * instead of alpha-over (smoke). Only meaningful with
                                         * HWR_BILLBOARD_TRANSLUCENT. */
#define HWR_BILLBOARD_UNLIT      0x10   /* bypass scene lighting (unlit=1): use for sprites that
                                         * must render at full brightness regardless of shadows/lights
                                         * (e.g. character firing frames with gun-flash overlay). */

/** A screen-space coloured quad (four corners in screen pixels, plus an RGBA
 *  colour) for flat-tinted 2D overlay effects (shield-hit spheres, blast rings,
 *  lightning). Corners are in the engine's projected pixel space (origin at the
 *  screen top-left, same units as the camera centre_x/centre_y). */
typedef struct {
    float x[4], y[4];      /* corner screen positions (pixels) */
    float r, g, b, a;      /* fill colour 0..1 (flat quads) */
    /* Textured quads (slot >= 0): sampled from the sprite atlas — used to draw
     * HUD sprite art (e.g. the target-box tile) blended over the 3D. Corners map
     * TL,TR,BR,BL to the UV rect. a is the blend opacity. */
    int   slot;            /* atlas slot, or -1 for a flat coloured quad */
    float u0, v0, u1, v1;
} HwrOverlayQuad;

/** Pull interface implemented per game title. All getters return the number of
 *  items produced (>=0) or a negative value on error. The backend calls
 *  begin_frame() once, then the getters, every frame. Pointers handed back by
 *  get_geometry/get_floor remain valid until the next begin_frame(). */
typedef struct HwrSceneSource {
    void *ctx;

    void (*begin_frame)(void *ctx);

    /** Fills *out with the current camera; returns 0 on success. */
    int  (*get_camera)(void *ctx, HwrCamera *out);

    /** Floor geometry assembled from the map grid (Phase 3). */
    int  (*get_floor)(void *ctx, HwrGeometryBatch *out);

    /** Object/building faces (Phase 4). */
    int  (*get_faces)(void *ctx, HwrGeometryBatch *out);

    /** Reflective (chameleon paint) faces, drawn by the chameleon pass with a
     *  procedural view-angle hue shift + sheen (Phase 7). May be NULL. */
    int  (*get_reflect_faces)(void *ctx, HwrReflectBatch *out);

    /** Semi-transparent object/building faces (Phase 8): deep-radar see-through
     *  buildings and static glass/fence faces. Same vertex layout as get_faces;
     *  the batch is pre-sorted back-to-front for correct alpha blending. Drawn
     *  by the blended transparent pass after the opaque scene. May be NULL. */
    int  (*get_transparent_faces)(void *ctx, HwrGeometryBatch *out);

    /** Up to max lights into out[]; returns count (Phase 5). */
    int  (*get_lights)(void *ctx, HwrLight *out, int max);

    /** Up to max billboards into out[]; returns count (Phase 6). */
    int  (*get_sprites)(void *ctx, HwrBillboard *out, int max);

    /** 256*3 bytes of 6-bit-per-channel palette (raw game values 0..63). */
    const uint8_t *(*get_palette)(void *ctx);

    /** Indexed texture pages backing the floor/face geometry. Returns 0 and
     *  fills *out on success; the texels pointer must stay valid for the frame.
     *  May be NULL if the source provides no textures. */
    int (*get_texture_pages)(void *ctx, HwrTexturePages *out);

    /** The palette index used as the transparent key when compositing the
     *  software HUD/objects over the 3D scene (see the hybrid present path).
     *  May be NULL if compositing is not used. */
    int (*get_key_index)(void *ctx);

    /** Screen-space coloured overlay quads (flat-tinted special faces the engine
     *  draws in 2D: shield-hit spheres, blast rings, lightning slices). Up to
     *  max quads into out[]; returns count. Drawn blended on top of the resolved
     *  3D scene. May be NULL. */
    int (*get_overlays)(void *ctx, HwrOverlayQuad *out, int max);
} HwrSceneSource;

/******************************************************************************/
#ifdef __cplusplus
}
#endif
#endif
