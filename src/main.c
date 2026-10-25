#include "chrome.h"
#include "cdconfig.h"
#include "fbdev_pixfmt.h"
#include "i18n.h"
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
 server_touch_cancel(s);server_release_keys(s,time);clear_drive_controls(s);s->door_shortcut_down=false;s->autopilot_shortcut_down=false;s->quit_shortcut_down=false;s->hud_shortcut_down=false;scene_wheel_cancel(s);
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
/* Render sampling tokens map straight to GL filters. Upscale only accepts
 * the two non-mipmap modes; downscale additionally accepts mipmap modes. */
static bool config_filter(const char *value,bool mipmaps,GLuint *out){
 if(!strcmp(value,"nearest")){*out=GL_NEAREST;return true;}
 if(!strcmp(value,"linear")){*out=GL_LINEAR;return true;}
 if(!mipmaps)return false;
 if(!strcmp(value,"nearest_mipmap_nearest")){*out=GL_NEAREST_MIPMAP_NEAREST;return true;}
 if(!strcmp(value,"linear_mipmap_nearest")){*out=GL_LINEAR_MIPMAP_NEAREST;return true;}
 if(!strcmp(value,"nearest_mipmap_linear")){*out=GL_NEAREST_MIPMAP_LINEAR;return true;}
 if(!strcmp(value,"linear_mipmap_linear")){*out=GL_LINEAR_MIPMAP_LINEAR;return true;}
 return false;
}
static void apply_render_config(Server *s,const CdConfig *cfg){
 s->render_to_texture=cd_config_get_bool(cfg,"display","render_to_texture",s->render_to_texture);
 const char *v=cd_config_get_string(cfg,"display","render_scale",NULL);
 if(v){
  char *end=NULL;errno=0;double n=strtod(v,&end);
  if(errno==0&&end!=v&&end&&!*end&&isfinite(n)&&n>=0.001&&n<=100.0)s->render_scale=n;
  else fprintf(stderr,"[config] ignoring render_scale '%s' (want finite 0.001..100)\n",v);
 }
 v=cd_config_get_string(cfg,"display","render_upscale",NULL);
 if(v){GLuint f;if(config_filter(v,false,&f))s->render_mag_filter=f;
  else fprintf(stderr,"[config] ignoring render_upscale '%s' (want nearest|linear)\n",v);}
 v=cd_config_get_string(cfg,"display","render_downscale",NULL);
 if(v){
  GLuint f;
  if(config_filter(v,true,&f))s->render_min_filter=f;
  else fprintf(stderr,"[config] ignoring render_downscale '%s' (want nearest|linear|nearest_mipmap_nearest|linear_mipmap_nearest|nearest_mipmap_linear|linear_mipmap_linear)\n",v);
 }
 /* display.post_shader is just a raw ';'-separated list here:
  * apply_render_config runs before (and independently of) GL context setup.
  * scene.c notices post_dirty and parses/loads/compiles the stages lazily in
  * scene_rtt_begin when the context is current. A missing key clears the
  * setting, so removing it from the file turns the chain off on the next
  * reload; scene.c also skips empty entries and per-stage "off" tokens. */
 v=cd_config_get_string(cfg,"display","post_shader",NULL);
 if(!v)v="";
 if(!strcmp(v,"off"))v="";   /* settings GUI's explicit whole-chain "off" */
 if(strcmp(v,s->post_shader_path)){
  snprintf(s->post_shader_path,sizeof(s->post_shader_path),"%s",v);
  s->post_dirty=true;
 }
}
static bool parse_backend(const char *name,Backend *out){
 if(!strcmp(name,"headless")){*out=CD_BACKEND_HEADLESS;return true;}
 if(!strcmp(name,"windowed")){*out=CD_BACKEND_WINDOWED;return true;}
 if(!strcmp(name,"fbdev")){*out=CD_BACKEND_FBDEV;return true;}
 if(!strcmp(name,"drm")){*out=CD_BACKEND_DRM;return true;}
 return false;
}
static bool set_fbdev_path(Server *s,const char *path){
 if(snprintf(s->fbdev_path,sizeof(s->fbdev_path),"%s",path)>=(int)sizeof(s->fbdev_path)){
  fprintf(stderr,"Framebuffer device path too long\n");return false;
 }
 return true;
}
static bool set_drm_path(Server *s,const char *path){
 if(snprintf(s->drm_path,sizeof(s->drm_path),"%s",path)>=(int)sizeof(s->drm_path)){
  fprintf(stderr,"DRM device path too long\n");return false;
 }
 return true;
}
/* ------------------------------------------------------------ language */
/* The UI language is applied when a texture is *baked*, not per frame, so a
 * runtime switch has to rebuild the baked text (ui_refresh_text). The install
 * root is not known when the config file is first parsed - it comes from
 * /proc/self/exe later in main() - so the startup pass only records what was
 * requested and main() applies it once s.root exists. */
static char language_request[32]="auto";  /* [ui] language from the config file */
static char language_live[32];            /* language currently baked in */

/* $CHROMEDRIVE_LANG wins over the config file, matching the CHROMEDRIVE_*
 * override convention used for the Launch Control slots. */
