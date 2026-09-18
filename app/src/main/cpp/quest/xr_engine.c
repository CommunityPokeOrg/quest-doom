#include "xr_engine.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define XR_CHECK(expr)                                                  \
    do {                                                                \
        XrResult _r = (expr);                                           \
        if (XR_FAILED(_r)) {                                            \
            LOGE("%s failed: %s", #expr, xr_result_string(_r));         \
            return false;                                               \
        }                                                               \
    } while (0)

// ---------------------------------------------------------------------------
// Extension function pointers
// ---------------------------------------------------------------------------

static PFN_xrGetOpenGLESGraphicsRequirementsKHR pfnGetGraphicsReqs = NULL;

const char* xr_result_string(XrResult r) {
    switch (r) {
        case XR_SUCCESS: return "XR_SUCCESS";
        case XR_TIMEOUT_EXPIRED: return "XR_TIMEOUT_EXPIRED";
        case XR_ERROR_SESSION_LOST: return "XR_ERROR_SESSION_LOST";
        case XR_ERROR_INSTANCE_LOST: return "XR_ERROR_INSTANCE_LOST";
        case XR_ERROR_RUNTIME_FAILURE: return "XR_ERROR_RUNTIME_FAILURE";
        case XR_ERROR_FORM_FACTOR_UNAVAILABLE: return "XR_ERROR_FORM_FACTOR_UNAVAILABLE";
        case XR_ERROR_FUNCTION_UNSUPPORTED: return "XR_ERROR_FUNCTION_UNSUPPORTED";
        case XR_ERROR_VALIDATION_FAILURE: return "XR_ERROR_VALIDATION_FAILURE";
        default: return "XR_ERROR_UNKNOWN";
    }
}

// ---------------------------------------------------------------------------
// EGL
// ---------------------------------------------------------------------------

static bool egl_init(XrEngine* e) {
    e->eglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (e->eglDisplay == EGL_NO_DISPLAY) {
        LOGE("eglGetDisplay failed");
        return false;
    }
    if (!eglInitialize(e->eglDisplay, NULL, NULL)) {
        LOGE("eglInitialize failed: 0x%x", eglGetError());
        return false;
    }

    const EGLint configAttribs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 16,
        EGL_NONE,
    };
    EGLint numConfigs = 0;
    if (!eglChooseConfig(e->eglDisplay, configAttribs, &e->eglConfig, 1, &numConfigs) ||
        numConfigs < 1) {
        LOGE("eglChooseConfig failed: 0x%x", eglGetError());
        return false;
    }

    const EGLint contextAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    e->eglContext =
        eglCreateContext(e->eglDisplay, e->eglConfig, EGL_NO_CONTEXT, contextAttribs);
    if (e->eglContext == EGL_NO_CONTEXT) {
        LOGE("eglCreateContext failed: 0x%x", eglGetError());
        return false;
    }

    const EGLint pbufferAttribs[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    e->eglSurface =
        eglCreatePbufferSurface(e->eglDisplay, e->eglConfig, pbufferAttribs);
    if (e->eglSurface == EGL_NO_SURFACE) {
        LOGE("eglCreatePbufferSurface failed: 0x%x", eglGetError());
        return false;
    }

    if (!eglMakeCurrent(e->eglDisplay, e->eglSurface, e->eglSurface, e->eglContext)) {
        LOGE("eglMakeCurrent failed: 0x%x", eglGetError());
        return false;
    }

    LOGI("EGL initialized: GLES %s", glGetString(GL_VERSION));
    return true;
}

// ---------------------------------------------------------------------------
// OpenXR instance/session
// ---------------------------------------------------------------------------

// The Khronos Android loader requires xrInitializeLoaderKHR (with the app
// JavaVM + context) before any other xr* call. The symbol is intentionally
// NOT exported by libopenxr_loader.so — it must be fetched via
// xrGetInstanceProcAddr with XR_NULL_HANDLE.
static bool xr_loader_init(XrEngine* e) {
    PFN_xrInitializeLoaderKHR pfnInit = NULL;
    XrResult r = xrGetInstanceProcAddr(XR_NULL_HANDLE,
                                       "xrInitializeLoaderKHR",
                                       (PFN_xrVoidFunction*)&pfnInit);
    if (XR_FAILED(r) || pfnInit == NULL) {
        LOGE("xrInitializeLoaderKHR unavailable: %s", xr_result_string(r));
        return false;
    }
    XrLoaderInitInfoAndroidKHR init = {
        .type = XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR,
        .applicationVM = e->app->activity->vm,
        .applicationContext = e->app->activity->clazz,
    };
    r = pfnInit((XrLoaderInitInfoBaseHeaderKHR*)&init);
    if (XR_FAILED(r)) {
        LOGE("xrInitializeLoaderKHR failed: %s", xr_result_string(r));
        return false;
    }
    LOGI("OpenXR loader initialized");
    return true;
}

static bool xr_create_instance(XrEngine* e) {
    XrInstanceCreateInfoAndroidKHR androidCi = {
        .type = XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR,
        .applicationVM = e->app->activity->vm,
        .applicationActivity = e->app->activity->clazz,
    };

    // Check runtime extension support
    uint32_t extCount = 0;
    xrEnumerateInstanceExtensionProperties(NULL, 0, &extCount, NULL);
    XrExtensionProperties* exts =
        malloc(extCount * sizeof(XrExtensionProperties));
    for (uint32_t i = 0; i < extCount; i++)
        exts[i].type = XR_TYPE_EXTENSION_PROPERTIES;
    xrEnumerateInstanceExtensionProperties(NULL, extCount, &extCount, exts);
    for (uint32_t i = 0; i < extCount; i++) {
        if (strcmp(exts[i].extensionName,
                   XR_EXT_HAND_TRACKING_EXTENSION_NAME) == 0)
            e->handTrackingExt = true;
    }
    free(exts);
    LOGI("hand tracking extension: %s",
         e->handTrackingExt ? "supported" : "not present");

    const char* extensions[] = {
        XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
        XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
        // enabled only when advertised — enabling an unsupported extension
        // makes xrCreateInstance fail outright
        e->handTrackingExt ? XR_EXT_HAND_TRACKING_EXTENSION_NAME : NULL,
    };
    const uint32_t extEnabled = e->handTrackingExt ? 3 : 2;

    XrInstanceCreateInfo ci = {
        .type = XR_TYPE_INSTANCE_CREATE_INFO,
        .next = &androidCi,
        .enabledExtensionCount = extEnabled,
        .enabledExtensionNames = extensions,
        .applicationInfo = {
            .applicationName = "Quest DOOM",
            .applicationVersion = 1,
            .engineName = "doomgeneric",
            .engineVersion = 1,
            .apiVersion = XR_CURRENT_API_VERSION,
        },
    };

    XR_CHECK(xrCreateInstance(&ci, &e->instance));

    XrInstanceProperties props = {.type = XR_TYPE_INSTANCE_PROPERTIES};
    xrGetInstanceProperties(e->instance, &props);
    LOGI("OpenXR runtime: %s %u.%u.%u", props.runtimeName,
         XR_VERSION_MAJOR(props.runtimeVersion),
         XR_VERSION_MINOR(props.runtimeVersion),
         XR_VERSION_PATCH(props.runtimeVersion));

    XR_CHECK(xrGetInstanceProcAddr(e->instance,
                                   "xrGetOpenGLESGraphicsRequirementsKHR",
                                   (PFN_xrVoidFunction*)&pfnGetGraphicsReqs));

    XrSystemGetInfo sysInfo = {
        .type = XR_TYPE_SYSTEM_GET_INFO,
        .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY,
    };
    XR_CHECK(xrGetSystem(e->instance, &sysInfo, &e->systemId));

    XrSystemProperties sysProps = {.type = XR_TYPE_SYSTEM_PROPERTIES};
    xrGetSystemProperties(e->instance, e->systemId, &sysProps);
    LOGI("OpenXR system: %s (%ux%u max)", sysProps.systemName,
         sysProps.graphicsProperties.maxSwapchainImageWidth,
         sysProps.graphicsProperties.maxSwapchainImageHeight);
    return true;
}

static bool xr_create_session(XrEngine* e) {
    XrGraphicsRequirementsOpenGLESKHR reqs = {
        .type = XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
    XR_CHECK(pfnGetGraphicsReqs(e->instance, e->systemId, &reqs));

    XrGraphicsBindingOpenGLESAndroidKHR binding = {
        .type = XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR,
        .display = e->eglDisplay,
        .config = e->eglConfig,
        .context = e->eglContext,
    };

    XrSessionCreateInfo sessionCi = {
        .type = XR_TYPE_SESSION_CREATE_INFO,
        .next = &binding,
        .systemId = e->systemId,
    };
    XR_CHECK(xrCreateSession(e->instance, &sessionCi, &e->session));

    XrReferenceSpaceCreateInfo spaceCi = {
        .type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO,
        .referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL,
        .poseInReferenceSpace = {{0, 0, 0, 1}, {0, 0, 0}},
    };
    XR_CHECK(xrCreateReferenceSpace(e->session, &spaceCi, &e->localSpace));

    spaceCi.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    XR_CHECK(xrCreateReferenceSpace(e->session, &spaceCi, &e->viewSpace));

    // STAGE gives a floor-stable, room-calibrated origin; fall back to LOCAL
    // on runtimes without guardian/stage data.
    spaceCi.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    if (XR_SUCCEEDED(xrCreateReferenceSpace(e->session, &spaceCi,
                                          &e->appSpace))) {
        e->appSpaceIsStage = true;
        LOGI("app space: STAGE");
    } else {
        e->appSpace = e->localSpace;
        e->appSpaceIsStage = false;
        LOGI("app space: LOCAL (stage unavailable)");
    }
    return true;
}

static bool xr_create_swapchains(XrEngine* e) {
    uint32_t count = 0;
    XR_CHECK(xrEnumerateViewConfigurationViews(e->instance, e->systemId,
                                             XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                             0, &count, NULL));
    if (count == 0 || count > MAX_EYE_COUNT) {
        LOGE("unexpected view count: %u", count);
        return false;
    }
    e->viewCount = (int)count;

    XrViewConfigurationView configs[MAX_EYE_COUNT];
    for (uint32_t i = 0; i < count; i++)
        configs[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
    XR_CHECK(xrEnumerateViewConfigurationViews(e->instance, e->systemId,
                                             XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                             count, &count, configs));

    uint32_t formatCount = 0;
    xrEnumerateSwapchainFormats(e->session, 0, &formatCount, NULL);
    int64_t* formats = malloc(formatCount * sizeof(int64_t));
    xrEnumerateSwapchainFormats(e->session, formatCount, &formatCount, formats);
    int64_t chosenFormat = -1;
    const int64_t wanted[] = {GL_RGBA8, GL_SRGB8_ALPHA8, GL_RGBA16F};
    for (size_t w = 0; w < sizeof(wanted) / sizeof(wanted[0]) && chosenFormat < 0; w++)
        for (uint32_t f = 0; f < formatCount; f++)
            if (formats[f] == wanted[w]) chosenFormat = formats[f];
    if (chosenFormat < 0) chosenFormat = formats[0];
    free(formats);
    LOGI("Swapchain format: %lld", (long long)chosenFormat);

    for (int i = 0; i < e->viewCount; i++) {
        XrEyeSwapchain* sc = &e->eyeSwapchains[i];
        e->viewConfig[i] = configs[i];
        sc->width = configs[i].recommendedImageRectWidth;
        sc->height = configs[i].recommendedImageRectHeight;

        XrSwapchainCreateInfo sci = {
            .type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
            .usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                          XR_SWAPCHAIN_USAGE_SAMPLED_BIT,
            .format = chosenFormat,
            .sampleCount = 1,
            .width = sc->width,
            .height = sc->height,
            .faceCount = 1,
            .arraySize = 1,
            .mipCount = 1,
        };
        XR_CHECK(xrCreateSwapchain(e->session, &sci, &sc->swapchain));

        xrEnumerateSwapchainImages(sc->swapchain, 0, &sc->imageCount, NULL);
        XrSwapchainImageOpenGLESKHR* imgs =
            malloc(sc->imageCount * sizeof(XrSwapchainImageOpenGLESKHR));
        for (uint32_t j = 0; j < sc->imageCount; j++)
            imgs[j].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
        XR_CHECK(xrEnumerateSwapchainImages(
            sc->swapchain, sc->imageCount, &sc->imageCount,
            (XrSwapchainImageBaseHeader*)imgs));

        sc->images = malloc(sc->imageCount * sizeof(GLuint));
        for (uint32_t j = 0; j < sc->imageCount; j++)
            sc->images[j] = imgs[j].image;
        free(imgs);

        e->projViews[i].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
        e->projViews[i].subImage.swapchain = sc->swapchain;
        e->projViews[i].subImage.imageRect.offset = (XrOffset2Di){0, 0};
        e->projViews[i].subImage.imageRect.extent =
            (XrExtent2Di){(int32_t)sc->width, (int32_t)sc->height};
        e->projViews[i].subImage.imageArrayIndex = 0;
    }
    return true;
}

bool xr_init(XrEngine* e, struct android_app* app) {
    memset(e, 0, sizeof(*e));
    e->app = app;
    e->sessionState = XR_SESSION_STATE_UNKNOWN;
    if (!xr_loader_init(e)) return false;
    if (!egl_init(e)) return false;
    if (!xr_create_instance(e)) return false;
    if (!xr_create_session(e)) return false;
    if (!xr_create_swapchains(e)) return false;
    return true;
}

void xr_shutdown(XrEngine* e) {
    for (int i = 0; i < e->viewCount; i++) {
        if (e->eyeSwapchains[i].swapchain != XR_NULL_HANDLE)
            xrDestroySwapchain(e->eyeSwapchains[i].swapchain);
        free(e->eyeSwapchains[i].images);
    }
    if (e->localSpace != XR_NULL_HANDLE) xrDestroySpace(e->localSpace);
    if (e->viewSpace != XR_NULL_HANDLE) xrDestroySpace(e->viewSpace);
    if (e->appSpace != XR_NULL_HANDLE && e->appSpace != e->localSpace)
        xrDestroySpace(e->appSpace);
    if (e->session != XR_NULL_HANDLE) xrDestroySession(e->session);
    if (e->instance != XR_NULL_HANDLE) xrDestroyInstance(e->instance);
    if (e->eglDisplay != EGL_NO_DISPLAY) {
        eglMakeCurrent(e->eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (e->eglSurface != EGL_NO_SURFACE)
            eglDestroySurface(e->eglDisplay, e->eglSurface);
        if (e->eglContext != EGL_NO_CONTEXT)
            eglDestroyContext(e->eglDisplay, e->eglContext);
        eglTerminate(e->eglDisplay);
    }
}

// ---------------------------------------------------------------------------
// Event pumping
// ---------------------------------------------------------------------------

static void handle_xr_event(XrEngine* e, XrEventDataBuffer* ev) {
    switch (ev->type) {
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
            XrEventDataSessionStateChanged* se =
                (XrEventDataSessionStateChanged*)ev;
            e->sessionState = se->state;
            LOGI("Session state -> %d", se->state);
            switch (se->state) {
                case XR_SESSION_STATE_READY: {
                    XrSessionBeginInfo bi = {
                        .type = XR_TYPE_SESSION_BEGIN_INFO,
                        .primaryViewConfigurationType =
                            XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                    };
                    XrResult r = xrBeginSession(e->session, &bi);
                    if (XR_FAILED(r))
                        LOGE("xrBeginSession: %s", xr_result_string(r));
                    else
                        e->sessionRunning = true;
                    break;
                }
                case XR_SESSION_STATE_STOPPING:
                    e->sessionRunning = false;
                    xrEndSession(e->session);
                    break;
                case XR_SESSION_STATE_EXITING:
                case XR_SESSION_STATE_LOSS_PENDING:
                    e->exitRequested = true;
                    break;
                default:
                    break;
            }
            break;
        }
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
            e->exitRequested = true;
            break;
        default:
            break;
    }
}

void xr_poll_events(XrEngine* e) {
    // Android app-glue events
    int events;
    struct android_poll_source* source;
    while (ALooper_pollOnce(0, NULL, &events, (void**)&source) >= 0) {
        if (source) source->process(e->app, source);
    }

    // OpenXR events
    XrEventDataBuffer ev = {.type = XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(e->instance, &ev) == XR_SUCCESS) {
        handle_xr_event(e, &ev);
        ev = (XrEventDataBuffer){.type = XR_TYPE_EVENT_DATA_BUFFER};
    }
}

// ---------------------------------------------------------------------------
// Frame loop
// ---------------------------------------------------------------------------

bool xr_begin_frame(XrEngine* e) {
    if (!e->sessionRunning) return false;

    XrFrameWaitInfo waitInfo = {.type = XR_TYPE_FRAME_WAIT_INFO};
    e->frameState.type = XR_TYPE_FRAME_STATE;
    if (XR_FAILED(xrWaitFrame(e->session, &waitInfo, &e->frameState)))
        return false;

    XrFrameBeginInfo beginInfo = {.type = XR_TYPE_FRAME_BEGIN_INFO};
    if (XR_FAILED(xrBeginFrame(e->session, &beginInfo)))
        return false;

    e->frameBegun = true;
    return true;
}

void xr_end_frame(XrEngine* e, XrCompositionLayerBaseHeader** layers,
                  int layerCount) {
    if (!e->frameBegun) return;
    e->frameBegun = false;

    XrFrameEndInfo endInfo = {
        .type = XR_TYPE_FRAME_END_INFO,
        .displayTime = e->frameState.predictedDisplayTime,
        .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
        .layerCount = e->frameState.shouldRender ? (uint32_t)layerCount : 0,
        .layers = e->frameState.shouldRender
                      ? (const XrCompositionLayerBaseHeader* const*)layers
                      : NULL,
    };
    xrEndFrame(e->session, &endInfo);
}

bool xr_locate_views(XrEngine* e) {
    XrViewLocateInfo locateInfo = {
        .type = XR_TYPE_VIEW_LOCATE_INFO,
        .viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
        .displayTime = e->frameState.predictedDisplayTime,
        .space = e->appSpace,
    };
    XrViewState viewState = {.type = XR_TYPE_VIEW_STATE};
    uint32_t count = 0;
    for (int i = 0; i < e->viewCount; i++) e->views[i].type = XR_TYPE_VIEW;
    if (XR_FAILED(xrLocateViews(e->session, &locateInfo, &viewState,
                              e->viewCount, &count, e->views)))
        return false;
    return (viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) &&
           (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT);
}

GLuint xr_acquire_eye_image(XrEngine* e, int i, uint32_t* index) {
    XrEyeSwapchain* sc = &e->eyeSwapchains[i];
    XrSwapchainImageAcquireInfo ai = {.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    uint32_t idx = 0;
    if (XR_FAILED(xrAcquireSwapchainImage(sc->swapchain, &ai, &idx)))
        return 0;
    XrSwapchainImageWaitInfo wi = {
        .type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO,
        .timeout = XR_INFINITE_DURATION,
    };
    xrWaitSwapchainImage(sc->swapchain, &wi);
    *index = idx;
    return sc->images[idx];
}

void xr_release_eye_image(XrEngine* e, int i) {
    XrSwapchainImageReleaseInfo ri = {.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(e->eyeSwapchains[i].swapchain, &ri);
}
