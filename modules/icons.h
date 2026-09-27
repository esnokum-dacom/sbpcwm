#pragma once

#include "sbcct.h"
#include "sbpcwm.h"

void icons_load_state(Config *cfg);
void icons_rebuild(void);
void icons_save(void);
void icons_reposition(void);
void icons_cleanup(void);
void icons_lower(void);
int  icons_visible(void);
int  icon_window_is_icon(xcb_window_t w);
void toggle_icons(const Arg arg);
int  icon_handle_press(xcb_button_press_event_t *e);
int  icon_handle_motion(xcb_motion_notify_event_t *e);
int  icon_handle_release(xcb_button_release_event_t *e);
int  icons_redraw_win(xcb_window_t w);

int  icons_draw_thumb(Drawable d, const char *path, int x, int y,
                      int w, int h, unsigned long bg);
