#include "hwr_api.h"
#include "hwr_gl.h"
#include "hwr_internal.h"

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <stdio.h>

/* ---- Minimal Thing stub (real sizeof = 168, X/Y/Z at offset 24/28/32) ---- */
struct HwrThingStub {
    int16_t  Parent, Next, LinkParent, LinkChild;
    uint8_t  SubType, Type;
    int16_t  State;
    uint32_t Flag;
    int16_t  LinkSame, LinkSameGroup, Radius, ThingOffset;
    int32_t  X, Y, Z;
};
#define THING_SIZEOF 168
extern char *things;

/* ---- Minimal SimpleThing stub (real sizeof = 60, X/Y/Z as int16_t at 16/18/20) */
/* NOTE: things and sthings point to the SAME address (boundary between the
 * two arrays). SimpleThings are at negative indices from this shared pointer. */
struct HwrSThingStub {
    int16_t  Parent, Next;
    uint8_t  SubType, Type;
    int16_t  State;
    uint32_t Flag;
    int16_t  LinkSame, LinkSameGroup;
    int16_t  X, Y, Z;
};
#define STHING_SIZEOF 60
extern unsigned short things_used;
extern unsigned short sthings_used;

/* ---- FullLight struct (sizeof=32, must match source_sw.c HwrFullLight) --- */
struct HwrFullLightStub {
    int16_t Intensity, TrueIntensity, Command, NextFull;
    int16_t X, Y, Z;
    int16_t lgtfld_E, lgtfld_10, lgtfld_12;
    uint8_t lgtfld_14[10];
    uint16_t Flags;
};
extern struct HwrFullLightStub *game_full_lights;
extern uint16_t                next_full_light;

/* ---- Minimal game object stub (full 36-byte SingleObject) ---------------- */
struct HwrObjStub {
    uint16_t StartFace, NumbFaces, NextObject, StartFace4, NumbFaces4;
    int16_t  ThingNo;     /* offset 10 */
    int16_t  OffsetX, OffsetY, OffsetZ;
    int16_t  ObjectNo;
    int16_t  MapX, MapZ;
};
extern struct HwrObjStub *game_objects;
extern unsigned short     next_object;

/* Camera globals (same symbols as source_sw.c uses). */
extern int32_t        engn_xc, engn_yc, engn_zc;
extern int32_t        dword_176D10, dword_176D14;
extern int32_t        dword_176D18, dword_176D1C;
extern int32_t        dword_176D3C, dword_176D40;
extern unsigned short overall_scale;
extern int32_t        game_perspective;

/* ---- Internal state ------------------------------------------------------ */
static int         dbg_thingno_enabled = 0;
GLuint      dbg_prog = 0;
GLuint      dbg_vao = 0;
GLuint      dbg_vbo = 0;
GLuint      dbg_font_tex = 0;
int         dbg_ready = 0;

/* ---- 5×7 bitmap font: 0-9, '-', '.', 'A'-'Z' ----------------------------- */
#define DBG_NUM_GLYPHS 38

