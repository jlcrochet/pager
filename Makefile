CC ?= clang
CFLAGS ?= -O2
LDFLAGS ?=

BIN := pager
SRC := src/pager.c

all: $(BIN)

$(BIN): $(SRC) $(wildcard src/lib/*.h)
	$(CC) $(CFLAGS) $(SRC) -o $@ $(LDFLAGS)
ifndef DEBUG
	strip $@
endif

install: $(BIN)
	install -d ~/.local/bin
	install -m 755 $(BIN) ~/.local/bin/

clean:
	rm -f $(BIN)

.PHONY: all install clean
