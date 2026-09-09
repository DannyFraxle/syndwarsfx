/******************************************************************************/
// Syndicate Wars Fan Expansion, source port of the classic game from Bullfrog.
/******************************************************************************/
/** @file purpldrwlst.c
 *     Drawlists handling for the purple projector screens.
 * @par Purpose:
 *     Implements functions for filling and using drawlists.
 * @par Comment:
 *     None.
 * @author   Tomasz Lis
 * @date     22 Apr 2024 - 28 Sep 2024
 * @par  Copying and copyrights:
 *     This program is free software; you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of the License, or
 *     (at your option) any later version.
 */
/******************************************************************************/
#include "purpldrwlst.h"

#include <stdlib.h>
#include "bfjoyst.h"
#include "bflib_vidraw.h"
#include "bfbox.h"
#include "bfline.h"
#include "bfmouse.h"
#include "bftext.h"
#include "bftringl.h"
#include "bfscreen.h"
#include "bfscrcopy.h"
#include "bfsprite.h"
#include "poly.h"

#include "display.h"
#include "guiboxes.h"
#include "mydraw.h"
#include "swlog.h"
/******************************************************************************/

struct PurpleDrawItem *purple_draw_list = NULL;
ushort purple_draw_index = 0;

ushort hotspot_next = 1;

ubyte purple_joy_move = 0;

/******************************************************************************/

ushort find_closest_hotspot_down(void)
{
    ulong hmin;
    short imin, i;

    hmin = 0x80000000;
    imin = 0;
    for (i = 1; i < hotspot_next; i++)
    {
        short ms_x, ms_y;
        short shift_w, shift_h;

        ms_x = lbDisplay.MMouseX;
        ms_y = lbDisplay.MMouseY;
        shift_w = hotspot_buffer[i].X - ms_x;
        shift_h = hotspot_buffer[i].Y - ms_y;
        if ((shift_h > 0) && (shift_h > abs(shift_w)))
        {
            ulong hcur;
            if (shift_h <= abs(shift_w))
                hcur = (shift_h >> 1) + abs(shift_w);
            else
                hcur = shift_h + (abs(shift_w) >> 1);
            if ((hcur < hmin) && (hcur != 0)) {
                hmin = hcur;
                imin = i;
            }
        }
    }
    if (hmin == 0x80000000)
        return 0;
    return imin;
}

ushort find_closest_hotspot_up(void)
{
    ulong hmin;
    short imin, i;

    hmin = 0x80000000;
    imin = 0;
    for (i = 1; i < hotspot_next; i++)
    {
        short ms_x, ms_y;
        short shift_w, shift_h;

        ms_x = lbDisplay.MMouseX;
        ms_y = lbDisplay.MMouseY;
        shift_w = hotspot_buffer[i].X - ms_x;
        shift_h = ms_y - hotspot_buffer[i].Y;
        if ((shift_h > 0) && (shift_h > abs(shift_w)))
        {
            ulong hcur;
            if (shift_h <= abs(shift_w))
                hcur = (shift_h >> 1) + abs(shift_w);
            else
                hcur = shift_h + (abs(shift_w) >> 1);
            if ((hcur < hmin) && (hcur != 0)) {
                hmin = hcur;
                imin = i;
            }
        }
    }
    if (hmin == 0x80000000)
        return 0;
    return imin;
}

ushort find_closest_hotspot_right(void)
{
    ulong hmin;
    short imin, i;

    hmin = 0x80000000;
    imin = 0;
    for (i = 1; i < hotspot_next; i++)
    {
        short ms_x, ms_y;
        short shift_w, shift_h;

        ms_x = lbDisplay.MMouseX;
        ms_y = lbDisplay.MMouseY;
        shift_w = hotspot_buffer[i].X - ms_x;
        shift_h = hotspot_buffer[i].Y - ms_y;
        if ((shift_w > 0) && (shift_w > abs(shift_h)))
        {
            ulong hcur;
            if (abs(shift_h) <= shift_w)
                hcur = shift_w + (abs(shift_h) >> 1);
            else
                hcur = (shift_w >> 1) + abs(shift_h);
            if ((hcur < hmin) && (hcur != 0)) {
                hmin = hcur;
                imin = i;
            }
        }
    }
    if (hmin == 0x80000000)
        return 0;
    return imin;
}

