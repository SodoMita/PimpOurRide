#include "chrome.h"
#include "vehicle_seat.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <webp/encode.h>
#define PI 3.14159265358979323846f
#define RAD (PI/180.f)
static void color(float r,float g,float b){glColor3f(r,g,b);}
static void quad(float x,float y,float w,float h,float z){glBegin(GL_QUADS);glNormal3f(0,0,1);glVertex3f(x,y,z);glVertex3f(x+w,y,z);glVertex3f(x+w,y+h,z);glVertex3f(x,y+h,z);glEnd();}
static void texture(GLuint tex,float x,float y,float w,float h,float z){
 glDisable(GL_LIGHTING);glEnable(GL_TEXTURE_2D);glBindTexture(GL_TEXTURE_2D,tex);glColor4f(1,1,1,1);glEnable(GL_BLEND);glBlendFunc(GL_ONE,GL_ONE_MINUS_SRC_ALPHA);
 glBegin(GL_QUADS);glTexCoord2f(0,1);glVertex3f(x,y,z);glTexCoord2f(1,1);glVertex3f(x+w,y,z);glTexCoord2f(1,0);glVertex3f(x+w,y+h,z);glTexCoord2f(0,0);glVertex3f(x,y+h,z);glEnd();glDisable(GL_TEXTURE_2D);glDisable(GL_BLEND);glEnable(GL_LIGHTING);
}
static void box(float x,float y,float z,float w,float h,float d){
 float a=x-w/2,b=x+w/2,c=y-h/2,e=y+h/2,f=z-d/2,g=z+d/2;
 glBegin(GL_QUADS);
 glNormal3f(0,0,1);glVertex3f(a,c,g);glVertex3f(b,c,g);glVertex3f(b,e,g);glVertex3f(a,e,g);
 glNormal3f(0,0,-1);glVertex3f(b,c,f);glVertex3f(a,c,f);glVertex3f(a,e,f);glVertex3f(b,e,f);
 glNormal3f(0,1,0);glVertex3f(a,e,g);glVertex3f(b,e,g);glVertex3f(b,e,f);glVertex3f(a,e,f);
 glNormal3f(0,-1,0);glVertex3f(a,c,f);glVertex3f(b,c,f);glVertex3f(b,c,g);glVertex3f(a,c,g);
 glNormal3f(1,0,0);glVertex3f(b,c,g);glVertex3f(b,c,f);glVertex3f(b,e,f);glVertex3f(b,e,g);
 glNormal3f(-1,0,0);glVertex3f(a,c,f);glVertex3f(a,c,g);glVertex3f(a,e,g);glVertex3f(a,e,f);glEnd();
}
static void rbox(float x,float y,float z,float w,float h,float d,float r){
 const int n=48;float px[48],py[48];r=fminf(r,fminf(w,h)*.48f);
 for(int i=0;i<n;i++){int corner=i/12;float angle=(-90+corner*90+(i%12)*90.f/11)*RAD;float cx=(corner==0||corner==1)?w/2-r:-w/2+r;float cy=(corner==0||corner==3)?-h/2+r:h/2-r;px[i]=x+cx+r*cosf(angle);py[i]=y+cy+r*sinf(angle);}
 glBegin(GL_TRIANGLE_FAN);glNormal3f(0,0,1);glVertex3f(x,y,z+d/2);for(int i=0;i<=n;i++)glVertex3f(px[i%n],py[i%n],z+d/2);glEnd();
 glBegin(GL_QUAD_STRIP);for(int i=0;i<=n;i++){int j=i%n;float nx=px[j]-x,ny=py[j]-y;float len=sqrtf(nx*nx+ny*ny);glNormal3f(nx/len,ny/len,0);glVertex3f(px[j],py[j],z-d/2);glVertex3f(px[j],py[j],z+d/2);}glEnd();
}
static void cylinder(Vec3 a,Vec3 b,float radius,int slices){
 Vec3 dir=vnorm(vsub(b,a)),u=vnorm(vcross(dir,fabsf(dir.y)>.9?v3(1,0,0):v3(0,1,0))),v=vcross(dir,u);
 glBegin(GL_QUAD_STRIP);for(int i=0;i<=slices;i++){float t=i*2*PI/slices;Vec3 norm=vadd(vmul(u,cosf(t)),vmul(v,sinf(t))),pa=vadd(a,vmul(norm,radius)),pb=vadd(b,vmul(norm,radius));glNormal3f(norm.x,norm.y,norm.z);glVertex3f(pa.x,pa.y,pa.z);glVertex3f(pb.x,pb.y,pb.z);}glEnd();
 glBegin(GL_TRIANGLE_FAN);glNormal3f(dir.x,dir.y,dir.z);glVertex3f(b.x,b.y,b.z);for(int i=0;i<=slices;i++){float t=i*2*PI/slices;Vec3 p=vadd(b,vmul(vadd(vmul(u,cosf(t)),vmul(v,sinf(t))),radius));glVertex3f(p.x,p.y,p.z);}glEnd();
}
static void torus(float major,float minor,int rings,int sides){
 for(int i=0;i<rings;i++){glBegin(GL_QUAD_STRIP);for(int j=0;j<=sides;j++)for(int k=0;k<2;k++){float a=(i+k)*2*PI/rings,b=j*2*PI/sides;glNormal3f(cosf(a)*cosf(b),sinf(a)*cosf(b),sinf(b));glVertex3f((major+minor*cosf(b))*cosf(a),(major+minor*cosf(b))*sinf(a),minor*sinf(b));}glEnd();}
}
static void glow_line(Vec3 a,Vec3 b,float radius,float r,float g,float bl){
 glDisable(GL_LIGHTING);color(r,g,bl);cylinder(a,b,radius,10);glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE);glDepthMask(GL_FALSE);
 for(int k=3;k>=1;k--){glColor4f(r,g,bl,.035f);cylinder(a,b,radius*(1+k*1.6f),10);}glDepthMask(GL_TRUE);glDisable(GL_BLEND);glEnable(GL_LIGHTING);
}
void scene_neon_color(Server *s,float *r,float *g,float *b){if(s->theme==1){*r=.18;*g=.85;*b=1;}else if(s->theme==2){*r=.64;*g=1;*b=.19;}else{*r=.65;*g=.22;*b=1;}}
#define neon scene_neon_color
static void plane_begin(Plane p){glPushMatrix();glTranslatef(p.center.x,p.center.y,p.center.z);glRotatef(p.yaw,0,1,0);glRotatef(p.pitch,1,0,0);glRotatef(p.roll,0,0,1);}
static void lighting(void){
 glEnable(GL_LIGHTING);glEnable(GL_LIGHT0);glEnable(GL_LIGHT1);glEnable(GL_LIGHT2);glEnable(GL_COLOR_MATERIAL);glColorMaterial(GL_FRONT_AND_BACK,GL_AMBIENT_AND_DIFFUSE);glEnable(GL_NORMALIZE);
 GLfloat ambient[]={.29,.28,.37,1},pos[]={-3,4,4,1},diff[]={.77,.83,.98,1},spec[]={.7,.8,1,1};glLightModelfv(GL_LIGHT_MODEL_AMBIENT,ambient);glLightfv(GL_LIGHT0,GL_POSITION,pos);glLightfv(GL_LIGHT0,GL_DIFFUSE,diff);glLightfv(GL_LIGHT0,GL_SPECULAR,spec);
 GLfloat p1[]={-4,1,-1,1},d1[]={.08,.36,.38,1};glLightfv(GL_LIGHT1,GL_POSITION,p1);glLightfv(GL_LIGHT1,GL_DIFFUSE,d1);
 GLfloat p2[]={4,2,1,1},d2[]={.36,.1,.5,1};glLightfv(GL_LIGHT2,GL_POSITION,p2);glLightfv(GL_LIGHT2,GL_DIFFUSE,d2);
 GLfloat shine[]={.5,.52,.62,1};glMaterialfv(GL_FRONT_AND_BACK,GL_SPECULAR,shine);glMaterialf(GL_FRONT_AND_BACK,GL_SHININESS,65);
}
/* ------------------------------------------------------------------ *
 * environment(): the garage beyond the windshield, as real geometry.
 *
 * This replaces the original implementation, which drew assets/garage.webp
 * as one flat 90x38 textured quad at z=-35 (a poster, not a place: looking
 * left/right with middle-drag slid across a photograph). It now builds the
 * same visual from boxes/cylinders in compositor world space - the car
 * (eye at z=2.8) sits *inside* a 30 m wide, 13.5 m high, 56 m deep
 * concrete hall whose far wall (z=-40) has the mockup's rectangular
 * opening onto a rainy procedural city. Parallax, the existing fixed-
 * function lights (teal LIGHT1 left / violet LIGHT2 right) and the
 * cockpit's own occlusion now all agree, which a backdrop never could.
 *
 * The standalone clients/garage3d program contains the full GLES 3.1
 * version of this scene (scene graph, shaders, physics). This port keeps
 * the compositor's immediate-mode style/helpers (box/cylinder/glow_line)
 * so both backends (SDL/GLX and surfaceless EGL) render it unchanged;
 * only closed-form time functions are used here, so no extra state had
 * to be added to Server. garage.webp is still loaded for the tiny
 * rear-view-mirror reflection in dashboard() - a deliberate homage, not
 * the world geometry any more.
 * ------------------------------------------------------------------ */
static float garage_hash(int n){float x=sinf((float)n*127.1f)*43758.5453f;return x-floorf(x);}

static void garage_city(Server *s){
 /* The old flat skyline has been replaced by the same connected, collidable
  * city used by navigation. Its real geometry is visible through the actual
  * garage doorway and remains present when the car drives outside. */
 city_render(s);
}

