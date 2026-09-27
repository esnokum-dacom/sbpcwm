#pragma once
#include <xcb/xcb.h>
#include <xcb/randr.h>
#include <X11/Xft/Xft.h>
#include <signal.h>
#include <stdlib.h>

#include "sbcct.h"

struct Config;

#define MAX_MONITORS 8
#define TITLEBAR_HEIGHT 25
#define SPAWN_SEARCH_STEP 30
#define SPAWN_SEARCH_MAX  40

#define PHYS_ENABLED      1
#define PHYS_RESTITUTION  0.70f  /* bounciness of window-window hits       */
#define PHYS_FRICTION     3.4f   /* exponential damping per second         */
#define PHYS_STOP_SPEED   8.0f   /* px/s below which a window sleeps       */
#define PHYS_VMAX         3200.0f
#define PHYS_SLOP         0.5f   /* allowed overlap, in px, before pushing */
#define PHYS_CORRECT      0.9f   /* fraction of the overlap solved per pass */
#define PHYS_BOUNCE_MIN   45.0f  /* closing speed under which a hit only
                                    separates, it does not bounce: keeps
                                    touching windows from shivering        */
#define PHYS_MAX_STEP     6.0f   /* px a window may travel per collision
                                    sub-step: above this it could jump
                                    clean over another window              */
#define PHYS_SUBSTEPS     16     /* upper bound on sub-steps per frame     */
#define PHYS_ITERATIONS   2      /* relaxation passes per sub-step         */
#define THROW_WINDOW_MS   120
#define THROW_BOOST       1.15f

#define FRAME_MIN_MS      10

#define win (client *t = 0, *c = list; c && t != list->prev; t = c, c = c->next)

#define canvas_to_screen(val, pan) (int)(((val) - (pan)))
#define screen_to_canvas(val, pan) ((val) / (pan))

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define CLAMP(v, lo, hi) (MAX((lo), MIN((v), (hi))))

#define mod_clean(mask) \
  (mask & ~(numlock | XCB_MOD_MASK_LOCK) & \
   (XCB_MOD_MASK_SHIFT | XCB_MOD_MASK_CONTROL | XCB_MOD_MASK_1 | XCB_MOD_MASK_2 | \
    XCB_MOD_MASK_3 | XCB_MOD_MASK_4 | XCB_MOD_MASK_5))

typedef struct Arg Arg;
struct Arg {
  const char **com;
  const int    i;
  const float  f;
  const xcb_window_t w;
};

typedef struct client {
  struct client *next, *prev;
  xcb_window_t w;
  xcb_window_t titlebar;
  int mon;
  int f;
  int wx, wy;
  int nvfc, isug;
  unsigned int ww, wh;
  int x, y;
  int width, height;
  int oldx, oldy, oldwidth, oldheight;
  unsigned char shaped, tb_shaped;
  int clip_r[4], tb_clip_r[4];
  int basew, baseh, incw, inch, maxw, maxh, minw, minh;
  float mina, maxa;
  float cx, cy;
  float vx, vy;
  uint8_t       vdepth;
  xcb_visualid_t vid;
  unsigned char awake;
} client;

typedef struct {
  float pan_x[MAX_MONITORS];
  float pan_y[MAX_MONITORS];
} canvas_state;

typedef struct {
  int x, y, w, h;
} MonitorInfo;

typedef struct {
  int x, y;
  int tx, ty;
  int active;
} MinimapState;

typedef struct {
  int w, h;
  int x, y;
} ButtonTb;

typedef struct {
  unsigned long cs[16];
  unsigned long background;
  unsigned long foreground;
  unsigned long deco;
  unsigned long deco_dim;
  unsigned long icon_text;
  unsigned short deco_rgb[3];
  unsigned short deco_dim_rgb[3];
} ColorScheme;

const char *get_home(void);
xcb_atom_t get_atom(const char *name);

