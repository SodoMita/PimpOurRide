#include "chrome.h"
#include "input_wait.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int failures;
#define CHECK(cond) do { if(!(cond)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#cond);failures++;} } while(0)

#define MAX_FAKE_DEVICES 64

typedef enum { FAKE_MOUSE, FAKE_KEYBOARD, FAKE_HYBRID, FAKE_UNSUPPORTED } FakeKind;
typedef struct {
    char path[64];
    char name[64];
    FakeKind kind;
    int fd,write_fd;
    int opens;
    int grab_calls,grab_value;
    int fail_grab_value; /* -1: never; 0: fail release; 1: fail acquire */
    int grab_errno;
    bool deny_write_open;
    uint8_t evbit[EV_MAX/8+1];
    uint8_t keybit[KEY_MAX/8+1];
    uint8_t ledbit[LED_MAX/8+1];
    uint8_t relbit[REL_MAX/8+1];
    uint8_t absbit[ABS_MAX/8+1];
} FakeDevice;

static FakeDevice devices[MAX_FAKE_DEVICES];
static int device_count;
static int fake_dir_position;
static int fake_dir_cookie;
static int key_calls,last_key,vt_calls,last_vt;
static uint64_t key_seen;
static bool fake_vt_managed;

static void bit_set(uint8_t *bits,unsigned code){bits[code/8]|=(uint8_t)(1u<<(code%8));}
static FakeDevice *find_path(const char *path){
    for(int i=0;i<device_count;i++)if(!strcmp(devices[i].path,path))return &devices[i];
    return NULL;
}
static FakeDevice *find_fd(int fd){
    for(int i=0;i<device_count;i++)if(devices[i].fd==fd)return &devices[i];
    return NULL;
}
static void add_device(int index,FakeKind kind){
    CHECK(device_count<MAX_FAKE_DEVICES);
    if(device_count>=MAX_FAKE_DEVICES)return;
    FakeDevice *d=&devices[device_count++];
    snprintf(d->path,sizeof(d->path),"/dev/input/event%d",index);
    snprintf(d->name,sizeof(d->name),"fake-%d",index);
    d->kind=kind;d->fd=-1;d->write_fd=-1;
    d->grab_value=-1;d->fail_grab_value=-1;d->grab_errno=EBUSY;
    if(kind==FAKE_UNSUPPORTED)return;
    bit_set(d->evbit,EV_KEY);
    if(kind==FAKE_KEYBOARD){
        bit_set(d->keybit,KEY_A);bit_set(d->keybit,KEY_F1);
        bit_set(d->keybit,KEY_LEFTCTRL);bit_set(d->keybit,KEY_LEFTALT);
        bit_set(d->keybit,KEY_LEFTSHIFT);bit_set(d->keybit,KEY_CAPSLOCK);
        bit_set(d->keybit,KEY_NUMLOCK);bit_set(d->keybit,KEY_SCROLLLOCK);return;
    }
    bit_set(d->evbit,EV_REL);bit_set(d->relbit,REL_X);bit_set(d->relbit,REL_Y);bit_set(d->keybit,BTN_LEFT);
    if(kind==FAKE_HYBRID){
        bit_set(d->keybit,KEY_A);bit_set(d->keybit,KEY_B);
        bit_set(d->keybit,KEY_LEFTCTRL);bit_set(d->keybit,KEY_LEFTALT);
    }
}
static void reset_fake(void){
    memset(devices,0,sizeof(devices));device_count=0;fake_dir_position=0;
    key_calls=vt_calls=0;last_key=last_vt=-1;key_seen=0;fake_vt_managed=false;
}

int __real_open(const char *path,int flags,...);
int __real_close(int fd);
int __real_ioctl(int fd,unsigned long request,...);
DIR *__real_opendir(const char *path);
struct dirent *__real_readdir(DIR *dir);
int __real_closedir(DIR *dir);

