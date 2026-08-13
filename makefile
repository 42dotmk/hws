.POSIX:

CC     = cc
CFLAGS = -std=c99 -pedantic -Wall -Wextra -Os -D_POSIX_C_SOURCE=200809L
LDLIBS = -lX11 -lXrender -lXcomposite -lXdamage -lXfixes -lXrandr -lXtst
BINDIR = $(HOME)/.local/bin

all: hws

hws: hws.c
	$(CC) $(CFLAGS) -o $@ hws.c $(LDLIBS)

install: hws
	mkdir -p $(BINDIR)
	ln -sf "$$(pwd)/hws" $(BINDIR)/hws

uninstall:
	rm -f $(BINDIR)/hws

clean:
	rm -f hws

.PHONY: all install uninstall clean
