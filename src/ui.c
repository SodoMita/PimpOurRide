#include "chrome.h"
#include "city.h"
#include "fontprobe.h"
#include "i18n.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdio.h>
#include <webp/decode.h>
#define PI 3.14159265358979323846
static void rgb(cairo_t *c,float r,float g,float b){cairo_set_source_rgb(c,r,g,b);}
static void rgba(cairo_t *c,float r,float g,float b,float a){cairo_set_source_rgba(c,r,g,b,a);}
static const char *ui_font_family(bool mono){const char *f=mono?i18n_monofont():i18n_font();return f?f:(mono?"DejaVu Sans Mono":"DejaVu Sans");}
static void font(cairo_t *c,double size,bool bold,bool mono){cairo_select_font_face(c,ui_font_family(mono),CAIRO_FONT_SLANT_NORMAL,bold?CAIRO_FONT_WEIGHT_BOLD:CAIRO_FONT_WEIGHT_NORMAL);cairo_set_font_size(c,size);}
static void text(cairo_t *c,double x,double y,double size,const char *s,bool bold){font(c,size,bold,false);cairo_move_to(c,x,y);cairo_show_text(c,s);}
static void mono(cairo_t *c,double x,double y,double size,const char *s){font(c,size,false,true);cairo_move_to(c,x,y);cairo_show_text(c,s);}
/* Bounded mono text for baked textures: shrink to 85%, then ellipsize. Bake-
 * time only (these are 1200x76 titlebar strips), so the extra measurement pass
 * costs nothing per frame. */
