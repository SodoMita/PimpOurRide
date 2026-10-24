#include "chrome.h"
#include "cdconfig.h"
#include <errno.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
static volatile sig_atomic_t interrupted;
static void on_signal(int sig){interrupted=1;}
static double clock_now(void){return (double)SDL_GetPerformanceCounter()/SDL_GetPerformanceFrequency();}
void app_notice(Server *s,const char *fmt,...){va_list args;va_start(args,fmt);vsnprintf(s->notice,sizeof(s->notice),fmt,args);va_end(args);s->notice_until=s->time+4.0;}
static void clear_drive_controls(Server *s){s->drive_forward=s->drive_reverse=s->drive_left=s->drive_right=s->drive_brake=false;s->wheel_steer=0;}
void app_release_input(Server *s,uint32_t time){
 /* Drive keys are compositor-local and therefore are not present in the
  * Wayland seat's s->keys[] bitmap. Both sets must be released on focus loss,
  * otherwise switching windows could leave a client key or motor latched. */
 server_release_keys(s,time);clear_drive_controls(s);scene_wheel_cancel(s);
}
/* ---- persistent configuration (chromedrive.conf, edited by the GUI) ----
 * Precedence: built-in defaults < config file < command line. The file is
 * re-read whenever its mtime changes while running (config_poll, 2 Hz), so
 * `bin/chromedrive-config` edits apply live where that is meaningful:
 * fps/fov/neon/hud/background hot-apply; size/backend/VNC-port/demo/socket
 * are startup-only. Invalid values are ignored with a warning - a typo in
 * a hand-edited config must never kill the compositor. See
 * docs/configuration.md for the schema. */
static char config_path[4096];
static time_t config_mtime_seen;
static bool config_enabled=true;
static void apply_config(Server *s,const CdConfig *cfg,bool startup,const char **socket_out){
 if(startup){
  long w=cd_config_get_int(cfg,"display","width",s->width),h=cd_config_get_int(cfg,"display","height",s->height);
  if(w>=640&&w<=7680&&h>=400&&h<=4320){s->width=(int)w;s->height=(int)h;}
  else if(cd_config_has(cfg,"display","width")||cd_config_has(cfg,"display","height"))fprintf(stderr,"[config] ignoring out-of-range display size\n");
  s->fullscreen=cd_config_get_bool(cfg,"display","fullscreen",s->fullscreen);
  s->headless=cd_config_get_bool(cfg,"session","headless",s->headless);
  long port=cd_config_get_int(cfg,"session","vnc_port",s->vnc_port);
  if(port>=1&&port<=65535)s->vnc_port=(int)port;else if(cd_config_has(cfg,"session","vnc_port"))fprintf(stderr,"[config] ignoring bad vnc_port\n");
  s->demo=cd_config_get_bool(cfg,"session","demo",s->demo);
  static char socket_buf[128];
  const char *sock=cd_config_get_string(cfg,"session","socket","");
  if(socket_out&&sock&&*sock){snprintf(socket_buf,sizeof(socket_buf),"%s",sock);*socket_out=socket_buf;}
 }
 s->fps_limit=(int)clampf((float)cd_config_get_int(cfg,"display","fps",s->fps_limit),1,120);
 s->target_fov=clampf((float)cd_config_get_double(cfg,"display","fov",s->target_fov),40,100);
 if(startup)s->fov=s->target_fov;
 long neon=cd_config_get_int(cfg,"cockpit","neon",s->theme);neon=((neon%3)+3)%3;
 if((int)neon!=s->theme){s->theme=(int)neon;s->rebuild_scene=true;}
 s->hud_visible=cd_config_get_bool(cfg,"cockpit","hud",s->hud_visible);
 const char *bg=cd_config_get_string(cfg,"cockpit","background",NULL);
 if(bg){int mode=!strcmp(bg,"texture")?CD_BG_TEXTURE:!strcmp(bg,"none")?CD_BG_NONE:!strcmp(bg,"3d")?CD_BG_3D:-1;
  if(mode>=0)s->bg_mode=mode;else fprintf(stderr,"[config] unknown background '%s' (want 3d|texture|none)\n",bg);}
}
/* ---- Launch Control menu configuration ([app.*]) ----
 * The KDE-menu-editor half of the config file. Each of the four app slots
 * has a section ([app.terminal] ... [app.meters]) with:
 *   name, sub       button label lines (also for [app.neon]/[app.cockpit])
 *   exec            shell command; "%root%" expands to the install root
 *   app_id          substring matched to focus an already-running instance
 *   icon            0..5 (prompt, folder, music, bars, sun, wheel)
 *   enabled         false greys the button out (click = notice)
 *   autostart       join the --demo launch set
 * Everything applies live: relabels regenerate button textures, exec/app_id
 * take effect on the next click. CHROMEDRIVE_* env vars still win for exec
 * (they are an explicit launch-time override - the GUI shows this state). */