int __wrap_open(const char *path,int flags,...){
    FakeDevice *d=find_path(path);
    if(d){
        if(d->deny_write_open&&(flags&O_ACCMODE)==O_RDWR){errno=EACCES;return -1;}
        if(d->fd>=0){errno=EBUSY;return -1;}
        int fds[2];
        /* A full-duplex evdev stand-in: test input travels write_fd -> fd,
         * while production EV_LED output travels fd -> write_fd without being
         * looped back as fresh input. Pipes cannot exercise writable evdev. */
        if(socketpair(AF_UNIX,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0,fds)<0)return -1;
        d->fd=fds[0];d->write_fd=fds[1];d->opens++;
        return d->fd;
    }
    mode_t mode=0;
    if(flags&O_CREAT){va_list ap;va_start(ap,flags);mode=(mode_t)va_arg(ap,int);va_end(ap);}
    return __real_open(path,flags,mode);
}
int __wrap_close(int fd){
    FakeDevice *d=find_fd(fd);
    int result=__real_close(fd);
    if(d&&result==0)d->fd=-1;
    return result;
}
static void copy_bits(void *argument,const uint8_t *source,size_t source_size,unsigned long request){
    size_t size=_IOC_SIZE(request);if(size>source_size)size=source_size;
    memset(argument,0,size);memcpy(argument,source,size);
}
int __wrap_ioctl(int fd,unsigned long request,...){
    va_list ap;va_start(ap,request);
    if(request==EVIOCGRAB){
        int value=va_arg(ap,int);va_end(ap);
        FakeDevice *d=find_fd(fd);
        if(!d)return __real_ioctl(fd,request,value);
        d->grab_calls++;d->grab_value=value;
        if(d->fail_grab_value==value){errno=d->grab_errno;return -1;}
        return 0;
    }
    void *argument=va_arg(ap,void *);va_end(ap);
    FakeDevice *d=find_fd(fd);
    if(!d)return __real_ioctl(fd,request,argument);
    if(request==EVIOCSCLOCKID)return 0;
    unsigned nr=_IOC_NR(request);
    if(nr>=_IOC_NR(EVIOCGBIT(0,0))&&nr<=_IOC_NR(EVIOCGBIT(EV_MAX,0))){
        unsigned ev=nr-_IOC_NR(EVIOCGBIT(0,0));
        if(ev==0)copy_bits(argument,d->evbit,sizeof(d->evbit),request);
        else if(ev==EV_KEY)copy_bits(argument,d->keybit,sizeof(d->keybit),request);
        else if(ev==EV_LED)copy_bits(argument,d->ledbit,sizeof(d->ledbit),request);
        else if(ev==EV_REL)copy_bits(argument,d->relbit,sizeof(d->relbit),request);
        else if(ev==EV_ABS)copy_bits(argument,d->absbit,sizeof(d->absbit),request);
        else{errno=EINVAL;return -1;}
        return (int)_IOC_SIZE(request);
    }
    if(nr==_IOC_NR(EVIOCGPROP(0))){memset(argument,0,_IOC_SIZE(request));return (int)_IOC_SIZE(request);}
    if(nr==_IOC_NR(EVIOCGNAME(0))){
        size_t size=_IOC_SIZE(request);snprintf(argument,size,"%s",d->name);return (int)strlen(d->name)+1;
    }
    errno=EINVAL;return -1;
}
DIR *__wrap_opendir(const char *path){
    if(!strcmp(path,"/dev/input")){fake_dir_position=0;return (DIR *)&fake_dir_cookie;}
    return __real_opendir(path);
}
struct dirent *__wrap_readdir(DIR *dir){
    if(dir==(DIR *)&fake_dir_cookie){
        static struct dirent entry;
        if(fake_dir_position>=device_count)return NULL;
        memset(&entry,0,sizeof(entry));
        const char *slash=strrchr(devices[fake_dir_position++].path,'/');
        snprintf(entry.d_name,sizeof(entry.d_name),"%s",slash?slash+1:devices[fake_dir_position-1].path);
        entry.d_type=DT_CHR;return &entry;
    }
    return __real_readdir(dir);
}
int __wrap_closedir(DIR *dir){return dir==(DIR *)&fake_dir_cookie?0:__real_closedir(dir);}

