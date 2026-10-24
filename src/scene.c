#include "chrome.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
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
static void environment(Server *s){
 glDisable(GL_LIGHTING);glDisable(GL_DEPTH_TEST);texture(s->garage_tex,-45,-17.8,90,38.2,-35);glEnable(GL_DEPTH_TEST);
 color(.021,.019,.036);glBegin(GL_QUADS);glNormal3f(0,1,0);glVertex3f(-50,-1.5,-35);glVertex3f(50,-1.5,-35);glVertex3f(50,-1.5,35);glVertex3f(-50,-1.5,35);glEnd();
 glDisable(GL_LIGHTING);glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);glColor4f(.28,.28,.5,.2);glBegin(GL_LINES);for(int i=-25;i<=25;i+=3){glVertex3f(i,-1.495,-35);glVertex3f(i,-1.495,25);glVertex3f(-35,-1.495,i);glVertex3f(35,-1.495,i);}glEnd();glDisable(GL_BLEND);glEnable(GL_LIGHTING);
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
static void steering_wheel(Server *s){
 float nr,ng,nb;neon(s,&nr,&ng,&nb);glPushMatrix();glTranslatef(-.84,.28,1.10);glRotatef(-18,1,0,0);glScalef(.74,.74,.74);
 /* Thick violet leather rim, bright machined inner and outer rings. */
 color(.065,.033,.098);torus(.595,.078,96,16);color(.37,.35,.46);torus(.67,.009,96,10);color(.51,.54,.63);torus(.515,.012,96,10);color(.28,.067,.41);torus(.595,.063,96,12);
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
 char path[4200];snprintf(path,sizeof(path),"%s/assets/garage.png",s->root);s->garage_tex=ui_load_png(path);
 glEnable(GL_DEPTH_TEST);glDepthFunc(GL_LEQUAL);glEnable(GL_MULTISAMPLE);glShadeModel(GL_SMOOTH);glClearColor(.013,.015,.031,1);s->pitch=s->target_pitch=-6;
 Plane panel={.center={.78,.33,.48},.width=1.56,.height=.9,.yaw=-5,.pitch=-28};Vec3 right=rotate_y(v3(1,0,0),panel.yaw),up=rotate_y(rotate_x(v3(0,1,0),panel.pitch),panel.yaw),normal=vcross(right,up);
 const char *names[]={"TERMINAL","FILES","AUDIO","METERS","NEON","COCKPIT"};const char *subs[]={"01 / SHELL","02 / EXPLORE","03 / SESSION","04 / LIVE","05 / COLOR","06 / HOME"};
 for(int i=0;i<6;i++){float x=(i%3-1)*.48f,y=i<3?.095f:-.22f;s->buttons[i]=(DashButton){.plane={.center=vadd(panel.center,vadd(vadd(vmul(right,x),vmul(up,y)),vmul(normal,.084f))),.width=.426,.height=.248,.yaw=panel.yaw,.pitch=panel.pitch},.name=names[i],.sub=subs[i],.action=i};}
}
void scene_render(Server *s){
 glViewport(0,0,s->width,s->height);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT|GL_STENCIL_BUFFER_BIT);glEnable(GL_DEPTH_TEST);
 glMatrixMode(GL_PROJECTION);glLoadIdentity();double t=tan(s->fov*RAD/2)*.05;glFrustum(-t*s->width/s->height,t*s->width/s->height,-t,t,.05,150);
 glMatrixMode(GL_MODELVIEW);glLoadIdentity();glRotatef(-s->pitch,1,0,0);glRotatef(-s->yaw,0,1,0);glTranslatef(-s->eye.x,-s->eye.y,-s->eye.z);
 lighting();environment(s);lighting();dashboard(s);draw_buttons(s);for(int i=0;i<CD_MAX_MONITORS;i++)draw_monitor(s,i);steering_wheel(s);
}
Hit scene_pick(Server *s,float x,float y,bool grab){
 Hit best={.kind=HIT_NONE,.distance=1e9f};Vec3 ray=camera_ray(x,y,s->width,s->height,s->fov,s->yaw,s->pitch);float u,v,d;
 if(grab&&s->pointer_grab){Surface *root=surface_root(s->pointer_grab);if(root&&root->slot>=0){Plane p=s->monitors[root->slot].plane;if(plane_hit(p,s->eye,ray,&u,&v,&d,false)){float scale=fminf(p.width/root->width,p.height/root->height);float sx=(u-.5f)*p.width/scale+root->width/2.f,sy=(v-.5f)*p.height/scale+root->height/2.f;Surface *child=s->pointer_grab;while(child!=root&&child){sx-=child->x;sy-=child->y;child=child->parent;}return (Hit){.kind=HIT_APP,.surface=s->pointer_grab,.slot=root->slot,.x=sx,.y=sy,.distance=d};}}return best;}
 for(int i=0;i<CD_MAX_MONITORS;i++){Monitor *m=&s->monitors[i];if(!m->surface)continue;Plane p=m->plane;if(!plane_hit(p,s->eye,ray,&u,&v,&d,false)||d>=best.distance)continue;
  if(u>.92&&u<1.01&&v<0&&v>-.12){best=(Hit){.kind=HIT_CLOSE,.surface=m->surface,.slot=i,.distance=d};continue;}
  if(u<0||u>1||v<0||v>1)continue;float scale=fminf(p.width/m->surface->width,p.height/m->surface->height);float sx=(u-.5f)*p.width/scale+m->surface->width/2.f,sy=(v-.5f)*p.height/scale+m->surface->height/2.f,px,py;
  Surface *surface=surface_at(m->surface,sx,sy,&px,&py);if(surface)best=(Hit){.kind=HIT_APP,.surface=surface,.slot=i,.x=px,.y=py,.distance=d};
 }
 for(int i=0;i<6;i++)if(plane_hit(s->buttons[i].plane,s->eye,ray,&u,&v,&d,true)&&d<best.distance)best=(Hit){.kind=HIT_BUTTON,.button=i,.distance=d};return best;
}
void scene_focus_monitor(Server *s,int slot,bool zoom){
 if(slot<0||slot>=CD_MAX_MONITORS)return;Plane p=s->monitors[slot].plane;float desired=atan2f(-p.center.x,s->eye.z-p.center.z)/RAD;
 while(desired-s->yaw>180)desired-=360;while(desired-s->yaw<-180)desired+=360;s->target_yaw=desired;s->target_pitch=atan2f(p.center.y-s->eye.y,4.4f)/RAD;s->target_fov=zoom?32:65;s->focus_view=zoom;
}
void scene_reset_camera(Server *s){s->target_yaw=roundf(s->yaw/360)*360;s->target_pitch=-6;s->target_fov=65;s->focus_view=false;}
void scene_save_screenshot(Server *s,const char *path){
 int w=s->width,h=s->height;uint8_t *pixels=malloc((size_t)w*h*4);if(!pixels)return;glReadPixels(0,0,w,h,GL_BGRA,GL_UNSIGNED_BYTE,pixels);
 cairo_surface_t *im=cairo_image_surface_create(CAIRO_FORMAT_ARGB32,w,h);uint8_t *dst=cairo_image_surface_get_data(im);int stride=cairo_image_surface_get_stride(im);for(int y=0;y<h;y++){memcpy(dst+y*stride,pixels+(size_t)(h-1-y)*w*4,w*4);for(int x=0;x<w;x++)dst[y*stride+x*4+3]=255;}cairo_surface_mark_dirty(im);cairo_status_t status=cairo_surface_write_to_png(im,path);fprintf(stderr,"[capture] %s: %s\n",path,cairo_status_to_string(status));cairo_surface_destroy(im);free(pixels);
}
void scene_finish(Server *s){
 GLuint textures[]={s->garage_tex,s->carbon_tex,s->gauge_tex,s->console_tex,s->hub_tex,s->hud_tex,s->help_tex,s->plate_tex};glDeleteTextures(sizeof(textures)/sizeof(textures[0]),textures);for(int i=0;i<6;i++)glDeleteTextures(1,&s->buttons[i].texture);for(int i=0;i<CD_MAX_MONITORS;i++)glDeleteTextures(1,&s->monitors[i].label);
}
