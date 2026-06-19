#include "hwr_tuning.h"
#include "hwr_api.h"
#include "hwr_gl.h"
#include "hwr_internal.h"
#include "hwr_lights.h"

#include <SDL.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

/* ---- Slider descriptor --------------------------------------------------- */
typedef struct {
    const char *label;         /* short label (max 4 chars) */
    float       lr, lg, lb;    /* label colour */
    float      *value;         /* pointer into HwrLightDefaults */
    float       vmin, vmax;
    float       step, fine_step;
    const char *fmt;
} Slider;

/* Tuning state */
static int          tun_active = 0;
static int          prev_tog = 0;
static int          prev_up = 0, prev_dn = 0;
static int          prev_lt = 0, prev_rt = 0;
static int          prev_tab = 0;
static int          prev_kp7 = 0;
static int          save_flash = 0;
static int          sel = 0;

/* Number of sliders — keep in sync with the array below */
#define NSLIDERS 14

/* Single static slider array — used by both nav + render */
static Slider sliders[NSLIDERS];
static int    sliders_init = 0;

static void sliders_bind(HwrLightDefaults *d)
{
    sliders[0]  = (Slider){"GAIN", 1,1,1, &d->intensity, 0,2, 0.05f, 0.01f, "%.2f"};
    sliders[1]  = (Slider){"BLDG_B", 0.3f,0.8f,0.3f, &d->building_brightness, 0,10, 0.05f, 0.01f, "%.2f"};
    sliders[2]  = (Slider){"STRT_B", 1,1,0.5f, &d->street_brightness, 0,10, 0.05f, 0.01f, "%.2f"};
    sliders[3]  = (Slider){"FILL_B", 0.7f,0.7f,0.7f, &d->filler_brightness, 0,2, 0.05f, 0.01f, "%.2f"};
    sliders[4]  = (Slider){"SUN_BR", 1,1,0.5f, &d->sun_bright, 0,1, 0.02f, 0.005f, "%.2f"};
    sliders[5]  = (Slider){"SUN_AM", 0.5f,0.5f,1, &d->sun_ambient, 0,1, 0.01f, 0.005f, "%.2f"};
    sliders[6]  = (Slider){"SUN_AZ", 1,0.5f,0.5f, &d->sun_azimuth, 0,360, 5, 1, "%.0f"};
    sliders[7]  = (Slider){"SUN_EL", 0.5f,1,0.5f, &d->sun_elevation, 0,90, 2, 1, "%.0f"};
    sliders[8]  = (Slider){"SUN_HZ", 0.5f,0.8f,1, &d->sun_haze, 0,1, 0.05f, 0.01f, "%.2f"};
    sliders[9]  = (Slider){"AMBIENT", 0.5f,0.5f,1, &d->ambient, 0,1, 0.02f, 0.005f, "%.2f"};
    sliders[10] = (Slider){"SUN_PC", 0.8f,0.6f,1, (float*)&d->sun_pcf, 0,12, 1, 1, "%.0f"};
    sliders[11] = (Slider){"FILL_R", 0.7f,0.7f,0.7f, &d->filler_radius, 1, 500, 1, 0.5f, "%.0f"};
    sliders[12] = (Slider){"BLDG_R", 0.3f,0.8f,0.3f, &d->building_radius, 1, 500, 1, 0.5f, "%.0f"};
    sliders[13] = (Slider){"STRT_R", 1,1,0.5f, &d->street_radius, 1, 500, 1, 0.5f, "%.0f"};

    sliders_init = 1;
}

/* ---- Simple colour-quad shader (passthrough position + flat colour) ----- */
static GLuint tun_prog = 0;
static GLuint tun_vao = 0, tun_vbo = 0;
static int    tun_ready = 0;

static const char *tun_vert_src =
    "#version 330 core\n"
    "layout(location=0) in vec2 aPos;\n"
    "void main(){\n"
    "    gl_Position = vec4(aPos, 0.0, 1.0);\n"
    "}\n";
static const char *tun_frag_src =
    "#version 330 core\n"
    "out vec4 frag;\n"
    "uniform vec4 uCol;\n"
    "void main(){\n"
    "    frag = uCol;\n"
    "}\n";

static void tun_init_prog(void)
{
    GLuint vs, fs;
    GLint ok;
    vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &tun_vert_src, NULL);
    glCompileShader(vs);
    glGetShaderiv(vs, GL_COMPILE_STATUS, &ok);
    if (!ok) { glDeleteShader(vs); return; }
    fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &tun_frag_src, NULL);
    glCompileShader(fs);
    glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
    if (!ok) { glDeleteShader(vs); glDeleteShader(fs); return; }
    tun_prog = glCreateProgram();
    glAttachShader(tun_prog, vs);
    glAttachShader(tun_prog, fs);
    glLinkProgram(tun_prog);
    glGetProgramiv(tun_prog, GL_LINK_STATUS, &ok);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!ok) return;
    glGenVertexArrays(1, &tun_vao);
    glGenBuffers(1, &tun_vbo);
    tun_ready = 1;
}

