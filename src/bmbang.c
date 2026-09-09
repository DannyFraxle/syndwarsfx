/******************************************************************************/
// Syndicate Wars Fan Expansion, source port of the classic game from Bullfrog.
/******************************************************************************/
/** @file bmbang.c
 *     On-map explosion effect implementation.
 * @par Purpose:
 *     Implement functions displaying an explosion within the game world.
 * @par Comment:
 *     None.
 * @author   Tomasz Lis
 * @date     19 Sep 2023 - 17 Mar 2024
 * @par  Copying and copyrights:
 *     This program is free software; you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of the License, or
 *     (at your option) any later version.
 */
/******************************************************************************/
#include "bmbang.h"

#include "enginshrapn.h"

#include "bigmap.h"
#include "thing.h"
#include "swlog.h"
#include "game_speed.h"
#include "bigmap.h"
#include "engincam.h"
/******************************************************************************/

#define MAP_CRATERS_COUNT 128

extern ubyte map_crater_next;
extern struct MapCreater map_craters[MAP_CRATERS_COUNT];

void bang_init(void)
{
#if 0
    asm volatile ("call ASM_bang_init\n"
        :  :  : "eax" );
#endif
    shrapnel_init();
    phwoar_init();
}

void new_bang(int x, int y, int z, int type, int owner, int c)
{
    // Pushed through a register holding them: a "g" operand may be placed
    // relative to the stack pointer, which each push moves.
    int stkargs[2];

    stkargs[0] = (int)(intptr_t)owner;
    stkargs[1] = (int)(intptr_t)c;

    asm volatile (
      "push 4(%4)\n"
      "push 0(%4)\n"
      "call ASM_new_bang\n"
        : : "a" (x), "d" (y), "b" (z), "c" (type), "S" (stkargs)
        : "cc", "memory");
}

void bang_new5(int x, int y, int z, int type, int owner)
{
    new_bang(x, y, z, type, owner, 0);
}

void bang_new4(int x, int y, int z, int type)
{
    new_bang(x, y, z, type, 0, 0);
}

void create_crater(short tile_x, short tile_y, short depth)
{
    int cratr_no;

    LOGSYNC("crater at (%d,%d) depth %d\n", tile_x, tile_y, depth);
    if (tile_x < 0 || tile_x >= MAP_TILE_WIDTH)
        return;
    if (tile_y < 0 || tile_y >= MAP_TILE_HEIGHT)
        return;
    cratr_no = map_crater_next;
    map_crater_next++;
    if (map_crater_next >= MAP_CRATERS_COUNT)
        map_crater_next = 0;
    map_craters[cratr_no].MapX = tile_x;
    map_craters[cratr_no].MapY = tile_y;
    map_craters[cratr_no].Depth = depth;
    map_craters[cratr_no].Iterations = 0;
}

ubyte unused_func_026(ubyte a1)
{
    ubyte ret;
    asm volatile (
      "call ASM_unused_func_026\n"
        : "=r" (ret) : "a" (a1));
    return ret;
}

void unused_func_025(short a1, short a2, short a3)
{
    asm volatile (
      "call ASM_unused_func_025\n"
        : : "a" (a1), "d" (a2), "b" (a3));
}

// Whether (x,z) is within the configured bullet-time trigger range of the
// current camera/view centre (PRCCOORD units, 256/tile) - i.e. whether the
// explosion is actually visible-ish on screen, not just near whichever agent
// happens to be "under control". Despite the handful of C-side assignments
// like "engn_xc = PRCCOORD_TO_MAPCOORD(p_thing->X)" (which would suggest
// MAP-tile-index scale), the value actually live during normal gameplay
// scrolling (set by the still-unported ASM camera code) is already in the
// same raw PRCCOORD scale as Thing.X/Z - confirmed empirically via a debug
// log: engn_xc/engn_zc values like 11016/3791 are only sensible (~tile
// 43/15) when read directly, and are wildly out of range if divided by 256
// again. engn_yc has its own non-linear scale (see the "-yc: 8*yc quirk"
// note in emit_explode_faces) and isn't meaningful here, so height is
// ignored - only horizontal (on-screen) distance matters. range_tiles == 0
// means unlimited (intensity gate only).
static TbBool bullettime_explosion_is_near_view(int x, int y, int z)
{
    int range_tiles;
    u32 dist;

    (void)y;
    range_tiles = bullettime_range_tiles();
    if (range_tiles <= 0)
        return true;

    dist = map_distance_deltas_fast(x - engn_xc, 0, z - engn_zc);
    return dist <= (u32)(range_tiles * 256);
}

