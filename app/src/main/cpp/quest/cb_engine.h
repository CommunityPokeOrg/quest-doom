// Cardboard/phone backend: plain EGL window surface, SensorManager
// TYPE_ROTATION_VECTOR 3DOF head pose, side-by-side split viewports and
// touch/gamepad input. No OpenXR anywhere in this path.
#pragma once

#ifdef __ANDROID__

#include <stdbool.h>
#include <stdint.h>

#include <EGL/egl.h>
#include <android/input.h>
#include <android_native_app_glue.h>
#include <android/sensor.h>

#include "gl_common.h"
#include "gl_renderer.h"

// Key event callback (same signature as the doom key queue push fn).
typedef void (*CbKeyFn)(int pressed, unsigned char key, void* user);

typedef struct {
    struct android_app* app;

    EGLDisplay display;
    EGLContext context;
    EGLConfig  config;
    EGLSurface surface;
    int surfW, surfH;

    ASensorManager*    sm;
    ASensorEventQueue* queue;
    const ASensor*     rotSensor;
    float quat[4];     // device->world rotation (rotation vector)
    bool  quatValid;
    bool  running;     // resumed/focused
    uint64_t frameCount;

    CbKeyFn keyFn;
    void*   keyUser;
} CbEngine;

// EGL display+context (no surface yet — waits for APP_CMD_INIT_WINDOW),
// sensor queue attach on the app looper. Returns false if EGL/GLES3 or a
// rotation sensor are unavailable.
bool cb_init(CbEngine* e, struct android_app* app, CbKeyFn keyFn, void* user);
void cb_shutdown(CbEngine* e);

// App cmd handler to install as app->onAppCmd while in Cardboard mode.
// Manages the EGL window surface and run state. Call your own lifecycle
// logging first, then this.
void cb_on_cmd(CbEngine* e, int32_t cmd);
// Input handler to install as app->onInputEvent (touch + key/gamepad->doom).
int32_t cb_on_input(CbEngine* e, AInputEvent* ev);

// Pump app events, input and the sensor queue once (non-blocking-ish:
// ~4ms timeout so frames keep flowing without sensor updates).
void cb_pump(CbEngine* e);

bool cb_has_surface(const CbEngine* e);
void cb_make_current(CbEngine* e);
void cb_swap(CbEngine* e);

// Fill backend-neutral eye params: side-by-side viewport, symmetric per-eye
// projection, sensor-quat view matrix with landscape roll compensation and
// ±32mm IPD offsets along the head right axis.
void cb_eye_params(CbEngine* e, int eye, GlrEyeParams* out);

// Latest head pose as yaw/pitch (for the doom VR layer) — NULL-safe.
void cb_head_yawpitch(const CbEngine* e, float* yawDeg, float* pitchDeg);

#endif // __ANDROID__
