/******************************************************************************/
// Syndicate Wars FX3D - OpenGL hardware renderer for Bullfrog titles.
/******************************************************************************/
/**                        2026 danny@fraxle.net                             **/
/******************************************************************************/
/** @file hwr_lights.c
 *     Light-colour table: maps FullLight.Command ids to RGB + intensity scale.
 * @par Purpose:
 *     Loads fx3d_lights.ini so different lamp types (street light, fire, plasma
 *     discharge, etc.) can be given distinct colours without recompiling.  The
 *     table is keyed on FullLight.Command, the per-light type index stored in
 *     the game's level data.
 */
/******************************************************************************/
#include "hwr_lights.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define HWR_LIGHT_CMD_MAX 256
#define HWR_TYPECAT_FILE "fx3d_lights.ini"

static HwrLightColor hwr_light_table[HWR_LIGHT_CMD_MAX];
static int           hwr_light_table_init = 0;

/* Stored path from hwr_lights_load() so hwr_lights_save() writes to the
 * same file instead of the (potentially different) working directory. */
static char          hwr_lights_path[512];

/* Per-(Type,SubType) category: 0=unset, 1=filler, 2=building, 3=street */
static unsigned char hwr_thing_cats[256][256];

/* Built-in defaults, overridden by the [defaultlighting] section.
 * Matches SW inverse-square additive model. Was cyberpunk noir before the rewrite. */
static HwrLightDefaults hwr_defaults = {
    1.0f,           /* intensity/gain — linear model needs 1.0, no inverse-square */
    21.0f,           /* radius (21 = exact SW inverse-square constant at 34019) */
    2.0f,           /* shadow_strength (heavy anti-light darkening) */
    0.0f,           /* falloff (unused with inverse-square model, kept for compat) */
    0.55f,          /* ambient — daylight base fill (sun off by default; the
                     * smoothed baked-Ambient floor shade modulates this) */
    1.0f, 1.0f, 1.0f, /* tint */
    1.0f,           /* ao — full strength: applies the baked floor shade/shadows */
    4194304.0f,     /* max_light_dist2 (8 tiles squared in PRCCOORD) */
    0,              /* ssao_enable — off by default (less GPU, no G-buffer) */
    0.016f,         /* ssao_radius (UV) */
    320.0f,         /* ssao_world (units) */
    2.0f,           /* ssao_strength */
    64.0f,          /* ssao_bias (min occluder height, world units) */
    0,              /* ssao_debug */
    /* --- sun (visible highlights, subtle shadows) --- */
    0,              /* sun_enable — OFF by default; baked-Ambient floor shade is
                     * the default shadow system. Set 1 to use the GL sun shadow map. */
    0.10f,          /* sun_bright */
    0.02f,          /* sun_ambient */
    315.0f,         /* sun_azimuth (NW) */
    0,              /* sun_auto_azimuth (per-level derive from baked shading; unreliable, off) */
    35.0f,          /* sun_elevation */
    2,              /* sun_pcf (5x5) */
    0.0005f,        /* sun_bias */
    2.0f,           /* sun_slope */
    4.0f,           /* sun_units */
    0,              /* sun_debug */
    0.0f,           /* sun_haze (crisp shadow edges) */
    0,              /* light_debug */
     0,              /* thingno_debug */
     0,              /* sprite_debug */
    /* Per-category brightness: fillers off, buildings moderate, streetlamps vivid */
    0.0f,           /* filler_brightness (0% — fillers off entirely) */
    0.3f,           /* building_brightness (30% — dim building pools) */
    1.5f,           /* street_brightness (150% — streetlamp glow spots) */
    21.0f,          /* filler_radius (21 = SW default) */
    21.0f,          /* building_radius (21 = SW default) */
    21.0f,          /* street_radius (21 = SW default) */
    50,             /* filler_maxint — Intensity ≤ 50 = filler */
    200,            /* building_maxint — Intensity ≤ 200 = building, > 200 = street */
    0,              /* xbr_scale — off by default */
    /* --- transparency (Phase 8) --- */
    1,              /* transp_enable — blended faces on */
    0.5f,           /* transp_alpha */
    1,              /* transp_sprite_enable — blended sprites on */
    0.85f,          /* transp_sprite_alpha — slightly see-through glow */
    0,              /* transp_debug — diagnostic: force all faces transparent */
    /* --- glare billboard settings --- */
    6.5f,           /* glare_headlamp_width */
    6.5f,           /* glare_red_width */
    6.5f,           /* glare_blue_width */
    1.0f,           /* glare_headlamp_alpha (vi) */
    3.5f,           /* glare_red_alpha (vi) */
    5.5f,           /* glare_blue_alpha (vi) */
    /* --- fire dynamic light --- */
    1,              /* firelight_enable */
    1.6f,           /* firelight_brightness */
    11.0f,          /* firelight_radius (~5.7 tile pool) */
    0.30f,          /* firelight_flicker */
    3.0f,           /* firelight_cluster (merge radius, tiles) */
    1,              /* firelight_min_flames (light all clusters) */
    1.0f,           /* face_ao (1.0 = SW-linear building shade, >1 = darker curve) */
    0.6f,           /* shade_sat (shadow saturation boost; 0 = plain linear) */
    1.6f,           /* shadow_depth (baked floor-shadow gamma; 1 = linear SW) */
    0.3f,           /* sprite_persp_strength (0=distance-flat, 1=full 3D
                     * perspective; see hwr_billboard_dist_scale in
                     * hwr_sprite.c). Tune live via [sprites] persp_strength. */
    468.0f,         /* sprite_persp_zoom_ref (zoom/scale at which sprite size is
                     * nominal; size ∝ scale/ref. <=0 disables zoom scaling.
                     * Tune live via [sprites] persp_zoom_ref). */
    /* --- procedural rain overlay --- */
    1,              /* rain_enable */
    0.35f,          /* rain_alpha (translucent) */
    600.0f,         /* rain_density (columns per screen-height of width) */
    2.5f,           /* rain_speed (screen-heights/second) */
    0.6f,           /* rain_width (very thin streaks, pixels) */
    0.05f,          /* rain_length (fraction of screen height) */
    0.0f,           /* rain_angle (degrees, 0 = straight down) */
};

