/******************************************************************************/
// Syndicate Wars Fan Expansion, source port of the classic game from Bullfrog.
/******************************************************************************/
/** @file game_speed.h
 *     Header file for game_speed.c.
 * @par Purpose:
 *     Control of the game speed.
 * @par Comment:
 *     Just a header file - #defines, typedefs, function prototypes etc.
 * @author   Tomasz Lis
 * @date     10 Feb 2024 - 02 May 2024
 * @par  Copying and copyrights:
 *     This program is free software; you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of the License, or
 *     (at your option) any later version.
 */
/******************************************************************************/
#ifndef GAME_SPEED_H
#define GAME_SPEED_H

#include "bftypes.h"
#include "game_bstype.h"

#ifdef __cplusplus
extern "C" {
#endif
/******************************************************************************/

/******************************************************************************/
extern ulong curr_tick_time;
extern ulong prev_tick_time;
extern GameTurn gameturn;
extern GameTurn prev_gameturn;

/** Counter of frames drawn to the screen.
 *
 * Advances once per drawn frame. It is the counter for things which only
 * exist while drawing - marking which elements were already drawn within the
 * current frame, for instance. Nothing in the simulation reads it, so drawing
 * code should use it rather than `gameturn` whenever it only needs to tell
 * one frame from the next.
 */
extern GameTurn drawturn;
extern ulong turns_delta;
extern ushort fifties_per_gameturn;

/** Base simulation-tuning rate (turns/sec) that per-turn game values are
 * authored against. Stays fixed even when the display runs faster. */
extern ushort game_num_fps;

/** Desired display/present rate (FX3D), from the [fx3d] ini; 0 = uncapped. */
extern ushort target_fps;
/** Fraction of a base 16Hz turn advanced this sub-tick (continuous scaling). */
extern float world_dt;
/** Whole logical turns elapsed this sub-tick (discrete per-turn scaling). */
extern int dt_units;
/** 1 on frames where a whole logical (16Hz) turn is due: discrete per-turn logic
 * (timers, RNG events, gameturn-mask blocks, animation) runs only on these. */
extern int new_logical_turn;
/** Global slow-motion multiplier (1.0 = normal); drives explosion bullet-time. */
extern float bullet_time;
/** Fraction [0..1] into the current 16Hz turn; renderer interpolation factor. */
extern float g_interp_alpha;
/** When set, draw the on-screen FPS/TPS readout ([fx3d] ShowFPS). */
extern int show_fps_counter;

/** Draw the FPS/TPS overlay; no-op unless show_fps_counter is set. */
void draw_fps_counter(void);

/* --- Bullet-time-on-explosion ([bullettime] section, fx3d.ini) ----- */

/** Configure the bullet-time-on-explosion effect. scale is the world_dt
 *  multiplier while fully dipped (e.g. 0.25 = quarter speed); hold_ms is how
 *  long it stays at that depth before easing back; ramp_ms is the ease-back
 *  duration; min_intensity is the do_shockwave() intensity threshold that
 *  triggers it (filters out plain bullet-impact shockwaves, which pass a
 *  small fixed intensity); range_tiles caps how far from the local player's
 *  controlled agent an explosion can be and still trigger it (0 = no range
 *  check, intensity gate only). */
void bullettime_config(int enable, float scale, int hold_ms, int ramp_ms,
    int min_intensity, int range_tiles);

/** Trigger bullet-time from an explosion of the given shockwave intensity;
 *  no-op if disabled or below the configured threshold. Retriggering while
 *  already active refreshes the hold timer instead of stacking. */
void bullettime_trigger(int intensity);

/** Configured trigger range in tiles (see bullettime_config); 0 = unlimited.
 *  Exposed so the trigger site (do_shockwave in bmbang.c) can check the
 *  explosion's distance from the local player before calling
 *  bullettime_trigger(). */
int bullettime_range_tiles(void);

/** 0 (normal speed) .. <1 (deep slow-motion): how far into the bullet-time
 *  dip the current frame is. Drives the renderer's screen filter. */
float bullettime_intensity(void);

/**
 * Handles game speed control inputs.
 * @return Returns true if packet was created, false otherwise.
 */
ubyte get_speed_control_inputs(void);

void wait_next_gameturn(void);

/**
 * Whether a game-logic turn should run on this iteration of the main loop.
 * The simulation currently advances at a single fixed rate (game_num_fps), so
 * every loop iteration is a turn. This is the seam where display frames will be
 * decoupled from sim turns (see the TODO on game_num_fps); until then it simply
 * reports true so behaviour matches the pre-decoupling main loop.
 */
TbBool is_game_turn_due(void);

/**
 * Paces the main loop to the next display frame. With rendering and simulation
 * still locked together, this is the existing per-turn wait.
 */
void wait_next_displayframe(void);

TbBool display_needs_redraw_this_turn(void);
void update_tick_time(void);

/******************************************************************************/
#ifdef __cplusplus
}
#endif
#endif
