#include "hwr_thingbrowse.h"
#include "hwr_api.h"
#include "hwr_gl.h"
#include "hwr_internal.h"
#include "hwr_lights.h"

#include <SDL.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <stdint.h>

#define THING_SIZEOF 168
#define STHING_SIZEOF 60
#define THINGS_LIMIT 2000
struct TbStub {
    int16_t  Parent, Next, LinkParent, LinkChild;
    uint8_t  SubType, Type;
    int16_t  State;
    uint32_t Flag;
    int16_t  LinkSame, LinkSameGroup, Radius, ThingOffset;
    int32_t  X, Y, Z;
};

struct STbStub {
    int16_t  Parent, Next, LinkParent, LinkChild;
    uint8_t  SubType, Type;
    int16_t  State;
    uint32_t Flag;
    int16_t  LinkSame, Object, Radius, ThingOffset;
    int32_t  X, Y, Z;
    int16_t  Frame, StartFrame, Timer1, StartTimer1;
    int16_t  U_Frame, U_StartFrame, U_LightHead, U_LightDie, U_LightAnim, U_Health;
    int16_t  Owner2;
    uint16_t UniqueID;
};

struct QLStub {
    uint16_t Ratio, Light, NextQuick;
};

struct ObjStub {
    uint16_t StartFace, NumbFaces, NextObject, StartFace4, NumbFaces4;
    int16_t  ThingNo;
    int16_t  OffsetX, OffsetY, OffsetZ, ObjectNo, MapX, MapZ;
    uint16_t StartPoint, EndPoint;
    uint16_t field_1C, field_1E;
    uint8_t  field_20[3];
    uint8_t  field_23;
};

struct F3Stub {
    int16_t  PointNo[3];
    uint16_t Texture;
    uint8_t  GFlags, Flags;
    uint16_t ExCol, Object;
    int16_t  Shade0, Shade1, Shade2;
    uint16_t Light0, Light1, Light2;
    uint16_t FaceNormal, WalkHeader, UnknTringl;
};

struct F4Stub {
    int16_t  PointNo[4];
    uint16_t Texture;
    uint8_t  GFlags, Flags;
    uint16_t ExCol, Object;
    int16_t  Shade0, Shade1, Shade2, Shade3;
    uint16_t Light0, Light1, Light2, Light3;
    uint16_t FaceNormal, WalkHeader, UnknTringl1, UnknTringl2;
};

struct PtStub {
    uint16_t PointOffset;
    int16_t  X, Y, Z;
    uint8_t  Pad1, Flags;
};

struct FlStub {
    int16_t  Intensity, TrueIntensity, Command, NextFull;
    int16_t  X, Y, Z;
    int16_t  lgtfld_E, lgtfld_10, lgtfld_12;
    uint8_t  lgtfld_14[10];
    uint16_t Flags;
};

extern char    *things;
extern struct ObjStub  *game_objects;
extern uint16_t         next_object;
extern struct F3Stub   *game_object_faces3;
extern uint16_t         next_object_face3;
extern struct F4Stub   *game_object_faces4;
extern uint16_t         next_object_face4;
extern struct QLStub   *game_quick_lights;
extern uint16_t         next_quick_light;
extern struct FlStub   *game_full_lights;
extern uint16_t         next_full_light;
extern uint16_t         things_used;

extern const char *thing_type_name(unsigned char tngtype, unsigned char subtype);

extern int32_t engn_xc, engn_yc, engn_zc;
extern int32_t engn_cam_yaw;
extern int32_t engn_cam_tilt;

static int br_mclick_x = -1, br_mclick_y = -1;

static int br_mode = 0; /* 0=off, 1=obj browser, 2=light browser */
static int br_sel_obj = 0;
static int br_hover_oi = 0;
static int br_light_sel = 0;
static int br_light_hover = 0;
static int br_prev_lt = 0, br_prev_rt = 0, br_prev_sav = 0, br_prev_tog = 0;
static int br_prev_pgup = 0, br_prev_pgdn = 0, br_prev_home = 0;
static int br_prev_dbg = 0;

struct BrLightLink {
    int obj_idx;
    int thingno;
    int ttype, tsub;
};
#define BR_MAX_FL_LINKS 16
#define BR_MAX_FL 4000
struct BrLightInfo {
    int nlinks;
    struct BrLightLink links[BR_MAX_FL_LINKS];
};
static struct BrLightInfo br_light_info[BR_MAX_FL];
static int br_light_map_built = 0;

/* Dot tracking: stores FullLight index per dot for per-dot color lookup */
#define BR_MAX_DOTS 4000
static int br_dot_list[BR_MAX_DOTS];

/* Category colors for lightbulb dots: index = hwr_thing_category_get() result */
static const float br_cat_colors[4][4] = {
    {0.7f, 0.7f, 1.0f, 0.7f},  /* 0 auto      – light blue-white */
    {1.0f, 0.8f, 0.3f, 0.7f},  /* 1 filler    – warm yellow     */
    {0.3f, 1.0f, 0.3f, 0.7f},  /* 2 building  – green           */
    {1.0f, 0.9f, 0.2f, 0.8f},  /* 3 street    – yellow          */
};

static int count_linked_lights(int thingno, int *light_indices, int max_light);

