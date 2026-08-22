#define _POSIX_C_SOURCE 200809L
#include <sys/socket.h>
#include <sys/un.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ctl.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
            "usage: sbcwmctl <command> [args...]\n"
            "  get <option>                  read a runtime option\n"
            "  set <option> <value>          write a runtime option\n"
            "  reload                        reload config.lua\n"
            "  shortcut add <name> <image> <x> <y> <cmd...>  create a desktop icon shortcut\n"
            "  shortcut del <name>                            remove a desktop icon shortcut\n"
            "  shortcut move <name> <x> <y>                   move a desktop icon shortcut\n"
            "  shortcut list                                  list desktop icon shortcuts\n"
            "  shortcut show on|off                           show/hide desktop icons\n"
            "  options                       list runtime options\n");
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    ctl_socket_path(addr.sun_path, sizeof addr.sun_path);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("sbcwmctl: socket"); return 1; }

    if (connect(fd, (struct sockaddr *)&addr, (socklen_t)sizeof addr) < 0) {
        fprintf(stderr, "sbcwmctl: cannot connect to %s (is sbcwm running?)\n", addr.sun_path);
        close(fd);
        return 1;
    }

    size_t len = 0;
    for (int i = 1; i < argc; i++) len += strlen(argv[i]) + 1;
    char *line = malloc(len + 2);
    line[0] = 0;
    for (int i = 1; i < argc; i++) {
        if (i > 1) strcat(line, " ");
        strcat(line, argv[i]);
    }
    strcat(line, "\n");

    if (write(fd, line, strlen(line)) < 0) {
        perror("sbcwmctl: write");
        close(fd);
        free(line);
        return 1;
    }

    char buf[2048];
    ssize_t r;
    while ((r = read(fd, buf, sizeof buf - 1)) > 0) {
        buf[r] = 0;
        fputs(buf, stdout);
        if (buf[r - 1] == '\n') break;
    }

    close(fd);
    free(line);
    return 0;
}
