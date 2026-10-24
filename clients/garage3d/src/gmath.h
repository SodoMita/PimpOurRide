/*
 * gmath.h - minimal 3D math for garage3d.
 *
 * Why a hand-rolled library instead of cglm/GLM? The brief asks for a
 * self-contained native client with no windowing/math abstraction
 * dependencies. Everything this renderer needs (Vec3, column-major Mat4,
 * quaternions for rolling bodies, perspective/look-at) fits in one small,
 * auditable module, and it can be unit-tested without a GL context
 * (see tests/test_gmath_physics.c).
 *
 * Conventions (documented once, used everywhere):
 *   - Right-handed world: +X right, +Y up, -Z forward (into the garage).
 *   - Mat4 is COLUMN-MAJOR, m[col*4 + row], exactly the layout
 *     glUniformMatrix4fv(..., GL_FALSE, m) expects. No transposes anywhere.
 *   - C = A * B means "apply B first, then A" (standard OpenGL order), so a
 *     child's world matrix is parent_world * child_local.
 *   - Angles in the public Euler helpers are DEGREES (matches the level
 *     layout numbers a human reads in scene.c); radians stay internal.
 */
#ifndef GARAGE3D_GMATH_H
#define GARAGE3D_GMATH_H

#include <stdbool.h>

typedef struct { float x, y, z; } Vec3;
typedef struct { float x, y, z, w; } Quat; /* w is the scalar part */
typedef struct { float m[16]; } Mat4;

Vec3 vec3(float x, float y, float z);
Vec3 vec3_add(Vec3 a, Vec3 b);
Vec3 vec3_sub(Vec3 a, Vec3 b);
Vec3 vec3_mul(Vec3 a, float k);
float vec3_dot(Vec3 a, Vec3 b);
Vec3 vec3_cross(Vec3 a, Vec3 b);
float vec3_length(Vec3 v);
/* Returns the zero vector for (near-)zero input instead of NaN: degenerate
 * directions happen transiently in physics (a ball at rest), and silently
 * producing NaNs there would poison the whole scene graph. */
Vec3 vec3_normalize(Vec3 v);
float clampf_(float v, float lo, float hi);
float lerpf(float a, float b, float t);

Quat quat_identity(void);
Quat quat_from_axis_angle(Vec3 axis, float radians);
Quat quat_from_euler_deg(float pitch_x, float yaw_y, float roll_z);
Quat quat_mul(Quat a, Quat b); /* apply b, then a */
Quat quat_normalize(Quat q);

Mat4 mat4_identity(void);
Mat4 mat4_mul(Mat4 a, Mat4 b); /* apply b, then a */
Mat4 mat4_translation(Vec3 t);
Mat4 mat4_scale(Vec3 s);
Mat4 mat4_from_quat(Quat q);
/* Local node transform T * R * S (scale first, then rotate, then move). */
Mat4 mat4_trs(Vec3 t, Quat r, Vec3 s);
/* OpenGL-style perspective: clip z in [-1, 1], as GLES expects. */
Mat4 mat4_perspective(float fov_deg, float aspect, float near_z, float far_z);
Mat4 mat4_look_at(Vec3 eye, Vec3 target, Vec3 up);
/* Translation column of a world matrix = the node's world position. */
Vec3 mat4_translation_of(Mat4 m);
/* Transform a point (w=1, with perspective divide) - used by tests. */
Vec3 mat4_mul_point(Mat4 m, Vec3 p);

#endif
