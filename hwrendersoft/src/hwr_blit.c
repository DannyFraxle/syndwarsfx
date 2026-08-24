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

/** Configure the procedural rain overlay (from fx3d.ini [rain]).
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

/* ---------------------------------------------------------------------------
 * Distance fog (weather haze). Rain in the real world comes with a wall of
 * mist that swallows the far end of the street, so this pass greys out
 * geometry by its distance from the camera.
 *
 * Two modes, because the true per-pixel distance is only available when the
 * SSAO/water G-buffer path is running:
 *   - G-buffer present: sample the world-position attachment and fog by VIEW
 *     DEPTH, dot(P - centre, viewdir), between uStart and uEnd (world units).
 *     View depth is 0 at the screen-centre look-at point and grows into the
 *     distance, so start = 0 means "clear up to mid-screen, haze beyond".
 *     Do NOT use distance from the camera eye: the eye sits ~16384 units back
 *     from the centre, and that constant offset swamps the on-screen depth
 *     spread, flattening the whole view to one shade of fog. Background pixels
 *     (no geometry written) are left alone so the void beyond the map edge
 *     doesn't turn into a grey frame.
 *   - No G-buffer: fall back to a screen-space vertical gradient. The camera's
 *     tilt is fixed, so screen Y tracks distance closely enough for a haze;
 *     the trade-off is that tall near buildings get slightly over-fogged at
 *     their tops. */

static const char *fog_frag_src =
    "#version 330 core\n"
    "out vec4 frag;\n"
    "uniform vec2  uResolution;\n"
    "uniform sampler2D uPosition;  // xyz world pos (w = water mask)\n"
    "uniform int   uHasPos;        // 1 = uPosition is valid this frame\n"
    "uniform vec3  uCtr;           // camera centre / look-at point, world\n"
    "uniform vec3  uViewDir;       // unit view direction, into the screen\n"
    "uniform vec3  uColour;\n"
    "uniform float uDensity;       // max fog opacity, 0..1\n"
    "uniform float uStart, uEnd;   // view-depth ramp (uHasPos == 1)\n"
    "uniform float uScrStart, uScrEnd; // screen-Y ramp, 0 = top (uHasPos == 0)\n"
    "void main(){\n"
    "    vec2 uv = gl_FragCoord.xy / uResolution;\n"
    "    float t;\n"
    "    if (uHasPos == 1) {\n"
    "        vec4 pw = texture(uPosition, uv);\n"
    "        if (dot(pw.xyz, pw.xyz) < 1.0)\n"
    "            discard;                       // background / no geometry\n"
    "        float d = dot(pw.xyz - uCtr, uViewDir);   // 0 at screen centre\n"
    "        t = clamp((d - uStart) / max(uEnd - uStart, 1.0), 0.0, 1.0);\n"
    "    } else {\n"
    "        float sy = 1.0 - uv.y;             // 0 = top of screen\n"
    "        t = 1.0 - smoothstep(uScrStart, uScrEnd, sy);\n"
    "    }\n"
    "    /* Square the ramp so the near half stays clear and the haze builds up\n"
    "     * toward the horizon instead of washing the whole scene evenly. */\n"
    "    float a = t * t * uDensity;\n"
    "    if (a <= 0.003)\n"
    "        discard;\n"
    "    frag = vec4(uColour, a);\n"
    "}\n";

static GLuint fog_prog = 0;
static GLuint fog_vao = 0, fog_vbo = 0;
static GLint  fog_loc_res = -1, fog_loc_pos = -1, fog_loc_haspos = -1;
static GLint  fog_loc_ctr = -1, fog_loc_viewdir = -1;
static GLint  fog_loc_colour = -1, fog_loc_density = -1;
static GLint  fog_loc_start = -1, fog_loc_end = -1;
static GLint  fog_loc_scrstart = -1, fog_loc_scrend = -1;
static int    fog_ready = 0;

static int   fog_enable = 0;
static float fog_colour[3] = { 0.55f, 0.58f, 0.62f };
static float fog_density = 0.55f;
static float fog_start = -1500.0f;  /* view depth 0 = screen centre; negative
                                     * pulls the haze nearer than mid-screen */
