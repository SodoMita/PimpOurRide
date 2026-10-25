#ifndef CD_WHEEL_INPUT_H
#define CD_WHEEL_INPUT_H

/* Convert one client-mode wheel drag sample into server_pointer_axis deltas.
 * The residual is retained across high-frequency RFB/SDL/evdev motion events. */
void wheel_client_axis(float *pending, float drag_dx,
                       float *horizontal, float *vertical);

#endif