ushort find_closest_hotspot_left(void)
{
    ulong hmin;
    short imin, i;

    hmin = 0x80000000;
    imin = 0;
    for (i = 1; i < hotspot_next; i++)
    {
        short ms_x, ms_y;
        short shift_w, shift_h;

        ms_x = lbDisplay.MMouseX;
        ms_y = lbDisplay.MMouseY;
        shift_w = ms_x - hotspot_buffer[i].X;
        shift_h = hotspot_buffer[i].Y - ms_y;
        if ((shift_w > 0) && (shift_w > abs(shift_h)))
        {
            ulong hcur;
            if (abs(shift_h) <= shift_w)
                hcur = shift_w + (abs(shift_h) >> 1);
            else
                hcur = (shift_w >> 1) + abs(shift_h);
            if ((hcur < hmin) && (hcur != 0)) {
                hmin = hcur;
                imin = i;
            }
        }
    }
    if (hmin == 0x80000000)
        return 0;
    return imin;
}

void input_screen_hotspots(void)
{
    ushort hs;

    hs = 0;
    if (purple_joy_move)
    {
        if ((joy.DigitalY[0] == 0) && (joy.DigitalX[0] == 0))
            purple_joy_move = 0;
    }
    else if (joy.DigitalY[0] == 1)
    {
        hs = find_closest_hotspot_down();
        purple_joy_move = 1;
    }
    else if (joy.DigitalY[0] == -1)
    {
        hs = find_closest_hotspot_up();
        purple_joy_move = 1;
    }
    else if (joy.DigitalX[0] == 1)
    {
        hs = find_closest_hotspot_right();
        purple_joy_move = 1;
    }
    else if (joy.DigitalX[0] == -1)
    {
        hs = find_closest_hotspot_left();
        purple_joy_move = 1;
    }

    if (hs > 0) {
        LbMouseSetPosition(hotspot_buffer[hs].X, hotspot_buffer[hs].Y);
    }
}

void screen_hotspots_clear(void)
{
    hotspot_next = 1;
    hotspot_buffer[0].X = lbDisplay.GraphicsScreenWidth / 2;
    hotspot_buffer[0].Y = lbDisplay.GraphicsScreenHeight / 2;
}

void screen_hotspot_add(int x, int y)
{
    ushort hs;
    hs = hotspot_next;
    if (hs + 1 > hotspot_buffer_len)
        return;
    hotspot_next++;
    hotspot_buffer[hs].X = x;
    hotspot_buffer[hs].Y = y;
}

