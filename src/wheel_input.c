#include "wheel_input.h"
#include <math.h>

void wheel_client_axis(float drag_dx, float *horizontal, float *vertical)
{
 /* Preserve the previous 28-pixel sensitivity as a scale, not a threshold.
  * Wayland's 24.8 fixed-point axis values retain these fractions, so even a
  * tiny drag produces a distinct analog value rather than waiting for a
  * detent. This is deliberately the HORIZONTAL axis: the continuous sender
  * negates deltas, so right = positive/right and left = negative/left at the
  * client. Vertical output remains exactly zero on the physical-wheel path. */
 const float pixels_per_axis_step=28.0f;
 *horizontal=-drag_dx/pixels_per_axis_step;
 *vertical=0.0f;
}

int wheel_client_scroll_steps(double *pending, double axis_value)
{
 *pending+=axis_value/15.0;
 /* floor(), including for negative values, keeps the remainder in [0,1).
  * Exact forward/reverse sequences therefore restore both the step count and
  * remainder. Truncating toward zero would drift by one row after reversal. */
 int steps=(int)floor(*pending);
 *pending-=steps;
 return steps;
}
