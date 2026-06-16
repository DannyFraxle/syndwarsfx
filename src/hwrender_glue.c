/******************************************************************************/
// Syndicate Wars Fan Expansion, source port of the classic game from Bullfrog.
/******************************************************************************/
/** @file hwrender_glue.c
 *     Host-side glue to the optional FX3D OpenGL hardware renderer.
 * @par Purpose:
 *     See hwrender_glue.h. When the build is configured without
 *     --enable-hwrender, every entry point here is a cheap stub so the rest of
 *     the game links and behaves exactly as before.
 * @par  Copying and copyrights:
 *     This program is free software; you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of the License, or
 *     (at your option) any later version.
 */
/******************************************************************************/
#include "hwrender_glue.h"

/* The CLI request is tracked regardless of build so option parsing is uniform;
 * it simply never activates anything in a software-only build. */
static TbBool hwr_glue_requested = false;

/* FX3D config, with defaults (overridden by rules.ini [fx3d] then CLI). */
int fx3d_aa_samples = 0;
int fx3d_filter_ground = 1;
int fx3d_filter_objects = 1;
int fx3d_filter_sprites = 0;

int fx3d_cli_aa = -1;
int fx3d_cli_filter_ground = -1;
int fx3d_cli_filter_objects = -1;
int fx3d_cli_filter_sprites = -1;

void fx3d_config_finalize(void)
{
    /* Command line wins over rules.ini for any explicitly-set option. */
    if (fx3d_cli_aa >= 0)             fx3d_aa_samples    = fx3d_cli_aa;
    if (fx3d_cli_filter_ground >= 0)  fx3d_filter_ground = fx3d_cli_filter_ground;
    if (fx3d_cli_filter_objects >= 0) fx3d_filter_objects = fx3d_cli_filter_objects;
    if (fx3d_cli_filter_sprites >= 0) fx3d_filter_sprites = fx3d_cli_filter_sprites;
#if defined(HAVE_HWRENDER)
    {
        /* Publish the MSAA sample count so the GL window is created with a
         * multisample pixel format. */
        extern int lbGLMultisampleSamples;
        lbGLMultisampleSamples = fx3d_aa_samples;
    }
#endif
}

#if defined(HAVE_HWRENDER)

#include "hwr_api.h"
#include "hwr_source_sw.h"
#include "bfscreen.h"
#include "game_options.h"
#include "swlog.h"

#include <string.h>
#include <SDL.h>

/* Set in bflibrary's SDL2 screen backend so the window is created with the
 * SDL_WINDOW_OPENGL flag (required before a GL context can be made current). */
extern int lbUseOpenGLWindow;
/* The window created by the bflibrary SDL2 backend. */
extern SDL_Window *lbWindow;
/* Presentation hook called by bflibrary's swap routines while a GL window is
 * in use (covers e.g. palette fades that swap outside the main loop). */
extern void (*lbScreenSwapHook)(void);
/* The active palette as set by LbPaletteSet - the authoritative palette the
 * software path blits with (full-range 8-bit). Every screen and fade funnels
 * through it, unlike the various game-side palette arrays (display_palette,
 * anim_palette, vga_pal, ...). 256 SDL_Color entries. */
extern SDL_Color lbPaletteColors[256];

static TbBool hwr_glue_active = false;

/* Set by hwrender_floor_gate() when the engine view was gated for 3D this
 * frame; tells the present path to render the 3D scene and key the overlay. */
int hwr_floor_gated_frame = 0;
/* Key index the source side discards (kept in sync with HWR_KEY_INDEX). */
extern int hwr_sw_key_index;

/* Present the current 8-bit WScreen through the GL pipeline and swap. Shared by
 * the main-loop present and the bflibrary swap hook. */
static void glue_present(void)
{
    int w = lbDisplay.GraphicsScreenWidth;
    int h = lbDisplay.GraphicsScreenHeight;
    unsigned char pal[256 * 3];
    int c;

    if (!hwr_glue_active)
        return;
    if (lbDisplay.WScreen == NULL || w <= 0 || h <= 0) {
        hwr_draw_frame();
        hwr_present();
        hwr_floor_gated_frame = 0;
        return;
    }

    for (c = 0; c < 256; c++) {
        pal[c * 3 + 0] = lbPaletteColors[c].r;
        pal[c * 3 + 1] = lbPaletteColors[c].g;
        pal[c * 3 + 2] = lbPaletteColors[c].b;
    }

    if (ingame.DisplayMode == DpM_ENGINEPLY) {
        /* In-game engine view: render the 3D scene, then composite the software
         * objects/sprites/HUD on top, discarding the key so the floor shows.
         * Driven by the display mode (not a per-frame flag) so skipped-redraw
         * frames stay consistent instead of flashing the key colour. */
        hwr_scene_begin();
        hwr_floor_render(pal, fx3d_filter_ground);
        hwr_faces_render(pal, fx3d_filter_objects);
        hwr_present_indexed_keyed((const unsigned char *)lbDisplay.WScreen,
            w, h, w, pal, HWR_KEY_INDEX);
    } else {
        /* Menus / non-engine screens: plain full blit. */
        hwr_present_indexed((const unsigned char *)lbDisplay.WScreen, w, h, w,
            pal);
    }
    hwr_present();
    hwr_floor_gated_frame = 0;
}

