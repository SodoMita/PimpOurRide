/* Spline-built electric hover-car renderer.
 *
 * This is deliberately a compositor-side GL renderer, not a second scene or a
 * JavaScript model. The same current EGL/GLX context draws the car, cockpit,
 * and Wayland textures, so there is no context sharing or cross-thread GL use.
 * Physics and Catmull-Rom evaluation live in vehicle.c; this file only turns
 * that state and the design sections into triangles in car-local space.
 */
#include "chrome.h"

#include <math.h>
#include <string.h>

#define EV_PI 3.14159265358979323846f

static void ev_color(float r, float g, float b) { glColor3f(r, g, b); }

static void ev_box(float x, float y, float z, float w, float h, float d) {
 float a=x-w/2,b=x+w/2,c=y-h/2,e=y+h/2,f=z-d/2,g=z+d/2;
 glBegin(GL_QUADS);
 glNormal3f(0,0,1);glVertex3f(a,c,g);glVertex3f(b,c,g);glVertex3f(b,e,g);glVertex3f(a,e,g);
 glNormal3f(0,0,-1);glVertex3f(b,c,f);glVertex3f(a,c,f);glVertex3f(a,e,f);glVertex3f(b,e,f);
 glNormal3f(0,1,0);glVertex3f(a,e,g);glVertex3f(b,e,g);glVertex3f(b,e,f);glVertex3f(a,e,f);
 glNormal3f(0,-1,0);glVertex3f(a,c,f);glVertex3f(b,c,f);glVertex3f(b,c,g);glVertex3f(a,c,g);
 glNormal3f(1,0,0);glVertex3f(b,c,g);glVertex3f(b,c,f);glVertex3f(b,e,f);glVertex3f(b,e,g);
 glNormal3f(-1,0,0);glVertex3f(a,c,f);glVertex3f(a,c,g);glVertex3f(a,e,g);glVertex3f(a,e,f);
 glEnd();
}

static void ev_cylinder(Vec3 a, Vec3 b, float radius, int slices) {
 Vec3 dir=vnorm(vsub(b,a));
 Vec3 u=vnorm(vcross(dir,fabsf(dir.y)>.9f?v3(1,0,0):v3(0,1,0))),v=vcross(dir,u);
 glBegin(GL_QUAD_STRIP);
 for(int i=0;i<=slices;i++){
  float t=i*2*EV_PI/slices;
  Vec3 n=vadd(vmul(u,cosf(t)),vmul(v,sinf(t))),pa=vadd(a,vmul(n,radius)),pb=vadd(b,vmul(n,radius));
  glNormal3f(n.x,n.y,n.z);glVertex3f(pa.x,pa.y,pa.z);glVertex3f(pb.x,pb.y,pb.z);
 }
 glEnd();
 glBegin(GL_TRIANGLE_FAN);glNormal3f(dir.x,dir.y,dir.z);glVertex3f(b.x,b.y,b.z);
 for(int i=0;i<=slices;i++){
  float t=i*2*EV_PI/slices;Vec3 p=vadd(b,vmul(vadd(vmul(u,cosf(t)),vmul(v,sinf(t))),radius));
  glVertex3f(p.x,p.y,p.z);
 }
 glEnd();
}

static void ev_torus(float major, float minor, int rings, int sides) {
 for(int i=0;i<rings;i++){
  glBegin(GL_QUAD_STRIP);
  for(int j=0;j<=sides;j++)for(int k=0;k<2;k++){
   float a=(i+k)*2*EV_PI/rings,b=j*2*EV_PI/sides;
   glNormal3f(cosf(a)*cosf(b),sinf(a)*cosf(b),sinf(b));
   glVertex3f((major+minor*cosf(b))*cosf(a),(major+minor*cosf(b))*sinf(a),minor*sinf(b));
  }
  glEnd();
 }
}