static const char *language_want(void){
 const char *env=getenv("CHROMEDRIVE_LANG");
 return (env&&*env)?env:language_request;
}
static void apply_language(Server *s,bool startup){
 const char *now=i18n_set_language(s->root,language_want());
 if(!strcmp(now,language_live))return;
 snprintf(language_live,sizeof(language_live),"%s",now);
 fprintf(stderr,"[i18n] language %s, catalog %s\n",now,*i18n_catalog_path()?i18n_catalog_path():"built-in English");
 /* At startup ui_init() has not run yet, so there is nothing baked to redo. */
 if(!startup&&s->buttons[0].texture)ui_refresh_text(s);
}
static void apply_config(Server *s,const CdConfig *cfg,bool startup,const char **socket_out){
  snprintf(language_request,sizeof(language_request),"%s",cd_config_get_string(cfg,"ui","language","auto"));
  if(!startup)apply_language(s,false); /* on reload s.root is already known */
  if(startup){
   long w=cd_config_get_int(cfg,"display","width",s->width),h=cd_config_get_int(cfg,"display","height",s->height);
   if(w>=640&&w<=7680&&h>=400&&h<=4320){s->width=(int)w;s->height=(int)h;}
   else if(cd_config_has(cfg,"display","width")||cd_config_has(cfg,"display","height"))fprintf(stderr,"[config] ignoring out-of-range display size\n");
   s->fullscreen=cd_config_get_bool(cfg,"display","fullscreen",s->fullscreen);
   const char *backend=cd_config_get_string(cfg,"session","backend",NULL);
   if(backend){
    Backend parsed;
    if(parse_backend(backend,&parsed))s->backend=parsed;
    else fprintf(stderr,"[config] unknown session.backend '%s' (want headless|windowed|fbdev|drm)\n",backend);
   }else s->backend=cd_config_get_bool(cfg,"session","headless",s->backend==CD_BACKEND_HEADLESS)?CD_BACKEND_HEADLESS:CD_BACKEND_WINDOWED;
   const char *dev=cd_config_get_string(cfg,"session","fbdev_device",NULL);
   if(dev){
    if(!*dev)s->fbdev_path[0]=0;
    else{char tmp[sizeof s->fbdev_path];
     if(snprintf(tmp,sizeof(tmp),"%s",dev)>=(int)sizeof(tmp))fprintf(stderr,"[config] session.fbdev_device path too long; using the previous value\n");
     else snprintf(s->fbdev_path,sizeof(s->fbdev_path),"%s",tmp);}
   }
   const char *drm_dev=cd_config_get_string(cfg,"session","drm_device",NULL);
   if(drm_dev){
    if(!*drm_dev)s->drm_path[0]=0;
    else{char tmp[sizeof s->drm_path];
     if(snprintf(tmp,sizeof(tmp),"%s",drm_dev)>=(int)sizeof(tmp))fprintf(stderr,"[config] session.drm_device path too long; using the previous value\n");
     else snprintf(s->drm_path,sizeof(s->drm_path),"%s",tmp);}
   }
   const char *drm_mode=cd_config_get_string(cfg,"session","drm_mode",NULL);
   if(drm_mode){
    if(!*drm_mode)s->drm_mode_width=s->drm_mode_height=0;
    else{
     int mw,mh,mbpp;
     if(cd_fb_mode_parse(drm_mode,&mw,&mh,&mbpp)&&mbpp==0){s->drm_mode_width=mw;s->drm_mode_height=mh;}
     else fprintf(stderr,"[config] ignoring session.drm_mode '%s' (want WIDTHxHEIGHT)\n",drm_mode);
    }
   }
   s->fbdev_vsync=cd_config_get_bool(cfg,"session","fbdev_vsync",s->fbdev_vsync);
   s->fbdev_pan=cd_config_get_bool(cfg,"session","fbdev_pan",s->fbdev_pan);
   s->fbdev_vnc_mirror=cd_config_get_bool(cfg,"session","fbdev_vnc_mirror",s->fbdev_vnc_mirror);
   s->fbdev_evdev=cd_config_get_bool(cfg,"session","fbdev_evdev",s->fbdev_evdev);
   s->fbdev_grab_input=cd_config_get_bool(cfg,"session","fbdev_grab_input",s->fbdev_grab_input);
   const char *input_devs=cd_config_get_string(cfg,"session","fbdev_input_devices",NULL);
   if(input_devs){
    char *copy=strdup(input_devs);
    if(copy){
     char *tok=strtok(copy," ,\t\n");
     s->fbdev_input_device_count=0;
     while(tok && s->fbdev_input_device_count<8){
      snprintf(s->fbdev_input_devices[s->fbdev_input_device_count],sizeof(s->fbdev_input_devices[0]),"%s",tok);
      s->fbdev_input_device_count++;
      tok=strtok(NULL," ,\t\n");
     }
     free(copy);
    }
   }
   long port=cd_config_get_int(cfg,"session","vnc_port",s->vnc_port);
   if(port>=1&&port<=65535)s->vnc_port=(int)port;else if(cd_config_has(cfg,"session","vnc_port"))fprintf(stderr,"[config] ignoring bad vnc_port\n");
   s->demo=cd_config_get_bool(cfg,"session","demo",s->demo);
   static char socket_buf[128];
   const char *sock=cd_config_get_string(cfg,"session","socket","");
   if(socket_out&&sock&&*sock){snprintf(socket_buf,sizeof(socket_buf),"%s",sock);*socket_out=socket_buf;}
  }
 static float config_fov_seen=65.f;
 float cfg_fov=clampf((float)cd_config_get_double(cfg,"display","fov",65.0),CD_DRIVER_FOV_MIN,CD_DRIVER_FOV_MAX);
 s->fps_limit=(int)clampf((float)cd_config_get_int(cfg,"display","fps",s->fps_limit),1,120);
 /* Preserve live camera state across unrelated config reloads. A changed
  * display.fov still applies, except while a monitor owns focus-view zoom. */
 if(startup){
  s->base_fov=s->target_fov=s->fov=cfg_fov;
  config_fov_seen=cfg_fov;
 }else if(fabsf(cfg_fov-config_fov_seen)>1e-4f){
  config_fov_seen=cfg_fov;
  s->base_fov=cfg_fov;
  if(!s->focus_view)s->target_fov=cfg_fov;
 }
 apply_render_config(s,cfg);
 long neon=cd_config_get_int(cfg,"cockpit","neon",s->theme);neon=((neon%3)+3)%3;
 if((int)neon!=s->theme){s->theme=(int)neon;s->rebuild_scene=true;}
 s->hud_visible=cd_config_get_bool(cfg,"cockpit","hud",s->hud_visible);
 const char *bg=cd_config_get_string(cfg,"cockpit","background",NULL);
 if(bg){int mode=!strcmp(bg,"texture")?CD_BG_TEXTURE:!strcmp(bg,"none")?CD_BG_NONE:!strcmp(bg,"3d")?CD_BG_3D:-1;
  if(mode>=0)s->bg_mode=mode;else fprintf(stderr,"[config] unknown background '%s' (want 3d|texture|none)\n",bg);}
}
/* ---- Launch Control menu configuration ([app.*]) ----
 * The KDE-menu-editor half of the config file. Each of the six physical
 * Launch Control slots has a section ([app.terminal] ... [app.cockpit]):
 *   action          app | neon | cockpit (slots 5/6 default to built-ins)
 *   name, sub       button label lines
 *   exec            shell command for action=app; "%root%" = install root
 *   app_id          substring matched to focus an already-running instance
 *   icon            0..5 (prompt, folder, music, bars, sun, wheel)
 *   enabled         false greys the button out (click = notice)
 *   autostart       join the --demo launch set (action=app slots)
 * Everything applies live: relabels regenerate button textures, assignment
 * changes take effect on the next click. CHROMEDRIVE_TERMINAL/FILES/AUDIO/
 * METERS still win for those four legacy sections' exec values. */
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
  const char *action=cd_config_get_string(cfg,sec,"action",NULL);
  if(action){
   if(!strcmp(action,"app"))b->action=CD_BUTTON_APP;
   else if(!strcmp(action,"neon"))b->action=CD_BUTTON_NEON;
   else if(!strcmp(action,"cockpit"))b->action=CD_BUTTON_COCKPIT;
   else fprintf(stderr,"[config] %s unknown action '%s' (want app|neon|cockpit)\n",sec,action);
  }
  const char *id=cd_config_get_string(cfg,sec,"app_id",NULL);
  if(id)snprintf(s->app_ids[i],sizeof(s->app_ids[i]),"%s",id);
  s->app_autostart[i]=cd_config_get_bool(cfg,sec,"autostart",s->app_autostart[i]);
  const char *exec=cd_config_get_string(cfg,sec,"exec",NULL);
  if(exec){
   if(*exec){
    if(i<4&&getenv(envnames[i]))fprintf(stderr,"[config] %s exec ignored: $%s is set\n",sec,envnames[i]);
    else {char *x=expand_root(exec,s->root);if(x){free(s->launch_commands[i]);s->launch_commands[i]=x;}}
   }else if(i>=4||!getenv(envnames[i])){free(s->launch_commands[i]);s->launch_commands[i]=NULL;}
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
 app_notice(s,tr("Configuration reloaded"));
}
void app_exec(Server *s,const char *command){
 if(!command||!*command)return;
 if(s->child_count>=128){app_notice(s,tr("Child process limit reached"));return;}
 posix_spawnattr_t attr;posix_spawnattr_init(&attr);
 short spawn_flags=POSIX_SPAWN_SETPGROUP;sigset_t child_mask;
 /* signalfd requires SIGUSR1/2 blocked in every compositor thread. Clients
  * must inherit the caller's original mask, not our private VT mask. */
 if(backend_vt_child_mask(&child_mask)){
  posix_spawnattr_setsigmask(&attr,&child_mask);spawn_flags|=POSIX_SPAWN_SETSIGMASK;
 }
 posix_spawnattr_setflags(&attr,spawn_flags);posix_spawnattr_setpgroup(&attr,0);
 char *args[]={"sh","-lc",(char*)command,NULL};pid_t pid;int err=posix_spawn(&pid,"/bin/sh",NULL,&attr,args,environ);posix_spawnattr_destroy(&attr);
 if(err){app_notice(s,tr("Launch failed: %s"),strerror(err));fprintf(stderr,"[launch] %s: %s\n",command,strerror(err));return;}
 s->children[s->child_count++]=pid;fprintf(stderr,"[launch] pid=%d %s\n",(int)pid,command);app_notice(s,tr("Connecting a new native Wayland client..."));
}
static void action_cycle_neon(Server *s){s->theme=(s->theme+1)%3;s->rebuild_scene=true;app_notice(s,tr("Underglow / %s"),s->theme==0?tr("ULTRAVIOLET"):s->theme==1?tr("ICE BLUE"):tr("ACID LIME"));}
static void action_driver_view(Server *s){scene_reset_camera(s);app_notice(s,tr("Selected seat view restored"));}
void app_launch(Server *s,int action,bool force_new){
 if(action<0||action>=6)return;
 if(!s->buttons[action].enabled){app_notice(s,tr("%s is disabled in the menu configuration"),tr(s->buttons[action].name));return;}
 if(s->buttons[action].action==CD_BUTTON_NEON){action_cycle_neon(s);return;}
 if(s->buttons[action].action==CD_BUTTON_COCKPIT){action_driver_view(s);return;}
 if(!s->launch_commands[action]||!*s->launch_commands[action]){app_notice(s,tr("No app assigned to %s - right-click it to choose one"),tr(s->buttons[action].name));return;}
 if(!force_new&&*s->app_ids[action]){for(int i=0;i<CD_MAX_MONITORS;i++){Surface *p=s->monitors[i].surface;if(p&&strstr(p->app_id,s->app_ids[action])){server_focus(s,p);scene_focus_monitor(s,i,true);return;}}}
 app_exec(s,s->launch_commands[action]);
}
static bool contains_ci(const char *haystack,const char *needle){return haystack&&strcasestr(haystack,needle);}
static bool slot_looks_terminal(Server *s,int i){
 if(s->buttons[i].action!=CD_BUTTON_APP||!s->launch_commands[i]||!*s->launch_commands[i])return false;
 static const char *const words[]={"terminal","console","konsole","foot","alacritty","kitty","wezterm","xterm","zutty","terminator","tilix","ptyxis","blackbox","contour",NULL};
 for(int w=0;words[w];w++)if(contains_ci(s->buttons[i].name,words[w])||contains_ci(s->app_ids[i],words[w])||contains_ci(s->launch_commands[i],words[w]))return true;
 return false;
}
/* Ctrl+Alt+Enter keeps its documented meaning even when slot 1 has been
 * reassigned: prefer any slot that still points at a terminal, then the
 * historical slot-1 command, then the environment/default terminal. */
static void launch_new_terminal(Server *s){
 for(int i=0;i<6;i++)if(slot_looks_terminal(s,i)){app_exec(s,s->launch_commands[i]);app_notice(s,tr("New terminal launched"));return;}
 if(s->buttons[0].action==CD_BUTTON_APP&&s->launch_commands[0]&&*s->launch_commands[0]){app_exec(s,s->launch_commands[0]);app_notice(s,tr("New terminal launched"));return;}
 const char *env=getenv("CHROMEDRIVE_TERMINAL");
 const char *cmd=env&&*env?env:(s->launch_commands[0]&&*s->launch_commands[0]?s->launch_commands[0]:"foot");
 app_exec(s,cmd);app_notice(s,tr("New terminal launched"));
}
/* A running settings GUI cannot receive new argv, so the compositor leaves
 * it a one-line request file ("<slot> <nonce>") in the private runtime dir.
 * chromedrive-config polls for it and opens its picker. The temp+rename
 * dance means the GUI never observes a half-written request. */
