#include "chrome.h"
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
void app_exec(Server *s,const char *command){
 if(!command||!*command)return;
 if(s->child_count>=128){app_notice(s,"Child process limit reached");return;}
 posix_spawnattr_t attr;posix_spawnattr_init(&attr);posix_spawnattr_setflags(&attr,POSIX_SPAWN_SETPGROUP);posix_spawnattr_setpgroup(&attr,0);
 char *args[]={"sh","-lc",(char*)command,NULL};pid_t pid;int err=posix_spawn(&pid,"/bin/sh",NULL,&attr,args,environ);posix_spawnattr_destroy(&attr);
 if(err){app_notice(s,"Launch failed: %s",strerror(err));fprintf(stderr,"[launch] %s: %s\n",command,strerror(err));return;}
 s->children[s->child_count++]=pid;fprintf(stderr,"[launch] pid=%d %s\n",(int)pid,command);app_notice(s,"Connecting a new native Wayland client...");
}
void app_launch(Server *s,int action,bool force_new){
 if(action==4){s->theme=(s->theme+1)%3;s->rebuild_scene=true;app_notice(s,"Underglow / %s",s->theme==0?"ULTRAVIOLET":s->theme==1?"ICE BLUE":"ACID LIME");return;}
 if(action==5){scene_reset_camera(s);app_notice(s,"Driver view restored");return;}
 if(action<0||action>3)return;
 const char *ids[]={"foot","chromedrive.files","chromedrive.audio","chromedrive.telemetry"};
 if(!force_new){for(int i=0;i<CD_MAX_MONITORS;i++){Surface *p=s->monitors[i].surface;if(p&&strstr(p->app_id,ids[action])){server_focus(s,p);scene_focus_monitor(s,i,true);return;}}}
 app_exec(s,s->launch_commands[action]);
}
static void next_monitor(Server *s){for(int d=1;d<=CD_MAX_MONITORS;d++){int i=(s->active_slot+d)%CD_MAX_MONITORS;if(s->monitors[i].surface){server_focus(s,s->monitors[i].surface);scene_focus_monitor(s,i,s->focus_view);return;}}}
/* Compositor-level hotkeys, shared by both input backends (SDL and headless/VNC).
 * Keyed on evdev codes + the live s->keys[] modifier bitmap (already updated by
 * server_key() for prior events in the same stream), so it works identically
 * whether the raw event came from an SDL_Keycode+scancode or an X11 keysym. */
