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

/* --- Water screen-space reflection ([water] section) --- */
static int   ss_refl_enable = 0;
static float ss_refl_strength = 0.25f;
static float ss_refl_sky[3] = { 0.10f, 0.13f, 0.18f };  /* dark blue/grey */
static int   ss_refl_debug = 0;   /* colour water by ray outcome */
static float ss_refl_blur = 2.0f; /* reflection blur reach in pixels (0 = sharp) */
/* Camera projection factors (transform_shpoint), fed each frame so the composite
 * can project marched world points back to screen UVs. */
static float ss_cam_d10, ss_cam_d14, ss_cam_d18, ss_cam_d1c;
static float ss_cam_scale, ss_cam_cx, ss_cam_cy8, ss_cam_cz;
static float ss_cam_ctrx, ss_cam_ctry;   /* screen centre */
static int   ss_cam_persp;
static float ss_cam_eye[3];              /* reconstructed camera eye (world) */

/* --- GL objects --- */
static int    ss_ready = 0;
static int    ss_w = 0, ss_h = 0;       /* current G-buffer size */
static GLuint g_fbo = 0, g_color = 0, g_pos = 0, g_depth = 0;
static GLuint ao_fbo = 0, ao_tex = 0;
static GLuint blur_fbo = 0, blur_tex = 0;
static GLuint noise_tex = 0;
static GLuint quad_vao = 0;

static GLuint ssao_prog = 0, blur_prog = 0, comp_prog = 0;
static GLuint refl_prog = 0, reflblur_prog = 0;
static GLuint refl_fbo = 0, refl_tex = 0;
static GLuint reflblur_fbo = 0, reflblur_tex = 0;

/* SSAO program uniforms. */
static GLint  u_ss_pos = -1, u_ss_noise = -1, u_ss_noisescale = -1;
static GLint  u_ss_samples = -1, u_ss_radius = -1, u_ss_world = -1;
static GLint  u_ss_bias = -1, u_ss_strength = -1, u_ss_viewdir = -1;

static float  ss_viewdir[3] = { 0.0f, 1.0f, 0.0f };
/* Blur program uniforms. */
static GLint  u_bl_ao = -1, u_bl_texel = -1;
/* Composite program uniforms. */
static GLint  u_cp_color = -1, u_cp_ao = -1, u_cp_pos = -1, u_cp_debug = -1;
static GLint  u_cp_reflblur = -1, u_cp_useao = -1;
static GLint  u_cp_refl = -1, u_cp_reflstr = -1, u_cp_sky = -1, u_cp_refldbg = -1;
/* Reflection pass uniforms. */
static GLint  u_rf_color = -1, u_rf_pos = -1, u_rf_refl = -1, u_rf_sky = -1;
static GLint  u_rf_viewdir = -1, u_rf_eye = -1, u_rf_refldbg = -1;
static GLint  u_rf_d10 = -1, u_rf_d14 = -1, u_rf_d18 = -1, u_rf_d1c = -1;
static GLint  u_rf_scale = -1, u_rf_ctr = -1, u_rf_centre = -1, u_rf_persp = -1;
/* Reflection blur uniforms. */
static GLint  u_rb_refl = -1, u_rb_texel = -1, u_rb_radius = -1;

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

/* Water reflection pass. Ray-marches the reflection for every water pixel into
 * its OWN buffer (rgb = reflected colour, a = confidence) so the result can then
 * be blurred. Blurring the result is the only way to soften the hard polygon
 * edges: the hit test is binary per pixel (it either finds geometry or falls to
 * sky), so no amount of inline maths produces a soft boundary. */
