/* Default production backend: no X server anywhere.
 *  - GL context/render target: backend_egl.c creates the shared surfaceless
 *    EGL context and explicit FBO used by both EGL backends.
 *  - Display output: an embedded libvncserver RFB server (backend_vnc.c).
 *    Pixel-pack buffers and fences pipeline readback with the next frame on
 *    drivers that support them; older drivers keep the synchronous fallback.
 *    Browsers reach the server via websockify+noVNC (tools/viewer).
 *  - Input: RFB pointer/keyboard callbacks drive the same server_* input API
 *    as the SDL backend, so behavior (picking, focus, hotkeys) matches.
 *  - Single-thread GL & teardown invariants: all GL work stays on the main
 *    compositor thread, and backend_egl_shutdown deletes FBO/renderbuffers
 *    while the context is still current before EGL termination.
 *  The EGL backends (headless and fbdev) support zwp_linux_dmabuf_v1 because
 *  EGLImage only interoperates with an EGL-created GL context (see dmabuf.c).
 */
#include "chrome.h"
#include <stdlib.h>

#define CD_HEADLESS_PBO_COUNT 3

typedef struct {
 GLuint pbo[CD_HEADLESS_PBO_COUNT];
 GLsync fence[CD_HEADLESS_PBO_COUNT];
 uint64_t serial[CD_HEADLESS_PBO_COUNT],next_serial;
 bool pending[CD_HEADLESS_PBO_COUNT],async_readback;
 size_t bytes;
} HeadlessState;

static HeadlessState *headless_state(Server *s){return s?(HeadlessState*)s->headless_state:NULL;}

static void headless_release_buffers(HeadlessState *st){
 if(!st)return;
 if(glBindBuffer)glBindBuffer(GL_PIXEL_PACK_BUFFER,0);
 for(int i=0;i<CD_HEADLESS_PBO_COUNT;i++){
  if(st->fence[i]&&glDeleteSync)glDeleteSync(st->fence[i]);
  st->fence[i]=NULL;st->pending[i]=false;st->serial[i]=0;
 }
 if(st->pbo[0]&&glDeleteBuffers)glDeleteBuffers(CD_HEADLESS_PBO_COUNT,st->pbo);
 for(int i=0;i<CD_HEADLESS_PBO_COUNT;i++)st->pbo[i]=0;
 st->async_readback=false;
}

static void headless_init_readback(Server *s){
 HeadlessState *st=calloc(1,sizeof(*st));
 if(!st){fprintf(stderr,"[headless] no state for asynchronous readback; using synchronous readback\n");return;}
 s->headless_state=st;st->bytes=(size_t)s->width*(size_t)s->height*4;
 /* Entry-point checks matter on older remote/fbdev machines: the headers may
  * expose GL 3.2 calls even when the created compatibility context does not. */
 if(!glGenBuffers||!glBindBuffer||!glBufferData||!glDeleteBuffers||
    !glFenceSync||!glClientWaitSync||!glDeleteSync||!glMapBufferRange||!glUnmapBuffer){
  fprintf(stderr,"[headless] GL pixel-pack/fence API unavailable; using synchronous readback\n");return;
 }
 while(glGetError()!=GL_NO_ERROR){}
 glGenBuffers(CD_HEADLESS_PBO_COUNT,st->pbo);
 for(int i=0;i<CD_HEADLESS_PBO_COUNT;i++){
  glBindBuffer(GL_PIXEL_PACK_BUFFER,st->pbo[i]);
  glBufferData(GL_PIXEL_PACK_BUFFER,(GLsizeiptr)st->bytes,NULL,GL_STREAM_READ);
 }
 glBindBuffer(GL_PIXEL_PACK_BUFFER,0);
 GLenum err=glGetError();
 if(err!=GL_NO_ERROR||!st->pbo[0]||!st->pbo[1]||!st->pbo[2]){
  fprintf(stderr,"[headless] pixel-pack allocation failed (0x%x); using synchronous readback\n",err);
  headless_release_buffers(st);return;
 }
 st->async_readback=true;st->next_serial=1;
 fprintf(stderr,"[headless] asynchronous readback: %d pixel-pack buffers, %.1f MiB total\n",
         CD_HEADLESS_PBO_COUNT,(double)(st->bytes*CD_HEADLESS_PBO_COUNT)/(1024.0*1024.0));
}

