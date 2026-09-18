#include "gl_renderer.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Minimal mat4 / quat math (column-major, like GLSL)
// ---------------------------------------------------------------------------

typedef struct { float m[16]; } Mat4;

static Mat4 mat4_identity(void) {
    Mat4 r = {{0}};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
}

static Mat4 mat4_mul(Mat4 a, Mat4 b) {
    Mat4 r = {{0}};
    for (int c = 0; c < 4; c++)
        for (int rw = 0; rw < 4; rw++)
            for (int k = 0; k < 4; k++)
                r.m[c * 4 + rw] += a.m[k * 4 + rw] * b.m[c * 4 + k];
    return r;
}

// XrFovf angles -> perspective projection (0..1 depth is what GLES expects via
// the standard OpenGL projection; XR gives real fov per eye).
static Mat4 mat4_projection(XrFovf fov, float nearZ, float farZ) {
    float tanL = tanf(fov.angleLeft), tanR = tanf(fov.angleRight);
    float tanD = tanf(fov.angleDown), tanU = tanf(fov.angleUp);
    float w = tanR - tanL, h = tanD - tanU;

    Mat4 p = {{0}};
    p.m[0] = 2.0f / w;
    p.m[5] = 2.0f / h;
    p.m[8] = (tanR + tanL) / w;
    p.m[9] = (tanU + tanD) / h;
    p.m[10] = -(farZ + nearZ) / (farZ - nearZ);
    p.m[11] = -1.0f;
    p.m[14] = -(farZ * (nearZ + nearZ)) / (farZ - nearZ);
    return p;
}

// Rigid transform (quat + pos) -> 4x4.
static Mat4 mat4_from_pose(XrPosef pose) {
    XrQuaternionf q = pose.orientation;
    XrVector3f v = pose.position;
    float x2 = q.x + q.x, y2 = q.y + q.y, z2 = q.z + q.z;
    float xx = q.x * x2, xy = q.x * y2, xz = q.x * z2;
    float yy = q.y * y2, yz = q.y * z2, zz = q.z * z2;
    float wx = q.w * x2, wy = q.w * y2, wz = q.w * z2;

    Mat4 r = mat4_identity();
    r.m[0] = 1 - (yy + zz); r.m[1] = xy + wz;       r.m[2] = xz - wy;
    r.m[4] = xy - wz;       r.m[5] = 1 - (xx + zz); r.m[6] = yz + wx;
    r.m[8] = xz + wy;       r.m[9] = yz - wx;       r.m[10] = 1 - (xx + yy);
    r.m[12] = v.x; r.m[13] = v.y; r.m[14] = v.z;
    return r;
}

// Inverse of a rigid transform = transpose rotation, negate translation.
static Mat4 mat4_invert_rigid(Mat4 t) {
    Mat4 r = mat4_identity();
    for (int c = 0; c < 3; c++)
        for (int rw = 0; rw < 3; rw++)
            r.m[c * 4 + rw] = t.m[rw * 4 + c];
    r.m[12] = -(t.m[0] * t.m[12] + t.m[1] * t.m[13] + t.m[2] * t.m[14]);
    r.m[13] = -(t.m[4] * t.m[12] + t.m[5] * t.m[13] + t.m[6] * t.m[14]);
    r.m[14] = -(t.m[8] * t.m[12] + t.m[9] * t.m[13] + t.m[10] * t.m[14]);
    return r;
}

// ---------------------------------------------------------------------------
// Shaders
// ---------------------------------------------------------------------------

// Fullscreen quad: clip-space positions, no view transform — the doom texture
// already encodes the eye's view. Byte swizzle: DOOM pixels are little-endian
// 0x00RRGGBB words, uploaded as GL_RGBA bytes -> B,G,R,0.
static const char* kQuadVert =
    "#version 300 es\n"
    "layout(location=0) in vec2 aPos;\n"
    "out vec2 vUV;\n"
    "void main() { vUV = aPos * 0.5 + 0.5; vUV.y = 1.0 - vUV.y; "
    "  gl_Position = vec4(aPos, 0.0, 1.0); }\n";

static const char* kQuadFrag =
    "#version 300 es\n"
    "precision mediump float;\n"
    "uniform sampler2D uTex;\n"
    "in vec2 vUV;\n"
    "out vec4 frag;\n"
    "void main() { vec4 t = texture(uTex, vUV); frag = vec4(t.b, t.g, t.r, 1.0); }\n";

// Joint cubes: per-instance model offset + world position, view/proj applied.
static const char* kCubeVert =
    "#version 300 es\n"
    "layout(location=0) in vec3 aPos;\n"     // unit cube corner
    "layout(location=1) in vec3 aOffset;\n"  // joint position, local space
    "layout(location=2) in vec3 aColor;\n"
    "uniform mat4 uViewProj;\n"
    "out vec3 vColor;\n"
    "void main() { vColor = aColor; "
    "  gl_Position = uViewProj * vec4(aPos * 0.006 + aOffset, 1.0); }\n";

static const char* kCubeFrag =
    "#version 300 es\n"
    "precision mediump float;\n"
    "in vec3 vColor;\n"
    "out vec4 frag;\n"
    "void main() { frag = vec4(vColor, 0.85); }\n";

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

