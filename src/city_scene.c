#include "chrome.h"
#include "city.h"
#include "i18n.h"
#include <math.h>
#include <string.h>

#define CITY_PI 3.14159265358979323846f

static float city_hash(int n){float x=sinf((float)n*127.1f)*43758.5453f;return x-floorf(x);}
static void city_color(float r,float g,float b){glColor3f(r,g,b);}
static void city_box(float x,float y,float z,float w,float h,float d){
 float a=x-w/2,b=x+w/2,c=y-h/2,e=y+h/2,f=z-d/2,g=z+d/2;
 glBegin(GL_QUADS);
 glNormal3f(0,0,1);glVertex3f(a,c,g);glVertex3f(b,c,g);glVertex3f(b,e,g);glVertex3f(a,e,g);
 glNormal3f(0,0,-1);glVertex3f(b,c,f);glVertex3f(a,c,f);glVertex3f(a,e,f);glVertex3f(b,e,f);
 glNormal3f(0,1,0);glVertex3f(a,e,f);glVertex3f(b,e,f);glVertex3f(b,e,g);glVertex3f(a,e,g);
 glNormal3f(0,-1,0);glVertex3f(a,c,g);glVertex3f(b,c,g);glVertex3f(b,c,f);glVertex3f(a,c,f);
 glNormal3f(1,0,0);glVertex3f(b,c,g);glVertex3f(b,c,f);glVertex3f(b,e,f);glVertex3f(b,e,g);
 glNormal3f(-1,0,0);glVertex3f(a,c,f);glVertex3f(a,c,g);glVertex3f(a,e,g);glVertex3f(a,e,f);glEnd();
}
static void city_cylinder(Vec3 a,Vec3 b,float radius,int slices){
 Vec3 dir=vnorm(vsub(b,a)),u=vnorm(vcross(dir,fabsf(dir.y)>.9?v3(1,0,0):v3(0,1,0))),v=vcross(dir,u);
 glBegin(GL_QUAD_STRIP);for(int i=0;i<=slices;i++){float t=i*2*CITY_PI/slices;Vec3 n=vadd(vmul(u,cosf(t)),vmul(v,sinf(t))),pa=vadd(a,vmul(n,radius)),pb=vadd(b,vmul(n,radius));glNormal3f(n.x,n.y,n.z);glVertex3f(pa.x,pa.y,pa.z);glVertex3f(pb.x,pb.y,pb.z);}glEnd();
}
static void city_glow(Vec3 a,Vec3 b,float radius,float r,float g,float bl){
 glDisable(GL_LIGHTING);city_color(r,g,bl);city_cylinder(a,b,radius,8);glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE);glDepthMask(GL_FALSE);
 glColor4f(r,g,bl,.06f);city_cylinder(a,b,radius*3.2f,8);glDepthMask(GL_TRUE);glDisable(GL_BLEND);glEnable(GL_LIGHTING);
}
static void city_texture(GLuint tex,float x,float y,float w,float h,float z){
 glDisable(GL_LIGHTING);glEnable(GL_TEXTURE_2D);glBindTexture(GL_TEXTURE_2D,tex);glColor4f(1,1,1,1);glEnable(GL_BLEND);glBlendFunc(GL_ONE,GL_ONE_MINUS_SRC_ALPHA);
 /* Text must read correctly from either approach to a drive-through bay.
  * Separate front/back windings avoid showing a mirrored transparent back. */
 GLboolean was_culled=glIsEnabled(GL_CULL_FACE);glEnable(GL_CULL_FACE);glCullFace(GL_BACK);
 glBegin(GL_QUADS);glTexCoord2f(0,1);glVertex3f(x,y,z);glTexCoord2f(1,1);glVertex3f(x+w,y,z);glTexCoord2f(1,0);glVertex3f(x+w,y+h,z);glTexCoord2f(0,0);glVertex3f(x,y+h,z);glEnd();
 glBegin(GL_QUADS);glTexCoord2f(0,1);glVertex3f(x+w,y,z-.01f);glTexCoord2f(1,1);glVertex3f(x,y,z-.01f);glTexCoord2f(1,0);glVertex3f(x,y+h,z-.01f);glTexCoord2f(0,0);glVertex3f(x+w,y+h,z-.01f);glEnd();
 if(!was_culled)glDisable(GL_CULL_FACE);
 glDisable(GL_TEXTURE_2D);glDisable(GL_BLEND);glEnable(GL_LIGHTING);
}
static GLuint city_sign_texture(const char *name,int index){
 cairo_surface_t *im=cairo_image_surface_create(CAIRO_FORMAT_ARGB32,1024,192);cairo_t*c=cairo_create(im);
 cairo_set_source_rgb(c,.018,.027,.045);cairo_paint(c);
 const float accents[][3]={{.45,.85,1},{.25,.95,.72},{.72,.48,1},{1,.48,.32}};
 cairo_set_source_rgb(c,accents[index][0],accents[index][1],accents[index][2]);cairo_set_line_width(c,7);cairo_rectangle(c,7,7,1010,178);cairo_stroke(c);
 cairo_select_font_face(c,"DejaVu Sans",CAIRO_FONT_SLANT_NORMAL,CAIRO_FONT_WEIGHT_BOLD);cairo_set_font_size(c,58);
 cairo_text_extents_t ex;cairo_text_extents(c,name,&ex);cairo_move_to(c,(1024-ex.width)/2,92);cairo_show_text(c,name);
 cairo_set_font_size(c,25);cairo_set_source_rgb(c,.62,.72,.82);const char *sub=tr("AUTONOMOUS HOVER SERVICE  /  DRIVE THROUGH");cairo_text_extents(c,sub,&ex);cairo_move_to(c,(1024-ex.width)/2,145);cairo_show_text(c,sub);
 GLuint tex=ui_texture(im,0);cairo_destroy(c);cairo_surface_destroy(im);return tex;
}