/* Publish only the newest completed frame. If several fences complete while a
 * client is busy, copying/encoding every stale frame would add latency without
 * improving the picture. Pending buffers are never overwritten or unmapped
 * before their fence signals. */
static bool headless_reap(Server *s,HeadlessState *st){
 int newest=-1;bool failed=false,ready[CD_HEADLESS_PBO_COUNT]={false};
 for(int i=0;i<CD_HEADLESS_PBO_COUNT;i++)if(st->pending[i]){
  GLenum status=glClientWaitSync(st->fence[i],0,0);
  if(status==GL_WAIT_FAILED){failed=true;break;}
  if(status==GL_ALREADY_SIGNALED||status==GL_CONDITION_SATISFIED){
   ready[i]=true;if(newest<0||st->serial[i]>st->serial[newest])newest=i;
  }
 }
 if(failed){fprintf(stderr,"[headless] fence wait failed; using synchronous readback\n");headless_release_buffers(st);return false;}
 for(int i=0;i<CD_HEADLESS_PBO_COUNT;i++)if(st->pending[i]&&ready[i]){
  if(i==newest&&backend_vnc_has_clients(s)){
   glBindBuffer(GL_PIXEL_PACK_BUFFER,st->pbo[i]);
   void *pixels=glMapBufferRange(GL_PIXEL_PACK_BUFFER,0,(GLsizeiptr)st->bytes,GL_MAP_READ_BIT);
   if(pixels){backend_vnc_present_pixels(s,pixels);glUnmapBuffer(GL_PIXEL_PACK_BUFFER);}
   else failed=true;
   glBindBuffer(GL_PIXEL_PACK_BUFFER,0);
  }
  glDeleteSync(st->fence[i]);st->fence[i]=NULL;st->pending[i]=false;
 }
 if(failed){fprintf(stderr,"[headless] pixel-pack mapping failed; using synchronous readback\n");headless_release_buffers(st);return false;}
 return true;
}

bool backend_headless_init(Server *s){
 if(!backend_egl_init(s,"headless"))return false;
 /* The bind is a session setting, not a backend constant: it defaults to
  * loopback because RFB here has no authentication (see src/backend_vnc.c),
  * and --vnc-listen-all / session.vnc_loopback opt out. */
 if(!backend_vnc_init(s,s->vnc_loopback)){backend_egl_shutdown(s);return false;}
 headless_init_readback(s);return true;
}

void backend_headless_poll(Server *s){backend_vnc_poll(s);}

void backend_headless_present(Server *s){
 HeadlessState *st=headless_state(s);
 if(st&&st->async_readback&&!headless_reap(s,st))st=NULL;
 /* Idle headless compositors still need render backpressure, but there is no
  * reason to read, flip and mark a 6.4 MB frame that nobody can consume. */
 if(!backend_vnc_has_clients(s)){glFinish();return;}
 glBindFramebuffer(GL_FRAMEBUFFER,s->target_fbo);
 if(st&&st->async_readback){
  int slot=-1;for(int i=0;i<CD_HEADLESS_PBO_COUNT;i++)if(!st->pending[i]){slot=i;break;}
  if(slot>=0){
   glBindBuffer(GL_PIXEL_PACK_BUFFER,st->pbo[slot]);
   glReadPixels(0,0,s->width,s->height,GL_RGBA,GL_UNSIGNED_BYTE,(void*)0);
   glBindBuffer(GL_PIXEL_PACK_BUFFER,0);
   st->fence[slot]=glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE,0);
   if(st->fence[slot]){
    st->serial[slot]=st->next_serial++;st->pending[slot]=true;glFlush();return;
   }
   fprintf(stderr,"[headless] fence creation failed; using synchronous readback\n");
   headless_release_buffers(st);
  }else return; /* all readbacks in flight: drop this presentation, not input/physics */
 }
 /* Compatibility path for GL drivers without asynchronous pixel-pack support. */
 glReadPixels(0,0,s->width,s->height,GL_RGBA,GL_UNSIGNED_BYTE,s->readback_pixels);
 backend_vnc_present_readback(s);
}

void backend_headless_shutdown(Server *s){
 backend_vnc_shutdown(s);
 HeadlessState *st=headless_state(s);
 if(st){headless_release_buffers(st);free(st);s->headless_state=NULL;}
 backend_egl_shutdown(s);
}
