CC = gcc

# Baked into the binary and shown in the startup banner. CI overrides this
# with the release tag (make VERSION=v1.2.3); local builds fall back to
# `git describe`, so a dev build identifies itself as e.g.
# "v0.1.1-3-gabc1234-dirty" rather than claiming to be a release.
VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)

CFLAGS = -Wall -g -DFLASHSSH_VERSION=\"$(VERSION)\"
LDLIBS = -lm -lssh -lpthread

OBJS = main.o headers/ssh_connection.o headers/ssh_session.o headers/history.o headers/line_editor.o headers/dir_cache.o headers/stream.o

main: $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o main $(LDLIBS)

clean:
	rm -f main *.o headers/*.o