static void draw_purple_drawitems(void)
{
    struct PolyPoint point_a;
    struct PolyPoint point_c;
    struct PolyPoint point_b;
    ushort pditm;

    point_a.X = proj_origin.X;
    point_a.Y = proj_origin.Y;
    point_a.S = 0x200000;
    point_c.S = 0x200000;
    point_b.S = 0x8000;

    for (pditm = 0; pditm < purple_draw_index; pditm++)
    {
        struct PurpleDrawItem *p_pditem;
        short x, y;
        short w, h;
        short shift_w, shift_h;

        p_pditem = &purple_draw_list[pditm];

        lbDisplay.DrawFlags = p_pditem->Flags;

        switch (p_pditem->Type)
        {
        case PuDT_BOX:
            x = p_pditem->U.Box.X;
            y = p_pditem->U.Box.Y;
            w = p_pditem->U.Box.Width;
            h = p_pditem->U.Box.Height;
            LbDrawBox(x, y, w, h, p_pditem->U.Box.Colour);
            if ((lbDisplay.DrawFlags & 0x8000) != 0)
            {
                shift_w = (w >> 1);
                shift_h = (h >> 1);
                screen_hotspot_add(x + shift_w, y + shift_h);
            }
            break;
        case PuDT_TEXT:
            lbDisplay.DrawColour = p_pditem->U.Text.Colour;
            lbFontPtr = p_pditem->U.Text.Font;
            my_set_text_window(p_pditem->U.Text.WindowX, p_pditem->U.Text.WindowY,
              p_pditem->U.Text.Width, p_pditem->U.Text.Height);
            my_draw_text(p_pditem->U.Text.X, p_pditem->U.Text.Y,
              p_pditem->U.Text.Text, p_pditem->U.Text.Line);
            if ((lbDisplay.DrawFlags & 0x8000) != 0)
            {
                w = my_string_width(p_pditem->U.Text.Text);
                if ((w >= p_pditem->U.Text.Width)
                  || ((lbDisplay.DrawFlags & Lb_TEXT_HALIGN_CENTER)) != 0)
                {
                    x = p_pditem->U.Text.WindowX;
                    shift_w = p_pditem->U.Text.Width >> 1;
                }
                else
                {
                    x = p_pditem->U.Text.X + p_pditem->U.Text.WindowX;
                    shift_w = w >> 1;
                }
                shift_h = my_char_height('A') >> 1;
                y = p_pditem->U.Text.Y + p_pditem->U.Text.WindowY;
                screen_hotspot_add(x + shift_w, y + shift_h);
            }
            break;
        case PuDT_UNK03:
            break;
        case PuDT_COPYBOX:
            x = p_pditem->U.Box.X;
            y = p_pditem->U.Box.Y;
            shift_w = p_pditem->U.Box.Width;
            shift_h = p_pditem->U.Box.Height;
            LbScreenCopyBox(lbDisplay.WScreen, back_buffer,
                x, y, x, y, shift_w, shift_h);
            break;
        case PuDT_SPRITE:
            x = p_pditem->U.Sprite.X;
            y = p_pditem->U.Sprite.Y;
            lbDisplay.DrawColour = p_pditem->U.Box.Colour;
            if ((lbDisplay.DrawFlags & Lb_TEXT_ONE_COLOR) != 0)
                LbSpriteDrawOneColour(x, y, p_pditem->U.Sprite.Sprite, lbDisplay.DrawColour);
            else
                LbSpriteDraw(x, y, p_pditem->U.Sprite.Sprite);
            if ((lbDisplay.DrawFlags & 0x8000) != 0)
            {
                w = p_pditem->U.Sprite.Sprite->SWidth;
                h = p_pditem->U.Sprite.Sprite->SHeight;
                shift_w = (w >> 1);
                shift_h = (h >> 1);
                screen_hotspot_add(x + shift_w, y + shift_h);
            }
            break;
        case PuDT_POTRIG:
            vec_colour = p_pditem->U.Line.Colour;
            point_c.X = p_pditem->U.Line.X1;
            point_c.Y = p_pditem->U.Line.Y1;
            point_b.X = p_pditem->U.Line.X2;
            point_b.Y = p_pditem->U.Line.Y2;
            if ((point_c.Y - point_b.Y) * (point_b.X - point_a.X)
                - (point_b.Y - point_a.Y) * (point_c.X - point_b.X) > 0)
                trig(&point_a, &point_b, &point_c);
            else
                trig(&point_a, &point_c, &point_b);
            break;
        case PuDT_FLIC:
            p_pditem->U.Flic.Function();
            break;
        case PuDT_NOISEBOX:
            draw_noise_box(p_pditem->U.Box.X, p_pditem->U.Box.Y,
              p_pditem->U.Box.Width, p_pditem->U.Box.Height);
            break;
        case PuDT_LINE:
            LbDrawLine(p_pditem->U.Line.X1, p_pditem->U.Line.Y1,
                p_pditem->U.Line.X2, p_pditem->U.Line.Y2, p_pditem->U.Line.Colour);
            break;
        case PuDT_HVLINE:
            LbDrawHVLine(p_pditem->U.Line.X1, p_pditem->U.Line.Y1,
                p_pditem->U.Line.X2, p_pditem->U.Line.Y2, p_pditem->U.Line.Colour);
            break;
        case PuDT_TRIANGLE:
            LbDrawTriangle(p_pditem->U.Triangle.X1, p_pditem->U.Triangle.Y1,
                p_pditem->U.Triangle.X2, p_pditem->U.Triangle.Y2,
                p_pditem->U.Triangle.X3, p_pditem->U.Triangle.Y3, p_pditem->U.Triangle.Colour);
            break;
        case PuDT_HOTSPOT:
            screen_hotspot_add(p_pditem->U.Hotspot.X, p_pditem->U.Hotspot.Y);
            break;
        }
    }
}

void draw_purple_screen(void)
{
    LbScreenSetGraphicsWindow(0, 0, lbDisplay.GraphicsScreenWidth,
        lbDisplay.GraphicsScreenHeight);
    my_set_text_window(0, 0, lbDisplay.GraphicsScreenWidth,
        lbDisplay.GraphicsScreenHeight);
    screen_hotspots_clear();
    vec_mode = 17;
    draw_purple_drawitems();
    purple_draw_index = 0;
    lbDisplay.DrawFlags = 0;
    // TODO Input should be separated from drawing
    input_screen_hotspots();
}

/******************************************************************************/
