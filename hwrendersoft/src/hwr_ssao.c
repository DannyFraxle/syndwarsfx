/******************************************************************************/
// Syndicate Wars FX3D - OpenGL hardware renderer for Bullfrog titles.
/******************************************************************************/
/**                        2026 danny@fraxle.net                             **/
/******************************************************************************/
/** @file hwr_ssao.c
 *     Screen-space ambient occlusion.
 * @par Purpose:
 *     The floor and face passes render into a G-buffer (lit colour + world
 *     position) instead of the back buffer. This module then estimates per-
 *     pixel occlusion from the world-position buffer, blurs it, and composites
 *     colour*AO back to the default framebuffer for the keyed HUD overlay.
 *
 *     The game's projection is a custom isometric transform (transform_shpoint,
 *     reproduced in hwr_floor.c), so there is no ordinary projection matrix to
 *     project kernel samples with. Instead the AO pass samples neighbours in
 *     screen space and compares their *world* positions read straight from the
 *     G-buffer - the fixed near-orthographic view makes a screen-space sample
 *     radius map to a roughly constant world radius. Surface normals are
 *     reconstructed from screen-space derivatives of the world position, so no
 *     normal attribute has to be plumbed through the geometry.
 */
/******************************************************************************/
#include "hwr_api.h"
#include "hwr_gl.h"
#include "hwr_internal.h"

#include <math.h>
#include <stddef.h>

#define SSAO_KERNEL 16

/* --- Config (set from fx3d_lights.ini via hwr_ssao_config) --- */
static int   ss_enable = 0;
static float ss_radius = 0.015f;   /* screen-space sample radius (UV) */
static float ss_world  = 384.0f;   /* world-space occlusion range (units) */
static float ss_strength = 1.5f;
static float ss_bias   = 0.04f;
static int   ss_debug  = 0;

/* --- GL objects --- */
static int    ss_ready = 0;
static int    ss_w = 0, ss_h = 0;       /* current G-buffer size */
static GLuint g_fbo = 0, g_color = 0, g_pos = 0, g_depth = 0;
static GLuint ao_fbo = 0, ao_tex = 0;
static GLuint blur_fbo = 0, blur_tex = 0;
static GLuint noise_tex = 0;
static GLuint quad_vao = 0;

static GLuint ssao_prog = 0, blur_prog = 0, comp_prog = 0;

/* SSAO program uniforms. */
static GLint  u_ss_pos = -1, u_ss_noise = -1, u_ss_noisescale = -1;
static GLint  u_ss_samples = -1, u_ss_radius = -1, u_ss_world = -1;
static GLint  u_ss_bias = -1, u_ss_strength = -1, u_ss_viewdir = -1;

static float  ss_viewdir[3] = { 0.0f, 1.0f, 0.0f };
/* Blur program uniforms. */
static GLint  u_bl_ao = -1, u_bl_texel = -1;
/* Composite program uniforms. */
static GLint  u_cp_color = -1, u_cp_ao = -1, u_cp_pos = -1, u_cp_debug = -1;

static float  ss_kernel[SSAO_KERNEL * 2];

/* ---- Shaders ------------------------------------------------------------- */

static const char *quad_vert_src =
    "#version 330 core\n"
    "out vec2 vUV;\n"
    "void main(){\n"
    "    vec2 p = vec2((gl_VertexID == 1) ? 3.0 : -1.0,\n"
    "                  (gl_VertexID == 2) ? 3.0 : -1.0);\n"
    "    vUV = p * 0.5 + 0.5;\n"
    "    gl_Position = vec4(p, 0.0, 1.0);\n"
    "}\n";