static void garage_reflection(float x,float z,float r,float g,float b,float alpha){
 /* Additive streak on the wet floor running from a tube towards the car;
  * alpha fades along its length (per-vertex colour does the gradient). */
 glBegin(GL_QUADS);glNormal3f(0,1,0);
 glColor4f(r,g,b,alpha);glVertex3f(x-.16f,-1.485f,z);glVertex3f(x+.16f,-1.485f,z);
 glColor4f(r,g,b,.015f);glVertex3f(x+.30f,-1.485f,z+10.f);glVertex3f(x-.30f,-1.485f,z+10.f);glEnd();
}

/* The pre-Garage3D backdrop, kept as a selectable mode (config/GUI
 * background=texture): the flat garage.webp poster quad + wire grid floor,
 * byte-for-byte the original environment(). Cheaper than the 3D hall and a
 * useful A/B comparison for what real geometry buys you. */
static void environment_flat(Server *s){
 glDisable(GL_LIGHTING);glDisable(GL_DEPTH_TEST);texture(s->garage_tex,-45,-17.8,90,38.2,-35);glEnable(GL_DEPTH_TEST);
 color(.021,.019,.036);glBegin(GL_QUADS);glNormal3f(0,1,0);glVertex3f(-50,-1.5,-35);glVertex3f(50,-1.5,-35);glVertex3f(50,-1.5,35);glVertex3f(-50,-1.5,35);glEnd();
 glDisable(GL_LIGHTING);glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);glColor4f(.28,.28,.5,.2);glBegin(GL_LINES);for(int i=-25;i<=25;i+=3){glVertex3f(i,-1.495,-35);glVertex3f(i,-1.495,25);glVertex3f(-35,-1.495,i);glVertex3f(35,-1.495,i);}glEnd();glDisable(GL_BLEND);glEnable(GL_LIGHTING);
}

