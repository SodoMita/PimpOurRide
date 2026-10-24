#include "chrome.h"
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
static void plane_begin(Plane p){glPushMatrix();glTranslatef(p.center.x,p.center.y,p.center.z);glRotatef(p.yaw,0,1,0);glRotatef(p.pitch,1,0,0);}
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
 /* Night sky: one far quad with a vertical vertex-colour gradient. */
 glDisable(GL_LIGHTING);
 glBegin(GL_QUADS);glNormal3f(0,0,1);
 glColor3f(.10f,.09f,.24f);glVertex3f(-38,-2,-58);glVertex3f(38,-2,-58);
 glColor3f(.012f,.025f,.085f);glVertex3f(38,24,-58);glVertex3f(-38,24,-58);glEnd();
 /* Skyline: deterministic dark towers (same layout every frame - hash on
  * the building index, never rand(), so nothing shimmers or re-rolls). */
 float bx[26],bw[26],bh[26];
 float x=-25.f;
 for(int i=0;i<26;i++){bw[i]=1.3f+garage_hash(i*13+5)*1.5f;bh[i]=2.5f+garage_hash(i*7+2)*7.5f;bx[i]=x+bw[i]*.5f;x+=bw[i]+.35f;}
 glEnable(GL_LIGHTING);
 for(int i=0;i<26;i++){float shade=.7f+garage_hash(i*3)*.5f;color(.018f*shade,.028f*shade,.065f*shade);box(bx[i],-1.5f+bh[i]*.5f,-52,bw[i],bh[i],1.2f);}
 /* Lit windows as points on the tower faces; a few twinkle by re-hashing
  * against a slow time bucket. Points, not quads: ~800 windows/frame. */
 glDisable(GL_LIGHTING);glPointSize(2.f);glBegin(GL_POINTS);
 int bucket=(int)(s->time*.5);
 for(int i=0;i<26;i++)
  for(float wy=-1.1f;wy<-1.5f+bh[i]-.3f;wy+=.45f)
   for(float wx=bx[i]-bw[i]*.5f+.25f;wx<bx[i]+bw[i]*.5f-.2f;wx+=.35f){
    int cell=i*1000+(int)((wy+2)*10)*37+(int)((wx+30)*10);
    float h=garage_hash(cell);
    if(h>.32f)continue;
    float twinkle=(h>.28f&&garage_hash(cell+bucket*91)<.5f)?.25f:1.f;
    if(garage_hash(cell+77)<.6f)glColor3f(.95f*twinkle,.68f*twinkle,.34f*twinkle);
    else glColor3f(.55f*twinkle,.78f*twinkle,1.f*twinkle);
    glVertex3f(wx,wy,-51.35f);
   }
 glEnd();
 /* Rain streaks between the doorway and the towers, falling in +time. */
 glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE);glDepthMask(GL_FALSE);
 glColor4f(.55f,.7f,1.f,.30f);glBegin(GL_LINES);
 for(int i=0;i<130;i++){
  float rx=-9.f+18.f*garage_hash(i*3+1),rz=-42.f-8.f*garage_hash(i*7+4);
  float speed=4.f+3.f*garage_hash(i*5+9);
  float ry=garage_hash(i*11+6)*10.f-fmodf((float)s->time*speed,10.f);if(ry<0)ry+=10.f;ry-=1.5f;
  glVertex3f(rx,ry,rz);glVertex3f(rx-.07f,ry+.5f,rz);
 }
 glEnd();glDepthMask(GL_TRUE);glDisable(GL_BLEND);glEnable(GL_LIGHTING);
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
 /* Deep padded dashboard: bevelled profile, rather than a flat HUD backdrop. */
 color(.115,.075,.175);glEnable(GL_TEXTURE_2D);glBindTexture(GL_TEXTURE_2D,s->carbon_tex);
 glBegin(GL_QUAD_STRIP);float profile[][2]={{-1.0f,.18f},{-.72f,.34f},{-.36f,.31f},{.2f,.14f},{.88f,-.24f},{1.5f,-.55f},{1.7f,-1.2f}};
 for(int i=0;i<7;i++){float ny=.9f,nz=.4f;if(i>3){ny=.55;nz=.8;}glNormal3f(0,ny,nz);glTexCoord2f(0,i*.38f);glVertex3f(-3.05,profile[i][1],profile[i][0]);glTexCoord2f(5,i*.38f);glVertex3f(3.05,profile[i][1],profile[i][0]);}glEnd();glDisable(GL_TEXTURE_2D);
 /* Rich violet edge insert and chrome piping. */
 color(.28,.058,.44);box(0,-.11,.37,5.7,.19,.21);
 color(.43,.48,.57);cylinder(v3(-2.95,.18,-.49),v3(2.95,.18,-.49),.018,12);
 glow_line(v3(-2.92,.195,-.465),v3(2.92,.195,-.465),.008,nr,ng,nb);
 glow_line(v3(-2.88,-.23,.56),v3(2.88,-.23,.56),.009,nr,ng,nb);
 /* Purple hand-stitched seams, following the hood. */
 glDisable(GL_LIGHTING);color(.56,.31,.69);glLineWidth(1.3f);glBegin(GL_LINES);for(float x=-2.9;x<2.9;x+=.07f){glVertex3f(x,.317,-.22);glVertex3f(x+.035,.317,-.22);}glEnd();glEnable(GL_LIGHTING);
 /* Air vents in a machined surround. */
 for(int side=-1;side<=1;side+=2){glPushMatrix();glTranslatef(side*2.15,.19,-.46);glRotatef(-22,1,0,0);color(.24,.27,.33);rbox(0,0,0,.62,.23,.05,.045);color(.022,.027,.037);rbox(0,0,.031,.55,.16,.014,.023);for(int i=-3;i<=3;i++){color(.19,.22,.28);box(i*.068,0,.055,.012,.14,.025);}glPopMatrix();}
 /* Instrument binnacle. */
 Plane gauge={.center={-.94,.43,.28},.width=1.46,.height=.59,.pitch=-12};plane_begin(gauge);color(.025,.03,.043);rbox(0,0,-.05,1.62,.76,.14,.18);color(.32,.34,.44);rbox(0,0,.005,1.51,.63,.035,.10);texture(s->gauge_tex,-.73,-.295,1.46,.59,.03);glPopMatrix();
 /* Passenger-side brushed plate, cut into the dash. */
 Plane plate={.center={2.04,-.015,.13},.width=.82,.height=.30,.yaw=-8,.pitch=-24};plane_begin(plate);color(.26,.29,.38);rbox(0,0,0,.91,.37,.028,.025);texture(s->plate_tex,-.41,-.15,.82,.30,.02);glPopMatrix();
 /* Sloped touch-and-switch center console; each key has its own pick plane. */
 Plane console={.center={.78,.33,.48},.width=1.56,.height=.90,.yaw=-5,.pitch=-28};plane_begin(console);
 color(.37,.39,.49);rbox(0,0,-.028,1.64,.99,.12,.09);color(.035,.040,.059);rbox(0,0,.033,1.57,.92,.027,.065);texture(s->console_tex,-.78,-.45,1.56,.9,.05);
 glow_line(v3(-.70,-.425,.063),v3(.70,-.425,.063),.004,nr,ng,nb);glPopMatrix();
 /* A-pillars and the upper windshield frame establish a seated camera. */
 for(int side=-1;side<=1;side+=2){color(.11,.068,.17);cylinder(v3(side*3.55,.19,-1.0),v3(side*3.38,2.4,-.32),.085,18);color(.36,.36,.46);cylinder(v3(side*3.49,.23,-.96),v3(side*3.32,2.38,-.28),.023,12);glow_line(v3(side*3.51,.23,-.92),v3(side*3.33,2.38,-.25),.008,nr,ng,nb);}
 color(.022,.022,.035);cylinder(v3(-3.4,2.39,-.35),v3(3.4,2.39,-.35),.13,18);
 /* Rear-view mirror, tiny garage reflection. */
 color(.2,.23,.3);cylinder(v3(.30,2.39,-.4),v3(.30,2.2,-.53),.022,12);
 glPushMatrix();glTranslatef(.30,2.15,-.52);glRotatef(-5,1,0,0);color(.40,.44,.52);rbox(0,0,0,.85,.26,.07,.06);color(.015,.028,.04);rbox(0,0,.043,.78,.20,.025,.04);texture(s->garage_tex,-.37,-.08,.74,.16,.06);glPopMatrix();
 /* Quilted doors and visible subwoofers. */
 for(int side=-1;side<=1;side+=2){glPushMatrix();glTranslatef(side*2.88,-.39,.64);glRotatef(side*-40,0,1,0);color(.13,.058,.21);rbox(0,-.15,0,.68,1.04,.15,.14);glDisable(GL_LIGHTING);color(.36,.19,.46);glBegin(GL_LINES);for(float x=-.8;x<.8;x+=.12){glVertex3f(-.30,x,.081);glVertex3f(.3,x+.45,.081);glVertex3f(-.30,x+.45,.082);glVertex3f(.30,x,.082);}glEnd();glEnable(GL_LIGHTING);
 glTranslatef(0,-.30,.15);color(.45,.48,.56);torus(.21,.024,48,10);color(.018,.023,.037);cylinder(v3(0,0,-.06),v3(0,0,.005),.19,48);color(.075,.085,.115);torus(.15,.024,48,10);color(.14,.15,.21);cylinder(v3(0,0,.008),v3(0,0,.045),.055,32);glPopMatrix();}
 /* Polished shifter on the transmission tunnel. */
 color(.045,.04,.072);rbox(.66,-.84,1.02,.70,.48,.29,.10);color(.28,.32,.40);cylinder(v3(.66,-.88,.96),v3(.66,-.49,.96),.025,20);glPushMatrix();glTranslatef(.66,-.47,.96);color(.65,.64,.74);torus(.054,.049,28,16);glPopMatrix();
}
static Plane steering_plane(void){return (Plane){.center={-.84f,.28f,1.10f},.width=1.12f,.height=1.12f,.pitch=-18};}
static void steering_wheel(Server *s){
 float nr,ng,nb;neon(s,&nr,&ng,&nb);glPushMatrix();glTranslatef(-.84,.28,1.10);glRotatef(-18,1,0,0);
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
typedef struct {float scale,w,h;int layer;} DrawSurface;
static void transformed_uv(int transform,float x,float y,float *u,float *v){float a=x,b=y;switch(transform&3){case 1:a=1-y;b=x;break;case 2:a=1-x;b=1-y;break;case 3:a=y;b=1-x;break;}if(transform>=4)a=1-a;*u=a;*v=b;}
static void draw_app_surface(Surface *p,int sx,int sy,void *data){
 DrawSurface *d=data;float x=-d->w/2+sx*d->scale,y=d->h/2-sy*d->scale,w=p->width*d->scale,h=p->height*d->scale,z=.024f+.0007f*d->layer++;
 glDisable(GL_LIGHTING);glEnable(GL_TEXTURE_2D);glBindTexture(GL_TEXTURE_2D,p->texture);glEnable(GL_BLEND);glBlendFunc(GL_ONE,GL_ONE_MINUS_SRC_ALPHA);glColor4f(1,1,1,1);
 float coords[4][2]={{0,0},{1,0},{1,1},{0,1}},vx[]={x,x+w,x+w,x},vy[]={y,y,y-h,y-h};glBegin(GL_QUADS);for(int i=0;i<4;i++){float u,v;transformed_uv(p->transform,coords[i][0],coords[i][1],&u,&v);glTexCoord2f(u,v);glVertex3f(vx[i],vy[i],z);}glEnd();glDisable(GL_BLEND);glDisable(GL_TEXTURE_2D);glEnable(GL_LIGHTING);
}
static void draw_monitor(Server *s,int i){
 Monitor *m=&s->monitors[i];if(!m->surface)return;Plane p=m->plane;float w=p.width,h=p.height;bool active=s->keyboard_focus==m->surface;
 plane_begin(p);
 color(.08,.095,.13);cylinder(v3(0,-h/2-.03,-.065),v3(0,-.90,-.065),.045,16);color(.16,.18,.23);box(0,-.88,-.085,.48,.06,.25);
 color(.34,.39,.46);rbox(0,.075,-.056,w+.12,h+.25,.10,.065);
 color(.018,.027,.038);rbox(0,.075,-.003,w+.086,h+.219,.038,.055);
 glDisable(GL_LIGHTING);color(.010,.016,.026);quad(-w/2,-h/2,w,h,.019);glEnable(GL_LIGHTING);
 float nr,ng,nb;neon(s,&nr,&ng,&nb);
 if(active)glow_line(v3(-w/2,-h/2-.022,.025),v3(w/2,-h/2-.022,.025),.004,.35,.92,.74);else glow_line(v3(-w/2,-h/2-.022,.025),v3(w/2,-h/2-.022,.025),.003,nr*.65,ng*.7,nb*.7);
 ui_monitor_label(s,i);texture(m->label,-w/2,h/2+.011,w,.137,.024);
 glEnable(GL_STENCIL_TEST);glClear(GL_STENCIL_BUFFER_BIT);glStencilMask(0xff);glStencilFunc(GL_ALWAYS,1,0xff);glStencilOp(GL_KEEP,GL_KEEP,GL_REPLACE);glColorMask(GL_FALSE,GL_FALSE,GL_FALSE,GL_FALSE);glDepthMask(GL_FALSE);quad(-w/2,-h/2,w,h,.021);glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);glDepthMask(GL_TRUE);glStencilFunc(GL_EQUAL,1,0xff);glStencilOp(GL_KEEP,GL_KEEP,GL_KEEP);glStencilMask(0);
 float scale=fminf(w/m->surface->width,h/m->surface->height);DrawSurface d={.scale=scale,.w=m->surface->width*scale,.h=m->surface->height*scale};server_each_surface(m->surface,draw_app_surface,&d);
 glStencilMask(0xff);glDisable(GL_STENCIL_TEST);glPopMatrix();
}
void scene_init(Server *s){
 char path[4200];snprintf(path,sizeof(path),"%s/assets/garage.webp",s->root);s->garage_tex=ui_load_webp(path);
 glEnable(GL_DEPTH_TEST);glDepthFunc(GL_LEQUAL);glEnable(GL_MULTISAMPLE);glShadeModel(GL_SMOOTH);glClearColor(.013,.015,.031,1);s->pitch=s->target_pitch=-6;
 Plane panel={.center={.78,.33,.48},.width=1.56,.height=.9,.yaw=-5,.pitch=-28};Vec3 right=rotate_y(v3(1,0,0),panel.yaw),up=rotate_y(rotate_x(v3(0,1,0),panel.pitch),panel.yaw),normal=vcross(right,up);
 const char *names[]={"TERMINAL","FILES","AUDIO","METERS","NEON","COCKPIT"};const char *subs[]={"01 / SHELL","02 / EXPLORE","03 / SESSION","04 / LIVE","05 / COLOR","06 / HOME"};
 /* Built-in menu defaults; [app.*] config (applied in main.c after ui_init)
  * may relabel/re-icon/disable any of these live. */
 for(int i=0;i<6;i++){float x=(i%3-1)*.48f,y=i<3?.095f:-.22f;s->buttons[i].plane=(Plane){.center=vadd(panel.center,vadd(vadd(vmul(right,x),vmul(up,y)),vmul(normal,.084f))),.width=.426,.height=.248,.yaw=panel.yaw,.pitch=panel.pitch};snprintf(s->buttons[i].name,sizeof(s->buttons[i].name),"%s",names[i]);snprintf(s->buttons[i].sub,sizeof(s->buttons[i].sub),"%s",subs[i]);s->buttons[i].action=i;s->buttons[i].icon=i;s->buttons[i].enabled=true;s->buttons[i].texture=0;}
 /* Settings cog medallion on the sloped dash between wheel and console.
  * The dash profile segment (z .2,y .14)->(z .88,y -.24) has geometric
  * normal (0,.873,.487); pitch -61 lays the gear flush on it, and the
  * center sits .02 along that normal from the slope point (-.30,-.084,.60)
  * so the boss reads as recessed into the padding. (The violet strip box
  * at z=.37 is behind this sloped surface from the driver's seat.) */
 s->cog=(Plane){.center={-.30f,-.066f,.610f},.width=.24f,.height=.24f,.yaw=0,.pitch=-61};
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
   * the vehicle moves. SYSTEMS uses a side view to expose the EV skateboard. */
  Vec3 local_eye=s->camera_mode==VEHICLE_CAMERA_CHASE?v3(8.8f,5.4f,11.8f):v3(9.8f,3.4f,1.2f);
  Vec3 local_target=v3(0,-.15f,0);
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
void scene_render(Server *s){
 glViewport(0,0,s->width,s->height);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT|GL_STENCIL_BUFFER_BIT);glEnable(GL_DEPTH_TEST);
 glMatrixMode(GL_PROJECTION);glLoadIdentity();double t=tan(s->fov*RAD/2)*.05;glFrustum(-t*s->width/s->height,t*s->width/s->height,-t,t,.05,150);
 glMatrixMode(GL_MODELVIEW);glLoadIdentity();SceneCamera camera=scene_camera(s);scene_camera_view(camera);
 lighting();environment(s);
 glPushMatrix();vehicle_model_transform(s);lighting();
 bool driver=s->camera_mode==VEHICLE_CAMERA_DRIVER;
 vehicle_render(s,s->camera_mode==VEHICLE_CAMERA_SYSTEMS);
 if(driver){dashboard(s);draw_buttons(s);draw_cog(s);for(int i=0;i<CD_MAX_MONITORS;i++)draw_monitor(s,i);steering_wheel(s);}
 vehicle_render_glass(s);glPopMatrix();
}
Hit scene_pick(Server *s,float x,float y,bool grab){
 Hit best={.kind=HIT_NONE,.distance=1e9f};
 /* Exterior inspection views deliberately have no invisible cockpit hit
  * targets. Driver picking transforms the world ray back into car-local space,
  * allowing all existing Plane/UV mathematics to remain unchanged and exact. */
 if(s->camera_mode!=VEHICLE_CAMERA_DRIVER)return best;
 SceneCamera camera=scene_camera(s);Vec3 ray=vehicle_world_direction(&s->vehicle,scene_camera_ray(s,camera,x,y));float u,v,d;
 if(grab&&s->pointer_grab){Surface *root=surface_root(s->pointer_grab);if(root&&root->slot>=0){Plane p=s->monitors[root->slot].plane;if(plane_hit(p,s->eye,ray,&u,&v,&d,false)){float scale=fminf(p.width/root->width,p.height/root->height);float sx=(u-.5f)*p.width/scale+root->width/2.f,sy=(v-.5f)*p.height/scale+root->height/2.f;Surface *child=s->pointer_grab;while(child!=root&&child){sx-=child->x;sy-=child->y;child=child->parent;}return (Hit){.kind=HIT_APP,.surface=s->pointer_grab,.slot=root->slot,.x=sx,.y=sy,.distance=d};}}return best;}
 for(int i=0;i<CD_MAX_MONITORS;i++){Monitor *m=&s->monitors[i];if(!m->surface)continue;Plane p=m->plane;if(!plane_hit(p,s->eye,ray,&u,&v,&d,false)||d>=best.distance)continue;
  if(u>.92&&u<1.01&&v<0&&v>-.12){best=(Hit){.kind=HIT_CLOSE,.surface=m->surface,.slot=i,.distance=d};continue;}
  if(u<0||u>1||v<0||v>1)continue;float scale=fminf(p.width/m->surface->width,p.height/m->surface->height);float sx=(u-.5f)*p.width/scale+m->surface->width/2.f,sy=(v-.5f)*p.height/scale+m->surface->height/2.f,px,py;
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
 if(slot<0||slot>=CD_MAX_MONITORS)return;Plane p=s->monitors[slot].plane;float desired=atan2f(-p.center.x,s->eye.z-p.center.z)/RAD;
 while(desired-s->yaw>180)desired-=360;while(desired-s->yaw<-180)desired+=360;s->target_yaw=desired;s->target_pitch=atan2f(p.center.y-s->eye.y,4.4f)/RAD;s->target_fov=zoom?32:65;s->focus_view=zoom;
}
void scene_reset_camera(Server *s){s->camera_mode=VEHICLE_CAMERA_DRIVER;s->target_yaw=roundf(s->yaw/360)*360;s->target_pitch=-6;s->target_fov=65;s->focus_view=false;}
void scene_save_screenshot(Server *s,const char *path){
 int w=s->width,h=s->height;uint8_t *pixels=malloc((size_t)w*h*4);if(!pixels)return;glReadPixels(0,0,w,h,GL_BGRA,GL_UNSIGNED_BYTE,pixels);
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
 GLuint textures[]={s->garage_tex,s->carbon_tex,s->gauge_tex,s->console_tex,s->hub_tex,s->hud_tex,s->help_tex,s->plate_tex};glDeleteTextures(sizeof(textures)/sizeof(textures[0]),textures);for(int i=0;i<6;i++)glDeleteTextures(1,&s->buttons[i].texture);for(int i=0;i<CD_MAX_MONITORS;i++)glDeleteTextures(1,&s->monitors[i].label);
}
