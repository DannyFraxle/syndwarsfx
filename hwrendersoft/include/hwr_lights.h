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
    int   sun_auto_azimuth; /**< 1 = override sun_azimuth with the per-level angle auto-derived from the baked SW floor shading. */
    float sun_elevation;    /**< Sun angle above horizon, degrees. */
    int   sun_pcf;          /**< PCF kernel half-radius in texels (0=no PCF, 1=3x3, ..., 12=25x25). */
    float sun_bias;         /**< Constant depth bias in shadow shader (small, post-offset). */
    float sun_slope;        /**< glPolygonOffset factor  (slope-proportional bias). */
    float sun_units;        /**< glPolygonOffset units   (constant depth-unit bias). */
    int   sun_debug;        /**< 1 = show greyscale lit factor for calibration. */
    float sun_haze;         /**< 0..1: haze/atmospheric scatter; higher = softer, less defined shadows. */
    int   light_debug;      /**< 1 = write fx3d_light_ids.txt with all light IDs in current level. */
    int   thingno_debug;     /**< 1 = overlay ThingNo labels on all in-game objects. */
    int   sprite_debug;       /**< 1 = overlay sprite debug labels on billboards. */
    /* Per-category brightness + radius ([defaultlighting]).
     * Category is determined by the Thing (Type,SubType) mapping from
     * LightHead-connected lights. Unconnected lights default to filler. */
    float filler_brightness;    /**< 0.0-2.0: brightness for filler lights */
    float building_brightness;  /**< 0.0-2.0: brightness for building/object lights */
    float street_brightness;    /**< 0.0-2.0: brightness for street lamps */
    float filler_radius;        /**< Radius multiplier for filler category */
    float building_radius;      /**< Radius multiplier for building category */
    float street_radius;        /**< Radius multiplier for street category */
    int   filler_maxint;        /**< Intensity threshold: <= this => filler */
    int   building_maxint;      /**< Intensity threshold: <= this => building, > => street */
    int   xbr_scale;            /**< 0=off, 2=2x, 3=3x, 4=4x upscale via xBR */
    /* Transparency pass ([transparency] section, Phase 8). */
    int   transp_enable;        /**< 1 = draw semi-transparent faces (deep-radar + glass/fence) blended. */
    float transp_alpha;         /**< Blended face opacity, 0..1 (default 0.5). */
    int   transp_sprite_enable; /**< 1 = draw translucent sprites (fire/smoke/glow) blended in GL. */
    float transp_sprite_alpha;  /**< Translucent sprite opacity scale, 0..1 (default 1.0). */
    int   transp_debug;         /**< 1 = force ALL building/object faces transparent (diagnostic). */
    /* Glare billboard controls ([glare] section). */
    float glare_headlamp_width; /**< Billboard size multiplier for white headlamp/lamp glare (default 6.5). */
    float glare_red_width;      /**< Billboard size multiplier for red siren glare (default 6.5). */
    float glare_blue_width;     /**< Billboard size multiplier for blue siren glare (default 6.5). */
    float glare_headlamp_alpha; /**< Glow texture intensity (vi) for white headlamp (default 1.0). */
    float glare_red_alpha;      /**< Glow texture intensity (vi) for red siren (default 3.5). */
    float glare_blue_alpha;     /**< Glow texture intensity (vi) for blue siren (default 5.5). */
    /* Fire dynamic light ([firelight] section). Ground fires emit a flickering
     * warm point light so they illuminate the floor and nearby objects — SW
     * parity for the apply_full_light path the GL renderer otherwise misses
     * (the flames' own billboards are self-lit and cast nothing). */
    int   firelight_enable;     /**< 1 = fires cast dynamic ground light. */
    float firelight_brightness; /**< RGB gain for fire light (default 1.6). */
    float firelight_radius;     /**< Reach multiplier, 21 convention (default 11 ≈ 5.7 tiles). */
    float firelight_flicker;    /**< Flicker depth, 0..1 (default 0.3). */
    float firelight_cluster;    /**< Merge radius in tiles: flames within this of a
                                     cluster join it, so one blaze (and neighbouring
                                     fires) = one light instead of one-per-tile.
                                     Default 3. Bigger = fewer, larger lights. */
    int   firelight_min_flames; /**< A cluster needs at least this many flames to emit
                                     a light — raise it to drop small/lone fires and
                                     only light real blazes. Default 1 (light all). */
    float face_ao;              /**< Building/object face baked-shade strength:
                                     1.0 = SW-linear, >1 = power curve (darker),
                                     independent of the floor ao. */
    float shade_sat;            /**< Shadow saturation boost (0 = plain linear
                                     shading, ~0.6 = SW-like hue-rich darks). */
    float shadow_depth;         /**< Baked floor-shadow gamma: 1 = linear SW
                                     Ambient, >1 = deeper shadows (lit ground
                                     unchanged). */
    /* Sprite/effects billboard perspective strength ([sprites] section).
     * Billboard corners go through the same true-3D per-vertex correction as
     * floor/face vertices, which is fine for them (their world size is always
     * correct - a tile really is 256 units), but sprite billboards also have a
     * CPU-computed reference world-size on top, meant to be depth-independent
     * - so the SAME shader term ends up scaling that reference size far more
     * than intended (measured ~0.49x-1.75x across a typical street view). A
     * flat CPU-side size multiplier can't compensate: it scales both ends of
     * that range equally, so fixing one distance makes another look wrong. */
    float sprite_persp_strength; /**< 0 = no per-object distance foreshortening
                                     (flat with DISTANCE, but still scales with
                                     zoom — see sprite_persp_zoom_ref); 1 = full
                                     uncancelled 3D perspective. Dampens between.
                                     Default 0.3. */
    /* Sprite billboard zoom reference. Billboard on-screen size is made
     * proportional to camera zoom (scale) — lockstep with the floor/world, as
     * the original SW blit did — via a scale/persp_zoom_ref multiplier. This
     * ref is the zoom (scale) value at which the multiplier is 1.0, i.e. the
     * zoom at which the sprites' calibrated ("fit") size is exact; other zooms
     * scale proportionally from there. Set it to your usual in-game zoom's
     * scale. Without this the CPU base size's 1/scale term exactly cancels the
     * shader's uScale, leaving sprites zoom-INDEPENDENT (they never shrank when
     * zooming out). */
    float sprite_persp_zoom_ref; /**< Zoom (scale) at which sprite size is
                                     nominal; size ∝ scale/this. <=0 disables
                                     the zoom scaling (old zoom-independent
                                     behaviour). Default 468. */
    /* Procedural rain overlay ([rain] section). Replaces the SW pixel-block
     * rain (which the GL keyed composite could only draw fully opaque) with a
     * genuine alpha-blended fullscreen shader pass. */
    int   rain_enable;          /**< 1 = draw the GL rain overlay when raining. */
    float rain_alpha;           /**< Streak opacity, 0..1 (default 0.35). */
    float rain_density;         /**< Streak columns per screen-height of width (default 60). */
    float rain_speed;           /**< Fall speed, screen-heights/second (default 0.6). */
    float rain_width;           /**< Streak thickness in pixels (default 1.5). */
    float rain_length;          /**< Streak length, fraction of screen height (default 0.10). */
    float rain_angle;           /**< Wind slant in degrees, 0 = straight down (default 0). */
    /* Bullet-time-on-explosion ([bullettime] section). A big explosion (see
     * bullettime_trigger() in game_speed.c, fed from do_shockwave() intensity)
     * dips the sim's world_dt/dt_units toward bullettime_scale for
     * bullettime_hold_ms, then eases back over bullettime_ramp_ms; these
     * screen-filter fields drive the accompanying GL vignette/tint so it's
     * clear the slow-down is a deliberate effect, not a hitch. */
    int   bullettime_enable;        /**< 1 = enable the effect (sim dip + screen filter). */
    float bullettime_scale;         /**< world_dt multiplier while fully dipped (default 0.25). */
    int   bullettime_hold_ms;       /**< Milliseconds held at full dip (default 700). */
    int   bullettime_ramp_ms;       /**< Milliseconds easing back to normal speed (default 900). */
    int   bullettime_min_intensity; /**< do_shockwave() intensity threshold to trigger (default 100). */
    int   bullettime_range_tiles;   /**< Max distance (tiles) from the local player's controlled
                                          agent an explosion can be and still trigger (default 15;
                                          0 = unlimited, intensity gate only). */
    float bullettime_alpha;         /**< Screen filter max opacity at full dip, 0..1 (default 0.5). */
    float bullettime_vignette;      /**< Vignette strength (edge darkening/tint), 0..1 (default 0.6). */
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
