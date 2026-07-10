/******************************************************************************/
// Syndicate Wars FX3D - OpenGL hardware renderer for Bullfrog titles.
/******************************************************************************/
/**                        2026 danny@fraxle.net                             **/
/******************************************************************************/
/** @file hwr_floor.c
 *     Phase 3: render the level floor as real 3D geometry.
 * @par Purpose:
 *     Pulls the floor geometry, camera matrix and indexed texture pages from the
 *     bound scene source, uploads them, and draws the floor with the game's exact
 *     isometric projection. Textures stay indexed (GL_R8 texture array) and are
 *     depalettised in the fragment shader against the active palette, matching
 *     the software look.
 * @par  Copying and copyrights:
 *     This program is free software; you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of the License, or
 *     (at your option) any later version.
 */
/******************************************************************************/
#include "hwr_api.h"
#include "hwr_gl.h"
#include "hwr_internal.h"
#include "hwr_lights.h"
#include "hwr_scene_source.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

/* SW's 0-terminated list of palette indices exempted from all shading (see
 * LbFadeTableToRGBGenerate, ggenf.c) - baked window/road-marking paint on an
 * otherwise normally-shaded texture uses these indices to stay full-bright at
 * any darkness level. Read once to build a 256-entry GPU lookup so the GL
 * fragment shader can mirror the same per-pixel exemption. */
extern unsigned char fade_unaffected_colours[];

/* SW's palette fade table (bfgentab.h struct TbColorTables; fade_table is the
 * FIRST member, 64 rows x 256 palette indices, row 32 = identity). Uploaded as
 * a GL texture so the fragment shader darkens colours EXACTLY like the software
 * renderer: through the palette-quantized fade rows (hue-rich hand-made dark
 * shades) instead of linear RGB multiplication (which drifts grey/washed). */
extern unsigned char pixmap[];   /* first 64*256 bytes = fade_table */

/* Reproduces transform_shpoint() per-vertex, including the mode-5 perspective
 * foreshortening (which no single matrix can express). */
static const char *floor_vert_src =
    "#version 330 core\n"
    "layout(location=0) in vec3 aPos;\n"
    "layout(location=1) in vec2 aUV;\n"
    "layout(location=2) in float aDepth;\n"  /* per-tile constant scrd */
    "layout(location=3) in uint aPage;\n"
    "layout(location=4) in float aLight;\n"  /* SW baked shade 0..1 (AO) */
    "layout(location=5) in float aEmissive;\n"  /* SW baked emissive 0..1 (windows) */
    "uniform float uD10, uD14, uD18, uD1C;\n"
    "uniform float uScale;\n"
    "uniform vec2 uCentre;   // D3C, D40\n"
    "uniform vec3 uCtr;      // camera centre: cx, 8*yc, cz\n"
    "uniform int  uPersp;\n"
    "out vec3 vUV;\n"
    "out vec3 vWorldPos;\n"
    "out float vAO;\n"
    "out float vEmissive;\n"
    "out float vScrd;\n"
    "void main(){\n"
    "    float dx = aPos.x - uCtr.x;\n"
    "    float dy = aPos.y - uCtr.y;\n"
    "    float dz = aPos.z - uCtr.z;\n"
    "    float fa = (uD14*dx - uD10*dz) / 65536.0;\n"
    "    float fb = (uD10*dx + uD14*dz) / 65536.0;\n"
    "    float fc = (uD1C*dy - uD18*fb) / 65536.0;\n"
    "    float scrd = (uD18*dy + uD1C*fb) / 65536.0;\n"
    "    if (uPersp == 5 && scrd > 1024.0)\n"
    "        scrd = 16384.0*scrd/(scrd + 16384.0);\n"
    "    float shx = uScale*fa / 2048.0;\n"
    "    float shy = uScale*fc / 2048.0;\n"
    "    if (uPersp == 5) {\n"
    "        shx = shx*(16384.0 - scrd) / 16384.0;\n"
    "        shy = shy*(16384.0 - scrd) / 16384.0;\n"
    "    }\n"
    "    float sx = uCentre.x + shx;\n"
    "    float sy = uCentre.y - shy;\n"
    "    vUV = vec3((aUV + 0.5) / 256.0, float(aPage));\n"
    "    vWorldPos = aPos;\n"
    "    vAO = aLight;\n"
    "    vEmissive = aEmissive;\n"
    "    vScrd = scrd;                   // view depth -> SSAO occlusion test\n"
    "    /* aDepth is the tile's scrd at its centre (already perspective-clamped\n"
    "     * in C), matching the SW bucket sort key. scrd is small/near-zero or\n"
    "     * negative for tiles close to the camera and asymptotes to ~16384 far\n"
    "     * away. Map that whole range across NDC z directly - the previous\n"
    "     * (scrd/8192 - 1) mapping clamped every near tile to -1, collapsing the\n"
    "     * bottom half of the screen to one depth and causing z-fighting. */\n"
    "    float ndc_z = clamp(aDepth / 16384.0, -1.0, 1.0);\n"
    "    gl_Position = vec4(sx/uCentre.x - 1.0, 1.0 - sy/uCentre.y, ndc_z, 1.0);\n"
    "}\n";

