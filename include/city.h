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

/* Infinite city streaming. A chunk owns the roads on its minimum X/Z edges
 * and the land inside it. All values are pure functions of integer chunk
 * coordinates: no frame-rate-dependent random state, allocation, or finite
 * world origin is involved. The aligned home core remains hand-authored. */
#define CITY_STREAM_CHUNK_SIZE 80.0f
#define CITY_STREAM_RADIUS 2

typedef enum {
 CITY_DISTRICT_DOWNTOWN = 0,
 CITY_DISTRICT_RESIDENTIAL,
 CITY_DISTRICT_INDUSTRIAL,
 CITY_DISTRICT_PARK,
 CITY_DISTRICT_CIVIC,
 CITY_DISTRICT_SERVICE
} CityDistrict;
typedef enum {
 CITY_FEATURE_BLOCKS = 0,
 CITY_FEATURE_SQUARE,
 CITY_FEATURE_PARKING,
 CITY_FEATURE_PARK,
 CITY_FEATURE_GARAGES
} CityFeature;
typedef enum {
 CITY_ROAD_LOCAL = 0,
 CITY_ROAD_ALLEY,
 CITY_ROAD_AVENUE,
 CITY_ROAD_BOULEVARD,
 CITY_ROAD_TUNNEL
} CityRoadType;
typedef enum {
 CITY_SOLID_BUILDING = 0,
 CITY_SOLID_FOUNTAIN,
 CITY_SOLID_PARKED_CAR,
 CITY_SOLID_TREE,
 CITY_SOLID_KIOSK,
 CITY_SOLID_TUNNEL_WALL
} CitySolidKind;
typedef struct {
 float x,z,width,depth,height;
 int style;
 CitySolidKind kind;
} CitySolid;
typedef struct {
 int x,z;
 CityDistrict district;
 CityFeature feature;
} CityChunk;

CityChunk city_chunk(int x,int z);
CityChunk city_chunk_at(Vec3 position);
bool city_stream_chunk_is_core(CityChunk chunk);
CityDistrict city_district_at(Vec3 position);
const char *city_district_name(CityDistrict district);
const char *city_feature_name(CityFeature feature);
CityRoadType city_road_type(int edge,int segment,bool vertical);
float city_road_width(CityRoadType type);
bool city_stream_is_tunnel(Vec3 position);
int city_chunk_solid_count(CityChunk chunk);
CitySolid city_chunk_solid(CityChunk chunk,int index);
int city_chunk_garage_count(CityChunk chunk);
CityGarage city_chunk_garage(CityChunk chunk,int index);
int city_chunk_road_solid_count(CityChunk chunk);
CitySolid city_chunk_road_solid(CityChunk chunk,int index);
const char *city_stream_garage_at(Vec3 position);

/* Additional physical workshops around Home. These are deliberately separate
 * from the four autopilot destinations, so campus expansion cannot silently
 * change the validated tour's destination indices. */
int city_campus_garage_count(void);
CityGarage city_campus_garage(int index);

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