static const char *refl_frag_src =
    "#version 330 core\n"
    "in vec2 vUV;\n"
    "out vec4 frag;                 // rgb = reflected colour, a = confidence\n"
    "uniform sampler2D uColor;\n"
    "uniform sampler2D uPosition;   // xyz world pos, w = water mask (1=water)\n"
    "// --- water screen-space reflection ---\n"
    "uniform int   uRefl;           // 1 = reflect water pixels\n"
    "uniform int   uReflDebug;      // 1 = colour water by ray outcome\n"
    "uniform vec3  uSky;            // sky fallback colour (no geometry hit)\n"
    "uniform vec3  uViewDir;        // world view dir, into screen (+depth)\n"
    "uniform vec3  uEye;            // reconstructed camera eye (world)\n"
    "uniform float uD10, uD14, uD18, uD1C;\n"
    "uniform float uScale;\n"
    "uniform vec3  uCtr;            // camera centre cx, 8*yc, cz\n"
    "uniform vec2  uCentre;         // screen centre (px)\n"
    "uniform int   uPersp;\n"
    "// Project a world point to screen UV, reproducing transform_shpoint exactly\n"
    "// (see floor_vert_src). Returns UV; writes the (perspective-warped) view depth.\n"
    "vec2 world_to_uv(vec3 P, out float scrd){\n"
    "    float dx = P.x - uCtr.x, dy = P.y - uCtr.y, dz = P.z - uCtr.z;\n"
    "    float fa = (uD14*dx - uD10*dz) / 65536.0;\n"
    "    float fb = (uD10*dx + uD14*dz) / 65536.0;\n"
    "    float fc = (uD1C*dy - uD18*fb) / 65536.0;\n"
    "    float s  = (uD18*dy + uD1C*fb) / 65536.0;\n"
    "    if (uPersp == 5 && s > 1024.0) s = 16384.0*s/(s + 16384.0);\n"
    "    float shx = uScale*fa / 2048.0;\n"
    "    float shy = uScale*fc / 2048.0;\n"
    "    if (uPersp == 5) { shx *= (16384.0 - s)/16384.0; shy *= (16384.0 - s)/16384.0; }\n"
    "    float sx = uCentre.x + shx;\n"
    "    float sy = uCentre.y - shy;\n"
    "    scrd = s;\n"
    "    return vec2(sx/(2.0*uCentre.x), 1.0 - sy/(2.0*uCentre.y));\n"
    "}\n"
    "float view_depth(vec3 P){ return dot(P - uCtr, uViewDir); }\n"
    "void main(){\n"
    "    frag = vec4(uSky, 0.0);               // default: sky, zero confidence\n"
    "    if (uRefl == 1) {\n"
    "        vec4 pw = texture(uPosition, vUV);\n"
    "        if (pw.w > 0.5) {                     // this pixel is water\n"
    "            vec3 P = pw.xyz;\n"
    "            // PER-PIXEL incident ray from the real camera eye. This must NOT be\n"
    "            // the constant screen-centre direction: with a shared direction every\n"
    "            // pixel points the same way, so distant water merely has to march\n"
    "            // further to run into the trees - sprites then reflect everywhere and\n"
    "            // smear down the water even where they physically shouldn't appear.\n"
    "            // The per-pixel ray gives foreground water its true steep angle, so it\n"
    "            // correctly shoots past the trees and returns sky.\n"
    "            vec3 I = normalize(P - uEye);\n"
    "            vec3 R = reflect(I, vec3(0.0, 1.0, 0.0));        // mirror about up\n"
    "            vec3 refl = uSky;\n"
    "            float conf = 0.0;\n"
    "            int reason = 0;                    // 0 runout, 1 sc<=0, 2 offscreen, 3 hit, 4 too-thick\n"
    "            const float STEP = 90.0;\n"
    "            const int STEPS = 96;              // long reach - foreground hits are long rays\n"
    "            // NO per-pixel jitter. It was added to break step-banding, but the\n"
    "            // binary refinement below already lands 3D surfaces on their exact\n"
    "            // crossing, so it is redundant there - and it is actively harmful on\n"
    "            // sprites, which deliberately skip refinement: a random per-pixel\n"
    "            // offset sends neighbouring water pixels to different points on a\n"
    "            // coplanar billboard, fragmenting one streak into split pieces.\n"
    "            vec3 Pm = P;\n"
    "            vec3 Pprev = Pm;\n"
    "            for (int i = 0; i < STEPS; i++) {\n"
    "                Pm += R * STEP;\n"
    "                float sc;\n"
    "                vec2 uv = world_to_uv(Pm, sc);\n"
    "                // Near plane is the CAMERA eye at scrd = -16384, NOT the\n"
    "                // mid-screen look-at point at scrd = 0. scrd in (-16384, 0) is\n"
    "                // the visible UPPER half of the screen (where the trees are) -\n"
    "                // culling at 0 threw all of it away, which was the whole cutoff.\n"
    "                if (sc <= -16300.0) { reason = 1; break; }\n"
    "                if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) { reason = 2; break; }\n"
    "                vec4 g = texture(uPosition, uv);\n"
    "                if (dot(g.xyz, g.xyz) < 1.0) { Pprev = Pm; continue; }\n"
    "                float dm = view_depth(Pm);\n"
    "                float dg = view_depth(g.xyz);\n"
    "                if (dm > dg - 60.0) {\n"
    "                    if (g.w < -0.5) {\n"
    "                        // SPRITE / billboard: a camera-facing flat sheet, not a\n"
    "                        // real surface. If the ray runs nearly PARALLEL to the\n"
    "                        // sheet it rides it for many steps, each of which looks\n"
    "                        // like a valid crossing - stamping a repeating chain of\n"
    "                        // copies (the hoop/tube artifact). Measure how fast the\n"
    "                        // ray is closing on the sheet; if it is skimming rather\n"
    "                        // than crossing, reject and keep marching past it.\n"
    "                        float scp; vec2 uvp = world_to_uv(Pprev, scp);\n"
    "                        float dgp = view_depth(texture(uPosition, uvp).xyz);\n"
    "                        float closing = abs((dm - dg) - (view_depth(Pprev) - dgp));\n"
    "                        if (closing < 25.0) { Pprev = Pm; continue; }  // riding it - skip\n"
    "                        if (dm < dg + 130.0) {\n"
    "                            refl = texture(uColor, uv).rgb;\n"
    "                            vec2 e = min(uv, 1.0 - uv);\n"
    "                            conf = clamp(min(e.x, e.y) / 0.08, 0.0, 1.0);\n"
    "                            reason = 3;\n"
    "                        } else { reason = 4; }\n"
    "                        break;\n"
    "                    }\n"
    "                    // Real 3D surface: binary-refine the crossing between Pprev\n"
    "                    // and Pm so the hit is exact instead of snapped to the step.\n"
    "                    vec3 a = Pprev, b = Pm;\n"
    "                    for (int k = 0; k < 6; k++) {\n"
    "                        vec3 mid = 0.5 * (a + b);\n"
    "                        float scm; vec2 uvm = world_to_uv(mid, scm);\n"
    "                        float dgm = view_depth(texture(uPosition, uvm).xyz);\n"
    "                        if (view_depth(mid) > dgm - 20.0) b = mid; else a = mid;\n"
    "                    }\n"
    "                    float sh; vec2 uvh = world_to_uv(b, sh);\n"
    "                    vec4 gh = texture(uPosition, uvh);\n"
    "                    float dmh = view_depth(b);\n"
    "                    float dgh = view_depth(gh.xyz);\n"
    "                    if (dot(gh.xyz, gh.xyz) >= 1.0 && dmh < dgh + 220.0) {\n"
    "                        refl = texture(uColor, uvh).rgb;\n"
    "                        vec2  e = min(uvh, 1.0 - uvh);\n"
    "                        conf = clamp(min(e.x, e.y) / 0.08, 0.0, 1.0);\n"
    "                        reason = 3;\n"
    "                    } else { reason = 4; }\n"
    "                    break;\n"
    "                }\n"
    "                Pprev = Pm;\n"
    "            }\n"
    "            if (uReflDebug == 1) {             // diagnostic: colour water by outcome\n"
    "                vec3 dbg;\n"
    "                if      (reason == 3) dbg = vec3(0.0, 1.0, 0.0);   // green = hit\n"
    "                else if (reason == 1) dbg = vec3(1.0, 0.0, 0.0);   // red   = behind camera\n"
    "                else if (reason == 2) dbg = vec3(0.0, 0.0, 1.0);   // blue  = off screen\n"
    "                else if (reason == 4) dbg = vec3(1.0, 0.0, 1.0);   // magenta = hit too thick\n"
    "                else                  dbg = vec3(1.0, 1.0, 0.0);   // yellow = ran out of steps\n"
    "                frag = vec4(dbg, 1.0); return;\n"
    "            }\n"
    "            frag = vec4(refl, conf);\n"
    "        }\n"
    "    }\n"
    "}\n";

