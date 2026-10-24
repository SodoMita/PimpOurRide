/* Coherent concept-car body, glazing and upholstered seating.
 *
 * Every exterior view evaluates the same longitudinal stations and the same
 * closed cross-section. The upper canopy and lower hull share their beltline
 * vertices exactly; there are no independently positioned windshield, roof or
 * deck meshes to drift apart. All geometry is native immediate-mode GL so it
 * uses the compositor's existing single EGL/GLX context and render thread. */
#include "vehicle_body.h"
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
void vehicle_seats_render(void){seat(-.90f,-1);seat(1.25f,1);}

/* z, half width, underbody, beltline, canopy crown, lower haunch. */
typedef struct {float z,width,bottom,belt,roof,haunch;} ShellStation;
static const ShellStation shell_stations[]={
 /* roof is the centre crown even on the painted hood/deck: keeping it above
  * the belt there prevents the collapsed, flat "toy car" panels. */
 {-7.05f,1.25f,-.58f,-.34f,-.24f,.02f},
 {-6.50f,1.95f,-.68f,-.12f,-.02f,.04f},
 {-5.45f,2.45f,-.75f, .10f, .28f,.07f},
 {-3.85f,2.62f,-.78f, .27f, .48f,.09f},
 {-2.80f,2.66f,-.79f, .32f, .56f,.09f},
 {-1.70f,2.64f,-.78f, .34f,1.35f,.07f},
 {-0.55f,2.60f,-.77f, .35f,1.74f,.06f},
 { 1.00f,2.58f,-.76f, .35f,1.78f,.06f},
 { 2.35f,2.56f,-.75f, .34f,1.66f,.07f},
 { 3.65f,2.52f,-.72f, .30f,1.10f,.08f},
 { 4.75f,2.36f,-.66f, .16f, .38f,.07f},
 { 5.80f,2.05f,-.60f,-.02f, .10f,.05f},
 { 6.75f,1.30f,-.52f,-.28f,-.24f,.02f}
};
#define SHELL_COUNT ((int)(sizeof(shell_stations)/sizeof(shell_stations[0])))
static int shell_clamp(int i){return i<0?0:i>=SHELL_COUNT?SHELL_COUNT-1:i;}
static ShellStation shell_at(float u){
 float g=clampf(u,0,1)*(SHELL_COUNT-1);int i=(int)floorf(g);if(i>=SHELL_COUNT-1)i=SHELL_COUNT-2;float t=g-i;
 const ShellStation *a=&shell_stations[shell_clamp(i-1)],*b=&shell_stations[shell_clamp(i)],*c=&shell_stations[shell_clamp(i+1)],*d=&shell_stations[shell_clamp(i+2)];
 ShellStation o;
 o.z=vehicle_catmull_rom(a->z,b->z,c->z,d->z,t);o.width=fmaxf(.4f,vehicle_catmull_rom(a->width,b->width,c->width,d->width,t));
 o.bottom=vehicle_catmull_rom(a->bottom,b->bottom,c->bottom,d->bottom,t);o.belt=vehicle_catmull_rom(a->belt,b->belt,c->belt,d->belt,t);
 o.roof=fmaxf(o.belt,vehicle_catmull_rom(a->roof,b->roof,c->roof,d->roof,t));o.haunch=vehicle_catmull_rom(a->haunch,b->haunch,c->haunch,d->haunch,t);return o;
}
static Vec3 shell_point(float u,float angle){
 ShellStation s=shell_at(u);float c=cosf(angle),sn=sinf(angle),x,y;
 if(sn>=0){
  float rise=powf(sn,.72f),width=s.width*(1-.16f*rise);
  x=width*sp(c,.82f);y=s.belt+(s.roof-s.belt)*rise;
 }else{
  float down=-sn,width=s.width+s.haunch*powf(down,.55f);
  x=width*sp(c,.58f);y=s.belt-(s.belt-s.bottom)*powf(down,.82f);
 }
 return v3(x,y,s.z);
}
static Vec3 shell_normal(float u,float angle){
 Vec3 du=vsub(shell_point(u+.0007f,angle),shell_point(u-.0007f,angle));
 Vec3 da=vsub(shell_point(u,angle+.0007f),shell_point(u,angle-.0007f));
 return vnorm(vcross(da,du));
}
static void shell_vertex(float u,float angle){Vec3 p=shell_point(u,angle),n=shell_normal(u,angle);glNormal3f(n.x,n.y,n.z);glVertex3f(p.x,p.y,p.z);}
static void shell_strip(int ring,float from,float to){
 float a=ring*2*B_PI/SHELL_RING,b=(ring+1)*2*B_PI/SHELL_RING;
 int first=(int)floorf(from*SHELL_Z+.5f),last=(int)ceilf(to*SHELL_Z-.5f);if(last<=first)return;
 glBegin(GL_QUAD_STRIP);for(int i=first;i<=last;i++){float u=i/(float)SHELL_Z;shell_vertex(u,a);shell_vertex(u,b);}glEnd();
}
static void shell_surface(bool glass){
 const float cabin_from=4.0f/(SHELL_COUNT-1),roof_from=6.0f/(SHELL_COUNT-1);
 const float roof_to=8.0f/(SHELL_COUNT-1),cabin_to=11.0f/(SHELL_COUNT-1);
 for(int ring=0;ring<SHELL_RING;ring++){
  bool upper=ring<32,roof_band=upper&&ring>=11&&ring<=20;
  if(!upper){if(!glass)shell_strip(ring,0,1);continue;}
  if(glass){
   if(roof_band){shell_strip(ring,cabin_from,roof_from);shell_strip(ring,roof_to,cabin_to);}
   else shell_strip(ring,cabin_from,cabin_to);
  }else{
   shell_strip(ring,0,cabin_from);shell_strip(ring,cabin_to,1);
   if(roof_band)shell_strip(ring,roof_from,roof_to);
  }
 }
}
static void shell_caps(void){
 for(int end=0;end<2;end++){
  float u=end,sign=end?1:-1;ShellStation s=shell_at(u);float centre=(s.bottom+s.belt)*.5f;
  glBegin(GL_TRIANGLE_FAN);glNormal3f(0,0,sign);glVertex3f(0,centre,s.z+sign*.16f);
  for(int ring=0;ring<=SHELL_RING;ring++){Vec3 p=shell_point(u,ring*2*B_PI/SHELL_RING);glVertex3f(p.x,p.y,p.z);}glEnd();
 }
}
static float shell_u_for_z(float z){
 ShellStation previous=shell_at(0);
 for(int i=1;i<=160;i++){float u=i/160.0f;ShellStation next=shell_at(u);if(z>=previous.z&&z<=next.z)return (i-1+(z-previous.z)/(next.z-previous.z))/160.0f;previous=next;}
 return z<shell_at(0).z?0:1;
}
static Vec3 shell_detail(float z,float angle,float offset){float u=shell_u_for_z(z);Vec3 p=shell_point(u,angle),n=shell_normal(u,angle);return vadd(p,vmul(n,offset));}
/* Painted canopy structure is a set of panels on the identical parametric
 * surface, not tubes or a second approximate cage. A/C pillars widen where
 * they meet the beltline and the roof rails join them without a seam. */
