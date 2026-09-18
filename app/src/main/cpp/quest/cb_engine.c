// Cardboard/phone backend. See cb_engine.h.
//
// Rendering target is the activity window's default framebuffer, split into
// left/right halves. Head pose comes from TYPE_ROTATION_VECTOR (gyro+accel+
// mag fusion; falls back to GAME_ROTATION_VECTOR where only gyro+accel is
// available) — true 3DOF, no positional tracking on phones.
#include "cb_engine.h"

#ifdef __ANDROID__

#include <android/input.h>
#include <android/keycodes.h>
#include <string.h>

#include "../doomgeneric/doomkeys.h"

#define LOOPER_ID_SENSOR 8

// The rotation vector is reported in the portrait sensor frame; in a
// landscape-mounted viewer the camera needs a roll about the view axis.
// Sign convention chosen for "rotate left edge down" (standard landscape);
// flip to +90 if the device sits rotated the other way.
#define CB_ROLL_FIX_DEG (-90.0f)

#define CB_EYE_HALF_IPD 0.032f   // 32mm half-IPD
#define CB_HEAD_Y       0.0f     // head fixed at origin (no positional tracking)
#define CB_VFOV_DEG     60.0f    // generic per-eye vertical FOV

// --- EGL -------------------------------------------------------------------

static bool cb_egl_init(CbEngine* e) {
    e->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (e->display == EGL_NO_DISPLAY || !eglInitialize(e->display, 0, 0)) {
        LOGE("cardboard: eglInitialize failed");
        return false;
    }
    const EGLint cfgAttrs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_DEPTH_SIZE, 24,
        EGL_NONE,
    };
    EGLint n = 0;
    if (!eglChooseConfig(e->display, cfgAttrs, NULL, 0, &n) || n == 0) {
        LOGE("cardboard: no RGBA8+depth24 GLES3 EGL config");
        return false;
    }
    EGLConfig cfgs[8];
    if (n > 8) n = 8;
    eglChooseConfig(e->display, cfgAttrs, cfgs, n, &n);
    e->config = cfgs[0];

    const EGLint ctxAttrs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    e->context = eglCreateContext(e->display, e->config,
                                  EGL_NO_CONTEXT, ctxAttrs);
    if (e->context == EGL_NO_CONTEXT) {
        LOGE("cardboard: GLES3 context creation failed");
        return false;
    }
    return true;
}

static void cb_create_surface(CbEngine* e) {
    if (e->surface != EGL_NO_SURFACE || !e->app->window) return;
    e->surface = eglCreateWindowSurface(e->display, e->config,
                                      e->app->window, NULL);
    if (e->surface == EGL_NO_SURFACE) {
        LOGE("cardboard: eglCreateWindowSurface failed");
        return;
    }
    eglQuerySurface(e->display, e->surface, EGL_WIDTH, &e->surfW);
    eglQuerySurface(e->display, e->surface, EGL_HEIGHT, &e->surfH);
    eglSwapInterval(e->display, 1);  // vsync-paced frame loop
    LOGI("cardboard: window surface %dx%d", e->surfW, e->surfH);
}