/* Blur the reflection buffer. This is what turns the binary per-pixel hit/miss
 * boundary into a smooth watery reflection instead of hard polygon edges. The
 * kernel is deliberately wider vertically than horizontally: real water smears
 * reflections along the view direction, and our artifacts (stepped hits, thin
 * grazing streaks) are predominantly vertical too. Confidence blurs with the
 * colour, so the reflection also fades in/out smoothly at its edges. */
static const char *reflblur_frag_src =
    "#version 330 core\n"
    "in vec2 vUV;\n"
    "out vec4 frag;\n"
    "uniform sampler2D uRefl;\n"
    "uniform vec2  uTexel;          // 1/width, 1/height\n"
    "uniform float uRadius;         // blur reach in pixels (0 = passthrough)\n"
    "void main(){\n"
    "    if (uRadius <= 0.0) { frag = texture(uRefl, vUV); return; }\n"
    "    vec4 sum = vec4(0.0);\n"
    "    float wsum = 0.0;\n"
    "    for (int y = -3; y <= 3; y++) {\n"
    "        for (int x = -2; x <= 2; x++) {\n"
    "            vec2 off = vec2(float(x), float(y) * 1.6) * uTexel * uRadius;\n"
    "            float w = exp(-0.35 * float(x*x + y*y));\n"
    "            sum += texture(uRefl, vUV + off) * w;\n"
    "            wsum += w;\n"
    "        }\n"
    "    }\n"
    "    frag = sum / max(wsum, 1e-4);\n"
    "}\n";

