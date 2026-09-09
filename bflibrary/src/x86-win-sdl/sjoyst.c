/******************************************************************************/
// Bullfrog Engine Emulation Library - for use to remake classic games like
// Syndicate Wars, Magic Carpet or Dungeon Keeper.
/******************************************************************************/
/** @file sjoyst.c
 *     Joystick support.
 * @par Purpose:
 *     Implement Joystick input support for games.
 * @par Comment:
 *     None.
 * @author   Tomasz Lis
 * @date     11 Feb 2010 - 11 Oct 2020
 * @par  Copying and copyrights:
 *     This program is free software; you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of the License, or
 *     (at your option) any later version.
 */
/******************************************************************************/
#include "bfjoyst.h"

#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>

#include "privbflog.h"

/******************************************************************************/

//SDL1 joysticks are stubbed out, as we use SDL2 GameControllers instead
//SDL1 technically supports joysticks, but they are hard to map to modern controllers
/******************************************************************************/

struct DevInput joy;
/******************************************************************************/

const char *JoyGetButtonLabel(int button)
{
    static char buf[16];
    snprintf(buf, sizeof(buf), "%d", button + 1);
    return buf;
}

TbResult JoySetInterrupt(short val)
{
    return Lb_FAIL;
}

TbResult JoyGetDeviceName(char *textbuf)
{
    const char *name = "No Joystick";
    strncpy(textbuf, name, 52);
    textbuf[51] = '\0';
    return Lb_SUCCESS;
}

TbResult JoyUpdateInputs(struct DevInput *dinp)
{
    return Lb_OK;
}

TbResult JoyRefreshDevices(struct DevInput *dinp)
{
    return Lb_SUCCESS;
}

void devinput_clear(struct DevInput *dinp)
{
    memset(dinp, 0, sizeof(struct DevInput));
    dinp->MinXAxis[0] = 0x10000;
    dinp->MaxXAxis[0] = 0;
    dinp->MinYAxis[0] = 0x10000;
    dinp->MaxYAxis[0] = 0;
    dinp->MinZAxis[0] = 0x10000;
    dinp->MaxZAxis[0] = 0;
    dinp->MinRAxis[0] = 0x10000;
    dinp->MaxRAxis[0] = 0;
    dinp->MinUAxis[0] = 0x10000;
    dinp->MaxUAxis[0] = 0;
    dinp->MinVAxis[0] = 0x10000;
    dinp->MaxVAxis[0] = 0;
    dinp->HatMax[0] = 0;
    dinp->NumberOfDevices = 1;
}

TbResult JoySetupDevice(struct DevInput *dinp, int jtype)
{
    devinput_clear(dinp);
    return Lb_SUCCESS;
}

TbResult JoyDriverInit(void)
{
    return Lb_SUCCESS;
}

TbResult JoyDriverShutdown(void)
{
    return Lb_SUCCESS;
}
/******************************************************************************/
