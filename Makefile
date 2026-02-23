CC ?= clang
CFLAGS := -O3 -ffast-math

BIN := pager
SRC := src/pager.c

all: $(BIN)

$(BIN): $(SRC) $(wildcard src/lib/*.h)
	$(CC) $(CFLAGS) $(SRC) -o $@ $(LDFLAGS)
	strip $@

install: $(BIN)
	install -d ~/.local/bin
	install -m 755 $(BIN) ~/.local/bin/

clean:
	rm -f $(BIN)

.PHONY: all install clean
