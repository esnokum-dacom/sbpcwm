#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ctl.h"

const char *ctl_socket_path(char *buf, size_t n) {
    const char *disp = getenv("DISPLAY");
    if (disp && *disp) {
        const char *p = strchr(disp, ':');
        p = p ? p + 1 : disp;
        char d[64];
        size_t i = 0;
        while (*p && i + 1 < sizeof d) {
            d[i] = (*p == '.') ? '_' : *p;
            i++;
            p++;
        }
        d[i] = 0;
        const char *rt = getenv("XDG_RUNTIME_DIR");
        if (rt && *rt)
            snprintf(buf, n, "%s/sbpcwm-%s.sock", rt, d);
        else
            snprintf(buf, n, "/tmp/sbpcwm-%ld-%s.sock", (long)getuid(), d);
        return buf;
    }

    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (rt && *rt) {
        snprintf(buf, n, "%s/sbpcwm.sock", rt);
        return buf;
    }
    snprintf(buf, n, "/tmp/sbpcwm-%ld.sock", (long)getuid());
    return buf;
}