static void init_server(Server *s){
    memset(s,0,sizeof(*s));s->width=1600;s->height=1000;s->backend=CD_BACKEND_DRM;
    s->evdev_chrome_touch_id=-1;
}
static void check_all_closed(void){
    for(int i=0;i<device_count;i++){
        CHECK(devices[i].fd<0);
        if(devices[i].write_fd>=0){__real_close(devices[i].write_fd);devices[i].write_fd=-1;}
    }
}
static void send_key(int index,uint16_t code,int value){
    char path[64];snprintf(path,sizeof(path),"/dev/input/event%d",index);
    FakeDevice *d=find_path(path);CHECK(d&&d->write_fd>=0);
    if(!d||d->write_fd<0)return;
    struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);
    struct input_event event={.time={.tv_sec=now.tv_sec,.tv_usec=now.tv_nsec/1000},
        .type=EV_KEY,.code=code,.value=value};
    ssize_t written=write(d->write_fd,&event,sizeof(event));
    CHECK(written==(ssize_t)sizeof(event));
}
static void enable_leds(int index, uint32_t mask){
    char path[64];snprintf(path,sizeof(path),"/dev/input/event%d",index);
    FakeDevice *d=find_path(path);CHECK(d!=NULL);if(!d)return;
    static const uint16_t codes[CD_XKB_LED_COUNT]={
        LED_NUML,LED_CAPSL,LED_SCROLLL,LED_COMPOSE,LED_KANA,
    };
    bit_set(d->evbit,EV_LED);
    for(size_t i=0;i<CD_XKB_LED_COUNT;i++)if(mask&(1u<<i))bit_set(d->ledbit,codes[i]);
}
static int read_output(FakeDevice *d,struct input_event *events,int capacity){
    int count=0;
    while(count<capacity){
        ssize_t n=read(d->write_fd,&events[count],(size_t)(capacity-count)*sizeof(events[0]));
        if(n>0){
            CHECK(n%(ssize_t)sizeof(events[0])==0);
            count+=(int)(n/sizeof(events[0]));continue;
        }
        if(n<0&&errno==EINTR)continue;
        CHECK(n==0||(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK)));
        break;
    }
    return count;
}
static int output_led_count(const struct input_event *events,int count,
                            uint16_t code,int value){
    int found=0;
    for(int i=0;i<count;i++)if(events[i].type==EV_LED&&events[i].code==code&&
        events[i].value==value)found++;
    return found;
}
static bool output_has_syn(const struct input_event *events,int count){
    for(int i=0;i<count;i++)if(events[i].type==EV_SYN&&events[i].code==SYN_REPORT)return true;
    return false;
}
static void poll_until(Server *s,int *value,int target){
    for(int i=0;i<200&&*value<target;i++){
        backend_evdev_poll(s);
        if(*value<target){
            int fd=backend_evdev_wait_fd(s);CHECK(fd>=0);
            if(fd>=0)CHECK(cd_input_wait(fd,10)>=0);else usleep(1000);
        }
    }
    backend_evdev_poll(s);
    CHECK(*value>=target);
}
static void test_mouse_nodes_do_not_hide_keyboard(void){
    reset_fake();
    for(int i=0;i<12;i++){
        FakeKind kind=i==8?FAKE_UNSUPPORTED:(i==9?FAKE_KEYBOARD:(i==0?FAKE_HYBRID:FAKE_MOUSE));
        add_device(i,kind);
    }
    Server s;init_server(&s);s.drm_grab_input=true;s.fbdev_grab_input=false;
    CHECK(backend_evdev_init(&s));
    EvdevState *st=(EvdevState *)s.evdev_state;
    CHECK(st!=NULL);
    if(st){
        CHECK(st->device_count==11); /* one unusable node must not consume a slot */
        CHECK(st->grab_input&&st->grab_required);
        bool keyboard=false,hybrid=false,all_grabbed=true;
        for(int i=0;i<st->device_count;i++){
            EvdevDevice *d=&st->devices[i];
            if(!strcmp(d->path,"/dev/input/event9"))keyboard=d->type==EVDEV_DEV_KEYBOARD;
            if(!strcmp(d->path,"/dev/input/event0"))hybrid=d->type==EVDEV_DEV_REL_POINTER&&d->has_keys;
            FakeDevice *fake=find_path(d->path);
            all_grabbed&=d->grabbed&&fake&&fake->grab_value==1;
        }
        CHECK(keyboard);CHECK(hybrid);CHECK(all_grabbed);
        /* Drive the real epoll worker + bounded queue. Distinct keys prove
         * that both the late keyboard and hybrid pointer retain key delivery. */
        fake_vt_managed=true;
        send_key(9,KEY_A,1);send_key(0,KEY_B,1);
        poll_until(&s,&key_calls,2);
        CHECK(key_seen&(1ull<<KEY_A));CHECK(key_seen&(1ull<<KEY_B));
        /* Ctrl and Alt may arrive on different physical evdev nodes; F1 is
         * consumed only by that physical chord and requests VT 1. */
        int modifiers_before=key_calls;
        send_key(0,KEY_LEFTCTRL,1);send_key(9,KEY_LEFTALT,1);
        poll_until(&s,&key_calls,modifiers_before+2);
        /* Eventfd dispatch is intentionally prompt: do not rely on a cross-
         * device chord being batched before F1. Both held modifiers are now
         * visible in their respective physical-device states. */
        send_key(9,KEY_F1,1);poll_until(&s,&vt_calls,1);
        CHECK(last_vt==1);CHECK(!(key_seen&(1ull<<KEY_F1)));
    }
    backend_evdev_shutdown(&s);check_all_closed();
    for(int i=0;i<12;i++)if(i!=8)CHECK(devices[i].grab_value==0);
}
static void test_discovery_scans_past_unusable_candidates(void){
    reset_fake();
    for(int i=0;i<40;i++)add_device(i,i==34?FAKE_MOUSE:(i==35?FAKE_KEYBOARD:FAKE_UNSUPPORTED));
    Server s;init_server(&s);CHECK(backend_evdev_init(&s));
    EvdevState *st=(EvdevState *)s.evdev_state;
    CHECK(st!=NULL);
    if(st){
        CHECK(st->device_count==2);
        if(st->device_count==2){
            CHECK(st->devices[0].type==EVDEV_DEV_REL_POINTER);
            CHECK(st->devices[1].type==EVDEV_DEV_KEYBOARD);
            CHECK(!strcmp(st->devices[1].path,"/dev/input/event35"));
        }
    }
    backend_evdev_shutdown(&s);check_all_closed();
}
static void test_explicit_devices_use_full_capacity(void){
    CHECK(EVDEV_MAX_DEVICES>=32);
    if(EVDEV_MAX_DEVICES<12)return;
    reset_fake();
    Server s;init_server(&s);
    for(int i=0;i<12;i++){
        add_device(i,i==11?FAKE_KEYBOARD:FAKE_MOUSE);
        strcpy(s.fbdev_input_devices[i],devices[i].path);
    }
    s.fbdev_input_device_count=12;
    CHECK(backend_evdev_init(&s));
    EvdevState *st=(EvdevState *)s.evdev_state;
    CHECK(st&&st->device_count==12);
    if(st&&st->device_count==12)CHECK(st->devices[11].type==EVDEV_DEV_KEYBOARD);
    backend_evdev_shutdown(&s);check_all_closed();
}

