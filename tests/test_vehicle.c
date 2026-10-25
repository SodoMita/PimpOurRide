#include "vehicle.h"
#include "city.h"

#include <math.h>
#include <stdio.h>

static int failures;
#define CHECK(condition, ...) do { if (!(condition)) { \
 fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
 fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); failures++; \
} } while (0)

static int nearf(float a, float b, float epsilon) { return fabsf(a - b) <= epsilon; }

/* Return the outer side-skin x coordinate at a point's z/y. The simple
 * envelope deliberately checks overall packaging bounds; this finer test also
 * respects the sculpted door waist, where maximum hull width is not the skin. */
static float hull_side_limit(Vec3 p) {
 float lo = 0.0f, hi = 1.0f;
 for (int i = 0; i < 16; i++) {
  float mid = 0.5f * (lo + hi);
  if (vehicle_hull_section(mid).z < p.z) lo = mid; else hi = mid;
 }
 float u = 0.5f * (lo + hi), limit = 0.0f;
 Vec3 a = vehicle_hull_point(u, 0.0f, 1, NULL);
 for (int i = 1; i <= 240; i++) {
  Vec3 b = vehicle_hull_point(u, i / 240.0f, 1, NULL);
  if ((p.y >= a.y && p.y <= b.y) || (p.y >= b.y && p.y <= a.y)) {
   float t = fabsf(b.y - a.y) > 1e-6f ? (p.y - a.y) / (b.y - a.y) : 0.0f;
   float x = a.x + (b.x - a.x) * t;
   if (x > limit) limit = x;
  }
  a = b;
 }
 return limit;
}

static void run_steps(Vehicle *v, VehicleInput input, int steps) {
 for (int i = 0; i < steps; i++) vehicle_step(v, input, (float)VEHICLE_FIXED_DT);
}

typedef struct {
 Vehicle vehicle;
 double phase;
 int ticks, contacts, yield_ticks;
 bool visited[4];
 bool visited_tunnel,visited_square,visited_parking,visited_stream_garage;
 int first_contact_tick,last_contact_tick,first_contact_wp,last_contact_wp;
 Vec3 first_contact_position,last_contact_position;
} TourResult;

static TourResult run_city_tour(double phase) {
 TourResult r = {.phase = phase,.first_contact_tick=-1};
 vehicle_init(&r.vehicle);
 r.vehicle.simulation_time = phase;
 vehicle_autopilot_set(&r.vehicle, true);
 while (r.vehicle.autopilot_enabled && r.ticks < 60000) {
  vehicle_step(&r.vehicle, (VehicleInput){0}, (float)VEHICLE_FIXED_DT);
  r.ticks++;
  if(r.vehicle.collision){
   if(r.first_contact_tick<0){r.first_contact_tick=r.ticks;r.first_contact_wp=r.vehicle.autopilot_waypoint;r.first_contact_position=r.vehicle.position;}
   r.last_contact_tick=r.ticks;r.last_contact_wp=r.vehicle.autopilot_waypoint;r.last_contact_position=r.vehicle.position;r.contacts++;
  }
  r.yield_ticks += r.vehicle.autopilot_yield;
  const char *here = city_garage_at(r.vehicle.position);
  for (int g = 0; g < 4; g++) if (here == city_garage(g).name) r.visited[g] = true;
  CityChunk chunk=city_chunk_at(r.vehicle.position);
  r.visited_tunnel |= city_stream_is_tunnel(r.vehicle.position);
  r.visited_square |= chunk.x==0&&chunk.z==-4&&chunk.feature==CITY_FEATURE_SQUARE;
  r.visited_parking |= chunk.x==1&&chunk.z==-4&&chunk.feature==CITY_FEATURE_PARKING;
  if(here==city_chunk_garage(city_chunk(2,-4),1).name)r.visited_stream_garage=true;
 }
 /* Autopilot requests a final brake at the home terminus. Keep simulating
  * after it disables so the test proves actual parked rest, not merely route
  * completion with residual speed. */
 if (!r.vehicle.autopilot_enabled) for (int i = 0; i < 240; i++) {
  vehicle_step(&r.vehicle, (VehicleInput){0}, (float)VEHICLE_FIXED_DT);
  r.contacts += r.vehicle.collision;
 }
 return r;
}

