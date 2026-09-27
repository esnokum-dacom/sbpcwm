CFLAGS += -std=c99 -Wall -Wextra -pedantic -Wold-style-declaration
CFLAGS += -Wmissing-prototypes -Wno-unused-parameter
CFLAGS += -I. -Imodules
CLIBS  += -lX11 -lXext -lXrender -lXinerama -lm -lXft -lpng16 -ljpeg $(shell pkg-config --cflags --libs xft xcb xcb-xfixes xcb-randr xcb-shape xcb-icccm xcb-keysyms xcb-util x11-xcb lua5.3)
PREFIX ?= /usr
BINDIR ?= $(PREFIX)/bin
CC     ?= gcc

SRC = sbpcwm.c sbccl.c modules/ctl.c modules/ctl_path.c modules/icons.c \
      modules/sbcs/sbcs.c modules/sbcs/sbcs_save.c

all: sbpcwm sbpcwmctl sbpcs

sbpcwm: $(SRC) sbpcwm.h sbcct.h modules/ctl.h modules/icons.h \
        modules/sbcs/sbcs.h Makefile
	$(CC) -O3 $(CFLAGS) -o sbpcwm $(SRC) $(CLIBS) $(LDFLAGS)

sbpcwmctl: modules/sbpcwmctl.c modules/ctl_path.c modules/ctl.h Makefile
	$(CC) -O3 $(CFLAGS) -o sbpcwmctl modules/sbpcwmctl.c modules/ctl_path.c

sbpcs: modules/sbcs/open.c modules/ctl_path.c modules/ctl.h Makefile
	$(CC) -O3 $(CFLAGS) -o sbpcs modules/sbcs/open.c modules/ctl_path.c

install: all
	install -Dm755 sbpcwm $(DESTDIR)$(BINDIR)/sbpcwm
	install -Dm755 sbpcwmctl $(DESTDIR)$(BINDIR)/sbpcwmctl
	install -Dm755 sbpcs $(DESTDIR)$(BINDIR)/sbpcs
	mkdir -p ~/.config/sbcwm/icons
	@if [ -f ~/.config/sbcwm/config.lua ]; then \
	    echo "keeping existing ~/.config/sbcwm/config.lua"; \
	else \
	    install -Dm644 ./config.lua ~/.config/sbcwm/config.lua; \
	    echo "installed sample ~/.config/sbcwm/config.lua"; \
	fi

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/sbpcwm
	rm -f $(DESTDIR)$(BINDIR)/sbpcwmctl
	rm -f $(DESTDIR)$(BINDIR)/sbpcs

clean:
	rm -f sbpcwm sbpcwmctl sbpcs sbcs *.o modules/*.o modules/sbcs/*.o

.PHONY: all install uninstall clean
