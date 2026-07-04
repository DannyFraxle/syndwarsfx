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
float  bullet_time = 1.0f;

static float       world_accum = 0.0f;
static TbClockMSec sim_last_time = 0;

/* Runtime FPS/TPS readout (for the [fx3d] ShowFPS overlay). */
int  show_fps_counter = 0;
static int         fps_present_count = 0;
static int         fps_logic_count = 0;
static TbClockMSec fps_report_time = 0;
static int         fps_display_val = 0;
static int         fps_logic_val = 0;

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
        sim_last_time = 0;
        world_accum = 0.0f;
        world_dt = bullet_time;
        dt_units = 1;
        return true;
    }

    // Decoupled path: the loop spins at the display rate. Measure real elapsed
    // time and advance the world by that fraction of a base turn, so gameplay
    // speed stays identical regardless of the present rate. A whole logical turn
    // (dt_units) is due only when the accumulator crosses 1.0.
    {
        TbClockMSec now = LbTimerClock();
        long elapsed = (sim_last_time == 0) ? 0 : (long)(now - sim_last_time);
        float turn_ms = 1000.0f / (float)game_num_fps;
        sim_last_time = now;
        if (elapsed < 0)
            elapsed = 0;
        if (elapsed > 250)          // hitch guard: never advance more than ~4 turns
            elapsed = 250;
        world_dt = ((float)elapsed / turn_ms) * bullet_time;
        world_accum += world_dt;
        dt_units = (int)world_accum;
        world_accum -= (float)dt_units;
        if (dt_units > 0)
            fps_logic_count += dt_units;
    }
    // Phase 0: run the (unscaled) sim only on whole logical turns, so gameplay
    // speed is unchanged while presentation runs faster. Phase 1 will move the
    // sim to every sub-tick and scale it by world_dt.
    return (dt_units > 0);
}

void wait_next_displayframe(void)
{
    ushort fps;
    static TbClockMSec last_frame = 0;
    TbClockMSec now, sleep_end, frame_ms;

    // Count every presented frame for the FPS readout.
    fps_present_count++;
    now = LbTimerClock();
    if (fps_report_time == 0)
        fps_report_time = now;
    if ((long)(now - fps_report_time) >= 1000)
    {
        fps_display_val = fps_present_count;
        fps_logic_val = fps_logic_count;
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
    snprintf(msg, sizeof(msg), "FPS %d  TPS %d", fps_display_val, fps_logic_val);
    draw_text(8, 8, msg, colour_lookup[ColLU_WHITE]);
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
