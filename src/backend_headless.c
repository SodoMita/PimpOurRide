/* Default production backend: no X server anywhere.
 *  - GL context/render target: backend_egl.c creates the shared surfaceless
 *    EGL context and explicit FBO used by both EGL backends.
 *  - Display output: an embedded libvncserver RFB server (backend_vnc.c) —
 *    one glReadPixels() per frame, flipped into the top-down RFB framebuffer.
 *    Browsers reach it via a websockify+noVNC bridge (tools/viewer).
 *  - Input: RFB pointer/keyboard callbacks drive the same server_* input API
 *    as the SDL backend, so behavior (picking, focus, hotkeys) matches.
 *  - Single-thread GL & teardown invariants: all GL work stays on the main
 *    compositor thread, and backend_egl_shutdown deletes FBO/renderbuffers
 *    while the context is still current before EGL termination.
 *  The EGL backends (headless and fbdev) support zwp_linux_dmabuf_v1 because
 *  EGLImage only interoperates with an EGL-created GL context (see dmabuf.c).
 */
#include "chrome.h"

bool backend_headless_init(Server *s){
 if(!backend_egl_init(s,"headless"))return false;
 if(!backend_vnc_init(s,false)){backend_egl_shutdown(s);return false;}
 return true;
}

void backend_headless_poll(Server *s){backend_vnc_poll(s);}

void backend_headless_present(Server *s){
 glBindFramebuffer(GL_FRAMEBUFFER,s->target_fbo);
 glReadPixels(0,0,s->width,s->height,GL_RGBA,GL_UNSIGNED_BYTE,s->readback_pixels);
 backend_vnc_present_readback(s);
}

void backend_headless_shutdown(Server *s){
 backend_vnc_shutdown(s);
 backend_egl_shutdown(s);
}
