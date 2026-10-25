/* scene.c - builds the garage graph. See scene.h for the mockup mapping. */
#include "scene.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define DEG2RAD_F (3.14159265358979323846f / 180.0f)

static Material mat(Vec3 albedo, float alpha, Vec3 emissive,
                    float specular, float shininess, float wet) {
    Material m = { albedo, alpha, emissive, specular, shininess, wet };
    return m;
}

/* Shared material palette. Colours are linear-ish values tuned against
 * garage.webp: blue-grey concrete, near-black wet floor, split neon. */
static Material MAT_CONCRETE, MAT_PILLAR, MAT_CEILING, MAT_FLOOR, MAT_METAL,
    MAT_METAL_DARK, MAT_TEAL, MAT_MAGENTA, MAT_BULB, MAT_CRATE, MAT_BARREL,
    MAT_CITY, MAT_MIST, MAT_BALL_A, MAT_BALL_B, MAT_BALL_C;

static Vec3 settings_color(const float rgb[3], float gain) {
    return vec3(rgb[0] * gain, rgb[1] * gain, rgb[2] * gain);
}

static void init_materials(const GarageSettings *g) {
    Vec3 black = vec3(0, 0, 0);
    MAT_CONCRETE = mat(vec3(0.15f, 0.17f, 0.19f), 1, black, 0.25f, 24, 0);
    MAT_PILLAR   = mat(vec3(0.12f, 0.13f, 0.15f), 1, black, 0.20f, 20, 0);
    MAT_CEILING  = mat(vec3(0.09f, 0.09f, 0.11f), 1, black, 0.15f, 16, 0);
    /* Alpha < 1 is load-bearing: it is what lets the mirrored neon tubes
     * under the floor show through as a wet-concrete reflection. */
    MAT_FLOOR    = mat(vec3(0.045f, 0.055f, 0.075f), 0.86f, black, 1.3f, 90, 1);
    MAT_METAL    = mat(vec3(0.27f, 0.31f, 0.37f), 1, black, 0.9f, 70, 0);
    MAT_METAL_DARK = mat(vec3(0.07f, 0.08f, 0.10f), 1, black, 0.5f, 40, 0);
    /* Neon emissives come from settings (base colour x HDR gain) so the
     * config GUI can re-tint the installation; with defaults these are the
     * original (0.25, 2.4, 2.2) / (2.3, 0.35, 2.7). */
    MAT_TEAL     = mat(black, 1, settings_color(g->neon_left, g->neon_intensity), 0, 1, 0);
    MAT_MAGENTA  = mat(black, 1, settings_color(g->neon_right, g->neon_intensity), 0, 1, 0);
    MAT_BULB     = mat(black, 1, vec3(2.6f, 2.3f, 1.7f), 0, 1, 0);
    MAT_CRATE    = mat(vec3(0.16f, 0.13f, 0.09f), 1, black, 0.15f, 12, 0);
    MAT_BARREL   = mat(vec3(0.10f, 0.16f, 0.20f), 1, black, 0.6f, 48, 0);
    MAT_CITY     = mat(vec3(1, 1, 1), 1, black, 0, 1, 0); /* shader-driven */
    MAT_MIST     = mat(vec3(0.55f, 0.45f, 0.85f), 1, black, 0, 1, 0);
    MAT_BALL_A   = mat(vec3(0.55f, 0.60f, 0.68f), 1, black, 1.2f, 110, 0);
    MAT_BALL_B   = mat(vec3(0.35f, 0.15f, 0.55f), 1, black, 1.0f, 80, 0);
    MAT_BALL_C   = mat(vec3(0.10f, 0.45f, 0.50f), 1, black, 1.0f, 80, 0);
}