static void make_led_config(CdXkbConfig *config,const char *layouts){
    CdXkbConfig literal={.rules="evdev",.model="pc105",.layouts=(char *)layouts,
        .variants=",",.options="grp:alt_shift_toggle,grp_led:caps,scrolllock:mod3"};
    CHECK(cd_xkb_config_copy(config,&literal));
}
static void tap_fake_key(Server *s,int index,uint16_t code){
    int target=key_calls+2;
    send_key(index,code,1);send_key(index,code,0);
    poll_until(s,&key_calls,target);
}
static void test_keyboard_led_sync(void){
    reset_fake();
    add_device(0,FAKE_KEYBOARD);add_device(1,FAKE_KEYBOARD);add_device(2,FAKE_MOUSE);
    enable_leds(0,CD_XKB_LED_NUM_LOCK|CD_XKB_LED_CAPS_LOCK|CD_XKB_LED_SCROLL_LOCK|
        CD_XKB_LED_COMPOSE|CD_XKB_LED_KANA);
    enable_leds(1,CD_XKB_LED_CAPS_LOCK);
    Server s;init_server(&s);s.backend=CD_BACKEND_FBDEV;s.fbdev_grab_input=false;
    CHECK(backend_evdev_init(&s));
    EvdevState *st=(EvdevState *)s.evdev_state;
    CHECK(st&&st->device_count==3);
    CdXkbConfig config={0};make_led_config(&config,"us,ru");
    char error[512]="";
    CHECK(cd_xkb_keyboard_init(&s.xkb,&config,error,sizeof(error)));
    if(!s.xkb.state){backend_evdev_shutdown(&s);check_all_closed();cd_xkb_config_clear(&config);return;}

    /* Initial synchronization happens after XKB exists even though evdev was
     * opened first. Every supported LED is explicitly cleared, no others. */
    backend_evdev_poll(&s);
    struct input_event first[32],second[32];
    int n0=read_output(&devices[0],first,32);
    int n1=read_output(&devices[1],second,32);
    CHECK(n0==CD_XKB_LED_COUNT+1&&output_has_syn(first,n0));
    for(int i=0;i<n0;i++)CHECK(first[i].type!=EV_LED||first[i].value==0);
    CHECK(output_led_count(first,n0,LED_COMPOSE,0)==1&&
        output_led_count(first,n0,LED_KANA,0)==1);
    CHECK(n1==2&&output_led_count(second,n1,LED_CAPSL,0)==1&&
        output_led_count(second,n1,LED_NUML,0)==0&&output_has_syn(second,n1));
    CHECK(read_output(&devices[2],first,32)==0);
    backend_evdev_poll(&s);
    CHECK(read_output(&devices[0],first,32)==0&&read_output(&devices[1],second,32)==0);

    int target=key_calls+2;
    send_key(0,KEY_LEFTALT,1);send_key(0,KEY_LEFTSHIFT,1);
    poll_until(&s,&key_calls,target);
    CHECK(xkb_state_serialize_layout(s.xkb.state,XKB_STATE_LAYOUT_EFFECTIVE)==1);
    n0=read_output(&devices[0],first,32);n1=read_output(&devices[1],second,32);
    CHECK(output_led_count(first,n0,LED_CAPSL,1)==1&&output_has_syn(first,n0));
    CHECK(output_led_count(second,n1,LED_CAPSL,1)==1&&output_has_syn(second,n1));
    target=key_calls+2;
    send_key(0,KEY_LEFTSHIFT,0);send_key(0,KEY_LEFTALT,0);
    poll_until(&s,&key_calls,target);
    CHECK(read_output(&devices[0],first,32)==0&&read_output(&devices[1],second,32)==0);

    /* A valid replacement rebuilds state and LED indices. With no held chord,
     * the new map starts in group 0 and both physical indicators follow it. */
    CdXkbConfig next={0};make_led_config(&next,"de,us");bool changed=false;
    CHECK(cd_xkb_keyboard_reconfigure(&s.xkb,&next,s.xkb_pressed,&changed,error,sizeof(error)));
    CHECK(changed);backend_evdev_poll(&s);
    n0=read_output(&devices[0],first,32);n1=read_output(&devices[1],second,32);
    CHECK(output_led_count(first,n0,LED_CAPSL,0)==1);
    CHECK(output_led_count(second,n1,LED_CAPSL,0)==1);
    cd_xkb_config_clear(&next);

    /* Num exists only on device 0: device 1 must not receive redundant or
     * unsupported packets when the shared seat mask changes. */
    tap_fake_key(&s,0,KEY_NUMLOCK);
    n0=read_output(&devices[0],first,32);
    CHECK(output_led_count(first,n0,LED_NUML,1)==1);
    CHECK(read_output(&devices[1],second,32)==0);
    tap_fake_key(&s,0,KEY_NUMLOCK);
    n0=read_output(&devices[0],first,32);
    CHECK(output_led_count(first,n0,LED_NUML,0)==1);
    CHECK(read_output(&devices[1],second,32)==0);

    target=key_calls+4;
    send_key(0,KEY_LEFTALT,1);send_key(0,KEY_LEFTSHIFT,1);
    send_key(0,KEY_LEFTSHIFT,0);send_key(0,KEY_LEFTALT,0);
    poll_until(&s,&key_calls,target);
    CHECK(xkb_state_serialize_layout(s.xkb.state,XKB_STATE_LAYOUT_EFFECTIVE)==1);
    n0=read_output(&devices[0],first,32);n1=read_output(&devices[1],second,32);
    CHECK(output_led_count(first,n0,LED_CAPSL,1)==1);
    CHECK(output_led_count(second,n1,LED_CAPSL,1)==1);

    backend_evdev_suspend(&s);
    n0=read_output(&devices[0],first,32);n1=read_output(&devices[1],second,32);
    CHECK(output_led_count(first,n0,LED_CAPSL,0)==1);
    CHECK(output_led_count(second,n1,LED_CAPSL,0)==1);
    CHECK(backend_evdev_resume(&s));
    n0=read_output(&devices[0],first,32);n1=read_output(&devices[1],second,32);
    CHECK(output_led_count(first,n0,LED_CAPSL,1)==1);
    CHECK(output_led_count(second,n1,LED_CAPSL,1)==1);

    backend_evdev_shutdown(&s);
    n0=read_output(&devices[0],first,32);n1=read_output(&devices[1],second,32);
    CHECK(output_led_count(first,n0,LED_CAPSL,0)==1);
    CHECK(output_led_count(second,n1,LED_CAPSL,0)==1);
    cd_xkb_keyboard_finish(&s.xkb);cd_xkb_config_clear(&config);
    check_all_closed();
}

