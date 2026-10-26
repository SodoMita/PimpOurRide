#include "chrome.h"

/* Complete one compositor-clock iteration after its scene/present attempt.
 * PresentResult describes output progress only: a busy DRM scanout must not
 * revoke Wayland render credit. The one-shot callback committed by each
 * surface and main's fps_limit remain the client backpressure mechanism. */
bool server_complete_frame(Server *s,PresentResult presented,uint32_t time){
 if(!s->running||backend_vt_is_suspended(s))return false;
 server_frame_done(s,time);
 wl_display_flush_clients(s->display);
 if(presented==CD_PRESENT_OK)s->frames++;
 return true;
}
