/* Embedded RFB server shared by the headless backend and the optional fbdev
 * mirror. RFB input callbacks drive the same server_* input API as SDL.
 *
 * The headless backend keeps its historical 0.0.0.0 bind. The fbdev mirror is
 * explicit opt-in and loopback-only because RFB here has no authentication. */
#include "chrome.h"
#include <rfb/rfb.h>
#include <rfb/keysym.h>
#include <arpa/inet.h>
#include <linux/input-event-codes.h>
#include <stdlib.h>
#include <string.h>

#define CD_VNC_LEFT 1
#define CD_VNC_MIDDLE 2
#define CD_VNC_RIGHT 4
#define CD_VNC_WHEEL_UP 8
#define CD_VNC_WHEEL_DOWN 16

void cd_vnc_ptr(int mask,int x,int y,rfbClientPtr cl){
 Server *s=cl->screen->screenData;uint32_t now=SDL_GetTicks();
 bool middle=(mask&CD_VNC_MIDDLE)!=0,prev_middle=(s->vnc_prev_mask&CD_VNC_MIDDLE)!=0;
 bool left=(mask&CD_VNC_LEFT)!=0,prev_left=(s->vnc_prev_mask&CD_VNC_LEFT)!=0;
 bool right=(mask&CD_VNC_RIGHT)!=0,prev_right=(s->vnc_prev_mask&CD_VNC_RIGHT)!=0;
 if(x<0||y<0||x>=s->width||y>=s->height){
  /* VNC has no separate release event: a button-up outside the framebuffer
   * must still end grabs/drags instead of being silently consumed. Never
   * synthesize presses here, only releases/cancels. */
  if(prev_middle&&!middle){s->middle_look=false;if(s->client_input_lock)server_pointer_button(s,BTN_MIDDLE,false,now);}
  if(prev_left&&!left)server_pointer_button(s,BTN_LEFT,false,now);
  if(prev_right&&!right)server_pointer_button(s,BTN_RIGHT,false,now);
  if(s->arrange_dragging&&((s->arrange_drag_button==BTN_LEFT&&!left)||(s->arrange_drag_button==BTN_RIGHT&&!right)))
   app_arrange_pointer_button(s,(Hit){0},(uint32_t)s->arrange_drag_button,false);
  if(s->frame_dragging&&!left)app_frame_pointer_button(s,(Hit){0},BTN_LEFT,false);
  if(!left&&!right&&!middle){
   s->middle_look=false;s->arrange_dragging=false;s->arrange_drag_button=0;s->arrange_handle=CD_HANDLE_NONE;
   s->frame_dragging=false;s->frame_drag_slot=-1;server_release_pointer_buttons(s,now);
  }
  s->vnc_prev_mask=mask;return;
 }
 s->mouse_x=x;s->mouse_y=y;
 if(s->wheel_drag){
  if(left)scene_wheel_motion(s,x,y,now);else scene_wheel_button(s,(Hit){0},x,y,false,now);
  s->vnc_prev_mask=mask;s->vnc_prev_x=x;s->vnc_prev_y=y;return;
 }
 if(middle&&!s->client_input_lock){
  if(!prev_middle){s->middle_look=true;server_pointer_motion(s,(Hit){0},now);}
  else{s->target_yaw-=(x-s->vnc_prev_x)*.5f;s->target_pitch=clampf(s->target_pitch-(y-s->vnc_prev_y)*.4f,-35,32);s->focus_view=false;s->arrange_last_x=x;s->arrange_last_y=y;s->frame_last_x=x;s->frame_last_y=y;}
 }else{
  if(prev_middle)s->middle_look=false;
  Hit hit=scene_pick(s,x,y,s->pointer_grab!=NULL);
  bool wu=(mask&CD_VNC_WHEEL_UP)&&!(s->vnc_prev_mask&CD_VNC_WHEEL_UP);
  bool wd=(mask&CD_VNC_WHEEL_DOWN)&&!(s->vnc_prev_mask&CD_VNC_WHEEL_DOWN);
  if(s->arrange_mode){
   if(left!=prev_left)app_arrange_pointer_button(s,hit,BTN_LEFT,left);
   if(right!=prev_right)app_arrange_pointer_button(s,hit,BTN_RIGHT,right);
   if(left||right)app_arrange_pointer_motion(s,x,y);
   if(wu||wd)app_arrange_pointer_wheel(s,wu?1.f:-1.f);
  }else if(s->frame_dragging||((hit.kind==HIT_FRAME||hit.kind==HIT_HANDLES_BUTTON)&&left!=prev_left)){
   if(left!=prev_left)app_frame_pointer_button(s,hit,BTN_LEFT,left);
   if(left&&s->frame_dragging)app_frame_pointer_motion(s,x,y);
  }else{
   server_pointer_motion(s,hit,now);
   if(s->client_input_lock&&middle!=prev_middle&&(hit.kind==HIT_APP||s->pointer_grab))server_pointer_button(s,BTN_MIDDLE,middle,now);
   if(left&&!prev_left&&hit.kind==HIT_WHEEL)scene_wheel_button(s,hit,x,y,true,now);
   else if(left&&!prev_left&&hit.kind==HIT_BUTTON)app_launch(s,hit.button,false);
   else if(left&&!prev_left&&hit.kind==HIT_COG)app_open_settings(s);
   else if(left&&!prev_left&&hit.kind==HIT_QUIT)app_request_quit(s,false);
   else if(left&&!prev_left&&hit.kind==HIT_HUD)app_toggle_hud(s);
   else if(left&&!prev_left&&hit.kind==HIT_CLOSE)server_close_surface(hit.surface);
   else if(left!=prev_left&&(hit.kind==HIT_APP||s->pointer_grab))server_pointer_button(s,BTN_LEFT,left,now);
   if(right&&!prev_right&&hit.kind==HIT_BUTTON)app_open_settings_slot(s,hit.button,true);
   else if(right!=prev_right&&(hit.kind==HIT_APP||s->pointer_grab))server_pointer_button(s,BTN_RIGHT,right,now);
   if(wu||wd){if(hit.kind==HIT_APP)server_pointer_axis(s,0,wu?-3:3,now);else s->target_fov=clampf(s->target_fov-(wu?3:-3),CD_DRIVER_FOV_MIN,CD_DRIVER_SCROLL_FOV_MAX);}
  }
 }
 s->vnc_prev_mask=mask;s->vnc_prev_x=x;s->vnc_prev_y=y;
}
void cd_vnc_key(rfbBool down,rfbKeySym key,rfbClientPtr cl){
 Server *s=cl->screen->screenData;uint32_t evdev=keysym_to_evdev((uint32_t)key);if(!evdev)return;
 uint32_t now=SDL_GetTicks();
 if(!app_global_key(s,evdev,down))server_key(s,evdev,down,now);
}

