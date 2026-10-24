#include "vehicle.h"

#include <math.h>
#include <string.h>

#define CD_PI 3.14159265358979323846f
#define CD_RAD (CD_PI / 180.0f)
#define CD_DEG (180.0f / CD_PI)

static float approach(float value, float target, float rate, float dt) {
 /* Exponential response has two useful properties here: it cannot overshoot
  * and its result is stable when the fixed timestep changes during a test.
  * A per-frame lerp constant would make steering depend on render fps. */
 return target + (value - target) * expf(-rate * dt);
}

static float sign_nonzero(float value) {
 return value < 0.0f ? -1.0f : 1.0f;
}

static int clamp_idx(int i, int count) {
 return i < 0 ? 0 : (i >= count ? count - 1 : i);
}

static int door_index(int side) { return side < 0 ? 0 : 1; }

void vehicle_toggle_doors(Vehicle *v) {
 /* If either side is closed or closing, the first press opens both. A second
  * press closes both; reversing a half-travel door never jumps its pose. */
 bool open = !(v->door_target[0] > 0.5f && v->door_target[1] > 0.5f);
 for (int i = 0; i < 2; i++) v->door_target[i] = open ? 1.0f : 0.0f;
}

float vehicle_door_open(const Vehicle *v, int side) {
 return v->door_open[door_index(side)];
}

float vehicle_door_angle(const Vehicle *v, int side) {
 float t = clampf(vehicle_door_open(v, side), 0.0f, 1.0f);
 t = t * t * (3.0f - 2.0f * t); /* soft start/stop at the hinge detents */
 return (side < 0 ? -1.0f : 1.0f) * t * VEHICLE_DOOR_MAX_ANGLE;
}

void vehicle_reset_pose(Vehicle *v) {
 /* Reset is a recovery action, not a magical battery refill or odometer
  * rollback. Energy and distance deliberately survive an F10 recovery. */
 double battery = v->battery_kwh;
 float temperature = v->motor_temp_c;
 float distance = v->distance_m;
 memset(v, 0, sizeof(*v));
 v->battery_kwh = battery;
 v->motor_temp_c = temperature;
 v->distance_m = distance;
 v->hover_height = VEHICLE_HOVER_REST_HEIGHT;
}

void vehicle_init(Vehicle *v) {
 memset(v, 0, sizeof(*v));
 v->battery_kwh = VEHICLE_BATTERY_CAPACITY_KWH * 0.84f;
 v->motor_temp_c = 24.0f;
 vehicle_reset_pose(v);
}

