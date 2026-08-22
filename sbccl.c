
/* Sowm C Binding Lua config */

#include <X11/X.h>
#include <X11/Xlib.h>
#include <lua5.3/lua.h>
#include <lua5.3/lualib.h>
#include <lua5.3/lauxlib.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "sbcct.h"

typedef struct { const char *name; void (*fn)(const Arg); } FuncEntry;
typedef struct { const char *name; unsigned int mask; } ModEntry;

static void *dupstr(const char *s);
static void (*func_from_name(const char *n))(const Arg);
static void build_key(lua_State *L, int idx, struct key *k);
void config_free(Config *cfg);static void *dupstr(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;

    char *r = malloc(n);

    memcpy(r, s, n);

    return r;
}

static FuncEntry funcs[] = {
    {"win_kill", win_kill}, {"win_center", win_center}, {"win_fs", win_fs},
    {"run", run}, {"quit", quit}, {"canvas_pan_key", canvas_pan_key},
    {"canvas_reset", canvas_reset},     {"move_nextmon", move_nextmon},
    {"ws_focusnext", ws_focusnext},
    {"toggle_icons", toggle_icons},
    {"reload_config", reload_config},
};

static ModEntry mods[] = {
    {"super", Mod4Mask}, {"shift", ShiftMask},
    {"ctrl", ControlMask}, {"alt", Mod1Mask},
};

static void (*func_from_name(const char *n))(const Arg) {
    for (size_t i = 0; i < sizeof(funcs)/sizeof(*funcs); i++)
        if (n && !strcmp(funcs[i].name, n)) return funcs[i].fn;
    return NULL;
}

unsigned int mod_from_name(const char *n) {
    for (size_t i = 0; i < sizeof(mods)/sizeof(*mods); i++)
        if (n && !strcmp(mods[i].name, n)) return mods[i].mask;
    return 0;
}