static void table_defaults(void)
{
    int i;
    for (i = 0; i < HWR_LIGHT_CMD_MAX; i++) {
        hwr_light_table[i].r = 1.0f;
        hwr_light_table[i].g = 1.0f;
        hwr_light_table[i].b = 1.0f;
        hwr_light_table[i].intensity_scale = 1.0f;
        hwr_light_table[i].brightness = 1.0f;
    }
    hwr_light_table_init = 1;

    /* Default category assignments: T5S1 = STREET, T5S2 = BUILDING */
    hwr_thing_cats[5][1] = 3;
    hwr_thing_cats[5][2] = 2;
}

void hwr_lights_clear(void)
{
    table_defaults();
}

/* Section ids for the simple line-by-line parser. */
enum { SEC_NONE = 0, SEC_LIGHTS, SEC_DEFAULTS, SEC_SSAO, SEC_SUN, SEC_CATEGORIES, SEC_SPRITES, SEC_TRANSP, SEC_GLARE, SEC_FIRELIGHT, SEC_RAIN };

static void parse_transp_line(const char *p)
{
    float fv;
    int iv;
    if (sscanf(p, "enable = %d", &iv) == 1) {
        hwr_defaults.transp_enable = (iv != 0) ? 1 : 0;
    } else if (sscanf(p, "alpha = %f", &fv) == 1) {
        if (fv < 0.0f) fv = 0.0f;
        if (fv > 1.0f) fv = 1.0f;
        hwr_defaults.transp_alpha = fv;
    } else if (sscanf(p, "sprite_enable = %d", &iv) == 1) {
        hwr_defaults.transp_sprite_enable = (iv != 0) ? 1 : 0;
    } else if (sscanf(p, "sprite_alpha = %f", &fv) == 1) {
        if (fv < 0.0f) fv = 0.0f;
        if (fv > 1.0f) fv = 1.0f;
        hwr_defaults.transp_sprite_alpha = fv;
    } else if (sscanf(p, "debug = %d", &iv) == 1) {
        hwr_defaults.transp_debug = (iv != 0) ? 1 : 0;
    }
}