static const char *floor_frag_src =
    "#version 330 core\n"
    "in vec3 vUV;\n"
    "in vec3 vWorldPos;\n"
    "in float vAO;\n"
    "in float vEmissive;\n"
    "in float vScrd;\n"
    "layout(location=0) out vec4 frag;\n"
    "layout(location=1) out vec4 fragPos;   // xyz world pos + w view depth -> SSAO\n"
    "uniform sampler2DArray uTex;         // R8 palette indices\n"
    "uniform sampler2D uPalette;          // RGB8 256x1, active 8-bit palette\n"
    "uniform sampler2D uSelfLit;          // R8 256x1, 1.0 for SW's fade_unaffected_colours\n"
    "uniform int uTransKey;               // texel index to treat as transparent (<0 = none)\n"
    "uniform vec3  uLightPos[64];\n"
    "uniform vec3  uLightRgb[64];\n"
    "uniform float uLightRadius[64];\n"
    "uniform int   uNumLights;\n"
    "uniform float uAmbient;              // floor brightness in unlit areas (legacy / sun disabled)\n"
    "uniform float uGain;                 // overall brightness (0..2)\n"
    "uniform vec3  uTint;                 // global colour cast on all lights\n"
    "uniform float uAO;                   // ambient-occlusion strength 0..1\n"
    "uniform float uLightMaxDist2[64];    // per-light distance cull (PRCCOORD^2)\n"
    "uniform vec2  uLightFwd[64];         // shaped-headlight forward dir (0 = round)\n"
    "// --- Sun shadow map ---\n"
    "uniform sampler2D uShadowMap;        // depth texture from the sun FBO\n"
    "uniform mat4  uSunMVP;              // sun projection matrix\n"
    "uniform float uSunBright;           // lit-ground level\n"
    "uniform float uSunAmbient;          // shadowed-ground level\n"
    "uniform float uSunBias;             // depth bias\n"
    "uniform int   uSunEnable;           // 1 = use sun path\n"
    "uniform int   uSunPCF;              // PCF kernel half-radius (texels)\n"
    "uniform int   uSunDebug;            // 1 = greyscale lit factor\n"
    "uniform float uSunHaze;             // 0..1 atmospheric scatter (softens shadow edges)\n"
    "uniform int uFilter;                // 0 = nearest, 1 = palette-correct bilinear\n"
    "uniform float uAlpha;               // output alpha (1 = opaque; <1 = blended transparency)\n"
    "uniform int uDeepRadarIdx;          // palette index for deep-radar tint (page sentinel 254)\n"
    "uniform sampler2D uFadeTab;          // R8 256x64: SW palette fade table (row 32 = identity)\n"
    "uniform float uShadeSat;             // shadow saturation boost (0 = plain linear)\n"
    "vec3 pal_lookup(int idx) {\n"
    "    return texture(uPalette, vec2((float(idx) + 0.5) / 256.0, 0.5)).rgb;\n"
    "}\n"
    "float selflit_lookup(int idx) {\n"
    "    return texture(uSelfLit, vec2((float(idx) + 0.5) / 256.0, 0.5)).r;\n"
    "}\n"
    "// SW colour pipeline: darken/brighten a palette index through the real\n"
    "// fade table (palette-quantized, exactly the software renderer's colours).\n"
    "int fade_lookup(int idx, float frow) {\n"
    "    return int(texture(uFadeTab,\n"
    "        vec2((float(idx) + 0.5) / 256.0, frow)).r * 255.0 + 0.5);\n"
    "}\n"
    "void main(){\n"
    "    fragPos = vec4(vWorldPos, vScrd);   // G-buffer attachment 1\n"
    "    if (vUV.z > 253.5 && vUV.z < 254.5) {  // deep-radar: flat tint, skip lighting entirely\n"
    "        frag = vec4(pal_lookup(uDeepRadarIdx), uAlpha);\n"
    "        return;\n"
    "    }\n"
    "    vec3 light_col = vec3(0.0);\n"
    "    float shadow = 0.0;             // accumulated darkening from anti-lights\n"
    "    for (int i = 0; i < uNumLights; i++) {\n"
    "        float r = uLightRadius[i];\n"
    "        if (r == 0.0) continue;\n"
    "        vec3 delta = vWorldPos - uLightPos[i];\n"
    "        vec2 fwd = uLightFwd[i];\n"
    "        float brightness;\n"
    "        if (fwd.x != 0.0 || fwd.y != 0.0) {\n"
    "            // Shaped headlight: teardrop along the forward dir. Bright/narrow\n"
    "            // near the lamp, widening and fading forward.\n"
    "            float L = sqrt(uLightMaxDist2[i]);          // forward reach\n"
    "            float along = delta.x*fwd.x + delta.z*fwd.y;        // forward distance\n"
    "            float side  = delta.z*fwd.x - delta.x*fwd.y;        // perpendicular (XZ)\n"
    "            if (along < -0.08*L) continue;              // cull behind the lamp\n"
    "            float t = clamp(along / L, 0.0, 1.0);               // 0 at lamp .. 1 at reach\n"
    "            float hw = L * (0.10 + 0.55*t);             // half-width grows forward\n"
    "            float perp = sqrt(side*side + delta.y*delta.y);\n"
    "            float radial = clamp(1.0 - perp/hw, 0.0, 1.0);\n"
    "            brightness = (1.0 - t) * radial * radial;          // concentrate near, fade out\n"
    "        } else {\n"
    "            float dist2 = delta.x*delta.x + delta.z*delta.z + delta.y*delta.y;\n"
    "            float nd = dist2 / uLightMaxDist2[i];               // 0..1 normalized dist²\n"
    "            if (nd >= 1.0) continue;\n"
    "            brightness = 1.0 - sqrt(nd);                        // radial falloff\n"
    "        }\n"
    "        if (r > 0.0)\n"
    "            light_col += uLightRgb[i] * brightness;           // additive accumulation\n"
    "        else\n"
    "            shadow += uLightRgb[i].x * brightness;            // anti-light: fake shadow\n"
    "    }\n"
    "    // Baked-shade strength: uAO in 0..1 blends it in; above 1 it becomes a\n"
    "    // power (darkening curve) so shadows can go deeper than the SW-linear look.\n"
    "    float ao = (uAO <= 1.0) ? mix(1.0, vAO, uAO) : pow(vAO, uAO);\n"
    "    // --- Sun base lighting ---\n"
    "    float base = uAmbient;\n"
    "    if (uSunEnable == 1) {\n"
    "        vec4 sc = uSunMVP * vec4(vWorldPos, 1.0);\n"
    "        vec3 p   = sc.xyz / sc.w * 0.5 + 0.5;\n"
    "        float lit = 1.0;\n"
    "        if (p.x >= 0.0 && p.x <= 1.0 &&\n"
    "            p.y >= 0.0 && p.y <= 1.0 &&\n"
    "            p.z >= 0.0 && p.z <= 1.0) {\n"
    "            float texel = 1.0 / 2048.0;\n"
    "            float halfK = float(uSunPCF);\n"
    "            float sigma = halfK * 0.5 + 1.0;\n"
    "            float sum_w = 0.0;\n"
    "            float sum_lit = 0.0;\n"
    "            for (int sx = -uSunPCF; sx <= uSunPCF; sx++) {\n"
    "                for (int sy = -uSunPCF; sy <= uSunPCF; sy++) {\n"
    "                    float dsq = float(sx*sx + sy*sy);\n"
    "                    float w = exp(-dsq / (2.0 * sigma * sigma));\n"
    "                    float closest = texture(uShadowMap,\n"
    "                        p.xy + vec2(float(sx), float(sy)) * texel).r;\n"
    "                    sum_lit += w * ((p.z - uSunBias > closest) ? 0.0 : 1.0);\n"
    "                    sum_w += w;\n"
    "                }\n"
    "            }\n"
    "            lit = sum_lit / sum_w;\n"
    "            lit = mix(lit, 1.0, uSunHaze);\n"
    "        }\n"
    "        if (uSunDebug == 1) { frag = vec4(vec3(lit), 1.0); return; }\n"
    "        base += uSunAmbient + uSunBright * lit;\n"
    "    }\n"
    "    // SW model: dynamic lamp light is ADDED on top of the baked shade\n"
    "    // (shpoint_compute_shade / quicklights), it is not modulated by it -\n"
    "    // so streetlight pools stay bright on baked-dark ground. Only the base\n"
    "    // (ambient) is scaled by the baked shade.\n"
    "    light_col = light_col * uGain * uTint + base * ao;\n"
    "    // SW caps the total shade at index 63 ~= 2x identity (0x7E00 clamp in\n"
    "    // shpoint_compute_shade) - stops lamp pools burning out to white.\n"
    "    light_col = min(light_col, vec3(1.97));\n"
    "    light_col *= clamp(1.0 - shadow, 0.0, 1.0);\n"
    "    light_col = max(light_col, 0.0);              // allow >1.0 for overbright glow\n"
    "    light_col = max(light_col, vec3(vEmissive)); // SW baked emissive (lit windows)\n"
    "    if (vUV.z > 254.5) {             // flat-shaded face (Texture==0), no texture\n"
    "        frag = vec4(vec3(0.15) * light_col, uAlpha);\n"
    "        return;\n"
    "    }\n"
    "    // Saturation-compensated shading: SW's palette fade rows keep dark\n"
    "    // colours hue-rich (hand-quantized dark palette entries), while a plain\n"
    "    // linear multiply reads washed-out/grey. Mimic the table smoothly:\n"
    "    // shade linearly, then boost saturation as the light level drops.\n"
    "    // uShadeSat = strength (0 = plain linear, ~0.6 = SW-like depth).\n"
    "    float lv = clamp(dot(light_col, vec3(0.299, 0.587, 0.114)), 0.0, 1.97);\n"
    "    float sboost = 1.0 + uShadeSat * clamp(1.0 - lv, 0.0, 1.0);\n"
    "    // Nearest: single texel with GL_NEAREST.\n"
    "    int idx = int(texture(uTex, vUV).r * 255.0 + 0.5);\n"
    "    if (uFilter == 1) {\n"
    "        // Palette-correct bilinear: sample 4 nearest integer texels via\n"
    "        // texelFetch (bypasses GL filtering), convert each to RGB through\n"
    "        // the palette, then bilinear blend in RGB space.  For keyed (cutout)\n"
    "        // textures (wire fence / scaffolding / grates) the key texel is\n"
    "        // excluded from BOTH the colour blend (no dark key-colour fringe) and\n"
    "        // a coverage value, so the cutout edge follows the smooth bilinear\n"
    "        // iso-line instead of the blocky texel grid.\n"
    "        int page = int(vUV.z);\n"
    "        vec2 tc = vUV.xy * 256.0 - 0.5;\n"
    "        ivec2 uv0 = ivec2(floor(tc));\n"
    "        vec2  f = fract(tc);\n"
    "        ivec2 uv1 = min(uv0 + 1, ivec2(255));\n"
    "        int i00 = int(texelFetch(uTex, ivec3(uv0.x, uv0.y, page), 0).r * 255.0 + 0.5);\n"
    "        int i10 = int(texelFetch(uTex, ivec3(uv1.x, uv0.y, page), 0).r * 255.0 + 0.5);\n"
    "        int i01 = int(texelFetch(uTex, ivec3(uv0.x, uv1.y, page), 0).r * 255.0 + 0.5);\n"
    "        int i11 = int(texelFetch(uTex, ivec3(uv1.x, uv1.y, page), 0).r * 255.0 + 0.5);\n"
    "        // Per-texel weight: 0 for the transparent key, else the bilinear weight.\n"
    "        float w00 = (1.0-f.x)*(1.0-f.y); float w10 = f.x*(1.0-f.y);\n"
    "        float w01 = (1.0-f.x)*f.y;       float w11 = f.x*f.y;\n"
    "        if (uTransKey >= 0) {\n"
    "            if (i00 == uTransKey) w00 = 0.0;\n"
    "            if (i10 == uTransKey) w10 = 0.0;\n"
    "            if (i01 == uTransKey) w01 = 0.0;\n"
    "            if (i11 == uTransKey) w11 = 0.0;\n"
    "        }\n"
    "        float cov = w00 + w10 + w01 + w11;\n"
    "        if (uTransKey >= 0 && cov < 0.5)\n"
    "            discard;\n"
    "        // Palette-correct bilinear blend, self-lit texels kept bright, then\n"
    "        // linear shade + saturation compensation.\n"
    "        vec3 c = (pal_lookup(i00)*w00 + pal_lookup(i10)*w10\n"
    "                + pal_lookup(i01)*w01 + pal_lookup(i11)*w11) / max(cov, 1e-4);\n"
    "        float sl = (selflit_lookup(i00)*w00 + selflit_lookup(i10)*w10\n"
    "                  + selflit_lookup(i01)*w01 + selflit_lookup(i11)*w11) / max(cov, 1e-4);\n"
    "        vec3 lin = c * max(light_col, vec3(sl));\n"
    "        float g = dot(lin, vec3(0.299, 0.587, 0.114));\n"
    "        frag = vec4(max(mix(vec3(g), lin, sboost), 0.0), uAlpha);\n"
    "    } else {\n"
    "        if (uTransKey >= 0 && idx == uTransKey)\n"
    "            discard;\n"
    "        vec3 c = pal_lookup(idx);\n"
    "        float sl = selflit_lookup(idx);\n"
    "        vec3 lin = c * max(light_col, vec3(sl));\n"
    "        float g = dot(lin, vec3(0.299, 0.587, 0.114));\n"
    "        frag = vec4(max(mix(vec3(g), lin, sboost), 0.0), uAlpha);\n"
    "    }\n"
    "}\n";