static void ev_glow_line(Vec3 a, Vec3 b, float radius, float r, float g, float bl) {
 glDisable(GL_LIGHTING);ev_color(r,g,bl);ev_cylinder(a,b,radius,10);
 glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE);glDepthMask(GL_FALSE);
 for(int k=3;k>=1;k--){glColor4f(r,g,bl,.035f);ev_cylinder(a,b,radius*(1+k*1.6f),10);}
 glDepthMask(GL_TRUE);glDisable(GL_BLEND);glEnable(GL_LIGHTING);
}

/* Eight design stations describe a complete lower body. Catmull-Rom locally
 * interpolates width and heights between them; changing one station reshapes
 * only the neighboring panels. That is much easier to art-direct than a pile
 * of unrelated boxes and, unlike a baked mesh, needs no model-loading code. */
typedef struct { float z,width,lower,shoulder,top; } BodySection;
static const BodySection body_sections[] = {
 /* Low, tapered nose and tail: the plan-view width is splined as carefully as
  * the vertical profile, avoiding the upright "brick with a cabin" silhouette. */
 {-5.90f,2.10f,-.70f,-.40f,-.30f},
 {-5.35f,2.40f,-.77f,-.12f, .04f},
 {-4.35f,2.86f,-.81f, .10f, .27f},
 {-2.80f,3.03f,-.82f, .25f, .43f},
 {-1.25f,3.06f,-.81f, .36f, .53f},
 { 1.45f,3.02f,-.80f, .35f, .53f},
 { 3.65f,2.96f,-.78f, .30f, .52f},
 { 4.95f,2.55f,-.73f, .02f, .25f},
 { 5.80f,1.40f,-.64f,-.34f,-.22f}
};
#define BODY_COUNT ((int)(sizeof(body_sections)/sizeof(body_sections[0])))

static int clamp_index(int i, int count) { return i<0?0:i>=count?count-1:i; }

static BodySection body_at(float u) {
 float g=clampf(u,0,1)*(BODY_COUNT-1);
 int i=(int)floorf(g);if(i>=BODY_COUNT-1)i=BODY_COUNT-2;
 float t=g-i;
 const BodySection *a=&body_sections[clamp_index(i-1,BODY_COUNT)];
 const BodySection *b=&body_sections[clamp_index(i,BODY_COUNT)];
 const BodySection *c=&body_sections[clamp_index(i+1,BODY_COUNT)];
 const BodySection *d=&body_sections[clamp_index(i+2,BODY_COUNT)];
 BodySection out;
 out.z=vehicle_catmull_rom(a->z,b->z,c->z,d->z,t);
 out.width=fmaxf(1.5f,vehicle_catmull_rom(a->width,b->width,c->width,d->width,t));
 out.lower=vehicle_catmull_rom(a->lower,b->lower,c->lower,d->lower,t);
 out.shoulder=vehicle_catmull_rom(a->shoulder,b->shoulder,c->shoulder,d->shoulder,t);
 out.top=vehicle_catmull_rom(a->top,b->top,c->top,d->top,t);
 return out;
}

static Vec3 body_ring(BodySection s, int side, int level) {
 /* Seven cross-section rings give the shoulder a smooth, continuously crowned
  * surface. The old four-ring version produced a flat off-road-style slab. */
 static const float width[7]={.68f,.86f,.97f,1.00f,.985f,.91f,.80f};
 float mid=(s.lower+s.shoulder)*.5f,upper=(s.shoulder+s.top)*.5f;
 static const float width_y[7]={-1.00f,-.94f,0,0,0,0,0};
 float y=width_y[level];
 if(level==2)y=s.lower;else if(level==3)y=mid;else if(level==4)y=s.shoulder;else if(level==5)y=upper;else if(level==6)y=s.top;
 return v3(side*s.width*width[level],y,s.z);
}

static void body_sides(void) {
 static const float normal_y[7]={-.50f,-.22f,.02f,.15f,.30f,.48f,.66f};
 for(int side=-1;side<=1;side+=2)for(int level=0;level<6;level++){
  glBegin(GL_QUAD_STRIP);
  for(int i=0;i<=80;i++){
   BodySection s=body_at(i/80.0f);Vec3 a=body_ring(s,side,level),b=body_ring(s,side,level+1);
   float ny=(normal_y[level]+normal_y[level+1])*.5f,nx=sqrtf(fmaxf(.05f,1-ny*ny));
   glNormal3f(side*nx,ny,0);glVertex3f(a.x,a.y,a.z);glVertex3f(b.x,b.y,b.z);
  }
  glEnd();
 }
}

