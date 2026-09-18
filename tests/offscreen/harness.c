// Offscreen visual-verification harness for the backend-neutral renderer.
//
// Runs the real doomgeneric engine + gl_world + gl_renderer code paths on
// desktop Mesa EGL (surfaceless/pbuffer), renders the Cardboard-style
// side-by-side stereo frame to a fake "phone" framebuffer, dumps RGBA raw
// files for PNG conversion, and prints assertions about geometry counts.
//
// Build/run: see tests/offscreen/build_harness.sh
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "doomgeneric/doomgeneric.h"
#include "doomgeneric/doomstat.h"
#include "doomgeneric/d_player.h"
#include "doomgeneric/p_mobj.h"
#include "doomgeneric/vr_doom.h"
#include "quest/gl_renderer.h"

#define SURF_W 2160
#define SURF_H 1080

static GlRenderer g_r;
static GLuint g_dumpFbo;
static GLuint g_dumpTex;

// --- doomgeneric platform hooks -------------------------------------------

void DG_Init(void) {}

void DG_DrawFrame(void) {
    glr_upload_frame(&g_r, vr_eye, (const uint32_t*)DG_ScreenBuffer,
                     DOOMGENERIC_RESX, DOOMGENERIC_RESY);
}

void DG_SleepMs(uint32_t ms) { usleep(ms * 1000); }

uint32_t DG_GetTicksMs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

int DG_GetKey(int* pressed, unsigned char* doomKey) {
    (void)pressed; (void)doomKey; return 0;
}

void DG_SetWindowTitle(const char* t) { (void)t; }

// --- EGL boilerplate --------------------------------------------------------

