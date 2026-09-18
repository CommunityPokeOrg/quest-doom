// doomgeneric platform layer for Meta Quest (OpenXR + Android NDK).
//
// android_main -> OpenXR session -> doomgeneric_Create -> per-frame:
//   xrSyncActions -> doomgeneric_Tick -> upload framebuffer -> stereo panel render
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

#include <android/asset_manager.h>
#include <android/log.h>

#include "../doomgeneric/doomgeneric.h"
#include "../doomgeneric/doomkeys.h"
#include "../doomgeneric/doomstat.h"
#include "../doomgeneric/d_player.h"
#include "../doomgeneric/p_mobj.h"
#include "../doomgeneric/vr_doom.h"
#include "xr_engine.h"
#include "xr_input.h"
#include "xr_hands.h"
#include "gl_renderer.h"
#include "gl_xr.h"
#include "cb_engine.h"

#define KEYQUEUE_SIZE 64

static XrEngine g_xr;
static XrInput g_input;
static XrHands g_hands;
static GlRenderer g_renderer;
static CbEngine g_cb;
static struct android_app* g_app;
static bool g_doomStarted = false;
static bool g_doomExited = false;

static unsigned short s_KeyQueue[KEYQUEUE_SIZE];
static unsigned int s_KeyQueueWriteIndex = 0;
static unsigned int s_KeyQueueReadIndex = 0;

static void push_key(int pressed, unsigned char key, void* user) {
    (void)user;
    s_KeyQueue[s_KeyQueueWriteIndex] = (unsigned short)((pressed << 8) | key);
    s_KeyQueueWriteIndex = (s_KeyQueueWriteIndex + 1) % KEYQUEUE_SIZE;
    if (s_KeyQueueWriteIndex == s_KeyQueueReadIndex)
        s_KeyQueueReadIndex = (s_KeyQueueReadIndex + 1) % KEYQUEUE_SIZE;
}

// ---------------------------------------------------------------------------
// stdout/stderr -> logcat (doom prints diagnostics via printf)
// ---------------------------------------------------------------------------

static void* log_thread(void* arg) {
    int fd = *(int*)arg;
    char buf[512];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        __android_log_write(ANDROID_LOG_INFO, "DoomCore", buf);
    }
    return NULL;
}

static void redirect_stdio_to_logcat(void) {
    int p[2];
    if (pipe(p) != 0) return;
    dup2(p[1], STDOUT_FILENO);
    dup2(p[1], STDERR_FILENO);
    setvbuf(stdout, NULL, _IOLBF, 0);
    pthread_t t;
    if (pthread_create(&t, NULL, log_thread, &p[0]) == 0)
        pthread_detach(t);
}

// ---------------------------------------------------------------------------
// IWAD handling: extract bundled WADs from assets, then pick the best IWAD
// ---------------------------------------------------------------------------

static const char* kIwadNames[] = {
    "doom2.wad", "doom.wad", "doom1.wad", "plutonia.wad", "tnt.wad",
    "freedoom2.wad", "freedoom1.wad", "chex.wad", "hacx.wad", NULL,
};

static void extract_assets_wads(struct android_app* app, const char* destDir) {
    AAssetManager* am = app->activity->assetManager;
    AAssetDir* dir = AAssetManager_openDir(am, "wads");
    if (!dir) return;
    const char* name;
    while ((name = AAssetDir_getNextFileName(dir)) != NULL) {
        char dest[512];
        snprintf(dest, sizeof(dest), "%s/%s", destDir, name);
        struct stat st;
        if (stat(dest, &st) == 0) continue;  // already extracted

        char rel[256];
        snprintf(rel, sizeof(rel), "wads/%s", name);
        AAsset* a = AAssetManager_open(am, rel, AASSET_MODE_STREAMING);
        if (!a) continue;
        FILE* out = fopen(dest, "wb");
        if (out) {
            char buf[8192];
            int rd;
            while ((rd = AAsset_read(a, buf, sizeof(buf))) > 0)
                fwrite(buf, 1, rd, out);
            fclose(out);
            LOGI("extracted IWAD asset %s", dest);
        }
        AAsset_close(a);
    }
    AAssetDir_close(dir);
}