bool app_global_key(Server *s,uint32_t evdev,bool down){
 bool ctrl=s->keys[KEY_LEFTCTRL]||s->keys[KEY_RIGHTCTRL];
 bool alt=s->keys[KEY_LEFTALT]||s->keys[KEY_RIGHTALT];
 if(ctrl&&alt&&(evdev==KEY_ENTER||evdev==KEY_Q)){if(down){if(evdev==KEY_Q)s->running=false;else app_launch(s,0,true);}return true;}
 bool global=evdev==KEY_F1||evdev==KEY_F2||evdev==KEY_F3||evdev==KEY_F4||evdev==KEY_F6||evdev==KEY_F11||evdev==KEY_F12;
 if(!global)return false;if(!down)return true;
 switch(evdev){
 case KEY_F1:s->show_help=!s->show_help;break;
 case KEY_F2:scene_reset_camera(s);break;
 case KEY_F3:if(s->keyboard_focus)scene_focus_monitor(s,s->keyboard_focus->slot,!s->focus_view);else next_monitor(s);break;
 case KEY_F4:app_launch(s,4,false);break;
 case KEY_F6:next_monitor(s);break;
 case KEY_F11:if(!s->headless&&s->window){s->fullscreen=!s->fullscreen;SDL_SetWindowFullscreen(s->window,s->fullscreen?SDL_WINDOW_FULLSCREEN_DESKTOP:0);}break;
 case KEY_F12:snprintf(s->screenshot_path,sizeof(s->screenshot_path),"%.4000s/screenshots/cockpit-%llu.png",s->root,(unsigned long long)SDL_GetTicks64());s->screenshot_requested=true;break;
 default:break;
 }
 return true;
}
static void usage(const char *name){printf("ChromeDrive " CD_VERSION " — native, first-person Wayland cockpit\n\nUsage: %s [options]\n  --demo                Start a real terminal + Files + Audio clients\n  --socket NAME         Wayland socket (default: automatic)\n  --exec COMMAND        Launch an extra Wayland client (repeatable)\n  --size WIDTHxHEIGHT   Framebuffer/host window size (default 1600x1000)\n  --fps N               Frame cap, 1..120 (default 60)\n  --headless            Run with no X server: surfaceless EGL + built-in VNC server (default)\n  --windowed            Dev/debug mode: open a real SDL2/GLX window on an X server instead\n  --vnc-port N          VNC port for --headless mode (default 5900)\n  --fullscreen          --windowed only: start fullscreen, F11 to leave\n  --exit-after SECONDS   Useful for automated checks\n  --screenshot PATH     Capture a PNG after 3 seconds\n  --help                Show this help\n\nF1 help | F2 cockpit | F3 focus | F4 neon | F6 next display\nMiddle-drag/VNC-drag to look | Click a screen to focus | Ctrl+Alt+Enter new terminal\n",name);}
int main(int argc,char **argv){
 Server s={.width=1600,.height=1000,.fps_limit=60,.running=true,.hud_visible=true,.hover_button=-1,.keymap_fd=-1,.fov=65,.target_fov=65,.eye={0,1.0f,2.8f},.headless=true,.vnc_port=5900};
 const char *socket_name=NULL;double exit_after=0;bool auto_screenshot=false;
 for(int i=1;i<argc;i++){
  if(!strcmp(argv[i],"--help")){usage(argv[0]);return 0;}
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
 char *defaults[4]={0};asprintf(&defaults[0],"foot --config='%s/assets/foot.ini' --title='Terminal' /bin/sh '%s/assets/terminal-welcome.sh'",s.root,s.root);asprintf(&defaults[1],"'%s/bin/chromedrive-demo' files",s.root);asprintf(&defaults[2],"'%s/bin/chromedrive-demo' audio",s.root);asprintf(&defaults[3],"'%s/bin/chromedrive-demo' telemetry",s.root);
 const char *envnames[]={"CHROMEDRIVE_TERMINAL","CHROMEDRIVE_FILES","CHROMEDRIVE_AUDIO","CHROMEDRIVE_METERS"};for(int i=0;i<4;i++)s.launch_commands[i]=getenv(envnames[i])?strdup(getenv(envnames[i])):strdup(defaults[i]);for(int i=0;i<4;i++)free(defaults[i]);
 scene_init(&s);ui_init(&s);s.start_time=clock_now();double last=s.start_time,fps_start=last;uint64_t fps_frames=0;bool initial_focus=false;
 if(s.demo){app_exec(&s,s.launch_commands[0]);app_exec(&s,s.launch_commands[1]);app_exec(&s,s.launch_commands[2]);}for(int i=0;i<s.exec_count;i++)app_exec(&s,s.exec_commands[i]);
 app_notice(&s,"Welcome to the driver's seat. F1 for controls.");
 while(s.running&&!interrupted){
  double frame_start=clock_now();s.time=frame_start-s.start_time;s.dt=frame_start-last;last=frame_start;
  if(s.headless)backend_headless_poll(&s);else backend_sdl_poll(&s);
  server_dispatch(&s);
  int status;pid_t pid;while((pid=waitpid(-1,&status,WNOHANG))>0){for(int i=0;i<s.child_count;i++)if(s.children[i]==pid){s.children[i]=s.children[--s.child_count];break;}if(WIFEXITED(status)&&WEXITSTATUS(status)!=0){fprintf(stderr,"[launch] pid %d exited %d\n",(int)pid,WEXITSTATUS(status));app_notice(&s,"Client exited with status %d — see launch log",WEXITSTATUS(status));}}
  if(s.demo&&!initial_focus&&s.time>2.0&&s.monitors[0].surface){server_focus(&s,s.monitors[0].surface);initial_focus=true;}
  float blend=1.f-expf(-(float)s.dt*12.f);s.yaw+=(s.target_yaw-s.yaw)*blend;s.pitch+=(s.target_pitch-s.pitch)*blend;s.fov+=(s.target_fov-s.fov)*blend;
  Hit hover=scene_pick(&s,s.mouse_x,s.mouse_y,false);s.hover_button=hover.kind==HIT_BUTTON?hover.button:-1;
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