static char *expand_root(const char *in,const char *root){
 const char *needle="%root%";size_t nl=strlen(needle),rl=strlen(root),count=0;
 for(const char *p=in;(p=strstr(p,needle))!=NULL;p+=nl)count++;
 char *out=malloc(strlen(in)+count*(rl-nl)+1);if(!out)return NULL;
 char *w=out;const char *r=in,*hit;
 while((hit=strstr(r,needle))!=NULL){size_t pre=(size_t)(hit-r);memcpy(w,r,pre);w+=pre;memcpy(w,root,rl);w+=rl;r=hit+nl;}
 strcpy(w,r);return out;
}
static void apply_app_config(Server *s,const CdConfig *cfg){
 static const char *sections[6]={"app.terminal","app.files","app.audio","app.meters","app.neon","app.cockpit"};
 static const char *envnames[4]={"CHROMEDRIVE_TERMINAL","CHROMEDRIVE_FILES","CHROMEDRIVE_AUDIO","CHROMEDRIVE_METERS"};
 for(int i=0;i<6;i++){
  const char *sec=sections[i];DashButton *b=&s->buttons[i];
  const char *name=cd_config_get_string(cfg,sec,"name",NULL);
  if(name&&*name)snprintf(b->name,sizeof(b->name),"%s",name);
  const char *sub=cd_config_get_string(cfg,sec,"sub",NULL);
  if(sub)snprintf(b->sub,sizeof(b->sub),"%s",sub);
  long icon=cd_config_get_int(cfg,sec,"icon",b->icon);
  if(icon>=0&&icon<=5)b->icon=(int)icon;else if(cd_config_has(cfg,sec,"icon"))fprintf(stderr,"[config] %s icon out of range 0..5\n",sec);
  b->enabled=cd_config_get_bool(cfg,sec,"enabled",b->enabled);
  if(i<4){
   const char *id=cd_config_get_string(cfg,sec,"app_id",NULL);
   if(id)snprintf(s->app_ids[i],sizeof(s->app_ids[i]),"%s",id);
   s->app_autostart[i]=cd_config_get_bool(cfg,sec,"autostart",s->app_autostart[i]);
   const char *exec=cd_config_get_string(cfg,sec,"exec",NULL);
   if(exec&&*exec&&!getenv(envnames[i])){char *x=expand_root(exec,s->root);if(x){free(s->launch_commands[i]);s->launch_commands[i]=x;}}
   else if(exec&&*exec)fprintf(stderr,"[config] %s exec ignored: $%s is set\n",sec,envnames[i]);
  }
 }
 ui_refresh_buttons(s);
}
static void config_load_startup(Server *s,const char **socket_out){
 if(!config_enabled||!*config_path){fprintf(stderr,"[config] disabled\n");return;}
 CdConfig *cfg=cd_config_load(config_path);
 if(cfg){apply_config(s,cfg,true,socket_out);cd_config_free(cfg);}
 config_mtime_seen=cd_config_mtime(config_path);
 fprintf(stderr,"[config] %s (%s)\n",config_path,config_mtime_seen?"loaded":"not present, built-in defaults");
}
static void config_poll(Server *s){
 if(!config_enabled)return;
 static double next_check;double t=clock_now();if(t<next_check)return;next_check=t+0.5;
 time_t m=cd_config_mtime(config_path);
 if(m==config_mtime_seen)return;
 config_mtime_seen=m;
 if(!m)return; /* file deleted: keep current values */
 CdConfig *cfg=cd_config_load(config_path);
 if(!cfg)return;
 apply_config(s,cfg,false,NULL);apply_app_config(s,cfg);cd_config_free(cfg);
 fprintf(stderr,"[config] reloaded %s\n",config_path);
 app_notice(s,"Configuration reloaded");
}
void app_exec(Server *s,const char *command){
 if(!command||!*command)return;
 if(s->child_count>=128){app_notice(s,"Child process limit reached");return;}
 posix_spawnattr_t attr;posix_spawnattr_init(&attr);posix_spawnattr_setflags(&attr,POSIX_SPAWN_SETPGROUP);posix_spawnattr_setpgroup(&attr,0);
 char *args[]={"sh","-lc",(char*)command,NULL};pid_t pid;int err=posix_spawn(&pid,"/bin/sh",NULL,&attr,args,environ);posix_spawnattr_destroy(&attr);
 if(err){app_notice(s,"Launch failed: %s",strerror(err));fprintf(stderr,"[launch] %s: %s\n",command,strerror(err));return;}
 s->children[s->child_count++]=pid;fprintf(stderr,"[launch] pid=%d %s\n",(int)pid,command);app_notice(s,"Connecting a new native Wayland client...");
}
void app_launch(Server *s,int action,bool force_new){
 if(action>=0&&action<6&&!s->buttons[action].enabled){app_notice(s,"%s is disabled in the menu configuration",s->buttons[action].name);return;}
 if(action==4){s->theme=(s->theme+1)%3;s->rebuild_scene=true;app_notice(s,"Underglow / %s",s->theme==0?"ULTRAVIOLET":s->theme==1?"ICE BLUE":"ACID LIME");return;}
 if(action==5){scene_reset_camera(s);app_notice(s,"Driver view restored");return;}
 if(action<0||action>3)return;
 if(!force_new&&*s->app_ids[action]){for(int i=0;i<CD_MAX_MONITORS;i++){Surface *p=s->monitors[i].surface;if(p&&strstr(p->app_id,s->app_ids[action])){server_focus(s,p);scene_focus_monitor(s,i,true);return;}}}
 app_exec(s,s->launch_commands[action]);
}
/* The dash cog: focus the settings GUI if it is already on a monitor,
 * otherwise launch it (it maps to the next free screen like any app). */
