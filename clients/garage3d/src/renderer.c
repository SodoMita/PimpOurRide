/* renderer.c - GLES shader/mesh management and scene traversal. */
#define _POSIX_C_SOURCE 200809L /* readlink() for the exe-relative shader path */
#include "renderer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ------------------------------------------------------------- shader IO */

/* Shader search order is part of the packaging story:
 *   1. explicit --shader-dir / $GARAGE3D_SHADER_DIR (tests, distros)
 *   2. next to the executable (installed layout: bin/../share/...)
 *   3. the development tree (binary in clients/garage3d/bin/, shaders in
 *      clients/garage3d/shaders/, or cwd = repo root / client dir)
 * The binary must work from any cwd, because a compositor launching it
 * (e.g. ChromeDrive's --exec) does not control our working directory. */
static char *read_shader(const char *override_dir, const char *name) {
    static char exe_dir[4096];
    if (!exe_dir[0]) {
        ssize_t n = readlink("/proc/self/exe", exe_dir, sizeof(exe_dir) - 1);
        if (n > 0) {
            exe_dir[n] = '\0';
            char *slash = strrchr(exe_dir, '/');
            if (slash) *slash = '\0'; else snprintf(exe_dir, sizeof(exe_dir), ".");
        } else {
            snprintf(exe_dir, sizeof(exe_dir), ".");
        }
    }
    const char *env_dir = getenv("GARAGE3D_SHADER_DIR");
    char candidates[9][4352];
    int count = 0;
    if (override_dir) snprintf(candidates[count++], 4352, "%s/%s", override_dir, name);
    if (env_dir) snprintf(candidates[count++], 4352, "%s/%s", env_dir, name);
    snprintf(candidates[count++], 4352, "%s/shaders/%s", exe_dir, name);
    snprintf(candidates[count++], 4352, "%s/../shaders/%s", exe_dir, name);
    snprintf(candidates[count++], 4352, "%s/../share/garage3d/shaders/%s", exe_dir, name);
    snprintf(candidates[count++], 4352, "shaders/%s", name);
    snprintf(candidates[count++], 4352, "clients/garage3d/shaders/%s", name);
    snprintf(candidates[count++], 4352, "/usr/local/share/garage3d/shaders/%s", name);
    snprintf(candidates[count++], 4352, "/usr/share/garage3d/shaders/%s", name);

    for (int i = 0; i < count; i++) {
        FILE *f = fopen(candidates[i], "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (size <= 0 || size > 1 << 20) { fclose(f); continue; }
        char *buf = malloc((size_t)size + 1);
        if (!buf) { fclose(f); return NULL; }
        size_t got = fread(buf, 1, (size_t)size, f);
        fclose(f);
        if (got != (size_t)size) { free(buf); continue; }
        buf[size] = '\0';
        return buf;
    }
    fprintf(stderr, "garage3d: shader '%s' not found (searched %d locations; "
                    "set GARAGE3D_SHADER_DIR or pass --shader-dir)\n", name, count);
    return NULL;
}

static GLuint compile_shader(GLenum type, const char *src, const char *name) {
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    GLint ok = GL_FALSE;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetShaderInfoLog(sh, sizeof(log), NULL, log);
        fprintf(stderr, "garage3d: shader %s failed to compile:\n%s\n", name, log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static GLuint build_program(const char *dir, const char *vert_name, const char *frag_name) {
    char *vs = read_shader(dir, vert_name);
    char *fs = vs ? read_shader(dir, frag_name) : NULL;
    if (!vs || !fs) { free(vs); free(fs); return 0; }
    GLuint v = compile_shader(GL_VERTEX_SHADER, vs, vert_name);
    GLuint f = v ? compile_shader(GL_FRAGMENT_SHADER, fs, frag_name) : 0;
    free(vs); free(fs);
    if (!v || !f) { if (v) glDeleteShader(v); if (f) glDeleteShader(f); return 0; }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, v);
    glAttachShader(prog, f);
    glLinkProgram(prog);
    /* Shaders can be deleted once linked; the program keeps the code. */
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = GL_FALSE;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetProgramInfoLog(prog, sizeof(log), NULL, log);
        fprintf(stderr, "garage3d: program %s/%s failed to link:\n%s\n", vert_name, frag_name, log);
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

static GLint loc(GLuint prog, const char *name) {
    GLint l = glGetUniformLocation(prog, name);
    if (l < 0)
        fprintf(stderr, "garage3d: warning: uniform '%s' not active (optimised out?)\n", name);
    return l;
}

/* -------------------------------------------------------------- lifecycle */

bool renderer_init(Renderer *r, const char *shader_dir) {
    memset(r, 0, sizeof(*r));

    r->lit.program = build_program(shader_dir, "lit.vert", "lit.frag");
    r->neon.program = build_program(shader_dir, "neon.vert", "neon.frag");
    r->city.program = build_program(shader_dir, "city.vert", "city.frag");
    r->mist.program = build_program(shader_dir, "mist.vert", "mist.frag");
    r->particle.program = build_program(shader_dir, "particle.vert", "particle.frag");
    if (!r->lit.program || !r->neon.program || !r->city.program ||
        !r->mist.program || !r->particle.program) {
        renderer_shutdown(r);
        return false;
    }

    LitProgram *l = &r->lit;
    l->u_mvp = loc(l->program, "u_mvp");
    l->u_model = loc(l->program, "u_model");
    l->u_normal_matrix = loc(l->program, "u_normal_matrix");
    l->u_albedo = loc(l->program, "u_albedo");
    l->u_alpha = loc(l->program, "u_alpha");
    l->u_emissive = loc(l->program, "u_emissive");
    l->u_emissive_scale = loc(l->program, "u_emissive_scale");
    l->u_specular = loc(l->program, "u_specular");
    l->u_shininess = loc(l->program, "u_shininess");
    l->u_wet = loc(l->program, "u_wet");
    l->u_pattern = loc(l->program, "u_pattern");
    l->u_time = loc(l->program, "u_time");
    l->u_camera_pos = loc(l->program, "u_camera_pos");
    l->u_dir_dir = loc(l->program, "u_dir_dir");
    l->u_dir_color = loc(l->program, "u_dir_color");
    l->u_dir_intensity = loc(l->program, "u_dir_intensity");
    /* Uniform arrays: the [0] location addresses the whole contiguous array. */
    l->u_point_pos = loc(l->program, "u_point_pos[0]");
    l->u_point_color = loc(l->program, "u_point_color[0]");
    l->u_point_intensity = loc(l->program, "u_point_intensity[0]");
    l->u_fog_color = loc(l->program, "u_fog_color");
    l->u_fog_density = loc(l->program, "u_fog_density");

    r->neon.u_mvp = loc(r->neon.program, "u_mvp");
    r->neon.u_color = loc(r->neon.program, "u_color");
    r->neon.u_emissive_scale = loc(r->neon.program, "u_emissive_scale");
    r->neon.u_halo = loc(r->neon.program, "u_halo");
    r->city.u_mvp = loc(r->city.program, "u_mvp");
    r->city.u_time = loc(r->city.program, "u_time");
    r->mist.u_mvp = loc(r->mist.program, "u_mvp");
    r->mist.u_color = loc(r->mist.program, "u_color");
    r->mist.u_time = loc(r->mist.program, "u_time");
    r->mist.u_seed = loc(r->mist.program, "u_seed");
    r->particle.u_mvp = loc(r->particle.program, "u_mvp");
    r->particle.u_color = loc(r->particle.program, "u_color");

    for (int i = 0; i < MESH_COUNT; i++) {
        const MeshData *m = mesh_get((enum MeshId)i);
        glGenVertexArrays(1, &r->mesh_vao[i]);
        glGenBuffers(1, &r->mesh_vbo[i]);
        glGenBuffers(1, &r->mesh_ibo[i]);
        glBindVertexArray(r->mesh_vao[i]);
        glBindBuffer(GL_ARRAY_BUFFER, r->mesh_vbo[i]);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(m->vertex_floats * sizeof(float)),
                     m->vertices, GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, r->mesh_ibo[i]);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(m->index_count * sizeof(unsigned short)),
                     m->indices, GL_STATIC_DRAW);
        const GLsizei stride = MESH_VERTEX_STRIDE_FLOATS * (GLsizei)sizeof(float);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (const void *)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (const void *)(3 * sizeof(float)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, (const void *)(6 * sizeof(float)));
    }
    glBindVertexArray(0);

    /* Rain: preallocated dynamic buffer, streamed with glBufferSubData each
     * frame. Allocating once (instead of glBufferData per frame) avoids
     * driver reallocations; 420 streaks = 840 verts is a tiny upload. */
    glGenVertexArrays(1, &r->rain_vao);
    glGenBuffers(1, &r->rain_vbo);
    glBindVertexArray(r->rain_vao);
    glBindBuffer(GL_ARRAY_BUFFER, r->rain_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(MAX_RAIN * 2 * 3 * sizeof(float)),
                 NULL, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * (GLsizei)sizeof(float), (const void *)0);
    glBindVertexArray(0);

    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        fprintf(stderr, "garage3d: GL error 0x%x during renderer init\n", err);
        renderer_shutdown(r);
        return false;
    }
    r->initialized = true;
    return true;
}

