/*
 * renderer.h - OpenGL ES 3.1 renderer.
 *
 * Ownership split (the reason this module knows nothing about Wayland):
 * the platform module owns the EGLDisplay/EGLContext/EGLSurface and makes
 * the context current; the renderer owns everything *inside* GL - shader
 * programs loaded from the shaders/ directory, mesh VAOs, and the dynamic
 * rain buffer. renderer_draw() issues GL calls only; presenting the frame
 * (eglSwapBuffers, which is what actually attaches+commits the wl_buffer)
 * stays in the platform module.
 *
 * Target profile: an ES 3.1 context is requested by the platform with an
 * ES 3.0 fallback, and shaders are written as `#version 300 es`, because
 * every feature used (VAOs, uniform arrays, fwidth, dynamic streaming) is
 * core ES 3.0. Requesting 3.1 where available costs nothing and keeps the
 * door open to compute/instancing later.
 */
#ifndef GARAGE3D_RENDERER_H
#define GARAGE3D_RENDERER_H

#include "animation.h"
#include "scene.h"
#include <GLES3/gl31.h>
#include <stdbool.h>

typedef struct {
    GLuint program;
    GLint u_mvp, u_model, u_normal_matrix;
    GLint u_albedo, u_alpha, u_emissive, u_emissive_scale;
    GLint u_specular, u_shininess, u_wet, u_pattern, u_time;
    GLint u_camera_pos, u_dir_dir, u_dir_color, u_dir_intensity;
    GLint u_point_pos, u_point_color, u_point_intensity;
    GLint u_fog_color, u_fog_density;
} LitProgram;

typedef struct {
    GLuint program;
    GLint u_mvp, u_color, u_emissive_scale, u_halo;
} NeonProgram;

typedef struct {
    GLuint program;
    GLint u_mvp, u_time, u_density;
} CityProgram;

typedef struct {
    GLuint program;
    GLint u_mvp, u_color, u_time, u_seed;
} MistProgram;

typedef struct {
    GLuint program;
    GLint u_mvp, u_color;
} ParticleProgram;

typedef struct {
    LitProgram lit;
    NeonProgram neon;
    CityProgram city;
    MistProgram mist;
    ParticleProgram particle;
    GLuint mesh_vao[MESH_COUNT];
    GLuint mesh_vbo[MESH_COUNT];
    GLuint mesh_ibo[MESH_COUNT];
    GLuint rain_vao, rain_vbo;
    bool initialized;
} Renderer;

/* Compiles shaders (searched in shader_dir, $GARAGE3D_SHADER_DIR, next to
 * the executable, cwd, then system share dirs) and uploads meshes.
 * Requires a current GL context. */
bool renderer_init(Renderer *r, const char *shader_dir);
void renderer_draw(Renderer *r, const Scene *scene, const AnimationState *anim,
                   int viewport_w, int viewport_h, float time);
void renderer_shutdown(Renderer *r);
void renderer_log_info(void);

#endif
