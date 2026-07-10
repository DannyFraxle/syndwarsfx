/******************************************************************************/
// Syndicate Wars FX3D - OpenGL hardware renderer for Bullfrog titles.
/******************************************************************************/
/**                        2026 danny@fraxle.net                             **/
/******************************************************************************/
/** @file hwr_blit.c
 *     Presents the game's 8-bit indexed framebuffer through OpenGL.
 * @par Purpose:
 *     Uploads the software-rendered WScreen as a single-channel (GL_R8)
 *     texture and draws it as a fullscreen quad, depalettising in the fragment
 *     shader against the 6-bit game palette. This makes the existing software
 *     output display on a GL window (so --hwrender is usable now), and provides
 *     the exact indexed-sampling shader the 3D phases build on.
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

#include <SDL.h>

static const char *blit_vert_src =
    "#version 330 core\n"
    "layout(location=0) in vec2 aPos;\n"
    "out vec2 vUV;\n"
    "void main(){\n"
    "    /* Map clip-space quad to texture coords; flip V so the top row of the\n"
    "       top-down framebuffer appears at the top of the window. */\n"
    "    vUV = vec2((aPos.x + 1.0) * 0.5, 1.0 - (aPos.y + 1.0) * 0.5);\n"
    "    gl_Position = vec4(aPos, 0.0, 1.0);\n"
    "}\n";

static const char *blit_frag_src =
    "#version 330 core\n"
    "in vec2 vUV;\n"
    "out vec4 frag;\n"
    "uniform sampler2D uScreen;   // R8, palette index in 0..255\n"
    "uniform sampler2D uPalette;  // RGB8 256x1, full-range 8-bit colour\n"
    "uniform int uKey;            // index to discard (<0 = none)\n"
    "uniform float uAlpha;        // background opacity (<1 = transparent HUD fill)\n"
    "uniform float uBgLuma;       // luminance threshold: pixels below are background\n"
    "                             //   (get uAlpha); pixels above are content (solid).\n"
    "                             //   Set to 0 to make everything solid (uBgLuma<=0).\n"
    "void main(){\n"
    "    float idx = texture(uScreen, vUV).r * 255.0;\n"
    "    if (uKey >= 0 && int(idx + 0.5) == uKey)\n"
    "        discard;             // let the 3D scene below show through\n"
    "    vec3 c = texture(uPalette, vec2((idx + 0.5) / 256.0, 0.5)).rgb;\n"
    "    float a = 1.0;\n"
    "    if (uBgLuma > 0.0) {\n"
    "        float luma = dot(c, vec3(0.299, 0.587, 0.114));\n"
    "        a = (luma < uBgLuma) ? uAlpha : 1.0;\n"
    "    }\n"
    "    frag = vec4(c, a);\n"
    "}\n";

static GLuint blit_prog = 0;
static GLuint blit_vao = 0;
static GLuint blit_vbo = 0;
static GLuint blit_screen_tex = 0;
static GLuint blit_pal_tex = 0;
static GLint  blit_loc_screen = -1;
static GLint  blit_loc_palette = -1;
static GLint  blit_loc_key = -1;
static GLint  blit_loc_alpha = -1;
static GLint  blit_loc_bgluma = -1;
static int    blit_ready = 0;

