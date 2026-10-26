#include "chrome.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/vt.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/signalfd.h>
#include <unistd.h>

static int failures;
#define CHECK(cond) do{if(!(cond)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#cond);failures++;}}while(0)

static struct {
 int home_vt;
 int tty_fd;
 bool fail_open,fail_getstate,fail_getmode,fail_setmode,fail_activate,fail_reldisp;
 int open_calls,close_calls,getstate_calls,getmode_calls,setmode_calls;
 int activate_calls,activate_target,wait_active_calls,reldisp_calls;
 unsigned long last_reldisp;
 struct vt_mode process_mode,restored_mode;
 bool drm_release_result,drm_acquire_result,evdev_resume_result;
 int evdev_suspend_calls,evdev_resume_calls,drm_release_calls,drm_acquire_calls;
 int64_t now_ms;
 char order[64];
} W;

static sigset_t original_mask;
static int evdev_dummy;
static bool mask_saved;

static void note(char c){
 size_t n=strlen(W.order);
 if(n+1<sizeof(W.order)){W.order[n]=c;W.order[n+1]=0;}
}

static void reset_world(void){
 memset(&W,0,sizeof(W));
 W.home_vt=2;W.tty_fd=-1;
 W.drm_release_result=true;W.drm_acquire_result=true;W.evdev_resume_result=true;
 W.now_ms=1000;
}

int __real_open(const char *path,int flags,...);
int __real_close(int fd);

int __wrap_open(const char *path,int flags,...){
 if(strcmp(path,"/dev/tty"))return __real_open(path,flags);
 W.open_calls++;
 if(W.fail_open){errno=ENXIO;return -1;}
 int fd=__real_open("/dev/null",O_RDWR|O_CLOEXEC);
 if(fd>=0)W.tty_fd=fd;
 return fd;
}

int __wrap_close(int fd){
 W.close_calls++;
 if(fd==W.tty_fd)W.tty_fd=-1;
 return __real_close(fd);
}

int __wrap_ioctl(int fd,unsigned long request,...){
 (void)fd;
 va_list ap;va_start(ap,request);
 int result=0;
 if(request==VT_GETSTATE){
  struct vt_stat *state=va_arg(ap,struct vt_stat*);W.getstate_calls++;
  if(W.fail_getstate){errno=ENOTTY;result=-1;}
  else{memset(state,0,sizeof(*state));state->v_active=(unsigned short)W.home_vt;}
 }else if(request==VT_GETMODE){
  struct vt_mode *mode=va_arg(ap,struct vt_mode*);W.getmode_calls++;
  if(W.fail_getmode){errno=EIO;result=-1;}
  else{
   memset(mode,0,sizeof(*mode));mode->mode=VT_AUTO;mode->waitv=1;
   mode->relsig=17;mode->acqsig=18;mode->frsig=0;
  }
 }else if(request==VT_SETMODE){
  struct vt_mode *mode=va_arg(ap,struct vt_mode*);W.setmode_calls++;
  if(W.fail_setmode){errno=EPERM;result=-1;}
  else if(mode->mode==VT_PROCESS)W.process_mode=*mode;
  else W.restored_mode=*mode;
 }else if(request==VT_ACTIVATE){
  unsigned long target=va_arg(ap,unsigned long);W.activate_calls++;W.activate_target=(int)target;
  if(W.fail_activate){errno=EINVAL;result=-1;}
 }else if(request==VT_RELDISP){
  unsigned long value=va_arg(ap,unsigned long);W.reldisp_calls++;
  if(W.fail_reldisp){errno=EIO;result=-1;}
  else{W.last_reldisp=value;note(value==VT_ACKACQ?'a':'r');}
 }else if(request==VT_WAITACTIVE){
  W.wait_active_calls++;result=-1;errno=EINVAL;
 }else{
  errno=ENOTTY;result=-1;
 }
 va_end(ap);return result;
}

int __wrap_clock_gettime(clockid_t clock,struct timespec *ts){
 (void)clock;
 ts->tv_sec=(time_t)(W.now_ms/1000);
 ts->tv_nsec=(long)((W.now_ms%1000)*1000000);
 return 0;
}

