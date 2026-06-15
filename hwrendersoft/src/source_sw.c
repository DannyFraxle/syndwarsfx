/******************************************************************************/
// Syndicate Wars FX3D - OpenGL hardware renderer for Bullfrog titles.
/******************************************************************************/
/** @file source_sw.c
 *     Syndicate Wars implementation of the HwrSceneSource interface.
 * @par Purpose:
 *     Bridges the game's world data to the engine-agnostic scene source the GL
 *     backend pulls from. This is the only file in libhwrender that knows about
 *     Syndicate Wars globals; other Bullfrog titles supply their own source_*.c.
 *
 *     The needed globals are declared here directly (with link-compatible
 *     types) rather than by including the game's deeply-tangled headers, so the
 *     library stays buildable on its own. They resolve at the final link of the
 *     game executable.
 * @par Comment:
 *     Phase 1 fills the camera and palette and reports no geometry yet; the
 *     map-grid floor (Phase 3), faces, lights and sprites land in later phases.
 * @par  Copying and copyrights:
 *     This program is free software; you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of the License, or
 *     (at your option) any later version.
 */
/******************************************************************************/
#include "hwr_scene_source.h"

#include <stddef.h>
#include <stdint.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* --- Game globals (resolved at the executable's link step) --- */
/* Camera, from swrendersoft/include/engincam.h (s32 == int32_t). */
extern int32_t        engn_xc, engn_yc, engn_zc, engn_anglexz;
extern unsigned short overall_scale;            /* world-to-screen scale */
/* Current 6-bit-per-channel palette (256 RGB triplets, values 0..63). */
extern unsigned char  display_palette[768];

/* Angle units: the game uses 2048 steps per full circle (LbFPMath). The
 * projection index is (engn_anglexz >> 5) & 0x7FF (see src/lvdraw3d.c:144). */
#define HWR_ANGLE_STEPS 2048
/* Fixed isometric pitch of the SW camera. Refined against lvdraw3d in Phase 3. */
#define HWR_ISO_PITCH   (35.264f * (float)M_PI / 180.0f)

/* Viewport, supplied by the host at creation time. */
static int sw_view_w = 0;
static int sw_view_h = 0;

static int sw_get_camera(void *ctx, HwrCamera *out)
{
    int ang = (int)((engn_anglexz >> 5) & 0x7FF);
    (void)ctx;
    if (out == NULL)
        return -1;
    out->angle_xz = (float)ang * (2.0f * (float)M_PI / (float)HWR_ANGLE_STEPS);
    out->angle_y  = HWR_ISO_PITCH;
    out->scale    = (overall_scale != 0) ? (float)overall_scale : 1.0f;
    /* engn_xc/zc are map coordinates (8.8 fixed point); pass world units. */
    out->centre_x = (float)engn_xc / 256.0f;
    out->centre_z = (float)engn_zc / 256.0f;
    out->view_w   = sw_view_w;
    out->view_h   = sw_view_h;
    return 0;
}

static int sw_get_floor(void *ctx, HwrGeometryBatch *out)
{
    (void)ctx;
    if (out != NULL) {
        out->verts = NULL; out->vert_count = 0;
        out->indices = NULL; out->index_count = 0;
    }
    return 0;   /* Phase 3 */
}

static int sw_get_faces(void *ctx, HwrGeometryBatch *out)
{
    (void)ctx;
    if (out != NULL) {
        out->verts = NULL; out->vert_count = 0;
        out->indices = NULL; out->index_count = 0;
    }
    return 0;   /* Phase 4 */
}

static int sw_get_lights(void *ctx, HwrLight *out, int max)
{
    (void)ctx; (void)out; (void)max;
    return 0;   /* Phase 5 */
}

static int sw_get_sprites(void *ctx, HwrBillboard *out, int max)
{
    (void)ctx; (void)out; (void)max;
    return 0;   /* Phase 6 */
}

static const uint8_t *sw_get_palette(void *ctx)
{
    (void)ctx;
    return (const uint8_t *)display_palette;
}

static HwrSceneSource sw_source = {
    NULL,           /* ctx */
    NULL,           /* begin_frame */
    sw_get_camera,
    sw_get_floor,
    sw_get_faces,
    sw_get_lights,
    sw_get_sprites,
    sw_get_palette,
};

/** Return the Syndicate Wars scene source, configured for the given viewport.
 *  The host calls this once after hwr_init() and hands the result to
 *  hwr_set_source(). */
const HwrSceneSource *hwr_sw_source(int view_w, int view_h)
{
    sw_view_w = view_w;
    sw_view_h = view_h;
    return &sw_source;
}
