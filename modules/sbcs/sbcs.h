#pragma once

#include "sbcct.h"
#include "sbpcwm.h"

#define SBCS_W        600
#define SBCS_H        400
#define SBCS_MIN_W    480
#define SBCS_MIN_H    280
#define SBCS_NAV_W    168
#define SBCS_ITEM_H    26
#define SBCS_ROW_H     24
#define SBCS_FIELD_W   232
#define SBCS_FIELD_H   20
#define SBCS_BOX       16
#define SBCS_FOOT_H    22
#define SBCS_PAD       14
#define SBCS_VAL_LEN  512
#define SBCS_MAX_OPTS  16
#define SBCS_MAX_ROWS  32
#define SBCS_CHIP_W    46
#define SBCS_CHIP_H    18
#define SBCS_DEL_W     20
#define SBCS_THUMB     16
#define SBCS_KEY_W     84
#define SBCS_NAME_W    96
#define SBCS_IMG_W    190
#define SBCS_CTX_LBL_W 120
#define SBCS_CTX_FN_W  120

#define SBCS_MOD_MOD   1
#define SBCS_MOD_ALT   2
#define SBCS_MOD_CTRL  4
#define SBCS_MOD_SHIFT 8

typedef struct {
    char     key[64];
    unsigned mods;
    char     cmd[256];
    char     arg[64];
    int      quit;
} SbcsKeyRow;

typedef struct {
    char name[64];
    char image[256];
    char cmd[256];
    int  x, y, mon;
    char image_src[256];
} SbcsIconRow;

typedef struct {
    char label[64];
    char func[64];
    char arg[256];
} SbcsCtxRow;

extern SbcsKeyRow  sbcs_keyrows[SBCS_MAX_ROWS];
extern int         sbcs_nkeys;
extern SbcsIconRow sbcs_iconrows[SBCS_MAX_ROWS];
extern int         sbcs_nicons;
extern SbcsCtxRow  sbcs_ctxrows[SBCS_MAX_ROWS];
extern int         sbcs_nctx;

extern xcb_window_t sbcs_win;

void sbcs_open(void);
void sbcs_close(void);
void sbcs_refresh(void);
void sbcs_cleanup(void);

int sbcs_config_save(void);

int sbcs_lists_save(const char *path);

void sbcs_lists_load(const char *path);

int  sbcs_handle_press(xcb_button_press_event_t *e);
int  sbcs_handle_motion(xcb_motion_notify_event_t *e);
int  sbcs_handle_key(xcb_key_press_event_t *e);
int  sbcs_redraw(xcb_expose_event_t *e);
int  sbcs_handle_configure(xcb_configure_notify_event_t *e);