static void test_keyboard_led_read_only_fallback(void){
    reset_fake();add_device(0,FAKE_KEYBOARD);enable_leds(0,CD_XKB_LED_CAPS_LOCK);
    devices[0].deny_write_open=true;
    Server s;init_server(&s);s.backend=CD_BACKEND_FBDEV;
    CHECK(backend_evdev_init(&s));
    EvdevState *st=(EvdevState *)s.evdev_state;
    CHECK(st&&st->device_count==1&&!st->devices[0].led_writable);
    CdXkbConfig config={0};make_led_config(&config,"us,ru");char error[512]="";
    CHECK(cd_xkb_keyboard_init(&s.xkb,&config,error,sizeof(error)));
    backend_evdev_poll(&s);
    struct input_event out[16];
    CHECK(read_output(&devices[0],out,16)==0);
    int target=key_calls+4;
    send_key(0,KEY_LEFTALT,1);send_key(0,KEY_LEFTSHIFT,1);
    send_key(0,KEY_LEFTSHIFT,0);send_key(0,KEY_LEFTALT,0);
    poll_until(&s,&key_calls,target);
    CHECK(xkb_state_serialize_layout(s.xkb.state,XKB_STATE_LAYOUT_EFFECTIVE)==1);
    CHECK(read_output(&devices[0],out,16)==0);
    backend_evdev_shutdown(&s);
    CHECK(read_output(&devices[0],out,16)==0);
    cd_xkb_keyboard_finish(&s.xkb);cd_xkb_config_clear(&config);check_all_closed();
}

