# Usage: make [all | check | check-gpu | install | uninstall | clean]
#   make                        build ./rtx50-keepalive
#   make check                  tests that need no GPU (what CI runs)
#   make check-gpu              tests on the NVIDIA GPU; run them with no game running
#   sudo make install           install to /usr/local/bin (PREFIX=... to change)
#   sudo make uninstall

PREFIX ?= /usr/local
CC ?= cc
CFLAGS ?= -O2
# -Werror is left to `make check`, so a newer compiler's new warning never
# stops someone from building it.
WARNINGS = -std=c17 -Wall -Wextra -Wshadow -Wconversion
LDLIBS = -lvulkan

all: rtx50-keepalive

rtx50-keepalive: rtx50-keepalive.c
	$(CC) $(WARNINGS) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

check:
	tests/check.sh

check-gpu:
	tests/check-gpu.sh

install: rtx50-keepalive
	install -D -m 755 rtx50-keepalive $(DESTDIR)$(PREFIX)/bin/rtx50-keepalive

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/rtx50-keepalive

clean:
	rm -f rtx50-keepalive

.PHONY: all check check-gpu install uninstall clean
