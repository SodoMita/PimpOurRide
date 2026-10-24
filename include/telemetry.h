#ifndef CD_TELEMETRY_H
#define CD_TELEMETRY_H
#include <stdbool.h>
#include <stdint.h>

/* Shared read-only view consumed by the native instrument-cluster client.
 * The compositor owns one memfd and publishes a snapshot at presentation rate;
 * clients inherit the fd at launch and mmap it without a socket/protocol
 * extension. A seqlock prevents a 30 Hz client from observing a mixture of
 * two vehicle states on an unlucky preemption. */
#define CD_TELEMETRY_MAGIC 0x43445431u /* "CDT1" */
#define CD_TELEMETRY_VERSION 1u

typedef struct {
 uint32_t magic,version,sequence;
 float speed_mps,acceleration,power_kw,motor_temp_c,hover_height,steering,wheel_angle;
 double battery_kwh,battery_capacity_kwh,distance_m;
 uint32_t connected_screens;
 uint8_t drive_mode,gear,braking,collision;
} CdTelemetry;

static inline bool cd_telemetry_snapshot(const CdTelemetry *shared,CdTelemetry *out) {
 if(!shared||!out)return false;
 for(int attempt=0;attempt<8;attempt++){
  uint32_t before=__atomic_load_n(&shared->sequence,__ATOMIC_ACQUIRE);
  if(before&1)continue;
  CdTelemetry copy=*shared;
  uint32_t after=__atomic_load_n(&shared->sequence,__ATOMIC_ACQUIRE);
  if(before==after&&!(after&1)&&copy.magic==CD_TELEMETRY_MAGIC&&copy.version==CD_TELEMETRY_VERSION){*out=copy;return true;}
 }
 return false;
}
#endif
