/******************************************************************************/
// Syndicate Wars FX3D - OpenGL hardware renderer for Bullfrog titles.
/******************************************************************************/
/** @file hwr_source_sw.h
 *     Accessor for the Syndicate Wars scene source.
 * @par Purpose:
 *     The one game-specific entry point of libhwrender: hands the host the
 *     HwrSceneSource that reads Syndicate Wars world data. Other Bullfrog
 *     titles provide their own equivalent.
 * @par  Copying and copyrights:
 *     This program is free software; you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of the License, or
 *     (at your option) any later version.
 */
/******************************************************************************/
#ifndef HWR_SOURCE_SW_H
#define HWR_SOURCE_SW_H

#include "hwr_scene_source.h"

#ifdef __cplusplus
extern "C" {
#endif
/******************************************************************************/

/** Configure (for the given viewport) and return the Syndicate Wars scene
 *  source. Pass the result to hwr_set_source(). */
const HwrSceneSource *hwr_sw_source(int view_w, int view_h);

/******************************************************************************/
#ifdef __cplusplus
}
#endif
#endif