void backend_evdev_suspend(Server *s){
 (void)s;W.evdev_suspend_calls++;note('i');
}
bool backend_evdev_resume(Server *s){
 (void)s;W.evdev_resume_calls++;note('I');return W.evdev_resume_result;
}
bool backend_drm_release_for_vt(Server *s){
 (void)s;W.drm_release_calls++;note('d');return W.drm_release_result;
}
bool backend_drm_acquire_after_vt(Server *s){
 (void)s;W.drm_acquire_calls++;note('D');return W.drm_acquire_result;
}

static void init_server(Server *s){
 memset(s,0,sizeof(*s));s->backend=CD_BACKEND_DRM;s->running=true;s->evdev_state=&evdev_dummy;
}

static void check_shutdown(Server *s,bool expect_restore){
 backend_vt_shutdown(s);
 CHECK(s->vt_state==NULL);
 CHECK(W.tty_fd==-1);
 if(expect_restore){
  CHECK(W.restored_mode.mode==VT_AUTO);
  CHECK(W.restored_mode.waitv==1);
  CHECK(W.restored_mode.relsig==17);
  CHECK(W.restored_mode.acqsig==18);
 }
 CHECK(W.wait_active_calls==0);
}

static void test_no_tty(void){
 reset_world();W.fail_open=true;
 Server s;init_server(&s);
 CHECK(backend_vt_init(&s));
 CHECK(!backend_vt_managed(&s));
 CHECK(!backend_vt_is_suspended(&s));
 CHECK(!backend_vt_request(&s,3));
 backend_vt_service(&s);
 CHECK(W.drm_release_calls==0&&W.drm_acquire_calls==0);
 check_shutdown(&s,false);
}

static void test_non_tty_and_mode_failure(void){
 reset_world();W.fail_getstate=true;
 Server s;init_server(&s);CHECK(backend_vt_init(&s));
 CHECK(!backend_vt_managed(&s));check_shutdown(&s,false);

 reset_world();W.fail_setmode=true;
 init_server(&s);CHECK(backend_vt_init(&s));
 CHECK(!backend_vt_managed(&s));
 CHECK(W.setmode_calls==1);
 check_shutdown(&s,false);
}

static void test_signals_shortcuts_and_retry(void){
 reset_world();
 Server s;init_server(&s);CHECK(backend_vt_init(&s));
 CHECK(backend_vt_managed(&s));
 CHECK(!backend_vt_is_suspended(&s));
 CHECK(W.process_mode.mode==VT_PROCESS);
 CHECK(W.process_mode.waitv==0);
 CHECK(W.process_mode.relsig==SIGUSR1);
 CHECK(W.process_mode.acqsig==SIGUSR2);
 CHECK(W.process_mode.frsig==0);
 sigset_t child_mask;CHECK(backend_vt_child_mask(&child_mask));
 CHECK(!!sigismember(&child_mask,SIGUSR1)==!!sigismember(&original_mask,SIGUSR1));
 CHECK(!!sigismember(&child_mask,SIGUSR2)==!!sigismember(&original_mask,SIGUSR2));

 CHECK(backend_vt_request(&s,5));
 CHECK(W.activate_calls==1&&W.activate_target==5);
 CHECK(backend_vt_request(&s,W.home_vt)); /* redundant self-switch is a no-op */
 CHECK(W.activate_calls==1);
 CHECK(!backend_vt_request(&s,13));
 W.fail_activate=true;CHECK(!backend_vt_request(&s,4));
 W.fail_activate=false;

 W.order[0]=0;
 CHECK(raise(SIGUSR1)==0);
 backend_vt_service(&s);
 CHECK(!strcmp(W.order,"idr")); /* input, DRM, then release ACK */
 CHECK(W.evdev_suspend_calls==1&&W.drm_release_calls==1);
 CHECK(W.last_reldisp==1);
 CHECK(backend_vt_is_suspended(&s));
 CHECK(!backend_vt_needs_resume(&s));
 CHECK(s.running);

 W.drm_acquire_result=false;W.order[0]=0;
 CHECK(raise(SIGUSR2)==0);
 backend_vt_service(&s);
 CHECK(!strcmp(W.order,"aD")); /* acquire ACK precedes master attempt */
 CHECK(backend_vt_is_suspended(&s));
 CHECK(backend_vt_needs_resume(&s));
 CHECK(W.drm_acquire_calls==1);
 backend_vt_service(&s); /* retry is throttled */
 CHECK(W.drm_acquire_calls==1);
 W.now_ms+=999;backend_vt_service(&s);
 CHECK(W.drm_acquire_calls==1);
 W.drm_acquire_result=true;W.now_ms++;W.order[0]=0;
 backend_vt_service(&s);
 CHECK(!strcmp(W.order,"DI"));
 CHECK(W.drm_acquire_calls==2&&W.evdev_resume_calls==1);
 CHECK(!backend_vt_is_suspended(&s));
 CHECK(!backend_vt_needs_resume(&s));

 /* A failed release is still acknowledged, but fails closed into cleanup. */
 W.drm_release_result=false;W.order[0]=0;
 CHECK(raise(SIGUSR1)==0);
 backend_vt_service(&s);
 CHECK(!strcmp(W.order,"idr"));
 CHECK(!s.running);
 CHECK(backend_vt_is_suspended(&s));
 CHECK(W.last_reldisp==1);
 check_shutdown(&s,true);
}