static void body_top(float from, float to) {
 for(int strip=0;strip<16;strip++){
  float x0=-1.0f+strip*.125f,x1=x0+.125f;
  glBegin(GL_QUAD_STRIP);
  for(int i=0;i<=64;i++){
   BodySection s=body_at(from+(to-from)*i/64.0f);
   float y0=s.top+.13f*(1-x0*x0),y1=s.top+.13f*(1-x1*x1);
   glNormal3f(x0*.34f,1,0);glVertex3f(s.width*.80f*x0,y0,s.z);
   glNormal3f(x1*.34f,1,0);glVertex3f(s.width*.80f*x1,y1,s.z);
  }
  glEnd();
 }
}

static void body_cap(bool front) {
 /* Build the fascia from the *same* ring vertices as the side hull. Sharing
  * those edges is what removes gaps and the appearance of a bolted-on bumper. */
 BodySection s=body_at(front?0:1);float sign=front?-1:1;
 for(int side=-1;side<=1;side+=2)for(int level=0;level<6;level++){
  Vec3 outer0=body_ring(s,side,level),outer1=body_ring(s,side,level+1);
  Vec3 centre0=v3(0,outer0.y,s.z+sign*.26f),centre1=v3(0,outer1.y,s.z+sign*.26f);
  if(level==5)centre1.y+=.13f; /* meet the crowned hood/deck centreline */
  glBegin(GL_QUAD_STRIP);
  for(int i=0;i<=16;i++){
   float t=i/16.0f,bow=sign*.26f*(1-t*t);
   Vec3 a=vadd(centre0,vmul(vsub(outer0,centre0),t));
   Vec3 b=vadd(centre1,vmul(vsub(outer1,centre1),t));
   a.z=s.z+bow;b.z=s.z+bow;
   glNormal3f(side*t*.25f,.10f,sign);glVertex3f(a.x,a.y,a.z);glVertex3f(b.x,b.y,b.z);
  }
  glEnd();
 }
}

static void body_outline(float nr, float ng, float nb) {
 /* Systems view retains the spline silhouette as a wire cage while removing
  * opaque panels that would otherwise hide the battery and power electronics. */
 glDisable(GL_LIGHTING);glLineWidth(1.4f);
 static const int levels[]={1,3,4,6};
 for(int side=-1;side<=1;side+=2)for(int li=0;li<4;li++){
  int level=levels[li];glColor3f(level==6?nr:.22f,level==6?ng:.29f,level==6?nb:.40f);
  glBegin(GL_LINE_STRIP);
  for(int i=0;i<=64;i++){Vec3 p=body_ring(body_at(i/64.0f),side,level);glVertex3f(p.x,p.y,p.z);}
  glEnd();
 }
 glEnable(GL_LIGHTING);glLineWidth(1);
}

typedef struct { float z,width,height; } RoofSection;
static const RoofSection roof_sections[] = {
 /* A long fastback arc, not a separate upright cabin planted on the hull. */
 {-1.50f,2.52f,.50f},{-.15f,2.30f,2.10f},{1.85f,2.22f,2.45f},
 {3.75f,2.30f,1.92f},{5.20f,2.52f,.40f}
};
#define ROOF_COUNT ((int)(sizeof(roof_sections)/sizeof(roof_sections[0])))
static RoofSection roof_at(float u) {
 float g=clampf(u,0,1)*(ROOF_COUNT-1);int i=(int)floorf(g);if(i>=ROOF_COUNT-1)i=ROOF_COUNT-2;
 float t=g-i;const RoofSection *a=&roof_sections[clamp_index(i-1,ROOF_COUNT)];
 const RoofSection *b=&roof_sections[clamp_index(i,ROOF_COUNT)],*c=&roof_sections[clamp_index(i+1,ROOF_COUNT)];
 const RoofSection *d=&roof_sections[clamp_index(i+2,ROOF_COUNT)];
 return (RoofSection){vehicle_catmull_rom(a->z,b->z,c->z,d->z,t),
  fmaxf(2.1f,vehicle_catmull_rom(a->width,b->width,c->width,d->width,t)),
  vehicle_catmull_rom(a->height,b->height,c->height,d->height,t)};
}