static void parse_glare_line(const char *p)
{
    float fv;
    if (sscanf(p, "headlamp_width = %f", &fv) == 1) {
        if (fv < 0.5f) fv = 0.5f;
        hwr_defaults.glare_headlamp_width = fv;
    } else if (sscanf(p, "red_width = %f", &fv) == 1) {
        if (fv < 0.5f) fv = 0.5f;
        hwr_defaults.glare_red_width = fv;
    } else if (sscanf(p, "blue_width = %f", &fv) == 1) {
        if (fv < 0.5f) fv = 0.5f;
        hwr_defaults.glare_blue_width = fv;
    } else if (sscanf(p, "headlamp_alpha = %f", &fv) == 1) {
        if (fv < 0.0f) fv = 0.0f;
        hwr_defaults.glare_headlamp_alpha = fv;
    } else if (sscanf(p, "red_alpha = %f", &fv) == 1) {
        if (fv < 0.0f) fv = 0.0f;
        hwr_defaults.glare_red_alpha = fv;
    } else if (sscanf(p, "blue_alpha = %f", &fv) == 1) {
        if (fv < 0.0f) fv = 0.0f;
        hwr_defaults.glare_blue_alpha = fv;
    }
}

static void parse_firelight_line(const char *p)
{
    float fv;
    int iv;
    if (sscanf(p, "enable = %d", &iv) == 1) {
        hwr_defaults.firelight_enable = (iv != 0) ? 1 : 0;
    } else if (sscanf(p, "brightness = %f", &fv) == 1) {
        if (fv < 0.0f) fv = 0.0f;
        hwr_defaults.firelight_brightness = fv;
    } else if (sscanf(p, "radius = %f", &fv) == 1) {
        if (fv < 1.0f) fv = 1.0f;
        hwr_defaults.firelight_radius = fv;
    } else if (sscanf(p, "flicker = %f", &fv) == 1) {
        if (fv < 0.0f) fv = 0.0f;
        if (fv > 1.0f) fv = 1.0f;
        hwr_defaults.firelight_flicker = fv;
    } else if (sscanf(p, "cluster = %f", &fv) == 1) {
        if (fv < 0.5f) fv = 0.5f;
        hwr_defaults.firelight_cluster = fv;
    } else if (sscanf(p, "min_flames = %d", &iv) == 1) {
        if (iv < 1) iv = 1;
        hwr_defaults.firelight_min_flames = iv;
    }
}

static void parse_rain_line(const char *p)
{
    float fv;
    int iv;
    if (sscanf(p, "enable = %d", &iv) == 1) {
        hwr_defaults.rain_enable = (iv != 0) ? 1 : 0;
    } else if (sscanf(p, "alpha = %f", &fv) == 1) {
        if (fv < 0.0f) fv = 0.0f;
        if (fv > 1.0f) fv = 1.0f;
        hwr_defaults.rain_alpha = fv;
    } else if (sscanf(p, "density = %f", &fv) == 1) {
        if (fv < 1.0f) fv = 1.0f;
        hwr_defaults.rain_density = fv;
    } else if (sscanf(p, "speed = %f", &fv) == 1) {
        if (fv < 0.0f) fv = 0.0f;
        hwr_defaults.rain_speed = fv;
    } else if (sscanf(p, "width = %f", &fv) == 1) {
        if (fv < 0.1f) fv = 0.1f;
        hwr_defaults.rain_width = fv;
    } else if (sscanf(p, "length = %f", &fv) == 1) {
        if (fv < 0.0f) fv = 0.0f;
        if (fv > 1.0f) fv = 1.0f;
        hwr_defaults.rain_length = fv;
    } else if (sscanf(p, "angle = %f", &fv) == 1) {
        hwr_defaults.rain_angle = fv;
    }
}