static const unsigned char dbg_font_bits[DBG_NUM_GLYPHS][7] = {
    /* 0-9 */ {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},
              {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
              {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F},
              {0x0E,0x11,0x01,0x06,0x01,0x11,0x0E},
              {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},
              {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
              {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E},
              {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
              {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},
              {0x0E,0x11,0x11,0x0F,0x01,0x11,0x0E},
    /* '-' */ {0x00,0x00,0x00,0x1F,0x00,0x00,0x00},
    /* '.' */ {0x00,0x00,0x00,0x00,0x00,0x00,0x04},
    /* A-Z */ {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11},  /* A */
              {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},  /* B */
              {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E},  /* C */
              {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E},  /* D */
              {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F},  /* E */
              {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},  /* F */
              {0x0E,0x11,0x10,0x17,0x11,0x11,0x0E},  /* G */
              {0x11,0x11,0x11,0x1F,0x11,0x11,0x11},  /* H */
              {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E},  /* I */
              {0x07,0x02,0x02,0x02,0x02,0x12,0x0C},  /* J */
              {0x11,0x12,0x14,0x18,0x14,0x12,0x11},  /* K */
              {0x10,0x10,0x10,0x10,0x10,0x10,0x1F},  /* L */
              {0x11,0x1B,0x15,0x15,0x11,0x11,0x11},  /* M */
              {0x11,0x19,0x15,0x13,0x11,0x11,0x11},  /* N */
              {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},  /* O */
              {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},  /* P */
              {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D},  /* Q */
              {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},  /* R */
              {0x0E,0x11,0x10,0x0E,0x01,0x11,0x0E},  /* S */
              {0x1F,0x04,0x04,0x04,0x04,0x04,0x04},  /* T */
              {0x11,0x11,0x11,0x11,0x11,0x11,0x0E},  /* U */
              {0x11,0x11,0x11,0x11,0x0A,0x0A,0x04},  /* V */
              {0x11,0x11,0x11,0x15,0x15,0x1B,0x11},  /* W */
              {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},  /* X */
              {0x11,0x11,0x0A,0x04,0x04,0x04,0x04},  /* Y */
              {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F},  /* Z */
};

static void dbg_build_font(unsigned char *tex)
{
    int d, r, c;
    memset(tex, 0, DBG_NUM_GLYPHS * 8 * 7 * 4);
    for (d = 0; d < DBG_NUM_GLYPHS; d++) {
        for (r = 0; r < 7; r++) {
            unsigned char bits = dbg_font_bits[d][r];
            for (c = 0; c < 5; c++) {
                if (bits & (1 << (4 - c))) {
                    int px = d * 8 + c;
                    int idx = (r * (DBG_NUM_GLYPHS * 8) + px) * 4;
                    tex[idx + 0] = 0;
                    tex[idx + 1] = 200;
                    tex[idx + 2] = 255;
                    tex[idx + 3] = 255;
                }
            }
        }
    }
}

/* ---- Shaders ------------------------------------------------------------- */
static const char *dbg_vert_src =
    "#version 330 core\n"
    "layout(location=0) in vec2 aPos;\n"
    "layout(location=1) in vec2 aUV;\n"
    "out vec2 vUV;\n"
    "void main(){\n"
    "    vUV = aUV;\n"
    "    gl_Position = vec4(aPos, 0.0, 1.0);\n"
    "}\n";

static const char *dbg_frag_src =
    "#version 330 core\n"
    "in vec2 vUV;\n"
    "out vec4 frag;\n"
    "uniform sampler2D uFont;\n"
    "uniform vec4 uColor;\n"
    "void main(){\n"
    "    vec4 t = texture(uFont, vUV);\n"
    "    if (t.a < 0.5) discard;\n"
    "    frag = uColor;\n"
    "}\n";

/* ---- Init ---------------------------------------------------------------- */
static int dbg_init(void)
{
    GLuint vs, fs;
    GLint ok;

    vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &dbg_vert_src, NULL);
    glCompileShader(vs);
    glGetShaderiv(vs, GL_COMPILE_STATUS, &ok);
    if (!ok) { glDeleteShader(vs); return HWR_ERROR; }

    fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &dbg_frag_src, NULL);
    glCompileShader(fs);
    glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
    if (!ok) { glDeleteShader(vs); glDeleteShader(fs); return HWR_ERROR; }

    dbg_prog = glCreateProgram();
    glAttachShader(dbg_prog, vs);
    glAttachShader(dbg_prog, fs);
    glLinkProgram(dbg_prog);
    glGetProgramiv(dbg_prog, GL_LINK_STATUS, &ok);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!ok) return HWR_ERROR;

    {
        int tw = DBG_NUM_GLYPHS * 8;
        unsigned char *tex = (unsigned char*)malloc((size_t)(tw * 7 * 4));
        dbg_build_font(tex);
        glGenTextures(1, &dbg_font_tex);
        glBindTexture(GL_TEXTURE_2D, dbg_font_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tw, 7, 0,
            GL_RGBA, GL_UNSIGNED_BYTE, tex);
        free(tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    glGenVertexArrays(1, &dbg_vao);
    glGenBuffers(1, &dbg_vbo);

    dbg_ready = 1;
    return HWR_OK;
}

/* ---- Project world (X,Y,Z) to screen pixel (sx,sy) using game camera -----
 *     Matches transform_shpoint() from engintrns.c.
 *     Uses the camera snapshot captured at floor-draw time (see hwr_sw_capture
 *     in source_sw.c) so the projection globals are valid — later sub-renders
 *     (BAT/billboard) overwrite them.                                     */
int dbg_project(float wx, float wy, float wz,
    float *sx, float *sy)
{
    int32_t capt_xc, capt_yc, capt_zc;
    int32_t capt_d10, capt_d14, capt_d18, capt_d1c;
    int32_t capt_d3c, capt_d40, capt_scale, capt_persp;

    if (!hwr_sw_camera_snapshot(&capt_xc, &capt_yc, &capt_zc,
        &capt_d10, &capt_d14, &capt_d18, &capt_d1c,
        &capt_d3c, &capt_d40, &capt_scale, &capt_persp))
        return 0;   /* snapshot not yet available — skip this frame */

    float dx = (wx / 256.0f) - (float)capt_xc;
    float dy = (wy / 32.0f) - 8.0f * (float)capt_yc;
    float dz = (wz / 256.0f) - (float)capt_zc;
    float d10 = (float)capt_d10, d14 = (float)capt_d14;
    float d18 = (float)capt_d18, d1c = (float)capt_d1c;
    float scale = (float)capt_scale;
    float ctrx = (float)capt_d3c, ctry = (float)capt_d40;

    float fa = (d14 * dx - d10 * dz) / 65536.0f;
    float fb = (d10 * dx + d14 * dz) / 65536.0f;
    float fc = (d1c * dy - d18 * fb) / 65536.0f;
    float scrd = (d18 * dy + d1c * fb) / 65536.0f;

    if (capt_persp == 5 && scrd > 1024.0f)
        scrd = 16384.0f * scrd / (scrd + 16384.0f);

    float shx = scale * fa / 2048.0f;
    float shy = scale * fc / 2048.0f;

    if (capt_persp == 5) {
        shx = shx * (16384.0f - scrd) / 16384.0f;
        shy = shy * (16384.0f - scrd) / 16384.0f;
    }

    *sx = ctrx + shx;
    *sy = ctry - shy;
    return 1;
}

    /* ---- Emit a single digit quad (GL_TRIANGLES, 6 verts) -------------------- */
#define DBG_MAX_VERTS (600 * 6 * 6 * 4)

void dbg_emit_digit(float *vbuf, int *nv,
    float sx, float sy, int glyph, float vw, float vh)
{
    float du = (float)glyph * (1.0f / (float)DBG_NUM_GLYPHS);
    float dv = 0.0f;
    float w = 8.0f, h = 8.0f;

    float x0 = (sx / vw) * 2.0f - 1.0f;
    float x1 = ((sx + w) / vw) * 2.0f - 1.0f;
    float y0 = 1.0f - (sy / vh) * 2.0f;
    float y1 = 1.0f - ((sy + h) / vh) * 2.0f;

    float du2 = du + (1.0f / (float)DBG_NUM_GLYPHS);

    vbuf[(*nv)++] = x0; vbuf[(*nv)++] = y0; vbuf[(*nv)++] = du;        vbuf[(*nv)++] = dv;
    vbuf[(*nv)++] = x1; vbuf[(*nv)++] = y0; vbuf[(*nv)++] = du2;       vbuf[(*nv)++] = dv;
    vbuf[(*nv)++] = x0; vbuf[(*nv)++] = y1; vbuf[(*nv)++] = du;        vbuf[(*nv)++] = dv + 1.0f;

    vbuf[(*nv)++] = x1; vbuf[(*nv)++] = y0; vbuf[(*nv)++] = du2;       vbuf[(*nv)++] = dv;
    vbuf[(*nv)++] = x1; vbuf[(*nv)++] = y1; vbuf[(*nv)++] = du2;       vbuf[(*nv)++] = dv + 1.0f;
    vbuf[(*nv)++] = x0; vbuf[(*nv)++] = y1; vbuf[(*nv)++] = du;        vbuf[(*nv)++] = dv + 1.0f;
}

/* Map an ASCII char to a glyph index (0-9, '-', '.', 'A'-'Z'). Returns -1 if unsupported. */
static int dbg_char_to_glyph(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c == '-') return 10;
    if (c == '.') return 11;
    if (c >= 'A' && c <= 'Z') return 12 + (c - 'A');
    return -1;
}

void dbg_emit_text(float *vbuf, int *nv,
    float sx, float sy, const char *text, float vw, float vh)
{
    int i;
    for (i = 0; text[i]; i++) {
        int g = dbg_char_to_glyph(text[i]);
        if (g < 0) continue;
        dbg_emit_digit(vbuf, nv, sx, sy, g, vw, vh);
        sx += 8.0f;
    }
}

/* ---- Emit a numbered label ----------------------------------------------- */
static void dbg_emit_label(float *vbuf, int *nv,
    float sx, float sy, float vw, float vh, int label)
{
    int digits[6], nd = 0, d, tn = label;
    if (tn == 0) { digits[nd++] = 0; }
    else { while (tn > 0) { digits[nd++] = tn % 10; tn /= 10; } }
    if (nd == 0) return;

    for (d = nd - 1; d >= 0; d--) {
        if (*nv + 6 * 4 > (int)(DBG_MAX_VERTS * 4))
            break;
        dbg_emit_digit(vbuf, nv, sx, sy, digits[d], vw, vh);
        sx += 9.0f;
    }
}

/* ---- Render current vertex buffer with a given color -------------------- */
static void dbg_render(float *vbuf, int nv, float r, float g, float b)
{
    if (nv == 0) return;

    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram(dbg_prog);
    glUniform1i(glGetUniformLocation(dbg_prog, "uFont"), 0);
    glUniform4f(glGetUniformLocation(dbg_prog, "uColor"), r, g, b, 1.0f);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, dbg_font_tex);

    glBindVertexArray(dbg_vao);
    glBindBuffer(GL_ARRAY_BUFFER, dbg_vbo);
    glBufferData(GL_ARRAY_BUFFER, nv * (GLsizeiptr)sizeof(float), vbuf, GL_STREAM_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * (GLsizeiptr)sizeof(float),
        (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * (GLsizeiptr)sizeof(float),
        (void *)(2 * sizeof(float)));

    glDrawArrays(GL_TRIANGLES, 0, nv / 4);

    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glBindVertexArray(0);
    glUseProgram(0);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);

    hwr_gl_check("dbg_render");
}