static int vehicle_traffic_overlap(const Vehicle *v, CityTrafficCar car) {
 Vec3 a = vehicle_collision_extents_for_vehicle(v);
 Vec3 b = city_traffic_collision_extents(car.yaw);
 return fabsf(v->position.x - car.position.x) < a.x + b.x - 1e-4f
        && fabsf(v->position.z - car.position.z) < a.z + b.z - 1e-4f;
}

static int traffic_box_overlap(CityTrafficCar car, float x, float z, float w, float d) {
 Vec3 e = city_traffic_collision_extents(car.yaw);
 return fabsf(car.position.x - x) < e.x + w * 0.5f
        && fabsf(car.position.z - z) < e.z + d * 0.5f;
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
 v.position = v3(5, 0, 0); v.yaw = -90; /* drive directly at the right wall */
 bool wall_contact = false;
 for (int i = 0; i < 1200; i++) {
  vehicle_step(&v, (VehicleInput){.throttle = 1}, (float)VEHICLE_FIXED_DT);
  wall_contact |= v.collision;
 }
 Vec3 extent = vehicle_collision_extents(v.yaw);
 CHECK(wall_contact && v.position.x + extent.x <= 14.7001f,
       "visible rotated car crosses the shared home wall: pos=(%f,%f) extent=(%f,%f)",
       v.position.x, v.position.z, extent.x, extent.z);
 /* The far wall is not a clamp anymore: its real 13 m opening leads onto the
  * central street. A straight car must be able to leave the original hall. */
 vehicle_reset_pose(&v);
 bool exited = false;
 for (int i = 0; i < 1800 && !exited; i++) {
  vehicle_step(&v, (VehicleInput){.throttle = 1}, (float)VEHICLE_FIXED_DT);
  exited = v.position.z < -42.0f;
 }
 CHECK(exited && fabsf(v.position.x) < 0.01f,
       "car could not drive through the real home-garage opening: %f %f", v.position.x, v.position.z);
 CHECK(city_building_count() >= 16 && city_garage_count() == 4,
       "connected city data is incomplete");
 CHECK(city_garage_at(v3(65, 0, -95)) == city_garage(3).name,
       "east service garage is not enterable/addressable");
 /* The outer city has no finite-world clamp. A far road intersection stays
  * untouched, while a streamed civic fountain is genuinely physical. */
 Vehicle far; vehicle_init(&far); far.position = v3(8000, 0, 8000);
 city_constrain_vehicle(&far, far.position);
 CHECK(!far.collision && far.position.x == 8000.0f && far.position.z == 8000.0f,
       "infinite-city road was clamped or obstructed: (%f,%f)", far.position.x, far.position.z);
 CitySolid streamed = city_chunk_solid(city_chunk(0, -4), 0);
 Vehicle civic; vehicle_init(&civic); civic.position = v3(streamed.x, 0, streamed.z);
 city_constrain_vehicle(&civic, civic.position);
 CHECK(civic.collision
       && (fabsf(civic.position.x - streamed.x) > 0.01f
           || fabsf(civic.position.z - streamed.z) > 0.01f),
       "streamed square solid has no matching collision");
 Vec3 straight = vehicle_collision_extents(0), sideways = vehicle_collision_extents(90);
 CHECK(nearf(straight.x, VEHICLE_COLLISION_HALF_WIDTH, 1e-5f)
       && nearf(straight.z, VEHICLE_COLLISION_HALF_LENGTH, 1e-5f)
       && nearf(sideways.x, VEHICLE_COLLISION_HALF_LENGTH, 1e-5f)
       && nearf(sideways.z, VEHICLE_COLLISION_HALF_WIDTH, 1e-5f),
       "collision box does not rotate with the visible car");
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

 /* Autopilot is a fixed-step input controller, not a render animation. Full
  * tours at independent traffic phases must be deterministic, visit every
  * garage, yield when necessary, and settle at home without a single contact. */
 {
  const double phases[] = {0, 3, 6, 9, 12, 15};
  TourResult first = run_city_tour(phases[0]);
  TourResult repeat = run_city_tour(phases[0]);
  CHECK(first.ticks == repeat.ticks && first.contacts == repeat.contacts
        && nearf(first.vehicle.position.x, repeat.vehicle.position.x, 1e-5f)
        && nearf(first.vehicle.position.z, repeat.vehicle.position.z, 1e-5f)
        && nearf(first.vehicle.distance_m, repeat.vehicle.distance_m, 1e-4f),
        "identical traffic-phase tours are not deterministic");
  int total_yields = 0;
  for (size_t p = 0; p < sizeof(phases) / sizeof(phases[0]); p++) {
   TourResult r = p == 0 ? first : run_city_tour(phases[p]);
   total_yields += r.yield_ticks;
   CHECK(r.visited[0] && r.visited[1] && r.visited[2] && r.visited[3]
         && r.visited_tunnel && r.visited_square && r.visited_parking && r.visited_stream_garage
         && !r.vehicle.autopilot_enabled && r.ticks < 60000
         && r.vehicle.distance_m > 450.0f && r.contacts == 0
         && fabsf(r.vehicle.speed) < 0.2f
         && city_garage_at(r.vehicle.position) == city_garage(0).name,
         "phase %.1f tour failed: ticks=%d contacts=%d enabled=%d visited=%d%d%d%d features=%d%d%d%d speed=%f pos=(%f,%f) distance=%f wp=%d",
         phases[p], r.ticks, r.contacts, r.vehicle.autopilot_enabled,
         r.visited[0], r.visited[1], r.visited[2], r.visited[3],
         r.visited_tunnel,r.visited_square,r.visited_parking,r.visited_stream_garage,
         r.vehicle.speed, r.vehicle.position.x, r.vehicle.position.z, r.vehicle.distance_m,r.vehicle.autopilot_waypoint);
   if(r.contacts)fprintf(stderr,"contact trace phase %.1f: first tick=%d wp=%d pos=(%f,%f), last tick=%d wp=%d pos=(%f,%f)\n",
                         phases[p],r.first_contact_tick,r.first_contact_wp,r.first_contact_position.x,r.first_contact_position.z,
                         r.last_contact_tick,r.last_contact_wp,r.last_contact_position.x,r.last_contact_position.z);

   Vehicle exit = r.vehicle;
   vehicle_autopilot_set(&exit, true);
   bool reversed = false;
   int exit_contacts = 0, exit_ticks = 0;
   while (exit_ticks++ < 6000 && (exit.autopilot_reverse || exit.autopilot_waypoint < 3)) {
    vehicle_step(&exit, (VehicleInput){0}, (float)VEHICLE_FIXED_DT);
    reversed |= exit.speed < -0.5f;
    exit_contacts += exit.collision;
   }
   CHECK(reversed && !exit.autopilot_reverse && exit.autopilot_waypoint >= 3
         && exit_ticks < 6000 && exit_contacts == 0,
         "phase %.1f reverse restart failed: ticks=%d wp=%d reverse=%d contacts=%d",
         phases[p], exit_ticks, exit.autopilot_waypoint, exit.autopilot_reverse, exit_contacts);
  }
  CHECK(total_yields > 0, "traffic phases never exercised predictive yielding");

  /* Probe the home gate directly across a complete stream of gaps. At least
   * one phase must hold outside the junction and one must commit through it. */
  bool blocked = false, released = false;
  for (float phase = 0; phase < 16.0f; phase += 0.25f) {
   Vehicle gate; vehicle_init(&gate);
   gate.position = v3(0, 0, -46); gate.simulation_time = phase;
   gate.autopilot_enabled = true; gate.autopilot_waypoint = 1;
   VehicleInput in = city_autopilot_input(&gate, (float)VEHICLE_FIXED_DT);
   blocked |= gate.autopilot_yield && gate.autopilot_crossing == 0 && in.brake > 0.9f;
   released |= !gate.autopilot_yield && gate.autopilot_crossing == 1;
  }
  CHECK(blocked && released, "home crossing did not demonstrate both predictive braking and gap release");

  /* A completed run parked nose-in must back through the opening, then resume
  * the forward route without resetting the world clock or contacting traffic. */
  Vehicle restart = first.vehicle;
  vehicle_autopilot_set(&restart, true);
  CHECK(restart.autopilot_reverse, "nose-in restart did not select reverse exit");
  bool reversed = false, forward = false;
  int restart_contacts = 0;
  double restart_distance = restart.distance_m;
  for (int i = 0; i < 7200 && restart.autopilot_enabled; i++) {
   vehicle_step(&restart, (VehicleInput){0}, (float)VEHICLE_FIXED_DT);
   reversed |= restart.speed < -0.5f;
   forward |= !restart.autopilot_reverse && restart.speed > 0.5f;
   restart_contacts += restart.collision;
  }
  CHECK(reversed && forward && !restart.autopilot_reverse
        && restart.autopilot_waypoint >= 3 && restart.distance_m > restart_distance + 100.0f
        && restart_contacts == 0,
        "reverse exit did not resume the forward city route cleanly: wp=%d contacts=%d",
        restart.autopilot_waypoint, restart_contacts);
 }

 /* The renderer's deterministic traffic poses also clear the visible static
  * city. This prevents a collision-free tour from being achieved by moving
  * ambient cars through a wall or off their rendered roads. */
 CHECK(city_traffic_count() == CITY_TRAFFIC_COUNT, "traffic count mismatch");
 for (double time = -2.0; time < 124.0; time += 0.25) {
  for (int i = 0; i < city_traffic_count(); i++) {
   CityTrafficCar car = city_traffic_at(i, time);
   CityTrafficCar same = city_traffic_at(i, time);
   CHECK(nearf(car.position.x, same.position.x, 1e-6f)
         && nearf(car.position.z, same.position.z, 1e-6f) && car.speed > 0.0f,
         "traffic pose is not deterministic");
   bool on_road = nearf(car.position.z, -60.0f, 1e-4f)
                  || nearf(car.position.z, -124.0f, 1e-4f)
                  || nearf(fabsf(car.position.x), 150.0f, 1e-4f);
   CHECK(on_road, "traffic left the outer ring at t=%f car=%d pos=(%f,%f)",
         time, i, car.position.x, car.position.z);
   for (int b = 0; b < city_building_count(); b++) {
    CityBuilding building = city_building(b);
    CHECK(!traffic_box_overlap(car, building.x, building.z,
                               building.width + 2, building.depth + 2),
          "traffic intersects building %d at t=%f car=%d", b, time, i);
   }
   for (int g = 1; g < city_garage_count(); g++) {
    CityGarage garage = city_garage(g);
    if (garage.axis == 1) {
     CHECK(!traffic_box_overlap(car, garage.x - garage.width * 0.5f, garage.z, 0.6f, garage.depth)
           && !traffic_box_overlap(car, garage.x + garage.width * 0.5f, garage.z, 0.6f, garage.depth),
           "traffic intersects garage %d side wall", g);
    } else {
     CHECK(!traffic_box_overlap(car, garage.x, garage.z - garage.depth * 0.5f, garage.width, 0.6f)
           && !traffic_box_overlap(car, garage.x, garage.z + garage.depth * 0.5f, garage.width, 0.6f),
           "traffic intersects garage %d side wall", g);
    }
   }
  }
 }

 /* Collision fallback uses the same end-of-tick traffic pose as rendering.
  * A stationary manual car ahead of an ambient car must be pushed clear, not
  * left visibly interpenetrated, and F10 recovery must not rewind that world. */
 {
  Vehicle fallback; vehicle_init(&fallback);
  CityTrafficCar target = city_traffic_at(2, fallback.simulation_time);
  fallback.position = vadd(target.position, v3(12, 0, 0));
  fallback.yaw = target.yaw;
  bool contacted = false, overlapped = false;
  for (int i = 0; i < 180; i++) {
   vehicle_step(&fallback, (VehicleInput){0}, (float)VEHICLE_FIXED_DT);
   contacted |= fallback.collision;
   target = city_traffic_at(2, fallback.simulation_time);
   overlapped |= vehicle_traffic_overlap(&fallback, target);
  }
  CHECK(contacted && !overlapped, "moving traffic fallback left visible overlap: contact=%d overlap=%d",
        contacted, overlapped);
  double world_time = fallback.simulation_time;
  vehicle_reset_pose(&fallback);
  CHECK(nearf((float)fallback.simulation_time, (float)world_time, 1e-5f),
        "F10 recovery rewound the shared traffic clock");
 }

 /* The physical door interlock remains authoritative in automatic mode. */
 vehicle_reset_pose(&v); vehicle_toggle_doors(&v); run_steps(&v, (VehicleInput){0}, 240);
 vehicle_autopilot_set(&v, true); run_steps(&v, (VehicleInput){0}, 60);
 CHECK(v.door_target[0] == 0.0f && v.door_target[1] == 0.0f && v.speed == 0.0f,
       "autopilot moved before the open doors were closed");
 run_steps(&v, (VehicleInput){0}, 300);
 CHECK(v.door_open[0] == 0.0f && v.door_open[1] == 0.0f && v.distance_m > 0.1f,
       "autopilot did not start after door closure");
 vehicle_autopilot_set(&v, false);

 /* Real door assemblies: fixed-step travel, reversible targets and one rigid
  * hinge transform shared by exterior skin, interior trim and side glass. */
 vehicle_reset_pose(&v);
 CHECK(vehicle_door_aperture(0.40f, 0.50f) && !vehicle_door_aperture(0.20f, 0.50f)
       && !vehicle_door_aperture(0.40f, 0.10f), "door aperture bounds are inconsistent");
 CHECK(vehicle_door_aperture(0.40f, VEHICLE_DOOR_V0)
       && vehicle_door_aperture(0.40f, VEHICLE_DOOR_V1)
       && !vehicle_door_aperture(0.40f, VEHICLE_DOOR_V0 - 0.001f)
       && !vehicle_door_aperture(0.40f, VEHICLE_DOOR_V1 + 0.001f),
       "lower door does not reach the exact sill/beltline boundaries");
 float door_uc = (0.40f - 0.26f) / 0.64f;
 CHECK(vehicle_door_canopy_aperture(door_uc, 0.0f)
       && vehicle_door_canopy_aperture(door_uc, VEHICLE_DOOR_CANOPY_V1 - 0.001f)
       && !vehicle_door_canopy_aperture(door_uc, VEHICLE_DOOR_CANOPY_V1 + 0.001f)
       && !vehicle_door_canopy_aperture(0.0f, 0.30f),
       "upper door does not reach the roof rail or leaks into fixed roof");
 for (int side = -1; side <= 1; side += 2) {
  Vec3 pivot = vehicle_door_pivot(side);
  Vec3 closed = vehicle_hull_point(VEHICLE_DOOR_U1, 0.55f, side, NULL);
  Vec3 identity = vehicle_door_point(&v, side, closed);
  CHECK(nearf(identity.x, closed.x, 1e-5f) && nearf(identity.z, closed.z, 1e-5f),
        "closed door is not an identity pose");
  Vec3 pivot_moved = vehicle_door_point(&v, side, pivot);
  CHECK(nearf(pivot_moved.x, pivot.x, 1e-5f) && nearf(pivot_moved.z, pivot.z, 1e-5f),
        "door hinge pivot moved in the closed pose");
 }
 vehicle_toggle_doors(&v);
 run_steps(&v, (VehicleInput){0}, 60);
 CHECK(v.door_open[0] > 0.2f && v.door_open[0] < 0.8f
       && nearf(v.door_open[0], v.door_open[1], 1e-6f), "doors did not begin synchronized travel");
 float reversing = v.door_open[0];
 vehicle_toggle_doors(&v);
 run_steps(&v, (VehicleInput){0}, 30);
 CHECK(v.door_open[0] < reversing, "second Meta+O did not reverse half-travel doors");
 vehicle_toggle_doors(&v);
 run_steps(&v, (VehicleInput){0}, 240);
 CHECK(nearf(v.door_open[0], 1.0f, 1e-6f) && nearf(v.door_open[1], 1.0f, 1e-6f),
       "doors never reached the fully open detent");
 for (int side = -1; side <= 1; side += 2) {
  Vec3 pivot = vehicle_door_pivot(side);
  Vec3 closed = vehicle_canopy_point(0.30f, 0.35f, side, NULL);
  Vec3 opened = vehicle_door_point(&v, side, closed);
  float closed_radius = sqrtf(vdot(vsub(closed, pivot), vsub(closed, pivot)));
  float opened_radius = sqrtf(vdot(vsub(opened, pivot), vsub(opened, pivot)));
  CHECK(nearf(closed_radius, opened_radius, 2e-4f), "door glass did not rotate rigidly");
  CHECK(nearf(vehicle_door_angle(&v, side), side * VEHICLE_DOOR_MAX_ANGLE, 1e-4f),
        "fully open hinge angle is wrong on side %d", side);
  CHECK(side < 0 ? opened.x < closed.x : opened.x > closed.x,
        "door opened into the body instead of outward on side %d", side);
  Vec3 rail = vehicle_canopy_point(0.30f, VEHICLE_DOOR_CANOPY_V1 - 0.001f, side, NULL);
  Vec3 rail_opened = vehicle_door_point(&v, side, rail);
  float rail_radius = sqrtf(vdot(vsub(rail, pivot), vsub(rail, pivot)));
  float opened_rail_radius = sqrtf(vdot(vsub(rail_opened, pivot), vsub(rail_opened, pivot)));
  CHECK(nearf(rail_radius, opened_rail_radius, 2e-4f)
        && (side < 0 ? rail_opened.x < rail.x : rail_opened.x > rail.x),
        "door roof rail did not rotate rigidly outward on side %d", side);
 }
 Vec3 closed_extent = vehicle_collision_extents(v.yaw);
 Vec3 open_extent = vehicle_collision_extents_for_vehicle(&v);
 CHECK(open_extent.x > closed_extent.x + 0.25f && open_extent.z >= closed_extent.z,
       "open door assemblies were not included in collision bounds");
 vehicle_toggle_doors(&v);
 run_steps(&v, (VehicleInput){0}, 240);
 CHECK(v.door_open[0] == 0.0f && v.door_open[1] == 0.0f, "doors did not close exactly");
 vehicle_toggle_doors(&v);run_steps(&v, (VehicleInput){0}, 240);vehicle_reset_pose(&v);
 CHECK(v.door_open[0] == 0.0f && v.door_target[0] == 0.0f
       && v.door_open[1] == 0.0f && v.door_target[1] == 0.0f,
       "F10 recovery did not close and latch both doors");

 /* Door expansion is a collision event even without center translation. The
  * resolver must move the rig inward rather than letting trim cross a wall. */
 vehicle_reset_pose(&v); v.position.x = 10.5f; vehicle_toggle_doors(&v);
 bool door_wall_contact = false;
 for (int i = 0; i < 240; i++) {
  vehicle_step(&v, (VehicleInput){0}, (float)VEHICLE_FIXED_DT);
  door_wall_contact |= v.collision;
 }
 Vec3 door_wall_extent = vehicle_collision_extents_for_vehicle(&v);
 CHECK(door_wall_contact && v.position.x + door_wall_extent.x <= 14.7001f,
       "opening door crossed the home wall: x=%f extent=%f", v.position.x, door_wall_extent.x);
 vehicle_reset_pose(&v);

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
  CHECK(sec.width <= VEHICLE_COLLISION_HALF_WIDTH
        && sec.z >= -VEHICLE_COLLISION_HALF_LENGTH && sec.z <= VEHICLE_COLLISION_HALF_LENGTH,
        "visible hull exceeds collision proxy at u=%f", u);
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
  {-1.18f, -0.25f, -2.75f}, { 1.25f,  0.74f,  2.95f - VEHICLE_COCKPIT_FORWARD}, /* 12 V battery & sport seat head */
  { 1.50f,  1.04f,  3.02f - VEHICLE_COCKPIT_FORWARD}, {-1.15f,  1.04f,  3.02f - VEHICLE_COCKPIT_FORWARD}, /* detailed headrest outer crowns */
  { 0.00f, -0.72f,  1.70f - VEHICLE_COCKPIT_FORWARD}, { 0.66f, -0.80f,  1.02f - VEHICLE_COCKPIT_FORWARD}  /* lower dashboard & shifter console */
 };
 for (size_t i = 0; i < sizeof(internal_parts) / sizeof(internal_parts[0]); i++) {
  CHECK(vehicle_point_inside_envelope(internal_parts[i], 0.02f),
        "EV component %zu protrudes outside hull envelope: (%f, %f, %f)",
        i, internal_parts[i].x, internal_parts[i].y, internal_parts[i].z);
 }

 for (int seat = VEHICLE_SEAT_DRIVER; seat <= VEHICLE_SEAT_PASSENGER; seat++) {
  Vec3 anchor = vehicle_seat_anchor((VehicleSeat)seat);
  Vec3 eye = vehicle_seat_eye((VehicleSeat)seat);
  float door_rear_z = vehicle_hull_point(VEHICLE_DOOR_U1, 0.55f, 1, NULL).z;
  CHECK(nearf(anchor.x, eye.x, 1e-5f) && nearf(anchor.z, -VEHICLE_COCKPIT_FORWARD, 1e-5f)
        && eye.z < anchor.z + 2.82f && eye.z > anchor.z + 2.0f
        && eye.z < door_rear_z + 0.50f && eye.y > 0.65f && eye.y < 1.04f,
        "seat %d eye is not in the forward cockpit beside the door aperture", seat);
  CHECK(vehicle_point_inside_envelope(eye, 0.02f),
        "seat %d eye lies outside the shared canopy", seat);
 }

 /* Every part of the shared interior door assembly must fit the actual
  * bicubic skin seen outside/cutaway, not merely the maximum-width envelope. */
 for (int side = -1; side <= 1; side += 2) {
  for (int ix = -1; ix <= 1; ix += 2) for (int iy = -1; iy <= 1; iy += 2)
   for (int iz = -1; iz <= 1; iz += 2) {
    Vec3 locals[] = {
     v3(ix * 0.28f, 0.35f + iy * 0.55f, iz * 0.075f), /* upholstered panel */
     v3(ix * 0.28f, 0.29f + iy * 0.05f, 0.12f + iz * 0.06f), /* armrest */
     v3(ix * 0.242f, 0.57f + iy * 0.022f, 0.14f + iz * 0.022f) /* pull */
    };
    for (size_t part = 0; part < sizeof(locals) / sizeof(locals[0]); part++) {
     Vec3 p = vadd(v3(side * 2.00f, -0.39f, 0.64f - VEHICLE_COCKPIT_FORWARD),
                   rotate_y(locals[part], side * -40.0f));
     CHECK(vehicle_point_inside_envelope(p, 0.0f) && fabsf(p.x) <= hull_side_limit(p) + 1e-4f,
           "door part %zu protrudes: side=%d corner=(%f,%f,%f) skin_x=%f",
           part, side, p.x, p.y, p.z, hull_side_limit(p));
    }
   }
  /* Corners alone miss a concave scoop crossing the middle of a panel face.
   * Sample the front, centre and back surfaces across the complete upholstery. */
  for (int ix = 0; ix <= 14; ix++) for (int iy = 0; iy <= 24; iy++)
   for (int iz = 0; iz <= 2; iz++) {
    Vec3 local = v3(-0.28f + 0.56f * ix / 14.0f,
                    -0.20f + 1.10f * iy / 24.0f,
                    -0.075f + 0.15f * iz / 2.0f);
    Vec3 p = vadd(v3(side * 2.00f, -0.39f, 0.64f - VEHICLE_COCKPIT_FORWARD), rotate_y(local, side * -40.0f));
    CHECK(fabsf(p.x) <= hull_side_limit(p) + 1e-4f,
          "door face crosses scoop: side=%d point=(%f,%f,%f) skin_x=%f",
          side, p.x, p.y, p.z, hull_side_limit(p));
   }
  /* Sample the complete speaker surround and cone, including their depth. */
  for (int i = 0; i < 32; i++) for (int iz = -1; iz <= 1; iz += 2) {
   float a = i * 6.28318530718f / 32.0f;
   Vec3 locals[] = {
    v3(0.234f * cosf(a), 0.03f + 0.234f * sinf(a), 0.15f + iz * 0.024f),
    v3(0.190f * cosf(a), 0.03f + 0.190f * sinf(a), 0.09f)
   };
   for (size_t part = 0; part < 2; part++) {
    Vec3 p = vadd(v3(side * 2.00f, -0.39f, 0.64f - VEHICLE_COCKPIT_FORWARD),
                  rotate_y(locals[part], side * -40.0f));
    CHECK(fabsf(p.x) <= hull_side_limit(p) + 1e-4f,
          "door speaker protrudes: side=%d point=(%f,%f,%f) skin_x=%f",
          side, p.x, p.y, p.z, hull_side_limit(p));
   }
  }
 }

 /* All nine Wayland outputs are physical car-part displays, not a separate
  * exterior monitor ring. Check deployed, stowed and mid-animation corners,
  * together with the car-mounted telescopic manipulator joints. */
 for (int slot = 0; slot < 9; slot++) for (int pose = 0; pose <= 2; pose++) {
  Plane plane = monitor_plane_at(slot, pose * 0.5f);
  Vec3 right = rotate_y(v3(1, 0, 0), plane.yaw);
  Vec3 up = rotate_y(rotate_x(v3(0, 1, 0), plane.pitch), plane.yaw);
  for (int ix = -1; ix <= 1; ix += 2) for (int iy = -1; iy <= 1; iy += 2) {
   Vec3 p = vadd(plane.center, vadd(vmul(right, ix * plane.width * 0.5f),
                                    vmul(up, iy * plane.height * 0.5f)));
   CHECK(vehicle_point_inside_envelope(p, 0.0f),
         "monitor %d pose %d corner outside shared body: (%f,%f,%f)",
         slot, pose, p.x, p.y, p.z);
  }
  if (slot < 8) {
   MonitorManipulator m = monitor_manipulator_at(slot, pose * 0.5f);
   Vec3 joints[] = {m.base, m.shoulder, m.elbow, m.wrist, m.piston_base, m.piston_rod};
   for (size_t j = 0; j < sizeof(joints) / sizeof(joints[0]); j++) {
    CHECK(vehicle_point_inside_envelope(joints[j], 0.0f),
          "manipulator %d pose %d joint %zu outside shared body: (%f,%f,%f)",
          slot, pose, j, joints[j].x, joints[j].y, joints[j].z);
   }
  }
 }

 /* The live windshield HUD is a physical projection too. Its fitted corners
  * must remain inside the lower forward windshield, not be clipped by it. */
 Plane hud = windshield_hud_plane();Vec3 hud_right, hud_up;
 plane_axes(hud, &hud_right, &hud_up, NULL);
 for (int ix = -1; ix <= 1; ix += 2) for (int iy = -1; iy <= 1; iy += 2) {
  Vec3 p = vadd(hud.center, vadd(vmul(hud_right, ix * hud.width * 0.5f),
                                vmul(hud_up, iy * hud.height * 0.5f)));
  CHECK(vehicle_point_inside_envelope(p, 0.0f),
        "windshield HUD corner outside forward canopy: (%f,%f,%f)", p.x, p.y, p.z);
 }

 if (failures) {
  fprintf(stderr, "%d vehicle checks failed\n", failures);
  return 1;
 }
 puts("PASS: Catmull-Rom splines, hover EV dynamics, reversible rigid doors, collision bounds, seat/rig transforms, fixed-step determinism");
 return 0;
}
