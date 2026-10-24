#include "chrome.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdio.h>
#include <webp/decode.h>
#define PI 3.14159265358979323846
static void rgb(cairo_t *c,float r,float g,float b){cairo_set_source_rgb(c,r,g,b);}
static void rgba(cairo_t *c,float r,float g,float b,float a){cairo_set_source_rgba(c,r,g,b,a);}
static void font(cairo_t *c,double size,bool bold,bool mono){cairo_select_font_face(c,mono?"DejaVu Sans Mono":"DejaVu Sans",CAIRO_FONT_SLANT_NORMAL,bold?CAIRO_FONT_WEIGHT_BOLD:CAIRO_FONT_WEIGHT_NORMAL);cairo_set_font_size(c,size);}
static void text(cairo_t *c,double x,double y,double size,const char *s,bool bold){font(c,size,bold,false);cairo_move_to(c,x,y);cairo_show_text(c,s);}
static void mono(cairo_t *c,double x,double y,double size,const char *s){font(c,size,false,true);cairo_move_to(c,x,y);cairo_show_text(c,s);}
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
static GLuint make_button(DashButton *b,int index){
 cairo_surface_t *im=cairo_image_surface_create(CAIRO_FORMAT_ARGB32,384,224);cairo_t *c=cairo_create(im);
 cairo_pattern_t *grad=cairo_pattern_create_linear(0,0,0,224);cairo_pattern_add_color_stop_rgb(grad,0,.075,.095,.133);cairo_pattern_add_color_stop_rgb(grad,1,.033,.047,.073);cairo_set_source(c,grad);cairo_paint(c);cairo_pattern_destroy(grad);
 rgb(c,.17,.22,.30);cairo_set_line_width(c,2);rounded(c,2,2,380,220,15);cairo_stroke(c);
 icon(c,index,30,40);rgb(c,.83,.89,.94);text(c,29,128,32,b->name,true);rgb(c,.42,.51,.60);mono(c,30,180,20,b->sub);rgb(c,.36,.91,.73);cairo_arc(c,345,43,4,0,2*PI);cairo_fill(c);
 GLuint tex=ui_texture(im,0);cairo_destroy(c);cairo_surface_destroy(im);return tex;
}
static void gauges(Server *s){
 cairo_surface_t *im=cairo_image_surface_create(CAIRO_FORMAT_ARGB32,1200,484);cairo_t *c=cairo_create(im);rgb(c,.015,.021,.037);cairo_paint(c);
 int count=0;for(int i=0;i<CD_MAX_MONITORS;i++)if(s->monitors[i].surface)count++;
 for(int k=0;k<2;k++){
  double cx=k?930:270,cy=247,r=184;rgba(c,.55,.42,.8,.13);cairo_arc(c,cx,cy,r+12,0,2*PI);cairo_set_line_width(c,16);cairo_stroke(c);rgb(c,.27,.29,.38);cairo_arc(c,cx,cy,r+6,0,2*PI);cairo_set_line_width(c,3);cairo_stroke(c);
  cairo_set_line_width(c,9);rgb(c,.12,.16,.23);cairo_arc(c,cx,cy,r-4,.73*PI,2.27*PI);cairo_stroke(c);rgb(c,k?.67:.30,k?.34:.89,k?.98:.78);double value=k?(double)count/8:fmin(s->fps/120,1);cairo_arc(c,cx,cy,r-4,.73*PI,.73*PI+1.54*PI*value);cairo_stroke(c);
  for(int i=0;i<=40;i++){double a=.73*PI+i*1.54*PI/40;rgb(c,i%5==0?.77:.3,i%5==0?.79:.36,i%5==0?.84:.43);cairo_set_line_width(c,i%5==0?3:1.4);line(c,cx+(r-19)*cos(a),cy+(r-19)*sin(a),cx+(r-(i%5==0?38:28))*cos(a),cy+(r-(i%5==0?38:28))*sin(a));}
  char value_text[32];if(k)snprintf(value_text,sizeof(value_text),"%02d",count);else snprintf(value_text,sizeof(value_text),"%.0f",s->fps);
  rgb(c,.90,.93,.98);font(c,99,true,false);cairo_text_extents_t e;cairo_text_extents(c,value_text,&e);cairo_move_to(c,cx-e.width/2-e.x_bearing,cy+28);cairo_show_text(c,value_text);rgb(c,.43,.56,.63);mono(c,cx-40,cy+74,19,k?"SCREENS":"FPS");
  rgb(c,.36,.90,.74);cairo_arc(c,cx,cy+115,4,0,2*PI);cairo_fill(c);
 }
 rgb(c,.67,.72,.80);mono(c,495,153,20,"CD // 001");rgb(c,.35,.87,.72);text(c,529,255,58,"P",true);rgb(c,.46,.53,.65);mono(c,495,303,18,"PARKED");mono(c,485,381,15,"NATIVE / SHM");
 s->gauge_tex=ui_texture(im,s->gauge_tex);cairo_destroy(c);cairo_surface_destroy(im);
}
static void make_console(Server *s){
 cairo_surface_t *im=cairo_image_surface_create(CAIRO_FORMAT_ARGB32,1200,692);cairo_t *c=cairo_create(im);rgb(c,.032,.041,.061);cairo_paint(c);rgba(c,.15,.18,.26,.15);cairo_set_line_width(c,1);for(int x=-700;x<1400;x+=14)line(c,x,0,x+692,692);
 rgb(c,.78,.84,.9);text(c,58,76,33,"LAUNCH CONTROL",true);rgb(c,.41,.49,.60);mono(c,61,114,16,"YOUR APPS. THEIR OWN DISPLAYS.");rgb(c,.34,.85,.70);cairo_arc(c,1067,69,5,0,2*PI);cairo_fill(c);mono(c,1090,76,17,"ON");
 s->console_tex=ui_texture(im,0);cairo_destroy(c);cairo_surface_destroy(im);
}
static void make_plate(Server *s){
 cairo_surface_t *im=cairo_image_surface_create(CAIRO_FORMAT_ARGB32,800,300);cairo_t *c=cairo_create(im);rgb(c,.08,.10,.14);cairo_paint(c);rgba(c,.2,.23,.31,.4);cairo_set_line_width(c,.7);for(int y=0;y<300;y+=4)line(c,0,y,800,y);
 rgb(c,.84,.86,.92);cairo_select_font_face(c,"DejaVu Sans",CAIRO_FONT_SLANT_OBLIQUE,CAIRO_FONT_WEIGHT_BOLD);cairo_set_font_size(c,67);cairo_move_to(c,45,134);cairo_show_text(c,"CUSTOM BUILT.");rgb(c,.57,.47,.78);mono(c,48,210,24,"NO STOCK PARTS / 001");s->plate_tex=ui_texture(im,0);cairo_destroy(c);cairo_surface_destroy(im);
}
static void make_hub(Server *s){cairo_surface_t *im=cairo_image_surface_create(CAIRO_FORMAT_ARGB32,400,400);cairo_t *c=cairo_create(im);rgb(c,.025,.033,.052);cairo_arc(c,200,200,195,0,2*PI);cairo_fill(c);rgb(c,.28,.34,.43);cairo_set_line_width(c,2);cairo_arc(c,200,200,178,0,2*PI);cairo_stroke(c);rgb(c,.81,.88,.93);cairo_select_font_face(c,"DejaVu Sans",CAIRO_FONT_SLANT_OBLIQUE,CAIRO_FONT_WEIGHT_BOLD);cairo_set_font_size(c,110);cairo_move_to(c,100,233);cairo_show_text(c,"CD");rgb(c,.65,.42,.88);mono(c,99,292,19,"CUSTOM / 01");s->hub_tex=ui_texture(im,0);cairo_destroy(c);cairo_surface_destroy(im);}
void ui_monitor_label(Server *s,int i){
 Monitor *m=&s->monitors[i];if(!m->surface)return;char buf[300];bool active=s->keyboard_focus==m->surface;snprintf(buf,sizeof(buf),"%d:%.140s:%.140s",active,m->surface->title,m->surface->app_id);
 if(!strcmp(buf,m->label_text)&&m->label)return;snprintf(m->label_text,sizeof(m->label_text),"%s",buf);
 cairo_surface_t *im=cairo_image_surface_create(CAIRO_FORMAT_ARGB32,1200,76);cairo_t *c=cairo_create(im);rgb(c,.025,.04,.06);cairo_paint(c);
 rgb(c,active?.40:.52,active?.93:.49,active?.76:.74);cairo_arc(c,29,38,5,0,2*PI);cairo_fill(c);
 char title[90],name[48];const char *display_name=m->surface->title;if(strstr(m->surface->app_id,"foot"))display_name="TERMINAL";else if(strstr(m->surface->app_id,"files"))display_name="FILES";else if(strstr(m->surface->app_id,"audio"))display_name="AUDIO";else if(strstr(m->surface->app_id,"telemetry"))display_name="METERS";snprintf(name,sizeof(name),"%.42s",display_name);snprintf(title,sizeof(title),"%02d  /  %s",i+1,name);
 rgb(c,.76,.82,.89);mono(c,54,48,25,title);rgb(c,.36,.49,.58);mono(c,845,47,19,active?"INPUT ACTIVE":"WAYLAND");rgb(c,.50,.59,.67);cairo_set_line_width(c,2);line(c,1150,30,1168,48);line(c,1168,30,1150,48);m->label=ui_texture(im,m->label);cairo_destroy(c);cairo_surface_destroy(im);
}
static void hud(Server *s){
 int w=s->width,h=s->height;cairo_surface_t *im=cairo_image_surface_create(CAIRO_FORMAT_ARGB32,w,h);cairo_t *c=cairo_create(im);
 /* Subtle edge shade keeps actual client windows unobstructed. */
 cairo_pattern_t *shade=cairo_pattern_create_linear(0,0,0,150);cairo_pattern_add_color_stop_rgba(shade,0,.018,.023,.038,.92);cairo_pattern_add_color_stop_rgba(shade,1,.018,.023,.038,0);cairo_set_source(c,shade);cairo_rectangle(c,0,0,w,150);cairo_fill(c);cairo_pattern_destroy(shade);
 shade=cairo_pattern_create_linear(0,h-140,0,h);cairo_pattern_add_color_stop_rgba(shade,0,.012,.019,.029,0);cairo_pattern_add_color_stop_rgba(shade,1,.012,.019,.029,.95);cairo_set_source(c,shade);cairo_rectangle(c,0,h-140,w,140);cairo_fill(c);cairo_pattern_destroy(shade);
 rgb(c,.61,.40,.91);cairo_set_line_width(c,2);rounded(c,28,26,44,44,11);cairo_stroke(c);cairo_move_to(c,44,37);cairo_line_to(c,56,48);cairo_line_to(c,44,59);cairo_stroke(c);
 rgb(c,.93,.95,.97);cairo_select_font_face(c,"DejaVu Sans",CAIRO_FONT_SLANT_OBLIQUE,CAIRO_FONT_WEIGHT_BOLD);cairo_set_font_size(c,26);cairo_move_to(c,87,49);cairo_show_text(c,"CHROMEDRIVE");rgb(c,.47,.54,.63);mono(c,89,70,10,"THE DESKTOP. REBUILT.");
 int count=0;for(int i=0;i<CD_MAX_MONITORS;i++)if(s->monitors[i].surface)count++;char b[128];
 if(w>1050){rgb(c,.33,.43,.53);mono(c,w/2-72,43,10,"CUSTOM BUILD / 001");rgb(c,.79,.82,.88);mono(c,w/2-65,64,12,"MIDNIGHT EDITION");}
 rgba(c,.075,.12,.14,.85);rounded(c,w-210,28,182,41,20);cairo_fill(c);rgb(c,.38,.88,.70);cairo_arc(c,w-189,49,3.5,0,2*PI);cairo_fill(c);mono(c,w-174,53,11,"NATIVE SESSION");
 rgba(c,.39,.45,.56,.2);cairo_set_line_width(c,1);line(c,28,91,w-28,91);
 rgb(c,.42,.49,.6);mono(c,29,116,10,"DRIVER VIEW  /  FIRST PERSON");snprintf(b,sizeof(b),"%02d CONNECTED  /  %02d BAYS",count,CD_MAX_MONITORS);mono(c,w-237,116,10,b);
 /* The control hints are chrome, not fake app controls. The actual buttons are 3D. */
 if(w>=1100){int bx=w/2-438,by=h-66;rgba(c,.025,.038,.059,.88);rounded(c,bx,by,876,43,21);cairo_fill(c);rgba(c,.35,.43,.53,.30);rounded(c,bx,by,876,43,21);cairo_stroke(c);
  const char *keys[]={"MMB","F2","F3","F4","F6","F1"};const char *labels[]={"Look around","Driver view","Focus app","Neon","Next screen","Help"};int widths[]={181,165,159,115,159,0};int x=bx+18;
  for(int i=0;i<6;i++){rgb(c,.14,.19,.25);rounded(c,x,by+11,i?25:34,21,4);cairo_fill(c);rgb(c,.80,.85,.91);mono(c,x+5,by+25,10,keys[i]);rgb(c,.60,.67,.74);text(c,x+(i?34:44),by+26,11,labels[i],false);x+=widths[i];}
 }
 rgb(c,.75,.81,.87);text(c,29,h-47,12,s->focus_view?"FOCUS MODE":"IN THE DRIVER'S SEAT",true);rgb(c,.37,.45,.55);mono(c,29,h-26,9,"NESTED WAYLAND / v" CD_VERSION);
 rgb(c,.72,.78,.84);snprintf(b,sizeof(b),"%02.0f FPS",s->fps);mono(c,w-94,h-47,12,b);rgb(c,.39,.48,.56);mono(c,w-107,h-26,9,"OPENGL / SHM");
 if(s->notice_until>s->time){font(c,12,false,false);cairo_text_extents_t ex;cairo_text_extents(c,s->notice,&ex);float tw=ex.width+44;rgba(c,.035,.06,.08,.94);rounded(c,(w-tw)/2,h-115,tw,31,15);cairo_fill(c);rgb(c,.53,.90,.77);text(c,(w-ex.width)/2,h-94,12,s->notice,false);}
 if(s->middle_look){rgba(c,.38,.93,.77,.8);cairo_set_line_width(c,1);line(c,w/2-9,h/2,w/2+9,h/2);line(c,w/2,h/2-9,w/2,h/2+9);}
 s->hud_tex=ui_texture(im,s->hud_tex);cairo_destroy(c);cairo_surface_destroy(im);
}
static void help(Server *s){
 cairo_surface_t *im=cairo_image_surface_create(CAIRO_FORMAT_ARGB32,1000,720);cairo_t *c=cairo_create(im);rgba(c,.025,.038,.063,.98);rounded(c,1,1,998,718,25);cairo_fill(c);rgb(c,.25,.33,.43);cairo_set_line_width(c,2);rounded(c,1,1,998,718,25);cairo_stroke(c);
 rgb(c,.52,.90,.77);mono(c,58,63,15,"CHROMEDRIVE / DRIVER'S MANUAL");rgb(c,.94,.95,.97);text(c,55,130,45,"Make yourself at home.",true);rgb(c,.47,.58,.69);text(c,58,176,18,"Real Wayland apps. One virtual monitor for every window.",false);
 const char *keys[]={"MIDDLE + DRAG","CLICK A SCREEN","F2 / F3","F4 / F6","SHIFT + BUTTON","CTRL + ALT + ENTER","F11 / F12"};const char *desc[]={"Look around from the driver's seat","Send keyboard and pointer input to that app","Restore driver view / focus the selected monitor","Change underglow / cycle to the next monitor","Launch another instance instead of focusing","Open a new terminal on its own screen","Fullscreen host window / save a screenshot"};
 for(int i=0;i<7;i++){int y=234+i*51;rgb(c,.69,.77,.88);mono(c,61,y,17,keys[i]);rgb(c,.54,.63,.73);text(c,328,y,17,desc[i],false);rgba(c,.26,.33,.43,.4);line(c,58,y+18,942,y+18);}
 rgb(c,.40,.49,.60);text(c,58,640,15,"Dashboard controls are ray-picked in 3D. Right click is passed to apps.",false);rgb(c,.50,.87,.73);mono(c,58,686,15,"F1 TO CLOSE       CTRL + ALT + Q TO EXIT");s->help_tex=ui_texture(im,0);cairo_destroy(c);cairo_surface_destroy(im);
}
static void make_leather(Server *s){
 cairo_surface_t *im=cairo_image_surface_create(CAIRO_FORMAT_ARGB32,256,256);
 uint32_t *data=(uint32_t*)cairo_image_surface_get_data(im);uint32_t seed=7219;
 for(int y=0;y<256;y++)for(int x=0;x<256;x++){seed=1664525u*seed+1013904223u;int n=198+((seed>>24)&31);if((x+y)%6==0)n-=12;data[y*256+x]=0xff000000u|(n<<16)|(n<<8)|n;}
 cairo_surface_mark_dirty(im);s->carbon_tex=ui_texture(im,0);glBindTexture(GL_TEXTURE_2D,s->carbon_tex);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_REPEAT);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_REPEAT);cairo_surface_destroy(im);
}
void ui_init(Server *s){make_leather(s);for(int i=0;i<6;i++)s->buttons[i].texture=make_button(&s->buttons[i],i);make_console(s);make_plate(s);make_hub(s);gauges(s);help(s);hud(s);}
void ui_update(Server *s){static int old_w,old_h;if(s->time-s->last_ui_time>.24||old_w!=s->width||old_h!=s->height){gauges(s);hud(s);s->last_ui_time=s->time;old_w=s->width;old_h=s->height;}}
static void overlay_texture(GLuint tex,float x,float y,float w,float h){glBindTexture(GL_TEXTURE_2D,tex);glBegin(GL_QUADS);glTexCoord2f(0,0);glVertex2f(x,y);glTexCoord2f(1,0);glVertex2f(x+w,y);glTexCoord2f(1,1);glVertex2f(x+w,y+h);glTexCoord2f(0,1);glVertex2f(x,y+h);glEnd();}
/* Custom neon reticle, drawn into the framebuffer itself rather than relying
 * on a host cursor. There is no OS cursor at all in --headless mode (no X,
 * no window — raw VNC clients with no cursor-shape-update support would show
 * nothing), and this stays visually consistent with --windowed too. Widens
 * and brightens to the active underglow color on a hovered dashboard button. */
