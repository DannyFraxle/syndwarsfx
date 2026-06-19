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

/* Reproduces transform_shpoint() per-vertex, including the mode-5 perspective
 * foreshortening (which no single matrix can express). */
static const char *floor_vert_src =
    "#version 330 core\n"
    "layout(location=0) in vec3 aPos;\n"
    "layout(location=1) in vec2 aUV;\n"
    "layout(location=2) in float aDepth;\n"  /* per-tile constant scrd */
    "layout(location=3) in uint aPage;\n"
    "layout(location=4) in float aLight;\n"  /* SW baked shade 0..1 (AO) */
    "uniform float uD10, uD14, uD18, uD1C;\n"
    "uniform float uScale;\n"
    "uniform vec2 uCentre;   // D3C, D40\n"
    "uniform vec3 uCtr;      // camera centre: cx, 8*yc, cz\n"
    "uniform int  uPersp;\n"
    "out vec3 vUV;\n"
    "out vec3 vWorldPos;\n"
    "out float vAO;\n"
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
    "in float vScrd;\n"
    "layout(location=0) out vec4 frag;\n"
    "layout(location=1) out vec4 fragPos;   // xyz world pos + w view depth -> SSAO\n"
    "uniform sampler2DArray uTex;         // R8 palette indices\n"
    "uniform sampler2D uPalette;          // RGB8 256x1, active 8-bit palette\n"
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
    "void main(){\n"
    "    fragPos = vec4(vWorldPos, vScrd);   // G-buffer attachment 1\n"
    "    vec3 light_col = vec3(0.0);\n"
    "    float shadow = 0.0;             // accumulated darkening from anti-lights\n"
    "    for (int i = 0; i < uNumLights; i++) {\n"
    "        float r = uLightRadius[i];\n"
    "        if (r == 0.0) continue;\n"
    "        vec3 delta = vWorldPos - uLightPos[i];\n"
    "        float dist2 = delta.x * delta.x + delta.z * delta.z\n"
    "                 + delta.y * delta.y;                    // full 3D distance\n"
    "        float nd   = dist2 / uLightMaxDist2[i];                // 0..1 normalized dist²\n"
    "        if (nd >= 1.0) continue;\n"
    "        float brightness = 1.0 - sqrt(nd);                     // linear 1..0 radial falloff\n"
    "        if (r > 0.0)\n"
    "            light_col += uLightRgb[i] * brightness;           // additive accumulation\n"
    "        else\n"
    "            shadow += uLightRgb[i].x * brightness;            // anti-light: fake shadow\n"
    "    }\n"
    "    float ao = mix(1.0, vAO, uAO);\n"
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
    "    light_col = light_col * uGain * uTint + base;\n"
    "    light_col *= ao;\n"
    "    light_col *= clamp(1.0 - shadow, 0.0, 1.0);\n"
    "    light_col = max(light_col, 0.0);              // allow >1.0 for overbright glow\n"
    "    if (vUV.z > 254.5) {             // flat-shaded face (Texture==0), no texture\n"
    "        frag = vec4(vec3(0.55) * light_col, 1.0);\n"
    "        return;\n"
    "    }\n"
    "    int idx = int(texture(uTex, vUV).r * 255.0 + 0.5);\n"
    "    if (uTransKey >= 0 && idx == uTransKey)\n"
    "        discard;                     // see through windows/grates to faces behind\n"
    "    vec3 c = texture(uPalette, vec2((float(idx) + 0.5) / 256.0, 0.5)).rgb;\n"
    "    frag = vec4(c * light_col, 1.0);\n"
    "}\n";

static GLuint fl_prog = 0;
static GLuint fl_vao = 0, fl_vbo = 0, fl_ebo = 0;
static GLuint fl_tex = 0, fl_pal = 0;
static GLint  fl_loc_tex = -1, fl_loc_pal = -1, fl_loc_transkey = -1;
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
static GLint  fl_loc_shadowmap = -1;
static GLint  fl_loc_sun_mvp   = -1;
static GLint  fl_loc_sun_bright = -1;
static GLint  fl_loc_sun_ambient = -1;
static GLint  fl_loc_sun_bias  = -1;
static GLint  fl_loc_sun_enable = -1;
static GLint  fl_loc_sun_pcf   = -1;
static GLint  fl_loc_sun_debug = -1;
static GLint  fl_loc_sun_haze  = -1;
static int    fl_ready = 0;
static int    fl_pages_uploaded = 0;
static int    fl_filter = -1;

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
    fl_loc_shadowmap  = glGetUniformLocation(fl_prog, "uShadowMap");
    fl_loc_sun_mvp    = glGetUniformLocation(fl_prog, "uSunMVP");
    fl_loc_sun_bright = glGetUniformLocation(fl_prog, "uSunBright");
    fl_loc_sun_ambient= glGetUniformLocation(fl_prog, "uSunAmbient");
    fl_loc_sun_bias   = glGetUniformLocation(fl_prog, "uSunBias");
    fl_loc_sun_enable = glGetUniformLocation(fl_prog, "uSunEnable");
    fl_loc_sun_pcf    = glGetUniformLocation(fl_prog, "uSunPCF");
    fl_loc_sun_debug  = glGetUniformLocation(fl_prog, "uSunDebug");
    fl_loc_sun_haze   = glGetUniformLocation(fl_prog, "uSunHaze");

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
    }
    glBindVertexArray(0);

    glGenTextures(1, &fl_tex);
    glGenTextures(1, &fl_pal);
    glBindTexture(GL_TEXTURE_2D, fl_pal);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

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
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, fl_tex);

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
    glUniform1i(fl_loc_transkey, trans_key);

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
    {
        HwrLight lights[HWR_MAX_LIGHTS];
        int nlight = (s->get_lights != NULL)
            ? s->get_lights(s->ctx, lights, HWR_MAX_LIGHTS) : 0;
        fl_upload_lights(lights, nlight < 0 ? 0 : nlight);
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

    (void)filter_linear;   /* pages already uploaded with the chosen filter */
    /* Faces are double-sided (cull stays disabled): through a window you see the
     * building's back wall. Index 0 is the texture transparent key (windows /
     * grates), so discard it to let those back faces show through. */
    fl_setup_program(&cam, pal8, 0);
    {
        HwrLight lights[HWR_MAX_LIGHTS];
        int nlight = (s->get_lights != NULL)
            ? s->get_lights(s->ctx, lights, HWR_MAX_LIGHTS) : 0;
        fl_upload_lights(lights, nlight < 0 ? 0 : nlight);
    }
    fl_draw_batch(&batch);

    hwr_gl_check("hwr_faces_render");
    return 1;
}

/** Drop cached GPU art (e.g. on level change). */
void hwr_floor_reset(void)
{
    fl_pages_uploaded = 0;
}