static void test_fbdev_policy_remains_opt_in(void){
    reset_fake();add_device(0,FAKE_KEYBOARD);
    Server s;init_server(&s);s.backend=CD_BACKEND_FBDEV;
    s.drm_grab_input=true;s.fbdev_grab_input=false;
    CHECK(backend_evdev_init(&s));
    EvdevState *st=(EvdevState *)s.evdev_state;
    CHECK(st&&st->device_count==1);
    if(st){
        CHECK(!st->grab_input&&!st->grab_required);
        CHECK(!st->devices[0].grabbed);
    }
    CHECK(devices[0].grab_calls==0);
    backend_evdev_shutdown(&s);check_all_closed();
}

static void test_startup_grab_failure_fails_closed(void){
    reset_fake();add_device(0,FAKE_KEYBOARD);add_device(1,FAKE_MOUSE);
    devices[0].fail_grab_value=1;devices[0].grab_errno=EPERM;
    Server s;init_server(&s);s.drm_grab_input=true;
    CHECK(!backend_evdev_init(&s));
    CHECK(s.evdev_state==NULL);
    CHECK(devices[0].fd<0&&devices[1].fd<0);
    CHECK(devices[1].grab_value==0); /* partial acquisition was released */
    check_all_closed();

    /* fbdev's explicitly requested grab retains its documented weaker
     * fallback; the new fail-closed guarantee belongs to managed DRM. */
    reset_fake();add_device(0,FAKE_KEYBOARD);devices[0].fail_grab_value=1;
    init_server(&s);s.backend=CD_BACKEND_FBDEV;s.fbdev_grab_input=true;
    CHECK(backend_evdev_init(&s));
    EvdevState *st=(EvdevState *)s.evdev_state;
    CHECK(st&&st->grab_input&&!st->grab_required&&st->device_count==1);
    if(st)CHECK(!st->devices[0].grabbed);
    backend_evdev_shutdown(&s);check_all_closed();
}

