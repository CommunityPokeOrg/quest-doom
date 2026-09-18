#include "gl_renderer.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef GlMat4 Mat4;

// ---------------------------------------------------------------------------
// Shaders
// ---------------------------------------------------------------------------

// Immersive quad: clip-space fullscreen, the doom texture already encodes the
// eye's view of the world (per-eye render with stereo offset baked in), so no
// view transform is applied — this is first-person, not a head-locked window.
// Byte swizzle: DOOM pixels are little-endian 0x00RRGGBB words uploaded as
// GL_RGBA bytes -> B,G,R,0.
// The OpenXR compositor samples swapchain images top-left-origin while GL
// writes bottom-up, so for texture targets the quad samples unflipped (texel
// row 0 = compositor top); a native window framebuffer needs the V flipped.
static const char* kQuadVert =
    "#version 300 es\n"
    "layout(location=0) in vec2 aPos;\n"
    "uniform float uVFlip;\n"
    "out vec2 vUV;\n"
    "void main() { vec2 uv = aPos * 0.5 + 0.5; "
    "  vUV = vec2(uv.x, mix(uv.y, 1.0 - uv.y, uVFlip)); "
    "  gl_Position = vec4(aPos, 0.0, 1.0); }\n";

static const char* kTexFrag =
    "#version 300 es\n"
    "precision mediump float;\n"
    "uniform sampler2D uTex;\n"
    "in vec2 vUV;\n"
    "out vec4 frag;\n"
    "void main() { vec4 t = texture(uTex, vUV); frag = vec4(t.b, t.g, t.r, 1.0); }\n";

// World-locked quad: the doom frame on a fixed app-space plane (menus etc.).
// uUVFlip lets the backend correct panel texture orientation: XR texture
// targets are consumed by the compositor (top-left origin) while a native
// window surface presents bottom-up, and the two disagree on panel UV.
static const char* kWorldVert =
    "#version 300 es\n"
    "layout(location=0) in vec3 aPos;\n"
    "layout(location=1) in vec2 aUV;\n"
    "uniform mat4 uViewProj;\n"
    "uniform mat4 uModel;\n"
    "uniform vec2 uUVFlip;\n"
    "out vec2 vUV;\n"
    "void main() { vUV = mix(aUV, 1.0 - aUV, uUVFlip); "
    "  gl_Position = uViewProj * uModel * vec4(aPos,1); }\n";

// Joint cubes: per-instance world offset + color, view/proj applied.
static const char* kCubeVert =
    "#version 300 es\n"
    "layout(location=0) in vec3 aPos;\n"     // unit cube corner
    "layout(location=1) in vec3 aOffset;\n"  // joint position, app space
    "layout(location=2) in vec3 aColor;\n"
    "uniform mat4 uViewProj;\n"
    "out vec3 vColor;\n"
    "void main() { vColor = aColor; "
    "  gl_Position = uViewProj * vec4(aPos * 0.005 + aOffset, 1.0); }\n";

static const char* kColorFrag =
    "#version 300 es\n"
    "precision mediump float;\n"
    "in vec3 vColor;\n"
    "out vec4 frag;\n"
    "void main() { frag = vec4(vColor, 0.85); }\n";

// Bone segments: unit cube stretched from aPosA to aPosB with radius aRadius.
static const char* kBoneVert =
    "#version 300 es\n"
    "layout(location=0) in vec3 aPos;\n"      // unit cube corner
    "layout(location=1) in vec3 aPosA;\n"     // bone start, app space
    "layout(location=2) in vec3 aPosB;\n"     // bone end
    "layout(location=3) in vec3 aColor;\n"
    "layout(location=4) in float aRadius;\n"
    "uniform mat4 uViewProj;\n"
    "out vec3 vColor;\n"
    "void main() {\n"
    "  vColor = aColor;\n"
    "  vec3 axis = aPosB - aPosA;\n"
    "  float len = max(length(axis), 1e-5);\n"
    "  axis /= len;\n"
    "  vec3 ref = abs(axis.y) > 0.9 ? vec3(1,0,0) : vec3(0,1,0);\n"
    "  vec3 right = normalize(cross(ref, axis));\n"
    "  vec3 up = cross(axis, right);\n"
    "  vec3 w = aPosA + axis * (aPos.x + 0.5) * len\n"
    "         + right * aPos.y * aRadius + up * aPos.z * aRadius;\n"
    "  gl_Position = uViewProj * vec4(w, 1.0);\n"
    "}\n";

static GLuint compile_shader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        LOGE("shader compile failed: %s", log);
    }
    return s;
}