static enum rfbNewClientAction cd_vnc_new_client(rfbClientPtr cl){
 Server *s=cl->screen->screenData;
 /* Dirty tiles are deltas against the previous server framebuffer. A new
  * client has no such history, so its first update must remain a full frame. */
 rfbMarkRectAsModified(cl->screen,0,0,s->width,s->height);
 return RFB_CLIENT_ACCEPT;
}

bool backend_vnc_init(Server *s,bool loopback_only){
 if(s->vnc_screen)return true;
 if(s->vnc_port<1||s->vnc_port>65535){fprintf(stderr,"[vnc] invalid port %d\n",s->vnc_port);return false;}
 int fake_argc=0;
 rfbScreenInfoPtr screen=rfbGetScreen(&fake_argc,NULL,s->width,s->height,8,3,4);
 if(!screen){fprintf(stderr,"[vnc] rfbGetScreen failed\n");return false;}
 screen->desktopName="ChromeDrive cockpit";
 screen->frameBuffer=(char*)malloc((size_t)s->width*(size_t)s->height*4);
 if(!screen->frameBuffer){fprintf(stderr,"[vnc] out of memory for VNC framebuffer\n");rfbScreenCleanup(screen);return false;}
 memset(screen->frameBuffer,0,(size_t)s->width*(size_t)s->height*4);
 screen->alwaysShared=TRUE;
 screen->port=s->vnc_port;
 screen->ipv6port=-1; /* IPv4 only keeps the sandbox preview proxy simple */
 screen->listenInterface=htonl(loopback_only?INADDR_LOOPBACK:INADDR_ANY);
 screen->screenData=s;
 extern void cd_vnc_ptr(int,int,int,rfbClientPtr);
 extern void cd_vnc_key(rfbBool,rfbKeySym,rfbClientPtr);
 screen->ptrAddEvent=cd_vnc_ptr;
 screen->kbdAddEvent=cd_vnc_key;
 screen->newClientHook=cd_vnc_new_client;
 rfbInitServer(screen);
 if(screen->listenSock==RFB_INVALID_SOCKET){
  fprintf(stderr,"[vnc] cannot listen on %s:%d\n",loopback_only?"127.0.0.1":"0.0.0.0",s->vnc_port);
  free(screen->frameBuffer);rfbScreenCleanup(screen);return false;
 }
 s->vnc_screen=screen;
 fprintf(stderr,"[vnc] VNC server listening on %s:%d%s\n",loopback_only?"127.0.0.1":"0.0.0.0",s->vnc_port,
  loopback_only?" (loopback-only fbdev mirror)":" — bridge with websockify+noVNC for a browser preview (see tools/viewer)");
 return true;
}

