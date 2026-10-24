#ifndef CHROMEDRIVE_KEYMAP_H
#define CHROMEDRIVE_KEYMAP_H
#include <stdint.h>
/* Pure RFB/X11 keysym -> Linux evdev translation, kept separate from the EGL
 * backend so the exact remote-driving/input mapping can be unit-tested. */
uint32_t keysym_to_evdev(uint32_t key);
#endif
