CFLAGS += -std=c99 -Wall -Wextra -pedantic -Wold-style-declaration
CFLAGS += -Wmissing-prototypes -Wno-unused-parameter
CFLAGS += -I. -Imodules
CLIBS  += -lX11 -lXext -lXrender -lXinerama -lm -lXft -lpng16 -ljpeg $(shell pkg-config --cflags --libs xft xcb xcb-xfixes xcb-randr xcb-shape xcb-icccm xcb-keysyms xcb-util x11-xcb lua5.3)
PREFIX ?= /usr
BINDIR ?= $(PREFIX)/bin
CC     ?= gcc

SRC = sbpcwm.c sbccl.c modules/ctl.c modules/ctl_path.c modules/icons.c

all: sbpcwm sbpcwmctl

sbpcwm: $(SRC) sbpcwm.h sbcct.h modules/ctl.h modules/icons.h Makefile
	$(CC) -O3 $(CFLAGS) -o sbpcwm $(SRC) $(CLIBS) $(LDFLAGS)

sbpcwmctl: modules/sbpcwmctl.c modules/ctl_path.c modules/ctl.h Makefile
	$(CC) -O3 $(CFLAGS) -o sbpcwmctl modules/sbpcwmctl.c modules/ctl_path.c

install: all
	install -Dm755 sbpcwm $(DESTDIR)$(BINDIR)/sbpcwm
	install -Dm755 sbpcwmctl $(DESTDIR)$(BINDIR)/sbpcwmctl
	mkdir -p ~/.config/sbpcwm
	cp ./config.lua ~/.config/sbpcwm

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/sbpcwm
	rm -f $(DESTDIR)$(BINDIR)/sbpcwmctl

clean:
	rm -f sbpcwm sbpcwmctl *.o modules/*.o

.PHONY: all install uninstall clean
