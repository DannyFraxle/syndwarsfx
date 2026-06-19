/******************************************************************************/
// Syndicate Wars FX3D - OpenGL hardware renderer for Bullfrog titles.
/******************************************************************************/
/**                        2026 danny@fraxle.net                             **/
/******************************************************************************/
/** @file hwr_lights.h
 *     Light-colour table: maps FullLight.Command ids to RGB + intensity scale.
 */
/******************************************************************************/
#ifndef HWR_LIGHTS_H
#define HWR_LIGHTS_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float r, g, b;          /**< Linear 0..1 colour. */
    float intensity_scale;  /**< Multiplier on the game Intensity value (radius). */
    float brightness;       /**< Per-ID brightness multiplier (0.0-2.0, default 1.0). */
} HwrLightColor;

/** Global lighting controls from the [defaultlighting] section of
 *  fx3d_lights.ini. All hand-tunable without recompiling. */
typedef struct {
    float intensity;        /**< Overall brightness gain, 0..2 (from 0-200%). */
    float radius;           /**< Inverse-square attenuation scale multiplier.
                                 CPU: Intensity * 34019 * (radius/21) * scale.
                                 radius=21 yields exact SW super-quick-light formula. */
    float shadow_strength;  /**< Darkening multiplier for negative-Intensity
                                 "anti-lights" the original used as fake shadows. */
    float falloff;          /**< Edge exponent: (1-d)^falloff; higher = tighter/darker. */
    float ambient;          /**< Floor brightness in unlit areas, 0..1. */
    float tint_r, tint_g, tint_b;  /**< Global colour cast on all lights, 0..1. */
    float ao;               /**< Per-vertex AO strength, 0..1 (legacy/baked path). */
    float max_light_dist2;  /**< Per-pixel distance cull threshold in PRCCOORD^2 (default ~4194304 = 8 tiles). */
    int   ssao_enable;      /**< 1 = run the screen-space AO pipeline. */
    float ssao_radius;      /**< SSAO screen-space sample radius (UV units). */
    float ssao_world;       /**< SSAO world-space occlusion range (units). */
    float ssao_strength;    /**< SSAO darkening multiplier. */
    float ssao_bias;        /**< SSAO self-occlusion rejection bias. */
    int   ssao_debug;       /**< 0=final, 1=world-pos, 2=raw AO, 3=blurred AO. */
    /* Sun shadow-map controls ([sun] section). */
    int   sun_enable;       /**< 1 = render depth from sun, sample in floor pass. */
    float sun_bright;       /**< Lit-ground brightness (replaces ambient when enabled). */
    float sun_ambient;      /**< Floor brightness inside shadow (dark floor). */
    float sun_azimuth;      /**< Compass bearing of sun, degrees (0=N, 90=E, 180=S, 270=W). */
    float sun_elevation;    /**< Sun angle above horizon, degrees. */
    int   sun_pcf;          /**< PCF kernel half-radius in texels (0=no PCF, 1=3x3, ..., 12=25x25). */
    float sun_bias;         /**< Constant depth bias in shadow shader (small, post-offset). */
    float sun_slope;        /**< glPolygonOffset factor  (slope-proportional bias). */
    float sun_units;        /**< glPolygonOffset units   (constant depth-unit bias). */
    int   sun_debug;        /**< 1 = show greyscale lit factor for calibration. */
    float sun_haze;         /**< 0..1: haze/atmospheric scatter; higher = softer, less defined shadows. */
    int   light_debug;      /**< 1 = write fx3d_light_ids.txt with all light IDs in current level. */
    int   thingno_debug;     /**< 1 = overlay ThingNo labels on all in-game objects. */
    /* Per-category brightness + radius ([defaultlighting]).
     * Category is determined by the Thing (Type,SubType) mapping from
     * LightHead-connected lights. Unconnected lights default to filler. */
    float filler_brightness;    /**< 0.0-2.0: brightness for filler lights */
    float building_brightness;  /**< 0.0-2.0: brightness for building/object lights */
    float street_brightness;    /**< 0.0-2.0: brightness for street lamps */
    float filler_radius;        /**< Radius multiplier for filler category */
    float building_radius;      /**< Radius multiplier for building category */
    float street_radius;        /**< Radius multiplier for street category */
} HwrLightDefaults;

/** Reset every entry to white (1,1,1) at scale 1.0 and defaults to sane values. */
void hwr_lights_clear(void);

/** Parse fx3d_lights.ini from path and populate the table.
 *  Safe to call before hwr_init(); silent on missing file (leaves defaults). */
void hwr_lights_load(const char *path);

/** Return the colour entry for command_id (default white if unknown). */
HwrLightColor hwr_lights_lookup(int command_id);

/** Return the current global lighting controls. */
HwrLightDefaults hwr_lights_defaults(void);

/** Return a mutable pointer to the internal defaults struct (for real-time
 *  tuning via the in-game panel — modifications take effect next frame). */
HwrLightDefaults *hwr_lights_ptr(void);

/** Write the current [defaultlighting], [ssao] and [sun] sections back to
 *  fx3d_lights.ini, overwriting any previous values while preserving other
 *  sections ([fx3d_lights], [thing_categories]). */
void hwr_lights_save(void);

/* ---- Per-(Type,SubType) category overrides (thing browser) --------------- */

/** Get the category for a (type, subtype) pair (0=unset, 1=filler, 2=building, 3=street). */
int  hwr_thing_category_get(int type, int subtype);

/** Set the category for a (type, subtype) pair. Clamped to 0-3. */
void hwr_thing_category_set(int type, int subtype, int cat);

/** Write all non-zero category assignments back to fx3d_lights.ini
 *  (reads existing file, merges, deduplicates, writes). */
void hwr_thing_category_save_all(void);

#ifdef __cplusplus
}
#endif
#endif