void vehicle_step(Vehicle *v, VehicleInput input, float dt) {
 if (dt <= 0.0f) return;

 /* Door actuators share the same fixed clock as hover dynamics. Linear target
  * travel plus smoothstep hinge angle is deterministic and reversible without
  * the asymptotic "never quite closed" behavior of an exponential filter. */
 for (int i = 0; i < 2; i++) {
  float travel = dt / VEHICLE_DOOR_TRAVEL_SECONDS;
  float delta = v->door_target[i] - v->door_open[i];
  v->door_open[i] += clampf(delta, -travel, travel);
  v->door_open[i] = clampf(v->door_open[i], 0.0f, 1.0f);
 }

 const float mass = 2200.0f;
 const float wheelbase = 3.25f;
 const float max_steer = 32.0f * CD_RAD;
 const float max_drive_force = 13500.0f;
 const float max_motor_power = 360000.0f; /* dual PMSM, combined 360 kW */
 const float max_speed = 52.0f;
 const float max_reverse_speed = 9.0f;

 input.throttle = clampf(input.throttle, -1.0f, 1.0f);
 input.steering = clampf(input.steering, -1.0f, 1.0f);
 input.brake = clampf(input.brake, 0.0f, 1.0f);
 v->throttle = approach(v->throttle, input.throttle, 4.5f, dt);
 v->steering = approach(v->steering, input.steering, 8.0f, dt);
 v->brake = approach(v->brake, input.brake, 9.0f, dt);
 v->wheel_angle = -v->steering * 135.0f;

 float service_brake = v->brake;
 float motor_command = v->throttle;
 /* Holding S/down is both intuitive reverse control at rest and a brake while
  * travelling forward. Once stopped, the next fixed tick can start reverse;
  * crossing zero in one step is clamped below, preventing oscillation. */
 if (motor_command < 0.0f && v->speed > 0.6f) {
  service_brake = fmaxf(service_brake, -motor_command);
  motor_command = 0.0f;
 }

 float force_limit = fminf(max_drive_force,
                            max_motor_power / fmaxf(3.0f, fabsf(v->speed)));
 float drive_force = motor_command * force_limit;
 if (v->battery_kwh <= 0.0f) drive_force = 0.0f;

 float speed_abs = fabsf(v->speed);
 float aero_drag = 0.82f * v->speed * speed_abs;
 float levitation_drag = speed_abs > 0.001f ? 85.0f * sign_nonzero(v->speed) : 0.0f;
 float coast_regen = fabsf(motor_command) < 0.03f && speed_abs > 0.15f ? 650.0f : 0.0f;
 float brake_force = service_brake * 15500.0f + coast_regen;
 float braking_signed = speed_abs > 0.001f ? brake_force * sign_nonzero(v->speed) : 0.0f;
 float net_force = drive_force - aero_drag - levitation_drag - braking_signed;

 v->acceleration = net_force / mass;
 float next_speed = v->speed + v->acceleration * dt;
 /* Brake/drag forces must not become propulsion when a small timestep crosses
  * zero. This check still permits intentional powered reverse on a later tick. */
 if (v->speed * next_speed < 0.0f && net_force * v->speed < 0.0f) next_speed = 0.0f;
 v->speed = clampf(next_speed, -max_reverse_speed, max_speed);

 /* No lateral tire model is needed for a hover car. A bounded kinematic
  * bicycle yaw response provides predictable steering without simulating
  * wheels, contact patches, slip angles, or individual suspension corners. */
 v->yaw_rate = -(v->speed / wheelbase) * tanf(max_steer * v->steering);
 v->yaw += v->yaw_rate * dt * CD_DEG;
 if (v->yaw > 360.0f || v->yaw < -360.0f) v->yaw = fmodf(v->yaw, 360.0f);

 Vec3 forward = rotate_y(v3(0, 0, -1), v->yaw);
 v->position = vadd(v->position, vmul(forward, v->speed * dt));
 v->distance_m += fabsf(v->speed) * dt;

 /* Collide the complete yaw-rotated visible car with the actual inner wall
  * faces (x = +/-14.70, z = -39.70/+15.70), not a smaller skateboard box.
  * This makes the nose, tail and mirrors contact before crossing the wall. */
 v->collision = false;
 float old_x = v->position.x, old_z = v->position.z;
 Vec3 extent = vehicle_collision_extents_for_vehicle(v);
 float x_limit = 14.70f - extent.x;
 v->position.x = clampf(v->position.x, -x_limit, x_limit);
 v->position.z = clampf(v->position.z, -39.70f + extent.z, 15.70f - extent.z);
 if (old_x != v->position.x || old_z != v->position.z) {
  v->speed *= 0.18f;
  v->collision = true;
 }

 float roll_target = clampf(v->steering * v->speed * v->speed * 0.004f, -4.5f, 4.5f);
 float pitch_target = clampf(v->acceleration * 0.14f, -2.8f, 3.5f);
 v->roll = approach(v->roll, roll_target, 5.0f, dt);
 v->pitch = approach(v->pitch, pitch_target, 5.5f, dt);

 float hover_target = VEHICLE_HOVER_REST_HEIGHT + clampf(-v->acceleration * 0.0015f, -0.025f, 0.025f);
 float hover_acceleration = 48.0f * (hover_target - v->hover_height) - 11.0f * v->hover_speed;
 v->hover_speed += hover_acceleration * dt;
 v->hover_height += v->hover_speed * dt;
 v->position.y = v->hover_height - VEHICLE_HOVER_REST_HEIGHT;

 /* Electrical telemetry is coupled to actual force and velocity, not a random
  * dashboard animation. Regenerative braking returns the recoverable fraction
  * to the same 82 kWh pack; auxiliaries still draw power while parked. */
 float traction_kw = fmaxf(0.0f, drive_force * v->speed / 1000.0f)
                     + fabsf(drive_force) * 0.35f / 1000.0f;
 float regen_kw = fminf(180.0f, brake_force * speed_abs / 1000.0f * 0.72f);
 float auxiliary_kw = 1.4f; /* thermal loop, computers, lighting, low-voltage DC/DC */
 v->power_kw = traction_kw - regen_kw + auxiliary_kw;
 /* At parking load one tick is ~3 micro-kWh. Subtracting that from a float
  * near 69 kWh rounds back to the same value, so energy would never drain;
  * keep the accumulator double-precision until it is displayed. */
 double energy_kwh = (double)v->power_kw * dt / 3600.0;
 if (energy_kwh > 0.0) energy_kwh /= 0.93; /* inverter + battery discharge losses */
 v->battery_kwh = fmin(VEHICLE_BATTERY_CAPACITY_KWH,
                       fmax(0.0, v->battery_kwh - energy_kwh));
 if (v->battery_kwh <= 0.0f && v->power_kw > 0.0f) v->power_kw = 0.0f;

 float temperature_target = 24.0f + fminf(82.0f, fabsf(v->power_kw) * 0.19f);
 v->motor_temp_c = approach(v->motor_temp_c, temperature_target, 0.035f, dt);
}

