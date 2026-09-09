/******************************************************************************/
// Syndicate Wars Fan Expansion, source port of the classic game from Bullfrog.
/******************************************************************************/
/** @file enginshrapn.c
 *     Shrapnel structures required by the 3D engine.
 * @par Purpose:
 *     Implement functions for handling shrapnel in 3D world.
 * @par Comment:
 *     None.
 * @author   Tomasz Lis
 * @date     19 Apr 2022 - 27 Aug 2023
 * @par  Copying and copyrights:
 *     This program is free software; you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of the License, or
 *     (at your option) any later version.
 */
/******************************************************************************/
#include "enginshrapn.h"

#include "bfmemut.h"
#include "privrdlog.h"
/******************************************************************************/
struct Shrapnel shrapnel[SHRAPNEL_COUNT];
ushort shrapnel_free = 1;

struct Phwoar phwoar[PHWOAR_COUNT];
ushort phwoar_free = 1;

u32 phwoar_num_used = 0;
u32 phwoar_num_free = 1;

struct FireFlame FIRE_flame[FLAME_COUNT];

/******************************************************************************/
void shrapnel_init(void)
{
    int i;

    LbMemorySet(shrapnel, 0, sizeof(shrapnel));

    for (i = 1; i < SHRAPNEL_COUNT-1; i++) {
        shrapnel[i].child = i + 1;
    }
    shrapnel[i].child = 0;
    shrapnel_free = 1;
}

void phwoar_init(void)
{
    int i;

    LbMemorySet(phwoar, 0, sizeof(phwoar));
    for (i = 1; i < PHWOAR_COUNT-1; i++) {
        phwoar[i].child = i + 1;
    }
    phwoar[i].child = 0;
    phwoar_free = 1;

    phwoar_num_free = PHWOAR_COUNT-1;
    phwoar_num_used = 0;
}

/******************************************************************************/