static GLuint link_program(const char* vs, const char* fs) {
    GLuint v = compile_shader(GL_VERTEX_SHADER, vs);
    GLuint f = compile_shader(GL_FRAGMENT_SHADER, fs);
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(p, sizeof(log), NULL, log);
        LOGE("program link failed: %s", log);
        glDeleteProgram(p);
        p = 0;
    }
    glDeleteShader(v);
    glDeleteShader(f);
    return p;
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

static const float kQuadVerts[] = {
    -1.f, -1.f,  1.f, -1.f,  -1.f, 1.f,
     1.f, -1.f,  1.f, 1.f,   -1.f, 1.f,
};

// World-locked panel: 2.56m x 1.6m (matches doom 640x400 aspect), pos+uv.
static const float kWorldQuadVerts[] = {
    -1.28f, -0.8f, 0.f,  0.f, 1.f,
     1.28f, -0.8f, 0.f,  1.f, 1.f,
    -1.28f,  0.8f, 0.f,  0.f, 0.f,
     1.28f, -0.8f, 0.f,  1.f, 1.f,
     1.28f,  0.8f, 0.f,  1.f, 0.f,
    -1.28f,  0.8f, 0.f,  0.f, 0.f,
};

// unit cube centered at origin, 36 verts (12 tris), side = 1
static const float kCubeVerts[] = {
    -0.5f,-0.5f,-0.5f,  0.5f, 0.5f,-0.5f,  0.5f,-0.5f,-0.5f,
    -0.5f,-0.5f,-0.5f, -0.5f, 0.5f,-0.5f,  0.5f, 0.5f,-0.5f,
    -0.5f,-0.5f, 0.5f,  0.5f,-0.5f, 0.5f,  0.5f, 0.5f, 0.5f,
    -0.5f,-0.5f, 0.5f,  0.5f, 0.5f, 0.5f, -0.5f, 0.5f, 0.5f,
    -0.5f,-0.5f,-0.5f,  0.5f,-0.5f,-0.5f,  0.5f,-0.5f, 0.5f,
    -0.5f,-0.5f,-0.5f,  0.5f,-0.5f, 0.5f, -0.5f,-0.5f, 0.5f,
    -0.5f, 0.5f,-0.5f,  0.5f, 0.5f,-0.5f,  0.5f, 0.5f,-0.5f,
    -0.5f, 0.5f,-0.5f, -0.5f, 0.5f, 0.5f,  0.5f, 0.5f, 0.5f,
    -0.5f,-0.5f,-0.5f, -0.5f,-0.5f, 0.5f, -0.5f,-0.5f,-0.5f,
     0.5f, 0.5f,-0.5f, -0.5f,-0.5f, 0.5f, -0.5f,-0.5f,-0.5f,
     0.5f, 0.5f, 0.5f,  0.5f,-0.5f, 0.5f,  0.5f,-0.5f, 0.5f,
};

// XR_HAND_JOINT_* bone connectivity (default joint set, indices per hand:
// palm=0 wrist=1 thumb 2-5, index 6-10, middle 11-15, ring 16-20, little
// 21-25; each finger metacarpal->proximal->intermediate->distal->tip).
#define BONE_COUNT 28
static const int kBones[BONE_COUNT][2] = {
    {1, 0},
    // knuckle webbing across the metacarpals
    {6, 11}, {11, 16}, {16, 21},
    // thumb
    {1, 2}, {2, 3}, {3, 4}, {4, 5},
    // index
    {1, 6}, {6, 7}, {7, 8}, {8, 9}, {9, 10},
    // middle
    {1, 11}, {11, 12}, {12, 13}, {13, 14}, {14, 15},
    // ring
    {1, 16}, {16, 17}, {17, 18}, {18, 19}, {19, 20},
    // little
    {1, 21}, {21, 22}, {22, 23}, {23, 24}, {24, 25},
};

static void build_quad(GlRenderer* r) {
    glGenVertexArrays(1, &r->quadVao);
    glBindVertexArray(r->quadVao);
    GLuint vbo;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kQuadVerts), kQuadVerts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), NULL);
    glBindVertexArray(0);
    glDeleteBuffers(1, &vbo);
}

static void build_world_quad(GlRenderer* r) {
    glGenVertexArrays(1, &r->worldVao);
    glBindVertexArray(r->worldVao);
    GLuint vbo;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kWorldQuadVerts), kWorldQuadVerts,
                 GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), NULL);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
                          (void*)(3 * sizeof(float)));
    glBindVertexArray(0);
    glDeleteBuffers(1, &vbo);
}