static void egl_setup(void) {
    EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (d == EGL_NO_DISPLAY || !eglInitialize(d, 0, 0)) {
        fprintf(stderr, "eglInitialize failed\n");
        exit(1);
    }
    EGLint ca[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                   EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                   EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
                   EGL_DEPTH_SIZE, 24, EGL_NONE};
    EGLConfig c;
    EGLint n = 0;
    eglChooseConfig(d, ca, &c, 1, &n);
    if (!n) { fprintf(stderr, "no egl config\n"); exit(1); }
    EGLint ctxa[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    EGLContext ctx = eglCreateContext(d, c, 0, ctxa);
    EGLint pa[] = {EGL_WIDTH, SURF_W, EGL_HEIGHT, SURF_H, EGL_NONE};
    EGLSurface s = eglCreatePbufferSurface(d, c, pa);
    eglMakeCurrent(d, s, s, ctx);
    printf("GL: %s\n", glGetString(GL_VERSION));
}

// --- output -----------------------------------------------------------------

static void dump_pixels(const char* path) {
    static unsigned char px[SURF_W * SURF_H * 4];
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glReadPixels(0, 0, SURF_W, SURF_H, GL_RGBA, GL_UNSIGNED_BYTE, px);
    FILE* f = fopen(path, "wb");
    fwrite(px, 4, SURF_W * SURF_H, f);
    fclose(f);
    printf("wrote %s (%d x %d RGBA, GL bottom-up rows)\n",
           path, SURF_W, SURF_H);
}

// Cardboard-style eye params (mirrors cb_eye_params): half-width viewport,
// symmetric FOV, identity/rotated head quat, ±32mm IPD, no targetTex.
static void eye_params(int eye, float qw, float qx, float qy, float qz,
                       float rollDeg, GlrEyeParams* out) {
    memset(out, 0, sizeof(*out));
    int hw = SURF_W / 2;
    out->vpX = (eye == 0) ? 0 : hw;
    out->vpY = 0;
    out->vpW = hw;
    out->vpH = SURF_H;
    out->targetTex = 0;
    out->srcEye = eye;

    float aspect = (float)hw / (float)SURF_H;
    float tanV = tanf(30.0f * (float)M_PI / 180.0f);
    float tanH = aspect * tanV;
    GlMat4 proj = glmat_projection(-tanH, tanH, tanV, -tanV, 0.05f, 400.0f);
    memcpy(out->proj, proj.m, sizeof(proj.m));

    GlMat4 head = glmat_pose(qx, qy, qz, qw, 0, 0, 0);
    float ex = (eye == 0) ? -0.032f : 0.032f;
    GlMat4 view = glmat_mul(glmat_roll_z(rollDeg),
                            glmat_mul(glmat_translate(-ex, 0, 0),
                                      glmat_invert_rigid(head)));
    memcpy(out->view, view.m, sizeof(view.m));
}

int main(int argc, char** argv) {
    const char* waddir = (argc > 1) ? argv[1] : ".";
    if (chdir(waddir) != 0) { perror("chdir"); return 1; }

    egl_setup();
    if (!glr_init(&g_r)) { fprintf(stderr, "glr_init failed\n"); return 1; }

    char* av[] = {"qd", "-iwad", "doom1.wad", NULL};
    doomgeneric_Create(3, av);
    VR_EnableStereo();

    // Tick until a level with a player mobj is live (attract demo loads E1M1).
    int iters = 0;
    while (!(gamestate == GS_LEVEL && players[consoleplayer].mo) &&
           iters < 2000) {
        doomgeneric_Tick();
        usleep(8000);
        iters++;
    }
    if (!(gamestate == GS_LEVEL && players[consoleplayer].mo)) {
        fprintf(stderr, "FAIL: no GS_LEVEL after %d iters (gs=%d)\n",
                iters, gamestate);
        return 2;
    }
    printf("level live after %d ticks; gamestate=%d demoplayback=%d\n",
           iters, gamestate, demoplayback);

    mobj_t* mo = players[consoleplayer].mo;
    float camX = (float)mo->x / 65536.0f, camY = (float)mo->y / 65536.0f;
    float camZ = (float)players[consoleplayer].viewz / 65536.0f;
    float moAng = (float)((double)mo->angle * (360.0 / 4294967296.0));
    printf("player at doom(%.0f,%.0f) z=%.1f ang=%.1f\n",
           camX, camY, camZ, moAng);

    // ---- test 1: identity head quat, no roll fix, world 3D mode ----
    glr_set_immersive(&g_r, true);
    glr_set_world_mode(&g_r, true);
    glr_world_frame_camera(&g_r, camX, camY);
    glr_world_begin_frame(&g_r);
    printf("glw_available=%d fail=%s\n", glr_world_active(&g_r),
           glr_world_fail(&g_r));
    glr_world_camera(&g_r, 0, 0, 0, 0, camX, camY, camZ, moAng);

    for (int eye = 0; eye < 2; eye++) {
        GlrEyeParams p;
        eye_params(eye, 1, 0, 0, 0, 0.0f, &p);
        glr_draw_eye_params(&g_r, &p);
    }
    glFinish();
    dump_pixels("world_identity.rgba");

    // ---- test 2: head yawed 90 deg (y-rotation quat) ----
    float s45 = sinf(45.0f * (float)M_PI / 180.0f), c45 = cosf(45.0f * (float)M_PI / 180.0f);
    // head yaw=90 -> model keeps doom forward aligned with head forward
    glr_world_camera(&g_r, 0, 0, 0, 90.0f, camX, camY, camZ, moAng);
    for (int eye = 0; eye < 2; eye++) {
        GlrEyeParams p;
        eye_params(eye, c45, 0, s45, 0, 0.0f, &p);
        glr_draw_eye_params(&g_r, &p);
    }
    glFinish();
    dump_pixels("world_yaw90.rgba");

    // ---- test 3: immersive quad mode (software frame, V-flip path) ----
    glr_set_world_mode(&g_r, false);   // immersive clip-quad
    for (int eye = 0; eye < 2; eye++) {
        GlrEyeParams p;
        eye_params(eye, 1, 0, 0, 0, 0.0f, &p);
        glr_draw_eye_params(&g_r, &p);
    }
    glFinish();
    dump_pixels("quad_immersive.rgba");

    // ---- test 4: world-locked panel (non-level path), all UV flips ----
    glr_set_immersive(&g_r, false);
    const char* names[4] = {"00", "10", "01", "11"};
    float flips[4][2] = {{0,0},{1,0},{0,1},{1,1}};
    for (int v = 0; v < 4; v++) {
        g_r.panelPlaced = false;
        glr_set_panel_uv_flip(&g_r, flips[v][0], flips[v][1]);
        for (int eye = 0; eye < 2; eye++) {
            GlrEyeParams p;
            eye_params(eye, 1, 0, 0, 0, 0.0f, &p);
            glr_draw_eye_params(&g_r, &p);
        }
        glFinish();
        char path[64];
        snprintf(path, sizeof(path), "panel_uv%s.rgba", names[v]);
        dump_pixels(path);
    }

    printf("DONE\n");
    return 0;
}