static const char* find_iwad(const char* dir, char* out, size_t outSize) {
    for (int i = 0; kIwadNames[i]; i++) {
        snprintf(out, outSize, "%s/%s", dir, kIwadNames[i]);
        if (access(out, R_OK) == 0) return out;
    }
    // also check cwd (we chdir'd into files dir, same thing) and /sdcard
    for (int i = 0; kIwadNames[i]; i++) {
        snprintf(out, outSize, "/sdcard/%s", kIwadNames[i]);
        if (access(out, R_OK) == 0) return out;
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// doomgeneric platform hooks
// ---------------------------------------------------------------------------

void DG_Init(void) {
    // OpenXR + GL are initialized in android_main before doomgeneric_Create.
}

void DG_DrawFrame(void) {
    // Called once per rendered game frame (twice per tick in stereo mode,
    // once per eye): upload the current eye's framebuffer to its texture.
    // EGL context is current on this thread.
    glr_upload_frame(&g_renderer, vr_eye, (const uint32_t*)DG_ScreenBuffer,
                     DOOMGENERIC_RESX, DOOMGENERIC_RESY);
}

void DG_SleepMs(uint32_t ms) { usleep(ms * 1000); }

uint32_t DG_GetTicksMs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

int DG_GetKey(int* pressed, unsigned char* doomKey) {
    if (s_KeyQueueReadIndex == s_KeyQueueWriteIndex) return 0;
    unsigned short kd = s_KeyQueue[s_KeyQueueReadIndex];
    s_KeyQueueReadIndex = (s_KeyQueueReadIndex + 1) % KEYQUEUE_SIZE;
    *pressed = kd >> 8;
    *doomKey = kd & 0xFF;
    return 1;
}

void DG_SetWindowTitle(const char* title) { (void)title; }

// ---------------------------------------------------------------------------
// Pose extraction: quaternion -> yaw/pitch (forward = -Z in XR space)
// ---------------------------------------------------------------------------

// Latest head pose in app space (for the true-3D world camera).
static float s_headX, s_headY, s_headZ, s_headYawDeg;

static void quat_to_yawpitch(XrQuaternionf q, float* yawDeg, float* pitchDeg) {
    float fx = -2.0f * (q.y * q.w + q.x * q.z);
    float fy =  2.0f * (q.x * q.w - q.y * q.z);
    float fz = -1.0f + 2.0f * (q.x * q.x + q.y * q.y);
    *yawDeg = atan2f(-fx, -fz) * (180.0f / (float)M_PI);
    *pitchDeg = asinf(fy > 1.0f ? 1.0f : (fy < -1.0f ? -1.0f : fy))
                * (180.0f / (float)M_PI);
}

// Feed latest head/eye/aim poses into the doom side VR layer.
static void update_vr_poses(void) {
    // Head pose: locate the view space in local space.
    XrSpaceLocation headLoc = {.type = XR_TYPE_SPACE_LOCATION};
    if (XR_FAILED(xrLocateSpace(g_xr.viewSpace, g_xr.appSpace,
                                g_xr.frameState.predictedDisplayTime,
                                &headLoc)))
        return;
    if (!(headLoc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) ||
        !(headLoc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
        return;

    float yaw, pitch;
    quat_to_yawpitch(headLoc.pose.orientation, &yaw, &pitch);
    s_headX = headLoc.pose.position.x;
    s_headY = headLoc.pose.position.y;
    s_headZ = headLoc.pose.position.z;
    s_headYawDeg = yaw;
    VR_SetHeadPose(headLoc.pose.position.x, headLoc.pose.position.y,
                   headLoc.pose.position.z, yaw, pitch);

    // Per-eye stereo offsets relative to the head centre.
    if (g_xr.viewCount == 2) {
        XrVector3f hp = headLoc.pose.position;
        VR_SetEyeOffsets(g_xr.views[0].pose.position.x - hp.x,
                         g_xr.views[0].pose.position.z - hp.z,
                         g_xr.views[1].pose.position.x - hp.x,
                         g_xr.views[1].pose.position.z - hp.z);
    }

    // Right-controller aim pose drives the decoupled weapon pitch.
    XrPosef aim;
    if (xri_get_aim_pose(&g_input, &g_xr, &aim)) {
        float ayaw, apitch;
        quat_to_yawpitch(aim.orientation, &ayaw, &apitch);
        VR_SetAimPose(ayaw, apitch, 1);
    } else {
        VR_SetAimPose(0, 0, 0);
    }
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

static void handle_cmd(struct android_app* app, int32_t cmd) {
    (void)app;
    switch (cmd) {
        case APP_CMD_RESUME: LOGI("resumed"); break;
        case APP_CMD_PAUSE: LOGI("paused"); break;
        case APP_CMD_DESTROY: LOGI("destroy"); break;
        default: break;
    }
}

// Cardboard event dispatch: forward app lifecycle + input to cb_engine.
static void cb_dispatch_cmd(struct android_app* app, int32_t cmd) {
    handle_cmd(app, cmd);
    cb_on_cmd(&g_cb, cmd);
}

static int32_t cb_dispatch_input(struct android_app* app, AInputEvent* ev) {
    (void)app;
    return cb_on_input(&g_cb, ev);
}

// Feed sensor head pose (3DOF: no positional tracking) into the VR layer.
static void update_vr_poses_cb(void) {
    float yaw, pitch;
    cb_head_yawpitch(&g_cb, &yaw, &pitch);
    s_headX = s_headY = s_headZ = 0.0f;
    s_headYawDeg = yaw;
    VR_SetHeadPose(0, 0, 0, yaw, pitch);
    // Per-eye offsets along the head's right axis (quat column 0).
    GlMat4 head = glmat_pose(g_cb.quat[0], g_cb.quat[1], g_cb.quat[2],
                             g_cb.quat[3], 0, 0, 0);
    VR_SetEyeOffsets(-head.m[0] * 0.032f, -head.m[2] * 0.032f,
                     head.m[0] * 0.032f, head.m[2] * 0.032f);
    VR_SetAimPose(0, 0, 0);   // no controllers; aim follows view
}

// Shared doom tick + render-mode selection used by both backends.
// Returns the player mobj when a level is active, NULL otherwise.
typedef struct { mobj_t* mo; int worldMode; } FrameMode;

static FrameMode doom_frame(void) {
    FrameMode fm = {NULL, 0};
    if (g_doomStarted && !g_doomExited) {
        doomgeneric_Tick();
    }

    // Any GS_LEVEL frame (including attract demo playback) renders the
    // true-3D world path; automap/menu overlays fall back to the software
    // quad, and title/menus/intermissions go to the world-locked panel —
    // 2D content is never glued to the head pose.
    fm.mo = (g_doomStarted && gamestate == GS_LEVEL)
            ? players[consoleplayer].mo : NULL;
    int renderInLevel = (fm.mo != NULL);
    fm.worldMode = renderInLevel && !automapactive && !menuactive;
    {
        static int lastMode = -1;
        int mode = (fm.worldMode && glr_world_active(&g_renderer)) ? 3
                 : fm.worldMode ? 2
                 : renderInLevel ? 1 : 0;
        if (mode != lastMode) {
            if (mode == 3)
                LOGI("QuestDOOM: RENDER_MODE: 3D_WORLD");
            else if (mode == 2)
                LOGI("QuestDOOM: RENDER_MODE: 3D_WORLD fallback -> "
                     "SOFTWARE_QUAD, reason: %s",
                     glr_world_fail(&g_renderer));
            else if (mode == 1)
                LOGI("QuestDOOM: RENDER_MODE: SOFTWARE_QUAD "
                     "(automap/menu in-level)");
            else
                LOGI("QuestDOOM: RENDER_MODE: WORLD_PANEL (menu/title)");
            lastMode = mode;
        }
        glr_set_immersive(&g_renderer, renderInLevel);
        glr_set_world_mode(&g_renderer, fm.worldMode);
    }

    if (fm.worldMode && fm.mo) {
        glr_world_frame_camera(&g_renderer,
                               (float)fm.mo->x / 65536.0f,
                               (float)fm.mo->y / 65536.0f);
        glr_world_begin_frame(&g_renderer);
    }
    return fm;
}

// Per-eye world camera: doom camera pos + mo angle, mapped onto head pose.
static void set_world_camera(int eye, mobj_t* mo) {
    VR_SelectEye(eye);  // refresh per-eye vr_viewofs_*
    float camX = (float)(mo->x + vr_viewofs_x) / 65536.0f;
    float camY = (float)(mo->y + vr_viewofs_y) / 65536.0f;
    float camZ = (float)(players[consoleplayer].viewz + vr_viewofs_z)
                 / 65536.0f;
    float moAng = (float)((double)mo->angle * (360.0 / 4294967296.0));
    glr_world_camera(&g_renderer, s_headX, s_headY, s_headZ, s_headYawDeg,
                     camX, camY, camZ, moAng);
}

static void start_doom(const char* iwad) {
    g_doomStarted = true;
    char arg0[] = "questdoom";
    char argIwad[] = "-iwad";
    char* argv[] = {arg0, argIwad, (char*)iwad, NULL};
    doomgeneric_Create(3, argv);
    VR_EnableStereo();  // dual D_Display passes, per-eye buffers
    LOGI("doomgeneric created, stereo enabled");
}

// ---------------------------------------------------------------------------
// Cardboard / generic phone loop (no OpenXR runtime)
// ---------------------------------------------------------------------------

static void cardboard_loop(struct android_app* app, const char* iwad) {
    if (!cb_init(&g_cb, app, push_key, NULL)) {
        LOGE("cardboard init failed (no EGL/GLES3 or no rotation sensor)");
        return;
    }
    app->onAppCmd = cb_dispatch_cmd;
    app->onInputEvent = cb_dispatch_input;

    bool glReady = false;
    while (!app->destroyRequested) {
        cb_pump(&g_cb);
        if (!cb_has_surface(&g_cb) || !g_cb.running) continue;

        cb_make_current(&g_cb);
        if (!glReady) {
            if (!glr_init(&g_renderer)) {
                LOGE("GL renderer init failed");
                return;
            }
            // Native window framebuffer presents bottom-up: verified via the
            // offscreen harness that the panel texture needs a V flip only.
            glr_set_panel_uv_flip(&g_renderer, 0.f, 1.f);
            glReady = true;
        }

        if (!g_doomStarted && iwad) start_doom(iwad);

        update_vr_poses_cb();
        FrameMode fm = doom_frame();

        for (int eye = 0; eye < 2; eye++) {
            if (fm.worldMode && fm.mo) set_world_camera(eye, fm.mo);
            GlrEyeParams p;
            cb_eye_params(&g_cb, eye, &p);
            glr_draw_eye_params(&g_renderer, &p);
        }
        cb_swap(&g_cb);
    }

    glr_shutdown(&g_renderer);
    cb_shutdown(&g_cb);
}

void android_main(struct android_app* app) {
    g_app = app;
    app->onAppCmd = handle_cmd;
    redirect_stdio_to_logcat();

    const char* filesDir = app->activity->internalDataPath;
    if (filesDir) chdir(filesDir);
    LOGI("files dir: %s", filesDir ? filesDir : "(null)");

    LOGI("=== quest-doom v0.2.6 (3D world, Y-flip fix) starting ===");
    extract_assets_wads(app, filesDir ? filesDir : ".");

    char iwadPath[512];
    const char* iwad = find_iwad(filesDir ? filesDir : ".", iwadPath,
                                 sizeof(iwadPath));
    if (!iwad) {
        LOGE("No IWAD found. Place doom1.wad/doom.wad/etc in the app files "
             "dir: %s", filesDir ? filesDir : "?");
    } else {
        LOGI("Using IWAD: %s", iwad);
    }

    if (!xr_init(&g_xr, app)) {
        // No OpenXR runtime: generic Android/Cardboard path (phone sensors
        // + window surface + split-screen stereo).
        cardboard_loop(app, iwad);
        return;
    }
    LOGI("QuestDOOM: BACKEND: OPENXR");
    if (!glr_init(&g_renderer)) {
        LOGE("GL renderer init failed");
        xr_shutdown(&g_xr);
        return;
    }
    xri_init(&g_input, &g_xr);   // non-fatal if it fails
    xrh_init(&g_hands, &g_xr);   // hand tracking; falls back to controllers

    while (!app->destroyRequested && !g_xr.exitRequested) {
        xr_poll_events(&g_xr);

        if (!g_xr.sessionRunning) {
            usleep(10000);
            continue;
        }
        if (!xr_begin_frame(&g_xr)) {
            usleep(2000);
            continue;
        }

        // Start DOOM once the session is running.
        if (!g_doomStarted && iwad) start_doom(iwad);

        // Locate views first so this frame's head/eye poses drive the tick.
        bool viewsOk = xr_locate_views(&g_xr);
        if (viewsOk) update_vr_poses();

        xri_sync(&g_input, &g_xr, push_key, NULL);
        xrh_update(&g_hands, &g_xr, push_key, NULL);
        {
            float jpos[52 * 3];
            int jvis[2];
            xrh_get_joints(&g_hands, jpos, jvis);
            glr_set_joints(&g_renderer, jpos, 52, jvis);
        }

        FrameMode fm = doom_frame();

        if (viewsOk) {
            for (int eye = 0; eye < g_xr.viewCount; eye++) {
                if (fm.worldMode && fm.mo) set_world_camera(eye, fm.mo);
                glr_draw_eye(&g_renderer, &g_xr, eye);
            }
        }

        XrCompositionLayerProjection proj = {
            .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION,
            .space = g_xr.appSpace,
            .viewCount = g_xr.viewCount,
            .views = g_xr.projViews,
        };
        XrCompositionLayerBaseHeader* layers[] =
            {(XrCompositionLayerBaseHeader*)&proj};
        xr_end_frame(&g_xr, layers, 1);
    }

    glr_shutdown(&g_renderer);
    xrh_shutdown(&g_hands);
    xri_shutdown(&g_input);
    xr_shutdown(&g_xr);
}