static void canopy_panel(int side,int part){
 glBegin(GL_QUAD_STRIP);
 for(int i=0;i<=36;i++){
  float t=i/36.0f,u,centre,half;
  if(part==0){u=(4+2*t)/(SHELL_COUNT-1);centre=1.05f*t;half=.075f;}
  else if(part==1){u=(6+2*t)/(SHELL_COUNT-1);centre=1.05f;half=.075f;}
  else{u=(8+2*t)/(SHELL_COUNT-1);centre=1.05f*(1-t);half=.075f+.13f*t;}
  if(side<0)centre=B_PI-centre;
  for(int edge=0;edge<2;edge++){
   float angle=centre+(edge?half:-half);Vec3 p=shell_point(u,angle),n=shell_normal(u,angle);p=vadd(p,vmul(n,.006f));
   glNormal3f(n.x,n.y,n.z);glVertex3f(p.x,p.y,p.z);
  }
 }
 glEnd();
}
static void canopy_structure(void){b_color(.28f,.31f,.38f);for(int side=-1;side<=1;side+=2)for(int part=0;part<3;part++)canopy_panel(side,part);}
static float shell_cap_z(float x,float y,bool front){
 float u=front?0:1,sign=front?-1:1;ShellStation s=shell_at(u);Vec3 centre=v3(0,(s.bottom+s.belt)*.5f,s.z+sign*.16f),point=v3(x,y,0);
 for(int ring=0;ring<SHELL_RING;ring++){
  Vec3 a=shell_point(u,ring*2*B_PI/SHELL_RING),b=shell_point(u,(ring+1)*2*B_PI/SHELL_RING);
  Vec3 v0=vsub(a,centre),v1=vsub(b,centre),v2=vsub(point,centre);float den=v0.x*v1.y-v1.x*v0.y;if(fabsf(den)<1e-7f)continue;
  float wa=(v2.x*v1.y-v1.x*v2.y)/den,wb=(v0.x*v2.y-v2.x*v0.y)/den,wc=1-wa-wb;
  if(wa>=-1e-4f&&wb>=-1e-4f&&wc>=-1e-4f)return centre.z+(wa+wb)*(s.z-centre.z)+sign*.009f;
 }
 return centre.z+sign*.009f;
}
static void led_curve(Vec3 a,Vec3 b,Vec3 c){
 glDisable(GL_LIGHTING);Vec3 previous=a;
 for(int i=1;i<=20;i++){float t=i/20.0f,it=1-t;Vec3 p=vadd(vadd(vmul(a,it*it),vmul(b,2*it*t)),vmul(c,t*t));b_cylinder(previous,p,.014f,10);previous=p;}glEnable(GL_LIGHTING);
}
static void side_blade(int side){
 float a0=side>0?-.12f:B_PI+.12f,a1=side>0?-.40f:B_PI+.40f;b_color(.030f,.040f,.057f);
 glBegin(GL_QUAD_STRIP);
 for(int i=0;i<=40;i++){float z=-2.7f+6.3f*i/40;Vec3 p=shell_detail(z,a0,.012f),q=shell_detail(z,a1,.012f);glNormal3f(side,0,0);glVertex3f(p.x,p.y,p.z);glVertex3f(q.x,q.y,q.z);}glEnd();
 b_color(.22f,.25f,.31f);
 for(int i=0;i<3;i++){float z=-1.7f+i*.32f;Vec3 p=shell_detail(z,a0,.020f),q=shell_detail(z,a1,.020f);b_cylinder(p,q,.012f,8);}
}
static void exterior_details(Server *s){
 float nr,ng,nb;scene_neon_color(s,&nr,&ng,&nb);
 /* Flush single-frame grille and emblem are tessellated against the actual
  * front cap, including its compound curvature. */
 glDisable(GL_LIGHTING);b_color(.012f,.019f,.028f);glBegin(GL_TRIANGLE_FAN);glVertex3f(0,-.45f,shell_cap_z(0,-.45f,true));
 for(int i=0;i<=64;i++){float a=i*2*B_PI/64,x=1.34f*cosf(a),y=-.45f+.155f*sinf(a);glVertex3f(x,y,shell_cap_z(x,y,true));}glEnd();
 b_color(.07f,.085f,.11f);glLineWidth(1.1f);glBegin(GL_LINE_STRIP);
 for(int i=0;i<=64;i++){float a=i*2*B_PI/64,x=1.34f*cosf(a),y=-.45f+.155f*sinf(a);glVertex3f(x,y,shell_cap_z(x,y,true)-.005f);}glEnd();
 b_color(.55f,.61f,.69f);glLineWidth(1.4f);
 for(int ring=0;ring<4;ring++){float centre=(ring-1.5f)*.15f;glBegin(GL_LINE_STRIP);for(int i=0;i<=32;i++){float a=i*2*B_PI/32,x=centre+.105f*cosf(a),y=-.43f+.105f*sinf(a);glVertex3f(x,y,shell_cap_z(x,y,true)-.009f);}glEnd();}
 glLineWidth(1);glEnable(GL_LIGHTING);
 for(int side=-1;side<=1;side+=2){
  float a0=side>0?.20f:B_PI-.20f,a1=side>0?.52f:B_PI-.52f,a2=side>0?.86f:B_PI-.86f;
  b_color(.60f,.84f,.98f);led_curve(shell_detail(-5.72f,a0,.018f),shell_detail(-5.65f,a1,.018f),shell_detail(-5.56f,a2,.018f));
  b_color(.92f,.025f,.075f);led_curve(shell_detail(5.48f,a0,.018f),shell_detail(5.55f,a1,.018f),shell_detail(5.62f,a2,.018f));
  side_blade(side);
  /* Camera mirrors grow out of the actual upper belt surface. */
  Vec3 root=shell_detail(-.35f,side>0?.30f:B_PI-.30f,.015f),tip=vadd(root,v3(side*.34f,.035f,.04f));b_color(.12f,.15f,.20f);b_cylinder(root,tip,.026f,14);
  b_soft(tip,v3(.16f,.075f,.22f),.70f,32,16);b_color(.018f,.030f,.045f);b_soft(vadd(tip,v3(0,0,.10f)),v3(.12f,.052f,.09f),.72f,28,14);
  /* Flush handles follow the belt instead of floating at an assumed width. */
  for(float z=.75f;z<3.2f;z+=2.15f){float angle=side>0?0:B_PI;Vec3 a=shell_detail(z,angle,.014f),b=shell_detail(z+.30f,angle,.014f);b_color(.055f,.065f,.085f);b_cylinder(a,b,.012f,10);}
 }
 /* Restrained hood creases and a flush roof sensor, all evaluated on the
  * same shell; no independent rails or floating decorative rectangles. */
 for(int side=-1;side<=1;side+=2){float angle=side>0?.62f:B_PI-.62f;b_color(.15f,.18f,.24f);Vec3 previous=shell_detail(-5.75f,angle,.010f);for(int i=1;i<=24;i++){float z=-5.75f+2.65f*i/24;Vec3 next=shell_detail(z,angle,.010f);b_cylinder(previous,next,.006f,8);previous=next;}}
 Vec3 sensor=shell_detail(.65f,B_PI*.5f,.025f);b_color(.055f,.07f,.095f);b_soft(sensor,v3(.34f,.035f,.15f),.35f,28,14);
 b_color(nr*.55f,ng*.55f,nb*.55f);b_cylinder(vadd(sensor,v3(-.20f,.035f,0)),vadd(sensor,v3(.20f,.035f,0)),.008f,10);
}