static bool settings_picker_request(Server *s,int slot){
 if(slot<0||slot>=6||!*s->runtime)return false;
 const char *base=strrchr(s->socket,'/');base=base?base+1:s->socket;
 char safe[128];size_t j=0;
 for(size_t i=0;base[i]&&j+1<sizeof(safe);i++){unsigned char ch=(unsigned char)base[i];safe[j++]=((ch>='A'&&ch<='Z')||(ch>='a'&&ch<='z')||(ch>='0'&&ch<='9')||ch=='.'||ch=='_'||ch=='-')?(char)ch:'_';}
 safe[j]=0;if(!*safe)snprintf(safe,sizeof(safe),"wayland-0");
 char path[4400],tmp[4400];
 if(snprintf(path,sizeof(path),"%s/chromedrive-config.%s.picker-request",s->runtime,safe)>=(int)sizeof(path))return false;
 if(snprintf(tmp,sizeof(tmp),"%s.tmp-%d",path,(int)getpid())>=(int)sizeof(tmp))return false;
 FILE *f=fopen(tmp,"w");if(!f)return false;
 unsigned long long nonce=((unsigned long long)(clock_now()*1000000.0)<<20)^(unsigned long long)s->frames^(unsigned long long)getpid();
 bool ok=fprintf(f,"%d %llu\n",slot,nonce)>=0;
 if(fclose(f)!=0)ok=false;
 if(ok&&rename(tmp,path)==0)return true;
 unlink(tmp);return false;
}
/* The dash cog / right-clicking a Launch Control tile: focus the settings
 * GUI if it is already on a monitor, otherwise launch it. A fresh launch may
 * be deep-linked to one slot with its app picker already open; an already
 * running GUI gets the same deep link through the request file above. */
void app_open_settings_slot(Server *s,int slot,bool choose_app){
 s->cog_spin_until=s->time+0.9;
 for(int i=0;i<CD_MAX_MONITORS;i++){Surface *p=s->monitors[i].surface;if(p&&strstr(p->app_id,"chromedrive.config")){server_focus(s,p);scene_focus_monitor(s,i,true);bool requested=choose_app&&settings_picker_request(s,slot);app_notice(s,choose_app?(requested?tr("Opening app picker..."):tr("Right-click the slot in Settings to assign an app")):tr("Settings"));return;}}
 char cmd[4400];
 if(slot>=0&&slot<6)snprintf(cmd,sizeof(cmd),"'%s/bin/chromedrive-config' --slot %d%s",s->root,slot,choose_app?" --choose-app":"");
 else snprintf(cmd,sizeof(cmd),"'%s/bin/chromedrive-config'",s->root);
 app_exec(s,cmd);app_notice(s,choose_app?tr("Opening app picker..."):tr("Opening Settings..."));
}
void app_open_settings(Server *s){app_open_settings_slot(s,-1,false);}
/* Quit paths share one function so the keyboard shortcut and the dashboard
 * button behave predictably. The hotkey is explicit and immediate. The button
 * is deliberately two-stage: a stray click arms EXIT for three seconds, and a
 * second click inside that window commits. */
void app_request_quit(Server *s,bool via_hotkey){
 if(via_hotkey){fprintf(stderr,"[input] Meta+Shift+Q quit\n");s->running=false;return;}
 if(s->time<s->quit_armed_until){fprintf(stderr,"[input] EXIT confirmed from dashboard button\n");s->running=false;return;}
 s->quit_armed_until=s->time+3.0;
 fprintf(stderr,"[input] EXIT armed from dashboard button (3 s)\n");
 app_notice(s,tr("EXIT armed — press the red button again to quit ChromeDrive"));
}
/* The HUD overlay is session-only runtime state: the dash button and Meta+H
 * flip Server.hud_visible without touching chromedrive.conf. A later config
 * hot reload writes the file's [cockpit] hud value back over this choice. */
void app_toggle_hud(Server *s){
 s->hud_visible=!s->hud_visible;
 fprintf(stderr,"[input] HUD %s\n",s->hud_visible?"on":"off");
 if(s->hud_visible)app_notice(s,tr("HUD on - Meta+H or the dash HUD button hides it"));
}
/* ---- F6 Arrange mode ---------------------------------------------------
 * A modal, session-only transform editor for whole spatial monitors. The
 * selected monitor's Plane is translated along its own right/up axes (and
 * toward/away from the eye), rotated in yaw/pitch/roll, and uniformly
 * scaled. Because rendering and scene_pick() both use that same Plane, the
 * app remains clickable exactly where it is drawn after exiting the mode. */