static float fog_end = 6000.0f;
static float fog_scr_start = 0.0f;
static float fog_scr_end = 0.55f;

static int fog_init(void)
{
    GLuint vs, fs;
    static const float quad[] = {
        -1.0f, -1.0f,   1.0f, -1.0f,   -1.0f, 1.0f,
        -1.0f,  1.0f,   1.0f, -1.0f,    1.0f, 1.0f,
    };

    /* The rain vertex shader is a bare fullscreen quad - reuse its source. */
    vs = compile_shader(GL_VERTEX_SHADER, rain_vert_src);
    if (vs == 0)
        return HWR_ERROR;
    fs = compile_shader(GL_FRAGMENT_SHADER, fog_frag_src);
    if (fs == 0) {
        glDeleteShader(vs);
        return HWR_ERROR;
    }
    fog_prog = glCreateProgram();
    glAttachShader(fog_prog, vs);
    glAttachShader(fog_prog, fs);
    glLinkProgram(fog_prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    {
        GLint ok = 0;
        glGetProgramiv(fog_prog, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[512];
            glGetProgramInfoLog(fog_prog, sizeof(log), NULL, log);
            hwr_set_error("fog program link failed: %s", log);
            return HWR_ERROR;
        }
    }
    fog_loc_res      = glGetUniformLocation(fog_prog, "uResolution");
    fog_loc_pos      = glGetUniformLocation(fog_prog, "uPosition");
    fog_loc_haspos   = glGetUniformLocation(fog_prog, "uHasPos");
    fog_loc_ctr      = glGetUniformLocation(fog_prog, "uCtr");
    fog_loc_viewdir  = glGetUniformLocation(fog_prog, "uViewDir");
    fog_loc_colour   = glGetUniformLocation(fog_prog, "uColour");
    fog_loc_density  = glGetUniformLocation(fog_prog, "uDensity");
    fog_loc_start    = glGetUniformLocation(fog_prog, "uStart");
    fog_loc_end      = glGetUniformLocation(fog_prog, "uEnd");
    fog_loc_scrstart = glGetUniformLocation(fog_prog, "uScrStart");
    fog_loc_scrend   = glGetUniformLocation(fog_prog, "uScrEnd");

    glGenVertexArrays(1, &fog_vao);
    glBindVertexArray(fog_vao);
    glGenBuffers(1, &fog_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, fog_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void *)0);
    glBindVertexArray(0);

    if (hwr_gl_check("fog_init"))
        return HWR_ERROR;
    fog_ready = 1;
    return HWR_OK;
}

/** Configure the distance fog overlay (from fx3d.ini [fog]). enable
 *  toggles the pass; colour is the haze tint; density is the maximum opacity
 *  at full distance (0..1); start/end are the world-unit VIEW-DEPTH ramp used
 *  when the G-buffer is available (0 = the screen-centre look-at point);
 *  scr_start/scr_end are the screen-Y ramp (0 = top of screen) used as the
 *  geometry-free fallback. */
void hwr_fog_config(int enable, float r, float g, float b, float density,
    float start, float end, float scr_start, float scr_end)
{
    fog_enable = enable;
    fog_colour[0] = r; fog_colour[1] = g; fog_colour[2] = b;
    fog_density = density;
    fog_start = start;
    fog_end = (end > start) ? end : (start + 1.0f);
    fog_scr_start = scr_start;
    fog_scr_end = (scr_end > scr_start) ? scr_end : (scr_start + 0.01f);
}

/** Draw the distance fog, alpha-blended over the already-rendered 3D scene.
 *  Call after the 3D passes and before the rain overlay, so the rain streaks
 *  stay crisp in front of the haze. No-op when disabled or not ready. */
