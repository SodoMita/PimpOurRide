#include "chrome.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

bool telemetry_init(Server *s) {
 s->telemetry_fd=memfd_create("chromedrive-ev-telemetry",0); /* inherited by native clients */
 if(s->telemetry_fd<0||ftruncate(s->telemetry_fd,sizeof(CdTelemetry))<0){
  fprintf(stderr,"[telemetry] memfd setup failed: %s\n",strerror(errno));return false;
 }
 s->telemetry=mmap(NULL,sizeof(CdTelemetry),PROT_READ|PROT_WRITE,MAP_SHARED,s->telemetry_fd,0);
 if(s->telemetry==MAP_FAILED){s->telemetry=NULL;fprintf(stderr,"[telemetry] mmap failed: %s\n",strerror(errno));return false;}
 memset(s->telemetry,0,sizeof(*s->telemetry));s->telemetry->magic=CD_TELEMETRY_MAGIC;s->telemetry->version=CD_TELEMETRY_VERSION;
 char fd[24];snprintf(fd,sizeof(fd),"%d",s->telemetry_fd);setenv("CHROMEDRIVE_TELEMETRY_FD",fd,1);
 telemetry_update(s);return true;
}

void telemetry_update(Server *s) {
 CdTelemetry *t=s->telemetry;if(!t)return;
 uint32_t sequence=__atomic_load_n(&t->sequence,__ATOMIC_RELAXED);
 __atomic_store_n(&t->sequence,sequence+1,__ATOMIC_RELEASE);
 int screens=0;for(int i=0;i<CD_APP_BAYS;i++)if(s->monitors[i].surface)screens++;
 t->speed_mps=s->vehicle.speed;t->acceleration=s->vehicle.acceleration;
 t->power_kw=s->vehicle.power_kw;t->motor_temp_c=s->vehicle.motor_temp_c;
 t->hover_height=s->vehicle.hover_height;
 if(s->steering_mode==VEHICLE_STEER_CLIENT){
  t->wheel_angle=s->wheel_client_angle;
  t->steering=clampf(-s->wheel_client_angle/CD_WHEEL_MAX_ANGLE,-1.f,1.f);
 }else{
  t->wheel_angle=s->vehicle.wheel_angle;
  t->steering=s->vehicle.steering;
 }
 t->battery_kwh=s->vehicle.battery_kwh;t->battery_capacity_kwh=VEHICLE_BATTERY_CAPACITY_KWH;t->distance_m=s->vehicle.distance_m;
 t->connected_screens=(uint32_t)screens;t->drive_mode=(uint8_t)s->steering_mode;
 t->braking=s->drive_brake?1:0;t->collision=s->vehicle.collision?1:0;
 t->gear=s->steering_mode==VEHICLE_STEER_CLIENT?'C':s->vehicle.speed>.25f?'D':s->vehicle.speed<-.25f?'R':'N';
 __atomic_store_n(&t->sequence,sequence+2,__ATOMIC_RELEASE);
}

void telemetry_finish(Server *s) {
 if(s->telemetry){munmap(s->telemetry,sizeof(CdTelemetry));s->telemetry=NULL;}
 if(s->telemetry_fd>=0){close(s->telemetry_fd);s->telemetry_fd=-1;}
 unsetenv("CHROMEDRIVE_TELEMETRY_FD");
}
