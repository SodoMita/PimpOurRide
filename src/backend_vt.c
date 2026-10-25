/* Managed Linux VT switching for the direct DRM backend.
 *
 * SIGUSR1/SIGUSR2 are used as VT_PROCESS release/acquire notifications. They
 * are blocked process-wide (before SDL or any evdev worker starts) and read
 * only on the compositor main thread through signalfd. A missing /dev/tty or a
 * descriptor that is not a virtual terminal is non-fatal: DRM still runs, just
 * without managed switching. VT_WAITACTIVE is deliberately never used. */
#include "chrome.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/vt.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/signalfd.h>
#include <time.h>
#include <unistd.h>

typedef struct {
 int tty_fd;
 int sig_fd;
 bool managed;
 bool suspended;
 bool active;        /* our VT is the current one, even if DRM reacquire is pending */
 bool resume_pending;/* ACQ arrived; DRM master still needs to be reacquired */
 int64_t retry_after_ms;
 int vt_number;
 bool have_saved_mode;
 struct vt_mode saved_mode;
 bool warned_activate;
 bool warned_release;
 bool warned_acquire;
} VtState;

static VtState *vt_state(Server *s){return s?(VtState*)s->vt_state:NULL;}
static sigset_t vt_signals;
static sigset_t vt_inherited_mask;
static bool vt_mask_saved;

void backend_vt_block_signals(void){
 if(vt_mask_saved)return;
 sigemptyset(&vt_signals);
 sigaddset(&vt_signals,SIGUSR1);
 sigaddset(&vt_signals,SIGUSR2);
 if(sigprocmask(SIG_BLOCK,&vt_signals,&vt_inherited_mask)<0){
  fprintf(stderr,"[vt] cannot block SIGUSR1/SIGUSR2: %s; managed VT switching disabled\n",strerror(errno));
  return;
 }
 vt_mask_saved=true;
}

bool backend_vt_child_mask(sigset_t *mask){
 if(!mask||!vt_mask_saved)return false;
 *mask=vt_inherited_mask;
 return true;
}

static void vt_close_tty(VtState *st){
 if(!st)return;
 if(st->tty_fd>=0)close(st->tty_fd);
 st->tty_fd=-1;
}

bool backend_vt_init(Server *s){
 if(!s)return false;
 if(s->vt_state)return true;
 VtState *st=calloc(1,sizeof(*st));
 if(!st){fprintf(stderr,"[vt] out of memory\n");return true;} /* display can still run */
 st->tty_fd=st->sig_fd=-1;
 s->vt_state=st;
 /* main normally blocks these before SDL_Init; this keeps the init path
  * self-contained for focused tests and direct callers. */
 backend_vt_block_signals();
 if(!vt_mask_saved){
  /* Signals are not blocked, so signalfd would never see them. Leave the
   * state allocated but unmanaged so shutdown stays a no-op. */
  return true;
 }
 st->sig_fd=signalfd(-1,&vt_signals,SFD_NONBLOCK|SFD_CLOEXEC);
 if(st->sig_fd<0){
  fprintf(stderr,"[vt] signalfd failed: %s; managed VT switching disabled\n",strerror(errno));
  return true;
 }
 st->tty_fd=open("/dev/tty",O_RDWR|O_NONBLOCK|O_CLOEXEC);
 if(st->tty_fd<0){
  fprintf(stderr,"[vt] cannot open /dev/tty: %s; managed VT switching disabled (DRM session continues)\n",strerror(errno));
  return true;
 }
 struct vt_stat state;
 if(ioctl(st->tty_fd,VT_GETSTATE,&state)<0){
  fprintf(stderr,"[vt] /dev/tty is not a virtual terminal (%s); managed VT switching disabled\n",strerror(errno));
  vt_close_tty(st);
  return true;
 }
 st->vt_number=(int)state.v_active;
 struct vt_mode mode;
 if(ioctl(st->tty_fd,VT_GETMODE,&mode)<0){
  fprintf(stderr,"[vt] VT_GETMODE on VT %d failed: %s; managed VT switching disabled\n",st->vt_number,strerror(errno));
  vt_close_tty(st);
  return true;
 }
 st->saved_mode=mode;
 st->have_saved_mode=true;
 struct vt_mode request=mode;
 request.mode=VT_PROCESS;
 request.waitv=0;
 request.relsig=SIGUSR1;
 request.acqsig=SIGUSR2;
 request.frsig=0;
 if(ioctl(st->tty_fd,VT_SETMODE,&request)<0){
  fprintf(stderr,"[vt] VT_SETMODE(VT_PROCESS) on VT %d failed: %s; managed VT switching disabled\n",st->vt_number,strerror(errno));
  st->have_saved_mode=false;
  vt_close_tty(st);
  return true;
 }
 st->managed=true;st->active=true;
 fprintf(stderr,"[vt] managing VT %d (Ctrl+Alt+F1..F12); release/acquire via SIGUSR1/SIGUSR2\n",st->vt_number);
 return true;
}