static bool arrange_valid(Server *s){
 return s->arrange_mode&&s->arrange_slot>=0&&s->arrange_slot<CD_MAX_MONITORS&&s->monitors[s->arrange_slot].surface;
}
static void monitor_transform_notice(Server *s,int slot,const char *prefix){
 if(slot<0||slot>=CD_MAX_MONITORS||!s->monitors[slot].surface)return;
 Plane p=s->monitors[slot].plane;Surface *surf=s->monitors[slot].surface;
 int win_w,win_h;
 if(surf->slot==CD_CLUSTER_SLOT||strstr(surf->app_id,"chromedrive.cluster")){win_w=CD_CLUSTER_W;win_h=CD_CLUSTER_H;}
 else{win_w=surf->win_w>0?surf->win_w:(surf->width>0?surf->width:CD_APP_W);win_h=surf->win_h>0?surf->win_h:(surf->height>0?surf->height:CD_APP_H);}
 app_notice(s,tr("%s CD-%02d | %.2f, %.2f, %.2f m | yaw %.0f pitch %.0f roll %.0f | %.2fx%.2f m | window %dx%d"),
  prefix,slot+1,p.center.x,p.center.y,p.center.z,p.yaw,p.pitch,p.roll,p.width,p.height,win_w,win_h);
}
static void arrange_notice(Server *s){
 if(arrange_valid(s))monitor_transform_notice(s,s->arrange_slot,tr("Arrange"));
}
static bool arrange_set_target(Server *s,int slot,bool move_camera){
 if(slot<0||slot>=CD_MAX_MONITORS||slot==CD_CLUSTER_SLOT||!s->monitors[slot].surface)return false;
 s->arrange_slot=slot;s->active_slot=slot;s->arrange_dragging=false;s->arrange_drag_button=0;s->arrange_handle=CD_HANDLE_NONE;
 server_focus(s,s->monitors[slot].surface);
 if(move_camera)scene_focus_monitor(s,slot,false);
 arrange_notice(s);
 return true;
}
static int arrange_find_slot(Server *s){
 Surface *root=surface_root(s->keyboard_focus);
 if(root&&root->slot>=0&&root->slot<CD_APP_BAYS&&s->monitors[root->slot].surface==root)return root->slot;
 if(s->active_slot>=0&&s->active_slot<CD_APP_BAYS&&s->monitors[s->active_slot].surface)return s->active_slot;
 for(int i=0;i<CD_APP_BAYS;i++)if(s->monitors[i].surface)return i;
 return -1;
}
static bool arrange_ensure_target(Server *s){
 if(arrange_valid(s))return true;
 int slot=arrange_find_slot(s);
 return slot>=0&&arrange_set_target(s,slot,false);
}
static void arrange_exit(Server *s){
 s->arrange_mode=false;s->arrange_dragging=false;s->arrange_drag_button=0;s->arrange_handle=CD_HANDLE_NONE;s->arrange_handles=false;
 app_notice(s,tr("Arrange mode off"));
}
static void arrange_toggle(Server *s){
 if(s->arrange_mode){arrange_exit(s);return;}
 if(s->held_buttons>0||s->frame_dragging){app_notice(s,tr("Arrange: release the pointer button first"));return;}
 int slot=arrange_find_slot(s);
 if(slot<0){app_notice(s,tr("Arrange: no window is open"));return;}
 server_touch_cancel(s); /* Arrange is modal: do not leave app contacts live */
 s->arrange_mode=true;s->arrange_dragging=false;s->arrange_drag_button=0;s->arrange_handle=CD_HANDLE_NONE;s->arrange_handles=false;
 arrange_set_target(s,slot,true);
}
static const char *arrange_handle_name(int handle){
 switch(handle){
 case CD_HANDLE_MOVE:return "move in plane";
 case CD_HANDLE_AXIS_X:return tr("move along X");
 case CD_HANDLE_AXIS_Y:return tr("move along Y");
 case CD_HANDLE_AXIS_Z:return tr("move along Z/depth");
 case CD_HANDLE_ROTATE_YAW:return "rotate yaw";
 case CD_HANDLE_ROTATE_PITCH:return "rotate pitch";
 case CD_HANDLE_ROTATE_ROLL:return "rotate roll";
 case CD_HANDLE_SCALE:return "uniform scale";
 case CD_HANDLE_RESIZE_NW:return "resize top-left corner";
 case CD_HANDLE_RESIZE_N:return "resize top edge";
 case CD_HANDLE_RESIZE_NE:return "resize top-right corner";
 case CD_HANDLE_RESIZE_E:return "resize right edge";
 case CD_HANDLE_RESIZE_SE:return "resize bottom-right corner";
 case CD_HANDLE_RESIZE_S:return "resize bottom edge";
 case CD_HANDLE_RESIZE_SW:return "resize bottom-left corner";
 case CD_HANDLE_RESIZE_W:return "resize left edge";
 default:return "drag";
 }
}
static void arrange_handles_notice(Server *s){
 if(!arrange_valid(s))return;
 if(s->arrange_handles)app_notice(s,tr("Handles on CD-%02d: amber move | RGB axes/rings | white resize | magenta scale"),s->arrange_slot+1);
 else app_notice(s,tr("Handles off CD-%02d"),s->arrange_slot+1);
}
static bool arrange_handles_button(Server *s,int slot){
 bool same=arrange_valid(s)&&s->arrange_slot==slot;
 if(!s->arrange_mode){
  if(s->held_buttons>0||s->frame_dragging){app_notice(s,tr("Arrange: release the pointer button first"));return false;}
  server_touch_cancel(s); /* Arrange is modal: do not leave app contacts live */
  s->arrange_mode=true;
 }
 if(!arrange_set_target(s,slot,false))return false;
 s->arrange_handles=same?!s->arrange_handles:true;
 arrange_handles_notice(s);
 return true;
}
static bool arrange_local_point(Server *s,Plane p,int x,int y,float *lx,float *ly){
 float u,v,d;Vec3 ray=camera_ray((float)x,(float)y,s->width,s->height,s->fov,s->yaw,s->pitch);
 if(!plane_hit(p,s->eye,ray,&u,&v,&d,false))return false;
 *lx=(u-.5f)*p.width;*ly=(.5f-v)*p.height;return true;
}
static void arrange_begin_drag(Server *s,uint32_t button,int handle){
 s->arrange_dragging=true;s->arrange_drag_button=(int)button;s->arrange_handle=handle;
 s->arrange_last_x=s->arrange_start_x=s->mouse_x;s->arrange_last_y=s->arrange_start_y=s->mouse_y;
 s->arrange_start_plane=s->monitors[s->arrange_slot].plane;
 Surface *surf=s->monitors[s->arrange_slot].surface;
 s->arrange_start_win_w=surf&&surf->win_w>0?surf->win_w:(surf&&surf->width>0?surf->width:CD_APP_W);
 s->arrange_start_win_h=surf&&surf->win_h>0?surf->win_h:(surf&&surf->height>0?surf->height:CD_APP_H);
 if(!arrange_local_point(s,s->arrange_start_plane,s->mouse_x,s->mouse_y,&s->arrange_start_local_x,&s->arrange_start_local_y)){s->arrange_start_local_x=0;s->arrange_start_local_y=0;}
}
static void arrange_cycle(Server *s){
 int start=arrange_valid(s)?s->arrange_slot:s->active_slot;
 for(int d=1;d<=CD_MAX_MONITORS;d++){
  int slot=(start+d)%CD_MAX_MONITORS;
  if(arrange_set_target(s,slot,true))return;
 }
 app_notice(s,tr("Arrange: no window is open"));
}
bool app_arrange_pointer_button(Server *s,Hit hit,uint32_t button,bool down){
 if(!s->arrange_mode)return false;
 if(button!=BTN_LEFT&&button!=BTN_RIGHT)return true;
 if(down){
  if(hit.kind==HIT_HANDLES_BUTTON){
   if(button==BTN_LEFT)arrange_handles_button(s,hit.slot);
   return true;
  }
  if(hit.kind==HIT_ARRANGE_HANDLE&&button==BTN_LEFT&&arrange_set_target(s,hit.slot,false)){
   arrange_begin_drag(s,button,hit.handle);
   app_notice(s,tr("CD-%02d handle: %s"),hit.slot+1,arrange_handle_name(hit.handle));
  }else if((hit.kind==HIT_APP||hit.kind==HIT_FRAME)&&arrange_set_target(s,hit.slot,false)){
   arrange_begin_drag(s,button,CD_HANDLE_NONE);
  }
  return true; /* dashboard/close/app clicks are modal while arranging */
 }
 if(s->arrange_dragging&&s->arrange_drag_button==(int)button){
  s->arrange_dragging=false;s->arrange_drag_button=0;s->arrange_handle=CD_HANDLE_NONE;arrange_notice(s);
 }
 return true;
}
void app_arrange_pointer_motion(Server *s,int x,int y){
 if(!s->arrange_mode)return;
 if(!s->arrange_dragging||!arrange_ensure_target(s)){s->arrange_last_x=x;s->arrange_last_y=y;return;}
 int slot=s->arrange_slot;
 if(s->arrange_handle!=CD_HANDLE_NONE){
  Plane start=s->arrange_start_plane,base=monitor_plane(slot),out=start;
  int total_dx=x-s->arrange_start_x,total_dy=y-s->arrange_start_y;
  Vec3 rel=vsub(start.center,s->eye);float dist=sqrtf(vdot(rel,rel));
  float meters_per_pixel=2.f*dist*tanf(s->fov*.00872664626f)/(float)s->height;
  float lx=0,ly=0;bool have_local=arrange_local_point(s,start,x,y,&lx,&ly);
  float dlx=have_local?lx-s->arrange_start_local_x:total_dx*meters_per_pixel;
  float dly=have_local?ly-s->arrange_start_local_y:-total_dy*meters_per_pixel;
  bool shift=s->keys[KEY_LEFTSHIFT]||s->keys[KEY_RIGHTSHIFT];
  switch(s->arrange_handle){
  case CD_HANDLE_MOVE:out=plane_translate_local(start,s->eye,dlx,dly,0);break;
  case CD_HANDLE_AXIS_X:out=plane_translate_local(start,s->eye,dlx,0,0);break;
  case CD_HANDLE_AXIS_Y:out=plane_translate_local(start,s->eye,0,dly,0);break;
  case CD_HANDLE_AXIS_Z:out=plane_translate_local(start,s->eye,0,0,-total_dy*meters_per_pixel);break;
  case CD_HANDLE_ROTATE_YAW:out=plane_rotate(start,total_dx*.25f,0,0);break;
  case CD_HANDLE_ROTATE_PITCH:out=plane_rotate(start,0,total_dy*.25f,0);break;
  case CD_HANDLE_ROTATE_ROLL:{
   float delta=total_dx*.30f;
   if(have_local&&hypotf(s->arrange_start_local_x,s->arrange_start_local_y)>.06f&&hypotf(lx,ly)>.06f){
    delta=(atan2f(ly,lx)-atan2f(s->arrange_start_local_y,s->arrange_start_local_x))*57.2957795f;
    while(delta>180.f)delta-=360.f;while(delta<=-180.f)delta+=360.f;
   }
   out=plane_rotate(start,0,0,delta);break;
  }
  case CD_HANDLE_SCALE:out=plane_uniform_scale(start,base,expf((total_dx-total_dy)*.005f));break;
  default:{
   float new_w=start.width,new_h=start.height;int fixed_x=0,fixed_y=0;
   int hd=s->arrange_handle;
   if(hd==CD_HANDLE_RESIZE_E||hd==CD_HANDLE_RESIZE_NE||hd==CD_HANDLE_RESIZE_SE){new_w+=dlx;fixed_x=-1;}
   if(hd==CD_HANDLE_RESIZE_W||hd==CD_HANDLE_RESIZE_NW||hd==CD_HANDLE_RESIZE_SW){new_w-=dlx;fixed_x=1;}
   if(hd==CD_HANDLE_RESIZE_N||hd==CD_HANDLE_RESIZE_NE||hd==CD_HANDLE_RESIZE_NW){new_h+=dly;fixed_y=-1;}
   if(hd==CD_HANDLE_RESIZE_S||hd==CD_HANDLE_RESIZE_SE||hd==CD_HANDLE_RESIZE_SW){new_h-=dly;fixed_y=1;}
   bool corner=hd==CD_HANDLE_RESIZE_NW||hd==CD_HANDLE_RESIZE_NE||hd==CD_HANDLE_RESIZE_SE||hd==CD_HANDLE_RESIZE_SW;
   bool aspect=shift&&corner;
   if(aspect)out=plane_resize_aspect(start,base,s->eye,new_w,new_h,fixed_x,fixed_y);
   else out=plane_resize(start,base,s->eye,new_w,new_h,fixed_x,fixed_y);
   if(hd>=CD_HANDLE_RESIZE_NW&&hd<=CD_HANDLE_RESIZE_W){
    Surface *surf=s->monitors[slot].surface;
    if(surf){
     int start_w=s->arrange_start_win_w>0?s->arrange_start_win_w:(surf->win_w>0?surf->win_w:CD_APP_W);
     int start_h=s->arrange_start_win_h>0?s->arrange_start_win_h:(surf->win_h>0?surf->win_h:CD_APP_H);
     int req_w=start_w,req_h=start_h,got_w=req_w,got_h=req_h;
     plane_window_pixels(start,out,start_w,start_h,&req_w,&req_h);
     got_w=req_w;got_h=req_h;
     if(server_request_window_size(surf,req_w,req_h,&got_w,&got_h)&&(got_w!=req_w||got_h!=req_h))
      out=aspect?plane_resize_window_pixels_aspect(start,base,s->eye,start_w,start_h,got_w,got_h,fixed_x,fixed_y)
                :plane_resize_window_pixels(start,base,s->eye,start_w,start_h,got_w,got_h,fixed_x,fixed_y);
    }
   }
   break;
  }
  }
  scene_monitor_set_plane(s,slot,out);
  s->arrange_last_x=x;s->arrange_last_y=y;
  return;
 }
 int dx=x-s->arrange_last_x,dy=y-s->arrange_last_y;
 s->arrange_last_x=x;s->arrange_last_y=y;
 if(!dx&&!dy)return;
 bool shift=s->keys[KEY_LEFTSHIFT]||s->keys[KEY_RIGHTSHIFT];
 bool ctrl=s->keys[KEY_LEFTCTRL]||s->keys[KEY_RIGHTCTRL];
 bool alt=s->keys[KEY_LEFTALT]||s->keys[KEY_RIGHTALT];
 if(ctrl)scene_monitor_rotate(s,slot,0,0,dx*.30f);
 else if(shift||s->arrange_drag_button==BTN_RIGHT)scene_monitor_rotate(s,slot,dx*.25f,dy*.25f,0);
 else if(alt)scene_monitor_translate(s,slot,0,0,-dy*.010f);
 else{
  Plane p=s->monitors[slot].plane;Vec3 rel=vsub(p.center,s->eye);
  float dist=sqrtf(vdot(rel,rel));
  float meters_per_pixel=2.f*dist*tanf(s->fov*.00872664626f)/(float)s->height;
  scene_monitor_translate(s,slot,dx*meters_per_pixel,-dy*meters_per_pixel,0);
 }
}
bool app_arrange_pointer_wheel(Server *s,float steps){
 if(!s->arrange_mode)return false;
 if(!arrange_ensure_target(s)||steps==0)return true;
 bool alt=s->keys[KEY_LEFTALT]||s->keys[KEY_RIGHTALT];
 if(alt)scene_monitor_translate(s,s->arrange_slot,0,0,steps*.18f);
 else scene_monitor_scale(s,s->arrange_slot,expf(steps*.10f));
 arrange_notice(s);
 return true;
}
bool app_frame_pointer_button(Server *s,Hit hit,uint32_t button,bool down){
 if(s->arrange_mode)return app_arrange_pointer_button(s,hit,button,down);
 if(button!=BTN_LEFT)return s->frame_dragging;
 if(down){
  if(hit.kind==HIT_HANDLES_BUTTON){arrange_handles_button(s,hit.slot);return true;}
  if(hit.kind!=HIT_FRAME||hit.slot<0||hit.slot>=CD_MAX_MONITORS||!s->monitors[hit.slot].surface)return false;
  if(s->held_buttons>0){app_notice(s,tr("Move: release the pointer button first"));return true;}
  s->frame_dragging=true;s->frame_drag_slot=hit.slot;s->active_slot=hit.slot;
  s->frame_last_x=s->mouse_x;s->frame_last_y=s->mouse_y;
  server_focus(s,s->monitors[hit.slot].surface);
  return true;
 }
 if(s->frame_dragging){
  int slot=s->frame_drag_slot;s->frame_dragging=false;s->frame_drag_slot=-1;
  monitor_transform_notice(s,slot,tr("Moved"));
  return true;
 }
 return false;
}
void app_frame_pointer_motion(Server *s,int x,int y){
 if(!s->frame_dragging||s->arrange_mode)return;
 int slot=s->frame_drag_slot;
 if(slot<0||slot>=CD_MAX_MONITORS||!s->monitors[slot].surface){s->frame_dragging=false;s->frame_drag_slot=-1;return;}
 int dx=x-s->frame_last_x,dy=y-s->frame_last_y;s->frame_last_x=x;s->frame_last_y=y;
 if(!dx&&!dy)return;
 Plane p=s->monitors[slot].plane;Vec3 rel=vsub(p.center,s->eye);float dist=sqrtf(vdot(rel,rel));
 float meters_per_pixel=2.f*dist*tanf(s->fov*.00872664626f)/(float)s->height;
 scene_monitor_translate(s,slot,dx*meters_per_pixel,-dy*meters_per_pixel,0);
}
static void next_monitor(Server *s){for(int d=1;d<=CD_MAX_MONITORS;d++){int i=(s->active_slot+d)%CD_MAX_MONITORS;if(s->monitors[i].surface){server_focus(s,s->monitors[i].surface);scene_focus_monitor(s,i,s->focus_view);return;}}}
/* Compositor-level hotkeys, shared by every backend/input path (SDL, RFB/VNC).
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
 if(down&&s->vehicle.autopilot_enabled){
  /* A real driver input has priority over navigation in DRIVE mode. In CLIENT
   * mode the same keys still belong to the Wayland app and do not cancel the
   * route, so it remains possible to work while the car drives itself. */
  vehicle_autopilot_set(&s->vehicle,false);app_notice(s,tr("Manual control / autopilot disengaged"));
 }
 *held=down;
 return true; /* consumed locally: never leak motor controls into a Wayland app */
}
bool app_global_key(Server *s,uint32_t evdev,bool down){
 bool ctrl=s->keys[KEY_LEFTCTRL]||s->keys[KEY_RIGHTCTRL];
 bool alt=s->keys[KEY_LEFTALT]||s->keys[KEY_RIGHTALT];
 bool meta=s->keys[KEY_LEFTMETA]||s->keys[KEY_RIGHTMETA];
 /* Meta+O is a compositor shortcut, not an O key for the focused client.
  * Track its press independently of the modifier: auto-repeat must not
  * retrigger the doors, and releasing Meta before O must not leak a stray
  * O release into Wayland. Plain O and pre-existing client O holds pass
  * through unchanged. */
 if(evdev==KEY_O){
  if(s->door_shortcut_down){if(!down)s->door_shortcut_down=false;return true;}
  if(down&&meta&&!s->keys[KEY_O]){
   s->door_shortcut_down=true;vehicle_toggle_doors(&s->vehicle);
   app_notice(s,"%s",s->vehicle.door_target[0]>.5f?tr("Doors opening / Meta+O closes both"):tr("Doors closing / Meta+O opens both"));
   return true;
  }
 }
 /* Meta+A is deliberately separate from plain A (steering) and F7 (input
  * ownership). It can be engaged while working in a Wayland client. */
 if(evdev==KEY_A){
  if(s->autopilot_shortcut_down){if(!down)s->autopilot_shortcut_down=false;return true;}
  if(down&&meta&&!s->keys[KEY_A]){
   s->autopilot_shortcut_down=true;bool enable=!s->vehicle.autopilot_enabled;
   vehicle_autopilot_set(&s->vehicle,enable);
   app_notice(s,"%s",enable?tr("Autopilot engaged / doors closing, then city route"):tr("Autopilot disengaged / vehicle braking"));
   return true;
  }
 }
 /* Meta+H hides/shows the 2D HUD from anywhere, but is handled before the
  * Arrange block below where plain H toggles the transform handles. Track
  * the press like Meta+O so auto-repeat cannot flap the HUD and an early
  * Meta release cannot leak the H release. Plain H and Shift+H pass through. */
 if(evdev==KEY_H){
  if(s->hud_shortcut_down){if(!down)s->hud_shortcut_down=false;return true;}
  if(down&&meta&&!s->keys[KEY_H]){
   s->hud_shortcut_down=true;app_toggle_hud(s);
   return true;
  }
 }
 /* Meta+Shift+Q quits from anywhere, including Arrange mode where plain Q
  * rolls the selected monitor. Track the press like Meta+O/Meta+A so key
  * auto-repeat cannot retrigger it and an early Meta release cannot leak the
  * letter release into a Wayland client. Plain Q, Meta+Q and Shift+Q pass
  * through unchanged. */
 if(evdev==KEY_Q){
  if(s->quit_shortcut_down){if(!down)s->quit_shortcut_down=false;return true;}
  bool shift=s->keys[KEY_LEFTSHIFT]||s->keys[KEY_RIGHTSHIFT];
  if(down&&meta&&shift&&!s->keys[KEY_Q]){
   s->quit_shortcut_down=true;app_request_quit(s,true);
   return true;
  }
 }
 if(ctrl&&alt&&(evdev==KEY_ENTER||evdev==KEY_KPENTER||evdev==KEY_Q)){if(down){if(evdev==KEY_Q)s->running=false;else launch_new_terminal(s);}return true;}
 if(evdev==KEY_F6){if(down)arrange_toggle(s);return true;}
 if(s->arrange_mode&&evdev!=KEY_F1&&evdev!=KEY_F2&&evdev!=KEY_F4&&evdev!=KEY_F5&&evdev!=KEY_F7&&evdev!=KEY_F8&&evdev!=KEY_F9&&evdev!=KEY_F10&&evdev!=KEY_F11&&evdev!=KEY_F12){
  bool modifier=evdev==KEY_LEFTSHIFT||evdev==KEY_RIGHTSHIFT||evdev==KEY_LEFTCTRL||evdev==KEY_RIGHTCTRL||evdev==KEY_LEFTALT||evdev==KEY_RIGHTALT||evdev==KEY_LEFTMETA||evdev==KEY_RIGHTMETA;
  if(modifier)return false; /* let server_key maintain s->keys[] for drags */
  if(evdev==KEY_ESC){if(down)arrange_exit(s);return true;}
  if(evdev==KEY_TAB){if(down)arrange_cycle(s);return true;}
  if(evdev==KEY_F3){if(down&&arrange_ensure_target(s))scene_focus_monitor(s,s->arrange_slot,!s->focus_view);return true;}
  if(!down)return true;
  if(!arrange_ensure_target(s)){app_notice(s,tr("Arrange: no window is open"));return true;}
  if(evdev==KEY_H){s->arrange_handles=!s->arrange_handles;arrange_handles_notice(s);return true;}
  bool shift=s->keys[KEY_LEFTSHIFT]||s->keys[KEY_RIGHTSHIFT];
  float step=ctrl?.25f:shift?3.f:1.f;
  int slot=s->arrange_slot;bool changed=true;
  switch(evdev){
  case KEY_LEFT:scene_monitor_translate(s,slot,-.08f*step,0,0);break;
  case KEY_RIGHT:scene_monitor_translate(s,slot,.08f*step,0,0);break;
  case KEY_UP:scene_monitor_translate(s,slot,0,.08f*step,0);break;
  case KEY_DOWN:scene_monitor_translate(s,slot,0,-.08f*step,0);break;
  case KEY_PAGEUP:scene_monitor_translate(s,slot,0,0,.12f*step);break;
  case KEY_PAGEDOWN:scene_monitor_translate(s,slot,0,0,-.12f*step);break;
  case KEY_A:scene_monitor_rotate(s,slot,-3.f*step,0,0);break;
  case KEY_D:scene_monitor_rotate(s,slot,3.f*step,0,0);break;
  case KEY_W:scene_monitor_rotate(s,slot,0,-3.f*step,0);break;
  case KEY_S:scene_monitor_rotate(s,slot,0,3.f*step,0);break;
  case KEY_Q:scene_monitor_rotate(s,slot,0,0,-3.f*step);break;
  case KEY_E:scene_monitor_rotate(s,slot,0,0,3.f*step);break;
  case KEY_MINUS:case KEY_KPMINUS:scene_monitor_scale(s,slot,powf(1.08f,-step));break;
  case KEY_EQUAL:case KEY_KPPLUS:scene_monitor_scale(s,slot,powf(1.08f,step));break;
  case KEY_R:case KEY_0:{scene_monitor_reset(s,slot);Surface *surf=s->monitors[slot].surface;if(surf)server_request_window_size(surf,CD_APP_W,CD_APP_H,NULL,NULL);break;}
  default:changed=false;break;
  }
  if(changed)arrange_notice(s);
  return true;
 }
 bool global=evdev==KEY_F1||evdev==KEY_F2||evdev==KEY_F3||evdev==KEY_F4||evdev==KEY_F6||evdev==KEY_F7||evdev==KEY_F8||evdev==KEY_F9||evdev==KEY_F10||evdev==KEY_F11||evdev==KEY_F12;
 if(!global)return drive_key(s,evdev,down);if(!down)return true;
 switch(evdev){
 case KEY_F1:s->show_help=!s->show_help;break;
 case KEY_F2:scene_reset_camera(s);break;
 case KEY_F3:if(s->keyboard_focus)scene_focus_monitor(s,s->keyboard_focus->slot,!s->focus_view);else next_monitor(s);break;
 case KEY_F4:action_cycle_neon(s);break;
 case KEY_F7:
  /* Release app keys before changing ownership. Otherwise a key held during
   * the transition could remain pressed in the client even after its release
   * starts being consumed as a vehicle control. */
  app_release_input(s,SDL_GetTicks());
  s->steering_mode=s->steering_mode==VEHICLE_STEER_DRIVE?VEHICLE_STEER_CLIENT:VEHICLE_STEER_DRIVE;
  app_notice(s,s->steering_mode==VEHICLE_STEER_DRIVE?tr("DRIVE / WASD + wheel; F7 returns input to apps"):tr("CLIENT / wheel scrolls the focused Wayland app"));
  break;
 case KEY_F8:
  if(s->arrange_mode)arrange_exit(s);
  s->camera_mode=(VehicleCameraMode)((s->camera_mode+1)%3);
  /* Exterior orbiting must not become the cockpit's look direction when the
   * cycle returns to a seat. Also leave any tight app-focus FOV behind. */
  s->focus_view=false;s->target_fov=65;
  if(s->camera_mode==VEHICLE_CAMERA_DRIVER)scene_reset_camera(s);
  scene_wheel_cancel(s);
  app_notice(s,"%s",s->camera_mode==VEHICLE_CAMERA_DRIVER?(s->camera_seat==VEHICLE_SEAT_DRIVER?tr("Driver seat camera"):tr("Passenger seat camera")):s->camera_mode==VEHICLE_CAMERA_CHASE?tr("Exterior concept camera"):tr("EV systems cutaway"));
  break;
 case KEY_F9:
  s->camera_seat=s->camera_seat==VEHICLE_SEAT_DRIVER?VEHICLE_SEAT_PASSENGER:VEHICLE_SEAT_DRIVER;
  scene_reset_camera(s);scene_wheel_cancel(s);
  app_notice(s,"%s",s->camera_seat==VEHICLE_SEAT_DRIVER?tr("Driver seat / aligned with steering wheel"):tr("Passenger seat / F9 returns to driver"));
  break;
 case KEY_F10:
  vehicle_reset_pose(&s->vehicle);clear_drive_controls(s);scene_wheel_cancel(s);
  app_notice(s,tr("Vehicle recovered to the garage start pose"));break;
 case KEY_F11:if(s->backend==CD_BACKEND_WINDOWED&&s->window){s->fullscreen=!s->fullscreen;SDL_SetWindowFullscreen(s->window,s->fullscreen?SDL_WINDOW_FULLSCREEN_DESKTOP:0);}break;
 case KEY_F12:snprintf(s->screenshot_path,sizeof(s->screenshot_path),"%.4000s/screenshots/cockpit-%llu.webp",s->root,(unsigned long long)SDL_GetTicks64());s->screenshot_requested=true;break;
 default:break;
 }
 return true;
}
static const char *backend_label(Backend backend){
 switch(backend){
 case CD_BACKEND_HEADLESS:return "headless/EGL/VNC";
 case CD_BACKEND_WINDOWED:return "windowed/SDL/GLX";
 case CD_BACKEND_FBDEV:return "fbdev";
 case CD_BACKEND_DRM:return "DRM/KMS + GBM/EGL";
 default:return "unknown";
 }
}
static void backend_poll(Server *s){
 backend_vt_service(s);
 if(!s->running||backend_vt_is_suspended(s))return;
 switch(s->backend){
 case CD_BACKEND_HEADLESS:backend_headless_poll(s);break;
 case CD_BACKEND_WINDOWED:backend_sdl_poll(s);break;
 case CD_BACKEND_FBDEV:backend_fbdev_poll(s);break;
 case CD_BACKEND_DRM:backend_drm_poll(s);break;
 }
 /* A signal may have arrived while evdev/DRM events were being drained. */
 backend_vt_service(s);
}
static PresentResult backend_present(Server *s){
 PresentResult result=CD_PRESENT_OK;
 switch(s->backend){
 case CD_BACKEND_HEADLESS:backend_headless_present(s);break;
 case CD_BACKEND_WINDOWED:backend_sdl_present(s);break;
 case CD_BACKEND_FBDEV:backend_fbdev_present(s);break;
 case CD_BACKEND_DRM:result=backend_drm_present(s);break;
 }
 /* A VT-aware page-flip wait returns before handling the signal, avoiding
  * recursive release from inside the wait. Service it before frame_done. */
 backend_vt_service(s);return result;
}
static void backend_shutdown(Server *s){
 switch(s->backend){
 case CD_BACKEND_HEADLESS:backend_headless_shutdown(s);break;
 case CD_BACKEND_WINDOWED:backend_sdl_shutdown(s);break;
 case CD_BACKEND_FBDEV:backend_fbdev_shutdown(s);break;
 case CD_BACKEND_DRM:backend_drm_shutdown(s);break;
 }
}
static void usage(const char *name){printf("ChromeDrive " CD_VERSION " \xe2\x80\x94 native, first-person Wayland cockpit\n\nUsage: %s [options]\n  --demo                Start the configured autostart Wayland clients\n  --socket NAME         Wayland socket (default: automatic)\n  --exec COMMAND        Launch an extra Wayland client (repeatable)\n  --size WIDTHxHEIGHT   Framebuffer/host window size (default 1600x1000)\n  --fps N               Frame cap, 1..120 (default 60)\n  --backend MODE        headless (default) | windowed | fbdev | drm\n  --headless            Alias for --backend headless: surfaceless EGL + built-in VNC\n  --windowed            Alias for --backend windowed: SDL2/GLX dev window on X\n  --fbdev [DEVICE]      Direct Linux framebuffer output (default /dev/fb0)\n  --fbdev-device PATH   Framebuffer device for --backend fbdev\n  --drm [DEVICE]        Direct DRM/KMS + GBM/EGL output (default /dev/dri/card0)\n  --drm-device PATH     KMS card device for --backend drm\n  --drm-mode WxH        Require an exact KMS mode; default current or preferred\n  --evdev / --no-evdev  Enable/disable local input for fbdev and DRM\n  --grab-input          EVIOCGRAB local devices (DRM needs managed VT or downgrades)\n  --input-device PATH   Restrict local input (repeatable, max 8)\n  --fbdev-modeset       Select fbdev backend and request modeset to configured render size\n  --fbdev-mode WxH[@BPP] Select fbdev backend, imply modeset, set size/BPP (BPP in 16,24,32)\n  --no-fbdev-vsync      Do not wait for FBIO_WAITFORVSYNC\n  --no-fbdev-pan        Do not use FBIOPAN_DISPLAY double buffering\n  --fbdev-vnc-mirror    Loopback-only RFB input/output companion for fbdev\n  --vnc-mirror          Alias for --fbdev-vnc-mirror\n  --fbdev-evdev         Enable local evdev input for direct backends (default: on)\n  --no-fbdev-evdev      Disable local evdev input for direct backends\n  --fbdev-grab-input    Grab evdev devices (fbdev VT lockout warning; DRM managed VT)\n  --fbdev-input-device PATH Restrict evdev to explicit device (repeatable, max 8)\n  --vnc-port N          VNC port for headless or the fbdev mirror (default 5900)\n  --fullscreen          --windowed only: start fullscreen, F11 to leave\n  --exit-after SECONDS   Useful for automated checks\n  --screenshot PATH     Capture a WebP after 3 seconds\n  --config PATH         Config file (default: ~/.config/chromedrive/chromedrive.conf)\n  --no-config           Ignore the config file entirely\n  --help                Show this help\n\nfbdev adopts the current kernel framebuffer mode by default (no modesetting);\nwith --fbdev-modeset or --fbdev-mode it requests a modeset via FBIOPUT_VSCREENINFO.\nThe original mode is restored on exit. Local evdev input (keyboard/mouse/touchscreen)\nis enabled by default for fbdev and DRM; use --no-evdev to disable. Explicit devices\nvia --input-device replace auto-enumeration. Startup-only enumeration; no hotplug.\nDRM/KMS uses direct GBM/EGL scanout with page-flip events (no readback blit).\nIt retains the current mode or chooses the preferred one; --drm-mode requires an exact mode.\nOn a controlling Linux VT, physical Ctrl+Alt+F1..F12 releases input/DRM master and resumes on return.\nWithout a managed VT, DRM downgrades input grabbing; fbdev VT switching remains unmanaged.\nLocal evdev is shared by fbdev and DRM, with monotonic kernel timestamps and a bounded async queue.\n\nF1 help | F2 selected seat | F3 focus/next | F4 neon | F6 arrange | Meta+H HUD | Meta+O doors\nF7 client/drive input | F8 driver/exterior/systems | F9 switch seat | F10 recover car\nMeta+Shift+Q quit | red dash EXIT button: press twice\nF5 is unbound for browser/client restart or reload\nDrive: WASD/arrows + Space + drag wheel\nMiddle-drag/VNC-drag to look | Drag a titlebar to move | HANDLES shows the Arrange gizmo\nArrange: Tab next window, Esc/F6 exit | Ctrl+Alt+Enter new terminal\n",name);}
int main(int argc,char **argv){
 Server s={.width=1600,.height=1000,.fps_limit=60,.running=true,.hud_visible=true,.hover_button=-1,.hover_close=-1,.keymap_fd=-1,.arrange_slot=-1,.frame_drag_slot=-1,.fov=65,.target_fov=65,.base_fov=65,.backend=CD_BACKEND_HEADLESS,.fbdev_vsync=true,.fbdev_pan=true,.fbdev_evdev=true,.fbdev_grab_input=false,.vnc_port=5900,.telemetry_fd=-1,.fbdev_modeset=false,.fbdev_mode_width=0,.fbdev_mode_height=0,.fbdev_mode_bpp=0,.fbdev_input_device_count=0,.render_to_texture=true,.render_scale=1,.render_mag_filter=GL_LINEAR,.render_min_filter=GL_LINEAR_MIPMAP_LINEAR};
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
  else if(!strcmp(argv[i],"--backend")&&i+1<argc){if(!parse_backend(argv[++i],&s.backend)){fprintf(stderr,"Invalid backend '%s' (want headless|windowed|fbdev|drm)\n",argv[i]);return 2;}}
  else if(!strcmp(argv[i],"--headless"))s.backend=CD_BACKEND_HEADLESS;
  else if(!strcmp(argv[i],"--windowed"))s.backend=CD_BACKEND_WINDOWED;
  else if(!strcmp(argv[i],"--fbdev")){s.backend=CD_BACKEND_FBDEV;if(i+1<argc&&argv[i+1][0]!='-'&&!set_fbdev_path(&s,argv[++i]))return 2;}
  else if(!strcmp(argv[i],"--fbdev-device")&&i+1<argc){s.backend=CD_BACKEND_FBDEV;if(!set_fbdev_path(&s,argv[++i]))return 2;}
  else if(!strcmp(argv[i],"--drm")){s.backend=CD_BACKEND_DRM;if(i+1<argc&&argv[i+1][0]!='-'&&!set_drm_path(&s,argv[++i]))return 2;}
  else if(!strcmp(argv[i],"--drm-device")&&i+1<argc){s.backend=CD_BACKEND_DRM;if(!set_drm_path(&s,argv[++i]))return 2;}
  else if(!strcmp(argv[i],"--drm-mode")&&i+1<argc){
   int mw,mh,mbpp;
   if(!cd_fb_mode_parse(argv[++i],&mw,&mh,&mbpp)||mbpp!=0){fprintf(stderr,"Invalid DRM mode '%s' (use WIDTHxHEIGHT)\n",argv[i]);return 2;}
   s.drm_mode_width=mw;s.drm_mode_height=mh;s.backend=CD_BACKEND_DRM;
  }
  else if(!strcmp(argv[i],"--no-fbdev-vsync"))s.fbdev_vsync=false;
   else if(!strcmp(argv[i],"--no-fbdev-pan"))s.fbdev_pan=false;
   else if(!strcmp(argv[i],"--fbdev-evdev")||!strcmp(argv[i],"--evdev"))s.fbdev_evdev=true;
   else if(!strcmp(argv[i],"--no-fbdev-evdev")||!strcmp(argv[i],"--no-evdev"))s.fbdev_evdev=false;
   else if(!strcmp(argv[i],"--fbdev-grab-input")||!strcmp(argv[i],"--grab-input"))s.fbdev_grab_input=true;
   else if((!strcmp(argv[i],"--fbdev-input-device")||!strcmp(argv[i],"--input-device"))&&i+1<argc){
    const char *path=argv[++i]; /* consume the path even when the limit is reached */
    if(s.fbdev_input_device_count<8){
     snprintf(s.fbdev_input_devices[s.fbdev_input_device_count],sizeof(s.fbdev_input_devices[0]),"%s",path);
     s.fbdev_input_device_count++;
    }else fprintf(stderr,"[cli] too many input devices (max 8): %s\n",path);
   }
 else if(!strcmp(argv[i],"--fbdev-vnc-mirror")||!strcmp(argv[i],"--vnc-mirror"))s.fbdev_vnc_mirror=true;
    else if(!strcmp(argv[i],"--fbdev-modeset")){s.fbdev_modeset=true;s.backend=CD_BACKEND_FBDEV;}
   else if(!strcmp(argv[i],"--fbdev-mode")&&i+1<argc){
    int mw,mh,mbpp;
    if(!cd_fb_mode_parse(argv[++i],&mw,&mh,&mbpp)){fprintf(stderr,"Invalid fbdev mode '%s' (use WIDTHxHEIGHT[@BPP])\n",argv[i]);return 2;}
    s.fbdev_mode_width=mw;s.fbdev_mode_height=mh;s.fbdev_mode_bpp=mbpp;s.fbdev_modeset=true;s.backend=CD_BACKEND_FBDEV;
   }
   else if(!strcmp(argv[i],"--vnc-port")&&i+1<argc){int p=atoi(argv[++i]);if(p<1||p>65535){fprintf(stderr,"Invalid VNC port\n");return 2;}s.vnc_port=p;}
  else if(!strcmp(argv[i],"--socket")&&i+1<argc)socket_name=argv[++i];
  else if(!strcmp(argv[i],"--exec")&&i+1<argc&&s.exec_count<16)s.exec_commands[s.exec_count++]=argv[++i];
  else if(!strcmp(argv[i],"--size")&&i+1<argc){if(sscanf(argv[++i],"%dx%d",&s.width,&s.height)!=2||s.width<640||s.height<400||s.width>7680||s.height>4320){fprintf(stderr,"Invalid window size\n");return 2;}}
  else if(!strcmp(argv[i],"--fps")&&i+1<argc)s.fps_limit=(int)clampf(atoi(argv[++i]),1,120);
  else if(!strcmp(argv[i],"--exit-after")&&i+1<argc)exit_after=atof(argv[++i]);
  else if(!strcmp(argv[i],"--screenshot")&&i+1<argc){snprintf(s.screenshot_path,sizeof(s.screenshot_path),"%s",argv[++i]);auto_screenshot=true;}
  else{fprintf(stderr,"Unknown or incomplete option: %s\n",argv[i]);usage(argv[0]);return 2;}
 }
 if((s.backend==CD_BACKEND_FBDEV||s.backend==CD_BACKEND_DRM)&&s.fullscreen){fprintf(stderr,"[cli] --fullscreen has no meaning for direct display output; ignoring\n");s.fullscreen=false;}
 if(s.backend!=CD_BACKEND_FBDEV&&s.fbdev_vnc_mirror)fprintf(stderr,"[vnc] --fbdev-vnc-mirror only applies to the fbdev backend; ignoring\n");
 s.mouse_x=s.width/2;s.mouse_y=s.height/2; /* start the cursor centered, not pinned to the top-left corner at (0,0) */
 ssize_t n=readlink("/proc/self/exe",s.root,sizeof(s.root)-1);if(n<0)return 1;s.root[n]=0;char *slash=strrchr(s.root,'/');if(slash)*slash=0;slash=strrchr(s.root,'/');if(slash)*slash=0;
 /* Catalog lives at <root>/assets/locale, so this waits for s.root. Runs
  * before ui_init() below, so the first bake is already in the right language. */
 apply_language(&s,true);
 if(!getenv("XDG_RUNTIME_DIR")){snprintf(s.runtime,sizeof(s.runtime),"/tmp/chromedrive-%d",(int)getuid());if(mkdir(s.runtime,0700)<0&&errno!=EEXIST){perror("runtime directory");return 1;}struct stat st;if(stat(s.runtime,&st)<0||st.st_uid!=getuid()||(st.st_mode&0077)){fprintf(stderr,"Runtime directory must be private and owned by you\n");return 1;}setenv("XDG_RUNTIME_DIR",s.runtime,1);}else snprintf(s.runtime,sizeof(s.runtime),"%s",getenv("XDG_RUNTIME_DIR"));
 signal(SIGINT,on_signal);signal(SIGTERM,on_signal);signal(SIGPIPE,SIG_IGN);
 /* signalfd is process-wide only if these signals are blocked before SDL or
  * any evdev thread can be created. backend_vt_init() is intentionally later. */
 if(s.backend==CD_BACKEND_DRM)backend_vt_block_signals();
 if(SDL_Init(SDL_INIT_TIMER)<0){fprintf(stderr,"SDL: %s\n",SDL_GetError());return 1;} /* timer only: used for clock_now()/frame pacing in both backends */
 bool backend_ok=false;
 switch(s.backend){
 case CD_BACKEND_HEADLESS:backend_ok=backend_headless_init(&s);break;
 case CD_BACKEND_WINDOWED:backend_ok=backend_sdl_init(&s);break;
 case CD_BACKEND_FBDEV:backend_ok=backend_fbdev_init(&s);break;
 case CD_BACKEND_DRM:backend_ok=backend_drm_init(&s);break;
 }
 if(!backend_ok){fprintf(stderr,"Backend init failed (%s)\n",backend_label(s.backend));SDL_Quit();return 1;}
 dmabuf_probe(&s);
 if(!server_init(&s,socket_name)){
  fprintf(stderr,"Could not start Wayland server: %s\n",strerror(errno));
  /* The direct-input worker may already be running even though no frame was
   * dispatched. Stop it before partial Wayland/xkb teardown, as on normal exit,
   * then drop the still-current presentation backend after server_finish. */
  if(s.evdev_state)backend_evdev_shutdown(&s);
  server_finish(&s);backend_shutdown(&s);SDL_Quit();return 1;
 }
 telemetry_init(&s);
 /* Only after opening the host window/backend do we replace WAYLAND_DISPLAY. */
 /* Built-in menu defaults (%root% templates - the same strings the config
  * GUI shows as the editable starting point) + per-slot app_id/autostart.
  * [app.*] config (applied after ui_init below) overrides; env vars win. */
 const char *templates[4]={"foot --config='%root%/assets/foot.ini' --title='Terminal' /bin/sh '%root%/assets/terminal-welcome.sh'","'%root%/bin/chromedrive-demo' files","'%root%/bin/chromedrive-demo' audio","'%root%/bin/chromedrive-demo' cluster"};
 const char *def_ids[4]={"foot","chromedrive.files","chromedrive.audio","chromedrive.cluster"};
 const bool def_autostart[4]={true,true,true,false};
 const char *envnames[]={"CHROMEDRIVE_TERMINAL","CHROMEDRIVE_FILES","CHROMEDRIVE_AUDIO","CHROMEDRIVE_METERS"};
 for(int i=0;i<6;i++){if(i<4){s.launch_commands[i]=getenv(envnames[i])?strdup(getenv(envnames[i])):expand_root(templates[i],s.root);snprintf(s.app_ids[i],sizeof(s.app_ids[i]),"%s",def_ids[i]);s.app_autostart[i]=def_autostart[i];}else{s.launch_commands[i]=NULL;s.app_ids[i][0]=0;s.app_autostart[i]=false;}}
 scene_init(&s);ui_init(&s);
 if(config_enabled){CdConfig *cfg=cd_config_load(config_path);if(cfg){apply_app_config(&s,cfg);cd_config_free(cfg);}} /* needs buttons+GL: after ui_init */
 s.start_time=clock_now();double last=s.start_time,fps_start=last;uint64_t fps_frames=0;bool initial_focus=false;
 /* The instrument cluster is part of the car, not an optional floating app.
  * Skip a duplicate if a configured autostart slot also names that app_id. */
 char cluster_command[4400];snprintf(cluster_command,sizeof(cluster_command),"'%s/bin/chromedrive-demo' cluster",s.root);app_exec(&s,cluster_command);
 if(s.demo){for(int i=0;i<6;i++)if(s.buttons[i].enabled&&s.buttons[i].action==CD_BUTTON_APP&&s.app_autostart[i]&&s.launch_commands[i]&&!strstr(s.app_ids[i],"chromedrive.cluster"))app_exec(&s,s.launch_commands[i]);}for(int i=0;i<s.exec_count;i++)app_exec(&s,s.exec_commands[i]);
 app_notice(&s,tr("Welcome to the driver's seat. F1 for controls."));
 while(s.running&&!interrupted){
  double frame_start=clock_now();s.time=frame_start-s.start_time;s.dt=frame_start-last;last=frame_start;
  backend_poll(&s);
  if(!s.running)break;
  if(backend_vt_is_suspended(&s)){
   /* Wayland dispatch, physics, GL and frame callbacks all pause with the
    * session. A finite signalfd wait still lets SIGINT/SIGTERM leave. */
   backend_vt_wait(&s,50);continue;
  }
  config_poll(&s);
  server_dispatch(&s);
  int status;pid_t pid;while((pid=waitpid(-1,&status,WNOHANG))>0){for(int i=0;i<s.child_count;i++)if(s.children[i]==pid){s.children[i]=s.children[--s.child_count];break;}if(WIFEXITED(status)&&WEXITSTATUS(status)!=0){fprintf(stderr,"[launch] pid %d exited %d\n",(int)pid,WEXITSTATUS(status));app_notice(&s,tr("Client exited with status %d — see launch log"),WEXITSTATUS(status));}}
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
  /* Publish the new physics state before presentation wakes frame-callback
   * clients; the cluster then draws one coherent, current snapshot. */
  telemetry_update(&s);scene_update_mounts(&s);
  static bool collision_seen;if(s.vehicle.collision&&!collision_seen)app_notice(&s,tr("Hover bumper absorbed a garage wall contact"));collision_seen=s.vehicle.collision;
  if(!s.wheel_drag)s.wheel_client_angle+=(0-s.wheel_client_angle)*(1.f-expf(-(float)s.dt*9.f));
  float blend=1.f-expf(-(float)s.dt*12.f);s.yaw+=(s.target_yaw-s.yaw)*blend;s.pitch+=(s.target_pitch-s.pitch)*blend;s.fov+=(s.target_fov-s.fov)*blend;
  Hit hover=scene_pick(&s,s.mouse_x,s.mouse_y,false);s.hover_button=hover.kind==HIT_BUTTON?hover.button:-1;s.hover_close=hover.kind==HIT_CLOSE?hover.slot:-1;s.cog_hover=hover.kind==HIT_COG;s.quit_hover=hover.kind==HIT_QUIT;s.hud_hover=hover.kind==HIT_HUD;s.wheel_hover=hover.kind==HIT_WHEEL;
  s.cog_angle+=((s.time<s.cog_spin_until)?540.f:s.cog_hover?160.f:20.f)*(float)s.dt;if(s.cog_angle>=360.f)s.cog_angle-=360.f;
  /* One thread owns the current GL context for the entire frame: UI texture
   * regeneration, vehicle/cockpit geometry, and Wayland surface textures are
   * rendered into one FBO. No web worker or physics callback touches GL. */
  ui_update(&s);
  bool rtt=scene_rtt_begin(&s);
  scene_render(&s);
  if(rtt)scene_rtt_end(&s);
  else{glBindFramebuffer(GL_FRAMEBUFFER,s.target_fbo);glViewport(0,0,s.width,s.height);}
  /* Keep text and pointer pixels sharp: only the 3D world uses render_scale;
   * the HUD is composited after the offscreen sample at output resolution. */
  ui_draw_overlay(&s);
  if(auto_screenshot&&s.time>3){s.screenshot_requested=true;auto_screenshot=false;}if(s.screenshot_requested){scene_save_screenshot(&s,s.screenshot_path);s.screenshot_requested=false;app_notice(&s,tr("Screenshot saved"));}
  PresentResult presented=backend_present(&s);
  /* Frame callback render loop pattern:
   * Only AFTER scene_render() and backend_*_present() have sampled the newly
   * committed Wayland textures and pushed the finished frame to RFB/SDL—or
   * completed the DRM page-flip event—do we fire wl_callback.done(compositor_ms)
   * via server_frame_done() and flush the client sockets. This paces Wayland
   * clients to actual display consumption rather than allowing them to spin in
   * an unthrottled render loop. A VT-interrupted/failed present is not a
   * completed frame and therefore cannot release waiting client callbacks. */
  if(s.running&&presented==CD_PRESENT_OK&&!backend_vt_is_suspended(&s)){
   server_frame_done(&s,(uint32_t)(clock_now()*1000));wl_display_flush_clients(s.display);s.frames++;fps_frames++;
  }
  if(frame_start-fps_start>.5){s.fps=fps_frames/(frame_start-fps_start);fps_frames=0;fps_start=frame_start;}
  if(exit_after>0&&s.time>=exit_after)s.running=false;
  double remain=1.0/s.fps_limit-(clock_now()-frame_start);if(remain>0)SDL_Delay((uint32_t)(remain*1000));
 }
 for(int i=0;i<s.child_count;i++)kill(-s.children[i],SIGTERM);
 /* Direct-input release needs live Wayland/xkb state. Stop that worker and
  * balance keys/touches before server_finish; backend_*_shutdown is idempotent
  * here and later tears down presentation after GL objects are gone. */
 if(s.evdev_state)backend_evdev_shutdown(&s);
 telemetry_finish(&s);server_finish(&s);scene_finish(&s);
 backend_shutdown(&s);
 SDL_Quit();for(int i=0;i<6;i++)free(s.launch_commands[i]);return 0;
}