static int add_node(Scene *s, const char *name, int parent, int mesh,
                    int program, int layer, Material material,
                    Vec3 pos, float pitch, float yaw, float roll, Vec3 scale) {
    if (s->node_count >= MAX_NODES) {
        fprintf(stderr, "garage3d: scene graph full, dropping '%s'\n", name);
        return -1;
    }
    int i = s->node_count++;
    Node *n = &s->nodes[i];
    memset(n, 0, sizeof(*n));
    snprintf(n->name, sizeof(n->name), "%s", name);
    n->parent = parent; n->mesh = mesh; n->program = program; n->layer = layer;
    n->material = material; n->position = pos;
    n->rotation = quat_from_euler_deg(pitch, yaw, roll);
    n->scale = scale; n->world = mat4_identity();
    n->emissive_scale = 1.0f; n->visible = true;
    return i;
}

void scene_build(Scene *s, const GarageSettings *settings) {
    memset(s, 0, sizeof(*s));
    s->settings = *settings;
    settings_clamp(&s->settings);
    s->lamp_pivot = s->lamp_bulb = s->fan_pivot = -1;
    s->flicker_tube = s->flicker_halo = -1;
    init_materials(&s->settings);

    const float W = HALL_HALF_W, H = HALL_HEIGHT, ZF = HALL_FAR_Z;
    const float len = HALL_NEAR_Z - ZF;          /* 45 m of hall */
    const float zc = (HALL_NEAR_Z + ZF) * 0.5f;  /* hall centre z */

    /* ---- shell: floor, ceiling, side walls ------------------------------ */
    add_node(s, "floor", -1, MESH_PLANE_XZ, PROG_LIT, LAYER_FLOOR, MAT_FLOOR,
             vec3(0, 0, zc), 0, 0, 0, vec3(2 * W, 1, len));
    add_node(s, "exterior ground", -1, MESH_PLANE_XZ, PROG_LIT, LAYER_OPAQUE, MAT_PILLAR,
             vec3(0, -0.02f, ZF - 9), 0, 0, 0, vec3(60, 1, 20));
    /* Ceiling plane flipped so its normal faces down into the hall. */
    add_node(s, "ceiling", -1, MESH_PLANE_XZ, PROG_LIT, LAYER_OPAQUE, MAT_CEILING,
             vec3(0, H, zc), 180, 0, 0, vec3(2 * W, 1, len));
    add_node(s, "wall left", -1, MESH_PLANE_XY, PROG_LIT, LAYER_OPAQUE, MAT_CONCRETE,
             vec3(-W, H * 0.5f, zc), 0, 90, 0, vec3(len, H, 1));
    add_node(s, "wall right", -1, MESH_PLANE_XY, PROG_LIT, LAYER_OPAQUE, MAT_CONCRETE,
             vec3(W, H * 0.5f, zc), 0, -90, 0, vec3(len, H, 1));
    /* Rear wall behind the default camera: orbiting 180 degrees must find a
     * sealed hall, not the fog-coloured void beyond the floor's edge. */
    add_node(s, "rear wall", -1, MESH_PLANE_XY, PROG_LIT, LAYER_OPAQUE, MAT_CONCRETE,
             vec3(0, H * 0.5f, HALL_NEAR_Z), 0, 180, 0, vec3(2 * W, H, 1));

    /* Far wall with a rectangular opening (the mockup's city doorway):
     * left/right segments + a lintel above the hole. No CSG needed. */
    const float hole_hw = 5.5f, hole_h = 5.2f;
    float seg_w = W - hole_hw;
    add_node(s, "far wall L", -1, MESH_PLANE_XY, PROG_LIT, LAYER_OPAQUE, MAT_CONCRETE,
             vec3(-(hole_hw + seg_w * 0.5f), H * 0.5f, ZF), 0, 0, 0, vec3(seg_w, H, 1));
    add_node(s, "far wall R", -1, MESH_PLANE_XY, PROG_LIT, LAYER_OPAQUE, MAT_CONCRETE,
             vec3(hole_hw + seg_w * 0.5f, H * 0.5f, ZF), 0, 0, 0, vec3(seg_w, H, 1));
    add_node(s, "far lintel", -1, MESH_PLANE_XY, PROG_LIT, LAYER_OPAQUE, MAT_CONCRETE,
             vec3(0, (hole_h + H) * 0.5f, ZF), 0, 0, 0, vec3(2 * hole_hw, H - hole_h, 1));
    /* Roller-shutter housing above the opening, as in the mockup. */
    add_node(s, "shutter box", -1, MESH_BOX, PROG_LIT, LAYER_OPAQUE, MAT_METAL_DARK,
             vec3(0, hole_h + 0.35f, ZF + 0.35f), 0, 0, 0, vec3(2 * hole_hw + 0.6f, 0.7f, 0.7f));
    add_node(s, "shutter slat", -1, MESH_BOX, PROG_LIT, LAYER_OPAQUE, MAT_METAL,
             vec3(0, hole_h - 0.12f, ZF + 0.15f), 0, 0, 0, vec3(2 * hole_hw, 0.3f, 0.12f));

    /* ---- structure: pillars, beams, duct, pipes -------------------------- */
    static const float bay_z[] = { 4.0f, -4.0f, -12.0f, -20.0f };
    static const float pillar_z[] = { 8.0f, 0.0f, -8.0f, -16.0f, -24.0f };
    for (int i = 0; i < 5; i++) {
        for (int side = -1; side <= 1; side += 2) {
            char name[32]; snprintf(name, sizeof(name), "pillar %c%d", side < 0 ? 'L' : 'R', i);
            add_node(s, name, -1, MESH_BOX, PROG_LIT, LAYER_OPAQUE, MAT_PILLAR,
                     vec3(side * (W - 0.35f), H * 0.5f, pillar_z[i]), 0, 0, 0, vec3(1.3f, H, 1.1f));
        }
        char name[32]; snprintf(name, sizeof(name), "beam %d", i);
        add_node(s, name, -1, MESH_BOX, PROG_LIT, LAYER_OPAQUE, MAT_CEILING,
                 vec3(0, H - 0.45f, pillar_z[i]), 0, 0, 0, vec3(2 * W, 0.9f, 0.8f));
    }
    for (int side = -1; side <= 1; side += 2)
        add_node(s, "longitudinal beam", -1, MESH_BOX, PROG_LIT, LAYER_OPAQUE, MAT_CEILING,
                 vec3(side * 5.5f, H - 0.4f, zc), 0, 0, 0, vec3(0.7f, 0.7f, len));
    add_node(s, "central duct", -1, MESH_BOX, PROG_LIT, LAYER_OPAQUE, MAT_METAL_DARK,
             vec3(0, H - 0.55f, zc), 0, 0, 0, vec3(2.2f, 0.55f, len));
    for (int i = 0; i < 8; i++) /* duct vents */
        add_node(s, "vent", -1, MESH_BOX, PROG_LIT, LAYER_OPAQUE, MAT_METAL,
                 vec3(0, H - 0.86f, 9.0f - i * 5.0f), 0, 0, 0, vec3(1.7f, 0.08f, 1.1f));
    /* Pipes run along the upper right wall in the mockup; one on the left. */
    add_node(s, "pipe R1", -1, MESH_CYLINDER, PROG_LIT, LAYER_OPAQUE, MAT_METAL,
             vec3(W - 0.55f, H - 0.9f, zc), 90, 0, 0, vec3(0.24f, len, 0.24f));
    add_node(s, "pipe R2", -1, MESH_CYLINDER, PROG_LIT, LAYER_OPAQUE, MAT_METAL_DARK,
             vec3(W - 0.55f, H - 1.25f, zc), 90, 0, 0, vec3(0.16f, len, 0.16f));
    add_node(s, "pipe L1", -1, MESH_CYLINDER, PROG_LIT, LAYER_OPAQUE, MAT_METAL,
             vec3(-(W - 0.55f), H - 0.9f, zc), 90, 0, 0, vec3(0.24f, len, 0.24f));

    /* ---- neon tubes: per-bay parents, tube+halo children -----------------
     * The bay node carries the wall orientation; children only offset along
     * its local X (which is world Z on the walls). This is the hierarchy
     * doing real work - move a bay and its tubes, halos and (separately
     * authored) reflections' source positions move conceptually with it. */
    for (int bay = 0; bay < 4; bay++) {
        for (int side = -1; side <= 1; side += 2) {
            bool left = side < 0;
            Material neon = left ? MAT_TEAL : MAT_MAGENTA;
            char name[32]; snprintf(name, sizeof(name), "bay %c%d", left ? 'L' : 'R', bay);
            int parent = add_node(s, name, -1, -1, PROG_LIT, LAYER_OPAQUE, MAT_CONCRETE,
                                  vec3(side * (W - 0.12f), 3.5f, bay_z[bay]),
                                  0, left ? 90 : -90, 0, vec3(1, 1, 1));
            for (int t = -1; t <= 1; t += 2) {
                int tube = add_node(s, left ? "tube teal" : "tube magenta", parent,
                                    MESH_CYLINDER, PROG_NEON, LAYER_TRANSPARENT, neon,
                                    vec3(t * 1.15f, 0, 0.06f), 0, 0, 0, vec3(0.07f, 3.4f, 0.07f));
                int halo = add_node(s, "tube halo", parent, MESH_PLANE_XY, PROG_NEON,
                                    LAYER_TRANSPARENT, neon,
                                    vec3(t * 1.15f, 0, 0.03f), 0, 0, 0, vec3(0.9f, 4.0f, 1));
                s->nodes[halo].shader_param = 1.0f; /* halo mode in neon.frag */
                if (!left && bay == 0 && t < 0) { s->flicker_tube = tube; s->flicker_halo = halo; }
                /* Mirrored tube under the floor = the wet reflection. */
                float wz = bay_z[bay] + (left ? -t : t) * 1.15f;
                int refl = add_node(s, "tube reflection", -1, MESH_CYLINDER, PROG_NEON, LAYER_REFLECT,
                         neon, vec3(side * (W - 0.18f), -3.5f, wz), 0, 0, 0,
                         vec3(0.10f, 3.4f, 0.10f));
                /* Tag everything this settings colour drives (see color_role). */
                unsigned char role = left ? 1 : 2;
                s->nodes[tube].color_role = role;
                s->nodes[halo].color_role = role;
                if (refl >= 0) s->nodes[refl].color_role = role;
            }
        }
    }

    /* ---- hanging work lamp (physics pendulum lives in animation.c) -------
     * pivot -> cable / head / bulb are siblings under the pivot on purpose:
     * parenting the bulb under the cable would make it inherit the cable's
     * Y scale of 3 and stretch into an egg. Hierarchy != free lunch. */
    s->lamp_pivot = add_node(s, "lamp pivot", -1, -1, PROG_LIT, LAYER_OPAQUE, MAT_METAL,
                             vec3(0, H - 0.3f, -6.0f), 0, 0, 0, vec3(1, 1, 1));
    add_node(s, "lamp cable", s->lamp_pivot, MESH_CYLINDER, PROG_LIT, LAYER_OPAQUE,
             MAT_METAL_DARK, vec3(0, -1.5f, 0), 0, 0, 0, vec3(0.045f, 3.0f, 0.045f));
    add_node(s, "lamp shade", s->lamp_pivot, MESH_CYLINDER, PROG_LIT, LAYER_OPAQUE,
             MAT_METAL, vec3(0, -3.05f, 0), 0, 0, 0, vec3(0.55f, 0.4f, 0.55f));
    s->lamp_bulb = add_node(s, "lamp bulb", s->lamp_pivot, MESH_SPHERE, PROG_NEON,
                            LAYER_TRANSPARENT, MAT_BULB, vec3(0, -3.32f, 0), 0, 0, 0,
                            vec3(0.34f, 0.34f, 0.34f));

    /* ---- ceiling fan (procedural spin) ----------------------------------- */
    s->fan_pivot = add_node(s, "fan pivot", -1, -1, PROG_LIT, LAYER_OPAQUE, MAT_METAL,
                            vec3(0, H - 1.05f, -15.0f), 0, 0, 0, vec3(1, 1, 1));
    add_node(s, "fan drop rod", -1, MESH_CYLINDER, PROG_LIT, LAYER_OPAQUE, MAT_METAL_DARK,
             vec3(0, H - 0.5f, -15.0f), 0, 0, 0, vec3(0.09f, 0.9f, 0.09f));
    add_node(s, "fan hub", s->fan_pivot, MESH_CYLINDER, PROG_LIT, LAYER_OPAQUE, MAT_METAL,
             vec3(0, 0, 0), 0, 0, 0, vec3(0.5f, 0.28f, 0.5f));
    for (int i = 0; i < 3; i++) {
        float a = i * 2.0943951f; /* 120 degrees */
        add_node(s, "fan blade", s->fan_pivot, MESH_BOX, PROG_LIT, LAYER_OPAQUE, MAT_METAL_DARK,
                 vec3(cosf(a) * 1.05f, 0, sinf(a) * 1.05f), 0, -a / DEG2RAD_F, 0,
                 vec3(1.8f, 0.05f, 0.4f));
    }

    /* ---- props: keep the centre aisle empty, as in the mockup ------------ */
    int pallet = add_node(s, "crate pallet", -1, -1, PROG_LIT, LAYER_OPAQUE, MAT_CRATE,
                          vec3(8.7f, 0, -18.0f), 0, 12, 0, vec3(1, 1, 1));
    add_node(s, "crate", pallet, MESH_BOX, PROG_LIT, LAYER_OPAQUE, MAT_CRATE,
             vec3(0, 0.55f, 0), 0, 0, 0, vec3(1.4f, 1.1f, 1.4f));
    add_node(s, "crate", pallet, MESH_BOX, PROG_LIT, LAYER_OPAQUE, MAT_CRATE,
             vec3(0.15f, 1.55f, -0.1f), 0, 18, 0, vec3(1.05f, 0.9f, 1.05f));
    add_node(s, "crate", pallet, MESH_BOX, PROG_LIT, LAYER_OPAQUE, MAT_CRATE,
             vec3(-0.1f, 2.35f, 0.05f), 0, -9, 0, vec3(0.75f, 0.7f, 0.75f));
    int barrels = add_node(s, "barrel group", -1, -1, PROG_LIT, LAYER_OPAQUE, MAT_BARREL,
                           vec3(-8.9f, 0, -21.0f), 0, 0, 0, vec3(1, 1, 1));
    add_node(s, "barrel", barrels, MESH_CYLINDER, PROG_LIT, LAYER_OPAQUE, MAT_BARREL,
             vec3(0, 0.55f, 0), 0, 0, 0, vec3(0.85f, 1.1f, 0.85f));
    add_node(s, "barrel", barrels, MESH_CYLINDER, PROG_LIT, LAYER_OPAQUE, MAT_BARREL,
             vec3(1.0f, 0.55f, 0.35f), 0, 0, 0, vec3(0.85f, 1.1f, 0.85f));
    add_node(s, "barrel tipped", barrels, MESH_CYLINDER, PROG_LIT, LAYER_OPAQUE, MAT_BARREL,
             vec3(0.45f, 0.43f, 1.25f), 90, 24, 0, vec3(0.85f, 1.1f, 0.85f));

    /* Physics spheres (initial transforms overwritten by animation.c). */
    const Material *ball_mats[3] = { &MAT_BALL_A, &MAT_BALL_B, &MAT_BALL_C };
    s->ball_count = 3;
    for (int i = 0; i < 3; i++) {
        char name[32]; snprintf(name, sizeof(name), "ball %d", i);
        s->balls[i] = add_node(s, name, -1, MESH_SPHERE, PROG_LIT, LAYER_OPAQUE,
                               *ball_mats[i], vec3(0, 1 + i, -6 - 3 * i), 0, 0, 0,
                               vec3(0.7f, 0.7f, 0.7f));
    }

    /* ---- exterior: procedural city + mist -------------------------------- */
    add_node(s, "city", -1, MESH_PLANE_XY, PROG_CITY, LAYER_OPAQUE, MAT_CITY,
             vec3(0, 4.5f, ZF - 20.0f), 0, 0, 0, vec3(38, 15, 1));
    static const float mist_cfg[4][4] = { /* x, y, z, width */
        { 0.0f, 1.1f, -29.2f, 11.0f }, { -1.5f, 1.7f, -27.8f, 8.0f },
        { 1.8f, 0.8f, -31.5f, 12.0f }, { 0.0f, 2.3f, -33.5f, 14.0f },
    };
    s->mist_count = 4;
    for (int i = 0; i < 4; i++) {
        s->mists[i] = add_node(s, "mist", -1, MESH_PLANE_XY, PROG_MIST, LAYER_TRANSPARENT,
                               MAT_MIST, vec3(mist_cfg[i][0], mist_cfg[i][1], mist_cfg[i][2]),
                               0, 0, 0, vec3(mist_cfg[i][3], 2.6f, 1));
        s->nodes[s->mists[i]].shader_param = (float)i * 1.7f;
    }

    /* ---- lights & fog ------------------------------------------------------
     * Directional: cool blue "city night" light travelling +Z into the hall
     * through the opening, slightly downwards. Points: 4 neon clusters
     * (attenuated), the moving lamp, and a cool spill just inside the door. */
    s->dir_light_dir = vec3_normalize(vec3(0.08f, -0.25f, 1.0f));
    s->dir_light_color = vec3(0.30f, 0.46f, 0.80f);
    s->dir_light_intensity = 0.35f;
    /* Intensities are in the attenuation curve's units (see lit.frag):
     * ~7 gives a hot splash on the light's own wall (d~2 m), a gentle tint
     * mid-hall (d~10 m) and almost nothing on the far wall - the split
     * teal/magenta look. (An earlier 55-90 tuning blew the whole hall to
     * white; there is no tone mapping in this renderer to hide behind.) */
    s->points[LIGHT_LAMP] = (PointLight){ vec3(0, 5, -6), vec3(1.0f, 0.90f, 0.66f), 6.0f };
    s->points[LIGHT_TEAL_NEAR]    = (PointLight){ vec3(-9.2f, 3.5f, 2.0f), vec3(0.10f, 0.85f, 0.80f), 7.0f };
    s->points[LIGHT_MAGENTA_NEAR] = (PointLight){ vec3(9.2f, 3.5f, 2.0f), vec3(0.85f, 0.15f, 1.0f), 7.0f };
    s->points[LIGHT_TEAL_FAR]     = (PointLight){ vec3(-9.2f, 3.5f, -16.0f), vec3(0.10f, 0.85f, 0.80f), 5.5f };
    s->points[LIGHT_MAGENTA_FAR]  = (PointLight){ vec3(9.2f, 3.5f, -16.0f), vec3(0.85f, 0.15f, 1.0f), 5.5f };
    s->points[LIGHT_CITY]         = (PointLight){ vec3(0, 3.0f, ZF + 2.0f), vec3(0.35f, 0.50f, 0.95f), 10.0f };
    s->fog_color = vec3(0.045f, 0.045f, 0.10f);

    s->camera.near_z = 0.1f;
    s->camera.far_z = 220.0f;
    /* Fog density, camera FOV, light colours, mist, ball visibility all
     * land via the same path a live config reload takes. */
    scene_apply_settings(s, &s->settings);
    camera_reset(s);
    scene_update_world(s);
}

