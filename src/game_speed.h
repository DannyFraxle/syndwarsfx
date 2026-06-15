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
extern ulong turns_delta;
extern ushort fifties_per_gameturn;

/** Amount of frames per second drawn by the game, and also aount of game
 * turns per second. */
extern ushort game_num_fps;

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
