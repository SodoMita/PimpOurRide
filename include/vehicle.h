#ifndef CHROMEDRIVE_VEHICLE_H
#define CHROMEDRIVE_VEHICLE_H

#include <stdbool.h>
#include "math3d.h"

/* The dynamics always advance at this rate, independently of display fps.
 * Keeping this visible to tests makes it explicit that render callbacks are a
 * presentation clock, not a license to integrate physics with arbitrary dt. */
#define VEHICLE_FIXED_DT (1.0 / 120.0)
#define VEHICLE_BATTERY_CAPACITY_KWH 82.0
#define VEHICLE_HOVER_REST_HEIGHT 1.5f
/* Collision proxy includes the complete visible nose/tail and camera-mirror
 * clearance, not just the skateboard. Yaw rotates this box with the car. */
#define VEHICLE_COLLISION_HALF_WIDTH 3.42f
#define VEHICLE_COLLISION_HALF_LENGTH 5.90f

typedef enum {
 VEHICLE_STEER_CLIENT = 0, /* wheel is a scroll/input controller for a Wayland app */
 VEHICLE_STEER_DRIVE = 1   /* wheel + WASD are consumed by the simulated vehicle */
} VehicleSteeringMode;

typedef enum {
 VEHICLE_CAMERA_DRIVER = 0,
 VEHICLE_CAMERA_CHASE = 1,
 VEHICLE_CAMERA_SYSTEMS = 2 /* exterior cutaway: body hidden, EV systems exposed */
} VehicleCameraMode;

typedef enum {
 VEHICLE_SEAT_DRIVER = 0,
 VEHICLE_SEAT_PASSENGER = 1
} VehicleSeat;

typedef struct {
 float throttle;  /* -1 reverse .. +1 forward; S also service-brakes while moving forward */
 float steering;  /* -1 left .. +1 right, normalized before the low-speed steering limit */
 float brake;     /* 0..1 blended friction + regenerative braking */
} VehicleInput;

typedef struct {
 /* Rigid-body pose of the cockpit/car rig. All cockpit and virtual-monitor
  * geometry remains in car-local space; these values transform the whole rig
  * through the garage instead of moving individual Wayland surfaces. */
 Vec3 position;
 float yaw, pitch, roll; /* degrees, matching the existing scene/math3d convention */

 float speed;          /* signed longitudinal speed, scene metres/second */
 float acceleration;   /* last longitudinal acceleration, for telemetry/body pitch */
 float yaw_rate;       /* radians/second */
 float steering;       /* smoothed normalized steering position */
 float wheel_angle;    /* visual steering-wheel angle in degrees */
 float throttle, brake;/* smoothed controls, useful for diagnostics */

 /* A single heave degree of freedom is intentional: the request explicitly
  * excludes wheel simulation. Four visual hover pods therefore share this
  * stable spring/damper model instead of pretending to be four tire solvers. */
 float hover_height, hover_speed;

 double battery_kwh; /* double: a parked tick uses only a few micro-kWh */
 float power_kw;       /* positive = battery discharge, negative = net charging */
 float motor_temp_c;
 float distance_m;
 bool collision;       /* true only on a tick that contacts the garage safety bounds */

 double accumulator;   /* fixed-step remainder carried between rendered frames */
} Vehicle;

/* Parametric bicubic Catmull-Rom hull & fastback canopy evaluation.
 * Keeping the surface math in vehicle.c (pure C/math, no GL dependency) allows
 * both the OpenGL renderer (vehicle_scene.c) and native unit tests
 * (test_vehicle.c) to query the exact same hull/canopy surface, verify
 * watertight closure, and assert that all exterior trims and internal EV
 * cutaway components stay strictly within the hull envelope. */
typedef struct {
 float z;        /* longitudinal station in car-local space (-Z = front) */
 float width;    /* half-width at the main shoulder crease */
 float lower;    /* underbody floor height */
 float shoulder; /* main character crease height */
 float top;      /* upper deck / beltline height */
 float arch;     /* fender crown height over front/rear hover thruster bays */
 float scoop;    /* side aero channel indentation along the doors */
} VehicleHullSection;

void vehicle_init(Vehicle *v);
void vehicle_reset_pose(Vehicle *v);
void vehicle_step(Vehicle *v, VehicleInput input, float dt);
void vehicle_update(Vehicle *v, VehicleInput input, double frame_dt);

/* Catmull-Rom is shared by the body/cable renderer and unit tests. It passes
 * through the design control points (unlike an approximating B-spline), has
 * C1 continuity, and needs no external mesh/asset runtime. */
float vehicle_catmull_rom(float p0, float p1, float p2, float p3, float t);
Vec3 vehicle_catmull_rom_vec3(const Vec3 p[4], float t);

/* Parametric hull (u: 0 nose .. 1 tail, v: 0 undertray centre .. 1 deck centre)
 * and fastback canopy (u: 0 cowl .. 1 rear deck, v: 0 beltline .. 1 roof crown)
 * evaluators plus envelope containment check for internal EV packaging. */
VehicleHullSection vehicle_hull_section(float u);
Vec3 vehicle_hull_point(float u, float v, int side, Vec3 *normal);
Vec3 vehicle_canopy_point(float u, float v, int side, Vec3 *normal);
bool vehicle_point_inside_envelope(Vec3 p, float margin);
/* One seating layout owns both upholstered geometry and first-person eyes. */
Vec3 vehicle_seat_anchor(VehicleSeat seat);
Vec3 vehicle_seat_eye(VehicleSeat seat);
/* World-XZ half extents of the visible collision box at the current yaw. */
Vec3 vehicle_collision_extents(float yaw_degrees);

/* Exact inverse transforms keep rendering and 3D ray picking in agreement.
 * GL applies the rig as T * Ry * Rx * Rz; these helpers use the same order. */
Vec3 vehicle_local_to_world(const Vehicle *v, Vec3 point);
Vec3 vehicle_world_to_local(const Vehicle *v, Vec3 point);
Vec3 vehicle_local_direction(const Vehicle *v, Vec3 direction);
Vec3 vehicle_world_direction(const Vehicle *v, Vec3 direction);

#endif
