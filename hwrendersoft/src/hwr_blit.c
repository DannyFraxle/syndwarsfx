/******************************************************************************/
// Syndicate Wars FX3D - OpenGL hardware renderer for Bullfrog titles.
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
    "void main(){\n"
    "    float idx = texture(uScreen, vUV).r * 255.0;\n"
    "    vec3 c = texture(uPalette, vec2((idx + 0.5) / 256.0, 0.5)).rgb;\n"
    "    frag = vec4(c, 1.0);\n"
    "}\n";

static GLuint blit_prog = 0;
static GLuint blit_vao = 0;
static GLuint blit_vbo = 0;
static GLuint blit_screen_tex = 0;
static GLuint blit_pal_tex = 0;
static GLint  blit_loc_screen = -1;
static GLint  blit_loc_palette = -1;
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

void hwr_present_indexed(const uint8_t *px, int w, int h, int pitch,
    const uint8_t *pal6)
{
    if (!hwr_is_ready() || px == NULL || w <= 0 || h <= 0)
        return;
    if (!blit_ready && blit_init() != HWR_OK)
        return;
    /* WScreen rows are tightly packed at GraphicsScreenWidth; the present path
     * assumes pitch == w. If that ever differs we'd need GL_UNPACK_ROW_LENGTH. */
    (void)pitch;

    hwr_sync_viewport();   /* the window resizes per game video mode */
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glClear(GL_COLOR_BUFFER_BIT);

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, blit_screen_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, w, h, 0, GL_RED, GL_UNSIGNED_BYTE, px);

    if (pal6 != NULL) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, blit_pal_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, 256, 1, 0, GL_RGB,
            GL_UNSIGNED_BYTE, pal6);
    }

    glUseProgram(blit_prog);
    glUniform1i(blit_loc_screen, 0);
    glUniform1i(blit_loc_palette, 1);

    glBindVertexArray(blit_vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);

    hwr_gl_check("hwr_present_indexed");
}
