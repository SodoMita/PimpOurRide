/* Detailed upholstered seating shared by every vehicle camera view.
 * Sculpted lofts, separate bolsters/inserts and real small hardware replace
 * the original three-cuboid placeholders. Native GL on the scene thread only. */
#include "vehicle_seat.h"
#include <math.h>

#define B_PI 3.14159265358979323846f
#define SHELL_Z 96
#define SHELL_RING 64

static float sp(float value,float power){return (value<0?-1.f:1.f)*powf(fabsf(value),power);}
static void b_color(float r,float g,float b){glColor3f(r,g,b);}

static void b_cylinder(Vec3 a,Vec3 b,float radius,int slices){
 Vec3 dir=vnorm(vsub(b,a)),u=vnorm(vcross(dir,fabsf(dir.y)>.9f?v3(1,0,0):v3(0,1,0))),v=vcross(dir,u);
 glBegin(GL_QUAD_STRIP);
 for(int i=0;i<=slices;i++){
  float t=i*2*B_PI/slices;Vec3 n=vadd(vmul(u,cosf(t)),vmul(v,sinf(t))),pa=vadd(a,vmul(n,radius)),pb=vadd(b,vmul(n,radius));
  glNormal3f(n.x,n.y,n.z);glVertex3f(pa.x,pa.y,pa.z);glVertex3f(pb.x,pb.y,pb.z);
 }
 glEnd();
 glBegin(GL_TRIANGLE_FAN);glNormal3f(dir.x,dir.y,dir.z);glVertex3f(b.x,b.y,b.z);
 for(int i=0;i<=slices;i++){float t=i*2*B_PI/slices;Vec3 p=vadd(b,vmul(vadd(vmul(u,cosf(t)),vmul(v,sinf(t))),radius));glVertex3f(p.x,p.y,p.z);}glEnd();
}

static Vec3 soft_point(Vec3 radii,float lat,float lon,float shape){
 float cl=cosf(lat),sl=sinf(lat),co=cosf(lon),so=sinf(lon);
 return v3(radii.x*sp(cl,shape)*sp(co,shape),radii.y*sp(sl,shape),radii.z*sp(cl,shape)*sp(so,shape));
}
static Vec3 soft_normal(Vec3 p,Vec3 radii,float shape){
 Vec3 n=v3(sp(p.x/radii.x,2-shape)/radii.x,sp(p.y/radii.y,2-shape)/radii.y,sp(p.z/radii.z,2-shape)/radii.z);
 if(vdot(n,n)<1e-8f)n=vnorm(p);return vnorm(n);
}
/* Superellipsoids with a low exponent give rounded, upholstered cuboids with
 * broad nearly-flat centres, not low-poly boxes and not balloon-like spheres. */
static void b_soft(Vec3 centre,Vec3 radii,float shape,int slices,int stacks){
 /* The sculpted base/back lofts stay at full resolution; small upholstered
  * inserts use a sensible software-rasterizer budget without visible facets. */
 if(slices>28)slices=28;if(stacks>14)stacks=14;
 for(int i=0;i<stacks;i++){
  float lat0=-B_PI*.5f+B_PI*i/stacks,lat1=-B_PI*.5f+B_PI*(i+1)/stacks;
  glBegin(GL_QUAD_STRIP);
  for(int j=0;j<=slices;j++){
   float lon=j*2*B_PI/slices;
   for(int k=0;k<2;k++){
    Vec3 p=soft_point(radii,k?lat1:lat0,lon,shape),n=soft_normal(p,radii,shape);
    glNormal3f(n.x,n.y,n.z);glVertex3f(centre.x+p.x,centre.y+p.y,centre.z+p.z);
   }
  }
  glEnd();
 }
}
static void b_stitch(Vec3 a,Vec3 b,int stitches){
 glDisable(GL_LIGHTING);glColor3f(.48f,.31f,.57f);glLineWidth(1.1f);glBegin(GL_LINES);
 for(int i=0;i<stitches;i+=2){float t0=(float)i/stitches,t1=(float)(i+1)/stitches;Vec3 p=vadd(a,vmul(vsub(b,a),t0)),q=vadd(a,vmul(vsub(b,a),t1));glVertex3f(p.x,p.y,p.z);glVertex3f(q.x,q.y,q.z);}
 glEnd();glLineWidth(1);glEnable(GL_LIGHTING);
}
static void b_perforations(Vec3 centre,float width,float height,int columns,int rows,bool horizontal){
 glDisable(GL_LIGHTING);glPointSize(1.8f);glColor3f(.018f,.013f,.027f);glBegin(GL_POINTS);
 for(int y=0;y<rows;y++)for(int x=0;x<columns;x++){
  float u=columns==1?0:(x/(float)(columns-1)-.5f)*width,v=rows==1?0:(y/(float)(rows-1)-.5f)*height;
  glVertex3f(centre.x+u,centre.y+(horizontal?0:v),centre.z+(horizontal?v:0));
 }
 glEnd();glEnable(GL_LIGHTING);
}

