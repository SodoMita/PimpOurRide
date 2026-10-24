/* animation.c - procedural tracks + fixed-step physics. See animation.h. */
#include "animation.h"
#include <math.h>
#include <string.h>

#define GRAVITY 9.81f
#define PENDULUM_L 3.0f
#define PENDULUM_DAMPING 0.06f

static unsigned rng_next(AnimationState *a) {
    /* xorshift32: tiny, deterministic (screenshots/tests reproduce), and
     * good enough for flicker/rain respawn - this is not cryptography. */
    unsigned x = a->rng_state ? a->rng_state : 0x2545u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    a->rng_state = x;
    return x;
}
static float rng_float(AnimationState *a) { /* [0,1) */
    return (rng_next(a) >> 8) * (1.0f / 16777216.0f);
}

static void rain_respawn(AnimationState *a, int i, bool anywhere) {
    /* Volume just outside the opening: x spans the doorway plus a margin,
     * z from the far wall out towards the city plane. */
    a->rain_pos[i] = vec3(-13.0f + rng_float(a) * 26.0f,
                          anywhere ? rng_float(a) * 9.0f : 8.0f + rng_float(a) * 2.0f,
                          HALL_FAR_Z - 1.0f - rng_float(a) * 16.0f);
    /* Terminal-velocity-ish fall with a slight wind slant (+x, towards +z
     * into the doorway's light spill), matched to the streak renderer. */
    a->rain_vel[i] = vec3(0.7f + rng_float(a) * 0.5f,
                          -(7.5f + rng_float(a) * 3.5f),
                          0.9f + rng_float(a) * 0.6f);
}

void anim_init(AnimationState *a, const Scene *scene) {
    memset(a, 0, sizeof(*a));
    a->rng_state = 0x9E3779B9u;
    a->flicker = 1.0f;
    /* Lamp starts displaced at the configured amplitude, like someone just
     * brushed past it (default 18 deg = the original 0.32 rad). */
    float amp = scene->settings.lamp_swing * 0.0174532925f;
    a->swing_x = amp; a->swing_z = amp * 0.65625f;
    a->next_kick = 9.0;
    static const struct { Vec3 p, v; float r; } seed[3] = {
        { { -2.5f, 2.6f, -5.0f }, { 1.6f, 0.0f, -2.2f }, 0.35f },
        { { 2.8f, 1.4f, -10.0f }, { -2.0f, 0.0f, 1.4f }, 0.28f },
        { { 0.6f, 3.4f, -16.0f }, { 0.9f, 0.0f, 2.6f }, 0.22f },
    };
    for (int i = 0; i < 3 && i < scene->ball_count; i++) {
        a->balls[i].position = seed[i].p;
        a->balls[i].velocity = seed[i].v;
        a->balls[i].radius = seed[i].r;
        a->balls[i].restitution = 0.72f;
        a->balls[i].orientation = quat_identity();
    }
    a->rain_count = scene->settings.rain < MAX_RAIN ? scene->settings.rain : MAX_RAIN;
    for (int i = 0; i < a->rain_count; i++) rain_respawn(a, i, true);
}

void anim_set_rain_count(AnimationState *a, int count) {
    if (count < 0) count = 0;
    if (count > MAX_RAIN) count = MAX_RAIN;
    /* Newly activated streaks must not all fall in a sheet from y=0: seed
     * them through the whole volume, same as at init. */
    for (int i = a->rain_count; i < count; i++) rain_respawn(a, i, true);
    a->rain_count = count;
}

void anim_rescale_swing(AnimationState *a, float ratio) {
    if (ratio < 0.0f) ratio = 0.0f;
    a->swing_x *= ratio; a->swing_z *= ratio;
    a->swing_vx *= ratio; a->swing_vz *= ratio;
}

