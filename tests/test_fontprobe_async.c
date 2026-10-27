/* Regression for Russian selection: coverage probing must not be able to hold
 * startup or the compositor loop. This includes src/main.c directly, as the
 * other focused main-loop tests do, and uses a deliberately hanging helper. */
#define main chromedrive_embedded_main
#include "../src/main.c"
#undef main

#include <sys/stat.h>
#include <sys/time.h>

static int refresh_calls;
void ui_refresh_text(Server *s){(void)s;refresh_calls++;}
bool backend_vt_child_mask(sigset_t *mask){(void)mask;return false;}

#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);return 1;}}while(0)
static double now_ms(void){struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);return ts.tv_sec*1000.0+ts.tv_nsec/1e6;}
static int write_file(const char *path,const char *text){
 FILE *f=fopen(path,"w");if(!f)return -1;int rc=fputs(text,f)<0?-1:0;if(fclose(f)&&!rc)rc=-1;return rc;
}
static int reap_result(Server *s){
 for(int i=0;i<200&&s->font_probe_pid>=0;i++){font_probe_poll(s);usleep(5000);}
 return s->font_probe_pid<0?0:-1;
}
static int reap_generic_result(Server *s){
 /* Do not call font_probe_poll: the generic waitpid(-1) loop must route a
  * helper which exits in the inter-poll window to the same verdict handler. */
 for(int i=0;i<200&&s->font_probe_pid>=0;i++){reap_children(s);usleep(5000);}
 return s->font_probe_pid<0?0:-1;
}
int main(int argc,char **argv){
 const char *root=argc>1?argv[1]:".";
 unsetenv("CHROMEDRIVE_LANG");
 Server s;memset(&s,0,sizeof(s));s.font_probe_pid=-1;
 snprintf(s.root,sizeof(s.root),"%s",root);
 snprintf(language_request,sizeof(language_request),"ru");
 double begin=now_ms();apply_language(&s,true);
 CHECK(now_ms()-begin<500.0);
 CHECK(!strcmp(language_live,"ru"));
 CHECK(!strcmp(s.font_probe_pending,"ru"));
 CHECK(s.font_probe_pid==-1); /* nothing launches before the backend opens */
 CHECK(refresh_calls==0);

 /* A helper which never produces a verdict must remain pollable. Use exec so
  * terminating the child cannot leave a shell's sleep process behind. */
 char tmp[]="/tmp/chromedrive-fontprobe-async-XXXXXX";
 CHECK(mkdtemp(tmp));
 char bindir[512],helper[1024];
 snprintf(bindir,sizeof(bindir),"%s/bin",tmp);
 snprintf(helper,sizeof(helper),"%s/chromedrive-fontprobe",bindir);
 CHECK(mkdir(bindir,0700)==0);
 CHECK(write_file(helper,"#!/bin/sh\nexec sleep 30\n")==0);
 CHECK(chmod(helper,0700)==0);
 snprintf(s.root,sizeof(s.root),"%s",tmp);
 font_probe_schedule(&s,"ru");
 begin=now_ms();font_probe_start(&s);
 CHECK(now_ms()-begin<500.0);
 CHECK(s.font_probe_pid>=0);
 CHECK(!s.font_probe_pending[0]);
 pid_t hanging=s.font_probe_pid;
 for(int i=0;i<20;i++){
  begin=now_ms();font_probe_poll(&s);
  CHECK(now_ms()-begin<100.0);
  CHECK(s.font_probe_pid==hanging);
  usleep(2000);
 }
 CHECK(kill(hanging,0)==0); /* still hung, yet every poll returned */
 font_probe_finish(&s);
 int status=0;CHECK(waitpid(hanging,&status,0)==hanging);
 CHECK(s.font_probe_pid==-1);

 /* A current missing-glyph verdict still performs the English fallback. */
 snprintf(s.root,sizeof(s.root),"%s",root);
 snprintf(language_request,sizeof(language_request),"ru");
 language_failed[0]=0;
 CHECK(!strcmp(i18n_set_language(root,"ru"),"ru"));
 snprintf(language_live,sizeof(language_live),"ru");
 pid_t child=fork();CHECK(child>=0);if(child==0)_exit(3);
 s.font_probe_pid=child;snprintf(s.font_probe_lang,sizeof(s.font_probe_lang),"ru");
 s.buttons[0].texture=1;int refreshes_before=refresh_calls;
 CHECK(reap_generic_result(&s)==0);
 CHECK(!strcmp(language_live,"en"));
 CHECK(!strcmp(i18n_language(),"en"));
 CHECK(!strcmp(language_failed,"ru"));
 CHECK(refresh_calls==refreshes_before+1); /* English really is re-baked */
 CHECK(!s.notice[0]); /* helper status 3 is not a Wayland-client failure */
 s.buttons[0].texture=0;

 /* But a late verdict for an old catalog must never undo a newer selection. */
 language_failed[0]=0;
 snprintf(language_request,sizeof(language_request),"ja");
 CHECK(!strcmp(i18n_set_language(root,"ja"),"ja"));
 snprintf(language_live,sizeof(language_live),"ja");
 child=fork();CHECK(child>=0);if(child==0)_exit(3);
 s.font_probe_pid=child;snprintf(s.font_probe_lang,sizeof(s.font_probe_lang),"ru");
 CHECK(reap_result(&s)==0);
 CHECK(!strcmp(language_live,"ja"));
 CHECK(!strcmp(i18n_language(),"ja"));

 unlink(helper);rmdir(bindir);rmdir(tmp);i18n_free();
 printf("font probe async: startup queues only, hanging helper polls non-blockingly, generic-reaper fallback/rebake and stale-result guards pass\n");
 return 0;
}