// As above, but for the do_shockwave_building/vehicle/person variants, which
// give us the affected Thing directly instead of a raw epicentre - use its
// own position. NULL (no specific target) counts as "near".
static TbBool bullettime_thing_is_near_view(struct Thing *p_target)
{
    if (p_target == NULL)
        return true;
    return bullettime_explosion_is_near_view((int)p_target->X, (int)p_target->Y,
        (int)p_target->Z);
}

void do_shockwave(int x, int y, int z, int radius, int intensity, struct Thing *p_owner)
{
    // Pushed through a register holding them: a "g" operand may be placed
    // relative to the stack pointer, which each push moves.
    int stkargs[2];

    // Big shockwaves (rockets, mines, building demolitions) pass intensity far
    // above the small fixed value plain bullet-impact ground hits use; gate on
    // that (and on proximity to the camera view) so bullet-time triggers on
    // real, on-screen explosions only, not gunfire or fights elsewhere on the
    // map that the player isn't even looking at.
    if (bullettime_explosion_is_near_view(x, y, z))
        bullettime_trigger(intensity);

    stkargs[0] = (int)(intptr_t)intensity;
    stkargs[1] = (int)(intptr_t)p_owner;

    asm volatile (
      "push 4(%4)\n"
      "push 0(%4)\n"
      "call ASM_do_shockwave\n"
        : : "a" (x), "d" (y), "b" (z), "c" (radius), "S" (stkargs)
        : "cc", "memory");
}


void do_shockwave_building(int dist, int intensity, struct Thing *p_thing, struct Thing *p_owner)
{
    // Building collapse (explode_thing_building) routes its shockwave through
    // this and the person/vehicle variants below, not the general
    // do_shockwave() - hook here too so collapsing buildings can trigger
    // bullet-time.
    if (bullettime_thing_is_near_view(p_thing))
        bullettime_trigger(intensity);
    asm volatile (
      "call ASM_do_shockwave_building\n"
        : : "a" (dist), "d" (intensity), "b" (p_thing), "c" (p_owner));
}

void do_shockwave_vehicle(int dx, int dz, int dist, int intensity,
  struct Thing *p_vevicle, struct Thing *p_owner)
{
    // Pushed through a register holding them: a "g" operand may be placed
    // relative to the stack pointer, which each push moves.
    int stkargs[2];

    if (bullettime_thing_is_near_view(p_vevicle))
        bullettime_trigger(intensity);

    stkargs[0] = (int)(intptr_t)p_vevicle;
    stkargs[1] = (int)(intptr_t)p_owner;

    asm volatile (
      "push 4(%4)\n"
      "push 0(%4)\n"
      "call ASM_do_shockwave_vehicle\n"
        : : "a" (dx), "d" (dz), "b" (dist), "c" (intensity), "S" (stkargs)
        : "cc", "memory");
}

void do_shockwave_person(int dx, int dz, int dist, int intensity,
  struct Thing *p_person, struct Thing *p_owner)
{
    // Pushed through a register holding them: a "g" operand may be placed
    // relative to the stack pointer, which each push moves.
    int stkargs[2];

    // The primary trigger site for building-collapse bullet-time: explode_thing_building
    // calls this once per nearby person affected by the blast.
    if (bullettime_thing_is_near_view(p_person))
        bullettime_trigger(intensity);

    stkargs[0] = (int)(intptr_t)p_person;
    stkargs[1] = (int)(intptr_t)p_owner;

    asm volatile (
      "push 4(%4)\n"
      "push 0(%4)\n"
      "call ASM_do_shockwave_person\n"
        : : "a" (dx), "d" (dz), "b" (dist), "c" (intensity), "S" (stkargs)
        : "cc", "memory");
}

void do_shockwave_scale_effect(int dx, int dz, int dist, int intensity,
  struct SimpleThing *p_sthing, struct Thing *p_owner)
{
    // Pushed through a register holding them: a "g" operand may be placed
    // relative to the stack pointer, which each push moves.
    int stkargs[2];

    stkargs[0] = (int)(intptr_t)p_sthing;
    stkargs[1] = (int)(intptr_t)p_owner;

    asm volatile (
      "push 4(%4)\n"
      "push 0(%4)\n"
      "call ASM_do_shockwave_scale_effect\n"
        : : "a" (dx), "d" (dz), "b" (dist), "c" (intensity), "S" (stkargs)
        : "cc", "memory");
}
/******************************************************************************/