static void roof_panel(void) {
 for(int strip=0;strip<16;strip++){
  float x0=-1+strip*.125f,x1=x0+.125f;glBegin(GL_QUAD_STRIP);
  for(int i=0;i<=48;i++){
   RoofSection r=roof_at(.24f+.53f*i/48.0f);
   glNormal3f(x0*.32f,1,0);glVertex3f(r.width*x0,r.height+.12f*(1-x0*x0),r.z);
   glNormal3f(x1*.32f,1,0);glVertex3f(r.width*x1,r.height+.12f*(1-x1*x1),r.z);
  }
  glEnd();
 }
}

static float signed_power(float value,float power){return (value<0?-1.f:1.f)*powf(fabsf(value),power);}

static Vec3 concept_hull_point(BodySection s,float angle,Vec3 *normal) {
 float c=cosf(angle),sn=sinf(angle),centre=(s.lower+s.top)*.5f,half=fmaxf(.08f,(s.top-s.lower)*.5f);
 *normal=vnorm(v3(c/fmaxf(s.width,.1f),sn/half,0));
 return v3(s.width*signed_power(c,.72f),centre+half*signed_power(sn,.78f),s.z);
}

static void concept_hull(void) {
 /* Exterior hull: a closed superellipse loft through the same Catmull-Rom
  * body stations used by the driver cutaway. Rounded rectangular sections
  * give broad modern shoulders without flat off-road slabs or open seams. */
 for(int side=0;side<48;side++){
  float a=side*2*EV_PI/48,b=(side+1)*2*EV_PI/48;glBegin(GL_QUAD_STRIP);
  for(int i=0;i<=80;i++){
   BodySection s=body_at(i/80.0f);Vec3 na,nb,pa=concept_hull_point(s,a,&na),pb=concept_hull_point(s,b,&nb);
   glNormal3f(na.x,na.y,na.z);glVertex3f(pa.x,pa.y,pa.z);glNormal3f(nb.x,nb.y,nb.z);glVertex3f(pb.x,pb.y,pb.z);
  }
  glEnd();
 }
 /* Small rounded end fans close the loft at the nose and tail centreline. */
 for(int end=0;end<2;end++){
  BodySection s=body_at(end);float sign=end?1:-1,centre=(s.lower+s.top)*.5f;
  glBegin(GL_TRIANGLE_FAN);glNormal3f(0,0,sign);glVertex3f(0,centre,s.z+sign*.18f);
  for(int i=0;i<=48;i++){Vec3 n,p=concept_hull_point(s,i*2*EV_PI/48,&n);glVertex3f(p.x,p.y,p.z);}glEnd();
 }
}

static Vec3 concept_canopy_point(RoofSection r,float angle,Vec3 *normal) {
 float c=cosf(angle),sn=sinf(angle),bottom=.34f,height=fmaxf(.05f,r.height-bottom);
 *normal=vnorm(v3(c/r.width,sn/height,0));
 return v3(r.width*c,bottom+height*powf(fmaxf(0,sn),.62f),r.z);
}

static void concept_canopy(void) {
 /* One opaque, continuous fastback surface replaces the old collection of
  * separate roof/deck/window pieces. Deep panoramic glass is a deliberate
  * exterior treatment; the driver renderer below keeps its transparent pass. */
 for(int strip=0;strip<40;strip++){
  float a=strip*EV_PI/40,b=(strip+1)*EV_PI/40;glBegin(GL_QUAD_STRIP);
  for(int i=0;i<=64;i++){
   RoofSection r=roof_at(i/64.0f);Vec3 na,nb,pa=concept_canopy_point(r,a,&na),pb=concept_canopy_point(r,b,&nb);
   glNormal3f(na.x,na.y,na.z);glVertex3f(pa.x,pa.y,pa.z);glNormal3f(nb.x,nb.y,nb.z);glVertex3f(pb.x,pb.y,pb.z);
  }
  glEnd();
 }
}

