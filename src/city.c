#include "city.h"

#include <math.h>
#include <stddef.h>

#define CITY_DEG (180.0f / 3.14159265358979323846f)

static const CityBuilding buildings[] = {
 {-110,-145,28,24,46,0},{-32,-145,26,24,28,1},{32,-145,24,24,58,2},{110,-145,30,24,36,3},
 {-110,-94,26,18,32,1},{-32,-94,24,18,52,2},{32,-94,24,18,30,0},{110,-94,28,18,64,3},
 {-110,-42,26,20,24,2},{-32,-42,22,20,38,0},{32,-42,22,20,45,1},{110,-42,28,20,30,3},
 {-110,25,28,26,42,0},{-32,25,24,24,26,2},{32,25,24,24,34,1},{110,25,30,26,55,0},
};

static const CityGarage garages[] = {
 {0,-12,30,56,1,"CHROMEDRIVE / HOME"},
 {-65,-95,24,24,1,"NEON WORKS / WEST"},
 {0,-115,34,24,0,"SKYLINE LAB / NORTH"},
 {65,-95,24,24,1,"HARBOR SERVICE / EAST"},
};

typedef struct { float x,z,speed; const char *destination; } RoutePoint;
static const RoutePoint route[] = {
 {0,-24,8,"HOME EXIT"},
 {0,-52,12,"CITY LOOP"},
 {0,-70,8,"NEON WORKS / WEST"},
 {-65,-70,10,"NEON WORKS / WEST"},
 {-65,-95,5,"NEON WORKS / WEST"},
 {-65,-115,8,"SKYLINE LAB / NORTH"},
 {0,-115,5,"SKYLINE LAB / NORTH"},
 {65,-115,9,"HARBOR SERVICE / EAST"},
 {65,-95,5,"HARBOR SERVICE / EAST"},
 {65,-70,8,"CHROMEDRIVE / HOME"},
 {0,-70,10,"CHROMEDRIVE / HOME"},
 {0,-52,10,"CHROMEDRIVE / HOME"},
 {0,-24,7,"CHROMEDRIVE / HOME"},
 {0,0,4,"CHROMEDRIVE / HOME"},
};
#define ROUTE_COUNT ((int)(sizeof(route)/sizeof(route[0])))

int city_building_count(void){return (int)(sizeof(buildings)/sizeof(buildings[0]));}
CityBuilding city_building(int index){
 if(index<0)index=0;if(index>=city_building_count())index=city_building_count()-1;
 return buildings[index];
}
int city_garage_count(void){return (int)(sizeof(garages)/sizeof(garages[0]));}
CityGarage city_garage(int index){
 if(index<0)index=0;if(index>=city_garage_count())index=city_garage_count()-1;
 return garages[index];
}
const char *city_garage_at(Vec3 p){
 for(int i=0;i<city_garage_count();i++){
  CityGarage g=garages[i];
  if(fabsf(p.x-g.x)<=g.width*.5f&&fabsf(p.z-g.z)<=g.depth*.5f)return g.name;
 }
 return NULL;
}
int city_nearest_route_point(Vec3 p){
 int best=0;float best_d2=1e30f;
 for(int i=0;i<ROUTE_COUNT;i++){
  float dx=p.x-route[i].x,dz=p.z-route[i].z,d2=dx*dx+dz*dz;
  if(d2<best_d2){best_d2=d2;best=i;}
 }
 /* The last point is the home parking stop, not a turnaround target. Starting
  * another run while parked there should head out through the door again. */
 return best==ROUTE_COUNT-1?0:best;
}
const char *city_autopilot_destination(const Vehicle *v){
 if(!v->autopilot_enabled)return "CITY LOOP / STANDBY";
 int i=v->autopilot_waypoint;if(i<0||i>=ROUTE_COUNT)i=0;
 return route[i].destination;
}