typedef Vec3 (*SeatSkin)(float t,float angle);
static Vec3 cushion_skin(float t,float angle){
 float width=.45f+.065f*sinf(B_PI*t)+.035f*(1-t),mid=-.45f+.025f*sinf(B_PI*t);
 return v3(width*sp(cosf(angle),.40f),mid+.145f*sp(sinf(angle),.52f),1.57f+1.25f*t);
}
static Vec3 back_skin(float t,float angle){
 float width=.43f+.095f*sinf(B_PI*t)-.025f*t,depth=.125f+.018f*sinf(B_PI*t);
 float centre=.015f+.035f*sinf((t-.15f)*B_PI);
 return v3(width*sp(cosf(angle),.44f),-.64f+1.28f*t,centre+depth*sp(sinf(angle),.52f));
}
static Vec3 skin_normal(SeatSkin skin,float t,float angle,bool back){
 Vec3 dt=vsub(skin(t+.0007f,angle),skin(t-.0007f,angle)),da=vsub(skin(t,angle+.0007f),skin(t,angle-.0007f));
 return vnorm(back?vcross(dt,da):vcross(da,dt));
}
static void seat_loft(SeatSkin skin,bool back){
 /* 25 longitudinal rings x 49 radial samples: actual sculpted upholstery,
  * with a narrower shoulder crown and a shaped lumbar/thigh profile. */
 for(int i=0;i<24;i++){
  glBegin(GL_QUAD_STRIP);
  for(int j=0;j<=48;j++)for(int k=0;k<2;k++){
   float t=(i+k)/24.0f,angle=j*2*B_PI/48;Vec3 p=skin(t,angle),n=skin_normal(skin,t,angle,back);
   glNormal3f(n.x,n.y,n.z);glVertex3f(p.x,p.y,p.z);
  }
  glEnd();
 }
 for(int end=0;end<2;end++){
  Vec3 centre={0};for(int j=0;j<48;j++)centre=vadd(centre,skin(end,j*2*B_PI/48));centre=vmul(centre,1.f/48);
  Vec3 n=back?v3(0,end?1:-1,0):v3(0,0,end?1:-1);bool reverse=end==(back?1:0);
  glBegin(GL_TRIANGLE_FAN);glNormal3f(n.x,n.y,n.z);glVertex3f(centre.x,centre.y,centre.z);
  for(int j=0;j<=48;j++){int sample=reverse?48-j:j;Vec3 p=skin(end,sample*2*B_PI/48);glVertex3f(p.x,p.y,p.z);}glEnd();
 }
}

