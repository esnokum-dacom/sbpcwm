#define _POSIX_C_SOURCE 200809L
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <poll.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stddef.h>
#include <X11/X.h>
#include <X11/keysym.h>

#include "sbcct.h"
#include "sbpcwm.h"
#include "ctl.h"

static int listen_fd = -1;

void ctl_init(void) {
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    ctl_socket_path(addr.sun_path, sizeof addr.sun_path);

    listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) return;

    unlink(addr.sun_path);
    if (bind(listen_fd, (struct sockaddr *)&addr, (socklen_t)sizeof addr) < 0 ||
        listen(listen_fd, 8) < 0) {
        close(listen_fd);
        listen_fd = -1;
        return;
    }
    chmod(addr.sun_path, 0600);

    int flags = fcntl(listen_fd, F_GETFL, 0);
    fcntl(listen_fd, F_SETFL, flags | O_NONBLOCK);
}

int ctl_fd(void) { return listen_fd; }

void ctl_cleanup(void) {
    if (listen_fd >= 0) {
        close(listen_fd);
        listen_fd = -1;
    }
}

static int ctl_read_line(int fd, char *buf, size_t n) {
    size_t i = 0;
    while (i + 1 < n) {
        char c;
        ssize_t r = read(fd, &c, 1);
        if (r == 1) {
            if (c == '\n') break;
            buf[i++] = c;
        } else if (r == 0) {
            break;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            return -1;
        }
    }
    buf[i] = 0;
    return (int)i;
}

static const char *next_tok(const char *s, char *out, size_t n) {
    while (*s == ' ') s++;
    if (!*s) { out[0] = 0; return s; }
    size_t i = 0;
    while (*s && *s != ' ' && i + 1 < n) out[i++] = *s++;
    out[i] = 0;
    while (*s == ' ') s++;
    return s;
}

typedef enum { OPT_INT, OPT_STR } OptType;

typedef struct {
    const char *name;
    OptType type;
    size_t  field;
    size_t  size;
    void (*apply)(void);
} CtlOpt;

static void apply_none(void) {}
static void apply_borders(void) { update_border_widths(); }
static void apply_fonts(void) { fonts_reload(); canvas_apply_all(); }
static void apply_visual(void) { canvas_apply_all(); }

