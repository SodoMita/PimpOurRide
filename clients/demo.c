/* Small, independent native Wayland clients for the cockpit's launch buttons.
 * Files is read-only, host Meters reads /proc, Audio is a UI demo, and Cluster
 * renders the compositor's shared EV state inside the instrument binnacle.
 * Every mode submits real double-buffered wl_shm surfaces, not captured images. */
#define _GNU_SOURCE
#include <wayland-client.h>
#include "xdg-shell-client.h"
#include "i18n.h"
#include "telemetry.h"
#include "wheel_input.h"
#include <cairo/cairo.h>
#include <xkbcommon/xkbcommon.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#define PI 3.14159265358979323846
#define MAX_FILES 256
static volatile sig_atomic_t stop;
typedef struct {struct wl_buffer *wl;void *pixels;size_t bytes;bool busy;int w,h;} Buffer;
typedef struct {char name[256];bool directory;off_t size;} FileEntry;
typedef struct {
 struct wl_display *display;struct wl_registry *registry;struct wl_compositor *compositor;struct wl_shm *shm;struct xdg_wm_base *wm;struct wl_surface *surface;struct xdg_surface *xdg;struct xdg_toplevel *top;struct wl_seat *seat;struct wl_pointer *pointer;struct wl_keyboard *keyboard;
 struct xkb_context *xkb_context;struct xkb_keymap *keymap;struct xkb_state *xkb_state;
 Buffer buffers[2];int w,h,mode;bool configured,frame_ready,dirty,running;
 float px,py;double started,last_draw,last_metric,progress;bool playing;int track;
 char directory[4096],workspace[4096],preview_name[256],preview[16000];bool preview_open;
 FileEntry files[MAX_FILES];int file_count,scroll;
 /* Continuous pointer-axis input is fractional. Keep the sub-step remainder
  * instead of turning every tiny analog event into a three-row binary jump. */
 double scroll_axis;uint32_t axis_source;
 double cpu,mem_used,mem_total,cpu_history[80];int history_head;unsigned long long last_total,last_idle;
 CdTelemetry *telemetry;
 uint64_t frames;
} App;
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
static void sig(int n){stop=1;}
static void rgb(cairo_t *c,double r,double g,double b){cairo_set_source_rgb(c,r,g,b);}
static void rgba(cairo_t *c,double r,double g,double b,double a){cairo_set_source_rgba(c,r,g,b,a);}
static void font(cairo_t *c,int size,bool bold,bool mono){cairo_select_font_face(c,mono?"DejaVu Sans Mono":"DejaVu Sans",CAIRO_FONT_SLANT_NORMAL,bold?CAIRO_FONT_WEIGHT_BOLD:CAIRO_FONT_WEIGHT_NORMAL);cairo_set_font_size(c,size);}
static void text(cairo_t *c,int x,int y,int size,const char *str,bool bold){font(c,size,bold,false);cairo_move_to(c,x,y);cairo_show_text(c,str);}
static void mono(cairo_t *c,int x,int y,int size,const char *str){font(c,size,false,true);cairo_move_to(c,x,y);cairo_show_text(c,str);}
static void rr(cairo_t *c,double x,double y,double w,double h,double r){cairo_new_sub_path(c);cairo_arc(c,x+w-r,y+r,r,-PI/2,0);cairo_arc(c,x+w-r,y+h-r,r,0,PI/2);cairo_arc(c,x+r,y+h-r,r,PI/2,PI);cairo_arc(c,x+r,y+r,r,PI,1.5*PI);cairo_close_path(c);}
static void line(cairo_t *c,double x,double y,double xx,double yy){cairo_move_to(c,x,y);cairo_line_to(c,xx,yy);cairo_stroke(c);}
static void folder(cairo_t *c,double x,double y,double size){
 cairo_save(c);cairo_translate(c,x,y);cairo_scale(c,size/62,size/62);rgb(c,.37,.27,.55);cairo_move_to(c,0,10);cairo_line_to(c,23,10);cairo_line_to(c,30,18);cairo_line_to(c,61,18);cairo_line_to(c,61,52);cairo_line_to(c,0,52);cairo_close_path(c);cairo_fill(c);
 cairo_pattern_t *g=cairo_pattern_create_linear(0,20,0,57);cairo_pattern_add_color_stop_rgb(g,0,.69,.53,.91);cairo_pattern_add_color_stop_rgb(g,1,.37,.27,.56);cairo_set_source(c,g);rr(c,0,23,63,34,5);cairo_fill(c);cairo_pattern_destroy(g);cairo_restore(c);
}
static int compare_files(const void *a,const void *b){const FileEntry *aa=a,*bb=b;if(aa->directory!=bb->directory)return bb->directory-aa->directory;return strcasecmp(aa->name,bb->name);}
static void read_dir(App *a){a->file_count=0;a->scroll=0;a->scroll_axis=0;a->preview_open=false;DIR *dir=opendir(a->directory);if(!dir){snprintf(a->preview,sizeof(a->preview),tr("Cannot open directory: %s"),strerror(errno));a->preview_open=true;a->dirty=true;return;}struct dirent *de;
 while((de=readdir(dir))&&a->file_count<MAX_FILES){if(de->d_name[0]=='.')continue;char path[4352];snprintf(path,sizeof(path),"%s/%s",a->directory,de->d_name);struct stat st;if(stat(path,&st))continue;FileEntry *e=&a->files[a->file_count++];snprintf(e->name,sizeof(e->name),"%s",de->d_name);e->directory=S_ISDIR(st.st_mode);e->size=st.st_size;}
 closedir(dir);qsort(a->files,a->file_count,sizeof(FileEntry),compare_files);a->dirty=true;
}
static void go_up(App *a){if(a->preview_open){a->preview_open=false;a->dirty=true;return;}char *slash=strrchr(a->directory,'/');if(slash&&slash!=a->directory)*slash=0;else strcpy(a->directory,"/");read_dir(a);}
static void open_file(App *a,int index){if(index<0||index>=a->file_count)return;FileEntry *e=&a->files[index];char path[4352];snprintf(path,sizeof(path),"%s/%s",a->directory,e->name);if(e->directory){char resolved[4096];if(realpath(path,resolved)){snprintf(a->directory,sizeof(a->directory),"%s",resolved);read_dir(a);}return;}
 snprintf(a->preview_name,sizeof(a->preview_name),"%s",e->name);FILE *f=fopen(path,"rb");if(!f){snprintf(a->preview,sizeof(a->preview),tr("Cannot read: %s"),strerror(errno));}else{size_t n=fread(a->preview,1,sizeof(a->preview)-1,f);a->preview[n]=0;fclose(f);bool binary=false;for(size_t i=0;i<n;i++)if(a->preview[i]==0){binary=true;break;}if(binary)snprintf(a->preview,sizeof(a->preview),"Binary file — preview unavailable.\n\n%s\n%lld bytes\n\nThis browser is intentionally read-only.",e->name,(long long)e->size);else for(size_t i=0;i<n;i++)if((unsigned char)a->preview[i]>=128)a->preview[i]='?';}
 a->preview_open=true;a->dirty=true;
}
static void files_draw(App *a,cairo_t *c){
 rgb(c,.033,.047,.069);cairo_paint(c);rgb(c,.046,.063,.088);cairo_rectangle(c,0,0,205,625);cairo_fill(c);
 rgb(c,.74,.64,.93);text(c,28,53,21,tr("CD FILES"),true);rgb(c,.31,.39,.49);mono(c,30,89,11,tr("YOUR LOCAL WORKSPACE"));
 const char *nav[]={tr("Workspace"),tr("Home"),tr("Up a level")};for(int i=0;i<3;i++){if(!i){rgb(c,.11,.10,.19);rr(c,17,136+i*51,170,41,9);cairo_fill(c);}rgb(c,i?.48:.78,i?.57:.71,i?.68:.93);text(c,44,163+i*51,17,nav[i],!i);}
 rgb(c,.23,.30,.39);cairo_set_line_width(c,1);line(c,27,326,178,326);rgb(c,.36,.45,.55);mono(c,29,366,12,tr("READ ONLY"));text(c,29,395,13,tr("Click folders to explore."),false);text(c,29,420,13,tr("Click files to preview."),false);mono(c,28,583,10,tr("REAL FILES / NATIVE SHM"));
 rgb(c,.89,.93,.97);text(c,239,62,31,a->preview_open?tr("File preview"):tr("Your garage."),true);rgb(c,.40,.50,.61);char path[96];size_t len=strlen(a->directory);char tail[64];i18n_utf8_truncate(tail,sizeof(tail),len>57?a->directory+len-57:a->directory,57);snprintf(path,sizeof(path),"%s%s",len>57?"...":"",tail);mono(c,241,105,14,path);
 rgb(c,.14,.19,.25);rr(c,889,26,79,43,10);cairo_fill(c);rgb(c,.65,.75,.83);text(c,908,55,18,tr("↑  Up"),false);
 rgba(c,.24,.32,.42,.55);line(c,239,132,968,132);
 if(a->preview_open){rgb(c,.56,.82,.77);text(c,242,177,18,a->preview_name,true);rgb(c,.66,.74,.82);char *copy=strdup(a->preview),*save,*ln=strtok_r(copy,"\n",&save);int y=214;while(ln&&y<581){char cut[84];i18n_utf8_truncate(cut,sizeof(cut),ln,79);mono(c,242,y,13,cut);y+=20;ln=strtok_r(NULL,"\n",&save);}free(copy);return;}
 char count[64];snprintf(count,sizeof(count),tr("%d ITEMS"),a->file_count);rgb(c,.44,.55,.65);mono(c,241,163,12,count);
 for(int n=0;n<9;n++){int i=a->scroll+n;if(i>=a->file_count)break;int x=240+(n%3)*246,y=183+(n/3)*124;FileEntry *e=&a->files[i];
  rgb(c,.052,.073,.102);rr(c,x,y,230,112,10);cairo_fill(c);rgba(c,.26,.32,.43,.45);rr(c,x+.5,y+.5,229,111,10);cairo_set_line_width(c,1);cairo_stroke(c);
  if(e->directory)folder(c,x+17,y+6,42);else{rgb(c,.26,.40,.48);rr(c,x+21,y+15,25,33,3);cairo_fill(c);rgb(c,.59,.81,.83);line(c,x+27,y+26,x+41,y+26);line(c,x+27,y+32,x+39,y+32);}
  char name[25];i18n_utf8_truncate(name,sizeof(name),e->name,22);rgb(c,.78,.84,.90);text(c,x+18,y+75,16,name,true);rgb(c,.36,.48,.58);char info[40];if(e->directory)strcpy(info,tr("DIRECTORY"));else snprintf(info,sizeof(info),"%.1f KB",(double)e->size/1024);mono(c,x+18,y+97,10,info);
 }
 rgb(c,.35,.45,.55);mono(c,241,599,11,tr("SCROLL TO EXPLORE  /  ESC TO GO BACK"));
}
static const char *tracks[]={"Midnight Circuit","After Hours","Neon Interchange","Backseat Radio"};
static void audio_draw(App *a,cairo_t *c,double t){
 rgb(c,.031,.047,.067);cairo_paint(c);rgb(c,.39,.87,.75);mono(c,42,42,12,tr("CD AUDIO / VISUAL PLAYER DEMO"));rgb(c,.91,.94,.97);text(c,40,96,35,tr("After hours."),true);rgb(c,.37,.49,.58);text(c,766,88,14,tr("SESSION  /  001"),false);
 int x=43,y=141,cover=326;cairo_pattern_t *grad=cairo_pattern_create_linear(x,y,x+cover,y+cover);cairo_pattern_add_color_stop_rgb(grad,0,.13,.14,.30);cairo_pattern_add_color_stop_rgb(grad,.55,.19,.09,.33);cairo_pattern_add_color_stop_rgb(grad,1,.042,.065,.12);cairo_set_source(c,grad);rr(c,x,y,cover,cover,13);cairo_fill(c);cairo_pattern_destroy(grad);
 cairo_save(c);rr(c,x,y,cover,cover,13);cairo_clip(c);for(int i=0;i<13;i++){double xx=x+70+i*26;rgba(c,.66,.35,1,.14+i*.015);cairo_set_line_width(c,2+i%3);line(c,xx,y,xx-150,y+cover);}for(int i=0;i<8;i++){rgba(c,.3,.89,.89,.28);line(c,x,y+cover-44+i*12,x+cover,y+cover-90+i*7);}cairo_restore(c);
 rgb(c,.83,.75,.99);mono(c,x+23,y+38,12,tr("CHROMEDRIVE SOUND LAB"));rgb(c,.93,.90,1);text(c,x+20,y+169,37,"MIDNIGHT",true);text(c,x+20,y+211,37,"CIRCUIT",true);rgb(c,.64,.60,.78);mono(c,x+24,y+293,12,tr("VOL. 01  /  NO AUDIO"));
 rgb(c,.36,.79,.68);mono(c,419,173,12,tr("NOW VISUALIZING"));rgb(c,.86,.91,.95);text(c,416,228,31,tracks[a->track],true);rgb(c,.42,.54,.63);text(c,419,265,19,tr("ChromeDrive Sound Lab"),false);
 for(int i=0;i<38;i++){double height=a->playing?8+27*(.5+.5*sin(t*3+i*.64))*(.5+.5*sin(t*1.3+i*.91)):6;rgb(c,.30+i*.006,.63+i*.004,.61+i*.006);rr(c,422+i*12,327-height,5,height,2);cairo_fill(c);}
 rgb(c,.14,.23,.28);rr(c,420,354,484,4,2);cairo_fill(c);rgb(c,.39,.88,.75);rr(c,420,354,484*a->progress/195,4,2);cairo_fill(c);rgb(c,.44,.55,.63);char time[20];snprintf(time,sizeof(time),"%02d:%02d",(int)a->progress/60,(int)a->progress%60);mono(c,420,385,12,time);mono(c,867,385,12,"03:15");
 rgb(c,.41,.89,.75);cairo_arc(c,662,431,28,0,2*PI);cairo_fill(c);rgb(c,.03,.08,.09);if(a->playing){cairo_rectangle(c,652,421,6,20);cairo_rectangle(c,665,421,6,20);cairo_fill(c);}else{cairo_move_to(c,655,419);cairo_line_to(c,655,443);cairo_line_to(c,676,431);cairo_close_path(c);cairo_fill(c);}
 rgb(c,.60,.70,.77);cairo_set_line_width(c,2);line(c,588,422,588,440);cairo_move_to(c,608,421);cairo_line_to(c,592,431);cairo_line_to(c,608,441);cairo_close_path(c);cairo_fill(c);line(c,737,422,737,440);cairo_move_to(c,718,421);cairo_line_to(c,734,431);cairo_line_to(c,718,441);cairo_close_path(c);cairo_fill(c);
 rgba(c,.26,.34,.43,.5);line(c,43,502,949,502);rgb(c,.43,.52,.64);mono(c,45,537,11,tr("UP NEXT"));rgb(c,.77,.83,.90);text(c,46,573,17,tracks[(a->track+1)%4],true);rgb(c,.35,.47,.57);text(c,46,600,12,tr("Interactive controls + animation. No audio engine is bundled."),false);mono(c,820,574,13,"CD / 002");
}
static void read_metrics(App *a){
 FILE *f=fopen("/proc/stat","r");if(f){char name[10];unsigned long long user,nice,system,idle,iowait,irq,softirq,steal;if(fscanf(f,"%9s %llu %llu %llu %llu %llu %llu %llu %llu",name,&user,&nice,&system,&idle,&iowait,&irq,&softirq,&steal)==9){unsigned long long total=user+nice+system+idle+iowait+irq+softirq+steal,idle_total=idle+iowait;if(a->last_total&&total>a->last_total)a->cpu=100.0*(1-(double)(idle_total-a->last_idle)/(total-a->last_total));a->last_total=total;a->last_idle=idle_total;}fclose(f);}
 f=fopen("/proc/meminfo","r");if(f){char buf[256];unsigned long total=0,avail=0;while(fgets(buf,sizeof(buf),f)){if(sscanf(buf,"MemTotal: %lu kB",&total)==1)continue;if(sscanf(buf,"MemAvailable: %lu kB",&avail)==1)continue;}fclose(f);a->mem_total=total/1048576.0;a->mem_used=(total-avail)/1048576.0;}
 a->cpu_history[a->history_head++%80]=a->cpu;
}
static void meters_draw(App *a,cairo_t *c,double t){
 rgb(c,.034,.049,.070);cairo_paint(c);rgb(c,.40,.89,.76);mono(c,43,44,12,tr("CD METERS / LIVE HOST TELEMETRY"));rgb(c,.90,.94,.97);text(c,40,103,36,tr("Engine room."),true);rgb(c,.40,.52,.64);text(c,43,139,16,tr("Live readings from /proc — not simulated vehicle data."),false);
 for(int i=0;i<3;i++){int x=43+i*315;rgb(c,.053,.076,.103);rr(c,x,174,288,132,12);cairo_fill(c);rgb(c,.44,.57,.65);mono(c,x+20,208,12,i==0?tr("CPU UTILIZATION"):i==1?tr("MEMORY IN USE"):tr("LOGICAL CORES"));char val[40];if(i==0)snprintf(val,sizeof(val),"%.1f%%",a->cpu);else if(i==1)snprintf(val,sizeof(val),"%.1f GB",a->mem_used);else snprintf(val,sizeof(val),"%ld",sysconf(_SC_NPROCESSORS_ONLN));rgb(c,.74,.93,.87);text(c,x+20,269,39,val,true);}
 rgb(c,.055,.076,.106);rr(c,43,331,918,220,12);cairo_fill(c);rgb(c,.45,.58,.67);mono(c,64,365,12,tr("CPU HISTORY / 40 SECONDS"));cairo_set_line_width(c,1);for(int i=0;i<4;i++){rgba(c,.30,.42,.51,.25);line(c,65,396+i*43,939,396+i*43);}rgb(c,.40,.90,.76);cairo_set_line_width(c,2.5);for(int i=0;i<80;i++){int index=(a->history_head+i)%80;double x=65+i*874.0/79,y=531-a->cpu_history[index]*1.33;if(i==0)cairo_move_to(c,x,y);else cairo_line_to(c,x,y);}cairo_stroke(c);rgb(c,.34,.48,.59);mono(c,45,593,11,"LINUX / PROCFS / READ ONLY");char up[60];snprintf(up,sizeof(up),"SESSION UPTIME  %02d:%02d",(int)t/60,(int)t%60);mono(c,704,593,11,up);
}
static void cluster_gauge(cairo_t *c,double cx,double cy,double r,double value,const char *label,const char *unit,bool power){
 rgba(c,.26,.31,.42,.35);cairo_set_line_width(c,12);cairo_arc(c,cx,cy,r,.73*PI,2.27*PI);cairo_stroke(c);
 double fraction=power?fmin(fmax((value+60)/420,0),1):fmin(fmax(value/240,0),1);
 rgb(c,power?.67:.30,power?.34:.89,power?.98:.78);cairo_set_line_width(c,7);cairo_arc(c,cx,cy,r,.73*PI,.73*PI+1.54*PI*fraction);cairo_stroke(c);
 for(int i=0;i<=40;i++){double a=.73*PI+i*1.54*PI/40;rgb(c,i%5==0?.68:.27,i%5==0?.74:.33,i%5==0?.80:.40);cairo_set_line_width(c,i%5==0?2.4:1);line(c,cx+(r-14)*cos(a),cy+(r-14)*sin(a),cx+(r-(i%5==0?29:21))*cos(a),cy+(r-(i%5==0?29:21))*sin(a));}
 char text_value[32];if(power)snprintf(text_value,sizeof(text_value),"%+.0f",value);else snprintf(text_value,sizeof(text_value),"%.0f",value);
 rgb(c,.90,.94,.98);font(c,power?57:68,true,false);cairo_text_extents_t ex;cairo_text_extents(c,text_value,&ex);cairo_move_to(c,cx-ex.width/2-ex.x_bearing,cy+22);cairo_show_text(c,text_value);
 rgb(c,.43,.57,.65);font(c,13,false,true);cairo_text_extents(c,unit,&ex);cairo_move_to(c,cx-ex.width/2,cy+53);cairo_show_text(c,unit);
 rgb(c,.40,.51,.60);font(c,11,false,true);cairo_text_extents(c,label,&ex);cairo_move_to(c,cx-ex.width/2,cy+r+23);cairo_show_text(c,label);
}
static void cluster_draw(App *a,cairo_t *c){
 rgb(c,.010,.018,.030);cairo_paint(c);rgba(c,.18,.25,.34,.45);cairo_set_line_width(c,1);rr(c,2,2,996,400,18);cairo_stroke(c);
 CdTelemetry t;bool online=cd_telemetry_snapshot(a->telemetry,&t);
 if(!online){rgb(c,.65,.74,.82);text(c,290,190,26,tr("TELEMETRY LINK OFFLINE"),true);rgb(c,.40,.51,.60);mono(c,285,232,14,tr("WAITING FOR COMPOSITOR SHARED STATE"));return;}
 rgb(c,.40,.89,.76);mono(c,25,28,11,tr("EV-01 / INSTRUMENT CLUSTER"));rgb(c,.34,.45,.55);mono(c,738,28,11,"NATIVE WAYLAND");
 cluster_gauge(c,205,202,137,fabs(t.speed_mps)*3.6,tr("GROUND SPEED"),tr("KM/H"),false);
 cluster_gauge(c,795,202,137,t.power_kw,tr("800 V DRIVE / REGEN"),tr("KW"),true);
 char gear[2]={(char)t.gear,0};rgb(c,.38,.90,.75);font(c,62,true,false);cairo_text_extents_t ex;cairo_text_extents(c,gear,&ex);cairo_move_to(c,500-ex.width/2,179);cairo_show_text(c,gear);
 const char *mode=t.drive_mode?(t.braking?tr("REGEN BRAKE"):tr("HOVER DRIVE")):tr("CLIENT / PARKED");rgb(c,.48,.59,.68);font(c,12,false,true);cairo_text_extents(c,mode,&ex);cairo_move_to(c,500-ex.width/2,210);cairo_show_text(c,mode);
 double pack=t.battery_capacity_kwh>0?t.battery_kwh/t.battery_capacity_kwh:0;char value[80];snprintf(value,sizeof(value),tr("%.0f%% / %.1f KWH"),pack*100,t.battery_kwh);rgb(c,.78,.86,.91);font(c,17,true,true);cairo_text_extents(c,value,&ex);cairo_move_to(c,500-ex.width/2,253);cairo_show_text(c,value);
 rgba(c,.20,.28,.36,.6);rr(c,413,270,174,7,3);cairo_fill(c);rgb(c,.34,.87,.72);rr(c,413,270,174*fmin(fmax(pack,0),1),7,3);cairo_fill(c);
 snprintf(value,sizeof(value),tr("MOTOR %.0f C"),t.motor_temp_c);rgb(c,.43,.55,.63);mono(c,394,310,12,value);snprintf(value,sizeof(value),tr("ODO %.1f KM"),t.distance_m/1000);mono(c,394,333,12,value);
 snprintf(value,sizeof(value),tr("%02d APPS"),t.connected_screens);mono(c,394,365,11,value);rgb(c,.34,.87,.72);cairo_arc(c,386,360,3,0,2*PI);cairo_fill(c);
}

