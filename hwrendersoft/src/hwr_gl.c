/******************************************************************************/
// Syndicate Wars FX3D - OpenGL hardware renderer for Bullfrog titles.
/******************************************************************************/
/**                        2026 danny@fraxle.net                             **/
/******************************************************************************/
/** @file hwr_gl.c
 *     Runtime resolution of the GL 3.3 core entry points.
 * @par  Copying and copyrights:
 *     This program is free software; you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of the License, or
 *     (at your option) any later version.
 */
/******************************************************************************/
#include "hwr_gl.h"
#include "hwr_api.h"
#include "hwr_internal.h"

#include <SDL.h>

/* Define storage for every entry point declared in hwr_gl.h. */
#define HWR_GL_FUNC(ret, name, args) PFN_##name name = NULL;
#include "hwr_gl_funcs.inc"
#undef HWR_GL_FUNC

int hwr_gl_load(void)
{
    /* Resolve each pointer; record the first that fails to load. */
    #define HWR_GL_FUNC(ret, name, args) \
        name = (PFN_##name)SDL_GL_GetProcAddress(#name); \
        if (name == NULL) { \
            hwr_set_error("GL entry point not available: %s", #name); \
            return HWR_ERROR; \
        }
    #include "hwr_gl_funcs.inc"
    #undef HWR_GL_FUNC
    return HWR_OK;
}

int hwr_gl_check(const char *where)
{
    GLenum err;
    int seen = 0;
    if (glGetError == NULL)
        return 0;
    while ((err = glGetError()) != GL_NO_ERROR) {
        seen = 1;
        hwr_set_error("GL error 0x%04x at %s", (unsigned)err,
            where ? where : "?");
    }
    return seen;
}