static GLuint fl_prog = 0;
static GLuint fl_vao = 0, fl_vbo = 0, fl_ebo = 0;
static GLuint fl_tex = 0, fl_pal = 0, fl_selflit = 0, fl_fade = 0;
static GLint  fl_loc_tex = -1, fl_loc_pal = -1, fl_loc_selflit = -1, fl_loc_transkey = -1;
static GLint  fl_loc_fadetab = -1;
static GLint  fl_loc_shadesat = -1;
static GLint  fl_loc_d10 = -1, fl_loc_d14 = -1, fl_loc_d18 = -1, fl_loc_d1c = -1;
static GLint  fl_loc_scale = -1, fl_loc_centre = -1, fl_loc_ctr = -1, fl_loc_persp = -1;
static GLint  fl_loc_lpos_base = -1;
static GLint  fl_loc_lrgb_base = -1;
static GLint  fl_loc_lrad_base = -1;
static GLint  fl_loc_nlights = -1;
static GLint  fl_loc_ambient = -1;
static GLint  fl_loc_gain = -1;
static GLint  fl_loc_tint = -1;
static GLint  fl_loc_ao        = -1;
static GLint  fl_loc_maxdist2  = -1;  /* array base, set per-light via glUniform1fv */
static GLint  fl_loc_lfwd      = -1;  /* shaped-headlight forward dirs (vec2[]) */
static GLint  fl_loc_shadowmap = -1;
static GLint  fl_loc_sun_mvp   = -1;
static GLint  fl_loc_sun_bright = -1;
static GLint  fl_loc_sun_ambient = -1;
static GLint  fl_loc_sun_bias  = -1;
static GLint  fl_loc_sun_enable = -1;
static GLint  fl_loc_sun_pcf   = -1;
static GLint  fl_loc_sun_debug = -1;
static GLint  fl_loc_sun_haze  = -1;
static GLint  fl_loc_filter   = -1;
static GLint  fl_loc_alpha    = -1;
static GLint  fl_loc_deepradar = -1;
static int    fl_filter = -1;            /* 0 = params applied, non-zero = need update */
static int    fl_ready = 0;
static int    fl_pages_uploaded = 0;

static GLuint fl_compile(GLenum type, const char *src)
{
    GLuint sh = glCreateShader(type);
    GLint ok = 0;
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(sh, sizeof(log), NULL, log);
        hwr_set_error("floor shader compile failed: %s", log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static int fl_init(void)
{
    GLuint vs, fs;
    GLint ok = 0;

    vs = fl_compile(GL_VERTEX_SHADER, floor_vert_src);
    if (!vs) return HWR_ERROR;
    fs = fl_compile(GL_FRAGMENT_SHADER, floor_frag_src);
    if (!fs) { glDeleteShader(vs); return HWR_ERROR; }
    fl_prog = glCreateProgram();
    glAttachShader(fl_prog, vs);
    glAttachShader(fl_prog, fs);
    glLinkProgram(fl_prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    glGetProgramiv(fl_prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(fl_prog, sizeof(log), NULL, log);
        hwr_set_error("floor program link failed: %s", log);
        return HWR_ERROR;
    }
    fl_loc_tex    = glGetUniformLocation(fl_prog, "uTex");
    fl_loc_pal    = glGetUniformLocation(fl_prog, "uPalette");
    fl_loc_selflit = glGetUniformLocation(fl_prog, "uSelfLit");
    fl_loc_fadetab = glGetUniformLocation(fl_prog, "uFadeTab");
    fl_loc_shadesat = glGetUniformLocation(fl_prog, "uShadeSat");
    fl_loc_transkey = glGetUniformLocation(fl_prog, "uTransKey");
    fl_loc_d10    = glGetUniformLocation(fl_prog, "uD10");
    fl_loc_d14    = glGetUniformLocation(fl_prog, "uD14");
    fl_loc_d18    = glGetUniformLocation(fl_prog, "uD18");
    fl_loc_d1c    = glGetUniformLocation(fl_prog, "uD1C");
    fl_loc_scale  = glGetUniformLocation(fl_prog, "uScale");
    fl_loc_centre = glGetUniformLocation(fl_prog, "uCentre");
    fl_loc_ctr    = glGetUniformLocation(fl_prog, "uCtr");
    fl_loc_persp  = glGetUniformLocation(fl_prog, "uPersp");
    fl_loc_lpos_base = glGetUniformLocation(fl_prog, "uLightPos");
    fl_loc_lrgb_base = glGetUniformLocation(fl_prog, "uLightRgb");
    fl_loc_lrad_base = glGetUniformLocation(fl_prog, "uLightRadius");
    fl_loc_nlights   = glGetUniformLocation(fl_prog, "uNumLights");
    fl_loc_ambient   = glGetUniformLocation(fl_prog, "uAmbient");
    fl_loc_gain      = glGetUniformLocation(fl_prog, "uGain");
    fl_loc_tint      = glGetUniformLocation(fl_prog, "uTint");
    fl_loc_ao        = glGetUniformLocation(fl_prog, "uAO");
    fl_loc_maxdist2  = glGetUniformLocation(fl_prog, "uLightMaxDist2");
    fl_loc_lfwd      = glGetUniformLocation(fl_prog, "uLightFwd");
    fl_loc_shadowmap  = glGetUniformLocation(fl_prog, "uShadowMap");
    fl_loc_sun_mvp    = glGetUniformLocation(fl_prog, "uSunMVP");
    fl_loc_sun_bright = glGetUniformLocation(fl_prog, "uSunBright");
    fl_loc_sun_ambient= glGetUniformLocation(fl_prog, "uSunAmbient");
    fl_loc_sun_bias   = glGetUniformLocation(fl_prog, "uSunBias");
    fl_loc_sun_enable = glGetUniformLocation(fl_prog, "uSunEnable");
    fl_loc_sun_pcf    = glGetUniformLocation(fl_prog, "uSunPCF");
    fl_loc_sun_debug  = glGetUniformLocation(fl_prog, "uSunDebug");
    fl_loc_sun_haze   = glGetUniformLocation(fl_prog, "uSunHaze");
    fl_loc_filter     = glGetUniformLocation(fl_prog, "uFilter");
    fl_loc_alpha      = glGetUniformLocation(fl_prog, "uAlpha");
    fl_loc_deepradar  = glGetUniformLocation(fl_prog, "uDeepRadarIdx");

    glGenVertexArrays(1, &fl_vao);
    glBindVertexArray(fl_vao);
    glGenBuffers(1, &fl_vbo);
    glGenBuffers(1, &fl_ebo);
    glBindBuffer(GL_ARRAY_BUFFER, fl_vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, fl_ebo);
    {
        GLsizei stride = (GLsizei)sizeof(HwrVertex);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride,
            (void *)offsetof(HwrVertex, x));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride,
            (void *)offsetof(HwrVertex, u));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, stride,
            (void *)offsetof(HwrVertex, tile_depth));
        glEnableVertexAttribArray(3);
        glVertexAttribIPointer(3, 1, GL_UNSIGNED_BYTE, stride,
            (void *)offsetof(HwrVertex, page));
        /* SW baked shade -> normalised 0..1 float (location 4, aLight/AO). */
        glEnableVertexAttribArray(4);
        glVertexAttribPointer(4, 1, GL_UNSIGNED_BYTE, GL_TRUE, stride,
            (void *)offsetof(HwrVertex, light));
        /* SW baked emissive -> normalised 0..1 float (location 5, aEmissive). */
        glEnableVertexAttribArray(5);
        glVertexAttribPointer(5, 1, GL_UNSIGNED_BYTE, GL_TRUE, stride,
            (void *)offsetof(HwrVertex, emissive));
    }
    glBindVertexArray(0);

    glGenTextures(1, &fl_tex);
    glGenTextures(1, &fl_pal);
    glBindTexture(GL_TEXTURE_2D, fl_pal);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenTextures(1, &fl_fade);
    glBindTexture(GL_TEXTURE_2D, fl_fade);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenTextures(1, &fl_selflit);
    glBindTexture(GL_TEXTURE_2D, fl_selflit);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    {
        unsigned char mask[256];
        int i;
        memset(mask, 0, sizeof(mask));
        for (i = 0; fade_unaffected_colours[i] != 0; i++)
            mask[fade_unaffected_colours[i]] = 255;
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, 256, 1, 0, GL_RED,
            GL_UNSIGNED_BYTE, mask);
    }

    if (hwr_gl_check("fl_init"))
        return HWR_ERROR;
    fl_ready = 1;
    return HWR_OK;
}