/** Dump full hierarchy for a given FullLight index to thingbrowse_debug.txt.
 *  Triggered by KP7 in light browser mode on the hovered/selected light. */
static void br_dump_light(int fidx)
{
    FILE *fp = fopen("thingbrowse_debug.txt", "a");
    if (!fp) return;

    struct FlStub *fl = &game_full_lights[fidx];
    fprintf(fp, "\n=== Light %d ===================================\n", fidx);
    fprintf(fp, "Intensity=%d  TrueIntensity=%d  Command=%d  NextFull=%d\n",
        fl->Intensity, fl->TrueIntensity, fl->Command, fl->NextFull);
    fprintf(fp, "X=%d Y=%d Z=%d\n", fl->X, fl->Y, fl->Z);

    /* ---- SimpleThings that OWN this light (LightHead chain) ---- */
    fprintf(fp, "\n-- Owners (SimpleThings with LightHead→...→%d) --\n", fidx);
    int n_owners = 0;
    for (int si = 1; si < 1500; si++) {
        struct STbStub *st = (struct STbStub *)((char *)things - si * 60);
        if (st->Type == 0 || st->U_LightHead == 0) continue;
        int ci = st->U_LightHead, visited = 0;
        while (ci > 0 && ci < (int)next_full_light) {
            if (ci == fidx) {
                fprintf(fp, "  SimpleThing[-%d] Type=%d SubType=%d Object=%d\n",
                    si, st->Type, st->SubType, st->Object);
                n_owners++;
                break;
            }
            ci = game_full_lights[ci].NextFull;
            if (++visited > 100) break;
        }
    }
    if (n_owners == 0)
        fprintf(fp, "  (no SimpleThing owner — unconnected light)\n");

    /* ---- Objects whose faces reference this light ---- */
    fprintf(fp, "\n-- Illuminators (Object faces → QuickLight → %d) --\n", fidx);
    int n_illum = 0;
    for (int oi = 1; oi < (int)next_object; oi++) {
        struct ObjStub *ob = &game_objects[oi];
        int f;
        for (f = 0; f < (int)ob->NumbFaces; f++) {
            int fi3 = (int)ob->StartFace + f;
            if (fi3 < 0 || fi3 >= (int)next_object_face3) continue;
            struct F3Stub *fc = &game_object_faces3[fi3];
            uint16_t lh[3] = {fc->Light0, fc->Light1, fc->Light2};
            for (int li = 0; li < 3; li++) {
                uint16_t qidx = lh[li];
                while (qidx != 0 && qidx < next_quick_light) {
                    struct QLStub *q = &game_quick_lights[qidx];
                    if (q->Light == (uint16_t)fidx) {
                        int tno = ob->ThingNo;
                        if (tno > 0 && tno < 1000) {
                            struct TbStub *th = (struct TbStub *)(things + tno * 168);
                            fprintf(fp, "  Obj[%d] Thing[%d] Type=%d SubType=%d  (face %d, QuickLight %d)\n",
                                oi, tno, th->Type, th->SubType, fi3, qidx);
                        } else if (tno < 0 && -tno < 1500) {
                            struct STbStub *st = (struct STbStub *)((char *)things - (-tno) * 60);
                            fprintf(fp, "  Obj[%d] SThing[-%d] Type=%d SubType=%d  (face %d, QuickLight %d)\n",
                                oi, -tno, st->Type, st->SubType, fi3, qidx);
                        } else {
                            fprintf(fp, "  Obj[%d] ThingNo=%d  (face %d, QuickLight %d)\n",
                                oi, tno, fi3, qidx);
                        }
                        n_illum++;
                    }
                    qidx = q->NextQuick;
                }
            }
        }
        for (f = 0; f < (int)ob->NumbFaces4; f++) {
            int fi4 = (int)ob->StartFace4 + f;
            if (fi4 < 0 || fi4 >= (int)next_object_face4) continue;
            struct F4Stub *fc = &game_object_faces4[fi4];
            uint16_t lh[4] = {fc->Light0, fc->Light1, fc->Light2, fc->Light3};
            for (int li = 0; li < 4; li++) {
                uint16_t qidx = lh[li];
                while (qidx != 0 && qidx < next_quick_light) {
                    struct QLStub *q = &game_quick_lights[qidx];
                    if (q->Light == (uint16_t)fidx) {
                        int tno = ob->ThingNo;
                        if (tno > 0 && tno < 1000) {
                            struct TbStub *th = (struct TbStub *)(things + tno * 168);
                            fprintf(fp, "  Obj[%d] Thing[%d] Type=%d SubType=%d  (face4 %d, QuickLight %d)\n",
                                oi, tno, th->Type, th->SubType, fi4, qidx);
                        } else if (tno < 0 && -tno < 1500) {
                            struct STbStub *st = (struct STbStub *)((char *)things - (-tno) * 60);
                            fprintf(fp, "  Obj[%d] SThing[-%d] Type=%d SubType=%d  (face4 %d, QuickLight %d)\n",
                                oi, -tno, st->Type, st->SubType, fi4, qidx);
                        } else {
                            fprintf(fp, "  Obj[%d] ThingNo=%d  (face4 %d, QuickLight %d)\n",
                                oi, tno, fi4, qidx);
                        }
                        n_illum++;
                    }
                    qidx = q->NextQuick;
                }
            }
        }
    }
    if (n_illum == 0)
        fprintf(fp, "  (no face references)\n");

    /* ---- Browser mapping result ---- */
    if (fidx < BR_MAX_FL) {
        struct BrLightInfo *info = &br_light_info[fidx];
        fprintf(fp, "\n-- Browser mapping: %d link(s)\n", info->nlinks);
        for (int li = 0; li < info->nlinks && li < BR_MAX_FL_LINKS; li++) {
            int cat = hwr_thing_category_get(info->links[li].ttype, info->links[li].tsub);
            fprintf(fp, "  SThing[-%d] Type=%d SubType=%d cat=%d%s\n",
                info->links[li].thingno,
                info->links[li].ttype, info->links[li].tsub, cat,
                info->links[li].tsub == 1 && cat == 3 ? "  (LAMP→STREET)" :
                info->links[li].tsub == 2 && cat == 2 ? "  (BLDG→BUILDING)" : "");
        }
    }

    fprintf(fp, "\n");
    fclose(fp);
}

