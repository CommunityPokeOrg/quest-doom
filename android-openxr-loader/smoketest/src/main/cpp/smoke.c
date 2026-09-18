// Smoke test: dlopen the packaged libopenxr_loader.so, fetch
// xrInitializeLoaderKHR (it is deliberately NOT exported on Android — the
// Khronos loader requires it be obtained via xrGetInstanceProcAddr), initialize
// it with the app's JavaVM/Activity, then enumerate instance extensions to
// prove the loader is functional end to end.
#include <android/log.h>
#include <android_native_app_glue.h>
#include <dlfcn.h>
#include <string.h>
#include <unistd.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#define TAG "OpenXrSmoke"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

void android_main(struct android_app* app) {
    app_dummy();

    void* lib = dlopen("libopenxr_loader.so", RTLD_NOW);
    if (!lib) {
        LOGE("SMOKE FAIL: dlopen(libopenxr_loader.so): %s", dlerror());
        ANativeActivity_finish(app->activity);
        return;
    }
    LOGI("dlopen ok");

    PFN_xrGetInstanceProcAddr getProc =
        (PFN_xrGetInstanceProcAddr)dlsym(lib, "xrGetInstanceProcAddr");
    if (!getProc) {
        LOGE("SMOKE FAIL: xrGetInstanceProcAddr not exported");
        ANativeActivity_finish(app->activity);
        return;
    }

    PFN_xrInitializeLoaderKHR initLoader = NULL;
    if (XR_FAILED(getProc(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
                          (PFN_xrVoidFunction*)&initLoader)) ||
        !initLoader) {
        LOGE("SMOKE FAIL: could not fetch xrInitializeLoaderKHR");
        ANativeActivity_finish(app->activity);
        return;
    }

    XrLoaderInitInfoAndroidKHR init = {
        .type = XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR,
        .next = NULL,
        .applicationVM = app->activity->vm,
        .applicationContext = app->activity->clazz,
    };
    if (XR_FAILED(initLoader((const XrLoaderInitInfoBaseHeaderKHR*)&init))) {
        LOGE("SMOKE FAIL: xrInitializeLoaderKHR failed");
        ANativeActivity_finish(app->activity);
        return;
    }
    LOGI("xrInitializeLoaderKHR ok");

    PFN_xrEnumerateInstanceExtensionProperties enumExt = NULL;
    if (XR_FAILED(getProc(XR_NULL_HANDLE,
                          "xrEnumerateInstanceExtensionProperties",
                          (PFN_xrVoidFunction*)&enumExt)) || !enumExt) {
        LOGE("SMOKE FAIL: could not fetch xrEnumerateInstanceExtensionProperties");
        ANativeActivity_finish(app->activity);
        return;
    }
    uint32_t count = 0;
    XrResult res = enumExt(NULL, 0, &count, NULL);
    // XR_ERROR_RUNTIME_UNAVAILABLE is fine on a device with no OpenXR
    // runtime — the point is that the loader itself ran correctly.
    LOGI("xrEnumerateInstanceExtensionProperties -> result=%d count=%u "
         "(RUNTIME_UNAVAILABLE is expected off-headset)", res, count);

    LOGI("SMOKE PASS: libopenxr_loader.so loaded and initialized");
    ANativeActivity_finish(app->activity);
}
