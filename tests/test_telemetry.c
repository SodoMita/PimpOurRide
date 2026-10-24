#include "chrome.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

int main(void) {
 Server s={.telemetry_fd=-1};vehicle_init(&s.vehicle);
 s.vehicle.speed=12.5f;s.vehicle.power_kw=88.0f;s.vehicle.motor_temp_c=47.0f;
 s.vehicle.battery_kwh=61.5;s.vehicle.distance_m=1234;s.steering_mode=VEHICLE_STEER_DRIVE;
 s.monitors[0].surface=(Surface*)1;
 assert(telemetry_init(&s));telemetry_update(&s);
 const char *fd_text=getenv("CHROMEDRIVE_TELEMETRY_FD");assert(fd_text);
 CdTelemetry *shared=mmap(NULL,sizeof(CdTelemetry),PROT_READ,MAP_SHARED,atoi(fd_text),0);assert(shared!=MAP_FAILED);
 CdTelemetry shot;assert(cd_telemetry_snapshot(shared,&shot));
 assert(shot.magic==CD_TELEMETRY_MAGIC&&shot.version==CD_TELEMETRY_VERSION&&!(shot.sequence&1));
 assert(fabsf(shot.speed_mps-12.5f)<.001f&&fabsf(shot.power_kw-88.0f)<.001f);
 assert(fabs(shot.battery_kwh-61.5)<.001&&shot.connected_screens==1&&shot.gear=='D'&&shot.drive_mode==VEHICLE_STEER_DRIVE);
 CdTelemetry invalid={0};assert(!cd_telemetry_snapshot(&invalid,&shot));
 munmap(shared,sizeof(CdTelemetry));telemetry_finish(&s);assert(s.telemetry_fd==-1&&!s.telemetry);
 puts("PASS: inherited EV telemetry memfd, seqlock snapshot, live vehicle values, cleanup");return 0;
}