extern xcb_window_t root;
extern Display *dpy;
extern xcb_connection_t *conn;
extern int scrno;
extern xcb_screen_t *screen;
extern Visual *visual;
extern Colormap cmap;
extern int depth;
extern ColorScheme cols;
extern MonitorInfo mons[MAX_MONITORS];
extern int n_mons;
extern canvas_state canvas;

extern XftFont *open_font(const char *name);

void ctx_close(void);
void ctx_open(int x, int y);

void button_press(xcb_button_press_event_t *gen_e);
void button_release(xcb_button_release_event_t *e);
void configure_request(xcb_configure_request_event_t *e);
void input_grab(xcb_window_t root);
void key_press(xcb_key_press_event_t *e);
xcb_keysym_t event_keysym(xcb_key_press_event_t *e);
int event_text(xcb_key_press_event_t *e, char *buf, size_t n);
void notify_property(xcb_property_notify_event_t *e);
void notify_unmap(xcb_unmap_notify_event_t *e);
void map_request(xcb_map_request_event_t *e);
void expose_event(xcb_expose_event_t *e);
void mapping_notify(xcb_mapping_notify_event_t *e);
void notify_motion(xcb_motion_notify_event_t *e);
void notify_screen_change(xcb_randr_screen_change_notify_event_t *e);
void notify_destroy(xcb_destroy_notify_event_t *gen_e);
void notify_enter(xcb_enter_notify_event_t *e);
void focusin(xcb_focus_in_event_t *e);
void client_message(xcb_generic_event_t *gen_e);

void run(const Arg arg);
void quit(const Arg arg);
void win_add(xcb_window_t w);
void win_center(const Arg arg);
void win_del(xcb_window_t w);
void win_fs(const Arg arg);
void win_focus(client *c);
void focus_win_id(xcb_window_t w);
void sbcs_manage(xcb_window_t w);
void titlebar_focus(xcb_window_t w);
void win_kill(const Arg arg);
void win_prev(const Arg arg);
void win_next(const Arg arg);
void win_round_corners(xcb_window_t w, int rad);
void move_nextmon(const Arg arg);
void ws_focusnext(const Arg arg);

void canvas_pan(int mon, float dx, float dy);
void canvas_pan_key(const Arg arg);
void canvas_reset(const Arg arg);
void canvas_apply_all(void);
void canvas_focus(client *c);

void physics_init(void);
void physics_tick(void);
void physics_wake(client *c);
void physics_sleep(client *c);
void physics_drag_start(client *c);
void physics_drag_release(client *c, int pointer_x, int pointer_y);
void physics_push_away(client *pusher);
void win_place_free(client *c);

void hud_update(void);

void titlebar_update(client *c);
xcb_window_t titlebar_create(client *c);
void titlebar_draw(client *c);
void titlebar_del(client *c);
client *client_from_titlebar(xcb_window_t w);
int is_titlebar(xcb_window_t w);
client *client_of_window(xcb_window_t w);

void update_borders(void);

void update_border_widths(void);
void fonts_reload(void);
void apply_titlebars(void);

unsigned long hex_to_xcolor(const char *hex);
void load_colors(void);
void apply_colors(void);
void titlebar_redraw_all(void);
void xcolor_to_xftcolor(unsigned long pixel, XftColor *xft);

void client_move(client *c, int x, int y);
void updatesizehints(client *c);
void resizeclient(client *c, int w, int h);
void configure(client *c);
void configure_notify(xcb_configure_notify_event_t *e);
void client_resize(client *c, unsigned int w, unsigned int h);
int applysizehints(client *c, int *w, int *h);
char *copystr(const char *s);
void win_size(xcb_window_t w, int *x, int *y, unsigned int *wd, unsigned int *ht);

void handle_sigusr2(int sig);

void reload_config(const Arg arg);
int  reload_config_quiet(int announce);

void update_client_list_stacking(void);

void notify_show(const char *msg, uint32_t bg);

void monitors_refresh(void);
int mon_at_ptr(void);
int mon_at_win(xcb_window_t w);
int mon_from_point(int px, int py);

void icons_load_state(struct Config *cfg);
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