void vehicle_body_render(Server *s){
 bool driver=s->camera_mode==VEHICLE_CAMERA_DRIVER;
 /* Lower hull and central painted roof spine share exact belt vertices with
  * the glass bands. Exterior glass is opaque; driver glass is a late pass. */
 b_color(.28f,.31f,.38f);shell_surface(false);
 if(!driver){b_color(.020f,.032f,.050f);shell_surface(true);shell_caps();}
 else shell_caps();
 canopy_structure();
 exterior_details(s);
}
void vehicle_body_render_glass(Server *s){
 if(s->camera_mode!=VEHICLE_CAMERA_DRIVER)return;
 glDisable(GL_LIGHTING);glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);glDepthMask(GL_FALSE);glColor4f(.035f,.070f,.105f,.13f);shell_surface(true);
 glDepthMask(GL_TRUE);glDisable(GL_BLEND);glEnable(GL_LIGHTING);
}
void vehicle_body_outline(float r,float g,float b){
 glDisable(GL_LIGHTING);glLineWidth(1.15f);
 static const float angles[]={0,.62f,B_PI*.5f,B_PI-.62f,B_PI,B_PI*1.5f};
 for(int a=0;a<6;a++){glColor3f(a==2?r:.22f,a==2?g:.29f,a==2?b:.40f);glBegin(GL_LINE_STRIP);for(int i=0;i<=SHELL_Z;i++){Vec3 p=shell_point(i/(float)SHELL_Z,angles[a]);glVertex3f(p.x,p.y,p.z);}glEnd();}
 glColor3f(.18f,.24f,.32f);for(int i=0;i<=12;i++){float u=i/12.0f;glBegin(GL_LINE_STRIP);for(int j=0;j<=SHELL_RING;j++){Vec3 p=shell_point(u,j*2*B_PI/SHELL_RING);glVertex3f(p.x,p.y,p.z);}glEnd();}
 glLineWidth(1);glEnable(GL_LIGHTING);
}
