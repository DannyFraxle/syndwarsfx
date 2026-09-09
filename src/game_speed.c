/******************************************************************************/
// Syndicate Wars Fan Expansion, source port of the classic game from Bullfrog.
/******************************************************************************/
/** @file game_speed.c
 *     Control of the game speed.
 * @par Purpose:
 *     Variables and functions keeping game speed and frame rate at required pace.
 * @par Comment:
 *     None.
 * @author   Tomasz Lis
 * @date     10 Feb 2024 - 02 May 2024
 * @par  Copying and copyrights:
 *     This program is free software; you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of the License, or
 *     (at your option) any later version.
 */
/******************************************************************************/
#include "game_speed.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <SDL.h>
#include "bfkeybd.h"
#include "bfscreen.h"
#include "bftime.h"
#include "game.h"
#include "game_options.h"
#include "hwrender_glue.h"
#include "drawtext.h"
#include "engincolour.h"
#include "keyboard.h"
#include "swlog.h"

/******************************************************************************/

short frameskip = 0;

GameTurn drawturn = 1;

// Base simulation-tuning rate: all per-turn game values are authored against
// this rate (16 turns/sec). It does NOT change when the display runs faster;
// instead the sim is advanced in fractions of a turn (world_dt) - see below.
ushort game_num_fps = 16;

ushort fifties_per_gameturn = 3;

/* Frame-rate decoupling (FX3D).
 * target_fps  - desired display/present rate, from the [fx3d] ini (0 = uncapped).
 * world_dt    - fraction of a base 16Hz turn advanced this sub-tick; continuous
 *               quantities (movement etc.) scale by this in Phase 1.
 * dt_units    - whole logical turns elapsed this sub-tick (0 or 1); discrete
 *               per-turn counters and gameturn advance by this.
 * bullet_time - global slow-motion multiplier (1.0 = normal), used for the
 *               explosion bullet-time effect.
 * These implement "keep the float, round to int handed to the game": world_accum
 * carries the fraction, dt_units is the integer the game logic consumes. */
ushort target_fps = 60;
float  world_dt = 1.0f;
int    dt_units = 1;
int    new_logical_turn = 1;
float  bullet_time = 1.0f;
/* Fraction [0..1] of the way through the current 16Hz turn, advanced every
 * presented frame. The hardware renderer uses this to interpolate the camera
 * (and later objects) between the two most recent turn snapshots for smooth
 * motion at the display rate. 1.0 = no interpolation (snap to latest). */
float  g_interp_alpha = 1.0f;

/* --- Bullet-time-on-explosion state ([bullettime] section) --- */
static int    bt_enable = 0;
static float  bt_scale = 0.25f;
static int    bt_hold_ms = 700;
static int    bt_ramp_ms = 900;
static int    bt_min_intensity = 100;
static int    bt_range_tiles = 15;

static double bt_last_ms = 0.0;
static float  bt_hold_remain = 0.0f;
static float  bt_ramp_remain = 0.0f;

static float       world_accum = 0.0f;
/* High-resolution timestamp (milliseconds) of the previous sim tick. 0 = not
 * yet sampled / reset. Deliberately a double driven by the SDL performance
 * counter, NOT LbTimerClock: the latter is backed by C clock() which resolves
 * to ~15ms on Windows - far too coarse to measure a sub-frame interval at
 * 60fps+ (10-16ms). A coarse elapsed makes world_dt (and thus the interpolation
 * alpha) step in lumps of 0 / 15 / 31 ms, which is exactly the "unsteady and
 * glitchy" judder seen at high present rates. */
static double      sim_last_ms = 0.0;

/* Monotonic high-resolution wall clock in milliseconds. Sub-microsecond
 * precision, so a single frame interval is measured accurately. */
static double hires_now_ms(void)
{
    static Uint64 freq = 0;
    if (freq == 0)
        freq = SDL_GetPerformanceFrequency();
    if (freq == 0)              /* pathological: fall back to the coarse clock */
        return (double)LbTimerClock();
    return (double)SDL_GetPerformanceCounter() * 1000.0 / (double)freq;
}

