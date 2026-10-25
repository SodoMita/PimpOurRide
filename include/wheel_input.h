#ifndef CD_WHEEL_INPUT_H
#define CD_WHEEL_INPUT_H

/* Convert one client-mode wheel drag sample into analog axis deltas. The
 * 28-pixel sensitivity scale is deliberately not a detent: sub-pixel and
 * fractional motion must reach the Wayland client instead of becoming a
 * binary/stateless wheel click. server_pointer_axis_continuous() applies the
 * final axis-unit scale and negation. */
void wheel_client_axis(float drag_dx, float *horizontal, float *vertical);

/* Convert a continuous axis value to whole client scroll steps while keeping
 * a non-negative fractional remainder. This is for row/list UIs only; the
 * compositor's analog protocol stream itself is never quantized. */
int wheel_client_scroll_steps(double *pending, double axis_value);

#endif