static void city_sky(Vec3 at){
 glDisable(GL_LIGHTING);glDisable(GL_DEPTH_TEST);glDepthMask(GL_FALSE);
 /* An infinity backdrop follows the rig, but stays inside the 150 m far
  * plane. The old fixed 260 m dome was clipped away completely. */
 glBegin(GL_QUAD_STRIP);for(int i=0;i<=48;i++){
  float a=i*2*CITY_PI/48,x=at.x+cosf(a)*110,z=at.z+sinf(a)*110;
  glColor3f(.025,.035,.085);glVertex3f(x,-2,z);
  glColor3f(.11,.055,.20);glVertex3f(x,85,z);
 }glEnd();
 glPointSize(2);glBegin(GL_POINTS);for(int i=0;i<180;i++){
  float a=city_hash(i*7+2)*2*CITY_PI,r=105,y=15+city_hash(i*11+4)*57;
  float tw=.45+city_hash(i*13+8)*.55;glColor3f(.65*tw,.78*tw,1*tw);glVertex3f(at.x+cosf(a)*r,y,at.z+sinf(a)*r);
 }glEnd();
 glColor3f(.72,.79,1);glBegin(GL_TRIANGLE_FAN);glVertex3f(at.x-55,48,at.z-75);
 for(int i=0;i<=32;i++){float a=i*2*CITY_PI/32;glVertex3f(at.x-55+cosf(a)*5,48+sinf(a)*5,at.z-75);}glEnd();
 glDepthMask(GL_TRUE);glEnable(GL_DEPTH_TEST);glEnable(GL_LIGHTING);
}
static void city_vertical_road(float x,float near_z,float far_z){
 float mid=(near_z+far_z)*.5f,length=far_z-near_z;
 city_color(.11,.13,.16);city_box(x,-1.50,mid,24,.10,length);
 city_color(.035,.042,.055);city_box(x,-1.43,mid,16,.08,length);
 city_color(.52,.57,.62);city_box(x-6,-1.382,mid,.12,.018,length);city_box(x+6,-1.382,mid,.12,.018,length);
}
static void city_roads(void){
 city_color(.025,.032,.045);city_box(0,-1.62,-65,340,.18,340);
 const float xs[]={-65,0,65},zs[]={-70,-115};
 for(int i=0;i<3;i++){
  /* Keep the original hall's detailed floor. The central street stops at the
   * two real home-garage walls instead of painting asphalt through the room. */
  if(i==1){city_vertical_road(0,-225,-40);city_vertical_road(0,16,95);}
  else city_vertical_road(xs[i],-225,95);
  for(float z=-218;z<91;z+=10){
   if(i==1&&z>-44&&z<20)continue;
   city_color(.85,.68,.26);city_box(xs[i],-1.378,z,.18,.022,4.5f);
  }
 }
 /* Ambient through-traffic owns the outer ring. These connectors keep every
  * rendered traffic pose on an actual road and outside the service garages. */
 for(int side=-1;side<=1;side+=2){
  float x=side*150.0f;city_vertical_road(x,-124,-60);
  for(float z=-120;z<-60;z+=10){city_color(.85,.68,.26);city_box(x,-1.378,z,.18,.022,4.5f);}
 }
 for(int i=0;i<2;i++){
  city_color(.11,.13,.16);city_box(0,-1.49,zs[i],320,.10,24);
  city_color(.035,.042,.055);city_box(0,-1.42,zs[i],320,.08,16);
  city_color(.52,.57,.62);city_box(0,-1.372,zs[i]-6,320,.018,.12);city_box(0,-1.372,zs[i]+6,320,.018,.12);
  for(float x=-153;x<154;x+=10){city_color(.85,.68,.26);city_box(x,-1.368,zs[i],4.5f,.022,.18);}
 }
 /* Zebra crossings make the road intersections legible at street level. */
 for(int r=0;r<3;r++)for(int k=0;k<2;k++)for(int s=-1;s<=1;s+=2)for(int j=-3;j<=3;j++){
  city_color(.62,.68,.73);city_box(xs[r]+j*1.25f,-1.36,zs[k]+s*10.2f,.72,.024,3.2f);
 }
}
static void city_windows(CityBuilding b,int index){
 glDisable(GL_LIGHTING);glBegin(GL_QUADS);
 for(int row=0;row*4+3<b.height-2;row++){
  float y=-1.5f+3+row*4;
  for(float x=b.x-b.width*.5f+2;x<b.x+b.width*.5f-1;x+=4){
   for(int side=-1;side<=1;side+=2){
    int cell=index*10000+row*173+(int)(x*17)+side*31;float h=city_hash(cell);if(h>.53)continue;
    float z=b.z+side*(b.depth*.5f+.025f);
    if(h<.20)glColor3f(.95,.67,.32);else if(h<.36)glColor3f(.42,.78,1);else glColor3f(.65,.43,1);
    glVertex3f(x,y,z);glVertex3f(x+1.25f,y,z);glVertex3f(x+1.25f,y+1.45f,z);glVertex3f(x,y+1.45f,z);
   }
  }
  for(float z=b.z-b.depth*.5f+2;z<b.z+b.depth*.5f-1;z+=4){
   for(int side=-1;side<=1;side+=2){
    int cell=index*13000+row*197+(int)(z*19)+side*37;float h=city_hash(cell);if(h>.50)continue;
    float x=b.x+side*(b.width*.5f+.025f);
    if(h<.19)glColor3f(.95,.67,.32);else if(h<.35)glColor3f(.42,.78,1);else glColor3f(.65,.43,1);
    glVertex3f(x,y,z);glVertex3f(x,y,z+1.25f);glVertex3f(x,y+1.45f,z+1.25f);glVertex3f(x,y+1.45f,z);
   }
  }
 }
 glEnd();glEnable(GL_LIGHTING);
}
static void city_buildings(void){
 const float palettes[][3]={{.08,.12,.18},{.12,.085,.17},{.07,.15,.16},{.16,.10,.10}};
 for(int i=0;i<city_building_count();i++){
  CityBuilding b=city_building(i);const float *p=palettes[b.style&3];
  city_color(p[0],p[1],p[2]);city_box(b.x,-1.5f+b.height*.5f,b.z,b.width,b.height,b.depth);
  city_color(p[0]*1.35f,p[1]*1.35f,p[2]*1.35f);city_box(b.x,0,b.z,b.width+2,3,b.depth+2);
  city_color(.16,.20,.25);city_box(b.x,-1.5f+b.height+.45f,b.z,b.width*.72f,.9f,b.depth*.72f);
  for(float y=6;y<b.height-2;y+=8){city_color(.12,.16,.21);city_box(b.x,-1.5f+y,b.z,b.width+.25f,.22f,b.depth+.25f);}
  float accent=city_hash(i*29+3);city_color(.15+.35*accent,.55+.25*(1-accent),.75+.25*accent);
  city_box(b.x,1.1f,b.z-b.depth*.5f-.10f,b.width*.68f,.22f,.18f);
  city_color(.025,.035,.05);city_box(b.x,-.05f,b.z-b.depth*.5f-.14f,3.2f,2.9f,.20f);
  city_cylinder(v3(b.x,-1.5f+b.height+.9f,b.z),v3(b.x,-1.5f+b.height+5.5f,b.z),.11f,8);
  city_windows(b,i);
 }
}
static void city_streetlights(void){
 const float xs[]={-65,0,65},zs[]={-70,-115};
 for(int r=0;r<3;r++)for(float z=-202;z<82;z+=36)for(int side=-1;side<=1;side+=2){
  if(fabsf(z+70)<12||fabsf(z+115)<12)continue;
  float x=xs[r]+side*10.2f;city_color(.19,.23,.28);city_cylinder(v3(x,-1.5f,z),v3(x,5.4f,z),.09f,8);
  city_glow(v3(x,5.35f,z),v3(x-side*1.8f,5.35f,z),.07f,.35,.78,1);
 }
 for(int r=0;r<2;r++)for(float x=-145;x<146;x+=42)for(int side=-1;side<=1;side+=2){
  if(fabsf(x)<12||fabsf(x-65)<12||fabsf(x+65)<12)continue;
  float z=zs[r]+side*10.2f;city_color(.19,.23,.28);city_cylinder(v3(x,-1.5f,z),v3(x,5.4f,z),.09f,8);
  city_glow(v3(x,5.35f,z),v3(x,5.35f,z-side*1.8f),.07f,1,.52,.30);
 }
}
static void city_garage_sign(Server *s,CityGarage g,int index,int end){
 glPushMatrix();
 if(g.axis==1){glTranslatef(g.x,5.0f,g.z+end*(g.depth*.5f+.38f));glRotatef(end>0?0:180,0,1,0);}
 else{glTranslatef(g.x+end*(g.width*.5f+.38f),5.0f,g.z);glRotatef(end>0?90:-90,0,1,0);}
 city_texture(s->city_signs[index],-5,-.75f,10,1.5f,0);glPopMatrix();
}
static void city_garages(Server *s){
 for(int i=1;i<city_garage_count();i++){
  CityGarage g=city_garage(i);float along=g.axis==1?g.depth:g.width;
  city_color(.055,.068,.085);city_box(g.x,-1.48f,g.z,g.width,.16f,g.depth);
  city_color(.10,.12,.15);city_box(g.x,5.85f,g.z,g.width+1,.45f,g.depth+1);
  if(g.axis==1){
   city_color(.12,.15,.18);city_box(g.x-g.width*.5f,2.1f,g.z,.6f,7.2f,g.depth);city_box(g.x+g.width*.5f,2.1f,g.z,.6f,7.2f,g.depth);
  }else{
   city_color(.12,.15,.18);city_box(g.x,2.1f,g.z-g.depth*.5f,g.width,7.2f,.6f);city_box(g.x,2.1f,g.z+g.depth*.5f,g.width,7.2f,.6f);
  }
  for(int end=-1;end<=1;end+=2)for(int side=-1;side<=1;side+=2){
   float x=g.x,z=g.z;if(g.axis==1){x+=side*g.width*.5f;z+=end*along*.5f;}else{x+=end*along*.5f;z+=side*g.depth*.5f;}
   city_color(.22,.26,.31);city_box(x,2.1f,z,.8f,7.2f,.8f);
  }
  float ar=i==1?.20:i==2?.70:1,ag=i==1?.95:i==2?.42:.48,ab=i==1?.72:i==2?1:.30;
  if(g.axis==1){
   city_glow(v3(g.x-g.width*.5f+.5f,5.45f,g.z-along*.5f),v3(g.x-g.width*.5f+.5f,5.45f,g.z+along*.5f),.07f,ar,ag,ab);
   city_glow(v3(g.x+g.width*.5f-.5f,5.45f,g.z-along*.5f),v3(g.x+g.width*.5f-.5f,5.45f,g.z+along*.5f),.07f,ar,ag,ab);
  }else{
   city_glow(v3(g.x-along*.5f,5.45f,g.z-g.depth*.5f+.5f),v3(g.x+along*.5f,5.45f,g.z-g.depth*.5f+.5f),.07f,ar,ag,ab);
   city_glow(v3(g.x-along*.5f,5.45f,g.z+g.depth*.5f-.5f),v3(g.x+along*.5f,5.45f,g.z+g.depth*.5f-.5f),.07f,ar,ag,ab);
  }
  /* Service benches and charging pads sit against the side walls, leaving the
   * central drive-through lane physically and visually clear. */
  for(int side=-1;side<=1;side+=2){
   if(g.axis==1){city_color(.15,.20,.23);city_box(g.x+side*(g.width*.5f-1.4f),-.85f,g.z,1.8f,1.3f,along*.55f);}
   else{city_color(.15,.20,.23);city_box(g.x,-.85f,g.z+side*(g.depth*.5f-1.4f),along*.55f,1.3f,1.8f);}
  }
  city_garage_sign(s,g,i,-1);city_garage_sign(s,g,i,1);
 }
 /* Home has its original detailed hall; add a sign to its real doorway. */
 glPushMatrix();glTranslatef(0,7.2f,-39.55f);city_texture(s->city_signs[0],-6,-.75f,12,1.5f,0);glPopMatrix();
 glPushMatrix();glTranslatef(0,7.2f,-40.45f);glRotatef(180,0,1,0);city_texture(s->city_signs[0],-6,-.75f,12,1.5f,0);glPopMatrix();
}
static void city_static(Server *s){city_roads();city_buildings();city_streetlights();city_garages(s);}

