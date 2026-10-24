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
