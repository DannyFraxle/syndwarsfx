/******************************************************************************/
// Syndicate Wars Fan Expansion, source port of the classic game from Bullfrog.
/******************************************************************************/
/** @file hwrender_glue.c
 *     Host-side glue to the FX3D OpenGL hardware renderer.
 * @par Purpose:
 *     See hwrender_glue.h. The hardware renderer is always built and enabled
 *     by default in this build, so all entry points are fully functional.
 * @par  Copying and copyrights:
 *     This program is free software; you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of the License, or
 *     (at your option) any later version.
 */
/******************************************************************************/
#include "hwrender_glue.h"

/* The hardware renderer is enabled by default. The --hwrender CLI flag is
 * retained for backward compatibility but is no longer required. */
static TbBool hwr_glue_requested = true;

/* FX3D config, with defaults (overridden by rules.ini [fx3d] then CLI). */
int fx3d_aa_samples = 0;
int fx3d_filter_ground = 1;
int fx3d_filter_objects = 1;
int fx3d_filter_sprites = 0;

/* Frame-rate: default 60fps target, vsync on, FPS overlay off. */
int fx3d_target_fps = 60;
int fx3d_vsync = 1;
int fx3d_show_fps = 0;
int fx3d_debug_things = 0;

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

    /* Publish frame-rate settings to the game-speed layer. */
    {
        extern unsigned short target_fps;   /* game_speed.c */
        extern int show_fps_counter;        /* game_speed.c */
        if (fx3d_target_fps < 0) fx3d_target_fps = 0;
        target_fps = (unsigned short)fx3d_target_fps;
        show_fps_counter = fx3d_show_fps;
    }
    {
        extern TbBool debug_hud_things;   /* thing_debug.c */
        debug_hud_things = fx3d_debug_things ? true : false;
    }
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
#include "hwr_lights.h"
#include "hwr_sprite.h"
#include "hwr_tuning.h"
#include "hwr_thingbrowse.h"
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
/* User-toggled (spacebar): true forces the software renderer even though the
 * GL context stays up. Keeping the context alive avoids re-creating every
 * phase module's lazily-allocated GL objects, and LbScreenSwap() has no
 * software-surface fallback for a GL window, so presentation always goes
 * through the GL blit path regardless of this flag. */
static TbBool hwr_sw_forced = false;

/* Set by hwrender_floor_gate() when the engine view was gated for 3D this
 * frame; tells the present path to render the 3D scene and key the overlay. */
int hwr_floor_gated_frame = 0;
/* When set, forces the WScreen composite to full opacity so popup controls
 * (e.g. pause screen) stay solid while the box fill tint comes from the GL
 * overlay pass instead of alpha-blending the whole WScreen. */