static void fl_upload_pages(const HwrTexturePages *pg, int filter_linear)
{
    /* The pages are GL_R8 *palette indices*, not colours. GL_LINEAR would
     * interpolate the indices themselves (index 20 + 200 -> 110), and
     * palette[110] is an unrelated colour - that is the sparkle/marbling. So
     * the indexed texture must always be NEAREST; smooth filtering, if wanted,
     * has to be done after the palette lookup (palette-correct bilinear). */
    GLint filt = GL_NEAREST;
    (void)filter_linear;
    glBindTexture(GL_TEXTURE_2D_ARRAY, fl_tex);
    if (!fl_pages_uploaded && pg != NULL && pg->texels != NULL) {
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_R8, pg->width, pg->height,
            pg->count, 0, GL_RED, GL_UNSIGNED_BYTE, pg->texels);
        fl_pages_uploaded = 1;
    } else if (fl_pages_uploaded && pg != NULL && pg->texels != NULL) {
        /* Pages 4/5 hold FLIC-animated content (billboards/equipment/cyborg
         * playback) that source_sw.c re-decodes into the source buffer every
         * frame; page 0 gets rain ripple/splash pixels painted into it by
         * water_droplets_on_floor while it's raining. The rest of the array
         * is static art uploaded once above. */
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        if (0 < pg->count)
            glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0,
                pg->width, pg->height, 1, GL_RED, GL_UNSIGNED_BYTE,
                pg->texels);
        if (4 < pg->count)
            glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 4,
                pg->width, pg->height, 1, GL_RED, GL_UNSIGNED_BYTE,
                pg->texels + (size_t)4 * pg->width * pg->height);
        if (5 < pg->count)
            glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 5,
                pg->width, pg->height, 1, GL_RED, GL_UNSIGNED_BYTE,
                pg->texels + (size_t)5 * pg->width * pg->height);
    }
    if (fl_filter != 0) {
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, filt);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, filt);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        fl_filter = 0;
    }
}

/* Upload point light uniforms. Call after fl_setup_program (program must be bound). */
static void fl_upload_lights(const HwrLight *lights, int n)
{
    float pos_buf[HWR_MAX_LIGHTS * 3];
    float rgb_buf[HWR_MAX_LIGHTS * 3];
    float rad_buf[HWR_MAX_LIGHTS];
    float maxd2_buf[HWR_MAX_LIGHTS];
    float fwd_buf[HWR_MAX_LIGHTS * 2];
    int i;
    if (n > HWR_MAX_LIGHTS) n = HWR_MAX_LIGHTS;
    {
        HwrLightDefaults d = hwr_lights_defaults();
        float global_base = d.max_light_dist2;
        for (i = 0; i < n; i++) {
            pos_buf[i*3+0] = lights[i].x;
            pos_buf[i*3+1] = lights[i].y;
            pos_buf[i*3+2] = lights[i].z;
            rgb_buf[i*3+0] = lights[i].r;
            rgb_buf[i*3+1] = lights[i].g;
            rgb_buf[i*3+2] = lights[i].b;
            rad_buf[i]     = lights[i].radius;
            fwd_buf[i*2+0] = lights[i].fdx;
            fwd_buf[i*2+1] = lights[i].fdz;
            /* Per-light distance cull: use per-light max_dist2 if set by source,
             * otherwise global_base + Y² (Y² so elevated lights reach the ground). */
            {
                if (lights[i].max_dist2 > 0.0f) {
                    maxd2_buf[i] = lights[i].max_dist2;
                } else {
                    float yabs = (lights[i].y < 0.0f) ? -lights[i].y : lights[i].y;
                    maxd2_buf[i] = global_base + yabs * yabs;
                }
            }
        }
        glUniform1f(fl_loc_ambient, d.ambient);
        glUniform1f(fl_loc_gain, d.intensity);
        glUniform3f(fl_loc_tint, d.tint_r, d.tint_g, d.tint_b);
        glUniform1f(fl_loc_ao, d.ao);
    }
    if (n > 0) {
        glUniform3fv(fl_loc_lpos_base, n, pos_buf);
        glUniform3fv(fl_loc_lrgb_base, n, rgb_buf);
        glUniform1fv(fl_loc_lrad_base, n, rad_buf);
        glUniform1fv(fl_loc_maxdist2, n, maxd2_buf);
        glUniform2fv(fl_loc_lfwd, n, fwd_buf);
    }
    glUniform1i(fl_loc_nlights, n);
}

/* Bind the floor program, set the camera uniforms and the indexed-texture +
 * palette samplers. Shared by the floor and face passes (both use the same
 * transform_shpoint projection and texture pages). */