static void mono_fit(cairo_t *c,double x,double y,double size,double maxw,const char *s){
 char buf[512];char cur[512];snprintf(cur,sizeof(cur),"%s",s?s:"");
 double scale=1.0;font(c,size,false,true);
 cairo_text_extents_t e;cairo_text_extents(c,cur,&e);
 if(e.x_advance>maxw&&maxw>0){
  scale=maxw/e.x_advance>(0.85)?maxw/e.x_advance:0.85;
  font(c,size*scale,false,true);cairo_text_extents(c,cur,&e);
  if(e.x_advance>maxw){
   size_t n=strlen(cur);
   while(n>0){
    while(n>0&&((unsigned char)cur[n]&0xC0)==0x80)n--;
    cur[n]=0;snprintf(buf,sizeof(buf),"%s\xe2\x80\xa6",cur);
    cairo_text_extents(c,buf,&e);
    if(e.x_advance<=maxw)break;
    if(n==0)break;
    n--;
   }
   snprintf(cur,sizeof(cur),"%s",buf);
  }
 }
 font(c,size*scale,false,true);cairo_move_to(c,x,y);cairo_show_text(c,cur);
}
static void rounded(cairo_t *c,double x,double y,double w,double h,double r){cairo_new_sub_path(c);cairo_arc(c,x+w-r,y+r,r,-PI/2,0);cairo_arc(c,x+w-r,y+h-r,r,0,PI/2);cairo_arc(c,x+r,y+h-r,r,PI/2,PI);cairo_arc(c,x+r,y+r,r,PI,PI*1.5);cairo_close_path(c);}
static void line(cairo_t *c,double x,double y,double xx,double yy){cairo_move_to(c,x,y);cairo_line_to(c,xx,yy);cairo_stroke(c);}
GLuint ui_texture(cairo_surface_t *im,GLuint reuse){
 cairo_surface_flush(im);GLuint tex=reuse;if(!tex)glGenTextures(1,&tex);glBindTexture(GL_TEXTURE_2D,tex);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
 glPixelStorei(GL_UNPACK_ALIGNMENT,4);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,cairo_image_surface_get_width(im),cairo_image_surface_get_height(im),0,GL_BGRA,GL_UNSIGNED_BYTE,cairo_image_surface_get_data(im));return tex;
}
GLuint ui_load_webp(const char *path){
 uint8_t *filebuf=NULL;long filesz=0;FILE *f=fopen(path,"rb");
 if(f){fseek(f,0,SEEK_END);filesz=ftell(f);fseek(f,0,SEEK_SET);if(filesz>0){filebuf=malloc((size_t)filesz);if(filebuf&&fread(filebuf,1,(size_t)filesz,f)!=(size_t)filesz){free(filebuf);filebuf=NULL;}}fclose(f);}
 int w=0,h=0;uint8_t *decoded=filebuf?WebPDecodeBGRA(filebuf,(size_t)filesz,&w,&h):NULL;
 free(filebuf);
 cairo_surface_t *im;
 if(!decoded){
  fprintf(stderr,"Texture: %s: webp decode failed\n",path);
  im=cairo_image_surface_create(CAIRO_FORMAT_ARGB32,64,64);
 } else {
  im=cairo_image_surface_create(CAIRO_FORMAT_ARGB32,w,h);
  uint8_t *dst=cairo_image_surface_get_data(im);int stride=cairo_image_surface_get_stride(im);
  for(int y=0;y<h;y++)memcpy(dst+(size_t)y*stride,decoded+(size_t)y*w*4,(size_t)w*4);
  cairo_surface_mark_dirty(im);
  WebPFree(decoded);
 }
 GLuint out=ui_texture(im,0);cairo_surface_destroy(im);return out;
}
static void icon(cairo_t *c,int i,double x,double y){
 cairo_save(c);cairo_translate(c,x,y);cairo_set_line_width(c,3);cairo_set_line_cap(c,CAIRO_LINE_CAP_ROUND);rgb(c,.48,.89,.78);
 if(i==0){cairo_move_to(c,0,0);cairo_line_to(c,13,11);cairo_line_to(c,0,22);cairo_stroke(c);line(c,18,22,34,22);}
 if(i==1){cairo_move_to(c,0,24);cairo_line_to(c,0,0);cairo_line_to(c,13,0);cairo_line_to(c,18,6);cairo_line_to(c,35,6);cairo_line_to(c,35,24);cairo_close_path(c);cairo_stroke(c);}
 if(i==2){line(c,10,22,10,-2);line(c,10,-2,34,-7);line(c,34,-7,34,17);cairo_arc(c,4,23,6,0,2*PI);cairo_fill(c);cairo_arc(c,28,18,6,0,2*PI);cairo_fill(c);}
 if(i==3){for(int n=0;n<4;n++){cairo_rectangle(c,n*10,25-n*8,5,n*8+3);cairo_fill(c);}}
 if(i==4){cairo_arc(c,17,12,11,0,2*PI);cairo_stroke(c);for(int n=0;n<8;n++){double a=n*PI/4;line(c,17+18*cos(a),12+18*sin(a),17+22*cos(a),12+22*sin(a));}}
 if(i==5){cairo_arc(c,17,12,18,0,2*PI);cairo_stroke(c);cairo_arc(c,17,12,5,0,2*PI);cairo_stroke(c);line(c,0,12,12,12);line(c,22,12,35,12);line(c,17,18,17,29);}
 cairo_restore(c);
}
static GLuint make_button(DashButton *b){
 cairo_surface_t *im=cairo_image_surface_create(CAIRO_FORMAT_ARGB32,384,224);cairo_t *c=cairo_create(im);
 cairo_pattern_t *grad=cairo_pattern_create_linear(0,0,0,224);cairo_pattern_add_color_stop_rgb(grad,0,.075,.095,.133);cairo_pattern_add_color_stop_rgb(grad,1,.033,.047,.073);cairo_set_source(c,grad);cairo_paint(c);cairo_pattern_destroy(grad);
 rgb(c,.17,.22,.30);cairo_set_line_width(c,2);rounded(c,2,2,380,220,15);cairo_stroke(c);
 icon(c,b->icon,30,40);rgb(c,.83,.89,.94);text(c,29,128,32,tr(b->name),true);rgb(c,.42,.51,.60);mono(c,30,180,20,tr(b->sub));rgb(c,.36,.91,.73);cairo_arc(c,345,43,4,0,2*PI);cairo_fill(c);
 if(!b->enabled){rgba(c,.01,.01,.02,.58);cairo_paint(c);rgb(c,.50,.55,.63);mono(c,246,198,16,tr("DISABLED"));} /* config-disabled entry reads as off, not broken */
 GLuint tex=ui_texture(im,0);cairo_destroy(c);cairo_surface_destroy(im);return tex;
}
void ui_refresh_buttons(Server *s){for(int i=0;i<6;i++){if(s->buttons[i].texture)glDeleteTextures(1,&s->buttons[i].texture);s->buttons[i].texture=make_button(&s->buttons[i]);}}
static void windshield_hud(Server *s){
 cairo_surface_t *im=cairo_image_surface_create(CAIRO_FORMAT_ARGB32,1400,460);cairo_t *c=cairo_create(im); /* transparent glass, not a panel */
 float r,g,b;scene_neon_color(s,&r,&g,&b);char value[96];
 rgba(c,r,g,b,.42);cairo_set_line_width(c,2);line(c,44,342,478,342);line(c,922,342,1356,342);
 rgba(c,r,g,b,.20);line(c,44,72,44,292);line(c,1356,72,1356,292);line(c,44,72,92,72);line(c,1308,72,1356,72);
 /* Live speed and gear at the left edge of the windshield. */
 snprintf(value,sizeof(value),"%.0f",fabs(s->vehicle.speed)*3.6);rgba(c,.80,.95,1,.92);text(c,70,218,112,value,true);
 rgba(c,.43,.68,.76,.85);mono(c,78,260,22,tr("KM/H"));mono(c,78,305,14,tr("GROUND SPEED"));
 const char *gear=s->vehicle.autopilot_enabled?"A":s->steering_mode==VEHICLE_STEER_CLIENT?"C":s->vehicle.speed>.25f?"D":s->vehicle.speed<-.25f?"R":"N";
 rgba(c,.45,.95,.79,.95);text(c,358,211,72,gear,true);rgba(c,.40,.61,.69,.85);mono(c,352,260,15,s->vehicle.autopilot_enabled?tr("AUTO"):s->steering_mode==VEHICLE_STEER_DRIVE?tr("DRIVE"):tr("CLIENT"));
 /* Pack and instantaneous power at the right edge. */
 snprintf(value,sizeof(value),"%.0f%%",100*s->vehicle.battery_kwh/VEHICLE_BATTERY_CAPACITY_KWH);rgba(c,.80,.95,1,.92);text(c,1070,190,72,value,true);
 rgba(c,.43,.68,.76,.85);mono(c,1080,232,18,tr("82 KWH PACK"));snprintf(value,sizeof(value),tr("%+.0f KW"),s->vehicle.power_kw);mono(c,1080,278,21,value);
 /* Centre stays mostly empty. A small live steering chevron and status line
  * read as glass projection without covering the fixed windshield client. */
 rgba(c,.48,.72,.80,.72);mono(c,568,62,14,tr("WINDSHIELD HUD / LIVE"));
 float steer_hud=s->steering_mode==VEHICLE_STEER_DRIVE?s->vehicle.steering:wheel_client_steer_from_angle(s->wheel_client_angle);
 rgba(c,r,g,b,.62);cairo_set_line_width(c,4);float cx=700+steer_hud*90;
 cairo_move_to(c,cx-34,370);cairo_line_to(c,cx,352);cairo_line_to(c,cx+34,370);cairo_stroke(c);
 char auto_mode[96];
 const char *mode=s->steering_mode==VEHICLE_STEER_DRIVE?(s->drive_brake?tr("REGEN BRAKE"):tr("HOVER DRIVE")):tr("CLIENT INPUT / PARKED");
 if(s->vehicle.autopilot_enabled){char dest[64];i18n_utf8_truncate(dest,sizeof(dest),city_autopilot_destination(&s->vehicle),62);snprintf(auto_mode,sizeof(auto_mode),tr("AUTOPILOT / %s"),dest);mode=auto_mode;}
 if(s->vehicle.door_target[0]>.5f||s->vehicle.door_target[1]>.5f)mode=s->vehicle.door_open[0]>.995f&&s->vehicle.door_open[1]>.995f?tr("DOORS OPEN / META+O TO CLOSE"):tr("DOORS OPENING");
 else if(s->vehicle.door_open[0]>.001f||s->vehicle.door_open[1]>.001f)mode=tr("DOORS CLOSING");
 rgba(c,.55,.77,.82,.82);cairo_text_extents_t ex;font(c,17,false,true);cairo_text_extents(c,mode,&ex);cairo_move_to(c,700-ex.width/2,414);cairo_show_text(c,mode);
 snprintf(value,sizeof(value),tr("HOVER %.2f M  /  MOTOR %.0f C"),s->vehicle.hover_height,s->vehicle.motor_temp_c);font(c,13,false,true);cairo_text_extents(c,value,&ex);cairo_move_to(c,700-ex.width/2,442);cairo_show_text(c,value);
 s->windshield_tex=ui_texture(im,s->windshield_tex);cairo_destroy(c);cairo_surface_destroy(im);
}
static void make_console(Server *s){
 cairo_surface_t *im=cairo_image_surface_create(CAIRO_FORMAT_ARGB32,1200,692);cairo_t *c=cairo_create(im);rgb(c,.032,.041,.061);cairo_paint(c);rgba(c,.15,.18,.26,.15);cairo_set_line_width(c,1);for(int x=-700;x<1400;x+=14)line(c,x,0,x+692,692);
 rgb(c,.78,.84,.9);text(c,58,76,33,tr("LAUNCH CONTROL"),true);rgb(c,.41,.49,.60);mono(c,61,114,16,tr("YOUR APPS. THEIR OWN DISPLAYS."));rgb(c,.34,.85,.70);cairo_arc(c,1067,69,5,0,2*PI);cairo_fill(c);mono(c,1090,76,17,tr("ON"));
 s->console_tex=ui_texture(im,0);cairo_destroy(c);cairo_surface_destroy(im);
}
static void make_plate(Server *s){
 cairo_surface_t *im=cairo_image_surface_create(CAIRO_FORMAT_ARGB32,800,300);cairo_t *c=cairo_create(im);rgb(c,.08,.10,.14);cairo_paint(c);rgba(c,.2,.23,.31,.4);cairo_set_line_width(c,.7);for(int y=0;y<300;y+=4)line(c,0,y,800,y);
 rgb(c,.84,.86,.92);cairo_select_font_face(c,ui_font_family(false),CAIRO_FONT_SLANT_OBLIQUE,CAIRO_FONT_WEIGHT_BOLD);cairo_set_font_size(c,67);cairo_move_to(c,45,134);cairo_show_text(c,tr("CUSTOM BUILT."));rgb(c,.57,.47,.78);mono(c,48,210,24,tr("NO STOCK PARTS / 001"));s->plate_tex=ui_texture(im,0);cairo_destroy(c);cairo_surface_destroy(im);
}
static void make_hub(Server *s){cairo_surface_t *im=cairo_image_surface_create(CAIRO_FORMAT_ARGB32,400,400);cairo_t *c=cairo_create(im);rgb(c,.025,.033,.052);cairo_arc(c,200,200,195,0,2*PI);cairo_fill(c);rgb(c,.28,.34,.43);cairo_set_line_width(c,2);cairo_arc(c,200,200,178,0,2*PI);cairo_stroke(c);rgb(c,.81,.88,.93);cairo_select_font_face(c,ui_font_family(false),CAIRO_FONT_SLANT_OBLIQUE,CAIRO_FONT_WEIGHT_BOLD);cairo_set_font_size(c,110);cairo_move_to(c,100,233);cairo_show_text(c,"CD");rgb(c,.65,.42,.88);mono(c,99,292,19,tr("CUSTOM / 01"));s->hub_tex=ui_texture(im,0);cairo_destroy(c);cairo_surface_destroy(im);}
static void make_handles_button(Server *s){
 cairo_surface_t *im=cairo_image_surface_create(CAIRO_FORMAT_ARGB32,384,112);cairo_t *c=cairo_create(im);
 cairo_pattern_t *grad=cairo_pattern_create_linear(0,0,0,112);cairo_pattern_add_color_stop_rgb(grad,0,.052,.105,.132);cairo_pattern_add_color_stop_rgb(grad,1,.018,.043,.066);cairo_set_source(c,grad);cairo_paint(c);cairo_pattern_destroy(grad);
 rgb(c,.28,.87,.72);cairo_set_line_width(c,3);rounded(c,3,3,378,106,14);cairo_stroke(c);
 /* Four-arrow move icon in a square: readable at titlebar size. */
 cairo_new_path(c);rgb(c,1,.68,.25);cairo_set_line_width(c,4);cairo_rectangle(c,27,27,58,58);cairo_stroke(c);
 line(c,38,56,74,56);line(c,56,38,56,74);line(c,38,56,48,47);line(c,38,56,48,65);line(c,74,56,64,47);line(c,74,56,64,65);line(c,56,38,47,48);line(c,56,38,65,48);line(c,56,74,47,64);line(c,56,74,65,64);
 rgb(c,.91,.95,.98);text(c,108,61,35,tr("HANDLES"),true);rgb(c,.48,.66,.72);mono(c,110,91,16,tr("MOVE / ROTATE / RESIZE"));
 s->handles_tex=ui_texture(im,0);cairo_destroy(c);cairo_surface_destroy(im);
}
void ui_monitor_label(Server *s,int i){
 Monitor *m=&s->monitors[i];if(!m->surface)return;char buf[300];bool active=s->keyboard_focus==m->surface,hot=s->hover_close==i;snprintf(buf,sizeof(buf),"%d:%d:%.138s:%.138s",active,hot,m->surface->title,m->surface->app_id);
 if(!strcmp(buf,m->label_text)&&m->label)return;snprintf(m->label_text,sizeof(m->label_text),"%s",buf);
 cairo_surface_t *im=cairo_image_surface_create(CAIRO_FORMAT_ARGB32,1200,76);cairo_t *c=cairo_create(im);rgb(c,.025,.04,.06);cairo_paint(c);
 rgb(c,active?.40:.52,active?.93:.49,active?.76:.74);cairo_arc(c,29,38,5,0,2*PI);cairo_fill(c);
 const char *display_name=m->surface->title;if(strstr(m->surface->app_id,"foot"))display_name=tr("TERMINAL");else if(strstr(m->surface->app_id,"files"))display_name=tr("FILES");else if(strstr(m->surface->app_id,"audio"))display_name=tr("AUDIO");else if(strstr(m->surface->app_id,"telemetry"))display_name=tr("METERS");/* Width-aware, not byte-aware: the title lane runs from x=54 to the mount
  * label at x=790, and a translated window name is not the same width as the
  * English it replaces. i18n_utf8_truncate() counts bytes - right for a cache
  * key, wrong for something a person reads. Shrink to 0.85, then ellipsize. */
 {
  /* surface->title is up to 255 bytes; snprintf is safe (it truncates) but it
   * would cut a UTF-8 sequence, so bound the name on a code-point boundary
   * first with the existing helper, then format. */
  char safe_name[128];i18n_utf8_truncate(safe_name,sizeof(safe_name),display_name,120);
  char whole[160];snprintf(whole,sizeof(whole),"%02d  /  %s",i+1,safe_name);
  mono_fit(c,54,48,25,730,whole);
 }
 rgb(c,.36,.49,.58);mono(c,790,47,17,active?tr("INPUT ACTIVE"):tr(monitor_mount_name(i)));
 if(hot){rgba(c,.65,.12,.22,.95);rounded(c,1128,9,62,58,12);cairo_fill(c);}
 rgb(c,hot?1:.50,hot?.82:.59,hot?.88:.67);cairo_set_line_width(c,hot?3:2);line(c,1150,30,1168,48);line(c,1168,30,1150,48);m->label=ui_texture(im,m->label);cairo_destroy(c);cairo_surface_destroy(im);
}
static void hud(Server *s){
 int w=s->width,h=s->height;cairo_surface_t *im=cairo_image_surface_create(CAIRO_FORMAT_ARGB32,w,h);cairo_t *c=cairo_create(im);
 /* Subtle edge shade keeps actual client windows unobstructed. */
 cairo_pattern_t *shade=cairo_pattern_create_linear(0,0,0,150);cairo_pattern_add_color_stop_rgba(shade,0,.018,.023,.038,.92);cairo_pattern_add_color_stop_rgba(shade,1,.018,.023,.038,0);cairo_set_source(c,shade);cairo_rectangle(c,0,0,w,150);cairo_fill(c);cairo_pattern_destroy(shade);
 shade=cairo_pattern_create_linear(0,h-140,0,h);cairo_pattern_add_color_stop_rgba(shade,0,.012,.019,.029,0);cairo_pattern_add_color_stop_rgba(shade,1,.012,.019,.029,.95);cairo_set_source(c,shade);cairo_rectangle(c,0,h-140,w,140);cairo_fill(c);cairo_pattern_destroy(shade);
 rgb(c,.61,.40,.91);cairo_set_line_width(c,2);rounded(c,28,26,44,44,11);cairo_stroke(c);cairo_move_to(c,44,37);cairo_line_to(c,56,48);cairo_line_to(c,44,59);cairo_stroke(c);
 rgb(c,.93,.95,.97);cairo_select_font_face(c,ui_font_family(false),CAIRO_FONT_SLANT_OBLIQUE,CAIRO_FONT_WEIGHT_BOLD);cairo_set_font_size(c,26);cairo_move_to(c,87,49);cairo_show_text(c,"CHROMEDRIVE");rgb(c,.47,.54,.63);mono(c,89,70,10,tr("THE DESKTOP. REBUILT."));
 int count=0;for(int i=0;i<CD_APP_BAYS;i++)if(s->monitors[i].surface)count++;char b[128];
 if(w>1050){rgb(c,.33,.43,.53);mono(c,w/2-72,43,10,tr("CUSTOM BUILD / 001"));rgb(c,.79,.82,.88);mono(c,w/2-65,64,12,tr("MIDNIGHT EDITION"));}
 rgba(c,.075,.12,.14,.85);rounded(c,w-210,28,182,41,20);cairo_fill(c);rgb(c,.38,.88,.70);cairo_arc(c,w-189,49,3.5,0,2*PI);cairo_fill(c);mono(c,w-174,53,11,tr("NATIVE SESSION"));
 rgba(c,.39,.45,.56,.2);cairo_set_line_width(c,1);line(c,28,91,w-28,91);
 const char *view=s->camera_mode==VEHICLE_CAMERA_DRIVER?(s->camera_seat==VEHICLE_SEAT_DRIVER?tr("DRIVER SEAT  /  FIRST PERSON"):tr("PASSENGER SEAT  /  FIRST PERSON")):s->camera_mode==VEHICLE_CAMERA_CHASE?tr("EXTERIOR VIEW  /  CONCEPT SPLINE BODY"):tr("EV SYSTEMS  /  800 V CUTAWAY");
 rgb(c,.42,.49,.6);mono(c,29,116,10,view);snprintf(b,sizeof(b),tr("%02d CONNECTED  /  %02d BAYS"),count,CD_APP_BAYS);mono(c,w-237,116,10,b);
 const char *garage=city_garage_at(s->vehicle.position);
 CityChunk city_here=city_chunk_at(s->vehicle.position);
 if(garage)snprintf(b,sizeof(b),"%s",garage);
 else if(city_stream_is_tunnel(s->vehicle.position))snprintf(b,sizeof(b),"COVERED TUNNEL / THROUGH ROAD");
 else if(city_stream_chunk_is_core(city_here)&&fabsf(s->vehicle.position.x)<=55.0f
         &&s->vehicle.position.z>=-50.0f&&s->vehicle.position.z<=60.0f)
  snprintf(b,sizeof(b),"HOME GARAGE CAMPUS / OFF-STREET COURT");
 else if(city_stream_chunk_is_core(city_here))snprintf(b,sizeof(b),"CHROMEDRIVE CORE / CITY STREETS");
 else snprintf(b,sizeof(b),"%s / %s",city_district_name(city_here.district),city_feature_name(city_here.feature));
 rgb(c,.38,.66,.72);mono(c,29,135,9,b);
 snprintf(b,sizeof(b),tr("FOV %.1f DEG"),s->fov);rgb(c,.38,.49,.60);mono(c,w-237,135,9,b);
 if(w>1050){snprintf(b,sizeof(b),tr("%3.0f%% PACK  /  %+6.1f KW"),100*s->vehicle.battery_kwh/VEHICLE_BATTERY_CAPACITY_KWH,s->vehicle.power_kw);mono(c,w/2-78,116,10,b);}
 /* The control hints are chrome, not fake app controls. The actual buttons are 3D. */
 if(w>=1100){int bx=w/2-438,by=h-66;rgba(c,.025,.038,.059,.88);rounded(c,bx,by,876,43,21);cairo_fill(c);rgba(c,.35,.43,.53,.30);rounded(c,bx,by,876,43,21);cairo_stroke(c);
  if(s->arrange_mode){rgb(c,1,.70,.28);mono(c,bx+28,by+26,11,s->arrange_handles?tr("GIZMO: amber move | RGB axes/rings | white corners/edges resize | magenta scale"):tr("DRAG move | HANDLES/H gizmo | SHIFT-DRAG yaw/pitch | CTRL-DRAG roll | WHEEL scale"));}
  else{
   const char *keys[]={"F7","WASD","META+O","META+A","META+H","F8","F9","F6","F1"};
   const char *labels[]={tr("Input"),tr("Drive"),tr("Doors"),tr("Auto"),tr("HUD"),tr("View"),tr("Seat"),tr("Arrange"),tr("Help")};
   int widths[]={94,98,106,100,94,84,82,100,0};int x=bx+12;
   for(int i=0;i<9;i++){int kw=strlen(keys[i])>5?48:strlen(keys[i])>3?34:25;rgb(c,.14,.19,.25);rounded(c,x,by+11,kw,21,4);cairo_fill(c);rgb(c,.80,.85,.91);mono(c,x+5,by+25,10,keys[i]);rgb(c,.60,.67,.74);text(c,x+kw+9,by+26,11,labels[i],false);x+=widths[i];}
  }
 }
 char mode[64];snprintf(mode,sizeof(mode),"%s",tr("ARRANGE MODE"));if(s->arrange_mode&&s->arrange_slot>=0)snprintf(mode,sizeof(mode),tr("ARRANGE MODE / CD-%02d"),s->arrange_slot+1);
 const char *seat_state=s->camera_mode==VEHICLE_CAMERA_SYSTEMS?tr("EV SKATEBOARD / NO WHEELS"):s->camera_mode==VEHICLE_CAMERA_CHASE?tr("SPLINE-BUILT ELECTRIC HOVER CAR"):s->steering_mode==VEHICLE_STEER_DRIVE?tr("HOVER DRIVE / F7 FOR CLIENTS"):tr("CLIENT INPUT / JOYSTICK WHEEL");
 char auto_state[128];if(s->vehicle.autopilot_enabled){char dest[92];i18n_utf8_truncate(dest,sizeof(dest),city_autopilot_destination(&s->vehicle),90);snprintf(auto_state,sizeof(auto_state),tr("AUTOPILOT / %s"),dest);seat_state=auto_state;}
 const char *state=s->arrange_mode?mode:s->focus_view&&s->camera_mode==VEHICLE_CAMERA_DRIVER?tr("FOCUS MODE"):seat_state;
 rgb(c,.75,.81,.87);text(c,29,h-47,12,state,true);rgb(c,.37,.45,.55);mono(c,29,h-26,9,"NATIVE WAYLAND / v" CD_VERSION);
 rgb(c,.72,.78,.84);snprintf(b,sizeof(b),"%02.0f FPS",s->fps);mono(c,w-94,h-47,12,b);rgb(c,.39,.48,.56);mono(c,w-107,h-26,9,"OPENGL / SHM");
 if(s->notice_until>s->time){font(c,12,false,false);cairo_text_extents_t ex;cairo_text_extents(c,s->notice,&ex);float tw=ex.width+44;rgba(c,.035,.06,.08,.94);rounded(c,(w-tw)/2,h-115,tw,31,15);cairo_fill(c);rgb(c,.53,.90,.77);text(c,(w-ex.width)/2,h-94,12,s->notice,false);}
 if(s->middle_look){rgba(c,.38,.93,.77,.8);cairo_set_line_width(c,1);line(c,w/2-9,h/2,w/2+9,h/2);line(c,w/2,h/2-9,w/2,h/2+9);}
 s->hud_tex=ui_texture(im,s->hud_tex);cairo_destroy(c);cairo_surface_destroy(im);
}
static void help(Server *s){
 cairo_surface_t *im=cairo_image_surface_create(CAIRO_FORMAT_ARGB32,1000,720);cairo_t *c=cairo_create(im);rgba(c,.025,.038,.063,.98);rounded(c,1,1,998,718,25);cairo_fill(c);rgb(c,.25,.33,.43);cairo_set_line_width(c,2);rounded(c,1,1,998,718,25);cairo_stroke(c);
 rgb(c,.52,.90,.77);mono(c,58,63,15,tr("CHROMEDRIVE / DRIVER'S MANUAL"));rgb(c,.94,.95,.97);text(c,55,130,45,tr("Make yourself at home."),true);rgb(c,.47,.58,.69);text(c,58,176,18,tr("Real Wayland apps. One virtual monitor for every window."),false);
 /* Physical key names (F7, META+O, META+H, WASD, META+SHIFT+Q) are not translated -
  * they are on the keyboard. The descriptive ones are chrome and do translate. */
 const char *keys[]={tr("MIDDLE + DRAG"),tr("CLICK / DRAG FRAME"),"META+O / META+A","F7 / META+SCROLLLOCK","WASD / SPACE",tr("DRAG WHEEL"),"F8 / F9 / F10","F2 / F3","F4 / F12","META+H","META+SHIFT+Q",tr("DASH COG")};
 const char *desc[]={tr("Look around from the selected seat"),tr("Focus a screen / drag its titlebar to move it"),tr("Doors / city autopilot; a drive key returns manual control"),tr("Toggle client/drive; Meta+ScrollLock grabs/releases app input"),tr("Accelerate, reverse, steer and regenerative brake"),tr("Steer in drive mode; joystick wheel in client mode"),tr("Change view / switch seat / recover the car"),tr("Selected seat / focus app or next spatial monitor"),tr("Change underglow / save a WebP screenshot"),tr("Show or hide the 2D HUD; the dash HUD button mirrors it"),tr("Quit immediately; red EXIT button needs a second press to confirm"),tr("Settings; Shift-click a tile for another instance")};
 for(int i=0;i<12;i++){int y=218+i*34;rgb(c,.69,.77,.88);mono(c,61,y,14,keys[i]);rgb(c,.54,.63,.73);text(c,294,y,14,desc[i],false);rgba(c,.26,.33,.43,.4);line(c,58,y+13,942,y+13);}
 rgb(c,.40,.49,.60);text(c,58,640,14,tr("Arrange: Tab next - HANDLES/H gizmo - drag transforms - R reset - right-click tile assigns app"),false);rgb(c,.50,.87,.73);mono(c,58,686,15,tr("F1 CLOSE    F6/ESC EXIT ARRANGE    CTRL+ALT+Q / META+SHIFT+Q EXIT"));s->help_tex=ui_texture(im,0);cairo_destroy(c);cairo_surface_destroy(im);

}
static void make_leather(Server *s){
 cairo_surface_t *im=cairo_image_surface_create(CAIRO_FORMAT_ARGB32,256,256);
 uint32_t *data=(uint32_t*)cairo_image_surface_get_data(im);uint32_t seed=7219;
 for(int y=0;y<256;y++)for(int x=0;x<256;x++){seed=1664525u*seed+1013904223u;int n=198+((seed>>24)&31);if((x+y)%6==0)n-=12;data[y*256+x]=0xff000000u|(n<<16)|(n<<8)|n;}
 cairo_surface_mark_dirty(im);s->carbon_tex=ui_texture(im,0);glBindTexture(GL_TEXTURE_2D,s->carbon_tex);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_REPEAT);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_REPEAT);cairo_surface_destroy(im);
}
void ui_init(Server *s){make_leather(s);ui_refresh_buttons(s);make_console(s);make_plate(s);make_hub(s);make_handles_button(s);windshield_hud(s);help(s);hud(s);}
/* Language changes re-bake every texture that has text drawn into it. Most
 * of these are built once in ui_init(); hud() and windshield_hud() already
 * refresh on their own timers, but rebuilding them here makes the switch
 * immediate instead of up-to-a-quarter-second late. Monitor titlebars cache
 * on their text, so clearing label_text forces the next ui_monitor_label()
 * to re-render them (the GL texture itself is reused, not leaked). */