static void br_build_light_map(void)
{
    int i;
    for (i = 0; i < BR_MAX_FL && i < (int)next_full_light; i++)
        br_light_info[i].nlinks = 0;

    /* Category defaults now set in hwr_lights.c:table_defaults() at startup.
     * Only the per-light ownership map is built here. */

    /* SimpleThings own FullLights via LightHead → NextFull linked list.
     * SimpleThings are at negative offsets from `things` (60-byte stride).
     * STHINGS_LIMIT = 1500 — do NOT go past this or garbage data can
     * overwrite valid cached_type entries for real lights. */
    int max_si = 1500;
    for (int si = 1; si < max_si; si++) {
        struct STbStub *st = (struct STbStub *)((char *)things - si * STHING_SIZEOF);
        if (st->Type == 0 || st->U_LightHead == 0) continue;
        int fidx = st->U_LightHead;
        int visited = 0;
        while (fidx > 0 && fidx < (int)next_full_light && fidx < BR_MAX_FL) {
            struct FlStub *fl = &game_full_lights[fidx];
            struct BrLightInfo *info = &br_light_info[fidx];
            if (info->nlinks < BR_MAX_FL_LINKS) {
                int dup = 0;
                for (int di = 0; di < info->nlinks; di++)
                    if (info->links[di].thingno == si) { dup = 1; break; }
                if (!dup) {
                    info->links[info->nlinks].thingno = si;
                    info->links[info->nlinks].obj_idx = st->Object;
                    info->links[info->nlinks].ttype = st->Type;
                    info->links[info->nlinks].tsub = st->SubType;
                    info->nlinks++;
                }
            }
            fidx = fl->NextFull;
            if (++visited > 100) break; /* guard against loops */
        }
    }
    br_light_map_built = 1;
}

static int object_has_light(struct ObjStub *ob)
{
    int f;
    for (f = 0; f < (int)ob->NumbFaces; f++) {
        int fi3 = (int)ob->StartFace + f;
        if (fi3 < 0 || fi3 >= (int)next_object_face3) continue;
        struct F3Stub *fc = &game_object_faces3[fi3];
        uint16_t lh[3] = {fc->Light0, fc->Light1, fc->Light2};
        for (int li = 0; li < 3; li++) {
            uint16_t qidx = lh[li];
            while (qidx != 0 && qidx < next_quick_light) {
                struct QLStub *q = &game_quick_lights[qidx];
                if (q->Light > 0 && q->Light < next_full_light) {
                    struct FlStub *fl = &game_full_lights[q->Light];
                    if (fl->Intensity > 0) return 1;
                }
                qidx = q->NextQuick;
            }
        }
    }
    for (f = 0; f < (int)ob->NumbFaces4; f++) {
        int fi4 = (int)ob->StartFace4 + f;
        if (fi4 < 0 || fi4 >= (int)next_object_face4) continue;
        struct F4Stub *fc = &game_object_faces4[fi4];
        uint16_t lh[4] = {fc->Light0, fc->Light1, fc->Light2, fc->Light3};
        for (int li = 0; li < 4; li++) {
            uint16_t qidx = lh[li];
            while (qidx != 0 && qidx < next_quick_light) {
                struct QLStub *q = &game_quick_lights[qidx];
                if (q->Light > 0 && q->Light < next_full_light) {
                    struct FlStub *fl = &game_full_lights[q->Light];
                    if (fl->Intensity > 0) return 1;
                }
                qidx = q->NextQuick;
            }
        }
    }
    return 0;
}

