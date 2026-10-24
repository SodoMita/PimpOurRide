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
 {-5.70f,1.75f,-.78f,-.24f,-.05f},
 {-4.90f,2.65f,-.86f, .08f, .26f},
 {-3.60f,3.00f,-.86f, .28f, .47f},
 {-2.00f,3.10f,-.82f, .44f, .58f},
 { -.70f,3.12f,-.80f, .50f, .56f},
 { 1.50f,3.05f,-.80f, .48f, .52f},
 { 3.70f,3.00f,-.80f, .42f, .60f},
 { 4.80f,2.75f,-.80f, .24f, .47f},
 { 5.60f,1.90f,-.76f,-.16f, .10f}
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
 if(level==0)return v3(side*s.width*.72f,-1.00f,s.z);
 if(level==1)return v3(side*s.width,s.lower,s.z);
 if(level==2)return v3(side*s.width*.99f,s.shoulder,s.z);
 return v3(side*s.width*.80f,s.top,s.z);
}

static void body_sides(void) {
 for(int side=-1;side<=1;side+=2)for(int level=0;level<3;level++){
  glBegin(GL_QUAD_STRIP);
  for(int i=0;i<=64;i++){
   BodySection s=body_at(i/64.0f);Vec3 a=body_ring(s,side,level),b=body_ring(s,side,level+1);
   glNormal3f(side*.86f,level*.22f,0);glVertex3f(a.x,a.y,a.z);glVertex3f(b.x,b.y,b.z);
  }
  glEnd();
 }
}

static void body_top(float from, float to) {
 for(int strip=0;strip<10;strip++){
  float x0=-1.0f+strip*.2f,x1=x0+.2f;
  glBegin(GL_QUAD_STRIP);
  for(int i=0;i<=48;i++){
   BodySection s=body_at(from+(to-from)*i/48.0f);
   float y0=s.top+.13f*(1-x0*x0),y1=s.top+.13f*(1-x1*x1);
   glNormal3f(0,1,0);glVertex3f(s.width*.80f*x0,y0,s.z);glVertex3f(s.width*.80f*x1,y1,s.z);
  }
  glEnd();
 }
}

static void body_outline(float nr, float ng, float nb) {
 /* Systems view retains the spline silhouette as a wire cage while removing
  * opaque panels that would otherwise hide the battery and power electronics. */
 glDisable(GL_LIGHTING);glLineWidth(1.4f);
 for(int side=-1;side<=1;side+=2)for(int level=1;level<=3;level++){
  glColor3f(level==3?nr:.22f,level==3?ng:.29f,level==3?nb:.40f);
  glBegin(GL_LINE_STRIP);
  for(int i=0;i<=64;i++){Vec3 p=body_ring(body_at(i/64.0f),side,level);glVertex3f(p.x,p.y,p.z);}
  glEnd();
 }
 glEnable(GL_LIGHTING);glLineWidth(1);
}

