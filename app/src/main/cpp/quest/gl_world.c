// True-3D DOOM world renderer (incremental prototype). See gl_world.h.
//
// Geometry is rebuilt every frame straight from doomgeneric's live level
// globals (sectors/lines/sides/segs/subsectors/mobj thinkers), so doors,
// lifts, light changes and sprite animation Just Work without a separate
// sync step. DOOM map units are kept in the vertex data; the model matrix
// scales them to metres and anchors the world onto the real head pose.

#include "gl_world.h"
#include "gl_common.h"   // GLES3 + logging, backend-neutral

#include <math.h>
#include <stdlib.h>
#include <string.h>

// doomgeneric data ------------------------------------------------------------

#include "../doomgeneric/doomtype.h"
#include "../doomgeneric/doomstat.h"
#include "../doomgeneric/d_player.h"
#include "../doomgeneric/d_think.h"
#include "../doomgeneric/p_local.h"
#include "../doomgeneric/p_mobj.h"
#include "../doomgeneric/p_pspr.h"
#include "../doomgeneric/r_defs.h"
#include "../doomgeneric/r_state.h"
#include "../doomgeneric/r_main.h"
#include "../doomgeneric/w_wad.h"
#include "../doomgeneric/z_zone.h"
#include "../doomgeneric/i_swap.h"
#include "../doomgeneric/v_patch.h"
#include "../doomgeneric/tables.h"
#include "../doomgeneric/m_fixed.h"
#include "../doomgeneric/info.h"

// r_data.c private layout (mirrored — texture_t is not in a public header)
typedef struct { short originx; short originy; int patch; } glw_texpatch_t;
typedef struct glw_texture_s {
    char name[8];
    short width, height;
    int index;
    struct glw_texture_s* next;
    short patchcount;
    glw_texpatch_t patches[1];
} glw_texture_t;
extern glw_texture_t** textures;
extern int numtextures;
extern int firstpatch;

#define DOOM_TO_M (1.0f / 39.3701f)  // 1 doom unit ~ 1 inch
#define FX2F(v) ((float)(v) / 65536.0f)
#define MAX_SSEC_VERTS 64

// ---------------------------------------------------------------------------
// Shaders
// ---------------------------------------------------------------------------

static const char* kGeoVert =
    "#version 300 es\n"
    "layout(location=0) in vec3 aPos;\n"   // doom units
    "layout(location=1) in vec2 aUV;\n"
    "layout(location=2) in vec4 aCol;\n"
    "uniform mat4 uViewProj;\n"
    "uniform mat4 uModel;\n"
    "out vec2 vUV;\n"
    "out vec4 vCol;\n"
    "void main() { vUV = aUV; vCol = aCol; "
    "  gl_Position = uViewProj * uModel * vec4(aPos, 1.0); }\n";

static const char* kGeoFrag =
    "#version 300 es\n"
    "precision mediump float;\n"
    "uniform sampler2D uTex;\n"
    "uniform float uAlphaTest;\n"
    "in vec2 vUV;\n"
    "in vec4 vCol;\n"
    "out vec4 frag;\n"
    "void main() { vec4 t = texture(uTex, vUV);\n"
    "  if (uAlphaTest > 0.5 && t.a < 0.4) discard;\n"
    "  frag = vec4(t.rgb * vCol.rgb, t.a * vCol.a); }\n";

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static GLuint compile_shader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        LOGE("glw shader: %s", log);
    }
    return s;
}

static GLuint link_program(const char* vs, const char* fs) {
    GLuint v = compile_shader(GL_VERTEX_SHADER, vs);
    GLuint f = compile_shader(GL_FRAGMENT_SHADER, fs);
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(p, sizeof(log), NULL, log);
        LOGE("glw link: %s", log);
        glDeleteProgram(p);
        return 0;
    }
    glDeleteShader(v);
    glDeleteShader(f);
    return p;
}

typedef struct { float m[16]; } M4;

// ---------------------------------------------------------------------------
// Mesh builder: growable non-indexed vertex soup + per-texture draw ranges
// ---------------------------------------------------------------------------

typedef struct { float x, y, z, u, v, r, g, b, a; } GV;

typedef struct {
    GLuint tex;
    int start;   // first vertex
    int count;
    int alpha;   // 1 = alpha-tested pass
} GlwRange;