void scene_apply_settings(Scene *s, const GarageSettings *settings) {
    float prev_cfg_fov = s->settings.fov;
    s->settings = *settings;
    settings_clamp(&s->settings);
    const GarageSettings *g = &s->settings;

    /* Re-tint every neon-driven node, and the cluster lights that fake
     * their wall splash, so emissive tubes and lighting stay in sync. */
    Vec3 e_left = settings_color(g->neon_left, g->neon_intensity);
    Vec3 e_right = settings_color(g->neon_right, g->neon_intensity);
    for (int i = 0; i < s->node_count; i++) {
        if (s->nodes[i].color_role == 1) s->nodes[i].material.emissive = e_left;
        else if (s->nodes[i].color_role == 2) s->nodes[i].material.emissive = e_right;
    }
    Vec3 c_left = vec3(g->neon_left[0], g->neon_left[1], g->neon_left[2]);
    Vec3 c_right = vec3(g->neon_right[0], g->neon_right[1], g->neon_right[2]);
    s->points[LIGHT_TEAL_NEAR].color = c_left;
    s->points[LIGHT_TEAL_FAR].color = c_left;
    s->points[LIGHT_MAGENTA_NEAR].color = c_right;
    s->points[LIGHT_MAGENTA_FAR].color = c_right;
    /* (Point *intensities* stay as authored; animation.c modulates the
     * flicker light per frame, and the GUI's light_intensity multiplies
     * everything at upload time in the renderer.) */

    s->fog_density = g->fog_density;
    /* Only overwrite the live camera FOV when camera.fov in settings actually
     * changed (or on initial scene_build when fov_deg is 0), so reloading
     * unrelated settings never resets interactive +/- FOV adjustments. */
    if (s->camera.fov_deg <= 0.0f || fabsf(g->fov - prev_cfg_fov) > 1e-4f)
        s->camera.fov_deg = g->fov;
    for (int i = 0; i < s->mist_count; i++)
        s->nodes[s->mists[i]].material.albedo =
            vec3(0.55f * g->mist, 0.45f * g->mist, 0.85f * g->mist);
    for (int i = 0; i < s->ball_count; i++)
        s->nodes[s->balls[i]].visible = g->balls;
}

