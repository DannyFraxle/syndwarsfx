/******************************************************************************/
// Syndicate Wars Fan Expansion, source port of the classic game from Bullfrog.
/******************************************************************************/
/** @file lvwalk.c
 *     Routines for handling walk items on a map.
 * @par Purpose:
 *     Implement functions storing and handling walk items list.
 * @par Comment:
 *     None.
 * @author   Tomasz Lis
 * @date     27 May 2022 - 24 Sep 2025
 * @par  Copying and copyrights:
 *     This program is free software; you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of the License, or
 *     (at your option) any later version.
 */
/******************************************************************************/
#include "lvwalk.h"

#include <assert.h>

#include "swlog.h"
/******************************************************************************/

struct WalkHeader *game_walk_headers = NULL;
ushort next_walk_header = 1;

short *game_walk_items = NULL;
ushort next_walk_item = 1;

/******************************************************************************/

void reset_all_walk_lists(void)
{
    next_walk_header = 1;
    next_walk_item = 1;
}

ushort add_walk_face_to_list(ushort wlkhead, short wlkface)
{
    struct WalkHeader *p_walk_head;
    ushort wi;

    p_walk_head = &game_walk_headers[wlkhead];

    assert(p_walk_head->StartItem + p_walk_head->Count == next_walk_item);

    wi = next_walk_item;
    next_walk_item++;

    p_walk_head->Count++;
    game_walk_items[wi] = wlkface;

    return wi;
}

ushort create_walk_list(void)
{
    struct WalkHeader *p_walk_head;
    ushort wlkhead;

    wlkhead = next_walk_header;
    next_walk_header++;

    p_walk_head = &game_walk_headers[wlkhead];
    p_walk_head->Count = 0;
    p_walk_head->StartItem = next_walk_item;

    return wlkhead;
}

void destroy_walk_list(ushort wlkhead)
{
    assert (wlkhead + 1 == next_walk_header);
    next_walk_header--;
}

TbBool walk_face_is_in_list(ushort wlkhead, short walk_face)
{
    struct WalkHeader *p_walk_head;
    ushort wi;

    p_walk_head = &game_walk_headers[wlkhead];

    for (wi = p_walk_head->StartItem;
      wi < p_walk_head->StartItem + p_walk_head->Count; wi++)
    {
        if (game_walk_items[wi] == walk_face)
            return true;
    }
    return false;
}

/******************************************************************************/
