#include "gl_xr.h"

#include <math.h>
#include <string.h>

void glr_draw_eye(GlRenderer* r, XrEngine* e, int eye) {
    uint32_t index = 0;
    GLuint tex = xr_acquire_eye_image(e, eye, &index);
    if (!tex) return;

    XrEyeSwapchain* sc = &e->eyeSwapchains[eye];
    XrView* view = &e->views[eye];

    GlMat4 viewMat = glmat_invert_rigid(
        glmat_pose(view->pose.orientation.x, view->pose.orientation.y,
                   view->pose.orientation.z, view->pose.orientation.w,
                   view->pose.position.x, view->pose.position.y,
                   view->pose.position.z));
    GlMat4 projMat = glmat_projection(tanf(view->fov.angleLeft),
                                      tanf(view->fov.angleRight),
                                      tanf(view->fov.angleUp),
                                      tanf(view->fov.angleDown),
                                      0.01f, 600.0f);
    // GL writes the swapchain image bottom-up; the XR compositor reads it
    // top-down. Negating projection Y renders the scene upside-down in GL
    // terms so it presents upright (without this everything shows flipped).
    projMat.m[5] = -projMat.m[5];

    GlrEyeParams p;
    memcpy(p.view, viewMat.m, sizeof(p.view));
    memcpy(p.proj, projMat.m, sizeof(p.proj));
    p.vpX = p.vpY = 0;
    p.vpW = (int)sc->width;
    p.vpH = (int)sc->height;
    p.targetTex = tex;
    p.srcEye = eye;

    // The compositor V-flips the swapchain image once: panel UVs need a
    // different correction than a plain window framebuffer.
    glr_set_panel_uv_flip(r, 1.f, 0.f);

    glr_draw_eye_params(r, &p);

    e->projViews[eye].pose = view->pose;
    e->projViews[eye].fov = view->fov;

    xr_release_eye_image(e, eye);
}