static const char *ssao_frag_src =
    "#version 330 core\n"
    "in vec2 vUV;\n"
    "out float frag;\n"
    "uniform sampler2D uPosition;\n"
    "uniform sampler2D uNoise;\n"
    "uniform vec2  uNoiseScale;\n"
    "uniform vec2  uSamples[16];\n"
    "uniform vec3  uViewDir;         // world-space camera view dir (+depth)\n"
    "uniform float uRadius;\n"
    "uniform float uWorld;\n"
    "uniform float uBias;\n"
    "uniform float uStrength;\n"
    "void main(){\n"
    "    vec3 P = texture(uPosition, vUV).xyz;\n"
    "    if (dot(P, P) < 1.0) { frag = 1.0; return; }   // background\n"
    "    // Surface normal from screen-space derivatives of world position. The\n"
    "    // cross-product sign is ambiguous, so orient it toward the camera using\n"
    "    // the known view direction - valid for floor and walls alike.\n"
    "    vec3 N = normalize(cross(dFdx(P), dFdy(P)));\n"
    "    if (dot(N, uViewDir) > 0.0) N = -N;\n"
    "    vec2 rnd = normalize(texture(uNoise, vUV * uNoiseScale).xy * 2.0 - 1.0);\n"
    "    float occ = 0.0;\n"
    "    for (int i = 0; i < 16; i++) {\n"
    "        vec2 off = reflect(uSamples[i], rnd) * uRadius;\n"
    "        vec3 Q = texture(uPosition, vUV + off).xyz;\n"
    "        if (dot(Q, Q) < 1.0) continue;\n"
    "        vec3 v = Q - P;\n"
    "        float dist = length(v);\n"
    "        // Height the neighbour rises above the tangent plane, in world units.\n"
    "        // Only count it once that rise clears uBias - this ignores the floor's\n"
    "        // gentle per-tile undulation and reacts only to real raised geometry\n"
    "        // (walls, columns), so smooth ground stays unshadowed.\n"
    "        float h = dot(N, v);\n"
    "        if (dist > 1e-3 && dist < uWorld) {\n"
    "            float w = clamp((h - uBias) / max(uBias, 1.0), 0.0, 1.0);\n"
    "            occ += w * (1.0 - dist / uWorld);\n"
    "        }\n"
    "    }\n"
    "    occ = occ / 16.0;\n"
    "    frag = clamp(1.0 - occ * uStrength, 0.0, 1.0);\n"
    "}\n";

static const char *blur_frag_src =
    "#version 330 core\n"
    "in vec2 vUV;\n"
    "out float frag;\n"
    "uniform sampler2D uAO;\n"
    "uniform vec2 uTexel;\n"
    "void main(){\n"
    "    float s = 0.0;\n"
    "    for (int x = -2; x < 2; x++)\n"
    "        for (int y = -2; y < 2; y++)\n"
    "            s += texture(uAO, vUV + vec2(float(x), float(y)) * uTexel).r;\n"
    "    frag = s / 16.0;\n"
    "}\n";

static const char *comp_frag_src =
    "#version 330 core\n"
    "in vec2 vUV;\n"
    "out vec4 frag;\n"
    "uniform sampler2D uColor;\n"
    "uniform sampler2D uAO;\n"
    "uniform sampler2D uPosition;\n"
    "uniform int uDebug;\n"
    "void main(){\n"
    "    if (uDebug == 1) { frag = vec4(fract(texture(uPosition, vUV).xyz / 2048.0), 1.0); return; }\n"
    "    float ao = texture(uAO, vUV).r;\n"
    "    if (uDebug == 2 || uDebug == 3) { frag = vec4(vec3(ao), 1.0); return; }\n"
    "    vec3 c = texture(uColor, vUV).rgb;\n"
    "    frag = vec4(c * ao, 1.0);\n"
    "}\n";

/* ---- Helpers ------------------------------------------------------------- */