typedef struct {
    GV* v; int vn, vc;
    GlwRange* r; int rn, rc;
    GLuint curTex;
    int curAlpha;
} Mesh;

static void mesh_reserve(Mesh* m, int n) {
    if (m->vn + n > m->vc) {
        m->vc = (m->vc + n) * 2;
        m->v = realloc(m->v, m->vc * sizeof(GV));
    }
}

// Emit a quad (4 corners, ccw order irrelevant — culling disabled) as 2 tris.
static void mesh_quad(Mesh* m, const GV q[4]) {
    mesh_reserve(m, 6);
    m->v[m->vn++] = q[0]; m->v[m->vn++] = q[1]; m->v[m->vn++] = q[2];
    m->v[m->vn++] = q[0]; m->v[m->vn++] = q[2]; m->v[m->vn++] = q[3];
}

static void mesh_tex(Mesh* m, GLuint tex, int alpha) {
    if (tex == m->curTex && alpha == m->curAlpha && m->rn > 0) return;
    if (m->rn == m->rc) {
        m->rc = m->rc ? m->rc * 2 : 256;
        m->r = realloc(m->r, m->rc * sizeof(GlwRange));
    }
    GlwRange* rg = &m->r[m->rn];
    rg->tex = tex;
    rg->alpha = alpha;
    rg->start = m->vn;
    rg->count = 0;
    m->rn++;
    m->curTex = tex;
    m->curAlpha = alpha;
}

static void mesh_bump_count(Mesh* m, int n) {
    if (m->rn > 0) m->r[m->rn - 1].count += n;
}

// ---------------------------------------------------------------------------
// Texture decoding (palette -> RGBA GL textures, cached by lump/num)
// ---------------------------------------------------------------------------

typedef struct {
    int key;          // kind*1M + num ; kind 0=texture 1=flat 2=lump(patch)
    GLuint id;
    int w, h;
} TexEntry;

struct GlWorld {
    GLuint program;
    GLuint vao, vbo;
    Mesh mesh;
    TexEntry* tex; int ntex, ctex;
    byte* palette;    // 256*3
    float model[16];
    int built;        // geometry built this level
    int lastLevelKey;
    float camX, camY; // doom units, for sprite facing
    int statWalls, statFlats, statSprites;
    unsigned long statFrame;
    const char* fail;
};

static void palette_init(GlWorld* w) {
    if (w->palette) return;
    int lump = W_CheckNumForName("PLAYPAL");
    if (lump < 0) return;
    w->palette = W_CacheLumpNum(lump, PU_CACHE);
}

static GLuint rgba_upload(const byte* rgba, int tw, int th, int wrap) {
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tw, th, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
    return t;
}

static void pal_to_rgb(byte* dst, const byte* src, int n, const byte* pal,
                       byte alpha) {
    for (int i = 0; i < n; i++) {
        const byte* c = pal + src[i] * 3;
        dst[i * 4 + 0] = c[0];
        dst[i * 4 + 1] = c[1];
        dst[i * 4 + 2] = c[2];
        dst[i * 4 + 3] = alpha;
    }
}

// Decode a DOOM patch (column-post format) into RGBA with transparency.
static byte* decode_patch(int lump, int* ow, int* oh, const byte* pal) {
    patch_t* p = W_CacheLumpNum(lump, PU_CACHE);
    int pw = SHORT(p->width), ph = SHORT(p->height);
    byte* out = calloc(pw * ph, 4);
    if (!out) return NULL;
    for (int x = 0; x < pw; x++) {
        column_t* col = (column_t*)((byte*)p + LONG(p->columnofs[x]));
        while (col->topdelta != 0xff) {
            int top = col->topdelta;
            int len = col->length;
            const byte* src = (const byte*)col + 3;
            for (int i = 0; i < len; i++) {
                int y = top + i;
                if (y < 0 || y >= ph) continue;
                const byte* c = pal + src[i] * 3;
                byte* d = out + (y * pw + x) * 4;
                d[0] = c[0]; d[1] = c[1]; d[2] = c[2]; d[3] = 255;
            }
            col = (column_t*)((byte*)col + len + 4);
        }
    }
    *ow = pw;
    *oh = ph;
    return out;
}