void scene_update_world(Scene *s) {
    for (int i = 0; i < s->node_count; i++) {
        Node *n = &s->nodes[i];
        Mat4 local = mat4_trs(n->position, n->rotation, n->scale);
        n->world = n->parent >= 0 ? mat4_mul(s->nodes[n->parent].world, local) : local;
    }
    /* The lamp point light is parented to the bulb by data, not by graph:
     * read the bulb's world translation back out of its matrix. */
    if (s->lamp_bulb >= 0)
        s->points[LIGHT_LAMP].position = scene_node_world_position(s, s->lamp_bulb);
}

Vec3 scene_node_world_position(const Scene *s, int node) {
    if (node < 0 || node >= s->node_count) return vec3(0, 0, 0);
    return mat4_translation_of(s->nodes[node].world);
}

/* ------------------------------------------------------------------ camera */

void camera_reset(Scene *s) {
    Camera *c = &s->camera;
    float fov = c->fov_deg > 0 ? c->fov_deg : 62.0f;
    /* The default framing is chosen to match garage.webp: dead centre,
     * eye height ~2.8 m equivalent, looking straight down the aisle to
     * the city opening with teal left / magenta right. */
    c->target = vec3(0, 2.5f, -9.0f);
    c->yaw_deg = 0.0f;
    c->pitch_deg = 1.5f;
    c->distance = 19.0f;
    c->fov_deg = fov;
    c->yaw_vel = c->pitch_vel = 0.0f;
    c->eff_yaw_deg = c->yaw_deg;
    c->eff_pitch_deg = c->pitch_deg;
    c->eye = vec3(0, 2.9f, 10.0f);
}

