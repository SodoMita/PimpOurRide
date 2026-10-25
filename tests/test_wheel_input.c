#include "wheel_input.h"
#include <math.h>
#include <stdio.h>

static int failures;
static void expect_axis(float *pending,float drag,float horizontal,float vertical,float residual){
 float h=123.0f,v=123.0f;
 wheel_client_axis(pending,drag,&h,&v);
 if(fabsf(h-horizontal)>0.0001f||fabsf(v-vertical)>0.0001f||
    fabsf(*pending-residual)>0.0001f){
  fprintf(stderr,"FAIL drag %.3f: axis (%.3f, %.3f), residual %.3f; expected (%.3f, %.3f), %.3f\n",
          drag,h,v,*pending,horizontal,vertical,residual);
  failures++;
 }
}
int main(void){
 float pending=0.0f;
 expect_axis(&pending,27.75f,0,0,27.75f);       /* below one detent */
 expect_axis(&pending,0.25f,0,-1,0);           /* right: vertical down */
 expect_axis(&pending,10.0f,0,0,10.0f);        /* retain fractional input */
 expect_axis(&pending,74.0f,0,-3,0);           /* coalesce high-speed motion */
 expect_axis(&pending,-56.0f,0,2,0);           /* left: vertical up */
 expect_axis(&pending,-13.0f,0,0,-13.0f);
 expect_axis(&pending,13.0f,0,0,0);            /* reversible without drift */
 expect_axis(&pending,0.0f,0,0,0);
 if(failures)return 1;
 puts("PASS: steering-wheel detents, residuals, direction and vertical Wayland axis routing");
 return 0;
}