void app_open_settings(Server *s){
 s->cog_spin_until=s->time+0.9;
 for(int i=0;i<CD_MAX_MONITORS;i++){Surface *p=s->monitors[i].surface;if(p&&strstr(p->app_id,"chromedrive.config")){server_focus(s,p);scene_focus_monitor(s,i,true);app_notice(s,"Settings");return;}}
 char cmd[4400];snprintf(cmd,sizeof(cmd),"'%s/bin/chromedrive-config'",s->root);
 app_exec(s,cmd);app_notice(s,"Opening Settings...");
}
static void next_monitor(Server *s){for(int d=1;d<=CD_MAX_MONITORS;d++){int i=(s->active_slot+d)%CD_MAX_MONITORS;if(s->monitors[i].surface){server_focus(s,s->monitors[i].surface);scene_focus_monitor(s,i,s->focus_view);return;}}}
/* Compositor-level hotkeys, shared by both input backends (SDL and headless/VNC).
 * Keyed on evdev codes + the live s->keys[] modifier bitmap (already updated by
 * server_key() for prior events in the same stream), so it works identically
 * whether the raw event came from an SDL_Keycode+scancode or an X11 keysym. */
static bool drive_key(Server *s,uint32_t key,bool down){
 if(s->steering_mode!=VEHICLE_STEER_DRIVE)return false;
 bool *held=NULL;
 if(key==KEY_W||key==KEY_UP)held=&s->drive_forward;
 else if(key==KEY_S||key==KEY_DOWN)held=&s->drive_reverse;
 else if(key==KEY_A||key==KEY_LEFT)held=&s->drive_left;
 else if(key==KEY_D||key==KEY_RIGHT)held=&s->drive_right;
 else if(key==KEY_SPACE)held=&s->drive_brake;
 if(!held)return false;
 *held=down;
 return true; /* consumed locally: never leak motor controls into a Wayland app */
}
bool app_global_key(Server *s,uint32_t evdev,bool down){
 bool ctrl=s->keys[KEY_LEFTCTRL]||s->keys[KEY_RIGHTCTRL];
 bool alt=s->keys[KEY_LEFTALT]||s->keys[KEY_RIGHTALT];
 if(ctrl&&alt&&(evdev==KEY_ENTER||evdev==KEY_Q)){if(down){if(evdev==KEY_Q)s->running=false;else app_launch(s,0,true);}return true;}
 bool global=evdev==KEY_F1||evdev==KEY_F2||evdev==KEY_F3||evdev==KEY_F4||evdev==KEY_F5||evdev==KEY_F6||evdev==KEY_F7||evdev==KEY_F8||evdev==KEY_F11||evdev==KEY_F12;
 if(!global)return drive_key(s,evdev,down);if(!down)return true;
 switch(evdev){
 case KEY_F1:s->show_help=!s->show_help;break;
 case KEY_F2:scene_reset_camera(s);break;
 case KEY_F3:if(s->keyboard_focus)scene_focus_monitor(s,s->keyboard_focus->slot,!s->focus_view);else next_monitor(s);break;
 case KEY_F4:app_launch(s,4,false);break;
 case KEY_F5:
  s->camera_mode=(VehicleCameraMode)((s->camera_mode+1)%3);
  scene_wheel_cancel(s);
  app_notice(s,"%s",s->camera_mode==VEHICLE_CAMERA_DRIVER?"Driver camera":s->camera_mode==VEHICLE_CAMERA_CHASE?"Exterior concept camera":"EV systems cutaway");
  break;
 case KEY_F6:next_monitor(s);break;
 case KEY_F7:
  /* Release app keys before changing ownership. Otherwise a key held during
   * the transition could remain pressed in the client even after its release
   * starts being consumed as a vehicle control. */
  app_release_input(s,SDL_GetTicks());
  s->steering_mode=s->steering_mode==VEHICLE_STEER_DRIVE?VEHICLE_STEER_CLIENT:VEHICLE_STEER_DRIVE;
  app_notice(s,s->steering_mode==VEHICLE_STEER_DRIVE?"DRIVE / WASD + wheel; F7 returns input to apps":"CLIENT / wheel scrolls the focused Wayland app");
  break;
 case KEY_F8:
  vehicle_reset_pose(&s->vehicle);clear_drive_controls(s);scene_wheel_cancel(s);
  app_notice(s,"Vehicle recovered to the garage start pose");break;
 case KEY_F11:if(!s->headless&&s->window){s->fullscreen=!s->fullscreen;SDL_SetWindowFullscreen(s->window,s->fullscreen?SDL_WINDOW_FULLSCREEN_DESKTOP:0);}break;
 case KEY_F12:snprintf(s->screenshot_path,sizeof(s->screenshot_path),"%.4000s/screenshots/cockpit-%llu.webp",s->root,(unsigned long long)SDL_GetTicks64());s->screenshot_requested=true;break;
 default:break;
 }
 return true;
}
static void usage(const char *name){printf("ChromeDrive " CD_VERSION " — native, first-person Wayland cockpit\n\nUsage: %s [options]\n  --demo                Start a real terminal + Files + Audio clients\n  --socket NAME         Wayland socket (default: automatic)\n  --exec COMMAND        Launch an extra Wayland client (repeatable)\n  --size WIDTHxHEIGHT   Framebuffer/host window size (default 1600x1000)\n  --fps N               Frame cap, 1..120 (default 60)\n  --headless            Run with no X server: surfaceless EGL + built-in VNC server (default)\n  --windowed            Dev/debug mode: open a real SDL2/GLX window on an X server instead\n  --vnc-port N          VNC port for --headless mode (default 5900)\n  --fullscreen          --windowed only: start fullscreen, F11 to leave\n  --exit-after SECONDS   Useful for automated checks\n  --screenshot PATH     Capture a WebP after 3 seconds\n  --config PATH         Config file (default: ~/.config/chromedrive/chromedrive.conf)\n  --no-config           Ignore the config file entirely\n  --help                Show this help\n\nF1 help | F2 cockpit | F3 focus | F4 neon | F5 driver/exterior/systems | F6 next display\nF7 client/drive input | F8 recover car | Drive: WASD/arrows + Space + drag wheel\nMiddle-drag/VNC-drag to look | Click a screen to focus | Ctrl+Alt+Enter new terminal\n",name);}
int main(int argc,char **argv){
 Server s={.width=1600,.height=1000,.fps_limit=60,.running=true,.hud_visible=true,.hover_button=-1,.keymap_fd=-1,.fov=65,.target_fov=65,.eye={0,1.0f,2.8f},.headless=true,.vnc_port=5900};
 vehicle_init(&s.vehicle);
 const char *socket_name=NULL;double exit_after=0;bool auto_screenshot=false;
 /* Config pre-scan: --config/--no-config must be known before the file is
  * read, and the file must be read before the real option pass below so
  * command-line options override file values. */
 if(!cd_config_path(config_path,sizeof(config_path),"chromedrive","chromedrive.conf"))config_enabled=false;
 for(int i=1;i<argc;i++){
  if(!strcmp(argv[i],"--no-config"))config_enabled=false;
  else if(!strcmp(argv[i],"--config")&&i+1<argc){snprintf(config_path,sizeof(config_path),"%s",argv[i+1]);config_enabled=true;}
 }
 config_load_startup(&s,&socket_name);
 for(int i=1;i<argc;i++){
  if(!strcmp(argv[i],"--help")){usage(argv[0]);return 0;}
  else if(!strcmp(argv[i],"--config")&&i+1<argc)i++; /* consumed by the pre-scan */
  else if(!strcmp(argv[i],"--no-config")){/* consumed by the pre-scan */}
  else if(!strcmp(argv[i],"--demo"))s.demo=true;
  else if(!strcmp(argv[i],"--fullscreen"))s.fullscreen=true;
  else if(!strcmp(argv[i],"--headless"))s.headless=true;
  else if(!strcmp(argv[i],"--windowed"))s.headless=false;
  else if(!strcmp(argv[i],"--vnc-port")&&i+1<argc)s.vnc_port=atoi(argv[++i]);
  else if(!strcmp(argv[i],"--socket")&&i+1<argc)socket_name=argv[++i];
  else if(!strcmp(argv[i],"--exec")&&i+1<argc&&s.exec_count<16)s.exec_commands[s.exec_count++]=argv[++i];
  else if(!strcmp(argv[i],"--size")&&i+1<argc){if(sscanf(argv[++i],"%dx%d",&s.width,&s.height)!=2||s.width<640||s.height<400||s.width>7680||s.height>4320){fprintf(stderr,"Invalid window size\n");return 2;}}
  else if(!strcmp(argv[i],"--fps")&&i+1<argc)s.fps_limit=(int)clampf(atoi(argv[++i]),1,120);
  else if(!strcmp(argv[i],"--exit-after")&&i+1<argc)exit_after=atof(argv[++i]);
  else if(!strcmp(argv[i],"--screenshot")&&i+1<argc){snprintf(s.screenshot_path,sizeof(s.screenshot_path),"%s",argv[++i]);auto_screenshot=true;}
  else{fprintf(stderr,"Unknown or incomplete option: %s\n",argv[i]);usage(argv[0]);return 2;}
 }
 s.mouse_x=s.width/2;s.mouse_y=s.height/2; /* start the cursor centered, not pinned to the top-left corner at (0,0) */
 ssize_t n=readlink("/proc/self/exe",s.root,sizeof(s.root)-1);if(n<0)return 1;s.root[n]=0;char *slash=strrchr(s.root,'/');if(slash)*slash=0;slash=strrchr(s.root,'/');if(slash)*slash=0;
 if(!getenv("XDG_RUNTIME_DIR")){snprintf(s.runtime,sizeof(s.runtime),"/tmp/chromedrive-%d",(int)getuid());if(mkdir(s.runtime,0700)<0&&errno!=EEXIST){perror("runtime directory");return 1;}struct stat st;if(stat(s.runtime,&st)<0||st.st_uid!=getuid()||(st.st_mode&0077)){fprintf(stderr,"Runtime directory must be private and owned by you\n");return 1;}setenv("XDG_RUNTIME_DIR",s.runtime,1);}else snprintf(s.runtime,sizeof(s.runtime),"%s",getenv("XDG_RUNTIME_DIR"));
 signal(SIGINT,on_signal);signal(SIGTERM,on_signal);signal(SIGPIPE,SIG_IGN);
 if(SDL_Init(SDL_INIT_TIMER)<0){fprintf(stderr,"SDL: %s\n",SDL_GetError());return 1;} /* timer only: used for clock_now()/frame pacing in both backends */
 bool backend_ok=s.headless?backend_headless_init(&s):backend_sdl_init(&s);
 if(!backend_ok){fprintf(stderr,"Backend init failed (%s)\n",s.headless?"headless/EGL/VNC":"windowed/SDL/GLX");SDL_Quit();return 1;}
 dmabuf_probe(&s);
 if(!server_init(&s,socket_name)){fprintf(stderr,"Could not start Wayland server: %s\n",strerror(errno));server_finish(&s);SDL_Quit();return 1;}
 /* Only after opening the host window/backend do we replace WAYLAND_DISPLAY. */
 /* Built-in menu defaults (%root% templates - the same strings the config
  * GUI shows as the editable starting point) + per-slot app_id/autostart.
  * [app.*] config (applied after ui_init below) overrides; env vars win. */
 const char *templates[4]={"foot --config='%root%/assets/foot.ini' --title='Terminal' /bin/sh '%root%/assets/terminal-welcome.sh'","'%root%/bin/chromedrive-demo' files","'%root%/bin/chromedrive-demo' audio","'%root%/bin/chromedrive-demo' telemetry"};
 const char *def_ids[4]={"foot","chromedrive.files","chromedrive.audio","chromedrive.telemetry"};
 const bool def_autostart[4]={true,true,true,false};
 const char *envnames[]={"CHROMEDRIVE_TERMINAL","CHROMEDRIVE_FILES","CHROMEDRIVE_AUDIO","CHROMEDRIVE_METERS"};
 for(int i=0;i<4;i++){s.launch_commands[i]=getenv(envnames[i])?strdup(getenv(envnames[i])):expand_root(templates[i],s.root);snprintf(s.app_ids[i],sizeof(s.app_ids[i]),"%s",def_ids[i]);s.app_autostart[i]=def_autostart[i];}
 scene_init(&s);ui_init(&s);
 if(config_enabled){CdConfig *cfg=cd_config_load(config_path);if(cfg){apply_app_config(&s,cfg);cd_config_free(cfg);}} /* needs buttons+GL: after ui_init */
 s.start_time=clock_now();double last=s.start_time,fps_start=last;uint64_t fps_frames=0;bool initial_focus=false;
 if(s.demo){for(int i=0;i<4;i++)if(s.buttons[i].enabled&&s.app_autostart[i])app_exec(&s,s.launch_commands[i]);}for(int i=0;i<s.exec_count;i++)app_exec(&s,s.exec_commands[i]);
 app_notice(&s,"Welcome to the driver's seat. F1 for controls.");
 while(s.running&&!interrupted){
  double frame_start=clock_now();s.time=frame_start-s.start_time;s.dt=frame_start-last;last=frame_start;
  if(s.headless)backend_headless_poll(&s);else backend_sdl_poll(&s);
  config_poll(&s);
  server_dispatch(&s);
  int status;pid_t pid;while((pid=waitpid(-1,&status,WNOHANG))>0){for(int i=0;i<s.child_count;i++)if(s.children[i]==pid){s.children[i]=s.children[--s.child_count];break;}if(WIFEXITED(status)&&WEXITSTATUS(status)!=0){fprintf(stderr,"[launch] pid %d exited %d\n",(int)pid,WEXITSTATUS(status));app_notice(&s,"Client exited with status %d — see launch log",WEXITSTATUS(status));}}
  if(s.demo&&!initial_focus&&s.time>2.0&&s.monitors[0].surface){server_focus(&s,s.monitors[0].surface);initial_focus=true;}
  /* Physics consumes a snapshot of the current input mode, then advances in
   * 120 Hz fixed ticks. CLIENT mode applies a parking brake: manipulating a
   * Wayland app must never leave the car accelerating behind the desktop. */
  VehicleInput vehicle_input={.brake=1};
  if(s.steering_mode==VEHICLE_STEER_DRIVE){
   vehicle_input.throttle=(s.drive_forward?1.f:0.f)-(s.drive_reverse?1.f:0.f);
   vehicle_input.steering=s.wheel_drag?s.wheel_steer:(s.drive_right?1.f:0.f)-(s.drive_left?1.f:0.f);
   vehicle_input.brake=s.drive_brake?1.f:0.f;
  }
  vehicle_update(&s.vehicle,vehicle_input,s.dt);
  static bool collision_seen;if(s.vehicle.collision&&!collision_seen)app_notice(&s,"Hover bumper absorbed a garage wall contact");collision_seen=s.vehicle.collision;
  if(!s.wheel_drag)s.wheel_client_angle+=(0-s.wheel_client_angle)*(1.f-expf(-(float)s.dt*9.f));
  float blend=1.f-expf(-(float)s.dt*12.f);s.yaw+=(s.target_yaw-s.yaw)*blend;s.pitch+=(s.target_pitch-s.pitch)*blend;s.fov+=(s.target_fov-s.fov)*blend;
  Hit hover=scene_pick(&s,s.mouse_x,s.mouse_y,false);s.hover_button=hover.kind==HIT_BUTTON?hover.button:-1;s.cog_hover=hover.kind==HIT_COG;s.wheel_hover=hover.kind==HIT_WHEEL;
  s.cog_angle+=((s.time<s.cog_spin_until)?540.f:s.cog_hover?160.f:20.f)*(float)s.dt;if(s.cog_angle>=360.f)s.cog_angle-=360.f;
  /* One thread owns the current GL context for the entire frame: UI texture
   * regeneration, vehicle/cockpit geometry, and Wayland surface textures are
   * rendered into one FBO. No web worker or physics callback touches GL. */
  glBindFramebuffer(GL_FRAMEBUFFER,s.target_fbo);
  ui_update(&s);scene_render(&s);ui_draw_overlay(&s);
  if(auto_screenshot&&s.time>3){s.screenshot_requested=true;auto_screenshot=false;}if(s.screenshot_requested){scene_save_screenshot(&s,s.screenshot_path);s.screenshot_requested=false;app_notice(&s,"Screenshot saved");}
  if(s.headless)backend_headless_present(&s);else backend_sdl_present(&s);
  server_frame_done(&s,(uint32_t)(clock_now()*1000));wl_display_flush_clients(s.display);s.frames++;fps_frames++;
  if(frame_start-fps_start>.5){s.fps=fps_frames/(frame_start-fps_start);fps_frames=0;fps_start=frame_start;}
  if(exit_after>0&&s.time>=exit_after)s.running=false;
  double remain=1.0/s.fps_limit-(clock_now()-frame_start);if(remain>0)SDL_Delay((uint32_t)(remain*1000));
 }
 for(int i=0;i<s.child_count;i++)kill(-s.children[i],SIGTERM);
 server_finish(&s);scene_finish(&s);
 if(s.headless)backend_headless_shutdown(&s);else backend_sdl_shutdown(&s);
 SDL_Quit();for(int i=0;i<4;i++)free(s.launch_commands[i]);return 0;
}
