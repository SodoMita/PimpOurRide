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

typedef struct { float x,z,speed; const char *destination; bool precise; } RoutePoint;
/* The tour uses the inner service loop. Ambient through-traffic uses a real
 * outer ring and separate avenue lanes. Skyline is drive-through along X, so
 * its through-lane is offset from the service lane and clears both side walls.
 * The home junction is the one shared crossing; prediction also covers manual
 * departures and any approach to the shared avenues. */
static const RoutePoint route[] = {
 {0,-24,6,"HOME EXIT",false},
 {0,-48,6,"CITY LOOP",false},
 {0,-76.0f,5,"HARBOR SERVICE / EAST",false},
 {59.0f,-76.0f,6,"HARBOR SERVICE / EAST",false},
 {65.0f,-76.0f,4,"HARBOR SERVICE / EAST",true}, /* align before the side walls */
 {65.0f,-95,4,"HARBOR SERVICE / EAST",false},
 {65.0f,-112.0f,5,"SKYLINE LAB / NORTH",true},  /* straight out before turning */
 {0,-109.0f,4,"SKYLINE LAB / NORTH",false},
 {-65.0f,-112.0f,5,"NEON WORKS / WEST",true},
 {-65.0f,-95,4,"NEON WORKS / WEST",false},
 {-65.0f,-80.0f,5,"CHROMEDRIVE / HOME",true},
 {-59.0f,-76.0f,6,"CHROMEDRIVE / HOME",false},
 {0,-76.0f,6,"CHROMEDRIVE / HOME",false},
 {0,-48,6,"CHROMEDRIVE / HOME",false},
 {0,-24,5,"CHROMEDRIVE / HOME",false},
 {0,0,3,"CHROMEDRIVE / HOME",false},
};
#define ROUTE_COUNT ((int)(sizeof(route)/sizeof(route[0])))

int city_traffic_count(void){return CITY_TRAFFIC_COUNT;}
CityTrafficCar city_traffic_at(int index,double time){
 if(index<0)index=0;if(index>=CITY_TRAFFIC_COUNT)index=CITY_TRAFFIC_COUNT-1;
 const double span_x=300.0,span_z=64.0,perimeter=2.0*(span_x+span_z),speed=6.0;
 double d=fmod(time*speed+index*perimeter/CITY_TRAFFIC_COUNT,perimeter);
 if(d<0)d+=perimeter;
 CityTrafficCar car={.speed=(float)speed};
 if(d<span_x){car.position=v3(-150.0f+(float)d,0,-60.0f);car.yaw=-90;}
 else if(d<span_x+span_z){car.position=v3(150.0f,0,-60.0f-(float)(d-span_x));car.yaw=0;}
 else if(d<2*span_x+span_z){car.position=v3(150.0f-(float)(d-span_x-span_z),0,-124.0f);car.yaw=90;}
 else{car.position=v3(-150.0f,0,-124.0f+(float)(d-2*span_x-span_z));car.yaw=180;}
 return car;
}
Vec3 city_traffic_collision_extents(float yaw){
 float c=fabsf(cosf(yaw*(3.14159265358979323846f/180.0f)));
 float s=fabsf(sinf(yaw*(3.14159265358979323846f/180.0f)));
 return v3(c*CITY_TRAFFIC_HALF_WIDTH+s*CITY_TRAFFIC_HALF_LENGTH,0,
           s*CITY_TRAFFIC_HALF_WIDTH+c*CITY_TRAFFIC_HALF_LENGTH);
}

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

typedef struct { Vec3 position; float yaw; bool reverse,turning; } RouteProbe;
/* Project along the same waypoint polyline the controller is following. This
 * is deliberately not a second vehicle integrator: it is only a short-horizon
 * occupancy query used to decide whether the real fixed-step car may proceed. */