/* ---- Diagnostic dump: write raw positions to file ----------------------- */
static int dbg_dumped = 0;
static void dbg_dump_positions(void)
{
    FILE *f;
    int i;
    if (dbg_dumped) return;
    dbg_dumped = 1;
    f = fopen("debug_positions.txt", "w");
    if (!f) return;

    fprintf(f, "Camera: engn_xc=%d engn_yc=%d engn_zc=%d\n", engn_xc, engn_yc, engn_zc);
    fprintf(f, "Camera: d10=%d d14=%d d18=%d d1c=%d\n", dword_176D10, dword_176D14, dword_176D18, dword_176D1C);
    fprintf(f, "Camera: scale=%d persp=%d\n", overall_scale, game_perspective);
    fprintf(f, "things_used=%d next_object=%d next_full_light=%d\n", things_used, next_object, next_full_light);

    /* Dump game_objects positions */
    fprintf(f, "\n=== game_objects (first 30) ===\n");
    if (game_objects != NULL) {
        for (i = 1; i < (int)next_object && i <= 30; i++) {
            fprintf(f, "  obj[%d]: ThingNo=%d MapX=%d MapZ=%d OffsetY=%d\n",
                i, (int)game_objects[i].ThingNo,
                (int)game_objects[i].MapX, (int)game_objects[i].MapZ,
                (int)game_objects[i].OffsetY);
        }
    }

    /* Dump things positions (first 30) */
    fprintf(f, "\n=== things (first 30) ===\n");
    if (things != NULL) {
        for (i = 1; i < (int)things_used && i <= 30; i++) {
            struct HwrThingStub *t = (struct HwrThingStub *)(things + i * THING_SIZEOF);
            fprintf(f, "  thing[%d]: Type=%d SubType=%d X=%d Y=%d Z=%d\n",
                i, (int)t->Type, (int)t->SubType, (int)t->X, (int)t->Y, (int)t->Z);
        }
    }

    /* Dump sthings positions (first 30 with Type != 0) */
    fprintf(f, "\n=== sthings (first 30 with Type!=0) ===\n");
    if (things != NULL) {
        int found = 0;
        for (i = 1; i <= 1500 && found < 30; i++) {
            struct HwrSThingStub *s = (struct HwrSThingStub *)((char *)things - i * STHING_SIZEOF);
            if (s->Type == 0) continue;
            fprintf(f, "  sthing[-%d]: Type=%d SubType=%d X=%d Y=%d Z=%d\n",
                i, (int)s->Type, (int)s->SubType, (int)s->X, (int)s->Y, (int)s->Z);
            found++;
        }
        fprintf(f, "  (total found in 1500 slots: %d)\n", found);
    }

    /* Dump FullLights positions (first 30) */
    fprintf(f, "\n=== FullLights (first 30) ===\n");
    if (game_full_lights != NULL) {
        for (i = 1; i < (int)next_full_light && i <= 30; i++) {
            float x = (float)game_full_lights[i].X;
            float y = (float)game_full_lights[i].Y;
            float z = (float)game_full_lights[i].Z;
            fprintf(f, "  light[%d]: Intensity=%d X=%.0f Y=%.0f Z=%.0f\n",
                i, (int)game_full_lights[i].Intensity, x, y, z);
        }
    }

    fclose(f);
}

