#include "evdev_queue.h"

bool cd_evdev_queue_init(CdEvdevQueue *q){
 if(!q)return false;
 q->head=q->tail=q->count=0;q->overflowed=false;q->dropped=0;
 return pthread_mutex_init(&q->lock,NULL)==0;
}
void cd_evdev_queue_destroy(CdEvdevQueue *q){if(q)pthread_mutex_destroy(&q->lock);}
bool cd_evdev_queue_push(CdEvdevQueue *q,const CdEvdevEvent *event){
 if(!q||!event)return false;
 pthread_mutex_lock(&q->lock);
 bool ok=false;
 if(q->overflowed){q->dropped++;}
 else if(q->count==CD_EVDEV_QUEUE_CAPACITY){q->overflowed=true;q->dropped=1;}
 else{
  q->events[q->head]=*event;q->head=(q->head+1)%CD_EVDEV_QUEUE_CAPACITY;q->count++;ok=true;
 }
 pthread_mutex_unlock(&q->lock);return ok;
}
bool cd_evdev_queue_pop(CdEvdevQueue *q,CdEvdevEvent *event){
 if(!q||!event)return false;
 pthread_mutex_lock(&q->lock);
 bool ok=q->count>0;
 if(ok){*event=q->events[q->tail];q->tail=(q->tail+1)%CD_EVDEV_QUEUE_CAPACITY;q->count--;}
 pthread_mutex_unlock(&q->lock);return ok;
}
bool cd_evdev_queue_overflowed(CdEvdevQueue *q){
 if(!q)return false;
 pthread_mutex_lock(&q->lock);bool overflowed=q->overflowed;pthread_mutex_unlock(&q->lock);return overflowed;
}
bool cd_evdev_queue_overflow_drained(CdEvdevQueue *q,uint64_t *dropped){
 if(!q)return false;
 pthread_mutex_lock(&q->lock);
 bool drained=q->overflowed&&q->count==0;
 if(drained&&dropped)*dropped=q->dropped;
 pthread_mutex_unlock(&q->lock);return drained;
}
void cd_evdev_queue_acknowledge(CdEvdevQueue *q){
 if(!q)return;
 pthread_mutex_lock(&q->lock);q->overflowed=false;q->dropped=0;pthread_mutex_unlock(&q->lock);
}