CdVtSignal backend_vt_poll(Server *s){
 VtState *st=vt_state(s);
 if(!st||st->sig_fd<0)return CD_VT_SIGNAL_NONE;
 struct signalfd_siginfo info;
 ssize_t bytes;
 do bytes=read(st->sig_fd,&info,sizeof(info));while(bytes<0&&errno==EINTR);
 if(bytes!=(ssize_t)sizeof(info))return CD_VT_SIGNAL_NONE;
 if(info.ssi_signo==SIGUSR1)return CD_VT_SIGNAL_RELEASE;
 if(info.ssi_signo==SIGUSR2)return CD_VT_SIGNAL_ACQUIRE;
 return CD_VT_SIGNAL_NONE;
}

int backend_vt_wait(Server *s,int timeout_ms){
 VtState *st=vt_state(s);
 if(!st||st->sig_fd<0)return 0;
 struct pollfd pfd={.fd=st->sig_fd,.events=POLLIN};
 /* Do not restart after SIGINT/SIGTERM: the finite wait exists to let the
  * main loop leave promptly even while another VT is active. */
 int result=poll(&pfd,1,timeout_ms);
 if(result<0&&errno==EINTR)return 0;
 if(result<=0)return result;
 if(pfd.revents&(POLLERR|POLLHUP|POLLNVAL))return -1;
 return (pfd.revents&POLLIN)?1:0;
}

int backend_vt_fd(Server *s){
 VtState *st=vt_state(s);
 return st&&st->managed?st->sig_fd:-1;
}

bool backend_vt_managed(Server *s){
 VtState *st=vt_state(s);
 return st&&st->managed;
}

bool backend_vt_is_suspended(Server *s){
 VtState *st=vt_state(s);
 return st&&st->managed&&st->suspended;
}

bool backend_vt_needs_resume(Server *s){
 VtState *st=vt_state(s);
 return st&&st->managed&&st->active&&st->resume_pending;
}

bool backend_vt_request(Server *s,int vt){
 VtState *st=vt_state(s);
 if(!st||!st->managed||st->suspended||st->tty_fd<0||vt<1||vt>12)return false;
 if(vt==st->vt_number)return true; /* redundant switch to our own VT */
 if(ioctl(st->tty_fd,VT_ACTIVATE,(unsigned long)vt)<0){
  if(!st->warned_activate)
   fprintf(stderr,"[vt] VT_ACTIVATE %d failed: %s\n",vt,strerror(errno));
  st->warned_activate=true;
  return false;
 }
 return true;
}

static bool vt_ack(VtState *st,unsigned long value,bool *warned,const char *what){
 if(!st||!st->managed||st->tty_fd<0)return true;
 if(ioctl(st->tty_fd,VT_RELDISP,value)<0){
  if(warned&&!*warned){
   fprintf(stderr,"[vt] %s acknowledgement failed: %s\n",what,strerror(errno));
   *warned=true;
  }
  return false;
 }
 return true;
}

