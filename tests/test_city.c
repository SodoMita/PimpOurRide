#include "city.h"

#include <math.h>
#include <stdio.h>

static int failures;
#define CHECK(condition, ...) do { if (!(condition)) { \
 fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
 fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); failures++; \
} } while (0)

static int same_vehicle_model(CityVehicleModel a,CityVehicleModel b){
 return a.width==b.width&&a.length==b.length&&a.height==b.height
        &&a.cabin_height==b.cabin_height&&a.front_taper==b.front_taper
        &&a.rear_taper==b.rear_taper&&a.color[0]==b.color[0]
        &&a.color[1]==b.color[1]&&a.color[2]==b.color[2]&&a.wheels==b.wheels;
}
static float sampled_body_width(CityVehicleModel m){
 /* Independent dense oracle for the renderer's Catmull-Rom shoulder. This
  * catches spline overshoot that checking only control-point width misses. */
 float hw=m.width*.5f;
 float width[5]={hw*m.front_taper,hw*.92f,hw,hw*.90f,hw*m.rear_taper};
 float maximum=0;
 for(int segment=0;segment<4;segment++)for(int sample=0;sample<=64;sample++){
  int i1=segment,i2=segment+1,i0=i1>0?i1-1:0,i3=i2+1<4?i2+1:4;
  float w=vehicle_catmull_rom(width[i0],width[i1],width[i2],width[i3],sample/64.0f);
  if(fabsf(w)>maximum)maximum=fabsf(w);
 }
 return maximum*2;
}
static void check_vehicle_model(CityVehicleModel m,uint32_t seed){
 CHECK(isfinite(m.width)&&isfinite(m.length)&&isfinite(m.height)
       &&isfinite(m.cabin_height)&&isfinite(m.front_taper)&&isfinite(m.rear_taper)
       &&isfinite(m.color[0])&&isfinite(m.color[1])&&isfinite(m.color[2]),
       "non-finite spline-car field for seed %u",seed);
 CHECK(m.width>=2.65f&&m.width<=3.05f&&m.length>=6.15f&&m.length<=6.55f
       &&m.height>=1.20f&&m.height<=1.60f&&m.cabin_height>=.70f&&m.cabin_height<=.95f
       &&m.front_taper>=.20f&&m.front_taper<=.40f&&m.rear_taper>=.28f&&m.rear_taper<=.50f,
       "invalid spline-car dimensions for seed %u",seed);
 for(int c=0;c<3;c++)CHECK(m.color[c]>=0&&m.color[c]<=1,"invalid color for seed %u",seed);
 float width,length;city_vehicle_dimensions(m,&width,&length);
 float visible_width=sampled_body_width(m);
 if(m.wheels&&m.width+.20f>visible_width)visible_width=m.width+.20f;
 float visible_length=m.length+.098f; /* attached lamp outer faces */
 CHECK(width<=CITY_TRAFFIC_HALF_WIDTH*2&&length<=CITY_TRAFFIC_HALF_LENGTH*2,
       "vehicle escapes maximum traffic envelope for seed %u",seed);
 CHECK(width+1e-4f>=visible_width&&length+1e-4f>=visible_length,
       "physical footprint is smaller than the mesh for seed %u: %f/%f %f/%f",
       seed,width,visible_width,length,visible_length);
 CHECK(width-visible_width<=.12f&&length-visible_length<=.01f,
       "invisible collision gap is too large for seed %u: %f %f",
       seed,width-visible_width,length-visible_length);
}

static void check_road_clearance(CityChunk c,CitySolid s){
 for(int edge=c.x;edge<=c.x+1;edge++){
  float half=city_road_width(city_road_type(edge,c.z,true))*.5f;
  CHECK(fabsf(s.x-edge*CITY_STREAM_CHUNK_SIZE)>=half+s.width*.5f,
        "solid crosses vertical road: chunk=(%d,%d) kind=%d x=%f edge=%d",
        c.x,c.z,s.kind,s.x,edge);
 }
 for(int edge=c.z;edge<=c.z+1;edge++){
  float half=city_road_width(city_road_type(edge,c.x,false))*.5f;
  CHECK(fabsf(s.z-edge*CITY_STREAM_CHUNK_SIZE)>=half+s.depth*.5f,
        "solid crosses horizontal road: chunk=(%d,%d) kind=%d z=%f edge=%d",
        c.x,c.z,s.kind,s.z,edge);
 }
}

