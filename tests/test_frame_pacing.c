#include "chrome.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond) do { if(!(cond)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#cond);failures++;} } while(0)

static bool suspended;
static int done_calls,flush_calls;
static uint32_t last_time;
static Server *last_server;

bool backend_vt_is_suspended(Server *s){(void)s;return suspended;}
void server_frame_done(Server *s,uint32_t time){done_calls++;last_server=s;last_time=time;}
void wl_display_flush_clients(struct wl_display *display){CHECK(display!=NULL);flush_calls++;}

int main(void){
 Server s;memset(&s,0,sizeof(s));s.running=true;s.display=(struct wl_display *)(uintptr_t)1;s.frames=10;
 CHECK(server_complete_frame(&s,CD_PRESENT_OK,100));
 CHECK(done_calls==1&&flush_calls==1&&s.frames==11);
 CHECK(last_server==&s&&last_time==100);
 /* These are the regression cases: physical scanout backpressure or a VT
  * interrupt must not revoke client render credit while the session runs. */
 CHECK(server_complete_frame(&s,CD_PRESENT_SKIPPED,101));
 CHECK(done_calls==2&&flush_calls==2&&s.frames==11&&last_time==101);
 CHECK(server_complete_frame(&s,CD_PRESENT_INTERRUPTED,102));
 CHECK(done_calls==3&&flush_calls==3&&s.frames==11&&last_time==102);
 suspended=true;
 CHECK(!server_complete_frame(&s,CD_PRESENT_OK,103));
 CHECK(done_calls==3&&flush_calls==3&&s.frames==11);
 suspended=false;s.running=false;
 CHECK(!server_complete_frame(&s,CD_PRESENT_OK,104));
 CHECK(done_calls==3&&flush_calls==3&&s.frames==11);
 if(failures){fprintf(stderr,"test_frame_pacing: %d failure(s)\n",failures);return 1;}
 puts("test_frame_pacing: ok");return 0;
}