static RouteProbe route_probe(const Vehicle *v,int waypoint,float travel,bool turning){
 int wi=waypoint<0?0:waypoint>=ROUTE_COUNT?ROUTE_COUNT-1:waypoint;
 float wx=route[wi].x-v->position.x;
 float wz=route[wi].z-v->position.z;
 float turn_span=turning?fminf(20.0f,sqrtf(wx*wx+wz*wz)+6.0f):0;
 RouteProbe p={.position=v->position,.yaw=v->yaw,.reverse=v->autopilot_reverse,.turning=turning&&travel<=turn_span};
 int i=waypoint;
 for(int guard=0;guard<ROUTE_COUNT+1&&travel>1e-5f;guard++){
  if(i<0||i>=ROUTE_COUNT)break;
  float dx=route[i].x-p.position.x,dz=route[i].z-p.position.z;
  float length=sqrtf(dx*dx+dz*dz);
  if(length>1e-4f){
   p.yaw=atan2f(p.reverse?dx:-dx,p.reverse?dz:-dz)*CITY_DEG;
   if(travel<=length){
    p.position.x+=dx*travel/length;p.position.z+=dz*travel/length;
    return p;
   }
   travel-=length;p.position.x=route[i].x;p.position.z=route[i].z;
  }
  if(p.reverse&&i==2)p.reverse=false;
  if(i==ROUTE_COUNT-1)break;
  i++;
 }
 return p;
}
static bool traffic_overlap(Vec3 p,float yaw,bool turning,CityTrafficCar car,float margin){
 Vec3 a=vehicle_collision_extents(yaw),b=city_traffic_collision_extents(car.yaw);
 if(turning){float swept=fmaxf(a.x,a.z);a.x=a.z=swept;}
 return fabsf(p.x-car.position.x)<a.x+b.x+margin&&
        fabsf(p.z-car.position.z)<a.z+b.z+margin;
}
static float traffic_speed_limit(const Vehicle *v,int waypoint,float requested,bool turning,bool clearing){
 /* Once a bend is clear, do not crawl through it below the ambient traffic
  * gap. Precise garage alignment deliberately does not use this minimum. */
 float proceed=clearing?fmaxf(requested,5.0f):requested;
 float probe_speed=fmaxf(fabsf(v->speed),proceed);
 const float horizon=6.0f;
 float earliest=horizon+1.0f,turn_earliest=horizon+1.0f,stationary_earliest=horizon+1.0f;
 /* A stop line is useful only if the stopped silhouette stays clear. If our
  * rear is already in a crossing, braking would create the very contact we
  * are trying to avoid; in that case the controller must clear the junction. */
 for(float t=0;t<=horizon+1e-4f;t+=.20f){
  for(int car=0;car<CITY_TRAFFIC_COUNT;car++){
   if(traffic_overlap(v->position,v->yaw,false,city_traffic_at(car,v->simulation_time+t),0.0f)){
    stationary_earliest=t;goto waiting_checked;
   }
  }
 }
waiting_checked:
 /* A 1 m safety envelope and 200 ms sampling cover both current lane
  * occupancy and future intersection crossings. The render pose at the end
  * of this tick uses this same city_traffic_at function and simulation clock. */
 for(float t=0;t<=horizon+1e-4f;t+=.20f){
  RouteProbe p=route_probe(v,waypoint,probe_speed*t,turning);
  for(int car=0;car<CITY_TRAFFIC_COUNT;car++){
   if(traffic_overlap(p.position,p.yaw,p.turning,city_traffic_at(car,v->simulation_time+t),1.5f)){
    if(t<earliest)earliest=t;
    if(p.turning&&t<turn_earliest)turn_earliest=t;
    break;
   }
  }
 }
 if(earliest>horizon)return proceed;
 if(stationary_earliest<=horizon&&earliest>stationary_earliest+.35f)return fmaxf(proceed,2.0f);
 /* A conflict inside the swept corner is an all-or-nothing yield. A later
  * straight-road conflict must not hold the corner hostage: it uses the
  * graduated limit below instead. */
 if(turn_earliest<=horizon)return 0;
 float stopping_time=fabsf(v->speed)/4.2f;
 float hold_window=stopping_time+3.0f;
 if(earliest<=hold_window)return 0;
 return fminf(proceed,fmaxf(0.0f,(earliest-.90f)*1.7f));
}