static void drop_texture(GLuint *t){
 if(*t){glDeleteTextures(1,t);*t=0;}
}
void ui_refresh_text(Server *s){
 drop_texture(&s->plate_tex);drop_texture(&s->console_tex);drop_texture(&s->hub_tex);
 drop_texture(&s->handles_tex);drop_texture(&s->help_tex);
 for(int i=0;i<CD_MAX_MONITORS;i++)s->monitors[i].label_text[0]=0;
 ui_refresh_buttons(s);make_console(s);make_plate(s);make_hub(s);make_handles_button(s);
 help(s);hud(s);windshield_hud(s);
}
void ui_update(Server *s){
 static int old_w,old_h;
 if(s->time-s->last_windshield_time>.10){windshield_hud(s);s->last_windshield_time=s->time;}
 if(s->time-s->last_ui_time>.24||old_w!=s->width||old_h!=s->height){hud(s);s->last_ui_time=s->time;old_w=s->width;old_h=s->height;}
}
static void overlay_texture_region(GLuint tex,float x,float y,float w,float h,float u0,float v0,float u1,float v1){glBindTexture(GL_TEXTURE_2D,tex);glBegin(GL_QUADS);glTexCoord2f(u0,v0);glVertex2f(x,y);glTexCoord2f(u1,v0);glVertex2f(x+w,y);glTexCoord2f(u1,v1);glVertex2f(x+w,y+h);glTexCoord2f(u0,v1);glVertex2f(x,y+h);glEnd();}
static void overlay_texture(GLuint tex,float x,float y,float w,float h){overlay_texture_region(tex,x,y,w,h,0,0,1,1);}
/* Custom neon reticle, drawn into the framebuffer itself rather than relying
 * on a host cursor. There is no OS cursor at all in --headless mode (no X,
 * no window — raw VNC clients with no cursor-shape-update support would show
 * nothing), and this stays visually consistent with --windowed too. Widens
 * and brightens to the active underglow color on a hovered dashboard button. */
