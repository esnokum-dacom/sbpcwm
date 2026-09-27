#pragma once

#include <stddef.h>

typedef enum { CTL_OPT_BOOL, CTL_OPT_INT, CTL_OPT_STR } CtlOptType;

typedef struct {
    const char *name;
    const char *label;
    CtlOptType  type;
    size_t      field;
    size_t      size;
    void      (*apply)(void);
} CtlOpt;

const char *ctl_socket_path(char *buf, size_t n);
const char *ctl_socket_path_for(char *buf, size_t n, const char *wm);

void ctl_init(void);
int  ctl_fd(void);
void ctl_accept(void);
void ctl_cleanup(void);

void ctl_handle(int fd, const char *line);

const CtlOpt *ctl_find_opt(const char *name);
const CtlOpt *ctl_opt_list(size_t *n);
const char *ctl_opt_label(const CtlOpt *o);
int  ctl_opt_get(const CtlOpt *o, char *buf, size_t n);
int  ctl_opt_set(const CtlOpt *o, const char *value);
