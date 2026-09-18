// OpenXR-specific render wrapper: acquires an eye swapchain image, builds
// backend-neutral GlrEyeParams from the located XrView, draws via
// glr_draw_eye_params, then releases the image.
#pragma once

#include "gl_renderer.h"
#include "xr_engine.h"

// Render into the acquired swapchain image for eye `e` (0=left,1=right).
void glr_draw_eye(GlRenderer* r, XrEngine* e, int eye);