static void spline_cable(const Vec3 *points, int count, float radius) {
 /* Piecewise Catmull-Rom cable routing avoids the impossible right angles
  * that make concept EV models look like toys. Endpoint duplication keeps the
  * cable anchored exactly at its connector and preserves tangent continuity. */
 for(int segment=0;segment<count-1;segment++){
  Vec3 p[4]={points[clamp_index(segment-1,count)],points[segment],points[segment+1],points[clamp_index(segment+2,count)]};
  Vec3 previous=p[1];
  for(int i=1;i<=12;i++){
   Vec3 next=vehicle_catmull_rom_vec3(p,i/12.0f);ev_cylinder(previous,next,radius,8);previous=next;
  }
 }
}

static void spline_led(const Vec3 *points,int count) {
 /* Thin emissive tubes follow the body splines; no chunky off-road lamp pods. */
 glDisable(GL_LIGHTING);spline_cable(points,count,.016f);glEnable(GL_LIGHTING);
}

static void finned_box(float x,float y,float z,float w,float h,float d) {
 ev_color(.26f,.30f,.38f);ev_box(x,y,z,w,h,d);
 ev_color(.48f,.52f,.60f);
 for(float fx=x-w*.42f;fx<=x+w*.42f;fx+=w*.14f)ev_box(fx,y+h*.55f,z,w*.035f,h*.25f,d*.86f);
}

static void hover_pod(float x,float z,float nr,float ng,float nb,float phase) {
 ev_color(.15f,.18f,.24f);ev_cylinder(v3(x,-1.30f,z),v3(x,-.78f,z),.30f,24);
 glPushMatrix();glTranslatef(x,-1.25f,z);glRotatef(90,1,0,0);
 ev_color(.30f,.33f,.41f);ev_torus(.39f,.055f,40,10);
 float pulse=.78f+.22f*sinf(phase);glDisable(GL_LIGHTING);glColor3f(nr*pulse,ng*pulse,nb*pulse);ev_torus(.31f,.025f,40,8);
 glEnable(GL_LIGHTING);glPopMatrix();
 /* The luminous downward column makes it explicit that these are hover pods,
  * not tire models with missing spokes. It is visual emissive geometry only. */
 glDisable(GL_LIGHTING);glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE);glDepthMask(GL_FALSE);
 glColor4f(nr,ng,nb,.10f);ev_cylinder(v3(x,-1.46f,z),v3(x,-1.28f,z),.25f,20);
 glDepthMask(GL_TRUE);glDisable(GL_BLEND);glEnable(GL_LIGHTING);
}

