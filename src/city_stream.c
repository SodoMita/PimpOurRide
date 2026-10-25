#include "city.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>

/* Integer hashing is stable across platforms and at negative world
 * coordinates. Unlike sin-based hashes it also remains repeatable for the
 * very large chunk indices reached after a long drive. */
static uint32_t stream_hash(int x,int z,int salt){
 uint32_t h=(uint32_t)x*0x9e3779b1u^(uint32_t)z*0x85ebca6bu^(uint32_t)salt*0xc2b2ae35u;
 h^=h>>16;h*=0x7feb352du;h^=h>>15;h*=0x846ca68bu;h^=h>>16;return h;
}
static float stream_random(int x,int z,int salt){return (stream_hash(x,z,salt)&0x00ffffffu)/16777216.0f;}
static int floor_div(int value,int divisor){
 int q=value/divisor,r=value%divisor;return r<0?q-1:q;
}

bool city_stream_chunk_is_core(CityChunk c){
 /* Exact multiples of the chunk size: procedural roads never cut through
  * Home, the old service garages, or the ambient traffic ring. */
 return c.x>=-2&&c.x<=1&&c.z>=-3&&c.z<=1;
}

static CityDistrict generated_district(int x,int z){
 /* Two-chunk cells form coherent 160 m districts instead of unrelated random
  * buildings on every block. */
 int cell=stream_hash(floor_div(x,2),floor_div(z,2),11)%6;
 return (CityDistrict)cell;
}
static CityFeature generated_feature(int x,int z,CityDistrict district){
 float h=stream_random(x,z,29);
 if(district==CITY_DISTRICT_CIVIC){
  if(h<.28f)return CITY_FEATURE_SQUARE;
  if(h<.48f)return CITY_FEATURE_PARKING;
 }else if(district==CITY_DISTRICT_PARK){
  if(h<.72f)return CITY_FEATURE_PARK;
  if(h<.84f)return CITY_FEATURE_SQUARE;
 }else if(district==CITY_DISTRICT_SERVICE){
  if(h<.42f)return CITY_FEATURE_GARAGES;
  if(h<.64f)return CITY_FEATURE_PARKING;
 }else if(district==CITY_DISTRICT_INDUSTRIAL){
  if(h<.20f)return CITY_FEATURE_GARAGES;
  if(h<.43f)return CITY_FEATURE_PARKING;
 }else{
  if(h<.09f)return CITY_FEATURE_SQUARE;
  if(h<.25f)return CITY_FEATURE_PARKING;
  if(h<.35f)return CITY_FEATURE_PARK;
 }
 return CITY_FEATURE_BLOCKS;
}
CityChunk city_chunk(int x,int z){
 CityChunk c={.x=x,.z=z};
 if(city_stream_chunk_is_core(c)){
  c.district=CITY_DISTRICT_SERVICE;c.feature=CITY_FEATURE_BLOCKS;return c;
 }
 c.district=generated_district(x,z);c.feature=generated_feature(x,z,c.district);
 /* A legible first expedition north of Home: tunnel, civic square, parking,
  * park and service court share one connected boulevard loop. These explicit
  * landmarks use the same chunk API as all generated land. */
 if(x==0&&z==-4){c.district=CITY_DISTRICT_CIVIC;c.feature=CITY_FEATURE_SQUARE;}
 if(x==1&&z==-4){c.district=CITY_DISTRICT_CIVIC;c.feature=CITY_FEATURE_PARKING;}
 if(x==-1&&z==-4){c.district=CITY_DISTRICT_PARK;c.feature=CITY_FEATURE_PARK;}
 if(x==2&&z==-4){c.district=CITY_DISTRICT_SERVICE;c.feature=CITY_FEATURE_GARAGES;}
 return c;
}
CityChunk city_chunk_at(Vec3 p){
 return city_chunk((int)floorf(p.x/CITY_STREAM_CHUNK_SIZE),
                   (int)floorf(p.z/CITY_STREAM_CHUNK_SIZE));
}
CityDistrict city_district_at(Vec3 p){return city_chunk_at(p).district;}
const char *city_district_name(CityDistrict d){
 static const char *const names[]={
  "NEON DOWNTOWN","AERIAL RESIDENTIAL","FOUNDRY INDUSTRIAL",
  "SKY GARDENS","CIVIC COMMONS","SERVICE ROW"
 };
 if(d<0||d>CITY_DISTRICT_SERVICE)d=CITY_DISTRICT_SERVICE;
 return names[d];
}
const char *city_feature_name(CityFeature f){
 static const char *const names[]={"CITY BLOCKS","PUBLIC SQUARE","PARKING COURT","POCKET PARK","GARAGE COURT"};
 if(f<0||f>CITY_FEATURE_GARAGES)f=CITY_FEATURE_BLOCKS;
 return names[f];
}

