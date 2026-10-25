#include "wheel_input.h"
#include <math.h>
#include <stdio.h>

static int failures;
static void expect_axis(float drag,float horizontal,float vertical){
 float h=123.0f,v=123.0f;
 wheel_client_axis(drag,&h,&v);
 if(fabsf(h-horizontal)>0.000001f||fabsf(v-vertical)>0.000001f){
  fprintf(stderr,"FAIL drag %.9f: axis (%.9f, %.9f); expected (%.9f, %.9f)\n",
          drag,h,v,horizontal,vertical);
  failures++;
 }
}
int main(void){
 expect_axis(0.0f,0,0);
 expect_axis(0.25f,0,-0.25f/28.0f);      /* no 28-pixel detent threshold */
 expect_axis(27.75f,0,-27.75f/28.0f);    /* fractional below old detent */
 expect_axis(84.0f,0,-3);                /* high-speed samples stay linear */
 expect_axis(-56.0f,0,2);                /* left: vertical up */
 expect_axis(-0.25f,0,0.25f/28.0f);      /* immediately reversible */
 expect_axis(1.0f/1024.0f,0,-1.0f/(1024.0f*28.0f)); /* beyond 8-bit resolution */

 float sum=0.0f;
 const float sequence[]={0.125f,3.5f,19.25f,-11.0f,-11.875f};
 for(unsigned i=0;i<sizeof(sequence)/sizeof(sequence[0]);i++){
  float h,v;wheel_client_axis(sequence[i],&h,&v);sum+=v;
 }
 if(fabsf(sum)>0.000001f){
  fprintf(stderr,"FAIL reversible analog sum %.9f\n",sum);failures++;
 }

 double pending=0.0;
 if(wheel_client_scroll_steps(&pending,15.0)!=1||fabs(pending)>1e-12||
    wheel_client_scroll_steps(&pending,-15.0)!=-1||fabs(pending)>1e-12){
  fprintf(stderr,"FAIL whole continuous scroll steps, remainder %.12f\n",pending);failures++;
 }
 /* Exact fixed-point values captured from a 12 px drag followed by varied
  * +53/-53 px motion. Files may quantize its row-based drawing, but this
  * sequence must not drift or lose the original fractional remainder. */
 const double varied[]={0.53515625,1.07031250,1.60546875,2.67578125,
                        4.28515625,6.96093750,11.25000000};
 int steps=0;pending=0.0;
 for(int i=0;i<12;i++)steps+=wheel_client_scroll_steps(&pending,0.53515625);
 for(unsigned i=0;i<sizeof(varied)/sizeof(varied[0]);i++)
  steps+=wheel_client_scroll_steps(&pending,varied[i]);
 for(unsigned i=0;i<sizeof(varied)/sizeof(varied[0]);i++)
  steps+=wheel_client_scroll_steps(&pending,-varied[sizeof(varied)/sizeof(varied[0])-1-i]);
 if(steps!=0||fabs(pending-0.428125)>1e-9){
  fprintf(stderr,"FAIL scroll reversal: steps %d, remainder %.12f\n",steps,pending);failures++;
 }
 if(failures)return 1;
 puts("PASS: steering-wheel analog fractions, high resolution, direction and reversible client scrolling");
 return 0;
}
