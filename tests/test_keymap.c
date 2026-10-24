#include "keymap.h"
#include <linux/input-event-codes.h>
#include <rfb/keysym.h>
#include <stdio.h>

static int failures;
#define EXPECT(keysym,evdev) do { uint32_t actual=keysym_to_evdev(keysym); \
 if(actual!=(evdev)){fprintf(stderr,"FAIL keysym 0x%x: got %u expected %u\n",(unsigned)(keysym),actual,(unsigned)(evdev));failures++;} } while(0)
int main(void){
 /* Regression: evdev letters are not contiguous, especially B and W. */
 EXPECT('a',KEY_A);EXPECT('b',KEY_B);EXPECT('w',KEY_W);EXPECT('s',KEY_S);EXPECT('d',KEY_D);EXPECT('z',KEY_Z);
 EXPECT('A',KEY_A);EXPECT('W',KEY_W);EXPECT('Z',KEY_Z);
 EXPECT('0',KEY_0);EXPECT('7',KEY_7);EXPECT(XK_F5,KEY_F5);EXPECT(XK_F7,KEY_F7);EXPECT(XK_F8,KEY_F8);EXPECT(XK_F9,KEY_F9);EXPECT(XK_F10,KEY_F10);
 EXPECT(XK_Up,KEY_UP);EXPECT(XK_Control_L,KEY_LEFTCTRL);EXPECT(XK_space,KEY_SPACE);
 EXPECT(0,0);EXPECT(0x7fffffffu,0);
 if(failures)return 1;
 puts("PASS: RFB/X11 keysyms map to exact evdev typing, WASD, arrows and function keys");
 return 0;
}
