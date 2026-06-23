/******************************************************************************/
// Syndicate Wars FX3D - OpenGL hardware renderer for Bullfrog titles.
/******************************************************************************/
/**                        2026 danny@fraxle.net                             **/
/******************************************************************************/
/** @file hwr_draw.c
 *     Per-frame top-level draw orchestration.
 * @par Purpose:
 *     Clears the frame and (as phases land) drives the floor, face, lighting,
 *     sprite and translucent passes by pulling geometry from the bound
 *     HwrSceneSource. In Phase 1 this only clears the colour/depth buffers so
 *     the GL path can be verified end to end before any game geometry exists.
 * @par  Copying and copyrights:
 *     This program is free software; you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of the License, or
 *     (at your option) any later version.
 */
/******************************************************************************/
#include "hwr_api.h"
#include "hwr_gl.h"
#include "hwr_internal.h"

void hwr_scene_begin(void)
{
    if (!hwr_is_ready())
        return;
    hwr_sync_viewport();
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void hwr_draw_frame(void)
{
    HwrCamera cam;

    if (!hwr_is_ready())
        return;

    if (hwr_source != NULL && hwr_source->begin_frame != NULL)
        hwr_source->begin_frame(hwr_source->ctx);

    /* Match the viewport to the camera the source reports, when available. */
    if (hwr_source != NULL && hwr_source->get_camera != NULL &&
        hwr_source->get_camera(hwr_source->ctx, &cam) == 0 &&
        cam.view_w > 0 && cam.view_h > 0) {
        glViewport(0, 0, cam.view_w, cam.view_h);
    }

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    /* Phase 3+: floor, faces, lights, sprites and the translucent pass are
     * dispatched from here as each module lands. */

    hwr_gl_check("hwr_draw_frame");
}

/* hwr_sprites_render / hwr_sprites_reset live in hwr_sprite.c */