Vec3 vehicle_collision_extents(float yaw_degrees) {
 float yaw = yaw_degrees * CD_RAD;
 float c = fabsf(cosf(yaw)), s = fabsf(sinf(yaw));
 return v3(c * VEHICLE_COLLISION_HALF_WIDTH + s * VEHICLE_COLLISION_HALF_LENGTH,
           0.0f,
           s * VEHICLE_COLLISION_HALF_WIDTH + c * VEHICLE_COLLISION_HALF_LENGTH);
}

static float door_collision_width(const Vehicle *v, int side, Vec3 closed, float width) {
 Vec3 p = vehicle_door_point(v, side, closed);
 /* Covers the outer offset, flush grip and inner-shell thickness between
  * samples. The closed hull proxy already includes fixed mirror clearance. */
 return fmaxf(width, fabsf(p.x) + 0.04f);
}

Vec3 vehicle_collision_extents_for_vehicle(const Vehicle *v) {
 if (v->door_open[0] < 1e-6f && v->door_open[1] < 1e-6f)
  return vehicle_collision_extents(v->yaw);
 float half_width = VEHICLE_COLLISION_HALF_WIDTH;
 for (int side = -1; side <= 1; side += 2) {
  /* Dense lower-skin sampling follows the sculpted waist through the swing. */
  for (int iu = 0; iu <= 8; iu++) for (int iv = 0; iv <= 6; iv++) {
   float u = VEHICLE_DOOR_U0 + (VEHICLE_DOOR_U1 - VEHICLE_DOOR_U0) * iu / 8.0f;
   float h = VEHICLE_DOOR_V0 + (VEHICLE_DOOR_V1 - VEHICLE_DOOR_V0) * iv / 6.0f;
   half_width = door_collision_width(v, side, vehicle_hull_point(u, h, side, NULL), half_width);
  }
  /* Side glass, belt trim and the moving cantrail all belong to the same
   * full-height collision silhouette; the fixed roof crown is already inside
   * the closed hull proxy and does not rotate with the door. */
  for (int iu = 0; iu <= 6; iu++) for (int iv = 0; iv <= 6; iv++) {
   float hu = VEHICLE_DOOR_U0 + (VEHICLE_DOOR_U1 - VEHICLE_DOOR_U0) * iu / 6.0f;
   float uc = (hu - 0.26f) / 0.64f;
   float vc = VEHICLE_DOOR_CANOPY_V1 * iv / 6.0f;
   half_width = door_collision_width(v, side, vehicle_canopy_point(uc, vc, side, NULL), half_width);
  }
  /* The upholstered panel is the farthest interior trim from the hinge. */
  for (int ix = -1; ix <= 1; ix += 2) for (int iy = -1; iy <= 1; iy += 2)
   for (int iz = -1; iz <= 1; iz += 2) {
    Vec3 local = v3(ix * 0.28f, 0.35f + iy * 0.55f, iz * 0.075f);
    Vec3 closed = vadd(v3(side * 2.00f, -0.39f, 0.64f - VEHICLE_COCKPIT_FORWARD), rotate_y(local, side * -40.0f));
    half_width = door_collision_width(v, side, closed, half_width);
   }
 }
 float yaw = v->yaw * CD_RAD, c = fabsf(cosf(yaw)), s = fabsf(sinf(yaw));
 return v3(c * half_width + s * VEHICLE_COLLISION_HALF_LENGTH, 0.0f,
           s * half_width + c * VEHICLE_COLLISION_HALF_LENGTH);
}

