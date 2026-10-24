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

void vehicle_reset_pose(Vehicle *v) {
 /* Reset is a recovery action, not a magical battery refill or odometer
  * rollback. Energy and distance deliberately survive an F8 recovery. */
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

 /* The garage is a real safety envelope. Clamping at its inner wall faces is
  * preferable to allowing the camera rig to leave the modeled world; losing
  * most longitudinal energy makes a remote-controlled contact recoverable. */
 v->collision = false;
 float old_x = v->position.x, old_z = v->position.z;
 v->position.x = clampf(v->position.x, -11.5f, 11.5f);
 v->position.z = clampf(v->position.z, -35.5f, 12.0f);
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