static void seat(float x,int side){
 /* Sliding steel rails and recliner pivots ground the seat in the skateboard. */
 b_color(.20f,.23f,.29f);
 for(int rail=-1;rail<=1;rail+=2){b_cylinder(v3(x+rail*.34f,-.68f,1.70f),v3(x+rail*.34f,-.68f,2.78f),.026f,16);b_cylinder(v3(x+rail*.49f,-.51f,2.73f),v3(x+rail*.49f,-.24f,2.73f),.075f,20);}
 /* Cushion: a shaped base, inset perforated panel and independently raised
  * bolsters. The broad soft-box centre remains a seat, not a rounded cube. */
 b_color(.075f,.034f,.105f);glPushMatrix();glTranslatef(x,0,0);seat_loft(cushion_skin,false);glPopMatrix();
 b_color(.155f,.095f,.205f);b_soft(v3(x,-.305f,2.18f),v3(.37f,.055f,.50f),.44f,36,16);
 b_color(.105f,.045f,.15f);
 for(int edge=-1;edge<=1;edge+=2)b_soft(v3(x+edge*.48f,-.285f,2.23f),v3(.115f,.145f,.57f),.62f,32,16);
 b_stitch(v3(x-.29f,-.242f,1.78f),v3(x-.29f,-.242f,2.56f),28);b_stitch(v3(x+.29f,-.242f,1.78f),v3(x+.29f,-.242f,2.56f),28);
 b_stitch(v3(x-.15f,-.241f,1.80f),v3(x-.15f,-.241f,2.54f),26);b_stitch(v3(x+.15f,-.241f,1.80f),v3(x+.15f,-.241f,2.54f),26);
 b_perforations(v3(x,-.244f,2.18f),.42f,.68f,7,12,true);
 /* Backrest and its own lumbar/shoulder geometry, reclined as one assembly. */
 glPushMatrix();glTranslatef(x,.18f,2.82f);glRotatef(12,1,0,0);
 b_color(.070f,.032f,.098f);seat_loft(back_skin,true);
 b_color(.145f,.085f,.195f);b_soft(v3(0,.015f,-.132f),v3(.355f,.50f,.055f),.43f,36,20);
 b_color(.11f,.047f,.155f);
 for(int edge=-1;edge<=1;edge+=2)b_soft(v3(edge*.465f,.035f,-.10f),v3(.115f,.55f,.12f),.59f,32,20);
 b_soft(v3(0,-.32f,-.205f),v3(.36f,.15f,.095f),.52f,36,16);
 b_stitch(v3(-.275f,-.41f,-.193f),v3(-.275f,.43f,-.193f),30);b_stitch(v3(.275f,-.41f,-.193f),v3(.275f,.43f,-.193f),30);
 b_stitch(v3(-.14f,-.40f,-.195f),v3(-.14f,.40f,-.195f),28);b_stitch(v3(.14f,-.40f,-.195f),v3(.14f,.40f,-.195f),28);
 b_perforations(v3(0,.02f,-.194f),.40f,.72f,7,13,false);
 /* Metal headrest posts and a separate shaped head restraint. */
 b_color(.31f,.34f,.40f);for(int post=-1;post<=1;post+=2)b_cylinder(v3(post*.17f,.56f,.015f),v3(post*.17f,.88f,.015f),.018f,14);
 b_color(.085f,.038f,.12f);b_soft(v3(0,1.02f,-.015f),v3(.36f,.20f,.16f),.37f,40,20);
 b_color(.16f,.10f,.21f);b_soft(v3(0,1.02f,-.155f),v3(.25f,.12f,.035f),.46f,32,14);
 b_stitch(v3(-.22f,.96f,-.194f),v3(.22f,.96f,-.194f),18);
 glPopMatrix();
 /* Inner belt buckle with a real red release insert. */
 float inner=side<0?1:-1;b_color(.035f,.040f,.055f);b_soft(v3(x+inner*.62f,-.29f,2.48f),v3(.07f,.09f,.12f),.38f,24,14);
 b_color(.55f,.035f,.055f);b_soft(v3(x+inner*.62f,-.205f,2.46f),v3(.045f,.018f,.065f),.42f,20,12);
 /* Outer recliner bezel and two metallic seat-position switches. */
 float outer=-inner;b_color(.025f,.030f,.043f);b_soft(v3(x+outer*.565f,-.40f,2.23f),v3(.035f,.085f,.22f),.40f,28,14);
 b_color(.48f,.51f,.59f);for(int control=-1;control<=1;control+=2)b_soft(v3(x+outer*.59f,-.385f,2.23f+control*.10f),v3(.018f,.025f,.045f),.45f,20,12);
}
void vehicle_seats_render(void){
 /* Match the bounded cabin packaging in vehicle.c while scaling the detailed
  * geometry around each seat centre, not around the whole cabin. */
 for(int index=0;index<2;index++){
  Vec3 anchor=vehicle_seat_anchor((VehicleSeat)index);int side=index?1:-1;
  glPushMatrix();glTranslatef(anchor.x,0,anchor.z);glScalef(.80f,.85f,1.0f);
  seat(0,side);glPopMatrix();
 }
}