static void fl_setup_program(const HwrCamera *cam, const unsigned char *pal8,
    int trans_key)
{
    if (pal8 != NULL) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, fl_pal);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, 256, 1, 0, GL_RGB,
            GL_UNSIGNED_BYTE, pal8);
        /* SW fade table (64 rows x 256 indices; regenerated with the palette).
         * Re-uploaded with it so shader colour shading always matches. */
        glActiveTexture(GL_TEXTURE5);
        glBindTexture(GL_TEXTURE_2D, fl_fade);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, 256, 64, 0, GL_RED,
            GL_UNSIGNED_BYTE, pixmap);
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, fl_tex);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, fl_selflit);
    glActiveTexture(GL_TEXTURE5);
    glBindTexture(GL_TEXTURE_2D, fl_fade);
    glActiveTexture(GL_TEXTURE0);   /* restore default active unit */

    glEnable(GL_DEPTH_TEST);
    glUseProgram(fl_prog);
    glUniform1f(fl_loc_d10, cam->d10);
    glUniform1f(fl_loc_d14, cam->d14);
    glUniform1f(fl_loc_d18, cam->d18);
    glUniform1f(fl_loc_d1c, cam->d1c);
    glUniform1f(fl_loc_scale, cam->scale);
    glUniform2f(fl_loc_centre, cam->centre_x, cam->centre_y);
    glUniform3f(fl_loc_ctr, cam->cx, cam->cy8, cam->cz);
    glUniform1i(fl_loc_persp, cam->perspective);
    glUniform1i(fl_loc_tex, 0);
    glUniform1i(fl_loc_pal, 1);
    glUniform1i(fl_loc_selflit, 2);
    glUniform1i(fl_loc_fadetab, 5);
    glUniform1f(fl_loc_shadesat, hwr_lights_defaults().shade_sat);
    glUniform1i(fl_loc_transkey, trans_key);
    glUniform1f(fl_loc_alpha, 1.0f);    /* opaque by default; transparent pass overrides */

    /* Sun shadow map on texture unit 3. */
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, (GLuint)hwr_sun_texture());
    glActiveTexture(GL_TEXTURE0);   /* restore default active unit */
    glUniform1i(fl_loc_shadowmap, 3);
    {
        int sun_on = hwr_sun_enabled();
        glUniform1i(fl_loc_sun_enable, sun_on);
        if (sun_on) {
            glUniformMatrix4fv(fl_loc_sun_mvp, 1, GL_FALSE, hwr_sun_mvp());
            glUniform1f(fl_loc_sun_bright,  hwr_sun_bright());
            glUniform1f(fl_loc_sun_ambient, hwr_sun_ambient());
            glUniform1f(fl_loc_sun_bias,    hwr_sun_bias());
            glUniform1i(fl_loc_sun_pcf,     hwr_sun_pcf());
            glUniform1i(fl_loc_sun_debug,   hwr_sun_debug());
            glUniform1f(fl_loc_sun_haze,    hwr_sun_haze());
        }
    }

    /* World-space view direction for SSAO normal orientation. scrd (view depth)
     * is linear in world position: scrd ~ d1c*d10*dx + d18*65536*dy + d1c*d14*dz,
     * so its gradient is the into-screen (+depth) direction. */
    hwr_ssao_set_viewdir(cam->d1c * cam->d10,
                         cam->d18 * 65536.0f,
                         cam->d1c * cam->d14);
}

/* Stream one indexed geometry batch through the shared VBO/EBO and draw it.
 * The program, uniforms and textures must already be bound (fl_setup_program).
 * Draws are sequential so reusing the buffers between batches is safe. */
static void fl_draw_batch(const HwrGeometryBatch *batch)
{
    glBindVertexArray(fl_vao);
    glBindBuffer(GL_ARRAY_BUFFER, fl_vbo);
    glBufferData(GL_ARRAY_BUFFER,
        (GLsizeiptr)batch->vert_count * sizeof(HwrVertex), batch->verts,
        GL_STREAM_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, fl_ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
        (GLsizeiptr)batch->index_count * sizeof(uint32_t), batch->indices,
        GL_STREAM_DRAW);
    glDrawElements(GL_TRIANGLES, batch->index_count, GL_UNSIGNED_INT, (void *)0);
    glBindVertexArray(0);
}

/** Render the floor for this frame. pal8 is the active 256*3 8-bit palette;
 *  filter_linear selects smooth vs crisp. Pulls geometry/camera/pages from the
 *  bound scene source. Returns nonzero if anything was drawn. */
int hwr_floor_render(const unsigned char *pal8, int filter_linear)
{
    HwrCamera cam;
    HwrGeometryBatch batch;
    HwrTexturePages pages;
    const HwrSceneSource *s = hwr_source;

    if (!hwr_is_ready() || s == NULL)
        return 0;
    if (s->get_camera == NULL || s->get_floor == NULL)
        return 0;
    if (!fl_ready && fl_init() != HWR_OK)
        return 0;

    {
        int cam_ok = (s->get_camera(s->ctx, &cam) == 0);
        int idx = s->get_floor(s->ctx, &batch);
        int tex_ok = (s->get_texture_pages != NULL &&
                      s->get_texture_pages(s->ctx, &pages) == 0);
        if (!cam_ok || idx <= 0 || batch.index_count <= 0 || !tex_ok)
            return 0;
    }

    fl_upload_pages(&pages, filter_linear);
    fl_setup_program(&cam, pal8, -1);   /* floor tiles are fully opaque */
    glUniform1i(fl_loc_filter, filter_linear);
    {
        /* The floor's per-vertex shade (vAO) is the COMPLETE static SW light
         * (Ambient + every map lamp + anti-light shadows, from the engine's
         * per-tile quicklight data - see corner_baked_shade). So:
         *  - upload only DYNAMIC lights (headlights/fires); static ones are
         *    already inside the vertex shade - uploading them double-counts;
         *  - reconstruct SW's absolute level as base*ao = (2*ambient)*vAO:
         *    vertex byte 128 = SW identity -> 1.0 at ambient=1.0, up to ~2x
         *    overbright. ambient stays as a master brightness scale. */
        HwrLight lights[HWR_MAX_LIGHTS];
        int nlight = (s->get_lights != NULL)
            ? s->get_lights(s->ctx, lights, HWR_MAX_LIGHTS) : 0;
        int i, nd = 0;
        for (i = 0; i < nlight; i++)
            if (lights[i].dynamic)
                lights[nd++] = lights[i];
        fl_upload_lights(lights, nd);
        {
            HwrLightDefaults d = hwr_lights_defaults();
            glUniform1f(fl_loc_ambient, 2.0f * d.ambient);
            glUniform1f(fl_loc_ao, 1.0f);   /* linear vAO: the SW value as-is */
        }
    }
    fl_draw_batch(&batch);

    hwr_gl_check("hwr_floor_render");
    return 1;
}

/** Render object/building faces for this frame (Phase 4). Reuses the floor
 *  program, texture pages and palette; pulls face geometry from the bound
 *  source's get_faces. Assumes the texture pages are already uploaded (call
 *  after hwr_floor_render). Returns nonzero if anything was drawn. */
int hwr_faces_render(const unsigned char *pal8, int filter_linear)
{
    HwrCamera cam;
    HwrGeometryBatch batch;
    const HwrSceneSource *s = hwr_source;

    if (!hwr_is_ready() || s == NULL || s->get_faces == NULL)
        return 0;
    if (s->get_camera == NULL || !fl_ready)
        return 0;
    if (s->get_camera(s->ctx, &cam) != 0)
        return 0;
    if (s->get_faces(s->ctx, &batch) <= 0 || batch.index_count <= 0)
        return 0;

    /* Faces are double-sided (cull stays disabled): through a window you see the
     * building's back wall. Index 0 is the texture transparent key (windows /
     * grates), so discard it to let those back faces show through. */
    fl_setup_program(&cam, pal8, 0);
    glUniform1i(fl_loc_filter, filter_linear);
    {
        /* SW-exact face lighting, mirroring the floor pass: each face vertex
         * carries the complete static SW shade (Shade0..3 + its Light0..3
         * quicklight chains = every map lamp and anti-light shadow at SW
         * falloff), on the unified scale (byte 128 = identity). So upload only
         * DYNAMIC lights (headlights/fires) - static ones are inside the vertex
         * data - and reconstruct the absolute level as (2*ambient)*vAO. */
        HwrLight lights[HWR_MAX_LIGHTS];
        int nlight = (s->get_lights != NULL)
            ? s->get_lights(s->ctx, lights, HWR_MAX_LIGHTS) : 0;
        int i, nd = 0;
        for (i = 0; i < nlight; i++)
            if (lights[i].dynamic)
                lights[nd++] = lights[i];
        fl_upload_lights(lights, nd);
        {
            HwrLightDefaults d = hwr_lights_defaults();
            glUniform1f(fl_loc_ambient, 2.0f * d.ambient);
            glUniform1f(fl_loc_ao, 1.0f);   /* linear vAO: the SW value as-is */
        }
    }
    fl_draw_batch(&batch);

    hwr_gl_check("hwr_faces_render");
    return 1;
}

/* ---- Transparent (blended) face pass config (Phase 8) ------------------- */
static int   tr_enable = 1;
static float tr_alpha  = 0.5f;   /* fragment alpha for blended faces */
static int   tr_deepradar_idx = 216; /* palette index for the deep-radar tint (0xd8) */

