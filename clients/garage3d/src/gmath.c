/* gmath.c - see gmath.h for conventions. */
#include "gmath.h"
#include <math.h>
#include <string.h>

#define G_PI 3.14159265358979323846f
#define DEG2RAD (G_PI / 180.0f)

Vec3 vec3(float x, float y, float z) { Vec3 v = { x, y, z }; return v; }
Vec3 vec3_add(Vec3 a, Vec3 b) { return vec3(a.x + b.x, a.y + b.y, a.z + b.z); }
Vec3 vec3_sub(Vec3 a, Vec3 b) { return vec3(a.x - b.x, a.y - b.y, a.z - b.z); }
Vec3 vec3_mul(Vec3 a, float k) { return vec3(a.x * k, a.y * k, a.z * k); }
float vec3_dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 vec3_cross(Vec3 a, Vec3 b) {
    return vec3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
float vec3_length(Vec3 v) { return sqrtf(vec3_dot(v, v)); }
Vec3 vec3_normalize(Vec3 v) {
    float n = vec3_length(v);
    return n > 1e-9f ? vec3_mul(v, 1.0f / n) : vec3(0, 0, 0);
}
float clampf_(float v, float lo, float hi) { return fminf(hi, fmaxf(lo, v)); }
float lerpf(float a, float b, float t) { return a + (b - a) * t; }

Quat quat_identity(void) { Quat q = { 0, 0, 0, 1 }; return q; }

Quat quat_from_axis_angle(Vec3 axis, float radians) {
    Vec3 a = vec3_normalize(axis);
    if (vec3_length(a) < 0.5f) return quat_identity(); /* degenerate axis */
    float s = sinf(radians * 0.5f);
    Quat q = { a.x * s, a.y * s, a.z * s, cosf(radians * 0.5f) };
    return q;
}

Quat quat_normalize(Quat q) {
    float n = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (n < 1e-9f) return quat_identity();
    float inv = 1.0f / n;
    Quat r = { q.x * inv, q.y * inv, q.z * inv, q.w * inv };
    return r;
}

Quat quat_mul(Quat a, Quat b) {
    Quat r;
    r.w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z;
    r.x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y;
    r.y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x;
    r.z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w;
    return r;
}

Quat quat_from_euler_deg(float pitch_x, float yaw_y, float roll_z) {
    /* Intrinsic Y * X * Z order: yaw about world Y, then pitch, then roll.
     * That matches how scene.c authors think about wall panels and pipes. */
    Quat qy = quat_from_axis_angle(vec3(0, 1, 0), yaw_y * DEG2RAD);
    Quat qx = quat_from_axis_angle(vec3(1, 0, 0), pitch_x * DEG2RAD);
    Quat qz = quat_from_axis_angle(vec3(0, 0, 1), roll_z * DEG2RAD);
    return quat_normalize(quat_mul(qy, quat_mul(qx, qz)));
}

Mat4 mat4_identity(void) {
    Mat4 m; memset(&m, 0, sizeof(m));
    m.m[0] = m.m[5] = m.m[10] = m.m[15] = 1.0f;
    return m;
}

Mat4 mat4_mul(Mat4 a, Mat4 b) {
    Mat4 r; memset(&r, 0, sizeof(r));
    for (int col = 0; col < 4; col++)
        for (int row = 0; row < 4; row++) {
            float s = 0.0f;
            for (int k = 0; k < 4; k++) s += a.m[k * 4 + row] * b.m[col * 4 + k];
            r.m[col * 4 + row] = s;
        }
    return r;
}

Mat4 mat4_translation(Vec3 t) {
    Mat4 m = mat4_identity();
    m.m[12] = t.x; m.m[13] = t.y; m.m[14] = t.z;
    return m;
}

Mat4 mat4_scale(Vec3 s) {
    Mat4 m = mat4_identity();
    m.m[0] = s.x; m.m[5] = s.y; m.m[10] = s.z;
    return m;
}

Mat4 mat4_from_quat(Quat q_) {
    Quat q = quat_normalize(q_);
    float x = q.x, y = q.y, z = q.z, w = q.w;
    Mat4 m = mat4_identity();
    /* Column-major rotation matrix for a unit quaternion. */
    m.m[0] = 1 - 2 * (y * y + z * z); m.m[1] = 2 * (x * y + z * w);     m.m[2] = 2 * (x * z - y * w);
    m.m[4] = 2 * (x * y - z * w);     m.m[5] = 1 - 2 * (x * x + z * z); m.m[6] = 2 * (y * z + x * w);
    m.m[8] = 2 * (x * z + y * w);     m.m[9] = 2 * (y * z - x * w);     m.m[10] = 1 - 2 * (x * x + y * y);
    return m;
}

Mat4 mat4_trs(Vec3 t, Quat r, Vec3 s) {
    return mat4_mul(mat4_translation(t), mat4_mul(mat4_from_quat(r), mat4_scale(s)));
}

Mat4 mat4_perspective(float fov_deg, float aspect, float near_z, float far_z) {
    /* Guard against a 0-height surface during configure churn: an infinite
     * aspect here would poison every vertex for one frame. */
    if (aspect <= 0.0f || !isfinite(aspect)) aspect = 1.0f;
    float f = 1.0f / tanf(clampf_(fov_deg, 1.0f, 179.0f) * DEG2RAD * 0.5f);
    Mat4 m; memset(&m, 0, sizeof(m));
    m.m[0] = f / aspect;
    m.m[5] = f;
    m.m[10] = (far_z + near_z) / (near_z - far_z);
    m.m[11] = -1.0f;
    m.m[14] = (2.0f * far_z * near_z) / (near_z - far_z);
    return m;
}

Mat4 mat4_look_at(Vec3 eye, Vec3 target, Vec3 up) {
    Vec3 fwd = vec3_normalize(vec3_sub(target, eye));
    if (vec3_length(fwd) < 0.5f) fwd = vec3(0, 0, -1); /* eye == target */
    Vec3 right = vec3_normalize(vec3_cross(fwd, up));
    if (vec3_length(right) < 0.5f) right = vec3(1, 0, 0); /* looking straight up/down */
    Vec3 true_up = vec3_cross(right, fwd);
    Mat4 m = mat4_identity();
    /* View matrix = R^T * T^-1, written out directly in column-major. */
    m.m[0] = right.x;   m.m[4] = right.y;   m.m[8] = right.z;
    m.m[1] = true_up.x; m.m[5] = true_up.y; m.m[9] = true_up.z;
    m.m[2] = -fwd.x;    m.m[6] = -fwd.y;    m.m[10] = -fwd.z;
    m.m[12] = -vec3_dot(right, eye);
    m.m[13] = -vec3_dot(true_up, eye);
    m.m[14] = vec3_dot(fwd, eye);
    return m;
}

Vec3 mat4_translation_of(Mat4 m) { return vec3(m.m[12], m.m[13], m.m[14]); }

Vec3 mat4_mul_point(Mat4 m, Vec3 p) {
    float x = m.m[0] * p.x + m.m[4] * p.y + m.m[8] * p.z + m.m[12];
    float y = m.m[1] * p.x + m.m[5] * p.y + m.m[9] * p.z + m.m[13];
    float z = m.m[2] * p.x + m.m[6] * p.y + m.m[10] * p.z + m.m[14];
    float w = m.m[3] * p.x + m.m[7] * p.y + m.m[11] * p.z + m.m[15];
    if (fabsf(w) > 1e-9f && w != 1.0f) { x /= w; y /= w; z /= w; }
    return vec3(x, y, z);
}
