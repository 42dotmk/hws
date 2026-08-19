.POSIX:

CC     = cc

# Version derived from `git describe` at build time so the binary reports
# the exact tag/commit it was built from; "dev" without git metadata.
VERSION != git describe --tags --always --dirty 2>/dev/null || echo dev

CFLAGS = -std=c11 -pedantic -Wall -Wextra -Os -D_POSIX_C_SOURCE=200809L \
         -DHWS_VERSION='"$(VERSION)"'
LDLIBS = -lX11 -lXrender -lXcomposite -lXdamage -lXfixes -lXrandr -lXtst
BINDIR = $(HOME)/.local/bin

all: hws

hws: hws.c config.h
	$(CC) $(CFLAGS) -o $@ hws.c $(LDLIBS)

install: hws
	mkdir -p $(BINDIR)
	ln -sf "$$(pwd)/hws" $(BINDIR)/hws

uninstall:
	rm -f $(BINDIR)/hws

clean:
	rm -f hws

.PHONY: all install uninstall clean