/* ---- Public API ---------------------------------------------------------- */
void hwr_thingno_debug(int enable)
{
    dbg_thingno_enabled = enable;
}

void hwr_thingno_render(void)
{
    int vw, vh;
    float vbuf[DBG_MAX_VERTS * 4];
    int nv = 0;
    int i;

    /* Always init the debug shader/font so hwr_tuning.c can share them. */
    if (!dbg_ready && dbg_init() != HWR_OK)
        return;

    dbg_dump_positions();

    if (!dbg_thingno_enabled)
        return;
    if (things == NULL || game_objects == NULL)
        return;

    hwr_drawable_size(&vw, &vh);
    if (vw <= 0 || vh <= 0)
        return;

    dbg_dump_positions();

    /* Iterate all game_objects[] — show ThingNo or object index */
    for (i = 1; i < (int)next_object; i++) {
        float wx, wy, wz;
        float sx, sy;
        int thingno = (int)game_objects[i].ThingNo;
        int label;

        if (thingno >= 1 && thingno < 1000) {
            struct HwrThingStub *t = (struct HwrThingStub *)(things + thingno * THING_SIZEOF);
            wx = (float)t->X;
            wy = (float)t->Y;
            wz = (float)t->Z;
            label = thingno;
        } else {
            /* MapX/MapZ might already be in PRCCOORD — try without *256. */
            wx = (float)game_objects[i].MapX;
            wy = 0.0f;
            wz = (float)game_objects[i].MapZ;
            label = i;
        }
        /* Skip objects at world origin (invalid position). */
        if (fabsf(wx) < 1.0f && fabsf(wz) < 1.0f)
            continue;

        if (!dbg_project(wx, wy, wz, &sx, &sy))
            continue;
        if (sx < -100.0f || sx > (float)vw + 100.0f ||
            sy < -100.0f || sy > (float)vh + 100.0f)
            continue;

        dbg_emit_label(vbuf, &nv, sx, sy, (float)vw, (float)vh, label);
    }
    dbg_render(vbuf, nv, 1.0f, 1.0f, 1.0f); /* white = game_objects */

    /* ---- Iterate all things[] - show ThingNo on every valid Thing --------- */
    nv = 0;
    for (i = 1; i < (int)things_used; i++) {
        struct HwrThingStub *t = (struct HwrThingStub *)(things + i * THING_SIZEOF);
        if (t->Type == 0)
            continue;
        float sx, sy;
        if (!dbg_project((float)t->X, (float)t->Y, (float)t->Z, &sx, &sy))
            continue;
        if (sx < -100.0f || sx > (float)vw + 100.0f ||
            sy < -100.0f || sy > (float)vh + 100.0f)
            continue;
        dbg_emit_label(vbuf, &nv, sx, sy, (float)vw, (float)vh, i);
    }
    dbg_render(vbuf, nv, 0.0f, 1.0f, 1.0f); /* cyan = things */

    /* ---- Iterate all sthings[] - label every SimpleThing ----------------- */
    /* SimpleThings are at negative indices from the shared things/sthings
     * pointer. Iterate all 1500 slots. */
    nv = 0;
    for (i = 1; i <= 1500; i++) {
        struct HwrSThingStub *s = (struct HwrSThingStub *)((char *)things - i * STHING_SIZEOF);
        if (s->Type == 0)
            continue;
        float sx, sy;
        float wx = (float)s->X;
        float wy = (float)s->Y;
        float wz = (float)s->Z;
            if (!dbg_project(wx, wy, wz, &sx, &sy))
                continue;
            if (sx < -100.0f || sx > (float)vw + 100.0f ||
                sy < -100.0f || sy > (float)vh + 100.0f)
                continue;
            dbg_emit_label(vbuf, &nv, sx, sy, (float)vw, (float)vh, i);
        }
    dbg_render(vbuf, nv, 1.0f, 1.0f, 0.0f); /* yellow = sthings */

    /* ---- Iterate all FullLights - label every light source --------------- */
    nv = 0;
    if (game_full_lights != NULL) {
        int32_t capt_xc, capt_yc, capt_zc;
        int32_t capt_d10, capt_d14, capt_d18, capt_d1c;
        int32_t capt_d3c, capt_d40, capt_scale, capt_persp;
        if (!hwr_sw_camera_snapshot(&capt_xc, &capt_yc, &capt_zc,
            &capt_d10, &capt_d14, &capt_d18, &capt_d1c,
            &capt_d3c, &capt_d40, &capt_scale, &capt_persp))
            goto skip_lights;
        for (i = 1; i < (int)next_full_light; i++) {
            if ((int)game_full_lights[i].Intensity == 0)
                continue;
            float sx, sy;
/* FullLight X/Z are in map tiles (same as engn_xc).
             * FullLight Y is a height offset in engine Y units. */
            float dx = (float)game_full_lights[i].X - (float)capt_xc;
            float dy = (float)game_full_lights[i].Y - 8.0f * (float)capt_yc;
            float dz = (float)game_full_lights[i].Z - (float)capt_zc;
            float d10f = (float)capt_d10, d14f = (float)capt_d14;
            float d18f = (float)capt_d18, d1cf = (float)capt_d1c;
            float scalef = (float)capt_scale;
            float ctrxf = (float)capt_d3c, ctryf = (float)capt_d40;
            float fa = (d14f * dx - d10f * dz) / 65536.0f;
            float fb = (d10f * dx + d14f * dz) / 65536.0f;
            float fc = (d1cf * dy - d18f * fb) / 65536.0f;
            float scrd = (d18f * dy + d1cf * fb) / 65536.0f;
            if (capt_persp == 5 && scrd > 1024.0f)
                scrd = 16384.0f * scrd / (scrd + 16384.0f);
            float shx = scalef * fa / 2048.0f;
            float shy = scalef * fc / 2048.0f;
            if (capt_persp == 5) {
                shx = shx * (16384.0f - scrd) / 16384.0f;
                shy = shy * (16384.0f - scrd) / 16384.0f;
            }
            sx = ctrxf + shx;
            sy = ctryf - shy;
            /* Skip lights at map origin (invalid/unset position). */
            if (game_full_lights[i].X == 0 && game_full_lights[i].Z == 0)
                continue;
            if (sx < -100.0f || sx > (float)vw + 100.0f ||
                sy < -100.0f || sy > (float)vh + 100.0f)
                continue;
            dbg_emit_label(vbuf, &nv, sx, sy, (float)vw, (float)vh, i);
        }
    }
skip_lights:
    dbg_render(vbuf, nv, 1.0f, 0.0f, 0.0f); /* red = FullLights */
}