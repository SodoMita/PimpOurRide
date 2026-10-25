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
 cairo_set_source_rgb(c,accents[index&3][0],accents[index&3][1],accents[index&3][2]);cairo_set_line_width(c,7);cairo_rectangle(c,7,7,1010,178);cairo_stroke(c);
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
 /* The hand-authored core exactly fills the aligned 4x5 streaming chunks.
  * Outside it, procedural chunks continue these same roads to infinity. */
 city_color(.025,.032,.045);city_box(0,-1.62,-40,320,.18,400);
 /* Home sits in a paved garage campus. There is no public through-street
  * behind its rear wall; the northern driveway remains the autopilot exit. */
 city_color(.055,.065,.078);city_box(0,-1.56,5,110,.10,110);
 const float xs[]={-65,0,65},zs[]={-70,-115};
 for(int i=0;i<3;i++){
  if(i==1)city_vertical_road(0,-240,-40);
  else city_vertical_road(xs[i],-240,160);
  for(float z=-238;z<158;z+=10){
   if(i==1&&z>-44)continue;
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
 /* Perimeter avenues join the procedural grid to the original streets. A
  * route arriving on any outer road can turn here and reach the campus. */
 const float edge_z[]={-240,160};
 for(int i=0;i<2;i++){
  float z=edge_z[i];city_color(.11,.13,.16);city_box(0,-1.50,z,320,.10,24);
  city_color(.035,.042,.055);city_box(0,-1.43,z,320,.08,16);
  for(float x=-156;x<158;x+=10){city_color(.85,.68,.26);city_box(x,-1.378,z,4.5f,.022,.18);}
 }
 for(int side=-1;side<=1;side+=2){
  city_vertical_road(side*160,-240,160);
  for(float z=-236;z<158;z+=10){city_color(.85,.68,.26);city_box(side*160,-1.378,z,.18,.022,4.5f);}
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
 for(int r=0;r<3;r++)for(float z=-202;z<154;z+=36)for(int side=-1;side<=1;side+=2){
  if(fabsf(z+70)<12||fabsf(z+115)<12||(r==1&&z>-44))continue;
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
static void city_drive_garage(Server *s,CityGarage g,int sign_index,bool with_sign){
 float along=g.axis==1?g.depth:g.width;
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
 static const float accents[][3]={{.45,.85,1},{.25,.95,.72},{.72,.48,1},{1,.48,.32}};
 const float *a=accents[sign_index&3];
 if(g.axis==1){
  city_glow(v3(g.x-g.width*.5f+.5f,5.45f,g.z-along*.5f),v3(g.x-g.width*.5f+.5f,5.45f,g.z+along*.5f),.07f,a[0],a[1],a[2]);
  city_glow(v3(g.x+g.width*.5f-.5f,5.45f,g.z-along*.5f),v3(g.x+g.width*.5f-.5f,5.45f,g.z+along*.5f),.07f,a[0],a[1],a[2]);
 }else{
  city_glow(v3(g.x-along*.5f,5.45f,g.z-g.depth*.5f+.5f),v3(g.x+along*.5f,5.45f,g.z-g.depth*.5f+.5f),.07f,a[0],a[1],a[2]);
  city_glow(v3(g.x-along*.5f,5.45f,g.z+g.depth*.5f-.5f),v3(g.x+along*.5f,5.45f,g.z+g.depth*.5f-.5f),.07f,a[0],a[1],a[2]);
 }
 /* Benches stay against the side walls; the central drive-through lane is
  * identical to the collision-free campus and streamed-court route. */
 for(int side=-1;side<=1;side+=2){
  if(g.axis==1){city_color(.15,.20,.23);city_box(g.x+side*(g.width*.5f-1.4f),-.85f,g.z,1.8f,1.3f,along*.55f);}
  else{city_color(.15,.20,.23);city_box(g.x,-.85f,g.z+side*(g.depth*.5f-1.4f),along*.55f,1.3f,1.8f);}
 }
 if(with_sign){city_garage_sign(s,g,sign_index,-1);city_garage_sign(s,g,sign_index,1);}
}
static void city_garages(Server *s){
 for(int i=1;i<city_garage_count();i++)city_drive_garage(s,city_garage(i),i,true);
 for(int i=0;i<city_campus_garage_count();i++)city_drive_garage(s,city_campus_garage(i),4+i,true);
 /* Home has its original detailed hall; add a sign to its real doorway. */
 glPushMatrix();glTranslatef(0,7.2f,-39.55f);city_texture(s->city_signs[0],-6,-.75f,12,1.5f,0);glPopMatrix();
 glPushMatrix();glTranslatef(0,7.2f,-40.45f);glRotatef(180,0,1,0);city_texture(s->city_signs[0],-6,-.75f,12,1.5f,0);glPopMatrix();
}
static void city_static(Server *s){city_roads();city_buildings();city_streetlights();city_garages(s);}

/* Procedural geometry is deliberately generated from the same chunk records
 * used by collision and garage lookup. There is no second visual-only city
 * with decorative buildings that the vehicle can drive through. */
static void city_stream_road(int edge,int segment,bool vertical,bool near){
 CityRoadType type=city_road_type(edge,segment,vertical);
 float width=city_road_width(type),start=segment*CITY_STREAM_CHUNK_SIZE;
 float x=vertical?edge*CITY_STREAM_CHUNK_SIZE:start+CITY_STREAM_CHUNK_SIZE*.5f;
 float z=vertical?start+CITY_STREAM_CHUNK_SIZE*.5f:edge*CITY_STREAM_CHUNK_SIZE;
 float sx=vertical?width:CITY_STREAM_CHUNK_SIZE,sz=vertical?CITY_STREAM_CHUNK_SIZE:width;
 float bx=vertical?width+8:CITY_STREAM_CHUNK_SIZE,bz=vertical?CITY_STREAM_CHUNK_SIZE:width+8;
 city_color(type==CITY_ROAD_ALLEY?.075:.105,type==CITY_ROAD_ALLEY?.082:.12,.15);
 city_box(x,-1.50f,z,bx,.10f,bz);
 city_color(type==CITY_ROAD_ALLEY?.055:.032,.040,.053);city_box(x,-1.43f,z,sx,.08f,sz);
 if(type==CITY_ROAD_TUNNEL){
  /* The 23 m clear road and wall centres match city_chunk_road_solid(). */
  float wx=vertical?11.5f:0,wz=vertical?0:11.5f;
  for(int side=-1;side<=1;side+=2){
   city_color(.16,.19,.24);city_box(x+side*wx,1.9f,z+side*wz,vertical?1:56,6.8f,vertical?56:1);
  }
  city_color(.11,.13,.17);city_box(x,5.5f,z,vertical?24:80,.8f,vertical?80:24);
  for(float d=5;d<80;d+=10){
   float px=vertical?x:start+d,pz=vertical?start+d:z;
   city_color(.85,.68,.26);city_box(px,-1.376f,pz,vertical?.18f:4.5f,.022f,vertical?4.5f:.18f);
  }
  for(int side=-1;side<=1;side+=2)city_color(.52,.57,.62),city_box(x+(vertical?side*6:0),-1.372f,z+(vertical?0:side*6),vertical?.12f:80,.018f,vertical?80:.12f);
  for(float d=8;d<80;d+=16)for(int side=-1;side<=1;side+=2){
   float lx=x+(vertical?side*8.2f:d-40),lz=z+(vertical?d-40:side*8.2f);
   city_glow(v3(lx,4.75f,lz),v3(lx+(vertical?0:4),4.75f,lz+(vertical?4:0)),.06f,.35,.72,1);
   float wxl=x+(vertical?side*10.85f:d-40),wzl=z+(vertical?d-40:side*10.85f);
   city_glow(v3(wxl,.1f,wzl),v3(wxl,2.4f,wzl),.055f,.22,.62,1);
  }
  return;
 }
 for(float d=5;d<80;d+=10){
  float px=vertical?x:start+d,pz=vertical?start+d:z;
  if(type==CITY_ROAD_BOULEVARD){
   /* A planted median distinguishes boulevards from ordinary local streets. */
   if(d>12&&d<68)city_color(.075,.20,.12),city_box(px,-1.34f,pz,vertical?1.8f:8,.12f,vertical?8:1.8f);
   for(int side=-1;side<=1;side+=2){city_color(.72,.75,.70);city_box(px+(vertical?side*width*.25f:0),-1.376f,pz+(vertical?0:side*width*.25f),vertical?.14f:4.5f,.022f,vertical?4.5f:.14f);}
  }else if(type==CITY_ROAD_AVENUE){
   city_color(.85,.68,.26);city_box(px,-1.376f,pz,vertical?.18f:4.5f,.022f,vertical?4.5f:.18f);
  }else if(type==CITY_ROAD_LOCAL){
   city_color(.58,.63,.67);city_box(px,-1.376f,pz,vertical?.14f:3.2f,.022f,vertical?3.2f:.14f);
  }
 }
 if(type==CITY_ROAD_BOULEVARD){
  float mx=vertical?x:start+40,mz=vertical?start+40:z;
  city_color(.52,.57,.62);city_box(mx+(vertical?-width*.5f:0),-1.36f,mz+(vertical?0:-width*.5f),vertical?.14f:80,.025f,vertical?80:.14f);
  city_box(mx+(vertical?width*.5f:0),-1.36f,mz+(vertical?0:width*.5f),vertical?.14f:80,.025f,vertical?80:.14f);
 }
 if(near&&type!=CITY_ROAD_ALLEY)for(int side=-1;side<=1;side+=2){
  float lx=x+(vertical?side*(width*.5f+2):-22),lz=z+(vertical?-22:side*(width*.5f+2));
  city_color(.20,.24,.29);city_cylinder(v3(lx,-1.45f,lz),v3(lx,5.75f,lz),.10f,6);
  city_glow(v3(lx,5.7f,lz),v3(lx+(vertical?0:1.3f),5.7f,lz+(vertical?1.3f:0)),.055f,.42,.72,1);
 }
}
static void city_stream_building(CitySolid solid,CityDistrict district,int key,bool near){
 static const float palettes[6][3]={{.065,.09,.17},{.15,.115,.17},{.12,.14,.15},{.08,.17,.12},{.16,.16,.20},{.075,.16,.17}};
 const float *p=palettes[district];float variation=.82f+.32f*city_hash(key);
 city_color(p[0]*variation,p[1]*variation,p[2]*variation);
 city_box(solid.x,-1.5f+solid.height*.5f,solid.z,solid.width,solid.height,solid.depth);
 city_color(.12,.15,.20);city_box(solid.x,-1.5f+solid.height+.15f,solid.z,solid.width+.3f,.3f,solid.depth+.3f);
 if(solid.height<5)return;
 CityBuilding b={0};b.x=solid.x;b.z=solid.z;b.width=solid.width;b.depth=solid.depth;b.height=solid.height;b.style=(int)district%3;
 if(near)city_windows(b,key);
 if(district==CITY_DISTRICT_INDUSTRIAL){
  for(int door=-1;door<=1;door++){
   city_color(.20,.23,.25);city_box(solid.x+door*solid.width*.24f,.2f,solid.z-solid.depth*.5f-.08f,solid.width*.18f,3.2f,.14f);
  }
  city_color(.22,.25,.27);city_box(solid.x,solid.height-1.1f,solid.z,solid.width*.35f,.8f,solid.depth*.35f);
 }else if(district==CITY_DISTRICT_CIVIC){
  for(int col=-2;col<=2;col++){float cx=solid.x+col*solid.width*.16f,cz=solid.z-solid.depth*.5f-.8f;city_color(.34,.36,.40);city_cylinder(v3(cx,-1.4f,cz),v3(cx,3.1f,cz),.28f,8);}
 }else if(district==CITY_DISTRICT_RESIDENTIAL&&near){
  for(float y=2;y<solid.height-2;y+=5){
   city_color(.22,.20,.25);city_box(solid.x,y,solid.z-solid.depth*.5f-.45f,solid.width*.75f,.16f,1.0f);
  }
 }else if(district==CITY_DISTRICT_DOWNTOWN){
  city_glow(v3(solid.x-solid.width*.5f-.05f,1,solid.z-solid.depth*.5f),v3(solid.x-solid.width*.5f-.05f,solid.height-2,solid.z-solid.depth*.5f),.045f,.25,.55,1);
 }
}
static void city_stream_solid(Server *s,CityChunk chunk,CitySolid solid,int index,bool near){
 int key=(int)(((unsigned)chunk.x*97u+(unsigned)chunk.z*131u+(unsigned)index*17u)&0x7fffffffu);
 switch(solid.kind){
 case CITY_SOLID_BUILDING:city_stream_building(solid,chunk.district,key,near);break;
 case CITY_SOLID_KIOSK:
  city_color(.12,.17,.21);city_box(solid.x,.5f,solid.z,solid.width,4,solid.depth);
  city_glow(v3(solid.x-solid.width*.45f,2.2f,solid.z-solid.depth*.51f),v3(solid.x+solid.width*.45f,2.2f,solid.z-solid.depth*.51f),.06f,.3,.85,1);break;
 case CITY_SOLID_FOUNTAIN:
  city_color(.20,.25,.30);city_cylinder(v3(solid.x,-1.38f,solid.z),v3(solid.x,-.38f,solid.z),solid.width*.5f,16);
  city_color(.08,.32,.46);city_cylinder(v3(solid.x,-.35f,solid.z),v3(solid.x,-.23f,solid.z),solid.width*.42f,16);
  city_glow(v3(solid.x,-.15f,solid.z),v3(solid.x,2.2f,solid.z),.09f,.25,.65,1);break;
 case CITY_SOLID_PARKED_CAR:{
  float hue=city_hash(key);city_color(.18+.55*hue,.16+.35*(1-hue),.28+.45*hue);
  city_box(solid.x,-.72f,solid.z,solid.width,.85f,solid.depth);
  city_color(.035,.055,.085);city_box(solid.x,-.12f,solid.z+.15f,solid.width*.78f,.55f,solid.depth*.52f);break;}
 case CITY_SOLID_TREE:
  city_color(.20,.12,.065);city_cylinder(v3(solid.x,-1.45f,solid.z),v3(solid.x,1.85f,solid.z),.30f,7);
  city_color(.07+.05f*city_hash(key),.24+.12f*city_hash(key+2),.10);city_cylinder(v3(solid.x,1.2f,solid.z),v3(solid.x,5.4f,solid.z),2.4f+.6f*city_hash(key+4),8);break;
 default:city_color(.16,.18,.22);city_box(solid.x,-1.5f+solid.height*.5f,solid.z,solid.width,solid.height,solid.depth);break;
 }
}
static void city_stream_chunk(Server *s,CityChunk chunk,bool near){
 float x=chunk.x*CITY_STREAM_CHUNK_SIZE+40,z=chunk.z*CITY_STREAM_CHUNK_SIZE+40;
 static const float ground[6][3]={{.035,.045,.065},{.055,.045,.065},{.055,.060,.065},{.035,.075,.045},{.065,.065,.080},{.040,.065,.070}};
 const float *g=ground[chunk.district];city_color(g[0],g[1],g[2]);city_box(x,-1.62f,z,80,.18f,80);
 city_stream_road(chunk.x,chunk.z,true,near);city_stream_road(chunk.z,chunk.x,false,near);
 CityChunk east={.x=chunk.x+1,.z=chunk.z},south={.x=chunk.x,.z=chunk.z+1};
 if(city_stream_chunk_is_core(east))city_stream_road(chunk.x+1,chunk.z,true,near);
 if(city_stream_chunk_is_core(south))city_stream_road(chunk.z+1,chunk.x,false,near);
 /* Land-use paving occupies only the interior, leaving all five road types
  * and their sidewalks unobstructed. */
 if(chunk.feature==CITY_FEATURE_PARK){city_color(.055,.19,.085);city_box(x,-1.48f,z,50,.10f,50);}
 else if(chunk.feature==CITY_FEATURE_SQUARE){city_color(.19,.18,.22);city_box(x,-1.48f,z,52,.10f,52);}
 else if(chunk.feature==CITY_FEATURE_PARKING||chunk.feature==CITY_FEATURE_GARAGES){
  city_color(.055,.065,.075);city_box(x,-1.48f,z,52,.10f,52);
  for(int stall=-2;stall<=2;stall++){city_color(.52,.57,.60);city_box(x+stall*4.5f,-1.415f,z-23,.10f,.025f,8);city_box(x+stall*4.5f,-1.415f,z+23,.10f,.025f,8);}
 }
 int count=city_chunk_solid_count(chunk);for(int i=0;i<count;i++)city_stream_solid(s,chunk,city_chunk_solid(chunk,i),i,near);
 for(int i=0;i<city_chunk_garage_count(chunk);i++)city_drive_garage(s,city_chunk_garage(chunk,i),8+i,true);
}
static void city_stream(Server *s){
 CityChunk centre=city_chunk_at(s->vehicle.position);
 for(int dz=-2;dz<=2;dz++)for(int dx=-2;dx<=2;dx++){
  CityChunk chunk=city_chunk(centre.x+dx,centre.z+dz);if(city_stream_chunk_is_core(chunk))continue;
  float x=chunk.x*80+40-s->vehicle.position.x,z=chunk.z*80+40-s->vehicle.position.z;
  float distance=x*x+z*z;if(distance>200.0f*200.0f)continue;
  city_stream_chunk(s,chunk,distance<110.0f*110.0f);
 }
}

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
  float rx=s->vehicle.position.x-155+310*city_hash(i*3+1),rz=s->vehicle.position.z-155+310*city_hash(i*7+4);
  if(city_garage_at(v3(rx,0,rz))||city_stream_is_tunnel(v3(rx,0,rz)))continue;
  float speed=8+5*city_hash(i*5+9),ry=city_hash(i*11+6)*28-fmodf((float)s->time*speed,28);if(ry<0)ry+=28;ry-=1.5f;
  glVertex3f(rx,ry,rz);glVertex3f(rx-.09f,ry+.65f,rz);
 }
 glEnd();glDepthMask(GL_TRUE);glDisable(GL_BLEND);glEnable(GL_LIGHTING);
}
void city_render(Server *s){
 if(!s->city_signs[0]){
  for(int i=0;i<4;i++)s->city_signs[i]=city_sign_texture(city_garage(i).name,i);
  for(int i=0;i<city_campus_garage_count();i++)s->city_signs[4+i]=city_sign_texture(city_campus_garage(i).name,4+i);
  for(int i=0;i<2;i++)s->city_signs[8+i]=city_sign_texture(city_chunk_garage(city_chunk(2,-4),i).name,8+i);
 }
 if(!s->city_list){s->city_list=glGenLists(1);glNewList(s->city_list,GL_COMPILE);city_static(s);glEndList();}
 city_sky(s->vehicle.position);glCallList(s->city_list);city_stream(s);city_dynamic(s);
}