/* Emit one filled quad (2 tris = 6 verts, each x,y) */
static void quad(float *buf, int *off, float x0, float y0, float x1, float y1)
{
    float v[6][2] = {{x0,y0},{x1,y0},{x0,y1},{x0,y1},{x1,y0},{x1,y1}};
    int i;
    for (i = 0; i < 6; i++) { buf[(*off)*2+0] = v[i][0]; buf[(*off)*2+1] = v[i][1]; (*off)++; }
}

/* ---- Public API --------------------------------------------------------- */
void hwr_tuning_toggle(void) { tun_active = !tun_active; }
int  hwr_tuning_active(void) { return tun_active; }

void hwr_tuning_render(void)
{
    int vw, vh, i;
    HwrLightDefaults *d = hwr_lights_ptr();

    if (!sliders_init) sliders_bind(d);

    /* --- F7 toggle — must run even when inactive so F7 can turn us ON --- */
    {
        const Uint8 *keys = SDL_GetKeyboardState(NULL);
        int f7 = keys[SDL_SCANCODE_F7];
        if (f7 && !prev_tog) { tun_active = !tun_active; prev_tog = 1; return; }
        prev_tog = f7;
    }

    if (!tun_active) return;

    /* --- Keyboard navigation (keypad) --- */
    {
        const Uint8 *keys = SDL_GetKeyboardState(NULL);
        int up = keys[SDL_SCANCODE_KP_8];
        int dn = keys[SDL_SCANCODE_KP_2];
        int lt = keys[SDL_SCANCODE_KP_4];
        int rt = keys[SDL_SCANCODE_KP_6];
        int tab = keys[SDL_SCANCODE_TAB];
        int shift = keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT];

        if (tab && !prev_tab) { sel = (sel + 1) % NSLIDERS; }
        if (up && !prev_up)   { sel = (sel - 1 + NSLIDERS) % NSLIDERS; }
        if (dn && !prev_dn)   { sel = (sel + 1) % NSLIDERS; }

        Slider *s = &sliders[sel];
        float step = shift ? s->fine_step : s->step;

        if (lt) { *s->value -= step; if (*s->value < s->vmin) *s->value = s->vmin; }
        if (rt) { *s->value += step; if (*s->value > s->vmax) *s->value = s->vmax; }

        prev_up = up; prev_dn = dn;
        prev_lt = lt; prev_rt = rt;
        prev_tab = tab;

        /* max_dist2 is derived from per-category radius in sw_get_lights() —
         * no panel-side sync needed. */

        int kp7 = keys[SDL_SCANCODE_KP_7];
        if (kp7 && !prev_kp7) { hwr_lights_save(); save_flash = 90; }
        prev_kp7 = kp7;
    }

    if (!tun_ready) tun_init_prog();
    if (!tun_ready || !dbg_ready) return;

    hwr_drawable_size(&vw, &vh);
    if (vw <= 0 || vh <= 0) return;

    /* ---- Layout in pixels (bottom-right) ---- */
    int pw  = 230;                  /* panel width */
    int px  = vw - pw - 8;          /* right edge, 8px margin */
    int ph  = NSLIDERS * 24 + 14;   /* total panel height */
    int py0 = vh - ph - 8;          /* bottom edge, 8px margin */
    int lh  = 24;                   /* line height */
    int lx  = px + 4;              /* label text x */
    int llw = 68;                   /* label text reserved width */
    int vx  = lx + llw + 4;        /* value text x */
    int vw2 = 48;                   /* value text width (reserved) */
    int sx  = vx + vw2 + 4;        /* slider track x */
    int sw  = 94;                   /* slider track width */

    /* Convert screen → NDC */
    float tox(float x) { return x / (float)vw * 2.0f - 1.0f; }
    float toy(float y) { return 1.0f - y / (float)vh * 2.0f; }

    /* ---- Build quad vertex buffer (colour-quad shader) ---- */
    float qbuf[4096 * 2];
    int nq = 0;

    /* Background panel */
    quad(qbuf, &nq, tox(px), toy(py0), tox(px + pw), toy(py0 + ph));

    for (i = 0; i < NSLIDERS; i++) {
        int ly = py0 + 5 + i * lh;
        Slider *s = &sliders[i];
        float t = (s->vmax > s->vmin) ? (*s->value - s->vmin) / (s->vmax - s->vmin) : 0;
        if (t < 0) t = 0;
        if (t > 1) t = 1;

        /* Selection highlight */
        if (i == sel)
            quad(qbuf, &nq, tox(px + 2), toy(ly - 1), tox(px + pw - 2), toy(ly + lh - 3));

        /* Slider track */
        quad(qbuf, &nq, tox(sx), toy(ly + 5), tox(sx + sw), toy(ly + 10));

        /* Slider thumb */
        int tx = sx + (int)(t * (float)(sw - 8));
        quad(qbuf, &nq, tox(tx), toy(ly + 2), tox(tx + 8), toy(ly + 14));
    }

    /* ---- Render quads via colour shader ---- */
    glUseProgram(tun_prog);
    GLint ucol = glGetUniformLocation(tun_prog, "uCol");
    glBindVertexArray(tun_vao);
    glBindBuffer(GL_ARRAY_BUFFER, tun_vbo);
    glBufferData(GL_ARRAY_BUFFER, nq * 2 * sizeof(float), qbuf, GL_STREAM_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (void*)0);
    glEnableVertexAttribArray(0);

    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    int vi = 0;

    /* Background */
    glUniform4f(ucol, 0.05f, 0.05f, 0.05f, 0.75f);
    glDrawArrays(GL_TRIANGLES, vi, 6); vi += 6;

    /* Slider elements — render in same order as built */
    for (i = 0; i < NSLIDERS; i++) {
        /* Selection highlight (only emitted for selected slider) */
        if (i == sel) {
            glUniform4f(ucol, 0.25f, 0.25f, 0.5f, 0.35f);
            glDrawArrays(GL_TRIANGLES, vi, 6); vi += 6;
        }

        /* Track */
        glUniform4f(ucol, 0.3f, 0.3f, 0.3f, 0.7f);
        glDrawArrays(GL_TRIANGLES, vi, 6); vi += 6;

        /* Thumb */
        glUniform4f(ucol, 0.9f, 0.9f, 0.9f, 1.0f);
        glDrawArrays(GL_TRIANGLES, vi, 6); vi += 6;
    }

    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glUseProgram(0);

    /* ---- Render labels + value numbers using the debug font ---- */
    {
        float fbuf[4096 * 4];  /* x,y,u,v per vertex */
        int nf = 0;
        char tmp[16];

        for (i = 0; i < NSLIDERS; i++) {
            Slider *s = &sliders[i];
            int ly = py0 + 5 + i * lh;

            /* Label text in white */
            dbg_emit_text(fbuf, &nf, (float)lx + 1, (float)ly + 1, s->label, (float)vw, (float)vh);

            /* Value text */
            int cx = vx;
            snprintf(tmp, sizeof(tmp), s->fmt, *s->value);
            int len = (int)strlen(tmp);
            int ch;
            for (ch = 0; ch < len && ch < 7; ch++) {
                char c = tmp[ch];
                int g;
                if (c >= '0' && c <= '9') g = c - '0';
                else if (c == '.') g = 11;
                else if (c == '-') g = 10;
                else continue;
                if (nf + 6 * 4 > 4096 * 4) break;
                dbg_emit_digit(fbuf, &nf, (float)cx, (float)ly, g, (float)vw, (float)vh);
                cx += 8;
            }
        }

        /* KP_7 save flash */
        if (save_flash > 0) {
            int sy = py0 - 20;
            dbg_emit_text(fbuf, &nf, (float)(px + 4), (float)sy, "SAVED", (float)vw, (float)vh);
            save_flash--;
        }

        if (nf > 0) {
            glDisable(GL_DEPTH_TEST);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

            glUseProgram(dbg_prog);
            glUniform1i(glGetUniformLocation(dbg_prog, "uFont"), 0);
            glUniform4f(glGetUniformLocation(dbg_prog, "uColor"), 0.9f, 0.9f, 0.9f, 1.0f);

            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, dbg_font_tex);

            glBindVertexArray(dbg_vao);
            glBindBuffer(GL_ARRAY_BUFFER, dbg_vbo);
            glBufferData(GL_ARRAY_BUFFER, nf * (GLsizeiptr)sizeof(float), fbuf, GL_STREAM_DRAW);

            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * (GLsizeiptr)sizeof(float), (void*)0);
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * (GLsizeiptr)sizeof(float), (void*)(2*sizeof(float)));

            glDrawArrays(GL_TRIANGLES, 0, nf / 4);

            glDisableVertexAttribArray(0);
            glDisableVertexAttribArray(1);
            glBindVertexArray(0);
            glUseProgram(0);
            glDisable(GL_BLEND);
            glEnable(GL_DEPTH_TEST);
        }
    }

    hwr_gl_check("hwr_tuning_render");
}