static void br_csv_export(void)
{
    FILE *fp = fopen("thingbrowse_lights.csv", "w");
    if (!fp) return;
    fprintf(fp, "ObjIdx,ThingNo,Type,SubType,TypeName,X,Y,Z,MapX,MapZ,LightCount\n");
    int n_objs = (int)next_object;
    int total_lit = 0;
    int oi;
    for (oi = 1; oi < n_objs; oi++) {
        struct ObjStub *ob = &game_objects[oi];
        if (ob->NumbFaces == 0 && ob->NumbFaces4 == 0) continue;
        int thingno = ob->ThingNo;
        if (thingno < 1 || thingno >= THINGS_LIMIT) continue;
        struct TbStub *th = (struct TbStub*)(things + thingno * THING_SIZEOF);
        if (th->Type != 9) continue;
        if (!object_has_light(ob)) continue;
        total_lit++;
        int ttype = (int)th->Type;
        int tsub  = (int)th->SubType;
        const char *tname = thing_type_name((unsigned char)ttype, (unsigned char)tsub);
        if (!tname) tname = "";
        int lid[64];
        int nl = count_linked_lights(thingno, lid, 64);
        fprintf(fp, "%d,%d,%d,%d,%s,%d,%d,%d,%d,%d,%d\n",
            oi, thingno, ttype, tsub, tname,
            (int)th->X, (int)th->Y, (int)th->Z,
            (int)ob->MapX, (int)ob->MapZ, nl);
    }
    fprintf(fp, "TOTAL_LIT,%d\n", total_lit);
    fclose(fp);
}

static GLuint br_prog   = 0;
static GLuint br_vao    = 0;
static GLuint br_vbo    = 0;
static int    br_ready  = 0;

static const char *br_vert_src =
    "#version 330 core\n"
    "layout(location=0) in vec2 aPos;\n"
    "void main(){ gl_Position = vec4(aPos, 0.0, 1.0); }\n";
static const char *br_frag_src =
    "#version 330 core\n"
    "out vec4 frag;\n"
    "uniform vec4 uCol;\n"
    "void main(){ frag = uCol; }\n";

static void br_init_prog(void)
{
    GLuint vs, fs; GLint ok;
    vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &br_vert_src, NULL); glCompileShader(vs);
    glGetShaderiv(vs, GL_COMPILE_STATUS, &ok);
    if (!ok) { glDeleteShader(vs); return; }
    fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &br_frag_src, NULL); glCompileShader(fs);
    glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
    if (!ok) { glDeleteShader(vs); glDeleteShader(fs); return; }
    br_prog = glCreateProgram();
    glAttachShader(br_prog, vs); glAttachShader(br_prog, fs);
    glLinkProgram(br_prog);
    glGetProgramiv(br_prog, GL_LINK_STATUS, &ok);
    glDeleteShader(vs); glDeleteShader(fs);
    if (!ok) return;
    glGenVertexArrays(1, &br_vao); glGenBuffers(1, &br_vbo);
    br_ready = 1;
}

static void br_quad(float *buf, int *off, float x0, float y0, float x1, float y1)
{
    float v[6][2] = {{x0,y0},{x1,y0},{x0,y1},{x0,y1},{x1,y0},{x1,y1}};
    int i;
    for (i = 0; i < 6; i++) { buf[(*off)*2+0] = v[i][0]; buf[(*off)*2+1] = v[i][1]; (*off)++; }
}

static int count_linked_lights(int thingno, int *light_indices, int max_light)
{
    int nlt = 0, oi, fi;
    for (oi = 1; oi < (int)next_object; oi++) {
        struct ObjStub *ob = &game_objects[oi];
        if (ob->ThingNo != thingno) continue;
        int f;
        for (f = 0; f < (int)ob->NumbFaces; f++) {
            int fi3 = (int)ob->StartFace + f;
            if (fi3 < 0 || fi3 >= (int)next_object_face3) continue;
            struct F3Stub *fc = &game_object_faces3[fi3];
            uint16_t lh[3] = {fc->Light0, fc->Light1, fc->Light2};
            int li;
            for (li = 0; li < 3; li++) {
                uint16_t qidx = lh[li];
                while (qidx != 0 && qidx < next_quick_light) {
                    struct QLStub *q = &game_quick_lights[qidx];
                    uint16_t fidx = q->Light;
                    int dup = 0;
                    for (fi = 0; fi < nlt; fi++)
                        if (light_indices[fi] == (int)fidx) { dup = 1; break; }
                    if (!dup && nlt < max_light) light_indices[nlt++] = (int)fidx;
                    qidx = q->NextQuick;
                }
            }
        }
        for (f = 0; f < (int)ob->NumbFaces4; f++) {
            int fi4 = (int)ob->StartFace4 + f;
            if (fi4 < 0 || fi4 >= (int)next_object_face4) continue;
            struct F4Stub *fc = &game_object_faces4[fi4];
            uint16_t lh[4] = {fc->Light0, fc->Light1, fc->Light2, fc->Light3};
            int li;
            for (li = 0; li < 4; li++) {
                uint16_t qidx = lh[li];
                while (qidx != 0 && qidx < next_quick_light) {
                    struct QLStub *q = &game_quick_lights[qidx];
                    uint16_t fidx = q->Light;
                    int dup = 0;
                    for (fi = 0; fi < nlt; fi++)
                        if (light_indices[fi] == (int)fidx) { dup = 1; break; }
                    if (!dup && nlt < max_light) light_indices[nlt++] = (int)fidx;
                    qidx = q->NextQuick;
                }
            }
        }
    }
    return nlt;
}