static GLuint compile_shader(GLenum type, const char *src)
{
    GLuint sh = glCreateShader(type);
    GLint ok = 0;
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(sh, sizeof(log), NULL, log);
        hwr_set_error("blit shader compile failed: %s", log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static int blit_init(void)
{
    GLuint vs, fs;
    static const float quad[] = {
        -1.0f, -1.0f,   1.0f, -1.0f,   -1.0f, 1.0f,
        -1.0f,  1.0f,   1.0f, -1.0f,    1.0f, 1.0f,
    };

    vs = compile_shader(GL_VERTEX_SHADER, blit_vert_src);
    if (vs == 0)
        return HWR_ERROR;
    fs = compile_shader(GL_FRAGMENT_SHADER, blit_frag_src);
    if (fs == 0) {
        glDeleteShader(vs);
        return HWR_ERROR;
    }
    blit_prog = glCreateProgram();
    glAttachShader(blit_prog, vs);
    glAttachShader(blit_prog, fs);
    glLinkProgram(blit_prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    {
        GLint ok = 0;
        glGetProgramiv(blit_prog, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[512];
            glGetProgramInfoLog(blit_prog, sizeof(log), NULL, log);
            hwr_set_error("blit program link failed: %s", log);
            return HWR_ERROR;
        }
    }
    blit_loc_screen  = glGetUniformLocation(blit_prog, "uScreen");
    blit_loc_palette = glGetUniformLocation(blit_prog, "uPalette");
    blit_loc_key     = glGetUniformLocation(blit_prog, "uKey");
    blit_loc_alpha   = glGetUniformLocation(blit_prog, "uAlpha");
    blit_loc_bgluma  = glGetUniformLocation(blit_prog, "uBgLuma");

    glGenVertexArrays(1, &blit_vao);
    glBindVertexArray(blit_vao);
    glGenBuffers(1, &blit_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, blit_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void *)0);
    glBindVertexArray(0);

    glGenTextures(1, &blit_screen_tex);
    glBindTexture(GL_TEXTURE_2D, blit_screen_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenTextures(1, &blit_pal_tex);
    glBindTexture(GL_TEXTURE_2D, blit_pal_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    if (hwr_gl_check("blit_init"))
        return HWR_ERROR;
    blit_ready = 1;
    return HWR_OK;
}

static void blit_core(const uint8_t *px, int w, int h, int pitch,
    const uint8_t *pal, int key_index, int do_clear, float alpha, float bg_luma)
{
    if (!hwr_is_ready() || px == NULL || w <= 0 || h <= 0)
        return;
    if (!blit_ready && blit_init() != HWR_OK)
        return;
    /* WScreen rows are tightly packed at GraphicsScreenWidth; the present path
     * assumes pitch == w. If that ever differs we'd need GL_UNPACK_ROW_LENGTH. */
    (void)pitch;

    hwr_sync_viewport();   /* the window resizes per game video mode */
    glDisable(GL_DEPTH_TEST);   /* the overlay sits on top of the 3D scene */
    if (alpha < 1.0f) {
        /* Transparent HUD: blend the overlay over the 3D scene. */
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    } else {
        glDisable(GL_BLEND);
    }
    if (do_clear)
        glClear(GL_COLOR_BUFFER_BIT);

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, blit_screen_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, w, h, 0, GL_RED, GL_UNSIGNED_BYTE, px);

    if (pal != NULL) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, blit_pal_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, 256, 1, 0, GL_RGB,
            GL_UNSIGNED_BYTE, pal);
    }

    glUseProgram(blit_prog);
    glUniform1i(blit_loc_screen, 0);
    glUniform1i(blit_loc_palette, 1);
    glUniform1i(blit_loc_key, key_index);
    glUniform1f(blit_loc_alpha, alpha);
    glUniform1f(blit_loc_bgluma, bg_luma);

    glBindVertexArray(blit_vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);

    if (alpha < 1.0f)
        glDisable(GL_BLEND);

    hwr_gl_check("blit_core");
}

void hwr_present_indexed(const uint8_t *px, int w, int h, int pitch,
    const uint8_t *pal)
{
    /* Full opaque blit: clear first, no key (menus / non-engine screens). */
    blit_core(px, w, h, pitch, pal, -1, 1, 1.0f, 0.0f);
}

void hwr_present_indexed_keyed(const uint8_t *px, int w, int h, int pitch,
    const uint8_t *pal, int key_index)
{
    /* Overlay over the already-rendered 3D scene: no clear, discard key pixels. */
    blit_core(px, w, h, pitch, pal, key_index, 0, 1.0f, 0.0f);
}

void hwr_present_indexed_keyed_alpha(const uint8_t *px, int w, int h, int pitch,
    const uint8_t *pal, int key_index, float alpha)
{
    /* Keyed overlay, blended at `alpha` over the 3D scene (transparent HUD). */
    blit_core(px, w, h, pitch, pal, key_index, 0, alpha, 0.0f);
}

void hwr_present_indexed_keyed_luma(const uint8_t *px, int w, int h, int pitch,
    const uint8_t *pal, int key_index, float bg_alpha, float bg_luma)
{
    /* Keyed overlay with per-pixel luma split: pixels below bg_luma threshold
     * are composited at bg_alpha (panel background fill); pixels at or above
     * are fully opaque (outlines, numbers, powerbar, map). */
    blit_core(px, w, h, pitch, pal, key_index, 0, bg_alpha, bg_luma);
}

/* ---------------------------------------------------------------------------
 * Procedural rain overlay (weather effect). Replaces the old approach of
 * blitting SW-drawn rain droplets into WScreen: those got composited by the
 * keyed present above as fully opaque pixels (the keyed shader only has an
 * on/off discard, no per-pixel alpha), so against the 3D floor/buildings they
 * showed as solid, blocky streaks instead of translucent rain. Drawing rain
 * as its own alpha-blended fullscreen pass lets it genuinely blend over the
 * already-rendered 3D scene, and its thickness/opacity are shader parameters
 * instead of baked SW pixel blocks. */

static const char *rain_vert_src =
    "#version 330 core\n"
    "layout(location=0) in vec2 aPos;\n"
    "void main(){ gl_Position = vec4(aPos, 0.0, 1.0); }\n";

static const char *rain_frag_src =
    "#version 330 core\n"
    "out vec4 frag;\n"
    "uniform vec2 uResolution;\n"
    "uniform float uTime;\n"
    "uniform float uAlpha;    // overall streak opacity\n"
    "uniform float uDensity;  // streak columns per screen-height unit of width\n"
    "uniform float uSpeed;    // fall speed, screen-heights per second\n"
    "uniform float uWidth;    // streak thickness in pixels\n"
    "uniform float uLength;   // streak length, fraction of screen height\n"
    "uniform float uAngle;    // wind slant, radians (0 = straight down)\n"
    "float hash(float n){ return fract(sin(n) * 43758.5453123); }\n"
    "void main(){\n"
    "    vec2 uv = gl_FragCoord.xy / uResolution.y;\n"
    "    /* gl_FragCoord.y is 0 at the BOTTOM of the window, so uv.y counts up\n"
    "     * from the bottom. Flip to sy (0 = top, 1 = bottom) so increasing time\n"
    "     * moves streaks from top toward bottom instead of climbing upward. */\n"
    "    float sy = 1.0 - uv.y;\n"
    "    float x = uv.x - tan(uAngle) * sy;\n"
    "    float xs = x * uDensity;\n"
    "    float col = floor(xs);\n"
    "    float cellX = fract(xs);\n"
    "    float seed = hash(col);\n"
    "    float speedMul = 0.6 + 0.8 * hash(col + 13.7);\n"
    "    float phase = seed * 10.0;\n"
    "    float y = fract(sy + phase - uTime * uSpeed * speedMul);\n"
    "    float within = smoothstep(0.0, uLength * 0.15, y) * (1.0 - smoothstep(uLength * 0.5, uLength, y));\n"
    "    float dx = abs(cellX - 0.5) / uDensity;\n"
    "    float widthUv = max(uWidth / uResolution.y, 0.0005);\n"
    "    float streak = 1.0 - smoothstep(0.0, widthUv, dx);\n"
    "    float a = streak * within * uAlpha;\n"
    "    if (a <= 0.003)\n"
    "        discard;\n"
    "    frag = vec4(0.75, 0.82, 0.90, a);\n"
    "}\n";

static GLuint rain_prog = 0;
static GLuint rain_vao = 0, rain_vbo = 0;
static GLint  rain_loc_res = -1, rain_loc_time = -1, rain_loc_alpha = -1;
static GLint  rain_loc_density = -1, rain_loc_speed = -1, rain_loc_width = -1;
static GLint  rain_loc_length = -1, rain_loc_angle = -1;
static int    rain_ready = 0;

static int   rain_enable = 0;
static float rain_alpha = 0.35f;
static float rain_density = 60.0f;
static float rain_speed = 0.6f;
static float rain_width = 1.5f;
static float rain_length = 0.10f;
static float rain_angle = 0.0f;

static int rain_init(void)
{
    GLuint vs, fs;
    static const float quad[] = {
        -1.0f, -1.0f,   1.0f, -1.0f,   -1.0f, 1.0f,
        -1.0f,  1.0f,   1.0f, -1.0f,    1.0f, 1.0f,
    };

    vs = compile_shader(GL_VERTEX_SHADER, rain_vert_src);
    if (vs == 0)
        return HWR_ERROR;
    fs = compile_shader(GL_FRAGMENT_SHADER, rain_frag_src);
    if (fs == 0) {
        glDeleteShader(vs);
        return HWR_ERROR;
    }
    rain_prog = glCreateProgram();
    glAttachShader(rain_prog, vs);
    glAttachShader(rain_prog, fs);
    glLinkProgram(rain_prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    {
        GLint ok = 0;
        glGetProgramiv(rain_prog, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[512];
            glGetProgramInfoLog(rain_prog, sizeof(log), NULL, log);
            hwr_set_error("rain program link failed: %s", log);
            return HWR_ERROR;
        }
    }
    rain_loc_res     = glGetUniformLocation(rain_prog, "uResolution");
    rain_loc_time    = glGetUniformLocation(rain_prog, "uTime");
    rain_loc_alpha   = glGetUniformLocation(rain_prog, "uAlpha");
    rain_loc_density = glGetUniformLocation(rain_prog, "uDensity");
    rain_loc_speed   = glGetUniformLocation(rain_prog, "uSpeed");
    rain_loc_width   = glGetUniformLocation(rain_prog, "uWidth");
    rain_loc_length  = glGetUniformLocation(rain_prog, "uLength");
    rain_loc_angle   = glGetUniformLocation(rain_prog, "uAngle");

    glGenVertexArrays(1, &rain_vao);
    glBindVertexArray(rain_vao);
    glGenBuffers(1, &rain_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, rain_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void *)0);
    glBindVertexArray(0);

    if (hwr_gl_check("rain_init"))
        return HWR_ERROR;
    rain_ready = 1;
    return HWR_OK;
}

/** Configure the procedural rain overlay (from fx3d_lights.ini [rain]).
 *  enable toggles the pass; alpha is the streak opacity (0..1); density is the
 *  streak-column count across one screen-height of width; speed is the fall
 *  speed in screen-heights/second; width is streak thickness in pixels;
 *  length is streak length as a fraction of screen height; angle is a wind
 *  slant in radians (0 = straight down). */
void hwr_rain_config(int enable, float alpha, float density, float speed,
    float width, float length, float angle)
{
    rain_enable = enable;
    rain_alpha = alpha;
    rain_density = density;
    rain_speed = speed;
    rain_width = width;
    rain_length = length;
    rain_angle = angle;
}

/** Draw the procedural rain overlay, alpha-blended over the already-rendered
 *  3D scene. Call after the opaque/translucent 3D passes, before the keyed
 *  WScreen (HUD) composite. No-op when disabled or not ready. */
void hwr_rain_render(void)
{
    int dw = 0, dh = 0;

    if (!rain_enable || !hwr_is_ready())
        return;
    if (!rain_ready && rain_init() != HWR_OK)
        return;

    hwr_drawable_size(&dw, &dh);
    if (dw <= 0 || dh <= 0)
        return;

    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram(rain_prog);
    glUniform2f(rain_loc_res, (float)dw, (float)dh);
    glUniform1f(rain_loc_time, (float)(SDL_GetTicks() / 1000.0));
    glUniform1f(rain_loc_alpha, rain_alpha);
    glUniform1f(rain_loc_density, rain_density);
    glUniform1f(rain_loc_speed, rain_speed);
    glUniform1f(rain_loc_width, rain_width);
    glUniform1f(rain_loc_length, rain_length);
    glUniform1f(rain_loc_angle, rain_angle);

    glBindVertexArray(rain_vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);

    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);

    hwr_gl_check("hwr_rain_render");
}