void hwr_transparent_config(int enable, float alpha, int deepradar_idx)
{
    tr_enable = enable;
    if (alpha < 0.0f) alpha = 0.0f;
    if (alpha > 1.0f) alpha = 1.0f;
    tr_alpha = alpha;
    if (deepradar_idx >= 0 && deepradar_idx < 256)
        tr_deepradar_idx = deepradar_idx;
}

/** Render semi-transparent object/building faces (Phase 8): deep-radar
 *  see-through buildings and static glass/fence faces, pulled from the bound
 *  source's get_transparent_faces. Reuses the floor/face program and texture
 *  pages (call after hwr_faces_render so the pages are uploaded). Draws with
 *  alpha blending, testing against the opaque depth but not writing it, and the
 *  batch is pre-sorted back-to-front by the source. Returns nonzero if drawn. */
int hwr_transparent_render(const unsigned char *pal8, int filter_linear)
{
    HwrCamera cam;
    HwrGeometryBatch batch;
    const HwrSceneSource *s = hwr_source;

    if (!tr_enable)
        return 0;
    if (!hwr_is_ready() || s == NULL || s->get_transparent_faces == NULL)
        return 0;
    if (s->get_camera == NULL || !fl_ready)
        return 0;
    if (s->get_camera(s->ctx, &cam) != 0)
        return 0;
    if (s->get_transparent_faces(s->ctx, &batch) <= 0 || batch.index_count <= 0)
        return 0;

    /* Same projection/texture/lighting as opaque faces; index 0 stays the
     * texture transparent key (windows / grates). */
    fl_setup_program(&cam, pal8, 0);
    glUniform1i(fl_loc_filter, filter_linear);
    glUniform1f(fl_loc_alpha, tr_alpha);
    glUniform1i(fl_loc_deepradar, tr_deepradar_idx);
    {
        HwrLight lights[HWR_MAX_LIGHTS];
        int nlight = (s->get_lights != NULL)
            ? s->get_lights(s->ctx, lights, HWR_MAX_LIGHTS) : 0;
        fl_upload_lights(lights, nlight < 0 ? 0 : nlight);
    }

    /* Blend over the opaque scene; test depth but don't write it (so blended
     * faces don't occlude each other in the z-buffer; correctness comes from
     * the source's back-to-front triangle sort). */
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    fl_draw_batch(&batch);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);

    hwr_gl_check("hwr_transparent_render");
    return 1;
}

/* ===================================================================== */
/* Reflective "chameleon" (spectraflair) paint pass.                      */
/* Reuses the floor projection in the vertex shader, but shades the paint */
/* procedurally: a view-angle hue shift across the base colour plus a     */
/* faked fresnel sheen. No texture pages, no scene lighting (matches the  */
/* SW mode-27 "textured, no shading" reflective look, modernised).        */
/* ===================================================================== */

static const char *refl_vert_src =
    "#version 330 core\n"
    "layout(location=0) in vec3 aPos;\n"
    "layout(location=1) in vec3 aNormal;\n"
    "layout(location=2) in float aDepth;\n"
    "layout(location=3) in float aBase;\n"
    "uniform float uD10, uD14, uD18, uD1C;\n"
    "uniform float uScale;\n"
    "uniform vec2 uCentre;\n"
    "uniform vec3 uCtr;\n"
    "uniform int  uPersp;\n"
    "out vec3 vN;\n"
    "out float vBase;\n"
    "out vec3 vWorldPos;\n"
    "void main(){\n"
    "    float dx = aPos.x - uCtr.x;\n"
    "    float dy = aPos.y - uCtr.y;\n"
    "    float dz = aPos.z - uCtr.z;\n"
    "    float fa = (uD14*dx - uD10*dz) / 65536.0;\n"
    "    float fb = (uD10*dx + uD14*dz) / 65536.0;\n"
    "    float fc = (uD1C*dy - uD18*fb) / 65536.0;\n"
    "    float scrd = (uD18*dy + uD1C*fb) / 65536.0;\n"
    "    if (uPersp == 5 && scrd > 1024.0)\n"
    "        scrd = 16384.0*scrd/(scrd + 16384.0);\n"
    "    float shx = uScale*fa / 2048.0;\n"
    "    float shy = uScale*fc / 2048.0;\n"
    "    if (uPersp == 5) {\n"
    "        shx = shx*(16384.0 - scrd) / 16384.0;\n"
    "        shy = shy*(16384.0 - scrd) / 16384.0;\n"
    "    }\n"
    "    float sx = uCentre.x + shx;\n"
    "    float sy = uCentre.y - shy;\n"
    "    vN = aNormal;\n"
    "    vBase = aBase;\n"
    "    vWorldPos = aPos;\n"
    "    float ndc_z = clamp(aDepth / 16384.0, -1.0, 1.0);\n"
    "    gl_Position = vec4(sx/uCentre.x - 1.0, 1.0 - sy/uCentre.y, ndc_z, 1.0);\n"
    "}\n";