static float crossing_travel(const Vehicle *v,float t,float acceleration){
 const float cruise=5.0f;
 float start=clampf(fabsf(v->speed),0,cruise),ramp=(cruise-start)/acceleration;
 if(t<=ramp)return start*t+.5f*acceleration*t*t;
 return start*ramp+.5f*acceleration*ramp*ramp+cruise*(t-ramp);
}
static bool home_crossing_profile_clear(const Vehicle *v,int waypoint,int crossing,float acceleration){
 /* Test the whole maneuver to the far safe side, not merely the next few
  * metres. Every pose comes from the same route/traffic functions. */
 for(float t=0;t<=9.0f;t+=.10f){
  RouteProbe p=route_probe(v,waypoint,crossing_travel(v,t,acceleration),true);
  for(int car=0;car<CITY_TRAFFIC_COUNT;car++)
   if(traffic_overlap(p.position,p.yaw,p.turning,city_traffic_at(car,v->simulation_time+t),1.5f))return false;
  if(crossing==1&&p.position.z<-70.0f)return true;
  if(crossing==2&&p.position.z>-50.0f)return true;
 }
 return false;
}
static bool home_crossing_clear(const Vehicle *v,int waypoint,int crossing){
 /* Smoothed throttle can accelerate the real car faster or slower than a
  * single nominal profile. Both ends of this envelope must clear, otherwise
  * a safe slow forecast can still release an early fast car into traffic. */
 return home_crossing_profile_clear(v,waypoint,crossing,3.0f)&&
        home_crossing_profile_clear(v,waypoint,crossing,6.0f);
}
VehicleInput city_autopilot_input(Vehicle *v,float dt){
 VehicleInput input={.brake=1};
 v->autopilot_yield=false;
 if(!v->autopilot_enabled)return input;
 /* Door interlock: engaging autopilot requests closure in vehicle.c, and the
  * route remains parked until both physical assemblies have finished. */
 if(v->door_open[0]>.005f||v->door_open[1]>.005f)return input;
 int i=v->autopilot_waypoint;if(i<0||i>=ROUTE_COUNT)i=city_nearest_route_point(v->position);
 if(v->autopilot_crossing==1&&v->position.z<-70.0f)v->autopilot_crossing=0;
 if(v->autopilot_crossing==2&&v->position.z>-50.0f)v->autopilot_crossing=0;
 float dx=route[i].x-v->position.x,dz=route[i].z-v->position.z;
 float distance=sqrtf(dx*dx+dz*dz);
 if(!v->autopilot_crossing&&(i==1||(i==12&&!v->autopilot_reverse))&&distance<8.0f){
  /* Nose-in restart reverses through the same home junction and therefore
   * uses the same all-or-nothing exit reservation as a forward first exit. */
  int crossing=i==1?1:2;
  if(home_crossing_clear(v,i,crossing))v->autopilot_crossing=crossing;
  else{v->autopilot_yield=true;input.throttle=0;input.brake=1;return input;}
 }
 float arrival=i==ROUTE_COUNT-1?3.4f:route[i].precise?1.0f:2.5f;
 if(distance<arrival){
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
 float error=wrap_angle(desired_yaw-v->yaw),steer_error=error;
 bool turning=fabsf(error)>12.0f;
 if(i+1<ROUTE_COUNT&&distance<14.0f){
  float nx=route[i+1].x-route[i].x,nz=route[i+1].z-route[i].z;
  float next_yaw=atan2f(v->autopilot_reverse?nx:-nx,v->autopilot_reverse?nz:-nz)*CITY_DEG;
  float bend=wrap_angle(next_yaw-desired_yaw);
  turning|=!route[i].precise&&fabsf(bend)>25.0f;
  /* Begin steering before the waypoint, not after the nose has crossed it.
   * The target/arrival test is unchanged; only the steering command receives
   * this bounded anticipation, reducing outer-lane overshoot on 90° bends. */
  if(fabsf(bend)>25.0f&&!route[i].precise){
   float anticipate=clampf((12.0f-distance)/12.0f,0,1)*.55f;
   steer_error=wrap_angle(error+bend*anticipate);
  }
 }
 /* The bicycle model's positive steering input produces negative yaw rate;
  * reversing flips that relationship again. Slow for curvature and arrival
  * instead of feeding constant throttle and hoping the body makes the corner. */
 float steer_scale=clampf(13.f/fmaxf(7.f,fabsf(v->speed)),.42f,1.f);
 input.steering=clampf((v->autopilot_reverse?steer_error:-steer_error)/32.f,-1.f,1.f)*steer_scale;
 float target=route[i].speed*clampf(1.f-fabsf(steer_error)/135.f,.20f,1.f);
 target*=clampf(distance/11.f,.18f,1.f);
 float requested=fabsf(target);
 float allowed=v->autopilot_crossing?fmaxf(requested,5.0f):traffic_speed_limit(v,i,requested,turning,turning&&!route[i].precise);
 v->autopilot_yield=allowed+.02f<requested;
 target=(v->autopilot_reverse?-1.f:1.f)*allowed;
 float speed_error=target-v->speed;
 if(allowed<.05f&&requested>.05f){
  input.throttle=0;input.brake=1;return input;
 }
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
 }else v->position=previous;
 /* A moving car can occupy the previous safe ego pose. Restore first, then
  * push out of the *current* shared silhouette if necessary; never leave a
  * one-tick visible interpenetration as the collision fallback. */
 if(inside_expanded(v->position,extent,o)){
  float cx=o.w*.5f+extent.x-fabsf(v->position.x-o.x),cz=o.d*.5f+extent.z-fabsf(v->position.z-o.z);
  if(cx<cz){float side=v->position.x-o.x<0?-1.f:1.f;v->position.x=o.x+side*(o.w*.5f+extent.x);}
  else{float side=v->position.z-o.z<0?-1.f:1.f;v->position.z=o.z+side*(o.d*.5f+extent.z);}
 }
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
 /* The exact poses drawn by city_scene.c are physical for manual and
  * automatic driving alike. Prediction should prevent contact; this resolver
  * is the final guard against a moving car entering an already safe pose. */
 for(int i=0;i<CITY_TRAFFIC_COUNT;i++){
  CityTrafficCar car=city_traffic_at(i,v->simulation_time);
  Vec3 e=city_traffic_collision_extents(car.yaw);
  Obstacle o={car.position.x,car.position.z,e.x*2,e.z*2};
  if(collide_obstacle(v,previous,extent,o))return;
 }
 float old_x=v->position.x,old_z=v->position.z;
 v->position.x=clampf(v->position.x,-170.f+extent.x,170.f-extent.x);
 v->position.z=clampf(v->position.z,-235.f+extent.z,105.f-extent.z);
 if(old_x!=v->position.x||old_z!=v->position.z){v->speed*=.18f;v->collision=true;}
}
