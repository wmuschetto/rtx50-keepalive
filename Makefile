# Usage: make [all | check | check-gpu | install | uninstall | clean]
#   make                        build ./rtx50-keepalive
#   make check                  tests that need no GPU (what CI runs)
#   make check-gpu              tests on the NVIDIA GPU; run them with no game running
#   sudo make install           install to /usr/local/bin (PREFIX=... to change)
#   sudo make uninstall         remove it (the same PREFIX=... as install)

PREFIX ?= /usr/local
CC ?= cc
CFLAGS ?= -O2
# -Werror is left to `make check`, so a newer compiler's new warning never
# stops someone from building it.
WARNINGS = -std=c17 -Wall -Wextra -Wshadow -Wconversion
LDLIBS = -lvulkan
# Left unquoted in the recipes so the shell expands a ~ that zsh and fish pass
# to make literally; whitespace would split it into several paths, so refuse it.
# Expanded only when install or uninstall runs.
install_path = $(DESTDIR)$(PREFIX)/bin/rtx50-keepalive
installed = $(if $(filter 1,$(words $(install_path))),$(install_path),$(error \
    PREFIX and DESTDIR must form one path without spaces, not [$(DESTDIR)$(PREFIX)]))

all: rtx50-keepalive

rtx50-keepalive: rtx50-keepalive.c
	$(CC) $(WARNINGS) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

check:
	tests/check.sh

check-gpu:
	tests/check-gpu.sh

install: rtx50-keepalive
	install -D -m 755 rtx50-keepalive $(installed)

uninstall:
	rm -f $(installed)

clean:
	rm -f rtx50-keepalive

.PHONY: all check check-gpu install uninstall clean