static void ev_systems(Server *s,bool cutaway) {
 float nr,ng,nb;scene_neon_color(s,&nr,&ng,&nb);
 /* Skateboard platform + 82 kWh structural pack, twelve serviceable modules. */
 ev_color(.10f,.13f,.18f);ev_box(0,-.96f,.35f,4.18f,.16f,5.70f);
 for(int row=0;row<4;row++)for(int col=0;col<3;col++){
  float x=(col-1)*1.18f,z=-1.60f+row*1.18f;
  ev_color(.12f,.22f+.025f*row,.30f+.025f*col);ev_box(x,-.76f,z,1.04f,.24f,1.02f);
  ev_color(.30f,.55f,.62f);ev_box(x,-.625f,z,.82f,.025f,.78f);
  ev_color(.92f,.38f,.08f);ev_box(x+.43f,-.60f,z,.055f,.055f,.64f);
 }
 if(!cutaway){ev_color(.15f,.18f,.24f);ev_box(0,-.54f,.35f,4.12f,.08f,5.64f);}

 /* Front/rear permanent-magnet drive units, reduction housings and inverters. */
 for(int end=-1;end<=1;end+=2){
  float z=end<0?-3.55f:3.65f;
  ev_color(.24f,.28f,.35f);ev_cylinder(v3(-.72f,-.43f,z),v3(.72f,-.43f,z),.43f,32);
  for(int fin=0;fin<8;fin++){
   float a=fin*EV_PI/4;ev_cylinder(v3(-.62f,-.43f+.45f*cosf(a),z+.45f*sinf(a)),
                                    v3(.62f,-.43f+.45f*cosf(a),z+.45f*sinf(a)),.018f,6);
  }
  ev_color(.36f,.39f,.46f);ev_cylinder(v3(-1.15f,-.43f,z),v3(-.74f,-.43f,z),.34f,24);
  finned_box(1.35f,-.32f,z,1.0f,.42f,1.10f);
  ev_color(.16f,.19f,.25f);ev_cylinder(v3(-2.50f,-.68f,z),v3(2.50f,-.68f,z),.065f,12);
 }

 /* 800 V junction/fuse contactor box, onboard charger, DC/DC and 12 V battery. */
 finned_box(0,-.30f,2.90f,1.20f,.40f,.72f);
 finned_box(-1.55f,-.30f,4.45f,1.25f,.40f,.90f); /* onboard charger */
 finned_box( 1.55f,-.30f,4.45f,1.10f,.40f,.90f); /* high-voltage -> 12 V DC/DC */
 ev_color(.09f,.11f,.15f);ev_box(-2.05f,-.28f,-2.55f,.78f,.52f,1.10f);
 ev_color(.85f,.28f,.08f);ev_box(-2.24f,.005f,-2.72f,.13f,.07f,.18f);
 ev_color(.15f,.18f,.23f);ev_box(-1.87f,.005f,-2.72f,.13f,.07f,.18f);

 /* Orange high-voltage harnesses and a separate blue thermal loop. */
 ev_color(1.0f,.30f,.035f);
 const Vec3 front_hv[]={{0,-.31f,2.9f},{.65f,-.25f,1.5f},{1.25f,-.25f,-1.3f},{1.35f,-.25f,-3.55f}};
 const Vec3 rear_hv[]={{0,-.31f,2.9f},{.75f,-.20f,3.25f},{1.35f,-.25f,3.65f}};
 const Vec3 charge_hv[]={{0,-.31f,2.9f},{-.85f,-.20f,3.6f},{-1.55f,-.18f,4.45f}};
 spline_cable(front_hv,4,.042f);spline_cable(rear_hv,3,.042f);spline_cable(charge_hv,3,.042f);
 ev_color(.08f,.48f,.67f);
 const Vec3 coolant[]={{0,-.45f,-4.55f},{-1.4f,-.40f,-3.5f},{-1.5f,-.42f,0},{-1.0f,-.40f,3.65f}};
 spline_cable(coolant,4,.030f);

 /* Heat exchanger, coolant reservoir and charge-port hardware. */
 ev_color(.20f,.24f,.30f);ev_box(0,-.30f,-4.75f,3.55f,.72f,.20f);
 ev_color(.43f,.48f,.55f);for(float x=-1.55f;x<1.6f;x+=.16f)ev_box(x,-.30f,-4.63f,.035f,.60f,.035f);
 ev_color(.18f,.48f,.57f);ev_cylinder(v3(2.15f,-.55f,-3.6f),v3(2.15f,.02f,-3.6f),.22f,20);
 ev_color(.10f,.12f,.17f);ev_box(-2.77f,.16f,3.95f,.16f,.38f,.48f);
 ev_glow_line(v3(-2.87f,.15f,3.79f),v3(-2.87f,.15f,4.11f),.025f,nr,ng,nb);

 /* Four independent visual pods share the single heave DOF in vehicle.c. */
 for(int side=-1;side<=1;side+=2)for(int end=-1;end<=1;end+=2)
  hover_pod(side*2.45f,end<0?-3.55f:3.65f,nr,ng,nb,(float)s->time*3+side+end);

 /* Seats remain visible in the systems cutaway to preserve the car scale. */
 for(int side=-1;side<=1;side+=2){
  float x=side<0?-.90f:1.25f;
  ev_color(.12f,.055f,.18f);ev_box(x,-.48f,2.25f,1.15f,.28f,1.30f);
  glPushMatrix();glTranslatef(x,.12f,2.85f);glRotatef(12,1,0,0);ev_box(0,0,0,1.12f,1.30f,.24f);glPopMatrix();
  ev_color(.26f,.20f,.34f);ev_box(x,.90f,2.98f,.62f,.35f,.25f);
 }
}

