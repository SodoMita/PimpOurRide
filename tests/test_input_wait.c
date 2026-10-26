#include "input_wait.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

static int failures;
#define CHECK(cond) do{if(!(cond)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#cond);failures++;}}while(0)
static double seconds(void){struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);return ts.tv_sec+ts.tv_nsec/1e9;}
static void *delayed_write(void *arg){
 int fd=(int)(intptr_t)arg;struct timespec delay={.tv_sec=0,.tv_nsec=30000000};nanosleep(&delay,NULL);
 uint64_t one=1;CHECK(write(fd,&one,sizeof(one))==(ssize_t)sizeof(one));return NULL;
}
int main(void){
 CHECK(cd_input_wait(-1,0)<0);
 int fd=eventfd(0,EFD_NONBLOCK|EFD_CLOEXEC);CHECK(fd>=0);if(fd<0)return 1;
 double start=seconds();CHECK(cd_input_wait(fd,25)==0);double elapsed=seconds()-start;
 CHECK(elapsed>=.015&&elapsed<.30);
 uint64_t five=5;CHECK(write(fd,&five,sizeof(five))==(ssize_t)sizeof(five));
 CHECK(cd_input_wait(fd,0)==1);CHECK(cd_input_wait(fd,0)==0);
 pthread_t thread;CHECK(pthread_create(&thread,NULL,delayed_write,(void*)(intptr_t)fd)==0);
 start=seconds();CHECK(cd_input_wait(fd,500)==1);elapsed=seconds()-start;
 CHECK(elapsed>=.015&&elapsed<.40);pthread_join(thread,NULL);
 close(fd);if(failures)fprintf(stderr,"input_wait: %d failure(s)\n",failures);
 else printf("PASS: eventfd input wait timeout, coalescing, delayed wake, and error paths\n");
 return failures?1:0;
}
