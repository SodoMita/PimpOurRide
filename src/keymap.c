#include "keymap.h"
#include <rfb/keysym.h>
#include <linux/input-event-codes.h>

/* X11 keysym -> Linux evdev keycode. Covers typing, navigation, function keys
 * and modifiers. libvncserver hands us rfbKeySym values (== X11 keysymdef
 * values), and both noVNC and native VNC clients depend on this exact table. */
uint32_t keysym_to_evdev(uint32_t key) {
 /* Linux evdev letter codes follow keyboard scan-code positions, not the
  * alphabet (KEY_B is 48, not KEY_A+1). Arithmetic mapping silently routes
  * valid RFB typing and WASD to unrelated keys, so use the explicit table. */
 static const uint32_t letters[26]={KEY_A,KEY_B,KEY_C,KEY_D,KEY_E,KEY_F,KEY_G,KEY_H,KEY_I,KEY_J,KEY_K,KEY_L,KEY_M,KEY_N,KEY_O,KEY_P,KEY_Q,KEY_R,KEY_S,KEY_T,KEY_U,KEY_V,KEY_W,KEY_X,KEY_Y,KEY_Z};
 if(key>='a'&&key<='z')return letters[key-'a'];
 if(key>='A'&&key<='Z')return letters[key-'A'];
 if(key>='0'&&key<='9'){static const uint32_t row[10]={KEY_0,KEY_1,KEY_2,KEY_3,KEY_4,KEY_5,KEY_6,KEY_7,KEY_8,KEY_9};return row[key-'0'];}
 if(key>=XK_KP_0&&key<=XK_KP_9){static const uint32_t kp[10]={KEY_KP0,KEY_KP1,KEY_KP2,KEY_KP3,KEY_KP4,KEY_KP5,KEY_KP6,KEY_KP7,KEY_KP8,KEY_KP9};return kp[key-XK_KP_0];}
 switch(key){
 case XK_space:case XK_KP_Space:return KEY_SPACE;case XK_Return:return KEY_ENTER;case XK_KP_Enter:return KEY_KPENTER;case XK_BackSpace:return KEY_BACKSPACE;case XK_Tab:return KEY_TAB;case XK_Escape:return KEY_ESC;case XK_Delete:return KEY_DELETE;
 case XK_minus:return KEY_MINUS;case XK_KP_Subtract:return KEY_KPMINUS;case XK_equal:return KEY_EQUAL;case XK_KP_Add:return KEY_KPPLUS;case XK_bracketleft:return KEY_LEFTBRACE;case XK_bracketright:return KEY_RIGHTBRACE;
 case XK_semicolon:return KEY_SEMICOLON;case XK_apostrophe:return KEY_APOSTROPHE;case XK_grave:return KEY_GRAVE;case XK_backslash:return KEY_BACKSLASH;
 case XK_comma:return KEY_COMMA;case XK_period:return KEY_DOT;case XK_slash:return KEY_SLASH;
 case XK_Up:return KEY_UP;case XK_Down:return KEY_DOWN;case XK_Left:return KEY_LEFT;case XK_Right:return KEY_RIGHT;
 case XK_Home:return KEY_HOME;case XK_End:return KEY_END;case XK_Page_Up:return KEY_PAGEUP;case XK_Page_Down:return KEY_PAGEDOWN;case XK_Insert:return KEY_INSERT;
 case XK_Shift_L:return KEY_LEFTSHIFT;case XK_Shift_R:return KEY_RIGHTSHIFT;case XK_Control_L:return KEY_LEFTCTRL;case XK_Control_R:return KEY_RIGHTCTRL;
 case XK_Alt_L:return KEY_LEFTALT;case XK_Alt_R:return KEY_RIGHTALT;case XK_Super_L:return KEY_LEFTMETA;case XK_Super_R:return KEY_RIGHTMETA;
 case XK_Caps_Lock:return KEY_CAPSLOCK;case XK_Num_Lock:return KEY_NUMLOCK;case XK_Scroll_Lock:return KEY_SCROLLLOCK;case XK_Menu:return KEY_MENU;
 case XK_F1:return KEY_F1;case XK_F2:return KEY_F2;case XK_F3:return KEY_F3;case XK_F4:return KEY_F4;case XK_F5:return KEY_F5;case XK_F6:return KEY_F6;
 case XK_F7:return KEY_F7;case XK_F8:return KEY_F8;case XK_F9:return KEY_F9;case XK_F10:return KEY_F10;case XK_F11:return KEY_F11;case XK_F12:return KEY_F12;
 default:return 0;
 }
}