static GLuint tex_for_key(GlWorld* w, int key, int* outW, int* outH) {
    for (int i = 0; i < w->ntex; i++)
        if (w->tex[i].key == key) {
            if (outW) *outW = w->tex[i].w;
            if (outH) *outH = w->tex[i].h;
            return w->tex[i].id;
        }
    if (!w->palette) { if (outW) *outW = 1; if (outH) *outH = 1; return 0; }

    GLuint id = 0;
    int tw = 0, th = 0;
    int kind = key / 1000000;
    int num = key % 1000000;
    if (kind == 1) {                       // flat: 64x64 indexed
        int lump = firstflat + num;
        byte* idx = W_CacheLumpNum(lump, PU_CACHE);
        byte* rgb = malloc(64 * 64 * 4);
        if (rgb) {
            pal_to_rgb(rgb, idx, 64 * 64, w->palette, 255);
            id = rgba_upload(rgb, 64, 64, GL_REPEAT);
            free(rgb);
            tw = th = 64;
        }
    } else if (kind == 0 && num >= 0 && num < numtextures) {  // wall texture
        glw_texture_t* t = textures[num];
        tw = t->width;
        th = t->height;
        byte* rgb = calloc(tw * th, 4);
        if (rgb) {
            for (int i = 0; i < t->patchcount; i++) {
                int pw, ph;
                byte* px = decode_patch(t->patches[i].patch, &pw, &ph,
                                        w->palette);
                if (!px) continue;
                for (int y = 0; y < ph; y++) {
                    int dy = y + t->patches[i].originy;
                    if (dy < 0 || dy >= th) continue;
                    for (int x = 0; x < pw; x++) {
                        int dx = x + t->patches[i].originx;
                        if (dx < 0 || dx >= tw) continue;
                        const byte* s = px + (y * pw + x) * 4;
                        if (s[3] == 0) continue;
                        byte* d = rgb + (dy * tw + dx) * 4;
                        memcpy(d, s, 4);
                    }
                }
                free(px);
            }
            id = rgba_upload(rgb, tw, th, GL_REPEAT);
            free(rgb);
        }
    } else if (kind == 2) {                // raw patch lump (sprite)
        byte* px = decode_patch(num, &tw, &th, w->palette);
        if (px) {
            id = rgba_upload(px, tw, th, GL_CLAMP_TO_EDGE);
            free(px);
        }
    }
    if (w->ntex == w->ctex) {
        w->ctex = w->ctex ? w->ctex * 2 : 64;
        w->tex = realloc(w->tex, w->ctex * sizeof(TexEntry));
    }
    w->tex[w->ntex++] = (TexEntry){key, id, tw, th};
    if (outW) *outW = tw;
    if (outH) *outH = th;
    return id;
}

// ---------------------------------------------------------------------------
// Geometry generation
// ---------------------------------------------------------------------------

static float sector_light(const sector_t* s) {
    float l = s->lightlevel / 255.0f;
    return l < 0.15f ? 0.15f : (l > 1.0f ? 1.0f : l);
}

// Wall quad between two heights on one seg, texture `texNum`.
static void emit_wall(Mesh* m, GlWorld* w,
                      const side_t* side, const sector_t* sec,
                      float x1, float y1, float x2, float y2,
                      float zLo, float zHi, int texNum,
                      float u0, float u1, int alpha) {
    if (texNum < 0 || zHi <= zLo) return;
    int tex = texturetranslation[texNum];
    int tw = 0, th = 0;
    GLuint gt = tex_for_key(w, tex, &tw, &th);
    if (!gt || tw <= 0 || th <= 0) return;
    float l = sector_light(sec);
    mesh_tex(m, gt, alpha);
    float vOff = FX2F(side->rowoffset) / (float)th;
    float v0 = vOff;                        // v=0 = texture top row
    float v1 = vOff + (zHi - zLo) / (float)th;
    GV q[4] = {
        {x1, y1, zLo, u0 / tw, v1, l, l, l, 1.f},
        {x2, y2, zLo, u1 / tw, v1, l, l, l, 1.f},
        {x2, y2, zHi, u1 / tw, v0, l, l, l, 1.f},
        {x1, y1, zHi, u0 / tw, v0, l, l, l, 1.f},
    };
    mesh_quad(m, q);
    mesh_bump_count(m, 6);
    w->statWalls++;
}

