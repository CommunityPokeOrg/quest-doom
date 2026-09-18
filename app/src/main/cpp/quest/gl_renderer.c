#include "gl_renderer.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Minimal mat4 / quat math (column-major, like GLSL)
// ---------------------------------------------------------------------------

typedef struct { float m[16]; } Mat4;

static Mat4 mat4_identity(void) {
    Mat4 r = {0};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
}

static Mat4 mat4_mul(Mat4 a, Mat4 b) {
    Mat4 r = {0};
    for (int c = 0; c < 4; c++)
        for (int rw = 0; rw < 4; rw++)
            for (int k = 0; k < 4; k++)
                r.m[c * 4 + rw] += a.m[k * 4 + rw] * b.m[c * 4 + k];
    return r;
}

// XrFovf angles → perspective projection (0..1 depth, GLES style).
static Mat4 mat4_projection(XrFovf fov, float nearZ, float farZ) {
    float tanL = tanf(fov.angleLeft), tanR = tanf(fov.angleRight);
    float tanD = tanf(fov.angleDown), tanU = tanf(fov.angleUp);
    float w = tanR - tanL, h = tanD - tanU;

    Mat4 p = {0};
    p.m[0] = 2.0f / w;
    p.m[5] = 2.0f / h;
    p.m[8] = (tanR + tanL) / w;
    p.m[9] = (tanU + tanD) / h;
    p.m[10] = -(farZ + nearZ) / (farZ - nearZ);
    p.m[11] = -1.0f;
    p.m[14] = -(farZ * (nearZ + nearZ)) / (farZ - nearZ);
    return p;
}

// Rigid transform (quat + pos) → 4x4.
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

static const char* kVert =
    "#version 300 es\n"
    "layout(location=0) in vec3 aPos;\n"
    "layout(location=1) in vec2 aUV;\n"
    "uniform mat4 uViewProj;\n"
    "out vec2 vUV;\n"
    "void main() { vUV = aUV; gl_Position = uViewProj * vec4(aPos, 1.0); }\n";

// DOOM pixels arrive as little-endian 0x00RRGGBB words -> RGBA texture bytes
// are B,G,R,0, so swizzle back to R,G,B and force opaque alpha.
static const char* kFrag =
    "#version 300 es\n"
    "precision mediump float;\n"
    "uniform sampler2D uTex;\n"
    "in vec2 vUV;\n"
    "out vec4 frag;\n"
    "void main() { vec4 t = texture(uTex, vUV); frag = vec4(t.b, t.g, t.r, 1.0); }\n";

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

// ---------------------------------------------------------------------------
// Panel geometry: a horizontally-curved segment centered in front of the user
// ---------------------------------------------------------------------------

#define PANEL_RADIUS 3.0f      // metres from the user
#define PANEL_WIDTH 3.4f       // arc length, metres
#define PANEL_SEGMENTS 64
#define PANEL_Y_CENTER (-0.1f)