void vehicle_update(Vehicle *v, VehicleInput input, double frame_dt) {
 if (frame_dt < 0.0f) return;
 /* A debugger stop or a busy llvmpipe frame must not trigger hundreds of
  * catch-up steps. At most 250 ms is retained (30 steps), and excess backlog
  * is dropped rather than causing the classic spiral of death. */
 v->accumulator += fmin(frame_dt, 0.25);
 int steps = 0;
 while (v->accumulator + 1e-9 >= VEHICLE_FIXED_DT && steps < 30) {
  vehicle_step(v, input, (float)VEHICLE_FIXED_DT);
  v->accumulator -= VEHICLE_FIXED_DT;
  steps++;
 }
 if (steps == 30 && v->accumulator >= VEHICLE_FIXED_DT)
  v->accumulator = fmod(v->accumulator, VEHICLE_FIXED_DT);
}

float vehicle_catmull_rom(float p0, float p1, float p2, float p3, float t) {
 float t2 = t * t, t3 = t2 * t;
 return 0.5f * ((2.0f * p1) + (-p0 + p2) * t
                + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2
                + (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
}

Vec3 vehicle_catmull_rom_vec3(const Vec3 p[4], float t) {
 return v3(vehicle_catmull_rom(p[0].x, p[1].x, p[2].x, p[3].x, t),
           vehicle_catmull_rom(p[0].y, p[1].y, p[2].y, p[3].y, t),
           vehicle_catmull_rom(p[0].z, p[1].z, p[2].z, p[3].z, t));
}

/* Nine longitudinal design stations define the lower grand-tourer hovercar body:
 *   - low shark-nose aero splitter & matrix-lamp fascia (z = -5.85 .. -5.15)
 *   - muscular flared front fenders over the front mag-lev thruster bays (-3.55)
 *   - sculpted coke-bottle waist with deep door air-channel scoops (0.15)
 *   - broad rear haunches over the rear thruster/powertrain bay (3.55)
 *   - Kammback ducktail spoiler & rising twin-tunnel Venturi diffuser (5.72) */
static const VehicleHullSection hull_sections[] = {
 /*   z      width  lower  shoulder  top    arch   scoop */
 {-5.85f,  1.95f, -.66f,  -.32f,  -.16f,  .00f,  .00f},
 {-5.15f,  2.56f, -.76f,  -.08f,   .14f,  .05f,  .00f},
 {-3.55f,  2.98f, -.82f,   .18f,   .42f,  .15f,  .02f},
 {-1.95f,  2.92f, -.82f,   .28f,   .50f,  .05f,  .11f},
 { 0.15f,  2.80f, -.82f,   .31f,   .52f,  .00f,  .17f},
 { 2.05f,  2.94f, -.82f,   .35f,   .56f,  .07f,  .12f},
 { 3.55f,  3.06f, -.80f,   .38f,   .58f,  .16f,  .02f},
 { 4.80f,  2.72f, -.73f,   .22f,   .46f,  .04f,  .00f},
 { 5.72f,  2.18f, -.60f,   .08f,   .34f,  .00f,  .00f}
};
#define HULL_SECTION_COUNT ((int)(sizeof(hull_sections) / sizeof(hull_sections[0])))

VehicleHullSection vehicle_hull_section(float u) {
 float g = clampf(u, 0.0f, 1.0f) * (float)(HULL_SECTION_COUNT - 1);
 int i = (int)floorf(g);
 if (i >= HULL_SECTION_COUNT - 1) i = HULL_SECTION_COUNT - 2;
 float t = g - (float)i;
 const VehicleHullSection *a = &hull_sections[clamp_idx(i - 1, HULL_SECTION_COUNT)];
 const VehicleHullSection *b = &hull_sections[clamp_idx(i,     HULL_SECTION_COUNT)];
 const VehicleHullSection *c = &hull_sections[clamp_idx(i + 1, HULL_SECTION_COUNT)];
 const VehicleHullSection *d = &hull_sections[clamp_idx(i + 2, HULL_SECTION_COUNT)];
 VehicleHullSection out;
 out.z        = vehicle_catmull_rom(a->z,        b->z,        c->z,        d->z,        t);
 out.width    = fmaxf(1.50f, vehicle_catmull_rom(a->width, b->width, c->width, d->width, t));
 out.lower    = vehicle_catmull_rom(a->lower,    b->lower,    c->lower,    d->lower,    t);
 out.shoulder = vehicle_catmull_rom(a->shoulder, b->shoulder, c->shoulder, d->shoulder, t);
 out.top      = vehicle_catmull_rom(a->top,      b->top,      c->top,      d->top,      t);
 out.arch     = fmaxf(0.0f, vehicle_catmull_rom(a->arch,     b->arch,     c->arch,     d->arch,     t));
 out.scoop    = fmaxf(0.0f, vehicle_catmull_rom(a->scoop,    b->scoop,    c->scoop,    d->scoop,    t));
 return out;
}

static Vec3 hull_eval_raw(float u, float v, int side) {
 VehicleHullSection s = vehicle_hull_section(u);
 float sgn = side < 0 ? -1.0f : 1.0f;
 float span = fmaxf(0.18f, s.shoulder - s.lower);
 /* Seven transverse control points from undertray centreline (0) to hood/deck
  * centreline (6). Reflecting the phantom end control points across x = 0
  * guarantees a horizontal tangent (C1 continuity) across the symmetry plane. */
 float rx[7] = {
  0.00f,
  0.74f * s.width,
  0.96f * s.width,
  (0.98f - s.scoop) * s.width,
  1.00f * s.width,
  0.82f * s.width,
  0.00f
 };
 float ry[7] = {
  s.lower - 0.015f,
  s.lower,
  s.lower + 0.18f * span,
  s.lower + 0.56f * span,
  s.shoulder,
  s.top + s.arch,
  s.top + 0.02f
 };
 float gv = clampf(v, 0.0f, 1.0f) * 6.0f;
 int k = (int)floorf(gv);
 if (k >= 6) k = 5;
 float tv = gv - (float)k;

 float x0 = (k - 1 < 0) ? -rx[1] : rx[k - 1];
 float y0 = (k - 1 < 0) ?  ry[1] : ry[k - 1];
 float x1 = rx[k],     y1 = ry[k];
 float x2 = rx[k + 1], y2 = ry[k + 1];
 float x3 = (k + 2 > 6) ? -rx[5] : rx[k + 2];
 float y3 = (k + 2 > 6) ?  ry[5] : ry[k + 2];

 float x = sgn * fmaxf(0.0f, vehicle_catmull_rom(x0, x1, x2, x3, tv));
 float y = vehicle_catmull_rom(y0, y1, y2, y3, tv);
 return v3(x, y, s.z);
}

Vec3 vehicle_hull_point(float u, float v, int side, Vec3 *normal) {
 Vec3 p = hull_eval_raw(u, v, side);
 if (normal) {
  float du = 0.008f, dv = 0.008f;
  float u0 = fmaxf(0.0f, u - du), u1 = fminf(1.0f, u + du);
  float v0 = fmaxf(0.0f, v - dv), v1 = fminf(1.0f, v + dv);
  Vec3 pu = vsub(hull_eval_raw(u1, v, side), hull_eval_raw(u0, v, side));
  Vec3 pv = vsub(hull_eval_raw(u, v1, side), hull_eval_raw(u, v0, side));
  Vec3 n = (side < 0) ? vcross(pu, pv) : vcross(pv, pu);
  if (fabsf(v) < 1e-4f || fabsf(v - 1.0f) < 1e-4f) n.x = 0.0f;
  *normal = vnorm(n);
 }
 return p;
}

bool vehicle_door_aperture(float u, float v) {
 return u >= VEHICLE_DOOR_U0 && u <= VEHICLE_DOOR_U1
     && v >= VEHICLE_DOOR_V0 && v <= VEHICLE_DOOR_V1;
}

bool vehicle_door_canopy_aperture(float u, float v) {
 float hull_u = 0.26f + 0.64f * u;
 return hull_u >= VEHICLE_DOOR_U0 && hull_u <= VEHICLE_DOOR_U1
     && v >= 0.0f && v < VEHICLE_DOOR_CANOPY_V1;
}

Vec3 vehicle_door_pivot(int side) {
 /* A vertical front-edge hinge on the actual sculpted skin. All closed door
  * points, including the interior trim and frameless glass, rotate about this
  * exact axis; there is no separate open-door approximation. */
 return vehicle_hull_point(VEHICLE_DOOR_U0, 0.52f, side, NULL);
}

Vec3 vehicle_door_point(const Vehicle *v, int side, Vec3 closed_point) {
 Vec3 pivot = vehicle_door_pivot(side);
 return vadd(pivot, rotate_y(vsub(closed_point, pivot), vehicle_door_angle(v, side)));
}

/* Seven longitudinal canopy stations parameterize the fastback greenhouse relative
 * to the hull's upper beltline deck (u_hull in [0.26, 0.90]). Because the base
 * height and width are anchored directly to the hull deck at each z and dh = 0
 * at both endpoints with shallow endpoint tangents, the canopy flows smoothly
 * from the hood cowl into the rear decklid without steps, overhangs, or gaps. */
typedef struct { float dh, width_scale; } CanopyStation;
static const CanopyStation canopy_stations[] = {
 {0.00f, 0.88f}, /* u_c = 0.00: windshield base flush on the hood cowl */
 {0.65f, 0.85f}, /* u_c = 0.17: lower windshield rake */
 {1.05f, 0.82f}, /* u_c = 0.33: windshield header / A-pillar top */
 {1.12f, 0.80f}, /* u_c = 0.50: sleek GT roof crown over the seats */
 {0.78f, 0.74f}, /* u_c = 0.67: B/C-pillar fastback arch */
 {0.32f, 0.64f}, /* u_c = 0.83: shallow fastback rear glass between haunches */
 {0.00f, 0.52f}  /* u_c = 1.00: trailing edge merged seamlessly into rear deck */
};
#define CANOPY_STATION_COUNT ((int)(sizeof(canopy_stations) / sizeof(canopy_stations[0])))

static Vec3 canopy_eval_raw(float u_c, float v_c, int side) {
 float uc = clampf(u_c, 0.0f, 1.0f);
 float vc = clampf(v_c, 0.0f, 1.0f);
 float u_hull = 0.26f + uc * 0.64f;
 Vec3 deck_outer = hull_eval_raw(u_hull, 0.84f, 1);
 Vec3 deck_centre = hull_eval_raw(u_hull, 1.00f, 1);

 float g = uc * (float)(CANOPY_STATION_COUNT - 1);
 int i = (int)floorf(g);
 if (i >= CANOPY_STATION_COUNT - 1) i = CANOPY_STATION_COUNT - 2;
 float t = g - (float)i;
 const CanopyStation *a = &canopy_stations[clamp_idx(i - 1, CANOPY_STATION_COUNT)];
 const CanopyStation *b = &canopy_stations[clamp_idx(i,     CANOPY_STATION_COUNT)];
 const CanopyStation *c = &canopy_stations[clamp_idx(i + 1, CANOPY_STATION_COUNT)];
 const CanopyStation *d = &canopy_stations[clamp_idx(i + 2, CANOPY_STATION_COUNT)];

 float dh = fmaxf(0.0f, vehicle_catmull_rom(a->dh, b->dh, c->dh, d->dh, t));
 float wf = clampf(vehicle_catmull_rom(a->width_scale, b->width_scale,
                                       c->width_scale, d->width_scale, t), 0.40f, 0.95f);
 /* Smooth Hermite taper at the exact cowl and rear-deck endpoints so dh = 0
  * with a gentle slope rather than an abrupt vertical step. */
 if (uc < 0.08f) dh *= (uc / 0.08f);
 if (uc > 0.92f) dh *= ((1.0f - uc) / 0.08f);

 float sgn = side < 0 ? -1.0f : 1.0f;
 float base_x = deck_outer.x * wf;
 /* Blend base_y between the outer beltline and the centre deck spine so that
  * as vc -> 1 and dh -> 0 at the nose/tail, the canopy meets the spine flush. */
 float deck_y = deck_outer.y * (1.0f - vc) + deck_centre.y * vc;
 float base_y = deck_y - 0.012f;
 float angle = vc * 0.5f * CD_PI;
 float x = sgn * base_x * cosf(angle);
 float arch = powf(fmaxf(0.0f, sinf(angle)), 0.78f);
 return v3(x, base_y + dh * arch, deck_outer.z);
}

Vec3 vehicle_canopy_point(float u, float v, int side, Vec3 *normal) {
 Vec3 p = canopy_eval_raw(u, v, side);
 if (normal) {
  float du = 0.01f, dv = 0.01f;
  float u0 = fmaxf(0.0f, u - du), u1 = fminf(1.0f, u + du);
  float v0 = fmaxf(0.0f, v - dv), v1 = fminf(1.0f, v + dv);
  Vec3 pu = vsub(canopy_eval_raw(u1, v, side), canopy_eval_raw(u0, v, side));
  Vec3 pv = vsub(canopy_eval_raw(u, v1, side), canopy_eval_raw(u, v0, side));
  Vec3 n = (side < 0) ? vcross(pu, pv) : vcross(pv, pu);
  if (fabsf(v - 1.0f) < 1e-4f) n.x = 0.0f;
  *normal = vnorm(n);
 }
 return p;
}

bool vehicle_point_inside_envelope(Vec3 p, float margin) {
 VehicleHullSection nose = vehicle_hull_section(0.0f);
 VehicleHullSection tail = vehicle_hull_section(1.0f);
 if (p.z < nose.z + margin || p.z > tail.z - margin) return false;

 /* Binary search longitudinal parameter u in [0, 1] matching p.z. */
 float lo = 0.0f, hi = 1.0f;
 for (int iter = 0; iter < 16; iter++) {
  float mid = 0.5f * (lo + hi);
  if (vehicle_hull_section(mid).z < p.z) lo = mid;
  else hi = mid;
 }
 float u = 0.5f * (lo + hi);
 VehicleHullSection s = vehicle_hull_section(u);

 float ax = fabsf(p.x);
 if (ax > s.width - margin) return false;
 if (p.y < s.lower - 0.04f + margin) return false;

 /* Ceiling height is the hull upper deck outside the greenhouse, or the
  * fastback canopy arch when inside the cabin footprint (u in [0.26, 0.90]). */
 float y_ceiling = s.top + s.arch * 0.6f;
 if (u >= 0.26f && u <= 0.90f) {
  float u_c = (u - 0.26f) / 0.64f;
  Vec3 c_base = canopy_eval_raw(u_c, 0.0f, 1);
  Vec3 c_top  = canopy_eval_raw(u_c, 1.0f, 1);
  if (c_base.x > 0.05f && ax <= c_base.x) {
   float cos_a = clampf(ax / c_base.x, 0.0f, 1.0f);
   float angle = acosf(cos_a);
   float arch = powf(fmaxf(0.0f, sinf(angle)), 0.78f);
   float y_canopy = c_base.y + (c_top.y - c_base.y) * arch;
   if (y_canopy > y_ceiling) y_ceiling = y_canopy;
  }
 }
 return p.y <= y_ceiling - margin;
}

Vec3 vehicle_seat_anchor(VehicleSeat seat) {
 return v3(seat == VEHICLE_SEAT_PASSENGER ? 1.25f : -0.90f,
           0.0f, -VEHICLE_COCKPIT_FORWARD);
}

Vec3 vehicle_seat_eye(VehicleSeat seat) {
 Vec3 anchor = vehicle_seat_anchor(seat);
 /* Forward of the reclined back/head restraint and directly above the
  * cushion. The shared cockpit offset places this real seat beside the door
  * aperture instead of leaving the occupant behind its rear edge. */
 return v3(anchor.x, 0.86f, anchor.z + 2.55f);
}

Vec3 vehicle_local_direction(const Vehicle *v, Vec3 direction) {
 return rotate_y(rotate_x(rotate_z(direction, v->roll), v->pitch), v->yaw);
}

Vec3 vehicle_world_direction(const Vehicle *v, Vec3 direction) {
 return rotate_z(rotate_x(rotate_y(direction, -v->yaw), -v->pitch), -v->roll);
}

Vec3 vehicle_local_to_world(const Vehicle *v, Vec3 point) {
 return vadd(v->position, vehicle_local_direction(v, point));
}

Vec3 vehicle_world_to_local(const Vehicle *v, Vec3 point) {
 return vehicle_world_direction(v, vsub(point, v->position));
}
