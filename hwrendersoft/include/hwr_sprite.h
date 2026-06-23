#ifndef HWR_SPRITE_H
#define HWR_SPRITE_H

#include <stdint.h>

#define HWR_ATLAS_W         4096
#define HWR_ATLAS_H         4096
#define HWR_ATLAS_MAX_SLOTS 4096

int  hwr_rle_decode(const uint8_t *rle, uint8_t *out, int w, int h);
int  hwr_rle_decode_opaque(const uint8_t *rle, uint8_t *out, uint8_t *opq, int w, int h);

int  hwr_atlas_find(uint32_t key);  /* lookup only, no insert */
int  hwr_atlas_register(uint32_t key, const uint8_t *pixels, int w, int h);
void hwr_atlas_upload_pending(void);
void hwr_atlas_uv(int slot, float *u0, float *v0, float *u1, float *v1);
void hwr_atlas_size(int slot, int *w, int *h);
void hwr_atlas_bind(int unit);
void hwr_atlas_reset(void);

int  hwr_sprites_render(const unsigned char *pal8, int filter_linear);
int  hwr_shadows_render(void);
void hwr_sprites_reset(void);

#endif