static void build_key(lua_State *L, int idx, struct key *k) {
    memset(k, 0, sizeof(*k));

    unsigned int mod = 0;
    lua_getfield(L, idx, "mod");
    if (lua_istable(L, -1)) {
        int n = lua_rawlen(L, -1);
        for (int i = 1; i <= n; i++) {
            lua_rawgeti(L, -1, i);
            mod |= mod_from_name(lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    k->mod = mod;

    lua_getfield(L, idx, "key");
    const char *keystr = lua_tostring(L, -1);
    k->keysym = keystr ? XStringToKeysym(keystr) : NoSymbol;
    lua_pop(L, 1);

    lua_getfield(L, idx, "func");
    k->function = func_from_name(lua_tostring(L, -1));
    if (!k->function)
	fprintf(stderr, "sbpcwm: unknown func '%s' in config.lua\n", lua_tostring(L, -1));
    lua_pop(L, 1);

    Arg a = {0};
    lua_getfield(L, idx, "arg");
    if (lua_istable(L, -1)) {
        int n = lua_rawlen(L, -1);
        const char **argv = malloc(sizeof(char*) * (n + 1));
        for (int i = 1; i <= n; i++) {
            lua_rawgeti(L, -1, i);
            argv[i-1] = dupstr(lua_tostring(L, -1));
            lua_pop(L, 1);
        }
        argv[n] = NULL;
        Arg tmp = { .com = argv };
        memcpy(&a, &tmp, sizeof(Arg));
    } else if (lua_isnumber(L, -1)) {
        double d = lua_tonumber(L, -1);
        Arg tmp = { .i = (int)d, .f = (float)d };
        memcpy(&a, &tmp, sizeof(Arg));
    }
    lua_pop(L, 1);
    memcpy(&k->arg, &a, sizeof(Arg));
}

static int opt_int(lua_State *L, const char *key, int def) {
    lua_getfield(L, -1, key);
    int v = lua_isnumber(L, -1) ? (int)lua_tonumber(L, -1) : def;
    lua_pop(L, 1);
    return v;
}

static const char *opt_str(lua_State *L, const char *key, const char *def) {
    lua_getfield(L, -1, key);
    const char *v = lua_isstring(L, -1) ? lua_tostring(L, -1) : def;
    lua_pop(L, 1);
    return v;
}

static void free_icons(Config *c) {
    if (!c->icons) return;
    for (int i = 0; i < c->nicons; i++) {
        free(c->icons[i].name);
        free(c->icons[i].image);
        if (c->icons[i].cmd) {
            for (int j = 0; j < c->icons[i].ncmd; j++)
                free(c->icons[i].cmd[j]);
            free(c->icons[i].cmd);
        }
    }
    free(c->icons);
    c->icons = NULL;
    c->nicons = 0;
}

static void parse_icons_table(lua_State *L, int idx, Config *c) {
    free_icons(c);
    if (!lua_istable(L, idx)) return;

    int n = lua_rawlen(L, idx);
    c->icons = calloc((size_t)n, sizeof(LauncherIcon));
    c->nicons = n;

    for (int i = 1; i <= n; i++) {
        lua_rawgeti(L, idx, i);
        int tidx = lua_gettop(L);
        LauncherIcon *ic = &c->icons[i - 1];

        lua_getfield(L, tidx, "name");
        ic->name = dupstr(lua_tostring(L, -1));
        lua_pop(L, 1);

        lua_getfield(L, tidx, "image");
        ic->image = dupstr(lua_tostring(L, -1));
        lua_pop(L, 1);

        lua_getfield(L, tidx, "x");
        if (lua_isnumber(L, -1)) ic->x = (int)lua_tonumber(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, tidx, "y");
        if (lua_isnumber(L, -1)) ic->y = (int)lua_tonumber(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, tidx, "mon");
        if (lua_isnumber(L, -1)) ic->mon = (int)lua_tonumber(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, tidx, "cmd");
        if (lua_istable(L, -1)) {
            int na = lua_rawlen(L, -1);
            ic->cmd = malloc(sizeof(char *) * (na + 1));
            for (int j = 1; j <= na; j++) {
                lua_rawgeti(L, -1, j);
                ic->cmd[j - 1] = dupstr(lua_tostring(L, -1));
                lua_pop(L, 1);
            }
            ic->cmd[na] = NULL;
            ic->ncmd = na;
        }
        lua_pop(L, 1);

        lua_pop(L, 1);
    }
}

Config *config_load(const char *path) {
    lua_State *L = luaL_newstate();
    luaL_openlibs(L);

    if (luaL_dofile(L, path) != LUA_OK) {
        fprintf(stderr, "Sbcwm - %s\n", lua_tostring(L, -1));
        lua_close(L);
        return NULL;
    }

    Config *cfg = malloc(sizeof(Config));

    cfg->pan_step     = 120;
    cfg->titlebar     = 0;
    cfg->ui           = 1;
    cfg->xr_colors    = 1;
    cfg->border       = 1;
    cfg->border_width = 1;
    cfg->defaultsh    = dupstr("/bin/sh");
    cfg->fonts        = dupstr("Terminus:style=Regular:pixelsize=16:antialias=false");
    cfg->fontb        = dupstr("FiraMonoNerdFont:style=Regular:pixelsize=20:antialias=false");
    cfg->ctxbg        = NULL;
    cfg->ctxborder    = NULL;
    cfg->shortcuts    = NULL;
    cfg->nshortcuts   = 0;
    cfg->icons        = NULL;
    cfg->nicons       = 0;

    lua_getglobal(L, "opts");
    if (lua_istable(L, -1)) {
        cfg->pan_step     = (uint32_t)opt_int(L, "pan_step",     120);
        cfg->titlebar     = (uint8_t)opt_int(L,  "titlebar",     0);
        cfg->ui           = (uint8_t)opt_int(L,  "ui",           1);
        cfg->xr_colors    = (uint8_t)opt_int(L,  "xr_colors",    1);
        cfg->border       = (uint8_t)opt_int(L,  "border",       1);
        cfg->border_width = (uint16_t)opt_int(L, "border_width", 1);
        const char *cbg = opt_str(L, "ctxbg", NULL);
        if (cbg) cfg->ctxbg = dupstr(cbg);
        const char *cbd = opt_str(L, "ctxborder", NULL);
        if (cbd) cfg->ctxborder = dupstr(cbd);
    }
    lua_pop(L, 1);

    lua_getglobal(L, "defaultsh");
    if (lua_isstring(L, -1)) { free(cfg->defaultsh); cfg->defaultsh = dupstr(lua_tostring(L, -1)); }
    lua_pop(L, 1);

    lua_getglobal(L, "fonts");
    if (lua_isstring(L, -1)) { free(cfg->fonts); cfg->fonts = dupstr(lua_tostring(L, -1)); }
    lua_pop(L, 1);

    lua_getglobal(L, "fontb");
    if (lua_isstring(L, -1)) { free(cfg->fontb); cfg->fontb = dupstr(lua_tostring(L, -1)); }
    lua_pop(L, 1);

    lua_getglobal(L, "keys");
    if (lua_istable(L, -1)) {
        int n = lua_rawlen(L, -1);
        cfg->keys = malloc(sizeof(struct key) * n);
        cfg->nkeys = n;
        for (int i = 1; i <= n; i++) {
            lua_rawgeti(L, -1, i);
            build_key(L, lua_gettop(L), &cfg->keys[i - 1]);
            lua_pop(L, 1);
        }
    } else {
        cfg->keys = NULL;
        cfg->nkeys = 0;
    }
    lua_pop(L, 1);

    lua_getglobal(L, "ctx");
    if (lua_istable(L, -1)) {
        int n = lua_rawlen(L, -1);
        cfg->ctx = calloc((size_t)n, sizeof(CtxItem));
        cfg->nctx = n;
        for (int i = 1; i <= n; i++) {
            lua_rawgeti(L, -1, i);
            int idx = lua_gettop(L);
            CtxItem *item = &cfg->ctx[i - 1];

            lua_getfield(L, idx, "label");
            item->label = dupstr(lua_tostring(L, -1));
            lua_pop(L, 1);

            lua_getfield(L, idx, "func");
            item->function = func_from_name(lua_tostring(L, -1));
            if (!item->function)
                fprintf(stderr, "sbpcwm: unknown func '%s' in config.lua ctx\n", lua_tostring(L, -1));
            lua_pop(L, 1);

            Arg a = {0};
            lua_getfield(L, idx, "arg");
            if (lua_istable(L, -1)) {
                int na = lua_rawlen(L, -1);
                const char **argv = malloc(sizeof(char *) * (na + 1));
                for (int j = 1; j <= na; j++) {
                    lua_rawgeti(L, -1, j);
                    argv[j - 1] = dupstr(lua_tostring(L, -1));
                    lua_pop(L, 1);
                }
                argv[na] = NULL;
                Arg tmp = { .com = argv };
                memcpy(&a, &tmp, sizeof(Arg));
            } else if (lua_isnumber(L, -1)) {
                double d = lua_tonumber(L, -1);
                Arg tmp = { .i = (int)d, .f = (float)d };
                memcpy(&a, &tmp, sizeof(Arg));
            }
            lua_pop(L, 1);
            memcpy(&item->arg, &a, sizeof(Arg));

            lua_pop(L, 1);
        }
    } else {
        cfg->ctx = NULL;
        cfg->nctx = 0;
    }
    lua_pop(L, 1);

    lua_getglobal(L, "shortcuts");
    if (lua_istable(L, -1)) {
        int n = lua_rawlen(L, -1);
        cfg->shortcuts = calloc((size_t)n, sizeof(Shortcut));
        cfg->nshortcuts = n;
        for (int i = 1; i <= n; i++) {
            lua_rawgeti(L, -1, i);
            int idx = lua_gettop(L);
            Shortcut *s = &cfg->shortcuts[i - 1];

            unsigned int mod = 0;
            lua_getfield(L, idx, "mod");
            if (lua_istable(L, -1)) {
                int nm = lua_rawlen(L, -1);
                for (int j = 1; j <= nm; j++) {
                    lua_rawgeti(L, -1, j);
                    mod |= mod_from_name(lua_tostring(L, -1));
                    lua_pop(L, 1);
                }
            }
            lua_pop(L, 1);
            s->mod = mod;

            lua_getfield(L, idx, "key");
            s->keysym = XStringToKeysym(lua_tostring(L, -1));
            lua_pop(L, 1);

            lua_getfield(L, idx, "arg");
            if (lua_istable(L, -1)) {
                int na = lua_rawlen(L, -1);
                s->cmd = malloc(sizeof(char *) * (na + 1));
                for (int j = 1; j <= na; j++) {
                    lua_rawgeti(L, -1, j);
                    s->cmd[j - 1] = dupstr(lua_tostring(L, -1));
                    lua_pop(L, 1);
                }
                s->cmd[na] = NULL;
                s->ncmd = na;
            }
            lua_pop(L, 1);

            lua_pop(L, 1);
        }
    } else {
        cfg->shortcuts = NULL;
        cfg->nshortcuts = 0;
    }
    lua_pop(L, 1);

    lua_getglobal(L, "icons");
    parse_icons_table(L, lua_gettop(L), cfg);
    lua_pop(L, 1);

    lua_close(L);
    return cfg;
}

void config_load_icons(const char *path, Config *c) {
    lua_State *L = luaL_newstate();
    luaL_openlibs(L);

    if (luaL_dofile(L, path) != LUA_OK) {
        fprintf(stderr, "Sbcwm icons - %s\n", lua_tostring(L, -1));
        lua_close(L);
        return;
    }

    lua_getglobal(L, "icons");
    parse_icons_table(L, lua_gettop(L), c);
    lua_pop(L, 1);

    lua_close(L);
}

void config_free(Config *cfg) {
    if (!cfg) return;
    free(cfg->defaultsh);
    free(cfg->fonts);
    free(cfg->fontb);
    for (int i = 0; i < cfg->nkeys; i++) {
        if (cfg->keys[i].arg.com) {
            for (int j = 0; cfg->keys[i].arg.com[j]; j++)
                free((void *)cfg->keys[i].arg.com[j]);
            free((void *)cfg->keys[i].arg.com);
        }
    }
    free(cfg->keys);
    for (int i = 0; i < cfg->nctx; i++) {
        free(cfg->ctx[i].label);
        if (cfg->ctx[i].arg.com) {
            for (int j = 0; cfg->ctx[i].arg.com[j]; j++)
                free((void *)cfg->ctx[i].arg.com[j]);
            free((void *)cfg->ctx[i].arg.com);
        }
    }
    free(cfg->ctx);
    free(cfg->ctxbg);
    free(cfg->ctxborder);
    for (int i = 0; i < cfg->nshortcuts; i++) {
        for (int j = 0; j < cfg->shortcuts[i].ncmd; j++)
            free(cfg->shortcuts[i].cmd[j]);
        free(cfg->shortcuts[i].cmd);
    }
    free(cfg->shortcuts);
    free_icons(cfg);
    free(cfg);
}
