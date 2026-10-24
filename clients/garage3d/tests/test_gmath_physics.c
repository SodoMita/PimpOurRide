/*
 * test_gmath_physics.c - headless unit tests for garage3d's pure modules.
 *
 * gmath, mesh, scene and animation contain no GL/Wayland calls by design,
 * so the scene graph and the *production* fixed-step integrator can be
 * verified on any CI box with nothing but a C compiler and libm:
 *
 *     make -C clients/garage3d test
 */
#include "animation.h"
#include "gmath.h"
#include "mesh.h"
#include "scene.h"
#include <math.h>
#include <stdio.h>

static int failures = 0;
#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);           \
            fprintf(stderr, __VA_ARGS__);                                  \
            fputc('\n', stderr);                                           \
            failures++;                                                    \
        }                                                                  \
    } while (0)

static bool approx(float a, float b, float eps) { return fabsf(a - b) <= eps; }

int main(void) {
    /* --- perspective: near plane maps to NDC z=-1, far to +1 (GLES clip) */
    Mat4 proj = mat4_perspective(60.0f, 16.0f / 9.0f, 0.1f, 100.0f);
    Vec3 near_pt = mat4_mul_point(proj, vec3(0, 0, -0.1f));
    Vec3 far_pt = mat4_mul_point(proj, vec3(0, 0, -100.0f));
    CHECK(approx(near_pt.z, -1.0f, 1e-3f), "near plane z=%f, want -1", near_pt.z);
    CHECK(approx(far_pt.z, 1.0f, 1e-3f), "far plane z=%f, want +1", far_pt.z);

    /* --- look_at: eye at origin looking down -Z is (near-)identity */
    Mat4 view = mat4_look_at(vec3(0, 0, 0), vec3(0, 0, -1), vec3(0, 1, 0));
    Vec3 ahead = mat4_mul_point(view, vec3(0, 0, -5));
    CHECK(approx(ahead.z, -5.0f, 1e-4f), "look_at ahead z=%f, want -5", ahead.z);

    /* --- quaternions: 90 degrees about Y sends +Z to +X */
    Mat4 rot = mat4_from_quat(quat_from_euler_deg(0, 90, 0));
    Vec3 turned = mat4_mul_point(rot, vec3(0, 0, 1));
    CHECK(approx(turned.x, 1.0f, 1e-4f) && approx(turned.z, 0.0f, 1e-4f),
          "yaw90 * +Z = (%f,%f,%f), want +X", turned.x, turned.y, turned.z);

    /* --- meshes: every index references a real vertex */
    for (int id = 0; id < MESH_COUNT; id++) {
        const MeshData *m = mesh_get((enum MeshId)id);
        size_t vertex_count = m->vertex_floats / MESH_VERTEX_STRIDE_FLOATS;
        CHECK(m->vertex_floats % MESH_VERTEX_STRIDE_FLOATS == 0,
              "mesh %d has ragged vertices", id);
        CHECK(m->index_count % 3 == 0, "mesh %d index count %zu not triangles",
              id, m->index_count);
        bool in_range = true;
        for (size_t i = 0; i < m->index_count; i++)
            if (m->indices[i] >= vertex_count) in_range = false;
        CHECK(in_range, "mesh %d has an out-of-range index", id);
    }

    /* --- scene graph: hierarchy resolves, parents precede children */
    Scene scene;
    scene_build(&scene, 62.0f);
    CHECK(scene.node_count > 80, "expected a substantial scene, got %d nodes",
          scene.node_count);
    bool ordering = true, finite = true;
    for (int i = 0; i < scene.node_count; i++) {
        if (scene.nodes[i].parent >= i) ordering = false;
        for (int k = 0; k < 16; k++)
            if (!isfinite(scene.nodes[i].world.m[k])) finite = false;
    }
    CHECK(ordering, "a node's parent does not precede it (world pass invalid)");
    CHECK(finite, "non-finite world matrix in freshly built scene");
    CHECK(scene.lamp_pivot >= 0 && scene.lamp_bulb >= 0 && scene.fan_pivot >= 0,
          "special animated nodes were not recorded");
    /* The lamp bulb hangs 3.32 m below a pivot at the ceiling (8.2 m). */
    Vec3 bulb = scene_node_world_position(&scene, scene.lamp_bulb);
    CHECK(bulb.y > 4.2f && bulb.y < 5.6f, "lamp bulb height %f looks wrong", bulb.y);
    /* Lamp point light must track the bulb (scene_update_world syncs it). */
    CHECK(approx(scene.points[LIGHT_LAMP].position.y, bulb.y, 1e-4f),
          "lamp light not synced to bulb");

    /* --- physics: 10 simulated seconds stay bounded and above the floor */
    AnimationState anim;
    anim_init(&anim, &scene);
    for (int step = 0; step < 1200; step++) {
        anim.time += 1.0 / PHYSICS_HZ;
        anim_physics_step(&anim, &scene, (float)(1.0 / PHYSICS_HZ));
        scene_update_world(&scene);
        for (int b = 0; b < scene.ball_count; b++) {
            BallBody *body = &anim.balls[b];
            CHECK(isfinite(body->position.x) && isfinite(body->position.y) &&
                  isfinite(body->position.z), "ball %d went non-finite", b);
            CHECK(body->position.y >= body->radius - 1e-3f,
                  "ball %d sank through the floor: y=%f r=%f",
                  b, body->position.y, body->radius);
            CHECK(fabsf(body->position.x) <= HALL_HALF_W,
                  "ball %d escaped sideways: x=%f", b, body->position.x);
        }
        CHECK(fabsf(anim.swing_x) < 1.2f && fabsf(anim.swing_z) < 1.2f,
              "pendulum diverged: (%f, %f)", anim.swing_x, anim.swing_z);
        for (int p = 0; p < anim.rain_count; p++)
            CHECK(anim.rain_pos[p].z < HALL_FAR_Z,
                  "rain particle %d leaked into the hall: z=%f", p, anim.rain_pos[p].z);
        if (failures > 20) break; /* don't flood stderr on a broken build */
    }

    /* --- frame-rate independence: 1 s of wall time integrated as one
     *     clamped 0.1 s frame must not explode the accumulator (spiral-of-
     *     death guard), and two half-second updates advance time by ~1 s. */
    AnimationState a2;
    anim_init(&a2, &scene);
    anim_update(&a2, &scene, 0.5f);
    anim_update(&a2, &scene, 0.5f);
    CHECK(approx((float)a2.time, 1.0f, 1e-4f), "anim time=%f, want 1.0", a2.time);
    CHECK(a2.accumulator < 1.0, "accumulator not drained: %f", a2.accumulator);
    CHECK(isfinite(a2.balls[0].position.y), "post-update ball is non-finite");

    if (failures) {
        fprintf(stderr, "garage3d tests: %d FAILURE(S)\n", failures);
        return 1;
    }
    printf("PASS: garage3d math (projection/look-at/quaternions), mesh index "
           "bounds, scene-graph hierarchy, 10 s of pendulum/ball/rain physics, "
           "fixed-step accumulator\n");
    return 0;
}
