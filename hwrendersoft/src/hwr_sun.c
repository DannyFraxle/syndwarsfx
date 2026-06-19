/******************************************************************************/
// Syndicate Wars FX3D - OpenGL hardware renderer for Bullfrog titles.
/******************************************************************************/
/**                        2026 danny@fraxle.net                             **/
/******************************************************************************/
/** @file hwr_sun.c
 *     Directional sun + shadow map.
 * @par Purpose:
 *     A directional "sun" renders the scene depth from its viewpoint each
 *     frame into a 2048² shadow map (depth-only FBO).  The main floor/face
 *     pass then projects each fragment into sun-space and compares depth to
 *     decide lit vs shadowed; PCF softens the edges.
 *
 *     The sun's contribution becomes the base lighting: lit ground ≈
 *     sun_bright, shadowed ground falls to sun_ambient.  Lamp pools are
 *     layered on top in fl_prog, exactly as before.
 *
 *     Coordinate space: geometry is submitted in world space (HwrVertex.x/y/z:
 *     XZ = tile<<8, Y = 8×alt).  The shadow map uses an orthographic
 *     projection from the sun's viewpoint — fully independent of the game's
 *     isometric transform_shpoint.
 */
/******************************************************************************/
#include "hwr_api.h"
#include "hwr_gl.h"
#include "hwr_internal.h"
#include "hwr_scene_source.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

/* ---- Config (set from fx3d_lights.ini via hwr_sun_config) --------------- */
static int   sn_enable    = 0;
static float sn_bright    = 0.08f;   /* dim dystopian lit ground */
static float sn_ambient   = 0.0f;    /* pure black shadows */
static float sn_azimuth   = 315.0f;   /* compass degrees: NW */
static float sn_elevation = 35.0f;    /* degrees above horizon */
static int   sn_pcf       = 2;        /* PCF half-radius in texels */
static float sn_bias      = 0.0005f;  /* small constant shader bias (post-offset) */
static float sn_slope     = 2.0f;     /* glPolygonOffset factor */
static float sn_units     = 4.0f;     /* glPolygonOffset units */
static int   sn_debug     = 0;
static float sn_haze      = 0.0f;     /* crisp shadow edges */

/* ---- GL objects --------------------------------------------------------- */
static int    sn_ready   = 0;
static GLuint sn_fbo     = 0;
static GLuint sn_depthtex= 0;
#define SN_MAP_SIZE 2048

/* Depth-only program for the shadow pass. */
static GLuint sn_prog    = 0;
static GLint  sn_u_mvp   = -1;

/* Geometry buffers (position-only; mirrors fl_vao layout but only attr 0). */
static GLuint sn_vao     = 0;
static GLuint sn_vbo     = 0;
static GLuint sn_ebo     = 0;

/* Cached sun MVP matrix (column-major, uploaded to fl_prog every frame). */
static float sn_mvp[16];

/* ---- Math helpers ------------------------------------------------------- */