void backend_vnc_poll(Server *s){
 if(s->vnc_screen)rfbProcessEvents((rfbScreenInfoPtr)s->vnc_screen,0);
}

bool backend_vnc_has_clients(Server *s){
 if(!s||!s->vnc_screen)return false;
 rfbScreenInfoPtr screen=(rfbScreenInfoPtr)s->vnc_screen;
 return screen->clientHead!=NULL;
}

void backend_vnc_present_pixels(Server *s,const uint8_t *pixels){
 if(!s->vnc_screen||!pixels)return;
 rfbScreenInfoPtr screen=(rfbScreenInfoPtr)s->vnc_screen;
 const int tile=128,stride=s->width*4;
 /* Compare in top-down RFB space while the GL image is still bottom-up.
  * Copying and marking only changed 128x128 tiles avoids pushing a 6.4 MB raw
  * frame for a stationary cockpit with a few animated instruments/raindrops.
  * libvncserver coalesces intersecting tiles in each client's update region. */
 for(int y=0;y<s->height;y+=tile)for(int x=0;x<s->width;x+=tile){
  int tw=x+tile<=s->width?tile:s->width-x,th=y+tile<=s->height?tile:s->height-y;
  bool changed=false;
  for(int row=0;row<th&&!changed;row++){
   const uint8_t *src=pixels+(size_t)(s->height-1-y-row)*stride+(size_t)x*4;
   const uint8_t *dst=(const uint8_t*)screen->frameBuffer+(size_t)(y+row)*stride+(size_t)x*4;
   changed=memcmp(src,dst,(size_t)tw*4)!=0;
  }
  if(!changed)continue;
  for(int row=0;row<th;row++){
   const uint8_t *src=pixels+(size_t)(s->height-1-y-row)*stride+(size_t)x*4;
   uint8_t *dst=(uint8_t*)screen->frameBuffer+(size_t)(y+row)*stride+(size_t)x*4;
   memcpy(dst,src,(size_t)tw*4);
  }
  rfbMarkRectAsModified(screen,x,y,tw,th);
 }
}

void backend_vnc_present_readback(Server *s){backend_vnc_present_pixels(s,s->readback_pixels);}

void backend_vnc_shutdown(Server *s){
 if(s->vnc_screen){rfbScreenInfoPtr screen=(rfbScreenInfoPtr)s->vnc_screen;rfbShutdownServer(screen,TRUE);free(screen->frameBuffer);rfbScreenCleanup(screen);s->vnc_screen=NULL;}
}