static GLuint ss_compile(GLenum type, const char *src)
{
    GLuint sh = glCreateShader(type);
    GLint ok = 0;
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(sh, sizeof(log), NULL, log);
        hwr_set_error("ssao shader compile failed: %s", log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static GLuint ss_link(const char *vs_src, const char *fs_src)
{
    GLuint vs, fs, prog;
    GLint ok = 0;
    vs = ss_compile(GL_VERTEX_SHADER, vs_src);
    if (!vs) return 0;
    fs = ss_compile(GL_FRAGMENT_SHADER, fs_src);
    if (!fs) { glDeleteShader(vs); return 0; }
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
        hwr_set_error("ssao program link failed: %s", log);
        return 0;
    }
    return prog;
}

/* A cheap deterministic [0,1) generator so the kernel is stable across runs. */
static float ss_randf(unsigned *state)
{
    *state = (*state * 1103515245u) + 12345u;
    return (float)((*state >> 16) & 0x7FFF) / 32768.0f;
}

static void ss_build_kernel(void)
{
    unsigned st = 0x1234567u;
    int i;
    for (i = 0; i < SSAO_KERNEL; i++) {
        /* Random direction in the unit disc, length biased toward the centre so
         * nearer samples dominate (matches a hemisphere kernel's weighting). */
        float ang = ss_randf(&st) * 6.2831853f;
        float r = ss_randf(&st);
        float scale = 0.3f + 0.7f * (r * r);   /* 0.3..1.0 */
        ss_kernel[i * 2 + 0] = cosf(ang) * scale;
        ss_kernel[i * 2 + 1] = sinf(ang) * scale;
    }
}

static void ss_make_noise(void)
{
    unsigned char px[4 * 4 * 3];
    unsigned st = 0x9E3779B1u;
    int i;
    for (i = 0; i < 4 * 4; i++) {
        px[i * 3 + 0] = (unsigned char)(ss_randf(&st) * 255.0f);
        px[i * 3 + 1] = (unsigned char)(ss_randf(&st) * 255.0f);
        px[i * 3 + 2] = 0;
    }
    glGenTextures(1, &noise_tex);
    glBindTexture(GL_TEXTURE_2D, noise_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, 4, 4, 0, GL_RGB, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
}

static int ss_init_once(void)
{
    if (ss_ready)
        return HWR_OK;

    ssao_prog = ss_link(quad_vert_src, ssao_frag_src);
    blur_prog = ss_link(quad_vert_src, blur_frag_src);
    comp_prog = ss_link(quad_vert_src, comp_frag_src);
    if (!ssao_prog || !blur_prog || !comp_prog)
        return HWR_ERROR;

    u_ss_pos        = glGetUniformLocation(ssao_prog, "uPosition");
    u_ss_noise      = glGetUniformLocation(ssao_prog, "uNoise");
    u_ss_noisescale = glGetUniformLocation(ssao_prog, "uNoiseScale");
    u_ss_samples    = glGetUniformLocation(ssao_prog, "uSamples");
    u_ss_radius     = glGetUniformLocation(ssao_prog, "uRadius");
    u_ss_world      = glGetUniformLocation(ssao_prog, "uWorld");
    u_ss_bias       = glGetUniformLocation(ssao_prog, "uBias");
    u_ss_strength   = glGetUniformLocation(ssao_prog, "uStrength");
    u_ss_viewdir    = glGetUniformLocation(ssao_prog, "uViewDir");

    u_bl_ao    = glGetUniformLocation(blur_prog, "uAO");
    u_bl_texel = glGetUniformLocation(blur_prog, "uTexel");

    u_cp_color = glGetUniformLocation(comp_prog, "uColor");
    u_cp_ao    = glGetUniformLocation(comp_prog, "uAO");
    u_cp_pos   = glGetUniformLocation(comp_prog, "uPosition");
    u_cp_debug = glGetUniformLocation(comp_prog, "uDebug");

    ss_build_kernel();
    ss_make_noise();
    glGenVertexArrays(1, &quad_vao);

    if (hwr_gl_check("ssao init"))
        return HWR_ERROR;
    ss_ready = 1;
    return HWR_OK;
}

/* Make a colour texture attachment. */
static GLuint ss_make_tex(int w, int h, GLint internal, GLenum format, GLenum type)
{
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, format, type, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

static void ss_free_targets(void)
{
    if (g_color)  { glDeleteTextures(1, &g_color); g_color = 0; }
    if (g_pos)    { glDeleteTextures(1, &g_pos); g_pos = 0; }
    if (g_depth)  { glDeleteRenderbuffers(1, &g_depth); g_depth = 0; }
    if (ao_tex)   { glDeleteTextures(1, &ao_tex); ao_tex = 0; }
    if (blur_tex) { glDeleteTextures(1, &blur_tex); blur_tex = 0; }
    if (g_fbo)    { glDeleteFramebuffers(1, &g_fbo); g_fbo = 0; }
    if (ao_fbo)   { glDeleteFramebuffers(1, &ao_fbo); ao_fbo = 0; }
    if (blur_fbo) { glDeleteFramebuffers(1, &blur_fbo); blur_fbo = 0; }
}

static int ss_resize(int w, int h)
{
    static const GLenum draw2[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
    if (w == ss_w && h == ss_h && g_fbo != 0)
        return HWR_OK;
    ss_free_targets();
    ss_w = w; ss_h = h;

    /* G-buffer: colour (RGB8) + world position (RGB32F) + depth. */
    g_color = ss_make_tex(w, h, GL_RGB8, GL_RGB, GL_UNSIGNED_BYTE);
    g_pos   = ss_make_tex(w, h, GL_RGBA32F, GL_RGBA, GL_FLOAT);   /* xyz + view depth */
    glGenRenderbuffers(1, &g_depth);
    glBindRenderbuffer(GL_RENDERBUFFER, g_depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);

    glGenFramebuffers(1, &g_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_color, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, g_pos, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g_depth);
    glDrawBuffers(2, draw2);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        hwr_set_error("ssao G-buffer incomplete");
        return HWR_ERROR;
    }

    /* AO + blur targets: single-channel. */
    ao_tex = ss_make_tex(w, h, GL_R16F, GL_RED, GL_FLOAT);
    glGenFramebuffers(1, &ao_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, ao_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ao_tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        hwr_set_error("ssao AO buffer incomplete");
        return HWR_ERROR;
    }

    blur_tex = ss_make_tex(w, h, GL_R16F, GL_RED, GL_FLOAT);
    glGenFramebuffers(1, &blur_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, blur_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, blur_tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        hwr_set_error("ssao blur buffer incomplete");
        return HWR_ERROR;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return HWR_OK;
}

/* ---- Public API ---------------------------------------------------------- */

void hwr_ssao_config(int enable, float radius, float world, float strength,
    float bias, int debug)
{
    ss_enable   = enable;
    ss_radius   = (radius > 0.0f) ? radius : 0.015f;
    ss_world    = (world > 0.0f) ? world : 384.0f;
    ss_strength = strength;
    ss_bias     = bias;
    ss_debug    = debug;
}

int hwr_ssao_active(void)
{
    return (ss_enable && ss_ready) ? 1 : 0;
}

void hwr_ssao_set_viewdir(float x, float y, float z)
{
    float len = (float)sqrt((double)(x*x + y*y + z*z));
    if (len > 1e-6f) {
        ss_viewdir[0] = x / len;
        ss_viewdir[1] = y / len;
        ss_viewdir[2] = z / len;
    }
}

void hwr_ssao_begin(int w, int h)
{
    if (!ss_enable || w <= 0 || h <= 0) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);   /* draw straight to back buffer */
        return;
    }
    if (ss_init_once() != HWR_OK || ss_resize(w, h) != HWR_OK) {
        ss_enable = 0;                          /* give up; fall back this run */
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    glViewport(0, 0, w, h);
    /* Clear to zero so the world-position attachment reads 0 on background
     * pixels - the AO pass treats position 0 as "no geometry" and skips it. */
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

static void ss_fullscreen(void)
{
    glBindVertexArray(quad_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

void hwr_ssao_resolve(void)
{
    if (!ss_enable || !ss_ready)
        return;

    glDisable(GL_DEPTH_TEST);

    /* --- SSAO pass: world-position G-buffer -> raw AO --- */
    glBindFramebuffer(GL_FRAMEBUFFER, ao_fbo);
    glViewport(0, 0, ss_w, ss_h);
    glUseProgram(ssao_prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_pos);
    glUniform1i(u_ss_pos, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, noise_tex);
    glUniform1i(u_ss_noise, 1);
    glUniform2f(u_ss_noisescale, (float)ss_w / 4.0f, (float)ss_h / 4.0f);
    glUniform2fv(u_ss_samples, SSAO_KERNEL, ss_kernel);
    glUniform1f(u_ss_radius, ss_radius);
    glUniform1f(u_ss_world, ss_world);
    glUniform1f(u_ss_bias, ss_bias);
    glUniform1f(u_ss_strength, ss_strength);
    glUniform3f(u_ss_viewdir, ss_viewdir[0], ss_viewdir[1], ss_viewdir[2]);
    ss_fullscreen();

    /* --- Blur pass: raw AO -> blurred AO --- */
    glBindFramebuffer(GL_FRAMEBUFFER, blur_fbo);
    glUseProgram(blur_prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ao_tex);
    glUniform1i(u_bl_ao, 0);
    glUniform2f(u_bl_texel, 1.0f / (float)ss_w, 1.0f / (float)ss_h);
    ss_fullscreen();

    /* --- Composite: scene colour * AO -> back buffer --- */
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glUseProgram(comp_prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_color);
    glUniform1i(u_cp_color, 0);
    glActiveTexture(GL_TEXTURE1);
    /* debug 2 shows the raw (pre-blur) AO; everything else uses the blurred AO. */
    glBindTexture(GL_TEXTURE_2D, (ss_debug == 2) ? ao_tex : blur_tex);
    glUniform1i(u_cp_ao, 1);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, g_pos);
    glUniform1i(u_cp_pos, 2);
    glUniform1i(u_cp_debug, ss_debug);
    ss_fullscreen();

    glBindVertexArray(0);
    glEnable(GL_DEPTH_TEST);
    hwr_gl_check("ssao resolve");
}