typedef struct { float z,width,height; } RoofSection;
static const RoofSection roof_sections[] = {
 {-1.35f,2.55f,.58f},{.10f,2.45f,2.28f},{2.40f,2.35f,2.55f},
 {4.00f,2.45f,2.05f},{4.90f,2.55f,.55f}
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
 for(int strip=0;strip<10;strip++){
  float x0=-1+strip*.2f,x1=x0+.2f;glBegin(GL_QUAD_STRIP);
  for(int i=0;i<=36;i++){
   RoofSection r=roof_at(.26f+.50f*i/36.0f);
   glNormal3f(0,1,0);glVertex3f(r.width*x0,r.height+.12f*(1-x0*x0),r.z);
   glVertex3f(r.width*x1,r.height+.12f*(1-x1*x1),r.z);
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
  hover_pod(side*2.63f,end<0?-3.55f:3.65f,nr,ng,nb,(float)s->time*3+side+end);

 /* Seats remain visible in the systems cutaway to preserve the car scale. */
 for(int side=-1;side<=1;side+=2){
  float x=side<0?-.90f:1.25f;
  ev_color(.12f,.055f,.18f);ev_box(x,-.48f,2.25f,1.15f,.28f,1.30f);
  glPushMatrix();glTranslatef(x,.12f,2.85f);glRotatef(12,1,0,0);ev_box(0,0,0,1.12f,1.30f,.24f);glPopMatrix();
  ev_color(.26f,.20f,.34f);ev_box(x,.90f,2.98f,.62f,.35f,.25f);
 }
}

static void exterior_details(Server *s) {
 float nr,ng,nb;scene_neon_color(s,&nr,&ng,&nb);
 /* Full-width light signatures, charge indicator, mirrors and roof lidar. */
 ev_glow_line(v3(-2.15f,.10f,-5.22f),v3(-.35f,.20f,-5.52f),.045f,.55f,.85f,1.0f);
 ev_glow_line(v3(.35f,.20f,-5.52f),v3(2.15f,.10f,-5.22f),.045f,.55f,.85f,1.0f);
 ev_glow_line(v3(-2.15f,.15f,5.10f),v3(2.15f,.15f,5.10f),.045f,1.0f,.06f,.16f);
 for(int side=-1;side<=1;side+=2){
  ev_color(.16f,.09f,.24f);ev_cylinder(v3(side*2.75f,.68f,.15f),v3(side*3.35f,.72f,.10f),.055f,12);
  ev_box(side*3.42f,.72f,.08f,.36f,.20f,.48f);
  ev_color(.03f,.06f,.08f);ev_box(side*3.43f,.73f,.33f,.26f,.12f,.025f);
  ev_color(.48f,.50f,.58f);ev_box(side*3.04f,.37f,1.10f,.035f,.045f,.42f);ev_box(side*3.00f,.34f,3.35f,.035f,.045f,.42f);
  ev_glow_line(v3(side*3.05f,-.40f,-2.3f),v3(side*3.05f,-.40f,3.5f),.018f,nr,ng,nb);
 }
 ev_color(.15f,.18f,.24f);ev_cylinder(v3(0,2.56f,1.9f),v3(0,2.82f,1.9f),.26f,32);
 ev_glow_line(v3(-.20f,2.76f,1.9f),v3(.20f,2.76f,1.9f),.025f,nr,ng,nb);
}

void vehicle_render(Server *s,bool cutaway) {
 float nr,ng,nb;scene_neon_color(s,&nr,&ng,&nb);
 ev_systems(s,cutaway);
 if(cutaway){body_outline(nr,ng,nb);return;}

 /* Multi-layer metallic paint is approximated with the existing fixed-function
  * specular material; the spline curvature, not a normal-map dependency,
  * supplies the highlights in both EGL/llvmpipe and GLX backends. */
 ev_color(.19f,.065f,.29f);body_sides();
 ev_color(.24f,.085f,.36f);body_top(0,.43f);body_top(.78f,1);
 ev_color(.17f,.065f,.25f);roof_panel();
 /* Structural roof rails make the canopy read as a complete car rather than
  * an open chassis. They follow the same roof splines as the glass. */
 ev_color(.24f,.09f,.33f);
 for(int side=-1;side<=1;side+=2){
  const Vec3 rail[]={{side*2.55f,.58f,-1.35f},{side*2.45f,2.28f,.10f},{side*2.35f,2.55f,2.40f},{side*2.45f,2.05f,4.0f},{side*2.55f,.55f,4.90f}};
  spline_cable(rail,5,.065f);
 }
 ev_cylinder(v3(-2.45f,2.28f,.10f),v3(2.45f,2.28f,.10f),.055f,12);
 ev_cylinder(v3(-2.45f,2.05f,4.0f),v3(2.45f,2.05f,4.0f),.055f,12);
 ev_color(.08f,.09f,.13f);ev_box(0,-.83f,.55f,4.15f,.16f,8.90f);
 ev_color(.24f,.27f,.34f);ev_box(0,-.55f,-5.46f,3.75f,.34f,.30f);ev_box(0,-.55f,5.35f,3.75f,.34f,.30f);
 exterior_details(s);
}

void vehicle_render_glass(Server *s) {
 if(s->camera_mode==VEHICLE_CAMERA_SYSTEMS)return;
 /* Glass is a separate late pass. It must not write depth: Wayland monitors
  * behind the windshield still need to render, and their textures must retain
  * correct alpha compositing instead of being discarded by transparent glass. */
 glDisable(GL_LIGHTING);glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);glDepthMask(GL_FALSE);
 glColor4f(.10f,.24f,.34f,.28f);
 for(int strip=0;strip<10;strip++){
  float x0=-1+strip*.2f,x1=x0+.2f;
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
   glVertex3f(side*2.93f,.52f,r.z);glVertex3f(side*r.width,r.height,r.z);
  }
  glEnd();
 }
 glDepthMask(GL_TRUE);glDisable(GL_BLEND);glEnable(GL_LIGHTING);
}
