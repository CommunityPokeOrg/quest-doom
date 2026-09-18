// GLES3 renderer: draws the doom framebuffer either as a per-eye fullscreen
// image (immersive first-person, used in-level) or onto a world-locked quad
// at a fixed tracking-space pose (menus / title / intermission / demos —
// never head-locked), plus tracked-hand skeleton cubes+bone segments.
//
// The render core is backend-agnostic: callers pass a GlrEyeParams with
// world->eye + projection matrices and a viewport/target. The OpenXR
// swapchain wrapper lives in gl_xr.c; the Cardboard backend in cb_engine.c
// builds the same params from sensor data.
#pragma once

#include "gl_common.h"
#include "gl_world.h"

#define GLR_MAX_JOINTS 52  // 2 hands x 26 XR hand joints

// One eye's render inputs. Backend-neutral.
typedef struct {
    float view[16];     // world->eye rigid view matrix
    float proj[16];     // projection (backend applies any needed flip)
    int   vpX, vpY, vpW, vpH;   // pixel viewport
    GLuint targetTex;   // GL texture to render into; 0 = default framebuffer
    int   srcEye;       // which doomTex[] upload this eye displays (0/1)
} GlrEyeParams;

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
    GLuint depthRbo;      // depth attachment matching the render size
    int    depthW, depthH;
    bool   immersive;     // true => first-person; false => panel
    bool   worldMode;     // in-level: render real 3D level geometry
    GlWorld* world;       // true-3D level geometry renderer
    bool   panelPlaced;   // world-locked panel pose computed
    float  panelModel[16];
    float  panelUVFlipX;  // panel texture u mirror (backend-dependent)
    float  panelUVFlipY;  // panel texture v flip (backend-dependent)

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
// true-3D level geometry controls (in-level only).
void glr_set_world_mode(GlRenderer* r, bool enabled);
bool glr_world_active(const GlRenderer* r);
void glr_set_panel_uv_flip(GlRenderer* r, float fx, float fy);
const char* glr_world_fail(const GlRenderer* r);
void glr_world_begin_frame(GlRenderer* r);
void glr_world_frame_camera(GlRenderer* r, float camXu, float camYu);
void glr_world_camera(GlRenderer* r,
                      float headX, float headY, float headZ, float headYawDeg,
                      float camXu, float camYu, float camZu, float moAngleDeg);
// Backend-neutral per-eye render. Call once per eye.
void glr_draw_eye_params(GlRenderer* r, const GlrEyeParams* p);
void glr_shutdown(GlRenderer* r);
