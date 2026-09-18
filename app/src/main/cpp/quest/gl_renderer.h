// GLES3 renderer for the immersive VR path.
//
// The doomgeneric engine renders each eye's view into a separate framebuffer
// (left/right DG_ScreenBuffer). We upload both into GL textures and draw each
// as a full-viewport quad into that eye's swapchain image — the compositor
// presents them as a stereo projection layer, giving true stereoscopic 3D.
//
// On top of each eye image we also draw small cubes at the tracked hand-joint
// positions (XR_EXT_hand_tracking) so the user's hands are visible in-game.
#pragma once

#include "xr_engine.h"

#define GLR_MAX_JOINTS 52  // 2 hands x 26 joints

typedef struct {
    GLuint program;       // fullscreen textured quad (doom frame)
    GLuint cubeProgram;   // unlit colored cubes for hand joints
    GLuint doomTex[2];    // per-eye DOOMGENERIC_RESX x DOOMGENERIC_RESY
    bool   texInit[2];
    GLuint quadVao;
    GLuint cubeVao;
    GLuint cubeVbo;
    GLuint fbo;

    // hand joints in XR local space (m); jointsVisible = active joint count
    float jointPos[GLR_MAX_JOINTS][3];
    int   jointsVisible[2];   // per-hand active flag
} GlRenderer;

bool glr_init(GlRenderer* r);

// Upload an eye framebuffer. eye = 0/1.
void glr_upload_frame(GlRenderer* r, int eye, const uint32_t* pixels,
                      int w, int h);

// Update hand-joint positions for the in-world cube viz. pos = xyz metres in
// XR local space; count up to GLR_MAX_JOINTS; 'visible' per hand.
void glr_set_joints(GlRenderer* r, const float* pos, int count,
                    const int* visible);

// Render eye `eye` into its swapchain using the located view pose/fov.
void glr_draw_eye(GlRenderer* r, XrEngine* e, int eye);

void glr_shutdown(GlRenderer* r);
