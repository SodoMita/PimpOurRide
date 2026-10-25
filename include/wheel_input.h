#ifndef CD_WHEEL_INPUT_H
#define CD_WHEEL_INPUT_H

#include <stdbool.h>
#include <stdint.h>

/* Maximum visual & physical deflection of the 3D cockpit steering wheel
 * in degrees from center (lock-to-lock = 270 deg = [-135, +135]). */
#define CD_WHEEL_MAX_ANGLE 135.0f

/* Standard Linux evdev / SDL2 signed 16-bit joystick axis range. */
#define CD_WHEEL_JOY_MIN (-32767)
#define CD_WHEEL_JOY_MAX 32767

/* Standard USB XInput / Linux GameController identity (045e:028e v0114) so
 * SDL_GameControllerDB, SDL_Joystick, GLFW, Wine/Proton, RetroArch, and
 * browser Gamepad APIs recognize the steering wheel out of the box without
 * custom mapping files. */
#define CD_WHEEL_VENDOR_ID  0x045e
#define CD_WHEEL_PRODUCT_ID 0x028e
#define CD_WHEEL_VERSION_ID 0x0114
#define CD_WHEEL_DEVICE_NAME "ChromeDrive Virtual Steering Wheel"

typedef struct {
 int uinput_fd;
 int major_num,minor_num;
 int last_abs_x;
 bool active;
 bool created_node;
 bool created_udev;
 char sysname[64];
 char dev_node[128];
 char udev_file[128];
} WheelJoystick;

/* Advance the 3D cockpit steering wheel angle (in degrees, clamped to
 * [-CD_WHEEL_MAX_ANGLE, +CD_WHEEL_MAX_ANGLE]) from a pointer/touch drag.
 * In OpenGL (scene.c), positive wheel_angle rotates counter-clockwise (left),
 * so dragging right (drag_dx > 0) decreases angle_deg toward -135 deg. */
float wheel_client_angle_step(float angle_deg,float drag_dx);

/* Convert a 3D cockpit steering wheel angle in [-135, +135] degrees into a
 * normalized steering ratio in [-1.0, +1.0]:
 *   -1.0 = full left lock (+135 deg visual wheel rotation)
 *    0.0 = centered
 *   +1.0 = full right lock (-135 deg visual wheel rotation) */
float wheel_client_steer_from_angle(float angle_deg);

/* Map a 3D cockpit steering wheel angle in [-135, +135] degrees to a signed
 * 16-bit Linux joystick axis value in [-32767, +32767] for ABS_X / ABS_WHEEL. */
int wheel_client_joy_value(float angle_deg);

/* Create and register the kernel virtual joystick via /dev/uinput, ensuring
 * its /dev/input/eventN node and udev joystick metadata are present even in
 * headless containers without udevd. Safe to call when /dev/uinput is
 * unavailable (leaves joy->active = false and logs a notice). */
bool wheel_joystick_init(WheelJoystick *joy);

/* Emit EV_ABS (ABS_X and ABS_WHEEL) + EV_SYN (SYN_REPORT) for the given 3D
 * steering wheel angle in degrees. Redundant identical values are coalesced. */
bool wheel_joystick_update_angle(WheelJoystick *joy,float angle_deg);

/* Center and destroy the virtual joystick device and clean up any device node
 * or udev tag created by wheel_joystick_init(). */
void wheel_joystick_finish(WheelJoystick *joy);

/* Legacy continuous-scroll accumulator helper used by touchpad scrolling in
 * the demo file browser. */
void wheel_client_axis(float drag_dx,float *horizontal,float *vertical);
int wheel_client_scroll_steps(double *accum,double delta);

#endif