static void cb_destroy_surface(CbEngine* e) {
    if (e->surface == EGL_NO_SURFACE) return;
    eglMakeCurrent(e->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(e->display, e->surface);
    e->surface = EGL_NO_SURFACE;
}

// --- sensors ---------------------------------------------------------------

static void cb_sensor_init(CbEngine* e) {
    e->sm = ASensorManager_getInstance();
    if (!e->sm) { LOGE("cardboard: no sensor manager"); return; }
    e->rotSensor = ASensorManager_getDefaultSensor(e->sm,
                                                   ASENSOR_TYPE_ROTATION_VECTOR);
    if (!e->rotSensor)
        e->rotSensor = ASensorManager_getDefaultSensor(
            e->sm, ASENSOR_TYPE_GAME_ROTATION_VECTOR);
    if (!e->rotSensor) {
        LOGE("cardboard: no rotation vector sensor — head pose stays identity");
        return;
    }
    e->queue = ASensorManager_createEventQueue(e->sm, e->app->looper,
                                               LOOPER_ID_SENSOR, NULL, NULL);
    if (!e->queue) { LOGE("cardboard: sensor event queue failed"); return; }
    ASensorEventQueue_enableSensor(e->queue, e->rotSensor);
    ASensorEventQueue_setEventRate(e->queue, e->rotSensor, 16000);  // ~60Hz
    LOGI("cardboard: tracking via %s", ASensor_getName(e->rotSensor));
}

static void cb_drain_sensors(CbEngine* e) {
    if (!e->queue) return;
    ASensorEvent ev;
    while (ASensorEventQueue_getEvents(e->queue, &ev, 1) > 0) {
        if (ev.type != ASENSOR_TYPE_ROTATION_VECTOR &&
            ev.type != ASENSOR_TYPE_GAME_ROTATION_VECTOR)
            continue;
        float x = ev.data[0], y = ev.data[1], z = ev.data[2];
        float w = ev.data[3];
        if (w == 0.0f) {
            // optional scalar component absent: w = sqrt(1 - x²-y²-z²)
            float t = 1.0f - x * x - y * y - z * z;
            w = t > 0.0f ? sqrtf(t) : 0.0f;
        }
        e->quat[0] = x; e->quat[1] = y; e->quat[2] = z; e->quat[3] = w;
        e->quatValid = true;
    }
}

// --- input -----------------------------------------------------------------

static unsigned char cb_map_key(int32_t keyCode) {
    switch (keyCode) {
        case AKEYCODE_DPAD_UP:                         return KEY_UPARROW;
        case AKEYCODE_DPAD_DOWN:                       return KEY_DOWNARROW;
        case AKEYCODE_DPAD_LEFT:                       return KEY_LEFTARROW;
        case AKEYCODE_DPAD_RIGHT:                      return KEY_RIGHTARROW;
        case AKEYCODE_ENTER:
        case AKEYCODE_DPAD_CENTER:                     return KEY_ENTER;
        case AKEYCODE_BACK:
        case AKEYCODE_BUTTON_B:                        return KEY_ESCAPE;
        case AKEYCODE_BUTTON_A:
        case AKEYCODE_BUTTON_R1:
        case AKEYCODE_BUTTON_R2:
        case AKEYCODE_BUTTON_L1:                       return KEY_FIRE;
        case AKEYCODE_BUTTON_X:
        case AKEYCODE_BUTTON_Y:                        return KEY_USE;
        default:                                       return 0;
    }
}

int32_t cb_on_input(CbEngine* e, AInputEvent* ev) {
    if (!e->keyFn) return 0;
    int32_t type = AInputEvent_getType(ev);
    if (type == AINPUT_EVENT_TYPE_KEY) {
        unsigned char k = cb_map_key(AKeyEvent_getKeyCode(ev));
        if (k) {
            e->keyFn(AKeyEvent_getAction(ev) == AKEY_EVENT_ACTION_DOWN,
                     k, e->keyUser);
            return 1;
        }
    } else if (type == AINPUT_EVENT_TYPE_MOTION) {
        // Minimal touch scheme: right half tap = fire, left half tap = use.
        int32_t act = AMotionEvent_getAction(ev) & AMOTION_EVENT_ACTION_MASK;
        if (act == AMOTION_EVENT_ACTION_DOWN ||
            act == AMOTION_EVENT_ACTION_UP) {
            int pressed = (act == AMOTION_EVENT_ACTION_DOWN);
            float x = AMotionEvent_getX(ev, 0);
            e->keyFn(pressed,
                     (x > e->surfW * 0.5f) ? KEY_FIRE : KEY_USE, e->keyUser);
            return 1;
        }
    }
    return 0;
}

// --- lifecycle -------------------------------------------------------------

void cb_on_cmd(CbEngine* e, int32_t cmd) {
    switch (cmd) {
        case APP_CMD_INIT_WINDOW:   cb_create_surface(e); break;
        case APP_CMD_TERM_WINDOW:   cb_destroy_surface(e); break;
        case APP_CMD_GAINED_FOCUS:
        case APP_CMD_RESUME:        e->running = true; break;
        case APP_CMD_LOST_FOCUS:
        case APP_CMD_PAUSE:         e->running = false; break;
        default: break;
    }
}

bool cb_init(CbEngine* e, struct android_app* app, CbKeyFn keyFn, void* user) {
    memset(e, 0, sizeof(*e));
    e->app = app;
    e->surface = EGL_NO_SURFACE;
    e->keyFn = keyFn;
    e->keyUser = user;
    e->quat[3] = 1.0f;   // identity

    if (!cb_egl_init(e)) return false;
    cb_sensor_init(e);
    if (app->window) cb_create_surface(e);   // window may already exist
    e->running = true;
    LOGI("QuestDOOM: BACKEND: CARDBOARD (EGL window + rotation vector, 3DOF)");
    return true;
}

void cb_shutdown(CbEngine* e) {
    cb_destroy_surface(e);
    if (e->queue && e->rotSensor)
        ASensorEventQueue_disableSensor(e->queue, e->rotSensor);
    if (e->queue && e->sm)
        ASensorManager_destroyEventQueue(e->sm, e->queue);
    if (e->display != EGL_NO_DISPLAY) {
        if (e->context != EGL_NO_CONTEXT)
            eglDestroyContext(e->display, e->context);
        eglTerminate(e->display);
    }
}

void cb_pump(CbEngine* e) {
    int fd, events;
    struct android_poll_source* src;
    // 4ms budget: keeps ~250fps polling while staying vsync-paced overall.
    while (ALooper_pollOnce(4, &fd, &events, (void**)&src) >= 0) {
        if (src) src->process(e->app, src);
        if (fd == LOOPER_ID_SENSOR) cb_drain_sensors(e);
        if (e->app->destroyRequested) return;
    }
}

bool cb_has_surface(const CbEngine* e) {
    return e->surface != EGL_NO_SURFACE;
}

void cb_make_current(CbEngine* e) {
    eglMakeCurrent(e->display, e->surface, e->surface, e->context);
}

void cb_swap(CbEngine* e) {
    eglSwapBuffers(e->display, e->surface);
    e->frameCount++;
    if ((e->frameCount % 240) == 1)
        LOGI("cardboard: frame #%lu sensors=%s", (unsigned long)e->frameCount,
             e->quatValid ? "ok" : "no-data");
}

void cb_eye_params(CbEngine* e, int eye, GlrEyeParams* out) {
    memset(out, 0, sizeof(*out));
    int hw = e->surfW / 2;
    out->vpX = (eye == 0) ? 0 : hw;
    out->vpY = 0;
    out->vpW = hw;
    out->vpH = e->surfH;
    out->targetTex = 0;
    out->srcEye = eye;

    // symmetric per-eye projection: vfov fixed, hfov from half-aspect
    float aspect = (e->surfH > 0) ? (float)hw / (float)e->surfH : 1.0f;
    float tanV = tanf(CB_VFOV_DEG * 0.5f * (float)M_PI / 180.0f);
    float tanH = aspect * tanV;
    GlMat4 proj = glmat_projection(-tanH, tanH, tanV, -tanV, 0.05f, 400.0f);
    memcpy(out->proj, proj.m, sizeof(proj.m));

    // view = roll-fix * T(-eyeOffset) * inv(headPose)
    GlMat4 head = glmat_pose(e->quat[0], e->quat[1], e->quat[2], e->quat[3],
                             0.0f, CB_HEAD_Y, 0.0f);
    float ex = (eye == 0) ? -CB_EYE_HALF_IPD : CB_EYE_HALF_IPD;
    GlMat4 view = glmat_mul(glmat_roll_z(CB_ROLL_FIX_DEG),
                            glmat_mul(glmat_translate(-ex, 0.0f, 0.0f),
                                      glmat_invert_rigid(head)));
    memcpy(out->view, view.m, sizeof(view.m));
}

void cb_head_yawpitch(const CbEngine* e, float* yawDeg, float* pitchDeg) {
    glmat_quat_to_yawpitch(e->quat[0], e->quat[1], e->quat[2], e->quat[3],
                           yawDeg, pitchDeg);
}

#endif // __ANDROID__