static float concept_nose_z(float x,float y) {
 /* Invert the rounded superellipse section radially, then apply the same bow
  * as concept_hull's end fan. A tiny offset prevents z-fighting only. */
 BodySection s=body_at(0);float centre=(s.lower+s.top)*.5f,half=(s.top-s.lower)*.5f;
 float ax=fabsf(x)/s.width,ay=fabsf(y-centre)/half;
 float sum=powf(ax,2/.72f)+powf(ay,2/.78f),radial=sum>1?1:powf(sum,.375f);
 return s.z-.195f+.18f*radial;
}

static void exterior_details(Server *s) {
 float nr,ng,nb;scene_neon_color(s,&nr,&ng,&nb);
 /* A flush single-frame fascia follows the nose fan's compound curvature.
  * Its centre sits just ahead of the cap and its edges return into the hull;
  * there is no separate rectangular bumper to break the concept silhouette. */
 glDisable(GL_LIGHTING);glColor3f(.012f,.020f,.030f);
 glBegin(GL_TRIANGLE_FAN);glVertex3f(0,-.46f,concept_nose_z(0,-.46f));
 for(int i=0;i<=48;i++){
  float a=i*2*EV_PI/48,x=1.75f*cosf(a),y=-.46f+.17f*sinf(a);
  glVertex3f(x,y,concept_nose_z(x,y));
 }
 glEnd();
 glColor3f(.065f,.085f,.115f);glLineWidth(1.2f);glBegin(GL_LINE_STRIP);
 for(int i=0;i<=48;i++){
  float a=i*2*EV_PI/48,x=1.75f*cosf(a),y=-.46f+.17f*sinf(a);
  glVertex3f(x,y,concept_nose_z(x,y)-.008f);
 }
 glEnd();
 /* Four small interlocked metal rings provide the requested Audi-concept
  * visual cue without introducing a raster badge or external asset. */
 glColor3f(.58f,.65f,.74f);glLineWidth(1.5f);
 for(int ring=0;ring<4;ring++){
  float centre=(ring-1.5f)*.16f;glBegin(GL_LINE_STRIP);
  for(int i=0;i<=32;i++){
   float a=i*2*EV_PI/32,x=centre+.115f*cosf(a),y=-.43f+.115f*sinf(a);
   glVertex3f(x,y,concept_nose_z(x,y)-.012f);
  }
  glEnd();
 }
 glLineWidth(1);glEnable(GL_LIGHTING);
 /* Concept-car light signatures are curved, flush and deliberately much
  * thinner than the utility-vehicle bars in the first hull. */
 for(int side=-1;side<=1;side+=2){
  ev_color(.60f,.86f,1.0f);
  const Vec3 headlamp[]={{side*.25f,-.08f,-5.86f},{side*1.10f,.015f,-5.70f},{side*2.06f,.12f,-5.28f}};
  spline_led(headlamp,3);
  ev_color(1.0f,.035f,.10f);
  const Vec3 tail_lamp[]={{side*.10f,.02f,5.74f},{side*1.15f,.10f,5.60f},{side*2.10f,.16f,5.18f}};
  spline_led(tail_lamp,3);
  ev_color(.12f,.14f,.19f);ev_cylinder(v3(side*2.70f,.67f,.05f),v3(side*3.18f,.71f,.02f),.032f,12);
  ev_box(side*3.24f,.71f,.02f,.25f,.13f,.34f);
  ev_color(.025f,.04f,.055f);ev_box(side*3.25f,.72f,.20f,.18f,.07f,.018f);
  /* Flush handles and a subtle lower aero highlight, not exposed hinges. */
  ev_color(.07f,.08f,.11f);ev_box(side*3.035f,.35f,1.10f,.018f,.025f,.34f);ev_box(side*2.99f,.32f,3.35f,.018f,.025f,.34f);
  ev_glow_line(v3(side*3.02f,-.43f,-2.3f),v3(side*2.98f,-.43f,3.5f),.010f,nr,ng,nb);
 }
 /* Flush roof sensor strip replaces the tall aftermarket-looking lidar pod. */
 ev_color(.08f,.10f,.14f);ev_box(0,2.18f,-.05f,.72f,.075f,.30f);
 ev_glow_line(v3(-.25f,2.22f,-.10f),v3(.25f,2.22f,-.10f),.010f,nr,ng,nb);
}

