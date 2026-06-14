/******************************************************************************/
// Bullfrog Engine Emulation Library - for use to remake classic games like
// Syndicate Wars, Magic Carpet, Genewars or Dungeon Keeper.
/******************************************************************************/
/** @file smouse.cpp
 *     Implementation of related functions.
 * @par Purpose:
 *     Unknown.
 * @par Comment:
 *     None.
 * @author   Tomasz Lis
 * @date     12 Nov 2008 - 05 Nov 2021
 * @par  Copying and copyrights:
 *     This program is free software; you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of the License, or
 *     (at your option) any later version.
 */
/******************************************************************************/
#include <stdbool.h>
#include <SDL.h>
#include "bfmouse.h"

#if defined(HAVE_CONFIG_H)
#  include "bfconfig.h"
#endif

#include "mshandler.hpp"
#include "bfscreen.h"
#include "bfsprite.h"
#include "bfplanar.h"
#include "privbflog.h"

extern SDL_Window *lbWindow;
extern SDL_Color lbPaletteColors[256];

extern "C" {

#define AUTORESET_MIN_SHIFT 50

};

// ---------------------------------------------------------------------------
// SDL hardware cursor — decodes the 8bpp RLE sprite into a scaled RGBA
// SDL_Cursor so that the OS compositor moves it independently of the
// software blit rate.  Position tracking stays in the existing bflib path.
// ---------------------------------------------------------------------------

static SDL_Cursor      *lbHwCursor       = NULL;
static const TbSprite  *lbHwCursorSprite = NULL;
static long             lbHwCursorHotX   = 0;
static long             lbHwCursorHotY   = 0;

static void LbI_UpdateHardwareCursor(void)
{
    if (lbHwCursor != NULL) {
        SDL_FreeCursor(lbHwCursor);
        lbHwCursor = NULL;
    }

    const TbSprite *spr = lbHwCursorSprite;
    if (spr == NULL || spr->SWidth == 0 || spr->SHeight == 0 || spr->Data == NULL) {
        SDL_ShowCursor(SDL_DISABLE);
        return;
    }

    int sw = spr->SWidth;
    int sh = spr->SHeight;
    int surf_w = sw;
    int surf_h = sh;

    SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, surf_w, surf_h, 32, SDL_PIXELFORMAT_RGBA8888);
    if (surf == NULL) return;

    // All pixels transparent to start.
    SDL_memset(surf->pixels, 0, (size_t)surf->h * surf->pitch);

    // Decode 8bpp RLE sprite (0=end-of-row, N>0=N opaque pixels, N<0=skip -N).
    unsigned char *sprdata = (unsigned char *)spr->Data;
    for (int row = 0; row < sh; row++) {
        int col = 0;
        while (1) {
            int pxlen = (signed char)*sprdata++;
            if (pxlen == 0) break;
            if (pxlen < 0) {
                col += -pxlen;
            } else {
                for (int i = 0; i < pxlen; i++, col++) {
                    unsigned char idx = *sprdata++;
                    SDL_Color c = lbPaletteColors[idx];
                    Uint32 pxval = SDL_MapRGBA(surf->format, c.r, c.g, c.b, 255);
                    if (col < surf_w) {
                        Uint32 *row_ptr = (Uint32 *)((Uint8 *)surf->pixels + row * surf->pitch);
                        row_ptr[col] = pxval;
                    }
                }
            }
        }
    }

    // pointer_hotspot values are negative offsets (e.g. {-7,-7} = tip at pixel 7,7).
    // SDL hotspot is the pixel within the sprite that is the pointer tip — negate to convert.
    int hot_x = (int)(-lbHwCursorHotX);
    int hot_y = (int)(-lbHwCursorHotY);
    if (hot_x < 0) hot_x = 0;
    if (hot_y < 0) hot_y = 0;
    lbHwCursor = SDL_CreateColorCursor(surf, hot_x, hot_y);
    SDL_FreeSurface(surf);

    if (lbHwCursor != NULL) {
        SDL_SetCursor(lbHwCursor);
        SDL_ShowCursor(SDL_ENABLE);
    }
}

// ---------------------------------------------------------------------------

