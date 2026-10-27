/* White-box fault injection: including the implementation lets this test drive
 * the static evdev/udev preparation path as well as the public init/finish API.
 * No host device is touched. Linker wrapping is confined to this test binary;
 * the ordinary test-wheel-input executable still exercises the real API. */
#include "../src/wheel_input.c"
#include <stdarg.h>

static int open_errno;
static int system_calls;
static int mknod_calls;
static int access_calls;
static int chmod_calls;
static int unlink_calls;
static bool preexisting_case;
static int failures;

int __wrap_open(const char *path,int flags,...){
 (void)path;(void)flags;
 errno=open_errno;
 return -1;
}

int __wrap_mknod(const char *path,mode_t mode,dev_t dev){
 (void)path;(void)mode;(void)dev;
 mknod_calls++;
 errno=EPERM;
 return -1;
}

int __wrap_system(const char *command){
 (void)command;
 system_calls++;
 return 0;
}

int __wrap_access(const char *path,int mode){
 (void)mode;
 access_calls++;
 if(preexisting_case&&strncmp(path,"/run/udev/",10)==0)return 0;
 errno=EACCES;
 return -1;
}

int __wrap_lstat(const char *path,struct stat *st){
 (void)path;
 memset(st,0,sizeof(*st));
 if(preexisting_case)return 0; /* existing host node/tag: do not modify it */
 errno=ENOENT;
 return -1;
}

int __wrap_chmod(const char *path,mode_t mode){
 (void)path;(void)mode;
 chmod_calls++;
 return 0;
}

int __wrap_unlink(const char *path){
 (void)path;
 unlink_calls++;
 return 0;
}

static void check_case(int error,bool want_mknod){
 WheelJoystick joy;
 open_errno=error;
 system_calls=mknod_calls=0;
 bool ok=wheel_joystick_init(&joy);
 if(ok){fprintf(stderr,"FAIL: init unexpectedly succeeded for errno %d\n",error);failures++;}
 if(system_calls){fprintf(stderr,"FAIL: %d privilege subprocess(es) for errno %d\n",system_calls,error);failures++;}
 if(mknod_calls!=(want_mknod?1:0)){
  fprintf(stderr,"FAIL: direct mknod calls=%d for errno %d (wanted %d)\n",mknod_calls,error,want_mknod?1:0);
  failures++;
 }
 if(joy.active||joy.uinput_fd!=-1){fprintf(stderr,"FAIL: failed init left active joystick state\n");failures++;}
 system_calls=0;
 wheel_joystick_finish(&joy);
 if(system_calls){fprintf(stderr,"FAIL: cleanup spawned a privilege subprocess\n");failures++;}
}

static void check_preexisting_node_untouched(void){
 WheelJoystick joy;
 memset(&joy,0,sizeof(joy));
 snprintf(joy.sysname,sizeof(joy.sysname),"missing-test-input");
 snprintf(joy.dev_node,sizeof(joy.dev_node),"/tmp/chromedrive-preexisting-event0");
 joy.major_num=13;joy.minor_num=64;
 preexisting_case=true;
 system_calls=mknod_calls=access_calls=chmod_calls=unlink_calls=0;
 ensure_event_node_and_udev(&joy);
 preexisting_case=false;
 if(system_calls||mknod_calls||chmod_calls||unlink_calls){
  fprintf(stderr,"FAIL: pre-existing node modified: system=%d mknod=%d chmod=%d unlink=%d\n",
          system_calls,mknod_calls,chmod_calls,unlink_calls);
  failures++;
 }
 if(joy.created_node||joy.created_udev){
  fprintf(stderr,"FAIL: pre-existing node/tag incorrectly marked as created by ChromeDrive\n");
  failures++;
 }
 if(!access_calls){fprintf(stderr,"FAIL: pre-existing path was not exercised\n");failures++;}
}

int main(void){
 check_case(EACCES,false); /* existing but inaccessible: never repair by escalation */
 check_case(ENOENT,true);  /* missing: one direct best-effort mknod, then soft failure */
 check_preexisting_node_untouched();
 if(failures)return 1;
 puts("PASS: uinput denial/missing-node paths never spawn helpers or modify pre-existing nodes");
 return 0;
}
