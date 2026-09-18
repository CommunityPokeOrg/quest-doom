// OpenXR engine glue for Quest DOOM: instance/session lifecycle, EGL/GLES3
// context, per-eye swapchains, frame loop, and event pumping.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES3/gl3ext.h>

#include <android_native_app_glue.h>
#include <android/log.h>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#define LOG_TAG "QuestDOOM"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define MAX_EYE_COUNT 2

typedef struct {
    XrSwapchain swapchain;
    uint32_t width;
    uint32_t height;
    uint32_t imageCount;
    GLuint* images;  // GL texture names, one per swapchain image
} XrEyeSwapchain;

typedef struct {
    struct android_app* app;

    XrInstance instance;
    XrSystemId systemId;
    XrSession session;
    XrSessionState sessionState;
    bool sessionRunning;
    bool exitRequested;

    XrSpace localSpace;
    XrSpace viewSpace;  // head-locked space

    EGLDisplay eglDisplay;
    EGLConfig eglConfig;
    EGLContext eglContext;
    EGLSurface eglSurface;  // 1x1 pbuffer, kept current

    int viewCount;
    XrViewConfigurationView viewConfig[MAX_EYE_COUNT];
    XrEyeSwapchain eyeSwapchains[MAX_EYE_COUNT];
    XrView views[MAX_EYE_COUNT];
    XrCompositionLayerProjectionView projViews[MAX_EYE_COUNT];

    XrFrameState frameState;
    bool frameBegun;

    bool handTrackingExt;  // XR_EXT_hand_tracking advertised by runtime
} XrEngine;

// Returns false if OpenXR runtime is unavailable.
bool xr_init(XrEngine* e, struct android_app* app);
void xr_shutdown(XrEngine* e);

// Pump android_app_glue + OpenXR events; transitions session state.
void xr_poll_events(XrEngine* e);

// Wait for/begin/end a frame. xr_begin_frame returns false when the frame
// should be skipped (session not running or shouldRender=false handling).
bool xr_begin_frame(XrEngine* e);
void xr_end_frame(XrEngine* e, XrCompositionLayerBaseHeader** layers, int layerCount);

// Locate eye views for the current frame in local space. Fills e->views.
bool xr_locate_views(XrEngine* e);

// Acquire the next swapchain image for eye `i`, return GL texture name.
GLuint xr_acquire_eye_image(XrEngine* e, int i, uint32_t* index);
void xr_release_eye_image(XrEngine* e, int i);

const char* xr_result_string(XrResult r);
