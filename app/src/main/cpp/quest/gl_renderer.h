// GLES3 renderer: uploads the doomgeneric framebuffer to a texture and draws it
// on a curved world-locked panel rendered separately into each eye's swapchain
// image — true stereo presentation with full 6DOF head tracking.
#pragma once

#include "xr_engine.h"

typedef struct {
    GLuint program;
    GLuint doomTexture;   // DOOMGENERIC_RESX x DOOMGENERIC_RESY framebuffer
    GLuint panelVao;
    GLuint panelVbo;
    GLuint panelIbo;
    GLuint fbo;
    int panelIndexCount;
    bool textureDirty;
} GlRenderer;

bool glr_init(GlRenderer* r);

// Re-upload DG_ScreenBuffer to the panel texture.
void glr_upload_frame(GlRenderer* r, const uint32_t* pixels, int w, int h);

// Render the panel into eye `eye`'s swapchain for view `view`.
void glr_draw_eye(GlRenderer* r, XrEngine* e, int eye);

void glr_shutdown(GlRenderer* r);