TbResult LbMousePlace(void)
{
    if (!lbMouseInstalled)
        return Lb_FAIL;

    if (!pointerHandler.PointerBeginPartialUpdate())
        return Lb_FAIL;

    return Lb_SUCCESS;
}

TbResult LbMouseRemove(void)
{
    if (!lbMouseInstalled)
        return Lb_FAIL;

    if (!pointerHandler.PointerEndPartialUpdate())
        return Lb_FAIL;

    return Lb_SUCCESS;
}

// Hardware cursor is active: skip software Backup/Draw/Undraw on WScreen.
TbResult LbMouseOnBeginSwap(void)
{
    return Lb_SUCCESS;
}

TbResult LbMouseOnEndSwap(void)
{
    return Lb_SUCCESS;
}

TbResult LbMouseChangeSpriteOffset(long hot_x, long hot_y)
{
    if (!lbMouseInstalled)
        return Lb_FAIL;
    LOGDBG("setting hs (%ld,%ld)", hot_x, hot_y);

    if (!pointerHandler.SetPointerOffset(-hot_x, -hot_y))
        return Lb_FAIL;

    lbHwCursorHotX = hot_x;
    lbHwCursorHotY = hot_y;
    LbI_UpdateHardwareCursor();

    return Lb_SUCCESS;
}

TbResult LbMouseGetSpriteOffset(long *hot_x, long *hot_y)
{
    struct TbPoint *hotspot;

    hotspot = pointerHandler.GetPointerOffset();
    if (hotspot == NULL)
        return Lb_FAIL;
    *hot_x = -hotspot->x;
    *hot_y = -hotspot->y;

    return Lb_SUCCESS;
}

TbResult LbMouseChangeSprite(const struct TbSprite *pointer_spr)
{
    if (!lbMouseInstalled)
        return Lb_FAIL;

    if (pointer_spr == NULL)
        LOGDBG("setting to %s", "NONE");
    else
        LOGDBG("setting to %dx%d, data at %p", (int)pointer_spr->SWidth,
          (int)pointer_spr->SHeight, pointer_spr);

    if (!pointerHandler.SetMousePointer(pointer_spr))
        return Lb_FAIL;

    lbHwCursorSprite = pointer_spr;
    LbI_UpdateHardwareCursor();

    return Lb_SUCCESS;
}

TbResult LbMouseChangeMoveRatio(long ratio_x, long ratio_y)
{
    if ((ratio_x < -32 * NORMAL_MOUSE_MOVE_RATIO) ||
      (ratio_x > 32 * NORMAL_MOUSE_MOVE_RATIO) || (ratio_x == 0))
        return Lb_FAIL;
    if ((ratio_y < -32 * NORMAL_MOUSE_MOVE_RATIO) ||
      (ratio_y > 32 * NORMAL_MOUSE_MOVE_RATIO) || (ratio_y == 0))
        return Lb_FAIL;

    LOGSYNC("new ratio %ldx%ld", ratio_x, ratio_y);

#if defined(LB_ENABLE_MOUSE_MOVE_RATIO)
    lbDisplay.MouseMoveRatioX = ratio_x;
    lbDisplay.MouseMoveRatioY = ratio_y;
#endif

    return Lb_SUCCESS;
}

TbBool LbMouseIsInstalled(void)
{
    if (!lbMouseInstalled)
        return false;

    if (!pointerHandler.IsInstalled())
        return false;

    return true;
}