static const char *refl_frag_src =
    "#version 330 core\n"
    "in vec3 vN;\n"
    "in float vBase;\n"
    "in vec3 vWorldPos;\n"
    "layout(location=0) out vec4 frag;\n"
    "layout(location=1) out vec4 fragPos;\n"
    "uniform sampler2D uPalette;\n"
    "uniform float uD10, uD14, uD18, uD1C;\n"
    "uniform float uHueBase;     // start hue (0..1): 0=red .33=green .66=blue .8=purple\n"
    "uniform float uHueSpan;     // hue wheel fraction swept across the surface\n"
    "uniform float uSat;         // colour saturation (deep = high)\n"
    "uniform float uPaintLevel;  // base paint brightness (keep < 1, it's metallic)\n"
    "uniform float uStreakFreq;  // number of streak bands across the sweep\n"
    "uniform float uStreakSharp; // streak thinness (higher = thinner, sharper)\n"
    "uniform float uSheen;       // streak highlight strength\n"
    "uniform vec3  uTintHi;      // streak highlight colour\n"
    "// --- scene lighting (mirrors the floor/face shader) so paint darkens in\n"
    "//     unlit/shadowed areas instead of glowing at constant brightness ---\n"
    "uniform vec3  uLightPos[64];\n"
    "uniform vec3  uLightRgb[64];\n"
    "uniform float uLightRadius[64];\n"
    "uniform float uLightMaxDist2[64];\n"
    "uniform int   uNumLights;\n"
    "uniform float uAmbient;\n"
    "uniform float uGain;\n"
    "uniform vec3  uTint;\n"
    "uniform sampler2D uShadowMap;\n"
    "uniform mat4  uSunMVP;\n"
    "uniform float uSunBright;\n"
    "uniform float uSunAmbient;\n"
    "uniform float uSunBias;\n"
    "uniform int   uSunEnable;\n"
    "uniform int   uSunPCF;\n"
    "uniform float uSunHaze;\n"
    "vec3 pal_lookup(int idx){\n"
    "    return texture(uPalette, vec2((float(idx)+0.5)/256.0, 0.5)).rgb;\n"
    "}\n"
    "vec3 rgb2hsv(vec3 c){\n"
    "    vec4 K = vec4(0.0, -1.0/3.0, 2.0/3.0, -1.0);\n"
    "    vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));\n"
    "    vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));\n"
    "    float d = q.x - min(q.w, q.y);\n"
    "    float e = 1.0e-10;\n"
    "    return vec3(abs(q.z + (q.w - q.y)/(6.0*d + e)), d/(q.x + e), q.x);\n"
    "}\n"
    "vec3 hsv2rgb(vec3 c){\n"
    "    vec3 p = abs(fract(c.xxx + vec3(0.0,2.0/3.0,1.0/3.0))*6.0 - 3.0);\n"
    "    return c.z * mix(vec3(1.0), clamp(p - 1.0, 0.0, 1.0), c.y);\n"
    "}\n"
    "void main(){\n"
    "    fragPos = vec4(0.0);\n"
    "    // Smoothly-interpolated per-vertex normal -> shade curves across faces.\n"
    "    vec3 N = normalize(vN);\n"
    "    // View-angle sweep, same axes as the SW matcap (camera sin/cos).\n"
    "    float a  = clamp((uD14*N.x - uD10*N.z) / 65536.0, -1.0, 1.0);\n"
    "    float fb = (uD10*N.x + uD14*N.z) / 65536.0;\n"
    "    float b  = clamp((uD1C*N.y - uD18*fb) / 65536.0, -1.0, 1.0);\n"
    "    // Chameleon: hue sweeps a wide band (green->blue->purple) with the view\n"
    "    // angle, deeply saturated and independent of the base paint colour, so\n"
    "    // the full spectraflair rainbow shows as the surface curves/camera turns.\n"
    "    float sweep = (a*0.5 + 0.5) + 0.25*b;     // 2D-ish view-angle sweep\n"
    "    float hue = fract(uHueBase + uHueSpan * sweep);\n"
    "    // Near-black metallic body: uPaintLevel keeps the flat base very dark.\n"
    "    vec3 paint = hsv2rgb(vec3(hue, uSat, uPaintLevel));\n"
    "    // Full-value chameleon colour used for the REFLECTIONS, so the multicolour\n"
    "    // iridescence still shows vividly even though the body is mostly black.\n"
    "    vec3 refl  = hsv2rgb(vec3(hue, uSat, 1.0));\n"
    "    // Streaks: thin anisotropic highlight bands that sweep as the angle\n"
    "    // changes. cos() of the vertical sweep gives smooth repeating lines; the\n"
    "    // high power thins them into streaks. The interpolated normal keeps them\n"
    "    // continuous (curving) across adjacent polygons.\n"
    "    float bands  = 0.5 + 0.5*cos(b*uStreakFreq*6.2831853 + a*2.5);\n"
    "    float streak = pow(bands, uStreakSharp);\n"
    "    float edge   = clamp(length(vec2(a, b)), 0.0, 1.0);  // brighter at grazing\n"
    "    float fres   = pow(edge, 3.0);                       // grazing-angle rim\n"
    "    // Mostly-black base + multicolour reflective streaks + rainbow rim + a\n"
    "    // touch of white sparkle highlight.\n"
    "    vec3 col = paint\n"
    "             + refl * streak * uSheen\n"
    "             + refl * fres * 0.35\n"
    "             + uTintHi * streak * edge * uSheen * 0.4;\n"
    "    // --- scene lighting factor (point lights + sun + ambient), same as the\n"
    "    //     floor, so painted panels go dark in shadow / unlit interiors ---\n"
    "    vec3 light_col = vec3(0.0);\n"
    "    float shadow = 0.0;\n"
    "    for (int i = 0; i < uNumLights; i++) {\n"
    "        float r = uLightRadius[i];\n"
    "        if (r == 0.0) continue;\n"
    "        vec3 delta = vWorldPos - uLightPos[i];\n"
    "        float dist2 = dot(delta, delta);\n"
    "        float nd = dist2 / uLightMaxDist2[i];\n"
    "        if (nd >= 1.0) continue;\n"
    "        float brightness = 1.0 - sqrt(nd);\n"
    "        if (r > 0.0) light_col += uLightRgb[i] * brightness;\n"
    "        else         shadow    += uLightRgb[i].x * brightness;\n"
    "    }\n"
    "    float lbase = uAmbient;\n"
    "    if (uSunEnable == 1) {\n"
    "        vec4 sc = uSunMVP * vec4(vWorldPos, 1.0);\n"
    "        vec3 p = sc.xyz / sc.w * 0.5 + 0.5;\n"
    "        float lit = 1.0;\n"
    "        if (p.x>=0.0&&p.x<=1.0&&p.y>=0.0&&p.y<=1.0&&p.z>=0.0&&p.z<=1.0) {\n"
    "            float texel = 1.0/2048.0; float sigma = float(uSunPCF)*0.5+1.0;\n"
    "            float sw=0.0, sl=0.0;\n"
    "            for (int sx=-uSunPCF; sx<=uSunPCF; sx++)\n"
    "            for (int sy=-uSunPCF; sy<=uSunPCF; sy++) {\n"
    "                float w = exp(-float(sx*sx+sy*sy)/(2.0*sigma*sigma));\n"
    "                float closest = texture(uShadowMap, p.xy+vec2(float(sx),float(sy))*texel).r;\n"
    "                sl += w * ((p.z-uSunBias > closest) ? 0.0 : 1.0); sw += w;\n"
    "            }\n"
    "            lit = mix(sl/sw, 1.0, uSunHaze);\n"
    "        }\n"
    "        lbase += uSunAmbient + uSunBright * lit;\n"
    "    }\n"
    "    light_col = light_col * uGain * uTint + lbase;\n"
    "    light_col *= clamp(1.0 - shadow, 0.0, 1.0);\n"
    "    light_col = max(light_col, 0.0);\n"
    "    frag = vec4(col * light_col, 1.0);\n"
    "}\n";

static GLuint rf_prog = 0, rf_vao = 0, rf_vbo = 0, rf_ebo = 0;
static GLint  rf_l_d10=-1, rf_l_d14=-1, rf_l_d18=-1, rf_l_d1c=-1;
static GLint  rf_l_scale=-1, rf_l_centre=-1, rf_l_ctr=-1, rf_l_persp=-1;
static GLint  rf_l_pal=-1, rf_l_huebase=-1, rf_l_huespan=-1, rf_l_sat=-1, rf_l_paintlevel=-1;
static GLint  rf_l_streakfreq=-1, rf_l_streaksharp=-1, rf_l_sheen=-1, rf_l_tinthi=-1;
/* Lighting uniforms (mirror the floor program). */
static GLint  rf_l_lpos=-1, rf_l_lrgb=-1, rf_l_lrad=-1, rf_l_lmaxd2=-1, rf_l_nlights=-1;
static GLint  rf_l_ambient=-1, rf_l_gain=-1, rf_l_tint=-1;
static GLint  rf_l_shadowmap=-1, rf_l_sunmvp=-1, rf_l_sunbright=-1, rf_l_sunambient=-1;
static GLint  rf_l_sunbias=-1, rf_l_sunenable=-1, rf_l_sunpcf=-1, rf_l_sunhaze=-1;
static int    rf_ready = 0;

static int rf_init(void)
{
    GLuint vs, fs;
    GLint ok = 0;
    vs = fl_compile(GL_VERTEX_SHADER, refl_vert_src);
    if (!vs) return HWR_ERROR;
    fs = fl_compile(GL_FRAGMENT_SHADER, refl_frag_src);
    if (!fs) { glDeleteShader(vs); return HWR_ERROR; }
    rf_prog = glCreateProgram();
    glAttachShader(rf_prog, vs);
    glAttachShader(rf_prog, fs);
    glLinkProgram(rf_prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    glGetProgramiv(rf_prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(rf_prog, sizeof(log), NULL, log);
        hwr_set_error("reflect program link failed: %s", log);
        return HWR_ERROR;
    }
    rf_l_d10=glGetUniformLocation(rf_prog,"uD10");
    rf_l_d14=glGetUniformLocation(rf_prog,"uD14");
    rf_l_d18=glGetUniformLocation(rf_prog,"uD18");
    rf_l_d1c=glGetUniformLocation(rf_prog,"uD1C");
    rf_l_scale=glGetUniformLocation(rf_prog,"uScale");
    rf_l_centre=glGetUniformLocation(rf_prog,"uCentre");
    rf_l_ctr=glGetUniformLocation(rf_prog,"uCtr");
    rf_l_persp=glGetUniformLocation(rf_prog,"uPersp");
    rf_l_pal=glGetUniformLocation(rf_prog,"uPalette");
    rf_l_huebase=glGetUniformLocation(rf_prog,"uHueBase");
    rf_l_huespan=glGetUniformLocation(rf_prog,"uHueSpan");
    rf_l_sat=glGetUniformLocation(rf_prog,"uSat");
    rf_l_paintlevel=glGetUniformLocation(rf_prog,"uPaintLevel");
    rf_l_streakfreq=glGetUniformLocation(rf_prog,"uStreakFreq");
    rf_l_streaksharp=glGetUniformLocation(rf_prog,"uStreakSharp");
    rf_l_sheen=glGetUniformLocation(rf_prog,"uSheen");
    rf_l_tinthi=glGetUniformLocation(rf_prog,"uTintHi");
    rf_l_lpos=glGetUniformLocation(rf_prog,"uLightPos");
    rf_l_lrgb=glGetUniformLocation(rf_prog,"uLightRgb");
    rf_l_lrad=glGetUniformLocation(rf_prog,"uLightRadius");
    rf_l_lmaxd2=glGetUniformLocation(rf_prog,"uLightMaxDist2");
    rf_l_nlights=glGetUniformLocation(rf_prog,"uNumLights");
    rf_l_ambient=glGetUniformLocation(rf_prog,"uAmbient");
    rf_l_gain=glGetUniformLocation(rf_prog,"uGain");
    rf_l_tint=glGetUniformLocation(rf_prog,"uTint");
    rf_l_shadowmap=glGetUniformLocation(rf_prog,"uShadowMap");
    rf_l_sunmvp=glGetUniformLocation(rf_prog,"uSunMVP");
    rf_l_sunbright=glGetUniformLocation(rf_prog,"uSunBright");
    rf_l_sunambient=glGetUniformLocation(rf_prog,"uSunAmbient");
    rf_l_sunbias=glGetUniformLocation(rf_prog,"uSunBias");
    rf_l_sunenable=glGetUniformLocation(rf_prog,"uSunEnable");
    rf_l_sunpcf=glGetUniformLocation(rf_prog,"uSunPCF");
    rf_l_sunhaze=glGetUniformLocation(rf_prog,"uSunHaze");

    glGenVertexArrays(1, &rf_vao);
    glBindVertexArray(rf_vao);
    glGenBuffers(1, &rf_vbo);
    glGenBuffers(1, &rf_ebo);
    glBindBuffer(GL_ARRAY_BUFFER, rf_vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, rf_ebo);
    {
        GLsizei stride = (GLsizei)sizeof(HwrReflectVertex);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride,
            (void *)offsetof(HwrReflectVertex, x));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride,
            (void *)offsetof(HwrReflectVertex, nx));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, stride,
            (void *)offsetof(HwrReflectVertex, depth));
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, stride,
            (void *)offsetof(HwrReflectVertex, base));
    }
    glBindVertexArray(0);
    if (hwr_gl_check("rf_init"))
        return HWR_ERROR;
    rf_ready = 1;
    return HWR_OK;
}