void bullettime_config(int enable, float scale, int hold_ms, int ramp_ms,
    int min_intensity, int range_tiles)
{
    bt_enable = enable;
    if (scale < 0.05f) scale = 0.05f;
    if (scale > 1.0f) scale = 1.0f;
    bt_scale = scale;
    bt_hold_ms = (hold_ms > 0) ? hold_ms : 0;
    bt_ramp_ms = (ramp_ms > 0) ? ramp_ms : 1;
    bt_min_intensity = min_intensity;
    bt_range_tiles = (range_tiles > 0) ? range_tiles : 0;
}

int bullettime_range_tiles(void)
{
    return bt_range_tiles;
}

void bullettime_trigger(int intensity)
{
    if (!bt_enable || intensity < bt_min_intensity)
        return;
    bt_hold_remain = (float)bt_hold_ms;
    bt_ramp_remain = (float)bt_ramp_ms;
}

/* Advances bullet_time toward/away from bt_scale using real wall-clock time;
 * called once per presented frame (wait_next_displayframe), regardless of sim
 * mode, so the slow-motion dip plays out at real speed even when sim turns
 * are decoupled from the display rate. Takes effect from the NEXT frame's
 * is_game_turn_due() (this function runs after that call in the main loop). */
static void bullettime_tick(void)
{
    double now, elapsed;

    now = hires_now_ms();
    elapsed = (bt_last_ms == 0.0) ? 0.0 : (now - bt_last_ms);
    bt_last_ms = now;
    if (elapsed < 0.0 || elapsed > 250.0)
        elapsed = 0.0;

    if (bt_hold_remain > 0.0f) {
        bt_hold_remain -= (float)elapsed;
        bullet_time = bt_scale;
    } else if (bt_ramp_remain > 0.0f) {
        float t;
        bt_ramp_remain -= (float)elapsed;
        t = 1.0f - (bt_ramp_remain / (float)bt_ramp_ms);
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        bullet_time = bt_scale + (1.0f - bt_scale) * t;
    } else {
        bullet_time = 1.0f;
    }
}

float bullettime_intensity(void)
{
    return 1.0f - bullet_time;
}

/* Runtime FPS/TPS readout (for the [fx3d] ShowFPS overlay). */
int  show_fps_counter = 0;
static int         fps_present_count = 0;
static int         fps_logic_count = 0;
static TbClockMSec fps_report_time = 0;
static int         fps_display_val = 0;
static int         fps_logic_val = 0;
/* Frame-pacing diagnostics (published to the ShowFPS overlay). Measured with the
 * hi-res clock so we can see jitter directly: worst is the longest present
 * interval in the last second (a value >> 1000/refresh means a frame blew the
 * vblank budget - e.g. the heavy 16Hz draw_game turn-frame), avg is the mean. */
static double      frame_prev_ms = 0.0;
static double      frame_worst_ms = 0.0;
static double      frame_sum_ms = 0.0;
static int         frame_worst_val = 0;   /* published: worst frame ms (rounded) */
static int         frame_avg_val = 0;     /* published: avg frame ms (rounded) */

/* True when the sim should run decoupled from the 16Hz cap: in-engine gameplay
 * under the HW renderer with a target above the base rate (0 = uncapped). */
static TbBool sim_fast_mode(void)
{
    return hwrender_active()
        && (ingame.DisplayMode == DpM_ENGINEPLY)
        && (target_fps == 0 || target_fps > game_num_fps);
}

/******************************************************************************/

void frameskip_clip(void)
{
    if (frameskip > 512)
        frameskip = 512;
    else if (frameskip < 0)
        frameskip = 0;
}

void frameskip_increase(void)
{
    if (frameskip < 2)
        frameskip++;
    else if (frameskip < 16)
        frameskip += 2;
    else
        frameskip += (frameskip/3);
    frameskip_clip();
    //show_onscreen_msg(game_num_fps+frameskip, "Frame skip %d",frameskip);
}

void frameskip_decrease(void)
{
    if (frameskip <= 2)
        frameskip--;
    else if (frameskip <= 16)
        frameskip -= 2;
    else
        frameskip -= (frameskip/4);
    frameskip_clip();
    //show_onscreen_msg(game_num_fps+frameskip, "Frame skip %d",frameskip);
}

ubyte get_speed_control_inputs(void)
{
    ubyte did_inp;

    did_inp = GINPUT_NONE;
    if (is_gamekey_pressed(GKey_GAMESPEED_INC))
    {
        clear_gamekey_pressed(GKey_GAMESPEED_INC);
        frameskip_increase();
        did_inp |= GINPUT_DIRECT;
    }
    if (is_gamekey_pressed(GKey_GAMESPEED_DEC))
    {
        clear_gamekey_pressed(GKey_GAMESPEED_DEC);
        frameskip_decrease();
        did_inp |= GINPUT_DIRECT;
    }
    return did_inp;
}