void vehicle_render(Server *s,bool cutaway) {
 float nr,ng,nb;scene_neon_color(s,&nr,&ng,&nb);
 ev_systems(s,cutaway);
 if(cutaway){body_outline(nr,ng,nb);return;}

 /* Multi-layer metallic paint is approximated with the existing fixed-function
  * specular material; the spline curvature, not a normal-map dependency,
  * supplies the highlights in both EGL/llvmpipe and GLX backends. */
 if(s->camera_mode==VEHICLE_CAMERA_DRIVER){
  /* Interior visibility cutaway: retain transparent glazing and expose the
   * dashboard, while using the same spline stations as the closed exterior. */
  ev_color(.20f,.22f,.30f);body_sides();
  ev_color(.235f,.255f,.335f);body_top(0,.48f);body_top(.82f,1);body_cap(true);body_cap(false);
  ev_color(.115f,.135f,.19f);roof_panel();
 }else{
  ev_color(.24f,.27f,.35f);concept_hull();
  ev_color(.022f,.035f,.055f);concept_canopy();
 }
 ev_color(.07f,.08f,.11f);ev_box(0,-.90f,.55f,4.15f,.12f,9.20f);
 exterior_details(s);
}

static float body_width_at_z(float z) {
 BodySection previous=body_at(0);
 for(int i=1;i<=80;i++){
  BodySection next=body_at(i/80.0f);
  if(z>=previous.z&&z<=next.z){float t=(z-previous.z)/(next.z-previous.z);return previous.width+(next.width-previous.width)*t;}
  previous=next;
 }
 return z<0?body_at(0).width:body_at(1).width;
}

void vehicle_render_glass(Server *s) {
 if(s->camera_mode!=VEHICLE_CAMERA_DRIVER)return; /* exterior canopy is opaque */
 /* Glass is a separate late pass. It must not write depth: Wayland monitors
  * behind the windshield still need to render, and their textures must retain
  * correct alpha compositing instead of being discarded by transparent glass. */
 glDisable(GL_LIGHTING);glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);glDepthMask(GL_FALSE);
 glColor4f(.035f,.075f,.11f,.14f);
 for(int strip=0;strip<16;strip++){
  float x0=-1+strip*.125f,x1=x0+.125f;
  for(int part=0;part<2;part++){
   float from=part? .76f:0,to=part?1:.27f;glBegin(GL_QUAD_STRIP);
   for(int i=0;i<=24;i++){
    RoofSection r=roof_at(from+(to-from)*i/24.0f);
    glVertex3f(r.width*x0,r.height+.10f*(1-x0*x0),r.z);
    glVertex3f(r.width*x1,r.height+.10f*(1-x1*x1),r.z);
   }
   glEnd();
  }
 }
 for(int side=-1;side<=1;side+=2){
  glBegin(GL_QUAD_STRIP);
  for(int i=0;i<=32;i++){
   RoofSection r=roof_at(.25f+.53f*i/32.0f);
   float belt_width=body_width_at_z(r.z)*.97f;
   glVertex3f(side*belt_width,.48f,r.z);glVertex3f(side*r.width,r.height,r.z);
  }
  glEnd();
 }
 glDepthMask(GL_TRUE);glDisable(GL_BLEND);glEnable(GL_LIGHTING);
}