void hwr_thingbrowse_render(void)
{
    int vw, vh;

    if (!dbg_ready) return;
    if (!br_ready) br_init_prog();
    if (!br_ready) return;

    hwr_drawable_size(&vw, &vh);
    if (vw <= 0 || vh <= 0) return;

    {
        const Uint8 *keys = SDL_GetKeyboardState(NULL);
        int f5 = keys[SDL_SCANCODE_F5];
        if (f5 && !br_prev_tog) {
            br_mode = (br_mode + 1) % 3;
            if (br_mode == 2 && !br_light_map_built) br_build_light_map();
            br_prev_tog = 1;
            return;
        }
        br_prev_tog = f5;
    }

    if (!br_mode) return;

    {
        const Uint8 *keys = SDL_GetKeyboardState(NULL);
        int lt  = keys[SDL_SCANCODE_KP_4];
        int rt  = keys[SDL_SCANCODE_KP_6];
        int sav = keys[SDL_SCANCODE_KP_5];

        if (br_mode == 1) {
            int cat_idx = br_hover_oi > 0 ? br_hover_oi : br_sel_obj;
            if (cat_idx > 0 && cat_idx < (int)next_object) {
                struct ObjStub *ob = &game_objects[cat_idx];
                int thingno = ob->ThingNo;
                if (thingno > 0 && thingno < THINGS_LIMIT) {
                    struct TbStub *t = (struct TbStub*)(things + thingno * THING_SIZEOF);
                    int cur_cat = hwr_thing_category_get(t->Type, t->SubType);
                    if (lt && !br_prev_lt) { cur_cat--; if (cur_cat < 0) cur_cat = 3; hwr_thing_category_set(t->Type, t->SubType, cur_cat); }
                    if (rt && !br_prev_rt) { cur_cat++; if (cur_cat > 3) cur_cat = 0; hwr_thing_category_set(t->Type, t->SubType, cur_cat); }
                }
            }
        }

        if (sav && !br_prev_sav) { hwr_thing_category_save_all(); }

        /* KP7 in light browser mode: dump hierarchy of hovered/selected light */
        if (br_mode == 2) {
            int dbg = keys[SDL_SCANCODE_KP_7];
            if (dbg && !br_prev_dbg) {
                int di = br_light_hover > 0 ? br_light_hover : br_light_sel;
                if (di > 0) br_dump_light(di);
            }
            br_prev_dbg = dbg;
        }

        if (keys[SDL_SCANCODE_PAGEUP] && !br_prev_pgup)   engn_cam_tilt += 64;
        if (keys[SDL_SCANCODE_PAGEDOWN] && !br_prev_pgdn) engn_cam_tilt -= 64;
        if (keys[SDL_SCANCODE_HOME] && !br_prev_home)     engn_cam_tilt = -172;

        br_prev_lt = lt; br_prev_rt = rt; br_prev_sav = sav;
        br_prev_pgup = keys[SDL_SCANCODE_PAGEUP];
        br_prev_pgdn = keys[SDL_SCANCODE_PAGEDOWN];
        br_prev_home = keys[SDL_SCANCODE_HOME];
    }

    int mx, my;
    SDL_GetMouseState(&mx, &my);

    if (br_mode == 1) {
        br_hover_oi = 0;
        float best_d2 = 1200.0f;
        int n_objs = (int)next_object;
        int oi;
        for (oi = 1; oi < n_objs; oi++) {
            struct ObjStub *ob = &game_objects[oi];
            if (ob->NumbFaces == 0 && ob->NumbFaces4 == 0) continue;
            int thingno = ob->ThingNo;
            if (thingno < 1 || thingno >= THINGS_LIMIT) continue;
            struct TbStub *th = (struct TbStub*)(things + thingno * THING_SIZEOF);
            if (th->Type != 9) continue;
            float sx, sy;
            if (dbg_project((float)th->X, (float)th->Y, (float)th->Z, &sx, &sy)) {
                float dx = sx - (float)mx, dy = sy - (float)my;
                float d2 = dx*dx + dy*dy;
                if (d2 < best_d2) { best_d2 = d2; br_hover_oi = oi; }
            }
        }

        Uint32 mbtn = SDL_GetMouseState(&mx, &my);
        if (mbtn & SDL_BUTTON(SDL_BUTTON_LEFT)) {
            if (br_mclick_x < 0) { br_mclick_x = mx; br_mclick_y = my; }
        } else {
            if (br_mclick_x >= 0) {
                br_mclick_x = br_mclick_y = -1;
                if (br_hover_oi > 0) br_sel_obj = br_hover_oi;
            }
        }
    } else if (br_mode == 2) {
        br_light_hover = 0;
        float best_d2 = 2500.0f;
        int nfl = (int)next_full_light;
        if (nfl > BR_MAX_FL) nfl = BR_MAX_FL;
        int fi;
        for (fi = 1; fi < nfl; fi++) {
            struct FlStub *fl = &game_full_lights[fi];
            if (fl->Intensity <= 0) continue;
            float sx, sy;
            float lx = (float)((int)fl->X + 70) * 256.0f;
            float ly = (float)fl->Y * 32.0f;
            float lz = (float)((int)fl->Z + 50) * 256.0f;
            if (dbg_project(lx, ly, lz, &sx, &sy)) {
                float dx = sx - (float)mx, dy = sy - (float)my;
                float d2 = dx*dx + dy*dy;
                if (d2 < best_d2) { best_d2 = d2; br_light_hover = fi; }
            }
        }

        Uint32 mbtn = SDL_GetMouseState(&mx, &my);
        if (mbtn & SDL_BUTTON(SDL_BUTTON_LEFT)) {
            if (br_mclick_x < 0) { br_mclick_x = mx; br_mclick_y = my; }
        } else {
            if (br_mclick_x >= 0) {
                br_mclick_x = br_mclick_y = -1;
                if (br_light_hover > 0) br_light_sel = br_light_hover;
            }
        }
    }

    int pw  = 220;
    int px  = vw - pw - 8;
    int py0 = vh - 240;
    int lh  = 20;
    int ph  = 230;

    float tox(float x) { return x / (float)vw * 2.0f - 1.0f; }
    float toy(float y) { return 1.0f - y / (float)vh * 2.0f; }

    float qbuf[8192 * 2];
    int nq = 0;

    int nq_bg   = 0; /* panel background end index   */
    int nq_dots = 0; /* lightbulb dots end index     */
    int nq_sel  = 0; /* selection crosshair end index */
    int nq_hov  = 0; /* hover crosshair end index     */

    br_quad(qbuf, &nq, tox(px), toy(py0), tox(px + pw), toy(py0 + ph));
    nq_bg = nq;

    float sel_x = 0, sel_y = 0;
    int sel_on = 0;
    float hov_x = 0, hov_y = 0;
    int hov_on = 0;

    if (br_mode == 1) {
        int obj_idx = br_sel_obj;
        if (obj_idx > 0 && obj_idx < (int)next_object) {
            struct ObjStub *ob = &game_objects[obj_idx];
            int thingno = ob->ThingNo;
            if (thingno > 0 && thingno < THINGS_LIMIT) {
                struct TbStub *th = (struct TbStub*)(things + thingno * THING_SIZEOF);
                if (th->Type != 0 && dbg_project((float)th->X, (float)th->Y, (float)th->Z, &sel_x, &sel_y)) {
                    if (sel_x >= -50 && sel_x <= (float)vw + 50 && sel_y >= -50 && sel_y <= (float)vh + 50) {
                        sel_on = 1;
                        int ch = 6;
                        br_quad(qbuf, &nq, tox(sel_x - ch), toy(sel_y - 1), tox(sel_x + ch), toy(sel_y + 1));
                        br_quad(qbuf, &nq, tox(sel_x - 1), toy(sel_y - ch), tox(sel_x + 1), toy(sel_y + ch));
                    }
                }
            }
        }
        nq_sel = nq;
        if (br_hover_oi > 0 && br_hover_oi != obj_idx) {
            struct ObjStub *hob = &game_objects[br_hover_oi];
            int h_tno = hob->ThingNo;
            if (h_tno > 0 && h_tno < THINGS_LIMIT) {
                struct TbStub *hth = (struct TbStub*)(things + h_tno * THING_SIZEOF);
                if (hth->Type != 0 && dbg_project((float)hth->X, (float)hth->Y, (float)hth->Z, &hov_x, &hov_y)) {
                    if (hov_x >= -50 && hov_x <= (float)vw + 50 && hov_y >= -50 && hov_y <= (float)vh + 50) {
                        hov_on = 1;
                        int ch = 8;
                        br_quad(qbuf, &nq, tox(hov_x - ch), toy(hov_y - 1), tox(hov_x + ch), toy(hov_y + 1));
                        br_quad(qbuf, &nq, tox(hov_x - 1), toy(hov_y - ch), tox(hov_x + 1), toy(hov_y + ch));
                    }
                }
            }
        }
        nq_hov = nq;
    } else if (br_mode == 2) {
        int fi;
        int nfl_all = (int)next_full_light;
        if (nfl_all > BR_MAX_FL) nfl_all = BR_MAX_FL;
        int ndots = 0;
        for (fi = 1; fi < nfl_all; fi++) {
            struct FlStub *fl = &game_full_lights[fi];
            if (fl->Intensity <= 0) continue;
            float bx, by;
            float lx = (float)((int)fl->X + 70) * 256.0f;
            float ly = (float)fl->Y * 32.0f;
            float lz = (float)((int)fl->Z + 50) * 256.0f;
            if (!dbg_project(lx, ly, lz, &bx, &by)) continue;
            if (bx < -50 || bx > (float)vw + 50 || by < -50 || by > (float)vh + 50) continue;
            if (fi == br_light_sel) {
                sel_x = bx; sel_y = by; sel_on = 1;
            } else if (fi == br_light_hover) {
                hov_x = bx; hov_y = by; hov_on = 1;
            } else {
                int ch = 3;
                br_quad(qbuf, &nq, tox(bx - ch), toy(by - ch), tox(bx + ch), toy(by + ch));
                if (ndots < BR_MAX_DOTS) br_dot_list[ndots++] = fi;
            }
        }
        nq_dots = nq;
        if (sel_on) {
            int ch = 6;
            br_quad(qbuf, &nq, tox(sel_x - ch), toy(sel_y - 1), tox(sel_x + ch), toy(sel_y + 1));
            br_quad(qbuf, &nq, tox(sel_x - 1), toy(sel_y - ch), tox(sel_x + 1), toy(sel_y + ch));
        }
        nq_sel = nq;
        if (hov_on) {
            int ch = 8;
            br_quad(qbuf, &nq, tox(hov_x - ch), toy(hov_y - 1), tox(hov_x + ch), toy(hov_y + 1));
            br_quad(qbuf, &nq, tox(hov_x - 1), toy(hov_y - ch), tox(hov_x + 1), toy(hov_y + ch));
        }
        nq_hov = nq;
    }

    glUseProgram(br_prog);
    GLint ucol = glGetUniformLocation(br_prog, "uCol");
    glBindVertexArray(br_vao);
    glBindBuffer(GL_ARRAY_BUFFER, br_vbo);
    glBufferData(GL_ARRAY_BUFFER, nq * 2 * sizeof(float), qbuf, GL_STREAM_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (void*)0);
    glEnableVertexAttribArray(0);

    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    int vi = 0;
    glUniform4f(ucol, 0.05f, 0.05f, 0.05f, 0.75f);
    glDrawArrays(GL_TRIANGLES, vi, nq_bg); vi += nq_bg;

    if (br_mode == 2) {
        int nd = (nq_dots - nq_bg) / 6;
        for (int di = 0; di < nd; di++) {
            int fi = br_dot_list[di];
            int cat = 0;
            if (fi > 0 && fi < BR_MAX_FL && br_light_info[fi].nlinks > 0) {
                int tt = br_light_info[fi].links[0].ttype;
                int ts = br_light_info[fi].links[0].tsub;
                if (tt > 0) cat = hwr_thing_category_get(tt, ts);
                if (cat < 0 || cat > 3) cat = 0;
            }
            const float *c = br_cat_colors[cat];
            glUniform4f(ucol, c[0], c[1], c[2], c[3]);
            glDrawArrays(GL_TRIANGLES, vi + di * 6, 6);
        }
        vi += (nq_dots - nq_bg);
    }

    if (sel_on) {
        glUniform4f(ucol, br_mode == 2 ? 1.0f : 0.0f, 0.0f, 1.0f, 1.0f);
        glDrawArrays(GL_TRIANGLES, vi, nq_sel - vi); vi = nq_sel;
    }

    if (hov_on) {
        glUniform4f(ucol, 1.0f, 1.0f, 0.0f, 1.0f);
        glDrawArrays(GL_TRIANGLES, vi, nq_hov - vi); vi = nq_hov;
    }

    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glUseProgram(0);

    {
        float fbuf[8192 * 4];
        int nf = 0;
        char tmp[64];

        int ly = py0 + 4;

        if (br_mode == 1) {
            dbg_emit_text(fbuf, &nf, (float)px + 6, (float)ly, "OBJ BROWSER [F5]", (float)vw, (float)vh);
            ly += lh;

            int obj_idx = br_sel_obj, thingno = 0, ttype = 0, tsub = 0;
            const char *tname = "---";
            int linked_lts = 0;
            int cat = 0;
            char catname[16] = "UNSET";

            if (obj_idx > 0 && obj_idx < (int)next_object) {
                struct ObjStub *ob = &game_objects[obj_idx];
                thingno = ob->ThingNo;
                if (thingno > 0 && thingno < THINGS_LIMIT) {
                    struct TbStub *th = (struct TbStub*)(things + thingno * THING_SIZEOF);
                    if (th->Type != 0) {
                        ttype = (int)th->Type;
                        tsub  = (int)th->SubType;
                        tname = thing_type_name((unsigned char)ttype, (unsigned char)tsub);
                        if (!tname) tname = "?";
                        cat = hwr_thing_category_get(ttype, tsub);
                        switch (cat) {
                            case 1: strcpy(catname, "FILLER  "); break;
                            case 2: strcpy(catname, "BUILDING"); break;
                            case 3: strcpy(catname, "STREET  "); break;
                            default: strcpy(catname, "UNSET   "); break;
                        }
                        int lid[64];
                        linked_lts = count_linked_lights(thingno, lid, 64);
                    }
                }
            }

            snprintf(tmp, 64, "OBJ:%04d  THG:%04d", obj_idx, thingno);
            dbg_emit_text(fbuf, &nf, (float)px + 6, (float)ly, tmp, (float)vw, (float)vh);
            ly += lh;

            snprintf(tmp, 64, "T:%d  S:%d", ttype, tsub);
            dbg_emit_text(fbuf, &nf, (float)px + 6, (float)ly, tmp, (float)vw, (float)vh);
            ly += lh;

            dbg_emit_text(fbuf, &nf, (float)px + 6, (float)ly, tname, (float)vw, (float)vh);
            ly += lh;

            snprintf(tmp, 64, "LIGHT:%d", linked_lts);
            dbg_emit_text(fbuf, &nf, (float)px + 6, (float)ly, tmp, (float)vw, (float)vh);
            ly += lh + 2;

            snprintf(tmp, 64, "CAT: %s", catname);
            dbg_emit_text(fbuf, &nf, (float)px + 6, (float)ly, tmp, (float)vw, (float)vh);
            ly += lh;

            dbg_emit_text(fbuf, &nf, (float)px + 6, (float)ly, "HOVER + CLICK  4/6 CAT 5 SAVE", (float)vw, (float)vh);
            ly += lh;

            if (br_hover_oi > 0 && br_hover_oi != obj_idx) {
                struct ObjStub *hob = &game_objects[br_hover_oi];
                int htno = hob->ThingNo;
                const char *hname = "?";
                if (htno > 0 && htno < THINGS_LIMIT) {
                    struct TbStub *hth = (struct TbStub*)(things + htno * THING_SIZEOF);
                    hname = thing_type_name(hth->Type, hth->SubType);
                    if (!hname) hname = "?";
                }
                snprintf(tmp, 64, "HOV OBJ:%04d THG:%04d %s", br_hover_oi, htno, hname);
                dbg_emit_text(fbuf, &nf, (float)px + 6, (float)ly, tmp, (float)vw, (float)vh);
            }

            if (sel_on) {
                snprintf(tmp, 64, "OBJ #%d  THG #%d", obj_idx, thingno);
                dbg_emit_text(fbuf, &nf, sel_x + 10, sel_y - 4, tmp, (float)vw, (float)vh);
            }
            if (hov_on) {
                struct ObjStub *hob = &game_objects[br_hover_oi];
                int htno = hob->ThingNo;
                snprintf(tmp, 64, "OBJ #%d  THG #%d", br_hover_oi, htno);
                dbg_emit_text(fbuf, &nf, hov_x + 10, hov_y - 4, tmp, (float)vw, (float)vh);
            }
        } else if (br_mode == 2) {
            dbg_emit_text(fbuf, &nf, (float)px + 6, (float)ly, "LIGHT BROWSER [F5]", (float)vw, (float)vh);
            ly += lh;

            int fi = br_light_sel;
            int intensity = 0, trueint = 0, cmd = 0;
            int fl_x = 0, fl_y = 0, fl_z = 0;
            int active = 0;
            struct BrLightInfo *info = NULL;

            if (fi > 0 && fi < (int)next_full_light) {
                struct FlStub *fl = &game_full_lights[fi];
                intensity = (int)fl->Intensity;
                trueint   = (int)fl->TrueIntensity;
                cmd       = (int)fl->Command;
                fl_x      = (int)fl->X;
                fl_y      = (int)fl->Y;
                fl_z      = (int)fl->Z;
                active    = (int)fl->Intensity > 0;
                if (fi < BR_MAX_FL) info = &br_light_info[fi];
            }

            snprintf(tmp, 64, "FL:%04d  %s", fi, active ? "ON" : "OFF");
            dbg_emit_text(fbuf, &nf, (float)px + 6, (float)ly, tmp, (float)vw, (float)vh);
            ly += lh;

            snprintf(tmp, 64, "INT:%d  TRUE:%d", intensity, trueint);
            dbg_emit_text(fbuf, &nf, (float)px + 6, (float)ly, tmp, (float)vw, (float)vh);
            ly += lh;

            snprintf(tmp, 64, "CMD:%d  XYZ:%d %d %d", cmd, fl_x, fl_y, fl_z);
            dbg_emit_text(fbuf, &nf, (float)px + 6, (float)ly, tmp, (float)vw, (float)vh);
            ly += lh + 2;

            if (info && info->nlinks > 0) {
                snprintf(tmp, 64, "CONNECTS TO: %d", info->nlinks);
                dbg_emit_text(fbuf, &nf, (float)px + 6, (float)ly, tmp, (float)vw, (float)vh);
                ly += lh;
                int li;
                for (li = 0; li < info->nlinks && li < 6; li++) {
                    int tt = info->links[li].ttype;
                    int ts = info->links[li].tsub;
                    const char *lbl;
                    if (tt == 5 && ts == 1)      lbl = "LAMP LIGHT";
                    else if (tt == 5 && ts == 2) lbl = "BUILDING LIGHT";
                    else {
                        const char *tn = thing_type_name((unsigned char)tt, (unsigned char)ts);
                        lbl = tn ? tn : "?";
                    }
                    static const char *cat_names[4] = {"AUTO","FILLER","BUILDING","STREET"};
                    int cat = hwr_thing_category_get(tt, ts);
                    if (cat < 0 || cat > 3) cat = 0;
                    snprintf(tmp, 64, "  %s  %s", lbl, cat_names[cat]);
                    dbg_emit_text(fbuf, &nf, (float)px + 6, (float)ly, tmp, (float)vw, (float)vh);
                    ly += lh;
                }
            } else {
                dbg_emit_text(fbuf, &nf, (float)px + 6, (float)ly, "NO CONNECTED OBJECTS", (float)vw, (float)vh);
                ly += lh;
            }
            ly += 2;

            dbg_emit_text(fbuf, &nf, (float)px + 6, (float)ly, "HOVER + CLICK  PGUP/DN TILT", (float)vw, (float)vh);

            if (br_light_hover > 0 && br_light_hover != fi) {
                snprintf(tmp, 64, "HOV FL:%04d", br_light_hover);
                dbg_emit_text(fbuf, &nf, (float)px + 6, (float)ly + lh, tmp, (float)vw, (float)vh);
            }

            if (sel_on) {
                snprintf(tmp, 64, "FL #%d", fi);
                dbg_emit_text(fbuf, &nf, sel_x + 10, sel_y - 4, tmp, (float)vw, (float)vh);
            }
            if (hov_on) {
                snprintf(tmp, 64, "FL #%d", br_light_hover);
                dbg_emit_text(fbuf, &nf, hov_x + 10, hov_y - 4, tmp, (float)vw, (float)vh);
            }
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

    hwr_gl_check("hwr_thingbrowse_render");
}

int hwr_thingbrowse_active(void)
{
    return br_mode != 0;
}