static void test_grab_suspend_resume_and_resume_failure(void){
    reset_fake();add_device(0,FAKE_KEYBOARD);add_device(1,FAKE_MOUSE);
    Server s;init_server(&s);s.drm_grab_input=true;
    CHECK(backend_evdev_init(&s));
    EvdevState *st=(EvdevState *)s.evdev_state;
    CHECK(st&&st->device_count==2);
    backend_evdev_suspend(&s);
    CHECK(st->suspended);
    for(int i=0;i<2;i++){
        CHECK(st->devices[i].fd>=0&&!st->devices[i].grabbed);
        CHECK(devices[i].grab_value==0);
    }
    CHECK(backend_evdev_resume(&s));
    CHECK(!st->suspended);
    for(int i=0;i<2;i++)CHECK(st->devices[i].grabbed&&devices[i].grab_value==1);

    backend_evdev_suspend(&s);
    devices[1].fail_grab_value=1;
    CHECK(!backend_evdev_resume(&s));
    CHECK(st->suspended); /* the VT layer must now enter cleanup */
    for(int i=0;i<2;i++)CHECK(!st->devices[i].grabbed);
    CHECK(devices[0].grab_value==0); /* earlier partial regrab was released */
    backend_evdev_shutdown(&s);check_all_closed();
}

static void test_failed_release_cannot_resume_unverified_device(void){
    reset_fake();add_device(0,FAKE_KEYBOARD);
    Server s;init_server(&s);s.drm_grab_input=true;
    CHECK(backend_evdev_init(&s));
    EvdevState *st=(EvdevState *)s.evdev_state;
    devices[0].fail_grab_value=0;
    backend_evdev_suspend(&s);
    CHECK(st&&st->suspended);
    if(st){
        CHECK(st->devices[0].fd<0&&st->devices[0].grab_release_failed);
        CHECK(!backend_evdev_resume(&s));
        CHECK(st->suspended);
    }
    backend_evdev_shutdown(&s);check_all_closed();
}

static void test_virtual_device_is_excluded_before_grab(void){
    reset_fake();add_device(0,FAKE_KEYBOARD);
    snprintf(devices[0].name,sizeof(devices[0].name),"ChromeDrive test steering wheel");
    Server s;init_server(&s);s.drm_grab_input=true;
    CHECK(backend_evdev_init(&s));
    EvdevState *st=(EvdevState *)s.evdev_state;
    CHECK(st&&st->device_count==0);
    CHECK(devices[0].grab_calls==0);
    backend_evdev_shutdown(&s);check_all_closed();
}

