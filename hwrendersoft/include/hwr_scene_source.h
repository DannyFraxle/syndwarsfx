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
 *  Coordinates are world units (Y already negated for OpenGL by the source).
 *  u,v index into the texture atlas in texels; the shader normalises them.
 *  light is a 0..255 per-vertex shade applied on top of point lighting. */
typedef struct {
    float x, y, z;
    float u, v;
    uint8_t light;
} HwrVertex;

/** Camera state derived from the game's isometric projection
 *  (engn_anglexz / overall_scale in Syndicate Wars). The backend builds an
 *  orthographic-ish view-projection matrix from these. */
typedef struct {
    float angle_xz;     /**< Horizontal rotation, radians. */
    float angle_y;      /**< Pitch of the isometric view, radians. */
    float scale;        /**< World-to-screen scale (overall_scale). */
    float centre_x;     /**< World-space point the camera is centred on. */
    float centre_z;
    int   view_w, view_h; /**< Viewport size in pixels. */
} HwrCamera;

/** A batch of geometry: indexed triangles over a shared vertex array.
 *  Used for both floor tiles (Phase 3) and object/building faces (Phase 4). */
typedef struct {
    const HwrVertex *verts;
    int              vert_count;
    const uint16_t  *indices;
    int              index_count;
} HwrGeometryBatch;

/** A point light. r,g,b are linear 0..1 (already 6-bit-expanded by the
 *  source). radius is in world units; intensity falls off to zero at radius. */
typedef struct {
    float x, y, z;
    float r, g, b;
    float radius;
} HwrLight;

/** A camera-facing billboard sprite (a Thing). pos is the world anchor;
 *  sprite indexes into the atlas; shade is a 0..255 brightness; flags carries
 *  per-sprite hints (bit0 = translucent -> deferred to the Phase 7 pass). */
typedef struct {
    float    x, y, z;
    uint16_t sprite;
    uint8_t  shade;
    uint8_t  flags;
} HwrBillboard;

#define HWR_BILLBOARD_TRANSLUCENT 0x01

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

    /** Up to max lights into out[]; returns count (Phase 5). */
    int  (*get_lights)(void *ctx, HwrLight *out, int max);

    /** Up to max billboards into out[]; returns count (Phase 6). */
    int  (*get_sprites)(void *ctx, HwrBillboard *out, int max);

    /** 256*3 bytes of 6-bit-per-channel palette (raw game values 0..63). */
    const uint8_t *(*get_palette)(void *ctx);
} HwrSceneSource;

/******************************************************************************/
#ifdef __cplusplus
}
#endif
#endif
