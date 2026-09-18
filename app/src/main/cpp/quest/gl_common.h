// Platform-neutral GL/math bits shared by the Quest (OpenXR), Cardboard
// (phone sensors) and offscreen verification backends. No OpenXR or
// android_native_app_glue types appear here.
#pragma once

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <GLES3/gl3.h>
#include <GLES3/gl3ext.h>

#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "QuestDOOM"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#else
#include <stdio.h>
#define LOGI(...) do { fprintf(stderr, "[QD] " __VA_ARGS__); \
                       fputc('\n', stderr); } while (0)
#define LOGW(...) LOGI(__VA_ARGS__)
#define LOGE(...) do { fprintf(stderr, "[QD-ERR] " __VA_ARGS__); \
                       fputc('\n', stderr); } while (0)
#endif

// Column-major 4x4, same layout as GLSL/GL uniform matrices.
typedef struct { float m[16]; } GlMat4;

// Quaternion + translation pose in the backend's tracking space.
typedef struct {
    float qx, qy, qz, qw;
    float px, py, pz;
} GlPose;

static inline GlMat4 glmat_identity(void) {
    GlMat4 r = {{0}};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
}

static inline GlMat4 glmat_mul(GlMat4 a, GlMat4 b) {
    GlMat4 r = {{0}};
    for (int c = 0; c < 4; c++)
        for (int rw = 0; rw < 4; rw++)
            for (int k = 0; k < 4; k++)
                r.m[c * 4 + rw] += a.m[k * 4 + rw] * b.m[c * 4 + k];
    return r;
}

// Perspective projection from view-frustum tangents (tan of the half-angles).
static inline GlMat4 glmat_projection(float tanL, float tanR, float tanU,
                                      float tanD, float nearZ, float farZ) {
    float w = tanR - tanL, h = tanD - tanU;
    GlMat4 p = {{0}};
    p.m[0] = 2.0f / w;
    p.m[5] = 2.0f / h;
    p.m[8] = (tanR + tanL) / w;
    p.m[9] = (tanU + tanD) / h;
    p.m[10] = -(farZ + nearZ) / (farZ - nearZ);
    p.m[11] = -1.0f;
    p.m[14] = -(farZ * (nearZ + nearZ)) / (farZ - nearZ);
    return p;
}

// Rigid transform (quat + pos) -> 4x4 world matrix.
static inline GlMat4 glmat_pose(float qx, float qy, float qz, float qw,
                                float px, float py, float pz) {
    float x2 = qx + qx, y2 = qy + qy, z2 = qz + qz;
    float xx = qx * x2, xy = qx * y2, xz = qx * z2;
    float yy = qy * y2, yz = qy * z2, zz = qz * z2;
    float wx = qw * x2, wy = qw * y2, wz = qw * z2;

    GlMat4 r = glmat_identity();
    r.m[0] = 1 - (yy + zz); r.m[1] = xy + wz;       r.m[2] = xz - wy;
    r.m[4] = xy - wz;       r.m[5] = 1 - (xx + zz); r.m[6] = yz + wx;
    r.m[8] = xz + wy;       r.m[9] = yz - wx;       r.m[10] = 1 - (xx + yy);
    r.m[12] = px; r.m[13] = py; r.m[14] = pz;
    return r;
}

// Inverse of a rigid transform = transpose rotation, negate translation.
static inline GlMat4 glmat_invert_rigid(GlMat4 t) {
    GlMat4 r = glmat_identity();
    for (int c = 0; c < 3; c++)
        for (int rw = 0; rw < 3; rw++)
            r.m[c * 4 + rw] = t.m[rw * 4 + c];
    r.m[12] = -(t.m[0] * t.m[12] + t.m[1] * t.m[13] + t.m[2] * t.m[14]);
    r.m[13] = -(t.m[4] * t.m[12] + t.m[5] * t.m[13] + t.m[6] * t.m[14]);
    r.m[14] = -(t.m[8] * t.m[12] + t.m[9] * t.m[13] + t.m[10] * t.m[14]);
    return r;
}

static inline GlMat4 glmat_translate(float x, float y, float z) {
    GlMat4 r = glmat_identity();
    r.m[12] = x; r.m[13] = y; r.m[14] = z;
    return r;
}

// Roll around view Z (forward axis), degrees. Used by the Cardboard backend
// to compensate the landscape-mounted sensor frame.
static inline GlMat4 glmat_roll_z(float deg) {
    float rad = deg * (float)M_PI / 180.0f;
    float c = cosf(rad), s = sinf(rad);
    GlMat4 r = glmat_identity();
    r.m[0] = c;  r.m[1] = s;
    r.m[4] = -s; r.m[5] = c;
    return r;
}

// Quaternion -> yaw/pitch (forward = -Z), degrees.
static inline void glmat_quat_to_yawpitch(float qx, float qy, float qz,
                                          float qw, float* yawDeg,
                                          float* pitchDeg) {
    float fx = -2.0f * (qy * qw + qx * qz);
    float fy =  2.0f * (qx * qw - qy * qz);
    float fz = -1.0f + 2.0f * (qx * qx + qy * qy);
    *yawDeg = atan2f(-fx, -fz) * (180.0f / (float)M_PI);
    *pitchDeg = asinf(fy > 1.0f ? 1.0f : (fy < -1.0f ? -1.0f : fy))
                * (180.0f / (float)M_PI);
}
