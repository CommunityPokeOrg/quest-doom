// doomgeneric platform layer for Meta Quest (OpenXR + Android NDK).
//
// android_main -> OpenXR session -> doomgeneric_Create -> per-frame:
//   xrSyncActions -> doomgeneric_Tick -> upload framebuffer -> stereo panel render
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
#include "xr_engine.h"
#include "xr_input.h"
#include "gl_renderer.h"

#define KEYQUEUE_SIZE 64

static XrEngine g_xr;
static XrInput g_input;
static GlRenderer g_renderer;
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
    // Called once per rendered game frame: upload framebuffer to the panel
    // texture. EGL context is current on this thread.
    glr_upload_frame(&g_renderer, (const uint32_t*)DG_ScreenBuffer,
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

void android_main(struct android_app* app) {
    g_app = app;
    app->onAppCmd = handle_cmd;
    redirect_stdio_to_logcat();

    const char* filesDir = app->activity->internalDataPath;
    if (filesDir) chdir(filesDir);
    LOGI("files dir: %s", filesDir ? filesDir : "(null)");

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
        LOGE("OpenXR initialization failed");
        return;
    }
    if (!glr_init(&g_renderer)) {
        LOGE("GL renderer init failed");
        xr_shutdown(&g_xr);
        return;
    }
    xri_init(&g_input, &g_xr);  // non-fatal if it fails

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
        if (!g_doomStarted && iwad) {
            g_doomStarted = true;
            char arg0[] = "questdoom";
            char argIwad[] = "-iwad";
            char* argv[] = {arg0, argIwad, (char*)iwad, NULL};
            doomgeneric_Create(3, argv);
            LOGI("doomgeneric created");
        }

        xri_sync(&g_input, &g_xr, push_key, NULL);

        if (g_doomStarted && !g_doomExited) {
            doomgeneric_Tick();
        }

        if (xr_locate_views(&g_xr)) {
            for (int eye = 0; eye < g_xr.viewCount; eye++)
                glr_draw_eye(&g_renderer, &g_xr, eye);
        }

        XrCompositionLayerProjection proj = {
            .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION,
            .space = g_xr.localSpace,
            .viewCount = g_xr.viewCount,
            .views = g_xr.projViews,
        };
        XrCompositionLayerBaseHeader* layers[] =
            {(XrCompositionLayerBaseHeader*)&proj};
        xr_end_frame(&g_xr, layers, 1);
    }

    glr_shutdown(&g_renderer);
    xri_shutdown(&g_input);
    xr_shutdown(&g_xr);
}
