#include "city.h"

#include <math.h>
#include <stdio.h>

static int failures;
#define CHECK(condition, ...) do { if (!(condition)) { \
 fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
 fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); failures++; \
} } while (0)

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
         &&s.depth==repeat.depth&&s.height==repeat.height&&s.kind==repeat.kind,
         "solid is not deterministic at chunk %d,%d index %d",x,z,i);
   CHECK(isfinite(s.x)&&isfinite(s.z)&&s.width>0&&s.depth>0&&s.height>0,
         "invalid solid at chunk %d,%d index %d",x,z,i);
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
         &&s.height==repeat.height&&s.kind==repeat.kind,
         "far-world solid changed between visits at %d,%d index %d",c.x,c.z,i);
  }
 }

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