int main(void){
 bool districts[6]={0},features[5]={0},roads[5]={0};
 bool parked_wheeled=false,parked_hover=false;
 int core_chunks=0,checked_solids=0,checked_garages=0;
 for(int x=-32;x<=32;x++)for(int z=-32;z<=32;z++){
  CityChunk c=city_chunk(x,z),same=city_chunk(x,z);
  CHECK(c.district==same.district&&c.feature==same.feature,
        "chunk metadata is not deterministic at %d,%d",x,z);
  CHECK(c.district>=0&&c.district<=CITY_DISTRICT_SERVICE
        &&c.feature>=0&&c.feature<=CITY_FEATURE_GARAGES,
        "invalid generated metadata at %d,%d",x,z);
  districts[c.district]=true;features[c.feature]=true;
  if(city_stream_chunk_is_core(c)){
   core_chunks++;
   CHECK(city_chunk_solid_count(c)==0&&city_chunk_garage_count(c)==0
         &&city_chunk_road_solid_count(c)==0,
         "procedural solids leaked into aligned home core %d,%d",x,z);
   continue;
  }
  for(int i=0;i<city_chunk_solid_count(c);i++){
   CitySolid s=city_chunk_solid(c,i),repeat=city_chunk_solid(c,i);
   CHECK(s.x==repeat.x&&s.z==repeat.z&&s.width==repeat.width
         &&s.depth==repeat.depth&&s.height==repeat.height&&s.kind==repeat.kind
         &&s.vehicle_seed==repeat.vehicle_seed,
         "solid is not deterministic at chunk %d,%d index %d",x,z,i);
   CHECK(isfinite(s.x)&&isfinite(s.z)&&s.width>0&&s.depth>0&&s.height>0,
         "invalid solid at chunk %d,%d index %d",x,z,i);
   if(s.kind==CITY_SOLID_PARKED_CAR){
    CityVehicleModel m=city_vehicle_model(s.vehicle_seed);float width,length;
    city_vehicle_dimensions(m,&width,&length);
    CHECK(s.vehicle_seed==city_parked_vehicle_seed(c,i)
          &&fabsf(s.width-width)<1e-5f&&fabsf(s.depth-length)<1e-5f,
          "parked render seed/physical footprint diverged at %d,%d index %d",x,z,i);
    check_vehicle_model(m,s.vehicle_seed);
    parked_wheeled|=m.wheels;parked_hover|=!m.wheels;
   }
   check_road_clearance(c,s);checked_solids++;
  }
  for(int i=0;i<city_chunk_garage_count(c);i++){
   CityGarage g=city_chunk_garage(c,i);
   CitySolid bounds={.x=g.x,.z=g.z,.width=g.width,.depth=g.depth};
   check_road_clearance(c,bounds);checked_garages++;
   CHECK(city_stream_garage_at(v3(g.x,0,g.z))==g.name,
         "streamed garage is not addressable at %f,%f",g.x,g.z);
  }
 }
 for(int edge=-40;edge<=40;edge++)for(int segment=-40;segment<=40;segment++){
  roads[city_road_type(edge,segment,true)]=true;
  roads[city_road_type(edge,segment,false)]=true;
 }
 for(int i=0;i<6;i++)CHECK(districts[i],"district %d missing from generated city",i);
 for(int i=0;i<5;i++)CHECK(features[i],"land-use feature %d missing from generated city",i);
 for(int i=0;i<5;i++)CHECK(roads[i],"road type %d missing from generated city",i);
 CHECK(core_chunks==20,"aligned home core size changed: %d",core_chunks);
 CHECK(checked_solids>1000&&checked_garages>20,
       "insufficient generated solids/garages: %d %d",checked_solids,checked_garages);

 /* Explicit northern expedition landmarks and their physical/open road roles. */
 CHECK(city_chunk(0,-4).feature==CITY_FEATURE_SQUARE
       &&city_chunk(1,-4).feature==CITY_FEATURE_PARKING
       &&city_chunk(-1,-4).feature==CITY_FEATURE_PARK
       &&city_chunk(2,-4).feature==CITY_FEATURE_GARAGES,
       "northern civic/service landmarks are missing");
 CHECK(city_road_type(0,-4,true)==CITY_ROAD_TUNNEL
       &&city_stream_is_tunnel(v3(0,0,-280))
       &&!city_stream_is_tunnel(v3(40,0,-280)),
       "covered northern road and open square are confused");
 CityChunk tunnel=city_chunk(0,-4);
 CHECK(city_chunk_road_solid_count(tunnel)==2,
       "tunnel must have exactly two physical side walls");
 for(int i=0;i<2;i++){
  CitySolid wall=city_chunk_road_solid(tunnel,i);
  CHECK(wall.kind==CITY_SOLID_TUNNEL_WALL&&fabsf(wall.x)>10
        &&fabsf(wall.x)<13&&wall.depth==CITY_STREAM_CHUNK_SIZE-24.0f,
        "tunnel walls must leave 12 m intersection openings at both ends (wall %d)",i);
 }

 /* Far, negative and asymmetric chunks must be just as valid as the origin. */
 const int far_coords[][2]={{100000,-100000},{-100000,100000},{99991,100003},{-99991,-100003}};
 for(unsigned n=0;n<sizeof(far_coords)/sizeof(far_coords[0]);n++){
  CityChunk c=city_chunk(far_coords[n][0],far_coords[n][1]),same=city_chunk(far_coords[n][0],far_coords[n][1]);
  CHECK(c.x==same.x&&c.z==same.z&&c.district==same.district&&c.feature==same.feature,
        "far-world metadata is not repeatable at %d,%d",c.x,c.z);
  CityChunk at=city_chunk_at(v3(c.x*CITY_STREAM_CHUNK_SIZE+40,0,c.z*CITY_STREAM_CHUNK_SIZE+40));
  CHECK(at.x==c.x&&at.z==c.z&&at.district==c.district&&at.feature==c.feature,
        "far-world position/chunk lookup disagree at %d,%d",c.x,c.z);
  for(int i=0;i<city_chunk_solid_count(c);i++){
   CitySolid s=city_chunk_solid(c,i),repeat=city_chunk_solid(c,i);
   CHECK(isfinite(s.x)&&isfinite(s.z)&&isfinite(s.height)
         &&s.width>0&&s.depth>0&&s.height>0,
         "non-finite far-world solid at chunk %d,%d",c.x,c.z);
   CHECK(s.x==repeat.x&&s.z==repeat.z&&s.width==repeat.width&&s.depth==repeat.depth
         &&s.height==repeat.height&&s.kind==repeat.kind&&s.vehicle_seed==repeat.vehicle_seed,
         "far-world solid changed between visits at %d,%d index %d",c.x,c.z,i);
   if(s.kind==CITY_SOLID_PARKED_CAR){
    CHECK(s.vehicle_seed==city_parked_vehicle_seed(c,i)
          &&same_vehicle_model(city_vehicle_model(s.vehicle_seed),city_vehicle_model(s.vehicle_seed)),
          "far-world parked vehicle seed is not stable at %d,%d index %d",c.x,c.z,i);
    check_vehicle_model(city_vehicle_model(s.vehicle_seed),s.vehicle_seed);
   }
  }
 }

 /* Every descriptor field is bounded and diverse. Dense spline sampling and
  * the production parked/traffic seeds verify the actual mesh/physics contract. */
 bool wheeled=false,hover=false;unsigned varied=0;
 CityVehicleModel reference=city_vehicle_model(0);
 for(uint32_t seed=0;seed<64;seed++){
  CityVehicleModel m=city_vehicle_model(seed),repeat=city_vehicle_model(seed);
  CHECK(same_vehicle_model(m,repeat),"vehicle variant changed for seed %u",seed);
  check_vehicle_model(m,seed);
  wheeled|=m.wheels;hover|=!m.wheels;
  varied|=(fabsf(m.width-reference.width)>.01f)<<0;
  varied|=(fabsf(m.length-reference.length)>.01f)<<1;
  varied|=(fabsf(m.height-reference.height)>.01f)<<2;
  varied|=(fabsf(m.cabin_height-reference.cabin_height)>.01f)<<3;
  varied|=(fabsf(m.front_taper-reference.front_taper)>.01f)<<4;
  varied|=(fabsf(m.rear_taper-reference.rear_taper)>.01f)<<5;
  varied|=(fabsf(m.color[0]-reference.color[0])>.02f)<<6;
  varied|=(fabsf(m.color[1]-reference.color[1])>.02f)<<7;
  varied|=(fabsf(m.color[2]-reference.color[2])>.02f)<<8;
 }
 CHECK(wheeled&&hover&&varied==0x1ffu,
       "vehicle generator lacks wheel/hover or field diversity: %x",varied);
 CHECK(parked_wheeled&&parked_hover,"production parked seeds lack a wheel/hover variant");
 bool traffic_wheeled=false,traffic_hover=false;
 for(int i=0;i<city_traffic_count();i++){
  CityTrafficCar car=city_traffic_at(i,0),same=city_traffic_at(i,0);
  CityVehicleModel expected=city_vehicle_model(0xc0010000u+(uint32_t)i);
  CHECK(same_vehicle_model(car.model,expected)&&same_vehicle_model(car.model,same.model),
        "production traffic model changed for car %d",i);
  check_vehicle_model(car.model,0xc0010000u+(uint32_t)i);
  float width,length;city_vehicle_dimensions(car.model,&width,&length);
  Vec3 e=city_traffic_collision_extents(car);
  float c=fabsf(cosf(car.yaw*3.14159265358979323846f/180.0f));
  float s=fabsf(sinf(car.yaw*3.14159265358979323846f/180.0f));
  CHECK(fabsf(e.x-(c*width*.5f+s*length*.5f))<1e-5f
        &&fabsf(e.z-(s*width*.5f+c*length*.5f))<1e-5f,
        "traffic extents do not use the production model for car %d",i);
  traffic_wheeled|=car.model.wheels;traffic_hover|=!car.model.wheels;
 }
 CHECK(traffic_wheeled&&traffic_hover,"production traffic seeds lack a wheel/hover variant");

 /* Home is a garage campus, not a lone building straddling a through-road. */
 CHECK(city_campus_garage_count()>=4,"home campus lacks neighboring garages");
 for(int i=0;i<city_campus_garage_count();i++){
  CityGarage g=city_campus_garage(i);
  CHECK(fabsf(g.x)>=25&&city_garage_at(v3(g.x,0,g.z))==g.name,
        "campus workshop %d is missing or intrudes on Home's central aisle",i);
 }
 if(failures){fprintf(stderr,"%d city checks failed\n",failures);return 1;}
 printf("PASS: infinite deterministic districts, roads, parking, tunnels, squares, garage courts and Home campus (%d solids, %d streamed garages)\n",
        checked_solids,checked_garages);
 return 0;
}
