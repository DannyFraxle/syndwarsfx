#ifndef HWR_SPRITE_H
#define HWR_SPRITE_H

#include <stdint.h>

#define HWR_ATLAS_W         8192
#define HWR_ATLAS_H         8192
#define HWR_ATLAS_MAX_SLOTS 8192

int  hwr_rle_decode(const uint8_t *rle, uint8_t *out, int w, int h);
int  hwr_rle_decode_opaque(const uint8_t *rle, uint8_t *out, uint8_t *opq, int w, int h);

int  hwr_atlas_find(uint32_t key);  /* lookup only, no insert */
int  hwr_atlas_register(uint32_t key, const uint8_t *pixels, int w, int h);
void hwr_atlas_upload_pending(void);
void hwr_atlas_uv(int slot, float *u0, float *v0, float *u1, float *v1);
void hwr_atlas_size(int slot, int *w, int *h);
void hwr_atlas_bind(int unit);
void hwr_atlas_reset(void);

/* Tell the atlas which palette its tiles were baked against and which palette
 * the game is displaying now (both 256*3 RGB, 8-bit channels). When they
 * differ — infrared/thermal view swaps in pal3, brightness reloads the palette
 * — the sprite/overlay shaders remap the cached tile colours to the live
 * palette instead of the atlas going stale. Cheap; call once per turn. */
void hwr_atlas_set_palettes(const uint8_t *bake_pal, const uint8_t *live_pal);

/* Lazily registers a procedural radial-glow tile and returns its atlas slot, or
 * <0 on failure. variant: 0 = white, 1 = red, 2 = blue. Used for additive light
 * glares (car headlights / street lamps; red/blue police siren). Cached. */
int  hwr_atlas_glow_slot(int variant);

int  hwr_sprites_render(const unsigned char *pal8, int filter_linear);
int  hwr_shadows_render(void);
/* Screen-space coloured overlay quads (shield-hit spheres / blast rings /
 * lightning). Call after the scene is resolved to the back buffer. */
int  hwr_overlay_render(void);
int  hwr_beams_render(void);
void hwr_sprites_reset(void);

#endif
