/*
 * scene.h - scene graph, camera and lights for the 3D garage.
 *
 * The scene is a real-time reinterpretation of assets/garage.webp, the flat
 * backdrop the ChromeDrive compositor used to draw as one textured quad
 * (src/scene.c, old environment()). The translation, element by element:
 *
 *   mockup (garage.webp)               this scene
 *   --------------------------------  ----------------------------------------
 *   concrete hangar, one-point view   22 m wide x 8.5 m x 46 m hall, camera
 *                                     on the centre line looking down -Z
 *   cyan neon tubes, left wall        8 emissive cylinders + halo quads,
 *                                     grouped under per-bay parent nodes
 *   magenta neon tubes, right wall    same, mirrored (split teal/magenta
 *                                     lighting comes from 4 cluster point
 *                                     lights, not a baked gradient)
 *   protruding concrete pillars       box pillars every 8 m, both walls
 *   coffered ceiling, beams, pipes    transverse/longitudinal beam boxes,
 *                                     central duct + side pipe cylinders
 *   wet reflective floor              dark glossy floor (alpha 0.86) over
 *                                     mirrored neon tubes - a cheap planar
 *                                     reflection that costs no extra FBO
 *                                     (see renderer.c draw-order comment)
 *   far opening to a rainy city       hole in the far wall, procedural city
 *                                     shader plane behind it, rain particle
 *                                     physics and drifting mist in between
 *
 * Scene graph: nodes live in one flat array; a parent always has a smaller
 * index than its children, so world matrices resolve in a single forward
 * pass (scene_update_world). Local transform is T * R(quat) * S. Children
 * inherit scale - scene.c is careful never to parent geometry under a
 * scaled node unless that inheritance is intended (see the lamp).
 */
#ifndef GARAGE3D_SCENE_H
#define GARAGE3D_SCENE_H

#include "config.h"
#include "gmath.h"
#include "input.h"
#include "mesh.h"
#include <stdbool.h>

/* Hall dimensions in metres. Physics (animation.c) reuses these bounds. */
#define HALL_HALF_W   11.0f
#define HALL_HEIGHT    8.5f
#define HALL_FAR_Z   (-30.0f)
#define HALL_NEAR_Z    15.0f

#define MAX_NODES 320

enum ProgramKind { PROG_LIT = 0, PROG_NEON, PROG_CITY, PROG_MIST };
enum DrawLayer {
    LAYER_OPAQUE = 0,     /* walls, pillars, city, props                  */
    LAYER_REFLECT = 1,    /* mirrored neon under the floor (before floor)  */
    LAYER_FLOOR = 2,      /* the semi-transparent wet floor itself         */
    LAYER_TRANSPARENT = 3 /* halos + mist, additive, depth-write off       */
};

typedef struct {
    Vec3 albedo;
    float alpha;
    Vec3 emissive;        /* neon/bulb colour (HDR values >1 are fine) */
    float specular;       /* Blinn-Phong strength */
    float shininess;      /* Blinn-Phong exponent */
    float wet;            /* 1.0 on the floor: tighter, rippled highlight */
} Material;

typedef struct {
    char name[32];
    int parent;           /* -1 = root; always < own index */
    int mesh;             /* enum MeshId, or -1 for a pure transform node */
    int program;          /* enum ProgramKind */
    int layer;            /* enum DrawLayer */
    Material material;
    Vec3 position;
    Quat rotation;
    Vec3 scale;
    Mat4 world;           /* written by scene_update_world() */
    float emissive_scale; /* animated (flicker/pulse), default 1 */
    float shader_param;   /* per-node seed/phase for mist & city */
    bool visible;
    /* Which settings colour (if any) drives this node's emissive. Set at
     * build for tubes/halos/reflections so a config reload can re-tint the
     * whole installation without rebuilding the graph. 0=none 1=left 2=right */
    unsigned char color_role;
} Node;

typedef struct {
    Vec3 target;          /* orbit centre */
    float yaw_deg, pitch_deg, distance;
    float fov_deg, near_z, far_z;
    /* Smoothed angular velocity (deg/s): gives drag a short glide instead of
     * a dead stop, integrated with the same exponential damping the rest of
     * the codebase uses (1 - exp(-k*dt)), which is frame-rate independent. */
    float yaw_vel, pitch_vel;
    float eff_yaw_deg, eff_pitch_deg; /* + idle attract offset, for the eye */
    Vec3 eye;             /* cached by camera_update() */
} Camera;

enum LightIndex {
    LIGHT_LAMP = 0,       /* dynamic: follows the swinging work lamp */
    LIGHT_TEAL_NEAR,
    LIGHT_MAGENTA_NEAR,
    LIGHT_TEAL_FAR,
    LIGHT_MAGENTA_FAR,
    LIGHT_CITY,           /* cool spill through the far opening */
    LIGHT_COUNT
};

typedef struct {
    Vec3 position;        /* world space */
    Vec3 color;
    float intensity;
} PointLight;

typedef struct {
    Node nodes[MAX_NODES];
    int node_count;
    Camera camera;

    /* One directional light (moon/city glow through the opening) plus a
     * fixed small set of point lights. 6 points is a deliberate mobile
     * budget: 16 physical tubes are represented by 4 cluster lights. */
    Vec3 dir_light_dir;   /* direction the light travels */
    Vec3 dir_light_color;
    float dir_light_intensity;
    PointLight points[LIGHT_COUNT];
    Vec3 fog_color;
    float fog_density;

    /* Well-known nodes the animation/physics system drives. */
    int lamp_pivot, lamp_bulb, fan_pivot;
    int balls[3], ball_count;
    int flicker_tube, flicker_halo; /* the one faulty tube, right wall */
    int mists[4], mist_count;

    /* The effective settings (config file + CLI, hot-reloadable). Owned by
     * the scene so animation/renderer read tunables without extra plumbing
     * parameters; main.c replaces the copy via scene_apply_settings(). */
    GarageSettings settings;
} Scene;

/* Builds the whole garage from `settings` (copied into the scene; the
 * camera FOV, neon colours, fog etc. all come from there). Pure CPU work:
 * no GL calls here. */
void scene_build(Scene *scene, const GarageSettings *settings);
/* Hot-swaps the scene's settings: re-tints neon nodes, re-colours the
 * cluster lights, updates fog/FOV/mist/ball visibility. Safe to call every
 * frame, but main.c calls it when the config file's mtime changes.
 * Edge-triggered consequences (rain count, pause state, pendulum energy,
 * fullscreen, scale) are main.c's job - they need old-vs-new diffing. */
void scene_apply_settings(Scene *scene, const GarageSettings *settings);
/* Resolves world matrices (parents precede children) and syncs the lamp
 * light to the bulb node's world position. Call after animation. */
void scene_update_world(Scene *scene);
Vec3 scene_node_world_position(const Scene *scene, int node);

void camera_reset(Scene *scene);
/* Consumes one frame of InputState. dt is real elapsed seconds (clamped by
 * the caller); every integration below is dt-scaled. */
void camera_update(Scene *scene, const InputState *in, float dt, double time);
void camera_matrices(const Scene *scene, float aspect, Mat4 *view, Mat4 *proj);

#endif