static void build_walls(GlWorld* w, Mesh* m) {
    for (int i = 0; i < numsubsectors; i++) {
        const subsector_t* ss = &subsectors[i];
        for (int j = 0; j < ss->numlines; j++) {
            const seg_t* seg = &segs[ss->firstline + j];
            const line_t* line = seg->linedef;
            const side_t* side = seg->sidedef;
            if (!line || !side) continue;    // miniseg: no wall surface

            const sector_t* front = side->sector;
            // neighbour sector across the line (NULL = one-sided)
            const sector_t* back;
            if (side == &sides[line->sidenum[0]])
                back = (line->sidenum[1] >= 0) ? line->backsector : NULL;
            else
                back = line->frontsector;
            if (!front) continue;

            float x1 = FX2F(seg->v1->x), y1 = FX2F(seg->v1->y);
            float x2 = FX2F(seg->v2->x), y2 = FX2F(seg->v2->y);

            // distance of each seg endpoint along the parent line, for U
            float lx1 = FX2F(line->v1->x), ly1 = FX2F(line->v1->y);
            float ldx = FX2F(line->dx), ldy = FX2F(line->dy);
            float llen = sqrtf(ldx * ldx + ldy * ldy);
            float u0 = 0, u1 = 0, toff = FX2F(side->textureoffset);
            if (llen > 1e-4f) {
                float nx = ldx / llen, ny = ldy / llen;
                u0 = toff + ((x1 - lx1) * nx + (y1 - ly1) * ny);
                u1 = toff + ((x2 - lx1) * nx + (y2 - ly1) * ny);
            }

            float ff = FX2F(front->floorheight);
            float fc = FX2F(front->ceilingheight);
            if (!back) {
                emit_wall(m, w, side, front, x1, y1, x2, y2,
                          ff, fc, side->midtexture, u0, u1, 0);
                continue;
            }
            float bf = FX2F(back->floorheight);
            float bc = FX2F(back->ceilingheight);
            // upper / lower / masked-mid
            emit_wall(m, w, side, front, x1, y1, x2, y2,
                      bc, fc, side->toptexture, u0, u1, 0);
            emit_wall(m, w, side, front, x1, y1, x2, y2,
                      ff, bf, side->bottomtexture, u0, u1, 0);
            float midLo = ff > bf ? ff : bf;
            float midHi = fc < bc ? fc : bc;
            if (side->midtexture >= 0 && midHi > midLo)
                emit_wall(m, w, side, front, x1, y1, x2, y2,
                          midLo, midHi, side->midtexture, u0, u1, 1);
        }
    }
}

static float s_sortCx, s_sortCy;

static int cmp_angle2(const void* a, const void* b) {
    const float* pa = a;
    const float* pb = b;
    float aa = atan2f(pa[1] - s_sortCy, pa[0] - s_sortCx);
    float ab = atan2f(pb[1] - s_sortCy, pb[0] - s_sortCx);
    return (aa < ab) ? -1 : (aa > ab);
}