static void mat4_identity(float *m)
{
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

/* Column-major 4x4 multiply: dst = a * b. */
static void mat4_mul(float *dst, const float *a, const float *b)
{
    int i, j, k;
    float tmp[16];
    for (j = 0; j < 4; j++)
        for (i = 0; i < 4; i++) {
            float s = 0.0f;
            for (k = 0; k < 4; k++)
                s += a[k*4+i] * b[j*4+k];
            tmp[j*4+i] = s;
        }
    memcpy(dst, tmp, sizeof(tmp));
}

/* Column-major orthographic projection (maps xyz to NDC). */
static void mat4_ortho(float *m,
    float l, float r, float b, float t, float n, float f)
{
    mat4_identity(m);
    m[0]  =  2.0f / (r - l);
    m[5]  =  2.0f / (t - b);
    m[10] = -2.0f / (f - n);
    m[12] = -(r + l) / (r - l);
    m[13] = -(t + b) / (t - b);
    m[14] = -(f + n) / (f - n);
}

/* Column-major lookAt (right-handed).
 * eye, center, up are float[3]. */
static void mat4_lookat(float *m,
    const float *eye, const float *center, const float *up)
{
    float fx = center[0] - eye[0];
    float fy = center[1] - eye[1];
    float fz = center[2] - eye[2];
    float flen = (float)sqrt((double)(fx*fx + fy*fy + fz*fz));
    if (flen < 1e-6f) flen = 1.0f;
    fx /= flen; fy /= flen; fz /= flen;

    /* right = forward × up */
    float rx = fy*up[2] - fz*up[1];
    float ry = fz*up[0] - fx*up[2];
    float rz = fx*up[1] - fy*up[0];
    float rlen = (float)sqrt((double)(rx*rx + ry*ry + rz*rz));
    if (rlen < 1e-6f) rlen = 1.0f;
    rx /= rlen; ry /= rlen; rz /= rlen;

    /* true up = right × forward */
    float ux = ry*fz - rz*fy;
    float uy = rz*fx - rx*fz;
    float uz = rx*fy - ry*fx;

    mat4_identity(m);
    m[0]  =  rx;  m[4]  =  ry;  m[8]  =  rz;
    m[1]  =  ux;  m[5]  =  uy;  m[9]  =  uz;
    m[2]  = -fx;  m[6]  = -fy;  m[10] = -fz;
    m[12] = -(rx*eye[0] + ry*eye[1] + rz*eye[2]);
    m[13] = -(ux*eye[0] + uy*eye[1] + uz*eye[2]);
    m[14] =  (fx*eye[0] + fy*eye[1] + fz*eye[2]);
}

/* ---- Shaders ------------------------------------------------------------ */

static const char *sn_vert_src =
    "#version 330 core\n"
    "layout(location=0) in vec3 aPos;\n"
    "uniform mat4 uSunMVP;\n"
    "void main(){\n"
    "    gl_Position = uSunMVP * vec4(aPos, 1.0);\n"
    "}\n";

static const char *sn_frag_src =
    "#version 330 core\n"
    "void main(){}\n";

/* ---- Init --------------------------------------------------------------- */

static GLuint sn_compile(GLenum type, const char *src)
{
    GLuint sh = glCreateShader(type);
    GLint ok = 0;
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(sh, sizeof(log), NULL, log);
        hwr_set_error("sun shader compile failed: %s", log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static int sn_init(void)
{
    GLuint vs, fs;
    GLint ok = 0;

    /* --- Depth-only program --- */
    vs = sn_compile(GL_VERTEX_SHADER,   sn_vert_src);
    if (!vs) return HWR_ERROR;
    fs = sn_compile(GL_FRAGMENT_SHADER, sn_frag_src);
    if (!fs) { glDeleteShader(vs); return HWR_ERROR; }
    sn_prog = glCreateProgram();
    glAttachShader(sn_prog, vs);
    glAttachShader(sn_prog, fs);
    glLinkProgram(sn_prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    glGetProgramiv(sn_prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(sn_prog, sizeof(log), NULL, log);
        hwr_set_error("sun program link failed: %s", log);
        return HWR_ERROR;
    }
    sn_u_mvp = glGetUniformLocation(sn_prog, "uSunMVP");

    /* --- Shadow-map depth FBO --- */
    glGenTextures(1, &sn_depthtex);
    glBindTexture(GL_TEXTURE_2D, sn_depthtex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24,
        SN_MAP_SIZE, SN_MAP_SIZE, 0,
        GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, &sn_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, sn_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
        GL_TEXTURE_2D, sn_depthtex, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        hwr_set_error("sun shadow FBO incomplete");
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return HWR_ERROR;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    /* --- Position-only VAO/VBO/EBO for depth submission --- */
    glGenVertexArrays(1, &sn_vao);
    glBindVertexArray(sn_vao);
    glGenBuffers(1, &sn_vbo);
    glGenBuffers(1, &sn_ebo);
    glBindBuffer(GL_ARRAY_BUFFER, sn_vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, sn_ebo);
    /* Only position (attr 0) at the same stride as HwrVertex so the VBO data
     * can be uploaded straight from the source batch. */
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE,
        (GLsizei)sizeof(HwrVertex),
        (void *)offsetof(HwrVertex, x));
    glBindVertexArray(0);

    if (hwr_gl_check("sn_init"))
        return HWR_ERROR;

    sn_ready = 1;
    return HWR_OK;
}

/* ---- Shadow pass -------------------------------------------------------- */

/* Compute sun_mvp for the current camera position. */
static void sn_compute_mvp(float cx, float cz)
{
    /* Direction TOWARD the sun in world space (where the sun sits in the sky).
     * Azimuth: 0 = north (+Z), 90 = east (+X), going clockwise.
     * Elevation: 0 = horizon, 90 = straight up (dy = sinE > 0 means up). */
    const float deg2rad = 3.14159265f / 180.0f;
    float az  = sn_azimuth  * deg2rad;
    float el  = sn_elevation * deg2rad;
    float cosE = (float)cos((double)el);
    float sinE = (float)sin((double)el);
    /* +X = east, +Z = south in Syndicate world space (XZ = tile<<8). */
    float dx =  cosE * (float)sin((double)az);
    float dy =  sinE;
    float dz =  cosE * (float)cos((double)az);

    /* The shadow camera sits AT the sun (above the scene, along +dir) and looks
     * back down toward the ground focus.  D must exceed the furthest geometry
     * from the centre so the whole visible region stays in front of the eye. */
    float D = 16384.0f;
    float center[3] = { cx, 0.0f, cz };
    float eye[3]    = { center[0] + dx*D, center[1] + dy*D, center[2] + dz*D };

    /* Choose an up vector; if the sun is nearly vertical use +X. */
    float up[3] = { 0.0f, 1.0f, 0.0f };
    if ((float)fabs((double)dy) > 0.99f) { up[0] = 1.0f; up[1] = 0.0f; }

    float view[16], proj[16];
    mat4_lookat(view, eye, center, up);

    /* Ortho extents cover ±8192 units in XZ and full building height depth. */
    float half = 8192.0f;
    mat4_ortho(proj, -half, half, -half, half, 1.0f, 2.0f * D + half);

    mat4_mul(sn_mvp, proj, view);
}

/* Draw one geometry batch through the depth-only program. */
static void sn_draw_batch(const HwrGeometryBatch *batch)
{
    glBindVertexArray(sn_vao);
    glBindBuffer(GL_ARRAY_BUFFER, sn_vbo);
    glBufferData(GL_ARRAY_BUFFER,
        (GLsizeiptr)batch->vert_count * (GLsizeiptr)sizeof(HwrVertex),
        batch->verts, GL_STREAM_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, sn_ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
        (GLsizeiptr)batch->index_count * (GLsizeiptr)sizeof(uint32_t),
        batch->indices, GL_STREAM_DRAW);
    glDrawElements(GL_TRIANGLES, batch->index_count, GL_UNSIGNED_INT, (void *)0);
    glBindVertexArray(0);
}

/* ---- Public API --------------------------------------------------------- */

void hwr_sun_config(int enable, float brightness, float ambient,
    float azimuth, float elevation, int pcf,
    float bias, float slope, float units, int debug, float haze)
{
    sn_enable    = enable;
    sn_bright    = (brightness >= 0.0f && brightness <= 1.0f) ? brightness : 0.30f;
    sn_ambient   = (ambient   >= 0.0f && ambient   <= 1.0f) ? ambient   : 0.05f;
    sn_azimuth   = azimuth;
    sn_elevation = (elevation >= 0.0f && elevation <= 90.0f) ? elevation : 40.0f;
    sn_pcf       = (pcf >= 0 && pcf <= 12) ? pcf : 6;
    sn_bias      = bias;
    sn_slope     = (slope >= 0.0f) ? slope : 2.0f;
    sn_units     = (units >= 0.0f) ? units : 4.0f;
    sn_debug     = debug ? 1 : 0;
    sn_haze      = (haze >= 0.0f && haze <= 1.0f) ? haze : 0.35f;
}

void hwr_sun_shadow_pass(void)
{
    HwrCamera cam;
    HwrGeometryBatch floor_batch, face_batch;
    const HwrSceneSource *s = hwr_source;
    int have_floor, have_faces;

    if (!sn_enable || s == NULL)
        return;
    if (s->get_camera == NULL || s->get_floor == NULL)
        return;

    /* Init once. On failure disable and bail so we don't spam errors. */
    if (!sn_ready) {
        if (sn_init() != HWR_OK) {
            sn_enable = 0;
            return;
        }
    }

    if (s->get_camera(s->ctx, &cam) != 0)
        return;

    sn_compute_mvp(cam.cx, cam.cz);

    /* Gather geometry. Both batches are static buffers in source_sw.c that
     * stay valid for the whole frame, so calling the getters again is safe. */
    have_floor = (s->get_floor(s->ctx, &floor_batch) > 0 &&
                  floor_batch.index_count > 0);
    have_faces = (s->get_faces != NULL &&
                  s->get_faces(s->ctx, &face_batch) > 0 &&
                  face_batch.index_count > 0);

    if (!have_floor && !have_faces)
        return;

    /* --- Bind shadow FBO and render depth --- */
    glBindFramebuffer(GL_FRAMEBUFFER, sn_fbo);
    glViewport(0, 0, SN_MAP_SIZE, SN_MAP_SIZE);
    glClear(GL_DEPTH_BUFFER_BIT);

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(sn_slope, sn_units);
    glUseProgram(sn_prog);
    glUniformMatrix4fv(sn_u_mvp, 1, GL_FALSE, sn_mvp);

    if (have_floor)
        sn_draw_batch(&floor_batch);
    if (have_faces)
        sn_draw_batch(&face_batch);

    glDisable(GL_POLYGON_OFFSET_FILL);

    /* --- Restore state for the main passes --- */
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    hwr_sync_viewport();   /* critical when SSAO is disabled (ssao_begin won't reset it) */

    hwr_gl_check("hwr_sun_shadow_pass");
}

const float *hwr_sun_mvp(void)
{
    return sn_mvp;
}

unsigned int hwr_sun_texture(void)
{
    return sn_depthtex;
}

int hwr_sun_enabled(void)
{
    return sn_enable && sn_ready;
}

float hwr_sun_bright(void)   { return sn_bright;  }
float hwr_sun_ambient(void)  { return sn_ambient; }
int   hwr_sun_pcf(void)      { return sn_pcf;     }
float hwr_sun_bias(void)     { return sn_bias;    }
int   hwr_sun_debug(void)    { return sn_debug;   }
float hwr_sun_haze(void)     { return sn_haze;    }
