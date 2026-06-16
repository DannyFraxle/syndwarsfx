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

#include <stddef.h>

/* Reproduces transform_shpoint() per-vertex, including the mode-5 perspective
 * foreshortening (which no single matrix can express). */
static const char *floor_vert_src =
    "#version 330 core\n"
    "layout(location=0) in vec3 aPos;\n"
    "layout(location=1) in vec2 aUV;\n"
    "layout(location=2) in float aDepth;\n"  /* per-tile constant scrd */
    "layout(location=3) in uint aPage;\n"
    "layout(location=4) in float aLight;\n"
    "uniform float uD10, uD14, uD18, uD1C;\n"
    "uniform float uScale;\n"
    "uniform vec2 uCentre;   // D3C, D40\n"
    "uniform vec3 uCtr;      // camera centre: cx, 8*yc, cz\n"
    "uniform int  uPersp;\n"
    "out vec3 vUV;\n"
    "out float vLight;\n"
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
    "    vLight = aLight;\n"
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
    "in float vLight;\n"
    "out vec4 frag;\n"
    "uniform sampler2DArray uTex;   // R8 palette indices\n"
    "uniform sampler2D uPalette;    // RGB8 256x1, active 8-bit palette\n"
    "uniform int uTransKey;         // texel index to treat as transparent (<0 = none)\n"
    "void main(){\n"
    "    if (vUV.z > 254.5) {       // flat-shaded face (Texture==0), no texture\n"
    "        frag = vec4(vec3(0.55) * vLight, 1.0);\n"
    "        return;\n"
    "    }\n"
    "    int idx = int(texture(uTex, vUV).r * 255.0 + 0.5);\n"
    "    if (uTransKey >= 0 && idx == uTransKey)\n"
    "        discard;               // see through windows/grates to faces behind\n"
    "    vec3 c = texture(uPalette, vec2((float(idx) + 0.5) / 256.0, 0.5)).rgb;\n"
    "    frag = vec4(c * vLight, 1.0);\n"
    "}\n";

static GLuint fl_prog = 0;
static GLuint fl_vao = 0, fl_vbo = 0, fl_ebo = 0;
static GLuint fl_tex = 0, fl_pal = 0;
static GLint  fl_loc_tex = -1, fl_loc_pal = -1, fl_loc_transkey = -1;
static GLint  fl_loc_d10 = -1, fl_loc_d14 = -1, fl_loc_d18 = -1, fl_loc_d1c = -1;
static GLint  fl_loc_scale = -1, fl_loc_centre = -1, fl_loc_ctr = -1, fl_loc_persp = -1;
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
    fl_draw_batch(&batch);

    hwr_gl_check("hwr_faces_render");
    return 1;
}

/** Drop cached GPU art (e.g. on level change). */
void hwr_floor_reset(void)
{
    fl_pages_uploaded = 0;
}
