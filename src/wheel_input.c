#include "wheel_input.h"
#include <math.h>

void wheel_client_axis(float *pending, float drag_dx,
                       float *horizontal, float *vertical)
{
 const float pixels_per_detent=28.0f;
 *pending+=drag_dx;
 float detents=truncf(*pending/pixels_per_detent);
 if(detents!=0.0f)*pending-=detents*pixels_per_detent;

 /* Ordinary toolkit clients expect wheel input on the vertical axis. Sending
  * only horizontal axis events is protocol-valid but left apps such as Files
  * apparently unresponsive. server_pointer_axis negates its deltas, so these
  * signs produce right = down and left = up in the client. */
 *horizontal=0.0f;
 *vertical=-detents;
}