// Shared cube VBO (mesh) for joints and bones; per-instance data streamed per
// draw. cubeVao gets offset+color instancing; boneVao gets A/B/color/radius.
static void build_cubes(GlRenderer* r) {
    glGenBuffers(1, &r->cubeVbo);
    glBindBuffer(GL_ARRAY_BUFFER, r->cubeVbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kCubeVerts), kCubeVerts,
                 GL_STATIC_DRAW);

    glGenVertexArrays(1, &r->cubeVao);
    glBindVertexArray(r->cubeVao);
    glBindBuffer(GL_ARRAY_BUFFER, r->cubeVbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), NULL);
    glBindVertexArray(0);

    glGenVertexArrays(1, &r->boneVao);
    glBindVertexArray(r->boneVao);
    glBindBuffer(GL_ARRAY_BUFFER, r->cubeVbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), NULL);
    glGenBuffers(1, &r->boneInstVbo);
    glBindBuffer(GL_ARRAY_BUFFER, r->boneInstVbo);
    glBufferData(GL_ARRAY_BUFFER,
                 2 * BONE_COUNT * 10 * sizeof(float), NULL, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 10 * sizeof(float), NULL);
    glVertexAttribDivisor(1, 1);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, 10 * sizeof(float),
                          (void*)(3 * sizeof(float)));
    glVertexAttribDivisor(2, 1);
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, 10 * sizeof(float),
                          (void*)(6 * sizeof(float)));
    glVertexAttribDivisor(3, 1);
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, 10 * sizeof(float),
                          (void*)(9 * sizeof(float)));
    glVertexAttribDivisor(4, 1);
    glBindVertexArray(0);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool glr_init(GlRenderer* r) {
    memset(r, 0, sizeof(*r));

    r->program = link_program(kQuadVert, kTexFrag);
    r->worldProgram = link_program(kWorldVert, kTexFrag);
    r->cubeProgram = link_program(kCubeVert, kColorFrag);
    r->boneProgram = link_program(kBoneVert, kColorFrag);
    if (!r->program || !r->worldProgram || !r->cubeProgram || !r->boneProgram)
        return false;

    glGenTextures(2, r->doomTex);
    for (int i = 0; i < 2; i++) {
        glBindTexture(GL_TEXTURE_2D, r->doomTex[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        r->texInit[i] = false;
    }

    build_quad(r);
    build_world_quad(r);
    build_cubes(r);
    glGenFramebuffers(1, &r->fbo);
    glw_init(&r->world);   // non-fatal; quad path remains the fallback
    return true;
}

void glr_upload_frame(GlRenderer* r, int eye, const uint32_t* pixels,
                      int w, int h) {
    if (eye < 0 || eye > 1) return;
    glBindTexture(GL_TEXTURE_2D, r->doomTex[eye]);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (!r->texInit[eye]) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, pixels);
        r->texInit[eye] = true;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA,
                        GL_UNSIGNED_BYTE, pixels);
    }
}

void glr_set_joints(GlRenderer* r, const float* pos, int count,
                    const int* visible) {
    if (count > GLR_MAX_JOINTS) count = GLR_MAX_JOINTS;
    memcpy(r->jointPos, pos, count * 3 * sizeof(float));
    r->jointsVisible[0] = visible[0];
    r->jointsVisible[1] = visible[1];
}

void glr_set_immersive(GlRenderer* r, bool immersive) { r->immersive = immersive; }

void glr_set_world_mode(GlRenderer* r, bool enabled) {
    r->worldMode = enabled;
}

void glr_set_panel_uv_flip(GlRenderer* r, float fx, float fy) {
    r->panelUVFlipX = fx;
    r->panelUVFlipY = fy;
}

bool glr_world_active(const GlRenderer* r) {
    return r->immersive && r->worldMode && r->world && glw_available(r->world);
}

const char* glr_world_fail(const GlRenderer* r) {
    if (!r->world) return "gl_world not initialized";
    return glw_fail_reason(r->world);
}

void glr_world_begin_frame(GlRenderer* r) {
    if (r->world) glw_begin_frame(r->world);
}

void glr_world_frame_camera(GlRenderer* r, float camXu, float camYu) {
    if (r->world) glw_set_frame_camera(r->world, camXu, camYu);
}

void glr_world_camera(GlRenderer* r,
                      float headX, float headY, float headZ, float headYawDeg,
                      float camXu, float camYu, float camZu, float moAngleDeg) {
    if (r->world)
        glw_set_camera(r->world, headX, headY, headZ, headYawDeg,
                       camXu, camYu, camZu, moAngleDeg);
}