/* Final composite: scene colour * AO, with the blurred reflection blended over
 * water pixels. */
static const char *comp_frag_src =
    "#version 330 core\n"
    "in vec2 vUV;\n"
    "out vec4 frag;\n"
    "uniform sampler2D uColor;\n"
    "uniform sampler2D uAO;\n"
    "uniform sampler2D uPosition;   // xyz world pos, w = water mask (1=water)\n"
    "uniform sampler2D uReflBlur;   // blurred reflection: rgb + confidence\n"
    "uniform int   uDebug;\n"
    "uniform int   uUseAO;          // 0 = reflection-only run, hold AO at 1.0\n"
    "uniform int   uRefl;\n"
    "uniform int   uReflDebug;\n"
    "uniform float uReflStr;\n"
    "uniform vec3  uSky;\n"
    "void main(){\n"
    "    if (uDebug == 1) { frag = vec4(fract(texture(uPosition, vUV).xyz / 2048.0), 1.0); return; }\n"
    "    float ao = (uUseAO == 1) ? texture(uAO, vUV).r : 1.0;\n"
    "    if (uDebug == 2 || uDebug == 3) { frag = vec4(vec3(ao), 1.0); return; }\n"
    "    vec3 c = texture(uColor, vUV).rgb;\n"
    "    if (uRefl == 1 && texture(uPosition, vUV).w > 0.5) {\n"
    "        vec4 rb = texture(uReflBlur, vUV);\n"
    "        if (uReflDebug == 1) { frag = vec4(rb.rgb, 1.0); return; }\n"
    "        vec3 rcol = mix(uSky, rb.rgb, clamp(rb.a, 0.0, 1.0));\n"
    "        c = mix(c, rcol, uReflStr);\n"
    "    }\n"
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
    refl_prog = ss_link(quad_vert_src, refl_frag_src);
    reflblur_prog = ss_link(quad_vert_src, reflblur_frag_src);
    if (!ssao_prog || !blur_prog || !comp_prog || !refl_prog || !reflblur_prog)
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
    u_cp_useao    = glGetUniformLocation(comp_prog, "uUseAO");   /* AO on/off flag */
    u_cp_refl     = glGetUniformLocation(comp_prog, "uRefl");
    u_cp_reflstr  = glGetUniformLocation(comp_prog, "uReflStr");
    u_cp_sky      = glGetUniformLocation(comp_prog, "uSky");
    u_cp_refldbg  = glGetUniformLocation(comp_prog, "uReflDebug");
    u_cp_reflblur = glGetUniformLocation(comp_prog, "uReflBlur");

    u_rf_color   = glGetUniformLocation(refl_prog, "uColor");
    u_rf_pos     = glGetUniformLocation(refl_prog, "uPosition");
    u_rf_refl    = glGetUniformLocation(refl_prog, "uRefl");
    u_rf_sky     = glGetUniformLocation(refl_prog, "uSky");
    u_rf_viewdir = glGetUniformLocation(refl_prog, "uViewDir");
    u_rf_eye     = glGetUniformLocation(refl_prog, "uEye");
    u_rf_refldbg = glGetUniformLocation(refl_prog, "uReflDebug");
    u_rf_d10   = glGetUniformLocation(refl_prog, "uD10");
    u_rf_d14   = glGetUniformLocation(refl_prog, "uD14");
    u_rf_d18   = glGetUniformLocation(refl_prog, "uD18");
    u_rf_d1c   = glGetUniformLocation(refl_prog, "uD1C");
    u_rf_scale = glGetUniformLocation(refl_prog, "uScale");
    u_rf_ctr   = glGetUniformLocation(refl_prog, "uCtr");
    u_rf_centre = glGetUniformLocation(refl_prog, "uCentre");
    u_rf_persp = glGetUniformLocation(refl_prog, "uPersp");

    u_rb_refl   = glGetUniformLocation(reflblur_prog, "uRefl");
    u_rb_texel  = glGetUniformLocation(reflblur_prog, "uTexel");
    u_rb_radius = glGetUniformLocation(reflblur_prog, "uRadius");

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
    if (refl_tex)     { glDeleteTextures(1, &refl_tex); refl_tex = 0; }
    if (reflblur_tex) { glDeleteTextures(1, &reflblur_tex); reflblur_tex = 0; }
    if (refl_fbo)     { glDeleteFramebuffers(1, &refl_fbo); refl_fbo = 0; }
    if (reflblur_fbo) { glDeleteFramebuffers(1, &reflblur_fbo); reflblur_fbo = 0; }
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

    /* Water reflection + its blur (RGBA16F: rgb reflected colour, a confidence).
     * Linear filtering so the blur taps interpolate smoothly. */
    refl_tex = ss_make_tex(w, h, GL_RGBA16F, GL_RGBA, GL_FLOAT);
    glBindTexture(GL_TEXTURE_2D, refl_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glGenFramebuffers(1, &refl_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, refl_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, refl_tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        hwr_set_error("water reflection buffer incomplete");
        return HWR_ERROR;
    }

    reflblur_tex = ss_make_tex(w, h, GL_RGBA16F, GL_RGBA, GL_FLOAT);
    glBindTexture(GL_TEXTURE_2D, reflblur_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glGenFramebuffers(1, &reflblur_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, reflblur_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, reflblur_tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        hwr_set_error("water reflection blur buffer incomplete");
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

/* The G-buffer/composite path runs when SSAO OR water reflection is enabled. */
static int ss_effective(void)
{
    return (ss_enable || ss_refl_enable) ? 1 : 0;
}

void hwr_ssao_reflect_config(int enable, float strength,
    float sky_r, float sky_g, float sky_b, int debug, float blur)
{
    ss_refl_enable   = enable ? 1 : 0;
    ss_refl_strength = (strength < 0.0f) ? 0.0f : (strength > 1.0f ? 1.0f : strength);
    ss_refl_sky[0] = sky_r;
    ss_refl_sky[1] = sky_g;
    ss_refl_sky[2] = sky_b;
    ss_refl_debug = debug ? 1 : 0;
    ss_refl_blur  = (blur < 0.0f) ? 0.0f : blur;
}

void hwr_ssao_set_camera(float d10, float d14, float d18, float d1c,
    float scale, float centre_x, float centre_y,
    float cx, float cy8, float cz, int perspective)
{
    ss_cam_d10 = d10; ss_cam_d14 = d14; ss_cam_d18 = d18; ss_cam_d1c = d1c;
    ss_cam_scale = scale;
    ss_cam_ctrx = centre_x; ss_cam_ctry = centre_y;
    ss_cam_cx = cx; ss_cam_cy8 = cy8; ss_cam_cz = cz;
    ss_cam_persp = perspective;

    /* Reconstruct the camera eye in world space so the reflection can use a
     * PER-PIXEL incident ray (P - eye) instead of one constant screen-centre
     * direction. transform_shpoint's screen scale works out to 16384/(scrd+16384)
     * (a pinhole with focal 16384 scrd-units), so the eye sits at scrd = -16384.
     * scrd's world gradient is v/65536^2 with v = (d1c*d10, d18*65536, d1c*d14)
     * (the same vector fed to uViewDir), so the eye is uCtr - v * 16384*65536^2/|v|^2.
     * Done in double for the large intermediates. */
    {
        double vx = (double)d1c * (double)d10;
        double vy = (double)d18 * 65536.0;
        double vz = (double)d1c * (double)d14;
        double vv = vx*vx + vy*vy + vz*vz;
        if (vv > 1e-6) {
            double k = 16384.0 * 65536.0 * 65536.0 / vv;
            ss_cam_eye[0] = (float)((double)cx  - vx * k);
            ss_cam_eye[1] = (float)((double)cy8 - vy * k);
            ss_cam_eye[2] = (float)((double)cz  - vz * k);
        } else {
            ss_cam_eye[0] = cx; ss_cam_eye[1] = cy8; ss_cam_eye[2] = cz;
        }
    }
}

int hwr_ssao_active(void)
{
    return (ss_effective() && ss_ready) ? 1 : 0;
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
    if (!ss_effective() || w <= 0 || h <= 0) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);   /* draw straight to back buffer */
        return;
    }
    if (ss_init_once() != HWR_OK || ss_resize(w, h) != HWR_OK) {
        ss_enable = 0;                          /* give up; fall back this run */
        ss_refl_enable = 0;
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
    if (!ss_effective() || !ss_ready)
        return;

    glDisable(GL_DEPTH_TEST);

    /* --- SSAO + blur passes (only when SSAO itself is on; a reflection-only run
     *     skips them and the composite holds AO at 1.0). --- */
    if (ss_enable) {
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
    }

    /* --- Water reflection pass: ray-march into refl_tex, then blur it. Blurring
     *     the reflection RESULT is what softens the binary hit/miss polygon
     *     edges into a smooth watery reflection. --- */
    if (ss_refl_enable) {
        glBindFramebuffer(GL_FRAMEBUFFER, refl_fbo);
        glViewport(0, 0, ss_w, ss_h);
        glUseProgram(refl_prog);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, g_color);
        glUniform1i(u_rf_color, 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, g_pos);
        glUniform1i(u_rf_pos, 1);
        glUniform1i(u_rf_refl, 1);
        glUniform1i(u_rf_refldbg, ss_refl_debug);
        glUniform3f(u_rf_sky, ss_refl_sky[0], ss_refl_sky[1], ss_refl_sky[2]);
        glUniform3f(u_rf_viewdir, ss_viewdir[0], ss_viewdir[1], ss_viewdir[2]);
        glUniform3f(u_rf_eye, ss_cam_eye[0], ss_cam_eye[1], ss_cam_eye[2]);
        glUniform1f(u_rf_d10, ss_cam_d10);
        glUniform1f(u_rf_d14, ss_cam_d14);
        glUniform1f(u_rf_d18, ss_cam_d18);
        glUniform1f(u_rf_d1c, ss_cam_d1c);
        glUniform1f(u_rf_scale, ss_cam_scale);
        glUniform3f(u_rf_ctr, ss_cam_cx, ss_cam_cy8, ss_cam_cz);
        glUniform2f(u_rf_centre, ss_cam_ctrx, ss_cam_ctry);
        glUniform1i(u_rf_persp, ss_cam_persp);
        ss_fullscreen();

        glBindFramebuffer(GL_FRAMEBUFFER, reflblur_fbo);
        glUseProgram(reflblur_prog);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, refl_tex);
        glUniform1i(u_rb_refl, 0);
        glUniform2f(u_rb_texel, 1.0f / (float)ss_w, 1.0f / (float)ss_h);
        /* Debug view stays sharp so the colour regions remain readable. */
        glUniform1f(u_rb_radius, ss_refl_debug ? 0.0f : ss_refl_blur);
        ss_fullscreen();
    }

    /* --- Composite: scene colour * AO (+ blurred water reflection) -> back buffer --- */
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, ss_w, ss_h);
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
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, reflblur_tex);
    glUniform1i(u_cp_reflblur, 3);
    glUniform1i(u_cp_debug, ss_debug);
    glUniform1i(u_cp_useao, ss_enable ? 1 : 0);
    glUniform1i(u_cp_refl, ss_refl_enable ? 1 : 0);
    glUniform1i(u_cp_refldbg, ss_refl_debug);
    glUniform1f(u_cp_reflstr, ss_refl_strength);
    glUniform3f(u_cp_sky, ss_refl_sky[0], ss_refl_sky[1], ss_refl_sky[2]);
    ss_fullscreen();
    glActiveTexture(GL_TEXTURE0);   /* restore default active unit */

    glBindVertexArray(0);
    glEnable(GL_DEPTH_TEST);
    hwr_gl_check("ssao resolve");
}

void hwr_ssao_blit_depth(void)
{
    /* Copy the scene depth from the G-buffer into the back buffer so a following
     * depth-tested pass (weapon beams) is occluded by the 3D geometry. No-op when
     * SSAO is inactive: the scene was then drawn straight to the back buffer,
     * which already holds the correct depth. */
    if (!ss_effective() || !ss_ready)
        return;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glBlitFramebuffer(0, 0, ss_w, ss_h, 0, 0, ss_w, ss_h,
        GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    hwr_gl_check("ssao blit depth");
}