void anim_physics_step(AnimationState *a, Scene *scene, float h) {
    /* --- pendulum (semi-implicit Euler: update v from a, then x from new
     *     v; this ordering is symplectic, so energy doesn't blow up the way
     *     naive explicit Euler would over thousands of swings) --- */
    const float g = GRAVITY * scene->settings.gravity;
    if (a->time >= a->next_kick) {
        /* Occasional nudge keeps the lamp alive forever without violating
         * physics the rest of the time; direction is pseudo-random. The
         * nudge scales with the configured amplitude, so lamp_swing = 0
         * lets the lamp settle to a dead hang and stay there. */
        float kick = 0.55f * (scene->settings.lamp_swing / 18.0f);
        a->swing_vx += (rng_float(a) - 0.5f) * kick;
        a->swing_vz += (rng_float(a) - 0.5f) * kick;
        a->next_kick = a->time + 8.0 + rng_float(a) * 5.0;
    }
    float ax = -(g / PENDULUM_L) * sinf(a->swing_x) - PENDULUM_DAMPING * a->swing_vx;
    float az = -(g / PENDULUM_L) * sinf(a->swing_z) - PENDULUM_DAMPING * a->swing_vz;
    a->swing_vx += ax * h; a->swing_vz += az * h;
    a->swing_x += a->swing_vx * h; a->swing_z += a->swing_vz * h;
    if (scene->lamp_pivot >= 0) {
        Node *pivot = &scene->nodes[scene->lamp_pivot];
        const float rad2deg = 57.2957795f;
        pivot->rotation = quat_from_euler_deg(a->swing_x * rad2deg, 0, a->swing_z * rad2deg);
    }

    /* --- balls: gravity, floor bounce with restitution, wall bounds,
     *     rolling rotation from horizontal velocity (axis = up x v) --- */
    for (int i = 0; i < scene->ball_count && i < 3; i++) {
        BallBody *b = &a->balls[i];
        b->velocity.y -= g * h;
        b->position = vec3_add(b->position, vec3_mul(b->velocity, h));
        if (b->position.y < b->radius) {
            b->position.y = b->radius;
            if (fabsf(b->velocity.y) > 0.85f)
                b->velocity.y = -b->velocity.y * b->restitution;
            else
                b->velocity.y = 0.0f; /* settled: stop micro-bouncing */
            /* Rolling friction only while in floor contact. */
            b->velocity.x *= (1.0f - 0.25f * h);
            b->velocity.z *= (1.0f - 0.25f * h);
        }
        float xlim = HALL_HALF_W - 0.8f - b->radius;
        if (b->position.x < -xlim) { b->position.x = -xlim; b->velocity.x = fabsf(b->velocity.x) * b->restitution; }
        if (b->position.x > xlim)  { b->position.x = xlim;  b->velocity.x = -fabsf(b->velocity.x) * b->restitution; }
        if (b->position.z < HALL_FAR_Z + 1.0f + b->radius) {
            b->position.z = HALL_FAR_Z + 1.0f + b->radius;
            b->velocity.z = fabsf(b->velocity.z) * b->restitution;
        }
        if (b->position.z > HALL_NEAR_Z - 1.5f - b->radius) {
            b->position.z = HALL_NEAR_Z - 1.5f - b->radius;
            b->velocity.z = -fabsf(b->velocity.z) * b->restitution;
        }
        /* Keep settled balls interesting: if one fully stops, relaunch it
         * from above the aisle (an unseen "service" toss) rather than
         * leaving a dead prop for the rest of the session. */
        if (b->position.y <= b->radius + 1e-4f && vec3_length(b->velocity) < 0.05f) {
            b->position = vec3((rng_float(a) - 0.5f) * 6.0f, 2.5f + rng_float(a) * 2.0f,
                               -6.0f - rng_float(a) * 10.0f);
            b->velocity = vec3((rng_float(a) - 0.5f) * 3.5f, 0, (rng_float(a) - 0.5f) * 3.5f);
        }
        Vec3 hv = vec3(b->velocity.x, 0, b->velocity.z);
        float hs = vec3_length(hv);
        if (hs > 1e-4f && b->position.y <= b->radius + 0.01f) {
            Vec3 axis = vec3_normalize(vec3_cross(vec3(0, 1, 0), hv));
            Quat dq = quat_from_axis_angle(axis, hs * h / b->radius);
            b->orientation = quat_normalize(quat_mul(dq, b->orientation));
        }
        int node = scene->balls[i];
        if (node >= 0) {
            scene->nodes[node].position = b->position;
            scene->nodes[node].rotation = b->orientation;
            float d = b->radius * 2.0f;
            scene->nodes[node].scale = vec3(d, d, d);
        }
    }

    /* --- rain: constant-velocity fall (already at terminal velocity in
     *     the seed), respawn at the top on ground/exit. 420 particles is
     *     trivial CPU work but reads as weather through the doorway. --- */
    for (int i = 0; i < a->rain_count; i++) {
        a->rain_pos[i] = vec3_add(a->rain_pos[i], vec3_mul(a->rain_vel[i], h));
        if (a->rain_pos[i].y < 0.0f || a->rain_pos[i].z > HALL_FAR_Z - 0.3f)
            rain_respawn(a, i, false);
    }
}

