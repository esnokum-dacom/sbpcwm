
#pragma once

#include <X11/X.h>
#include <xcb/xcb.h>
#include <xcb/xcb_keysyms.h>

#include "sbpcwm.h"

struct key {
    unsigned int  mod;
    xcb_keysym_t  keysym;
    void        (*function)(const Arg arg);
    Arg     arg;
};

typedef struct {
    char *label;
    void (*function)(const Arg arg);
    Arg   arg;
} CtxItem;

typedef struct {
    unsigned int  mod;
    xcb_keysym_t  keysym;
    char        **cmd;
    int           ncmd;
} Shortcut;

typedef struct {
    char  *name;
    char *image;
    char  **cmd;
    int    ncmd;
    int    x, y;
    int    mon;
} LauncherIcon;

typedef struct Config {
    char *defaultsh;
    char *fonts;
    char *fontb;

    uint32_t pan_step;
    uint8_t titlebar;
    uint8_t focus_follow;

    uint8_t border;
    uint16_t border_width;

    char *ctxbg;
    char *ctxborder;
    char *deco;

    struct key *keys;
    int   nkeys;

    CtxItem *ctx;
    int   nctx;

    Shortcut *shortcuts;
    int       nshortcuts;

    LauncherIcon *icons;
    int           nicons;
} Config;

extern Config *cfg;

Config *config_load(const char *path);
void config_free(Config *cfg);
void config_load_icons(const char *path, Config *cfg);
void config_apply_icon_state(const char *path, Config *cfg);

unsigned int mod_from_name(const char *n);