static void environment(Server *s){
 if(s->bg_mode==CD_BG_NONE)return;               /* backdrop disabled */
 if(s->bg_mode==CD_BG_TEXTURE){environment_flat(s);return;}
 const float floor_y=-1.5f,ceil_y=12.f,wall_x=15.f,far_z=-40.f,near_z=16.f,center_z=-12.f,len=56.f;
 const float mid_y=(floor_y+ceil_y)*.5f,height=ceil_y-floor_y;
 static const float pillar_z[]={-4.f,-11.5f,-19.f,-26.5f,-34.f};
 static const float bay_z[]={-7.75f,-15.25f,-22.75f,-30.25f};
 /* Fog is scoped to the hall: enabled here, disabled before the cockpit
  * is drawn, so the dashboard at z~0 stays crisp while the far wall
  * melts into the same violet as the city opening. */
 GLfloat fog_color[]={.035f,.03f,.09f,1};
 glEnable(GL_FOG);glFogi(GL_FOG_MODE,GL_EXP2);glFogfv(GL_FOG_COLOR,fog_color);glFogf(GL_FOG_DENSITY,.010f);

 garage_city(s);

 /* Shell. Boxes (not single quads) so wall/pillar edges catch light and
  * thickness reads correctly when the driver looks sideways. */
 color(.020f,.027f,.048f);box(0,floor_y-.05f,center_z,wall_x*2,.1f,len);          /* wet floor slab */
 color(.035f,.035f,.052f);box(0,ceil_y,center_z,wall_x*2,.5f,len);               /* ceiling */
 color(.070f,.085f,.105f);box(-wall_x,mid_y,center_z,.6f,height,len);            /* left wall */
 color(.070f,.085f,.105f);box( wall_x,mid_y,center_z,.6f,height,len);            /* right wall */
 /* Far wall around the city opening (half-width 6.5, lintel above y=6). */
 color(.065f,.078f,.098f);box(-10.75f,mid_y,far_z,8.5f,height,.6f);
 color(.065f,.078f,.098f);box( 10.75f,mid_y,far_z,8.5f,height,.6f);
 color(.065f,.078f,.098f);box(0,9.f,far_z,13.f,6.f,.6f);
 color(.10f,.11f,.14f);box(0,6.35f,far_z+.45f,13.6f,.7f,.8f);                     /* shutter housing */
 color(.065f,.078f,.098f);box(0,mid_y,near_z,wall_x*2,height,.6f);                /* rear wall (180-degree monitor sits inside the hall) */

 /* Structure: pillars, transverse beams, longitudinal beams, duct, pipes. */
 for(int i=0;i<5;i++){
  color(.055f,.062f,.082f);box(-14.4f,mid_y,pillar_z[i],1.6f,height,1.4f);
  color(.055f,.062f,.082f);box( 14.4f,mid_y,pillar_z[i],1.6f,height,1.4f);
  color(.040f,.042f,.062f);box(0,ceil_y-.7f,pillar_z[i],wall_x*2,1.2f,1.f);
 }
 color(.040f,.042f,.062f);box(-7,ceil_y-.6f,center_z,1,.8f,len);box(7,ceil_y-.6f,center_z,1,.8f,len);
 color(.075f,.082f,.105f);box(0,ceil_y-.75f,center_z,2.6f,.7f,len);              /* central duct */
 for(int i=0;i<7;i++){color(.16f,.18f,.22f);box(0,ceil_y-1.13f,-2.f-i*5.f,2.f,.08f,1.2f);}
 color(.22f,.25f,.30f);cylinder(v3(14,ceil_y-1.3f,near_z-1),v3(14,ceil_y-1.3f,-39),.13f,12);
 color(.13f,.15f,.19f);cylinder(v3(14,ceil_y-1.65f,near_z-1),v3(14,ceil_y-1.65f,-39),.09f,12);
 color(.22f,.25f,.30f);cylinder(v3(-14,ceil_y-1.3f,near_z-1),v3(-14,ceil_y-1.3f,-39),.13f,12);
 /* Shallow wall panels between pillars (the mockup's concrete bays). */
 for(int i=0;i<4;i++)for(int side=-1;side<=1;side+=2){
  color(.082f,.096f,.118f);box(side*14.62f,mid_y,bay_z[i],.14f,height-2.f,5.6f);
 }

 /* Neon tubes: cyan left, magenta right, two per bay (glow_line gives a
  * core + additive halo cylinders). One right-hand tube flickers, in
  * 12 Hz hash buckets exactly like the standalone client. */
 int flicker_bucket=(int)(s->time*12);
 float flicker=garage_hash(flicker_bucket*17+3)<.12f?.15f:1.f;
 glDisable(GL_LIGHTING);glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE);glDepthMask(GL_FALSE);
 for(int i=0;i<4;i++)for(int side=-1;side<=1;side+=2)
  for(int t=-1;t<=1;t+=2){
   float z=bay_z[i]+t*1.6f;
   float r=side<0?.15f:.92f,g=side<0?.95f:.25f,b=side<0?.90f:1.f;
   if(side>0&&i==0&&t<0){r*=flicker;g*=flicker;b*=flicker;}
   garage_reflection(side*14.3f,z,r,g,b,.16f);
  }
 glDepthMask(GL_TRUE);glDisable(GL_BLEND);glEnable(GL_LIGHTING);
 for(int i=0;i<4;i++)for(int side=-1;side<=1;side+=2)
  for(int t=-1;t<=1;t+=2){
   float z=bay_z[i]+t*1.6f,x=side*14.5f;
   float r=side<0?.15f:.92f,g=side<0?.95f:.25f,b=side<0?.90f:1.f;
   if(side>0&&i==0&&t<0){r*=flicker;g*=flicker;b*=flicker;}
   glow_line(v3(x,.8f,z),v3(x,5.6f,z),.055f,r,g,b);
  }

 /* Hanging work lamp on a closed-form pendulum (the standalone client
  * integrates the ODE; here a backdrop prop should not add Server state). */
 float swing_x=.30f*sinf((float)s->time*.9f),swing_z=.22f*sinf((float)s->time*.75f+1.1f);
 Vec3 pivot=v3(0,ceil_y-.5f,-17),lamp_end=v3(sinf(swing_z)*4.2f,ceil_y-.5f-cosf(swing_x)*cosf(swing_z)*4.2f,-17+sinf(swing_x)*4.2f);
 color(.05f,.05f,.07f);cylinder(pivot,lamp_end,.035f,8);
 color(.25f,.28f,.34f);cylinder(lamp_end,v3(lamp_end.x,lamp_end.y-.35f,lamp_end.z),.34f,16);
 glow_line(v3(lamp_end.x,lamp_end.y-.38f,lamp_end.z),v3(lamp_end.x,lamp_end.y-.55f,lamp_end.z),.13f,1.f,.88f,.62f);

 /* A few props parked against the far walls; the centre aisle stays
  * empty, as in the mockup (and clear of the virtual monitors). */
 color(.09f,.14f,.17f);cylinder(v3(-12.2f,floor_y,-28),v3(-12.2f,floor_y+1.3f,-28),.5f,16);
 color(.09f,.14f,.17f);cylinder(v3(-11.2f,floor_y,-27.4f),v3(-11.2f,floor_y+1.3f,-27.4f),.5f,16);
 color(.14f,.11f,.08f);box(12.1f,floor_y+.65f,-24,1.5f,1.3f,1.5f);box(12.3f,floor_y+1.8f,-24.1f,1.1f,1.f,1.1f);

 /* Doorway mist: two soft additive sheets drifting across the opening. */
 glDisable(GL_LIGHTING);glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE);glDepthMask(GL_FALSE);
 float drift=sinf((float)s->time*.10f)*1.4f;
 glColor4f(.42f,.30f,.72f,.06f);quad(-6.5f+drift,floor_y+.3f,13,3.2f,far_z+1.2f);
 glColor4f(.42f,.30f,.72f,.045f);quad(-6.5f-drift,floor_y+.8f,13,2.6f,far_z+2.6f);
 glDepthMask(GL_TRUE);glDisable(GL_BLEND);glEnable(GL_LIGHTING);

 glDisable(GL_FOG);
}
static void dashboard(Server *s){
 float nr,ng,nb;neon(s,&nr,&ng,&nb);
 /* The complete dashboard/console/shifter uses the same forward rig offset as
  * its buttons, displays and real-seat cameras. This is shared geometry, not
  * a driver-camera translation. */
 glPushMatrix();glTranslatef(0,0,-VEHICLE_COCKPIT_FORWARD);
 /* Deep padded dashboard: bevelled profile, rather than a flat HUD backdrop. */
 color(.115,.075,.175);glEnable(GL_TEXTURE_2D);glBindTexture(GL_TEXTURE_2D,s->carbon_tex);
 glBegin(GL_QUAD_STRIP);float profile[][2]={{-1.0f,.18f},{-.72f,.34f},{-.36f,.31f},{.2f,.14f},{.88f,-.24f},{1.5f,-.45f},{1.7f,-.72f}};
 for(int i=0;i<7;i++){float ny=.9f,nz=.4f;if(i>3){ny=.55;nz=.8;}glNormal3f(0,ny,nz);glTexCoord2f(0,i*.38f);glVertex3f(-2.60,profile[i][1],profile[i][0]);glTexCoord2f(5,i*.38f);glVertex3f(2.60,profile[i][1],profile[i][0]);}glEnd();glDisable(GL_TEXTURE_2D);
 /* Rich violet edge insert and chrome piping. */
 color(.28,.058,.44);box(0,-.11,.37,5.0,.19,.21);
 color(.43,.48,.57);cylinder(v3(-2.50,.18,-.49),v3(2.50,.18,-.49),.018,12);
 glow_line(v3(-2.47,.195,-.465),v3(2.47,.195,-.465),.008,nr,ng,nb);
 glow_line(v3(-2.43,-.23,.56),v3(2.43,-.23,.56),.009,nr,ng,nb);
 /* Purple hand-stitched seams, following the hood. */
 glDisable(GL_LIGHTING);color(.56,.31,.69);glLineWidth(1.3f);glBegin(GL_LINES);for(float x=-2.45;x<2.45;x+=.07f){glVertex3f(x,.317,-.22);glVertex3f(x+.035,.317,-.22);}glEnd();glEnable(GL_LIGHTING);
 /* Air vents in a machined surround. */
 for(int side=-1;side<=1;side+=2){glPushMatrix();glTranslatef(side*2.15,.19,-.46);glRotatef(-22,1,0,0);color(.24,.27,.33);rbox(0,0,0,.62,.23,.05,.045);color(.022,.027,.037);rbox(0,0,.031,.55,.16,.014,.023);for(int i=-3;i<=3;i++){color(.19,.22,.28);box(i*.068,0,.055,.012,.14,.025);}glPopMatrix();}
 /* Instrument binnacle. monitor_plane() is already in shared cockpit space;
  * cancel this function's parent offset for the physical binnacle substrate. */
 Plane gauge=monitor_plane(CD_CLUSTER_SLOT);gauge.center.z+=VEHICLE_COCKPIT_FORWARD;plane_begin(gauge);color(.025,.03,.043);rbox(0,0,-.05,1.62,.76,.14,.18);color(.32,.34,.44);rbox(0,0,.005,1.51,.63,.035,.10);
 /* Empty dark substrate only. The visible gauges are the real Wayland
  * cluster client's committed surface, drawn by draw_monitor below. */
 glDisable(GL_LIGHTING);color(.010,.016,.026);quad(-gauge.width/2,-gauge.height/2,gauge.width,gauge.height,.023);glEnable(GL_LIGHTING);glPopMatrix();
 /* Passenger-side brushed plate, cut into the dash. */
 Plane plate={.center={2.04,-.015,.13},.width=.82,.height=.30,.yaw=-8,.pitch=-24};plane_begin(plate);color(.26,.29,.38);rbox(0,0,0,.91,.37,.028,.025);texture(s->plate_tex,-.41,-.15,.82,.30,.02);glPopMatrix();
 /* Sloped touch-and-switch center console; each key has its own pick plane. */
 Plane console={.center={.78,.33,.48},.width=1.56,.height=.90,.yaw=-5,.pitch=-28};plane_begin(console);
 color(.37,.39,.49);rbox(0,0,-.028,1.64,.99,.12,.09);color(.035,.040,.059);rbox(0,0,.033,1.57,.92,.027,.065);texture(s->console_tex,-.78,-.45,1.56,.9,.05);
 glow_line(v3(-.70,-.425,.063),v3(.70,-.425,.063),.004,nr,ng,nb);glPopMatrix();
 /* Canopy structure is rendered from vehicle_canopy_point() in both camera
  * modes; no separate tall driver-only roof or straight header is needed. */
 /* Rear-view mirror and bracket stay anchored to the fixed windshield header,
  * so cancel the dashboard's parent translation for this one attached part. */
 Vec3 header=vehicle_canopy_point(.45f,1.0f,1,NULL);header.z+=VEHICLE_COCKPIT_FORWARD;
 color(.30,.34,.42);cylinder(v3(0,header.y-.025f,header.z),v3(0,header.y-.15f,header.z+.09f),.035,16);
 color(.24,.28,.36);cylinder(v3(0,header.y-.15f,header.z+.09f),v3(0,header.y-.21f,header.z+.12f),.024,14);
 glPushMatrix();glTranslatef(0,header.y-.23f,header.z+.12f);glRotatef(-5,1,0,0);color(.40,.44,.52);rbox(0,0,0,.85,.26,.07,.06);color(.015,.028,.04);rbox(0,0,.043,.78,.20,.025,.04);texture(s->garage_tex,-.37,-.08,.74,.16,.06);glPopMatrix();
 /* Door upholstery, handles, shells and glass are one rigid assembly in
  * vehicle_scene.c; nothing door-shaped remains statically in the cabin. */
 /* Polished shifter on the transmission tunnel. */
 color(.045,.04,.072);rbox(.66,-.62,1.02,.70,.36,.29,.10);color(.28,.32,.40);cylinder(v3(.66,-.65,.96),v3(.66,-.30,.96),.025,20);glPushMatrix();glTranslatef(.66,-.28,.96);color(.65,.64,.74);torus(.054,.049,28,16);glPopMatrix();
 glPopMatrix();
}
static Plane steering_plane(void){return (Plane){.center={-.84f,.28f,1.10f-VEHICLE_COCKPIT_FORWARD},.width=1.12f,.height=1.12f,.pitch=-18};}
static void steering_wheel(Server *s){
 float nr,ng,nb;neon(s,&nr,&ng,&nb);glPushMatrix();glTranslatef(-.84,.28,1.10-VEHICLE_COCKPIT_FORWARD);glRotatef(-18,1,0,0);
 /* Spin in the wheel's own XY plane, then tilt the complete assembly. Applying
  * Z before the tilt would wobble the rim instead of rotating the control. */
 float wheel_angle=s->steering_mode==VEHICLE_STEER_DRIVE?s->vehicle.wheel_angle:s->wheel_client_angle;
 glRotatef(wheel_angle,0,0,1);glScalef(.74,.74,.74);
 /* Thick violet leather rim, bright machined inner and outer rings. */
 color(.065,.033,.098);torus(.595,.078,96,16);color(.37,.35,.46);torus(.67,.009,96,10);color(.51,.54,.63);torus(.515,.012,96,10);color(.28,.067,.41);torus(.595,.063,96,12);
 if(s->wheel_hover||s->wheel_drag){glDisable(GL_LIGHTING);color(nr,ng,nb);torus(.675,.010,96,8);glEnable(GL_LIGHTING);}
 /* Grip pads at nine and three o'clock, wrapped in dark leather. */
 for(int side=-1;side<=1;side+=2){color(.033,.033,.051);rbox(side*.58,0,.028,.145,.26,.096,.055);glDisable(GL_LIGHTING);color(.31,.20,.38);glBegin(GL_LINES);for(int i=-4;i<=4;i++){glVertex3f(side*.58-.04,i*.023,.082);glVertex3f(side*.58+.04,i*.023,.082);}glEnd();glEnable(GL_LIGHTING);}
 /* Three swept chrome spokes and hollow screw details. */
 for(int i=0;i<3;i++){glPushMatrix();glRotatef(i==0?0:i==1?180:270,0,0,1);color(.42,.47,.57);glBegin(GL_QUADS);glNormal3f(0,0,1);glVertex3f(.11,-.115,.035);glVertex3f(.51,-.09,.035);glVertex3f(.51,.066,.035);glVertex3f(.11,.13,.035);glEnd();color(.032,.039,.054);rbox(.34,0,.047,.24,.036,.015,.015);glPopMatrix();}
 color(.53,.56,.64);cylinder(v3(0,0,-.12),v3(0,0,.11),.197,64);color(.025,.029,.045);cylinder(v3(0,0,.113),v3(0,0,.126),.172,64);texture(s->hub_tex,-.155,-.155,.31,.31,.128);
 for(int i=0;i<6;i++){float a=i*2*PI/6;glPushMatrix();glTranslatef(.185*cosf(a),.185*sinf(a),.115);color(.065,.075,.10);torus(.010,.004,14,6);glPopMatrix();}
 glDisable(GL_LIGHTING);color(nr,ng,nb);quad(-.016,.524,.032,.115,.06);glEnable(GL_LIGHTING);glPopMatrix();
}
static void draw_buttons(Server *s){for(int i=0;i<6;i++){DashButton *b=&s->buttons[i];plane_begin(b->plane);color(.16,.19,.26);rbox(0,0,-.02,b->plane.width+.02,b->plane.height+.02,.04,.018);color(.04,.06,.085);rbox(0,0,.005,b->plane.width,b->plane.height,.022,.013);texture(b->texture,-b->plane.width/2,-b->plane.height/2,b->plane.width,b->plane.height,.019);
 if(s->hover_button==i){glDisable(GL_LIGHTING);glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE);glColor4f(.25,.85,.70,.18);quad(-b->plane.width/2,-b->plane.height/2,b->plane.width,b->plane.height,.021);glDisable(GL_BLEND);glEnable(GL_LIGHTING);}glPopMatrix();}}