static float wrap_angle(float a){
 while(a>180)a-=360;while(a<-180)a+=360;return a;
}
VehicleInput city_autopilot_input(Vehicle *v,float dt){
 VehicleInput input={.brake=1};
 if(!v->autopilot_enabled)return input;
 /* Door interlock: engaging autopilot requests closure in vehicle.c, and the
  * route remains parked until both physical assemblies have finished. */
 if(v->door_open[0]>.005f||v->door_open[1]>.005f)return input;
 int i=v->autopilot_waypoint;if(i<0||i>=ROUTE_COUNT)i=city_nearest_route_point(v->position);
 float dx=route[i].x-v->position.x,dz=route[i].z-v->position.z;
 float distance=sqrtf(dx*dx+dz*dz);
 if(distance<3.4f){
  if(v->autopilot_reverse&&i==2)v->autopilot_reverse=false;
  if(i==ROUTE_COUNT-1){
   /* Home is a terminus. Brake to a stop instead of attempting a 180-degree
    * turn inside the original enclosed garage. Meta+A starts another run. */
   v->autopilot_enabled=false;return input;
  }
  i++;v->autopilot_waypoint=i;
  dx=route[i].x-v->position.x;dz=route[i].z-v->position.z;
  distance=sqrtf(dx*dx+dz*dz);
 }
 float desired_yaw=atan2f(v->autopilot_reverse?dx:-dx,v->autopilot_reverse?dz:-dz)*CITY_DEG;
 float error=wrap_angle(desired_yaw-v->yaw);
 /* The bicycle model's positive steering input produces negative yaw rate;
  * reversing flips that relationship again. Slow for curvature and arrival
  * instead of feeding constant throttle and hoping the body makes the corner. */
 float steer_scale=clampf(13.f/fmaxf(7.f,fabsf(v->speed)),.42f,1.f);
 input.steering=clampf((v->autopilot_reverse?error:-error)/32.f,-1.f,1.f)*steer_scale;
 float target=route[i].speed*clampf(1.f-fabsf(error)/135.f,.20f,1.f);
 target*=clampf(distance/11.f,.18f,1.f);if(v->autopilot_reverse)target=-target;
 float speed_error=target-v->speed;
 if(v->autopilot_reverse){
  input.throttle=clampf(speed_error*.24f,-1.f,0.f);
  input.brake=clampf(speed_error*.30f,0.f,1.f);
 }else{
  input.throttle=clampf(speed_error*.24f,0.f,1.f);
  input.brake=clampf(-speed_error*.30f,0.f,1.f);
 }
 return input;
}

typedef struct { float x,z,w,d; } Obstacle;
static bool inside_expanded(Vec3 p,Vec3 e,Obstacle o){
 return fabsf(p.x-o.x)<o.w*.5f+e.x&&fabsf(p.z-o.z)<o.d*.5f+e.z;
}
static bool collide_obstacle(Vehicle *v,Vec3 previous,Vec3 extent,Obstacle o){
 /* Reject a new penetration with the last safe pose. If the previous center
  * is already inside the *current* expanded box, the rig may instead be
  * opening a door near a wall: push it clear along the nearest face. A pose
  * that is actively reducing its overlap is allowed to escape. */
 if(!inside_expanded(v->position,extent,o))return false;
 if(inside_expanded(previous,extent,o)){
  float cx=o.w*.5f+extent.x-fabsf(v->position.x-o.x),cz=o.d*.5f+extent.z-fabsf(v->position.z-o.z);
  float px=o.w*.5f+extent.x-fabsf(previous.x-o.x),pz=o.d*.5f+extent.z-fabsf(previous.z-o.z);
  if(fminf(cx,cz)<fminf(px,pz)-1e-5f)return false;
  if(cx<cz){float side=previous.x-o.x<0?-1.f:1.f;v->position.x=o.x+side*(o.w*.5f+extent.x);}
  else{float side=previous.z-o.z<0?-1.f:1.f;v->position.z=o.z+side*(o.d*.5f+extent.z);}
 }else v->position=previous;
 v->speed*=.18f;v->collision=true;return true;
}
void city_constrain_vehicle(Vehicle *v,Vec3 previous){
 v->collision=false;
 Vec3 extent=vehicle_collision_extents_for_vehicle(v);
 static const Obstacle home[]={
  {-15,-12,.6f,56},{15,-12,.6f,56},{0,16,30,.6f},
  {-10.75f,-40,8.5f,.6f},{10.75f,-40,8.5f,.6f},
 };
 for(unsigned i=0;i<sizeof(home)/sizeof(home[0]);i++)if(collide_obstacle(v,previous,extent,home[i]))return;
 for(int i=0;i<city_building_count();i++){
  CityBuilding b=buildings[i];Obstacle o={b.x,b.z,b.width+2,b.depth+2}; /* visible ground-floor podium */
  if(collide_obstacle(v,previous,extent,o))return;
 }
 /* Other garages are drive-through: only their two side walls are obstacles.
  * There is deliberately no invisible front/back wall across the route. */
 for(int i=1;i<city_garage_count();i++){
  CityGarage g=garages[i];Obstacle walls[2];
  if(g.axis==1){
   walls[0]=(Obstacle){g.x-g.width*.5f,g.z,.6f,g.depth};
   walls[1]=(Obstacle){g.x+g.width*.5f,g.z,.6f,g.depth};
  }else{
   walls[0]=(Obstacle){g.x,g.z-g.depth*.5f,g.width,.6f};
   walls[1]=(Obstacle){g.x,g.z+g.depth*.5f,g.width,.6f};
  }
  for(int w=0;w<2;w++)if(collide_obstacle(v,previous,extent,walls[w]))return;
 }
 float old_x=v->position.x,old_z=v->position.z;
 v->position.x=clampf(v->position.x,-170.f+extent.x,170.f-extent.x);
 v->position.z=clampf(v->position.z,-235.f+extent.z,105.f-extent.z);
 if(old_x!=v->position.x||old_z!=v->position.z){v->speed*=.18f;v->collision=true;}
}