static void parse_sun_line(const char *p)
{
    float fv;
    int iv;
    if (sscanf(p, "enable = %d", &iv) == 1) {
        hwr_defaults.sun_enable = (iv != 0);
    } else if (sscanf(p, "brightness = %f", &fv) == 1) {
        if (fv < 0.0f) fv = 0.0f;
        if (fv > 1.0f) fv = 1.0f;
        hwr_defaults.sun_bright = fv;
    } else if (sscanf(p, "ambient = %f", &fv) == 1) {
        if (fv < 0.0f) fv = 0.0f;
        if (fv > 1.0f) fv = 1.0f;
        hwr_defaults.sun_ambient = fv;
    } else if (sscanf(p, "azimuth = %f", &fv) == 1) {
        hwr_defaults.sun_azimuth = fv;
    } else if (sscanf(p, "auto_azimuth = %d", &iv) == 1) {
        hwr_defaults.sun_auto_azimuth = (iv != 0);
    } else if (sscanf(p, "elevation = %f", &fv) == 1) {
        if (fv < 0.0f) fv = 0.0f;
        if (fv > 90.0f) fv = 90.0f;
        hwr_defaults.sun_elevation = fv;
    } else if (sscanf(p, "softness = %d", &iv) == 1) {
        if (iv < 0) iv = 0;
        if (iv > 12) iv = 12;
        hwr_defaults.sun_pcf = iv;
    } else if (sscanf(p, "bias = %f", &fv) == 1) {
        hwr_defaults.sun_bias = fv;
    } else if (sscanf(p, "slope = %f", &fv) == 1) {
        if (fv < 0.0f) fv = 0.0f;
        hwr_defaults.sun_slope = fv;
    } else if (sscanf(p, "units = %f", &fv) == 1) {
        if (fv < 0.0f) fv = 0.0f;
        hwr_defaults.sun_units = fv;
    } else if (sscanf(p, "debug = %d", &iv) == 1) {
        hwr_defaults.sun_debug = (iv != 0) ? 1 : 0;
    } else if (sscanf(p, "haze = %f", &fv) == 1) {
        if (fv < 0.0f) fv = 0.0f;
        if (fv > 1.0f) fv = 1.0f;
        hwr_defaults.sun_haze = fv;
    }
}

static void parse_ssao_line(const char *p)
{
    float fv;
    int iv;
    if (sscanf(p, "enable = %d", &iv) == 1) {
        hwr_defaults.ssao_enable = (iv != 0);
    } else if (sscanf(p, "radius = %f", &fv) == 1) {
        if (fv > 0.0f) hwr_defaults.ssao_radius = fv;
    } else if (sscanf(p, "world = %f", &fv) == 1) {
        if (fv > 0.0f) hwr_defaults.ssao_world = fv;
    } else if (sscanf(p, "strength = %f", &fv) == 1) {
        if (fv < 0.0f) fv = 0.0f;
        hwr_defaults.ssao_strength = fv;
    } else if (sscanf(p, "bias = %f", &fv) == 1) {
        hwr_defaults.ssao_bias = fv;
    } else if (sscanf(p, "debug = %d", &iv) == 1) {
        if (iv < 0) iv = 0;
        if (iv > 3) iv = 3;
        hwr_defaults.ssao_debug = iv;
    }
}