CityRoadType city_road_type(int edge,int segment,bool vertical){
 /* A covered northern approach and an eastern civic underpass are continuous
  * across segment boundaries. Their centres remain drivable; only the shared
  * wall geometry is solid. */
 if(vertical&&edge==0&&(segment==-4||segment==-5))return CITY_ROAD_TUNNEL;
 if(!vertical&&edge==2&&(segment==2||segment==3))return CITY_ROAD_TUNNEL;
 CityChunk a=vertical?city_chunk(edge,segment):city_chunk(segment,edge);
 CityChunk b=vertical?city_chunk(edge-1,segment):city_chunk(segment,edge-1);
 if(city_stream_chunk_is_core(a)||city_stream_chunk_is_core(b))return CITY_ROAD_AVENUE;
 int h=stream_hash(floor_div(edge,2),floor_div(segment,2),vertical?71:97)%100;
 if(h<10)return CITY_ROAD_ALLEY;
 if(h<39)return CITY_ROAD_LOCAL;
 if(h<78)return CITY_ROAD_AVENUE;
 return CITY_ROAD_BOULEVARD;
}
float city_road_width(CityRoadType type){
 switch(type){
  case CITY_ROAD_ALLEY:return 9.0f;
  case CITY_ROAD_LOCAL:return 14.0f;
  case CITY_ROAD_AVENUE:return 22.0f;
  case CITY_ROAD_BOULEVARD:return 30.0f;
  case CITY_ROAD_TUNNEL:return 20.0f;
 }
 return 14.0f;
}

static CitySolid solid(float x,float z,float w,float d,float h,int style,CitySolidKind kind){
 CitySolid s={.x=x,.z=z,.width=w,.depth=d,.height=h,.style=style,.kind=kind};return s;
}
int city_chunk_solid_count(CityChunk c){
 if(city_stream_chunk_is_core(c))return 0;
 switch(c.feature){
  case CITY_FEATURE_SQUARE:return 1;
  case CITY_FEATURE_PARKING:return 5; /* kiosk + four physical parked cars */
  case CITY_FEATURE_PARK:return 8;
  case CITY_FEATURE_GARAGES:return 2; /* courtyard cars; garages are separate */
  case CITY_FEATURE_BLOCKS:
   if(c.district==CITY_DISTRICT_DOWNTOWN)return 2;
   if(c.district==CITY_DISTRICT_RESIDENTIAL)return 3;
   if(c.district==CITY_DISTRICT_SERVICE)return 2;
   return 1;
 }
 return 0;
}
CitySolid city_chunk_solid(CityChunk c,int i){
 int count=city_chunk_solid_count(c);if(i<0)i=0;if(i>=count)i=count-1;
 float ox=c.x*CITY_STREAM_CHUNK_SIZE,oz=c.z*CITY_STREAM_CHUNK_SIZE;
 int variation=(int)(stream_hash(c.x,c.z,i+101)&15);
 int style=(int)c.district*16+variation;
 if(c.feature==CITY_FEATURE_SQUARE)
  return solid(ox+40,oz+40,12,12,1.3f,style,CITY_SOLID_FOUNTAIN);
 if(c.feature==CITY_FEATURE_PARKING){
  if(i==0)return solid(ox+21,oz+57,10,10,5.5f,style,CITY_SOLID_KIOSK);
  static const float cars[4][2]={{28,26},{41,26},{54,26},{34,45}};
  return solid(ox+cars[i-1][0],oz+cars[i-1][1],3.4f,6.8f,1.5f,style,CITY_SOLID_PARKED_CAR);
 }
 if(c.feature==CITY_FEATURE_PARK){
  float x=22+(i%3)*18,z=24+(i/3)*16;
  return solid(ox+x,oz+z,1.2f,1.2f,5.0f+2.0f*stream_random(c.x,c.z,i+173),style,CITY_SOLID_TREE);
 }
 if(c.feature==CITY_FEATURE_GARAGES)
  return solid(ox+40,oz+(i==0?20:60),3.4f,6.8f,1.5f,style,CITY_SOLID_PARKED_CAR);
 if(c.district==CITY_DISTRICT_DOWNTOWN){
  float height=48+52*stream_random(c.x,c.z,i+211);
  return solid(ox+(i==0?28:52),oz+40,16,30,height,style,CITY_SOLID_BUILDING);
 }
 if(c.district==CITY_DISTRICT_RESIDENTIAL){
  static const float plots[3][2]={{26,26},{54,26},{40,54}};
  float height=19+19*stream_random(c.x,c.z,i+223);
  return solid(ox+plots[i][0],oz+plots[i][1],17,18,height,style,CITY_SOLID_BUILDING);
 }
 if(c.district==CITY_DISTRICT_INDUSTRIAL)
  return solid(ox+40,oz+40,36,34,12+10*stream_random(c.x,c.z,227),style,CITY_SOLID_BUILDING);
 if(c.district==CITY_DISTRICT_CIVIC)
  return solid(ox+40,oz+40,34,30,20+15*stream_random(c.x,c.z,229),style,CITY_SOLID_BUILDING);
 if(c.district==CITY_DISTRICT_PARK)
  return solid(ox+40,oz+40,18,18,7+4*stream_random(c.x,c.z,233),style,CITY_SOLID_BUILDING);
 return solid(ox+(i==0?28:52),oz+40,18,30,10+7*stream_random(c.x,c.z,i+239),style,CITY_SOLID_BUILDING);
}