static void build_flats(GlWorld* w, Mesh* m) {
    float pts[MAX_SSEC_VERTS][2];
    for (int i = 0; i < numsubsectors; i++) {
        const subsector_t* ss = &subsectors[i];
        const sector_t* sec = ss->sector;
        if (!sec) continue;

        // collect unique ring points (seg endpoints bound the convex leaf)
        int np = 0;
        for (int j = 0; j < ss->numlines && np < MAX_SSEC_VERTS; j++) {
            const seg_t* seg = &segs[ss->firstline + j];
            for (int e = 0; e < 2 && np < MAX_SSEC_VERTS; e++) {
                const vertex_t* v = e ? seg->v2 : seg->v1;
                float px = FX2F(v->x), py = FX2F(v->y);
                int dup = 0;
                for (int k = 0; k < np; k++)
                    if (fabsf(pts[k][0] - px) < 0.5f &&
                        fabsf(pts[k][1] - py) < 0.5f) { dup = 1; break; }
                if (!dup) { pts[np][0] = px; pts[np][1] = py; np++; }
            }
        }
        if (np < 3) continue;
        int tris = np - 2;

        float cx = 0, cy = 0;
        for (int k = 0; k < np; k++) { cx += pts[k][0]; cy += pts[k][1]; }
        cx /= np; cy /= np;
        s_sortCx = cx; s_sortCy = cy;
        qsort(pts, np, sizeof(pts[0]), cmp_angle2);

        float l = sector_light(sec);
        float fz = FX2F(sec->floorheight);
        float cz = FX2F(sec->ceilingheight);
        int fnum = flattranslation[sec->floorpic];
        int cnum = flattranslation[sec->ceilingpic];

        if (sec->floorpic != skyflatnum) {
            GLuint gt = tex_for_key(w, 1000000 + fnum, NULL, NULL);
            if (gt) {
                mesh_tex(m, gt, 0);
                for (int k = 1; k + 1 < np; k++) {
                    GV t[3] = {
                        {pts[0][0], pts[0][1], fz, pts[0][0] / 64.f,
                         pts[0][1] / 64.f, l, l, l, 1.f},
                        {pts[k][0], pts[k][1], fz, pts[k][0] / 64.f,
                         pts[k][1] / 64.f, l, l, l, 1.f},
                        {pts[k + 1][0], pts[k + 1][1], fz,
                         pts[k + 1][0] / 64.f, pts[k + 1][1] / 64.f,
                         l, l, l, 1.f},
                    };
                    mesh_reserve(m, 3);
                    m->v[m->vn++] = t[0];
                    m->v[m->vn++] = t[1];
                    m->v[m->vn++] = t[2];
                    mesh_bump_count(m, 3);
                }
                w->statFlats += tris;
            }
        }
        if (sec->ceilingpic != skyflatnum) {
            GLuint gt = tex_for_key(w, 1000000 + cnum, NULL, NULL);
            if (gt) {
                mesh_tex(m, gt, 0);
                for (int k = 1; k + 1 < np; k++) {
                    mesh_reserve(m, 3);
                    m->v[m->vn++] = (GV){pts[0][0], pts[0][1], cz,
                        pts[0][0] / 64.f, pts[0][1] / 64.f, l, l, l, 1.f};
                    m->v[m->vn++] = (GV){pts[k + 1][0], pts[k + 1][1], cz,
                        pts[k + 1][0] / 64.f, pts[k + 1][1] / 64.f,
                        l, l, l, 1.f};
                    m->v[m->vn++] = (GV){pts[k][0], pts[k][1], cz,
                        pts[k][0] / 64.f, pts[k][1] / 64.f, l, l, l, 1.f};
                    mesh_bump_count(m, 3);
                }
                w->statFlats += tris;
            }
        }
    }
}