static void parse_default_line(const char *p)
{
    float fv;
    int r, g, b, iv;
    if (sscanf(p, "intensity = %f", &fv) == 1) {
        /* Accept 0..200 (percent) and clamp; store as 0..2 gain. */
        if (fv < 0.0f) fv = 0.0f;
        if (fv > 200.0f) fv = 200.0f;
        hwr_defaults.intensity = fv / 100.0f;
    } else if (sscanf(p, "shadow = %f", &fv) == 1) {
        if (fv < 0.0f) fv = 0.0f;
        hwr_defaults.shadow_strength = fv;
    } else if (sscanf(p, "radius = %f", &fv) == 1) {
        hwr_defaults.radius = fv;
    } else if (sscanf(p, "falloff = %f", &fv) == 1) {
        if (fv < 0.1f) fv = 0.1f;
        hwr_defaults.falloff = fv;
    } else if (sscanf(p, "ambient = %f", &fv) == 1) {
        if (fv < 0.0f) fv = 0.0f;
        if (fv > 1.0f) fv = 1.0f;
        hwr_defaults.ambient = fv;
    } else if (sscanf(p, "tint = %d %d %d", &r, &g, &b) == 3) {
        hwr_defaults.tint_r = (float)r / 255.0f;
        hwr_defaults.tint_g = (float)g / 255.0f;
        hwr_defaults.tint_b = (float)b / 255.0f;
    } else if (sscanf(p, "ao = %f", &fv) == 1) {
        /* 0..100 percent -> 0..1 strength; above 100 (up to 300) the baked
         * shade is raised to a power (darkening curve) for a deeper look. */
        if (fv < 0.0f) fv = 0.0f;
        if (fv > 300.0f) fv = 300.0f;
        hwr_defaults.ao = fv / 100.0f;
    } else if (sscanf(p, "face_ao = %f", &fv) == 1) {
        /* Building/object face shade strength: 100 = SW-linear, >100 = power
         * curve (darker), independent of the floor ao. */
        if (fv < 0.0f) fv = 0.0f;
        if (fv > 300.0f) fv = 300.0f;
        hwr_defaults.face_ao = fv / 100.0f;
    } else if (sscanf(p, "shade_sat = %f", &fv) == 1) {
        /* Shadow saturation boost, percent: 0 = plain linear shading,
         * 60 = SW-like hue-rich darks, 200 = maximum. */
        if (fv < 0.0f) fv = 0.0f;
        if (fv > 200.0f) fv = 200.0f;
        hwr_defaults.shade_sat = fv / 100.0f;
    } else if (sscanf(p, "shadow_depth = %f", &fv) == 1) {
        /* Baked floor-shadow gamma, percent: 100 = linear SW Ambient,
         * >100 = deeper shadows (lit ground unchanged). */
        if (fv < 50.0f)  fv = 50.0f;
        if (fv > 400.0f) fv = 400.0f;
        hwr_defaults.shadow_depth = fv / 100.0f;
    } else if (sscanf(p, "filler_brightness = %f", &fv) == 1) {
        if (fv < 0.0f) fv = 0.0f;
        if (fv > 2.0f) fv = 2.0f;
        hwr_defaults.filler_brightness = fv;
    } else if (sscanf(p, "building_brightness = %f", &fv) == 1) {
        if (fv < 0.0f) fv = 0.0f;
        if (fv > 10.0f) fv = 10.0f;
        hwr_defaults.building_brightness = fv;
    } else if (sscanf(p, "street_brightness = %f", &fv) == 1) {
        if (fv < 0.0f) fv = 0.0f;
        if (fv > 10.0f) fv = 10.0f;
        hwr_defaults.street_brightness = fv;
    } else if (sscanf(p, "max_light_dist = %f", &fv) == 1) {
        /* Tiles -> PRCCOORD² (XZ Euclidean radius). */
        if (fv < 1.0f) fv = 1.0f;
        hwr_defaults.max_light_dist2 = (fv * 256.0f) * (fv * 256.0f);
    } else if (sscanf(p, "filler_radius = %f", &fv) == 1) {
        if (fv < 1.0f) fv = 1.0f;
        hwr_defaults.filler_radius = fv;
    } else if (sscanf(p, "building_radius = %f", &fv) == 1) {
        if (fv < 1.0f) fv = 1.0f;
        hwr_defaults.building_radius = fv;
    } else if (sscanf(p, "street_radius = %f", &fv) == 1) {
        if (fv < 1.0f) fv = 1.0f;
        hwr_defaults.street_radius = fv;
    } else if (sscanf(p, "filler_maxint = %d", &iv) == 1) {
        if (iv < 0) iv = 0;
        if (iv > 1000) iv = 1000;
        hwr_defaults.filler_maxint = iv;
    } else if (sscanf(p, "building_maxint = %d", &iv) == 1) {
        if (iv < 0) iv = 0;
        if (iv > 5000) iv = 5000;
        hwr_defaults.building_maxint = iv;
    } else if (sscanf(p, "thingno_debug = %d", &iv) == 1) {
        hwr_defaults.thingno_debug = (iv != 0) ? 1 : 0;
    }
}

static void parse_categories_line(const char *p)
{
    int t, s, c;
    /* Format: "type:subtype = category"  —  type,subtype 0-255, category 0-3 */
    if (sscanf(p, "%d:%d = %d", &t, &s, &c) == 3) {
        if (t >= 0 && t < 256 && s >= 0 && s < 256 && c >= 0 && c <= 3)
            hwr_thing_cats[t][s] = (unsigned char)c;
    }
}