void renderer_shutdown(Renderer *r) {
    for (int i = 0; i < MESH_COUNT; i++) {
        if (r->mesh_vao[i]) glDeleteVertexArrays(1, &r->mesh_vao[i]);
        if (r->mesh_vbo[i]) glDeleteBuffers(1, &r->mesh_vbo[i]);
        if (r->mesh_ibo[i]) glDeleteBuffers(1, &r->mesh_ibo[i]);
    }
    if (r->rain_vao) glDeleteVertexArrays(1, &r->rain_vao);
    if (r->rain_vbo) glDeleteBuffers(1, &r->rain_vbo);
    const GLuint progs[] = { r->lit.program, r->neon.program, r->city.program,
                             r->mist.program, r->particle.program };
    for (size_t i = 0; i < sizeof(progs) / sizeof(progs[0]); i++)
        if (progs[i]) glDeleteProgram(progs[i]);
    memset(r, 0, sizeof(*r));
}

void renderer_log_info(void) {
    fprintf(stderr, "garage3d: GL_VERSION=%s\n", (const char *)glGetString(GL_VERSION));
    fprintf(stderr, "garage3d: GL_RENDERER=%s\n", (const char *)glGetString(GL_RENDERER));
}

/* ----------------------------------------------------------------- drawing */

/* Inverse-transpose of the model's upper 3x3 (= cofactor matrix / det). */
static void normal_matrix_of(Mat4 m, float out[9]) {
    float a00 = m.m[0], a01 = m.m[4], a02 = m.m[8];
    float a10 = m.m[1], a11 = m.m[5], a12 = m.m[9];
    float a20 = m.m[2], a21 = m.m[6], a22 = m.m[10];
    float c00 = (a11 * a22 - a12 * a21), c01 = -(a10 * a22 - a12 * a20), c02 = (a10 * a21 - a11 * a20);
    float c10 = -(a01 * a22 - a02 * a21), c11 = (a00 * a22 - a02 * a20), c12 = -(a00 * a21 - a01 * a20);
    float c20 = (a01 * a12 - a02 * a11), c21 = -(a00 * a12 - a02 * a10), c22 = (a00 * a11 - a01 * a10);
    float det = a00 * c00 + a01 * c01 + a02 * c02;
    float inv = (det > 1e-12f || det < -1e-12f) ? 1.0f / det : 1.0f;
    out[0] = c00 * inv; out[1] = c10 * inv; out[2] = c20 * inv;
    out[3] = c01 * inv; out[4] = c11 * inv; out[5] = c21 * inv;
    out[6] = c02 * inv; out[7] = c12 * inv; out[8] = c22 * inv;
}