void hwr_fog_render(void)
{
    int dw = 0, dh = 0;
    unsigned int pos_tex;
    float ctr[3] = { 0.0f, 0.0f, 0.0f };
    float dir[3] = { 0.0f, 0.0f, 1.0f };

    if (!fog_enable || !hwr_is_ready())
        return;
    if (!fog_ready && fog_init() != HWR_OK)
        return;

    hwr_drawable_size(&dw, &dh);
    if (dw <= 0 || dh <= 0)
        return;

    pos_tex = hwr_ssao_position_texture();
    hwr_ssao_get_view(ctr, dir);

    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram(fog_prog);
    glUniform2f(fog_loc_res, (float)dw, (float)dh);
    glUniform1i(fog_loc_haspos, pos_tex ? 1 : 0);
    glUniform1i(fog_loc_pos, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)pos_tex);
    glUniform3f(fog_loc_ctr, ctr[0], ctr[1], ctr[2]);
    glUniform3f(fog_loc_viewdir, dir[0], dir[1], dir[2]);
    glUniform3f(fog_loc_colour, fog_colour[0], fog_colour[1], fog_colour[2]);
    glUniform1f(fog_loc_density, fog_density);
    glUniform1f(fog_loc_start, fog_start);
    glUniform1f(fog_loc_end, fog_end);
    glUniform1f(fog_loc_scrstart, fog_scr_start);
    glUniform1f(fog_loc_scrend, fog_scr_end);

    glBindVertexArray(fog_vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);

    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);

    hwr_gl_check("hwr_fog_render");
}

/* ---------------------------------------------------------------------------
 * Bullet-time screen filter. Cues the player that an explosion just slowed
 * the game down on purpose (not a hitch): a real radial "zoom blur" growing
 * outward from screen centre (so it reads as edge blur), plus a temporal
 * blend with the previous frame for a motion-trail/ghosting feel - both
 * scaled by how deep into the slow-motion dip the current frame is
 * (game_speed.c's bullettime_intensity(), 0 = normal speed, 1 = deepest
 * dip). Unlike the rain overlay, this genuinely needs the already-rendered
 * scene pixels, so it captures the back buffer into a texture (via
 * glBlitFramebuffer, already used the same way for SSAO's depth blit) rather
 * than just drawing a flat screen-space quad:
 *
 *   back buffer --blit--> bt_scene_tex --radial blur--> bt_blur_tex
 *     --blend with bt_hist_tex--> back buffer --blit--> bt_hist_tex (next frame)
 *
 * Runs after all opaque/translucent 3D passes and the rain overlay, before
 * debug overlays and the keyed WScreen (HUD) composite - so the HUD stays
 * crisp on top of the blurred scene. */

static const char *bt_fs_vert_src =
    "#version 330 core\n"
    "out vec2 vUV;\n"
    "void main(){\n"
    "    vec2 p = vec2((gl_VertexID == 1) ? 3.0 : -1.0,\n"
    "                  (gl_VertexID == 2) ? 3.0 : -1.0);\n"
    "    vUV = p * 0.5 + 0.5;\n"
    "    gl_Position = vec4(p, 0.0, 1.0);\n"
    "}\n";

static const char *bt_radial_frag_src =
    "#version 330 core\n"
    "in vec2 vUV;\n"
    "out vec4 frag;\n"
    "uniform sampler2D uScene;\n"
    "uniform vec2 uResolution;\n"
    "uniform float uIntensity;   // 0..1, current dip depth\n"
    "uniform float uStrength;    // max blur reach (UV units) at the edge, full dip\n"
    "void main(){\n"
    "    vec2 aspect = vec2(uResolution.x / uResolution.y, 1.0);\n"
    "    vec2 d = (vUV - vec2(0.5)) * aspect;\n"
    "    float dist = length(d);\n"
    "    vec2 dirUV = (dist > 1e-5) ? (d / dist) / aspect : vec2(0.0);\n"
    "    // Wide dead zone covering most of the screen (radius < 0.45 = zero\n"
    "    // blur, stays genuinely sharp), ramping up to full uStrength only in\n"
    "    // the outer rim by dist ~0.85 (right at the edges) - unlike a plain\n"
    "    // linear ramp from the exact centre, this keeps almost the whole view\n"
    "    // crisp and confines the blur to a narrow edge band.\n"
    "    float edgeFactor = smoothstep(0.45, 0.85, dist);\n"
    "    float amount = uIntensity * uStrength * edgeFactor;\n"
    "    vec3 sum = vec3(0.0);\n"
    "    const int N = 10;\n"
    "    for (int i = 0; i < N; i++) {\n"
    "        float t = (float(i) / float(N - 1) - 0.5) * amount;\n"
    "        vec2 uv = clamp(vUV - dirUV * t, vec2(0.001), vec2(0.999));\n"
    "        sum += texture(uScene, uv).rgb;\n"
    "    }\n"
    "    frag = vec4(sum / float(N), 1.0);\n"
    "}\n";