static void test_input_resume_failure_enters_cleanup(void){
 reset_world();
 Server s;init_server(&s);CHECK(backend_vt_init(&s));
 CHECK(raise(SIGUSR1)==0);backend_vt_service(&s);
 CHECK(backend_vt_is_suspended(&s));
 W.evdev_resume_result=false;
 CHECK(raise(SIGUSR2)==0);backend_vt_service(&s);
 CHECK(W.evdev_resume_calls==1);
 /* Do not resume presentation after a failed regrab. Keeping the suspended
  * state also prevents another frame before the normal cleanup/VT restore. */
 CHECK(backend_vt_is_suspended(&s));
 CHECK(backend_vt_needs_resume(&s));
 CHECK(!s.running);
 check_shutdown(&s,true);
}

static void test_ack_failures_enter_cleanup(void){
 reset_world();
 Server s;init_server(&s);CHECK(backend_vt_init(&s));
 W.fail_reldisp=true;
 CHECK(raise(SIGUSR1)==0);backend_vt_service(&s);
 CHECK(!s.running); /* do not pause forever with a pending kernel release */
 CHECK(backend_vt_is_suspended(&s));
 CHECK(W.drm_acquire_calls==0&&W.evdev_resume_calls==0);
 check_shutdown(&s,true);

 reset_world();init_server(&s);CHECK(backend_vt_init(&s));
 CHECK(raise(SIGUSR1)==0);backend_vt_service(&s);
 CHECK(s.running&&backend_vt_is_suspended(&s));
 W.fail_reldisp=true;
 CHECK(raise(SIGUSR2)==0);backend_vt_service(&s);
 CHECK(!s.running);
 CHECK(W.drm_acquire_calls==0); /* no master without a successful ACKACQ */
 CHECK(W.evdev_resume_calls==0);
 check_shutdown(&s,true);
}

static void test_shutdown_acknowledges_pending_release(void){
 reset_world();
 Server s;init_server(&s);CHECK(backend_vt_init(&s));
 CHECK(raise(SIGUSR1)==0);
 int inputs=W.evdev_suspend_calls,drm=W.drm_release_calls;
 check_shutdown(&s,true);
 CHECK(W.reldisp_calls==1&&W.last_reldisp==1);
 CHECK(W.evdev_suspend_calls==inputs); /* teardown must not touch live input */
 CHECK(W.drm_release_calls==drm);
}

static void drain_pending_signals(void){
 sigset_t set,pending;sigemptyset(&set);sigaddset(&set,SIGUSR1);sigaddset(&set,SIGUSR2);
 for(;;){
  sigpending(&pending);
  if(!sigismember(&pending,SIGUSR1)&&!sigismember(&pending,SIGUSR2))break;
  siginfo_t info;
  if(sigwaitinfo(&set,&info)<0)break;
 }
}

int main(void){
 sigprocmask(SIG_SETMASK,NULL,&original_mask);mask_saved=true;
 backend_vt_block_signals();
 test_no_tty();
 test_non_tty_and_mode_failure();
 test_signals_shortcuts_and_retry();
 test_input_resume_failure_enters_cleanup();
 test_ack_failures_enter_cleanup();
 test_shutdown_acknowledges_pending_release();
 drain_pending_signals();
 if(mask_saved)sigprocmask(SIG_SETMASK,&original_mask,NULL);
 if(failures){fprintf(stderr,"test-vt: %d check(s) failed\n",failures);return 1;}
 printf("test-vt: all VT signal/switch checks passed\n");return 0;
}