static void parse_sprites_line(const char *p)
{
    int iv;
    float fv;
    if (sscanf(p, "sprite_debug = %d", &iv) == 1) {
        hwr_defaults.sprite_debug = (iv != 0) ? 1 : 0;
    } else if (sscanf(p, "xbr_scale = %d", &iv) == 1) {
        if (iv < 0) iv = 0;
        if (iv == 1) iv = 0;
        if (iv > 4) iv = 4;
        hwr_defaults.xbr_scale = iv;
    } else if (sscanf(p, "persp_strength = %f", &fv) == 1) {
        if (fv < 0.0f) fv = 0.0f;
        hwr_defaults.sprite_persp_strength = fv;
    } else if (sscanf(p, "persp_zoom_ref = %f", &fv) == 1) {
        hwr_defaults.sprite_persp_zoom_ref = fv;
    }
}

void hwr_thing_category_save_all(void)
{
    const char *path = hwr_lights_path;
    FILE *f, *out;
    char *buf = NULL;
    long flen;
    int t, s;

    if (*path == '\0')
        path = HWR_TYPECAT_FILE;

    f = fopen(path, "r");
    if (f) {
        fseek(f, 0, SEEK_END);
        flen = ftell(f);
        fseek(f, 0, SEEK_SET);
        buf = (char*)malloc((size_t)(flen + 1));
        if (buf) {
            size_t r = fread(buf, 1, (size_t)flen, f);
            buf[r] = '\0';
        }
        fclose(f);
    }

    out = fopen(path, "w");
    if (!out) { free(buf); return; }

    if (buf) {
        /* Copy lines up to [thing_categories] or end, skipping any existing [thing_categories] block */
        char *p = buf;
        int skipping = 0;
        while (*p) {
            char *nl = strchr(p, '\n');
            size_t linelen = nl ? (size_t)(nl - p + 1) : strlen(p);
            if (*p == '[' && strncmp(p, "[thing_categories]", 18) == 0) {
                skipping = 1;
            } else if (skipping && *p == '[') {
                skipping = 0;
            }
            if (!skipping) {
                fwrite(p, 1, linelen, out);
            }
            if (nl) p = nl + 1; else break;
        }
        free(buf);
    }

    /* Write new [thing_categories] section */
    fprintf(out, "\n[thing_categories]\n");
    fprintf(out, "; type:subtype = category  (0=unset 1=filler 2=building 3=street)\n");
    for (t = 0; t < 256; t++) {
        for (s = 0; s < 256; s++) {
            if (hwr_thing_cats[t][s] != 0) {
                fprintf(out, "%d:%d = %d\n", t, s, (int)hwr_thing_cats[t][s]);
            }
        }
    }
    fclose(out);
}

int hwr_thing_category_get(int type, int subtype)
{
    if (type < 0 || type >= 256 || subtype < 0 || subtype >= 256)
        return 0;
    return (int)hwr_thing_cats[type][subtype];
}

void hwr_thing_category_set(int type, int subtype, int cat)
{
    if (type < 0 || type >= 256 || subtype < 0 || subtype >= 256)
        return;
    if (cat < 0) cat = 0;
    if (cat > 3) cat = 3;
    hwr_thing_cats[type][subtype] = (unsigned char)cat;
}

