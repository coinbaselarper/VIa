#pragma once
#include "mac_window.h"

#ifdef __cplusplus
extern "C" {
#endif
int gui_has_display(void);
int gui_make_key(gta6rt_mac_window* win);
int gui_menu_items(void);
/* characters "F11" sends the F11 function key; ctrl and cmd are modifier flags. */
void gui_key(gta6rt_mac_window* win, const char* characters, unsigned short key_code, int ctrl, int cmd);
#ifdef __cplusplus
}
#endif