TbResult LbMouseSetup(const struct TbSprite *pointer_spr, int ratio_x, int ratio_y)
{
    long x,y;

    if (lbMouseInstalled)
        LbMouseSuspend();
    // Make sure the pointer sprite gets updated, even if address stays unchanged
    lbDisplay.MouseSprite = NULL;

    pointerHandler.Install();

#if 0
    minfo.XSpriteOffset = 0;
    minfo.YSpriteOffset = 0;
    minfo.XMoveRatio = 1;
    minfo.YMoveRatio = 1;
    memset(minfo.Sprite, 254, 0x1000u);
    redraw_active_lock = 0;
    memset(&mbuffer, 0, 0x1020u);
#endif

    lbMouseOffline = true;
    lbMouseInstalled = true;

    if ( LbMouseSetWindow(0, 0, lbDisplay.GraphicsScreenWidth, lbDisplay.GraphicsScreenHeight) != Lb_SUCCESS )
    {
        LOGERR("could not set mouse window, size (%d,%d)",
          (int)lbDisplay.GraphicsScreenWidth, (int)lbDisplay.GraphicsScreenHeight);
        lbMouseInstalled = false;
        return Lb_FAIL;
    }
    y = lbDisplay.MouseWindowY + lbDisplay.MouseWindowHeight / 2;
    x = lbDisplay.MouseWindowX + lbDisplay.MouseWindowWidth / 2;
    if ( LbMouseChangeMoveRatio(ratio_x, ratio_y) != Lb_SUCCESS )
    {
        LOGERR("could not change move ratio to (%d,%d)", ratio_x, ratio_y);
        lbMouseInstalled = false;
        return Lb_FAIL;
    }
    if ( LbMouseSetPosition(x, y) != Lb_SUCCESS )
    {
        LOGERR("could not set position to (%d,%d)", x, y);
        lbMouseInstalled = false;
        return Lb_FAIL;
    }
    if ( LbMouseChangeSprite(pointer_spr) != Lb_SUCCESS )
    {
        LOGERR("could not change sprite");
        lbMouseInstalled = false;
        return Lb_FAIL;
    }
    lbMouseOffline = false;
    return Lb_SUCCESS;
}

TbResult LbMouseReset(void)
{
    return LbMouseSuspend();
}

TbResult LbMouseSuspend(void)
{
    if (!lbMouseInstalled)
        return Lb_FAIL;

    if (!pointerHandler.Release())
        return Lb_FAIL;

    if (lbHwCursor != NULL) {
        SDL_FreeCursor(lbHwCursor);
        lbHwCursor = NULL;
    }
    lbHwCursorSprite = NULL;
    SDL_ShowCursor(SDL_DISABLE);

    return Lb_SUCCESS;
}

TbResult LbMouseSetWindow(long x, long y, long width, long height)
{
    if (!lbMouseInstalled)
        return Lb_FAIL;

    if (!pointerHandler.SetMouseWindow(x, y, width, height))
        return Lb_FAIL;

    return Lb_SUCCESS;
}

TbResult LbMouseSetPosition(long x, long y)
{
    if (!lbMouseInstalled)
        return Lb_FAIL;

    if (!pointerHandler.SetMousePosition(x, y))
        return Lb_FAIL;

    return Lb_SUCCESS;
}

extern "C" {
TbResult LbMouseOnMove(struct TbPoint pos);
}

TbResult LbMouseOnMove(struct TbPoint pos)
{
    if ((!lbMouseInstalled) || (lbMouseOffline))
        return Lb_FAIL;

    if (!pointerHandler.SetMousePosition(pos.x, pos.y))
        return Lb_FAIL;

    return Lb_SUCCESS;
}