bool backend_vt_ack_release(Server *s){
 VtState *st=vt_state(s);if(!st)return true;
 st->suspended=true;st->active=false;st->resume_pending=false;
 return vt_ack(st,1,&st->warned_release,"release");
}

bool backend_vt_ack_acquire(Server *s){
 VtState *st=vt_state(s);if(!st)return true;
 st->active=true;st->resume_pending=st->suspended;
 /* Acquisition acknowledgement is intentionally before drmSetMaster: unlike
  * release, Linux does not need us to hold the VT switch while a previous
  * master finishes leaving, and an unresponsive GPU must not lock the console. */
 return vt_ack(st,VT_ACKACQ,&st->warned_acquire,"acquire");
}

void backend_vt_mark_resumed(Server *s){
 VtState *st=vt_state(s);if(!st)return;
 st->suspended=false;st->resume_pending=false;st->retry_after_ms=0;
}

static int64_t vt_now_ms(void){
 struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);
 return (int64_t)now.tv_sec*1000+now.tv_nsec/1000000;
}

/* All device, Wayland and GL-adjacent transitions stay on this compositor
 * thread. The signalfd worker is only the kernel's pollable notification fd;
 * there is no VT callback thread and no ioctl in a signal handler. */
void backend_vt_service(Server *s){
 VtState *st=vt_state(s);if(!st||!st->managed)return;
 for(int guard=0;guard<8;guard++){
  CdVtSignal signal=backend_vt_poll(s);if(signal==CD_VT_SIGNAL_NONE)break;
  if(signal==CD_VT_SIGNAL_RELEASE){
   if(s->evdev_state)backend_evdev_suspend(s);
   bool clean=backend_drm_release_for_vt(s);
   bool acknowledged=backend_vt_ack_release(s);st->retry_after_ms=0;
   /* A failed ACK can leave the kernel switch pending. Exit into the mode
    * restoration/close path rather than pausing forever in that half-state. */
   if(!clean||!acknowledged){s->running=false;break;}
  }else if(signal==CD_VT_SIGNAL_ACQUIRE){
   bool acknowledged=backend_vt_ack_acquire(s);st->retry_after_ms=0;
   /* Never take DRM master for an acquisition the kernel did not accept. */
   if(!acknowledged){s->running=false;break;}
  }
 }
 if(!s->running||!backend_vt_needs_resume(s))return;
 int64_t now=vt_now_ms();if(now<st->retry_after_ms)return;
 st->retry_after_ms=now+1000; /* a previous master may need a moment to leave */
 if(!backend_drm_acquire_after_vt(s))return;
 if(s->evdev_state&&!backend_evdev_resume(s))
  fprintf(stderr,"[vt] input resume failed; devices remain ungrabbed so kernel VT switching stays available\n");
 backend_vt_mark_resumed(s);
}

void backend_vt_shutdown(Server *s){
 VtState *st=vt_state(s);if(!st)return;
 /* Drain without touching Wayland/input. A pending release/acquire must be
  * acknowledged or the kernel can keep the VT switch blocked while we exit. */
 if(st->sig_fd>=0){
  for(;;){
   CdVtSignal signal=backend_vt_poll(s);
   if(signal==CD_VT_SIGNAL_NONE)break;
   if(signal==CD_VT_SIGNAL_RELEASE)vt_ack(st,1,&st->warned_release,"release");
   else if(signal==CD_VT_SIGNAL_ACQUIRE)vt_ack(st,VT_ACKACQ,&st->warned_acquire,"acquire");
  }
 }
 if(st->managed&&st->tty_fd>=0&&st->have_saved_mode){
  if(ioctl(st->tty_fd,VT_SETMODE,&st->saved_mode)<0)
   fprintf(stderr,"[vt] restoring the original VT mode on VT %d failed: %s\n",st->vt_number,strerror(errno));
  else fprintf(stderr,"[vt] restored VT %d mode\n",st->vt_number);
 }
 if(st->sig_fd>=0)close(st->sig_fd);
 vt_close_tty(st);
 free(st);
 s->vt_state=NULL;
}
