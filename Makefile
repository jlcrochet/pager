CC ?= clang
CFLAGS ?= -O3
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
	rm -f $(BIN) tests/test-pager

tests/test-pager: tests/test_pager.c $(SRC) $(wildcard src/lib/*.h)
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

test: tests/test-pager
	./tests/test-pager

.PHONY: all install clean test