static bool build_panel(GlRenderer* r) {
    const float aspect = 400.0f / 640.0f;  // DOOMGENERIC_RESY / RESX
    const float arcAngle = PANEL_WIDTH / PANEL_RADIUS;
    const float height = PANEL_WIDTH * aspect;
    const int cols = PANEL_SEGMENTS + 1;

    float* verts = malloc(cols * 2 * 5 * sizeof(float));
    uint16_t* idx = malloc(PANEL_SEGMENTS * 6 * sizeof(uint16_t));

    for (int i = 0; i < cols; i++) {
        float t = (float)i / PANEL_SEGMENTS;            // 0..1 across the arc
        float theta = (t - 0.5f) * arcAngle;            // -a/2..+a/2
        float x = PANEL_RADIUS * sinf(theta);
        float z = -PANEL_RADIUS * cosf(theta);
        float u = t;
        // bottom vertex
        float* vb = &verts[(i * 2 + 0) * 5];
        vb[0] = x; vb[1] = PANEL_Y_CENTER - height * 0.5f; vb[2] = z;
        vb[3] = u; vb[4] = 1.0f;
        // top vertex
        float* vt = &verts[(i * 2 + 1) * 5];
        vt[0] = x; vt[1] = PANEL_Y_CENTER + height * 0.5f; vt[2] = z;
        vt[3] = u; vt[4] = 0.0f;
    }
    int ii = 0;
    for (int i = 0; i < PANEL_SEGMENTS; i++) {
        uint16_t b0 = (uint16_t)(i * 2), t0 = b0 + 1;
        uint16_t b1 = (uint16_t)(i * 2 + 2), t1 = b0 + 3;
        idx[ii++] = b0; idx[ii++] = b1; idx[ii++] = t0;
        idx[ii++] = t0; idx[ii++] = b1; idx[ii++] = t1;
    }
    r->panelIndexCount = ii;

    glGenVertexArrays(1, &r->panelVao);
    glBindVertexArray(r->panelVao);
    glGenBuffers(1, &r->panelVbo);
    glBindBuffer(GL_ARRAY_BUFFER, r->panelVbo);
    glBufferData(GL_ARRAY_BUFFER, cols * 2 * 5 * sizeof(float), verts,
                 GL_STATIC_DRAW);
    glGenBuffers(1, &r->panelIbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, r->panelIbo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, ii * sizeof(uint16_t), idx,
                 GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
                          (void*)(3 * sizeof(float)));
    glBindVertexArray(0);

    free(verts);
    free(idx);
    return true;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool glr_init(GlRenderer* r) {
    memset(r, 0, sizeof(*r));

    GLuint vs = compile_shader(GL_VERTEX_SHADER, kVert);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, kFrag);
    r->program = glCreateProgram();
    glAttachShader(r->program, vs);
    glAttachShader(r->program, fs);
    glLinkProgram(r->program);
    GLint ok = 0;
    glGetProgramiv(r->program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(r->program, sizeof(log), NULL, log);
        LOGE("program link failed: %s", log);
        return false;
    }
    glDeleteShader(vs);
    glDeleteShader(fs);

    glGenTextures(1, &r->doomTexture);
    glBindTexture(GL_TEXTURE_2D, r->doomTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, &r->fbo);
    return build_panel(r);
}

void glr_upload_frame(GlRenderer* r, const uint32_t* pixels, int w, int h) {
    glBindTexture(GL_TEXTURE_2D, r->doomTexture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, pixels);
}

void glr_draw_eye(GlRenderer* r, XrEngine* e, int eye) {
    uint32_t index = 0;
    GLuint tex = xr_acquire_eye_image(e, eye, &index);
    if (!tex) return;

    XrEyeSwapchain* sc = &e->eyeSwapchains[eye];
    XrView* view = &e->views[eye];

    Mat4 viewMat = mat4_invert_rigid(mat4_from_pose(view->pose));
    Mat4 projMat = mat4_projection(view->fov, 0.05f, 100.0f);
    Mat4 vp = mat4_mul(projMat, viewMat);

    glBindFramebuffer(GL_FRAMEBUFFER, r->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         tex, 0);

    glViewport(0, 0, sc->width, sc->height);
    glClearColor(0.02f, 0.02f, 0.03f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glUseProgram(r->program);
    glUniformMatrix4fv(glGetUniformLocation(r->program, "uViewProj"), 1,
                      GL_FALSE, vp.m);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, r->doomTexture);
    glUniform1i(glGetUniformLocation(r->program, "uTex"), 0);

    glBindVertexArray(r->panelVao);
    glDrawElements(GL_TRIANGLES, r->panelIndexCount, GL_UNSIGNED_SHORT, NULL);
    glBindVertexArray(0);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    e->projViews[eye].pose = view->pose;
    e->projViews[eye].fov = view->fov;

    xr_release_eye_image(e, eye);
}

void glr_shutdown(GlRenderer* r) {
    glDeleteFramebuffers(1, &r->fbo);
    glDeleteProgram(r->program);
    glDeleteTextures(1, &r->doomTexture);
    glDeleteBuffers(1, &r->panelVbo);
    glDeleteBuffers(1, &r->panelIbo);
    glDeleteVertexArrays(1, &r->panelVao);
}
