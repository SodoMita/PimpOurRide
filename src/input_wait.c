#include "input_wait.h"

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

static int64_t wait_mono_ns(void){
 struct timespec ts;if(clock_gettime(CLOCK_MONOTONIC,&ts)<0)return 0;
 return (int64_t)ts.tv_sec*1000000000ll+ts.tv_nsec;
}

int cd_input_wait(int fd,int timeout_ms){
 if(fd<0||timeout_ms<0){errno=EINVAL;return -1;}
 int64_t deadline=wait_mono_ns()+(int64_t)timeout_ms*1000000ll;
 for(;;){
  int64_t left_ns=deadline-wait_mono_ns();
  int left=left_ns<=0?0:(int)((left_ns+999999ll)/1000000ll);
  struct pollfd pfd={.fd=fd,.events=POLLIN};
  int result=poll(&pfd,1,left);
  if(result<0&&errno==EINTR){if(wait_mono_ns()<deadline)continue;return 0;}
  if(result<0)return -1;
  if(result==0)return 0;
  if(!(pfd.revents&POLLIN)){errno=EIO;return -1;}
  /* eventfd's counter coalesces any number of producer wakeups. One read
   * drains them all; the following backend drain owns the corresponding
   * queue events. Never turn this into a timed polling loop. */
  uint64_t value;ssize_t bytes=read(fd,&value,sizeof(value));
  if(bytes==(ssize_t)sizeof(value))return 1;
  if(bytes<0&&(errno==EAGAIN||errno==EWOULDBLOCK))return 0;
  return -1;
 }
}