static void draw_cursor(Server *s){
 if(s->middle_look)return; /* mid-drag means "looking around", not pointing at anything */
 float r,g,b;scene_neon_color(s,&r,&g,&b);
 bool hot=s->hover_button>=0;float cx=s->mouse_x,cy=s->mouse_y,ring=hot?11.f:8.f;
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
 glDisable(GL_DEPTH_TEST);glDisable(GL_LIGHTING);glMatrixMode(GL_PROJECTION);glPushMatrix();glLoadIdentity();glOrtho(0,s->width,s->height,0,-1,1);glMatrixMode(GL_MODELVIEW);glPushMatrix();glLoadIdentity();glEnable(GL_BLEND);glBlendFunc(GL_ONE,GL_ONE_MINUS_SRC_ALPHA);glEnable(GL_TEXTURE_2D);glColor4f(1,1,1,1);if(s->hud_visible)overlay_texture(s->hud_tex,0,0,s->width,s->height);
 if(s->show_help){glDisable(GL_TEXTURE_2D);glColor4f(0,0,0,.50);glBegin(GL_QUADS);glVertex2f(0,0);glVertex2f(s->width,0);glVertex2f(s->width,s->height);glVertex2f(0,s->height);glEnd();glEnable(GL_TEXTURE_2D);glColor4f(1,1,1,1);float scale=fminf((s->width-90)/1000.f,(s->height-120)/720.f);scale=fminf(scale,.9);overlay_texture(s->help_tex,(s->width-1000*scale)/2,(s->height-720*scale)/2,1000*scale,720*scale);}
 draw_cursor(s);
 glDisable(GL_TEXTURE_2D);glDisable(GL_BLEND);glPopMatrix();glMatrixMode(GL_PROJECTION);glPopMatrix();glMatrixMode(GL_MODELVIEW);glEnable(GL_DEPTH_TEST);
}