void wait_next_gameturn(void)
{
    static TbClockMSec last_loop_time = 0;
    TbClockMSec curr_time = LbTimerClock();
    TbClockMSec sleep_end;

    if (frameskip == 0)
        sleep_end = last_loop_time + 1000/game_num_fps;
    else
        sleep_end = curr_time;
    // If we missed the normal sleep target (ie. there was a slowdown), reset the value and do not sleep
    if ((sleep_end < curr_time) || (sleep_end > curr_time + 1000/game_num_fps)) {
        LOGNO("missed FPS target, last frame time %ld too far from current %ld",
          (ulong)sleep_end, (ulong)curr_time);
        sleep_end = curr_time;
    }
    LbSleepUntil(sleep_end);
    last_loop_time = sleep_end;
}

TbBool is_game_turn_due(void)
{
    // In the classic (non-decoupled) path every loop iteration is a turn; pacing
    // is handled by wait_next_gameturn(). Preserve that exactly for menus, the
    // software renderer, pause/loading, etc.
    if (!sim_fast_mode())
    {
        sim_last_ms = 0.0;
        world_accum = 0.0f;
        world_dt = bullet_time;
        dt_units = 1;
        new_logical_turn = 1;
        g_interp_alpha = 1.0f;
        return true;
    }

    // Decoupled path: the loop spins at the display rate. Measure real elapsed
    // time and advance the world by that fraction of a base turn, so gameplay
    // speed stays identical regardless of the present rate. A whole logical turn
    // (dt_units) is due only when the accumulator crosses 1.0.
    {
        double now = hires_now_ms();
        double elapsed = (sim_last_ms == 0.0) ? 0.0 : (now - sim_last_ms);
        float turn_ms = 1000.0f / (float)game_num_fps;
        sim_last_ms = now;
        if (elapsed < 0.0)
            elapsed = 0.0;
        if (elapsed > 250.0)        // hitch guard: never advance more than ~4 turns
            elapsed = 250.0;
        world_dt = ((float)elapsed / turn_ms) * bullet_time;
        world_accum += world_dt;
        dt_units = (int)world_accum;
        world_accum -= (float)dt_units;
        // Interpolation fraction is the leftover AFTER consuming whole turns.
        // The main loop runs draw_game (which captures the new renderer
        // snapshot) before the present on turn frames, so the leftover maps
        // onto the fresh snapshot pair. The previous scheme (alpha taken
        // pre-consume, clamped to 1.0) discarded the overshoot fraction every
        // turn boundary - motion stalled then jumped, a ~4Hz micro-hitch.
        g_interp_alpha = world_accum;
        if (g_interp_alpha > 1.0f) g_interp_alpha = 1.0f;
        if (g_interp_alpha < 0.0f) g_interp_alpha = 0.0f;
        if (dt_units > 0)
            fps_logic_count += dt_units;
    }
    // Phase 1: the sim runs every presented frame. Continuous quantities scale by
    // world_dt (the fraction of a base turn elapsed); discrete per-turn logic
    // (timers, RNG events, gameturn-mask blocks, animation) runs only when a
    // whole logical turn is due, flagged by new_logical_turn.
    new_logical_turn = (dt_units > 0);
    return true;
}