#define NUMO(name, applyfn) \
    { #name, OPT_INT, offsetof(Config, name), sizeof(((Config *)0)->name), applyfn }
#define STRO(name, applyfn) \
    { #name, OPT_STR, offsetof(Config, name), 0, applyfn }

static CtlOpt opts[] = {
    NUMO(pan_step,     apply_none),
    NUMO(titlebar,     apply_none),
    NUMO(ui,           apply_none),
    NUMO(xr_colors,    apply_visual),
    NUMO(border,       apply_borders),
    NUMO(border_width, apply_borders),
    STRO(ctxbg,        apply_none),
    STRO(ctxborder,    apply_none),
    STRO(fonts,        apply_fonts),
    STRO(fontb,        apply_fonts),
    STRO(defaultsh,    apply_none),
};

static CtlOpt *find_opt(const char *name) {
    for (size_t i = 0; i < sizeof opts / sizeof *opts; i++)
        if (!strcmp(opts[i].name, name)) return &opts[i];
    return NULL;
}

static void *opt_ptr(CtlOpt *o) { return (char *)cfg + o->field; }

static int get_num(const void *p, size_t sz) {
    if (sz == 1) return *(const uint8_t *)p;
    if (sz == 2) return *(const uint16_t *)p;
    return *(const int *)p;
}

static void set_num(void *p, size_t sz, int v) {
    if (sz == 1)      *(uint8_t *)p  = (uint8_t)v;
    else if (sz == 2) *(uint16_t *)p = (uint16_t)v;
    else              *(int *)p      = v;
}

static void ctl_get(const char *name, char *reply, size_t n) {
    CtlOpt *o = find_opt(name);
    if (!o) { snprintf(reply, n, "ERR unknown option: %s", name); return; }
    if (o->type == OPT_INT)
        snprintf(reply, n, "OK %s=%d", o->name, get_num(opt_ptr(o), o->size));
    else {
        const char *v = *(char **)opt_ptr(o);
        snprintf(reply, n, "OK %s=%s", o->name, v ? v : "");
    }
}

static void ctl_set(const char *args, char *reply, size_t n) {
    char name[64], value[512];
    args = next_tok(args, name, sizeof name);
    next_tok(args, value, sizeof value);

    CtlOpt *o = find_opt(name);
    if (!o) { snprintf(reply, n, "ERR unknown option: %s", name); return; }

    if (o->type == OPT_INT) {
        char *end = NULL;
        long v = strtol(value, &end, 10);
        if (!value[0] || (end && *end)) {
            snprintf(reply, n, "ERR invalid integer: %s", value);
            return;
        }
        set_num(opt_ptr(o), o->size, (int)v);
    } else {
        char **dst = (char **)opt_ptr(o);
        free(*dst);
        *dst = strdup(value);
    }
    o->apply();
    snprintf(reply, n, "OK %s=%s", o->name, value);
}

static int icon_find(const char *name) {
    for (int i = 0; i < cfg->nicons; i++)
        if (cfg->icons[i].name && !strcmp(cfg->icons[i].name, name)) return i;
    return -1;
}

static void ctl_icons_list(char *reply, size_t n) {
    size_t off = 0;
    off += (size_t)snprintf(reply + off, n - off, "OK %d icons", cfg->nicons);
    for (int i = 0; i < cfg->nicons && off + 8 < n; i++) {
        LauncherIcon *ic = &cfg->icons[i];
        off += (size_t)snprintf(reply + off, n - off, "\n%s %s %d %d",
                                ic->name ? ic->name : "?",
                                ic->image ? ic->image : "",
                                ic->x, ic->y);
        for (int j = 0; j < ic->ncmd && off + 4 < n; j++)
            off += (size_t)snprintf(reply + off, n - off, " %s", ic->cmd[j]);
    }
}

static void ctl_icons_add(const char *args, char *reply, size_t n) {
    char name[64], image[512], xs[32], ys[32], cmdline[512];
    args = next_tok(args, name, sizeof name);
    args = next_tok(args, image, sizeof image);
    args = next_tok(args, xs, sizeof xs);
    args = next_tok(args, ys, sizeof ys);
    {
        const char *p = args;
        size_t i = 0;
        while (*p && i + 1 < sizeof cmdline) cmdline[i++] = *p++;
        cmdline[i] = 0;
    }

    if (!name[0] || !cmdline[0]) {
        snprintf(reply, n, "ERR usage: icons add <name> <image> <x> <y> <cmd...>");
        return;
    }
    if (icon_find(name) >= 0) { snprintf(reply, n, "ERR icon exists: %s", name); return; }

    int x = (int)strtol(xs, NULL, 10);
    int y = (int)strtol(ys, NULL, 10);

    char *argv[64];
    int argc = 0;
    char *save = NULL;
    char *copy = strdup(cmdline);
    for (char *t = strtok_r(copy, " ", &save); t && argc < 63; t = strtok_r(NULL, " ", &save))
        argv[argc++] = t;
    argv[argc] = NULL;
    if (argc == 0) { free(copy); snprintf(reply, n, "ERR missing command"); return; }

    char **cmd = malloc(sizeof(char *) * (argc + 1));
    for (int i = 0; i < argc; i++) cmd[i] = strdup(argv[i]);
    cmd[argc] = NULL;
    free(copy);

    cfg->icons = realloc(cfg->icons, sizeof(LauncherIcon) * (cfg->nicons + 1));
    LauncherIcon *ic = &cfg->icons[cfg->nicons++];
    memset(ic, 0, sizeof(*ic));
    ic->name = strdup(name);
    ic->image = image[0] ? strdup(image) : NULL;
    ic->x = x;
    ic->y = y;
    ic->mon = mon_at_ptr();
    ic->cmd = cmd;
    ic->ncmd = argc;

    icons_rebuild();
    icons_save();
    snprintf(reply, n, "OK icon added");
}

static void ctl_icons_del(const char *args, char *reply, size_t n) {
    char name[64];
    next_tok(args, name, sizeof name);
    int i = icon_find(name);
    if (i < 0) { snprintf(reply, n, "ERR icon not found: %s", name); return; }

    LauncherIcon *ic = &cfg->icons[i];
    free(ic->name);
    free(ic->image);
    for (int j = 0; j < ic->ncmd; j++) free(ic->cmd[j]);
    free(ic->cmd);
    memmove(&cfg->icons[i], &cfg->icons[i + 1], sizeof(LauncherIcon) * (cfg->nicons - i - 1));
    cfg->nicons--;
    icons_rebuild();
    icons_save();
    snprintf(reply, n, "OK icon removed");
}

static void ctl_icons_move(const char *args, char *reply, size_t n) {
    char name[64], xs[32], ys[32];
    args = next_tok(args, name, sizeof name);
    args = next_tok(args, xs, sizeof xs);
    next_tok(args, ys, sizeof ys);

    int i = icon_find(name);
    if (i < 0) { snprintf(reply, n, "ERR icon not found: %s", name); return; }

    cfg->icons[i].x = (int)strtol(xs, NULL, 10);
    cfg->icons[i].y = (int)strtol(ys, NULL, 10);
    icons_reposition();
    icons_save();
    snprintf(reply, n, "OK icon moved");
}

static void ctl_icons_show(const char *args, char *reply, size_t n) {
    char verb[16];
    next_tok(args, verb, sizeof verb);
    if (!strcmp(verb, "on")) {
        if (!icons_visible()) toggle_icons((Arg){0});
        snprintf(reply, n, "OK icons visible");
    } else if (!strcmp(verb, "off")) {
        if (icons_visible()) toggle_icons((Arg){0});
        snprintf(reply, n, "OK icons hidden");
    } else {
        snprintf(reply, n, "ERR usage: icons show on|off");
    }
}

static void ctl_options(char *reply, size_t n) {
    size_t off = 0;
    off += (size_t)snprintf(reply + off, n - off, "OK\n");
    for (size_t i = 0; i < sizeof opts / sizeof *opts && off + 4 < n; i++)
        off += (size_t)snprintf(reply + off, n - off, "%s\n", opts[i].name);
}

void ctl_handle(int fd, const char *line) {
    char cmd[32], args[1024], reply[2048];
    const char *p = next_tok(line, cmd, sizeof cmd);
    strncpy(args, p, sizeof args - 1);
    args[sizeof args - 1] = 0;

    if (!strcmp(cmd, "get"))
        ctl_get(args, reply, sizeof reply);
    else if (!strcmp(cmd, "set"))
        ctl_set(args, reply, sizeof reply);
    else if (!strcmp(cmd, "reload")) {
        reload_config((Arg){0});
        snprintf(reply, sizeof reply, "OK reloaded");
    } else if (!strcmp(cmd, "options")) {
        ctl_options(reply, sizeof reply);
    } else if (!strcmp(cmd, "shortcut") || !strcmp(cmd, "icons")) {
        char sub[32];
        const char *a = next_tok(args, sub, sizeof sub);
        if (!strcmp(sub, "list"))
            ctl_icons_list(reply, sizeof reply);
        else if (!strcmp(sub, "add"))
            ctl_icons_add(a, reply, sizeof reply);
        else if (!strcmp(sub, "del"))
            ctl_icons_del(a, reply, sizeof reply);
        else if (!strcmp(sub, "move"))
            ctl_icons_move(a, reply, sizeof reply);
        else if (!strcmp(sub, "save")) {
            icons_save();
            snprintf(reply, sizeof reply, "OK icons saved");
        } else if (!strcmp(sub, "show"))
            ctl_icons_show(a, reply, sizeof reply);
        else
            snprintf(reply, sizeof reply,
                     "ERR usage: shortcut list|add|del|move|save|show");
    } else {
        snprintf(reply, sizeof reply, "ERR unknown command: %s", cmd);
    }

    write(fd, reply, strlen(reply));
}

void ctl_accept(void) {
    if (listen_fd < 0) return;
    int cfd;
    while ((cfd = accept(listen_fd, NULL, NULL)) >= 0) {
        struct pollfd pfd = { cfd, POLLIN, 0 };
        if (poll(&pfd, 1, 200) > 0) {
            char buf[1024];
            if (ctl_read_line(cfd, buf, sizeof buf) > 0)
                ctl_handle(cfd, buf);
        }
        close(cfd);
    }
}