static void draw_mesh(Renderer *r, int mesh_id) {
    if (mesh_id < 0 || mesh_id >= MESH_COUNT) return;
    glBindVertexArray(r->mesh_vao[mesh_id]);
    glDrawElements(GL_TRIANGLES, (GLsizei)mesh_get((enum MeshId)mesh_id)->index_count,
                   GL_UNSIGNED_SHORT, (const void *)0);
}

void renderer_draw(Renderer *r, const Scene *scene, const AnimationState *anim,
                   int viewport_w, int viewport_h, float time) {
    if (!r->initialized || viewport_w <= 0 || viewport_h <= 0) return;
    glViewport(0, 0, viewport_w, viewport_h);
    /* Clear colour = fog colour so anything beyond the far wall melts into
     * the same violet instead of a hard black edge. */
    glClearColor(scene->fog_color.x, scene->fog_color.y, scene->fog_color.z, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    /* No face culling: walls/ceiling/halos are single-sided planes seen from
     * both sides. The scene is ~150 draws; the culling saving is irrelevant
     * next to the correctness simplicity (lit.frag flips back-face normals). */
    glDisable(GL_CULL_FACE);

    Mat4 view, proj;
    camera_matrices(scene, (float)viewport_w / (float)viewport_h, &view, &proj);
    Mat4 vp = mat4_mul(proj, view);

    /* Draw-order contract (see DrawLayer in scene.h):
     *   0 opaque  - writes depth, establishes occlusion
     *   1 reflect - mirrored neon UNDER the floor, additive, no depth write
     *   2 floor   - alpha-blended over the reflections => wet concrete look
     *               without a planar-reflection FBO render pass
     *   3 transp. - halos/mist, additive, no depth write (never occlude)
     * Rain streaks go last so they shine over the city and mist. */
    int current_program_kind = -1;
    for (int layer = 0; layer <= LAYER_TRANSPARENT; layer++) {
        if (layer == LAYER_OPAQUE) {
            glDisable(GL_BLEND);
            glDepthMask(GL_TRUE);
        } else if (layer == LAYER_REFLECT) {
            glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_ONE);
            glDepthMask(GL_FALSE);
        } else if (layer == LAYER_FLOOR) {
            glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_TRUE);
        } else {
            glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_ONE);
            glDepthMask(GL_FALSE);
        }

        for (int i = 0; i < scene->node_count; i++) {
            const Node *n = &scene->nodes[i];
            if (!n->visible || n->mesh < 0 || n->layer != layer) continue;
            Mat4 mvp = mat4_mul(vp, n->world);

            if (n->program == PROG_LIT) {
                if (current_program_kind != PROG_LIT) {
                    glUseProgram(r->lit.program);
                    current_program_kind = PROG_LIT;
                    /* Per-frame (not per-node) uniforms only on the switch. */
                    glUniform3f(r->lit.u_camera_pos, scene->camera.eye.x,
                                scene->camera.eye.y, scene->camera.eye.z);
                    glUniform3f(r->lit.u_dir_dir, scene->dir_light_dir.x,
                                scene->dir_light_dir.y, scene->dir_light_dir.z);
                    glUniform3f(r->lit.u_dir_color, scene->dir_light_color.x,
                                scene->dir_light_color.y, scene->dir_light_color.z);
                    glUniform1f(r->lit.u_dir_intensity, scene->dir_light_intensity);
                    float pos[LIGHT_COUNT * 3], col[LIGHT_COUNT * 3], inten[LIGHT_COUNT];
                    for (int k = 0; k < LIGHT_COUNT; k++) {
                        pos[k * 3] = scene->points[k].position.x;
                        pos[k * 3 + 1] = scene->points[k].position.y;
                        pos[k * 3 + 2] = scene->points[k].position.z;
                        col[k * 3] = scene->points[k].color.x;
                        col[k * 3 + 1] = scene->points[k].color.y;
                        col[k * 3 + 2] = scene->points[k].color.z;
                        inten[k] = scene->points[k].intensity;
                    }
                    glUniform3fv(r->lit.u_point_pos, LIGHT_COUNT, pos);
                    glUniform3fv(r->lit.u_point_color, LIGHT_COUNT, col);
                    glUniform1fv(r->lit.u_point_intensity, LIGHT_COUNT, inten);
                    glUniform3f(r->lit.u_fog_color, scene->fog_color.x,
                                scene->fog_color.y, scene->fog_color.z);
                    glUniform1f(r->lit.u_fog_density, scene->fog_density);
                    glUniform1f(r->lit.u_time, time);
                }
                float nm[9];
                normal_matrix_of(n->world, nm);
                glUniformMatrix4fv(r->lit.u_mvp, 1, GL_FALSE, mvp.m);
                glUniformMatrix4fv(r->lit.u_model, 1, GL_FALSE, n->world.m);
                glUniformMatrix3fv(r->lit.u_normal_matrix, 1, GL_FALSE, nm);
                glUniform3f(r->lit.u_albedo, n->material.albedo.x,
                            n->material.albedo.y, n->material.albedo.z);
                glUniform1f(r->lit.u_alpha, n->material.alpha);
                glUniform3f(r->lit.u_emissive, n->material.emissive.x,
                            n->material.emissive.y, n->material.emissive.z);
                glUniform1f(r->lit.u_emissive_scale, n->emissive_scale);
                glUniform1f(r->lit.u_specular, n->material.specular);
                glUniform1f(r->lit.u_shininess, n->material.shininess);
                glUniform1f(r->lit.u_wet, n->material.wet);
                glUniform1f(r->lit.u_pattern, n->mesh == MESH_SPHERE ? 1.0f : 0.0f);
                draw_mesh(r, n->mesh);
            } else if (n->program == PROG_NEON) {
                if (current_program_kind != PROG_NEON) {
                    glUseProgram(r->neon.program);
                    current_program_kind = PROG_NEON;
                }
                glUniformMatrix4fv(r->neon.u_mvp, 1, GL_FALSE, mvp.m);
                glUniform3f(r->neon.u_color, n->material.emissive.x,
                            n->material.emissive.y, n->material.emissive.z);
                glUniform1f(r->neon.u_emissive_scale, n->emissive_scale);
                glUniform1f(r->neon.u_halo, n->shader_param);
                draw_mesh(r, n->mesh);
            } else if (n->program == PROG_CITY) {
                if (current_program_kind != PROG_CITY) {
                    glUseProgram(r->city.program);
                    current_program_kind = PROG_CITY;
                    glUniform1f(r->city.u_time, time);
                }
                glUniformMatrix4fv(r->city.u_mvp, 1, GL_FALSE, mvp.m);
                draw_mesh(r, n->mesh);
            } else if (n->program == PROG_MIST) {
                if (current_program_kind != PROG_MIST) {
                    glUseProgram(r->mist.program);
                    current_program_kind = PROG_MIST;
                    glUniform1f(r->mist.u_time, time);
                }
                glUniformMatrix4fv(r->mist.u_mvp, 1, GL_FALSE, mvp.m);
                glUniform3f(r->mist.u_color, n->material.albedo.x,
                            n->material.albedo.y, n->material.albedo.z);
                glUniform1f(r->mist.u_seed, n->shader_param);
                draw_mesh(r, n->mesh);
            }
        }
    }

    /* Rain streaks from the physics state (lines head->tail along -vel). */
    if (anim && anim->rain_count > 0) {
        static float verts[MAX_RAIN * 2 * 3];
        int count = anim->rain_count < MAX_RAIN ? anim->rain_count : MAX_RAIN;
        for (int i = 0; i < count; i++) {
            Vec3 p = anim->rain_pos[i], v = anim->rain_vel[i];
            float *d = &verts[i * 6];
            d[0] = p.x; d[1] = p.y; d[2] = p.z;
            d[3] = p.x - v.x * 0.032f; d[4] = p.y - v.y * 0.032f; d[5] = p.z - v.z * 0.032f;
        }
        glUseProgram(r->particle.program);
        glUniformMatrix4fv(r->particle.u_mvp, 1, GL_FALSE, vp.m);
        glUniform4f(r->particle.u_color, 0.55f, 0.70f, 1.0f, 0.22f);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);
        glDepthMask(GL_FALSE);
        glBindVertexArray(r->rain_vao);
        glBindBuffer(GL_ARRAY_BUFFER, r->rain_vbo);
        glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)(count * 6 * sizeof(float)), verts);
        glDrawArrays(GL_LINES, 0, count * 2);
    }

    /* Leave GL in a neutral state for the next frame / other users. */
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glBindVertexArray(0);
    glUseProgram(0);
}