// Depth renderbuffer matching the swapchain dimensions (depth-test for
// world-locked content; swapped only when size changes).
static void ensure_depth(GlRenderer* r, int w, int h) {
    if (r->depthRbo && r->depthW == w && r->depthH == h) return;
    if (!r->depthRbo) glGenRenderbuffers(1, &r->depthRbo);
    glBindRenderbuffer(GL_RENDERBUFFER, r->depthRbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, r->depthRbo);
    r->depthW = w;
    r->depthH = h;
}

void glr_draw_eye_params(GlRenderer* r, const GlrEyeParams* p) {
    if (p->targetTex) {
        glBindFramebuffer(GL_FRAMEBUFFER, r->fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                             GL_TEXTURE_2D, p->targetTex, 0);
        ensure_depth(r, p->vpW, p->vpH);
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    // On a shared window framebuffer each eye owns a viewport: the scissor
    // keeps this eye's clear+draw from wiping or bleeding into the other
    // half (glClear ignores glViewport). Harmless on texture targets.
    glEnable(GL_SCISSOR_TEST);
    glScissor(p->vpX, p->vpY, p->vpW, p->vpH);
    glViewport(p->vpX, p->vpY, p->vpW, p->vpH);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    Mat4 viewMat, projMat;
    memcpy(viewMat.m, p->view, sizeof(viewMat.m));
    memcpy(projMat.m, p->proj, sizeof(projMat.m));
    Mat4 vp = glmat_mul(projMat, viewMat);
    int eye = p->srcEye ? 1 : 0;

    // --- doom frame ---
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, r->doomTex[eye]);
    if (glr_world_active(r)) {
        // true-3D level geometry (walls/flats/sprites as GL triangles)
        glw_draw(r->world, vp.m);
    } else if (r->immersive) {
        // first-person: the texture IS the eye view — no pose applied
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glUseProgram(r->program);
        glUniform1i(glGetUniformLocation(r->program, "uTex"), 0);
        // XR swapchain texture targets are sampled top-left-origin by the
        // compositor; a native window framebuffer presents bottom-up.
        glUniform1f(glGetUniformLocation(r->program, "uVFlip"),
                    p->targetTex ? 0.0f : 1.0f);
        glBindVertexArray(r->quadVao);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glBindVertexArray(0);
        glDepthMask(GL_TRUE);
    } else {
        // World-locked panel: placed once, in front of wherever the user was
        // facing when it spawned (yaw only — never re-anchored, never VIEW).
        // Works whether appSpace is STAGE (floor origin) or LOCAL (head
        // origin): the pose is derived from the first located head pose.
        if (!r->panelPlaced) {
            // Recover the head pose from the view matrix: head world
            // transform is the inverse rigid of world->eye, and forward is
            // the -Z column of its rotation.
            Mat4 headW = glmat_invert_rigid(viewMat);
            float fx = -headW.m[8];
            float fz = -headW.m[10];
            float hp[3] = {headW.m[12], headW.m[13], headW.m[14]};
            float fl = sqrtf(fx * fx + fz * fz);
            if (fl > 1e-4f) { fx /= fl; fz /= fl; }
            float px = hp[0] + fx * 2.4f;
            float py = hp[1] - 0.1f;
            float pz = hp[2] + fz * 2.4f;
            // rotate the quad to face the user (yaw of the flattened forward)
            // quad +Z is its front face; it must point opposite the
            // user->panel direction (back at the user), so yaw on -forward
            float yaw = atan2f(-fx, -fz);
            float c = cosf(yaw), s = sinf(yaw);
            Mat4 m = glmat_identity();
            m.m[0] = c;  m.m[2] = -s;
            m.m[8] = s;  m.m[10] = c;
            m.m[12] = px;
            m.m[13] = py;
            m.m[14] = pz;
            memcpy(r->panelModel, m.m, sizeof(r->panelModel));
            r->panelPlaced = true;
            LOGI("panel placed at (%.2f, %.2f, %.2f) yaw %.1f deg",
                 px, py, pz, yaw * 57.29578f);
        }
        Mat4 model;
        memcpy(model.m, r->panelModel, sizeof(model.m));
        glEnable(GL_DEPTH_TEST);
        glUseProgram(r->worldProgram);
        glUniformMatrix4fv(glGetUniformLocation(r->worldProgram, "uViewProj"),
                          1, GL_FALSE, vp.m);
        glUniform2f(glGetUniformLocation(r->worldProgram, "uUVFlip"),
                    r->panelUVFlipX, r->panelUVFlipY);
        glUniformMatrix4fv(glGetUniformLocation(r->worldProgram, "uModel"),
                          1, GL_FALSE, model.m);
        glUniform1i(glGetUniformLocation(r->worldProgram, "uTex"), 0);
        glBindVertexArray(r->worldVao);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glBindVertexArray(0);
    }

    // --- tracked hands: joint knuckles + anatomical bones ---
    int jointCount = r->jointsVisible[0] + r->jointsVisible[1];
    if (jointCount > 0) {
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        static const float kHandColor[2][3] = {
            {0.95f, 0.55f, 0.35f},   // left: warm
            {0.35f, 0.55f, 0.95f},   // right: cool
        };

        // joint knuckles (small cubes)
        int n = 0;
        for (int h = 0; h < 2; h++) if (r->jointsVisible[h]) n += 26;
        float* inst = malloc(n * 6 * sizeof(float));
        int j = 0;
        for (int h = 0; h < 2; h++) {
            if (!r->jointsVisible[h]) continue;
            for (int i = 0; i < 26; i++) {
                float* dst = &inst[j * 6];
                const float* p = r->jointPos[h * 26 + i];
                dst[0] = p[0]; dst[1] = p[1]; dst[2] = p[2];
                dst[3] = kHandColor[h][0];
                dst[4] = kHandColor[h][1];
                dst[5] = kHandColor[h][2];
                j++;
            }
        }
        glUseProgram(r->cubeProgram);
        glUniformMatrix4fv(glGetUniformLocation(r->cubeProgram, "uViewProj"),
                          1, GL_FALSE, vp.m);
        GLuint instVbo;
        glGenBuffers(1, &instVbo);
        glBindVertexArray(r->cubeVao);
        glBindBuffer(GL_ARRAY_BUFFER, instVbo);
        glBufferData(GL_ARRAY_BUFFER, n * 6 * sizeof(float), inst,
                     GL_DYNAMIC_DRAW);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), NULL);
        glVertexAttribDivisor(1, 1);
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float),
                              (void*)(3 * sizeof(float)));
        glVertexAttribDivisor(2, 1);
        glDrawArraysInstanced(GL_TRIANGLES, 0, 36, n);
        glBindVertexArray(0);
        glDeleteBuffers(1, &instVbo);
        free(inst);

        // bone segments
        int nb = 0;
        for (int h = 0; h < 2; h++) if (r->jointsVisible[h]) nb += BONE_COUNT;
        float* bdata = malloc(nb * 10 * sizeof(float));
        int bi = 0;
        for (int h = 0; h < 2; h++) {
            if (!r->jointsVisible[h]) continue;
            for (int b = 0; b < BONE_COUNT; b++) {
                const float* a = r->jointPos[h * 26 + kBones[b][0]];
                const float* c = r->jointPos[h * 26 + kBones[b][1]];
                float* dst = &bdata[bi * 10];
                dst[0] = a[0]; dst[1] = a[1]; dst[2] = a[2];
                dst[3] = c[0]; dst[4] = c[1]; dst[5] = c[2];
                dst[6] = kHandColor[h][0] * 0.75f;
                dst[7] = kHandColor[h][1] * 0.75f;
                dst[8] = kHandColor[h][2] * 0.75f;
                // wrist/palm bones thicker than finger bones
                dst[9] = (b < 4) ? 0.011f : 0.007f;
                bi++;
            }
        }
        glUseProgram(r->boneProgram);
        glUniformMatrix4fv(glGetUniformLocation(r->boneProgram, "uViewProj"),
                          1, GL_FALSE, vp.m);
        glBindVertexArray(r->boneVao);
        glBindBuffer(GL_ARRAY_BUFFER, r->boneInstVbo);
        glBufferData(GL_ARRAY_BUFFER, nb * 10 * sizeof(float), bdata,
                     GL_DYNAMIC_DRAW);
        glDrawArraysInstanced(GL_TRIANGLES, 0, 36, nb);
        glBindVertexArray(0);
        free(bdata);

        glDisable(GL_BLEND);
    }

    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // Texture targets are consumed by a compositor: ensure all rendering is
    // complete before the caller releases the image, or it can present a
    // black/partially-rendered texture. Window-framebuffer callers swap next.
    if (p->targetTex) glFinish();
}

void glr_shutdown(GlRenderer* r) {
    glDeleteFramebuffers(1, &r->fbo);
    if (r->depthRbo) glDeleteRenderbuffers(1, &r->depthRbo);
    glDeleteProgram(r->program);
    glDeleteProgram(r->worldProgram);
    glDeleteProgram(r->cubeProgram);
    glDeleteProgram(r->boneProgram);
    glDeleteTextures(2, r->doomTex);
}
