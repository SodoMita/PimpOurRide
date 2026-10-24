/* Spline-built electric hover-car renderer.
 *
 * This is deliberately a compositor-side GL renderer, not a second scene or a
 * JavaScript model. The same current EGL/GLX context draws the car, cockpit,
 * and Wayland textures, so there is no context sharing or cross-thread GL use.
 * Physics and Catmull-Rom evaluation live in vehicle.c; this file renders the
 * EV systems and cables, while vehicle_body.c owns the shared shell and seats.
 */
#include "chrome.h"
#include "vehicle_body.h"

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

static int clamp_index(int i, int count) { return i<0?0:i>=count?count-1:i; }

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
 /* A closed body occludes the high-voltage hardware. Do not draw it through
  * the passenger cabin in driver view; the complete implementation remains
  * visible in the systems cutaway below. Only the external pods and seats
  * contribute to the closed-car silhouette. */
 if(!cutaway){
  for(int side=-1;side<=1;side+=2)for(int end=-1;end<=1;end+=2)
   hover_pod(side*2.45f,end<0?-3.55f:3.65f,nr,ng,nb,(float)s->time*3+side+end);
  vehicle_seats_render();
  return;
 }
 /* Skateboard platform + 82 kWh structural pack, twelve serviceable modules. */
 ev_color(.10f,.13f,.18f);ev_box(0,-.96f,.35f,4.18f,.16f,5.70f);
 for(int row=0;row<4;row++)for(int col=0;col<3;col++){
  float x=(col-1)*1.18f,z=-1.60f+row*1.18f;
  ev_color(.12f,.22f+.025f*row,.30f+.025f*col);ev_box(x,-.76f,z,1.04f,.24f,1.02f);
  ev_color(.30f,.55f,.62f);ev_box(x,-.625f,z,.82f,.025f,.78f);
  ev_color(.92f,.38f,.08f);ev_box(x+.43f,-.60f,z,.055f,.055f,.64f);
 }

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

 /* The same sculpted seats remain visible in the systems cutaway. */
 vehicle_seats_render();
}

void vehicle_render(Server *s,bool cutaway) {
 ev_systems(s,cutaway);
 if(cutaway){
  float nr,ng,nb;scene_neon_color(s,&nr,&ng,&nb);
  vehicle_body_outline(nr,ng,nb);
  return;
 }
 /* Paint, canopy, deck and glazing are one loft in every camera mode. */
 vehicle_body_render(s);
 ev_color(.07f,.08f,.11f);ev_box(0,-.90f,.55f,4.15f,.12f,9.20f);
}

void vehicle_render_glass(Server *s) {
 /* Late non-depth-writing glass preserves Wayland textures behind it. */
 vehicle_body_render_glass(s);
}