void hwr_lights_load(const char *path)
{
    FILE *f;
    char line[256];
    int section = SEC_NONE;

    if (!hwr_light_table_init)
        table_defaults();

    strncpy(hwr_lights_path, path, sizeof(hwr_lights_path) - 1);
    hwr_lights_path[sizeof(hwr_lights_path) - 1] = '\0';

    f = fopen(path, "r");
    if (f == NULL)
        return;   /* missing file is not an error; defaults stand */

    while (fgets(line, sizeof(line), f) != NULL) {
        char *p = line;
        /* Skip leading whitespace. */
        while (*p == ' ' || *p == '\t') p++;
        if (*p == ';' || *p == '\0' || *p == '\r' || *p == '\n')
            continue;
        if (*p == '[') {
            if (strncmp(p, "[fx3d_lights]", 13) == 0)
                section = SEC_LIGHTS;
            else if (strncmp(p, "[defaultlighting]", 17) == 0)
                section = SEC_DEFAULTS;
            else if (strncmp(p, "[ssao]", 6) == 0)
                section = SEC_SSAO;
            else if (strncmp(p, "[sun]", 5) == 0)
                section = SEC_SUN;
            else if (strncmp(p, "[thing_categories]", 18) == 0)
                section = SEC_CATEGORIES;
            else if (strncmp(p, "[sprites]", 9) == 0)
                section = SEC_SPRITES;
            else if (strncmp(p, "[transparency]", 14) == 0)
                section = SEC_TRANSP;
            else if (strncmp(p, "[glare]", 7) == 0)
                section = SEC_GLARE;
            else if (strncmp(p, "[firelight]", 11) == 0)
                section = SEC_FIRELIGHT;
            else if (strncmp(p, "[rain]", 6) == 0)
                section = SEC_RAIN;
            else
                section = SEC_NONE;
            continue;
        }
        if (section == SEC_DEFAULTS) {
            parse_default_line(p);
        } else if (section == SEC_SSAO) {
            parse_ssao_line(p);
        } else if (section == SEC_SUN) {
            parse_sun_line(p);
        } else if (section == SEC_LIGHTS) {
            int id, r, g, b, iv, n;
            float scale = 1.0f, bright = 1.0f;
            if (sscanf(p, "brightness_debug = %d", &iv) == 1) {
                hwr_defaults.light_debug = (iv != 0);
            } else if ((n = sscanf(p, "%d = %d %d %d %f %f", &id, &r, &g, &b, &scale, &bright)) >= 4) {
                if (id >= 0 && id < HWR_LIGHT_CMD_MAX) {
                    hwr_light_table[id].r = (float)r / 255.0f;
                    hwr_light_table[id].g = (float)g / 255.0f;
                    hwr_light_table[id].b = (float)b / 255.0f;
                    hwr_light_table[id].intensity_scale = (n >= 5) ? scale : 1.0f;
                    hwr_light_table[id].brightness = (n >= 6) ? bright : 1.0f;
                }
            }
        } else if (section == SEC_CATEGORIES) {
            parse_categories_line(p);
        } else if (section == SEC_SPRITES) {
            parse_sprites_line(p);
        } else if (section == SEC_TRANSP) {
            parse_transp_line(p);
        } else if (section == SEC_GLARE) {
            parse_glare_line(p);
        } else if (section == SEC_FIRELIGHT) {
            parse_firelight_line(p);
        } else if (section == SEC_RAIN) {
            parse_rain_line(p);
        }
    }
    fclose(f);
}

HwrLightDefaults hwr_lights_defaults(void)
{
    return hwr_defaults;
}

HwrLightDefaults *hwr_lights_ptr(void)
{
    return &hwr_defaults;
}

/* ---- Write the three tunable sections back to fx3d_lights.ini ----------- */

static void write_defaults(FILE *out)
{
    fprintf(out,
        "[defaultlighting]\n"
        "intensity          = %.0f\n"
        "radius             = %.0f\n"
        "shadow             = %.1f\n"
        "ambient            = %.2f\n"
        "tint               = %d %d %d\n"
        "ao                 = %.0f\n"
        "face_ao            = %.0f\n"
        "shade_sat          = %.0f\n"
        "shadow_depth       = %.0f\n"
        "max_light_dist     = %.0f\n"
        "filler_brightness  = %.2f\n"
        "building_brightness = %.2f\n"
        "street_brightness  = %.2f\n"
        "filler_radius      = %.0f\n"
        "building_radius    = %.0f\n"
        "street_radius      = %.0f\n"
        "filler_maxint      = %d\n"
        "building_maxint    = %d\n"
        "thingno_debug      = %d\n",
        hwr_defaults.intensity * 100.0f,
        hwr_defaults.radius,
        hwr_defaults.shadow_strength,
        hwr_defaults.ambient,
        (int)(hwr_defaults.tint_r * 255.0f),
        (int)(hwr_defaults.tint_g * 255.0f),
        (int)(hwr_defaults.tint_b * 255.0f),
        hwr_defaults.ao * 100.0f,
        hwr_defaults.face_ao * 100.0f,
        hwr_defaults.shade_sat * 100.0f,
        hwr_defaults.shadow_depth * 100.0f,
        (double)(sqrtf(hwr_defaults.max_light_dist2) / 256.0f),
        hwr_defaults.filler_brightness,
        hwr_defaults.building_brightness,
        hwr_defaults.street_brightness,
        hwr_defaults.filler_radius,
        hwr_defaults.building_radius,
        hwr_defaults.street_radius,
        hwr_defaults.filler_maxint,
        hwr_defaults.building_maxint,
        hwr_defaults.thingno_debug);
}

