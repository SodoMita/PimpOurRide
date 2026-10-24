#include "vehicle.h"

#include <math.h>
#include <stdio.h>

static int failures;
#define CHECK(condition, ...) do { if (!(condition)) { \
 fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
 fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); failures++; \
} } while (0)

static int nearf(float a, float b, float epsilon) { return fabsf(a - b) <= epsilon; }

static void run_steps(Vehicle *v, VehicleInput input, int steps) {
 for (int i = 0; i < steps; i++) vehicle_step(v, input, (float)VEHICLE_FIXED_DT);
}

int main(void) {
 CHECK(vehicle_catmull_rom(9, 2, 7, 20, 0) == 2.0f, "spline must interpolate p1");
 CHECK(vehicle_catmull_rom(9, 2, 7, 20, 1) == 7.0f, "spline must interpolate p2");
 Vec3 controls[4] = {{0, 0, 0}, {1, 2, 3}, {4, 5, 6}, {9, 9, 9}};
 Vec3 midpoint = vehicle_catmull_rom_vec3(controls, 0.5f);
 CHECK(nearf(midpoint.x, 2.25f, 1e-5f) && nearf(midpoint.y, 3.375f, 1e-5f),
       "3D spline midpoint mismatch: %f %f", midpoint.x, midpoint.y);

 Vehicle v;
 vehicle_init(&v);
 CHECK(nearf(v.battery_kwh, 82.0f * 0.84f, 1e-4f), "initial state of charge");
 float initial_energy = v.battery_kwh;
 run_steps(&v, (VehicleInput){0}, 240);
 CHECK(v.speed == 0.0f && v.position.z == 0.0f, "parked car drifted");
 CHECK(v.battery_kwh < initial_energy, "parked auxiliary load did not consume energy");

 run_steps(&v, (VehicleInput){.throttle = 1}, 360);
 CHECK(v.speed > 5.0f && v.position.z < -2.0f,
       "forward drive did not move car: speed=%f z=%f", v.speed, v.position.z);
 CHECK(v.motor_temp_c >= 24.0f && v.battery_kwh < initial_energy, "drive telemetry invalid");
 float drive_z = v.position.z;
 run_steps(&v, (VehicleInput){.brake = 1}, 360);
 CHECK(fabsf(v.speed) < 0.2f && v.position.z < drive_z,
       "service brake did not stop car: speed=%f", v.speed);

 vehicle_reset_pose(&v);
 run_steps(&v, (VehicleInput){.throttle = 1}, 240);
 run_steps(&v, (VehicleInput){.throttle = 0.35f, .steering = 1}, 180);
 CHECK(v.yaw < -1.0f && v.position.x > 0.01f,
       "right steering has wrong coordinate sign: yaw=%f x=%f", v.yaw, v.position.x);

 vehicle_reset_pose(&v);
 run_steps(&v, (VehicleInput){.throttle = -1}, 120);
 CHECK(v.speed < -1.0f && v.position.z > 0.2f,
       "reverse drive failed: speed=%f z=%f", v.speed, v.position.z);

 /* Rig transforms are the bridge between rendering and picking. A round-trip
  * test catches the easy-to-miss inverse Euler-order mistake. */
 vehicle_reset_pose(&v);
 v.position = v3(3, 0.2f, -7); v.yaw = 37; v.pitch = 2; v.roll = -3;
 Vec3 local = {0.4f, 1.1f, -2.3f};
 Vec3 round_trip = vehicle_world_to_local(&v, vehicle_local_to_world(&v, local));
 CHECK(nearf(local.x, round_trip.x, 2e-4f) && nearf(local.y, round_trip.y, 2e-4f)
       && nearf(local.z, round_trip.z, 2e-4f), "vehicle transform round-trip failed");

 vehicle_reset_pose(&v);
 v.position = v3(11.49f, 0, -35.49f);
 run_steps(&v, (VehicleInput){.throttle = 1, .steering = -1}, 1200);
 CHECK(v.position.x >= -11.5001f && v.position.x <= 11.5001f
       && v.position.z >= -35.5001f && v.position.z <= 12.0001f,
       "car escaped garage bounds: %f %f", v.position.x, v.position.z);
 CHECK(isfinite(v.hover_height) && isfinite(v.roll) && isfinite(v.pitch),
       "hover/body dynamics became non-finite");

 /* Both update patterns execute exactly 240 fixed ticks. Their states should
  * therefore match even though one render stream ran at 120 and one at 60 fps. */
 Vehicle a, b;
 vehicle_init(&a); vehicle_init(&b);
 VehicleInput input = {.throttle = 0.7f, .steering = -0.3f};
 for (int i = 0; i < 240; i++) vehicle_update(&a, input, VEHICLE_FIXED_DT);
 for (int i = 0; i < 120; i++) vehicle_update(&b, input, VEHICLE_FIXED_DT * 2);
 CHECK(nearf(a.position.x, b.position.x, 1e-3f) && nearf(a.position.z, b.position.z, 1e-3f)
       && nearf(a.speed, b.speed, 1e-3f), "fixed-step result depends on render cadence");

 /* Verify bicubic Catmull-Rom hull & fastback canopy watertightness, symmetry,
  * and that all cutaway EV systems sit strictly inside the hull envelope. */
 for (int i = 0; i <= 20; i++) {
  float u = (float)i / 20.0f;
  Vec3 n_bot, n_top;
  Vec3 bot_l = vehicle_hull_point(u, 0.0f, -1, &n_bot);
  Vec3 bot_r = vehicle_hull_point(u, 0.0f,  1, NULL);
  Vec3 top_l = vehicle_hull_point(u, 1.0f, -1, &n_top);
  Vec3 top_r = vehicle_hull_point(u, 1.0f,  1, NULL);
  CHECK(nearf(bot_l.x, 0.0f, 1e-4f) && nearf(bot_r.x, 0.0f, 1e-4f),
        "undertray seam not closed at u=%f", u);
  CHECK(nearf(top_l.x, 0.0f, 1e-4f) && nearf(top_r.x, 0.0f, 1e-4f),
        "hood/deck seam not closed at u=%f", u);
  CHECK(nearf(n_bot.x, 0.0f, 1e-4f) && nearf(n_top.x, 0.0f, 1e-4f),
        "centreline normal not horizontal at u=%f", u);

  VehicleHullSection sec = vehicle_hull_section(u);
  Vec3 c_sill = vehicle_canopy_point(u, 0.0f, 1, NULL);
  Vec3 c_peak = vehicle_canopy_point(u, 1.0f, 1, NULL);
  VehicleHullSection c_sec = vehicle_hull_section(0.26f + u * 0.64f);
  CHECK(c_sill.x <= c_sec.width * 0.95f && nearf(c_peak.x, 0.0f, 1e-4f),
        "canopy overhangs hull at u=%f: sill_x=%f hull_w=%f", u, c_sill.x, c_sec.width);
  CHECK(sec.width >= 1.90f && sec.top > sec.lower, "invalid hull section at u=%f", u);
 }

 /* Representative bounding corners of the internal EV cutaway components:
  * skateboard battery modules, front/rear motors & inverters, rear OBC/DCDC,
  * slanted front heat exchanger, coolant tank, 12 V battery, and cabin seats. */
 const Vec3 internal_parts[] = {
  { 1.40f, -0.50f, -1.47f}, {-1.40f, -0.50f,  1.47f}, /* 82 kWh battery modules */
  { 1.25f, -0.34f, -3.45f}, {-0.98f, -0.42f, -3.45f}, /* front motor/inverter/gearbox */
  { 1.25f, -0.34f,  3.45f}, {-0.98f, -0.42f,  3.45f}, /* rear motor/inverter/gearbox */
  { 0.00f, -0.30f,  2.55f}, {-1.15f, -0.30f,  4.10f}, /* 800 V junction, OBC, DC/DC */
  { 1.05f, -0.42f, -4.55f}, { 1.18f, -0.25f, -2.75f}, /* front radiator & coolant tank */
  {-1.18f, -0.25f, -2.75f}, { 0.82f,  0.74f,  1.50f}  /* 12 V battery & sport seat head */
 };
 for (size_t i = 0; i < sizeof(internal_parts) / sizeof(internal_parts[0]); i++) {
  CHECK(vehicle_point_inside_envelope(internal_parts[i], 0.02f),
        "EV component %zu protrudes outside hull envelope: (%f, %f, %f)",
        i, internal_parts[i].x, internal_parts[i].y, internal_parts[i].z);
 }

 if (failures) {
  fprintf(stderr, "%d vehicle checks failed\n", failures);
  return 1;
 }
 puts("PASS: Catmull-Rom splines, hover EV dynamics, braking/reverse, bounds, rig transforms, fixed-step determinism");
 return 0;
}
