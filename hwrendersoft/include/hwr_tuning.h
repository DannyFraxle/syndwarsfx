/******************************************************************************/
// Syndicate Wars FX3D - OpenGL hardware renderer for Bullfrog titles.
/******************************************************************************/
/**                        2026 danny@fraxle.net                             **/
/******************************************************************************/
/** @file hwr_tuning.h
 *     In-game lighting tuning panel (real-time slider UI).
 */
/******************************************************************************/
#ifndef HWR_TUNING_H
#define HWR_TUNING_H

#ifdef __cplusplus
extern "C" {
#endif

/** Toggle the tuning panel on/off (F9). */
void hwr_tuning_toggle(void);

/** Render the tuning panel overlay. Call after scene passes, before present. */
void hwr_tuning_render(void);

/** 1 if the panel is currently visible. */
int hwr_tuning_active(void);

#ifdef __cplusplus
}
#endif
#endif