static const char *bt_blend_frag_src =
    "#version 330 core\n"
    "in vec2 vUV;\n"
    "out vec4 frag;\n"
    "uniform sampler2D uCurrent;   // this frame's radial-blurred scene\n"
    "uniform sampler2D uHistory;   // previous frame's final blended output\n"
    "uniform float uTrail;         // 0..1, how much of history persists\n"
    "void main(){\n"
    "    vec3 cur = texture(uCurrent, vUV).rgb;\n"
    "    vec3 hist = texture(uHistory, vUV).rgb;\n"
    "    frag = vec4(mix(cur, hist, uTrail), 1.0);\n"
    "}\n";

static GLuint bt_radial_prog = 0, bt_blend_prog = 0;
static GLint  btr_loc_scene = -1, btr_loc_res = -1, btr_loc_intensity = -1, btr_loc_strength = -1;
static GLint  btb_loc_cur = -1, btb_loc_hist = -1, btb_loc_trail = -1;
static GLuint bt_quad_vao = 0;
static int    bullettime_ready = 0;

static int   bullettime_enable = 0;
static float bullettime_blur_strength = 0.06f;
static float bullettime_trail = 0.4f;

/* Capture / blur / history render targets, resized to the drawable each time
 * it changes (mirrors hwr_ssao.c's G-buffer resize pattern). */
static int    bt_w = 0, bt_h = 0;
static GLuint bt_scene_fbo = 0, bt_scene_tex = 0;
static GLuint bt_blur_fbo = 0, bt_blur_tex = 0;
static GLuint bt_hist_fbo = 0, bt_hist_tex = 0;

static GLuint bt_make_color_fbo(int w, int h, GLuint *out_tex)
{
    GLuint fbo, tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);   /* avoid a garbage flash on first use (esp. history) */

    *out_tex = tex;
    return fbo;
}

static void bt_free_targets(void)
{
    if (bt_scene_tex) { glDeleteTextures(1, &bt_scene_tex); bt_scene_tex = 0; }
    if (bt_blur_tex)  { glDeleteTextures(1, &bt_blur_tex);  bt_blur_tex = 0; }
    if (bt_hist_tex)  { glDeleteTextures(1, &bt_hist_tex);  bt_hist_tex = 0; }
    if (bt_scene_fbo) { glDeleteFramebuffers(1, &bt_scene_fbo); bt_scene_fbo = 0; }
    if (bt_blur_fbo)  { glDeleteFramebuffers(1, &bt_blur_fbo);  bt_blur_fbo = 0; }
    if (bt_hist_fbo)  { glDeleteFramebuffers(1, &bt_hist_fbo);  bt_hist_fbo = 0; }
}

static int bt_resize(int w, int h)
{
    if (w == bt_w && h == bt_h && bt_scene_fbo != 0)
        return HWR_OK;
    bt_free_targets();
    bt_w = w; bt_h = h;

    bt_scene_fbo = bt_make_color_fbo(w, h, &bt_scene_tex);
    bt_blur_fbo  = bt_make_color_fbo(w, h, &bt_blur_tex);
    bt_hist_fbo  = bt_make_color_fbo(w, h, &bt_hist_tex);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (hwr_gl_check("bt_resize"))
        return HWR_ERROR;
    return HWR_OK;
}

static GLuint bt_link_prog(const char *frag_src)
{
    GLuint vs, fs, prog;
    GLint ok = 0;

    vs = compile_shader(GL_VERTEX_SHADER, bt_fs_vert_src);
    if (vs == 0)
        return 0;
    fs = compile_shader(GL_FRAGMENT_SHADER, frag_src);
    if (fs == 0) {
        glDeleteShader(vs);
        return 0;
    }
    prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(prog, sizeof(log), NULL, log);
        hwr_set_error("bullettime program link failed: %s", log);
        return 0;
    }
    return prog;
}

