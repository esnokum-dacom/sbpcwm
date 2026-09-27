#define _POSIX_C_SOURCE 200809L
#include <sys/socket.h>
#include <sys/un.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "ctl.h"

/* sbcwm and sbpcwm read the same ~/.config/sbcwm state, so the settings panel
   works on either one. Ask sbpcwm first, then fall back to sbcwm. */
static const char *const wms[] = { "sbpcwm", "sbcwm" };

static int send_settings(int fd) {
    if (write(fd, "settings\n", 9) < 0) {
        perror("sbpcs: write");
        return -1;
    }

    char buf[256];
    ssize_t r;
    while ((r = read(fd, buf, sizeof buf - 1)) > 0) {
        buf[r] = 0;
        fputs(buf, stdout);
        if (buf[r - 1] == '\n') break;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1) {
        fprintf(stderr, "usage: sbpcs\n");
        return 1;
    }

    char tried[2][sizeof(((struct sockaddr_un *)0)->sun_path)];
    size_t n_tried = 0;

    for (size_t i = 0; i < sizeof wms / sizeof *wms; i++) {
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof addr);
        addr.sun_family = AF_UNIX;
        ctl_socket_path_for(addr.sun_path, sizeof addr.sun_path, wms[i]);

        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) { perror("sbpcs: socket"); return 1; }

        if (connect(fd, (struct sockaddr *)&addr, (socklen_t)sizeof addr) == 0) {
            int rc = send_settings(fd);
            close(fd);
            return rc < 0 ? 1 : 0;
        }
        close(fd);

        if (n_tried < sizeof tried / sizeof *tried)
            snprintf(tried[n_tried++], sizeof tried[0], "%s", addr.sun_path);
    }

    fprintf(stderr, "sbpcs: no running window manager found (tried");
    for (size_t i = 0; i < n_tried; i++)
        fprintf(stderr, "%s %s", i ? "," : "", tried[i]);
    fprintf(stderr, ")\n");
    return 1;
}