/* The settings cog: a real little gear (torus rim + 8 teeth + hub + spokes)
 * medallioned onto the sloped dash between wheel and console. It
 * idles at a slow spin so it is discoverable, brightens on hover, and does
 * a fast twirl after a click (cog_spin_until, set by app_open_settings).
 * Angle integration lives in main.c beside the hover pick; this only draws
 * from Server state. Clicking it opens bin/chromedrive-config. */
static void draw_cog(Server *s){
 float nr,ng,nb;neon(s,&nr,&ng,&nb);
 bool hot=s->cog_hover||s->time<s->cog_spin_until;
 plane_begin(s->cog);
 color(.05,.045,.09);cylinder(v3(0,0,-.030),v3(0,0,-.004),.118,48);       /* recessed boss */
 color(.30,.32,.40);torus(.113,.006,64,10);                               /* machined rim ring */
 glPushMatrix();glRotatef(s->cog_angle,0,0,1);
 if(hot)color(.58,.63,.76);else color(.42,.46,.56);
 torus(.080,.021,64,14);
 for(int i=0;i<8;i++){glPushMatrix();glRotatef(i*45.f,0,0,1);glTranslatef(0,.096f,0);box(0,0,0,.034f,.038f,.042f);glPopMatrix();}
 for(int i=0;i<4;i++){glPushMatrix();glRotatef(22.5f+i*90.f,0,0,1);glTranslatef(0,.048f,0);box(0,0,0,.020f,.058f,.030f);glPopMatrix();}
 cylinder(v3(0,0,-.014),v3(0,0,.020),.030,32);
 glPopMatrix();
 /* Non-rotating center cap + neon pip, so the rotation reads at a glance. */
 color(.06,.07,.11);cylinder(v3(0,0,.020),v3(0,0,.030),.020,32);
 glDisable(GL_LIGHTING);if(hot)color(.55,1,.85);else color(nr,ng,nb);quad(-.006,-.006,.012,.012,.031);
 if(hot){glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE);glColor4f(.25,.85,.70,.15);quad(-.12,-.12,.24,.24,.033);glDisable(GL_BLEND);}
 glEnable(GL_LIGHTING);glPopMatrix();
}
typedef struct {float scale,w,h,z0,zstep;int layer;} DrawSurface;
static void transformed_uv(int transform,float x,float y,float *u,float *v){float a=x,b=y;switch(transform&3){case 1:a=1-y;b=x;break;case 2:a=1-x;b=1-y;break;case 3:a=y;b=1-x;break;}if(transform>=4)a=1-a;*u=a;*v=b;}
static void draw_app_surface(Surface *p,int sx,int sy,void *data){
 DrawSurface *d=data;float x=-d->w/2+sx*d->scale,y=d->h/2-sy*d->scale,w=p->width*d->scale,h=p->height*d->scale,z=d->z0+d->zstep*d->layer++;
 glDisable(GL_LIGHTING);glEnable(GL_TEXTURE_2D);glBindTexture(GL_TEXTURE_2D,p->texture);glEnable(GL_BLEND);glBlendFunc(GL_ONE,GL_ONE_MINUS_SRC_ALPHA);glColor4f(1,1,1,1);
 float coords[4][2]={{0,0},{1,0},{1,1},{0,1}},vx[]={x,x+w,x+w,x},vy[]={y,y,y-h,y-h};glBegin(GL_QUADS);for(int i=0;i<4;i++){float u,v;transformed_uv(p->transform,coords[i][0],coords[i][1],&u,&v);glTexCoord2f(u,v);glVertex3f(vx[i],vy[i],z);}glEnd();glDisable(GL_BLEND);glDisable(GL_TEXTURE_2D);glEnable(GL_LIGHTING);
}
static float monitor_iso_scale(Plane p,int slot){
 Plane base=monitor_plane(slot);float kx=p.width/base.width,ky=p.height/base.height;
 return sqrtf(fmaxf(kx*ky,.0001f));
}
static void handles_button_rect(Plane p,float k,float *x,float *y,float *w,float *h){
 *w=.36f*k;*h=.105f*k;*x=p.width*.11f-*w*.5f;*y=p.height*.5f+.026f*k;
}
static bool handles_button_at(Plane p,float k,float lx,float ly){
 float x,y,w,h;handles_button_rect(p,k,&x,&y,&w,&h);
 return lx>=x&&lx<=x+w&&ly>=y&&ly<=y+h;
}
static void arrange_handle_point(Plane p,float k,int handle,float *x,float *y){
 float hw=p.width*.5f,hh=p.height*.5f;
 switch(handle){
 case CD_HANDLE_MOVE:*x=.32f*k;*y=-.32f*k;break;
 case CD_HANDLE_AXIS_X:*x=.42f*k;*y=0;break;
 case CD_HANDLE_AXIS_Y:*x=0;*y=.42f*k;break;
 case CD_HANDLE_AXIS_Z:*x=0;*y=0;break;
 case CD_HANDLE_ROTATE_YAW:*x=hw+.28f*k;*y=0;break;
 case CD_HANDLE_ROTATE_PITCH:*x=-hw-.28f*k;*y=0;break;
 case CD_HANDLE_ROTATE_ROLL:*x=0;*y=hh+.30f*k;break;
 case CD_HANDLE_SCALE:*x=hw-.34f*k;*y=-hh+.34f*k;break; /* just inside SE corner: outside would hide behind the dash in focus view */
 case CD_HANDLE_RESIZE_NW:*x=-hw;*y=hh;break;
 case CD_HANDLE_RESIZE_N:*x=0;*y=hh;break;
 case CD_HANDLE_RESIZE_NE:*x=hw;*y=hh;break;
 case CD_HANDLE_RESIZE_E:*x=hw;*y=0;break;
 case CD_HANDLE_RESIZE_SE:*x=hw;*y=-hh;break;
 case CD_HANDLE_RESIZE_S:*x=0;*y=-hh;break;
 case CD_HANDLE_RESIZE_SW:*x=-hw;*y=-hh;break;
 case CD_HANDLE_RESIZE_W:*x=-hw;*y=0;break;
 default:*x=0;*y=0;break;
 }
}
static bool arrange_z_handle_hit(Plane p,float k,Vec3 eye,Vec3 ray,float *lx,float *ly,float *distance){
 Vec3 normal;plane_axes(p,NULL,NULL,&normal);
 Plane knob=p;knob.center=vadd(p.center,vmul(normal,.44f*k));
 float u,v,d;
 if(!plane_hit(knob,eye,ray,&u,&v,&d,false))return false;
 if(lx)*lx=(u-.5f)*p.width;if(ly)*ly=(.5f-v)*p.height;if(distance)*distance=d;
 return true;
}
static int arrange_handle_at(Plane p,float k,float lx,float ly,bool z_hit,float zlx,float zly){
 static const int ids[]={CD_HANDLE_MOVE,CD_HANDLE_AXIS_X,CD_HANDLE_AXIS_Y,
  CD_HANDLE_ROTATE_YAW,CD_HANDLE_ROTATE_PITCH,CD_HANDLE_ROTATE_ROLL,CD_HANDLE_SCALE,
  CD_HANDLE_RESIZE_NW,CD_HANDLE_RESIZE_N,CD_HANDLE_RESIZE_NE,CD_HANDLE_RESIZE_E,
  CD_HANDLE_RESIZE_SE,CD_HANDLE_RESIZE_S,CD_HANDLE_RESIZE_SW,CD_HANDLE_RESIZE_W};
 float tol=.115f*k,best=tol*tol;int found=CD_HANDLE_NONE;
 /* The blue Z knob is raised .44k along the plane normal; pick it on a
  * parallel plane at that offset instead of at the base-plane centre. */
 if(z_hit){float d=zlx*zlx+zly*zly;if(d<=best){best=d;found=CD_HANDLE_AXIS_Z;}}
 for(unsigned i=0;i<sizeof(ids)/sizeof(ids[0]);i++){float x,y;arrange_handle_point(p,k,ids[i],&x,&y);float dx=lx-x,dy=ly-y,d=dx*dx+dy*dy;if(d<=best){best=d;found=ids[i];}}
 if(found!=CD_HANDLE_NONE)return found;
 if(lx>=0&&lx<=.42f*k&&fabsf(ly)<=.055f*k)return CD_HANDLE_AXIS_X;
 if(ly>=0&&ly<=.42f*k&&fabsf(lx)<=.055f*k)return CD_HANDLE_AXIS_Y;
 return CD_HANDLE_NONE;
}
static void handle_box(float x,float y,float z,float size,float r,float g,float b){
 color(.015,.02,.035);box(x,y,z,size*1.28f,size*1.28f,size*.30f);
 color(r,g,b);box(x,y,z+size*.10f,size,size,size*.34f);
}
static void handle_ring(float x,float y,float z,float radius,float r,float g,float b){
 glPushMatrix();glTranslatef(x,y,z);color(r,g,b);torus(radius,radius*.17f,36,8);glPopMatrix();
}
static void draw_arrange_handles(Server *s,Plane p,float k){
 (void)s;float z=.052f*k,size=.105f*k,x,y;
 /* Window-border resize grips: white corner/edge squares. */
 for(int id=CD_HANDLE_RESIZE_NW;id<=CD_HANDLE_RESIZE_W;id++){arrange_handle_point(p,k,id,&x,&y);bool corner=id==CD_HANDLE_RESIZE_NW||id==CD_HANDLE_RESIZE_NE||id==CD_HANDLE_RESIZE_SE||id==CD_HANDLE_RESIZE_SW;handle_box(x,y,z,size*(corner?1.08f:.88f),.94,.96,1);}
 /* Local axes from the centre. The blue Z knob sits toward the viewer;
  * the amber square offset from centre is unconstrained plane movement. */
 glow_line(v3(0,0,z),v3(.42f*k,0,z),.006f*k,1,.22,.22);
 glow_line(v3(.42f*k,0,z),v3(.35f*k,.055f*k,z),.005f*k,1,.22,.22);glow_line(v3(.42f*k,0,z),v3(.35f*k,-.055f*k,z),.005f*k,1,.22,.22);
 glow_line(v3(0,0,z),v3(0,.42f*k,z),.006f*k,.25,1,.35);
 glow_line(v3(0,.42f*k,z),v3(.055f*k,.35f*k,z),.005f*k,.25,1,.35);glow_line(v3(0,.42f*k,z),v3(-.055f*k,.35f*k,z),.005f*k,.25,1,.35);
 glow_line(v3(0,0,z),v3(0,0,.44f*k),.006f*k,.30,.55,1);
 arrange_handle_point(p,k,CD_HANDLE_MOVE,&x,&y);handle_box(x,y,z,size*1.08f,1,.62,.16);
 glow_line(v3(x-.035f*k,y,z+.025f*k),v3(x+.035f*k,y,z+.025f*k),.004f*k,.04,.05,.07);glow_line(v3(x,y-.035f*k,z+.025f*k),v3(x,y+.035f*k,z+.025f*k),.004f*k,.04,.05,.07);
 handle_box(.42f*k,0,z,size*.82f,1,.22,.22);handle_box(0,.42f*k,z,size*.82f,.25,1,.35);handle_box(0,0,.44f*k,size*.88f,.30,.55,1);
 /* Rotation rings just outside the frame; their colours match the axis
  * they rotate around (yaw=Y/green, pitch=X/red, roll=Z/blue). */
 arrange_handle_point(p,k,CD_HANDLE_ROTATE_YAW,&x,&y);handle_ring(x,y,z,.105f*k,.25,1,.35);
 arrange_handle_point(p,k,CD_HANDLE_ROTATE_PITCH,&x,&y);handle_ring(x,y,z,.105f*k,1,.22,.22);
 arrange_handle_point(p,k,CD_HANDLE_ROTATE_ROLL,&x,&y);handle_ring(x,y,z,.105f*k,.30,.55,1);
 /* Magenta diamond: uniform scale, preserving the current aspect. */
 arrange_handle_point(p,k,CD_HANDLE_SCALE,&x,&y);glPushMatrix();glTranslatef(x,y,z);glRotatef(45,0,0,1);color(.015,.02,.035);box(0,0,0,size*1.22f,size*1.22f,size*.28f);color(.94,.30,.92);box(0,0,size*.09f,size,size,size*.32f);glPopMatrix();
}
static void draw_monitor(Server *s,int i){
 Monitor *m=&s->monitors[i];if(!m->surface)return;
 Plane p=m->plane;float w=p.width,h=p.height,k=monitor_iso_scale(p,i);
 bool active=s->keyboard_focus==m->surface,cluster=i==CD_CLUSTER_SLOT;
 bool arranging=!cluster&&s->arrange_mode&&s->arrange_slot==i;
 plane_begin(p);
 if(!cluster){
  /* Hardware belongs to the named car part, not a generic floor stand. */
  MonitorMount mount=monitor_mount(i);color(.24,.28,.35);
  if(mount==MOUNT_DASH_SLIDE){
   for(int side=-1;side<=1;side+=2){cylinder(v3(side*w*.32,-h/2,-.075),v3(side*w*.32,-h/2-.58,-.075),.028,12);color(.40,.43,.50);box(side*w*.32,-h/2-.58,-.075,.15,.045,.16);color(.24,.28,.35);}
  }else if(mount==MOUNT_DOOR_SLIDE){
   float side=i==3?-1:1;cylinder(v3(side*w*.43,0,-.08),v3(side*(w*.5+.20),0,-.08),.045,14);box(side*(w*.5+.20),0,-.08,.12,.34,.18);
  }else if(mount==MOUNT_ROOF_FLIP){
   cylinder(v3(-w*.34,h/2+.08,-.07),v3(w*.34,h/2+.08,-.07),.045,16);cylinder(v3(0,h/2+.08,-.07),v3(0,h/2+.20,-.07),.035,12);
  }
  color(.055,.075,.105);rbox(0,.075,-.056,w+.12,h+.25,.10,.065);
  color(.018,.027,.038);rbox(0,.075,-.003,w+.086,h+.219,.038,.055);
  glDisable(GL_LIGHTING);color(.010,.016,.026);quad(-w/2,-h/2,w,h,.019);glEnable(GL_LIGHTING);
  float nr,ng,nb;neon(s,&nr,&ng,&nb);
  if(arranging)glow_line(v3(-w/2,-h/2-.022,.025),v3(w/2,-h/2-.022,.025),.005,1,.62,.16);
  else if(active)glow_line(v3(-w/2,-h/2-.022,.025),v3(w/2,-h/2-.022,.025),.004,.35,.92,.74);
  else glow_line(v3(-w/2,-h/2-.022,.025),v3(w/2,-h/2-.022,.025),.003,nr*.65,ng*.7,nb*.7);
  ui_monitor_label(s,i);texture(m->label,-w/2,h/2+.011,w,.137,.024);
  {float bx,by,bw,bh;handles_button_rect(p,k,&bx,&by,&bw,&bh);if(s->handles_tex)texture(s->handles_tex,bx,by,bw,bh,.032*k);if(arranging&&s->arrange_handles){float z=.040*k,r=.004*k;glow_line(v3(bx,by,z),v3(bx+bw,by,z),r,1,.62,.16);glow_line(v3(bx+bw,by,z),v3(bx+bw,by+bh,z),r,1,.62,.16);glow_line(v3(bx+bw,by+bh,z),v3(bx,by+bh,z),r,1,.62,.16);glow_line(v3(bx,by+bh,z),v3(bx,by,z),r,1,.62,.16);}}
 }
 glEnable(GL_STENCIL_TEST);glClear(GL_STENCIL_BUFFER_BIT);glStencilMask(0xff);glStencilFunc(GL_ALWAYS,1,0xff);glStencilOp(GL_KEEP,GL_KEEP,GL_REPLACE);glColorMask(GL_FALSE,GL_FALSE,GL_FALSE,GL_FALSE);glDepthMask(GL_FALSE);glDisable(GL_DEPTH_TEST);quad(-w/2,-h/2,w,h,.021*k);glEnable(GL_DEPTH_TEST);glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);glDepthMask(GL_TRUE);glStencilFunc(GL_EQUAL,1,0xff);glStencilOp(GL_KEEP,GL_KEEP,GL_KEEP);glStencilMask(0);
 float scale=fminf(w/m->surface->width,h/m->surface->height);DrawSurface d={.scale=scale,.w=m->surface->width*scale,.h=m->surface->height*scale,.z0=.024*k,.zstep=.0007*k};server_each_surface(m->surface,draw_app_surface,&d);
 glStencilMask(0xff);glDisable(GL_STENCIL_TEST);
 if(arranging){
  /* Amber frame + local RGB axes (right/up/normal) make the current
   * Arrange target and its orientation legible while it is transformed. */
  float z=.038*k,r=.005*k;
  glow_line(v3(-w/2,-h/2,z),v3(w/2,-h/2,z),r,1,.62,.16);
  glow_line(v3(w/2,-h/2,z),v3(w/2,h/2,z),r,1,.62,.16);
  glow_line(v3(w/2,h/2,z),v3(-w/2,h/2,z),r,1,.62,.16);
  glow_line(v3(-w/2,h/2,z),v3(-w/2,-h/2,z),r,1,.62,.16);
  glow_line(v3(0,0,z),v3(.30*k,0,z),r,1,.22,.22);
  glow_line(v3(0,0,z),v3(0,.30*k,z),r,.25,1,.35);
  glow_line(v3(0,0,z),v3(0,0,.34*k),r,.30,.55,1);
  if(s->arrange_handles)draw_arrange_handles(s,p,k);
 }
 glPopMatrix();
}
static void draw_windshield_hud(Server *s){
 if(!s->hud_visible||!s->windshield_tex)return;
 /* A transparent projection on the actual glass plane, not framebuffer
  * chrome. Its centre is intentionally quiet so the windshield app remains
  * readable; speed, pack and drive state sit around the edges. */
 Plane p=windshield_hud_plane();
 glDepthMask(GL_FALSE);plane_begin(p);texture(s->windshield_tex,-p.width/2,-p.height/2,p.width,p.height,0);glPopMatrix();glDepthMask(GL_TRUE);
}
void scene_init(Server *s){
 char path[4200];snprintf(path,sizeof(path),"%s/assets/garage.webp",s->root);s->garage_tex=ui_load_webp(path);
 glEnable(GL_DEPTH_TEST);glDepthFunc(GL_LEQUAL);glEnable(GL_MULTISAMPLE);glShadeModel(GL_SMOOTH);glClearColor(.013,.015,.031,1);s->pitch=s->target_pitch=-6;s->eye=vehicle_seat_eye(s->camera_seat);
 Plane panel={.center={.78,.33,.48-VEHICLE_COCKPIT_FORWARD},.width=1.56,.height=.9,.yaw=-5,.pitch=-28};Vec3 right=rotate_y(v3(1,0,0),panel.yaw),up=rotate_y(rotate_x(v3(0,1,0),panel.pitch),panel.yaw),normal=vcross(right,up);
 const char *names[]={"TERMINAL","FILES","AUDIO","METERS","NEON","COCKPIT"};const char *subs[]={"01 / SHELL","02 / EXPLORE","03 / SESSION","04 / EV","05 / COLOR","06 / HOME"};
 /* Built-in menu defaults; [app.*] config (applied in main.c after ui_init)
  * may relabel/re-icon/disable any of these live. */
 for(int i=0;i<6;i++){float x=(i%3-1)*.48f,y=i<3?.095f:-.22f;s->buttons[i].plane=(Plane){.center=vadd(panel.center,vadd(vadd(vmul(right,x),vmul(up,y)),vmul(normal,.084f))),.width=.426,.height=.248,.yaw=panel.yaw,.pitch=panel.pitch};snprintf(s->buttons[i].name,sizeof(s->buttons[i].name),"%s",names[i]);snprintf(s->buttons[i].sub,sizeof(s->buttons[i].sub),"%s",subs[i]);s->buttons[i].action=i==4?CD_BUTTON_NEON:i==5?CD_BUTTON_COCKPIT:CD_BUTTON_APP;s->buttons[i].icon=i;s->buttons[i].enabled=true;s->buttons[i].texture=0;}
 /* Settings cog medallion on the sloped dash between wheel and console.
  * The dash profile segment (z .2,y .14)->(z .88,y -.24) has geometric
  * normal (0,.873,.487); pitch -61 lays the gear flush on it, and the
  * center sits .02 along that normal from the slope point (-.30,-.084,.60)
  * so the boss reads as recessed into the padding. (The violet strip box
  * at z=.37 is behind this sloped surface from the driver's seat.) */
 s->cog=(Plane){.center={-.30f,-.066f,.610f-VEHICLE_COCKPIT_FORWARD},.width=.24f,.height=.24f,.yaw=0,.pitch=-61};
}
typedef struct {Vec3 eye,forward,right,up;} SceneCamera;
static SceneCamera scene_camera(Server *s){
 SceneCamera c;
 if(s->camera_mode==VEHICLE_CAMERA_DRIVER){
  c.eye=vehicle_local_to_world(&s->vehicle,s->eye);
  Vec3 forward=rotate_y(rotate_x(v3(0,0,-1),s->pitch),s->yaw);
  Vec3 right=rotate_y(v3(1,0,0),s->yaw),up=vcross(right,forward);
  c.forward=vehicle_local_direction(&s->vehicle,forward);
  c.right=vehicle_local_direction(&s->vehicle,right);
  c.up=vehicle_local_direction(&s->vehicle,up);
 }else{
  /* Exterior cameras are expressed in car-local coordinates too, so they
   * follow the same rigid transform as the cockpit instead of drifting when
   * the vehicle moves. EXTERIOR uses a low front-quarter showcase angle;
   * SYSTEMS uses a side view to expose the EV skateboard. Middle-drag yaw/pitch
   * orbits smoothly around the car in both exterior inspection modes. */
  Vec3 base_eye=s->camera_mode==VEHICLE_CAMERA_CHASE?v3(8.2f,2.3f,-10.4f):v3(9.6f,2.9f,1.2f);
  Vec3 local_eye=rotate_y(base_eye,s->yaw);
  local_eye.y=clampf(base_eye.y+(s->pitch+6.0f)*0.12f,0.4f,6.5f);
  Vec3 local_target=s->camera_mode==VEHICLE_CAMERA_CHASE?v3(0,-.08f,-.20f):v3(0,-.15f,0);
  c.eye=vehicle_local_to_world(&s->vehicle,local_eye);
  Vec3 target=vehicle_local_to_world(&s->vehicle,local_target);
  c.forward=vnorm(vsub(target,c.eye));c.right=vnorm(vcross(c.forward,v3(0,1,0)));c.up=vcross(c.right,c.forward);
 }
 return c;
}
static Vec3 scene_camera_ray(Server *s,SceneCamera c,float x,float y){
 float t=tanf(s->fov*RAD*.5f);
 Vec3 ray=vadd(c.forward,vadd(vmul(c.right,(2*x/s->width-1)*((float)s->width/s->height)*t),vmul(c.up,(1-2*y/s->height)*t)));
 return vnorm(ray);
}
static void scene_camera_view(SceneCamera c){
 /* gluLookAt equivalent, kept local to avoid a GLU dependency. The matrix is
  * column-major: right/up/-forward are the camera's world-space basis rows. */
 GLfloat m[16]={c.right.x,c.up.x,-c.forward.x,0,c.right.y,c.up.y,-c.forward.y,0,c.right.z,c.up.z,-c.forward.z,0,0,0,0,1};
 glMultMatrixf(m);glTranslatef(-c.eye.x,-c.eye.y,-c.eye.z);
}
static void vehicle_model_transform(Server *s){
 Vehicle *v=&s->vehicle;glTranslatef(v->position.x,v->position.y,v->position.z);
 glRotatef(v->yaw,0,1,0);glRotatef(v->pitch,1,0,0);glRotatef(v->roll,0,0,1);
}
void scene_update_mounts(Server *s){
 for(int i=0;i<CD_MAX_MONITORS;i++){
  Monitor *m=&s->monitors[i];if(!m->surface)continue;
  if(m->reveal<m->target_reveal){float duration=monitor_mount(i)==MOUNT_ROOF_FLIP?.90f:.72f;m->reveal=fminf(m->target_reveal,m->reveal+(float)s->dt/duration);}
  /* This exact plane is also consumed by scene_pick below: a client never
   * renders at one position while its pointer coordinates use another.
   * Arrange/frame transforms are session overrides; mount animation must not
   * stomp them on the next frame. Reset or remapping clears the override. */
  if(!m->custom_transform){
   Plane p=monitor_plane_at(i,m->reveal);
   if(i==3||i==4){
    int side=i==3?-1:1;
    p.center=vehicle_door_point(&s->vehicle,side,p.center);
    p.yaw+=vehicle_door_angle(&s->vehicle,side);
   }
   m->plane=p;
  }
 }
}
/* ---------------------------------------------------- render-to-texture */
/* The offscreen pass exists so display.render_scale can render below or
 * above the output size with independent magnification/minification
 * sampling. It only runs when enabled and the effective scaled size differs
 * from the output (so scale 1 keeps the historical direct path and its MSAA
 * in windowed mode). It is intentionally not multisampled, and it falls back
 * to direct-to-output whenever allocation/FBO setup fails. */
