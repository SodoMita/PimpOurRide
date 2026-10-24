/*
 * animation.h - procedural animation + small physics simulation.
 *
 * Two kinds of motion, deliberately kept apart:
 *
 * 1. PROCEDURAL (analytic, evaluated from total time): ceiling-fan spin,
 *    neon pulse, mist drift, city-window twinkle (shader-side). These have
 *    closed-form answers f(t), so they are exact at any frame rate and can
 *    never accumulate integration error.
 *
 * 2. PHYSICS (integrated, fixed-step): the hanging-lamp pendulum, three
 *    bouncing/rolling balls, and 420 rain streaks outside the far opening.
 *    Integrated with semi-implicit Euler at a FIXED 120 Hz from an
 *    accumulator fed by real delta-time. Fixed-step is what makes the
 *    simulation frame-rate independent *and* stable: a 120 Hz physics tick
 *    behaves identically whether the compositor presents at 30, 60 or
 *    120 fps, and the integrator's stability does not depend on a hitchy
 *    render dt. Render dt is clamped to 0.1 s before it reaches here so a
 *    suspended/occluded window cannot ask for a catch-up spiral.
 */
#ifndef GARAGE3D_ANIMATION_H
#define GARAGE3D_ANIMATION_H

#include "scene.h"
#include <stdbool.h>

#define PHYSICS_HZ 120.0
#define MAX_RAIN 420

typedef struct {
    Vec3 position, velocity;
    float radius, restitution;
    Quat orientation; /* accumulated rolling rotation */
} BallBody;

typedef struct {
    double time;          /* total simulated seconds (pauses with Space) */
    double accumulator;   /* unconsumed physics time */
    bool paused;

    /* Pendulum: two-axis swing of the lamp pivot. L = cable length (3 m,
     * matching scene.c), integrated as a'' = -(g/L) sin(a) - c a'. */
    float swing_x, swing_z, swing_vx, swing_vz;
    double next_kick;     /* periodic gentle re-energising (a "breeze") */

    BallBody balls[3];

    /* Rain lives in a volume OUTSIDE the far opening (z < HALL_FAR_Z), so
     * it reads through the doorway exactly like the mockup's rainy city. */
    Vec3 rain_pos[MAX_RAIN];
    Vec3 rain_vel[MAX_RAIN];
    int rain_count;

    float fan_angle_deg;
    float flicker;        /* 0..1 emissive scale of the faulty tube */
    unsigned rng_state;   /* xorshift32: deterministic, no libc rand() */
} AnimationState;

void anim_init(AnimationState *a, const Scene *scene);
/* Advance everything by one rendered frame's dt (already clamped). */
void anim_update(AnimationState *a, Scene *scene, float dt);
/* One fixed physics tick. Exposed (not static) so the unit tests can step
 * the exact production integrator without a renderer. */
void anim_physics_step(AnimationState *a, Scene *scene, float h);

#endif
