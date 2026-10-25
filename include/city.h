#ifndef CD_CITY_H
#define CD_CITY_H

#include "vehicle.h"

/* One data model owns the visible city and its physical boundaries. Renderer
 * code iterates these same buildings/garages; it does not keep a second set of
 * positions that could drift away from collision or navigation. */
typedef struct {
 float x,z,width,depth,height;
 int style;
} CityBuilding;

typedef struct {
 float x,z,width,depth;
 int axis; /* 0: drive-through along X, 1: along Z */
 const char *name;
} CityGarage;

/* Ambient cars are part of the simulation, not renderer-only decoration.
 * These dimensions cover the visible body and lamps; poses are evaluated
 * from the same fixed-step clock by rendering, control and collision. */
#define CITY_TRAFFIC_COUNT 8
#define CITY_TRAFFIC_HALF_WIDTH 1.75f
#define CITY_TRAFFIC_HALF_LENGTH 3.40f /* includes the front/rear lamp lenses at +/-3.37 m */
typedef struct {
 Vec3 position;
 float yaw,speed;
} CityTrafficCar;

int city_traffic_count(void);
CityTrafficCar city_traffic_at(int index,double simulation_time);
Vec3 city_traffic_collision_extents(float yaw_degrees);

int city_building_count(void);
CityBuilding city_building(int index);
int city_garage_count(void);
CityGarage city_garage(int index);
const char *city_garage_at(Vec3 position);
const char *city_autopilot_destination(const Vehicle *v);
int city_nearest_route_point(Vec3 position);

/* Called by the fixed-step vehicle integrator. Both functions are pure C and
 * intentionally have no GL dependency, so tests exercise the real route and
 * collision behavior without creating a compositor or graphics context. */
VehicleInput city_autopilot_input(Vehicle *v,float dt);
void city_constrain_vehicle(Vehicle *v,Vec3 previous);

#endif