static void write_ssao(FILE *out)
{
    fprintf(out,
        "[ssao]\n"
        "enable   = %d\n"
        "radius   = %.3f\n"
        "world    = %.0f\n"
        "strength = %.1f\n"
        "bias     = %.0f\n"
        "debug    = %d\n",
        hwr_defaults.ssao_enable,
        hwr_defaults.ssao_radius,
        hwr_defaults.ssao_world,
        hwr_defaults.ssao_strength,
        hwr_defaults.ssao_bias,
        hwr_defaults.ssao_debug);
}

static void write_sun(FILE *out)
{
    fprintf(out,
        "[sun]\n"
        "enable     = %d\n"
        "brightness = %.2f\n"
        "ambient    = %.2f\n"
        "azimuth    = %.0f\n"
        "auto_azimuth = %d\n"
        "elevation  = %.0f\n"
        "softness   = %d\n"
        "bias       = %.4f\n"
        "slope      = %.1f\n"
        "units      = %.1f\n"
        "haze       = %.2f\n"
        "debug      = %d\n",
        hwr_defaults.sun_enable,
        hwr_defaults.sun_bright,
        hwr_defaults.sun_ambient,
        hwr_defaults.sun_azimuth,
        hwr_defaults.sun_auto_azimuth,
        hwr_defaults.sun_elevation,
        hwr_defaults.sun_pcf,
        hwr_defaults.sun_bias,
        hwr_defaults.sun_slope,
        hwr_defaults.sun_units,
        hwr_defaults.sun_haze,
        hwr_defaults.sun_debug);
}

void hwr_lights_save(void)
{
    const char *path = hwr_lights_path;
    FILE *f, *out;
    char *buf = NULL;
    long flen;

    if (*path == '\0')
        path = HWR_TYPECAT_FILE;

    f = fopen(path, "r");
    if (f) {
        fseek(f, 0, SEEK_END);
        flen = ftell(f);
        fseek(f, 0, SEEK_SET);
        buf = (char*)malloc((size_t)(flen + 1));
        if (buf) {
            size_t r = fread(buf, 1, (size_t)flen, f);
            buf[r] = '\0';
        }
        fclose(f);
    }

    out = fopen(path, "w");
    if (!out) { free(buf); return; }

    if (buf) {
        char *p = buf;
        int replacing = 0;
        while (*p) {
            char *nl = strchr(p, '\n');
            size_t linelen = nl ? (size_t)(nl - p + 1) : strlen(p);

            if (*p == '[') {
                int is_dflt = (strncmp(p, "[defaultlighting]", 17) == 0);
                int is_ssao = (strncmp(p, "[ssao]", 6) == 0);
                int is_sun  = (strncmp(p, "[sun]", 5) == 0);

                if (is_dflt || is_ssao || is_sun) {
                    if (is_dflt) write_defaults(out);
                    else if (is_ssao) write_ssao(out);
                    else write_sun(out);
                    replacing = 1;
                } else {
                    if (replacing) replacing = 0;
                    fwrite(p, 1, linelen, out);
                }
            } else if (!replacing) {
                fwrite(p, 1, linelen, out);
            } else if (strchr(p, '=') == NULL) {
                fwrite(p, 1, linelen, out);
            }

            if (nl) p = nl + 1; else break;
        }
        free(buf);
    } else {
        fprintf(out, "; FX3D Lighting Configuration (auto-saved from tuning panel)\n\n");
        write_defaults(out);
        fprintf(out, "\n");
        write_ssao(out);
        fprintf(out, "\n");
        write_sun(out);
    }

    fclose(out);
}

HwrLightColor hwr_lights_lookup(int id)
{
    static const HwrLightColor dflt = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    if (!hwr_light_table_init)
        table_defaults();
    if (id < 0 || id >= HWR_LIGHT_CMD_MAX)
        return dflt;
    return hwr_light_table[id];
}