static void build_sprites(GlWorld* w, Mesh* m) {
    fixed_t camX = (fixed_t)(w->camX * FRACUNIT);
    fixed_t camY = (fixed_t)(w->camY * FRACUNIT);
    mobj_t* playerMo = players[consoleplayer].mo;

    for (thinker_t* th = thinkercap.next; th != &thinkercap; th = th->next) {
        if (th->function.acp1 != (actionf_p1)P_MobjThinker) continue;
        mobj_t* mo = (mobj_t*)th;
        if (mo == playerMo) continue;                  // camera is inside it
        if (mo->sprite < 0 || mo->sprite >= numsprites) continue;
        if (mo->subsector == NULL || mo->subsector->sector == NULL) continue;

        spritedef_t* sd = &sprites[mo->sprite];
        int frame = mo->frame & FF_FRAMEMASK;
        if (frame >= sd->numframes) frame = 0;
        spriteframe_t* fr = &sd->spriteframes[frame];

        int lumpIdx, flip;
        if (fr->rotate) {
            angle_t ang = R_PointToAngle2(camX, camY, mo->x, mo->y);
            unsigned rot =
                (ang - mo->angle + (unsigned)(ANG45 / 2) * 9) >> 29;
            lumpIdx = fr->lump[rot];
            flip = fr->flip[rot];
        } else {
            lumpIdx = fr->lump[0];
            flip = fr->flip[0];
        }
        int lump = firstspritelump + lumpIdx;
        int pw = 0, ph = 0;
        GLuint gt = tex_for_key(w, 2000000 + lump, &pw, &ph);
        if (!gt || pw <= 0 || ph <= 0) continue;

        float x = FX2F(mo->x), y = FX2F(mo->y);
        float zBot = FX2F(mo->z);
        float zTop = zBot + FX2F(spritetopoffset[lumpIdx]);
        float halfW = FX2F(spritewidth[lumpIdx]) * 0.5f;

        // cylindrical billboard: right vector = perp of (thing - camera)
        float dx = x - w->camX, dy = y - w->camY;
        float dlen = sqrtf(dx * dx + dy * dy);
        if (dlen < 1e-3f) continue;
        float rxv = -dy / dlen, ryv = dx / dlen;

        const sector_t* sec = mo->subsector->sector;
        float l = (mo->frame & FF_FULLBRIGHT) ? 1.0f : sector_light(sec);
        float a = (mo->flags & MF_SHADOW) ? 0.35f : 1.0f;

        float uA = flip ? 1.0f : 0.0f;
        float uB = flip ? 0.0f : 1.0f;
        mesh_tex(m, gt, 1);
        GV q[4] = {
            {x - rxv * halfW, y - ryv * halfW, zBot, uA, 1.f, l, l, l, a},
            {x + rxv * halfW, y + ryv * halfW, zBot, uB, 1.f, l, l, l, a},
            {x + rxv * halfW, y + ryv * halfW, zTop, uB, 0.f, l, l, l, a},
            {x - rxv * halfW, y - ryv * halfW, zTop, uA, 0.f, l, l, l, a},
        };
        mesh_quad(m, q);
        mesh_bump_count(m, 6);
        w->statSprites++;
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool glw_init(GlWorld** out) {
    GlWorld* w = calloc(1, sizeof(GlWorld));
    if (!w) return false;
    w->program = link_program(kGeoVert, kGeoFrag);
    if (!w->program) { free(w); return false; }
    glGenVertexArrays(1, &w->vao);
    glGenBuffers(1, &w->vbo);
    glBindVertexArray(w->vao);
    glBindBuffer(GL_ARRAY_BUFFER, w->vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(GV), NULL);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(GV),
                          (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(GV),
                          (void*)(5 * sizeof(float)));
    glBindVertexArray(0);
    w->lastLevelKey = -1;
    *out = w;
    return true;
}

bool glw_available(const GlWorld* w) {
    return w && w->built;
}

const char* glw_fail_reason(const GlWorld* w) {
    return w ? w->fail : "no glw instance";
}

void glw_begin_frame(GlWorld* w) {
    w->built = 0;
    if (!w) { return; }
    w->fail = NULL;
    if (!sectors || numsectors <= 0 || !subsectors || !segs) {
        w->fail = "no level data";
        return;
    }
    if (!sprites || numsprites <= 0) { w->fail = "no sprites"; return; }
    if (!texturetranslation || !flattranslation) {
        w->fail = "no texture tables";
        return;
    }

    palette_init(w);
    if (!w->palette) { w->fail = "no PLAYPAL"; return; }

    w->statWalls = w->statFlats = w->statSprites = 0;

    Mesh* m = &w->mesh;
    m->vn = 0;
    m->rn = 0;
    m->curTex = 0;
    m->curAlpha = -1;

    // camera doom pos needed by sprite facing; stored by glw_set_camera before
    build_walls(w, m);
    build_flats(w, m);
    build_sprites(w, m);

    w->built = (m->vn > 0);
    if (!w->built) { w->fail = "empty mesh"; return; }

    w->statFrame++;
    if ((w->statFrame % 240) == 1 || w->statFrame == 1)
        LOGI("QuestDOOM: RENDER_MODE: 3D_WORLD (drawn %d walls, %d flats, "
             "%d sprites | verts=%d ranges=%d glTex=%d)",
             w->statWalls, w->statFlats, w->statSprites, m->vn, m->rn,
             w->ntex);
}

// Sprite-facing camera (head-centred, doom map units). Call before
// glw_begin_frame; the per-eye model matrix comes from glw_set_camera.
void glw_set_frame_camera(GlWorld* w, float camXu, float camYu) {
    w->camX = camXu;
    w->camY = camYu;
}

void glw_set_camera(GlWorld* w,
                    float headX, float headY, float headZ, float headYawDeg,
                    float camXu, float camYu, float camZu, float moAngleDeg) {
    w->camX = camXu;
    w->camY = camYu;

    // doom->app: scale to metres, map doom (x,y,z) -> app (x,z,-y), yaw so
    // that the player's facing direction matches the head's real yaw, and
    // translate so the doom camera lands on the real head position.
    float alpha = (headYawDeg + 90.0f - moAngleDeg) * (float)M_PI / 180.0f;
    float ca = cosf(alpha), sa = sinf(alpha);
    float k = DOOM_TO_M;

    // prelim app pos of doom camera (scaled, axis-mapped, pre-rotation)
    float px = camXu * k, py = camZu * k, pz = -camYu * k;
    // rotated
    float rx = px * ca + pz * sa;
    float ry = py;
    float rz = -px * sa + pz * ca;
    float tx = headX - rx, ty = headY - ry, tz = headZ - rz;

    // M = T(t) * R_y(alpha) * S(k) * M0 ; M0: (x,y,z)->(x,z,-y)
    M4 M = {{0}};
    // columns of R_y*S*M0:
    // doom x -> R_y * (k,0,0) = (k*ca, 0, -k*sa)
    // doom y -> R_y * (0,0,-k) = (-k*sa, 0, -k*ca)
    // doom z -> R_y * (0,k,0) = (0, k, 0)
    M.m[0] = k * ca;  M.m[1] = 0.f;  M.m[2] = -k * sa;  M.m[3] = 0.f;
    M.m[4] = -k * sa; M.m[5] = 0.f;  M.m[6] = -k * ca;  M.m[7] = 0.f;
    M.m[8] = 0.f;     M.m[9] = k;    M.m[10] = 0.f;    M.m[11] = 0.f;
    M.m[12] = tx;     M.m[13] = ty;  M.m[14] = tz;     M.m[15] = 1.f;
    memcpy(w->model, M.m, sizeof(M.m));
}

void glw_draw(GlWorld* w, const float* viewProj) {
    if (!w || !w->built || w->mesh.vn == 0) return;

    glUseProgram(w->program);
    glUniformMatrix4fv(glGetUniformLocation(w->program, "uViewProj"),
                       1, GL_FALSE, viewProj);
    glUniformMatrix4fv(glGetUniformLocation(w->program, "uModel"),
                       1, GL_FALSE, w->model);
    glUniform1i(glGetUniformLocation(w->program, "uTex"), 0);
    GLint locAlpha = glGetUniformLocation(w->program, "uAlphaTest");

    glBindVertexArray(w->vao);
    glBindBuffer(GL_ARRAY_BUFFER, w->vbo);
    glBufferData(GL_ARRAY_BUFFER, w->mesh.vn * sizeof(GV), w->mesh.v,
                 GL_STREAM_DRAW);

    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);

    glActiveTexture(GL_TEXTURE0);
    for (int i = 0; i < w->mesh.rn; i++) {
        GlwRange* rg = &w->mesh.r[i];
        if (rg->count <= 0 || rg->alpha) continue;
        glBindTexture(GL_TEXTURE_2D, rg->tex);
        glUniform1f(locAlpha, 0.0f);
        glDrawArrays(GL_TRIANGLES, rg->start, rg->count);
    }
    // alpha-tested pass (masked walls + sprites)
    for (int i = 0; i < w->mesh.rn; i++) {
        GlwRange* rg = &w->mesh.r[i];
        if (rg->count <= 0 || !rg->alpha) continue;
        glBindTexture(GL_TEXTURE_2D, rg->tex);
        glUniform1f(locAlpha, 1.0f);
        glDrawArrays(GL_TRIANGLES, rg->start, rg->count);
    }
    glBindVertexArray(0);
}

void glw_shutdown(GlWorld* w) {
    if (!w) return;
    for (int i = 0; i < w->ntex; i++)
        if (w->tex[i].id) glDeleteTextures(1, &w->tex[i].id);
    free(w->tex);
    free(w->mesh.v);
    free(w->mesh.r);
    if (w->vao) glDeleteVertexArrays(1, &w->vao);
    if (w->vbo) glDeleteBuffers(1, &w->vbo);
    if (w->program) glDeleteProgram(w->program);
    free(w);
}