int city_chunk_garage_count(CityChunk c){
 return !city_stream_chunk_is_core(c)&&c.feature==CITY_FEATURE_GARAGES?2:0;
}
CityGarage city_chunk_garage(CityChunk c,int i){
 if(i<0)i=0;if(i>1)i=1;
 CityGarage g={
  .x=c.x*CITY_STREAM_CHUNK_SIZE+(i==0?28:52),
  .z=c.z*CITY_STREAM_CHUNK_SIZE+40,
  .width=20,.depth=34,.axis=1,
  .name=i==0?"DISTRICT GARAGE / WEST":"DISTRICT GARAGE / EAST"
 };
 return g;
}
const char *city_stream_garage_at(Vec3 p){
 CityChunk c=city_chunk_at(p);
 for(int i=0;i<city_chunk_garage_count(c);i++){
  CityGarage g=city_chunk_garage(c,i);
  if(fabsf(p.x-g.x)<=g.width*.5f&&fabsf(p.z-g.z)<=g.depth*.5f)return g.name;
 }
 return NULL;
}

int city_chunk_road_solid_count(CityChunk c){
 if(city_stream_chunk_is_core(c))return 0;
 int count=0;
 if(city_road_type(c.x,c.z,true)==CITY_ROAD_TUNNEL)count+=2;
 if(city_road_type(c.z,c.x,false)==CITY_ROAD_TUNNEL)count+=2;
 return count;
}
CitySolid city_chunk_road_solid(CityChunk c,int i){
 bool vertical=city_road_type(c.x,c.z,true)==CITY_ROAD_TUNNEL;
 if(i<0)i=0;
 if(vertical&&i<2){
  int side=i==0?-1:1;
  return solid(c.x*CITY_STREAM_CHUNK_SIZE+side*11.5f,
               c.z*CITY_STREAM_CHUNK_SIZE+40,
               1,CITY_STREAM_CHUNK_SIZE-24.0f,7,0,CITY_SOLID_TUNNEL_WALL);
 }
 if(vertical)i-=2;
 int side=i==0?-1:1;
 return solid(c.x*CITY_STREAM_CHUNK_SIZE+40,
              c.z*CITY_STREAM_CHUNK_SIZE+side*11.5f,
              CITY_STREAM_CHUNK_SIZE-24.0f,1,7,0,CITY_SOLID_TUNNEL_WALL);
}
bool city_stream_is_tunnel(Vec3 p){
 int cx=(int)floorf(p.x/CITY_STREAM_CHUNK_SIZE),cz=(int)floorf(p.z/CITY_STREAM_CHUNK_SIZE);
 float lx=p.x-cx*CITY_STREAM_CHUNK_SIZE,lz=p.z-cz*CITY_STREAM_CHUNK_SIZE;
 int edge_x=lx<16?cx:lx>CITY_STREAM_CHUNK_SIZE-16?cx+1:cx;
 int edge_z=lz<16?cz:lz>CITY_STREAM_CHUNK_SIZE-16?cz+1:cz;
 if(fabsf(p.x-edge_x*CITY_STREAM_CHUNK_SIZE)<11.0f&&
    city_road_type(edge_x,cz,true)==CITY_ROAD_TUNNEL)return true;
 if(fabsf(p.z-edge_z*CITY_STREAM_CHUNK_SIZE)<11.0f&&
    city_road_type(edge_z,cx,false)==CITY_ROAD_TUNNEL)return true;
 return false;
}
