#ifndef EVDEV_QUEUE_H
#define EVDEV_QUEUE_H

#include <linux/input.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* One worker produces and the compositor thread consumes. The mutex also
 * provides the overflow barrier: once full, the producer stays blocked from
 * adding more events until the consumer has drained the old ordered prefix,
 * released every potentially latched input, and acknowledged the reset. */
#define CD_EVDEV_QUEUE_CAPACITY 4096

typedef enum {
 CD_EVDEV_EVENT_RAW = 0,
 CD_EVDEV_EVENT_DISCONNECT = 1
} CdEvdevEventKind;

typedef struct {
 struct input_event input;
 uint32_t time; /* CLOCK_MONOTONIC milliseconds, as expected by wl_seat */
 uint8_t device; /* stable EvdevState slot, never a compacting array pointer */
 uint8_t kind;
} CdEvdevEvent;

typedef struct {
 pthread_mutex_t lock;
 CdEvdevEvent events[CD_EVDEV_QUEUE_CAPACITY];
 size_t head,tail,count;
 bool overflowed;
 uint64_t dropped;
} CdEvdevQueue;

bool cd_evdev_queue_init(CdEvdevQueue *q);
void cd_evdev_queue_destroy(CdEvdevQueue *q);
bool cd_evdev_queue_push(CdEvdevQueue *q,const CdEvdevEvent *event);
bool cd_evdev_queue_pop(CdEvdevQueue *q,CdEvdevEvent *event);
bool cd_evdev_queue_overflowed(CdEvdevQueue *q);
/* Returns true exactly when a drained overflow needs a compositor-side input
 * reset. The acknowledgement reopens the producer only after that reset. */
bool cd_evdev_queue_overflow_drained(CdEvdevQueue *q,uint64_t *dropped);
/* Called after the reset, not merely after the queue becomes empty. */
void cd_evdev_queue_acknowledge(CdEvdevQueue *q);

#endif
