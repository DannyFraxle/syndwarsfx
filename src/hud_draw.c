/******************************************************************************/
// Syndicate Wars Fan Expansion, source port of the classic game from Bullfrog.
/******************************************************************************/
/** @file hud_draw.c
 *     Ingame Heads-Up Display drawing routines.
 * @par Purpose:
 *     Implement functions drawing the HUD as a whole.
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
#include "hud_draw.h"

#include "bfscreen.h"

#include "engindrwlstx_tng.h"
#include "game_options.h"
#include "game.h"
#include "hud_panel.h"
#include "hud_target.h"
#include "packet.h"
#include "player.h"
#include "swlog.h"
#include "thing.h"
/******************************************************************************/

void draw_engine_net_text(void)
{
    asm volatile ("call ASM_draw_engine_net_text\n"
        :  :  : "eax" );
}

void draw_hud(int dcthing)
{
#if 0
    asm volatile ("call ASM_draw_hud\n"
        : : "a" (dcthing));
    return;
#endif
    PlayerInfo *p_locplayer;

    p_locplayer = &players[local_player_no];
    if (!panel_any_visible()) {
        return;
    }

    show_goto_point(0);
    init_draw_target();

    {
        struct Thing *p_mothing;

        p_mothing = &things[p_locplayer->DirectControl[mouser]];
        if (!lbDisplay.MRightButton
          || (p_mothing->PTarget == NULL)
          || current_weapon_has_targetting(p_mothing)
          || (p_mothing->U.UPerson.WeaponTimer < 14)) {
            p_locplayer->field_102 = 0;
        }
    }

    draw_hud_lock_target();

    if (ingame.DisplayMode == 50)
    {
        short plagent;
        short target;

        for (plagent = 0; plagent < playable_agents; plagent++)
        {
            struct Thing *p_agent;

            p_agent = p_locplayer->MyAgent[plagent];
            number_player(p_agent, plagent);
            if ((p_agent->Flag & TngF_SelectedAgent) != 0)
            {
                ushort ctlmode;
                ctlmode = user_input_control_mode_get(local_player_no, plagent);
                if (ctlmode != UInpCtr_Mouse)
                {
                    if (p_agent->PTarget != NULL)
                        draw_target_person(p_agent->PTarget, 2);
                }
            }
        }

        if (!PacketRecord_IsPlayback())
        {
          draw_hud_target_mouse(dcthing);
        }

        target = p_locplayer->field_102;
        if (target > 0)
        {
            if (!thing_is_destroyed(target))
                draw_hud_target2(dcthing, target);
        }
        draw_new_panel();
    }
}

/******************************************************************************/