static bool rtt_mipmapped(GLuint f){
 return f==GL_NEAREST_MIPMAP_NEAREST||f==GL_LINEAR_MIPMAP_NEAREST||
        f==GL_NEAREST_MIPMAP_LINEAR||f==GL_LINEAR_MIPMAP_LINEAR;
}
static const char *rtt_filter_name(GLuint f){
 switch(f){
 case GL_NEAREST:return "nearest";
 case GL_LINEAR:return "linear";
 case GL_NEAREST_MIPMAP_NEAREST:return "nearest_mipmap_nearest";
 case GL_LINEAR_MIPMAP_NEAREST:return "linear_mipmap_nearest";
 case GL_NEAREST_MIPMAP_LINEAR:return "nearest_mipmap_linear";
 case GL_LINEAR_MIPMAP_LINEAR:return "linear_mipmap_linear";
 default:return "invalid";
 }
}
static int rtt_max_dim(void){
 GLint tex=0,rb=0;glGetIntegerv(GL_MAX_TEXTURE_SIZE,&tex);glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE,&rb);
 int max=tex>0?tex:4096;
 if(rb>0&&rb<max)max=rb;
 return max;
}
static void rtt_release(Server *s){
 if(s->rtt_color_tex)glDeleteTextures(1,&s->rtt_color_tex);
 if(s->rtt_depth_rb)glDeleteRenderbuffers(1,&s->rtt_depth_rb);
 if(s->rtt_fbo)glDeleteFramebuffers(1,&s->rtt_fbo);
 s->rtt_color_tex=s->rtt_depth_rb=s->rtt_fbo=0;
 s->render_width=s->render_height=0;
}
bool scene_rtt_begin(Server *s){
 if(!s->render_to_texture){s->render_width=s->render_height=0;return false;}
 int max=rtt_max_dim();double fw=(double)s->width*s->render_scale,fh=(double)s->height*s->render_scale;
 double limit=fmax(1.0,(double)max),factor=1.0;
 if(fw>limit)factor=fmin(factor,limit/fw);
 if(fh>limit)factor=fmin(factor,limit/fh);
 int w=(int)floor(fw*factor+.5),h=(int)floor(fh*factor+.5);
 if(w<1)w=1;if(h<1)h=1;if(w>max)w=max;if(h>max)h=max;
 if(w==s->width&&h==s->height){s->render_width=s->render_height=0;return false;}
 GLuint mag=s->render_mag_filter,min=s->render_min_filter;
 if(w==s->rtt_width&&h==s->rtt_height&&mag==s->rtt_mag_filter&&min==s->rtt_min_filter){
  if(!s->rtt_fbo){s->render_width=s->render_height=0;return false;}
  s->render_width=w;s->render_height=h;
  glBindFramebuffer(GL_FRAMEBUFFER,s->rtt_fbo);glViewport(0,0,w,h);
  return true;
 }
 rtt_release(s);
 s->rtt_width=w;s->rtt_height=h;s->rtt_mag_filter=mag;s->rtt_min_filter=min;
 GLint prev_tex=0;glGetIntegerv(GL_TEXTURE_BINDING_2D,&prev_tex);
 while(glGetError()!=GL_NO_ERROR){}
 GLuint tex=0,rb=0,fbo=0;
 glGenTextures(1,&tex);glBindTexture(GL_TEXTURE_2D,tex);
 glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
 glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,(GLint)mag);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,(GLint)min);
 glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,w,h,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
 if(rtt_mipmapped(min))glGenerateMipmap(GL_TEXTURE_2D);
 glGenRenderbuffers(1,&rb);glBindRenderbuffer(GL_RENDERBUFFER,rb);glRenderbufferStorage(GL_RENDERBUFFER,GL_DEPTH24_STENCIL8,w,h);
 glGenFramebuffers(1,&fbo);glBindFramebuffer(GL_FRAMEBUFFER,fbo);
 glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,tex,0);
 glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_RENDERBUFFER,rb);
 glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_STENCIL_ATTACHMENT,GL_RENDERBUFFER,rb);
 GLenum status=glCheckFramebufferStatus(GL_FRAMEBUFFER),err=glGetError();
 glBindTexture(GL_TEXTURE_2D,(GLuint)prev_tex);
 if(!tex||!rb||!fbo||status!=GL_FRAMEBUFFER_COMPLETE||err!=GL_NO_ERROR){
  fprintf(stderr,"[rtt] allocation failed %dx%d (output %dx%d, scale %g, mag %s, min %s): status 0x%x error 0x%x\n",
          w,h,s->width,s->height,s->render_scale,rtt_filter_name(mag),rtt_filter_name(min),status,err);
  if(tex)glDeleteTextures(1,&tex);if(rb)glDeleteRenderbuffers(1,&rb);if(fbo)glDeleteFramebuffers(1,&fbo);
  s->render_width=s->render_height=0;
  glBindFramebuffer(GL_FRAMEBUFFER,s->target_fbo);glViewport(0,0,s->width,s->height);
  return false;
 }
 s->rtt_color_tex=tex;s->rtt_depth_rb=rb;s->rtt_fbo=fbo;
 s->render_width=w;s->render_height=h;
 fprintf(stderr,"[rtt] render target %dx%d (output %dx%d, scale %g, mag %s, min %s)\n",
         w,h,s->width,s->height,s->render_scale,rtt_filter_name(mag),rtt_filter_name(min));
 glBindFramebuffer(GL_FRAMEBUFFER,s->rtt_fbo);glViewport(0,0,w,h);
 return true;
}
void scene_rtt_end(Server *s){
 if(!s->rtt_fbo){glBindFramebuffer(GL_FRAMEBUFFER,s->target_fbo);glViewport(0,0,s->width,s->height);return;}
 GLint prev_tex=0;GLfloat color[4];
 GLboolean depth=glIsEnabled(GL_DEPTH_TEST),light=glIsEnabled(GL_LIGHTING),blend=glIsEnabled(GL_BLEND);
 GLboolean tex2d=glIsEnabled(GL_TEXTURE_2D),stencil=glIsEnabled(GL_STENCIL_TEST),fog=glIsEnabled(GL_FOG),cull=glIsEnabled(GL_CULL_FACE);
 glGetIntegerv(GL_TEXTURE_BINDING_2D,&prev_tex);glGetFloatv(GL_CURRENT_COLOR,color);
 if(rtt_mipmapped(s->rtt_min_filter)){glBindTexture(GL_TEXTURE_2D,s->rtt_color_tex);glGenerateMipmap(GL_TEXTURE_2D);}
 glBindFramebuffer(GL_FRAMEBUFFER,s->target_fbo);glViewport(0,0,s->width,s->height);
 glDisable(GL_DEPTH_TEST);glDisable(GL_LIGHTING);glDisable(GL_BLEND);glDisable(GL_STENCIL_TEST);glDisable(GL_FOG);glDisable(GL_CULL_FACE);
 glEnable(GL_TEXTURE_2D);glBindTexture(GL_TEXTURE_2D,s->rtt_color_tex);
 glMatrixMode(GL_PROJECTION);glPushMatrix();glLoadIdentity();glMatrixMode(GL_MODELVIEW);glPushMatrix();glLoadIdentity();
 glColor4f(1,1,1,1);
 glBegin(GL_QUADS);
 glTexCoord2f(0,0);glVertex2f(-1,-1);glTexCoord2f(1,0);glVertex2f(1,-1);
 glTexCoord2f(1,1);glVertex2f(1,1);glTexCoord2f(0,1);glVertex2f(-1,1);
 glEnd();
 glPopMatrix();glMatrixMode(GL_PROJECTION);glPopMatrix();glMatrixMode(GL_MODELVIEW);
 if(depth)glEnable(GL_DEPTH_TEST);else glDisable(GL_DEPTH_TEST);
 if(light)glEnable(GL_LIGHTING);else glDisable(GL_LIGHTING);
 if(blend)glEnable(GL_BLEND);else glDisable(GL_BLEND);
 if(tex2d)glEnable(GL_TEXTURE_2D);else glDisable(GL_TEXTURE_2D);
 if(stencil)glEnable(GL_STENCIL_TEST);else glDisable(GL_STENCIL_TEST);
 if(fog)glEnable(GL_FOG);else glDisable(GL_FOG);
 if(cull)glEnable(GL_CULL_FACE);else glDisable(GL_CULL_FACE);
 glBindTexture(GL_TEXTURE_2D,(GLuint)prev_tex);glColor4f(color[0],color[1],color[2],color[3]);
 glBindFramebuffer(GL_FRAMEBUFFER,s->target_fbo);
}
void scene_render(Server *s){
 int rw=s->render_width>0?s->render_width:s->width,rh=s->render_height>0?s->render_height:s->height;
 glViewport(0,0,rw,rh);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT|GL_STENCIL_BUFFER_BIT);glEnable(GL_DEPTH_TEST);
 glMatrixMode(GL_PROJECTION);glLoadIdentity();double t=tan(s->fov*RAD/2)*.05;glFrustum(-t*(double)rw/rh,t*(double)rw/rh,-t,t,.05,150);
 glMatrixMode(GL_MODELVIEW);glLoadIdentity();SceneCamera camera=scene_camera(s);scene_camera_view(camera);
 lighting();environment(s);
 glPushMatrix();vehicle_model_transform(s);lighting();
 bool driver=s->camera_mode==VEHICLE_CAMERA_DRIVER;
 /* One physical cockpit in all views. Drawing it before the hull lets opaque
  * bodywork occlude it naturally; cutaway and the late shared glass pass then
  * reveal exactly the same seats, panels, controls and Wayland surfaces. */
 vehicle_seats_render();dashboard(s);draw_buttons(s);draw_cog(s);
 for(int i=0;i<CD_MAX_MONITORS;i++)draw_monitor(s,i);
 steering_wheel(s);
 vehicle_render(s,s->camera_mode==VEHICLE_CAMERA_SYSTEMS);
 if(driver)draw_windshield_hud(s);
 vehicle_render_glass(s);glPopMatrix();
}
Hit scene_pick_surface(Server *s,Surface *surface,float x,float y){
 Hit best={.kind=HIT_NONE,.distance=1e9f};
 if(!surface||s->camera_mode!=VEHICLE_CAMERA_DRIVER)return best;
 Surface *root=surface_root(surface);
 if(!root||root->slot<0||root->slot>=CD_MAX_MONITORS||root->width<=0||root->height<=0)return best;
 SceneCamera camera=scene_camera(s);Vec3 ray=vehicle_world_direction(&s->vehicle,scene_camera_ray(s,camera,x,y));
 Plane p=s->monitors[root->slot].plane;float u,v,d;
 if(!plane_hit(p,s->eye,ray,&u,&v,&d,false))return best;
 float scale=fminf(p.width/root->width,p.height/root->height);
 if(scale<=0)return best;
 float sx=(u-.5f)*p.width/scale+root->width/2.f,sy=(v-.5f)*p.height/scale+root->height/2.f;
 Surface *child=surface;
 while(child&&child!=root){sx-=child->x;sy-=child->y;child=child->parent;}
 return (Hit){.kind=HIT_APP,.surface=surface,.slot=root->slot,.x=sx,.y=sy,.distance=d};
}
Hit scene_pick(Server *s,float x,float y,bool grab){
 Hit best={.kind=HIT_NONE,.distance=1e9f};
 /* Exterior inspection views deliberately have no invisible cockpit hit
  * targets. Driver picking transforms the world ray back into car-local space,
  * allowing all existing Plane/UV mathematics to remain unchanged and exact. */
 if(s->camera_mode!=VEHICLE_CAMERA_DRIVER)return best;
 SceneCamera camera=scene_camera(s);Vec3 ray=vehicle_world_direction(&s->vehicle,scene_camera_ray(s,camera,x,y));float u,v,d;
 if(grab&&s->pointer_grab)return scene_pick_surface(s,s->pointer_grab,x,y);
 for(int i=0;i<CD_MAX_MONITORS;i++){Monitor *m=&s->monitors[i];if(!m->surface)continue;Plane p=m->plane;if(!plane_hit(p,s->eye,ray,&u,&v,&d,false)||d>=best.distance)continue;
  float lx=(u-.5f)*p.width,ly=(.5f-v)*p.height,k=monitor_iso_scale(p,i);
  if(i!=CD_CLUSTER_SLOT&&handles_button_at(p,k,lx,ly)){best=(Hit){.kind=HIT_HANDLES_BUTTON,.surface=m->surface,.slot=i,.x=lx,.y=ly,.distance=d};continue;}
  if(i!=CD_CLUSTER_SLOT&&s->arrange_mode&&s->arrange_handles&&s->arrange_slot==i){
   float zlx=0,zly=0,zd=0;bool z_hit=arrange_z_handle_hit(p,k,s->eye,ray,&zlx,&zly,&zd);
   int handle=arrange_handle_at(p,k,lx,ly,z_hit,zlx,zly);
   if(handle!=CD_HANDLE_NONE){best=(Hit){.kind=HIT_ARRANGE_HANDLE,.surface=m->surface,.slot=i,.handle=handle,.x=lx,.y=ly,.distance=handle==CD_HANDLE_AXIS_Z?zd:d};continue;}
  }
  float close_x,close_y,close_w,close_h;plane_close_rect(p,&close_x,&close_y,&close_w,&close_h);
  if(i!=CD_CLUSTER_SLOT&&fabsf(lx-close_x)<=close_w*.5f&&fabsf(ly-close_y)<=close_h*.5f){best=(Hit){.kind=HIT_CLOSE,.surface=m->surface,.slot=i,.distance=d};continue;}
  if(u<0||u>1||v<0||v>1){
   bool frame=i!=CD_CLUSTER_SLOT&&lx>=-(p.width*.5f+.07f*k)&&lx<=p.width*.5f+.07f*k&&ly>=-(p.height*.5f+.06f*k)&&ly<=p.height*.5f+.21f*k;
   if(frame)best=(Hit){.kind=HIT_FRAME,.surface=m->surface,.slot=i,.x=lx,.y=ly,.distance=d};
   continue;
  }
  float scale=fminf(p.width/m->surface->width,p.height/m->surface->height);float sx=(u-.5f)*p.width/scale+m->surface->width/2.f,sy=(v-.5f)*p.height/scale+m->surface->height/2.f,px,py;
  Surface *surface=surface_at(m->surface,sx,sy,&px,&py);if(surface)best=(Hit){.kind=HIT_APP,.surface=surface,.slot=i,.x=px,.y=py,.distance=d};
 }
 for(int i=0;i<6;i++)if(plane_hit(s->buttons[i].plane,s->eye,ray,&u,&v,&d,true)&&d<best.distance)best=(Hit){.kind=HIT_BUTTON,.button=i,.distance=d};
 /* Cog: same pick plane as drawn, then a radial test so only the gear
  * itself (teeth at .115 m of the .12 m half-plane) is clickable. */
 if(plane_hit(s->cog,s->eye,ray,&u,&v,&d,false)&&d<best.distance){float du=u-.5f,dv=v-.5f;if(du*du+dv*dv<=.25f)best=(Hit){.kind=HIT_COG,.distance=d};}
 Plane wheel=steering_plane();
 if(plane_hit(wheel,s->eye,ray,&u,&v,&d,false)&&d<best.distance){
  float dx=(u-.5f)*wheel.width,dy=(v-.5f)*wheel.height;
  if(dx*dx+dy*dy<=.50f*.50f)best=(Hit){.kind=HIT_WHEEL,.x=u,.y=v,.distance=d};
 }
 return best;
}
void scene_monitor_reset(Server *s,int slot){
 if(slot<0||slot>=CD_MAX_MONITORS)return;
 Monitor *m=&s->monitors[slot];m->custom_transform=false;m->reveal=m->target_reveal=1;m->plane=monitor_plane(slot);
}
void scene_monitor_translate(Server *s,int slot,float right,float up,float toward_eye){
 if(slot<0||slot>=CD_MAX_MONITORS||slot==CD_CLUSTER_SLOT)return;
 s->monitors[slot].custom_transform=true;s->monitors[slot].plane=plane_translate(s->monitors[slot].plane,s->eye,right,up,toward_eye);
}
void scene_monitor_rotate(Server *s,int slot,float yaw,float pitch,float roll){
 if(slot<0||slot>=CD_MAX_MONITORS||slot==CD_CLUSTER_SLOT)return;
 s->monitors[slot].custom_transform=true;s->monitors[slot].plane=plane_rotate(s->monitors[slot].plane,yaw,pitch,roll);
}
void scene_monitor_scale(Server *s,int slot,float factor){
 if(slot<0||slot>=CD_MAX_MONITORS||slot==CD_CLUSTER_SLOT)return;
 s->monitors[slot].custom_transform=true;s->monitors[slot].plane=plane_uniform_scale(s->monitors[slot].plane,monitor_plane(slot),factor);
}
void scene_monitor_set_plane(Server *s,int slot,Plane plane){
 if(slot<0||slot>=CD_MAX_MONITORS||slot==CD_CLUSTER_SLOT)return;
 s->monitors[slot].custom_transform=true;s->monitors[slot].plane=plane;
}
void scene_wheel_cancel(Server *s){s->wheel_drag=false;s->wheel_steer=0;s->wheel_axis_accum=0;}
bool scene_wheel_button(Server *s,Hit hit,int x,int y,bool down,uint32_t time){
 if(down){
  if(s->wheel_drag||hit.kind!=HIT_WHEEL)return s->wheel_drag;
  s->wheel_drag=true;s->wheel_last_x=x;s->wheel_last_y=y;s->wheel_axis_accum=0;s->wheel_steer=0;
  /* A wheel grab is compositor-owned. Send the matching pointer leave before
   * intercepting motion; otherwise the previously hovered client would see a
   * pointer that appears frozen inside its surface. */
  server_pointer_motion(s,(Hit){0},time);
  if(s->steering_mode==VEHICLE_STEER_CLIENT&&s->keyboard_focus){
   /* wl_pointer.axis is legal only on a surface with pointer focus. Move that
    * focus to the active app's centre before emitting wheel/scroll events;
    * keyboard focus alone is not sufficient in the Wayland protocol. */
   Surface *p=s->keyboard_focus;
   Hit center={.kind=HIT_APP,.surface=p,.slot=p->slot,.x=p->width*.5f,.y=p->height*.5f,.distance=0};
   server_pointer_motion(s,center,time);
  }
  return true;
 }
 if(s->wheel_drag){scene_wheel_cancel(s);return true;}
 return false;
}
void scene_wheel_motion(Server *s,int x,int y,uint32_t time){
 if(!s->wheel_drag)return;
 float dx=x-s->wheel_last_x;s->wheel_last_x=x;s->wheel_last_y=y;
 if(s->steering_mode==VEHICLE_STEER_DRIVE){
  s->wheel_steer=clampf(s->wheel_steer+dx*.006f,-1,1);
 }else{
  s->wheel_client_angle=clampf(s->wheel_client_angle-dx*.65f,-135,135);
  /* Coalesce high-frequency pointer samples into detents. This preserves the
   * familiar wheel-click semantics for toolkits while still accepting smooth
   * trackpad-style axis values through server_pointer_axis(). */
  s->wheel_axis_accum+=dx;
  if(fabsf(s->wheel_axis_accum)>=28){
   float detents=truncf(s->wheel_axis_accum/28);s->wheel_axis_accum-=detents*28;
   server_pointer_axis(s,-detents,0,time);
  }
 }
}
void scene_focus_monitor(Server *s,int slot,bool zoom){
 s->camera_mode=VEHICLE_CAMERA_DRIVER;
 if(slot<0||slot>=CD_MAX_MONITORS)return;
 /* Focus the actual animated/arranged plane, not the bay's deployed default. */
 Plane p=s->monitors[slot].plane;Vec3 rel=vsub(p.center,s->eye);
 float horizontal=sqrtf(rel.x*rel.x+rel.z*rel.z),dist=sqrtf(vdot(rel,rel));
 float desired=atan2f(-rel.x,-rel.z)/RAD;
 while(desired-s->yaw>180)desired-=360;while(desired-s->yaw<-180)desired+=360;
 s->target_yaw=desired;s->target_pitch=atan2f(rel.y,fmaxf(horizontal,.05f))/RAD;
 float fit_fov=fmaxf(2.f*atanf((p.height*.85f)/fmaxf(dist,.1f)),2.f*atanf((p.width*.85f)/(fmaxf(dist,.1f)*fmaxf((float)s->width/(float)s->height,.1f))))/RAD;
 s->target_fov=zoom?clampf(fit_fov,CD_DRIVER_FOV_MIN,55.f):65;s->focus_view=zoom;
}
void scene_reset_camera(Server *s){s->camera_mode=VEHICLE_CAMERA_DRIVER;s->eye=vehicle_seat_eye(s->camera_seat);s->target_yaw=roundf(s->yaw/360)*360;s->target_pitch=-6;s->target_fov=65;s->focus_view=false;}
void scene_save_screenshot(Server *s,const char *path){
 int w=s->width,h=s->height;uint8_t *pixels=malloc((size_t)w*h*4);if(!pixels)return;glBindFramebuffer(GL_FRAMEBUFFER,s->target_fbo);glReadPixels(0,0,w,h,GL_BGRA,GL_UNSIGNED_BYTE,pixels);
 uint8_t *flipped=malloc((size_t)w*h*4);
 if(!flipped){free(pixels);return;}
 for(int y=0;y<h;y++){memcpy(flipped+(size_t)y*w*4,pixels+(size_t)(h-1-y)*w*4,(size_t)w*4);for(int x=0;x<w;x++)flipped[(size_t)y*w*4+x*4+3]=255;}
 uint8_t *webp=NULL;size_t webp_size=WebPEncodeBGRA(flipped,w,h,w*4,90.0f,&webp);
 bool ok=false;
 if(webp_size){FILE *f=fopen(path,"wb");if(f){ok=fwrite(webp,1,webp_size,f)==webp_size;fclose(f);}}
 fprintf(stderr,"[capture] %s: %s\n",path,ok?"success":"failed");
 if(webp)WebPFree(webp);
 free(flipped);free(pixels);
}
void scene_finish(Server *s){
 if(s->city_list)glDeleteLists(s->city_list,1);
 for(int i=0;i<4;i++)if(s->city_signs[i])glDeleteTextures(1,&s->city_signs[i]);
 GLuint textures[]={s->garage_tex,s->carbon_tex,s->console_tex,s->hub_tex,s->hud_tex,s->windshield_tex,s->help_tex,s->plate_tex,s->handles_tex};glDeleteTextures(sizeof(textures)/sizeof(textures[0]),textures);for(int i=0;i<6;i++)glDeleteTextures(1,&s->buttons[i].texture);for(int i=0;i<CD_MAX_MONITORS;i++)glDeleteTextures(1,&s->monitors[i].label);
 rtt_release(s);
}