static void draw_cursor(Server *s){
 if(s->middle_look)return; /* mid-drag means "looking around", not pointing at anything */
 float r,g,b;scene_neon_color(s,&r,&g,&b);
 bool hot=s->hover_button>=0||s->hover_close>=0||s->cog_hover||s->quit_hover||s->hud_hover||s->wheel_hover||s->wheel_drag;float cx=s->mouse_x,cy=s->mouse_y,ring=hot?11.f:8.f;
 glDisable(GL_TEXTURE_2D);glLineWidth(hot?2.2f:1.4f);
 glColor4f(hot?1.f:r,hot?1.f:g,hot?.95f:b,hot?.95f:.78f);
 glBegin(GL_LINE_LOOP);for(int i=0;i<24;i++){float a=i*(2.f*(float)PI/24.f);glVertex2f(cx+cosf(a)*ring,cy+sinf(a)*ring);}glEnd();
 glBegin(GL_LINES);
 glVertex2f(cx-ring-6,cy);glVertex2f(cx-ring+2,cy);glVertex2f(cx+ring-2,cy);glVertex2f(cx+ring+6,cy);
 glVertex2f(cx,cy-ring-6);glVertex2f(cx,cy-ring+2);glVertex2f(cx,cy+ring-2);glVertex2f(cx,cy+ring+6);
 glEnd();
 glColor4f(r,g,b,.92f);glBegin(GL_TRIANGLE_FAN);for(int i=0;i<12;i++){float a=i*(2.f*(float)PI/12.f);glVertex2f(cx+cosf(a)*2.2f,cy+sinf(a)*2.2f);}glEnd();
 glLineWidth(1.f);
}
void ui_draw_overlay(Server *s){
 glDisable(GL_DEPTH_TEST);glDisable(GL_LIGHTING);glMatrixMode(GL_PROJECTION);glPushMatrix();glLoadIdentity();glOrtho(0,s->width,s->height,0,-1,1);glMatrixMode(GL_MODELVIEW);glPushMatrix();glLoadIdentity();glEnable(GL_BLEND);glBlendFunc(GL_ONE,GL_ONE_MINUS_SRC_ALPHA);glEnable(GL_TEXTURE_2D);glColor4f(1,1,1,1);
 if(s->hud_visible){
  /* hud() only paints the top 150 and bottom 140 pixels (apart from the
   * optional centre look reticle). Cropping the source texture avoids a
   * full-screen alpha blend over millions of known-transparent pixels. */
  float top=fminf(150,(float)s->height),bottom=fminf(140,(float)s->height-top);
  overlay_texture_region(s->hud_tex,0,0,s->width,top,0,0,1,top/s->height);
  if(bottom>0)overlay_texture_region(s->hud_tex,0,s->height-bottom,s->width,bottom,0,(s->height-bottom)/s->height,1,1);
  if(s->middle_look){float x=s->width/2-16,y=s->height/2-16;overlay_texture_region(s->hud_tex,x,y,32,32,x/s->width,y/s->height,(x+32)/s->width,(y+32)/s->height);}
 }
 if(s->show_help){glDisable(GL_TEXTURE_2D);glColor4f(0,0,0,.50);glBegin(GL_QUADS);glVertex2f(0,0);glVertex2f(s->width,0);glVertex2f(s->width,s->height);glVertex2f(0,s->height);glEnd();glEnable(GL_TEXTURE_2D);glColor4f(1,1,1,1);float scale=fminf((s->width-90)/1000.f,(s->height-120)/720.f);scale=fminf(scale,.9);overlay_texture(s->help_tex,(s->width-1000*scale)/2,(s->height-720*scale)/2,1000*scale,720*scale);}
 draw_cursor(s);
 glDisable(GL_TEXTURE_2D);glDisable(GL_BLEND);glPopMatrix();glMatrixMode(GL_PROJECTION);glPopMatrix();glMatrixMode(GL_MODELVIEW);glEnable(GL_DEPTH_TEST);
}