int hwr_reflect_render(const unsigned char *pal8)
{
    HwrCamera cam;
    HwrReflectBatch batch;
    const HwrSceneSource *s = hwr_source;

    if (!hwr_is_ready() || s == NULL || s->get_reflect_faces == NULL)
        return 0;
    if (s->get_camera == NULL)
        return 0;
    if (!rf_ready && rf_init() != HWR_OK)
        return 0;
    if (s->get_camera(s->ctx, &cam) != 0)
        return 0;
    if (s->get_reflect_faces(s->ctx, &batch) <= 0 || batch.index_count <= 0)
        return 0;

    /* Palette on unit 1 (reuse the floor palette texture, refreshed here so the
     * pass is independent of floor/face draw order). */
    if (pal8 != NULL) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, fl_pal);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, 256, 1, 0, GL_RGB,
            GL_UNSIGNED_BYTE, pal8);
    }
    glActiveTexture(GL_TEXTURE0);

    glEnable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glUseProgram(rf_prog);
    glUniform1f(rf_l_d10, cam.d10);
    glUniform1f(rf_l_d14, cam.d14);
    glUniform1f(rf_l_d18, cam.d18);
    glUniform1f(rf_l_d1c, cam.d1c);
    glUniform1f(rf_l_scale, cam.scale);
    glUniform2f(rf_l_centre, cam.centre_x, cam.centre_y);
    glUniform3f(rf_l_ctr, cam.cx, cam.cy8, cam.cz);
    glUniform1i(rf_l_persp, cam.perspective);
    glUniform1i(rf_l_pal, 1);
    /* Tunable look — deep saturated spectraflair rainbow + thin sweeping streaks.
     * Hue sweeps from green (.33) through blue (.66) to purple (~.85). */
    glUniform1f(rf_l_huebase, 0.33f);      /* start at green */
    glUniform1f(rf_l_huespan, 0.55f);      /* sweep ~green->blue->purple */
    glUniform1f(rf_l_sat, 0.9f);           /* deep, saturated colours */
    glUniform1f(rf_l_paintlevel, 0.07f);   /* near-black metallic body */
    glUniform1f(rf_l_streakfreq, 3.0f);    /* number of streak bands */
    glUniform1f(rf_l_streaksharp, 8.0f);   /* thin, sharp streaks */
    glUniform1f(rf_l_sheen, 0.9f);         /* multicolour reflection strength */
    glUniform3f(rf_l_tinthi, 0.85f, 0.90f, 1.0f);

    /* Scene lighting so painted panels darken in shadow / unlit interiors,
     * mirroring fl_upload_lights + the floor's sun setup. */
    {
        /* Vehicle lamps are appended at the end of the light list; exclude them
         * here so a car's own headlights/tails don't self-illuminate its paint.
         * The paint is still lit by map lights (streetlamps, etc.). */
        extern int hwr_sw_vehicle_lights;
        HwrLight lights[HWR_MAX_LIGHTS];
        int n = (s->get_lights != NULL)
            ? s->get_lights(s->ctx, lights, HWR_MAX_LIGHTS) : 0;
        float pos_buf[HWR_MAX_LIGHTS*3], rgb_buf[HWR_MAX_LIGHTS*3];
        float rad_buf[HWR_MAX_LIGHTS], maxd2_buf[HWR_MAX_LIGHTS];
        HwrLightDefaults d = hwr_lights_defaults();
        float global_base = d.max_light_dist2;
        int i;
        if (n < 0) n = 0;
        if (n > HWR_MAX_LIGHTS) n = HWR_MAX_LIGHTS;
        n -= hwr_sw_vehicle_lights;     /* drop the trailing vehicle lamps */
        if (n < 0) n = 0;
        for (i = 0; i < n; i++) {
            pos_buf[i*3+0]=lights[i].x; pos_buf[i*3+1]=lights[i].y; pos_buf[i*3+2]=lights[i].z;
            rgb_buf[i*3+0]=lights[i].r; rgb_buf[i*3+1]=lights[i].g; rgb_buf[i*3+2]=lights[i].b;
            rad_buf[i]=lights[i].radius;
            if (lights[i].max_dist2 > 0.0f) {
                maxd2_buf[i]=lights[i].max_dist2;
            } else {
                float yabs = (lights[i].y < 0.0f) ? -lights[i].y : lights[i].y;
                maxd2_buf[i]=global_base + yabs*yabs;
            }
        }
        glUniform1f(rf_l_ambient, d.ambient);
        glUniform1f(rf_l_gain, d.intensity);
        glUniform3f(rf_l_tint, d.tint_r, d.tint_g, d.tint_b);
        if (n > 0) {
            glUniform3fv(rf_l_lpos, n, pos_buf);
            glUniform3fv(rf_l_lrgb, n, rgb_buf);
            glUniform1fv(rf_l_lrad, n, rad_buf);
            glUniform1fv(rf_l_lmaxd2, n, maxd2_buf);
        }
        glUniform1i(rf_l_nlights, n);
    }
    /* Sun shadow map on texture unit 3 (same as the floor pass). */
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, (GLuint)hwr_sun_texture());
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(rf_l_shadowmap, 3);
    {
        int sun_on = hwr_sun_enabled();
        glUniform1i(rf_l_sunenable, sun_on);
        if (sun_on) {
            glUniformMatrix4fv(rf_l_sunmvp, 1, GL_FALSE, hwr_sun_mvp());
            glUniform1f(rf_l_sunbright,  hwr_sun_bright());
            glUniform1f(rf_l_sunambient, hwr_sun_ambient());
            glUniform1f(rf_l_sunbias,    hwr_sun_bias());
            glUniform1i(rf_l_sunpcf,     hwr_sun_pcf());
            glUniform1f(rf_l_sunhaze,    hwr_sun_haze());
        }
    }

    glBindVertexArray(rf_vao);
    glBindBuffer(GL_ARRAY_BUFFER, rf_vbo);
    glBufferData(GL_ARRAY_BUFFER,
        (GLsizeiptr)batch.vert_count * sizeof(HwrReflectVertex), batch.verts,
        GL_STREAM_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, rf_ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
        (GLsizeiptr)batch.index_count * sizeof(uint32_t), batch.indices,
        GL_STREAM_DRAW);
    glDrawElements(GL_TRIANGLES, batch.index_count, GL_UNSIGNED_INT, (void *)0);
    glBindVertexArray(0);

    hwr_gl_check("hwr_reflect_render");
    return 1;
}

/** Drop cached GPU art (e.g. on level change). */
void hwr_floor_reset(void)
{
    fl_pages_uploaded = 0;
}