/* Compositor/Wayland stubs record real queue-driven input dispatch. */
void app_release_input(Server *s,uint32_t time){(void)s;(void)time;}
bool app_arrange_pointer_button(Server *s,Hit hit,uint32_t button,bool down){(void)s;(void)hit;(void)button;(void)down;return false;}
bool app_arrange_pointer_wheel(Server *s,float delta){(void)s;(void)delta;return false;}
bool app_frame_pointer_button(Server *s,Hit hit,uint32_t button,bool down){(void)s;(void)hit;(void)button;(void)down;return false;}
void app_launch(Server *s,int slot,bool shift){(void)s;(void)slot;(void)shift;}
void app_open_settings(Server *s){(void)s;}
void app_open_settings_slot(Server *s,int slot,bool context){(void)s;(void)slot;(void)context;}
void app_request_quit(Server *s,bool force){(void)s;(void)force;}
void app_toggle_hud(Server *s){(void)s;}
bool app_global_key(Server *s,uint32_t key,bool down){(void)s;(void)key;(void)down;return false;}
void app_arrange_pointer_motion(Server *s,int x,int y){(void)s;(void)x;(void)y;}
void app_frame_pointer_motion(Server *s,int x,int y){(void)s;(void)x;(void)y;}
Hit scene_pick(Server *s,float x,float y,bool locked){(void)s;(void)x;(void)y;(void)locked;return (Hit){0};}
bool scene_wheel_button(Server *s,Hit hit,int x,int y,bool down,uint32_t time){(void)s;(void)hit;(void)x;(void)y;(void)down;(void)time;return false;}
void scene_wheel_motion(Server *s,int x,int y,uint32_t time){(void)s;(void)x;(void)y;(void)time;}
void server_key(Server *s,uint32_t key,bool down,uint32_t time){
    (void)time;key_calls++;last_key=(int)key;if(key<64)key_seen|=1ull<<key;
    /* Keep the production XKB transition in this transport-level harness;
     * other compositor/Wayland delivery remains a deliberately small stub. */
    if(s->xkb.state&&key<256){
        s->xkb_pressed[key]=down;
        xkb_state_update_key(s->xkb.state,key+8,down?XKB_KEY_DOWN:XKB_KEY_UP);
    }
}
void server_pointer_axis(Server *s,double horizontal,double vertical,uint32_t time){(void)s;(void)horizontal;(void)vertical;(void)time;}
void server_pointer_button(Server *s,uint32_t button,bool down,uint32_t time){(void)s;(void)button;(void)down;(void)time;}
void server_pointer_motion(Server *s,Hit hit,uint32_t time){(void)s;(void)hit;(void)time;}
bool server_touch_down(Server *s,Hit hit,int32_t id,uint32_t time){(void)s;(void)hit;(void)id;(void)time;return false;}
bool server_touch_motion(Server *s,int32_t id,float x,float y,uint32_t time){(void)s;(void)id;(void)x;(void)y;(void)time;return false;}
bool server_touch_up(Server *s,int32_t id,uint32_t time){(void)s;(void)id;(void)time;return false;}
void server_close_surface(Surface *surface){(void)surface;}
bool backend_vt_managed(Server *s){(void)s;return fake_vt_managed;}
bool backend_vt_request(Server *s,int vt){(void)s;vt_calls++;last_vt=vt;return true;}

int main(void){
    test_mouse_nodes_do_not_hide_keyboard();
    test_discovery_scans_past_unusable_candidates();
    test_explicit_devices_use_full_capacity();
    test_keyboard_led_sync();
    test_keyboard_led_read_only_fallback();
    test_fbdev_policy_remains_opt_in();
    test_startup_grab_failure_fails_closed();
    test_grab_suspend_resume_and_resume_failure();
    test_failed_release_cannot_resume_unverified_device();
    test_virtual_device_is_excluded_before_grab();
    if(failures){fprintf(stderr,"test_evdev: %d failure(s)\n",failures);return 1;}
    puts("test_evdev: ok");return 0;
}