static void release_buffer(void *data,struct wl_buffer *wl){((Buffer*)data)->busy=false;}
static const struct wl_buffer_listener buffer_listener={.release=release_buffer};
static bool make_buffer(App *a,Buffer *b){
 if(b->wl){wl_buffer_destroy(b->wl);munmap(b->pixels,b->bytes);}memset(b,0,sizeof(*b));b->w=a->w;b->h=a->h;b->bytes=(size_t)a->w*a->h*4;
 int fd=memfd_create("chromedrive-demo",MFD_CLOEXEC);if(fd<0||ftruncate(fd,b->bytes)<0){if(fd>=0)close(fd);return false;}
 b->pixels=mmap(NULL,b->bytes,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);if(b->pixels==MAP_FAILED){close(fd);return false;}struct wl_shm_pool *pool=wl_shm_create_pool(a->shm,fd,b->bytes);b->wl=wl_shm_pool_create_buffer(pool,0,a->w,a->h,a->w*4,WL_SHM_FORMAT_ARGB8888);wl_shm_pool_destroy(pool);close(fd);wl_buffer_add_listener(b->wl,&buffer_listener,b);return true;
}
static void frame_done(void *data,struct wl_callback *cb,uint32_t time){
 /* Why we destroy cb and flip frame_ready here:
  * wl_surface.frame callbacks are one-shot resources fired by the compositor
  * in server_frame_done() only AFTER the previous frame was composited into the
  * 3D cockpit and presented. Waiting for frame_done before issuing the next
  * draw() provides natural compositor-driven backpressure so the client never
  * renders faster than the compositor displays frames. */
 App *a=data;(void)time;wl_callback_destroy(cb);a->frame_ready=true;
}
static const struct wl_callback_listener frame_listener={.done=frame_done};
static void draw(App *a,double t){
 /* Double-buffered SHM submission sequence:
  * 1. Pick an idle wl_shm buffer (released by the compositor via wl_buffer.release
  *    as soon as surface_commit uploaded its pixels into the GL texture).
  * 2. Rasterize with Cairo into the mmap'd memfd region and flush.
  * 3. Stage wl_surface.attach + wl_surface.damage_buffer + wl_surface.frame,
  *    then finalize atomically with wl_surface.commit and mark frame_ready=false
  *    until the compositor's frame_done event arrives. */
 Buffer *b=NULL;for(int i=0;i<2;i++)if(!a->buffers[i].busy){b=&a->buffers[i];break;}if(!b)return;if(!b->wl||b->w!=a->w||b->h!=a->h)if(!make_buffer(a,b)){a->running=false;return;}
 cairo_surface_t *surface=cairo_image_surface_create_for_data(b->pixels,CAIRO_FORMAT_ARGB32,a->w,a->h,a->w*4);cairo_t *c=cairo_create(surface);cairo_scale(c,a->w/1000.0,a->h/(a->mode==3?404.0:625.0));
 if(a->mode==0)files_draw(a,c);else if(a->mode==1)audio_draw(a,c,t);else if(a->mode==2)meters_draw(a,c,t);else cluster_draw(a,c);cairo_destroy(c);cairo_surface_flush(surface);cairo_surface_destroy(surface);
 wl_surface_attach(a->surface,b->wl,0,0);wl_surface_damage_buffer(a->surface,0,0,a->w,a->h);struct wl_callback *cb=wl_surface_frame(a->surface);wl_callback_add_listener(cb,&frame_listener,a);wl_surface_commit(a->surface);b->busy=true;a->frame_ready=false;a->dirty=false;a->frames++;a->last_draw=now();
}
static void wm_ping(void *data,struct xdg_wm_base *wm,uint32_t serial){xdg_wm_base_pong(wm,serial);}
static const struct xdg_wm_base_listener wm_listener={.ping=wm_ping};
static void xdg_configure(void *data,struct xdg_surface *xdg,uint32_t serial){App *a=data;xdg_surface_ack_configure(xdg,serial);a->configured=true;a->dirty=true;}
static const struct xdg_surface_listener xdg_listener={.configure=xdg_configure};
static void top_configure(void *data,struct xdg_toplevel *top,int32_t w,int32_t h,struct wl_array *states){App *a=data;if(w>0&&h>0){a->w=w;a->h=h;}}
static void top_close(void *data,struct xdg_toplevel *top){((App*)data)->running=false;}
static const struct xdg_toplevel_listener top_listener={.configure=top_configure,.close=top_close};
static void pointer_enter(void *data,struct wl_pointer *p,uint32_t serial,struct wl_surface *s,wl_fixed_t x,wl_fixed_t y){App *a=data;a->px=wl_fixed_to_double(x)*1000/a->w;a->py=wl_fixed_to_double(y)*625/a->h;}
static void pointer_leave(void *data,struct wl_pointer *p,uint32_t serial,struct wl_surface *s){}
static void pointer_motion(void *data,struct wl_pointer *p,uint32_t time,wl_fixed_t x,wl_fixed_t y){App *a=data;a->px=wl_fixed_to_double(x)*1000/a->w;a->py=wl_fixed_to_double(y)*625/a->h;}
static void pointer_button(void *data,struct wl_pointer *p,uint32_t serial,uint32_t time,uint32_t button,uint32_t state){
 App *a=data;if(state!=WL_POINTER_BUTTON_STATE_PRESSED||button!=0x110)return;float x=a->px,y=a->py;
 if(a->mode==0){if(x>889&&y<80){go_up(a);return;}if(x<205){if(y>136&&y<177){snprintf(a->directory,sizeof(a->directory),"%s",a->workspace);read_dir(a);}else if(y>187&&y<228){snprintf(a->directory,sizeof(a->directory),"%s",getenv("HOME")?getenv("HOME"):"/");read_dir(a);}else if(y>238&&y<279)go_up(a);return;}if(!a->preview_open&&x>=240&&x<978&&y>=183&&y<555){int col=(x-240)/246,row=(y-183)/124;open_file(a,a->scroll+row*3+col);}}
 else if(a->mode==1){if(x>620&&x<701&&y>399&&y<466)a->playing=!a->playing;else if(x>566&&x<618&&y>400&&y<455){a->track=(a->track+3)%4;a->progress=0;}else if(x>705&&x<756&&y>400&&y<455){a->track=(a->track+1)%4;a->progress=0;}else if(x>420&&x<904&&y>339&&y<371)a->progress=(x-420)/484*195;a->dirty=true;}
}
static void pointer_axis(void *data,struct wl_pointer *p,uint32_t time,uint32_t axis,wl_fixed_t value){
 App *a=data;if(a->mode!=0||axis!=WL_POINTER_AXIS_VERTICAL_SCROLL)return;
 double amount=wl_fixed_to_double(value);int old=a->scroll;
 if(a->axis_source==WL_POINTER_AXIS_SOURCE_CONTINUOUS){
  /* Row-based Files drawing quantizes only its own presentation, after the
   * analog events arrive. The shared helper preserves fractional remainder
   * and exact forward/reverse symmetry. */
  int steps=wheel_client_scroll_steps(&a->scroll_axis,amount);
  if(!steps)return;a->scroll+=steps*3;
 }else{a->scroll_axis=0;a->scroll+=amount>0?3:-3;}
 if(a->scroll<0)a->scroll=0;int max=a->file_count>9?((a->file_count-7)/3)*3:0;if(a->scroll>max)a->scroll=max;
 if(a->scroll==0||a->scroll==max)a->scroll_axis=0;
 if(a->scroll!=old)a->dirty=true;
}
static void pointer_frame(void *data,struct wl_pointer *p){}
static void pointer_source(void *data,struct wl_pointer *p,uint32_t source){((App*)data)->axis_source=source;}
static void pointer_stop(void *data,struct wl_pointer *p,uint32_t time,uint32_t axis){}
static void pointer_discrete(void *data,struct wl_pointer *p,uint32_t axis,int32_t discrete){}
static const struct wl_pointer_listener pointer_listener={.enter=pointer_enter,.leave=pointer_leave,.motion=pointer_motion,.button=pointer_button,.axis=pointer_axis,.frame=pointer_frame,.axis_source=pointer_source,.axis_stop=pointer_stop,.axis_discrete=pointer_discrete};
static void keyboard_map(void *data,struct wl_keyboard *k,uint32_t format,int32_t fd,uint32_t size){App *a=data;if(format!=WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1){close(fd);return;}char *map=mmap(NULL,size,PROT_READ,MAP_PRIVATE,fd,0);if(map!=MAP_FAILED){if(a->keymap)xkb_keymap_unref(a->keymap);if(a->xkb_state)xkb_state_unref(a->xkb_state);a->keymap=xkb_keymap_new_from_string(a->xkb_context,map,XKB_KEYMAP_FORMAT_TEXT_V1,0);if(a->keymap)a->xkb_state=xkb_state_new(a->keymap);munmap(map,size);}close(fd);}
static void keyboard_enter(void *data,struct wl_keyboard *k,uint32_t serial,struct wl_surface *s,struct wl_array *keys){}
static void keyboard_leave(void *data,struct wl_keyboard *k,uint32_t serial,struct wl_surface *s){}
static void keyboard_key(void *data,struct wl_keyboard *k,uint32_t serial,uint32_t time,uint32_t key,uint32_t state){App *a=data;if(state!=WL_KEYBOARD_KEY_STATE_PRESSED||!a->xkb_state)return;xkb_keysym_t sym=xkb_state_key_get_one_sym(a->xkb_state,key+8);if(a->mode==0&&(sym==XKB_KEY_Escape||sym==XKB_KEY_BackSpace))go_up(a);if(a->mode==1&&sym==XKB_KEY_space){a->playing=!a->playing;a->dirty=true;}}
static void keyboard_mods(void *data,struct wl_keyboard *k,uint32_t serial,uint32_t depressed,uint32_t latched,uint32_t locked,uint32_t group){App *a=data;if(a->xkb_state)xkb_state_update_mask(a->xkb_state,depressed,latched,locked,0,0,group);}
static void keyboard_repeat(void *data,struct wl_keyboard *k,int32_t rate,int32_t delay){}
static const struct wl_keyboard_listener keyboard_listener={.keymap=keyboard_map,.enter=keyboard_enter,.leave=keyboard_leave,.key=keyboard_key,.modifiers=keyboard_mods,.repeat_info=keyboard_repeat};
static void seat_caps(void *data,struct wl_seat *seat,uint32_t caps){App *a=data;if((caps&WL_SEAT_CAPABILITY_POINTER)&&!a->pointer){a->pointer=wl_seat_get_pointer(seat);wl_pointer_add_listener(a->pointer,&pointer_listener,a);}if((caps&WL_SEAT_CAPABILITY_KEYBOARD)&&!a->keyboard){a->keyboard=wl_seat_get_keyboard(seat);wl_keyboard_add_listener(a->keyboard,&keyboard_listener,a);}}
static void seat_name(void *data,struct wl_seat *seat,const char *name){}
static const struct wl_seat_listener seat_listener={.capabilities=seat_caps,.name=seat_name};
static void registry_global(void *data,struct wl_registry *reg,uint32_t name,const char *interface,uint32_t version){
 App *a=data;if(!strcmp(interface,"wl_compositor"))a->compositor=wl_registry_bind(reg,name,&wl_compositor_interface,version<4?version:4);else if(!strcmp(interface,"wl_shm"))a->shm=wl_registry_bind(reg,name,&wl_shm_interface,1);else if(!strcmp(interface,"xdg_wm_base")){a->wm=wl_registry_bind(reg,name,&xdg_wm_base_interface,version<3?version:3);xdg_wm_base_add_listener(a->wm,&wm_listener,a);}else if(!strcmp(interface,"wl_seat")){a->seat=wl_registry_bind(reg,name,&wl_seat_interface,version<7?version:7);wl_seat_add_listener(a->seat,&seat_listener,a);}
}
static void registry_remove(void *data,struct wl_registry *r,uint32_t name){}
static const struct wl_registry_listener registry_listener={.global=registry_global,.global_remove=registry_remove};
int main(int argc,char **argv){
 App a={.w=1000,.h=625,.running=true,.frame_ready=true,.dirty=true,.playing=true,.progress=24};const char *mode=argc>1?argv[1]:"files";
 if(!strcmp(mode,"audio"))a.mode=1;else if(!strcmp(mode,"telemetry"))a.mode=2;else if(!strcmp(mode,"cluster")){a.mode=3;a.h=404;}else if(strcmp(mode,"files")){fprintf(stderr,"Usage: %s files|audio|telemetry|cluster\n",argv[0]);return 2;}
 /* Catalogs live in <root>/assets/locale; a demo client is launched from the
  * cockpit, so it must speak the same language the compositor does. */
 {char root[4096];if(i18n_root_from_exe(root,sizeof(root))){const char *want=getenv("CHROMEDRIVE_LANG");i18n_init(root,want&&*want?want:"auto");}}
 signal(SIGINT,sig);signal(SIGTERM,sig);
 if(a.mode==3){
  const char *fd_text=getenv("CHROMEDRIVE_TELEMETRY_FD");int fd=fd_text?atoi(fd_text):-1;struct stat st;
  if(fd>=0&&fstat(fd,&st)==0&&(size_t)st.st_size>=sizeof(CdTelemetry)){
   void *map=mmap(NULL,sizeof(CdTelemetry),PROT_READ,MAP_SHARED,fd,0);if(map!=MAP_FAILED)a.telemetry=map;
  }
  if(!a.telemetry)fprintf(stderr,"[client:cluster] no shared EV telemetry; displaying offline state\n");
 }
 a.xkb_context=xkb_context_new(0);if(!getcwd(a.directory,sizeof(a.directory)))strcpy(a.directory,"/");snprintf(a.workspace,sizeof(a.workspace),"%s",a.directory);if(a.mode==0)read_dir(&a);a.started=now();a.last_draw=a.started;
 a.display=wl_display_connect(NULL);if(!a.display){fprintf(stderr,"Connect to a Wayland display: %s\n",strerror(errno));return 1;}a.registry=wl_display_get_registry(a.display);wl_registry_add_listener(a.registry,&registry_listener,&a);wl_display_roundtrip(a.display);
 if(!a.compositor||!a.shm||!a.wm){fprintf(stderr,"Missing wl_compositor, wl_shm or xdg-shell\n");return 1;}
 a.surface=wl_compositor_create_surface(a.compositor);a.xdg=xdg_wm_base_get_xdg_surface(a.wm,a.surface);xdg_surface_add_listener(a.xdg,&xdg_listener,&a);a.top=xdg_surface_get_toplevel(a.xdg);xdg_toplevel_add_listener(a.top,&top_listener,&a);char app_id[100];snprintf(app_id,sizeof(app_id),"chromedrive.%s",mode);xdg_toplevel_set_app_id(a.top,app_id);xdg_toplevel_set_title(a.top,a.mode==0?tr("Files"):a.mode==1?tr("Audio / UI demo"):a.mode==2?tr("Meters"):tr("EV Instrument Cluster"));wl_surface_commit(a.surface);wl_display_roundtrip(a.display);
 double last=now();while(a.running&&!stop){
  while(wl_display_prepare_read(a.display)!=0)if(wl_display_dispatch_pending(a.display)<0)goto done;
  wl_display_flush(a.display);struct pollfd fd={.fd=wl_display_get_fd(a.display),.events=POLLIN};int ret=poll(&fd,1,25);if(ret>0&&(fd.revents&POLLIN)){if(wl_display_read_events(a.display)<0)break;}else wl_display_cancel_read(a.display);if(ret<0&&errno!=EINTR)break;if(wl_display_dispatch_pending(a.display)<0)break;
  double current=now(),dt=current-last;last=current;if(a.mode==1&&a.playing){a.progress+=dt;if(a.progress>195){a.progress=0;a.track=(a.track+1)%4;}}
  if(a.mode==2&&current-a.last_metric>.5){read_metrics(&a);a.last_metric=current;a.dirty=true;}
  if(a.configured&&a.frame_ready&&(a.dirty||(a.mode==1&&current-a.last_draw>.09)||(a.mode==3&&current-a.last_draw>.06)))draw(&a,current-a.started);
 }
 done:
 fprintf(stderr,"[client:%s] %llu frames submitted\n",mode,(unsigned long long)a.frames);
 if(a.telemetry)munmap(a.telemetry,sizeof(CdTelemetry));
 if(a.top)xdg_toplevel_destroy(a.top);if(a.xdg)xdg_surface_destroy(a.xdg);if(a.surface)wl_surface_destroy(a.surface);
 for(int i=0;i<2;i++)if(a.buffers[i].wl){wl_buffer_destroy(a.buffers[i].wl);munmap(a.buffers[i].pixels,a.buffers[i].bytes);}if(a.pointer)wl_pointer_release(a.pointer);if(a.keyboard)wl_keyboard_release(a.keyboard);if(a.seat)wl_seat_release(a.seat);if(a.wm)xdg_wm_base_destroy(a.wm);if(a.shm)wl_shm_destroy(a.shm);if(a.compositor)wl_compositor_destroy(a.compositor);if(a.registry)wl_registry_destroy(a.registry);wl_display_flush(a.display);wl_display_disconnect(a.display);if(a.xkb_state)xkb_state_unref(a.xkb_state);if(a.keymap)xkb_keymap_unref(a.keymap);xkb_context_unref(a.xkb_context);return 0;
}
