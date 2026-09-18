// GLES3 renderer: draws the doom framebuffer either as a per-eye fullscreen
// image (immersive first-person, used in-level) or onto a world-locked quad
// at a fixed app-space pose (menus / title / intermission / demos — never
// head-locked), plus tracked-hand skeleton cubes+bone segments.
#pragma once

#include "xr_engine.h"

#define GLR_MAX_JOINTS 52  // 2 hands x 26 XR hand joints

typedef struct {
    GLuint program;       // fullscreen textured quad (immersive doom frame)
    GLuint worldProgram;  // world-locked textured quad (non-level frames)
    GLuint cubeProgram;   // unlit colored cubes for hand joints
    GLuint boneProgram;   // stretched cubes between joint pairs (bones)
    GLuint doomTex[2];    // per-eye DOOMGENERIC_RESX x DOOMGENERIC_RESY
    bool   texInit[2];
    GLuint quadVao;
    GLuint worldVao;
    GLuint cubeVao;
    GLuint cubeVbo;
    GLuint boneVao;
    GLuint boneInstVbo;
    GLuint fbo;
    GLuint depthRbo;      // depth attachment matching swapchain size
    int    depthW, depthH;
    bool   immersive;     // true => first-person fullscreen; false => panel
    bool   panelPlaced;   // world-locked panel pose computed
    float  panelModel[16];

    float jointPos[GLR_MAX_JOINTS][3];
    int   jointsVisible[2];
} GlRenderer;

bool glr_init(GlRenderer* r);
// Upload the current eye's rendered frame.
void glr_upload_frame(GlRenderer* r, int eye, const uint32_t* pixels,
                      int w, int h);
// pos = xyz triplets [hand*26+joint], visible per hand.
void glr_set_joints(GlRenderer* r, const float* pos, int count,
                    const int* visible);
// true => doom frame fills the eye view; false => world-locked quad.
void glr_set_immersive(GlRenderer* r, bool immersive);
// Render into the acquired swapchain image for eye `e` (0=left,1=right).
void glr_draw_eye(GlRenderer* r, XrEngine* e, int eye);
void glr_shutdown(GlRenderer* r);