void MouseToScreen(struct TbPoint *pos)
{
    // Static variables for storing last mouse coordinates; needed
    // because lbDisplay.MMouseX/MMouseY coords are scaled
    static long mx = 0;
    static long my = 0;
    struct TbRect clip;
    struct TbPoint orig;

    if (!pointerHandler.GetMouseWindow(&clip))
        return;

    if (lbMouseAutoReset)
    {
      orig.x = pos->x;
      orig.y = pos->y;
#if defined(LB_ENABLE_MOUSE_MOVE_RATIO)
      pos->x = lbDisplay.MMouseX + ((pos->x - mx) *
        (long)lbDisplay.MouseMoveRatioX) / NORMAL_MOUSE_MOVE_RATIO;
      pos->y = lbDisplay.MMouseY + ((pos->y - my) *
        (long)lbDisplay.MouseMoveRatioY) / NORMAL_MOUSE_MOVE_RATIO;
#else
      pos->x = lbDisplay.MMouseX + (pos->x - mx);
      pos->y = lbDisplay.MMouseY + (pos->y - my);
#endif
      mx = orig.x;
      my = orig.y;
      if ((mx < clip.left + AUTORESET_MIN_SHIFT)
       || (mx > clip.right - AUTORESET_MIN_SHIFT)
       || (my < clip.top + AUTORESET_MIN_SHIFT)
       || (my > clip.bottom - AUTORESET_MIN_SHIFT))
      {
          mx = (clip.right - clip.left) / 2 + clip.left;
          my = (clip.bottom - clip.top) / 2 + clip.top;
          SDL_WarpMouseInWindow(lbWindow, mx, my);
      }
    } else
    {
      orig.x = pos->x;
      orig.y = pos->y;
#if defined(LB_ENABLE_MOUSE_MOVE_RATIO)
      pos->x = mx + ((pos->x - mx) *
        (long)lbDisplay.MouseMoveRatioX) / NORMAL_MOUSE_MOVE_RATIO;
      pos->y = my + ((pos->y - my) *
        (long)lbDisplay.MouseMoveRatioY) / NORMAL_MOUSE_MOVE_RATIO;
#endif
      mx = orig.x;
      my = orig.y;
    }

    // Allow the custom set mouse clip window to extend move range beyond graphics screen
    if (clip.right < lbDisplay.GraphicsScreenWidth)
        clip.right = lbDisplay.GraphicsScreenWidth;
    if (clip.bottom < lbDisplay.GraphicsScreenHeight)
        clip.bottom = lbDisplay.GraphicsScreenHeight;
    if (lbScreenSurfaceDimensions.Width != clip.right)
        pos->x = (pos->x * clip.right) / lbScreenSurfaceDimensions.Width;
    if (lbScreenSurfaceDimensions.Height != clip.bottom)
        pos->y = (pos->y * clip.bottom) / lbScreenSurfaceDimensions.Height;

    LOGNO("before (%ld,%ld) after (%ld,%ld)", orig.x, orig.y, pos->x, pos->y);
}

/**
 * Converts an SDL mouse button event and button state to platform-independent action.
 * @param eventType SDL event type.
 * @param button SDL button definition.
 * @return
 */
static TbMouseAction MouseButtonActionsMapping(int eventType, const SDL_MouseButtonEvent * button)
{
    if (eventType == SDL_MOUSEBUTTONDOWN) {
        switch (button->button)  {
        case SDL_BUTTON_LEFT: return MActn_LBUTTONDOWN;
        case SDL_BUTTON_MIDDLE: return MActn_MBUTTONDOWN;
        case SDL_BUTTON_RIGHT: return MActn_RBUTTONDOWN;
        }
    }
    else if (eventType == SDL_MOUSEBUTTONUP) {
        switch (button->button) {
        case SDL_BUTTON_LEFT: return MActn_LBUTTONUP;
        case SDL_BUTTON_MIDDLE: return MActn_MBUTTONUP;
        case SDL_BUTTON_RIGHT: return MActn_RBUTTONUP;
        }
    }
    LOGWARN("unidentified event, type %d button %d", eventType, (int)button->button);
    return MActn_NONE;
}

extern "C" {
TbResult MEvent(const SDL_Event *ev);
};

/** @internal
 * Triggers mouse control function for given SDL mouse event.
 * @return SUCCESS if the event was processed, FAIL if key isn't supported, OK if no mouse event.
 */
TbResult MEvent(const SDL_Event *ev)
{
    TbMouseAction action;
    struct TbPoint pos;
    TbResult ret;

    switch (ev->type)
    {
    case SDL_MOUSEMOTION:
        action = MActn_MOUSEMOVE;
        pos.x = ev->motion.x;
        pos.y = ev->motion.y;
        ret = mouseControl(action, &pos);
        return ret;

    case SDL_MOUSEBUTTONUP:
    case SDL_MOUSEBUTTONDOWN:
        action = MouseButtonActionsMapping(ev->type, &ev->button);
        pos.x = ev->button.x;
        pos.y = ev->button.y;
        ret = mouseControl(action, &pos);
        return ret;
    case SDL_MOUSEWHEEL:
        pos.x = 0;
        pos.y = 0;
        ret = mouseControl(ev->wheel.y > 0 ? MActn_WHEELMOVEUP : MActn_WHEELMOVEDOWN, &pos);
        break;
    }
    return Lb_OK;
}

/******************************************************************************/
