/******************************************************************************/
// Syndicate Wars FX3D - OpenGL hardware renderer for Bullfrog titles.
/******************************************************************************/
/**                        2026 danny@fraxle.net                             **/
/******************************************************************************/
/** @file hwr_init.c
 *     GL context lifecycle, error reporting and the bound scene source.
 * @par Purpose:
 *     Creates a GL 3.3 core context on the host's SDL window, loads the GL
 *     entry points, and owns the small amount of global renderer state. The
 *     actual per-frame drawing lives in hwr_draw.c and the phase modules.
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

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <SDL.h>

const HwrSceneSource *hwr_source = NULL;

static char hwr_error_buf[256] = "no error";
static int  hwr_ready = 0;
static HwrConfig hwr_cfg = { 0, 1, 1, 0, 1 };

void hwr_set_config(const HwrConfig *cfg)
{
    if (cfg != NULL)
        hwr_cfg = *cfg;
}

const HwrConfig *hwr_config(void)
{
    return &hwr_cfg;
}

static SDL_Window   *hwr_window = NULL;
static SDL_GLContext hwr_context = NULL;

void hwr_set_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(hwr_error_buf, sizeof(hwr_error_buf), fmt, ap);
    va_end(ap);
}

const char *hwr_last_error(void)
{
    return hwr_error_buf;
}

int hwr_is_ready(void)
{
    return hwr_ready;
}

void hwr_set_source(const HwrSceneSource *src)
{
    hwr_source = src;
}

int hwr_init(void *sdl_window)
{
    if (hwr_ready)
        return HWR_OK;
    if (sdl_window == NULL) {
        hwr_set_error("hwr_init: NULL window");
        return HWR_ERROR;
    }
    hwr_window = (SDL_Window *)sdl_window;

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
        SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

    hwr_context = SDL_GL_CreateContext(hwr_window);
    if (hwr_context == NULL) {
        hwr_set_error("SDL_GL_CreateContext failed: %s", SDL_GetError());
        return HWR_ERROR;
    }
    if (SDL_GL_MakeCurrent(hwr_window, hwr_context) != 0) {
        hwr_set_error("SDL_GL_MakeCurrent failed: %s", SDL_GetError());
        SDL_GL_DeleteContext(hwr_context);
        hwr_context = NULL;
        return HWR_ERROR;
    }

    if (hwr_gl_load() != HWR_OK) {
        /* hwr_gl_load already set the error text. */
        SDL_GL_DeleteContext(hwr_context);
        hwr_context = NULL;
        return HWR_ERROR;
    }

    /* Vsync per config. Hard vsync (1) is tear-free; the previously-tried
     * adaptive vsync (-1) removed the 30fps stall but tears on any late frame,
     * which showed up as a "wave" band drifting down the screen. With the
     * interpolation now driven by a hi-res clock (see game_speed.c) motion is
     * smooth, so we keep hard vsync for a clean image and use the ShowFPS
     * worst-ms readout to tell whether a frame is actually missing the vblank
     * budget (which would be the real cause of any residual 30fps lock).
     * Ignore failure (some drivers reject the request). */
    SDL_GL_SetSwapInterval(hwr_cfg.vsync ? 1 : 0);

    glDisable(GL_CULL_FACE);   /* mesh winding is inconsistent in the data */
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    if (hwr_cfg.aa_samples > 0)
        glEnable(GL_MULTISAMPLE);  /* MSAA pixel format requested at window creation */
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);

    hwr_ready = 1;
    return HWR_OK;
}

void hwr_sync_viewport(void)
{
    int dw = 0, dh = 0;
    if (!hwr_ready || hwr_window == NULL)
        return;
    SDL_GL_GetDrawableSize(hwr_window, &dw, &dh);
    if (dw > 0 && dh > 0)
        glViewport(0, 0, dw, dh);
}

void hwr_drawable_size(int *w, int *h)
{
    int dw = 0, dh = 0;
    if (hwr_ready && hwr_window != NULL)
        SDL_GL_GetDrawableSize(hwr_window, &dw, &dh);
    if (w != NULL) *w = dw;
    if (h != NULL) *h = dh;
}

void hwr_present(void)
{
    if (hwr_ready && hwr_window != NULL)
        SDL_GL_SwapWindow(hwr_window);
}

void hwr_shutdown(void)
{
    if (hwr_context != NULL) {
        SDL_GL_DeleteContext(hwr_context);
        hwr_context = NULL;
    }
    hwr_window = NULL;
    hwr_ready = 0;
}