// unit cube centered at origin, 36 verts (12 tris), side = 1
static const float kCubeVerts[] = {
    -0.5f,-0.5f,-0.5f,  0.5f, 0.5f,-0.5f,  0.5f,-0.5f,-0.5f,
    -0.5f,-0.5f,-0.5f, -0.5f, 0.5f,-0.5f,  0.5f, 0.5f,-0.5f,
    -0.5f,-0.5f, 0.5f,  0.5f,-0.5f, 0.5f,  0.5f, 0.5f, 0.5f,
    -0.5f,-0.5f, 0.5f,  0.5f, 0.5f, 0.5f, -0.5f, 0.5f, 0.5f,
    -0.5f,-0.5f,-0.5f,  0.5f,-0.5f,-0.5f,  0.5f,-0.5f, 0.5f,
    -0.5f,-0.5f,-0.5f,  0.5f,-0.5f, 0.5f, -0.5f,-0.5f, 0.5f,
    -0.5f, 0.5f,-0.5f,  0.5f, 0.5f, 0.5f,  0.5f, 0.5f,-0.5f,
    -0.5f, 0.5f,-0.5f, -0.5f, 0.5f, 0.5f,  0.5f, 0.5f, 0.5f,
    -0.5f,-0.5f,-0.5f, -0.5f,-0.5f, 0.5f, -0.5f, 0.5f,-0.5f,
     0.5f, 0.5f,-0.5f, -0.5f,-0.5f, 0.5f, -0.5f,-0.5f,-0.5f,
     0.5f, 0.5f,-0.5f,  0.5f,-0.5f, 0.5f,  0.5f,-0.5f,-0.5f,
     0.5f, 0.5f, 0.5f,  0.5f,-0.5f,-0.5f,  0.5f,-0.5f, 0.5f,
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

static void build_cubes(GlRenderer* r) {
    glGenVertexArrays(1, &r->cubeVao);
    glBindVertexArray(r->cubeVao);
    glGenBuffers(1, &r->cubeVbo);
    glBindBuffer(GL_ARRAY_BUFFER, r->cubeVbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kCubeVerts), kCubeVerts,
                 GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), NULL);
    glBindVertexArray(0);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool glr_init(GlRenderer* r) {
    memset(r, 0, sizeof(*r));

    r->program = link_program(kQuadVert, kQuadFrag);
    r->cubeProgram = link_program(kCubeVert, kCubeFrag);
    if (!r->program || !r->cubeProgram) return false;

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
    build_cubes(r);
    glGenFramebuffers(1, &r->fbo);
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

void glr_draw_eye(GlRenderer* r, XrEngine* e, int eye) {
    uint32_t index = 0;
    GLuint tex = xr_acquire_eye_image(e, eye, &index);
    if (!tex) return;

    XrEyeSwapchain* sc = &e->eyeSwapchains[eye];
    XrView* view = &e->views[eye];

    glBindFramebuffer(GL_FRAMEBUFFER, r->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         tex, 0);

    glViewport(0, 0, sc->width, sc->height);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glDisable(GL_DEPTH_TEST);
    glUseProgram(r->program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, r->doomTex[eye]);
    glUniform1i(glGetUniformLocation(r->program, "uTex"), 0);
    glBindVertexArray(r->quadVao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);

    // Hand-joint cubes in world space
    int jointCount = r->jointsVisible[0] + r->jointsVisible[1];
    if (jointCount > 0) {
        Mat4 viewMat = mat4_invert_rigid(mat4_from_pose(view->pose));
        Mat4 projMat = mat4_projection(view->fov, 0.01f, 50.0f);
        Mat4 vp = mat4_mul(projMat, viewMat);

        glEnable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glUseProgram(r->cubeProgram);
        glUniformMatrix4fv(glGetUniformLocation(r->cubeProgram, "uViewProj"),
                          1, GL_FALSE, vp.m);

        // build instance data: offset + color per joint
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
                // left hand warm, right hand cool
                dst[3] = h ? 0.35f : 0.95f;
                dst[4] = 0.55f;
                dst[5] = h ? 0.95f : 0.35f;
                j++;
            }
        }
        GLuint instVbo;
        glGenBuffers(1, &instVbo);
        glBindBuffer(GL_ARRAY_BUFFER, instVbo);
        glBufferData(GL_ARRAY_BUFFER, n * 6 * sizeof(float), inst,
                     GL_DYNAMIC_DRAW);
        glBindVertexArray(r->cubeVao);
        glBindBuffer(GL_ARRAY_BUFFER, instVbo);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), NULL);
        glVertexAttribDivisor(1, 1);
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float),
                              (void*)(3 * sizeof(float)));
        glVertexAttribDivisor(2, 1);
        glDrawArraysInstanced(GL_TRIANGLES, 0, 36, n);
        glVertexAttribDivisor(1, 0);
        glVertexAttribDivisor(2, 0);
        glDisableVertexAttribArray(1);
        glDisableVertexAttribArray(2);
        glBindVertexArray(0);
        glDeleteBuffers(1, &instVbo);
        free(inst);
        glDisable(GL_BLEND);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    e->projViews[eye].pose = view->pose;
    e->projViews[eye].fov = view->fov;

    xr_release_eye_image(e, eye);
}

void glr_shutdown(GlRenderer* r) {
    glDeleteFramebuffers(1, &r->fbo);
    glDeleteProgram(r->program);
    glDeleteProgram(r->cubeProgram);
    glDeleteTextures(2, r->doomTex);
}