static void city_traffic_car(int index,double time){
 CityTrafficCar car=city_traffic_at(index,time);
 glPushMatrix();glTranslatef(car.position.x,-.12f+sinf((float)time*2+index)*.035f,car.position.z);glRotatef(car.yaw,0,1,0);
 const float colors[][3]={{.12,.55,.72},{.62,.20,.75},{.78,.42,.12},{.18,.68,.42}};
 city_color(colors[index&3][0],colors[index&3][1],colors[index&3][2]);city_box(0,-.40f,0,CITY_TRAFFIC_HALF_WIDTH*2,.65f,(CITY_TRAFFIC_HALF_LENGTH-.10f)*2);
 city_color(.055,.075,.11);city_box(0,.12f,.25f,2.5f,.72f,3.3f);
 city_color(.02,.025,.035);city_box(0,.20f,-.45f,2.3f,.48f,1.7f);
 glDisable(GL_LIGHTING);city_color(.65,.90,1);city_box(0,-.30f,-3.34f,2.5f,.15f,.06f);city_color(1,.15,.25);city_box(0,-.30f,3.34f,2.3f,.13f,.06f);glEnable(GL_LIGHTING);
 for(int side=-1;side<=1;side+=2)for(int end=-1;end<=1;end+=2)city_glow(v3(side*1.35f,-.95f,end*2.2f),v3(side*1.35f,-.80f,end*2.2f),.16f,.25,.65,1);
 glPopMatrix();
}
static void city_dynamic(Server *s){
 for(int i=0;i<city_traffic_count();i++)city_traffic_car(i,s->vehicle.simulation_time);
 /* Rooftop beacons and rain are time-dependent and therefore stay outside the
  * static display list. Everything remains a closed-form function of time. */
 glDisable(GL_LIGHTING);glPointSize(5);glBegin(GL_POINTS);
 for(int i=0;i<city_building_count();i++){
  CityBuilding b=city_building(i);float pulse=fmodf((float)s->time*1.7f+i*.37f,1.f)<.18f?1.f:.12f;
  glColor3f(1*pulse,.12*pulse,.28*pulse);glVertex3f(b.x,-1.5f+b.height+5.6f,b.z);
 }glEnd();
 glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE);glDepthMask(GL_FALSE);glColor4f(.48,.66,1,.22f);glBegin(GL_LINES);
 for(int i=0;i<300;i++){
  float rx=-155+310*city_hash(i*3+1),rz=-220+310*city_hash(i*7+4);
  if(city_garage_at(v3(rx,0,rz)))continue;
  float speed=8+5*city_hash(i*5+9),ry=city_hash(i*11+6)*28-fmodf((float)s->time*speed,28);if(ry<0)ry+=28;ry-=1.5f;
  glVertex3f(rx,ry,rz);glVertex3f(rx-.09f,ry+.65f,rz);
 }
 glEnd();glDepthMask(GL_TRUE);glDisable(GL_BLEND);glEnable(GL_LIGHTING);
}
void city_render(Server *s){
 if(!s->city_signs[0])for(int i=0;i<4;i++)s->city_signs[i]=city_sign_texture(city_garage(i).name,i);
 if(!s->city_list){s->city_list=glGenLists(1);glNewList(s->city_list,GL_COMPILE);city_static(s);glEndList();}
 city_sky(s->vehicle.position);glCallList(s->city_list);city_dynamic(s);
}