static int bullettime_init(void)
{
    bt_radial_prog = bt_link_prog(bt_radial_frag_src);
    bt_blend_prog  = bt_link_prog(bt_blend_frag_src);
    if (!bt_radial_prog || !bt_blend_prog)
        return HWR_ERROR;

    btr_loc_scene     = glGetUniformLocation(bt_radial_prog, "uScene");
    btr_loc_res       = glGetUniformLocation(bt_radial_prog, "uResolution");
    btr_loc_intensity = glGetUniformLocation(bt_radial_prog, "uIntensity");
    btr_loc_strength  = glGetUniformLocation(bt_radial_prog, "uStrength");

    btb_loc_cur   = glGetUniformLocation(bt_blend_prog, "uCurrent");
    btb_loc_hist  = glGetUniformLocation(bt_blend_prog, "uHistory");
    btb_loc_trail = glGetUniformLocation(bt_blend_prog, "uTrail");

    glGenVertexArrays(1, &bt_quad_vao);

    if (hwr_gl_check("bullettime_init"))
        return HWR_ERROR;
    bullettime_ready = 1;
    return HWR_OK;
}

/** Configure the bullet-time screen filter (from fx3d.ini
 *  [bullettime]). enable toggles the pass; blur_strength is the max radial-
 *  blur reach at the screen edge (UV units, full dip); trail is how much of
 *  the previous frame persists into this one at full dip (0..1, motion-trail
 *  strength). */
void hwr_bullettime_config(int enable, float blur_strength, float trail)
{
    bullettime_enable = enable;
    bullettime_blur_strength = blur_strength;
    bullettime_trail = trail;
}

/** Draw the bullet-time radial blur + motion-trail effect over the already-
 *  rendered scene. intensity is game_speed.c's bullettime_intensity() (0 =
 *  normal speed, no-op; up to 1 = deepest slow-motion dip). Call after the 3D
 *  passes, before debug overlays and the keyed WScreen (HUD) composite -
 *  captures/replaces the CURRENT back buffer content, so anything drawn
 *  after this (debug labels, HUD) stays crisp on top. */
void hwr_bullettime_render(float intensity)
{
    int dw = 0, dh = 0;

    if (!bullettime_enable || intensity <= 0.001f || !hwr_is_ready())
        return;
    if (!bullettime_ready && bullettime_init() != HWR_OK)
        return;

    hwr_drawable_size(&dw, &dh);
    if (dw <= 0 || dh <= 0)
        return;
    if (bt_resize(dw, dh) != HWR_OK)
        return;

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glBindVertexArray(bt_quad_vao);

    /* 1) Capture the already-rendered scene into bt_scene_tex. */
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, bt_scene_fbo);
    glBlitFramebuffer(0, 0, dw, dh, 0, 0, dw, dh, GL_COLOR_BUFFER_BIT, GL_NEAREST);

    /* 2) Radial/zoom blur: bt_scene_tex -> bt_blur_tex. */
    glBindFramebuffer(GL_FRAMEBUFFER, bt_blur_fbo);
    glViewport(0, 0, dw, dh);
    glUseProgram(bt_radial_prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, bt_scene_tex);
    glUniform1i(btr_loc_scene, 0);
    glUniform2f(btr_loc_res, (float)dw, (float)dh);
    glUniform1f(btr_loc_intensity, intensity);
    glUniform1f(btr_loc_strength, bullettime_blur_strength);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    /* 3) Blend with last frame's result (motion trail) -> back buffer. */
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, dw, dh);
    glUseProgram(bt_blend_prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, bt_blur_tex);
    glUniform1i(btb_loc_cur, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, bt_hist_tex);
    glUniform1i(btb_loc_hist, 1);
    glUniform1f(btb_loc_trail, intensity * bullettime_trail);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    /* 4) Save this frame's final result as history for next frame's trail. */
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, bt_hist_fbo);
    glBlitFramebuffer(0, 0, dw, dh, 0, 0, dw, dh, GL_COLOR_BUFFER_BIT, GL_NEAREST);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindVertexArray(0);
    glEnable(GL_DEPTH_TEST);

    hwr_gl_check("hwr_bullettime_render");
}