void camera_update(Scene *s, const InputState *in, float dt, double time) {
    Camera *c = &s->camera;
    if (dt <= 0.0f) dt = 1.0f / 240.0f;
    if (in->reset_camera) camera_reset(s);

    /* Orbit with short inertial glide. The track-to-target factor
     * 1-exp(-k*dt) is the frame-rate independent form of exponential
     * smoothing: at 30 fps and 120 fps the glide lasts the same seconds. */
    const float orbit_sens = 0.22f * s->settings.orbit_speed; /* deg per logical pixel */
    float want_yaw_vel = in->orbit_dx * orbit_sens / dt;
    float want_pitch_vel = in->orbit_dy * orbit_sens / dt;
    float blend = 1.0f - expf(-18.0f * dt);
    c->yaw_vel = lerpf(c->yaw_vel, want_yaw_vel, blend);
    c->pitch_vel = lerpf(c->pitch_vel, want_pitch_vel, blend);
    c->yaw_deg += c->yaw_vel * dt;
    c->pitch_deg = clampf_(c->pitch_deg + c->pitch_vel * dt, -55.0f, 70.0f);

    /* Zoom: exponential so a wheel click / pinch ratio scales distance
     * multiplicatively (equal ratios feel equal at any distance). */
    c->distance = clampf_(c->distance * expf(in->zoom_delta * 0.10f * s->settings.zoom_speed), 3.5f, 30.0f);
    c->fov_deg = clampf_(c->fov_deg + in->fov_delta * dt * 28.0f, 30.0f, 90.0f);

    float yaw = c->yaw_deg * DEG2RAD_F;
    Vec3 fwd = vec3(-sinf(yaw), 0, -cosf(yaw));
    Vec3 right = vec3(cosf(yaw), 0, -sinf(yaw));
    /* Pan: convert pixels to metres at the target's depth. */
    float pan_scale = c->distance * 0.0016f * s->settings.pan_speed;
    c->target = vec3_add(c->target, vec3_mul(right, -in->pan_dx * pan_scale));
    c->target.y += in->pan_dy * pan_scale;
    /* Held-key locomotion (state-based, dt-scaled). */
    const float speed = s->settings.move_speed;
    c->target = vec3_add(c->target, vec3_mul(fwd, in->move_forward * speed * dt));
    c->target = vec3_add(c->target, vec3_mul(right, in->move_strafe * speed * dt));
    c->target.y += in->move_vertical * speed * dt;
    c->target.x = clampf_(c->target.x, -HALL_HALF_W + 0.5f, HALL_HALF_W - 0.5f);
    c->target.y = clampf_(c->target.y, 0.3f, HALL_HEIGHT - 0.3f);
    c->target.z = clampf_(c->target.z, HALL_FAR_Z + 1.0f, HALL_NEAR_Z - 0.5f);

    /* Idle attract: after 6 s without input, breathe the view a few degrees
     * so a kiosk/demo screen never looks frozen. Applied as a display-only
     * offset - the user's own yaw/pitch are untouched, so the first input
     * event doesn't cause a jump. */
    double idle = time - in->last_activity;
    float drift_yaw = 0.0f, drift_pitch = 0.0f;
    if (s->settings.attract && idle > s->settings.attract_delay) {
        float t = (float)(idle - s->settings.attract_delay);
        float fade = fminf(t / 2.0f, 1.0f);
        drift_yaw = sinf(t * 0.21f) * 3.5f * fade;
        drift_pitch = sinf(t * 0.16f + 1.3f) * 1.6f * fade;
    }
    c->eff_yaw_deg = c->yaw_deg + drift_yaw;
    c->eff_pitch_deg = clampf_(c->pitch_deg + drift_pitch, -55.0f, 70.0f);

    float ey = c->eff_yaw_deg * DEG2RAD_F, ep = c->eff_pitch_deg * DEG2RAD_F;
    Vec3 offset = vec3(sinf(ey) * cosf(ep), sinf(ep), cosf(ey) * cosf(ep));
    c->eye = vec3_add(c->target, vec3_mul(offset, c->distance));
    /* Cheap collision: the eye may never leave the hall box. Orbiting wide
     * at long distance slides along the walls instead of clipping through. */
    c->eye.x = clampf_(c->eye.x, -HALL_HALF_W + 0.5f, HALL_HALF_W - 0.5f);
    c->eye.y = clampf_(c->eye.y, 0.35f, HALL_HEIGHT - 0.3f);
    c->eye.z = clampf_(c->eye.z, HALL_FAR_Z + 0.6f, HALL_NEAR_Z - 0.3f);
}

void camera_matrices(const Scene *s, float aspect, Mat4 *view, Mat4 *proj) {
    *view = mat4_look_at(s->camera.eye, s->camera.target, vec3(0, 1, 0));
    *proj = mat4_perspective(s->camera.fov_deg, aspect,
                             s->camera.near_z, s->camera.far_z);
}