void wait_next_displayframe(void)
{
    ushort fps;
    static TbClockMSec last_frame = 0;
    TbClockMSec now, sleep_end, frame_ms;

    // Advance the bullet-time slow-motion dip (independent of sim mode).
    bullettime_tick();

    // Count every presented frame for the FPS readout.
    fps_present_count++;
    now = LbTimerClock();
    // Per-frame interval via the hi-res clock, so the overlay can expose pacing
    // jitter that the coarse 1s counter hides. This is called once per presented
    // frame, so consecutive samples are the true present interval.
    {
        double hnow = hires_now_ms();
        if (frame_prev_ms != 0.0)
        {
            double dms = hnow - frame_prev_ms;
            if (dms > 0.0 && dms < 1000.0)   // ignore first sample / long stalls
            {
                if (dms > frame_worst_ms)
                    frame_worst_ms = dms;
                frame_sum_ms += dms;
            }
        }
        frame_prev_ms = hnow;
    }
    if (fps_report_time == 0)
        fps_report_time = now;
    if ((long)(now - fps_report_time) >= 1000)
    {
        fps_display_val = fps_present_count;
        fps_logic_val = fps_logic_count;
        frame_worst_val = (int)(frame_worst_ms + 0.5);
        frame_avg_val = (fps_present_count > 0)
            ? (int)(frame_sum_ms / (double)fps_present_count + 0.5) : 0;
        frame_worst_ms = 0.0;
        frame_sum_ms = 0.0;
        fps_present_count = 0;
        fps_logic_count = 0;
        fps_report_time = now;
    }

    if (!sim_fast_mode())
    {
        // Classic pacing (menus, software renderer, pause, loading).
        last_frame = 0;
        wait_next_gameturn();
        return;
    }

    // Pacing in fast mode.
    // When vsync is on, the buffer swap (SDL_GL_SwapWindow) blocks precisely to
    // the monitor refresh - that IS the frame pace. Adding a software sleep on
    // top only fights the coarse OS timer (LbTimerClock/LbSleepUntil resolve to
    // ~15ms), which quantises the frame time and locks the game to ~30fps even
    // when the GPU is nearly idle. So with vsync on we do not sleep at all and
    // let the swap set the rate (monitor refresh, e.g. 60/120/144).
    if (fx3d_vsync)
    {
        last_frame = 0;
        return;
    }

    // Vsync off: the render rate is the natural limiter. Only software-pace when
    // an explicit sub-render cap is requested (TargetFPS > 0); 0 = run free.
    fps = target_fps;
    if (fps == 0)
    {
        last_frame = 0;
        return;
    }
    frame_ms = 1000 / fps;
    sleep_end = last_frame + frame_ms;
    // If we missed the target (slowdown), reset and do not sleep.
    if ((sleep_end < now) || (sleep_end > now + frame_ms))
        sleep_end = now;
    LbSleepUntil(sleep_end);
    last_frame = sleep_end;
}

/** Draw the FPS/TPS overlay (called from the draw path when ShowFPS is on). */
void draw_fps_counter(void)
{
    char msg[64];
    if (!show_fps_counter)
        return;
    /* Only draw where the engine WScreen/fonts/colours are known valid (same
     * context as the debug HUD); avoids touching WScreen in menu/intro modes. */
    if (ingame.DisplayMode != DpM_ENGINEPLY)
        return;
    if (lbDisplay.WScreen == NULL)
        return;
    snprintf(msg, sizeof(msg), "FPS %d  TPS %d  ms avg %d worst %d  BT %.2f",
        fps_display_val, fps_logic_val, frame_avg_val, frame_worst_val, bullet_time);
    draw_text(8, 8, msg, colour_lookup[ColLU_WHITE]);
    {
        /* Report the most recent building whose Thing Y moved (hovering/animating
         * or collapsing) so we can identify the subtype to make dynamic. */
        extern int hwr_dbg_hover_sub, hwr_dbg_hover_state, hwr_dbg_hover_y;
        if (hwr_dbg_hover_sub >= 0) {
            snprintf(msg, sizeof(msg), "MOVING BLD SUB=%d ST=%d Y=%d",
                hwr_dbg_hover_sub, hwr_dbg_hover_state, hwr_dbg_hover_y);
            draw_text(8, 20, msg, colour_lookup[ColLU_WHITE]);
        }
    }
}

/**
 * Checks if the game screen needs redrawing.
 */
TbBool display_needs_redraw_this_turn(void)
{
    if ( (frameskip == 0) || ((gameturn % frameskip) == 0))
        return true;
    return false;
}

void update_tick_time(void)
{
    ulong tick_time = clock();
    tick_time = tick_time / 100;
    curr_tick_time = tick_time;
    if (tick_time != prev_tick_time)
    {
        ulong tmp;
        tmp = gameturn - prev_gameturn;
        prev_gameturn = gameturn;
        turns_delta = tmp;
    }
    if ( turns_delta != 0 ) {
        fifties_per_gameturn = 800 / turns_delta;
    } else {
        fifties_per_gameturn = 50;
    }
    if ( in_network_game )
        fifties_per_gameturn = 80;
    if ( fifties_per_gameturn > 400 )
        fifties_per_gameturn = 400;
    prev_tick_time = curr_tick_time;
}

/******************************************************************************/