void anim_update(AnimationState *a, Scene *scene, float dt) {
    if (a->paused || dt <= 0.0f) return;
    a->time += dt;

    /* Fixed-step accumulator. The step cap is a safety valve: after a long
     * stall we simulate at most 24 ticks (0.2 s) and drop the rest rather
     * than entering the classic "spiral of death". */
    a->accumulator += dt;
    const float h = (float)(1.0 / PHYSICS_HZ);
    int steps = 0;
    while (a->accumulator >= h && steps < 24) {
        anim_physics_step(a, scene, h);
        a->accumulator -= h;
        steps++;
    }
    if (a->accumulator > h * 24) a->accumulator = 0.0;

    const float t = (float)a->time;

    /* Procedural: fan integrates a constant angular speed (dt-based, so
     * its rpm is identical at any presentation rate). */
    a->fan_angle_deg += 140.0f * scene->settings.fan_speed * dt;
    if (scene->fan_pivot >= 0)
        scene->nodes[scene->fan_pivot].rotation =
            quat_from_euler_deg(0, a->fan_angle_deg, 0);

    /* Procedural: healthy neon breathes very slightly; the one faulty tube
     * drops out in 12 Hz buckets with a hashed duty cycle, and its cluster
     * point light follows so the wall splash flickers with the tube. */
    float pulse = 0.96f + 0.04f * sinf(t * 7.0f);
    unsigned bucket = (unsigned)(t * 12.0f);
    unsigned hash = bucket * 2654435761u >> 24; /* 0..255 */
    /* flicker=false in settings "repairs" the faulty tube: it breathes
     * with the healthy ones and its cluster light goes steady. */
    a->flicker = scene->settings.flicker
        ? (hash < 26 ? 0.12f : (hash < 44 ? 0.45f : 1.0f)) : 1.0f;
    for (int i = 0; i < scene->node_count; i++) {
        Node *n = &scene->nodes[i];
        if (n->program != PROG_NEON) continue;
        bool faulty = scene->settings.flicker &&
                      (i == scene->flicker_tube || i == scene->flicker_halo);
        n->emissive_scale = faulty ? a->flicker : pulse;
        if (n->layer == LAYER_REFLECT)
            n->emissive_scale *= 0.55f * scene->settings.reflections;
    }
    scene->points[LIGHT_MAGENTA_NEAR].intensity = 7.0f * (0.35f + 0.65f * a->flicker) * pulse;

    /* Procedural: mist sheets drift on slow analytic sine paths around
     * their authored anchors (duplicated here so the motion is a pure
     * function of t - no per-frame accumulation, hence no frame-rate
     * dependence and no long-term wander). */
    static const float mist_anchor[4][2] = { { 0.0f, 1.1f }, { -1.5f, 1.7f },
                                             { 1.8f, 0.8f }, { 0.0f, 2.3f } };
    for (int i = 0; i < scene->mist_count && i < 4; i++) {
        Node *n = &scene->nodes[scene->mists[i]];
        n->position.x = mist_anchor[i][0] + sinf(t * 0.10f + n->shader_param * 2.1f) * 1.2f;
        n->position.y = mist_anchor[i][1] + sinf(t * 0.17f + n->shader_param) * 0.18f;
    }
}