TbBool hwrender_floor_gate(void)
{
    int w, h;
    if (!hwr_glue_active)
        return false;
    w = lbDisplay.GraphicsScreenWidth;
    h = lbDisplay.GraphicsScreenHeight;
    /* Snapshot the engine camera now, while the projection globals are valid for
     * the floor (later BAT/billboard sub-renders overwrite them). */
    hwr_sw_capture();
    if (lbDisplay.WScreen != NULL && w > 0 && h > 0) {
        /* Fill the engine framebuffer with the key; SW objects/sprites/HUD draw
         * over it, the 3D floor shows through it at present time. */
        memset(lbDisplay.WScreen, HWR_KEY_INDEX, (size_t)w * h);
    }
    hwr_floor_gated_frame = 1;
    return true;
}

void hwrender_set_requested(TbBool on)
{
    hwr_glue_requested = on;
    /* Request a GL-capable window now, before the video mode is set up. */
    lbUseOpenGLWindow = on ? 1 : 0;
    if (on) {
        /* A click that brings the window into focus should also be delivered to
         * the game, so the first click after launch isn't silently swallowed. */
        SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
    }
}

TbBool hwrender_requested(void)
{
    return hwr_glue_requested;
}

TbBool hwrender_active(void)
{
    return hwr_glue_active;
}

TbBool hwrender_startup(int view_w, int view_h)
{
    if (!hwr_glue_requested || hwr_glue_active)
        return hwr_glue_active;
    if (lbWindow == NULL) {
        LOGERR("FX3D: no SDL window to attach a GL context to");
        return false;
    }
    {
        HwrConfig cfg;
        cfg.aa_samples     = fx3d_aa_samples;
        cfg.filter_ground  = fx3d_filter_ground;
        cfg.filter_objects = fx3d_filter_objects;
        cfg.filter_sprites = fx3d_filter_sprites;
        hwr_set_config(&cfg);
    }
    if (hwr_init(lbWindow) != HWR_OK) {
        LOGERR("FX3D: hardware renderer init failed: %s", hwr_last_error());
        return false;
    }
    hwr_set_source(hwr_sw_source(view_w, view_h));
    hwr_sw_key_index = HWR_KEY_INDEX;
    lbScreenSwapHook = glue_present;
    hwr_glue_active = true;
    /* Make sure the GL window is shown, raised and holds input focus from the
     * start; otherwise (e.g. launched from a terminal that keeps foreground
     * focus) the first click is consumed just focusing the window, and input
     * stays stale until the user alt-tabs. */
    SDL_ShowWindow(lbWindow);
    SDL_RaiseWindow(lbWindow);
    SDL_SetWindowInputFocus(lbWindow);
    SDL_PumpEvents();
    LOGSYNC("FX3D: hardware renderer active (%dx%d)", view_w, view_h);
    return true;
}

TbBool hwrender_present_frame(void)
{
    if (!hwr_glue_active)
        return false;
    /* WScreen is a persistent app-owned 8-bit buffer (allocated via the engine
     * memory table as *W_SCREEN), tightly packed at GraphicsScreenWidth and
     * valid regardless of surface lock state in this build. */
    glue_present();
    return true;
}

void hwrender_shutdown(void)
{
    if (hwr_glue_active) {
        lbScreenSwapHook = NULL;
        hwr_shutdown();
        hwr_glue_active = false;
    }
}

#else /* !HAVE_HWRENDER : software-only build, everything is inert */

void   hwrender_set_requested(TbBool on)   { hwr_glue_requested = on; }
TbBool hwrender_requested(void)            { return false; }
TbBool hwrender_active(void)               { return false; }
TbBool hwrender_floor_gate(void)           { return false; }
TbBool hwrender_startup(int w, int h)      { (void)w; (void)h; return false; }
TbBool hwrender_present_frame(void)        { return false; }
void   hwrender_shutdown(void)             { }

#endif