static int hwr_opaque_present = 0;
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

    if (ingame.DisplayMode == DpM_ENGINEPLY && hwrender_active()) {
        /* In-game engine view: render the 3D scene, then composite the software
         * objects/sprites/HUD on top, discarding the key so the floor shows.
         * Driven by the display mode (not a per-frame flag) so skipped-redraw
         * frames stay consistent instead of flashing the key colour. */
        {
            HwrLightDefaults d = hwr_lights_defaults();
            int dw = 0, dh = 0;
            hwr_drawable_size(&dw, &dh);
            hwr_sun_config(d.sun_enable, d.sun_bright, d.sun_ambient,
                d.sun_azimuth, d.sun_elevation, d.sun_pcf,
                d.sun_bias, d.sun_slope, d.sun_units, d.sun_debug,
                d.sun_haze);
            hwr_ssao_config(d.ssao_enable, d.ssao_radius, d.ssao_world,
                d.ssao_strength, d.ssao_bias, d.ssao_debug);
            hwr_transparent_config(d.transp_enable, d.transp_alpha,
                deep_radar_surface_col);
            hwr_sprites_trans_config(d.transp_sprite_enable, d.transp_sprite_alpha);
            hwr_thingno_debug(d.thingno_debug);
            hwr_sprites_debug(d.sprite_debug);
            hwr_scene_begin();
            hwr_sun_shadow_pass();           /* depth from sun -> shadow map */
            hwr_ssao_begin(dw, dh);          /* binds the G-buffer (or back buffer) */
            hwr_floor_render(pal, fx3d_filter_ground);
            hwr_shadows_render();
            hwr_faces_render(pal, fx3d_filter_objects);
            hwr_reflect_render(pal);          /* chameleon vehicle paint */
            hwr_sprites_render(pal, fx3d_filter_sprites);
            hwr_transparent_render(pal, fx3d_filter_objects);  /* blended faces (Phase 8) */
            hwr_sprites_trans_render(pal, fx3d_filter_sprites); /* blended sprites (Phase 8) */
            hwr_ssao_resolve();              /* composites colour*AO to back buffer */
            hwr_overlay_render();            /* screen-space tinted overlays (shield/blast) */
            hwr_thingno_render();            /* overlay ThingNo debug labels */
            hwr_sprites_debug_render();      /* overlay sprite debug labels */
            hwr_thingbrowse_render();        /* thing category browser (F5) */
            hwr_tuning_render();             /* lighting tuning panel (F7) */
        }
        /* Transparent HUD option (PanelPermutation == -1): blend the keyed HUD
         * overlay over the 3D scene. The software path's own transparency
         * blends against the engine framebuffer, which here is just the key, so
         * it can't see the 3D — do the blend at composite time instead. */
        /* PanelPermutation == -1: luma-split composite — dark pixels (panel
         * background fill) blended at 0.5 alpha, bright pixels (outlines,
         * numbers, powerbar, map) fully opaque. bg_luma threshold 0.18 sits
         * between the dark purple fill (luma ~0.03) and bright cyan outlines
         * (luma ~0.6); adjust in fx3d_lights.ini if needed.
         * Exception: hwr_opaque_present (pause popup) forces full opacity. */
        if (ingame.PanelPermutation == -1 && !hwr_opaque_present) {
            hwr_present_indexed_keyed_luma((const unsigned char *)lbDisplay.WScreen,
                w, h, w, pal, HWR_KEY_INDEX, 0.5f, 0.38f);
        } else {
            hwr_present_indexed_keyed((const unsigned char *)lbDisplay.WScreen,
                w, h, w, pal, HWR_KEY_INDEX);
        }
    } else {
        /* Menus / non-engine screens: plain full blit. */
        hwr_present_indexed((const unsigned char *)lbDisplay.WScreen, w, h, w,
            pal);
    }
    hwr_present();
    hwr_floor_gated_frame = 0;
}

/* Declarations from libswrender for the sprite-suppression gate. */
extern int engine_hwr_suppress_sprites;
/* Target-box list (hud_target.c); cleared per frame, refilled during HUD draw,
 * consumed by the GL overlay pass. */
extern int hwr_tgtbox_count;
/* Pause popup box fills (fepause.c); consumed by the GL overlay pass. */
extern int hwr_pause_box_count;

TbBool hwrender_floor_gate(void)
{
    int w, h;
    /* Reset the sprite-suppression gate; only set it below if we collect. */
    engine_hwr_suppress_sprites = 0;
    hwr_tgtbox_count = 0;
    hwr_pause_box_count = 0;
    if (!hwrender_active())
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
    /* Collect Thing-based sprites for HW billboard rendering, set the
     * suppress-sprites gate so the SW drawlist exec skips them. */
    hwr_sw_collect_sprites();
    engine_hwr_suppress_sprites = 1;
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
    return hwr_glue_active && !hwr_sw_forced;
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
        cfg.vsync          = fx3d_vsync;
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

void hwrender_toggle(void)
{
    if (!hwr_glue_active)
        return;
    hwr_sw_forced = !hwr_sw_forced;
    LOGSYNC("FX3D: %s renderer active", hwr_sw_forced ? "software" : "hardware");
}

void hwrender_set_opaque_present(int on)
{
#if defined(HAVE_HWRENDER)
    hwr_opaque_present = on;
#else
    (void)on;
#endif
}

#endif /* HAVE_HWRENDER */
